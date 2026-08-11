// Codegen slice CG21b — `finally` (§31.2). A finally block runs on every
// exit from the try: normal completion of the try body, a caught
// exception (after the arm), and an uncaught/propagating exception
// (before it re-propagates), as well as when a catch arm itself throws.
// A static counter set by the finally makes this observable.
//
// (Deferred: return/break/continue inside the try or a catch arm routing
// through finally; finally bodies that transfer control.)

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

} // namespace

BEZEL_TEST(codegen_cg21b, finally_runs_on_normal_completion) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static _n: int := 0; "
        "  public static method Main(): int "
        "    try _n := 1; finally _n := _n + 10; end; "
        "    return _n; "
        "  end Main; "
        "end C;"), 11);
}

BEZEL_TEST(codegen_cg21b, finally_runs_after_catch) {
    BEZEL_EXPECT_EQ(run(
        "module M; class E: Exception end E; class C "
        "  public static _n: int := 0; "
        "  public static method Main(): int "
        "    try throw new E(); catch E => _n := 1; finally _n := _n + 10; end; "
        "    return _n; "
        "  end Main; "
        "end C;"), 11);
}

BEZEL_TEST(codegen_cg21b, finally_runs_on_uncaught_propagation) {
    // Inner try has only a finally; the exception propagates to the outer
    // catch, but the inner finally runs first (sets the counter).
    BEZEL_EXPECT_EQ(run(
        "module M; class E: Exception end E; class C "
        "  public static _n: int := 0; "
        "  public static method Main(): int "
        "    try "
        "      try throw new E(); finally _n := 5; end; "
        "    catch E => _n := _n + 100; end; "
        "    return _n; "
        "  end Main; "
        "end C;"), 105);
}

BEZEL_TEST(codegen_cg21b, finally_runs_when_no_arm_matches) {
    // Inner catch doesn't match; finally runs, then the outer catches.
    BEZEL_EXPECT_EQ(run(
        "module M; class A: Exception end A; class B: Exception end B; class C "
        "  public static _n: int := 0; "
        "  public static method Main(): int "
        "    try "
        "      try throw new B(); catch A => _n := 1; finally _n := _n + 3; end; "
        "    catch B => _n := _n + 100; end; "
        "    return _n; "
        "  end Main; "
        "end C;"), 103);
}

BEZEL_TEST(codegen_cg21b, finally_runs_when_arm_rethrows) {
    // The matching arm re-throws; finally still runs before propagation.
    BEZEL_EXPECT_EQ(run(
        "module M; class E: Exception end E; class C "
        "  public static _n: int := 0; "
        "  public static method Main(): int "
        "    try "
        "      try throw new E(); catch E => throw; finally _n := 7; end; "
        "    catch E => _n := _n + 100; end; "
        "    return _n; "
        "  end Main; "
        "end C;"), 107);
}

BEZEL_TEST(codegen_cg21b, try_finally_without_catch) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static _n: int := 0; "
        "  public static method Main(): int "
        "    try _n := 2; finally _n := _n * 5; end; "
        "    return _n; "
        "  end Main; "
        "end C;"), 10);
}

// §31.2 / §27.5 / issue #546: returning a function-typed (closure) value
// through a try-with-finally must not crash the compiler.  The bug was that
// lowerLambda cleared finallyStack_ without saving/restoring it, so the
// enclosing lowerTry's pop_back() fired on an empty stack (assertion failure).
// The lambda () => 42 is compiled INSIDE the try body while the FinallyFrame
// is live on finallyStack_; after the fix, the frame is preserved across the
// nested lambda compilation and the finally exit-mode dispatch works correctly.
BEZEL_TEST(codegen_cg21b_546, finally_function_return_compiles_and_runs) {
    // A method that returns a function type via `return <lambda>` inside a
    // try-with-finally.  The returned value (exit mode = 1) must be stashed in
    // the retValSlot, survive the finally body, and be returned to the caller.
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method GetFn(): () -> int "
        "    try return lambda() => 42; finally var _x: int := 0; end; "
        "  end GetFn; "
        "  public static method Main(): int "
        "    var f: () -> int := C.GetFn(); "
        "    return f(); "
        "  end Main; "
        "end C;"), 42);
}

