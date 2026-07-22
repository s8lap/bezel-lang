// CG27a: string type — literals, Length, equality, concatenation (§4.6,
// stdlib §9.6). String is a reference type with content-based equality and
// runtime-managed lifetime (no dispose). This slice implements:
//   - StringLiteralExpr and VerbatimStringExpr lowering to global %bz_string
//   - `.Length` property → int (number of code points)
//   - `=` and `<>` intrinsic operators → content comparison via memcmp
//   - `+` operator → malloc-backed concatenation

#include "support/codegen_support.hpp"
#include "test_harness.hpp"
#include "support/aot_support.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

// §4.6: a string literal evaluates to a string value whose Length equals
// the number of characters in the literal.
BEZEL_TEST(codegen_cg27a, string_literal_length) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"Hello\"; return s.Length; "
        "end Main; end C;"), 5);
}

// §4.6: an empty string literal has Length 0.
BEZEL_TEST(codegen_cg27a, empty_string_length) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"\"; return s.Length; "
        "end Main; end C;"), 0);
}

// §4.6: two string literals with the same content compare equal (=).
BEZEL_TEST(codegen_cg27a, string_equality_true) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a := \"abc\"; var b := \"abc\"; "
        "  return (if a = b then 1 else 0); "
        "end Main; end C;"), 1);
}

// §4.6: two string literals with different content compare unequal (<>).
BEZEL_TEST(codegen_cg27a, string_inequality_different) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a := \"abc\"; var b := \"xyz\"; "
        "  return (if a <> b then 1 else 0); "
        "end Main; end C;"), 1);
}

// §4.6: strings of different length are unequal (quick-path: lengths differ).
BEZEL_TEST(codegen_cg27a, string_equality_different_length) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a := \"hi\"; var b := \"hello\"; "
        "  return (if a = b then 1 else 0); "
        "end Main; end C;"), 0);
}

// §4.6: `+` concatenates two strings; the result's Length is the sum of
// the two operand lengths.
BEZEL_TEST(codegen_cg27a, string_concat_length) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a := \"Hello\"; var b := \" World\"; "
        "  var c := a + b; return c.Length; "
        "end Main; end C;"), 11);
}

// §4.6: a concatenated string compares equal to a literal with the same
// content — confirms that concat produces correct content, not just length.
BEZEL_TEST(codegen_cg27a, string_concat_equality) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a := \"He\" + \"llo\"; "
        "  var b := \"Hello\"; "
        "  return (if a = b then 1 else 0); "
        "end Main; end C;"), 1);
}

// §4.6: escape sequence `\n` is a newline (one character), so "A\nB"
// has Length 3.
BEZEL_TEST(codegen_cg27a, string_escape_newline_length) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"A\\nB\"; return s.Length; "
        "end Main; end C;"), 3);
}

// §4.6, §18.6: verbatim string (@"...") suppresses escape processing;
// `@"A\nB"` contains 4 characters: A, \, n, B.
BEZEL_TEST(codegen_cg27a, verbatim_string_length) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := @\"A\\nB\"; return s.Length; "
        "end Main; end C;"), 4);
}

// §4.6: string comparison uses content equality, not reference identity;
// two separately constructed strings with the same content are equal.
BEZEL_TEST(codegen_cg27a, string_param_equality) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Check(a: string, b: string): int "
        "    return (if a = b then 1 else 0); "
        "  end Check; "
        "  public static method Main(): int "
        "    return Check(\"xyz\", \"xyz\"); "
        "  end Main; "
        "end C;"), 1);
}

// §4.6: a null check on a nullable string — not-null string is not null.
BEZEL_TEST(codegen_cg27a, string_literal_nonnull) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s: string? := \"hi\"; "
        "  return (if s <> null then 1 else 0); "
        "end Main; end C;"), 1);
}

// §4.6: multi-concatenation — (a + b) + c.
BEZEL_TEST(codegen_cg27a, string_multi_concat) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var r := \"ab\" + \"cd\" + \"ef\"; "
        "  return r.Length; "
        "end Main; end C;"), 6);
}

}  // namespace
