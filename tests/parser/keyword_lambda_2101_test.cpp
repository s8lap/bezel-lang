// Spec-derived parser tests for KEYWORD-LED lambdas — §27.3 / A.7, v0.217
// (proposal 0064 graduation; impl issue #2101):
//
//   LambdaExpr      ::= "async"? "lambda" "(" LambdaParams? ")"
//                       (":" Type)? "=>" LambdaBody
//   LambdaParams    ::= LambdaParam ("," LambdaParam)* ","?
//   LambdaParam     ::= FuncParamMods? Identifier (":" Type)? | LambdaDestructure
//   LambdaBody      ::= Expression | "do" Statement* "end"
//
// The reserved keyword `lambda` (Appendix B.1, v0.217) commits the parse at
// its first token (the second, after `async`); the pre-v0.217 spelling
// `(params) => body` is a parse error at this cutover (the targeted
// diagnostics are lit-tested in tests/lit/lambda_keyword_*_reject_2101.bzl).
// This file pins the PARSE SHAPES of the new form: the keyword head, the
// `async lambda` modifier order, the (":" Type) return-type ascription slot —
// including a function-type ascription ending before `=>` (§27.3: no Type
// production contains `=>`) — destructuring heads, and the §19.4 when-guard
// interaction that previously needed the #1256 suppression carve.

#include <string>

#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::exprToString;
using bezel::test::parseAll;

namespace {

// Render the single expression-stmt RHS of `_ := EXPR;` inside a method
// body; lets us assert directly on the expression shape.
std::string exprFromStmt(const std::string& exprSrc) {
    const std::string src =
        "module M; class C public method M() _ := " + exprSrc +
        "; end M; end C;";
    auto r = parseAll(src);
    if (r.diagnostics.hasErrors()) return "<errors>";
    const auto& topDecls = r.sourceFile->topDecls;
    if (topDecls.empty()) return "<no-topdecls>";
    const auto* td = dynamic_cast<const bezel::ast::TypeDecl*>(topDecls[0].get());
    if (!td || td->members.empty()) return "<no-member>";
    const auto* md = dynamic_cast<const bezel::ast::MethodDecl*>(
        td->members[0].get());
    if (!md || md->stmts.empty()) return "<no-stmt>";
    const auto* ds = dynamic_cast<const bezel::ast::DiscardStmt*>(
        md->stmts[0].get());
    if (!ds || !ds->value) return "<not-discard>";
    return exprToString(*ds->value);
}

// True iff the source parses without diagnostics.
bool parsesClean(const std::string& src) {
    auto r = parseAll(src);
    return !r.diagnostics.hasErrors();
}

} // namespace

// §27.3 v0.217 example 1: expression body, typed parameter.
BEZEL_TEST(parser_keyword_lambda, spec_27_3_expression_body) {
    BEZEL_EXPECT_EQ(exprFromStmt("lambda(x: int) => x * 2"),
                    std::string("(lambda(x: int) => (x * 2))"));
}

// §27.3: the empty pair spells zero parameters (no paren-free form).
BEZEL_TEST(parser_keyword_lambda, spec_27_3_zero_params_empty_pair) {
    BEZEL_EXPECT_EQ(exprFromStmt("lambda() => 42"),
                    std::string("(lambda() => 42)"));
}

// §27.4: untyped (target-inferred) parameters keep their spelling.
BEZEL_TEST(parser_keyword_lambda, spec_27_4_untyped_params) {
    BEZEL_EXPECT_EQ(exprFromStmt("lambda(x) => x"),
                    std::string("(lambda(x) => x)"));
    BEZEL_EXPECT_EQ(exprFromStmt("lambda(a, b) => a"),
                    std::string("(lambda(a, b) => a)"));
}

// §30.6 / v0.217 decision (3): `async lambda(…)` is the modifier order.
BEZEL_TEST(parser_keyword_lambda, spec_30_6_async_lambda_order) {
    BEZEL_EXPECT_EQ(exprFromStmt("async lambda(url) => url"),
                    std::string("(async lambda(url) => url)"));
}

// §27.3 v0.217: the return-type ascription slot between `)` and `=>`.
BEZEL_TEST(parser_keyword_lambda, spec_27_3_return_ascription) {
    BEZEL_EXPECT_EQ(exprFromStmt("lambda(x: int): long => x * 2"),
                    std::string("(lambda(x: int): long => (x * 2))"));
}

