// Spec-derived lexer tests for operators and punctuation — §17 (Expressions
// and Operators) and the grammar's lexical layer (Appendix A).
//
// Token-model decision recorded in the PR: all operators and punctuation share
// a single `Punctuator` TokenKind, with the specific symbol carried in the
// token lexeme (`text`). Mirrors how keywords are handled in slice 1; a
// per-symbol enum can be introduced later if the parser needs faster dispatch.

#include <string_view>

#include "support/lex_support.hpp"
#include "test_harness.hpp"

using bezel::TokenKind;
using bezel::test::expectTokens;
using bezel::test::lexAll;

namespace {

// The complete punctuator set for this slice. Derived by extracting every
// quoted terminal from the grammar in Appendix A and removing keywords,
// identifier characters, and string/char-literal delimiters (`'` and `"` are
// deferred to the literal slices). Cross-checked against the §17.1 precedence
// table.
constexpr std::string_view kPunctuators[] = {
    // Grouping & indexing
    "(", ")", "[", "]", "{", "}",
    // Separators
    ",", ";", ":",
    // Member / range / null-conditional access
    ".", "..", "..<", "?.", "?(",
    // Unary
    "!", "~",
    // Increment / decrement (§17.6)
    "++", "--",
    // Arithmetic
    "+", "-", "*", "/",
    // Bitwise
    "&", "|", "^",
    // Shift
    "<<", ">>",
    // Relational / equality (§17.3 — `=` is equality, `<>` is not-equal)
    "<", ">", "<=", ">=", "=", "<>",
    // Null-coalescing
    "??",
    // Nullable-type suffix / standalone `?`
    "?",
    // Lambda / function-type arrows (§27)
    "=>", "->",
    // Assignment & compound assignment (§17.1 level 16)
    ":=", "+=", "-=", "*=", "/=", "&=", "|=", "^=", "<<=", ">>=", "??=",
};

} // namespace

// §17 / App. A: each punctuator listed in the grammar lexes as a single
// Punctuator token whose lexeme is the symbol.
BEZEL_TEST(lexer_punctuators, spec_17_each_punctuator_is_one_token) {
    for (std::string_view punct : kPunctuators) {
        expectTokens(lexAll(punct), {{TokenKind::Punctuator, punct}});
    }
}

// --- Maximal munch -----------------------------------------------------------
// When a prefix of a multi-char punctuator is itself a punctuator, the lexer
// takes the longest match.

BEZEL_TEST(lexer_punctuators, maximal_munch_walrus) {
    // `:=` is the assignment operator (§17.1 level 16), not `:` then `=`.
    expectTokens(lexAll(":="), {{TokenKind::Punctuator, ":="}});
}

BEZEL_TEST(lexer_punctuators, maximal_munch_range) {
    // `..` is the range operator; `..<` is the half-open variant (§17.1 level 6).
    expectTokens(lexAll(".."),  {{TokenKind::Punctuator, ".."}});
    expectTokens(lexAll("..<"), {{TokenKind::Punctuator, "..<"}});
}

BEZEL_TEST(lexer_punctuators, maximal_munch_shifts_and_compound) {
    expectTokens(lexAll("<<"),  {{TokenKind::Punctuator, "<<"}});
    expectTokens(lexAll(">>"),  {{TokenKind::Punctuator, ">>"}});
    expectTokens(lexAll("<<="), {{TokenKind::Punctuator, "<<="}});
    expectTokens(lexAll(">>="), {{TokenKind::Punctuator, ">>="}});
}

BEZEL_TEST(lexer_punctuators, maximal_munch_null_conditional) {
    // `?.`, `?(`, `??`, `??=` are single tokens; lone `?` falls through.
    expectTokens(lexAll("?."),  {{TokenKind::Punctuator, "?."}});
    expectTokens(lexAll("?("),  {{TokenKind::Punctuator, "?("}});
    expectTokens(lexAll("??"),  {{TokenKind::Punctuator, "??"}});
    expectTokens(lexAll("??="), {{TokenKind::Punctuator, "??="}});
}

