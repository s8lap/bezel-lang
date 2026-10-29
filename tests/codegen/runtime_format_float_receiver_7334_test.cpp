// Regression tests for #7334 (stdlib §2.18, v0.345; spec §18.5, v0.345):
// the RUNTIME spelling of `ToString(format: string)` — a format string held
// in a VARIABLE, so its content is unknown at compile time — rendered the
// PLAIN (no-format) text on a FLOATING receiver, where the LITERAL spelling
// rendered the format form.
//
// THE GOVERNING TEXT.
//
// Standard library §2.18 (docs/Bezel_Standard_Library.md, v0.345),
// the format-specifier table, per KIND:
//
//   | Floating (`double`, `float`) | `F`/`Fn` fixed-point (n decimals,
//     default 2) · `E`/`En` scientific · `G`/`Gn` general · `P`/`Pn`
//     percent (×100, `%` suffix) · `R` round-trip | … |
//
// The document fixes a default digit count for `F` only. The `E`/`G`/`P`
// defaults this test relies on (six, six, two) are the HOUSE's, from the
// shared routine emitWithFormatSpec; the document says nothing about them.
//
//   | `decimal` | `F`/`Fn` · `E`/`En` · `G` · `P`/`Pn` | … |
//
// and, for the whole table:
//
//   "**An unrecognized specifier raises `FormatException` (strict-fault, as
//    everywhere).**"
//
// The `decimal` row names NO `R`, and its arithmetic is EXACT (§2.18's
// midpoint rule), so the decimal row is its own rendering — not the floating
// one.
//
// Language spec §18.5 (docs/Bezel_Language_Spec.md:3799, v0.345), "Format
// dispatch": "A hole *with* a format spec dispatches to `ToString(format:
// string)`: the primitive and temporal types provide it intrinsically (the
// specifier vocabulary and its invariant-culture semantics are normative in
// stdlib §2.18) … **An unrecognized specifier at runtime raises
// `FormatException`**".
//
// stdlib §2.18 (v0.344, #7301): "The no-format `ToString()` and every
// floating specifier (`F`, `E`, `G` and `P`, with or without a digit count,
// and `R`) render a NaN as `NaN`, positive infinity as `Infinity` and negative
// infinity as `-Infinity` — the names of the `NaN`, `PositiveInfinity` and
// `NegativeInfinity` constants (§9.2) — with no digits, no exponent and no
// `%` suffix."
//
// THE DEFECT. §2.18's table is about the VALUE'S TYPE, not about the spelling
// of the format argument, so the two spellings of one specifier are ONE
// obligation. The runtime spelling was dispatched only for an INTEGER
// receiver, so a `double`/`float` receiver never reached the runtime dispatch
// at all: it fell out of the arm to the plain (no-format) rendering.
//
//   * `1.5.ToString(f)` with `f := "F2"` rendered "1.5" where
//     `1.5.ToString("F2")` rendered "1.50";
//   * `1.5.ToString(f)` with `f := "E"` rendered "1.5" where
//     `1.5.ToString("E")` rendered "1.500000E+00";
//   * `1.5.ToString(f)` with `f := "P"` rendered "1.5" where
//     `1.5.ToString("P")` rendered "150.00%";
//   * and a specifier OUTSIDE the floating vocabulary ("Z", "D") rendered the
//     plain text where the literal spelling raised `FormatException`.
//
// THE FIX. The runtime-format arm in `lowerExprInner` now dispatches for a
// FLOATING receiver over §2.18's floating vocabulary, rendering it in the
// runtime helpers `bz_double_to_string_fmt` / `bz_float_to_string_fmt` —
// the same table the LITERAL spelling renders in the shared routine
// (`emitWithFormatSpec`). The two spellings of one specifier answer alike.
//
// The `decimal` row is NOT part of this fix: it is its own vocabulary (no
// `R`) with EXACT arithmetic, and it already reached the decimal formatter
// (`bz_decimal_to_string_fmt`) at BOTH spellings. The rows below pin that
// too, so a later change cannot quietly route the decimal row through the
// floating one (which would lose the exactness) or give it an `R`.
//
// THE ORACLE. `runAot` — the real AOT path. Every row pins the program's
// EXACT stdout: a wrong rendering's oracle is the text, never the exit code.
// The agreement rows print the RUNTIME spelling and the LITERAL spelling
// side by side, separated by "|", so the row says BOTH answers.
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

