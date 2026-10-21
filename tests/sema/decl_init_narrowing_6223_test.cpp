// Spec-derived regression tests for issue #6223 — a nullable local narrowed by
// its declaration initializer is accepted as an argument to a non-nullable
// parameter, exactly as an assignment would narrow it.
//
// THE RULE. Spec §7.4 (v0.339), "How narrowing is established", assignment
// clause: narrowing follows "assignment: after `x := expr;` where `expr`'s
// static type is non-nullable — or `expr` is itself narrowed non-null at that
// point — `x` is narrowed until invalidated … and from a declaration whose
// initializer is such an `expr` — `var x: T? := expr;` or `const x: T? := expr;`
// ([§21.1]) — which is the declaration's assignment, its narrowing invalidated
// by exactly the forms below". So `var x: string? := "s"; F(x);` narrows `x`.
//
// The forms that invalidate it are those of "How narrowing is invalidated":
// reassignment from a nullable-typed expression, a `ref` or `out` argument, and
// a mutable capture followed by an operation that can transfer control.
//
// "Narrowing inside a lambda body": a fact on a `var` local does not reach a
// lambda formed under it, and a fact a lambda body establishes holds inside it
// alone. A `const` local's fact is the one that does reach a lambda body.
//
// WHAT WAS BROKEN. MEASURED on f9c549e0 with the real `bezelc`: the issue's
// program was rejected ("the argument `x` to call to `F` is nullable but the
// target type is non-nullable"). A declaration records its fact in
// `initNarrowed_` only, and the argument check consulted `nullNarrowed_` only.
// MEASURED on the same build: the lambda-body entry and exit also ignored
// `initNarrowed_`, so `var x: string? := "s"; lambda(): int => x.Length` and a
// lambda's assignment to an outer local both compiled, which §7.4 forbids.
//
// Accept rows assert the program runs and prints the spec-stated value. Reject
// rows assert the §7.4 diagnostic and that the program did not run.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

constexpr const char* kPrelude = R"bzl(module M;
class Dog
  public method Speak(): string => "woof";
end Dog;
static class S
  public static method F(x: string): int => 1;
  public static method K(x: int): int => 2;
  public static method D(x: Dog): int => 3;
end S;
static class T
  public static method O(x: string): int => 1;
  public static method O(x: int): int => 2;
  public static method O(x: Dog): int => 3;
end T;
public class P
  public static method NS(): string => "s";
  public static method NI(): int => 4;
  public static method ND(): owned Dog => new Dog();
  public static method MS(f: bool): string?
    if f then return "s"; end;
    return null;
  end MS;
  public static method MI(f: bool): int?
    if f then return 4; end;
    return null;
  end MI;
  public static method MD(f: bool): owned Dog?
    if f then return new Dog(); end;
    return null;
  end MD;
  public static method ClearS(ref v: string?)
    v := null;
  end ClearS;
  public static method FillS(out v: string?)
    v := "z";
  end FillS;
)bzl";

constexpr const char* kTail = "end P;\n";

JitResult run(const char* body) {
    return runAot(std::string(kPrelude) + body + kTail);
}

const char* const kNullableArgDiag =
    "is nullable but the target type is non-nullable";
const char* const kNullableInitDiag =
    "the initializer for `z` is nullable but the target type is non-nullable";
const char* const kNullableMemberDiag =
    "member access `.Length` on a nullable value requires narrowing";
const char* const kCapturedMemberDiag =
    "cannot narrow `T?` accessed through a mutably-captured local";

void expectRejected(const JitResult& r, const char* diag) {
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, diag));
}

// ---- ACCEPT: a declaration initializer narrows an argument (§7.4 v0.339) -----

// The issue's repro, string. Spec §7.4 (v0.339), declaration clause.
BEZEL_TEST(sema_decl_init_narrowing_6223,
           string_literal_initializer_narrows_an_argument_spec_7_4) {
    auto r = run(R"bzl(
  public static method Main(): int
    var a: string? := "s";
    Console.WriteLine(S.F(a));
    return 0;
  end Main;
)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n"));
}

// The issue's repro, int?. Spec §7.4 (v0.339), declaration clause.
BEZEL_TEST(sema_decl_init_narrowing_6223,
           int_literal_initializer_narrows_an_argument_spec_7_4) {
    auto r = run(R"bzl(
  public static method Main(): int
    var i: int? := 4;
    Console.WriteLine(S.K(i));
    return 0;
  end Main;
)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2\n"));
}

