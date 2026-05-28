// Spec-derived parser tests for basic statements — §A.6 Statement subset:
// LocalDecl (`var`, `using var`), ReturnStmt, AssignmentStmt, ExpressionStmt.
// Control-flow statements (`if`/`while`/`for`/`do`/`try`), `throw`,
// `break`/`continue`/`yield`, discard, increment/decrement, and the
// `await using var` form all land in their own slices.
//
// Test vehicle: method block bodies, now that MethodDecl admits a sequence
// of Statements before `end Identifier? ;`.

#include <string>

#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::memberSummaries;
using bezel::test::parseAll;

// Parse `class Foo public method run() <BODY> end run; end Foo;` and return
// the rendered method summary (everything after `method run() `).
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

// --- ReturnStmt -------------------------------------------------------------

BEZEL_TEST(parser_basic_stmt, return_with_value) {
    BEZEL_EXPECT_EQ(parseBodyText("return 42;"),
                    std::string("{ return 42; }"));
}

BEZEL_TEST(parser_basic_stmt, return_without_value) {
    BEZEL_EXPECT_EQ(parseBodyText("return;"), std::string("{ return; }"));
}

BEZEL_TEST(parser_basic_stmt, return_complex_expression) {
    BEZEL_EXPECT_EQ(parseBodyText("return a + b;"),
                    std::string("{ return (a + b); }"));
}

// --- LocalDecl --------------------------------------------------------------

BEZEL_TEST(parser_basic_stmt, var_with_initializer_only) {
    // Inferred type — no `: Type` clause.
    BEZEL_EXPECT_EQ(parseBodyText("var x := 42;"),
                    std::string("{ var x := 42; }"));
}

BEZEL_TEST(parser_basic_stmt, var_with_type_only) {
    // Bare `var x: int;` is permitted by the grammar (initializer optional);
    // sema may add a "must be initialized before use" check.
    BEZEL_EXPECT_EQ(parseBodyText("var x: int;"),
                    std::string("{ var x: int; }"));
}

BEZEL_TEST(parser_basic_stmt, var_with_type_and_initializer) {
    BEZEL_EXPECT_EQ(parseBodyText("var x: int := 42;"),
                    std::string("{ var x: int := 42; }"));
}

// A.6: `using var Identifier (":" Type)? ":=" Expression ";"` — initializer
// is required for `using var`.
BEZEL_TEST(parser_basic_stmt, using_var_with_initializer) {
    BEZEL_EXPECT_EQ(
        parseBodyText("using var c := Acquire();"),
        std::string("{ using var c := Acquire(); }"));
}

BEZEL_TEST(parser_basic_stmt, using_var_without_initializer_is_error) {
    auto r = parseAll(
        "module M; class Foo public method run() "
        "using var c; end run; end Foo;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// --- AssignmentStmt ---------------------------------------------------------

BEZEL_TEST(parser_basic_stmt, simple_assignment) {
    BEZEL_EXPECT_EQ(parseBodyText("x := 42;"),
                    std::string("{ x := 42; }"));
}

BEZEL_TEST(parser_basic_stmt, member_assignment) {
    BEZEL_EXPECT_EQ(parseBodyText("obj.field := value;"),
                    std::string("{ obj.field := value; }"));
}

// §17.1 level 16: compound assignment operators.
BEZEL_TEST(parser_basic_stmt, compound_assignment_add) {
    BEZEL_EXPECT_EQ(parseBodyText("counter += 1;"),
                    std::string("{ counter += 1; }"));
}

BEZEL_TEST(parser_basic_stmt, compound_assignment_subtract) {
    BEZEL_EXPECT_EQ(parseBodyText("counter -= 1;"),
                    std::string("{ counter -= 1; }"));
}

BEZEL_TEST(parser_basic_stmt, compound_assignment_null_coalesce) {
    BEZEL_EXPECT_EQ(parseBodyText("opt ??= fallback;"),
                    std::string("{ opt ??= fallback; }"));
}

// --- ExpressionStmt (call) --------------------------------------------------

BEZEL_TEST(parser_basic_stmt, expression_statement_call) {
    BEZEL_EXPECT_EQ(parseBodyText("doSomething();"),
                    std::string("{ doSomething(); }"));
}

BEZEL_TEST(parser_basic_stmt, expression_statement_call_with_args) {
    BEZEL_EXPECT_EQ(parseBodyText("log(\"hi\", level);"),
                    std::string("{ log(\"hi\", level); }"));
}

// --- Multiple statements in sequence ---------------------------------------

BEZEL_TEST(parser_basic_stmt, sequence_of_statements) {
    BEZEL_EXPECT_EQ(
        parseBodyText("var x := 0; x := x + 1; return x;"),
        std::string("{ var x := 0; x := (x + 1); return x; }"));
}

// --- Error cases ------------------------------------------------------------

BEZEL_TEST(parser_basic_stmt, return_without_semicolon_is_error) {
    auto r = parseAll(
        "module M; class Foo public method run() "
        "return 42 end run; end Foo;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_basic_stmt, assignment_without_semicolon_is_error) {
    auto r = parseAll(
        "module M; class Foo public method run() "
        "x := 1 end run; end Foo;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(parser_basic_stmt, expression_statement_without_semicolon_is_error) {
    auto r = parseAll(
        "module M; class Foo public method run() "
        "f() g() end run; end Foo;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
