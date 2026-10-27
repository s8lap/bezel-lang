// Regression tests for #7341 (stdlib §9.2, §2.3, §2.15, v0.344): a point
// with NO DIGITS AFTER IT, followed by an exponent, was admitted everywhere.
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
// AND the neighbouring sentences (line 5265):
//
//   "`TryParse` returns `false` in every case where `Parse` would raise, and
//   the span forms carry the string forms' contract."
//
// AND stdlib §2.3 (line 219) — the mirror the Convert family owes the Parse
// family:
//
//   "The `To*` methods raise `FormatException` when the string is not a
//   well-formed representation of the target type and `OverflowException` when
//   the value does not fit — the same contract as the §9 per-type `Parse`
//   family these conversions mirror; ... `Try` forms return `false` in exactly
//   the cases the throwing forms would raise; the `out` parameter receives the
//   default value."
//
// THE DEFECT.  Every one of these arms decided the point with the SAME test:
// "the LAST character strtod consumed must not be '.', 'e', 'E', '+' or '-'".
// That test looks only at the FINAL character of the input, so a point with
// no digit after it was rejected only when the point ENDED the input ("5.",
// "5.e") — and NOT when an exponent followed it, because then the last
// character is a digit ("5.e3" was read as 5000).  The same class was in
// THREE families:
//
//   * `Convert.ToDouble` / `Convert.TryToDouble` (stdlib §2.3),
//   * `double.Parse` / `double.TryParse` and the span forms (stdlib §9.2),
//   * the JSON deserializer's `ReadFloat` (stdlib §2.15).
//
// THE ORACLE.  Every row below asserts THREE things: that the program RAN
// (`runAot` reports `ran == false` when the COMPILER rejected the program,
// which is a different outcome from "the program ran and raised"), the
// program's EXIT STATUS, and the program's EXACT stdout.  The exit status is
// the SUM of the per-input outcome codes (0 = the parse succeeded, 1 =
// `FormatException`, 2 = `OverflowException`), so the status says WHICH path
// ran; the TEXT carries the value and the exception kind, because a process
// status is masked to 0..255 and cannot carry them.
//
// The rows are on `runAot` (NOT `runAotChecked`), deliberately: a
// FormatException / SerializationException is a RUNTIME outcome, observable
// from the exit status and the stdout, and needs no sanitizer — so this file
// adds no `runAotChecked` row and does not move `check_toolchain_skip_surface`'s
// `checked_calls` ratchet.
//
// THE CONTROL ROWS are §9.2's own admitted examples, §9.2's own rejected
// examples, the non-finite family and the three exact non-finite spellings,
// the range paragraph, and the signed / unsigned bare point.  They must be
// byte-identical on the pre-fix and the post-fix arm.

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

// One measured input and the outcome the spec requires of it.  `outcome` is
// the text the outcome program prints after the "-> ": "OK <value>" for a
// successful parse, "FMT" for a FormatException, "OVF" for an
// OverflowException.
struct Case {
    std::string input;
    std::string outcome;
};

// The DEFECT set: a point with NO digit after it, whatever exponent follows
// it.  Every one of them is a grammar violation of §9.2 and must raise
// FormatException (and §2.3's mirror says the same of `Convert`).
const std::vector<Case> kDefect = {
    {"5.e3", "FMT"},    {"1.e+3", "FMT"},  {"5.e+3", "FMT"},  {"5.E3", "FMT"},
    {"5.e-3", "FMT"},   {"+5.e3", "FMT"},  {"-5.e3", "FMT"},  {"0.e0", "FMT"},
    {"5.e0", "FMT"},    {"3.e3", "FMT"},   {"9.e9", "FMT"},   {"12.e34", "FMT"},
    {"1.e999", "FMT"},
};

