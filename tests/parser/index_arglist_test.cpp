// Spec-derived parser tests for indexer index lists — §A.7 PrimarySuffix
// `[ ArgList ]`. Slice 55 changes IndexExpr to carry the same Argument
// shape as call/new arg lists, so modifiers (ref/out), named-args,
// and the `_` discard are admissible inside `[...]`.
//
// Note: bare `owned` was removed from ArgModifiers per §A.4 / proposal 0090
// (v0.246); use `move` to transfer ownership.
//
// Sema enforces which apply to indexer getters vs setters; the parser
// admits the full ArgList grammar at the slot.

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

BEZEL_TEST(parser_index_args, plain_single_index) {
    BEZEL_EXPECT_EQ(render("arr[i]"), std::string("arr[i]"));
}

BEZEL_TEST(parser_index_args, multi_dim_index) {
    BEZEL_EXPECT_EQ(render("arr[i, j]"), std::string("arr[i, j]"));
}

BEZEL_TEST(parser_index_args, ref_modifier_on_index) {
    BEZEL_EXPECT_EQ(
        render("arr[ref i]"),
        std::string("arr[ref i]"));
}

// per §A.4 / proposal 0090 (v0.246): bare `owned` is not an ArgModifiers
// alternative; `arr[owned r]` is a parse error. Use `move`.
BEZEL_TEST(parser_index_args, owned_modifier_on_index) {
    BEZEL_EXPECT_EQ(render("arr[owned r]"),
                    std::string("<errors>"));
}

BEZEL_TEST(parser_index_args, named_index) {
    BEZEL_EXPECT_EQ(
        render("dict[key := \"k\"]"),
        std::string("dict[key := \"k\"]"));
}

BEZEL_TEST(parser_index_args, expression_index_still_works) {
    BEZEL_EXPECT_EQ(
        render("arr[i + 1]"),
        std::string("arr[(i + 1)]"));
}

BEZEL_TEST(parser_index_args, regression_call_through_index) {
    BEZEL_EXPECT_EQ(
        render("getMap()[k]"),
        std::string("getMap()[k]"));
}
