// fix #4870 / §6.4 (v0.307): runtime decimal division kept ~1 significant
// digit, and missed OverflowException, when the dividend's mantissa is near
// 2^96 - 1 and the divisor has a large scale.
//
// bz_decimal_div (src/runtime/bezel_runtime_decimal_guid.cpp) pre-scales the
// dividend by 10^extra while it fits 96 bits, divides, and lands at net scale
// (sa + extra) - sb. With a dividend near MaxValue `extra` stays 0, so a
// large-scale divisor leaves a negative net scale; the code then multiplied
// the integer quotient by 10 per missing digit, APPENDING ZEROS where the true
// digits belong. decimal.MaxValue / 1.0000000000000000000000000001m printed
// 70000000000000000000000000000, and decimal.MaxValue /
// 0.9999999999999999999999999999m printed the same value instead of raising
// OverflowException (§6.4: "decimal arithmetic raises OverflowException on
// overflow"). The fix generates each missing digit by long division on the
// remainder (exact truncation at scale 0) and raises OverflowException iff
// the truncated quotient exceeds MaxValue. It also clamps the one path that
// produced a scale-29 result (sa + extra == 29 with sb == 0).
//
// The `/` rounding convention stays TRUNCATION (§6.4 states no rounding
// mode), and every division whose net scale was already in 0..28 is
// byte-identical before and after.
//
// BUG PIN — every row except the two `_unchanged` controls FAILS on the
// pre-fix runtime (value MEASURED on e47e35f2 and quoted in each row's
// comment) and PASSES with the fix.

#include "support/aot_support.hpp"   // runAot / runAotChecked
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

// §6.4, #4870: decimal.MaxValue / 1.0000000000000000000000000001m -> 79228162514264337593543950327
// issue repro 1: true quotient ~7.92281625142643375935439503272e28 is in range; pre-fix printed 70000000000000000000000000000 (one significant digit + appended zeros).
BEZEL_TEST(decimal_div_precision_4870, max_value_div_just_above_one_keeps_all_digits) {
    BEZEL_EXPECT_EQ(runAotChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    var a: decimal := decimal.MaxValue; "
        "    var b: decimal := 1.0000000000000000000000000001m; "
        "    try "
        "      Console.WriteLine(a / b); "
        "    catch OverflowException => Console.WriteLine(\"ove\"); end; "
        "    return 0; "
        "  end Main; "
        "end C;").stdoutText,
        "79228162514264337593543950327\n");
}

// §6.4, #4870: decimal.MaxValue / 0.9999999999999999999999999999m -> ove
// issue repro 2: true quotient ~7.92281625142643375935439503429e28 exceeds MaxValue; pre-fix printed 70000000000000000000000000000 instead of raising.
BEZEL_TEST(decimal_div_precision_4870, max_value_div_just_below_one_raises_overflow) {
    BEZEL_EXPECT_EQ(runAotChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    var a: decimal := decimal.MaxValue; "
        "    var b: decimal := 0.9999999999999999999999999999m; "
        "    try "
        "      Console.WriteLine(a / b); "
        "    catch OverflowException => Console.WriteLine(\"ove\"); end; "
        "    return 0; "
        "  end Main; "
        "end C;").stdoutText,
        "ove\n");
}

// §6.4, #4870: decimal.MaxValue / -1.0000000000000000000000000001m -> -79228162514264337593543950327
// sign handling of repro 1; pre-fix printed -70000000000000000000000000000.
BEZEL_TEST(decimal_div_precision_4870, max_value_div_negative_just_above_one) {
    BEZEL_EXPECT_EQ(runAotChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    var a: decimal := decimal.MaxValue; "
        "    var b: decimal := -1.0000000000000000000000000001m; "
        "    try "
        "      Console.WriteLine(a / b); "
        "    catch OverflowException => Console.WriteLine(\"ove\"); end; "
        "    return 0; "
        "  end Main; "
        "end C;").stdoutText,
        "-79228162514264337593543950327\n");
}

// §6.4, #4870: -79228162514264337593543950335m / 0.9999999999999999999999999999m -> ove
// negative dividend of repro 2; pre-fix printed -70000000000000000000000000000.
BEZEL_TEST(decimal_div_precision_4870, min_value_div_just_below_one_raises_overflow) {
    BEZEL_EXPECT_EQ(runAotChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    var a: decimal := -79228162514264337593543950335m; "
        "    var b: decimal := 0.9999999999999999999999999999m; "
        "    try "
        "      Console.WriteLine(a / b); "
        "    catch OverflowException => Console.WriteLine(\"ove\"); end; "
        "    return 0; "
        "  end Main; "
        "end C;").stdoutText,
        "ove\n");
}

