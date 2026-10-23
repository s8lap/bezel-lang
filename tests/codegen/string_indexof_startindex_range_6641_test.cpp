// Regression tests for #6641 — `string.IndexOf`'s STRING-NEEDLE arm has no
// range check on `startIndex`.
//
// WHAT THE DOCUMENTS REQUIRE.
//
//   * stdlib §9.6 (v0.343): `IndexOf(value: string, startIndex: int := 0): int`
//     — "at or after startIndex, or -1; 0 <= startIndex <= Length
//     (IndexOutOfRangeException otherwise)". The same bound is declared for
//     the `char` needle form, `IndexOf(value: char, startIndex: int := 0)`.
//   * spec §5.1 (v0.343): a `char` widens implicitly to `int`.
//
// THE DEFECT. The `char`-needle arm checked `0 <= startIndex <= Length` and
// raised IndexOutOfRangeException otherwise; the `string`-needle arm had NO
// check at all. An out-of-range start therefore reached the runtime, where
// `bz_str_indexof_from` (`src/runtime/bezel_runtime_strings.cpp:195`) clamps a
// NEGATIVE start to 0 and returns -1 for a start past the end.
//
// MEASURED (pre-fix, at the staging tip 6fa57f51):
//
//   * "abcabc".IndexOf("c", -1)  printed 2  (the negative start was clamped
//     to 0, so it searched from the front and found the 'c' at index 2);
//   * "abcabc".IndexOf("c", 99)  printed -1 (past the end);
//   * both exited 0, with NO exception;
//   * the `char`-needle forms of the same two calls each raised
//     IndexOutOfRangeException (exit 70).
//
// EXPECTED: the string-needle form behaves like the char-needle form — it
// raises IndexOutOfRangeException.
//
// THE INTERACTION WITH #6639 (the same lane, an earlier commit). #6639 stops
// the arms from sign-extending an UNSIGNED `startIndex`: a `byte` 200 is the
// `int` 200, not -56. The range check must therefore run against the
// NORMALISED start — the value the search actually starts from. Were the check
// applied to a pre-widening value, `byte` 200 would still be rejected and the
// two fixes would contradict each other. The row
// `byte_startindex_on_long_string_is_in_range` pins that: a `byte` 200 is a
// LEGAL start on a 252-code-point string, and both needle forms must answer
// 251.
//
// ORACLE CHOICE. The defect is a MISSING raise, so the rows that pin it
// assert the exception TYPE (an in-source `catch IndexOutOfRangeException`
// that returns a distinctive code — a different exception type would not be
// caught and the program would exit 70 instead), plus the issue's literal
// repro asserted UNCAUGHT (exit 70). A row that merely asserted "did not
// crash" would be vacuous: the pre-fix symptom is a silent wrong value.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::runAot;

