// Regression tests for #7301 (stdlib §2.18, v0.344; stdlib §2.3, v0.344):
// a non-finite `double` or `float` rendered as "inf" / "-inf" / "nan" where
// the document names "Infinity" / "-Infinity" / "NaN".
//
// THE SPEC SENTENCE (docs/Bezel_Standard_Library.md line 1162, stdlib §2.18,
// v0.344):
//
//   "**Non-finite `double` and `float` values.** The no-format `ToString()`
//   and every floating specifier (`F`, `E`, `G` and `P`, with or without a
//   digit count, and `R`) render a NaN as `NaN`, positive infinity as
//   `Infinity` and negative infinity as `-Infinity` — the names of the `NaN`,
//   `PositiveInfinity` and `NegativeInfinity` constants (§9.2) — with no
//   digits, no exponent and no `%` suffix: `$"{1.0 / 0.0}"` and
//   `(1.0 / 0.0).ToString("P2")` both render `"Infinity"`. Every NaN renders
//   `NaN`, whatever its sign bit and payload. `Parse` reads each of the three
//   back (§9.2); for a NaN the round trip yields `NaN`, equal to the original
//   under `=` (language spec §4.3.1), and does not carry its sign or payload
//   bits, which `BitConverter` (§2.12) carries."
//
// AND the document's own worked example (docs/Bezel_Standard_Library.md line
// 222, stdlib §2.3, v0.344):
//
//   "… `Convert.ToString(true)` is `"True"`, `Convert.ToString(0.1f)` is
//   `"0.1"` and `Convert.ToString(1.0 / 0.0)` is `"Infinity"`."
//
// THE DEFECT. Every rendering of a non-finite value printed C's own "%g"
// spelling — "inf", "-inf", "nan" — so the compiler matched NEITHER of the
// document's two statements of the spelling.
//
// THE ORACLE. Every row below asserts the program's EXACT stdout, together
// with "it ran" and "exit code 0". A row that asserted only that the program
// compiles would be green on the pre-fix arm (the pre-fix compiler COMPILES
// these programs — it prints the wrong text), so the text is the oracle.
//
// THE CONTROL ROWS are the FINITE renderings: they must be byte-identical on
// both arms, so a control row that is green on both arms is the proof that
// the fix did not over-reach into the finite values.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// A program that must RUN CLEANLY — no crash, no sanitizer report — and
// print the expected text.
//
// The exit-status assertion is LOAD-BEARING: under
// `BEZEL_AOT_FORCE_SANITIZE=address` an AddressSanitizer report replaces the
// program's own exit status with the sanitizer's, so a row that asserted only
// the OUTPUT would still fail on a crashing arm — but a row that asserted only
// "it compiled" would pass.
void expectCleanRunWithOutput(const std::string& src,
                               const std::string& want) {
    JitResult r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, want);
}

// A program whose body is the given statements, printing one line each.
std::string prog(const std::string& body) {
    return "module Conf;\n"
           "public class Program\n"
           "  public static method Main(): int\n" + body +
           "    return 0;\n"
           "  end Main;\n"
           "end Program;\n";
}

// The three non-finite double values, bound by name.
const char* kNonFiniteDoubles =
    "    var pInf := double.PositiveInfinity;\n"
    "    var nInf := double.NegativeInfinity;\n"
    "    var nan := double.NaN;\n";

// The three non-finite float values, bound by name.
const char* kNonFiniteFloats =
    "    var pInf := float.PositiveInfinity;\n"
    "    var nInf := float.NegativeInfinity;\n"
    "    var nan := float.NaN;\n";

// The three names, one per line.
const std::string kThreeNames = "Infinity\n-Infinity\nNaN\n";

}  // namespace

// ---------------------------------------------------------------------------
// §2.18's first requirement — the SPELLING — under the no-format ToString().
// ---------------------------------------------------------------------------

BEZEL_TEST(nonfinite_rendering_7301, double_no_format_renders_the_three_names) {
    expectCleanRunWithOutput(
        prog(std::string(kNonFiniteDoubles) +
             "    Console.WriteLine(pInf.ToString());\n"
             "    Console.WriteLine(nInf.ToString());\n"
             "    Console.WriteLine(nan.ToString());\n"),
        kThreeNames);
}

// ---------------------------------------------------------------------------
// §2.18's second requirement — the COVERAGE: every floating specifier, with
// or without a digit count, and `R`.
// ---------------------------------------------------------------------------

