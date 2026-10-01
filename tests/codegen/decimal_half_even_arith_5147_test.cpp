// fix #5147 / §6.4 (v0.313): decimal +, - and * round the exact result ONCE,
// to nearest with a tie to the even last digit, at the largest scale s <= 28
// whose rounded mantissa is at most 2^96 - 1; a result that does not fit at
// scale 0 is OverflowException. bz_decimal_add/mul (src/runtime/
// bezel_runtime_decimal_guid.cpp) used to round half-up, per dropped digit,
// and left product scales up to 56.
//
// Every expected line is the exact round-half-even value computed
// independently with Python fractions.Fraction (largest s <= 28 with rounded
// mantissa <= 2^96 - 1); the same oracle agreed on 2211 random/edge cases
// with zero mismatches. Rows pinned on the half-up runtime FAIL without the
// fix and PASS with it.

#include "support/aot_support.hpp"   // runAot / runAotChecked
#include "support/codegen_support.hpp"
#include "test_harness.hpp"
#include <string>

using bezel::test::runAot;

// §6.4 (v0.313): Products whose exact value has a 29th fractional digit of
// exactly 5 (a tie at scale 28) round to the even last digit, positive and
// negative: 5e-28 * 0.5 = 2.5e-28 -> 2e-28 (2 is even); 15e-28 * 0.5 =
// 7.5e-28 -> 8e-28 (7 is odd). The half-up runtime rounded every tie up.
BEZEL_TEST(decimal_half_even_arith_5147, mul_ties_at_scale_28) {
    BEZEL_EXPECT_EQ(runAot(
        "module M; class C "
        "  public static method Main(): int "
        "try Console.WriteLine(0.0000000000000000000000000005m * 0.5m); catch OverflowException => Console.WriteLine(\"OverflowException\"); end; "
        "try Console.WriteLine(0.0000000000000000000000000015m * 0.5m); catch OverflowException => Console.WriteLine(\"OverflowException\"); end; "
        "try Console.WriteLine(0.0000000000000000000000000025m * 0.5m); catch OverflowException => Console.WriteLine(\"OverflowException\"); end; "
        "try Console.WriteLine(0.0000000000000000000000000035m * 0.5m); catch OverflowException => Console.WriteLine(\"OverflowException\"); end; "
        "try Console.WriteLine((-0.0000000000000000000000000005m) * 0.5m); catch OverflowException => Console.WriteLine(\"OverflowException\"); end; "
        "try Console.WriteLine((-0.0000000000000000000000000015m) * 0.5m); catch OverflowException => Console.WriteLine(\"OverflowException\"); end; "
        "try Console.WriteLine((-0.0000000000000000000000000025m) * 0.5m); catch OverflowException => Console.WriteLine(\"OverflowException\"); end; "
        "try Console.WriteLine(0.0000000000000000000000000001m * 0.5m); catch OverflowException => Console.WriteLine(\"OverflowException\"); end; "
        "try Console.WriteLine(0.0000000000000000000000000001m * 0.4m); catch OverflowException => Console.WriteLine(\"OverflowException\"); end; "
        "try Console.WriteLine(0.0000000000000000000000000001m * 0.6m); catch OverflowException => Console.WriteLine(\"OverflowException\"); end; "
        "try Console.WriteLine((-0.0000000000000000000000000001m) * 0.5m); catch OverflowException => Console.WriteLine(\"OverflowException\"); end; "
        "    return 0; "
        "  end Main; "
        "end C;").stdoutText,
        "0.0000000000000000000000000002\n0.0000000000000000000000000008\n0.0000000000000000000000000012\n0.0000000000000000000000000018\n-0.0000000000000000000000000002\n-0.0000000000000000000000000008\n-0.0000000000000000000000000012\n0\n0\n0.0000000000000000000000000001\n0\n");
}

