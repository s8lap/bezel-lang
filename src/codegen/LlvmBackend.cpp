#include "codegen/LlvmBackend.hpp"

#include <cstdint>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ast/Node.hpp"
#include "sema/SemaContext.hpp"
#include "sema/Symbol.hpp"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/TargetParser/Host.h"

namespace bezel::codegen {

namespace {

// Parse a Bezel integer literal lexeme to its value. Slice CG1 handles
// decimal and the `0x` / `0b` / `0o` bases, with `_` digit separators
// stripped and any trailing type-suffix letters ignored. Returns 0 on a
// shape it doesn't recognize (the lexer already validated the literal).
std::int64_t parseIntLiteral(std::string_view text) {
    std::string digits;
    int base = 10;
    std::size_t i = 0;
    if (text.size() >= 2 && text[0] == '0') {
        char b = text[1];
        if (b == 'x' || b == 'X') { base = 16; i = 2; }
        else if (b == 'b' || b == 'B') { base = 2; i = 2; }
        else if (b == 'o' || b == 'O') { base = 8; i = 2; }
    }
    for (; i < text.size(); ++i) {
        char c = text[i];
        if (c == '_') continue;
        // Stop at a type-suffix letter (e.g. `u`, `l`) for decimal; for
        // hex, hex-digits are consumed and only non-hex letters stop us.
        bool isDigit = (c >= '0' && c <= '9');
        bool isHex = isDigit || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (base == 16 ? !isHex : !isDigit) break;
        digits.push_back(c);
    }
    if (digits.empty()) return 0;
    return static_cast<std::int64_t>(std::strtoll(digits.c_str(), nullptr, base));
}

// Per-module emitter. Holds the LLVM context/module/builder and lowers the
// checked AST. Grows one expression/statement kind per codegen slice.
class Emitter {
public:
    Emitter(sema::SemaContext& ctx, llvm::LLVMContext& llvmCtx)
        : ctx_(ctx), llvm_(llvmCtx),
          module_(std::make_unique<llvm::Module>("bezel", llvmCtx)),
          builder_(llvmCtx) {}

    std::unique_ptr<llvm::Module> run() {
        // CG8: build every class's instance layout first (so field
        // access and `new` see field indices). CG6: declare all static
        // method functions, then emit bodies.
        declareStructs();
        declareStatics();
        declareFunctions();
        declareVtables();  // CG17: after functions exist (needs fn ptrs)
        for (auto& [sym, fn] : methodFns_) {
            emitFunctionBody(sym, fn);
        }
        for (auto& [sym, fn] : ctorFns_) {
            emitConstructorBody(sym, fn);
        }
        for (auto& [ts, fn] : disposerFns_) {
            emitDisposerBody(ts, fn);
        }
        return std::move(module_);
    }

private:
    sema::SemaContext& ctx_;
    llvm::LLVMContext& llvm_;
    std::unique_ptr<llvm::Module> module_;
    llvm::IRBuilder<> builder_;
    llvm::Function* currentFn_ = nullptr;
    // CG9: the `this` pointer of the instance method currently being
    // emitted (null in static methods). Kept as a value, not a slot —
    // `this` is never reassignable.
    llvm::Value* currentThis_ = nullptr;
    // CG3: each local's storage, keyed by its declaration node (a
    // LocalDecl/LocalConstDecl/ParameterSymbol). An IdentifierExpr
    // resolves to a Local/Parameter symbol whose `decl` is that node.
    std::unordered_map<const ast::Node*, llvm::AllocaInst*> locals_;
    // CG6: parameter storage, keyed by ParameterSymbol* (params share
    // their owning method as `decl`, so the decl node can't disambiguate).
    std::unordered_map<const sema::Symbol*, llvm::AllocaInst*> paramSlots_;
    // CG6: emitted LLVM function for each static method symbol.
    std::unordered_map<sema::MethodSymbol*, llvm::Function*> methodFns_;
    // CG10: emitted LLVM function for each (non-static) constructor symbol.
    std::unordered_map<sema::ConstructorSymbol*, llvm::Function*> ctorFns_;

    // CG11: enclosing-loop branch targets for `break`/`continue` (§A.6).
    // `continue` jumps to `continueTarget` (the loop's condition block),
    // `break` to `breakTarget` (the loop's exit block). Innermost loop is
    // the back of the vector.
    struct LoopTargets {
        llvm::BasicBlock* continueTarget;
        llvm::BasicBlock* breakTarget;
        // CG13: disposal-scope depth just outside this loop's body, so
        // break/continue dispose the body's (and any nested) using vars.
        std::size_t scopeDepth;
    };
    std::vector<LoopTargets> loopStack_;

    // CG13: a pending scope-based disposal — a `using var`'s storage slot
    // and the class whose Dispose to call. Held in a stack of scopes; the
    // innermost block is the back. Disposed in reverse declaration order
    // at each scope's normal exit (§28.3).
    struct Disposal {
        llvm::AllocaInst* slot;
        const sema::TypeSymbol* cls;
    };
    std::vector<std::vector<Disposal>> disposeStack_;
    // CG13: the compiler-generated Dispose function for each class.
    std::unordered_map<const sema::TypeSymbol*, llvm::Function*> disposerFns_;

    // CG8: per-class instance layout (struct of instance fields) and each
    // field's index within it.
    std::unordered_map<const sema::TypeSymbol*, llvm::StructType*> structs_;
    std::unordered_map<const sema::Symbol*, unsigned> fieldIndex_;

    // CG12: each static field's backing global variable.
    std::unordered_map<const sema::Symbol*, llvm::GlobalVariable*>
        staticGlobals_;

    // CG17: virtual dispatch. Each class's vtable is a constant array of
    // function pointers: slot 0 is Dispose, slots 1+ are virtual methods
    // (an override reuses its base method's slot). The hidden vtable
    // pointer lives at instance struct index 0.
    std::unordered_map<const sema::TypeSymbol*, llvm::GlobalVariable*> vtables_;
    std::unordered_map<const sema::MethodSymbol*, unsigned> vtableSlot_;
    // Per-class slot→method (vector index i = vtable slot i+1; slot 0 is
    // Dispose) and the virtual-method-name→slot map used to inherit slots.
    std::unordered_map<const sema::TypeSymbol*,
                       std::vector<sema::MethodSymbol*>> vtableMethods_;
    std::unordered_map<const sema::TypeSymbol*,
                       std::unordered_map<std::string_view, unsigned>>
        vtableNameSlot_;
    // CG19: per-type runtime descriptor (class AND interface) — a
    // null-terminated array of pointers to the descriptor globals of this
    // type plus all its transitive base types. A type's identity is its
    // own descriptor-array global address; `x is T` walks x's array for
    // T's address. Reachable from an object via vtable index 0. Also used
    // by catch type-matching (CG20).
    std::unordered_map<const sema::TypeSymbol*, llvm::GlobalVariable*>
        descArray_;

    llvm::Type* i32() { return llvm::Type::getInt32Ty(llvm_); }
    llvm::Type* i1() { return llvm::Type::getInt1Ty(llvm_); }
    llvm::Type* i64() { return llvm::Type::getInt64Ty(llvm_); }
    llvm::PointerType* ptrTy() { return llvm::PointerType::get(llvm_, 0); }

    // Create an alloca in the function's entry block (the conventional
    // home for locals, so they dominate all uses once control flow lands).
    llvm::AllocaInst* entryAlloca(llvm::Type* ty) {
        llvm::BasicBlock& entry = currentFn_->getEntryBlock();
        llvm::IRBuilder<> tmp(&entry, entry.begin());
        return tmp.CreateAlloca(ty);
    }

    // Is this member symbol a static method with a lowerable body?
    static bool isStaticMethod(sema::MethodSymbol* ms) {
        for (auto mod : ms->modifiers) {
            if (mod == "static") return true;
        }
        return false;
    }

    static bool isMain(sema::MethodSymbol* ms) {
        return ms->name == "Main" && isStaticMethod(ms);
    }

    static bool isStaticField(const sema::FieldSymbol* fs) {
        for (auto mod : fs->modifiers) {
            if (mod == "static") return true;
        }
        return false;
    }

    // CG17: a method participates in virtual dispatch if it is `virtual`,
    // `override`, or `abstract` (all imply a vtable slot; §12.1/§12.5).
    static bool isVirtual(const sema::MethodSymbol* ms) {
        for (auto mod : ms->modifiers) {
            if (mod == "virtual" || mod == "override" || mod == "abstract")
                return true;
        }
        return false;
    }

    // Lower a Bezel type (AST node) to its LLVM representation.
    llvm::Type* lowerType(const ast::Type* t) {
        if (!t) return llvm::Type::getVoidTy(llvm_);
        return lowerTypeRef(ctx_.resolvedType(*t));
    }

