// Codegen tests for fix #521: 128-bit decimal type (§4.4, stdlib §9.3).
//
// Spec reference: Bezel_Language_Spec.md §4.4 (decimal literal, precision,
//   arithmetic, DivideByZeroException), §5.1 (int→decimal widening),
//   §6.1 (decimal operators), §6.4 (overflow/divide-by-zero).
// Stdlib reference: Bezel_Standard_Library.md §9.3 (Decimal record).
//
// Implementation: .NET-compatible 128-bit layout:
//   { uint32 lo32, uint32 mid32, uint32 hi32, uint32 flags }
//   96-bit mantissa; flags bit 31 = sign, bits 16-23 = scale (0-28).
//
// Test groups:
//   521_literal     — m/M suffix produces decimal (not double); literals accepted
//   521_arith       — exact arithmetic (+,-,*,/,mod); carries beyond double precision
//   521_divzero     — decimal/0 raises DivideByZeroException (§6.4)
//   521_cmp         — comparison operators (<, <=, >, >=, =, <>)
//   521_widen       — int→decimal widening per §5.1
//   521_static      — static decimal field with m-literal initializer
//   521_stdlib      — Decimal.Parse, TryParse, ToString, CompareTo, Round (§9.3)
//   521_consts      — Decimal.Zero, One, MinValue, MaxValue (§9.3)

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>

#include "driver/Driver.hpp"
#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// Check that the program produces a sema error (decimal type is rejected by sema).
bool hasErrors(const std::string& program) {
    auto r = runAot(program);
    return r.diagnostics.hasErrors();
}

std::string prog(const std::string& mainBody) {
    return "module M; "
           "class C public static method Main(): int "
           + mainBody +
           " end Main; end C;";
}

} // namespace

// ---------------------------------------------------------------------------
// 521_literal: §4.4 — the `m`/`M` suffix produces a decimal literal, NOT
// double. The literal is now fully implemented (128-bit bz_decimal_t).
// ---------------------------------------------------------------------------

// §4.4: 1.5m is a decimal literal. Compare against itself to verify it compiles
// and the result is treated as decimal (not double, which would allow
// double + decimal — that must still be rejected).
BEZEL_TEST(codegen_521_literal, m_literal_compiles_and_runs_spec_4_4) {
    // If decimal literal compiled, the program can run. Return 1 on success.
    BEZEL_EXPECT_EQ(run(prog(
        "var d := 1.5m; "
        "if d = 1.5m then return 1; end; "
        "return 0; ")), 1);
}

// §4.4: upper-case M suffix is also a decimal literal.
BEZEL_TEST(codegen_521_literal, M_suffix_upper_compiles_spec_4_4) {
    BEZEL_EXPECT_EQ(run(prog(
        "var d := 2.0M; "
        "if d = 2.0m then return 1; end; "
        "return 0; ")), 1);
}

// §4.4 / §5.1 / §6.2: assigning a decimal literal to a `double` variable
// must be rejected by sema (no implicit decimal → double conversion per §5.1).
BEZEL_TEST(codegen_521_literal, decimal_literal_rejected_as_double_spec_5_1) {
    BEZEL_EXPECT_TRUE(hasErrors(prog(
        "var d: double := 1.5m; "
        "return 0; ")));
}

// §4.4: 0m (zero) decimal literal.
BEZEL_TEST(codegen_521_literal, zero_m_literal_spec_4_4) {
    BEZEL_EXPECT_EQ(run(prog(
        "var z := 0m; "
        "if z = 0m then return 1; end; "
        "return 0; ")), 1);
}

// ---------------------------------------------------------------------------
// 521_arith: §6.1 — decimal arithmetic is exact (no floating-point error).
// The key invariant: 0.1m + 0.2m = 0.3m EXACTLY (contrast with double).
// ---------------------------------------------------------------------------

// §4.4 / §6.1: 0.1m + 0.2m = 0.3m EXACTLY.
// With double: 0.1 + 0.2 != 0.3 due to IEEE 754 representation error.
// With decimal: the computation is exact.
BEZEL_TEST(codegen_521_arith, add_exact_0_1_plus_0_2_spec_4_4_6_1) {
    BEZEL_EXPECT_EQ(run(prog(
        "var a := 0.1m; "
        "var b := 0.2m; "
        "var c := a + b; "
        "if c = 0.3m then return 1; end; "
        "return 0; ")), 1);
}

