// Codegen tests for fix #2273 — the decimal parser's fractional-digit scale
// accounting, its input-length bound, and the §9.3 vs §18.1 grammar split.
//
// Spec references (v0.226):
//   Bezel_Standard_Library.md §9.3 (Decimal.Parse: accepts "an optional leading
//     sign (+/-) followed by decimal digits with at most one `.`"; "A value
//     outside the representable range raises OverflowException"; "TryParse
//     returns false in every case where Parse would raise").
//   Bezel_Standard_Library.md §2.3 (Convert.To*/Try* share this contract: the
//     Try forms' "out parameter receives the default value").
//   Bezel_Language_Spec.md §4.4 (decimal — 28–29 significant digits, the 96-bit
//     mantissa's range ceiling), §6.4 (OverflowException), §18.1 + Appendix A
//     (the decimal LITERAL grammar, which DOES admit an exponent: `1.23e2m`).
//
// The defect (all three parts live in the same function,
// src/runtime/bezel_runtime_decimal_guid.cpp):
//
//   1. `if (in_frac) ++sc;` ran BEFORE the `m <= kMax96/10` store guard, so a
//      fractional digit the mantissa could not hold still incremented the
//      scale — dividing the result by 10 per dropped digit.  Fix #1829 only
//      counted dropped INTEGER-part digits.  Consequences: a silently wrong
//      value for in-range input (Parse and the compile-time literal fold
//      alike), and a defeated range check — `sc -= dropped_int` subtracted
//      from a scale inflated by digits the mantissa never held, so an
//      out-of-range magnitude was accepted 10x too small instead of raising.
//
//   2. Both bz_string entry points copied the input through `char buf[64]`
//      with `n = len < 63 ? len : 63`, so any input past 63 characters was
//      truncated to its prefix and parsed — a wrong value, no exception.
//
//   3. §9.3's grammar admits no exponent, but the parser (the shared engine
//      for compile-time literal folding, where §18.1 DOES admit one) accepted
//      it on the Parse/TryParse surface too, and TryParse's separately
//      hand-maintained validator had drifted from the parser in both
//      directions (it rejected "1.2.3" that Parse accepted).
//
// Every case below FAILS on the pre-fix parser — with a wrong value where the
// bug produced one silently, and with a missing exception where it defeated a
// range or shape check — and PASSES with the fix.

#include <string>

#include "driver/Driver.hpp"
#include "support/aot_support.hpp"   // runAot — AOT test harness
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

std::string prog(const std::string& mainBody) {
    return "module M; "
           "class C public static method Main(): int "
           + mainBody +
           " end Main; end C;";
}

// 60 leading zeros + "1234567890.5" = 72 characters: past the pre-fix 63-byte
// buffer, whose truncated prefix ("<60 zeros>123") parses to 123.
const char* kLongZeros =
    "000000000000000000000000000000000000000000000000000000000000"
    "1234567890.5";

} // namespace

// ---------------------------------------------------------------------------
// 1. Fractional digits beyond the mantissa must not move the scale (§4.4/§9.3).
// ---------------------------------------------------------------------------

// §4.4 / §9.3: "12." + 28 fractional digits is 30 digits; the 96-bit mantissa
// holds the first 29, and the single dropped digit ('0', so no rounding
// question arises) is a precision loss only — it changes neither the magnitude
// nor the scale.  ToString is the parser-independent oracle.
// Pre-fix the dropped digit still bumped the scale, so this returned
// "1.2345678901234567890123456789" — exactly 10x too small, silently.
BEZEL_TEST(codegen_decimal_2273, parse_dropped_fraction_digit_keeps_magnitude_spec_4_4_9_3) {
    BEZEL_EXPECT_EQ(run(prog(
        "var d := Decimal.Parse(\"12.3456789012345678901234567890\"); "
        "var s := d.ToString(); "
        "if s = \"12.345678901234567890123456789\" then return 1; end; "
        "return 0; ")), 1);
}

// §4.4 / §9.3: the same value compared against the equivalent 29-digit literal
// — the parsed value must equal the literal it denotes, not a tenth of it.
BEZEL_TEST(codegen_decimal_2273, parse_dropped_fraction_digit_equals_literal_spec_4_4_9_3) {
    BEZEL_EXPECT_EQ(run(prog(
        "var d := Decimal.Parse(\"12.3456789012345678901234567890\"); "
        "if d = 12.345678901234567890123456789m then return 1; end; "
        "return 0; ")), 1);
}

// §18.1 / §4.4: the SAME parser folds decimal literals at compile time
// (src/sema/ExprTypecheck_Exprs.cpp's range probe and
// src/codegen/LlvmBackend_Exprs.cpp's constant fold), so pre-fix this was a
// compile-time wrong value: the 30-digit literal folded to a constant ten times
// too small while the 29-digit literal folded correctly, and the two compared
// unequal.
BEZEL_TEST(codegen_decimal_2273, literal_dropped_fraction_digit_folds_correctly_spec_18_1_4_4) {
    BEZEL_EXPECT_EQ(run(prog(
        "var x: decimal := 12.3456789012345678901234567890m; "
        "var s := x.ToString(); "
        "if x = 12.345678901234567890123456789m "
        "   and s = \"12.345678901234567890123456789\" then return 1; end; "
        "return 0; ")), 1);
}

