// Regression tests for #7361 (stdlib §2.18, v0.345; spec §18.5, v0.345):
// the RUNTIME spelling of `ToString(format: string)` — a format string held
// in a VARIABLE, so its content is unknown at compile time — did not render
// the specifier vocabulary §2.18 states.
//
// THE GOVERNING TEXT.
//
// Standard library §2.18 (docs/Bezel_Standard_Library.md:1146-1156, v0.345):
//
//   "The specifier vocabulary is invariant-culture only — Bezel has no
//    culture machinery, and formatted output is identical on every platform.
//    **An unrecognized specifier raises `FormatException` (strict-fault, as
//    everywhere).**"
//
// and the Integers row of its table:
//
//   | Integers (`int`, `long`, `byte`, …) | `D`/`Dn` decimal (n = min digits,
//     zero-padded) · `X`/`x` hex (upper/lower, `Xn` pads) · `B` binary | no
//     grouping separators (no culture) |
//
// Language spec §18.5 (docs/Bezel_Language_Spec.md:3789, v0.345), "Format
// dispatch": "A hole *with* a format spec dispatches to `ToString(format:
// string)`: the primitive and temporal types provide it intrinsically (the
// specifier vocabulary and its invariant-culture semantics are normative in
// stdlib §2.18) … **An unrecognized specifier at runtime raises
// `FormatException`**".
//
// THE DEFECT. The RUNTIME spelling had a rendering of its own, and it was a
// DIFFERENT rendering from the LITERAL spelling's:
//
//   * it widened its receiver to 32 bits and read the hex digits off THAT
//     pattern — so a `sbyte` -5 rendered "FFFFFFFB" where the literal
//     spelling renders "FB" (the receiver's natural-width bit pattern);
//   * it case-folded the specifier's letter, so "x" rendered "FF";
//   * it had no `B` at all — the binary specifier rendered the plain text;
//   * it rendered the floating specifiers' plain text;
//   * and a specifier OUTSIDE the vocabulary ("Q") rendered the plain text
//     instead of raising `FormatException`.
//
// So the two spellings of one specifier gave two answers — the defect the
// family's own brief names: "Two call sites producing two answers is the
// defect; fixing one of them is not a fix."
//
// THE FIX. The runtime path now renders the SAME table the literal path
// renders (§2.18's vocabulary, the receiver's signedness, the natural-width
// bit pattern for `X`/`x`/`B`, the case the letter asks for, and the strict
// fault for a specifier outside the vocabulary). The dispatch happens at RUN
// time — the specifier is only known at run time here — so it is a
// run-time table, not a second compile-time one.
//
// THE ORACLE. `runAot` — the real AOT path. Every row pins the program's
// EXACT stdout: a wrong rendering's oracle is the text, never the exit code.
// The agreement rows print the RUNTIME spelling and the LITERAL spelling
// side by side, separated by "|", so the row says BOTH answers — and the
// literal half of every pair is independently pinned by
// `tests/codegen/int_format_dx_width_7329_test.cpp`.
//
// NOT `runAotChecked`: the ASan oracle moves `check_toolchain_skip_surface`'s
// `checked_calls` denominator, which is a coordinator-owned ratchet.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// A program whose `Main` body is `body`.
std::string prog(const std::string& body) {
    return "module M;\n"
           "public class Program\n"
           "  public static method Main(): int\n" + body +
           "    return 0;\n"
           "  end Main;\n"
           "end Program;\n";
}

// A program that RUNS (the compiler did not abort) and prints `want`.
void expectOutput(const std::string& p, const std::string& want) {
    JitResult r = runAot(p);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, want);
}

// A program whose `expr` must raise `FormatException`: the program catches
// it and prints "11", so the row distinguishes the fault (11) from the plain
// rendering (99).
void expectFormatException(const std::string& expr,
                           const std::string& decls = std::string()) {
    const std::string p =
        prog(decls +
             "    var result := 0;\n"
             "    try\n"
             "      var s: string := " + expr + ";\n"
             "      result := 99;\n"
             "    catch FormatException => result := 11;\n"
             "    end;\n"
             "    Console.WriteLine(result);\n");
    JitResult r = runAot(p);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("11\n"));
}

