// Regression tests for #7324 — the `Convert` float arms (stdlib §2.3, §9.2,
// v0.344).
//
// stdlib §2.3 (docs/Bezel_Standard_Library.md:220, v0.344):
//
//   "The `To*` methods raise `FormatException` when the string is not a
//   well-formed representation of the target type and `OverflowException` when
//   the value does not fit — the same contract as the §9 per-type `Parse`
//   family these conversions mirror ... `Try` forms return `false` in exactly
//   the cases the throwing forms would raise; the `out` parameter receives the
//   default value."
//
// stdlib §9.2 (docs/Bezel_Standard_Library.md:5263, v0.344):
//
//   "`Parse` accepts an optional sign (`+` or `-`), then one or more of the
//   digits `0`–`9`, then optionally a `.` followed by one or more digits ... and
//   nothing else ... There is no surrounding whitespace, no digit separator, no
//   hexadecimal form and no bare point, so `" 5"`, `"5 "`, `"1_000"`, `"0x1p3"`,
//   `".5"` and `"5."` raise `FormatException`, as does any other input apart
//   from exactly `"NaN"`, `"Infinity"` and `"-Infinity"`. Those three stand
//   outside the grammar ... They are matched exactly, so `"nan"`, `"inf"`,
//   `"infinity"`, `"INFINITY"`, `"+Infinity"` and `"-NaN"` raise
//   `FormatException`."
//
// Two defects, both in `src/codegen/LlvmBackend_Calls.cpp` (the `Convert`
// family), are pinned here:
//
//   (a) the arms decided a non-finite spelling by SNIFFING THE FIRST CHARACTER
//       ('i'/'I'/'n'/'N'), so "inf", "nan", "infinity", "INFINITY",
//       "+Infinity" and "-NaN" were accepted;
//   (b) the "first character must be a digit" test WAIVED itself when the
//       first character was a sign, so a signed bare point ("+.5", "-.5") was
//       accepted.
//
// The oracle is `runAot` (the AOT path), not `runAotChecked`: a
// FormatException is a runtime outcome, observable from the printed text and
// the exit code, and needs no sanitizer — so this file adds no
// `runAotChecked` row and does not move the toolchain-skip ratchet.
//
// The two arms are checked against the SAME input matrix: the throwing form
// must raise FormatException exactly where the Try form returns false, and the
// Try form's `out` parameter must receive the default value (§2.3).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

std::string prog(const std::string& body) {
    return "module M; "
           "class C public static method Main(): int " + body +
           " return 0; end Main; end C;";
}

// Prints FMT on FormatException, OVF on OverflowException, else the value.
std::string guarded(const std::string& expr) {
    return "try Console.WriteLine(" + expr + "); "
           "catch OverflowException => Console.WriteLine(\"OVF\"); "
           "catch FormatException  => Console.WriteLine(\"FMT\"); end; ";
}

struct Run {
    bool ran = false;
    int exitCode = -1;
    std::string text;
};

Run runProg(const std::string& body) {
    auto r = runAot(prog(body));
    std::string text = r.stdoutText;
    if (!text.empty() && text.back() == '\n') {
        text.pop_back();
        if (!text.empty() && text.back() == '\r') text.pop_back();
    }
    return Run{r.ran, r.exitCode, text};
}

// The program COMPILED and RAN (a compile reject is a different outcome from a
// program that runs and throws), it exited normally, and it PRINTED the
// expected text.
void expectPrints(const std::string& body, const std::string& expected) {
    Run r = runProg(body);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.text, expected);
}

}  // namespace

// ===========================================================================
// (a) stdlib §9.2 (v0.344): the non-finite spellings are matched EXACTLY.
//     "nan", "inf", "infinity", "INFINITY", "+Infinity" and "-NaN" raise
//     FormatException; only "NaN", "Infinity" and "-Infinity" are admitted.
// ===========================================================================
BEZEL_TEST(convert_float_nonfinite_7324, convert_todouble_inf_spelling_rejected) {
    expectPrints(guarded("Convert.ToDouble(\"inf\")"), "FMT");
}
BEZEL_TEST(convert_float_nonfinite_7324, convert_todouble_nan_spelling_rejected) {
    expectPrints(guarded("Convert.ToDouble(\"nan\")"), "FMT");
}
BEZEL_TEST(convert_float_nonfinite_7324, convert_todouble_infinity_lowercase_rejected) {
    expectPrints(guarded("Convert.ToDouble(\"infinity\")"), "FMT");
}
BEZEL_TEST(convert_float_nonfinite_7324, convert_todouble_infinity_uppercase_rejected) {
    expectPrints(guarded("Convert.ToDouble(\"INFINITY\")"), "FMT");
}
BEZEL_TEST(convert_float_nonfinite_7324, convert_todouble_plus_infinity_rejected) {
    expectPrints(guarded("Convert.ToDouble(\"+Infinity\")"), "FMT");
}
BEZEL_TEST(convert_float_nonfinite_7324, convert_todouble_minus_nan_rejected) {
    expectPrints(guarded("Convert.ToDouble(\"-NaN\")"), "FMT");
}
BEZEL_TEST(convert_float_nonfinite_7324, convert_todouble_signed_inf_rejected) {
    expectPrints(guarded("Convert.ToDouble(\"+inf\")"), "FMT");
}
BEZEL_TEST(convert_float_nonfinite_7324, convert_todouble_nan_with_payload_rejected) {
    expectPrints(guarded("Convert.ToDouble(\"nan(0)\")"), "FMT");
}