// §31.2 / §27.5 / issue #546: finally body runs before the closure is
// returned — the side-effect counter shows finally executed, and the closure
// value (exit mode = 1 through the retValSlot) still reaches the caller intact.
BEZEL_TEST(codegen_cg21b_546, finally_runs_then_function_value_returned) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static _n: int := 0; "
        "  public static method GetFn(): () -> int "
        "    try return lambda() => _n; finally _n := 7; end; "
        "  end GetFn; "
        "  public static method Main(): int "
        "    var f: () -> int := C.GetFn(); "
        "    return f(); "          // _n was set to 7 by finally before return
        "  end Main; "
        "end C;"), 7);
}

// #720 / §31.2: a `throw` inside a `finally` that runs during active exception
// propagation SUPERSEDES the original exception. Previously the original was
// caught instead — the synchronous finally ran with bz$exc still holding the
// original, so a `new` in the finally hit emitNewPendingCheck and unwound the
// original before the finally's own `throw` executed. The finally now runs with
// a cleared bz$exc and supersedes (or restores) correctly.
BEZEL_TEST(codegen_cg21b, throw_in_finally_supersedes_original_720) {
    BEZEL_EXPECT_EQ(run(
        "module M; class AppErr: Exception end AppErr; class FinErr: Exception end FinErr; "
        "class C public static method Main(): int "
        "  try "
        "    try throw new AppErr(); "
        "    finally throw new FinErr(); end; "
        "  catch FinErr => return 99; "
        "  catch AppErr => return 1; "
        "  end; "
        "end Main; end C;"), 99);
}

// #720 regression: a finally that does NOT throw lets the original propagate.
BEZEL_TEST(codegen_cg21b, finally_no_throw_original_propagates_720) {
    BEZEL_EXPECT_EQ(run(
        "module M; class AppErr: Exception end AppErr; "
        "class C public static method Main(): int "
        "  var ran := 0; "
        "  try "
        "    try throw new AppErr(); "
        "    finally ran := 1; end; "
        "  catch AppErr => return ran + 40; "
        "  end; "
        "end Main; end C;"), 41);
}

// ── #1149 / §31.2: a finally that THROWS supersedes the in-flight exception ───
// E1, which must then be freed exactly once (sibling of the #1108 catch-guard
// supersede leak). E1 was previously freed ZERO times — a 16-byte leak — because
// a direct `throw` in the finally TERMINATES the body and bypasses the
// supersede/restore check (the body's own throw propagates E2 directly). The fix
// exposes E1 as the caught object for the duration of the finally body so a
// `throw` routes through emitThrowValue's #397/#866 caught-exception free,
// guarded against double-free by isDirectRethrow / throwLeavesArm. runJit exit
// codes don't observe the leak directly (gated by the run's ASan harness); these
// cases pin the supersede CONTROL FLOW and stress the free path so a double-free
// regression — which crashes the in-process JIT suite — is caught.

// #1149: the finally's E2 supersedes E1 and is caught by the outer `catch E2`.
BEZEL_TEST(finally_supersede_1149, finally_throw_supersedes_outer_catches) {
    BEZEL_EXPECT_EQ(run(
        "module M; class E1: Exception end E1; class E2: Exception end E2; "
        "class C public static method Main(): int "
        "  try "
        "    try throw new E1(); finally throw new E2(); end; "
        "  catch E2 => return 7; "                       // superseding E2 caught
        "  catch E1 => return 1; "                       // E1 must NOT be seen
        "  end; "
        "  return 0; "
        "end Main; end C;"), 7);
}

