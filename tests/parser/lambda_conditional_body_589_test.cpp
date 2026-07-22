// Spec-derived parser tests for expression-bodied lambdas whose body is a
// conditional expression (§20 / §27.3) used as an assignment RHS, a var-decl
// initializer, and a call argument.
//
// §20: the Bezel conditional expression is `trueValue if condition else
// falseValue` (level-15 precedence, right-associative).
//
// §27.3: the lambda body may be any Expression:
//   LambdaBody ::= Expression | "do" Statement* "end"
//
// Issue #589: the pre-fix version of the recursive-lambda workaround test
// (§27.5 / #579, recursive_lambda_579_test.cpp) contained the comment
// "an expression-bodied ternary lambda directly in an assignment RHS does
// not parse in this version" referring to conditional-expression-bodied
// lambdas like `(n: int) => 0 if n <= 0 else n` used on the right-hand
// side of an assignment.  These cases are all valid and do parse correctly.
// This file documents and guards that behaviour.
//
// Grammar (A.7):
//   LambdaExpr   ::= "async"? "(" LambdaParams? ")" "=>" LambdaBody
//   LambdaBody   ::= Expression | "do" Statement* "end"
//   Expression   ::= ConditionalExpr
//   ConditionalExpr ::= LogicalOr ("if" Expression "else" Expression)?

#include <string>

#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::exprToString;
using bezel::test::parseAll;

namespace {

// Parse `_ := EXPR;` inside a method body and return the expression string.
// Uses the DiscardStmt path so any expression is valid as the RHS.
std::string exprFromDiscard(const std::string& exprSrc) {
    const std::string src =
        "module M; class C public method M() _ := " + exprSrc +
        "; end M; end C;";
    auto r = parseAll(src);
    if (r.diagnostics.hasErrors()) return "<errors>";
    const auto& topDecls = r.sourceFile->topDecls;
    if (topDecls.empty()) return "<no-topdecls>";
    const auto* td =
        dynamic_cast<const bezel::ast::TypeDecl*>(topDecls[0].get());
    if (!td || td->members.empty()) return "<no-member>";
    const auto* md = dynamic_cast<const bezel::ast::MethodDecl*>(
        td->members[0].get());
    if (!md || md->stmts.empty()) return "<no-stmt>";
    const auto* ds =
        dynamic_cast<const bezel::ast::DiscardStmt*>(md->stmts[0].get());
    if (!ds || !ds->value) return "<not-discard>";
    return exprToString(*ds->value);
}


} // namespace

// --- §27.3 / §20: lambda with conditional expression body ------------------

// §20 / §27.3: simple conditional body — `0 if n <= 0 else n`.
BEZEL_TEST(parser_lambda_cond_589, conditional_body_basic_spec_20_27_3) {
    BEZEL_EXPECT_EQ(
        exprFromDiscard("lambda(n: int) => 0 if n <= 0 else n"),
        std::string("(lambda(n: int) => (0 if (n <= 0) else n))"));
}

// §20 / §27.3: conditional body as an assignment RHS (#589 primary case).
// `f := (n: int) => 0 if n <= 0 else n;` must parse without error.
BEZEL_TEST(parser_lambda_cond_589, conditional_body_as_assignment_rhs_spec_20_27_3) {
    const std::string src =
        "module M; class C "
        "  public method run() "
        "    var f: (int) -> int; "
        "    f := lambda(n: int) => 0 if n <= 0 else n; "
        "  end run; "
        "end C;";
    auto r = parseAll(src);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §21.1 / §27.3: conditional body in a var-decl initializer.
// `var f: (int) -> int := (n: int) => 0 if n <= 0 else n;` must parse.
BEZEL_TEST(parser_lambda_cond_589, conditional_body_as_var_decl_init_spec_21_1_27_3) {
    const std::string src =
        "module M; class C "
        "  public method run() "
        "    var f: (int) -> int := lambda(n: int) => 0 if n <= 0 else n; "
        "  end run; "
        "end C;";
    auto r = parseAll(src);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §27.3 / §27.4: conditional body as a call argument.
// `Apply((n: int) => 0 if n <= 0 else n)` must parse.
BEZEL_TEST(parser_lambda_cond_589, conditional_body_as_call_arg_spec_27_3_27_4) {
    const std::string src =
        "module M; class C "
        "  public method Apply(f: (int) -> int): int return f(1); end Apply; "
        "  public method run() "
        "    _ := Apply(lambda(n: int) => 0 if n <= 0 else n); "
        "  end run; "
        "end C;";
    auto r = parseAll(src);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §20: right-associative chained conditional in a lambda body.
// `(n: int) => "A" if n > 90 else "B" if n > 80 else "C"` — §20 says the
// form composes right-associatively.
BEZEL_TEST(parser_lambda_cond_589, chained_conditional_body_spec_20) {
    BEZEL_EXPECT_EQ(
        exprFromDiscard(
            "lambda(n: int) => \"A\" if n > 90 else \"B\" if n > 80 else \"C\""),
        std::string(
            "(lambda(n: int) => (\"A\" if (n > 90) else "
            "(\"B\" if (n > 80) else \"C\")))"));
}

// §27.5 / §27.3: expression-bodied recursive lambda — the case whose block-
// bodied form was used as a workaround in recursive_lambda_579_test.cpp.
// `(n: int) => 0 if n <= 0 else n + sum(n - 1)` is a valid lambda body.
BEZEL_TEST(parser_lambda_cond_589, recursive_self_ref_conditional_body_spec_27_5_20) {
    const std::string src =
        "module M; class C "
        "  public method run() "
        "    var sum: (int) -> int; "
        "    sum := lambda(n: int) => 0 if n <= 0 else n + sum(n - 1); "
        "  end run; "
        "end C;";
    auto r = parseAll(src);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Regression: block-bodied lambda must still parse correctly.
BEZEL_TEST(parser_lambda_cond_589, block_body_regression_spec_27_3) {
    BEZEL_EXPECT_EQ(
        exprFromDiscard("lambda(x: int) => do return x; end"),
        std::string("(lambda(x: int) => do { return x; } end)"));
}

// Regression: simple expression body (no conditional) must still work.
BEZEL_TEST(parser_lambda_cond_589, simple_expr_body_regression_spec_27_3) {
    BEZEL_EXPECT_EQ(
        exprFromDiscard("lambda(x: int) => x + 1"),
        std::string("(lambda(x: int) => (x + 1))"));
}
