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
#include <optional>
#include <string>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <functional>

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
#include "llvm/IR/Verifier.h"
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
    // §4.2 / A.1: the trailing IntSuffix (`U`, `UL`, `LU`, case-insensitive)
    // marks an unsigned literal. A `ulong` value above long.MaxValue (e.g.
    // 18000000000000000000uL) overflows signed strtoll, which saturates to
    // long.MaxValue and loses the value. Parse such literals with the unsigned
    // strtoull so the full 64-bit value is preserved; the caller stores the
    // returned bit pattern into an i64 LLVM constant (issue #421).
    bool isUnsigned = false;
    for (std::size_t s = i; s < text.size(); ++s) {
        char c = text[s];
        if (c == 'u' || c == 'U') { isUnsigned = true; break; }
    }
    if (isUnsigned)
        return static_cast<std::int64_t>(
            std::strtoull(digits.c_str(), nullptr, base));
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
        // #405 / §31.4: build the built-in exception vtables/descriptors BEFORE
        // declareStructs — class layout (lowerTypeRef) consults builtinVtable_ to
        // lower a built-in-exception-typed field to a pointer (it is a reference
        // type, §31.1), so the set must already be populated. setupBuiltin
        // Exceptions only creates module globals + the shared no-op dispose; it
        // depends on nothing declared later.
        setupBuiltinExceptions();  // CG22
        declareStructs();
        declareStatics();
        declareFunctions();
        declareVtables();  // CG17: after functions exist (needs fn ptrs)
        setupBoxDescriptors();  // §8.6: after descArray_/vtable globals exist
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
            // §2.5 (issue #328 item 1): an IMPORTED class's disposerFns_ entry is
            // an EXTERNAL declaration (the exporter holds the body); skip it.
            if (ctx_.isImported(ts)) continue;
            emitDisposerBody(ts, fn);
        }
        // §2.5 / §22.8 (issue #328 item 2+3): emit the exported
        // `bz$<Type>$init` body for every public non-generic class declared in
        // THIS module. (An IMPORTED type's entry in initFns_ is an external
        // declaration — emitInstanceInitFunction is a no-op for it: imported
        // types are never declared with a local init body here, only via
        // declareImportedInit at the `new` site.)
        for (const auto& sym : ctx_.symbols()) {
            auto* ts = dynamic_cast<const sema::TypeSymbol*>(sym.get());
            if (ts && initFns_.count(ts)) emitInstanceInitFunction(ts);
        }
        // FINDING [3]: malformed IR (e.g. a block with two terminators) reached
        // the optimizer as a segfault because nothing checked the IR after
        // emission. The module verifier turns such a defect into a printed
        // report instead of a crash deep in an LLVM pass.
        //
        // The corpus-surfaced malformed-IR defects this verifier originally
        // flagged (bz_string_split arg-count mismatch; dead unterminated
        // ine.null/env.null blocks; the rt.inc continue-target with no
        // terminator; the void-handler lambda defined as i32 / case+conditional
        // all-arms-diverge join blocks) are now FIXED, so the full test suite is
        // verifier-clean. The verifier therefore runs unconditionally in
        // ASSERT builds (!NDEBUG) — early detection of any future malformed-IR
        // regression — and remains opt-in via BEZEL_VERIFY_IR in release builds.
        // It stays NON-FATAL (report-and-continue): a not-yet-exercised program
        // shape that trips it should print a diagnostic, not hard-fail the
        // build/JIT, until that case too is fixed.
#ifndef NDEBUG
        bool wantVerify = true;
#else
        bool wantVerify = std::getenv("BEZEL_VERIFY_IR") != nullptr;
#endif
        if (module_ && wantVerify &&
            llvm::verifyModule(*module_, &llvm::errs())) {
            llvm::errs() << "bezel codegen: IR verification failed "
                            "(malformed IR emitted; see report above)\n";
        }
        return std::move(module_);
    }

