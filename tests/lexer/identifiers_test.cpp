// Spec-derived lexer tests for identifiers — §1.3 (Identifiers).
//
// Tests-first per CLAUDE.md: these derive from the spec and are expected to
// fail until the lexer is implemented.

#include <string_view>

#include "support/lex_support.hpp"
#include "test_harness.hpp"

using bezel::TokenKind;
using bezel::test::expectTokens;
using bezel::test::lexAll;

// §1.3: an identifier begins with a letter or underscore and continues with
// letters, digits, or underscores.
BEZEL_TEST(lexer_identifiers, spec_1_3_simple) {
    expectTokens(lexAll("foo"), {{TokenKind::Identifier, "foo"}});
}

BEZEL_TEST(lexer_identifiers, spec_1_3_underscore_start) {
    expectTokens(lexAll("_foo"), {{TokenKind::Identifier, "_foo"}});
}

BEZEL_TEST(lexer_identifiers, spec_1_3_letters_digits_underscores) {
    expectTokens(lexAll("foo_bar2"), {{TokenKind::Identifier, "foo_bar2"}});
}

// §1.3: "Foo, foo, and FOO are distinct."
BEZEL_TEST(lexer_identifiers, spec_1_3_case_sensitive) {
    expectTokens(lexAll("Foo foo FOO"),
                 {{TokenKind::Identifier, "Foo"},
                  {TokenKind::Identifier, "foo"},
                  {TokenKind::Identifier, "FOO"}});
}

// §1.3 / A.1 / B.1 (v0.244, proposal 0083; #3049): "The bare underscore `_` is
// **not** an identifier: it is a reserved token … never declarable and never
// readable as a name." A.1 makes the exclusion GRAMMATICAL by splitting the
// production — `Identifier ::= Letter IdentCont* | "_" IdentCont+` — so the
// one-character lexeme matches neither alternative.
//
// (This test asserted `{Identifier, "_"}` up to v0.243, quoting §1.3's then-
// current "the bare identifier _ is a valid identifier name everywhere it
// appears as a name". That sentence is gone; the rule it encoded is what
// proposal 0083 retired.)
BEZEL_TEST(lexer_identifiers, spec_1_3_bare_underscore_is_reserved_token) {
    expectTokens(lexAll("_"), {{TokenKind::Discard, "_"}});
}

// A.1's boundary, from the other side: the `"_" IdentCont+` alternative needs
// at least ONE continuation character, and everything that satisfies it stays
// an ordinary Identifier. §1.3: "Identifiers merely *containing* underscores
// (`_camelCase`, `snake_case`, `_f`) are ordinary identifiers; only the bare
// single-character token is reserved."
BEZEL_TEST(lexer_identifiers, spec_1_3_underscore_prefixed_stay_identifiers) {
    expectTokens(lexAll("_f"), {{TokenKind::Identifier, "_f"}});
    expectTokens(lexAll("_1"), {{TokenKind::Identifier, "_1"}});
    expectTokens(lexAll("__"), {{TokenKind::Identifier, "__"}});
    expectTokens(lexAll("_camelCase"), {{TokenKind::Identifier, "_camelCase"}});
    expectTokens(lexAll("snake_case"), {{TokenKind::Identifier, "snake_case"}});
}

// Two bare underscores are two reserved tokens, not one identifier — the
// maximal-munch scan stops at the first non-IdentCont, and whitespace is
// trivia (§1.2). Pins that the split is per-lexeme, not per-run.
BEZEL_TEST(lexer_identifiers, spec_1_3_two_bare_underscores_are_two_tokens) {
    expectTokens(lexAll("_ _"),
                 {{TokenKind::Discard, "_"}, {TokenKind::Discard, "_"}});
}

// The discard is a token, not a keyword: `_x` is NOT `_` followed by `x`.
// (`lambda(_, x)` vs a parameter named `_x` is exactly this boundary.)
BEZEL_TEST(lexer_identifiers, spec_1_3_underscore_then_letter_is_one_token) {
    expectTokens(lexAll("_x, _"), {{TokenKind::Identifier, "_x"},
                                   {TokenKind::Punctuator, ","},
                                   {TokenKind::Discard, "_"}});
}

// §1.3 / A.1: an identifier may begin with "any Unicode letter"; source is
// UTF-8 (§1.1). λ is U+03BB.
BEZEL_TEST(lexer_identifiers, spec_1_3_unicode_letter) {
    expectTokens(lexAll("λ"), {{TokenKind::Identifier, "λ"}});
}

// §1.3: `string` is a reserved keyword (Appendix B) producing a primitive-type
// token; `String` is an ordinary PascalCase identifier. The lexer treats them
// as different tokens.
BEZEL_TEST(lexer_identifiers, spec_1_3_string_keyword_vs_String_identifier) {
    expectTokens(lexAll("string String"),
                 {{TokenKind::Keyword, "string"},
                  {TokenKind::Identifier, "String"}});
}
