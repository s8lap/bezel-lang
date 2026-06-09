// Spec-derived parser tests for the expression-form catch arm body (§31.2.2 / #588).
//
// §31.2.2 ArmBody (§A.6 grammar):
//   ArmBody ::= Expression
//             | Statement        /* terminating `;` ends the body */
//             | "do" Statement* "end"
//
// The expression form has NO `;` — the arm separator `,` or `end`/`finally`
// follow directly. Prior to the fix for #588, the parser required `;` for
// all non-block catch arm bodies, causing a "expected ';' after expression
// statement" error on the expression form (form 1).
//
// The spec example from §31.2.2:
//   catch
//     var ex: ArgumentException when ex.ParamName = "amount" => Console.WriteLine(...),
//     InvalidOperationException => Console.WriteLine("Invalid state"),
//     var ex: IOException, TimeoutException => Retry(ex),
//     var ex: Exception => do Log(ex); throw; end
//   end;
//
// The repro from #588:
//   catch
//     var ex: OverflowException => Console.WriteLine($"Overflow: {ex.Message}"),
//     var ex: Exception => Console.WriteLine($"Other: {ex.Message}")
//   end;

#include <string>

#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAll;
using bezel::test::stmtSummary;

namespace {

const bezel::ast::TryStmt* extract(const bezel::test::ParseResult& r) {
    if (!r.sourceFile || r.sourceFile->topDecls.empty()) return nullptr;
    const auto* td = dynamic_cast<const bezel::ast::TypeDecl*>(
        r.sourceFile->topDecls[0].get());
    if (!td || td->members.empty()) return nullptr;
    const auto* md = dynamic_cast<const bezel::ast::MethodDecl*>(
        td->members[0].get());
    if (!md || md->stmts.empty()) return nullptr;
    return dynamic_cast<const bezel::ast::TryStmt*>(md->stmts[0].get());
}

bezel::test::ParseResult parse(const std::string& body) {
    return parseAll("module M; class C public method M() " + body +
                    " end M; end C;");
}

// Count catch arms in the first TryStmt.
int armCount(const std::string& body) {
    auto r = parse(body);
    const auto* t = extract(r);
    if (!t) return -1;
    return static_cast<int>(t->catches.size());
}

} // namespace

// ---------------------------------------------------------------------------
// §31.2.2: Expression form — arm body is an expression, no `;`
// ---------------------------------------------------------------------------

// Two arms, both with expression bodies (no `;`). This is the #588 repro.
// per §31.2.2 (expression form), §31.2.1 (binding pattern), §A.6 CatchBlock
BEZEL_TEST(parser_catch_588, two_expr_body_arms_no_semicolon_spec_31_2_2) {
    auto r = parse(
        "try risky(); "
        "catch "
        "  var ex: OverflowException => log(ex), "
        "  var ex: Exception => abort(ex) "
        "end;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(armCount(
        "try risky(); "
        "catch "
        "  var ex: OverflowException => log(ex), "
        "  var ex: Exception => abort(ex) "
        "end;"), 2);
}

// Single arm, expression body, no binding — no `;` required. per §31.2.2
BEZEL_TEST(parser_catch_588, single_expr_body_no_binding_no_semicolon_spec_31_2_2) {
    auto r = parse(
        "try risky(); "
        "catch IOException => log() "
        "end;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(armCount(
        "try risky(); catch IOException => log() end;"), 1);
}

// Three arms: expression, expression, expression. per §31.2.2 / §31.2.3
BEZEL_TEST(parser_catch_588, three_expr_body_arms_spec_31_2_2_31_2_3) {
    auto r = parse(
        "try risky(); "
        "catch "
        "  var ex: ArgumentException => a(ex), "
        "  var ex: IOException => b(ex), "
        "  var ex: Exception => c(ex) "
        "end;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(armCount(
        "try risky(); "
        "catch "
        "  var ex: ArgumentException => a(ex), "
        "  var ex: IOException => b(ex), "
        "  var ex: Exception => c(ex) "
        "end;"), 3);
}

// Mixed: first arm expression body (no `;`), second arm statement body (with `;`).
// per §31.2.2 (both forms are valid in the same catch block)
BEZEL_TEST(parser_catch_588, mixed_expr_and_stmt_body_arms_spec_31_2_2) {
    auto r = parse(
        "try risky(); "
        "catch "
        "  var ex: IOException => log(ex), "
        "  var ex: Exception => abort(ex); "
        "end;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(armCount(
        "try risky(); "
        "catch "
        "  var ex: IOException => log(ex), "
        "  var ex: Exception => abort(ex); "
        "end;"), 2);
}

// Bare `throw` (re-raise) as expression body — no `;`. per §31.2.2 / §31.2.4
BEZEL_TEST(parser_catch_588, expr_body_bare_throw_reraise_spec_31_2_2_31_2_4) {
    auto r = parse(
        "try risky(); "
        "catch "
        "  var ex: IOException => log(ex), "
        "  var ex: Exception => throw "
        "end;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(armCount(
        "try risky(); "
        "catch "
        "  var ex: IOException => log(ex), "
        "  var ex: Exception => throw "
        "end;"), 2);
}

// Expression body with a trailing comma (trailing comma is permitted per §A.6).
// per §A.6: CatchBlock ::= "catch" CatchArm ("," CatchArm)* ","?
BEZEL_TEST(parser_catch_588, trailing_comma_after_expr_body_spec_A_6) {
    auto r = parse(
        "try risky(); "
        "catch var ex: Exception => log(ex), "
        "end;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(armCount(
        "try risky(); catch var ex: Exception => log(ex), end;"), 1);
}

// Expression body with `when` guard. per §31.2.1 / §31.2.2
BEZEL_TEST(parser_catch_588, expr_body_with_guard_spec_31_2_1_31_2_2) {
    auto r = parse(
        "try risky(); "
        "catch "
        "  var ex: IOException when ex.IsFatal => abort(ex), "
        "  var ex: IOException => log(ex) "
        "end;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(armCount(
        "try risky(); "
        "catch "
        "  var ex: IOException when ex.IsFatal => abort(ex), "
        "  var ex: IOException => log(ex) "
        "end;"), 2);
}

// ---------------------------------------------------------------------------
// Regressions — statement-body arms (with `;`) must still work
// ---------------------------------------------------------------------------

// Single arm, statement body (with `;`). Must still parse without error.
// per §31.2.2 (statement form)
BEZEL_TEST(parser_catch_588, regression_single_stmt_body_arm_spec_31_2_2) {
    auto r = parse(
        "try risky(); catch var ex: Exception => log(ex); end;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(armCount(
        "try risky(); catch var ex: Exception => log(ex); end;"), 1);
}

// Two arms with statement bodies — the pre-#588 supported style.
// per §31.2.2 / §31.2.3
BEZEL_TEST(parser_catch_588, regression_two_stmt_body_arms_spec_31_2_2) {
    auto r = parse(
        "try risky(); "
        "catch "
        "  var ex: IOException => log(ex);, "
        "  var ex: Exception => abort(ex); "
        "end;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(armCount(
        "try risky(); "
        "catch "
        "  var ex: IOException => log(ex);, "
        "  var ex: Exception => abort(ex); "
        "end;"), 2);
}

// `do...end` block body — multi-statement body. per §31.2.2 (block form)
BEZEL_TEST(parser_catch_588, regression_do_end_block_body_spec_31_2_2) {
    auto r = parse(
        "try risky(); "
        "catch var ex: Exception => do log(ex); abort(); end "
        "end;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(armCount(
        "try risky(); "
        "catch var ex: Exception => do log(ex); abort(); end "
        "end;"), 1);
}