// The receivers the rows below use — one of every integer width, so the
// NATURAL-WIDTH rules are exercised at every width. The values are the ones
// `tests/codegen/int_format_dx_width_7329_test.cpp` pins for the LITERAL
// spelling, so the two halves of every agreement pair are pinned twice.
const char* kReceivers =
    "    var b: byte := 200;\n"
    "    var sb: sbyte := -5;\n"
    "    var sh: short := -300;\n"
    "    var ush: ushort := 65000;\n"
    "    var i: int := -5;\n"
    "    var u: uint := 4000000000;\n"
    "    var l: long := 5000000000;\n"
    "    var ul: ulong := 18446744073709551615;\n";

}  // namespace

// ===========================================================================
// #7361 — the NATURAL-WIDTH bit pattern, and the LETTER's case.
// ===========================================================================

// stdlib §2.18's integer row names `X`/`x`; the hex rendering is the
// receiver's own width's bit pattern (a `byte` 200 is "C8", not
// "00000000000000C8"). The runtime spelling used to widen the receiver to 32
// bits and print THAT pattern — "FFFFFFFB" for a `sbyte` -5.
BEZEL_TEST(runtime_format_spec_vocabulary_7361,
           the_runtime_format_renders_the_natural_width_hex_pattern_2_18) {
    expectOutput(
        prog("    var sb: sbyte := -5;\n"
             "    var b: byte := 200;\n"
             "    var sh: short := -300;\n"
             "    var i: int := -5;\n"
             "    var fmt: string := \"X\";\n"
             "    Console.WriteLine(sb.ToString(fmt));\n"
             "    Console.WriteLine(b.ToString(fmt));\n"
             "    Console.WriteLine(sh.ToString(fmt));\n"
             "    Console.WriteLine(i.ToString(fmt));\n"),
        "FB\nC8\nFED4\nFFFFFFFB\n");
}

// stdlib §2.18: "`X`/`x` hex (upper/lower, `Xn` pads)" — the LETTER fixes
// the CASE of the digits. The runtime spelling case-folded it, so "x"
// printed "FF".
BEZEL_TEST(runtime_format_spec_vocabulary_7361,
           the_runtime_format_honours_the_letter_case_2_18) {
    expectOutput(
        prog("    var b: byte := 200;\n"
             "    var sb: sbyte := -5;\n"
             "    var fmt: string := \"x\";\n"
             "    Console.WriteLine(b.ToString(fmt));\n"
             "    Console.WriteLine(sb.ToString(fmt));\n"),
        "c8\nfb\n");
}

// ===========================================================================
// THE AGREEMENT ROWS — the two spellings of one specifier must answer
// alike. Each line is "<runtime spelling>|<literal spelling>".
// ===========================================================================

// The hex specifier, at every integer width, in both cases.
BEZEL_TEST(runtime_format_spec_vocabulary_7361,
           the_two_spellings_agree_on_the_hex_specifier_2_18) {
    expectOutput(
        prog(std::string(kReceivers) +
             "    var fX: string := \"X\";\n"
             "    var fx: string := \"x\";\n"
             "    Console.WriteLine(b.ToString(fX) + \"|\" + b.ToString(\"X\"));\n"
             "    Console.WriteLine(b.ToString(fx) + \"|\" + b.ToString(\"x\"));\n"
             "    Console.WriteLine(sb.ToString(fX) + \"|\" + sb.ToString(\"X\"));\n"
             "    Console.WriteLine(sb.ToString(fx) + \"|\" + sb.ToString(\"x\"));\n"
             "    Console.WriteLine(sh.ToString(fX) + \"|\" + sh.ToString(\"X\"));\n"
             "    Console.WriteLine(sh.ToString(fx) + \"|\" + sh.ToString(\"x\"));\n"
             "    Console.WriteLine(ush.ToString(fX) + \"|\" + ush.ToString(\"X\"));\n"
             "    Console.WriteLine(ush.ToString(fx) + \"|\" + ush.ToString(\"x\"));\n"
             "    Console.WriteLine(i.ToString(fX) + \"|\" + i.ToString(\"X\"));\n"
             "    Console.WriteLine(i.ToString(fx) + \"|\" + i.ToString(\"x\"));\n"
             "    Console.WriteLine(u.ToString(fX) + \"|\" + u.ToString(\"X\"));\n"
             "    Console.WriteLine(u.ToString(fx) + \"|\" + u.ToString(\"x\"));\n"
             "    Console.WriteLine(l.ToString(fX) + \"|\" + l.ToString(\"X\"));\n"
             "    Console.WriteLine(l.ToString(fx) + \"|\" + l.ToString(\"x\"));\n"
             "    Console.WriteLine(ul.ToString(fX) + \"|\" + ul.ToString(\"X\"));\n"
             "    Console.WriteLine(ul.ToString(fx) + \"|\" + ul.ToString(\"x\"));\n"),
        "C8|C8\n"
        "c8|c8\n"
        "FB|FB\n"
        "fb|fb\n"
        "FED4|FED4\n"
        "fed4|fed4\n"
        "FDE8|FDE8\n"
        "fde8|fde8\n"
        "FFFFFFFB|FFFFFFFB\n"
        "fffffffb|fffffffb\n"
        "EE6B2800|EE6B2800\n"
        "ee6b2800|ee6b2800\n"
        "12A05F200|12A05F200\n"
        "12a05f200|12a05f200\n"
        "FFFFFFFFFFFFFFFF|FFFFFFFFFFFFFFFF\n"
        "ffffffffffffffff|ffffffffffffffff\n");
}