// The CONTROL set: §3's whole list.  §9.2's own admitted examples, §9.2's
// own rejected examples, the non-finite family, the three exact non-finite
// spellings, the signed / unsigned bare point, and the range paragraph.
const std::vector<Case> kControl = {
    // §9.2's own ADMITTED examples.
    {"5", "OK 5"},
    {"+5", "OK 5"},
    {"-0.25", "OK -0.25"},
    {"007.5", "OK 7.5"},
    {"6.02E+23", "OK 6.02e+23"},
    // The three exact non-finite spellings (matched exactly).
    {"NaN", "OK NaN"},
    {"Infinity", "OK Infinity"},
    {"-Infinity", "OK -Infinity"},
    // §9.2's own REJECTED examples.
    {"5.", "FMT"},
    {"5.e", "FMT"},
    {"1.2.3", "FMT"},
    {".e3", "FMT"},
    {"1.2e", "FMT"},
    {"1e3.", "FMT"},
    {" 5", "FMT"},
    {"5 ", "FMT"},
    {"1_000", "FMT"},
    {"0x1p3", "FMT"},
    {"1e", "FMT"},
    // The non-finite family — matched exactly, so every other spelling raises.
    {"nan", "FMT"},
    {"inf", "FMT"},
    {"infinity", "FMT"},
    {"INFINITY", "FMT"},
    {"+Infinity", "FMT"},
    {"-NaN", "FMT"},
    {"+inf", "FMT"},
    {"-inf", "FMT"},
    {"nan(0)", "FMT"},
    // The bare point, signed and unsigned.
    {"+.5", "FMT"},
    {"-.5", "FMT"},
    {".5", "FMT"},
    // The range paragraph: an input the grammar does not admit raises
    // FormatException whatever number a part of it spells ("1_000e999"),
    // while a finite spelling that does not fit raises OverflowException.
    {"1_000e999", "FMT"},
    {"1e999", "OVF"},
};

// The parse arms' operand: §9.2 declares both the `string` form and the
// `ReadOnlyStringSpan` form, and the span form carries the string form's
// contract ("the span forms carry the string forms' contract", §9.2:5265).
std::string operand(bool span) { return span ? "s.AsSpan()" : "s"; }

// The initial value of the `out` slot of a Try form.  It is NOT the default,
// so a row that sees "0" after a failure also proves the `out` parameter
// received the default value (§2.3).
std::string initial(const std::string& type) {
    return type == "float" ? "7.0f" : "7.0";
}

// A program that parses each input in turn and prints "<input> -> OK <value>"
// (the parse succeeded), "<input> -> FMT" (a `FormatException` was caught) or
// "<input> -> OVF" (an `OverflowException` was caught), and whose exit status
// is the SUM of the per-input outcome codes.
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
           "      Console.WriteLine(s + \" -> OK \" + v.ToString());\n"
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

// The same program for the `Convert` family (stdlib §2.3).
std::string convertOutcomeProg(const std::vector<std::string>& inputs) {
    std::string body;
    for (const std::string& in : inputs) {
        body += "    total := total + Conv1(\"" + in + "\");\n";
    }
    return "module Conf;\n"
           "public class Program\n"
           "  public static method Main(): int\n"
           "    var total: int := 0;\n" + body +
           "    return total;\n"
           "  end Main;\n"
           "  private static method Conv1(s: string): int\n"
           "    try\n"
           "      var v := Convert.ToDouble(s);\n"
           "      Console.WriteLine(s + \" -> OK \" + v.ToString());\n"
           "      return " + std::to_string(kParsed) + ";\n"
           "    catch\n"
           "      var e: FormatException => do Console.WriteLine(s + \" -> FMT\"); "
           "return " + std::to_string(kFormat) + "; end,\n"
           "      var e: OverflowException => do Console.WriteLine(s + \" -> OVF\"); "
           "return " + std::to_string(kOverflow) + "; end\n"
           "    end;\n"
           "  end Conv1;\n"
           "end Program;\n";
}

// A program that parses each input with the `TryParse` form and prints
// "<input> -> True / <value>" or "<input> -> False / <value>" — the second
// half of the line is the `out` slot, which must hold the DEFAULT value when
// the call returned false (§2.3).
std::string tryParseOutcomeProg(const std::string& type,
                                 const std::vector<std::string>& inputs,
                                 bool span = false) {
    std::string body;
    for (const std::string& in : inputs) {
        body += "    total := total + Try1(\"" + in + "\");\n";
    }
    return "module Conf;\n"
           "public class Program\n"
           "  public static method Main(): int\n"
           "    var total: int := 0;\n" + body +
           "    return total;\n"
           "  end Main;\n"
           "  private static method Try1(s: string): int\n"
           "    var v := " + initial(type) + ";\n"
           "    var ok := " + type + ".TryParse(" + operand(span) + ", out v);\n"
           "    Console.WriteLine(s + \" -> \" + ok.ToString() + \" / \" + "
           "v.ToString());\n"
           "    return (if ok then " + std::to_string(kParsed) + " else " +
           std::to_string(kFormat) + ");\n"
           "  end Try1;\n"
           "end Program;\n";
}

