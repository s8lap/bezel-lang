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
