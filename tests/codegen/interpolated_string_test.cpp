// CG27d: interpolated strings `$"..."` (§18.5).
// The parser splits the token into alternating literal and expression parts.
// Expression parts are typechecked by sema (identifier resolution etc.) and
// lowered by codegen via emitPrimitiveToString + string concatenation.
//
// Spec: §18.5 ("A `$` prefix introduces interpolation. Any expression may
// appear inside `{}`.")

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& src) {
    auto r = runJit(src);
    return r.ran ? r.exitCode : -999;
}

// §18.5: literal-only interpolated string — no expressions; content equals
// the literal, same length as the string between the quotes.
BEZEL_TEST(codegen_cg27d, literal_only_length) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := $\"hello\"; return s.Length; "
        "end Main; end C;"), 5);
}

// §18.5: an empty interpolated string has Length 0.
BEZEL_TEST(codegen_cg27d, empty_string_length) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := $\"\"; return s.Length; "
        "end Main; end C;"), 0);
}

// §18.5: a single int expression `{n}` converts to its decimal repr.
BEZEL_TEST(codegen_cg27d, int_expression_length) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var n := 42; var s := $\"n={n}\"; return s.Length; "
        "end Main; end C;"), 4);  // "n=42" has length 4
}

// §18.5: interpolation result equals the expected string.
BEZEL_TEST(codegen_cg27d, int_expression_equality) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var n := 7; var s := $\"x={n}\"; "
        "  return (1 if s = \"x=7\" else 0); "
        "end Main; end C;"), 1);
}

// §18.5: multiple expression parts.
BEZEL_TEST(codegen_cg27d, multiple_expressions) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a := 1; var b := 2; "
        "  var s := $\"{a}+{b}={a+b}\"; "
        "  return (1 if s = \"1+2=3\" else 0); "
        "end Main; end C;"), 1);
}

// §18.5: string expression part is interpolated directly.
BEZEL_TEST(codegen_cg27d, string_expression_part) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var name := \"World\"; "
        "  var s := $\"Hello, {name}!\"; "
        "  return (1 if s = \"Hello, World!\" else 0); "
        "end Main; end C;"), 1);
}

// §18.5: bool expression part — per stdlib bool.ToString() → "True"/"False"
// (#452: was "ok=true"; fixed to "ok=True" because interpolation routes through
// emitPrimitiveToString which now emits "True"/"False" per the stdlib contract).
BEZEL_TEST(codegen_cg27d, bool_expression_part) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var b := true; "
        "  var s := $\"ok={b}\"; "
        "  return (1 if s = \"ok=True\" else 0); "
        "end Main; end C;"), 1);
}

// §18.5: expression-only (no literal prefix or suffix).
BEZEL_TEST(codegen_cg27d, expression_only) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var n := 99; "
        "  var s := $\"{n}\"; "
        "  return (1 if s = \"99\" else 0); "
        "end Main; end C;"), 1);
}

// §18.5: concat (via +) of two interpolated strings.
BEZEL_TEST(codegen_cg27d, interpolated_concat) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var x := 3; var y := 4; "
        "  var s := $\"({x},{y})\"; "
        "  return s.Length; "
        "end Main; end C;"), 5);  // "(3,4)" has length 5
}

// R35 / §18 / §8.3: interpolating a NULL `string?` renders the literal "null"
// (the same rendering nullable value types use), NOT a SIGSEGV from
// dereferencing the null. Previously crashed (the `string?` part fell into the
// primitive-ToString path on a null pointer).
BEZEL_TEST(codegen_cg27d, null_nullable_string_renders_null_r35) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s: string? := null; "
        "  var result := $\"val={s}\"; "
        "  return (1 if result = \"val=null\" else 0); "
        "end Main; end C;"), 1);
}

// R35: a non-null `string?` interpolates its value normally.
BEZEL_TEST(codegen_cg27d, nonnull_nullable_string_renders_value_r35) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s: string? := \"hi\"; "
        "  var r := $\"v={s}\"; "
        "  return (1 if r = \"v=hi\" else 0); "
        "end Main; end C;"), 1);
}

