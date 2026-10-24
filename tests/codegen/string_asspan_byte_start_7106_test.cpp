// Regression tests for #7106 — `string.AsSpan(start)` TRUNCATES a `byte`
// instead of widening it: the compiler crashes.
//
// WHAT THE DOCUMENTS REQUIRE.
//
//   * spec §5.1 (v0.343) *implicit numeric conversions*: "A value of one type
//     implicitly converts to another when no programmer-visible information is
//     lost", with the table row
//     `| byte | short, ushort, int, uint, long, ulong, float, double, decimal |`
//     and, in the never-implicit list, the counterpart that makes the widening
//     LOSSLESS: "Widening from unsigned to a strictly larger signed type
//     (e.g. `byte → short`, `ushort → int`) is permitted because no value is
//     lost."
//     ⇒ a `byte` 200 IS the `int` 200 — NOT -56 — and a `byte` 3 is the
//     `int` 3.
//   * stdlib §9.6 (v0.343): `AsSpan(start: int): ReadOnlyStringSpan` — "from
//     offset to end"; and `AsSpan(start: int, length: int):
//     ReadOnlyStringSpan`.
//
// THE DEFECT. `src/codegen/LlvmBackend_String.cpp` fit the `start` (and the
// `length`) into the span struct's i32 slots with a bare
// `CreateTrunc(start, i32())`. A `trunc` to a WIDER type is malformed LLVM IR,
// so a `byte` argument (i8) aborted the COMPILER:
//
//     DestTy too big for Trunc
//       %4 = trunc i8 %3 to i32
//     bezelc: codegen error: IR verification failed (malformed IR emitted)
//
// ⇒ an ICE on a program §5.1 admits — the compiler dies, so nothing about the
// program's runtime behaviour can be observed.
//
// ⚠ This is a TRUNC, not a sign-extension, so the `widenIntParam` helper
// (#6639/#6641/#7102) does NOT apply: it widens to i64, and this value must
// land in an i32 slot. The repair is a widen-to-i32 BY THE SOURCE'S
// SIGNEDNESS — an UNSIGNED source (`byte`, `ushort`, `uint`, `ulong`, `char`)
// ZERO-extends and a SIGNED one (`sbyte`, `short`, `int`) SIGN-extends.
//
// ORACLE CHOICE. The pre-fix symptom is a COMPILER CRASH, so the rows assert
// the program's OUTPUT — a row that merely asserted "did not crash" would
// also be satisfied by a program that printed the WRONG number. Every row
// that uses a `byte` start prints the LITERAL arm in the SAME program, so the
// equivalence claim is checked inside the test.
//
// ⚠ NOT A LEAK ORACLE. `runAot` does not check leaks by contract.

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
// #7106 — a `byte` start must reach AsSpan's i32 slot as its UNSIGNED value
// ===========================================================================

// §5.1: `byte` 3 is the `int` 3. The span must cover the last three code
// points of "abcdef".
//
// Pre-fix this did not run at all: the compiler aborted with "DestTy too big
// for Trunc" and the program was never produced.
BEZEL_TEST(string_asspan_byte_start_7106,
           byte_start_span_covers_the_rest_of_the_string) {
    auto r = runAot(prog(
        "    var b: byte := 3;\n"
        "    Console.WriteLine(\"abcdef\".AsSpan(b).Length);\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "3\n");
}

// The CONTROL arm, in the SAME program: the `byte` spelling and the literal
// spelling must agree. Pre-fix neither line ran (the compiler died).
BEZEL_TEST(string_asspan_byte_start_7106,
           byte_start_agrees_with_the_literal_start) {
    auto r = runAot(prog(
        "    var b: byte := 3;\n"
        "    Console.WriteLine(\"abcdef\".AsSpan(b).Length);\n"
        "    Console.WriteLine(\"abcdef\".AsSpan(3).Length);\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "3\n3\n");
}

// The two-argument form carries the SAME defect on BOTH parameters (the
// `start` and the `length`).
//
// Pre-fix this did not run at all: the compiler aborted with "DestTy too big
// for Trunc".
BEZEL_TEST(string_asspan_byte_start_7106,
           byte_start_and_length_span_covers_the_window) {
    auto r = runAot(prog(
        "    var b: byte := 3;\n"
        "    Console.WriteLine(\"abcdef\".AsSpan(b, 2).Length);\n"
        "    Console.WriteLine(\"abcdef\".AsSpan(3, 2).Length);\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "2\n2\n");
}

// ⚠ The `byte` ABOVE the signed midpoint — the case §5.1 is really about. A
// `byte` 200 must reach the i32 slot as 200, not -56.
//
// On a 252-code-point string a start of 200 is IN range, so the span covers
// the last 52 code points. Were the byte read as -56 the arm would raise
// IndexOutOfRangeException instead — so this row discriminates the signedness
// and not only the crash.
BEZEL_TEST(string_asspan_byte_start_7106,
           byte_start_above_the_signed_midpoint_is_in_range) {
    auto r = runAot(prog(
        "    var b: byte := 200;\n"
        "    var s: string := string.Repeat('x', 252);\n"
        "    Console.WriteLine(s.AsSpan(b).Length);\n"
        "    Console.WriteLine(s.AsSpan(200).Length);\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "52\n52\n");
}

// The SIGNED side of the same widening. §5.1's table row for `sbyte` is
// `| sbyte | short, int, long, float, double, decimal |` — a SIGNED
// conversion — so `sbyte` -1 must reach the i32 slot as -1 and the arm's
// bound check must REJECT it (stdlib §9.6 / §3.3: the span's start is a
// code-point offset, and a negative one is out of range).
//
// Pre-fix this did not run at all: the compiler aborted with "DestTy too big
// for Trunc". The row pins the SIGN-EXTENSION of a signed source, which is the
// mirror of the byte rows above.
BEZEL_TEST(string_asspan_byte_start_7106,
           sbyte_negative_start_raises_index_out_of_range) {
    auto r = runAot(prog(
        "    var b: sbyte := -1;\n"
        "    Console.WriteLine(\"abcdef\".AsSpan(b).Length);\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 70);
    BEZEL_EXPECT_EQ(r.stdoutText, "");
}