// A non-nullable call result proves the target non-null (§7.4 v0.339).
BEZEL_TEST(sema_decl_init_narrowing_6223,
           string_call_initializer_narrows_an_argument_spec_7_4) {
    auto r = run(R"bzl(
  public static method Main(): int
    var a: string? := NS();
    Console.WriteLine(S.F(a));
    return 0;
  end Main;
)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n"));
}

// An int? call result, non-null. Spec §7.4 (v0.339), declaration clause.
BEZEL_TEST(sema_decl_init_narrowing_6223,
           int_call_initializer_narrows_an_argument_spec_7_4) {
    auto r = run(R"bzl(
  public static method Main(): int
    var i: int? := NI();
    Console.WriteLine(S.K(i));
    return 0;
  end Main;
)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2\n"));
}

// `??` with a non-null right operand is non-null (§7.7). Spec §7.4 (v0.339).
BEZEL_TEST(sema_decl_init_narrowing_6223,
           string_coalesce_initializer_narrows_an_argument_spec_7_4) {
    auto r = run(R"bzl(
  public static method Main(): int
    var a: string? := MS(false) ?? "d";
    Console.WriteLine(S.F(a));
    return 0;
  end Main;
)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n"));
}

// int? with `??` and a non-null right operand. Spec §7.4 (v0.339), §7.7.
BEZEL_TEST(sema_decl_init_narrowing_6223,
           int_coalesce_initializer_narrows_an_argument_spec_7_4) {
    auto r = run(R"bzl(
  public static method Main(): int
    var i: int? := MI(false) ?? 9;
    Console.WriteLine(S.K(i));
    return 0;
  end Main;
)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2\n"));
}

// A class reference, non-null literal initializer. Spec §7.4 (v0.339).
BEZEL_TEST(sema_decl_init_narrowing_6223,
           class_literal_initializer_narrows_an_argument_spec_7_4) {
    auto r = runAotChecked(std::string(kPrelude) + R"bzl(
  public static method Main(): int
    using var d: Dog? := new Dog();
    Console.WriteLine(S.D(d));
    return 0;
  end Main;
)bzl" + kTail);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\n"));
}

// A class reference, non-null call result. Spec §7.4 (v0.339).
BEZEL_TEST(sema_decl_init_narrowing_6223,
           class_call_initializer_narrows_an_argument_spec_7_4) {
    auto r = runAotChecked(std::string(kPrelude) + R"bzl(
  public static method Main(): int
    using var d: Dog? := ND();
    Console.WriteLine(S.D(d));
    return 0;
  end Main;
)bzl" + kTail);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3\n"));
}

// Overload resolution grades the same narrowed argument. Spec §7.4 (v0.339).
BEZEL_TEST(sema_decl_init_narrowing_6223,
           overloaded_call_with_declaration_narrowed_argument_spec_7_4) {
    auto r = run(R"bzl(
  public static method Main(): int
    var a: string? := "s";
    Console.WriteLine(T.O(a));
    return 0;
  end Main;
)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n"));
}

// A narrowed declaration initializes a non-nullable local. Spec §7.4 (v0.339)
// names the declaration's assignment; the target check is the same one an
// assignment-narrowed read passes.
BEZEL_TEST(sema_decl_init_narrowing_6223,
           narrowed_declaration_flows_to_a_non_nullable_local_spec_7_4) {
    auto r = run(R"bzl(
  public static method Main(): int
    var a: string? := "s";
    var z: string := a;
    Console.WriteLine(z);
    return 0;
  end Main;
)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("s\n"));
}

// Assignment to a non-nullable local. Spec §7.4 (v0.339), §7.1.
BEZEL_TEST(sema_decl_init_narrowing_6223,
           narrowed_declaration_flows_to_an_assignment_spec_7_4) {
    auto r = run(R"bzl(
  public static method Main(): int
    var a: string? := "s";
    var z: string := "t";
    z := a;
    Console.WriteLine(z);
    return 0;
  end Main;
)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("s\n"));
}

// `return` of a narrowed local to a non-nullable result. Spec §7.4 (v0.339).
BEZEL_TEST(sema_decl_init_narrowing_6223,
           narrowed_declaration_flows_to_a_return_spec_7_4) {
    auto r = run(R"bzl(
  public static method RS(): string
    var a: string? := "s";
    return a;
  end RS;
  public static method Main(): int
    Console.WriteLine(RS());
    return 0;
  end Main;
)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("s\n"));
}

