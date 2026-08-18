// Regression for #3109 narrow re-attempt (v0.256).
//
// The over-broad fix (de3cd71, reverted in ba9ea9b) rejected `char` on either
// side of `mod`, which broke `codegen_string_length_528.japanese_index_0_mod256`
// and `codegen_string_length_528.emoji_index_0_mod256` (`s[0] mod 256` is the
// canonical code-point-to-byte cast pattern used for exit codes). The narrow
// predicate rejects ONLY `char mod char` (both sides char); the cast pattern
// `char mod <int>` and `<int> mod char` remain admitted via §5.1's char→int
// widening. The 2 #528 tests must continue to pass.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

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

// ── MUST-PASS: the 2 Wave 2 false-reject cases (re-pinned here as the
//    canonical cast pattern). These are the *exact* bodies of the existing
//    codegen_string_length_528 tests; they must remain admitted. ───────────

BEZEL_TEST(binary_mod_char_char_rejected_3109_narrow, char_mod_int_literal_japanese_cast) {
    BEZEL_EXPECT_EQ(
        run("var s := \"\xe6\x97\xa5\xe6\x9c\xac\"; return s[0] mod 256;"),
        26085 % 256);
}

BEZEL_TEST(binary_mod_char_char_rejected_3109_narrow, char_mod_int_literal_emoji_cast) {
    BEZEL_EXPECT_EQ(
        run("var s := \"\xf0\x9f\x98\x80\"; return s[0] mod 256;"),
        128512 % 256);
}

// ── Cast pattern controls (non-literal RHS) ────────────────────────────────

BEZEL_TEST(binary_mod_char_char_rejected_3109_narrow, char_mod_int_variable_admitted) {
    BEZEL_EXPECT_EQ(
        run("var c: char := char(26085); var n: int := 256; return c mod n;"),
        26085 % 256);
}

BEZEL_TEST(binary_mod_char_char_rejected_3109_narrow, int_variable_mod_char_admitted) {
    BEZEL_EXPECT_EQ(
        run("var n: int := 47; var c: char := char(10); return n mod c;"),
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

BEZEL_TEST(binary_mod_char_char_rejected_3109_narrow, char_compound_mod_assign_int_rejected) {
    BEZEL_EXPECT_TRUE(compileErrored(
        "  var c: char := 'a'; "
        "  c mod= 2; "
        "  return int(c);"));
}

BEZEL_TEST(binary_mod_char_char_rejected_3109_narrow, char_compound_mod_assign_char_rejected) {
    BEZEL_EXPECT_TRUE(compileErrored(
        "  var c: char := 'a'; "
        "  var d: char := 'b'; "
        "  c mod= d; "
        "  return int(c);"));
}

BEZEL_TEST(binary_mod_char_char_rejected_3109_narrow, int_compound_mod_assign_char_admitted) {
    BEZEL_EXPECT_EQ(
        run("var c: char := char(10); var n: int := 47; n mod= c; return n;"),
        47 % 10);
}