// §6.1: subtraction is exact.
BEZEL_TEST(codegen_521_arith, sub_exact_spec_6_1) {
    BEZEL_EXPECT_EQ(run(prog(
        "var a := 1.0m; "
        "var b := 0.1m; "
        "var c := a - b; "
        "if c = 0.9m then return 1; end; "
        "return 0; ")), 1);
}

// §6.1: multiplication is exact.
BEZEL_TEST(codegen_521_arith, mul_exact_spec_6_1) {
    BEZEL_EXPECT_EQ(run(prog(
        "var a := 3.0m; "
        "var b := 4.0m; "
        "var c := a * b; "
        "if c = 12.0m then return 1; end; "
        "return 0; ")), 1);
}

// §6.1 / §4.4: integer division with remainder — quotient is exact.
BEZEL_TEST(codegen_521_arith, div_integer_spec_6_1) {
    BEZEL_EXPECT_EQ(run(prog(
        "var a := 10.0m; "
        "var b := 4.0m; "
        "var c := a / b; "
        "if c = 2.5m then return 1; end; "
        "return 0; ")), 1);
}

// §6.1 / §4.4: `mod` operator. 10.0m mod 3.0m = 1.0m.
// This was defect #521 sub-bug: before fix, miscompiled as double arithmetic.
BEZEL_TEST(codegen_521_arith, mod_10_mod_3_is_1_spec_6_1_4_4) {
    BEZEL_EXPECT_EQ(run(prog(
        "var a := 10.0m; "
        "var b := 3.0m; "
        "var c := a mod b; "
        "if c = 1.0m then return 1; end; "
        "return 0; ")), 1);
}

// §6.1 / §4.4: negation.
BEZEL_TEST(codegen_521_arith, negate_spec_6_1) {
    BEZEL_EXPECT_EQ(run(prog(
        "var a := 5.5m; "
        "var b := -a; "
        "if b = -5.5m then return 1; end; "
        "return 0; ")), 1);
}

// §4.4 / §6.1: high-precision case: 20 significant digits.
// 1234567890123456789.0m + 1.0m = 1234567890123456790.0m
// This exceeds double's 15-17 significant digit precision.
BEZEL_TEST(codegen_521_arith, high_precision_20_digits_spec_4_4) {
    BEZEL_EXPECT_EQ(run(prog(
        "var a := 1234567890123456789.0m; "
        "var b := a + 1.0m; "
        "if b = 1234567890123456790.0m then return 1; end; "
        "return 0; ")), 1);
}

// ---------------------------------------------------------------------------
// 521_divzero: §4.4 / §6.4 — decimal / 0 raises DivideByZeroException.
// Before fix #521, the code reached codegen as i32/i32, raising SIGFPE.
// ---------------------------------------------------------------------------

// §4.4 / §6.4: decimal / 0m raises DivideByZeroException; caught by try/catch.
BEZEL_TEST(codegen_521_divzero, divide_by_zero_raises_exception_spec_4_4_6_4) {
    BEZEL_EXPECT_EQ(run(prog(
        "var r := 0; "
        "try "
        "  var a := 1.0m; "
        "  var b := 0.0m; "
        "  _ := a / b; "
        "catch DivideByZeroException => r := 1; "
        "end; "
        "return r; ")), 1);
}

// §4.4 / §6.4: decimal mod 0m also raises DivideByZeroException.
BEZEL_TEST(codegen_521_divzero, mod_by_zero_raises_exception_spec_4_4_6_4) {
    BEZEL_EXPECT_EQ(run(prog(
        "var r := 0; "
        "try "
        "  var a := 5.0m; "
        "  var b := 0.0m; "
        "  _ := a mod b; "
        "catch DivideByZeroException => r := 1; "
        "end; "
        "return r; ")), 1);
}

