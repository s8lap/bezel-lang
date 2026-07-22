// Codegen slice CG25b — unsigned integer operations (§5, §6). Operands of
// an unsigned type (uint, ulong, ushort, byte) select unsigned LLVM ops:
// udiv/urem, lshr, unsigned comparisons, zero-extension on widening, and
// the unsigned overflow intrinsics. Observed via comparisons.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& body) {
    auto r = runAot(
        std::string("module M; class C public static method Main(): int ") +
        body + " end Main; end C;");
    return r.ran ? r.exitCode : -999;
}

} // namespace

BEZEL_TEST(codegen_cg25b, unsigned_comparison_high_bit) {
    // 3000000000u has the high bit set: as unsigned it is > 1, but as a
    // signed i32 it would be negative. Unsigned `>` must see it as large.
    BEZEL_EXPECT_EQ(run(
        "var a := 3000000000u; var b := 1u; "
        "return (if a > b then 1 else 0);"), 1);
}

BEZEL_TEST(codegen_cg25b, unsigned_division) {
    BEZEL_EXPECT_EQ(run(
        "var a := 4000000000u; var b := 2u; "
        "return (if a / b = 2000000000u then 1 else 0);"), 1);
}

BEZEL_TEST(codegen_cg25b, unsigned_shift_right_is_logical) {
    // Logical (zero-filling) shift of a high-bit value.
    BEZEL_EXPECT_EQ(run(
        "var a := 4000000000u; "
        "return (if (a >> 1) = 2000000000u then 1 else 0);"), 1);
}

BEZEL_TEST(codegen_cg25b, unsigned_less_than) {
    BEZEL_EXPECT_EQ(run(
        "var a := 10u; var b := 3000000000u; "
        "return (if a < b then 1 else 0);"), 1);
}

BEZEL_TEST(codegen_cg25b, ulong_arithmetic) {
    BEZEL_EXPECT_EQ(run(
        "var a := 10000000000uL; var b := 4uL; "
        "return (if a / b = 2500000000uL then 1 else 0);"), 1);
}

BEZEL_TEST(codegen_cg25b, unsigned_addition) {
    BEZEL_EXPECT_EQ(run(
        "var a := 2000000000u; var b := 2000000000u; "
        "return (if a + b = 4000000000u then 1 else 0);"), 1);
}

// §4.2 / A.1 (issue #421): a `ulong` literal above long.MaxValue must keep its
// true 64-bit value. Before the fix, parseIntLiteral used signed strtoll, which
// saturates 18_000_000_000_000_000_000uL to long.MaxValue (≈9.2e18) — so both
// of these distinct ulongs would collapse to the same saturated value and the
// strict `>` below would be FALSE. With unsigned strtoull both keep their true
// values (1.8e19 > 1.7e19) → 1.
BEZEL_TEST(codegen_cg25b, ulong_literal_above_long_max_keeps_value_4_2) {
    BEZEL_EXPECT_EQ(run(
        "var a := 18000000000000000000uL; var b := 17000000000000000000uL; "
        "return (if a > b then 1 else 0);"), 1);
}

// The exact value is recoverable: 1.8e19 − 1.7e19 == 1e18 (which fits in a
// signed long), proving the high literal carried its real magnitude, not the
// saturated long.MaxValue (which would give 1.8e19 − 9.2e18 ≈ 8.8e18 ≠ 1e18).
BEZEL_TEST(codegen_cg25b, ulong_literal_above_long_max_exact_difference_4_2) {
    BEZEL_EXPECT_EQ(run(
        "var a := 18000000000000000000uL; var b := 17000000000000000000uL; "
        "return (if a - b = 1000000000000000000uL then 1 else 0);"), 1);
}

// A single ulong literal above long.MaxValue compared against the boundary:
// 18_000_000_000_000_000_000uL > long.MaxValue (unsigned compare). The signed
// strtoll bug would make it EQUAL to long.MaxValue, so the strict `>` is the
// discriminator → 1 with the fix, 0 without it.
BEZEL_TEST(codegen_cg25b, ulong_literal_exceeds_long_max_value_4_2) {
    BEZEL_EXPECT_EQ(run(
        "var a := 18000000000000000000uL; "
        "return (if a > 9223372036854775807uL then 1 else 0);"), 1);
}
