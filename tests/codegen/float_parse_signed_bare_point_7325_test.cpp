// Regression tests for #7325 (stdlib §9.2, v0.344): the floating-point
// `Parse` / `TryParse` arms admitted a SIGNED BARE POINT — `"+.5"` / `"-.5"` —
// where stdlib §9.2's input grammar rejects them.
//
// THE SPEC SENTENCE (docs/Bezel_Standard_Library.md, stdlib §9.2, v0.344 —
// line 5263):
//
//   "**Input grammar.** `Parse` accepts an optional sign (`+` or `-`), then
//   one or more of the digits `0`–`9`, then optionally a `.` followed by one
//   or more digits, then optionally an exponent — `e` or `E`, an optional
//   sign, and one or more digits — and nothing else: `"5"`, `"+5"`,
//   `"-0.25"`, `"007.5"` and `"6.02E+23"` are admitted. ... There is no
//   surrounding whitespace, no digit separator, no hexadecimal form and no
//   bare point, so `" 5"`, `"5 "`, `"1_000"`, `"0x1p3"`, `".5"` and `"5."`
//   raise `FormatException`, as does any other input apart from exactly
//   `"NaN"`, `"Infinity"` and `"-Infinity"`. ..."
//
// AND the neighbouring sentence:
//
//   "`TryParse` returns `false` in every case where `Parse` would raise, and
//   the span forms carry the string forms' contract."
//
// THE AXIS IS FLOAT vs EVERYTHING ELSE, NOT SIGNED vs UNSIGNED.  §9.2's
// grammar puts the DIGITS AFTER THE SIGN, so `"+.5"` has ZERO digits before
// the point and does not match — it is rejected for exactly the reason
// `".5"` is.  Every INTEGER and `decimal` arm already rejects it
// (`Int32.Parse("+.5")`, `UInt32.Parse("+.5")`, `Int64.Parse("+.5")`,
// `UInt64.Parse("+.5")`, `decimal.Parse("+.5")`, `Convert.ToInt32("+.5")` and
// `Convert.ToDecimal("+.5")` all raise `FormatException`); only the
// FLOATING arms disagreed.
//
// THE DEFECT.  The floating arms' "the numeric part must start with a digit"
// test was applied to the FIRST CHARACTER of the input, and the test was
// WAIVED whenever that character was a sign — so a sign exempted the digit
// rule entirely, and `"+.5"` was admitted even though it has no digit at
// all.  The character AFTER the sign (the one the hex and non-finite checks
// already read) was never consulted by the digit test.
//
// THE ORACLE.  Every row below asserts the program's EXACT stdout, together
// with "it ran" and with the exit status.  The exit status is the second,
// independent witness: each parse contributes a code to it (0 = the parse
// SUCCEEDED, 1 = `FormatException`, 2 = `OverflowException`), so
//   * "the program ran and the `FormatException` was caught"  = ran, exit 1,
//     and the text "FMT";
//   * "the compiler rejected the program"                    = NOT ran
//     (`runAot` reports `ran == false`), so no exit code at all.
// The exit status is NOT the value channel (a process status is masked to
// 0..255), so the TEXT carries the value and the exception kind; the status
// only says WHICH path ran.
//
// THE CONTROL ROWS are §9.2's own admitted examples, §9.2's own rejected
// examples, the three non-finite spellings, and the range paragraph's
// overflow boundary.  They must be byte-identical on the pre-fix and the
// post-fix arm — a control row that is green on both arms is the proof that
// the fix did not over-reach.
//
// The JSON number grammar (stdlib §2.15) is NOT §9.2's and is not touched:
// `JsonDeserializer.ReadFloat` is lowered to the runtime's
// `bz_binder_read_float`, which never reaches these arms.

#include <string>
#include <utility>
#include <vector>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// The outcome of ONE parse, as a code the program's EXIT STATUS carries:
//   0 = the parse SUCCEEDED, 1 = FormatException, 2 = OverflowException.
constexpr int kParsed = 0;
constexpr int kFormat = 1;
constexpr int kOverflow = 2;

