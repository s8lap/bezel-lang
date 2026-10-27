// Regression tests for the FORMAT-SPECIFIER RENDERING CONTRACT of the
// one-argument `ToString(format: string)` member call — the family
// #7311 / #7359 / #7265 / #7303 / #7362 / #7358 (stdlib §2.18, v0.345;
// spec §18.5, v0.345).
//
// THE GOVERNING TEXT.
//
// Standard library §2.18, the specifier table (docs/
// Bezel_Standard_Library.md:1149-1156, v0.345), the Integers row:
//
//   | Integers (`int`, `long`, `byte`, …) | `D`/`Dn` decimal (n = min digits,
//     zero-padded) · `X`/`x` hex (upper/lower, `Xn` pads) · `B` binary | no
//     grouping separators (no culture) |
//
// and the Floating row:
//
//   | Floating (`double`, `float`) | `F`/`Fn` fixed-point (n decimals,
//     default 2) · `E`/`En` scientific · `G`/`Gn` general · `P`/`Pn` percent
//     (×100, `%` suffix) · `R` round-trip | … |
//
// and the sentence that fixes the fault for a specifier outside the
// vocabulary (the same section, :1146):
//
//   **An unrecognized specifier raises `FormatException` (strict-fault, as
//   everywhere).**
//
// Language spec §18.5 (docs/Bezel_Language_Spec.md:3789, v0.345), "Format
// dispatch": "A hole *with* a format spec dispatches to `ToString(format:
// string)`: the primitive and temporal types provide it intrinsically (the
// specifier vocabulary and its invariant-culture semantics are normative in
// stdlib §2.18) … **An unrecognized specifier at runtime raises
// `FormatException`**".
//
// THE DEFECT. The MEMBER-CALL spelling of `ToString(format)` implemented its
// own PARTIAL copy of §2.18's table — `D`, `X` and the floating `F`/`E`/`G`,
// with the letter case folded away and no `B` — and every specifier outside
// that copy silently rendered the receiver's plain text. The
// INTERPOLATION spelling went through the shared routine and had the whole
// table. Two call sites, two answers:
//
//   #7311  the member-call path renders the no-form text where the
//          interpolation path renders the format form;
//   #7359  `(255).ToString("x")` printed "FF" — §2.18's "X/x hex
//          (upper/lower)" makes the LETTER fix the CASE;
//   #7265  an unsigned receiver's `D` rendered signed, the binary specifier
//          was ignored, and a `long`/`uint` receiver ABORTED the compiler;
//   #7303  `(1.5).ToString("C")` printed "1.5" where §2.18 says an
//          unrecognized specifier raises `FormatException` (`C` deliberately
//          does not exist);
//   #7362  `D` on a floating receiver TRUNCATED the value ("3" for 3.5)
//          where the specifier is not in the floating receiver's vocabulary;
//   #7358  a malformed suffix on a narrow receiver emitted INVALID IR and
//          the compiler aborted ("DestTy too big for Trunc").
//
// THE FIX. The member-call spelling now goes through the SHARED formatting
// routine — the same one the interpolation spelling uses — so the two call
// sites cannot produce two answers. The receiver-kind gates that the shared
// routine does not have (an EMPTY vocabulary for `bool`/`char`/`string`, and
// the §18.5 alignment prefix, which is not a §2.18 specifier) stay in the
// member arm.
//
// THE ORACLE. `runAot` — the real AOT path. Every row pins the program's
// EXACT stdout: a wrong rendering's oracle is the text, never the exit code.
// A row that expects the `FormatException` catches it and prints a marker, so
// the row says WHICH outcome happened. A compiler abort is observed as "the
// program did not run" (`r.ran` is false), so the #7358 row is a real
// oracle too.
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

}  // namespace

// ===========================================================================
// #7359 — the LETTER fixes the CASE of the hex digits.
// ===========================================================================

// stdlib §2.18's integer row: "`X`/`x` hex (upper/lower, `Xn` pads)". So
// "x" renders lower-case and "X" upper-case. Before the shared routing the
// member spelling case-folded the letter and printed "C8" for "x".
BEZEL_TEST(format_spec_shared_rendering_7311,
           the_lower_case_hex_specifier_renders_lower_case_digits_2_18) {
    expectOutput(
        prog("    var b: byte := 200;\n"
             "    Console.WriteLine(b.ToString(\"x\"));\n"
             "    Console.WriteLine(b.ToString(\"X\"));\n"),
        "c8\nC8\n");
}

