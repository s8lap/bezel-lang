// fix #4845 / §5.2 (v0.307): `decimal(x)` for binary-float x must round to
// the nearest representable decimal value with scale ≤ 28 (§4.4). Pre-fix
// the implementation formatted the double with `%.15g` and round-tripped it
// through `bz_decimal_parse_cstr`, which admits any exponent — so a value
// like `1e-40` produced a decimal with scale 40 (over the §4.4 limit),
// silently violating the nearest-representable contract.
//
// BUG PIN — the four rows below. The `1e-40` row FAILS (wrong scale-40
// decimal, or no output) on pre-fix bezelc and PASSES ("0" output) with
// the fix. The other three are in-range controls that must still produce
// a sensible decimal.
//
//   decimal(1e-40)        -> "0"           (rounds to zero, §5.2 nearest-rep)
//   decimal(0.1)          -> "0.1"         (no regression on common path)
//   decimal(1.0e30)       -> OverflowException  (already enforced above)
//   decimal(0.0)          -> "0"           (zero in, zero out)

#include "support/aot_support.hpp"   // runAotChecked
#include "test_harness.hpp"

using bezel::test::runAotChecked;

// §5.2 (v0.307): `decimal(1e-40)` must round to the nearest representable
// decimal value. 1e-40 is much smaller than 1e-28 (decimal's smallest
// non-zero representable magnitude), so the nearest representable value is
// zero. Pre-fix this produced a decimal with scale 40 (silently over the
// §4.4 scale limit) and the printed form was 40 fractional digits long.
BEZEL_TEST(decimal_double_subnormal_scale_4845,
           decimal_from_subnormal_double_rounds_to_zero_spec_5_2) {
    BEZEL_EXPECT_EQ(runAotChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    var d: decimal := decimal(1e-40); "
        "    Console.WriteLine(d); "
        "    return 0; "
        "  end Main; "
        "end C;").stdoutText,
        "0\n");
}

// §5.2: control — a normal in-range double must still round-trip without
// disturbance. Pre-fix this passed; the post-fix code must keep it passing.
BEZEL_TEST(decimal_double_subnormal_scale_4845,
           decimal_from_zero_one_unchanged_spec_5_2) {
    BEZEL_EXPECT_EQ(runAotChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    var d: decimal := decimal(0.1); "
        "    Console.WriteLine(d); "
        "    return 0; "
        "  end Main; "
        "end C;").stdoutText,
        "0.1\n");
}

// §5.2: control — a magnitude outside decimal's range (1.0e30 > MaxValue ≈
// 7.9e28) must continue to raise OverflowException. The existing overflow
// guard is above the fix, so this is a regression guard for the fix itself.
// NOTE: must come from a runtime variable, not a constant — per fix #4801
// (v0.307), §17.14 rejects a constant operand that would fault the
// conversion at compile time.
BEZEL_TEST(decimal_double_subnormal_scale_4845,
           decimal_from_out_of_range_double_still_raises_overflow_spec_5_2_6_4) {
    BEZEL_EXPECT_EQ(runAotChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    var big := 1.0e30; "
        "    var caught := 0; "
        "    try "
        "      var d: decimal := decimal(big); "
        "      Console.WriteLine(\"no-throw:\" + d.ToString()); "
        "    catch OverflowException => caught := 1; end; "
        "    Console.WriteLine(caught); "
        "    return 0; "
        "  end Main; "
        "end C;").stdoutText,
        "1\n");
}

// §5.2: control — zero in, zero out. The post-fix rounding-to-zero must
// not produce negative zero.
BEZEL_TEST(decimal_double_subnormal_scale_4845,
           decimal_from_zero_double_is_zero_spec_5_2) {
    BEZEL_EXPECT_EQ(runAotChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    var d: decimal := decimal(0.0); "
        "    Console.WriteLine(d); "
        "    return 0; "
        "  end Main; "
        "end C;").stdoutText,
        "0\n");
}