// Run a program on the AOT path.  The caller asserts THREE things about the
// result: `ran` (false when the COMPILER rejected the program, which is a
// different outcome from "the program ran and raised"), the exit status
// (which carries the parse outcome) and the exact stdout (which carries the
// value and the exception kind).
JitResult runWith(const std::string& src) { return runAot(src); }

// The parse arms' operand: §9.2 declares both the `string` form and the
// `ReadOnlyStringSpan` form, and the span form carries the string form's
// contract ("the span forms carry the string forms' contract", §9.2).
std::string operand(bool span) { return span ? "s.AsSpan()" : "s"; }

// A program that parses each input in turn and prints "<input> -> OK" (the
// parse succeeded), "<input> -> FMT" (a `FormatException` was caught) or
// "<input> -> OVF" (an `OverflowException` was caught), and whose exit
// status is the SUM of the per-input outcome codes.
std::string parseOutcomeProg(const std::string& type,
                              const std::vector<std::string>& inputs,
                              bool span = false) {
    std::string body;
    for (const std::string& in : inputs) {
        body += "    total := total + Parse1(\"" + in + "\");\n";
    }
    return "module Conf;\n"
           "public class Program\n"
           "  public static method Main(): int\n"
           "    var total: int := 0;\n" + body +
           "    return total;\n"
           "  end Main;\n"
           "  private static method Parse1(s: string): int\n"
           "    try\n"
           "      var v := " + type + ".Parse(" + operand(span) + ");\n"
           "      Console.WriteLine(s + \" -> OK\");\n"
           "      return " + std::to_string(kParsed) + ";\n"
           "    catch\n"
           "      var e: FormatException => do Console.WriteLine(s + \" -> FMT\"); "
           "return " + std::to_string(kFormat) + "; end,\n"
           "      var e: OverflowException => do Console.WriteLine(s + \" -> OVF\"); "
           "return " + std::to_string(kOverflow) + "; end\n"
           "    end;\n"
           "  end Parse1;\n"
           "end Program;\n";
}

// A program that parses each input in turn and prints "<input> -> <True|False>
// / <value>" — the `TryParse` form — and whose exit status is the SUM of the
// per-input outcome codes (0 = the parse succeeded, 1 = it did not).
std::string tryParseOutcomeProg(const std::string& type,
                                 const std::vector<std::string>& inputs,
                                 bool span = false) {
    std::string body;
    for (const std::string& in : inputs) {
        body += "    total := total + Try1(\"" + in + "\");\n";
    }
    const std::string zero = (type == "float") ? "0.0f" : "0.0";
    return "module Conf;\n"
           "public class Program\n"
           "  public static method Main(): int\n"
           "    var total: int := 0;\n" + body +
           "    return total;\n"
           "  end Main;\n"
           "  private static method Try1(s: string): int\n"
           "    var v := " + zero + ";\n"
           "    var ok := " + type + ".TryParse(" + operand(span) + ", out v);\n"
           "    Console.WriteLine(s + \" -> \" + ok.ToString() + \" / \" + "
           "v.ToString());\n"
           "    return (if ok then " + std::to_string(kParsed) + " else " +
           std::to_string(kFormat) + ");\n"
           "  end Try1;\n"
           "end Program;\n";
}

// A program that parses each input and prints the parsed value's RENDERING,
// one per line.  It pins the value, not just "the parse succeeded".
std::string parseRenderingProg(const std::string& type,
                               const std::vector<std::string>& inputs) {
    std::string body;
    for (const std::string& in : inputs) {
        body += "    Render1(\"" + in + "\");\n";
    }
    return "module Conf;\n"
           "public class Program\n"
           "  public static method Main(): int\n" + body +
           "    return 0;\n"
           "  end Main;\n"
           "  private static method Render1(s: string): int\n"
           "    try\n"
           "      var v := " + type + ".Parse(s);\n"
           "      Console.WriteLine(v.ToString());\n"
           "    catch\n"
           "      var e: FormatException => Console.WriteLine(\"FMT\"),\n"
           "      var e: OverflowException => Console.WriteLine(\"OVF\")\n"
           "    end;\n"
           "    return 0;\n"
           "  end Render1;\n"
           "end Program;\n";
}