    // Lower a resolved TypeRef. `()`/null → void; primitives → scalars;
    // class/interface references → opaque pointer (heap reference).
    // Other kinds fall back to i32 (refined as more types are lowered).
    llvm::Type* lowerTypeRef(const sema::TypeRef* rt) {
        if (!rt) return i32();
        switch (rt->kind) {
            case sema::TypeRef::Kind::Unit:
                return llvm::Type::getVoidTy(llvm_);
            case sema::TypeRef::Kind::Primitive:
                return rt->primitive == sema::PrimitiveType::Bool ? i1()
                                                                  : i32();
            case sema::TypeRef::Kind::Named:
                if (rt->declaration &&
                    (rt->declaration->kind == ast::TypeDeclKind::Class ||
                     rt->declaration->kind == ast::TypeDeclKind::Interface)) {
                    return ptrTy();  // reference types are heap pointers
                }
                return i32();  // value-kind named (record/enum) — later
            default:
                return i32();
        }
    }

    // CG15: collect a class's instance fields in layout order —
    // base-class fields FIRST (prefix layout: a `Derived*` aliases a
    // `Base*`), then own fields. Recurses the single base class
    // (baseTypes[0] when it is a class). Static fields are excluded.
    void collectInstanceFields(const sema::TypeSymbol* t,
                               std::vector<const sema::FieldSymbol*>& out) {
        for (const sema::TypeSymbol* b : t->baseTypes) {
            if (b && b->kind == ast::TypeDeclKind::Class) {
                collectInstanceFields(b, out);
                break;  // a class has at most one base class
            }
        }
        for (const sema::Symbol* m : t->members) {
            auto* fs = dynamic_cast<const sema::FieldSymbol*>(m);
            if (fs && !isStaticField(fs)) out.push_back(fs);
        }
    }

    // CG8/CG15: the LLVM struct for a class's instance, built lazily.
    // Inherited fields come first (prefix layout), then own fields;
    // fieldIndex_ records each field's absolute index. The prefix
    // property means an inherited field has the same index in the base
    // struct and every derived struct, so member access through
    // fs->parent's struct is correct on a derived instance.
    llvm::StructType* classStruct(const sema::TypeSymbol* t) {
        auto it = structs_.find(t);
        if (it != structs_.end()) return it->second;
        auto* st = llvm::StructType::create(
            llvm_, std::string("class.") + std::string(t->name));
        structs_[t] = st;  // record before filling (supports self-reference)
        std::vector<const sema::FieldSymbol*> fields;
        collectInstanceFields(t, fields);
        std::vector<llvm::Type*> fieldTys;
        // CG17: slot 0 is the hidden vtable pointer; data fields follow.
        fieldTys.push_back(ptrTy());
        unsigned idx = 1;
        for (const sema::FieldSymbol* fs : fields) {
            auto* fd = dynamic_cast<const ast::FieldDecl*>(fs->decl);
            fieldTys.push_back(fd ? lowerType(fd->type.get()) : i32());
            fieldIndex_[fs] = idx++;
        }
        st->setBody(fieldTys);
        return st;
    }

    // §33: the entry-point method's LLVM function is named `main`; every
    // other static method gets a mangled, collision-free name.
    std::string functionName(sema::MethodSymbol* ms) {
        if (isMain(ms)) return "main";
        std::string cls = ms->parent ? std::string(ms->parent->name) : "";
        return "bz$" + cls + "$" + std::string(ms->name);
    }

    // CG12: a static field becomes an internal global. Its initializer
    // is the literal constant when present (int/bool literal), else the
    // zero value. Non-literal static initializers (which the spec runs in
    // a static constructor, §22) are deferred — the global is zeroed.
    void declareStatics() {
        for (const auto& sym : ctx_.symbols()) {
            auto* fs = dynamic_cast<const sema::FieldSymbol*>(sym.get());
            if (!fs || !isStaticField(fs)) continue;
            auto* fd = dynamic_cast<const ast::FieldDecl*>(fs->decl);
            llvm::Type* ty = fd ? lowerType(fd->type.get()) : i32();
            std::string cls = fs->parent ? std::string(fs->parent->name) : "";
            auto* gv = new llvm::GlobalVariable(
                *module_, ty, /*isConstant=*/false,
                llvm::GlobalValue::InternalLinkage,
                constInitFor(fd, ty),
                "bz$" + cls + "$" + std::string(fs->name));
            staticGlobals_[fs] = gv;
        }
    }

    // The constant initializer for a static field, or zero if absent or
    // non-literal (non-literal static init is deferred — see declareStatics).
    llvm::Constant* constInitFor(const ast::FieldDecl* fd, llvm::Type* ty) {
        if (fd && fd->initializer) {
            const ast::Expr* e = fd->initializer.get();
            if (auto* lit = dynamic_cast<const ast::IntLiteralExpr*>(e)) {
                return llvm::ConstantInt::get(
                    ty, static_cast<std::uint64_t>(parseIntLiteral(lit->text)),
                    /*isSigned=*/true);
            }
            if (auto* b = dynamic_cast<const ast::BoolLiteralExpr*>(e)) {
                return llvm::ConstantInt::get(ty, b->value ? 1 : 0);
            }
        }
        return llvm::Constant::getNullValue(ty);
    }

    // CG12: the backing global of a bare identifier that resolves to a
    // static field, or null if `e` isn't such a reference.
    llvm::GlobalVariable* staticFieldGlobal(const ast::Expr& e) {
        if (!dynamic_cast<const ast::IdentifierExpr*>(&e)) return nullptr;
        auto* fs = dynamic_cast<sema::FieldSymbol*>(ctx_.exprSymbol(e));
        if (!fs) return nullptr;
        auto it = staticGlobals_.find(fs);
        return it == staticGlobals_.end() ? nullptr : it->second;
    }

    // CG8: build the instance layout for every class up front.
    void declareStructs() {
        for (const auto& sym : ctx_.symbols()) {
            auto* ts = dynamic_cast<const sema::TypeSymbol*>(sym.get());
            if (ts && ts->kind == ast::TypeDeclKind::Class) classStruct(ts);
        }
    }

    // Pass 1: create a Function (no body) for every method with a body.
    // CG9: instance methods take an extra leading `this` pointer.
    void declareFunctions() {
        for (const auto& sym : ctx_.symbols()) {
            auto* ms = dynamic_cast<sema::MethodSymbol*>(sym.get());
            if (!ms) continue;
            auto* md = dynamic_cast<const ast::MethodDecl*>(ms->decl);
            if (!md || md->bodyKind == ast::MethodBodyKind::Abstract) continue;

            llvm::Type* retTy = isMain(ms) ? i32()
                                           : lowerType(md->returnType.get());
            std::vector<llvm::Type*> paramTys;
            if (!isStaticMethod(ms)) paramTys.push_back(ptrTy());  // this
            for (const auto& p : md->params) {
                paramTys.push_back(lowerType(p.type.get()));
            }
            auto* fnType = llvm::FunctionType::get(retTy, paramTys,
                                                   /*isVarArg=*/false);
            auto* fn = llvm::Function::Create(
                fnType, llvm::Function::ExternalLinkage, functionName(ms),
                module_.get());
            methodFns_[ms] = fn;
        }

        // CG10: declare each instance constructor — `void(this, params...)`.
        for (const auto& sym : ctx_.symbols()) {
            auto* cs = dynamic_cast<sema::ConstructorSymbol*>(sym.get());
            if (!cs || cs->isStatic) continue;
            auto* cd = dynamic_cast<const ast::ConstructorDecl*>(cs->decl);
            if (!cd) continue;
            std::vector<llvm::Type*> paramTys;
            paramTys.push_back(ptrTy());  // this
            for (const auto& p : cd->params) {
                paramTys.push_back(lowerType(p.type.get()));
            }
            auto* fnType = llvm::FunctionType::get(
                llvm::Type::getVoidTy(llvm_), paramTys, /*isVarArg=*/false);
            std::string cls = cs->parent ? std::string(cs->parent->name) : "";
            auto* fn = llvm::Function::Create(
                fnType, llvm::Function::ExternalLinkage, "bz$" + cls + "$ctor",
                module_.get());
            ctorFns_[cs] = fn;
        }

        // CG13: declare a Dispose function `void(this)` for every class.
        for (const auto& sym : ctx_.symbols()) {
            auto* ts = dynamic_cast<const sema::TypeSymbol*>(sym.get());
            if (!ts || ts->kind != ast::TypeDeclKind::Class) continue;
            auto* fnType = llvm::FunctionType::get(
                llvm::Type::getVoidTy(llvm_), {ptrTy()}, /*isVarArg=*/false);
            auto* fn = llvm::Function::Create(
                fnType, llvm::Function::ExternalLinkage,
                "bz$" + std::string(ts->name) + "$Dispose", module_.get());
            disposerFns_[ts] = fn;
        }
    }