// The same program for `Convert.TryToDouble` (stdlib §2.3).
std::string convertTryOutcomeProg(const std::vector<std::string>& inputs) {
    std::string body;
    for (const std::string& in : inputs) {
        body += "    total := total + ConvTry1(\"" + in + "\");\n";
    }
    return "module Conf;\n"
           "public class Program\n"
           "  public static method Main(): int\n"
           "    var total: int := 0;\n" + body +
           "    return total;\n"
           "  end Main;\n"
           "  private static method ConvTry1(s: string): int\n"
           "    var v := 7.0;\n"
           "    var ok := Convert.TryToDouble(s, out v);\n"
           "    Console.WriteLine(s + \" -> \" + ok.ToString() + \" / \" + "
           "v.ToString());\n"
           "    return (if ok then " + std::to_string(kParsed) + " else " +
           std::to_string(kFormat) + ");\n"
           "  end ConvTry1;\n"
           "end Program;\n";
}

// A program that runs BOTH families on each input and prints
// "<input> | Convert=<outcome> | Parse=<outcome>" — so §2.3's mirror is
// checked STRUCTURALLY, on every measured input, and not by comparing two
// separate rows.
std::string mirrorProg(const std::vector<std::string>& inputs) {
    std::string body;
    for (const std::string& in : inputs) {
        body += "    total := total + Both1(\"" + in + "\");\n";
    }
    return "module Conf;\n"
           "public class Program\n"
           "  public static method Main(): int\n"
           "    var total: int := 0;\n" + body +
           "    return total;\n"
           "  end Main;\n"
           "  private static method Both1(s: string): int\n"
           "    var c := \"?\";\n"
           "    var p := \"?\";\n"
           "    var code := 0;\n"
           "    try\n"
           "      var cv := Convert.ToDouble(s);\n"
           "      c := \"OK \" + cv.ToString();\n"
           "    catch\n"
           "      var e1: FormatException => do c := \"FMT\"; code := code + 1; end,\n"
           "      var e1: OverflowException => do c := \"OVF\"; code := code + 2; end\n"
           "    end;\n"
           "    try\n"
           "      var pv := double.Parse(s);\n"
           "      p := \"OK \" + pv.ToString();\n"
           "    catch\n"
           "      var e2: FormatException => do p := \"FMT\"; code := code + 1; end,\n"
           "      var e2: OverflowException => do p := \"OVF\"; code := code + 2; end\n"
           "    end;\n"
           "    Console.WriteLine(s + \" | Convert=\" + c + \" | Parse=\" + p);\n"
           "    return code;\n"
           "  end Both1;\n"
           "end Program;\n";
}

// A program that reads ONE JSON number text through the JSON deserializer's
// `ReadFloat` (stdlib §2.15) and prints "OK <value>" or "SER" (a
// SerializationException was caught).  The JSON text is a LITERAL, so the
// program allocates no runtime-built string (a JSON document built at runtime
// and handed to a `JsonDeserializer` leaks that string when the deserializer
// is disposed - a pre-existing defect, not this lane's; see the hand-back).
std::string jsonOutcomeProg(const std::string& number) {
    return "module Conf;\n"
           "import System.Serialization;\n"
           "public record RF(V: float): ISerializable, IDeserializable;\n"
           "public class Program\n"
           "  public static method Main(): int\n"
           "    var json: string := \"{\\\"V\\\": " + number + "}\";\n"
           "    using var d := new JsonDeserializer(json);\n"
           "    try\n"
           "      var v := RF.Deserialize(d);\n"
           "      Console.WriteLine(\"OK \" + v.V.ToString());\n"
           "      return 0;\n"
           "    catch\n"
           "      var e: SerializationException => do Console.WriteLine(\"SER\"); "
           "return 1; end\n"
           "    end;\n"
           "  end Main;\n"
           "end Program;\n";
}

// The inputs of a table, in order.
std::vector<std::string> inputsOf(const std::vector<Case>& cases) {
    std::vector<std::string> out;
    for (const Case& c : cases) out.push_back(c.input);
    return out;
}

// The expected stdout of a parse outcome program over a table.
std::string wantOutcome(const std::vector<Case>& cases) {
    std::string want;
    for (const Case& c : cases) {
        want += c.input + " -> " + c.outcome + "\n";
    }
    return want;
}

// The expected stdout of a Try outcome program over a table: "True / <value>"
// for a successful parse and "False / 0" for a rejected one (the `out` slot
// holds the default).
std::string wantTried(const std::vector<Case>& cases) {
    std::string want;
    for (const Case& c : cases) {
        if (c.outcome == "FMT" || c.outcome == "OVF") {
            want += c.input + " -> False / 0\n";
        } else {
            want += c.input + " -> True / " + c.outcome.substr(3) + "\n";
        }
    }
    return want;
}

