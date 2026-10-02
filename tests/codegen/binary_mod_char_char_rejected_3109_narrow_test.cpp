// NOTE (fix #5150, §3.5, v0.309): the "cast pattern" admissions described below
// were FLIPPED to rejections -- see the `fix #5150` section further down.
// Regression for #3109 narrow re-attempt (v0.256).
//
// The over-broad fix (de3cd71, reverted in ba9ea9b) rejected `char` on either
// side of `mod`, which broke `codegen_string_length_528.japanese_index_0_mod256`
// and `codegen_string_length_528.emoji_index_0_mod256` (`s[0] mod 256` is the
// canonical code-point-to-byte cast pattern used for exit codes). The narrow
// predicate rejects ONLY `char mod char` (both sides char); the cast pattern
// `char mod <int>` and `<int> mod char` remain admitted via §5.1's char→int
// widening. The 2 #528 tests must continue to pass.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

int run(const std::string& body) {
    auto r = runAot(
        std::string("module M; class C public static method Main(): int ") +
        body + " end Main; end C;");
    return r.ran ? r.exitCode : -999;
}

bool compileErrored(const std::string& body) {
    return runAot(
        std::string("module M; class C public static method Main(): int ") +
        body + " end Main; end C;").diagnostics.hasErrors();
}

// fix #5150 (§3.5/§6.1/§6.6, v0.309): `mod` is listed on the integer types and
// `decimal` only and the operand lists name types BEFORE §5.1 conversion, so
// a `char` operand on either side of `mod` (and `n mod= c`) is a compile
// error. Asserts the location AND the diagnostic needle.
void expectRejected(const std::string& body, const char* needle) {
    auto r = runAot(
        std::string("module M; class C public static method Main(): int ") +
        body + " end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, needle));
}

constexpr const char* kModChar = "`mod` is not defined on `char` operands";
constexpr const char* kCharOperand = "is not defined for `char` operands";

}  // namespace

// ── NEW: char mod char rejected ────────────────────────────────────────────

BEZEL_TEST(binary_mod_char_char_rejected_3109_narrow, char_mod_char_rejected) {
    BEZEL_EXPECT_TRUE(compileErrored(
        "  var c: char := 'a'; "
        "  var d: char := 'b'; "
        "  return int(c mod d);"));
}

BEZEL_TEST(binary_mod_char_char_rejected_3109_narrow, char_mod_char_rejected_symmetric) {
    BEZEL_EXPECT_TRUE(compileErrored(
        "  var c: char := 'a'; "
        "  var d: char := 'b'; "
        "  return int(d mod c);"));
}

// ── fix #5150 (§3.5, v0.309) FLIPPED: the v0.256 "cast pattern" `s[0] mod 256`
//    admitted `char mod <int>` / `<int> mod char` via §5.1's char->int
//    widening. §3.5's operand lists name types before §5.1 conversion and
//    `mod` lists the integer types and `decimal`, so each is now a compile
//    error; the arithmetic is `int(c) mod n`. Each reject row is paired with
//    its `int(...)`-spelled accept control. ──────────────────────────────

BEZEL_TEST(binary_mod_char_char_rejected_3109_narrow, char_mod_int_literal_japanese_cast_rejected) {
    expectRejected(
        "var s := \"\xe6\x97\xa5\xe6\x9c\xac\"; return s[0] mod 256;", kModChar);
}

BEZEL_TEST(binary_mod_char_char_rejected_3109_narrow, char_mod_int_literal_emoji_cast_rejected) {
    expectRejected(
        "var s := \"\xf0\x9f\x98\x80\"; return s[0] mod 256;", kModChar);
}

BEZEL_TEST(binary_mod_char_char_rejected_3109_narrow, int_cast_mod_int_literal_japanese_admitted) {
    BEZEL_EXPECT_EQ(
        run("var s := \"\xe6\x97\xa5\xe6\x9c\xac\"; return int(s[0]) mod 256;"),
        26085 % 256);
}

BEZEL_TEST(binary_mod_char_char_rejected_3109_narrow, int_cast_mod_int_literal_emoji_admitted) {
    BEZEL_EXPECT_EQ(
        run("var s := \"\xf0\x9f\x98\x80\"; return int(s[0]) mod 256;"),
        128512 % 256);
}

