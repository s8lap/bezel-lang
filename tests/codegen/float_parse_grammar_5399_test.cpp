// Regression tests for #5399 (stdlib §9.2 v0.314).
//
// The spec's float/double Parse/TryParse grammar is:
//   [+-]? [0-9]+ (\.[0-9]+)? ([eE][+-]?[0-9]+)?
// Anything else → FormatException.  Specifically rejected:
//   ".5"  (no integer part before decimal)
//   "5."  (no fractional digits)
//   "1e"  (no exponent digits)
//   "1_000" (digit separator)
//   "0x1p3" (hex float form)
//
// JSON ReadFloat (§2.15 v0.314) accepts any JSON number, rounds once to
// double, and raises SerializationException where the rounded value is not
// finite.  The JSON number grammar requires at least one digit after '.'
// and after the exponent sign.
//
// The existing overflow rows (double.Parse("1e999"), etc.) stay valid and
// continue to raise OverflowException.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

std::string prog(const std::string& body) {
    return "module M; "
           "static class U "
           "  public static method TryIt<T>(s: string, out v: T?): bool "
           "      where T: IParsable<T> "
           "    return T.TryParse(s, out v); "
           "  end TryIt; "
           "end U; "
           "class C public static method Main(): int " + body +
           " return 0; end Main; end C;";
}

// Prints FMT on FormatException, OVF on OverflowException, else the value.
std::string guarded(const std::string& expr) {
    return "try Console.WriteLine(" + expr + "); "
           "catch OverflowException => Console.WriteLine(\"OVF\"); "
           "catch FormatException  => Console.WriteLine(\"FMT\"); end; ";
}

std::string out(const std::string& body) { return runAotOut(prog(body)); }

}  // namespace

// ===========================================================================
// §9.2: double.Parse rejects invalid grammar forms
// ===========================================================================
BEZEL_TEST(float_parse_grammar_5399, double_parse_bare_point_rejected) {
    BEZEL_EXPECT_EQ(out(guarded("double.Parse(\".5\")")), "FMT");
}
BEZEL_TEST(float_parse_grammar_5399, double_parse_trailing_point_rejected) {
    BEZEL_EXPECT_EQ(out(guarded("double.Parse(\"5.\")")), "FMT");
}
BEZEL_TEST(float_parse_grammar_5399, double_parse_incomplete_exponent_rejected) {
    BEZEL_EXPECT_EQ(out(guarded("double.Parse(\"1e\")")), "FMT");
}
BEZEL_TEST(float_parse_grammar_5399, double_parse_digit_separator_rejected) {
    BEZEL_EXPECT_EQ(out(guarded("double.Parse(\"1_000\")")), "FMT");
}
BEZEL_TEST(float_parse_grammar_5399, double_parse_hex_float_rejected) {
    BEZEL_EXPECT_EQ(out(guarded("double.Parse(\"0x1p3\")")), "FMT");
}
// §9.2 (v0.314, #5444): signed hex float also rejected.
BEZEL_TEST(float_parse_grammar_5399, double_parse_signed_hex_float_minus_rejected) {
    BEZEL_EXPECT_EQ(out(guarded("double.Parse(\"-0x1p3\")")), "FMT");
}
BEZEL_TEST(float_parse_grammar_5399, double_parse_signed_hex_float_plus_rejected) {
    BEZEL_EXPECT_EQ(out(guarded("double.Parse(\"+0x10\")")), "FMT");
}

// ---- float.Parse -----------------------------------------------------------
BEZEL_TEST(float_parse_grammar_5399, float_parse_bare_point_rejected) {
    BEZEL_EXPECT_EQ(out(guarded("float.Parse(\".5\")")), "FMT");
}
BEZEL_TEST(float_parse_grammar_5399, float_parse_trailing_point_rejected) {
    BEZEL_EXPECT_EQ(out(guarded("float.Parse(\"5.\")")), "FMT");
}
BEZEL_TEST(float_parse_grammar_5399, float_parse_incomplete_exponent_rejected) {
    BEZEL_EXPECT_EQ(out(guarded("float.Parse(\"1e\")")), "FMT");
}
// §9.2 (v0.314, #5444): signed hex float also rejected.
BEZEL_TEST(float_parse_grammar_5399, float_parse_signed_hex_float_minus_rejected) {
    BEZEL_EXPECT_EQ(out(guarded("float.Parse(\"-0x1p3\")")), "FMT");
}

