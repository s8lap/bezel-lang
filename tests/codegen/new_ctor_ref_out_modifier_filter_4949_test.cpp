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
// This test deliberately does NOT also call `new J(ref n, ...)` here. Per
// the issue itself, an explicit call-site `ref`/`out` modifier at a `new`
// site is NOT resolved by `ExprTC::checkNewExprArgs` at all (the loop this
// fix adds is guarded by the pre-existing "any arg carries a modifier ->
// return" early-out just above it) — it is left entirely to codegen's
// `ctorForArgs` fallback (LlvmBackend_impl.hpp), which the #4924 fix filtered
// on `ref`/`out` only for `chainCall`, NOT for `new` sites, precisely
// because doing so flips the pinned row
// `rec_ref_ctor_4805.combined_form_literal_arg_rejects_clean_22_3` and turns
// on the open §9.3 "same parameter list" ruling (#4775 Q1). MEASURED with
// this fix: `ctorForArgs`'s un-fixed fallback is order-dependent for a
// modifier-bearing `new` call — with the `ref` ctor declared SECOND,
// `new J(ref n, 6)` (this same class shape) prints `V6,3`, not `R6,3`. That
// is the pre-existing, explicitly-deferred codegen gap the issue describes,
// not a regression from this fix, so it is intentionally left unexercised
// here rather than pinned with a weakened expectation.
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
        "    return 0; "
        "  end Main; "
        "end P;",
        "V5,3\n");
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
