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
#include "common/BuiltinRegistry.hpp"  // #1414 stage 3: builtin name registry
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
    // §4.2 / A.1: parse the magnitude with the UNSIGNED strtoull regardless of
    // the trailing IntSuffix. The caller always stores the result as a 64-bit
    // pattern (int64 -> uint64 -> i64 LLVM constant), and `digits` is the bare
    // magnitude — the sign of a negative literal is a separate UnaryExpr — so
    // the full [0, 2^64) range is meaningful. Signed strtoll SATURATES at
    // long.MaxValue (and sets ERANGE) for any magnitude in [2^63, 2^64):
    //   - a `ulong` value above long.MaxValue (e.g. 18000000000000000000UL)
    //     lost its value (issue #421 — already handled for the `U` case), and
    //   - the 2^63 magnitude of `-9223372036854775808L` (long.MinValue)
    //     saturated to 2^63-1, so the literal lowered to INT64_MIN+1 instead of
    //     INT64_MIN (the codegen counterpart of the #749 fold bug).
    // strtoull preserves the bit pattern across the whole range; the
    // out-of-range guard lives in sema (the literal type-check, §5).
    return static_cast<std::int64_t>(
        std::strtoull(digits.c_str(), nullptr, base));
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

// fix #678 (narrowed) / §27.2: returns true when a lambda's expression body
// is a call to a known void-returning BUILTIN INTRINSIC whose sema exprType
// is null (never set to Kind::Unit by the typecheck pass).  Only these
// intrinsic patterns need the explicit void-inference fallback in codegen;
// user-defined void methods and extern void methods receive a Kind::Unit
// exprType from sema, so lowerTypeRef already maps them to voidTy via the
// `ctx_.exprType(*lam.exprBody) != null` branch.
//
// Covered patterns:
//   Console.Write(v)     Console.WriteLine()    Console.WriteLine(v)
//   Console.Error.Write  Console.Error.WriteLine
//   Console.Out.Write    Console.Out.WriteLine
//
// Collection void mutators (Add, Sort, …) return a sentinel i32(0) from
// their codegen path, so they do NOT produce `ret void` in an i32 function;
// they don't need this fallback.
inline bool isLambdaExprBodyVoidBuiltin(const ast::LambdaExpr& lam) {
    if (!lam.exprBody) return false;
    auto* ce = dynamic_cast<const ast::CallExpr*>(lam.exprBody.get());
    if (!ce || !ce->callee) return false;
    auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(ce->callee.get());
    if (!ma || !ma->target) return false;
    if (ma->member != "Write" && ma->member != "WriteLine") return false;
    // Direct Console.Write/WriteLine — callee target is the bare identifier
    // "Console".
    if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(ma->target.get()))
        return id->name == "Console";
    // Console.Error.Write/WriteLine  and  Console.Out.Write/WriteLine.
    if (auto* inner =
            dynamic_cast<const ast::MemberAccessExpr*>(ma->target.get())) {
        if ((inner->member == "Error" || inner->member == "Out") &&
                inner->target) {
            if (auto* id2 = dynamic_cast<const ast::IdentifierExpr*>(
                    inner->target.get()))
                return id2->name == "Console";
        }
    }
    return false;
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
          checkedDisposal_(opts.checkedDisposal),
          isMainModule_(opts.isMainModule) {}

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
        // #429 (box RETURN + FIELD-STORE category) / §8.6 / §28.9, reordered
        // here by fix #2500 (§23.3/§28.9): determine, BEFORE any caller of
        // ensurePropertyAccessorFn runs —
        //   (a) which locally-compiled methods always return boxable value
        //       sources (boxReturnMethods_) — callers of these treat the result
        //       as a +1 box producer and may release it at scope end;
        //   (b) which Object/interface-typed CLASS fields ONLY receive boxable
        //       value stores across the entire class (boxStorageFields_) —
        //       emitDisposerCore releases these fields via bz_box_release.
        // Both analyses depend only on ctx_ (sema, resolved before codegen),
        // fieldIndex_ (declareStructs above), and methodFns_/ctorFns_
        // (declareFunctions above) — none on vtables or late codegen state — so
        // moving them here (before declareVtables/emitExportedPropertySetters,
        // the two earliest ensurePropertyAccessorFn callers) is sound. This is
        // required so that ensurePropertyAccessorFn's auto-SETTER body can
        // consult isBoxStorageMember to decide whether to retain/release a
        // box-typed backing field (the box twin of #2490's string/closure ARC):
        // the body is built ONCE on first request and memoized, and before this
        // reorder both first-call sites ran ahead of collectBoxStorageFields,
        // leaving boxStorageFields_ empty at the point the setter body needed
        // it (the ordering hazard #2500 documents). The analyses STILL satisfy
        // their original "before the method body loop" obligation (that loop is
        // well below), so collectBoxReleaseEligible's isPlusOneBoxProducer ->
        // isUserMethodBoxReturnCall continues to see a populated
        // boxReturnMethods_, and the disposer (emitted after all method bodies)
        // sees a fully-populated boxStorageFields_.
        collectBoxReturnMethods();
        collectBoxStorageFields();
        declareVtables();  // CG17: after functions exist (needs fn ptrs)
        // §2.5 / §22.9 (issue #503): emit ExternalLinkage getter stubs for
        // non-virtual public properties (exporter side). After declareVtables so
        // propGetSlot_ is populated; virtual properties are skipped.
        emitExportedPropertyGetters();
        // fix #2400: the SETTER twin — emit ExternalLinkage setter stubs for
        // non-virtual public properties WITH a setter (exporter side). Same
        // ordering requirement as the getter call above (propSetSlot_ populated).
        emitExportedPropertySetters();
        // fix #2414: the INDEXER analogs of the two calls immediately above —
        // same ordering requirement (propGetSlot_/propSetSlot_ populated by
        // declareVtables, which also runs buildVtable's isVirtualIndexer gate).
        // Gated on `!isMainModule_` (§33.1/§33.6: a main-module executable is
        // never itself an importable artifact, so nothing can ever call a
        // cross-module indexer-accessor symbol emitted for one) — UNLIKE
        // emitExportedPropertyGetters/-Setters just above, which run
        // unconditionally. This gate is deliberate, not just an optimization:
        // probing this fix live turned up is_transitive_generic_2192's
        // `is_transitive2_reified_int_matches_2192_14_1` — a single-module,
        // no-indexer, no-import executable-build test — flipping from a
        // reproducible PASS to a reproducible FAIL whenever
        // emitExportedIndexerGetters/-Setters were called unconditionally
        // (bisected: commenting out just these two calls fixed it; the two
        // functions' own loops provably never entered their `isIndexer`
        // branch for that program, ruled out by both a stderr trace and a
        // file-write trace that logged zero indexer members across every
        // class it visited — yet the *mere execution* of the enclosing
        // no-op-outcome loop, even wrapped in nothing but dynamic_casts and
        // a vector walk, changed the unrelated test's result). That is the
        // signature of a pre-existing, latent memory-safety bug elsewhere in
        // the reified-generic descriptor/`is`-check machinery being exposed
        // by an incidental codegen-layout shift, not a logic error in the
        // code added here — filed as its own issue (see this fix's PR/commit
        // description) since chasing the root cause is a separate, larger
        // undertaking. Restricting these two calls to library builds
        // (already semantically correct — see above) happens to sidestep
        // the trigger entirely: this exact executable-build program takes
        // isMainModule_ == true, so the calls below never fire for it, while
        // every #2414 cross-module test (which compiles the exporting side
        // with `-c` / `compileObject`, isMainModule_ == false) is unaffected.
        if (!isMainModule_) {
            emitExportedIndexerGetters();
            emitExportedIndexerSetters();
        }
        // fix #2387 / fix #2400 / fix #2414: declareImportedPropertyGetters /
        // declareImportedPropertySetters / declareImportedIndexerGetters /
        // declareImportedIndexerSetters (importer-side external getter/setter
        // stubs) now run INSIDE declareVtables() itself, between the buildVtable
        // loops and Pass D — see the call site there for why.
        setupBoxDescriptors();  // §8.6: after descArray_/vtable globals exist
        declareExterns();  // CG26
        // collectBoxReturnMethods()/collectBoxStorageFields() now run BEFORE
        // declareVtables() (see the call site just above computeEnumValues) so
        // that ensurePropertyAccessorFn — invoked from declareVtables (Pass D)
        // and emitExportedPropertySetters — sees a populated boxStorageFields_
        // for the auto-setter body's box-ARC branch (fix #2500).
        for (auto& [sym, fn] : methodFns_) {
            emitFunctionBody(sym, fn);
        }
        emittingGenericTemplateBody_ = false;  // #817: don't leak into ctors/etc.
        for (auto& [sym, fn] : ctorFns_) {
            // §2.5 / §14 (issue #328 item 1, fix #711): an IMPORTED class's
            // ctorFns_ entry is an EXTERNAL declaration — the DEFINING module
            // owns `bz$<Type>$ctor` and emitted its body there. Skip it, exactly
            // as the disposer loop below skips imported disposers. Without this,
            // an imported GENERIC class whose ctor body was exported (#662
            // Foundation C, so `cs->decl != nullptr`) had a second definition of
            // the template symbol emitted importer-side by declareImportedCtor +
            // this loop → `multiple definition of bz$Container$ctor` at link.
            // That standalone copy is also DEAD: every use of an imported
            // generic ctor goes through the per-instantiation inline path
            // (lowerNew / emitChainCall → inlineCtorBody with genericSubst_
            // active), never a call to the unspecialized template symbol.
            if (ctx_.isImported(sym)) continue;
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
        // #1843 / §8.6 #417: now that every method/ctor/disposer/init body has
        // been lowered, allVtables_ holds every vtable an object can carry —
        // including the per-monomorphization vt$C$specN created lazily above.
        // Materialize the real bz$vtable$table (bz_obj_is_string's whitelist).
        finalizeVtableTable();
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
    // per §33.1/§2.4/§33.6 (#1563): true when this compilation unit is the
    // executable's main module, so a qualifying `Main` (§33.2) is lowered as
    // the native process entry point. False for a library (`-c`) build — see
    // CodegenOptions::isMainModule.
    bool isMainModule_ = true;
    llvm::Function* currentFn_ = nullptr;
    // §9.8 v0.141 / #703: the set of class types whose synthesized Serialize /
    // Deserialize body is currently being INLINE-emitted. Serialization codegen
    // inlines an owned/Shared class component's body via a recursive
    // emitClassSerialize / emitClassDeserialize call; a recursive type (a class
    // reached through a Shared<T> field of its own type — a linked list, a cycle
    // spine) would recurse unboundedly. Re-entry on a type already in this set is
    // detected and reported as a clean "not yet implemented" diagnostic instead
    // (a synthesized Serialize FUNCTION would be required to support it).
    std::unordered_set<const sema::TypeSymbol*> serializeEmitStack_;
    std::unordered_set<const sema::TypeSymbol*> deserializeEmitStack_;
    // §8.6: the declared return-type node of the method currently being
    // emitted, so a `return <value>` whose declared return type is
    // Object/interface boxes the value before returning.
    const ast::Type* currentReturnTypeNode_ = nullptr;
    // fix #2688 / §27.3 / §28.9: the LAMBDA currently being emitted carries no
    // declared return-type NODE (#2515 nulls currentReturnTypeNode_ above so a
    // lambda body does not box/mint against the enclosing METHOD's node), but a
    // lambda still has a return TYPE of its own — `retSemaTy` in lowerLambda.
    // The expression-body path already keys its `string`/closure +1 mint off
    // that type; this field carries the same type into emitReturn so the
    // BLOCK-body path mints identically. Non-null only while a synchronous
    // lambda body is being emitted (null for methods, ctors, and async lambdas,
    // whose declared return is the outer Task).
    const sema::TypeRef* currentLambdaReturnSemaTy_ = nullptr;
    // §22.16 (v0.203, #2053) Slice 4b: the ensures single-exit epilogue state
    // for the method/ctor currently being emitted. When `ensuresFn_` is
    // non-null, `emitReturn(fn, v)` redirects the return into
    // `ensuresResultSlot_` and branches to `ensuresExitBB_` instead of emitting
    // `ret` directly; the epilogue block (emitted by `finalizeEnsuresEpilogue`
    // at body-end) reloads the result temp, evaluates each `ensures` clause in
    // declaration order, throws ContractViolationException on a false clause,
    // and only then performs the original `ret`. `ensuresReturnTypeNode_`
    // carries the declared return-type node so the epilogue can box/coerce
    // the result temp exactly as emitReturn would have done. Restored to null
    // when the body is complete and the epilogue has been emitted — so a
    // nested lambda/accessor body (which swaps `currentFn_` and restores it
    // on exit) sees `ensuresFn_ == nullptr` for ITS own returns, even while
    // the outer method is still mid-emission. The save/restore pattern keeps
    // the epilogue scoped to one body.
    llvm::Function* ensuresFn_ = nullptr;
    llvm::BasicBlock* ensuresExitBB_ = nullptr;
    llvm::AllocaInst* ensuresResultSlot_ = nullptr;  // null for void return
    const ast::Type* ensuresReturnTypeNode_ = nullptr;
    const std::vector<ast::ContractClause>* ensuresClauses_ = nullptr;
    std::string ensuresTypeName_;
    std::string ensuresMemberName_;
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

    // #823 / §27.5: slots of locals DECLARED INSIDE a loop body. Such a local is
    // re-initialised every iteration, so a closure capturing it must get a FRESH
    // cell per iteration. boxLocalSlot consults this when its current-BB store
    // heuristic would otherwise miss the init store (nested control flow puts it
    // in an earlier basic block) and wrongly reuse one stale cell. Keyed by the
    // AllocaInst (module-lifetime, unique across the compilation — no clearing).
    std::unordered_set<llvm::AllocaInst*> loopBodyDeclaredSlots_;
    // §27.5 / #673: for guarded-boxed (non-for-var) locals, maps each
    // cell-holder alloca (newSlot) to the ORIGINAL pre-boxing alloca
    // (the variable's original storage slot).  After boxing, the original
    // slot is kept in sync on every write so that pre-emitted code (e.g.
    // a while-loop condition lowered before the body) that loads from the
    // original alloca sees the updated value.  NOT populated for for-var
    // loop variables (their incBB manages the original slot directly).
    // Part of the saved/restored emitter state across nested-function emission.
    std::unordered_map<llvm::AllocaInst*, llvm::AllocaInst*> cellVarSyncSlot_;
    // #429 / §27.5: cells registered by the most-recent lowerLambda call:
    // pairs of (newSlot, captured-decl-or-sym-key) for freshly-boxed,
    // non-for-var captures.  Consumed by lowerLocalDecl when the lambda is
    // proven non-escaping to register boxedCellFree disposals.  Gated on
    // cellFreeEligible_ (pre-scan result) to avoid freeing cells shared with
    // escaping lambdas.  Saved/restored across lowerLambda body-emission so
    // nested lambdas compiled inside this body do not clobber the outer
    // lambda's captures.
    struct LastCellEntry {
        llvm::AllocaInst* newSlot;
        const void* key;   // LocalDecl* (ast::Node*) or Symbol* for params
        // #782: N>0 when `key` is a move-only OWNING inline-array local captured
        // by this (non-escaping) lambda — drives an element-dispose-through-cell
        // disposal next to the boxedCellFree. 0 otherwise.
        unsigned inlineArrayN = 0;
        // fix #2334 / §28.14 / §28.9: true when `key`'s ONLY route into
        // cellFreeEligible_ includes the TaskScope.Spawn special case
        // (unwindUnsafeCellFree_ pre-scan result) — propagated onto the
        // Disposal::skipCellFreeOnUnwind entries consumeLastLambdaCapturedCells
        // registers, so the cell-free is skipped on the unwind edge (see that
        // flag's declaration for the full soundness argument).
        bool unwindUnsafe = false;
    };
    std::vector<LastCellEntry> lastLambdaCapturedCells_;
    // §27.5 / §28.9 (#969): block-nesting depth (== disposeStack_.size()) of each
    // LOCAL at its declaration, keyed by its LocalDecl AST node. Used by the
    // boxedCellFree registration to skip freeing a capture cell at an inner
    // lambda's scope when the captured local was declared in an ENCLOSING scope
    // (the cell must outlive the inner block — it is read through, after the
    // inner block exits, by the outer local). Params are absent (handled by the
    // existing reads-through-cell disposal guard, or left as a noted follow-up).
    std::unordered_map<const void*, std::size_t> declScopeDepth_;
    // CG6: emitted LLVM function for each static method symbol.
    std::unordered_map<sema::MethodSymbol*, llvm::Function*> methodFns_;
    // #1170 / §24.4: cached method-reference thunk per method symbol. A named
    // method reference (`event += OnTick`) must resolve to a STABLE function
    // pointer so `event -= OnTick` removes the subscribed handler by value
    // (fn + receiver) — see bz_event_remove_closure. Without caching, each
    // makeMethodThunk call emits a fresh thunk$N, defeating the equality. The
    // thunk body is receiver-independent (it loads `this` from the env block),
    // so a single thunk per method is correct; the per-conversion receiver
    // lives in the freshly-allocated env, never in the shared thunk.
    // §27.6 (#1306 rework): keyed by {method, isUnboundExtRef} so a BOUND
    // (`x.M`, receiver captured) and an UNBOUND (`T.M`, receiver is the first
    // parameter) reference to the same extension method cache DISTINCT thunks
    // (they have different signatures).
    std::map<std::pair<sema::MethodSymbol*, bool>, llvm::Function*> methodThunks_;
    // §27.6 (v0.184, #1758): cached thunk per primitive-intrinsic static method
    // reference, keyed by (primitive kind, member name). A primitive intrinsic
    // (`int.Parse`, `char.IsDigit`, …) has no MethodSymbol — it is recognized by
    // name in sema/codegen — so makeMethodThunk cannot back a bare method
    // reference to it. makePrimIntrinsicThunk emits a `ret (ptr env, params…)`
    // thunk that internally re-lowers the intrinsic call, and this cache gives
    // repeated references to the same intrinsic a STABLE fn pointer (the same
    // #1170 reasoning methodThunks_ uses for §27.7.1 equality).
    std::map<std::pair<int, std::string>, llvm::Function*> primIntrinsicThunks_;
    // §27.6 (v0.184, #1758): while emitting a primitive-intrinsic thunk body,
    // maps each synthetic argument node to the thunk's corresponding LLVM
    // parameter so the reused intrinsic-call lowering (`lowerExpr` on the
    // synthetic arg) yields the thunk's incoming value. Empty outside thunk
    // emission; checked at the top of lowerExpr only when non-empty.
    std::unordered_map<const ast::Expr*, llvm::Value*> exprValueOverride_;
    // §27.6 (v0.184, #1758): owns the synthetic `Type.Member(args…)` call trees
    // built for primitive-intrinsic thunk bodies. Kept alive for the whole
    // compilation so the exprType entries set on their argument nodes never
    // outlive their key (one small tree per cached thunk).
    std::vector<std::unique_ptr<ast::CallExpr>> primIntrinsicSynthCalls_;
    // §27.7.1 (v0.180, #1544): cached property-getter-reference thunk per
    // property symbol — the SAME #1170 stability reasoning as methodThunks_
    // above, applied to a property used as a function value (§27.6, `var f:
    // () -> T := b.P`). Without this, each makePropertyThunk call emitted a
    // fresh pthunk$N, so two references to the SAME property on the SAME
    // receiver had different `fn` pointers and never compared equal under
    // the general `=`/`<>` operator (bz_closure_value_equals) even though
    // "property references follow the method-reference rule" (§27.7.1
    // fourth bullet).
    std::unordered_map<sema::PropertySymbol*, llvm::Function*> propertyThunks_;
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

    // #720 / §22.13 / §22.7: lazy static initialization (NON-GENERIC types only;
    // generic per-instantiation lazy init is a tracked follow-up). For each
    // non-generic type with static state (an sfinit and/or a cctor), a
    // `bz$<T>$ensureinit()` function runs that type's initializers ONCE, on first
    // use, with a per-type state guard `bz$<T>$initstate` (0=uninit, 1=in-progress,
    // 2=done, 3=failed) and a saved-exception global `bz$<T>$initexc`. On an
    // uncaught initializer exception the type enters the permanent failed state
    // and every subsequent access re-raises the saved TypeInitializationException
    // (§22.7). Static-access lowering sites call emitEnsureStaticInit(owner) before
    // the access so the (possible) TIE routes through the current unwind target.
    std::unordered_map<const sema::TypeSymbol*, llvm::Function*> ensureInitFns_;
    // #1553 / §22.13 (v0.181): per-type static-init state globals, created in
    // emitEnsureInitFns and shared with the sfinit body (progress bumps) and the
    // partial-state read guard. initState: 0 uninit / 1 in-progress / 2 done /
    // 3 failed. initOwner: id of the thread that moved the state to in-progress
    // (owner-thread tracking). initProgress: count of non-constant static-field
    // initializers completed (textual order).
    std::unordered_map<const sema::TypeSymbol*, llvm::GlobalVariable*> initStateGV_;
    std::unordered_map<const sema::TypeSymbol*, llvm::GlobalVariable*> initOwnerGV_;
    std::unordered_map<const sema::TypeSymbol*, llvm::GlobalVariable*> initProgressGV_;
    // Per-type sfinit / cctor functions, indexed for ensureinit emission. Built
    // from staticCtors_ in declareStaticInitializers.
    std::unordered_map<const sema::TypeSymbol*, llvm::Function*> sfinitFnOf_;
    std::unordered_map<const sema::TypeSymbol*, llvm::Function*> cctorFnOf_;

    // #1513 / §2.8.3 rule 4 / §22.12 rule 1: cross-module per-INSTANTIATION lazy
    // static init for a generic class's static state — the generic-instantiation
    // counterpart of the ensureInitFns_/initStateGV_ family above, keyed by
    // (generic TypeSymbol*, recursive type-arg mangle) instead of by type alone.
    // A generic's static state is per-instantiation (§22.12: "each distinct
    // instantiation has its own static state"), and — the specific gap #1513
    // closes — one instantiation (e.g. `Reg<int>`) may be independently
    // monomorphized by SEVERAL importing modules' separate compilations
    // (§2.8.2's definition-site visibility re-monomorphizes carried generic
    // bodies at each consumer). The guard, its state, and the state it guards
    // are all named "<declaring-module>.<Class>$<mangle>$..." (mangledTypeName
    // + the SAME typeArgMangle genericInstStaticGlobals_ already uses) and
    // linked LinkOnceODR, so every consumer's independently-compiled, byte-
    // identical copy converges on ONE runtime definition, ONE state cell, and
    // ONE execution — §2.8.4 rule 5's "one copy per process" applied to
    // generic-instantiation state. (Deliberately mirrors, rather than reuses,
    // the per-type FSM in emitEnsureInitFns/emitStaticConstructorBody: those
    // functions assume a LOCAL, single-compilation-unit type and are not
    // parameterized by instantiation; duplicating the (proven) pattern here
    // keeps that delicate, heavily-fixed per-type path untouched.)
    using GenericInstKey = std::pair<const sema::TypeSymbol*, std::string>;
    // The type-argument substitution for one instantiation, recorded the first
    // time ANY of its static fields is touched (staticFieldGlobal's redirect)
    // and reused when genericInstSfinitFn later needs to lower a SIBLING
    // field's initializer under the same substitution.
    struct GenericInstArgs {
        std::vector<llvm::Type*> substTypes;
        std::vector<const void*> substIds;
        std::vector<const sema::TypeRef*> substSemaTys;
    };
    std::map<GenericInstKey, GenericInstArgs> genericInstArgsOf_;
    std::map<GenericInstKey, llvm::Function*> genericInstSfinitFns_;
    std::map<GenericInstKey, llvm::Function*> genericInstEnsureInitFns_;
    std::map<GenericInstKey, llvm::GlobalVariable*> genericInstInitStateGV_;
    std::map<GenericInstKey, llvm::GlobalVariable*> genericInstInitExcGV_;
    // #1513 / #1391 / §22.12 rule 1: per-instantiation owner-thread global —
    // the per-instantiation counterpart of initOwnerGV_ above, recorded by
    // whichever thread's CAS wins the 0(uninit)->1(in-progress) claim in
    // genericInstEnsureInitFn, so a same-thread re-entrant touch (proceed) can
    // be told apart from a genuine cross-domain touch (block via
    // bz_static_init_wait) exactly as emitEnsureInitFns does for the per-type
    // case.
    std::map<GenericInstKey, llvm::GlobalVariable*> genericInstInitOwnerGV_;
    // Reverse lookup: a per-instantiation static-field global (as returned by
    // staticFieldGlobal) to the (type, typeArgMangle) key that owns it, so a
    // static-access call site can find/build the right ensureinit function
    // without re-deriving the generic-instantiation key from the AST a second
    // time (staticFieldGlobal's own Path A / Path B logic already computed it).
    std::unordered_map<llvm::GlobalVariable*, GenericInstKey>
        genericInstGlobalKeyOf_;

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
        // (Array<T>/OwningArray<T>/…). At scope end it is freed via
        // bz_array_free. `collectionElemDisposer` is the per-element dispose
        // callback for an OWNING collection of class elements (null for a
        // borrowing collection or value elements — only the backing is freed).
        bool collection = false;
        llvm::Value* collectionElemDisposer = nullptr;
        // #394 / stdlib §4.9 + #401 / stdlib §4.8: a `using var` bound to a
        // hash-backed dictionary (Dictionary<K,V>/OwningDictionary<K,V>). At scope
        // end it is freed via bz_dict_free. `dictValueDisposer` is the per-VALUE
        // callback passed to bz_dict_free; it is one of exactly two mutually
        // exclusive things (or null):
        //   * #394: an OwningDictionary<K,V> with a CLASS value type → the
        //     owning-class value-disposer (dispose + free the object via its
        //     vtable). OwningDictionary owns its VALUES only.
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
        // OwningLinkedList<T>. At scope end it is freed via bz_llist_free.
        // `collectionElemDisposer` (reused) is the per-element dispose callback
        // for an OwningLinkedList<T> of class elements (null for LinkedList<T>
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
        // §30.15 / §28.13 / §28.3 (#1957): like `unwindOnly`, but for a
        // TYPE-SPECIFIC builtin disposal (channelSender / channel / sharedHandle)
        // rather than the generic vtable dispatch. Set for an `owned`
        // ChannelSender<T>/Channel<T>/Shared<T> PARAMETER that is discharged on
        // the normal path (moved/returned/disposed) and so must run its
        // type-specific runtime release (bz_channel_sender_release, etc.) ONLY on
        // the exception-unwind exit — never on normal exit, where the discharge
        // site already released it (a normal-exit release would double-release /
        // underflow the refcount). The auto-drop case (a never-discharged owner)
        // instead leaves this false so the type-specific branch runs on BOTH
        // exits (the sole release). The channelSender/channel/sharedHandle
        // disposal branches gate on this flag; it is unset (false) for every
        // `using var` registration, so those are unaffected.
        bool releaseOnUnwindOnly = false;
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
        // fix #1747 / §28.9 / §9.7 / §28.3: a plain `var` RECORD local (value
        // type) bound from a RECORD-RETURNING CALL (`var p := Mk()`) whose
        // declared type owns at least one `string` field, and which the
        // conservative escape walk proved NON-escaping. A record's string
        // fields are runtime-managed (§28.9); a record returned by value
        // carries its callee-minted +1 on each string field out to the caller
        // (a directly-returned `new P(...)` retains each string field; this
        // is the narrow scope the prior lane decline pinned). A record has
        // no disposer (§9.7), so without this scope-end release the caller's
        // local leaks the +1 (#1747). At scope end — on BOTH the normal and
        // unwind exit (the fields die either way; a given exit goes one way
        // only, so each is released exactly once) — every string field is
        // released via bz_string_release (null-safe / sentinel-no-op) and
        // nulled. The local was proven non-escaping, so no alias survives
        // the scope (no UAF) and each field owns exactly one +1 (no
        // double-free). `slot` is the record's struct alloca; `cls` is the
        // record TypeSymbol (used to GEP each string storage member).
        bool recordStringFields = false;
        // fix #2425 / §9.8 / §28.3 / §28.9 / §35.4 v0.235: an ALREADY-
        // POPULATED deserialize component/payload slot (#2398 disposal
        // scope, LlvmBackend_Serialization.cpp) whose declared type is a
        // value-like CHOICE that owns a string somewhere reachable from its
        // active case (directly, or through a nested record/choice
        // payload). Guards the slot against a LATER sibling's read
        // throwing, the choice-side counterpart of `recordStringFields`
        // just above — released via emitChoiceStringFieldReleases, a
        // separate tag-switched walker (the choice struct's `$case`-tagged
        // variant layout differs from a record's plain field sequence, so
        // it cannot reuse `emitRecordStringFieldReleases` verbatim). `slot`
        // is the choice's struct alloca; `cls` is the choice TypeSymbol.
        bool choiceStringFields = false;
        // fix #2326 / §9.8 v0.228: an ALREADY-POPULATED deserialize component
        // slot (#2398 disposal scope, LlvmBackend_Serialization.cpp) whose
        // declared type is a non-nullable INLINE ARRAY whose element type
        // carries a string — `[N]string`, `[N]RecWithAStringField`, etc.
        // Guards the slot against a LATER sibling's read throwing; released
        // via emitInlineArrayStringReleases, the array-side mirror of
        // `choiceStringFields` (its own walker, because the array's
        // fixed-size-element layout has no analog of the record/choice
        // field-index machinery the existing helpers key off). `slot` is
        // the array's `[N x elem]` struct alloca; `inlineArrayElemType` is
        // the element TypeRef the walker dispatches on. NOT the same
        // mechanism as the #747 owning-inline-array scope-end disposal
        // (a per-element dispose-and-reclaim via the element vtable — that
        // path is for `[N owned]T` of class elements; this one is for
        // `[N]T` of string/record/choice/inline-array elements whose
        // strings must be released at scope end).
        bool inlineArrayStringFields = false;
        const sema::TypeRef* inlineArrayElemType = nullptr;
        // §2.15 / issue #550: a `using var` bound to a BinarySerializer or
        // BinaryDeserializer.  These are opaque heap handles with no user
        // TypeSymbol and no vtable.  At scope end they are freed via
        // bz_binser_free / bz_binder_free respectively.  Recognised by the
        // local's declared/resolved type name or the NewExpr type name.
        bool binarySerializer = false;
        bool binaryDeserializer = false;
        // §2.15: a `using var` bound to a JsonSerializer or JsonDeserializer.
        // These are heap-allocated via bz_jser_alloc / bz_jder_alloc(/_str) and
        // disposed via bz_binser_free / bz_binder_free respectively.
        bool jsonSerializer = false;
        bool jsonDeserializer = false;
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
        // stdlib §7.2 / issue #645: a `using var` bound to a FileStream. The
        // stream is a heap handle (bz_filestream_t*) with no user TypeSymbol,
        // so classOfLocal returns null and the class-disposal path misses it.
        // At scope end it is disposed via bz_filestream_dispose, which is what
        // realizes §7.2's flush-on-dispose contract on EVERY exit edge,
        // exceptions included.
        bool fileStream = false;
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
        // §30.5 / #1258: for a `using var` bound to an OwningTask<T:class>, the
        // per-T disposer that disposes the produced-but-unconsumed owned class
        // result at scope end (passed to bz_task_dispose_owned_result before the
        // task is freed). Null for a Task<T> / value-result / no-value task.
        llvm::Value* taskOwnedResultDisposer = nullptr;
        // §30.15 (v0.179, #1524): true when this task local's initializer is
        // (syntactically) a `Channel<T>.Receive()` call. Overrides the ordinary
        // `task` disposal below at scope end: instead of
        // `bz_task_fire_and_forget[_checked]` (which — per §30.5's default
        // dispose-continuation semantics — would eventually dispose a produced-
        // but-unconsumed item, or silently drop a not-yet-produced one), the
        // scope-end drop calls `bz_channel_receive_abandon`, which deregisters a
        // still-parked waiter or gives a produced item back to the channel's
        // head — "no message is consumed or lost" (§30.15). `taskOwnedResultDisposer`
        // is intentionally left null alongside this flag (see
        // isChannelReceiveInitializer's call sites): the abandon path never
        // disposes the item, so no per-T disposer is needed.
        bool channelReceiveTask = false;
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
        // stdlib §6.10 / §28.14 (v0.175, #808): an `await using var` bound to
        // a TaskScope. NORMAL exit: bz_task_scope_dispose_async (cancel the
        // scope's token, drain remaining children, swallow their faults;
        // fault the returned task with InvalidOperationException iff `await
        // Join()` was never reached) and AWAIT the returned task — the drain
        // is a real suspend point. UNWIND exit (or non-async context):
        // bz_task_scope_dispose_unwind — cancel + NON-BLOCKING zombie drain
        // (a blocking wait inside a domain continuation deadlocks by design;
        // the runtime frees the scope at the last child completion). The
        // runtime frees the scope struct on both paths — no free() here.
        bool taskScope = false;
        // §30.15 (#600 phase c): a `using var` bound to a Channel<T>. At scope
        // end it is freed via bz_channel_dispose (closes the channel, faults any
        // pending receivers with ChannelClosedException, disposes undelivered
        // items in reverse insertion order per §28.11). No user TypeSymbol;
        // recognised by the builtin type name "Channel". The element disposer
        // (for class-typed T) is embedded in the channel struct itself (set at
        // construction time), so the Disposal struct needs no extra field.
        bool channel = false;
        // §6.2 / issue #647: a `using var` bound to a Mutex. At scope end it
        // is disposed via bz_mutex_dispose (idempotent: marks disposed, drops
        // the Mutex object's core ref). No user TypeSymbol; recognised by
        // initializer type name "Mutex" or by declared type annotation.
        bool mutex = false;
        // §6.2 / issue #647: a `using var` bound to a Lock. At scope end it
        // is released via bz_lock_release (idempotent per §28.2: released flag
        // prevents double-release). No user TypeSymbol; recognised by the
        // inferred expression type name "Lock" (return of Acquire/TryAcquire).
        // bz_lock_release is null-safe (handles a null Lock? that was never set).
        bool lock = false;
        // §6.3 / issue #647: a `using var` bound to a Semaphore. At scope end
        // it is disposed via bz_semaphore_dispose (idempotent: marks disposed,
        // drops the Semaphore object's core ref). No user TypeSymbol; recognised
        // by initializer type name "Semaphore" or by declared type annotation.
        bool semaphore = false;
        // §6.3 / issue #647: a `using var` bound to a SemaphoreToken. At scope
        // end it is released via bz_semaphore_token_release (idempotent per §28.2:
        // released flag prevents double-release). No user TypeSymbol; recognised
        // by the inferred expression type name "SemaphoreToken" (return of
        // Semaphore.Acquire/TryAcquire).
        // bz_semaphore_token_release is null-safe (handles null SemaphoreToken?).
        bool semaphoreToken = false;
        // §30.15 (#600 phase c): a `using var` bound to a ChannelSender<T>. At
        // scope end it is released via bz_channel_sender_release (decrements
        // sender count; if last sender, closes the channel and faults pending
        // receivers with ChannelClosedException). No user TypeSymbol; recognised
        // by the builtin type name "ChannelSender".
        bool channelSender = false;
        // §6.6 / issue #974: a `using var` bound to a TaskCompletionSource<T>,
        // OwningTaskCompletionSource<T>, or TaskCompletionSource (non-generic).
        // At scope end:
        //   1. bz_tcs_fault_if_pending(tcs, ioe_vtable) — faults the inner
        //      task with InvalidOperationException if not already completed.
        //   2. Error_obj cleanup on the inner task (same as d.task path).
        //   3. bz_task_fire_and_forget(inner_task) — releases TCS's task ref.
        //   4. bz_tcs_free(tcs) — frees the TCS struct itself.
        // No user TypeSymbol; recognised by the builtin type name.
        bool tcs = false;
        // §6.5 / §24.3 (#2026): a `using var` bound to an EventSource<T> /
        // AsyncEventSource<T> (constructed via `new EventSource<T>()` / a factory
        // returning the source). The runtime value is a bz_event_source_t* with
        // no user TypeSymbol and no vtable, so classOfLocal returns null and
        // every prior detection block misses it — without this arm the source
        // leaked its 32-byte struct (and its handler array once subscribed) at
        // scope end. At scope end it is freed via bz_event_source_free (null-safe:
        // `if (!s) return;`), which severs the subscriber list and frees the
        // struct (the borrowed handler closures are owned by their Subscriptions,
        // disposed separately). Recognised by builtin type name "EventSource" /
        // "AsyncEventSource" in the declared type, the NewExpr type name, or the
        // inferred initializer exprType (the #2017 generic-factory shape).
        bool eventSource = false;
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
        // issue #545: a `using var` bound to a Buffer<T>. At scope end it is
        // freed via bz_buffer_free (null-safe; plain free because value elements
        // have no disposal obligation — no per-element callback needed).
        // No user TypeSymbol; recognised by the builtin type name "Buffer" in
        // lowerLocalDecl.
        bool buffer = false;
        // stdlib §10.1/§10.2/§10.3 / §32.6 / fix #977: a `using var` bound to a
        // CString, WString, or RawBytes produced by a static From/FromBytes
        // factory. The factory allocates a fresh malloc'd buffer (bz_string_to_utf8z
        // for CString, bz_string_to_utf16z for WString, memdup for RawBytes). At
        // scope end the buffer is released via plain free() — the same allocator
        // used by those runtime helpers. Null-guarded. No user TypeSymbol;
        // recognised by the result type's builtinTypeName ("CString"/"WString"/
        // "RawBytes") in lowerLocalDecl.
        bool ffiBuffer = false;
        // stdlib §10.3 / fix #985: a `using var` bound to a RawBytes. Unlike
        // CString/WString (bare malloc'd buffers freed with a single free()),
        // RawBytes is a two-level allocation — a { ptr, len } struct whose `ptr`
        // owns a separate byte buffer. Disposal must free BOTH the buffer and the
        // struct exactly once via bz_rawbytes_free. Set together with ffiBuffer
        // so the disposal path (emitDisposals) can pick the RawBytes free helper.
        bool rawBytes = false;
        // §29.7 (#623): an iterator enumerator (IEnumerable<T> state machine)
        // whose Dispose is at vtable slot 1 (iterator vtable format:
        // [0]=td, [1]=Dispose, [2]=MoveNext, [3]=Current) — NOT slot 2 (class
        // vtable format).  At scope end (break / return / exception) we call
        // vtable[1](self) then free the frame.  Distinct from `dynamicDispose`
        // which dispatches through slot 2 of the class vtable.
        bool iterEnumDispose = false;
        // #429 / §27.5: a boxed capture cell (the shared heap alloc from
        // boxLocalSlot) whose owning lambda is proven non-escaping.  `slot`
        // is the ptr-alloca (newSlot) whose current value is the cell ptr.
        // At scope end: free(load(slot)).  Sound because:
        //   (a) lowerLocalDecl registers boxedCellFree BEFORE closureRelease in
        //       the disposal vector; emitDisposals iterates in reverse, so
        //       closureRelease (higher index) fires FIRST — the envdtor reads
        //       all captured values through the cell — THEN boxedCellFree
        //       (lower index) frees the cell.  No UAF: the cell is not read
        //       after the closureRelease completes.
        //   (b) All lambdas that captured this cell are non-escaping
        //       (cellFreeEligible_ pre-scan), so no live env holds a pointer
        //       to this cell past scope exit.
        //   (c) The cell ptr may be null (guarded boxing, guard never ran).
        //       free(null) is a no-op per POSIX.
        // Fires on BOTH normal and unwind paths (symmetric with closureRelease).
        bool boxedCellFree = false;
        // #1017 / §27.5: a boxed capture cell whose plain `boxedCellFree` was
        // SUPPRESSED by the hasOwnerDisposal guard (an owned / `using var` local
        // captured by ≥1 non-escaping closure: the local's own scope-end disposal
        // reads the object THROUGH the cell, so a `boxedCellFree` firing first
        // would UAF). Freeing the cell was therefore skipped entirely and it
        // leaked (8 bytes; masked by LSan reachability in some shapes). This
        // deferred entry is registered in the OWNER's scope (the scope that holds
        // the reads-through-cell disposal — possibly an ENCLOSING scope, #966) and
        // is freed in a SECOND pass of emitDisposals, AFTER every main-loop
        // disposal in that scope has fired — so the owner has already read the
        // object through the cell and every capturing closure (env) has been
        // released. Mutually exclusive with `boxedCellFree` (one or the other,
        // never both → freed exactly once), and gated by cellFreeEligible_ (no
        // escaping env holds the cell past scope exit → no UAF). `slot` is the
        // cell-holder ptr-alloca; the cell ptr may be null (free(null) no-op).
        bool boxedCellFreeDeferred = false;
        // fix #2334 / §28.14 / §28.9: applies to `boxedCellFree` /
        // `boxedCellFreeDeferred` ONLY. `boxedCellFree`'s "fires on BOTH
        // normal and unwind paths" soundness argument (above) rests on every
        // capturing lambda being PROVABLY non-escaping BY SCOPE END on
        // EITHER exit edge — true for the ordinary IIFE / `var f := lambda`
        // forms, where the closure's own release is registered in the SAME
        // disposeStack_ scope and fires on both edges symmetrically. The
        // collectCellFreeEligible TaskScope.Spawn special case (LlvmBackend_
        // Setup.cpp) is different: its "non-escaping by scope end" argument
        // depends on the bounding `using var`'s disposal being QUIESCENT —
        // true for the properly-awaited NORMAL edge, but NOT for the unwind
        // edge, where TaskScope.DisposeAsync is a documented (#1422)
        // NON-BLOCKING zombie drain that returns before children finish (the
        // synchronous-fallback path noted in taskscope_asan_808_test.cpp /
        // taskscope_behavior_610_test.cpp's exception_edge_drain_is_
        // nonblocking_zombie_1422). Freeing the cell on the unwind edge in
        // that shape is a genuine use-after-free: a still-running child can
        // read the cell after it is freed (confirmed by ASan — the adversarial
        // regression test in taskscope_asan_2334_test.cpp derives from the
        // actual crash this flag fixes). Set true only for cellFreeEligible_
        // captures the pre-scan additionally flagged unwindUnsafeCellFree_;
        // emitDisposals skips the free on the unwind edge for such an entry,
        // leaving the ordinary conservative leak instead of freeing early.
        bool skipCellFreeOnUnwind = false;
        // §15.3 / §15.1.1 (#747): a move-only OWNING inline-array local
        // ([N owned]T? / [N owned]T) whose ownership is NOT transferred out of
        // the local anywhere in the body (per the inlineOwnedArrayTransferred_
        // pre-scan). At scope end — on BOTH the normal and the unwind exit (the
        // array's scope ends either way; a given exit goes one way only → exactly
        // once per exit, §15.3 line 2021 + §28.10 line 2025) — each non-null slot
        // is disposed-and-reclaimed via the slot object's own vtable (dynamic
        // dispatch, mirroring emitOwnedFieldDisposals' class-field path), then
        // nulled so a re-entrant pass is a no-op. `slot` is the array alloca
        // ([N x ptr]); `inlineSlotCount` is N. Registration is restricted to
        // never-transferred locals so this is the SOLE disposal site (a
        // transferred-out array is skipped — the receiving local / callee owns it
        // — so no double-free; a conservatively-misjudged transfer at worst
        // leaks, never double-frees).
        bool inlineOwnedArray = false;
        unsigned inlineSlotCount = 0;
        // §15.3 / §27.5 (#782): a move-only OWNING inline-array local that is
        // CAPTURED by a non-escaping closure. Capture boxes the [N x ptr] value
        // into a heap cell (boxLocalSlot, §27.5) and marks the local "transferred"
        // so its plain inlineOwnedArray slot disposal is skipped (the boxed slot is
        // now a ptr-holder, so the alloca GEP would be out of bounds). Without a
        // replacement the owned elements leak (#782). This disposal disposes each
        // non-null element THROUGH the cell: `slot` is the cell-holder ptr-alloca
        // (newSlot), `inlineSlotCount` is N. Registered next to the captured cell's
        // boxedCellFree (so it is non-escaping-gated via lastLambdaCapturedCells_)
        // and BEFORE it in firing order (dispose elements, then free the cell). The
        // original [N x ptr] alloca disposal stays skipped, so each element is
        // disposed exactly once (no double-free). bz_box-style no GEP on the
        // ptr-holder: the cell pointer is loaded first.
        bool inlineOwnedArrayCell = false;
        // #1930 / stdlib §5 / §28.9: a plain `var g := <src>.GroupBy(sel).First()`
        // (or .Single()/.Last()/.ElementAt(i)/.*OrNull()/no-seed .Aggregate(...)) —
        // a local bound to a bare-GroupBy element-RETURNING terminal, whose result
        // is the returned IGrouping pointer. The terminal already freed the SIBLING
        // groups + the outer array spine (bz_groupby_free_groups_except), keeping
        // ONLY the returned grouping alive; this disposes THAT grouping (its element
        // array + key buffer + object) at scope end via bz_grouping_free. Fires on
        // BOTH the normal and unwind exit (the reference dies either way; a given
        // exit goes one way only → exactly once per exit). Registered ONLY for a
        // local the StringEscapeWalker proved never escapes (groupingFreeEligible_):
        // nothing else holds the pointer, so no UAF and no double-free. `slot` holds
        // the grouping pointer; bz_grouping_free is null-safe (an *OrNull empty
        // result is null). The class/collection/dict fields are unused.
        bool grouping = false;
        // §35.4 (v0.200, #2045 stage 3): an OWNING-CLASSIFIED choice local
        // (a sum whose classification is Owning, e.g. one with an `owned`
        // payload slot per §35.4.1). At scope end the active case's owned
        // payloads are disposed in payload declaration order (§35.4 "Disposal
        // is defined by the active case"). `slot` holds the choice struct
        // value (the `tag` lives at field 0; payload slot i is field 1+i).
        // The disposal emit switches on tag, dispatching Dispose via the
        // payload's runtime vtable (dynamicDispose, since the static class
        // may not be known at the choice site — an interface-typed payload
        // or an interface return reaching an owning choice). For payload-
        // free / value-payload active cases the switch arm is empty.
        // Choice typed by `choiceTy` (the TypeSymbol — null when the slot
        // is statically known to be an owning choice via the
        // payload-marker walk).
        bool owningChoice = false;
        // §35.4 (#2045 stage 3): the choice TypeSymbol whose owning-
        // classified payload fields the active-case disposal switch must
        // traverse. Set together with `owningChoice = true`. Used by the
        // emit to walk the cases in declaration order and find each case's
        // owned payload fields; pairing these (tag == i, payload slot ==
        // 1+i) lets the dispose switch pick the right field offsets.
        const sema::TypeSymbol* owningChoiceTy = nullptr;
        // §35.4 / §28.1 (v0.200, #2047): the INSTANTIATED choice TypeRef (with
        // genericArgs, e.g. `Result<C, string>`) so the active-case disposal can
        // (a) install the generic substitution → the specialized payload struct,
        // and (b) apply §28.1 per-instantiation erasure per owned field — a
        // `owned` field whose instantiated type is value-like / runtime-managed
        // (e.g. `owned error: string`) is NOT vtable-disposed. Null for a
        // non-generic owning choice (owningChoiceTy alone suffices there).
        const sema::TypeRef* owningChoiceRef = nullptr;
        // fix #2115 / §28.9 / §35.4: a VALUE-LIKE choice local whose cases
        // carry a runtime-managed (`string`) payload. The choice value is a
        // value-typed struct on the stack; a +1 producer string stored into a
        // case payload (`Msg.Note("he" + "llo")`) is held by NO other owner —
        // the construction path (LlvmBackend_Calls2.cpp §35.3) stores the
        // pointer raw, with no retain tracking, and value-like choices are
        // skipped by registerOwningChoiceDisposal (the owning switch is for
        // `owned`-marked heap payloads only). So the scope-end auto-dispose
        // must release the ACTIVE case's string slot(s): a tag-switched
        // bz_string_release walk, the same switch/join shape as the owning-
        // choice active-case disposal but calling emitStringRelease (null- and
        // sentinel-safe) on the active case's string-typed payload fields.
        // Fires on BOTH the normal and unwind exit, exactly once per exit
        // (mirroring owningChoice). Registered ONLY for a local the escape
        // walk (StringEscapeWalker) proved never escapes AND whose initializer
        // is a choice construction every string payload arg of which is a +1
        // producer or literal — so the slot provably owns exactly +1 on the
        // active case's strings and no copy/alias/release-site can double-free.
        bool valueLikeChoiceStrRelease = false;
        // fix #2115: the choice TypeSymbol + INSTANTIATED TypeRef the release
        // switch traverses (the tag is field 0; case i's payload slot is
        // field 1+i; a string payload field is released via emitStringRelease).
        const sema::TypeSymbol* valueLikeChoiceStrReleaseTy = nullptr;
        const sema::TypeRef* valueLikeChoiceStrReleaseRef = nullptr;
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
    // callbacks for OwningArray<Shared<T>> / OwningArray<Weak<T>>.
    llvm::Function* sharedReleaseThunk_ = nullptr;
    llvm::Function* weakReleaseThunk_ = nullptr;
    // stdlib §5 (#651): bz_grouping_free thunk for Array<IGrouping> disposal.
    llvm::Function* groupingFreeThunk_ = nullptr;
    // §2.11 (#838): bz_subscription_dispose thunk for OwningArray<Subscription>
    // (and other owning collections) element disposal.
    llvm::Function* subscriptionDisposeThunk_ = nullptr;
    // §28.11 (#868): per-element-collection-type thunks for nested owning
    // collections (e.g. OwningArray<OwningArray<R>>). Keyed by the TypeRef* of
    // the inner collection element type (unique per sema type).
    std::map<const sema::TypeRef*, llvm::Function*> nestedCollectionDisposerThunks_;
    // #2279 (§30.5 Pattern A, v0.236): per-result-collection-type thunks for the
    // intrinsic-collection case `Task<intrinsic-coll>` — when a Pattern A task
    // produces an Array<T>/Set<T>/Dictionary<K,V>/LinkedList<T>/OwningBuffer<T>/
    // OwningArray<T>/... etc. as its class-typed result, the existing
    // elementPolicyFor-based path returns a null disposer (the element is a
    // value/string/etc., not a class), so the produced collection's backing
    // storage was never reclaimed at task disposal (an LSan leak of the
    // bz_array_t*/bz_dict_t*/bz_llist_t* header+data). This cache holds the
    // per-collection-kind thunks that free the collection via the appropriate
    // runtime call (bz_array_free / bz_dict_free / bz_llist_free / bz_buffer_free)
    // with the right per-element disposer, exactly mirroring the `using var`
    // collection disposal path. Keyed on the result-collection TypeRef*.
    std::map<const sema::TypeRef*, llvm::Function*> patternAResultCollectionDisposers_;
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
        llvm::AllocaInst* exitModSlot;  // i32: 0=fall-through,1=return,2=break,3=cont,4=yield-break,5=async-return
        llvm::AllocaInst* retValSlot;   // alloca for return value (null if void)
        llvm::Type*       retTy;        // function return type
        llvm::BasicBlock* afterBB;      // continuation after the try
        llvm::BasicBlock* breakBB;      // outer loop break target at try-entry time
        llvm::BasicBlock* contBB;       // outer loop continue target at try-entry time
        // #1083 / §31.2: loopStack_ size at try-entry. A break/continue whose
        // target loop was pushed INSIDE the try body (loopStack_.size() >
        // loopDepthAtEntry) stays within the try — it must jump straight to the
        // loop target WITHOUT routing through this finally (the finally runs
        // later, when the try itself exits). Only a break/continue that EXITS
        // the try (target loop at/below this depth) routes through the finally.
        std::size_t loopDepthAtEntry = 0;
        // fix #1567 / §29.7/§28.10 (v0.182): disposeStack_ depth at this try's
        // entry (== the `unwindScopeDepth_`/`thisTryDepth` captured in lowerTry).
        // A `return`/`break`/`continue` routing through this finally disposes only
        // the try-BODY scopes (indices >= this depth) BEFORE the finally runs; the
        // ENCLOSING scopes (indices < this depth) are disposed AFTER the finally,
        // as the exit edge unwinds further outward — the same reverse-establishment
        // interleave the exception edge realizes via finUnwindBB +
        // emitUnwindDisposalsInRange. Without this, an enclosing `using var`
        // established before the try was disposed BEFORE the finally, a
        // use-after-dispose if the finally touches it.
        std::size_t disposeDepthAtEntry = 0;
        // #721 C1: an async method's `return` exits via emitAsyncComplete, not a
        // function `ret`, so its unwrapped result value can't ride retValSlot
        // (typed as the void Resume return). This slot (typed asyncResultTy_)
        // carries the value across the finally for exit mode 5; null for a
        // valueless `return` / a `Task` method / a non-async frame.
        llvm::AllocaInst* asyncRetSlot = nullptr;
        // §21.5 (v0.201, #2051): labeled `break`/`continue` that CROSS this
        // finally to reach an OUTER loop. The fixed modes 0-5 name only the
        // innermost-loop break/continue (mode 2/3), so a labeled jump to an
        // outer loop — or one with a loop between the target and this try — needs
        // its own exit mode carrying the true target block and scope-floor. Each
        // distinct (isBreak, target loop) routed through this finally gets a
        // fresh mode >= 6 (deduped); the dispatch switch emits a case per entry.
        struct LabeledRoute {
            int mode;                        // exit-mode value (>= 6)
            bool isBreak;
            llvm::BasicBlock* targetBlock;   // target loop's break/continue block
            std::size_t targetScopeDepth;    // dispose floor (target's scopeDepth)
            std::size_t targetLoopIdx;       // loopStack_ index — for the outer
                                             // frame's inside-loop test
        };
        std::vector<LabeledRoute> labeledRoutes;
        int nextLabeledMode = 6;
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

    // #821 / §29.7: per-yield-state → the `using var` (owned-resource) disposals
    // active at that yield, recorded as (decl-node, Disposal). The local is
    // spilled to the frame and restored in Dispose; the recorded decl-node lets
    // Dispose rebind the Disposal's slot to the restored alloca and run it. This
    // is the using-var analogue of smIterFinallyAtState_ — §29 says `using var`
    // compiles to try/finally, but the codegen tracks it via disposeStack_, so
    // the iterator Dispose needs this parallel record to honour §29.7's "Dispose
    // every using var active at the point of suspension."
    std::unordered_map<unsigned,
        std::vector<std::pair<const ast::Node*, Disposal>>>
        smIterDisposalsAtState_;

    // §29.7 (#944): per-yield-state → the frame field indices holding live
    // iterator-format enumerator/source frames (a for-in's GetEnumerator clone
    // and, for a loop-owned CallExpr/NewExpr iterable, its source IEnumERABLE)
    // active at that yield. Unlike a `using var` (a named LocalDecl restored via
    // localFields/locals_), these implicit for-in resources have no decl node, so
    // they are tracked by FRAME FIELD INDEX directly: the yield snapshot records
    // the index of each such resource live in the surrounding scope, and the
    // iterator Dispose (for a suspend state in this map) GEPs the field, loads the
    // frame ptr, null-guards, and disposes it via iterator-vtable slot 1 (then
    // nulls the field). This disposes the inner enumerator/source EXACTLY when the
    // enclosing iterator is abandoned while suspended inside the loop — the loop's
    // own scope-exit / break disposal handles every other exit (never both,
    // discriminated by the suspend state). Outermost-first; Dispose runs reverse.
    std::unordered_map<unsigned, std::vector<unsigned>> smIterEnumFramesAtState_;
    // §29.7 (#944): maps an iterator-format enumerator/source disposal slot
    // (an `iterEnumDispose` Disposal's alloca) → its frame field index, for the
    // for-in resources wired to a dedicated frame field in the current iterator
    // body. The yield snapshot consults this to record live frames in
    // smIterEnumFramesAtState_. Reset per iterator body.
    std::unordered_map<llvm::AllocaInst*, unsigned> smIterEnumFrameSlot_;

    // fix #1567 / §29.7/§28.10 (v0.182): per-yield-state → the ONE
    // establishment-ordered cleanup stack active at that yield. `using var`
    // disposals, for-in enumerator-frame disposals, and `finally` bodies form a
    // SINGLE establishment-ordered stack (§28.10: "one establishment-ordered
    // stack ... no edge runs the two kinds in separate category-ordered phases").
    // On early Dispose (§29.7) they run in exact REVERSE of this vector — a
    // `finally` established when the dispose-stack had depth d sits, in
    // establishment order, immediately before scope d (after every scope < d,
    // before every scope >= d), so it runs before the enclosing scopes' disposals
    // and after its own try-body's. The pre-v0.182 code ran all using-var
    // disposals, then all finally bodies (category phases), inverting the order
    // for a finally that touched an enclosing `using var` — a use-after-dispose.
    // This replaces the three category-split maps above for the SYNC iterator
    // Dispose; those maps remain for the async-iterator Dispose (its ordering is
    // tracked as a separate follow-up).
    struct IterCleanup {
        enum class Kind { Disposal, EnumFrame, Finally };
        Kind kind;
        // Kind::Disposal — the decl node (to rebind the restored alloca slot in
        // Dispose) and the Disposal action recorded at snapshot time.
        const ast::Node* node = nullptr;
        Disposal disposal{};
        // Kind::EnumFrame — the frame field index holding the live for-in
        // enumerator/source frame (disposed via iterator-vtable slot 1).
        unsigned frameField = 0;
        // Kind::Finally — the finally-body AST to re-run under Dispose.
        const std::vector<std::unique_ptr<ast::Stmt>>* finallyBody = nullptr;
    };
    std::unordered_map<unsigned, std::vector<IterCleanup>> smIterCleanupAtState_;

    // CG20: exception handling (explicit-unwinding model). A module-level
    // global holds the in-flight exception (null when none). A `throw` and
    // any call that leaves the global non-null branch to the current
    // unwind target — the enclosing try's catch dispatch, or (when none)
    // the function's propagate block, which returns so the caller's
    // post-call check unwinds further.
    llvm::GlobalVariable* excGlobal_ = nullptr;   // kept for compat; unused after #704
    llvm::Function*       excAddrFn_ = nullptr;   // fix #704: cached bz_exc_addr decl
    llvm::BasicBlock* unwindTarget_ = nullptr;   // innermost catch dispatch
    llvm::BasicBlock* propagateBB_ = nullptr;    // per-function, lazy
    // §22.9 (#739): when non-null, a property accessor (setter) body is being
    // INLINED at an assignment site; a `return;` in it ends the accessor by
    // branching here, NOT by emitting a function return (which would exit the
    // enclosing method or, unconditionally, leave a malformed block).
    llvm::BasicBlock* inlinedAccessorEnd_ = nullptr;
    // CG20: the exception value caught by the enclosing catch arm, so a
    // bare `throw;` can re-raise it (the global was cleared on arm entry).
    llvm::Value* currentCaughtExc_ = nullptr;
    // #397 / §31.1: the name the enclosing catch arm binds the caught
    // exception to ("" for a bare-type arm with no binding). Used by
    // emitThrowValue to detect a direct re-throw `throw ex;` (the one case
    // that must not free the caught exception when throwing from the arm).
    std::string_view currentCaughtName_;
    // #866 / §31.2: the unwind target that LEAVES the enclosing catch arm (the
    // arm's `unwindTarget_` at entry — the enclosing handler / finally / propagate
    // boundary). emitThrowValue's #397 free of the caught exception is sound ONLY
    // when the throw actually exits the arm (unwindBB() == this). When the throw
    // is caught by an INNER try/catch within the arm, unwindBB() is that inner
    // dispatch instead, the arm continues, and the caught exception must NOT be
    // freed (it stays in flight for the arm's own exit) — else a double-free.
    llvm::BasicBlock* caughtExcArmTarget_ = nullptr;
    // CG40: set true by lowering a `throw` expression (§31.2) — the
    // expression diverges (branches to the unwind target), leaving the
    // current block terminated. Phi-building consumers (conditional, `??`)
    // read this immediately after lowering a sub-expression to exclude a
    // diverged branch from the join.
    bool diverged_ = false;
    // #1467 / §22.4 / §28.3: scratch slots holding an `out owned`
    // (class/interface) value that a call passed via `out _` (discard). The
    // callee writes a fresh OWNED heap object into each such slot, but no
    // binding takes ownership — so §28.3 makes it an unbound owned temporary
    // that must be disposed at the end of the full statement (else the object
    // is neither disposed nor reclaimed → leak). buildOrderedArgs records the
    // slot here; lowerCall disposes+reclaims it (null-guarded, exactly once)
    // right after the call on the normal-return path. Used as a stack: each
    // lowerCall snapshots the size before buildOrderedArgs and consumes only
    // the entries it added, so a nested `M(out _, N(out _))` stays correct.
    std::vector<llvm::AllocaInst*> discardOwnedOutSlots_;
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
    // Shared no-op Dispose for every value-type box (record / primitive / span
    // box, §8.6). Built-in exceptions used to share this; #1033 gave them their
    // own string-releasing disposer (builtinExceptionDisposeFn_).
    llvm::Function* builtinDisposeFn_ = nullptr;
    // #1033 / §31.1 / §28.9: Dispose for built-in exceptions ONLY — releases
    // the owned Message@8 (and ParamName@32) string slots, then NULLs them so a
    // repeated dispatch (explicit .Dispose() + scope-end, the §28.6 split) is a
    // no-op. Value-type boxes keep the no-op builtinDisposeFn_ (they carry no
    // string at offset 8); this disposer is wired only into the built-in
    // exception vtables (slot 2).
    llvm::Function* builtinExceptionDisposeFn_ = nullptr;

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
    // specializedMethods_[(ms, llvm_subst_key, sema_id_key, param_tys)] =
    // specialized
    // LLVM function. per §14.2 (generics are monomorphized): the cache key must
    // distinguish instantiations that produce DIFFERENT specialized bodies. The
    // LLVM type vector alone is insufficient because every class/interface type
    // erases to `ptr` in IR, so `Maker<A>.Run` and `Maker<B>.Run` would share a
    // key `[ptr]` and the second would reuse the first's body — wrong when the
    // body contains a concrete-class-specific disposer (`using var a := new
    // T()`). The third element carries a stable per-type identity KEY for each
    // substituted type parameter, in the SAME deterministic order as the LLVM
    // type vector, so distinct concrete type args get distinct entries and
    // genuinely-identical instantiations still share one.
    // fix #1947: the key is the RECURSIVE structural encoding (typeRefIdentityKey
    // → appendDescKey), NOT the old per-declaration identity token. A single
    // declaration pointer collapsed a self-nested instantiation onto one entry —
    // `Foo<int>` and `Foo<Foo<int>>` (top decl `Foo` for both) shared a key AND
    // the erased `ptr` LLVM type, so the second reused the first's specialized
    // body (first-seen-wins mis-dispatch: an `is Foo<T>` in the body baked the
    // FIRST T's descriptor). The recursive key encodes every nested type
    // argument, so nested instantiations that share a top-level declaration get
    // DISTINCT specialized bodies (§14.2 monomorphization, §12.9 invariance).
    // fix #1894 #1895: the fourth element is the concrete LLVM PARAMETER type
    // vector. Two call sites can share the same type-argument substitution yet
    // pass arguments at DIFFERENT LLVM widths — e.g. `Make<long,long>(100,200)`
    // lowers the int literals as i32 while `Make<long,long>(x,y)` (x,y: long)
    // passes i64. The specialized function's signature is built from these
    // argument types (buildSpecializedMethodFn's paramTys), so without this
    // element the second call reuses the first's `(i32,i32)` function and the
    // `call` passes i64 args to i32 params — an IR-verification failure. Keying
    // on the param types mints one specialization per distinct signature (each
    // internally coerces its narrow params to the substituted field/return
    // widths), so both call sites get a correctly-typed callee.
    std::map<std::tuple<const sema::MethodSymbol*,
                        std::vector<llvm::Type*>,
                        std::vector<std::string>,  // fix #1947: recursive id keys
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
    // fix #628 / §9.5 / §14.2: when emitting a SPECIALIZED method body via
    // buildSpecializedMethodFn, currentFn_ is set to the freshly-created specFn
    // which is NOT registered in methodFns_.  lowerWith's §575 reverse-lookup
    // (fn == currentFn_ in methodFns_) therefore fails — t stays null and
    // `this with … end` returns the raw `this` pointer instead of a struct copy
    // → ret-type mismatch (IR verification failure).  currentSpecMethodSymbol_
    // carries the ORIGINAL MethodSymbol* being specialized so lowerWith can find
    // the parent TypeSymbol without requiring specFn to be in methodFns_.
    sema::MethodSymbol* currentSpecMethodSymbol_ = nullptr;

    // #817 / §14.2: true ONLY while emitting the UNSPECIALIZED generic TEMPLATE
    // body of a generic-class / generic method (emitFunctionBody). The template
    // is never the call target — every call binds the concrete substitution and
    // routes to a specialization — but it must still IR-verify. Its `T`-typed
    // field/local reads take the i32/`{i8,i32}` placeholder layout, which can
    // disagree with a CONCRETE signature return type (e.g. `GetConcrete():
    // Counter?` returning a `T?` field → `{i8,i32}` value vs `ptr` return).
    // When set, emitReturn replaces an irreconcilable return-type mismatch with a
    // type-safe `ret null` so the (dead) template verifies, without masking real
    // return-coercion bugs in non-generic methods or in the live specializations
    // (where this flag is false). Saved/restored false around a specialization.
    bool emittingGenericTemplateBody_ = false;

    // §19.3 (v0.182, #1852): the names of the generic parameters in scope while
    // emitting the UNSPECIALIZED template body (set alongside
    // emittingGenericTemplateBody_ in emitFunctionBody). In the dead template the
    // active `genericSubstSemaTy_` is empty, so a type-parameter pattern
    // (`case x of T`, `var x: T`) cannot resolve T to a concrete type; this set
    // lets patternMatches recognize such a name as a parameter and emit a
    // well-formed never-match (the template verifies but is never called), while
    // a genuinely-unsupported name still reaches the #1847 diagnostic.
    std::unordered_set<std::string_view> templateGenericParamNames_;

    // #383 / §28.8.1: true while emitting a class's compiler-generated Dispose
    // (a destructor body). A `using var u := move this._f;` inside a destructor
    // is NOT registered for disposal: the destructor's auto field-dispose pass
    // (emitOwnedFieldDisposals) already disposes _f, and codegen does not track
    // moved-from state, so registering the moved value too would double-free.
    // The field-restoration rule (§28.8.1) guarantees safety OUTSIDE a
    // destructor (the field is reassigned to null/a fresh object on every exit,
    // and field disposal is null-safe).
    bool emittingDisposerBody_ = false;

    // fix #1468 / §28.3 "Exception-path discharge" / §28.4: the in-flight
    // exception value captured at the entry of the CURRENT user destructor body
    // (null outside a destructor body, and null at runtime on the normal — non-
    // unwind — disposal path). When a destructor runs as an owned local/field is
    // disposed during stack unwinding, bz_exc already holds the PROPAGATING
    // exception before the body executes. A pending-check for an operation inside
    // the body (e.g. the §22.13 static-init guard emitted for a user static-
    // member touch) must fire only for a NEW exception that operation itself
    // threw — never for the pre-existing propagating one, which would abandon the
    // rest of the destructor and leak the object plus its whole owned subtree.
    // emitPendingCheck compares against this value (only while
    // emittingDisposerBody_) to make that distinction. Set/cleared around the
    // destructor body in emitDisposerCore; the load lives in the disposer's entry
    // block so it dominates every body block.
    llvm::Value* disposerEntryExc_ = nullptr;

    // Block-form property getter functions (§22.9 `get => do...end;`).
    // Keyed by PropertyDecl so the same getter function is reused across
    // multiple access sites (non-generic; generic getters are specialised).
    std::unordered_map<const ast::PropertyDecl*, llvm::Function*>
        propBlockGetterFns_;

    // #815 / §9.1: standalone getter functions for a positional record field
    // used as the implementation of an interface property (the itable slot).
    // `T get(ptr this)` — GEP the field in the record struct and load. Keyed by
    // the FieldSymbol so it is reused across interfaces/access sites.
    std::unordered_map<const sema::FieldSymbol*, llvm::Function*>
        posFieldGetterFns_;

    // Build (or return the cached) getter function for a positional record field
    // backing an interface property. Returns null if the field has no struct
    // slot. Mirrors ensurePropertyBlockGetter's function-creation boilerplate.
    llvm::Function* ensurePositionalFieldGetterFn(const sema::TypeSymbol* cls,
                                                  const sema::FieldSymbol* fs) {
        if (auto it = posFieldGetterFns_.find(fs);
            it != posFieldGetterFns_.end())
            return it->second;
        llvm::StructType* rst = classStruct(cls);
        auto idxIt = fieldIndex_.find(const_cast<sema::FieldSymbol*>(fs));
        if (!rst || idxIt == fieldIndex_.end()) return nullptr;
        llvm::Type* retTy = rst->getElementType(idxIt->second);
        auto* fnTy = llvm::FunctionType::get(retTy, {ptrTy()}, false);
        auto* fn = llvm::Function::Create(
            fnTy, llvm::GlobalValue::InternalLinkage,
            "bz$posfield$" + std::string(cls->name) + "$" +
                std::string(fs->name) + "$get",
            *module_);
        posFieldGetterFns_[fs] = fn;
        auto* entryBB = llvm::BasicBlock::Create(llvm_, "entry", fn);
        auto* savedFn = currentFn_;
        auto savedBB = builder_.GetInsertBlock();
        auto savedIP = savedBB ? builder_.GetInsertPoint()
                               : decltype(savedBB->begin()){};
        currentFn_ = fn;
        builder_.SetInsertPoint(entryBB);
        llvm::Value* thisPtr = &*fn->args().begin();
        llvm::Value* slot =
            builder_.CreateStructGEP(rst, thisPtr, idxIt->second);
        builder_.CreateRet(builder_.CreateLoad(retTy, slot));
        currentFn_ = savedFn;
        if (savedBB) builder_.SetInsertPoint(savedBB, savedIP);
        return fn;
    }

    // #730 / §23.4 / §13: standalone indexer accessor functions used for
    // INTERFACE-typed indexer dispatch (`(arr: IFoo)[i]`). The getter has the
    // signature `T get(ptr this, idx...)` and the setter `void set(ptr this,
    // idx..., value)`; both handle expression- and block-bodied accessors. Keyed
    // by PropertySymbol (the class's concrete indexer) so the same function fills
    // the itable slot and is reused across dispatch sites.
    std::unordered_map<const sema::PropertySymbol*, llvm::Function*>
        indexerGetterFns_;
    std::unordered_map<const sema::PropertySymbol*, llvm::Function*>
        indexerSetterFns_;

    // fix #2414 (§22.10/§23.4/§2.8.2 clause 1/§2.8.3 rule 4, v0.235): the
    // indexer analogs of importedPropGetterFns_/importedPropSetterFns_ below
    // — an EXTERNAL `bz$<Class>$idxget$<arity>`/`$idxset$<arity>` function
    // declared by declareImportedIndexerGetters/-Setters for a NON-VIRTUAL,
    // non-interface imported indexer (the exporter's twin,
    // emitExportedIndexerGetters/-Setters, wraps the existing
    // ensureIndexerGetterFn/-SetterFn body builders with ExternalLinkage).
    // Consulted by lowerIndexerGet/lowerIndexerSet's decl-null fallback —
    // an imported PropertySymbol has no ast::PropertyDecl (`ps->decl` is
    // always null, mirroring Property's own #503 limitation), so the usual
    // inline-body / itable-fill paths (which all read `pd`) cannot resolve
    // it directly.
    std::unordered_map<const sema::PropertySymbol*, llvm::Function*>
        importedIndexerGetterFns_;
    std::unordered_map<const sema::PropertySymbol*, llvm::Function*>
        importedIndexerSetterFns_;

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
    // §2.15 / issue #687: JsonOptions value struct
    // { i1 Indented, i32 IndentWidth, i1 AllowTrailingCommas,
    //   i1 IgnoreUnknownFields, i32 MaxDepth }.
    llvm::StructType* jsonOptionsStructTy_ = nullptr;
    // #422 / #672 / §27.5 / §28.9: the closure-cell type
    // { ptr fn, ptr env, i64 rc, ptr envdtor }.
    // envdtor (field 3, off 24) is a per-lambda typed disposal function emitted
    // beside the trampoline (#672 foundation); null = no env disposal (back-compat).
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
    // template-level one.  Keyed by (FieldSymbol*, ((llvm_types, identity_tokens),
    // typeArgMangle)) — the llvm types collapse class/interface to `ptr` and the
    // identity token is per-DECLARATION (so Box<int> and Box<string> share the
    // `Box` token); the recursive type-arg mangle string (#738) breaks that
    // collision for nested generic arguments.
    std::map<std::pair<const sema::Symbol*,
                       std::pair<std::pair<std::vector<llvm::Type*>,
                                           std::vector<const void*>>,
                                 std::string>>,
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
    // §9.6 (#946/#947/#948): per-class Equals/GetHashCode thunk caches (slot
    // kEqualsSlot / kGetHashCodeSlot) and the shared box-default thunks.
    std::unordered_map<const sema::TypeSymbol*, llvm::Function*> classEqualsFns_;
    std::unordered_map<const sema::TypeSymbol*, llvm::Function*> classHashCodeFns_;
    llvm::Function* boxDefaultEqualsFn_ = nullptr;
    llvm::Function* boxDefaultHashCodeFn_ = nullptr;
    // §9.1 / §9.6 (#961): per-type structural Equals/GetHashCode thunks for
    // boxed records, enums, and primitives. Installed at kEqualsSlot /
    // kGetHashCodeSlot of the box vtable so an Object-typed receiver gets
    // value-semantics equality and hash, not reference identity.
    std::unordered_map<const sema::TypeSymbol*, llvm::Function*> boxRecordEqualsFns_;
    std::unordered_map<const sema::TypeSymbol*, llvm::Function*> boxRecordHashFns_;
    std::unordered_map<std::string, llvm::Function*> primBoxEqualsFns_;
    std::unordered_map<std::string, llvm::Function*> primBoxHashFns_;
    // §8.6 / §12.7 (#417): every vtable / box-vtable / exception-vtable global
    // emitted, collected so emitVtableTable() can publish a null-terminated
    // array `bz$vtable$table` that bz_obj_is_string scans to tell a string (no
    // vtable at word 0) from a class/box (vtable at word 0) in an Object slot.
    std::vector<llvm::GlobalVariable*> allVtables_;
    // The published null-terminated vtable table (lazily emitted).
    llvm::GlobalVariable* vtableTableGV_ = nullptr;
    // §8.6 / §12.7 (#417): emit (once) and return the bz$vtable$table global.
    llvm::Value* vtableTablePtr();
    // #1843 / §8.6 #417: replace the vtableTablePtr placeholder with the real,
    // complete table once every (incl. lazily-created specialized) vtable exists.
    void finalizeVtableTable();
    // §33.3 / #1366: null-terminated table of just the BUILT-IN exception
    // vtables. bz_unhandled_exception scans it to decide whether the uncaught
    // object has the §31.4 built-in layout (message@8, CauseChain@24) before
    // reading those slots — a user class thrown uncaught (any layout) is
    // reported by type name only, never by an out-of-bounds field read.
    llvm::GlobalVariable* excVtableTableGV_ = nullptr;
    llvm::Value* builtinExcVtableTablePtr();
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
    // fix #2400 (§2.8.2/§2.8.3 rule 4, §11.2, §23; v0.235): the SETTER twin of
    // importedPropGetterFns_ — no equivalent existed before this fix, which is
    // why a settable property inherited from an imported base class had no
    // cross-module ABI surface at all (only a getter stub was ever declared).
    // Keyed by the imported PropertySymbol; the value is an ExternalLinkage
    // `void(ptr this, T value)` declaration linking against the exporter's
    // setter function (`bz$<Class>$<Prop>$propset`). Populated by
    // declareImportedPropertySetters.
    std::unordered_map<const sema::PropertySymbol*, llvm::Function*>
        importedPropSetterFns_;
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
    // fix #2506 (§23.3/§28.9, v0.236): TOTAL number of fn-ptr slots in every
    // itable built for this interface — the running `idx` value at the end of
    // the methods+events+properties indexing loop above (methods + sync events +
    // one getter per property + one setter per settable property). Every
    // itable$C$I / itable$box$R$I appends ONE trailing "owner-kind" flag word at
    // exactly this index (non-null = CLASS-backed retaining setter, null =
    // box/record-backed plain-store setter), which lowerVirtualPropertySet's
    // itable branch reads to decide whether to ARGUMENT-PASS release a +1
    // producer: the call site cannot see the concrete runtime receiver's kind
    // from `ps->parent` (always the Interface), but the flag travels INSIDE the
    // itable bytes the concrete type's own module produced, so it is sound
    // cross-module regardless of vtable/descriptor linkage. Captured for both
    // local and imported interfaces (the import mirror below runs the identical
    // indexing loop). Method-only box itables (prim/span/value-builtin) omit
    // the flag: their interfaces declare no settable property, so a setter
    // dispatch never indexes into them and the absence is never observed.
    std::unordered_map<const sema::TypeSymbol*, unsigned> ifaceItabSlotCount_;
    // Per (class × interface): the itab$C$I global (array of fn ptrs in
    // interface declaration order). Key: (cls, iface).
    std::map<std::pair<const sema::TypeSymbol*, const sema::TypeSymbol*>,
             llvm::GlobalVariable*>
        itables_;
    // §24.7 (#477 B7): per-class event-accessor functions (emit-once cache),
    // keyed by the class's own instance EventSymbol.
    std::unordered_map<const sema::EventSymbol*, llvm::Function*>
        eventAccessorFns_;
    // fix #1139 / §11.2 nullability-widening: adapter thunks for itable slots
    // where the implementing method declares T? and the interface declares T.
    // Keyed by (ifaceMethod, implMethod) pair; generated once per pair.
    std::map<std::pair<const sema::MethodSymbol*, const sema::MethodSymbol*>,
             llvm::Function*>
        nullableWideningThunks_;
    // #1592 / §12.1: per virtual-slot FAMILY (keyed by the introducing,
    // non-override virtual declaration — see introducingVirtualMethod), which
    // 0-based parameter positions are widened (base `T` → some override's
    // `T?`, VALUE type) by ANY contributor anywhere in the whole class
    // hierarchy reachable from the family's root. Memoized: computed once per
    // root MethodSymbol by scanning ctx_.symbols() (mirrors the existing
    // whole-program subclass scan in emitVirtualGenericDispatch).
    std::unordered_map<const sema::MethodSymbol*, std::vector<bool>>
        vtableFamilyWidenedParams_;
    // #1592: adapter thunks presenting a widened family's uniform (boxed)
    // ABI for a contributor whose OWN declared parameter is still the narrow
    // (un-widened) form — the base's own un-overridden body, or a sibling
    // override that never widened. Keyed by the contributor's own
    // MethodSymbol (its narrow body); generated once per contributor.
    std::unordered_map<const sema::MethodSymbol*, llvm::Function*>
        vtableWideningThunks_;
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
    // §4.16 / §8.6 (#970): per-element-LLVM-type span box vtable. A span
    // value boxed into IReadOnlySpan<T> uses this vtable + a synthesized itable.
    // Keyed by the concrete element LLVM type (i32 for int, ptr for string, etc.)
    // so each element-specialization shares one vtable even when the four span
    // names differ.  Populated lazily by ensureSpanBoxVtable().
    std::unordered_map<llvm::Type*, llvm::GlobalVariable*> spanBoxVtables_;
    llvm::GlobalVariable* ensureSpanBoxVtable(llvm::Type* elemTy);

    llvm::Type* i8() { return llvm::Type::getInt8Ty(llvm_); }
    llvm::Type* i16() { return llvm::Type::getInt16Ty(llvm_); }
    llvm::Type* i32() { return llvm::Type::getInt32Ty(llvm_); }
    llvm::Type* i1() { return llvm::Type::getInt1Ty(llvm_); }
    llvm::Type* i64() { return llvm::Type::getInt64Ty(llvm_); }
    llvm::PointerType* ptrTy() { return llvm::PointerType::get(llvm_, 0); }
    // fix #1720 (§32.5, v0.186): is `t` the FFI-boundary `RawBytes` type
    // (nullable or not)? A RawBytes parameter marshals as two consecutive C
    // slots (pointer, then length), so both the extern declaration
    // (declareExterns) and the call site (lowerExternCall) special-case it —
    // shared here so the two stay in lockstep.
    static bool isRawBytesFfiType(const ast::Type* t) {
        return t && t->kind == ast::TypeKind::Named && t->name &&
               t->name->parts.size() == 1 &&
               t->name->parts.front() == "RawBytes";
    }
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
    // v0.160: `out owned var` replaces the former bare `out owned` call-site
    // form (§22.4); both are by-reference. per §22.4
    static bool isByRefMod(std::string_view m) {
        return m == "ref" || m == "out" || m == "out var" ||
               m == "out owned" || m == "out owned var" || m == "out using var";
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

    // per §33.1/§2.4 (#1563): a static `Main` is the native process entry
    // point only when this compilation unit IS the executable's main module
    // (isMainModule_). In a library build (§33.6: "a library module has no
    // entry point") — which is also how a dependency module is compiled,
    // §2.5 — a same-shaped `Main` is an ordinary method: no native `main`
    // symbol, no argc/argv ABI, no process-boundary unwind handling. Not
    // `static` (was, pre-#1563) since it now reads instance state.
    bool isMain(sema::MethodSymbol* ms) const {
        return isMainModule_ && ms->name == "Main" && isStaticMethod(ms);
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

    // #1080 / §23.4: a virtual/override/abstract (or interface) INDEXER needs a
    // vtable slot so `base[i]` and `this[i]` dispatch to the most-derived getter
    // — the indexer analog of isVirtualProperty (the non-indexer guard there
    // deliberately excludes indexers, which historically had no slot).
    // #2449: marked `const` (pure predicate over `ps`, no Emitter state
    // touched) so the const `findIndexer`/`indexerCandidateWireable` below
    // can call it.
    bool isVirtualIndexer(const sema::PropertySymbol* ps) const {
        if (!ps || !ps->isIndexer) return false;
        for (auto mod : ps->modifiers) {
            if (mod == "static") return false;
            if (mod == "virtual" || mod == "override" || mod == "abstract")
                return true;
        }
        return ps->parent && ps->parent->kind == ast::TypeDeclKind::Interface;
    }

    // #2449 (§11.2/§23.4, v0.235): can `ps` actually be installed as an
    // interface-indexer itable slot? A LOCAL indexer (real `ast::PropertyDecl`)
    // always can — `ensureIndexerGetterFn`/`-SetterFn` build its body from
    // the AST. An IMPORTED indexer (`ps->decl == nullptr` — ModuleLoader
    // reconstructs no AST, mirrors Property's #503 limitation) can ONLY
    // through #2414's non-virtual external-stub ABI
    // (`declareImportedIndexerGetters`/`-Setters`,
    // `bz$<Class>$idxget$<arity>`/`$idxset$<arity>`) — those two functions
    // themselves skip a VIRTUAL or interface-declared indexer (dispatches
    // through a vtable/itable slot instead, not this direct-call ABI), so
    // this predicate excludes the identical shape rather than reporting a
    // candidate that would silently find no entry in
    // importedIndexerGetterFns_/-SetterFns_ (see `indexerItableAccessor`,
    // LlvmBackend_Setup.cpp, which resolves the actual function once a
    // candidate passes this gate). `findIndexer` (LlvmBackend_Assign.cpp)
    // is the sole caller.
    bool indexerCandidateWireable(const sema::PropertySymbol* ps) const {
        if (!ps) return false;
        if (dynamic_cast<const ast::PropertyDecl*>(ps->decl)) return true;
        if (ps->decl) return false;  // defensive: not a PropertyDecl either
        return !isVirtualIndexer(ps);
    }

    // #481 / §23: does this property declare a setter? An auto-property without
    // a `readonly`/`private set` restriction has both; an expression-bodied or
    // block getter-only property has only a getter. Checked off the PropertyDecl
    // so it matches across base/override (the override must match the base's
    // accessor set per §12.1).
    //
    // fix #2406 (§23 / §11.2 / §2.8.2 / §2.8.3 rule 4, v0.235): `ps->decl` is
    // unconditionally null for an IMPORTED PropertySymbol (ModuleLoader never
    // reconstructs an ast::PropertyDecl for a body-less imported declaration).
    // Called on an IMPORTED INTERFACE's own PropertySymbol (to decide whether
    // declareVtables's Pass 0/its import-mirror should reserve an itable
    // setter slot at all, and whether Pass D should attempt to fill one),
    // this used to unconditionally return `false`, so NO setter slot was ever
    // reserved for a read-write property declared on an imported interface —
    // `b.Name := v` through such a reference silently no-op'd even when the
    // implementing class and its setter were entirely local (#2400's own
    // declareImportedPropertySetters comment flags this exact gate as "filed
    // separately" — this is that follow-up).
    //
    // Fall back to `ctx_.importedPropertyHasSetter(ps)` — the MetaMember::
    // hasSetter bit #2400 added (ModuleMetadata.hpp) and already threads
    // builder -> loader -> this same SemaContext side table for its own
    // (non-interface, direct cross-module set) shape. `ps->modifiers` alone is
    // NOT sufficient here: on a BLOCK-form property, `readonly` is a compile
    // error whether or not a `set` arm is present (Bezel_Language_Spec.md
    // §22.3/§23.1, v0.235 — "on a block form declaring a setter it is
    // contradictory and likewise an error", and equally an error on a
    // get-only block form since the shape already expresses read-onlyness),
    // so a get-only block-form interface property and a read-write auto-form
    // interface property are INDISTINGUISHABLE by modifiers alone — a naive
    // "no `readonly`" fallback would wrongly reserve a setter slot for the
    // former, and Pass D would fill it with ConstantPointerNull (no real
    // setter exists to find), planting the #2387 null-itable-slot SIGSEGV
    // shape on any write attempt. The real hasSetter bit (computed at EXPORT
    // time from the actual PropertyDecl shape) has no such ambiguity.
    bool propertyHasSetter(const sema::PropertySymbol* ps) {
        auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl);
        if (!pd) return ctx_.importedPropertyHasSetter(ps);
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

    // §10.3 (#740): an enum's underlying-type primitive (default `int`) and the
    // matching LLVM integer type / signedness. The enum representation is the
    // underlying integer; these route every enum-value site to the correct
    // width instead of the historical hardcoded i32.
    sema::PrimitiveType enumUnderlyingPrim(const sema::TypeSymbol* enumTy) {
        if (enumTy && enumTy->enumUnderlyingType)
            return enumTy->enumUnderlyingType->primitive;
        return sema::PrimitiveType::Int;
    }
    llvm::Type* enumLlvmType(const sema::TypeSymbol* enumTy) {
        return lowerPrimitive(enumUnderlyingPrim(enumTy));
    }
    bool enumIsUnsigned(const sema::TypeSymbol* enumTy) {
        return isUnsignedPrim(enumUnderlyingPrim(enumTy));
    }
    // The owning enum TypeSymbol of an enum-constant symbol, or null.
    const sema::TypeSymbol* enumOfConstant(const sema::Symbol* m) {
        if (auto* ec = dynamic_cast<const sema::EnumConstantSymbol*>(m))
            return ec->parent;
        return nullptr;
    }

    // per §8.4/§10.3 (#1189): the signedness of a resolved TypeRef. An enum is
    // represented by its underlying integer (§10.3), so a Named-kind TypeRef
    // whose declaration is an enum carries the signedness of that underlying
    // primitive — not "signed by default". Routing the enum case through
    // enumIsUnsigned lets relational comparison of an unsigned-backed enum
    // (byte/ushort/uint/ulong) select the unsigned predicate (icmp ult) instead
    // of the signed one (icmp slt), which read values above the signed midpoint
    // as negative.
    bool typeRefIsUnsigned(const sema::TypeRef* rt) {
        if (!rt) return false;
        if (rt->kind == sema::TypeRef::Kind::Primitive)
            return isUnsignedPrim(rt->primitive);
        if (rt->kind == sema::TypeRef::Kind::Named && rt->declaration &&
            rt->declaration->kind == ast::TypeDeclKind::Enum)
            return enumIsUnsigned(rt->declaration);
        return false;
    }

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

    // §4.14 / §4.15: test whether a TypeRef is one of the span-like value types
    // that share the bz.arrayspan layout { ptr source, i32 start, i32 length }.
    // Covers the borrowing spans over Array<T> (ArraySpan/ReadOnlyArraySpan,
    // §4.15) AND the owning spans over OwningBuffer<T> (Span/ReadOnlySpan, §4.14).
    // All four reuse the same shared-surface lowering (indexer get/set, Slice,
    // IndexOf, Contains, SequenceEqual, Length/IsEmpty, for-in); the owning
    // class-T behaviour (disposing set, `move`, Clear/MoveFrom/MoveTo) is gated
    // separately on isOwningSpanTypeName.
    static bool isSpanLikeTypeName(const sema::TypeRef* rt) {
        if (!rt || rt->kind != sema::TypeRef::Kind::Named || rt->declaration)
            return false;
        if (!rt->astNode || !rt->astNode->name || rt->astNode->name->parts.empty())
            return false;
        auto n = rt->astNode->name->parts[0];
        return n == "ArraySpan" || n == "ReadOnlyArraySpan" ||
               n == "Span" || n == "ReadOnlySpan";
    }

    // §4.16 / §29.2 (#970/#1262): the synthesized IReadOnlySpan<T> builtin
    // interface — a span boxed into { vtable, rc, bz.arrayspan } (arrayspan at
    // box+16). Recognised by the IReadOnlySpan TypeSymbol declaration or, for a
    // not-yet-resolved ref, the leading name part.
    static bool isIReadOnlySpanType(const sema::TypeRef* rt) {
        if (!rt || rt->kind != sema::TypeRef::Kind::Named) return false;
        if (rt->declaration && rt->declaration->name == "IReadOnlySpan")
            return true;
        if (rt->astNode && rt->astNode->name &&
            !rt->astNode->name->parts.empty() &&
            rt->astNode->name->parts[0] == "IReadOnlySpan")
            return true;
        return false;
    }

    // §4.14 (#779): test whether a TypeRef is the OWNING `Span<T>` over an
    // OwningBuffer<T> (its slots own their class-typed elements). `ReadOnlySpan<T>`
    // is a read-only view (no set/move/clear), so it is NOT matched here.
    static bool isOwningSpanTypeName(const sema::TypeRef* rt) {
        if (!rt || rt->kind != sema::TypeRef::Kind::Named || rt->declaration)
            return false;
        if (!rt->astNode || !rt->astNode->name || rt->astNode->name->parts.empty())
            return false;
        return rt->astNode->name->parts[0] == "Span";
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

    // §2.15 / issue #687: JsonOptions value struct.
    // Layout: { i1 Indented, i32 IndentWidth, i1 AllowTrailingCommas,
    //           i1 IgnoreUnknownFields, i32 MaxDepth }
    // Field indices:  0=Indented, 1=IndentWidth, 2=AllowTrailingCommas,
    //                 3=IgnoreUnknownFields, 4=MaxDepth
    llvm::StructType* jsonOptionsStructType() {
        if (!jsonOptionsStructTy_)
            jsonOptionsStructTy_ = llvm::StructType::create(
                llvm_,
                {i1(), i32(), i1(), i1(), i32()},
                "bz.json_options");
        return jsonOptionsStructTy_;
    }

    // §2.15 / #687: helpers to extract the individual JsonOptions fields from
    // a lowered JsonOptions struct value (held in a register or an alloca slot).
    // Each helper stores the struct into a temp alloca and GEPs the field.
    llvm::Value* jsonOptIndented(llvm::Value* opts) {
        auto* st = jsonOptionsStructType();
        auto* slot = entryAlloca(st);
        builder_.CreateStore(opts, slot);
        llvm::Value* v = builder_.CreateLoad(i1(),
            builder_.CreateStructGEP(st, slot, 0), "jopt.indented");
        return builder_.CreateZExt(v, i8(), "jopt.indented8");
    }
    llvm::Value* jsonOptIndentWidth(llvm::Value* opts) {
        auto* st = jsonOptionsStructType();
        auto* slot = entryAlloca(st);
        builder_.CreateStore(opts, slot);
        return builder_.CreateLoad(i32(),
            builder_.CreateStructGEP(st, slot, 1), "jopt.iwidth");
    }
    llvm::Value* jsonOptAllowTrailing(llvm::Value* opts) {
        auto* st = jsonOptionsStructType();
        auto* slot = entryAlloca(st);
        builder_.CreateStore(opts, slot);
        llvm::Value* v = builder_.CreateLoad(i1(),
            builder_.CreateStructGEP(st, slot, 2), "jopt.trailing");
        return builder_.CreateZExt(v, i8(), "jopt.trailing8");
    }
    llvm::Value* jsonOptIgnoreUnknown(llvm::Value* opts) {
        auto* st = jsonOptionsStructType();
        auto* slot = entryAlloca(st);
        builder_.CreateStore(opts, slot);
        llvm::Value* v = builder_.CreateLoad(i1(),
            builder_.CreateStructGEP(st, slot, 3), "jopt.ignunk");
        return builder_.CreateZExt(v, i8(), "jopt.ignunk8");
    }
    llvm::Value* jsonOptMaxDepth(llvm::Value* opts) {
        auto* st = jsonOptionsStructType();
        auto* slot = entryAlloca(st);
        builder_.CreateStore(opts, slot);
        return builder_.CreateLoad(i32(),
            builder_.CreateStructGEP(st, slot, 4), "jopt.maxdepth");
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

    // #422 / #672 / §27.5 / §28.9 / §27.7.1 (#1544): the closure-cell heap layout —
    //   { ptr fn (off 0), ptr env (off 8), i64 rc (off 16), ptr envdtor (off 24),
    //     i64 kind (off 32) }
    //   = 40 bytes.
    // fn (off 0) and env (off 8) keep the offsets the existing closure-call sites
    // use (fn at offset 0, env at GEP index 1); rc (field index 2, off 16) is
    // unchanged; envdtor (field index 3, off 24) is the #672 typed env-disposer;
    // kind (field index 4, off 32) is NEW (#1544): a FnValKind tag recording which
    // §27.7.1 equality rule the value obeys, consulted only by
    // bz_closure_value_equals / bz_closure_same (never by retain/release). The
    // values MUST agree with bezel_runtime_internal.hpp's BZ_FVK_* constants.
    // Every closure-cell producer calloc's this size and stores rc := 1,
    // envdtor := null, and kind := the producer-appropriate FnValKind.
    enum FnValKind : int64_t {
        // Lambda or value-shaped-receiver (record/enum/primitive) ref: equals
        // only itself (reference identity on the cell). §27.7.1 first & third.
        FVK_REF_IDENTITY = 0,
        // Class/interface-receiver method/property ref: same fn + same receiver.
        // §27.7.1 second ("guaranteed equal" across re-evaluations).
        FVK_FN_PLUS_RECV = 1,
        // Static / unbound-extension method/property ref: same fn. §27.7.1 second.
        FVK_FN_ONLY = 2,
    };
    llvm::StructType* closureCellType() {
        if (!closureCellTy_)
            closureCellTy_ = llvm::StructType::create(
                llvm_, {ptrTy(), ptrTy(), i64(), ptrTy(), i64()}, "bz.closure");
        return closureCellTy_;
    }

    // #422: store rc := 1 into a freshly calloc'd closure cell (field index 2).
    // The producer holds the first reference (§28.9 runtime-managed lifetime).
    void initClosureRc(llvm::Value* cellObj) {
        builder_.CreateStore(
            llvm::ConstantInt::get(i64(), 1),
            builder_.CreateStructGEP(closureCellType(), cellObj, 2));
    }

    // #672: store envdtor := null into a freshly calloc'd closure cell (field 3).
    // Foundation: all producers start with a null envdtor (no env disposal yet).
    // The follow-up activation lane will populate this for closures that retain
    // owned captures.  callocFn zero-fills the allocation so this is technically
    // redundant, but the explicit store makes the intent clear and guards against
    // any future switch away from calloc.
    void initClosureEnvdtor(llvm::Value* cellObj) {
        builder_.CreateStore(
            llvm::ConstantPointerNull::get(ptrTy()),
            builder_.CreateStructGEP(closureCellType(), cellObj, 3));
    }

    // §27.7.1 (#1544): store the function-value KIND tag into a freshly calloc'd
    // closure cell (field index 4, off 32). Records which equality rule the value
    // obeys so bz_closure_value_equals / bz_closure_same can dispatch on it. The
    // default (calloc zero) is FVK_REF_IDENTITY; this explicit store documents the
    // producer's intent and pins the value independently of the allocator.
    void initClosureKind(llvm::Value* cellObj, FnValKind kind) {
        builder_.CreateStore(
            llvm::ConstantInt::get(i64(), static_cast<int64_t>(kind)),
            builder_.CreateStructGEP(closureCellType(), cellObj, 4));
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

    // fix #1747 / §28.9 / §9.7 / §28.3: GEP every string storage member of
    // record `ts` at struct ADDRESS `recAddr` (the slot in a plain-`var`
    // local), load the field's current string pointer, bz_string_release
    // (null-/sentinel-safe), and null the slot so a re-read sees a defined
    // null (matching the boxRelease / closureRelease / stringRelease
    // semantics). Used by the recordStringFields scope-end Disposal.
    //
    // fix #2405 / §9.7 / §28.9: RECURSE into a non-nullable nested-record
    // storage member so a string owned two-or-more levels deep is also
    // released, not just the outer record's OWN direct string fields (the
    // #1747 one-level scope this helper originally had). A nested record
    // is embedded INLINE (records are value types, §9), so its storage sits
    // at a GEP off the SAME outer struct address `recAddr` -- no load/
    // indirection needed to reach it, exactly like reaching a direct string
    // field's slot. The recursion is guaranteed to terminate: sema rejects
    // a record that transitively contains itself by value at the
    // declaration site (TypeResolution_Checks.cpp's recordValueContainsSelf,
    // "` cannot recursively contain itself by value" §9), so the nested-
    // record type graph reachable from any `ts` is a finite DAG -- no cycle
    // guard is needed, mirroring emitStructEquality's identical unconditional
    // recursion into nested-record fields for `=`/`<>` (LlvmBackend_Layout.cpp).
    // A NULLABLE nested-record field (`P?`) is excluded here, matching
    // isRecordWithStringFieldsTypeRef's own PRE-EXISTING top-level
    // exclusion (unchanged by this fix): it lowers to a {i8, P} wrapper
    // struct, not the bare record layout this walker (and
    // recordHasStringFields, below) assume.
    //
    // fix #2425 / §9.8 / §28.9 / §35.4 v0.235: a record component may also
    // be a value-like CHOICE (§9.8 record-admissibility: "a value-like
    // serialization-eligible choice" is a record-admissible component type,
    // matching §9.2's admission of value-like choices as equality
    // components). Recurse into a non-nullable choice-typed storage member
    // exactly as into a nested record: the choice struct is likewise
    // embedded INLINE (§35.1 — a choice is a value type regardless of its
    // §35.4 classification), so a GEP off this SAME outer struct address
    // reaches it, and the recursion terminates for the same reason (§35.1's
    // choice self-containment check, TypeResolution_Checks.cpp's
    // checkChoiceSelfContainment/recordValueContainsSelf, rejects a choice
    // that transitively contains itself through value-typed payloads — the
    // combined record+choice value-type graph reachable from any
    // declaration is a finite DAG regardless of which kind starts the
    // walk). The actual per-case, tag-switched release lives in
    // emitChoiceStringFieldReleases_impl (LlvmBackend_Disposal.cpp) — the
    // choice-side counterpart this comment's prior revision (#2405) noted
    // was missing.
    void emitRecordStringFieldReleases_impl(llvm::Value* recAddr,
                                            const sema::TypeSymbol* ts) {
        if (!recAddr || !ts || ts->kind != ast::TypeDeclKind::Record) return;
        llvm::StructType* st = recordStruct(ts);
        if (!st) return;
        std::vector<const sema::Symbol*> storage;
        collectRecordStorage(ts, storage);
        for (const sema::Symbol* m : storage) {
            auto idxIt = fieldIndex_.find(m);
            if (idxIt == fieldIndex_.end()) continue;
            if (isStringStorageMember(m)) {
                llvm::Value* addr =
                    builder_.CreateStructGEP(st, recAddr, idxIt->second,
                                              "recstrf.addr");
                llvm::Value* val =
                    builder_.CreateLoad(ptrTy(), addr, "recstrf.val");
                emitStringRelease(val);
                builder_.CreateStore(
                    llvm::ConstantPointerNull::get(ptrTy()), addr);
                continue;
            }
            const sema::TypeRef* mty = recordMemberType(m);
            if (mty && !mty->nullable &&
                mty->kind == sema::TypeRef::Kind::Named && mty->declaration &&
                mty->declaration->kind == ast::TypeDeclKind::Record &&
                recordHasStringFields(mty->declaration)) {
                llvm::Value* nestedAddr = builder_.CreateStructGEP(
                    st, recAddr, idxIt->second, "recstrf.nested.addr");
                emitRecordStringFieldReleases_impl(nestedAddr, mty->declaration);
                continue;
            }
            // fix #2425: a nested value-like CHOICE component (see the
            // function comment above).
            if (mty && !mty->nullable &&
                mty->kind == sema::TypeRef::Kind::Named && mty->declaration &&
                mty->declaration->kind == ast::TypeDeclKind::Choice &&
                choiceHasStringFields(mty->declaration)) {
                llvm::Value* nestedAddr = builder_.CreateStructGEP(
                    st, recAddr, idxIt->second, "recstrf.nested.ch.addr");
                emitChoiceStringFieldReleases_impl(nestedAddr, mty->declaration);
                continue;
            }
            // fix #2326 / §9.8 v0.228: a non-nullable INLINE-ARRAY field
            // (e.g. `[N]string`, `[N]RecWithAStringField`, `[M][N]string`) whose
            // element type carries a string. The array is a value type
            // embedded INLINE in the record (§15.1), so its first slot sits
            // at a GEP off the SAME outer struct address `recAddr` — exactly
            // like reaching a direct string field or a nested record's
            // storage. The per-element walker (emitInlineArrayStringReleases_impl)
            // dispatches on each element's static type: `string` → release;
            // non-nullable record → recurse; non-nullable choice → choice
            // walker; non-nullable inline array → recurse. The call-result
            // / deserialize-disposal-scope registrations gate on
            // inlineArrayElementCarriesString, so this is reached only when
            // a release is owed. The element-type recursion is finite for
            // the same reason as the named record/choice arms: sema rejects
            // a self-containing value-type graph at the declaration site.
            if (mty && !mty->nullable &&
                mty->kind == sema::TypeRef::Kind::InlineArray &&
                mty->arrayElement &&
                inlineArrayElementCarriesString(mty->arrayElement)) {
                llvm::Value* arrAddr = builder_.CreateStructGEP(
                    st, recAddr, idxIt->second, "recstrf.arr.addr");
                emitInlineArrayStringReleases_impl(arrAddr, mty);
            }
        }
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
    // R33: slice a string by a Range *value* (a %bz.range struct in a variable).
    llvm::Value* emitStringRangeSliceValue(llvm::Value* self,
                                           llvm::Value* rangeVal);
    // Shared slice mechanics given the resolved [start64, endExcl) endpoints.
    llvm::Value* emitStringSliceBytes(llvm::Value* self, llvm::Value* start64,
                                      llvm::Value* endExcl);

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

    // §18.5 (v0.184, #1691): apply ONLY the alignment portion of an encoded
    // InterpPart::formatSpec (a `,WIDTH` prefix; any trailing `:FORMAT` is
    // ignored) to an already-rendered bz_string. Used for the null-hole
    // "null" text: per §18.5 v0.184 the rendered text `null` "participates
    // in field padding" exactly as any other rendered text, but the FORMAT
    // spec (if any) is never dispatched on a null value. Returns `strVal`
    // unchanged when `spec` carries no alignment prefix or the width is 0.
    llvm::Value* emitAlignOnly(llvm::Value* strVal, const std::string& spec);

    // §18.5 (v0.172, #812/#1339): dispatch a hole's format spec to a user type's
    // IFormattable.ToString(format): string. Returns null if `ts` has no such
    // method (caller falls back). Handles class (ptr) and record (value)
    // receivers via emitMethodCall.
    llvm::Value* emitIFormattableDispatch(llvm::Value* val,
                                          const sema::TypeSymbol* ts,
                                          const std::string& spec);
    // §18.5 (v0.172): raise a catchable FormatException for an unrecognized
    // specifier on an intrinsically-formattable operand (the specifier is a
    // compile-time constant in interpolation, so the throw is unconditional).
    // Returns a sentinel empty string keeping the CFG well-formed.
    llvm::Value* emitAlwaysFormatException();

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
    // §35.7 (v0.210, #2083): per-value equality and hashing over a choice
    // struct. Both compare the i32 tag first; on tag match they descend
    // into the active case's payload struct field-by-field using the
    // case's payload types (so a payload's record recurses structurally,
    // a payload's string compares by content). Inactive-case payload
    // slots are NOT compared — their bits are `undef` in freshly
    // constructed values (§35.13 implementation-defined layout, and
    // struct storage order is unordered for inactive unions). Dispatched
    // from emitValueEquality/emitValueHashCode when a field's declared
    // type is a Choice, so a record can hold a choice component and the
    // synthesized Equals/GetHashCode remains consistent.
    llvm::Value* emitChoiceEquality(llvm::Value* l, llvm::Value* r,
                                    const sema::TypeSymbol* choiceTy);
    llvm::Value* emitChoiceHashCode(llvm::Value* val,
                                    const sema::TypeSymbol* choiceTy);
    // §35.7 (#2479): "TypeName.CaseName(field1: value1, …)" / "TypeName.
    // CaseName" over the choice struct's active case, mirroring
    // emitChoiceEquality/emitChoiceHashCode's tag-switch shape. Callers must
    // confirm auto-equality-bearing (sema::isAutoEqualityBearingChoice)
    // first — this function does not re-check it.
    llvm::Value* emitChoiceToString(llvm::Value* val,
                                    const sema::TypeSymbol* choiceTy);
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

    // #1075: the value-semantics dispatch core, taking an ALREADY-LOWERED,
    // non-nullable receiver value + type. Factored out of
    // tryLowerValueSemanticsCall so the null-conditional path
    // (lowerNullCondValueSemCall) can reuse it with the unwrapped inner value of
    // a nullable value-type receiver. Returns the GetHashCode (i32) / Equals
    // (i1) / record-or-enum ToString result, or null if not handled (a primitive
    // ToString is left to the dedicated string path).
    llvm::Value* lowerValueSemMethodOn(const sema::TypeRef* recvTy,
                                       llvm::Value* recv,
                                       const ast::CallExpr& call,
                                       std::string_view member);

    // §7.5 / #1075: lower `valueRecv?.ToString()/GetHashCode()/Equals(x)` on a
    // NULLABLE value-type/enum/record receiver — `hasValue ? wrap(method(inner))
    // : null` — mirroring the §7.5 lift the MethodSymbol path applies. `recv` is
    // the receiver's nullable {i8 hasValue; T} struct.
    llvm::Value* lowerNullCondValueSemCall(const ast::CallExpr& call,
                                           const ast::NullCondMemberExpr& nm,
                                           const sema::TypeRef* recvTy,
                                           llvm::Value* recv, bool isToString,
                                           bool isGetHashCode, bool isEquals);

    // §9.8 / §2.14–2.15 (issue #550): BinarySerializer/BinaryDeserializer
    // construction, ToBytes, AsReadOnlySpan, and synthesized Serialize/
    // Deserialize on records and enums.  Returns the lowered value, or
    // nullptr if the call is not a serialization-related call (or is void).
    // Use isSerializationCall() to distinguish "void result" from "not ours".
    llvm::Value* tryLowerSerializationCall(const ast::CallExpr& call);
    // Returns true iff the call should be handled by tryLowerSerializationCall.
    bool isSerializationCall(const ast::CallExpr& call,
                             const ast::MemberAccessExpr& ma);

    // fix #1397 / §2.14: the ISerializer write/structure surface, forwarded to
    // the bz_binser_* runtime.  Shared by the concrete BinarySerializer /
    // JsonSerializer receivers and the ISerializer interface receiver so a
    // direct `ser.WriteInt(x)` on a concrete serializer moves data (previously
    // only the ISerializer-receiver path was wired).  `ser` is the (opaque)
    // serializer pointer.  Returns true iff `methodName` named a write/structure
    // method and a call was emitted (all such methods are void).
    bool lowerSerializerWriteMethod(llvm::Value* ser,
                                    std::string_view methodName,
                                    const ast::CallExpr& call);
    // fix #1397 / §2.14: the IDeserializer read/structure surface, forwarded to
    // the bz_binder_* runtime.  Shared by the concrete BinaryDeserializer /
    // JsonDeserializer receivers and the IDeserializer interface receiver.
    // `der` is the (opaque) deserializer pointer.  Sets `handled` iff
    // `methodName` named a read/structure method; returns the lowered value
    // (nullptr for the void methods EndObject/EndArray/SkipValue).
    llvm::Value* lowerDeserializerReadMethod(llvm::Value* der,
                                             std::string_view methodName,
                                             const ast::CallExpr& call,
                                             bool& handled);

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

    // §9.8 choice serialization (v0.200, #2046) — the `$case`-discriminated
    // object form for a value-like choice.
    // Serialize: switch on the active-case tag; each arm writes an object whose
    // leading field is `"$case"` (the case's declared name or its `serialized
    // "<name>"` rename), followed by the active case's payload fields by name.
    // `choiceVal` is a loaded choice struct value.
    void emitChoiceSerialize(const sema::TypeSymbol* ts,
                             llvm::Value* choiceVal,
                             llvm::Value* serPtr);
    // Deserialize: read the `$case` discriminator FIRST (bz_binder_read_case_
    // discriminator), dispatch to select the case, read the payload fields, and
    // build the choice struct.  Returns the constructed choice struct value.
    llvm::Value* emitChoiceDeserialize(const sema::TypeSymbol* ts,
                                       llvm::Value* derPtr);

    // §9.8 tier 2 — class synthesis (tree shape).
    // Emit the Serialize body for a `serialized tree`/`graph`/`cyclic` class.
    // `polyDiscriminator`: when non-empty, adds a leading "$type" field with
    // this value to the serialized object — used by polymorphic dispatch (§9.8
    // tier 4) when the concrete type is known statically at each dispatch branch.
    void emitClassSerialize(const sema::TypeSymbol* ts,
                            llvm::Value* objPtr,
                            llvm::Value* serPtr,
                            std::string_view polyDiscriminator = {});
    // Emit the Deserialize body for a `serialized tree`/`graph`/`cyclic` class.
    // Returns an owned heap pointer (ptr) to the newly constructed instance.
    llvm::Value* emitClassDeserialize(const sema::TypeSymbol* ts,
                                      llvm::Value* derPtr);

    // §9.8 tier 4 — polymorphic registry dispatch synthesis.
    // Emits runtime type dispatch for a `serialized * polymorphic(...)` base class:
    //   Serialize:   reads vtable type-descriptor, compares against each registered
    //                concrete type, writes "$type" + body for the match; raises
    //                SerializationException for an unregistered dynamic type.
    //   Deserialize: reads "$type" discriminator via bz_binder_read_poly_discriminator,
    //                compares against each registered discriminator string, dispatches
    //                to that type's emitClassDeserialize; raises SerializationException
    //                for an unknown discriminator.
    void emitPolyClassSerialize(const sema::TypeSymbol* baseTs,
                                llvm::Value* objPtr,
                                llvm::Value* serPtr);
    llvm::Value* emitPolyClassDeserialize(const sema::TypeSymbol* baseTs,
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

    // §6.6/§5.2 (#1961): is the compound-assignment/increment target a
    // (non-nullable) `char` lvalue? A `char` LHS desugars `c OP= e` to
    // `c := char(c OP e)`, so the write-back must apply the §5.2 code-point
    // narrowing — emitCompoundOp cannot see the char-ness (char lowers to i32
    // exactly like int/uint), so the caller supplies this flag.
    bool isCharTargetExpr(const ast::Expr& e);

    // CG31: the address (and element type) of an assignable target — a
    // local/parameter slot, a static-field global, a `this.f`/`obj.f`
    // field, or a bare implicit-`this` field. Null if not assignable.
    llvm::Value* lvalueAddr(const ast::Expr& target, llvm::Type*& elemTy);

    // CG31: apply a compound-assignment/increment arithmetic or bitwise
    // operator (op without the trailing `=`). Integer `+ - *` and `/ mod`
    // are overflow/zero-checked exactly as in lowerBinary; bitwise/shift
    // are exact; floats use IEEE ops.
    // `rUns` (#2140) is the RHS operand's OWN signedness — needed only when the
    // §6.2-promoted result is WIDER than the LHS (`int += long`, `byte += long`,
    // `uint += ulong`): the arithmetic runs in the wider operand's type, so each
    // operand extends to that width by its own signedness and the op takes the
    // wider operand's. Defaults to the LHS signedness's role being sufficient
    // (callers whose RHS is never wider than the LHS — `++`/`--`, an already-
    // width-matched RHS — leave it), so existing call sites are unaffected.
    llvm::Value* emitCompoundOp(const std::string& op, llvm::Value* l,
                                llvm::Value* r, bool fp, bool uns,
                                bool charTarget = false, bool rUns = false);

    // §5.2 (#1961): apply the char code-point narrowing check to an integer
    // value — valid U+0000–U+10FFFF excluding the surrogate range
    // U+D800–U+DFFF, otherwise OverflowException — and return the value as the
    // `char` (i32) type. Shared by the explicit `char(n)` cast
    // (lowerPrimCastScalar) and the §6.6 char compound-assignment/increment
    // write-back (emitCompoundOp) so both enforce the identical §5.2 contract.
    llvm::Value* emitCharRangeCheck(llvm::Value* payload);

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
    // fix #2447 / §35.9 / §7.2: is rt a NULLABLE CHOICE (a nullable Named
    // TypeRef whose declaration is a choice)? "`C?` is the ordinary
    // nullable value type (§7.2)" (§35.9, "Nullable `C?`") — a choice's
    // classification (value-like/borrow-bearing/owning, §35.4) does not
    // change this: the WRAPPER `C?` is always the tagged {i8 hasValue; T}
    // struct, exactly like a nullable record. Before this fix, no
    // predicate recognized a nullable choice as a nullable value type at
    // all — lowerTypeRef's `isNullableValueTy` gate returned false, so a
    // `Wrap?` field fell through to the LATER Named/Choice branch, which
    // returns the BARE (non-nullable) choiceStruct regardless of
    // `rt->nullable`, contradicting its own comment ("the nullable `C?`
    // wraps this struct in nullableValueStructTy's `{ i8 hasValue,
    // <choice struct> }`") — no such wrap ever happened. This produced a
    // choice-typed value with no null representation, which crashed
    // (an ICmp i32-vs-i8 type-mismatch assertion) the first time code
    // tried to test its (non-existent) hasValue tag — confirmed via
    // serializing a record with a `Wrap?` component. This predicate is
    // the choice analogue of isNullableRecordTy; the two are unified by
    // isNullableValueTy below. Defined in LlvmBackend_Layout.cpp (needs
    // the choice-declaration kind check, like isNullableRecordTy).
    bool isNullableChoiceTy(const sema::TypeRef* rt);
    // per §2.5–§2.6 / issue #1954: is rt a NULLABLE VALUE-BUILTIN (a nullable
    // Named TypeRef naming DateTime/TimeSpan/DateOnly/TimeOnly/Guid)? These are
    // scalar/struct value types whose decl kind is Class (or declaration-less), so
    // none of isNullablePrimTy/isNullableEnumTy/isNullableRecordTy recognise them —
    // a `Guid?` therefore fell through to the bare {i64,i64} lowering with NO null
    // representation, and the nullable-wrapper GEP over it was malformed (LLVM
    // checkGEPType abort). It now lowers, like records, to the tagged
    // {i8 hasValue; <builtin payload>} struct (nullableValueStructTy), so `null` is
    // representable and distinct from every value — critically for the MULTI-FIELD
    // Guid {i64,i64}. Defined in LlvmBackend_Layout.cpp (delegates to
    // valueBuiltinName); the four unify under isNullableValueTy below.
    bool isNullableValueBuiltinTy(const sema::TypeRef* rt);
    // A nullable VALUE type — a value primitive (isNullablePrimTy), an enum
    // (isNullableEnumTy), a record (isNullableRecordTy), a CHOICE
    // (isNullableChoiceTy, fix #2447), or a value-builtin
    // (isNullableValueBuiltinTy, #1954). All lower to the tagged {i8 hasValue; T
    // value} struct; the only difference is T (the primitive's LLVM type, i32 for
    // an enum, the bare record struct, the bare choice struct, or the
    // value-builtin's lowering — i64 for DateTime/TimeSpan, i32 for
    // DateOnly/TimeOnly, {i64,i64} for Guid). All null/wrap/value-semantics
    // sites key off this combined predicate.
    bool isNullableValueTy(const sema::TypeRef* rt) {
        return isNullablePrimTy(rt) || isNullableEnumTy(rt) ||
               isNullableRecordTy(rt) || isNullableChoiceTy(rt) ||
               isNullableValueBuiltinTy(rt);
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
    // §5.1 / §5.2 (#743): materialize the bz_decimal for an integer source into
    // `outSlot`, honouring UNSIGNED source types. A `ulong` above long.MaxValue
    // must use the unsigned 64-bit conversion (not from_int64, which would read
    // it as negative); a `uint` above int.MaxValue zero-extends to i64 (always
    // positive) before from_int64. byte/ushort/char are always < int.MaxValue,
    // so the signed i32 path is exact for them.
    void emitIntToDecimal(llvm::Value* v, sema::PrimitiveType p,
                          llvm::Value* outSlot) {
        using P = sema::PrimitiveType;
        auto* voidTy = llvm::Type::getVoidTy(llvm_);
        if (v->getType()->isIntegerTy(64)) {
            const char* fn = (p == P::ULong) ? "bz_decimal_from_uint64"
                                             : "bz_decimal_from_int64";
            builder_.CreateCall(declareC(fn, voidTy, {i64(), ptrTy()}),
                                {v, outSlot});
            return;
        }
        if (p == P::UInt) {
            llvm::Value* v64 = builder_.CreateZExt(v, i64());
            builder_.CreateCall(
                declareC("bz_decimal_from_int64", voidTy, {i64(), ptrTy()}),
                {v64, outSlot});
            return;
        }
        llvm::Value* v32 = v;
        if (!v->getType()->isIntegerTy(32)) {
            bool uns = (p == P::Byte || p == P::UShort || p == P::Char);
            v32 = uns ? builder_.CreateZExt(v, i32())
                      : builder_.CreateSExt(v, i32());
        }
        builder_.CreateCall(
            declareC("bz_decimal_from_int32", voidTy, {i32(), ptrTy()}),
            {v32, outSlot});
    }
    // #813 / §15.1: wrap a bare inner value (or a null literal) into a
    // nullable-value slot struct `{i8 hasValue, T value}` when the destination
    // LLVM slot type IS such a struct and `v` is not already that struct. This
    // mirrors the general assignment path (LlvmBackend_Stmts.cpp, the
    // "CG-nullable" block) so the fast paths that bypass it — inline `[N]T?`
    // element assignment (`a[i] := v`), inline-array literal assignment, and the
    // inline-array literal initializer — store a properly-tagged value rather
    // than the raw inner bytes (which clobber the hasValue byte). Returns `v`
    // unchanged when the slot is not a nullable-value struct or `v` is already it.
    llvm::Value* coerceToNullableSlot(llvm::Type* elemTy, llvm::Value* v) {
        auto* st = elemTy ? llvm::dyn_cast<llvm::StructType>(elemTy) : nullptr;
        if (!st || st->getNumElements() != 2 ||
            st->getElementType(0) != nullablePrimHasValTy() || !v ||
            v->getType() == elemTy)
            return v;
        if (v->getType()->isPointerTy() &&
            llvm::isa<llvm::ConstantPointerNull>(v))
            return llvm::ConstantAggregateZero::get(elemTy);
        if (v->getType() == st->getElementType(1)) {
            llvm::Value* res = builder_.CreateInsertValue(
                llvm::UndefValue::get(elemTy),
                llvm::ConstantInt::get(nullablePrimHasValTy(), 1), {0u});
            return builder_.CreateInsertValue(res, v, {1u});
        }
        return v;
    }

    // v0.171 (#1336) — the Try-convention writeback. Every extraction
    // `TryX(out item: T?)` builtin must assign the out slot on BOTH exit paths
    // (§22.3): `null` on the false path, the extracted value on the true path.
    // For a nullable VALUE instantiation (Stack<int>, Queue<int>,
    // Dictionary<K,int>, …) the slot is the tagged `{i8 hasValue, T}` struct;
    // for a class/ref instantiation it is a plain nullable pointer.
    //   storeTryOutNull  — write the null form (getNullValue covers both:
    //                       `{i8 0, T 0}` for the tagged struct, a null pointer
    //                       for a ref slot, 0 for a legacy non-nullable scalar).
    //   storeTryOutValue — write the present form, wrapping a raw non-nullable
    //                       value into `{i8 1, T}` when the slot is the tagged
    //                       struct, else storing the raw value/pointer directly.
    void storeTryOutNull(llvm::Value* outAddr, llvm::Type* outTy) {
        builder_.CreateStore(llvm::Constant::getNullValue(outTy), outAddr);
    }
    void storeTryOutValue(llvm::Value* outAddr, llvm::Type* outTy,
                          llvm::Value* raw, bool isValTy) {
        if (isValTy && outTy && outTy->isStructTy()) {
            auto* nst = llvm::cast<llvm::StructType>(outTy);
            raw = reinterpretToType(raw, nst->getElementType(1));
            llvm::Value* res = builder_.CreateInsertValue(
                llvm::UndefValue::get(nst),
                llvm::ConstantInt::get(nullablePrimHasValTy(), 1), {0u});
            res = builder_.CreateInsertValue(res, raw, {1u});
            builder_.CreateStore(res, outAddr);
        } else {
            builder_.CreateStore(raw, outAddr);
        }
    }
    // §11.8 / stdlib §2.19 (#1470): store a TryParse out-result. The per-type
    // CONCRETE member's out is a BARE value (stdlib §9.1: `out result: int`), so
    // a direct `int.TryParse(s, out var b)` slot is a plain scalar and the bare
    // store below is correct. Dispatch through IParsable<T> instead uses the
    // INTERFACE member `out value: T?` (§2.19): when `T.TryParse(s, out v)`
    // forwards its result into a generic `out T?` parameter monomorphized to a
    // value type, the caller's out slot is the nullable companion struct
    // `{i8 hasValue; T value}`. Detect that shape from the slot's element type
    // and marshal the value into field 1 with hasValue = ok (the v0.171
    // nullable-out convention: the false path reads as null). Without this the
    // bare-value store landed at offset 0 — the hasValue byte + padding — leaving
    // the value field at offset 4 uninitialized (garbage on force-unwrap).
    void storeTryParseOut(llvm::Value* outAddr, llvm::Type* et,
                          llvm::Value* value, llvm::Value* ok) {
        if (auto* st = et ? llvm::dyn_cast<llvm::StructType>(et) : nullptr) {
            if (st->getNumElements() == 2 &&
                st->getElementType(0) == nullablePrimHasValTy()) {
                llvm::Type* innerTy = st->getElementType(1);
                // Layout-identical record/struct mismatch (monomorphized body)
                // then any residual numeric-width mismatch — both no-ops when the
                // parsed value already has the companion's inner type.
                value = reinterpretToType(value, innerTy);
                if (value->getType() != innerTy)
                    value = coerceNumericToType(value, innerTy);
                llvm::Value* hv =
                    builder_.CreateZExt(ok, nullablePrimHasValTy(), "tryp.hv");
                llvm::Value* nv = builder_.CreateInsertValue(
                    llvm::UndefValue::get(st), hv, {0u});
                nv = builder_.CreateInsertValue(nv, value, {1u}, "tryp.nv");
                builder_.CreateStore(nv, outAddr);
                return;
            }
        }
        builder_.CreateStore(value, outAddr);
    }
    // Is the receiver's element/value sema type a nullable VALUE type? (i.e. a
    // `Stack<int>`/`Dictionary<K,int>`-style value instantiation whose `T?` out
    // slot is the tagged `{i8,T}` struct rather than a nullable pointer.)
    // `genericIdx` selects the element (0) or dictionary value (1) argument.
    bool tryOutIsNullableValue(const sema::TypeRef* recvTy,
                               std::size_t genericIdx) {
        if (!recvTy || recvTy->genericArgs.size() <= genericIdx ||
            !recvTy->genericArgs[genericIdx])
            return false;
        sema::TypeRef nn = *recvTy->genericArgs[genericIdx];
        nn.nullable = true;
        return isNullableValueTy(&nn);
    }

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
    // §23.2/§23.3 (#1168) / §7.1 (#541): coerce an expression-bodied property
    // getter result `v` to the property's declared nullable-VALUE type `declTy`
    // (`int?`, `bool?`, …) — exactly as a method return / lambda body does. When
    // `declTy` is `T?` and `v` is the bare inner `T` the getter body yielded, a
    // `null` pointer literal becomes the all-zero null struct and a bare `T`
    // (after any numeric widening, §5.1) is wrapped to `{i8 1, T}`. Returns `v`
    // unchanged when it is already the `{i8,T}` optional or `declTy` is not a
    // nullable value type. Without this the getter read returns a bare `T`;
    // unwrapping it (`??`, `!`) does CreateExtractValue on a non-struct and
    // asserts in LLVM (#1168).
    llvm::Value* wrapNullableGetterResult(const sema::TypeRef* declTy,
                                          llvm::Value* v) {
        if (!v || !declTy || !isNullableValueTy(declTy)) return v;
        auto* expectedSt = nullableValueStructTy(declTy);
        if (v->getType() == expectedSt) return v;  // already wrapped
        if (v->getType()->isPointerTy() &&
            llvm::isa<llvm::ConstantPointerNull>(v))
            return nullableValueNull(declTy);
        llvm::Type* payloadTy = expectedSt->getElementType(1);
        if (v->getType() != payloadTy)
            v = coerceNumericToType(v, payloadTy);
        if (v->getType() == payloadTy)
            v = wrapNullableValue(declTy, v);
        return v;
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

    // #914 / §7.4 / §12.7: a value-type (primitive) nullable local that sema
    // proved non-null by null-flow narrowing is assignable to / returnable as a
    // non-nullable target (sema kept its declared {flag,value} layout but
    // accepted the coercion). When `v` is exactly that tagged-optional struct
    // and `targetTy` is its non-nullable value element, extract the value;
    // otherwise return `v` unchanged.
    //
    // PRECONDITION (#2614 / #2618) — this keys on NULLABILITY (the {flag,value}
    // shape), NOT on narrowing. It cannot distinguish a sema-proved-non-null
    // read from a genuinely-nullable one, so it is sound ONLY at a site where a
    // sema gate already rejects the un-narrowed form. This comment previously
    // asserted the opposite as a global invariant ("fires ONLY for an accepted
    // narrowed read; a genuinely-nullable source is rejected by sema") and that
    // was false: `Span<T>.Slice`, `string.AsSpan` and the `new Atomic<T>(...)`
    // NewExpr argument each had NO nullability gate, so applying the unwrap
    // there turned a loud verifyModule ICE into a SILENT WRONG VALUE — the
    // payload of a possibly-null box. Those three gates now exist
    // (checkBuiltinSpanCallArgs, checkBuiltinStringCallArgs' intPos set, and
    // the Atomic NewExpr check).
    //
    // Before adding a call site: confirm sema rejects the un-narrowed form
    // there, and ship a reject control asserting the diagnostic TEXT. If no
    // gate exists and cannot be added, leave the ICE -- it is strictly better
    // than a silent miscompile.
    llvm::Value* unwrapNarrowedNullable(llvm::Value* v, llvm::Type* targetTy) {
        if (!v || !targetTy) return v;
        if (auto* st = llvm::dyn_cast<llvm::StructType>(v->getType()))
            if (st->getNumElements() == 2 &&
                st->getElementType(0) == nullablePrimHasValTy() &&
                st->getElementType(1) == targetTy)
                return nullablePrimValue(v);
        return v;
    }

    // #2614 / §7.4 / §12.7: the same unwrap, applied at a BUILTIN lowering site
    // that needs a bare scalar operand, keyed on the argument expression's own
    // sema type rather than on a target LLVM type the caller must supply.
    //
    // A builtin's parameter list is synthetic: its arguments are lowered ad hoc
    // by each intrinsic's lowering code, so they never pass through the
    // user-method argument binding that already performs this unwrap
    // (LlvmBackend_Assign.cpp ~7897). When such an argument is a §7.4
    // flow-narrowed nullable value, sema types the READ as the non-nullable
    // primitive T (the null-check proved it present) while codegen still loads
    // the declared `{i8 hasVal; T payload}` box — and the site's subsequent
    // trunc/sext on a struct is malformed IR that verifyModule rejects.
    //
    // The guard is two-sided, and deliberately NOT the §7.11 binary-operator
    // one (LlvmBackend_Exprs.cpp ~6025). That path keys on a NON-nullable sema
    // type because its operand is a lifted arithmetic RESULT, which sema
    // retypes. A narrowed *read* is different: sema leaves the expression's
    // declared type as `T?` and records the narrowing in checkAssignable
    // instead (see LlvmBackend_Assign.cpp ~7897), so the operand here arrives
    // with sema type `int?`. Hence:
    //   * the lowered value must have the tagged-optional {i8 hasVal; T} shape,
    //     AND
    //   * the argument's sema type must be a nullable PRIMITIVE
    //     (isNullablePrimTy) — never a record, choice, enum, or value builtin,
    //     whose boxes can share the {i8, ...} shape but are not this family.
    //
    // ⚠⚠ SOUNDNESS PRECONDITION — READ BEFORE ADDING A CALL SITE. This helper
    // fires on NULLABILITY (the box shape + a nullable-primitive sema type). It
    // does NOT and CANNOT test that sema proved the value non-null: a narrowed
    // read keeps its `T?` sema type, so narrowed and un-narrowed are
    // indistinguishable here. Extracting the payload is therefore correct ONLY
    // at a site where a SEMA GATE already rejects the un-narrowed form. Where no
    // such gate exists, this helper reads the payload of a possibly-null box and
    // converts a loud verifyModule ICE into a SILENT WRONG VALUE — strictly
    // worse than the bug it fixes. The first version of #2614 did exactly that
    // at three ungated sites (`Span/ArraySpan.Slice`, `string.AsSpan`), which is
    // why this warning is here.
    //
    // The established pattern in this family (#2601/#2602) is a PAIR: sema
    // REJECTS the un-narrowed form, codegen UNWRAPS the narrowed one. Current
    // gates, all in src/sema/:
    //   * string int params  → checkBuiltinStringCallArgs' `intPos` set
    //                          (ExprTypecheck_Calls.cpp), incl. AsSpan (#2614)
    //   * ReadOnlyStringSpan and span-buffer `Slice`
    //                        → checkBuiltinSpanCallArgs (#2601, #2614)
    // Adding a call site without first extending a gate is a defect, not an
    // optimization. A no-op for every ordinary scalar operand (no struct →
    // returns `v` untouched).
    llvm::Value* unwrapNarrowedNullableOperand(llvm::Value* v,
                                               const ast::Expr* e) {
        if (!v || !e) return v;
        auto* st = llvm::dyn_cast<llvm::StructType>(v->getType());
        if (!st || st->getNumElements() != 2 ||
            st->getElementType(0) != nullablePrimHasValTy())
            return v;
        if (!isNullablePrimTy(ctx_.exprType(*e))) return v;
        return nullablePrimValue(v);
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
    // #1767 / §19.2: `sourceIsNullableRef` — the RETURNED expression's own sema
    // type is a nullable REFERENCE (`Object?`), not the value-nullable target.
    // Set only by the return path, it admits a RUNTIME `ptr` (a PHI/select, not
    // just a ConstantPointerNull) as the {i8 0, T 0} null constant. See the
    // definition's comment for the soundness argument (sema admits a reference-
    // nullable value into a value-nullable slot ONLY when provably null).
    llvm::Value* coerceReturnToNullableValue(llvm::Value* v,
                                             const sema::TypeRef* declRet,
                                             bool srcUnsigned = false,
                                             bool sourceIsNullableRef = false);

    // §11.2 / §23.4 (#2437/#2442, v0.235): the indexer PropertySymbol on `ts`
    // — or an inherited base class — whose FULL parameter list (arity AND
    // every parameter type, §23.4: "Indexers may be overloaded by parameter
    // type list") matches interface indexer `ifaceIdx`'s. Replaces the prior
    // first-parameter-only match (#774), which was arity-blind: it matched a
    // `[i:int,j:int]` obligation against a narrower `[i:int]` overload
    // whenever the two shared a first parameter type (#2437, confirmed:
    // silently installed the wrong getter). Also now walks `baseClassOf`
    // (#2442), mirroring `classPropertyByName`'s identical walk for named
    // properties — a class satisfying the obligation only through an
    // inherited indexer is a valid candidate. A candidate is considered only
    // if it carries a genuine LOCAL `ast::PropertyDecl`: `ensureIndexerGetterFn`/
    // `-SetterFn` require one unconditionally, so an indexer reached only
    // through an IMPORTED base class cannot be wired into the itable today —
    // selecting one here would report a match this function's own callers
    // then silently fail to install (see the .cpp definition). Returns null
    // when no member, own or inherited, matches — never a partial/best-
    // effort candidate (no more "fall back to the first indexer found"):
    // `checkAbstractImplementations` (sema, TypeResolution_Checks.cpp)
    // mirrors this exact selection, so null here always corresponds to a
    // compile-time reject, never a silently-wrong itable install.
    const sema::PropertySymbol* findIndexer(
        const sema::TypeSymbol* ts,
        const sema::PropertySymbol* ifaceIdx) const;

    // §23.4 (#1172): select the indexer OVERLOAD for the access `ix` by its
    // FULL parameter list — the candidate's parameter count must equal the
    // number of index arguments AND each parameter type must structurally match
    // the corresponding argument (an exact full-list match is preferred; a sole
    // arity-matching candidate is accepted, leaving implicit conversion to the
    // per-argument coercion at the call site). Candidates are gathered over the
    // receiver type and its base types (an inherited indexer is a valid
    // candidate), mirroring the sema selector in ExprTypecheck_Exprs.cpp.
    // Returns null when no candidate's arity matches. Unlike findIndexer
    // (matched against a fixed interface indexer's DECLARED parameter list
    // for §11.2 conformance, with no implicit-conversion fallback), this
    // selects against a real call site's ARGUMENT expressions and accepts an
    // arity-only match for implicit conversion at the call.
    const sema::PropertySymbol* selectIndexer(
        const sema::TypeSymbol* ts, const ast::IndexExpr& ix) const;

    // §22.10/§23.4 (#747 R42): resolve the receiver TypeSymbol of an indexer
    // access `target[i]`. Normally the target's sema exprType carries the
    // declaration; but `this[i]` inside an instance method has no sema exprType
    // (the member resolver uses the current type directly), so recover the owner
    // TypeSymbol from the current method, mirroring lowerWith. Null when the
    // receiver is not a user-defined type.
    const sema::TypeSymbol* indexReceiverDecl(const ast::Expr* target);

    // §22.10: emit the getter of an indexer property via inline body.
    // Returns the loaded value, or null if no indexer applies.
    llvm::Value* lowerIndexerGet(const ast::IndexExpr& ix);

    // §22.10: emit the setter of an indexer property via inline body.
    // Returns true if the setter was handled. `valueExpr` is the original
    // (pre-lowering) RHS expression on `as->value.get()` — used to gate the
    // paired call-site retain (#2540) so only the named-local RHS shapes the
    // walker armed for scope-end release get the balancing +1 on the
    // record-backed itable path (mirrors #2535's lowerVirtualPropertySet
    // `ifaceNamedLocalEligible` gate). nullptr is a safe no-op (no retain
    // emitted) — used by the ref-arg write-back path
    // (emitAccessorRefArgWriteBacks) where the value arrives pre-loaded and
    // has no associated source expression the walker could have armed.
    bool lowerIndexerSet(const ast::IndexExpr& ix, llvm::Value* val,
                         const ast::Expr* valueExpr = nullptr);

    // §20 / §23.4: compound assignment `a[i] OP= v` through a user-defined
    // indexer — evaluates the receiver and each index param ONCE, calls the
    // getter to read the current value, applies OP, then calls the setter to
    // write the result back. Returns true if handled (user-defined indexer
    // target); false to let the caller fall through to other paths.
    bool lowerIndexerCompound(const ast::AssignmentStmt& as);

    // fix #508 / stdlib §4.4 / §4.5 / §17.15: compound assignment `arr[i] OP= v`
    // on a HEAP-collection Array<T>/OwningArray<T>/Dictionary<K,V> indexer.
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

    // fix #682/#1079 / stdlib §4.4 / spec §16.3 (v0.182, #1579): the shared
    // Range-slice logic behind `arr[r]` (an Array<T> indexer get whose index
    // is Range-typed) AND the named `Array<T>.Slice(r)` method — spec §16.3
    // states the two are the compiler-recognized-equivalent forms of the
    // same operation. `recv` is the already-lowered Array<T> pointer;
    // `recvTy` its sema TypeRef (for the element size); `idxExpr` the
    // Range-valued argument expression (either `ix.indices[0].value` or a
    // `.Slice(...)` call's sole argument). Returns the sliced (fresh, owned)
    // `bz_array_t*`, or null if `idxExpr` is not Range-typed (the scalar-
    // index case; callers fall through to their own int-index path).
    llvm::Value* lowerArrayRangeSlice(llvm::Value* recv,
                                      const sema::TypeRef* recvTy,
                                      const ast::Expr& idxExpr);

    // issue #409 / language spec §28.8.1 / §28.11: lower `move coll[i]` (the
    // slot-take) on an OWNING heap collection (OwningArray / OwningDictionary).
    // Reads the element out as owned AND leaves the slot moved-from (null) so the
    // collection's later element-disposer skips it — no #409 double-free. Returns
    // null if `ix` is not an owning-collection slot (caller falls through to the
    // borrowing-read lowering, which does NOT null the slot).
    llvm::Value* lowerMoveFromCollectionSlot(const ast::IndexExpr& ix);

    // issue #388 / stdlib §4.4-§4.9: lower a HEAP-collection indexer setter
    // `a[i] := v` / `dict[key] := v`. For an OWNING collection (OwningArray /
    // OwningDictionary) the displaced slot/value is disposed exactly once before
    // the new (owned) one is stored (§4.5 / §4.9); a borrowing collection just
    // overwrites. `val` is the already-lowered RHS. Returns true if handled.
    // `valIsFreshPlusOneString` (fix #2596): when true, the caller guarantees
    // `val` is ALREADY a +1 string producer (e.g. a compound-assignment OP-
    // result from emitStringConcat, which always mints rc=1 per #422), so the
    // borrowing-Dictionary<K,string> co-ownership retain (§28.9 / #429) is
    // SKIPPED — the +1 transfers into the slot. Only the dict compound path
    // sets this; the plain-set path derives +1-ness from `valueExpr`.
    bool lowerCollectionIndexerSet(const ast::IndexExpr& ix, llvm::Value* val,
                                   const ast::Expr* valueExpr = nullptr,
                                   bool valIsFreshPlusOneString = false);

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

    // §23.1 (#743): a STATIC auto-property — like isAutoProperty but for the
    // `static` form. Its backing storage is a module-level global (instance
    // auto-properties use field storage); declareStatics allocates it and
    // staticFieldGlobal routes the implicit get/set there.
    bool isStaticAutoProperty(const sema::Symbol* m) {
        auto* ps = dynamic_cast<const sema::PropertySymbol*>(m);
        if (!ps || ps->isIndexer) return false;
        auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl);
        if (!pd || pd->isIndexer ||
            pd->bodyKind != ast::PropertyBodyKind::Auto)
            return false;
        for (auto mod : ps->modifiers)
            if (mod == "static") return true;
        return false;
    }

    // fix #2478 (§9.2/§22.8): isAutoProperty above is DECL-DEPENDENT
    // (`dynamic_cast<const ast::PropertyDecl*>(ps->decl)`) — it always
    // returns `false` for an IMPORTED PropertySymbol (`ps->decl` is null for
    // every imported symbol, ModuleLoader.cpp never reconstructs an
    // ast::PropertyDecl). This was a confirmed live memory-safety hazard:
    // `collectInstanceStorage`/`collectRecordStorage` below call
    // `isAutoProperty` to decide whether a member occupies a struct slot, so
    // an imported class/record with a PUBLIC (or now-#2456/#2468-visible
    // private) auto-property was silently under-counted — a locally-declared
    // derived class's own field then collided with the physical offset the
    // exporter's own compiled functions use for a LATER member (confirmed:
    // a derived class's own private field read back the BASE auto-property's
    // sibling field's value instead of its own, 122 instead of the correct
    // 66 in the discovering repro). This helper is the STORAGE-LAYOUT-ONLY
    // fallback: try the local (decl-based) predicate first (unchanged
    // behavior for every LOCAL declaration); when that is false AND the
    // symbol is an imported (decl-null) Property, consult the wire-carried
    // classification (`ctx_.importedPropertyIsAutoStorage`, set from
    // MetaMember::isAutoProperty by ModuleLoader.cpp) instead of silently
    // treating it as "not storage".
    //
    // SCOPE (CLAUDE.md rule 4 — deliberately narrow, not a blanket fix):
    // `sema::isAutoPropertySymbol`/`isAutoProperty`/`isStaticAutoProperty`
    // have roughly thirty OTHER call sites (definite assignment, ownership/
    // disposal ARC, sendability, serialization eligibility, pattern
    // destructuring, the for-in "named vs. computed source" disposal test)
    // audited during #2478's investigation — each has its own failure
    // direction (some silent-wrong-data, some spurious-reject, at least one
    // disposal-miscount/double-free-adjacent) and its own regression-test
    // burden. Only the two STORAGE-LAYOUT call sites that directly cause the
    // confirmed struct-offset collision (`collectInstanceStorage`,
    // `collectRecordStorage`, immediately below) are widened here; the rest
    // remain open, filed as a follow-up (see #2478's tracking comment).
    bool isAutoPropertyStorage(const sema::Symbol* m) {
        if (isAutoProperty(m)) return true;
        auto* ps = dynamic_cast<const sema::PropertySymbol*>(m);
        if (!ps || ps->isIndexer || ps->decl != nullptr) return false;
        return ctx_.importedPropertyIsAutoStorage(ps);
    }

    // fix #2421 (§24/§2.8.2/§2.8.3 rule 4, v0.236): the EventSymbol
    // classification bits the event-dispatch path recovers from
    // `ast::EventDecl` in the LOCAL path (collectInstanceStorage /
    // emitOwnFieldInit / ctor-cleanup need isStatic; the ifaceEventIndex_
    // index + itab$C$I BUILD loops need isAsync) are decl-dependent
    // (`dynamic_cast<const ast::EventDecl*>(es->decl)`), so they always
    // returned false for an IMPORTED EventSymbol (no decl crosses the module
    // boundary — ModuleLoader.cpp's Event case reconstructs the symbol
    // without one). An imported event was therefore silently DROPPED by every
    // one of those consumers: no fieldIndex_ slot, no itable index, no itable
    // entry — so cross-module Subscribe/Raise resolved to nothing and the
    // handler never fired (filed as #2421, one layer past #2407's symbol
    // reconstruction). These helpers mirror isAutoPropertyStorage's shape
    // exactly: try the local decl-based bit first (unchanged for every LOCAL
    // event); when the decl is null (an imported event), consult the wire-
    // carried classification (ctx_.importedEventIsStatic/Async, set from
    // MetaMember::isEventStatic/isEventAsync by ModuleLoader.cpp) instead of
    // silently treating it as a non-event. A WRONG guess on either bit is a
    // real hazard, not just an imprecision — it would reserve an importer-
    // side itable/struct slot the exporter's real layout does not have (or
    // vice versa), an itable/struct-layout MISMATCH between the two
    // compilations (the exact class #2420/#2406 caught elsewhere) — so the
    // wire bit is the sole safe source of truth for an imported event.
    bool eventIsStatic(const sema::EventSymbol* es) {
        if (!es) return false;
        if (auto* ed = dynamic_cast<const ast::EventDecl*>(es->decl))
            return ed->isStatic;
        if (es->decl != nullptr) return false;  // non-EventDecl decl
        return ctx_.importedEventIsStatic(es);
    }
    bool eventIsAsync(const sema::EventSymbol* es) {
        if (!es) return false;
        if (auto* ed = dynamic_cast<const ast::EventDecl*>(es->decl))
            return ed->isAsync;
        if (es->decl != nullptr) return false;  // non-EventDecl decl
        return ctx_.importedEventIsAsync(es);
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
    //
    // fix #2490 (§23.3/§4.6/§27.5/§28.9, v0.236): `valueExpr` is the SOURCE
    // ast::Expr `v` was lowered from, when the caller has one (nullptr at the
    // `ref`/`out` accessor write-back call site, whose `v` is a reload of an
    // existing slot, never a fresh producer). See the .cpp definition for why
    // this is needed — the callee (the generated setter body) now always
    // retains its incoming borrow (§23.3), so a caller-side +1 producer
    // passed DIRECTLY as the argument (no local binding of its own) needs the
    // SAME post-call release the #429 ARGUMENT-PASS call-argument release
    // already gives an ordinary method call (LlvmBackend_Calls2.cpp), or it
    // leaks.
    bool lowerVirtualPropertySet(const ast::Expr& target, llvm::Value* v,
                                 const ast::Expr* valueExpr = nullptr,
                                 bool valueIsPlusOne = false);

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

    // #730 / §23.4 / §13: emit (or retrieve cached) a standalone LLVM function
    // for a class's concrete indexer getter / setter, used to fill the itable
    // slot for INTERFACE-typed indexer dispatch and to call through it. The
    // getter is `T get(ptr this, idx...)`, the setter `void set(ptr this, idx...,
    // value)`; both handle expression- and block-bodied accessors. Returns null
    // if the property has no getter / setter body. `ps` must be a concrete
    // (non-interface) class indexer.
    llvm::Function* ensureIndexerGetterFn(const sema::TypeSymbol* cls,
                                          const sema::PropertySymbol* ps);
    llvm::Function* ensureIndexerSetterFn(const sema::TypeSymbol* cls,
                                          const sema::PropertySymbol* ps);

    // #730/#2437/#2442/#2449: the class `cls`'s (or an inherited local OR
    // imported base's) concrete indexer that implements the interface
    // indexer `ifaceIdx`, matched by FULL parameter list via findIndexer.
    const sema::PropertySymbol* classIndexerImplementing(
        const sema::TypeSymbol* cls, const sema::PropertySymbol* ifaceIdx);

    // fix #2449 (§11.2/§23.4, v0.235): the itable-slot function for
    // classIndexerImplementing's result `iimpl` — LOCAL (own or inherited
    // from a local base) via ensureIndexerGetterFn/-SetterFn, IMPORTED
    // (inherited from an imported base) via the importedIndexerGetterFns_/
    // -SetterFns_ cross-module ABI stub. See the .cpp definition.
    llvm::Function* indexerItableAccessor(const sema::TypeSymbol* ts,
                                          const sema::PropertySymbol* iimpl,
                                          bool isSet);

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
    //
    // fix #2568 (§23.3/§28.9, v0.236): `valueExpr` (default nullptr for the
    // `ref`/`out` write-back call site and the increment-stmt call site that
    // have no original source expression) is the source ast::Expr `v` was
    // lowered from — see lowerVirtualPropertySet's comment for why a
    // caller-side post-call release is needed now that the inlined setter
    // body mints its own +1 on the class path.
    bool lowerPropertySetValue(const ast::Expr& target, llvm::Value* v,
                               const ast::Expr* valueExpr = nullptr);

    // fix #2400 (§2.8.2/§2.8.3 rule 4, §11.2, §23; v0.235): a DIRECT
    // (non-itable, non-virtual) cross-module property SET — `importedObj.Prop
    // := v` for a plain property declared or inherited on an imported class,
    // accessed through the imported class's own type (not through an
    // interface reference, which dispatches via lowerVirtualPropertySet
    // instead). computedSetterOf returns nullptr for this shape (the imported
    // PropertySymbol has no ast::PropertyDecl), so lowerPropertySet /
    // lowerPropertyCompound bail before reaching lowerPropertySetValue; tried
    // as a fallback AFTER lowerPropertySet fails. Calls the external setter
    // stub (`bz$<Class>$<Prop>$propset`) declared by
    // declareImportedPropertySetters — the direct-set twin of the imported-
    // property getter fallback in LlvmBackend_Exprs.cpp (~4283-4300, issue
    // #503). Returns false (so the caller's existing fallthrough is
    // unaffected) when `target` is not an imported property with a declared
    // setter stub.
    bool lowerImportedPropertySet(const ast::Expr& target,
                                  const ast::Expr& value);
    // fix #2490 (§23.3/§4.6/§27.5/§28.9, v0.236): `valueExpr` (default
    // nullptr for the `ref`/`out` write-back call site) is the source
    // ast::Expr `v` was lowered from — see lowerVirtualPropertySet's comment
    // just above for why a caller-side post-call release is needed now that
    // the callee always retains.
    bool lowerImportedPropertySetValue(const ast::Expr& target, llvm::Value* v,
                                       const ast::Expr* valueExpr = nullptr);

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
            // fix #2478 (§9.2/§22.8): isAutoPropertyStorage (not the bare
            // decl-dependent isAutoProperty) so an IMPORTED auto-property
            // occupies its correct storage slot — see that helper's comment
            // for the confirmed memory-safety hazard this closes.
            if (isAutoPropertyStorage(m)) out.push_back(m);
            // per §24.1 / §24.5 (#1176): each INSTANCE event declaration (sync
            // or async) gets a compiler-generated owned backing field of type
            // EventSource<T> / AsyncEventSource<T>, laid out as a plain `ptr`
            // slot. Static events (static-init backed) are handled separately in
            // the staticEventGlobals_ path and must not appear here.
            if (auto* es = dynamic_cast<const sema::EventSymbol*>(m)) {
                // fix #2421 (§24/§2.8.2): eventIsStatic, not the bare
                // decl-dependent `ed->isStatic` — an IMPORTED (decl-null) event
                // was silently dropped here before, giving it no fieldIndex_
                // slot so a directly-typed/imported-base event's EventSource
                // lived at no offset.
                if (!eventIsStatic(es)) out.push_back(m);
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

    // §35 (v0.200, #2045): the choice struct — a value-type sum lowered as
    // `{ i32 tag, case0_payload, case1_payload, ... }`. Each case's payload
    // is its own struct type at field index `1 + caseIndex` (a payload-free
    // case's slot is a single-byte `{ i8 }` placeholder; a payload case's
    // slot is `{ type0, type1, ... }` for its payload fields in declaration
    // order). The tag is the case's declaration-order index (0-based). This
    // "one slot per case" representation wastes memory (only the active
    // case's slot is live) but is dead-simple to lower — no union byte-
    // casting — and is sound for the §35.13 "implementation-defined layout"
    // latitude. Cached in `structs_[t]` like recordStruct.
    llvm::StructType* choiceStruct(const sema::TypeSymbol* t);

    // §35: a choice's cases in declaration order. The case's index in this
    // vector is its tag value. Defined in LlvmBackend_Layout.cpp (needs
    // dynamic_cast on the choice's members).
    std::vector<const sema::ChoiceCaseSymbol*> choiceCasesOf(
        const sema::TypeSymbol* choiceTy);

    // §35: find a choice case by name on a choice TypeSymbol; null if not
    // found. Reports the case's declaration-order tag index via *tagOut
    // (when non-null) — the value stored into the tag slot at construction
    // and read by the match-side tag comparison.
    const sema::ChoiceCaseSymbol* choiceCaseByName(
        const sema::TypeSymbol* choiceTy, std::string_view name,
        std::size_t* tagOut = nullptr);

    // §35: a case's payload struct type — the LLVM struct laid out as
    // `{ type0, type1, ... }` for the case's payload fields in declaration
    // order, or `{ i8 }` for a payload-free case (the placeholder slot —
    // every case occupies exactly one field in the choice struct so the
    // active-case slot index is uniform: `1 + caseIndex`).
    llvm::StructType* choiceCasePayloadStruct(
        const sema::TypeSymbol* choiceTy,
        const sema::ChoiceCaseSymbol* cc);

    // §9 / §9.1 (#1759): if the instance method currently being emitted is a
    // RECORD method, return its record TypeSymbol; otherwise null (class / enum
    // / extension receiver, a static method, or no method in flight). Recovers
    // the owner exactly as lowerWith does (#575 / #628): reverse-look currentFn_
    // in methodFns_, then fall back to currentSpecMethodSymbol_ for a
    // specialized generic body that is not registered there.
    const sema::TypeSymbol* currentReceiverRecordType();

    // §9 / §9.1 (#1759): a record has value semantics — assignment / return /
    // by-value argument copies the struct. Inside a record method `this` lowers
    // to a POINTER to the by-value receiver (CG29 / §9.2 / #794). When the whole
    // `this` value flows to a location that expects the record STRUCT, `v` is
    // the struct *this, not the 8-byte pointer. If `v` is exactly that receiver
    // pointer and `expectedTy` is precisely the current receiver record's struct
    // type, load the struct so the value-semantics copy is produced; otherwise
    // return `v` unchanged (a no-op for every non-`this` value, class/enum
    // receivers, and any nullable-tagged / unrelated struct destination).
    llvm::Value* coerceThisRecordValue(llvm::Value* v, llvm::Type* expectedTy);

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
            // fix #2478 (§9.2/§22.8): isAutoPropertyStorage (not the bare
            // decl-dependent sema::isAutoPropertySymbol) so an IMPORTED
            // record's auto-property occupies its correct storage slot —
            // see that helper's comment (near isAutoProperty above) for the
            // confirmed memory-safety hazard this closes.
            if (isAutoPropertyStorage(m)) out.push_back(m);
        }
    }

    // §27.4 (v0.183, #1408): lambda-parameter record destructuring prologue.
    // `structVal` is an already-lowered record struct VALUE (the lambda's
    // incoming argument, or a field extracted from an enclosing level for a
    // nested LambdaDestructure slot); binds each slot's LocalSymbol to a
    // fresh alloca holding the corresponding positional field, recursing into
    // nested DestructuringPattern slots. Mirrors patternMatches' unconditional
    // field-extraction for a plain (non-boxed) record scrutinee (§19/§9.4,
    // LlvmBackend_Exprs2.cpp ~6151) but with no runtime match test — sema
    // (ExprTC::bindLambdaDestructure) already enforced the head is exactly the
    // parameter's (or field's) static type, so every slot unconditionally
    // reads its field.
    void bindLambdaDestructureParam(const ast::DestructuringPattern& dp,
                                    llvm::Value* structVal);

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

    // #1488 / §2.8.3 rule 3: the module-qualified mangling name for a type —
    // "<declaring-module>.<TypeName>" — used as the `<Class>` component
    // everywhere a "bz$"/"vt$"/"td$"/... symbol name is built from a
    // sema::TypeSymbol. Two modules exporting a same-named public type (e.g.
    // both declaring `public class Tx`) previously mangled identically
    // ("bz$Tx$Hello" from both), which the linker rejected as a multiple
    // definition (issue #1488) and which — even before the link step — made
    // one compilation's OWN two external declarations of the same bare name
    // collide (LLVM auto-uniquifying the second as "bz$Tx$Hello.1", a symbol
    // that exists nowhere). Qualifying by the declaring module — carried on
    // every TypeSymbol as `moduleName`, populated identically for a local
    // declaration (SymbolCollection) and an imported one (ModuleLoader) so
    // the exporter's definition and every importer's external declaration
    // agree — makes every such symbol program-wide unique, matching
    // §2.8.3 rule 3's identity ("declaring module name, ... qualified type
    // name, and generic arity"). `ts` is never null at a genuine call site;
    // the guard only covers a defensive/degenerate caller.
    static std::string mangledTypeName(const sema::TypeSymbol* ts) {
        if (!ts) return "";
        if (ts->moduleName.empty()) return std::string(ts->name);
        return ts->moduleName + "." + std::string(ts->name);
    }

    // §26 (#433): is `ms` an extension method (declared in an `extension`
    // block)? Such a method has no `parent` TypeSymbol; its receiver type is
    // the extension's targetType.
    static bool isExtensionMethod(const sema::MethodSymbol* ms) {
        return ms && ms->extensionDecl != nullptr;
    }
    // #720: the declaring type of a field/property/method symbol (its `parent`),
    // or null. Used to find the type whose static state to lazily initialize.
    static const sema::TypeSymbol* memberOwner(const sema::Symbol* sym) {
        if (auto* ms = dynamic_cast<const sema::MemberSymbol*>(sym))
            return ms->parent;
        return nullptr;
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

    // §35 (v0.200, #2045): the choice-typed analogue of enumTypeOf —
    // recover a choice TypeSymbol from a value expression (a scrutinee,
    // a local, a qualified case access). Null when the expr's resolved
    // type is not a choice. Defined in LlvmBackend_Setup.cpp next to
    // enumTypeOf.
    const sema::TypeSymbol* choiceTypeOf(const ast::Expr& e);

    // §35.3 (#2045): materialize a bare PAYLOAD-FREE choice case access
    // (`Opt.None`) as the choice struct constant. The payload-carrying
    // call form (`Opt.Some(42)`) is lowered by lowerCall; this handles
    // only the no-parens value position. Null when `e` is not a choice-
    // case access or the case carries a payload. Defined in
    // LlvmBackend_Setup.cpp next to enumConstant.
    llvm::Value* choiceCaseConstant(const ast::Expr& e);

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
    // #1146: the constant-folding core, taking the initializer expression
    // directly so it serves both a static field's `FieldDecl::initializer` and a
    // static auto-property's `PropertyDecl::autoInitializer`.
    llvm::Constant* constInitForExpr(const ast::Expr* e, llvm::Type* ty);

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

    // fix #2400 (§2.8.2/§2.8.3 rule 4, §11.2, §23; v0.235): the SETTER twin of
    // emitExportedPropertyGetters — emit a named ExternalLinkage setter function
    // `bz$<Class>$<Prop>$propset(ptr %this, T value) -> void` for every PUBLIC,
    // non-virtual, non-indexer property WITH A SETTER on every PUBLIC, non-generic
    // LOCAL class. No setter-side equivalent existed before this fix (confirmed
    // empirically: `nm` on a compiled library object with a settable property
    // showed `...$propget` but no `...$propset` symbol at all), so cross-module
    // SETTING of a non-virtual imported property had no ABI surface whatsoever —
    // independent of interfaces/itables. Mirrors emitExportedPropertyGetters
    // exactly, including relying on ensurePropertyAccessorFn's own body-presence
    // check (rather than a separate "hasSetter" filter here) to skip getter-only
    // properties: it returns nullptr for those and the incomplete declaration is
    // erased.
    void emitExportedPropertySetters();

    // fix #2400: the SETTER twin of declareImportedPropertyGetters — declare
    // EXTERNAL setter stubs for every imported PropertySymbol that is not
    // virtual (not in propSetSlot_), not an interface member (those dispatch
    // through the itable), and actually HAS a setter per the imported
    // MetaMember::hasSetter bit (ctx_.importedPropertyHasSetter — a new
    // metadata field this fix adds; see its comment in ModuleMetadata.hpp).
    // Unlike declareImportedPropertyGetters, this CANNOT declare unconditionally
    // for every eligible property: Pass D's itable-building loop
    // (LlvmBackend_Setup.cpp) calls ensurePropertyAccessorFn(ts, impl,
    // isSet=true) for every property a LOCAL interface declares read-write,
    // regardless of whether the concrete implementing property actually has a
    // setter (no accessor-set matching check exists there — a separate,
    // pre-existing gap) — a getter-only imported base property satisfying such
    // an obligation would otherwise get a REAL relocation installed into the
    // itable to a setter symbol the exporter never emitted, an unresolved-
    // symbol link error at BUILD time regardless of whether the program ever
    // writes through that slot (confirmed: this exact shape is in the
    // pre-existing #2387/#2380 test fixtures). The stubs are stored in
    // importedPropSetterFns_ for use by both the itable-slot fallback
    // (ensurePropertyAccessorFn) and the direct (non-itable) property-set
    // lowering path. MUST be called from the SAME call site as
    // declareImportedPropertyGetters (inside declareVtables, after
    // propSetSlot_ is populated by the buildVtable loops, before Pass D) —
    // #2387 already discovered that calling this after declareVtables() returns
    // leaves the map empty during Pass D, silently defeating the fallback.
    void declareImportedPropertySetters();

    // fix #2414 (§22.10/§23.4/§2.8.2 clause 1/§2.8.3 rule 4, v0.235): the
    // INDEXER analog of emitExportedPropertyGetters — emit a named
    // ExternalLinkage getter function `bz$<Class>$idxget$<arity>(ptr %this,
    // idx...) -> T` for every PUBLIC, non-virtual, non-interface indexer on
    // every PUBLIC, non-generic LOCAL class, so the importer has a stable
    // cross-module symbol to call. Reuses ensureIndexerGetterFn (already
    // exists for the itable-fill path) for the actual body — this just wraps
    // it with ExternalLinkage, exactly as emitExportedPropertyGetters wraps
    // ensurePropertyAccessorFn. Keyed by CLASS + ARITY, not the full
    // index-parameter type signature (a documented simplification — see
    // xmod_indexer_import_2414_test.cpp's file header): two indexer
    // overloads of the SAME arity on one class would collide on this symbol;
    // no spec example needs that shape, so it is left as a known limitation
    // rather than built out here. Virtual/interface indexers already
    // dispatch through the vtable/itable slot machinery (#1080/#730) and are
    // excluded, matching emitExportedPropertyGetters's own exclusion.
    void emitExportedIndexerGetters();

    // fix #2414: the SETTER twin of emitExportedIndexerGetters immediately
    // above — `bz$<Class>$idxset$<arity>(ptr %this, idx..., T value) ->
    // void`, wrapping ensureIndexerSetterFn. Skipped (no wrapper emitted)
    // when the class's indexer has no setter, mirroring
    // emitExportedPropertySetters's reliance on the inner builder's own
    // body-presence check.
    void emitExportedIndexerSetters();

    // fix #2414: the INDEXER analog of declareImportedPropertyGetters —
    // declare an EXTERNAL `bz$<Class>$idxget$<arity>` stub for every
    // imported PropertySymbol with isIndexer=true that is not virtual (not
    // in propGetSlot_) and not an interface member (those dispatch through
    // the itable — out of scope for this fix, see the test file's "SCOPE"
    // note). Stored in importedIndexerGetterFns_ for lowerIndexerGet's
    // decl-null fallback. Must run at the SAME call site as
    // declareImportedPropertyGetters (inside declareVtables, after
    // propGetSlot_ is populated, before Pass D) — #2387's ordering lesson
    // applies identically here.
    void declareImportedIndexerGetters();

    // fix #2414: the SETTER twin — declare an EXTERNAL
    // `bz$<Class>$idxset$<arity>` stub for every eligible imported indexer
    // that ALSO has a setter per the reconstructed MetaMember::hasSetter bit
    // (ctx_.importedPropertyHasSetter, reused from #2400 — see
    // ModuleLoader.cpp's Indexer case, which publishes it through the same
    // propHasSetter_ side table Property uses). Gating on the real bit
    // (rather than declaring unconditionally) mirrors #2400's own documented
    // reasoning: an unconditionally-declared external symbol the exporter
    // never emitted (a getter-only indexer) would be an unresolved-symbol
    // link error if ever referenced.
    void declareImportedIndexerSetters();

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

    // #742 / §11.2 / §13: the class method that implements a specific interface
    // method `iface` for v/itable binding. findMethodByName matches by name
    // ONLY, so when the class has multiple overloads of the interface method
    // name it binds the WRONG overload into the itable slot (e.g. `Run()` for
    // an interface `Run(x: int)` → dispatch ignores the argument). Prefer the
    // overload whose parameter COUNT matches `iface` (sema already guarantees
    // one exists — see the arity-conformance check in TypeResolution_Checks).
    // Scans most-derived class first so an override wins. Falls back to a
    // name-only match (single-overload cases are unchanged). NOTE: among
    // same-arity overloads it still takes the first in declaration order;
    // full parameter-TYPE disambiguation is a follow-up.
    sema::MethodSymbol* findInterfaceImpl(const sema::TypeSymbol* cls,
                                          const sema::MethodSymbol* iface) {
        if (!iface) return nullptr;
        sema::MethodSymbol* nameOnly = nullptr;
        for (const sema::TypeSymbol* t = cls; t;) {
            for (sema::Symbol* m : t->members) {
                auto* ms = dynamic_cast<sema::MethodSymbol*>(m);
                if (!ms || isStaticMethod(ms) || ms->name != iface->name ||
                    !methodFns_.count(ms))
                    continue;
                if (ms->params.size() == iface->params.size()) return ms;
                if (!nameOnly) nameOnly = ms;  // remember first name match
            }
            t = baseClassOf(t);
        }
        return nameOnly;
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

    // #1080 / §23.4: a vtable-slot key for an INDEXER that distinguishes
    // overloads by their index-parameter signature (two indexers `[i: int]` and
    // `[s: bool]` must get DISTINCT slots, not collide on the shared name
    // ".this"). A base virtual indexer and its derived override have the same
    // index signature → the same key → the override reuses the base's slot
    // (mirrors how #1085 keys method-overload slots by arity).
    std::string indexerSlotKey(const sema::PropertySymbol* ps) {
        std::string key(ps->name);  // ".this"
        // fix #2545 (§23.4 / §12.1 / §2.8.2 clause 1, v0.236): an abstract
        // IMPORTED indexer has no ast::PropertyDecl (ModuleLoader's Indexer
        // case reconstructs `indexerParams` as ParameterSymbols with
        // `resolvedDeclType`), but the key MUST still include the index-param
        // TYPE spellings — otherwise the abstract base's key (just ".this")
        // does NOT match the derived class's override key (".this#int."), and
        // `buildVtable` invokes the existing assignSlot override reuse exactly
        // once (key match) so the override gets a NEW slot (slot 8) instead of
        // reusing the base's (slot 6). The vtable then has BOTH slots — the
        // base's abstract slot is null (no body), and the override is at a
        // different offset. The cross-module dispatch through the base slot
        // lands on null, segfaulting the abstract-typed `b[i]` call. Read
        // the parameter types from the reconstructed side tables (the same
        // mirror of #2414's `indexerParamType` fallback) when the AST is
        // absent, so the abstract base and the concrete derived class's
        // overrides share the same key across the chain.
        if (auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl)) {
            for (const auto& p : pd->indexerParams) {
                key += "#";
                if (p.type && p.type->name && !p.type->name->parts.empty()) {
                    for (auto part : p.type->name->parts) {
                        key += std::string(part);
                        key += ".";
                    }
                } else {
                    key += "?";
                }
            }
        } else {
            for (sema::Symbol* p : ps->indexerParams) {
                key += "#";
                auto* pps = dynamic_cast<sema::ParameterSymbol*>(p);
                sema::TypeRef* tr = pps ? pps->resolvedDeclType : nullptr;
                if (!tr) {
                    key += "?";
                } else if (tr->kind == sema::TypeRef::Kind::Primitive) {
                    // #2545: an imported indexer parameter's type is
                    // reconstructed as a Primitive TypeRef (no AST, no
                    // imports). Map the PrimitiveType enum to the same
                    // lowercase spelling the local AST `parts` iteration
                    // produces above (the source-keyed spelling — case
                    // matters, e.g. "int" vs "Int"), so the abstract base's
                    // key matches the derived class's override key.
                    switch (tr->primitive) {
                        case sema::PrimitiveType::Bool:    key += "bool";    break;
                        case sema::PrimitiveType::Char:    key += "char";    break;
                        case sema::PrimitiveType::String:  key += "string";  break;
                        case sema::PrimitiveType::Byte:    key += "byte";    break;
                        case sema::PrimitiveType::SByte:   key += "sbyte";   break;
                        case sema::PrimitiveType::Short:   key += "short";   break;
                        case sema::PrimitiveType::UShort:  key += "ushort";  break;
                        case sema::PrimitiveType::Int:     key += "int";     break;
                        case sema::PrimitiveType::UInt:    key += "uint";    break;
                        case sema::PrimitiveType::Long:    key += "long";    break;
                        case sema::PrimitiveType::ULong:   key += "ulong";   break;
                        case sema::PrimitiveType::Float:   key += "float";   break;
                        case sema::PrimitiveType::Double:  key += "double";  break;
                        case sema::PrimitiveType::Decimal: key += "decimal"; break;
                        case sema::PrimitiveType::Object:  key += "Object";  break;
                        default:                           key += "?";       break;
                    }
                    key += ".";
                } else if (!tr->headName().empty()) {
                    // headName() returns the alias-transparent head for a
                    // Named type (the underlying builtin head for an alias,
                    // the use-site syntactic head for a local Named type, the
                    // metadata importedName for an imported type). The local
                    // AST path above iterates over parts with "." separators;
                    // for an imported/abstracted key this single-head
                    // shorthand is enough (the override's key, computed
                    // locally with the same shape, ends in the same head).
                    for (auto part : tr->headName()) {
                        key += part;
                    }
                    key += ".";
                } else {
                    key += "?";
                }
            }
        }
        return key;
    }

    // #1080 / §23.4: the most-derived INDEXER on `cls`'s chain whose index
    // signature matches `proto` (the indexer that introduced the vtable slot).
    // classPropertyByName deliberately skips indexers; this fills an indexer's
    // slot with the concrete class's getter/setter so a derived override fills
    // the base-introduced slot, while keeping overloaded indexers distinct.
    sema::PropertySymbol* classIndexerMatching(const sema::TypeSymbol* cls,
                                               const sema::PropertySymbol* proto) {
        if (!proto) return nullptr;
        const std::string want = indexerSlotKey(proto);
        for (const sema::TypeSymbol* t = cls; t;) {
            for (sema::Symbol* m : t->members) {
                auto* ps = dynamic_cast<sema::PropertySymbol*>(m);
                if (!ps || !ps->isIndexer) continue;
                bool isStatic = false;
                for (auto mod : ps->modifiers)
                    if (mod == "static") { isStatic = true; break; }
                if (!isStatic && indexerSlotKey(ps) == want) return ps;
            }
            t = baseClassOf(t);
        }
        return nullptr;
    }

    // #815 / §9.1: a positional record field generates a public read-only
    // property of the same name, but it is represented as a FieldSymbol (with a
    // non-null `posParam`), not a PropertySymbol — so classPropertyByName misses
    // it. When a positional record implements an interface property (or is used
    // through a `where T: IFace` constraint), the same-named positional field is
    // the implementation. Return it so the caller can emit a struct-GEP+load.
    sema::FieldSymbol* classPositionalFieldProperty(const sema::TypeSymbol* cls,
                                                    std::string_view name) {
        for (const sema::TypeSymbol* t = cls; t;) {
            for (sema::Symbol* m : t->members) {
                auto* fs = dynamic_cast<sema::FieldSymbol*>(m);
                if (!fs || fs->posParam == nullptr || fs->name != name) continue;
                bool isStatic = false;
                for (auto mod : fs->modifiers)
                    if (mod == "static") { isStatic = true; break; }
                if (!isStatic) return fs;
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
                // fix #2421 (§24/§2.8.2): eventIsStatic/eventIsAsync, not
                // the bare decl-dependent bits — an event inherited from an
                // IMPORTED base (decl-null) was silently never resolved here,
                // so the implementing class's itable got a null accessor for a
                // sync imported-base event the exporter did lay out.
                if (!eventIsStatic(es) && !eventIsAsync(es) &&
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
    // fix #1139 / §11.2: generate (or return cached) a thunk that adapts the
    // interface ABI (T for each param) to the impl ABI (T? for widened params).
    llvm::Function* ensureNullableWideningThunk(const sema::MethodSymbol* ifaceMs,
                                                const sema::MethodSymbol* implMs);
    llvm::Function* emitEventAccessor(const sema::TypeSymbol* cls,
                                      const sema::EventSymbol* es);

    // #1592: the resolved declared TypeRef for a ParameterSymbol (prefers
    // resolvedDeclType, falls back to resolving declType) — the same
    // resolution order used throughout the widening machinery below and in
    // ensureNullableWideningThunk.
    sema::TypeRef* paramTypeOf(sema::Symbol* p) {
        auto* ps = dynamic_cast<sema::ParameterSymbol*>(p);
        if (!ps) return nullptr;
        if (ps->resolvedDeclType) return ps->resolvedDeclType;
        return ps->declType ? ctx_.resolvedType(*ps->declType) : nullptr;
    }

    // #1592 / §12.1: find the INTRODUCING (non-override) virtual declaration
    // for `ms` by walking the base-class chain while the same vtable slot
    // continues to be present on the ancestor. Returns `ms` itself if it has
    // no vtable slot info, or if it already is the introducing declaration.
    const sema::MethodSymbol* introducingVirtualMethod(
        const sema::MethodSymbol* ms);
    // #1592: the widened parameter-position bitmap for `rootMs`'s whole
    // virtual-slot family (memoized in vtableFamilyWidenedParams_). Empty
    // vector or all-false ⇒ no widening anywhere ⇒ existing (pre-#1592)
    // direct-install / direct-call behavior applies unchanged.
    const std::vector<bool>& widenedParamPositions(
        const sema::MethodSymbol* rootMs);
    // #1592: the family's uniform ABI function type — `rootMs`'s own declared
    // signature with each widened position's type replaced by the tagged
    // `{i8 hasValue, T}` struct. Returns methodFnType(rootMs) unchanged (no
    // widened positions) when `widened` is all-false.
    llvm::FunctionType* uniformVtableFnType(const sema::MethodSymbol* rootMs,
                                            const std::vector<bool>& widened);
    // #1592: adapter thunk presenting the family's uniform (possibly wide)
    // ABI for a contributor (`leafMs`) whose own compiled body is still the
    // narrow form — unboxes each widened argument before forwarding. Returns
    // nullptr when leafMs's own ABI already matches the uniform type (no
    // thunk needed — direct install/call suffices).
    llvm::Function* ensureVtableWideningThunk(const sema::MethodSymbol* rootMs,
                                              const sema::MethodSymbol* leafMs,
                                              const std::vector<bool>& widened);

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

    // §19 / §A.8 (#957, #1853): fold the value of a NamedPattern whose name
    // resolves to a named CONST. Three resolution forms:
    //  - a bare name that sema recorded as a LOCAL compile-time const
    //    (§17.14 — `const K := 7; case n of K`): fold K's initializer. A
    //    pattern name carries no resolved-symbol entry under exprSymbol (a
    //    Pattern is not an Expr) and locals are not in `symbols()`, so sema's
    //    checkTopLevelNamedPattern records the resolved LocalSymbol on the
    //    pattern via setPatternSymbol; read it here (#1853).
    //  - a qualified `C.K`: K is a const member of the type named just before.
    //  - a bare `K`: a unique const MEMBER of that name (module/own-class).
    // Returns null when the name is not a const — the pattern is then a
    // type-test / enum constant handled elsewhere, or deferred.
    llvm::Value* constValueForPatternName(const ast::NamedPattern* np);

    // §19.3 (v0.182, #1852): emit the i1 match condition "scrut is-a `ct`" for a
    // type-test pattern whose type is a bare **type parameter** resolved, through
    // the active monomorphization substitution, to the concrete type `ct`. Shared
    // by the bare pattern `case x of T` and the typed binding `var x: T`, whose
    // §19.3 rule is identical ("test against the substituted type argument, per
    // instantiation"): a class/interface → runtime is-a against the PER-
    // MONOMORPHIZATION descriptor (#1843 perInstDescriptorForTarget, so
    // `T=Wrap<int>` does not match a `Wrap<string>` scrutinee); a built-in
    // exception → the btd$ descriptor is-a (like #1854); a value-builtin
    // (DateTime/Guid/… — no reference descriptor) → a well-formed never-match; a
    // record/enum → static decidability; a primitive incl. string → the boxed /
    // same-primitive test. Returns null ONLY for a type-test kind it cannot lower
    // — the caller then emits a TERMINATED never-match (never falls through to a
    // return-nullptr, which would leave `case.end` unterminated → malformed IR).
    // `loc` (#2069) is the pattern's span — the collapse guard
    // (rejectCollapsedTypeTestTarget) runs here for a bare-parameter pattern whose
    // SUBSTITUTED target is a runtime-erased constructed generic.
    llvm::Value* emitTypeParamPatternTest(const sema::TypeRef* ct,
                                          llvm::Value* scrut,
                                          const sema::TypeRef* scrutTy,
                                          SourceLocation loc);

    // §19.3/§19.5 (#1938): emit a runtime is-a of `scrut` against `desc`, but
    // short-circuit a runtime bz_string* scrutinee to a clean never-match BEFORE
    // emitIsA when the scrutinee's static type could hold one (`Object` or an
    // interface). emitIsA loads obj[0] as a descriptor and dereferences it; a
    // string's word 0 is `char* data`, not a descriptor, so the walk faults
    // (SIGSEGV). A string is never a class/interface/exception, so no-match is
    // the correct result. This is the concrete-type-name analogue of the guard
    // emitTypeParamPatternTest::guardedIsA applies for the type-PARAMETER form
    // (#1852) — the two paths must stay consistent.
    llvm::Value* emitStringGuardedIsA(llvm::Value* scrut,
                                      llvm::GlobalVariable* desc,
                                      const sema::TypeRef* scrutTy);

    // §31.4 (v0.162/v0.163) built-in exception object layout. A directly-
    // constructed built-in exception (rt->declaration == null) is a single
    // calloc'd block whose slots are, at these fixed byte offsets:
    //   [0]  vtable ptr
    //   [8]  Message      — bz_string*  (null → "")
    //   [16] InnerException — TypeInitializationException only (§22.13); other
    //        types leave it null. Kept at its historical offset so the TIE
    //        wrapper and the InnerException getter are unchanged.
    //   [24] CauseChain   — bz_array_t* of ExceptionInfo (null → empty), the
    //        #811 snapshot built by the (message, cause) constructor.
    //   [32] ParamName    — bz_string* (ArgumentException; null when absent).
    // The block is a uniform kExcObjSize bytes for every built-in exception so
    // the base-Exception getters (CauseChain at 24) read a valid slot on any of
    // them; calloc zeroes the unused slots. Growing (never moving) the block
    // keeps every existing read (Message@8, InnerException@16) intact.
    static constexpr int64_t kExcObjSize        = 40;
    static constexpr int64_t kExcMsgOffset      = 8;
    static constexpr int64_t kExcInnerOffset    = 16;
    static constexpr int64_t kExcCauseOffset    = 24;
    static constexpr int64_t kExcParamOffset    = 32;

    // §31.4 / #811: build the CauseChain snapshot for `new Exception(msg, cause)`
    // from the (already-lowered) borrowed `cause` object. Reads the cause's type
    // name (virtual ToString, vtable slot 3), its Message (offset 8) and — for a
    // built-in cause — its own CauseChain (offset 24), and returns a fresh
    // Array<ExceptionInfo> = [ {typeName, message, ""} ] ++ cause.CauseChain via
    // the runtime bz_exc_causechain_link. `causeIsBuiltin` gates the nested-chain
    // read: a user-derived exception has no CauseChain slot at offset 24.
    llvm::Value* emitExcCauseSnapshot(llvm::Value* cause, bool causeIsBuiltin);

    // §31.4: read field `fieldIdx` (TypeName=0, Message=1, StackTrace=2) of an
    // ExceptionInfo value named by `target`. Handles the common `chain[i]` index
    // form directly (bounds-checked bz_array_get into a temp, then field GEP) and
    // an addressable ExceptionInfo local as a fallback.
    llvm::Value* lowerExceptionInfoField(const ast::Expr& target,
                                         unsigned fieldIdx);

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
        // §22.10 (v0.180) / #1594: a captured `owned` primary-ctor parameter
        // typed as a BUILTIN exception (e.g. `owned e: Exception`, referenced
        // in a body position). Mirrors isOwnedReferenceField's posParam
        // branch (#1543); capturedOwnedPrimaryParamType is defined further
        // below, alongside isCapturedOwnedPrimaryParam — forward-declared use
        // here is fine since this is a member function of the same class.
        if (fs->posParam)
            return isBuiltinExceptionTypeRef(capturedOwnedPrimaryParamType(fs));
        if (!hasOwnedModifier(fs)) return false;
        // #662 foundation B: imported field — type from importedMemberType.
        if (!fs->decl)
            return isBuiltinExceptionTypeRef(ctx_.importedMemberType(fs));
        auto* fd = dynamic_cast<const ast::FieldDecl*>(fs->decl);
        if (!fd || !fd->type) return false;
        return isBuiltinExceptionTypeRef(ctx_.resolvedType(*fd->type));
    }

    // #1643 / §28 / §29.7: an `owned` field whose type is a builtin ITERATOR
    // interface (IEnumerable<T> / IOwningEnumerable<T>) — a declaration-less
    // Named type whose runtime value is the iterator-vtable object
    // ([0]=td,[1]=Dispose,[2]=MoveNext,[3]=Current — §29.7), NOT a user class.
    // isOwnedReferenceField (which requires a class/interface `declaration`) and
    // the other builtin-field predicates do not match it (it has no declaration
    // and is not an exception/collection/Shared/Subscription), so without this
    // the field is skipped by emitOwnedFieldDisposals and the iterator FRAME
    // (bz$C$Gen, incl. its owned captures) leaks at class disposal. Disposed via
    // the iterator-vtable slot-1 Dispose (which runs pending finally blocks and
    // frees the frame itself, §29.7), NOT the class-vtable slot-2 dynamicDispose
    // (which would call MoveNext and never free) — see emitOwnedFieldDisposals.
    //
    // #662 foundation B: for IMPORTED fields (fs->decl == nullptr), the field's
    // type rides ctx_.importedMemberType(fs). Mirrors isOwnedBuiltinException
    // Field's structure (posParam branch from #1543/#1594 included).
    bool isOwnedIteratorInterfaceField(const sema::FieldSymbol* fs) {
        // §22.10 (v0.180) / #1594: a captured `owned` primary-ctor parameter
        // typed as IEnumerable<T>/IOwningEnumerable<T>. Mirrors isOwnedReference
        // Field's posParam branch (#1543); capturedOwnedPrimaryParamType is
        // defined further below (forward-declared use is fine — same class).
        if (fs->posParam)
            return isVtableBackedIEnumerableType(
                capturedOwnedPrimaryParamType(fs));
        if (!hasOwnedModifier(fs)) return false;
        // #662 foundation B: imported field — type from importedMemberType.
        if (!fs->decl)
            return isVtableBackedIEnumerableType(ctx_.importedMemberType(fs));
        auto* fd = dynamic_cast<const ast::FieldDecl*>(fs->decl);
        if (!fd || !fd->type) return false;
        return isVtableBackedIEnumerableType(ctx_.resolvedType(*fd->type));
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

    // §22.16 (v0.203, #2053): construct + throw a ContractViolationException
    // carrying `msgStr` (a lowered bz_string pointer) as its Message slot,
    // then unwind. The Message slot is at byte offset 8 of the uniform
    // kExcObjSize block (§31.4). Leaves the current block terminated.
    void emitThrowContractViolation(llvm::Value* msgStr);

    // §22.16 (v0.203, #2053): emit the runtime checks for a member's
    // `requires` clauses — the always-on prologue that runs AFTER argument
    // binding and BEFORE any of the member's own execution (before a
    // constructor's `base(...)`/`this(...)` chain, before field initializers,
    // before the first body statement). Each clause's expression is lowered
    // and inverted; a false result throws ContractViolationException with a
    // message naming the clause kind, the enclosing type, and the member.
    // Source-text-in-message is deferred (recorded decision; the message
    // carries kind + qualified member name for this slice).
    void emitRequiresPrologue(
        const std::vector<ast::ContractClause>& clauses,
        std::string_view typeName, std::string_view memberName);

    // §22.16 (v0.203, #2053) Slice 4b: arm the single-exit ensures epilogue
    // for the body about to be emitted. Sets up `ensuresFn_` /
    // `ensuresExitBB_` / `ensuresResultSlot_` so `emitReturn` redirects to
    // the epilogue, and the epilogue evaluates each `ensures` clause in
    // declaration order, throwing ContractViolationException on a false
    // clause. Returns true iff the epilogue was armed (i.e. `clauses`
    // contains at least one `ensures` clause). `returnTypeNode` is the
    // declared return type (null for constructors / unit methods); it is
    // carried so the epilogue can box/coerce the result temp exactly as the
    // original return would have done.
    bool armEnsuresEpilogue(
        llvm::Function* fn, const std::vector<ast::ContractClause>& clauses,
        const ast::Type* returnTypeNode,
        std::string_view typeName, std::string_view memberName);

    // §22.16 Slice 4b: emit the armed ensures epilogue at the current
    // insertion point. The caller (emitFunctionBody / emitConstructorBody)
    // arranges for the current block to be the epilogue's predecessor
    // (either via emitReturn's redirect or via the body's fall-through into
    // the epilogue). Reloads the result temp, evaluates each ensures clause,
    // throws on failure, then performs the original return (ret value / ret
    // void). Clears the armed state.
    void finalizeEnsuresEpilogue();

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

    // §16.3 (v0.183, #1586): compute the value `step` successor-applications
    // away from `cur` for a `Range<char>` iterator, skipping the UTF-16
    // surrogate gap U+D800-U+DFFF (the successor of U+D7FF is U+E000, the
    // predecessor of U+E000 is U+D7FF). `cur` and `step` are i32; `step` may
    // be negative (descending ranges apply the predecessor). Used at every
    // codegen site that advances a char-typed range's working value — both
    // the counted-loop `for` optimization (lowerRangeFor) and the runtime
    // lazy-enumerator path share this formula via a bijection with a dense,
    // gap-free index space: d(cp) = cp below the gap, cp - 0x800 above it;
    // stepping is plain addition in that space, then the inverse mapping
    // restores the code point. See bz_lazy_movenext's BZ_LAZY_RANGE case
    // (bezel_runtime_collections.cpp) for the runtime-side mirror.
    llvm::Value* emitCharRangeStep(llvm::Value* cur, llvm::Value* step);

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

    // §8.6 / §8.5 / §9.6 (#417, #946/#947/#948): the fixed vtable slot indices
    // of the uniform Object virtual members. Layout: [0]=td, [1]=itmap,
    // [2]=Dispose, [3]=ToString, [4]=Equals(Object?), [5]=GetHashCode,
    // [6..]=user virtuals. A dispatch `o.ToString()`/`o.Equals(x)`/
    // `o.GetHashCode()` on an Object/interface receiver loads the fixed slot and
    // calls it. The three Object methods (§9.6) each occupy a fixed slot so an
    // override is reachable through an Object-typed receiver, exactly as ToString
    // already was. `kVirtualMethodsBase` is where user-declared virtuals begin
    // (it follows the last fixed Object slot); buildVtable assigns absolute slot
    // indices from it, so growing this prefix shifts every user virtual uniformly.
    static constexpr unsigned kToStringSlot     = 3u;
    static constexpr unsigned kEqualsSlot       = 4u;
    static constexpr unsigned kGetHashCodeSlot  = 5u;
    static constexpr unsigned kVirtualMethodsBase = 6u;

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

    // §9.6 (#946/#947/#948): emit (once) an Equals thunk `i1(ptr this, ptr other)`
    // for a CLASS — the user `Equals(Object?)` override if present (walking the
    // class chain), else the §8.3 reference-identity default (`this == other`).
    // Returned as a Constant for the class vtable's slot kEqualsSlot.
    llvm::Constant* classEqualsThunk(const sema::TypeSymbol* ts);

    // §9.6 (#946/#947/#948): emit (once) a GetHashCode thunk `i32(ptr this)` for
    // a CLASS — the user `GetHashCode()` override if present, else the §8.3
    // identity-hash default (the pointer bits truncated to i32). Returned as a
    // Constant for the class vtable's slot kGetHashCodeSlot.
    llvm::Constant* classHashCodeThunk(const sema::TypeSymbol* ts);

    // §9.6 (#946/#947/#948): shared default Equals/GetHashCode thunks installed
    // in BOX vtables (boxed records/enums/primitives). They give the §8.3
    // reference-identity / identity-hash default so the uniform Object dispatch
    // (lowerObjectEquals/lowerObjectGetHashCode reads the fixed slot on every
    // vtable-bearing value) is in-bounds and never crashes. Structural/value
    // equality for a boxed value type reached through Object is a separate gap
    // (tracked as a follow-up issue), not these defaults.
    llvm::Constant* boxDefaultEqualsThunk();
    llvm::Constant* boxDefaultHashCodeThunk();

    // §8.6 / §2.5 (#1945): the ToString thunk `ptr(ptr this)` for a boxed VALUE-
    // BUILTIN (DateTime/…) — returns the type name (the class-default form), so
    // the box vtable's slot 3 is a valid function rather than a null that would
    // fault on an `Object.ToString()` dispatch. Per-type value formatting is out
    // of scope (it belongs to the value-type's own methods).
    llvm::Constant* valueBuiltinBoxToStringThunk(const char* name);

    // §9.1 / §9.6 (#961): per-type structural Equals `i1(ptr this, ptr other)`
    // and GetHashCode `i32(ptr this)` thunks for boxed RECORDS and ENUMS.
    // Equals guards `other` is a box of the same type (matching vtable[0]
    // descriptor), then loads both payloads and runs emitStructEquality (record)
    // or an integer-value compare (enum). GetHashCode loads the payload and runs
    // emitRecordHashCode (record) or emitValueHashCode (enum). These replace the
    // box-default reference-identity thunks at kEqualsSlot/kGetHashCodeSlot.
    llvm::Constant* boxRecordEqualsThunk(const sema::TypeSymbol* ts);
    llvm::Constant* boxRecordHashThunk(const sema::TypeSymbol* ts);

    // #2176 (§9.1/§9.6, spec v0.226): PER-INSTANTIATION Equals/GetHashCode
    // thunks for a CONSTRUCTED GENERIC RECORD. The shared declaration-level
    // thunk (boxRecordEqualsThunk/boxRecordHashThunk above) is emitted with NO
    // active genericSubst_, so recordStruct(ts) returns the per-declaration
    // TEMPLATE struct (erased type-parameter fields) and a type-parameter field
    // degrades to its i32 placeholder — a boxed `Pair<string,int>("ab",2)`
    // then compared the low 32 bits of two distinct "ab" allocations, reporting
    // same-value pairs as unequal. These helpers save/install/restore a
    // genericSubst_ + genericSubstSemaTy_ built from `args` (one entry per
    // ts->genericParams, positional) so the emitted thunk's recordStruct and
    // recordMemberTypeResolved resolve the SUBSTITUTED concrete layout, then
    // delegate to the core emit path under a per-instantiation function name
    // (`bz$boxeq$<T>$<suffix>` / `bz$boxhash$<T>$<suffix>`). Mirrors the
    // per-instantiation vtable minted by specializedBoxVtableFor, which is the
    // sole caller and is itself cached per instantiation (so no extra cache is
    // needed here). `args` must be concrete (descArgsConcrete) and align
    // positionally with ts->genericParams; `suffix` disambiguates the function
    // name across instantiations (the readable mangle works).
    llvm::Constant* boxRecordEqualsThunkFor(const sema::TypeSymbol* ts,
                                            const std::vector<const sema::TypeRef*>& args,
                                            const std::string& suffix);
    llvm::Constant* boxRecordHashThunkFor(const sema::TypeSymbol* ts,
                                          const std::vector<const sema::TypeRef*>& args,
                                          const std::string& suffix);
    // #2176: shared emit cores for the two thunks above. `fnName` lets the
    // per-instantiation variants emit under a distinct function name. The
    // RECORD branch reads the ACTIVE genericSubst_ (recordStruct +
    // recordMemberTypeResolved); the shared thunk passes no subst, the
    // per-instantiation variants install one before calling these.
    llvm::Function* boxRecordEqualsThunkCore(const sema::TypeSymbol* ts,
                                             const std::string& fnName);
    llvm::Function* boxRecordHashThunkCore(const sema::TypeSymbol* ts,
                                           const std::string& fnName);

    // per §10.5 (v0.183): an enum may declare plain `ToString(): string`,
    // `Equals(E): bool`, and `GetHashCode(): int` members that REPLACE the
    // compiler-generated defaults for every consumer (interpolation §8.5, boxed
    // dispatch §8.6, hash containers §4.8/§4.10). enumUserMember finds such a
    // member (non-static, matching name + arity) or returns null. The three
    // emit helpers dispatch to the user member when present (passing the enum's
    // underlying integer as `this`, §10.3) and otherwise emit the §8.5/§8.3
    // default (type name / integer compare / integer hash). Centralizing the
    // user-or-default choice keeps the four consumer paths consistent.
    sema::MethodSymbol* enumUserMember(const sema::TypeSymbol* ts,
                                       std::string_view name, size_t nParams);
    llvm::Value* emitEnumToString(llvm::Value* enumVal,
                                  const sema::TypeSymbol* ts);
    llvm::Value* emitEnumEquals(llvm::Value* a, llvm::Value* b,
                                const sema::TypeSymbol* ts);
    llvm::Value* emitEnumHashCode(llvm::Value* enumVal,
                                  const sema::TypeSymbol* ts);

    // §9.1 / §9.6 (#961): per-primitive structural Equals and GetHashCode thunks
    // for boxed PRIMITIVES (int, bool, char, …). Equals guards `other` is a box
    // of the same primitive (matching vtable[0] descriptor), then loads both
    // scalar payloads and ICmpEQ / FCmpOEQ. GetHashCode loads the scalar and
    // runs emitValueHashCode with the primitive's TypeRef.
    llvm::Constant* primBoxEqualsThunk(sema::PrimitiveType prim, const char* nm);
    llvm::Constant* primBoxHashThunk(sema::PrimitiveType prim, const char* nm);

    // §8.6 / §5.1 (#1311): build the interface itable/itmap for a boxed
    // PRIMITIVE so virtual dispatch through a builtin generic interface it
    // implements (IComparable<T>/IEquatable<T>, stdlib §9.1–§9.5) reaches a
    // real CompareTo/Equals. Returns the itmap global (a null-terminated
    // [td$I, itab, …] pair list) to install at vtable slot 1, or nullptr when
    // the primitive implements no such interface or the interface descriptors
    // are unavailable. Mirrors ensureSpanBoxVtable's itable/itmap assembly.
    llvm::GlobalVariable* buildPrimBoxItmap(sema::PrimitiveType prim,
                                            const char* nm);

    // §9.6 (#946/#947/#948): lower `o.Equals(x)` / `o.GetHashCode()` where `o`
    // is Object/interface (no resolved MethodSymbol). Mirrors lowerObjectToString:
    // a string operand (no vtable) uses the string content intrinsic; a null
    // operand uses reference identity / hash 0; a class/box dispatches through
    // the fixed vtable slot. Equals returns i1 (so `not r` on an inferred result
    // is valid IR, #947); GetHashCode returns i32. nullptr if not lowerable.
    llvm::Value* lowerObjectEquals(llvm::Value* recv, llvm::Value* other);
    llvm::Value* lowerObjectGetHashCode(llvm::Value* recv);

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

    // §2.5–§2.6 (#1945/#1954): the value-builtin name (DateTime/TimeSpan/DateOnly/
    // TimeOnly/Guid — scalar/struct value types) a TypeRef denotes, across the three
    // TypeRef shapes (builtinClassSym decl, declaration-less annotation, imported),
    // IGNORING nullability. The single home for the value-builtin name set; both
    // valueBuiltinBoxName and isNullableValueBuiltinTy delegate here.
    const char* valueBuiltinName(const sema::TypeRef* rt);

    // §8.6 / §2.5–§2.6 (#1945): the stable box-key name for a boxable VALUE-
    // BUILTIN, keyed into primBoxDesc_/primBoxVtable_ so a boxed value-builtin
    // carries a walkable type descriptor (emitIsA) and boxValue can wrap it. Returns
    // nullptr for any other type and for a nullable form — a `DateTime?` → Object
    // box is a distinct shape, and a nullable value-builtin's tagged-struct layout
    // is keyed by isNullableValueBuiltinTy instead.
    const char* valueBuiltinBoxName(const sema::TypeRef* rt);

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

    // #1911 / stdlib §5 / §29.11: like wrapArrayAsIEnumerable, but the resulting
    // enumerator OWNS its backing `arr` — its slot-1 Dispose frees the array (a
    // fresh temporary with no other owner, e.g. a lazy LINQ pipeline drained via
    // bz_lazy_to_array). Its GetEnumerator (slot 4) hands out NON-owning clones
    // that borrow the shared array, so a per-loop enumerator disposes only its
    // own struct while the owning source frees the array exactly once at scope
    // end (leak-free, no double-free — mirrors the borrow/own split the plain
    // array wrapper relies on for a separately-owned Array<T> source).
    llvm::Value* wrapOwnedArrayAsIEnumerable(llvm::Value* arr,
                                             const sema::TypeRef* iEnumTy);

    // §29.2 (#437 / #1850): the cached machinery (struct type, vtable global,
    // element stride) for the array enumerator over a given lowered element type.
    // Extracted from wrapArrayAsIEnumerable so both the top-level wrap and the
    // per-element inner wrap of a nested covariant view (via ensureIEnumViewFactory)
    // share the exact same functions/vtable (keyed by element type).
    struct ArrEnumMachinery {
        llvm::StructType* stTy = nullptr;
        llvm::GlobalVariable* vtGV = nullptr;
        int64_t esz = 8;
    };
    ArrEnumMachinery ensureArrayEnumMachinery(llvm::Type* elemTy);

    // #2471 (stdlib §4.11; language spec §29.1, agreeing with #2472's
    // for-loop fix): the REVERSE-order twin of ensureArrayEnumMachinery /
    // wrapArrayAsIEnumerable, for Stack<T>/OwningStack<T>'s explicit
    // `.GetEnumerator()` call. Same 4-field struct layout { vtable, arr,
    // idx, esz } and the same Dispose/Current bodies, but MoveNext COUNTS
    // DOWN (`idx--; return idx >= 0`) instead of up, and the "before-first"
    // sentinel is `bz_array_len(arr)` (a RUNTIME value fetched fresh on
    // every wrap/clone) instead of the forward machinery's fixed `-1` —
    // reverse iteration's starting point depends on the collection's
    // current length, which the constant sentinel has no way to express.
    // A distinct name prefix ("stackenum."/"bz$stackenum$..." vs
    // "arrenum."/"bz$arrenum$...") keeps this cache fully separate from the
    // forward machinery even though the struct shape is identical — the two
    // must never be interchanged (a forward-cursor MoveNext on a
    // reverse-initialized struct, or vice versa, would silently read
    // out-of-bounds or terminate immediately).
    ArrEnumMachinery ensureStackEnumMachinery(llvm::Type* elemTy);
    llvm::Value* wrapStackAsIEnumerator(llvm::Value* arr,
                                        const sema::TypeRef* iEnumTy);

    // §29 / §12.9 (#1850): wrap an array-backed collection whose ELEMENTS are
    // themselves array-backed collections viewed covariantly as IEnumerable<T>
    // (`Array<Array<Dog>>` → `IEnumerable<IEnumerable<Dog>>`). The plain
    // wrapArrayAsIEnumerable hands out each stored element raw (a bz_array_t*),
    // but the consumer iterates it as `IEnumerable<T>` via vtable dispatch → a
    // null-function-pointer call (SIGSEGV). This variant's Current lazily
    // materialises a proper vtable-backed inner view per element (disposing the
    // previous one on MoveNext / at Dispose), so the doubly-nested for-in works.
    // `storedTy` is the concrete source collection type (e.g. Array<Array<Dog>>);
    // `viewTy` is the target IEnumerable<IEnumerable<T>>.
    llvm::Value* wrapNestedArrayAsIEnumerable(llvm::Value* arr,
                                              const sema::TypeRef* storedTy,
                                              const sema::TypeRef* viewTy);

    // §29 / §12.9 (#1850): a factory function `ptr F(ptr backingArray)` that
    // wraps a backing array-backed collection of `storedTy` as `viewTy`
    // (IEnumerable-family). Chooses a plain array enumerator when the element
    // representation matches the view, or a nested wrapper (recursing through
    // this factory for the inner level) when the element itself needs a
    // covariant-view re-representation. Cached/keyed by the wrap shape.
    llvm::Function* ensureIEnumViewFactory(const sema::TypeRef* storedTy,
                                           const sema::TypeRef* viewTy);

    // §29 / §12.9 (#1850): true when wrapping the array-backed collection
    // `storedTy` as the IEnumerable view `viewTy` requires the nested wrapper —
    // i.e. the ELEMENT is itself an array-backed collection viewed as a pure
    // (vtable-dispatched) IEnumerable<T>. The plain wrapArrayAsIEnumerable is
    // used otherwise (including ordinary single-level covariance).
    bool nestedIEnumViewNeeded(const sema::TypeRef* storedTy,
                               const sema::TypeRef* viewTy) const;

    // §29.2 / §4.3 (#1310): if `targetTy` is the pure declaration-less
    // `IEnumerable<T>` and the source value `srcExpr` is a dense array-backed
    // collection / a (Owned)LinkedList / a user class implementing IEnumerable,
    // wrap `v` into the vtable-backed enumerator (wrapArrayAsIEnumerable /
    // wrapLinkedListAsIEnumerable / wrapUserClassAsIEnumerable). Otherwise
    // return `v` unchanged. Shared by the call-argument path (buildOrderedArgs),
    // the local-decl init path, and the method-RETURN coercion so a value
    // coerced to IEnumerable<T> is ALWAYS a real vtable object (a raw
    // bz_array_t* returned as IEnumerable would SIGSEGV on iteration/disposal).
    //
    // #1916 / stdlib §5 / §29.11: `ownLazyPipeline` additionally handles a lazy
    // LINQ pipeline (a bz_lazy_t* sentinel, isLazyEnumerableType) widened to a
    // pure `IEnumerable<T>` — the same 4-slot-vs-5-slot crash #1911 fixed for the
    // local-decl case. It is gated on the flag because the disposal contract
    // differs by position: a RETURN value is OWNED by the caller (an owning
    // wrapper is leak-free — the four RETURN call sites pass true), whereas a
    // callee `IEnumerable<T>` PARAMETER is BORROWED. #1923 handles that ARGUMENT
    // position too: buildOrderedArgs passes true AND registers a scope-end
    // iterEnumDispose of the owning-enumerator arg temporary (the caller disposes
    // it exactly once, after the call, on both the normal and unwind edges — the
    // callee never disposes a borrowed arg), so the drained array is leak-free
    // and never double-freed.
    //
    // fix #1069 / stdlib §4.10: a `Set<T>` / `OwningSet<T>` source is handled
    // here too. A CLASS-element set is bz_array_t-backed and its backing IS the
    // dense element array, so it takes the plain (BORROWING) array enumerator; a
    // DICT-backed set (value/record/string element) is a hash table with no dense
    // element array, so it is snapshotted (bz_lazy_from_set → bz_lazy_to_array)
    // into a fresh array behind an OWNING enumerator. `outWrapMade`, when
    // non-null, reports whether the returned value is a freshly allocated
    // enumerator wrapper the CALLER must dispose exactly once — true for the
    // lazy-pipeline drain (#1916), the dict-backed-set snapshot (#1069), AND
    // the plain borrowing Array/Stack/Queue/LinkedList wraps whose slot-1
    // Dispose is `free(self)` (the wrapper frame only, never the shared
    // backing, exactly like the local-decl `wrappedHere` path #943/#1911).
    // A RETURN site can ignore it — the caller owns a returned enumerator by
    // construction (#1916); the ARGUMENT site must consult it and register
    // the scope-end iterEnumDispose (#1923, #2298).
    // `srcOwnedByCallee`, when true, signals that the source expression is a
    // callee-owned local whose underlying collection (Array/Stack/Queue/
    // LinkedList/dict-backed-Set) the wrap only BORROWS — fix #2299 / §28.9:
    // the wrap's slot-1 Dispose frees the wrapper frame but NEVER the backing,
    // and the source local's scope-end reclaim is suppressed by §28.3
    // ownership-transfer on `return`. This registers a scope-end disposal for
    // the source so the RETURN produces no leak — the wrapper frame is freed
    // first (LIFO), then the source collection. Caller MUST have verified
    // callee-ownership (a NewExpr, a producer-initialized local, or a move);
    // parameter/field/already-IEnumerable sources stay false to avoid freeing
    // someone else's collection.
    llvm::Value* wrapAsIEnumerableForTarget(llvm::Value* v,
                                            const sema::TypeRef* targetTy,
                                            const ast::Expr* srcExpr,
                                            bool ownLazyPipeline = false,
                                            bool* outWrapMade = nullptr,
                                            bool srcOwnedByCallee = false);
    // fix #2299 / §28.9 (RETURN-coercion source-ownership probe): is `src`
    // the OWNED producer of a heap-allocated collection whose RETURN transfer
    // moves ownership into the callee? Returns true for `new Array<int>()`,
    // `new LinkedList<int>()`, `new Set<int>()` etc. (a NewExpr for a class
    // type — §28.3 K1 — or a move expression), and for an IdentifierExpr
    // naming a LOCAL whose initializer was a producer form (NewExpr/CallExpr)
    // — mirrors producerState's "Owned" verdict at the OWNERSHIP pass. A bare
    // parameter/field read, a method-call result whose declared return is not
    // `owned`, and any initializer that is itself a borrow (an IdentifierExpr
    // naming another local, a MemberAccess, an IndexExpr — §28.3 "storage
    // lends") return false. Conservative: a shape we cannot classify without
    // a deeper ownership analysis defaults to false (do NOT free — better
    // leak a non-confirmed transfer than risk a UAF).
    bool returnSourceIsCalleeOwnedCollection(const ast::Expr* src) const;

    // fix #2299 / §28.9: emit an immediate free on the source collection
    // right at the RETURN coercion (NOT at scope end — scope-end disposal
    // would race the caller's iteration of the wrapping enumerator and
    // free the source out from under it). Only safe when the caller has
    // already established srcOwnedByCallee (the source is no longer needed
    // because the wrap takes its slot-1 backing; for Array-family the
    // OWNING wrap owns the source via its slot-1 Dispose; for Stack and
    // LinkedList the source is freed here and the caller disposes only the
    // wrapper frame; for dict-backed Set the snapshot is independent).
    void freeCollectionSourceNow(llvm::Value* v);
    void freeDictSourceNow(llvm::Value* v);
    void freeLinkedListSourceNow(llvm::Value* v);

    // #1916 / #1911 / stdlib §5 / §29.11: drain a lazy LINQ pipeline (bz_lazy_t*)
    // once into a fresh owned Array<T> (bz_lazy_to_array also disposes the
    // pipeline, releasing its inline-lambda closures) and wrap that array in an
    // OWNING vtable-backed enumerator (wrapOwnedArrayAsIEnumerable), suitable for
    // an `IEnumerable<T>` binding whose consumer disposes it exactly once — a
    // local (#1911) or a returned value the caller owns (#1916). `iEnumTy` is the
    // target `IEnumerable<T>` type; its genericArgs[0] supplies the element
    // stride. Shared by lowerLocalDecl and wrapAsIEnumerableForTarget.
    llvm::Value* drainLazyPipelineToOwnedIEnumerable(
        llvm::Value* lazyVal, const sema::TypeRef* iEnumTy);

    // #2218 / stdlib §4.5/§4.13 / spec §29.1/§29.2: wrap a bz_array_t*-backed
    // owning collection (OwningArray<T> / OwningSet<T> / OwningQueue<T> — the
    // .Drain() view returns the collection itself at static type
    // IOwningEnumerable<T>, per stdlib §4.5 "no separate enumerable object") in a
    // vtable-backed IOwningEnumerator<T>. The wrapper's MoveNext pops the next
    // element via bz_array_drain_next, TakeCurrent transfers ownership of the
    // pending element to the caller, and Dispose disposes any advanced-but-
    // untaken pending value (§29.1) then frees the wrapper. The wrapper BORROWS
    // the array — the source collection remains the array's owner and disposes
    // any remaining elements on its own scope end. Returns a fresh calloc'd
    // wrapper pointer (the caller / using-var machinery disposes it via slot 1).
    // Generic key matches the ensureArrayEnumMachinery naming so the same
    // per-element-type machinery is shared across an entire compilation unit.
    llvm::Value* wrapArrayAsIOwningEnumerator(llvm::Value* arr,
                                              const sema::TypeRef* iOwnEnumTy);

    // #2218 / stdlib §4.5/§4.13 / spec §29.1/§29.2: at the call site of
    // `recv.GetOwningEnumerator()`, detect whether `recv` is actually a
    // bz_array_t*-backed owning collection (owning array family, set, queue,
    // buffer) — typically `coll.Drain()` directly, or a local whose
    // initializer was a `Drain()` call on one of these. Returns a fresh
    // vtable-backed IOwningEnumerator<T> wrapper (via
    // wrapArrayAsIOwningEnumerator) on hit; null when the receiver is a
    // compiler-emitted iterator frame (the slot-4 dispatch path) or any
    // other non-builtin source. The detection mirrors lowerOwnedSeqDrainFor
    // (Stmts.cpp) which similarly recovers the original Drain() target from
    // a bound IOwningEnumerable<T> local so the for-loop can drain the
    // underlying collection instead of treating its data pointer as a
    // vtable. LinkedList and Dictionary need their own wrappers (out of
    // scope here) — those cases return null and fall through to the
    // iterator-frame path, which will surface as a SEGV just like the
    // bz_array_t case did before this fix. Stack is LIFO (owning); left for
    // a follow-up that mirrors the OwningStack drain loop.
    llvm::Value* tryBuildOwningEnumeratorForBuiltinSource(
        const ast::Expr* targetExpr, const sema::TypeRef* recvTy);

    // #1069: wrap a (Owned)LinkedList<T> — node-based bz_llist, NOT array-backed —
    // in the same vtable-backed enumerator the array wrapper produces, but walking
    // nodes (bz_llist_first_node / node_next / node_value_ptr) instead of an index.
    // The wrapper SHARES the list (cloning over the borrowed list pointer); a
    // second for-in replays from the head (§29.11). Cursor encoding: next_node is
    // the node MoveNext returns; current_node is what Current reads — no
    // started-flag needed (exhaustion is next_node == null).
    llvm::Value* wrapLinkedListAsIEnumerable(llvm::Value* list,
                                             const sema::TypeRef* iEnumTy);

    // #2471 (stdlib §4.8/§4.12; language spec §29.1, v0.236): wrap a
    // Dictionary<K,V>'s entries as a vtable-backed IEnumerable<DictionaryEntry<K,V>>.
    // Dictionary is hash-backed (bz_dict_t*) with a DictionaryEntry<K,V> Current
    // type, so (unlike Array/Stack/Queue) its entries are not stored as a dense
    // element array — they must be materialised. Snapshots the live keys +
    // values via bz_dict_keys_*/bz_dict_values_*, decodes each pair into a
    // {K, V} struct (matching lowerDictEntryFor's layout, including the
    // record-box and nullable-value shapes), appends each to a fresh owned
    // bz_array_t, frees the snapshot buffers, and wraps the result via
    // wrapOwnedArrayAsIEnumerable. The returned IEnumerable is OWNING — the
    // caller's slot-1 Dispose frees the materialised snapshot exactly once; the
    // SOURCE dictionary is untouched (no entry retained/released/disposed), so
    // an owning dictionary still disposes each value at its own binder's scope
    // end. `iEnumTy` is the call's result type — `IEnumerable<DictionaryEntry<K,V>>`
    // whose genericArgs[0] gives the entry element type/stride.
    llvm::Value* wrapDictionaryAsIEnumerable(
        llvm::Value* dict, const sema::TypeRef* iEnumTy);

    // fix #2522 (stdlib §4.8/§4.12; language spec §29.1, v0.236): the
    // string-keyed sibling of wrapOwnedArrayAsIEnumerable, used by
    // wrapDictionaryAsIEnumerable when the rebuilt DictionaryEntry<K,V>
    // array's K field (offset 0) holds a ref-counted bz_string*. The plain
    // owning Dispose (`bz_array_free(arr, null)`) cannot reach struct-
    // embedded string pointers — bz_array_free's element-disposer is
    // `void(*)(void*)` and treats each slot as a pointer. This wrapper's
    // Dispose walks arr->count slots via bz_array_ptr_at, calls
    // bz_string_release on the string at each struct offset 0, then frees
    // the array (null disposer) and the wrapper struct. MoveNext/Current
    // are reused from the plain array-enum machinery (they don't touch
    // element ownership). `entryStTy`/`esz` describe the materialised
    // {K, V} struct layout used to size the slot walk.
    llvm::Value* wrapDictionaryEntryArrayAsIEnumerable(
        llvm::Value* arr,
        const sema::TypeRef* iEnumTy,
        llvm::StructType* entryStTy,
        int64_t esz);

    // fix #2546 (stdlib §4.17/§4.18; language spec §29.1, v0.236):
    // SortedDictionary<K,V>.GetEnumerator() — snapshot+sort+wrap. Same
    // materialisation as wrapDictionaryAsIEnumerable, but with a sort step
    // after the snapshot so the entries iterate in ascending key order
    // (matching the for-loop path's #1426 promise). String keys use
    // bz_sort_kv_str (strncmp per §9.6); generic keys use bz_sort_kv_gen
    // with a per-element-type comparator built by buildSortedKeyComparator.
    // Lives in LlvmBackend_Calls2.cpp (not LlvmBackend_Assign.cpp) to avoid
    // the lane-D file-disjoint restriction; the wrap helper is identical
    // in spirit to wrapDictionaryAsIEnumerable but with the sort interleaved.
    llvm::Value* wrapSortedDictionaryAsIEnumerable(
        llvm::Value* dict, const sema::TypeRef* iEnumTy);

    // fix #2546 (stdlib §4.18; language spec §29.1, v0.236):
    // SortedSet<T>.GetEnumerator() — snapshot+sort+wrap. Same key-snapshot
    // pattern as wrapDictionaryAsIEnumerable (no values), but with a sort
    // step so the elements iterate in ascending order. Returns the same
    // OWNING-vtable-backed wrapper shape as wrapOwnedArrayAsIEnumerable,
    // so the same slot-4-getenumerator + slot-1-dispose dance in the
    // dispatch applies.
    llvm::Value* wrapSortedSetAsIEnumerable(
        llvm::Value* set, const sema::TypeRef* iEnumTy);


    // §29.2 (#552): wrap a user-class instance (implementing IEnumerable<T>)
    // in a type-specific vtable-backed enumerator compatible with the iterator
    // state machine protocol ([0]=td, [1]=Dispose, [2]=MoveNext→i1,
    // [3]=Current→T). Eagerly calls GetEnumerator() on the source and forwards
    // MoveNext/Current/Dispose to the concrete enumerator class's methods.
    // Used when a user class is passed (or assigned) where IEnumerable<T> is
    // expected, so that lowerIEnumerableBuiltinFor's vtable dispatch works
    // uniformly for all IEnumerable<T> sources. Returns src unchanged on
    // resolution failure (GetEnumerator or enumerator-class not found).
    //
    // fix #2424 (§28 preamble/§28.9, §29.1, v0.235): `ownsSrc` tells the
    // generated wrapper whether ITS OWN scope-end disposal is `src`'s ONLY
    // disposer (a genuinely unnamed temporary — `var s: IEnumerable<int> :=
    // new IntRange(...);`) as opposed to a NAMED, independently-owned source
    // that disposes itself at its own scope end (freeing it here too would
    // double-free). This is call-SITE data, not a per-(enumerator class)
    // constant — the SAME cached wrapper function (keyed on the (source,
    // enumerator) class pair, see the .cpp) may be reached from both a
    // temporary and a named call site across a program, so it is threaded
    // through as a stored runtime flag on the wrapper instance, not baked
    // into the shared IR function. Defaults to `false` (preserves the
    // pre-#2424 behavior — never dispose `src`) for every call site that has
    // not been individually analyzed for named-vs-temporary ownership.
    llvm::Value* wrapUserClassAsIEnumerable(llvm::Value* src,
                                            const sema::TypeSymbol* srcTs,
                                            const sema::TypeRef* iEnumTy,
                                            bool ownsSrc = false);

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

    // §28.5 / §14.1 / §2.5 (#662 foundation A): build (and cache) the
    // complete per-instantiation vtable for an IMPORTED generic class, using
    // the currently active `genericSubst_`.  Unlike `specializedVtableFor`
    // (which patches an existing shared vtable), this builds a fresh vtable
    // from scratch because the exporter emits NO shared `vt$<Type>` for a
    // generic template (§2.5: only public non-generic types export their
    // vtable).  The vtable carries:
    //   [0] per-instantiation descriptor (td$<Type>$specN — [self, null])
    //   [1] empty itmap (imported generics implement no interfaces here)
    //   [2] specialized Dispose (emitSpecializedDisposer, honoring owned T)
    //   [3] ToString thunk (type-name default or user override if in metadata)
    //   [4+] virtual method slots from methodFns_ where bodies exist
    // disposerCoreFns_[ts] is also populated (blocker-4 residue from B).
    // Cached in specializedVtables_[ts][specFieldTys].  Returns null if
    // something critical is missing (ts not a class, no active subst).
    llvm::GlobalVariable* buildImportedGenericVtable(const sema::TypeSymbol* ts);

    // specializedVtables_[ts][substituted field types] = cached per-
    // instantiation vtable global (slot 1 = specialized Dispose). Keyed by the
    // same field-type vector as specializedStructs_ so distinct instantiations
    // (Box<Res> vs Box<int>) get distinct vtables.
    std::map<const sema::TypeSymbol*,
             std::map<std::vector<llvm::Type*>, llvm::GlobalVariable*>>
        specializedVtables_;

    // #1843 (§12.7/§12.9/§19.6): per-monomorphization runtime type descriptor.
    // The shared `descArray_[ts]` is keyed by the base declaration, so every
    // instantiation of a generic type used to share ONE descriptor — making
    // `x is Box<int>` match a `Box<string>` (type confusion). A constructed
    // generic type `C<a…>` gets its OWN descriptor global here whose flat array
    // lists the per-instantiation descriptors of `C<a…>` and every transitive
    // (substituted) base/interface, so `emitIsA`'s pointer-identity walk
    // distinguishes instantiations. Keyed by a structural pointer-based string
    // (declaration identity + recursive type arguments) so `Box<string>` and
    // `Box<Dog>` — same LLVM layout `ptr` — still map to DISTINCT globals. The
    // `new` site (specializedVtableFor patches vtable slot 0) and the type-test
    // site (perInstDescriptorForTarget) both resolve to this same global.
    std::unordered_map<std::string, llvm::GlobalVariable*> instanceDesc_;
    // Arena of substituted TypeRefs synthesized while building a base chain
    // (`Ok<T>: Result<T>` at Ok<int> needs a Result<int> TypeRef); owned by the
    // Emitter so they outlive descriptor construction.
    std::vector<std::unique_ptr<sema::TypeRef>> descArena_;
    // Per-monomorphization vtable, keyed by the instance descriptor global so a
    // generic instantiation whose only per-instantiation need is its descriptor
    // (slot 0) still gets a distinct vtable. Finer than the layout key above
    // (Box<string> vs Box<Dog> share layout but differ in descriptor).
    std::map<const sema::TypeSymbol*,
             std::map<llvm::GlobalVariable*, llvm::GlobalVariable*>>
        specVtablesByDesc_;

    // #1843: the per-monomorphization descriptor factory. Returns a global whose
    // ADDRESS is `C<args>`'s runtime identity and whose CONTENTS are the flat
    // ancestor-descriptor array `emitIsA` walks. Falls back to the shared
    // `descArray_[ts]` for a non-generic type, an arity mismatch, or when any
    // argument is not a concrete (fully-resolved) type. Memoized.
    llvm::GlobalVariable* instanceDescriptorFor(
        const sema::TypeSymbol* ts,
        const std::vector<const sema::TypeRef*>& args);
    // #1843: resolve a type-test TARGET (`is`/`as`/case-pattern) to its per-
    // monomorphization descriptor when it is a constructed generic class or
    // interface with concrete arguments; nullptr otherwise (caller keeps its
    // existing shared-descriptor resolution).
    llvm::GlobalVariable* perInstDescriptorForTarget(const sema::TypeRef* rt);
    // §12.9/§14.2/§19.3/§31.2.1 (#2012, per the #2069 ruling): at a given
    // monomorphization, REJECT a runtime type-test target whose type-parameter-
    // mentioning generic argument substitutes to a GROUND but runtime-ERASED kind
    // (a function type, or a collapse at any depth) — the descriptor key would
    // collide with a sibling instantiation and bind a value at a wrong static type
    // (#2001/#2006 confusion class). An implementation-limitation diagnostic (the
    // #1943 device); lifts when function-type identity is reified (#1939). Returns
    // true iff it reported an error (caller bails with a well-formed placeholder).
    // Called at the four type-test surfaces only, NEVER at a new/box-mint site.
    bool rejectCollapsedTypeTestTarget(const sema::TypeRef* rt, SourceLocation loc,
                                       bool targetFromTypeVar = false);
    // #1843: substitute `map` (param-name → concrete TypeRef) through `t`,
    // allocating into descArena_ where substitution changes structure.
    const sema::TypeRef* substituteTypeRefLocal(
        const sema::TypeRef* t,
        const std::unordered_map<std::string_view, const sema::TypeRef*>& map);
    // #1843: a structural, pointer-based cache key + a readable LLVM global-name
    // fragment for a constructed generic type. The key disambiguates distinct
    // declarations that share a name; the name is only cosmetic (LLVM
    // uniquifies). Both recurse through the argument list.
    std::string descIdentityKey(const sema::TypeSymbol* ts,
                                const std::vector<const sema::TypeRef*>& args);
    // #1947: the structural, RECURSIVE identity key of a single (concrete)
    // constructed type — the per-argument counterpart of descIdentityKey. Encodes
    // the top-level declaration AND every nested type argument, so a self-nested
    // instantiation (`Wrap<int>` vs `Wrap<Wrap<int>>`) yields DISTINCT keys. Used
    // by buildSpecializedMethodFn's monomorphization cache, where the old
    // per-declaration `typeRefIdentityToken` collapsed them onto one specialized
    // body (first-seen-wins mis-dispatch). Delegates to appendDescKey.
    std::string typeRefIdentityKey(const sema::TypeRef* t);
    std::string descReadableMangle(const sema::TypeRef* t);
    // #1843: are all of `args` concrete (fully-resolved) types — i.e. carry a
    // real identity token, not an unbound generic parameter placeholder?
    bool descArgsConcrete(const std::vector<const sema::TypeRef*>& args);

    // #1939 leg 2 (§12.7/§12.9/§17.4): per-monomorphization BOX vtables for
    // constructed generic RECORDS with concrete args, keyed by descIdentityKey.
    // Slot 0 is a chain of [per-inst descriptor elements, shared chain
    // elements] so precise targets AND the name-resolved shared descriptor
    // (record destructuring) both match. Minted lazily at the box site
    // (boxVtableFor) and registered in allVtables_ (bz_obj_is_string).
    std::unordered_map<std::string, llvm::GlobalVariable*> specBoxVtable_;
    llvm::GlobalVariable* specializedBoxVtableFor(const sema::TypeRef* rt);
    // #1939 leg 3 (§12.7/§31.2.1): every CONCRETE constructed generic ancestor
    // instantiation of `ts` — (base symbol, transitively-substituted concrete
    // args) pairs reached through baseTypeRefs. Used to enrich a SHARED
    // descriptor chain with the per-instantiation base descriptors so a
    // non-generic type deriving/implementing `K<string>` matches the precise
    // `is K<string>` / catch-arm target. Mints nothing (usable for sizing).
    std::vector<std::pair<const sema::TypeSymbol*,
                          std::vector<const sema::TypeRef*>>>
    collectConcreteGenericBaseInsts(const sema::TypeSymbol* ts);

    // #816: per-instantiation CORE disposer (unguarded destructor body + owned-
    // field disposal + base chain) for a class reached as a BASE of a specialized
    // derived class. emitBaseDisposeChain calls this (instead of the shared,
    // type-erased disposerCoreFns_[base]) so the base's owned type-parameter
    // field is disposed with the substitution active. Keyed like specializedVtables_.
    std::map<const sema::TypeSymbol*,
             std::map<std::vector<llvm::Type*>, llvm::Function*>>
        specializedDisposerCores_;
    llvm::Function* specializedDisposerCoreFor(const sema::TypeSymbol* ts);

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
    // CG20: lazy declaration of bz_exc_addr() (returns &bz_exc for this thread).
    llvm::GlobalVariable* excGlobal();
    // CG20 / fix #704: emit a call to bz_exc_addr() and return the resulting
    // ptr — the address of the calling thread's bz_exc slot.  Use this
    // instead of excGlobal() for all load/store of the exception slot.
    // bz_exc_addr() is a plain extern "C" function so the JIT resolves it via
    // DynamicLibrarySearchGenerator (unlike a TLS GlobalVariable which the JIT
    // would lower via __emutls_v.bz_exc — a symbol not in the host dylib).
    llvm::Value* emitExcSlot();

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

    // fix #1567 / §29.7/§28.10 (v0.182): the NORMAL-exit analogue of
    // emitUnwindDisposalsInRange — dispose disposeStack_ scopes in the half-open
    // index range [lo, hi), innermost (higher index) first, on a non-unwind exit
    // edge (isUnwind=false: caughtException entries are NOT skipped). Used by a
    // `return` (and the finally normal-exit dispatch it routes through) to
    // dispose the scopes between two successive finally bodies in reverse
    // establishment order, so an enclosing `using var` is disposed AFTER an inner
    // finally rather than before it.
    void emitDisposalsInRange(std::size_t lo, std::size_t hi);

    // CG20/CG21: after a call that may throw, branch to the unwind target
    // when the exception global is non-null (disposing unwound using-vars
    // first), else continue.
    void emitPendingCheck();

    // CG20 + #792 / §22.13: route the pending exception ONLY when `cond` is
    // true. emitPendingCheck computes its own condition (bz_exc != null, ANDed
    // with the #1468 disposer-body "is-new" gate) and delegates here. Callers
    // that have ALREADY snapshotted bz_exc around an operation that could raise
    // (notably the §22.13 static-init guard in emitEnsureStaticInit) pass a
    // "the operation raised a NEW exception" condition so a PRE-EXISTING
    // in-flight exception (we were called during an exception unwind) is NOT
    // misrouted here — it stays in bz_exc and routes at the next natural
    // pending check. The unwind block (scope disposal + #1216 supersede) is
    // shared verbatim with emitPendingCheck.
    void emitPendingCheckIf(llvm::Value* cond);

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

    // §28.3 (#1968): the CONSTRUCTOR-ARGUMENT variant of emitNewPendingCheck.
    // `lowerNew` calloc's the instance BEFORE it evaluates the constructor
    // arguments; when a LATER argument's evaluation throws, `obj` is already
    // allocated but NOT yet constructed (every field is calloc-zeroed — the ctor
    // never ran), and it is bound to no `using var`/scope, so the ordinary
    // scope-end / owned-local unwind disposal never reclaims it → leak. Return a
    // cleanup block that RAW-frees the uninitialized instance block (a plain
    // `free` — NOT a dispose: no destructor, no owned-field disposal, because no
    // field is initialized) and then continues to the CURRENT enclosing unwind
    // (`unwindBB()`, captured before the caller overrides `unwindTarget_`). The
    // caller installs this as `unwindTarget_` ONLY while the constructor
    // arguments are evaluated and restores the previous target before the ctor
    // body/call runs — whose own cleanup (inlineCtorBody / emitNewPendingCheck)
    // then owns the block's disposal, so the two edges are disjoint and never
    // double-free. `unwindScopeDepth_` is left unchanged, so the pending-check at
    // the arg site still disposes the same enclosing scopes (including any
    // already-evaluated owned-temp / transferred-owned-arg registered by
    // buildOrderedArgs, #993/#1918) BEFORE branching here to free `obj`.
    llvm::BasicBlock* newArgUnwindCleanup(llvm::Value* obj);

    // fix #1073 / §28.14 / §28.3 "Exception-path discharge" / §28.9: the
    // EXCEPTION-edge half of the call-bounded closure ARGUMENT-PASS release.
    // Builds a cleanup block that drops the producer's +1 on each inline-closure
    // argument and then continues to the CURRENT enclosing unwind (`unwindBB()`,
    // captured before the caller overrides `unwindTarget_`). `lowerCall`
    // installs it as `unwindTarget_` ONLY across `emitMethodCall` and restores
    // the previous target immediately after, so its predecessors are exactly the
    // unwind edges of that call — disjoint from the normal-path release emitted
    // in the pending check's `cont` block. See the definition for the
    // exactly-once argument.
    llvm::BasicBlock* closureArgUnwindCleanup(
        const std::vector<llvm::Value*>& closures);

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

    // #1833 (b) / #1467 / §22.4 / §28.3: for a `discardOwnedOutSlots_` range
    // `[base, end)` recorded by buildOrderedArgs on the constructor call path,
    // emit the disposal+reclaim of each slot that was written by a normal
    // constructor return (the `!diverged_` guard). The loop is byte-identical
    // to lowerCall's end-of-statement disposal pass: null-guard (the slot was
    // null-initialized before buildOrderedArgs; a false-returning ctor may
    // leave it null), dynamic vtable Dispose + reclaim, store null back
    // (so any later debug-mode readers observe the post-dispose state).
    // Drives `new T(out _)` at the top level / statement context (issue #1833
    // (b)), strictly mirroring how lowerCall drives the same shape for method
    // calls (the #1467 path). On the divergent path (the ctor threw) the
    // slots are untouched: an unwind disposes the heap block via the
    // enclosing `obj` reclamation, not through these scratch slots. The
    // resize argument trims the stack back to `base` so a NESTED discard
    // inside an arg has its own scope, exactly like lowerCall's stack
    // discipline.
    void disposeNewDiscardOwnedOutSlots(std::size_t base);


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
    // `owningFamily` is true when the containing collection is in the "Owning"
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

    // #2608 / #2635 / §28.11 / stdlib §4.3: record an OWNING array-backed
    // collection's element disposer in its `bz_array_t` header at CONSTRUCTION,
    // so an erased free site can recover it (bz_array_free_dynamic).
    //
    // Element ownership is otherwise a purely STATIC property, chosen at the
    // free site from the collection's static type. That fails for an `owned`
    // field typed by a read-only interface SHARED by both hierarchies
    // (stdlib §4.3: "The two hierarchies share `IEnumerable<T>`,
    // `IReadOnlyCollection<T>`, and `IReadOnlyList<T>` as common ancestors"),
    // where one static type legitimately carries EITHER backing and the free
    // site cannot tell which. The concrete type IS known at construction.
    //
    // WHY THIS IS A SHARED MEMBER RATHER THAN #2608's FUNCTION-LOCAL LAMBDA
    // (#2635): #2608 inlined the stamp as a `[&]` lambda inside lowerNew's
    // array arm, capturing that arm's `colName`/`rt`. It was therefore
    // unreachable from the OTHER construction sites, which call
    // `bz_array_alloc` directly and silently produced UNTAGGED owning arrays —
    // a residual element leak, mechanically confirmed. Pairing the allocation
    // with the tag in one callable is what stops the next construction site
    // from skipping it.
    //
    // NEVER over-disposes: `owning` is the caller's CONCRETE-type test, a
    // borrowing collection is never tagged, and an untagged header holds NULL,
    // which is exactly the pre-#2608 behaviour.
    void stampOwningElemDisposer(llvm::Value* arr,
                                 const sema::TypeRef* elemTy,
                                 bool owning) {
        if (!arr || !owning) return;
        // #816: inside a specialized body the element may still be a generic
        // parameter — resolve it so elementPolicyFor can build the thunk (a
        // no-op outside a substitution).
        llvm::Function* d =
            elementPolicyFor(resolveTypeParamRef(elemTy), /*owningFamily=*/true)
                .disposerThunk;
        if (!d) return;   // value/unknown elements: nothing to run
        auto* setFn = declareC("bz_array_set_elem_disposer",
                               llvm::Type::getVoidTy(llvm_),
                               {ptrTy(), ptrTy()});
        builder_.CreateCall(setFn, {arr, d});
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
    llvm::Function* subscriptionDisposeThunk(); // §2.11 (#838): Subscription elem
    // §28.11 (#868): void(void*) thunk that calls the right free fn on a nested
    // builtin owning collection element (e.g. the OwningArray<R> inside an
    // OwningArray<OwningArray<R>>). Recursively resolves the inner element's
    // disposer via elementPolicyFor. Cached per element-collection TypeRef*.
    llvm::Function* nestedCollectionDisposerThunk(const sema::TypeRef* elemCollTy);

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
    // strictly the value-BOX-free case: an OwningDictionary<K, class> instead
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

    // issue #400 / §25.2: compute the equality `Array<T>`/`OwningArray<T>`'s
    // .Remove(item) uses to match `el` against `target`, dispatched
    // by the element's static Bezel type `elemTy`. A class with a user `Equals`
    // override → a (null-guarded) virtual `el.Equals(target)` call (§8.4); a
    // class without one → reference identity (§8.3); a record → structural
    // equality (§8.3, emitStructEquality); a string → content (§4.6); a value
    // type → `==`. `el` and `target` are both pure borrows (no consumption).
    llvm::Value* emitRemoveElementEquality(llvm::Value* el, llvm::Value* target,
                                           const sema::TypeRef* elemTy,
                                           bool elemIsClass);

    // stdlib §4.10 / §8.3 (#837 / #1474 / #1070): dedup-insert `item` into the
    // ARRAY-BACKED `Set<T>`/`OwningSet<T>` whose bz_array_t backing is `setArr`
    // (a class/interface element T — genericBackingType declines those, so the
    // set is a linear-scan array, not a hash table). A duplicate (per the §8.3
    // equality of `elemRef`) is passed to `dupDisposer` when non-null (the
    // OWNING family disposes the rejected element, §4.10) and not appended;
    // otherwise `item` is appended via bz_array_add. Shared by the `.Add` /
    // `.UnionWith` call paths and the §15.4 literal materializer. Returns false
    // only on an internal emitRemoveElementEquality failure.
    bool emitArrayBackedSetInsert(llvm::Value* setArr, llvm::Value* item,
                                  const sema::TypeRef* elemRef,
                                  llvm::Function* dupDisposer, int64_t esz);

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

    // #816 / §28.5/§28.11: is `fs` an `owned` generic OWNING-collection field
    // (OwningArray<T> / OwningStack<T> / OwningQueue<T> / OwningLinkedList<T> /
    // OwningSet<T> / OwningBuffer<T> / OwningDictionary<K,T>) whose disposed element
    // type is a generic type PARAMETER of the enclosing class? Such a field needs
    // a PER-INSTANTIATION disposer: only an owning collection disposes its
    // elements, and the element's concrete type (and so whether each element must
    // be disposed at all) is unknown in the shared, type-erased disposer — so the
    // elements leak unless the disposer is emitted with the substitution active.
    // A borrowing `owned Array<T>` field (frees the backing, borrows elements) is
    // intentionally excluded: its elements are never disposed regardless of T.
    bool isOwnedTypeParamCollectionField(const sema::FieldSymbol* fs) {
        const sema::TypeRef* collTy = ownedCollectionFieldType(fs);
        if (!collTy || !collTy->astNode || !collTy->astNode->name ||
            collTy->astNode->name->parts.empty())
            return false;
        // Owning families (name prefixed "Owning") dispose their elements.
        if (!common::isOwningPrefixedName(collTy->astNode->name->parts[0])) return false;
        // OwningDictionary owns its V value (genericArgs[1]); the ordered/set
        // families own their single element (genericArgs[0]).
        bool isDict = isDictTypeName(collTy);
        const sema::TypeRef* elem =
            isDict ? (collTy->genericArgs.size() > 1 ? collTy->genericArgs[1]
                                                     : nullptr)
                   : (!collTy->genericArgs.empty() ? collTy->genericArgs[0]
                                                    : nullptr);
        if (!elem || elem->kind != sema::TypeRef::Kind::Named ||
            elem->declaration)
            return false;
        std::string_view pname;
        if (elem->astNode && elem->astNode->name &&
            elem->astNode->name->parts.size() == 1)
            pname = elem->astNode->name->parts[0];
        else if (!elem->importedName.empty())
            pname = elem->importedName;
        else
            return false;
        if (!fs->parent) return false;
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
    // §22.10 (v0.180): an `owned` CLASS primary-constructor parameter captured
    // into a compiler-generated `owned` private field. The `owned` modifier
    // rides `posParam->modifier` (not `fs->modifiers`), and disposal applies
    // only to a *captured* parameter — one referenced in a body position and
    // NOT in any initializer position (posParamBodyRef && !posParamInitRef). A
    // parameter consumed by an owned-field initializer (the §11.1
    // `ResourceHolder` pattern) is disposed by that explicit field, so disposing
    // the capture slot too would double-free; the conservative "no initializer
    // reference" gate keeps the capture slot the sole owner. The typechecker
    // sets the two flags (ExprTypecheck_Exprs.cpp).
    static bool isCapturedOwnedPrimaryParam(const sema::FieldSymbol* fs) {
        return fs->posParam && fs->parent &&
               fs->parent->kind == ast::TypeDeclKind::Class &&
               fs->posParam->modifier == "owned" &&
               fs->posParamBodyRef && !fs->posParamInitRef;
    }

    // §22.10 (v0.180) / #1594: the resolved declared type of a captured `owned`
    // primary-ctor parameter (isCapturedOwnedPrimaryParam(fs)), or null when
    // `fs` isn't one. Its type rides `posParam->type` (fs->decl is the owning
    // TypeDecl, not a FieldDecl — see isOwnedReferenceField below). Shared by
    // the Shared<T>/Weak<T>, owning-collection, Subscription, and builtin-
    // exception owned-field predicates below, mirroring the class/interface
    // branch isOwnedReferenceField already has (#1543).
    const sema::TypeRef* capturedOwnedPrimaryParamType(
        const sema::FieldSymbol* fs) const {
        if (!isCapturedOwnedPrimaryParam(fs) || !fs->posParam->type)
            return nullptr;
        return ctx_.resolvedType(*fs->posParam->type);
    }

    bool isOwnedReferenceField(const sema::FieldSymbol* fs) {
        // §22.10 (v0.180): a captured `owned` primary-ctor parameter. Its type
        // rides `posParam->type` (fs->decl is the owning TypeDecl, not a
        // FieldDecl), so the FieldDecl-based extraction below never matches it.
        if (fs->posParam) {
            if (!isCapturedOwnedPrimaryParam(fs) || !fs->posParam->type)
                return false;
            const sema::TypeRef* pt = ctx_.resolvedType(*fs->posParam->type);
            if (!pt || pt->kind != sema::TypeRef::Kind::Named || !pt->declaration)
                return false;  // (generic-param / Shared / collection params: a
                               // narrower obligation left to a follow-up — never
                               // a double-free, at worst the pre-v0.180 leak)
            auto pk = pt->declaration->kind;
            return pk == ast::TypeDeclKind::Class ||
                   pk == ast::TypeDeclKind::Interface;
        }
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
            if (!concr || concr->kind != sema::TypeRef::Kind::Named)
                return false;
            // §28.5 (#937): a type param substituting to a heap COLLECTION is
            // disposed by the collection path (ownedCollectionFieldType), which
            // frees the backing AND cascades into class elements — NOT here,
            // which would free the backing but leak the elements. Defer to it.
            if (isCollectionTypeName(concr) || isLinkedListTypeName(concr) ||
                isDictTypeName(concr))
                return false;
            if (!concr->declaration) return false;
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
        // §22.10 (v0.180) / #1594: a captured `owned` primary-ctor parameter of
        // Shared<T>/Weak<T> type. Mirrors isOwnedReferenceField's posParam
        // branch (#1543) — Shared/Weak are compiler-known builtins with no
        // user TypeDecl, so `declaration` is null and the name comes from
        // astNode (a local field's posParam type is always locally resolved,
        // never an imported side-table lookup).
        if (fs->posParam) {
            const sema::TypeRef* pt = capturedOwnedPrimaryParamType(fs);
            if (!pt || pt->kind != sema::TypeRef::Kind::Named || pt->declaration)
                return false;
            if (!pt->astNode || !pt->astNode->name ||
                pt->astNode->name->parts.empty())
                return false;
            std::string_view n = pt->astNode->name->parts[0];
            if (n == "Shared") { if (isWeak) *isWeak = false; return true; }
            if (n == "Weak")   { if (isWeak) *isWeak = true;  return true; }
            return false;
        }
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

    // §28.5/§24.4 (#879): true when `fs` is an `owned` field of type
    // Subscription (or Subscription?).  Subscription is a compiler-known opaque
    // handle with no user TypeDecl / no vtable; it is disposed via
    // bz_subscription_dispose (which is null-safe: `if (!subp) return;`), not
    // through the vtable-Dispose path.  This mirrors isOwnedSharedWeakField.
    bool isOwnedSubscriptionField(const sema::FieldSymbol* fs) const {
        // §22.10 (v0.180) / #1594: a captured `owned` primary-ctor parameter of
        // Subscription type. Mirrors isOwnedReferenceField's posParam branch
        // (#1543).
        if (fs->posParam) return isSubscriptionType(capturedOwnedPrimaryParamType(fs));
        if (!hasOwnedModifier(fs)) return false;
        const sema::TypeRef* rt = nullptr;
        if (!fs->decl) {
            rt = ctx_.importedMemberType(fs);
        } else {
            auto* fd = dynamic_cast<const ast::FieldDecl*>(fs->decl);
            if (!fd || !fd->type) return false;
            rt = ctx_.resolvedType(*fd->type);
        }
        return isSubscriptionType(rt);
    }

    // §6.5 / §24.3 (#1646): true when `fs` is an `owned` field of type
    // EventSource<T> or AsyncEventSource<T> — the producer-side event sources a
    // user may now construct directly (`new EventSource<T>()`; the pattern's
    // `private owned readonly _e: EventSource<T> := new();`). These are
    // compiler-known heap objects with no user TypeDecl / no vtable (the same
    // bz_event_source_t* emitInstanceInit mints for a keyword-form event); they
    // are freed via bz_event_source_free (null-safe — `if (!s) return;`), which
    // frees the subscriber (handler) list and the struct, NOT the borrowed handler
    // closures (those are owned by their Subscriptions, disposed separately). This
    // mirrors isOwnedSharedWeakField / isOwnedSubscriptionField: a builtin runtime
    // object released outside the vtable-Dispose path. Async and sync sources share
    // one runtime representation, so both free identically.
    bool isOwnedEventSourceField(const sema::FieldSymbol* fs) const {
        if (fs->posParam) {
            const sema::TypeRef* pt = capturedOwnedPrimaryParamType(fs);
            return isEventSourceTypeName(pt);
        }
        if (!hasOwnedModifier(fs)) return false;
        const sema::TypeRef* rt = nullptr;
        if (!fs->decl) {
            rt = ctx_.importedMemberType(fs);
        } else {
            auto* fd = dynamic_cast<const ast::FieldDecl*>(fs->decl);
            if (!fd || !fd->type) return false;
            rt = ctx_.resolvedType(*fd->type);
        }
        if (!rt || rt->kind != sema::TypeRef::Kind::Named || rt->declaration)
            return false;
        if (isEventSourceTypeName(rt)) return true;
        // #662 foundation B: an IMPORTED field has no astNode; the type name
        // rides importedName (isEventSourceTypeName inspects astNode only).
        if (!fs->decl) {
            std::string_view n = rt->importedName;
            return n == "EventSource" || n == "AsyncEventSource";
        }
        return false;
    }

    // #362 / stdlib §4.5: if `fs` is an `owned` field whose type is a heap
    // collection (Array<T>/OwningArray<T>/…, builtins with no TypeSymbol so
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
        // §22.10 (v0.180) / #1594: a captured `owned` primary-ctor parameter
        // of a concretely-typed owning-collection (e.g. `owned v: OwningArray
        // <Cell>`). Mirrors isOwnedReferenceField's posParam branch (#1543).
        // Unlike the FieldDecl path below, no type-PARAMETER substitution
        // resolution is attempted here (a bare `owned v: T` captured param
        // where T substitutes to a collection is the generic-param case,
        // tracked separately — #1594 piece 1, not a double-free either way).
        if (fs->posParam) {
            const sema::TypeRef* rt = capturedOwnedPrimaryParamType(fs);
            if (isCollectionTypeName(rt)) return rt;
            if (isLinkedListTypeName(rt)) return rt;
            if (isDictTypeName(rt)) return rt;
            // #2603: an owned IList<T>/IReadOnlyList<T> captured primary-ctor
            // parameter is array-backed + borrowing (issue #401) — same path as
            // the FieldDecl arm below. #2606: IOwningList<T> (owning) is also
            // admitted; collIsOwning at the disposal sites marks it owning.
            if (isBorrowingListInterfaceName(rt)) return rt;
            if (isOwningListInterfaceName(rt)) return rt;
            return nullptr;
        }
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
        // §28.5 (#937): an owned type-PARAMETER field (`owned _v: T`) whose
        // concrete instantiation is a heap collection (`T = OwningArray<Cell>`)
        // is a collection field — it must free its backing AND cascade disposal
        // into its class elements. Resolve the bare type param to its concrete
        // substituted type (only when a per-instantiation substitution is
        // active) and classify on THAT; returning the concrete (not the bare
        // `T`) gives emitOwnedFieldDisposals the right name + genericArgs for
        // the element disposer. Without this the field falls to
        // isOwnedReferenceField (or to nothing) and its elements leak.
        if (rt && rt->kind == sema::TypeRef::Kind::Named && !rt->declaration) {
            if (const sema::TypeRef* concr = ownedTypeParamConcreteType(fs))
                rt = concr;
        }
        if (isCollectionTypeName(rt)) return rt;
        // per §4.6/§4.7: owned LinkedList<T>/OwningLinkedList<T> fields are also
        // heap collections — freed via bz_llist_free, not bz_array_free; but
        // they are identified here so emitOwnedFieldDisposals handles them.
        if (isLinkedListTypeName(rt)) return rt;
        // §28.5/§28.11, #893 #894: owned Dictionary<K,V>/OwningDictionary<K,V>
        // fields are heap-backed (bz_dict_t*) and must be freed via bz_dict_free.
        // They own their V values for the OwningDictionary case; emitOwnedFieldDisposals
        // and lowerOwnedCollectionFieldStore detect isDict and use genericArgs[1].
        if (isDictTypeName(rt)) return rt;
        // #2603 / §28.9 / issue #401: an owned IList<T> / IReadOnlyList<T> field
        // is array-backed (same bz_array_t* as Array<T>) and borrowing, so it
        // routes through the SAME collection-disposal path as owned Array<T> —
        // emitOwnedFieldDisposals / lowerOwnedCollectionFieldStore free its
        // backing via bz_array_free with the element disposer from
        // elementPolicyFor (stringElementReleaseThunk for a string element).
        // isCollectionTypeName only admits CONCRETE builtin names, so the
        // borrowing list interfaces must be admitted explicitly. #2606: an
        // owned IOwningList<T> field is the OWNING sibling — array-backed (same
        // bz_array_t* as OwningArray<T>, the only implementer per stdlib §4.5)
        // and owning, so it routes through the same path with collIsOwning=TRUE
        // (the disposal sites consult isOwningListInterfaceName to mark it
        // owning, since isOwningPrefixedName("IOwningList") is false). All
        // three helpers require declaration==nullptr, so a user class literally
        // named IList/IOwningList keeps the existing class-instance path.
        if (isBorrowingListInterfaceName(rt)) return rt;
        if (isOwningListInterfaceName(rt)) return rt;
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

    // fix #1747 / §28.9 / §9.7 / §28.3: does record `ts` have at least one
    // `string` storage member (positional field, body field, or auto-property)?
    // Such a field holds a runtime-managed reference whose +1 must be
    // released when the record value dies (§28.9, §9.7 — a record has no
    // disposer, so the discipline falls on the caller / disposal walker).
    //
    // fix #2405 / §9.7 / §28.9: RECURSES into a non-nullable nested-record
    // storage member — a string owned two-or-more levels deep still needs
    // releasing, matching emitRecordStringFieldReleases_impl's mirrored
    // recursion (see its comment for the termination argument: sema rejects
    // a record that transitively contains itself by value, §9, so this walk
    // is over a finite DAG). This is the SHARED classifier both the
    // deserialize disposal-scope registration (#2398/#2405,
    // LlvmBackend_Serialization.cpp) and the #1747 record-call-result
    // scope-end release (LlvmBackend_Setup.cpp) key off of via
    // isRecordWithStringFieldsTypeRef below — recursing here is required
    // for BOTH sites' matching release walk to ever recurse past one level
    // (each site's own release walker calls this same function to decide
    // whether to descend into a nested-record member), and is safe for the
    // #1747 site: a record value's fields carry whatever +1 was minted at
    // ITS OWN construction (no separate per-level retain step exists to
    // fall out of sync with — #1747's own landed commit message: "no extra
    // +1 mint is needed ... the record VALUE carries its fields' +1
    // references to the caller" — a fact that holds compositionally at any
    // nesting depth, since a by-value struct copy/embed never touches a
    // refcount).
    // fix #2425 / §9.8 / §35.4 v0.235: ALSO recurses into a non-nullable
    // CHOICE-typed storage member — a record component may itself be a
    // value-like choice (§9.8's record-admissibility clause), whose own
    // payload fields (in any declared case) may own a string directly or
    // transitively through a further nested record/choice payload. This is
    // the choice-side mirror of the #2405 nested-record recursion just
    // above; see choiceHasStringFields (LlvmBackend_Disposal.cpp) for the
    // choice-side termination argument (§35.1 self-containment).
    bool recordHasStringFields(const sema::TypeSymbol* ts) {
        if (!ts || ts->kind != ast::TypeDeclKind::Record) return false;
        std::vector<const sema::Symbol*> storage;
        collectRecordStorage(ts, storage);
        for (const sema::Symbol* m : storage) {
            if (isStringStorageMember(m)) return true;
            const sema::TypeRef* mty = recordMemberType(m);
            if (mty && !mty->nullable &&
                mty->kind == sema::TypeRef::Kind::Named && mty->declaration &&
                mty->declaration->kind == ast::TypeDeclKind::Record &&
                recordHasStringFields(mty->declaration))
                return true;
            if (mty && !mty->nullable &&
                mty->kind == sema::TypeRef::Kind::Named && mty->declaration &&
                mty->declaration->kind == ast::TypeDeclKind::Choice &&
                choiceHasStringFields(mty->declaration))
                return true;
            // fix #2326 / §9.8 v0.228: a non-nullable INLINE-ARRAY field whose
            // element type (recursively, through nested record/choice/inline-
            // array) carries a string needs the release walker to recurse into
            // it. Without this arm a `[N]string` (or `[N]RecWithAStringField`)
            // component is invisible to recordHasStringFields, and the call-
            // result / deserialize-disposal-scope registrations BOTH key off
            // this predicate to decide whether to recurse — so the +1
            // allocated by the deserializer's `bz_binder_read_string` per
            // element would never be released (verified: 2 allocations / 2
            // leaks per round-trip under ASan/LSan). The element-type
            // recursion is finite for the same reason as the named record /
            // choice arms above: sema rejects a self-containing value graph,
            // so the reachable type graph is a finite DAG.
            if (mty && !mty->nullable &&
                mty->kind == sema::TypeRef::Kind::InlineArray &&
                mty->arrayElement &&
                inlineArrayElementCarriesString(mty->arrayElement))
                return true;
        }
        return false;
    }

    // fix #2326 / §9.8 v0.228: does the element type of a non-nullable inline
    // array carry a string — directly (a `string` element), or transitively
    // through a non-nullable nested record / choice / inline array?
    // Declared here so recordHasStringFields (above) and the deserialize-
    // disposal-scope registration (LlvmBackend_Serialization.cpp) can share
    // the same predicate, and so emitInlineArrayStringReleases_impl (below)
    // can call it to recurse into a nested array element. Mirrors
    // recordHasStringFields / choiceHasStringFields in shape; the termination
    // argument is the same finite-DAG walk over the value-type graph.
    bool inlineArrayElementCarriesString(const sema::TypeRef* tr);

    // fix #2425 / §9.8 / §28.9 / §35.4 v0.235: does value-like choice `ts`
    // own, directly (a `string` payload field in some declared case) or
    // transitively (through a non-nullable nested-record or nested-choice
    // payload field), at least one `string`? Mirrors recordHasStringFields's
    // mutual recursion just above. Termination: §35.1's choice
    // self-containment check (TypeResolution_Checks.cpp's
    // checkChoiceSelfContainment, which reuses recordValueContainsSelf —
    // "a choice cannot recursively contain itself by value through its case
    // payloads") rejects a choice/record graph cycle at DECLARATION time
    // exactly as §9 rejects a self-containing record, so the combined
    // record+choice value-type graph reachable from any record or choice
    // declaration is a finite DAG regardless of which kind starts the walk
    // — the same basis emitStructEquality and the #2405 record recursion
    // rely on. Defined in LlvmBackend_Disposal.cpp, next to the release
    // walker it gates. Declared here so recordHasStringFields (above) and
    // emitRecordStringFieldReleases_impl (above) can call it.
    bool choiceHasStringFields(const sema::TypeSymbol* ts);

    // fix #2425 / §9.8 / §28.3 / §28.9 / §35.4 v0.235: release the ACTIVE
    // case's `string`-owning payload(s) of value-like choice `choiceTy` at
    // struct ADDRESS `choiceAddr`, recursing into a non-nullable nested
    // record/choice payload field to ARBITRARY depth — the choice-side
    // counterpart to emitRecordStringFieldReleases/_impl. Defined in
    // LlvmBackend_Disposal.cpp. Used by the #2398/#2425 deserialize
    // disposal-scope registration (LlvmBackend_Serialization.cpp); NOT the
    // same mechanism as #2115's emitValueLikeChoiceActiveCaseStrRelease
    // (a separate, VALUE-based, one-level walker for the scope-end LOCAL
    // release, LlvmBackend_Setup.cpp/_Disposal.cpp) — see the #2425/#2428
    // lane report for which mechanism covers which shape.
    void emitChoiceStringFieldReleases_impl(llvm::Value* choiceAddr,
                                            const sema::TypeSymbol* choiceTy);
    void emitChoiceStringFieldReleases(llvm::Value* choiceAddr,
                                       const sema::TypeSymbol* choiceTy) {
        emitChoiceStringFieldReleases_impl(choiceAddr, choiceTy);
    }

    // fix #2326 / §9.8 v0.228: release every `string` storage reachable from
    // inline-array element type `elemTy` at array ADDRESS `arrAddr` —
    // mirrors emitInlineArrayEquality (the auto-equality walker, §8.3 /
    // §15.1.2) in shape: walk N unrolled slots, dispatch on each slot's
    // static type. `string` → bz_string_release; non-nullable record →
    // recurse via emitRecordStringFieldReleases_impl; non-nullable choice
    // → emitChoiceStringFieldReleases_impl; non-nullable nested inline
    // array → recurse. Takes the OUTER array TypeRef (not just the
    // element type) so the per-element loop can derive N from
    // `arraySizeValue` (§15.1.2) without inspecting the LLVM type
    // (modern LLVM opaque pointers would otherwise require a
    // `getElementType` on a pointee that may be a primitive ptr — not a
    // PointerType). Used by emitRecordStringFieldReleases_impl when
    // a record's storage member is an inline array whose element type
    // carries a string (#2326), and by the #2326 deserialize-disposal-
    // scope registration (LlvmBackend_Serialization.cpp) for the same
    // shape. Defined in LlvmBackend_Disposal.cpp next to its sibling
    // choice walker.
    void emitInlineArrayStringReleases_impl(llvm::Value* arrAddr,
                                            const sema::TypeRef* arrayTy);
    void emitInlineArrayStringReleases(llvm::Value* arrAddr,
                                       const sema::TypeRef* arrayTy) {
        emitInlineArrayStringReleases_impl(arrAddr, arrayTy);
    }

    // fix #1747 / §28.9 / §9.7 / §28.3: is `tr` a (non-nullable) record
    // TypeRef whose record owns at least one `string` field? Convenience
    // classifier used at return sites (the callee retain) and the caller's
    // call-result registration. A nullable record (`P?`) is excluded — it
    // lowers to {i8, P} and a nullable-result release path is deferred.
    bool isRecordWithStringFieldsTypeRef(const sema::TypeRef* tr) {
        if (!tr || tr->nullable ||
            tr->kind != sema::TypeRef::Kind::Named || !tr->declaration)
            return false;
        return recordHasStringFields(tr->declaration);
    }

    // fix #1747 / §28.9 / §9.7 / §28.3: release each string storage member
    // of record `ts` at the struct ADDRESS `recAddr`. For each string
    // field: GEP the slot, load the string pointer, bz_string_release (null-
    // safe / sentinel-no-op), null the slot. Used by the recordStringFields
    // scope-end Disposal.
    //
    // fix #2448 / §9.7 / §28.9 (v0.235): this release is balanced NOT by a
    // callee-side retain (no such retain runs — see #2453, which removed
    // the never-wired `emitRecordStringFieldRetains`/`_impl` and the
    // never-set `currentReturnIsRecord_` flag that #1747-era comments here
    // used to credit with minting that +1) but by
    // `recordCallResultReleaseEligible_` only admitting a local whose
    // initializer is PROVABLY a fresh, alias-free construction
    // (`isFreshRecordConstructionOfExactType` / `isProvablyFreshRecordFactoryMethod`,
    // LlvmBackend_Setup.cpp): each string field's value already carries
    // whatever +1/borrow state it had AT CONSTRUCTION (a by-value return is
    // a bit-copy that touches no refcount), and the eligibility walk proves
    // that state is exclusively this local's to release — no separate mint
    // step is needed or exists.
    void emitRecordStringFieldReleases(llvm::Value* recAddr,
                                       const sema::TypeSymbol* ts) {
        emitRecordStringFieldReleases_impl(recAddr, ts);
    }

    // fix #1747 / §28.9 / §9.7 / §28.3: the set of plain `var` RECORD
    // LocalDecl nodes whose declared type owns at least one `string` field,
    // whose initializer is a CALL returning that record (`var p := Mk()`),
    // and which the conservative escape walk proved never to escape.
    //
    // fix #2448 / §9.7 / §28.9 (v0.235): ALSO now requires the CALLEE be
    // isProvablyFreshRecordFactoryMethod-eligible (see that function). The
    // ORIGINAL #1747 gate admitted ANY CallExpr returning a record-with-
    // string-fields type, reasoning (per its own comment, since corrected)
    // that the callee always "mints the +1" on return — but no such mint
    // ever existed (see the #2448 discovery note above): a record's string
    // field carries whatever +1/borrow state it had AT CONSTRUCTION,
    // unchanged by a by-value return (a bit-copy touches no refcount). That
    // is sound ONLY when the callee's return value is a fresh, alias-free
    // construction (isFreshRecordConstructionOfExactType) — NOT when it is
    // a BORROWED value already reachable from a longer-lived binding (a
    // plain field read, `return this.f;`, empirically confirmed as a
    // heap-use-after-free: the caller's scope-end release frees a string
    // the field's owner still references). A NewExpr-init local, a copy
    // (`var q := p`), a with-copy, a by-value-argument, a collection/field
    // store, or a nullable result keeps the prior safe-leak behaviour — the
    // walker over-approximates and the eligibility is intentionally narrow.
    std::unordered_set<const void*> recordCallResultReleaseEligible_;
    llvm::Function* recordCallResultReleaseEligibleFn_ = nullptr;

    // fix #1747: walk the function body, classifying each plain-`var`
    // record-call-result LocalDecl by escape verdict.
    void collectRecordCallResultReleaseEligible(
        const std::vector<std::unique_ptr<ast::Stmt>>& body);

    // fix #2448 / §9.7 / §28.9 (v0.235): is `expr` a provably fresh, alias-
    // free construction of EXACTLY record type `recTs`? See the definition
    // (LlvmBackend_Setup.cpp) for the full rationale.
    bool isFreshRecordConstructionOfExactType(
        const ast::Expr* expr, const sema::TypeSymbol* recTs) const;

    // fix #2448 / §9.7 / §28.9 (v0.235): is MethodSymbol `ms` provably a
    // "fresh record factory" for record `recTs`? Consulted by
    // collectRecordCallResultReleaseEligible to close the #2448 use-after-
    // free (a caller-side gate blind to the callee's own body). See the
    // definition (LlvmBackend_Setup.cpp) for the full rationale and its
    // deliberately narrow scope.
    bool isProvablyFreshRecordFactoryMethod(
        const sema::MethodSymbol* ms, const sema::TypeSymbol* recTs) const;

    // fix #2428 / §9.8 / §28.9 / §35.4, generalized by fix #2448 / §9.7: is
    // `call` a call to SOME type's own SYNTHESIZED `Deserialize` factory?
    // Returns the resolved TypeSymbol (Record or Choice) when recognized,
    // null otherwise. Shared recognition logic behind both
    // isSynthesizedValueLikeChoiceDeserializeCall (choice-kind check) and
    // isSynthesizedRecordDeserializeCall (record-kind check) below. See the
    // definition (LlvmBackend_Setup.cpp) for the full rationale.
    const sema::TypeSymbol* resolveSynthesizedDeserializeCallTarget(
        const ast::CallExpr* call) const;

    // fix #2448 / §9.7 / §28.9 (v0.235): is `call` a call to record
    // `recTs`'s own synthesized `Deserialize` factory? An UNCONDITIONALLY
    // sound alternative to isProvablyFreshRecordFactoryMethod — a
    // synthesized Deserialize's body is codegen-emitted directly
    // (emitRecordDeserialize), never a user ast::MethodDecl, so
    // isProvablyFreshRecordFactoryMethod's AST-body walk can never admit
    // it; without this alternative, `var back := Root.Deserialize(d);`
    // would silently lose the release coverage the PRIOR (unsound) #1747
    // gate happened to also provide for this exact shape. See the
    // definition (LlvmBackend_Setup.cpp) for the full rationale.
    bool isSynthesizedRecordDeserializeCall(
        const ast::CallExpr* call, const sema::TypeSymbol* recTs) const;

    // fix #2428 / §9.8 / §28.9 / §35.4: is `call` a call to a value-like
    // choice type's own SYNTHESIZED `Deserialize(d: IDeserializer)` factory
    // — the ONE call shape admitted for the #2428 call-result release below
    // (deliberately narrower than #1747's own "any CallExpr returning the
    // type" gate — mirroring that general gate was found, during this
    // issue's own scoping, to be UNSOUND even for records: a callee that
    // returns a BORROWED value rather than a fresh construction has no
    // genuine +1 for the caller to release; a heap-use-after-free,
    // empirically confirmed and filed as #2448, not fixed here). A
    // SYNTHESIZED Deserialize is provably always a fresh construction — it
    // is compiler-generated, so there is no user body that could instead
    // return a borrowed field/param — so admitting ONLY this shape keeps
    // the release sound without a whole-callee-body analysis. Structurally
    // mirrors the EXACT recognition LlvmBackend_Serialization.cpp's own
    // call-site dispatch uses to route `Type.Deserialize(d)` to
    // emitChoiceDeserialize (methodName=="Deserialize", one arg, callee
    // target is an IdentifierExpr naming a TypeSymbol, the type declares
    // IDeserializable, and has no hand-written "Deserialize" method — a
    // hand-written Deserialize suppresses synthesis, §9.8, and need not be
    // fresh-construction-shaped). Defined in LlvmBackend_Setup.cpp, next to
    // its sole caller (collectValueLikeChoiceCallResultReleaseEligible).
    // GENERIC choices are excluded (ts->genericParams must be empty),
    // matching #2425's own documented generic decline.
    bool isSynthesizedValueLikeChoiceDeserializeCall(
        const ast::CallExpr* call) const;

    // fix #2428 / §9.8 / §28.9 / §35.4: the set of plain `var` VALUE-LIKE
    // CHOICE LocalDecl nodes whose initializer is a call recognized by
    // isSynthesizedValueLikeChoiceDeserializeCall, whose resolved type
    // (declared or inferred) is that SAME non-nullable, non-generic choice
    // with at least one string reachable from its active case
    // (choiceHasStringFields, #2425), and which the conservative escape
    // walk proved never to escape. Consulted in lowerLocalDecl
    // (LlvmBackend_Setup.cpp), mirroring recordCallResultReleaseEligible_
    // exactly. Released via the SAME choiceStringFields Disposal / arbitrary-
    // depth emitChoiceStringFieldReleases walker #2425 built for the
    // deserialize disposal scope — reused here rather than a third ad hoc
    // mechanism, per #2425's own filed recommendation ("one shared...
    // walker... rather than two parallel ad hoc mechanisms").
    std::unordered_set<const void*> valueLikeChoiceCallResultReleaseEligible_;
    llvm::Function* valueLikeChoiceCallResultReleaseEligibleFn_ = nullptr;

    // fix #2428: walk the function body, classifying each plain-`var`
    // value-like-choice call-result LocalDecl by escape verdict.
    void collectValueLikeChoiceCallResultReleaseEligible(
        const std::vector<std::unique_ptr<ast::Stmt>>& body);

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
        // §29.7 (#944): for each for-in whose lowering creates an iterator-format
        // enumerator over a LOOP-OWNED source temporary (a CallExpr/NewExpr
        // iterable, e.g. `P.Inner()`), the frame field holding that source's
        // IEnumERABLE, keyed on the iterable expression node. The ENUMERATOR (the
        // GetEnumerator clone) round-trips via localFields[iterable]; this extra
        // field round-trips the SOURCE so the enclosing iterator's Dispose can
        // dispose it too when abandoned mid-loop (else the source frame leaks).
        // Absent when the source is a named local/param (disposed at its own
        // scope end via the using/local machinery, not loop-owned).
        std::unordered_map<const ast::Node*, unsigned> forInSrcFields;
    };

    // Pre-scan a method body to collect all LocalDecl/ForStmt nodes and their
    // types — the locals that must survive across a suspend point (a `yield`
    // or, for async, an `await`). Returns {node, llvmType} pairs. Kind-
    // agnostic: the conservative spill set is "every local in the body."
    void collectSuspendLocals(
        const std::vector<std::unique_ptr<ast::Stmt>>& body,
        std::vector<std::pair<const ast::Node*, llvm::Type*>>& out);

    // §29.3 / §30.2 (#2132): inside a state-machine body (iterator or async), a
    // suspend-live local's alloca must be mapped to its reserved frame field via
    // smAllocaToField_, or the per-suspend spill/restore (emitAwaitSuspend / the
    // yield handler, both iterate smAllocaToField_) never saves it — so on resume
    // the fresh-stack alloca holds garbage. The general lowerLocalDecl slot path
    // does this inline; the early-return initializer paths (heap-collection
    // literal, dict literal, inline-array literal) must call this so a collection
    // built from a `[...]` LITERAL and held across an await round-trips too.
    void mapSuspendLocalSlot(const ast::Node* declNode, llvm::AllocaInst* slot);

    // §29.7 (#944): true when `fs` is a for-in whose lowering creates an
    // implicit iterator-format enumerator (Dispose at vtable slot 1) — i.e. the
    // builtin-enumerable vtable path of lowerIEnumerableBuiltinFor (a CallExpr /
    // NewExpr iterable, or a pure-`IEnumerable<T>` param/local). That enumerator
    // is an owned resource scoped to the loop body, just like a `using var`. When
    // the for-in is inside a yielding iterator AND its body itself suspends, the
    // enumerator must round-trip through a DEDICATED frame field (so the enclosing
    // iterator's Dispose can restore and dispose it if it is abandoned while
    // suspended inside the loop, §29.7) — keyed on `fs.iterable.get()`. This
    // predicate is the single source of truth shared by collectSuspendLocals
    // (which reserves the field) and lowerIEnumerableBuiltinFor (which wires the
    // enumerator slot to it and registers the suspension disposal). It mirrors the
    // `isArrayBacked == false` decision in lowerIEnumerableBuiltinFor exactly.
    bool forInBuiltinEnumeratorNeedsFrameSlot(const ast::ForStmt& fs);

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

    // §29.8 / §30.1 (#973a): per state-machine FUNCTION, the frame pointer its
    // captures were restored from and the capture-field index of each `owned`
    // PARAMETER in it. Together they let a discharge lowered inside a
    // state-machine body write the runtime moved-from flag —
    // nullStateMachineOwnedParamCapture stores null into the FRAME slot, not
    // just the restored alloca — so the frame's null-guarded terminal-state
    // disposal skips a parameter the body moved out on this path.
    //
    // Keyed by `llvm::Function*` rather than held as three scalars ON PURPOSE:
    // emitting a body can recursively emit ANOTHER function (a lazily
    // monomorphized generic callee, a nested async lambda), and that inner
    // emission calls emitFrameCaptureRestores too. Scalars would be clobbered
    // and the OUTER body's later adoption would silently skip its frame null —
    // a missed moved-from flag is a DOUBLE FREE, not a leak. A per-function map
    // survives any nesting depth, and the lookup keys on the function being
    // emitted right now, so a write can only ever land in its own frame. The
    // Emitter is constructed per module, so the map never outlives the
    // `llvm::Function*`s it keys on.
    struct SmFrameCaptures {
        llvm::Value* self = nullptr;
        llvm::StructType* structTy = nullptr;
        std::unordered_map<const sema::Symbol*, unsigned> ownedParamField;
    };
    std::unordered_map<const llvm::Function*, SmFrameCaptures> smFrameCaptures_;

    // §29.8 / §30.1 (#973a): null the FRAME capture slot of the `owned`
    // state-machine parameter named by the bare identifier `src`, if any. A
    // no-op outside a state-machine body, for a non-parameter/borrow source,
    // and for a parameter that is not a frame capture. Called beside the #1978
    // `nullTransferredOwnedLocalSlot` at the marked-adoption lowering — the one
    // discharge form common::stateMachineOwnedParamIsFrameNullable admits — so
    // `admitted ⟹ frame slot nulled` is exact.
    void nullStateMachineOwnedParamCapture(const ast::Expr* src);

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

    // §29.3 / §30.7 (#747 R42): a for-loop inside a SUSPENDING async iterator /
    // async body creates implicit locals (index counter, loop variable,
    // enumerator) as plain stack allocas whose live range crosses the
    // yield/await suspend. fixupSuspendDominance only demotes REGISTERS (an
    // alloca's pointer trivially dominates, so it is skipped), so on a
    // cross-thread Resume these allocas read fresh-stack garbage → wrong value
    // (index) / segfault (enumerator). Register them into asyncHandlerSpillSlots_
    // so promoteDemotedSlotsToFrame heap-spills them into the persistent
    // state-machine frame. Gated on the loop body actually suspending (else the
    // cheap stack allocas are correct and promotion would only waste a field).
    void registerForLoopSpillSlots(
        const ast::ForStmt& fs,
        std::initializer_list<llvm::AllocaInst*> slots);

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

    // §30.7 v0.137 (#655): does the body contain an `await` inside ANY `finally`
    // block?  Forbidden in async iterators because the Dispose path runs
    // synchronously — there is no async driver when a consumer abandons iteration
    // early (§29.7), so a suspension there would have nowhere to resume.
    // Only called from emitAsyncIteratorMethod; emitAsyncMethod must NOT call
    // this (an ordinary async method may await in its finally, §30.9).
    static bool awaitInFinallyPosition(
        const std::vector<std::unique_ptr<ast::Stmt>>& stmts);

    // §30.1 / §30.2: emit an `async` method (one returning Task / Task<T> /
    // OwningTask<T>; the async-iterator forms — async + yield — are deferred).
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

    // §30.5 / #1503: classify a declared/annotated async return type (the
    // FULL `Task<T>`/`OwningTask<T>` type, not just T) and return the per-T
    // disposer thunk iff it is Pattern A `Task<T:class>` — the shape whose
    // task always owns its produced result directly and so must dispose it
    // at task-free time regardless of whether any await ever borrowed it
    // (§30.5: "the task owns the result and its destructor disposes it
    // directly"). Returns null for `Task` (no generic arg), `Task<T>` where T
    // is not class/interface (no ownership obligation crosses the await
    // boundary), and `OwningTask<T>` (Pattern B — its unconsumed-result
    // disposal is the CONSUMER's using-var scope-end responsibility, wired
    // separately in LlvmBackend_Disposal.cpp). Shared by emitAsyncMethod and
    // emitAsyncLambda.
    llvm::Function* patternAResultDisposerFor(const sema::TypeRef* taskTypeRef);

    // #2279 (§30.5 Pattern A, v0.236): when the result type of a Pattern A
    // `Task<T:class>` is itself an INTRINSIC COLLECTION (Array<T> / Set<T> /
    // Dictionary<K,V> / LinkedList<T> / Stack<T> / Queue<T> / OwningBuffer<T> /
    // Owning* collections), the task owns the result directly, and disposing
    // the task must free the collection's backing storage (a bz_array_t*,
    // bz_dict_t*, bz_llist_t*, or bz_buffer_t*). Returns the per-result-
    // collection-type `void(void*)` thunk that frees the collection via the
    // appropriate runtime call with the right per-element disposer, mirroring
    // the `using var` collection disposal path. Cached. Returns null when
    // `resultCollTy` is not an intrinsic collection (the existing
    // class-typed-result path is then correct).
    llvm::Function* patternAResultCollectionDisposer(
        const sema::TypeRef* resultCollTy);

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
    // does. `resultElemSemaTy` is T in the lambda's `Task<T>`/`OwningTask<T>`
    // return type (null for a no-value `Task`), taken from the call-site
    // expected function type. `declaredTaskTy` is the FULL `Task<T>`/
    // `OwningTask<T>` type carrying that same annotation (null when no
    // call-site annotation was visible, e.g. an IIFE-invoked async lambda) —
    // used only to compute the Pattern A result disposer (#1503); passing
    // null there simply means no disposer is attached, matching the
    // pre-existing conservative behavior for that unannotated shape.
    // `paramSyms` are the lambda's user parameter symbols (already bound from
    // the entry fn's args 1..N). On return `fn` is fully terminated; the
    // builder is left at the resume function's end and lowerLambda restores
    // its saved insert point. Leaves the sm*_ async state cleared.
    void emitAsyncLambda(const ast::LambdaExpr& lam, llvm::Function* fn,
                         const std::vector<sema::Symbol*>* paramSyms,
                         const sema::TypeRef* resultElemSemaTy,
                         const sema::TypeRef* declaredTaskTy = nullptr);

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
    // result value (an int/value/string for Task<T>/OwningTask<T>; a null
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
    // takeResultAfter=true: after loading the result, call bz_task_take_result to
    // null the task's result buffer (§28.3/#1250: consumed-tracking for owned results).
    llvm::Value* emitAwaitSuspend(llvm::Value* task, llvm::Type* resultTy,
                                  bool freeTaskAfter,
                                  bool takeResultAfter = false,
                                  bool markSpentAfter = false);

    // §28.12 / §30 (`await using` normal-exit path): call DisposeAsync() on `obj`
    // stdlib §6.10 (v0.175, #808): emit the TaskScope disposal for one scope
    // object `sc` (non-null; the caller null-guards). On the normal exit of an
    // active async state machine: bz_task_scope_dispose_async(sc, 1, ioeVt) +
    // emitAwaitSuspend on the returned task (drain awaited; missing-Join IOE
    // re-raises in the resume block). Otherwise (unwind edge, or no async SM):
    // bz_task_scope_dispose_unwind(sc) — non-blocking zombie drain.
    void emitTaskScopeDispose(llvm::Value* sc, bool isUnwind);

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
    // §29.7 (#944): maps a for-in iterable node → the frame field holding its
    // loop-owned SOURCE IEnumERABLE (the enumerator clone is in smLocalFields_).
    // Published alongside smLocalFields_; consumed by lowerIEnumerableBuiltinFor
    // (wires the source slot to the field) and the iterator Dispose body.
    std::unordered_map<const ast::Node*, unsigned> smForInSrcFields_;
    // Maps alloca slot → frame field index (spill at suspend, restore at state entry).
    std::unordered_map<llvm::AllocaInst*, unsigned> smAllocaToField_;

    // fix #2702 (§23.3/§23.4/§30.5/§28.4, v0.240): ONE place that saves,
    // neutralises and restores the emitter state a NESTED body emission must
    // not inherit. `ensurePropertyBlockGetter`, `ensureIndexerBlockGetter` and
    // `ensurePropertyAccessorFn` each lower a user statement body into a
    // SEPARATE, MEMOIZED function from the middle of another function's
    // emission; any per-body field they forget leaks the caller's context into
    // the callee and is then frozen module-wide by the memoisation. #2170
    // (smKind_/smStateStruct_), #2274/#2281 (finallyStack_/finallyBodyStack_)
    // and #2694 (currentReturnTypeNode_/currentLambdaReturnSemaTy_) each had to
    // patch this by hand, field by field, at two or three sites; #2701/#2702
    // are the next three omissions of the same shape. This guard exists so the
    // NEXT per-body field is added in one place instead of three.
    //
    // The exact reset list, and the members deliberately INHERITED (with the
    // reason each is correct to inherit), are documented on the constructor's
    // definition in LlvmBackend_Assign.cpp. Read that before adding a field.
    //
    // It does NOT touch the emitter-specific entry state — `currentFn_`,
    // `currentThis_`, the parameter slots and the builder insert point — which
    // each emitter must still set for itself because each sets them
    // differently (an indexer getter spills index parameters, a property
    // accessor binds an implicit `value`).
    struct NestedBodyScope {
        explicit NestedBodyScope(Emitter& e);
        ~NestedBodyScope();
        NestedBodyScope(const NestedBodyScope&) = delete;
        NestedBodyScope& operator=(const NestedBodyScope&) = delete;

        Emitter& e_;
        // --- return contract (#2694) ---
        const ast::Type*     retNode_;
        const sema::TypeRef* lamRet_;
        // --- suspend/resume frame (#2170 + #2701 + #2702) ---
        SuspendKind          smKind_;
        llvm::StructType*    smStateStruct_;
        llvm::Value*         smSelf_;
        llvm::Value*         smStateGEP_;
        std::vector<llvm::BasicBlock*> smStateBBs_;
        llvm::BasicBlock*    smDoneBB_;
        unsigned             smNextSuspendState_;
        std::unordered_map<const ast::Node*, unsigned>     smLocalFields_;
        std::unordered_map<const ast::Node*, unsigned>     smForInSrcFields_;
        std::unordered_map<llvm::AllocaInst*, unsigned>    smAllocaToField_;
        // --- iterator-specific frame state ---
        unsigned             iterCurrentValField_;
        llvm::Type*          iterElemTy_;
        const sema::TypeRef* iterElemSemaTy_;
        bool                 iterElemIsOwnedClass_;
        unsigned             iterCurrentValOwnedField_;
        // --- async-specific frame state ---
        unsigned             asyncOuterTaskField_;
        unsigned             asyncPendingTaskField_;
        unsigned             asyncSpillPtrField_;
        // --- destructor-body context (#2702 item 2) ---
        bool                 emittingDisposerBody_;
        llvm::Value*         disposerEntryExc_;
        // --- catch/guard-arm context ---
        std::string_view     currentCaughtName_;
        llvm::BasicBlock*    caughtExcArmTarget_;
        // --- narrow per-call channels ---
        const sema::TypeRef* expectedFunctionType_;
        std::unordered_map<std::string_view, llvm::Type*> lastCallGenericSubst_;
        llvm::BasicBlock*    inlinedAccessorEnd_;
        bool                 asyncChainedReturn_;   // #2731 item 1
        // --- root-scope registration channels (#2731 item 3) ---
        std::vector<std::pair<sema::Symbol*, llvm::AllocaInst*>>
                             pendingOwnedParams_;
        std::optional<Disposal> pendingArgsDisposal_;
        // --- SAVED AND RESTORED BUT DELIBERATELY NOT CLEARED (see the .cpp) ---
        std::unordered_map<std::string_view, const sema::TypeRef*>
                             genericSubstSemaTy_;
    };
    // ---- Yield-specific live state (used only when smKind_ == Yield) ----
    unsigned iterCurrentValField_ = 0;
    llvm::Type* iterElemTy_ = nullptr;
    // #1102B: the element's SEMA type (the iterator return's generic arg), so the
    // yield-return store can coerce a bare value / `null` into a nullable-value
    // element struct `{i8,T}` (an `IEnumerable<T?>` iterator). Without it the
    // bare payload's LLVM type != iterElemTy_ and the store was silently dropped.
    const sema::TypeRef* iterElemSemaTy_ = nullptr;
    // §29 (#1120): true when the element type is an owned class or interface (its
    // TypeSymbol is a Class or Interface TypeDecl). The enumerator then owns the
    // most-recently-yielded value and must dispose it on next MoveNext + on Dispose.
    bool iterElemIsOwnedClass_ = false;
    // §29 (#1120): frame field index for the `current_val_owned` i8 flag (1 = the
    // enumerator owns the value in current_val, 0 = it is a borrowed reference to a
    // using-var local or an owned param — another mechanism disposes it).  Only
    // meaningful when iterElemIsOwnedClass_ is true.  Set alongside
    // iterCurrentValField_ in emitIteratorMethod; cleared to 0 after MoveNext.
    unsigned iterCurrentValOwnedField_ = 0;

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
    // The async method's result element type T (Task<T>/OwningTask<T>), or null
    // for a `Task` (no value). Used to size bz_task_complete's result buffer and
    // to type the value read back at the await site.
    llvm::Type* asyncResultTy_ = nullptr;
    // The sema TypeRef for T in Task<T>, mirroring asyncResultTy_. Kept
    // alongside it so emitAsyncComplete can detect a nullable value type (int?,
    // double?, enum?) and wrap a plain returned value into the {hasValue=1,
    // value} struct before storing — per §30.1 / §7.2 (#493).
    const sema::TypeRef* asyncResultSemaType_ = nullptr;
    // §30.5 / #1503: the per-T disposer thunk for the CURRENT async body's
    // declared return type, iff that type is Pattern A `Task<T:class>` (the
    // one shape whose task always owns its produced result directly — every
    // await/`.Result` on a Task<T> only ever borrows it, §30.5). Computed
    // once (patternAResultDisposerFor) by emitAsyncMethod / emitAsyncLambda
    // from the declared/annotated return type; lowerStmt(ReturnStmt) attaches
    // it to the outer task (bz_task_set_result_disposer) at each NON-chained
    // `return <owned>` site, right before bz_task_complete. Null for `Task`,
    // `Task<non-class T>`, and `OwningTask<T>` (Pattern B has its own
    // consumer-side disposal path in LlvmBackend_Disposal.cpp). Reset to null
    // when the owning async body's emission finishes.
    llvm::Function* asyncPatternAResultDisposer_ = nullptr;
    // §30.5 / #1332 (v0.162): set to true in lowerStmt(ReturnStmt) when the
    // pattern `return await <call>` with a pointer-typed (Pattern A class)
    // result is detected inside an async method — signals lowerAwait to NOT
    // free the source task immediately (it will be chained to the outer task).
    // Reset to false after lowerExpr returns in lowerStmt.
    bool asyncChainedReturn_ = false;
    // §30.1 (#1521): the current plain (non-iterator) async method's frame,
    // published by emitAsyncMethod right after buildStateMachineFrame so
    // emitAsyncOwnedParamAutoDispose can walk frame.captures from EVERY
    // terminal-completion site — emitAsyncComplete (every `return`/fall-off,
    // §30.1 success path) and the async.fault boundary (§30.9, covering both
    // an ordinary uncaught throw and an uncaught OperationCanceledException,
    // i.e. the §30.1 "fault, and cancellation" edges — the runtime models
    // cancellation as an OperationCanceledException fault, so no separate
    // edge is needed). Not the same mechanism as the async-ITERATOR path
    // (emitAsyncIteratorMethod has its own captures-driven dispose loop
    // inline in its Dispose body, gated on frame.captures directly since that
    // function does not share emitAsyncComplete). Null outside emitAsyncMethod
    // (emitAsyncComplete/emitAsyncOwnedParamAutoDispose no-op when null) —
    // an async lambda does not publish this (out of #1521's scope; a lambda's
    // own `owned` params are a separate, not-yet-flagged case).
    const StateMachineFrame* asyncOwnedParamFrame_ = nullptr;

    // §30.1 (#1521): dispose every `owned`/move-only-value capture of
    // *asyncOwnedParamFrame_ that sema flagged iteratorAutoDispose (never
    // discharged on any path — the same flag §29.8's iterator machinery
    // uses), in REVERSE declaration order (§28.4), reading/nulling each
    // through frame pointer `self`. Null-guarded per capture so a parameter
    // already moved out (chained via `return await <param>`, or explicitly
    // disposed) — whose field was nulled at the move/dispose site — is safely
    // skipped; a param flagged here is, by construction, never discharged on
    // any path, so this runs at most once per parameter per task lifetime
    // regardless of which terminal edge calls it (success xor fault, never
    // both, since bz_task_complete / the fault boundary are mutually
    // exclusive terminal states). No-op when asyncOwnedParamFrame_ is null.
    void emitAsyncOwnedParamAutoDispose(llvm::Value* self);

    // Pass 2: emit a function's body.
    void emitFunctionBody(sema::MethodSymbol* ms, llvm::Function* fn);

    // §22.7 / §22.13 / §24.1 (#1502): the entry-point (Main) static-init
    // prologue — SerializationException vtable, non-lazy static ctors, and
    // static-event backing-source allocation. Shared by the synchronous-Main
    // body path (emitFunctionBody) and the async-Main entry (emitAsyncMethod).
    void emitMainStaticInitPrologue();

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

    // #720 / §22.13 / §22.7: build the per-(non-generic-)type `bz$<T>$ensureinit()`
    // functions (one per type that has an sfinit and/or cctor), with their state +
    // saved-exception globals. Called from declareStaticInitializers after
    // staticCtors_ (and sfinitFnOf_/cctorFnOf_) are populated.
    void emitEnsureInitFns();

    // #720: emit a call to `owner`'s ensureinit (if it has one — non-generic with
    // static state) at a static-access site, then route a possible
    // TypeInitializationException through the current unwind target. No-op for a
    // type without lazy static state (generic types, types with no static init).
    void emitEnsureStaticInit(const sema::TypeSymbol* owner);

    // #1513 / §2.8.3 rule 4 / §22.12 rule 1: get-or-create the shared,
    // LinkOnceODR, per-instantiation static-field global for `fs` under the
    // substitution described by (substTypes, substIds, substSemaTys,
    // typeArgMangle) — the SAME redirect condition/key staticFieldGlobal's
    // generic branch already computes. Factored out so genericInstSfinitFn can
    // materialize a SIBLING field's per-instantiation global (one instantiation
    // has ONE ensureinit covering every static field of that instantiation,
    // mirroring the per-type design where one ensureinit covers a whole type).
    llvm::GlobalVariable* getOrCreateGenericInstStaticGlobal(
        const sema::FieldSymbol* fs,
        const std::vector<llvm::Type*>& substTypes,
        const std::vector<const void*>& substIds,
        const std::vector<const sema::TypeRef*>& substSemaTys,
        const std::string& typeArgMangle);

    // #1513: lazily build (once per instantiation, LinkOnceODR) the "sfinit"
    // function that stores every one of `ts`'s non-constant static field
    // initializers into instantiation `typeArgMangle`'s per-instantiation
    // globals. T-independent initializers only (mirrors the #709 loop's
    // limitation in emitStaticConstructorBody — an initializer whose VALUE
    // depends on the instantiation's type argument needs per-instantiation
    // lowering that is not yet supported, tracked as a follow-up exactly like
    // the existing local-generic limitation).
    llvm::Function* genericInstSfinitFn(const sema::TypeSymbol* ts,
                                        const std::string& typeArgMangle);

    // #1513 / #1391: lazily build (once per instantiation, LinkOnceODR) the
    // ensureinit function for generic type `ts`'s instantiation
    // `typeArgMangle` — the generic-instantiation counterpart of
    // emitEnsureInitFns's per-type functions, at FULL parity with that
    // function's #1391 hardening: an atomic cmpxchg for the uninit->
    // in-progress claim, same-thread owner-tracking for the re-entrancy
    // backstop, and genuine cross-domain blocking (bz_static_init_wait/
    // notify) — required because this guard's state is LinkOnceODR (shared
    // across domains for real), so a racing double-init is reachable here
    // exactly as it is for the per-type case.
    llvm::Function* genericInstEnsureInitFn(const sema::TypeSymbol* ts,
                                            const std::string& typeArgMangle);

    // #1513: emit a call to the per-instantiation ensureinit for `key` (looked
    // up via genericInstGlobalKeyOf_ from a global staticFieldGlobal returned),
    // then route a possible TypeInitializationException through the current
    // unwind target — the generic-instantiation counterpart of
    // emitEnsureStaticInit(TypeSymbol*).
    void emitEnsureGenericInstStaticInit(const GenericInstKey& key);

    // #1553 / §22.13 (v0.181): the ordinal of `fs` among its type's non-constant
    // static-field initializers in textual declaration order (the sequence sfinit
    // stores, and initProgress counts). Returns -1 for a field sfinit does not
    // store at runtime — a compile-time-constant initializer (already in the
    // global) or one without a backing global — i.e. always-assigned, no guard.
    int staticFieldInitIndex(const sema::FieldSymbol* fs);

    // #1553 / §22.13 (v0.181): the static-initialization-cycle runtime backstop.
    // Emitted after emitEnsureStaticInit at a static-FIELD READ site: if `owner`'s
    // static init is in progress on THIS thread and `fs` has not been assigned yet
    // (its declaration index >= owner's init progress), the read would observe a
    // partially-initialized static — synthesize a TypeInitializationException
    // (naming `owner`, InnerException null) into bz_exc and route it through the
    // current unwind target instead. A no-op when `owner` has no static state, or
    // `fs` is always-assigned (staticFieldInitIndex < 0), or the state is not
    // in-progress (the normal post-init read).
    void emitStaticPartialReadGuard(const sema::TypeSymbol* owner,
                                    const sema::FieldSymbol* fs);

    // #1553 / §22.13 (v0.181): store a freshly-synthesized cycle-backstop
    // TypeInitializationException — vtable + a message naming `typeName`,
    // InnerException null — into bz_exc (does not itself route/unwind).
    void emitSynthesizeCycleTIE(const std::string& typeName);

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

    // §22.8 step 2 (#929/#930): run class `t`'s OWN (non-inherited) field
    // initializers + own EventSource allocations. Called from a constructor
    // body AFTER the base chain (emitConstructorBody / inlineCtorBody) and from
    // the no-explicit-ctor construction sequence after the base sequence, so
    // §22.8's base-ctor → field-init → body order holds. emitInstanceInit only
    // installs the vtable; this performs step 2.
    void emitOwnFieldInit(const sema::TypeSymbol* t, llvm::Value* obj,
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

    // fix #2452 (§22.8 step 1-2 / §2.5 / §2.8.2-§2.8.3, v0.235): run an
    // IMPORTED, no-explicit-ctor base's own construction sequence (its own
    // base chain + its own field initializers) via the already-exported
    // `bz$<Base>$init`, instead of `emitOwnFieldInit(base, …)` — which walks
    // `ast::FieldDecl`/`PropertyDecl` initializers off each member's `decl`,
    // always null for an imported symbol (the #2374-class gap), so it
    // silently no-ops for every one of `base`'s fields. Undoes `bz$<Base>
    // $init`'s own vtable-install side effect (save/restore around the call)
    // so it does not clobber the most-derived vtable the `new` site already
    // installed. Returns true if it handled construction (caller should NOT
    // also call emitOwnFieldInit); false if `base` is not an eligible
    // imported no-ctor class (caller falls back to the local path).
    bool emitImportedBaseFieldInit(const sema::TypeSymbol* base,
                                   llvm::Value* obj);

    // CG16: if `s` is a leading constructor chain call (`base(...)` or
    // `this(...)` as an ExpressionStmt-of-CallExpr), return the call and
    // set `isThis`; else null.
    const ast::CallExpr* chainCallOf(const ast::Stmt& s, bool& isThis);

    // CG16: the base class (baseTypes[0] when it is a class), else null.
    // #2442 (v0.235): marked `const` (pure read of `t->baseTypes`, no
    // Emitter state touched) so the const `findIndexer` below can call it to
    // walk a class's base chain.
    const sema::TypeSymbol* baseClassOf(const sema::TypeSymbol* t) const;

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

    // #2182 / §28.3: consume lastLambdaCapturedCells_ — for each freshly-boxed
    // capture cell of a cellFreeEligible_ local (populated by lowerLambda),
    // register a boxedCellFree (or boxedCellFreeDeferred / inline-array-cell
    // disposal) in the appropriate scope's disposeStack_. Mirrors the per-cell
    // soundness checks documented at LlvmBackend_impl.hpp's Disposal flags:
    //   - skip + defer when a reads-through-cell disposal exists (owner scope);
    //   - skip when the captured local's decl scope is shallower than the
    //     current scope (#969 conservative leak);
    //   - else register boxedCellFree in the current scope (fires at scope end,
    //     AFTER any closureRelease in the same scope, so the envdtor can read
    //     through the cell first).
    // Called from lowerLocalDecl for `var f := lambda` (the existing path) and
    // from lowerCall's IIFE arm for `(lambda() => …)()` — an IIFE has no LocalDecl
    // to drive the consumption, but its lowered lambda populates the vector the
    // same way. Clears lastLambdaCapturedCells_ (owned by the most recent
    // lowerLambda call).
    void consumeLastLambdaCapturedCells();

    // CG13: register a `using var`'s scope-based disposal (§28.3). The
    // local is disposed at the end of its enclosing block in reverse
    // declaration order. Recorded against the innermost open scope.
    void registerUsingDisposal(const ast::LocalDecl* ld);

    // §30.5 / #1258 / #1868: attach the per-T owned-result disposer to a task
    // Disposal when `taskTy` is an `OwningTask<T:class>` — the produced owned
    // class result would otherwise leak when the task is dropped without an
    // intervening await taking it. Shared by BOTH the `using var` task
    // registration (registerUsingDisposal) and the plain-`var` auto-drop task
    // registration (lowerStmt LocalDecl) so the two paths cannot drift. No-op for
    // a channel-receive task, a non-OwningTask, or a value-typed result. // per §30.5
    void attachOwningTaskResultDisposer(Disposal& d, const sema::TypeRef* taskTy);

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

    // §4.5/§4.7/§4.9/§4.11/§2.14/§29.1: is `call` a BUILTIN method that
    // TRANSFERS an owned class element to the caller (Pop/Dequeue on owning
    // stacks/queues, PopLast/PopFirst/PopAt/PopValue, T.Deserialize,
    // TakeCurrent on owning enumerators)? Builtins have no MethodSymbol, so
    // owned-return recognition keys on the callee name plus an OWNING receiver
    // type. Shared by usingVarOwnsClassObjectDynamically (using-var scope-end
    // disposal) and unwindOwnedLocalClass (explicit-dispose reclaim, §28.3
    // unwind disposal, §28.7 drop-on-overwrite).
    bool isBuiltinOwnedTransferCall(const ast::CallExpr& call);

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

    // §15.3 (#747): the set of move-only owning inline-array ([N owned]T?/
    // [N owned]T) LocalDecl nodes whose ownership is TRANSFERRED out of the local
    // somewhere in the body (init-RHS, assignment-RHS, owned-arg, `return arr`,
    // field-store, throw, `move arr`, or any other non-borrow use). Computed once
    // per body by collectMoveOnlyArrayTransferred via a DEFAULT-TRANSFER walk:
    // only borrow positions (index/member receiver, for-in iterable) keep a local
    // OUT of the set. A local NOT in the set gets scope-end slot disposal
    // registered; a local IN the set is skipped (the receiving local / callee
    // owns the slots — so the array's slots are disposed exactly once, never
    // double-freed). A conservatively-misjudged transfer at worst LEAKS.
    std::unordered_set<const void*> moveOnlyArrayTransferred_;
    // The function for which moveOnlyArrayTransferred_ was computed (mirrors
    // unwindIneligibleFn_): registration only fires when currentFn_ matches, so a
    // lambda/thunk body lowered mid-function does not consult a stale set.
    llvm::Function* moveOnlyArrayTransferredFn_ = nullptr;
    void collectMoveOnlyArrayTransferred(
        const std::vector<std::unique_ptr<ast::Stmt>>& body);
    // §15.3 (#747): if `ld` is a move-only owning inline-array local, register
    // (or, when transferred-out / outside the analyzed body, deliberately skip)
    // its scope-end slot disposal and return true (so the caller bypasses the
    // using/task/unwind registration chain, which does not apply to a move-only
    // value-typed local). Returns false for any non-(inline-owned-array) local.
    bool registerInlineOwnedArrayDisposal(const ast::LocalDecl* ld);

    // §35.4 (v0.200, #2045 stage 3): if `ld` is an OWNING-CLASSIFIED choice
    // local (a sum whose classification is Owning — any case has an `owned`
    // payload field, or a move-only value-typed payload field), register its
    // scope-end active-case disposal and return true (the using/task/unwind
    // chain does not apply — the disposal shape is per-tag switch). Returns
    // false for any non-(owning-choice) local; for value-like and borrow-
    // bearing choices, the choice struct is a plain value with no slot-
    // resident owned payloads to dispose, so the ordinary local path runs.
    // The register-using-task-unwind chain (`using var`, etc.) still applies
    // for an owning choice: a `using var o: Outcome := Outcome.Fetched(...);`
    // is the normal spelling, and the active-case disposal runs on top of
    // the using-var's normal-exit-and-unwind path.
    bool registerOwningChoiceDisposal(const ast::LocalDecl* ld);

    // §29.11 (#943): if `ld` is a plain (non-`using`) `var` whose value is a
    // vtable-backed re-iterable IEnumerable<T>/IOwningEnumerable<T> (the result of
    // a yield iterator method, or an array/user-class wrapped into one), register
    // a scope-end disposal of that source via the iterator-format slot-1 Dispose
    // (iterEnumDispose) and return true. The for-in loop over such a source clones
    // a fresh enumerator per iteration and leaves the source alive (so a second
    // loop replays), so the SOURCE itself must be disposed once at scope end here.
    // Returns false for any other local (the caller falls through to the normal
    // using/task/unwind registration chain).
    bool registerIterableSourceDisposal(const ast::LocalDecl* ld);

    // §28.11 / #889: the set of LocalDecl nodes that RECEIVED ownership of a
    // moved-out owned element via an `out owned` argument (e.g.
    // `s.TryPop(out owned item)`). The element is written into the local's slot
    // by the Try* writeback; the local becomes the SOLE owner (the ownership
    // pass marks it Owned per §28.11, and there is no scope-end auto-dispose for
    // an out-owned recipient — discharge is the programmer's explicit
    // `item?.Dispose()` / `move item`). Recorded in ensureOutVarSlots when the
    // `out owned` arg targets an identifier local. Consulted by
    // explicitDisposeOwnedReceiverSlot so an explicit `.Dispose()` / `?.Dispose()`
    // on such a local RECLAIMS the heap block (and nulls the slot) exactly once,
    // rather than only dispatching the destructor (which would leak the block).
    std::unordered_set<const void*> outOwnedRecipients_;

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

    // fix #2696 / §28.1 / §28.9 / §4.6: give every non-nullable `string`
    // PARAMETER this body ASSIGNS TO an independent +1 at bind, so the
    // drop-on-overwrite release the assignment emits consumes THIS frame's
    // reference instead of the caller's borrow. Call right after
    // bindParameters, with the body statements and the member's parameter
    // symbols (see the .cpp for the full argument).
    //
    // fix #2712 / §23.3: `setterValueAnchor` extends the same treatment to an
    // INLINED accessor's implicit `value` parameter, which sema anchors on the
    // PropertyDecl and codegen binds into locals_ (not paramSlots_) — pass the
    // PropertyDecl after `locals_[pd]` has been bound; null for every non-
    // accessor caller.
    void retainAssignedStringParams(
        const std::vector<std::unique_ptr<ast::Stmt>>& body,
        const std::vector<sema::Symbol*>& params,
        const ast::PropertyDecl* setterValueAnchor = nullptr);

    // fix #2115 / §28.9 / §35.4: the set of `var` value-like choice LocalDecl
    // nodes whose initializer is a choice construction every string payload arg
    // of which is a +1 producer or a literal, AND a conservative whole-body walk
    // (the shared StringEscapeWalker) proved the value never escapes its scope.
    // For such a local the slot is the SOLE owner of the active case's string
    // payload(s); a scope-end tag-switched bz_string_release walk (the value-
    // like analog of emitOwningChoiceActiveCaseDisposal) reclaims them exactly
    // once. Computed by collectStringReleaseEligible (it reuses that scan's
    // StringEscapeWalker invocations); consulted in lowerLocalDecl. Membership is
    // the ONLY thing that arms the release — absence ⇒ the payload leaks (the
    // pre-existing, safe behavior). Gated on stringReleaseEligibleFn_ (the same
    // analyzed-function contract as the string set).
    std::unordered_set<const void*> valueLikeChoiceStrReleaseEligible_;

    // #1003 / stdlib §5 / §28 (trivial-dispose borrowing collections): the set
    // of plain `var` LocalDecl nodes that bind a FRESHLY-MATERIALIZED borrowing
    // array-backed collection (`Array<T>`/`Stack<T>`/`Queue<T>`) produced by a
    // LINQ/copy materializer call (`ToArray`/`Sorted`/`Reversed`) and proven by
    // the conservative escape walk never to escape their scope. Per §28, a
    // borrowing `Array<T>` is trivial-dispose: its backing must be reclaimed at
    // scope end (running NO user code — elements are borrows), but the sema/
    // ownership pass classifies the local as carrying no disposal obligation, so
    // codegen is solely responsible for the implicit reclaim. Membership arms a
    // scope-end bz_array_free of the single producer reference (the only one — the
    // local is non-escaping). Absence ⇒ the array leaks (the prior, safe
    // behavior). Reuses the StringEscapeWalker (type-agnostic — it tracks only
    // the target LocalDecl identifier), so a returned/moved/aliased/captured
    // local is excluded → never a UAF, never a double-free.
    std::unordered_set<const void*> borrowingArrayFreeEligible_;
    // The function for which borrowingArrayFreeEligible_ was computed (mirrors
    // stringReleaseEligibleFn_): a lambda/thunk body lowered mid-function
    // inherits no eligibility, so its array locals just leak (never double-freed).
    llvm::Function* borrowingArrayFreeEligibleFn_ = nullptr;
    // #1028: the dict analog of borrowingArrayFreeEligible_ — plain `var` locals
    // bound to a fresh `owned Dictionary<K,V>` materializer (`var d :=
    // src.ToDictionary(...)`) that the type-agnostic escape walk proves never
    // escape, so a scope-end bz_dict_free of the single producer reference is
    // sound (no UAF, no double-free). Populated alongside borrowingArrayFreeEligible_
    // (same escape walk, separate set because dicts free via bz_dict_free, not
    // bz_array_free).
    std::unordered_set<const void*> dictMaterializerFreeEligible_;
    llvm::Function* dictMaterializerFreeEligibleFn_ = nullptr;
    // #1930 / stdlib §5 / §28.9: the grouping analog of borrowingArrayFreeEligible_
    // — plain `var g := <src>.GroupBy(sel).First()` (or .Single()/.Last()/
    // .ElementAt(i)/.*OrNull()/no-seed .Aggregate(...)) locals bound to a bare-
    // GroupBy element-RETURNING terminal, whose returned IGrouping the type-agnostic
    // escape walk (StringEscapeWalker) proves never escapes. Membership arms a
    // scope-end bz_grouping_free of that single returned grouping (the terminal
    // already reclaimed its sibling groups + the outer array). An escaping local
    // (returned — statically rejected as a borrow —, captured by a closure, added
    // to an Array<IGrouping>, or aliased) is excluded → the returned grouping just
    // leaks (the safe direction: never a UAF, never a double-free with an owner
    // that took it). Populated alongside borrowingArrayFreeEligible_ (same escape
    // walk, separate set because groupings free via bz_grouping_free).
    std::unordered_set<const void*> groupingFreeEligible_;
    llvm::Function* groupingFreeEligibleFn_ = nullptr;
    // #2142 / §28.9 / §28.11: the Set<T> analog of borrowingArrayFreeEligible_ —
    // plain `var s: Set<T> := [ ... ]` collection-literal locals (dict-backed,
    // bz_dict_t*) that the type-agnostic escape walk proves never escape. Set<T>
    // is a borrowing-family collection (spec table, §28.9/§7052) exactly like
    // Array<T>: trivial-dispose, backing "reclaimed with the instance". Populated
    // alongside borrowingArrayFreeEligible_ (same escape walk, separate set
    // because a Set frees via bz_dict_free, not bz_array_free).
    std::unordered_set<const void*> setLiteralFreeEligible_;
    llvm::Function* setLiteralFreeEligibleFn_ = nullptr;
    // #2143 / §28.9 / §28.11: the Dictionary<K,V> LITERAL analog of
    // borrowingArrayFreeEligible_ — plain `var d: Dictionary<K,V> := [k:v, …]`
    // (or the empty `[]` literal targeting a Dictionary) locals that the
    // type-agnostic escape walk proves never escape. Distinct from
    // dictMaterializerFreeEligible_ (ToDictionary()-call results): this set is
    // for the LITERAL initializer form, which has no reclaim path at all today
    // (#2143) — the materializer path already reclaims via the sibling set.
    std::unordered_set<const void*> dictLiteralFreeEligible_;
    llvm::Function* dictLiteralFreeEligibleFn_ = nullptr;
    // fix #2660 / §15.4 / §28.9 / §28.11: the NESTED-literal analog of the three
    // flat sets above. A `var`/`using var` local whose initializer is a
    // collection literal each of whose element/value NODES is itself a fresh
    // nested collection literal — §15.4's own `groups`/`config` examples — owns
    // a TREE of heap collections, not one. The flat reclaim frees only the root;
    // the interior nodes have no releaser at all, so they leak (measured: 224 B
    // for the two-`Array<int>` `groups` example, 837 B for `config`). This set
    // holds the roots for which freeing the whole tree with the root is PROVEN
    // exactly-once. Membership requires all three of:
    //   (a) the root never escapes — the same StringEscapeWalker the flat
    //       #1871/#2142/#2143 family uses;
    //   (b) EVERY node of the tree is a fresh nested literal, recursively (a
    //       BORROWED element — `["a": [1], "b": cached]` — is never an aggregate
    //       member per proposal 0077 §3, and freeing it would double-free the
    //       binding that owns it);
    //   (c) no INTERIOR-node alias can outlive the root's scope
    //       (literalTreeInteriorAliasSafe — a default-deny walk, because the
    //       StringEscapeWalker tracks only the root identifier and classifies
    //       `var inner := groups["primes"];` as a safe read).
    // A root failing any of the three keeps today's behaviour exactly: the
    // interior nodes leak. That is the deliberate safe direction — a bounded
    // leak, never a use-after-free.
    std::unordered_set<const void*> literalTreeAggregateEligible_;
    // fix #2660: true if every node of `lit` (recursively, through the
    // element/value type `collTy`) is a fresh nested collection literal —
    // condition (b) above. False for a flat literal (nothing to free) and for
    // any literal with a borrowed / call-produced node member.
    bool isLiteralTreeAggregate(const ast::CollectionLiteralExpr* lit,
                                const sema::TypeRef* collTy);
    // fix #2660: condition (c) — true if NO value read out of `ld` can outlive
    // `ld`'s scope. DEFAULT-DENY: every mention of `ld` must sit in one of two
    // whitelisted shapes (`ld[i]` used only as the target of a further index or
    // member access, or a member access on `ld` naming a non-node-producing
    // member), and any statement/expression kind the walk does not enumerate
    // returns false. Unrecognised shapes therefore LEAK, never dangle.
    bool literalTreeInteriorAliasSafe(
        const std::vector<std::unique_ptr<ast::Stmt>>& body,
        const ast::LocalDecl* ld) const;
    // fix #2660: the `void(void*)` thunk that reclaims ONE interior node of a
    // literal-tree aggregate whose node type is `nodeTy` (bz_array_free /
    // bz_dict_free / bz_llist_free, with the node's OWN element disposer derived
    // recursively). Null when `nodeTy` is not a node shape this can express as a
    // single thunk — the caller then registers no interior free (a leak, the
    // safe direction). Cached per node type.
    llvm::Function* collectionNodeFreeThunk(const sema::TypeRef* nodeTy);
    std::unordered_map<const sema::TypeRef*, llvm::Function*>
        collectionNodeFreeThunks_;
    // fix #2660: if `declNode` is a proven literal-tree aggregate root of type
    // `collTy`, return the interior-node disposer to install on the root's
    // Disposal; null otherwise. Single entry point for the three registration
    // sites (plain-`var` sequence literal, plain-`var` dict literal, `using var`).
    llvm::Function* literalTreeNodeDisposerFor(const ast::Node* declNode,
                                               const sema::TypeRef* collTy);
    // Compute borrowingArrayFreeEligible_ (and #1930 groupingFreeEligible_, #2142
    // setLiteralFreeEligible_, #2143 dictLiteralFreeEligible_, #2660
    // literalTreeAggregateEligible_) for a block body (see the .cpp).
    void collectBorrowingArrayFreeEligible(
        const std::vector<std::unique_ptr<ast::Stmt>>& body);
    // #1930 / stdlib §5 / §28.9: true if `ld` is a plain `var g := <expr>` whose
    // initializer is a bare-GroupBy element-RETURNING terminal (result type
    // IGrouping<…>): `<src>.GroupBy(sel).First()/.Single()/.Last()/.ElementAt(i)/
    // .FirstOrNull()/.LastOrNull()/.SingleOrNull()/no-seed .Aggregate(...)`. Such a
    // local binds the returned grouping (freed at scope end when non-escaping).
    bool isFreshGroupingTerminalLocal(const ast::LocalDecl* ld) const;
    // #1871 / §28.9 / §28.11: true if `ld` is a plain `var xs: Array<T> := [ ... ]`
    // (a borrowing `Array<T>` local initialised by a collection literal) whose
    // element policy is backing-only (null per-element disposer; value or
    // class/interface borrows). Such a literal's backing is reclaimed at scope end
    // via a single bz_array_free(backing, null) — the §28.9 "reclaimed with the
    // instance" mechanism. Non-const: calls elementPolicyFor.
    bool isBorrowingArrayLiteralLocal(const ast::LocalDecl* ld);
    // #2142 / §28.9 / §28.11: true if `ld` is a plain `var s: Set<T> := [ ... ]`
    // — a borrowing `Set<T>` local (name-head exactly "Set", not OwningSet/
    // SortedSet — kept minimal per #2142) initialised by a LIST-form collection
    // literal (Set<T> never uses the ":" dict-literal syntax). Such a literal's
    // backing (a bz_dict_t*) is reclaimed at scope end via a single
    // bz_dict_free — the §28.9 "reclaimed with the instance" mechanism, mirrored
    // from isBorrowingArrayLiteralLocal.
    bool isBorrowingSetLiteralLocal(const ast::LocalDecl* ld) const;
    // #2143 / §28.9 / §28.11: true if `ld` is a plain `var d: Dictionary<K,V> :=
    // [k:v, …]` (or the empty `[]` literal targeting a Dictionary) — a borrowing
    // `Dictionary<K,V>` local (name-head exactly "Dictionary", not
    // "OwningDictionary" — that family owns its values and requires `using`,
    // already handled by registerUsingDisposal) initialised by a collection
    // literal. Such a literal's backing (a bz_dict_t*) is reclaimed at scope end
    // via a single bz_dict_free, mirroring isBorrowingArrayLiteralLocal.
    bool isBorrowingDictLiteralLocal(const ast::LocalDecl* ld) const;
    // #1871 / §28.9 / §28.11: collect the `var v: IEnumerable<T> := <arrLd>` VIEW
    // locals in `body` that are proven never to escape — a binding of the array
    // into such a borrowing view does not extend the backing's lifetime, so it
    // does not disqualify the array from scope-end reclaim.
    void collectSafeViewAliases(
        const std::vector<std::unique_ptr<ast::Stmt>>& body,
        const ast::LocalDecl* arrLd,
        std::unordered_set<const void*>& out);
    // #1003: true if `ld` binds a plain `var` whose resolved type is a borrowing
    // bz_array_t-backed collection (`Array`/`Stack`/`Queue`, NOT an `Owned*`
    // family, NOT dict-backed Dictionary/Set/LinkedList) AND whose initializer is
    // a fresh-array materializer CALL (`ToArray`/`Sorted`/`Reversed`). These are
    // the exact shapes codegen lowers to a fresh `bz_array_t` the local solely
    // owns. A `new Array<T>(...)` initializer is excluded (a NewExpr carries a
    // sema disposal obligation → rejected as a plain undisposed `var`, never
    // reaching this discardable path). The per-element disposer for the scope-end
    // free is derived via elementPolicyFor (null for value/class borrows; the
    // string-release thunk for `Array<string>` whose ToArray retained a +1/elem).
    bool isFreshBorrowingArrayMaterializerLocal(const ast::LocalDecl* ld) const;
    // #1004: true if `e` is an EXPRESSION codegen lowers to a FRESH, OWNERLESS
    // borrowing `Array<T>` temporary — a fresh-array materializer CALL
    // (`recv.ToArray()`/`recv.Sorted(...)`/`recv.Reversed()`) whose result no
    // named local/field/param owns. Used at the copy-form `.Sorted()`/
    // `.Reversed()` lowering to decide whether the RECEIVER (input) array is a
    // disposable temporary that must be `bz_array_free`d after the copy (else it
    // leaks — #1004). Intentionally narrow: only a whitelisted materializer
    // CallExpr matches, so an identifier/field/param/`move`/`new` receiver (a
    // borrowed or otherwise-owned array) is never matched and never freed → no
    // UAF, no double-free. The `Array<string>` +1/element co-ownership is
    // released by passing the string-release thunk to the free.
    bool isFreshOwnerlessArrayMaterializerExpr(const ast::Expr* e) const;
    // #1013: true if `e` is a fresh-Dictionary materializer call
    // (`recv.ToDictionary(...)`) returning a builtin borrowing `Dictionary<K,V>`
    // temporary — reclaimed at end-of-statement via bz_dict_free.
    bool isFreshOwnerlessDictMaterializerExpr(const ast::Expr* e) const;
    // #1006(b): true if `e` is a CALL to a USER method declared `owned`-return
    // (`method M(): owned Array<T>`) whose result is a builtin borrowing
    // `Array<T>` temporary — i.e. an unbound, must-consume owned array. Used at
    // the copy-form `.Sorted()`/`.Reversed()` lowering (alongside the #1004
    // materializer check) so that an UNBOUND owned-returning call used as the
    // chained RECEIVER is `bz_array_free`d after the copy (else it leaks —
    // #1006b). Generalizes #1004 along the `owned` axis: the producing method's
    // `owned` return marker — not a builtin name whitelist — is the authority that
    // the call owns its result. A bound (`using var`/`var`), `move`d, returned, or
    // owned-arg receiver is never a chained-receiver CallExpr here, so it is never
    // matched → no UAF, no double-free. Borrowing-array element semantics (stdlib
    // §3/§5) mean the per-element disposer is null (string-release for
    // `Array<string>`), identical to #1004.
    bool isUnboundOwnedBorrowingArrayCallExpr(const ast::Expr* e) const;
    // #1006(b) / #1004: emit the end-of-full-expression bz_array_free of an
    // UNBOUND fresh borrowing-`Array<T>` temporary `recvVal` (a #1004 materializer
    // call OR a #1006b `owned`-returning user-method call given by `recvExpr`),
    // with the borrowing-snapshot element disposer (null / string-release). A
    // no-op for any other receiver shape. The caller must have fully read its
    // result from `recvVal` first (the backing is released here). Shared by the
    // chained-method-receiver sites and the `.Length`/`.Count` borrow-position
    // property read.
    void freeUnboundOwnedBorrowingArrayTemp(const ast::Expr* recvExpr,
                                            llvm::Value* recvVal,
                                            const sema::TypeRef* recvTy);
    // fix #1909 / §17.13 / §28.3 / §28.11: dispose an UNBOUND owned temporary
    // whose type is an OWNING collection (OwningArray/OwningDictionary/
    // OwningSet/OwningStack/OwningQueue/OwningLinkedList) — the discard
    // `_ := <owned-producer-of-owning-collection>` shape (the
    // DiscardStmt::disposeOwnedCollectionValue gate, set by the ownership
    // pass). Frees the container AND cascades disposal into its owned
    // elements (the owning family's contract, §28.11 "Owning collections") —
    // unlike freeUnboundOwnedBorrowingArrayTemp, which frees only the
    // backing of a BORROWING collection (its elements are never disposed).
    // Dispatches on `rt`'s name-head to the matching runtime free (array-
    // backed / bz_dict_t*-backed / bz_llist_t*-backed), mirroring
    // registerUsingDisposal's collection dispatch for a NAMED owning-
    // collection local — the same classification, applied to a bare value
    // instead of a LocalDecl. `v` must already be the fully-lowered
    // collection pointer (never re-lowered). Every one of the three runtime
    // frees is null-safe, so a nullable owning-collection temp needs no
    // separate null guard here.
    void freeUnboundOwnedOwningCollectionTemp(llvm::Value* v,
                                              const sema::TypeRef* rt);
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
    // #2321 / §22.3 / §28.9: REASSIGNMENT through a string-typed `ref`/`out`
    // PARAMETER (the assignment is an ordinary store through the alias, not a
    // local-slot reassignment). The caller's slot owns an INDEPENDENT +1 that
    // the local-slot escape walk (stringReleaseEligible_) does not cover, so
    // a plain `CreateStore` leaves the heap string leaked — the new value is
    // an independent producer the caller has no other release for, and the
    // displaced previous value had its +1 in this slot. Emit the same retain-
    // new (transfer a producer) / store / release-old pair lowerStringLocalReassign
    // does, against the `refParamAddr` pointer the caller already resolved.
    // Mirrors lowerStringLocalReassign; returns true when handled.
    bool lowerStringRefParamStore(const ast::Expr& target, llvm::Value* slotAddr,
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

    // fix #2115 / §28.9 / §35.4: classify a `var` choice local for scope-end
    // managed-payload release. Returns the INSTANTIATED choice TypeRef iff `ld`
    // is a non-`using` choice local whose resolved type classifies ValueLike AND
    // whose initializer is a choice-case construction (`Msg.Note(...)`) every
    // string-typed payload arg of which is a +1 producer (concat / interp /
    // range-slice / ToString-mint) or a string literal (sentinel rc — release is
    // a no-op) — so the slot provably owns +1 on the active case's strings and
    // releasing them at scope end is balanced. Returns null otherwise (a borrow
    // payload arg, a non-construction initializer, an owning/borrow-bearing
    // choice, or a non-choice type → no release; the conservative, safe default).
    const sema::TypeRef* valueLikeChoiceLocalMintsStringPayload(
        const ast::LocalDecl* ld) const;
    // fix #2115: register (if eligible) the scope-end managed-payload release of
    // a value-like choice local. Called from lowerLocalDecl after the slot store;
    // gated on valueLikeChoiceStrReleaseEligible_ (the escape walk's verdict) so
    // it never fires on a local whose value is copied/returned/captured/case-
    // matched (a second release through an alias would double-free). Mirrors
    // armStringLocalRefcount's contract for the string-local release.
    void armValueLikeChoiceStrRelease(const void* declNode,
                                      llvm::AllocaInst* slot,
                                      const ast::LocalDecl* ld);
    // fix #2115: emit a tag-switched bz_string_release walk over the ACTIVE
    // case's string-typed payload fields of a value-like choice struct. Mirrors
    // emitOwningChoiceActiveCaseDisposal's switch/join shape but calls
    // emitStringRelease (null- and sentinel-safe) on each string payload slot.
    void emitValueLikeChoiceActiveCaseStrRelease(
        llvm::AllocaInst* slot, const sema::TypeSymbol* choiceTy,
        const sema::TypeRef* choiceRef);

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

    // §27.5 / §28.9 / #672 / #802: the set of +1-box-producer LOCAL DECL NODES
    // (ast::Node*) that are captured by AT LEAST ONE lambda env in the function
    // body. A captured boxed-value local has its scope-end bz_box_release
    // SUPPRESSED by BoxEscapeWalker (the capture marks it escaped); each capturing
    // lambda's per-lambda envdtor performs a release instead (mirroring the
    // string/closure capture handling). rc-retain model (#802): the FIRST capture
    // transfers the producer's single suppressed +1; every ADDITIONAL capture
    // bz_box_retains an independent +1 (in lowerLambda) so its envdtor's release
    // is balanced. N envs ⇒ rc = 1 + (N-1) = N, and N envdtor releases reach 0
    // after the last env dies — no leak, no double-free, for any capture count.
    // Computed once per body by collectBoxCaptureLocals; consulted at the lambda
    // capture site in lowerLambda to arm the envdtor box release + per-env retain.
    std::unordered_set<const void*> boxCaptureLocals_;
    // §27.5 / §28.9 / #694: LOCAL DECL NODES captured by EXACTLY ONE lambda env
    // (computed alongside boxCaptureLocals_, guarded by boxCaptureLocalsFn_).
    // Combined with a DEFINITELY-escaping sole capturer (definitelyEscapingLambdas_)
    // at the capture site, the escaped closure's envdtor frees the boxed capture
    // cell at last-drop — closing the 8B/cell escaping-capture leak. Single holder
    // ⇒ no double-free; definite escape ⇒ the outer scope is gone before the
    // closure dies ⇒ no UAF.
    std::unordered_set<const void*> singleCaptureLocals_;
    // fix #2706 / §27.5 / §28.9 (v0.240): LOCAL DECL NODES and PARAMETER SYMBOLS
    // that this function body ASSIGNS TO (`x := e` / `x += e`), including from
    // inside a lambda body. Computed alongside boxCaptureLocals_/
    // singleCaptureLocals_ and guarded by the same boxCaptureLocalsFn_ token.
    //
    // A capture cell is SHARED and MUTABLE (§27.5). The per-capture retain is
    // taken on the value in the cell AT CAPTURE, while every env's envdtor
    // releases the value in the cell AT DESTRUCTION. A write between those two
    // points makes them different strings: the displaced value is over-retained
    // and the new value takes one release per env against its single +1 — a
    // double free as soon as two envs share the cell. Keying the envdtor's
    // release to what was retained is impossible without refcounting the CELL
    // (the closures read the cell, so the FINAL value must stay alive until the
    // LAST env dies, and that count is dynamic). Consulted at the capture site
    // to suppress BOTH the retain and the envdtor release for such a cell —
    // a bounded leak instead of an over-release.
    std::unordered_set<const void*> assignedCaptureVars_;
    // fix #2706 / §27.5 / §28.9 (v0.240): does this capture's cell need BOTH its
    // per-capture retain and its envdtor release suppressed? True iff
    //   (a) the capture is of a runtime-managed type the envdtor releases at all
    //       (non-nullable `string` or a function type — exactly the envdtor's own
    //       two branches, so the predicates agree by construction), AND
    //   (b) the enclosing function ASSIGNS to the variable (assignedCaptureVars_),
    //       so the cell's contents at destruction differ from those at capture,
    //       AND
    //   (c) MORE THAN ONE env can share that one cell: either two or more lambda
    //       NODES capture it (not in singleCaptureLocals_), or a single node sits
    //       inside a loop over a cell that is NOT reallocated per iteration
    //       (guarded boxing) — so one cell backs every iteration's closure.
    // (c) is what keeps the sole-capture, no-loop case — which is BALANCED today
    // (one env, one release, one reference) — byte-identical.
    // Gated on `currentFn_ == boxCaptureLocalsFn_`, the token the two sets it
    // reads are computed under: outside the analyzed function they are stale, and
    // the conservative answer there is FALSE (leave today's behavior alone).
    bool mutatedSharedCaptureCell(const void* key, const sema::TypeRef* semaTy,
                                  bool freshCellPerIteration) {
        if (currentFn_ != boxCaptureLocalsFn_) return false;
        if (!isStringTypeRefNonNull(semaTy) &&
            !(semaTy && semaTy->kind == sema::TypeRef::Kind::Function))
            return false;
        if (!assignedCaptureVars_.count(key)) return false;
        const bool manyEnvsShareOneCell =
            !singleCaptureLocals_.count(key) ||
            (!loopStack_.empty() && !freshCellPerIteration);
        return manyEnvsShareOneCell;
    }
    // §27.5 / #694: LambdaExpr nodes that DEFINITELY escape the function (directly
    // returned). Computed once per body by collectDefinitelyEscapingLambdas;
    // consulted in lowerLambda to arm the envdtor cell-free.
    std::unordered_set<const void*> definitelyEscapingLambdas_;
    void collectDefinitelyEscapingLambdas(
        const std::vector<std::unique_ptr<ast::Stmt>>& body);
    // The function for which boxCaptureLocals_ was computed (mirrors
    // boxReleaseEligibleFn_): a lambda/thunk body lowered mid-function inherits
    // no eligibility, so its captured boxes just leak (never double-freed).
    llvm::Function* boxCaptureLocalsFn_ = nullptr;
    // Compute boxCaptureLocals_ for a block body (see the .cpp).
    void collectBoxCaptureLocals(
        const std::vector<std::unique_ptr<ast::Stmt>>& body);

    // #429 / §27.5: the set of LOCAL DECL NODES (ast::Node*) and PARAMETER
    // SYMBOLS (sema::Symbol*) whose boxed capture cell can be safely freed at
    // outer-scope exit.  A captured local/param qualifies iff EVERY lambda in
    // the function body that captures it is proven non-escaping (in
    // closureReleaseEligible_) AND it is not a for-var capture (fresh-per-
    // iter cells are excluded — the scope-exit free would only reclaim the last
    // iteration's cell; prior iterations' cells would still leak). Computed
    // once per body by collectCellFreeEligible AFTER closureReleaseEligible_.
    // Consulted in lowerLambda when registering boxedCellFree disposals.
    std::unordered_set<const void*> cellFreeEligible_;
    // The function for which cellFreeEligible_ was computed (mirrors
    // closureReleaseEligibleFn_): a lambda/thunk body lowered mid-function
    // is NOT analyzed; its cells continue to leak (never double-freed).
    llvm::Function* cellFreeEligibleFn_ = nullptr;
    // fix #2334 / §28.14 / §28.9: the subset of cellFreeEligible_ whose ONLY
    // eligibility route includes the TaskScope.Spawn special case
    // (collectCellFreeEligible) — see Disposal::skipCellFreeOnUnwind for the
    // full soundness argument for why such a cell's free must be skipped on
    // the unwind edge specifically (TaskScope.DisposeAsync's unwind path is a
    // documented (#1422) non-blocking zombie drain, so a captured local's
    // quiescence guarantee does not hold there). Computed alongside
    // cellFreeEligible_ by the same pre-scan; consulted by lowerLambda when
    // populating lastLambdaCapturedCells_.
    std::unordered_set<const void*> unwindUnsafeCellFree_;
    // Compute cellFreeEligible_ for a block body (see the .cpp).
    void collectCellFreeEligible(
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

    // #824 / §28.5 / §28.9: is `target` an `owned` heap-collection CLASS
    // instance field lvalue (OwningArray<T>/OwningStack<T>/OwningQueue<T>/
    // OwningSet<T>/OwningLinkedList<T>/OwningBuffer<T>/OwningDictionary<T> etc.)?
    // Only such a location is auto-disposed by the enclosing class's destructor
    // (emitOwnedFieldDisposals) and needs dispose-old/store-new on reassignment.
    // Excludes locals, ref params, static fields, record fields, and
    // non-collection-typed fields.
    bool isOwnedCollectionFieldLvalue(const ast::Expr& target);
    // #824 / §28.5 / §28.9: store `newVal` into the owned collection class
    // field at `fieldAddr`, disposing the prior value first. Order: load-old →
    // store-new → dispose-old (bz_array_free / bz_llist_free are null-safe so
    // no null-guard needed; disposal mirrors emitOwnedFieldDisposals). Ownership
    // transfers into the field; the caller must NOT additionally dispose the
    // source local (the ownership checker discharges the local's obligation at
    // the assignment). Returns true when handled.
    bool lowerOwnedCollectionFieldStore(const ast::Expr& target,
                                        llvm::Value* fieldAddr,
                                        llvm::Value* newVal);

    // §28.7 / #1328: is `target` an `owned` CLASS/INTERFACE instance field
    // lvalue? Excludes locals, ref params, static fields, record fields, and any
    // field whose type is already handled by a sibling helper (string, closure,
    // box, collection — those dispatch through isOwnedReferenceField returning
    // false for collection kinds, and through other helpers for the refcounted
    // categories). Only plain class- or interface-typed `owned` fields on CLASS
    // instances remain; these are disposed at scope end by emitOwnedFieldDisposals
    // and need drop-on-overwrite on reassignment.
    bool isOwnedClassFieldLvalue(const ast::Expr& target);
    // §28.7 / #1328: dispose the prior value of an owned class/interface CLASS
    // field before storing the new owner.  Order (per spec §28.7): load-old →
    // null-guard (moved-from / never-assigned slot holds null) → identity-guard
    // (self-assign `this.f := this.f` must NOT dispose the incoming value) →
    // dispose-old → store-new.  Constructor first-time assignments are safe
    // because the object is calloc-zeroed (null → guard skips). Returns true
    // when handled; false if not an owned class/interface field (caller stores).
    bool lowerOwnedClassFieldStore(const ast::Expr& target,
                                   llvm::Value* fieldAddr,
                                   llvm::Value* newVal);

    // §28.7 / §30.5 (#2173): drop-on-overwrite for an owned Task-family FIELD
    // — the field analog of the using-var task drop path. Mirrors
    // lowerOwnedClassFieldStore's shape with two crucial differences (see
    // the Setup.cpp body for the rationale): the store-new happens BEFORE
    // disposal (a task's bz_task_fire_and_forget completion continuation can
    // re-enter the slot on this thread), and the disposal is the FULL
    // using-var task protocol via emitOwnedTaskDisposal (error_obj prelude,
    // per-T result_disposer, fire-and-forget[_checked]) — not a plain
    // emitDisposeAndReclaim (which would free the task synchronously and
    // not attach the per-class result disposer, leaking any unconsumed
    // Owned T result).
    bool lowerOwnedTaskFieldStore(const ast::Expr& target,
                                  llvm::Value* fieldAddr,
                                  llvm::Value* newVal);

    // §28.7 / §15.3 (#1385): drop-on-overwrite for a WHOLE-VALUE reassignment
    // of a move-only OWNING inline-array local or field (`[N owned]T` /
    // `[N owned]T?`). Disposes each non-null slot of the displaced value in
    // REVERSE slot order (the spec wording of #1385: "disposes each non-moved-
    // from slot of the displaced value (reverse slot order), then stores"), then
    // returns so the caller performs the new-value store. A slot that has been
    // `move a[i]`d or that the user restored with `a[i] := null` already holds
    // null in the alloca (codegen nulls on `lowerMoveFromCollectionSlot` per
    // #747), so the null-guard skips it — exactly once across move + reassign
    // + scope-end. Called from the AssignmentStmt lowering path after RHS
    // evaluation and before any field-store handler / the plain CreateStore.
    // Returns true when it ran (slot disposal emitted); the caller continues
    // with the store. Returns false when the target is not a move-only owning
    // inline array (no-op — caller is on the ordinary path).
    bool lowerOwningInlineArrayReassign(const ast::Expr& target,
                                        llvm::Value* arrayAddr,
                                        llvm::Type* elemTy);

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

    // #990 / §4.5 / §14.2: `using var _ := item` where `item` is the loop
    // variable of a `for var item in coll.Drain()` loop over an owning sequence
    // collection (OwningArray/Stack/Queue/LinkedList).  A drained owning-collection
    // element is, by the stdlib type definitions, a class object with a vtable —
    // even when its static type is an unresolved generic class type parameter `T`
    // in the generic TEMPLATE body (classOfLocal cannot recover a concrete class
    // there, so the static-disposal path misses it).  Dispose it dynamically
    // through its own vtable (idempotent + reclaim + null, §28.2/#380) so each
    // drained element disposes exactly once.
    bool usingVarIsDrainedOwningElement(const ast::LocalDecl* ld);

    // #362 / stdlib §4.5: the resolved collection TypeRef of a local (declared
    // type, then `new` initializer type, then inferred initializer type), or
    // null if the local is not a heap collection.
    const sema::TypeRef* collectionTypeOfLocal(const ast::LocalDecl* ld);

    // #465 / §15.4 / stdlib §4.4–§4.5: materialize a sequence collection literal
    // `[e1, e2, …]` into a freshly-allocated heap collection (Array<T> /
    // OwningArray<T> / Stack / Queue and their owning variants), reusing the exact
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

    // #999 / §15.4: materialize a collection literal `[…]` against ANY heap
    // collection target type (Array/OwningArray/Set/OwningSet/Stack/Queue and their
    // owned forms, LinkedList/OwningLinkedList, Dictionary/OwningDictionary). This
    // is the single entry the call-argument path uses: it dispatches the dict
    // literal form to materializeHeapDictLiteral and every sequence form to
    // materializeHeapCollectionLiteral (which itself routes Set/LinkedList).
    // Returns a `ptr` to the freshly-built heap collection, or null when `collTy`
    // is not a recognised heap-collection target (caller then declines).
    llvm::Value* materializeCollectionLiteralForArg(
        const ast::CollectionLiteralExpr* cl, const sema::TypeRef* collTy);

    // #999 / stdlib §10: a synthesized, concrete `Array<byte>` sema::TypeRef for
    // the byte-array FFI factory arguments (RawBytes.From / CString.FromBytes,
    // both taking a BORROW `Array<byte>`). Those intrinsic call sites have no
    // MethodDecl params and sema does not set the literal argument's target type,
    // so the materialization needs this explicit target. Shaped exactly like the
    // Array<byte> TypeRef makeConcreteCollectionResult builds (Named, no
    // declaration, a static "Array" astNode, genericArgs[0] = byte).
    const sema::TypeRef* builtinByteArrayTypeRef();

    // #999 / stdlib §10 / §15.4: lower the single `Array<byte>` BORROW argument
    // of a byte-array FFI factory (RawBytes.From / CString.FromBytes). When the
    // argument is a collection literal `[…]`, materialize it into an owned
    // Array<byte> temporary (the literal is not a general expression value) and
    // register its scope-end disposal (the factory only borrows the bytes — it
    // copies them — so the caller owns and disposes the temp exactly once on the
    // normal AND unwind paths). For any other argument expression (a named local,
    // a method result, …) this is exactly lowerExpr.
    llvm::Value* lowerByteArrayFactoryArg(const ast::Expr& argExpr);

    // fix #2728 / §32.6 + §28.3 *Unbound owned temporaries* (v0.240): true when
    // `e` is a FRESHLY-MINTED FFI buffer — a direct call to one of the four
    // factories `CString.From` / `CString.FromBytes` / `WString.From` /
    // `RawBytes.From` (tryLowerBuiltinClassCall's table, LlvmBackend_Calls.cpp).
    // §32.6: "The `CString` class is for caller-owned strings: those that the
    // Bezel side allocated and is responsible for disposing." Sets `isRawBytes`
    // when the buffer needs the two-level `bz_rawbytes_free` rather than free().
    //
    // ⚠ The predicate is deliberately SYNTACTIC-plus-type, never type alone.
    // `builtinTypeName(exprType)` is true of EVERY CString-typed expression —
    // a `using var` binding's read, an extern `CString` PARAMETER, a field —
    // and freeing one of those is a double free or a use-after-free. §28.3
    // draws exactly this line: "A temporary that is instead bound (to a
    // `var`/`using var`), `move`d, returned, or transferred into an owning
    // location is governed by the corresponding transfer or discharge rule and
    // is **not** disposed under this rule (it would otherwise be disposed
    // twice)." A factory call written directly at the use site is bound to
    // nothing and reachable by nothing else, so it is the unbound case and only
    // the unbound case.
    bool isFfiBufferFactoryCall(const ast::Expr& e, bool& isRawBytes);

    // fix #2728: emit the null-guarded reclaim of one FFI buffer temporary held
    // in `slot` — free() for CString/WString, bz_rawbytes_free (buffer + the
    // {ptr,len} struct) for RawBytes, matching emitDisposals' ffiBuffer arm.
    void emitFfiBufferTempFree(llvm::AllocaInst* slot, bool isRawBytes);

    // #1501 / stdlib §6.7 (line 2884) / §30.10: a synthesized, concrete
    // `Array<Task>` sema::TypeRef for the Task.WhenAny/WhenAll `tasks`
    // argument. Mirrors builtinByteArrayTypeRef's rationale: these intrinsic
    // call sites have no MethodDecl params, so a literal argument needs an
    // explicit materialization target. genericArgs[0] is a bare `Task`
    // (Named, no declaration, no generic args) — an opaque task handle.
    const sema::TypeRef* builtinTaskArrayTypeRef();

    // #1501 / stdlib §6.7 (line 2884) / §30.10: lower the `tasks` argument of
    // Task.WhenAny/WhenAll. When the argument is an inline collection literal
    // `[t1, t2, …]`, materialize it into an owned Array<Task> temporary and
    // report it as such (second element = true) — the caller must free it
    // SYNCHRONOUSLY via freeTaskArrayArgTemp immediately after the runtime
    // call, NOT via scope-end disposal (see the .cpp comment: the temp's
    // alloca is invisible to collectSuspendLocals, so a deferred free across
    // a genuine await-suspend boundary reads back stack garbage). For any
    // other argument expression (a named local, a method result, …) this is
    // exactly lowerExpr, reported as not-a-temp (false) — the caller must NOT
    // free it (its own declaration already owns disposal).
    std::pair<llvm::Value*, bool> lowerTaskArrayArg(const ast::Expr& argExpr);

    // #1501: free a literal temp Array<Task> built by lowerTaskArrayArg,
    // synchronously, right after the runtime call that borrowed it.
    void freeTaskArrayArgTemp(llvm::Value* arr);

    // #999 / §15.4 / §28.3: build the scope-end Disposal entry for an owned
    // temporary heap collection materialized for a BORROW call-argument (the temp
    // is an owned local; the callee borrows it for the call duration; it must be
    // disposed exactly once at scope exit, on the normal AND unwind paths). The
    // entry mirrors the `using var` collection/linked-list/dict registration in
    // lowerLocalDecl, selecting the right runtime free + per-element disposer for
    // `collTy`. `slot` is an alloca holding the collection pointer. Not called for
    // an `owned` parameter (ownership transfers into the callee — no caller
    // disposal).
    Disposal buildCollectionTempDisposal(llvm::Value* slot,
                                         const sema::TypeRef* collTy);

    // #559 / stdlib §4.10: materialize a Set<T>/OwningSet<T> sequence literal
    // `[e1, e2, …]` into a freshly-allocated Set backed by bz_dict_alloc_generic,
    // reusing the SAME allocation and per-element insertion as the
    // `new Set<T>()` + `.Add(e)` paths (bz_dict_set_gen + sentinel value 1).
    // Duplicates are silently collapsed (dict stores the first; second call is
    // a no-op on an already-present key). Returns a `ptr` to the bz_dict_t
    // (or null if `setTy` is not a Set/OwningSet TypeRef with a recognised
    // element type).
    llvm::Value* materializeHeapSetLiteral(
        const ast::CollectionLiteralExpr* cl, const sema::TypeRef* setTy);

    // fix #1070 / #1069 / stdlib §4.10 / §15.4: the CLASS/INTERFACE-element half
    // of materializeHeapSetLiteral — such a set has no generic-dict backing, so
    // it is a bz_array_t built with the same bz_array_alloc `new Set<C>()` emits
    // and the same dedup insertion `.Add` emits (emitArrayBackedSetInsert).
    // Returns a `ptr` to the bz_array_t, or null for a non-class element.
    llvm::Value* materializeArrayBackedSetLiteral(
        const ast::CollectionLiteralExpr* cl, const sema::TypeRef* setTy);

    // per §4.6/§4.7: the resolved LinkedList<T>/OwningLinkedList<T> TypeRef of
    // a local, or null if the local is not a linked list.
    const sema::TypeRef* linkedListTypeOfLocal(const ast::LocalDecl* ld);

    // #394 / stdlib §4.9: the resolved Dictionary/OwningDictionary TypeRef of a
    // local (declared type, then `new` initializer type, then inferred
    // initializer type), or null if the local is not a hash-backed dictionary.
    const sema::TypeRef* dictTypeOfLocal(const ast::LocalDecl* ld);

    // CG13: lower a block as a disposal scope. Pushes a fresh scope; on
    // normal fall-through, disposes its `using var`s in reverse and pops.
    // If the block terminated early (return/break/continue), that
    // terminator already emitted the needed disposals, so we only pop.
    bool lowerScopedBlock(const std::vector<std::unique_ptr<ast::Stmt>>& body,
                          std::size_t startIdx = 0);

    // §27.5 / #754: lower a for-loop body. If the loop variable is captured by a
    // closure anywhere in the body, first pre-box it into a fresh per-iteration
    // cell AT BODY ENTRY (rather than lazily at the capturing lambda site) so the
    // cell is refreshed on EVERY iteration — including iterations where the
    // capturing lambda sits inside a conditionally-executed branch (a nested
    // `if`). Then delegate to lowerScopedBlock. Used by every for-handler in
    // place of a bare `lowerScopedBlock(fs.body)`.
    bool lowerForBody(const ast::ForStmt& fs);

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

    // §21.5 (v0.201, #2051): return the exit-mode value for routing a labeled
    // `break`/`continue` (to loop `targetLoopIdx`, block `targetBlock`, floor
    // `targetScopeDepth`) THROUGH finally frame `fr`, minting a fresh mode the
    // first time and reusing it for identical (isBreak, targetLoopIdx) requests.
    int getOrAddLabeledRoute(FinallyFrame& fr, bool isBreak,
                             std::size_t targetLoopIdx,
                             llvm::BasicBlock* targetBlock,
                             std::size_t targetScopeDepth);

    // §35.4 (v0.200, #2045 stage 3): emit the active-case disposal for an
    // OWNING-classified choice value held in `slot`. Reads the runtime TAG
    // (field 0 of the choice struct, see choiceStruct), switches on it, and
    // for each case with at least one `owned` payload field, GEPs into the
    // case's payload slot (field 1+caseIndex), extracts each owned payload's
    // pointer, null-guards, and dispatches Dispose+reclaim via the runtime
    // vtable (dynamic dispatch — the concrete class is the runtime payload
    // type, statically unknown at the choice site). Payload-free / value-
    // payload cases dispose nothing, so their switch arm is a direct branch
    // to the join. Fires on BOTH the normal and unwind exit — like an
    // `inlineOwnedArray` disposal, the slot value is whatever the active
    // case holds at scope end, and disposing it is the SOLE path for those
    // payloads (no separate `using var` registration fires for choice
    // payloads — the choice value IS the owner).
    void emitOwningChoiceActiveCaseDisposal(llvm::AllocaInst* slot,
                                            const sema::TypeSymbol* choiceTy,
                                            const sema::TypeRef* choiceRef);

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

    // §21.5 (v0.201, #2051): lower a `break`/`continue` to the target loop
    // `offset` levels out (0 = innermost enclosing loop, from
    // BreakStmt/ContinueStmt::targetLoopOffset). Disposes every scope crossed
    // (reverse establishment) and routes through every crossed `finally`. offset
    // 0 reproduces the pre-v0.201 unlabeled path exactly. Precondition:
    // loopStack_ is non-empty and offset < loopStack_.size(). Always terminal.
    bool lowerLoopJump(int offset, bool isBreak);

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
    // #728: `for var x in <LinkedList<T>>` — walk the node chain (§4.13).
    bool lowerLinkedListFor(const ast::ForStmt& fs);

    // §4.6 / §29.2: `for var c in s do … end` over a string — iterates
    // each char (i32 code point) in order. For ASCII/UTF-8 strings this
    // iterates bytes; full Unicode code-point iteration deferred.
    bool lowerStringFor(const ast::ForStmt& fs);
    // §3.3 / §29.2 / #971 item 2: `for var c in sp do … end` over a
    // ReadOnlyStringSpan — iterates code points over the span's [start, start+len)
    // sub-range of the source string. Same lowering shape as lowerStringFor but
    // the source pointer and the length come from the {ptr, i32 start, i32 length}
    // span struct (field 0 is the source, field 1 is the start offset, field 2
    // is the code-point length). The span itself is a stateless view, so no
    // disposal scope is needed for the iterable.
    bool lowerStringSpanFor(const ast::ForStmt& fs);

    // §29.2: `for var x in obj` over a user-defined IEnumerable<T> type,
    // or over a runtime Array<T>/collection when the iterable's type has
    // a concrete GetEnumerator() / MoveNext() / Current interface.
    // Desugar per spec (IEnumerable):
    //   using var __enum := obj.GetEnumerator();
    //   while __enum.MoveNext() do
    //     var x := __enum.Current;   // borrowing
    //     body
    //   end;
    // Desugar per spec (IOwningEnumerable, §29.2/v0.161/#1349):
    //   using var __enum := obj.GetOwningEnumerator();
    //   while __enum.MoveNext() do
    //     var x := __enum.TakeCurrent();  // ownership transfer
    //     body
    //   end;
    //   __enum.Dispose();   ← via disposeStack_ at scope exit
    // §29.2 (B1): `for var x in items` where items is typed as IEnumerable<T> /
    // IOwningEnumerable<T> / IReadOnlyCollection<T> etc. with null declaration (builtin interface).
    // Uses the vtable protocol (MoveNext at slot 2, Current/TakeCurrent at slot 3)
    // which works for BOTH iterator state machines AND Array<T> (via its vtable).
    // If the actual runtime object is a bz_array_t, falls back to direct loop.
    bool lowerIEnumerableBuiltinFor(const ast::ForStmt& fs);

    bool lowerIEnumerableFor(const ast::ForStmt& fs);

    // fix #2553 (stdlib §4.18, v0.236): `for var x in <SortedSet<T>>` — route
    // through wrapSortedSetAsIEnumerable (the same sorted-snapshot wrap the
    // explicit `.GetEnumerator()` call site uses via #2550/#2546) and dispatch
    // MoveNext/Current via the wrapper's slot-4 enumerator's vtable, so the
    // for-in order matches the §4.18 ascending-iteration contract. The
    // stdlib-synthesised SortedSet.GetEnumerator iterates the dict's internal
    // order[] insertion-order array; lowerIEnumerableFor's direct
    // `emitMethodCall(getEnumSym, ...)` therefore yields insertion order on
    // non-trivial inserts — the two §4.18 surfaces (`using var e := s.GetEnumerator()`
    // vs `for var x in s`) were inconsistent on the same receiver before this
    // helper made them agree.
    bool lowerIEnumerableForSortedSet(const ast::ForStmt& fs,
                                      const sema::TypeRef* iterTy);

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
    // ownership-transferring drain over OwningArray<T>, OwningLinkedList<T>,
    // OwningStack<T>, OwningQueue<T>.
    bool lowerOwnedSeqDrainFor(const ast::ForStmt& fs);

    // #628 / stdlib §4.9 v0.135: `for var entry in ownedDict.Drain()`
    // ownership-transferring drain iteration over OwningDictionary<K,V>.
    bool lowerDictDrainFor(const ast::ForStmt& fs);

    // issue #370: emit a counted loop over a snapshot of generic-table keys
    // (from bz_dict_keys_gen). `tableObj` is the bz_dict_t*; `elemTyRef` is the
    // key/element TypeRef (its LLVM type is how each key is loaded and bound to
    // the loop variable). Used by both lowerDictBackedSetFor and the
    // non-string branch of lowerDictKeysFor.
    // fix #1426: `sorted` param (default false) triggers bz_sort_keys_gen before
    // iteration — used for SortedSet to satisfy §4.18's ascending-order contract.
    bool emitGenericKeyTableFor(const ast::ForStmt& fs, llvm::Value* tableObj,
                                const sema::TypeRef* elemTyRef,
                                bool sorted = false);

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
    // #429 / §27.5: `wasGuardedBoxing` (optional) is set to true when the
    // boxing uses the guarded path (the local was declared outside the current
    // loop body — its cell is allocated ONCE and reused across iterations).
    // Set to false for the fresh-each-iteration path (for-var loop variables
    // or per-iteration re-init locals).  Callers use this to decide whether
    // it is safe to free the cell at inner-scope exit (only fresh-path cells
    // are safe to free inside the loop; guarded cells must survive until the
    // captured variable's own scope exits).
    llvm::Value* boxLocalSlot(llvm::AllocaInst* slot, llvm::AllocaInst*& newSlot,
                              bool forLoopVar = false,
                              bool* wasGuardedBoxing = nullptr);

    // §27.5 / #673: after writing `val` to the cell of a guarded-boxed
    // local (whose cell-holder alloca is `newSlot`), also store `val` to
    // the original pre-boxing alloca if `cellVarSyncSlot_` has an entry.
    // This keeps pre-emitted code that reads the original alloca (e.g. a
    // while-loop condition lowered before the lambda was encountered) in
    // sync with the cell.  No-op for for-var locals (their incBB manages
    // the original slot directly and they have no cellVarSyncSlot_ entry).
    void syncCellToOrigSlot(llvm::AllocaInst* newSlot, llvm::Value* val) {
        auto it = cellVarSyncSlot_.find(newSlot);
        if (it != cellVarSyncSlot_.end() && it->second)
            builder_.CreateStore(val, it->second);
    }

    // §27.5: when a `using var` local is boxed for capture its storage slot
    // changes; update any pending disposal entry so scope-end disposal loads
    // the object through the new (cell-holder) slot.
    void rebindDisposalSlot(llvm::AllocaInst* oldSlot, llvm::AllocaInst* newSlot);

    // §27.5: collect the local/parameter symbols a lambda body references
    // from the enclosing scope (its capture set). Nested lambdas contribute
    // their captures too. Identifier resolution uses sema's exprSymbol map.
    void collectCapturedSyms(const ast::LambdaExpr& lam,
                             std::unordered_set<const sema::Symbol*>& out);

    // §27.5 / #672: collect EVERY LambdaExpr in a statement list, descending
    // into nested lambda bodies (unlike LambdaCollector, which stops at the
    // outermost). Each distinct lambda node corresponds to one closure env, so
    // the caller can count how many envs capture a given local.
    void collectAllLambdas(
        const std::vector<std::unique_ptr<ast::Stmt>>& body,
        std::vector<const ast::LambdaExpr*>& out);

    // §27.5 / §30 (#808 exposure; pre-existing spill-width bug): the LOCAL DECL
    // NODES (LocalDecl* / ForStmt*) captured by ANY lambda in the CURRENT
    // frame body. A captured local's storage slot is rebound to a shared CELL
    // POINTER (boxLocalSlot, §27.5), so its async/iterator frame field must be
    // POINTER-typed: the value-typed field collectSuspendLocals would plan
    // from the declared type (e.g. i32) is overflowed by the 8-byte cell-
    // pointer spill at a suspend point, corrupting the adjacent frame field
    // (garbled pointer → SIGSEGV on resume). Computed by
    // computeFrameCapturedLocals at each frame-building site (async method /
    // async lambda / iterator), consulted in collectSuspendLocals.
    std::unordered_set<const void*> frameCapturedLocals_;
    void computeFrameCapturedLocals(
        const std::vector<std::unique_ptr<ast::Stmt>>& body);

    // §27.5 / #754: does any closure in `fs.body` capture the for-loop variable?
    // Finds the outermost LambdaExprs in the body and runs collectCapturedSyms on
    // each (which descends into nested lambdas), returning true iff some captured
    // symbol is the for-var's LocalSymbol (its `decl == &fs`).
    bool forVarCapturedInBody(const ast::ForStmt& fs);

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

    // §27.6 (v0.184, #1758): produce a closure { thunk, null-env } for a bare
    // reference to a primitive-intrinsic static method (`int.Parse`,
    // `char.IsDigit`, `double.IsNaN`, …), which has no MethodSymbol. `ma` is the
    // `Type.Member` access node; `fnTy` is the function type sema gave the bare
    // reference (`(paramTypes) -> returnType`, §27.6). The thunk `(ptr env,
    // params…) -> ret` re-lowers the intrinsic through tryLowerPrimitiveStaticCall
    // over a synthetic call, so every primitive-static member reachable as a
    // direct call is reachable as a reference with no per-member codegen.
    llvm::Value* makePrimIntrinsicThunk(const ast::MemberAccessExpr& ma,
                                        const sema::TypeRef* fnTy);

    // §27.6 (v0.184, #1758): is `e` a bare reference to a primitive-intrinsic
    // static method (`int.Parse`, `char.IsDigit`, …) used as a function VALUE —
    // the exact shape makePrimIntrinsicThunk mints a fresh +1 closure cell for
    // (no exprSymbol, Function-typed, primitive-keyword `Type.Member` target)?
    // Shared by the CG60 value-lowering gate, isPlusOneClosureProducer (so a
    // bound local releases the cell at scope end), and the LINQ inline-closure
    // ownership check (so a `Select(int.Parse)` temporary is released on
    // enumerator dispose) — one predicate keeps all three recognizing exactly
    // the same set, which is what makes the producer/consumer balance sound.
    bool isPrimIntrinsicMethodRef(const ast::Expr* e) const;

    // #1170: build a fresh closure cell { thunk, env, rc, envdtor, kind }
    // wrapping a (possibly cached) method thunk and the current receiver. Shared
    // by the cache-hit and cache-miss paths of makeMethodThunk. `kind` tags the
    // §27.7.1 equality rule the cell obeys (set via initClosureKind).
    llvm::Value* buildMethodClosureCell(llvm::Function* thunk, bool instMethod,
                                        llvm::Value* receiverVal,
                                        FnValKind kind);

    // §27.7.1 (#1544): classify a method-reference cell's equality kind from the
    // method symbol and whether the receiver is captured. A static / unbound-
    // extension reference (no captured receiver) is FVK_FN_ONLY; a class /
    // interface receiver is FVK_FN_PLUS_RECV; a value-shaped receiver (record /
    // enum / primitive) is FVK_REF_IDENTITY (no identity — equals only itself).
    FnValKind closureKindForMethod(sema::MethodSymbol* ms, bool extRef);

    // §27.6 (#756): a property reference in function-value position →
    // a closure whose thunk computes the getter on the captured receiver.
    llvm::Value* makePropertyThunk(sema::PropertySymbol* ps,
                                   llvm::Value* receiverVal,
                                   const ast::MemberAccessExpr* ma);

    // §27.7.1 (#1544): classify a property-reference cell's equality kind: a
    // static property (no receiver) is FVK_FN_ONLY; a class/interface receiver
    // is FVK_FN_PLUS_RECV; a value-shaped receiver (record) is FVK_REF_IDENTITY.
    FnValKind closureKindForProperty(sema::PropertySymbol* ps,
                                     llvm::Value* receiverVal);

    // §27.7.1 (v0.180, #1544): build a fresh closure cell { thunk, env, rc,
    // envdtor, kind } wrapping a (possibly cached, propertyThunks_) property-
    // getter thunk and the current receiver. Shared by the cache-hit and
    // cache-miss paths of makePropertyThunk — mirrors buildMethodClosureCell.
    llvm::Value* buildPropertyClosureCell(llvm::Function* thunk,
                                          llvm::Value* receiverVal,
                                          FnValKind kind);

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
    // §8.4 (#848): null-safe `=`/`<>` for a record overloading `operator =`/`<>`
    // when an operand is nullable (`T?`) or the `null` literal.
    llvm::Value* lowerNullSafeRecordEquality(const ast::BinaryExpr& bin,
                                             sema::MethodSymbol* opm);

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

    // fix #2722 / stdlib §4.8/§4.9/§4.10 (v0.240): diagnose the hash-collection
    // `IEqualityComparer` constructors, which lowerNew's Set and Dictionary arms
    // previously accepted and then silently DROPPED (building the table with the
    // default content-based trampolines, so a supplied comparer never
    // participated). `sec` is the stdlib section that declares the overload for
    // `colName`. The declaring section is derived from `colName` inside (§4.10
    // covers Set and OwningSet together; the dictionary pair splits into §4.8 and
    // §4.9), so the two dictionary names cannot collapse onto one citation.
    // Returns true when it reported, so the caller can stop rather than fall
    // through into the array arm (which would read the comparer as a capacity and
    // emit malformed IR); the diagnostic is what fails the compile.
    bool rejectUnimplementedComparerCtor(const ast::NewExpr& ne,
                                         std::string_view colName);

    // CG8: address of a field access `obj.f`. Returns the GEP pointer and
    // sets `elemTy` to the field's type. Null if `ma` isn't a field
    // access we can lower. #1039: when `outRecvPtr` is non-null and the
    // receiver is a heap-class instance, it receives the lowered receiver
    // pointer (so a value-read caller can dispose a fresh `new` temporary).
    llvm::Value* fieldAddr(const ast::MemberAccessExpr& ma,
                           llvm::Type*& elemTy,
                           llvm::Value** outRecvPtr = nullptr);

    // #1039 / #1053 / §28.3: a fresh owned-class PRODUCER used ONLY as a
    // member/method-call receiver — a literal `new <class>()` (#1039) or a call
    // to a method returning `owned <class>` (#1053-case-2) — where the extracted
    // result is a primitive / enum / unit value (provably no reference or borrow
    // into the receiver), leaves the temporary owned by nothing and it leaks.
    // Dispose it here (idempotent vtable Dispose + reclaim block exactly once),
    // null-guarded. A no-op for any other receiver/result shape — a non-fresh
    // receiver, a borrow-returning call, or a reference-yielding result is never
    // disposed (no double-free, no UAF). `recvPtr` MUST be the already-lowered
    // receiver pointer (never re-lowered — that would re-run the producer).
    // fix #1859 / §28.3: `calleeReturnsOwned` is true when the CALL (not the
    // field-read overload of this call site) has an ANNOUNCED `owned` return
    // type (ast::MethodDecl::returnOwned). An announced-owned return is the
    // SAME signature-level discriminant isFreshOwnedClassProducer already
    // uses to admit a call as a fresh owned RECEIVER — applied here to the
    // call's RESULT instead: a plain (unmarked) class/interface return is a
    // borrow of the receiver (K1.5, "instance method: source is the
    // receiver") and must never free it (the class_reference_result_not_
    // disposed / borrow_returning_receiver_not_disposed_1053 controls in
    // fresh_owned_receiver_disposal_1039_test.cpp), but an `owned` return
    // announces a FRESH transfer that carries no signature-level borrow of
    // the receiver (only a deferred-activation return type — Task family,
    // an iterator interface — announces a carried receiver borrow, per K6.2;
    // a plain `owned` class/interface return is not one of those kinds).
    // Defaults to false so the field-read call site (which can never reach
    // an `owned` producing position — accessor reads are lend-only, axiom
    // A4) is unaffected.
    void disposeFreshNewReceiverTemp(const ast::Expr* recvExpr,
                                     llvm::Value* recvPtr,
                                     const sema::TypeRef* resultTy,
                                     bool calleeReturnsOwned = false);

    // #913 / #1054 / §28.7: dispose the displaced old value of an owned class
    // local before a reassignment store overwrites it. Fires only when `target`
    // is a plain LOCAL variable (IdentifierExpr → LocalSymbol → LocalDecl) that
    // is a definitely-owned plain class/interface local (unwindOwnedLocalClass:
    // `new C(...)` or an `owned`-returning initializer; collections/tasks/
    // subscriptions/borrow-initialized locals and params all return null). A
    // borrow local/param, a field/index/static target, or an owned param is NOT
    // an owned plain class local here, so its old value is never disposed (no
    // double-free). Null-guarded (a moved-out slot holds null) and identity-
    // guarded (self-assignment `c := c` must not dispose the value it stores).
    // Call immediately before `CreateStore(newVal, addr)`.
    void disposeDisplacedOwnedClassLocal(const ast::Expr* target,
                                         llvm::Value* addr, llvm::Type* elemTy,
                                         llvm::Value* newVal);

    // #1039 / #1053: true if `e` is a FRESH owned-CLASS producer that transfers
    // a +1 the caller solely owns — a literal `new <heap class>()` or a call to
    // a method declared `): owned <heap class>`. A borrow-returning call (no
    // `owned` return), a variable / field / `this`, or a non-class type all
    // return false, so such a receiver's result is never reclaimed as a fresh
    // temporary (no double-free / use-after-free).
    bool isFreshOwnedClassProducer(const ast::Expr* e) const;

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

    // §5.3 (#839): scalar-level primitive conversion used by both
    // lowerConversion (non-nullable target) and the nullable-to-nullable cast
    // path in lowerExpr (NullCondCallExpr with a primitive callee). Converts
    // `payload` from `srcPt` to `targetPt` applying the same checked rules as
    // §5.2 (OverflowException on narrowing / range failure, etc.). Returns the
    // converted scalar (same type as lowerPrimitive(targetPt)) or `payload`
    // unchanged when the types are already identical (identity cast).
    // `srcUns` is the source primitive's signedness (isUnsignedPrim(srcPt));
    // passing it explicitly avoids repeating isUnsignedPrim at every call site.
    llvm::Value* lowerPrimCastScalar(sema::PrimitiveType targetPt,
                                     sema::PrimitiveType srcPt,
                                     llvm::Value* payload,
                                     bool srcUns);

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
    // §6.4: decimal arithmetic for compound-assign / increment (sweep #6).
    llvm::Value* emitDecimalArith(const std::string& op, llvm::Value* lhs,
                                  llvm::Value* rhs);
    llvm::Value* emitDecimalOne();

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

    // #1302 / §22.3 (v0.228) "**`ref` local and field targets alias; accessor
    // targets bracket.**": a `ref`/`out` argument naming an **indexer slot or
    // property** is NOT an alias — it is **copy-in / copy-out** under §17.6's
    // evaluate-once discipline: "the receiver and every index expression are
    // evaluated exactly once, in §22.5's call-site textual order; for `ref`,
    // the getter is called once to produce the callee's slot value (for `out`
    // no getter call occurs); on **normal return** the setter is called once
    // with the slot's final value." One record per such argument.
    struct AccessorRefArg {
        const ast::Expr* target = nullptr;   // the lvalue (`a[i]`, `o.P`)
        llvm::AllocaInst* slot = nullptr;    // the callee's copy-in/out slot
        llvm::Type* valueTy = nullptr;       // the LOCATION's value type
        // The receiver and index subexpressions, lowered ONCE at copy-in and
        // pinned in exprValueOverride_ so the getter here and the setter in
        // the write-back reuse those values instead of re-running a
        // side-effecting subexpression. Erased when the record is flushed.
        std::vector<const ast::Expr*> pins;
    };

    // §22.4/§22.7: lower the call arguments into parameter order, honoring
    // named arguments and default values. Provided arguments are evaluated
    // in source order; defaults afterward in parameter order. Appends to
    // `out`; returns false if an argument can't be placed. When `outExprs` is
    // non-null, appends the source expression for each emitted argument in the
    // same order (a defaulted parameter contributes nullptr) — used to recover
    // each argument's signedness for numeric widening (issue #421 / #410).
    // #1918 / §28.3: when `nullTransferredOwned` is set (only the ordinary
    // user-method call site passes it), the caller's slot for each bare owned
    // local/param argument transferred to an `owned` parameter is NULLED at the
    // "call about to run" point — so a plain owned local discharged on ONE branch
    // via an owned-argument transfer, but LIVE at a throw on a SIBLING branch,
    // is unwind-disposed exactly once (the null-guarded unwindOnly disposal skips
    // the transferred value on the transfer/post-transfer edge, disposes the live
    // one on the sibling-throw edge). Paired with UnwindIneligibleWalker no longer
    // tainting that exact argument shape (LlvmBackend_Setup.cpp).
    // #1302 / §22.3 ("`ref` local and field targets alias; accessor targets
    // bracket"): when `accessorRefArgs` is non-null, a `ref`/`out` argument
    // naming an ACCESSOR-mediated location — an indexer slot or a property,
    // both admitted §22.3 storage categories — is bracketed copy-in /
    // copy-out instead of aliased, and one record per such argument is
    // appended for the caller to flush with emitAccessorRefArgWriteBacks
    // after the call returns normally. When it is null (the constructor /
    // record paths, which have no post-call flush point) the behavior is
    // unchanged: `lvalueAddr` returns null for those locations and the
    // caller's existing #1386 diagnostic fires.
    bool buildOrderedArgs(const std::vector<ast::Param>& params,
                          const std::vector<ast::CallArgument>& args,
                          std::vector<llvm::Value*>& out,
                          std::vector<const ast::Expr*>* outExprs = nullptr,
                          bool nullTransferredOwned = false,
                          std::vector<AccessorRefArg>* accessorRefArgs = nullptr);

    // #1302 / §22.3: emit the copy-IN half of an accessor-mediated `ref`/`out`
    // argument and record what the copy-OUT half needs. `target` is the
    // argument's lvalue expression (`a[i]`, `d[k]`, `o.P`), `isOut` selects
    // the `out` variant (no getter call — §22.3). Returns the address of the
    // callee's slot, or null when `target` is not an accessor-mediated
    // location this bracket can drive (the caller then falls through to its
    // existing #1386 diagnostic).
    //
    // fix #2462 (§22.3/§2.8.2 clause 1/§2.8.3 rule 4, v0.235): `paramTy` is
    // the callee's declared parameter type, ALREADY LOWERED — was formerly
    // `const ast::Param&`, but the two IMPORTED-callee call sites this fix
    // adds (LlvmBackend_Calls2.cpp) have no `ast::Param` at all (an imported
    // method's own AST is never reconstructed — #1221's same limitation
    // #2400/#2412/#2415 hit elsewhere). `paramTy` is used only to WIDEN the
    // slot when the callee's declared width exceeds the target's own type
    // (the #2266 slot-clobber shape); §22.3's "exact referent type" rule
    // means the common case has `paramTy == valueTy` anyway, so a caller
    // that cannot cheaply recover the callee's declared type may pass
    // `nullptr` — `valueTy` (from the target's own exprType) alone still
    // drives the slot's primary type, so this degrades the #2266 widening
    // refinement only, not ordinary correctness.
    llvm::Value* emitAccessorRefArgCopyIn(const ast::Expr& target,
                                          llvm::Type* paramTy, bool isOut,
                                          std::vector<AccessorRefArg>& sink);

    // #1302 / §22.3: emit the copy-OUT half — "on **normal return** the setter
    // is called once with the slot's final value", in argument textual order.
    // `emit` is false on the exception edge, where "no write-back occurs";
    // either way the pinned receiver/index values are released. Clears `recs`.
    void emitAccessorRefArgWriteBacks(std::vector<AccessorRefArg>& recs,
                                      bool emit);

    // #1955 / §28.3: null the caller's slot for a call ARGUMENT that transferred
    // a bare owned LOCAL / owned PARAMETER's ownership to an owned recipient at a
    // lowering site OUTSIDE buildOrderedArgs — an owning-collection element-
    // transfer method (`OwningArray.Add(owned x)` / Push / Enqueue / Insert). The
    // null store dominates the transfer's post-throw edge so the caller's null-
    // guarded unwindOnly disposal — which UnwindIneligibleWalker leaves eligible
    // for exactly this shape (calleeIsOwningCollectionTransfer) — is exactly-once
    // (skips the moved value; disposes one still live at a preceding throw). Same
    // effect buildOrderedArgs' `nullTransferredOwned` has for the call/ctor paths.
    // A no-op unless `argValue` is a bare `var` owned local or an `owned`
    // parameter of pointer representation. Call AFTER the element is consumed.
    void nullTransferredOwnedLocalSlot(const ast::Expr* argValue);

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
    // per §4.6/§4.7: true for LinkedList<T> / OwningLinkedList<T> (bz_llist_t-backed).
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
            "Array", "OwningArray", "Dictionary", "OwningDictionary",
            "Stack", "OwningStack", "Queue", "OwningQueue",
            "Set", "OwningSet", "LinkedList", "OwningLinkedList",
            "LinkedListNode", "StringBuilder", "Random",
            "IEnumerable", "IEnumerator", "IOwningEnumerable", "IOwningEnumerator",
            "IAsyncEnumerable", "IAsyncEnumerator",
            "IOwningAsyncEnumerable", "IOwningAsyncEnumerator",
            "IReadOnlyCollection", "IReadOnlyList", "ICollection", "IList",
            "IOwningCollection", "IOwningList", "Task", "OwningTask",
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
            // §6.2 / #647: Mutex and Lock are compiler-known heap-pointer builtins
            // (bz_mutex_t* and bz_lock_t* respectively). Without these entries a
            // nullable `Lock?` was mis-classified as a bare generic type-parameter
            // and the nullable-type-param branch (fix #568) lowered it to {i8,i32}
            // instead of ptr → emitStructEquality crash on `guard <> null`.
            "Mutex", "Lock",
            // §6.3 / #647: Semaphore and SemaphoreToken are compiler-known
            // heap-pointer builtins (bz_semaphore_t* and bz_semaphore_token_t*
            // respectively). Same fix as Mutex/Lock above: without these entries a
            // nullable `SemaphoreToken?` was mis-classified as a bare generic
            // type-parameter and the nullable-type-param branch lowered it to
            // {i8,i32} instead of ptr → emitStructEquality crash on `tok <> null`.
            "Semaphore", "SemaphoreToken",
            // §6.6 / issue #974: TaskCompletionSource<T>/OwningTaskCompletionSource<T>/
            // TaskCompletionSource — compiler-known heap-pointer builtins (bz_tcs_t*).
            "TaskCompletionSource", "OwningTaskCompletionSource",
            // §6.4 / #647: Atomic<T> and AtomicInteger<T> are compiler-known
            // heap-pointer builtins (bz_atomic_t*).  Without these entries a
            // nullable `Atomic<T>?` was mis-classified as a bare generic
            // type-parameter and the nullable-type-param branch lowered it to
            // {i8,i32} instead of ptr.
            "Atomic", "AtomicInteger",
            // issue #545: Buffer<T> is a compiler-known heap-pointer builtin
            // (bz_buffer_hdr_t* + inline payload).  Without this entry a
            // nullable `Buffer<T>?` would be mis-classified as a bare generic
            // type-parameter and lowered to {i8,i32} instead of ptr.
            "Buffer",
        };
        if (kBuiltins.count(name)) return true;
        // §31.4: a built-in exception type (Exception / a subtype).
        return builtinVtable_.find(std::string(name)) != builtinVtable_.end();
    }

    static bool isDictTypeName(const sema::TypeRef* rt);

    // stdlib §6.7 / §30.5: true when `rt` is a Task / Task<T> or OwningTask<T>
    // (bare name "Task" or "OwningTask", with or without generic args, no user
    // TypeDecl — these are builtin runtime types, not user-defined classes).
    static bool isTaskTypeName(const sema::TypeRef* rt);
    // stdlib §6.7 / §30.5: return a local's resolved Task / Task<T> TypeRef if
    // it is Task-typed; null otherwise. Used by the using-var registration path.
    const sema::TypeRef* taskTypeOfLocal(const ast::LocalDecl* ld);
    // §30.15 (v0.179, #1524): true when `ld`'s initializer is (syntactically)
    // a no-arg `Receive()` call on a `Channel<T>` receiver — the same shape
    // tryLowerChannelAndDomainCall recognises when lowering the call itself.
    // Used to route the task-local's scope-end drop through the abandonment
    // give-back protocol instead of the ordinary task disposal.
    bool isChannelReceiveInitializer(const ast::LocalDecl* ld);
    // §28.3 v0.190 SB-01 (#1786 / #1870): true when a task-typed local's
    // initializer is a BORROW SOURCE — a bare-identifier ALIAS of another task
    // binding, or a READ of task-typed STORAGE (a field/property access, incl.
    // the null-conditional form, or an indexer read). Under "calls mint,
    // storage lends" such a local does NOT own the task frame, so it must NOT
    // register a scope-end task disposal — its owner (the mint CALL) disposes
    // the one frame. Mirrors producerState's Owned-vs-Borrow split.
    bool taskLocalInitializerIsBorrowSource(const ast::LocalDecl* ld);

    // §28.7 / §30.5 (#2173): is `target` an `owned` Task-family field lvalue
    // that needs drop-on-overwrite? Mirrors isOwnedClassFieldLvalue's scope
    // exclusions (locals, ref params, static fields) and the class-instance
    // restriction. Used by lowerOwnedTaskFieldStore to gate the field-store
    // emit; emitOwnedFieldDisposals uses taskFamilyTypeFromField directly for
    // its scope-end classification (it doesn't need the lvalue shape).
    bool isOwnedTaskField(const ast::Expr& target);
    // §28.7 / §30.5 (#2173): recover the resolved Task / Task<T> / OwningTask<T>
    // TypeRef for an `owned` field. Mirrors taskTypeOfLocal's local-form pattern
    // but reads the declared type from the FieldDecl (or importedMemberType for
    // an imported field). Null if the field's type is not Task-family.
    const sema::TypeRef* taskFamilyTypeFromField(const sema::FieldSymbol* fs);
    // §28.3 / §30.5 (#1905): dispose an already-evaluated owned Task-family
    // value with the complete protocol (fault object, unconsumed owned result,
    // then fire-and-forget/free). Used at an unbound task receiver's full-
    // expression boundary after synchronous Wait(), and by the field wrapper
    // below. `taskTy` supplies the OwningTask<T> result disposer when needed.
    void emitTaskDisposal(const sema::TypeRef* taskTy,
                          llvm::Value* task);
    // §28.7 / §30.5 (#2173 / #1889): drop-on-overwrite / scope-end of an
    // owned Task-family FIELD. Invoke the full task-disposal sequence
    // (error_obj prelude + per-T result_disposer attach +
    // bz_task_fire_and_forget[_checked]) at the OLD task pointer. The caller
    // is responsible for null-guarding / nulling the slot (the field-store
    // caller has stored the new value BEFORE this call per the store-new-
    // FIRST ordering in #2173; the scope-end caller nulls the slot AFTER).
    // `fs` is the field's FieldSymbol — used to resolve the declared Task /
    // Task<T> / OwningTask<T> type via taskFamilyTypeFromField for the per-T
    // result_disposer derivation. Shared by lowerOwnedTaskFieldStore (the
    // field-store call site) and emitOwnedFieldDisposals (the scope-end
    // #1889 path). Mirrors the using-var task disposal sequence in
    // emitDisposals -> if (it->task) block; extracted to a single helper so
    // the two field paths cannot drift.
    void emitOwnedTaskDisposal(const sema::FieldSymbol* fs,
                              llvm::Value* oldVal);

    // issue #401 / stdlib §4.1 / §4.2: is `rt` one of the int-indexed list
    // INTERFACES (IReadOnlyList<T>, IList<T>, IOwningList<T>)? These are the only
    // collection interfaces that declare an indexer, and the only implementers
    // are the array-backed Array<T> (IList) / OwningArray<T> (IOwningList) — so an
    // interface-typed receiver holds the same bz_array_t* and lowers through the
    // same runtime as the concrete array path. `outOwning` (optional) is set to
    // true for IOwningList<T> (the owning-set, disposing variant) and false for
    // the borrowing/read-only interfaces.
    static bool isListInterfaceName(const sema::TypeRef* rt,
                                    bool* outOwning = nullptr);

    // #2603 / §28.9 / issue #401: the BORROWING list interfaces — IList<T> and
    // IReadOnlyList<T>. Both are array-backed (same bz_array_t* as Array<T>) and
    // borrowing (the holder does not own the elements, except the runtime-
    // managed string +1 co-ownership), so an `owned` field of either static type
    // routes through the IDENTICAL collection-disposal path as `owned Array<T>`:
    // bz_array_free with the element disposer from elementPolicyFor (and the
    // string-element-release thunk for a string element). IOwningList<T> is
    // EXCLUDED here — it is the OWNING variant (its elements are owned by the
    // holder) and is covered by isOwningListInterfaceName below (#2606).
    static bool isBorrowingListInterfaceName(const sema::TypeRef* rt) {
        bool owning = false;
        return isListInterfaceName(rt, &owning) && !owning;
    }

    // #2606 / §28.11 / stdlib §4.5 / issue #401: the OWNING list interface —
    // IOwningList<T>. It is array-backed (same bz_array_t* as OwningArray<T> —
    // the only implementer per stdlib §4.5) AND owning: its elements are owned
    // by the holder and disposed on the collection's Dispose (stdlib §4.5
    // "Disposing an OwningArray<T> automatically disposes every element"). So an
    // `owned` field of this static type routes through the collection-disposal
    // path with collIsOwning=TRUE — bz_array_free frees the backing and the
    // class-element disposer from elementPolicyFor(elem, owning=true) disposes
    // each element, IDENTICAL to `owned OwningArray<T>`. (String elements are
    // rejected at instantiation for owning collections per stdlib §4.4, so the
    // borrowing-family string-element-release gate never applies here.) The
    // `isOwningPrefixedName("IOwningList")` check used at the collIsOwning
    // derivation sites returns FALSE (the prefix is "Owning", but the head
    // starts with "I"), so this helper is also consulted there to mark the
    // family owning.
    static bool isOwningListInterfaceName(const sema::TypeRef* rt) {
        bool owning = false;
        return isListInterfaceName(rt, &owning) && owning;
    }

    // fix #853 / stdlib §4.1–§4.3: the *non-list* collection interfaces
    // (IReadOnlyCollection<T>, ICollection<T>, IOwningCollection<T>). These are
    // genuinely polymorphic — implemented by collections with incompatible
    // runtime representations (bz_array_t*/bz_llist_t*/bz_dict_t*) with no shared
    // kind tag — so an interface-typed receiver cannot be soundly routed to a
    // single runtime at compile time. Member calls through them are diagnosed,
    // not miscompiled. (The list interfaces above are array-backed-only and ARE
    // soundly bridged.)
    static bool isCollectionInterfaceName(const sema::TypeRef* rt);

    // issue #343 / §4.8: if the type's key generic arg (genericArgs[idx]) is a
    // record (a value type with synthesized GetHashCode/Equals), return its
    // TypeSymbol; otherwise null (string/int keys use the existing fast paths).
    // Non-static: routes through isAutoEqualityBearingBodyRecord, which needs
    // ctx_ to resolve component types (§9.2 v0.158).
    const sema::TypeSymbol* recordKeyType(const sema::TypeRef* collTy,
                                          unsigned idx);

    // issue #343 / §9.1 / §9.2: if `ref` itself resolves to an
    // equality-bearing record type (positional or auto-equality body record),
    // return its TypeSymbol; otherwise null. Used to route a record *element*
    // sequence (e.g. lazy LINQ Distinct) through structural equality.
    const sema::TypeSymbol* recordTypeOfRef(const sema::TypeRef* ref);

    // issue #343 / §8.3: build C-ABI trampolines over a record key type's
    // synthesized hash / equality for the generic dict/set runtime variant.
    //   hash: uint64_t(const void* key)  — loads the record struct, hashes it.
    //   eq:   bool(const void* a, const void* b) — loads both, compares.
    // The runtime stores raw key bytes; the trampolines reinterpret the byte
    // pointer as a record-struct pointer.
    llvm::Function* buildRecordHashTrampoline(const sema::TypeSymbol* rec);
    llvm::Function* buildRecordEqTrampoline(const sema::TypeSymbol* rec);
    // §35.7 (v0.200, #2047): the choice-key analogues — a value-like choice used
    // as a Dictionary/Set key hashes/compares by its §35.7 tag + active-payload
    // form (emitChoiceHashCode / emitChoiceEquality). Call under the key's
    // instantiation substitution so a generic choice key (`Result<int, string>`)
    // uses the specialized struct + concrete per-payload hashing/equality.
    llvm::Function* buildChoiceHashTrampoline(const sema::TypeSymbol* cho);
    llvm::Function* buildChoiceEqTrampoline(const sema::TypeSymbol* cho);

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
    // #1414 stage 4: const-qualified (pure reads of genericSubstSemaTy_) so the
    // `const` isFreshOwnedClassProducer wrapper can inject it as the shared
    // ownership-facts core's TypeParamResolver hook.
    // §2.8.2/§11.8 (#2359 bug 2c): a bare type-parameter TypeRef resolved
    // through an IMPORTED constraint interface (e.g. `T.Zero()`'s return,
    // or a constraint-static-operator operand, when the interface itself
    // came from a dependency artifact) carries no `astNode` at all — same
    // gap as bugs 2a/2b/2d (ModuleLoader's metadata-built TypeRef has only
    // `importedName`). The old `astNode`-only guard left the param
    // unresolved for exactly this shape, so the redirect below in
    // lowerBinary saw `concrete == lt` (unsubstituted) and fell through to
    // the primitive/placeholder intrinsic path — emitting a raw pointer
    // `add` on two class-typed operands (malformed IR). Read the name via
    // the shared alias-transparent `TypeRef::headName()` (falls back to
    // `importedName`) instead, matching the fix shape used at the sema
    // sites.
    const sema::TypeRef* resolveTypeParamRef(const sema::TypeRef* ty) const {
        if (!ty || ty->kind != sema::TypeRef::Kind::Named || ty->declaration ||
            genericSubstSemaTy_.empty() || ty->headName().empty())
            return ty;
        auto it = genericSubstSemaTy_.find(ty->headName());
        if (it != genericSubstSemaTy_.end() && it->second) return it->second;
        return ty;
    }

    // §28.1 (v0.196, #1817 E1 / #1860): does an `out owned T` RECIPIENT of type
    // `rt` carry a scope-end disposal obligation at THIS substitution?  Only a
    // genuine class/interface substitution does; at a value/runtime-managed
    // substitution the marker is erased (§28.1) — a primitive incl. string, a
    // record/enum, or a value-builtin (DateTime/Guid/…, whose declaration kind
    // is Class but is `isValueBuiltin`) carries no obligation.  Root `Object`
    // (Primitive::Object) is the class-like exception and DOES dispose.  Mirrors
    // the ownership pass's `isValueSubstitutionErased` (its inverse over the
    // heap-obligation cases), the drift locus these two must track.  Resolves a
    // bare-T ref through the ACTIVE substitution first (the recipient is
    // monomorphic here), so a class instantiation still disposes and a value one
    // is a clean no-op.  Gates ensureOutVarSlots' out-owned dispose emission so
    // the erased value is not dynamic-disposed as an object.  // per §28.1 v0.196
    bool outOwnedRecipientDisposes(const sema::TypeRef* rt) const {
        auto genuineHeapObligation = [](const sema::TypeRef* t) {
            if (!t) return false;
            if (t->kind == sema::TypeRef::Kind::Primitive)
                return t->primitive == sema::PrimitiveType::Object;
            if (t->kind != sema::TypeRef::Kind::Named || !t->declaration)
                return false;
            if (t->declaration->isValueBuiltin) return false;  // value type
            return t->declaration->kind == ast::TypeDeclKind::Class ||
                   t->declaration->kind == ast::TypeDeclKind::Interface;
        };
        return genuineHeapObligation(rt) ||
               genuineHeapObligation(resolveTypeParamRef(rt));
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
    // fix #1426 / stdlib §4.17–§4.18: Build a cmp(const void* a, const void* b)
    // thunk for SortedDictionary/SortedSet ordered-key operations.
    // a and b are direct pointers to key bytes (NOT void** of void*).
    // Supports integer (i32/i64/i8/i16/i1) and float (f32/f64) keys.
    // Unsupported types (record, decimal, enum, unresolved T): returns null.
    llvm::Function* buildSortedKeyComparator(const sema::TypeRef* keyTy);

    // §3: instance method calls on Array<T>, Dictionary<K,V>, etc.
    // Called from lowerCall before the normal method-symbol dispatch.
    llvm::Value* tryLowerCollectionInstanceCall(const ast::CallExpr& call);

    // fix #2513 / spec §35.2 (v0.236): the stdlib §2.20 `Result<T, E>` carries
    // four compiler-provided per-instantiation combinators (Map / MapError /
    // AndThen / UnwrapOr) — only admissible where the receiver AND result
    // instantiation are value-like (§35.4 rule 3). These members have NO
    // MethodSymbol (they are compiler-provided; the prelude body injection that
    // the previous lane attempted aborts on a choice-shaped return's PHI in
    // lowerCaseExpr). The dedicated codegen path emits the inline body — a
    // switch on the Ok/Error discriminator tag + branch-specific bodies — and
    // returns null when the call is not a Result combinator (the regular
    // method-symbol dispatch then runs unchanged).
    llvm::Value* tryLowerResultCombinatorCall(const ast::CallExpr& call);

    // fix #2513 helper: invoke the function-typed expression `arg` (a lambda
    // or function parameter) with the single payload value `val`. Mirrors
    // lowerCall's closure-call arm's fn-ptr / env-ptr / retTy derivation.
    llvm::Value* invokeClosureOnValue(const ast::Expr& arg, llvm::Value* val,
                                      llvm::Type* valLL,
                                      const sema::TypeRef* retSemTy);
    // fix #2513 helper: build a choice struct value { i32 tag, ok_payload,
    // err_payload } with the active case's payload filled from `okPayload`
    // or `errPayload`. Inactive slots stay undef (per §35.4 choice values
    // carry no per-slot moved-from state). Used by every combinator branch.
    llvm::Value* buildChoiceStructValue(const sema::TypeSymbol* choiceTy,
                                        unsigned activeTag,
                                        llvm::Value* okPayload,
                                        llvm::Value* errPayload);
    // per §4.6/§4.7: lower LinkedList<T>/OwningLinkedList<T> instance calls
    // (Add*/Remove*/Find/Count/First/Last/etc.) and LinkedListNode<T> property
    // accesses (.Value/.Next/.Previous) via the bz_llist_* runtime.
    llvm::Value* tryLowerLinkedListInstanceCall(const ast::CallExpr& call);
    // stdlib §5 (#755, supersedes the #1296 hand-rolled terminal set): the
    // ELEMENT TypeRef of an `IGrouping<TKey, T>` — genericArgs[1] (genericArgs[0]
    // is TKey). Returns null when `rt` is not an IGrouping. IGrouping is a
    // compiler-known builtin (common/BuiltinRegistry) with no user declaration,
    // and its TypeRef reaches codegen in three shapes — a declaration-less
    // astNode (annotated/inferred local), a builtin TypeSymbol whose `decl` is
    // null, or an `importedName` (cross-module metadata) — so all three are
    // recognized here.
    static const sema::TypeRef* iGroupingElemTypeRef(const sema::TypeRef* rt);

    // stdlib §5 (#755) / §4.4: a synthesized `Array<T>` TypeRef over `elem`.
    // `IGrouping<TKey, T>`'s elements are a concrete `bz_array_t*` the grouping
    // owns (bz_grouping_elements), so the array-receiver lowerings — every §5
    // terminal, with their spec-mandated bounds/arity faults — apply verbatim
    // once the receiver TYPE is restated as the element array's. Mirrors
    // builtinByteArrayTypeRef / builtinTaskArrayTypeRef in construction.
    sema::TypeRef* elementArrayTypeRef(const sema::TypeRef* elem);

    // §4.15 (#549): lower Array<T>.AsSpan()/AsReadOnlySpan() and
    // ArraySpan<T>/ReadOnlyArraySpan<T> instance method calls
    // (Length, IsEmpty, Slice, AsReadOnly, IndexOf, Contains, Fill,
    //  CopyFrom, CopyTo, Reverse, SequenceEqual).
    llvm::Value* tryLowerArraySpanCall(const ast::CallExpr& call);

    // §15.1.2 / #1342: lower an inline-array `[N]T` intrinsic call:
    //   ToArray()    → fresh owned Array<T> (bz_array_t*) containing copies of
    //                  all N element slots; string elements are +1 retained.
    //   Contains(v)  → i1 from a linear scan using emitRemoveElementEquality
    //                  (§9.2: value-eq for values, content-eq for strings,
    //                  identity/Equals for classes, structural for records).
    // Returns nullptr when `call` is not an InlineArray receiver call.
    llvm::Value* tryLowerInlineArrayCall(const ast::CallExpr& call);

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

    // issue #995 / stdlib §5 / §29.11: is `rt` a VTABLE-BACKED IEnumerable<T> /
    // IOwningEnumerable<T> — an interface-typed local/param/field or a yield-method
    // result whose runtime value is the iterator-vtable object (not a bz_lazy_t*
    // / bz_array_t*)? Unlike the lazy sentinel (isLazyEnumerableType) it carries
    // the interface name in an astNode or importedName. Such a receiver is
    // materialized into an Array<T> via materializeVtableIEnumerableToArray so the
    // LINQ operators/terminals and for-in compose uniformly (was the #995
    // miscompile/NYI).
    bool isVtableBackedIEnumerableType(const sema::TypeRef* rt);

    // issue #995 / stdlib §5 / §29.11: materialize a vtable-backed IEnumerable<T>
    // receiver into a fresh owned Array<T> (a bz_array_t* of element bytes) by
    // driving its iterator-vtable protocol once (GetEnumerator/MoveNext/Current/
    // Dispose). Disposes the per-consumption enumerator, and the source iff it is
    // a temporary (a named local/param/field source is disposed at its own scope
    // end). `elemRef` receives the element TypeRef. Returns the array, or null if
    // the receiver is not vtable-backed.
    llvm::Value* materializeVtableIEnumerableToArray(const ast::Expr& recvExpr,
                                                     sema::TypeRef*& elemRef);

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
    //
    // #1914: `ownFreshGroupings` — when the receiver is a bare `GroupBy(sel)`
    // whose fresh groups temporary is consumed by a DRAIN-then-DISPOSE consumer
    // that reads each grouping only DURING iteration (the for-in loop), pass true
    // so the lazy source OWNS and disposes the groupings + array
    // (bz_lazy_from_owned_grouping_array). Default false (the borrowing
    // bz_lazy_from_array) is required for consumers that either re-own the
    // groupings via a produced `Array<IGrouping>` local (ToArray) or read an
    // element pointer AFTER the materialize-and-dispose (First/Aggregate over a
    // snapshot) — owning there would double-free / UAF respectively. Ignored for
    // non-GroupBy receivers.
    llvm::Value* lowerToLazyEnumerator(const ast::Expr& recvExpr,
                                       sema::TypeRef*& elemRef,
                                       bool ownFreshGroupings = false,
                                       llvm::Value** outRecvArr = nullptr);

    // stdlib §5 (#651/#752): materialize a `GroupBy(keySelector)` call into an
    // owned `Array<IGrouping<TKey, T>>` (a bz_array_t* of bz_grouping_t*
    // pointers) via bz_groupby_to_array. `srcCall` is the GroupBy CallExpr
    // (callee a MemberAccessExpr with member "GroupBy", one argument). Returns
    // the groups array pointer, or null on failure. Shared by the
    // `GroupBy(...).ToArray()` interception and by lowerToLazyEnumerator (a bare
    // `GroupBy(...)` lowers to a lazy enumerator over this array so it composes
    // with Count/Any/Sum/for-in like any other lazy source).
    llvm::Value* lowerGroupByMaterialize(const ast::CallExpr& srcCall);

    // stdlib §5 (#651/#752): is `e` a bare `GroupBy(keySelector)` call — a
    // CallExpr whose callee is a MemberAccessExpr with member "GroupBy" and
    // exactly one argument? Used to intercept the GroupBy shape in
    // lowerToLazyEnumerator and the scalar-terminal path (#1924). Member so both
    // LlvmBackend_Calls3.cpp and LlvmBackend_Calls.cpp can call it.
    static bool isGroupByCall(const ast::Expr* e);

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
    // `rangeInnerIsChar` (§16.3, v0.183, #1586): true when `rangeInner` and the
    // selector's Range<T> result has T = char — the emitted bz_lazy_from_range
    // call flags the enumerator to step across the UTF-16 surrogate gap.
    // `groupingInner` (stdlib §5, #755): true when the selector returns an
    // `IGrouping<TKey, U>` (`GroupBy(k).SelectMany(lambda(g) => g)`) — the
    // returned pointer is a `bz_grouping_t*`, not a `bz_array_t*`, so the
    // trampoline reads the grouping's element array (bz_grouping_elements)
    // before wrapping. Without it the grouping struct was read AS an array
    // header, whose second field is `key_esz` — every group then yielded
    // key_esz elements regardless of its size (issue #755 finding [20]).
    llvm::Function* buildSelectManyTrampoline(llvm::Type* elemTy,
                                              int64_t innerEsz,
                                              bool innerIsLazy = false,
                                              bool rangeInner = false,
                                              bool rangeInnerIsChar = false,
                                              bool groupingInner = false);

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
    // §6.2 / issue #647: Mutex.Acquire, TryAcquire, AcquireAsync, TryAcquireAsync.
    llvm::Value* tryLowerMutexLockCall(const ast::CallExpr& call);
    // §6.3 / issue #647: Semaphore.Acquire, TryAcquire, AcquireAsync, TryAcquireAsync.
    llvm::Value* tryLowerSemaphoreTokenCall(const ast::CallExpr& call);
    // §30.12 (v0.179 / #1522): shared would-park guard for Mutex.Acquire()/
    // TryAcquire(timeout) and Semaphore.Acquire()/TryAcquire(timeout). Probes
    // non-blocking (timeout=0) when inside a domain continuation and raises
    // InvalidOperationException instead of parking a thread; unchanged
    // blocking behavior outside a domain. See tryLowerMutexLockCall /
    // tryLowerSemaphoreTokenCall for callers.
    llvm::Value* emitGuardedBlockingAcquire(llvm::Value* handle,
                                             const char* tryAcquireFnName,
                                             const char* acquireFnName,
                                             llvm::Value* timeoutMs);
    // §6.6 / issue #974: TaskCompletionSource<T>/OwningTaskCompletionSource<T>/
    //   TaskCompletionSource instance methods: TrySetResult, SetResult,
    //   TrySetException, SetException, TrySetCancelled, SetCancelled, Dispose.
    llvm::Value* tryLowerTcsCall(const ast::CallExpr& call);
    // §6.4 / issue #647: Atomic<T>/AtomicInteger<T> Exchange, CompareExchange,
    //   Increment, Decrement, Add; also Value setter (a.Value := v) via call shape.
    llvm::Value* tryLowerAtomicCall(const ast::CallExpr& call);

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

    // §22.4 / §7.9 (v0.235, #2395; extended #2402): recursively pre-create
    // (and, via ensureOutVarSlots / ensureCtorOutVarSlots,
    // §7.9-default-zero-initialize) the slot for every inline `out var` call
    // OR constructor call found inside `e` — called from lowerShortCircuit
    // BEFORE its conditional branch is emitted, mirroring the placement
    // lowerNullCondCall already uses for `?.` (#2345): entryAlloca hoists the
    // ALLOCA to the function entry regardless of insertion point, but the
    // zero-STORE ensureOutVarSlots/ensureCtorOutVarSlots emits lands wherever
    // the builder currently is — inside `and`/`or`'s conditionally-reached
    // RHS block if called from there, which does not dominate the skip path.
    // Calling this first, while still in the block that dominates both the
    // skip and the taken path, fixes that. Bounded to the shapes a
    // short-circuit skip can directly wrap — nested `and`/`or`, `not`, a
    // call (and its callee) combined with a further operator
    // (`recv?.M(out var x) = true`), a `new T(out var x)` constructor call,
    // and a member access on one (`new T(out var x).field`, #2402's own
    // repro shape) — the same scope DefiniteAssignment.cpp's
    // evalCond/evalNullCondCallArgs track (its leaf case falls through to the
    // general visitExpr walker, which recurses into MemberAccessExpr/NewExpr
    // exactly as this does); a call nested in a lambda/conditional-expression
    // body is out of scope for both (§22.4 does not extend inline out-var
    // path-sensitivity there; see #2395's own PR notes for the discovery
    // boundary).
    void hoistSkippableOutVarSlots(const ast::Expr& e);

    // §28.3 / §28.11 / §22.4 (#1466): is the argument at `pos` in `call` a
    // genuine `out owned` transfer position — a USER method whose formal at
    // `pos` is declared `out owned`, or one of the builtin owned-OUT collection
    // Try* methods (OwningStack.TryPop, OwningQueue.TryDequeue, OwningArray/
    // OwningLinkedList.TryPopFirst/TryPopLast, OwningSet.TryTake,
    // OwningDictionary.TryPopValue, Weak.TryUpgrade, Channel<T>.TryReceive for
    // class/interface T)? Mirrors MethodOwnership::builtinOutOwnedArg /
    // markOutOwnedArgs (src/ownership/Ownership_Checks.cpp) so codegen recognizes
    // the SAME positions the ownership pass marks Owned. Consulted so a PRE-
    // DECLARED recipient passed via the reachable plain `out item` call-site form
    // (v0.160 dropped the bare `out owned item` spelling; the parser emits the
    // plain "out" modifier for it) is also registered in outOwnedRecipients_ and
    // given a scope-end reclaim — distinct from an ordinary borrowing `out` arg.
    bool isOutOwnedArgPosition(const ast::CallExpr& call, std::size_t pos);

    // §22.4 (#968): the constructor-call analogue of ensureOutVarSlots. Pre-
    // creates the `out var x` slot for a `new T(out var x)` argument (typed from
    // the matching constructor's out parameter) and registers it in locals_ so
    // lowerNew's arg lowering passes its address and a later read sees the value
    // the constructor wrote. Must be called at the start of lowerNew.
    void ensureCtorOutVarSlots(const ast::NewExpr& ne, const sema::TypeRef* rt);

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

    // per §14.1/§14.2 + §26.5/§7.5 (issue #1278): compute and INSTALL (into
    // genericSubst_/genericSubstSemaTy_) the method-level generic-parameter
    // substitution for a call to generic method `ms` — from argument-type
    // structural unification (#785), explicit call-site type args on the
    // callee (`obj.Method<T>(...)` MemberAccessExpr, `obj?.Method<T>(...)`
    // NullCondMemberExpr, or a bare `Id<T>(...)` IdentifierExpr — #509/#561),
    // and finally the call's own (possibly target-inferred, #391) result type.
    // A no-op when `ms->decl` has no method-level generic params. Builds on
    // top of whatever genericSubst_/genericSubstSemaTy_ the caller already
    // has active (e.g. a receiver-class substitution) — the caller is
    // responsible for saving/restoring those maps around the call.
    // Factored out of lowerCall (the `.` call path) so lowerNullCondCall
    // (`?.`) drives the SAME monomorphization trigger — previously only the
    // `.` path installed this substitution, so an explicit-type-arg `?.`
    // generic call reached emitMethodCall with an empty genericSubst_ and
    // called the unspecialized i32-placeholder body (malformed IR).
    void applyMethodGenericSubst(const ast::CallExpr& call,
                                 sema::MethodSymbol* ms);

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

    // §24.3 (#747): lower an event handler argument (a lambda / method ref) to a
    // closure {fn, env}, informing lowerLambda of the expected `(payload) -> ()`
    // type so an untyped `(e) => …` param resolves the payload and a block-bodied
    // handler resolves its return to void (not the i32 default). Shared by the
    // direct-event `.Subscribe`/`.Add`/`.Remove` path and the stored-IEvent<T>
    // `.Subscribe` path. Returns null on failure.
    llvm::Value* lowerEventHandlerClosure(const ast::Expr* handlerExpr,
                                          const sema::TypeRef* payload);
    // §24.3 (#747): is `rt` the builtin IEvent<T> / IAsyncEvent<T> interface
    // (a Named TypeRef with a null user declaration whose first name part is
    // "IEvent"/"IAsyncEvent")? Mirrors isSubscriptionType.
    static bool isIEventTypeName(const sema::TypeRef* rt);
    // §6.5 / §24.3 (#1610): producer-side "EventSource"/"AsyncEventSource".
    static bool isEventSourceTypeName(const sema::TypeRef* rt);

    // per §24.4: `<event> += h` / `<event> -= h` inside the declaring class —
    // a fixed desugaring to Add/Remove. Returns true when handled.
    bool tryLowerEventCompound(const ast::AssignmentStmt& as);
    // §6.4 / #647: `atom.Value := v` — seq-cst store into an Atomic<T> or
    // AtomicInteger<T> cell via bz_atomic_store.  Returns true when handled.
    bool tryLowerAtomicValueSet(const ast::AssignmentStmt& as);
    // stdlib §7.1 / #645: `fs.Position := n` on a FileStream receiver — an
    // absolute seek. Same shape/reason as tryLowerAtomicValueSet: the builtin
    // has no user PropertyDecl, so without this the write is silently dropped.
    bool tryLowerFileStreamPositionSet(const ast::AssignmentStmt& as);

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

    // per §24.5 / §30: emit the async-event raise sequence for `RaiseAsync` —
    // invoke each subscribed handler closure (which returns a Task*) in
    // subscription order, awaiting each handler's task synchronously before
    // invoking the next (per §24.5: "each handler's task is awaited before the
    // next is invoked"). If any handler's task faults, subsequent handlers are
    // not invoked and the fault is carried in the returned Task. Returns a
    // Task* that is always already-done on return (enabling the caller's
    // `await E.RaiseAsync(...)` to take the already-done fast path). The Task*
    // is single-use (bz_task_free'd by the await site's freeTaskAfter path).
    llvm::Value* emitAsyncEventRaise(llvm::Value* srcPtr, llvm::Value* payloadLL,
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

    // §17.15 / K8.5 (v0.216, #2098): the binding atom `(var x := E; body)`.
    // Pushes a dedicated dispose scope (the ATOM boundary), lowers each
    // clause via lowerLocalDecl (alloca + init + locals_ registration —
    // clause decl nodes key locals_ uniquely, so no shadow save/restore is
    // needed), registers the atom-boundary disposal for every clause the
    // ownership pass flagged (bindAtomDisposeAtEnd — an owned production
    // left undischarged in the atom; registerUsingDisposal picks the right
    // Disposal kind), lowers the body, then emits the scope's disposals on
    // the normal edge (reverse registration order per K8.5) and pops. The
    // exception edge is covered by the shared unwind machinery — the atom
    // scope sits on disposeStack_, so every emitPendingCheck inside the atom
    // disposes it during unwinding (K3.15 parity). Returns the body's value.
    llvm::Value* lowerBindExpr(const ast::BindExpr& bx);

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

    // §11.1 / #873: before emitInstanceInit runs a class's field initializers,
    // pre-populate the positional-param field slots of every base class in the
    // hierarchy so that initializers like `_field := v` (where `v` is a
    // primary-ctor parameter) read the value that was passed via the base(...)
    // chain call rather than the calloc-zeroed default.
    //
    // `t`    — the most-derived class being constructed (the `new` target)
    // `obj`  — the freshly calloc'd instance pointer (currentThis_ is set to it)
    // `st`   — LLVM struct type for `t`
    // `ne`   — the NewExpr (supplies the ctor argument list)
    //
    // For a class with a TypeDecl::primaryBaseCtorArgs, those expressions are
    // evaluated (primary params are already pre-stored into their field slots)
    // and stored into the first base class's positional-param field slots.
    // For an explicit ctor with a leading `base(...)` call, the ctor params are
    // temporarily bound to the evaluated `ne.args` values and the chain-call
    // arg expressions are evaluated and stored into the base's slots.
    // Recursive: the base class itself is also processed if needed.
    // `ctorArgsByRefAbi` is true only at the top-level `new`-expression call
    // site, where ctorArgVals[i] is the caller's storage ADDRESS for a
    // ref/out param (CG50); the recursion passes lowered VALUES. fix #2584.
    void prePopulateBasePositionalFields(
        const sema::TypeSymbol* t,
        llvm::Value* obj,
        const std::vector<llvm::Value*>& ctorArgVals,  // already-evaluated ctor args
        const sema::ConstructorSymbol* cs,             // null if no explicit ctor
        bool ctorArgsByRefAbi = false);
};

}  // namespace bezel::codegen
