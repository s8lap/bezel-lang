#pragma once

// Private implementation header for the LLVM code generator (decision #147).
//
// Declares the per-module `Emitter` worker class whose methods are defined
// across several translation units (LlvmBackend.cpp and LlvmBackend_*.cpp),
// mirroring how LLVM/Clang split CodeGenFunction across CGExpr.cpp/CGStmt.cpp/
// etc. This header is included ONLY by those implementation files; the public
// surface stays in LlvmBackend.hpp (emitModule/emitObjectFile). Keeping the
// class out of the public header preserves the encapsulation the previous
// anonymous-namespace placement gave, while allowing the definitions to live
// in separately-compiled units.

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <map>
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
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/raw_ostream.h"

namespace bezel::codegen {

// Parse a Bezel integer literal lexeme to its value. Slice CG1 handles
// decimal and the `0x` / `0b` / `0o` bases, with `_` digit separators
// stripped and any trailing type-suffix letters ignored. Returns 0 on a
// shape it doesn't recognize (the lexer already validated the literal).
inline std::int64_t parseIntLiteral(std::string_view text) {
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
inline std::int64_t parseCharLiteral(std::string_view text) {
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

    // CG21c (§31.2): when a `return`/`break`/`continue` exits a `try`-with-
    // `finally` block, the finally body must run before the actual jump.
    // Each active try-with-finally pushes a frame; innermost is at back.
    struct FinallyFrame {
        llvm::BasicBlock* finNormalBB;  // the finally block for normal exits
        llvm::AllocaInst* exitModSlot;  // i32: 0=fall-through, 1=return, 2=break, 3=cont
        llvm::AllocaInst* retValSlot;   // alloca for return value (null if void)
        llvm::Type*       retTy;        // function return type
        llvm::BasicBlock* afterBB;      // continuation after the try
        llvm::BasicBlock* breakBB;      // outer loop break target at try-entry time
        llvm::BasicBlock* contBB;       // outer loop continue target at try-entry time
    };
    std::vector<FinallyFrame> finallyStack_;

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

    // CG-generic: generic type substitution — maps type-parameter names to
    // their concrete LLVM types during the lowering of a specific instantiation
    // (e.g. Box<int>: {"T" → i32}). Set by lowerNew before calling classStruct
    // and kept active through the constructor; also set during field access.
    std::unordered_map<std::string_view, llvm::Type*> genericSubst_;
    // specializedStructs_[ts][field_types] = cached specialized StructType
    std::map<const sema::TypeSymbol*,
             std::map<std::vector<llvm::Type*>, llvm::StructType*>>
        specializedStructs_;
    // specializedMethods_[(ms, subst_key)] = specialized LLVM function
    std::map<std::pair<const sema::MethodSymbol*,
                       std::vector<llvm::Type*>>,
             llvm::Function*>
        specializedMethods_;
    // localGenericSubst_[decl] = concrete subst when local was created.
    // Populated when a local is assigned from a generic expression. Used
    // to recover the concrete type args when calling methods on that local.
    std::unordered_map<const ast::Node*,
                       std::unordered_map<std::string_view, llvm::Type*>>
        localGenericSubst_;
    // lastCallGenericSubst_: the most recently active genericSubst_ during a
    // method call. Set by lowerCallImpl before restoring; read by lowerLocalDecl
    // to associate a local with the concrete types of its initializer.
    std::unordered_map<std::string_view, llvm::Type*> lastCallGenericSubst_;
    // genericSubstSemaTy_: parallel to genericSubst_, maps generic param name
    // to the sema TypeRef of the concrete substitution.  Used in binary
    // expression lowering to distinguish string vs. class pointer equality.
    std::unordered_map<std::string_view, const sema::TypeRef*> genericSubstSemaTy_;
    // expectedFunctionType_: set by buildOrderedArgs when processing a lambda
    // argument for a function-typed parameter.  lowerLambda reads this to
    // infer the LLVM types for unannotated lambda parameters.
    const sema::TypeRef* expectedFunctionType_ = nullptr;
    // lambdaParamTypeHint_: maps ParameterSymbol* → sema TypeRef* for lambda
    // params whose type is inferred from expectedFunctionType_ (no declType AST
    // node).  Used in identifier lookup to supply the missing sema type so that
    // string subscript, member access, etc. work on inferred-type params.
    std::unordered_map<const sema::Symbol*, sema::TypeRef*> lambdaParamTypeHint_;
    // currentEnumMethod_: when we're emitting an enum instance method, set to
    // the enum TypeSymbol so that `case this` knows the scrutinee's enum type.
    const sema::TypeSymbol* currentEnumMethod_ = nullptr;

    // Block-form property getter functions (§22.9 `get => do...end;`).
    // Keyed by PropertyDecl so the same getter function is reused across
    // multiple access sites (non-generic; generic getters are specialised).
    std::unordered_map<const ast::PropertyDecl*, llvm::Function*>
        propBlockGetterFns_;

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
    llvm::Type* lowerPrimitive(sema::PrimitiveType p);

    // §4.6: lazily get (or create) the %bz_string struct type.
    // Layout: { ptr data, i64 length, i8 width }
    //   data  : pointer to the code-point array (elements are width bytes each)
    //   length: number of Unicode code points (NOT bytes)
    //   width : bytes per code point — 1 (≤U+00FF), 2 (≤U+FFFF), or 4 (full)
    // This is the Python PEP 393 / Bezel §4.6 "per-string storage width" model.
    llvm::StructType* stringType() {
        if (!stringStructTy_)
            stringStructTy_ = llvm::StructType::create(
                llvm_, {ptrTy(), i64(), llvm::Type::getInt8Ty(llvm_)}, "bz.string");
        return stringStructTy_;
    }

    // Load the data pointer (field 0) from a bz_string pointer.
    llvm::Value* loadStringData(llvm::Value* strPtr) {
        llvm::Value* p = builder_.CreateStructGEP(stringType(), strPtr, 0);
        return builder_.CreateLoad(ptrTy(), p, "str.data");
    }

    // Load the code-point count (field 1) from a bz_string pointer.
    llvm::Value* loadStringLen(llvm::Value* strPtr) {
        llvm::Value* p = builder_.CreateStructGEP(stringType(), strPtr, 1);
        return builder_.CreateLoad(i64(), p, "str.len");
    }

    // Load the per-code-point byte width (field 2) from a bz_string pointer.
    llvm::Value* loadStringWidth(llvm::Value* strPtr) {
        auto* i8Ty = llvm::Type::getInt8Ty(llvm_);
        llvm::Value* p = builder_.CreateStructGEP(stringType(), strPtr, 2);
        return builder_.CreateLoad(i8Ty, p, "str.width");
    }

    // Return the total byte size of the string data (len * width).
    llvm::Value* loadStringByteSize(llvm::Value* strPtr) {
        llvm::Value* len = loadStringLen(strPtr);
        llvm::Value* w = builder_.CreateZExt(loadStringWidth(strPtr), i64(), "str.w64");
        return builder_.CreateMul(len, w, "str.bytesize");
    }

    // Load the code point at position idx (i64) as i32. Runtime-dispatches on
    // width so that all three widths are handled correctly.
    llvm::Value* loadCodePoint(llvm::Value* strPtr, llvm::Value* idx);

    // CG27a: lazily declare an external C function (for memcmp, malloc, memcpy).
    llvm::Function* declareC(const char* name, llvm::Type* ret,
                             std::vector<llvm::Type*> args) {
        if (auto* f = module_->getFunction(name)) return f;
        auto* ft = llvm::FunctionType::get(ret, args, /*variadic=*/false);
        return llvm::Function::Create(
            ft, llvm::Function::ExternalLinkage, name, module_.get());
    }

    // §4.6 / §18.8: decode string literal source text to a sequence of
    // Unicode code points. This is the canonical internal form; callers
    // then choose the storage width based on the largest code point.
    // `text` is the full source token including surrounding quotes.
    static std::vector<uint32_t> processStringCodePoints(
            std::string_view text, bool verbatim);

    // Determine the minimum storage width for a sequence of code points.
    static uint8_t codePointsWidth(const std::vector<uint32_t>& cps);

    // Build an LLVM constant data array for a code-point sequence at the
    // given storage width. The array is null-terminated (extra element = 0).
    llvm::Constant* makeCodePointArray(const std::vector<uint32_t>& cps,
                                        uint8_t width);

    // Backward-compat wrapper: returns Latin-1/ASCII byte string.
    // Used by format-string detection and other ASCII-only paths.
    static std::string processStringText(std::string_view text, bool verbatim);

    // §4.6: materialize a string literal as a private global %bz_string
    // constant with the correct per-string storage width.
    llvm::Value* lowerStringLiteral(std::string_view text, bool verbatim);

    // §4.6: content equality of two bz_string pointers → i1.
    // Compares code-point sequences. For equal-width strings, uses memcmp on
    // len*width bytes (fast, correct). For different-width strings, uses
    // code-point-by-code-point comparison (correct for all widths).
    llvm::Value* emitStringEq(llvm::Value* l, llvm::Value* r);

    // §4.6: string concatenation. Width of result = max(wL, wR).
    // When both strings have the same width: memcpy. Different widths: element loop.
    llvm::Value* emitStringConcat(llvm::Value* l, llvm::Value* r);

    // §4.6: Store code point cp (i32) at byte pointer ptr with dstW64 bytes per
    // element (1, 2, or 4). Correct block structure: exactly one terminator per
    // block. After return, builder insert point is at the "scp.e" merge block.
    //
    // Block layout (B = current block on entry):
    //   B        → condBr(is4, s4Block, chk2Block)
    //   s4Block  → store i32; br seBlock
    //   chk2Block→ condBr(is2, s2Block, s1Block)
    //   s2Block  → store i16; br seBlock
    //   s1Block  → store i8;  br seBlock
    //   seBlock  ← (builder left here on return)
    void emitStoreCodePoint(llvm::Value* cp, llvm::Value* ptr, llvm::Value* dstW64);

    // CG27c (§5): emit `.ToString()` for a Bezel primitive value.
    // §18.5: emit a formatted conversion using a Bezel format specifier
    // (e.g. "F2" → "%.2f", "X" → "%X", "D5" → "%5d"). Returns the bz_string,
    // or null if the format spec is unrecognized.
    llvm::Value* emitWithFormatSpec(llvm::Value* val,
                                    const std::string& spec,
                                    const sema::TypeRef* rt);

    // Helper: emit snprintf(buf, 64, fmt, val) → bz_string.
    llvm::Value* emitSnprintfToString(const std::string& fmt,
                                      llvm::Value* val);

    // Uses snprintf to format into a stack buffer, then malloc a bz_string.
    // The format specifier depends on the primitive type.
    llvm::Value* emitPrimitiveToString(llvm::Value* val,
                                       sema::PrimitiveType prim);

    // §8 / §9 / §10: compiler-synthesized value-semantics members for records
    // and enums. Each returns the lowered value, or null if the member is not
    // a synthesized one (so callers fall through to ordinary dispatch).
    //
    // §9.1: record ToString → `"TypeName(Field1: value1, Field2: value2)"`,
    // built from the field values in declaration order (each field's value is
    // formatted by its own type's ToString — recursing into nested records).
    llvm::Value* emitRecordToString(llvm::Value* recVal,
                                    const sema::TypeSymbol* rec);
    // §8.3 / §9.1: record GetHashCode → fold the per-field hashes in
    // declaration order with hash = hash*31 + fieldHash. Consistent with the
    // structural equality emitted by emitStructEquality.
    llvm::Value* emitRecordHashCode(llvm::Value* recVal,
                                    const sema::TypeSymbol* rec);
    // §8.5: enum ToString → the type's qualified name (the spec default for
    // enums; member-name formatting is left to user code via Type.EnumNames).
    // §8.3: enum GetHashCode → the underlying integer value.
    // Helper that converts a single field/element value of static type
    // `fieldTy` to its string form (used by emitRecordToString).
    llvm::Value* emitValueToString(llvm::Value* val,
                                   const sema::TypeRef* fieldTy);
    // Helper that computes the i32 hash of a single field value of static
    // type `fieldTy` (used by emitRecordHashCode).
    llvm::Value* emitValueHashCode(llvm::Value* val,
                                   const sema::TypeRef* fieldTy);
    // CG: intercept `.ToString()`, `.GetHashCode()`, and `.Equals(x)` on a
    // record- or enum-typed receiver (the value-semantics members the compiler
    // synthesizes, §8/§9/§10). Returns the lowered value, or null if not such
    // a call (caller falls through to ordinary method dispatch).
    llvm::Value* tryLowerValueSemanticsCall(const ast::CallExpr& call);
    // The resolved sema TypeRef of a record field's declared type (positional
    // Param or body FieldDecl). Null if it cannot be determined.
    const sema::TypeRef* recordFieldType(const sema::FieldSymbol* fs);

    // CG27b (§4.6): ordinal comparison of two bz_string pointers → i32.
    // Returns negative / 0 / positive (same sign contract as memcmp/strcmp).
    // Ordinal = code-point-by-code-point; shorter string is "less" when it
    // is a prefix of the longer one (per §4.6 length-aware comparison).
    llvm::Value* emitStringOrdCmp(llvm::Value* l, llvm::Value* r);

    // CG27f (stdlib §9.6): intercept stdlib string method calls.
    // Returns the lowered value, or null if the method is not recognised.
    llvm::Value* tryLowerStringMethod(const ast::CallExpr& call,
                                       const ast::MemberAccessExpr& ma);

    // CG25: is a Bezel primitive an unsigned integer type? (Drives
    // zext/udiv/lshr/unsigned-comparison choices.)
    static bool isUnsignedPrim(sema::PrimitiveType p);

    // CG25b: does either operand of a binary expression have an unsigned
    // integer type (drives unsigned op selection)?
    bool isUnsignedBinOperand(const ast::BinaryExpr& bin);

    // CG31: is the assignment/increment target an unsigned-typed lvalue?
    bool isUnsignedTargetExpr(const ast::Expr& e);

    // CG31: the address (and element type) of an assignable target — a
    // local/parameter slot, a static-field global, a `this.f`/`obj.f`
    // field, or a bare implicit-`this` field. Null if not assignable.
    llvm::Value* lvalueAddr(const ast::Expr& target, llvm::Type*& elemTy);

    // CG31: apply a compound-assignment/increment arithmetic or bitwise
    // operator (op without the trailing `=`). Integer `+ - *` and `/ mod`
    // are overflow/zero-checked exactly as in lowerBinary; bitwise/shift
    // are exact; floats use IEEE ops.
    llvm::Value* emitCompoundOp(const std::string& op, llvm::Value* l,
                                llvm::Value* r, bool fp, bool uns);

    // Lower a Bezel type (AST node) to its LLVM representation.
    llvm::Type* lowerType(const ast::Type* t);

    // Lower a resolved TypeRef. `()`/null → void; primitives → scalars;
    // class/interface references → opaque pointer (heap reference).
    // Other kinds fall back to i32 (refined as more types are lowered).
    // CG-nullable: helpers for nullable primitive value types (int?, bool?, etc.)
    // Represented as a packed struct {i1 hasValue; primType value} per §18.9.
    // Is rt a nullable VALUE primitive (int?, bool?, double?, etc.)?
    // Excludes reference-like primitives (String, Char, Object) and None.
    // These are the types represented as tagged {i8, primTy} structs.
    bool isNullablePrimTy(const sema::TypeRef* rt);
    // {i8 hasValue, primType value}  — i8 instead of i1 avoids x86 JIT ABI issues
    // (LLVM can't emit physreg-copy for i1 in struct return/param contexts).
    llvm::Type* nullablePrimHasValTy() { return llvm::Type::getInt8Ty(llvm_); }
    llvm::StructType* nullablePrimStructTy(sema::PrimitiveType pt);
    // Constant null for a nullable primitive: {i8 0, primType 0}
    llvm::Constant* nullablePrimNull(sema::PrimitiveType pt) {
        return llvm::ConstantAggregateZero::get(nullablePrimStructTy(pt));
    }
    // Wrap a primitive value into a nullable: {i8 1, value}
    llvm::Value* wrapNullablePrim(sema::PrimitiveType pt, llvm::Value* val);
    // Extract the hasValue field (nonzero = has value, 0 = null) as i1.
    llvm::Value* nullablePrimHasValue(llvm::Value* nv) {
        llvm::Value* hv8 = builder_.CreateExtractValue(nv, {0u}, "has.val8");
        return builder_.CreateICmpNE(hv8,
            llvm::ConstantInt::get(nullablePrimHasValTy(), 0), "has.val");
    }
    // Extract the primitive value field (valid only if hasValue is true).
    llvm::Value* nullablePrimValue(llvm::Value* nv) {
        return builder_.CreateExtractValue(nv, {1u}, "nv.val");
    }

    llvm::Type* lowerTypeRef(const sema::TypeRef* rt);

    // §22.10: find the indexer PropertySymbol on a TypeSymbol, if any.
    const sema::PropertySymbol* findIndexer(
        const sema::TypeSymbol* ts) const;

    // §22.10: emit the getter of an indexer property via inline body.
    // Returns the loaded value, or null if no indexer applies.
    llvm::Value* lowerIndexerGet(const ast::IndexExpr& ix);

    // §22.10: emit the setter of an indexer property via inline body.
    // Returns true if the setter was handled.
    bool lowerIndexerSet(const ast::IndexExpr& ix, llvm::Value* val);

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
    const ast::Expr* computedGetterOf(sema::Symbol* sym);

    // §22.9: return the block-form getter accessor (get => do...end;) if the
    // property has one, else null.  Arrow-expression getters are handled by
    // computedGetterOf above.
    const ast::PropertyAccessor* blockGetterOf(sema::Symbol* sym);

    // Emit (or retrieve cached) a standalone LLVM function for a block-form
    // property getter body (`get => do…end;`).  Calling it with `this` as the
    // sole argument returns the getter's value — avoids premature `ret` in the
    // outer method when `return` appears inside the getter.
    llvm::Function* ensurePropertyBlockGetter(const ast::PropertyDecl* pd,
                                              const ast::PropertyAccessor* acc);

    // §22.9: the block-form setter accessor of the property `target` names,
    // or null (auto-properties have backing storage and use the normal
    // store path; a non-property target is not one). Used to run a custom
    // setter on `obj.P := v`.
    const ast::PropertyAccessor* computedSetterOf(const ast::Expr& target);

    // §22.9: run a block-form property setter for `obj.P := value`. Binds
    // `value` (anchored on the PropertyDecl in sema) to a slot, sets `this`
    // to the receiver, and lowers the setter body. Returns false (so the
    // caller falls through to the normal store path) when `target` is not a
    // block-setter property.
    bool lowerPropertySet(const ast::Expr& target, const ast::Expr& value);

    // §22.9: run a block-form property setter with an already-lowered value
    // `v` (the `value` parameter). Shared by the plain `obj.P := …` path and
    // the compound-assignment read-modify-write path. Returns false when
    // `target` is not a block-setter property.
    bool lowerPropertySetValue(const ast::Expr& target, llvm::Value* v);

    // §22.9: compound assignment `obj.P OP= rhs` on a block-form property —
    // a read-modify-write: read the current value through the getter, apply
    // OP (overflow-checked, as for any compound assignment), then run the
    // setter with the result. Returns false when `target` is not a
    // block-setter property (so the caller uses the normal lvalue path).
    // NOTE: the receiver is currently lowered once for the getter read and
    // again for the setter write; this is observable only for receivers with
    // side effects, which are not expressible as assignment targets here.
    bool lowerPropertyCompound(const ast::AssignmentStmt& as);

    // The LLVM type backing an instance-storage member (field or
    // auto-property).
    llvm::Type* storageType(const sema::Symbol* m);

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
    //
    // CG-generic: when genericSubst_ is non-empty (during lowering of a
    // generic instantiation like Box<int>), the struct is built with the
    // substituted field types. Specializations are cached separately so
    // Box<int> and Box<string> get distinct LLVM struct types.
    llvm::StructType* classStruct(const sema::TypeSymbol* t);

    // CG29 (§9.4): the LLVM struct backing a record value. Unlike a class,
    // a record has no vtable header — its data fields start at index 0. The
    // fields are the record's storage members in declaration order: positional
    // fields and explicit body fields (FieldSymbol), plus body auto-properties
    // (PropertySymbol, §9.2 — backed by compiler-generated storage like a
    // field). `fieldIndex_` is populated so field access reuses the same GEP
    // path as classes. Records do not inherit (§9.6), so there is no
    // prefix-layout walk.
    llvm::StructType* recordStruct(const sema::TypeSymbol* t);

    // §9.1 / §9.2: a record's instance storage members in declaration order —
    // non-static FieldSymbols (positional + explicit body fields) and
    // auto-property PropertySymbols (body-record storage). The same ordering
    // recordStruct lays out, and the order the value-semantics synthesis
    // (Equals / GetHashCode / ToString) iterates.
    void collectRecordStorage(const sema::TypeSymbol* t,
                              std::vector<const sema::Symbol*>& out) {
        for (const sema::Symbol* m : t->members) {
            auto* fs = dynamic_cast<const sema::FieldSymbol*>(m);
            if (fs && !isStaticField(fs)) { out.push_back(m); continue; }
            if (sema::isAutoPropertySymbol(m)) out.push_back(m);
        }
    }

    // §9.1 / §9.2: the resolved declared type of a record storage member —
    // a positional Param, a body FieldDecl, or an auto-property PropertyDecl.
    // Null if it cannot be determined.
    const sema::TypeRef* recordMemberType(const sema::Symbol* m);

    // §33: the entry-point method's LLVM function is named `main`; every
    // other static method gets a mangled, collision-free name.
    std::string functionName(sema::MethodSymbol* ms);

    // CG12: a static field becomes an internal global. Its initializer
    // is the literal constant when present (int/bool literal), else the
    // zero value. Non-literal static initializers (which the spec runs in
    // a static constructor, §22) are deferred — the global is zeroed.
    // CG26: declare an external (C-ABI) function for each extern method
    // (§32). The function keeps the declared C name so the JIT/linker
    // resolves it against the host process / libc. FFI types lower via
    // lowerType (int → i32, void return for no type).
    void declareExterns();

    // CG30: compute each enum constant's integer value (§10). Explicit
    // `:= N` sets the value; otherwise it is the previous value + 1
    // (starting at 0). Only integer-literal initializers are evaluated;
    // other constant expressions are deferred.
    void computeEnumValues();

    // CG30: if `e` resolves to an enum constant, its integer value (i32).
    llvm::Value* enumConstant(const ast::Expr& e);

    // CG43: the enum TypeSymbol an expression's type denotes, or null. Uses
    // the expression's resolved type when present, else falls back to a
    // local/parameter's declared type (a scrutinee identifier often carries
    // only an exprSymbol, not an exprType).
    const sema::TypeSymbol* enumTypeOf(const ast::Expr& e);

    // CG43: resolve a `case`-pattern constant expression that names an enum
    // constant (`E.Red` member access or a bare `Red`) against the
    // scrutinee's enum, by name. Null if `e` is not such a name.
    llvm::Value* enumConstantOfPatternExpr(const ast::Expr& e,
                                           const sema::TypeSymbol* enumTy);

    // CG43: the value of the enum constant named `name` on `enumTy` (i32),
    // resolved by name — used for `case` patterns, whose enum-constant
    // names sema does not resolve to symbols. Null if no such constant.
    llvm::Value* enumConstantByName(const sema::TypeSymbol* enumTy,
                                    std::string_view name);

    // CG36: a const member (`const X := …`) lowers to its initializer's
    // value (a compile-time constant). Covers bare `X` and `Type.X`.
    llvm::Value* constMemberValue(const ast::Expr& e);

    void declareStatics();

    // The constant initializer for a static field, or zero if absent or
    // non-literal (non-literal static init is deferred — see declareStatics).
    llvm::Constant* constInitFor(const ast::FieldDecl* fd, llvm::Type* ty);

    // Version of lowerStringLiteral that only creates globals (no builder
    // instructions), safe to call during declareStatics.
    llvm::Constant* lowerStringLiteralConst(std::string_view text, bool verbatim);

    // CG12: the backing global of a bare identifier that resolves to a
    // static field, or null if `e` isn't such a reference.
    llvm::GlobalVariable* staticFieldGlobal(const ast::Expr& e);

    // CG8: build the instance layout for every class up front.
    void declareStructs();

    // Pass 1: create a Function (no body) for every method with a body.
    // CG9: instance methods take an extra leading `this` pointer.
    void declareFunctions();

    // CG17: assign vtable slots for a class, base-first. Slot 0 is Dispose
    // (handled at emit time); virtual methods take slots 1+. An override
    // reuses the base method's slot (matched by name — overloaded virtuals
    // are deferred). Records the slot for every virtual/override method so
    // a statically-resolved call can index the runtime object's vtable.
    void buildVtable(const sema::TypeSymbol* t);

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

    void declareVtables();

    // CG19: `ts` and all its transitive base types (classes + interfaces),
    // BFS, cycle-guarded.
    void collectTypeAndBases(const sema::TypeSymbol* ts,
                             std::vector<const sema::TypeSymbol*>& out);

    llvm::GlobalVariable* vtableOf(const sema::TypeSymbol* t);

    // CG22: the §31.4 built-in exception hierarchy (child → parent).
    static const std::vector<std::pair<const char*, const char*>>&
    builtinExceptionHierarchy();

    // CG22: build descriptor + vtable globals for the built-in exception
    // types (which have no user TypeSymbol). A type's descriptor lists its
    // own and all ancestor descriptors so `catch Ancestor` matches.
    void setupBuiltinExceptions();

    // CG22: descriptor global for a catch-arm/throw type name — a user
    // type's descArray_ or a built-in exception's descriptor.
    llvm::GlobalVariable* descriptorForTypeName(const sema::TypeRef* rt,
                                                const ast::Type* tyNode);

    // Returns true if `tt` is a Named type with no user declaration, i.e.,
    // one of the built-in exception types (Exception, ArgumentException, …).
    bool isBuiltinExceptionType(const sema::TypeRef* tt) const {
        if (!tt || tt->kind != sema::TypeRef::Kind::Named) return false;
        return tt->declaration == nullptr;  // built-ins have no TypeSymbol
    }

    // CG22: construct + throw a built-in exception by name, then unwind.
    void emitThrowBuiltin(const char* name);

    // CG20/CG40: throw `excVal` (or re-raise the enclosing catch arm's
    // exception when `excVal` is null and one is in flight): store it to
    // the exception global, dispose the unwound using-vars, and branch to
    // the unwind target. Leaves the current block terminated.
    void emitThrowValue(llvm::Value* excVal);

    // CG22: if `cond` (i1, true = error) holds, throw `name`; otherwise
    // continue. Splits the current block.
    void emitGuardThrow(llvm::Value* cond, const char* name);

    // CG22: a checked add/sub/mul via the with-overflow intrinsic; raises
    // OverflowException when the operation overflows (§6.1).
    llvm::Value* emitCheckedArith(llvm::Intrinsic::ID id, llvm::Value* l,
                                  llvm::Value* r);

    // CG19: lower `operand is T` / `operand is not T` / `operand as T`.
    // `is`/`is not` yield bool; `as` yields the operand reference when the
    // test holds, else null. The runtime check walks the operand's type
    // descriptor for T's descriptor address (an is-a test over classes and
    // implemented interfaces, §12.7).
    llvm::Value* lowerTypeTest(const ast::TypeTestExpr& te);

    // CG19: emit a runtime is-a test — is `targetDesc` present in `obj`'s
    // type-descriptor array (vtable[0])? Null operand → false. Returns i1.
    llvm::Value* emitIsA(llvm::Value* obj, llvm::GlobalVariable* targetDesc);

    // CG13: the class's user destructor body, if it declares one.
    const ast::DestructorDecl* findDestructor(const sema::TypeSymbol* ts);

    // CG13: emit the compiler-generated Dispose body. This slice runs the
    // user destructor body (if any) over `this`. Idempotency (the
    // run-once flag), owned-field auto-disposal (§28.5), and base-class
    // chaining (§28.4) are deferred to later slices — so Dispose is
    // currently correct for classes with a destructor and no owned fields
    // or disposable base. `using var` (the only caller in this slice)
    // invokes it exactly once, so the missing idempotency is not yet
    // observable.
    void emitDisposerBody(const sema::TypeSymbol* ts, llvm::Function* fn);

    // CG18 (§28.5): dispose the class's owned reference fields (class- OR
    // interface-typed) in reverse declaration order, null-checking each.
    // Disposal is DYNAMIC — dispatched through the field object's own
    // vtable slot 0 — so an owned field that holds a subclass (or, for an
    // interface-typed field, whichever class implements it) runs the
    // correct runtime Dispose (§28.4–28.5).
    void emitOwnedFieldDisposals(const sema::TypeSymbol* ts);

    // CG18: call an object's Dispose dynamically — load its vtable pointer
    // (instance slot 0), load vtable slot 0 (Dispose), and call it. The
    // object must be non-null. Resolves to the runtime class's Dispose.
    // CG20: the module global holding the in-flight exception (lazy).
    llvm::GlobalVariable* excGlobal();

    // CG20: the per-function propagate block — entered when an exception
    // is pending and not caught here. It returns a dummy value so the
    // caller's post-call check can unwind further. (CG21 will dispose live
    // owned locals here first.) Built lazily.
    llvm::BasicBlock* propagateBB();

    // CG20: where to branch when an exception becomes pending — the
    // innermost try's catch dispatch, else the function propagate block.
    llvm::BasicBlock* unwindBB();

    // CG21: dispose the using-vars of the scopes being exited as an
    // exception unwinds to the current unwind target — scopes from the
    // top of the stack down to unwindScopeDepth_ (inclusive), innermost
    // first (§28.3 reverse order; nested scopes inner-before-outer).
    void emitUnwindDisposals();

    // CG20/CG21: after a call that may throw, branch to the unwind target
    // when the exception global is non-null (disposing unwound using-vars
    // first), else continue.
    void emitPendingCheck();

    void emitDynamicDispose(llvm::Value* obj);

    // §28.4 step 3: chain to the base class's Dispose. baseTypes[0] is
    // conventionally the base class when it resolves to one.
    void emitBaseDisposeChain(const sema::TypeSymbol* ts);

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

    // §29.3: Pre-scan iterator body to collect all LocalDecl/ForStmt nodes and
    // their types. Returns {node, llvmType} pairs for all locals that need
    // to survive across yield points (i.e., all locals in the body).
    void collectIterLocals(
        const std::vector<std::unique_ptr<ast::Stmt>>& body,
        std::vector<std::pair<const ast::Node*, llvm::Type*>>& out);

    // §29.3: Detect if a block of statements contains any yield return / yield break.
    static bool containsYield(const std::vector<std::unique_ptr<ast::Stmt>>& stmts);

    // §29.3: Collect all YieldStmt pointers in order from a statement list.
    static void collectYields(const std::vector<std::unique_ptr<ast::Stmt>>& stmts,
                               std::vector<const ast::YieldStmt*>& out);

    // §29.3: Emit an iterator method (one containing `yield return`).
    // Creates:
    //   - State struct: { ptr vtable, i32 state, param0..N, T current_val }
    //   - MoveNext(ptr self): bool  — state machine body
    //   - Current getter(ptr self): T
    //   - Dispose(ptr self): void   — no-op
    //   - Module-level vtable for the anonymous IEnumerator<T> class
    //   - The original function becomes GetEnumerator: allocs + fills struct, returns it
    void emitIteratorMethod(sema::MethodSymbol* ms, llvm::Function* fn,
                             const CallableInfo& ci);

    // Iterator state for the currently-emitting iterator MoveNext function.
    llvm::StructType* iterStateStruct_ = nullptr;
    llvm::Value* iterSelf_ = nullptr;
    llvm::Value* iterStateGEP_ = nullptr;
    std::vector<llvm::BasicBlock*> iterStateBBs_;
    llvm::BasicBlock* iterDoneBB_ = nullptr;
    unsigned iterCurrentValField_ = 0;
    unsigned iterNextYieldState_ = 0;
    llvm::Type* iterElemTy_ = nullptr;
    // Maps LocalDecl/ForStmt AST node → struct field index for iterator locals.
    std::unordered_map<const ast::Node*, unsigned> iterLocalFields_;
    // Maps alloca slot → struct field index (for save at yield, restore at state entry).
    std::unordered_map<llvm::AllocaInst*, unsigned> iterAllocaToField_;

    // Pass 2: emit a function's body.
    void emitFunctionBody(sema::MethodSymbol* ms, llvm::Function* fn);

    // CG10: emit a constructor body. Always an instance function (leading
    // `this`); its params bind to the ConstructorSymbol's ParameterSymbols.
    // Field initializers run in `lowerNew` before the call, so the body
    // only runs the explicit statements. Returns void.
    // CG47: emit a static constructor body (§22.7). No `this`, no
    // parameters, no base/this chain — it runs the body statements once to
    // initialize the type's static state.
    void emitStaticConstructorBody(sema::ConstructorSymbol* cs,
                                   llvm::Function* fn);

    void emitConstructorBody(sema::ConstructorSymbol* cs, llvm::Function* fn);

    // CG16: if `s` is a leading constructor chain call (`base(...)` or
    // `this(...)` as an ExpressionStmt-of-CallExpr), return the call and
    // set `isThis`; else null.
    const ast::CallExpr* chainCallOf(const ast::Stmt& s, bool& isThis);

    // CG16: the base class (baseTypes[0] when it is a class), else null.
    const sema::TypeSymbol* baseClassOf(const sema::TypeSymbol* t);

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

    bool hasAnyConstructor(const sema::TypeSymbol* t);

    // §22.4/§22.7: select the constructor of `cls` whose parameters the
    // given arguments can fill (named/positional + defaults). Prefers an
    // exact-arity match, else the first that argsCanFill accepts.
    // Count constructors with exactly n params (excluding `exclude`).
    int ctorArityCount(const sema::TypeSymbol* cls, std::size_t n,
                       const sema::ConstructorSymbol* exclude);

    sema::ConstructorSymbol* ctorForArgs(
        const sema::TypeSymbol* cls,
        const std::vector<ast::CallArgument>& args) {
        if (!cls) return nullptr;
        // If exactly one constructor matches by arity, use it directly.
        if (ctorArityCount(cls, args.size(), nullptr) == 1) {
            if (auto* exact = ctorByArity(cls, args.size(), nullptr)) return exact;
        }
        // Multiple constructors with same arity: resolve by LLVM type
        // matching. Lower each arg, check if the lowered type matches
        // the constructor's parameter type. Use the sema type of each
        // arg (ptr for class/string, i32 for int, etc.) to discriminate.
        if (ctorArityCount(cls, args.size(), nullptr) > 1) {
            // Collect candidate ctors with matching arity.
            std::vector<sema::ConstructorSymbol*> candidates;
            for (sema::Symbol* m : cls->members) {
                auto* cs = dynamic_cast<sema::ConstructorSymbol*>(m);
                if (!cs || cs->isStatic) continue;
                if (cs->params.size() == args.size()) candidates.push_back(cs);
            }
            // For each candidate, check if positional arg sema types are
            // compatible with param sema types. We classify each type into:
            //   0 = bool (i1)
            //   1 = char / integer (i32/i64/etc)
            //   2 = float/double
            //   3 = ptr (class, string, char, nullable, function)
            //  -1 = unknown / don't care
            using P = sema::PrimitiveType;
            auto typeClass = [&](const sema::TypeRef* t) -> int {
                if (!t) return -1;
                if (t->nullable) return 3;
                if (t->kind == sema::TypeRef::Kind::Named) return 3;
                if (t->kind == sema::TypeRef::Kind::Function) return 3;
                if (t->kind == sema::TypeRef::Kind::Primitive) {
                    switch (t->primitive) {
                    case P::Bool:   return 0;
                    case P::Char:   return 1; // char is i32 in LLVM
                    case P::Int:   case P::Long:   case P::Byte:
                    case P::SByte: case P::Short:  case P::UShort:
                    case P::UInt:  case P::ULong:  return 1;
                    case P::Float:  return 2;
                    case P::Double: case P::Decimal: return 2;
                    case P::String: return 3;
                    default: return -1;
                    }
                }
                return -1;
            };
            for (auto* cs : candidates) {
                auto* cd = dynamic_cast<const ast::ConstructorDecl*>(cs->decl);
                if (!cd || cd->params.size() != args.size()) continue;
                bool match = true;
                for (std::size_t pi = 0; pi < args.size() && match; ++pi) {
                    if (!args[pi].value || !cd->params[pi].type) continue;
                    auto* argTy = ctx_.exprType(*args[pi].value);
                    bool argIsNull = dynamic_cast<const ast::NullLiteralExpr*>(
                        args[pi].value.get()) != nullptr;
                    int argCls = argIsNull ? 3 : typeClass(argTy);
                    auto* paramTy = ctx_.resolvedType(*cd->params[pi].type);
                    int paramCls = typeClass(paramTy);
                    // Both known: must agree. Unknown means any match.
                    if (argCls != -1 && paramCls != -1 && argCls != paramCls)
                        match = false;
                }
                if (match) return cs;
            }
            // No perfect type match — fall back to first by arity.
            return candidates.empty() ? nullptr : candidates[0];
        }
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
                       sema::ConstructorSymbol* cs);

    // CG16 (§12.8): the implicit `base()` — run the base class's
    // parameterless ctor (which chains further). If the base has no ctor,
    // recurse to its base so a deeper ctor still runs.
    void emitImplicitBase(const sema::TypeSymbol* cls, llvm::Value* obj);

    // Bind each parameter to an entry-block alloca. When `hasThis`, the
    // first arg is `this` (kept as a value in currentThis_, not a slot —
    // it isn't reassignable). Remaining args map to `paramSyms`.
    void bindParameters(llvm::Function* fn, bool hasThis,
                        const std::vector<sema::Symbol*>* paramSyms);

    // The ParameterSymbol list for a method (the MethodSymbol owning md).
    const std::vector<sema::Symbol*>* paramSymbolsOf(const ast::MethodDecl* md);

    bool emitReturn(llvm::Function* fn, llvm::Value* v);

    void emitImplicitReturn(llvm::Function* fn);

    // CG3/CG8: a local/const declaration. The slot type comes from the
    // initializer's lowered value (so pointer-typed locals like `var p
    // := new T()` work); without an initializer it falls back to the
    // declared type (or i32).
    void lowerLocalDecl(const ast::Node* declNode, const ast::Expr* init,
                        const ast::Type* declType);

    // CG13: register a `using var`'s scope-based disposal (§28.3). The
    // local is disposed at the end of its enclosing block in reverse
    // declaration order. Recorded against the innermost open scope.
    void registerUsingDisposal(const ast::LocalDecl* ld);

    // CG13: the class type of a local — from its declared type, else the
    // resolved type of a `new T(...)` initializer.
    const sema::TypeSymbol* classOfLocal(const ast::LocalDecl* ld);

    // CG13: lower a block as a disposal scope. Pushes a fresh scope; on
    // normal fall-through, disposes its `using var`s in reverse and pops.
    // If the block terminated early (return/break/continue), that
    // terminator already emitted the needed disposals, so we only pop.
    bool lowerScopedBlock(const std::vector<std::unique_ptr<ast::Stmt>>& body,
                          std::size_t startIdx = 0);

    // CG13: emit Dispose calls for one scope's `using var`s, in reverse
    // declaration order (§28.4 ordering).
    void emitDisposals(const std::vector<Disposal>& scope);

    // CG13: dispose every open scope (innermost → outermost) — the
    // cleanup a `return` runs before leaving the method.
    void emitAllDisposals();

    // CG13: dispose scopes from the innermost down to (and including) the
    // loop body's scope — the cleanup `break`/`continue` runs before
    // jumping out of / around the loop body.
    void emitLoopDisposals(std::size_t loopScopeDepth);

    // Lower a statement list into the current block; returns true if the
    // list terminated the block (so the caller stops adding to it).
    bool lowerBody(const std::vector<std::unique_ptr<ast::Stmt>>& body);

    // Lower one statement into the current block. Returns true if the
    // statement terminated the block (a return), so the caller stops.
    // CG3: LocalDecl/LocalConstDecl, AssignmentStmt, ReturnStmt,
    // ExpressionStmt, DiscardStmt.
    bool lowerStmt(const ast::Stmt& s);

    // CG20: lower try/catch (no finally yet — CG21). The protected body is
    // lowered with the unwind target set to a catch-dispatch block; a
    // throw or a pending exception after a call lands there. The dispatch
    // tests each arm's type(s) against the in-flight exception's runtime
    // type (emitIsA on the type descriptor) plus any `when` guard; the
    // first match consumes the exception (clears the global) and runs the
    // arm body. If no arm matches, the exception stays pending and
    // re-propagates to the enclosing unwind target.
    bool lowerTry(const ast::TryStmt& trys);

    // CG5: if / elif* / else. Each condition is tested in its own block;
    // a branch that doesn't terminate falls through to a lazily-created
    // merge block. Returns true only when every path terminates (no
    // merge needed) — so we never leave a danging unterminated block.
    bool lowerIf(const ast::IfStmt& ifs);

    // CG5: while loop. cond block tests, body block loops back.
    // CG11: `continue` targets condBB, `break` targets endBB.
    bool lowerWhile(const ast::WhileStmt& ws);

    // CG51 (§16.5) + CG51b (§16.4): lower `for var i in lo..hi [step k] do … end`
    // over a closed range literal to a counted loop. Returns false when the
    // iterable is not a closed range literal (open-ended / non-range deferred).
    // §16.4: `step k` supports positive (ascending) and constant negative
    // (descending) strides. A non-constant or zero step falls back to the
    // not-yet-implemented diagnostic.
    bool lowerRangeFor(const ast::ForStmt& fs);

    // §15.1: `for var x in a do … end` over an inline [N]T array.
    // Emits an indexed counted loop; the element value `a[i]` is stored in
    // the loop variable slot (same anchor `locals_[&fs]`) at the start of
    // each body iteration so `x` in the body resolves to the current element.
    bool lowerArrayFor(const ast::ForStmt& fs);

    // §4.6 / §29.2: `for var c in s do … end` over a string — iterates
    // each char (i32 code point) in order. For ASCII/UTF-8 strings this
    // iterates bytes; full Unicode code-point iteration deferred.
    bool lowerStringFor(const ast::ForStmt& fs);

    // §29.2: `for var x in obj` over a user-defined IEnumerable<T> type,
    // or over a runtime Array<T>/collection when the iterable's type has
    // a concrete GetEnumerator() / MoveNext() / Current interface.
    // Desugar per spec:
    //   using var __enum := obj.GetEnumerator();
    //   while __enum.MoveNext() do
    //     var x := __enum.Current;
    //     body
    //   end;
    //   __enum.Dispose();   ← via disposeStack_ at scope exit
    // §29.2 (B1): `for var x in items` where items is typed as IEnumerable<T> /
    // IReadOnlyCollection<T> etc. with null declaration (builtin interface).
    // Uses the vtable protocol (MoveNext at slot 2, Current at slot 3) which
    // works for BOTH iterator state machines AND Array<T> (via its vtable).
    // If the actual runtime object is a bz_array_t, falls back to direct loop.
    bool lowerIEnumerableBuiltinFor(const ast::ForStmt& fs);

    bool lowerIEnumerableFor(const ast::ForStmt& fs);

    // stdlib §5: `for var x in <lazy LINQ pipeline>` — when the iterable is a
    // deferred LINQ operator call (or a chain of them), iterate the resulting
    // lazy enumerator via its vtable: MoveNext at slot 2 (-> i32), Current at
    // slot 3 (-> void* pointer to the element bytes). Distinct from the
    // builtin/state-machine path because the lazy enumerators' Current returns
    // a pointer (type-erased), not a value.
    bool lowerLazyLinqFor(const ast::ForStmt& fs);

    // If `e` names a local or parameter, return its storage slot; else null.
    // CG50: if `e` names a ref/out parameter, return its storage address
    // (the incoming pointer) and set `elemTy` to the referent type.
    llvm::Value* refParamAddr(const ast::Expr& e, llvm::Type*& elemTy);

    llvm::AllocaInst* localSlot(const ast::Expr& e);

    // Lower an expression to an LLVM value.
    //   CG1: integer literals.
    //   CG2: integer arithmetic / bitwise / shift binary ops, unary -/~/+.
    llvm::Value* lowerExpr(const ast::Expr& e);

    // CG60: produce a closure struct { thunk_fn, env } for a method
    // reference. For static methods: env = null, thunk ignores env.
    // For instance methods: env = heap-copy of receiver ptr, thunk
    // loads receiver from env.
    // The thunk has calling convention (ptr env, params...) -> ret.
    llvm::Value* makeMethodThunk(sema::MethodSymbol* ms,
                                 llvm::Value* receiverVal);

    // CG9: address of an implicitly-`this`-qualified instance field
    // referenced bare inside an instance method (`_x` rather than
    // `this._x`). Returns the GEP and sets `elemTy`; null if `e` is not
    // such a reference (not an identifier, not a field, or no `this`).
    // CG33: the instance-storage member (field or auto-property) a symbol
    // refers to, or null.
    sema::MemberSymbol* storageMemberOf(sema::Symbol* sym);

    llvm::Value* implicitFieldAddr(const ast::Expr& e, llvm::Type*& elemTy);

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
    // When `rec` is supplied, per-field comparison is driven by each storage
    // member's declared Bezel type (§9.1/§9.2) so string fields compare by
    // content (consistent with GetHashCode) rather than by pointer; nested
    // records recurse with their own type. `rec` may be null (e.g. a defensive
    // fallback), in which case comparison falls back to the LLVM field shapes.
    llvm::Value* emitStructEquality(llvm::Value* l, llvm::Value* r,
                                    llvm::StructType* st,
                                    const sema::TypeSymbol* rec = nullptr);
    // §8.3 / §9.1: compare a single field/element value of static type
    // `fieldTy`. Strings compare by content (§4.6), nested records recurse,
    // primitives/enums/class references compare by value/identity. Used by
    // emitStructEquality (consistent with emitValueHashCode).
    llvm::Value* emitValueEquality(llvm::Value* l, llvm::Value* r,
                                   const sema::TypeRef* fieldTy);

    llvm::Value* lowerBinary(const ast::BinaryExpr& bin);

    // CG: short-circuit `and`/`or` via a branch + phi — the right operand
    // runs only on the deciding path (§17 logical operators).
    llvm::Value* lowerShortCircuit(const ast::BinaryExpr& bin);

    // CG39: null-coalescing `a ?? b` (§17.1 L12). Evaluates `a`; if it is a
    // non-null reference, that is the result; otherwise evaluates and yields
    // `b`. The right operand is not evaluated when `a` is non-null. Operands
    // are reference-typed here (a possibly-null heap pointer); `??` on
    // nullable value types (`int?`, …) is deferred until those have a
    // representation.
    llvm::Value* lowerCoalesce(const ast::BinaryExpr& bin);

    llvm::Value* lowerUnary(const ast::UnaryExpr& un);

    // Declare (once) `ptr @calloc(i64, i64)` from libc — zero-inits the
    // allocation, matching value-type field defaults.
    llvm::FunctionCallee callocFn();

    // CG60: lower a lambda expression. Lambdas are represented as a
    // heap-allocated two-pointer closure struct { fn_ptr, env_ptr }.
    //
    // For non-capturing lambdas: env_ptr = null; the function ignores it.
    // For capturing lambdas: env_ptr points to a heap struct of captured
    // variable values (captured by copy at creation time — per Bezel's
    // value-type capture model).
    //
    // All lambda functions use the calling convention:
    //   (env_ptr: ptr, param1, param2, …) → ret
    // The env_ptr is always the first argument, ensuring a uniform
    // calling convention regardless of capture status.
    //
    // A closure struct is stored as { ptr fn, ptr env } allocated on the
    // heap; its address (an opaque ptr) is the lambda value.
    llvm::Value* lowerLambda(const ast::LambdaExpr& lam);

    // CG8: `new T(...)` — heap-allocate (zeroed) an instance of T and run
    // its field initializers. (Explicit constructors, ownership-driven
    // disposal/free, and constructor args are later slices; trivial
    // instances currently leak — freed once disposal lowering lands.)
    // CG34: `source with f := v, …` (§A.7 object initializer on a `new`
    // expression). Construct the source, then store each named value into
    // the corresponding instance-storage member (field or auto-property).
    llvm::Value* lowerWith(const ast::WithExpr& we);

    llvm::Value* lowerNew(const ast::NewExpr& ne);

    // CG8: address of a field access `obj.f`. Returns the GEP pointer and
    // sets `elemTy` to the field's type. Null if `ma` isn't a field
    // access we can lower.
    llvm::Value* fieldAddr(const ast::MemberAccessExpr& ma,
                           llvm::Type*& elemTy);

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
                                 const ast::CallExpr& call);

    // CG44: lower an enum cast `E(i)` (§5.4). The enum's underlying type is
    // i32, so width-adjust the integer argument to i32. (Out-of-range
    // checks for the checked cast forms are deferred.)
    llvm::Value* lowerEnumCast(const ast::CallExpr& call);

    // §22.4/§22.7: can the given call arguments fill the parameter list?
    // (named arguments matched by name, positionals by position, every
    // remaining parameter defaulted). Pure check — no IR emitted — used to
    // pick a constructor/method overload before lowering its arguments.
    static bool argsCanFill(const std::vector<ast::Param>& params,
                            const std::vector<ast::CallArgument>& args);

    // §22.4/§22.7: lower the call arguments into parameter order, honoring
    // named arguments and default values. Provided arguments are evaluated
    // in source order; defaults afterward in parameter order. Appends to
    // `out`; returns false if an argument can't be placed.
    bool buildOrderedArgs(const std::vector<ast::Param>& params,
                          const std::vector<ast::CallArgument>& args,
                          std::vector<llvm::Value*>& out);

    // CG29 (§9): build a record value from positional arguments — used by
    // `new Record(a, b, …)` (the spec construction form, §9.5). Positional
    // arguments map to the record's fields in declaration order; the value is
    // materialized in a stack slot and returned by value (records are value
    // types, no heap allocation). Named arguments and defaults are a later
    // refinement.
    llvm::Value* buildRecordValue(const sema::TypeSymbol* t,
                                  const std::vector<ast::CallArgument>& args);

    // CG-console: emit a Console.Write/Console.WriteLine call as an
    // intrinsic. Uses libc write(1, data, len) for Write and additionally
    // write(1, "\n", 1) for WriteLine. This provides observable stdout.
    // `value` must be a ptr to a bz_string; if it's an integer or other
    // primitive, the caller converts via emitPrimitiveToString first.
    llvm::Value* emitConsoleWrite(llvm::Value* strPtr, bool newline,
                                   int fd = 1);

    // CG-console: Console.ReadLine() → string? (null at EOF).
    // Uses fgets(buf, 4096, stdin), strips trailing newline, copies to heap.
    llvm::Value* emitConsoleReadLine();

    // Detect `Console.Write(...)` / `Console.WriteLine(...)` /
    // `Console.WriteLine()` (no-arg) as intrinsic call patterns.
    // Returns true and emits code if the pattern matches; false otherwise.
    bool tryLowerConsoleCall(const ast::CallExpr& call);

    // Helper: get the element size (bytes) of the element type of a collection.
    int64_t collectionElemSize(const sema::TypeRef* collTy);

    // Check if the named type (TypeRef with null declaration) is a collection.
    static bool isCollectionTypeName(const sema::TypeRef* rt);

    static bool isDictTypeName(const sema::TypeRef* rt);

    // §3: instance method calls on Array<T>, Dictionary<K,V>, etc.
    // Called from lowerCall before the normal method-symbol dispatch.
    llvm::Value* tryLowerCollectionInstanceCall(const ast::CallExpr& call);

    // stdlib §5: is `rt` a builtin (un-declared) IEnumerable<T> family type —
    // the type carried by a lazy LINQ operator result?
    static bool isLazyEnumerableType(const sema::TypeRef* rt);

    // stdlib §5: lower a deferred LINQ operator call (Where/Select/Take/Skip/
    // Distinct/OrderBy[Descending]) or ToArray. Operators return a lazy
    // enumerator (bz_lazy_*); ToArray materializes it. The receiver may be a
    // concrete collection (wrapped via bz_lazy_from_array) or another lazy
    // enumerator (chained). Returns the lowered value, or null if `call` is not
    // a lazy LINQ operator. `elemRef` is set to the element TypeRef on success.
    llvm::Value* tryLowerLazyLinqCall(const ast::CallExpr& call);

    // stdlib §5: lower a receiver expression to a lazy enumerator pointer. If
    // the receiver is a collection (Array<T> etc.), wraps it with
    // bz_lazy_from_array(esz); if it is already a lazy enumerator
    // (IEnumerable<T>), returns the lowered value unchanged. `elemRef` receives
    // the element TypeRef.
    llvm::Value* lowerToLazyEnumerator(const ast::Expr& recvExpr,
                                       sema::TypeRef*& elemRef);

    // stdlib §5: build a trampoline LLVM function the runtime can call to
    // invoke a Bezel closure on a type-erased element. `kind`:
    //   0 = predicate  i32(closure, elem_ptr)        (loads elemTy, calls, ext)
    //   1 = selector   void(closure, elem_ptr, out)  (calls, stores outTy)
    //   2 = key        i64(closure, elem_ptr)         (loads elemTy, calls, sext)
    // `elemTy` is the input element LLVM type; `outTy` the selector output type.
    llvm::Function* buildLinqTrampoline(int kind, llvm::Type* elemTy,
                                        llvm::Type* outTy);

    // Detect Convert.ToInt32(s) / Convert.ToInt64(s) / Math.Abs(n) etc.
    // as intrinsic static class method calls. Returns a lowered value or null.
    llvm::Value* tryLowerBuiltinClassCall(const ast::CallExpr& call);

    // §3.4: static methods on primitive type names — `int.Parse(s)`,
    // `double.Parse(s)`, etc. The target is a primitive keyword identifier.
    llvm::Value* tryLowerPrimitiveStaticCall(const ast::CallExpr& call);

    // §22.4: Pre-create alloca slots for any `out var x` arguments before the
    // call is dispatched (primitive-static, builtin, or regular method). This
    // ensures lvalueAddr() finds the slot regardless of which code path handles
    // the call. Must be called at the start of lowerCall.
    void ensureOutVarSlots(const ast::CallExpr& call);

    llvm::Value* lowerCall(const ast::CallExpr& call);

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
                                bool baseQualified);

