// Spec-derived parser tests for class field declarations — §A.4
// `FieldDecl ::= Visibility "static"? "owned"? "readonly"? Identifier
//                ":" Type (":=" Expression)? ";"`.
//
// Slice 6 omits the optional initializer — initializers are expressions
// (their own slice). Interface members, record bodies, and enum constants
// have their own grammars and land in their own slices.

#include <string>
#include <vector>

#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::memberSummaries;
using bezel::test::parseAll;

BEZEL_TEST(parser_field_decls, single_public_field) {
    auto r = parseAll("module M; class Foo public x: int; end Foo;");
    const auto ms = memberSummaries(r);
    BEZEL_EXPECT_EQ(ms.size(), std::size_t{1});
    if (!ms.empty()) BEZEL_EXPECT_EQ(ms[0], std::string("public x: int"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_field_decls, multiple_fields_in_order) {
    auto r = parseAll(
        "module M; class Foo "
        "  public x: int; "
        "  private y: string; "
        "end Foo;");
    const auto ms = memberSummaries(r);
    BEZEL_EXPECT_EQ(ms.size(), std::size_t{2});
    if (ms.size() == 2) {
        BEZEL_EXPECT_EQ(ms[0], std::string("public x: int"));
        BEZEL_EXPECT_EQ(ms[1], std::string("private y: string"));
    }
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_field_decls, all_modifier_combinations) {
    auto r = parseAll(
        "module M; class Foo "
        "  public static x: int; "
        "  public readonly y: int; "
        "  public owned z: Connection; "
        "  public static readonly w: int; "
        "end Foo;");
    const auto ms = memberSummaries(r);
    BEZEL_EXPECT_EQ(ms.size(), std::size_t{4});
    if (ms.size() == 4) {
        BEZEL_EXPECT_EQ(ms[0], std::string("public static x: int"));
        BEZEL_EXPECT_EQ(ms[1], std::string("public readonly y: int"));
        BEZEL_EXPECT_EQ(ms[2], std::string("public owned z: Connection"));
        BEZEL_EXPECT_EQ(ms[3],
                        std::string("public static readonly w: int"));
    }
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_field_decls, generic_field_type) {
    auto r = parseAll(
        "module M; class Foo public items: List<int>; end Foo;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(ms[0], std::string("public items: List<int>"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_field_decls, nullable_field_type) {
    auto r = parseAll(
        "module M; class Foo public name: string?; end Foo;");
    const auto ms = memberSummaries(r);
    if (ms.size() == 1)
        BEZEL_EXPECT_EQ(ms[0], std::string("public name: string?"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §13: visibility is required on members.
BEZEL_TEST(parser_field_decls, missing_visibility_is_error) {
    auto r = parseAll("module M; class Foo x: int; end Foo;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_field_decls, missing_colon_is_error) {
    auto r = parseAll("module M; class Foo public x int; end Foo;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_field_decls, missing_semicolon_is_error) {
    auto r = parseAll("module M; class Foo public x: int end Foo;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// Fields combine cleanly with the slice-5 header additions (generics,
// inheritance).
BEZEL_TEST(parser_field_decls,
           generic_class_with_fields_and_inheritance) {
    auto r = parseAll(
        "module M; class Box<T>: Container<T> "
        "  public value: T; "
        "  public size: int; "
        "end Box;");
    const auto ms = memberSummaries(r);
    BEZEL_EXPECT_EQ(ms.size(), std::size_t{2});
    if (ms.size() == 2) {
        BEZEL_EXPECT_EQ(ms[0], std::string("public value: T"));
        BEZEL_EXPECT_EQ(ms[1], std::string("public size: int"));
    }
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Empty class body still works.
BEZEL_TEST(parser_field_decls, empty_class_body_still_parses) {
    auto r = parseAll("module M; class Foo end Foo;");
    BEZEL_EXPECT_EQ(memberSummaries(r).size(), std::size_t{0});
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
