// Regression tests for #5198 (spec-ahead-of-impl, stdlib §9.2 / §2.3 v0.313,
// #5193): binary-float `Parse` raises `OverflowException` where the input
// spells a finite number whose value, rounded once to the type as spec §4.3
// (v0.312) rounds a literal, is not finite; `TryParse` returns `false` (and the
// out receives the default, or `null` through `IParsable<T>`'s `out T?`);
// `Convert.ToDouble` raises / `Convert.TryToDouble` returns `false` alike
// (stdlib §2.3's mirror sentence). Underflow (rounding to zero or a subnormal)
// is in range. The `float` half rounds with the correctly rounded `strtof`,
// never `(float)strtod` (§4.3 v0.312, #5194).
//
// Stdlib §9.2: `double.Parse("1.7976931348623158e308")` is `double.MaxValue`,
// `float.Parse("3.4028235e38")` is `float.MaxValue`, while
// `double.Parse("1e999")`, `double.Parse("-1e999")` and `float.Parse("3.5e38")`
// raise; `double.Parse("1e-400")` is `0`.
//
// Every row asserts exact stdout. Rows tagged OVF printed `Infinity`/`-Infinity`
// style values (no exception) before the fix.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

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

// Statement printing OVF when `expr` raises OverflowException, FMT on
// FormatException, else the value.
std::string guarded(const std::string& expr) {
    return "try Console.WriteLine(" + expr + "); "
           "catch OverflowException => Console.WriteLine(\"OVF\"); "
           "catch FormatException => Console.WriteLine(\"FMT\"); end; ";
}

std::string out(const std::string& body) { return runAotOut(prog(body)); }

}  // namespace

// ---- double.Parse ---------------------------------------------------------
BEZEL_TEST(float_parse_overflow_5198, double_parse_1e999_overflow) {
    BEZEL_EXPECT_EQ(out(guarded("double.Parse(\"1e999\")")), "OVF");
}
BEZEL_TEST(float_parse_overflow_5198, double_parse_neg_1e999_overflow) {
    BEZEL_EXPECT_EQ(out(guarded("double.Parse(\"-1e999\")")), "OVF");
}
BEZEL_TEST(float_parse_overflow_5198, double_parse_plus_1e999_overflow) {
    BEZEL_EXPECT_EQ(out(guarded("double.Parse(\"+1e999\")")), "OVF");
}
BEZEL_TEST(float_parse_overflow_5198, double_parse_just_above_max_overflow) {
    // above MaxValue + half an ulp (1.797693134862315807e308) -> rounds to +inf.
    BEZEL_EXPECT_EQ(out(guarded("double.Parse(\"1.7976931348623159e308\")")), "OVF");
}
BEZEL_TEST(float_parse_overflow_5198, double_parse_just_above_max_negative_overflow) {
    BEZEL_EXPECT_EQ(out(guarded("double.Parse(\"-1.7976931348623159e308\")")), "OVF");
}
BEZEL_TEST(float_parse_overflow_5198, double_parse_max_boundary_is_max_value) {
    // control (passes before and after): within half an ulp of MaxValue.
    BEZEL_EXPECT_EQ(out(
        "Console.WriteLine(double.Parse(\"1.7976931348623158e308\") = double.MaxValue); "),
        "True");
}
BEZEL_TEST(float_parse_overflow_5198, double_parse_underflow_zero_in_range) {
    BEZEL_EXPECT_EQ(out(
        "Console.WriteLine(double.Parse(\"1e-400\") = 0.0); "), "True");
}
BEZEL_TEST(float_parse_overflow_5198, double_parse_subnormal_in_range) {
    BEZEL_EXPECT_EQ(out(
        "Console.WriteLine(double.Parse(\"5e-324\") = double.Epsilon); "), "True");
}
BEZEL_TEST(float_parse_overflow_5198, double_parse_ordinary_value) {
    BEZEL_EXPECT_EQ(out(guarded("double.Parse(\"1.5\")")), "1.5");
}
BEZEL_TEST(float_parse_overflow_5198, double_parse_infinity_spelling_round_trips) {
    // The overflow check must not fire on a spelling of an infinity (#4890).
    BEZEL_EXPECT_EQ(out(
        "Console.WriteLine(double.Parse(double.PositiveInfinity.ToString()) "
        "= double.PositiveInfinity); "), "True");
}
BEZEL_TEST(float_parse_overflow_5198, double_parse_garbage_is_format_exception) {
    BEZEL_EXPECT_EQ(out(guarded("double.Parse(\"abc\")")), "FMT");
}

