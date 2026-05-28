// Spec-derived parser tests for CollectionLiteral + DefaultExpr — §A.7:
//
//   CollectionLiteral ::= "[" (Expression ("," Expression)*)? "]"  /* sequence */
//                       | "[" DictEntry ("," DictEntry)* "]"        /* dictionary */
//   DictEntry         ::= Expression ":" Expression
//   DefaultExpr       ::= "default" "(" Type ")"
//
// The sequence-vs-dict discriminator is what follows the first element: a
// `:` puts the parser on the dictionary path; `,` / `]` keep it on the
// sequence path.

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

BEZEL_TEST(parser_collection_literal, empty_sequence) {
    BEZEL_EXPECT_EQ(render("[]"), std::string("[]"));
}

BEZEL_TEST(parser_collection_literal, single_element_sequence) {
    BEZEL_EXPECT_EQ(render("[42]"), std::string("[42]"));
}

BEZEL_TEST(parser_collection_literal, multi_element_sequence) {
    BEZEL_EXPECT_EQ(render("[1, 2, 3]"), std::string("[1, 2, 3]"));
}

BEZEL_TEST(parser_collection_literal, sequence_with_expressions) {
    BEZEL_EXPECT_EQ(
        render("[x + 1, y * 2, f(z)]"),
        std::string("[(x + 1), (y * 2), f(z)]"));
}

BEZEL_TEST(parser_collection_literal, sequence_trailing_comma) {
    BEZEL_EXPECT_EQ(render("[1, 2, 3,]"), std::string("[1, 2, 3]"));
}

BEZEL_TEST(parser_collection_literal, single_dict_entry) {
    BEZEL_EXPECT_EQ(
        render("[\"k\": 1]"),
        std::string("[\"k\": 1]"));
}

BEZEL_TEST(parser_collection_literal, multi_dict_entries) {
    BEZEL_EXPECT_EQ(
        render("[\"a\": 1, \"b\": 2, \"c\": 3]"),
        std::string("[\"a\": 1, \"b\": 2, \"c\": 3]"));
}

BEZEL_TEST(parser_collection_literal, dict_with_expression_values) {
    BEZEL_EXPECT_EQ(
        render("[\"sum\": a + b, \"prod\": a * b]"),
        std::string("[\"sum\": (a + b), \"prod\": (a * b)]"));
}

BEZEL_TEST(parser_collection_literal, dict_trailing_comma) {
    BEZEL_EXPECT_EQ(
        render("[\"a\": 1, \"b\": 2,]"),
        std::string("[\"a\": 1, \"b\": 2]"));
}

BEZEL_TEST(parser_collection_literal, nested_sequence) {
    BEZEL_EXPECT_EQ(
        render("[[1, 2], [3, 4]]"),
        std::string("[[1, 2], [3, 4]]"));
}

BEZEL_TEST(parser_default_expr, primitive_type) {
    BEZEL_EXPECT_EQ(render("default(int)"), std::string("default(int)"));
}

BEZEL_TEST(parser_default_expr, named_type) {
    BEZEL_EXPECT_EQ(
        render("default(MyType)"), std::string("default(MyType)"));
}

BEZEL_TEST(parser_default_expr, nullable_type) {
    BEZEL_EXPECT_EQ(
        render("default(int?)"), std::string("default(int?)"));
}

BEZEL_TEST(parser_default_expr, generic_type) {
    BEZEL_EXPECT_EQ(
        render("default(List<int>)"),
        std::string("default(List<int>)"));
}

BEZEL_TEST(parser_default_expr, in_expression_context) {
    BEZEL_EXPECT_EQ(
        render("default(int) + 1"),
        std::string("(default(int) + 1)"));
}