// The expected stdout of the mirror program over a table.
std::string wantMirror(const std::vector<Case>& cases) {
    std::string want;
    for (const Case& c : cases) {
        want += c.input + " | Convert=" + c.outcome + " | Parse=" + c.outcome +
                "\n";
    }
    return want;
}

// The exit status a program over a table returns: the SUM of the per-input
// outcome codes.
int sumOf(const std::vector<Case>& cases) {
    int total = 0;
    for (const Case& c : cases) {
        if (c.outcome == "FMT") total += kFormat;
        else if (c.outcome == "OVF") total += kOverflow;
    }
    return total;
}

// The exit status a TRY program over a table returns: every input that did
// not parse contributes 1 (the call returned false), whatever the reason.
int sumOfTry(const std::vector<Case>& cases) {
    int total = 0;
    for (const Case& c : cases) {
        if (c.outcome.rfind("OK ", 0) != 0) total += 1;
    }
    return total;
}

// The exit status the MIRROR program over a table returns: every input
// contributes to BOTH families, so each outcome code is counted twice.
int sumOfMirror(const std::vector<Case>& cases) { return 2 * sumOf(cases); }

}  // namespace

// ===========================================================================
// THE DEFECT — a point with NO digits after it, followed by an exponent.
// Every family must reject it with a FormatException.
// ===========================================================================

// stdlib §2.3 (line 219): `Convert.ToDouble` mirrors §9.2's `Parse`.
BEZEL_TEST(float_point_no_digits_7341,
           convert_to_double_rejects_a_point_with_no_digits_spec_2_3) {
    JitResult r = runAot(convertOutcomeProg(inputsOf(kDefect)));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, wantOutcome(kDefect));
    BEZEL_EXPECT_EQ(r.exitCode, sumOf(kDefect));
}

// stdlib §2.3 (line 219): "`Try` forms return `false` in exactly the cases
// the throwing forms would raise; the `out` parameter receives the default
// value."
BEZEL_TEST(float_point_no_digits_7341,
           convert_try_to_double_returns_false_for_a_point_with_no_digits_spec_2_3) {
    JitResult r = runAot(convertTryOutcomeProg(inputsOf(kDefect)));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, wantTried(kDefect));
    BEZEL_EXPECT_EQ(r.exitCode, sumOfTry(kDefect));
}

// stdlib §9.2 (line 5263): "optionally a `.` followed by one or more digits".
BEZEL_TEST(float_point_no_digits_7341,
           double_parse_rejects_a_point_with_no_digits_spec_9_2) {
    JitResult r = runAot(parseOutcomeProg("double", inputsOf(kDefect)));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, wantOutcome(kDefect));
    BEZEL_EXPECT_EQ(r.exitCode, sumOf(kDefect));
}

// The same grammar for `float` (stdlib §9.2 governs both).
BEZEL_TEST(float_point_no_digits_7341,
           float_parse_rejects_a_point_with_no_digits_spec_9_2) {
    JitResult r = runAot(parseOutcomeProg("float", inputsOf(kDefect)));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, wantOutcome(kDefect));
    BEZEL_EXPECT_EQ(r.exitCode, sumOf(kDefect));
}

// stdlib §9.2 (line 5265): "`TryParse` returns `false` in every case where
// `Parse` would raise".
BEZEL_TEST(float_point_no_digits_7341,
           double_try_parse_returns_false_for_a_point_with_no_digits_spec_9_2) {
    JitResult r = runAot(tryParseOutcomeProg("double", inputsOf(kDefect)));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, wantTried(kDefect));
    BEZEL_EXPECT_EQ(r.exitCode, sumOfTry(kDefect));
}

// stdlib §9.2 (line 5265): "the span forms carry the string forms'
// contract" — the span form must reject exactly what the string form
// rejects.
BEZEL_TEST(float_point_no_digits_7341,
           double_parse_span_rejects_a_point_with_no_digits_spec_9_2) {
    JitResult r = runAot(parseOutcomeProg("double", inputsOf(kDefect), true));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, wantOutcome(kDefect));
    BEZEL_EXPECT_EQ(r.exitCode, sumOf(kDefect));
}

BEZEL_TEST(float_point_no_digits_7341,
           double_try_parse_span_returns_false_for_a_point_with_no_digits_spec_9_2) {
    JitResult r = runAot(tryParseOutcomeProg("double", inputsOf(kDefect), true));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, wantTried(kDefect));
    BEZEL_EXPECT_EQ(r.exitCode, sumOfTry(kDefect));
}

