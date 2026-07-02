// Spec-derived parser tests for §A.3 GenericConstraints.
//
// v0.97 retired the `where T is owned` / `where T is not owned` modifier-
// constraint form together with `<owned T>` generic arguments (§14.4).
// The grammar now reads:
//
//   GenericConstraints ::= "where" Identifier ":" Constraint ("," Constraint)*
//   Constraint ::= "class" | "value" | "notnull" | "new" "(" ")" | Type
//
// These tests verify that:
//   - The retired `is owned` / `is not owned` form is REJECTED with the
//     specific diagnostic naming §14.2 (tests below that flip what was
//     "accepted" in slice-54 era).
//   - The colon-form (`where T: class`) still works (regression).

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

// Return true when any error diagnostic mentions the v0.97 retirement.
bool hasRetirementError(const bezel::test::ParseResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity == bezel::Severity::Error &&
            d.message.find("pre-v0.97") != std::string::npos)
            return true;
    }
    return false;
}

} // namespace

// §A.3 / §14.4 (v0.97): `where T is owned` must now be REJECTED — the form
// was retired and the parser emits "the pre-v0.97 `where T is owned`
// constraint form was removed; constraints bound types (§14.2)".
BEZEL_TEST(parser_modifier_constraint, where_T_is_owned_on_class) {
    auto r = parseAll(
        "module M; public class C<T> where T is owned end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasRetirementError(r));
}

BEZEL_TEST(parser_modifier_constraint, where_T_is_not_owned_on_class) {
    auto r = parseAll(
        "module M; public class C<T> where T is not owned end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasRetirementError(r));
}

BEZEL_TEST(parser_modifier_constraint, on_method) {
    auto r = parseAll(
        "module M; public class C "
        "public method M<T>() where T is owned => 0; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasRetirementError(r));
}

BEZEL_TEST(parser_modifier_constraint, multiple_clauses_mixed) {
    // The colon-form clause should parse; the retired `is owned` clause
    // must emit a retirement diagnostic.
    auto r = parseAll(
        "module M; public class C<T, U> "
        "where T: class "
        "where U is owned "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasRetirementError(r));
    const auto* td = extractTypeDecl(r);
    // The colon-form clause for T should still have been recorded.
    if (td && !td->genericConstraints.empty()) {
        BEZEL_EXPECT_TRUE(
            td->genericConstraints[0].modifierConstraint.empty());
        BEZEL_EXPECT_EQ(
            td->genericConstraints[0].items.size(), std::size_t(1));
    }
}

BEZEL_TEST(parser_modifier_constraint, regular_form_still_works) {
    // Regression: the `where T: class` regular form must continue to take
    // the items path without errors.
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
