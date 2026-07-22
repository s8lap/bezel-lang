// Spec-derived tests for #744 (stage 1) — the dedicated `when` condition-dispatch
// expression.
//
// Authority: #744 / §19. `when Arm ("," Arm)* ","? end` is the scrutinee-less
// condition-dispatch form: each arm is a boolean condition; the first true wins.
// It is the layout-independent replacement for the bare (scrutinee-less) `case`
// form. This stage adds `when` as an additive expression construct producing the
// existing null-scrutinee CaseExpr (sema/codegen reuse the scrutinee-less path);
// migrating bare `case` and deleting the newline heuristic is a later stage.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
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

// #744: first true arm wins — positive.
BEZEL_TEST(when_expr_744, positive_first_arm) {
    BEZEL_EXPECT_EQ(run(sign(5)), 1);
}

// #744: second arm — negative.
BEZEL_TEST(when_expr_744, negative_second_arm) {
    BEZEL_EXPECT_EQ(run(sign(-3)), 2);
}

// #744: the discard arm — zero.
BEZEL_TEST(when_expr_744, fallthrough_discard_arm) {
    BEZEL_EXPECT_EQ(run(sign(0)), 3);
}

// #744: a multi-line `when` (no layout-significance) selects the right arm.
BEZEL_TEST(when_expr_744, multiline_layout_independent) {
    BEZEL_EXPECT_EQ(run(
        "module M; static class F public static method Main(): int "
        "  var n := 7; "
        "  var r := (when "
        "    n > 100 => 10, "
        "    n > 5 => 20, "
        "    _ => 30); "
        "  return r; end Main; end F;"), 20);
}

// #744 (stage 2): statement-position `when` with `do … end` arm bodies.
BEZEL_TEST(when_expr_744, statement_form) {
    BEZEL_EXPECT_EQ(run(
        "module M; static class F public static method Main(): int "
        "  var n := 7; var result := 0; "
        "  when "
        "    n > 100 => do result := 1; end, "
        "    n > 5 => do result := 2; end, "
        "    _ => do result := 3; end "
        "  end; "
        "  return result; end Main; end F;"), 2);
}