// stdlib §2.15 (line 945): "`ReadFloat` reads a JSON number of any length".
// JSON's number grammar requires at least one digit after the '.', so "1.e3"
// is not a JSON number.
BEZEL_TEST(float_point_no_digits_7341,
           json_read_float_rejects_a_point_with_no_digits_spec_2_15) {
    JitResult r = runAot(jsonOutcomeProg("1.e3"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "SER\n");
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// ===========================================================================
// §2.3's MIRROR — `Convert.ToDouble` and `double.Parse` must AGREE on every
// measured input, the rejected ones included.
// ===========================================================================
BEZEL_TEST(float_point_no_digits_7341,
           convert_and_parse_agree_on_every_measured_input_spec_2_3) {
    std::vector<Case> all = kDefect;
    all.insert(all.end(), kControl.begin(), kControl.end());
    JitResult r = runAot(mirrorProg(inputsOf(all)));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, wantMirror(all));
    BEZEL_EXPECT_EQ(r.exitCode, sumOfMirror(all));
}

// ===========================================================================
// CONTROL — §9.2's own admitted examples, checked BY VALUE.
// ===========================================================================
BEZEL_TEST(float_point_no_digits_7341,
           double_parse_admits_the_spec_own_numeric_examples_spec_9_2) {
    JitResult r = runAot(parseOutcomeProg("double", inputsOf(kControl)));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, wantOutcome(kControl));
    BEZEL_EXPECT_EQ(r.exitCode, sumOf(kControl));
}

// ===========================================================================
// CONTROL — §9.2's own rejected examples, the non-finite family, the bare
// point and the range paragraph: every one of them still raises.
// ===========================================================================
BEZEL_TEST(float_point_no_digits_7341,
           float_parse_raises_on_the_spec_own_rejected_examples_spec_9_2) {
    JitResult r = runAot(parseOutcomeProg("float", inputsOf(kControl)));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, wantOutcome(kControl));
    BEZEL_EXPECT_EQ(r.exitCode, sumOf(kControl));
}

// The SPAN form agrees with the string form on every control input.
BEZEL_TEST(float_point_no_digits_7341,
           double_parse_span_agrees_with_the_string_form_spec_9_2) {
    JitResult r = runAot(parseOutcomeProg("double", inputsOf(kControl), true));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, wantOutcome(kControl));
    BEZEL_EXPECT_EQ(r.exitCode, sumOf(kControl));
}

// The `TryParse` form: false exactly where `Parse` raises, and the `out`
// parameter holds the DEFAULT value (it was initialised to 7.0, so "0"
// proves the store happened).
BEZEL_TEST(float_point_no_digits_7341,
           double_try_parse_returns_false_and_defaults_on_every_rejected_input_spec_9_2) {
    JitResult r = runAot(tryParseOutcomeProg("double", inputsOf(kControl)));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, wantTried(kControl));
    BEZEL_EXPECT_EQ(r.exitCode, sumOfTry(kControl));
}

// The same for `Convert.TryToDouble` (§2.3).
BEZEL_TEST(float_point_no_digits_7341,
           convert_try_to_double_returns_false_and_defaults_on_every_rejected_input_spec_2_3) {
    JitResult r = runAot(convertTryOutcomeProg(inputsOf(kControl)));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, wantTried(kControl));
    BEZEL_EXPECT_EQ(r.exitCode, sumOfTry(kControl));
}

// ===========================================================================
// CONTROL — the JSON reader: the valid JSON numbers still read, and the
// invalid ones still raise SerializationException.
// ===========================================================================
BEZEL_TEST(float_point_no_digits_7341,
           json_read_float_admits_a_valid_json_number_spec_2_15) {
    JitResult r = runAot(jsonOutcomeProg("1.5e3"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "OK 1500\n");
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

BEZEL_TEST(float_point_no_digits_7341,
           json_read_float_admits_a_valid_json_number_with_a_point_spec_2_15) {
    JitResult r = runAot(jsonOutcomeProg("1.5"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "OK 1.5\n");
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

BEZEL_TEST(float_point_no_digits_7341,
           json_read_float_rejects_a_json_number_ending_in_a_point_spec_2_15) {
    JitResult r = runAot(jsonOutcomeProg("1."));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "SER\n");
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

BEZEL_TEST(float_point_no_digits_7341,
           json_read_float_rejects_a_json_number_with_an_empty_exponent_spec_2_15) {
    JitResult r = runAot(jsonOutcomeProg("1.e"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "SER\n");
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}
