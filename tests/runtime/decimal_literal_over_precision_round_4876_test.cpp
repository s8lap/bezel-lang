// fix #4876 / §4.3 (v0.307): a `decimal` literal whose mantissa exceeds the
// 96-bit limit must round the dropped fractional digit half-up — matching the
// runtime's `dec_store` overflow-reduction path (`rem >= 5 → m += 1`).
//
// Pre-fix the parser silently truncated an over-precision literal's
// fractional part: `39614081257132168796771975167.5m` (the literal form of the
// issue's repro) folded to `39614081257132168796771975167`, while runtime
// arithmetic on the same operands (`39614081257132168796771975167m + 0.5m`)
// correctly produced `39614081257132168796771975168`. The two paths disagreed
// at a midpoint. §4.3 ("A floating-point literal denotes the nearest
// representable value of its type … The same rule bounds decimal literals
// against decimal's range") supports rounding over truncation.
//
// BUG PIN — the three EXACT-VALUE rows below all evaluate to
// `39614081257132168796771975168\n` post-fix; pre-fix the fold and Parse rows
// printed `39614081257132168796771975167\n` (the truncated form), failing.
// The runtime row was already correct (control).
//
//   const k := 39614081257132168796771975167.5m   -> "39614081257132168796771975168\n"  (fold; was truncated pre-fix)
//   d + 0.5m (d := 39614081257132168796771975167m) -> same as above (control: unchanged)
//   decimal.Parse("39614081257132168796771975167.5") -> same as above (Parse; was truncated pre-fix)
//
//   1.5m                                       -> "1.5\n"                                (in-range; no regression)
//   0m                                         -> "0\n"                                  (zero in, zero out)
//   1.0000000000000000000000000001m            -> "1.0000000000000000000000000001\n"     (28 frac digits — §4.4 boundary)
//   decimal.Parse("1.5")                       -> "1.5\n"                                (Parse in-range; no regression)

#include "support/aot_support.hpp"   // runAotChecked
#include "test_harness.hpp"

using bezel::test::runAotChecked;

// §4.3 (v0.307) / #4876: the literal-fold path is the bug's primary surface.
// A `const` decimal literal whose dropped frac digit is `5` must round up to
// the nearest representable value, not silently truncate.
BEZEL_TEST(decimal_literal_over_precision_round_4876,
           decimal_literal_over_precision_rounds_half_up_spec_4_3) {
    BEZEL_EXPECT_EQ(runAotChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    const k: decimal := 39614081257132168796771975167.5m; "
        "    Console.WriteLine(k); "
        "    return 0; "
        "  end Main; "
        "end C;").stdoutText,
        "39614081257132168796771975168\n");
}

// §4.3 (v0.307) / #4876: control — runtime `d + 0.5m` already produces the
// correct value via `dec_store`. The post-fix code must keep it passing and
// the two paths must agree on the same operands.
BEZEL_TEST(decimal_literal_over_precision_round_4876,
           runtime_plus_half_matches_fold_spec_4_3_6_4) {
    BEZEL_EXPECT_EQ(runAotChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    var d: decimal := 39614081257132168796771975167m; "
        "    var k: decimal := d + 0.5m; "
        "    Console.WriteLine(k); "
        "    return 0; "
        "  end Main; "
        "end C;").stdoutText,
        "39614081257132168796771975168\n");
}

// §4.3 (v0.307) / #4876: the same `dec_parse_src` backs both literal-fold and
// runtime `decimal.Parse`. The post-fix Parse path must round half-up on the
// same over-precision strings (consistent with the fold path).
BEZEL_TEST(decimal_literal_over_precision_round_4876,
           decimal_parse_over_precision_rounds_half_up_spec_4_3_9_3) {
    BEZEL_EXPECT_EQ(runAotChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    var s: string := \"39614081257132168796771975167.5\"; "
        "    var d: decimal := decimal.Parse(s); "
        "    Console.WriteLine(d); "
        "    return 0; "
        "  end Main; "
        "end C;").stdoutText,
        "39614081257132168796771975168\n");
}

// §4.4 (v0.307): control — a normal in-range literal folds unchanged. The
// post-fix code must not perturb the common path.
BEZEL_TEST(decimal_literal_over_precision_round_4876,
           one_point_five_literal_unchanged_spec_4_4) {
    BEZEL_EXPECT_EQ(runAotChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    const a: decimal := 1.5m; "
        "    Console.WriteLine(a); "
        "    return 0; "
        "  end Main; "
        "end C;").stdoutText,
        "1.5\n");
}

// §4.4 (v0.307): control — zero in, zero out. The post-fix code's half-up
// path must not produce negative zero on a dropped frac digit after m += 1.
BEZEL_TEST(decimal_literal_over_precision_round_4876,
           zero_literal_unchanged_spec_4_4) {
    BEZEL_EXPECT_EQ(runAotChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    const b: decimal := 0m; "
        "    Console.WriteLine(b); "
        "    return 0; "
        "  end Main; "
        "end C;").stdoutText,
        "0\n");
}

// §4.4 (v0.307): control — a literal with 28 fractional digits fits at
// scale 28 (the §4.4 limit) and folds unchanged; the post-fix code's
// half-up branch is NOT taken (m never reaches the 96-bit limit at this
// magnitude).
BEZEL_TEST(decimal_literal_over_precision_round_4876,
           twenty_eight_frac_digit_literal_unchanged_spec_4_4) {
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

// §9.3 (v0.307): control — `decimal.Parse` on an in-range string is
// unchanged. The post-fix code must not perturb the common Parse path.
BEZEL_TEST(decimal_literal_over_precision_round_4876,
           decimal_parse_one_point_five_unchanged_spec_9_3) {
    BEZEL_EXPECT_EQ(runAotChecked(
        "module M; class C "
        "  public static method Main(): int "
        "    var s: string := \"1.5\"; "
        "    var d: decimal := decimal.Parse(s); "
        "    Console.WriteLine(d); "
        "    return 0; "
        "  end Main; "
        "end C;").stdoutText,
        "1.5\n");
}
