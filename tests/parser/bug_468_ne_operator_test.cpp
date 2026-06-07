// Spec-derived tests for bug #468: `!=` must be a parse error.
//
// §17.3: the inequality operator is `<>`. `!=` is NOT in the grammar
// (Appendix A). Previously, `a != b` was silently accepted and miscompiled
// because the parser parsed `!` as a postfix null-forgiving on `a`, then `=`
// as an equality binary operator — producing `(a!) = b` instead of an error.
//
// Fix (two parts):
//  1. Lexer: `!=` is now lexed as a single 2-character Punctuator token
//     (maximal munch, per the existing pattern for `<=`, `>=`, `:=`, etc.).
//     Bare `!` (not followed by `=`) still lexes as a 1-character Punctuator,
//     preserving `x!` and `x!.member`.
//  2. Parser: `parseBinaryExpression` recognises the `!=` Punctuator and
//     emits a helpful "use '<>'" diagnostic rather than miscompiling.
//
// These tests also guard the positive regressions that must keep working:
//   - `a <> b`        (§17.3 inequality)
//   - `x!`            (§17.8 postfix null-forgiving)
//   - `x!.member`     (§17.8 null-forgiving + member access chain)
//   - `x! <> y`       (null-forgiving then inequality)
//   - `not cond`       (§17.2 logical negation — keyword, not `!`)
//   - `x! = y!`       (null-forgiving on both operands, equality — valid)

#include <string>

#include "support/lex_support.hpp"
#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::TokenKind;
using bezel::test::expectTokens;
using bezel::test::lexAll;

using bezel::test::parseAll;
using bezel::test::memberSummaries;

// ---------------------------------------------------------------------------
// Lexer tests: token-level behaviour of `!` and `!=` — §17.3, §17.8, #468
// ---------------------------------------------------------------------------

// §17.8: bare `!` (not followed by `=`) is still a 1-char Punctuator.
// Preserves `x!`, `x!.member`, `x! <> y`, etc.
BEZEL_TEST(parser_bug_468, lexer_bare_bang_is_one_char_token) {
    // `x! = y` — `!` not followed by `=` directly (space between).
    // The `!` is a 1-char Punctuator; `=` is a separate Punctuator.
    auto toks = lexAll("x! = y");
    expectTokens(toks, {
        {TokenKind::Identifier, "x"},
        {TokenKind::Punctuator, "!"},
        {TokenKind::Punctuator, "="},
        {TokenKind::Identifier, "y"},
    });
}

// §17.3 / #468: `!=` (no space) is lexed as a single 2-char Punctuator.
// This lets the parser produce a targeted "use '<>'" diagnostic.
BEZEL_TEST(parser_bug_468, lexer_bang_eq_no_space_is_two_char_token) {
    auto toks = lexAll("a != b");
    // a, !=, b (3 real tokens + EOF)
    expectTokens(toks, {
        {TokenKind::Identifier, "a"},
        {TokenKind::Punctuator, "!="},
        {TokenKind::Identifier, "b"},
    });
}

// `x!=y` (no spaces at all) — also a single `!=` token.
BEZEL_TEST(parser_bug_468, lexer_bang_eq_nospace_is_two_char_token) {
    auto toks = lexAll("x!=y");
    expectTokens(toks, {
        {TokenKind::Identifier, "x"},
        {TokenKind::Punctuator, "!="},
        {TokenKind::Identifier, "y"},
    });
}

// ---------------------------------------------------------------------------
// Negative tests: `!=` must produce a parse error — §17.3, Appendix A
// ---------------------------------------------------------------------------