// A program that parses each input and prints whether the parsed value EQUALS
// the expected literal — the numeric examples are checked BY VALUE, not by
// rendering (§2.18's rendering of a floating value is a separate question).
// A parse that raises is UNCAUGHT here, so the program exits non-zero and
// the row fails.
std::string parseValueProg(
    const std::string& type,
    const std::vector<std::pair<std::string, std::string>>& inputs) {
    std::string body;
    for (const auto& in : inputs) {
        body += "    Value1(\"" + in.first + "\", " + in.second + ");\n";
    }
    return "module Conf;\n"
           "public class Program\n"
           "  public static method Main(): int\n" + body +
           "    return 0;\n"
           "  end Main;\n"
           "  private static method Value1(s: string, want: " + type + "): int\n"
           "    var v := " + type + ".Parse(s);\n"
           "    Console.WriteLine(v = want);\n"
           "    return 0;\n"
           "  end Value1;\n"
           "end Program;\n";
}

// A program that parses a WINDOW of a longer source — the span overload over
// a `ReadOnlyStringSpan` that is not the whole string.
std::string parseWindowProg(const std::string& type, const std::string& source,
                             int start, int len) {
    return "module Conf;\n"
           "public class Program\n"
           "  public static method Main(): int\n"
           "    try\n"
           "      var sp := new ReadOnlyStringSpan(\"" + source + "\", " +
           std::to_string(start) + ", " + std::to_string(len) + ");\n"
           "      var v := " + type + ".Parse(sp);\n"
           "      Console.WriteLine(\"-> OK\");\n"
           "      return " + std::to_string(kParsed) + ";\n"
           "    catch\n"
           "      var e: FormatException => do Console.WriteLine(\"-> FMT\"); "
           "return " + std::to_string(kFormat) + "; end,\n"
           "      var e: OverflowException => do Console.WriteLine(\"-> OVF\"); "
           "return " + std::to_string(kOverflow) + "; end\n"
           "    end;\n"
           "  end Main;\n"
           "end Program;\n";
}

// The exit status a `parseOutcomeProg` / `tryParseOutcomeProg` program
// returns for a table of outcomes.
int sumOf(const std::vector<int>& codes) {
    int total = 0;
    for (int c : codes) total += c;
    return total;
}

// The expected text of `parseOutcomeProg` for a table of outcomes.
std::string wantOutcome(const std::vector<std::string>& inputs,
                         const std::vector<std::string>& results) {
    std::string want;
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        want += inputs[i] + " -> " + results[i] + "\n";
    }
    return want;
}

// The expected text of `tryParseOutcomeProg` for a table of outcomes.
std::string wantTried(const std::vector<std::string>& inputs,
                      const std::vector<std::string>& results) {
    return wantOutcome(inputs, results);
}

}  // namespace

// ===========================================================================
// §9.2: the SIGNED BARE POINT — the defect.
// ===========================================================================

// The string form, `double`.
BEZEL_TEST(float_parse_signed_bare_point_7325,
           double_parse_rejects_a_signed_bare_point_spec_9_2) {
    JitResult r = runWith(parseOutcomeProg("double", {"+.5"}));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "+.5 -> FMT\n");
    BEZEL_EXPECT_EQ(r.exitCode, kFormat);
}

BEZEL_TEST(float_parse_signed_bare_point_7325,
           double_parse_rejects_a_signed_negative_bare_point_spec_9_2) {
    JitResult r = runWith(parseOutcomeProg("double", {"-.5"}));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "-.5 -> FMT\n");
    BEZEL_EXPECT_EQ(r.exitCode, kFormat);
}

