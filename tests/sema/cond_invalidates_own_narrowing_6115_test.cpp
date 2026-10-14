// Spec-derived regression tests for issue #6115 — a narrowing that a
// condition establishes and the SAME condition then invalidates is not in
// force in the block the condition controls.
//
// THE RULE. Spec §7.4 "How narrowing is invalidated" (v0.338): the narrowing
// ends at "reassignment from a nullable-typed expression … passing the
// variable as a `ref` or `out` argument and, for a mutably captured variable,
// the first operation that can transfer control to user code"; §12.7 (v0.338)
// applies the same list to a type narrowing ("exactly as for nullability
// narrowing"). In `q is B and Rs(ref q, alt)` the right operand runs after
// the test and before the controlled block, so the block is entered with the
// narrowing already ended.
//
// WHAT WAS BROKEN. The facts a condition gives its block were collected from
// the condition's SHAPE after the whole condition had been visited, so the
// right operand's invalidation was not seen: `if q is B and Rs(ref q, alt)
// then return q.y; end if;` compiled and read `y` off an `A` (MEASURED on
// 89260997 with the real `bezelc --sanitize=address`: heap-buffer-overflow;
// the null form `s <> null and Ns(ref s)` then `s.Length`: SEGV). The `if`
// handler also reset the null set to the statement's entry after visiting a
// condition, which resurrected an OUTER fact the condition had invalidated.
//
// Reject rows assert the diagnostic the stale fact was hiding. Accept rows are
// the neighbouring valid shapes — the test FOLLOWS the invalidating operand —
// and compare stdout over three argument pairs.

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

constexpr const char* kPrelude = R"bzl(module M;
public class A
  public constructor() end constructor;
  public x: int := 1;
end A;
public class B: A
  public constructor() end constructor;
  public y: int := 9;
end B;
public class P
  public static method Rs(ref q: A, alt: A): bool
    q := alt;
    return true;
  end Rs;
  public static method Ro(out q: A, alt: A): bool
    q := alt;
    return true;
  end Ro;
  public static method Ns(ref s: string?): bool
    s := null;
    return true;
  end Ns;
  public static method Same(a: A, b: A): bool => true;
  public static method Pick(b: bool, v: int): int => v;
)bzl";

constexpr const char* kMain = R"bzl(
  public static method Main(): int
    var bb := new B(); var aa := new A();
    Console.WriteLine($"{T(bb, aa)} {T(aa, bb)} {T(bb, bb)}");
    return 0;
  end Main;
end P;
)bzl";

JitResult run(const char* body) {
    return runAot(std::string(kPrelude) + body + kMain);
}

void expectNoMemberY(const JitResult& r) {
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "no member `y` on type `A`"));
}

void expectNullableMemberAccess(const JitResult& r) {
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "member access `.Length` on a nullable value requires narrowing"));
}

}  // namespace

// THE ISSUE'S REPRO.
BEZEL_TEST(sema_cond_invalidates_own_narrowing_6115,
           ref_argument_after_the_type_test_in_an_if_condition_spec_7_4_issue_repro) {
    auto r = run(R"bzl(
  public static method T(o: A, alt: A): int
    var q: A := o; var s: string? := "ab";
    if q is B and Rs(ref q, alt) then return q.y; end if;
    return -9;
  end T;)bzl");
    expectNoMemberY(r);
}

// `out` invalidates like `ref`.
BEZEL_TEST(sema_cond_invalidates_own_narrowing_6115,
           out_argument_after_the_type_test_in_an_if_condition_spec_7_4) {
    auto r = run(R"bzl(
  public static method T(o: A, alt: A): int
    var q: A := o; var s: string? := "ab";
    if q is B and Ro(out q, alt) then return q.y; end if;
    return -9;
  end T;)bzl");
    expectNoMemberY(r);
}

// §7.4: "for a mutably captured variable, the first operation that can transfer
// control to user code" — the call of the capturing lambda.
BEZEL_TEST(sema_cond_invalidates_own_narrowing_6115,
           call_after_the_type_test_of_a_mutably_captured_local_spec_7_4) {
    auto r = run(R"bzl(
  public static method T(o: A, alt: A): int
    var q: A := o; var s: string? := "ab";
    var mutb := lambda(): bool => Rs(ref q, alt);
    if q is B and mutb() then return q.y; end if;
    return -9;
  end T;)bzl");
    expectNoMemberY(r);
}