// ===========================================================================
// §9.2: double.TryParse returns false for invalid grammar forms
// ===========================================================================
BEZEL_TEST(float_parse_grammar_5399, double_tryparse_bare_point_false) {
    BEZEL_EXPECT_EQ(out(
        "var v := 7.0; var ok := double.TryParse(\".5\", out v); "
        "Console.WriteLine(ok); Console.WriteLine(v = 0.0); "),
        "False\nTrue");
}
BEZEL_TEST(float_parse_grammar_5399, double_tryparse_trailing_point_false) {
    BEZEL_EXPECT_EQ(out(
        "var v := 7.0; var ok := double.TryParse(\"5.\", out v); "
        "Console.WriteLine(ok); Console.WriteLine(v = 0.0); "),
        "False\nTrue");
}
BEZEL_TEST(float_parse_grammar_5399, double_tryparse_incomplete_exponent_false) {
    BEZEL_EXPECT_EQ(out(
        "var v := 7.0; var ok := double.TryParse(\"1e\", out v); "
        "Console.WriteLine(ok); Console.WriteLine(v = 0.0); "),
        "False\nTrue");
}
BEZEL_TEST(float_parse_grammar_5399, double_tryparse_hex_float_false) {
    BEZEL_EXPECT_EQ(out(
        "var v := 7.0; var ok := double.TryParse(\"0x1p3\", out v); "
        "Console.WriteLine(ok); Console.WriteLine(v = 0.0); "),
        "False\nTrue");
}
// §9.2 (v0.314, #5444): signed hex float also returns false.
BEZEL_TEST(float_parse_grammar_5399, double_tryparse_signed_hex_float_minus_false) {
    BEZEL_EXPECT_EQ(out(
        "var v := 7.0; var ok := double.TryParse(\"-0x1p3\", out v); "
        "Console.WriteLine(ok); Console.WriteLine(v = 0.0); "),
        "False\nTrue");
}
BEZEL_TEST(float_parse_grammar_5399, float_tryparse_signed_hex_float_minus_false) {
    BEZEL_EXPECT_EQ(out(
        "var v := 7.0f; var ok := float.TryParse(\"-0x1p3\", out v); "
        "Console.WriteLine(ok); Console.WriteLine(v = 0.0f); "),
        "False\nTrue");
}

// ===========================================================================
// §2.3 / §9.2: Convert.ToDouble rejects invalid grammar forms
// ===========================================================================
BEZEL_TEST(float_parse_grammar_5399, convert_todouble_bare_point_rejected) {
    BEZEL_EXPECT_EQ(out(guarded("Convert.ToDouble(\".5\")")), "FMT");
}
BEZEL_TEST(float_parse_grammar_5399, convert_todouble_trailing_point_rejected) {
    BEZEL_EXPECT_EQ(out(guarded("Convert.ToDouble(\"5.\")")), "FMT");
}
BEZEL_TEST(float_parse_grammar_5399, convert_todouble_incomplete_exponent_rejected) {
    BEZEL_EXPECT_EQ(out(guarded("Convert.ToDouble(\"1e\")")), "FMT");
}
// §9.2 (v0.314, #5444): signed hex float also rejected.
BEZEL_TEST(float_parse_grammar_5399, convert_todouble_signed_hex_float_minus_rejected) {
    BEZEL_EXPECT_EQ(out(guarded("Convert.ToDouble(\"-0x1p3\")")), "FMT");
}
BEZEL_TEST(float_parse_grammar_5399, convert_todouble_signed_hex_float_plus_rejected) {
    BEZEL_EXPECT_EQ(out(guarded("Convert.ToDouble(\"+0x10\")")), "FMT");
}

// ===========================================================================
// §2.3 / §9.2: Convert.TryToDouble returns false for invalid grammar forms
// ===========================================================================
BEZEL_TEST(float_parse_grammar_5399, convert_trytodouble_bare_point_false) {
    BEZEL_EXPECT_EQ(out(
        "var v := 7.0; var ok := Convert.TryToDouble(\".5\", out v); "
        "Console.WriteLine(ok); Console.WriteLine(v = 0.0); "),
        "False\nTrue");
}
BEZEL_TEST(float_parse_grammar_5399, convert_trytodouble_trailing_point_false) {
    BEZEL_EXPECT_EQ(out(
        "var v := 7.0; var ok := Convert.TryToDouble(\"5.\", out v); "
        "Console.WriteLine(ok); Console.WriteLine(v = 0.0); "),
        "False\nTrue");
}
// §9.2 (v0.314, #5444): signed hex float also returns false.
BEZEL_TEST(float_parse_grammar_5399, convert_trytodouble_signed_hex_float_minus_false) {
    BEZEL_EXPECT_EQ(out(
        "var v := 7.0; var ok := Convert.TryToDouble(\"-0x1p3\", out v); "
        "Console.WriteLine(ok); Console.WriteLine(v = 0.0); "),
        "False\nTrue");
}