// The string form, `float` — the same axis (FLOAT), so it must agree.
BEZEL_TEST(float_parse_signed_bare_point_7325,
           float_parse_rejects_a_signed_bare_point_spec_9_2) {
    JitResult r = runWith(parseOutcomeProg("float", {"+.5"}));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "+.5 -> FMT\n");
    BEZEL_EXPECT_EQ(r.exitCode, kFormat);
}

BEZEL_TEST(float_parse_signed_bare_point_7325,
           float_parse_rejects_a_signed_negative_bare_point_spec_9_2) {
    JitResult r = runWith(parseOutcomeProg("float", {"-.5"}));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "-.5 -> FMT\n");
    BEZEL_EXPECT_EQ(r.exitCode, kFormat);
}

// The `TryParse` form — §9.2: "TryParse returns false in every case
// where Parse would raise", and the `out` result receives the default.
BEZEL_TEST(float_parse_signed_bare_point_7325,
           double_try_parse_returns_false_for_a_signed_bare_point_spec_9_2) {
    JitResult r = runWith(tryParseOutcomeProg("double", {"+.5"}));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "+.5 -> False / 0\n");
    BEZEL_EXPECT_EQ(r.exitCode, kFormat);
}

BEZEL_TEST(float_parse_signed_bare_point_7325,
           double_try_parse_returns_false_for_a_signed_negative_bare_point_spec_9_2) {
    JitResult r = runWith(tryParseOutcomeProg("double", {"-.5"}));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "-.5 -> False / 0\n");
    BEZEL_EXPECT_EQ(r.exitCode, kFormat);
}

BEZEL_TEST(float_parse_signed_bare_point_7325,
           float_try_parse_returns_false_for_a_signed_bare_point_spec_9_2) {
    JitResult r = runWith(tryParseOutcomeProg("float", {"+.5"}));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "+.5 -> False / 0\n");
    BEZEL_EXPECT_EQ(r.exitCode, kFormat);
}

// The SPAN forms — §9.2: "the span forms carry the string forms'
// contract", so the span form must reject exactly what the string form
// rejects.
BEZEL_TEST(float_parse_signed_bare_point_7325,
           double_parse_span_rejects_a_signed_bare_point_spec_9_2) {
    JitResult r = runWith(parseOutcomeProg("double", {"+.5"}, true));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "+.5 -> FMT\n");
    BEZEL_EXPECT_EQ(r.exitCode, kFormat);
}

BEZEL_TEST(float_parse_signed_bare_point_7325,
           double_parse_span_rejects_a_signed_negative_bare_point_spec_9_2) {
    JitResult r = runWith(parseOutcomeProg("double", {"-.5"}, true));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "-.5 -> FMT\n");
    BEZEL_EXPECT_EQ(r.exitCode, kFormat);
}

BEZEL_TEST(float_parse_signed_bare_point_7325,
           float_parse_span_rejects_a_signed_bare_point_spec_9_2) {
    JitResult r = runWith(parseOutcomeProg("float", {"+.5"}, true));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "+.5 -> FMT\n");
    BEZEL_EXPECT_EQ(r.exitCode, kFormat);
}

BEZEL_TEST(float_parse_signed_bare_point_7325,
           double_try_parse_span_returns_false_for_a_signed_bare_point_spec_9_2) {
    JitResult r = runWith(tryParseOutcomeProg("double", {"+.5"}, true));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "+.5 -> False / 0\n");
    BEZEL_EXPECT_EQ(r.exitCode, kFormat);
}

// A WINDOW of a longer source — the window's own text is the parse input,
// and it is a signed bare point here too.
BEZEL_TEST(float_parse_signed_bare_point_7325,
           double_parse_span_window_rejects_a_signed_bare_point_spec_9_2) {
    JitResult r = runWith(parseWindowProg("double", "x+.5", 1, 3));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "-> FMT\n");
    BEZEL_EXPECT_EQ(r.exitCode, kFormat);
}