// R35: interpolating a NULL nullable CLASS reference also renders "null"
// (was a SIGSEGV from calling ToString on null). "x=null" has length 6.
BEZEL_TEST(codegen_cg27d, null_nullable_class_renders_null_r35) {
    BEZEL_EXPECT_EQ(run(
        "module M; class Simple end Simple; "
        "class C public static method Main(): int "
        "  var x: Simple? := null; "
        "  var s := $\"x={x}\"; "
        "  return s.Length; "
        "end Main; end C;"), 6);
}

// R36 / §18.7 / #733: a verbatim interpolated string `$@"..."` keeps escapes
// LITERAL (the @ was previously ignored, so `\t`/`\n` were decoded). Verbatim
// `$@"a\tb"` must equal plain `@"a\tb"` and differ from interpolated `$"a\tb"`.
BEZEL_TEST(codegen_cg27d, verbatim_interpolated_keeps_escapes_literal_r36) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var v := $@\"a\\tb\"; "
        "  var plain := @\"a\\tb\"; "
        "  var interp := $\"a\\tb\"; "
        "  var ok := 0; "
        "  if v = plain then ok := ok + 1; end; "
        "  if v <> interp then ok := ok + 1; end; "
        "  return ok; "  // expect 2
        "end Main; end C;"), 2);
}

// R36 / §18.7: the interpolation hole still substitutes inside a verbatim string.
BEZEL_TEST(codegen_cg27d, verbatim_interpolated_hole_still_substitutes_r36) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var n := 7; "
        "  var s := $@\"x{n}y\"; "
        "  return (1 if s = \"x7y\" else 0); "
        "end Main; end C;"), 1);
}

// ── §18.5 format specifiers (issue #739, WR9/WR10/WR11) ──────────────────────

// WR9 / §18.5 normative example: `{ratio:P1}` → "85.0%" (multiply by 100,
// fixed-point to the digit count, append '%'). Previously the P specifier was
// silently dropped and the raw value printed.
BEZEL_TEST(codegen_cg27d, percent_format_p1_float_739) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var ratio: float := 0.85f; var s := $\"{ratio:P1}\"; "
        "  return (1 if s = \"85.0%\" else 0); "
        "end Main; end C;"), 1);
}

BEZEL_TEST(codegen_cg27d, percent_format_p1_double_739) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var r: double := 0.85; var s := $\"{r:P1}\"; "
        "  return (1 if s = \"85.0%\" else 0); "
        "end Main; end C;"), 1);
}

// Bare `P` defaults to two fractional digits: 0.5 → "50.00%".
BEZEL_TEST(codegen_cg27d, percent_format_default_digits_739) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var r: double := 0.5; var s := $\"{r:P}\"; "
        "  return (1 if s = \"50.00%\" else 0); "
        "end Main; end C;"), 1);
}

// WR10 / §18.5: an uppercase `E` specifier yields an uppercase exponent letter,
// lowercase `e` yields lowercase — the specifier's case is honoured (mirroring
// X/x). Previously both produced lowercase `e`.
BEZEL_TEST(codegen_cg27d, scientific_format_case_honoured_739) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var v: float := 1.23f; "
        "  var up := $\"{v:E2}\"; var lo := $\"{v:e2}\"; "
        "  return (1 if (up.Contains(\"E\") and lo.Contains(\"e\")) else 0); "
        "end Main; end C;"), 1);
}

// WR11 / §18.5: a fixed-point specifier applies to an int operand — `{42:F2}`
// → "42.00" (length 5). Previously F was dropped for int, yielding "42".
BEZEL_TEST(codegen_cg27d, fixed_point_format_on_int_739) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var n: int := 42; var s := $\"{n:F2}\"; "
        "  return s.Length; "  // "42.00" → 5
        "end Main; end C;"), 5);
}

BEZEL_TEST(codegen_cg27d, fixed_point_format_on_int_value_739) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var n: int := 42; var s := $\"{n:F2}\"; "
        "  return (1 if s = \"42.00\" else 0); "
        "end Main; end C;"), 1);
}

// Regression: a fixed-point specifier on a double is unchanged — `{1234.5:F2}`
// → "1234.50" — and the alignment+format combination still pads correctly.
BEZEL_TEST(codegen_cg27d, fixed_point_format_double_unchanged_739) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var amount: double := 1234.5; "
        "  var f := $\"{amount:F2}\"; "
        "  var aligned := $\"|{amount,12:F2}|\"; "
        "  var ok := (f = \"1234.50\") and (aligned = \"|     1234.50|\"); "
        "  return (1 if ok else 0); "
        "end Main; end C;"), 1);
}

