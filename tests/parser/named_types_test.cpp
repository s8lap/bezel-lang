// Spec-derived parser tests for named type references (§3 / grammar A.5
// `Type ::= TypeNoNullable "?"?`, with `TypeNoNullable` reduced for this
// slice to `QualifiedName GenericArgs?`) and the degenerate type-alias form
// (§A.2 `TypeAliasDecl ::= "type" Identifier "=" Type ";"`).
//
// Inline-array (§15.1), function (§17/27), and unit `()` types are deferred
// to later slices; generic parameters on the alias (`type X<T> = ...`) and
// generic constraints (`where T: ...`) likewise.

#include <string>
#include <vector>

#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAll;
using bezel::test::topDeclSummaries;

// --- Simple named types (no generics, no nullable) --------------------------

BEZEL_TEST(parser_named_types, simple_alias_to_primitive) {
    auto r = parseAll("module M; type X = int;");
    const auto s = topDeclSummaries(r);
    if (s.size() == 1) BEZEL_EXPECT_EQ(s[0], std::string("type X = int"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_named_types, alias_to_qualified_name) {
    auto r = parseAll("module M; type X = Foo.Bar;");
    const auto s = topDeclSummaries(r);
    if (s.size() == 1) BEZEL_EXPECT_EQ(s[0], std::string("type X = Foo.Bar"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// --- Nullable suffix (§7 / A.5 `Type ::= TypeNoNullable "?"?`) ---------------

BEZEL_TEST(parser_named_types, nullable_primitive) {
    auto r = parseAll("module M; type X = int?;");
    const auto s = topDeclSummaries(r);
    if (s.size() == 1) BEZEL_EXPECT_EQ(s[0], std::string("type X = int?"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// --- Generic arguments (§14 / A.5 `GenericArgs`) ----------------------------

BEZEL_TEST(parser_named_types, single_generic_arg) {
    auto r = parseAll("module M; type X = List<int>;");
    const auto s = topDeclSummaries(r);
    if (s.size() == 1)
        BEZEL_EXPECT_EQ(s[0], std::string("type X = List<int>"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_named_types, multiple_generic_args) {
    auto r = parseAll("module M; type X = Dictionary<string, int>;");
    const auto s = topDeclSummaries(r);
    if (s.size() == 1)
        BEZEL_EXPECT_EQ(s[0],
                        std::string("type X = Dictionary<string, int>"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// The lexer emits `>>` as a single Punctuator (maximal munch). The parser
// must split it when it closes nested generics — this is the test of that.
BEZEL_TEST(parser_named_types, nested_generics_split_double_close_angle) {
    auto r = parseAll("module M; type X = List<List<int>>;");
    const auto s = topDeclSummaries(r);
    if (s.size() == 1)
        BEZEL_EXPECT_EQ(s[0], std::string("type X = List<List<int>>"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_named_types, three_level_nested_generics) {
    auto r = parseAll("module M; type X = A<B<C<int>>>;");
    const auto s = topDeclSummaries(r);
    if (s.size() == 1)
        BEZEL_EXPECT_EQ(s[0], std::string("type X = A<B<C<int>>>"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Generic arg can itself be nullable.
BEZEL_TEST(parser_named_types, generic_with_nullable_arg) {
    auto r = parseAll("module M; type X = List<int?>;");
    const auto s = topDeclSummaries(r);
    if (s.size() == 1)
        BEZEL_EXPECT_EQ(s[0], std::string("type X = List<int?>"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// The whole generic instantiation can be nullable.
BEZEL_TEST(parser_named_types, nullable_generic) {
    auto r = parseAll("module M; type X = List<int>?;");
    const auto s = topDeclSummaries(r);
    if (s.size() == 1)
        BEZEL_EXPECT_EQ(s[0], std::string("type X = List<int>?"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// --- Error cases ------------------------------------------------------------

BEZEL_TEST(parser_named_types, alias_missing_name_is_error) {
    auto r = parseAll("module M; type = int;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_named_types, alias_missing_equals_is_error) {
    auto r = parseAll("module M; type X int;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_named_types, alias_missing_semicolon_is_error) {
    auto r = parseAll("module M; type X = int");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_named_types, unclosed_generic_args_is_error) {
    auto r = parseAll("module M; type X = List<int;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// --- Class decls still parse correctly after the TopDecl refactor -----------

BEZEL_TEST(parser_named_types, type_alias_alongside_class_decl) {
    auto r = parseAll("module M; type X = int; class Foo end Foo;");
    const auto s = topDeclSummaries(r);
    BEZEL_EXPECT_EQ(s.size(), std::size_t{2});
    if (s.size() == 2) {
        BEZEL_EXPECT_EQ(s[0], std::string("type X = int"));
        BEZEL_EXPECT_EQ(s[1], std::string("class Foo"));
    }
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
