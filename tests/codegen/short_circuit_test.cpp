// Codegen slice — short-circuit `and` / `or` (§17). The right operand is
// evaluated only on the deciding path. Observed by putting a throwing
// operation (divide-by-zero) in the RHS: if it were evaluated eagerly the
// program would raise; short-circuiting skips it.

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

BEZEL_TEST(codegen_sc, and_short_circuits_false_lhs) {
    // LHS false → RHS (which would divide by zero) is NOT evaluated.
    BEZEL_EXPECT_EQ(run(
        "var z := 0; "
        "if false and (10 / z > 0) then return 1; end; return 2;"), 2);
}

BEZEL_TEST(codegen_sc, or_short_circuits_true_lhs) {
    // LHS true → RHS (divide by zero) is NOT evaluated.
    BEZEL_EXPECT_EQ(run(
        "var z := 0; "
        "if true or (10 / z > 0) then return 3; end; return 4;"), 3);
}

BEZEL_TEST(codegen_sc, and_evaluates_rhs_when_lhs_true) {
    BEZEL_EXPECT_EQ(run(
        "var a := 5; "
        "if (a > 0) and (a < 10) then return 7; end; return 0;"), 7);
}

BEZEL_TEST(codegen_sc, or_evaluates_rhs_when_lhs_false) {
    BEZEL_EXPECT_EQ(run(
        "var a := 5; "
        "if (a > 100) or (a = 5) then return 8; end; return 0;"), 8);
}

BEZEL_TEST(codegen_sc, and_result_false_from_rhs) {
    BEZEL_EXPECT_EQ(run(
        "var a := 5; "
        "if (a > 0) and (a > 100) then return 1; end; return 9;"), 9);
}

BEZEL_TEST(codegen_sc, and_rhs_divide_would_throw_if_eager) {
    // A non-short-circuiting `and` would raise DivideByZeroException here;
    // short-circuiting returns the LHS-determined result instead.
    BEZEL_EXPECT_EQ(run(
        "var z := 0; var ok := false; "
        "var r := ok and (10 / z = 0); "
        "return (1 if r else 11);"), 11);
}