BEZEL_TEST(nonfinite_rendering_7301, double_fixed_point_specifier_renders_the_three_names) {
    expectCleanRunWithOutput(
        prog(std::string(kNonFiniteDoubles) +
             "    Console.WriteLine(pInf.ToString(\"F2\"));\n"
             "    Console.WriteLine(nInf.ToString(\"F2\"));\n"
             "    Console.WriteLine(nan.ToString(\"F2\"));\n"),
        kThreeNames);
}

BEZEL_TEST(nonfinite_rendering_7301, double_scientific_specifier_renders_the_three_names) {
    expectCleanRunWithOutput(
        prog(std::string(kNonFiniteDoubles) +
             "    Console.WriteLine(pInf.ToString(\"E\"));\n"
             "    Console.WriteLine(nInf.ToString(\"E\"));\n"
             "    Console.WriteLine(nan.ToString(\"E\"));\n"),
        kThreeNames);
}

BEZEL_TEST(nonfinite_rendering_7301, double_general_specifier_renders_the_three_names) {
    expectCleanRunWithOutput(
        prog(std::string(kNonFiniteDoubles) +
             "    Console.WriteLine(pInf.ToString(\"G\"));\n"
             "    Console.WriteLine(nInf.ToString(\"G\"));\n"
             "    Console.WriteLine(nan.ToString(\"G\"));\n"),
        kThreeNames);
}

// §2.18: "with no digits, no exponent and no `%` suffix" — so the percent
// specifier renders the bare name, NOT "Infinity%".
BEZEL_TEST(nonfinite_rendering_7301, double_percent_specifier_renders_the_three_names_without_a_suffix) {
    expectCleanRunWithOutput(
        prog(std::string(kNonFiniteDoubles) +
             "    Console.WriteLine(pInf.ToString(\"P2\"));\n"
             "    Console.WriteLine(nInf.ToString(\"P2\"));\n"
             "    Console.WriteLine(nan.ToString(\"P2\"));\n"),
        kThreeNames);
}

BEZEL_TEST(nonfinite_rendering_7301, double_round_trip_specifier_renders_the_three_names) {
    expectCleanRunWithOutput(
        prog(std::string(kNonFiniteDoubles) +
             "    Console.WriteLine(pInf.ToString(\"R\"));\n"
             "    Console.WriteLine(nInf.ToString(\"R\"));\n"
             "    Console.WriteLine(nan.ToString(\"R\"));\n"),
        kThreeNames);
}

// A `float` is a §2.18 subject too ("Non-finite `double` and `float`
// values"), under every floating specifier.
BEZEL_TEST(nonfinite_rendering_7301, float_specifiers_render_the_three_names) {
    expectCleanRunWithOutput(
        prog(std::string(kNonFiniteFloats) +
             "    Console.WriteLine(pInf.ToString());\n"
             "    Console.WriteLine(nInf.ToString());\n"
             "    Console.WriteLine(nan.ToString());\n"
             "    Console.WriteLine(pInf.ToString(\"F2\"));\n"
             "    Console.WriteLine(nInf.ToString(\"F2\"));\n"
             "    Console.WriteLine(nan.ToString(\"F2\"));\n"
             "    Console.WriteLine(pInf.ToString(\"E\"));\n"
             "    Console.WriteLine(nInf.ToString(\"E\"));\n"
             "    Console.WriteLine(nan.ToString(\"E\"));\n"
             "    Console.WriteLine(pInf.ToString(\"G\"));\n"
             "    Console.WriteLine(nInf.ToString(\"G\"));\n"
             "    Console.WriteLine(nan.ToString(\"G\"));\n"
             "    Console.WriteLine(pInf.ToString(\"P2\"));\n"
             "    Console.WriteLine(nInf.ToString(\"P2\"));\n"
             "    Console.WriteLine(nan.ToString(\"P2\"));\n"
             "    Console.WriteLine(pInf.ToString(\"R\"));\n"
             "    Console.WriteLine(nInf.ToString(\"R\"));\n"
             "    Console.WriteLine(nan.ToString(\"R\"));\n"),
        std::string("Infinity\n-Infinity\nNaN\n") +
            "Infinity\n-Infinity\nNaN\n" +
            "Infinity\n-Infinity\nNaN\n" +
            "Infinity\n-Infinity\nNaN\n" +
            "Infinity\n-Infinity\nNaN\n" +
            "Infinity\n-Infinity\nNaN\n");
}

