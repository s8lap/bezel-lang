// Spec-derived parser tests for IndexerDecl — §A.4 indexer form of the
// property family: `Visibility "static"? VirtualKind? "readonly"? "property"
// "[" ParamList "]" ":" Type PropertyBody`. The `readonly?` slot is an
// over-admission on the body-bearing indexer productions (§23.2 / v0.214);
// sema rejects it on an expression-bodied indexer. These tests exercise the
// PARSER only (no sema), and verify that the keyword-less expression-bodied
// indexer is the valid accepted form.

#include <string>

#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::memberSummaries;
using bezel::test::parseAll;

BEZEL_TEST(parser_indexer, simple_indexer) {
    auto r = parseAll(
        "module M; class Foo "
        "public property [i: int]: string => items; end Foo;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(
            ms[0],
            std::string("public property [i: int]: string => items"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_indexer, multi_dimensional_indexer) {
    auto r = parseAll(
        "module M; class Grid "
        "public property [r: int, c: int]: int => values; end Grid;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(
            ms[0],
            std::string("public property [r: int, c: int]: int => values"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_indexer, missing_close_bracket_is_error) {
    auto r = parseAll(
        "module M; class Foo "
        "public property [i: int: string => items; end Foo;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
