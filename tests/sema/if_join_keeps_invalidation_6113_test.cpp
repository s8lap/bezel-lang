// Spec-derived regression tests for issue #6113 — a §12.7 type narrowing that
// one branch of an `if` statement invalidated must not be back in force after
// the `if`.
//
// THE RULE. Spec §12.7 (v0.338): "Reassignment within the controlled block
// invalidates the type narrowing, exactly as for nullability narrowing", and
// §7.4 "How narrowing is invalidated" (v0.338) lists the invalidating forms:
// "reassignment from a nullable-typed expression … passing the variable as a
// `ref` or `out` argument and, for a mutably captured variable, the first
// operation that can transfer control to user code". The statement after an
// `if` is reached by every branch that falls through, so a narrowing holds
// there only if it held at the end of each of them.
//
// WHAT WAS BROKEN. The `if` handler restored the entry narrowing map by
// assignment after the branches, so a branch's invalidation was undone:
//   if q is B then if k = 1 then q := alt; end if; return q.y; end if;
// compiled and read `y` off an `A` (MEASURED on 89260997 with the real
// `bezelc --sanitize=address`: heap-buffer-overflow).
//
// Reject rows assert the diagnostic the stale narrowing was hiding (`A` has no
// member `y`). Accept rows are the neighbouring shapes that must keep
// compiling, each run with both outcomes of the test.

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
public class D: B
  public constructor() end constructor;
  public w: int := 4;
end D;
public class P
  public static method Rs(ref q: A, alt: A): bool
    q := alt;
    return true;
  end Rs;
)bzl";

constexpr const char* kMain = R"bzl(
  public static method Main(): int
    var bb := new B(); var aa := new A(); var dd := new D();
    Console.WriteLine($"{T(0, bb, aa)} {T(1, bb, aa)} {T(0, dd, aa)} {T(0, aa, aa)}");
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

}  // namespace

// THE ISSUE'S REPRO: one path reassigned `q`, one did not; after the inner `if`
// `q` is an `A` again.
BEZEL_TEST(sema_if_join_keeps_invalidation_6113,
           else_less_if_that_reassigns_ends_the_narrowing_spec_12_7_issue_repro) {
    auto r = run(R"bzl(
  public static method T(k: int, o: A, alt: A): int
    var q: A := o;
    if q is B then
      if k = 1 then q := alt; end if;
      return q.y;
    end if;
    return -1;
  end T;)bzl");
    expectNoMemberY(r);
}

// An explicit `else` that does not touch `q` changes nothing: the `then` path
// still reaches the read with `q` reassigned.
BEZEL_TEST(sema_if_join_keeps_invalidation_6113,
           if_else_with_one_reassigning_branch_ends_the_narrowing_spec_12_7) {
    auto r = run(R"bzl(
  public static method T(k: int, o: A, alt: A): int
    var q: A := o;
    if q is B then
      if k = 1 then q := alt; else Console.Write(""); end if;
      return q.y;
    end if;
    return -1;
  end T;)bzl");
    expectNoMemberY(r);
}

// The reassignment sits in an `elif` branch.
BEZEL_TEST(sema_if_join_keeps_invalidation_6113,
           elif_branch_that_reassigns_ends_the_narrowing_spec_12_7) {
    auto r = run(R"bzl(
  public static method T(k: int, o: A, alt: A): int
    var q: A := o;
    if q is B then
      if k = 5 then Console.Write(""); elif k = 1 then q := alt; end if;
      return q.y;
    end if;
    return -1;
  end T;)bzl");
    expectNoMemberY(r);
}

// The invalidation propagates out through every enclosing `if` join.
BEZEL_TEST(sema_if_join_keeps_invalidation_6113,
           reassignment_two_ifs_deep_ends_the_narrowing_spec_12_7) {
    auto r = run(R"bzl(
  public static method T(k: int, o: A, alt: A): int
    var q: A := o;
    if q is B then
      if k >= 0 then if k = 1 then q := alt; end if; end if;
      return q.y;
    end if;
    return -1;
  end T;)bzl");
    expectNoMemberY(r);
}

// §7.4: "passing the variable as a `ref` or `out` argument" invalidates just as a
// reassignment does.
BEZEL_TEST(sema_if_join_keeps_invalidation_6113,
           ref_argument_in_a_branch_ends_the_narrowing_spec_7_4) {
    auto r = run(R"bzl(
  public static method T(k: int, o: A, alt: A): int
    var q: A := o;
    if q is B then
      if k = 1 then Rs(ref q, alt); end if;
      return q.y;
    end if;
    return -1;
  end T;)bzl");
    expectNoMemberY(r);
}

// ACCEPT. The reassigning branch returns, so it never reaches the read: the
// only path to `q.y` left `q` alone.
BEZEL_TEST(sema_if_join_keeps_invalidation_6113,
           branch_that_reassigns_and_then_exits_keeps_the_narrowing_spec_12_7) {
    auto r = run(R"bzl(
  public static method T(k: int, o: A, alt: A): int
    var q: A := o;
    if q is B then
      if k = 1 then q := alt; return -5; end if;
      return q.y;
    end if;
    return -1;
  end T;)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("9 -5 9 -1\n"));
}

// ACCEPT. The inner `if` narrows `q` further for its own block and invalidates
// nothing; the outer `q is B` fact is still in force after it.
BEZEL_TEST(sema_if_join_keeps_invalidation_6113,
           branch_that_narrows_further_keeps_the_outer_narrowing_spec_12_7) {
    auto r = run(R"bzl(
  public static method T(k: int, o: A, alt: A): int
    var q: A := o;
    if q is B then
      var t := 0;
      if q is D then t := q.w; end if;
      return q.y + t;
    end if;
    return -1;
  end T;)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("9 9 13 -1\n"));
}

// ACCEPT. The narrowed read comes first; the later reassignment ends the
// narrowing only for what follows it.
BEZEL_TEST(sema_if_join_keeps_invalidation_6113,
           read_before_the_reassignment_is_unaffected_spec_12_7) {
    auto r = run(R"bzl(
  public static method T(k: int, o: A, alt: A): int
    var q: A := o;
    if q is B then
      var r := q.y;
      if k = 1 then q := alt; end if;
      return r;
    end if;
    return -1;
  end T;)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("9 9 9 -1\n"));
}

// ACCEPT control: an inner `if` that never touches `q`.
BEZEL_TEST(sema_if_join_keeps_invalidation_6113,
           if_that_does_not_touch_the_variable_keeps_the_narrowing_spec_12_7) {
    auto r = run(R"bzl(
  public static method T(k: int, o: A, alt: A): int
    var q: A := o;
    if q is B then
      if k = 1 then Console.Write(""); end if;
      return q.y;
    end if;
    return -1;
  end T;)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("9 9 9 -1\n"));
}
