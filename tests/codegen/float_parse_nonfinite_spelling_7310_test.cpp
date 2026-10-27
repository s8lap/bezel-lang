// Regression tests for #7310 (stdlib §9.2, v0.344): the floating-point
// `Parse` / `TryParse` arms admitted a whole FAMILY of non-finite
// spellings, where stdlib §9.2 admits exactly three of them.
//
// THE SPEC SENTENCE (docs/Bezel_Standard_Library.md, stdlib §9.2, v0.344):
//
//   "**Input grammar.** `Parse` accepts an optional sign (`+` or `-`), then
//   one or more of the digits `0`–`9`, then optionally a `.` followed by one
//   or more digits, then optionally an exponent — `e` or `E`, an optional
//   sign, and one or more digits — and nothing else: `"5"`, `"+5"`,
//   `"-0.25"`, `"007.5"` and `"6.02E+23"` are admitted. ... There is no
//   surrounding whitespace, no digit separator, no hexadecimal form and no
//   bare point, so `" 5"`, `"5 "`, `"1_000"`, `"0x1p3"`, `".5"` and `"5."`
//   raise `FormatException`, as does any other input apart from exactly
//   `"NaN"`, `"Infinity"` and `"-Infinity"`. Those three stand outside the
//   grammar: they are the renderings of the non-finite values (§2.18), and
//   `Parse` reads them back as `NaN`, `PositiveInfinity` and
//   `NegativeInfinity`, as §2.18's round trip requires. **They are matched
//   exactly, so `"nan"`, `"inf"`, `"infinity"`, `"INFINITY"`, `"+Infinity"`
//   and `"-NaN"` raise `FormatException`.**"
//
// AND §9.2's range paragraph, which this fix must not disturb:
//
//   "An input that spells a finite number whose value, so rounded, is not
//   finite ... raises `OverflowException` ... So `double.Parse("1e999")`,
//   `double.Parse("-1e999")` and `float.Parse("3.5e38")` raise. ... An
//   input the grammar does not admit raises `FormatException` whatever
//   number a part of it spells: `"1_000e999"` raises `FormatException`, not
//   the `OverflowException` below."
//
// THE DEFECT. The `double` / `float` `Parse` and `TryParse` arms decided
// "is this one of the non-finite spellings?" by SNIFFING THE FIRST CHARACTER
// (an `i`/`I`/`n`/`N`), which admits every spelling `strtod` / `strtof`
// accept: "inf", "infinity", "nan", "NAN", "Nan", "INF", "Inf", "+inf",
// "-inf", "+nan", "-nan", "nan(1)". §9.2 admits three EXACT strings only.
//
// THE ORACLE. Every row below asserts the program's EXACT stdout, together
// with "it ran" and "exit code 0". A row that asserted only that the program
// compiles would be green on the pre-fix arm — the pre-fix compiler COMPILES
// these programs and prints the wrong text — so the text is the oracle. The
// "raises FormatException" rows print "FMT" from a catch arm (so they also
// distinguish `FormatException` from `OverflowException`), and the
// "raises OverflowException" row prints "OVF".
//
// THE CONTROL ROWS are the inputs §9.2 admits and the inputs it rejects for
// reasons unrelated to the non-finite spellings: they must be byte-identical
// on both arms, so a control row that is green on both arms is the proof that
// the fix did not over-reach.

#include <string>
#include <vector>

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

// The three admitted non-finite spellings (§9.2).
const std::vector<std::string> kGood = {"NaN", "Infinity", "-Infinity"};

// The six spellings §9.2 names as raising `FormatException`.
const std::vector<std::string> kBad = {
    "nan", "inf", "infinity", "INFINITY", "+Infinity", "-NaN"};

// The remaining case/word variants of the same family — §9.2 admits only
// the three EXACT strings, so every one of these raises too.
const std::vector<std::string> kCaseVariants = {
    "NAN", "Nan", "INF", "Inf", "-inf", "+inf", "+nan", "-nan"};

