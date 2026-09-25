// Spec-derived regression test for #4939 — the §22.6 over-rejection of
// an ordinary-`owned` / plain pair on a `where T: IDisposable` generic
// type parameter.
//
// THE RULE. Spec §22.6 (v0.250 / v0.307, proposal 0091 §1, graduated
// v0.250): "A pair differing only by ordinary `owned` is declarable
// exactly when, at every differing position, the declared type —
// nullability stripped — is a class type, an interface type, or a type
// parameter whose constraints exclude every substitution at which the
// pair collapses." `where T: IDisposable` qualifies because the §8.7
// IDisposable-closure guarantees a class-only conforming set.
//
// THE DEFECT. The overload-distinguishability gate (the
// `signaturesIndistinguishable` predicate in
// `src/sema/TypeResolution_Checks.cpp`) rejected every owned/plain pair
// where the parameter type resolved to a TypeRef with no TypeSymbol at
// overload-signature-check time. For a `where T: IDisposable` bound the
// constraint-bound TypeRef's `declaration` is null (constraint resolution
// is a separate late pass, #3146) and `ctx_.builtinInterfaceSym` mints no
// TypeSymbol for `IDisposable` (it lives in the `nonSymbolBaseRefs`
// source — SymbolCollection.cpp:955). The check fell through to a
// conservative `false`, the pair collapsed, the pair was rejected — and
// the corpus row `tests/conformance/rules/cases/s22_6_0018.bzl`
// (`expect: accept`) stayed fail-of-text-pass-of-verdict under the §22.6
// guard.
//
// THE FIX (per §22.6 / proposal 0091 §1). Two narrow extensions to the
// declaration guard, both conservatively safe:
//
//   1. `positionTypePassesDeclarationGuard`: when a Named TypeRef's
//      declaration is null AND the name matches `IDisposable`, return
//      `true` — the §8.7 closure makes the conforming set class-only AS
//      A MATTER OF LANGUAGE RULE. (Other names still fall back to
//      `builtinInterfaceSym`, which only resolves for the synthesized
//      built-in set, not for `IDisposable`.)
//
//   2. `typeParamConstraintSetQualifies`: when the resolved TypeRef for
//      a `where T: X` bound is null (constraint resolution hasn't run
//      yet) and the AST `astBound->name->parts.back()` is `IDisposable`,
//      treat the bound as qualifying. Other names fall through and
//      reject the constraint set (the safe direction).
//
// The pre-fix behaviour was a corpus row at `tests/conformance/rules/
// cases/s22_6_0018.bzl` `fail divergence`. The post-fix behaviour is
// `pass` (the pair declares, no diagnostic, no compile error). The
// neighbour rows `s22_6_0015` (`out owned` clash), `s22_6_0016`
// (`scoped` clash), and `s22_6_0017` (unconstrained `T` / `where T:
// class` erasure guard) MUST still reject — they pin the un-lifted
// halves of proposal 0091 §1 and §3.
//
// FAIL-WITHOUT (verified mechanically against the pre-fix state — see
// the conversation that landed this fix): the §22.6 declaration guard
// returns `false` for the `where T: IDisposable` pair, the pair
// collapses, and the corpus row reads as `fail divergence` — same as
// pre-fix. FAIL-WITH: the declaration guard returns `true`, the pair
// declares, no diagnostic, no compile error, and the corpus row reads
// as `pass`.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::JitResult;
using bezel::test::runAot;

// =========================================================================
// §22.6 — the corpus cluster row s22_6_0018's exact repro: an `owned`/plain
// pair on a `where T: IDisposable` parameter passes the declaration guard
// (the §8.7 closure guarantees a class-only conforming set; proposal 0091
// §1 worked example). The pre-fix state REJECTED this pair; the post-fix
// state ACCEPTS it. Verified both ways: against the pre-fix base and the
// post-fix base.
// =========================================================================

// §22.6 / proposal 0091 §1 / §8.7 (issue #4939): the `where T:
// IDisposable` ordinary-`owned`/plain pair is DECLARABLE. The §22.6
// declaration guard admits the class-only-interface bound; the pair is
// distinguishable at every differing position; the declaration compiles
// without a §22.6/§11.3 diagnostic. The `runAot` AOT path here exercises
// declaration-site overload checking — the post-fix verdict is "compiles
// clean".
BEZEL_TEST(s22_6_disposable_constraint_pair_distinguishable_4939,
           where_t_idisposable_owned_plain_pair_admitted_spec_22_6_8_7) {
    auto r = runAot(
        "module M;\n"
        "public class Box<T> where T: IDisposable\n"
        "  public method Put(item: T)\n"
        "  end Put;\n"
        "  public method Put(owned item: T)\n"
        "  end Put;\n"
        "end Box;\n"
        "class C\n"
        "  public static method Main(): int return 0; end Main;\n"
        "end C;\n");
    // The §22.6/§11.3 "indistinguishable" diagnostic must NOT fire — the
    // pair declares.
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "indistinguishable"));
    // The §22.6/§11.3 diagnostic (when fired) names the "collapses" rule
    // for failing positions; this row's position PASSES the guard, so no
    // collapse wording.
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "collapses"));
}

