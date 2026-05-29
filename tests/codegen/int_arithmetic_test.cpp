// Codegen slice CG2 — integer arithmetic, bitwise, and shift operators,
// plus unary -/~/+ (§6.1, §17.1). Precedence/associativity come from the
// parser; codegen just lowers the resulting tree. NOTE: overflow and
// divide-by-zero checking (§6.1 OverflowException/DivideByZeroException)
// is deferred to the exception-runtime slice; these tests stay within the
// non-overflowing, non-zero-divisor range.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& expr) {
    auto r = runJit(std::string("module M; class C "
        "public static method Main(): int => ") + expr + "; end C;");
    return r.ran ? r.exitCode : -999;
}

} // namespace

BEZEL_TEST(codegen_cg2, addition) {
    BEZEL_EXPECT_EQ(run("2 + 3"), 5);
}

BEZEL_TEST(codegen_cg2, precedence_mul_over_add) {
    BEZEL_EXPECT_EQ(run("1 + 2 * 3"), 7);
}

BEZEL_TEST(codegen_cg2, subtraction_and_grouping) {
    BEZEL_EXPECT_EQ(run("(10 - 2) * 2"), 16);
}

BEZEL_TEST(codegen_cg2, division_truncates_toward_zero) {
    BEZEL_EXPECT_EQ(run("7 / 2"), 3);
}

BEZEL_TEST(codegen_cg2, mod_follows_dividend_sign) {
    // §6.1: -7 mod 3 == -1.
    BEZEL_EXPECT_EQ(run("-7 mod 3"), -1);
}

BEZEL_TEST(codegen_cg2, unary_negation) {
    BEZEL_EXPECT_EQ(run("-(5 + 1)"), -6);
}

BEZEL_TEST(codegen_cg2, bitwise_and_or_xor) {
    BEZEL_EXPECT_EQ(run("(12 & 10) | 1"), 9);   // (1100 & 1010)=1000 | 1 = 1001
}

BEZEL_TEST(codegen_cg2, left_shift) {
    BEZEL_EXPECT_EQ(run("1 << 4"), 16);
}

BEZEL_TEST(codegen_cg2, right_shift_arithmetic) {
    BEZEL_EXPECT_EQ(run("-16 >> 2"), -4);
}

BEZEL_TEST(codegen_cg2, bitwise_not) {
    BEZEL_EXPECT_EQ(run("~0"), -1);
}
