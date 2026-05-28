// Spec-derived parser tests for parameterised lambdas — §A.7:
//
//   LambdaExpr   ::= "async"? "(" LambdaParams? ")" "=>" LambdaBody
//   LambdaParams ::= LambdaParam ("," LambdaParam)*
//   LambdaParam  ::= FuncParamMods? Identifier (":" Type)?
//   LambdaBody   ::= Expression | "do" Statement* "end"
//
// Slice 28 covered only the empty-params form `() => …`. This slice adds
// parameterised lambdas, the `async` prefix, the typed-param form, and the
// FuncParamMods modifiers on lambda parameters. Disambiguation from a
// parenthesised expression uses buffered lookahead: any `(...)` whose
// matching `)` is followed by `=>` parses as a lambda.

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
    // Reach in via the SourceFile.
    const auto& topDecls = r.sourceFile->topDecls;
    if (topDecls.empty()) return "<no-topdecls>";
    const auto* td = dynamic_cast<const bezel::ast::TypeDecl*>(topDecls[0].get());
    if (!td || td->members.empty()) return "<no-member>";
    const auto* md = dynamic_cast<const bezel::ast::MethodDecl*>(
        td->members[0].get());
    if (!md || md->stmts.empty()) return "<no-stmt>";
    // `_ := EXPR;` is a DiscardStmt per A.6, not AssignmentStmt.
    const auto* ds = dynamic_cast<const bezel::ast::DiscardStmt*>(
        md->stmts[0].get());
    if (!ds || !ds->value) return "<not-discard>";
    return exprToString(*ds->value);
}

} // namespace

BEZEL_TEST(parser_lambda, single_untyped_param) {
    BEZEL_EXPECT_EQ(
        exprFromStmt("(x) => x"),
        std::string("((x) => x)"));
}

BEZEL_TEST(parser_lambda, single_typed_param) {
    BEZEL_EXPECT_EQ(
        exprFromStmt("(x: int) => x"),
        std::string("((x: int) => x)"));
}

BEZEL_TEST(parser_lambda, multiple_typed_params) {
    BEZEL_EXPECT_EQ(
        exprFromStmt("(a: int, b: int) => a + b"),
        std::string("((a: int, b: int) => (a + b))"));
}

BEZEL_TEST(parser_lambda, mixed_typed_untyped_params) {
    BEZEL_EXPECT_EQ(
        exprFromStmt("(a, b: int) => a"),
        std::string("((a, b: int) => a)"));
}

BEZEL_TEST(parser_lambda, async_with_params) {
    BEZEL_EXPECT_EQ(
        exprFromStmt("async (token) => await token"),
        std::string("(async (token) => (await token))"));
}

BEZEL_TEST(parser_lambda, ref_param_modifier) {
    BEZEL_EXPECT_EQ(
        exprFromStmt("(ref x: int) => x"),
        std::string("((ref x: int) => x)"));
}

BEZEL_TEST(parser_lambda, owned_param_modifier) {
    BEZEL_EXPECT_EQ(
        exprFromStmt("(owned r: Resource) => r"),
        std::string("((owned r: Resource) => r)"));
}

BEZEL_TEST(parser_lambda, out_owned_param_modifier) {
    BEZEL_EXPECT_EQ(
        exprFromStmt("(out owned r: Resource) => r"),
        std::string("((out owned r: Resource) => r)"));
}

BEZEL_TEST(parser_lambda, block_body_with_params) {
    BEZEL_EXPECT_EQ(
        exprFromStmt("(x: int) => do return x; end"),
        std::string("((x: int) => do { return x; } end)"));
}

BEZEL_TEST(parser_lambda, empty_params_still_work) {
    // Regression: the new buffered-lookahead path must still admit the
    // empty-params form from slice 28.
    BEZEL_EXPECT_EQ(
        exprFromStmt("() => 42"),
        std::string("(() => 42)"));
}

BEZEL_TEST(parser_lambda, paren_expr_is_not_lambda) {
    // Discriminator: the matching `)` is followed by `+`, not `=>`, so the
    // construct parses as a parenthesised expression. The tree shape for
    // `(a + b) * c` puts the `+` inside the `*` operand without ParenExpr;
    // exprToString parenthesises Binary nodes explicitly for unambiguous
    // assertions.
    BEZEL_EXPECT_EQ(
        exprFromStmt("(a + b) * c"),
        std::string("((a + b) * c)"));
}

BEZEL_TEST(parser_lambda, paren_with_typed_inner_member_access_is_not_lambda) {
    // `(foo.bar)` — the matching `)` is followed by `+`, not `=>`.
    BEZEL_EXPECT_EQ(
        exprFromStmt("(foo.bar) + 1"),
        std::string("(foo.bar + 1)"));
}

BEZEL_TEST(parser_lambda, nested_lambda_in_call_argument) {
    // The scanner must correctly skip an inner lambda's parens when looking
    // for an outer `)`. Here the outer `f(...)` is a call, not a lambda.
    BEZEL_EXPECT_EQ(
        exprFromStmt("f((x) => x + 1)"),
        std::string("f(((x) => (x + 1)))"));
}

BEZEL_TEST(parser_lambda, lambda_returning_lambda) {
    BEZEL_EXPECT_EQ(
        exprFromStmt("(x: int) => (y: int) => x + y"),
        std::string("((x: int) => ((y: int) => (x + y)))"));
}