// ===========================================================================
// §2.3 / §9.2: format-before-range — "1_000e999" raises FormatException,
// not OverflowException, per stdlib §9.2 (v0.314, #5199/#5368).
// ===========================================================================
BEZEL_TEST(float_parse_grammar_5399, double_parse_format_before_range_rejected) {
    BEZEL_EXPECT_EQ(out(guarded("double.Parse(\"1_000e999\")")), "FMT");
}

// ===========================================================================
// §9.2: valid inputs still work (control — no row flip)
// ===========================================================================
BEZEL_TEST(float_parse_grammar_5399, double_parse_valid_integer_still_works) {
    BEZEL_EXPECT_EQ(out(guarded("double.Parse(\"5\")")), "5");
}
BEZEL_TEST(float_parse_grammar_5399, double_parse_valid_fraction_still_works) {
    BEZEL_EXPECT_EQ(out(guarded("double.Parse(\"1.5\")")), "1.5");
}
BEZEL_TEST(float_parse_grammar_5399, double_parse_valid_exponent_still_works) {
    BEZEL_EXPECT_EQ(out(guarded("double.Parse(\"1e10\")")), "1e+10");
}
BEZEL_TEST(float_parse_grammar_5399, double_parse_underflow_still_zero) {
    BEZEL_EXPECT_EQ(out(
        "Console.WriteLine(double.Parse(\"1e-400\") = 0.0); "), "True");
}

// ===========================================================================
// §2.15 (JSON ReadFloat): SerializationException on non-finite numbers
// and invalid JSON number forms
// ===========================================================================
// ===========================================================================
// §2.15 (JSON ReadFloat): SerializationException on non-finite numbers
// and invalid JSON number forms.  These also cover the runtime changes made
// as part of the #5399 fix (bz_jder_parse_float: finiteness check and
// last-character validation).
// ===========================================================================
BEZEL_TEST(float_parse_grammar_5399, json_readfloat_1e999_not_finite) {
    // §2.15 (v0.314, #5200/#5369): a JSON number whose rounded value is
    // not finite raises SerializationException.
    auto r = runAot(
        "module M; "
        "import System.Serialization; "
        "record R(V: double): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  var json: string := \"{\\\"V\\\": 1e999}\"; "
        "  using var d := new JsonDeserializer(json); "
        "  var caught: bool := false; "
        "  try "
        "    var v := R.Deserialize(d); "
        "    return 1; "
        "  catch SerializationException => caught := true; end; "
        "  return (if caught then 42 else 1); "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 42);
}

BEZEL_TEST(float_parse_grammar_5399, json_readfloat_bare_point_invalid_form) {
    // §2.15 (v0.314, #5200/#5369): "1." is not a valid JSON number form
    // (JSON grammar requires at least one digit after the decimal point).
    auto r = runAot(
        "module M; "
        "import System.Serialization; "
        "record R(V: double): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  var json: string := \"{\\\"V\\\": 1.}\"; "
        "  using var d := new JsonDeserializer(json); "
        "  var caught: bool := false; "
        "  try "
        "    var v := R.Deserialize(d); "
        "    return 1; "
        "  catch SerializationException => caught := true; end; "
        "  return (if caught then 42 else 1); "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 42);
}

BEZEL_TEST(float_parse_grammar_5399, json_readfloat_incomplete_exponent_invalid_form) {
    // §2.15 (v0.314, #5200/#5369): "1e" is not a valid JSON number form
    // (JSON grammar requires at least one digit after the exponent sign).
    auto r = runAot(
        "module M; "
        "import System.Serialization; "
        "record R(V: double): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  var json: string := \"{\\\"V\\\": 1e}\"; "
        "  using var d := new JsonDeserializer(json); "
        "  var caught: bool := false; "
        "  try "
        "    var v := R.Deserialize(d); "
        "    return 1; "
        "  catch SerializationException => caught := true; end; "
        "  return (if caught then 42 else 1); "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 42);
}

BEZEL_TEST(float_parse_grammar_5399, json_readfloat_valid_1e_400_underflow_is_zero) {
    // Control: a valid JSON float that underflows to zero is in range (§2.15).
    auto r = runAot(
        "module M; "
        "import System.Serialization; "
        "record R(V: double): ISerializable, IDeserializable; "
        "class C public static method Main(): int "
        "  var json: string := \"{\\\"V\\\": 1e-400}\"; "
        "  using var d := new JsonDeserializer(json); "
        "  var v := R.Deserialize(d); "
        "  return (if v.V = 0.0 then 42 else 1); "
        "end Main; end C;");
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, 42);
}
