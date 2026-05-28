// Spec-derived lexer tests for numeric literals — §18.1 and grammar A.1.
//
// Decision recorded in the PR (CLAUDE.md §4): the lexer recognizes a literal
// token and spans its full lexeme; suffix interpretation (which integer or
// float type) and value-range checking (e.g. the spec's
// "3_000_000_000 without L is a compile error") are sema-level concerns and
// land with the type checker.

#include <string_view>

#include "support/lex_support.hpp"
#include "test_harness.hpp"

using bezel::TokenKind;
using bezel::test::expectTokens;
using bezel::test::lexAll;

// --- Decimal integer literals (§18.1, A.1 DecLiteral) -----------------------

BEZEL_TEST(lexer_int_literals, spec_18_1_simple_decimal) {
    // §18.1 example: `42` (int).
    expectTokens(lexAll("42"), {{TokenKind::IntLiteral, "42"}});
}

BEZEL_TEST(lexer_int_literals, decimal_zero) {
    expectTokens(lexAll("0"), {{TokenKind::IntLiteral, "0"}});
}

BEZEL_TEST(lexer_int_literals, spec_18_1_digit_separators) {
    // §18.1 example: `1_234_567` (digit separators).
    expectTokens(lexAll("1_234_567"),
                 {{TokenKind::IntLiteral, "1_234_567"}});
}

// --- Hex integer literals (§18.1, A.1 HexLiteral) ---------------------------

BEZEL_TEST(lexer_int_literals, spec_18_1_hex) {
    // §18.1 example: `0xFF`.
    expectTokens(lexAll("0xFF"), {{TokenKind::IntLiteral, "0xFF"}});
}

BEZEL_TEST(lexer_int_literals, hex_lowercase_digits) {
    // A.1 HexDigit: `0..9 | a..f | A..F`.
    expectTokens(lexAll("0xff"), {{TokenKind::IntLiteral, "0xff"}});
}

BEZEL_TEST(lexer_int_literals, hex_with_separators) {
    expectTokens(lexAll("0xCA_FE"),
                 {{TokenKind::IntLiteral, "0xCA_FE"}});
}

// --- Binary integer literals (§18.1, A.1 BinLiteral) ------------------------

BEZEL_TEST(lexer_int_literals, spec_18_1_binary) {
    // §18.1 example: `0b10101010`.
    expectTokens(lexAll("0b10101010"),
                 {{TokenKind::IntLiteral, "0b10101010"}});
}

BEZEL_TEST(lexer_int_literals, binary_with_separators) {
    expectTokens(lexAll("0b1010_1010"),
                 {{TokenKind::IntLiteral, "0b1010_1010"}});
}

// --- Integer suffixes (§18.1, A.1 IntSuffix = L | U | UL | LU) --------------

BEZEL_TEST(lexer_int_literals, spec_18_1_suffix_L) {
    // §18.1 example: `42L` (long).
    expectTokens(lexAll("42L"), {{TokenKind::IntLiteral, "42L"}});
}

BEZEL_TEST(lexer_int_literals, spec_18_1_suffix_U) {
    // §18.1 example: `42U` (uint).
    expectTokens(lexAll("42U"), {{TokenKind::IntLiteral, "42U"}});
}

BEZEL_TEST(lexer_int_literals, spec_18_1_suffix_UL) {
    // §18.1 example: `42UL` (ulong).
    expectTokens(lexAll("42UL"), {{TokenKind::IntLiteral, "42UL"}});
}

BEZEL_TEST(lexer_int_literals, suffix_LU_per_A1) {
    // A.1 IntSuffix includes `LU` alongside `UL`.
    expectTokens(lexAll("42LU"), {{TokenKind::IntLiteral, "42LU"}});
}

// §18.1: "Suffixes are case-insensitive: L/l, U/u, F/f, M/m."
BEZEL_TEST(lexer_int_literals, spec_18_1_int_suffixes_case_insensitive) {
    expectTokens(lexAll("42l"),  {{TokenKind::IntLiteral, "42l"}});
    expectTokens(lexAll("42u"),  {{TokenKind::IntLiteral, "42u"}});
    expectTokens(lexAll("42ul"), {{TokenKind::IntLiteral, "42ul"}});
    expectTokens(lexAll("42lu"), {{TokenKind::IntLiteral, "42lu"}});
}

// §18.1: "3_000_000_000 is a compile error without an L suffix." The lexer
// recognizes the token; value-range checking is sema's job (deferred). This
// test asserts the lexer's part.
BEZEL_TEST(lexer_int_literals,
           spec_18_1_overflow_lexes_as_int_sema_checks_range) {
    expectTokens(lexAll("3_000_000_000"),
                 {{TokenKind::IntLiteral, "3_000_000_000"}});
}

// --- Float and decimal literals (§18.1, A.1 FloatLiteral) -------------------

BEZEL_TEST(lexer_float_literals, spec_18_1_simple_double) {
    // §18.1 example: `3.14` (double) — A.1 form 1 (DigitGroup "." DigitGroup).
    expectTokens(lexAll("3.14"), {{TokenKind::FloatLiteral, "3.14"}});
}