// The decimal specifier — the DECIMAL of the value the caller holds, so an
// unsigned receiver keeps its value ("4000000000", not "-294967296") — and
// the count-bearing spelling (`Dn`).
BEZEL_TEST(runtime_format_spec_vocabulary_7361,
           the_two_spellings_agree_on_the_decimal_specifier_2_18) {
    expectOutput(
        prog(std::string(kReceivers) +
             "    var fD: string := \"D\";\n"
             "    var fD5: string := \"D5\";\n"
             "    Console.WriteLine(b.ToString(fD) + \"|\" + b.ToString(\"D\"));\n"
             "    Console.WriteLine(sb.ToString(fD) + \"|\" + sb.ToString(\"D\"));\n"
             "    Console.WriteLine(sh.ToString(fD) + \"|\" + sh.ToString(\"D\"));\n"
             "    Console.WriteLine(ush.ToString(fD) + \"|\" + ush.ToString(\"D\"));\n"
             "    Console.WriteLine(i.ToString(fD) + \"|\" + i.ToString(\"D\"));\n"
             "    Console.WriteLine(u.ToString(fD) + \"|\" + u.ToString(\"D\"));\n"
             "    Console.WriteLine(l.ToString(fD) + \"|\" + l.ToString(\"D\"));\n"
             "    Console.WriteLine(ul.ToString(fD) + \"|\" + ul.ToString(\"D\"));\n"
             "    Console.WriteLine(b.ToString(fD5) + \"|\" + b.ToString(\"D5\"));\n"
             "    Console.WriteLine(sb.ToString(fD5) + \"|\" + sb.ToString(\"D5\"));\n"
             "    Console.WriteLine(i.ToString(fD5) + \"|\" + i.ToString(\"D5\"));\n"),
        "200|200\n"
        "-5|-5\n"
        "-300|-300\n"
        "65000|65000\n"
        "-5|-5\n"
        "4000000000|4000000000\n"
        "5000000000|5000000000\n"
        "18446744073709551615|18446744073709551615\n"
        "00200|00200\n"
        "-0005|-0005\n"
        "-0005|-0005\n");
}

// stdlib §2.18's integer row names `B` — the binary specifier. The runtime
// spelling had no `B` at all and rendered the plain text.
BEZEL_TEST(runtime_format_spec_vocabulary_7361,
           the_two_spellings_agree_on_the_binary_specifier_2_18) {
    expectOutput(
        prog(std::string(kReceivers) +
             "    var fB: string := \"B\";\n"
             "    Console.WriteLine(b.ToString(fB) + \"|\" + b.ToString(\"B\"));\n"
             "    Console.WriteLine(i.ToString(fB) + \"|\" + i.ToString(\"B\"));\n"
             "    Console.WriteLine(u.ToString(fB) + \"|\" + u.ToString(\"B\"));\n"
             "    Console.WriteLine(ul.ToString(fB) + \"|\" + ul.ToString(\"B\"));\n"),
        "11001000|11001000\n"
        "11111111111111111111111111111011|"
        "11111111111111111111111111111011\n"
        "11101110011010110010100000000000|"
        "11101110011010110010100000000000\n"
        "1111111111111111111111111111111111111111111111111111111111111111|"
        "1111111111111111111111111111111111111111111111111111111111111111\n");
}

