// Regression tests for issue #4949 — sema's `new`-expression constructor
// overload resolution ignored the call-site `ref`/`out` modifier: a
// modifier-free call could bind a candidate whose FORMAL declares `ref`/`out`,
// because `resolveOverload`/`classifyArg` (ExprTypecheck_Calls.cpp) judge
// Exact-vs-Convert on TYPE ALONE and have no notion of parameter modifiers.
// This is the `new`-site twin of #4924 (which fixed the same gap for
// `this(...)`/`base(...)` chain calls) — same root cause, different call
// form.
//
// spec §22.6 "Resolution pipeline" step 3 ("Applicability over the
// (substituted) signatures: parameter types under the implicit conversions
// and the ref/out modifiers") and §22.3 "Parameter modifiers" (the caller
// writes `ref`/`out` at the call site, and only for a `ref`/`out`
// parameter), v0.307.
//
// Pre-fix (MEASURED on origin/main e3b0bdf0): `new J(n, 5)` against
// `constructor(ref y: int, x: int)` / `constructor(y: long, x: int)` saw
// int→int Exact for the `ref` candidate (its first formal happens to be
// `int`, matching the plain `int` argument on TYPE) beating int→long
// Convert for the value candidate, and picked the `ref` overload — silent
// wrong code, since §22.3 makes a `ref` formal inapplicable to a plain
// argument regardless of type. Fixed by filtering candidates on
// `ref`/`out` applicability (the same `refOutMatch` predicate the
// single-candidate arm already applies) before §22.6 step 5's exact-match
// ranking runs.
//
// fix #5081 (§22.3 / §22.6, v0.307), the remaining half of #4949: a `new`
// call whose OWN argument carries an explicit `ref`/`out` call-site
// modifier bypasses `ExprTC::checkNewExprArgs` entirely (its early-return
// guard, quoted in the test below) and was left to codegen's `ctorForArgs`
// proxy tier, whose arity>1 arm ranked candidates by TYPE alone and thus by
// declaration order — `new J(ref n, 6)` printed `R6,3` when the `ref` ctor
// happened to be declared first (by coincidence of iteration order) and
// `V6,3` when it was declared second. Fixed by reusing the same
// `sema::callSiteRefOutMatches` / `sema::formalParamModifier` predicate
// #4924 already applies for a chain call, scoped to the arity>1 arm only
// (`enforceRefOutAtNewSite` in `LlvmBackend_impl.hpp`) — the arity==1 /
// §22.10-primary interaction (#4805) is untouched, since it turns on the
// still-open §9.3 ruling #4775 Q1.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::runAot;

