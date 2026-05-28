// Spec-derived parser tests for `throw` in expression (A.7 ThrowExpr) and
// statement (A.6 ThrowStmt) positions.
//
// The conditional expression (`X if cond else Y`, §17.1 L15) is NOT
// implemented in this slice — see the note in Parser.cpp's parseExpression.
// It conflicts with statement-level `if cond then ...` because constructs
// like WhileStmt have no body-start keyword: `while cond if foo then ... end;`
// is ambiguous. A clean fix needs either a spec amendment (add a body-start
// keyword like `do` to WhileStmt) or required parens around the conditional.

#include <string>

#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::memberSummaries;
using bezel::test::parseAll;

static std::string parseInitText(const std::string& expr) {
    auto r = parseAll(std::string("module M; class Foo public x: int := ") +
                      expr + std::string("; end Foo;"));
    if (r.diagnostics.hasErrors()) return "<error>";
    const auto ms = memberSummaries(r);
    if (ms.size() != 1) return "<no member>";
    const std::string prefix = "public x: int := ";
    if (ms[0].rfind(prefix, 0) != 0) return "<bad prefix>";
    return ms[0].substr(prefix.size());
}

static std::string parseBodyText(const std::string& body) {
    auto r = parseAll(std::string("module M; class Foo public method run() ") +
                      body + std::string(" end run; end Foo;"));
    if (r.diagnostics.hasErrors()) return "<error>";
    const auto ms = memberSummaries(r);
    if (ms.size() != 1) return "<no member>";
    const std::string prefix = "public method run() ";
    if (ms[0].rfind(prefix, 0) != 0) return "<bad prefix>";
    return ms[0].substr(prefix.size());
}

// --- ThrowExpr (§A.7) ------------------------------------------------------

BEZEL_TEST(parser_throw, throw_expression_with_operand) {
    BEZEL_EXPECT_EQ(
        parseInitText("throw new ArgumentException(\"oops\")"),
        std::string("(throw new ArgumentException(\"oops\"))"));
}

BEZEL_TEST(parser_throw, throw_expression_with_identifier_operand) {
    BEZEL_EXPECT_EQ(parseInitText("throw err"),
                    std::string("(throw err)"));
}

// --- ThrowStmt (§A.6) ------------------------------------------------------

BEZEL_TEST(parser_throw, throw_statement_with_value) {
    BEZEL_EXPECT_EQ(parseBodyText("throw new Exception(\"x\");"),
                    std::string(
                        "{ throw new Exception(\"x\"); }"));
}

BEZEL_TEST(parser_throw, bare_throw_statement_parses) {
    // §A.7 / A.6 admit bare `throw` syntactically; sema enforces it's only
    // valid inside a catch arm body.
    BEZEL_EXPECT_EQ(parseBodyText("throw;"),
                    std::string("{ throw; }"));
}

BEZEL_TEST(parser_throw, throw_inside_if_branch) {
    BEZEL_EXPECT_EQ(
        parseBodyText("if cond then throw err; end;"),
        std::string("{ if cond then { throw err; } end; }"));
}
