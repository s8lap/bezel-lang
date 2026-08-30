// fix #3634 (§22.10 / §28.9, v0.286): the §22.10 captured-`owned`-primary-
// ctor-param arm (fix #3205) used to be spelled TWICE, character-identical —
// once as `Emitter::isCapturedOwnedPrimaryParam` (codegen,
// src/codegen/LlvmBackend_impl.hpp) and once as the `fs->posParam` arm inside
// `common::isNonTrivialDisposeClass` (src/common/OwnershipFacts.cpp). #3588
// declined to collapse the pair in passing because doing so would cross
// OwnershipFacts.hpp's own MECHANISM/EXPRESSION-SHAPE scope-boundary comment
// (lines 22-25) without arguing it, and filed the pair as #3634.
//
// THE FIX: `common::isCapturedOwnedPrimaryParam(const sema::FieldSymbol*)` is
// now the ONE body (OwnershipFacts.hpp/.cpp); `Emitter::isCapturedOwnedPrimaryParam`
// is a one-line delegating wrapper (the #2654 pattern), and
// `common::isNonTrivialDisposeClass`'s inline arm calls the same shared
// predicate instead of repeating the condition. See OwnershipFacts.hpp's
// fix-#3634 comment for the fact/mechanism argument that settles the
// boundary question: the predicate is a pure read of `fs->posParam`,
// `fs->parent->kind`, `fs->posParam->modifier`, `fs->posParamInitRef` — no
// Emitter state, no LLVM type, no disposerFns_/vtable/unwind lookup — so it
// is SHAPE, not MECHANISM, and sharing it does not cross the boundary the
// comment protects.
//
// WHY THIS MATTERS (the coupling is load-bearing, not cosmetic): #3205 is the
// measured proof. The codegen copy decides whether a compiler-generated
// `owned` capture FIELD is allocated for an unreferenced `owned` primary-ctor
// parameter (via `isOwnedReferenceField`/`capturedOwnedPrimaryParamType`,
// consumed by `emitOwnedFieldDisposals`); the common copy decides whether the
// enclosing class is classified non-trivial-dispose (consumed by the §21.9
// bare-discard reject and the §28.3 auto-drop warning gate). Both questions
// must agree — a captured slot is non-trivial-dispose EXACTLY when the
// generated `Dispose()` has that slot to walk. Before #3205 the codegen copy
// gated on `posParamBodyRef && !posParamInitRef`; the fix widened it to
// `!posParamInitRef` (capturing the "referenced nowhere" case too) to close a
// measured 16-byte-per-instance leak. A future edit to ONLY ONE of the two
// spellings reopens either that leak (codegen under-captures, common still
// says non-trivial ⇒ a real Dispose() call that skips a slot it should walk
// — no crash, but the object is still non-trivial-dispose so §21.9 keeps
// rejecting bare discard of it) or its mirror (common under-classifies ⇒ the
// class reads trivial-dispose while codegen still allocates and expects to
// walk a capture slot — a §21.9 false ACCEPT of a program that now leaks
// under bare discard). #3634 makes that drift structurally impossible: there
// is exactly one place left to edit.
//
// THIS FILE'S COVERAGE:
//   T1 — CARDINAL, LOOP-AMPLIFIED (500x): the #3205 repro (`owned` primary-
//     ctor param referenced NOWHERE) run 500 times in a loop. A one-shot
//     LSan row can pass on BOTH a correct and a broken predicate because the
//     lone leaked block can still be conservatively stack-reachable at exit
//     (see tests/README.md / the run-tracker's "one-shot LSan" note); 500
//     iterations make a broken predicate's leak (500 x 16 B = 8000 B) not
//     explicable by any reachability accident. Also confirms disposal
//     actually ran via an independent counter (`ResL._n`), not merely
//     "did not crash".
//   T2 — CONTROL, LOOP-AMPLIFIED (500x): the #1543 happy path (body-
//     referenced `owned` param) stays correct under the shared predicate —
//     guards against a fix that narrows the shared body and regresses the
//     already-working case.
//   T3 — the §21.9 bare-discard classification (driven by
//     `common::isNonTrivialDisposeClass`, the OTHER consumer of the shared
//     predicate) still rejects bare-discarding a class whose only `owned`
//     member is the unreferenced capture — pins that unifying the two
//     spellings kept BOTH consumers' verdicts in agreement, not just the
//     codegen one T1 exercises.
//
// MECHANICAL FAIL-WITHOUT / PASS-WITH (recorded here; not re-run by the test
// itself — a gtest cannot rebuild bezelc mid-run). This fix is a behaviour-
// preserving refactor (the shared body is textually the post-#3205 condition,
// unchanged), so reverting JUST this commit reproduces neither #3205's nor
// any other live bug — T1-T3 pass identically before and after. The
// mechanical proof that T1 is a real (non-vacuous) oracle, and that the
// unification is genuinely load-bearing rather than cosmetic, was done by
// hand during development: temporarily reverting the ONE shared body
// (`common::isCapturedOwnedPrimaryParam`) to the pre-#3205 condition
// (`posParam->modifier == "owned" && posParamBodyRef && !posParamInitRef`)
// and rebuilding reproduces #3205's leak in T1 (measured: LSan reports 500
// blocks / 8000 B leaked, `r.ran` flips false) while T2's body-referenced
// control stays clean (its `posParamBodyRef` is already true) — confirming
// (a) T1 actually exercises the predicate rather than passing vacuously, and
// (b) editing the ONE shared body moves BOTH consumers at once (T1's codegen
// capture-slot decision and T3's common-side trivial-dispose classification),
// which is the property #3634 exists to guarantee. Restored before commit;
// see the commit body for the exact counts.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;
using bezel::test::runAot;