// ---------------------------------------------------------------------------
// 521_cmp: §6.1 — decimal comparison operators (=, <>, <, <=, >, >=).
// ---------------------------------------------------------------------------

// §6.1: decimal equality.
BEZEL_TEST(codegen_521_cmp, equal_spec_6_1) {
    BEZEL_EXPECT_EQ(run(prog(
        "var a := 1.5m; "
        "var b := 1.5m; "
        "if a = b then return 1; end; "
        "return 0; ")), 1);
}

// §6.1: decimal inequality.
BEZEL_TEST(codegen_521_cmp, not_equal_spec_6_1) {
    BEZEL_EXPECT_EQ(run(prog(
        "var a := 1.5m; "
        "var b := 2.0m; "
        "if a <> b then return 1; end; "
        "return 0; ")), 1);
}

// §6.1: less-than.
BEZEL_TEST(codegen_521_cmp, less_than_spec_6_1) {
    BEZEL_EXPECT_EQ(run(prog(
        "var a := 1.0m; "
        "var b := 2.0m; "
        "if a < b then return 1; end; "
        "return 0; ")), 1);
}

// §6.1: less-than-or-equal (equality branch).
BEZEL_TEST(codegen_521_cmp, leq_equal_branch_spec_6_1) {
    BEZEL_EXPECT_EQ(run(prog(
        "var a := 3.0m; "
        "var b := 3.0m; "
        "if a <= b then return 1; end; "
        "return 0; ")), 1);
}

// §6.1: greater-than.
BEZEL_TEST(codegen_521_cmp, greater_than_spec_6_1) {
    BEZEL_EXPECT_EQ(run(prog(
        "var a := 5.0m; "
        "var b := 2.0m; "
        "if a > b then return 1; end; "
        "return 0; ")), 1);
}

// §6.1: greater-than-or-equal (equal branch).
BEZEL_TEST(codegen_521_cmp, geq_equal_branch_spec_6_1) {
    BEZEL_EXPECT_EQ(run(prog(
        "var a := 4.5m; "
        "var b := 4.5m; "
        "if a >= b then return 1; end; "
        "return 0; ")), 1);
}

// ---------------------------------------------------------------------------
// 521_widen: §5.1 — integers widen to decimal implicitly.
// ---------------------------------------------------------------------------

// §5.1: int + decimal → decimal (int is promoted to decimal).
BEZEL_TEST(codegen_521_widen, int_plus_decimal_spec_5_1) {
    BEZEL_EXPECT_EQ(run(prog(
        "var a := 2; "
        "var b := 1.5m; "
        "var c := a + b; "
        "if c = 3.5m then return 1; end; "
        "return 0; ")), 1);
}

// §5.1: decimal + int → decimal.
BEZEL_TEST(codegen_521_widen, decimal_plus_int_spec_5_1) {
    BEZEL_EXPECT_EQ(run(prog(
        "var a := 1.5m; "
        "var b := 3; "
        "var c := a + b; "
        "if c = 4.5m then return 1; end; "
        "return 0; ")), 1);
}

// §5.1: int literal widened to decimal in comparison.
BEZEL_TEST(codegen_521_widen, int_literal_widened_in_compare_spec_5_1) {
    BEZEL_EXPECT_EQ(run(prog(
        "var a := 5m; "
        "if a = 5m then return 1; end; "
        "return 0; ")), 1);
}

// ---------------------------------------------------------------------------
// 521_static: §4.4 / §22.8 — static decimal field with m-literal initializer.
// Before fix #521, this caused an IR verification crash (double ConstantFP
// stored into an i32 global).
// ---------------------------------------------------------------------------

// §4.4 / §22.8: static decimal field initialised with m-literal: reads back
// the same value.
BEZEL_TEST(codegen_521_static, static_decimal_field_init_spec_4_4_22_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static _tax: decimal := 0.1m; "
        "  public static method Main(): int "
        "    if C._tax = 0.1m then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end C;"), 1);
}

// §22.8: static decimal field updated at runtime.
BEZEL_TEST(codegen_521_static, static_decimal_field_update_spec_22_8) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static _balance: decimal := 0m; "
        "  public static method Main(): int "
        "    C._balance := 100.5m; "
        "    if C._balance = 100.5m then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end C;"), 1);
}

