// Regression tests for #7192 — `string.From(buffer, start, length)`
// SIGN-EXTENDS an unsigned `start` / `length`.
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
//     ⇒ a `byte` 200 IS the `int` 200 — NOT -56.
//   * stdlib §9.6 (v0.343): `string.From(buffer: Array<char>, start: int,
//     length: int): string` — the `(start, length)` window convention of §1
//     ("The convention names are `String.From(buffer, start, length)`").
//     `start` and `length` are `int` PARAMETERS, and §5.1 admits a `byte`
//     argument for an `int` parameter (lossless) — so the value the callee
//     must see is the `int` 200.
//
// THE DEFECT. `src/codegen/LlvmBackend_Calls2.cpp` (the `From(buffer, start,
// count)` arm of `lowerStaticCall`) widened the window with a bare
// `builder_.CreateSExt(start, i64(), "from.start")` /
// `CreateSExt(count, i64(), "from.count")`, so an UNSIGNED source's high bit
// was read as a sign bit: a `byte` 200 arrived as -56, the bounds guard
// `start < 0` fired, and the call raised `IndexOutOfRangeException` on a
// window that is entirely in range.
//
// MEASURED (pre-fix, on this lane's base commit 88247761 — the staging tip;
// the same on `origin/main`, so PRE-EXISTING), on a 300-element
// `Array<char>`:
//
//     var b: byte := 200;
//     var s: string := string.From(arr, b, 3);
//
// ⇒ compiled clean (rc=0) and at run time the process died with
// `Unhandled exception: System.IndexOutOfRangeException` (exit 70).
//
// The CONTROL arm — the same program with the literal `200` instead of `b` —
// compiled clean and printed `3`.
//
// §5.1 requires the two spellings to be EQUIVALENT. Here they were not: the
// literal worked and the `byte` spelling raised.
//
// The FIX routes the arm through the emitter-scope `widenIntParam` helper
// (`src/codegen/LlvmBackend_impl.hpp`) — the SAME rule the sibling string
// arms already use (#6639 `IndexOf`, #7104 `Repeat`, #7102 `Substring`,
// #7169's one-definition refactor). The widening is a PREREQUISITE of the
// bounds guard, not an extra: the guard must test the window's §5.1 value.
//
// ORACLE CHOICE. The rows assert the program's OUTPUT (the window's length /
// its content), not merely that it did not crash — a row that only checked
// "did not crash" would also be satisfied by a program that printed the WRONG
// number, which is the other half of what §5.1 forbids. The rows that use a
// `byte` window print the LITERAL spelling in the SAME program, so the
// equivalence claim is checked inside the test rather than asserted in prose.
//
// ⚠ NOT A LEAK ORACLE. `runAot` does not check leaks by contract. The static
// `string.From(buffer, …)` 3-arg form IS a recognized +1 producer
// (`isPlusOneStringProducer` in `src/codegen/LlvmBackend_Setup.cpp`), so the
// rows here are not leak rows; the leak property of this family is pinned by
// `stdlib_string_static_factories_leak_2228_test.cpp`.

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

// Wraps `body` in a runnable `Main` that returns 0 — for the body that
// supplies its OWN `return` (the content-equivalence row).
std::string progRaw(const std::string& body) {
    return "module M;\n"
           "class C\n"
           "  public static method Main(): int\n" +
           body +
           "  end Main;\n"
           "end C;\n";
}

// The 300-element `Array<char>` the window rows use: `byte` 200 as a start
// needs an array long enough for `200 + 3 <= Count`.
constexpr const char* kArr =
    "    using var arr := new Array<char>(300, 'x');\n";

}  // namespace

// ===========================================================================
// #7192 — a `byte` start / length must reach `string.From` as its UNSIGNED
//         value
// ===========================================================================