// The examples §9.2 rejects.
const std::vector<std::string> kRejectedExamples = {
    " 5", "5 ", "1_000", "0x1p3", ".5", "5."};

// A program whose body is the given statements, printing one line each.
std::string prog(const std::string& body) {
    return "module Conf;\n"
           "public class Program\n"
           "  public static method Main(): int\n" + body +
           "    return 0;\n"
           "  end Main;\n"
           "end Program;\n";
}

// The program that prints, per input, "<input> -> <result>", where the
// result is the parsed value, "FMT" (FormatException) or "OVF"
// (OverflowException).  `type` is the receiver's type name ("double" /
// "float") and `method` the helper's name in the generated program.
std::string parseProg(const std::string& type, const std::string& method,
                      const std::vector<std::string>& inputs) {
    std::string body;
    for (const std::string& in : inputs) {
        body += "    " + method + "(\"" + in + "\");\n";
    }
    return "module Conf;\n"
           "public class Program\n"
           "  private static method " + method + "(s: string)\n"
           "    try\n"
           "      var v := " + type + ".Parse(s);\n"
           "      Console.WriteLine(s + \" -> \" + v.ToString());\n"
           "    catch\n"
           "      var e: FormatException => Console.WriteLine(s + \" -> FMT\"),\n"
           "      var e: OverflowException => Console.WriteLine(s + \" -> OVF\")\n"
           "    end;\n"
           "  end " + method + ";\n"
           "  public static method Main(): int\n" + body +
           "    return 0;\n"
           "  end Main;\n"
           "end Program;\n";
}

// The program that prints, per input, "<input> -> <True|False> / <value>",
// for the `TryParse` form.
std::string tryParseProg(const std::string& type, const std::string& method,
                         const std::vector<std::string>& inputs) {
    std::string body;
    for (const std::string& in : inputs) {
        body += "    " + method + "(\"" + in + "\");\n";
    }
    const std::string zero = (type == "float") ? "0.0f" : "0.0";
    return "module Conf;\n"
           "public class Program\n"
           "  private static method " + method + "(s: string)\n"
           "    var v := " + zero + ";\n"
           "    var ok := " + type + ".TryParse(s, out v);\n"
           "    Console.WriteLine(s + \" -> \" + ok.ToString() + \" / \" + "
           "v.ToString());\n"
           "  end " + method + ";\n"
           "  public static method Main(): int\n" + body +
           "    return 0;\n"
           "  end Main;\n"
           "end Program;\n";
}

// The expected stdout of `parseProg` / `tryParseProg`: one "<input> ->
// <result>" line per input.
std::string wantParsed(const std::vector<std::string>& inputs,
                       const std::vector<std::string>& results) {
    std::string want;
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        want += inputs[i] + " -> " + results[i] + "\n";
    }
    return want;
}

// The expected stdout of `tryParseProg`.
std::string wantTried(const std::vector<std::string>& inputs,
                       const std::vector<std::string>& results) {
    return wantParsed(inputs, results);
}

// A program that parses one span window per input and prints
// "<input> -> <result>", as `parseProg` does but through the
// `ReadOnlyStringSpan` overload of §9.2.
std::string spanParseProg(const std::string& type, const std::string& method,
                           const std::vector<std::string>& inputs) {
    std::string body;
    for (const std::string& in : inputs) {
        body += "    " + method + "(\"" + in + "\", " +
                std::to_string(in.size()) + ");\n";
    }
    return "module Conf;\n"
           "public class Program\n"
           "  private static method " + method + "(s: string, n: int)\n"
           "    var sp := s.AsSpan(0, n);\n"
           "    try\n"
           "      var v := " + type + ".Parse(sp);\n"
           "      Console.WriteLine(s + \" -> \" + v.ToString());\n"
           "    catch\n"
           "      var e: FormatException => Console.WriteLine(s + \" -> FMT\"),\n"
           "      var e: OverflowException => Console.WriteLine(s + \" -> OVF\")\n"
           "    end;\n"
           "  end " + method + ";\n"
           "  public static method Main(): int\n" + body +
           "    return 0;\n"
           "  end Main;\n"
           "end Program;\n";
}