// ---------------------------------------------------------------------------
// 521_stdlib: stdlib §9.3 — Decimal.Parse, TryParse, ToString, CompareTo,
// Round.
// ---------------------------------------------------------------------------

// §9.3: Decimal.Parse("1.5") → 1.5m.
BEZEL_TEST(codegen_521_stdlib, parse_roundtrip_spec_9_3) {
    BEZEL_EXPECT_EQ(run(prog(
        "var d := Decimal.Parse(\"1.5\"); "
        "if d = 1.5m then return 1; end; "
        "return 0; ")), 1);
}

// §9.3: Decimal.ToString() — round-trip through string.
BEZEL_TEST(codegen_521_stdlib, tostring_parse_roundtrip_spec_9_3) {
    BEZEL_EXPECT_EQ(run(prog(
        "var d := 3.14m; "
        "var s := d.ToString(); "
        "var d2 := Decimal.Parse(s); "
        "if d2 = 3.14m then return 1; end; "
        "return 0; ")), 1);
}

// §9.3: Decimal.TryParse("2.5", out d) → true, d = 2.5m.
BEZEL_TEST(codegen_521_stdlib, try_parse_valid_spec_9_3) {
    BEZEL_EXPECT_EQ(run(prog(
        "var d: decimal; "
        "var ok := Decimal.TryParse(\"2.5\", out d); "
        "if ok and d = 2.5m then return 1; end; "
        "return 0; ")), 1);
}

// §9.3: Decimal.CompareTo — negative when a < b.
BEZEL_TEST(codegen_521_stdlib, compare_to_less_spec_9_3) {
    BEZEL_EXPECT_EQ(run(prog(
        "var a := 1.0m; "
        "var b := 2.0m; "
        "if a.CompareTo(b) < 0 then return 1; end; "
        "return 0; ")), 1);
}

// §9.3: Decimal.CompareTo — zero when equal.
BEZEL_TEST(codegen_521_stdlib, compare_to_equal_spec_9_3) {
    BEZEL_EXPECT_EQ(run(prog(
        "var a := 3.0m; "
        "var b := 3.0m; "
        "if a.CompareTo(b) = 0 then return 1; end; "
        "return 0; ")), 1);
}

// §9.3: Decimal.CompareTo — positive when a > b.
BEZEL_TEST(codegen_521_stdlib, compare_to_greater_spec_9_3) {
    BEZEL_EXPECT_EQ(run(prog(
        "var a := 5.0m; "
        "var b := 2.0m; "
        "if a.CompareTo(b) > 0 then return 1; end; "
        "return 0; ")), 1);
}

// §9.3: Round(0) — rounds to nearest integer.
BEZEL_TEST(codegen_521_stdlib, round_to_integer_spec_9_3) {
    BEZEL_EXPECT_EQ(run(prog(
        "var d := 2.7m; "
        "var r := d.Round(0); "
        "if r = 3m then return 1; end; "
        "return 0; ")), 1);
}

// §9.3: Round(2) — keeps 2 decimal places.
BEZEL_TEST(codegen_521_stdlib, round_to_2_places_spec_9_3) {
    BEZEL_EXPECT_EQ(run(prog(
        "var d := 3.14159m; "
        "var r := d.Round(2); "
        "if r = 3.14m then return 1; end; "
        "return 0; ")), 1);
}

// fix #2229: Round must look at the FIRST dropped digit (the digit at position
// `digits`), not the LOWEST dropped digit. 0.9651m rounded to 2 places has
// dropped "51" (> half) → must round up to 0.97m. Previously the runtime
// inspected i==0 (the units digit "1") and returned 0.96m.
BEZEL_TEST(codegen_521_stdlib, round_multi_drop_looks_at_first_dropped_digit_spec_9_3) {
    BEZEL_EXPECT_EQ(run(prog(
        "var d := 0.9651m; "
        "var r := d.Round(2); "
        "if r = 0.97m then return 1; end; "
        "return 0; ")), 1);
}

