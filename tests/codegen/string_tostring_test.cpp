// CG27c: `ToString()` intrinsic on numeric primitives (§5, stdlib §9.x).
// Each primitive type provides a `ToString(): string` method that returns
// the decimal (for ints) or default (for floats/bool) string representation.
// These are compiler intrinsics — there is no user-visible declaration in
// user code, but the compiler emits them using snprintf under the hood.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"
#include "support/aot_support.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

// §5: int.ToString() → decimal representation, then .Length.
BEZEL_TEST(codegen_cg27c, int_tostring_length) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var n := 42; var s := n.ToString(); return s.Length; "
        "end Main; end C;"), 2);
}

// §5: zero int.
BEZEL_TEST(codegen_cg27c, int_zero_tostring_length) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var n := 0; var s := n.ToString(); return s.Length; "
        "end Main; end C;"), 1);
}

// §5: negative int.
BEZEL_TEST(codegen_cg27c, int_negative_tostring_length) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var n := 0 - 5; var s := n.ToString(); return s.Length; "
        "end Main; end C;"), 2);  // "-5" has length 2
}

// §5: int.ToString() equality — "42" = "42".
BEZEL_TEST(codegen_cg27c, int_tostring_equality) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var n := 42; var s := n.ToString(); "
        "  return (1 if s = \"42\" else 0); "
        "end Main; end C;"), 1);
}

// §5 / stdlib bool.ToString() → "True"/"False": true.ToString() → "True", Length = 4.
// (Was incorrectly "true" — fixed per #452: stdlib specifies capital "True"/"False".)
BEZEL_TEST(codegen_cg27c, bool_true_tostring_length) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var b := true; var s := b.ToString(); return s.Length; "
        "end Main; end C;"), 4);
}

// §5 / stdlib bool.ToString() → "True"/"False": false.ToString() → "False", Length = 5.
// (Was incorrectly "false" — fixed per #452: stdlib specifies capital "True"/"False".)
BEZEL_TEST(codegen_cg27c, bool_false_tostring_length) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var b := false; var s := b.ToString(); return s.Length; "
        "end Main; end C;"), 5);
}

// §5 / stdlib bool.ToString() → "True": equality assertion.
// Per stdlib: `public method ToString(): string;  // "True" or "False"` (#452).
BEZEL_TEST(codegen_cg27c, bool_true_tostring_equality) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var b := true; var s := b.ToString(); "
        "  return (1 if s = \"True\" else 0); "
        "end Main; end C;"), 1);
}

// §5 / stdlib bool.ToString() → "False": equality assertion (#452).
BEZEL_TEST(codegen_cg27c, bool_false_tostring_equality) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var b := false; var s := b.ToString(); "
        "  return (1 if s = \"False\" else 0); "
        "end Main; end C;"), 1);
}

// §18.5 / stdlib bool.ToString(): string interpolation $"{true}" → "True" (#452).
// Interpolation routes through emitPrimitiveToString, same bool→string path.
BEZEL_TEST(codegen_cg27c, bool_true_interpolation_capitalized) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var b := true; var s := $\"{b}\"; "
        "  return (1 if s = \"True\" else 0); "
        "end Main; end C;"), 1);
}

// §18.5 / stdlib bool.ToString(): string interpolation $"{false}" → "False" (#452).
BEZEL_TEST(codegen_cg27c, bool_false_interpolation_capitalized) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var b := false; var s := $\"{b}\"; "
        "  return (1 if s = \"False\" else 0); "
        "end Main; end C;"), 1);
}

// §5: long.ToString() — large number.
BEZEL_TEST(codegen_cg27c, long_tostring_length) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var n: long := 1000000L; var s := n.ToString(); return s.Length; "
        "end Main; end C;"), 7);  // "1000000" = 7 chars
}

// string itself has a ToString() that returns itself — length unchanged.
BEZEL_TEST(codegen_cg27c, string_tostring_identity) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"hi\"; return (1 if s.ToString() = \"hi\" else 0); "
        "end Main; end C;"), 1);
}

// Concatenation of int.ToString() with a string literal.
BEZEL_TEST(codegen_cg27c, int_tostring_concat) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var n := 7; var s := \"x=\" + n.ToString(); return s.Length; "
        "end Main; end C;"), 3);  // "x=" (2) + "7" (1) = "x=7" (3)
}

}  // namespace