// Parentheses change nothing.
BEZEL_TEST(sema_cond_invalidates_own_narrowing_6115,
           parenthesized_operands_spec_7_4) {
    auto r = run(R"bzl(
  public static method T(o: A, alt: A): int
    var q: A := o; var s: string? := "ab";
    if (q is B) and (Rs(ref q, alt)) then return q.y; end if;
    return -9;
  end T;)bzl");
    expectNoMemberY(r);
}

// The `ref` argument sits inside an argument of the right operand.
BEZEL_TEST(sema_cond_invalidates_own_narrowing_6115,
           ref_argument_nested_in_another_call_spec_7_4) {
    auto r = run(R"bzl(
  public static method T(o: A, alt: A): int
    var q: A := o; var s: string? := "ab";
    if q is B and Pick(Rs(ref q, alt), 1) = 1 then return q.y; end if;
    return -9;
  end T;)bzl");
    expectNoMemberY(r);
}

// Both tests precede the invalidation: neither fact survives it.
BEZEL_TEST(sema_cond_invalidates_own_narrowing_6115,
           two_tests_then_the_ref_argument_spec_7_4) {
    auto r = run(R"bzl(
  public static method T(o: A, alt: A): int
    var q: A := o; var s: string? := "ab";
    if q is A and q is B and Rs(ref q, alt) then return q.y; end if;
    return -9;
  end T;)bzl");
    expectNoMemberY(r);
}

// The same condition on an `elif`.
BEZEL_TEST(sema_cond_invalidates_own_narrowing_6115,
           elif_condition_spec_7_4) {
    auto r = run(R"bzl(
  public static method T(o: A, alt: A): int
    var q: A := o; var s: string? := "ab";
    if alt is B then return -3; elif q is B and Rs(ref q, alt) then return q.y; end if;
    return -9;
  end T;)bzl");
    expectNoMemberY(r);
}

// §7.4 names "a `while` condition" among the establishing positions; the same
// invalidation applies to it.
BEZEL_TEST(sema_cond_invalidates_own_narrowing_6115,
           while_condition_spec_7_4) {
    auto r = run(R"bzl(
  public static method T(o: A, alt: A): int
    var q: A := o; var s: string? := "ab";
    while q is B and Rs(ref q, alt) do return q.y; end while;
    return -9;
  end T;)bzl");
    expectNoMemberY(r);
}

// The conditional expression's `then` value.
BEZEL_TEST(sema_cond_invalidates_own_narrowing_6115,
           conditional_expression_spec_7_4) {
    auto r = run(R"bzl(
  public static method T(o: A, alt: A): int
    var q: A := o; var s: string? := "ab";
    return (if q is B and Rs(ref q, alt) then q.y else -9);
  end T;)bzl");
    expectNoMemberY(r);
}

// The condition invalidates a fact established OUTSIDE it; the `else` branch
// (the call ran and returned false) must not get the outer fact back.
BEZEL_TEST(sema_cond_invalidates_own_narrowing_6115,
           outer_narrowing_ended_by_an_inner_condition_else_branch_spec_7_4) {
    auto r = run(R"bzl(
  public static method T(o: A, alt: A): int
    var q: A := o; var s: string? := "ab";
    if q is B then
      if Rs(ref q, alt) and alt is B then return -3; else return q.y; end if;
    end if;
    return -9;
  end T;)bzl");
    expectNoMemberY(r);
}

// …nor the branch of a LATER `elif`, reached only after the first condition ran.
BEZEL_TEST(sema_cond_invalidates_own_narrowing_6115,
           outer_narrowing_ended_by_an_elif_condition_spec_7_4) {
    auto r = run(R"bzl(
  public static method T(o: A, alt: A): int
    var q: A := o; var s: string? := "ab";
    if q is B then
      if Rs(ref q, alt) and alt is B then return -3; elif alt is A then return q.y; end if;
    end if;
    return -9;
  end T;)bzl");
    expectNoMemberY(r);
}

// The null-fact twin.
BEZEL_TEST(sema_cond_invalidates_own_narrowing_6115,
           ref_argument_after_the_null_test_in_an_if_condition_spec_7_4) {
    auto r = run(R"bzl(
  public static method T(o: A, alt: A): int
    var q: A := o; var s: string? := "ab";
    if s <> null and Ns(ref s) then return s.Length; end if;
    return -9;
  end T;)bzl");
    expectNullableMemberAccess(r);
}