private:
    sema::SemaContext& ctx_;
    llvm::LLVMContext& llvm_;
    std::unique_ptr<llvm::Module> module_;
    llvm::IRBuilder<> builder_;
    llvm::Function* currentFn_ = nullptr;
    // §8.6: the declared return-type node of the method currently being
    // emitted, so a `return <value>` whose declared return type is
    // Object/interface boxes the value before returning.
    const ast::Type* currentReturnTypeNode_ = nullptr;
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
    // §27.5: a captured local/parameter is shared by-reference through a
    // heap cell so that mutations inside a lambda are visible outside and
    // vice versa. When a variable is "boxed", its `locals_`/`paramSlots_`
    // alloca no longer holds the value directly — it holds a `ptr` to the
    // heap cell, and the cell holds the value. This map records, for each
    // such alloca, the value's element type. Accessors route reads/writes
    // through the cell (see `boxedStorage`). The set is part of the saved/
    // restored emitter state across nested-function emission.
    std::unordered_map<llvm::AllocaInst*, llvm::Type*> cellVars_;
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
    //
    // per §2.11: a `using var` of type `Subscription` has no user TypeSymbol
    // (it is a runtime-managed opaque handle, not a class with a generated
    // Dispose). For such entries `cls` is null and `subscription` is true; the
    // scope-end path then calls bz_subscription_dispose instead of dispatching
    // through a vtable.
    struct Disposal {
        llvm::AllocaInst* slot;
        const sema::TypeSymbol* cls;
        bool subscription = false;
        // #362 / stdlib §4.5: a `using var` bound to a heap collection
        // (Array<T>/OwnedArray<T>/…). At scope end it is freed via
        // bz_array_free. `collectionElemDisposer` is the per-element dispose
        // callback for an OWNING collection of class elements (null for a
        // borrowing collection or value elements — only the backing is freed).
        bool collection = false;
        llvm::Value* collectionElemDisposer = nullptr;
        // #394 / stdlib §4.9 + #401 / stdlib §4.8: a `using var` bound to a
        // hash-backed dictionary (Dictionary<K,V>/OwnedDictionary<K,V>). At scope
        // end it is freed via bz_dict_free. `dictValueDisposer` is the per-VALUE
        // callback passed to bz_dict_free; it is one of exactly two mutually
        // exclusive things (or null):
        //   * #394: an OwnedDictionary<K,V> with a CLASS value type → the
        //     owning-class value-disposer (dispose + free the object via its
        //     vtable). OwnedDictionary owns its VALUES only.
        //   * #401: a BORROWING Dictionary<K,V> with a RECORD value type → the
        //     value-box-free thunk (plain `free` of the heap box; a record is a
        //     value type, §3.2 / §9, so there is nothing to dispose — only the
        //     box storage to reclaim).
        // It is null for a borrowing Dictionary with a scalar/string value (no
        // box is stored). Keys are never disposed (stdlib §4.9: "Owns its values;
        // keys are value-like"; keys are freed by bz_dict_free's own free(e.key)).
        bool dict = false;
        llvm::Value* dictValueDisposer = nullptr;
        // #383 / §28: a `using var` that owns a class/interface object but
        // whose concrete class is not statically known here (an interface-
        // typed slot, or a call/`move` initializer). Disposed via DYNAMIC
        // dispatch through the object's own vtable (slot 1) — which runs the
        // most-derived destructor + base chain and reclaims the block (#380),
        // exactly like the `cls`-set path but without needing a TypeSymbol in
        // disposerFns_. `cls` is null for these.
        bool dynamicDispose = false;
        // #383 / §31.1: the in-flight exception object caught by a catch arm.
        // Freed (via the object's vtable — destructor + base chain + free) on a
        // NORMAL handled exit of the arm (fall-through, return, break,
        // continue), but SKIPPED on the UNWIND path (a re-raise/throw from the
        // arm keeps it in flight for the enclosing handler). `slot` holds the
        // object pointer; `cls` is null. See emitDisposals' isUnwind parameter.
        bool caughtException = false;
        // §28.3 (line 4910) "Exception-path discharge": a plain `var` owned local
        // or an `owned` parameter. Unlike `using var`, plain owned locals and
        // owned params are NOT auto-disposed on NORMAL exit — §28.3 requires them
        // only to REACH a discharge point statically (move/return/owned-arg/
        // explicit Dispose), which sema enforces. But on an EXCEPTION-UNWIND exit
        // they are auto-disposed "as if wrapped in an implicit try/finally". So
        // an `unwindOnly` entry is SKIPPED by normal-exit emitDisposals and
        // disposed (reclaimed, null-guarded) ONLY on the isUnwind path. `slot`
        // holds the object pointer (via its alloca); `cls` is the static class
        // when known (dispatch is dynamic regardless).
        bool unwindOnly = false;
        // #422 / §4.6 / §28.9: a `var` string local that PROVABLY does not
        // escape its scope (a non-escape walk found every use is a
        // non-consuming read) and was initialized once by a codegen-internal
        // +1 producer (string `+` / interpolation — never a call, so no
        // return-handoff ambiguity). The local holds exactly the producer's
        // single reference; at scope end (normal OR unwind) we
        // bz_string_release it, freeing the heap data+header exactly once. No
        // other reference exists (proven non-escaping), so there is no dangler
        // (no UAF) and it runs once per exit (no double-free). `slot` holds the
        // string pointer; the class/collection/dict fields are unused.
        bool stringRelease = false;
        // #422 / §27.5 / §28.9: a `var` closure (function-typed) local that
        // PROVABLY does not escape its scope (a non-escape walk found every use
        // is a non-consuming read) and was initialized once by a
        // codegen-internal +1 producer (a lambda expression or a method
        // reference — never a call result, so no return-handoff ambiguity). The
        // local holds exactly the producer's single reference; at scope end
        // (normal OR unwind) we bz_closure_release it, freeing the heap cell +
        // its env block exactly once. The closure owns neither its captures nor
        // the boxed cells backing them (they are borrows, §28.9), so the release
        // frees only the cell + env — no captured value is disposed, no
        // double-free of a capture. No other reference to the cell exists
        // (proven non-escaping), so there is no dangler (no UAF) and it runs
        // once per exit (no double-free of the cell). `slot` holds the closure
        // pointer; the class/collection/dict fields are unused.
        bool closureRelease = false;
        // #422 / §8.6 / §28.9: a `var` Object/interface local holding a BOXED
        // VALUE that PROVABLY does not escape its scope (a non-escape walk found
        // every use is a non-consuming read — `is`/`as`/`= null`/`<> null`) and
        // was initialized once by a codegen-internal +1 BOX producer (an
        // initializer whose static type is a boxable value type, so boxValue
        // itself allocated the wrapper). The local holds exactly the producer's
        // single reference; at scope end (normal OR unwind) we bz_box_release it,
        // freeing the box wrapper exactly once. The boxed value is a value-type
        // leaf (no owned sub-objects), so the release frees only the wrapper. A
        // class instance coerced to Object is NEVER a box producer (its
        // initializer has a reference static type → boxValue is not called), so
        // this release is never emitted for a class instance — no double-free of
        // an owned object, no corruption of the class model. No other reference
        // to the box exists (proven non-escaping), so there is no dangler (no
        // UAF) and it runs once per exit (no double-free). `slot` holds the box
        // pointer; the class/collection/dict fields are unused.
        bool boxRelease = false;
    };
    std::vector<std::vector<Disposal>> disposeStack_;
    // §28.3 (line 4910): owned parameters of the current function awaiting
    // unwind-disposal registration. bindParameters collects them (no dispose
    // scope is open yet); lowerScopedBlock registers them into the function-body
    // ROOT scope (the first scope it pushes) so they are auto-disposed on an
    // exception unwind out of the whole method. Cleared once registered.
    std::vector<std::pair<sema::Symbol*, llvm::AllocaInst*>> pendingOwnedParams_;
    // CG13: the compiler-generated Dispose function for each class.
    std::unordered_map<const sema::TypeSymbol*, llvm::Function*> disposerFns_;
    // §2.5 / §22.8 (issue #328 item 2+3): the `bz$<Type>$init(ptr)` function for
    // each public non-generic class (exporter: a definition; importer: an
    // external declaration). Installs the vtable + runs field initializers, so a
    // cross-module `new` runs §22.8 step-1+2 by linking to the exporter's code.
    std::unordered_map<const sema::TypeSymbol*, llvm::Function*> initFns_;
    // #362 / stdlib §4.5: the cached per-element dispose thunk (built lazily).
    llvm::Function* elementDisposerThunk_ = nullptr;
    // #401 / stdlib §4.8: the cached value-box-free thunk (built lazily) for a
    // borrowing Dictionary<K, record>'s heap-boxed record values.
    llvm::Function* valueBoxFreeThunk_ = nullptr;

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
    // #387 / §31.1: the name the enclosing catch arm binds the caught
    // exception to ("" for a bare-type arm with no binding). Used to decide
    // whether a `throw <newExpr>` leaving the arm provably cannot reference
    // the caught exception (see emitThrowValue / exprReferencesName).
    std::string_view currentCaughtName_;
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

    // #383 / §28.8.1: true while emitting a class's compiler-generated Dispose
    // (a destructor body). A `using var u := move this._f;` inside a destructor
    // is NOT registered for disposal: the destructor's auto field-dispose pass
    // (emitOwnedFieldDisposals) already disposes _f, and codegen does not track
    // moved-from state, so registering the moved value too would double-free.
    // The field-restoration rule (§28.8.1) guarantees safety OUTSIDE a
    // destructor (the field is reassigned to null/a fresh object on every exit,
    // and field disposal is null-safe).
    bool emittingDisposerBody_ = false;

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
    // #422 / §27.5 / §28.9: the closure-cell type { ptr fn, ptr env, i64 rc }.
    llvm::StructType* closureCellTy_ = nullptr;
    unsigned stringLiteralIdx_ = 0;

    // CG8: per-class instance layout (struct of instance fields) and each
    // field's index within it.
    std::unordered_map<const sema::TypeSymbol*, llvm::StructType*> structs_;
    std::unordered_map<const sema::Symbol*, unsigned> fieldIndex_;

    // CG12: each static field's backing global variable.
    std::unordered_map<const sema::Symbol*, llvm::GlobalVariable*>
        staticGlobals_;

    // CG17: virtual dispatch. Each class's vtable is a constant array of
    // function pointers: [0]=td$C, [1]=itmap$C, [2]=Dispose, [3..]=virtual
    // methods (override reuses the base method's slot). The hidden vtable
    // pointer lives at instance struct index 0.
    std::unordered_map<const sema::TypeSymbol*, llvm::GlobalVariable*> vtables_;
    std::unordered_map<const sema::MethodSymbol*, unsigned> vtableSlot_;
    // §8.6: per-boxable-value-type box vtable. A boxed value type (record /
    // enum) is a heap wrapper `{ vtable*, value-bytes… }`; its first word
    // points to this vtable so `emitIsA` (`*obj` → vtable, `vtable[0]` →
    // descriptor array) recognizes it uniformly with a class instance. The
    // vtable layout matches the class vtable prefix:
    //   [0]=td, [1]=itmap, [2]=Dispose
    // Interface dispatch on a boxed record goes through bz_itable_lookup.
    // The descriptor (vtable[0]) is the type's descArray_ entry.
    std::unordered_map<const sema::TypeSymbol*, llvm::GlobalVariable*>
        boxVtable_;
    // §8.6: per-primitive box descriptor + vtable (int/bool/string/… have no
    // user TypeSymbol). The descriptor is the 2-element array [self, null] used
    // by `o is int` / `o is string`; the vtable is the 1-element [descriptor]
    // word every box carries at offset 0. Keyed by the primitive's name.
    std::unordered_map<std::string, llvm::GlobalVariable*> primBoxDesc_;
    std::unordered_map<std::string, llvm::GlobalVariable*> primBoxVtable_;
    // Per-class slot→method (vector index i = vtable slot i+3; slots 0..2 are
    // td$, itmap$, Dispose) and the virtual-method-name→slot map used to
    // inherit slots.
    std::unordered_map<const sema::TypeSymbol*,
                       std::vector<sema::MethodSymbol*>> vtableMethods_;
    std::unordered_map<const sema::TypeSymbol*,
                       std::unordered_map<std::string_view, unsigned>>
        vtableNameSlot_;
    // CG35-itable (§13 / §12): per-interface itable dispatch. For each
    // interface method `ms`, ifaceMethodIndex_[ms] is its 0-based position in
    // the interface's non-static method declaration order. This index is used
    // to GEP into `itab$C$I` (the per-(class×interface) itable). The old
    // dense shared-slot region (ifaceMethodSlot_ / ifaceSlotCount_) has been
    // replaced with per-interface itables: vtable[1] = itmap$C (a
    // null-terminated [ td$I, itab$C$I, … ] array); dispatch calls the
    // runtime helper bz_itable_lookup(itmap, td$I) → itab$C$I, then GEPs
    // to position k within the itable to find the function pointer.
    std::unordered_map<const sema::MethodSymbol*, unsigned> ifaceMethodIndex_;
    // Per (class × interface): the itab$C$I global (array of fn ptrs in
    // interface declaration order). Key: (cls, iface).
    std::map<std::pair<const sema::TypeSymbol*, const sema::TypeSymbol*>,
             llvm::GlobalVariable*>
        itables_;
    // Per class: the itmap$C global ([td$I1, itab$C$I1, …, null]).
    std::unordered_map<const sema::TypeSymbol*, llvm::GlobalVariable*>
        itmaps_;
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
    // Layout: { ptr data, i64 length, i8 width, i64 rc }
    //   data  : pointer to the code-point array (elements are width bytes each)
    //   length: number of Unicode code points (NOT bytes)
    //   width : bytes per code point — 1 (≤U+00FF), 2 (≤U+FFFF), or 4 (full)
    //   rc    : #422 reference count — 1 at every heap producer (the producer's
    //           reference); SENTINEL (INT64_MAX) on literal/global strings so
    //           retain/release are no-ops and a constant is never freed. Field 3
    //           was appended AFTER width so fields 0/1/2 keep their StructGEP
    //           indices and the runtime's raw byte offsets (0/8/16) are unmoved.
    // This is the Python PEP 393 / Bezel §4.6 "per-string storage width" model.
    llvm::StructType* stringType() {
        if (!stringStructTy_)
            stringStructTy_ = llvm::StructType::create(
                llvm_, {ptrTy(), i64(), llvm::Type::getInt8Ty(llvm_), i64()},
                "bz.string");
        return stringStructTy_;
    }

    // #422: the sentinel rc value for static/literal strings (matches the
    // runtime's BZ_STRING_RC_SENTINEL). retain/release no-op on it.
    llvm::Constant* stringRcSentinel() {
        return llvm::ConstantInt::get(i64(), 0x7FFFFFFFFFFFFFFFLL);
    }

    // #422: store rc := 1 into a freshly malloc'd bz_string header (field 3).
    // Every heap producer in codegen calls this so the value carries the
    // producer's reference (§4.6 runtime-managed lifetime).
    void initStringRc(llvm::Value* strObj) {
        builder_.CreateStore(llvm::ConstantInt::get(i64(), 1),
                             builder_.CreateStructGEP(stringType(), strObj, 3));
    }

    // #422: emit a call to bz_string_retain(s) — increments rc (no-op on a
    // sentinel/literal or null). Used when a string is copied into a
    // longer-lived location (field store, return handoff, collection insert).
    void emitStringRetain(llvm::Value* s) {
        if (!s) return;
        auto* fn = declareC("bz_string_retain",
                            llvm::Type::getVoidTy(llvm_), {ptrTy()});
        builder_.CreateCall(fn, {s});
    }

    // #422: emit a call to bz_string_release(s) — decrements rc and frees at
    // zero (no-op on a sentinel/literal or null). Used at scope-end release of a
    // string local and on overwrite of a string slot.
    void emitStringRelease(llvm::Value* s) {
        if (!s) return;
        auto* fn = declareC("bz_string_release",
                            llvm::Type::getVoidTy(llvm_), {ptrTy()});
        builder_.CreateCall(fn, {s});
    }

    // #422 / §27.5 / §28.9: the closure-cell heap layout — { ptr fn, ptr env,
    // i64 rc } = 24 bytes. fn (off 0) and env (off 8) keep the offsets the
    // existing closure-call sites use (fn at offset 0, env at GEP index 1); rc
    // (field index 2, off 16) is appended after env so neither moves. Every
    // closure-cell producer (lowerLambda, makeMethodThunk) calloc's this size
    // and stores rc := 1.
    llvm::StructType* closureCellType() {
        if (!closureCellTy_)
            closureCellTy_ = llvm::StructType::create(
                llvm_, {ptrTy(), ptrTy(), i64()}, "bz.closure");
        return closureCellTy_;
    }

    // #422: store rc := 1 into a freshly calloc'd closure cell (field index 2).
    // The producer holds the first reference (§28.9 runtime-managed lifetime).
    void initClosureRc(llvm::Value* cellObj) {
        builder_.CreateStore(
            llvm::ConstantInt::get(i64(), 1),
            builder_.CreateStructGEP(closureCellType(), cellObj, 2));
    }

    // #422: emit a call to bz_closure_retain(c) — increments rc (no-op on null).
    // Used when a closure is copied into a longer-lived location (not armed in
    // this increment; provided for the symmetric retain-on-copy when it lands).
    void emitClosureRetain(llvm::Value* c) {
        if (!c) return;
        auto* fn = declareC("bz_closure_retain",
                            llvm::Type::getVoidTy(llvm_), {ptrTy()});
        builder_.CreateCall(fn, {c});
    }

    // #422: emit a call to bz_closure_release(c) — decrements rc and, at zero,
    // frees the env block then the cell (no-op on null). Captured values are
    // borrows (§28.9), so this frees ONLY the cell + env, never a capture. Used
    // at scope-end release of a non-escaping closure local.
    void emitClosureRelease(llvm::Value* c) {
        if (!c) return;
        auto* fn = declareC("bz_closure_release",
                            llvm::Type::getVoidTy(llvm_), {ptrTy()});
        builder_.CreateCall(fn, {c});
    }

    // #422 / §8.6 / §28.9: emit a call to bz_box_retain(b) — increments the box
    // wrapper's rc (no-op on null). Provided for the symmetric retain-on-copy
    // when it lands; not armed in this increment.
    void emitBoxRetain(llvm::Value* b) {
        if (!b) return;
        auto* fn = declareC("bz_box_retain",
                            llvm::Type::getVoidTy(llvm_), {ptrTy()});
        builder_.CreateCall(fn, {b});
    }

    // #422 / §8.6 / §28.9: emit a call to bz_box_release(b) — decrements the box
    // wrapper's rc and, at zero, frees the wrapper (no-op on null). The boxed
    // value is a value-type leaf (no owned sub-objects), so this frees ONLY the
    // wrapper. Emitted ONLY for a `var` Object/interface local codegen itself
    // boxed (a boxable-value initializer) and proved non-escaping — never for a
    // class instance coerced to Object (which keeps its bare pointer and never
    // becomes a box). Used at scope-end release of such a local.
    void emitBoxRelease(llvm::Value* b) {
        if (!b) return;
        auto* fn = declareC("bz_box_release",
                            llvm::Type::getVoidTy(llvm_), {ptrTy()});
        builder_.CreateCall(fn, {b});
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
    // per §8.3/§9.1: is rt a NULLABLE REFERENCE type — one that lowers to a
    // plain `ptr` whose null is a real null pointer? Covers `string?`,
    // `Object?`, and nullable class/interface fields. (Nullable VALUE
    // primitives go through isNullablePrimTy and carry an explicit hasValue
    // tag instead; they never null-deref.) The synthesized value-semantics
    // members must guard a load of such a field with a null check.
    bool isNullableRefTy(const sema::TypeRef* rt);
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
    // per §10 / issue #364: is rt a NULLABLE ENUM (a nullable Named TypeRef
    // whose declaration is an enum)? A non-nullable enum lowers to its
    // underlying integer (i32 — CG44), so `enum?` would also stay i32, making
    // `null` indistinguishable from the value-0 member. To give `null` a real
    // representation it lowers to the SAME tagged struct {i8 hasValue; i32
    // value} the value primitives use (isNullablePrimTy). This predicate is the
    // enum analogue of isNullablePrimTy; the two are unified by
    // isNullableValueTy below.
    bool isNullableEnumTy(const sema::TypeRef* rt);
    // per §7.2 / §8.6 / issue #416: is rt a NULLABLE RECORD (a nullable Named
    // TypeRef whose declaration is a record)? A non-nullable record lowers to a
    // bare by-value struct (§9.4) with NO null representation, so `record?` would
    // also stay the bare struct and `null` would be indistinguishable from a
    // value — routing `record? <> null` through emitStructEquality (ExtractValue
    // on the `null` operand) and aborting the backend (LLVM checkGEPType). It now
    // lowers, like the value primitives and enums, to a tagged struct
    // {i8 hasValue; <record payload>} so `null` is representable. This predicate
    // is the record analogue of isNullablePrimTy/isNullableEnumTy; the three are
    // unified by isNullableValueTy below. Defined in LlvmBackend_Layout.cpp
    // (needs the record-declaration kind check, like isNullableEnumTy).
    bool isNullableRecordTy(const sema::TypeRef* rt);
    // A nullable VALUE type — a value primitive (isNullablePrimTy), an enum
    // (isNullableEnumTy), or a record (isNullableRecordTy). All lower to the
    // tagged {i8 hasValue; T value} struct; the only difference is T (the
    // primitive's LLVM type, i32 for an enum, or the bare record struct). All
    // null/wrap/value-semantics sites key off this combined predicate.
    bool isNullableValueTy(const sema::TypeRef* rt) {
        return isNullablePrimTy(rt) || isNullableEnumTy(rt) ||
               isNullableRecordTy(rt);
    }
    // The tagged struct for a nullable value type (prim, enum, or record). For
    // an enum the value slot is the underlying integer (i32 — CG44); for a record
    // (§9.4 / issue #416) it is the bare record struct (recordStruct). Defined in
    // LlvmBackend_Layout.cpp where recordStruct is available.
    llvm::StructType* nullableValueStructTy(const sema::TypeRef* rt);
    // Constant null for a nullable value type: {i8 0, T 0}.
    llvm::Constant* nullableValueNull(const sema::TypeRef* rt) {
        return llvm::ConstantAggregateZero::get(nullableValueStructTy(rt));
    }
    // Wrap a plain value (primitive or enum integer) into a nullable: {i8 1, v}.
    llvm::Value* wrapNullableValue(const sema::TypeRef* rt, llvm::Value* val) {
        auto* st = nullableValueStructTy(rt);
        llvm::Value* res = builder_.CreateInsertValue(
            llvm::UndefValue::get(st),
            llvm::ConstantInt::get(nullablePrimHasValTy(), 1), {0u});
        return builder_.CreateInsertValue(res, val, {1u});
    }
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

    // issue #388 / stdlib §4.4-§4.9: lower a HEAP-collection indexer read
    // `a[i]` / `dict[key]` against the runtime backing (bz_array_* / bz_dict_*).
    // Returns the loaded element/value, or null if `ix`'s target is not a
    // built-in heap collection (the caller falls through to other paths).
    llvm::Value* lowerCollectionIndexerGet(const ast::IndexExpr& ix);

    // issue #409 / language spec §28.8.1 / §28.11: lower `move coll[i]` (the
    // slot-take) on an OWNING heap collection (OwnedArray / OwnedDictionary).
    // Reads the element out as owned AND leaves the slot moved-from (null) so the
    // collection's later element-disposer skips it — no #409 double-free. Returns
    // null if `ix` is not an owning-collection slot (caller falls through to the
    // borrowing-read lowering, which does NOT null the slot).
    llvm::Value* lowerMoveFromCollectionSlot(const ast::IndexExpr& ix);

    // issue #388 / stdlib §4.4-§4.9: lower a HEAP-collection indexer setter
    // `a[i] := v` / `dict[key] := v`. For an OWNING collection (OwnedArray /
    // OwnedDictionary) the displaced slot/value is disposed exactly once before
    // the new (owned) one is stored (§4.5 / §4.9); a borrowing collection just
    // overwrites. `val` is the already-lowered RHS. Returns true if handled.
    bool lowerCollectionIndexerSet(const ast::IndexExpr& ix, llvm::Value* val);

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

    // §23.4: emit (or retrieve cached) a standalone LLVM function for a
    // BLOCK-form indexer getter body (`property [i: …]: T get => do…end;`).
    // The signature is (this, index-params…) → T; calling it returns the
    // getter's value. As with the block-form property getter (§22.9) this
    // keeps a `return` inside the getter body from prematurely terminating the
    // enclosing method, and lets a record (aggregate) result flow back by
    // value through the function's return type (§9.4).
    llvm::Function* ensureIndexerBlockGetter(
        const ast::PropertyDecl* pd, const ast::PropertyAccessor* acc,
        const sema::PropertySymbol* ps);

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
            // per §24.1: each (instance, synchronous) event declaration gets a
            // compiler-generated owned backing field of type EventSource<T>,
            // laid out as a plain `ptr` slot. Static events (static owned field)
            // and async events (AsyncEventSource<T>) are deferred — async isn't
            // implemented and static-field events would need static-init.
            if (auto* es = dynamic_cast<const sema::EventSymbol*>(m)) {
                if (auto* ed = dynamic_cast<const ast::EventDecl*>(es->decl)) {
                    if (!ed->isStatic && !ed->isAsync) out.push_back(m);
                }
            }
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

    // §22.12 (issue #328 FINDING [3] BUG A): fold an IMPORTED const's value at
    // the read site. An imported const has no AST decl and no storage symbol —
    // a const initializer is a compile-time constant (§22.12), so the value was
    // carried textually in the dependency metadata and is materialized here as
    // an LLVM constant of the const's type. Null if no value was recorded.
    llvm::Value* importedConstValue(const ast::Expr& e,
                                    sema::ConstMemberSymbol* cs);

    // §22.12 / §22.8 (issue #328 FINDING [3] / [7]): fold a textual, compile-
    // time-constant value (an imported const value, or an imported field's
    // constant initializer) into an LLVM constant of type `rt`. Null when the
    // text is not a foldable literal shape.
    llvm::Value* foldImportedConstText(const std::string& text,
                                       const sema::TypeRef* rt);

    // §2.5 / §22.7 (issue #328 FINDING [7]): the external `bz$<Type>$ctor`
    // function for an IMPORTED type's reconstructed constructor, declared on
    // demand and cached in ctorFns_. The signature is `void(ptr this,
    // params...)`, matching the exporter's emitter, so the importer's `new` call
    // binds to the linked definition.
    llvm::Function* declareImportedCtor(const sema::ConstructorSymbol* cs);

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

    // §2.5 (issue #328 inc 3): create an EXTERNAL `llvm::Function` (declare, not
    // define) for every imported method, so a cross-module call links against
    // the dependency's object. Only called when ctx_.hasImports().
    void declareImportedFunctions();

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

    // #405 / §31.4: precise recognizer — `tt` resolves to one of the language-
    // built-in exception types (it has a built-in vtable from
    // setupBuiltinExceptions). Unlike isBuiltinExceptionType this checks the
    // type NAME against the built-in exception set, so it does not match other
    // declaration-less Named types (collections, primitives). Used by the
    // owned-field / using-var auto-dispose paths to recognize a builtin
    // exception held as an owned field/local (§28.5 / §28.10): such an object is
    // an ordinary owned heap class instance (§31.1) and must be disposed +
    // reclaimed when the owning slot's lifetime ends.
    bool isBuiltinExceptionTypeRef(const sema::TypeRef* tt) const {
        if (!tt || tt->kind != sema::TypeRef::Kind::Named) return false;
        if (tt->declaration != nullptr) return false;  // built-ins have none
        // The type NAME comes from the AST node (a TypeRef carries no name of
        // its own); match it against the built-in exception vtable set.
        const ast::Type* ty = tt->astNode;
        if (!ty || !ty->name || ty->name->parts.empty()) return false;
        return builtinVtable_.find(
                   std::string(ty->name->parts.back())) != builtinVtable_.end();
    }

    // #405 / §28.5: an `owned` field whose type is a BUILTIN exception (e.g.
    // `private owned _e: Exception?;` or a builtin subtype). Built-in exceptions
    // have no user TypeSymbol, so isOwnedReferenceField (which requires a
    // class/interface declaration) does not match them; they are nevertheless
    // ordinary owned heap class instances (§31.1) and the enclosing object's
    // Dispose must dispatch their (no-op) vtable Dispose and reclaim their block.
    bool isOwnedBuiltinExceptionField(const sema::FieldSymbol* fs) {
        if (!hasOwnedModifier(fs)) return false;
        auto* fd = dynamic_cast<const ast::FieldDecl*>(fs->decl);
        if (!fd || !fd->type) return false;
        return isBuiltinExceptionTypeRef(ctx_.resolvedType(*fd->type));
    }

    // CG22: construct + throw a built-in exception by name, then unwind.
    void emitThrowBuiltin(const char* name);

    // CG20/CG40: throw `excVal` (or re-raise the enclosing catch arm's
    // exception when `excVal` is null and one is in flight): store it to
    // the exception global, dispose the unwound using-vars, and branch to
    // the unwind target. Leaves the current block terminated.
    //
    // #387 / §31.1: `excAst` is the AST of the thrown expression (the
    // operand of the `throw`), used only when this `throw` leaves a catch
    // arm by constructing a NEW exception. A caught exception is a borrow
    // whose lifetime is the arm body; constructing a new exception never
    // transfers ownership of the caught one (the spec forbids assigning the
    // borrow to an owned field or otherwise retaining it). So when the new
    // exception's expression provably cannot reference the caught binding,
    // the caught object must be freed at the throw (its lifetime is ending
    // and nothing retains it). When it MIGHT reference it, codegen keeps the
    // status quo — the caught object stays in flight (a residual single-object
    // leak) rather than risk a use-after-free on a possibly-wrapped reference.
    // `excAst` is null for callers that cannot supply it (the expression-
    // position `throw`); those conservatively never free (no UAF).
    void emitThrowValue(llvm::Value* excVal, const ast::Expr* excAst = nullptr);

    // #387 / §31.1: does `e` syntactically reference an identifier named
    // `name` anywhere in its subtree? Fail-safe: an expression shape this
    // walker does not recurse into returns `true` ("might reference"), so an
    // unrecognized form never causes the caught exception to be freed — the
    // conservative direction (a leak, never a use-after-free). Used to decide
    // whether a `throw <newExpr>` from a catch arm can free the caught object.
    bool exprReferencesName(const ast::Expr& e, std::string_view name) const;

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

    // §8.6: build box descriptor + vtable globals for boxable value types
    // (records, enums) and for the boxable primitives + string. Called once
    // during setup, after the class descriptor/vtable globals exist.
    void setupBoxDescriptors();

    // §8.6: the descriptor-array global for a value type's box, by which
    // `o is T` (T a record/enum/primitive/string) recognizes a boxed value.
    // Records/enums reuse descArray_; primitives/string use primBoxDesc_.
    llvm::GlobalVariable* boxDescriptorFor(const sema::TypeRef* rt);

    // §8.6: the box vtable for a boxable value type — vtable[0] is its
    // descriptor; for records the iface-method slots are filled so dispatch on
    // a boxed record reaches the implementing method.
    llvm::GlobalVariable* boxVtableFor(const sema::TypeRef* rt);

    // §8.6: is `rt` a value type that boxes when coerced to Object/interface
    // (record, enum, value primitive, or inline array)? Reference types
    // (class, interface, string, Object, function, heap collections) do not.
    bool isBoxableValueType(const sema::TypeRef* rt);

    // §8.6: is `target` a reference type (Object / interface / Object?) — the
    // kind of declared location that triggers boxing of a value-typed source?
    bool isObjectOrInterfaceTarget(const sema::TypeRef* target);

    // §8.6: box `value` (already lowered, of static type `srcTy`) into a heap
    // wrapper `{ boxVtable*, value-bytes }` and return the box pointer. A
    // nullable value type whose source is null boxes to a null pointer (§8.6
    // Nullable value-type boxing). Returns `value` unchanged when it is not a
    // boxable value (already a reference) or when no descriptor is available.
    llvm::Value* boxValue(llvm::Value* value, const sema::TypeRef* srcTy);

    // §8.6: given a coercion of `init`'s lowered value `v` into a declared
    // location of type `declTy`, box `v` if the location is Object/interface
    // and the source expression is a value type. Returns the (possibly boxed)
    // value; a no-op for reference sources and non-Object targets.
    llvm::Value* boxForTarget(llvm::Value* v, const sema::TypeRef* declTy,
                              const ast::Expr* init);

    // CG13: the class's user destructor body, if it declares one.
    const ast::DestructorDecl* findDestructor(const sema::TypeSymbol* ts);

    // CG13 / §28.2 / §28.4 / §28.6: emit the PUBLIC, idempotent compiler-
    // generated Dispose (vtable slot 1). It checks the per-object run-once flag
    // (§28.6) and, on the first call only, sets it and calls the class's
    // unguarded core disposer (emitDisposerCore). Subsequent calls are no-ops
    // (§28.2). It does NOT free the object's block — reclamation is performed
    // once by the disposal site that owns the slot, so a repeat dispatch never
    // dereferences an already-freed object.
    void emitDisposerBody(const sema::TypeSymbol* ts, llvm::Function* fn);

    // §28.4: emit the UNGUARDED core disposer — runs the user destructor body
    // (if any), then disposes owned-bearing fields in reverse declaration order
    // (§28.5), then chains to the base class's CORE disposer (§28.4 step 3). No
    // run-once flag and no free; the public Dispose guards it.
    void emitDisposerCore(const sema::TypeSymbol* ts, llvm::Function* fn);

    // §28.5: does `ts` declare at least one `owned` field whose type is one of
    // its own generic type parameters? Such a field is classified (and so auto-
    // disposed) at INSTANTIATION, not in the shared type-erased disposer; a
    // class with one needs a per-instantiation specialized Dispose/vtable.
    static bool classHasOwnedTypeParamField(const sema::TypeSymbol* ts);

    // §28.5: for the generic instantiation currently active in `genericSubst_`
    // (e.g. Box<Res>), return the per-instantiation vtable global to install in
    // a freshly-constructed instance, in place of the shared `vt$<T>`. Its slot
    // 1 is a specialized Dispose that classifies and disposes the class's owned
    // type-parameter field(s) using the active substitution (§28.5); all other
    // slots are copied from the shared vtable. Returns the shared vtable when
    // the class needs no specialization. Cached per (class, substituted field
    // types) so each distinct instantiation builds exactly one.
    llvm::GlobalVariable* specializedVtableFor(const sema::TypeSymbol* ts);

    // §28.5: build the specialized core disposer for the active instantiation
    // of `ts` — same shape as emitDisposerCore but emitted with the current
    // `genericSubst_`/`genericSubstSemaTy_` active so that an owned type-
    // parameter field is laid out (spec struct) and classified (substituted
    // type) correctly. Returns the public idempotent Dispose wrapping it.
    llvm::Function* emitSpecializedDisposer(const sema::TypeSymbol* ts,
                                            const std::string& suffix);

    // specializedVtables_[ts][substituted field types] = cached per-
    // instantiation vtable global (slot 1 = specialized Dispose). Keyed by the
    // same field-type vector as specializedStructs_ so distinct instantiations
    // (Box<Res> vs Box<int>) get distinct vtables.
    std::map<const sema::TypeSymbol*,
             std::map<std::vector<llvm::Type*>, llvm::GlobalVariable*>>
        specializedVtables_;

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

    // §22.9 / §28.3 (FINDING [4]): the new-expression variant of emitPendingCheck.
    // When the constructor of an in-flight `new T(...)` throws, the freshly-
    // calloc'd instance `obj` is never bound to a `using var`/scope, so the
    // ordinary scope-end / owned-local unwind disposal never reclaims it — it
    // leaks. The constructor's own cleanup path has already disposed the owned
    // fields it assigned (and the base subobject) WITHOUT freeing `this` (the
    // ctor never frees the block it did not allocate). So on the unwind edge here
    // — the sole site that allocated `obj` — reclaim the instance block exactly
    // once with `free`. This fires ONLY on the throw edge; on normal completion
    // `obj` flows out as a normal owned value disposed later by its owning slot.
    void emitNewPendingCheck(llvm::Value* obj);

    // §28.2 / §28.4: dispatch an object's PUBLIC idempotent Dispose through its
    // vtable (slot 1) — runs the destructor + owned fields + base chain at most
    // once. Does NOT free the block. `obj` must be non-null. Used as the dispatch
    // primitive, and by explicit `obj.Dispose()` on a receiver this scope does
    // NOT own (a temporary/borrow) where reclaiming could double-free (#415).
    void emitDynamicDispose(llvm::Value* obj);

    // #380 / §28: dispatch the idempotent Dispose AND reclaim the heap block —
    // for an auto-dispose site that owns the slot/field and is the last dispose
    // of the object (using-var scope-end, owned-field auto-dispose, exception
    // unwind, caught-exception normal-exit free), and (since #415) for an
    // explicit `obj.Dispose()` whose receiver is a slot/field this scope owns and
    // can null to block a second reclaim. free(null) is a no-op.
    void emitDisposeAndReclaim(llvm::Value* obj);

    // #415 / §28.2 / §28.4: for an explicit `receiver.Dispose()` on a class/
    // interface instance, return the addressable slot/field this scope OWNS and
    // may null to block a second reclaim — i.e. when reclaiming the block at the
    // explicit site is safe (the slot is the sole other reclaim path, which the
    // null then suppresses). Returns the slot address (via lvalueAddr, so boxed/
    // captured locals and owned fields resolve correctly) for:
    //   - an identifier naming a `using var` local (scope owns it);
    //   - an identifier naming a plain `var` owned local (owned by structure —
    //     init is `new C(...)` or an `owned T` return; same test the §28.3
    //     unwind-disposal registration uses);
    //   - an identifier naming an `owned` parameter (§28.8);
    //   - a member access `obj.f` where `f` is an `owned` reference field (§28.5).
    // Returns nullptr for a temporary, a borrow, or a non-owned field — there
    // reclaiming could double-free with the value's real owner, so the caller
    // stays dispatch-only (#415 conservative fallback).
    llvm::Value* explicitDisposeOwnedReceiverSlot(const ast::Expr& receiver,
                                                  llvm::Type*& slotTy);

    // #362 / stdlib §4.5: a module-level `void(void*)` thunk that runs an
    // owning collection element's destructor via dynamic dispatch (vtable
    // slot 1), null-checking the element first. Suitable as the `dispose_elem`
    // callback for bz_array_free / bz_array_clear_disposing /
    // bz_array_remove_at_disposing. Built once per module and cached.
    llvm::Function* elementDisposerThunk();

    // #362 / stdlib §4.5: if `collTy` is an OWNING collection (name prefixed
    // "Owned") whose element type is a class, return the element-disposer thunk
    // (so disposing the collection cascades into its elements); otherwise null.
    // A borrowing collection, or one with value/string elements, has no element
    // disposal obligation and returns null (only the backing is freed).
    llvm::Function* owningCollectionElemDisposer(const sema::TypeRef* collTy);

    // #394 / stdlib §4.9: if `dictTy` is an OwnedDictionary<K,V> whose VALUE
    // type (genericArgs[1]) is a class, return the element-disposer thunk (so
    // freeing the dictionary at scope end cascades disposal into each surviving
    // value); otherwise null. A borrowing Dictionary, or a value-typed V, has no
    // value-disposal obligation and returns null (only the backing table and the
    // malloc'd key bytes are freed). Keys are never disposed (§4.9: keys are
    // value-like; the dictionary owns its values only).
    llvm::Function* owningDictValueDisposer(const sema::TypeRef* dictTy);

    // #401 / stdlib §4.8 + language spec §3.2 / §9: a standalone `void(void*)`
    // thunk that just `free`s the pointer (null-safe), with NO destructor /
    // dispatch. A record value in a (borrowing) Dictionary<K, record> is too
    // wide for the scalar slot encoding, so the runtime heap-BOXES it; that box
    // is the dict's OWN storage (a record is a value type — §3.2 / §9 — so there
    // is nothing to dispose inside it, only memory to free). Used as the
    // `dispose_val` callback for bz_dict_free / the disposing setters when the
    // value is a boxed record. Built once per module and cached.
    llvm::Function* valueBoxFreeThunk();

    // #401 / stdlib §4.8: if `dictTy` is a BORROWING Dictionary<K,V> whose value
    // type V is a record (and therefore heap-boxed, see the encode in
    // LlvmBackend_{Calls,Assign}.cpp), return the value-box-free thunk so the
    // dict's disposal reclaims each surviving value box; otherwise null. This is
    // strictly the value-BOX-free case: an OwnedDictionary<K, class> instead
    // routes through owningDictValueDisposer (#394, dispose+free a class object
    // via its vtable), and a scalar/string-valued borrowing Dictionary stores no
    // box (nothing extra to free). The two are mutually exclusive, so the
    // value box is never freed twice.
    llvm::Function* dictValueBoxFreer(const sema::TypeRef* dictTy);

    // #362 / stdlib §4.5: is genericArgs[idx] of `collTy` a class type (whose
    // runtime objects carry a vtable with a Dispose at slot 1)?
    bool collectionElemIsClass(const sema::TypeRef* collTy, unsigned idx = 0);

    // issue #400 / §25.2: find a user-declared `Equals` override on a CLASS type
    // (walking the base-class chain). `Object.Equals` is a compiler intrinsic
    // with no MethodSymbol in user members, so the absence of a result means the
    // type keeps the §8.3 reference-identity default. Returns null for a class
    // that does not override `Equals`. The match is by name + single parameter
    // (the `Equals(Object?)` shape), the only `Equals` the spec lets a class
    // declare (§8.4: classes customize equality by overriding `Equals`).
    sema::MethodSymbol* classEqualsOverride(const sema::TypeSymbol* cls);

    // issue #400 / §25.2: compute the equality `Array<T>`/`OwnedArray<T>`'s
    // .Remove(item) uses to match `el` against `target`, dispatched
    // by the element's static Bezel type `elemTy`. A class with a user `Equals`
    // override → a (null-guarded) virtual `el.Equals(target)` call (§8.4); a
    // class without one → reference identity (§8.3); a record → structural
    // equality (§8.3, emitStructEquality); a string → content (§4.6); a value
    // type → `==`. `el` and `target` are both pure borrows (no consumption).
    llvm::Value* emitRemoveElementEquality(llvm::Value* el, llvm::Value* target,
                                           const sema::TypeRef* elemTy,
                                           bool elemIsClass);

    // §28.4 step 3: chain to the base class's Dispose. baseTypes[0] is
    // conventionally the base class when it resolves to one.
    void emitBaseDisposeChain(const sema::TypeSymbol* ts);

    // §28.2 / §28.6: per-object "disposed" run-once flag support. The flag is an
    // i8 at struct index 1 in every class struct — right after the vtable
    // pointer and before all data fields (see classStruct), so it is at a fixed
    // offset uniform across the hierarchy and all generic specializations.
    // disposedFlagTy() is its LLVM type and disposedFlagIndex() its index. The
    // most-derived generated Dispose checks/sets it so the destructor body,
    // owned-field auto-disposal and base chain run AT MOST ONCE per object
    // regardless of how many dispose sites reach it (idempotency).
    llvm::Type* disposedFlagTy();
    unsigned disposedFlagIndex(const sema::TypeSymbol* t);

    // §28.4: the UNGUARDED core disposer for each class — runs the destructor
    // body, owned-field auto-disposal, then chains to the base CORE. The public
    // generated Dispose (disposerFns_) wraps this with the run-once flag guard.
    // The base chain calls the base CORE directly so a single per-object flag,
    // set once at the most-derived public entry, never short-circuits a base
    // level's destructor.
    std::unordered_map<const sema::TypeSymbol*, llvm::Function*> disposerCoreFns_;

    static bool hasOwnedModifier(const sema::FieldSymbol* fs) {
        for (auto mod : fs->modifiers) {
            if (mod == "owned") return true;
        }
        return false;
    }

    // §28.5: if `fs` is an `owned` field whose declared type is a generic type
    // PARAMETER of its enclosing class (`private owned _slot: T`), and there is
    // an active monomorphization substitution that binds that parameter to a
    // concrete sema TypeRef, return that concrete TypeRef; otherwise null.
    //
    // A type-parameter reference resolves to a Named TypeRef with a null
    // `declaration` (it is not a real user type) and a single-part name. Whether
    // such a field is an OWNED REFERENCE field (and so auto-disposed by §28.4
    // step 2) is decided at *instantiation*, per §28.5 — the substituted type is
    // what matters, not the bare parameter. The disposer that consults this is
    // emitted PER INSTANTIATION (see emitSpecializedDisposer), so the active
    // `genericSubstSemaTy_` names the concrete bound type for THIS instantiation
    // (e.g. T → Res when emitting Box<Res>'s disposer).
    const sema::TypeRef* ownedTypeParamConcreteType(const sema::FieldSymbol* fs) {
        if (!hasOwnedModifier(fs)) return nullptr;
        if (genericSubstSemaTy_.empty()) return nullptr;
        auto* fd = dynamic_cast<const ast::FieldDecl*>(fs->decl);
        if (!fd || !fd->type) return nullptr;
        auto* rt = ctx_.resolvedType(*fd->type);
        if (!rt || rt->kind != sema::TypeRef::Kind::Named ||
            rt->declaration != nullptr)
            return nullptr;
        if (!rt->astNode || !rt->astNode->name ||
            rt->astNode->name->parts.size() != 1)
            return nullptr;
        auto it = genericSubstSemaTy_.find(rt->astNode->name->parts[0]);
        return it != genericSubstSemaTy_.end() ? it->second : nullptr;
    }

    // per §14.1 (generics are monomorphized): install `genericSubst_` for a
    // member access / call / indexer get whose RECEIVER expression `target` has
    // a concrete instantiation of the generic class `parent` (e.g. the getter
    // body of `Box<Pair>[i]` must read its `T`-typed field as the substituted
    // `Pair`, not the i32 erasure). Builds the param→concrete-LLVM-type map from
    // the receiver's `genericArgs`, falling back to the local's stored
    // `localGenericSubst_` when a positional arg is still the i32 fallback (the
    // identical pattern fieldAddr / the property getter path use). Returns the
    // previous `genericSubst_` so the caller restores it after lowering the
    // member; a no-op (returns the current map unchanged) when the receiver is
    // not a concrete instantiation of `parent`.
    std::unordered_map<std::string_view, llvm::Type*>
    setupReceiverGenericSubst(const ast::Expr* target,
                              const sema::TypeSymbol* parent);

    // §28.5: is `fs` an `owned` field whose declared type is a generic type
    // PARAMETER of its enclosing class — regardless of any active substitution?
    // Used to decide, ONCE per generic class, whether its disposer must be
    // specialized per instantiation: the concrete T is unknown in the shared,
    // type-erased disposer, so only a per-instantiation disposer can correctly
    // classify and dispose the field (§28.5 instantiation-time classification).
    // The field's name must be one of the enclosing class's declared generic
    // parameters; a single-part name that is NOT a parameter (a concrete class)
    // takes the ordinary class-field disposal path, not specialization.
    static bool isOwnedTypeParamFieldShape(const sema::FieldSymbol* fs) {
        if (!hasOwnedModifier(fs)) return false;
        auto* fd = dynamic_cast<const ast::FieldDecl*>(fs->decl);
        if (!fd || !fd->type) return false;
        const ast::Type* ty = fd->type.get();
        if (!ty->name || ty->name->parts.size() != 1 ||
            !ty->genericArgs.empty())
            return false;
        if (!fs->parent) return false;
        std::string_view pname = ty->name->parts[0];
        for (const sema::Symbol* gp : fs->parent->genericParams)
            if (gp && gp->name == pname) return true;
        return false;
    }

    // CG18 / §28.5: an `owned` field that must be disposed dynamically through
    // the held object's vtable. Two cases:
    //   (a) the declared type is a class or interface type (the ordinary case);
    //   (b) the declared type is a generic type PARAMETER whose concrete
    //       instantiation (the active substitution) is a class or interface —
    //       §28.5's instantiation-time classification. Consulted only inside a
    //       per-instantiation specialized disposer, where the substitution is
    //       active; a value-type instantiation (where `owned` does not apply)
    //       yields no concrete class/interface and is NOT disposed as a
    //       reference (no spurious dispatch on a value slot).
    bool isOwnedReferenceField(const sema::FieldSymbol* fs) {
        if (!hasOwnedModifier(fs)) return false;
        auto* fd = dynamic_cast<const ast::FieldDecl*>(fs->decl);
        if (!fd || !fd->type) return false;
        auto* rt = ctx_.resolvedType(*fd->type);
        if (!rt || rt->kind != sema::TypeRef::Kind::Named) return false;
        // (b) §28.5: a generic type-parameter field — classify by the concrete
        // substituted type of the current instantiation.
        if (!rt->declaration) {
            const sema::TypeRef* concr = ownedTypeParamConcreteType(fs);
            if (!concr || concr->kind != sema::TypeRef::Kind::Named ||
                !concr->declaration)
                return false;
            auto ck = concr->declaration->kind;
            return ck == ast::TypeDeclKind::Class ||
                   ck == ast::TypeDeclKind::Interface;
        }
        // (a) the ordinary class- or interface-typed owned field.
        auto k = rt->declaration->kind;
        return k == ast::TypeDeclKind::Class || k == ast::TypeDeclKind::Interface;
    }

    // #362 / stdlib §4.5: if `fs` is an `owned` field whose type is a heap
    // collection (Array<T>/OwnedArray<T>/…, builtins with no TypeSymbol so
    // isOwnedReferenceField does not match them), return its resolved TypeRef;
    // otherwise null. The owner's Dispose frees such a field's backing and
    // cascades disposal into its class elements for an owning collection.
    const sema::TypeRef* ownedCollectionFieldType(const sema::FieldSymbol* fs) {
        if (!hasOwnedModifier(fs)) return nullptr;
        auto* fd = dynamic_cast<const ast::FieldDecl*>(fs->decl);
        if (!fd || !fd->type) return nullptr;
        auto* rt = ctx_.resolvedType(*fd->type);
        return isCollectionTypeName(rt) ? rt : nullptr;
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

    // §2.5 / §22.8 / §12 (issue #328 item 2+3): run §22.8 step-1+2 instance
    // initialization on `obj` (LLVM struct type `st`, sema type `t`): install
    // the most-derived vtable into slot 0 and execute the full-hierarchy field
    // initializers (base-first, own last), the collection-literal initializers,
    // and the event-source initializers. Factored out of `lowerNew` so the SAME
    // sequence runs at the in-module `new` site AND inside the cross-module
    // `bz$<Type>$init` function (so a non-foldable field initializer crosses the
    // module boundary). genericSubst_ must be set for a generic instantiation
    // when called from `lowerNew`; the standalone init function is emitted only
    // for non-generic public types (the importable construction surface).
    void emitInstanceInit(const sema::TypeSymbol* t, llvm::Value* obj,
                          llvm::StructType* st);

    // §2.5 / §2.6 / §22.8 (issue #328 item 2+3): emit the exported
    // `bz$<Type>$init(ptr this)` for a public non-generic class — it calls
    // emitInstanceInit so an importer's cross-module construction installs the
    // vtable and runs every field initializer (foldable or not) by linking to
    // this. One per public class; cached in initFns_.
    void emitInstanceInitFunction(const sema::TypeSymbol* ts);

    // §2.5 (issue #328 item 2+3): declare the external `bz$<Type>$init(ptr)` for
    // an IMPORTED type, so the importer's `new` calls the linked initializer
    // (vtable install + field initializers) before the imported ctor. Cached in
    // initFns_.
    llvm::Function* declareImportedInit(const sema::TypeSymbol* ts);

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

    // §28.3 (line 4910): register a plain `var` owned local for EXCEPTION-UNWIND
    // disposal only (an `unwindOnly` Disposal). Unlike `using var`, the plain
    // owned local is auto-disposed solely when an exception unwinds out of its
    // scope; on normal exit sema requires it to reach a discharge point. Only
    // owned, class/interface-typed, definitely-assigned locals that are never
    // moved / explicitly disposed / reassigned (unwindIneligible_) are
    // registered — that conservative subset guarantees the local is still live
    // (owned) at any throw in its scope, so the unwind disposal cannot double-
    // dispose a moved-out or already-disposed value. A movable-but-not-here
    // local is left unregistered (a bounded leak on its unwind path, never a
    // double-free).
    void registerUnwindOwnedLocal(const ast::LocalDecl* ld);

    // §28.3 (line 4910): register an `owned` parameter for exception-unwind
    // disposal (same unwindOnly mechanism / eligibility as plain owned locals).
    // An owned param is owned at method entry (§28.8) and must reach a discharge
    // point on normal exit; on an unwind out of the method it is auto-disposed.
    void registerUnwindOwnedParam(sema::Symbol* psym, llvm::AllocaInst* slot);

    // §28.3: is `ld`'s initializer an OWNED class/interface object that is
    // definitely assigned at the declaration (a `new T()`, or a call returning
    // `owned T`)? Only such plain owned locals are unwind-registered. A borrow
    // initializer, a value/runtime-managed type, or an uninitialized/null local
    // returns false.
    const sema::TypeSymbol* unwindOwnedLocalClass(const ast::LocalDecl* ld);

    // §28.3: the set of local-decl AST nodes and parameter symbols that are
    // INELIGIBLE for unwind disposal because they are moved, explicitly
    // disposed, reassigned, or otherwise consumed somewhere in the current
    // method body — computed once per body by collectUnwindIneligible.
    std::unordered_set<const void*> unwindIneligible_;
    // §28.3: the function for which unwindIneligible_ was computed. Unwind-
    // disposal registration only fires when currentFn_ matches — so a lambda /
    // thunk body (lowered mid-function, whose own body is NOT analyzed) does not
    // register its owned locals against the enclosing function's stale ineligible
    // set, which could otherwise miss a move/dispose inside the lambda and
    // double-dispose. Such a lambda local simply leaks on its unwind path
    // (a bounded leak, never a double-free).
    llvm::Function* unwindIneligibleFn_ = nullptr;

    // #422 / §4.6 / §28.9: the set of `var` string LocalDecl nodes PROVABLY
    // eligible for scope-end bz_string_release — initialized once by a
    // codegen-internal +1 producer (string `+` / interpolation) and never
    // escaping their scope (a conservative whole-body walk found every use is a
    // non-consuming read). Computed once per body by collectStringReleaseEligible
    // and consulted in lowerLocalDecl. Membership is the ONLY thing that arms a
    // release; absence ⇒ the string leaks (the pre-existing, safe behavior).
    std::unordered_set<const void*> stringReleaseEligible_;
    // The function for which stringReleaseEligible_ was computed (mirrors
    // unwindIneligibleFn_): a lambda/thunk body lowered mid-function inherits no
    // eligibility, so its string locals just leak (never double-freed).
    llvm::Function* stringReleaseEligibleFn_ = nullptr;
    // Compute stringReleaseEligible_ for a block body (see the .cpp).
    void collectStringReleaseEligible(
        const std::vector<std::unique_ptr<ast::Stmt>>& body);
    // True if `init` is a codegen-internal +1 string producer: a string `+`
    // BinaryExpr or an InterpolatedStringExpr. These are lowered to a fresh
    // malloc'd rc=1 header that codegen fully controls — so binding the result
    // to a local CONSUMES that single reference (no caller/callee handoff
    // ambiguity, unlike a string-returning call). Used both to gate release
    // eligibility and to release intermediate concat operands.
    bool isPlusOneStringProducer(const ast::Expr* init) const;

    // #422 / §27.5 / §28.9: the set of `var` closure (function-typed) LocalDecl
    // nodes PROVABLY eligible for scope-end bz_closure_release — initialized
    // once by a codegen-internal +1 producer (a lambda expression or a method
    // reference) and never escaping their scope (a conservative whole-body walk
    // found every use is a non-consuming read). Computed once per body by
    // collectClosureReleaseEligible and consulted in lowerLocalDecl. Membership
    // is the ONLY thing that arms a release; absence ⇒ the closure leaks (the
    // pre-existing, safe behavior).
    std::unordered_set<const void*> closureReleaseEligible_;
    // The function for which closureReleaseEligible_ was computed (mirrors
    // stringReleaseEligibleFn_): a lambda/thunk body lowered mid-function
    // inherits no eligibility, so its closure locals just leak (never
    // double-freed).
    llvm::Function* closureReleaseEligibleFn_ = nullptr;
    // Compute closureReleaseEligible_ for a block body (see the .cpp).
    void collectClosureReleaseEligible(
        const std::vector<std::unique_ptr<ast::Stmt>>& body);
    // True if `init` is a codegen-internal +1 closure producer: a LambdaExpr, or
    // a bare/qualified method reference (an IdentifierExpr / MemberAccessExpr
    // whose symbol is a MethodSymbol, used as a value rather than called). These
    // lower to a fresh calloc'd rc=1 closure cell that codegen fully controls —
    // so binding the result to a local CONSUMES that single reference (no
    // caller/callee handoff ambiguity, unlike a closure-returning call). Only
    // the static type being a function type qualifies (guards against a method
    // *call*, which is not a value-producing reference). Used to gate release
    // eligibility.
    bool isPlusOneClosureProducer(const ast::Expr* init) const;

    // #422 / §8.6 / §28.9: the set of `var` Object/interface LocalDecl nodes
    // PROVABLY eligible for scope-end bz_box_release — declared with an
    // Object/interface type, initialized once by a codegen-internal +1 BOX
    // producer (an initializer whose static type is a boxable VALUE type, so
    // boxValue itself allocated the wrapper), and never escaping their scope (a
    // conservative whole-body walk found every use is a non-consuming read —
    // `is`/`as`/`= null`). Computed once per body by collectBoxReleaseEligible
    // and consulted in lowerLocalDecl. Membership is the ONLY thing that arms a
    // release; absence ⇒ the box leaks (the pre-existing, safe behavior). A class
    // instance coerced to Object has a REFERENCE initializer (not a boxable
    // value), so it is never a producer here — it is never released by this path,
    // which is what keeps the class-instance model uncorrupted.
    std::unordered_set<const void*> boxReleaseEligible_;
    // The function for which boxReleaseEligible_ was computed (mirrors
    // stringReleaseEligibleFn_): a lambda/thunk body lowered mid-function
    // inherits no eligibility, so its boxed-value locals just leak (never
    // double-freed).
    llvm::Function* boxReleaseEligibleFn_ = nullptr;
    // Compute boxReleaseEligible_ for a block body (see the .cpp).
    void collectBoxReleaseEligible(
        const std::vector<std::unique_ptr<ast::Stmt>>& body);
    // True if local `ld` is a codegen-internal +1 box producer: its declared
    // type is Object or an interface AND its initializer's static type is a
    // boxable VALUE type (record/enum/primitive/nullable value). boxForTarget
    // then allocates a fresh rc=1 box wrapper that codegen fully controls — so
    // binding the result to the local CONSUMES that single reference. A reference
    // initializer (a class instance coerced to Object — the #399 path) is NOT a
    // box producer: boxForTarget is a no-op and no wrapper exists. Used to gate
    // release eligibility. (Not const: it consults the boxable-value predicates,
    // which are non-const Emitter members.)
    bool isPlusOneBoxProducer(const ast::LocalDecl* ld);

    // §28.3: populate unwindIneligible_ from a method/ctor body. A tracked owned
    // local/param becomes ineligible if it appears in a value-consuming position
    // (a `move`, an owned argument / any call argument conservatively, an
    // assignment target, a local-init RHS that moves it, a field store, or a
    // `.Dispose()` receiver). Borrow-position reads (method/property/indexer
    // receivers) and `return x` (a normal-exit discharge, skipped by the
    // unwindOnly disposal) keep the local eligible.
    void collectUnwindIneligible(
        const std::vector<std::unique_ptr<ast::Stmt>>& body);

    // CG13: the class type of a local — from its declared type, else the
    // resolved type of a `new T(...)` initializer.
    const sema::TypeSymbol* classOfLocal(const ast::LocalDecl* ld);

    // #383 / §28: a `using var` whose initializer yields an OWNED class/
    // interface object whose concrete class is not captured by classOfLocal
    // (which only matches a class-typed annotation or a `new`). Two cases:
    //   (a) the declared/resolved type is an interface (a reference type with
    //       a vtable, but no entry in disposerFns_); or
    //   (b) the initializer is a call whose method returns `owned T` (T a
    //       class or interface) — distinct from a borrow return (§28.10's
    //       "initialized from a borrow", which assumes NO disposal).
    // Returns true → register for dynamic disposal. The disposal dispatches
    // through the object's own vtable, so the concrete class is unneeded.
    bool usingVarOwnsClassObjectDynamically(const ast::LocalDecl* ld);

    // #405 / §28.10 / §31.1: a `using var` whose value is a BUILTIN exception
    // object (Exception or a §31.4 subtype). Built-in exceptions have no user
    // TypeSymbol, so classOfLocal / usingVarOwnsClassObjectDynamically miss them;
    // they are nevertheless owned heap class instances and must be disposed +
    // reclaimed at scope end (dynamic dispatch through the object's vtable).
    bool usingVarOwnsBuiltinException(const ast::LocalDecl* ld);

    // #383 / §28.8.1: a `using var u := move x.f;` takes the owned field's
    // value (a class object) and discharges it at scope end. Returns true if
    // the initializer is `move <expr>` whose moved value is a class/interface
    // object — register for dynamic disposal. Not registered while emitting a
    // destructor body (where the auto field-dispose pass would also touch the
    // moved-from field → a double-free; codegen does not track moved-from
    // state — see emittingDisposerBody_).
    bool usingVarIsMovedClassObject(const ast::LocalDecl* ld);

    // #362 / stdlib §4.5: the resolved collection TypeRef of a local (declared
    // type, then `new` initializer type, then inferred initializer type), or
    // null if the local is not a heap collection.
    const sema::TypeRef* collectionTypeOfLocal(const ast::LocalDecl* ld);

    // #394 / stdlib §4.9: the resolved Dictionary/OwnedDictionary TypeRef of a
    // local (declared type, then `new` initializer type, then inferred
    // initializer type), or null if the local is not a hash-backed dictionary.
    const sema::TypeRef* dictTypeOfLocal(const ast::LocalDecl* ld);

    // CG13: lower a block as a disposal scope. Pushes a fresh scope; on
    // normal fall-through, disposes its `using var`s in reverse and pops.
    // If the block terminated early (return/break/continue), that
    // terminator already emitted the needed disposals, so we only pop.
    bool lowerScopedBlock(const std::vector<std::unique_ptr<ast::Stmt>>& body,
                          std::size_t startIdx = 0);

    // CG13: emit Dispose calls for one scope's `using var`s, in reverse
    // declaration order (§28.4 ordering). #383 / §31.1: when `isUnwind` is true
    // (the exception-propagation path), a `caughtException` entry is SKIPPED —
    // a re-raise/throw leaving a catch arm keeps the exception in flight for
    // the enclosing handler to free.
    void emitDisposals(const std::vector<Disposal>& scope, bool isUnwind = false);

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

    // issue #370 / §4.10: `for var x in <set>` over a dict-backed Set<T>
    // (records, primitives/enums, strings — all generic-table-backed). Returns
    // false if the iterable is not such a set (so other for-handlers run).
    bool lowerDictBackedSetFor(const ast::ForStmt& fs);

    // issue #370 / §4.8: `for var k in <dict>.Keys` — iterate a Dictionary's
    // keys. The string-key dict uses the bz_dict_keys_str enumeration; a
    // non-string-key (generic-table) dict uses bz_dict_keys_gen. Returns false
    // if the iterable is not a `<dict>.Keys` access.
    bool lowerDictKeysFor(const ast::ForStmt& fs);

    // issue #370: emit a counted loop over a snapshot of generic-table keys
    // (from bz_dict_keys_gen). `tableObj` is the bz_dict_t*; `elemTyRef` is the
    // key/element TypeRef (its LLVM type is how each key is loaded and bound to
    // the loop variable). Used by both lowerDictBackedSetFor and the
    // non-string branch of lowerDictKeysFor.
    bool emitGenericKeyTableFor(const ast::ForStmt& fs, llvm::Value* tableObj,
                                const sema::TypeRef* elemTyRef);

    // If `e` names a local or parameter, return its storage slot; else null.
    // CG50: if `e` names a ref/out parameter, return its storage address
    // (the incoming pointer) and set `elemTy` to the referent type.
    llvm::Value* refParamAddr(const ast::Expr& e, llvm::Type*& elemTy);

    llvm::AllocaInst* localSlot(const ast::Expr& e);

    // §27.5: given a local/param slot, return the address at which the
    // variable's value actually lives and set `elemTy` to its type. For an
    // ordinary slot this is the alloca itself; for a captured ("boxed")
    // variable the alloca holds a pointer to a shared heap cell, so this
    // loads the cell pointer and returns it. Reads and writes both route
    // through this so the in-function side and the closure side share storage.
    llvm::Value* boxedStorage(llvm::AllocaInst* slot, llvm::Type*& elemTy);

    // §27.5: promote a local/param slot to a shared heap cell (if not
    // already), copying the current value into the cell. Because the value
    // slot may be too small to hold a pointer (e.g. an `alloca i32`), a fresh
    // `ptr`-typed alloca is created to hold the cell pointer; it is returned
    // through `newSlot` and recorded in `cellVars_` with the value type. The
    // caller rebinds its locals_/paramSlots_ entry to `newSlot`. Returns the
    // cell pointer (the value to store in a closure env).
    llvm::Value* boxLocalSlot(llvm::AllocaInst* slot, llvm::AllocaInst*& newSlot);

    // §27.5: when a `using var` local is boxed for capture its storage slot
    // changes; update any pending disposal entry so scope-end disposal loads
    // the object through the new (cell-holder) slot.
    void rebindDisposalSlot(llvm::AllocaInst* oldSlot, llvm::AllocaInst* newSlot);

    // §27.5: collect the local/parameter symbols a lambda body references
    // from the enclosing scope (its capture set). Nested lambdas contribute
    // their captures too. Identifier resolution uses sema's exprSymbol map.
    void collectCapturedSyms(const ast::LambdaExpr& lam,
                             std::unordered_set<const sema::Symbol*>& out);

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
    // §8.3 (equality table) / §15.1.2: element-by-element value equality of two
    // inline-array `[N]T` values. `at` is the LLVM `[N x elem]` type; `elemTy`
    // is the element's declared Bezel type (so string elements compare by
    // content, record elements recurse via emitStructEquality, nested inline
    // arrays recurse). Parallels emitStructEquality. `elemTy` may be null, in
    // which case comparison falls back to the LLVM element shape.
    llvm::Value* emitInlineArrayEquality(llvm::Value* l, llvm::Value* r,
                                         llvm::ArrayType* at,
                                         const sema::TypeRef* elemTy = nullptr);
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

    // #380: declare (once) `void @free(ptr)` from libc — the counterpart to
    // callocFn() that reclaims a disposed class instance's heap block.
    llvm::FunctionCallee freeFn();

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

    // §5.1: apply an implicit numeric widening to coerce a lowered value to
    // a target LLVM type when the pair is a §5.1-admitted widening — int →
    // float / double, float → double (FPExt), or integer widening. Returns
    // the value unchanged when no such widening applies (so pointers,
    // structs, narrowing, equal types, etc. are left untouched). Used at
    // store and argument-binding sites where sema admitted an implicit
    // conversion but the lowered value still carries its source LLVM type.
    //
    // `srcUnsigned` selects the source's signedness for the int→FP and
    // int→int widenings (§5.1 / issue #410): an unsigned source uses UIToFP
    // / ZExt, a signed source uses SIToFP / SExt. LLVM integer types carry
    // no signedness, so the caller supplies it (e.g. via isUnsignedTargetExpr
    // on the source expression). The two-argument overload assumes signed —
    // the common case — for callers that do not have the source expression.
    llvm::Value* coerceNumericToType(llvm::Value* v, llvm::Type* targetTy,
                                     bool srcUnsigned);
    llvm::Value* coerceNumericToType(llvm::Value* v, llvm::Type* targetTy) {
        return coerceNumericToType(v, targetTy, /*srcUnsigned=*/false);
    }

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
    // `out`; returns false if an argument can't be placed. When `outExprs` is
    // non-null, appends the source expression for each emitted argument in the
    // same order (a defaulted parameter contributes nullptr) — used to recover
    // each argument's signedness for numeric widening (issue #421 / #410).
    bool buildOrderedArgs(const std::vector<ast::Param>& params,
                          const std::vector<ast::CallArgument>& args,
                          std::vector<llvm::Value*>& out,
                          std::vector<const ast::Expr*>* outExprs = nullptr);

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

    // issue #401 / stdlib §4.1 / §4.2: is `rt` one of the int-indexed list
    // INTERFACES (IReadOnlyList<T>, IList<T>, IOwnedList<T>)? These are the only
    // collection interfaces that declare an indexer, and the only implementers
    // are the array-backed Array<T> (IList) / OwnedArray<T> (IOwnedList) — so an
    // interface-typed receiver holds the same bz_array_t* and lowers through the
    // same runtime as the concrete array path. `outOwning` (optional) is set to
    // true for IOwnedList<T> (the owning-set, disposing variant) and false for
    // the borrowing/read-only interfaces.
    static bool isListInterfaceName(const sema::TypeRef* rt,
                                    bool* outOwning = nullptr);

    // issue #343 / §4.8: if the type's key generic arg (genericArgs[idx]) is a
    // record (a value type with synthesized GetHashCode/Equals), return its
    // TypeSymbol; otherwise null (string/int keys use the existing fast paths).
    static const sema::TypeSymbol* recordKeyType(const sema::TypeRef* collTy,
                                                 unsigned idx);

    // issue #343 / §9.1 / §9.2: if `ref` itself resolves to an
    // equality-bearing record type (positional or auto-equality body record),
    // return its TypeSymbol; otherwise null. Used to route a record *element*
    // sequence (e.g. lazy LINQ Distinct) through structural equality.
    static const sema::TypeSymbol* recordTypeOfRef(const sema::TypeRef* ref);

    // issue #343 / §8.3: build C-ABI trampolines over a record key type's
    // synthesized hash / equality for the generic dict/set runtime variant.
    //   hash: uint64_t(const void* key)  — loads the record struct, hashes it.
    //   eq:   bool(const void* a, const void* b) — loads both, compares.
    // The runtime stores raw key bytes; the trampolines reinterpret the byte
    // pointer as a record-struct pointer.
    llvm::Function* buildRecordHashTrampoline(const sema::TypeSymbol* rec);
    llvm::Function* buildRecordEqTrampoline(const sema::TypeSymbol* rec);

    // issues D3 / D8 / §4.8 / §4.10: a Dictionary<K,V> key (idx=0) or Set<T>
    // element (idx=0) needs the generic-key runtime variant (bz_dict_*_gen)
    // when K/T is NOT a string-key-fast-path type. For dictionaries the
    // string key keeps the fast path; every other equality-bearing key
    // (int/long/bool/char/enum/double/float, or a record) goes generic.
    // For sets the array backing cannot dedup (§4.10), so ALL element types go
    // generic — including string elements. `isSet` selects the set rule.
    // Returns the resolved element/key TypeRef when generic backing applies
    // (and sets *byteSize to the stored key/element byte count); null when the
    // string-key fast path (dictionary) applies.
    const sema::TypeRef* genericBackingType(const sema::TypeRef* collTy,
                                            unsigned idx, bool isSet,
                                            uint64_t* byteSize);

    // issues D3 / D8 / §8.3: build C-ABI trampolines over an ARBITRARY
    // equality-bearing value type's hash / equality for the generic dict/set
    // runtime variant. Generalizes buildRecord*Trampoline from records to
    // primitives, enums, and strings. `ty` is the key/element TypeRef; the
    // runtime stores `byteSize` raw bytes per key (8 for a string pointer or a
    // scalar widened to a slot, sizeof(record) for a record). The hash reuses
    // emitValueHashCode (§8.3) and the equality reuses emitStringEq (strings,
    // §4.6), emitStructEquality (records, §9.1/§9.2), or an integer/float
    // compare (primitives/enums).
    llvm::Function* buildValueHashTrampoline(const sema::TypeRef* ty);
    llvm::Function* buildValueEqTrampoline(const sema::TypeRef* ty);

    // §3: instance method calls on Array<T>, Dictionary<K,V>, etc.
    // Called from lowerCall before the normal method-symbol dispatch.
    llvm::Value* tryLowerCollectionInstanceCall(const ast::CallExpr& call);

    // stdlib §4.10 (#351): lower a dict-backed Set's set-algebra op
    // (UnionWith/ExceptWith/IsSubsetOf/IsSupersetOf) when `otherExpr` is a
    // same-element dict-backed Set, by enumerating the other table's elements
    // (bz_dict_keys_gen) and folding them into/against `selfTbl`. Returns the
    // lowered value, or nullptr when `otherExpr` is not a same-typed dict-backed
    // Set (the caller then defers the op).
    llvm::Value* tryLowerSetAlgebra(std::string_view method,
                                    llvm::Value* selfTbl,
                                    const ast::Expr& otherExpr,
                                    const sema::TypeRef* selfElemRef,
                                    bool elemIsRecord);

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
    //   3 = equality   i32(a_ptr, b_ptr)              (Distinct element compare)
    // `elemTy` is the input element LLVM type; `outTy` the selector output type.
    // `elemRec` is non-null only for kind 3 over a record element type (D6):
    // the comparison routes through synthesized structural equality
    // (emitStructEquality, §9.1/§9.2) instead of an ICmp on the aggregate.
    llvm::Function* buildLinqTrampoline(int kind, llvm::Type* elemTy,
                                        llvm::Type* outTy,
                                        const sema::TypeSymbol* elemRec = nullptr);

    // stdlib §5 SelectMany: a flatten-selector trampoline `void*(closure, elem)`.
    // Calls the closure `(env, elem) -> ptr` (the inner collection) and wraps the
    // returned collection as a lazy enumerator (bz_lazy_from_array) the runtime
    // drains. `elemTy` is the upstream element type; `innerEsz` the inner U size.
    llvm::Function* buildSelectManyTrampoline(llvm::Type* elemTy,
                                              int64_t innerEsz);

    // stdlib §5 Zip: a combine trampoline `void(closure, a, b, out)`. Loads the
    // left element (`aTy`) and right element (`bTy`), calls the closure
    // `(env, a, b) -> outTy`, and stores the result through `out`.
    llvm::Function* buildZipTrampoline(llvm::Type* aTy, llvm::Type* bTy,
                                       llvm::Type* outTy);

    // Detect Convert.ToInt32(s) / Convert.ToInt64(s) / Math.Abs(n) etc.
    // as intrinsic static class method calls. Returns a lowered value or null.
    llvm::Value* tryLowerBuiltinClassCall(const ast::CallExpr& call);

    // stdlib §3.2: Encoding.Utf8.GetBytes(s) / Encoding.Utf8.GetString(bytes).
    // The callee is a member access whose target is itself `Encoding.<singleton>`.
    // Only the Utf8 singleton is implemented; the others are rejected. Returns a
    // lowered value (owned Array<byte> or string), or null if not this shape.
    llvm::Value* tryLowerEncodingCall(const ast::CallExpr& call);

    // §3.4: static methods on primitive type names — `int.Parse(s)`,
    // `double.Parse(s)`, etc. The target is a primitive keyword identifier.
    llvm::Value* tryLowerPrimitiveStaticCall(const ast::CallExpr& call);

    // §22.4: Pre-create alloca slots for any `out var x` arguments before the
    // call is dispatched (primitive-static, builtin, or regular method). This
    // ensures lvalueAddr() finds the slot regardless of which code path handles
    // the call. Must be called at the start of lowerCall.
    void ensureOutVarSlots(const ast::CallExpr& call);

    llvm::Value* lowerCall(const ast::CallExpr& call);

    // per §14.1/§14.2 + §26.5 (issue #391): bind a generic method's
    // return-only type parameter at the call site. `declRet` is the method's
    // *declared* return TypeRef (e.g. the bare `T`, or a constructed `Box<T>`);
    // `resultTy` is the concrete return type sema solved from the call's target
    // and recorded on the call's exprType (e.g. `Widget`, or `Box<Widget>`).
    // Unifies the two structurally, adding each still-unbound parameter in
    // `genericParams` to `subst`/`semaTy` and setting `added` when it binds
    // anything. Used by lowerCall so `new T()` in the monomorphized body
    // constructs the real type instead of falling to the i32 placeholder.
    void unifyMethodReturnTypeArg(
        const sema::TypeRef* declRet, const sema::TypeRef* resultTy,
        const std::vector<ast::GenericParam>& genericParams,
        std::unordered_map<std::string_view, llvm::Type*>& subst,
        std::unordered_map<std::string_view, const sema::TypeRef*>& semaTy,
        bool& added);

    // per §24: events. `eventSourcePtr` lowers an event-access expression
    // (a bare `Clicked` implicit-`this` identifier, or `obj.Clicked` /
    // `this.Clicked` member access whose symbol is an EventSymbol) to the
    // loaded backing EventSource<T> pointer; null if `e` is not an event
    // access. `payloadType` returns the resolved payload TypeRef of an event
    // member (the `: T` annotation, or the unit type when omitted). These
    // support Subscribe/Add/Remove/Raise lowering.
    const sema::EventSymbol* eventSymbolOf(const ast::Expr& e);
    llvm::Value* eventSourcePtr(const ast::Expr& e);
    const sema::TypeRef* eventPayloadType(const sema::EventSymbol* es);

    // per §24.3 / §24.5: recognize and lower an event method call —
    // `<event>.Subscribe(h)` (returns owned Subscription), `<event>.Add(h)`,
    // `<event>.Remove(h)`, `<event>.Raise(payload)` / `<event>.Raise()`.
    // Returns the produced value (Subscription ptr for Subscribe; null/unit for
    // the others), or std::nullopt when `call` is not an event method call so
    // the caller continues normal dispatch.
    std::optional<llvm::Value*> tryLowerEventCall(const ast::CallExpr& call);

    // per §24.4: `<event> += h` / `<event> -= h` inside the declaring class —
    // a fixed desugaring to Add/Remove. Returns true when handled.
    bool tryLowerEventCompound(const ast::AssignmentStmt& as);

    // per §2.11 / §24.4: true when `e`'s static type is the built-in
    // `Subscription` (stdlib §2.11), or — for an inferred local bound to a
    // literal `.Subscribe(...)` whose result sema does not yet type — when it is
    // structurally that call. Routes `sub.Dispose()` to bz_subscription_dispose
    // (a Subscription is a runtime-managed opaque handle with no vtable).
    bool isSubscriptionExpr(const ast::Expr& e);

    // per §2.11: true when the resolved type `rt` is the built-in
    // `Subscription` — a Named TypeRef with no user declaration whose name is
    // "Subscription" (resolved per TypeResolution_Names §24.4 / stdlib §2.11).
    static bool isSubscriptionType(const sema::TypeRef* rt);

    // per §24.5: emit the handler-invocation loop for `Raise(payload)` — call
    // every active handler closure with `payload` (already lowered, or null for
    // the no-payload unit form). `payloadLL`/`payloadTy` describe the argument.
    void emitEventRaise(llvm::Value* srcPtr, llvm::Value* payloadLL,
                        llvm::Type* payloadTy);

    // CG17/CG18/CG35: emit a call to `ms` with fully-built `args` (receiver
    // first for instance methods). Dynamic dispatch loads the receiver's
    // vtable pointer (instance slot 0) and calls through the method's slot
    // — the interface-method slot when `ms` is an interface method (CG35),
    // else the virtual slot (CG17); works for abstract/interface methods
    // (no concrete function) since the slot is filled by the runtime class.
    // A `base.M()` call (baseQualified) bypasses dispatch (§12.3), as do
    // non-virtual non-interface methods.
    // `argExprs`, when non-null, holds the source expression for each entry of
    // `args` (index-aligned, including a nullptr receiver slot for instance
    // methods) — used to recover each argument's signedness so an unsigned
    // source widens via UIToFP/ZExt at the parameter boundary (issue #421/#410).
    llvm::Value* emitMethodCall(sema::MethodSymbol* ms,
                                std::vector<llvm::Value*>& args,
                                bool baseQualified,
                                const std::vector<const ast::Expr*>* argExprs
                                    = nullptr);

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
