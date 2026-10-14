// Spec-derived regression tests for issue #6140 — `while true do … end` with a
// `break` that leaves it was treated as a statement that never completes.
//
// THE RULE. Spec §21.7 (v0.338), "Compound statements carry no special
// grant": a compound statement "counts as exiting on every path exactly when
// no path through it completes normally". §21.5 (v0.338): after `break`,
// "execution continues at the first statement after" the loop — so a
// `while true` loop a `break` leaves DOES complete normally. §7.4 (v0.338)
// narrows the surviving path after an early exit: "after `if x = null then
// return; end;` … (or `throw`, `break`, `continue` in that branch), the
// surviving path is narrowed".
//
// WHAT WAS BROKEN. `stmtAlwaysExits` answered "exits" for any literal-true
// `while`, break or no break, so
//   if s = null then while true do if k < 9 then break; end if; end while; end if;
//   return s.Length;
// compiled and dereferenced null (MEASURED on 89260997 with the real
// `bezelc --sanitize=address`: SEGV), and a value method could end with such a
// loop and no `return` (MEASURED: accepted, returned an indeterminate 0).
//
// Accept rows are the loops that still never complete normally: no `break`,
// a `break` that only leaves a NESTED loop, a `break` naming a different
// label, a `break` inside a lambda body, and a `continue`.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

JitResult run(const char* body) {
    return runAot(std::string(R"bzl(module M;
public class P
  public static method T(k: int, s: string?): int
    var t := 0;
    )bzl") + body + R"bzl(
  end T;
  public static method Main(): int
    Console.WriteLine($"{T(0, "ab")} {T(0, null)}");
    return 0;
  end Main;
end P;
)bzl");
}

void expectNullableAccessRejected(const JitResult& r) {
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "member access `.Length` on a nullable value requires narrowing "
           "it to non-null first"));
}

void expectRan(const JitResult& r, const char* out) {
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(out) + "\n");
}

}  // namespace

// THE ISSUE'S REPRO.
BEZEL_TEST(sema_while_true_break_falls_through_6140,
           break_under_an_if_spec_7_4_issue_repro) {
    auto r = run(
        "if s = null then while true do if k < 9 then break; end if; end while; end if; return s.Length;");
    expectNullableAccessRejected(r);
}

BEZEL_TEST(sema_while_true_break_falls_through_6140,
           bare_break_spec_7_4) {
    auto r = run(
        "if s = null then while true do break; end while; end if; return s.Length;");
    expectNullableAccessRejected(r);
}

BEZEL_TEST(sema_while_true_break_falls_through_6140,
           break_after_other_statements_spec_7_4) {
    auto r = run(
        "if s = null then while true do t := t + 1; break; end while; end if; return s.Length;");
    expectNullableAccessRejected(r);
}

BEZEL_TEST(sema_while_true_break_falls_through_6140,
           break_in_an_else_branch_spec_7_4) {
    auto r = run(
        "if s = null then while true do if k > 9 then t := 1; else break; end if; end while; end if; return s.Length;");
    expectNullableAccessRejected(r);
}

// §21.5: `break outer;` inside a nested loop leaves the labeled loop.
BEZEL_TEST(sema_while_true_break_falls_through_6140,
           labeled_break_from_a_nested_loop_spec_21_5) {
    auto r = run(
        "if s = null then outer: while true do while true do break outer; end while; end outer; end if; return s.Length;");
    expectNullableAccessRejected(r);
}

BEZEL_TEST(sema_while_true_break_falls_through_6140,
           break_after_a_nested_for_spec_7_4) {
    auto r = run(
        "if s = null then while true do for var i in 0..<3 do t := t + i; end for; if t > 1 then break; end if; end while; end if; return s.Length;");
    expectNullableAccessRejected(r);
}

BEZEL_TEST(sema_while_true_break_falls_through_6140,
           break_in_a_case_arm_spec_7_4) {
    auto r = run(
        "if s = null then while true do case k of 0 => do break; end, _ => do t := 1; end end case; end while; end if; return s.Length;");
    expectNullableAccessRejected(r);
}

BEZEL_TEST(sema_while_true_break_falls_through_6140,
           break_in_a_try_body_spec_7_4) {
    auto r = run(
        "if s = null then while true do try break; catch Exception => do t := 1; end; end try; end while; end if; return s.Length;");
    expectNullableAccessRejected(r);
}

BEZEL_TEST(sema_while_true_break_falls_through_6140,
           break_in_a_catch_arm_spec_7_4) {
    auto r = run(
        "if s = null then while true do try t := 1 / k; catch Exception => break; end try; end while; end if; return s.Length;");
    expectNullableAccessRejected(r);
}

// §21.7: the loop completes normally, so the method does not return on every
// path.
BEZEL_TEST(sema_while_true_break_falls_through_6140,
           value_method_ending_in_a_loop_with_a_break_spec_21_7) {
    auto r = run(
        "while true do if k < 9 then break; end if; end while;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "method `T` has a return type but does not return a value on every "
           "path"));
}

// ACCEPT.
BEZEL_TEST(sema_while_true_break_falls_through_6140,
           loop_without_a_break_spec_21_7) {
    auto r = run(
        "if s = null then while true do if k < 9 then return -1; end if; end while; end if; return s.Length;");
    expectRan(r, "2 -1");
}

// ACCEPT. The unlabeled `break` targets the inner loop.
BEZEL_TEST(sema_while_true_break_falls_through_6140,
           break_that_leaves_only_a_nested_while_spec_21_5) {
    auto r = run(
        "if s = null then while true do while true do break; end while; return -2; end while; end if; return s.Length;");
    expectRan(r, "2 -2");
}

// ACCEPT.
BEZEL_TEST(sema_while_true_break_falls_through_6140,
           break_that_leaves_only_a_nested_for_spec_21_5) {
    auto r = run(
        "if s = null then while true do for var i in 0..<3 do if i = 1 then break; end if; end for; return -3; end while; end if; return s.Length;");
    expectRan(r, "2 -3");
}

// ACCEPT.
BEZEL_TEST(sema_while_true_break_falls_through_6140,
           break_naming_a_nested_loops_label_spec_21_5) {
    auto r = run(
        "if s = null then outer: while true do inner: while true do break inner; end inner; return -5; end outer; end if; return s.Length;");
    expectRan(r, "2 -5");
}

// ACCEPT. A lambda body is its own callable.
BEZEL_TEST(sema_while_true_break_falls_through_6140,
           break_inside_a_lambda_body_spec_21_5) {
    auto r = run(
        "if s = null then while true do var f := lambda(): int => do while true do break; end while; return 1; end; return -6 + f() - 1; end while; end if; return s.Length;");
    expectRan(r, "2 -6");
}

// ACCEPT.
BEZEL_TEST(sema_while_true_break_falls_through_6140,
           continue_does_not_leave_the_loop_spec_21_5) {
    auto r = run(
        "if s = null then while true do t := t + 1; if t < 3 then continue; end if; return -7; end while; end if; return s.Length;");
    expectRan(r, "2 -7");
}

// ACCEPT. §21.7: no path through the loop completes normally.
BEZEL_TEST(sema_while_true_break_falls_through_6140,
           value_method_ending_in_a_loop_without_a_break_spec_21_7) {
    auto r = run(
        "while true do if k < 9 then return 7; end if; end while;");
    expectRan(r, "7 7");
}