namespace {

// Wraps `body` in a runnable `Main` that returns 0.
std::string prog(const std::string& body) {
    return "module M;\n"
           "class C\n"
           "  public static method Main(): int\n" +
           body +
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

}  // namespace

// ===========================================================================
// The DEFECT — the string-needle arm has no startIndex range check
// ===========================================================================

// The issue's LITERAL repro, uncaught: §9.6 requires the raise, so the
// program must leave Main through the runtime's uncaught-exception boundary
// (exit 70) and print nothing.
//
// Pre-fix this printed "2\n-1\n" and exited 0 — the negative start was
// clamped to 0 by the runtime and the start past the end returned -1.
BEZEL_TEST(string_indexof_startindex_range_6641,
           issue_repro_uncaught_raises) {
    auto r = runAot(prog(
        "    Console.WriteLine(\"abcabc\".IndexOf(\"c\", -1).ToString());\n"
        "    Console.WriteLine(\"abcabc\".IndexOf(\"c\", 99).ToString());\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 70);
    BEZEL_EXPECT_EQ(r.stdoutText, "");
}

// §9.6: startIndex < 0 raises IndexOutOfRangeException. The catch clause pins
// the exception TYPE: a different fault would escape it and exit 70 rather
// than 7.
BEZEL_TEST(string_indexof_startindex_range_6641,
           string_needle_negative_start_raises) {
    auto r = runAot(prog(
        "    try\n"
        "      _ := \"abcabc\".IndexOf(\"c\", -1);\n"
        "      return 0;\n"
        "    catch IndexOutOfRangeException => return 7;\n"
        "    end;\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// §9.6: startIndex > Length raises IndexOutOfRangeException ("abc" has
// Length 3, so 4 is past the end).
//
// Pre-fix the runtime returned -1 for this start (it is past the end of the
// haystack), so the program answered -1 instead of raising.
BEZEL_TEST(string_indexof_startindex_range_6641,
           string_needle_start_past_length_raises) {
    auto r = runAot(prog(
        "    try\n"
        "      _ := \"abc\".IndexOf(\"c\", 4);\n"
        "      return 0;\n"
        "    catch IndexOutOfRangeException => return 9;\n"
        "    end;\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 9);
}

// §7.4 + §5.1: a flow-narrowed `char?` is admissible at the `int` startIndex
// position (a `char` widens to `int`), and its VALUE is the payload — 'c',
// which is 99. 99 is past the Length (6) of "abcabc", so §9.6 requires the
// raise.
//
// Pre-fix this printed -1 (the runtime's past-the-end answer) and exited 0.
BEZEL_TEST(string_indexof_startindex_range_6641,
           narrowed_char_q_start_index_raises) {
    auto r = runAot(prog(
        "    var n: char? := 'c';\n"
        "    if n is null then return 1; end;\n"
        "    try\n"
        "      _ := \"abcabc\".IndexOf(\"c\", n);\n"
        "      return 0;\n"
        "    catch IndexOutOfRangeException => return 11;\n"
        "    end;\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 11);
}

// ===========================================================================
// CONTROLS — green on both sides of the fix
// ===========================================================================

// The `char`-needle arm already raised for an out-of-range startIndex. It is
// the control arm of the issue's own measurement, and it pins that the fix
// leaves the char arm alone.
BEZEL_TEST(string_indexof_startindex_range_6641,
           char_needle_out_of_range_start_raises_control) {
    auto r = runAot(prog(
        "    try\n"
        "      _ := \"abcabc\".IndexOf('c', -1);\n"
        "      return 0;\n"
        "    catch IndexOutOfRangeException => return 5;\n"
        "    end;\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// §9.6: startIndex == Length is IN range ("0 <= startIndex <= Length") — the
// search starts past the last code point, finds nothing, and answers -1.
// This is the boundary the new check must NOT reject.
BEZEL_TEST(string_indexof_startindex_range_6641,
           string_needle_start_equal_length_returns_minus_one) {
    auto r = runAot(prog(
        "    Console.WriteLine(\"abcabc\".IndexOf(\"c\", 6).ToString());\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "-1\n");
}

// §9.6: startIndex 0 is in range and finds the first occurrence ('c' is at
// index 2 of "abcabc").
BEZEL_TEST(string_indexof_startindex_range_6641,
           string_needle_start_zero_control) {
    auto r = runAot(prog(
        "    Console.WriteLine(\"abcabc\".IndexOf(\"c\", 0).ToString());\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "2\n");
}

// THE INTERACTION with #6639. §5.1: a `byte` 200 is the `int` 200 (the
// widening is lossless), so it is a LEGAL start on a 252-code-point string,
// and §9.6 puts its bound at Length — 252 — which 200 is inside. Both needle
// forms must therefore answer 251 and neither may raise.
//
// This row is GREEN on both sides of this fix: it is the guard that says the
// new range check sees the NORMALISED start. A check applied to the
// pre-widening value would reject the `byte` 200 (as -56) and this row would
// go RED.
BEZEL_TEST(string_indexof_startindex_range_6641,
           byte_startindex_on_long_string_is_in_range) {
    auto r = runAot(prog(
        "    var s: string := \"c\" + \"x\".Repeat(250) + \"c\";\n"
        "    var b: byte := 200;\n"
        "    Console.WriteLine(s.IndexOf('c', b).ToString());\n"
        "    Console.WriteLine(s.IndexOf(\"c\", b).ToString());\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "251\n251\n");
}
