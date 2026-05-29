// Codegen slice CG22 — overflow-checked integer arithmetic and division
// by zero (§6.1), deferred from CG2. `+ - *`, unary `-`, and `/`,`mod`
// raise OverflowException on overflow; `/` and `mod` raise
// DivideByZeroException on a zero divisor. Built-in exception types get
// runtime descriptors here (the §31.4 hierarchy), so the raises are
// catchable by the exact type or any ancestor. Observable via exit code.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& body) {
    auto r = runJit(
        std::string("module M; class C public static method Main(): int ") +
        body + " end Main; end C;");
    return r.ran ? r.exitCode : -999;
}

} // namespace

BEZEL_TEST(codegen_cg22, divide_by_zero_caught) {
    BEZEL_EXPECT_EQ(run(
        "var z := 0; "
        "try var r := 10 / z; return r; "
        "catch DivideByZeroException => return 1; end;"), 1);
}

BEZEL_TEST(codegen_cg22, divide_by_zero_caught_by_arithmetic_base) {
    BEZEL_EXPECT_EQ(run(
        "var z := 0; "
        "try var r := 10 / z; return r; "
        "catch ArithmeticException => return 2; end;"), 2);
}

BEZEL_TEST(codegen_cg22, divide_by_zero_caught_by_exception_root) {
    BEZEL_EXPECT_EQ(run(
        "var z := 0; "
        "try var r := 10 / z; return r; "
        "catch Exception => return 3; end;"), 3);
}

BEZEL_TEST(codegen_cg22, mod_by_zero_caught) {
    BEZEL_EXPECT_EQ(run(
        "var z := 0; "
        "try var r := 10 mod z; return r; "
        "catch DivideByZeroException => return 4; end;"), 4);
}

BEZEL_TEST(codegen_cg22, add_overflow_caught) {
    BEZEL_EXPECT_EQ(run(
        "var a := 2000000000; "
        "try var r := a + a; return r; "
        "catch OverflowException => return 5; end;"), 5);
}

BEZEL_TEST(codegen_cg22, mul_overflow_caught) {
    BEZEL_EXPECT_EQ(run(
        "var a := 100000; "
        "try var r := a * a; return r; "
        "catch OverflowException => return 6; end;"), 6);
}

BEZEL_TEST(codegen_cg22, overflow_caught_by_exception_root) {
    BEZEL_EXPECT_EQ(run(
        "var a := 2000000000; "
        "try var r := a + a; return r; "
        "catch Exception => return 7; end;"), 7);
}

BEZEL_TEST(codegen_cg22, no_overflow_computes_normally) {
    // Non-overflowing arithmetic is unaffected by the checks.
    BEZEL_EXPECT_EQ(run(
        "var a := 6; var b := 7; return a * b;"), 42);
}

BEZEL_TEST(codegen_cg22, division_no_zero_computes_normally) {
    BEZEL_EXPECT_EQ(run(
        "var a := 84; var b := 2; return a / b;"), 42);
}

BEZEL_TEST(codegen_cg22, overflow_propagates_uncaught_to_outer) {
    // Inner try catches only DivideByZero; an overflow propagates to the
    // outer handler.
    BEZEL_EXPECT_EQ(run(
        "var a := 2000000000; "
        "try "
        "  try var r := a + a; return r; "
        "  catch DivideByZeroException => return 1; end; "
        "catch OverflowException => return 9; end;"), 9);
}
