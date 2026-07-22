// Spec-derived parser tests for the statement-position `case` construct —
// §A.6 / §A.7. The expression-form `case` from slice 38 only admits
// Expression-body arms; the statement-form here admits the richer ArmBody
// alternatives the §A.7 ArmBody note enables only "in statement-position
// case or in a catch arm":
//
//   ArmBody ::= Expression                   /* expr-position only */
//             | Statement                    /* stmt-position only */
//             | "do" Statement* "end"        /* stmt-position only */
//
// CaseStmt ends with `;` (unlike CaseExpr which terminates at `end`).

#include <string>

#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAll;
using bezel::test::stmtSummary;

namespace {

const bezel::ast::Stmt* firstStmt(const bezel::test::ParseResult& r) {
    if (!r.sourceFile || r.sourceFile->topDecls.empty()) return nullptr;
    const auto* td = dynamic_cast<const bezel::ast::TypeDecl*>(
        r.sourceFile->topDecls[0].get());
    if (!td || td->members.empty()) return nullptr;
    const auto* md = dynamic_cast<const bezel::ast::MethodDecl*>(
        td->members[0].get());
    if (!md || md->stmts.empty()) return nullptr;
    return md->stmts[0].get();
}

std::string renderCaseStmt(const std::string& body) {
    const std::string src =
        "module M; class C public method M() " + body +
        " end M; end C;";
    auto r = parseAll(src);
    if (r.diagnostics.hasErrors()) return "<errors>";
    const auto* s = firstStmt(r);
    return s ? stmtSummary(*s) : std::string("<no-stmt>");
}

} // namespace

BEZEL_TEST(parser_case_stmt, single_stmt_arm) {
    BEZEL_EXPECT_EQ(
        renderCaseStmt("case x of 1 => return 1;, _ => return 0; end;"),
        std::string(
            "case x of { 1 => { return 1; } "
            "| _ => { return 0; } } end;"));
}

BEZEL_TEST(parser_case_stmt, do_end_arm_body) {
    BEZEL_EXPECT_EQ(
        renderCaseStmt(
            "case x of 1 => do _ := \"one\"; end, _ => do _ := \"other\"; end end;"),
        std::string(
            "case x of { 1 => { _ := \"one\"; } "
            "| _ => { _ := \"other\"; } } end;"));
}

BEZEL_TEST(parser_case_stmt, mixed_stmt_and_do_arms) {
    BEZEL_EXPECT_EQ(
        renderCaseStmt(
            "case x of 1 => return 1;, "
            "2 => do _ := x; return 2; end, "
            "_ => return 0; end;"),
        std::string(
            "case x of { 1 => { return 1; } "
            "| 2 => { _ := x; return 2; } "
            "| _ => { return 0; } } end;"));
}

BEZEL_TEST(parser_case_stmt, with_when_guard) {
    BEZEL_EXPECT_EQ(
        renderCaseStmt(
            "case x of var n: int when n > 0 => return n;, _ => return 0; end;"),
        std::string(
            "case x of { var n: int when (n > 0) => { return n; } "
            "| _ => { return 0; } } end;"));
}

BEZEL_TEST(parser_case_stmt, trailing_comma_allowed) {
    BEZEL_EXPECT_EQ(
        renderCaseStmt("case x of 1 => return 1;, _ => return 0;, end;"),
        std::string(
            "case x of { 1 => { return 1; } "
            "| _ => { return 0; } } end;"));
}

BEZEL_TEST(parser_case_stmt, destructuring_arm_with_block_body) {
    BEZEL_EXPECT_EQ(
        renderCaseStmt(
            "case x of Pair(var a, var b) => do _ := a + b; end, "
            "_ => return; end;"),
        std::string(
            "case x of { Pair(var a, var b) => { _ := (a + b); } "
            "| _ => { return; } } end;"));
}

BEZEL_TEST(parser_case_stmt, case_expression_still_works_in_expr_position) {
    // Regression: with the statement-dispatch entry, the expression-form
    // (via `_ := case ... end;`) must still take the CaseExpr path so its
    // arms allow Expression bodies.
    auto r = parseAll(
        "module M; class C "
        "public method M() _ := (case x of 1 => \"one\", _ => \"other\"); "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
