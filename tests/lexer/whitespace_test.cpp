// Spec-derived lexer tests for whitespace, line endings, and token separation
// — §1.1 (Source files) and §1.2 (Tokens).

#include "support/lex_support.hpp"
#include "test_harness.hpp"

using bezel::TokenKind;
using bezel::test::expectTokens;
using bezel::test::lexAll;

// §1.2: whitespace separates tokens but is not itself a token.
BEZEL_TEST(lexer_whitespace, spec_1_2_whitespace_separates_tokens) {
    expectTokens(lexAll("a   b\t c"),
                 {{TokenKind::Identifier, "a"},
                  {TokenKind::Identifier, "b"},
                  {TokenKind::Identifier, "c"}});
}

// §1.1: "Line endings are LF, CR, or CRLF." Each separates tokens identically.
BEZEL_TEST(lexer_whitespace, spec_1_1_line_ending_lf) {
    expectTokens(lexAll("a\nb"),
                 {{TokenKind::Identifier, "a"}, {TokenKind::Identifier, "b"}});
}

BEZEL_TEST(lexer_whitespace, spec_1_1_line_ending_cr) {
    expectTokens(lexAll("a\rb"),
                 {{TokenKind::Identifier, "a"}, {TokenKind::Identifier, "b"}});
}

BEZEL_TEST(lexer_whitespace, spec_1_1_line_ending_crlf) {
    expectTokens(lexAll("a\r\nb"),
                 {{TokenKind::Identifier, "a"}, {TokenKind::Identifier, "b"}});
}

// §1.2: empty source yields no tokens beyond the terminal EndOfFile.
BEZEL_TEST(lexer_whitespace, spec_1_2_empty_source) {
    expectTokens(lexAll(""), {});
}
