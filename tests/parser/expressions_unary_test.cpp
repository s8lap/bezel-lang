// Spec-derived parser tests for unary prefix expressions — §17.1 level 2.
// Slice 9 covers `+`, `-`, `~` (punctuator) and `not` (keyword). `await`
// (also level 2) lands with the async slice.

#include <string>

#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::memberSummaries;
using bezel::test::parseAll;

static std::string parseInitText(const std::string& expr) {
    auto r = parseAll(std::string("module M; class Foo public x: int := ") +
                      expr + std::string("; end Foo;"));
    if (r.diagnostics.hasErrors()) return "<error>";
    const auto ms = memberSummaries(r);
    if (ms.size() != 1) return "<no member>";
    const std::string prefix = "public x: int := ";
    if (ms[0].rfind(prefix, 0) != 0) return "<bad prefix>";
    return ms[0].substr(prefix.size());
}

// --- Basic unary forms -----------------------------------------------------

BEZEL_TEST(parser_unary_expr, unary_minus) {
    BEZEL_EXPECT_EQ(parseInitText("-x"), std::string("(-x)"));
}

BEZEL_TEST(parser_unary_expr, unary_plus) {
    BEZEL_EXPECT_EQ(parseInitText("+x"), std::string("(+x)"));
}

BEZEL_TEST(parser_unary_expr, bitwise_complement) {
    BEZEL_EXPECT_EQ(parseInitText("~bits"), std::string("(~bits)"));
}

// §6.5: `not` is the boolean negation keyword. Reserved per App. B.1.
BEZEL_TEST(parser_unary_expr, not_keyword) {
    BEZEL_EXPECT_EQ(parseInitText("not flag"), std::string("(not flag)"));
}

// --- Right associativity ---------------------------------------------------

BEZEL_TEST(parser_unary_expr, double_negation_is_right_associative) {
    // The lexer treats `--` as one token (decrement, statement-only per
    // §17.6), so double unary minus in expression position must be written
    // with a separator. `- -x` lexes as two `-` Punctuators.
    BEZEL_EXPECT_EQ(parseInitText("- -x"), std::string("(-(-x))"));
}

BEZEL_TEST(parser_unary_expr, mixed_unary_chain) {
    // `+ -x` rather than `+-x` for the same maximal-munch reason as above
    // (lexer doesn't combine `+-`, but separation is the cleaner convention).
    BEZEL_EXPECT_EQ(parseInitText("- +x"), std::string("(-(+x))"));
}

BEZEL_TEST(parser_unary_expr, not_not_x) {
    BEZEL_EXPECT_EQ(parseInitText("not not flag"),
                    std::string("(not (not flag))"));
}

// --- Precedence relative to binary operators -------------------------------
// §17.1: unary is level 2 (tighter than every binary level).

BEZEL_TEST(parser_unary_expr, unary_tighter_than_additive) {
    BEZEL_EXPECT_EQ(parseInitText("-a + b"), std::string("((-a) + b)"));
    BEZEL_EXPECT_EQ(parseInitText("a + -b"), std::string("(a + (-b))"));
}

BEZEL_TEST(parser_unary_expr, unary_tighter_than_multiplicative) {
    BEZEL_EXPECT_EQ(parseInitText("-a * -b"),
                    std::string("((-a) * (-b))"));
}

BEZEL_TEST(parser_unary_expr, not_tighter_than_logical) {
    BEZEL_EXPECT_EQ(parseInitText("not a and b"),
                    std::string("((not a) and b)"));
}

// --- Unary on literals and call results ------------------------------------

BEZEL_TEST(parser_unary_expr, negative_literal_is_unary_on_literal) {
    // The lexer emits IntLiteral "42"; the leading `-` is the unary operator.
    BEZEL_EXPECT_EQ(parseInitText("-42"), std::string("(-42)"));
}

BEZEL_TEST(parser_unary_expr, unary_on_call_result) {
    BEZEL_EXPECT_EQ(parseInitText("-f(a)"), std::string("(-f(a))"));
}

BEZEL_TEST(parser_unary_expr, unary_on_member_access) {
    BEZEL_EXPECT_EQ(parseInitText("-obj.field"),
                    std::string("(-obj.field)"));
}

// --- Parens around unary expressions ---------------------------------------

BEZEL_TEST(parser_unary_expr, parens_change_binding) {
    // `-(a + b)` negates the sum; `-a + b` negates only `a`.
    BEZEL_EXPECT_EQ(parseInitText("-(a + b)"), std::string("(-(a + b))"));
    BEZEL_EXPECT_EQ(parseInitText("-a + b"),    std::string("((-a) + b)"));
}
