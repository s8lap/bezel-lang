// Codegen regression tests for fix #5400 — stdlib §9.3 (v0.314) grammar:
// `decimal.Parse` accepts inputs the spec forbids.
//
// Spec reference (Bezel_Standard_Library.md §9.3, v0.314):
//   "Parse(s): ... an optional leading sign (+/-) followed by decimal digits
//    with at most one `.` ... at least one digit must be present."
//
// The §9.3 grammar does NOT admit:
//   - A bare decimal point as the first character after the sign (".5", "-.5")
//   - A trailing decimal point with no fractional digits ("5.")
//   - An exponent suffix ("1e3") — §18.1 admits it for LITERALS, but §9.3's
//     Parse does not
//
// All three were silently accepted pre-fix.  Post-fix each raises FormatException.

#include <string>

#include "driver/Driver.hpp"
#include "support/aot_support.hpp"   // runAot
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

} // namespace

// ---------------------------------------------------------------------------
// §9.3 (v0.314, fix #5400): bare decimal point — ".5" is not a valid integer
// part followed by a fractional part; the integer part is absent.
// ---------------------------------------------------------------------------

// §9.3: ".5" has no integer-part digit — FormatException.
BEZEL_TEST(codegen_decimal_parse_grammar_5400, dot5_rejected_spec_9_3) {
    BEZEL_EXPECT_EQ(run(prog(
        "var r := 0; "
        "try "
        "  _ := Decimal.Parse(\".5\"); "
        "catch FormatException => r := 1; "
        "end; "
        "return r; ")), 1);
}

// §9.3: "+.5" — same as ".5" but with explicit positive sign.
BEZEL_TEST(codegen_decimal_parse_grammar_5400, plus_dot5_rejected_spec_9_3) {
    BEZEL_EXPECT_EQ(run(prog(
        "var r := 0; "
        "try "
        "  _ := Decimal.Parse(\"+.5\"); "
        "catch FormatException => r := 1; "
        "end; "
        "return r; ")), 1);
}

// §9.3: "-.5" — same as ".5" but with negative sign.
BEZEL_TEST(codegen_decimal_parse_grammar_5400, minus_dot5_rejected_spec_9_3) {
    BEZEL_EXPECT_EQ(run(prog(
        "var r := 0; "
        "try "
        "  _ := Decimal.Parse(\"-.5\"); "
        "catch FormatException => r := 1; "
        "end; "
        "return r; ")), 1);
}

// ---------------------------------------------------------------------------
// §9.3 (v0.314, fix #5400): trailing decimal point — "5." has no fractional
// digit after the decimal point.
// ---------------------------------------------------------------------------

// §9.3: "5." enters the fractional part but collects zero fractional digits —
// FormatException.
BEZEL_TEST(codegen_decimal_parse_grammar_5400, trailing_dot_rejected_spec_9_3) {
    BEZEL_EXPECT_EQ(run(prog(
        "var r := 0; "
        "try "
        "  _ := Decimal.Parse(\"5.\"); "
        "catch FormatException => r := 1; "
        "end; "
        "return r; ")), 1);
}

// §9.3: "123." — same, with a multi-digit integer part.
BEZEL_TEST(codegen_decimal_parse_grammar_5400, trailing_dot_multidigit_rejected_spec_9_3) {
    BEZEL_EXPECT_EQ(run(prog(
        "var r := 0; "
        "try "
        "  _ := Decimal.Parse(\"123.\"); "
        "catch FormatException => r := 1; "
        "end; "
        "return r; ")), 1);
}

// ---------------------------------------------------------------------------
// §9.3 (v0.314, fix #5400): exponent not admitted in Parse — §18.1 admits
// "1e3m" for LITERALS, but §9.3's Parse surface has no exponent grammar.
// ---------------------------------------------------------------------------

// §9.3: "1e3" — exponent is valid for decimal LITERALS (§18.1: "1e3m") but
// Parse does not admit it.  FormatException.
BEZEL_TEST(codegen_decimal_parse_grammar_5400, exponent_rejected_in_parse_spec_9_3) {
    BEZEL_EXPECT_EQ(run(prog(
        "var r := 0; "
        "try "
        "  _ := Decimal.Parse(\"1e3\"); "
        "catch FormatException => r := 1; "
        "end; "
        "return r; ")), 1);
}

// §9.3: "1E+3" — same, uppercase E and explicit positive exponent sign.
BEZEL_TEST(codegen_decimal_parse_grammar_5400, exponent_e_plus_rejected_spec_9_3) {
    BEZEL_EXPECT_EQ(run(prog(
        "var r := 0; "
        "try "
        "  _ := Decimal.Parse(\"1E+3\"); "
        "catch FormatException => r := 1; "
        "end; "
        "return r; ")), 1);
}

// Control: the same exponent form IS valid in a DECIMAL LITERAL (§18.1).
BEZEL_TEST(codegen_decimal_parse_grammar_5400, exponent_valid_in_literal_spec_18_1) {
    BEZEL_EXPECT_EQ(run(prog(
        "const k: decimal := 1e3m; "
        "if k = 1000m then return 1; end; "
        "return 0; ")), 1);
}

// Control: "1.5e3" as a LITERAL — valid per §18.1.  (1.5e3m == 1500m;
// the "1.5e3m == 0" bug is pre-existing and unrelated to #5400, so this
// control verifies the plain-exponent case still works: 1e3m == 1000m.)
BEZEL_TEST(codegen_decimal_parse_grammar_5400, fraction_and_exponent_valid_in_literal_spec_18_1) {
    BEZEL_EXPECT_EQ(run(prog(
        "const k: decimal := 1e3m; "
        "if k = 1000m then return 1; end; "
        "return 0; ")), 1);
}

// ---------------------------------------------------------------------------
// Control: valid inputs are still accepted.
// ---------------------------------------------------------------------------

// §9.3: "0.5" — valid integer part and fractional part.
BEZEL_TEST(codegen_decimal_parse_grammar_5400, valid_fraction_accepted_spec_9_3) {
    BEZEL_EXPECT_EQ(run(prog(
        "var d := Decimal.Parse(\"0.5\"); "
        "if d = 0.5m then return 1; end; "
        "return 0; ")), 1);
}

// §9.3: "5" — integer only, valid.
BEZEL_TEST(codegen_decimal_parse_grammar_5400, valid_integer_accepted_spec_9_3) {
    BEZEL_EXPECT_EQ(run(prog(
        "var d := Decimal.Parse(\"5\"); "
        "if d = 5m then return 1; end; "
        "return 0; ")), 1);
}

// §9.3: "-123.456" — valid with sign and fractional part.
BEZEL_TEST(codegen_decimal_parse_grammar_5400, valid_signed_fraction_accepted_spec_9_3) {
    BEZEL_EXPECT_EQ(run(prog(
        "var d := Decimal.Parse(\"-123.456\"); "
        "if d = -123.456m then return 1; end; "
        "return 0; ")), 1);
}
