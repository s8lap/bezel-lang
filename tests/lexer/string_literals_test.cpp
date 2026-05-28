// Spec-derived lexer tests for string literals — §18.4 and the applicable
// escape sequences from §18.8. Interpolated strings (§18.5) and interpolated
// verbatim strings (§18.7) are deferred until the parser exists; verbatim
// strings (§18.6) live in their own test file.

#include <string_view>

#include "support/lex_support.hpp"
#include "test_harness.hpp"

using bezel::TokenKind;
using bezel::test::expectLexError;
using bezel::test::expectTokens;
using bezel::test::lexAll;

// §18.4 example: a plain string.
BEZEL_TEST(lexer_string_literals, spec_18_4_simple) {
    expectTokens(lexAll("\"Hello, Bezel!\""),
                 {{TokenKind::StringLiteral, "\"Hello, Bezel!\""}});
}

// §18.4 example: escape sequences inside a string are part of the lexeme; the
// decoded value is sema's job.
BEZEL_TEST(lexer_string_literals, spec_18_4_with_newline_escape) {
    expectTokens(lexAll("\"Line 1\\nLine 2\""),
                 {{TokenKind::StringLiteral, "\"Line 1\\nLine 2\""}});
}

BEZEL_TEST(lexer_string_literals, empty_string) {
    expectTokens(lexAll("\"\""), {{TokenKind::StringLiteral, "\"\""}});
}

// §18.8 escape table applies to strings. Spot-check the embedded-quote and
// backslash escapes that the simple cases don't already cover.
BEZEL_TEST(lexer_string_literals, spec_18_8_embedded_escaped_quote) {
    // Source:  "say \"hi\""
    expectTokens(lexAll("\"say \\\"hi\\\"\""),
                 {{TokenKind::StringLiteral, "\"say \\\"hi\\\"\""}});
}

BEZEL_TEST(lexer_string_literals, spec_18_8_embedded_backslash) {
    // Source:  "a\\b"  (one escaped backslash between the two letters)
    expectTokens(lexAll("\"a\\\\b\""),
                 {{TokenKind::StringLiteral, "\"a\\\\b\""}});
}

// §18.8 unicode and hex escapes inside strings.
BEZEL_TEST(lexer_string_literals, spec_18_8_unicode_4hex) {
    expectTokens(lexAll("\"\\u0041\""),
                 {{TokenKind::StringLiteral, "\"\\u0041\""}});
}

BEZEL_TEST(lexer_string_literals, spec_18_8_unicode_8hex) {
    expectTokens(lexAll("\"\\U0001F600\""),
                 {{TokenKind::StringLiteral, "\"\\U0001F600\""}});
}

// §18.8: "The \x escape greedily consumes the maximum number of valid
// hexadecimal digits, up to 4." For the lexer level, this means `\x41BC` is
// one escape, not `\x41` + `B` + `C` — but the resulting LEXEME is the same
// either way (the lexer terminates at the closing `"` either way). The
// greedy interpretation matters for value decoding (sema) AND for surrogate
// detection: see spec_18_8_x_escape_surrogate_via_greedy below.
BEZEL_TEST(lexer_string_literals, spec_18_8_x_escape_greedy_lexeme) {
    expectTokens(lexAll("\"\\x41BC\""),
                 {{TokenKind::StringLiteral, "\"\\x41BC\""}});
}

// Surrogate-via-greedy-`\x`: `\xD800` decodes to U+D800 only when `\x` is
// parsed greedily. This is the lexer-level test that exercises the greedy
// rule (combined with the surrogate ban).
BEZEL_TEST(lexer_string_literals, spec_18_8_x_escape_surrogate_via_greedy) {
    expectLexError("\"\\xD800\"");
}

// §18.8: surrogate via `\u` and `\U` are also errors.
BEZEL_TEST(lexer_string_literals, spec_18_8_surrogate_via_u_is_error) {
    expectLexError("\"\\uD800\"");
}

BEZEL_TEST(lexer_string_literals, spec_18_8_surrogate_via_U_is_error) {
    expectLexError("\"\\U0000DFFF\"");
}

// Unterminated string — A.1 `StringLiteral ::= '"' StringContent* '"'`
// requires the closing quote.
BEZEL_TEST(lexer_string_literals, unterminated_is_error) {
    expectLexError("\"hello");
}
