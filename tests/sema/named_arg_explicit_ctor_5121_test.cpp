// Regression tests for issue #5121 — §22.5 "Optional parameters and named
// arguments" / §22.7 / §22.10 "primary constructors" (spec v0.307):
// sema's `checkNewExprArgs` short-circuit at lines 17229-17262 deferred
// LOCAL combined-form classes (primary + explicit ctor) to codegen without
// running the §22.5 binder. Invalid named-argument programs (Probes A/B/C
// from #5107 review) silently COMPILED instead of being rejected per §22.5:
//
//   Probe A — duplicate named arg: `new C(x := 5, x := 7)` on
//             `class C(x: int)` + explicit `(y: int) this(y)` must reject
//             (`§22.5 "no parameter may be bound twice"`).
//   Probe B — unknown name:       `new C(x := 5, z := 7)` on the same class
//             must reject (§22.5: `z` is not a primary parameter).
//   Probe C — missing required:   `new C(x := 5)` on
//             `class C(x: int, y: int)` + explicit `(z: int) this(z)` must
//             reject (§22.5: `y` is required and not bound).
//
// ── ROOT CAUSE ────────────────────────────────────────────────────────────
//
// ExprTC::checkNewExprArgs (src/sema/ExprTypecheck_Calls.cpp) had an
// `if (hasExplicitCtor && !ctx_.isImported(t)) { ...; return; }` arm that
// ran `reportPrimaryBodyCtorAmbiguity` (which uses `bindArgsToParams` with
// `emit=false`) and returned BEFORE sema could publish a §22.5
// diagnostic. Codegen's #5107 fix (LlvmBackend_Exprs2.cpp around line
// 4761) added a defence-in-depth binder at `storePrimaryCtorPositionalArgs`
// for the NAMED path, but the SEMANTIC reject was still missing — invalid
// programs reached codegen at all.
//
// ── THE FIX ────────────────────────────────────────────────────────────────
//
// Inside the same `if (hasExplicitCtor && !ctx_.isImported(t))` arm, run
// `bindArgsToParams` (with `emit=false`) over the primary's
// `positionalFields` AND over every non-static `ConstructorDecl`'s params;
// if NONE of those bind, re-run the primary binder with `emit=true` to
// surface the §22.5 diagnostic. Named args that bind to the explicit
// ctor's params (T4 `new C(y := 5)` where `y` is the explicit ctor's
// param) are not spuriously rejected because at least one candidate
// binds. Positional args (T5) bind the primary or the explicit ctor
// normally.
//
// The §22.6 stage 8 ambiguity check (fix #5099) still runs after via
// `reportPrimaryBodyCtorAmbiguity`. The codegen #5107 binding remains as
// defence-in-depth for the named-arg-binding side-table this short-circuit
// never publishes.
//
// ── ORACLE NOTES ──────────────────────────────────────────────────────────
//
// T1/T2/T3 use `failedAtCompileTimeWithRealLocation` plus
// `diagnosticMessageContains` on the §22.5 wording to distinguish a genuine
// sema-side reject from a runtime crash / signal / ASan report
// (CLAUDE.md: "vacuous-oracle traps — `failedAtCompileTimeWithRealLocation`
// on a crash asserts the specific diagnostic"). T4/T5 are runAot-based
// exit-code / stdout-oracle rows that verify the fix doesn't over-reject.

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation, diagnosticMessageContains
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

// A class with a primary `(x: int)` parameter and an explicit
// `(y: int) this(y)` body constructor — exactly the shape that triggers
// the `if (hasExplicitCtor && !ctx_.isImported(t))` sema arm in
// `checkNewExprArgs`. The body ctor chains to the primary.
std::string scopedToPrimary(const std::string& callArgs) {
    return "module M; "
           "class C(x: int) "
           "  public constructor(y: int) this(y); end constructor; "
           "  public property X: int get => x; end; "
           "end C; "
           "class P public static method Main(): int "
           "  using var c := new C(" + callArgs + "); "
           "  return c.X; "
           "end Main; end P;";
}

// Same shape, primary `(x: int, y: int)` so a missing `y` is a §22.5
// reject. The explicit ctor's `this(z, 0);` chain provides BOTH
// primary params (so the chain does NOT itself report a §22.5 missing-
// required error — that would otherwise match the call site's diagnostic
// vacuously and let the pre-fix tree pass without surfacing the actual
// call-site reject). The chain shape `this(z, 0)` makes the
// `this(...)` §22.5 check at the chain site pass cleanly; the only §22.5
// diagnostic the program can carry is the call-site missing-required
// one the fix must emit.
std::string primaryWithRequiredY(const std::string& callArgs) {
    return "module M; "
           "class C(x: int, y: int) "
           "  public constructor(z: int) this(z, 0); end constructor; "
           "  public property X: int get => x; end; "
           "end C; "
           "class P public static method Main(): int "
           "  using var c := new C(" + callArgs + "); "
           "  return c.X; "
           "end Main; end P;";
}

}  // namespace