// The floating specifiers. §2.18's table puts `F`/`Fn`, `E`/`En`, `G`/`Gn`
// and `P`/`Pn` in the FLOATING row, but the house's shared rendering
// applies them to an INTEGER operand as well (the interpolation spelling has
// always done so: `$"{42:F2}"` is "42.00"). The runtime spelling rendered
// the plain text for all of them.
//
// THIS ROW PINS THE AGREEMENT BETWEEN THE TWO SPELLINGS, NOT THE CHOICE OF
// ANSWER: whether §2.18's FLOATING-row specifiers are recognized on an
// INTEGER receiver is an OPEN SPEC QUESTION (routed to the next train; the
// compiler's answer does not change in this batch). What is pinned here is
// only that the runtime spelling and the literal spelling of ONE specifier
// answer ALIKE — which holds whichever way that question is decided.
BEZEL_TEST(runtime_format_spec_vocabulary_7361,
           the_two_spellings_agree_on_the_floating_specifiers_2_18) {
    expectOutput(
        prog("    var i: int := 42;\n"
             "    var u: uint := 4000000000;\n"
             "    var fF2: string := \"F2\";\n"
             "    var fE: string := \"E\";\n"
             "    var fG: string := \"G\";\n"
             "    var fP1: string := \"P1\";\n"
             "    Console.WriteLine(i.ToString(fF2) + \"|\" + i.ToString(\"F2\"));\n"
             "    Console.WriteLine(i.ToString(fE) + \"|\" + i.ToString(\"E\"));\n"
             "    Console.WriteLine(i.ToString(fG) + \"|\" + i.ToString(\"G\"));\n"
             "    Console.WriteLine(i.ToString(fP1) + \"|\" + i.ToString(\"P1\"));\n"
             "    Console.WriteLine(u.ToString(fF2) + \"|\" + u.ToString(\"F2\"));\n"
             "    Console.WriteLine(u.ToString(fE) + \"|\" + u.ToString(\"E\"));\n"),
        "42.00|42.00\n"
        "4.200000E+01|4.200000E+01\n"
        "42|42\n"
        "4200.0%|4200.0%\n"
        "4000000000.00|4000000000.00\n"
        "4.000000E+09|4.000000E+09\n");
}

// ===========================================================================
// THE FAULT — a specifier outside the vocabulary.
// ===========================================================================

// stdlib §2.18: "An unrecognized specifier raises `FormatException`
// (strict-fault, as everywhere)". The runtime spelling used to render the
// plain text ("255") where the literal spelling raised.
BEZEL_TEST(runtime_format_spec_vocabulary_7361,
           an_unrecognized_runtime_specifier_raises_format_exception_2_18) {
    expectFormatException("(255).ToString(fmt)",
                           "    var fmt: string := \"Q\";\n");
}

// A malformed tail is not "the specifier with a 0 count" — it is an
// unrecognized specifier, and it must fault rather than render.
BEZEL_TEST(runtime_format_spec_vocabulary_7361,
           a_malformed_runtime_suffix_raises_format_exception_2_18) {
    expectFormatException("(255).ToString(fmt)",
                           "    var fmt: string := \"D4x\";\n");
}

// ===========================================================================
// THE BOUNDARY — a format with NO specifier.
// ===========================================================================

// An EMPTY format names no specifier at all, so the rendering is the
// no-format text — the answer the LITERAL spelling gives for "".
BEZEL_TEST(runtime_format_spec_vocabulary_7361,
           an_empty_runtime_format_renders_the_no_format_text_2_18) {
    expectOutput(
        prog("    var i: int := 255;\n"
             "    var fmt: string := \"\";\n"
             "    Console.WriteLine(i.ToString(fmt));\n"
             "    Console.WriteLine(i.ToString(fmt) + \"|\" + i.ToString(\"\"));\n"),
        "255\n255|255\n");
}

// The COUNT is bounded the same way in both spellings: stdlib §2.18's `Dn`
// count is a minimum digit count, and the house clamps it to what the
// rendering can hold (the LITERAL spelling clamps the digit run; the runtime
// spelling clamps the parsed width to the same bound).
BEZEL_TEST(runtime_format_spec_vocabulary_7361,
           the_two_spellings_agree_on_a_clamped_count_2_18) {
    expectOutput(
        prog("    var i: int := 42;\n"
             "    var fD100: string := \"D100\";\n"
             "    var fDbig: string := \"D1000000000\";\n"
             "    Console.WriteLine(i.ToString(fD100) + \"|\" + i.ToString(\"D100\"));\n"
             "    Console.WriteLine(i.ToString(fDbig) + \"|\" + i.ToString(\"D1000000000\"));\n"),
        "00000000000000000000000000000000000000000000000000000000000000042|"
        "00000000000000000000000000000000000000000000000000000000000000042\n"
        "00000000000000000000000000000000000000000000000000000000000000042|"
        "00000000000000000000000000000000000000000000000000000000000000042\n");
}
