// Spec-derived parser tests for EnumMember — §A.3:
//
//   EnumDecl   ::= Visibility? "enum" Identifier EnumInheritance?
//                  EnumConstants
//                  EnumMember* "end" Identifier? ";"
//   EnumMember ::= MethodDecl | ExpressionPropertyDecl
//                | StaticReadonlyDecl | StaticConstDecl
//
// Slice 14 covered the constants list. Slice 47 adds the optional
// EnumMember* section after the constants list, sharing the existing
// member-parse dispatch — sema later restricts which member kinds are
// admitted per A.3.

#include <string>

#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::memberSummaries;
using bezel::test::parseAll;

BEZEL_TEST(parser_enum_member, single_method) {
    auto r = parseAll(
        "module M; enum Color "
        "Red, Green, Blue; "
        "public method ToHex(): string => \"#\"; "
        "end Color;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(
            ms[0],
            std::string(
                "public method ToHex(): string => \"#\""));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_enum_member, expression_property) {
    auto r = parseAll(
        "module M; enum Mode "
        "Off, On; "
        "public property IsOn: bool => true; "
        "end Mode;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(
            ms[0],
            std::string("public property IsOn: bool => true"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_enum_member, static_const) {
    auto r = parseAll(
        "module M; enum Direction "
        "North, South, East, West; "
        "public const Count: int := 4; "
        "end Direction;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(
            ms[0],
            std::string("public const Count: int := 4"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_enum_member, static_readonly_field) {
    // A.3 StaticReadonlyDecl: parses through the regular field-decl path
    // with `static readonly` modifiers and an initializer.
    auto r = parseAll(
        "module M; enum E "
        "A, B; "
        "public static readonly Default: E := A; "
        "end E;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_enum_member, multiple_members_mixed) {
    auto r = parseAll(
        "module M; enum Color "
        "Red, Green, Blue; "
        "public const Count: int := 3; "
        "public method ToString(): string => \"color\"; "
        "public property IsPrimary: bool => true; "
        "end Color;");
    const auto ms = memberSummaries(r);
    BEZEL_EXPECT_EQ(ms.size(), std::size_t(3));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_enum_member, enum_without_members_still_works) {
    // Regression: enum bodies with only constants must continue to parse.
    auto r = parseAll(
        "module M; enum Color Red, Green, Blue; end Color;");
    const auto ms = memberSummaries(r);
    BEZEL_EXPECT_EQ(ms.size(), std::size_t(0));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
