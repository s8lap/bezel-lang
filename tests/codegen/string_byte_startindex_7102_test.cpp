// Regression tests for #7102 — `string.Substring` / `string.Remove` /
// `string.Insert` sign-extend an UNSIGNED `int` parameter.
//
// WHAT THE DOCUMENTS REQUIRE.
//
//   * spec §5.1 (v0.343): "Widening from unsigned to a strictly larger signed
//     type (e.g. `byte → short`, `ushort → int`) is permitted because no value
//     is lost." A `byte` 200 is therefore the `int` 200 — the widening is
//     LOSSLESS, so it must preserve the value.
//   * stdlib §9.6 (v0.343): `Substring(startIndex: int)`,
//     `Substring(startIndex: int, length: int)`, `Remove(startIndex: int)`,
//     `Remove(startIndex: int, length: int)` and `Insert(startIndex: int,
//     value: string)` all take an `int` index.
//
// THE DEFECT. Each of those `int` parameters was widened to i64 with a bare
// `CreateSExt`. A `byte` is UNSIGNED (§5.1), so its high bit is not a sign
// bit: `byte` 200 was sign-extended to -56 and `ushort` 40000 to -25536.
//
// MEASURED (pre-fix, at the staging tip 26f862bf), against the 252-code-point
// receiver `s` (see kLong252 below):
//
//   * `s.Substring(b)`            — the arm's own bounds check rejected the
//     sign-extended -56, so the program died with an uncaught
//     IndexOutOfRangeException (exit 70, no stdout).
//   * `s.Remove(b)`               — the sign-extended -56 reached `malloc` as
//     a NEGATIVE allocation size (ASan: "requested allocation size
//     0xffffffffffffffc9 (-55) exceeds maximum supported size"), and without a
//     sanitizer the null result was then written through — a memory-safety
//     defect, not merely a wrong answer.
//   * `s.Insert(b, "Z")`          — the arm clamps a negative index to 0, so
//     the insertion silently landed at the FRONT of the string (0 instead of
//     200).
//   * `s.Substring(0, b)` / `s.Remove(0, b)` — the `length` parameter of the
//     same two methods had the same widening: the length became negative, so
//     the first raised IndexOutOfRangeException and the second computed a
//     result LONGER than the receiver (380 instead of 124 for a length of
//     128).
//
// EXPECTED: the same value the literal index prints — the `byte` rows print
// what the literal control prints, and the `ushort` row prints 40000.
//
// ORACLE CHOICE. The defect is a value-bearing miscompile, so the rows that
// pin stdout are the oracle. `runAot` is used because the observable is the
// printed value, and the value is printed rather than returned so the row does
// not depend on the 8-bit exit-code channel.
//
// ORACLE DISCIPLINE. Every row asserts the EXACT printed text. A row that
// merely asserted "did not crash" would be vacuous: the pre-fix
// `Substring` symptom is an exception, the pre-fix `Insert` symptom is a wrong
// value, and the pre-fix `Remove` symptom is a crash.

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
// and index 251. A start of 200 therefore leaves 52 code points, a removal at
// 200 leaves 200, and an insertion at 200 lands at 200 — values that
// DISTINGUISH the correct index from the broken one (a start read as -56
// throws, is clamped to 0, or asks malloc for a negative size).
const char* kLong252 =
    "    var s: string := \"c\" + \"x\".Repeat(250) + \"c\";\n";

// The same wrapper, with a module-level `byte`-backed enum: an enum is
// represented by its underlying integer (§10.3), so `E.A` is an UNSIGNED 200.
std::string progWithByteEnum(const std::string& body) {
    return "module M;\n"
           "enum E: byte A := 200; end E;\n"
           "class C\n"
           "  public static method Main(): int\n" +
           body +
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

}  // namespace

// ===========================================================================
// The DEFECT — an UNSIGNED `startIndex` on Substring / Remove / Insert
// ===========================================================================