// `or`: the surviving path is the one where both operands were false — the
// right operand ran after the left one's test there too.
BEZEL_TEST(sema_cond_invalidates_own_narrowing_6115,
           ref_argument_after_the_null_test_in_an_or_condition_spec_7_4) {
    auto r = run(R"bzl(
  public static method T(o: A, alt: A): int
    var q: A := o; var s: string? := "ab";
    if s = null or not Ns(ref s) then return -9; end if;
    return s.Length;
  end T;)bzl");
    expectNullableMemberAccess(r);
}

// An outer null fact invalidated inside an inner condition stays invalidated in
// that condition's own `then` branch.
BEZEL_TEST(sema_cond_invalidates_own_narrowing_6115,
           outer_null_fact_ended_by_an_inner_condition_spec_7_4) {
    auto r = run(R"bzl(
  public static method T(o: A, alt: A): int
    var q: A := o; var s: string? := "ab";
    if s <> null then
      if Ns(ref s) then return s.Length; end if;
    end if;
    return -9;
  end T;)bzl");
    expectNullableMemberAccess(r);
}

// …and in its `else` branch.
BEZEL_TEST(sema_cond_invalidates_own_narrowing_6115,
           outer_null_fact_ended_by_an_inner_condition_else_branch_spec_7_4) {
    auto r = run(R"bzl(
  public static method T(o: A, alt: A): int
    var q: A := o; var s: string? := "ab";
    if s <> null then
      if Ns(ref s) and alt is B then return -3; else return s.Length; end if;
    end if;
    return -9;
  end T;)bzl");
    expectNullableMemberAccess(r);
}

// ACCEPT. The test follows the invalidating operand, so it describes the value
// the block sees.
BEZEL_TEST(sema_cond_invalidates_own_narrowing_6115,
           type_test_after_the_ref_argument_narrows_spec_7_4) {
    auto r = run(R"bzl(
  public static method T(o: A, alt: A): int
    var q: A := o; var s: string? := "ab";
    if Rs(ref q, alt) and q is B then return q.y; end if;
    return -9;
  end T;)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("-9 9 9\n"));
}

// ACCEPT. Same, with the mutably-capturing lambda called first.
BEZEL_TEST(sema_cond_invalidates_own_narrowing_6115,
           type_test_after_the_capturing_call_narrows_spec_7_4) {
    auto r = run(R"bzl(
  public static method T(o: A, alt: A): int
    var q: A := o; var s: string? := "ab";
    var mutb := lambda(): bool => Rs(ref q, alt);
    if mutb() and q is B then return q.y; end if;
    return -9;
  end T;)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("-9 9 9\n"));
}

// ACCEPT. The first test's fact is ended by the call; the test repeated after it
// establishes a fresh one.
BEZEL_TEST(sema_cond_invalidates_own_narrowing_6115,
           test_repeated_after_the_ref_argument_narrows_spec_7_4) {
    auto r = run(R"bzl(
  public static method T(o: A, alt: A): int
    var q: A := o; var s: string? := "ab";
    if q is B and Rs(ref q, alt) and q is B then return q.y; end if;
    return -9;
  end T;)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("-9 -9 9\n"));
}

// ACCEPT control: a by-value argument cannot rebind the caller's local.
BEZEL_TEST(sema_cond_invalidates_own_narrowing_6115,
           by_value_argument_does_not_invalidate_spec_7_4) {
    auto r = run(R"bzl(
  public static method T(o: A, alt: A): int
    var q: A := o; var s: string? := "ab";
    if q is B and Same(q, alt) then return q.y; end if;
    return -9;
  end T;)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("9 -9 9\n"));
}

// ACCEPT. Null-fact control: the test follows the `ref` argument (which always
// nulls `s` here, so the guarded read never runs).
BEZEL_TEST(sema_cond_invalidates_own_narrowing_6115,
           null_test_after_the_ref_argument_narrows_spec_7_4) {
    auto r = run(R"bzl(
  public static method T(o: A, alt: A): int
    var q: A := o; var s: string? := "ab";
    if Ns(ref s) and s <> null then return s.Length; end if;
    return -9;
  end T;)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("-9 -9 -9\n"));
}