// fix #2232: Round with remove > 1 and an EXACT midpoint (first dropped
// digit == 5, all later dropped == 0) must use banker's rounding (round-half-
// to-even) per stdlib §9.3. 0.9650m.Round(2): kept mantissa = 96 (even), so
// round DOWN to 0.96m. The mirror case 0.9750m.Round(2): kept mantissa = 97
// (odd), so round UP to 0.98m. The previous half-up implementation agreed with
// the odd case (0.9750 → 0.98) but disagreed with the even case (0.9650 → 0.97
// instead of 0.96), violating §9.3's "matching Math.Round (§2.2)" contract
// (Math.Round uses banker's rounding).
BEZEL_TEST(codegen_521_stdlib, round_exact_midpoint_even_kept_spec_9_3) {
    BEZEL_EXPECT_EQ(run(prog(
        "var d := 0.9650m; "
        "var r := d.Round(2); "
        "if r = 0.96m then return 1; end; "
        "return 0; ")), 1);
}

// fix #2232: companion to the even-kept test above — kept mantissa = 97 (odd)
// must round UP at the exact midpoint 0.9750 → 0.98. This is the round-half-
// to-even "round to odd when the kept digit is odd" arm.
BEZEL_TEST(codegen_521_stdlib, round_exact_midpoint_odd_kept_spec_9_3) {
    BEZEL_EXPECT_EQ(run(prog(
        "var d := 0.9750m; "
        "var r := d.Round(2); "
        "if r = 0.98m then return 1; end; "
        "return 0; ")), 1);
}

// fix #2229: Round with remove > 1 and the relevant dropped digit just below
// half must round DOWN. 0.9649m → 0.96m.
BEZEL_TEST(codegen_521_stdlib, round_multi_drop_below_half_spec_9_3) {
    BEZEL_EXPECT_EQ(run(prog(
        "var d := 0.9649m; "
        "var r := d.Round(2); "
        "if r = 0.96m then return 1; end; "
        "return 0; ")), 1);
}

// fix #2229: Round with a tail of 9s propagates the carry into the next place.
// 0.9999m.Round(1) → 1m.
BEZEL_TEST(codegen_521_stdlib, round_carry_propagation_spec_9_3) {
    BEZEL_EXPECT_EQ(run(prog(
        "var d := 0.9999m; "
        "var r := d.Round(1); "
        "if r = 1m then return 1; end; "
        "return 0; ")), 1);
}

// ---------------------------------------------------------------------------
// 521_consts: stdlib §9.3 — Decimal.Zero and Decimal.One static constants.
// ---------------------------------------------------------------------------

// §9.3: Decimal.Zero = 0m.
BEZEL_TEST(codegen_521_consts, zero_constant_spec_9_3) {
    BEZEL_EXPECT_EQ(run(prog(
        "var z := Decimal.Zero; "
        "if z = 0m then return 1; end; "
        "return 0; ")), 1);
}

// §9.3: Decimal.One = 1m.
BEZEL_TEST(codegen_521_consts, one_constant_spec_9_3) {
    BEZEL_EXPECT_EQ(run(prog(
        "var o := Decimal.One; "
        "if o = 1m then return 1; end; "
        "return 0; ")), 1);
}

// §9.3: Decimal.MinValue is less than zero.
BEZEL_TEST(codegen_521_consts, min_value_is_negative_spec_9_3) {
    BEZEL_EXPECT_EQ(run(prog(
        "var mv := Decimal.MinValue; "
        "if mv < 0m then return 1; end; "
        "return 0; ")), 1);
}

// §9.3: Decimal.MaxValue is greater than zero.
BEZEL_TEST(codegen_521_consts, max_value_is_positive_spec_9_3) {
    BEZEL_EXPECT_EQ(run(prog(
        "var mv := Decimal.MaxValue; "
        "if mv > 0m then return 1; end; "
        "return 0; ")), 1);
}

// §9.3: MinValue < MaxValue.
BEZEL_TEST(codegen_521_consts, min_less_than_max_spec_9_3) {
    BEZEL_EXPECT_EQ(run(prog(
        "var lo := Decimal.MinValue; "
        "var hi := Decimal.MaxValue; "
        "if lo < hi then return 1; end; "
        "return 0; ")), 1);
}