// The receivers the rows below use: a `double` and a `float` (Bezel's 32-bit
// single — the `f` suffix is what makes the literal a `float`).
const char* kReceivers =
    "    var d := 1.5;\n"
    "    var fl: float := 1.5f;\n";

}  // namespace

// ===========================================================================
// THE AGREEMENT ROWS — the two spellings of one specifier must answer alike.
// Each line is "<runtime spelling>|<literal spelling>".
// ===========================================================================

// §2.18's FLOATING row, whole: `F`/`Fn`, `E`/`En`, `G`/`Gn`, `P`/`Pn`, `R`.
// The runtime spelling rendered the PLAIN text for every one of them.
BEZEL_TEST(runtime_format_float_receiver_7334,
           the_two_spellings_agree_on_the_floating_row_2_18) {
    expectOutput(
        prog(std::string(kReceivers) +
             "    var fF: string := \"F\";\n"
             "    var fF2: string := \"F2\";\n"
             "    var fE: string := \"E\";\n"
             "    var fE3: string := \"E3\";\n"
             "    var fG: string := \"G\";\n"
             "    var fG5: string := \"G5\";\n"
             "    var fP: string := \"P\";\n"
             "    var fP1: string := \"P1\";\n"
             "    var fR: string := \"R\";\n"
             "    Console.WriteLine(d.ToString(fF) + \"|\" + d.ToString(\"F\"));\n"
             "    Console.WriteLine(d.ToString(fF2) + \"|\" + d.ToString(\"F2\"));\n"
             "    Console.WriteLine(d.ToString(fE) + \"|\" + d.ToString(\"E\"));\n"
             "    Console.WriteLine(d.ToString(fE3) + \"|\" + d.ToString(\"E3\"));\n"
             "    Console.WriteLine(d.ToString(fG) + \"|\" + d.ToString(\"G\"));\n"
             "    Console.WriteLine(d.ToString(fG5) + \"|\" + d.ToString(\"G5\"));\n"
             "    Console.WriteLine(d.ToString(fP) + \"|\" + d.ToString(\"P\"));\n"
             "    Console.WriteLine(d.ToString(fP1) + \"|\" + d.ToString(\"P1\"));\n"
             "    Console.WriteLine(d.ToString(fR) + \"|\" + d.ToString(\"R\"));\n"),
        "1.50|1.50\n"
        "1.50|1.50\n"
        "1.500000E+00|1.500000E+00\n"
        "1.500E+00|1.500E+00\n"
        "1.5|1.5\n"
        "1.5|1.5\n"
        "150.00%|150.00%\n"
        "150.0%|150.0%\n"
        "1.5|1.5\n");
}

// The `float` receiver — Bezel's 32-bit single — at the same specifiers.
BEZEL_TEST(runtime_format_float_receiver_7334,
           the_two_spellings_agree_on_a_float_receiver_2_18) {
    expectOutput(
        prog(std::string(kReceivers) +
             "    var fF2: string := \"F2\";\n"
             "    var fE3: string := \"E3\";\n"
             "    var fG5: string := \"G5\";\n"
             "    var fP1: string := \"P1\";\n"
             "    var fR: string := \"R\";\n"
             "    Console.WriteLine(fl.ToString(fF2) + \"|\" + fl.ToString(\"F2\"));\n"
             "    Console.WriteLine(fl.ToString(fE3) + \"|\" + fl.ToString(\"E3\"));\n"
             "    Console.WriteLine(fl.ToString(fG5) + \"|\" + fl.ToString(\"G5\"));\n"
             "    Console.WriteLine(fl.ToString(fP1) + \"|\" + fl.ToString(\"P1\"));\n"
             "    Console.WriteLine(fl.ToString(fR) + \"|\" + fl.ToString(\"R\"));\n"),
        "1.50|1.50\n"
        "1.500E+00|1.500E+00\n"
        "1.5|1.5\n"
        "150.0%|150.0%\n"
        "1.5|1.5\n");
}

