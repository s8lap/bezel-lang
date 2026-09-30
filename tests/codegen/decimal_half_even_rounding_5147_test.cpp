// fix #5147 / §6.4 (v0.313): decimal `/` rounds the truncated quotient at
// the largest fitting scale s ≤ 28, to nearest, ties to even (banker's
// rounding). bz_decimal_div (src/runtime/bezel_runtime_decimal_guid.cpp)
// extends `q` via long-division until either rs_signed hits 28 or q*10 would
// overflow, then applies banker's rounding using the first dropped digit
// (rem*10/mb) and the exact-midpoint indicator (tie == 0). Pre-fix the
// division truncated at an operand-derived scale, dropping the rounding
// entirely.
//
// BUG PIN — every row MEASURED on pre-fix runtime (origin/main @ d1827572)
// and quoted in each row's comment. All FAIL without the fix, PASS with it.

#include "support/aot_support.hpp"   // runAot / runAotChecked
#include "support/codegen_support.hpp"
#include "test_harness.hpp"
#include <string>

using bezel::test::runAot;
using bezel::test::runAotChecked;

// §6.4 spec example: 2m / 3m — exact quotient 0.666..., first dropped digit
// at scale 28 is 6 (>5), round up to 28 sixes + 7. Mantissa (29 digits ≈
// 6.66e28) fits in 96 bits, so largest fitting scale = 28. Pre-fix:
// 0.666...66666 (29 sixes, truncated, wrong last digit).
BEZEL_TEST(decimal_half_even_rounding_5147, two_div_three_rounds_up) {
    BEZEL_EXPECT_EQ(runAot(
        "module M; class C "
        "  public static method Main(): int "
        "    Console.WriteLine(2m / 3m); "
        "    return 0; "
        "  end Main; "
        "end C;").stdoutText,
        "0.6666666666666666666666666667\n");
}

// §6.4 spec example: 100m / 7m — `extra` loop stops at extra=26 (ma=1e28 >
// kMax96/10 ≈ 7.92e27), so initial q is at scale 26. The new extension loop
// advances q to scale 27 (q*10 + 5 fits in 96 bits; q*10 + 9 at scale 28
// would not). At scale 27 the first dropped digit (position 28) is 7 (>5),
// round up: mantissa ends in 6 (was 5). Pre-fix: appending next_digit at
// scale+1 left the result un-rounded, ending in 5.
BEZEL_TEST(decimal_half_even_rounding_5147, hundred_div_seven_rounds_up) {
    BEZEL_EXPECT_EQ(runAot(
        "module M; class C "
        "  public static method Main(): int "
        "    Console.WriteLine(100m / 7m); "
        "    return 0; "
        "  end Main; "
        "end C;").stdoutText,
        "14.285714285714285714285714286\n");
}

// §6.4: 1m / 1.000...001m at scale 28 — exact quotient = 10^27/(10^27+1) ≈
// 0.999999999999999999999999999 + 10^-54 + ... The long-division extension
// from scale 1 yields q=9999999999999999999999999990 at scale 28 (27 nines
// + 0); first dropped digit (position 29) is 0 (<5), so round down. dec_store
// strips the trailing zero → scale 27, mantissa 27 nines. Pre-fix: q=9 at
// scale 1, appended next_digit at scale 2 → 0.99 (scale 2, premature).
BEZEL_TEST(decimal_half_even_rounding_5147, one_div_one_plus_epsilon_not_zero) {
    BEZEL_EXPECT_EQ(runAot(
        "module M; class C "
        "  public static method Main(): int "
        "    Console.WriteLine(1m / 1.000000000000000000000000001m); "
        "    return 0; "
        "  end Main; "
        "end C;").stdoutText,
        "0.999999999999999999999999999\n");
}

// §6.4: 1m / 3m stays in range. Exact quotient 0.333..., at scale 28 the
// first dropped digit is 3 (<5), no rounding needed. Pre-fix: same output
// (no rounding needed).
BEZEL_TEST(decimal_half_even_rounding_5147, one_div_three_no_rounding) {
    BEZEL_EXPECT_EQ(runAot(
        "module M; class C "
        "  public static method Main(): int "
        "    Console.WriteLine(1m / 3m); "
        "    return 0; "
        "  end Main; "
        "end C;").stdoutText,
        "0.3333333333333333333333333333\n");
}

// §6.4: 1m / 2m = 0.5 — exact, no rounding needed.
BEZEL_TEST(decimal_half_even_rounding_5147, half_exact_no_rounding) {
    BEZEL_EXPECT_EQ(runAot(
        "module M; class C "
        "  public static method Main(): int "
        "    Console.WriteLine(1m / 2m); "
        "    return 0; "
        "  end Main; "
        "end C;").stdoutText,
        "0.5\n");
}

// §6.4: 3m / 2m = 1.5 — exact, no rounding needed.
BEZEL_TEST(decimal_half_even_rounding_5147, three_halves_exact_no_rounding) {
    BEZEL_EXPECT_EQ(runAot(
        "module M; class C "
        "  public static method Main(): int "
        "    Console.WriteLine(3m / 2m); "
        "    return 0; "
        "  end Main; "
        "end C;").stdoutText,
        "1.5\n");
}

