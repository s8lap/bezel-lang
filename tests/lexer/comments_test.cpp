// Spec-derived lexer tests for comments — §1.5 (Comments) and §34.
//
// Comments are trivia (§1.2: "Whitespace and comments separate tokens but are
// not themselves tokens"); the lexer skips them. Documentation-comment (`///`)
// preservation and attachment to the following declaration (§1.5, §34) is
// deferred to the parser milestone — this slice only checks recognition.

#include "support/lex_support.hpp"
#include "test_harness.hpp"

using bezel::TokenKind;
using bezel::test::expectTokens;
using bezel::test::lexAll;

// §1.5: "// ..." line comment extends to end of line.
BEZEL_TEST(lexer_comments, spec_1_5_line_comment_is_trivia) {
    expectTokens(lexAll("foo // a comment\nbar"),
                 {{TokenKind::Identifier, "foo"},
                  {TokenKind::Identifier, "bar"}});
}

// §1.5: "/* ... */" block comment.
BEZEL_TEST(lexer_comments, spec_1_5_block_comment_is_trivia) {
    expectTokens(lexAll("foo /* a comment */ bar"),
                 {{TokenKind::Identifier, "foo"},
                  {TokenKind::Identifier, "bar"}});
}

// §1.5 / §34: block comments "do not nest" — the first */ closes the comment,
// so the trailing `done` is ordinary source. If comments nested, `done` would
// be swallowed and only EndOfFile would remain.
BEZEL_TEST(lexer_comments, spec_1_5_block_comment_does_not_nest) {
    expectTokens(lexAll("/* outer /* inner */ done"),
                 {{TokenKind::Identifier, "done"}});
}

// §1.5 / §34: "/// ..." documentation comment is trivia for tokenization
// (attachment deferred).
BEZEL_TEST(lexer_comments, spec_34_doc_comment_is_trivia) {
    expectTokens(lexAll("/// <summary>doc</summary>\nfoo"),
                 {{TokenKind::Identifier, "foo"}});
}
