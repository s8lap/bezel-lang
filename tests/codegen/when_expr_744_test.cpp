// Spec-derived tests for #744 (stage 1) — the dedicated `when` condition-dispatch
// expression.
//
// Authority: #744 / §19.  `when Arm ("," Arm)* ","? end` is the scrutinee-less
// condition-dispatch form: each arm is a boolean condition; the first true wins.
// It is the layout-independent replacement for the bare (scrutinee-less) `case`
// form. This stage adds `when` as an additive expression construct producing the
// existing null-scrutinee CaseExpr (sema/codegen reuse the scrutinee-less path);
// migrating bare `case` and deleting the newline heuristic is a later stage.
//
// v0.289 (§19, §22.4, Appendix A.6/A.7, proposal 0122 L1): the `when`-led
// expression and statement are retired — all rows verify rejection.

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
std::string sign(int n) {
    return std::string(
        "module M; static class F public static method Main(): int "
        "  var n := ") + std::to_string(n) + "; "
        "  var s := (when n > 0 => 1, n < 0 => 2, _ => 3); "
        "  return s; end Main; end F;";
}
}  // namespace

// v0.289: the `(when …)` expression is retired.
BEZEL_TEST(when_expr_744, positive_first_arm) {
    auto r = runAot(sign(5));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "a `when`-led expression is retired"));
}

// v0.289: the `(when …)` expression is retired.
BEZEL_TEST(when_expr_744, negative_second_arm) {
    auto r = runAot(sign(-3));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "a `when`-led expression is retired"));
}

// v0.289: the `(when …)` expression is retired.
BEZEL_TEST(when_expr_744, fallthrough_discard_arm) {
    auto r = runAot(sign(0));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "a `when`-led expression is retired"));
}

// v0.289: the `(when …)` expression is retired.
BEZEL_TEST(when_expr_744, multiline_layout_independent) {
    auto r = runAot(
        "module M; static class F public static method Main(): int "
        "  var n := 7; "
        "  var r := (when "
        "    n > 100 => 10, "
        "    n > 5 => 20, "
        "    _ => 30); "
        "  return r; end Main; end F;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "a `when`-led expression is retired"));
}

// v0.289: the `when … end;` statement is retired.
BEZEL_TEST(when_expr_744, statement_form) {
    auto r = runAot(
        "module M; static class F public static method Main(): int "
        "  var n := 7; var result := 0; "
        "  when "
        "    n > 100 => do result := 1; end, "
        "    n > 5 => do result := 2; end, "
        "    _ => do result := 3; end "
        "  end; "
        "  return result; end Main; end F;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagContains(r, "a `when`-led statement is retired"));
}
