// fix #5174 / §4.4 (v0.310): a `decimal` literal and a `decimal.Parse` string
// whose value is NOT in decimal's value set denote that value rounded as §6.4
// rounds an inexact result; the parse path stored scale > 28 unrounded and
// rounded a full-mantissa drop half-up.
//
// Spec reference:
//   Bezel_Language_Spec.md §4.4 (v0.310): "the values are exactly ±m × 10^−s
//   for integers 0 ≤ m ≤ 2^96 − 1 and 0 ≤ s ≤ 28 … A `decimal` literal, a
//   string parsed as `decimal` … whose value is not in this set denote that
//   value rounded as §6.4 rounds an inexact result
//   (`39614081257132168796771975166.5m` is `39614081257132168796771975166`)"
//   Bezel_Language_Spec.md §6.4 (v0.310): "rounded at the largest scale s ≤ 28
//   whose rounded mantissa is at most 2^96 − 1 — to nearest, a tie to the even
//   last digit … a nonzero result that rounds to zero is zero".
//
// BUG SURFACES (dec_parse_src, src/runtime/bezel_runtime_decimal_guid.cpp —
// the engine behind BOTH the compile-time literal fold and decimal.Parse):
//
//   1. No bound on the scale. The digit loop ran `if (in_frac) ++sc;` with no
//      cap, and dec_store reduced only a mantissa above 2^96−1, never a scale
//      above 28. So a 29-fractional-digit string was stored at scale 29 — a
//      representation §4.4 does not admit — and `1e-29m` stored m = 1, s = 29.
//   2. Half-up, not ties-to-even. Both the full-mantissa drop in the digit
//      loop (`dropped_frac_digit >= 5 → m += 1`) and dec_store's overflow
//      reduction (`rem >= 5 → m += 1`) rounded a tie away from even, so the
//      §4.4-named example `39614081257132168796771975166.5m` came out as
//      `39614081257132168796771975167` instead of the specified
//      `39614081257132168796771975166`.
//
// FIX: the parse tail now reduces (m, s) and rounds ONCE through the same
// path decimal arithmetic already uses (dec_store_rounded / dec_store_wide's
// loop shape): drop digits until scale ≤ 28 AND mantissa ≤ 2^96−1, tracking
// the first dropped digit and a sticky bit, then round to nearest with ties to
// even. The digits the digit loop could not store are that first-dropped digit
// and sticky bit, so there is no second rounding.
//
// BUG PIN — the five rows marked [FAILS PRE-FIX] were MEASURED on pre-fix
// staging (r23-c28 @ b3ae4993) and each quotes what the pre-fix build printed;
// every one of them prints a different value than the spec requires, so the
// failure is a wrong value, not a crash. The four CONTROL rows pass both
// before and after, and pin that the shared arithmetic path, the in-range
// literal path and the §4.4 overflow edge are untouched.
//
//   const k := 39614081257132168796771975166.5m  -> "39614081257132168796771975166\n"  (was …167)
//   decimal.Parse("39614081257132168796771975166.5") -> same                        (was …167)
//   decimal.Parse("0.12345678901234567890123456785")  -> "0.1234567890123456789012345678\n" (was scale-29 …785)
//   decimal.Parse("0.00000000000000000000000000001")  -> "0\n"                       (was scale-29 …001)
//   const k := 1e-29m                                  -> "0\n"                       (was scale-29 …0001)
//
//   const c := 1.0000000000000000000000000001m           -> "1.0000000000000000000000000001\n" (control, 28 frac digits in range)
//   decimal.Parse("79228162514264337593543950335.4")     -> "79228162514264337593543950335\n" (control, §4.4: is MaxValue)
//   decimal.Parse("79228162514264337593543950335.5")     -> caught==1                  (control, §4.4: out of range)
//   1m / 3m                                             -> "0.3333333333333333333333333333\n" (control, §6.4 arithmetic untouched)

#include "support/aot_support.hpp"   // runAotChecked
#include "test_harness.hpp"

using bezel::test::runAotChecked;