// ===========================================================================
// (b) stdlib §9.2 (v0.344): one or more digits must follow the optional sign,
//     so a signed bare point is rejected.
// ===========================================================================
BEZEL_TEST(convert_float_nonfinite_7324, convert_todouble_signed_bare_point_plus_rejected) {
    expectPrints(guarded("Convert.ToDouble(\"+.5\")"), "FMT");
}
BEZEL_TEST(convert_float_nonfinite_7324, convert_todouble_signed_bare_point_minus_rejected) {
    expectPrints(guarded("Convert.ToDouble(\"-.5\")"), "FMT");
}

// ===========================================================================
// stdlib §2.3 (v0.344): the Try form returns false in exactly the cases the
// throwing form would raise, and the `out` parameter receives the DEFAULT
// value (0.0) — not the partial parse value.
// ===========================================================================
BEZEL_TEST(convert_float_nonfinite_7324, convert_trytodouble_inf_spelling_false) {
    expectPrints(
        "var v := 7.0; var ok := Convert.TryToDouble(\"inf\", out v); "
        "Console.WriteLine(ok); Console.WriteLine(v = 0.0); ",
        "False\nTrue");
}
BEZEL_TEST(convert_float_nonfinite_7324, convert_trytodouble_nan_spelling_false) {
    expectPrints(
        "var v := 7.0; var ok := Convert.TryToDouble(\"nan\", out v); "
        "Console.WriteLine(ok); Console.WriteLine(v = 0.0); ",
        "False\nTrue");
}
BEZEL_TEST(convert_float_nonfinite_7324, convert_trytodouble_infinity_lowercase_false) {
    expectPrints(
        "var v := 7.0; var ok := Convert.TryToDouble(\"infinity\", out v); "
        "Console.WriteLine(ok); Console.WriteLine(v = 0.0); ",
        "False\nTrue");
}
BEZEL_TEST(convert_float_nonfinite_7324, convert_trytodouble_infinity_uppercase_false) {
    expectPrints(
        "var v := 7.0; var ok := Convert.TryToDouble(\"INFINITY\", out v); "
        "Console.WriteLine(ok); Console.WriteLine(v = 0.0); ",
        "False\nTrue");
}
BEZEL_TEST(convert_float_nonfinite_7324, convert_trytodouble_plus_infinity_false) {
    expectPrints(
        "var v := 7.0; var ok := Convert.TryToDouble(\"+Infinity\", out v); "
        "Console.WriteLine(ok); Console.WriteLine(v = 0.0); ",
        "False\nTrue");
}
BEZEL_TEST(convert_float_nonfinite_7324, convert_trytodouble_minus_nan_false) {
    expectPrints(
        "var v := 7.0; var ok := Convert.TryToDouble(\"-NaN\", out v); "
        "Console.WriteLine(ok); Console.WriteLine(v = 0.0); ",
        "False\nTrue");
}
BEZEL_TEST(convert_float_nonfinite_7324, convert_trytodouble_signed_bare_point_plus_false) {
    expectPrints(
        "var v := 7.0; var ok := Convert.TryToDouble(\"+.5\", out v); "
        "Console.WriteLine(ok); Console.WriteLine(v = 0.0); ",
        "False\nTrue");
}
BEZEL_TEST(convert_float_nonfinite_7324, convert_trytodouble_signed_bare_point_minus_false) {
    expectPrints(
        "var v := 7.0; var ok := Convert.TryToDouble(\"-.5\", out v); "
        "Console.WriteLine(ok); Console.WriteLine(v = 0.0); ",
        "False\nTrue");
}