    // CG17: assign vtable slots for a class, base-first. Slot 0 is Dispose
    // (handled at emit time); virtual methods take slots 1+. An override
    // reuses the base method's slot (matched by name — overloaded virtuals
    // are deferred). Records the slot for every virtual/override method so
    // a statically-resolved call can index the runtime object's vtable.
    void buildVtable(const sema::TypeSymbol* t) {
        if (vtableMethods_.count(t)) return;
        std::vector<sema::MethodSymbol*> slots;
        std::unordered_map<std::string_view, unsigned> nameSlot;
        if (const sema::TypeSymbol* base = baseClassOf(t)) {
            buildVtable(base);
            slots = vtableMethods_[base];
            nameSlot = vtableNameSlot_[base];
        }
        for (sema::Symbol* m : t->members) {
            auto* ms = dynamic_cast<sema::MethodSymbol*>(m);
            if (!ms || isStaticMethod(ms) || !isVirtual(ms)) continue;
            // Vtable index 0 = type descriptor, 1 = Dispose, 2+ = methods,
            // so a method's slot is its position in `slots` + 2.
            auto it = nameSlot.find(ms->name);
            if (it != nameSlot.end()) {       // override: reuse base slot
                slots[it->second - 2] = ms;
                vtableSlot_[ms] = it->second;
            } else {                          // new virtual: fresh slot
                unsigned slot = static_cast<unsigned>(slots.size()) + 2;
                slots.push_back(ms);
                nameSlot[ms->name] = slot;
                vtableSlot_[ms] = slot;
            }
        }
        vtableMethods_[t] = std::move(slots);
        vtableNameSlot_[t] = std::move(nameSlot);
    }

    // CG17/CG19: build each class's vtable global, laid out as
    //   [0] type descriptor, [1] Dispose, [2+] virtual-method slots.
    // Also builds a runtime type descriptor for every class and interface
    // (CG19). Runs after declareFunctions so the function pointers exist.
    void declareVtables() {
        // Pass A: assign vtable slots for all classes (base-first).
        for (const auto& sym : ctx_.symbols()) {
            auto* ts = dynamic_cast<const sema::TypeSymbol*>(sym.get());
            if (ts && ts->kind == ast::TypeDeclKind::Class) buildVtable(ts);
        }
        // Pass B: declare a descriptor-array global (correctly sized, no
        // initializer yet) for every class/interface, so descriptors can
        // cross-reference each other in pass C.
        std::unordered_map<const sema::TypeSymbol*,
                           std::vector<const sema::TypeSymbol*>> supersOf;
        for (const auto& sym : ctx_.symbols()) {
            auto* ts = dynamic_cast<const sema::TypeSymbol*>(sym.get());
            if (!ts) continue;
            if (ts->kind != ast::TypeDeclKind::Class &&
                ts->kind != ast::TypeDeclKind::Interface)
                continue;
            std::vector<const sema::TypeSymbol*> supers;
            collectTypeAndBases(ts, supers);
            supersOf[ts] = supers;
            auto* arrTy = llvm::ArrayType::get(ptrTy(), supers.size() + 1);
            descArray_[ts] = new llvm::GlobalVariable(
                *module_, arrTy, /*isConstant=*/true,
                llvm::GlobalValue::InternalLinkage, nullptr,
                "td$" + std::string(ts->name));
        }
        // Pass C: fill each descriptor with [self + transitive bases, null].
        for (auto& [ts, supers] : supersOf) {
            std::vector<llvm::Constant*> elems;
            for (const sema::TypeSymbol* s : supers) elems.push_back(descArray_[s]);
            elems.push_back(llvm::ConstantPointerNull::get(ptrTy()));
            auto* arrTy = llvm::cast<llvm::ArrayType>(
                descArray_[ts]->getValueType());
            descArray_[ts]->setInitializer(
                llvm::ConstantArray::get(arrTy, elems));
        }
        // Pass D: build each class's vtable global.
        for (const auto& sym : ctx_.symbols()) {
            auto* ts = dynamic_cast<const sema::TypeSymbol*>(sym.get());
            if (!ts || ts->kind != ast::TypeDeclKind::Class) continue;
            const auto& slots = vtableMethods_[ts];
            std::vector<llvm::Constant*> elems;
            elems.push_back(descArray_[ts]);  // [0] type descriptor
            auto dit = disposerFns_.find(ts);  // [1] Dispose
            elems.push_back(dit != disposerFns_.end()
                                ? llvm::cast<llvm::Constant>(dit->second)
                                : llvm::ConstantPointerNull::get(ptrTy()));
            for (sema::MethodSymbol* ms : slots) {  // [2+] methods
                auto mit = methodFns_.find(ms);
                elems.push_back(
                    mit != methodFns_.end()
                        ? llvm::cast<llvm::Constant>(mit->second)
                        : llvm::ConstantPointerNull::get(ptrTy()));
            }
            auto* arrTy = llvm::ArrayType::get(ptrTy(), elems.size());
            auto* init = llvm::ConstantArray::get(arrTy, elems);
            vtables_[ts] = new llvm::GlobalVariable(
                *module_, arrTy, /*isConstant=*/true,
                llvm::GlobalValue::InternalLinkage, init,
                "vt$" + std::string(ts->name));
        }
    }

    // CG19: `ts` and all its transitive base types (classes + interfaces),
    // BFS, cycle-guarded.
    void collectTypeAndBases(const sema::TypeSymbol* ts,
                             std::vector<const sema::TypeSymbol*>& out) {
        std::vector<const sema::TypeSymbol*> frontier{ts};
        std::unordered_set<const sema::TypeSymbol*> seen{ts};
        std::size_t i = 0;
        while (i < frontier.size()) {
            const sema::TypeSymbol* t = frontier[i++];
            out.push_back(t);
            for (const sema::TypeSymbol* b : t->baseTypes) {
                if (b && seen.insert(b).second) frontier.push_back(b);
            }
        }
    }

    llvm::GlobalVariable* vtableOf(const sema::TypeSymbol* t) {
        auto it = vtables_.find(t);
        return it == vtables_.end() ? nullptr : it->second;
    }

    // CG19: lower `operand is T` / `operand is not T` / `operand as T`.
    // `is`/`is not` yield bool; `as` yields the operand reference when the
    // test holds, else null. The runtime check walks the operand's type
    // descriptor for T's descriptor address (an is-a test over classes and
    // implemented interfaces, §12.7).
    llvm::Value* lowerTypeTest(const ast::TypeTestExpr& te) {
        if (!te.operand || !te.testType) return nullptr;
        llvm::Value* obj = lowerExpr(*te.operand);
        if (!obj) return nullptr;
        sema::TypeRef* rt = ctx_.resolvedType(*te.testType);
        if (!rt || rt->kind != sema::TypeRef::Kind::Named || !rt->declaration)
            return nullptr;
        auto dit = descArray_.find(rt->declaration);
        if (dit == descArray_.end()) return nullptr;
        llvm::Value* isA = emitIsA(obj, dit->second);
        if (te.op == "is") return isA;
        if (te.op == "is not")
            return builder_.CreateXor(isA, builder_.getInt1(true));
        if (te.op == "as")
            return builder_.CreateSelect(
                isA, obj, llvm::ConstantPointerNull::get(ptrTy()));
        return nullptr;
    }

    // CG19: emit a runtime is-a test — is `targetDesc` present in `obj`'s
    // type-descriptor array (vtable[0])? Null operand → false. Returns i1.
    llvm::Value* emitIsA(llvm::Value* obj, llvm::GlobalVariable* targetDesc) {
        auto* nullBB = llvm::BasicBlock::Create(llvm_, "isa.null", currentFn_);
        auto* loadBB = llvm::BasicBlock::Create(llvm_, "isa.load", currentFn_);
        auto* bodyBB = llvm::BasicBlock::Create(llvm_, "isa.body", currentFn_);
        auto* matchBB = llvm::BasicBlock::Create(llvm_, "isa.match", currentFn_);
        auto* nextBB = llvm::BasicBlock::Create(llvm_, "isa.next", currentFn_);
        auto* endBB = llvm::BasicBlock::Create(llvm_, "isa.end", currentFn_);

        llvm::Value* objIsNull = builder_.CreateICmpEQ(
            obj, llvm::ConstantPointerNull::get(ptrTy()));
        builder_.CreateCondBr(objIsNull, nullBB, loadBB);

        builder_.SetInsertPoint(nullBB);
        builder_.CreateBr(endBB);

        builder_.SetInsertPoint(loadBB);
        llvm::Value* vptr = builder_.CreateLoad(ptrTy(), obj);
        llvm::Value* desc = builder_.CreateLoad(ptrTy(), vptr);  // vtable[0]
        builder_.CreateBr(bodyBB);

        builder_.SetInsertPoint(bodyBB);
        auto* idx = builder_.CreatePHI(i32(), 2);
        idx->addIncoming(builder_.getInt32(0), loadBB);
        llvm::Value* elem = builder_.CreateLoad(
            ptrTy(), builder_.CreateGEP(ptrTy(), desc, {idx}));
        llvm::Value* atNull = builder_.CreateICmpEQ(
            elem, llvm::ConstantPointerNull::get(ptrTy()));
        builder_.CreateCondBr(atNull, endBB, matchBB);

        builder_.SetInsertPoint(matchBB);
        llvm::Value* match = builder_.CreateICmpEQ(elem, targetDesc);
        builder_.CreateCondBr(match, endBB, nextBB);

        builder_.SetInsertPoint(nextBB);
        idx->addIncoming(builder_.CreateAdd(idx, builder_.getInt32(1)), nextBB);
        builder_.CreateBr(bodyBB);

        builder_.SetInsertPoint(endBB);
        auto* result = builder_.CreatePHI(i1(), 3);
        result->addIncoming(builder_.getInt1(false), nullBB);
        result->addIncoming(builder_.getInt1(false), bodyBB);    // hit null
        result->addIncoming(builder_.getInt1(true), matchBB);    // matched
        return result;
    }

