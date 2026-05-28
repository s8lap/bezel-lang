// Lexer source-location tests. The spec does not pin a line/column numbering
// convention; this slice establishes one (documented in lex_support.hpp's
// expectLocation helper and in Lexer::locationAt): line and column are 1-based,
// each of CR / LF / CRLF advances the line by one, and tabs advance the column
// by one (no expansion). These tests pin that convention.

#include <cstdint>

#include "support/lex_support.hpp"
#include "test_harness.hpp"

using bezel::TokenKind;
using bezel::test::expectLocation;
using bezel::test::lexAll;

BEZEL_TEST(lexer_locations, first_token_at_line_1_column_1) {
    auto tokens = lexAll("foo");
    expectLocation(tokens[0], 1, 1);
}

BEZEL_TEST(lexer_locations, column_advances_through_leading_spaces) {
    auto tokens = lexAll("  foo");
    expectLocation(tokens[0], 1, 3);
}

BEZEL_TEST(lexer_locations, lf_starts_new_line_column_1) {
    auto tokens = lexAll("a\nb");
    expectLocation(tokens[0], 1, 1);
    expectLocation(tokens[1], 2, 1);
}

BEZEL_TEST(lexer_locations, cr_starts_new_line_column_1) {
    auto tokens = lexAll("a\rb");
    expectLocation(tokens[1], 2, 1);
}

// CRLF is one line break, not two — the next line is 2, not 3.
BEZEL_TEST(lexer_locations, crlf_is_one_line_break) {
    auto tokens = lexAll("a\r\nb");
    expectLocation(tokens[1], 2, 1);
}

BEZEL_TEST(lexer_locations, multiple_tokens_on_one_line) {
    auto tokens = lexAll("var x");
    expectLocation(tokens[0], 1, 1); // var
    expectLocation(tokens[1], 1, 5); // x
}

BEZEL_TEST(lexer_locations, tab_advances_column_by_one) {
    auto tokens = lexAll("a\tb");
    expectLocation(tokens[1], 1, 3);
}

BEZEL_TEST(lexer_locations, token_after_block_comment) {
    auto tokens = lexAll("/* abc */ x");
    expectLocation(tokens[0], 1, 11); // x
}

BEZEL_TEST(lexer_locations, token_after_line_comment) {
    auto tokens = lexAll("// header\nfoo");
    expectLocation(tokens[0], 2, 1);
}

BEZEL_TEST(lexer_locations, multiline_string_terminator_token_after) {
    // The verbatim string spans multiple source lines; the token after it
    // should be on the line where the closing `"` sits.
    auto tokens = lexAll("@\"l1\nl2\nl3\";");
    expectLocation(tokens[0], 1, 1); // verbatim string
    expectLocation(tokens[1], 3, 4); // ';' — line 3, after `l3` + closing `"`
}

BEZEL_TEST(lexer_locations, eof_token_has_a_location) {
    auto tokens = lexAll("abc");
    expectLocation(tokens.back(), 1, 4); // EOF sits past `c`
}

BEZEL_TEST(lexer_locations, eof_after_newline) {
    auto tokens = lexAll("abc\n");
    expectLocation(tokens.back(), 2, 1);
}