// ===========================================================================
// §9.2 CONTROL: the spec's own ADMITTED examples (checked BY VALUE).
// ===========================================================================

BEZEL_TEST(float_parse_signed_bare_point_7325,
           double_parse_admits_the_spec_own_numeric_examples_spec_9_2) {
    JitResult r = runWith(parseValueProg(
        "double", {{"5", "5.0"}, {"+5", "5.0"}, {"-0.25", "-0.25"},
                   {"007.5", "7.5"}, {"6.02E+23", "6.02e23"}}));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\nTrue\nTrue\nTrue\nTrue\n");
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

BEZEL_TEST(float_parse_signed_bare_point_7325,
           float_parse_admits_the_spec_own_numeric_examples_spec_9_2) {
    JitResult r = runWith(parseValueProg(
        "float", {{"5", "5.0f"}, {"+5", "5.0f"}, {"-0.25", "-0.25f"},
                  {"007.5", "7.5f"}, {"6.02E+23", "6.02e23f"}}));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\nTrue\nTrue\nTrue\nTrue\n");
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// The SIGN is still admitted before the DIGITS — the fix rejects a sign with
// NO digit after it, not a sign.
BEZEL_TEST(float_parse_signed_bare_point_7325,
           double_parse_admits_a_sign_before_the_digits_spec_9_2) {
    JitResult r = runWith(parseValueProg(
        "double", {{"+0.5", "0.5"}, {"-0.5", "-0.5"}, {"+007.5", "7.5"}}));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\nTrue\nTrue\n");
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

BEZEL_TEST(float_parse_signed_bare_point_7325,
           float_parse_admits_a_sign_before_the_digits_spec_9_2) {
    JitResult r = runWith(parseValueProg(
        "float", {{"+0.5", "0.5f"}, {"-0.5", "-0.5f"}, {"+007.5", "7.5f"}}));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\nTrue\nTrue\n");
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// §9.2 CONTROL: the spec's own REJECTED examples — every one still raises
// `FormatException`, and the SPAN form agrees with the string form.
// ===========================================================================

BEZEL_TEST(float_parse_signed_bare_point_7325,
           double_parse_raises_on_the_spec_own_rejected_examples_spec_9_2) {
    std::vector<std::string> inputs = {".5", "5.", " 5", "5 ", "1_000",
                                       "0x1p3"};
    JitResult r = runWith(parseOutcomeProg("double", inputs));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    wantOutcome(inputs, {"FMT", "FMT", "FMT", "FMT", "FMT",
                                         "FMT"}));
    BEZEL_EXPECT_EQ(r.exitCode, sumOf({kFormat, kFormat, kFormat, kFormat,
                                        kFormat, kFormat}));
}

BEZEL_TEST(float_parse_signed_bare_point_7325,
           float_parse_raises_on_the_spec_own_rejected_examples_spec_9_2) {
    std::vector<std::string> inputs = {".5", "5.", " 5", "5 ", "1_000",
                                       "0x1p3"};
    JitResult r = runWith(parseOutcomeProg("float", inputs));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    wantOutcome(inputs, {"FMT", "FMT", "FMT", "FMT", "FMT",
                                         "FMT"}));
    BEZEL_EXPECT_EQ(r.exitCode, sumOf({kFormat, kFormat, kFormat, kFormat,
                                        kFormat, kFormat}));
}

BEZEL_TEST(float_parse_signed_bare_point_7325,
           double_parse_span_raises_on_the_spec_own_rejected_examples_spec_9_2) {
    std::vector<std::string> inputs = {".5", "5."};
    JitResult r = runWith(parseOutcomeProg("double", inputs, true));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    wantOutcome(inputs, {"FMT", "FMT"}));
    BEZEL_EXPECT_EQ(r.exitCode, sumOf({kFormat, kFormat}));
}

// ===========================================================================
// §9.2 CONTROL: the non-finite spellings (#7310's rows must not regress).
// ===========================================================================

BEZEL_TEST(float_parse_signed_bare_point_7325,
           double_parse_raises_on_the_non_finite_family_spellings_spec_9_2) {
    std::vector<std::string> inputs = {"inf", "nan"};
    JitResult r = runWith(parseOutcomeProg("double", inputs));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, wantOutcome(inputs, {"FMT", "FMT"}));
    BEZEL_EXPECT_EQ(r.exitCode, sumOf({kFormat, kFormat}));
}

