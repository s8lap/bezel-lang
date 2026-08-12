// Spec-derived lexer tests for keywords — §1.4 (Keywords) and Appendix B
// (Reserved Words).
//
// Design choice recorded in the PR (CLAUDE.md §4): contextual keywords (§B.2)
// are lexed as Identifier tokens; the parser distinguishes them by position.
// The spec says they "may also appear as identifiers in other positions" and
// that "the compiler distinguishes by context," which keeps the lexer
// context-free.

#include <string_view>

#include "support/lex_support.hpp"
#include "test_harness.hpp"

using bezel::TokenKind;
using bezel::test::expectTokens;
using bezel::test::lexAll;

namespace {

// Appendix B.1 — the unconditionally reserved keywords (verbatim, in spec
// order). `true`, `false`, and `null` are reserved words here; their literal
// role (§18.2) is a parser-level concern.
constexpr std::string_view kReservedKeywords[] = {
    "abstract",  "and",     "as",        "async",     "await",
    "base",      "bool",    "break",     "byte",      "case",
    "catch",     "char",    "choice",    "class",     "closed",
    "const",
    "constructor", "continue", "decimal", "default",  "destructor",
    "do",        "double",  "elif",      "else",      "end",
    "enum",
    "event",     "extension", "extern",  "false",     "finally",
    "float",     "for",     "if",        "import",    "in",
    "int",       "interface", "internal", "is",       "lambda",
    "long",
    "method",    "mod",     "module",    "move",      "nameof",
    "new",       "not",     "null",      "or",        "out",
    "owned",     "override", "private",  "property",  "protected",
    "public",    "readonly", "record",   "ref",       "return",
    "sbyte",     "sealed",  "short",     "static",    "string",
    "then",      "this",    "throw",     "true",      "try",
    "type",      "typeof",  "uint",      "ulong",     "ushort",
    "using",     "var",     "virtual",   "when",      "where",
    "while",     "with",    "yield",
};

// Appendix B.2 — contextual keywords: reserved only in specific positions and
// usable as identifiers elsewhere.
constexpr std::string_view kContextualKeywords[] = {
    "convention", "get", "notnull", "operator",
    "serialized", "set", "step",    "value",
};

} // namespace

// §1.4 / B.1: each unconditionally reserved word lexes as a single Keyword
// token whose lexeme is the word.
BEZEL_TEST(lexer_keywords, spec_B_1_reserved_lex_as_keyword) {
    for (std::string_view keyword : kReservedKeywords) {
        expectTokens(lexAll(keyword), {{TokenKind::Keyword, keyword}});
    }
}

// §1.4: "All keywords are lowercase." A capitalized spelling is an ordinary
// identifier, not a keyword.
BEZEL_TEST(lexer_keywords, spec_1_4_keywords_are_lowercase) {
    expectTokens(lexAll("Class"), {{TokenKind::Identifier, "Class"}});
    expectTokens(lexAll("If"), {{TokenKind::Identifier, "If"}});
}

// §1.4: PascalCase type names (Object, Array, Exception, …) are not keywords.
BEZEL_TEST(lexer_keywords, spec_1_4_pascalcase_types_not_keywords) {
    expectTokens(lexAll("Object Array Exception Task"),
                 {{TokenKind::Identifier, "Object"},
                  {TokenKind::Identifier, "Array"},
                  {TokenKind::Identifier, "Exception"},
                  {TokenKind::Identifier, "Task"}});
}

// §B.2: contextual keywords are lexed as identifiers (see header note).
BEZEL_TEST(lexer_keywords, spec_B_2_contextual_lex_as_identifier) {
    for (std::string_view keyword : kContextualKeywords) {
        expectTokens(lexAll(keyword), {{TokenKind::Identifier, keyword}});
    }
}