// §6.4, #4870: decimal.MaxValue / 1.1m -> 72025602285694852357767227577
// one-digit deficit filled by a true digit; pre-fix appended 0 (…570).
BEZEL_TEST(decimal_div_precision_4870, max_value_div_one_point_one) {
    BEZEL_EXPECT_EQ(runAotChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    var a: decimal := decimal.MaxValue; "
        "    var b: decimal := 1.1m; "
        "    try "
        "      Console.WriteLine(a / b); "
        "    catch OverflowException => Console.WriteLine(\"ove\"); end; "
        "    return 0; "
        "  end Main; "
        "end C;").stdoutText,
        "72025602285694852357767227577\n");
}

// §6.4, #4870: 39614081257132168796771975167m / 0.5m -> 79228162514264337593543950334
// exact quotient 2*39614081257132168796771975167; pre-fix printed …330.
BEZEL_TEST(decimal_div_precision_4870, half_max_div_half_in_range) {
    BEZEL_EXPECT_EQ(runAotChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    var a: decimal := 39614081257132168796771975167m; "
        "    var b: decimal := 0.5m; "
        "    try "
        "      Console.WriteLine(a / b); "
        "    catch OverflowException => Console.WriteLine(\"ove\"); end; "
        "    return 0; "
        "  end Main; "
        "end C;").stdoutText,
        "79228162514264337593543950334\n");
}

// §6.4, #4870: 39614081257132168796771975168m / 0.5m -> ove
// exact quotient 2^96 = MaxValue + 1; pre-fix printed 79228162514264337593543950330 instead of raising.
BEZEL_TEST(decimal_div_precision_4870, half_max_plus_one_div_half_raises_overflow) {
    BEZEL_EXPECT_EQ(runAotChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    var a: decimal := 39614081257132168796771975168m; "
        "    var b: decimal := 0.5m; "
        "    try "
        "      Console.WriteLine(a / b); "
        "    catch OverflowException => Console.WriteLine(\"ove\"); end; "
        "    return 0; "
        "  end Main; "
        "end C;").stdoutText,
        "ove\n");
}

// §6.4, #4870: 0.0000000000000000000000000001m / 3m -> 0
// exact 3.3e-29 truncated at scale 28 is 0; pre-fix produced a scale-29 value printing 0.00000000000000000000000000003.
BEZEL_TEST(decimal_div_precision_4870, tiny_div_three_clamps_to_scale_28) {
    BEZEL_EXPECT_EQ(runAotChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    var a: decimal := 0.0000000000000000000000000001m; "
        "    var b: decimal := 3m; "
        "    try "
        "      Console.WriteLine(a / b); "
        "    catch OverflowException => Console.WriteLine(\"ove\"); end; "
        "    return 0; "
        "  end Main; "
        "end C;").stdoutText,
        "0\n");
}

// §6.4, #4870: 0.1m / 3m -> 0.0333333333333333333333333333
// pre-fix produced a scale-29 value printing 29 fractional digits.
BEZEL_TEST(decimal_div_precision_4870, tenth_div_three_clamps_to_scale_28) {
    BEZEL_EXPECT_EQ(runAotChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    var a: decimal := 0.1m; "
        "    var b: decimal := 3m; "
        "    try "
        "      Console.WriteLine(a / b); "
        "    catch OverflowException => Console.WriteLine(\"ove\"); end; "
        "    return 0; "
        "  end Main; "
        "end C;").stdoutText,
        "0.0333333333333333333333333333\n");
}

// §6.4 (v0.309) / fix #5147: decimal.MaxValue / 10m — same value as the
// non-Checked row in decimal_div_overflow_4864: at scale 1, the exact
// quotient 7922816251426433759354395033.5 fits (mantissa = MaxValue), so
// the algorithm extends by one long-division step and keeps the .5.
BEZEL_TEST(decimal_div_precision_4870, max_value_div_ten_unchanged) {
    BEZEL_EXPECT_EQ(runAotChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    var a: decimal := decimal.MaxValue; "
        "    var b: decimal := 10m; "
        "    try "
        "      Console.WriteLine(a / b); "
        "    catch OverflowException => Console.WriteLine(\"ove\"); end; "
        "    return 0; "
        "  end Main; "
        "end C;").stdoutText,
        "7922816251426433759354395033.5\n");
}

// §6.4, #4870: decimal.MaxValue / 1.5m -> 52818775009509558395695966890
// control: one-digit deficit whose true digit is 0 — identical before and after.
BEZEL_TEST(decimal_div_precision_4870, max_value_div_one_point_five_unchanged) {
    BEZEL_EXPECT_EQ(runAotChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    var a: decimal := decimal.MaxValue; "
        "    var b: decimal := 1.5m; "
        "    try "
        "      Console.WriteLine(a / b); "
        "    catch OverflowException => Console.WriteLine(\"ove\"); end; "
        "    return 0; "
        "  end Main; "
        "end C;").stdoutText,
        "52818775009509558395695966890\n");
}
