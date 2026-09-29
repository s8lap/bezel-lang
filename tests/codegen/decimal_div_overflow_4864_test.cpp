// fix #4864 / §6.4 (v0.307): decimal division whose exact quotient exceeds
// decimal's representable range returned a wrong finite value instead of
// raising OverflowException.
//
// bz_decimal_div (src/runtime/bezel_runtime_decimal_guid.cpp) computes an
// integer quotient `q` at a net scale `rs_signed = (sa + extra) - sb`. When
// `rs_signed` comes out negative, the result needs a scale below 0 (not
// representable — a decimal's scale is 0..28), so the code tries to absorb
// the deficit by multiplying `q` by 10 (and incrementing rs_signed) while
// that stays within the 96-bit mantissa. Before this fix, if `rs_signed` was
// STILL negative once that upscaling loop stopped (because the true quotient
// needs more digits than 96 bits can hold at scale 0), the code stored the
// partially-upscaled `q` at scale 0 anyway and returned — silently producing
// a wrong finite value — instead of raising OverflowException like every
// other overflow path in the file (dec_store, bz_decimal_mul, the `to_*`
// conversions) does via bz_set_exc.
//
// BUG PIN — these three rows FAIL (wrong finite value, no exception) without
// the fix and PASS (OverflowException caught) with it. MEASURED on
// pre-fix bezelc (this exact repro, from the issue):
//   decimal.MaxValue / 0.0000000000000000000000000001m -> 79228162514264337593543950335
//   decimal.MaxValue / -0.4m                            -> -19807040628566084398385987583
//   decimal.MaxValue / 10m / 0.01m                      -> 79228162514264337593543950330

#include "support/aot_support.hpp"   // runAot / runAotChecked
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

// §6.4, #4864: decimal.MaxValue / 1e-28m — the exact quotient
// (decimal.MaxValue * 10^28) vastly exceeds the 96-bit mantissa at scale 0,
// so this must raise OverflowException. Pre-fix this printed the wrong
// finite value 79228162514264337593543950335 (decimal.MaxValue itself,
// unchanged) instead of raising.
BEZEL_TEST(decimal_div_overflow_4864, max_value_div_tiny_divisor_raises_overflow) {
    BEZEL_EXPECT_EQ(runAotChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    var a: decimal := decimal.MaxValue; "
        "    var b: decimal := 0.0000000000000000000000000001m; "
        "    try "
        "      Console.WriteLine(a / b); "
        "    catch OverflowException => Console.WriteLine(\"ove1\"); end; "
        "    return 0; "
        "  end Main; "
        "end C;").stdoutText,
        "ove1\n");
}

// §6.4, #4864: decimal.MaxValue / -0.4m — the exact quotient's magnitude
// (decimal.MaxValue * 2.5) exceeds decimal's range and must raise
// OverflowException. Pre-fix this printed the wrong finite value
// -19807040628566084398385987583 instead of raising.
BEZEL_TEST(decimal_div_overflow_4864, max_value_div_fractional_divisor_raises_overflow) {
    BEZEL_EXPECT_EQ(runAotChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    var a: decimal := decimal.MaxValue; "
        "    var n: decimal := -0.4m; "
        "    try "
        "      Console.WriteLine(a / n); "
        "    catch OverflowException => Console.WriteLine(\"ove2\"); end; "
        "    return 0; "
        "  end Main; "
        "end C;").stdoutText,
        "ove2\n");
}

// §6.4, #4864: chained division — decimal.MaxValue / 10m / 0.01m — the final
// division's exact quotient again exceeds decimal's range and must raise
// OverflowException. Pre-fix this printed the wrong finite value
// 79228162514264337593543950330 instead of raising.
BEZEL_TEST(decimal_div_overflow_4864, chained_division_raises_overflow) {
    BEZEL_EXPECT_EQ(runAotChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    var a: decimal := decimal.MaxValue; "
        "    var t: decimal := 10m; "
        "    try "
        "      Console.WriteLine(a / t / 0.01m); "
        "    catch OverflowException => Console.WriteLine(\"ove3\"); end; "
        "    return 0; "
        "  end Main; "
        "end C;").stdoutText,
        "ove3\n");
}

// ---------------------------------------------------------------------------
// Boundary/in-range regression rows: the fix must not disturb divisions whose
// net scale (rs_signed in bz_decimal_div) is already >= 0, or that succeed by
// upscaling `q` all the way to rs_signed == 0. MEASURED on both pre-fix and
// post-fix bezelc (unchanged by the fix, since none of these ever enter the
// `rs_signed < 0` branch touched here).
// ---------------------------------------------------------------------------

// §6.4: decimal.MaxValue / 1m is exactly decimal.MaxValue (rs_signed == 0,
// no rescale needed).
BEZEL_TEST(decimal_div_overflow_4864, max_value_div_one_in_range) {
    BEZEL_EXPECT_EQ(runAot(
        "module M; class C "
        "  public static method Main(): int "
        "    var a: decimal := decimal.MaxValue; "
        "    Console.WriteLine(a / 1m); "
        "    return 0; "
        "  end Main; "
        "end C;").stdoutText,
        "79228162514264337593543950335\n");
}

// §6.4: decimal.MaxValue / 10m stays in range (rs_signed == 0; the quotient
// truncates toward zero, dropping the exact ".5" — a pre-existing, unrelated
// precision characteristic of this path, not a regression from this fix).
BEZEL_TEST(decimal_div_overflow_4864, max_value_div_ten_in_range) {
    BEZEL_EXPECT_EQ(runAot(
        "module M; class C "
        "  public static method Main(): int "
        "    var a: decimal := decimal.MaxValue; "
        "    Console.WriteLine(a / 10m); "
        "    return 0; "
        "  end Main; "
        "end C;").stdoutText,
        "7922816251426433759354395033\n");
}

// §6.4: 1m / 3m stays in range (rs_signed == 28, upscaled entirely via
// `extra`, never touches the post-division rescale branch this fix changes).
BEZEL_TEST(decimal_div_overflow_4864, one_div_three_in_range) {
    BEZEL_EXPECT_EQ(runAot(
        "module M; class C "
        "  public static method Main(): int "
        "    Console.WriteLine(1m / 3m); "
        "    return 0; "
        "  end Main; "
        "end C;").stdoutText,
        "0.3333333333333333333333333333\n");
}

// §6.4: decimal.MaxValue / -1m stays in range (rs_signed == 0; sign flips,
// magnitude unchanged).
BEZEL_TEST(decimal_div_overflow_4864, max_value_div_negative_one_in_range) {
    BEZEL_EXPECT_EQ(runAot(
        "module M; class C "
        "  public static method Main(): int "
        "    var a: decimal := decimal.MaxValue; "
        "    Console.WriteLine(a / -1m); "
        "    return 0; "
        "  end Main; "
        "end C;").stdoutText,
        "-79228162514264337593543950335\n");
}