// A program that tries one span window per input and prints
// "<input> -> <True|False> / <value>".
std::string spanTryParseProg(const std::string& type, const std::string& method,
                              const std::vector<std::string>& inputs) {
    std::string body;
    for (const std::string& in : inputs) {
        body += "    " + method + "(\"" + in + "\", " +
                std::to_string(in.size()) + ");\n";
    }
    return "module Conf;\n"
           "public class Program\n"
           "  private static method " + method + "(s: string, n: int)\n"
           "    var sp := s.AsSpan(0, n);\n"
           "    var v := " + ((type == "float") ? "0.0f" : "0.0") + ";\n"
           "    var ok := " + type + ".TryParse(sp, out v);\n"
           "    Console.WriteLine(s + \" -> \" + ok.ToString() + \" / \" + "
           "v.ToString());\n"
           "  end " + method + ";\n"
           "  public static method Main(): int\n" + body +
           "    return 0;\n"
           "  end Main;\n"
           "end Program;\n";
}

}  // namespace

// ---------------------------------------------------------------------------
// §9.2's rule, first side: the SIX spellings it names raise FormatException.
// ---------------------------------------------------------------------------

BEZEL_TEST(float_parse_nonfinite_spelling_7310,
           double_parse_rejects_the_six_non_finite_spellings_spec_9_2) {
    expectCleanRunWithOutput(
        parseProg("double", "ShowD", kBad),
        wantParsed(kBad, {"FMT", "FMT", "FMT", "FMT", "FMT", "FMT"}));
}

BEZEL_TEST(float_parse_nonfinite_spelling_7310,
           float_parse_rejects_the_six_non_finite_spellings_spec_9_2) {
    expectCleanRunWithOutput(
        parseProg("float", "ShowF", kBad),
        wantParsed(kBad, {"FMT", "FMT", "FMT", "FMT", "FMT", "FMT"}));
}

// The `TryParse` form: §9.2's "TryParse returns false in every case where
// Parse would raise", and the `out` result receives the default.
BEZEL_TEST(float_parse_nonfinite_spelling_7310,
           double_try_parse_rejects_the_six_non_finite_spellings_spec_9_2) {
    expectCleanRunWithOutput(
        tryParseProg("double", "ShowTD", kBad),
        wantTried(kBad, {"False / 0", "False / 0", "False / 0", "False / 0",
                         "False / 0", "False / 0"}));
}

BEZEL_TEST(float_parse_nonfinite_spelling_7310,
           float_try_parse_rejects_the_six_non_finite_spellings_spec_9_2) {
    expectCleanRunWithOutput(
        tryParseProg("float", "ShowTF", kBad),
        wantTried(kBad, {"False / 0", "False / 0", "False / 0", "False / 0",
                         "False / 0", "False / 0"}));
}

// The rest of the family the first-character sniff admitted.
BEZEL_TEST(float_parse_nonfinite_spelling_7310,
           double_parse_rejects_the_case_variants_of_the_non_finite_spellings_spec_9_2) {
    expectCleanRunWithOutput(
        parseProg("double", "ShowD", kCaseVariants),
        wantParsed(kCaseVariants,
                   {"FMT", "FMT", "FMT", "FMT", "FMT", "FMT", "FMT", "FMT"}));
}

// ---------------------------------------------------------------------------
// §9.2's rule, second side: the THREE exact spellings are still admitted.
// ---------------------------------------------------------------------------

BEZEL_TEST(float_parse_nonfinite_spelling_7310,
           double_parse_admits_the_three_exact_non_finite_spellings_spec_9_2) {
    expectCleanRunWithOutput(
        parseProg("double", "ShowD", kGood),
        wantParsed(kGood, {"NaN", "Infinity", "-Infinity"}));
}

