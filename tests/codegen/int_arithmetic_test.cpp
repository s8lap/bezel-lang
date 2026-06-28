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

// #1049 / §5.1 / §6.2: a relational comparison between an unsigned value and a
// non-negative constant must be UNSIGNED. The signedness used to require BOTH
// operands unsigned, so `ulongVar > 1000` (unsigned vs a signed int literal)
// used a SIGNED compare and gave the wrong result once the unsigned value's
// high bit was set: `ulong 18e18 > 1000` returned false.
namespace {
int runBody(const std::string& body) {
    auto r = bezel::test::runJit(
        std::string("module M; class C public static method Main(): int ") +
        body + " end Main; end C;");
    return r.ran ? r.exitCode : -999;
}
}  // namespace
BEZEL_TEST(codegen_1049_unsigned_compare, ulong_high_bit_gt_small_literal) {
    BEZEL_EXPECT_EQ(runBody(
        "var u: ulong := 18000000000000000000; return (1 if (u > 1000) else 0);"), 1);
}
BEZEL_TEST(codegen_1049_unsigned_compare, ulong_maxvalue_gt_small_literal) {
    BEZEL_EXPECT_EQ(runBody("return (1 if (ulong.MaxValue > 1000) else 0);"), 1);
}
BEZEL_TEST(codegen_1049_unsigned_compare, uint_high_bit_gt_small_literal) {
    BEZEL_EXPECT_EQ(runBody(
        "var u: uint := 3000000000; return (1 if (u > 1000) else 0);"), 1);
}
BEZEL_TEST(codegen_1049_unsigned_compare, ulong_high_bit_lt_small_literal_false) {
    BEZEL_EXPECT_EQ(runBody(
        "var u: ulong := 18000000000000000000; return (1 if (u < 1000) else 0);"), 0);
}
BEZEL_TEST(codegen_1049_unsigned_compare, ulong_ge_small_literal) {
    BEZEL_EXPECT_EQ(runBody(
        "var u: ulong := 18000000000000000000; return (1 if (u >= 5) else 0);"), 1);
}
// No-regression: a small unsigned vs a NEGATIVE constant keeps the signed
// compare (`u > -5` is true), and small unsigned vs a larger positive literal
// is still correctly false.
BEZEL_TEST(codegen_1049_unsigned_compare, ulong_small_gt_negative_literal) {
    BEZEL_EXPECT_EQ(runBody(
        "var u: ulong := 10; return (1 if (u > 0 - 5) else 0);"), 1);
}
BEZEL_TEST(codegen_1049_unsigned_compare, uint_small_gt_large_literal_false) {
    BEZEL_EXPECT_EQ(runBody(
        "var u: uint := 5; return (1 if (u > 1000) else 0);"), 0);
}

// #1050 / §5.1: the LIFTED (nullable) relational comparison had the same
// unsigned-vs-non-negative-constant signedness bug as #1049 (the non-nullable
// site) — `uint? 3e9 > 1000` was signed-compared and returned false. (Observed
// through a `bool?` local, which correctly wraps the lifted i1 result; the
// inline `(cmp) ?? false` form is a separate consumption bug, #1045.)
BEZEL_TEST(codegen_1050_lifted_unsigned_compare, uint_nullable_high_bit_gt_literal) {
    BEZEL_EXPECT_EQ(runBody(
        "var u: uint? := 3000000000; var r: bool? := u > 1000; "
        "return (1 if (r ?? false) else 0);"), 1);
}
BEZEL_TEST(codegen_1050_lifted_unsigned_compare, ulong_nullable_high_bit_gt_literal) {
    BEZEL_EXPECT_EQ(runBody(
        "var u: ulong? := 18000000000000000000; var r: bool? := u > 1000; "
        "return (1 if (r ?? false) else 0);"), 1);
}
BEZEL_TEST(codegen_1050_lifted_unsigned_compare, uint_nullable_lt_literal_false) {
    BEZEL_EXPECT_EQ(runBody(
        "var u: uint? := 3000000000; var r: bool? := u < 1000; "
        "return (1 if (r ?? false) else 0);"), 0);
}
// No-regression: signed nullable and null propagation unchanged.
BEZEL_TEST(codegen_1050_lifted_unsigned_compare, int_nullable_signed_compare_unchanged) {
    BEZEL_EXPECT_EQ(runBody(
        "var i: int? := 0 - 5; var r: bool? := i < 1000; "
        "return (1 if (r ?? false) else 0);"), 1);
}
BEZEL_TEST(codegen_1050_lifted_unsigned_compare, uint_nullable_null_propagates) {
    BEZEL_EXPECT_EQ(runBody(
        "var u: uint? := null; var r: bool? := u > 1000; "
        "return (1 if (r ?? false) else 0);"), 0);
}

// #1210 / §6.5: const-fold of a shift masks the count by the LEFT operand's
// bit width (int → & 31, matching the runtime rule enforced by codegen #1094).
// Before the fix, `1 << 40` folded to 1099511627776 (unmasked), causing an
// inline-array with that size to allocate a ~16 GB stack frame → SIGSEGV.
// After the fix it folds to `1 << (40 & 31)` = `1 << 8` = 256.

// CRASH FIX: an inline array whose size is a shift const-fold with count >= 32
// must NOT produce a giant stack frame. `[1 << 40]int` must have length 256.
BEZEL_TEST(shift_const_fold_masks_count_1210,
           inline_array_shift_size_no_overflow_1210) {
    // §6.5: 1 << 40 for int-typed operand: 40 & 31 = 8, so 1 << 8 = 256.
    // Length must be 256, not ~1e12 (which was the pre-fix unmasked value).
    BEZEL_EXPECT_EQ(runBody(
        "var a: [1 << 40]int; return a.Length - 255;"), 1);
}

// RIGHT-SHIFT: same masking applies to >>.
// 1024 >> 33: 33 & 31 = 1, so 1024 >> 1 = 512.
BEZEL_TEST(shift_const_fold_masks_count_1210,
           inline_array_right_shift_size_masked_1210) {
    BEZEL_EXPECT_EQ(runBody(
        "var a: [1024 >> 33]int; return a.Length - 511;"), 1);
}

// NO-REGRESSION: a normal in-range shift const must still fold correctly.
// 1 << 4 = 16; array length must be 16, not 0 or some other value.
BEZEL_TEST(shift_const_fold_masks_count_1210,
           inline_array_normal_shift_size_unchanged_1210) {
    BEZEL_EXPECT_EQ(runBody(
        "var a: [1 << 4]int; return a.Length;"), 16);
}

// CONST-THROUGH-IDENTIFIER: `const SZ: int := 1 << 40;` used as array size —
// the fold recurses into the const's initialiser, masking there too.
BEZEL_TEST(shift_const_fold_masks_count_1210,
           inline_array_const_shift_size_masked_1210) {
    BEZEL_EXPECT_EQ(runBody(
        "const SZ: int := 1 << 40; "
        "var a: [SZ]int; return a.Length - 255;"), 1);
}
