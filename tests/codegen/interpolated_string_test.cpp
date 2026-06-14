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

}  // namespace