BEZEL_TEST(binary_mod_char_char_rejected_3109_narrow, char_mod_int_variable_rejected) {
    expectRejected(
        "var c: char := char(26085); var n: int := 256; return c mod n;", kModChar);
}

BEZEL_TEST(binary_mod_char_char_rejected_3109_narrow, int_variable_mod_char_rejected) {
    expectRejected(
        "var n: int := 47; var c: char := char(10); return n mod c;", kModChar);
}

BEZEL_TEST(binary_mod_char_char_rejected_3109_narrow, int_cast_mod_variable_admitted) {
    BEZEL_EXPECT_EQ(
        run("var c: char := char(26085); var n: int := 256; return int(c) mod n;"),
        26085 % 256);
}

BEZEL_TEST(binary_mod_char_char_rejected_3109_narrow, int_variable_mod_int_cast_admitted) {
    BEZEL_EXPECT_EQ(
        run("var n: int := 47; var c: char := char(10); return n mod int(c);"),
        47 % 10);
}

// ── Existing rejections stay rejected (broad-vs-narrow controls) ──────────

BEZEL_TEST(binary_mod_char_char_rejected_3109_narrow, float_mod_int_rejected) {
    BEZEL_EXPECT_TRUE(compileErrored(
        "  var f: float := 47.0f; "
        "  return int(f mod 10.0f);"));
}

BEZEL_TEST(binary_mod_char_char_rejected_3109_narrow, double_mod_int_rejected) {
    BEZEL_EXPECT_TRUE(compileErrored(
        "  var d: double := 47.5; "
        "  return int(d mod 10.0);"));
}

BEZEL_TEST(binary_mod_char_char_rejected_3109_narrow, char_mod_float_rejected) {
    BEZEL_EXPECT_TRUE(compileErrored(
        "  var c: char := 'a'; "
        "  return int(c mod 2.0f);"));
}

// ── Existing acceptances stay accepted (broad-vs-narrow controls) ──────────

BEZEL_TEST(binary_mod_char_char_rejected_3109_narrow, int_mod_int_still_admitted) {
    BEZEL_EXPECT_EQ(run("var i: int := 47; var j: int := 10; return i mod j;"), 7);
}

BEZEL_TEST(binary_mod_char_char_rejected_3109_narrow, decimal_mod_decimal_still_admitted) {
    BEZEL_EXPECT_EQ(
        runAotOut(
            "module M; class C public static method Main(): int "
            "  var d: decimal := 47.5m; "
            "  var e: decimal := 10m; "
            "  Console.WriteLine(d mod e); "
            "  return 0; "
            "end Main; end C;"),
        "7.5");
}

// ── Compound `mod=` form is unchanged (control) ────────────────────────────

// §6.6 (v0.309): `c mod= n` on a `char` target is a compile error (ground
// moved from the #3109 binary rule to §6.6); strengthened with the location
// and a needle (still rejected, not flipped).
BEZEL_TEST(binary_mod_char_char_rejected_3109_narrow, char_compound_mod_assign_int_rejected) {
    expectRejected(
        "  var c: char := 'a'; "
        "  c mod= 2; "
        "  return int(c);", "`mod=` is not valid on `char`");
}

BEZEL_TEST(binary_mod_char_char_rejected_3109_narrow, char_compound_mod_assign_char_rejected) {
    expectRejected(
        "  var c: char := 'a'; "
        "  var d: char := 'b'; "
        "  c mod= d; "
        "  return int(c);", "`mod=` is not valid on `char`");
}

// fix #5150 (§3.5/§6.6, v0.309) FLIPPED: `n mod= c` (integer target, `char`
// right operand) desugars to `n := n mod c`, a `char` operand of `mod`.
BEZEL_TEST(binary_mod_char_char_rejected_3109_narrow, int_compound_mod_assign_char_rejected) {
    expectRejected(
        "var c: char := char(10); var n: int := 47; n mod= c; return n;", kCharOperand);
}

BEZEL_TEST(binary_mod_char_char_rejected_3109_narrow, int_compound_mod_assign_int_cast_admitted) {
    BEZEL_EXPECT_EQ(
        run("var c: char := char(10); var n: int := 47; n mod= int(c); return n;"),
        47 % 10);
}
