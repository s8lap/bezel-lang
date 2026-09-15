// Spec-derived regression tests for issue #4424 — residual false-reject for
// CallExpr RHS rebind after consume. Sibling of #4225/#4250; #4250 trusted only
// `NewExpr` to close `recollectAliasBorrowSourceOnReassign`'s window, leaving a
// CallExpr RHS (e.g. `y := Factory.Make();`) to fall through to the fail-closed
// default — a window that the real, live `borrowSourceForExpr` forward walk
// already retired for every CallExpr (the function has NO CallExpr arm at all,
// only IdentifierExpr / MemberAccessExpr / NullCondMemberExpr / IndexExpr /
// UnaryExpr-await / NewExpr / LambdaExpr — see Ownership_Checks.cpp ~5582), so
// the pre-pass's "fail closed by default" divergence with the forward walk
// produces exactly the same false-reject #4225 closed for `NewExpr` RHS.
//
// Fix: extend `recollectAliasBorrowSourceOnReassign` with a `CallExpr` arm
// calling the same `closeOpenWindows()` the `NewExpr` arm already calls
// (Ownership_Checks.cpp ~10111). The soundness argument is the same as
// NewExpr's, and stronger — `borrowSourceForExpr` never returns a source for a
// CallExpr, so the window would have to be closed anyway to keep the pre-pass
// honest with the real dataflow. The receiver is irrelevant (a `Factory.Make()`
// static call and an `x.Clone()` instance call both fail the pre-pass's source
// query identically).
//
// Like the #4225/#4250 tests, real-execution rows are ASan-run (`runAotChecked`,
// the full AOT pipeline) so the FAIL-WITHOUT symptom of "compiles rc=0,
// freezes" is exercised through the same pipeline the `--full` ASan gate uses.
//
// per §28.3, §28.7, §30.14, v0.180, #4225, #4250, #4424

#include <string>

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation, diagnosticMessageContains
#include "support/aot_support.hpp"         // runAotChecked
#include "support/Diagnostic.hpp"
#include "support/ownership_support.hpp"   // parseAndCheckOwnership, hasErrorMessage
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::hasErrorMessage;
using bezel::test::JitResult;
using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAotChecked;

namespace {

const char* kFreezeWording =
    "cannot consume `x` here: the borrow held by `y` is still live — its "
    "later use would read freed memory (consume-site freeze, "
    "§28.3/§28.7/§30.14 v0.180)";

// Res + Factory: `Factory.Make()` returns a FRESH `owned Res` — the canonical
// CallExpr RHS that must close the alias window without re-opening it (no
// identifier chain to follow). // per §28.3, #4424
const char* kResAndFactory =
    "module M; "
    "class Res "
    "  public tag: int; "
    "  public constructor(t: int) this.tag := t; end constructor; "
    "  destructor end destructor; "
    "end Res; "
    "class Factory "
    "  public static method Make(): owned Res "
    "    return new Res(42); "
    "  end Make; "
    "end Factory; ";

// Res with a STATIC factory method taking an argument — exercises a CallExpr
// RHS with both a non-zero argument list (the most common static-factory
// shape) and a value-typed argument (an `int`), neither of which changes the
// pre-pass classification. // per §28.3, #4424
const char* kResWithArgFactory =
    "module M; "
    "class Res "
    "  public tag: int; "
    "  public constructor(t: int) this.tag := t; end constructor; "
    "  destructor end destructor; "
    "end Res; "
    "class Factory "
    "  public static method Make(v: int): owned Res "
    "    return new Res(v); "
    "  end Make; "
    "end Factory; ";

}  // namespace

// ===========================================================================
// Method scope — the issue's own shape (a static factory call).
// ===========================================================================

