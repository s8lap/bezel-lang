// Regression test for issue #7287 -- the LITERAL format overload of
// `ToString(format: string)` ABORTED THE COMPILER for every receiver whose
// width is NOT 32 bits.
//
// THE DEFECT (already fixed on this tree; the issue stayed OPEN).
//
//   The literal format path converted the receiver with an FP-to-SI
//   conversion (`CreateFPToSI(recv, i32())`), which is malformed IR for an
//   integer payload of any width but 32 -- so the compiler aborted with
//   "FPToSI source must be FP or FP vector" / "IR verification failed" on a
//   `long` (i64), `ulong` (i64), `byte` (i8), `sbyte`, `short` or `ushort`
//   receiver, on both the `D` (decimal) and the `X` (hex) spelling. `int` and
//   `uint` are both i32 and both compiled.
//
//   stdlib §2.18 (v0.344) gives the integer row -- the row that names `long`
//   and `byte` explicitly -- the vocabulary `D`/`Dn` decimal and `X`/`x` hex;
//   spec §18.5 (v0.344) puts the same specifiers on the one-argument
//   `ToString` member.
//
// WHAT FIXED IT.
//
//   commit 275fab52 -- "fix #7329: the integer row's D and X specifiers work at
//   every width". The receiver is now widened the way the interpolation
//   spelling widens it (to i64, sign-extended for a signed receiver and
//   zero-extended for an unsigned one), so no `fptosi` is emitted on an
//   integer value.
//
//   That commit closed #7329 (the specifier vocabulary) and did not name
//   #7287 (the compiler ABORT), so the issue stayed OPEN until this one.
//
// THE ORACLE.
//
//   Every row asserts the EXACT stdout of the AOT program -- the rendered
//   decimal / hex text -- and that the program ran to completion. On the
//   pre-fix tree the compiler aborts, so the program does not run and the
//   stdout is empty: every non-32-bit row is RED there and GREEN here.
//
//   The CONTROL rows are the 32-bit receivers, which compiled before the fix
//   and must keep compiling with the same text -- they pass on BOTH arms.
//
// Cite: stdlib §2.18 (v0.344); spec §18.5 (v0.344); spec §2.3 (v0.344).

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotOut;

namespace {

std::string prog(const std::string& body) {
    return "module M; import System; "
           "public class C "
           "  public static method Main(): int " +
           body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

}  // namespace

// ---------------------------------------------------------------------------
// THE DEFECT ROWS -- a receiver whose width is NOT 32 bits.
// ---------------------------------------------------------------------------
BEZEL_TEST(literal_format_width_7287,
           long_hex_renders_without_an_abort_2_18) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var l: long := 255; "
        "    Console.WriteLine(l.ToString(\"X\")); ")),
        std::string("FF"));
}

BEZEL_TEST(literal_format_width_7287,
           long_decimal_renders_without_an_abort_2_18) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var l: long := 255; "
        "    Console.WriteLine(l.ToString(\"D\")); ")),
        std::string("255"));
}

BEZEL_TEST(literal_format_width_7287,
           ulong_hex_renders_without_an_abort_2_18) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var u: ulong := 300; "
        "    Console.WriteLine(u.ToString(\"X\")); ")),
        std::string("12C"));
}

BEZEL_TEST(literal_format_width_7287,
           byte_decimal_renders_without_an_abort_2_18) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var b: byte := 200; "
        "    Console.WriteLine(b.ToString(\"D\")); ")),
        std::string("200"));
}

BEZEL_TEST(literal_format_width_7287,
           short_hex_renders_without_an_abort_2_18) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var s: short := 300; "
        "    Console.WriteLine(s.ToString(\"X\")); ")),
        std::string("12C"));
}

BEZEL_TEST(literal_format_width_7287,
           literal_long_receiver_renders_without_an_abort_2_18) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    Console.WriteLine((300L).ToString(\"X\")); ")),
        std::string("12C"));
}

// ---------------------------------------------------------------------------
// THE CONTROL ROWS -- the 32-bit receivers, which compiled before the fix
// and must keep compiling with the same text.  They pass on BOTH arms.
// ---------------------------------------------------------------------------
BEZEL_TEST(literal_format_width_7287,
           control_int_hex_is_unchanged_2_18) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var i: int := 255; "
        "    Console.WriteLine(i.ToString(\"X\")); ")),
        std::string("FF"));
}

BEZEL_TEST(literal_format_width_7287,
           control_int_decimal_is_unchanged_2_18) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var i: int := 255; "
        "    Console.WriteLine(i.ToString(\"D\")); ")),
        std::string("255"));
}

BEZEL_TEST(literal_format_width_7287,
           control_double_format_is_unchanged_2_18) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    Console.WriteLine((3.5).ToString(\"F1\")); ")),
        std::string("3.5"));
}