// §5.1: `byte` 200 is the `int` 200, and 200 + 3 <= 300 is in range — so the
// window must be BUILT, not rejected.
//
// Pre-fix this raised `IndexOutOfRangeException` (exit 70, no output): the
// -56 the sign-extension produced failed the guard's `start < 0` test.
BEZEL_TEST(string_from_byte_window_7192,
           byte_start_window_is_in_range) {
    auto r = runAot(prog(
        std::string(kArr) +
        "    var b: byte := 200;\n"
        "    var s: string := string.From(arr, b, 3);\n"
        "    Console.WriteLine(s.Length);\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "3\n");
}

// The CONTROL arm, in the SAME program: the `byte` spelling and the literal
// spelling must agree. Pre-fix the first line raised and the program printed
// nothing at all.
BEZEL_TEST(string_from_byte_window_7192,
           byte_start_agrees_with_the_literal_start) {
    auto r = runAot(prog(
        std::string(kArr) +
        "    var b: byte := 200;\n"
        "    var s1: string := string.From(arr, b, 3);\n"
        "    var s2: string := string.From(arr, 200, 3);\n"
        "    Console.WriteLine(s1.Length);\n"
        "    Console.WriteLine(s2.Length);\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "3\n3\n");
}

// The EQUIVALENCE claim is about the RESULT, not only about its length: the
// two spellings must produce the same string. The array carries a marker at
// slot 200 so a window that started at the WRONG slot would show it.
//
// The program returns 0 when the two strings are equal and 1 when they are
// not, so a wrong content is distinguishable from a wrong length.
BEZEL_TEST(string_from_byte_window_7192,
           byte_start_produces_the_same_content_as_the_literal_start) {
    auto r = runAot(progRaw(
        std::string(kArr) +
        "    arr[200] := 'b';\n"
        "    var b: byte := 200;\n"
        "    var s1: string := string.From(arr, b, 3);\n"
        "    var s2: string := string.From(arr, 200, 3);\n"
        "    Console.WriteLine(s1);\n"
        "    Console.WriteLine(s2);\n"
        "    return (if s1 = s2 then 0 else 1);\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "bxx\nbxx\n");
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// The LENGTH half of the window has the same defect: the count was sign-
// extended the same way, so a `byte` 200 length arrived as -56 and raised.
//
// 0 + 200 <= 300 is in range, so the window must be BUILT and must have 200
// code points.
BEZEL_TEST(string_from_byte_window_7192,
           byte_length_window_is_in_range) {
    auto r = runAot(prog(
        std::string(kArr) +
        "    var b: byte := 200;\n"
        "    var s: string := string.From(arr, 0, b);\n"
        "    Console.WriteLine(s.Length);\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "200\n");
}

// The CONTROL arm for the length: the `byte` spelling and the literal
// spelling must agree.
BEZEL_TEST(string_from_byte_window_7192,
           byte_length_agrees_with_the_literal_length) {
    auto r = runAot(prog(
        std::string(kArr) +
        "    var b: byte := 200;\n"
        "    var s1: string := string.From(arr, 0, b);\n"
        "    var s2: string := string.From(arr, 0, 200);\n"
        "    Console.WriteLine(s1.Length);\n"
        "    Console.WriteLine(s2.Length);\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "200\n200\n");
}

// ===========================================================================
// CONTROLS — these are GREEN on both sides of the fix; they are the guards
// that say the widening did not disturb the ordinary cases.
// ===========================================================================

// A `byte` BELOW the signed midpoint keeps its value (the widening is
// lossless in both directions of the byte range): `byte` 100 must select the
// same window as the literal 100.
BEZEL_TEST(string_from_byte_window_7192,
           byte_start_below_the_signed_midpoint_is_unchanged) {
    auto r = runAot(prog(
        std::string(kArr) +
        "    var b: byte := 100;\n"
        "    var s: string := string.From(arr, b, 3);\n"
        "    Console.WriteLine(s.Length);\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "3\n");
}

// A SIGNED negative start is out of range and MUST still raise
// `IndexOutOfRangeException` (§4.6 / stdlib §9.6's bounds convention). The
// widening is by the SOURCE's signedness, so `sbyte` -1 keeps its sign —
// this row is the guard against "fix" that zero-extends everything.
//
// Observed through the catch arm's stdout, not an exit code, so neither an
// abort nor a silent no-raise reads as green: the no-raise path prints its
// own distinct marker.
BEZEL_TEST(string_from_byte_window_7192,
           negative_signed_start_still_raises_index_out_of_range) {
    auto r = runAot(prog(
        std::string(kArr) +
        "    var b: sbyte := -1;\n"
        "    try\n"
        "      var s: string := string.From(arr, b, 3);\n"
        "      Console.WriteLine(\"no-raise\");\n"
        "    catch\n"
        "      IndexOutOfRangeException => Console.WriteLine(\"caught\")\n"
        "    end;\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "caught\n");
}

// The same guard on the LENGTH side.
BEZEL_TEST(string_from_byte_window_7192,
           negative_signed_length_still_raises_index_out_of_range) {
    auto r = runAot(prog(
        std::string(kArr) +
        "    var b: sbyte := -1;\n"
        "    try\n"
        "      var s: string := string.From(arr, 0, b);\n"
        "      Console.WriteLine(\"no-raise\");\n"
        "    catch\n"
        "      IndexOutOfRangeException => Console.WriteLine(\"caught\")\n"
        "    end;\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "caught\n");
}