// ---------------------------------------------------------------------------
// 521_overflow: §6.4 — decimal arithmetic overflow raises OverflowException.
// Before fix #521 (this commit), these paths called abort() and killed the
// process.  After the fix, OverflowException propagates through try/catch.
// ---------------------------------------------------------------------------

// §6.4: MaxValue + One raises OverflowException (add path via dec_store).
BEZEL_TEST(codegen_521_overflow, max_plus_one_raises_overflow_spec_6_4) {
    BEZEL_EXPECT_EQ(run(prog(
        "var r := 0; "
        "try "
        "  var big := Decimal.MaxValue; "
        "  var one := Decimal.One; "
        "  _ := big + one; "
        "catch OverflowException => r := 1; "
        "end; "
        "return r; ")), 1);
}

// §6.4: MaxValue * 2m raises OverflowException (mul path via dec_store).
BEZEL_TEST(codegen_521_overflow, max_times_two_raises_overflow_spec_6_4) {
    BEZEL_EXPECT_EQ(run(prog(
        "var r := 0; "
        "try "
        "  var big := Decimal.MaxValue; "
        "  var two := 2m; "
        "  _ := big * two; "
        "catch OverflowException => r := 1; "
        "end; "
        "return r; ")), 1);
}

// §6.4: MinValue - One raises OverflowException (sub path via add).
// MinValue is already the most negative representable decimal; subtracting
// one more pushes the mantissa past MaxValue.
BEZEL_TEST(codegen_521_overflow, min_minus_one_raises_overflow_spec_6_4) {
    BEZEL_EXPECT_EQ(run(prog(
        "var r := 0; "
        "try "
        "  var small := Decimal.MinValue; "
        "  var one := Decimal.One; "
        "  _ := small - one; "
        "catch OverflowException => r := 1; "
        "end; "
        "return r; ")), 1);
}

// §6.4 / §9.3: Decimal.Parse on an out-of-range string raises OverflowException.
// The value 10^29 exceeds MaxValue (≈7.9e28), so Parse must raise
// OverflowException rather than FormatException (the string is well-formed).
// fix #2273: the value was spelled "1e29" here, which §9.3's grammar — "an
// optional leading sign (+/-) followed by decimal digits with at most one `.`"
// — does not admit, so that spelling now raises FormatException and no longer
// tests what this case is about.  Spelled in digits, the test's intent (an
// out-of-range MAGNITUDE raises OverflowException) is unchanged and now also
// exercises #1829's dropped-integer-digit accounting.  The §9.3 rejection of
// the exponent spelling is covered by
// codegen_decimal_2273.parse_exponent_form_rejected_spec_9_3.
BEZEL_TEST(codegen_521_overflow, parse_out_of_range_raises_overflow_spec_6_4_9_3) {
    BEZEL_EXPECT_EQ(run(prog(
        "var r := 0; "
        "try "
        "  _ := Decimal.Parse(\"100000000000000000000000000000\"); "
        "catch OverflowException => r := 1; "
        "end; "
        "return r; ")), 1);
}

// §9.3: Decimal.Parse on a malformed string raises FormatException.
// "abc" is not a valid decimal string.
BEZEL_TEST(codegen_521_overflow, parse_malformed_raises_format_spec_9_3) {
    BEZEL_EXPECT_EQ(run(prog(
        "var r := 0; "
        "try "
        "  _ := Decimal.Parse(\"abc\"); "
        "catch FormatException => r := 1; "
        "end; "
        "return r; ")), 1);
}

// §6.4: overflow exception is catchable — execution continues after the catch.
// This test proves the exception is a proper exception (not abort) by checking
// that code after the try/catch runs.
BEZEL_TEST(codegen_521_overflow, overflow_catchable_execution_continues_spec_6_4) {
    BEZEL_EXPECT_EQ(run(prog(
        "var caught := 0; "
        "try "
        "  var big := Decimal.MaxValue; "
        "  _ := big + big; "
        "catch OverflowException => caught := 1; "
        "end; "
        "if caught = 1 then return 42; end; "
        "return 0; ")), 42);
}

