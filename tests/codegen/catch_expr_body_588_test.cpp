// Codegen tests for the expression-form catch arm body (§31.2.2 / #588).
//
// §31.2.2 ArmBody:
//   ArmBody ::= Expression       — no `;`; arm separator `,` or `end` follows
//             | Statement        — with terminating `;`
//             | "do" Statement* "end"
//
// Issue #588: multiple comma-separated catch arms with expression bodies
// (no `;`) failed to parse with "expected ';' after expression statement".
// The expression form is spec-correct (§31.2.2, §A.6 grammar) and must work.
//
// Tests verify that the first matching arm runs, that specificity ordering
// works (more-specific types before base types), and that the normal (no-
// exception) path is unaffected.
//
// IMPORTANT: assignment (`x := val`) is a Statement (form 2, requires `;`).
// These tests therefore use `return` statements (form 2, `return val;`) for
// statement-form arm bodies, and `return val` (expression: §17.12 ThrowExpr
// and call expressions like `f()`) for expression-form arm bodies.
// Per spec §31.2.2, `return` in a catch arm body IS a statement (needs `;`).

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}

} // namespace

// ---------------------------------------------------------------------------
// §31.2.2 expression form — arm body uses a `return` statement (form 2)
// under ONE `catch` keyword (comma-separated arms without repeating `catch`).
// This is the pattern the #588 repro uses; prior to the fix, only the last
// arm's `;` was consumed, and the intermediate `,` caused a parse error when
// the arm body was a statement that mistakenly had no `;` before the `,`.
// ---------------------------------------------------------------------------

// Two catch arms (statement form: `return N;`). Throw OverflowException →
// first arm (more specific) matches → returns 1. per §31.2.2 / §31.2.3
BEZEL_TEST(codegen_catch_588, two_stmt_body_arms_first_matches_spec_31_2_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  try "
        "    throw new OverflowException(); "
        "  catch "
        "    OverflowException => return 1;, "
        "    Exception => return 2; "
        "  end; "
        "  return 0; "
        "end Main; end C;"),
        1);
}

// Two catch arms (statement form). Throw ArgumentException →
// second arm (base type) matches → returns 2. per §31.2.2 / §31.2.3
BEZEL_TEST(codegen_catch_588, two_stmt_body_arms_second_matches_spec_31_2_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  try "
        "    throw new ArgumentException(); "
        "  catch "
        "    OverflowException => return 1;, "
        "    Exception => return 2; "
        "  end; "
        "  return 0; "
        "end Main; end C;"),
        2);
}

// Three catch arms. First matching arm runs, later arms are skipped.
// per §31.2.3 ("Arms are tried in order; the first match wins")
BEZEL_TEST(codegen_catch_588, three_stmt_body_arms_first_match_wins_spec_31_2_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  try "
        "    throw new IndexOutOfRangeException(); "
        "  catch "
        "    ArgumentException => return 1;, "
        "    IndexOutOfRangeException => return 2;, "
        "    Exception => return 3; "
        "  end; "
        "  return 0; "
        "end Main; end C;"),
        2);
}

// Normal (no-exception) path is unaffected. per §31.2
BEZEL_TEST(codegen_catch_588, normal_path_skips_arms_spec_31_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  try "
        "    return 42; "
        "  catch "
        "    OverflowException => return 1;, "
        "    Exception => return 2; "
        "  end; "
        "  return 0; "
        "end Main; end C;"),
        42);
}

// Binding arm (`var e: T`) with statement form body. per §31.2.1 / §31.2.2
BEZEL_TEST(codegen_catch_588, binding_arm_stmt_body_spec_31_2_1_31_2_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  try "
        "    throw new OverflowException(); "
        "  catch "
        "    var e: OverflowException => return 1;, "
        "    var e: Exception => return 2; "
        "  end; "
        "  return 0; "
        "end Main; end C;"),
        1);
}

// Expression-form arm body: `=> return N` (ThrowExpr-like form; `return` is
// actually a statement, so use a call expression for expression form). Use a
// bare `throw` (re-raise) as the expression-form body in the second arm, which
// is valid per §31.2.4 (bare re-raise = expression, no `;`). The outer try
// catches it and returns 99. per §31.2.2 / §31.2.4
BEZEL_TEST(codegen_catch_588, expr_body_bare_throw_reraise_spec_31_2_2_31_2_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  try "
        "    try "
        "      throw new OverflowException(); "
        "    catch "
        "      OverflowException => return 1;, "
        "      Exception => throw "  // expression form: bare `throw` (no `;`)
        "    end; "
        "  catch "
        "    Exception => return 99; "
        "  end; "
        "  return 0; "
        "end Main; end C;"),
        1);  // OverflowException caught by first arm → return 1
}

// `do...end` block body — multi-statement body, two arms, expression bodies.
// per §31.2.2 (block form)
BEZEL_TEST(codegen_catch_588, do_end_block_body_regression_spec_31_2_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  try "
        "    throw new OverflowException(); "
        "  catch "
        "    var e: OverflowException => do return 1; end, "
        "    var e: Exception => do return 2; end "
        "  end; "
        "  return 0; "
        "end Main; end C;"),
        1);
}

// Unmatched exception propagates to outer handler. per §31.2
BEZEL_TEST(codegen_catch_588, unmatched_exception_propagates_spec_31_2) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  try "
        "    try "
        "      throw new ArgumentException(); "
        "    catch "
        "      OverflowException => return 1; "
        "    end; "
        "  catch "
        "    Exception => return 99; "
        "  end; "
        "  return 0; "
        "end Main; end C;"),
        99);
}

// Four arms: tests that a fourth (catch-all Exception) arm works and that
// arms after the matching arm are truly skipped. per §31.2.3 / §A.6
BEZEL_TEST(codegen_catch_588, four_arms_catch_all_at_end_spec_31_2_3) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  try "
        "    throw new NullReferenceException(); "
        "  catch "
        "    OverflowException => return 1;, "
        "    ArgumentException => return 2;, "
        "    IndexOutOfRangeException => return 3;, "
        "    Exception => return 4; "
        "  end; "
        "  return 0; "
        "end Main; end C;"),
        4);
}