// §2.18: "The specifier vocabulary is invariant-culture only" — and the
// LETTER's case fixes the output case for `E`/`G` ("e" → "1.500000e+00",
// "E" → "1.500000E+00"). The runtime spelling must not fold the case away.
BEZEL_TEST(runtime_format_float_receiver_7334,
           the_runtime_format_honours_the_letter_case_2_18) {
    expectOutput(
        prog(std::string(kReceivers) +
             "    var fe: string := \"e\";\n"
             "    var fg: string := \"g\";\n"
             "    var fr: string := \"r\";\n"
             "    Console.WriteLine(d.ToString(fe) + \"|\" + d.ToString(\"e\"));\n"
             "    Console.WriteLine(d.ToString(fg) + \"|\" + d.ToString(\"g\"));\n"
             "    Console.WriteLine(d.ToString(fr) + \"|\" + d.ToString(\"r\"));\n"),
        "1.500000e+00|1.500000e+00\n"
        "1.5|1.5\n"
        "1.5|1.5\n");
}

// §2.18's `R` (round-trip) is the SHORTEST rendering that reads back as the
// same value — the same text the no-format `ToString()` produces, and the
// same text at both spellings.
BEZEL_TEST(runtime_format_float_receiver_7334,
           the_round_trip_specifier_agrees_at_both_spellings_2_18) {
    expectOutput(
        prog(std::string(kReceivers) +
             "    var fR: string := \"R\";\n"
             "    var third := 1.0 / 3.0;\n"
             "    Console.WriteLine(d.ToString(fR) + \"|\" + d.ToString(\"R\"));\n"
             "    Console.WriteLine(third.ToString(fR) + \"|\" + third.ToString(\"R\"));\n"
             "    Console.WriteLine(fl.ToString(fR) + \"|\" + fl.ToString(\"R\"));\n"),
        "1.5|1.5\n"
        "0.3333333333333333|0.3333333333333333\n"
        "1.5|1.5\n");
}

// ===========================================================================
// THE FAULT — a specifier outside §2.18's FLOATING vocabulary.
// ===========================================================================

// §2.18: "An unrecognized specifier raises `FormatException` (strict-fault,
// as everywhere)". On a FLOATING receiver the runtime spelling used to
// render the plain text where the literal spelling raised.
BEZEL_TEST(runtime_format_float_receiver_7334,
           an_unrecognized_runtime_specifier_raises_format_exception_2_18) {
    expectFormatException(
        "d.ToString(fmt)", std::string(kReceivers) + "    var fmt: string := \"Z\";\n");
    expectFormatException(
        "d.ToString(\"Z\")", std::string(kReceivers));
    expectFormatException(
        "d.ToString(fmt)", std::string(kReceivers) + "    var fmt: string := \"C\";\n");
    expectFormatException(
        "fl.ToString(fmt)", std::string(kReceivers) + "    var fmt: string := \"Z\";\n");
}

// `D`/`X`/`B` are the INTEGERS row's specifiers — §2.18's table is per KIND,
// so they are outside the floating vocabulary and fault at both spellings.
BEZEL_TEST(runtime_format_float_receiver_7334,
           an_integer_row_specifier_faults_on_a_floating_receiver_2_18) {
    expectFormatException(
        "d.ToString(fmt)", std::string(kReceivers) + "    var fmt: string := \"D\";\n");
    expectFormatException(
        "d.ToString(fmt)", std::string(kReceivers) + "    var fmt: string := \"X\";\n");
    expectFormatException(
        "d.ToString(fmt)", std::string(kReceivers) + "    var fmt: string := \"B\";\n");
}

// A MALFORMED tail is not a count but an unrecognized specifier: "F2x" is
// outside the vocabulary, at both spellings.
BEZEL_TEST(runtime_format_float_receiver_7334,
           a_malformed_runtime_specifier_raises_format_exception_2_18) {
    expectFormatException(
        "d.ToString(fmt)", std::string(kReceivers) + "    var fmt: string := \"F2x\";\n");
    expectFormatException(
        "d.ToString(\"F2x\")", std::string(kReceivers));
}

// §2.18's `R` takes NO count — "R2" is not in the vocabulary.
BEZEL_TEST(runtime_format_float_receiver_7334,
           a_counted_round_trip_specifier_raises_format_exception_2_18) {
    expectFormatException(
        "d.ToString(fmt)", std::string(kReceivers) + "    var fmt: string := \"R2\";\n");
    expectFormatException(
        "d.ToString(\"R2\")", std::string(kReceivers));
}

// ===========================================================================
// THE NON-FINITE OPERANDS — §2.18 (v0.344, #7301).
// ===========================================================================

