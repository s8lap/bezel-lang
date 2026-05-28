// Spec-derived parser tests for the empty class-like top-level declarations —
// §11 (Classes/Interfaces), §10 (Enums), §9 (Records), and §1.6 (the named-
// construct `end Identifier? ;` termination rule). Grammar A.3.
//
// "Empty" here means no members, no inheritance clause, no generic params,
// no primary-constructor params, no serialization clauses. Those land in
// their own slices.

#include <string>
#include <vector>

#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAll;
using bezel::test::topDeclSummaries;

BEZEL_TEST(parser_empty_type_decls, simple_class) {
    auto r = parseAll("module M; class Foo end Foo;");
    const auto s = topDeclSummaries(r);
    BEZEL_EXPECT_EQ(s.size(), std::size_t{1});
    if (!s.empty()) BEZEL_EXPECT_EQ(s[0], std::string("class Foo"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §1.6: the trailing identifier on `end` is optional; `end;` alone is valid.
BEZEL_TEST(parser_empty_type_decls, class_with_plain_end) {
    auto r = parseAll("module M; class Foo end;");
    const auto s = topDeclSummaries(r);
    BEZEL_EXPECT_EQ(s.size(), std::size_t{1});
    if (!s.empty()) BEZEL_EXPECT_EQ(s[0], std::string("class Foo"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §13 visibility keywords appear before any modifiers.
BEZEL_TEST(parser_empty_type_decls, class_with_visibility) {
    auto r = parseAll("module M; public class Foo end Foo;");
    const auto s = topDeclSummaries(r);
    if (s.size() == 1) BEZEL_EXPECT_EQ(s[0], std::string("public class Foo"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Grammar `Modifier* = ("abstract" | "sealed" | "static")*` — single modifier
// or a combination. (Semantic validity is §11.7; the grammar admits any combo.)
BEZEL_TEST(parser_empty_type_decls, class_with_modifier) {
    auto r = parseAll("module M; abstract class Foo end Foo;");
    const auto s = topDeclSummaries(r);
    if (s.size() == 1) BEZEL_EXPECT_EQ(s[0], std::string("abstract class Foo"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_empty_type_decls, class_with_visibility_and_modifier) {
    auto r = parseAll("module M; public sealed class Foo end Foo;");
    const auto s = topDeclSummaries(r);
    if (s.size() == 1)
        BEZEL_EXPECT_EQ(s[0], std::string("public sealed class Foo"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Grammar A.3: Visibility? must precede Modifier*. Reversed order is a syntax
// error (the modifier-keyword scanner won't accept `public` as a modifier).
BEZEL_TEST(parser_empty_type_decls, modifier_before_visibility_is_error) {
    auto r = parseAll("module M; abstract public class Foo end Foo;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §1.6: "The identifier must match the construct's declared name exactly;
// a mismatch is a compile error."
BEZEL_TEST(parser_empty_type_decls, end_label_mismatch_is_error) {
    auto r = parseAll("module M; class Foo end Bar;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// Missing `;` after `end` / `end Foo`.
BEZEL_TEST(parser_empty_type_decls, missing_semicolon_after_end_is_error) {
    auto r = parseAll("module M; class Foo end Foo");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_empty_type_decls, simple_interface) {
    auto r = parseAll("module M; interface IFoo end IFoo;");
    const auto s = topDeclSummaries(r);
    if (s.size() == 1) BEZEL_EXPECT_EQ(s[0], std::string("interface IFoo"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_empty_type_decls, simple_record) {
    auto r = parseAll("module M; record Bar end Bar;");
    const auto s = topDeclSummaries(r);
    if (s.size() == 1) BEZEL_EXPECT_EQ(s[0], std::string("record Bar"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_empty_type_decls, simple_enum) {
    // §A.3 EnumDecl requires `EnumConstants` (at least one constant); the
    // constants slice covers the populated form. Here we just verify the
    // empty form is rejected.
    auto r = parseAll("module M; enum E end E;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §2.4: many top-level declarations per source file.
BEZEL_TEST(parser_empty_type_decls, multiple_top_decls_in_order) {
    auto r = parseAll("module M; class A end A; interface IB end IB; "
                      "record C end C; enum D X; end D;");
    const auto s = topDeclSummaries(r);
    BEZEL_EXPECT_EQ(s.size(), std::size_t{4});
    if (s.size() == 4) {
        BEZEL_EXPECT_EQ(s[0], std::string("class A"));
        BEZEL_EXPECT_EQ(s[1], std::string("interface IB"));
        BEZEL_EXPECT_EQ(s[2], std::string("record C"));
        BEZEL_EXPECT_EQ(s[3], std::string("enum D { X }"));
    }
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Top-level decls follow imports.
BEZEL_TEST(parser_empty_type_decls, imports_then_top_decls) {
    auto r = parseAll("module M; import X; import Y.Z; class Foo end Foo;");
    BEZEL_EXPECT_EQ(topDeclSummaries(r).size(), std::size_t{1});
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
