// Spec-derived parser tests for binary expressions — §17.1 precedence table.
// Slice 8 covers multiplicative (`*` `/` `mod`), additive (`+` `-`), relational
// (`<` `>` `<=` `>=`), equality (`=` `<>`), and logical (`and`, `or`). Shift,
// range, bitwise, null-coalescing, conditional, and assignment operators are
// in later slices.
//
// Tests use the existing field-initializer entry point. The exprToString
// helper renders binary expressions with explicit parentheses, so the test
// strings make precedence and associativity directly observable.

#include <string>
#include <vector>

#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::memberSummaries;
using bezel::test::parseAll;

// Helper: parse `public x: int := EXPR;` and return the initializer text.
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

// --- Basic binary operators -------------------------------------------------

BEZEL_TEST(parser_binary_expr, additive) {
    BEZEL_EXPECT_EQ(parseInitText("a + b"), std::string("(a + b)"));
    BEZEL_EXPECT_EQ(parseInitText("a - b"), std::string("(a - b)"));
}

BEZEL_TEST(parser_binary_expr, multiplicative) {
    BEZEL_EXPECT_EQ(parseInitText("a * b"), std::string("(a * b)"));
    BEZEL_EXPECT_EQ(parseInitText("a / b"), std::string("(a / b)"));
}

BEZEL_TEST(parser_binary_expr, mod_is_keyword_operator) {
    // `mod` is a reserved keyword (App. B.1), not a punctuator.
    BEZEL_EXPECT_EQ(parseInitText("a mod b"), std::string("(a mod b)"));
}

BEZEL_TEST(parser_binary_expr, relational) {
    BEZEL_EXPECT_EQ(parseInitText("a < b"),  std::string("(a < b)"));
    BEZEL_EXPECT_EQ(parseInitText("a > b"),  std::string("(a > b)"));
    BEZEL_EXPECT_EQ(parseInitText("a <= b"), std::string("(a <= b)"));
    BEZEL_EXPECT_EQ(parseInitText("a >= b"), std::string("(a >= b)"));
}

BEZEL_TEST(parser_binary_expr, equality_uses_equals_and_diamond) {
    // §17.3: `=` is equality and `<>` is not-equal (NOT `==` and `!=`).
    BEZEL_EXPECT_EQ(parseInitText("a = b"),  std::string("(a = b)"));
    BEZEL_EXPECT_EQ(parseInitText("a <> b"), std::string("(a <> b)"));
}

BEZEL_TEST(parser_binary_expr, logical_and_or_are_keyword_operators) {
    BEZEL_EXPECT_EQ(parseInitText("a and b"), std::string("(a and b)"));
    BEZEL_EXPECT_EQ(parseInitText("a or b"),  std::string("(a or b)"));
}

// --- Precedence (§17.1 levels) ----------------------------------------------

// Multiplicative tighter than additive.
BEZEL_TEST(parser_binary_expr, mul_binds_tighter_than_add) {
    BEZEL_EXPECT_EQ(parseInitText("a + b * c"),
                    std::string("(a + (b * c))"));
    BEZEL_EXPECT_EQ(parseInitText("a * b + c"),
                    std::string("((a * b) + c)"));
}

// Additive tighter than relational.
BEZEL_TEST(parser_binary_expr, add_binds_tighter_than_relational) {
    BEZEL_EXPECT_EQ(parseInitText("a + b < c + d"),
                    std::string("((a + b) < (c + d))"));
}

// Relational tighter than equality.
BEZEL_TEST(parser_binary_expr, relational_binds_tighter_than_equality) {
    BEZEL_EXPECT_EQ(parseInitText("a < b = c < d"),
                    std::string("((a < b) = (c < d))"));
}

// Equality tighter than logical.
BEZEL_TEST(parser_binary_expr, equality_binds_tighter_than_logical) {
    BEZEL_EXPECT_EQ(parseInitText("a = b and c = d"),
                    std::string("((a = b) and (c = d))"));
}

// AND tighter than OR.
BEZEL_TEST(parser_binary_expr, and_binds_tighter_than_or) {
    BEZEL_EXPECT_EQ(parseInitText("a and b or c"),
                    std::string("((a and b) or c)"));
    BEZEL_EXPECT_EQ(parseInitText("a or b and c"),
                    std::string("(a or (b and c))"));
}

// `mod` shares the multiplicative level.
BEZEL_TEST(parser_binary_expr, mod_same_level_as_mul) {
    BEZEL_EXPECT_EQ(parseInitText("a mod b * c"),
                    std::string("((a mod b) * c)"));
}

// --- Associativity ----------------------------------------------------------
// Per §17.1, additive/multiplicative/logical are left-to-right.

BEZEL_TEST(parser_binary_expr, additive_is_left_associative) {
    BEZEL_EXPECT_EQ(parseInitText("a + b + c"),
                    std::string("((a + b) + c)"));
}

BEZEL_TEST(parser_binary_expr, multiplicative_is_left_associative) {
    BEZEL_EXPECT_EQ(parseInitText("a * b * c"),
                    std::string("((a * b) * c)"));
}

BEZEL_TEST(parser_binary_expr, and_is_left_associative) {
    BEZEL_EXPECT_EQ(parseInitText("a and b and c"),
                    std::string("((a and b) and c)"));
}

// --- Parens override precedence ---------------------------------------------

BEZEL_TEST(parser_binary_expr, parens_force_grouping) {
    BEZEL_EXPECT_EQ(parseInitText("(a + b) * c"),
                    std::string("((a + b) * c)"));
    BEZEL_EXPECT_EQ(parseInitText("a * (b + c)"),
                    std::string("(a * (b + c))"));
}

// --- Operands can be any primary form --------------------------------------

BEZEL_TEST(parser_binary_expr, mixed_with_calls_and_member_access) {
    BEZEL_EXPECT_EQ(parseInitText("f(a) + obj.field"),
                    std::string("(f(a) + obj.field)"));
}

BEZEL_TEST(parser_binary_expr, mixed_with_literals) {
    BEZEL_EXPECT_EQ(parseInitText("42 + 3.14 * 2"),
                    std::string("(42 + (3.14 * 2))"));
}
