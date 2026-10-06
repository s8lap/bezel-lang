// Spec-derived parser tests for the scrutinee-less `case` form (§19.1)
// and related fixes: #656 (enum constants in §22.8), #657 (bare-case form
// and expression arm bodies in statement-position case), per spec v0.135.
//
// §19.1 Scrutinee-less form:
//   "Scrutinee-less form — `case` with no expression, followed by arms
//   whose patterns are full boolean expressions."
//
//   CaseExpr ::= "case" Expression? CaseArm ("," CaseArm)* ","? "end"
//   (Expression? means scrutinee is optional)
//
// §19.7 / §19.2: In statement-position case arms, an Expression body is
// valid (value discarded); the § examples do NOT require a `;` before `,`.
//   "case month 1 => Console.WriteLine("Jan"), _ => Console.WriteLine("Other") end;"
//
// Issue #657: spec_0026 (enum CompareTo with `int(this) < int(other)`),
//             spec_0020 (record CompareTo with `X <> other.X`),
//             spec_0027 (case statement with multi-value arms),
//             spec_0100 (scrutinee-less grading case).

#include <string>

#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAll;

namespace {

// Helper: extract the expression from `_ := EXPR;` inside a class method.
const bezel::ast::Expr* extractExpr(const bezel::test::ParseResult& r) {
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

// Helper: extract the first statement from a class method.
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

} // namespace

// ---------------------------------------------------------------------------
// §19.1 Scrutinee-less case EXPRESSION form.
// The `case` keyword is on a different line than the first arm (line-break
// heuristic) or the arm contains a non-pattern operator.
// ---------------------------------------------------------------------------

// v0.289 (§19, §22.4, Appendix A.6/A.7, proposal 0122 L1): the `when`-led
// expression is retired. This test previously verified the when expression's AST;
// now verifies that the retired form is rejected.
BEZEL_TEST(parser_case_scrutineeless,
           spec_0100_scrutineeless_relational_arms_spec_19_1) {
    auto r = parseAll(
        "module M; class C public method M(score: int) _ := "
        "(when\n"
        "  score >= 90 => \"A\",\n"
        "  score >= 80 => \"B\",\n"
        "  _ => \"F\"\n"
        ")"
        "; end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// spec_0026 §10.5/§19.1: `int(x)` conversion expression in a case arm.
// The `int(x)` form must NOT be parsed as a TypePattern for `int`;
// it falls through to the expression catch-all (issue #657).
// (fix: primitive-keyword TypePattern requires next token != `(`)
// Tested in spec_0026_scrutineeless_int_conversion_comparison_spec_19_1 below.
// This test verifies the TypePattern fix does NOT break bare `int` as a keyword:
BEZEL_TEST(parser_case_scrutineeless,
           spec_0026_int_call_expression_not_type_pattern_fix_657) {
    // `int(x)` in expression position must parse as a call, not TypePattern.
    // The regression_primitive_type_pattern_bare_int_still_works test verifies
    // that bare `int` (without `(`) still works as a TypePattern.
    auto r = parseAll(
        "module M; class C "
        "  public method M(x: int) _ := int(x); end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// v0.289 (§19, §22.4, Appendix A.6/A.7, proposal 0122 L1): the `when`-led
// expression is retired. This test previously verified the when expression's parsing;
// now verifies that the retired form is rejected.
BEZEL_TEST(parser_case_scrutineeless,
           spec_0026_scrutineeless_int_conversion_comparison_spec_19_1) {
    auto r = parseAll(
        "module M; class C "
        "  public method CompareTo(x: int, y: int): int =>\n"
        "    (when\n"
        "      int(x) < int(y) => -1,\n"
        "      int(x) > int(y) => 1,\n"
        "      _ => 0\n"
        "    ); "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// v0.289 (§19, §22.4, Appendix A.6/A.7, proposal 0122 L1): the `when`-led
// expression is retired. This test previously verified the when expression's parsing;
// now verifies that the retired form is rejected.
BEZEL_TEST(parser_case_scrutineeless,
           spec_0020_record_compareto_inequality_arms_spec_9_6_19_1) {
    auto r = parseAll(
        "module M; "
        "record Point(X: int, Y: int) "
        "  public method CompareTo(other: Point): int =>\n"
        "    (when\n"
        "      X <> other.X => 1,\n"
        "      _ => 0\n"
        "    ); "
        "end Point;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// v0.289 (§19, §22.4, Appendix A.6/A.7, proposal 0122 L1): the `when`-led
// expression is retired. This test previously verified the when expression's parsing;
// now verifies that the retired form is rejected.
BEZEL_TEST(parser_case_scrutineeless,
           scrutineeless_logical_or_arm_multiline_spec_19_1) {
    auto r = parseAll(
        "module M; class C public method M(x: int): int\n"
        "_ :=\n"
        "(when\n"
        "  x = 1 or x = 2 => 10,\n"
        "  _ => 0\n"
        ")"
        "; end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// v0.289 (§19, §22.4, Appendix A.6/A.7, proposal 0122 L1): the `when`-led
// expression is retired. This test previously verified the when expression's parsing;
// now verifies that the retired form is rejected.
BEZEL_TEST(parser_case_scrutineeless,
           scrutineeless_logical_or_arm_same_line_spec_19_1) {
    auto r = parseAll(
        "module M; class C public method M(x: int): int "
        "_ := (when x = 1 or x = 2 => 10, _ => 0)"
        "; end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §19.1 same-line scrutinee form with integer constant arm — NOT scrutinee-less.
// `case x = 1 => 10` on one line is ambiguous without multi-line style.
// This test uses `case x 1 => 10` (constant pattern) to verify that same-line
// case with only constant patterns is correctly parsed as scrutinee form.
BEZEL_TEST(parser_case_scrutineeless,
           scrutinee_form_constant_arm_same_line_not_bare_spec_19_1) {
    // Scrutinee form: `case x  1 => 10` — scrutinee=x, arm=ConstantPattern(1).
    // Block-body method so extractExpr (stmts[0]) works correctly.
    auto r = parseAll(
        "module M; class C public method M(x: int): int "
        "_ := (case x of 1 => 10, _ => 0)"
        "; end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const auto* e = extractExpr(r);
    const auto* ce = dynamic_cast<const bezel::ast::CaseExpr*>(e);
    BEZEL_EXPECT_TRUE(ce != nullptr);
    // Scrutinee-FULL form: scrutinee is `x`.
    BEZEL_EXPECT_TRUE(ce->scrutinee != nullptr);
}

// ---------------------------------------------------------------------------
// §19.2 / §19.7: Expression arm bodies in statement-position case.
// The § examples do NOT use `;` before `,` for expression-form arm bodies.
// ---------------------------------------------------------------------------

// spec_0027 §10.6/§19.7: case statement with multi-value constant-pattern
// arms and expression bodies (no `;` required before `,`).
BEZEL_TEST(parser_case_scrutineeless,
           spec_0027_stmt_case_multi_value_arms_no_semicolon_spec_10_6_19_7) {
    // `case p  Low => Console.WriteLine("low"),  Medium, High => Console.WriteLine("...")  end;`
    // spec_0027.bzl verbatim (p is a placeholder; test parses without p in scope).
    // Note: `p` would be unresolved in sema; parser test only checks parse shape.
    auto r = parseAll(
        "module M; class C public method M(p: int) "
        "case p of "
        "  Low => Console.WriteLine(\"low\"), "
        "  Medium | High => Console.WriteLine(\"medium or high\") "
        "end; "
        " end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const auto* s = firstStmt(r);
    BEZEL_EXPECT_TRUE(s != nullptr);
    // Verify it parsed as a CaseStmt.
    const auto* cs = dynamic_cast<const bezel::ast::CaseStmt*>(s);
    BEZEL_EXPECT_TRUE(cs != nullptr);
    BEZEL_EXPECT_TRUE(cs->scrutinee != nullptr);  // scrutinee = p
    BEZEL_EXPECT_EQ(cs->arms.size(), std::size_t(2));
    // Second arm should have a 2-constant pattern (Medium, High).
    const auto* cp = dynamic_cast<const bezel::ast::ConstantPattern*>(
        cs->arms[1].pattern.get());
    BEZEL_EXPECT_TRUE(cp != nullptr);
    BEZEL_EXPECT_EQ(cp->constants.size(), std::size_t(2));
}

// §19.7 spec example: case statement with expression arms, no `;` before `,`.
BEZEL_TEST(parser_case_scrutineeless,
           stmt_case_expr_arm_body_no_semicolon_spec_19_7) {
    // Verbatim §19.7 example (module-wrapped):
    //   case month  1 => Console.WriteLine("Jan"),  _ => Console.WriteLine("Other")  end;
    auto r = parseAll(
        "module M; class C public method M(month: int) "
        "case month of "
        "  1 => Console.WriteLine(\"Jan\"), "
        "  _ => Console.WriteLine(\"Other\") "
        "end; "
        " end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §19.2 / §19.7: expression arm body WITH explicit `;` (both forms accepted).
BEZEL_TEST(parser_case_scrutineeless,
           stmt_case_expr_arm_body_with_semicolon_spec_19_2) {
    auto r = parseAll(
        "module M; class C public method M(x: int) "
        "case x of "
        "  1 => Console.WriteLine(\"one\");, "
        "  _ => Console.WriteLine(\"other\"); "
        "end; "
        " end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// Regression: primitive-type TypePattern (bare `int` without `(`) still works.
BEZEL_TEST(parser_case_scrutineeless,
           regression_primitive_type_pattern_bare_int_still_works) {
    // `case x int => 1, string => 2, _ => 0 end` must still parse correctly.
    // The fix only suppresses TypePattern when followed by `(`.
    auto r = parseAll(
        "module M; class C public method M() _ := "
        "(case x of int => 1, string => 2, _ => 0)"
        "; end M; end C;");
    // Note: `x` is unresolved in parser-only test; we just check parse success.
    // The test verifies the TypePattern branch still fires for bare `int`.
    // Parser errors only (sema not run in renderCaseExpr).
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const auto* e = extractExpr(r);
    const auto* ce = dynamic_cast<const bezel::ast::CaseExpr*>(e);
    BEZEL_EXPECT_TRUE(ce != nullptr);
    BEZEL_EXPECT_EQ(ce->arms.size(), std::size_t(3));
    // First arm pattern should be a TypePattern.
    const auto* tp = dynamic_cast<const bezel::ast::TypePattern*>(
        ce->arms[0].pattern.get());
    BEZEL_EXPECT_TRUE(tp != nullptr);
}