BEZEL_TEST(float_parse_nonfinite_spelling_7310,
           float_parse_admits_the_three_exact_non_finite_spellings_spec_9_2) {
    expectCleanRunWithOutput(
        parseProg("float", "ShowF", kGood),
        wantParsed(kGood, {"NaN", "Infinity", "-Infinity"}));
}

BEZEL_TEST(float_parse_nonfinite_spelling_7310,
           double_try_parse_admits_the_three_exact_non_finite_spellings_spec_9_2) {
    expectCleanRunWithOutput(
        tryParseProg("double", "ShowTD", kGood),
        wantTried(kGood, {"True / NaN", "True / Infinity",
                          "True / -Infinity"}));
}

BEZEL_TEST(float_parse_nonfinite_spelling_7310,
           float_try_parse_admits_the_three_exact_non_finite_spellings_spec_9_2) {
    expectCleanRunWithOutput(
        tryParseProg("float", "ShowTF", kGood),
        wantTried(kGood, {"True / NaN", "True / Infinity",
                          "True / -Infinity"}));
}

// ---------------------------------------------------------------------------
// The CONTROL rows: §9.2's own numeric examples, and §9.2's own rejected
// examples. Both are byte-identical on the pre-fix and the post-fix arm.
// ---------------------------------------------------------------------------

// The numeric examples §9.2 admits, checked by VALUE (not by rendering):
// each parse must succeed and spell the value the example names.
BEZEL_TEST(float_parse_nonfinite_spelling_7310,
           double_parse_admits_the_spec_own_numeric_examples_spec_9_2) {
    expectCleanRunWithOutput(
        prog("    Console.WriteLine(double.Parse(\"5\") = 5.0);\n"
             "    Console.WriteLine(double.Parse(\"+5\") = 5.0);\n"
             "    Console.WriteLine(double.Parse(\"-0.25\") = -0.25);\n"
             "    Console.WriteLine(double.Parse(\"007.5\") = 7.5);\n"
             "    Console.WriteLine(double.Parse(\"6.02E+23\") = 6.02e23);\n"),
        "True\nTrue\nTrue\nTrue\nTrue\n");
}

BEZEL_TEST(float_parse_nonfinite_spelling_7310,
           float_parse_admits_the_spec_own_numeric_examples_spec_9_2) {
    expectCleanRunWithOutput(
        prog("    Console.WriteLine(float.Parse(\"5\") = 5.0f);\n"
             "    Console.WriteLine(float.Parse(\"+5\") = 5.0f);\n"
             "    Console.WriteLine(float.Parse(\"-0.25\") = -0.25f);\n"
             "    Console.WriteLine(float.Parse(\"007.5\") = 7.5f);\n"
             "    Console.WriteLine(float.Parse(\"6.02E+23\") = 6.02e23f);\n"),
        "True\nTrue\nTrue\nTrue\nTrue\n");
}

// The examples §9.2 rejects — each must raise FormatException.
BEZEL_TEST(float_parse_nonfinite_spelling_7310,
           double_parse_raises_on_the_spec_own_rejected_examples_spec_9_2) {
    expectCleanRunWithOutput(
        parseProg("double", "ShowD", kRejectedExamples),
        wantParsed(kRejectedExamples,
                   {"FMT", "FMT", "FMT", "FMT", "FMT", "FMT"}));
}

BEZEL_TEST(float_parse_nonfinite_spelling_7310,
           float_parse_raises_on_the_spec_own_rejected_examples_spec_9_2) {
    expectCleanRunWithOutput(
        parseProg("float", "ShowF", kRejectedExamples),
        wantParsed(kRejectedExamples,
                   {"FMT", "FMT", "FMT", "FMT", "FMT", "FMT"}));
}

// ---------------------------------------------------------------------------
// §9.2's SPAN overload carries the string forms' contract — "the span forms
// carry the string forms' contract" (§9.2's range paragraph).
// ---------------------------------------------------------------------------