// §6.4 (v0.313): A product whose exact scale exceeds 28 is rounded once at
// scale 28 (the old runtime stored scales up to 56); a nonzero result that
// rounds to zero is 0 (1e-28 * 1e-28). Guards: 1e-14 * 1e-14 = 1e-28 is
// exact.
BEZEL_TEST(decimal_half_even_arith_5147, mul_scale_above_28_rounds_once) {
    BEZEL_EXPECT_EQ(runAot(
        "module M; class C "
        "  public static method Main(): int "
        "try Console.WriteLine(0.0000000000000000000000000001m * 0.0000000000000000000000000001m); catch OverflowException => Console.WriteLine(\"OverflowException\"); end; "
        "try Console.WriteLine(0.00000000000001m * 0.00000000000001m); catch OverflowException => Console.WriteLine(\"OverflowException\"); end; "
        "try Console.WriteLine(0.00000000000005m * 0.00000000000001m); catch OverflowException => Console.WriteLine(\"OverflowException\"); end; "
        "try Console.WriteLine(0.0000000000000000000000000005m * 0.0000000000000000000000000005m); catch OverflowException => Console.WriteLine(\"OverflowException\"); end; "
        "try Console.WriteLine(0.0000000000000000000000000003m * 0.1m); catch OverflowException => Console.WriteLine(\"OverflowException\"); end; "
        "try Console.WriteLine(0.0000000000000000000000000007m * 0.1m); catch OverflowException => Console.WriteLine(\"OverflowException\"); end; "
        "    return 0; "
        "  end Main; "
        "end C;").stdoutText,
        "0\n0.0000000000000000000000000001\n0.0000000000000000000000000005\n0\n0\n0.0000000000000000000000000001\n");
}

// §6.4 (v0.313): The 192-bit product of large mantissas must be exact
// before it is rounded (the old column-sum carry landed one limb low and
// corrupted large products); results round at the largest fitting scale,
// MaxValue * MaxValue overflows.
BEZEL_TEST(decimal_half_even_arith_5147, mul_large_operands_exact_carry) {
    BEZEL_EXPECT_EQ(runAot(
        "module M; class C "
        "  public static method Main(): int "
        "try Console.WriteLine(decimal.MaxValue * decimal.MaxValue); catch OverflowException => Console.WriteLine(\"OverflowException\"); end; "
        "try Console.WriteLine(7.9228162514264337593543950335m * 7.9228162514264337593543950335m); catch OverflowException => Console.WriteLine(\"OverflowException\"); end; "
        "try Console.WriteLine(decimal.MaxValue * 1m); catch OverflowException => Console.WriteLine(\"OverflowException\"); end; "
        "try Console.WriteLine(decimal.MaxValue * 0.1m); catch OverflowException => Console.WriteLine(\"OverflowException\"); end; "
        "try Console.WriteLine(decimal.MaxValue * 0.9m); catch OverflowException => Console.WriteLine(\"OverflowException\"); end; "
        "try Console.WriteLine(decimal.MaxValue * 0.5m); catch OverflowException => Console.WriteLine(\"OverflowException\"); end; "
        "try Console.WriteLine(79228162514264337593543950334m * 0.5m); catch OverflowException => Console.WriteLine(\"OverflowException\"); end; "
        "try Console.WriteLine(decimal.MaxValue * 1.1m); catch OverflowException => Console.WriteLine(\"OverflowException\"); end; "
        "try Console.WriteLine(7.9228162514264337593543950324m * 655026247.62141638432855060215m); catch OverflowException => Console.WriteLine(\"OverflowException\"); end; "
        "try Console.WriteLine(792281625142643.37593543950307m * 7.9228162514264337593543950315m); catch OverflowException => Console.WriteLine(\"OverflowException\"); end; "
        "try Console.WriteLine(79.228162514264337593543950299m * (-79.228162514264337593543950321m)); catch OverflowException => Console.WriteLine(\"OverflowException\"); end; "
        "try Console.WriteLine(45.920389650820644152249272203m * (-792281625142643375935439503.35m)); catch OverflowException => Console.WriteLine(\"OverflowException\"); end; "
        "    return 0; "
        "  end Main; "
        "end C;").stdoutText,
        "OverflowException\n62.77101735386680763835789423\n79228162514264337593543950335\n7922816251426433759354395033.5\n71305346262837903834189555302\n39614081257132168796771975168\n39614081257132168796771975167\nOverflowException\n5189652599.7658331306449058799\n6277101735386680.7638357894192\n-6277.1017353866807638357894191\n-36381880939735601936554675476\n");
}