// =========================================================================
// Companion (same-fix coverage, §22.6 un-lifted halves): the standing
// rejects must NOT have flipped. `scoped` (proposal 0091 §3), `out
// owned` (proposal 0091 §3), and the erasure-guard failures
// (`where T: class`, unconstrained `T`) keep their "indistinguishable"
// verdicts. If any of these turned ACCEPT with the fix, the guard has
// over-granted the lift — the safe direction would have flipped.
// =========================================================================

// §22.6 / proposal 0091 §3 (issue #4939 reviewer-side negative control):
// `scoped`/plain on a function type remains an over-rejection-free
// indistinguishable clash (the un-lifted half — §28.14 makes the
// `scoped` call-site form markerless, so no resolution rule can apply).
BEZEL_TEST(s22_6_disposable_constraint_pair_distinguishable_4939,
           scoped_plain_pair_still_rejected_spec_22_6_28_14) {
    auto r = runAot(
        "module M;\n"
        "class Sub\n"
        "  public method Register(cb: () -> ())\n"
        "  end Register;\n"
        "  public method Register(scoped cb: () -> ())\n"
        "  end Register;\n"
        "end Sub;\n");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "indistinguishable"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "scoped"));
}

// §22.6 / proposal 0091 §3 (issue #4939 reviewer-side negative control):
// `out owned`/plain clash stays a compile error (the un-lifted half).
BEZEL_TEST(s22_6_disposable_constraint_pair_distinguishable_4939,
           out_owned_plain_pair_still_rejected_spec_22_6) {
    auto r = runAot(
        "module M;\n"
        "public class Conn\n"
        "  public constructor() end constructor;\n"
        "end Conn;\n"
        "public class Pool\n"
        "  public method TryGet(name: string, out c: Conn?): bool\n"
        "    c := null;\n"
        "    return false;\n"
        "  end TryGet;\n"
        "  public method TryGet(name: string, out owned c: Conn?): bool\n"
        "    c := null;\n"
        "    return false;\n"
        "  end TryGet;\n"
        "end Pool;\n");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "indistinguishable"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "owned"));
}

// §22.6 / proposal 0091 §1 erasure guard (issue #4939 reviewer-side
// negative control): the `where T: class` constraint does NOT qualify
// (admits `string` and function-type substitutions, §14.2; §28.1 erases
// the marker at those substitutions and the pair collapses). The pair
// still clashes.
BEZEL_TEST(s22_6_disposable_constraint_pair_distinguishable_4939,
           where_t_class_owned_plain_pair_still_rejected_spec_22_6_14_2) {
    auto r = runAot(
        "module M;\n"
        "public class Box<T> where T: class\n"
        "  public method Put(item: T)\n"
        "  end Put;\n"
        "  public method Put(owned item: T)\n"
        "  end Put;\n"
        "end Box;\n");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "indistinguishable"));
    // The `where T: class` failure path names the "collapses" wording
    // (corpus row `s22_6_0017` `//! diagnostic: collapses`).
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "collapses"));
}

// §22.6 / proposal 0091 §1 erasure guard (issue #4939 reviewer-side
// negative control): an UNCONSTRAINED type parameter does NOT qualify
// (every substitution is admissible). The pair still clashes. This is
// the partner of the #1873 reject rows (proposal 0091 §5 calls them
// out as standing rejects).
BEZEL_TEST(s22_6_disposable_constraint_pair_distinguishable_4939,
           unconstrained_t_owned_plain_pair_still_rejected_spec_22_6) {
    auto r = runAot(
        "module M;\n"
        "class Box<T>\n"
        "  public method Store(x: T) _ := 1; end Store;\n"
        "  public method Store(owned x: T) _ := 2; end Store;\n"
        "end Box;\n");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "indistinguishable"));
}

// =========================================================================
// Companion (same-fix coverage, §22.6 lift accepts): the concrete-class
// `Put(t: Widget)` / `Put(owned t: Widget)` pair on a non-generic class
// — proposal 0091 §5's "one genuine flip" row, repurposed here as a
// gtest-level pinning of the lift.
// =========================================================================

// §22.6 / proposal 0091 §5 (issue #4939): a concrete-class
// ordinary-`owned`/plain pair is DECLARABLE. The position is a class
// type, which always qualifies under §22.6's erasure-test declaration
// guard. The previously-reject lit row `tests/lit/sema/sema_overload__
// owned_only_difference_diagnoses.bzl` was the family exemplar of the
// ban; it now reads as ACCEPT.
BEZEL_TEST(s22_6_disposable_constraint_pair_distinguishable_4939,
           concrete_class_owned_plain_pair_admitted_spec_22_6) {
    auto r = runAot(
        "module M;\n"
        "class Widget\n"
        "  public Value: int := 7;\n"
        "end Widget;\n"
        "class Registry\n"
        "  public method Put(item: Widget)\n"
        "  end Put;\n"
        "  public method Put(owned item: Widget)\n"
        "  end Put;\n"
        "end Registry;\n"
        "class C\n"
        "  public static method Main(): int return 0; end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "indistinguishable"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "collapses"));
}