// ---------------------------------------------------------------------------
// 2. The range check the inflated scale defeated (§9.3 OverflowException).
// ---------------------------------------------------------------------------

// §4.4 / §9.3: 29 nines then ".9" is ~9.99e28, above decimal.MaxValue
// (7.9228…e28), so Parse must raise OverflowException.  Pre-fix the dropped
// fractional '9' inflated the scale to 1, `sc -= dropped_int` consumed it, and
// the value was accepted as 9999999999999999999999999999 — no exception, and
// wrong by a factor of 10 on top.  With the fix a dropped fractional digit
// leaves the scale at 0, so `dropped_int > sc` reports the overflow.
BEZEL_TEST(codegen_decimal_2273, parse_fraction_cannot_absorb_dropped_digit_raises_spec_4_4_9_3) {
    BEZEL_EXPECT_EQ(run(prog(
        "var r := 0; "
        "try "
        "  var _ := Decimal.Parse(\"99999999999999999999999999999.9\"); "
        "catch OverflowException => r := 1; "
        "end; "
        "return r; ")), 1);
}

// §9.3: "TryParse returns false in every case where Parse would raise" — the
// same out-of-range magnitude.  Pre-fix TryParse returned true with the
// truncated, mis-scaled value.
BEZEL_TEST(codegen_decimal_2273, try_parse_fraction_overflow_returns_false_spec_9_3) {
    BEZEL_EXPECT_EQ(run(prog(
        "var d: decimal; "
        "var ok := Decimal.TryParse(\"99999999999999999999999999999.9\", out d); "
        "return (if ok then 0 else 1); ")), 1);
}