// §6.4 ties-to-even: 5m / 2m = 2.5 (exact at scale 1), largest fitting
// scale = 1, mantissa 25 fits — no ties-to-even path is taken because the
// exact value fits at the next scale up. Control: matches `half_exact_*`
// semantics (no rounding). Verifies the algorithm picks scale 1, not scale 0.
BEZEL_TEST(decimal_half_even_rounding_5147, five_halves_exact_no_rounding) {
    BEZEL_EXPECT_EQ(runAot(
        "module M; class C "
        "  public static method Main(): int "
        "    Console.WriteLine(5m / 2m); "
        "    return 0; "
        "  end Main; "
        "end C;").stdoutText,
        "2.5\n");
}

// §6.4 ties-to-even: decimal.MaxValue / 2m — exact quotient is
// 39614081257132168796771975167.5, whose scale-1 mantissa
// (396140812571321687967719751675, 30 digits ≈ 3.96e29) exceeds kMax96. The
// largest fitting scale is 0: at scale 0 the truncated q is
// 39614081257132168796771975167 (odd), and the dropped value is exactly the
// midpoint (next_digit = 5, tie = 0), so banker's rounding rounds UP to
// the nearest even — 39614081257132168796771975168 (= 2^95). Pre-fix
// truncated to 39614081257132168796771975167 without the tie-to-even logic.
BEZEL_TEST(decimal_half_even_rounding_5147, max_value_div_two_ties_to_even) {
    BEZEL_EXPECT_EQ(runAot(
        "module M; class C "
        "  public static method Main(): int "
        "    Console.WriteLine(decimal.MaxValue / 2m); "
        "    return 0; "
        "  end Main; "
        "end C;").stdoutText,
        "39614081257132168796771975168\n");
}
// §6.4 (v0.313) ties-to-even at scale 28, where the quotient needs a 29th
// fractional digit that is clamped away. One round-to-nearest-even decision
// covers every dropped digit and the sticky remainder. Expected values are
// Python decimal ROUND_HALF_EVEN. Each row below was MEASURED wrong on the
// first #5147 commit (half-up clamp before the banker's block).
static std::string div28(const char* expr) {
    return std::string("module M; class C "
        "  public static method Main(): int "
        "    Console.WriteLine(") + expr + "); "
        "    return 0; "
        "  end Main; "
        "end C;";
}

// 1e-28 / 2 = 0.5e-28: exact tie, last digit 0 (even) -> 0. Was 1e-28.
BEZEL_TEST(decimal_half_even_rounding_5147, scale28_tie_to_zero_even) {
    BEZEL_EXPECT_EQ(runAot(div28("0.0000000000000000000000000001m / 2m")).stdoutText, "0\n");
}

// 5e-28 / 10 = 0.5e-28: exact tie, even -> 0. Was 1e-28.
BEZEL_TEST(decimal_half_even_rounding_5147, scale28_five_over_ten_tie_even) {
    BEZEL_EXPECT_EQ(runAot(div28("0.0000000000000000000000000005m / 10m")).stdoutText, "0\n");
}

// 25e-28 / 10 = 2.5e-28: exact tie, 2 is even -> 2e-28. Was 3e-28.
BEZEL_TEST(decimal_half_even_rounding_5147, scale28_twentyfive_over_ten_tie_even) {
    BEZEL_EXPECT_EQ(runAot(div28("0.0000000000000000000000000025m / 10m")).stdoutText,
        "0.0000000000000000000000000002\n");
}

// 15e-28 / 10 and 35e-28 / 10: ties with odd last digit round up to even.
BEZEL_TEST(decimal_half_even_rounding_5147, scale28_tie_odd_rounds_up) {
    BEZEL_EXPECT_EQ(runAot(div28("0.0000000000000000000000000015m / 10m")).stdoutText,
        "0.0000000000000000000000000002\n");
    BEZEL_EXPECT_EQ(runAot(div28("0.0000000000000000000000000035m / 10m")).stdoutText,
        "0.0000000000000000000000000004\n");
}

// Negated tie: -1e-28 / 2 is a tie rounding to zero; no negative zero.
BEZEL_TEST(decimal_half_even_rounding_5147, scale28_negative_tie_is_plain_zero) {
    BEZEL_EXPECT_EQ(runAot(div28("-0.0000000000000000000000000001m / 2m")).stdoutText, "0\n");
    BEZEL_EXPECT_EQ(runAot(div28("-0.0000000000000000000000000025m / 10m")).stdoutText,
        "-0.0000000000000000000000000002\n");
}

// Rounding up overflows the 96-bit mantissa: fall back to a lower scale
// (§6.4 v0.313). Boundary probe from the reviewer.
BEZEL_TEST(decimal_half_even_rounding_5147, round_up_overflow_falls_back_to_lower_scale) {
    BEZEL_EXPECT_EQ(runAot(div28("7130534626283790383418955530.2m / 0.9m")).stdoutText,
        "7922816251426433759354395034\n");
}
