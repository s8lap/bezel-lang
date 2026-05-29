#include "codegen/LlvmBackend.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "ast/Node.hpp"
#include "sema/SemaContext.hpp"
#include "sema/Symbol.hpp"

#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
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
        declareFunctions();
        for (auto& [sym, fn] : methodFns_) {
            emitFunctionBody(sym, fn);
        }
        for (auto& [sym, fn] : ctorFns_) {
            emitConstructorBody(sym, fn);
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

    // CG8: per-class instance layout (struct of instance fields) and each
    // field's index within it.
    std::unordered_map<const sema::TypeSymbol*, llvm::StructType*> structs_;
    std::unordered_map<const sema::Symbol*, unsigned> fieldIndex_;

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

    // CG8: the LLVM struct for a class's instance fields, built lazily.
    // Fields are laid out in declaration order; their indices are
    // recorded for member access. Static fields and non-field members
    // are skipped.
    llvm::StructType* classStruct(const sema::TypeSymbol* t) {
        auto it = structs_.find(t);
        if (it != structs_.end()) return it->second;
        auto* st = llvm::StructType::create(
            llvm_, std::string("class.") + std::string(t->name));
        structs_[t] = st;  // record before filling (supports self-reference)
        std::vector<llvm::Type*> fieldTys;
        unsigned idx = 0;
        for (const sema::Symbol* m : t->members) {
            auto* fs = dynamic_cast<const sema::FieldSymbol*>(m);
            if (!fs) continue;
            bool isStatic = false;
            for (auto mod : fs->modifiers) {
                if (mod == "static") { isStatic = true; break; }
            }
            if (isStatic) continue;
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
    }

    // Pass 2: emit a function's body.
    void emitFunctionBody(sema::MethodSymbol* ms, llvm::Function* fn) {
        auto* md = dynamic_cast<const ast::MethodDecl*>(ms->decl);
        if (!md) return;
        currentFn_ = fn;
        currentThis_ = nullptr;
        locals_.clear();
        paramSlots_.clear();
        builder_.SetInsertPoint(llvm::BasicBlock::Create(llvm_, "entry", fn));

        const std::vector<sema::Symbol*>* paramSyms = paramSymbolsOf(md);
        bindParameters(fn, /*hasThis=*/!isStaticMethod(ms), paramSyms);

        bool terminated = false;
        if (md->bodyKind == ast::MethodBodyKind::Expression && md->exprBody) {
            llvm::Value* v = lowerExpr(*md->exprBody);
            terminated = emitReturn(fn, v);
        } else if (md->bodyKind == ast::MethodBodyKind::Block) {
            terminated = lowerBody(md->stmts);
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
        builder_.SetInsertPoint(llvm::BasicBlock::Create(llvm_, "entry", fn));
        bindParameters(fn, /*hasThis=*/true, &cs->params);
        bool terminated = lowerBody(cd->body);
        if (!terminated) builder_.CreateRetVoid();
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
            llvm::Value* v = rs->value ? lowerExpr(*rs->value) : nullptr;
            builder_.CreateRet(v ? v : llvm::ConstantInt::get(i32(), 0));
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
            if (!lowerBody(br->body)) {
                allPathsTerminate = false;
                builder_.CreateBr(merge());
            }
            builder_.SetInsertPoint(nextBB);  // next test / else goes here
        }

        // The final fall-through block is the else (or an empty pass-through).
        if (ifs.hasElse) {
            if (!lowerBody(ifs.elseBody)) {
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

    // CG5: while loop. cond block tests, body block loops back. break /
    // continue are not yet lowered (deferred; no test uses them).
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
        if (!lowerBody(ws.body)) builder_.CreateBr(condBB);  // back-edge

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

        // Run field initializers (calloc already zeroed the rest).
        for (const sema::Symbol* m : t->members) {
            auto* fs = dynamic_cast<const sema::FieldSymbol*>(m);
            if (!fs) continue;
            auto* fd = dynamic_cast<const ast::FieldDecl*>(fs->decl);
            if (!fd || !fd->initializer) continue;
            auto idxIt = fieldIndex_.find(fs);
            if (idxIt == fieldIndex_.end()) continue;  // static field
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
        if (sema::ConstructorSymbol* cs = soleConstructor(t)) {
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
        }
        return obj;
    }

    // CG10: the type's single non-static constructor, or null if it has
    // none (implicit default) or more than one (overloaded — deferred).
    sema::ConstructorSymbol* soleConstructor(const sema::TypeSymbol* t) {
        sema::ConstructorSymbol* found = nullptr;
        for (sema::Symbol* m : t->members) {
            if (auto* cs = dynamic_cast<sema::ConstructorSymbol*>(m)) {
                if (cs->isStatic) continue;
                if (found) return nullptr;  // overloaded — defer
                found = cs;
            }
        }
        return found;
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
        auto it = methodFns_.find(ms);
        if (it == methodFns_.end()) return nullptr;
        llvm::Function* callee = it->second;

        std::vector<llvm::Value*> args;
        if (!isStaticMethod(ms)) {
            // Receiver: the member-access target, else the current `this`.
            llvm::Value* recv = nullptr;
            if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(
                    call.callee.get())) {
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
        if (args.size() != callee->arg_size()) return nullptr;
        return builder_.CreateCall(callee, args);
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
    return Emitter(ctx, llvmCtx).run();
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
