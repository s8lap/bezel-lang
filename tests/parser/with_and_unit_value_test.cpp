// Spec-derived parser tests for the §A.7 `with` PrimarySuffix and the
// unit value `()`.
//
//   PrimarySuffix ::= ... | "with" InitAssign ("," InitAssign)* ","? "end"
//   InitAssign    ::= Identifier ":=" Expression
//   PrimaryAtom   ::= ... | "(" ")"            /* unit value */
//
// `with` has three uses across the language:
//   1. record copy-with-overrides (this test exercises the syntactic shape)
//   2. object initializer block on `new C(...)` (same shape, sema picks)
//   3. cancellation token in `await for ... with EXPR do ... end ;` —
//      regression-tested separately; the iterable-parse disables the
//      postfix `with` so the for-stmt sees its `with` clause.

#include <string>

#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::exprToString;
using bezel::test::parseAll;

namespace {

const bezel::ast::Expr* extract(const bezel::test::ParseResult& r) {
    if (!r.sourceFile || r.sourceFile->topDecls.empty()) return nullptr;
    const auto* td = dynamic_cast<const bezel::ast::TypeDecl*>(
        r.sourceFile->topDecls[0].get());
    if (!td || td->members.empty()) return nullptr;
    const auto* md = dynamic_cast<const bezel::ast::MethodDecl*>(
        td->members[0].get());
    if (!md || md->stmts.empty()) return nullptr;
    const auto* ds = dynamic_cast<const bezel::ast::DiscardStmt*>(
        md->stmts[0].get());
    if (!ds) return nullptr;
    return ds->value.get();
}

std::string render(const std::string& expr) {
    const std::string src = "module M; class C public method M() _ := " +
                            expr + "; end M; end C;";
    auto r = parseAll(src);
    if (r.diagnostics.hasErrors()) return "<errors>";
    const auto* e = extract(r);
    return e ? exprToString(*e) : std::string("<no-expr>");
}

} // namespace

BEZEL_TEST(parser_with_suffix, single_init_assign) {
    BEZEL_EXPECT_EQ(
        render("p with X := 1 end"),
        std::string("p with X := 1 end"));
}

BEZEL_TEST(parser_with_suffix, multiple_init_assigns) {
    BEZEL_EXPECT_EQ(
        render("p with X := 1, Y := 2, Z := 3 end"),
        std::string("p with X := 1, Y := 2, Z := 3 end"));
}

BEZEL_TEST(parser_with_suffix, on_new_expression) {
    // Object initializer block: `with` after `new C(...)` — same syntactic
    // shape as record copy-with.
    BEZEL_EXPECT_EQ(
        render("new Point(0, 0) with X := 5, Y := 7 end"),
        std::string("new Point(0, 0) with X := 5, Y := 7 end"));
}

BEZEL_TEST(parser_with_suffix, value_is_expression) {
    BEZEL_EXPECT_EQ(
        render("p with X := a + b end"),
        std::string("p with X := (a + b) end"));
}

BEZEL_TEST(parser_with_suffix, trailing_comma_allowed) {
    BEZEL_EXPECT_EQ(
        render("p with X := 1, Y := 2, end"),
        std::string("p with X := 1, Y := 2 end"));
}

BEZEL_TEST(parser_with_suffix, chained_after_call) {
    BEZEL_EXPECT_EQ(
        render("MakePoint() with X := 5 end"),
        std::string("MakePoint() with X := 5 end"));
}

BEZEL_TEST(parser_unit_value, bare_unit) {
    BEZEL_EXPECT_EQ(render("()"), std::string("()"));
}

BEZEL_TEST(parser_unit_value, unit_in_call_arg) {
    BEZEL_EXPECT_EQ(
        render("f((), ())"),
        std::string("f((), ())"));
}

BEZEL_TEST(parser_unit_value, unit_lambda_still_works) {
    // Regression: `() => 42` continues to take the empty-params lambda
    // path, not the unit-value path. The lambda discriminator runs first.
    BEZEL_EXPECT_EQ(
        render("lambda() => 42"),
        std::string("(lambda() => 42)"));
}