// The interpolation spelling renders lower-case too — the two spellings of
// one specifier agree.
BEZEL_TEST(format_spec_shared_rendering_7311,
           the_member_and_the_interpolation_spelling_of_x_agree_2_18) {
    expectOutput(
        prog("    var b: byte := 200;\n"
             "    Console.WriteLine(b.ToString(\"x\"));\n"
             "    Console.WriteLine($\"{b:x}\");\n"),
        "c8\nc8\n");
}

// A SIGNED narrow receiver renders its natural-width bit pattern, in the
// case its letter asks for.
BEZEL_TEST(format_spec_shared_rendering_7311,
           a_signed_narrow_receiver_renders_its_natural_width_pattern_2_18) {
    expectOutput(
        prog("    var s: sbyte := -5;\n"
             "    var sh: short := -300;\n"
             "    Console.WriteLine(s.ToString(\"X\"));\n"
             "    Console.WriteLine(s.ToString(\"x\"));\n"
             "    Console.WriteLine(sh.ToString(\"X\"));\n"),
        "FB\nfb\nFED4\n");
}

// ===========================================================================
// #7265 — an unsigned receiver keeps its value, and the binary specifier
// is rendered.
// ===========================================================================

// stdlib §2.18: "`D`/`Dn` decimal" — the DECIMAL of the value the caller
// holds. An unsigned receiver's value is its unsigned value, not a signed
// reading of the same bits.
BEZEL_TEST(format_spec_shared_rendering_7311,
           an_unsigned_receiver_renders_its_unsigned_value_2_18) {
    expectOutput(
        prog("    var u: uint := 4000000000;\n"
             "    var ul: ulong := 18446744073709551615;\n"
             "    Console.WriteLine(u.ToString(\"D\"));\n"
             "    Console.WriteLine(ul.ToString(\"D\"));\n"),
        "4000000000\n18446744073709551615\n");
}

// stdlib §2.18: "`B` binary" — the receiver's natural-width bit pattern in
// binary digits. The member spelling had no `B` at all and rendered the
// receiver's plain text.
BEZEL_TEST(format_spec_shared_rendering_7311,
           the_binary_specifier_renders_the_binary_digits_2_18) {
    expectOutput(
        prog("    var b: byte := 200;\n"
             "    Console.WriteLine(b.ToString(\"B\"));\n"
             "    Console.WriteLine((255).ToString(\"B\"));\n"
             "    Console.WriteLine($\"{b:B}\");\n"),
        "11001000\n11111111\n11001000\n");
}

// #7265's CRASH: the literal format overload used to ABORT the compiler for
// a `long`/`uint` receiver (an FP→SI conversion applied to an integer
// payload). The program must compile and run.
BEZEL_TEST(format_spec_shared_rendering_7311,
           the_literal_format_overload_compiles_for_every_integer_width_2_18) {
    expectOutput(
        prog("    var b: byte := 200;\n"
             "    var sb: sbyte := -5;\n"
             "    var sh: short := -300;\n"
             "    var ush: ushort := 60000;\n"
             "    var i: int := -5;\n"
             "    var u: uint := 4000000000;\n"
             "    var l: long := 5000000000;\n"
             "    var ul: ulong := 18446744073709551615;\n"
             "    Console.WriteLine(b.ToString(\"D\"));\n"
             "    Console.WriteLine(sb.ToString(\"D\"));\n"
             "    Console.WriteLine(sh.ToString(\"D\"));\n"
             "    Console.WriteLine(ush.ToString(\"D\"));\n"
             "    Console.WriteLine(i.ToString(\"D\"));\n"
             "    Console.WriteLine(u.ToString(\"D\"));\n"
             "    Console.WriteLine(l.ToString(\"D\"));\n"
             "    Console.WriteLine(ul.ToString(\"D\"));\n"),
        "200\n-5\n-300\n60000\n-5\n4000000000\n5000000000\n"
        "18446744073709551615\n");
}

// ===========================================================================
// #7303 / #7362 — a specifier outside the receiver's vocabulary raises
// FormatException; it is not rendered as the plain text.
// ===========================================================================

