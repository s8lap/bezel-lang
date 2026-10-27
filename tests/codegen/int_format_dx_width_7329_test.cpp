// Regression tests for #7329 (stdlib §2.18, v0.344; spec §18.5, v0.344): the
// integer row's `D`/`X` format specifiers on a receiver that is NOT 32 bits
// wide.
//
// THE GOVERNING TEXT. stdlib §2.18's specifier table (docs/
// Bezel_Standard_Library.md:1149, v0.344) — the Integers row:
//
//   | Integers (`int`, `long`, `byte`, …) | `D`/`Dn` decimal (n = min digits,
//     zero-padded) · `X`/`x` hex (upper/lower, `Xn` pads) · `B` binary | no
//     grouping separators (no culture) |
//
// The row NAMES `long` and `byte` explicitly, so `D` and `X` are in the
// vocabulary of EVERY integer width — and §2.18's general rule (the same
// section) makes the primitive types "intrinsically formattable", so a format
// spec on an integer of any width is not a compile error either.
//
// THE DEFECT. The one-argument `ToString` member (the "literal format
// spelling") converted its receiver with `builder_.CreateFPToSI(recv, i32())` —
// an FP→SI conversion applied to an INTEGER payload. That is malformed IR for
// every width but 32, so the compiler ABORTED:
//
//   FPToSI source must be FP or FP vector: %3 = fptosi i8 %2 to i32
//
// on a `byte`/`sbyte`/`short`/`ushort`/`long`/`ulong` receiver, on BOTH the
// `D` and the `X` spelling. A compiler that aborts is contrary to the table
// above.
//
// THE CONTRACT, PER WIDTH. The expected texts below are derived from the spec
// table and from the 32-bit `int` receiver — the shape that already worked
// (the CONTROL rows, which pass with and without the fix):
//
//   * `D`/`Dn` renders the DECIMAL of the value the caller holds, zero-padded
//     to n digits ("n = min digits, zero-padded");
//   * `X`/`Xn` renders the receiver's natural-width BIT PATTERN, zero-padded
//     to n digits ("`X`/`x` hex (upper/lower, `Xn` pads)");
//   * the SIGNEDNESS is the receiver's own TYPE's, never the bit width's:
//     `sbyte`/`short`/`int`/`long` are signed, `byte`/`ushort`/`uint`/`ulong`
//     are not — so an unsigned receiver's `D` is its unsigned decimal (a
//     `byte` 200 is "200", not "-56") and its `X` is the same bits an
//     unsigned value has.
//
// The expected texts are cross-checked against the INTERPOLATION spelling of
// the same specifier (`$"{value:D5}"`), which is the house's other
// implementation of §2.18's table and already renders every width.
//
// THE ORACLE. `runAot` — the real AOT path (fork-isolated
// `Driver::compileFile` + clang link + native exec). NOT `runAotChecked`: the
// ASan oracle moves `check_toolchain_skip_surface`'s `checked_calls`
// denominator, which is a coordinator-owned ratchet.
//
// Every row asserts the program's EXACT stdout AND its exit status — the
// point of this bug is that the compiler ABORTED, so a row that only checked
// "did not crash" would be vacuous.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

namespace {

using bezel::test::JitResult;
using bezel::test::runAot;

// A program whose `Main` declares `decl` (when it is not empty) and prints
// `call`.
std::string prog(const std::string& decl, const std::string& call) {
    std::string src = "module M;\n"
                      "public class Program\n"
                      "  public static method Main(): int\n";
    if (!decl.empty()) src += "    " + decl + "\n";
    src += "    Console.WriteLine(" + call + ");\n"
           "    return 0;\n"
           "  end Main;\n"
           "end Program;\n";
    return src;
}

}  // namespace

// ---------------------------------------------------------------------------
// `byte` — an UNSIGNED, 8-bit receiver. stdlib §2.18's integer row names
// `byte` explicitly, so `D`/`X` are in its vocabulary.
// ---------------------------------------------------------------------------