// #1149: the superseding E2 skips a non-matching outer arm and is caught by the
// matching one (the original E1 is superseded, never reaches the E1 arm).
BEZEL_TEST(finally_supersede_1149, finally_throw_supersede_skips_nonmatching_arm) {
    BEZEL_EXPECT_EQ(run(
        "module M; class E1: Exception end E1; class E2: Exception end E2; class E3: Exception end E3; "
        "class C public static method Main(): int "
        "  try "
        "    try throw new E1(); finally throw new E2(); end; "
        "  catch E3 => return 2; "                       // not matched
        "  catch E2 => return 9; "                       // superseding E2 caught
        "  catch E1 => return 1; "                       // superseded — skipped
        "  end; "
        "  return 0; "
        "end Main; end C;"), 9);
}

// #1149: a finally whose throw is conditional (the body does NOT statically
// terminate — exercises the supersede check path, finUTerm=false). E2 supersedes.
BEZEL_TEST(finally_supersede_1149, finally_conditional_throw_supersedes) {
    BEZEL_EXPECT_EQ(run(
        "module M; class E1: Exception end E1; class E2: Exception end E2; "
        "class C public static method Main(): int "
        "  var t := true; "
        "  try "
        "    try throw new E1(); finally if t then throw new E2(); end; end; "
        "  catch E2 => return 5; "
        "  catch E1 => return 1; "
        "  end; "
        "  return 0; "
        "end Main; end C;"), 5);
}

// #1149: the finally-supersede repeated 100x — each iteration's superseded E1
// must be freed exactly once. A double-free regression crashes the in-process
// JIT run here; a leak accumulates (caught by the ASan harness). rc=100.
BEZEL_TEST(finally_supersede_1149, finally_throw_supersede_repeated_no_corruption) {
    BEZEL_EXPECT_EQ(run(
        "module M; class E1: Exception end E1; class E2: Exception end E2; "
        "class C public static method Main(): int "
        "  var total := 0; var i := 0; "
        "  while i < 100 do "
        "    try "
        "      try throw new E1(); finally throw new E2(); end; "
        "    catch E2 => total := total + 1; "
        "    end; "
        "    i := i + 1; "
        "  end; "
        "  return total; "                               // 100
        "end Main; end C;"), 100);
}

// ── #1216 / §31.2: a finally that throws VIA A CALL supersedes E1 ──────────
// Residual of #1149: #1149 exposed E1 as currentCaughtExc_ so emitThrowValue
// (direct `throw`) would free it. But a call that propagates via
// emitPendingCheck (NOT emitThrowValue) never consulted currentCaughtExc_.
// Fix: add the same throwLeavesArm guard to emitPendingCheck /
// emitNewPendingCheck so the captured E1 is freed exactly once on the
// call-propagation unwind edge.

// #1216: the basic repro — `var x := Boom()` in the finally, Boom() throws.
// E1 must be freed, E2 must reach the outer `catch E2` (rc=6).
BEZEL_TEST(finally_supersede_1216, finally_call_throw_supersedes_outer_catches) {
    BEZEL_EXPECT_EQ(run(
        "module M; class E1: Exception end E1; class E2: Exception end E2; "
        "class C "
        "  private static method Boom(): int  throw new E2();  end Boom; "
        "  public static method Main(): int "
        "    try "
        "      try throw new E1(); finally var x := Boom(); end; "
        "    catch E2 => return 6; "                     // superseding E2 caught
        "    catch E1 => return 1; "                     // E1 must NOT be seen
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;"), 6);
}

// #1216: call-throw-in-finally repeated 100x — each iteration's superseded E1
// must be freed exactly once. A double-free crashes the in-process JIT; a leak
// accumulates (caught by the ASan harness). rc=100.
BEZEL_TEST(finally_supersede_1216, finally_call_throw_supersede_repeated_no_corruption) {
    BEZEL_EXPECT_EQ(run(
        "module M; class E1: Exception end E1; class E2: Exception end E2; "
        "class C "
        "  private static method Boom(): int  throw new E2();  end Boom; "
        "  public static method Main(): int "
        "    var total := 0; var i := 0; "
        "    while i < 100 do "
        "      try "
        "        try throw new E1(); finally var x := Boom(); end; "
        "      catch E2 => total := total + 1; "
        "      end; "
        "      i := i + 1; "
        "    end; "
        "    return total; "                             // 100
        "  end Main; "
        "end C;"), 100);
}