// stdlib §2.18: "`C` (currency) deliberately does not exist", and "An
// unrecognized specifier raises `FormatException` (strict-fault, as
// everywhere)". The member spelling printed "1.5".
BEZEL_TEST(format_spec_shared_rendering_7311,
           an_unrecognized_specifier_raises_format_exception_2_18) {
    expectFormatException("(1.5).ToString(\"C\")");
}

// The floating receiver's vocabulary is `F`/`E`/`G`/`P`/`R`; `D` is the
// INTEGERS row's specifier, so on a `double` it is unrecognized — and the
// value is not truncated to an int.
BEZEL_TEST(format_spec_shared_rendering_7311,
           the_D_specifier_on_a_floating_receiver_raises_format_exception_2_18) {
    expectFormatException("(3.5).ToString(\"D\")");
}

// The INTEGERS row's vocabulary is `D`/`X`/`x`/`B`; `F` is the FLOATING
// row's specifier. The house's shared rendering applies the floating
// specifiers to an integer operand as well (the interpolation spelling has
// always done so), so this row pins the SHARED answer — and it is the same
// answer the interpolation spelling gives.
BEZEL_TEST(format_spec_shared_rendering_7311,
           an_integer_receiver_renders_the_floating_specifier_like_the_hole_18_5) {
    expectOutput(
        prog("    Console.WriteLine((42).ToString(\"F2\"));\n"
             "    Console.WriteLine($\"{(42):F2}\");\n"),
        "42.00\n42.00\n");
}

// ===========================================================================
// #7358 — a malformed suffix is not the specifier with a 0 count; it is an
// unrecognized specifier, and it must not make the compiler emit invalid
// IR.
// ===========================================================================

// The member spelling used to build a 32-bit conversion for a narrow
// receiver ("trunc i8 %2 to i32") — invalid IR, and the compiler ABORTED
// with "DestTy too big for Trunc". The program must compile and run.
BEZEL_TEST(format_spec_shared_rendering_7311,
           a_malformed_suffix_on_a_narrow_receiver_raises_format_exception_2_18) {
    expectFormatException("b.ToString(\"D4x\")", "    var b: byte := 200;\n");
}

// The same shape on a 32-bit receiver: the fault, not the malformed
// rendering ("00ffd") the member spelling used to produce.
BEZEL_TEST(format_spec_shared_rendering_7311,
           a_malformed_suffix_raises_format_exception_2_18) {
    expectFormatException("(255).ToString(\"D4x\")");
}

// ===========================================================================
// #7311 — the DISPATCH defect: the two call sites must AGREE.
// ===========================================================================

// The member-call path and the interpolation hole path render the SAME text
// for every specifier in the shared vocabulary. Before the shared routing
// they did not: the member path rendered the no-form text for `P`, and the
// interpolation path rendered the format form.
BEZEL_TEST(format_spec_shared_rendering_7311,
           the_member_and_the_interpolation_spelling_agree_2_18) {
    expectOutput(
        prog("    var d := 1.5;\n"
             "    Console.WriteLine(d.ToString(\"F2\"));\n"
             "    Console.WriteLine($\"{d:F2}\");\n"
             "    Console.WriteLine(d.ToString(\"P1\"));\n"
             "    Console.WriteLine($\"{d:P1}\");\n"
             "    Console.WriteLine(d.ToString(\"R\"));\n"
             "    Console.WriteLine($\"{d:R}\");\n"
             "    Console.WriteLine((255).ToString(\"D4\"));\n"
             "    Console.WriteLine($\"{(255):D4}\");\n"
             "    Console.WriteLine((255).ToString(\"X4\"));\n"
             "    Console.WriteLine($\"{(255):X4}\");\n"),
        "1.50\n1.50\n150.0%\n150.0%\n1.5\n1.5\n0255\n0255\n00FF\n00FF\n");
}

// The width is the specifier's own: `D4` pads to four digits.
BEZEL_TEST(format_spec_shared_rendering_7311,
           the_minimum_digit_count_pads_the_rendering_2_18) {
    expectOutput(
        prog("    Console.WriteLine((255).ToString(\"D4\"));\n"
             "    Console.WriteLine((255).ToString(\"D6\"));\n"
             "    Console.WriteLine((255).ToString(\"X4\"));\n"),
        "0255\n000255\n00FF\n");
}