BEZEL_TEST(int_format_dx_width_7329, stdlib_2_18_byte_d_decimal) {
    JitResult r = runAot(prog("var b: byte := 200;", "b.ToString(\"D\")"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("200\n"));
}

BEZEL_TEST(int_format_dx_width_7329, stdlib_2_18_byte_d5_zero_padded) {
    JitResult r = runAot(prog("var b: byte := 200;", "b.ToString(\"D5\")"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("00200\n"));
}

BEZEL_TEST(int_format_dx_width_7329, stdlib_2_18_byte_x_hex) {
    JitResult r = runAot(prog("var b: byte := 200;", "b.ToString(\"X\")"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("C8\n"));
}

BEZEL_TEST(int_format_dx_width_7329, stdlib_2_18_byte_x4_hex_padded) {
    JitResult r = runAot(prog("var b: byte := 200;", "b.ToString(\"X4\")"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("00C8\n"));
}

// The lower-case spelling of the hex specifier ("`X`/`x` hex"). The MEMBER
// spelling case-folds the specifier letter, so it renders the same text as
// the upper-case one here — a divergence of its own (the interpolation
// spelling renders it lower-case), reported by this lane and not changed here.
BEZEL_TEST(int_format_dx_width_7329, stdlib_2_18_byte_x_lower_case_spelling) {
    JitResult r = runAot(prog("var b: byte := 200;", "b.ToString(\"x\")"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("C8\n"));
}

// ---------------------------------------------------------------------------
// `sbyte` — a SIGNED, 8-bit receiver. The hex spelling renders the
// natural-width bit pattern (the two's-complement bits of the byte), so -5 is
// "FB" — not the 64-bit pattern.
// ---------------------------------------------------------------------------

BEZEL_TEST(int_format_dx_width_7329, stdlib_2_18_sbyte_d_decimal) {
    JitResult r = runAot(prog("var s: sbyte := -5;", "s.ToString(\"D\")"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("-5\n"));
}

BEZEL_TEST(int_format_dx_width_7329, stdlib_2_18_sbyte_d3_zero_padded) {
    JitResult r = runAot(prog("var s: sbyte := -5;", "s.ToString(\"D3\")"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("-05\n"));
}

BEZEL_TEST(int_format_dx_width_7329, stdlib_2_18_sbyte_x_hex) {
    JitResult r = runAot(prog("var s: sbyte := -5;", "s.ToString(\"X\")"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("FB\n"));
}

BEZEL_TEST(int_format_dx_width_7329, stdlib_2_18_sbyte_x4_hex_padded) {
    JitResult r = runAot(prog("var s: sbyte := -5;", "s.ToString(\"X4\")"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("00FB\n"));
}

BEZEL_TEST(int_format_dx_width_7329, stdlib_2_18_sbyte_x_lower_case_spelling) {
    JitResult r = runAot(prog("var s: sbyte := -5;", "s.ToString(\"x\")"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("FB\n"));
}

// ---------------------------------------------------------------------------
// `short` — a SIGNED, 16-bit receiver.
// ---------------------------------------------------------------------------

BEZEL_TEST(int_format_dx_width_7329, stdlib_2_18_short_d_decimal) {
    JitResult r = runAot(prog("var s: short := -300;", "s.ToString(\"D\")"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("-300\n"));
}

BEZEL_TEST(int_format_dx_width_7329, stdlib_2_18_short_d5_zero_padded) {
    JitResult r = runAot(prog("var s: short := -300;", "s.ToString(\"D5\")"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("-0300\n"));
}

BEZEL_TEST(int_format_dx_width_7329, stdlib_2_18_short_x_hex) {
    JitResult r = runAot(prog("var s: short := -300;", "s.ToString(\"X\")"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("FED4\n"));
}

BEZEL_TEST(int_format_dx_width_7329, stdlib_2_18_short_x4_hex_padded) {
    JitResult r = runAot(prog("var s: short := -300;", "s.ToString(\"X4\")"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("FED4\n"));
}

BEZEL_TEST(int_format_dx_width_7329, stdlib_2_18_short_x_lower_case_spelling) {
    JitResult r = runAot(prog("var s: short := -300;", "s.ToString(\"x\")"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("FED4\n"));
}

// ---------------------------------------------------------------------------
// `ushort` — an UNSIGNED, 16-bit receiver.
// ---------------------------------------------------------------------------

BEZEL_TEST(int_format_dx_width_7329, stdlib_2_18_ushort_d_decimal) {
    JitResult r = runAot(prog("var u: ushort := 65000;", "u.ToString(\"D\")"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("65000\n"));
}

BEZEL_TEST(int_format_dx_width_7329, stdlib_2_18_ushort_d5_zero_padded) {
    JitResult r = runAot(prog("var u: ushort := 65000;", "u.ToString(\"D5\")"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("65000\n"));
}

BEZEL_TEST(int_format_dx_width_7329, stdlib_2_18_ushort_x_hex) {
    JitResult r = runAot(prog("var u: ushort := 65000;", "u.ToString(\"X\")"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("FDE8\n"));
}

BEZEL_TEST(int_format_dx_width_7329, stdlib_2_18_ushort_x4_hex_padded) {
    JitResult r = runAot(prog("var u: ushort := 65000;", "u.ToString(\"X4\")"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("FDE8\n"));
}

BEZEL_TEST(int_format_dx_width_7329, stdlib_2_18_ushort_x_lower_case_spelling) {
    JitResult r = runAot(prog("var u: ushort := 65000;", "u.ToString(\"x\")"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("FDE8\n"));
}

// ---------------------------------------------------------------------------
// `int` — the 32-bit CONTROL. These rows pass with and without the fix: they
// are the shape that already worked, and the other widths must agree with
// them.
// ---------------------------------------------------------------------------

BEZEL_TEST(int_format_dx_width_7329, stdlib_2_18_int_d_decimal_control) {
    JitResult r = runAot(prog("var i: int := 5;", "i.ToString(\"D\")"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("5\n"));
}

BEZEL_TEST(int_format_dx_width_7329, stdlib_2_18_int_d5_zero_padded_control) {
    JitResult r = runAot(prog("var i: int := 5;", "i.ToString(\"D5\")"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("00005\n"));
}

BEZEL_TEST(int_format_dx_width_7329, stdlib_2_18_int_x_hex_control) {
    JitResult r = runAot(prog("var i: int := 5;", "i.ToString(\"X\")"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("5\n"));
}

BEZEL_TEST(int_format_dx_width_7329, stdlib_2_18_int_x4_hex_padded_control) {
    JitResult r = runAot(prog("var i: int := 5;", "i.ToString(\"X4\")"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("0005\n"));
}

BEZEL_TEST(int_format_dx_width_7329, stdlib_2_18_int_x_lower_case_spelling_control) {
    JitResult r = runAot(prog("var i: int := 5;", "i.ToString(\"x\")"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("5\n"));
}

// The NEGATIVE 32-bit value — the control that says what the signed decimal
// and the hex bit pattern are at 32 bits: "-05" for `D3` (the sign counts
// toward the width) and "FFFFFFFB" for `X` (the two's-complement bits of the
// 32-bit value).
BEZEL_TEST(int_format_dx_width_7329, stdlib_2_18_int_negative_d3_control) {
    JitResult r = runAot(prog("var i: int := -5;", "i.ToString(\"D3\")"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("-05\n"));
}

BEZEL_TEST(int_format_dx_width_7329, stdlib_2_18_int_negative_x_control) {
    JitResult r = runAot(prog("var i: int := -5;", "i.ToString(\"X\")"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("FFFFFFFB\n"));
}

// ---------------------------------------------------------------------------
// `uint` — an UNSIGNED, 32-bit receiver. The `D` spelling is the decimal of
// the value the caller holds, so 4000000000 is "4000000000" (not the signed
// rendering of the same 32 bits).
// ---------------------------------------------------------------------------

BEZEL_TEST(int_format_dx_width_7329, stdlib_2_18_uint_d_decimal) {
    JitResult r = runAot(prog("var u: uint := 4000000000;", "u.ToString(\"D\")"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("4000000000\n"));
}

BEZEL_TEST(int_format_dx_width_7329, stdlib_2_18_uint_d5_zero_padded) {
    JitResult r = runAot(prog("var u: uint := 4000000000;", "u.ToString(\"D5\")"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("4000000000\n"));
}

BEZEL_TEST(int_format_dx_width_7329, stdlib_2_18_uint_x_hex) {
    JitResult r = runAot(prog("var u: uint := 4000000000;", "u.ToString(\"X\")"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("EE6B2800\n"));
}

BEZEL_TEST(int_format_dx_width_7329, stdlib_2_18_uint_x4_hex_padded) {
    JitResult r = runAot(prog("var u: uint := 4000000000;", "u.ToString(\"X4\")"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("EE6B2800\n"));
}

BEZEL_TEST(int_format_dx_width_7329, stdlib_2_18_uint_x_lower_case_spelling) {
    JitResult r = runAot(prog("var u: uint := 4000000000;", "u.ToString(\"x\")"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("EE6B2800\n"));
}

// ---------------------------------------------------------------------------
// `long` — a SIGNED, 64-bit receiver.
// ---------------------------------------------------------------------------

BEZEL_TEST(int_format_dx_width_7329, stdlib_2_18_long_d_decimal) {
    JitResult r = runAot(prog("var l: long := 5000000000L;", "l.ToString(\"D\")"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("5000000000\n"));
}

BEZEL_TEST(int_format_dx_width_7329, stdlib_2_18_long_d5_zero_padded) {
    JitResult r = runAot(prog("var l: long := 5000000000L;", "l.ToString(\"D5\")"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("5000000000\n"));
}

BEZEL_TEST(int_format_dx_width_7329, stdlib_2_18_long_x_hex) {
    JitResult r = runAot(prog("var l: long := 5000000000L;", "l.ToString(\"X\")"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("12A05F200\n"));
}

BEZEL_TEST(int_format_dx_width_7329, stdlib_2_18_long_x4_hex_padded) {
    JitResult r = runAot(prog("var l: long := 5000000000L;", "l.ToString(\"X4\")"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("12A05F200\n"));
}

BEZEL_TEST(int_format_dx_width_7329, stdlib_2_18_long_x_lower_case_spelling) {
    JitResult r = runAot(prog("var l: long := 5000000000L;", "l.ToString(\"x\")"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("12A05F200\n"));
}

// The NEGATIVE 64-bit value: the signed decimal is "-5" and the hex spelling
// is the 64-bit two's-complement pattern.
BEZEL_TEST(int_format_dx_width_7329, stdlib_2_18_long_negative_d) {
    JitResult r = runAot(prog("var l: long := -5L;", "l.ToString(\"D\")"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("-5\n"));
}

BEZEL_TEST(int_format_dx_width_7329, stdlib_2_18_long_negative_x) {
    JitResult r = runAot(prog("var l: long := -5L;", "l.ToString(\"X\")"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("FFFFFFFFFFFFFFFB\n"));
}

// ---------------------------------------------------------------------------
// `ulong` — an UNSIGNED, 64-bit receiver.
// ---------------------------------------------------------------------------

BEZEL_TEST(int_format_dx_width_7329, stdlib_2_18_ulong_d_decimal) {
    JitResult r = runAot(prog("var u: ulong := 18000000000000000000;",
                               "u.ToString(\"D\")"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("18000000000000000000\n"));
}

BEZEL_TEST(int_format_dx_width_7329, stdlib_2_18_ulong_d5_zero_padded) {
    JitResult r = runAot(prog("var u: ulong := 18000000000000000000;",
                               "u.ToString(\"D5\")"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("18000000000000000000\n"));
}

BEZEL_TEST(int_format_dx_width_7329, stdlib_2_18_ulong_x_hex) {
    JitResult r = runAot(prog("var u: ulong := 18000000000000000000;",
                               "u.ToString(\"X\")"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("F9CCD8A1C5080000\n"));
}

BEZEL_TEST(int_format_dx_width_7329, stdlib_2_18_ulong_x4_hex_padded) {
    JitResult r = runAot(prog("var u: ulong := 18000000000000000000;",
                               "u.ToString(\"X4\")"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("F9CCD8A1C5080000\n"));
}

BEZEL_TEST(int_format_dx_width_7329, stdlib_2_18_ulong_x_lower_case_spelling) {
    JitResult r = runAot(prog("var u: ulong := 18000000000000000000;",
                               "u.ToString(\"x\")"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("F9CCD8A1C5080000\n"));
}

// ---------------------------------------------------------------------------
// THE NEIGHBOURING ROWS OF THE TABLE — the controls that say this fix did
// not reach past the integer row's own `D`/`X` vocabulary.
// ---------------------------------------------------------------------------

// A FLOATING receiver keeps its rendering: the `D` spelling truncates the
// value to an int and the `F` spelling is the fixed-point rendering. (The
// floating specifiers are the numeric row's own vocabulary, not the integer
// row's.)
BEZEL_TEST(int_format_dx_width_7329, stdlib_2_18_double_d_control) {
    JitResult r = runAot(prog("", "(3.5).ToString(\"D\")"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\n"));
}

BEZEL_TEST(int_format_dx_width_7329, stdlib_2_18_double_f2_control) {
    JitResult r = runAot(prog("", "(3.5).ToString(\"F2\")"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3.50\n"));
}

// The BINARY specifier ("`B` binary" in the integer row) is a separate
// divergence of the member spelling — it is not in the `D`/`X` vocabulary
// this lane fixes, so the plain rendering is what it produces here. The row
// is a control: it says the `D`/`X` fix did not reach it.
BEZEL_TEST(int_format_dx_width_7329, stdlib_2_18_byte_b_binary_control) {
    JitResult r = runAot(prog("var b: byte := 200;", "b.ToString(\"B\")"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("200\n"));
}

// A MALFORMED suffix (a tail that is not the specifier's digit count) keeps
// the format string it had, verbatim — the promise #7286's fix-forward made
// for the pre-existing spellings.
BEZEL_TEST(int_format_dx_width_7329, stdlib_2_18_malformed_suffix_control) {
    JitResult r = runAot(prog("", "(255).ToString(\"D4x\")"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("00ffd\n"));
}