// §6.4 (v0.313): An exact sum needing more than 2^96-1 at its scale is
// rounded once at the largest fitting scale, ties to even: MaxValue-1 + 0.5
// -> MaxValue-1 (tie, even); MaxValue + 0.5 would round up past 2^96-1 ->
// OverflowException. The old alignment rounded the smaller operand half-up
// first.
BEZEL_TEST(decimal_half_even_arith_5147, add_ties_near_max) {
    BEZEL_EXPECT_EQ(runAot(
        "module M; class C "
        "  public static method Main(): int "
        "try Console.WriteLine(79228162514264337593543950334m + 0.5m); catch OverflowException => Console.WriteLine(\"OverflowException\"); end; "
        "try Console.WriteLine(decimal.MaxValue + 0.5m); catch OverflowException => Console.WriteLine(\"OverflowException\"); end; "
        "try Console.WriteLine(79228162514264337593543950333m + 0.5m); catch OverflowException => Console.WriteLine(\"OverflowException\"); end; "
        "try Console.WriteLine(79228162514264337593543950334m + 1.5m); catch OverflowException => Console.WriteLine(\"OverflowException\"); end; "
        "try Console.WriteLine((-79228162514264337593543950334m) + (-0.5m)); catch OverflowException => Console.WriteLine(\"OverflowException\"); end; "
        "try Console.WriteLine(decimal.MaxValue + 0.4m); catch OverflowException => Console.WriteLine(\"OverflowException\"); end; "
        "try Console.WriteLine(decimal.MaxValue + 0.6m); catch OverflowException => Console.WriteLine(\"OverflowException\"); end; "
        "try Console.WriteLine(decimal.MaxValue + 0.0000000000000000000000000001m); catch OverflowException => Console.WriteLine(\"OverflowException\"); end; "
        "try Console.WriteLine(decimal.MaxValue + 7.9228162514264337593543950335m); catch OverflowException => Console.WriteLine(\"OverflowException\"); end; "
        "try Console.WriteLine(79228162514264337593543950334m + 0.05m); catch OverflowException => Console.WriteLine(\"OverflowException\"); end; "
        "try Console.WriteLine(7922816251426433759354395033.5m + 7922816251426433759354395033.5m); catch OverflowException => Console.WriteLine(\"OverflowException\"); end; "
        "try Console.WriteLine(decimal.MaxValue + 1m); catch OverflowException => Console.WriteLine(\"OverflowException\"); end; "
        "    return 0; "
        "  end Main; "
        "end C;").stdoutText,
        "79228162514264337593543950334\nOverflowException\n79228162514264337593543950334\nOverflowException\n-79228162514264337593543950334\n79228162514264337593543950335\nOverflowException\n79228162514264337593543950335\nOverflowException\n79228162514264337593543950334\n15845632502852867518708790067\nOverflowException\n");
}

