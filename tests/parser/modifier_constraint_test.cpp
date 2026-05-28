// Spec-derived parser tests for the §A.3 GenericConstraints modifier-form:
//
//   GenericConstraints ::= "where" Identifier ":" Constraint ("," Constraint)*
//                        | "where" Identifier "is" ModifierConstraint
//   ModifierConstraint ::= "owned" | "not" "owned"
//
// Slice 54 adds the `is owned` / `is not owned` form (§14.4). The regular
// `:`-form path is unchanged and still parsed first; if `is` follows the
// type-parameter Identifier instead, the parser takes the modifier-form
// path and records `modifierConstraint` instead of `items`.

#include <string>

#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAll;

namespace {

const bezel::ast::TypeDecl* extractTypeDecl(
    const bezel::test::ParseResult& r) {
    if (!r.sourceFile || r.sourceFile->topDecls.empty()) return nullptr;
    return dynamic_cast<const bezel::ast::TypeDecl*>(
        r.sourceFile->topDecls[0].get());
}

const bezel::ast::MethodDecl* firstMethod(
    const bezel::test::ParseResult& r) {
    const auto* td = extractTypeDecl(r);
    if (!td || td->members.empty()) return nullptr;
    return dynamic_cast<const bezel::ast::MethodDecl*>(
        td->members[0].get());
}

} // namespace

BEZEL_TEST(parser_modifier_constraint, where_T_is_owned_on_class) {
    auto r = parseAll(
        "module M; public class C<T> where T is owned end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const auto* td = extractTypeDecl(r);
    if (td) {
        BEZEL_EXPECT_EQ(td->genericConstraints.size(), std::size_t(1));
        if (td->genericConstraints.size() == 1) {
            BEZEL_EXPECT_EQ(
                td->genericConstraints[0].typeParam,
                std::string_view("T"));
            BEZEL_EXPECT_EQ(
                td->genericConstraints[0].modifierConstraint,
                std::string_view("owned"));
            BEZEL_EXPECT_TRUE(td->genericConstraints[0].items.empty());
        }
    }
}

BEZEL_TEST(parser_modifier_constraint, where_T_is_not_owned_on_class) {
    auto r = parseAll(
        "module M; public class C<T> where T is not owned end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const auto* td = extractTypeDecl(r);
    if (td && td->genericConstraints.size() == 1) {
        BEZEL_EXPECT_EQ(
            td->genericConstraints[0].modifierConstraint,
            std::string_view("not owned"));
    }
}

BEZEL_TEST(parser_modifier_constraint, on_method) {
    auto r = parseAll(
        "module M; public class C "
        "public method M<T>() where T is owned => 0; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const auto* md = firstMethod(r);
    if (md && md->genericConstraints.size() == 1) {
        BEZEL_EXPECT_EQ(
            md->genericConstraints[0].modifierConstraint,
            std::string_view("owned"));
    }
}

BEZEL_TEST(parser_modifier_constraint, multiple_clauses_mixed) {
    auto r = parseAll(
        "module M; public class C<T, U> "
        "where T: class "
        "where U is owned "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const auto* td = extractTypeDecl(r);
    if (td) {
        BEZEL_EXPECT_EQ(td->genericConstraints.size(), std::size_t(2));
        if (td->genericConstraints.size() == 2) {
            // First clause: regular-form `where T: class`.
            BEZEL_EXPECT_TRUE(
                td->genericConstraints[0].modifierConstraint.empty());
            BEZEL_EXPECT_EQ(
                td->genericConstraints[0].items.size(), std::size_t(1));
            // Second clause: modifier-form `where U is owned`.
            BEZEL_EXPECT_EQ(
                td->genericConstraints[1].modifierConstraint,
                std::string_view("owned"));
        }
    }
}

BEZEL_TEST(parser_modifier_constraint, regular_form_still_works) {
    // Regression: the `where T: class` regular form must continue to take
    // the items path.
    auto r = parseAll(
        "module M; public class C<T> where T: class end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const auto* td = extractTypeDecl(r);
    if (td && td->genericConstraints.size() == 1) {
        BEZEL_EXPECT_TRUE(
            td->genericConstraints[0].modifierConstraint.empty());
        BEZEL_EXPECT_EQ(
            td->genericConstraints[0].items.size(), std::size_t(1));
    }
}