BEZEL_TEST(lexer_punctuators, maximal_munch_arrows_and_inc_dec) {
    expectTokens(lexAll("->"),  {{TokenKind::Punctuator, "->"}});
    expectTokens(lexAll("=>"),  {{TokenKind::Punctuator, "=>"}});
    expectTokens(lexAll("++"),  {{TokenKind::Punctuator, "++"}});
    expectTokens(lexAll("--"),  {{TokenKind::Punctuator, "--"}});
}

BEZEL_TEST(lexer_punctuators, maximal_munch_relational_and_equality) {
    expectTokens(lexAll("<="), {{TokenKind::Punctuator, "<="}});
    expectTokens(lexAll(">="), {{TokenKind::Punctuator, ">="}});
    expectTokens(lexAll("<>"), {{TokenKind::Punctuator, "<>"}});
}

// --- Disambiguation against shorter forms ------------------------------------

// Lone `/` is a Punctuator, not the start of a comment.
BEZEL_TEST(lexer_punctuators, slash_alone_is_punctuator_not_comment) {
    expectTokens(lexAll("a / b"),
                 {{TokenKind::Identifier, "a"},
                  {TokenKind::Punctuator, "/"},
                  {TokenKind::Identifier, "b"}});
}

// Lone `?` (e.g. `T?` nullable-type suffix per §3 / §7) vs `?.`/`?(`/`??`.
BEZEL_TEST(lexer_punctuators, question_alone_is_punctuator) {
    expectTokens(lexAll("T? x"),
                 {{TokenKind::Identifier, "T"},
                  {TokenKind::Punctuator, "?"},
                  {TokenKind::Identifier, "x"}});
}

// Lone `.` (member access) vs `..` range.
BEZEL_TEST(lexer_punctuators, dot_alone_is_punctuator) {
    expectTokens(lexAll("obj.member"),
                 {{TokenKind::Identifier, "obj"},
                  {TokenKind::Punctuator, "."},
                  {TokenKind::Identifier, "member"}});
}

// --- Integration with identifiers and keywords -------------------------------

BEZEL_TEST(lexer_punctuators, integration_var_declaration) {
    // `var x := y;` — `var` keyword, `x` ident, `:=` punct, `y` ident, `;`.
    expectTokens(lexAll("var x := y;"),
                 {{TokenKind::Keyword,    "var"},
                  {TokenKind::Identifier, "x"},
                  {TokenKind::Punctuator, ":="},
                  {TokenKind::Identifier, "y"},
                  {TokenKind::Punctuator, ";"}});
}

BEZEL_TEST(lexer_punctuators, integration_call) {
    expectTokens(lexAll("f(a, b)"),
                 {{TokenKind::Identifier, "f"},
                  {TokenKind::Punctuator, "("},
                  {TokenKind::Identifier, "a"},
                  {TokenKind::Punctuator, ","},
                  {TokenKind::Identifier, "b"},
                  {TokenKind::Punctuator, ")"}});
}

BEZEL_TEST(lexer_punctuators, integration_null_conditional) {
    expectTokens(lexAll("obj?.m"),
                 {{TokenKind::Identifier, "obj"},
                  {TokenKind::Punctuator, "?."},
                  {TokenKind::Identifier, "m"}});
}

// §17.4: `is not` is a single relational operator with a two-token spelling.
// The lexer emits two Keyword tokens; the parser recognizes the pair.
BEZEL_TEST(lexer_punctuators, spec_17_4_is_not_is_two_keyword_tokens) {
    expectTokens(lexAll("x is not null"),
                 {{TokenKind::Identifier, "x"},
                  {TokenKind::Keyword,    "is"},
                  {TokenKind::Keyword,    "not"},
                  {TokenKind::Keyword,    "null"}});
}