// §27.3 v0.217: a nullable ascription (`string?`).
BEZEL_TEST(parser_keyword_lambda, spec_27_3_nullable_ascription) {
    BEZEL_EXPECT_EQ(exprFromStmt("lambda(s: string): string? => s"),
                    std::string("(lambda(s: string): string? => s)"));
}

// §27.3 v0.217: a unit ascription spells a procedure lambda.
BEZEL_TEST(parser_keyword_lambda, spec_27_3_unit_ascription) {
    BEZEL_EXPECT_EQ(exprFromStmt("lambda(x: int): () => x"),
                    std::string("(lambda(x: int): () => x)"));
}

// §27.3 v0.217 (proposal 0064 §2's adder example): a FUNCTION-TYPE ascription
// ends before `=>` — no Type production contains `=>` (function types use
// `->`), so `(int) -> int` closes the ascription and the arrow begins the
// body, itself a lambda.
BEZEL_TEST(parser_keyword_lambda, spec_27_3_function_type_ascription) {
    BEZEL_EXPECT_EQ(
        exprFromStmt("lambda(n: int): (int) -> int => lambda(m: int) => n + m"),
        std::string("(lambda(n: int): (int) -> int => "
                    "(lambda(m: int) => (n + m)))"));
}

// §27.4 (v0.183, #1408): destructuring heads carry over verbatim.
BEZEL_TEST(parser_keyword_lambda, spec_27_4_destructuring_head) {
    BEZEL_EXPECT_EQ(exprFromStmt("lambda(Entry(k, v)) => k"),
                    std::string("(lambda(Entry(k, v)) => k)"));
}

// §27.1 (lambda parameter modifiers): FuncParamMods carry over verbatim.
BEZEL_TEST(parser_keyword_lambda, spec_27_1_owned_param_modifier) {
    BEZEL_EXPECT_EQ(exprFromStmt("lambda(owned h) => h"),
                    std::string("(lambda(owned h) => h)"));
}

// §27.3: block body (`do … end`).
BEZEL_TEST(parser_keyword_lambda, spec_27_3_block_body) {
    BEZEL_EXPECT_EQ(exprFromStmt("lambda(x: int) => do return x; end"),
                    std::string("(lambda(x: int) => do { return x; } end)"));
}

// §27.3 v0.217: ascribed block body (the divergent-return remedy spelling —
// the spec's own `classify` example shape).
BEZEL_TEST(parser_keyword_lambda, spec_27_3_ascribed_block_body) {
    BEZEL_EXPECT_EQ(
        exprFromStmt("lambda(score: int): string? => do return null; end"),
        std::string("(lambda(score: int): string? => do { return null; } end)"));
}

// §27.3 v0.217 / §19.4: "a parenthesized construct followed by `=>` (a
// when-guard arm …) can never be read as a lambda prefix" — the case that
// motivated the deleted #1256 suppression carve now parses with no special
// handling at all: the guard `(t > 0)` is a parenthesized guard expression
// and the `=>` is the case-arm separator.
BEZEL_TEST(parser_keyword_lambda, spec_19_4_paren_guard_not_lambda_prefix) {
    BEZEL_EXPECT_TRUE(parsesClean(
        "module M; class C public method M(t: int): int "
        "return (case t of _ when (t > 0) => 1, _ => 0); "
        "end M; end C;"));
}

// §27.3 v0.217: a lambda nested in a call argument inside a when-guard —
// the shape the #1256 flag's consume-on-first-primary dance existed to keep
// working — still parses (now trivially, by the keyword commit).
BEZEL_TEST(parser_keyword_lambda, spec_19_4_lambda_inside_guard_call_arg) {
    BEZEL_EXPECT_TRUE(parsesClean(
        "module M; class C public method M(xs: Array<int>, t: int): int "
        "return (case t of _ when xs.Any(lambda(y) => y > 0) => 1, _ => 0); "
        "end M; end C;"));
}

// A.7 trailing comma (§1.6, v0.214) carries over to the keyword form.
BEZEL_TEST(parser_keyword_lambda, spec_1_6_trailing_comma_params) {
    BEZEL_EXPECT_EQ(exprFromStmt("lambda(a, b,) => a"),
                    std::string("(lambda(a, b) => a)"));
}
