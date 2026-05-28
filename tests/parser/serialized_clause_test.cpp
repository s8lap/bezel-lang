// Spec-derived parser tests for SerializedClause — §A.4
// `SerializedClause ::= "serialized" ("skip" | SerializedName)`
// `SerializedName ::= StringLiteral? "optional"?`  (at least one of the two)
//
// Three positions admit SerializedClause per the grammar:
//   - A.3 PositionalField (record positional fields)
//   - A.3 EnumConstant
//   - A.4 Auto-property PropertyBody (`(":=" Expression)? SerializedClause? ";"`)
//
// `serialized`, `skip`, `optional` are contextual keywords (App. B.2)
// lexed as Identifiers; the parser dispatches by text. Semantic
// restrictions (skip excludes name+optional; enum constants reject skip
// and optional) belong to sema; the parser admits the grammar's full
// shape so sema can produce locatable diagnostics.

#include <string>

#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::memberSummaries;
using bezel::test::parseAll;
using bezel::test::topDeclSummaries;

BEZEL_TEST(parser_serialized, positional_field_renamed) {
    auto r = parseAll(
        "module M; record Point(x: int serialized \"X\", y: int);");
    const auto top = topDeclSummaries(r);
    if (top.size() == 1)
        BEZEL_EXPECT_EQ(
            top[0],
            std::string("record Point(x: int serialized \"X\", y: int)"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_serialized, positional_field_optional) {
    auto r = parseAll(
        "module M; record Cfg(host: string, port: int serialized optional);");
    const auto top = topDeclSummaries(r);
    if (top.size() == 1)
        BEZEL_EXPECT_EQ(
            top[0],
            std::string(
                "record Cfg(host: string, "
                "port: int serialized optional)"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_serialized, positional_field_name_and_optional) {
    auto r = parseAll(
        "module M; record Cfg(port: int serialized \"p\" optional);");
    const auto top = topDeclSummaries(r);
    if (top.size() == 1)
        BEZEL_EXPECT_EQ(
            top[0],
            std::string(
                "record Cfg(port: int serialized \"p\" optional)"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_serialized, positional_field_skip) {
    auto r = parseAll(
        "module M; record Cfg(host: string, secret: string serialized skip);");
    const auto top = topDeclSummaries(r);
    if (top.size() == 1)
        BEZEL_EXPECT_EQ(
            top[0],
            std::string(
                "record Cfg(host: string, secret: string serialized skip)"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_serialized, bare_serialized_is_error) {
    // Grammar requires at least one of `skip`, name, or `optional`.
    auto r = parseAll(
        "module M; record R(x: int serialized);");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_serialized, enum_constant_renamed) {
    auto r = parseAll(
        "module M; enum Color "
        "Red serialized \"red\", Green, Blue serialized \"blue\"; "
        "end Color;");
    const auto top = topDeclSummaries(r);
    if (top.size() == 1)
        BEZEL_EXPECT_EQ(
            top[0],
            std::string(
                "enum Color { Red serialized \"red\", Green, "
                "Blue serialized \"blue\" }"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_serialized, enum_constant_with_value_and_name) {
    auto r = parseAll(
        "module M; enum Mode "
        "Off := 0 serialized \"off\", "
        "On  := 1 serialized \"on\"; "
        "end Mode;");
    const auto top = topDeclSummaries(r);
    if (top.size() == 1)
        BEZEL_EXPECT_EQ(
            top[0],
            std::string(
                "enum Mode { Off := 0 serialized \"off\", "
                "On := 1 serialized \"on\" }"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_serialized, auto_property_with_name) {
    auto r = parseAll(
        "module M; class C "
        "public property Host: string serialized \"host\"; "
        "end C;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(
            ms[0],
            std::string(
                "public property Host: string serialized \"host\""));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_serialized, auto_property_with_initializer_and_optional) {
    auto r = parseAll(
        "module M; class C "
        "public property Port: int := 80 serialized optional; "
        "end C;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(
            ms[0],
            std::string(
                "public property Port: int := 80 serialized optional"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_serialized, auto_property_serialized_skip) {
    auto r = parseAll(
        "module M; class C "
        "public property Cache: string serialized skip; "
        "end C;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(
            ms[0],
            std::string(
                "public property Cache: string serialized skip"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
