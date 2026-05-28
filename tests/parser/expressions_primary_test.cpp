// Spec-derived parser tests for primary expressions — §17 / §18 / §A.5.
// Slice 7 covers literals, identifiers, parenthesized expressions, and the
// two postfix forms that don't require new lexer tokens: member access
// (`.x`) and call (`(args)`). Unary, binary, `new`, `?.`, `?(`, `[...]`,
// `!` (postfix), casts, and the rest are later slices.
//
// Test vehicle: field initializers (`public x: T := EXPR;`), now that field
// declarations from slice 6 admit an initializer.

#include <string>
#include <vector>

#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::memberSummaries;
using bezel::test::parseAll;

// --- Literals (§18) ---------------------------------------------------------

BEZEL_TEST(parser_primary_expr, int_literal_initializer) {
    auto r = parseAll(
        "module M; class Foo public x: int := 42; end Foo;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(ms[0], std::string("public x: int := 42"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_primary_expr, float_literal_initializer) {
    auto r = parseAll(
        "module M; class Foo public x: double := 3.14; end Foo;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(ms[0], std::string("public x: double := 3.14"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_primary_expr, char_literal_initializer) {
    auto r = parseAll(
        "module M; class Foo public x: char := 'a'; end Foo;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(ms[0], std::string("public x: char := 'a'"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_primary_expr, string_literal_initializer) {
    auto r = parseAll(
        "module M; class Foo public x: string := \"hi\"; end Foo;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(ms[0], std::string("public x: string := \"hi\""));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_primary_expr, verbatim_string_literal_initializer) {
    auto r = parseAll(
        "module M; class Foo public x: string := @\"raw\"; end Foo;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(ms[0], std::string("public x: string := @\"raw\""));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §18.2 boolean and null literals.
BEZEL_TEST(parser_primary_expr, bool_true_literal) {
    auto r = parseAll(
        "module M; class Foo public x: bool := true; end Foo;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(ms[0], std::string("public x: bool := true"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_primary_expr, bool_false_literal) {
    auto r = parseAll(
        "module M; class Foo public x: bool := false; end Foo;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(ms[0], std::string("public x: bool := false"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_primary_expr, null_literal) {
    auto r = parseAll(
        "module M; class Foo public x: string? := null; end Foo;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(ms[0], std::string("public x: string? := null"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// --- Identifiers and parenthesized expressions ------------------------------

BEZEL_TEST(parser_primary_expr, identifier_expression) {
    auto r = parseAll(
        "module M; class Foo public x: int := other; end Foo;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(ms[0], std::string("public x: int := other"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_primary_expr, parenthesized_expression) {
    auto r = parseAll(
        "module M; class Foo public x: int := (42); end Foo;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(ms[0], std::string("public x: int := (42)"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// --- Postfix: member access and call ----------------------------------------

BEZEL_TEST(parser_primary_expr, member_access) {
    auto r = parseAll(
        "module M; class Foo public x: int := obj.field; end Foo;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(ms[0], std::string("public x: int := obj.field"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_primary_expr, chained_member_access) {
    auto r = parseAll(
        "module M; class Foo public x: int := a.b.c.d; end Foo;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(ms[0], std::string("public x: int := a.b.c.d"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_primary_expr, call_no_args) {
    auto r = parseAll(
        "module M; class Foo public x: int := f(); end Foo;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(ms[0], std::string("public x: int := f()"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_primary_expr, call_with_args) {
    auto r = parseAll(
        "module M; class Foo public x: int := f(1, true, \"hi\"); end Foo;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(
            ms[0],
            std::string("public x: int := f(1, true, \"hi\")"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_primary_expr, call_chained_with_member_access) {
    // Note: `method` is a reserved keyword (App. B.1) so it can't appear as
    // a member name; we use `compute` instead.
    auto r = parseAll(
        "module M; class Foo public x: int := obj.compute(arg).field; "
        "end Foo;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(
            ms[0],
            std::string("public x: int := obj.compute(arg).field"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// --- Field without initializer still works ---------------------------------

BEZEL_TEST(parser_primary_expr,
           field_without_initializer_unchanged_from_slice_6) {
    auto r = parseAll(
        "module M; class Foo public x: int; end Foo;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(ms[0], std::string("public x: int"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// --- Error cases ------------------------------------------------------------

BEZEL_TEST(parser_primary_expr, missing_expression_after_walrus_is_error) {
    auto r = parseAll(
        "module M; class Foo public x: int := ; end Foo;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_primary_expr, unclosed_paren_is_error) {
    auto r = parseAll(
        "module M; class Foo public x: int := (42; end Foo;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_primary_expr, unclosed_call_args_is_error) {
    auto r = parseAll(
        "module M; class Foo public x: int := f(1, 2; end Foo;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
