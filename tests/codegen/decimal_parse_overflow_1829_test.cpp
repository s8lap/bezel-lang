// Codegen tests for fix #1829: bz_decimal_parse_cstr must raise
// OverflowException (not silently truncate) when a decimal value's magnitude
// exceeds `decimal`'s range.
//
// Spec reference:
//   Bezel_Language_Spec.md §4.4 (decimal — "28–29 significant decimal digits",
//   the 96-bit mantissa's precision/range ceiling), §6.4 (decimal arithmetic
//   raises OverflowException on overflow), v0.226.
//   Bezel_Standard_Library.md §9.3 (Decimal.Parse: "A value outside the
//   representable range raises OverflowException. TryParse returns false in
//   every case where Parse would raise."), v0.226.
//
// Before the fix the parse loop dropped every digit beyond the 29th the 96-bit
// mantissa could hold (comment: "lose trailing digits beyond 29"), so a value
// like the 40-digit integer below was silently truncated to an in-range
// mantissa and accepted — neither the literal range-check (§4.3, reusing this
// parser) nor Decimal.Parse/TryParse (§9.3) raised.  The fix counts dropped
// integer-part digits and, if the accumulated scale cannot absorb them, raises
// OverflowException; TryParse now observes the overflow and returns false.
//
// Each Overflow/compile-error case FAILS without the fix (silent truncation,
// no exception / no error) and PASSES with it.

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

// True iff the program is rejected by sema (compile error).
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
// Parse path (stdlib §9.3) — out-of-range magnitude must raise.
// ---------------------------------------------------------------------------

// §4.4 / §9.3 (fix #1829): a 40-significant-digit integer far exceeds
// decimal's 29-digit mantissa and ~7.9e28 range; Decimal.Parse must raise
// OverflowException.  Before the fix the parser silently truncated the
// mantissa and the catch never ran (returned 0).
BEZEL_TEST(codegen_decimal_1829, parse_40_digit_integer_raises_overflow_spec_4_4_9_3) {
    BEZEL_EXPECT_EQ(run(prog(
        "var r := 0; "
        "try "
        "  var _ := Decimal.Parse(\"1234567890123456789012345678901234567890\"); "
        "catch OverflowException => r := 1; "
        "end; "
        "return r; ")), 1);
}

// §4.4 / §9.3: the boundary just above the range — MaxValue+1 (29 digits) —
// also raises OverflowException.  This already worked (dec_store rejects a
// 29-digit mantissa that overflows the 96-bit field); it is a regression
// guard that the dropped-digit fix does not regress the near-boundary path.
BEZEL_TEST(codegen_decimal_1829, parse_maxvalue_plus_one_raises_overflow_spec_4_4_9_3) {
    BEZEL_EXPECT_EQ(run(prog(
        "var r := 0; "
        "try "
        "  var _ := Decimal.Parse(\"79228162514264337593543950336\"); "
        "catch OverflowException => r := 1; "
        "end; "
        "return r; ")), 1);
}

// §4.4 / §9.3: a 30-digit integer is one digit beyond the mantissa and is
// out of range (>= 10^29 > MaxValue); Parse raises OverflowException.  Before
// the fix the 30th digit was silently dropped and a truncated value returned.
BEZEL_TEST(codegen_decimal_1829, parse_30_digit_integer_raises_overflow_spec_4_4_9_3) {
    BEZEL_EXPECT_EQ(run(prog(
        "var r := 0; "
        "try "
        "  var _ := Decimal.Parse(\"123456789012345678901234567890\"); "
        "catch OverflowException => r := 1; "
        "end; "
        "return r; ")), 1);
}

// ---------------------------------------------------------------------------
// Parse path — in-range values are still accepted (no false positive).
// ---------------------------------------------------------------------------

