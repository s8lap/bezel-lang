// Regression tests for #7104 — `string.Repeat(n)` sign-extends a `byte` count.
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
//   * stdlib §9.6 (v0.343): `String.Repeat(c: char, count: int): string` —
//     "count < 0 raises ArgumentOutOfRangeException; count = 0 is \"\"".
//
// THE DEFECT. `src/codegen/LlvmBackend_String.cpp` lowered the count of the
// instance `self.Repeat(n)` form with a bare `CreateSExt(nArg, i64(), "rep.n")`,
// so an UNSIGNED source's high bit was read as a sign bit: `byte` 200 arrived
// as -56. The sibling arms of the same family (`Substring` / `Remove` /
// `Insert`, #7102; `IndexOf`, #6639/#6641) already widen by the SOURCE's
// signedness through the `widenIntParam` helper; this arm did not.
//
// MEASURED (pre-fix, at the staging tip f7332508):
//
//     var b: byte := 200;
//     Console.WriteLine("x".Repeat(b).Length);
//
// ⇒ compiled CLEAN (rc=0) and at run time the process died with SIGSEGV
// (rc=139): the -56 count reached the `malloc` as a negative size, so the
// program crashed instead of repeating the string 200 times.
//
// The CONTROL arm — the same program with the literal `200` instead of `b` —
// compiled clean and printed `200`.
//
// §5.1 requires the two spellings to be EQUIVALENT. Here they were not: the
// literal worked and the `byte` spelling CRASHED the program.
//
// ORACLE CHOICE. The pre-fix symptom is a crash, so the rows assert the
// program's OUTPUT (the repeated length) — a row that merely asserted "did not
// crash" would also be satisfied by a program that printed the WRONG number,
// which is the other half of what §5.1 forbids. Every row that uses a `byte`
// count prints the LITERAL arm in the SAME program, so the equivalence claim
// is checked inside the test rather than asserted in prose.
//
// ⚠ NOT A LEAK ORACLE. `runAot` does not check leaks by contract, and the
// INSTANCE `self.Repeat(n)` form leaks its producer's reference: the #2199
// producer-recognizer in `src/codegen/LlvmBackend_Setup.cpp` recognizes the
// STATIC 2-argument form (`ma->member == "Repeat" && argc == 2`) and not the
// 1-argument instance form, so the consumer retains on top of the producer's
// +1. That is a PRE-EXISTING defect this fix neither causes nor claims to
// repair — it is MEASURED on the LITERAL control arm too, on the pre-fix
// compiler (233 bytes leaked in 2 allocations), so it is not an artefact of
// the `byte` spelling.

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
// #7104 — a `byte` count must reach Repeat as its UNSIGNED value
// ===========================================================================

// §5.1: `byte` 200 is the `int` 200. The instance Repeat must therefore
// produce a 200-code-point string.
//
// Pre-fix this program crashed (SIGSEGV, rc=139): the count was sign-extended
// to -56 and the negative size reached the `malloc`.
BEZEL_TEST(string_repeat_byte_count_7104,
           byte_count_repeats_the_string_that_many_times) {
    auto r = runAot(prog(
        "    var b: byte := 200;\n"
        "    Console.WriteLine(String.Repeat(\"x\", b).Length);\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "200\n");
}

// The CONTROL arm, in the SAME program: the `byte` spelling and the literal
// spelling must agree. Pre-fix the two lines printed nothing at all (the
// first line crashed the program).
BEZEL_TEST(string_repeat_byte_count_7104,
           byte_count_agrees_with_the_literal_count) {
    auto r = runAot(prog(
        "    var b: byte := 200;\n"
        "    Console.WriteLine(String.Repeat(\"x\", b).Length);\n"
        "    Console.WriteLine(String.Repeat(\"x\", 200).Length);\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "200\n200\n");
}

// The EQUIVALENCE claim is about the RESULT, not only about its length: the
// two spellings must produce the same string.
//
// The program returns 0 when the two strings are equal and 1 when they are
// not, so a wrong content is distinguishable from a wrong length.
BEZEL_TEST(string_repeat_byte_count_7104,
           byte_count_produces_the_same_content_as_the_literal_count) {
    auto r = runAot(prog(
        "    var b: byte := 200;\n"
        "    var a: string := String.Repeat(\"x\", b);\n"
        "    var c: string := String.Repeat(\"x\", 200);\n"
        "    return (if a = c then 0 else 1);\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// A `byte` BELOW the signed midpoint keeps its value too (the widening is
// lossless in both directions of the byte range): `byte` 5 must repeat the
// string five times, exactly as the literal 5 does.
//
// This row is GREEN on both sides of the fix — it is the guard that says the
// widening did not disturb the ordinary case.
BEZEL_TEST(string_repeat_byte_count_7104,
           byte_count_below_the_signed_midpoint_is_unchanged) {
    auto r = runAot(prog(
        "    var b: byte := 5;\n"
        "    Console.WriteLine(String.Repeat(\"x\", b).Length);\n"
        "    Console.WriteLine(String.Repeat(\"x\", 5).Length);\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "5\n5\n");
}