namespace {

void expectPrints(const std::string& src, const std::string& expected) {
    auto r = runAot(src);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

}  // namespace

// per §22.3 / §22.6 (v0.307), fix #4949 — the issue's exact repro: a plain
// `new J(n, 5)` must skip the `ref y` overload and call `(y: long, x: int)`
// (prints `V5,3`); the explicit `new J(ref n, 6)` spelling must still select
// the `ref` overload (prints `R6,3`).
//
// fix #5081: with the `ref` ctor declared FIRST, `new J(ref n, 6)` printed
// `R6,3` even pre-#5081 — but only because `ctorForArgs`'s unfixed arity>1
// arm iterates `cls->members` in declaration order and this happened to be
// the first type-match, not because it was ref/out-aware (see the sibling
// test below, which declares the SAME two ctors in the OPPOSITE order and
// pre-#5081 printed `V6,3` for the identical call). #5081 makes the pick
// correct for the actual reason in both orders.
BEZEL_TEST(new_ctor_ref_out_modifier_filter_4949, new_site_plain_call_skips_ref_overload_issue_repro) {
    expectPrints(
        "module M; "
        "class J "
        "  public readonly property Tag: string; "
        "  public constructor(ref y: int, x: int) Tag := $\"R{x},{y}\"; end constructor; "
        "  public constructor(y: long, x: int) Tag := $\"V{x},{y}\"; end constructor; "
        "end J; "
        "class P "
        "  public static method Main(): int "
        "    var n: int := 3; "
        "    using var j := new J(n, 5); Console.WriteLine(j.Tag); "
        "    using var k := new J(ref n, 6); Console.WriteLine(k.Tag); "
        "    return 0; "
        "  end Main; "
        "end P;",
        "V5,3\nR6,3\n");
}

// per §22.3 / §22.6 (v0.307), fix #4949 — the fix is applicability-based, not
// declaration-order-based: with the value overload declared FIRST and the
// `ref` overload SECOND, `new J(n, 5)` (no call-site modifier) must still
// call the value overload (prints `V5,3`).
//
// fix #5081 (§22.3 / §22.6, v0.307): this test used to stop at the plain
// call above, because an explicit call-site `ref`/`out` modifier at a `new`
// site is NOT resolved by `ExprTC::checkNewExprArgs` at all (the loop the
// #4949 fix adds is guarded by the pre-existing "any arg carries a
// modifier -> return" early-out just above it) — it was left entirely to
// codegen's `ctorForArgs` fallback (LlvmBackend_impl.hpp), whose arity>1 arm
// ranked by TYPE alone and thus by declaration order: MEASURED pre-#5081,
// with the `ref` ctor declared SECOND (this class shape), `new J(ref n, 6)`
// printed `V6,3`, not `R6,3` — the sibling test above has the identical two
// ctors in the OPPOSITE order and printed the correct `R6,3`, proving the
// pick was order-dependent, not ref/out-aware. #5081 fixes `ctorForArgs`'s
// arity>1 arm to apply the same `sema::callSiteRefOutMatches` /
// `sema::formalParamModifier` predicate #4924 uses for a chain call, so
// this now also asserts `new J(ref n, 6)` prints `R6,3` regardless of
// declaration order.
//
// This does NOT touch #4805's pinned row
// (`rec_ref_ctor_4805.combined_form_literal_arg_rejects_clean_22_3`): that
// is the arity==1 / §22.10-primary-vs-explicit-ctor interaction, which turns
// on the still-open §9.3 "same parameter list" ruling (#4775 Q1) and is
// deliberately out of scope for #5081.
BEZEL_TEST(new_ctor_ref_out_modifier_filter_4949, new_site_plain_call_skips_ref_overload_declared_second) {
    expectPrints(
        "module M; "
        "class J "
        "  public readonly property Tag: string; "
        "  public constructor(y: long, x: int) Tag := $\"V{x},{y}\"; end constructor; "
        "  public constructor(ref y: int, x: int) Tag := $\"R{x},{y}\"; end constructor; "
        "end J; "
        "class P "
        "  public static method Main(): int "
        "    var n: int := 3; "
        "    using var j := new J(n, 5); Console.WriteLine(j.Tag); "
        "    using var k := new J(ref n, 6); Console.WriteLine(k.Tag); "
        "    return 0; "
        "  end Main; "
        "end P;",
        "V5,3\nR6,3\n");
}

// per §22.3 / §22.6 (v0.307), fix #4949 — CONTROL (passes pre- and post-fix
// through the SAME code path, `resolveOverload`'s type ranking): neither
// candidate declares `ref`/`out`, so the new modifier filter admits both
// unchanged and §22.6's Exact-beats-Convert tie-break still picks the exact
// `int` overload over the `long`-widening one.
BEZEL_TEST(new_ctor_ref_out_modifier_filter_4949, control_no_ref_out_candidates_unaffected) {
    expectPrints(
        "module M; "
        "class J "
        "  public readonly property Tag: string; "
        "  public constructor(y: int, x: int) Tag := $\"I{x},{y}\"; end constructor; "
        "  public constructor(y: long, x: int) Tag := $\"L{x},{y}\"; end constructor; "
        "end J; "
        "class P "
        "  public static method Main(): int "
        "    using var j := new J(5, 3); Console.WriteLine(j.Tag); "
        "    return 0; "
        "  end Main; "
        "end P;",
        "I3,5\n");
}

// per §22.3 / §22.6 (v0.307), fix #5081: two constructors that differ ONLY
// in the `ref` modifier (identical parameter TYPES otherwise), `ref` ctor
// declared FIRST. `new K(ref n, 6)` must select the `ref` overload: its body
// mutates the aliased `n` (`y := y + 1`), so both the printed `Tag` AND the
// caller's `n` afterward prove which ctor actually ran — the wrong
// (non-`ref`) overload would print `P6,10` and leave `n` at `10`, not
// `R6,11` / `11`.
BEZEL_TEST(new_ctor_ref_out_modifier_filter_4949, new_site_ref_modifier_only_difference_declared_first) {
    expectPrints(
        "module M; "
        "class K "
        "  public readonly property Tag: string; "
        "  public constructor(ref y: int, x: int) y := y + 1; Tag := $\"R{x},{y}\"; end constructor; "
        "  public constructor(y: int, x: int) Tag := $\"P{x},{y}\"; end constructor; "
        "end K; "
        "class P "
        "  public static method Main(): int "
        "    var n: int := 10; "
        "    using var k := new K(ref n, 6); Console.WriteLine(k.Tag); "
        "    Console.WriteLine(n); "
        "    return 0; "
        "  end Main; "
        "end P;",
        "R6,11\n11\n");
}

// per §22.3 / §22.6 (v0.307), fix #5081: same shape as above, ref/out-only
// difference, but the NON-`ref` overload is declared FIRST and the `ref`
// overload SECOND — proving the pick is applicability-based, not
// declaration-order-based, exactly like the `new_site_plain_call_skips_*`
// pair above but with a pure ref/out-only distinguishing candidate pair.
BEZEL_TEST(new_ctor_ref_out_modifier_filter_4949, new_site_ref_modifier_only_difference_declared_second) {
    expectPrints(
        "module M; "
        "class K "
        "  public readonly property Tag: string; "
        "  public constructor(y: int, x: int) Tag := $\"P{x},{y}\"; end constructor; "
        "  public constructor(ref y: int, x: int) y := y + 1; Tag := $\"R{x},{y}\"; end constructor; "
        "end K; "
        "class P "
        "  public static method Main(): int "
        "    var n: int := 10; "
        "    using var k := new K(ref n, 6); Console.WriteLine(k.Tag); "
        "    Console.WriteLine(n); "
        "    return 0; "
        "  end Main; "
        "end P;",
        "R6,11\n11\n");
}