// ---------------------------------------------------------------------------
// §2.18's own example: `$"{1.0 / 0.0}"` renders `"Infinity"` — the
// INTERPOLATION HOLE is a rendering site of its own.
// ---------------------------------------------------------------------------

BEZEL_TEST(nonfinite_rendering_7301, interpolation_hole_renders_the_name) {
    expectCleanRunWithOutput(
        prog(std::string(kNonFiniteDoubles) +
             "    Console.WriteLine($\"{pInf}\");\n"
             "    Console.WriteLine($\"{nInf}\");\n"
             "    Console.WriteLine($\"{nan}\");\n"),
        kThreeNames);
}

// The hole WITH a format spec renders the same name — and the percent
// specifier adds no '%' suffix.
BEZEL_TEST(nonfinite_rendering_7301, interpolated_hole_with_a_specifier_renders_the_name) {
    expectCleanRunWithOutput(
        prog(std::string(kNonFiniteDoubles) +
             "    Console.WriteLine($\"{pInf:F2}\");\n"
             "    Console.WriteLine($\"{nan:E}\");\n"
             "    Console.WriteLine($\"{pInf:P2}\");\n"),
        "Infinity\nNaN\nInfinity\n");
}

// ---------------------------------------------------------------------------
// §2.3's own worked example: `Convert.ToString(1.0 / 0.0)` is `"Infinity"`.
// ---------------------------------------------------------------------------

BEZEL_TEST(nonfinite_rendering_7301, convert_to_string_renders_the_name) {
    expectCleanRunWithOutput(
        prog(std::string(kNonFiniteDoubles) +
             "    Console.WriteLine(Convert.ToString(pInf));\n"
             "    Console.WriteLine(Convert.ToString(nInf));\n"
             "    Console.WriteLine(Convert.ToString(nan));\n"),
        kThreeNames);
}

// ---------------------------------------------------------------------------
// §2.18's fourth requirement — the ROUND TRIP: "`Parse` reads each of the
// three back".
// ---------------------------------------------------------------------------

BEZEL_TEST(nonfinite_rendering_7301, parse_reads_the_three_names_back) {
    expectCleanRunWithOutput(
        prog("    Console.WriteLine(double.Parse(\"Infinity\").ToString());\n"
             "    Console.WriteLine(double.Parse(\"-Infinity\").ToString());\n"
             "    Console.WriteLine(double.Parse(\"NaN\").ToString());\n"
             "    Console.WriteLine(float.Parse(\"Infinity\").ToString());\n"
             "    Console.WriteLine(float.Parse(\"-Infinity\").ToString());\n"
             "    Console.WriteLine(float.Parse(\"NaN\").ToString());\n"),
        std::string("Infinity\n-Infinity\nNaN\n") +
            "Infinity\n-Infinity\nNaN\n");
}

// §2.18: "for a NaN the round trip yields `NaN`, equal to the original under
// `=` (language spec §4.3.1)" — §4.3.1's reflexive equality makes
// `NaN = NaN` true.
BEZEL_TEST(nonfinite_rendering_7301, parsed_nan_equals_the_original_nan) {
    expectCleanRunWithOutput(
        prog("    Console.WriteLine(double.Parse(\"NaN\") = double.NaN);\n"
             "    Console.WriteLine(double.Parse(\"Infinity\") = double.PositiveInfinity);\n"
             "    Console.WriteLine(double.Parse(\"-Infinity\") = double.NegativeInfinity);\n"),
        "True\nTrue\nTrue\n");
}

// ---------------------------------------------------------------------------
// §2.18's third requirement — the VALUE, NOT THE SPELLING: "Every NaN
// renders `NaN`, whatever its sign bit and payload." A NaN produced by
// `0.0 / 0.0` and one produced by `Math.Sqrt(-1.0)` render identically.
// ---------------------------------------------------------------------------

BEZEL_TEST(nonfinite_rendering_7301, nan_renders_by_value_not_by_origin) {
    expectCleanRunWithOutput(
        prog("    var z := 0.0;\n"
             "    Console.WriteLine((z / z).ToString());\n"
             "    Console.WriteLine(Math.Sqrt(-1.0).ToString());\n"
             "    Console.WriteLine(double.NaN.ToString());\n"),
        "NaN\nNaN\nNaN\n");
}