namespace {

// ASan toolchain skip-guard (matches #2603/#2598/#2587/#3205's test pattern).
bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

// Disposable class whose destructor bumps a static counter (identical
// scaffolding to #1543/#1594/#2603/#3205's tests).
const char* kRes =
    "class ResL "
    "  public static _n: int := 0; "
    "  private _id: int; "
    "  public constructor(id: int) _id := id; end constructor; "
    "  public method Id(): int => _id; "
    "  destructor _n := _n + 1; end destructor; "
    "end ResL; ";

}  // namespace

// ===========================================================================
// T1 — CARDINAL, LOOP-AMPLIFIED (500x): `owned` primary-ctor param referenced
// NOWHERE, constructed and disposed 500 times via `using var` inside a loop.
// Pre-#3205 (the shared predicate's pre-widening condition): each iteration
// leaks its ResL block — 500 x 16 B = 8000 B over the run. Post-#3205 (and
// post-#3634's unification, which changes no behaviour): LSan-clean, and the
// independent counter confirms exactly 500 dispositions.
// per §22.10 (v0.254 widening / v0.180 base rule), §28.9 (v0.286)
// ===========================================================================
BEZEL_TEST(captured_owned_primary_param_shared_predicate_3634,
           unreferenced_owned_primary_param_disposed_amplified_500_spec_22_10_28_9) {
    JitResult r = runAotChecked(
        std::string("module M; import System; ") + kRes +
        "public class UnusedL3634(owned r: ResL, tag: int) "
        "end UnusedL3634; "
        "class C "
        "  public static method Main(): int "
        "    for var j in 0..<500 do "
        "      using var u := new UnusedL3634(new ResL(j), j); "
        "    end; "
        "    Console.WriteLine(ResL._n.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);                 // LSan-clean over 500 iterations
    BEZEL_EXPECT_EQ(r.stdoutText, "500\n");   // one disposal per construction
}

// ===========================================================================
// T2 — CONTROL, LOOP-AMPLIFIED (500x): the #1543 happy path (owned param
// referenced in a body position) stays correct — a regression guard against
// a fix that narrows the shared predicate and stops capturing this case.
// per §22.10 (v0.180)
// ===========================================================================
BEZEL_TEST(captured_owned_primary_param_shared_predicate_3634,
           body_referenced_owned_primary_param_unchanged_amplified_500_spec_22_10) {
    JitResult r = runAotChecked(
        std::string("module M; import System; ") + kRes +
        "public class UsedL3634(owned r: ResL, tag: int) "
        "  public method Tag(): int => tag + r.Id(); "
        "end UsedL3634; "
        "class C "
        "  public static method Main(): int "
        "    for var j in 0..<500 do "
        "      using var u := new UsedL3634(new ResL(j), j); "
        "    end; "
        "    Console.WriteLine(ResL._n.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "500\n");
}

// ===========================================================================
// T3 — the OTHER consumer of the shared predicate: `common::isNonTrivialDisposeClass`
// (via the §21.9 bare-discard check) must still reject a bare-statement
// discard of a class whose ONLY `owned` member is an unreferenced `owned`
// primary-ctor param — pinning that the unification kept the codegen
// capture-slot decision (T1) and the common-side trivial-dispose
// classification in agreement, not just one of the two.
// per §21.9 (v0.286), §22.10
// ===========================================================================
BEZEL_TEST(captured_owned_primary_param_shared_predicate_3634,
           bare_discard_of_unused_owned_only_param_rejected_spec_21_9) {
    auto r = runAot(
        std::string("module M; ") + kRes +
        "public class UnusedOnly3634(owned r: ResL, tag: int) "
        "end UnusedOnly3634; "
        "class C public static method Main(): int "
        "  new UnusedOnly3634(new ResL(1), 7); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(r.ran);
}
