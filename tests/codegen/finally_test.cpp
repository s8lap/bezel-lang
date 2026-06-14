// Codegen slice CG21b — `finally` (§31.2). A finally block runs on every
// exit from the try: normal completion of the try body, a caught
// exception (after the arm), and an uncaught/propagating exception
// (before it re-propagates), as well as when a catch arm itself throws.
// A static counter set by the finally makes this observable.
//
// (Deferred: return/break/continue inside the try or a catch arm routing
// through finally; finally bodies that transfer control.)

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& program) {
    auto r = runJit(program);
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
        "module M; class E end E; class C "
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
        "module M; class E end E; class C "
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
        "module M; class A end A; class B end B; class C "
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
        "module M; class E end E; class C "
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
        "    try return () => 42; finally var _x: int := 0; end; "
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
        "    try return () => _n; finally _n := 7; end; "
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
        "module M; class AppErr end AppErr; class FinErr end FinErr; "
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
        "module M; class AppErr end AppErr; "
        "class C public static method Main(): int "
        "  var ran := 0; "
        "  try "
        "    try throw new AppErr(); "
        "    finally ran := 1; end; "
        "  catch AppErr => return ran + 40; "
        "  end; "
        "end Main; end C;"), 41);
}