    // CG13: the class's user destructor body, if it declares one.
    const ast::DestructorDecl* findDestructor(const sema::TypeSymbol* ts) {
        for (const sema::Symbol* m : ts->members) {
            if (dynamic_cast<const sema::DestructorSymbol*>(m)) {
                return dynamic_cast<const ast::DestructorDecl*>(m->decl);
            }
        }
        return nullptr;
    }

    // CG13: emit the compiler-generated Dispose body. This slice runs the
    // user destructor body (if any) over `this`. Idempotency (the
    // run-once flag), owned-field auto-disposal (§28.5), and base-class
    // chaining (§28.4) are deferred to later slices — so Dispose is
    // currently correct for classes with a destructor and no owned fields
    // or disposable base. `using var` (the only caller in this slice)
    // invokes it exactly once, so the missing idempotency is not yet
    // observable.
    void emitDisposerBody(const sema::TypeSymbol* ts, llvm::Function* fn) {
        currentFn_ = fn;
        currentThis_ = &*fn->args().begin();
        locals_.clear();
        paramSlots_.clear();
        disposeStack_.clear();
        loopStack_.clear();
        builder_.SetInsertPoint(llvm::BasicBlock::Create(llvm_, "entry", fn));
        bool terminated = false;
        if (const ast::DestructorDecl* dd = findDestructor(ts)) {
            terminated = lowerScopedBlock(dd->body);
        }
        // §28.4: after the destructor body, dispose owned fields (reverse
        // declaration order), then chain to the base class's Dispose.
        // (A destructor that returns early skips this — destructors with
        // both an early return and owned fields are a deferred corner.)
        if (!terminated) {
            emitOwnedFieldDisposals(ts);
            emitBaseDisposeChain(ts);
            builder_.CreateRetVoid();
        }
    }

    // CG18 (§28.5): dispose the class's owned reference fields (class- OR
    // interface-typed) in reverse declaration order, null-checking each.
    // Disposal is DYNAMIC — dispatched through the field object's own
    // vtable slot 0 — so an owned field that holds a subclass (or, for an
    // interface-typed field, whichever class implements it) runs the
    // correct runtime Dispose (§28.4–28.5).
    void emitOwnedFieldDisposals(const sema::TypeSymbol* ts) {
        std::vector<const sema::FieldSymbol*> owned;
        for (const sema::Symbol* m : ts->members) {
            auto* fs = dynamic_cast<const sema::FieldSymbol*>(m);
            if (!fs || !isOwnedReferenceField(fs)) continue;
            if (fieldIndex_.find(fs) == fieldIndex_.end()) continue;
            owned.push_back(fs);
        }
        for (auto it = owned.rbegin(); it != owned.rend(); ++it) {
            llvm::StructType* st = classStruct(ts);
            unsigned idx = fieldIndex_[*it];
            llvm::Value* addr = builder_.CreateStructGEP(st, currentThis_, idx);
            llvm::Value* val = builder_.CreateLoad(ptrTy(), addr);
            llvm::Value* isNull = builder_.CreateICmpEQ(
                val, llvm::ConstantPointerNull::get(ptrTy()));
            auto* dispBB = llvm::BasicBlock::Create(llvm_, "owned.dispose",
                                                    currentFn_);
            auto* contBB = llvm::BasicBlock::Create(llvm_, "owned.cont",
                                                    currentFn_);
            builder_.CreateCondBr(isNull, contBB, dispBB);
            builder_.SetInsertPoint(dispBB);
            emitDynamicDispose(val);
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
        }
    }

    // CG18: call an object's Dispose dynamically — load its vtable pointer
    // (instance slot 0), load vtable slot 0 (Dispose), and call it. The
    // object must be non-null. Resolves to the runtime class's Dispose.
    void emitDynamicDispose(llvm::Value* obj) {
        // vtable layout: [0]=type descriptor, [1]=Dispose, [2+]=methods.
        llvm::Value* vptr = builder_.CreateLoad(ptrTy(), obj);
        llvm::Value* slot = builder_.CreateGEP(ptrTy(), vptr,
                                               {builder_.getInt32(1)});
        llvm::Value* disposeFn = builder_.CreateLoad(ptrTy(), slot);
        auto* fnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_),
                                             {ptrTy()}, /*isVarArg=*/false);
        builder_.CreateCall(fnTy, disposeFn, {obj});
    }

    // §28.4 step 3: chain to the base class's Dispose. baseTypes[0] is
    // conventionally the base class when it resolves to one.
    void emitBaseDisposeChain(const sema::TypeSymbol* ts) {
        for (const sema::TypeSymbol* base : ts->baseTypes) {
            if (!base || base->kind != ast::TypeDeclKind::Class) continue;
            auto it = disposerFns_.find(base);
            if (it != disposerFns_.end()) builder_.CreateCall(it->second,
                                                              {currentThis_});
            return;  // single base class
        }
    }

    static bool hasOwnedModifier(const sema::FieldSymbol* fs) {
        for (auto mod : fs->modifiers) {
            if (mod == "owned") return true;
        }
        return false;
    }

    // CG18: an `owned` field whose type is a reference type (class or
    // interface) — the kinds that carry a Dispose and can be disposed
    // dynamically through the held object's vtable.
    bool isOwnedReferenceField(const sema::FieldSymbol* fs) {
        if (!hasOwnedModifier(fs)) return false;
        auto* fd = dynamic_cast<const ast::FieldDecl*>(fs->decl);
        if (!fd || !fd->type) return false;
        auto* rt = ctx_.resolvedType(*fd->type);
        if (!rt || rt->kind != sema::TypeRef::Kind::Named || !rt->declaration)
            return false;
        auto k = rt->declaration->kind;
        return k == ast::TypeDeclKind::Class || k == ast::TypeDeclKind::Interface;
    }

    // Pass 2: emit a function's body.
    void emitFunctionBody(sema::MethodSymbol* ms, llvm::Function* fn) {
        auto* md = dynamic_cast<const ast::MethodDecl*>(ms->decl);
        if (!md) return;
        currentFn_ = fn;
        currentThis_ = nullptr;
        locals_.clear();
        paramSlots_.clear();
        disposeStack_.clear();
        loopStack_.clear();
        builder_.SetInsertPoint(llvm::BasicBlock::Create(llvm_, "entry", fn));

        const std::vector<sema::Symbol*>* paramSyms = paramSymbolsOf(md);
        bindParameters(fn, /*hasThis=*/!isStaticMethod(ms), paramSyms);

        bool terminated = false;
        if (md->bodyKind == ast::MethodBodyKind::Expression && md->exprBody) {
            llvm::Value* v = lowerExpr(*md->exprBody);
            terminated = emitReturn(fn, v);
        } else if (md->bodyKind == ast::MethodBodyKind::Block) {
            terminated = lowerScopedBlock(md->stmts);
        }
        if (!terminated) emitImplicitReturn(fn);
    }

    // CG10: emit a constructor body. Always an instance function (leading
    // `this`); its params bind to the ConstructorSymbol's ParameterSymbols.
    // Field initializers run in `lowerNew` before the call, so the body
    // only runs the explicit statements. Returns void.
    void emitConstructorBody(sema::ConstructorSymbol* cs, llvm::Function* fn) {
        auto* cd = dynamic_cast<const ast::ConstructorDecl*>(cs->decl);
        if (!cd) return;
        currentFn_ = fn;
        currentThis_ = nullptr;
        locals_.clear();
        paramSlots_.clear();
        disposeStack_.clear();
        loopStack_.clear();
        builder_.SetInsertPoint(llvm::BasicBlock::Create(llvm_, "entry", fn));
        bindParameters(fn, /*hasThis=*/true, &cs->params);

        // CG16 (§12.8): a leading `base(...)`/`this(...)` chain call runs
        // before the body. `this(...)` delegates to a sibling ctor (which
        // runs the base chain); `base(...)` runs the base ctor. With
        // neither written, an implicit `base()` runs the base class's
        // parameterless ctor.
        bool isThis = false;
        std::size_t startIdx = 0;
        if (!cd->body.empty()) {
            if (const ast::CallExpr* chain =
                    chainCallOf(*cd->body[0], isThis)) {
                emitChainCall(chain, isThis, cs);
                startIdx = 1;
            } else {
                emitImplicitBase(cs->parent, currentThis_);
            }
        } else {
            emitImplicitBase(cs->parent, currentThis_);
        }

        bool terminated = lowerScopedBlock(cd->body, startIdx);
        if (!terminated) builder_.CreateRetVoid();
    }

    // CG16: if `s` is a leading constructor chain call (`base(...)` or
    // `this(...)` as an ExpressionStmt-of-CallExpr), return the call and
    // set `isThis`; else null.
    const ast::CallExpr* chainCallOf(const ast::Stmt& s, bool& isThis) {
        auto* es = dynamic_cast<const ast::ExpressionStmt*>(&s);
        if (!es || !es->expr) return nullptr;
        auto* call = dynamic_cast<const ast::CallExpr*>(es->expr.get());
        if (!call || !call->callee) return nullptr;
        auto* id = dynamic_cast<const ast::IdentifierExpr*>(call->callee.get());
        if (!id) return nullptr;
        if (id->name == "this") { isThis = true; return call; }
        if (id->name == "base") { isThis = false; return call; }
        return nullptr;
    }

