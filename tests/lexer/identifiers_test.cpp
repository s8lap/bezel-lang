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

// §1.3: "The bare identifier _ is a valid identifier name everywhere it appears
// as a name." Its discard meaning (§17.13) is positional and handled later.
BEZEL_TEST(lexer_identifiers, spec_1_3_bare_underscore) {
    expectTokens(lexAll("_"), {{TokenKind::Identifier, "_"}});
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