// §4.4 (v0.310) names this example: "39614081257132168796771975166.5m is
// 39614081257132168796771975166". The literal's 30th digit ('5', the fraction)
// does not fit the 96-bit mantissa, so the parse loop drops it and the tail
// rounds once. Pre-fix the drop was half-up (`dropped_frac_digit >= 5 → m += 1`),
// printing "39614081257132168796771975167". Post-fix it is a true tie
// (dropTop = 5, sticky = false) and the retained last digit 6 is already even,
// so it stays — the spec's value.
BEZEL_TEST(decimal_literal_scale_bound_ties_to_even_5174,
           literal_tie_at_max96_rounds_to_even_spec_4_4_6_4) {
    BEZEL_EXPECT_EQ(runAotChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    const k: decimal := 39614081257132168796771975166.5m; "
        "    Console.WriteLine(k); "
        "    return 0; "
        "  end Main; "
        "end C;").stdoutText,
        "39614081257132168796771975166\n");
}

// §9.3 (v0.310) / §4.4: `decimal.Parse` runs the same `dec_parse_src`, so the
// same string must give the same value as the literal above. Pre-fix both
// printed "39614081257132168796771975167".
BEZEL_TEST(decimal_literal_scale_bound_ties_to_even_5174,
           parse_tie_at_max96_rounds_to_even_spec_4_4_6_4_9_3) {
    BEZEL_EXPECT_EQ(runAotChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    var s: string := \"39614081257132168796771975166.5\"; "
        "    var d: decimal := decimal.Parse(s); "
        "    Console.WriteLine(d); "
        "    return 0; "
        "  end Main; "
        "end C;").stdoutText,
        "39614081257132168796771975166\n");
}

// §4.4 (v0.310): 29 fractional digits is a scale §4.4 does not admit (s ≤ 28),
// so the value is inexact and is rounded per §6.4 at the largest scale ≤ 28.
// Here the whole 29-digit mantissa (1.23e28) FITS in 96 bits, so the only
// reduction needed is the scale one: drop the 29th digit '5'. That is a true
// tie and the retained last digit 8 is even, so it stays —
// 0.1234567890123456789012345678. Pre-fix the scale was never bounded and the
// value was stored and printed at scale 29 ("0.12345678901234567890123456785"),
// a representation §4.4 does not admit.
BEZEL_TEST(decimal_literal_scale_bound_ties_to_even_5174,
           parse_scale_29_fits_mantissa_rounds_to_28_spec_4_4_6_4_9_3) {
    BEZEL_EXPECT_EQ(runAotChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    var s: string := \"0.12345678901234567890123456785\"; "
        "    var d: decimal := decimal.Parse(s); "
        "    Console.WriteLine(d); "
        "    return 0; "
        "  end Main; "
        "end C;").stdoutText,
        "0.1234567890123456789012345678\n");
}

// §4.4 (v0.310) / §6.4 ("a nonzero result that rounds to zero is zero"):
// 0.00000000000000000000000000001 is 1e-29, whose nearest values in decimal's
// set are 0 and 1e-28 — exactly halfway, a tie. Ties go to the even last
// digit, and 0's last digit is 0, so the result is 0. Pre-fix the scale was
// never bounded and the value printed at scale 29 as
// "0.00000000000000000000000000001".
BEZEL_TEST(decimal_literal_scale_bound_ties_to_even_5174,
           parse_1e_minus_29_ties_to_even_zero_spec_4_4_6_4_9_3) {
    BEZEL_EXPECT_EQ(runAotChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    var s: string := \"0.00000000000000000000000000001\"; "
        "    var d: decimal := decimal.Parse(s); "
        "    Console.WriteLine(d); "
        "    return 0; "
        "  end Main; "
        "end C;").stdoutText,
        "0\n");
}

