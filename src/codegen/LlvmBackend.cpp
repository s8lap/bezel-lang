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
#include "llvm/IR/Intrinsics.h"
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

// CG32: decode a char literal lexeme (`'A'`, `'\n'`, …) to its code point.
// The lexer already validated the form; quotes are stripped and a leading
// backslash selects a simple escape. Multi-byte UTF-8 yields its first
// byte (full code-point decoding is deferred with string support).
std::int64_t parseCharLiteral(std::string_view text) {
    if (text.size() >= 2 && text.front() == '\'') text.remove_prefix(1);
    if (!text.empty() && text.back() == '\'') text.remove_suffix(1);
    if (text.empty()) return 0;
    if (text[0] != '\\') return static_cast<unsigned char>(text[0]);
    if (text.size() < 2) return 0;
    switch (text[1]) {
        case 'n': return '\n';
        case 't': return '\t';
        case 'r': return '\r';
        case '0': return '\0';
        case '\\': return '\\';
        case '\'': return '\'';
        case '"': return '"';
        case 'b': return '\b';
        case 'f': return '\f';
        case 'v': return '\v';
        default: return static_cast<unsigned char>(text[1]);
    }
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
        setupBuiltinExceptions();  // CG22
        computeEnumValues();  // CG30
        declareExterns();  // CG26
        for (auto& [sym, fn] : methodFns_) {
            emitFunctionBody(sym, fn);
        }
        for (auto& [sym, fn] : ctorFns_) {
            emitConstructorBody(sym, fn);
        }
        for (auto& [cs, fn] : staticCtors_) {
            emitStaticConstructorBody(cs, fn);
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
    // CG50: a `ref`/`out` parameter — the incoming argument is a pointer to
    // the caller's storage (not a fresh by-value slot), so reads/writes of
    // the parameter go through that pointer. Maps the ParameterSymbol to
    // {address, element type}.
    std::unordered_map<const sema::Symbol*, std::pair<llvm::Value*, llvm::Type*>>
        refParams_;
    // CG6: emitted LLVM function for each static method symbol.
    std::unordered_map<sema::MethodSymbol*, llvm::Function*> methodFns_;
    // CG10: emitted LLVM function for each (non-static) constructor symbol.
    std::unordered_map<sema::ConstructorSymbol*, llvm::Function*> ctorFns_;
    // CG47: static constructors (§22.7) — `void()` functions run once before
    // `main` (in declaration order). Kept ordered (not a map) for a stable
    // run order.
    std::vector<std::pair<sema::ConstructorSymbol*, llvm::Function*>>
        staticCtors_;

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

    // CG20: exception handling (explicit-unwinding model). A module-level
    // global holds the in-flight exception (null when none). A `throw` and
    // any call that leaves the global non-null branch to the current
    // unwind target — the enclosing try's catch dispatch, or (when none)
    // the function's propagate block, which returns so the caller's
    // post-call check unwinds further.
    llvm::GlobalVariable* excGlobal_ = nullptr;
    llvm::BasicBlock* unwindTarget_ = nullptr;   // innermost catch dispatch
    llvm::BasicBlock* propagateBB_ = nullptr;    // per-function, lazy
    // CG20: the exception value caught by the enclosing catch arm, so a
    // bare `throw;` can re-raise it (the global was cleared on arm entry).
    llvm::Value* currentCaughtExc_ = nullptr;
    // CG40: set true by lowering a `throw` expression (§31.2) — the
    // expression diverges (branches to the unwind target), leaving the
    // current block terminated. Phi-building consumers (conditional, `??`)
    // read this immediately after lowering a sub-expression to exclude a
    // diverged branch from the join.
    bool diverged_ = false;
    // CG21: the disposeStack_ depth to unwind to when branching to
    // unwindTarget_ — using-var scopes from here up are disposed as an
    // exception propagates out of them (§28.3). 0 = dispose all (the
    // function propagate path).
    std::size_t unwindScopeDepth_ = 0;
    // CG22: per-built-in-exception-type descriptor and vtable globals
    // (§31.4 hierarchy), keyed by type name (built-in exception types have
    // no user TypeSymbol). A shared no-op Dispose serves all of them.
    std::unordered_map<std::string, llvm::GlobalVariable*> builtinDesc_;
    std::unordered_map<std::string, llvm::GlobalVariable*> builtinVtable_;
    llvm::Function* builtinDisposeFn_ = nullptr;

    // CG30: computed integer value of each enum constant (§10).
    std::unordered_map<const sema::Symbol*, std::int64_t> enumValue_;

    // CG26: external (FFI) function declared by an extern block (§32).
    std::unordered_map<const sema::ExternDeclSymbol*, llvm::Function*>
        externFns_;

    // CG27a: string runtime — %bz_string = { ptr, i64 } (data, length).
    // Literals are materialized as private global constants; concatenation
    // allocates via malloc/memcpy. All string variables are ptr-typed (§4.6).
    llvm::StructType* stringStructTy_ = nullptr;
    unsigned stringLiteralIdx_ = 0;

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
    // CG35: interface method dispatch. Every interface method gets a fixed
    // vtable slot in [2, 2+G); each implementing class fills that slot with
    // its implementation, so a call through an interface-typed value indexes
    // the runtime object's vtable at the interface method's slot. Class
    // virtual-method slots follow at [2+G, …).
    std::unordered_map<const sema::MethodSymbol*, unsigned> ifaceMethodSlot_;
    unsigned ifaceSlotCount_ = 0;
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
    // CG22: zero / signed-minimum constants matching an integer value's type.
    llvm::Constant* zeroLike(llvm::Value* v) {
        return llvm::ConstantInt::get(v->getType(), 0);
    }
    llvm::Constant* minIntLike(llvm::Value* v) {
        unsigned bits = v->getType()->getIntegerBitWidth();
        return llvm::ConstantInt::get(
            v->getContext(), llvm::APInt::getSignedMinValue(bits));
    }

    // Create an alloca in the function's entry block (the conventional
    // home for locals, so they dominate all uses once control flow lands).
    llvm::AllocaInst* entryAlloca(llvm::Type* ty) {
        llvm::BasicBlock& entry = currentFn_->getEntryBlock();
        llvm::IRBuilder<> tmp(&entry, entry.begin());
        return tmp.CreateAlloca(ty);
    }

    // CG50: a `ref` / `out` parameter is passed by reference (its LLVM type
    // is a pointer to the value type, and the caller passes an address).
    static bool isByRefMod(std::string_view m) {
        return m == "ref" || m == "out" || m == "out var" ||
               m == "out owned" || m == "out using var";
    }

    // Is this member symbol a static method with a lowerable body?
    static bool isStaticMethod(sema::MethodSymbol* ms) {
        if (ms->isOperator) return true;  // §25: operators are always static
        for (auto mod : ms->modifiers) {
            if (mod == "static") return true;
        }
        return false;
    }

    // §25: the callable's parameters / return / body, abstracting over a
    // MethodDecl vs an OperatorDecl (an operator overload's symbol points at
    // an OperatorDecl, which has the same shape).
    struct CallableInfo {
        const std::vector<ast::Param>* params = nullptr;
        const ast::Type* returnType = nullptr;
        ast::MethodBodyKind bodyKind = ast::MethodBodyKind::Abstract;
        const ast::Expr* exprBody = nullptr;
        const std::vector<std::unique_ptr<ast::Stmt>>* stmts = nullptr;
    };
    static bool callableInfo(const sema::MethodSymbol* ms, CallableInfo& out) {
        if (auto* md = dynamic_cast<const ast::MethodDecl*>(ms->decl)) {
            out = {&md->params, md->returnType.get(), md->bodyKind,
                   md->exprBody.get(), &md->stmts};
            return true;
        }
        if (auto* od = dynamic_cast<const ast::OperatorDecl*>(ms->decl)) {
            out = {&od->params, od->returnType.get(), od->bodyKind,
                   od->exprBody.get(), &od->stmts};
            return true;
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

    // CG24: a primitive type's LLVM representation. Bool→i1, Float→f32,
    // Double→f64; the integer types are all i32 until CG25 widens them.
    llvm::Type* lowerPrimitive(sema::PrimitiveType p) {
        using P = sema::PrimitiveType;
        switch (p) {
            case P::Bool:   return i1();
            case P::Float:  return llvm::Type::getFloatTy(llvm_);
            case P::Double: return llvm::Type::getDoubleTy(llvm_);
            // CG25: integer widths (§5). Char is a 32-bit code point.
            case P::Byte: case P::SByte:   return llvm::Type::getInt8Ty(llvm_);
            case P::Short: case P::UShort: return llvm::Type::getInt16Ty(llvm_);
            case P::Long: case P::ULong:   return i64();
            case P::Int: case P::UInt: case P::Char:
                                           return i32();
            // CG27a (§4.6): string is a reference type — pointer to bz_string.
            case P::String:                return ptrTy();
            default:                       return i32();
        }
    }

    // CG27a: lazily get (or create) the %bz_string struct type { ptr, i64 }.
    llvm::StructType* stringType() {
        if (!stringStructTy_)
            stringStructTy_ = llvm::StructType::create(
                llvm_, {ptrTy(), i64()}, "bz.string");
        return stringStructTy_;
    }

    // CG27a: load the data pointer from a bz_string pointer.
    llvm::Value* loadStringData(llvm::Value* strPtr) {
        llvm::Value* p = builder_.CreateStructGEP(stringType(), strPtr, 0);
        return builder_.CreateLoad(ptrTy(), p, "str.data");
    }

    // CG27a: load the length (i64) from a bz_string pointer.
    llvm::Value* loadStringLen(llvm::Value* strPtr) {
        llvm::Value* p = builder_.CreateStructGEP(stringType(), strPtr, 1);
        return builder_.CreateLoad(i64(), p, "str.len");
    }

    // CG27a: lazily declare an external C function (for memcmp, malloc, memcpy).
    llvm::Function* declareC(const char* name, llvm::Type* ret,
                             std::vector<llvm::Type*> args) {
        if (auto* f = module_->getFunction(name)) return f;
        auto* ft = llvm::FunctionType::get(ret, args, /*variadic=*/false);
        return llvm::Function::Create(
            ft, llvm::Function::ExternalLinkage, name, module_.get());
    }

    // CG27a (§4.6): process string literal source text into raw bytes,
    // handling escape sequences (§18.8). `text` is the full source-level
    // token including surrounding `"` (or `@"` for verbatim). Returns the
    // raw byte content (not null-terminated; null added by LLVM DataArray).
    static std::string processStringText(std::string_view text, bool verbatim) {
        // Strip prefix (@" or ") and trailing ".
        size_t start = verbatim ? 2 : 1;
        if (text.size() < (verbatim ? 3u : 2u)) return {};
        text = text.substr(start, text.size() - start - 1);

        std::string result;
        result.reserve(text.size());
        for (size_t i = 0; i < text.size(); ++i) {
            if (verbatim) {
                // Verbatim: only `""` → `"`, everything else literal.
                if (text[i] == '"' && i + 1 < text.size() && text[i + 1] == '"') {
                    result.push_back('"');
                    ++i;
                } else {
                    result.push_back(text[i]);
                }
                continue;
            }
            if (text[i] != '\\') { result.push_back(text[i]); continue; }
            if (++i >= text.size()) break;
            switch (text[i]) {
                case 'n': result.push_back('\n'); break;
                case 'r': result.push_back('\r'); break;
                case 't': result.push_back('\t'); break;
                case '\\': result.push_back('\\'); break;
                case '"': result.push_back('"'); break;
                case '\'': result.push_back('\''); break;
                case '0': result.push_back('\0'); break;
                case 'a': result.push_back('\a'); break;
                case 'b': result.push_back('\b'); break;
                case 'f': result.push_back('\f'); break;
                case 'v': result.push_back('\v'); break;
                case 'u': {
                    // \uXXXX — 4 hex digits, BMP only → UTF-8
                    if (i + 4 < text.size()) {
                        uint32_t cp = 0;
                        for (int k = 0; k < 4; ++k)
                            cp = (cp << 4) | (uint8_t)
                                (text[i + 1 + k] >= 'a' ? text[i+1+k]-'a'+10 :
                                 text[i+1+k] >= 'A' ? text[i+1+k]-'A'+10 :
                                 text[i+1+k] - '0');
                        i += 4;
                        if (cp < 0x80) {
                            result.push_back((char)cp);
                        } else if (cp < 0x800) {
                            result.push_back((char)(0xC0 | (cp >> 6)));
                            result.push_back((char)(0x80 | (cp & 0x3F)));
                        } else {
                            result.push_back((char)(0xE0 | (cp >> 12)));
                            result.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
                            result.push_back((char)(0x80 | (cp & 0x3F)));
                        }
                    }
                    break;
                }
                default: result.push_back(text[i]); break;
            }
        }
        return result;
    }

    // CG27a (§4.6): materialize a string literal as a private global
    // %bz_string constant; return a ptr to the global.
    llvm::Value* lowerStringLiteral(std::string_view text, bool verbatim) {
        std::string content = processStringText(text, verbatim);
        // Global [N+1 x i8] constant for the char data (null-terminated).
        llvm::Constant* dataArr =
            llvm::ConstantDataArray::getString(llvm_, content, /*AddNull=*/true);
        unsigned id = stringLiteralIdx_++;
        auto* dataGV = new llvm::GlobalVariable(
            *module_, dataArr->getType(), /*isConstant=*/true,
            llvm::GlobalValue::PrivateLinkage, dataArr,
            ".bz.str.data." + std::to_string(id));
        dataGV->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
        dataGV->setAlignment(llvm::Align(1));
        // Global %bz_string = { ptr data, i64 len }.
        llvm::Constant* lenC = llvm::ConstantInt::get(i64(), content.size());
        llvm::Constant* strVal = llvm::ConstantStruct::get(
            stringType(), {dataGV, lenC});
        auto* strGV = new llvm::GlobalVariable(
            *module_, stringType(), /*isConstant=*/true,
            llvm::GlobalValue::PrivateLinkage, strVal,
            ".bz.str." + std::to_string(id));
        return strGV;
    }

    // CG27a (§4.6): content equality of two bz_string pointers → i1.
    // Fast path: pointer identity → true. Slow path: compare lengths then
    // memcmp the char data.
    llvm::Value* emitStringEq(llvm::Value* l, llvm::Value* r) {
        auto* entryBB = builder_.GetInsertBlock();
        auto* lenBB = llvm::BasicBlock::Create(llvm_, "str.eq.len", currentFn_);
        auto* cmpBB = llvm::BasicBlock::Create(llvm_, "str.eq.cmp", currentFn_);
        auto* endBB = llvm::BasicBlock::Create(llvm_, "str.eq.end", currentFn_);
        // Fast path: same pointer → equal.
        llvm::Value* samePtr = builder_.CreateICmpEQ(l, r, "str.sameptr");
        builder_.CreateCondBr(samePtr, endBB, lenBB);
        // Length check.
        builder_.SetInsertPoint(lenBB);
        llvm::Value* lenL = loadStringLen(l);
        llvm::Value* lenR = loadStringLen(r);
        llvm::Value* sameLen = builder_.CreateICmpEQ(lenL, lenR, "str.samelen");
        auto* lenEnd = builder_.GetInsertBlock();
        builder_.CreateCondBr(sameLen, cmpBB, endBB);
        // memcmp.
        builder_.SetInsertPoint(cmpBB);
        llvm::Value* dataL = loadStringData(l);
        llvm::Value* dataR = loadStringData(r);
        auto* memcmpFn = declareC("memcmp", i32(), {ptrTy(), ptrTy(), i64()});
        llvm::Value* cmpRes = builder_.CreateCall(memcmpFn, {dataL, dataR, lenL});
        llvm::Value* cmpEq = builder_.CreateICmpEQ(
            cmpRes, llvm::ConstantInt::get(i32(), 0), "str.cmpeq");
        auto* cmpEnd = builder_.GetInsertBlock();
        builder_.CreateBr(endBB);
        // Merge.
        builder_.SetInsertPoint(endBB);
        auto* phi = builder_.CreatePHI(i1(), 3, "str.eq");
        phi->addIncoming(builder_.getTrue(), entryBB);
        phi->addIncoming(builder_.getFalse(), lenEnd);
        phi->addIncoming(cmpEq, cmpEnd);
        return phi;
    }

    // CG27a (§4.6): string concatenation — malloc-backed new string object.
    llvm::Value* emitStringConcat(llvm::Value* l, llvm::Value* r) {
        llvm::Value* lenL = loadStringLen(l);
        llvm::Value* lenR = loadStringLen(r);
        llvm::Value* total = builder_.CreateAdd(lenL, lenR, "str.total");
        auto* i8Ty = llvm::Type::getInt8Ty(llvm_);
        auto* mallocFn = declareC("malloc", ptrTy(), {i64()});
        auto* memcpyFn = declareC("memcpy", ptrTy(), {ptrTy(), ptrTy(), i64()});
        // Allocate data buffer: total+1 (null-terminator).
        llvm::Value* dataSize = builder_.CreateAdd(
            total, builder_.getInt64(1), "str.datasize");
        llvm::Value* newData = builder_.CreateCall(mallocFn, {dataSize}, "str.data");
        llvm::Value* dataL = loadStringData(l);
        llvm::Value* dataR = loadStringData(r);
        builder_.CreateCall(memcpyFn, {newData, dataL, lenL});
        llvm::Value* midPtr = builder_.CreateGEP(i8Ty, newData, lenL, "str.mid");
        builder_.CreateCall(memcpyFn, {midPtr, dataR, lenR});
        llvm::Value* endPtr = builder_.CreateGEP(i8Ty, newData, total, "str.nul");
        builder_.CreateStore(llvm::ConstantInt::get(i8Ty, 0), endPtr);
        // Allocate the bz_string struct.
        uint64_t strBytes = module_->getDataLayout().getTypeAllocSize(stringType());
        llvm::Value* newStr = builder_.CreateCall(
            mallocFn, {builder_.getInt64(strBytes)}, "str.obj");
        llvm::Value* df = builder_.CreateStructGEP(stringType(), newStr, 0);
        builder_.CreateStore(newData, df);
        llvm::Value* lf = builder_.CreateStructGEP(stringType(), newStr, 1);
        builder_.CreateStore(total, lf);
        return newStr;
    }

    // CG27c (§5): emit `.ToString()` for a Bezel primitive value.
    // Uses snprintf to format into a stack buffer, then malloc a bz_string.
    // The format specifier depends on the primitive type.
    llvm::Value* emitPrimitiveToString(llvm::Value* val,
                                       sema::PrimitiveType prim) {
        using P = sema::PrimitiveType;
        auto* i8Ty = llvm::Type::getInt8Ty(llvm_);
        auto* mallocFn = declareC("malloc", ptrTy(), {i64()});

        // Special case: bool → "true" or "false"
        if (prim == P::Bool) {
            // lowerStringLiteral materializes constant globals.
            auto* trueStr  = lowerStringLiteral("\"true\"", false);
            auto* falseStr = lowerStringLiteral("\"false\"", false);
            return builder_.CreateSelect(
                builder_.CreateTrunc(val, i1()), trueStr, falseStr,
                "bool.str");
        }

        // Special case: string.ToString() → identity (return itself).
        if (prim == P::String) return val;

        // Numeric types: snprintf into a stack buffer, then heap-copy.
        // Buffer of 64 bytes is ample for any numeric type.
        const unsigned BUF = 64;
        llvm::Value* buf = entryAlloca(
            llvm::ArrayType::get(i8Ty, BUF));

        // Declare snprintf: int snprintf(char*, size_t, const char*, ...)
        auto* snprintfFn = module_->getFunction("snprintf");
        if (!snprintfFn) {
            auto* ft = llvm::FunctionType::get(
                i32(), {ptrTy(), i64(), ptrTy()}, /*variadic=*/true);
            snprintfFn = llvm::Function::Create(
                ft, llvm::Function::ExternalLinkage, "snprintf", module_.get());
        }

        // Pick format string and possibly widen/convert val.
        const char* fmt = "%d";
        llvm::Value* printVal = val;
        switch (prim) {
            case P::Long: case P::ULong:
                fmt = prim == P::ULong ? "%llu" : "%lld";
                break;
            case P::UInt:
                fmt = "%u"; break;
            case P::Byte: case P::SByte:
            case P::Short: case P::UShort:
                // Widen to i32 for snprintf
                printVal = (prim == P::Byte || prim == P::UShort ||
                            prim == P::Short)
                    ? builder_.CreateSExt(val, i32())
                    : builder_.CreateZExt(val, i32());
                fmt = "%d"; break;
            case P::Float:
                printVal = builder_.CreateFPExt(val, llvm::Type::getDoubleTy(llvm_));
                fmt = "%g"; break;
            case P::Double:
                fmt = "%g"; break;
            case P::Char:
                // Char is i32 code point → print as character
                fmt = "%lc"; break;
            default:
                fmt = "%d"; break;
        }

        // Create a global constant for the format string.
        llvm::Constant* fmtData = llvm::ConstantDataArray::getString(
            llvm_, fmt, /*AddNull=*/true);
        unsigned fmtId = stringLiteralIdx_++;
        auto* fmtGV = new llvm::GlobalVariable(
            *module_, fmtData->getType(), true,
            llvm::GlobalValue::PrivateLinkage, fmtData,
            ".bz.fmt." + std::to_string(fmtId));
        fmtGV->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);

        llvm::Value* bufPtr = builder_.CreateBitCast(
            builder_.CreateGEP(
                llvm::ArrayType::get(i8Ty, BUF), buf,
                {builder_.getInt64(0), builder_.getInt64(0)}),
            ptrTy());

        // snprintf(bufPtr, BUF, fmt, printVal)
        llvm::Value* written = builder_.CreateCall(
            snprintfFn,
            {bufPtr, builder_.getInt64(BUF), fmtGV, printVal},
            "snprintf.ret");

        // Allocate heap storage for the data: written+1 bytes.
        llvm::Value* dataSize = builder_.CreateAdd(
            builder_.CreateSExt(written, i64()), builder_.getInt64(1),
            "str.ts.datasize");
        auto* memcpyFn = declareC("memcpy", ptrTy(), {ptrTy(), ptrTy(), i64()});
        llvm::Value* dataCopy = builder_.CreateCall(mallocFn, {dataSize}, "str.ts.data");
        builder_.CreateCall(memcpyFn, {dataCopy, bufPtr,
                                       builder_.CreateSExt(written, i64())});
        // Null-terminate.
        llvm::Value* endPtr = builder_.CreateGEP(
            i8Ty, dataCopy, builder_.CreateSExt(written, i64()));
        builder_.CreateStore(llvm::ConstantInt::get(i8Ty, 0), endPtr);

        // Build the bz_string object.
        uint64_t strBytes = module_->getDataLayout().getTypeAllocSize(stringType());
        llvm::Value* newStr = builder_.CreateCall(
            mallocFn, {builder_.getInt64(strBytes)}, "str.ts.obj");
        builder_.CreateStore(dataCopy,
            builder_.CreateStructGEP(stringType(), newStr, 0));
        builder_.CreateStore(builder_.CreateSExt(written, i64()),
            builder_.CreateStructGEP(stringType(), newStr, 1));
        return newStr;
    }

    // CG27b (§4.6): ordinal comparison of two bz_string pointers → i32.
    // Returns negative / 0 / positive (same sign contract as memcmp/strcmp).
    // Ordinal = code-point-by-code-point; shorter string is "less" when it
    // is a prefix of the longer one (per §4.6 length-aware comparison).
    llvm::Value* emitStringOrdCmp(llvm::Value* l, llvm::Value* r) {
        // Compare min(lenL,lenR) bytes with memcmp; if equal, compare lengths.
        llvm::Value* lenL = loadStringLen(l);
        llvm::Value* lenR = loadStringLen(r);
        llvm::Value* minLen = builder_.CreateSelect(
            builder_.CreateICmpSLT(lenL, lenR), lenL, lenR, "str.minlen");
        llvm::Value* dataL = loadStringData(l);
        llvm::Value* dataR = loadStringData(r);
        auto* memcmpFn = declareC("memcmp", i32(), {ptrTy(), ptrTy(), i64()});
        llvm::Value* cmpRes = builder_.CreateCall(memcmpFn, {dataL, dataR, minLen},
                                                  "str.cmp");
        // If memcmp returned 0, fall back to comparing lengths.
        auto* cmpZero = builder_.CreateICmpEQ(
            cmpRes, llvm::ConstantInt::get(i32(), 0), "str.cmpeq");
        llvm::Value* lenDiff = builder_.CreateSub(
            builder_.CreateTrunc(lenL, i32()),
            builder_.CreateTrunc(lenR, i32()), "str.lendiff");
        return builder_.CreateSelect(cmpZero, lenDiff, cmpRes, "str.ord");
    }

    // CG27f (stdlib §9.6): intercept stdlib string method calls.
    // Returns the lowered value, or null if the method is not recognised.
    llvm::Value* tryLowerStringMethod(const ast::CallExpr& call,
                                       const ast::MemberAccessExpr& ma) {
        const auto& method = ma.member;
        if (!ma.target) return nullptr;
        llvm::Value* self = lowerExpr(*ma.target);
        if (!self) return nullptr;

        // Helpers for extracting the first string argument.
        auto getStrArg = [&](size_t idx) -> llvm::Value* {
            if (idx >= call.args.size() || !call.args[idx].value) return nullptr;
            return lowerExpr(*call.args[idx].value);
        };
        auto getIntArg = [&](size_t idx) -> llvm::Value* {
            if (idx >= call.args.size() || !call.args[idx].value) return nullptr;
            return lowerExpr(*call.args[idx].value);
        };

        // §9.6: StartsWith(prefix: string): bool
        // prefix.len <= self.len AND memcmp(self.data, prefix.data, prefix.len)==0
        if (method == "StartsWith") {
            llvm::Value* prefix = getStrArg(0);
            if (!prefix) return nullptr;
            llvm::Value* selfLen = loadStringLen(self);
            llvm::Value* prefLen = loadStringLen(prefix);
            // if prefLen > selfLen → false
            llvm::Value* tooLong = builder_.CreateICmpUGT(prefLen, selfLen, "sw.toolong");
            auto* cmpBB = llvm::BasicBlock::Create(llvm_, "sw.cmp", currentFn_);
            auto* endBB = llvm::BasicBlock::Create(llvm_, "sw.end", currentFn_);
            auto* entryBB = builder_.GetInsertBlock();
            builder_.CreateCondBr(tooLong, endBB, cmpBB);
            builder_.SetInsertPoint(cmpBB);
            auto* memcmpFn = declareC("memcmp", i32(), {ptrTy(), ptrTy(), i64()});
            llvm::Value* cmpRes = builder_.CreateCall(
                memcmpFn, {loadStringData(self), loadStringData(prefix), prefLen});
            llvm::Value* eqZ = builder_.CreateICmpEQ(cmpRes, llvm::ConstantInt::get(i32(),0));
            auto* cmpEnd = builder_.GetInsertBlock();
            builder_.CreateBr(endBB);
            builder_.SetInsertPoint(endBB);
            auto* phi = builder_.CreatePHI(i1(), 2, "sw.result");
            phi->addIncoming(builder_.getFalse(), entryBB);
            phi->addIncoming(eqZ, cmpEnd);
            return phi;
        }

        // §9.6: EndsWith(suffix: string): bool
        if (method == "EndsWith") {
            llvm::Value* suffix = getStrArg(0);
            if (!suffix) return nullptr;
            llvm::Value* selfLen = loadStringLen(self);
            llvm::Value* sufLen = loadStringLen(suffix);
            llvm::Value* tooLong = builder_.CreateICmpUGT(sufLen, selfLen, "ew.toolong");
            auto* cmpBB = llvm::BasicBlock::Create(llvm_, "ew.cmp", currentFn_);
            auto* endBB = llvm::BasicBlock::Create(llvm_, "ew.end", currentFn_);
            auto* entryBB = builder_.GetInsertBlock();
            builder_.CreateCondBr(tooLong, endBB, cmpBB);
            builder_.SetInsertPoint(cmpBB);
            // offset = selfLen - sufLen; compare self.data+offset with suffix.data
            llvm::Value* offset = builder_.CreateSub(selfLen, sufLen, "ew.off");
            auto* i8Ty = llvm::Type::getInt8Ty(llvm_);
            llvm::Value* selfOff = builder_.CreateGEP(i8Ty, loadStringData(self), offset);
            auto* memcmpFn = declareC("memcmp", i32(), {ptrTy(), ptrTy(), i64()});
            llvm::Value* cmpRes = builder_.CreateCall(
                memcmpFn, {selfOff, loadStringData(suffix), sufLen});
            llvm::Value* eqZ = builder_.CreateICmpEQ(cmpRes, llvm::ConstantInt::get(i32(),0));
            auto* cmpEnd = builder_.GetInsertBlock();
            builder_.CreateBr(endBB);
            builder_.SetInsertPoint(endBB);
            auto* phi = builder_.CreatePHI(i1(), 2, "ew.result");
            phi->addIncoming(builder_.getFalse(), entryBB);
            phi->addIncoming(eqZ, cmpEnd);
            return phi;
        }

        // §9.6: Contains(sub: string): bool — uses libc memmem (POSIX).
        if (method == "Contains") {
            llvm::Value* sub = getStrArg(0);
            if (!sub) return nullptr;
            // Declare memmem: void* memmem(void*, size_t, void*, size_t)
            auto* memmemFn = module_->getFunction("memmem");
            if (!memmemFn) {
                auto* ft = llvm::FunctionType::get(
                    ptrTy(), {ptrTy(), i64(), ptrTy(), i64()}, false);
                memmemFn = llvm::Function::Create(
                    ft, llvm::Function::ExternalLinkage, "memmem", module_.get());
            }
            llvm::Value* found = builder_.CreateCall(memmemFn, {
                loadStringData(self), loadStringLen(self),
                loadStringData(sub),  loadStringLen(sub)});
            return builder_.CreateICmpNE(
                found, llvm::ConstantPointerNull::get(ptrTy()), "contains");
        }

        // §9.6: IndexOf(sub: string): int — first occurrence, -1 if not found.
        if (method == "IndexOf") {
            llvm::Value* sub = getStrArg(0);
            if (!sub) return nullptr;
            auto* memmemFn = module_->getFunction("memmem");
            if (!memmemFn) {
                auto* ft = llvm::FunctionType::get(
                    ptrTy(), {ptrTy(), i64(), ptrTy(), i64()}, false);
                memmemFn = llvm::Function::Create(
                    ft, llvm::Function::ExternalLinkage, "memmem", module_.get());
            }
            llvm::Value* selfData = loadStringData(self);
            llvm::Value* found = builder_.CreateCall(memmemFn, {
                selfData, loadStringLen(self),
                loadStringData(sub), loadStringLen(sub)});
            llvm::Value* isNull = builder_.CreateICmpEQ(
                found, llvm::ConstantPointerNull::get(ptrTy()), "iof.null");
            // If found: index = (found ptr) - (selfData ptr), cast to i32.
            llvm::Value* diff = builder_.CreatePtrDiff(
                llvm::Type::getInt8Ty(llvm_), found, selfData, "iof.diff");
            llvm::Value* idx = builder_.CreateTrunc(diff, i32(), "iof.idx");
            return builder_.CreateSelect(isNull,
                llvm::ConstantInt::getSigned(i32(), -1), idx, "iof.result");
        }

        // §9.6: LastIndexOf(sub: string): int — last occurrence, -1 if not found.
        // Implemented by scanning from the end using reverse memmem search loop.
        if (method == "LastIndexOf") {
            llvm::Value* sub = getStrArg(0);
            if (!sub) return nullptr;
            auto* i8Ty = llvm::Type::getInt8Ty(llvm_);
            llvm::Value* selfLen  = loadStringLen(self);
            llvm::Value* selfData = loadStringData(self);
            llvm::Value* subLen   = loadStringLen(sub);
            llvm::Value* subData  = loadStringData(sub);
            auto* memmemFn = module_->getFunction("memmem");
            if (!memmemFn) {
                auto* ft = llvm::FunctionType::get(ptrTy(), {ptrTy(), i64(), ptrTy(), i64()}, false);
                memmemFn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "memmem", module_.get());
            }
            // Scan from offset 0..selfLen-subLen, track last match.
            auto* lastSlot = entryAlloca(i64());
            builder_.CreateStore(builder_.getInt64(-1), lastSlot);
            auto* posSlot = entryAlloca(i64());
            builder_.CreateStore(builder_.getInt64(0), posSlot);
            auto* condBB = llvm::BasicBlock::Create(llvm_, "liof.cond", currentFn_);
            auto* bodyBB = llvm::BasicBlock::Create(llvm_, "liof.body", currentFn_);
            auto* endBB  = llvm::BasicBlock::Create(llvm_, "liof.end",  currentFn_);
            builder_.CreateBr(condBB);
            builder_.SetInsertPoint(condBB);
            llvm::Value* pos = builder_.CreateLoad(i64(), posSlot);
            llvm::Value* rem = builder_.CreateSub(selfLen, pos, "liof.rem");
            llvm::Value* hasSub = builder_.CreateICmpSGE(rem, subLen, "liof.hasub");
            builder_.CreateCondBr(hasSub, bodyBB, endBB);
            builder_.SetInsertPoint(bodyBB);
            llvm::Value* posB = builder_.CreateLoad(i64(), posSlot);
            llvm::Value* remB = builder_.CreateSub(selfLen, posB, "liof.remb");
            llvm::Value* dataOff = builder_.CreateGEP(i8Ty, selfData, posB, "liof.off");
            llvm::Value* found = builder_.CreateCall(memmemFn, {dataOff, remB, subData, subLen}, "liof.found");
            auto* nullPtr = llvm::ConstantPointerNull::get(ptrTy());
            llvm::Value* notFound = builder_.CreateICmpEQ(found, nullPtr);
            auto* updateBB = llvm::BasicBlock::Create(llvm_, "liof.upd", currentFn_);
            builder_.CreateCondBr(notFound, endBB, updateBB);
            builder_.SetInsertPoint(updateBB);
            llvm::Value* matchOff = builder_.CreatePtrDiff(i8Ty, found, selfData, "liof.moff");
            builder_.CreateStore(matchOff, lastSlot);
            // Advance past this match.
            llvm::Value* newPos = builder_.CreateAdd(matchOff, subLen, "liof.npos");
            builder_.CreateStore(newPos, posSlot);
            builder_.CreateBr(condBB);
            builder_.SetInsertPoint(endBB);
            llvm::Value* lastI64 = builder_.CreateLoad(i64(), lastSlot);
            return builder_.CreateTrunc(lastI64, i32(), "liof.result");
        }

        // §9.6: Substring(start) → new string from start to end.
        // §9.6: Substring(start, length) → new string of given length.
        if (method == "Substring") {
            llvm::Value* startV = getIntArg(0);
            if (!startV) return nullptr;
            llvm::Value* selfLen = loadStringLen(self);
            llvm::Value* start64 = builder_.CreateSExt(startV, i64(), "sub.start");
            llvm::Value* subLen = (call.args.size() >= 2 && call.args[1].value)
                ? builder_.CreateSExt(lowerExpr(*call.args[1].value), i64(), "sub.len")
                : builder_.CreateSub(selfLen, start64, "sub.len");
            // Clamp to avoid over-reading.
            llvm::Value* maxLen = builder_.CreateSub(selfLen, start64, "sub.maxlen");
            llvm::Value* tooLong = builder_.CreateICmpSGT(subLen, maxLen, "sub.toolong");
            subLen = builder_.CreateSelect(tooLong, maxLen, subLen, "sub.clamped");
            // Allocate new string data + object.
            auto* i8Ty = llvm::Type::getInt8Ty(llvm_);
            auto* mallocFn = declareC("malloc", ptrTy(), {i64()});
            auto* memcpyFn = declareC("memcpy", ptrTy(), {ptrTy(), ptrTy(), i64()});
            llvm::Value* dataSize = builder_.CreateAdd(subLen, builder_.getInt64(1));
            llvm::Value* newData = builder_.CreateCall(mallocFn, {dataSize}, "sub.data");
            llvm::Value* srcPtr = builder_.CreateGEP(
                i8Ty, loadStringData(self), start64, "sub.src");
            builder_.CreateCall(memcpyFn, {newData, srcPtr, subLen});
            llvm::Value* endPtr = builder_.CreateGEP(i8Ty, newData, subLen);
            builder_.CreateStore(llvm::ConstantInt::get(i8Ty, 0), endPtr);
            uint64_t strBytes = module_->getDataLayout().getTypeAllocSize(stringType());
            llvm::Value* newStr = builder_.CreateCall(
                mallocFn, {builder_.getInt64(strBytes)}, "sub.obj");
            builder_.CreateStore(newData, builder_.CreateStructGEP(stringType(), newStr, 0));
            builder_.CreateStore(subLen, builder_.CreateStructGEP(stringType(), newStr, 1));
            return newStr;
        }

        // §9.6: Replace(oldStr, newStr): string — replaces all occurrences of
        // `oldStr` with `newStr` in `self`. Uses memmem for finding occurrences;
        // builds the result via repeated concatenation in a loop.
        if (method == "Replace" && call.args.size() == 2 &&
            call.args[0].value && call.args[1].value) {
            llvm::Value* oldStr = lowerExpr(*call.args[0].value);
            llvm::Value* newStr = lowerExpr(*call.args[1].value);
            if (!oldStr || !newStr) return nullptr;
            auto* i8Ty = llvm::Type::getInt8Ty(llvm_);
            auto* mallocFn = declareC("malloc", ptrTy(), {i64()});
            auto* memcpyFn = declareC("memcpy", ptrTy(), {ptrTy(), ptrTy(), i64()});
            // Declare memmem.
            auto* memmemFn = module_->getFunction("memmem");
            if (!memmemFn) {
                auto* ft = llvm::FunctionType::get(ptrTy(), {ptrTy(), i64(), ptrTy(), i64()}, false);
                memmemFn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "memmem", module_.get());
            }
            // Walk the input, collecting prefix+new segments.
            // For simplicity emit a result built from:
            //   acc = "" ; pos = self.data ; end = self.data + self.len
            //   while (found = memmem(pos, end-pos, old.data, old.len)):
            //     acc = acc + str(pos..found) + newStr
            //     pos = found + old.len
            //   acc = acc + str(pos..end)
            // All in LLVM IR using a loop.
            llvm::Value* selfLen  = loadStringLen(self);
            llvm::Value* selfData = loadStringData(self);
            llvm::Value* oldData  = loadStringData(oldStr);
            llvm::Value* oldLen   = loadStringLen(oldStr);
            llvm::Value* newData2 = loadStringData(newStr);
            llvm::Value* newLen   = loadStringLen(newStr);
            // Allocate result buffer: worst case self.len * (newLen / oldLen + 1)
            // For simplicity allocate 4x self.len + newLen as initial buffer.
            // Actually: build result as linked list of segments, then concat.
            // Simpler approach: use realloc-based appending.
            // Simplest for now: pre-allocate 4x self.len + 4*newLen bytes.
            llvm::Value* bound = builder_.CreateAdd(
                builder_.CreateMul(selfLen, builder_.getInt64(4)),
                builder_.CreateMul(newLen, builder_.getInt64(4)));
            bound = builder_.CreateAdd(bound, builder_.getInt64(1));
            llvm::Value* resBuf = builder_.CreateCall(mallocFn, {bound}, "repl.buf");
            auto* resLenSlot = entryAlloca(i64());
            builder_.CreateStore(builder_.getInt64(0), resLenSlot);
            auto* posSlot = entryAlloca(ptrTy());
            builder_.CreateStore(selfData, posSlot);
            llvm::Value* endPtr = builder_.CreateGEP(i8Ty, selfData, selfLen, "repl.end");
            // Loop.
            auto* condBB = llvm::BasicBlock::Create(llvm_, "repl.cond", currentFn_);
            auto* bodyBB = llvm::BasicBlock::Create(llvm_, "repl.body", currentFn_);
            auto* dumpBB = llvm::BasicBlock::Create(llvm_, "repl.dump", currentFn_);
            auto* endBB  = llvm::BasicBlock::Create(llvm_, "repl.end",  currentFn_);
            builder_.CreateBr(condBB);
            builder_.SetInsertPoint(condBB);
            llvm::Value* pos    = builder_.CreateLoad(ptrTy(), posSlot);
            llvm::Value* remain = builder_.CreatePtrDiff(i8Ty, endPtr, pos, "repl.rem");
            llvm::Value* hasOld = builder_.CreateICmpSGT(oldLen, builder_.getInt64(0));
            llvm::Value* remGe  = builder_.CreateICmpSGE(remain, oldLen);
            llvm::Value* canSearch = builder_.CreateAnd(hasOld, remGe);
            builder_.CreateCondBr(canSearch, bodyBB, dumpBB);
            builder_.SetInsertPoint(bodyBB);
            llvm::Value* pos2    = builder_.CreateLoad(ptrTy(), posSlot);
            llvm::Value* remain2 = builder_.CreatePtrDiff(i8Ty, endPtr, pos2, "repl.rem2");
            llvm::Value* found   = builder_.CreateCall(memmemFn, {pos2, remain2, oldData, oldLen}, "repl.found");
            auto* nullPtr = llvm::ConstantPointerNull::get(ptrTy());
            llvm::Value* notFound = builder_.CreateICmpEQ(found, nullPtr);
            auto* copyBB = llvm::BasicBlock::Create(llvm_, "repl.copy", currentFn_);
            builder_.CreateCondBr(notFound, dumpBB, copyBB);
            builder_.SetInsertPoint(copyBB);
            // Copy prefix: found - pos2 bytes.
            llvm::Value* prefLen = builder_.CreatePtrDiff(i8Ty, found, pos2, "repl.pref");
            llvm::Value* resLen  = builder_.CreateLoad(i64(), resLenSlot);
            llvm::Value* dst1    = builder_.CreateGEP(i8Ty, resBuf, resLen);
            builder_.CreateCall(memcpyFn, {dst1, pos2, prefLen});
            llvm::Value* resLen2 = builder_.CreateAdd(resLen, prefLen);
            // Copy newStr.
            llvm::Value* dst2    = builder_.CreateGEP(i8Ty, resBuf, resLen2);
            builder_.CreateCall(memcpyFn, {dst2, newData2, newLen});
            llvm::Value* resLen3 = builder_.CreateAdd(resLen2, newLen);
            builder_.CreateStore(resLen3, resLenSlot);
            // Advance pos past the old occurrence.
            llvm::Value* afterOld = builder_.CreateGEP(i8Ty, found, oldLen, "repl.after");
            builder_.CreateStore(afterOld, posSlot);
            builder_.CreateBr(condBB);
            // Dump remaining.
            builder_.SetInsertPoint(dumpBB);
            llvm::Value* pos3     = builder_.CreateLoad(ptrTy(), posSlot);
            llvm::Value* tailLen  = builder_.CreatePtrDiff(i8Ty, endPtr, pos3, "repl.tail");
            llvm::Value* resLen4  = builder_.CreateLoad(i64(), resLenSlot);
            llvm::Value* dst3     = builder_.CreateGEP(i8Ty, resBuf, resLen4);
            builder_.CreateCall(memcpyFn, {dst3, pos3, tailLen});
            llvm::Value* finalLen = builder_.CreateAdd(resLen4, tailLen);
            builder_.CreateStore(finalLen, resLenSlot);
            builder_.CreateBr(endBB);
            builder_.SetInsertPoint(endBB);
            // Null-terminate.
            llvm::Value* fLen     = builder_.CreateLoad(i64(), resLenSlot);
            llvm::Value* nulPtr2  = builder_.CreateGEP(i8Ty, resBuf, fLen);
            builder_.CreateStore(llvm::ConstantInt::get(i8Ty, 0), nulPtr2);
            // Build result string object.
            uint64_t strBytes = module_->getDataLayout().getTypeAllocSize(stringType());
            llvm::Value* resStr = builder_.CreateCall(mallocFn, {builder_.getInt64(strBytes)}, "repl.obj");
            builder_.CreateStore(resBuf, builder_.CreateStructGEP(stringType(), resStr, 0));
            builder_.CreateStore(fLen, builder_.CreateStructGEP(stringType(), resStr, 1));
            return resStr;
        }

        // §9.6: ToUpper/ToLower — ASCII-only case conversion via toupper/tolower.
        if (method == "ToUpper" || method == "ToLower") {
            if (!call.args.empty()) return nullptr;  // no extra args
            auto* i8Ty = llvm::Type::getInt8Ty(llvm_);
            llvm::Value* len = loadStringLen(self);
            llvm::Value* data = loadStringData(self);
            // Declare toupper / tolower: int fn(int)
            const char* fnName = method == "ToUpper" ? "toupper" : "tolower";
            auto* caseFn = module_->getFunction(fnName);
            if (!caseFn) {
                auto* ft = llvm::FunctionType::get(i32(), {i32()}, false);
                caseFn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, fnName, module_.get());
            }
            // Allocate new buffer.
            auto* mallocFn = declareC("malloc", ptrTy(), {i64()});
            llvm::Value* bufSz = builder_.CreateAdd(len, builder_.getInt64(1));
            llvm::Value* newData = builder_.CreateCall(mallocFn, {bufSz}, "tc.data");
            // Loop: for i in 0..len, newData[i] = fn(data[i])
            auto* idxSlot = entryAlloca(i64());
            builder_.CreateStore(builder_.getInt64(0), idxSlot);
            auto* condBB = llvm::BasicBlock::Create(llvm_, "tc.cond", currentFn_);
            auto* bodyBB = llvm::BasicBlock::Create(llvm_, "tc.body", currentFn_);
            auto* endBB  = llvm::BasicBlock::Create(llvm_, "tc.end",  currentFn_);
            builder_.CreateBr(condBB);
            builder_.SetInsertPoint(condBB);
            llvm::Value* idx = builder_.CreateLoad(i64(), idxSlot);
            builder_.CreateCondBr(builder_.CreateICmpSLT(idx, len), bodyBB, endBB);
            builder_.SetInsertPoint(bodyBB);
            llvm::Value* idxB = builder_.CreateLoad(i64(), idxSlot);
            llvm::Value* bytePtr = builder_.CreateGEP(i8Ty, data, idxB);
            llvm::Value* byte = builder_.CreateLoad(i8Ty, bytePtr);
            llvm::Value* asI32 = builder_.CreateZExt(byte, i32());
            llvm::Value* converted = builder_.CreateCall(caseFn, {asI32});
            llvm::Value* asByte = builder_.CreateTrunc(converted, i8Ty);
            llvm::Value* dstPtr = builder_.CreateGEP(i8Ty, newData, idxB);
            builder_.CreateStore(asByte, dstPtr);
            llvm::Value* next = builder_.CreateAdd(idxB, builder_.getInt64(1));
            builder_.CreateStore(next, idxSlot);
            builder_.CreateBr(condBB);
            builder_.SetInsertPoint(endBB);
            // Null-terminate.
            llvm::Value* nullPtr = builder_.CreateGEP(i8Ty, newData, len);
            builder_.CreateStore(llvm::ConstantInt::get(i8Ty, 0), nullPtr);
            // Build result string.
            uint64_t strBytes = module_->getDataLayout().getTypeAllocSize(stringType());
            llvm::Value* newStr = builder_.CreateCall(mallocFn, {builder_.getInt64(strBytes)}, "tc.obj");
            builder_.CreateStore(newData, builder_.CreateStructGEP(stringType(), newStr, 0));
            builder_.CreateStore(len, builder_.CreateStructGEP(stringType(), newStr, 1));
            return newStr;
        }

        // §9.6: Trim/TrimStart/TrimEnd — remove leading/trailing ASCII whitespace.
        if (method == "Trim" || method == "TrimStart" || method == "TrimEnd") {
            if (!call.args.empty()) return nullptr;
            // Implement via stripping bytes that match isspace.
            auto* i8Ty = llvm::Type::getInt8Ty(llvm_);
            llvm::Value* len = loadStringLen(self);
            llvm::Value* data = loadStringData(self);
            auto* mallocFn = declareC("malloc", ptrTy(), {i64()});
            // Declare isspace: int isspace(int)
            auto* isspaceFn = module_->getFunction("isspace");
            if (!isspaceFn) {
                auto* ft = llvm::FunctionType::get(i32(), {i32()}, false);
                isspaceFn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "isspace", module_.get());
            }
            // Find start and end indices.
            auto startSlot = entryAlloca(i64());
            auto endSlot   = entryAlloca(i64());
            builder_.CreateStore(builder_.getInt64(0), startSlot);
            builder_.CreateStore(len, endSlot);
            // Simple approach: use memmove-based trimming with loop.
            // For brevity, reuse Substring logic after computing new bounds.
            llvm::Value* startV = builder_.getInt64(0);
            llvm::Value* endV   = len;
            // Skip leading whitespace (for Trim and TrimStart).
            if (method == "Trim" || method == "TrimStart") {
                auto* stBB  = llvm::BasicBlock::Create(llvm_, "trim.s.cond", currentFn_);
                auto* stBdy = llvm::BasicBlock::Create(llvm_, "trim.s.body", currentFn_);
                auto* stEnd = llvm::BasicBlock::Create(llvm_, "trim.s.end",  currentFn_);
                builder_.CreateStore(startV, startSlot);
                builder_.CreateBr(stBB);
                builder_.SetInsertPoint(stBB);
                llvm::Value* si = builder_.CreateLoad(i64(), startSlot);
                llvm::Value* sLt = builder_.CreateICmpSLT(si, endV);
                builder_.CreateCondBr(sLt, stBdy, stEnd);
                builder_.SetInsertPoint(stBdy);
                llvm::Value* siB = builder_.CreateLoad(i64(), startSlot);
                llvm::Value* bp  = builder_.CreateGEP(i8Ty, data, siB);
                llvm::Value* by  = builder_.CreateLoad(i8Ty, bp);
                llvm::Value* byI = builder_.CreateZExt(by, i32());
                llvm::Value* isSp = builder_.CreateCall(isspaceFn, {byI});
                llvm::Value* isSpBool = builder_.CreateICmpNE(isSp, builder_.getInt32(0));
                auto* nextSBB = llvm::BasicBlock::Create(llvm_, "trim.s.next", currentFn_);
                builder_.CreateCondBr(isSpBool, nextSBB, stEnd);
                builder_.SetInsertPoint(nextSBB);
                llvm::Value* siN = builder_.CreateLoad(i64(), startSlot);
                builder_.CreateStore(builder_.CreateAdd(siN, builder_.getInt64(1)), startSlot);
                builder_.CreateBr(stBB);
                builder_.SetInsertPoint(stEnd);
                startV = builder_.CreateLoad(i64(), startSlot);
            }
            // Skip trailing whitespace (for Trim and TrimEnd).
            if (method == "Trim" || method == "TrimEnd") {
                builder_.CreateStore(len, endSlot);
                auto* etBB  = llvm::BasicBlock::Create(llvm_, "trim.e.cond", currentFn_);
                auto* etBdy = llvm::BasicBlock::Create(llvm_, "trim.e.body", currentFn_);
                auto* etEnd = llvm::BasicBlock::Create(llvm_, "trim.e.end",  currentFn_);
                builder_.CreateBr(etBB);
                builder_.SetInsertPoint(etBB);
                llvm::Value* ei = builder_.CreateLoad(i64(), endSlot);
                llvm::Value* eGt = builder_.CreateICmpSGT(ei, startV);
                builder_.CreateCondBr(eGt, etBdy, etEnd);
                builder_.SetInsertPoint(etBdy);
                llvm::Value* eiB = builder_.CreateLoad(i64(), endSlot);
                llvm::Value* prev = builder_.CreateSub(eiB, builder_.getInt64(1));
                llvm::Value* bp  = builder_.CreateGEP(i8Ty, data, prev);
                llvm::Value* by  = builder_.CreateLoad(i8Ty, bp);
                llvm::Value* byI = builder_.CreateZExt(by, i32());
                llvm::Value* isSp = builder_.CreateCall(isspaceFn, {byI});
                llvm::Value* isSpBool = builder_.CreateICmpNE(isSp, builder_.getInt32(0));
                auto* nextEBB = llvm::BasicBlock::Create(llvm_, "trim.e.next", currentFn_);
                builder_.CreateCondBr(isSpBool, nextEBB, etEnd);
                builder_.SetInsertPoint(nextEBB);
                llvm::Value* eiN = builder_.CreateLoad(i64(), endSlot);
                builder_.CreateStore(builder_.CreateSub(eiN, builder_.getInt64(1)), endSlot);
                builder_.CreateBr(etBB);
                builder_.SetInsertPoint(etEnd);
                endV = builder_.CreateLoad(i64(), endSlot);
            }
            // Build trimmed string.
            llvm::Value* trimLen = builder_.CreateSub(endV, startV);
            llvm::Value* dataSz = builder_.CreateAdd(trimLen, builder_.getInt64(1));
            llvm::Value* newData = builder_.CreateCall(mallocFn, {dataSz}, "trim.data");
            auto* memcpyFn = declareC("memcpy", ptrTy(), {ptrTy(), ptrTy(), i64()});
            llvm::Value* srcPtr = builder_.CreateGEP(i8Ty, data, startV);
            builder_.CreateCall(memcpyFn, {newData, srcPtr, trimLen});
            llvm::Value* nulPtr = builder_.CreateGEP(i8Ty, newData, trimLen);
            builder_.CreateStore(llvm::ConstantInt::get(i8Ty, 0), nulPtr);
            uint64_t strBytes = module_->getDataLayout().getTypeAllocSize(stringType());
            llvm::Value* newStr = builder_.CreateCall(mallocFn, {builder_.getInt64(strBytes)}, "trim.obj");
            builder_.CreateStore(newData, builder_.CreateStructGEP(stringType(), newStr, 0));
            builder_.CreateStore(trimLen, builder_.CreateStructGEP(stringType(), newStr, 1));
            return newStr;
        }

        // §9.6: PadLeft(totalWidth)/PadRight(totalWidth): string
        // Pads with spaces on the left/right to reach totalWidth.
        if ((method == "PadLeft" || method == "PadRight") &&
            call.args.size() == 1 && call.args[0].value) {
            llvm::Value* widthArg = lowerExpr(*call.args[0].value);
            if (!widthArg) return nullptr;
            auto* i8Ty = llvm::Type::getInt8Ty(llvm_);
            llvm::Value* selfLen = loadStringLen(self);
            llvm::Value* selfData = loadStringData(self);
            llvm::Value* width64 = builder_.CreateSExt(widthArg, i64(), "pad.width");
            // If selfLen >= totalWidth, return self unchanged.
            llvm::Value* needPad = builder_.CreateICmpSGT(width64, selfLen, "pad.need");
            auto* padBB = llvm::BasicBlock::Create(llvm_, "pad.do",  currentFn_);
            auto* endBB = llvm::BasicBlock::Create(llvm_, "pad.end", currentFn_);
            auto* entBB = builder_.GetInsertBlock();
            builder_.CreateCondBr(needPad, padBB, endBB);
            builder_.SetInsertPoint(padBB);
            llvm::Value* padLen = builder_.CreateSub(width64, selfLen, "pad.len");
            auto* mallocFn = declareC("malloc", ptrTy(), {i64()});
            auto* memcpyFn = declareC("memcpy", ptrTy(), {ptrTy(), ptrTy(), i64()});
            auto* memsetFn = module_->getFunction("memset");
            if (!memsetFn) {
                auto* ft = llvm::FunctionType::get(ptrTy(), {ptrTy(), i32(), i64()}, false);
                memsetFn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "memset", module_.get());
            }
            llvm::Value* dataSz = builder_.CreateAdd(width64, builder_.getInt64(1));
            llvm::Value* newData = builder_.CreateCall(mallocFn, {dataSz}, "pad.data");
            // Fill with spaces.
            builder_.CreateCall(memsetFn, {newData, builder_.getInt32(' '), width64});
            if (method == "PadLeft") {
                // Self goes after the spaces.
                llvm::Value* dst = builder_.CreateGEP(i8Ty, newData, padLen);
                builder_.CreateCall(memcpyFn, {dst, selfData, selfLen});
            } else {
                // Self goes at the start.
                builder_.CreateCall(memcpyFn, {newData, selfData, selfLen});
            }
            // Null-terminate.
            llvm::Value* nulPtr = builder_.CreateGEP(i8Ty, newData, width64);
            builder_.CreateStore(llvm::ConstantInt::get(i8Ty, 0), nulPtr);
            uint64_t strBytes = module_->getDataLayout().getTypeAllocSize(stringType());
            llvm::Value* newStr = builder_.CreateCall(mallocFn, {builder_.getInt64(strBytes)}, "pad.obj");
            builder_.CreateStore(newData, builder_.CreateStructGEP(stringType(), newStr, 0));
            builder_.CreateStore(width64, builder_.CreateStructGEP(stringType(), newStr, 1));
            auto* padEnd = builder_.GetInsertBlock();
            builder_.CreateBr(endBB);
            builder_.SetInsertPoint(endBB);
            auto* phi = builder_.CreatePHI(ptrTy(), 2, "pad.result");
            phi->addIncoming(self, entBB);    // no padding needed
            phi->addIncoming(newStr, padEnd); // padded result
            return phi;
        }

        // §9.6: Repeat(n: int): string — repeat self n times.
        if (method == "Repeat" && call.args.size() == 1 && call.args[0].value) {
            llvm::Value* nArg = lowerExpr(*call.args[0].value);
            if (!nArg) return nullptr;
            auto* i8Ty = llvm::Type::getInt8Ty(llvm_);
            llvm::Value* selfLen = loadStringLen(self);
            llvm::Value* selfData = loadStringData(self);
            llvm::Value* n64 = builder_.CreateSExt(nArg, i64(), "rep.n");
            llvm::Value* totalLen = builder_.CreateMul(selfLen, n64, "rep.total");
            auto* mallocFn = declareC("malloc", ptrTy(), {i64()});
            auto* memcpyFn = declareC("memcpy", ptrTy(), {ptrTy(), ptrTy(), i64()});
            llvm::Value* dataSz = builder_.CreateAdd(totalLen, builder_.getInt64(1));
            llvm::Value* newData = builder_.CreateCall(mallocFn, {dataSz}, "rep.data");
            // Loop: copy self n times.
            auto* idxSlot = entryAlloca(i64());
            builder_.CreateStore(builder_.getInt64(0), idxSlot);
            auto* condBB = llvm::BasicBlock::Create(llvm_, "rep.cond", currentFn_);
            auto* bodyBB = llvm::BasicBlock::Create(llvm_, "rep.body", currentFn_);
            auto* endBB  = llvm::BasicBlock::Create(llvm_, "rep.end",  currentFn_);
            builder_.CreateBr(condBB);
            builder_.SetInsertPoint(condBB);
            llvm::Value* idx = builder_.CreateLoad(i64(), idxSlot);
            builder_.CreateCondBr(builder_.CreateICmpSLT(idx, n64), bodyBB, endBB);
            builder_.SetInsertPoint(bodyBB);
            llvm::Value* idxB = builder_.CreateLoad(i64(), idxSlot);
            llvm::Value* off = builder_.CreateMul(idxB, selfLen, "rep.off");
            llvm::Value* dst = builder_.CreateGEP(i8Ty, newData, off, "rep.dst");
            builder_.CreateCall(memcpyFn, {dst, selfData, selfLen});
            builder_.CreateStore(builder_.CreateAdd(idxB, builder_.getInt64(1)), idxSlot);
            builder_.CreateBr(condBB);
            builder_.SetInsertPoint(endBB);
            llvm::Value* nulPtr = builder_.CreateGEP(i8Ty, newData, totalLen);
            builder_.CreateStore(llvm::ConstantInt::get(i8Ty, 0), nulPtr);
            uint64_t strBytes = module_->getDataLayout().getTypeAllocSize(stringType());
            llvm::Value* strObj = builder_.CreateCall(mallocFn, {builder_.getInt64(strBytes)}, "rep.obj");
            builder_.CreateStore(newData, builder_.CreateStructGEP(stringType(), strObj, 0));
            builder_.CreateStore(totalLen, builder_.CreateStructGEP(stringType(), strObj, 1));
            return strObj;
        }

        return nullptr;  // method not recognised
    }

    // CG25: is a Bezel primitive an unsigned integer type? (Drives
    // zext/udiv/lshr/unsigned-comparison choices.)
    static bool isUnsignedPrim(sema::PrimitiveType p) {
        using P = sema::PrimitiveType;
        return p == P::Byte || p == P::UShort || p == P::UInt ||
               p == P::ULong || p == P::Char;
    }

    // CG25b: does either operand of a binary expression have an unsigned
    // integer type (drives unsigned op selection)?
    bool isUnsignedBinOperand(const ast::BinaryExpr& bin) {
        auto unsignedExpr = [&](const ast::Expr* e) {
            if (!e) return false;
            auto* rt = ctx_.exprType(*e);
            return rt && rt->kind == sema::TypeRef::Kind::Primitive &&
                   isUnsignedPrim(rt->primitive);
        };
        return unsignedExpr(bin.left.get()) || unsignedExpr(bin.right.get());
    }

    // CG31: is the assignment/increment target an unsigned-typed lvalue?
    bool isUnsignedTargetExpr(const ast::Expr& e) {
        auto* rt = ctx_.exprType(e);
        return rt && rt->kind == sema::TypeRef::Kind::Primitive &&
               isUnsignedPrim(rt->primitive);
    }

    // CG31: the address (and element type) of an assignable target — a
    // local/parameter slot, a static-field global, a `this.f`/`obj.f`
    // field, or a bare implicit-`this` field. Null if not assignable.
    llvm::Value* lvalueAddr(const ast::Expr& target, llvm::Type*& elemTy) {
        // CG50: a ref/out parameter — its address is the incoming pointer.
        if (llvm::Value* a = refParamAddr(target, elemTy)) return a;
        if (llvm::AllocaInst* slot = localSlot(target)) {
            elemTy = slot->getAllocatedType();
            return slot;
        }
        if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&target)) {
            if (llvm::Value* a = fieldAddr(*ma, elemTy)) return a;
        }
        if (llvm::GlobalVariable* gv = staticFieldGlobal(target)) {
            elemTy = gv->getValueType();
            return gv;
        }
        // §15.1: inline array element lvalue `a[i]`.
        if (auto* ix = dynamic_cast<const ast::IndexExpr*>(&target)) {
            if (ix->target && !ix->indices.empty() && ix->indices[0].value) {
                llvm::Type* arrSlotTy = nullptr;
                llvm::Value* arrAddr = lvalueAddr(*ix->target, arrSlotTy);
                if (arrAddr && arrSlotTy) {
                    if (auto* arrTy = llvm::dyn_cast<llvm::ArrayType>(arrSlotTy)) {
                        llvm::Value* idx = lowerExpr(*ix->indices[0].value);
                        if (idx) {
                            elemTy = arrTy->getElementType();
                            return builder_.CreateGEP(
                                arrTy, arrAddr, {builder_.getInt64(0), idx});
                        }
                    }
                }
            }
        }
        return implicitFieldAddr(target, elemTy);
    }

    // CG31: apply a compound-assignment/increment arithmetic or bitwise
    // operator (op without the trailing `=`). Integer `+ - *` and `/ mod`
    // are overflow/zero-checked exactly as in lowerBinary; bitwise/shift
    // are exact; floats use IEEE ops.
    llvm::Value* emitCompoundOp(const std::string& op, llvm::Value* l,
                                llvm::Value* r, bool fp, bool uns) {
        if (fp) {
            if (op == "+") return builder_.CreateFAdd(l, r);
            if (op == "-") return builder_.CreateFSub(l, r);
            if (op == "*") return builder_.CreateFMul(l, r);
            if (op == "/") return builder_.CreateFDiv(l, r);
            return nullptr;
        }
        if (op == "+")
            return emitCheckedArith(uns ? llvm::Intrinsic::uadd_with_overflow
                                        : llvm::Intrinsic::sadd_with_overflow,
                                    l, r);
        if (op == "-")
            return emitCheckedArith(uns ? llvm::Intrinsic::usub_with_overflow
                                        : llvm::Intrinsic::ssub_with_overflow,
                                    l, r);
        if (op == "*")
            return emitCheckedArith(uns ? llvm::Intrinsic::umul_with_overflow
                                        : llvm::Intrinsic::smul_with_overflow,
                                    l, r);
        if (op == "/" || op == "mod") {
            emitGuardThrow(builder_.CreateICmpEQ(r, zeroLike(r)),
                           "DivideByZeroException");
            if (uns)
                return op == "/" ? builder_.CreateUDiv(l, r)
                                 : builder_.CreateURem(l, r);
            llvm::Value* negOne = llvm::ConstantInt::getSigned(r->getType(), -1);
            emitGuardThrow(
                builder_.CreateAnd(builder_.CreateICmpEQ(l, minIntLike(r)),
                                   builder_.CreateICmpEQ(r, negOne)),
                "OverflowException");
            return op == "/" ? builder_.CreateSDiv(l, r)
                             : builder_.CreateSRem(l, r);
        }
        if (op == "&") return builder_.CreateAnd(l, r);
        if (op == "|") return builder_.CreateOr(l, r);
        if (op == "^") return builder_.CreateXor(l, r);
        if (op == "<<") return builder_.CreateShl(l, r);
        if (op == ">>")
            return uns ? builder_.CreateLShr(l, r) : builder_.CreateAShr(l, r);
        return nullptr;
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
                return lowerPrimitive(rt->primitive);
            case sema::TypeRef::Kind::Named:
                if (rt->declaration &&
                    (rt->declaration->kind == ast::TypeDeclKind::Class ||
                     rt->declaration->kind == ast::TypeDeclKind::Interface)) {
                    return ptrTy();  // reference types are heap pointers
                }
                // §9.4: a record is a value type, lowered to a first-class
                // LLVM struct value. Locals/params/returns hold the struct by
                // value, so copy-on-assignment and copy-on-return are
                // automatic and frame-safe (a returned record travels in
                // registers, not as a pointer into the callee's dead frame).
                // Field access spills the value to a stack slot to take a
                // field address; records are immutable (§9.2) so the spill
                // never loses a write.
                if (rt->declaration &&
                    rt->declaration->kind == ast::TypeDeclKind::Record) {
                    return recordStruct(rt->declaration);
                }
                return i32();  // other value-kind named (enum) — int
            case sema::TypeRef::Kind::InlineArray: {
                // §15.1: [N]T is an LLVM fixed-size array [N x elemType].
                // N is stored as an IntLiteralExpr; element type recurses.
                if (!rt->arrayElement || !rt->arraySize) return i32();
                llvm::Type* elem = lowerTypeRef(rt->arrayElement);
                auto* lit =
                    dynamic_cast<const ast::IntLiteralExpr*>(rt->arraySize);
                if (!lit) return i32();
                uint64_t n = std::stoull(std::string(lit->text));
                return llvm::ArrayType::get(elem, n);
            }
            default:
                return i32();
        }
    }

    // CG15: collect a class's instance fields in layout order —
    // base-class fields FIRST (prefix layout: a `Derived*` aliases a
    // `Base*`), then own fields. Recurses the single base class
    // (baseTypes[0] when it is a class). Static fields are excluded.
    // CG33: an auto-property (`Name: T { get; set; }`) — no indexer, no
    // explicit accessor bodies, no arrow/block getter — is backed by a
    // compiler-generated instance field, so it gets storage like a field.
    bool isAutoProperty(const sema::Symbol* m) {
        auto* ps = dynamic_cast<const sema::PropertySymbol*>(m);
        if (!ps || ps->isIndexer) return false;
        auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl);
        if (!pd || pd->isIndexer ||
            pd->bodyKind != ast::PropertyBodyKind::Auto)
            return false;
        for (auto mod : ps->modifiers)
            if (mod == "static") return false;
        return true;
    }

    // CG33b: the arrow-getter expression of a computed property, or null
    // (auto-properties, indexers, and block/explicit-accessor getters
    // are not arrow getters).
    const ast::Expr* computedGetterOf(sema::Symbol* sym) {
        auto* ps = dynamic_cast<sema::PropertySymbol*>(sym);
        if (!ps || ps->isIndexer) return nullptr;
        auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl);
        if (!pd || pd->isIndexer) return nullptr;
        // Property-level expression body: `property P: T => expr;`.
        if (pd->bodyKind == ast::PropertyBodyKind::Expression)
            return pd->exprBody.get();
        // Accessor-block getter with an expression body: `{ get => expr; }`.
        if (pd->bodyKind == ast::PropertyBodyKind::Block) {
            for (const auto& acc : pd->accessors) {
                if (!acc.isSet &&
                    acc.bodyKind == ast::PropertyAccessorBodyKind::Expression)
                    return acc.exprBody.get();
            }
        }
        return nullptr;
    }

    // §22.9: the block-form setter accessor of the property `target` names,
    // or null (auto-properties have backing storage and use the normal
    // store path; a non-property target is not one). Used to run a custom
    // setter on `obj.P := v`.
    const ast::PropertyAccessor* computedSetterOf(const ast::Expr& target) {
        auto* ps = dynamic_cast<sema::PropertySymbol*>(ctx_.exprSymbol(target));
        if (!ps || ps->isIndexer || isAutoProperty(ps)) return nullptr;
        auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl);
        if (!pd || pd->bodyKind != ast::PropertyBodyKind::Block) return nullptr;
        for (const auto& acc : pd->accessors) {
            if (acc.isSet &&
                acc.bodyKind != ast::PropertyAccessorBodyKind::Abstract)
                return &acc;
        }
        return nullptr;
    }

    // §22.9: run a block-form property setter for `obj.P := value`. Binds
    // `value` (anchored on the PropertyDecl in sema) to a slot, sets `this`
    // to the receiver, and lowers the setter body. Returns false (so the
    // caller falls through to the normal store path) when `target` is not a
    // block-setter property.
    bool lowerPropertySet(const ast::Expr& target, const ast::Expr& value) {
        if (!computedSetterOf(target)) return false;
        llvm::Value* v = lowerExpr(value);
        return v ? lowerPropertySetValue(target, v) : false;
    }

    // §22.9: run a block-form property setter with an already-lowered value
    // `v` (the `value` parameter). Shared by the plain `obj.P := …` path and
    // the compound-assignment read-modify-write path. Returns false when
    // `target` is not a block-setter property.
    bool lowerPropertySetValue(const ast::Expr& target, llvm::Value* v) {
        const ast::PropertyAccessor* setter = computedSetterOf(target);
        if (!setter || !v) return false;
        auto* ps = dynamic_cast<sema::PropertySymbol*>(ctx_.exprSymbol(target));
        auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl);
        llvm::Value* recv = currentThis_;
        if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&target))
            recv = ma->target ? lowerExpr(*ma->target) : currentThis_;
        if (!recv) return false;
        // Bind `value` — sema anchored its symbol's decl on the PropertyDecl.
        llvm::AllocaInst* slot = entryAlloca(v->getType());
        builder_.CreateStore(v, slot);
        locals_[pd] = slot;
        llvm::Value* savedThis = currentThis_;
        currentThis_ = recv;
        if (setter->bodyKind == ast::PropertyAccessorBodyKind::Expression &&
            setter->exprBody)
            lowerExpr(*setter->exprBody);
        else
            lowerScopedBlock(setter->stmts);
        currentThis_ = savedThis;
        return true;
    }

    // §22.9: compound assignment `obj.P OP= rhs` on a block-form property —
    // a read-modify-write: read the current value through the getter, apply
    // OP (overflow-checked, as for any compound assignment), then run the
    // setter with the result. Returns false when `target` is not a
    // block-setter property (so the caller uses the normal lvalue path).
    // NOTE: the receiver is currently lowered once for the getter read and
    // again for the setter write; this is observable only for receivers with
    // side effects, which are not expressible as assignment targets here.
    bool lowerPropertyCompound(const ast::AssignmentStmt& as) {
        if (!computedSetterOf(*as.target)) return false;
        llvm::Value* cur = lowerExpr(*as.target);   // getter read (CG33c)
        llvm::Value* rhs = lowerExpr(*as.value);
        if (!cur || !rhs) return false;
        std::string op(as.op);
        if (!op.empty() && op.back() == '=') op.pop_back();
        llvm::Value* res = emitCompoundOp(
            op, cur, rhs, cur->getType()->isFloatingPointTy(),
            isUnsignedTargetExpr(*as.target));
        return res ? lowerPropertySetValue(*as.target, res) : false;
    }

    // The LLVM type backing an instance-storage member (field or
    // auto-property).
    llvm::Type* storageType(const sema::Symbol* m) {
        // §9.1: a record's positional field carries its type on the
        // originating Param, not a FieldDecl (its `decl` is the TypeDecl).
        if (auto* fs = dynamic_cast<const sema::FieldSymbol*>(m))
            if (fs->posParam)
                return lowerType(fs->posParam->type.get());
        if (auto* fd = dynamic_cast<const ast::FieldDecl*>(m->decl))
            return lowerType(fd->type.get());
        if (auto* pd = dynamic_cast<const ast::PropertyDecl*>(m->decl))
            return lowerType(pd->type.get());
        return i32();
    }

    // CG15/CG33: a class's instance storage in layout order — base-class
    // storage first (prefix layout), then own. Storage = non-static fields
    // and auto-properties.
    void collectInstanceStorage(const sema::TypeSymbol* t,
                                std::vector<const sema::Symbol*>& out) {
        for (const sema::TypeSymbol* b : t->baseTypes) {
            if (b && b->kind == ast::TypeDeclKind::Class) {
                collectInstanceStorage(b, out);
                break;  // a class has at most one base class
            }
        }
        for (const sema::Symbol* m : t->members) {
            auto* fs = dynamic_cast<const sema::FieldSymbol*>(m);
            if (fs && !isStaticField(fs)) { out.push_back(m); continue; }
            if (isAutoProperty(m)) out.push_back(m);
        }
    }

    // CG8/CG15: the LLVM struct for a class's instance, built lazily.
    // Inherited fields come first (prefix layout), then own fields;
    // fieldIndex_ records each field's absolute index. The prefix
    // property means an inherited field has the same index in the base
    // struct and every derived struct, so member access through
    // fs->parent's struct is correct on a derived instance.
    llvm::StructType* classStruct(const sema::TypeSymbol* t) {
        // CG29: a record has no vtable header; route to recordStruct so every
        // classStruct caller (field access, etc.) gets the right layout.
        if (t->kind == ast::TypeDeclKind::Record) return recordStruct(t);
        auto it = structs_.find(t);
        if (it != structs_.end()) return it->second;
        auto* st = llvm::StructType::create(
            llvm_, std::string("class.") + std::string(t->name));
        structs_[t] = st;  // record before filling (supports self-reference)
        std::vector<const sema::Symbol*> storage;
        collectInstanceStorage(t, storage);
        std::vector<llvm::Type*> fieldTys;
        // CG17: slot 0 is the hidden vtable pointer; data fields follow.
        fieldTys.push_back(ptrTy());
        unsigned idx = 1;
        for (const sema::Symbol* m : storage) {
            fieldTys.push_back(storageType(m));
            fieldIndex_[m] = idx++;
        }
        st->setBody(fieldTys);
        return st;
    }

    // CG29 (§9.4): the LLVM struct backing a record value. Unlike a class,
    // a record has no vtable header — its data fields start at index 0. The
    // fields are the record's non-static FieldSymbols in declaration order
    // (positional fields plus any body fields). `fieldIndex_` is populated so
    // field access reuses the same GEP path as classes. Records do not
    // inherit (§9.6), so there is no prefix-layout walk.
    llvm::StructType* recordStruct(const sema::TypeSymbol* t) {
        auto it = structs_.find(t);
        if (it != structs_.end()) return it->second;
        auto* st = llvm::StructType::create(
            llvm_, std::string("record.") + std::string(t->name));
        structs_[t] = st;  // before filling, to support self-reference
        std::vector<llvm::Type*> fieldTys;
        unsigned idx = 0;
        for (const sema::Symbol* m : t->members) {
            auto* fs = dynamic_cast<const sema::FieldSymbol*>(m);
            if (!fs || isStaticField(fs)) continue;
            fieldTys.push_back(storageType(fs));
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
        // §25: an operator's name is empty; build a unique, collision-free
        // name from its token (a type may declare several operators).
        if (ms->isOperator)
            return "bz$" + cls + "$op$" + std::string(ms->operatorToken);
        return "bz$" + cls + "$" + std::string(ms->name);
    }

    // CG12: a static field becomes an internal global. Its initializer
    // is the literal constant when present (int/bool literal), else the
    // zero value. Non-literal static initializers (which the spec runs in
    // a static constructor, §22) are deferred — the global is zeroed.
    // CG26: declare an external (C-ABI) function for each extern method
    // (§32). The function keeps the declared C name so the JIT/linker
    // resolves it against the host process / libc. FFI types lower via
    // lowerType (int → i32, void return for no type).
    void declareExterns() {
        for (const auto& sym : ctx_.symbols()) {
            auto* xb = dynamic_cast<const sema::ExternBlockSymbol*>(sym.get());
            if (!xb) continue;
            for (sema::Symbol* d : xb->decls) {
                auto* xd = dynamic_cast<sema::ExternDeclSymbol*>(d);
                if (!xd || !xd->externDecl) continue;
                const ast::ExternDecl* ed = xd->externDecl;
                llvm::Type* retTy = ed->returnType
                                        ? lowerType(ed->returnType.get())
                                        : llvm::Type::getVoidTy(llvm_);
                std::vector<llvm::Type*> paramTys;
                for (const auto& p : ed->params)
                    paramTys.push_back(lowerType(p.type.get()));
                auto* fnTy = llvm::FunctionType::get(retTy, paramTys, false);
                externFns_[xd] = llvm::Function::Create(
                    fnTy, llvm::Function::ExternalLinkage,
                    std::string(xd->name), module_.get());
            }
        }
    }

    // CG30: compute each enum constant's integer value (§10). Explicit
    // `:= N` sets the value; otherwise it is the previous value + 1
    // (starting at 0). Only integer-literal initializers are evaluated;
    // other constant expressions are deferred.
    void computeEnumValues() {
        for (const auto& sym : ctx_.symbols()) {
            auto* ts = dynamic_cast<const sema::TypeSymbol*>(sym.get());
            if (!ts || ts->kind != ast::TypeDeclKind::Enum) continue;
            auto* td = dynamic_cast<const ast::TypeDecl*>(ts->decl);
            if (!td) continue;
            std::int64_t running = 0;
            for (const auto& ec : td->enumConstants) {
                std::int64_t val = running;
                if (ec.value) {
                    if (auto* lit = dynamic_cast<const ast::IntLiteralExpr*>(
                            ec.value.get()))
                        val = parseIntLiteral(lit->text);
                }
                for (const sema::Symbol* m : ts->members) {
                    if (dynamic_cast<const sema::EnumConstantSymbol*>(m) &&
                        m->name == ec.name) {
                        enumValue_[m] = val;
                        break;
                    }
                }
                running = val + 1;
            }
        }
    }

    // CG30: if `e` resolves to an enum constant, its integer value (i32).
    llvm::Value* enumConstant(const ast::Expr& e) {
        auto* sym = ctx_.exprSymbol(e);
        if (!dynamic_cast<sema::EnumConstantSymbol*>(sym)) return nullptr;
        auto it = enumValue_.find(sym);
        if (it == enumValue_.end()) return nullptr;
        return llvm::ConstantInt::get(
            i32(), static_cast<std::uint64_t>(it->second), /*isSigned=*/true);
    }

    // CG43: the enum TypeSymbol an expression's type denotes, or null. Uses
    // the expression's resolved type when present, else falls back to a
    // local/parameter's declared type (a scrutinee identifier often carries
    // only an exprSymbol, not an exprType).
    const sema::TypeSymbol* enumTypeOf(const ast::Expr& e) {
        const sema::TypeRef* rt = ctx_.exprType(e);
        if (!rt) {
            sema::Symbol* sym = ctx_.exprSymbol(e);
            if (auto* loc = dynamic_cast<sema::LocalSymbol*>(sym))
                rt = loc->declType;
            else if (auto* par = dynamic_cast<sema::ParameterSymbol*>(sym))
                rt = par->declType ? ctx_.resolvedType(*par->declType)
                                   : nullptr;
        }
        if (!rt || rt->kind != sema::TypeRef::Kind::Named) return nullptr;
        const sema::TypeSymbol* ts = rt->declaration;
        if (ts && ts->kind == ast::TypeDeclKind::Enum) return ts;
        return nullptr;
    }

    // CG43: resolve a `case`-pattern constant expression that names an enum
    // constant (`E.Red` member access or a bare `Red`) against the
    // scrutinee's enum, by name. Null if `e` is not such a name.
    llvm::Value* enumConstantOfPatternExpr(const ast::Expr& e,
                                           const sema::TypeSymbol* enumTy) {
        if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e))
            return enumConstantByName(enumTy, ma->member);
        if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(&e))
            return enumConstantByName(enumTy, id->name);
        return nullptr;
    }

    // CG43: the value of the enum constant named `name` on `enumTy` (i32),
    // resolved by name — used for `case` patterns, whose enum-constant
    // names sema does not resolve to symbols. Null if no such constant.
    llvm::Value* enumConstantByName(const sema::TypeSymbol* enumTy,
                                    std::string_view name) {
        if (!enumTy) return nullptr;
        for (const sema::Symbol* m : enumTy->members) {
            if (m->name != name) continue;
            if (!dynamic_cast<const sema::EnumConstantSymbol*>(m)) continue;
            auto it = enumValue_.find(m);
            if (it == enumValue_.end()) return nullptr;
            return llvm::ConstantInt::get(
                i32(), static_cast<std::uint64_t>(it->second),
                /*isSigned=*/true);
        }
        return nullptr;
    }

    // CG36: a const member (`const X := …`) lowers to its initializer's
    // value (a compile-time constant). Covers bare `X` and `Type.X`.
    llvm::Value* constMemberValue(const ast::Expr& e) {
        auto* cs = dynamic_cast<sema::ConstMemberSymbol*>(ctx_.exprSymbol(e));
        if (!cs) return nullptr;
        auto* cd = dynamic_cast<const ast::ConstDecl*>(cs->decl);
        if (!cd || !cd->value) return nullptr;
        return lowerExpr(*cd->value);
    }

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
            // CG27a: string literal initializer for static string fields.
            if (auto* sl = dynamic_cast<const ast::StringLiteralExpr*>(e)) {
                // lowerStringLiteral creates globals and returns a GV*.
                // Since we're in declareStatics (before emitMethod bodies),
                // the builder's insert point is not set — set it temporarily
                // to avoid assertion errors in CreateStore etc. The string
                // literal only creates globals (no instruction emission needed).
                if (!currentFn_) {
                    // Create a temporary function context so lowerStringLiteral
                    // can use builder_ safely (though it doesn't emit instructions).
                    auto* strGV = lowerStringLiteralConst(sl->text, false);
                    return strGV;
                }
                auto* strGV = lowerStringLiteralConst(sl->text, false);
                return strGV;
            }
            if (auto* vs = dynamic_cast<const ast::VerbatimStringExpr*>(e)) {
                auto* strGV = lowerStringLiteralConst(vs->text, true);
                return strGV;
            }
        }
        return llvm::Constant::getNullValue(ty);
    }

    // Version of lowerStringLiteral that only creates globals (no builder
    // instructions), safe to call during declareStatics.
    llvm::Constant* lowerStringLiteralConst(std::string_view text, bool verbatim) {
        std::string content = processStringText(text, verbatim);
        llvm::Constant* dataArr =
            llvm::ConstantDataArray::getString(llvm_, content, /*AddNull=*/true);
        unsigned id = stringLiteralIdx_++;
        auto* dataGV = new llvm::GlobalVariable(
            *module_, dataArr->getType(), /*isConstant=*/true,
            llvm::GlobalValue::PrivateLinkage, dataArr,
            ".bz.str.data." + std::to_string(id));
        dataGV->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
        dataGV->setAlignment(llvm::Align(1));
        llvm::Constant* lenC = llvm::ConstantInt::get(i64(), content.size());
        llvm::Constant* strVal = llvm::ConstantStruct::get(stringType(), {dataGV, lenC});
        auto* strGV = new llvm::GlobalVariable(
            *module_, stringType(), /*isConstant=*/true,
            llvm::GlobalValue::PrivateLinkage, strVal,
            ".bz.str." + std::to_string(id));
        return strGV;  // Constant pointer to the bz_string struct
    }

    // CG12: the backing global of a bare identifier that resolves to a
    // static field, or null if `e` isn't such a reference.
    llvm::GlobalVariable* staticFieldGlobal(const ast::Expr& e) {
        // A static field reached either bare (`Count`) or qualified
        // (`Type.Count`, a member access) resolves to the same backing
        // global. Match on the field symbol; only static fields are in
        // staticGlobals_, so an instance field's FieldSymbol misses here
        // (and is handled by fieldAddr instead).
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
            CallableInfo ci;
            if (!callableInfo(ms, ci) ||
                ci.bodyKind == ast::MethodBodyKind::Abstract)
                continue;

            llvm::Type* retTy = isMain(ms) ? i32()
                                           : lowerType(ci.returnType);
            std::vector<llvm::Type*> paramTys;
            if (!isStaticMethod(ms)) paramTys.push_back(ptrTy());  // this
            for (const auto& p : *ci.params) {
                paramTys.push_back(isByRefMod(p.modifier)
                                       ? ptrTy()  // CG50: ref/out by reference
                                       : lowerType(p.type.get()));
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

        // CG47: declare static constructors (§22.7) as `void()` functions.
        for (const auto& sym : ctx_.symbols()) {
            auto* cs = dynamic_cast<sema::ConstructorSymbol*>(sym.get());
            if (!cs || !cs->isStatic) continue;
            if (!dynamic_cast<const ast::ConstructorDecl*>(cs->decl)) continue;
            std::string cls = cs->parent ? std::string(cs->parent->name) : "";
            auto* fnType = llvm::FunctionType::get(
                llvm::Type::getVoidTy(llvm_), {}, /*isVarArg=*/false);
            auto* fn = llvm::Function::Create(
                fnType, llvm::Function::ExternalLinkage, "bz$" + cls + "$cctor",
                module_.get());
            staticCtors_.push_back({cs, fn});
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
        // Vtable: [0]=descriptor, [1]=Dispose, [2..2+G)=interface-method
        // slots, [2+G..]=virtual methods. So a virtual method's slot is its
        // position in `slots` + (2 + G).
        const unsigned base = 2 + ifaceSlotCount_;
        for (sema::Symbol* m : t->members) {
            auto* ms = dynamic_cast<sema::MethodSymbol*>(m);
            if (!ms || isStaticMethod(ms) || !isVirtual(ms)) continue;
            auto it = nameSlot.find(ms->name);
            if (it != nameSlot.end()) {       // override: reuse base slot
                slots[it->second - base] = ms;
                vtableSlot_[ms] = it->second;
            } else {                          // new virtual: fresh slot
                unsigned slot = static_cast<unsigned>(slots.size()) + base;
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
    // CG35: the class's (or its bases') method implementing interface
    // method `name`, by name match (signatures match per §11.2).
    sema::MethodSymbol* findMethodByName(const sema::TypeSymbol* cls,
                                         std::string_view name) {
        for (const sema::TypeSymbol* t = cls; t;) {
            for (sema::Symbol* m : t->members) {
                auto* ms = dynamic_cast<sema::MethodSymbol*>(m);
                if (ms && !isStaticMethod(ms) && ms->name == name &&
                    methodFns_.count(ms))
                    return ms;
            }
            t = baseClassOf(t);
        }
        return nullptr;
    }

    void declareVtables() {
        // Pass 0 (CG35): assign each interface method a fixed vtable slot in
        // [2, 2+G), shared across all implementers.
        for (const auto& sym : ctx_.symbols()) {
            auto* ts = dynamic_cast<const sema::TypeSymbol*>(sym.get());
            if (!ts || ts->kind != ast::TypeDeclKind::Interface) continue;
            for (sema::Symbol* m : ts->members) {
                auto* ms = dynamic_cast<sema::MethodSymbol*>(m);
                if (ms && !isStaticMethod(ms))
                    ifaceMethodSlot_[ms] = 2 + ifaceSlotCount_++;
            }
        }
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
            // [0] descriptor, [1] Dispose, [2..2+G) interface slots,
            // [2+G..] virtual methods.
            std::vector<llvm::Constant*> elems(2 + ifaceSlotCount_ +
                                                   slots.size(),
                                               llvm::ConstantPointerNull::get(
                                                   ptrTy()));
            elems[0] = descArray_[ts];
            auto dit = disposerFns_.find(ts);
            if (dit != disposerFns_.end())
                elems[1] = llvm::cast<llvm::Constant>(dit->second);
            // CG35: fill the interface-method slots this class implements.
            std::vector<const sema::TypeSymbol*> supers;
            collectTypeAndBases(ts, supers);
            std::unordered_set<const sema::TypeSymbol*> superSet(supers.begin(),
                                                                supers.end());
            for (const auto& [im, slot] : ifaceMethodSlot_) {
                if (!im->parent || !superSet.count(im->parent)) continue;
                if (sema::MethodSymbol* impl =
                        findMethodByName(ts, im->name)) {
                    auto mit = methodFns_.find(impl);
                    if (mit != methodFns_.end())
                        elems[slot] = llvm::cast<llvm::Constant>(mit->second);
                }
            }
            // Virtual methods at [2+G..].
            for (std::size_t i = 0; i < slots.size(); ++i) {
                auto mit = methodFns_.find(slots[i]);
                if (mit != methodFns_.end())
                    elems[2 + ifaceSlotCount_ + i] =
                        llvm::cast<llvm::Constant>(mit->second);
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

    // CG22: the §31.4 built-in exception hierarchy (child → parent).
    static const std::vector<std::pair<const char*, const char*>>&
    builtinExceptionHierarchy() {
        static const std::vector<std::pair<const char*, const char*>> h = {
            {"Exception", ""},
            {"ArgumentException", "Exception"},
            {"ArgumentNullException", "ArgumentException"},
            {"ArithmeticException", "Exception"},
            {"OverflowException", "ArithmeticException"},
            {"DivideByZeroException", "ArithmeticException"},
            {"InvalidOperationException", "Exception"},
            {"NotImplementedException", "Exception"},
            {"NullReferenceException", "Exception"},
            {"IndexOutOfRangeException", "Exception"},
            {"MatchFailureException", "Exception"},
            {"OperationCanceledException", "Exception"},
            {"TypeInitializationException", "Exception"},
            {"FormatException", "Exception"},
            {"StackOverflowException", "Exception"},
        };
        return h;
    }

    // CG22: build descriptor + vtable globals for the built-in exception
    // types (which have no user TypeSymbol). A type's descriptor lists its
    // own and all ancestor descriptors so `catch Ancestor` matches.
    void setupBuiltinExceptions() {
        const auto& hier = builtinExceptionHierarchy();
        std::unordered_map<std::string, std::string> parent;
        for (const auto& [name, par] : hier) parent[name] = par;

        // Shared no-op Dispose (built-in exceptions own nothing).
        auto* dispTy = llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_),
                                               {ptrTy()}, false);
        builtinDisposeFn_ = llvm::Function::Create(
            dispTy, llvm::Function::InternalLinkage, "bz$exDispose",
            module_.get());
        {
            auto* bb = llvm::BasicBlock::Create(llvm_, "entry",
                                                builtinDisposeFn_);
            llvm::IRBuilder<> b(bb);
            b.CreateRetVoid();
        }

        // Pass 1: declare descriptor globals (sized by ancestor-chain len).
        std::unordered_map<std::string, std::vector<std::string>> chainOf;
        for (const auto& [name, par] : hier) {
            std::vector<std::string> chain;
            for (std::string c = name; !c.empty(); c = parent[c])
                chain.push_back(c);
            chainOf[name] = chain;
            auto* arrTy = llvm::ArrayType::get(ptrTy(), chain.size() + 1);
            builtinDesc_[name] = new llvm::GlobalVariable(
                *module_, arrTy, /*isConstant=*/true,
                llvm::GlobalValue::InternalLinkage, nullptr,
                std::string("btd$") + name);
        }
        // Pass 2: fill descriptors [self..Exception, null].
        for (const auto& [name, chain] : chainOf) {
            std::vector<llvm::Constant*> elems;
            for (const std::string& c : chain) elems.push_back(builtinDesc_[c]);
            elems.push_back(llvm::ConstantPointerNull::get(ptrTy()));
            auto* arrTy = llvm::cast<llvm::ArrayType>(
                builtinDesc_[name]->getValueType());
            builtinDesc_[name]->setInitializer(
                llvm::ConstantArray::get(arrTy, elems));
        }
        // Pass 3: vtable per built-in = [descriptor, Dispose].
        for (const auto& [name, par] : hier) {
            std::vector<llvm::Constant*> v = {builtinDesc_[name],
                                              builtinDisposeFn_};
            auto* arrTy = llvm::ArrayType::get(ptrTy(), v.size());
            builtinVtable_[name] = new llvm::GlobalVariable(
                *module_, arrTy, /*isConstant=*/true,
                llvm::GlobalValue::InternalLinkage,
                llvm::ConstantArray::get(arrTy, v),
                std::string("bvt$") + name);
        }
    }

    // CG22: descriptor global for a catch-arm/throw type name — a user
    // type's descArray_ or a built-in exception's descriptor.
    llvm::GlobalVariable* descriptorForTypeName(const sema::TypeRef* rt,
                                                const ast::Type* tyNode) {
        if (rt && rt->declaration) {
            auto it = descArray_.find(rt->declaration);
            if (it != descArray_.end()) return it->second;
        }
        if (tyNode && tyNode->name && !tyNode->name->parts.empty()) {
            auto it = builtinDesc_.find(
                std::string(tyNode->name->parts.back()));
            if (it != builtinDesc_.end()) return it->second;
        }
        return nullptr;
    }

    // CG22: construct + throw a built-in exception by name, then unwind.
    void emitThrowBuiltin(const char* name) {
        auto vit = builtinVtable_.find(name);
        if (vit == builtinVtable_.end()) return;
        // Minimal object: one pointer (the vtable) — built-ins have no
        // fields. 8 bytes covers the pointer on the host.
        llvm::Value* obj = builder_.CreateCall(
            callocFn(), {builder_.getInt64(1), builder_.getInt64(8)});
        builder_.CreateStore(vit->second, obj);
        builder_.CreateStore(obj, excGlobal());
        emitUnwindDisposals();
        builder_.CreateBr(unwindBB());
    }

    // CG20/CG40: throw `excVal` (or re-raise the enclosing catch arm's
    // exception when `excVal` is null and one is in flight): store it to
    // the exception global, dispose the unwound using-vars, and branch to
    // the unwind target. Leaves the current block terminated.
    void emitThrowValue(llvm::Value* excVal) {
        if (excVal) {
            builder_.CreateStore(excVal, excGlobal());
        } else if (currentCaughtExc_) {
            // Bare `throw` re-raises the arm's caught exception (the global
            // was cleared when the arm was entered).
            builder_.CreateStore(currentCaughtExc_, excGlobal());
        }
        emitUnwindDisposals();  // CG21: dispose unwound using-vars
        builder_.CreateBr(unwindBB());
    }

    // CG22: if `cond` (i1, true = error) holds, throw `name`; otherwise
    // continue. Splits the current block.
    void emitGuardThrow(llvm::Value* cond, const char* name) {
        auto* errBB = llvm::BasicBlock::Create(llvm_, "chk.err", currentFn_);
        auto* cont = llvm::BasicBlock::Create(llvm_, "chk.cont", currentFn_);
        builder_.CreateCondBr(cond, errBB, cont);
        builder_.SetInsertPoint(errBB);
        emitThrowBuiltin(name);
        builder_.SetInsertPoint(cont);
    }

    // CG22: a checked add/sub/mul via the with-overflow intrinsic; raises
    // OverflowException when the operation overflows (§6.1).
    llvm::Value* emitCheckedArith(llvm::Intrinsic::ID id, llvm::Value* l,
                                  llvm::Value* r) {
        auto* fn = llvm::Intrinsic::getOrInsertDeclaration(
            module_.get(), id, {l->getType()});
        llvm::Value* pair = builder_.CreateCall(fn, {l, r});
        llvm::Value* res = builder_.CreateExtractValue(pair, 0);
        llvm::Value* ovf = builder_.CreateExtractValue(pair, 1);
        emitGuardThrow(ovf, "OverflowException");
        return res;
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
        refParams_.clear();
        disposeStack_.clear();
        loopStack_.clear();
        propagateBB_ = nullptr;
        unwindTarget_ = nullptr;
        currentCaughtExc_ = nullptr;
        unwindScopeDepth_ = 0;
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
    // CG20: the module global holding the in-flight exception (lazy).
    llvm::GlobalVariable* excGlobal() {
        if (!excGlobal_) {
            excGlobal_ = new llvm::GlobalVariable(
                *module_, ptrTy(), /*isConstant=*/false,
                llvm::GlobalValue::InternalLinkage,
                llvm::ConstantPointerNull::get(ptrTy()), "bz$exc");
        }
        return excGlobal_;
    }

    // CG20: the per-function propagate block — entered when an exception
    // is pending and not caught here. It returns a dummy value so the
    // caller's post-call check can unwind further. (CG21 will dispose live
    // owned locals here first.) Built lazily.
    llvm::BasicBlock* propagateBB() {
        if (!propagateBB_) {
            auto* save = builder_.GetInsertBlock();
            propagateBB_ = llvm::BasicBlock::Create(llvm_, "propagate",
                                                    currentFn_);
            builder_.SetInsertPoint(propagateBB_);
            emitImplicitReturn(currentFn_);
            if (save) builder_.SetInsertPoint(save);
        }
        return propagateBB_;
    }

    // CG20: where to branch when an exception becomes pending — the
    // innermost try's catch dispatch, else the function propagate block.
    llvm::BasicBlock* unwindBB() {
        return unwindTarget_ ? unwindTarget_ : propagateBB();
    }

    // CG21: dispose the using-vars of the scopes being exited as an
    // exception unwinds to the current unwind target — scopes from the
    // top of the stack down to unwindScopeDepth_ (inclusive), innermost
    // first (§28.3 reverse order; nested scopes inner-before-outer).
    void emitUnwindDisposals() {
        for (std::size_t i = disposeStack_.size(); i-- > unwindScopeDepth_;)
            emitDisposals(disposeStack_[i]);
    }

    // CG20/CG21: after a call that may throw, branch to the unwind target
    // when the exception global is non-null (disposing unwound using-vars
    // first), else continue.
    void emitPendingCheck() {
        llvm::Value* exc = builder_.CreateLoad(ptrTy(), excGlobal());
        llvm::Value* pending = builder_.CreateICmpNE(
            exc, llvm::ConstantPointerNull::get(ptrTy()));
        auto* unwind =
            llvm::BasicBlock::Create(llvm_, "call.unwind", currentFn_);
        auto* cont = llvm::BasicBlock::Create(llvm_, "call.cont", currentFn_);
        builder_.CreateCondBr(pending, unwind, cont);
        builder_.SetInsertPoint(unwind);
        emitUnwindDisposals();
        builder_.CreateBr(unwindBB());
        builder_.SetInsertPoint(cont);
    }

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
        CallableInfo ci;
        if (!callableInfo(ms, ci)) return;
        currentFn_ = fn;
        currentThis_ = nullptr;
        locals_.clear();
        paramSlots_.clear();
        refParams_.clear();
        disposeStack_.clear();
        loopStack_.clear();
        propagateBB_ = nullptr;
        unwindTarget_ = nullptr;
        currentCaughtExc_ = nullptr;
        unwindScopeDepth_ = 0;
        builder_.SetInsertPoint(llvm::BasicBlock::Create(llvm_, "entry", fn));

        bindParameters(fn, /*hasThis=*/!isStaticMethod(ms), &ms->params);

        // CG47: run every static constructor once, before the program's
        // body. The entry point Main is the single root, so calling them at
        // its start runs them before any user code (§22.7).
        if (isMain(ms)) {
            for (auto& [cs, sfn] : staticCtors_) builder_.CreateCall(sfn, {});
        }

        bool terminated = false;
        if (ci.bodyKind == ast::MethodBodyKind::Expression && ci.exprBody) {
            llvm::Value* v = lowerExpr(*ci.exprBody);
            terminated = emitReturn(fn, v);
        } else if (ci.bodyKind == ast::MethodBodyKind::Block) {
            terminated = lowerScopedBlock(*ci.stmts);
        }
        if (!terminated) emitImplicitReturn(fn);
    }

    // CG10: emit a constructor body. Always an instance function (leading
    // `this`); its params bind to the ConstructorSymbol's ParameterSymbols.
    // Field initializers run in `lowerNew` before the call, so the body
    // only runs the explicit statements. Returns void.
    // CG47: emit a static constructor body (§22.7). No `this`, no
    // parameters, no base/this chain — it runs the body statements once to
    // initialize the type's static state.
    void emitStaticConstructorBody(sema::ConstructorSymbol* cs,
                                   llvm::Function* fn) {
        auto* cd = dynamic_cast<const ast::ConstructorDecl*>(cs->decl);
        if (!cd) return;
        currentFn_ = fn;
        currentThis_ = nullptr;
        locals_.clear();
        paramSlots_.clear();
        refParams_.clear();
        disposeStack_.clear();
        loopStack_.clear();
        propagateBB_ = nullptr;
        unwindTarget_ = nullptr;
        currentCaughtExc_ = nullptr;
        unwindScopeDepth_ = 0;
        builder_.SetInsertPoint(llvm::BasicBlock::Create(llvm_, "entry", fn));
        if (!lowerScopedBlock(cd->body)) builder_.CreateRetVoid();
    }

    void emitConstructorBody(sema::ConstructorSymbol* cs, llvm::Function* fn) {
        auto* cd = dynamic_cast<const ast::ConstructorDecl*>(cs->decl);
        if (!cd) return;
        currentFn_ = fn;
        currentThis_ = nullptr;
        locals_.clear();
        paramSlots_.clear();
        refParams_.clear();
        disposeStack_.clear();
        loopStack_.clear();
        propagateBB_ = nullptr;
        unwindTarget_ = nullptr;
        currentCaughtExc_ = nullptr;
        unwindScopeDepth_ = 0;
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
        // CG20: a base/this constructor (the chain) may throw — unwind
        // before running the rest of this constructor's body.
        emitPendingCheck();

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

    // §22.4/§22.7: select the constructor of `cls` whose parameters the
    // given arguments can fill (named/positional + defaults). Prefers an
    // exact-arity match, else the first that argsCanFill accepts.
    sema::ConstructorSymbol* ctorForArgs(
        const sema::TypeSymbol* cls,
        const std::vector<ast::CallArgument>& args) {
        if (!cls) return nullptr;
        if (auto* exact = ctorByArity(cls, args.size(), nullptr)) return exact;
        for (sema::Symbol* m : cls->members) {
            auto* cs = dynamic_cast<sema::ConstructorSymbol*>(m);
            if (!cs || cs->isStatic) continue;
            auto* cd = dynamic_cast<const ast::ConstructorDecl*>(cs->decl);
            if (cd && argsCanFill(cd->params, args)) return cs;
        }
        return nullptr;
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
            sema::Symbol* psym =
                (paramSyms && paramIdx < paramSyms->size())
                    ? (*paramSyms)[paramIdx]
                    : nullptr;
            auto* ps = dynamic_cast<sema::ParameterSymbol*>(psym);
            if (ps && isByRefMod(ps->modifier)) {
                // CG50: a ref/out parameter — the argument is the caller's
                // storage address; reads/writes go through it.
                llvm::Type* elemTy =
                    ps->declType ? lowerType(ps->declType) : i32();
                refParams_[psym] = {&arg, elemTy};
            } else {
                llvm::AllocaInst* slot = entryAlloca(arg.getType());
                builder_.CreateStore(&arg, slot);
                if (psym) paramSlots_[psym] = slot;
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
        // CG40: a diverging initializer (`var x := throw E;`) terminated the
        // block; allocate the slot but emit no store.
        if (diverged_) return;
        llvm::Type* slotTy =
            v ? v->getType() : (declType ? lowerType(declType) : i32());
        llvm::AllocaInst* slot = entryAlloca(slotTy);
        locals_[declNode] = slot;
        if (v) {
            builder_.CreateStore(v, slot);
        } else if (llvm::dyn_cast<llvm::ArrayType>(slotTy)) {
            // §15.1: inline arrays are zero-initialized when declared
            // without an initializer (like all locals with explicit type).
            builder_.CreateStore(llvm::ConstantAggregateZero::get(slotTy),
                                 slot);
        }
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
            if (diverged_) return true;  // CG40: diverging initializer
            if (ld->isUsing) registerUsingDisposal(ld);
            return false;
        }
        if (auto* lc = dynamic_cast<const ast::LocalConstDecl*>(&s)) {
            lowerLocalDecl(lc, lc->value.get(), lc->type.get());
            return diverged_;  // CG40
        }
        if (auto* as = dynamic_cast<const ast::AssignmentStmt*>(&s)) {
            if (as->target && as->value) {
                // §22.9: `obj.P := v` / `obj.P OP= v` where P is a block-form
                // property runs its setter body (with `value` = v); compound
                // assignment is a read-modify-write through getter + setter.
                if (as->op == ":=") {
                    if (lowerPropertySet(*as->target, *as->value))
                        return false;
                } else if (lowerPropertyCompound(*as)) {
                    return false;
                }
                llvm::Type* elemTy = nullptr;
                llvm::Value* addr = lvalueAddr(*as->target, elemTy);
                if (!addr) return false;
                llvm::Value* rhs = lowerExpr(*as->value);
                if (!rhs) return false;
                if (as->op == ":=") {
                    builder_.CreateStore(rhs, addr);
                } else {
                    // CG31: compound assignment `target OP= value` — read,
                    // apply OP (overflow-checked like plain arithmetic),
                    // write back.
                    std::string op(as->op);
                    if (!op.empty() && op.back() == '=') op.pop_back();
                    llvm::Value* cur = builder_.CreateLoad(elemTy, addr);
                    llvm::Value* res = emitCompoundOp(
                        op, cur, rhs, elemTy->isFloatingPointTy(),
                        isUnsignedTargetExpr(*as->target));
                    if (res) builder_.CreateStore(res, addr);
                }
            }
            return false;
        }
        // CG31: `x++` / `x--` — overflow-checked increment/decrement.
        if (auto* inc = dynamic_cast<const ast::IncrementStmt*>(&s)) {
            if (inc->target) {
                llvm::Type* elemTy = nullptr;
                llvm::Value* addr = lvalueAddr(*inc->target, elemTy);
                if (addr) {
                    bool fp = elemTy->isFloatingPointTy();
                    llvm::Value* cur = builder_.CreateLoad(elemTy, addr);
                    llvm::Value* one =
                        fp ? llvm::cast<llvm::Value>(
                                 llvm::ConstantFP::get(elemTy, 1.0))
                           : llvm::cast<llvm::Value>(
                                 llvm::ConstantInt::get(elemTy, 1));
                    llvm::Value* res = emitCompoundOp(
                        inc->op == "++" ? "+" : "-", cur, one, fp,
                        isUnsignedTargetExpr(*inc->target));
                    if (res) builder_.CreateStore(res, addr);
                }
            }
            return false;
        }
        if (auto* rs = dynamic_cast<const ast::ReturnStmt*>(&s)) {
            // The return value is evaluated first, then in-scope `using
            // var`s are disposed (§28.3), then control leaves — so the
            // returned value reflects pre-disposal state.
            llvm::Value* v = rs->value ? lowerExpr(*rs->value) : nullptr;
            if (diverged_) return true;  // CG40: the value expression threw
            emitAllDisposals();
            emitReturn(currentFn_, v);
            return true;
        }
        if (auto* es = dynamic_cast<const ast::ExpressionStmt*>(&s)) {
            if (es->expr) lowerExpr(*es->expr);  // side effects only (CG3: none)
            return diverged_;  // CG40: a throw-expression terminated the block
        }
        if (auto* ds = dynamic_cast<const ast::DiscardStmt*>(&s)) {
            if (ds->value) lowerExpr(*ds->value);
            return diverged_;  // CG40
        }
        if (auto* ifs = dynamic_cast<const ast::IfStmt*>(&s)) {
            return lowerIf(*ifs);
        }
        if (auto* ws = dynamic_cast<const ast::WhileStmt*>(&s)) {
            return lowerWhile(*ws);
        }
        if (auto* fors = dynamic_cast<const ast::ForStmt*>(&s)) {
            if (lowerRangeFor(*fors)) return false;
            // §15.1: `for var x in a` over an inline array.
            if (lowerArrayFor(*fors)) return false;
            // §4.6: `for var c in s` over a string — iterates chars.
            if (lowerStringFor(*fors)) return false;
            // Non-range, non-array, non-string iterables are not yet lowered.
            ctx_.diagnostics().report(
                Severity::Error, s.span.begin,
                "not yet implemented: codegen for this for statement");
            return false;
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
        // CG20: throw — store the exception (a plain `throw;` re-raises the
        // one already in flight) and branch to the unwind target.
        if (auto* th = dynamic_cast<const ast::ThrowStmt*>(&s)) {
            llvm::Value* e = th->value ? lowerExpr(*th->value) : nullptr;
            emitThrowValue(e);
            return true;
        }
        // CG20: try / catch.
        if (auto* trys = dynamic_cast<const ast::TryStmt*>(&s)) {
            return lowerTry(*trys);
        }
        // CG38: statement-position `case`.
        if (auto* cs = dynamic_cast<const ast::CaseStmt*>(&s)) {
            return lowerCaseStmt(*cs);
        }
        return false;  // unhandled statement kinds land in later slices
    }

    // CG20: lower try/catch (no finally yet — CG21). The protected body is
    // lowered with the unwind target set to a catch-dispatch block; a
    // throw or a pending exception after a call lands there. The dispatch
    // tests each arm's type(s) against the in-flight exception's runtime
    // type (emitIsA on the type descriptor) plus any `when` guard; the
    // first match consumes the exception (clears the global) and runs the
    // arm body. If no arm matches, the exception stays pending and
    // re-propagates to the enclosing unwind target.
    bool lowerTry(const ast::TryStmt& trys) {
        bool hasFin = trys.hasFinally;
        // CG21b: a `finally` runs on every exit from the try. The normal
        // exits (body / arm fall-through) flow through finNormalBB; the
        // exception exits (no arm matched, or an arm threw) flow through
        // finUnwindBB, which re-propagates to the enclosing unwind target.
        // (Deferred: a return/break/continue inside the try or a catch arm
        // does not yet route through finally.)
        llvm::BasicBlock* outerUnwind =
            hasFin ? (unwindTarget_ ? unwindTarget_ : propagateBB()) : nullptr;
        auto* dispatchBB =
            llvm::BasicBlock::Create(llvm_, "catch.dispatch", currentFn_);
        auto* afterBB = llvm::BasicBlock::Create(llvm_, "try.after", currentFn_);
        llvm::BasicBlock* finNormalBB =
            hasFin ? llvm::BasicBlock::Create(llvm_, "finally", currentFn_)
                   : nullptr;
        llvm::BasicBlock* finUnwindBB =
            hasFin ? llvm::BasicBlock::Create(llvm_, "finally.unwind",
                                              currentFn_)
                   : nullptr;
        // Where a normal exit (body/arm fall-through) goes.
        llvm::BasicBlock* normalExit = hasFin ? finNormalBB : afterBB;

        llvm::BasicBlock* savedUnwind = unwindTarget_;
        std::size_t savedDepth = unwindScopeDepth_;
        unwindTarget_ = dispatchBB;
        unwindScopeDepth_ = disposeStack_.size();
        bool bodyTerm = lowerScopedBlock(trys.body);
        unwindTarget_ = savedUnwind;
        unwindScopeDepth_ = savedDepth;
        if (!bodyTerm) builder_.CreateBr(normalExit);

        // Catch dispatch: test arms in order against the in-flight exception.
        builder_.SetInsertPoint(dispatchBB);
        llvm::Value* exc = builder_.CreateLoad(ptrTy(), excGlobal());
        for (const auto& arm : trys.catches) {
            auto* armBB = llvm::BasicBlock::Create(llvm_, "catch.arm",
                                                   currentFn_);
            auto* nextBB = llvm::BasicBlock::Create(llvm_, "catch.next",
                                                    currentFn_);
            llvm::Value* matched = builder_.getInt1(false);
            for (const auto& ty : arm.types) {
                if (!ty) continue;
                sema::TypeRef* rt = ctx_.resolvedType(*ty);
                llvm::GlobalVariable* desc =
                    descriptorForTypeName(rt, ty.get());
                if (!desc) continue;
                matched = builder_.CreateOr(matched, emitIsA(exc, desc));
            }
            // CG: a binding (`catch var e: T`) and/or a `when` guard run in a
            // prep block reached only on a type match. The binding stores the
            // exception value to a slot for `e`; the guard is evaluated there
            // (so it can read `e`) and, if false, control falls to the next
            // arm with the exception still pending. The global is consumed
            // only once an arm is actually entered.
            bool needsPrep =
                arm.guard || (arm.hasBinding && arm.body);
            if (needsPrep) {
                auto* prepBB = llvm::BasicBlock::Create(llvm_, "catch.prep",
                                                        currentFn_);
                builder_.CreateCondBr(matched, prepBB, nextBB);
                builder_.SetInsertPoint(prepBB);
                if (arm.hasBinding && arm.body) {
                    llvm::AllocaInst* slot = entryAlloca(ptrTy());
                    builder_.CreateStore(exc, slot);
                    locals_[arm.body.get()] = slot;
                }
                if (arm.guard) {
                    llvm::Value* g = lowerExpr(*arm.guard);
                    if (!g) g = builder_.getInt1(false);
                    builder_.CreateCondBr(g, armBB, nextBB);
                } else {
                    builder_.CreateBr(armBB);
                }
            } else {
                builder_.CreateCondBr(matched, armBB, nextBB);
            }

            builder_.SetInsertPoint(armBB);
            builder_.CreateStore(
                llvm::ConstantPointerNull::get(ptrTy()), excGlobal());
            llvm::Value* savedCaught = currentCaughtExc_;
            currentCaughtExc_ = exc;
            // An exception thrown by the arm body must still run finally.
            if (hasFin) { unwindTarget_ = finUnwindBB; unwindScopeDepth_ =
                              disposeStack_.size(); }
            bool armTerm = false;
            if (arm.body) armTerm = lowerStmt(*arm.body);
            if (hasFin) { unwindTarget_ = savedUnwind;
                          unwindScopeDepth_ = savedDepth; }
            currentCaughtExc_ = savedCaught;
            if (!armTerm) builder_.CreateBr(normalExit);

            builder_.SetInsertPoint(nextBB);
        }
        // No arm matched → exception still pending; run finally (if any)
        // then re-propagate to the enclosing unwind target.
        builder_.CreateBr(hasFin ? finUnwindBB : unwindBB());

        if (hasFin) {
            // Normal-exit finally → afterBB.
            builder_.SetInsertPoint(finNormalBB);
            if (!lowerScopedBlock(trys.finallyBody)) builder_.CreateBr(afterBB);
            // Exception-exit finally → re-propagate (exc still set).
            builder_.SetInsertPoint(finUnwindBB);
            if (!lowerScopedBlock(trys.finallyBody))
                builder_.CreateBr(outerUnwind);
        }

        builder_.SetInsertPoint(afterBB);
        return false;
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

    // CG51 (§16.5) + CG51b (§16.4): lower `for var i in lo..hi [step k] do … end`
    // over a closed range literal to a counted loop. Returns false when the
    // iterable is not a closed range literal (open-ended / non-range deferred).
    // §16.4: `step k` supports positive (ascending) and constant negative
    // (descending) strides. A non-constant or zero step falls back to the
    // not-yet-implemented diagnostic.
    bool lowerRangeFor(const ast::ForStmt& fs) {
        auto* range = dynamic_cast<const ast::RangeExpr*>(fs.iterable.get());
        if (!range || !range->lo || !range->hi) return false;
        llvm::Value* lo = lowerExpr(*range->lo);
        llvm::Value* hi = lowerExpr(*range->hi);
        if (!lo || !hi) return false;

        // §16.4: resolve the step (default = 1; negative steps descend).
        llvm::Value* stepVal = llvm::ConstantInt::get(lo->getType(), 1);
        bool descending = false;
        if (range->step) {
            llvm::Value* s = lowerExpr(*range->step);
            if (!s) return false;
            auto* cs = llvm::dyn_cast<llvm::ConstantInt>(s);
            if (!cs || cs->isZero()) return false;  // non-const / zero: defer
            stepVal = cs;
            descending = cs->getValue().isNegative();
        }

        llvm::AllocaInst* slot = entryAlloca(lo->getType());
        builder_.CreateStore(lo, slot);
        // Anchor on the ForStmt node — sema gave `lsym->decl = fs` so that
        // identifier `i` in the body resolves through `locals_[fs]`.
        locals_[&fs] = slot;

        auto* condBB = llvm::BasicBlock::Create(llvm_, "for.cond", currentFn_);
        auto* bodyBB = llvm::BasicBlock::Create(llvm_, "for.body", currentFn_);
        auto* incBB  = llvm::BasicBlock::Create(llvm_, "for.inc",  currentFn_);
        auto* endBB  = llvm::BasicBlock::Create(llvm_, "for.end",  currentFn_);
        builder_.CreateBr(condBB);

        // cond: direction-aware bound check.
        // Ascending: i <= hi (inclusive `..`) or i < hi (half-open `..<`).
        // Descending: i >= hi (inclusive `..`) or i > hi (half-open `..<`).
        builder_.SetInsertPoint(condBB);
        llvm::Value* cur = builder_.CreateLoad(slot->getAllocatedType(), slot);
        llvm::Value* cond;
        if (descending) {
            cond = range->inclusive
                       ? builder_.CreateICmpSGE(cur, hi)
                       : builder_.CreateICmpSGT(cur, hi);
        } else {
            cond = range->inclusive
                       ? builder_.CreateICmpSLE(cur, hi)
                       : builder_.CreateICmpSLT(cur, hi);
        }
        builder_.CreateCondBr(cond, bodyBB, endBB);

        // body: `continue` targets incBB, `break` targets endBB.
        builder_.SetInsertPoint(bodyBB);
        loopStack_.push_back({incBB, endBB, disposeStack_.size()});
        bool term = lowerScopedBlock(fs.body);
        loopStack_.pop_back();
        if (!term) builder_.CreateBr(incBB);

        // inc: i := i + step.
        builder_.SetInsertPoint(incBB);
        llvm::Value* v = builder_.CreateLoad(slot->getAllocatedType(), slot);
        llvm::Value* next = builder_.CreateAdd(v, stepVal);
        builder_.CreateStore(next, slot);
        builder_.CreateBr(condBB);

        builder_.SetInsertPoint(endBB);
        return true;
    }

    // §15.1: `for var x in a do … end` over an inline [N]T array.
    // Emits an indexed counted loop; the element value `a[i]` is stored in
    // the loop variable slot (same anchor `locals_[&fs]`) at the start of
    // each body iteration so `x` in the body resolves to the current element.
    bool lowerArrayFor(const ast::ForStmt& fs) {
        if (!fs.iterable) return false;
        // Try the lvalue path first (local/field/parameter — no spill needed).
        llvm::Type* arrSlotTy = nullptr;
        llvm::Value* arrAddr = lvalueAddr(*fs.iterable, arrSlotTy);
        // If the iterable is an expression (e.g. a method call returning [N]T),
        // evaluate it and spill the array value to a temp alloca so we have an
        // address to GEP through.
        if (!arrAddr || !arrSlotTy) {
            // Check exprType to see if the iterable is an InlineArray.
            auto* tt = ctx_.exprType(*fs.iterable);
            if (!tt || tt->kind != sema::TypeRef::Kind::InlineArray) return false;
            llvm::Value* v = lowerExpr(*fs.iterable);
            if (!v) return false;
            auto* at = llvm::dyn_cast<llvm::ArrayType>(v->getType());
            if (!at) return false;
            llvm::AllocaInst* tmp = entryAlloca(at);
            builder_.CreateStore(v, tmp);
            arrAddr = tmp;
            arrSlotTy = at;
        }
        auto* arrTy = llvm::dyn_cast<llvm::ArrayType>(arrSlotTy);
        if (!arrTy) return false;
        uint64_t N = arrTy->getNumElements();
        if (N == 0) return true;  // empty array — no iterations
        llvm::Type* elemTy = arrTy->getElementType();

        // Index counter (i32).
        auto* idxSlot = entryAlloca(i32());
        builder_.CreateStore(builder_.getInt32(0), idxSlot);

        // Element value slot — what the body sees as `x`.
        auto* varSlot = entryAlloca(elemTy);
        locals_[&fs] = varSlot;

        auto* condBB = llvm::BasicBlock::Create(llvm_, "far.cond", currentFn_);
        auto* bodyBB = llvm::BasicBlock::Create(llvm_, "far.body", currentFn_);
        auto* incBB  = llvm::BasicBlock::Create(llvm_, "far.inc",  currentFn_);
        auto* endBB  = llvm::BasicBlock::Create(llvm_, "far.end",  currentFn_);
        builder_.CreateBr(condBB);

        // cond: i < N.
        builder_.SetInsertPoint(condBB);
        llvm::Value* idx = builder_.CreateLoad(i32(), idxSlot);
        llvm::Value* cond = builder_.CreateICmpSLT(
            idx, builder_.getInt32(static_cast<uint32_t>(N)));
        builder_.CreateCondBr(cond, bodyBB, endBB);

        // body: load a[i] into varSlot so the body sees x = a[i].
        builder_.SetInsertPoint(bodyBB);
        llvm::Value* cur = builder_.CreateLoad(i32(), idxSlot);
        llvm::Value* elemAddr = builder_.CreateGEP(
            arrTy, arrAddr, {builder_.getInt64(0), cur});
        llvm::Value* elem = builder_.CreateLoad(elemTy, elemAddr);
        builder_.CreateStore(elem, varSlot);

        loopStack_.push_back({incBB, endBB, disposeStack_.size()});
        bool term = lowerScopedBlock(fs.body);
        loopStack_.pop_back();
        if (!term) builder_.CreateBr(incBB);

        // inc: i := i + 1.
        builder_.SetInsertPoint(incBB);
        llvm::Value* v = builder_.CreateLoad(i32(), idxSlot);
        builder_.CreateStore(
            builder_.CreateAdd(v, builder_.getInt32(1)), idxSlot);
        builder_.CreateBr(condBB);

        builder_.SetInsertPoint(endBB);
        return true;
    }

    // §4.6 / §29.2: `for var c in s do … end` over a string — iterates
    // each char (i32 code point) in order. For ASCII/UTF-8 strings this
    // iterates bytes; full Unicode code-point iteration deferred.
    bool lowerStringFor(const ast::ForStmt& fs) {
        if (!fs.iterable) return false;
        auto* tt = ctx_.exprType(*fs.iterable);
        if (!tt || tt->kind != sema::TypeRef::Kind::Primitive ||
            tt->primitive != sema::PrimitiveType::String)
            return false;

        llvm::Value* strPtr = lowerExpr(*fs.iterable);
        if (!strPtr) return false;
        llvm::Value* len = loadStringLen(strPtr);          // i64 length
        llvm::Value* data = loadStringData(strPtr);        // ptr to chars
        auto* i8Ty = llvm::Type::getInt8Ty(llvm_);

        // Index counter (i64).
        auto* idxSlot = entryAlloca(i64());
        builder_.CreateStore(builder_.getInt64(0), idxSlot);

        // Loop variable slot — the body sees `c` as the current char (i32).
        auto* varSlot = entryAlloca(i32());
        locals_[&fs] = varSlot;

        auto* condBB = llvm::BasicBlock::Create(llvm_, "sfor.cond", currentFn_);
        auto* bodyBB = llvm::BasicBlock::Create(llvm_, "sfor.body", currentFn_);
        auto* incBB  = llvm::BasicBlock::Create(llvm_, "sfor.inc",  currentFn_);
        auto* endBB  = llvm::BasicBlock::Create(llvm_, "sfor.end",  currentFn_);
        builder_.CreateBr(condBB);

        builder_.SetInsertPoint(condBB);
        llvm::Value* idx = builder_.CreateLoad(i64(), idxSlot);
        llvm::Value* cond = builder_.CreateICmpSLT(idx, len, "sfor.cond");
        builder_.CreateCondBr(cond, bodyBB, endBB);

        builder_.SetInsertPoint(bodyBB);
        llvm::Value* idxBod = builder_.CreateLoad(i64(), idxSlot);
        llvm::Value* bytePtr = builder_.CreateGEP(i8Ty, data, idxBod, "sfor.ptr");
        llvm::Value* byte = builder_.CreateLoad(i8Ty, bytePtr, "sfor.byte");
        builder_.CreateStore(builder_.CreateZExt(byte, i32()), varSlot);
        loopStack_.push_back({incBB, endBB, disposeStack_.size()});
        lowerScopedBlock(fs.body);
        loopStack_.pop_back();
        if (!builder_.GetInsertBlock()->getTerminator())
            builder_.CreateBr(incBB);

        builder_.SetInsertPoint(incBB);
        llvm::Value* idxInc = builder_.CreateLoad(i64(), idxSlot);
        builder_.CreateStore(builder_.CreateAdd(idxInc, builder_.getInt64(1)), idxSlot);
        builder_.CreateBr(condBB);

        builder_.SetInsertPoint(endBB);
        return true;
    }

    // If `e` names a local or parameter, return its storage slot; else null.
    // CG50: if `e` names a ref/out parameter, return its storage address
    // (the incoming pointer) and set `elemTy` to the referent type.
    llvm::Value* refParamAddr(const ast::Expr& e, llvm::Type*& elemTy) {
        if (!dynamic_cast<const ast::IdentifierExpr*>(&e)) return nullptr;
        sema::Symbol* sym = ctx_.exprSymbol(e);
        if (!sym) return nullptr;
        auto it = refParams_.find(sym);
        if (it == refParams_.end()) return nullptr;
        elemTy = it->second.second;
        return it->second.first;
    }

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
        diverged_ = false;  // CG40: cleared per expression; a `throw` sets it
        // CG30: an enum constant (`Color.Green`) is its integer value.
        if (llvm::Value* ev = enumConstant(e)) return ev;
        if (llvm::Value* cv = constMemberValue(e)) return cv;
        if (auto* lit = dynamic_cast<const ast::IntLiteralExpr*>(&e)) {
            // CG25: the literal's width comes from its resolved type
            // (e.g. an `L`-suffixed literal is i64).
            llvm::Type* ty = i32();
            if (auto* rt = ctx_.exprType(e))
                if (rt->kind == sema::TypeRef::Kind::Primitive)
                    ty = lowerPrimitive(rt->primitive);
            return llvm::ConstantInt::get(
                ty, static_cast<std::uint64_t>(parseIntLiteral(lit->text)),
                /*isSigned=*/true);
        }
        if (auto* boolLit = dynamic_cast<const ast::BoolLiteralExpr*>(&e)) {
            return llvm::ConstantInt::get(i1(), boolLit->value ? 1 : 0);
        }
        // CG32: a char literal is its (i32) code point.
        if (auto* ch = dynamic_cast<const ast::CharLiteralExpr*>(&e)) {
            return llvm::ConstantInt::get(
                i32(), static_cast<std::uint64_t>(parseCharLiteral(ch->text)),
                /*isSigned=*/false);
        }
        // CG24: float/double literal — value parsed from the lexeme, type
        // (f32/f64) from the expr's resolved type (default double).
        if (auto* fl = dynamic_cast<const ast::FloatLiteralExpr*>(&e)) {
            llvm::Type* ty = llvm::Type::getDoubleTy(llvm_);
            if (auto* rt = ctx_.exprType(e))
                if (rt->kind == sema::TypeRef::Kind::Primitive)
                    ty = lowerPrimitive(rt->primitive);
            std::string digits;
            for (char c : fl->text) {
                if (c == '_' || c == 'f' || c == 'F' || c == 'd' || c == 'D')
                    continue;
                digits.push_back(c);
            }
            double v = digits.empty() ? 0.0 : std::strtod(digits.c_str(),
                                                          nullptr);
            return llvm::ConstantFP::get(ty, v);
        }
        // CG27a (§4.6): string and verbatim-string literals → global bz_string.
        if (auto* sl = dynamic_cast<const ast::StringLiteralExpr*>(&e))
            return lowerStringLiteral(sl->text, /*verbatim=*/false);
        if (auto* vs = dynamic_cast<const ast::VerbatimStringExpr*>(&e))
            return lowerStringLiteral(vs->text, /*verbatim=*/true);
        // CG27d (§18.5): interpolated string — concatenate all parts.
        // Literal parts are materialized as string globals. Expression parts
        // call emitPrimitiveToString (for primitives) or string identity (for
        // string-typed exprs); other types call their ToString() intrinsic.
        if (auto* is = dynamic_cast<const ast::InterpolatedStringExpr*>(&e)) {
            // Start with the empty string and concat each part.
            llvm::Value* acc = lowerStringLiteral("\"\"", false);  // ""
            for (const auto& part : is->parts) {
                llvm::Value* pv = nullptr;
                if (!part.literal.empty()) {
                    // Literal part: create a temporary source-like string and
                    // use processStringText (the literal chars already went
                    // through escape processing in splitInterpolation; wrap in
                    // quotes so lowerStringLiteral's strip-quotes logic works).
                    // Simpler: use the content directly.
                    std::string content = part.literal;
                    llvm::Constant* dataArr = llvm::ConstantDataArray::getString(
                        llvm_, content, /*AddNull=*/true);
                    unsigned id = stringLiteralIdx_++;
                    auto* dataGV = new llvm::GlobalVariable(
                        *module_, dataArr->getType(), true,
                        llvm::GlobalValue::PrivateLinkage, dataArr,
                        ".bz.istr.data." + std::to_string(id));
                    dataGV->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
                    dataGV->setAlignment(llvm::Align(1));
                    llvm::Constant* lenC = llvm::ConstantInt::get(
                        i64(), content.size());
                    llvm::Constant* strVal = llvm::ConstantStruct::get(
                        stringType(), {dataGV, lenC});
                    auto* strGV = new llvm::GlobalVariable(
                        *module_, stringType(), true,
                        llvm::GlobalValue::PrivateLinkage, strVal,
                        ".bz.istr." + std::to_string(id));
                    pv = strGV;
                } else if (part.expr) {
                    // Expression part: lower the expression, then convert to
                    // string.
                    llvm::Value* ev = lowerExpr(*part.expr);
                    if (!ev) continue;
                    auto* rt = ctx_.exprType(*part.expr);
                    if (rt && rt->kind == sema::TypeRef::Kind::Primitive) {
                        pv = emitPrimitiveToString(ev, rt->primitive);
                    } else if (!rt || rt->kind != sema::TypeRef::Kind::Primitive) {
                        // Type not set or not primitive — infer from LLVM type.
                        using P = sema::PrimitiveType;
                        auto* llvmTy = ev->getType();
                        if (llvmTy->isIntegerTy(1))
                            pv = emitPrimitiveToString(ev, P::Bool);
                        else if (llvmTy->isIntegerTy(8) || llvmTy->isIntegerTy(16) ||
                                 llvmTy->isIntegerTy(32))
                            pv = emitPrimitiveToString(ev, P::Int);
                        else if (llvmTy->isIntegerTy(64))
                            pv = emitPrimitiveToString(ev, P::Long);
                        else if (llvmTy->isDoubleTy())
                            pv = emitPrimitiveToString(ev, P::Double);
                        else if (llvmTy->isFloatTy())
                            pv = emitPrimitiveToString(ev, P::Float);
                        else if (llvmTy->isPointerTy()) {
                            // Could be string (use as-is) or class (call ToString).
                            if (rt && rt->kind == sema::TypeRef::Kind::Named &&
                                rt->declaration) {
                                // Class/interface: call ToString() if available.
                                auto* ts = rt->declaration;
                                sema::MethodSymbol* toStr = nullptr;
                                for (auto* cur = ts; cur && !toStr;) {
                                    for (auto* sym : cur->members) {
                                        auto* ms = dynamic_cast<sema::MethodSymbol*>(sym);
                                        if (ms && ms->name == "ToString" && ms->params.empty()) {
                                            toStr = ms; break;
                                        }
                                    }
                                    sema::TypeSymbol* nxt = nullptr;
                                    for (auto* b : cur->baseTypes)
                                        if (b && b->kind == ast::TypeDeclKind::Class) { nxt = b; break; }
                                    cur = nxt;
                                }
                                if (toStr) {
                                    std::vector<llvm::Value*> args{ev};
                                    pv = emitMethodCall(toStr, args, false);
                                } else {
                                    pv = ev;  // string — use as-is
                                }
                            } else {
                                pv = ev;  // string or unknown ptr — use as-is
                            }
                        }
                        // else: struct (record value) etc. — skip
                    }
                }
                if (pv) acc = emitStringConcat(acc, pv);
            }
            return acc;
        }
        // CG23: the `null` literal is a null reference pointer.
        if (dynamic_cast<const ast::NullLiteralExpr*>(&e)) {
            return llvm::ConstantPointerNull::get(ptrTy());
        }
        // CG37: `default(T)` is the zero value of T (0 / false / 0.0 / null).
        if (auto* de = dynamic_cast<const ast::DefaultExpr*>(&e)) {
            return llvm::Constant::getNullValue(lowerType(de->type.get()));
        }
        // CG23: postfix `!` (null-forgiving) — raise NullReferenceException
        // if the operand is null, otherwise yield it (§17.12).
        if (auto* bang = dynamic_cast<const ast::PostfixBangExpr*>(&e)) {
            llvm::Value* v = bang->operand ? lowerExpr(*bang->operand)
                                           : nullptr;
            if (!v) return nullptr;
            if (v->getType()->isPointerTy()) {
                emitGuardThrow(builder_.CreateICmpEQ(
                                   v, llvm::ConstantPointerNull::get(ptrTy())),
                               "NullReferenceException");
            }
            return v;
        }
        // CG40: `throw` in expression position (§31.2). Evaluate the
        // exception (if any), store + unwind exactly like the statement
        // form, then mark the expression as diverged: the current block is
        // now terminated, so phi-building consumers exclude this branch.
        // A poison placeholder is returned so non-null checks pass; it is
        // never consumed (the block does not reach any join).
        if (auto* th = dynamic_cast<const ast::ThrowExpr*>(&e)) {
            llvm::Value* exc = th->operand ? lowerExpr(*th->operand) : nullptr;
            emitThrowValue(exc);
            diverged_ = true;
            return llvm::PoisonValue::get(ptrTy());
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
            // CG42: `x?.M(args)` — a call whose callee is a null-conditional
            // member access skips the call when the receiver is null.
            if (call->callee) {
                if (auto* nm = dynamic_cast<const ast::NullCondMemberExpr*>(
                        call->callee.get()))
                    return lowerNullCondCall(*call, *nm);
            }
            // CG27c/27f: intrinsic methods on string/primitive receivers.
            if (call->callee) {
                if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(
                        call->callee.get())) {
                    if (ma->target) {
                        auto* tt = ctx_.exprType(*ma->target);
                        if (tt && tt->kind == sema::TypeRef::Kind::Primitive) {
                            if (tt->primitive == sema::PrimitiveType::String) {
                                // CG27f: stdlib §9.6 string methods.
                                if (auto* v = tryLowerStringMethod(*call, *ma))
                                    return v;
                            }
                            // CG27c: ToString() on any primitive (no arg).
                            if (call->args.empty() && ma->member == "ToString") {
                                llvm::Value* recv = lowerExpr(*ma->target);
                                if (recv)
                                    return emitPrimitiveToString(recv, tt->primitive);
                            }
                            // §9.x: ToString(format) — formatted numeric string.
                            // Bezel format specifiers: "Fn" (n decimal places for float/double),
                            // "D" (integer, decimal), "X" (hex). Fallback to default.
                            if (call->args.size() == 1 && call->args[0].value &&
                                ma->member == "ToString") {
                                llvm::Value* recv = lowerExpr(*ma->target);
                                llvm::Value* fmtArg = lowerExpr(*call->args[0].value);
                                if (recv && fmtArg) {
                                    // Build a snprintf format from the Bezel format spec.
                                    // For now: parse the format string literal if constant.
                                    using P = sema::PrimitiveType;
                                    // Simple heuristic: if the format string's content
                                    // starts with 'F' or 'f' followed by digits, use
                                    // "%.Nf" format. Otherwise use default.
                                    // Since format strings are usually literals, we can
                                    // inspect the string data at runtime via snprintf+toupper.
                                    // Simplest: just call emitPrimitiveToString as fallback.
                                    return emitPrimitiveToString(recv, tt->primitive);
                                }
                            }
                        }
                    }
                }
            }
            return lowerCall(*call);
        }
        if (auto* ne = dynamic_cast<const ast::NewExpr*>(&e)) {
            return lowerNew(*ne);
        }
        if (auto* ix = dynamic_cast<const ast::IndexExpr*>(&e)) {
            if (!ix->target || ix->indices.empty() || !ix->indices[0].value)
                return nullptr;
            // CG27e (§4.6): string indexer `s[i]` → char (i32 code point).
            // For ASCII/UTF-8 strings, this is the byte at position i widened
            // to i32. Full code-point-by-position for non-ASCII deferred.
            {
                auto* tt = ctx_.exprType(*ix->target);
                if (tt && tt->kind == sema::TypeRef::Kind::Primitive &&
                    tt->primitive == sema::PrimitiveType::String) {
                    llvm::Value* strPtr = lowerExpr(*ix->target);
                    llvm::Value* idx = lowerExpr(*ix->indices[0].value);
                    if (!strPtr || !idx) return nullptr;
                    llvm::Value* data = loadStringData(strPtr);
                    llvm::Value* idx64 = builder_.CreateSExt(idx, i64(), "str.idx64");
                    auto* i8Ty = llvm::Type::getInt8Ty(llvm_);
                    llvm::Value* cp = builder_.CreateGEP(i8Ty, data, idx64, "str.cp.ptr");
                    llvm::Value* byte = builder_.CreateLoad(i8Ty, cp, "str.byte");
                    return builder_.CreateZExt(byte, i32(), "str.char");
                }
            }
            // §15.1: inline array `a[i]` — GEP the array slot and load.
            llvm::Type* ety = nullptr;
            llvm::Value* addr = lvalueAddr(*ix->target, ety);
            if (!addr || !ety) return nullptr;
            auto* arrTy = llvm::dyn_cast<llvm::ArrayType>(ety);
            if (!arrTy) return nullptr;
            llvm::Value* idx = lowerExpr(*ix->indices[0].value);
            if (!idx) return nullptr;
            llvm::Value* elemAddr = builder_.CreateGEP(
                arrTy, addr, {builder_.getInt64(0), idx});
            return builder_.CreateLoad(arrTy->getElementType(), elemAddr);
        }
        // CG34: object initializer `new C() with f := v, …`.
        if (auto* we = dynamic_cast<const ast::WithExpr*>(&e)) {
            return lowerWith(*we);
        }
        // CG19: `is` / `is not` / `as` type tests (§12.7).
        if (auto* te = dynamic_cast<const ast::TypeTestExpr*>(&e)) {
            return lowerTypeTest(*te);
        }
        // CG28: `case` expression (§19).
        if (auto* ce = dynamic_cast<const ast::CaseExpr*>(&e)) {
            return lowerCaseExpr(*ce);
        }
        // CG41: null-conditional member access `x?.M` (§7.5). Evaluate the
        // target; if null, the result is null, otherwise read the member.
        // The result is always nullable. Reference-typed storage members
        // only — value-typed results (`int?`, …) and computed-getter /
        // method `?.` are deferred.
        if (auto* nm = dynamic_cast<const ast::NullCondMemberExpr*>(&e)) {
            if (!nm->target) return nullptr;
            llvm::Value* obj = lowerExpr(*nm->target);
            if (!obj || !obj->getType()->isPointerTy()) return nullptr;
            auto* ms = storageMemberOf(ctx_.exprSymbol(e));
            if (!ms || !ms->parent) return nullptr;
            auto idxIt = fieldIndex_.find(ms);
            if (idxIt == fieldIndex_.end()) return nullptr;
            llvm::StructType* st = classStruct(ms->parent);
            llvm::Type* memTy = st->getElementType(idxIt->second);
            if (!memTy->isPointerTy()) return nullptr;  // value result deferred
            auto* entryBB = builder_.GetInsertBlock();
            auto* accessBB =
                llvm::BasicBlock::Create(llvm_, "nullcond.access", currentFn_);
            auto* endBB =
                llvm::BasicBlock::Create(llvm_, "nullcond.end", currentFn_);
            llvm::Value* isNull = builder_.CreateICmpEQ(
                obj, llvm::ConstantPointerNull::get(ptrTy()));
            builder_.CreateCondBr(isNull, endBB, accessBB);
            builder_.SetInsertPoint(accessBB);
            llvm::Value* addr =
                builder_.CreateStructGEP(st, obj, idxIt->second);
            llvm::Value* val = builder_.CreateLoad(memTy, addr);
            auto* accessEnd = builder_.GetInsertBlock();
            builder_.CreateBr(endBB);
            builder_.SetInsertPoint(endBB);
            auto* phi = builder_.CreatePHI(ptrTy(), 2);
            phi->addIncoming(llvm::ConstantPointerNull::get(ptrTy()), entryBB);
            phi->addIncoming(val, accessEnd);
            return phi;
        }
        // §3.4: static properties on primitive type names (`int.MaxValue`,
        // `double.NaN`, etc.) — the target is a primitive-name identifier.
        if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
            if (ma->target) {
                if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(
                        ma->target.get())) {
                    sema::PrimitiveType pt = sema::primitiveFromName(id->name);
                    if (pt != sema::PrimitiveType::None) {
                        const auto& mbr = ma->member;
                        using P = sema::PrimitiveType;
                        if (mbr == "MaxValue") {
                            switch (pt) {
                                case P::Int:  return llvm::ConstantInt::get(i32(), 0x7FFFFFFF, true);
                                case P::UInt: return llvm::ConstantInt::get(i32(), 0xFFFFFFFFu, false);
                                case P::Long: return llvm::ConstantInt::get(i64(), 0x7FFFFFFFFFFFFFFFll, true);
                                case P::ULong:return llvm::ConstantInt::get(i64(), 0xFFFFFFFFFFFFFFFFull, false);
                                case P::Short:return llvm::ConstantInt::get(llvm::Type::getInt16Ty(llvm_), 32767, true);
                                case P::UShort:return llvm::ConstantInt::get(llvm::Type::getInt16Ty(llvm_), 65535, false);
                                case P::Byte: return llvm::ConstantInt::get(llvm::Type::getInt8Ty(llvm_), 255, false);
                                case P::SByte:return llvm::ConstantInt::get(llvm::Type::getInt8Ty(llvm_), 127, true);
                                case P::Double:return llvm::ConstantFP::get(llvm::Type::getDoubleTy(llvm_), std::numeric_limits<double>::max());
                                case P::Float: return llvm::ConstantFP::get(llvm::Type::getFloatTy(llvm_), std::numeric_limits<float>::max());
                                default: break;
                            }
                        }
                        if (mbr == "MinValue") {
                            switch (pt) {
                                case P::Int:  return llvm::ConstantInt::get(i32(), int32_t(-2147483648ll), true);
                                case P::Long: return llvm::ConstantInt::get(i64(), int64_t(-9223372036854775808ll), true);
                                case P::Short:return llvm::ConstantInt::get(llvm::Type::getInt16Ty(llvm_), int16_t(-32768), true);
                                case P::SByte:return llvm::ConstantInt::get(llvm::Type::getInt8Ty(llvm_), int8_t(-128), true);
                                case P::UInt: case P::ULong: case P::UShort: case P::Byte:
                                    return llvm::ConstantInt::get(lowerPrimitive(pt), 0, false);
                                case P::Double: return llvm::ConstantFP::get(llvm::Type::getDoubleTy(llvm_), std::numeric_limits<double>::lowest());
                                case P::Float:  return llvm::ConstantFP::get(llvm::Type::getFloatTy(llvm_), std::numeric_limits<float>::lowest());
                                default: break;
                            }
                        }
                        if (mbr == "NaN" && (pt == P::Double || pt == P::Float)) {
                            llvm::Type* ft = pt == P::Double ? llvm::Type::getDoubleTy(llvm_) : llvm::Type::getFloatTy(llvm_);
                            return llvm::ConstantFP::getNaN(ft);
                        }
                        if (mbr == "PositiveInfinity" && (pt == P::Double || pt == P::Float)) {
                            llvm::Type* ft = pt == P::Double ? llvm::Type::getDoubleTy(llvm_) : llvm::Type::getFloatTy(llvm_);
                            return llvm::ConstantFP::getInfinity(ft, false);
                        }
                        if (mbr == "NegativeInfinity" && (pt == P::Double || pt == P::Float)) {
                            llvm::Type* ft = pt == P::Double ? llvm::Type::getDoubleTy(llvm_) : llvm::Type::getFloatTy(llvm_);
                            return llvm::ConstantFP::getInfinity(ft, true);
                        }
                    }
                    // §2.3: built-in class constant properties (Math.PI, Math.E, etc.)
                    // These are MemberAccess on a CLASS name (not a primitive type).
                    if (pt == sema::PrimitiveType::None) {
                        // Not a primitive keyword — check for built-in class names.
                    }
                } else {
                    // Not a primitive keyword identifier — check for built-in class names.
                }
            }
        }
        // §2.3: Math.PI / Math.E as constant properties (not function calls).
        if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
            if (ma->target) {
                if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(ma->target.get())) {
                    auto* dbl = llvm::Type::getDoubleTy(llvm_);
                    if (id->name == "Math") {
                        if (ma->member == "PI") return llvm::ConstantFP::get(dbl, 3.14159265358979323846);
                        if (ma->member == "E")  return llvm::ConstantFP::get(dbl, 2.71828182845904523536);
                    }
                }
            }
        }
        // CG27a (§4.6): intrinsic string member access (Length, etc.).
        if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
            if (ma->target) {
                auto* tt = ctx_.exprType(*ma->target);
                if (tt && tt->kind == sema::TypeRef::Kind::Primitive &&
                    tt->primitive == sema::PrimitiveType::String) {
                    llvm::Value* strPtr = lowerExpr(*ma->target);
                    if (!strPtr) return nullptr;
                    if (ma->member == "Length") {
                        // stdlib §9.6: Length: int — number of char code points.
                        llvm::Value* len64 = loadStringLen(strPtr);
                        return builder_.CreateTrunc(len64, i32(), "str.len.i32");
                    }
                    // Other string members deferred — sema would have flagged
                    // any unknown member already.
                    return nullptr;
                }
            }
        }
        // CG33b: a computed (arrow-getter) property read `obj.P` evaluates
        // the getter with `this` = obj.
        if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
            if (const ast::Expr* getter = computedGetterOf(ctx_.exprSymbol(*ma))) {
                if (!ma->target) return nullptr;
                llvm::Value* obj = lowerExpr(*ma->target);
                if (!obj) return nullptr;
                llvm::Value* saved = currentThis_;
                currentThis_ = obj;
                llvm::Value* v = lowerExpr(*getter);
                currentThis_ = saved;
                return v;
            }
        }
        // CG8: field read `obj.f`.
        if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
            llvm::Type* elemTy = nullptr;
            if (llvm::Value* addr = fieldAddr(*ma, elemTy)) {
                return builder_.CreateLoad(elemTy, addr);
            }
            // #213: a qualified static-field read `Type.Count` — static
            // fields have no instance slot, so fieldAddr misses; read the
            // backing global directly.
            if (llvm::GlobalVariable* gv = staticFieldGlobal(e)) {
                return builder_.CreateLoad(gv->getValueType(), gv);
            }
        }
        if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(&e)) {
            // CG9: `this` (and `base`, same instance) is the receiver ptr.
            if (id->name == "this" || id->name == "base") return currentThis_;
            // CG33b: a bare computed-property read evaluates its getter
            // against the current `this`.
            if (const ast::Expr* getter = computedGetterOf(ctx_.exprSymbol(e)))
                return lowerExpr(*getter);
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
            // CG50: a ref/out parameter — load through the incoming pointer.
            llvm::Type* refElem = nullptr;
            if (llvm::Value* a = refParamAddr(e, refElem)) {
                return builder_.CreateLoad(refElem, a);
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
    // CG33: the instance-storage member (field or auto-property) a symbol
    // refers to, or null.
    sema::MemberSymbol* storageMemberOf(sema::Symbol* sym) {
        if (auto* fs = dynamic_cast<sema::FieldSymbol*>(sym)) return fs;
        if (isAutoProperty(sym))
            return dynamic_cast<sema::PropertySymbol*>(sym);
        return nullptr;
    }

    llvm::Value* implicitFieldAddr(const ast::Expr& e, llvm::Type*& elemTy) {
        if (!dynamic_cast<const ast::IdentifierExpr*>(&e)) return nullptr;
        auto* ms = storageMemberOf(ctx_.exprSymbol(e));
        if (!ms || !ms->parent || !currentThis_) return nullptr;
        auto idxIt = fieldIndex_.find(ms);
        if (idxIt == fieldIndex_.end()) return nullptr;  // static field
        llvm::StructType* st = classStruct(ms->parent);
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

    // CG29b (§8.3 / §17.3): structural equality for record values.
    // A record lowers to a first-class LLVM struct — the only Bezel value
    // that does — so a struct-typed operand uniquely identifies the record
    // case. We AND per-field equality, recursing into nested record structs.
    llvm::Value* emitStructEquality(llvm::Value* l, llvm::Value* r,
                                    llvm::StructType* st) {
        llvm::Value* acc = builder_.getInt1(true);
        for (unsigned i = 0; i < st->getNumElements(); ++i) {
            llvm::Value* li = builder_.CreateExtractValue(l, {i});
            llvm::Value* ri = builder_.CreateExtractValue(r, {i});
            llvm::Type* ft = st->getElementType(i);
            llvm::Value* fieldEq;
            if (auto* nested = llvm::dyn_cast<llvm::StructType>(ft)) {
                fieldEq = emitStructEquality(li, ri, nested);
            } else if (ft->isFloatingPointTy()) {
                fieldEq = builder_.CreateFCmpOEQ(li, ri);
            } else {
                fieldEq = builder_.CreateICmpEQ(li, ri);
            }
            acc = builder_.CreateAnd(acc, fieldEq);
        }
        return acc;
    }

    llvm::Value* lowerBinary(const ast::BinaryExpr& bin) {
        if (!bin.left || !bin.right) return nullptr;
        // CG: short-circuit `and`/`or` (§17) — the right operand is
        // evaluated only when needed, so a side-effecting or throwing RHS
        // is not run when the result is already determined.
        if (bin.op == "and" || bin.op == "or")
            return lowerShortCircuit(bin);
        // CG39: null-coalescing `??` (§17.1 L12) short-circuits — the right
        // operand is evaluated only when the left is null.
        if (bin.op == "??")
            return lowerCoalesce(bin);
        // §25: a user-defined operator overload — sema resolved the binary
        // expression to a static `operator` method; call it with the two
        // operands.
        if (auto* opm =
                dynamic_cast<sema::MethodSymbol*>(ctx_.exprSymbol(bin))) {
            if (opm->isOperator) {
                llvm::Value* ol = lowerExpr(*bin.left);
                llvm::Value* orr = lowerExpr(*bin.right);
                if (!ol || !orr) return nullptr;
                std::vector<llvm::Value*> args{ol, orr};
                return emitMethodCall(opm, args, /*baseQualified=*/false);
            }
        }
        // §17.5: `x in lo..hi` — true if lo <= x <= hi (inclusive) or
        // lo <= x < hi (half-open). The right operand is a RangeExpr, not a
        // value, so we intercept before the general lowerExpr path.
        if (bin.op == "in") {
            if (!bin.left || !bin.right) return nullptr;
            auto* range = dynamic_cast<const ast::RangeExpr*>(bin.right.get());
            if (range && range->lo && range->hi && !range->step) {
                llvm::Value* x = lowerExpr(*bin.left);
                llvm::Value* lo = lowerExpr(*range->lo);
                llvm::Value* hi = lowerExpr(*range->hi);
                if (!x || !lo || !hi) return nullptr;
                // x >= lo AND (x <= hi for inclusive, x < hi for half-open).
                llvm::Value* geL = builder_.CreateICmpSGE(x, lo);
                llvm::Value* leH = range->inclusive
                                       ? builder_.CreateICmpSLE(x, hi)
                                       : builder_.CreateICmpSLT(x, hi);
                return builder_.CreateAnd(geL, leH);
            }
            return nullptr;  // non-range `in` (collection) not yet implemented
        }
        llvm::Value* l = lowerExpr(*bin.left);
        llvm::Value* r = lowerExpr(*bin.right);
        if (!l || !r) return nullptr;
        const auto& op = bin.op;
        // CG24 (§6.3): floating-point arithmetic and comparisons (IEEE 754,
        // no overflow/div0 checks). If one operand is FP and the other an
        // integer, promote the integer (§6.2 mixed-type promotion).
        if (l->getType()->isFloatingPointTy() ||
            r->getType()->isFloatingPointTy()) {
            bool lFP = l->getType()->isFloatingPointTy();
            bool rFP = r->getType()->isFloatingPointTy();
            // The result FP type is the wider of two FP operands (so `float`
            // mixed with `double` widens the float), else the lone FP type.
            llvm::Type* fty =
                (lFP && rFP)
                    ? (l->getType()->getPrimitiveSizeInBits() >=
                               r->getType()->getPrimitiveSizeInBits()
                           ? l->getType()
                           : r->getType())
                    : (lFP ? l->getType() : r->getType());
            auto toFP = [&](llvm::Value* v) -> llvm::Value* {
                if (!v->getType()->isFloatingPointTy())
                    return builder_.CreateSIToFP(v, fty);  // §6.2 int promote
                if (v->getType() != fty)
                    return builder_.CreateFPExt(v, fty);   // float → double
                return v;
            };
            l = toFP(l);
            r = toFP(r);
            if (op == "+") return builder_.CreateFAdd(l, r);
            if (op == "-") return builder_.CreateFSub(l, r);
            if (op == "*") return builder_.CreateFMul(l, r);
            if (op == "/") return builder_.CreateFDiv(l, r);
            if (op == "=") return builder_.CreateFCmpOEQ(l, r);
            // §6.3 (IEEE 754): `<>` is the negation of `=`, so it is true
            // when either operand is NaN — an *unordered* not-equal (UNE).
            // (`=` stays ordered-equal: `NaN = NaN` is false.)
            if (op == "<>") return builder_.CreateFCmpUNE(l, r);
            if (op == "<") return builder_.CreateFCmpOLT(l, r);
            if (op == ">") return builder_.CreateFCmpOGT(l, r);
            if (op == "<=") return builder_.CreateFCmpOLE(l, r);
            if (op == ">=") return builder_.CreateFCmpOGE(l, r);
            return nullptr;
        }
        // CG25b: unsigned integer operands select unsigned operations
        // (udiv/urem/lshr/unsigned compares, zext widening, uXXX overflow).
        bool uns = isUnsignedBinOperand(bin);
        // CG25 (§6.2): mixed-width integer operands — widen the narrower to
        // the wider (zero-extend for unsigned, sign-extend otherwise) so the
        // op and any overflow check run at one width.
        if (l->getType()->isIntegerTy() && r->getType()->isIntegerTy() &&
            !l->getType()->isIntegerTy(1) && !r->getType()->isIntegerTy(1) &&
            l->getType() != r->getType()) {
            unsigned lw = l->getType()->getIntegerBitWidth();
            unsigned rw = r->getType()->getIntegerBitWidth();
            llvm::Type* wide = lw >= rw ? l->getType() : r->getType();
            if (lw < rw) l = uns ? builder_.CreateZExt(l, wide)
                                 : builder_.CreateSExt(l, wide);
            if (rw < lw) r = uns ? builder_.CreateZExt(r, wide)
                                 : builder_.CreateSExt(r, wide);
        }
        // CG27a (§4.6): string binary operations — intercept before the
        // integer arithmetic path (string variables are ptr-typed, not int).
        // When either operand is a null literal, fall through to pointer
        // comparison (`s = null` / `s <> null` is a reference/null check).
        {
            auto isStr = [&](const ast::Expr* e) {
                if (!e) return false;
                auto* rt = ctx_.exprType(*e);
                return rt && rt->kind == sema::TypeRef::Kind::Primitive &&
                       rt->primitive == sema::PrimitiveType::String;
            };
            bool eitherNull =
                (bin.left && dynamic_cast<const ast::NullLiteralExpr*>(
                                  bin.left.get())) ||
                (bin.right && dynamic_cast<const ast::NullLiteralExpr*>(
                                   bin.right.get()));
            if (isStr(bin.left.get()) && !eitherNull) {
                if (op == "=") return emitStringEq(l, r);
                if (op == "<>") return builder_.CreateNot(emitStringEq(l, r));
                if (op == "+") return emitStringConcat(l, r);
                // CG27b (§4.6): ordinal comparison operators.
                if (op == "<" || op == ">" || op == "<=" || op == ">=") {
                    llvm::Value* cmp = emitStringOrdCmp(l, r);
                    llvm::Value* zero = llvm::ConstantInt::get(i32(), 0);
                    if (op == "<") return builder_.CreateICmpSLT(cmp, zero);
                    if (op == ">") return builder_.CreateICmpSGT(cmp, zero);
                    if (op == "<=") return builder_.CreateICmpSLE(cmp, zero);
                    /* >= */ return builder_.CreateICmpSGE(cmp, zero);
                }
            }
        }
        // CG22 (§6.1): integer + - * are overflow-checked; / and mod also
        // raise on a zero divisor and on INT_MIN / -1. Applies to integer
        // operands only (bool uses and/or; floats — CG24 — are unchecked).
        bool checkedInt = l->getType()->isIntegerTy() &&
                          !l->getType()->isIntegerTy(1);
        if (checkedInt) {
            if (op == "+")
                return emitCheckedArith(
                    uns ? llvm::Intrinsic::uadd_with_overflow
                        : llvm::Intrinsic::sadd_with_overflow, l, r);
            if (op == "-")
                return emitCheckedArith(
                    uns ? llvm::Intrinsic::usub_with_overflow
                        : llvm::Intrinsic::ssub_with_overflow, l, r);
            if (op == "*")
                return emitCheckedArith(
                    uns ? llvm::Intrinsic::umul_with_overflow
                        : llvm::Intrinsic::smul_with_overflow, l, r);
            if (op == "/" || op == "mod") {
                emitGuardThrow(builder_.CreateICmpEQ(r, zeroLike(r)),
                               "DivideByZeroException");
                if (uns)
                    return op == "/" ? builder_.CreateUDiv(l, r)
                                     : builder_.CreateURem(l, r);
                // Signed: INT_MIN / -1 (and mod) overflow the result type.
                llvm::Value* minV = minIntLike(r);
                llvm::Value* negOne =
                    llvm::ConstantInt::getSigned(r->getType(), -1);
                emitGuardThrow(
                    builder_.CreateAnd(builder_.CreateICmpEQ(l, minV),
                                       builder_.CreateICmpEQ(r, negOne)),
                    "OverflowException");
                return op == "/" ? builder_.CreateSDiv(l, r)
                                 : builder_.CreateSRem(l, r);
            }
        }
        if (op == "+") return builder_.CreateAdd(l, r);
        if (op == "-") return builder_.CreateSub(l, r);
        if (op == "*") return builder_.CreateMul(l, r);
        if (op == "/") return uns ? builder_.CreateUDiv(l, r)
                                  : builder_.CreateSDiv(l, r);
        if (op == "mod") return uns ? builder_.CreateURem(l, r)
                                    : builder_.CreateSRem(l, r);
        if (op == "&") return builder_.CreateAnd(l, r);
        if (op == "|") return builder_.CreateOr(l, r);
        if (op == "^") return builder_.CreateXor(l, r);
        if (op == "<<") return builder_.CreateShl(l, r);
        if (op == ">>") return uns ? builder_.CreateLShr(l, r)
                                   : builder_.CreateAShr(l, r);
        // CG29b (§8.3): records compare structurally — dispatch before the
        // scalar integer path (ICmp doesn't accept struct operands).
        if ((op == "=" || op == "<>") && l->getType()->isStructTy()) {
            llvm::Value* eq = emitStructEquality(
                l, r, llvm::cast<llvm::StructType>(l->getType()));
            return op == "=" ? eq : builder_.CreateNot(eq);
        }
        // CG4: integer comparisons → i1.
        if (op == "=") return builder_.CreateICmpEQ(l, r);
        if (op == "<>") return builder_.CreateICmpNE(l, r);
        if (op == "<") return uns ? builder_.CreateICmpULT(l, r)
                                  : builder_.CreateICmpSLT(l, r);
        if (op == ">") return uns ? builder_.CreateICmpUGT(l, r)
                                  : builder_.CreateICmpSGT(l, r);
        if (op == "<=") return uns ? builder_.CreateICmpULE(l, r)
                                   : builder_.CreateICmpSLE(l, r);
        if (op == ">=") return uns ? builder_.CreateICmpUGE(l, r)
                                   : builder_.CreateICmpSGE(l, r);
        // CG4: logical and/or on i1. NOTE: §17 specifies short-circuit
        // evaluation; this eager form is observationally identical while
        // operands are side-effect-free (no calls lowered yet) — true
        // short-circuiting lands once call lowering exists.
        return nullptr;  // `and`/`or` handled by lowerShortCircuit
    }

    // CG: short-circuit `and`/`or` via a branch + phi — the right operand
    // runs only on the deciding path (§17 logical operators).
    llvm::Value* lowerShortCircuit(const ast::BinaryExpr& bin) {
        bool isAnd = (bin.op == "and");
        llvm::Value* l = lowerExpr(*bin.left);
        if (!l) return nullptr;
        auto* entryBB = builder_.GetInsertBlock();
        auto* rhsBB = llvm::BasicBlock::Create(llvm_, "sc.rhs", currentFn_);
        auto* endBB = llvm::BasicBlock::Create(llvm_, "sc.end", currentFn_);
        // and: evaluate RHS only when l is true; or: only when l is false.
        if (isAnd) builder_.CreateCondBr(l, rhsBB, endBB);
        else       builder_.CreateCondBr(l, endBB, rhsBB);

        builder_.SetInsertPoint(rhsBB);
        llvm::Value* r = lowerExpr(*bin.right);
        if (!r) return nullptr;
        auto* rhsEndBB = builder_.GetInsertBlock();
        builder_.CreateBr(endBB);

        builder_.SetInsertPoint(endBB);
        auto* phi = builder_.CreatePHI(i1(), 2);
        // Short-circuit value when RHS is skipped: false for `and`, true
        // for `or`.
        phi->addIncoming(builder_.getInt1(!isAnd), entryBB);
        phi->addIncoming(r, rhsEndBB);
        return phi;
    }

    // CG39: null-coalescing `a ?? b` (§17.1 L12). Evaluates `a`; if it is a
    // non-null reference, that is the result; otherwise evaluates and yields
    // `b`. The right operand is not evaluated when `a` is non-null. Operands
    // are reference-typed here (a possibly-null heap pointer); `??` on
    // nullable value types (`int?`, …) is deferred until those have a
    // representation.
    llvm::Value* lowerCoalesce(const ast::BinaryExpr& bin) {
        llvm::Value* l = lowerExpr(*bin.left);
        if (!l) return nullptr;
        if (!l->getType()->isPointerTy())
            return nullptr;  // nullable value-type `??` deferred
        auto* entryBB = builder_.GetInsertBlock();
        auto* rhsBB = llvm::BasicBlock::Create(llvm_, "coalesce.rhs", currentFn_);
        auto* endBB = llvm::BasicBlock::Create(llvm_, "coalesce.end", currentFn_);
        llvm::Value* isNull = builder_.CreateICmpEQ(
            l, llvm::ConstantPointerNull::get(ptrTy()));
        builder_.CreateCondBr(isNull, rhsBB, endBB);

        builder_.SetInsertPoint(rhsBB);
        llvm::Value* r = lowerExpr(*bin.right);
        bool rhsDiverged = diverged_;  // CG40: e.g. `a ?? throw E`
        if (!r) return nullptr;
        llvm::BasicBlock* rhsEndBB = nullptr;
        if (!rhsDiverged) {
            rhsEndBB = builder_.GetInsertBlock();
            builder_.CreateBr(endBB);
        }

        builder_.SetInsertPoint(endBB);
        diverged_ = false;  // the result is reachable via the non-null path
        if (rhsDiverged) {
            // The right operand branched away (threw); endBB is reached only
            // by the non-null left path, so the result is always `l`.
            return l;
        }
        auto* phi = builder_.CreatePHI(ptrTy(), 2);
        phi->addIncoming(l, entryBB);  // left was non-null
        phi->addIncoming(r, rhsEndBB);
        return phi;
    }

    llvm::Value* lowerUnary(const ast::UnaryExpr& un) {
        if (!un.operand) return nullptr;
        // §25: a user-defined unary operator overload — sema resolved this
        // unary expression to a static `operator` method; call it with the
        // operand.
        if (auto* opm =
                dynamic_cast<sema::MethodSymbol*>(ctx_.exprSymbol(un))) {
            if (opm->isOperator) {
                llvm::Value* ov = lowerExpr(*un.operand);
                if (!ov) return nullptr;
                std::vector<llvm::Value*> args{ov};
                return emitMethodCall(opm, args, /*baseQualified=*/false);
            }
        }
        llvm::Value* v = lowerExpr(*un.operand);
        if (!v) return nullptr;
        const auto& op = un.op;
        if (op == "-") {
            if (v->getType()->isFloatingPointTy())  // CG24: §6.3 IEEE negate
                return builder_.CreateFNeg(v);
            // §6.1: unary negation overflows only at INT_MIN.
            if (v->getType()->isIntegerTy() && !v->getType()->isIntegerTy(1)) {
                emitGuardThrow(builder_.CreateICmpEQ(v, minIntLike(v)),
                               "OverflowException");
            }
            return builder_.CreateNeg(v);
        }
        if (op == "~") return builder_.CreateNot(v);   // bitwise complement
        if (op == "+") return v;                       // identity
        if (op == "not") return builder_.CreateNot(v); // i1 logical negation
        // §28.3: `move x` is a compile-time ownership transfer; at runtime
        // it yields the operand value unchanged (the same reference).
        if (op == "move") return v;
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
    // CG34: `source with f := v, …` (§A.7 object initializer on a `new`
    // expression). Construct the source, then store each named value into
    // the corresponding instance-storage member (field or auto-property).
    llvm::Value* lowerWith(const ast::WithExpr& we) {
        // `expr with name := v, ... end` has two spec forms, distinguished by
        // the source type:
        //  - Record (value type, §9.5): a *non-destructive copy* — the source
        //    record is unchanged. The source is a first-class struct value, so
        //    spill it to a fresh stack slot (the copy), overwrite the named
        //    fields through that pointer, and reload the result by value. Using
        //    the struct value directly as a GEP base is malformed (and hung the
        //    compiler — #254).
        //  - Class (object initializer block, §22.11): the source is a freshly
        //    `new`-constructed instance (a pointer); the named members are
        //    assigned in place and the same pointer is returned.
        if (!we.source) return nullptr;
        llvm::Value* src = lowerExpr(*we.source);
        if (!src) return nullptr;
        auto* rt = ctx_.exprType(*we.source);
        if (!rt || rt->kind != sema::TypeRef::Kind::Named || !rt->declaration)
            return src;
        const sema::TypeSymbol* t = rt->declaration;
        const bool isRecord = t->kind == ast::TypeDeclKind::Record;
        // classStruct routes records to recordStruct (no vtable), so this is
        // the right layout for both forms.
        llvm::StructType* st = classStruct(t);
        // The GEP base: a spilled copy for a record, the instance pointer for
        // a class.
        llvm::Value* base = src;
        if (isRecord) {
            base = entryAlloca(st);
            builder_.CreateStore(src, base);
        }
        std::vector<const sema::Symbol*> storage;
        collectInstanceStorage(t, storage);
        for (const auto& a : we.assigns) {
            if (!a.value) continue;
            const sema::Symbol* m = nullptr;
            for (const sema::Symbol* s : storage)
                if (s->name == a.name) { m = s; break; }
            if (!m) continue;
            auto idxIt = fieldIndex_.find(m);
            if (idxIt == fieldIndex_.end()) continue;
            llvm::Value* v = lowerExpr(*a.value);
            if (!v) continue;
            builder_.CreateStore(
                v, builder_.CreateStructGEP(st, base, idxIt->second));
        }
        // Record: return the modified copy by value. Class: return the mutated
        // instance pointer.
        return isRecord ? builder_.CreateLoad(st, base) : base;
    }

    llvm::Value* lowerNew(const ast::NewExpr& ne) {
        if (!ne.type) return nullptr;
        sema::TypeRef* rt = ctx_.resolvedType(*ne.type);
        if (!rt || rt->kind != sema::TypeRef::Kind::Named || !rt->declaration) {
            return nullptr;
        }
        const sema::TypeSymbol* t = rt->declaration;

        // §9 / §9.5: `new Record(args)` builds a value-type struct directly —
        // no heap allocation, vtable, constructor chain, or disposal. This is
        // the spec's record construction form (records use `new`, like classes,
        // but produce a value).
        if (t->kind == ast::TypeDeclKind::Record)
            return buildRecordValue(t, ne.args);

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
        std::vector<const sema::Symbol*> storage;
        collectInstanceStorage(t, storage);
        for (const sema::Symbol* m : storage) {
            const ast::Expr* init = nullptr;
            if (auto* fd = dynamic_cast<const ast::FieldDecl*>(m->decl))
                init = fd->initializer.get();
            else if (auto* pd = dynamic_cast<const ast::PropertyDecl*>(m->decl))
                init = pd->autoInitializer.get();  // CG33: `property X := v;`
            if (!init) continue;  // no initializer → calloc-zeroed
            auto idxIt = fieldIndex_.find(m);
            if (idxIt == fieldIndex_.end()) continue;
            llvm::Value* v = lowerExpr(*init);
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
        if (sema::ConstructorSymbol* cs = ctorForArgs(t, ne.args)) {
            auto it = ctorFns_.find(cs);
            auto* cd = dynamic_cast<const ast::ConstructorDecl*>(cs->decl);
            if (it != ctorFns_.end() && cd) {
                std::vector<llvm::Value*> args;
                args.push_back(obj);
                // §22.4/§22.7: named arguments and default values, as for a
                // method call.
                if (buildOrderedArgs(cd->params, ne.args, args) &&
                    args.size() == it->second->arg_size()) {
                    builder_.CreateCall(it->second, args);
                    emitPendingCheck();  // CG20: a constructor may throw
                }
            }
        } else if (!hasAnyConstructor(t)) {
            // CG16: no explicit ctor → the implicit ctor runs an implicit
            // `base()`, so a base-class ctor still runs on `new Derived()`.
            emitImplicitBase(t, obj);
            emitPendingCheck();  // CG20: the base constructor may throw
        }
        return obj;
    }

    // CG8: address of a field access `obj.f`. Returns the GEP pointer and
    // sets `elemTy` to the field's type. Null if `ma` isn't a field
    // access we can lower.
    llvm::Value* fieldAddr(const ast::MemberAccessExpr& ma,
                           llvm::Type*& elemTy) {
        auto* ms = storageMemberOf(ctx_.exprSymbol(ma));
        if (!ms || !ms->parent || !ma.target) return nullptr;
        auto idxIt = fieldIndex_.find(ms);
        if (idxIt == fieldIndex_.end()) return nullptr;
        llvm::StructType* st = classStruct(ms->parent);
        // CG29: a record is a first-class struct value, not a pointer. To take
        // the address of one of its fields, GEP the record's storage:
        //  - an addressable target (a record local/parameter) → GEP its slot
        //    directly, the natural value-type access;
        //  - an already-pointer target (a `this` pointer inside a record
        //    method) → GEP it as-is;
        //  - a temporary record value (e.g. a freshly returned record) → spill
        //    it to a stack slot and GEP that.
        // Records are immutable (§9.2), so none of these can lose a write.
        if (ms->parent->kind == ast::TypeDeclKind::Record) {
            llvm::Value* base = localSlot(*ma.target);
            if (!base) {
                llvm::Value* obj = lowerExpr(*ma.target);
                if (!obj) return nullptr;
                if (obj->getType()->isPointerTy()) {
                    base = obj;
                } else {
                    llvm::Value* tmp = entryAlloca(st);
                    builder_.CreateStore(obj, tmp);
                    base = tmp;
                }
            }
            elemTy = st->getElementType(idxIt->second);
            return builder_.CreateStructGEP(st, base, idxIt->second);
        }
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
    // CG44: lower an explicit conversion `T(x)` (§5.2). Converts the single
    // argument to the target primitive's LLVM type: integer width changes
    // via trunc / sext / zext (signedness from the operand), int↔float via
    // si/ui<->fp, and float↔double via fpext / fptrunc. (Narrowing overflow
    // checks — §5.2 raises OverflowException — are deferred.)
    llvm::Value* lowerConversion(sema::PrimitiveType target,
                                 const ast::CallExpr& call) {
        if (call.args.size() != 1 || !call.args[0].value) return nullptr;
        const ast::Expr& argE = *call.args[0].value;
        llvm::Value* src = lowerExpr(argE);
        if (!src) return nullptr;
        llvm::Type* dstTy = lowerPrimitive(target);
        llvm::Type* srcTy = src->getType();
        if (srcTy == dstTy) return src;
        bool dstFP = dstTy->isFloatingPointTy();
        bool srcFP = srcTy->isFloatingPointTy();
        bool dstUns = isUnsignedPrim(target);
        bool srcUns = isUnsignedTargetExpr(argE);
        if (srcFP && dstFP) {
            return srcTy->getPrimitiveSizeInBits() <
                           dstTy->getPrimitiveSizeInBits()
                       ? builder_.CreateFPExt(src, dstTy)
                       : builder_.CreateFPTrunc(src, dstTy);
        }
        if (srcFP)  // float → int
            return dstUns ? builder_.CreateFPToUI(src, dstTy)
                          : builder_.CreateFPToSI(src, dstTy);
        if (dstFP)  // int → float
            return srcUns ? builder_.CreateUIToFP(src, dstTy)
                          : builder_.CreateSIToFP(src, dstTy);
        // int → int.
        unsigned sb = srcTy->getIntegerBitWidth();
        unsigned db = dstTy->getIntegerBitWidth();
        if (db < sb) {
            // §5.2: integer narrowing raises OverflowException if the value
            // does not fit the target. Truncate, extend back with the
            // target's signedness, and compare to the original — a mismatch
            // means the value was out of range.
            llvm::Value* trunc = builder_.CreateTrunc(src, dstTy);
            llvm::Value* back = dstUns ? builder_.CreateZExt(trunc, srcTy)
                                       : builder_.CreateSExt(trunc, srcTy);
            emitGuardThrow(builder_.CreateICmpNE(back, src),
                           "OverflowException");
            return trunc;
        }
        if (db > sb)
            return srcUns ? builder_.CreateZExt(src, dstTy)
                          : builder_.CreateSExt(src, dstTy);
        return src;  // same width, differing signedness (e.g. int ↔ uint)
    }

    // CG44: lower an enum cast `E(i)` (§5.4). The enum's underlying type is
    // i32, so width-adjust the integer argument to i32. (Out-of-range
    // checks for the checked cast forms are deferred.)
    llvm::Value* lowerEnumCast(const ast::CallExpr& call) {
        if (call.args.size() != 1 || !call.args[0].value) return nullptr;
        llvm::Value* src = lowerExpr(*call.args[0].value);
        if (!src || !src->getType()->isIntegerTy()) return nullptr;
        unsigned sb = src->getType()->getIntegerBitWidth();
        if (sb < 32) return builder_.CreateSExt(src, i32());
        if (sb > 32) return builder_.CreateTrunc(src, i32());
        return src;
    }

    // §22.4/§22.7: can the given call arguments fill the parameter list?
    // (named arguments matched by name, positionals by position, every
    // remaining parameter defaulted). Pure check — no IR emitted — used to
    // pick a constructor/method overload before lowering its arguments.
    static bool argsCanFill(const std::vector<ast::Param>& params,
                            const std::vector<ast::CallArgument>& args) {
        std::vector<bool> filled(params.size(), false);
        std::size_t pos = 0;
        for (const auto& a : args) {
            std::size_t idx;
            if (a.name.empty()) {
                if (pos >= params.size()) return false;
                idx = pos++;
            } else {
                idx = params.size();
                for (std::size_t j = 0; j < params.size(); ++j)
                    if (params[j].name == a.name) { idx = j; break; }
                if (idx >= params.size()) return false;
            }
            if (filled[idx]) return false;
            filled[idx] = true;
        }
        for (std::size_t j = 0; j < params.size(); ++j)
            if (!filled[j] && !params[j].defaultValue) return false;
        return true;
    }

    // §22.4/§22.7: lower the call arguments into parameter order, honoring
    // named arguments and default values. Provided arguments are evaluated
    // in source order; defaults afterward in parameter order. Appends to
    // `out`; returns false if an argument can't be placed.
    bool buildOrderedArgs(const std::vector<ast::Param>& params,
                          const std::vector<ast::CallArgument>& args,
                          std::vector<llvm::Value*>& out) {
        std::vector<llvm::Value*> ordered(params.size(), nullptr);
        std::size_t pos = 0;
        for (const auto& a : args) {
            if (!a.value) return false;
            std::size_t idx = params.size();
            if (a.name.empty()) {
                idx = pos++;
            } else {
                for (std::size_t j = 0; j < params.size(); ++j)
                    if (params[j].name == a.name) { idx = j; break; }
            }
            if (idx >= params.size() || ordered[idx]) return false;
            // CG50: a ref/out parameter receives the address of its argument
            // lvalue; an ordinary parameter receives the argument's value.
            llvm::Value* v;
            if (isByRefMod(params[idx].modifier)) {
                llvm::Type* et = nullptr;
                v = lvalueAddr(*a.value, et);
            } else {
                v = lowerExpr(*a.value);
            }
            if (!v) return false;
            ordered[idx] = v;
        }
        for (std::size_t j = 0; j < params.size(); ++j) {
            if (!ordered[j]) {
                const ast::Expr* dv = params[j].defaultValue.get();
                if (!dv) return false;
                ordered[j] = lowerExpr(*dv);
                if (!ordered[j]) return false;
            }
            out.push_back(ordered[j]);
        }
        return true;
    }

    // CG29 (§9): build a record value from positional arguments — used by
    // `new Record(a, b, …)` (the spec construction form, §9.5). Positional
    // arguments map to the record's fields in declaration order; the value is
    // materialized in a stack slot and returned by value (records are value
    // types, no heap allocation). Named arguments and defaults are a later
    // refinement.
    llvm::Value* buildRecordValue(const sema::TypeSymbol* t,
                                  const std::vector<ast::CallArgument>& args) {
        llvm::StructType* st = recordStruct(t);
        llvm::Value* slot = entryAlloca(st);
        std::vector<const sema::Symbol*> fields;
        for (const sema::Symbol* m : t->members) {
            auto* fs = dynamic_cast<const sema::FieldSymbol*>(m);
            if (fs && !isStaticField(fs)) fields.push_back(fs);
        }
        if (args.size() != fields.size()) return nullptr;
        for (std::size_t i = 0; i < fields.size(); ++i) {
            if (!args[i].value) return nullptr;
            llvm::Value* v = lowerExpr(*args[i].value);
            if (!v) return nullptr;
            auto idxIt = fieldIndex_.find(fields[i]);
            if (idxIt == fieldIndex_.end()) return nullptr;
            builder_.CreateStore(
                v, builder_.CreateStructGEP(st, slot, idxIt->second));
        }
        return builder_.CreateLoad(st, slot);  // the record value (by value)
    }

    // CG-console: emit a Console.Write/Console.WriteLine call as an
    // intrinsic. Uses libc write(1, data, len) for Write and additionally
    // write(1, "\n", 1) for WriteLine. This provides observable stdout.
    // `value` must be a ptr to a bz_string; if it's an integer or other
    // primitive, the caller converts via emitPrimitiveToString first.
    llvm::Value* emitConsoleWrite(llvm::Value* strPtr, bool newline) {
        // Declare POSIX write(2): ssize_t write(int, const void*, size_t)
        auto* writeFn = module_->getFunction("write");
        if (!writeFn) {
            auto* ft = llvm::FunctionType::get(i64(), {i32(), ptrTy(), i64()},
                                               false);
            writeFn = llvm::Function::Create(
                ft, llvm::Function::ExternalLinkage, "write", module_.get());
        }
        llvm::Value* data = loadStringData(strPtr);
        llvm::Value* len  = loadStringLen(strPtr);
        builder_.CreateCall(writeFn,
                            {builder_.getInt32(1), data, len});  // fd=1 (stdout)
        if (newline) {
            // Write a newline byte.
            llvm::Constant* nlData = llvm::ConstantDataArray::getString(
                llvm_, "\n", /*AddNull=*/false);
            auto* nlGV = new llvm::GlobalVariable(
                *module_, nlData->getType(), true,
                llvm::GlobalValue::PrivateLinkage, nlData, ".bz.nl");
            builder_.CreateCall(writeFn,
                                {builder_.getInt32(1), nlGV,
                                 builder_.getInt64(1)});
        }
        return nullptr;  // unit (void)
    }

    // CG-console: Console.ReadLine() → string? (null at EOF).
    // Uses fgets(buf, 4096, stdin), strips trailing newline, copies to heap.
    llvm::Value* emitConsoleReadLine() {
        auto* i8Ty = llvm::Type::getInt8Ty(llvm_);
        const unsigned BUFSZ = 4096;
        llvm::Value* buf = entryAlloca(llvm::ArrayType::get(i8Ty, BUFSZ));
        llvm::Value* bufPtr = builder_.CreateGEP(
            llvm::ArrayType::get(i8Ty, BUFSZ), buf,
            {builder_.getInt64(0), builder_.getInt64(0)});
        // fgets(bufPtr, BUFSZ, stdin)
        auto* fgetsFn = module_->getFunction("fgets");
        if (!fgetsFn) {
            auto* ft = llvm::FunctionType::get(ptrTy(), {ptrTy(), i32(), ptrTy()}, false);
            fgetsFn = llvm::Function::Create(
                ft, llvm::Function::ExternalLinkage, "fgets", module_.get());
        }
        // stdin global (FILE*)
        auto* stdinGV = module_->getGlobalVariable("stdin");
        if (!stdinGV) {
            stdinGV = new llvm::GlobalVariable(
                *module_, ptrTy(), false,
                llvm::GlobalValue::ExternalLinkage, nullptr, "stdin");
        }
        llvm::Value* stdinVal = builder_.CreateLoad(ptrTy(), stdinGV, "stdin");
        llvm::Value* fgRet = builder_.CreateCall(
            fgetsFn, {bufPtr, builder_.getInt32(BUFSZ), stdinVal}, "fgets.ret");
        auto* nullPtr = llvm::ConstantPointerNull::get(ptrTy());
        llvm::Value* isEOF = builder_.CreateICmpEQ(fgRet, nullPtr, "rl.eof");
        auto* eofBB = llvm::BasicBlock::Create(llvm_, "rl.eof",  currentFn_);
        auto* okBB  = llvm::BasicBlock::Create(llvm_, "rl.ok",   currentFn_);
        auto* endBB = llvm::BasicBlock::Create(llvm_, "rl.end",  currentFn_);
        builder_.CreateCondBr(isEOF, eofBB, okBB);
        builder_.SetInsertPoint(eofBB);
        builder_.CreateBr(endBB);
        // OK: compute length, strip trailing '\n'.
        builder_.SetInsertPoint(okBB);
        auto* strlenFn = module_->getFunction("strlen");
        if (!strlenFn) {
            auto* ft2 = llvm::FunctionType::get(i64(), {ptrTy()}, false);
            strlenFn = llvm::Function::Create(
                ft2, llvm::Function::ExternalLinkage, "strlen", module_.get());
        }
        llvm::Value* rawLen = builder_.CreateCall(strlenFn, {bufPtr}, "rawlen");
        llvm::Value* hasBytes = builder_.CreateICmpSGT(rawLen, builder_.getInt64(0));
        auto* stripBB = llvm::BasicBlock::Create(llvm_, "rl.strip", currentFn_);
        auto* afterBB = llvm::BasicBlock::Create(llvm_, "rl.after", currentFn_);
        builder_.CreateCondBr(hasBytes, stripBB, afterBB);
        builder_.SetInsertPoint(stripBB);
        llvm::Value* lastIdx = builder_.CreateSub(rawLen, builder_.getInt64(1));
        llvm::Value* lastPtr = builder_.CreateGEP(i8Ty, bufPtr, lastIdx);
        llvm::Value* lastByte = builder_.CreateLoad(i8Ty, lastPtr);
        llvm::Value* isNL = builder_.CreateICmpEQ(lastByte, llvm::ConstantInt::get(i8Ty, '\n'));
        auto* nlBB = llvm::BasicBlock::Create(llvm_, "rl.nl", currentFn_);
        builder_.CreateCondBr(isNL, nlBB, afterBB);
        builder_.SetInsertPoint(nlBB);
        builder_.CreateStore(llvm::ConstantInt::get(i8Ty, 0), lastPtr);
        builder_.CreateBr(afterBB);
        builder_.SetInsertPoint(afterBB);
        // Final length after stripping.
        llvm::Value* finalLen = builder_.CreateCall(strlenFn, {bufPtr}, "finalLen");
        // Heap-copy the data.
        auto* mallocFn = declareC("malloc", ptrTy(), {i64()});
        auto* memcpyFn = declareC("memcpy", ptrTy(), {ptrTy(), ptrTy(), i64()});
        llvm::Value* dataSz = builder_.CreateAdd(finalLen, builder_.getInt64(1));
        llvm::Value* dataCopy = builder_.CreateCall(mallocFn, {dataSz}, "rl.data");
        builder_.CreateCall(memcpyFn, {dataCopy, bufPtr, finalLen});
        llvm::Value* endData = builder_.CreateGEP(i8Ty, dataCopy, finalLen);
        builder_.CreateStore(llvm::ConstantInt::get(i8Ty, 0), endData);
        uint64_t strBytes = module_->getDataLayout().getTypeAllocSize(stringType());
        llvm::Value* strObj = builder_.CreateCall(mallocFn, {builder_.getInt64(strBytes)}, "rl.obj");
        builder_.CreateStore(dataCopy, builder_.CreateStructGEP(stringType(), strObj, 0));
        builder_.CreateStore(finalLen, builder_.CreateStructGEP(stringType(), strObj, 1));
        auto* okEnd = builder_.GetInsertBlock();
        builder_.CreateBr(endBB);
        // Merge.
        builder_.SetInsertPoint(endBB);
        auto* phi = builder_.CreatePHI(ptrTy(), 2, "rl.result");
        phi->addIncoming(nullPtr, eofBB);
        phi->addIncoming(strObj, okEnd);
        return phi;
    }

    // Detect `Console.Write(...)` / `Console.WriteLine(...)` /
    // `Console.WriteLine()` (no-arg) as intrinsic call patterns.
    // Returns true and emits code if the pattern matches; false otherwise.
    bool tryLowerConsoleCall(const ast::CallExpr& call) {
        if (!call.callee) return false;
        auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(call.callee.get());
        if (!ma || !ma->target) return false;
        auto* id = dynamic_cast<const ast::IdentifierExpr*>(ma->target.get());
        if (!id || id->name != "Console") return false;

        bool isWriteLine = (ma->member == "WriteLine");
        bool isWrite = (ma->member == "Write");
        if (!isWrite && !isWriteLine) return false;

        // Console.WriteLine() with no args → just print a newline.
        if (call.args.empty() && isWriteLine) {
            auto* writeFn = module_->getFunction("write");
            if (!writeFn) {
                auto* ft = llvm::FunctionType::get(i64(), {i32(), ptrTy(), i64()}, false);
                writeFn = llvm::Function::Create(
                    ft, llvm::Function::ExternalLinkage, "write", module_.get());
            }
            llvm::Constant* nlData = llvm::ConstantDataArray::getString(
                llvm_, "\n", false);
            unsigned id2 = stringLiteralIdx_++;
            auto* nlGV = new llvm::GlobalVariable(
                *module_, nlData->getType(), true,
                llvm::GlobalValue::PrivateLinkage, nlData,
                ".bz.console.nl." + std::to_string(id2));
            builder_.CreateCall(writeFn, {builder_.getInt32(1), nlGV, builder_.getInt64(1)});
            return true;
        }
        if (call.args.size() != 1 || !call.args[0].value) return false;

        // Evaluate the argument. If it's a string, use directly; if it's a
        // primitive, convert via emitPrimitiveToString first.
        llvm::Value* arg = lowerExpr(*call.args[0].value);
        if (!arg) return false;
        auto* argTy = ctx_.exprType(*call.args[0].value);
        llvm::Value* strPtr = arg;
        if (argTy) {
            if (argTy->kind == sema::TypeRef::Kind::Primitive &&
                argTy->primitive != sema::PrimitiveType::String) {
                strPtr = emitPrimitiveToString(arg, argTy->primitive);
            } else if (argTy->kind == sema::TypeRef::Kind::Named &&
                       argTy->declaration) {
                // Named type (class/record) — call ToString() via virtual
                // dispatch if the type has a ToString method, else fall back
                // to a type-name placeholder string.
                auto* ts = argTy->declaration;
                // Find a 'ToString()' method walking the inheritance chain.
                // ToString() may be declared on a base class (e.g. Shape)
                // and inherited by the concrete type (Circle).
                sema::MethodSymbol* toStr = nullptr;
                for (auto* cur = ts; cur && !toStr;) {
                    for (auto* sym : cur->members) {
                        auto* ms = dynamic_cast<sema::MethodSymbol*>(sym);
                        if (ms && ms->name == "ToString" && ms->params.empty()) {
                            toStr = ms;
                            break;
                        }
                    }
                    // Walk to first base class.
                    sema::TypeSymbol* next = nullptr;
                    for (auto* b : cur->baseTypes)
                        if (b && b->kind == ast::TypeDeclKind::Class) {
                            next = b; break;
                        }
                    cur = next;
                }
                if (toStr) {
                    // Emit the method call (virtual if overriding, static otherwise).
                    std::vector<llvm::Value*> args{arg};
                    strPtr = emitMethodCall(toStr, args, false);
                } else {
                    // No ToString override — use type name as placeholder string.
                    strPtr = lowerStringLiteral(
                        std::string("\"") + std::string(ts->name) + "\"",
                        false);
                }
            }
        }
        if (!strPtr) return false;
        emitConsoleWrite(strPtr, isWriteLine);
        return true;
    }

    // Detect Convert.ToInt32(s) / Convert.ToInt64(s) / Math.Abs(n) etc.
    // as intrinsic static class method calls. Returns a lowered value or null.
    llvm::Value* tryLowerBuiltinClassCall(const ast::CallExpr& call) {
        if (!call.callee) return nullptr;
        auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(call.callee.get());
        if (!ma || !ma->target) return nullptr;
        auto* id = dynamic_cast<const ast::IdentifierExpr*>(ma->target.get());
        if (!id) return nullptr;
        const auto cls = id->name;
        const auto& m = ma->member;

        // §2.3 Environment class methods.
        if (cls == "Environment") {
            // Environment.Exit(code: int) — terminate the process.
            if (m == "Exit" && call.args.size() == 1 && call.args[0].value) {
                llvm::Value* code = lowerExpr(*call.args[0].value);
                if (!code) return nullptr;
                // Declare exit(3): void exit(int)
                auto* exitFn = module_->getFunction("exit");
                if (!exitFn) {
                    auto* ft = llvm::FunctionType::get(
                        llvm::Type::getVoidTy(llvm_), {i32()}, false);
                    exitFn = llvm::Function::Create(
                        ft, llvm::Function::ExternalLinkage, "exit", module_.get());
                    exitFn->addFnAttr(llvm::Attribute::NoReturn);
                }
                // Truncate to i32 if needed.
                if (!code->getType()->isIntegerTy(32))
                    code = builder_.CreateTrunc(code, i32());
                builder_.CreateCall(exitFn, {code});
                // Create an unreachable block after the noreturn call.
                builder_.CreateUnreachable();
                auto* deadBB = llvm::BasicBlock::Create(llvm_, "exit.dead", currentFn_);
                builder_.SetInsertPoint(deadBB);
                return llvm::ConstantInt::get(i32(), 0);  // nominal value, never reached
            }
            // Environment.GetEnvironmentVariable(name): string? — getenv
            if (m == "GetEnvironmentVariable" && call.args.size() == 1 &&
                call.args[0].value) {
                llvm::Value* nameStr = lowerExpr(*call.args[0].value);
                if (!nameStr) return nullptr;
                llvm::Value* nameData = loadStringData(nameStr);
                auto* getenvFn = module_->getFunction("getenv");
                if (!getenvFn) {
                    auto* ft = llvm::FunctionType::get(ptrTy(), {ptrTy()}, false);
                    getenvFn = llvm::Function::Create(
                        ft, llvm::Function::ExternalLinkage, "getenv", module_.get());
                }
                llvm::Value* rawPtr = builder_.CreateCall(getenvFn, {nameData}, "getenv.ret");
                // null → return null (string?). Non-null: wrap in bz_string.
                auto* nullPtr = llvm::ConstantPointerNull::get(ptrTy());
                llvm::Value* isNull = builder_.CreateICmpEQ(rawPtr, nullPtr);
                auto* nullBB = llvm::BasicBlock::Create(llvm_, "env.null", currentFn_);
                auto* okBB   = llvm::BasicBlock::Create(llvm_, "env.ok",   currentFn_);
                auto* endBB  = llvm::BasicBlock::Create(llvm_, "env.end",  currentFn_);
                auto* entBB  = builder_.GetInsertBlock();
                builder_.CreateCondBr(isNull, endBB, okBB);
                builder_.SetInsertPoint(okBB);
                // strlen of the result.
                auto* strlenFn = module_->getFunction("strlen");
                if (!strlenFn) {
                    auto* ft2 = llvm::FunctionType::get(i64(), {ptrTy()}, false);
                    strlenFn = llvm::Function::Create(ft2, llvm::Function::ExternalLinkage, "strlen", module_.get());
                }
                llvm::Value* rawLen = builder_.CreateCall(strlenFn, {rawPtr});
                auto* mallocFn = declareC("malloc", ptrTy(), {i64()});
                auto* memcpyFn = declareC("memcpy", ptrTy(), {ptrTy(), ptrTy(), i64()});
                auto* i8Ty = llvm::Type::getInt8Ty(llvm_);
                llvm::Value* dataSz = builder_.CreateAdd(rawLen, builder_.getInt64(1));
                llvm::Value* dataCopy = builder_.CreateCall(mallocFn, {dataSz});
                builder_.CreateCall(memcpyFn, {dataCopy, rawPtr, rawLen});
                llvm::Value* end = builder_.CreateGEP(i8Ty, dataCopy, rawLen);
                builder_.CreateStore(llvm::ConstantInt::get(i8Ty, 0), end);
                uint64_t strBytes = module_->getDataLayout().getTypeAllocSize(stringType());
                llvm::Value* strObj = builder_.CreateCall(mallocFn, {builder_.getInt64(strBytes)});
                builder_.CreateStore(dataCopy, builder_.CreateStructGEP(stringType(), strObj, 0));
                builder_.CreateStore(rawLen, builder_.CreateStructGEP(stringType(), strObj, 1));
                auto* okEnd = builder_.GetInsertBlock();
                builder_.CreateBr(endBB);
                builder_.SetInsertPoint(endBB);
                auto* phi = builder_.CreatePHI(ptrTy(), 2, "env.result");
                phi->addIncoming(nullPtr, entBB);
                phi->addIncoming(strObj, okEnd);
                return phi;
            }
        }

        if (cls == "Convert") {
            if (call.args.size() == 1 && call.args[0].value) {
                // Convert.ToInt32(s: string): int — atoi / strtol
                if (m == "ToInt32") {
                    llvm::Value* arg = lowerExpr(*call.args[0].value);
                    if (!arg) return nullptr;
                    auto* argTy = ctx_.exprType(*call.args[0].value);
                    // If it's already an int, return as-is.
                    if (argTy && argTy->kind == sema::TypeRef::Kind::Primitive &&
                        argTy->primitive == sema::PrimitiveType::Int)
                        return arg;
                    // For string: call atoi(data_ptr)
                    llvm::Value* strPtr = arg;
                    llvm::Value* data = loadStringData(strPtr);
                    auto* atoiFn = module_->getFunction("atoi");
                    if (!atoiFn) {
                        auto* ft = llvm::FunctionType::get(i32(), {ptrTy()}, false);
                        atoiFn = llvm::Function::Create(
                            ft, llvm::Function::ExternalLinkage, "atoi", module_.get());
                    }
                    return builder_.CreateCall(atoiFn, {data}, "atoi.ret");
                }
                // Convert.ToInt64(s: string): long — atoll
                if (m == "ToInt64") {
                    llvm::Value* arg = lowerExpr(*call.args[0].value);
                    if (!arg) return nullptr;
                    llvm::Value* data = loadStringData(arg);
                    auto* atolFn = module_->getFunction("atoll");
                    if (!atolFn) {
                        auto* ft = llvm::FunctionType::get(i64(), {ptrTy()}, false);
                        atolFn = llvm::Function::Create(
                            ft, llvm::Function::ExternalLinkage, "atoll", module_.get());
                    }
                    return builder_.CreateCall(atolFn, {data}, "atoll.ret");
                }
                // Convert.ToDouble(s: string): double — atof
                if (m == "ToDouble") {
                    llvm::Value* arg = lowerExpr(*call.args[0].value);
                    if (!arg) return nullptr;
                    llvm::Value* data = loadStringData(arg);
                    auto* atofFn = module_->getFunction("atof");
                    if (!atofFn) {
                        auto* ft = llvm::FunctionType::get(
                            llvm::Type::getDoubleTy(llvm_), {ptrTy()}, false);
                        atofFn = llvm::Function::Create(
                            ft, llvm::Function::ExternalLinkage, "atof", module_.get());
                    }
                    return builder_.CreateCall(atofFn, {data}, "atof.ret");
                }
                // Convert.ToString(n: int): string — alias for n.ToString()
                if (m == "ToString") {
                    llvm::Value* arg = lowerExpr(*call.args[0].value);
                    if (!arg) return nullptr;
                    auto* argTy = ctx_.exprType(*call.args[0].value);
                    if (argTy && argTy->kind == sema::TypeRef::Kind::Primitive)
                        return emitPrimitiveToString(arg, argTy->primitive);
                }
            }
            // Convert.ToString(n, radix: int): string — radix-based conversion.
            if (m == "ToString" && call.args.size() == 2 &&
                call.args[0].value && call.args[1].value) {
                llvm::Value* arg = lowerExpr(*call.args[0].value);
                llvm::Value* radix = lowerExpr(*call.args[1].value);
                if (!arg || !radix) return nullptr;
                // Widen to i32.
                if (!arg->getType()->isIntegerTy(32))
                    arg = builder_.CreateSExt(arg, i32());
                // Select format based on radix (constant check).
                const char* fmt = "%d";
                if (auto* cr = llvm::dyn_cast<llvm::ConstantInt>(radix)) {
                    uint64_t r = cr->getZExtValue();
                    if (r == 16) fmt = "%x";
                    else if (r == 8) fmt = "%o";
                    else if (r == 2) fmt = nullptr;  // handle below
                }
                if (fmt) {
                    // Use snprintf with the chosen format.
                    llvm::Constant* fmtData = llvm::ConstantDataArray::getString(llvm_, fmt, true);
                    unsigned fid = stringLiteralIdx_++;
                    auto* fmtGV = new llvm::GlobalVariable(*module_, fmtData->getType(), true,
                        llvm::GlobalValue::PrivateLinkage, fmtData, ".bz.rfmt." + std::to_string(fid));
                    fmtGV->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
                    auto* snpFn = module_->getFunction("snprintf");
                    if (!snpFn) {
                        auto* ft = llvm::FunctionType::get(i32(), {ptrTy(), i64(), ptrTy()}, true);
                        snpFn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "snprintf", module_.get());
                    }
                    auto* i8Ty = llvm::Type::getInt8Ty(llvm_);
                    llvm::Value* buf = entryAlloca(llvm::ArrayType::get(i8Ty, 64));
                    llvm::Value* bufPtr = builder_.CreateGEP(llvm::ArrayType::get(i8Ty, 64), buf, {builder_.getInt64(0), builder_.getInt64(0)});
                    llvm::Value* written = builder_.CreateCall(snpFn, {bufPtr, builder_.getInt64(64), fmtGV, arg}, "snp.ret");
                    auto* mallocFn = declareC("malloc", ptrTy(), {i64()});
                    auto* memcpyFn = declareC("memcpy", ptrTy(), {ptrTy(), ptrTy(), i64()});
                    llvm::Value* len64 = builder_.CreateSExt(written, i64());
                    llvm::Value* dataSz = builder_.CreateAdd(len64, builder_.getInt64(1));
                    llvm::Value* dataCopy = builder_.CreateCall(mallocFn, {dataSz}, "rd.data");
                    builder_.CreateCall(memcpyFn, {dataCopy, bufPtr, len64});
                    llvm::Value* nulPtr = builder_.CreateGEP(i8Ty, dataCopy, len64);
                    builder_.CreateStore(llvm::ConstantInt::get(i8Ty, 0), nulPtr);
                    uint64_t strBytes = module_->getDataLayout().getTypeAllocSize(stringType());
                    llvm::Value* strObj = builder_.CreateCall(mallocFn, {builder_.getInt64(strBytes)}, "rd.obj");
                    builder_.CreateStore(dataCopy, builder_.CreateStructGEP(stringType(), strObj, 0));
                    builder_.CreateStore(len64, builder_.CreateStructGEP(stringType(), strObj, 1));
                    return strObj;
                }
                return nullptr;  // binary radix deferred
            }
        }

        if (cls == "Math") {
            if (call.args.size() == 1 && call.args[0].value) {
                llvm::Value* arg = lowerExpr(*call.args[0].value);
                if (!arg) return nullptr;
                if (m == "Abs") {
                    // Abs for int — select(n<0, -n, n)
                    llvm::Value* neg = builder_.CreateNeg(arg, "abs.neg");
                    llvm::Value* isNeg = builder_.CreateICmpSLT(
                        arg, llvm::ConstantInt::get(arg->getType(), 0));
                    return builder_.CreateSelect(isNeg, neg, arg, "abs.result");
                }
                if (m == "Sqrt" || m == "Floor" || m == "Ceiling" || m == "Round") {
                    // Use llvm intrinsics for common math ops.
                    llvm::Intrinsic::ID iid =
                        m == "Sqrt"    ? llvm::Intrinsic::sqrt :
                        m == "Floor"   ? llvm::Intrinsic::floor :
                        m == "Ceiling" ? llvm::Intrinsic::ceil :
                                         llvm::Intrinsic::round;
                    auto* dbl = llvm::Type::getDoubleTy(llvm_);
                    if (!arg->getType()->isDoubleTy())
                        arg = builder_.CreateSIToFP(arg, dbl);
                    auto* fn = llvm::Intrinsic::getDeclaration(module_.get(), iid, {dbl});
                    return builder_.CreateCall(fn, {arg}, std::string(m) + ".result");
                }
            }
            if (call.args.size() == 2 && call.args[0].value && call.args[1].value) {
                llvm::Value* a = lowerExpr(*call.args[0].value);
                llvm::Value* b = lowerExpr(*call.args[1].value);
                if (!a || !b) return nullptr;
                if (m == "Min") {
                    // Handle int or double operands.
                    if (a->getType()->isIntegerTy()) {
                        llvm::Value* cmp = builder_.CreateICmpSLT(a, b);
                        return builder_.CreateSelect(cmp, a, b, "min.result");
                    } else {
                        // float/double: use llvm.minnum intrinsic
                        auto* dbl = llvm::Type::getDoubleTy(llvm_);
                        if (!a->getType()->isDoubleTy()) a = builder_.CreateSIToFP(a, dbl);
                        if (!b->getType()->isDoubleTy()) b = builder_.CreateSIToFP(b, dbl);
                        auto* fn = llvm::Intrinsic::getDeclaration(module_.get(), llvm::Intrinsic::minnum, {dbl});
                        return builder_.CreateCall(fn, {a, b}, "fmin.result");
                    }
                }
                if (m == "Max") {
                    if (a->getType()->isIntegerTy()) {
                        llvm::Value* cmp = builder_.CreateICmpSGT(a, b);
                        return builder_.CreateSelect(cmp, a, b, "max.result");
                    } else {
                        auto* dbl = llvm::Type::getDoubleTy(llvm_);
                        if (!a->getType()->isDoubleTy()) a = builder_.CreateSIToFP(a, dbl);
                        if (!b->getType()->isDoubleTy()) b = builder_.CreateSIToFP(b, dbl);
                        auto* fn = llvm::Intrinsic::getDeclaration(module_.get(), llvm::Intrinsic::maxnum, {dbl});
                        return builder_.CreateCall(fn, {a, b}, "fmax.result");
                    }
                }
                // Math.Pow(b, e): double — libc pow()
                if (m == "Pow") {
                    auto* dbl = llvm::Type::getDoubleTy(llvm_);
                    if (!a->getType()->isDoubleTy()) a = builder_.CreateSIToFP(a, dbl);
                    if (!b->getType()->isDoubleTy()) b = builder_.CreateSIToFP(b, dbl);
                    auto* powFn = module_->getFunction("pow");
                    if (!powFn) {
                        auto* ft = llvm::FunctionType::get(dbl, {dbl, dbl}, false);
                        powFn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "pow", module_.get());
                    }
                    return builder_.CreateCall(powFn, {a, b}, "pow.result");
                }
            }
            // Math.PI, Math.E — named constants.
            if (call.args.empty()) {
                auto* dbl = llvm::Type::getDoubleTy(llvm_);
                if (m == "PI") return llvm::ConstantFP::get(dbl, 3.14159265358979323846);
                if (m == "E")  return llvm::ConstantFP::get(dbl, 2.71828182845904523536);
            }
            // Math.Log(x), Math.Log2(x), Math.Log10(x)
            if (call.args.size() == 1 && call.args[0].value && (m == "Log" || m == "Log2" || m == "Log10")) {
                llvm::Value* arg = lowerExpr(*call.args[0].value);
                if (!arg) return nullptr;
                auto* dbl = llvm::Type::getDoubleTy(llvm_);
                if (!arg->getType()->isDoubleTy()) arg = builder_.CreateSIToFP(arg, dbl);
                const char* cname = m == "Log2" ? "log2" : m == "Log10" ? "log10" : "log";
                auto* logFn = module_->getFunction(cname);
                if (!logFn) {
                    auto* ft = llvm::FunctionType::get(dbl, {dbl}, false);
                    logFn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, cname, module_.get());
                }
                return builder_.CreateCall(logFn, {arg}, std::string(m) + ".result");
            }
        }
        return nullptr;
    }

    // §3.4: static methods on primitive type names — `int.Parse(s)`,
    // `double.Parse(s)`, etc. The target is a primitive keyword identifier.
    llvm::Value* tryLowerPrimitiveStaticCall(const ast::CallExpr& call) {
        if (!call.callee) return nullptr;
        auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(call.callee.get());
        if (!ma || !ma->target) return nullptr;
        auto* id = dynamic_cast<const ast::IdentifierExpr*>(ma->target.get());
        if (!id) return nullptr;
        sema::PrimitiveType pt = sema::primitiveFromName(id->name);
        if (pt == sema::PrimitiveType::None) return nullptr;
        const auto& m = ma->member;
        using P = sema::PrimitiveType;

        // §5: char static classification methods — char.IsDigit(c), etc.
        if (pt == P::Char && call.args.size() == 1 && call.args[0].value) {
            llvm::Value* arg = lowerExpr(*call.args[0].value);
            if (!arg) return nullptr;
            // Widen to i32 for libc functions if needed.
            if (!arg->getType()->isIntegerTy(32))
                arg = builder_.CreateSExt(arg, i32());
            // Map Bezel method names to libc functions.
            const char* fnName = nullptr;
            if (m == "IsDigit") fnName = "isdigit";
            else if (m == "IsLetter") fnName = "isalpha";
            else if (m == "IsLetterOrDigit") fnName = "isalnum";
            else if (m == "IsWhiteSpace") fnName = "isspace";
            else if (m == "IsUpper") fnName = "isupper";
            else if (m == "IsLower") fnName = "islower";
            else if (m == "IsPunctuation") fnName = "ispunct";
            if (fnName) {
                auto* fn = module_->getFunction(fnName);
                if (!fn) {
                    auto* ft = llvm::FunctionType::get(i32(), {i32()}, false);
                    fn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, fnName, module_.get());
                }
                llvm::Value* res = builder_.CreateCall(fn, {arg}, std::string(fnName) + ".res");
                return builder_.CreateICmpNE(res, builder_.getInt32(0), std::string(fnName) + ".bool");
            }
            if (m == "ToUpper") {
                auto* fn = module_->getFunction("toupper");
                if (!fn) {
                    auto* ft = llvm::FunctionType::get(i32(), {i32()}, false);
                    fn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "toupper", module_.get());
                }
                return builder_.CreateCall(fn, {arg}, "toupper.result");
            }
            if (m == "ToLower") {
                auto* fn = module_->getFunction("tolower");
                if (!fn) {
                    auto* ft = llvm::FunctionType::get(i32(), {i32()}, false);
                    fn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "tolower", module_.get());
                }
                return builder_.CreateCall(fn, {arg}, "tolower.result");
            }
        }

        // string.IsNullOrEmpty(s): bool — true if s is null or length 0.
        if (pt == P::String && m == "IsNullOrEmpty" &&
            call.args.size() == 1 && call.args[0].value) {
            llvm::Value* arg = lowerExpr(*call.args[0].value);
            if (!arg) return nullptr;
            // null → true
            llvm::Value* isNull = builder_.CreateICmpEQ(
                arg, llvm::ConstantPointerNull::get(ptrTy()), "ine.null");
            auto* nullBB = llvm::BasicBlock::Create(llvm_, "ine.null", currentFn_);
            auto* checkBB= llvm::BasicBlock::Create(llvm_, "ine.check", currentFn_);
            auto* endBB  = llvm::BasicBlock::Create(llvm_, "ine.end",   currentFn_);
            auto* entBB  = builder_.GetInsertBlock();
            builder_.CreateCondBr(isNull, endBB, checkBB);
            builder_.SetInsertPoint(checkBB);
            llvm::Value* len = loadStringLen(arg);
            llvm::Value* empty = builder_.CreateICmpEQ(len, builder_.getInt64(0), "ine.empty");
            auto* chkEnd = builder_.GetInsertBlock();
            builder_.CreateBr(endBB);
            builder_.SetInsertPoint(endBB);
            auto* phi = builder_.CreatePHI(i1(), 2, "ine.result");
            phi->addIncoming(builder_.getTrue(), entBB);
            phi->addIncoming(empty, chkEnd);
            return phi;
        }
        // string.Empty → empty string literal.
        if (pt == P::String && m == "Empty" && call.args.empty()) {
            return lowerStringLiteral("\"\"", false);
        }

        // int.Parse(s) / long.Parse(s) / double.Parse(s) etc.
        if (m == "Parse" && call.args.size() == 1 && call.args[0].value) {
            llvm::Value* arg = lowerExpr(*call.args[0].value);
            if (!arg) return nullptr;
            // If the argument is a string, use atoi/atoll/atof.
            auto* argTy = ctx_.exprType(*call.args[0].value);
            if (argTy && argTy->kind == sema::TypeRef::Kind::Primitive &&
                argTy->primitive == P::String) {
                llvm::Value* data = loadStringData(arg);
                if (pt == P::Int || pt == P::UInt || pt == P::Short ||
                    pt == P::UShort || pt == P::Byte || pt == P::SByte) {
                    auto* atoiFn = module_->getFunction("atoi");
                    if (!atoiFn) {
                        auto* ft = llvm::FunctionType::get(i32(), {ptrTy()}, false);
                        atoiFn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "atoi", module_.get());
                    }
                    llvm::Value* r = builder_.CreateCall(atoiFn, {data});
                    // Narrow if needed.
                    if (pt != P::Int && pt != P::UInt) r = builder_.CreateTrunc(r, lowerPrimitive(pt));
                    return r;
                }
                if (pt == P::Long || pt == P::ULong) {
                    auto* atollFn = module_->getFunction("atoll");
                    if (!atollFn) {
                        auto* ft = llvm::FunctionType::get(i64(), {ptrTy()}, false);
                        atollFn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "atoll", module_.get());
                    }
                    return builder_.CreateCall(atollFn, {data});
                }
                if (pt == P::Double || pt == P::Float) {
                    auto* atofFn = module_->getFunction("atof");
                    if (!atofFn) {
                        auto* ft = llvm::FunctionType::get(llvm::Type::getDoubleTy(llvm_), {ptrTy()}, false);
                        atofFn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "atof", module_.get());
                    }
                    llvm::Value* d = builder_.CreateCall(atofFn, {data});
                    if (pt == P::Float) d = builder_.CreateFPTrunc(d, llvm::Type::getFloatTy(llvm_));
                    return d;
                }
            }
            // If already the right type, just return it.
            return arg;
        }
        return nullptr;
    }

    llvm::Value* lowerCall(const ast::CallExpr& call) {
        if (!call.callee) return nullptr;
        // CG-console: Console.Write / Console.WriteLine / Console.WriteLine()
        if (tryLowerConsoleCall(call)) return nullptr;  // unit result
        // Console.ReadLine() → string? (returns a value, not unit)
        if (call.args.empty() && call.callee) {
            if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(call.callee.get())) {
                if (ma->member == "ReadLine" && ma->target) {
                    if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(ma->target.get())) {
                        if (id->name == "Console") return emitConsoleReadLine();
                    }
                }
            }
        }
        // §3.4: primitive type static methods: int.Parse(s), double.Parse(s), etc.
        if (auto* v = tryLowerPrimitiveStaticCall(call)) return v;
        // Built-in class methods: Convert.ToInt32, Math.Abs, etc.
        if (auto* v = tryLowerBuiltinClassCall(call)) return v;
        // CG44: explicit conversion `int(x)` (§5.2) — a callee that is a
        // primitive type name (no resolved symbol) converts its argument.
        if (auto* cid =
                dynamic_cast<const ast::IdentifierExpr*>(call.callee.get())) {
            if (!ctx_.exprSymbol(*call.callee)) {
                sema::PrimitiveType pt = sema::primitiveFromName(cid->name);
                if (pt != sema::PrimitiveType::None)
                    return lowerConversion(pt, call);
            }
        }
        sema::Symbol* calleeSym = ctx_.exprSymbol(*call.callee);
        // CG44: an enum cast `E(i)` (§5.4) — the callee is an enum type
        // name. An enum is represented by its underlying integer (i32), so
        // the argument, width-adjusted to i32, is the enum value.
        if (auto* ets = dynamic_cast<sema::TypeSymbol*>(calleeSym)) {
            if (ets->kind == ast::TypeDeclKind::Enum)
                return lowerEnumCast(call);
            // §9.5: records are constructed with `new Record(...)` (handled in
            // lowerNew), not call-style `Record(...)`. A bare call on a record
            // type name is not construction and is rejected in sema.
        }
        // CG26: a call to an extern (FFI) function — direct C-ABI call to
        // the external declaration. (Native functions don't set the
        // exception global, so no post-call unwind check is needed.)
        if (auto* xd = dynamic_cast<sema::ExternDeclSymbol*>(calleeSym)) {
            auto it = externFns_.find(xd);
            if (it == externFns_.end()) return nullptr;
            std::vector<llvm::Value*> args;
            for (const auto& a : call.args) {
                if (!a.value) return nullptr;
                llvm::Value* v = lowerExpr(*a.value);
                if (!v) return nullptr;
                args.push_back(v);
            }
            if (args.size() != it->second->arg_size()) return nullptr;
            return builder_.CreateCall(it->second, args);
        }
        auto* ms = dynamic_cast<sema::MethodSymbol*>(calleeSym);
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
            // CG29: a record method receives `this` as a pointer to the record
            // value; if the receiver is a struct value (the usual case), spill
            // it to a stack slot and pass its address.
            if (ms->parent && ms->parent->kind == ast::TypeDeclKind::Record &&
                !recv->getType()->isPointerTy()) {
                llvm::Value* tmp = entryAlloca(recordStruct(ms->parent));
                builder_.CreateStore(recv, tmp);
                recv = tmp;
            }
            args.push_back(recv);
        }
        // §22.4/§22.7: place arguments into parameter order (named by name,
        // positional by position, omitted parameters defaulted).
        if (auto* md = dynamic_cast<const ast::MethodDecl*>(ms->decl)) {
            if (!buildOrderedArgs(md->params, call.args, args)) return nullptr;
        } else {
            for (const auto& a : call.args) {
                if (!a.value) return nullptr;
                llvm::Value* v = lowerExpr(*a.value);
                if (!v) return nullptr;
                args.push_back(v);
            }
        }

        return emitMethodCall(ms, args, baseQualified);
    }

    // CG17/CG18/CG35: emit a call to `ms` with fully-built `args` (receiver
    // first for instance methods). Dynamic dispatch loads the receiver's
    // vtable pointer (instance slot 0) and calls through the method's slot
    // — the interface-method slot when `ms` is an interface method (CG35),
    // else the virtual slot (CG17); works for abstract/interface methods
    // (no concrete function) since the slot is filled by the runtime class.
    // A `base.M()` call (baseQualified) bypasses dispatch (§12.3), as do
    // non-virtual non-interface methods.
    llvm::Value* emitMethodCall(sema::MethodSymbol* ms,
                                std::vector<llvm::Value*>& args,
                                bool baseQualified) {
        bool canDispatch = !baseQualified && !isStaticMethod(ms);
        unsigned dispatchSlot = 0;
        if (canDispatch) {
            auto ifIt = ifaceMethodSlot_.find(ms);
            if (ifIt != ifaceMethodSlot_.end()) {
                dispatchSlot = ifIt->second;
            } else {
                auto slotIt = vtableSlot_.find(ms);
                if (slotIt != vtableSlot_.end()) dispatchSlot = slotIt->second;
                else canDispatch = false;
            }
        }
        if (canDispatch) {
            llvm::FunctionType* fnTy = methodFnType(ms);
            if (!fnTy || args.size() != fnTy->getNumParams()) return nullptr;
            llvm::Value* recv = args[0];
            llvm::Value* vptr = builder_.CreateLoad(ptrTy(), recv);
            llvm::Value* slotPtr = builder_.CreateGEP(
                ptrTy(), vptr, {builder_.getInt32(dispatchSlot)});
            llvm::Value* fnPtr = builder_.CreateLoad(ptrTy(), slotPtr);
            llvm::Value* r = builder_.CreateCall(fnTy, fnPtr, args);
            emitPendingCheck();  // CG20: a callee may have thrown
            return r;
        }

        // Non-virtual (or base-qualified) → direct call to the function.
        auto it = methodFns_.find(ms);
        if (it == methodFns_.end()) return nullptr;
        llvm::Function* callee = it->second;
        if (args.size() != callee->arg_size()) return nullptr;
        llvm::Value* r = builder_.CreateCall(callee, args);
        emitPendingCheck();  // CG20: a callee may have thrown
        return r;
    }

    // CG42: null-conditional call `x?.M(args)` (§7.5) — the callee is a
    // null-conditional member access. If the receiver is null the call is
    // skipped (result null / no effect); otherwise the method is invoked.
    // Handles void-returning calls (the `user?.Log(...)` idiom — result
    // discarded) and reference-returning calls (result is the nullable
    // pointer, null when the receiver was null). Value-typed returns need a
    // nullable-value representation and are deferred.
    llvm::Value* lowerNullCondCall(const ast::CallExpr& call,
                                   const ast::NullCondMemberExpr& nm) {
        auto* ms = dynamic_cast<sema::MethodSymbol*>(ctx_.exprSymbol(*call.callee));
        if (!ms || isStaticMethod(ms) || !nm.target) return nullptr;
        llvm::FunctionType* fnTy = methodFnType(ms);
        if (!fnTy) return nullptr;
        llvm::Type* retTy = fnTy->getReturnType();
        bool refResult = retTy->isPointerTy();
        if (!retTy->isVoidTy() && !refResult)
            return nullptr;  // value-typed result deferred
        llvm::Value* recv = lowerExpr(*nm.target);
        if (!recv || !recv->getType()->isPointerTy()) return nullptr;

        auto* entryBB = builder_.GetInsertBlock();
        auto* callBB =
            llvm::BasicBlock::Create(llvm_, "nullcall.do", currentFn_);
        auto* endBB =
            llvm::BasicBlock::Create(llvm_, "nullcall.end", currentFn_);
        llvm::Value* isNull = builder_.CreateICmpEQ(
            recv, llvm::ConstantPointerNull::get(ptrTy()));
        builder_.CreateCondBr(isNull, endBB, callBB);

        builder_.SetInsertPoint(callBB);
        std::vector<llvm::Value*> args;
        args.push_back(recv);
        for (const auto& a : call.args) {
            if (!a.value) return nullptr;
            llvm::Value* v = lowerExpr(*a.value);
            if (!v) return nullptr;
            args.push_back(v);
        }
        llvm::Value* r = emitMethodCall(ms, args, /*baseQualified=*/false);
        if (!r && refResult) return nullptr;
        auto* callEnd = builder_.GetInsertBlock();
        builder_.CreateBr(endBB);

        builder_.SetInsertPoint(endBB);
        if (!refResult) return nullptr;  // void: result discarded by caller
        auto* phi = builder_.CreatePHI(ptrTy(), 2);
        phi->addIncoming(llvm::ConstantPointerNull::get(ptrTy()), entryBB);
        phi->addIncoming(r, callEnd);
        return phi;
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
        for (const auto& p : md->params)
            paramTys.push_back(isByRefMod(p.modifier)
                                   ? ptrTy()  // CG50: ref/out by reference
                                   : lowerType(p.type.get()));
        return llvm::FunctionType::get(retTy, paramTys, /*isVarArg=*/false);
    }

    // CG4: the conditional expression `thenValue if condition else
    // elseValue` (§20). Lowers to a branch + phi so only the taken
    // branch's value expression is evaluated (lazy, per spec).
    // CG28: a `case` expression (§19). The scrutinee is evaluated once;
    // each arm's pattern (+ optional `when` guard) is tested in order; the
    // first match's body value is the result. A non-matching value raises
    // MatchFailureException (§19.6). Handles constant, relational-operator,
    // and discard patterns; binding/type/range/destructure patterns and
    // block-body arms are deferred.
    // CG38: statement-position `case` (§19.4, §19.6). Evaluates the
    // scrutinee once, tests each arm's pattern (+ optional `when` guard) in
    // order, and runs the first matching arm's body (a single statement or
    // a `do … end` block — both parsed as a `Block` arm). An unmatched
    // value falls through with no action (§19.6) — unlike the expression
    // form, no MatchFailureException. Returns false: the fall-through edge
    // keeps the post-case block reachable.
    // CG27b: is an expression's sema type a string (Primitive::String)?
    bool isStringExprType(const ast::Expr& e) {
        auto* rt = ctx_.exprType(e);
        return rt && rt->kind == sema::TypeRef::Kind::Primitive &&
               rt->primitive == sema::PrimitiveType::String;
    }

    bool lowerCaseStmt(const ast::CaseStmt& cs) {
        if (!cs.scrutinee) return false;  // scrutinee-less form deferred
        llvm::Value* scrut = lowerExpr(*cs.scrutinee);
        if (!scrut) return false;
        const sema::TypeSymbol* enumTy = enumTypeOf(*cs.scrutinee);
        bool isStr = isStringExprType(*cs.scrutinee);
        auto* endBB =
            llvm::BasicBlock::Create(llvm_, "casestmt.end", currentFn_);
        for (const auto& arm : cs.arms) {
            llvm::Value* pm = patternMatches(arm.pattern.get(), scrut, enumTy,
                                             isStr);
            if (!pm) {  // deferred pattern kind — degrade to no-op
                builder_.CreateBr(endBB);
                builder_.SetInsertPoint(endBB);
                return false;
            }
            auto* bodyBB =
                llvm::BasicBlock::Create(llvm_, "casestmt.arm", currentFn_);
            auto* nextBB =
                llvm::BasicBlock::Create(llvm_, "casestmt.next", currentFn_);
            if (arm.guard) {
                auto* guardBB = llvm::BasicBlock::Create(
                    llvm_, "casestmt.guard", currentFn_);
                builder_.CreateCondBr(pm, guardBB, nextBB);
                builder_.SetInsertPoint(guardBB);
                llvm::Value* g = lowerExpr(*arm.guard);
                if (!g) return false;
                builder_.CreateCondBr(g, bodyBB, nextBB);
            } else {
                builder_.CreateCondBr(pm, bodyBB, nextBB);
            }
            builder_.SetInsertPoint(bodyBB);
            bool armTerm = lowerScopedBlock(arm.stmts);
            if (!armTerm) builder_.CreateBr(endBB);
            builder_.SetInsertPoint(nextBB);
        }
        // No arm matched → fall through (§19.6).
        builder_.CreateBr(endBB);
        builder_.SetInsertPoint(endBB);
        return false;
    }

    llvm::Value* lowerCaseExpr(const ast::CaseExpr& ce) {
        if (!ce.scrutinee) return nullptr;  // scrutinee-less form deferred
        llvm::Value* scrut = lowerExpr(*ce.scrutinee);
        if (!scrut) return nullptr;
        const sema::TypeSymbol* enumTy = enumTypeOf(*ce.scrutinee);
        bool isStr = isStringExprType(*ce.scrutinee);
        llvm::Type* resultTy = i32();
        if (auto* rt = ctx_.exprType(ce)) resultTy = lowerTypeRef(rt);

        auto* endBB = llvm::BasicBlock::Create(llvm_, "case.end", currentFn_);
        std::vector<std::pair<llvm::Value*, llvm::BasicBlock*>> incomings;

        for (const auto& arm : ce.arms) {
            if (arm.bodyKind != ast::CaseArmBodyKind::Expression ||
                !arm.exprBody)
                return nullptr;  // block-body arms deferred
            llvm::Value* pm = patternMatches(arm.pattern.get(), scrut, enumTy,
                                             isStr);
            if (!pm) return nullptr;
            auto* bodyBB =
                llvm::BasicBlock::Create(llvm_, "case.arm", currentFn_);
            auto* nextBB =
                llvm::BasicBlock::Create(llvm_, "case.next", currentFn_);
            if (arm.guard) {
                auto* guardBB =
                    llvm::BasicBlock::Create(llvm_, "case.guard", currentFn_);
                builder_.CreateCondBr(pm, guardBB, nextBB);
                builder_.SetInsertPoint(guardBB);
                llvm::Value* g = lowerExpr(*arm.guard);
                if (!g) return nullptr;
                builder_.CreateCondBr(g, bodyBB, nextBB);
            } else {
                builder_.CreateCondBr(pm, bodyBB, nextBB);
            }
            builder_.SetInsertPoint(bodyBB);
            llvm::Value* v = lowerExpr(*arm.exprBody);
            if (!v) return nullptr;
            incomings.push_back({v, builder_.GetInsertBlock()});
            builder_.CreateBr(endBB);
            builder_.SetInsertPoint(nextBB);
        }
        // No arm matched → MatchFailureException (§19.6).
        emitThrowBuiltin("MatchFailureException");

        builder_.SetInsertPoint(endBB);
        if (incomings.empty()) return llvm::Constant::getNullValue(resultTy);
        auto* phi = builder_.CreatePHI(incomings[0].first->getType(),
                                       incomings.size());
        for (auto& [v, bb] : incomings) phi->addIncoming(v, bb);
        return phi;
    }

    // CG28: does `pattern` match `scrut`? Returns an i1.
    llvm::Value* patternMatches(const ast::Pattern* pat, llvm::Value* scrut,
                                const sema::TypeSymbol* enumTy = nullptr,
                                bool isStr = false) {
        if (dynamic_cast<const ast::DiscardPattern*>(pat))
            return builder_.getInt1(true);
        // CG43: a bare qualified name `E.Red` parses as a NamedPattern; when
        // the scrutinee is an enum, resolve it to that enum's constant and
        // test equality. (A name denoting a type — a type-test pattern — is
        // still deferred.)
        if (auto* np = dynamic_cast<const ast::NamedPattern*>(pat)) {
            if (enumTy && np->name && !np->name->parts.empty()) {
                if (llvm::Value* cv =
                        enumConstantByName(enumTy, np->name->parts.back()))
                    return emitEquals(scrut, cv, isStr);
            }
            return nullptr;  // type-test pattern deferred
        }
        if (auto* cp = dynamic_cast<const ast::ConstantPattern*>(pat)) {
            llvm::Value* any = builder_.getInt1(false);
            for (const auto& c : cp->constants) {
                if (!c) continue;
                llvm::Value* cv = lowerExpr(*c);
                // CG43: an enum-constant alternative (`E.Red`, `E.Green`) has
                // no resolved symbol on the pattern expression, so lowerExpr
                // misses; resolve it by name against the scrutinee's enum.
                if (!cv) cv = enumConstantOfPatternExpr(*c, enumTy);
                if (!cv) return nullptr;
                // CG27b: string scrutinee → content comparison (not ptr cmp).
                any = builder_.CreateOr(any, emitEquals(scrut, cv, isStr));
            }
            return any;
        }
        if (auto* rp = dynamic_cast<const ast::RelOpPattern*>(pat)) {
            llvm::Value* rv = rp->expr ? lowerExpr(*rp->expr) : nullptr;
            if (!rv) return nullptr;
            bool fp = scrut->getType()->isFloatingPointTy();
            if (rp->op == "<")
                return fp ? builder_.CreateFCmpOLT(scrut, rv)
                          : builder_.CreateICmpSLT(scrut, rv);
            if (rp->op == ">")
                return fp ? builder_.CreateFCmpOGT(scrut, rv)
                          : builder_.CreateICmpSGT(scrut, rv);
            if (rp->op == "<=")
                return fp ? builder_.CreateFCmpOLE(scrut, rv)
                          : builder_.CreateICmpSLE(scrut, rv);
            if (rp->op == ">=")
                return fp ? builder_.CreateFCmpOGE(scrut, rv)
                          : builder_.CreateICmpSGE(scrut, rv);
        }
        // §19: range patterns `lo..hi` and `lo..<hi` — same semantics as
        // the `in` operator but in a case arm. The pattern matches when
        // lo <= scrut <= hi (inclusive) or lo <= scrut < hi (half-open).
        if (auto* rp = dynamic_cast<const ast::RangePattern*>(pat)) {
            auto* range = dynamic_cast<const ast::RangeExpr*>(
                rp->rangeExpr.get());
            if (range && range->lo && range->hi && !range->step) {
                llvm::Value* lo = lowerExpr(*range->lo);
                llvm::Value* hi = lowerExpr(*range->hi);
                if (!lo || !hi) return nullptr;
                llvm::Value* geL = builder_.CreateICmpSGE(scrut, lo);
                llvm::Value* leH = range->inclusive
                                       ? builder_.CreateICmpSLE(scrut, hi)
                                       : builder_.CreateICmpSLT(scrut, hi);
                return builder_.CreateAnd(geL, leH);
            }
        }
        return nullptr;  // binding/type/null/destructure — deferred
    }

    // CG28: equality of two same-typed scalars (int or FP).
    llvm::Value* emitEquals(llvm::Value* a, llvm::Value* b,
                            bool isStr = false) {
        if (isStr) return emitStringEq(a, b);
        return a->getType()->isFloatingPointTy()
                   ? builder_.CreateFCmpOEQ(a, b)
                   : builder_.CreateICmpEQ(a, b);
    }

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
        bool thenDiv = diverged_;  // CG40: e.g. `cond ? throw E : b`
        llvm::BasicBlock* thenEnd = builder_.GetInsertBlock();
        if (!thenDiv) builder_.CreateBr(endBB);

        builder_.SetInsertPoint(elseBB);
        llvm::Value* ev = lowerExpr(*c.elseValue);
        bool elseDiv = diverged_;
        llvm::BasicBlock* elseEnd = builder_.GetInsertBlock();
        if (!elseDiv) builder_.CreateBr(endBB);

        builder_.SetInsertPoint(endBB);
        if (!tv || !ev) return nullptr;
        // CG40: a branch that diverged (threw) does not reach endBB, so it
        // is excluded from the join. When only one branch reaches, its
        // value is the result; when both diverge, endBB is unreachable.
        diverged_ = thenDiv && elseDiv;
        if (thenDiv && elseDiv) return llvm::PoisonValue::get(tv->getType());
        if (thenDiv) return ev;
        if (elseDiv) return tv;
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