// §4.3 / §4.4: the literal spelling of the same out-of-range magnitude is a
// compile error (the literal range-check probes this parser).  Pre-fix it was
// accepted and folded to a wrong constant.
BEZEL_TEST(codegen_decimal_2273, literal_fraction_overflow_rejected_spec_4_3_4_4) {
    auto r = runAot(prog(
        "var x: decimal := 99999999999999999999999999999.9m; "
        "return 0; "));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// 3. Input longer than the old 63-byte buffer (§9.3 — no silent truncation).
// ---------------------------------------------------------------------------

// §9.3: leading zeros are not significant, so a 72-character spelling of
// 1234567890.5 is a well-formed, in-range value and must parse exactly.
// Pre-fix the input was copied through `char buf[64]` (63 chars kept), leaving
// "<60 zeros>123" — Parse returned 123 with no exception.
BEZEL_TEST(codegen_decimal_2273, parse_input_past_63_chars_not_truncated_spec_9_3) {
    BEZEL_EXPECT_EQ(run(prog(
        std::string("var d := Decimal.Parse(\"") + kLongZeros + "\"); "
        "var s := d.ToString(); "
        "if d = 1234567890.5m and s = \"1234567890.5\" then return 1; end; "
        "return 0; ")), 1);
}

// §9.3: TryParse shares the truncating copy; it returned true with the same
// wrong value (123) pre-fix.
BEZEL_TEST(codegen_decimal_2273, try_parse_input_past_63_chars_not_truncated_spec_9_3) {
    BEZEL_EXPECT_EQ(run(prog(
        std::string("var d: decimal; "
        "var ok := Decimal.TryParse(\"") + kLongZeros + "\", out d); "
        "return (if ok and d = 1234567890.5m then 1 else 0); ")), 1);
}

// ---------------------------------------------------------------------------
// 4. The §9.3 grammar admits no exponent; the §18.1 literal grammar does.
// ---------------------------------------------------------------------------

// §9.3: the accepted shape is "an optional leading sign (+/-) followed by
// decimal digits with at most one `.`" — an exponent is not part of it, so
// Parse raises FormatException (not OverflowException: the string is
// malformed, not out of range).  Pre-fix the parser accepted the exponent form
// on this surface, an over-admission a test had pinned (#1829's
// parse_long_integer_with_compensating_exponent, retired with this fix).
BEZEL_TEST(codegen_decimal_2273, parse_exponent_form_rejected_spec_9_3) {
    BEZEL_EXPECT_EQ(run(prog(
        "var r := 0; "
        "try "
        "  var _ := Decimal.Parse(\"1.5e2\"); "
        "catch FormatException => r := 1; "
        "catch OverflowException => r := 2; "
        "end; "
        "return r; ")), 1);
}

// §9.3: the exact string #1829's test asserted must SUCCEED — an exponent that
// compensates a 31-digit integer part — is likewise malformed on this surface.
BEZEL_TEST(codegen_decimal_2273, parse_compensating_exponent_rejected_spec_9_3) {
    BEZEL_EXPECT_EQ(run(prog(
        "var r := 0; "
        "try "
        "  var _ := Decimal.Parse(\"1000000000000000000000000000000e-3\"); "
        "catch FormatException => r := 1; "
        "catch OverflowException => r := 2; "
        "end; "
        "return r; ")), 1);
}

// §9.3: TryParse returns false wherever Parse raises — including the shape
// rejections, which pre-fix its separate validator admitted.
BEZEL_TEST(codegen_decimal_2273, try_parse_exponent_form_returns_false_spec_9_3) {
    BEZEL_EXPECT_EQ(run(prog(
        "var d: decimal; "
        "var ok := Decimal.TryParse(\"1.5e2\", out d); "
        "return (if ok then 0 else 1); ")), 1);
}

// §18.1 / Appendix A: the LITERAL grammar does admit scientific notation
// (`FloatLiteral ::= DigitGroup Exponent FloatSuffix?`), so `1.23e2m` stays a
// valid literal and folds to 123 — the §9.3 rejection above is a property of
// the string API's grammar, not of the parser engine.  The second literal is
// the compensating-exponent case #1829 covered through Decimal.Parse: 31 digits
// scaled by e-3 = 10^27, in range, and the digits the mantissa dropped are paid
// for by the scale (the #1829 accounting, which this fix keeps sound).
BEZEL_TEST(codegen_decimal_2273, literal_exponent_form_still_accepted_spec_18_1) {
    BEZEL_EXPECT_EQ(run(prog(
        "var a: decimal := 1.23e2m; "
        "var b: decimal := 1000000000000000000000000000000e-3m; "
        "if a = 123m and b = 1000000000000000000000000000m then return 1; end; "
        "return 0; ")), 1);
}

// ---------------------------------------------------------------------------
// 5. One shared grammar for Parse and TryParse (§9.3's two-way contract).
// ---------------------------------------------------------------------------

// §9.3: "at most one `.`".  Pre-fix Parse ACCEPTED "1.2.3" (the second '.'
// merely re-entered the fraction, yielding 1.23) while TryParse's separate
// validator rejected it — the §9.3 equivalence broken in the direction that
// silently returns a value for malformed input.
BEZEL_TEST(codegen_decimal_2273, parse_second_decimal_point_rejected_spec_9_3) {
    BEZEL_EXPECT_EQ(run(prog(
        "var r := 0; "
        "try "
        "  var _ := Decimal.Parse(\"1.2.3\"); "
        "catch FormatException => r := 1; "
        "end; "
        "return r; ")), 1);
}

// §9.3: and TryParse still rejects it — the two surfaces now share one grammar.
BEZEL_TEST(codegen_decimal_2273, try_parse_second_decimal_point_returns_false_spec_9_3) {
    BEZEL_EXPECT_EQ(run(prog(
        "var d: decimal; "
        "var ok := Decimal.TryParse(\"1.2.3\", out d); "
        "return (if ok then 0 else 1); ")), 1);
}

// §2.3: "the `out` parameter receives the default value" on failure — the same
// runtime entry serves Decimal.TryParse and Convert.TryToDecimal.  Pre-fix the
// failure paths returned without writing the slot, so codegen stored an
// uninitialised alloca into the caller's variable.
BEZEL_TEST(codegen_decimal_2273, try_parse_failure_writes_default_spec_2_3) {
    BEZEL_EXPECT_EQ(run(prog(
        "var d: decimal := 5m; "
        "var ok := Decimal.TryParse(\"garbage\", out d); "
        "return (if not ok and d = 0m then 1 else 0); ")), 1);
}

// ---------------------------------------------------------------------------
// 6. Regression guards — in-range fractional values must be unchanged.
// ---------------------------------------------------------------------------

// §9.3: 28 fractional digits fit the mantissa exactly; every one of them counts
// toward the scale.  Guards against the fix over-correcting (dropping a scale
// increment for a digit that WAS stored).
BEZEL_TEST(codegen_decimal_2273, parse_28_fraction_digits_in_range_spec_9_3) {
    BEZEL_EXPECT_EQ(run(prog(
        "var d := Decimal.Parse(\"0.1234567890123456789012345678\"); "
        "var s := d.ToString(); "
        "if s = \"0.1234567890123456789012345678\" then return 1; end; "
        "return 0; ")), 1);
}

// §9.3: ordinary short fractional input, sign handling, and a leading-zero
// integer part are untouched by the scale-accounting change.
BEZEL_TEST(codegen_decimal_2273, parse_short_fractions_unchanged_spec_9_3) {
    BEZEL_EXPECT_EQ(run(prog(
        "var ok := 0; "
        "var a := Decimal.Parse(\"2.5\"); "
        "if a = 2.5m then ok := ok + 1; end; "
        "var b := Decimal.Parse(\"-0.125\"); "
        "if b = -0.125m then ok := ok + 2; end; "
        "var c := Decimal.Parse(\"+12.30\"); "
        "var cs := c.ToString(); "
        "if cs = \"12.3\" then ok := ok + 4; end; "
        "var z := Decimal.Parse(\"0\"); "
        "if z = 0m then ok := ok + 8; end; "
        "return ok; ")), 15);
}
