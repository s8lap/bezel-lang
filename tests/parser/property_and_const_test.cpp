// Spec-derived parser tests for ConstDecl (§A.4) and PropertyDecl
// (auto-property + expression-body forms, §A.4). The accessor-block form
// (`get`/`set` with bodies), AutoAccessorVis (`private set`), and the
// SerializedClause modifier on auto-properties are deferred.

#include <string>

#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::memberSummaries;
using bezel::test::parseAll;

// --- ConstDecl --------------------------------------------------------------

BEZEL_TEST(parser_const_property, const_with_type_and_value) {
    auto r = parseAll(
        "module M; class Foo public const PI: double := 3.14; end Foo;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(
            ms[0],
            std::string("public const PI: double := 3.14"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_const_property, const_inferred_type) {
    auto r = parseAll(
        "module M; class Foo public const ANSWER := 42; end Foo;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(ms[0],
                        std::string("public const ANSWER := 42"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_const_property, static_const) {
    auto r = parseAll(
        "module M; class Foo public static const X: int := 1; end Foo;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(
            ms[0],
            std::string("public static const X: int := 1"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// --- PropertyDecl: auto-property -------------------------------------------

BEZEL_TEST(parser_const_property, auto_property_no_initializer) {
    auto r = parseAll(
        "module M; class Foo public property Name: string; end Foo;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(ms[0],
                        std::string("public property Name: string"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_const_property, auto_property_with_initializer) {
    auto r = parseAll(
        "module M; class Foo public property Count: int := 0; end Foo;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(
            ms[0],
            std::string("public property Count: int := 0"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_const_property, readonly_auto_property) {
    auto r = parseAll(
        "module M; class Foo "
        "public readonly property Id: int := 0; end Foo;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(
            ms[0],
            std::string("public readonly property Id: int := 0"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// --- PropertyDecl: expression-body -----------------------------------------

BEZEL_TEST(parser_const_property, expression_body_property) {
    auto r = parseAll(
        "module M; class Foo "
        "public property FullName: string => firstName; "
        "end Foo;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(
            ms[0],
            std::string(
                "public property FullName: string => firstName"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// --- Errors -----------------------------------------------------------------

BEZEL_TEST(parser_const_property, const_without_initializer_is_error) {
    auto r = parseAll(
        "module M; class Foo public const X: int; end Foo;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_const_property,
           accessor_block_property_parses_in_slice_37) {
    // Accessor-block PropertyBody landed in slice 37 — now expected to
    // parse cleanly. The detailed shape assertions live in
    // `parser/property_accessor_blocks_test.cpp`.
    auto r = parseAll(
        "module M; class Foo "
        "public property X: int get => 0; end; end Foo;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