    // CG-generic: compile a specialized version of a block-body method with
    // the current genericSubst_ applied to all types.  The specialized
    // function is cached in specializedMethods_ so each (ms, subst) pair is
    // compiled at most once.  The result is a call to the specialized fn.
    llvm::Value* emitGenericSpecialization(sema::MethodSymbol* ms,
                                           std::vector<llvm::Value*>& args,
                                           const CallableInfo& ci);

    // CG-generic: emit a method body inline with the current genericSubst_
    // active, binding `this` and the parameters to the actual arguments.
    // Used when the method function was compiled for a generic template (T→i32)
    // but the actual call uses concrete types (e.g. double for Box<double>).
    llvm::Value* emitInlineGenericMethod(sema::MethodSymbol* ms,
                                          std::vector<llvm::Value*>& args);

    // CG42: null-conditional call `x?.M(args)` (§7.5) — the callee is a
    // null-conditional member access. If the receiver is null the call is
    // skipped (result null / no effect); otherwise the method is invoked.
    // Handles void-returning calls (the `user?.Log(...)` idiom — result
    // discarded) and reference-returning calls (result is the nullable
    // pointer, null when the receiver was null). Value-typed returns need a
    // nullable-value representation and are deferred.
    llvm::Value* lowerNullCondCall(const ast::CallExpr& call,
                                   const ast::NullCondMemberExpr& nm);

    // CG18: the LLVM function type of an instance/static method — leading
    // `this` for instance methods, then the declared parameters. Used for
    // indirect (vtable) calls, including to abstract methods that have no
    // concrete Function to read the type from.
    llvm::FunctionType* methodFnType(const sema::MethodSymbol* ms);

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
    bool isStringExprType(const ast::Expr& e);

    bool lowerCaseStmt(const ast::CaseStmt& cs);

    llvm::Value* lowerCaseExpr(const ast::CaseExpr& ce);

    // CG28: does `pattern` match `scrut`? Returns an i1.
    llvm::Value* patternMatches(const ast::Pattern* pat, llvm::Value* scrut,
                                const sema::TypeSymbol* enumTy = nullptr,
                                bool isStr = false);

    // CG28: equality of two same-typed scalars (int or FP).
    llvm::Value* emitEquals(llvm::Value* a, llvm::Value* b,
                            bool isStr = false);

    llvm::Value* lowerConditional(const ast::ConditionalExpr& c);
};

}  // namespace bezel::codegen