BEZEL_TEST(lexer_float_literals, spec_18_1_float_suffix) {
    // §18.1 example: `3.14f` (float).
    expectTokens(lexAll("3.14f"), {{TokenKind::FloatLiteral, "3.14f"}});
}

BEZEL_TEST(lexer_float_literals, spec_18_1_scientific_double) {
    // §18.1 example: `-1.23e-4`. Leading `-` is unary minus (§17.1 level 2);
    // the exponent's `-4` IS part of the literal per A.1 Exponent.
    expectTokens(lexAll("1.23e-4"),
                 {{TokenKind::FloatLiteral, "1.23e-4"}});
}

BEZEL_TEST(lexer_float_literals, exponent_positive_sign) {
    expectTokens(lexAll("1.23e+4"),
                 {{TokenKind::FloatLiteral, "1.23e+4"}});
}

BEZEL_TEST(lexer_float_literals, exponent_capital_E_per_A1) {
    // A.1 Exponent: ("e" | "E").
    expectTokens(lexAll("1.23E4"),
                 {{TokenKind::FloatLiteral, "1.23E4"}});
}

BEZEL_TEST(lexer_float_literals, no_dot_with_exponent_per_A1_form2) {
    // A.1 FloatLiteral form 2: DigitGroup Exponent — no dot.
    expectTokens(lexAll("1e5"), {{TokenKind::FloatLiteral, "1e5"}});
}

BEZEL_TEST(lexer_float_literals, no_dot_with_suffix_per_A1_form3) {
    // A.1 FloatLiteral form 3: DigitGroup FloatSuffix — no dot, no exponent.
    expectTokens(lexAll("42f"), {{TokenKind::FloatLiteral, "42f"}});
    expectTokens(lexAll("42m"), {{TokenKind::FloatLiteral, "42m"}});
}

BEZEL_TEST(lexer_float_literals, spec_18_1_decimal_suffix) {
    // §18.1 example: `1.23456789m` (decimal).
    expectTokens(lexAll("1.23456789m"),
                 {{TokenKind::FloatLiteral, "1.23456789m"}});
}

BEZEL_TEST(lexer_float_literals, spec_18_1_scientific_decimal) {
    // §18.1 example: `1.23e2m`.
    expectTokens(lexAll("1.23e2m"),
                 {{TokenKind::FloatLiteral, "1.23e2m"}});
}

// §18.1: "Suffixes are case-insensitive."
BEZEL_TEST(lexer_float_literals, spec_18_1_float_suffixes_case_insensitive) {
    expectTokens(lexAll("42F"), {{TokenKind::FloatLiteral, "42F"}});
    expectTokens(lexAll("42M"), {{TokenKind::FloatLiteral, "42M"}});
}

// --- Disambiguation: dot, range, and float ----------------------------------
// A.1 FloatLiteral form 1 requires DigitGroup *on both sides* of the dot, so
// `1.` followed by a non-digit is NOT a float. The lexer needs one character
// of lookahead past the dot to decide.

BEZEL_TEST(lexer_numeric_disambiguation, range_after_int_is_not_float) {
    // `1..10` is `1`, range `..`, `10` — not a float of any kind.
    expectTokens(lexAll("1..10"),
                 {{TokenKind::IntLiteral, "1"},
                  {TokenKind::Punctuator, ".."},
                  {TokenKind::IntLiteral, "10"}});
}

BEZEL_TEST(lexer_numeric_disambiguation, range_to_identifier) {
    expectTokens(lexAll("0..n"),
                 {{TokenKind::IntLiteral, "0"},
                  {TokenKind::Punctuator, ".."},
                  {TokenKind::Identifier, "n"}});
}

BEZEL_TEST(lexer_numeric_disambiguation,
           range_exclusive_with_int_bounds_per_17_1) {
    // §17.1 level 6 range operator `..<`. Verifies the int-`..<`-int chain
    // does not get swallowed into a float.
    expectTokens(lexAll("0..<10"),
                 {{TokenKind::IntLiteral, "0"},
                  {TokenKind::Punctuator, "..<"},
                  {TokenKind::IntLiteral, "10"}});
}

// --- Integration with identifiers, keywords, and punctuators ----------------

BEZEL_TEST(lexer_numeric_integration, var_assignment_int) {
    expectTokens(lexAll("var x := 42;"),
                 {{TokenKind::Keyword,    "var"},
                  {TokenKind::Identifier, "x"},
                  {TokenKind::Punctuator, ":="},
                  {TokenKind::IntLiteral, "42"},
                  {TokenKind::Punctuator, ";"}});
}

BEZEL_TEST(lexer_numeric_integration,
           spec_18_1_negative_scientific_double) {
    // §18.1 example as-written: `-1.23e-4`.
    expectTokens(lexAll("-1.23e-4"),
                 {{TokenKind::Punctuator,    "-"},
                  {TokenKind::FloatLiteral,  "1.23e-4"}});
}