// ===========================================================================
// CONTROLS — the three admitted spellings and the ordinary grammar are
// unchanged (stdlib §9.2, v0.344).
// ===========================================================================
BEZEL_TEST(convert_float_nonfinite_7324, convert_todouble_nan_exact_spelling_accepted) {
    expectPrints(guarded("Convert.ToDouble(\"NaN\")"), "NaN");
}
BEZEL_TEST(convert_float_nonfinite_7324, convert_todouble_infinity_exact_spelling_accepted) {
    expectPrints(guarded("Convert.ToDouble(\"Infinity\")"), "Infinity");
}
BEZEL_TEST(convert_float_nonfinite_7324, convert_todouble_minus_infinity_exact_spelling_accepted) {
    expectPrints(guarded("Convert.ToDouble(\"-Infinity\")"), "-Infinity");
}
BEZEL_TEST(convert_float_nonfinite_7324, convert_todouble_integer_unchanged) {
    expectPrints(guarded("Convert.ToDouble(\"5\")"), "5");
}
BEZEL_TEST(convert_float_nonfinite_7324, convert_todouble_signed_integer_unchanged) {
    expectPrints(guarded("Convert.ToDouble(\"+5\")"), "5");
}
BEZEL_TEST(convert_float_nonfinite_7324, convert_todouble_negative_fraction_unchanged) {
    expectPrints(guarded("Convert.ToDouble(\"-0.25\")"), "-0.25");
}
BEZEL_TEST(convert_float_nonfinite_7324, convert_todouble_leading_zeros_unchanged) {
    expectPrints(guarded("Convert.ToDouble(\"007.5\")"), "7.5");
}
BEZEL_TEST(convert_float_nonfinite_7324, convert_todouble_exponent_unchanged) {
    expectPrints(guarded("Convert.ToDouble(\"6.02E+23\")"), "6.02e+23");
}
BEZEL_TEST(convert_float_nonfinite_7324, convert_todouble_bare_point_still_rejected) {
    expectPrints(guarded("Convert.ToDouble(\".5\")"), "FMT");
}
BEZEL_TEST(convert_float_nonfinite_7324, convert_todouble_trailing_point_still_rejected) {
    expectPrints(guarded("Convert.ToDouble(\"5.\")"), "FMT");
}
BEZEL_TEST(convert_float_nonfinite_7324, convert_todouble_leading_space_still_rejected) {
    expectPrints(guarded("Convert.ToDouble(\" 5\")"), "FMT");
}
BEZEL_TEST(convert_float_nonfinite_7324, convert_todouble_digit_separator_still_rejected) {
    expectPrints(guarded("Convert.ToDouble(\"1_000\")"), "FMT");
}
BEZEL_TEST(convert_float_nonfinite_7324, convert_todouble_hex_float_still_rejected) {
    expectPrints(guarded("Convert.ToDouble(\"0x1p3\")"), "FMT");
}
BEZEL_TEST(convert_float_nonfinite_7324, convert_todouble_format_before_range_unchanged) {
    // §9.2 (v0.344): "1_000e999" raises FormatException, not OverflowException.
    expectPrints(guarded("Convert.ToDouble(\"1_000e999\")"), "FMT");
}
BEZEL_TEST(convert_float_nonfinite_7324, convert_todouble_overflow_unchanged) {
    expectPrints(guarded("Convert.ToDouble(\"1e999\")"), "OVF");
}
BEZEL_TEST(convert_float_nonfinite_7324, convert_trytodouble_nan_exact_spelling_true) {
    expectPrints(
        "var v := 0.0; var ok := Convert.TryToDouble(\"NaN\", out v); "
        "Console.WriteLine(ok); Console.WriteLine(v); ",
        "True\nNaN");
}
BEZEL_TEST(convert_float_nonfinite_7324, convert_trytodouble_infinity_exact_spelling_true) {
    expectPrints(
        "var v := 0.0; var ok := Convert.TryToDouble(\"Infinity\", out v); "
        "Console.WriteLine(ok); Console.WriteLine(v); ",
        "True\nInfinity");
}
BEZEL_TEST(convert_float_nonfinite_7324, convert_trytodouble_minus_infinity_exact_spelling_true) {
    expectPrints(
        "var v := 0.0; var ok := Convert.TryToDouble(\"-Infinity\", out v); "
        "Console.WriteLine(ok); Console.WriteLine(v); ",
        "True\n-Infinity");
}
BEZEL_TEST(convert_float_nonfinite_7324, convert_trytodouble_integer_unchanged) {
    expectPrints(
        "var v := 0.0; var ok := Convert.TryToDouble(\"5\", out v); "
        "Console.WriteLine(ok); Console.WriteLine(v); ",
        "True\n5");
}