// ---------------------------------------------------------------------------
// THE CONTROL ROWS — the FINITE renderings. §2.18 fixes the NON-FINITE
// spellings only: every finite rendering must be byte-identical before and
// after the fix. These rows are green on BOTH arms, and that is the point:
// they are the over-reach check.
// ---------------------------------------------------------------------------

BEZEL_TEST(nonfinite_rendering_7301, control_finite_renderings_are_unchanged) {
    expectCleanRunWithOutput(
        prog("    Console.WriteLine(1.0.ToString());\n"
             "    Console.WriteLine(0.1.ToString());\n"
             "    Console.WriteLine((1.0 / 3.0).ToString(\"F4\"));\n"
             "    Console.WriteLine((1.0 / 3.0).ToString(\"R\"));\n"
             "    Console.WriteLine((1.0 / 3.0).ToString(\"E\"));\n"
             "    Console.WriteLine((1.0 / 3.0).ToString(\"G\"));\n"
             "    Console.WriteLine(0.1f.ToString());\n"
             "    Console.WriteLine((0.0 * -1.0).ToString());\n"
             "    Console.WriteLine((0.0 * -1.0).ToString(\"F2\"));\n"
             "    Console.WriteLine(1e21.ToString());\n"
             "    Console.WriteLine(1e-7.ToString());\n"),
        "1\n0.1\n0.3333\n0.3333333333333333\n3.33e-01\n0.33\n0.1\n-0\n-0.00\n"
        "1e+21\n1e-07\n");
}

// The FINITE interpolation holes (and the finite percent hole, which keeps
// its '%' suffix — a non-finite value has none).
BEZEL_TEST(nonfinite_rendering_7301, control_finite_interpolation_is_unchanged) {
    expectCleanRunWithOutput(
        prog("    Console.WriteLine($\"{1.0 / 3.0:F4}\");\n"
             "    Console.WriteLine($\"{1.0 / 3.0:E}\");\n"
             "    Console.WriteLine($\"{1.0 / 3.0:G}\");\n"
             "    Console.WriteLine($\"{1.0 / 3.0:P}\");\n"
             "    Console.WriteLine($\"{1.0 / 3.0:R}\");\n"
             "    Console.WriteLine($\"{42:F2}\");\n"
             "    Console.WriteLine($\"{0.1}\");\n"
             "    Console.WriteLine($\"{(0.0 * -1.0)}\");\n"),
        "0.3333\n3.333333E-01\n0.333333\n33.33%\n0.3333333333333333\n42.00\n0.1\n-0\n");
}

// The BOUNDARY of the fix: §2.18's non-finite sentence is about the VALUE the
// caller holds, so a FINITE operand keeps its rendering even where the
// percent specifier's ×100 overflows to an infinity ("inf%" here). The
// non-finite OPERAND, by contrast, renders the name ("Infinity" — pinned by
// the interpolated-hole row above).
BEZEL_TEST(nonfinite_rendering_7301, control_a_finite_operand_whose_percent_scale_overflows_is_unchanged) {
    expectCleanRunWithOutput(
        prog("    var big := 1e308;\n"
             "    Console.WriteLine($\"{big:P2}\");\n"),
        "inf%\n");
}

// The finite CONVERT row — §2.3's own other example: `Convert.ToString(0.1f)`
// is `"0.1"`.
BEZEL_TEST(nonfinite_rendering_7301, control_convert_to_string_of_a_finite_float_is_unchanged) {
    expectCleanRunWithOutput(
        prog("    Console.WriteLine(Convert.ToString(0.1f));\n"
             "    Console.WriteLine(Convert.ToString(0.1));\n"),
        "0.1\n0.1\n");
}

// The non-finite rendering takes a DIFFERENT branch (the runtime's name
// rendering, not the snprintf path), so the minted string of that branch must
// still be DISCHARGED exactly once.
//
// NO leak oracle is asserted here, and the reason is MEASURED: the
// `ToString(format)` result of this path is not discharged AT ALL — the
// FINITE spelling leaks it too, on the pre-fix arm as well (39 bytes for
// `x.ToString("F4")`, measured under `bezelc --sanitize=address`). That is a
// PRE-EXISTING defect of the `ToString(format)` path, not of the non-finite
// branch, and it is out of scope here — so a leak oracle on this path would
// be red for a reason this fix does not own. It is reported to the
// coordinator instead.