// ---- float.Parse ----------------------------------------------------------
BEZEL_TEST(float_parse_overflow_5198, float_parse_3_5e38_overflow) {
    BEZEL_EXPECT_EQ(out(guarded("float.Parse(\"3.5e38\")")), "OVF");
}
BEZEL_TEST(float_parse_overflow_5198, float_parse_neg_3_5e38_overflow) {
    BEZEL_EXPECT_EQ(out(guarded("float.Parse(\"-3.5e38\")")), "OVF");
}
BEZEL_TEST(float_parse_overflow_5198, float_parse_just_above_max_overflow) {
    // above FLT_MAX + half an ulp (2^128 - 2^103 = 3.40282356779733661e38).
    BEZEL_EXPECT_EQ(out(guarded("float.Parse(\"3.4028236e38\")")), "OVF");
}
BEZEL_TEST(float_parse_overflow_5198, float_parse_max_is_max_value) {
    BEZEL_EXPECT_EQ(out(
        "Console.WriteLine(float.Parse(\"3.4028235e38\") = float.MaxValue); "),
        "True");
}
BEZEL_TEST(float_parse_overflow_5198, float_parse_double_rounding_window_is_max_value) {
    // (float)strtod would give +inf here; the single correctly rounded strtof
    // result is FLT_MAX (§4.3 v0.312, #5194) -> in range.
    BEZEL_EXPECT_EQ(out(
        "Console.WriteLine(float.Parse(\"3.4028235677973366e38\") = float.MaxValue); "),
        "True");
}
BEZEL_TEST(float_parse_overflow_5198, float_parse_underflow_zero_in_range) {
    BEZEL_EXPECT_EQ(out(
        "Console.WriteLine(float.Parse(\"1e-50\") = 0.0f); "), "True");
}
BEZEL_TEST(float_parse_overflow_5198, float_parse_subnormal_in_range) {
    BEZEL_EXPECT_EQ(out(
        "Console.WriteLine(float.Parse(\"1e-45\") = float.Epsilon); "), "True");
}
BEZEL_TEST(float_parse_overflow_5198, float_parse_ordinary_value) {
    BEZEL_EXPECT_EQ(out(guarded("float.Parse(\"2.5\")")), "2.5");
}

// ---- TryParse -------------------------------------------------------------
BEZEL_TEST(float_parse_overflow_5198, double_tryparse_1e999_false_and_zero) {
    BEZEL_EXPECT_EQ(out(
        "var v := 7.0; var ok := double.TryParse(\"1e999\", out v); "
        "Console.WriteLine(ok); Console.WriteLine(v = 0.0); "), "False\nTrue");
}
BEZEL_TEST(float_parse_overflow_5198, double_tryparse_neg_1e999_false_and_zero) {
    BEZEL_EXPECT_EQ(out(
        "var v := 7.0; var ok := double.TryParse(\"-1e999\", out v); "
        "Console.WriteLine(ok); Console.WriteLine(v = 0.0); "), "False\nTrue");
}
BEZEL_TEST(float_parse_overflow_5198, double_tryparse_max_boundary_true) {
    BEZEL_EXPECT_EQ(out(
        "var v := 0.0; var ok := double.TryParse(\"1.7976931348623158e308\", out v); "
        "Console.WriteLine(ok); Console.WriteLine(v = double.MaxValue); "),
        "True\nTrue");
}
BEZEL_TEST(float_parse_overflow_5198, double_tryparse_underflow_true) {
    BEZEL_EXPECT_EQ(out(
        "var v := 7.0; var ok := double.TryParse(\"1e-400\", out v); "
        "Console.WriteLine(ok); Console.WriteLine(v = 0.0); "), "True\nTrue");
}
BEZEL_TEST(float_parse_overflow_5198, double_tryparse_ordinary_true) {
    BEZEL_EXPECT_EQ(out(
        "var v := 0.0; var ok := double.TryParse(\"1.5\", out v); "
        "Console.WriteLine(ok); Console.WriteLine(v); "), "True\n1.5");
}
BEZEL_TEST(float_parse_overflow_5198, float_tryparse_3_5e38_false_and_zero) {
    BEZEL_EXPECT_EQ(out(
        "var v := 7.0f; var ok := float.TryParse(\"3.5e38\", out v); "
        "Console.WriteLine(ok); Console.WriteLine(v = 0.0f); "), "False\nTrue");
}
BEZEL_TEST(float_parse_overflow_5198, float_tryparse_window_is_max_value_true) {
    BEZEL_EXPECT_EQ(out(
        "var v := 0.0f; var ok := float.TryParse(\"3.4028235677973366e38\", out v); "
        "Console.WriteLine(ok); Console.WriteLine(v = float.MaxValue); "),
        "True\nTrue");
}
BEZEL_TEST(float_parse_overflow_5198, float_tryparse_subnormal_true) {
    BEZEL_EXPECT_EQ(out(
        "var v := 0.0f; var ok := float.TryParse(\"1e-45\", out v); "
        "Console.WriteLine(ok); Console.WriteLine(v = float.Epsilon); "),
        "True\nTrue");
}