// §6.4 regression: normal arithmetic is still exact after the overflow fix.
// 0.1m + 0.2m = 0.3m must still hold — the fix must not affect non-overflow paths.
BEZEL_TEST(codegen_521_overflow, normal_add_still_exact_regression_spec_6_4) {
    BEZEL_EXPECT_EQ(run(prog(
        "var a := 0.1m; "
        "var b := 0.2m; "
        "var c := a + b; "
        "if c = 0.3m then return 1; end; "
        "return 0; ")), 1);
}

// ---------------------------------------------------------------------------
// 521_overflow_aot: AOT (native executable) probe — §6.4 overflow exception
// propagates correctly through the native code path (not just JIT).
// Uses the Driver compile+link+exec pipeline, same as uncaught_exit_test.cpp.
// ---------------------------------------------------------------------------

namespace {

// Per-process unique working dir, under the shared scratch base (never
// /tmp — see support/scratch_dir.hpp) and reaped at process exit.
static const std::filesystem::path& wtDir521() {
    return wtScratchDir("wt-521");
}

// Compile `source` to a temp native executable and run it; returns the
// process exit code, or -1 if compilation/link/run failed.
// per §33.3: Main's return value is the process exit code.
int compileAndRun521(const std::string& source) {
    auto dir = wtDir521();
    std::string stem = unique_stem("bezel_521_aot");
    std::string src  = (dir / (stem + ".bzl")).string();
    std::string exe  = (dir / stem).string();
    {
        std::ofstream out(src);
        out << source;
    }
    bezel::Driver driver;
    bezel::CompileResult r = driver.compileFile(src, exe);
    int code = -1;
    if (r.success) {
        int status = std::system(exe.c_str());
        if (status != -1 && WIFEXITED(status)) code = WEXITSTATUS(status);
    }
    std::remove(src.c_str());
    std::remove(exe.c_str());
    return code;
}

} // namespace

// §6.4 AOT probe: Decimal.MaxValue + Decimal.One raises OverflowException;
// the catch runs and Main returns 42 → process exits with code 42.
// This verifies the exception propagates correctly in native (non-JIT) mode.
BEZEL_TEST(codegen_521_overflow, aot_max_plus_one_catches_and_returns_42_spec_6_4) {
    BEZEL_EXPECT_EQ(compileAndRun521(
        "module M; "
        "class C public static method Main(): int "
        "  try "
        "    var big := Decimal.MaxValue; "
        "    var one := Decimal.One; "
        "    _ := big + one; "
        "    return 1; "
        "  catch OverflowException => return 42; "
        "  end; "
        "  return 0; "
        "end Main; end C;"),
        42);
}

// Spec §5 (L243): "Static members of the standard library type are accessible
// through either form: int.Parse, int.MaxValue ..." — the keyword receiver
// (`decimal.MaxValue`) and the class receiver (`Decimal.MaxValue`) must agree.
// Regression for the silent-garbage keyword form found 2026-06-12.
BEZEL_TEST(codegen_521_overflow, keyword_receiver_statics_agree_spec_5) {
    BEZEL_EXPECT_EQ(compileAndRun521(
        "module M; "
        "class C public static method Main(): int "
        "  var a := Decimal.MaxValue; "
        "  var b := decimal.MaxValue; "
        "  if a = b and decimal.One = Decimal.One and decimal.Zero = 0.0m then "
        "    try "
        "      _ := decimal.MaxValue + decimal.One; "
        "      return 2; "
        "    catch OverflowException => return 42; "
        "    end; "
        "  end; "
        "  return 3; "
        "end Main; end C;"), 42);
}

// #743 / §5.2: an UNSIGNED integer above the signed maximum converts to decimal
// without being reinterpreted as negative. `decimal(uint)` zero-extends to i64
// before the conversion; `decimal(ulong)` uses the unsigned 64-bit conversion.
// Previously both used the signed conversion, giving a wrong (negative-derived)
// value for uint > int.MaxValue and ulong > long.MaxValue.
BEZEL_TEST(codegen_decimal_521, decimal_from_large_uint_743) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var u: uint := 3000000000u; var d: decimal := decimal(u); "
        "  return (if d > 2500000000m then 1 else 0); "  // 3e9 > 2.5e9 → 1
        "end Main; end C;"), 1);
}

