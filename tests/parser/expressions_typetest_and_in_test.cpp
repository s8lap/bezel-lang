// Spec-derived parser tests for the keyword relationals — §17.4 (`is`,
// `is not`, `as`) and §17.5 (`in`). All four sit at §17.1's relational
// precedence (level 10).

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

// --- `is` / `is not` / `as` (§17.4) ----------------------------------------

BEZEL_TEST(parser_type_test, is_with_type) {
    BEZEL_EXPECT_EQ(parseInitText("x is Foo"),
                    std::string("(x is Foo)"));
}

BEZEL_TEST(parser_type_test, is_with_generic_type) {
    BEZEL_EXPECT_EQ(parseInitText("x is List<int>"),
                    std::string("(x is List<int>)"));
}

BEZEL_TEST(parser_type_test, is_with_nullable_type) {
    BEZEL_EXPECT_EQ(parseInitText("x is Foo?"),
                    std::string("(x is Foo?)"));
}

// §17.4 + §B.2: "`is not` is a single operator with two-token spelling — the
// parser does not accept `not is`, and `not (x is T)` is a different
// expression (no narrowing in the negated branch)."
BEZEL_TEST(parser_type_test, is_not_is_two_keyword_tokens_one_operator) {
    BEZEL_EXPECT_EQ(parseInitText("x is not Foo"),
                    std::string("(x is not Foo)"));
}

BEZEL_TEST(parser_type_test, as_with_type) {
    BEZEL_EXPECT_EQ(parseInitText("x as Foo"),
                    std::string("(x as Foo)"));
}

// §17.4: "`as` on value types: x as T produces T?".  The lexeme of the type
// is what the parser stores; sema decides the result type.
BEZEL_TEST(parser_type_test, as_with_primitive_type) {
    BEZEL_EXPECT_EQ(parseInitText("x as int"),
                    std::string("(x as int)"));
}

// --- `in` (§17.5) -----------------------------------------------------------

BEZEL_TEST(parser_type_test, in_with_collection) {
    BEZEL_EXPECT_EQ(parseInitText("x in numbers"),
                    std::string("(x in numbers)"));
}

// §17.5 example: `score in 60..100` — `in` binds looser than range so the
// range is the collection.
BEZEL_TEST(parser_type_test, in_with_range_per_17_5) {
    BEZEL_EXPECT_EQ(parseInitText("score in 60 .. 100"),
                    std::string("(score in (60 .. 100))"));
}

// --- Precedence relative to other binary operators -------------------------

// Relational tighter than equality.
BEZEL_TEST(parser_type_test,
           is_at_relational_level_tighter_than_equality) {
    BEZEL_EXPECT_EQ(parseInitText("x is Foo = y is Bar"),
                    std::string("((x is Foo) = (y is Bar))"));
}

// Relational looser than additive (so `a + b is C` is `(a + b) is C`).
BEZEL_TEST(parser_type_test, is_looser_than_additive) {
    BEZEL_EXPECT_EQ(parseInitText("a + b is Foo"),
                    std::string("((a + b) is Foo)"));
}

BEZEL_TEST(parser_type_test, in_looser_than_additive) {
    BEZEL_EXPECT_EQ(parseInitText("a + b in items"),
                    std::string("((a + b) in items)"));
}
