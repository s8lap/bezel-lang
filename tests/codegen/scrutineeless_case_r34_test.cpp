// Codegen — the scrutinee-less `case` form (`case cond => ... end`, grammar
// `CaseExpr ::= "case" Expression? ...`; spec §19 / §A, line "case (in
// scrutinee-less form) require bool") is valid grammar but NOT yet lowered by
// the backend. R34 discovery: it was accepted and then MISCOMPILED — the case
// expression read back uninitialized garbage (a different value each run), and
// the statement form silently skipped all arms. Until the form is implemented,
// codegen must emit a clean diagnostic rather than a silent miscompile.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runJit;

// R34: a scrutinee-less case EXPRESSION must produce a clean compile error
// (was: returned uninitialized garbage).
BEZEL_TEST(scrutineeless_case_r34, case_expr_no_scrutinee_is_clean_error) {
    JitResult r = runJit(
        "module M; class C public static method Main(): int "
        "  var r := case "
        "    true => 42, "
        "    _ => 0 "
        "  end; "
        "  return r; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// R34: a scrutinee-less case STATEMENT must produce a clean compile error
// (was: silently skipped — the arm never ran).
BEZEL_TEST(scrutineeless_case_r34, case_stmt_no_scrutinee_is_clean_error) {
    JitResult r = runJit(
        "module M; class C public static method Main(): int "
        "  var r := 0; "
        "  case "
        "    true => do r := 42; end, "
        "  end; "
        "  return r; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// Regression: a normal `case` WITH a scrutinee still compiles and runs.
BEZEL_TEST(scrutineeless_case_r34, case_with_scrutinee_still_works) {
    JitResult r = runJit(
        "module M; class C public static method Main(): int "
        "  var x := 2; "
        "  return case x 1 => 10, 2 => 20, _ => 0 end; "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 20);
}
