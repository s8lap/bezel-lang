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
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <functional>

#include "ast/Node.hpp"
#include "codegen/LlvmBackend.hpp"
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
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Local.h"

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

// CG32 / #527: decode a char literal lexeme (`'A'`, `'\n'`, `'A'`, …)
// to its Unicode code point value (i32).
//
// The lexer already validated the form and consumed the correct number of
// source bytes; we re-parse the raw text here.  Three distinct input shapes:
//
//  1. Escape sequence (\n \t \r \0 \\ \' \" \a \b \f \v)  — simple table.
//  2. Unicode/hex escape (\uXXXX, \UXXXXXXXX, \xH..H) — accumulate hex digits.
//  3. Raw UTF-8 scalar (one or more bytes, non-backslash) — decode the full
//     code point from the UTF-8 byte sequence (per §18.3).
//
// The pre-fix code returned only the first byte for cases 2 and 3, which
// produced wrong values for every non-ASCII or hex-escape char literal (#527).
inline std::int64_t parseCharLiteral(std::string_view text) {
    // Strip the surrounding single quotes.
    if (text.size() >= 2 && text.front() == '\'') text.remove_prefix(1);
    if (!text.empty() && text.back() == '\'') text.remove_suffix(1);
    if (text.empty()) return 0;

    if (text[0] != '\\') {
        // §18.3: raw Unicode scalar — may be a multi-byte UTF-8 sequence.
        unsigned char b0 = static_cast<unsigned char>(text[0]);
        if (b0 < 0x80) {
            // Plain ASCII.
            return static_cast<std::int64_t>(b0);
        }
        // Multi-byte UTF-8 — decode the full code point.
        // (The lexer validated the sequence; we just extract the value.)
        std::uint32_t cp = 0;
        std::size_t extra = 0;
        if      ((b0 & 0xE0) == 0xC0) { cp = b0 & 0x1F; extra = 1; }
        else if ((b0 & 0xF0) == 0xE0) { cp = b0 & 0x0F; extra = 2; }
        else if ((b0 & 0xF8) == 0xF0) { cp = b0 & 0x07; extra = 3; }
        else { return static_cast<std::int64_t>(b0); } // malformed — return lead byte
        for (std::size_t i = 1; i <= extra && i < text.size(); ++i) {
            cp = (cp << 6) | (static_cast<unsigned char>(text[i]) & 0x3F);
        }
        return static_cast<std::int64_t>(cp);
    }

    // Escape sequence.
    if (text.size() < 2) return 0;
    switch (text[1]) {
        // §18.8 named single-character escapes.
        case 'n':  return '\n';
        case 't':  return '\t';
        case 'r':  return '\r';
        case '0':  return '\0';
        case '\\': return '\\';
        case '\'': return '\'';
        case '"':  return '"';
        case 'a':  return '\a';  // per §18.8 — alert/bell = U+0007
        case 'b':  return '\b';
        case 'f':  return '\f';
        case 'v':  return '\v';

        // §18.8 \uXXXX — exactly 4 hex digits, BMP.
        // §18.8 \UXXXXXXXX — exactly 8 hex digits, full range.
        case 'u':
        case 'U': {
            const int required = (text[1] == 'u') ? 4 : 8;
            std::uint32_t value = 0;
            for (int i = 0; i < required && (2 + i) < static_cast<int>(text.size()); ++i) {
                char c = text[static_cast<std::size_t>(2 + i)];
                std::uint32_t nibble = 0;
                if (c >= '0' && c <= '9') nibble = static_cast<std::uint32_t>(c - '0');
                else if (c >= 'a' && c <= 'f') nibble = static_cast<std::uint32_t>(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') nibble = static_cast<std::uint32_t>(c - 'A' + 10);
                else break;
                value = (value << 4) | nibble;
            }
            return static_cast<std::int64_t>(value);
        }

        // §18.8 \xH..H — 1–4 hex digits, greedy, BMP.
        case 'x': {
            std::uint32_t value = 0;
            for (std::size_t i = 2; i < text.size() && i < 6; ++i) {
                char c = text[i];
                std::uint32_t nibble = 0;
                if (c >= '0' && c <= '9') nibble = static_cast<std::uint32_t>(c - '0');
                else if (c >= 'a' && c <= 'f') nibble = static_cast<std::uint32_t>(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') nibble = static_cast<std::uint32_t>(c - 'A' + 10);
                else break;
                value = (value << 4) | nibble;
            }
            return static_cast<std::int64_t>(value);
        }

        default: return static_cast<unsigned char>(text[1]);
    }
}

// Per-module emitter. Holds the LLVM context/module/builder and lowers the
// checked AST. Grows one expression/statement kind per codegen slice.
class Emitter {
public:
    Emitter(sema::SemaContext& ctx, llvm::LLVMContext& llvmCtx,
            const CodegenOptions& opts = {})
        : ctx_(ctx), llvm_(llvmCtx),
          module_(std::make_unique<llvm::Module>("bezel", llvmCtx)),
          builder_(llvmCtx),
          checkedDisposal_(opts.checkedDisposal) {}

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
        // #510 / §10 / CG30: computeEnumValues must run BEFORE declareVtables
        // so that enumValue_ is populated when declareVtables calls
        // ensurePropertyAccessorFn to emit virtual/interface property accessor
        // bodies. An arrow getter `=> Enum.Member` calls lowerExpr →
        // enumConstant() which consults enumValue_; if enumValue_ is empty
        // (because computeEnumValues had not run yet) enumConstant() returns
        // nullptr and the body emits `ret i32 0` regardless of the accessor's
        // declared value — Bug2 of #510. computeEnumValues depends only on
        // ctx_.symbols() (populated by sema before codegen), so moving it here
        // is safe; it has no dependency on methodFns_, vtables_, or any other
        // codegen state that declareVtables or declareFunctions populates.
        computeEnumValues();  // CG30 — moved before declareVtables (fix #510)
        declareVtables();  // CG17: after functions exist (needs fn ptrs)
        // §2.5 / §22.9 (issue #503): emit ExternalLinkage getter stubs for
        // non-virtual public properties (exporter side). After declareVtables so
        // propGetSlot_ is populated; virtual properties are skipped.
        emitExportedPropertyGetters();
        // §2.5 / §22.9 (issue #503): declare external getter stubs for imported
        // non-virtual property getters (importer side). After declareVtables
        // so propGetSlot_ correctly reflects which imported properties are virtual.
        if (ctx_.hasImports()) declareImportedPropertyGetters();
        setupBoxDescriptors();  // §8.6: after descArray_/vtable globals exist
        declareExterns();  // CG26
        // #429 (box RETURN + FIELD-STORE category) / §8.6 / §28.9: before any
        // method bodies are lowered, determine:
        //   (a) which locally-compiled methods always return boxable value
        //       sources (boxReturnMethods_) — callers of these treat the result
        //       as a +1 box producer and may release it at scope end;
        //   (b) which Object/interface-typed CLASS fields ONLY receive boxable
        //       value stores across the entire class (boxStorageFields_) —
        //       emitDisposerCore releases these fields via bz_box_release.
        // Both analyses must run BEFORE the method body loop so that per-body
        // collectBoxReleaseEligible (which calls isPlusOneBoxProducer, which
        // calls isUserMethodBoxReturnCall) sees a populated boxReturnMethods_,
        // and the disposer (emitted after all method bodies) sees a fully-
        // populated boxStorageFields_.
        collectBoxReturnMethods();
        collectBoxStorageFields();
        for (auto& [sym, fn] : methodFns_) {
            emitFunctionBody(sym, fn);
        }
        for (auto& [sym, fn] : ctorFns_) {
            emitConstructorBody(sym, fn);
        }
        for (auto& e : staticCtors_) {
            emitStaticConstructorBody(e.cs, e.type, e.fn);
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
        // It stays NON-FATAL here (report-and-continue): the JIT test path uses
        // emitModule directly and is more lenient than the AOT file-emission
        // path. The AOT boundary (emitObjectFile) runs the verifier independently
        // and returns false on failure, so the driver exits non-zero (#506).
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
        // #628 (part 1): any Error-severity diagnostic emitted during codegen
        // (e.g. "not yet implemented: codegen for this for statement", `typeof`,
        // `GetType`, `await` in a catch guard) must abort emission — return
        // nullptr so the caller sees a failed module and produces no binary.
        // The diagnostic was already printed; the module may be partially built
        // and must not be handed to the AOT/JIT paths.
        if (ctx_.diagnostics().hasErrors()) return nullptr;
        return std::move(module_);
    }

private:
    sema::SemaContext& ctx_;
    llvm::LLVMContext& llvm_;
    std::unique_ptr<llvm::Module> module_;
    llvm::IRBuilder<> builder_;
    // §28.2 v0.134 / issue #601: true when building in checked disposal mode.
    // When set, emitFunctionBody and ensurePropertyAccessorFn prepend a
    // disposed-flag test on every instance-member entry; emitDisposeAndReclaim
    // suppresses the heap-free (quarantine) so the flag remains readable.
    bool checkedDisposal_ = false;
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
    // CG47 / fix #582 / §22.13: one entry per static-initialisation unit that
    // must run before `main`.  Kept ordered (declaration order) for a stable
    // run order.
    //
    // `cs` is non-null for a user-written static constructor; null for a
    // synthesized field-init-only function (see fix #582 / §22.13 — a type
    // that has non-constant static field initializers but no explicit static
    // constructor gets a synthesized `bz$<T>$sfinit` here).  `type` is always
    // set and drives field-initialiser emission in emitStaticConstructorBody.
    //
    // Within a single type, the synthesized sfinit entry (if any) is inserted
    // BEFORE the user cctor entry (if any), so field initializers run first
    // per §22.13 ("static fields are initialized in textual declaration order
    // … the static constructor, if any, runs after all static field
    // initializers complete").
    struct StaticCtorEntry {
        sema::ConstructorSymbol*    cs;    // null = synthesized field-init-only
        const sema::TypeSymbol*     type;  // always set
        llvm::Function*             fn;
    };
    std::vector<StaticCtorEntry> staticCtors_;

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
        //   EXCEPTION — #429 / §28.9 / §4.10: a borrowing Dictionary<K, string>
        //     has a runtime-managed string VALUE; its +1 per stored value must be
        //     released at free/clear/remove. Wire dictValueDisposer to the
        //     stringElementReleaseThunk so bz_dict_free calls bz_string_release
        //     on each surviving value. Mutually exclusive with the record-box-free
        //     and the owning-class disposer.
        //   EXCEPTION — Set<string>: elements are KEYS (not values) in the
        //     generic-key table. dictValueDisposer does not apply; instead
        //     dictStringKeys = true signals the scope-end disposal to call
        //     bz_set_string_keys_release(tbl) before bz_dict_free(tbl, null).
        bool dict = false;
        llvm::Value* dictValueDisposer = nullptr;
        // #429 / §28.9 / §4.10: true when the dict is a Set<string> and its
        // key slots carry a co-owned +1 per string element. At scope end the
        // disposal emits bz_set_string_keys_release(tbl) before bz_dict_free.
        bool dictStringKeys = false;
        // per §4.6/§4.7: a `using var` bound to a LinkedList<T> or
        // OwnedLinkedList<T>. At scope end it is freed via bz_llist_free.
        // `collectionElemDisposer` (reused) is the per-element dispose callback
        // for an OwnedLinkedList<T> of class elements (null for LinkedList<T>
        // or value elements — only the node chain is freed, not the values).
        bool linkedList = false;
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
        // §2.15 / issue #550: a `using var` bound to a BinarySerializer or
        // BinaryDeserializer.  These are opaque heap handles with no user
        // TypeSymbol and no vtable.  At scope end they are freed via
        // bz_binser_free / bz_binder_free respectively.  Recognised by the
        // local's declared/resolved type name or the NewExpr type name.
        bool binarySerializer = false;
        bool binaryDeserializer = false;
        // §6.1 / §30.8: a `using var` bound to a CancellationTokenSource.
        // The source has no user TypeSymbol, so classOfLocal returns null and the
        // class-disposal path misses it. At scope end it is freed via
        // bz_cts_dispose (idempotent — signals surviving tokens and frees the
        // source). Recognised by the local's declared/resolved type name.
        bool cancellationTokenSource = false;
        // §8.1 / issue #646: a `using var` bound to a Stopwatch. Stopwatch is
        // a heap-allocated timing object (bz_stopwatch_t*) with no user
        // TypeSymbol, so classOfLocal returns null and the class-disposal path
        // misses it. At scope end it is freed via bz_stopwatch_free.
        bool stopwatch = false;
        // §28.13 / stdlib §2.16 (#603): a `using var` bound to a Shared<T>
        // handle. At scope end it is released via bz_shared_release (decrements
        // strong count; at 0 disposes the value then frees the control block if
        // the weak count is also 0). No user TypeSymbol; recognised by builtin
        // type name "Shared".
        bool sharedHandle = false;
        // §28.13 / stdlib §2.16 (#603): a `using var` bound to a Weak<T>
        // handle. At scope end it is released via bz_weak_release (decrements
        // weak count; frees the control block if both strong and weak counts are
        // now 0). No user TypeSymbol; recognised by builtin type name "Weak".
        bool weakHandle = false;
        // stdlib §6.7 / §30.5: a `using var` bound to a Task / Task<T>.
        // Tasks have no user TypeSymbol; at scope end the task is disposed via
        // bz_task_fire_and_forget (non-blocking per §30.5: "does not block waiting
        // for completion"). FireAndForget registers a continuation that calls
        // bz_task_free when the task eventually completes — safe for both already-
        // done and still-running tasks (no UAF). Double-free safety: `await t`
        // where `t` is a using-var does NOT call bz_task_free (lowerAwait only
        // frees anonymous CallExpr operands), so the sole free is via the faf
        // continuation — exactly once.
        bool task = false;
        // §28.12 / §30 (`await using var`): the resource implements
        // IAsyncDisposable. On the NORMAL exit path (not unwind) we call
        // DisposeAsync() and await the returned Task (a proper suspend point),
        // then free the block (physical reclamation — §28.12 step 2). On the
        // UNWIND (exception) path we fall back to the synchronous Dispose() +
        // free (the class should have a destructor for the synchronous fallback
        // per §28.12; awaiting across an already-active unwind is a deferred
        // increment). `disposeAsyncFn` is the directly-resolved DisposeAsync
        // llvm::Function* for the concrete class (found via findMethodByName +
        // methodFns_ at registration time); null means "use synchronous Dispose
        // fallback" (class not statically known, or DisposeAsync not found).
        bool asyncDispose = false;
        llvm::Function* disposeAsyncFn = nullptr;
        // §30.15 (#600 phase c): a `using var` bound to a Channel<T>. At scope
        // end it is freed via bz_channel_dispose (closes the channel, faults any
        // pending receivers with ChannelClosedException, disposes undelivered
        // items in reverse insertion order per §28.11). No user TypeSymbol;
        // recognised by the builtin type name "Channel". The element disposer
        // (for class-typed T) is embedded in the channel struct itself (set at
        // construction time), so the Disposal struct needs no extra field.
        bool channel = false;
        // §30.15 (#600 phase c): a `using var` bound to a ChannelSender<T>. At
        // scope end it is released via bz_channel_sender_release (decrements
        // sender count; if last sender, closes the channel and faults pending
        // receivers with ChannelClosedException). No user TypeSymbol; recognised
        // by the builtin type name "ChannelSender".
        bool channelSender = false;
        // #628 / stdlib §4.9 v0.135: a `using var` bound to a DrainedEntry<K,V>
        // obtained from `for var entry in d.Drain() do … end`. The loop variable
        // `entry` is a struct {K key; V* value}; `using var e := entry` transfers
        // ownership of the entry to `e`. At scope end we must:
        //   1. Dispose + reclaim the owned V class value (field 1).
        //   2. Release the key if K is string (field 0 holds a bz_string_t*).
        // `drainedEntryKeyIsString` flags whether field[0] is a string pointer.
        // `drainedEntryValDisposer` is the owning-class disposer thunk for V
        // (null when V has no user TypeSymbol — then we use dynamic dispatch via
        // emitDisposeAndReclaim directly on the loaded pointer).
        // `drainedEntryKeyType` / `drainedEntryValType` are the LLVM types for
        // the GEP so we can load the fields correctly.
        bool drainedEntry = false;
        bool drainedEntryKeyIsString = false;
        llvm::Value* drainedEntryValDisposer = nullptr;
        llvm::Type* drainedEntryKeyType  = nullptr;
        llvm::Type* drainedEntryValType  = nullptr;
        // §29.7 (#623): an iterator enumerator (IEnumerable<T> state machine)
        // whose Dispose is at vtable slot 1 (iterator vtable format:
        // [0]=td, [1]=Dispose, [2]=MoveNext, [3]=Current) — NOT slot 2 (class
        // vtable format).  At scope end (break / return / exception) we call
        // vtable[1](self) then free the frame.  Distinct from `dynamicDispose`
        // which dispatches through slot 2 of the class vtable.
        bool iterEnumDispose = false;
    };
    std::vector<std::vector<Disposal>> disposeStack_;
    // §28.3 (line 4910): owned parameters of the current function awaiting
    // unwind-disposal registration. bindParameters collects them (no dispose
    // scope is open yet); lowerScopedBlock registers them into the function-body
    // ROOT scope (the first scope it pushes) so they are auto-disposed on an
    // exception unwind out of the whole method. Cleared once registered.
    std::vector<std::pair<sema::Symbol*, llvm::AllocaInst*>> pendingOwnedParams_;
    // fix #633 / §33.4: when Main(args: Array<string>) is emitted, the args
    // array is built in the function prolog (before any scope opens) and must be
    // disposed at the function-body root scope exit.  This optional Disposal is
    // set by emitMethodBody and consumed by the first lowerScopedBlock call
    // (which opens the root scope), mirroring the pendingOwnedParams_ pattern.
    std::optional<Disposal> pendingArgsDisposal_;
    // CG13: the compiler-generated Dispose function for each class.
    std::unordered_map<const sema::TypeSymbol*, llvm::Function*> disposerFns_;
    // §2.5 / §22.8 (issue #328 item 2+3): the `bz$<Type>$init(ptr)` function for
    // each public non-generic class (exporter: a definition; importer: an
    // external declaration). Installs the vtable + runs field initializers, so a
    // cross-module `new` runs §22.8 step-1+2 by linking to the exporter's code.
    std::unordered_map<const sema::TypeSymbol*, llvm::Function*> initFns_;
    // #362 / stdlib §4.5: the cached per-element dispose thunk (built lazily).
    llvm::Function* elementDisposerThunk_ = nullptr;
    // §28.13 / stdlib §2.16 (#603): void(void*) thunks that call
    // bz_shared_release / bz_weak_release, used as per-element disposal
    // callbacks for OwnedArray<Shared<T>> / OwnedArray<Weak<T>>.
    llvm::Function* sharedReleaseThunk_ = nullptr;
    llvm::Function* weakReleaseThunk_ = nullptr;
    // stdlib §5 (#651): bz_grouping_free thunk for Array<IGrouping> disposal.
    llvm::Function* groupingFreeThunk_ = nullptr;
    // #429 / §4.6 / §28.9: the cached per-element string-release thunk (built
    // lazily) for a borrowing Array<string>'s co-owned string elements.
    llvm::Function* stringElementReleaseThunk_ = nullptr;
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
    // §29.7 (#623): parallel to finallyStack_ — holds the finallyBody AST
    // pointer for each active try/finally, so yield points can snapshot the
    // chain for the Dispose function.  Always the same depth as finallyStack_;
    // pushed/popped in lowerTry alongside finallyStack_.
    std::vector<const std::vector<std::unique_ptr<ast::Stmt>>*>
        finallyBodyStack_;

    // §29.7 (#623): per-yield-state → snapshot of the finallyBodyStack_ at
    // the moment of that yield (outermost first).  Populated at each
    // `yield return` inside an iterator body; consumed by emitIteratorMethod
    // to build the real Dispose function.
    std::unordered_map<unsigned,
        std::vector<const std::vector<std::unique_ptr<ast::Stmt>>*>>
        smIterFinallyAtState_;

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
    // #397 / §31.1: the name the enclosing catch arm binds the caught
    // exception to ("" for a bare-type arm with no binding). Used by
    // emitThrowValue to detect a direct re-throw `throw ex;` (the one case
    // that must not free the caught exception when throwing from the arm).
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
    // specializedMethods_[(ms, llvm_subst_key, sema_id_key)] = specialized
    // LLVM function. per §14.2 (generics are monomorphized): the cache key must
    // distinguish instantiations that produce DIFFERENT specialized bodies. The
    // LLVM type vector alone is insufficient because every class/interface type
    // erases to `ptr` in IR, so `Maker<A>.Run` and `Maker<B>.Run` would share a
    // key `[ptr]` and the second would reuse the first's body — wrong when the
    // body contains a concrete-class-specific disposer (`using var a := new
    // T()`). The third element carries a stable per-type identity token (the
    // sema TypeSymbol* for a Named type, a tagged primitive id otherwise) for
    // each substituted type parameter, in the SAME deterministic order as the
    // LLVM type vector, so distinct concrete type args get distinct entries and
    // genuinely-identical instantiations still share one.
    std::map<std::tuple<const sema::MethodSymbol*,
                        std::vector<llvm::Type*>,
                        std::vector<const void*>>,
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
    // fix #628 / §9.5 / §14.2: when emitting a SPECIALIZED method body via
    // buildSpecializedMethodFn, currentFn_ is set to the freshly-created specFn
    // which is NOT registered in methodFns_.  lowerWith's §575 reverse-lookup
    // (fn == currentFn_ in methodFns_) therefore fails — t stays null and
    // `this with … end` returns the raw `this` pointer instead of a struct copy
    // → ret-type mismatch (IR verification failure).  currentSpecMethodSymbol_
    // carries the ORIGINAL MethodSymbol* being specialized so lowerWith can find
    // the parent TypeSymbol without requiring specFn to be in methodFns_.
    sema::MethodSymbol* currentSpecMethodSymbol_ = nullptr;

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

    // §29.2 (#552): expression-form property getter thunks generated by
    // ensureExprGetterThunk. Keyed by PropertySymbol so the thunk is shared
    // across all call sites that call the same getter.
    std::unordered_map<const sema::PropertySymbol*, llvm::Function*>
        exprGetterThunks_;

    // CG26: external (FFI) function declared by an extern block (§32).
    std::unordered_map<const sema::ExternDeclSymbol*, llvm::Function*>
        externFns_;

    // CG27a: string runtime — %bz_string = { ptr, i64 } (data, length).
    // Literals are materialized as private global constants; concatenation
    // allocates via malloc/memcpy. All string variables are ptr-typed (§4.6).
    llvm::StructType* stringStructTy_ = nullptr;
    // §4.4 (fix #521): decimal value struct { i32 lo32, i32 mid32, i32 hi32, i32 flags }.
    llvm::StructType* decimalStructTy_ = nullptr;
    // per stdlib §2.6: Guid value-type struct { i64 lo, i64 hi }.
    llvm::StructType* guidStructTy_ = nullptr;
    // fix #499 / §9.8: Range<T> value struct { i64 start, i64 end, i64 step, i8 flags }.
    // flags bit 0 = IsEndInclusive (1 = inclusive `..`, 0 = half-open `..<`).
    llvm::StructType* rangeStructTy_ = nullptr;
    // §4.15: ArraySpan<T>/ReadOnlyArraySpan<T> value struct { ptr source, i32 start, i32 length }.
    // Both writable and read-only spans share the same LLVM layout (per §4.15).
    llvm::StructType* arraySpanStructTy_ = nullptr;
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

    // §22.12 / #536: per-instantiation static-field globals for generic classes.
    // When a static field's parent class has generic params and genericSubst_ is
    // active (during monomorphization of a static method body), the field must
    // resolve to an instantiation-specific global rather than the shared
    // template-level one.  Keyed by (FieldSymbol*, (llvm_types, identity_tokens))
    // — same disambiguation as specializedMethods_ (llvm types alone collapse
    // class/interface to `ptr`; the identity token breaks that collision).
    std::map<std::pair<const sema::Symbol*,
                       std::pair<std::vector<llvm::Type*>,
                                 std::vector<const void*>>>,
             llvm::GlobalVariable*>
        genericInstStaticGlobals_;

    // §24.1 (#477 B6): each STATIC event's compiler-generated backing
    // EventSource<T> field — a static owned field, modeled as a `ptr` global
    // initialized to null at module load and allocated (bz_event_source_new)
    // at program start, alongside the static constructors run in Main's
    // prologue (§22.7 / §22.13). Keyed by the event's EventSymbol. An instance
    // event's backing source lives in the class struct (fieldIndex_); a static
    // event's lives here instead.
    std::unordered_map<const sema::Symbol*, llvm::GlobalVariable*>
        staticEventGlobals_;

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
    // vtable layout matches the class vtable prefix (CG35-itable + §8.6/§8.5
    // #417):
    //   [0]=td, [1]=itmap, [2]=Dispose, [3]=ToString
    // Interface dispatch on a boxed record goes through bz_itable_lookup.
    // The descriptor (vtable[0]) is the type's descArray_ entry.
    std::unordered_map<const sema::TypeSymbol*, llvm::GlobalVariable*>
        boxVtable_;
    // §8.6: per-primitive box descriptor + vtable (int/bool/string/… have no
    // user TypeSymbol). The descriptor is the 2-element array [self, null] used
    // by `o is int`; the vtable matches the uniform prefix
    // [descriptor, itmap(null), Dispose(no-op), ToString] (§8.6/§8.5 #417), so a
    // boxed primitive dispatches ToString through slot 3 like any other box.
    // Keyed by the primitive's name.
    std::unordered_map<std::string, llvm::GlobalVariable*> primBoxDesc_;
    std::unordered_map<std::string, llvm::GlobalVariable*> primBoxVtable_;
    // §8.5 / §8.6 (#417): per-type ToString thunk caches (vtable slot 3). One
    // thunk per class / boxed value type, reused across modules-internal sites.
    std::unordered_map<const sema::TypeSymbol*, llvm::Function*> classToStringFns_;
    std::unordered_map<const sema::TypeSymbol*, llvm::Function*> boxToStringFns_;
    std::unordered_map<std::string, llvm::Function*> primBoxToStringFns_;
    // §8.6 / §12.7 (#417): every vtable / box-vtable / exception-vtable global
    // emitted, collected so emitVtableTable() can publish a null-terminated
    // array `bz$vtable$table` that bz_obj_is_string scans to tell a string (no
    // vtable at word 0) from a class/box (vtable at word 0) in an Object slot.
    std::vector<llvm::GlobalVariable*> allVtables_;
    // The published null-terminated vtable table (lazily emitted).
    llvm::GlobalVariable* vtableTableGV_ = nullptr;
    // §8.6 / §12.7 (#417): emit (once) and return the bz$vtable$table global.
    llvm::Value* vtableTablePtr();
    // #481 / §12.1 / §12.5 / §23: a virtual vtable slot is either a virtual
    // METHOD or a virtual PROPERTY accessor (getter or setter). A `virtual`/
    // `abstract`/`override` property's get (and set, when present) each occupy
    // their own slot, exactly like a method, so polymorphic property access
    // dispatches through the vtable. Exactly one of method/prop is non-null.
    struct VSlot {
        sema::MethodSymbol* method = nullptr;    // virtual method, or
        sema::PropertySymbol* prop = nullptr;    // virtual property accessor
        bool isSet = false;                      // prop only: false=get,true=set
    };
    // Per-class slot→member (vector index i = vtable slot i+4; slots 0..3 are
    // td$, itmap$, Dispose, ToString) and the virtual-member-name→slot map used
    // to inherit slots. A property getter is keyed by its name; a property
    // setter by name + "$set" so get/set get distinct slots.
    std::unordered_map<const sema::TypeSymbol*,
                       std::vector<VSlot>> vtableMethods_;
    std::unordered_map<const sema::TypeSymbol*,
                       std::unordered_map<std::string, unsigned>>
        vtableNameSlot_;
    // #481: a virtual property accessor's vtable slot, for dispatch at the
    // access site. Keyed by the PropertySymbol; the getter slot in propGetSlot_,
    // the setter slot in propSetSlot_. Absent ⇒ the accessor is non-virtual.
    std::unordered_map<const sema::PropertySymbol*, unsigned> propGetSlot_;
    std::unordered_map<const sema::PropertySymbol*, unsigned> propSetSlot_;
    // #481: a virtual property accessor IS just a method shape — `T get(ptr
    // this)` / `void set(ptr this, T value)`. Emitted once per (class, prop,
    // isSet), filled into the vtable slot, and called through the vtable at a
    // polymorphic access site. Keyed by (prop, isSet).
    std::map<std::pair<const sema::PropertySymbol*, bool>, llvm::Function*>
        propAccessorFns_;
    // §2.5 / §22.9 (issue #503): IMPORTED non-virtual property getter stubs.
    // Keyed by the imported PropertySymbol; the value is an ExternalLinkage
    // `T(ptr this)` declaration linking against the exporter's getter function
    // (`bz$<Class>$<Prop>$propget`). Populated by declareImportedPropertyGetters.
    std::unordered_map<const sema::PropertySymbol*, llvm::Function*>
        importedPropGetterFns_;
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
    // §24.7 (#477 B7): an interface EVENT also occupies an itab slot — its
    // position is APPENDED after the interface's non-static methods (so method
    // indices are unchanged). The slot holds a per-class accessor `ptr(this)`
    // returning the implementing object's backing EventSource<T>; interface-
    // typed event access dispatches through it like an interface method call.
    // Synchronous events only (async deferred). Keyed by the interface's
    // EventSymbol.
    std::unordered_map<const sema::EventSymbol*, unsigned> ifaceEventIndex_;
    // #481 / §13 / §23: an interface PROPERTY accessor also occupies an itab
    // slot. The getter index is in ifacePropertyGetIndex_, the setter (when the
    // interface property declares one) in ifacePropertySetIndex_. Both are
    // APPENDED after the interface's methods and events (so method/event indices
    // are unchanged): the getter, then — if present — the setter. Keyed by the
    // interface's PropertySymbol. Interface-typed property access dispatches
    // through these like an interface method call.
    std::unordered_map<const sema::PropertySymbol*, unsigned>
        ifacePropertyGetIndex_;
    std::unordered_map<const sema::PropertySymbol*, unsigned>
        ifacePropertySetIndex_;
    // Per (class × interface): the itab$C$I global (array of fn ptrs in
    // interface declaration order). Key: (cls, iface).
    std::map<std::pair<const sema::TypeSymbol*, const sema::TypeSymbol*>,
             llvm::GlobalVariable*>
        itables_;
    // §24.7 (#477 B7): per-class event-accessor functions (emit-once cache),
    // keyed by the class's own instance EventSymbol.
    std::unordered_map<const sema::EventSymbol*, llvm::Function*>
        eventAccessorFns_;
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

    llvm::Type* i8() { return llvm::Type::getInt8Ty(llvm_); }
    llvm::Type* i16() { return llvm::Type::getInt16Ty(llvm_); }
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

    // #481 / §12.1 / §12.5: a PROPERTY participates in virtual dispatch if it
    // is `virtual`, `override`, or `abstract`. A property declared on an
    // interface is implicitly virtual (an interface member, like an interface
    // method — §13 / §23). Static and indexer properties are excluded here
    // (indexers dispatch via their own §22.10 path; this gates plain
    // get/set properties). Returns false for a null symbol.
    bool isVirtualProperty(const sema::PropertySymbol* ps) {
        if (!ps || ps->isIndexer) return false;
        for (auto mod : ps->modifiers) {
            if (mod == "static") return false;
            if (mod == "virtual" || mod == "override" || mod == "abstract")
                return true;
        }
        // An interface property is implicitly virtual (§13 / §23).
        return ps->parent && ps->parent->kind == ast::TypeDeclKind::Interface;
    }

    // #481 / §23: does this property declare a setter? An auto-property without
    // a `readonly`/`private set` restriction has both; an expression-bodied or
    // block getter-only property has only a getter. Checked off the PropertyDecl
    // so it matches across base/override (the override must match the base's
    // accessor set per §12.1).
    static bool propertyHasSetter(const sema::PropertySymbol* ps) {
        auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl);
        if (!pd) return false;
        if (pd->bodyKind == ast::PropertyBodyKind::Expression) return false;
        if (pd->bodyKind == ast::PropertyBodyKind::Auto) {
            // Auto-property: read-write unless readonly. (Auto-properties are
            // not virtual unless on an interface, but interfaces don't carry
            // an Auto body — they use the abstract/block form — so this branch
            // is effectively for completeness.)
            for (auto mod : pd->modifiers)
                if (mod == "readonly") return false;
            return true;
        }
        // Block form: a set accessor present.
        for (const auto& a : pd->accessors)
            if (a.isSet) return true;
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

    // per stdlib §2.6: Guid is a 128-bit value type represented as { i64 lo, i64 hi }.
    // Passed by pointer to runtime functions (bz_guid_t*); in LLVM IR a Guid-typed
    // §4.4 (fix #521): decimal value struct { i32 lo32, i32 mid32, i32 hi32, i32 flags }.
    // .NET-compatible layout: 96-bit mantissa (lo/mid/hi) + scale (bits 16..23 of flags)
    // + sign (bit 31 of flags).  16 bytes total.  All runtime functions receive a ptr.
    llvm::StructType* decimalStructType() {
        if (!decimalStructTy_)
            decimalStructTy_ = llvm::StructType::create(
                llvm_, {i32(), i32(), i32(), i32()}, "bz.decimal");
        return decimalStructTy_;
    }

    // local is stored in an alloca of guidStructType(), and the address is passed to
    // the runtime call.
    llvm::StructType* guidStructType() {
        if (!guidStructTy_)
            guidStructTy_ = llvm::StructType::create(
                llvm_, {i64(), i64()}, "bz.guid");
        return guidStructTy_;
    }

    // fix #499 / §9.8: Range<T> value struct { i64 start, i64 end, i64 step, i8 flags }.
    // Field layout:
    //   0: i64  start  (lo bound; 0 if open-ended on the low side)
    //   1: i64  end    (hi bound; 0 if open-ended on the high side)
    //   2: i64  step   (default 1; signed; negative means descending — §16.4)
    //   3: i8   flags  (bit 0 = IsEndInclusive; bit 1 = startOpen; bit 2 = endOpen)
    llvm::StructType* rangeStructType() {
        if (!rangeStructTy_)
            rangeStructTy_ = llvm::StructType::create(
                llvm_,
                {i64(), i64(), i64(), llvm::Type::getInt8Ty(llvm_)},
                "bz.range");
        return rangeStructTy_;
    }

    // §4.15: ArraySpan<T>/ReadOnlyArraySpan<T> value struct { ptr source, i32 start, i32 length }.
    // The element type T determines only the element-access stride, not the struct layout.
    llvm::StructType* arraySpanStructType() {
        if (!arraySpanStructTy_)
            arraySpanStructTy_ = llvm::StructType::create(
                llvm_, {ptrTy(), i32(), i32()}, "bz.arrayspan");
        return arraySpanStructTy_;
    }

    // §4.15: test whether a TypeRef is ArraySpan<T> or ReadOnlyArraySpan<T>.
    static bool isArraySpanTypeName(const sema::TypeRef* rt) {
        if (!rt || rt->kind != sema::TypeRef::Kind::Named || rt->declaration)
            return false;
        if (!rt->astNode || !rt->astNode->name || rt->astNode->name->parts.empty())
            return false;
        auto n = rt->astNode->name->parts[0];
        return n == "ArraySpan" || n == "ReadOnlyArraySpan";
    }

    // §4.15: extract the `Source` array pointer (field 0) from a lowered span struct value.
    llvm::Value* spanSource(llvm::Value* spanVal) {
        auto* st = arraySpanStructType();
        auto* slot = entryAlloca(st);
        builder_.CreateStore(spanVal, slot);
        llvm::Value* ptr = builder_.CreateLoad(ptrTy(),
            builder_.CreateStructGEP(st, slot, 0));
        return ptr;
    }

    // §4.15: extract the `Start` offset (field 1) from a lowered span struct value, as i32.
    llvm::Value* spanStart(llvm::Value* spanVal) {
        auto* st = arraySpanStructType();
        auto* slot = entryAlloca(st);
        builder_.CreateStore(spanVal, slot);
        return builder_.CreateLoad(i32(), builder_.CreateStructGEP(st, slot, 1));
    }

    // §4.15: extract the `Length` field (field 2) from a lowered span struct value, as i32.
    llvm::Value* spanLength(llvm::Value* spanVal) {
        auto* st = arraySpanStructType();
        auto* slot = entryAlloca(st);
        builder_.CreateStore(spanVal, slot);
        return builder_.CreateLoad(i32(), builder_.CreateStructGEP(st, slot, 2));
    }

    // §4.15: build an ArraySpan struct value { source, start, length }.
    llvm::Value* buildArraySpan(llvm::Value* source, llvm::Value* start,
                                llvm::Value* length) {
        auto* st = arraySpanStructType();
        auto* slot = entryAlloca(st);
        builder_.CreateStore(llvm::ConstantAggregateZero::get(st), slot);
        builder_.CreateStore(source, builder_.CreateStructGEP(st, slot, 0));
        builder_.CreateStore(start,  builder_.CreateStructGEP(st, slot, 1));
        builder_.CreateStore(length, builder_.CreateStructGEP(st, slot, 2));
        return builder_.CreateLoad(st, slot, "span.val");
    }

    // fix #499 / §9.8: test whether a TypeRef is a Range type (set by sema
    // when visiting a RangeExpr or declared as `Range<T>`).
    static bool isRangeType(const sema::TypeRef* rt) {
        return rt &&
               rt->kind == sema::TypeRef::Kind::Named &&
               rt->declaration &&
               !rt->declaration->decl &&
               rt->declaration->name == "Range";
    }

    // fix #499 / §9.8: load the Range struct from the slot of a range-typed
    // expression (an IdentifierExpr whose lsym->declType is Range). Returns
    // the struct value `{ i64 start, i64 end, i64 step, i8 flags }`.
    llvm::Value* loadRangeStruct(const ast::Expr& target) {
        llvm::Value* v = lowerExpr(target);
        if (!v) return nullptr;
        // lowerExpr for a local of range type returns the struct value directly.
        return v;
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

    // §3.4 / §4.6 / issue #474: lower `s[range]` — a Range index over a string —
    // to a fresh, width-preserving `%bz.string` substring (rc := 1, the
    // producer's reference). `self` is the source bz_string*; `range` supplies
    // the bounds. Inclusive (`a..b`) includes both ends; half-open (`a..<b`)
    // excludes the upper; open-ended forms (`..b`, `a..`, `..`) extend to the
    // start/end of the string. Out-of-range endpoints raise
    // IndexOutOfRangeException; an empty range produces the empty string.
    llvm::Value* emitStringRangeSlice(llvm::Value* self,
                                      const ast::RangeExpr& range);

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

    // §9.8 / §2.14–2.15 (issue #550): BinarySerializer/BinaryDeserializer
    // construction, ToBytes, AsReadOnlySpan, and synthesized Serialize/
    // Deserialize on records and enums.  Returns the lowered value, or
    // nullptr if the call is not a serialization-related call (or is void).
    // Use isSerializationCall() to distinguish "void result" from "not ours".
    llvm::Value* tryLowerSerializationCall(const ast::CallExpr& call);
    // Returns true iff the call should be handled by tryLowerSerializationCall.
    bool isSerializationCall(const ast::CallExpr& call,
                             const ast::MemberAccessExpr& ma);

    // Helpers for synthesized Serialize / Deserialize bodies.
    // Emit the Serialize body for a positional record value and a serializer ptr.
    void emitRecordSerialize(const sema::TypeSymbol* ts,
                             llvm::Value* recvVal,
                             llvm::Value* serPtr);
    // Emit a single field write (dispatches on type).
    void emitSerializeField(llvm::Value* val,
                            const sema::TypeRef* fieldTy,
                            llvm::Value* serPtr);
    // Emit the Serialize body for an enum value (writes member name string).
    void emitEnumSerialize(const sema::TypeSymbol* ts,
                           llvm::Value* enumVal,
                           llvm::Value* serPtr);
    // Emit the Deserialize body for a record: returns the constructed struct value.
    llvm::Value* emitRecordDeserialize(const sema::TypeSymbol* ts,
                                       llvm::Value* derPtr);
    // Emit a single field read (dispatches on type).
    llvm::Value* emitDeserializeField(const sema::TypeRef* fieldTy,
                                      llvm::Value* derPtr);
    // Emit the Deserialize body for an enum: returns i32 constant.
    llvm::Value* emitEnumDeserialize(const sema::TypeSymbol* ts,
                                     llvm::Value* derPtr);

    // §9.8 tier 2 — class synthesis (tree shape).
    // Emit the Serialize body for a `serialized tree` class object pointer.
    void emitClassSerialize(const sema::TypeSymbol* ts,
                            llvm::Value* objPtr,
                            llvm::Value* serPtr);
    // Emit the Deserialize body for a `serialized tree` class.
    // Returns an owned heap pointer (ptr) to the newly constructed instance.
    llvm::Value* emitClassDeserialize(const sema::TypeSymbol* ts,
                                      llvm::Value* derPtr);

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
                                       const ast::MemberAccessExpr& ma,
                                       llvm::Value* self = nullptr);

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
        // fix #569 / §14.1 / #460: inside a monomorphized body the value slot may
        // be a SPECIALIZED record struct (`record.Pt$spec0`) while `val` is the
        // unspecialized `record.Pt` (or vice versa) — layout-identical but a
        // distinct named LLVM type, which insertvalue rejects. Reinterpret `val`
        // to the slot type via an alloca round-trip when the layouts match.
        val = reinterpretToType(val, st->getElementType(1));
        llvm::Value* res = builder_.CreateInsertValue(
            llvm::UndefValue::get(st),
            llvm::ConstantInt::get(nullablePrimHasValTy(), 1), {0u});
        return builder_.CreateInsertValue(res, val, {1u});
    }
    // fix #569 / #460: if `v`'s type differs from `want` but both are
    // layout-identical aggregates (the specialized-vs-unspecialized record-struct
    // mismatch a monomorphized generic body induces), reinterpret `v` to `want`
    // through an alloca round-trip (store as v's type, reload as `want`). Returns
    // `v` unchanged when the types already match or are not safely reinterpretable.
    llvm::Value* reinterpretToType(llvm::Value* v, llvm::Type* want) {
        if (!v || !want || v->getType() == want) return v;
        auto* vSt = llvm::dyn_cast<llvm::StructType>(v->getType());
        auto* wSt = llvm::dyn_cast<llvm::StructType>(want);
        if (vSt && wSt && vSt->isLayoutIdentical(wSt)) {
            llvm::AllocaInst* tmp = entryAlloca(vSt);
            builder_.CreateStore(v, tmp);
            return builder_.CreateLoad(wSt, tmp, "nv.reinterp");
        }
        return v;
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

    // fix #568 / §14.1 / §7.1: coerce a ReturnStmt value `v` to the function's
    // declared nullable-VALUE return type when that type is reached through the
    // active monomorphization (`T?` with T = a value type). `declRet` is the
    // resolved declared return TypeRef. When the effective return type is a
    // nullable value type and `v` is not already that struct:
    //   * a null pointer  → the {i8 0, T 0} null constant;
    //   * a bare T value matching the value slot → wrapped {i8 1, v};
    //   * a bare T value that is a LAYOUT-EQUAL but differently-named struct
    //     (the specialized-vs-unspecialized record-struct mismatch #460 induces
    //     inside a monomorphized body) → reinterpreted via an alloca round-trip,
    //     then wrapped.
    // Returns `v` unchanged when no nullable-value coercion applies (a non-
    // generic nullable value return already lowers to the struct directly, and
    // a reference `?` / non-nullable return is untouched). Defined in
    // LlvmBackend_Layout.cpp (needs nullableValueStructTy / recordStruct).
    llvm::Value* coerceReturnToNullableValue(llvm::Value* v,
                                             const sema::TypeRef* declRet);

    // §22.10: find the indexer PropertySymbol on a TypeSymbol, if any.
    const sema::PropertySymbol* findIndexer(
        const sema::TypeSymbol* ts) const;

    // §22.10: emit the getter of an indexer property via inline body.
    // Returns the loaded value, or null if no indexer applies.
    llvm::Value* lowerIndexerGet(const ast::IndexExpr& ix);

    // §22.10: emit the setter of an indexer property via inline body.
    // Returns true if the setter was handled.
    bool lowerIndexerSet(const ast::IndexExpr& ix, llvm::Value* val);

    // §20 / §23.4: compound assignment `a[i] OP= v` through a user-defined
    // indexer — evaluates the receiver and each index param ONCE, calls the
    // getter to read the current value, applies OP, then calls the setter to
    // write the result back. Returns true if handled (user-defined indexer
    // target); false to let the caller fall through to other paths.
    bool lowerIndexerCompound(const ast::AssignmentStmt& as);

    // fix #508 / stdlib §4.4 / §4.5 / §17.15: compound assignment `arr[i] OP= v`
    // on a HEAP-collection Array<T>/OwnedArray<T>/Dictionary<K,V> indexer.
    // Evaluates receiver and index ONCE (array path), reads via bz_array_get /
    // lowerCollectionIndexerGet, applies OP, writes back via bz_array_set /
    // lowerCollectionIndexerSet. Returns true if handled; false if the target is
    // not a built-in heap collection (user-defined indexer, inline array).
    bool lowerCollectionIndexerCompound(const ast::AssignmentStmt& as);

    // fix #613 / §25.1 / §4.4 / §4.8: `arr[i]++` / `arr[i]--` / `dict[k]++` /
    // `dict[k]--` on a BUILT-IN heap collection (Array<T>, Dictionary<K,V>, and
    // their owning/list-interface variants). Reads via lowerCollectionIndexerGet,
    // computes ±1, writes back via lowerCollectionIndexerSet. Returns true if
    // handled; false if the target is not a built-in heap-collection IndexExpr.
    bool lowerCollectionIndexerIncrDecr(const ast::IncrementStmt& inc);

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
    bool lowerCollectionIndexerSet(const ast::IndexExpr& ix, llvm::Value* val,
                                   const ast::Expr* valueExpr = nullptr);

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

    // #481 / §12.1 / §12.5 / §23: emit (or retrieve cached) a uniform-signature
    // accessor FUNCTION for a virtual property, so it can be installed in a
    // vtable/itable slot and called through the receiver's vtable at a
    // polymorphic access site. Getter signature: `T get(ptr this)`; setter:
    // `void set(ptr this, T value)`. Handles arrow, block, and auto-property
    // accessors uniformly (the bodies reuse the existing inline lowering). An
    // ABSTRACT accessor (no body) returns nullptr — the slot stays null on the
    // abstract base and is filled by the override's function. `cls` is the class
    // whose accessor implementation to emit (the most-derived owner of the
    // accessor for `ps`).
    llvm::Function* ensurePropertyAccessorFn(const sema::TypeSymbol* cls,
                                             const sema::PropertySymbol* ps,
                                             bool isSet);

    // #481 / §12.1 / §12.5 / §13 / §23: if `e` is a read of a VIRTUAL (or
    // interface) property through a (potentially) polymorphic receiver, lower it
    // as a dynamic dispatch through the receiver's vtable (class virtual) or
    // itable (interface) and return the result. Returns nullptr when `e` is not
    // a virtual-property read (the caller falls through to the static getter
    // path). `base.P` (base-qualified) bypasses dispatch like a base method
    // call (§12.3), so it returns nullptr there too.
    llvm::Value* lowerVirtualPropertyGet(const ast::Expr& e);

    // #481: companion for `obj.P := v` — dispatch a virtual/interface property
    // SETTER through the vtable/itable with the already-lowered value `v`.
    // Returns true when it handled the assignment; false to fall through to the
    // static setter path.
    bool lowerVirtualPropertySet(const ast::Expr& target, llvm::Value* v);

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

    // issue #462 / §14.1: the declared type of a record storage member RESOLVED
    // through the active generic substitution. When the member's declared type is
    // a bare type parameter (`T` — a single-part Named with no `declaration`) and
    // `genericSubstSemaTy_` binds it to a concrete type for THIS instantiation,
    // return the concrete sema TypeRef (e.g. `T` → `string`); otherwise return
    // the unresolved declared type. The synthesized value-semantics members
    // (emitStructEquality / emitRecordToString / emitRecordHashCode over a
    // GENERIC record) drive per-field equality/format/hash off this, so a
    // `Box<string>` string field compares by CONTENT (not pointer bits), formats
    // its value, and hashes by content — instead of falling through to the
    // type-erased shape fallback. A non-generic record (empty subst, or a
    // declared type that is not a bound param) keeps its declared type unchanged.
    const sema::TypeRef* recordMemberTypeResolved(const sema::Symbol* m);

    // issue #460 / #462 / §14.1: a save/restore guard that installs the
    // type-parameter substitution for a CONCRETE generic-record instantiation
    // (e.g. `Box<string>`), so a subsequent `recordStruct` lays the T-typed
    // fields out as the specialized concrete types (the same mechanism #460
    // added to recordStruct + lowerNew). Constructed from the record's resolved
    // TypeRef `rt`; a no-op for a non-generic record or a TypeRef whose
    // genericArgs do not match the declared params. The OUTER substitution is
    // saved on construction and restored on destruction, so nested generics
    // chain and the enclosing instantiation is unaffected. RAII so every early
    // return / exception path restores correctly.
    struct RecordSubstGuard {
        Emitter* e;
        std::unordered_map<std::string_view, llvm::Type*> savedSubst;
        std::unordered_map<std::string_view, const sema::TypeRef*> savedSemaTy;
        bool active = false;
        RecordSubstGuard(Emitter* em, const sema::TypeRef* rt);
        ~RecordSubstGuard();
        RecordSubstGuard(const RecordSubstGuard&) = delete;
        RecordSubstGuard& operator=(const RecordSubstGuard&) = delete;
    };

    // §33: the entry-point method's LLVM function is named `main`; every
    // other static method gets a mangled, collision-free name.
    std::string functionName(sema::MethodSymbol* ms);

    // §26 (#433): is `ms` an extension method (declared in an `extension`
    // block)? Such a method has no `parent` TypeSymbol; its receiver type is
    // the extension's targetType.
    static bool isExtensionMethod(const sema::MethodSymbol* ms) {
        return ms && ms->extensionDecl != nullptr;
    }
    // §26.1: the receiver (`this`) representation for an extension method.
    // Returns the LLVM type of the `this` parameter and sets *byPointer to
    // true when `this` is passed as a pointer (class receiver, or a record
    // spilled to a stack slot) versus by value (primitive / enum). Returns
    // null when the extension's target type is unavailable.
    llvm::Type* extensionThisType(const sema::MethodSymbol* ms,
                                  bool* byPointer);

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
    // §10 / #482 fix A: helper — populate enumValue_ for a single enum
    // TypeSymbol (both module-level and nested-in-class).
    void populateEnumValuesFromDecl(const sema::TypeSymbol* ts);

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

    // §2.5 / §22.9 (issue #503): emit a named ExternalLinkage getter function
    // `bz$<Class>$<Prop>$propget(ptr %this) -> T` for every PUBLIC, non-virtual,
    // non-indexer property on every PUBLIC, non-generic LOCAL class. This gives
    // the importer a stable cross-module symbol to call for a non-virtual property
    // getter. Only called in the exporter (local symbols). Virtual properties
    // already dispatch through the vtable; they do not need this function.
    void emitExportedPropertyGetters();

    // §2.5 / §22.9 (issue #503): declare EXTERNAL getter stubs for every
    // imported PropertySymbol that is not virtual (not in propGetSlot_). Called
    // after buildVtable (which populates propGetSlot_), so that we correctly
    // distinguish virtual from non-virtual imported properties. The stubs are
    // stored in importedPropGetterFns_ for use by the property-access lowering.
    void declareImportedPropertyGetters();

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

    // #481 / §13 / §23: the (non-static, non-indexer) PropertySymbol named
    // `name` declared on `cls` or an inherited base — the concrete property an
    // interface property or an inherited virtual property resolves to on the
    // implementing/most-derived class. Returns the most-derived match (walks
    // base-last), so a derived override is preferred over the base declaration.
    sema::PropertySymbol* classPropertyByName(const sema::TypeSymbol* cls,
                                              std::string_view name) {
        for (const sema::TypeSymbol* t = cls; t;) {
            for (sema::Symbol* m : t->members) {
                auto* ps = dynamic_cast<sema::PropertySymbol*>(m);
                if (!ps || ps->isIndexer || ps->name != name) continue;
                bool isStatic = false;
                for (auto mod : ps->modifiers)
                    if (mod == "static") { isStatic = true; break; }
                if (!isStatic) return ps;
            }
            t = baseClassOf(t);
        }
        return nullptr;
    }

    // §24.7 (#477 B7): the instance EventSymbol named `name` declared on `cls`
    // (or an inherited base class) — the concrete backing-field owner an
    // interface event resolves to on the implementing class. Sync events only.
    const sema::EventSymbol* classEventByName(const sema::TypeSymbol* cls,
                                              std::string_view name) {
        for (const sema::TypeSymbol* t = cls; t;) {
            for (sema::Symbol* m : t->members) {
                auto* es = dynamic_cast<sema::EventSymbol*>(m);
                if (!es || es->name != name) continue;
                auto* ed = dynamic_cast<const ast::EventDecl*>(es->decl);
                if (ed && !ed->isStatic && !ed->isAsync &&
                    fieldIndex_.count(es))
                    return es;
            }
            t = baseClassOf(t);
        }
        return nullptr;
    }

    // §24.7 (#477 B7): emit (once, cached) a per-class accessor function
    // `ptr bz$<Class>$<Event>$get(ptr this)` that returns the object's backing
    // EventSource<T>. Placed in the class's itab so interface-typed event
    // access reaches the real source. `es` is the class's own instance event.
    llvm::Function* emitEventAccessor(const sema::TypeSymbol* cls,
                                      const sema::EventSymbol* es);

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

    // #473 / §19.3: descriptor global for a NamedPattern type-test name
    // (`Dog`, `M.Dog`), resolved by name against the local type symbols.
    llvm::GlobalVariable* descriptorForPatternName(
        const ast::QualifiedName* name);

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
    //
    // #662 foundation B: for IMPORTED fields (fs->decl == nullptr), the field's
    // type rides ctx_.importedMemberType(fs).
    bool isOwnedBuiltinExceptionField(const sema::FieldSymbol* fs) {
        if (!hasOwnedModifier(fs)) return false;
        // #662 foundation B: imported field — type from importedMemberType.
        if (!fs->decl)
            return isBuiltinExceptionTypeRef(ctx_.importedMemberType(fs));
        auto* fd = dynamic_cast<const ast::FieldDecl*>(fs->decl);
        if (!fd || !fd->type) return false;
        return isBuiltinExceptionTypeRef(ctx_.resolvedType(*fd->type));
    }

    // #595 / §12 / §31: detect whether a user-defined class or any of its user-
    // class ancestors has a built-in exception type as a direct AST-declared base
    // (e.g. `class ErrA: Exception`, or `class ErrB: ErrA` where ErrA: Exception).
    //
    // Built-in exception types have no user TypeSymbol (their TypeRef carries
    // `declaration == nullptr` and they are absent from TypeSymbol::baseTypes —
    // see ExprTypecheck.cpp line ~820).  We walk the AST TypeDecl::baseTypes list
    // at each level of the user class hierarchy, matching base-type names against
    // builtinVtable_.
    //
    // The FULL ancestor chain is checked (not just the direct base) because a
    // multi-level user hierarchy (ErrB: ErrA, ErrA: Exception) must embed the same
    // message-ptr prefix in ErrB's struct so byte offset 8 is uniform across the
    // entire user-exception subtree.
    //
    // Returns the built-in exception base name (e.g. "Exception"), or "" if none.
    std::string directBuiltinExceptionBase(const sema::TypeSymbol* t) const {
        if (!t || t->kind != ast::TypeDeclKind::Class) return {};
        // Walk up the user-class chain (TypeSymbol::baseTypes gives us user bases).
        for (const sema::TypeSymbol* cur = t; cur;) {
            auto* td = dynamic_cast<const ast::TypeDecl*>(cur->decl);
            if (td) {
                // Check each AST-declared base for a builtin exception name.
                for (const auto& bt : td->baseTypes) {
                    if (!bt || !bt->name || bt->name->parts.empty()) continue;
                    std::string n(bt->name->parts.back());
                    if (builtinVtable_.find(n) != builtinVtable_.end()) return n;
                }
            }
            // Advance to the next user-class base via the TypeSymbol chain.
            const sema::TypeSymbol* next = nullptr;
            for (const sema::TypeSymbol* b : cur->baseTypes) {
                if (b && b->kind == ast::TypeDeclKind::Class && b->decl) {
                    next = b;
                    break;
                }
            }
            cur = next;
        }
        return {};
    }

    // #595: true when `t` (or any user-class ancestor) inherits a built-in
    // exception type.  Used by classStruct / disposedFlagIndex to insert the
    // inherited message-ptr slot so `e.Message` reads the right offset whether
    // `e` is typed as the user class OR as the built-in Exception base.
    bool hasBuiltinExceptionBase(const sema::TypeSymbol* t) const {
        return !directBuiltinExceptionBase(t).empty();
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

    // §8.6 / §8.5 (#417): the fixed vtable slot index of the uniform virtual
    // ToString method-pointer. Layout: [0]=td, [1]=itmap, [2]=Dispose,
    // [3]=ToString, [4..]=virtuals. A dispatch `o.ToString()` on an Object /
    // interface receiver loads vtable[3] and calls it.
    static constexpr unsigned kToStringSlot = 3u;

    // §8.5 / §8.6 (#417): emit (once) a ToString thunk `ptr(ptr this)` for a
    // CLASS — the user override if present (walking the class chain), else a
    // default returning the type's name (§8.5 "For classes: the type's
    // qualified name"). Returned as a Constant for the class vtable's slot 3.
    llvm::Constant* classToStringThunk(const sema::TypeSymbol* ts);

    // §8.5 / §8.6 (#417): emit (once) a ToString thunk `ptr(ptr boxPtr)` for a
    // boxed RECORD or boxed ENUM. The thunk GEPs past the box header (vtable +
    // rc, two words) to the payload, then builds the §8.5/§9.1/§9.2 string:
    // a record's synthesized property-listing form, an enum's type name.
    llvm::Constant* boxToStringThunk(const sema::TypeSymbol* ts);

    // §8.5 / §8.6 (#417): emit (once) a ToString thunk `ptr(ptr boxPtr)` for a
    // boxed PRIMITIVE (int/bool/char/…). Loads the value from the box payload
    // (two words in) and formats it via emitPrimitiveToString. Keyed by the
    // primitive's stable name (primBoxName).
    llvm::Constant* primBoxToStringThunk(sema::PrimitiveType prim,
                                         const char* name);

    // §8.5 / §8.6 (#417): lower `o.ToString()` where `o` is Object/interface
    // (no resolved MethodSymbol). A string operand has no vtable, so the
    // emitted code branches on bz_obj_is_string: a string returns itself, a
    // class/box dispatches through vtable[kToStringSlot]. Returns the result
    // string, or nullptr if the receiver cannot be lowered.
    llvm::Value* lowerObjectToString(llvm::Value* recv);

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

    // §29.2 (#437): wrap a raw bz_array_t* in a type-specific vtable-backed
    // enumerator whose MoveNext → i1 and Current → T match the iterator state
    // machine's vtable protocol exactly.  Used when Array<T> is passed where
    // IEnumerable<T> is expected; the callee uses vtable dispatch uniformly.
    llvm::Value* wrapArrayAsIEnumerable(llvm::Value* arr,
                                        const sema::TypeRef* iEnumTy);

    // §29.2 (#552): wrap a user-class instance (implementing IEnumerable<T>)
    // in a type-specific vtable-backed enumerator compatible with the iterator
    // state machine protocol ([0]=td, [1]=Dispose, [2]=MoveNext→i1,
    // [3]=Current→T). Eagerly calls GetEnumerator() on the source and forwards
    // MoveNext/Current/Dispose to the concrete enumerator class's methods.
    // Used when a user class is passed (or assigned) where IEnumerable<T> is
    // expected, so that lowerIEnumerableBuiltinFor's vtable dispatch works
    // uniformly for all IEnumerable<T> sources. Returns src unchanged on
    // resolution failure (GetEnumerator or enumerator-class not found).
    llvm::Value* wrapUserClassAsIEnumerable(llvm::Value* src,
                                            const sema::TypeSymbol* srcTs,
                                            const sema::TypeRef* iEnumTy);

    // §29.2 (#552): generate (or reuse) a standalone thunk function for an
    // expression-form property getter (`property P: T => expr` or
    // `{ get => expr }`). The thunk signature is `T(ptr self)`, matching the
    // block-getter protocol used by ensurePropertyBlockGetter. Used by
    // wrapUserClassAsIEnumerable to wrap a Current property whose body is an
    // expression rather than a block.
    llvm::Function* ensureExprGetterThunk(const sema::PropertySymbol* ps,
                                          const ast::Expr* getterExpr,
                                          llvm::Type* retTy);

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
    bool classHasOwnedTypeParamField(const sema::TypeSymbol* ts);

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

    // #467 / §28.3: dispose disposeStack_ scopes in the half-open index range
    // [lo, hi), innermost (higher index) first. Used by a try-with-finally
    // re-propagating an exception to its enclosing handler to dispose the scopes
    // between the inner try and that handler (e.g. an outer `using var`) AFTER
    // the explicit finally body has run. See the definition for the full
    // rationale.
    void emitUnwindDisposalsInRange(std::size_t lo, std::size_t hi);

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

    // ── §28.11 v0.134 element-slot policy ────────────────────────────────
    // #602: a classification of a single collection element type and family
    // that drives all per-slot bookkeeping decisions.  The factory
    // `elementPolicyFor` is the ONLY place this classification is encoded;
    // the existing helpers below are thin delegates to it (batch 1).
    struct ElementPolicy {
        // Mirrors the four rows of the §28.11 v0.134 "element slot policy"
        // table (column: element category).
        enum class Category {
            // Row 1: value type (non-move-only) — copies; no slot bookkeeping.
            Value,
            // Row 2: runtime-managed reference (string, function types, boxed
            // values) — shared references; retain/release on insert/remove.
            RuntimeManaged,
            // Row 3, borrowing family: class/interface borrow — stores a
            // reference; no disposal obligation on the collection side.
            ClassBorrow,
            // Row 3, owning family: class/interface owned — disposal cascades
            // via vtable at collection Dispose/Remove/Clear.
            ClassOwned,
        };

        Category cat;
        // LLVM type for a slot of this category.  Null for Value (concrete
        // type varies per-element; callers use lowerTypeRef directly).
        llvm::Type* slotTy;
        // The `void(void*)` dispose callback for ClassOwned; null for all
        // other categories.
        llvm::Function* disposerThunk;
        // True iff this category requires retain/release bookkeeping (i.e.
        // strings and function-type elements in the RuntimeManaged row).
        bool retainRelease;
    };

    // #602 / §28.11 v0.134: classify `elemTy` against the slot-policy table.
    // `owningFamily` is true when the containing collection is in the "Owned"
    // family.  The factory's rows mirror the spec table with per-row citations.
    ElementPolicy elementPolicyFor(const sema::TypeRef* elemTy,
                                   bool owningFamily);

    // #602 batch 4 / §28.11 v0.135: convenience wrapper that returns the
    // dispose_elem `llvm::Value*` argument for any runtime function that takes
    // a `void(*)(void*)` element-finalizer:
    //   - ClassOwned  → cast of elementPolicyFor().disposerThunk to ptr
    //   - anything else → null pointer
    // Use this at EVERY site that passes a disposer to the runtime so the
    // null-vs-thunk decision lives in exactly one place (the #545 hook).
    llvm::Value* elementDisposerArg(const sema::TypeRef* elemTy,
                                    bool owningFamily) {
        llvm::Function* thunk = elementPolicyFor(elemTy, owningFamily).disposerThunk;
        return thunk ? llvm::cast<llvm::Value>(thunk)
                     : llvm::ConstantPointerNull::get(ptrTy());
    }

    // #362 / stdlib §4.5: a module-level `void(void*)` thunk that runs an
    // owning collection element's destructor via dynamic dispatch (vtable
    // slot 1), null-checking the element first. Suitable as the `dispose_elem`
    // callback for bz_array_free / bz_array_clear_disposing /
    // bz_array_remove_at_disposing. Built once per module and cached.
    llvm::Function* elementDisposerThunk();
    llvm::Function* sharedReleaseThunk();
    llvm::Function* weakReleaseThunk();
    llvm::Function* groupingFreeThunk(); // stdlib §5 (#651): IGrouping disposal

    // #429 / §4.6 / §28.9: a module-level `void(void*)` thunk that calls
    // bz_string_release on the element (the slot holds a bz_string*). Used as the
    // per-element callback for bz_array_free / bz_array_clear_disposing /
    // bz_array_remove_at_disposing when a borrowing Array<string> co-owns a +1
    // per element (retained at insert / copy). release is a no-op on a sentinel/
    // literal or null. Built once per module and cached.
    llvm::Function* stringElementReleaseThunk();

    // #429 / §4.6 / §28.9 / #602 batch 4: when `collTy` is a borrowing
    // Array<string> (or Stack/Queue<string>), emit bz_array_retain_strings(arr)
    // over the freshly produced array `arr` (a copy/materialize) so it
    // independently co-owns a +1 per element, matching the per-element release
    // its later free/dispose will issue. A no-op for any other element type.
    // `arr` must already hold its final elements.
    //
    // #602 batch 4 / §28.11 v0.135: the RuntimeManaged-category decision is
    // derived from elementPolicyFor (the #545 hook) — this is the only place
    // that decides when a bulk bz_array_retain_strings is needed. isStringElementArray
    // was deleted (batch 4) since its only use was this helper.
    void emitArrayRetainStringsIfStringArray(const sema::TypeRef* collTy,
                                             llvm::Value* arr) {
        if (!arr || !collTy || !collTy->astNode || !collTy->astNode->name ||
            collTy->astNode->name->parts.empty()) return;
        std::string_view cn = collTy->astNode->name->parts[0];
        if (cn != "Array" && cn != "Stack" && cn != "Queue") return;
        if (collTy->genericArgs.empty() || !collTy->genericArgs[0]) return;
        const sema::TypeRef* el = collTy->genericArgs[0];
        if (el->nullable) return;
        // per §28.11 v0.135: RuntimeManaged AND specifically `string` (not fn type).
        ElementPolicy pol = elementPolicyFor(el, /*owningFamily=*/false);
        if (pol.cat != ElementPolicy::Category::RuntimeManaged) return;
        if (el->kind != sema::TypeRef::Kind::Primitive ||
            el->primitive != sema::PrimitiveType::String) return;
        auto* fn = declareC("bz_array_retain_strings",
                            llvm::Type::getVoidTy(llvm_), {ptrTy()});
        builder_.CreateCall(fn, {arr});
    }

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
    //
    // #662 foundation B: for IMPORTED fields (fs->decl == nullptr), the field's
    // declared type is in ctx_.importedMemberType(fs) and the param name is in
    // that TypeRef's importedName (set by the loader when declaration == nullptr).
    const sema::TypeRef* ownedTypeParamConcreteType(const sema::FieldSymbol* fs) {
        if (!hasOwnedModifier(fs)) return nullptr;
        if (genericSubstSemaTy_.empty()) return nullptr;
        // #662 foundation B / §28.5: imported field — type in importedMemberType.
        if (!fs->decl) {
            const sema::TypeRef* rt = ctx_.importedMemberType(fs);
            if (!rt || rt->kind != sema::TypeRef::Kind::Named ||
                rt->declaration != nullptr)
                return nullptr;
            // The param name rides importedName for loader-reconstructed TypeRefs
            // whose declaration is null (generic-param references have no user
            // TypeSymbol, so lookupNamed returns null and the loader records the
            // raw name in importedName — see ModuleLoader.cpp convert(), §2.5).
            if (rt->importedName.empty()) return nullptr;
            auto it = genericSubstSemaTy_.find(rt->importedName);
            return it != genericSubstSemaTy_.end() ? it->second : nullptr;
        }
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

    // per §14.2: a stable identity token for a resolved concrete type, used to
    // make the generic-specialization cache key TYPE-AWARE. Two concrete types
    // that erase to the same LLVM type (every class/interface erases to `ptr`)
    // must still map to DIFFERENT tokens so their specializations don't collide.
    //  * Named (class/interface/record/enum): the TypeSymbol* declaration — a
    //    stable per-type pointer; nullable variants share a token (the LLVM type
    //    is identical and class-typed bodies don't branch on a subst's
    //    nullability).
    //  * Primitive: a small tagged integer cast to a pointer, distinct from any
    //    real heap pointer (low values are never valid TypeSymbol addresses).
    //  * anything else (Unit/Function/InlineArray/Error or an unresolved param):
    //    nullptr — these are already distinguished by the LLVM type vector that
    //    sits alongside this token in the key.
    static const void* typeRefIdentityToken(const sema::TypeRef* rt) {
        if (!rt) return nullptr;
        if (rt->kind == sema::TypeRef::Kind::Named && rt->declaration)
            return static_cast<const void*>(rt->declaration);
        if (rt->kind == sema::TypeRef::Kind::Primitive)
            // Tag primitives into a tiny address range that cannot alias a real
            // TypeSymbol*; +1 so PrimitiveType::None (0) is still non-null.
            return reinterpret_cast<const void*>(
                static_cast<std::uintptr_t>(rt->primitive) + 1);
        return nullptr;
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
    //
    // #662 foundation B: for IMPORTED fields (fs->decl == nullptr), the field's
    // declared type rides ctx_.importedMemberType(fs); the param name is in that
    // TypeRef's importedName when declaration == nullptr (loader sets it from the
    // raw namedName when lookupNamed returns null — see ModuleLoader.cpp, §2.5).
    bool isOwnedTypeParamFieldShape(const sema::FieldSymbol* fs) {
        if (!hasOwnedModifier(fs)) return false;
        if (!fs->parent) return false;
        // #662 foundation B: imported field — type and param name from side table.
        if (!fs->decl) {
            const sema::TypeRef* rt = ctx_.importedMemberType(fs);
            if (!rt || rt->kind != sema::TypeRef::Kind::Named ||
                rt->declaration != nullptr || rt->importedName.empty() ||
                !rt->genericArgs.empty())
                return false;
            std::string_view pname = rt->importedName;
            for (const sema::Symbol* gp : fs->parent->genericParams)
                if (gp && gp->name == pname) return true;
            return false;
        }
        auto* fd = dynamic_cast<const ast::FieldDecl*>(fs->decl);
        if (!fd || !fd->type) return false;
        const ast::Type* ty = fd->type.get();
        if (!ty->name || ty->name->parts.size() != 1 ||
            !ty->genericArgs.empty())
            return false;
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
    //
    // #662 foundation B: for IMPORTED fields (fs->decl == nullptr), the field's
    // declared type rides ctx_.importedMemberType(fs) rather than being read
    // from the AST. The declaration pointer on the TypeRef is the TypeSymbol*
    // resolved at load time; importedName covers the case where lookupNamed
    // returned null (generic-param fields and builtin types — §2.5).
    bool isOwnedReferenceField(const sema::FieldSymbol* fs) {
        if (!hasOwnedModifier(fs)) return false;
        const sema::TypeRef* rt = nullptr;
        // #662 foundation B: imported field — type from importedMemberType.
        if (!fs->decl) {
            rt = ctx_.importedMemberType(fs);
        } else {
            auto* fd = dynamic_cast<const ast::FieldDecl*>(fs->decl);
            if (!fd || !fd->type) return false;
            rt = ctx_.resolvedType(*fd->type);
        }
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

    // §28.13 / stdlib §2.16 (#620): true when `fs` is an `owned` field of type
    // Shared<T> or Weak<T>.  These are compiler-known heap-pointer types with no
    // user TypeDecl; they are not caught by isOwnedReferenceField (which requires
    // a class/interface declaration) and have no vtable (so dynamic Dispose does
    // not apply). The enclosing class's disposal cascade must call
    // bz_shared_release / bz_weak_release directly on the field value.
    // isWeak is set to true for Weak<T> fields (selects bz_weak_release).
    //
    // #662 foundation B: for IMPORTED fields (fs->decl == nullptr), the field's
    // type rides ctx_.importedMemberType(fs); the type name is in importedName
    // when declaration == nullptr (loader stores it for builtins not exported as
    // user types — Shared<T>/Weak<T> are runtime builtins, §2.5/§28.13).
    bool isOwnedSharedWeakField(const sema::FieldSymbol* fs,
                                bool* isWeak = nullptr) const {
        if (!hasOwnedModifier(fs)) return false;
        const sema::TypeRef* rt = nullptr;
        // #662 foundation B: imported field — type from importedMemberType.
        if (!fs->decl) {
            rt = ctx_.importedMemberType(fs);
        } else {
            auto* fd = dynamic_cast<const ast::FieldDecl*>(fs->decl);
            if (!fd || !fd->type) return false;
            rt = ctx_.resolvedType(*fd->type);
        }
        if (!rt || rt->kind != sema::TypeRef::Kind::Named) return false;
        if (rt->declaration) return false;  // user TypeDecl — not a builtin
        // Local (non-imported) field: name via astNode.
        std::string_view n;
        if (!fs->decl) {
            // Imported field: loader sets importedName when declaration is null.
            n = rt->importedName;
        } else {
            if (!rt->astNode || !rt->astNode->name ||
                rt->astNode->name->parts.empty()) return false;
            n = rt->astNode->name->parts[0];
        }
        if (n == "Shared") { if (isWeak) *isWeak = false; return true; }
        if (n == "Weak")   { if (isWeak) *isWeak = true;  return true; }
        return false;
    }

    // #362 / stdlib §4.5: if `fs` is an `owned` field whose type is a heap
    // collection (Array<T>/OwnedArray<T>/…, builtins with no TypeSymbol so
    // isOwnedReferenceField does not match them), return its resolved TypeRef;
    // otherwise null. The owner's Dispose frees such a field's backing and
    // cascades disposal into its class elements for an owning collection.
    //
    // #662 foundation B: for IMPORTED fields (fs->decl == nullptr), the field's
    // type rides ctx_.importedMemberType(fs).  Collection builtins have no user
    // TypeDecl so declaration == nullptr and importedName carries "Array" etc.;
    // isCollectionTypeName / isLinkedListTypeName already check importedName for
    // the TypeRef when astNode is null (they inspect the TypeRef via its own
    // name-detection logic), so the same predicates work for imported fields once
    // the TypeRef is obtained from the correct source.
    const sema::TypeRef* ownedCollectionFieldType(const sema::FieldSymbol* fs) {
        if (!hasOwnedModifier(fs)) return nullptr;
        const sema::TypeRef* rt = nullptr;
        // #662 foundation B: imported field — type from importedMemberType.
        if (!fs->decl) {
            rt = ctx_.importedMemberType(fs);
        } else {
            auto* fd = dynamic_cast<const ast::FieldDecl*>(fs->decl);
            if (!fd || !fd->type) return nullptr;
            rt = ctx_.resolvedType(*fd->type);
        }
        if (isCollectionTypeName(rt)) return rt;
        // per §4.6/§4.7: owned LinkedList<T>/OwnedLinkedList<T> fields are also
        // heap collections — freed via bz_llist_free, not bz_array_free; but
        // they are identified here so emitOwnedFieldDisposals handles them.
        if (isLinkedListTypeName(rt)) return rt;
        return nullptr;
    }

    // #429 / §4.6 / §28.9: is storage member `m` (a class FieldSymbol, OR a body
    // auto-property PropertySymbol) one whose static type is `string` (or
    // `string?`)? Strings are runtime-managed (§4.6): a class field / auto-
    // property holding a string owns an INDEPENDENT +1 (retained at store /
    // transferred from a producer, released on the owner's disposal and on
    // reassignment). Strings cannot be marked `owned` (§4.6), so
    // isOwnedReferenceField never matches them — this is a separate predicate for
    // the disposal-release path. recordMemberType reads the type off a positional
    // Param, a body FieldDecl, OR a PropertyDecl. Nullable string? is included (a
    // plain `ptr` whose null is null; bz_string_release is null-safe).
    bool isStringStorageMember(const sema::Symbol* m) {
        const sema::TypeRef* rt = recordMemberType(m);
        return rt && rt->kind == sema::TypeRef::Kind::Primitive &&
               rt->primitive == sema::PrimitiveType::String;
    }

    // #429 (closure FIELD-STORE / disposal): true when member `m` is a class
    // instance field / auto-property whose declared type is a function type
    // (Kind::Function). Such a field holds an independent +1 on a closure cell,
    // retained at store and released on the owner's disposal — parallel to
    // isStringStorageMember for strings. Nullable function types (`(() -> T)?`)
    // resolve as a function TypeRef with nullable=true; bz_closure_release is
    // null-safe, so nullable closure fields are included (same as
    // isStringStorageMember includes nullable string?).
    bool isClosureStorageMember(const sema::Symbol* m) {
        const sema::TypeRef* rt = recordMemberType(m);
        return rt && rt->kind == sema::TypeRef::Kind::Function;
    }

    // #429: is the resolved TypeRef `rt` a (non-nullable) `string`? Used to
    // zero-init a no-initializer `var s: string;` slot so reassignment's
    // release-old is null-safe. Nullable string? is excluded (left untouched).
    bool isStringTypeRefNonNull(const sema::TypeRef* rt) {
        return rt && !rt->nullable &&
               rt->kind == sema::TypeRef::Kind::Primitive &&
               rt->primitive == sema::PrimitiveType::String;
    }

    // ====================================================================
    // Suspend/resume state-machine scaffolding (§29 iterators / §30 async).
    //
    // Phase 2a: this scaffolding is the machinery shared by BOTH suspending
    // constructs in the language — `yield` (§29.3 iterator methods) and
    // `await` (§30 async methods). Both lower a method body into a state
    // machine that suspends at a *suspend point*, spilling the locals that
    // are live across the suspension into a heap-allocated frame, and resumes
    // by restoring them and branching on a saved state index. The pieces that
    // differ per construct (what a suspend point *does* — produce a value and
    // return `true` for `yield`, vs. register a continuation on a task and
    // return for `await`; and the surrounding driver shape — an
    // IEnumerator<T> with MoveNext/Current vs. a Task<T> with continuations)
    // are layered on top of the kind-agnostic scaffolding below.
    //
    // §30 (`Await`) is NOT yet emitted: `SuspendKind::Await` exists as the
    // seam where the async path plugs in (see the Phase 2b plan). The iterator
    // (`Yield`) path is byte-for-behavior identical to before this extraction.
    // ====================================================================

    // Which suspending construct the in-flight state machine lowers.
    //   Yield      — a synchronous iterator (§29.3): MoveNext/Current, suspend
    //                points are `yield return` (store current_val, ret true).
    //   Await      — a plain async method (§30.1): a Task driver, suspend points
    //                are `await` (register a continuation, return).
    //   AsyncYield — an ASYNC ITERATOR (§30.7): both at once. The MoveNextAsync
    //                resume drives the body until the next `yield return` (store
    //                current_val, complete the per-advance Task<bool> with true,
    //                return) and `await` suspends on a task between yields. The
    //                frame carries both the `current_val` field and the async
    //                task slots; the per-advance task is re-allocated each
    //                MoveNextAsync call (the frame persists across calls).
    enum class SuspendKind { Yield, Await, AsyncYield };

    // Layout of a suspend/resume frame (the heap struct a suspended method
    // spills into). Field order, kind-independent:
    //   [0] vtable        — driver dispatch table (IEnumerator<T> vtable for
    //                       `Yield`; the async path decides its own slot 0 use)
    //   [1] state         — i32 resume index (0 = entry, k = after k-th
    //                       suspend point, -1 = done)
    //   [2 .. ]           — captures: `this` (instance methods) then params
    //   [.. ]             — body locals live across a suspend point
    //   [last]            — `Yield` only: the current yielded value (`T`)
    struct StateMachineFrame {
        llvm::StructType* structTy = nullptr;
        // Captures (this + params) to spill at construction / restore on
        // resume. {nullptr, ptrTy()} is the `this` capture; others carry the
        // ParameterSymbol so the resume path can rebind a slot for it.
        std::vector<std::pair<sema::Symbol*, llvm::Type*>> captures;
        // Body locals (LocalDecl / ForStmt-loop-var nodes) that survive a
        // suspend point, with their lowered types.
        std::vector<std::pair<const ast::Node*, llvm::Type*>> bodyLocals;
        unsigned vtableField = 0;
        unsigned stateField = 1;
        unsigned captureStart = 2;
        unsigned localStart = 0;   // captureStart + captures.size()
        // `Yield` only: index of the `current_val` field (unused for `Await`).
        unsigned currentValField = 0;
        // Maps a body-local AST node → its frame field index.
        std::unordered_map<const ast::Node*, unsigned> localFields;
    };

    // Pre-scan a method body to collect all LocalDecl/ForStmt nodes and their
    // types — the locals that must survive across a suspend point (a `yield`
    // or, for async, an `await`). Returns {node, llvmType} pairs. Kind-
    // agnostic: the conservative spill set is "every local in the body."
    void collectSuspendLocals(
        const std::vector<std::unique_ptr<ast::Stmt>>& body,
        std::vector<std::pair<const ast::Node*, llvm::Type*>>& out);

    // Build the kind-agnostic frame layout for a state machine: compute the
    // captures (this + params), the suspend-live body locals, assemble the
    // LLVM struct type, and fill the AST-node→field-index map. `extraTrailing`
    // are kind-specific trailing fields appended after the locals (for `Yield`
    // this is the single `current_val` field of element type `T`); their first
    // field index is recorded in `frame.currentValField`. `structName` names
    // the emitted LLVM struct type.
    StateMachineFrame buildStateMachineFrame(
        sema::MethodSymbol* ms,
        const std::vector<std::unique_ptr<ast::Stmt>>& body,
        llvm::ArrayRef<llvm::Type*> extraTrailing,
        const std::string& structName);

    // On the construction path (e.g. iterator GetEnumerator), store each
    // capture (this + params) from the current function's bound slots into the
    // freshly-allocated frame `st`. Requires `bindParameters` to have run so
    // `paramSlots_`/`currentThis_` are populated. Kind-agnostic.
    void emitFrameCaptureStores(const StateMachineFrame& frame, llvm::Value* st);

    // On the resume path (e.g. iterator MoveNext entry), restore each capture
    // from the frame `self` back into fresh slots: `this` into `currentThis_`,
    // each param into a new alloca recorded in `paramSlots_`. Kind-agnostic.
    void emitFrameCaptureRestores(const StateMachineFrame& frame,
                                  llvm::Value* self);

    // Emit the resume dispatch: a `done` block (returns the kind's "exhausted"
    // value via `emitDone`), N+1 state blocks (entry + one per suspend point),
    // and a switch on the saved state index that targets them. Returns the
    // state blocks (index 0 is the entry block where body emission begins).
    // `numSuspends` is the count of suspend points in the body. Sets the
    // sm*_ members so the per-suspend-point spill/restore (lowerStmt) can run.
    // Kind-agnostic; `emitDone` supplies the kind-specific exhausted-return.
    std::vector<llvm::BasicBlock*> emitResumeDispatch(
        const StateMachineFrame& frame, llvm::Value* self,
        llvm::Value* stateGEP, unsigned numSuspends,
        const std::function<void()>& emitDone);

    // §29.3: Detect if a block of statements contains any yield return / yield break.
    static bool containsYield(const std::vector<std::unique_ptr<ast::Stmt>>& stmts);

    // §29.3: Collect all YieldStmt pointers in order from a statement list.
    static void collectYields(const std::vector<std::unique_ptr<ast::Stmt>>& stmts,
                               std::vector<const ast::YieldStmt*>& out);

    // §29.3: Emit an iterator method (one containing `yield return`). Layers
    // the yield-specific driver — IEnumerator<T> with MoveNext/Current/Dispose
    // and an IEnumerator<T> vtable — on top of the kind-agnostic suspend/resume
    // scaffolding above. Creates:
    //   - State frame: { ptr vtable, i32 state, param0..N, locals..., T current_val }
    //   - MoveNext(ptr self): bool  — state machine body (resume dispatch)
    //   - Current getter(ptr self): T
    //   - Dispose(ptr self): void   — no-op
    //   - Module-level IEnumerator<T> vtable
    //   - The original function becomes GetEnumerator: allocs + fills frame, returns it
    void emitIteratorMethod(sema::MethodSymbol* ms, llvm::Function* fn,
                             const CallableInfo& ci);

    // §30.1: a method carrying the `async` modifier (mirrors isStaticMethod).
    static bool isAsyncMethod(sema::MethodSymbol* ms) {
        for (auto mod : ms->modifiers)
            if (mod == "async") return true;
        return false;
    }

    // §30.7 / #442: true when the method's DECLARED return type is
    // IAsyncEnumerable<T> or IAsyncEnumerator<T> — the two interfaces that
    // must be routed to emitAsyncIteratorMethod regardless of whether the body
    // actually contains a `yield`. Without this, an async method declared to
    // return IAsyncEnumerable<T> but whose body has NO yield (e.g. empty or
    // await-only) is misrouted to emitAsyncMethod (which emits a bz_task*
    // result), and an `await for` that loads the vtable from the returned
    // bz_task* pointer crashes (SIGSEGV).
    bool returnsAsyncIteratorType(sema::MethodSymbol* ms) const {
        if (!ms->decl) return false;
        const auto* md = dynamic_cast<const ast::MethodDecl*>(ms->decl);
        if (!md || !md->returnType) return false;
        const sema::TypeRef* rt = ctx_.resolvedType(*md->returnType);
        if (!rt || rt->kind != sema::TypeRef::Kind::Named) return false;
        // Path A: no declaration — name is in astNode->name->parts.
        if (!rt->declaration) {
            if (rt->astNode && rt->astNode->name &&
                !rt->astNode->name->parts.empty()) {
                auto& n = rt->astNode->name->parts[0];
                return n == "IAsyncEnumerable" || n == "IAsyncEnumerator";
            }
            return false;
        }
        // Path B: builtinClassSym (non-null declaration with no user decl).
        if (!rt->declaration->decl) {
            return rt->declaration->name == "IAsyncEnumerable" ||
                   rt->declaration->name == "IAsyncEnumerator";
        }
        return false;
    }

    // §30.2: detect whether a block of statements contains an `await`. Used to
    // decide if an async method needs a real suspending state machine, or can
    // complete synchronously (no await → run-to-completion fast emit). Recurses
    // into compound statements and into expressions (await is an expression).
    static bool containsAwait(const std::vector<std::unique_ptr<ast::Stmt>>& stmts);
    static bool exprContainsAwait(const ast::Expr* e);

    // §30.9 / §31.2 (#438): detect an `await` in a try POSITION that is STILL
    // deferred — i.e. inside a catch arm body, a catch guard, or a `finally`
    // body. `await` in a try BODY is now supported (the resume block is lowered
    // inside the try's protected region, so a faulted await re-raises into the
    // enclosing catch and the finally runs on every path — see emitAsyncMethod /
    // lowerAwait). Resuming into a catch handler (re-establishing the caught-
    // exception disposal scope across a suspend) or into a finally (the exit-mode
    // re-dispatch across a suspend) is the next increment, so those positions are
    // rejected with a clear diagnostic + safe stub. Recurses into nested loops /
    // ifs / do-blocks / try-bodies so a deferred position nested in them is still
    // caught.
    static bool awaitInDeferredTryPosition(
        const std::vector<std::unique_ptr<ast::Stmt>>& stmts);

    // §30.7 (#440): detect a suspend point (yield / await) inside ANY try region
    // (body, catch arm, or finally) in an async iterator body. Resuming the fused
    // yield+await state machine into a protected scope across a suspend is
    // unimplemented; such a shape is rejected with a diagnostic + safe stub.
    // Stricter than awaitInDeferredTryPosition (which permits `await` in a try
    // body): an iterator's `yield` in a try body is itself a cross-region suspend.
    bool suspendInTryPosition(
        const std::vector<std::unique_ptr<ast::Stmt>>& stmts);

    // §30.7 (#440): does a single statement (e.g. a catch-arm body) contain a
    // suspend point (yield / await)? Helper for suspendInTryPosition.
    static bool stmtContainsSuspend(const ast::Stmt* s);

    // §29.6 / #554: does a single statement (e.g. a catch-arm body or a finally
    // body) contain a `yield`? Used by suspendInTryPosition to enforce the
    // §29.6 rule that yield in a catch arm body or finally is forbidden (but
    // `await` in those positions is allowed per §30.9).
    static bool stmtContainsYield(const ast::Stmt* s);

    // §30.9 / §31.2 (#438): does a single statement (a catch-arm body) contain an
    // `await`? Distinct from stmtContainsSuspend (which also reports `yield`):
    // lowerTry uses this to decide whether to frame-back the handler-context
    // allocas of an async catch arm (the binding / caught-exc disposal slots).
    static bool stmtContainsAwait(const ast::Stmt* s);

    // §30.1 / §30.2: emit an `async` method (one returning Task / Task<T> /
    // OwnedTask<T>; the async-iterator forms — async + yield — are deferred).
    // Mirrors emitIteratorMethod with smKind_ == Await. Layers the task-driver
    // shape on the kind-agnostic suspend/resume scaffolding:
    //   - State frame: { ptr vtable, i32 state, captures..., locals...,
    //                    ptr outerTask, ptr pendingTask }
    //   - The original function becomes the ENTRY: bz_task_alloc the outer task,
    //     calloc the frame, store captures + outer task, invoke Resume once
    //     (the already-done fast path lets a sync-completable body finish
    //     inline), and return the outer task pointer.
    //   - A Resume(void* completedTask, void* frame) continuation (matching the
    //     bz_task_then ABI) restores captures and runs the resume dispatch; each
    //     `await` suspend point registers Resume on the awaited task and returns;
    //     `return e;` completes the outer task (bz_task_complete) and returns.
    void emitAsyncMethod(sema::MethodSymbol* ms, llvm::Function* fn,
                         const CallableInfo& ci);

    // §30.6 (#488): emit the async state machine for an `async` LAMBDA. Called
    // from lowerLambda after the closure entry function `fn(env, params...)` is
    // created and its prologue has bound the lambda params (from args) and the
    // captured outer cells / `this` (from env) into paramSlots_/locals_/
    // cellVars_/currentThis_. This takes over body emission: `fn` becomes the
    // async ENTRY (alloc the outer Task, calloc the frame, spill the bound
    // params + captures into it, drive a Resume continuation once, return the
    // Task pointer); a sibling Resume(ptr completedTask, ptr frame) holds the
    // suspend/resume body — reusing the kind-agnostic scaffolding (await
    // suspend, ReturnStmt completion, fault boundary) exactly as emitAsyncMethod
    // does. `resultElemSemaTy` is T in the lambda's `Task<T>`/`OwnedTask<T>`
    // return type (null for a no-value `Task`), taken from the call-site
    // expected function type. `paramSyms` are the lambda's user parameter
    // symbols (already bound from the entry fn's args 1..N). On return `fn` is
    // fully terminated; the builder is left at the resume function's end and
    // lowerLambda restores its saved insert point. Leaves the sm*_ async state
    // cleared.
    void emitAsyncLambda(const ast::LambdaExpr& lam, llvm::Function* fn,
                         const std::vector<sema::Symbol*>* paramSyms,
                         const sema::TypeRef* resultElemSemaTy);

    // §30.1 / §30.9: complete the in-flight async method's outer task with the
    // given lowered return value (null for `Task` / fall-off / `return;`). Reads
    // the outer-task pointer from the frame and calls bz_task_complete. Requires
    // the sm*_ async state to be live (smKind_ == Await).
    void emitAsyncComplete(llvm::Value* value);

    // §30.13 (#439-followup): free the heap spill area pointed to from the
    // frame's spill-ptr field (asyncSpillPtrField_), if present. Emitted on each
    // terminal path that frees the frame (emitAsyncComplete + async.fault). The
    // field is null when no cross-suspend temporary was promoted, and free(null)
    // is a no-op, so this is safe in every body shape. Must run before free(self).
    void emitFreeSpillArea(llvm::Value* self);

    // §30.2 deferred tail: emit a well-formed but inert async-method entry for a
    // body whose codegen is deferred (await-in-loop), after the diagnostic has
    // failed the compile. Keeps emitted IR valid without lowering the body.
    void emitAsyncStub(sema::MethodSymbol* ms, llvm::Function* fn,
                       const CallableInfo& ci);

    // §30.2: lower an `await <task>` suspend point (smKind_ == Await). Emits the
    // already-done fast path inline and, on the slow path, spills suspend-locals,
    // registers the Resume continuation on the awaited task, returns from the
    // resume fn, and resumes at the next state block. Returns the awaited
    // result value (an int/value/string for Task<T>/OwnedTask<T>; a null
    // placeholder for `await Task` which yields unit). Returns nullptr if the
    // enclosing function is not an async state machine (so the await falls
    // through to the normal nullptr — a defensive guard, not a normal path).
    llvm::Value* lowerAwait(const ast::UnaryExpr& un);

    // §30.2 / §30.7: the reusable await suspend mechanism, shared by lowerAwait
    // and the `await for` desugaring (lowerAwaitFor). `task` is the awaited task
    // pointer; `resultTy` the lowered await-result type (null for a no-value
    // `await Task`); `freeTaskAfter` reclaims the task once its result is read.
    // Emits the already-done fast path inline, the slow path (register the resume
    // continuation + return), and the resume / fast-path convergence that reads
    // the result and re-raises a faulted task (§30.9). Returns the result value.
    llvm::Value* emitAwaitSuspend(llvm::Value* task, llvm::Type* resultTy,
                                  bool freeTaskAfter);

    // §28.12 / §30 (`await using` normal-exit path): call DisposeAsync() on `obj`
    // via `disposeAsyncFn` (the direct LLVM function pointer resolved at
    // registration time), await the returned Task (a proper suspend point), then
    // free the block (physical reclamation). Used by emitDisposals on the non-
    // unwind path when asyncDispose is set. When `disposeAsyncFn` is null (sync
    // fallback: class not statically known, or not in async SM context), falls back
    // to emitDisposeAndReclaim (synchronous Dispose + free).
    void emitAsyncDisposeAwaitAndFree(llvm::Value* obj,
                                      llvm::Function* disposeAsyncFn);

    // §30.7: emit an ASYNC ITERATOR method (declared `async`, contains `yield`,
    // returns IAsyncEnumerable<T> / IAsyncEnumerator<T>). Combines the iterator
    // driver (MoveNextAsync/Current/Dispose vtable, current_val field) with the
    // async state machine (Task-completing suspend points, await continuations).
    // smKind_ == AsyncYield. The original method becomes the construction entry
    // (calloc the frame, store captures, return it as the enumerator). MoveNextAsync
    // allocates a fresh per-advance Task<bool>, drives the Resume once, and returns
    // it; Resume runs the body — `yield return v` stores current_val and completes
    // the per-advance task with true; `await t` suspends on t (emitAwaitSuspend);
    // exhaustion / `yield break` completes with false. Current reads current_val.
    void emitAsyncIteratorMethod(sema::MethodSymbol* ms, llvm::Function* fn,
                                 const CallableInfo& ci);

    // §30.7: complete the in-flight async iterator's PER-ADVANCE Task<bool> with
    // `hasValue` (true = a value was yielded into current_val; false = exhausted).
    // Unlike emitAsyncComplete, it does NOT free the frame (the frame persists
    // across MoveNextAsync calls until Dispose). Requires AsyncYield sm*_ state.
    void emitAsyncIterComplete(bool hasValue);

    // §30.7: lower `await for var x in src do ... end`. Drives an IAsyncEnumerator
    // (the async-iterator frame returned by a CallExpr source): each iteration
    // calls MoveNextAsync (vtable slot 2) → Task<bool>, awaits it (a suspend point
    // in the consuming async method's own state machine — emitAwaitSuspend), reads
    // Current (vtable slot 3) on a true result and runs the body, and disposes the
    // enumerator (vtable slot 1) at loop exit. Returns true when it handled the
    // statement. Only the borrowing IAsyncEnumerable<T> / CallExpr-source shape is
    // supported; other shapes fall through to a diagnostic.
    bool lowerAwaitFor(const ast::ForStmt& fs);

    // §30.2 / §30.13 (#438 + #439-followup): repair SSA dominance in a resume
    // function after the body is lowered, moving the offending temporaries into
    // the HEAP FRAME so they survive a real cross-thread suspend. An `await`
    // inside a larger expression (e.g. `total + await f()`, including the
    // loop-carried `total := total + await f(i)` form) leaves an SSA temporary
    // computed BEFORE the suspend but used in the resume block — which the
    // dispatch switch (and a loop back-edge) re-enters, so the temporary's def no
    // longer dominates its use ("instruction does not dominate all uses"). Named
    // locals already round-trip through the frame (smAllocaToField_); this
    // catches the remaining anonymous temporaries by demoting every register
    // whose def fails to dominate a use (llvm::DemoteRegToStack /
    // DemotePHIToStack) and then PROMOTING each demoted slot into the heap frame
    // (promoteDemotedSlotsToFrame). A plain stack slot would be garbage after a
    // real cross-thread resume (the worker has a fresh stack); the heap spill
    // area, like named locals, is valid in BOTH the synchronous / already-done
    // cut and the cross-thread case. Runs only on async resume functions;
    // iterators (yield) never produce a cross-suspend temporary (the yielded
    // expression is fully consumed before the suspend), so this is async-specific.
    void fixupSuspendDominance(llvm::Function* fn);

    // §30.2 / §30.13 (#439-followup): move the demoted cross-suspend stack slots
    // (the allocas fixupSuspendDominance created) into a heap spill area pointed
    // to from the frame (asyncSpillPtrField_), so the pre-suspend temporaries
    // survive a REAL cross-thread resume — the worker runs on a fresh stack, so a
    // function-local slot would be garbage. Lays out a spill struct, lazily
    // callocs it on the first resume entry, and replaces each demoted alloca with
    // a GEP into that heap region. Async-only (called from fixupSuspendDominance).
    void promoteDemotedSlotsToFrame(
        llvm::Function* fn, const std::vector<llvm::AllocaInst*>& demoted);

    // ---- Live suspend/resume state for the in-flight MoveNext / resume fn ----
    // Kind-agnostic frame state. The per-suspend-point spill/restore
    // (lowerStmt's YieldStmt handler, and — for 2b — the await lowering) reads
    // these to spill `smAllocaToField_` slots into `smStateStruct_` before a
    // suspension and restore them at the next state block. (Historically named
    // iter*_; renamed to sm*_ in Phase 2a as the scaffolding is now shared.)
    SuspendKind smKind_ = SuspendKind::Yield;
    llvm::StructType* smStateStruct_ = nullptr;
    llvm::Value* smSelf_ = nullptr;
    llvm::Value* smStateGEP_ = nullptr;
    std::vector<llvm::BasicBlock*> smStateBBs_;
    llvm::BasicBlock* smDoneBB_ = nullptr;
    unsigned smNextSuspendState_ = 0;
    // Maps LocalDecl/ForStmt AST node → frame field index for suspend-locals.
    std::unordered_map<const ast::Node*, unsigned> smLocalFields_;
    // Maps alloca slot → frame field index (spill at suspend, restore at state entry).
    std::unordered_map<llvm::AllocaInst*, unsigned> smAllocaToField_;
    // ---- Yield-specific live state (used only when smKind_ == Yield) ----
    unsigned iterCurrentValField_ = 0;
    llvm::Type* iterElemTy_ = nullptr;

    // ---- Async-specific live state (used only when smKind_ == Await) ----
    // Frame field indices for the two trailing async slots (see emitAsyncMethod):
    //   asyncOuterTaskField_   — the Task this method returns / completes.
    //   asyncPendingTaskField_ — the task currently being awaited (set at the
    //                            suspend point so the resume can read its result).
    unsigned asyncOuterTaskField_ = 0;
    unsigned asyncPendingTaskField_ = 0;
    // §30.2 / §30.13 (#439-followup): frame field holding a pointer to the heap
    // spill area for anonymous cross-suspend temporaries (the pre-suspend
    // operands of awaits embedded in larger expressions). Lazily calloc'd on the
    // first resume entry by fixupSuspendDominance; null when the body has none.
    unsigned asyncSpillPtrField_ = 0;
    // §30.9 / §31.2 (#438): handler-context allocas that must survive a
    // cross-thread suspend when an `await` lives inside a catch arm or a
    // finally. lowerTry registers, in an async state machine, the catch-arm
    // exception binding slot, the synthetic caught-exception disposal slot, and
    // the finally exit-mode / return-value slots here. These are stored BEFORE
    // the in-handler await and read AFTER the resume; unlike named locals they
    // are not in smAllocaToField_ (the await spill/restore loop), so they would
    // hold a fresh-stack garbage value on a worker resume. fixupSuspendDominance
    // FORCE-promotes them into the heap spill area (alongside the dominance-
    // demoted register temporaries), repointing the alloca to a heap GEP so the
    // single store-before-suspend / read-after-resume round-trips the frame.
    // Only the slots whose try region actually contains an in-handler await are
    // added (a try whose catch/finally has no await never registers any).
    std::vector<llvm::AllocaInst*> asyncHandlerSpillSlots_;
    // The async resume function (the bz_task_then continuation). The await
    // suspend point registers this on the awaited task.
    llvm::Function* asyncResumeFn_ = nullptr;
    // The async method's result element type T (Task<T>/OwnedTask<T>), or null
    // for a `Task` (no value). Used to size bz_task_complete's result buffer and
    // to type the value read back at the await site.
    llvm::Type* asyncResultTy_ = nullptr;
    // The sema TypeRef for T in Task<T>, mirroring asyncResultTy_. Kept
    // alongside it so emitAsyncComplete can detect a nullable value type (int?,
    // double?, enum?) and wrap a plain returned value into the {hasValue=1,
    // value} struct before storing — per §30.1 / §7.2 (#493).
    const sema::TypeRef* asyncResultSemaType_ = nullptr;

    // Pass 2: emit a function's body.
    void emitFunctionBody(sema::MethodSymbol* ms, llvm::Function* fn);

    // CG10: emit a constructor body. Always an instance function (leading
    // `this`); its params bind to the ConstructorSymbol's ParameterSymbols.
    // Field initializers run in `lowerNew` before the call, so the body
    // only runs the explicit statements. Returns void.
    // CG47 / fix #582 / §22.13: emit a static initialisation unit.
    //   cs   — user-written ConstructorSymbol, or null for a synthesized
    //          field-init-only function.
    //   type — the owning TypeSymbol; used to find static fields (always set).
    //   fn   — the void() LLVM function to populate.
    // Emits, in order:
    //   1. Runtime stores for every non-constant static field initializer of
    //      `type` (in textual declaration order, per §22.13).
    //   2. The user-written static constructor body (only when cs != null).
    void emitStaticConstructorBody(sema::ConstructorSymbol* cs,
                                   const sema::TypeSymbol* type,
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

    // §12 / §14 (issue #585): inline a constructor's body INTO the current
    // function with the active generic substitution, having already bound
    // `this` (= obj) and its parameters in paramSlots_. Runs the leading
    // `base(...)`/`this(...)` chain (or the implicit base) first, then the
    // remaining body. Used both for the most-derived ctor at a generic `new`
    // site and, recursively, for a generic base ctor reached via `base(...)`
    // — the base ctor must be inlined (not called as the T→i32 template
    // function) so its field stores use the concrete substituted layout and
    // a non-int `T` argument matches the field type. Caller saves/restores
    // currentThis_, locals_, paramSlots_, and the unwind state.
    void inlineCtorBody(sema::ConstructorSymbol* cs, llvm::Value* obj);

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

    // #429 (RETURN-escape category) / §4.6 / §28.9: is `init` a CALL to a
    // USER-DEFINED, LOCALLY-COMPILED Bezel method whose declared return type is
    // (non-nullable) `string`? Such a call hands back EXACTLY +1: the callee's
    // ReturnStmt lowering mints a +1 for the returned string (transfer a +1
    // producer, else retain a borrow), so the caller owns that single reference
    // and may treat the result like any other +1 producer (transfer into a local
    // slot, release at scope end if non-escaping). Excludes builtin stdlib string
    // methods (no resolved MethodSymbol — matched structurally in
    // tryLowerStringMethod) and CROSS-MODULE IMPORTED methods (their callee does
    // NOT go through this module's ReturnStmt minting), which keep their prior
    // borrow-with-leak handling. Operators / constructors are excluded too.
    bool isUserMethodStringReturnCall(const ast::Expr* init) const;

    // #429 (RETURN-escape category) / §4.6 / §28.9: is `e` an IDENTIFIER naming a
    // non-nullable string LOCAL that already OWNS a +1 reference in its slot?
    // True for a producer-initialized local (the producer's +1 was transferred
    // into the slot) or a borrow local that was retained at bind
    // (stringBorrowAll_). When such a local is RETURNED, its single +1 must be
    // TRANSFERRED to the caller (no extra retain) — the local is escaped (so not
    // released at scope end) and excluded from disposal, so its +1 flows out
    // rather than leaking. A param read, a field read, a literal, or an alias
    // that does not own a +1 is NOT covered (those are pure borrows → the return
    // mint RETAINS them). Only meaningful while the current function is the one
    // collectStringReleaseEligible analyzed (stringReleaseEligibleFn_).
    bool returnedStringLocalOwnsPlusOne(const ast::Expr* e) const;

    // #429 / §4.6 / §28.9: store `newVal` (a string) into the CLASS string
    // field/auto-property at `fieldAddr` with retain-new / release-old, giving
    // the field an INDEPENDENT +1 (released on the owner's disposal). Returns
    // true (caller must NOT also store); false when `target` is not a class
    // string field lvalue (caller stores normally). `valueExpr` selects
    // transfer-vs-retain: a +1 producer is transferred (no retain), anything
    // else is retained. retain-before-release keeps a self-assignment safe.
    bool lowerStringFieldStore(const ast::Expr& target, llvm::Value* fieldAddr,
                               llvm::Value* newVal, const ast::Expr* valueExpr);
    // #429: is `target` a string-typed CLASS instance field / auto-property
    // lvalue (NOT a local / param / ref-param / array element / static field /
    // record field)? Only such a location owns an independent +1 released on the
    // owner's disposal — see lowerStringFieldStore / emitOwnedFieldDisposals.
    bool isStringFieldLvalue(const ast::Expr& target);
    // #429 / §4.6 / §28.9: REASSIGNMENT `x := newVal` where `x` is a string-typed
    // LOCAL/parameter SLOT. Retain-new (transfer a producer) / store / release-
    // old, so the slot keeps owning an independent +1 across the reassignment and
    // a later field disposal / overwrite that releases the same string never
    // dangles this slot (no UAF). Returns true when handled (caller must NOT also
    // store); false when `target` is not a string local slot. `slotAddr`/`elemTy`
    // are the lvalue address/type the caller already computed.
    bool lowerStringLocalReassign(const ast::Expr& target, llvm::Value* slotAddr,
                                  llvm::Type* elemTy, llvm::Value* newVal,
                                  const ast::Expr* valueExpr);
    // #429 / §4.6 / §28.9: the set of `var` string LocalDecl nodes whose
    // initializer is a BORROW (NOT a +1 producer) and which the escape walk
    // proved non-escaping — these are RETAINED at bind and RELEASED at scope end
    // (the local owns an independent +1, so a later field disposal / overwrite
    // that releases the same string does not dangle the local — no UAF).
    // Disjoint from the producer-transfer set (stringReleaseEligible_): a
    // producer-bound local transfers and releases without a retain.
    std::unordered_set<const void*> stringBorrowRetainEligible_;
    // #429: ALL borrow-initialized `var` string LocalDecl nodes (escaping AND
    // non-escaping). Every one is RETAINED at bind so the slot holds an
    // independent +1 — even an escaping borrow local must hold its own reference
    // so the returned/stored value is not freed out from under it by a field
    // disposal / overwrite (no UAF). The NON-escaping subset
    // (stringBorrowRetainEligible_) is additionally released at scope end; the
    // escaping ones leak their retained +1 (safe).
    std::unordered_set<const void*> stringBorrowAll_;
    // #429: emit retain-or-transfer for a `var`-string local at bind time, and
    // arm its scope-end release. Returns true if it armed/handled the local's
    // string reference (the caller still performs the slot store). `init` is the
    // initializer expr (null when absent). Centralizes the producer-transfer vs
    // borrow-retain decision and the disposeStack registration so lowerLocalDecl
    // stays readable.
    void armStringLocalRefcount(const void* declNode, llvm::AllocaInst* slot,
                                llvm::Value* v, const ast::Expr* init);

    // #429 (closure RETURN category) / §28.9 / §28.3: true when `init` is a
    // call to a user-defined, locally-compiled method whose declared return type
    // is a function type (Kind::Function). Such a call goes through THIS
    // module's ReturnStmt lowering, which mints exactly +1 for the returned
    // closure cell — so the caller owns that single reference (a +1 producer
    // at the call site). Cross-module imported methods are excluded (no local
    // mint). Parallel to isUserMethodStringReturnCall for strings.
    bool isUserMethodClosureReturnCall(const ast::Expr* init) const;
    // #429 (closure RETURN category) / §28.9 / §28.3: true when the returned
    // expression is an identifier naming a closure-typed local whose slot
    // already owns a +1 — either initialized by a +1 closure producer (lambda /
    // method-ref) or explicitly tracked as non-escaping and release-eligible
    // (closureReleaseEligible_). Such a returned local TRANSFERS its single +1
    // to the caller (no second retain). Parallel to returnedStringLocalOwnsPlusOne.
    bool returnedClosureLocalOwnsPlusOne(const ast::Expr* e) const;
    // #429 (closure RETURN + FIELD-STORE) / §28.9: is `target` a closure-typed
    // CLASS instance field / auto-property lvalue (NOT a local / param /
    // ref-param / array element / static field / record field)? Only such a
    // location owns an independent +1 released on the owner's disposal.
    bool isClosureFieldLvalue(const ast::Expr& target);
    // #429 (closure FIELD-STORE) / §28.9: store `newVal` into the class closure
    // field at `fieldAddr` with retain-new / release-old, giving the field an
    // INDEPENDENT +1. Order: load-old → retain-or-transfer-new → store-new →
    // release-old. A +1 producer (lambda / method-ref / user-method closure-
    // returning call) is TRANSFERRED; anything else is RETAINED. Parallel to
    // lowerStringFieldStore. Returns true when handled.
    bool lowerClosureFieldStore(const ast::Expr& target, llvm::Value* fieldAddr,
                                llvm::Value* newVal, const ast::Expr* valueExpr);

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
    // box producer: boxForTarget is a no-op and no wrapper exists.
    // Also true when the initializer is a call to a method in boxReturnMethods_
    // (a locally-compiled method whose body always returns boxable value sources —
    // see isUserMethodBoxReturnCall): such a call's return value was produced by
    // boxForTarget at the callee's return site and is a fresh rc=1 box. Used to
    // gate release eligibility. (Not const: it consults the boxable-value
    // predicates, which are non-const Emitter members.)
    bool isPlusOneBoxProducer(const ast::LocalDecl* ld);

    // #429 (box RETURN category) / §8.6 / §28.9: true when `init` is a call
    // to a USER-DEFINED, LOCALLY-COMPILED Bezel method whose declared return
    // type is Object or an interface AND whose body provably always returns
    // boxable value sources (i.e. all return statements return a boxable value
    // type, so boxForTarget at the callee's return site always creates a fresh
    // rc=1 box — see collectBoxReturnMethods). Such a call hands back EXACTLY
    // +1: the fresh box the callee created. The caller treats the result as a
    // +1 producer and may release it at scope end if non-escaping. Excludes
    // cross-module imported methods (their callee does NOT run THIS module's
    // boxForTarget) and methods that sometimes return class references.
    // Parallel to isUserMethodClosureReturnCall for closures.
    bool isUserMethodBoxReturnCall(const ast::Expr* init) const;

    // #429 (box RETURN + FIELD-STORE) / §8.6 / §28.9: true when `target` is
    // an Object- or interface-typed CLASS instance field / auto-property lvalue
    // (NOT a local / param / ref-param / array element / static field / record
    // field) AND the expression at `valueExpr` has a boxable value source type
    // (so boxForTarget will/has allocated a fresh box). Only such a combination
    // warrants an independent +1 on the field. Guards: a class reference
    // assigned to an Object field (reference source) returns false → no
    // bz_box_retain/release on a class instance.
    bool isBoxFieldLvalue(const ast::Expr& target, const ast::Expr* valueExpr);
    // #429 (box FIELD-STORE) / §8.6 / §28.9: store `newVal` (a box pointer
    // after boxForTarget) into the class Object/interface field at `fieldAddr`
    // with retain-new / release-old, giving the field an INDEPENDENT +1.
    // Order: load-old → retain-or-transfer-new → store-new → release-old.
    // A +1 producer (a freshly-boxed value or a box-return method call) is
    // TRANSFERRED (no extra retain); anything else (borrow — a param/field/
    // identifier) is RETAINED. retain-before-release keeps a self-overwrite
    // safe. Adds the field's symbol to boxStorageFields_ so the disposer
    // knows to release it. Returns true when handled; false if not a box field
    // (caller stores normally). Parallel to lowerClosureFieldStore for closures.
    bool lowerBoxFieldStore(const ast::Expr& target, llvm::Value* fieldAddr,
                            llvm::Value* newVal, const ast::Expr* valueExpr);

    // #429 (box FIELD-STORE / disposal) / §8.6 / §28.9: the per-module set of
    // class field symbols (FieldSymbol / auto-property PropertySymbol) for which
    // EVERY store (across all methods + constructors + field initializers of the
    // class) supplies a boxable value source type. These fields provably always
    // hold a box (never a class reference), so emitDisposerCore can safely call
    // bz_box_release on them at disposal. Populated incrementally by
    // lowerBoxFieldStore and emitInstanceInit (via isBoxFieldLvalue); fields
    // that receive a class-reference store are never added (they miss the
    // isBoxFieldLvalue gate) so the class-instance-in-Object hazard is avoided.
    // NOTE: this is further protected by collectBoxStorageFields, a pre-pass
    // that explicitly REMOVES any field from this set that receives a non-box
    // store, making the safety guarantee hold across compilation order.
    std::unordered_set<const void*> boxStorageFields_;

    // #429 (box RETURN + FIELD-STORE) / §8.6 / §28.9: is `m` in
    // boxStorageFields_? True iff this field was determined (via the
    // collectBoxStorageFields pre-pass + per-store bookkeeping) to hold ONLY
    // boxes — every store was a boxable value source. Used by emitDisposerCore
    // to gate bz_box_release at disposal. Parallel to isClosureStorageMember.
    bool isBoxStorageMember(const sema::Symbol* m) {
        return boxStorageFields_.count(static_cast<const void*>(m)) != 0;
    }

    // #429 (box RETURN category) / §8.6 / §28.9: the set of user-defined,
    // locally-compiled MethodSymbols whose declared return type is Object or
    // an interface AND whose body provably always returns boxable value source
    // types (so boxForTarget at the return site always creates a fresh rc=1
    // box). Populated by collectBoxReturnMethods() before any method bodies
    // are lowered. A call to such a method is recognized as a +1 box producer
    // by isUserMethodBoxReturnCall / isPlusOneBoxProducer.
    std::unordered_set<const sema::MethodSymbol*> boxReturnMethods_;
    // Scan all MethodDecl bodies to populate boxReturnMethods_. A method is a
    // "box return method" if its declared return type is Object/interface,
    // it is locally compiled (not imported), and every ReturnStmt in its body
    // carries a value whose static type is boxable (isBoxableValueType or
    // isNullableValueTy). An arrow-body method is a box return method iff its
    // expression body is of boxable type. Called once from run() before the
    // method-body emission loop.
    void collectBoxReturnMethods();
    // Scan all method / constructor bodies AND field initializers for stores
    // to Object/interface-typed CLASS instance fields. A field is added to
    // boxStorageFields_ ONLY if EVERY store to it (across all methods and
    // constructors of the class) carries a boxable value source type — i.e.
    // every store goes through boxForTarget. A field that receives a class-
    // reference store is never added (class references have reference source
    // types, failing the isBoxableValueType / isNullableValueTy check). This
    // pre-pass ensures disposal is sound: bz_box_release fires only for fields
    // we know can never hold a class instance. Called once from run() before
    // the method-body emission loop.
    void collectBoxStorageFields();

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

    // #465 / §15.4 / stdlib §4.4–§4.5: materialize a sequence collection literal
    // `[e1, e2, …]` into a freshly-allocated heap collection (Array<T> /
    // OwnedArray<T> / Stack / Queue and their owning variants), reusing the exact
    // `new Array<T>()` allocation (bz_array_alloc) + `.Add(...)` element-store
    // (bz_array_add) codegen. `collTy` is the resolved heap-collection target
    // TypeRef (carrying the element type in genericArgs[0]). Returns a `ptr` to
    // the bz_array_t, matching the variable's lowered type. Returns null when the
    // target is not an array-backed heap collection (caller falls back to the
    // inline-array path).
    llvm::Value* materializeHeapCollectionLiteral(
        const ast::CollectionLiteralExpr* cl, const sema::TypeRef* collTy);

    // #534 / stdlib §4.8: materialize a dictionary literal `["k1":v1, …]` into a
    // freshly-allocated Dictionary<K,V>, reusing the SAME allocation and .Add
    // codegen as the `new Dictionary<K,V>()` + `.Add(k, v)` paths. Returns a
    // `ptr` to the bz_dict_t (or null if `dictTy` is not a Dictionary family).
    llvm::Value* materializeHeapDictLiteral(
        const ast::CollectionLiteralExpr* cl, const sema::TypeRef* dictTy);

    // #559 / stdlib §4.10: materialize a Set<T>/OwnedSet<T> sequence literal
    // `[e1, e2, …]` into a freshly-allocated Set backed by bz_dict_alloc_generic,
    // reusing the SAME allocation and per-element insertion as the
    // `new Set<T>()` + `.Add(e)` paths (bz_dict_set_gen + sentinel value 1).
    // Duplicates are silently collapsed (dict stores the first; second call is
    // a no-op on an already-present key). Returns a `ptr` to the bz_dict_t
    // (or null if `setTy` is not a Set/OwnedSet TypeRef with a recognised
    // element type).
    llvm::Value* materializeHeapSetLiteral(
        const ast::CollectionLiteralExpr* cl, const sema::TypeRef* setTy);

    // per §4.6/§4.7: the resolved LinkedList<T>/OwnedLinkedList<T> TypeRef of
    // a local, or null if the local is not a linked list.
    const sema::TypeRef* linkedListTypeOfLocal(const ast::LocalDecl* ld);

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

    // issue #537 / §4.8: `for var v in <dict>.Values` — iterate a Dictionary's
    // values. The string-key dict uses bz_dict_values_str; the generic-key dict
    // uses bz_dict_values_gen. The loop variable binds to V (genericArgs[1]).
    // Returns false if the iterable is not a `<dict>.Values` access.
    bool lowerDictValuesFor(const ast::ForStmt& fs);

    // #548 / stdlib §4.8 v0.135: `for var e in dict` entry iteration over a
    // non-owning Dictionary<K,V>. The loop variable binds to a synthetic
    // DictionaryEntry<K,V> struct alloca {K key; V value} per live bucket, in
    // insertion order. e.Key/e.Value field reads are lowered via GEP on that
    // struct (field 0 = K, field 1 = V). Value is a borrow (not retained).
    // Returns false if the iterable is not a bare Dictionary<K,V> local.
    bool lowerDictEntryFor(const ast::ForStmt& fs);

    // #639 / stdlib §4.5/§4.7/§4.11 v0.135: `for var x in coll.Drain()`
    // ownership-transferring drain over OwnedArray<T>, OwnedLinkedList<T>,
    // OwnedStack<T>, OwnedQueue<T>.
    bool lowerOwnedSeqDrainFor(const ast::ForStmt& fs);

    // #628 / stdlib §4.9 v0.135: `for var entry in ownedDict.Drain()`
    // ownership-transferring drain iteration over OwnedDictionary<K,V>.
    bool lowerDictDrainFor(const ast::ForStmt& fs);

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
    //
    // `forLoopVar` controls the boxing strategy (BUG-L2 / §27.5 / §16.5):
    //   true  — for-var loop variable, needs a fresh cell per loop iteration
    //           so each captured closure sees its own iteration's binding;
    //           the incBB keeps the original slot current so each pass of the
    //           boxing code reads the correct value.
    //   false — regular local / param, the original slot goes stale after the
    //           first boxing (all writes go through the cell); a null-guarded
    //           single cell is used so the accumulated value is never reset
    //           by a re-initialisation on subsequent loop iterations.
    llvm::Value* boxLocalSlot(llvm::AllocaInst* slot, llvm::AllocaInst*& newSlot,
                              bool forLoopVar = false);

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
    // issue #597 (§14.1 / §22.4): `argSubst`/`argSemaTy` give the substitution
    // under which the constructor ARGUMENT expressions must be lowered — the
    // ENCLOSING scope's binding, which differs from the new record's own
    // substitution (`genericSubst_`, active for `recordStruct`/field layout)
    // when the record is built from an enclosing param in a non-identity
    // position (e.g. `new RBox<B>(_b)` inside `Pair<A,B>`). The field STORES
    // still run under the record's own substitution.
    llvm::Value* buildRecordValue(
        const sema::TypeSymbol* t,
        const std::vector<ast::CallArgument>& args,
        const std::unordered_map<std::string_view, llvm::Type*>& argSubst,
        const std::unordered_map<std::string_view, const sema::TypeRef*>&
            argSemaTy);

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
    // per §4.6/§4.7: true for LinkedList<T> / OwnedLinkedList<T> (bz_llist_t-backed).
    static bool isLinkedListTypeName(const sema::TypeRef* rt);
    // §6.1 / §30.8: extract the builtin type name from a Named TypeRef that has
    // no user TypeDecl.  Two shapes exist:
    //   a) !rt->declaration: the name lives in rt->astNode->name->parts[0].
    //   b) rt->declaration is a builtinClassSym (null decl): name is declaration->name.
    // Returns an empty string_view if the TypeRef is not a builtin named type.
    static std::string_view builtinTypeName(const sema::TypeRef* rt);

    // fix #568: is `name` a BUILTIN type name that lowerTypeRef recognizes (a
    // collection / interface / task / FFI / value builtin / exception), as
    // opposed to a user-chosen generic type PARAMETER? A bare single-part Named
    // TypeRef with no declaration is either a type parameter (`T`) or one of
    // these reserved builtins; the #568 nullable-type-param lowering must only
    // intercept the former (a builtin nullable like `Subscription?` keeps its own
    // ptr/value lowering through the switch below). Mirrors the name sets the
    // lowerTypeRef Named-case branches enumerate. Exception types are recognized
    // via builtinVtable_, so this is a member (not static).
    bool isBuiltinTypeRefName(std::string_view name) const {
        static const std::unordered_set<std::string_view> kBuiltins = {
            "IntPtr", "UIntPtr", "CString", "WString", "RawBytes",
            "DateTime", "TimeSpan", "Range", "Guid",
            "ArraySpan", "ReadOnlyArraySpan",
            "Array", "OwnedArray", "Dictionary", "OwnedDictionary",
            "Stack", "OwnedStack", "Queue", "OwnedQueue",
            "Set", "OwnedSet", "LinkedList", "OwnedLinkedList",
            "LinkedListNode", "StringBuilder", "Random",
            "IEnumerable", "IEnumerator", "OwnedEnumerable", "OwnedEnumerator",
            "IAsyncEnumerable", "IAsyncEnumerator",
            "OwnedAsyncEnumerable", "OwnedAsyncEnumerator",
            "IReadOnlyCollection", "IReadOnlyList", "ICollection", "IList",
            "IOwnedCollection", "IOwnedList", "Task", "OwnedTask",
            "Subscription", "CancellationToken", "CancellationTokenSource",
            // §28.13 / stdlib §2.16 (#620): Shared<T> and Weak<T> are
            // compiler-known heap-pointer types.  Without them a bare
            // `Shared<T>?` annotation (no declaration) was mis-classified as
            // a naked generic type-parameter, and the nullable-type-param branch
            // lowered it to {i8,i32} instead of ptr → LLVM GEP type assertion
            // crash on `Shared<T>? := null` / null-compare / `?.` / `!`.
            "Shared", "Weak",
            // §30.14/§30.15 (#600 phase c): Channel<T> and ChannelSender<T>
            // are compiler-known pointer-typed builtins backed by bz_channel_t
            // and bz_channel_sender_t respectively.
            "Channel", "ChannelSender",
            // stdlib §5 (#651): IGrouping<TKey, T> is a compiler-known pointer
            // (bz_grouping_t*) produced by GroupBy.
            "IGrouping",
        };
        if (kBuiltins.count(name)) return true;
        // §31.4: a built-in exception type (Exception / a subtype).
        return builtinVtable_.find(std::string(name)) != builtinVtable_.end();
    }

    static bool isDictTypeName(const sema::TypeRef* rt);

    // stdlib §6.7 / §30.5: true when `rt` is a Task / Task<T> or OwnedTask<T>
    // (bare name "Task" or "OwnedTask", with or without generic args, no user
    // TypeDecl — these are builtin runtime types, not user-defined classes).
    static bool isTaskTypeName(const sema::TypeRef* rt);
    // stdlib §6.7 / §30.5: return a local's resolved Task / Task<T> TypeRef if
    // it is Task-typed; null otherwise. Used by the using-var registration path.
    const sema::TypeRef* taskTypeOfLocal(const ast::LocalDecl* ld);

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

    // fix #567 / §14.1: resolve a bare type-parameter TypeRef (a single-part
    // Named with no `declaration`, e.g. `K` / `T`) to its CONCRETE substituted
    // sema type via the active monomorphization binding (`genericSubstSemaTy_`).
    // Inside a monomorphized generic body the genericArgs of a collection type
    // (`Dictionary<K, int>` / `Set<T>`) still carry the placeholder parameter;
    // returning it unresolved makes hash-backed-collection dispatch select the
    // string-key fast path instead of the concrete key/element's hash/eq
    // (malformed IR / segfault — issue #567). Any non-type-parameter ref, or a
    // type param with no active binding, is returned unchanged. Mirrors the same
    // param-name lookup recordMemberTypeResolved / lowerNew's `new T()` path use.
    const sema::TypeRef* resolveTypeParamRef(const sema::TypeRef* ty) {
        if (!ty || ty->kind != sema::TypeRef::Kind::Named || ty->declaration ||
            genericSubstSemaTy_.empty() || !ty->astNode || !ty->astNode->name ||
            ty->astNode->name->parts.size() != 1)
            return ty;
        auto it = genericSubstSemaTy_.find(ty->astNode->name->parts[0]);
        if (it != genericSubstSemaTy_.end() && it->second) return it->second;
        return ty;
    }

    // fix #568 / §14.1 / §7.1: given a NULLABLE bare type-parameter ref `ty`
    // (`T?`), resolve `T` to its concrete substituted sema type and write a
    // NULLABLE COPY of it into `out` (so isNullableValueTy / nullableValueStructTy
    // see the concrete value type with the `?` still applied). Returns true when
    // the parameter resolves; false otherwise (`out` left unset). Reuses the #567
    // resolveTypeParamRef param-name lookup. `out` is a value the caller owns so
    // the synthesized nullable variant lives only as long as the lowering call —
    // we never mutate the cached concrete TypeRef (its non-nullable form must be
    // preserved for the non-nullable `T`-typed sites).
    bool nullableTypeParamConcrete(const sema::TypeRef* ty, sema::TypeRef& out) {
        const sema::TypeRef* c = resolveTypeParamRef(ty);
        if (!c || c == ty) return false;  // unresolved param: nothing to do
        out = *c;
        out.nullable = true;
        return true;
    }

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
    // per §4.6/§4.7: lower LinkedList<T>/OwnedLinkedList<T> instance calls
    // (Add*/Remove*/Find/Count/First/Last/etc.) and LinkedListNode<T> property
    // accesses (.Value/.Next/.Previous) via the bz_llist_* runtime.
    llvm::Value* tryLowerLinkedListInstanceCall(const ast::CallExpr& call);

    // §4.15 (#549): lower Array<T>.AsSpan()/AsReadOnlySpan() and
    // ArraySpan<T>/ReadOnlyArraySpan<T> instance method calls
    // (Length, IsEmpty, Slice, AsReadOnly, IndexOf, Contains, Fill,
    //  CopyFrom, CopyTo, Reverse, SequenceEqual).
    llvm::Value* tryLowerArraySpanCall(const ast::CallExpr& call);

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

    // stdlib §5 Set-like: hash trampoline `uint64_t(const void* key)` for the
    // Union/Intersect/Except membership dict. Reuses buildValueHashTrampoline
    // for a non-null elemRef; falls back to a memhash for pointer-sized elements.
    llvm::Function* buildLinqHashTrampoline(llvm::Type* elemTy,
                                            const sema::TypeRef* elemRef,
                                            const sema::TypeSymbol* elemRec);

    // Detect Convert.ToInt32(s) / Convert.ToInt64(s) / Math.Abs(n) etc.
    // as intrinsic static class method calls. Returns a lowered value or null.
    llvm::Value* tryLowerBuiltinClassCall(const ast::CallExpr& call);

    // §30.14/§30.15 (#600 phase c): Channel<T>/ChannelSender<T> instance methods
    // and Domain.Spawn(entry, arg?) lowering. Returns null when not matched.
    llvm::Value* tryLowerChannelAndDomainCall(const ast::CallExpr& call);

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

    // §12 / §14 (issue #585): compute the concrete generic substitution for an
    // ANCESTOR generic class reached through inheritance from `recvTy`. When a
    // method is INHERITED — declared on a generic base (`ms->parent` ==
    // `ancestor`) but called on a derived instance (`recvTy` == `Wrapper<string>`)
    // — the receiver-class substitution block only binds the derived class's
    // params, leaving the ancestor's `T` unbound so the inherited method runs as
    // the T→i32 template (mislaying a non-int base field, the read half of #585).
    // Walks the base-class chain from `recvTy`'s declaration up to `ancestor`,
    // resolving each level's base type-arguments through the level below, and
    // fills `outSubst` (param-name → LLVM type) and `outSema` (param-name →
    // concrete TypeRef) for `ancestor`'s generic params. Returns true iff
    // `ancestor` is a generic class reachable from `recvTy` and the chain
    // resolved every parameter. A no-op (returns false) when the method is
    // declared directly on the receiver's class (the existing block handles it).
    bool computeAncestorGenericSubst(
        const sema::TypeRef* recvTy, const sema::TypeSymbol* ancestor,
        std::unordered_map<std::string_view, llvm::Type*>& outSubst,
        std::unordered_map<std::string_view, const sema::TypeRef*>& outSema);

    // CG-generic: compile a specialized version of a block-body method with
    // the current genericSubst_ applied to all types.  The specialized
    // function is cached in specializedMethods_ so each (ms, subst) pair is
    // compiled at most once.  The result is a call to the specialized fn.
    llvm::Value* emitGenericSpecialization(sema::MethodSymbol* ms,
                                           std::vector<llvm::Value*>& args,
                                           const CallableInfo& ci);

    // fix #580 / §14.1 / §14.2: build (or fetch from cache) a STANDALONE
    // monomorphized function for `ms` under the active genericSubst_, with the
    // given concrete parameter types. Handles both block AND expression bodies.
    // Returns the llvm::Function* (does NOT emit a call), so a virtual generic
    // call can place several candidate specializations behind a runtime
    // dispatch-by-descriptor (one per overriding class in the hierarchy).
    llvm::Function* buildSpecializedMethodFn(sema::MethodSymbol* ms,
                                             const CallableInfo& ci,
                                             const std::vector<llvm::Type*>&
                                                 paramTys);

    // fix #580: a VIRTUAL generic method call cannot dispatch through the fixed
    // vtable slot (the slot holds the i32-erased template, so a non-int type
    // argument is passed at the wrong width/representation — a miscompile). The
    // dynamic type is one of a statically-known finite set (the static class and
    // its subclasses). Emit a dispatch-by-descriptor: load the receiver's td
    // (vtable[0]), and for each candidate class call the monomorphized override
    // it most-derives, defaulting to the static class's own specialization.
    // Returns the call result, or nullptr if this path does not apply.
    llvm::Value* emitVirtualGenericDispatch(sema::MethodSymbol* ms,
                                            std::vector<llvm::Value*>& args);

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
    // scrutTy: the resolved TypeRef of the scrutinee, used to detect nullable
    // value types ({i8 hasValue; T value} struct — per §7 / issues #447/#448).
    // When scrutTy is a nullable value type the function extracts hasValue/value
    // and applies struct-based pattern comparison instead of pointer operations.
    llvm::Value* patternMatches(const ast::Pattern* pat, llvm::Value* scrut,
                                const sema::TypeSymbol* enumTy = nullptr,
                                bool isStr = false,
                                const sema::TypeRef* scrutTy = nullptr);

    // CG28: equality of two same-typed scalars (int or FP).
    llvm::Value* emitEquals(llvm::Value* a, llvm::Value* b,
                            bool isStr = false);

    llvm::Value* lowerConditional(const ast::ConditionalExpr& c);
};

}  // namespace bezel::codegen