// "or `expr` is itself narrowed non-null at that point": a narrowed read as the
// initializer. Spec §7.4 (v0.339), assignment clause.
BEZEL_TEST(sema_decl_init_narrowing_6223,
           declaration_from_a_narrowed_read_narrows_spec_7_4) {
    auto r = run(R"bzl(
  public static method Main(): int
    var y: string? := "s";
    var x: string? := y;
    Console.WriteLine(S.F(x));
    return 0;
  end Main;
)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n"));
}

// A lambda formed under the declaration but not mutating the local leaves the
// declaration's fact in force for the enclosing body. Spec §7.4 (v0.339),
// "How narrowing is invalidated" (a fact is invalidated only by the listed
// forms; forming a lambda that does not mutate is none of them).
BEZEL_TEST(sema_decl_init_narrowing_6223,
           unrelated_lambda_formation_keeps_declaration_narrowing_spec_7_4) {
    auto r = run(R"bzl(
  public static method Main(): int
    var a: string? := "s";
    var f := lambda(): int => 1;
    Console.WriteLine(S.F(a));
    return 0;
  end Main;
)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n"));
}

// A `const` local is the one fact that reaches a lambda body (§7.4 v0.339,
// "Narrowing inside a lambda body"). Control: this path already passed.
BEZEL_TEST(sema_decl_init_narrowing_6223,
           const_initializer_reaches_a_lambda_body_spec_7_4) {
    auto r = run(R"bzl(
  public static method Main(): int
    const a: string? := "s";
    var f := lambda(): int => S.F(a);
    Console.WriteLine(f());
    return 0;
  end Main;
)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n"));
}

// Control: reassignment from a non-nullable value narrows after a nullable
// initializer that did not narrow. Spec §7.4 (v0.339), assignment clause.
BEZEL_TEST(sema_decl_init_narrowing_6223,
           reassignment_from_non_nullable_narrows_after_nullable_init_spec_7_4) {
    auto r = run(R"bzl(
  public static method Main(): int
    var a: string? := MS(false);
    a := NS();
    Console.WriteLine(S.F(a));
    return 0;
  end Main;
)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("1\n"));
}

// ---- REJECT: the non-narrowing initializers and invalidations stay rejected --

// A `null` initializer never narrows. Spec §7.4 (v0.339), declaration clause.
BEZEL_TEST(sema_decl_init_narrowing_6223,
           null_string_initializer_does_not_narrow_spec_7_4) {
    auto r = run(R"bzl(
  public static method Main(): int
    var a: string? := null;
    Console.WriteLine(S.F(a));
    return 0;
  end Main;
)bzl");
    expectRejected(r, kNullableArgDiag);
}

// `null` for int?. Spec §7.4 (v0.339), declaration clause.
BEZEL_TEST(sema_decl_init_narrowing_6223,
           null_int_initializer_does_not_narrow_spec_7_4) {
    auto r = run(R"bzl(
  public static method Main(): int
    var i: int? := null;
    Console.WriteLine(S.K(i));
    return 0;
  end Main;
)bzl");
    expectRejected(r, kNullableArgDiag);
}

// `null` for a class reference. Spec §7.4 (v0.339), declaration clause.
BEZEL_TEST(sema_decl_init_narrowing_6223,
           null_class_initializer_does_not_narrow_spec_7_4) {
    auto r = run(R"bzl(
  public static method Main(): int
    using var d: Dog? := null;
    Console.WriteLine(S.D(d));
    return 0;
  end Main;
)bzl");
    expectRejected(r, kNullableArgDiag);
}

// A nullable-typed call result does not narrow. Spec §7.4 (v0.339).
BEZEL_TEST(sema_decl_init_narrowing_6223,
           nullable_call_initializer_does_not_narrow_spec_7_4) {
    auto r = run(R"bzl(
  public static method Main(): int
    var a: string? := MS(false);
    Console.WriteLine(S.F(a));
    return 0;
  end Main;
)bzl");
    expectRejected(r, kNullableArgDiag);
}