// §4.4 (v0.310): the same scale bound reached through the LITERAL grammar's
// exponent (stdlib §9.3's Decimal.Parse admits no exponent, so only a literal
// spells this value). `1e-29m` is m = 1, s = 29; reducing the scale to 28
// drops the digit '1' (< 5, so round down) to m = 0. §6.4: "a nonzero result
// that rounds to zero is zero." Pre-fix the literal was stored at scale 29 and
// printed "0.0000000000000000000000000001".
BEZEL_TEST(decimal_literal_scale_bound_ties_to_even_5174,
           literal_exponent_scale_29_rounds_to_zero_spec_4_4_6_4) {
    BEZEL_EXPECT_EQ(runAotChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    const k: decimal := 1e-29m; "
        "    Console.WriteLine(k); "
        "    return 0; "
        "  end Main; "
        "end C;").stdoutText,
        "0\n");
}

// §4.4 (v0.310) CONTROL: 28 fractional digits is IN decimal's set (s = 28), so
// this literal is exact and must fold unchanged. The post-fix code's scale
// reduction loop must not run for it (the loop condition is s > 28), which is
// what keeps this row non-vacuous: it fails if the reduction is off by one.
BEZEL_TEST(decimal_literal_scale_bound_ties_to_even_5174,
           twenty_eight_frac_digits_in_range_unchanged_spec_4_4) {
    BEZEL_EXPECT_EQ(runAotChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    const c: decimal := 1.0000000000000000000000000001m; "
        "    Console.WriteLine(c); "
        "    return 0; "
        "  end Main; "
        "end C;").stdoutText,
        "1.0000000000000000000000000001\n");
}

// §4.4 (v0.310) CONTROL: "79228162514264337593543950335.4m is MaxValue". This
// is the exact-overflow edge the post-fix rounding must NOT disturb: the 30th
// digit '4' is dropped for the same reason as the tie rows above, but being
// below half it rounds DOWN to exactly 2^96 − 1 at scale 0 — MaxValue, not an
// OverflowException.
BEZEL_TEST(decimal_literal_scale_bound_ties_to_even_5174,
           parse_maxvalue_boundary_rounds_down_to_max_spec_4_4_6_4_9_3) {
    BEZEL_EXPECT_EQ(runAotChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    var s: string := \"79228162514264337593543950335.4\"; "
        "    var d: decimal := decimal.Parse(s); "
        "    Console.WriteLine(d); "
        "    return 0; "
        "  end Main; "
        "end C;").stdoutText,
        "79228162514264337593543950335\n");
}

// §4.4 (v0.310) CONTROL: "79228162514264337593543950335.5m, a tie rounding to
// the even 2^96, is out of range." The retained mantissa 2^96 − 1 is odd, so
// the tie rounds UP to 2^96, which does not fit at scale 0 — OverflowException.
// This row pins that the new ties-to-even rounding did not turn the tie rows'
// round-down into a round-up that silently fits.
BEZEL_TEST(decimal_literal_scale_bound_ties_to_even_5174,
           parse_tie_to_2_pow_96_is_out_of_range_spec_4_4_6_4_9_3) {
    BEZEL_EXPECT_EQ(runAotChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    var s: string := \"79228162514264337593543950335.5\"; "
        "    var caught := 0; "
        "    try "
        "      var d: decimal := decimal.Parse(s); "
        "      Console.WriteLine(\"no-throw:\" + d.ToString()); "
        "    catch OverflowException => caught := 1; end; "
        "    Console.WriteLine(caught); "
        "    return 0; "
        "  end Main; "
        "end C;").stdoutText,
        "1\n");
}

// §6.4 (v0.310) CONTROL: "1m / 3m is 0.3333333333333333333333333333". Division
// routes through dec_store_wide/dec_store_rounded, which the fix leaves
// untouched; this row is the guard that the parse path's new tail did not
// change the shared rounding the arithmetic path already had.
BEZEL_TEST(decimal_literal_scale_bound_ties_to_even_5174,
           arithmetic_one_div_three_unchanged_spec_6_4) {
    BEZEL_EXPECT_EQ(runAotChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    Console.WriteLine(1m / 3m); "
        "    return 0; "
        "  end Main; "
        "end C;").stdoutText,
        "0.3333333333333333333333333333\n");
}