// ---- IParsable<T>.TryParse through the constraint (shape (a): out T?) -----
BEZEL_TEST(float_parse_overflow_5198, iparsable_double_1e999_false_and_null) {
    BEZEL_EXPECT_EQ(out(
        "var ok := U.TryIt<double>(\"1e999\", out var b); "
        "Console.WriteLine(ok); Console.WriteLine(b = null); "), "False\nTrue");
}
BEZEL_TEST(float_parse_overflow_5198, iparsable_float_3_5e38_false_and_null) {
    BEZEL_EXPECT_EQ(out(
        "var ok := U.TryIt<float>(\"3.5e38\", out var b); "
        "Console.WriteLine(ok); Console.WriteLine(b = null); "), "False\nTrue");
}
BEZEL_TEST(float_parse_overflow_5198, iparsable_double_ordinary_true) {
    BEZEL_EXPECT_EQ(out(
        "var ok := U.TryIt<double>(\"1.5\", out var b); "
        "Console.WriteLine(ok); Console.WriteLine(b!); "), "True\n1.5");
}

// ---- Convert.ToDouble / TryToDouble (stdlib §2.3) --------------------------
BEZEL_TEST(float_parse_overflow_5198, convert_todouble_1e999_overflow) {
    BEZEL_EXPECT_EQ(out(guarded("Convert.ToDouble(\"1e999\")")), "OVF");
}
BEZEL_TEST(float_parse_overflow_5198, convert_todouble_neg_1e999_overflow) {
    BEZEL_EXPECT_EQ(out(guarded("Convert.ToDouble(\"-1e999\")")), "OVF");
}
BEZEL_TEST(float_parse_overflow_5198, convert_todouble_underflow_in_range) {
    BEZEL_EXPECT_EQ(out(
        "Console.WriteLine(Convert.ToDouble(\"1e-400\") = 0.0); "), "True");
}
BEZEL_TEST(float_parse_overflow_5198, convert_todouble_ordinary_value) {
    BEZEL_EXPECT_EQ(out(guarded("Convert.ToDouble(\"1.5\")")), "1.5");
}
BEZEL_TEST(float_parse_overflow_5198, convert_trytodouble_1e999_false_and_zero) {
    BEZEL_EXPECT_EQ(out(
        "var v := 7.0; var ok := Convert.TryToDouble(\"1e999\", out v); "
        "Console.WriteLine(ok); Console.WriteLine(v = 0.0); "), "False\nTrue");
}
BEZEL_TEST(float_parse_overflow_5198, convert_trytodouble_neg_1e999_false_and_zero) {
    BEZEL_EXPECT_EQ(out(
        "var v := 7.0; var ok := Convert.TryToDouble(\"-1e999\", out v); "
        "Console.WriteLine(ok); Console.WriteLine(v = 0.0); "), "False\nTrue");
}
BEZEL_TEST(float_parse_overflow_5198, convert_trytodouble_underflow_true) {
    BEZEL_EXPECT_EQ(out(
        "var v := 7.0; var ok := Convert.TryToDouble(\"1e-400\", out v); "
        "Console.WriteLine(ok); Console.WriteLine(v = 0.0); "), "True\nTrue");
}
BEZEL_TEST(float_parse_overflow_5198, convert_trytodouble_ordinary_true) {
    BEZEL_EXPECT_EQ(out(
        "var v := 0.0; var ok := Convert.TryToDouble(\"1.5\", out v); "
        "Console.WriteLine(ok); Console.WriteLine(v); "), "True\n1.5");
}