    // CG16: the base class (baseTypes[0] when it is a class), else null.
    const sema::TypeSymbol* baseClassOf(const sema::TypeSymbol* t) {
        for (const sema::TypeSymbol* b : t->baseTypes) {
            if (b && b->kind == ast::TypeDeclKind::Class) return b;
        }
        return nullptr;
    }

    // CG16: a non-static ctor of `cls` taking `n` parameters (excluding
    // `exclude`). Overload resolution is by arity only for now.
    sema::ConstructorSymbol* ctorByArity(const sema::TypeSymbol* cls,
                                         std::size_t n,
                                         const sema::ConstructorSymbol* exclude) {
        if (!cls) return nullptr;
        for (sema::Symbol* m : cls->members) {
            auto* cs = dynamic_cast<sema::ConstructorSymbol*>(m);
            if (!cs || cs->isStatic || cs == exclude) continue;
            if (cs->params.size() == n) return cs;
        }
        return nullptr;
    }

    bool hasAnyConstructor(const sema::TypeSymbol* t) {
        for (sema::Symbol* m : t->members) {
            auto* cs = dynamic_cast<sema::ConstructorSymbol*>(m);
            if (cs && !cs->isStatic) return true;
        }
        return false;
    }

    // CG16: emit a resolved chain call with `this` + lowered arguments.
    void emitChainCall(const ast::CallExpr* call, bool isThis,
                       sema::ConstructorSymbol* cs) {
        const sema::TypeSymbol* targetCls =
            isThis ? cs->parent : baseClassOf(cs->parent);
        if (!targetCls) return;  // base() at the root (Object): nothing to run
        std::size_t n = call->args.size();
        sema::ConstructorSymbol* target =
            ctorByArity(targetCls, n, isThis ? cs : nullptr);
        if (!target) {
            // Explicit `base()` with no matching base ctor: the base's own
            // base may still need an implicit chain.
            if (!isThis && n == 0) emitImplicitBase(targetCls, currentThis_);
            return;
        }
        auto it = ctorFns_.find(target);
        if (it == ctorFns_.end()) return;
        std::vector<llvm::Value*> args;
        args.push_back(currentThis_);
        for (const auto& a : call->args) {
            if (!a.value) return;
            llvm::Value* v = lowerExpr(*a.value);
            if (!v) return;
            args.push_back(v);
        }
        if (args.size() == it->second->arg_size()) {
            builder_.CreateCall(it->second, args);
        }
    }

    // CG16 (§12.8): the implicit `base()` — run the base class's
    // parameterless ctor (which chains further). If the base has no ctor,
    // recurse to its base so a deeper ctor still runs.
    void emitImplicitBase(const sema::TypeSymbol* cls, llvm::Value* obj) {
        const sema::TypeSymbol* b = baseClassOf(cls);
        if (!b) return;
        if (sema::ConstructorSymbol* target = ctorByArity(b, 0, nullptr)) {
            auto it = ctorFns_.find(target);
            if (it != ctorFns_.end() && it->second->arg_size() == 1) {
                builder_.CreateCall(it->second, {obj});
            }
        } else {
            emitImplicitBase(b, obj);
        }
    }

    // Bind each parameter to an entry-block alloca. When `hasThis`, the
    // first arg is `this` (kept as a value in currentThis_, not a slot —
    // it isn't reassignable). Remaining args map to `paramSyms`.
    void bindParameters(llvm::Function* fn, bool hasThis,
                        const std::vector<sema::Symbol*>* paramSyms) {
        std::size_t paramIdx = 0;
        bool first = true;
        for (auto& arg : fn->args()) {
            if (hasThis && first) {
                currentThis_ = &arg;
                first = false;
                continue;
            }
            first = false;
            llvm::AllocaInst* slot = entryAlloca(arg.getType());
            builder_.CreateStore(&arg, slot);
            if (paramSyms && paramIdx < paramSyms->size() &&
                (*paramSyms)[paramIdx]) {
                paramSlots_[(*paramSyms)[paramIdx]] = slot;
            }
            ++paramIdx;
        }
    }

    // The ParameterSymbol list for a method (the MethodSymbol owning md).
    const std::vector<sema::Symbol*>* paramSymbolsOf(const ast::MethodDecl* md) {
        for (const auto& sym : ctx_.symbols()) {
            if (sym->decl != md) continue;
            if (auto* ms = dynamic_cast<sema::MethodSymbol*>(sym.get())) {
                return &ms->params;
            }
        }
        return nullptr;
    }

    bool emitReturn(llvm::Function* fn, llvm::Value* v) {
        if (fn->getReturnType()->isVoidTy()) builder_.CreateRetVoid();
        else builder_.CreateRet(v ? v
                                  : llvm::Constant::getNullValue(
                                        fn->getReturnType()));
        return true;
    }

    void emitImplicitReturn(llvm::Function* fn) {
        if (fn->getReturnType()->isVoidTy()) builder_.CreateRetVoid();
        else builder_.CreateRet(
            llvm::Constant::getNullValue(fn->getReturnType()));
    }

    // CG3/CG8: a local/const declaration. The slot type comes from the
    // initializer's lowered value (so pointer-typed locals like `var p
    // := new T()` work); without an initializer it falls back to the
    // declared type (or i32).
    void lowerLocalDecl(const ast::Node* declNode, const ast::Expr* init,
                        const ast::Type* declType) {
        llvm::Value* v = init ? lowerExpr(*init) : nullptr;
        llvm::Type* slotTy =
            v ? v->getType() : (declType ? lowerType(declType) : i32());
        llvm::AllocaInst* slot = entryAlloca(slotTy);
        locals_[declNode] = slot;
        if (v) builder_.CreateStore(v, slot);
    }

    // CG13: register a `using var`'s scope-based disposal (§28.3). The
    // local is disposed at the end of its enclosing block in reverse
    // declaration order. Recorded against the innermost open scope.
    void registerUsingDisposal(const ast::LocalDecl* ld) {
        if (disposeStack_.empty()) return;
        const sema::TypeSymbol* cls = classOfLocal(ld);
        auto it = locals_.find(ld);
        if (!cls || it == locals_.end()) return;
        if (disposerFns_.find(cls) == disposerFns_.end()) return;
        disposeStack_.back().push_back({it->second, cls});
    }

    // CG13: the class type of a local — from its declared type, else the
    // resolved type of a `new T(...)` initializer.
    const sema::TypeSymbol* classOfLocal(const ast::LocalDecl* ld) {
        if (ld->type) {
            if (auto* rt = ctx_.resolvedType(*ld->type)) {
                if (rt->kind == sema::TypeRef::Kind::Named) return rt->declaration;
            }
        }
        if (auto* ne =
                dynamic_cast<const ast::NewExpr*>(ld->initializer.get())) {
            if (ne->type) {
                if (auto* rt = ctx_.resolvedType(*ne->type)) {
                    if (rt->kind == sema::TypeRef::Kind::Named)
                        return rt->declaration;
                }
            }
        }
        return nullptr;
    }

    // CG13: lower a block as a disposal scope. Pushes a fresh scope; on
    // normal fall-through, disposes its `using var`s in reverse and pops.
    // If the block terminated early (return/break/continue), that
    // terminator already emitted the needed disposals, so we only pop.
    bool lowerScopedBlock(const std::vector<std::unique_ptr<ast::Stmt>>& body,
                          std::size_t startIdx = 0) {
        disposeStack_.emplace_back();
        bool terminated = false;
        for (std::size_t i = startIdx; i < body.size(); ++i) {
            if (body[i] && lowerStmt(*body[i])) { terminated = true; break; }
        }
        if (!terminated) emitDisposals(disposeStack_.back());
        disposeStack_.pop_back();
        return terminated;
    }

    // CG13: emit Dispose calls for one scope's `using var`s, in reverse
    // declaration order (§28.4 ordering).
    void emitDisposals(const std::vector<Disposal>& scope) {
        for (auto it = scope.rbegin(); it != scope.rend(); ++it) {
            if (disposerFns_.find(it->cls) == disposerFns_.end()) continue;
            // CG18: dispatch Dispose dynamically through the object's
            // vtable, so a `using var` bound to a subclass instance runs
            // the correct runtime Dispose.
            llvm::Value* obj = builder_.CreateLoad(ptrTy(), it->slot);
            emitDynamicDispose(obj);
        }
    }

    // CG13: dispose every open scope (innermost → outermost) — the
    // cleanup a `return` runs before leaving the method.
    void emitAllDisposals() {
        for (auto it = disposeStack_.rbegin(); it != disposeStack_.rend(); ++it)
            emitDisposals(*it);
    }