BEZEL_TEST(float_parse_nonfinite_spelling_7310,
           double_parse_span_rejects_the_six_non_finite_spellings_spec_9_2) {
    expectCleanRunWithOutput(
        spanParseProg("double", "ShowSD", kBad),
        wantParsed(kBad, {"FMT", "FMT", "FMT", "FMT", "FMT", "FMT"}));
}

BEZEL_TEST(float_parse_nonfinite_spelling_7310,
           double_parse_span_admits_the_three_exact_non_finite_spellings_spec_9_2) {
    expectCleanRunWithOutput(
        spanParseProg("double", "ShowSD", kGood),
        wantParsed(kGood, {"NaN", "Infinity", "-Infinity"}));
}

BEZEL_TEST(float_parse_nonfinite_spelling_7310,
           double_try_parse_span_rejects_the_six_non_finite_spellings_spec_9_2) {
    expectCleanRunWithOutput(
        spanTryParseProg("double", "ShowSTD", kBad),
        wantTried(kBad, {"False / 0", "False / 0", "False / 0", "False / 0",
                         "False / 0", "False / 0"}));
}

// ---------------------------------------------------------------------------
// §9.2: "There is no surrounding whitespace" — a trailing space is not part
// of the admitted spelling, so "NaN " raises (a "starts with 'N'" or a
// "trim" reading would wrongly admit it).
// ---------------------------------------------------------------------------

BEZEL_TEST(float_parse_nonfinite_spelling_7310,
           double_parse_rejects_a_non_finite_spelling_with_a_trailing_space_spec_9_2) {
    std::vector<std::string> inputs = {"NaN ", "Infinity ", "-Infinity "};
    expectCleanRunWithOutput(
        parseProg("double", "ShowD", inputs),
        wantParsed(inputs, {"FMT", "FMT", "FMT"}));
}

// ---------------------------------------------------------------------------
// §9.2's range paragraph — the OVERFLOW boundary. The exactness rule is
// about the INPUT TEXT, so a finite spelling that overflows still raises
// OverflowException, and a spelling the grammar does not admit still raises
// FormatException "whatever number a part of it spells".
// ---------------------------------------------------------------------------

BEZEL_TEST(float_parse_nonfinite_spelling_7310,
           double_parse_raises_overflow_for_a_finite_spelling_that_overflows_spec_9_2) {
    std::vector<std::string> inputs = {"1e999", "-1e999"};
    expectCleanRunWithOutput(
        parseProg("double", "ShowD", inputs),
        wantParsed(inputs, {"OVF", "OVF"}));
}

BEZEL_TEST(float_parse_nonfinite_spelling_7310,
           double_parse_raises_format_for_a_rejected_spelling_that_also_overflows_spec_9_2) {
    std::vector<std::string> inputs = {"1_000e999"};
    expectCleanRunWithOutput(
        parseProg("double", "ShowD", inputs),
        wantParsed(inputs, {"FMT"}));
}

// ---------------------------------------------------------------------------
// §2.18's round trip, through the Parse arms this fix touches: "Parse reads
// each of the three back".
// ---------------------------------------------------------------------------

BEZEL_TEST(float_parse_nonfinite_spelling_7310,
           double_parse_round_trip_renders_the_three_names_spec_2_18) {
    expectCleanRunWithOutput(
        prog("    Console.WriteLine(double.Parse(\"NaN\").ToString());\n"
             "    Console.WriteLine(double.Parse(\"Infinity\").ToString());\n"
             "    Console.WriteLine(double.Parse(\"-Infinity\").ToString());\n"
             "    Console.WriteLine(float.Parse(\"NaN\").ToString());\n"
             "    Console.WriteLine(float.Parse(\"Infinity\").ToString());\n"
             "    Console.WriteLine(float.Parse(\"-Infinity\").ToString());\n"),
        "NaN\nInfinity\n-Infinity\nNaN\nInfinity\n-Infinity\n");
}
