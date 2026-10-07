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
//
// v0.289 (§19, §22.4, Appendix A.6/A.7, proposal 0122 L1): the `when`-led
// expression and statement are retired. All rows now verify rejection of the
// retired form; `case_with_scrutinee_still_works` verifies the normal `case`
// with scrutinee remains functional.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

bool diagContains(const bezel::test::JitResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// v0.289 (§19, §22.4): the `(when …)` expression is retired.
BEZEL_TEST(scrutineeless_case_727, case_expr_literal_condition) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var r := (when\n"
        "    true => 42,\n"
        "    _ => 0\n"
        "  ); "
        "  return r; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "a `when`-led expression is retired"));
}

// v0.289 (§19, §22.4): the `(when …)` expression is retired.
BEZEL_TEST(scrutineeless_case_727, case_expr_variable_condition_chain) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var x := 7; "
        "  var r := (when\n"
        "    x > 10 => 1,\n"
        "    x > 5 => 2,\n"
        "    _ => 3\n"
        "  ); "
        "  return r; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "a `when`-led expression is retired"));
}

// v0.289 (§19, §22.4): the `(when …)` expression is retired.
BEZEL_TEST(scrutineeless_case_727, case_expr_first_match_wins) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var x := 100; "
        "  var r := (when\n"
        "    x > 5 => 1,\n"
        "    x > 50 => 2,\n"
        "    _ => 3\n"
        "  ); "
        "  return r; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "a `when`-led expression is retired"));
}

// v0.289 (§19, §22.4): the `when … end;` statement is retired.
BEZEL_TEST(scrutineeless_case_727, case_stmt_variable_condition) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "  var x := 3; var r := 0; "
        "  when\n"
        "    x = 1 => do r := 10; end,\n"
        "    x = 3 => do r := 30; end,\n"
        "    _ => do r := 99; end,\n"
        "  end; "
        "  return r; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "a `when`-led statement is retired"));
}

// Regression: a normal `case` WITH a scrutinee still compiles and runs.
BEZEL_TEST(scrutineeless_case_727, case_with_scrutinee_still_works) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var x := 2; "
        "  return (case x of 1 => 10, 2 => 20, _ => 0); "
        "end Main; end C;"), 20);
}
