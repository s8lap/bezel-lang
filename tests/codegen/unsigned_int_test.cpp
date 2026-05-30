// Codegen slice CG25b — unsigned integer operations (§5, §6). Operands of
// an unsigned type (uint, ulong, ushort, byte) select unsigned LLVM ops:
// udiv/urem, lshr, unsigned comparisons, zero-extension on widening, and
// the unsigned overflow intrinsics. Observed via comparisons.

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

BEZEL_TEST(codegen_cg25b, unsigned_comparison_high_bit) {
    // 3000000000u has the high bit set: as unsigned it is > 1, but as a
    // signed i32 it would be negative. Unsigned `>` must see it as large.
    BEZEL_EXPECT_EQ(run(
        "var a := 3000000000u; var b := 1u; "
        "return (1 if a > b else 0);"), 1);
}

BEZEL_TEST(codegen_cg25b, unsigned_division) {
    BEZEL_EXPECT_EQ(run(
        "var a := 4000000000u; var b := 2u; "
        "return (1 if a / b = 2000000000u else 0);"), 1);
}

BEZEL_TEST(codegen_cg25b, unsigned_shift_right_is_logical) {
    // Logical (zero-filling) shift of a high-bit value.
    BEZEL_EXPECT_EQ(run(
        "var a := 4000000000u; "
        "return (1 if (a >> 1) = 2000000000u else 0);"), 1);
}

BEZEL_TEST(codegen_cg25b, unsigned_less_than) {
    BEZEL_EXPECT_EQ(run(
        "var a := 10u; var b := 3000000000u; "
        "return (1 if a < b else 0);"), 1);
}

BEZEL_TEST(codegen_cg25b, ulong_arithmetic) {
    BEZEL_EXPECT_EQ(run(
        "var a := 10000000000uL; var b := 4uL; "
        "return (1 if a / b = 2500000000uL else 0);"), 1);
}

BEZEL_TEST(codegen_cg25b, unsigned_addition) {
    BEZEL_EXPECT_EQ(run(
        "var a := 2000000000u; var b := 2000000000u; "
        "return (1 if a + b = 4000000000u else 0);"), 1);
}