    // CG13: dispose scopes from the innermost down to (and including) the
    // loop body's scope — the cleanup `break`/`continue` runs before
    // jumping out of / around the loop body.
    void emitLoopDisposals(std::size_t loopScopeDepth) {
        for (std::size_t i = disposeStack_.size(); i-- > loopScopeDepth;)
            emitDisposals(disposeStack_[i]);
    }

    // Lower a statement list into the current block; returns true if the
    // list terminated the block (so the caller stops adding to it).
    bool lowerBody(const std::vector<std::unique_ptr<ast::Stmt>>& body) {
        for (const auto& s : body) {
            if (s && lowerStmt(*s)) return true;
        }
        return false;
    }

    // Lower one statement into the current block. Returns true if the
    // statement terminated the block (a return), so the caller stops.
    // CG3: LocalDecl/LocalConstDecl, AssignmentStmt, ReturnStmt,
    // ExpressionStmt, DiscardStmt.
    bool lowerStmt(const ast::Stmt& s) {
        if (auto* ld = dynamic_cast<const ast::LocalDecl*>(&s)) {
            lowerLocalDecl(ld, ld->initializer.get(), ld->type.get());
            if (ld->isUsing) registerUsingDisposal(ld);
            return false;
        }
        if (auto* lc = dynamic_cast<const ast::LocalConstDecl*>(&s)) {
            lowerLocalDecl(lc, lc->value.get(), lc->type.get());
            return false;
        }
        if (auto* as = dynamic_cast<const ast::AssignmentStmt*>(&s)) {
            if (as->op == ":=" && as->target && as->value) {
                if (llvm::AllocaInst* slot = localSlot(*as->target)) {
                    if (llvm::Value* v = lowerExpr(*as->value)) {
                        builder_.CreateStore(v, slot);
                    }
                } else if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(
                               as->target.get())) {
                    // CG8: field assignment `obj.f := v`.
                    llvm::Type* elemTy = nullptr;
                    if (llvm::Value* addr = fieldAddr(*ma, elemTy)) {
                        if (llvm::Value* v = lowerExpr(*as->value)) {
                            builder_.CreateStore(v, addr);
                        }
                    }
                } else if (llvm::GlobalVariable* gv =
                               staticFieldGlobal(*as->target)) {
                    // CG12: static field assignment `_s := v`.
                    if (llvm::Value* v = lowerExpr(*as->value)) {
                        builder_.CreateStore(v, gv);
                    }
                } else {
                    // CG9: implicit-`this` field assignment `_f := v`.
                    llvm::Type* elemTy = nullptr;
                    if (llvm::Value* addr =
                            implicitFieldAddr(*as->target, elemTy)) {
                        if (llvm::Value* v = lowerExpr(*as->value)) {
                            builder_.CreateStore(v, addr);
                        }
                    }
                }
            }
            return false;
        }
        if (auto* rs = dynamic_cast<const ast::ReturnStmt*>(&s)) {
            // The return value is evaluated first, then in-scope `using
            // var`s are disposed (§28.3), then control leaves — so the
            // returned value reflects pre-disposal state.
            llvm::Value* v = rs->value ? lowerExpr(*rs->value) : nullptr;
            emitAllDisposals();
            emitReturn(currentFn_, v);
            return true;
        }
        if (auto* es = dynamic_cast<const ast::ExpressionStmt*>(&s)) {
            if (es->expr) lowerExpr(*es->expr);  // side effects only (CG3: none)
            return false;
        }
        if (auto* ds = dynamic_cast<const ast::DiscardStmt*>(&s)) {
            if (ds->value) lowerExpr(*ds->value);
            return false;
        }
        if (auto* ifs = dynamic_cast<const ast::IfStmt*>(&s)) {
            return lowerIf(*ifs);
        }
        if (auto* ws = dynamic_cast<const ast::WhileStmt*>(&s)) {
            return lowerWhile(*ws);
        }
        // CG11: break / continue jump to the innermost loop's exit /
        // condition block. CG13: they first dispose the loop body's (and
        // any nested) `using var`s. Both terminate the current block.
        if (dynamic_cast<const ast::BreakStmt*>(&s)) {
            if (!loopStack_.empty()) {
                emitLoopDisposals(loopStack_.back().scopeDepth);
                builder_.CreateBr(loopStack_.back().breakTarget);
                return true;
            }
            return false;
        }
        if (dynamic_cast<const ast::ContinueStmt*>(&s)) {
            if (!loopStack_.empty()) {
                emitLoopDisposals(loopStack_.back().scopeDepth);
                builder_.CreateBr(loopStack_.back().continueTarget);
                return true;
            }
            return false;
        }
        // CG13: a `do … end` block is a disposal scope (§A.6, §28.3).
        if (auto* db = dynamic_cast<const ast::DoEndBlock*>(&s)) {
            return lowerScopedBlock(db->body);
        }
        return false;  // unhandled statement kinds land in later slices
    }

    // CG5: if / elif* / else. Each condition is tested in its own block;
    // a branch that doesn't terminate falls through to a lazily-created
    // merge block. Returns true only when every path terminates (no
    // merge needed) — so we never leave a danging unterminated block.
    bool lowerIf(const ast::IfStmt& ifs) {
        llvm::BasicBlock* mergeBB = nullptr;
        auto merge = [&]() -> llvm::BasicBlock* {
            if (!mergeBB) mergeBB = llvm::BasicBlock::Create(
                llvm_, "if.end", currentFn_);
            return mergeBB;
        };

        // The if-branch and each elif form a (condition, body) test chain.
        std::vector<const ast::IfStmt::Branch*> chain;
        chain.push_back(&ifs.ifBranch);
        for (const auto& el : ifs.elifBranches) chain.push_back(&el);

        bool allPathsTerminate = true;
        for (const ast::IfStmt::Branch* br : chain) {
            llvm::Value* cond =
                br->condition ? lowerExpr(*br->condition) : nullptr;
            auto* bodyBB = llvm::BasicBlock::Create(llvm_, "if.body", currentFn_);
            auto* nextBB = llvm::BasicBlock::Create(llvm_, "if.next", currentFn_);
            if (cond) builder_.CreateCondBr(cond, bodyBB, nextBB);
            else builder_.CreateBr(nextBB);  // defensive

            builder_.SetInsertPoint(bodyBB);
            if (!lowerScopedBlock(br->body)) {
                allPathsTerminate = false;
                builder_.CreateBr(merge());
            }
            builder_.SetInsertPoint(nextBB);  // next test / else goes here
        }

        // The final fall-through block is the else (or an empty pass-through).
        if (ifs.hasElse) {
            if (!lowerScopedBlock(ifs.elseBody)) {
                allPathsTerminate = false;
                builder_.CreateBr(merge());
            }
        } else {
            allPathsTerminate = false;
            builder_.CreateBr(merge());
        }

        if (mergeBB) {
            builder_.SetInsertPoint(mergeBB);
            return false;
        }
        return allPathsTerminate;
    }

    // CG5: while loop. cond block tests, body block loops back.
    // CG11: `continue` targets condBB, `break` targets endBB.
    bool lowerWhile(const ast::WhileStmt& ws) {
        auto* condBB = llvm::BasicBlock::Create(llvm_, "while.cond", currentFn_);
        auto* bodyBB = llvm::BasicBlock::Create(llvm_, "while.body", currentFn_);
        auto* endBB = llvm::BasicBlock::Create(llvm_, "while.end", currentFn_);

        builder_.CreateBr(condBB);
        builder_.SetInsertPoint(condBB);
        llvm::Value* cond = ws.condition ? lowerExpr(*ws.condition) : nullptr;
        if (cond) builder_.CreateCondBr(cond, bodyBB, endBB);
        else builder_.CreateBr(endBB);

        builder_.SetInsertPoint(bodyBB);
        // CG13: the loop body is a disposal scope; break/continue unwind
        // down to this depth (the scope index the body will occupy).
        loopStack_.push_back({condBB, endBB, disposeStack_.size()});
        bool bodyTerminated = lowerScopedBlock(ws.body);
        loopStack_.pop_back();
        if (!bodyTerminated) builder_.CreateBr(condBB);  // back-edge

        builder_.SetInsertPoint(endBB);
        return false;  // control may always exit the loop
    }

    // If `e` names a local or parameter, return its storage slot; else null.
    llvm::AllocaInst* localSlot(const ast::Expr& e) {
        if (!dynamic_cast<const ast::IdentifierExpr*>(&e)) return nullptr;
        sema::Symbol* sym = ctx_.exprSymbol(e);
        if (!sym) return nullptr;
        if (dynamic_cast<sema::ParameterSymbol*>(sym)) {
            auto it = paramSlots_.find(sym);
            return it == paramSlots_.end() ? nullptr : it->second;
        }
        if (auto* loc = dynamic_cast<sema::LocalSymbol*>(sym)) {
            if (!loc->decl) return nullptr;
            auto it = locals_.find(loc->decl);
            return it == locals_.end() ? nullptr : it->second;
        }
        return nullptr;
    }

    // Lower an expression to an LLVM value.
    //   CG1: integer literals.
    //   CG2: integer arithmetic / bitwise / shift binary ops, unary -/~/+.
    llvm::Value* lowerExpr(const ast::Expr& e) {
        if (auto* lit = dynamic_cast<const ast::IntLiteralExpr*>(&e)) {
            return llvm::ConstantInt::get(
                i32(),
                static_cast<std::uint64_t>(parseIntLiteral(lit->text)),
                /*isSigned=*/true);
        }
        if (auto* boolLit = dynamic_cast<const ast::BoolLiteralExpr*>(&e)) {
            return llvm::ConstantInt::get(i1(), boolLit->value ? 1 : 0);
        }
        if (auto* bin = dynamic_cast<const ast::BinaryExpr*>(&e)) {
            return lowerBinary(*bin);
        }
        if (auto* un = dynamic_cast<const ast::UnaryExpr*>(&e)) {
            return lowerUnary(*un);
        }
        if (auto* cond = dynamic_cast<const ast::ConditionalExpr*>(&e)) {
            return lowerConditional(*cond);
        }
        if (auto* call = dynamic_cast<const ast::CallExpr*>(&e)) {
            return lowerCall(*call);
        }
        if (auto* ne = dynamic_cast<const ast::NewExpr*>(&e)) {
            return lowerNew(*ne);
        }
        // CG19: `is` / `is not` / `as` type tests (§12.7).
        if (auto* te = dynamic_cast<const ast::TypeTestExpr*>(&e)) {
            return lowerTypeTest(*te);
        }
        // CG8: field read `obj.f`.
        if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
            llvm::Type* elemTy = nullptr;
            if (llvm::Value* addr = fieldAddr(*ma, elemTy)) {
                return builder_.CreateLoad(elemTy, addr);
            }
        }
        if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(&e)) {
            // CG9: `this` (and `base`, same instance) is the receiver ptr.
            if (id->name == "this" || id->name == "base") return currentThis_;
            // CG12: a bare identifier resolving to a static field reads
            // its backing global.
            if (llvm::GlobalVariable* gv = staticFieldGlobal(e)) {
                return builder_.CreateLoad(gv->getValueType(), gv);
            }
            // CG9: a bare identifier resolving to an instance field is an
            // implicit-`this` field read.
            llvm::Type* elemTy = nullptr;
            if (llvm::Value* addr = implicitFieldAddr(e, elemTy)) {
                return builder_.CreateLoad(elemTy, addr);
            }
            // CG3: a reference to a local variable loads from its slot.
            if (llvm::AllocaInst* slot = localSlot(e)) {
                return builder_.CreateLoad(slot->getAllocatedType(), slot);
            }
        }
        return nullptr;
    }

    // CG9: address of an implicitly-`this`-qualified instance field
    // referenced bare inside an instance method (`_x` rather than
    // `this._x`). Returns the GEP and sets `elemTy`; null if `e` is not
    // such a reference (not an identifier, not a field, or no `this`).
    llvm::Value* implicitFieldAddr(const ast::Expr& e, llvm::Type*& elemTy) {
        if (!dynamic_cast<const ast::IdentifierExpr*>(&e)) return nullptr;
        auto* fs = dynamic_cast<sema::FieldSymbol*>(ctx_.exprSymbol(e));
        if (!fs || !fs->parent || !currentThis_) return nullptr;
        auto idxIt = fieldIndex_.find(fs);
        if (idxIt == fieldIndex_.end()) return nullptr;  // static field
        llvm::StructType* st = classStruct(fs->parent);
        elemTy = st->getElementType(idxIt->second);
        return builder_.CreateStructGEP(st, currentThis_, idxIt->second);
    }

    // Slice CG2: integer binary operators. NOTE (§6.1): `+ - * / mod`
    // are *overflow-checked* in Bezel (raise OverflowException) and
    // `/`÷0 raises DivideByZeroException — those checks need the
    // exception runtime (§31), which is not built yet, so this slice
    // emits the value computation only (correct for the non-overflowing,
    // non-zero-divisor case). The checks land with exceptions codegen.
    // Bitwise/shift ops are not overflow-checked, so they are exact now.
    llvm::Value* lowerBinary(const ast::BinaryExpr& bin) {
        if (!bin.left || !bin.right) return nullptr;
        llvm::Value* l = lowerExpr(*bin.left);
        llvm::Value* r = lowerExpr(*bin.right);
        if (!l || !r) return nullptr;
        const auto& op = bin.op;
        if (op == "+") return builder_.CreateAdd(l, r);
        if (op == "-") return builder_.CreateSub(l, r);
        if (op == "*") return builder_.CreateMul(l, r);
        if (op == "/") return builder_.CreateSDiv(l, r);
        if (op == "mod") return builder_.CreateSRem(l, r);
        if (op == "&") return builder_.CreateAnd(l, r);
        if (op == "|") return builder_.CreateOr(l, r);
        if (op == "^") return builder_.CreateXor(l, r);
        if (op == "<<") return builder_.CreateShl(l, r);
        if (op == ">>") return builder_.CreateAShr(l, r);  // signed types
        // CG4: integer comparisons → i1 (Bezel uses `=` / `<>` for
        // equality, signed relational ops).
        if (op == "=") return builder_.CreateICmpEQ(l, r);
        if (op == "<>") return builder_.CreateICmpNE(l, r);
        if (op == "<") return builder_.CreateICmpSLT(l, r);
        if (op == ">") return builder_.CreateICmpSGT(l, r);
        if (op == "<=") return builder_.CreateICmpSLE(l, r);
        if (op == ">=") return builder_.CreateICmpSGE(l, r);
        // CG4: logical and/or on i1. NOTE: §17 specifies short-circuit
        // evaluation; this eager form is observationally identical while
        // operands are side-effect-free (no calls lowered yet) — true
        // short-circuiting lands once call lowering exists.
        if (op == "and") return builder_.CreateAnd(l, r);
        if (op == "or") return builder_.CreateOr(l, r);
        return nullptr;
    }

    llvm::Value* lowerUnary(const ast::UnaryExpr& un) {
        if (!un.operand) return nullptr;
        llvm::Value* v = lowerExpr(*un.operand);
        if (!v) return nullptr;
        const auto& op = un.op;
        if (op == "-") return builder_.CreateNeg(v);   // §6.1 checked; see above
        if (op == "~") return builder_.CreateNot(v);   // bitwise complement
        if (op == "+") return v;                       // identity
        if (op == "not") return builder_.CreateNot(v); // i1 logical negation
        return nullptr;
    }

    // Declare (once) `ptr @calloc(i64, i64)` from libc — zero-inits the
    // allocation, matching value-type field defaults.
    llvm::FunctionCallee callocFn() {
        auto* fnTy = llvm::FunctionType::get(ptrTy(), {i64(), i64()}, false);
        return module_->getOrInsertFunction("calloc", fnTy);
    }

    // CG8: `new T(...)` — heap-allocate (zeroed) an instance of T and run
    // its field initializers. (Explicit constructors, ownership-driven
    // disposal/free, and constructor args are later slices; trivial
    // instances currently leak — freed once disposal lowering lands.)
    llvm::Value* lowerNew(const ast::NewExpr& ne) {
        if (!ne.type) return nullptr;
        sema::TypeRef* rt = ctx_.resolvedType(*ne.type);
        if (!rt || rt->kind != sema::TypeRef::Kind::Named || !rt->declaration) {
            return nullptr;
        }
        const sema::TypeSymbol* t = rt->declaration;
        llvm::StructType* st = classStruct(t);

        // sizeof(st) via the null-GEP / ptrtoint idiom (no DataLayout dep).
        llvm::Value* sizeofSt = builder_.CreatePtrToInt(
            builder_.CreateGEP(st, llvm::ConstantPointerNull::get(ptrTy()),
                               {builder_.getInt32(1)}),
            i64());
        llvm::Value* obj = builder_.CreateCall(
            callocFn(), {builder_.getInt64(1), sizeofSt});

        // CG17: install the vtable pointer (instance slot 0) before field
        // initializers / the constructor, so virtual dispatch on the new
        // object resolves to its own class.
        if (llvm::GlobalVariable* vt = vtableOf(t)) {
            builder_.CreateStore(vt, builder_.CreateStructGEP(st, obj, 0));
        }

        // Run field initializers, inherited (base-first) then own, so a
        // `new Derived()` initializes base fields too (calloc already
        // zeroed the rest). CG16's base-ctor chaining runs ctor *bodies*,
        // not these initializers, so there is no double-initialization.
        std::vector<const sema::FieldSymbol*> fields;
        collectInstanceFields(t, fields);
        for (const sema::FieldSymbol* fs : fields) {
            auto* fd = dynamic_cast<const ast::FieldDecl*>(fs->decl);
            if (!fd || !fd->initializer) continue;
            auto idxIt = fieldIndex_.find(fs);
            if (idxIt == fieldIndex_.end()) continue;
            llvm::Value* v = lowerExpr(*fd->initializer);
            if (!v) continue;
            builder_.CreateStore(
                v, builder_.CreateStructGEP(st, obj, idxIt->second));
        }

        // CG10: run the explicit constructor (if any) after the field
        // initializers, passing the new instance as `this` plus the
        // lowered positional arguments. (Constructor overloads and
        // chained `base(...)`/`this(...)` calls are later slices — sema
        // defers the overloaded case too, so at most one ctor reaches us.)
        // CG16: select the ctor by arity (covers single-ctor and
        // arity-distinct overloads; full type-based overload resolution
        // is deferred). The chosen ctor runs its own base/this chain.
        if (sema::ConstructorSymbol* cs =
                ctorByArity(t, ne.args.size(), nullptr)) {
            auto it = ctorFns_.find(cs);
            if (it != ctorFns_.end()) {
                std::vector<llvm::Value*> args;
                args.push_back(obj);
                for (const auto& a : ne.args) {
                    if (!a.value) return obj;
                    llvm::Value* v = lowerExpr(*a.value);
                    if (!v) return obj;
                    args.push_back(v);
                }
                if (args.size() == it->second->arg_size()) {
                    builder_.CreateCall(it->second, args);
                }
            }
        } else if (!hasAnyConstructor(t)) {
            // CG16: no explicit ctor → the implicit ctor runs an implicit
            // `base()`, so a base-class ctor still runs on `new Derived()`.
            emitImplicitBase(t, obj);
        }
        return obj;
    }

    // CG8: address of a field access `obj.f`. Returns the GEP pointer and
    // sets `elemTy` to the field's type. Null if `ma` isn't a field
    // access we can lower.
    llvm::Value* fieldAddr(const ast::MemberAccessExpr& ma,
                           llvm::Type*& elemTy) {
        auto* fs = dynamic_cast<sema::FieldSymbol*>(ctx_.exprSymbol(ma));
        if (!fs || !fs->parent || !ma.target) return nullptr;
        auto idxIt = fieldIndex_.find(fs);
        if (idxIt == fieldIndex_.end()) return nullptr;
        llvm::StructType* st = classStruct(fs->parent);
        llvm::Value* obj = lowerExpr(*ma.target);
        if (!obj) return nullptr;
        elemTy = st->getElementType(idxIt->second);
        return builder_.CreateStructGEP(st, obj, idxIt->second);
    }

    // CG6/CG9: a call whose callee resolves to a known method → direct
    // LLVM call with lowered positional arguments. An instance method
    // (CG9) takes the receiver as an implicit leading `this` argument:
    // `obj.M(a)` passes `obj`; a bare `M(a)` inside an instance method
    // passes the current `this`. (Overloads, named/default args, and
    // virtual dispatch are later slices — calls bind statically here.)
    llvm::Value* lowerCall(const ast::CallExpr& call) {
        if (!call.callee) return nullptr;
        auto* ms = dynamic_cast<sema::MethodSymbol*>(
            ctx_.exprSymbol(*call.callee));
        if (!ms) return nullptr;

        std::vector<llvm::Value*> args;
        bool baseQualified = false;  // `base.M()` → non-virtual (§12.3)
        if (!isStaticMethod(ms)) {
            // Receiver: the member-access target, else the current `this`.
            llvm::Value* recv = nullptr;
            if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(
                    call.callee.get())) {
                if (auto* tid = dynamic_cast<const ast::IdentifierExpr*>(
                        ma->target.get())) {
                    if (tid->name == "base") baseQualified = true;
                }
                recv = ma->target ? lowerExpr(*ma->target) : nullptr;
            } else {
                recv = currentThis_;
            }
            if (!recv) return nullptr;
            args.push_back(recv);
        }
        for (const auto& a : call.args) {
            if (!a.value) return nullptr;
            llvm::Value* v = lowerExpr(*a.value);
            if (!v) return nullptr;
            args.push_back(v);
        }

        // CG17/CG18: virtual dispatch — load the receiver's vtable pointer
        // (instance slot 0) and call through the method's slot. Works even
        // when the statically-resolved method is `abstract` (no concrete
        // function), since the slot is filled by the runtime override. A
        // `base.M()` call bypasses dispatch (§12.3), as do non-virtual.
        auto slotIt = vtableSlot_.find(ms);
        if (!baseQualified && !isStaticMethod(ms) &&
            slotIt != vtableSlot_.end()) {
            llvm::FunctionType* fnTy = methodFnType(ms);
            if (!fnTy || args.size() != fnTy->getNumParams()) return nullptr;
            llvm::Value* recv = args[0];
            llvm::Value* vptr = builder_.CreateLoad(ptrTy(), recv);
            llvm::Value* slotPtr = builder_.CreateGEP(
                ptrTy(), vptr, {builder_.getInt32(slotIt->second)});
            llvm::Value* fnPtr = builder_.CreateLoad(ptrTy(), slotPtr);
            return builder_.CreateCall(fnTy, fnPtr, args);
        }

        // Non-virtual (or base-qualified) → direct call to the function.
        auto it = methodFns_.find(ms);
        if (it == methodFns_.end()) return nullptr;
        llvm::Function* callee = it->second;
        if (args.size() != callee->arg_size()) return nullptr;
        return builder_.CreateCall(callee, args);
    }

    // CG18: the LLVM function type of an instance/static method — leading
    // `this` for instance methods, then the declared parameters. Used for
    // indirect (vtable) calls, including to abstract methods that have no
    // concrete Function to read the type from.
    llvm::FunctionType* methodFnType(const sema::MethodSymbol* ms) {
        auto* md = dynamic_cast<const ast::MethodDecl*>(ms->decl);
        if (!md) return nullptr;
        llvm::Type* retTy = lowerType(md->returnType.get());
        std::vector<llvm::Type*> paramTys;
        if (!isStaticMethod(const_cast<sema::MethodSymbol*>(ms)))
            paramTys.push_back(ptrTy());
        for (const auto& p : md->params) paramTys.push_back(lowerType(p.type.get()));
        return llvm::FunctionType::get(retTy, paramTys, /*isVarArg=*/false);
    }

    // CG4: the conditional expression `thenValue if condition else
    // elseValue` (§20). Lowers to a branch + phi so only the taken
    // branch's value expression is evaluated (lazy, per spec).
    llvm::Value* lowerConditional(const ast::ConditionalExpr& c) {
        if (!c.condition || !c.thenValue || !c.elseValue) return nullptr;
        llvm::Value* cond = lowerExpr(*c.condition);
        if (!cond) return nullptr;

        auto* thenBB = llvm::BasicBlock::Create(llvm_, "cond.then", currentFn_);
        auto* elseBB = llvm::BasicBlock::Create(llvm_, "cond.else", currentFn_);
        auto* endBB = llvm::BasicBlock::Create(llvm_, "cond.end", currentFn_);
        builder_.CreateCondBr(cond, thenBB, elseBB);

        builder_.SetInsertPoint(thenBB);
        llvm::Value* tv = lowerExpr(*c.thenValue);
        builder_.CreateBr(endBB);
        llvm::BasicBlock* thenEnd = builder_.GetInsertBlock();

        builder_.SetInsertPoint(elseBB);
        llvm::Value* ev = lowerExpr(*c.elseValue);
        builder_.CreateBr(endBB);
        llvm::BasicBlock* elseEnd = builder_.GetInsertBlock();

        builder_.SetInsertPoint(endBB);
        if (!tv || !ev) return nullptr;
        llvm::PHINode* phi = builder_.CreatePHI(tv->getType(), 2);
        phi->addIncoming(tv, thenEnd);
        phi->addIncoming(ev, elseEnd);
        return phi;
    }
};

}  // namespace