// §5.1: `byte` 200 widens to `int` 200 without loss. §9.6: `Substring`'s
// startIndex is an `int` code-point index.
//
// Pre-fix this raised an uncaught IndexOutOfRangeException (exit 70, no
// stdout): the sign-extended start was -56, which the arm's own bounds check
// rejected.
BEZEL_TEST(string_byte_startindex_7102,
           byte_startindex_substring_spec_5_1) {
    auto r = runAot(prog(
        std::string(kLong252) +
        "    var b: byte := 200;\n"
        "    Console.WriteLine(s.Substring(b).Length.ToString());\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "52\n");
}

// The same widening on `Remove`, where the sign-extended start is a
// MEMORY-SAFETY defect: it reaches `malloc` as a negative allocation size
// (ASan: "requested allocation size 0xffffffffffffffc9 (-55) exceeds maximum
// supported size") and, without a sanitizer, the null result is written
// through — the program dies on a signal rather than printing.
//
// Pre-fix: the program was terminated by a signal, so it printed nothing.
BEZEL_TEST(string_byte_startindex_7102,
           byte_startindex_remove_spec_5_1) {
    auto r = runAot(prog(
        std::string(kLong252) +
        "    var b: byte := 200;\n"
        "    Console.WriteLine(s.Remove(b).Length.ToString());\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "200\n");
}

// `Insert` clamps an out-of-range index instead of raising, so the pre-fix
// symptom is a silent WRONG ANSWER rather than a crash: the sign-extended -56
// was clamped to 0 and the insertion landed at the front of the string.
BEZEL_TEST(string_byte_startindex_7102,
           byte_startindex_insert_spec_5_1) {
    auto r = runAot(prog(
        std::string(kLong252) +
        "    var b: byte := 200;\n"
        "    Console.WriteLine(s.Insert(b, \"Z\").IndexOf(\"Z\").ToString());\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "200\n");
}

// The `length` parameter of the SAME two methods had the same widening, so
// the two-argument forms of Substring and Remove were broken too.
//
// Pre-fix this raised an uncaught IndexOutOfRangeException (exit 70): the
// length became -128, which the arm's `length >= 0` check rejected.
BEZEL_TEST(string_byte_startindex_7102,
           byte_length_substring_spec_5_1) {
    auto r = runAot(prog(
        std::string(kLong252) +
        "    var b: byte := 128;\n"
        "    Console.WriteLine(s.Substring(0, b).Length.ToString());\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "128\n");
}

// The same `length` parameter on `Remove`. Here the pre-fix symptom was a
// wrong LENGTH: the length became -128, so the removal computed a result
// LONGER than the receiver (380 instead of 124).
BEZEL_TEST(string_byte_startindex_7102,
           byte_length_remove_spec_5_1) {
    auto r = runAot(prog(
        std::string(kLong252) +
        "    var b: byte := 128;\n"
        "    Console.WriteLine(s.Remove(0, b).Length.ToString());\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "124\n");
}

// The at-Length boundary: `startIndex == Length` is the empty tail, which §9.6
// admits (the arm's own check accepts `start == len`).
//
// Pre-fix this raised an uncaught IndexOutOfRangeException (exit 70): 252
// was sign-extended to -4.
BEZEL_TEST(string_byte_startindex_7102,
           byte_startindex_at_length_spec_9_6) {
    auto r = runAot(prog(
        std::string(kLong252) +
        "    var b: byte := 252;\n"
        "    Console.WriteLine(s.Substring(b).Length.ToString());\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "0\n");
}

// `ushort` is the other unsigned source §5.1 admits at an `int` parameter
// (strictly narrower than `int`, so no value is lost). Its sign-extension is
// only observable ABOVE the signed 16-bit midpoint (32767); below it, a
// sign-extension is the identity. This row therefore uses a value above that
// midpoint, on a receiver long enough to make it a legal index.
//
// Pre-fix the sign-extended start was 40000 - 65536 = -25536, which reached
// `malloc` as a negative allocation size and killed the program on a signal.
BEZEL_TEST(string_byte_startindex_7102,
           ushort_startindex_remove_spec_5_1) {
    auto r = runAot(prog(
        "    var s: string := \"c\" + \"x\".Repeat(40000) + \"c\";\n"
        "    var u: ushort := 40000;\n"
        "    Console.WriteLine(s.Remove(u).Length.ToString());\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "40000\n");
}

// A byte-BACKED ENUM is the other unsigned source that reaches an `int`
// parameter: an enum is represented by its underlying integer (§10.3), so a
// `byte`-backed enum's 200 is an unsigned 200 and must widen to `int` 200.
//
// Pre-fix this raised an uncaught IndexOutOfRangeException (exit 70): the
// sign-extended start was -56.
BEZEL_TEST(string_byte_startindex_7102,
           byte_backed_enum_startindex_substring_spec_10_3) {
    auto r = runAot(progWithByteEnum(
        std::string(kLong252) +
        "    var e: E := E.A;\n"
        "    Console.WriteLine(s.Substring(e).Length.ToString());\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "52\n");
}

// ===========================================================================
// CONTROLS — green on both sides of the fix
// ===========================================================================

// The literal 200 was never broken (an `int` literal is signed, and a signed
// source still sign-extends). This pins the CONTROL arm of the issue's own
// measurement: the literal prints 52, 200 and 200, so a `byte`/200 must print
// the same three values.
BEZEL_TEST(string_byte_startindex_7102,
           literal_startindex_control) {
    auto r = runAot(prog(
        std::string(kLong252) +
        "    Console.WriteLine(s.Substring(200).Length.ToString());\n"
        "    Console.WriteLine(s.Remove(200).Length.ToString());\n"
        "    Console.WriteLine(s.Insert(200, \"Z\").IndexOf(\"Z\").ToString());\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "52\n200\n200\n");
}

// A SIGNED source must still sign-extend — the fix must not zero-extend
// everything. `sbyte` -1 is -1, so §9.6's `0 <= startIndex` bound rejects it
// and Substring raises IndexOutOfRangeException (caught here so the row can
// pin the exception TYPE, not merely a non-zero exit).
BEZEL_TEST(string_byte_startindex_7102,
           signed_sbyte_startindex_still_sign_extends) {
    auto r = runAot(prog(
        "    var b: sbyte := -1;\n"
        "    try\n"
        "      _ := \"abcabc\".Substring(b);\n"
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
BEZEL_TEST(string_byte_startindex_7102,
           signed_sbyte_startindex_uncaught_raises) {
    auto r = runAot(prog(
        "    var b: sbyte := -1;\n"
        "    _ := \"abcabc\".Substring(b);\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 70);
}
