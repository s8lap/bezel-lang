// Regression tests for #6639 — an UNSIGNED `startIndex` is sign-extended by
// `string.IndexOf`'s two-argument arms.
//
// WHAT THE DOCUMENTS REQUIRE.
//
//   * spec §5.1 (v0.343): "a value converts implicitly … when no programmer-
//     visible information is lost", and "widening from unsigned to a strictly
//     larger signed type (e.g. `byte → short`, `ushort → int`) is permitted
//     because no value is lost." A `byte` 200 is therefore the `int` 200 —
//     the widening is LOSSLESS, so it must preserve the value.
//   * stdlib §9.6 (v0.343): `IndexOf(value: char, startIndex: int := 0): int`
//     and `IndexOf(value: string, startIndex: int := 0): int`. `startIndex` is
//     a code-point index; §9.6 requires `0 <= startIndex <= Length`.
//
// THE DEFECT. Both two-argument arms widened `startIndex` to i64 with a bare
// `CreateSExt`. A `byte` is UNSIGNED (§5.1), so its high bit is not a sign
// bit: `byte` 200 was sign-extended to -56.
//
// MEASURED (pre-fix, at the staging tip 6fa57f51):
//
//   * char needle  — the -56 start failed the arm's own bounds check, so the
//     program died with an uncaught IndexOutOfRangeException (exit 70).
//   * string needle — the runtime's `bz_str_indexof_from` CLAMPS a negative
//     start to 0 (`src/runtime/bezel_runtime_strings.cpp:200`), so the program
//     silently searched from the front and printed 0.
//   * control, the literal 200 in place of the `byte` — both forms printed 251.
//
// EXPECTED: the same value the literal start prints — 251 for the `byte` rows
// and 40001 for the `ushort` row.
//
// ORACLE CHOICE. The defect is a value-bearing miscompile, so the rows that
// pin stdout are the oracle. `runAotOut` is used because the observable is the
// printed value, and the value is printed rather than returned so the row does
// not depend on the 8-bit exit-code channel.
//
// ORACLE DISCIPLINE. Every row asserts the EXACT printed text. A row that
// merely asserted "did not crash" would be vacuous: the pre-fix char-needle
// symptom is a crash, and the pre-fix string-needle symptom is a wrong value.

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

// The receiver used by every value row: 252 code points, with 'c' at index 0
// and index 251. A start of 200 therefore finds the trailing 'c' at 251 — a
// value that DISTINGUISHES the correct start (200 → 251) from the broken one
// (a start clamped to 0 → 0, and a start read as -56 → an exception).
const char* kLong252 =
    "    var s: string := \"c\" + \"x\".Repeat(250) + \"c\";\n";

}  // namespace

// ===========================================================================
// The DEFECT — an unsigned `byte` startIndex
// ===========================================================================

// §5.1: `byte` 200 widens to `int` 200 without loss. §9.6: a start of 200 is
// in range on this 252-code-point string, and the search from 200 finds the
// trailing 'c' at 251.
//
// Pre-fix this raised an uncaught IndexOutOfRangeException (exit 70, no
// stdout): the sign-extended start was -56, which the char arm's own bounds
// check rejected.
BEZEL_TEST(string_indexof_byte_startindex_6639,
           byte_startindex_char_needle_spec_5_1) {
    auto r = runAot(prog(
        std::string(kLong252) +
        "    var b: byte := 200;\n"
        "    Console.WriteLine(s.IndexOf('c', b).ToString());\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "251\n");
}

// The `string` needle takes the other two-argument arm, which had the same
// widening.
//
// Pre-fix this printed 0: the sign-extended -56 reached the runtime, whose
// `bz_str_indexof_from` clamps a negative start to 0, so the search silently
// started at the front of the string.
BEZEL_TEST(string_indexof_byte_startindex_6639,
           byte_startindex_string_needle_spec_5_1) {
    auto r = runAot(prog(
        std::string(kLong252) +
        "    var b: byte := 200;\n"
        "    Console.WriteLine(s.IndexOf(\"c\", b).ToString());\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "251\n");
}

// `ushort` is the other unsigned source §5.1 admits at an `int` parameter (it
// is strictly narrower than `int`, so no value is lost). Its sign-extension is
// only observable ABOVE the signed 16-bit midpoint (32767): below it, a
// sign-extension is the identity. This row therefore uses a value above that
// midpoint and a receiver long enough to make it a legal start.
//
// Pre-fix the char needle raised an uncaught IndexOutOfRangeException (exit
// 70): the sign-extended start was 35000 - 65536 = -30536, which the arm's
// own bounds check rejected. The string needle clamped the same -30536 to 0
// and printed 0.
BEZEL_TEST(string_indexof_byte_startindex_6639,
           ushort_startindex_above_sign_bit_spec_5_1) {
    auto r = runAot(prog(
        "    var s: string := \"c\" + \"x\".Repeat(40000) + \"c\";\n"
        "    var u: ushort := 35000;\n"
        "    Console.WriteLine(s.IndexOf('c', u).ToString());\n"
        "    Console.WriteLine(s.IndexOf(\"c\", u).ToString());\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "40001\n40001\n");
}

// A `ushort` BELOW the signed midpoint is unaffected by the widening (a
// sign-extension of it is the identity), so this row is green on both sides of
// the fix. It is the control arm of the `ushort` case.
BEZEL_TEST(string_indexof_byte_startindex_6639,
           ushort_startindex_control) {
    auto r = runAot(prog(
        std::string(kLong252) +
        "    var u: ushort := 200;\n"
        "    Console.WriteLine(s.IndexOf('c', u).ToString());\n"
        "    Console.WriteLine(s.IndexOf(\"c\", u).ToString());\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "251\n251\n");
}

// ===========================================================================
// CONTROLS — green on both sides of the fix
// ===========================================================================

// The literal 200 was never broken (an `int` literal is signed, and a signed
// source still sign-extends). This pins the CONTROL arm of the issue's own
// measurement: the literal prints 251, so a `byte`/200 must print 251 too.
BEZEL_TEST(string_indexof_byte_startindex_6639,
           literal_200_startindex_control) {
    auto r = runAot(prog(
        std::string(kLong252) +
        "    Console.WriteLine(s.IndexOf('c', 200).ToString());\n"
        "    Console.WriteLine(s.IndexOf(\"c\", 200).ToString());\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "251\n251\n");
}

// A SIGNED source must still sign-extend — the fix must not zero-extend
// everything. `sbyte` -1 is -1, so §9.6's `0 <= startIndex` bound rejects it
// and the char arm raises IndexOutOfRangeException (caught here so the row can
// pin the exception TYPE, not merely a non-zero exit).
BEZEL_TEST(string_indexof_byte_startindex_6639,
           signed_sbyte_startindex_still_sign_extends) {
    auto r = runAot(prog(
        "    var b: sbyte := -1;\n"
        "    try\n"
        "      _ := \"abcabc\".IndexOf('c', b);\n"
        "      return 0;\n"
        "    catch IndexOutOfRangeException => return 7;\n"
        "    end;\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// The UNCAUGHT form of the same signed case: an uncaught exception leaves
// Main through the runtime's uncaught-exception boundary (exit 70). This pins
// the exception the fix must NOT change for a signed source.
BEZEL_TEST(string_indexof_byte_startindex_6639,
           signed_sbyte_startindex_uncaught_raises) {
    auto r = runAot(prog(
        "    var b: sbyte := -1;\n"
        "    _ := \"abcabc\".IndexOf('c', b);\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 70);
}