std::unique_ptr<llvm::Module> emitModule(const ast::SourceFile& src,
                                         sema::SemaContext& ctx,
                                         llvm::LLVMContext& llvmCtx) {
    // Codegen works off the SemaContext symbol table (each symbol points at
    // its AST decl), so the SourceFile root isn't needed directly; the
    // parameter is kept for API stability / future use.
    (void)src;
    auto m = Emitter(ctx, llvmCtx).run();
    if (m && std::getenv("BEZEL_DUMP_IR")) m->print(llvm::errs(), nullptr);
    return m;
}

bool emitObjectFile(llvm::Module& module, const std::string& objectPath,
                    std::string& error) {
    static const bool inited = [] {
        llvm::InitializeNativeTarget();
        llvm::InitializeNativeTargetAsmPrinter();
        return true;
    }();
    (void)inited;

    llvm::Triple triple(llvm::sys::getDefaultTargetTriple());
    const llvm::Target* target =
        llvm::TargetRegistry::lookupTarget(triple.str(), error);
    if (!target) return false;

    llvm::TargetOptions opts;
    std::unique_ptr<llvm::TargetMachine> tm(target->createTargetMachine(
        triple, "generic", "", opts, llvm::Reloc::PIC_));
    if (!tm) {
        error = "could not create target machine";
        return false;
    }

    module.setTargetTriple(triple);
    module.setDataLayout(tm->createDataLayout());

    std::error_code ec;
    llvm::raw_fd_ostream os(objectPath, ec, llvm::sys::fs::OF_None);
    if (ec) {
        error = "could not open '" + objectPath + "': " + ec.message();
        return false;
    }

    llvm::legacy::PassManager pm;
    if (tm->addPassesToEmitFile(pm, os, nullptr,
                                llvm::CodeGenFileType::ObjectFile)) {
        error = "target cannot emit an object file";
        return false;
    }
    pm.run(module);
    os.flush();
    return true;
}

}  // namespace bezel::codegen