// #1186 / §18.5: an alignment width wider than the old fixed 64-byte stack
// render buffer must render the full, correct padding (a positive width
// right-pads with spaces == PadLeft) and never over-read the buffer.  Before
// the fix this memcpy'd snprintf's untruncated length out of a 64-byte alloca,
// leaking uninitialized stack bytes (sub-crash) or SIGSEGV-ing at huge widths.
BEZEL_TEST(codegen_cg27d, interp_wide_alignment_no_overread_1186) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var x := 5; "
        "  var s := $\"{x,200}\"; "
        "  var expected := x.ToString().PadLeft(200); "
        "  var ok := (s.Length = 200) and (s = expected); "
        "  return (1 if ok else 0); "
        "end Main; end C;"), 1);
}

// #1186 / §18.5: a numeric format spec (D100 → 100-digit zero-padded) that
// renders far wider than the old 64-byte buffer must render correctly and
// in-bounds.  All format specs (D/F/E/G/P/X) funnel through the same helper.
BEZEL_TEST(codegen_cg27d, interp_wide_format_spec_no_overread_1186) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var n := 5; "
        "  var s := $\"{n:D100}\"; "
        "  var expected := n.ToString().PadLeft(100, '0'); "
        "  var ok := (s.Length = 100) and (s = expected); "
        "  return (1 if ok else 0); "
        "end Main; end C;"), 1);
}

// #1283 / §18.5 + §8.5: an alignment specifier `{x,N}` on a NON-primitive
// operand pads that operand's default ToString — it must NOT change the
// formatter. Previously an enum with alignment rendered its underlying integer
// (Green→"1", not the §8.5 type name) and a class/record/interface with
// alignment dropped the padding entirely (the operand produced no aligned
// string). The fix routes any Named operand through emitValueToString before
// padding.

// #1283 Defect A: an enum + alignment renders the §8.5 type name, padded — not
// the underlying integer. (Default enum ToString is the type name "Color".)
BEZEL_TEST(codegen_cg27d, enum_alignment_renders_type_name_1283) {
    BEZEL_EXPECT_EQ(run(
        "module M; enum Color Red, Green, Blue; end Color; "
        "class C public static method Main(): int "
        "  var col := Color.Green; "
        "  var s := $\"[{col,8}]\"; "        // "[   Color]" (5-char name in width 8)
        "  return (1 if s = \"[   Color]\" else 0); "
        "end Main; end C;"), 1);
}

// #1283 Defect A (left-align): a negative width left-aligns the enum name.
BEZEL_TEST(codegen_cg27d, enum_alignment_left_1283) {
    BEZEL_EXPECT_EQ(run(
        "module M; enum Color Red, Green, Blue; end Color; "
        "class C public static method Main(): int "
        "  var col := Color.Green; "
        "  var s := $\"[{col,-8}]\"; "       // "[Color   ]"
        "  return (1 if s = \"[Color   ]\" else 0); "
        "end Main; end C;"), 1);
}

// #1283 Defect B: a record + alignment right-aligns its structural ToString
// ("Point(x: 1, y: 2)" = 17 chars) in the field width — padding is applied,
// not dropped.
BEZEL_TEST(codegen_cg27d, record_alignment_padded_1283) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Point(x: int, y: int); "
        "class C public static method Main(): int "
        "  var p := new Point(1, 2); "
        "  var s := $\"[{p,22}]\"; "         // 5 leading spaces + 17-char ToString
        "  return (1 if s = \"[     Point(x: 1, y: 2)]\" else 0); "
        "end Main; end C;"), 1);
}

// #1283 Defect B (class ToString override + alignment): the override's result
// ("WW") is padded, not dropped.
BEZEL_TEST(codegen_cg27d, class_override_alignment_padded_1283) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class W public method ToString(): string return \"WW\"; end ToString; end W; "
        "class C public static method Main(): int "
        "  using var w := new W(); "
        "  var s := $\"[{w,8}]\"; "          // "[      WW]" (6 spaces + "WW")
        "  return (1 if s = \"[      WW]\" else 0); "
        "end Main; end C;"), 1);
}

}  // namespace