// Row 1 (diagnostic-only): `y` borrows `x`, `x` is reassigned (consuming the
// original), then `y` is rebound to `Factory.Make()` (a CallExpr RHS) BEFORE
// `y` is ever touched again — must NOT false-reject. FAIL-WITHOUT this fix
// (measured on r18-c7 HEAD 7c735181): rejected with kFreezeWording at the
// `x := new Res(9);` line. PASS-WITH: no errors. // per §28.3, #4424
BEZEL_TEST(reassign_call_expr_freeze_4424,
           method_static_factory_reassign_after_consume_accepted_4424) {
    std::string src =
        std::string(kResAndFactory) +
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(5); "
        "    var y := x; "
        "    x := new Res(9); "
        "    y := Factory.Make(); "
        "    var t := y.tag; "
        "    y.Dispose(); "
        "    x.Dispose(); "
        "    return t; "
        "  end Main; "
        "end C;";
    OwnershipResult r = parseAndCheckOwnership(src);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Row 2 (real execution, ASan-clean): same shape as row 1, executed through
// `runAotChecked` so the full AOT pipeline (compile + link + native exec +
// ASan) confirms no leak / UAF. FAIL-WITHOUT this fix (measured on r18-c7
// HEAD 7c735181): rejected with kFreezeWording at `x := new Res(9);`.
// PASS-WITH: compiles, runs, ASan-clean, returns `Factory.Make().tag` = 42.
// // per §28.3, #4424
BEZEL_TEST(reassign_call_expr_freeze_4424,
           method_static_factory_reassign_after_consume_runs_4424) {
    std::string src =
        std::string(kResAndFactory) +
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(5); "
        "    var y := x; "
        "    x := new Res(9); "
        "    y := Factory.Make(); "
        "    var t := y.tag; "
        "    y.Dispose(); "
        "    x.Dispose(); "
        "    return t; "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// Row 3 (negative control, real execution): the SAME wording still fires with
// the exact §28.3/§28.7/§30.14 text and a real diagnostic location when `y`
// is never rebound at all — the baseline true-positive this fix must not
// weaken. Pairs failedAtCompileTimeWithRealLocation with message text (a bare
// compile-time-failure check is not enough for a shape whose pre-fix history
// in this family included crash-routed diagnostics, lane rule §18). // per
// §28.3, #4424
BEZEL_TEST(reassign_call_expr_freeze_4424,
           method_static_factory_no_reassign_still_rejected_control_4424) {
    std::string src =
        std::string(kResAndFactory) +
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(5); "
        "    var y := x; "
        "    x := new Res(9); "
        "    y.Dispose(); "
        "    x.Dispose(); "
        "    return 0; "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kFreezeWording));
}

// Row 4 (real execution, static factory with arg — `Factory.Make(99)`): a
// CallExpr RHS with a non-zero argument list (the most common static-factory
// shape) and a value-typed `int` argument. Identical ownership classification
// to row 2's zero-arg twin, but exercises that the pre-pass close is
// structural on the CallExpr node — not on the arg count or arg-type shape.
// FAIL-WITHOUT this fix (measured on r18-c7 HEAD 7c735181): rejected with
// kFreezeWording. PASS-WITH: compiles, runs, ASan-clean, returns 99. // per
// §28.3, #4424
BEZEL_TEST(reassign_call_expr_freeze_4424,
           method_static_factory_with_arg_runs_4424) {
    std::string src =
        std::string(kResWithArgFactory) +
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(5); "
        "    var y := x; "
        "    x := new Res(9); "
        "    y := Factory.Make(99); "
        "    var t := y.tag; "
        "    y.Dispose(); "
        "    x.Dispose(); "
        "    return t; "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 99);
}

// Row 5 (negative control, real execution, the
// "different-symbol rebind via bare identifier" guard): `y` is rebound
// to `w` (a SEPARATE owned local) — a bare-identifier RHS that
// `recollectAliasBorrowSourceOnReassign`'s existing IdentifierExpr arm
// handles via `resolveAliasChainRoot`. `w` is its OWN owned local (NOT a
// rebind through `x`), so the new window's source is null/empty and the freeze
// does NOT fire. This row guards against the fix over-broadening into a
// shape that the existing IdentifierExpr arm already covers correctly —
// the new CallExpr arm must NOT also close windows for non-CallExpr RHS.
// // per §28.3, #4424
BEZEL_TEST(reassign_call_expr_freeze_4424,
           method_call_reassign_to_other_owned_local_runs_control_4424) {
    std::string src =
        std::string(kResAndFactory) +
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(5); "
        "    var w := new Res(7); "
        "    var y := x; "
        "    x := new Res(9); "
        "    y := w; "
        "    var t := y.tag; "
        "    w.Dispose(); "
        "    x.Dispose(); "
        "    return t; "
        "  end Main; "
        "end C;";
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}