// §17.3 / Appendix A: `!=` is not an operator in Bezel; use `<>`.
BEZEL_TEST(parser_bug_468, bang_equal_in_if_condition_is_error) {
    // `var x := 5; if x != 3 then return 9; end; return 0;`
    // should be a parse error (not silently compile).
    auto r = parseAll(
        "module M;\n"
        "class Foo\n"
        "  public method Run(): int do\n"
        "    var x := 5;\n"
        "    if x != 3 then\n"
        "      return 9;\n"
        "    end;\n"
        "    return 0;\n"
        "  end\n"
        "end Foo;\n");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// Minimal form: `a != b` in expression position is an error.
BEZEL_TEST(parser_bug_468, bang_equal_in_expr_position_is_error) {
    auto r = parseAll(
        "module M; class Foo public x: bool := a != b; end Foo;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// `x != null` (the form that previously raised an unhandled exception
// at runtime per the bug report) must be a parse error.
BEZEL_TEST(parser_bug_468, bang_equal_null_is_error) {
    auto r = parseAll(
        "module M;\n"
        "class Foo\n"
        "  public method Run(): bool do\n"
        "    var x: int? := null;\n"
        "    return x != null;\n"
        "  end\n"
        "end Foo;\n");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// The diagnostic message should mention `<>` as the correct alternative
// (per the fix's requirement for a helpful message).
BEZEL_TEST(parser_bug_468, bang_equal_diagnostic_mentions_diamond) {
    auto r = parseAll(
        "module M; class Foo public x: bool := a != b; end Foo;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool mentionsDiamond = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("<>") != std::string::npos) {
            mentionsDiamond = true;
            break;
        }
    }
    BEZEL_EXPECT_TRUE(mentionsDiamond);
}

// ---------------------------------------------------------------------------
// Positive regression tests: these must still parse without errors
// ---------------------------------------------------------------------------

// §17.3: `<>` is the correct inequality operator and must keep working.
BEZEL_TEST(parser_bug_468, diamond_inequality_still_works) {
    auto r = parseAll(
        "module M; class Foo public x: bool := a <> b; end Foo;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const auto ms = memberSummaries(r);
    BEZEL_EXPECT_EQ(ms.size(), std::size_t(1));
    BEZEL_EXPECT_EQ(ms[0], std::string("public x: bool := (a <> b)"));
}

// §17.8: `x!` (null-forgiving postfix) must keep working.
BEZEL_TEST(parser_bug_468, postfix_bang_still_works) {
    auto r = parseAll(
        "module M; class Foo public x: int := value!; end Foo;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const auto ms = memberSummaries(r);
    BEZEL_EXPECT_EQ(ms.size(), std::size_t(1));
    BEZEL_EXPECT_EQ(ms[0], std::string("public x: int := value!"));
}

// §17.8: `x!.member` (null-forgiving chained with member access) must work.
BEZEL_TEST(parser_bug_468, postfix_bang_member_chain_still_works) {
    auto r = parseAll(
        "module M; class Foo public x: int := obj!.field; end Foo;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const auto ms = memberSummaries(r);
    BEZEL_EXPECT_EQ(ms.size(), std::size_t(1));
    BEZEL_EXPECT_EQ(ms[0], std::string("public x: int := obj!.field"));
}

// §17.8 + §17.3: `x! <> y` (null-forgiving then inequality) must work.
BEZEL_TEST(parser_bug_468, postfix_bang_followed_by_diamond_still_works) {
    auto r = parseAll(
        "module M; class Foo public x: bool := val! <> other; end Foo;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const auto ms = memberSummaries(r);
    BEZEL_EXPECT_EQ(ms.size(), std::size_t(1));
    BEZEL_EXPECT_EQ(ms[0], std::string("public x: bool := (val! <> other)"));
}

// §17.2: `not cond` (logical negation, keyword-spelled) must keep working.
BEZEL_TEST(parser_bug_468, not_keyword_still_works) {
    auto r = parseAll(
        "module M; class Foo public x: bool := not flag; end Foo;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const auto ms = memberSummaries(r);
    BEZEL_EXPECT_EQ(ms.size(), std::size_t(1));
    BEZEL_EXPECT_EQ(ms[0], std::string("public x: bool := (not flag)"));
}

// Regression: `x! = y!` (null-forgiving on both sides of equality) must still
// parse without error — this is NOT `!=`; the `!` tokens are separated from
// `=` by other tokens. The lexer should NOT produce a `!=` 2-char token here.
// This guards the kBoxAliasShared pattern from box_refcount_test.cpp.
BEZEL_TEST(parser_bug_468, bang_equality_bang_still_works) {
    // `x! = y!` — valid: null-forgiving x, equality, null-forgiving y.
    // The space between `x!` and `=` means the lexer sees `!` then (space) `=`,
    // NOT `!=`. Both `!` tokens are 1-char; `=` is a separate token.
    auto r = parseAll(
        "module M; class Foo public x: bool := a! = b!; end Foo;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const auto ms = memberSummaries(r);
    BEZEL_EXPECT_EQ(ms.size(), std::size_t(1));
    BEZEL_EXPECT_EQ(ms[0], std::string("public x: bool := (a! = b!)"));
}