// ===========================================================================
// T1 — §22.5: duplicate named arg on a combined-form class is rejected.
//
// `new C(x := 5, x := 7)` binds `x` twice (both to the primary's `x`).
// Pre-fix, sema short-circuited without emitting the §22.5 diagnostic;
// the program reached codegen where #5107's defence-in-depth binder
// rejected it, but the SEMANTIC reject at sema was missing. Post-fix,
// sema emits `parameter 'x' is bound more than once (§22.5)`.
// ===========================================================================
// per §22.5, fix #5121
BEZEL_TEST(named_arg_explicit_ctor_5121,
           duplicate_named_arg_is_rejected) {
    auto r = runAot(scopedToPrimary("x := 5, x := 7"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "parameter `x` is bound more than once (§22.5)"));
}

// ===========================================================================
// T2 — §22.5: unknown named arg on a combined-form class is rejected.
//
// `new C(x := 5, z := 7)` names `z`, which is neither a primary
// parameter nor a body-ctor parameter. Pre-fix, no diagnostic; post-fix,
// sema emits ``new C` has no parameter named `z` (§22.5)``.
// ===========================================================================
// per §22.5, fix #5121
BEZEL_TEST(named_arg_explicit_ctor_5121,
           unknown_named_arg_is_rejected) {
    auto r = runAot(scopedToPrimary("x := 5, z := 7"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "has no parameter named `z` (§22.5)"));
}

// ===========================================================================
// T3 — §22.5: missing required named arg is rejected.
//
// `new C(x := 5)` on a class with primary `(x: int, y: int)`. The
// call binds `x` but leaves `y` required and unbound; the explicit ctor
// `(z: int) this(z)` accepts only `z` (and `x` is unknown to it). NEITHER
// candidate binds the call (the primary fails on missing `y`, the
// explicit ctor fails on `x` being unknown), so the fix re-emits the
// §22.5 diagnostic on the primary: ``new C`: required parameter `y`
// is not supplied (§22.5)``.
// ===========================================================================
// per §22.5, fix #5121
BEZEL_TEST(named_arg_explicit_ctor_5121,
           missing_required_named_arg_is_rejected) {
    auto r = runAot(primaryWithRequiredY("x := 5"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "required parameter `y` is not supplied (§22.5)"));
}

// ===========================================================================
// T4 — control: explicit ctor + valid named arg (name binds the body
// ctor's parameter, NOT the primary's) is accepted.
//
// `new C(y := 5)` on `class C(x: int, z: int) + constructor(y: int)
// this(y, 0)`: only the explicit ctor's `y` matches the named arg.
// The fix's "if NONE bind, emit" gate does NOT trip because the explicit
// ctor's binder succeeds, so the call defers cleanly to codegen which
// runs #5107's `storePrimaryCtorPositionalArgs` and the explicit ctor
// chains `this(5, 0)` so `x := 5`. This is the exact regression that the
// naive "just emit on primary" fix would break.
// ===========================================================================
// per §22.5 / §22.6 / §9.3, fix #5121
BEZEL_TEST(named_arg_explicit_ctor_5121,
           named_arg_targeting_explicit_ctor_is_accepted) {
    auto r = runAot(
        "module M; "
        "class C(x: int, z: int) "
        "  public constructor(y: int) this(y, 0); end constructor; "
        "  public property X: int get => x; end; "
        "end C; "
        "class P public static method Main(): int "
        "  using var c := new C(y := 5); "
        "  return c.X; "
        "end Main; end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// ===========================================================================
// T5 — control: explicit ctor + positional args (the existing
// `new C(7)` shape from the #5107 family) is accepted.
//
// `new C(7)` on `class C(x: int, z: int) + constructor(y: int)
// this(y, 0)`: the primary has arity 2 and `z` is required, so the
// primary's binder fails on the 1-arg call. The explicit ctor's binder
// accepts (1 arg, `y`), so the fix's "any binds" gate succeeds and the
// call defers cleanly. Codegen matches the explicit ctor and chains
// `this(7, 0)` so `x := 7`. This pins that the fix did not over-reject
// the positional path #5107 was designed for.
//
// Note: a 1-arg call against a 1-arg primary AND a 1-arg body ctor is
// pre-existing §22.6 ambiguous (both bind identically — measure at
// 487bbd92 / pre-fix HEAD), so this control must use a primary arity
// LARGER than the explicit ctor's to drive the "only the explicit binds"
// shape #5107's tests rely on.
// ===========================================================================
// per §22.5 / §22.6 / §9.3, fix #5121
BEZEL_TEST(named_arg_explicit_ctor_5121,
           positional_arg_to_explicit_ctor_is_accepted) {
    auto r = runAot(
        "module M; "
        "class C(x: int, z: int) "
        "  public constructor(y: int) this(y, 0); end constructor; "
        "  public property X: int get => x; end; "
        "end C; "
        "class P public static method Main(): int "
        "  using var c := new C(7); "
        "  return c.X; "
        "end Main; end P;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}