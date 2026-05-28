// Spec-derived lexer tests for character literals — §18.3 and the applicable
// escape sequences from §18.8. Surrogate escapes (U+D800–U+DFFF) are
// compile errors per both sections.

#include <string_view>

#include "support/lex_support.hpp"
#include "test_harness.hpp"

using bezel::TokenKind;
using bezel::test::expectLexError;
using bezel::test::expectTokens;
using bezel::test::lexAll;

// §18.3 example: a single ASCII character.
BEZEL_TEST(lexer_char_literals, spec_18_3_simple_ascii) {
    expectTokens(lexAll("'a'"), {{TokenKind::CharLiteral, "'a'"}});
}

// §18.3 / §18.8: each named single-letter escape is recognized inside a char
// literal. Data-driven against the §18.8 escape table (excluding the unicode
// and hex forms, which have their own tests).
BEZEL_TEST(lexer_char_literals, spec_18_8_named_escapes) {
    constexpr std::string_view kCharsWithNamedEscape[] = {
        "'\\n'", "'\\r'", "'\\t'", "'\\\\'", "'\\\"'", "'\\''",
        "'\\0'", "'\\a'", "'\\b'", "'\\f'", "'\\v'",
    };
    for (std::string_view src : kCharsWithNamedEscape) {
        expectTokens(lexAll(src), {{TokenKind::CharLiteral, src}});
    }
}

// §18.3 example: `A` — four hex digits, BMP only.
BEZEL_TEST(lexer_char_literals, spec_18_3_unicode_4hex) {
    expectTokens(lexAll("'\\u0041'"),
                 {{TokenKind::CharLiteral, "'\\u0041'"}});
}

// §18.3 example: `\U0001F600` — eight hex digits, full code-point range.
BEZEL_TEST(lexer_char_literals, spec_18_3_unicode_8hex) {
    expectTokens(lexAll("'\\U0001F600'"),
                 {{TokenKind::CharLiteral, "'\\U0001F600'"}});
}

// §18.3 / §18.8 example: `\x41` — hex escape, 1–4 hex digits.
BEZEL_TEST(lexer_char_literals, spec_18_3_hex_escape) {
    expectTokens(lexAll("'\\x41'"),
                 {{TokenKind::CharLiteral, "'\\x41'"}});
}

// §18.3 / §18.8: escape sequences resolving to U+D800–U+DFFF (surrogates) are
// compile errors. Test all three escape forms.
BEZEL_TEST(lexer_char_literals, spec_18_3_surrogate_via_u_is_error) {
    expectLexError("'\\uD800'");
}

BEZEL_TEST(lexer_char_literals, spec_18_3_surrogate_via_U_is_error) {
    expectLexError("'\\U0000DFFF'");
}

BEZEL_TEST(lexer_char_literals, spec_18_3_surrogate_via_x_is_error) {
    // §18.8 \x is greedy (up to 4 hex digits). \xDFFF is U+DFFF, a surrogate.
    expectLexError("'\\xDFFF'");
}

// Unterminated char literal — the spec doesn't enumerate this case explicitly
// but A.1's `CharLiteral ::= "'" CharContent "'"` requires the closing quote.
BEZEL_TEST(lexer_char_literals, unterminated_is_error) {
    expectLexError("'a");
}