BEZEL_TEST(float_parse_signed_bare_point_7325,
           double_parse_admits_the_three_exact_non_finite_spellings_spec_9_2) {
    std::vector<std::string> inputs = {"NaN", "Infinity", "-Infinity"};
    JitResult r = runWith(parseRenderingProg("double", inputs));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "NaN\nInfinity\n-Infinity\n");
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// §9.2 CONTROL: the exponent and the range paragraph.
// ===========================================================================

BEZEL_TEST(float_parse_signed_bare_point_7325,
           double_parse_raises_on_an_incomplete_exponent_spec_9_2) {
    std::vector<std::string> inputs = {"1e", "1e+"};
    JitResult r = runWith(parseOutcomeProg("double", inputs));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, wantOutcome(inputs, {"FMT", "FMT"}));
    BEZEL_EXPECT_EQ(r.exitCode, sumOf({kFormat, kFormat}));
}

BEZEL_TEST(float_parse_signed_bare_point_7325,
           double_parse_raises_overflow_on_a_finite_spelling_that_overflows_spec_9_2) {
    std::vector<std::string> inputs = {"1e999"};
    JitResult r = runWith(parseOutcomeProg("double", inputs));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, wantOutcome(inputs, {"OVF"}));
    BEZEL_EXPECT_EQ(r.exitCode, kOverflow);
}

// §9.2: "An input the grammar does not admit raises FormatException whatever
// number a part of it spells: "1_000e999" raises FormatException, not the
// OverflowException below."
BEZEL_TEST(float_parse_signed_bare_point_7325,
           double_parse_raises_format_not_overflow_on_a_rejected_spelling_that_overflows_spec_9_2) {
    std::vector<std::string> inputs = {"1_000e999"};
    JitResult r = runWith(parseOutcomeProg("double", inputs));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, wantOutcome(inputs, {"FMT"}));
    BEZEL_EXPECT_EQ(r.exitCode, kFormat);
}

// ===========================================================================
// §9.2 CONTROL: the `TryParse` form — false exactly where `Parse` raises,
// true (with the value) where it does not.
// ===========================================================================

BEZEL_TEST(float_parse_signed_bare_point_7325,
           double_try_parse_returns_false_for_the_spec_own_rejected_examples_spec_9_2) {
    std::vector<std::string> inputs = {".5", "5."};
    JitResult r = runWith(tryParseOutcomeProg("double", inputs));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    wantTried(inputs, {"False / 0", "False / 0"}));
    BEZEL_EXPECT_EQ(r.exitCode, sumOf({kFormat, kFormat}));
}

BEZEL_TEST(float_parse_signed_bare_point_7325,
           double_try_parse_admits_the_spec_own_numeric_examples_spec_9_2) {
    std::vector<std::string> inputs = {"5", "+5", "-0.25"};
    JitResult r = runWith(tryParseOutcomeProg("double", inputs));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    wantTried(inputs, {"True / 5", "True / 5", "True / -0.25"}));
    BEZEL_EXPECT_EQ(r.exitCode, kParsed);
}

// ===========================================================================
// §9.2 CONTROL: the SPAN form admits what the string form admits.
// ===========================================================================

BEZEL_TEST(float_parse_signed_bare_point_7325,
           double_parse_span_admits_a_signed_numeric_spelling_spec_9_2) {
    std::vector<std::string> inputs = {"+5"};
    JitResult r = runWith(parseOutcomeProg("double", inputs, true));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, wantOutcome(inputs, {"OK"}));
    BEZEL_EXPECT_EQ(r.exitCode, kParsed);
}