// §6.4 (v0.313): Guards: exact sums (same scale, mixed scale, opposite
// signs, zero) pass through unrounded.
BEZEL_TEST(decimal_half_even_arith_5147, add_exact_and_carry_controls) {
    BEZEL_EXPECT_EQ(runAot(
        "module M; class C "
        "  public static method Main(): int "
        "try Console.WriteLine(0.0000000000000000000000000001m + 0.0000000000000000000000000001m); catch OverflowException => Console.WriteLine(\"OverflowException\"); end; "
        "try Console.WriteLine(0.0000000000000000000000000001m + (-0.0000000000000000000000000001m)); catch OverflowException => Console.WriteLine(\"OverflowException\"); end; "
        "try Console.WriteLine(0.5m + 0.5m); catch OverflowException => Console.WriteLine(\"OverflowException\"); end; "
        "try Console.WriteLine(9999999999999999999999999999m + 0.0000000000000000000000000001m); catch OverflowException => Console.WriteLine(\"OverflowException\"); end; "
        "try Console.WriteLine(7.9228162514264337593543950335m + 7.9228162514264337593543950335m); catch OverflowException => Console.WriteLine(\"OverflowException\"); end; "
        "try Console.WriteLine(0.0000000000000000000000000003m + 5m); catch OverflowException => Console.WriteLine(\"OverflowException\"); end; "
        "try Console.WriteLine(1.2345678901234567890123456789m + decimal.MaxValue); catch OverflowException => Console.WriteLine(\"OverflowException\"); end; "
        "try Console.WriteLine(decimal.MaxValue + (-decimal.MaxValue)); catch OverflowException => Console.WriteLine(\"OverflowException\"); end; "
        "    return 0; "
        "  end Main; "
        "end C;").stdoutText,
        "0.0000000000000000000000000002\n0\n1\n9999999999999999999999999999\n15.845632502852867518708790067\n5.0000000000000000000000000003\nOverflowException\n0\n");
}

// §6.4 (v0.313): a - b is a + (-b), rounded once, ties to even (same shape
// as the addition rows, with the sign of b flipped).
BEZEL_TEST(decimal_half_even_arith_5147, sub_ties_near_max) {
    BEZEL_EXPECT_EQ(runAot(
        "module M; class C "
        "  public static method Main(): int "
        "try Console.WriteLine(79228162514264337593543950334m - (-0.5m)); catch OverflowException => Console.WriteLine(\"OverflowException\"); end; "
        "try Console.WriteLine(decimal.MaxValue - (-0.5m)); catch OverflowException => Console.WriteLine(\"OverflowException\"); end; "
        "try Console.WriteLine(79228162514264337593543950333m - (-0.5m)); catch OverflowException => Console.WriteLine(\"OverflowException\"); end; "
        "try Console.WriteLine((-decimal.MaxValue) - 0.5m); catch OverflowException => Console.WriteLine(\"OverflowException\"); end; "
        "try Console.WriteLine((-79228162514264337593543950334m) - 0.5m); catch OverflowException => Console.WriteLine(\"OverflowException\"); end; "
        "try Console.WriteLine((-decimal.MaxValue) - 1m); catch OverflowException => Console.WriteLine(\"OverflowException\"); end; "
        "try Console.WriteLine(decimal.MaxValue - (-0.0000000000000000000000000001m)); catch OverflowException => Console.WriteLine(\"OverflowException\"); end; "
        "try Console.WriteLine(decimal.MaxValue - (-7.9228162514264337593543950335m)); catch OverflowException => Console.WriteLine(\"OverflowException\"); end; "
        "try Console.WriteLine(0.0000000000000000000000000001m - 0.0000000000000000000000000001m); catch OverflowException => Console.WriteLine(\"OverflowException\"); end; "
        "try Console.WriteLine(0.5m - 0.5m); catch OverflowException => Console.WriteLine(\"OverflowException\"); end; "
        "try Console.WriteLine(decimal.MaxValue - decimal.MaxValue); catch OverflowException => Console.WriteLine(\"OverflowException\"); end; "
        "try Console.WriteLine(7.9228162514264337593543950335m - 0.0000000000000000000000000001m); catch OverflowException => Console.WriteLine(\"OverflowException\"); end; "
        "    return 0; "
        "  end Main; "
        "end C;").stdoutText,
        "79228162514264337593543950334\nOverflowException\n79228162514264337593543950334\nOverflowException\n-79228162514264337593543950334\nOverflowException\n79228162514264337593543950335\nOverflowException\n0\n0\n0\n7.9228162514264337593543950334\n");
}