// A nullable call for int?. Spec §7.4 (v0.339), declaration clause.
BEZEL_TEST(sema_decl_init_narrowing_6223,
           nullable_call_int_initializer_does_not_narrow_spec_7_4) {
    auto r = run(R"bzl(
  public static method Main(): int
    var i: int? := MI(false);
    Console.WriteLine(S.K(i));
    return 0;
  end Main;
)bzl");
    expectRejected(r, kNullableArgDiag);
}

// Reassignment to null invalidates. Spec §7.4 (v0.339), "How narrowing is
// invalidated": "the variable is reassigned from a nullable-typed expression".
BEZEL_TEST(sema_decl_init_narrowing_6223,
           reassignment_to_null_invalidates_declaration_narrowing_spec_7_4) {
    auto r = run(R"bzl(
  public static method Main(): int
    var a: string? := "s";
    a := null;
    Console.WriteLine(S.F(a));
    return 0;
  end Main;
)bzl");
    expectRejected(r, kNullableArgDiag);
}

// Reassignment from a nullable call invalidates. Spec §7.4 (v0.339).
BEZEL_TEST(sema_decl_init_narrowing_6223,
           reassignment_from_nullable_call_invalidates_spec_7_4) {
    auto r = run(R"bzl(
  public static method Main(): int
    var i: int? := 4;
    i := MI(false);
    Console.WriteLine(S.K(i));
    return 0;
  end Main;
)bzl");
    expectRejected(r, kNullableArgDiag);
}

// A `ref` argument invalidates. Spec §7.4 (v0.339), "passed as a `ref` or `out`
// argument".
BEZEL_TEST(sema_decl_init_narrowing_6223,
           ref_argument_invalidates_declaration_narrowing_spec_7_4) {
    auto r = run(R"bzl(
  public static method Main(): int
    var a: string? := "s";
    ClearS(ref a);
    Console.WriteLine(S.F(a));
    return 0;
  end Main;
)bzl");
    expectRejected(r, kNullableArgDiag);
}

// An `out` argument invalidates. Spec §7.4 (v0.339), "passed as a `ref` or
// `out` argument" (the callee may, for `out` must, reassign it).
BEZEL_TEST(sema_decl_init_narrowing_6223,
           out_argument_invalidates_declaration_narrowing_spec_7_4) {
    auto r = run(R"bzl(
  public static method Main(): int
    var a: string? := "s";
    FillS(out a);
    Console.WriteLine(S.F(a));
    return 0;
  end Main;
)bzl");
    expectRejected(r, kNullableArgDiag);
}

// A mutable capture followed by a function-value call invalidates. Spec §7.4
// (v0.339), "the variable is mutably captured … and the block performs any
// operation that can transfer control to user code".
BEZEL_TEST(sema_decl_init_narrowing_6223,
           mutable_capture_then_call_invalidates_declaration_narrowing_spec_7_4) {
    auto r = run(R"bzl(
  public static method Main(): int
    var a: string? := "s";
    var f := lambda() => do a := null; end;
    f();
    Console.WriteLine(S.F(a));
    return 0;
  end Main;
)bzl");
    expectRejected(r, kNullableArgDiag);
}

// A declaration's fact does not reach a lambda formed under it (§7.4 v0.339,
// "Narrowing inside a lambda body"): the body reads `a` at its declared type.
BEZEL_TEST(sema_decl_init_narrowing_6223,
           declaration_fact_does_not_reach_lambda_body_argument_spec_7_4) {
    auto r = run(R"bzl(
  public static method Main(): int
    var a: string? := "s";
    var f := lambda(): int => S.F(a);
    Console.WriteLine(f());
    return 0;
  end Main;
)bzl");
    expectRejected(r, kNullableArgDiag);
}

// The same rule for a member access in the lambda body. MEASURED on f9c549e0:
// this compiled, a silent acceptance. Spec §7.4 (v0.339), "Narrowing inside a
// lambda body".
BEZEL_TEST(sema_decl_init_narrowing_6223,
           declaration_fact_does_not_reach_lambda_body_member_access_spec_7_4) {
    auto r = run(R"bzl(
  public static method Main(): int
    var a: string? := "s";
    var f := lambda(): int => a.Length;
    Console.WriteLine(f());
    return 0;
  end Main;
)bzl");
    expectRejected(r, kNullableMemberDiag);
}

