// Spec-derived regression tests for issue #6156 — the non-null knowledge a
// declaration or an assignment gives a nullable local must not be back in
// force after an `if` one of whose branches reassigned it from a nullable.
//
// THE RULE. Spec §7.4 (v0.338): the knowledge is established by "assignment or
// initialization from a non-nullable expression", and "How narrowing is
// invalidated" lists "reassignment from a nullable-typed expression". §7.4:
// "Narrowing is flow-sensitive: at each program point the compiler tracks, per
// local variable … whether its value is known non-null" — after an `if`, that
// is what holds on every branch that falls through.
//
// WHAT WAS BROKEN. The `if` handler restored the declaration/assignment set
// by assignment after the branches. MEASURED on 89260997 with the real
// `bezelc`: the issue's program compiled and died with SIGSEGV (rc=139).
// This is the nullable-local twin of #6113 (the type-narrowing form).
//
// Reject rows assert the §7.4 member-access diagnostic; accept rows run both
// outcomes of the branch condition and compare stdout.

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
public class P
  public static method Ns(ref s: string?): bool
    s := null;
    return true;
  end Ns;
)bzl";

constexpr const char* kMain = R"bzl(
  public static method Main(): int
    Console.WriteLine($"{T(0, "wxyz")} {T(1, "wxyz")}");
    return 0;
  end Main;
end P;
)bzl";

JitResult run(const char* body) {
    return runAot(std::string(kPrelude) + body + kMain);
}

void expectNullableMemberAccess(const JitResult& r) {
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "member access `.Length` on a nullable value requires narrowing"));
}

}  // namespace

// THE ISSUE'S REPRO (as a method taking the branch selector).
BEZEL_TEST(sema_if_join_init_narrowing_6156,
           else_less_if_reassigning_from_a_nullable_ends_the_knowledge_spec_7_4_issue_repro) {
    auto r = run(R"bzl(
  public static method T(k: int, n: string?): int
    var s: string? := "ab";
    if k = 1 then s := n; end if;
    return s.Length;
  end T;)bzl");
    expectNullableMemberAccess(r);
}

// One branch reassigns from a nullable, the other does nothing.
BEZEL_TEST(sema_if_join_init_narrowing_6156,
           if_else_with_one_reassigning_branch_ends_the_knowledge_spec_7_4) {
    auto r = run(R"bzl(
  public static method T(k: int, n: string?): int
    var s: string? := "ab";
    if k = 1 then s := n; else Console.Write(""); end if;
    return s.Length;
  end T;)bzl");
    expectNullableMemberAccess(r);
}

// The reassignment sits in an `elif` branch.
BEZEL_TEST(sema_if_join_init_narrowing_6156,
           elif_branch_reassigning_from_a_nullable_ends_the_knowledge_spec_7_4) {
    auto r = run(R"bzl(
  public static method T(k: int, n: string?): int
    var s: string? := "ab";
    if k = 5 then Console.Write(""); elif k = 1 then s := n; end if;
    return s.Length;
  end T;)bzl");
    expectNullableMemberAccess(r);
}

// The knowledge came from an ASSIGNMENT rather than the declaration.
BEZEL_TEST(sema_if_join_init_narrowing_6156,
           knowledge_from_an_assignment_is_ended_the_same_way_spec_7_4) {
    auto r = run(R"bzl(
  public static method T(k: int, n: string?): int
    var s: string? := n;
    s := "ab";
    if k = 1 then s := n; end if;
    return s.Length;
  end T;)bzl");
    expectNullableMemberAccess(r);
}

// §7.4: "passing the variable as a `ref` or `out` argument" invalidates too.
BEZEL_TEST(sema_if_join_init_narrowing_6156,
           ref_argument_in_a_branch_ends_the_knowledge_spec_7_4) {
    auto r = run(R"bzl(
  public static method T(k: int, n: string?): int
    var s: string? := "ab";
    if k = 1 then Ns(ref s); end if;
    return s.Length;
  end T;)bzl");
    expectNullableMemberAccess(r);
}

// ACCEPT. The reassigning branch returns; the only path to the read left `s`
// alone.
BEZEL_TEST(sema_if_join_init_narrowing_6156,
           branch_that_reassigns_and_then_exits_keeps_the_knowledge_spec_7_4) {
    auto r = run(R"bzl(
  public static method T(k: int, n: string?): int
    var s: string? := "ab";
    if k = 1 then s := n; return -5; end if;
    return s.Length;
  end T;)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2 -5\n"));
}

// ACCEPT. Every path assigns a non-nullable value.
BEZEL_TEST(sema_if_join_init_narrowing_6156,
           branch_reassigning_from_a_non_nullable_keeps_the_knowledge_spec_7_4) {
    auto r = run(R"bzl(
  public static method T(k: int, n: string?): int
    var s: string? := "ab";
    if k = 1 then s := "abc"; end if;
    return s.Length;
  end T;)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2 3\n"));
}

// ACCEPT control.
BEZEL_TEST(sema_if_join_init_narrowing_6156,
           if_that_does_not_touch_the_variable_keeps_the_knowledge_spec_7_4) {
    auto r = run(R"bzl(
  public static method T(k: int, n: string?): int
    var s: string? := "ab";
    if k = 1 then Console.Write(""); end if;
    return s.Length;
  end T;)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2 2\n"));
}

// ACCEPT. After the join the variable is nullable again, and a fresh null test
// narrows it as usual.
BEZEL_TEST(sema_if_join_init_narrowing_6156,
           reassigned_then_tested_again_spec_7_4) {
    auto r = run(R"bzl(
  public static method T(k: int, n: string?): int
    var s: string? := "ab";
    if k = 1 then s := n; end if;
    if s = null then return -1; end if;
    return s.Length;
  end T;)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2 4\n"));
}