// "every floating specifier (`F`, `E`, `G` and `P`, with or without a digit
// count, and `R`) render a NaN as `NaN`, positive infinity as `Infinity` and
// negative infinity as `-Infinity` — with no digits, no exponent and no `%`
// suffix". The runtime spelling must not reach snprintf (C prints
// "inf"/"nan").
BEZEL_TEST(runtime_format_float_receiver_7334,
           the_non_finite_operands_render_their_names_2_18) {
    expectOutput(
        prog(std::string(kReceivers) +
             "    var pInf := double.PositiveInfinity;\n"
             "    var nInf := double.NegativeInfinity;\n"
             "    var nan := double.NaN;\n"
             "    var fF2: string := \"F2\";\n"
             "    var fE: string := \"E\";\n"
             "    var fG: string := \"G\";\n"
             "    var fP: string := \"P\";\n"
             "    var fR: string := \"R\";\n"
             "    Console.WriteLine(pInf.ToString(fF2) + \"|\" + pInf.ToString(\"F2\"));\n"
             "    Console.WriteLine(pInf.ToString(fE) + \"|\" + pInf.ToString(\"E\"));\n"
             "    Console.WriteLine(pInf.ToString(fG) + \"|\" + pInf.ToString(\"G\"));\n"
             "    Console.WriteLine(pInf.ToString(fP) + \"|\" + pInf.ToString(\"P\"));\n"
             "    Console.WriteLine(pInf.ToString(fR) + \"|\" + pInf.ToString(\"R\"));\n"
             "    Console.WriteLine(nInf.ToString(fF2) + \"|\" + nInf.ToString(\"F2\"));\n"
             "    Console.WriteLine(nan.ToString(fF2) + \"|\" + nan.ToString(\"F2\"));\n"
             "    Console.WriteLine(nan.ToString(fR) + \"|\" + nan.ToString(\"R\"));\n"),
        "Infinity|Infinity\n"
        "Infinity|Infinity\n"
        "Infinity|Infinity\n"
        "Infinity|Infinity\n"
        "Infinity|Infinity\n"
        "-Infinity|-Infinity\n"
        "NaN|NaN\n"
        "NaN|NaN\n");
}

// ===========================================================================
// THE DECIMAL ROW — its own vocabulary, its own arithmetic.
// ===========================================================================

// §2.18's `decimal` row is `F`/`Fn`, `E`/`En`, `G` and `P`/`Pn` — with NO
// `R` — and its arithmetic is EXACT (§2.18's midpoint rule), so the decimal
// row is rendered by the decimal formatter, not the floating one. The two
// spellings already agree; these rows pin that, so a later change cannot
// quietly route the decimal row through the floating rendering (which would
// lose the exactness) or give it an `R`.
BEZEL_TEST(runtime_format_float_receiver_7334,
           the_decimal_row_is_its_own_vocabulary_2_18) {
    expectOutput(
        prog("    var dec := 1.5m;\n"
             "    var fF2: string := \"F2\";\n"
             "    var fE: string := \"E\";\n"
             "    var fG: string := \"G\";\n"
             "    var fP: string := \"P\";\n"
             "    Console.WriteLine(dec.ToString(fF2) + \"|\" + dec.ToString(\"F2\"));\n"
             "    Console.WriteLine(dec.ToString(fE) + \"|\" + dec.ToString(\"E\"));\n"
             "    Console.WriteLine(dec.ToString(fG) + \"|\" + dec.ToString(\"G\"));\n"
             "    Console.WriteLine(dec.ToString(fP) + \"|\" + dec.ToString(\"P\"));\n"),
        "1.50|1.50\n"
        "1.500000E+000|1.500000E+000\n"
        "1.5|1.5\n"
        "150.00%|150.00%\n");
}

// The decimal row has NO `R`: the round-trip specifier is outside it, so it
// faults at BOTH spellings.
BEZEL_TEST(runtime_format_float_receiver_7334,
           the_round_trip_specifier_is_outside_the_decimal_row_2_18) {
    expectFormatException(
        "dec.ToString(fmt)",
        "    var dec := 1.5m;\n"
        "    var fmt: string := \"R\";\n");
    expectFormatException(
        "dec.ToString(\"R\")",
        "    var dec := 1.5m;\n");
}
