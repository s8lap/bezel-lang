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

// §18.5: bool expression part.
BEZEL_TEST(codegen_cg27d, bool_expression_part) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var b := true; "
        "  var s := $\"ok={b}\"; "
        "  return (1 if s = \"ok=true\" else 0); "
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

}  // namespace