BEZEL_TEST(codegen_decimal_521, decimal_from_large_ulong_743) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var u: ulong := 10000000000000000000uL; var d: decimal := decimal(u); "
        "  return (if d > 9300000000000000000m then 1 else 0); "  // 1e19 > 9.3e18 → 1
        "end Main; end C;"), 1);
}

// Arithmetic widening (§6.2): `uint + decimal` promotes the uint correctly.
BEZEL_TEST(codegen_decimal_521, large_uint_plus_decimal_widening_743) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var u: uint := 3000000000u; var r: decimal := u + 0m; "
        "  return (if r > 2500000000m then 1 else 0); "  // 1
        "end Main; end C;"), 1);
}

// Regression: a signed negative int still converts to a negative decimal.
BEZEL_TEST(codegen_decimal_521, decimal_from_negative_int_unchanged_743) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var n: int := -7; var d: decimal := decimal(n); "
        "  return (if d < 0m then 1 else 0); "  // 1
        "end Main; end C;"), 1);
}

// ---------------------------------------------------------------------------
// #1099B (§9.3): Decimal.ToString(format) honors the "F"/"f" fixed-point
// specifier. Before the fix the generic numeric ToString(format) path (which
// only handles float/double F/E/G and int D/X) dropped a decimal format to the
// plain ToString — `1.5m.ToString("F2")` gave "1.5" instead of "1.50". The
// runtime bz_decimal_to_string_fmt rounds half-up to the precision and pads with
// trailing zeros. (Tests check Length + selected chars; runJit returns the raw
// exit code.)
// ---------------------------------------------------------------------------

// 1.5m.ToString("F2") == "1.50" (length 4).
BEZEL_TEST(codegen_decimal_521, tostring_f2_pads_trailing_zero_1099b) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var d: decimal := 1.5m; return d.ToString(\"F2\").Length; "  // "1.50"
        "end Main; end C;"), 4);
}

// 1.5m.ToString("F2") char content is exactly "1.50".
BEZEL_TEST(codegen_decimal_521, tostring_f2_content_1099b) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := (1.5m).ToString(\"F2\"); "
        "  return (if s[0] = char(49) and s[1] = char(46) and "
        "                s[2] = char(53) and s[3] = char(48) then 1 else 0); "  // '1''.'​'5''0'
        "end Main; end C;"), 1);
}

// 123.456m.ToString("F2") rounds half-up to "123.46" (length 6).
BEZEL_TEST(codegen_decimal_521, tostring_f2_rounds_half_up_1099b) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := (123.456m).ToString(\"F2\"); "
        "  return (if s.Length = 6 and s[5] = char(54) then 1 else 0); "  // "123.46"
        "end Main; end C;"), 1);
}

// Integer decimal with "F2" gains the fraction: 2m → "2.00" (length 4).
BEZEL_TEST(codegen_decimal_521, tostring_f2_integer_adds_fraction_1099b) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var d: decimal := 2m; return d.ToString(\"F2\").Length; "  // "2.00"
        "end Main; end C;"), 4);
}

// "F0" drops the point and rounds: 1.5m → "2" (length 1).
BEZEL_TEST(codegen_decimal_521, tostring_f0_rounds_no_point_1099b) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var d: decimal := 1.5m; return d.ToString(\"F0\").Length; "  // "2"
        "end Main; end C;"), 1);
}

// Negative: -1.5m.ToString("F2") == "-1.50" (length 5).
BEZEL_TEST(codegen_decimal_521, tostring_f2_negative_1099b) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var d: decimal := -1.5m; return d.ToString(\"F2\").Length; "  // "-1.50"
        "end Main; end C;"), 5);
}

// Regression: the no-format ToString() is unchanged ("1.5", length 3).
BEZEL_TEST(codegen_decimal_521, tostring_no_format_unchanged_1099b) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var d: decimal := 1.5m; return d.ToString().Length; "  // "1.5"
        "end Main; end C;"), 3);
}
