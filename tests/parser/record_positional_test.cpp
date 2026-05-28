// Spec-derived parser tests for record positional fields and the `;`-
// shorthand body — §A.3 `RecordDecl` / `PositionalFields` /
// `PositionalField`. SerializedClause on positional fields is deferred to
// its own slice.

#include <string>

#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAll;
using bezel::test::topDeclSummaries;

BEZEL_TEST(parser_record_positional, single_positional_field_shorthand) {
    auto r = parseAll("module M; record Point(x: int);");
    const auto s = topDeclSummaries(r);
    if (s.size() == 1)
        BEZEL_EXPECT_EQ(s[0],
                        std::string("record Point(x: int)"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_record_positional, multiple_positional_fields_shorthand) {
    auto r = parseAll("module M; record Point(x: int, y: int);");
    const auto s = topDeclSummaries(r);
    if (s.size() == 1)
        BEZEL_EXPECT_EQ(s[0],
                        std::string("record Point(x: int, y: int)"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_record_positional, generic_record_with_positional) {
    auto r = parseAll(
        "module M; record Box<T>(value: T, size: int);");
    const auto s = topDeclSummaries(r);
    if (s.size() == 1)
        BEZEL_EXPECT_EQ(
            s[0],
            std::string("record Box(value: T, size: int)<T>"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_record_positional, record_with_full_body) {
    auto r = parseAll(
        "module M; record Point(x: int, y: int) end Point;");
    const auto s = topDeclSummaries(r);
    if (s.size() == 1)
        BEZEL_EXPECT_EQ(s[0],
                        std::string("record Point(x: int, y: int)"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_record_positional, record_without_positional_still_works) {
    auto r = parseAll("module M; record Empty end Empty;");
    const auto s = topDeclSummaries(r);
    if (s.size() == 1)
        BEZEL_EXPECT_EQ(s[0], std::string("record Empty"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_record_positional,
           positional_record_with_inheritance) {
    auto r = parseAll(
        "module M; record Sub(x: int): Base end Sub;");
    const auto s = topDeclSummaries(r);
    if (s.size() == 1)
        BEZEL_EXPECT_EQ(
            s[0], std::string("record Sub(x: int): Base"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Errors

BEZEL_TEST(parser_record_positional, missing_close_paren_is_error) {
    auto r = parseAll("module M; record Point(x: int;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_record_positional, missing_semicolon_in_shorthand_is_error) {
    // Without trailing `;` and without `end`, the record header dangles.
    auto r = parseAll("module M; record Point(x: int)");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