// An assignment's fact on a `var` local does not reach a lambda formed under it
// either (§7.4 v0.339, "Narrowing inside a lambda body"): `x := NS()` narrows `x`
// before the formation, and the body still reads `x` at its declared type.
// MEASURED on f9c549e0: this compiled, a silent acceptance.
BEZEL_TEST(sema_decl_init_narrowing_6223,
           outer_assignment_fact_does_not_reach_lambda_body_member_access_spec_7_4) {
    auto r = run(R"bzl(
  public static method Main(): int
    var x: string? := MS(false);
    x := NS();
    var f := lambda(): int => x.Length;
    Console.WriteLine(f());
    return 0;
  end Main;
)bzl");
    expectRejected(r, kNullableMemberDiag);
}

// The argument form of the same rule (§7.4 v0.339). The assignment fact sits in
// `initNarrowed_`, which the argument check consults, so this row fails if the
// lambda entry stops dropping that set.
BEZEL_TEST(sema_decl_init_narrowing_6223,
           outer_assignment_fact_does_not_reach_lambda_body_argument_spec_7_4) {
    auto r = run(R"bzl(
  public static method Main(): int
    var x: string? := MS(false);
    x := NS();
    var f := lambda(): int => S.F(x);
    Console.WriteLine(f());
    return 0;
  end Main;
)bzl");
    expectRejected(r, kNullableArgDiag);
}

// A fact a lambda body establishes holds inside it alone (§7.4 v0.339,
// "Narrowing inside a lambda body"). The `null` initializer records no fact, so
// there is nothing for the argument check to consult after the lambda; this row
// guards that the body's assignment does not narrow the argument. MEASURED on
// f9c549e0: rejected.
BEZEL_TEST(sema_decl_init_narrowing_6223,
           lambda_body_assignment_fact_does_not_leak_to_argument_spec_7_4) {
    auto r = run(R"bzl(
  public static method Main(): int
    var a: string? := null;
    var f := lambda(): int => do a := NS(); return 1; end;
    Console.WriteLine(S.F(a));
    return 0;
  end Main;
)bzl");
    expectRejected(r, kNullableArgDiag);
}

// The same leak through a local initializer, with no call between the lambda and
// the use (a call on a captured local is a separate, pre-existing over-invalidation,
// filed separately, and would mask this rule). The body's fact must not hold after
// the lambda (§7.4 v0.339, "Narrowing inside a lambda body"); the restore of
// initNarrowed_ on exit keeps it out. MEASURED on f9c549e0: rejected. With that
// restore removed this program compiles, so the row discriminates the restore.
BEZEL_TEST(sema_decl_init_narrowing_6223,
           lambda_body_assignment_fact_does_not_leak_to_local_init_spec_7_4) {
    auto r = run(R"bzl(
  public static method Main(): int
    var a: string? := null;
    var f := lambda(): int => do a := NS(); return 1; end;
    var z: string := a;
    Console.WriteLine(z);
    return 0;
  end Main;
)bzl");
    expectRejected(r, kNullableInitDiag);
}

// The member-access form of the same leak. The lambda assigns `a`, so `a` is
// mutably captured and its member access is the captured gate's case: the fact
// the body established does not hold after the lambda (§7.4 v0.339, "Narrowing
// inside a lambda body"), so the gate rejects the access. MEASURED on f9c549e0:
// this compiled. With the restore removed it compiles and dereferences null at
// run time (SIGSEGV).
BEZEL_TEST(sema_decl_init_narrowing_6223,
           lambda_body_assignment_fact_does_not_leak_to_member_access_spec_7_4) {
    auto r = run(R"bzl(
  public static method Main(): int
    var a: string? := null;
    var f := lambda(): int => do a := NS(); return 1; end;
    var n := a.Length;
    Console.WriteLine(n);
    return 0;
  end Main;
)bzl");
    expectRejected(r, kCapturedMemberDiag);
}

// A mutable capture keeps a declaration's narrowing while no operation that can
// transfer control follows it (§7.4 v0.339, "How narrowing is invalidated": the
// capture invalidates only when the block also performs such an operation). The
// lambda is formed and never called, and the use is a declaration's initializer.
BEZEL_TEST(sema_decl_init_narrowing_6223,
           mutable_capture_without_call_keeps_declaration_narrowing_spec_7_4) {
    auto r = run(R"bzl(
  public static method Main(): int
    var a: string? := "s";
    var f := lambda(): int => do a := NS(); return 1; end;
    var z: string := a;
    Console.WriteLine(z);
    return 0;
  end Main;
)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("s\n"));
}

}  // namespace