// §4.4 / §9.3: a 29-significant-digit integer is exactly at the mantissa
// ceiling but in range; Parse returns the correct value.  Regression guard
// that the dropped-digit accounting does not over-reject valid high-precision
// input.
BEZEL_TEST(codegen_decimal_1829, parse_29_digit_integer_in_range_spec_4_4_9_3) {
    BEZEL_EXPECT_EQ(run(prog(
        "var d := Decimal.Parse(\"12345678901234567890123456789\"); "
        "if d = 12345678901234567890123456789m then return 1; end; "
        "return 0; ")), 1);
}

// §4.4 / §9.3: Decimal.MaxValue itself (the largest representable value, 29
// digits) parses back to itself.  Boundary guard against over-rejection.
BEZEL_TEST(codegen_decimal_1829, parse_maxvalue_roundtrips_spec_4_4_9_3) {
    BEZEL_EXPECT_EQ(run(prog(
        "var d := Decimal.Parse(\"79228162514264337593543950335\"); "
        "if d = Decimal.MaxValue then return 1; end; "
        "return 0; ")), 1);
}

// §4.4 / §9.3 (fix #1829): a value whose digit count exceeds 29 but whose
// magnitude is brought back into range by a negative exponent must NOT raise
// — the scale absorbs the dropped integer digits.  10^30 e-3 = 10^27, well
// within range; Parse yields exactly 10^27.  Before the fix the parser lost
// the dropped digits' magnitude and returned 10^25 (silently wrong); with the
// fix the scale consumes them and the value is correct.
BEZEL_TEST(codegen_decimal_1829, parse_long_integer_with_compensating_exponent_spec_4_4_9_3) {
    BEZEL_EXPECT_EQ(run(prog(
        "var d := Decimal.Parse(\"1000000000000000000000000000000e-3\"); "
        "if d = 1000000000000000000000000000m then return 1; end; "
        "return 0; ")), 1);
}

// ---------------------------------------------------------------------------
// TryParse path (stdlib §9.3) — returns false wherever Parse would raise.
// ---------------------------------------------------------------------------

// §9.3 (fix #1829): TryParse of an out-of-range 40-digit value returns false
// (and writes the default).  Before the fix the overflow was swallowed
// (null OverflowException vtable) and TryParse returned true with a truncated
// value — the precise contract violation the fix corrects.
BEZEL_TEST(codegen_decimal_1829, try_parse_40_digit_integer_returns_false_spec_9_3) {
    BEZEL_EXPECT_EQ(run(prog(
        "var d: decimal; "
        "var ok := Decimal.TryParse(\"1234567890123456789012345678901234567890\", out d); "
        "return (if ok then 0 else 1); ")), 1);
}

// §9.3: TryParse of an in-range value still returns true and yields it.
// Regression guard that the sentinel-vtable change did not break success.
BEZEL_TEST(codegen_decimal_1829, try_parse_in_range_returns_true_spec_9_3) {
    BEZEL_EXPECT_EQ(run(prog(
        "var d: decimal; "
        "var ok := Decimal.TryParse(\"2.5\", out d); "
        "return (if ok and d = 2.5m then 1 else 0); ")), 1);
}

// ---------------------------------------------------------------------------
// Literal path (§4.3 / §4.4) — out-of-range literals are compile errors.
// ---------------------------------------------------------------------------

// §4.3 / §4.4 (fix #1829): a 40-digit `m` literal is out of range and must be
// rejected at formation (the literal range-check reuses this parser).  Before
// the fix the literal was silently truncated and accepted.
BEZEL_TEST(codegen_decimal_1829, literal_40_digit_integer_rejected_spec_4_3_4_4) {
    BEZEL_EXPECT_TRUE(hasErrors(prog(
        "var x: decimal := 1234567890123456789012345678901234567890m; "
        "return 0; ")));
}

// §4.3 / §4.4: a 29-digit in-range literal is still accepted (no false
// positive from the range-check).
BEZEL_TEST(codegen_decimal_1829, literal_29_digit_integer_accepted_spec_4_3_4_4) {
    BEZEL_EXPECT_EQ(run(prog(
        "var x: decimal := 12345678901234567890123456789m; "
        "if x = 12345678901234567890123456789m then return 1; end; "
        "return 0; ")), 1);
}
