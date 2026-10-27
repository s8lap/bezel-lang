// Regression tests for #7105 — `string.PadLeft` / `string.PadRight` sign-extend
// a `byte` totalWidth.
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
//   * stdlib §9.6 (v0.343): `PadLeft(totalWidth: int, paddingChar: char := ' ')
//     : string` / `PadRight(totalWidth: int, paddingChar: char := ' '): string`
//     — "Pad contract (all three Pad members): totalWidth <= Length returns the
//     original string unchanged; totalWidth < 0 raises
//     ArgumentOutOfRangeException."
//
// THE DEFECT. `src/codegen/LlvmBackend_String.cpp` lowered the width with a
// bare `CreateSExt(widthArg, i64(), "pad.width")`, so an UNSIGNED source's
// high bit was read as a sign bit: `byte` 200 arrived as -56. The runtime
// helper `bz_str_pad` answers `totalWidth <= Length` with "the original
// string unchanged", so the -56 produced the ORIGINAL 3-code-point string —
// a SILENT WRONG ANSWER (exit 0, no diagnostic).
//
// MEASURED (pre-fix, at the staging tip f7332508):
//
//     var b: byte := 200;
//     Console.WriteLine("abc".PadLeft(b).Length);   ⇒ printed 3
//     Console.WriteLine("abc".PadLeft(200).Length); ⇒ printed 200
//
// ⚠ `PadRight` was NOT separately measured by the coordinator — it is the
// sibling spelling of the SAME arm (the `leftAlign` argument is the only
// difference), so it was measured HERE, on the pre-fix compiler:
//
//     var b: byte := 200;
//     Console.WriteLine("abc".PadRight(b).Length);  ⇒ printed 3   (WRONG)
//     Console.WriteLine("abc".PadRight(200).Length); ⇒ printed 200
//
// ⚠ `PadCenter` is NOT a sibling of this defect — it has NO codegen arm at
// all, so it never reached this lowering. Measured on the pre-fix compiler:
//
//     var b: byte := 200;
//     Console.WriteLine("abc".PadCenter(b).Length);
//
// ⇒ the COMPILER REJECTS the program ("string member `.PadCenter` is not yet
// supported in codegen"), the same way it rejects the LITERAL spelling. The
// §9.6 `PadCenter` contract is therefore not reachable at all — a whole-
// member gap, tracked by the conformance corpus (case `l9_6_0004`, a
// recorded divergence), and NOT a byte-widening defect. There is no site in
// this file for it to fix.
//
// ⚠ THAT GAP IS NOW CLOSED (#7133, §9.6, v0.343). `PadCenter` has its own
// codegen arm and its own lowering, and it reaches the SAME `bz_str_pad`
// helper — so the `byte` width reaches it through the SAME `widenIntParam`
// path this file fixes for `PadLeft`/`PadRight`. The `PadCenter` contract is
// asserted in `tests/codegen/string_pad_center_7133_test.cpp`, not here.
// This paragraph records the PRE-#7133 measurement and is kept as history.
//
// ORACLE CHOICE. The pre-fix symptom is a SILENT WRONG VALUE (exit 0), so the
// rows assert the OUTPUT — a row that merely asserted "did not crash" would
// be vacuous here. Every row that uses a `byte` width prints the LITERAL arm
// in the SAME program, so the equivalence claim is checked inside the test.
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
// #7105 — a `byte` totalWidth must reach the Pad* family as its UNSIGNED value
// ===========================================================================

// §5.1: `byte` 200 is the `int` 200. PadLeft must therefore pad to 200
// code points.
//
// Pre-fix this printed 3 (the original string, unchanged) and exited 0.
BEZEL_TEST(string_pad_byte_width_7105,
           byte_totalwidth_pads_left_to_that_width) {
    auto r = runAot(prog(
        "    var b: byte := 200;\n"
        "    Console.WriteLine(\"abc\".PadLeft(b).Length);\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "200\n");
}

// The CONTROL arm, in the SAME program: the `byte` spelling and the literal
// spelling must agree. Pre-fix the two lines printed "3" and "200".
BEZEL_TEST(string_pad_byte_width_7105,
           byte_totalwidth_agrees_with_the_literal_width) {
    auto r = runAot(prog(
        "    var b: byte := 200;\n"
        "    Console.WriteLine(\"abc\".PadLeft(b).Length);\n"
        "    Console.WriteLine(\"abc\".PadLeft(200).Length);\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "200\n200\n");
}

// The EQUIVALENCE claim is about the RESULT, not only about its length: the
// two spellings must produce the same string (same code points, same order).
//
// The program returns 0 when the two strings are equal and 1 when they are
// not, so a wrong content is distinguishable from a wrong length.
BEZEL_TEST(string_pad_byte_width_7105,
           byte_totalwidth_produces_the_same_content_as_the_literal_width) {
    auto r = runAot(prog(
        "    var b: byte := 200;\n"
        "    var a: string := \"abc\".PadLeft(b);\n"
        "    var c: string := \"abc\".PadLeft(200);\n"
        "    return (if a = c then 0 else 1);\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ⚠ `PadRight` was measured HERE, not by the coordinator: it is the sibling
// spelling of the SAME arm, and an unmeasured sibling spelling is a guess.
//
// Pre-fix this printed 3 (the original string, unchanged) and exited 0.
BEZEL_TEST(string_pad_byte_width_7105,
           byte_totalwidth_pads_right_to_that_width) {
    auto r = runAot(prog(
        "    var b: byte := 200;\n"
        "    Console.WriteLine(\"abc\".PadRight(b).Length);\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "200\n");
}

// The CONTROL arm for PadRight, in the SAME program.
BEZEL_TEST(string_pad_byte_width_7105,
           byte_totalwidth_agrees_with_the_literal_width_right) {
    auto r = runAot(prog(
        "    var b: byte := 200;\n"
        "    Console.WriteLine(\"abc\".PadRight(b).Length);\n"
        "    Console.WriteLine(\"abc\".PadRight(200).Length);\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "200\n200\n");
}

// A `byte` BELOW the signed midpoint keeps its value too (the widening is
// lossless in both directions of the byte range): `byte` 5 must pad to 5
// code points, exactly as the literal 5 does.
//
// This row is GREEN on both sides of the fix — it is the guard that says the
// widening did not disturb the ordinary case.
BEZEL_TEST(string_pad_byte_width_7105,
           byte_totalwidth_below_the_signed_midpoint_is_unchanged) {
    auto r = runAot(prog(
        "    var b: byte := 5;\n"
        "    Console.WriteLine(\"abc\".PadLeft(b).Length);\n"
        "    Console.WriteLine(\"abc\".PadLeft(5).Length);\n"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "5\n5\n");
}
