// Codegen — the scrutinee-less `case` form (`case cond => ... end`, grammar
// `CaseExpr ::= "case" Expression? ...`; spec §19 / §A). Each arm's pattern is a
// boolean CONDITION and `_` is the catch-all; it is an ordered first-match chain
// (≈ if/elif). The parser detects the bare form by layout — the first arm on a
// different line than `case`, or an `or`/`and` before the first `=>` — so these
// sources put a newline after `case`.
//
// History: R34 found it was accepted then MISCOMPILED (the expression read back
// garbage; the statement form silently skipped all arms). First guarded with a
// clean "not yet implemented" diagnostic (b92974b), then implemented (#727):
// lowerCaseExpr/lowerCaseStmt compute each arm's test from the condition, with
// name-resolution + typecheck visiting the condition expressions.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& src) {
    auto r = runJit(src);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// #727: scrutinee-less case EXPRESSION with a literal condition — the first
// true arm wins (was garbage). `true => 42` → 42.
BEZEL_TEST(scrutineeless_case_727, case_expr_literal_condition) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var r := when\n"
        "    true => 42,\n"
        "    _ => 0\n"
        "  end; "
        "  return r; "
        "end Main; end C;"), 42);
}

// #727: a chain of variable conditions, ordered first-match. x=7 → second arm.
BEZEL_TEST(scrutineeless_case_727, case_expr_variable_condition_chain) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var x := 7; "
        "  var r := when\n"
        "    x > 10 => 1,\n"
        "    x > 5 => 2,\n"
        "    _ => 3\n"
        "  end; "
        "  return r; "  // 2
        "end Main; end C;"), 2);
}

// #727: FIRST matching arm wins even when a later condition is also true.
BEZEL_TEST(scrutineeless_case_727, case_expr_first_match_wins) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var x := 100; "
        "  var r := when\n"
        "    x > 5 => 1,\n"
        "    x > 50 => 2,\n"
        "    _ => 3\n"
        "  end; "
        "  return r; "  // 1 (x>5 matches first)
        "end Main; end C;"), 1);
}

// #727: scrutinee-less case STATEMENT form (was: silently skipped all arms).
BEZEL_TEST(scrutineeless_case_727, case_stmt_variable_condition) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var x := 3; var r := 0; "
        "  when\n"
        "    x = 1 => do r := 10; end,\n"
        "    x = 3 => do r := 30; end,\n"
        "    _ => do r := 99; end,\n"
        "  end; "
        "  return r; "  // 30
        "end Main; end C;"), 30);
}

// Regression: a normal `case` WITH a scrutinee still compiles and runs.
BEZEL_TEST(scrutineeless_case_727, case_with_scrutinee_still_works) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var x := 2; "
        "  return case x 1 => 10, 2 => 20, _ => 0 end; "
        "end Main; end C;"), 20);
}
