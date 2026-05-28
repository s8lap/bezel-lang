// Spec-derived parser tests for `throw` in expression and statement
// positions, and the conditional expression `X if cond else Y` (§17.1 L15).
//
// The conditional was previously deferred over a perceived spec ambiguity;
// turns out my parser was just missing the `do` body-start keyword that
// WhileStmt already requires per spec A.6. With that fixed, naive `if`-
// suffix parsing on every expression can't gobble a statement's `if` — it'd
// fail to find `else` and report a clear parse error.

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

// --- Conditional expression (§17.1 L15) ------------------------------------

BEZEL_TEST(parser_conditional, simple) {
    BEZEL_EXPECT_EQ(parseInitText("a if cond else b"),
                    std::string("(a if cond else b)"));
}

BEZEL_TEST(parser_conditional, with_complex_branches) {
    BEZEL_EXPECT_EQ(parseInitText("a + 1 if cond else b * 2"),
                    std::string("((a + 1) if cond else (b * 2))"));
}

// §17.1: conditional is right-associative.
BEZEL_TEST(parser_conditional, right_associative) {
    BEZEL_EXPECT_EQ(
        parseInitText("a if c1 else b if c2 else d"),
        std::string("(a if c1 else (b if c2 else d))"));
}

BEZEL_TEST(parser_conditional, throw_as_else_branch) {
    BEZEL_EXPECT_EQ(
        parseInitText(
            "value if value <> null else throw new Exception(\"nil\")"),
        std::string(
            "(value if (value <> null) else "
            "(throw new Exception(\"nil\")))"));
}

BEZEL_TEST(parser_conditional, missing_else_is_error) {
    auto r = parseAll(
        "module M; class Foo public x: int := a if cond; end Foo;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}
