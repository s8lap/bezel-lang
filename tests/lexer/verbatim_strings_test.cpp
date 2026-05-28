// Spec-derived lexer tests for verbatim string literals — §18.6.
//
// Two defining differences from §18.4 strings: (a) backslashes are NOT escape
// introducers — `@"C:\foo\bar"` contains four literal characters between the
// quotes; and (b) the only "escape" is `""`, which represents a single `"`
// inside the literal. Verbatim strings may also span multiple lines.

#include <string_view>

#include "support/lex_support.hpp"
#include "test_harness.hpp"

using bezel::TokenKind;
using bezel::test::expectLexError;
using bezel::test::expectTokens;
using bezel::test::lexAll;

// §18.6 example: a simple verbatim string.
BEZEL_TEST(lexer_verbatim_strings, spec_18_6_simple) {
    expectTokens(lexAll("@\"path\""),
                 {{TokenKind::VerbatimStringLiteral, "@\"path\""}});
}

// §18.6 example: backslashes are NOT escape introducers in verbatim strings.
// The lexer must not interpret `\` as starting an escape; the literal is the
// raw source between the quotes.
BEZEL_TEST(lexer_verbatim_strings, spec_18_6_backslashes_are_literal) {
    expectTokens(lexAll("@\"C:\\Users\\Bezel\\Program.bezel\""),
                 {{TokenKind::VerbatimStringLiteral,
                   "@\"C:\\Users\\Bezel\\Program.bezel\""}});
}

// §18.6: verbatim strings may span multiple lines; line breaks become part of
// the value (and therefore the lexeme).
BEZEL_TEST(lexer_verbatim_strings, spec_18_6_multiline) {
    constexpr std::string_view src =
        "@\"SELECT name, age\nFROM users\nWHERE active = 1\"";
    expectTokens(lexAll(src), {{TokenKind::VerbatimStringLiteral, src}});
}

// §18.6: inside a verbatim string, `""` produces a single `"`.  The pair of
// quote characters does NOT terminate the literal.
BEZEL_TEST(lexer_verbatim_strings, spec_18_6_double_quote_is_escape_for_quote) {
    // Source:  @"say ""hi"""
    expectTokens(lexAll("@\"say \"\"hi\"\"\""),
                 {{TokenKind::VerbatimStringLiteral, "@\"say \"\"hi\"\"\""}});
}

BEZEL_TEST(lexer_verbatim_strings, empty_verbatim) {
    expectTokens(lexAll("@\"\""),
                 {{TokenKind::VerbatimStringLiteral, "@\"\""}});
}

// Unterminated verbatim string — A.1 `StringLiteral ::= … | '@"' VerbatimContent* '"'`
// requires the closing quote (and that closing quote must not be doubled).
BEZEL_TEST(lexer_verbatim_strings, unterminated_is_error) {
    expectLexError("@\"hello");
}
