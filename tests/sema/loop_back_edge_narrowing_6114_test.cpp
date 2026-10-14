// Spec-derived regression tests for issue #6114 — a narrowing carried into a
// loop does not hold at a point of the loop that the loop's own back-edge
// reaches after the narrowing was invalidated.
//
// THE RULE. Spec §7.4 (v0.338): "Narrowing is flow-sensitive: at each program
// point the compiler tracks, per local variable … whether its value is known
// non-null", and "How narrowing is invalidated": "reassignment from a
// nullable-typed expression … passing the variable as a `ref` or `out`
// argument and, for a mutably captured variable, the first operation that can
// transfer control to user code". §12.7 (v0.338): "Reassignment within the
// controlled block invalidates the type narrowing, exactly as for nullability
// narrowing". A read at the top of a loop body is reached from the loop's
// entry and from the end of the previous iteration; the fact must hold on
// both.
//
// WHAT WAS BROKEN. The loop body was checked once with the entry facts and
// nothing looked at the back-edge:
//   if q is B then while i < 2 do t := t + q.y; q := alt; i := i + 1; end while; end if;
// compiled and, on iteration 2, read `y` off an `A` (MEASURED on 89260997 with
// the real `bezelc --sanitize=address`: heap-buffer-overflow; the null form:
// SEGV).
//
// Reject rows assert the new diagnostic, reported at the read that relied on
// the fact. Accept rows are loops that reassign the same variable and must
// keep compiling — the body never relies on the carried-in fact, re-tests
// it, leaves before the back-edge, or re-establishes it on every path — and
// compare stdout over three argument sets.

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
  public static method Ns(ref s: string?, n: string?): bool
    s := n;
    return true;
  end Ns;
  public static method Step(k: int): string?
    if k > 0 then return "ab"; end if;
    return null;
  end Step;
)bzl";

constexpr const char* kMain = R"bzl(
  public static method Main(): int
    var bb := new B(); var aa := new A();
    Console.WriteLine($"{T(bb, aa, null)} {T(bb, bb, "wxyz")} {T(aa, bb, null)}");
    return 0;
  end Main;
end P;
)bzl";

JitResult run(const char* body) {
    return runAot(std::string(kPrelude) + body + kMain);
}

void expectTypeNarrowingLostOnBackEdge(const JitResult& r) {
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`q` does not have its narrowed type here: the type narrowing was "
           "established before the loop"));
}

void expectNullKnowledgeLostOnBackEdge(const JitResult& r) {
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`s` is nullable here: the knowledge that it is non-null was "
           "established before the loop"));
}

}  // namespace

// THE ISSUE'S REPRO.
BEZEL_TEST(sema_loop_back_edge_narrowing_6114,
           while_body_reassigns_below_the_read_spec_12_7_issue_repro) {
    auto r = run(R"bzl(
  public static method T(o: A, alt: A, n: string?): int
    var q: A := o; var s: string? := "ab"; var t := 0; var i := 0;
    if q is B then while i < 2 do t := t + q.y; q := alt; i := i + 1; end while; end if;
    return t;
  end T;)bzl");
    expectTypeNarrowingLostOnBackEdge(r);
}

// The same body in a `for` loop.
BEZEL_TEST(sema_loop_back_edge_narrowing_6114,
           for_body_reassigns_below_the_read_spec_12_7) {
    auto r = run(R"bzl(
  public static method T(o: A, alt: A, n: string?): int
    var q: A := o; var s: string? := "ab"; var t := 0; var i := 0;
    if q is B then for var j in 0..<2 do t := t + q.y; q := alt; end for; end if;
    return t;
  end T;)bzl");
    expectTypeNarrowingLostOnBackEdge(r);
}

// A `ref` argument invalidates like a reassignment.
BEZEL_TEST(sema_loop_back_edge_narrowing_6114,
           ref_argument_below_the_read_spec_7_4) {
    auto r = run(R"bzl(
  public static method T(o: A, alt: A, n: string?): int
    var q: A := o; var s: string? := "ab"; var t := 0; var i := 0;
    if q is B then while i < 2 do t := t + q.y; Rs(ref q, alt); i := i + 1; end while; end if;
    return t;
  end T;)bzl");
    expectTypeNarrowingLostOnBackEdge(r);
}

// …and so does an `out` argument.
BEZEL_TEST(sema_loop_back_edge_narrowing_6114,
           out_argument_below_the_read_spec_7_4) {
    auto r = run(R"bzl(
  public static method T(o: A, alt: A, n: string?): int
    var q: A := o; var s: string? := "ab"; var t := 0; var i := 0;
    if q is B then while i < 2 do t := t + q.y; Ro(out q, alt); i := i + 1; end while; end if;
    return t;
  end T;)bzl");
    expectTypeNarrowingLostOnBackEdge(r);
}

// §7.4: "for a mutably captured variable, the first operation that can transfer
// control to user code".
BEZEL_TEST(sema_loop_back_edge_narrowing_6114,
           call_of_a_capturing_closure_below_the_read_spec_7_4) {
    auto r = run(R"bzl(
  public static method T(o: A, alt: A, n: string?): int
    var q: A := o; var s: string? := "ab"; var t := 0; var i := 0;
    var mut := lambda() => do q := alt; end;
    if q is B then while i < 2 do t := t + q.y; mut(); i := i + 1; end while; end if;
    return t;
  end T;)bzl");
    expectTypeNarrowingLostOnBackEdge(r);
}

// The reassignment is conditional: the back-edge still carries a path on which
// it ran.
BEZEL_TEST(sema_loop_back_edge_narrowing_6114,
           reassignment_in_a_nested_if_below_the_read_spec_12_7) {
    auto r = run(R"bzl(
  public static method T(o: A, alt: A, n: string?): int
    var q: A := o; var s: string? := "ab"; var t := 0; var i := 0;
    if q is B then while i < 2 do t := t + q.y; if i = 0 then q := alt; end if; i := i + 1; end while; end if;
    return t;
  end T;)bzl");
    expectTypeNarrowingLostOnBackEdge(r);
}

// A `continue` is a back-edge of its own.
BEZEL_TEST(sema_loop_back_edge_narrowing_6114,
           reassignment_before_a_continue_spec_12_7) {
    auto r = run(R"bzl(
  public static method T(o: A, alt: A, n: string?): int
    var q: A := o; var s: string? := "ab"; var t := 0; var i := 0;
    if q is B then while i < 2 do i := i + 1; t := t + q.y; if i = 1 then q := alt; continue; end if; end while; end if;
    return t;
  end T;)bzl");
    expectTypeNarrowingLostOnBackEdge(r);
}

// The CONDITION is re-evaluated on the back-edge too.
BEZEL_TEST(sema_loop_back_edge_narrowing_6114,
           while_condition_relies_on_the_narrowing_spec_12_7) {
    auto r = run(R"bzl(
  public static method T(o: A, alt: A, n: string?): int
    var q: A := o; var s: string? := "ab"; var t := 0; var i := 0;
    if q is B then while q.y > i do q := alt; i := i + 1; end while; end if;
    return t;
  end T;)bzl");
    expectTypeNarrowingLostOnBackEdge(r);
}

// The read is in an inner loop that never loses the fact; the OUTER loop's
// back-edge does.
BEZEL_TEST(sema_loop_back_edge_narrowing_6114,
           inner_loop_reads_outer_loop_reassigns_spec_12_7) {
    auto r = run(R"bzl(
  public static method T(o: A, alt: A, n: string?): int
    var q: A := o; var s: string? := "ab"; var t := 0; var i := 0;
    if q is B then while i < 2 do var j := 0; while j < 1 do t := t + q.y; j := j + 1; end while; q := alt; i := i + 1; end while; end if;
    return t;
  end T;)bzl");
    expectTypeNarrowingLostOnBackEdge(r);
}

// The nullable-local form: the null test is before the loop.
BEZEL_TEST(sema_loop_back_edge_narrowing_6114,
           null_test_before_the_loop_body_reassigns_from_a_nullable_spec_7_4) {
    auto r = run(R"bzl(
  public static method T(o: A, alt: A, n: string?): int
    var q: A := o; var s: string? := "ab"; var t := 0; var i := 0;
    if s <> null then while i < 2 do t := t + s.Length; s := n; i := i + 1; end while; end if;
    return t;
  end T;)bzl");
    expectNullKnowledgeLostOnBackEdge(r);
}

// The knowledge came from the declaration's non-nullable initializer.
BEZEL_TEST(sema_loop_back_edge_narrowing_6114,
           declaration_knowledge_body_reassigns_from_a_nullable_spec_7_4) {
    auto r = run(R"bzl(
  public static method T(o: A, alt: A, n: string?): int
    var q: A := o; var s: string? := "ab"; var t := 0; var i := 0;
    while i < 2 do t := t + s.Length; s := n; i := i + 1; end while;
    return t;
  end T;)bzl");
    expectNullKnowledgeLostOnBackEdge(r);
}

// A `ref` argument below the read.
BEZEL_TEST(sema_loop_back_edge_narrowing_6114,
           ref_argument_below_the_null_narrowed_read_spec_7_4) {
    auto r = run(R"bzl(
  public static method T(o: A, alt: A, n: string?): int
    var q: A := o; var s: string? := "ab"; var t := 0; var i := 0;
    if s <> null then while i < 2 do t := t + s.Length; Ns(ref s, n); i := i + 1; end while; end if;
    return t;
  end T;)bzl");
    expectNullKnowledgeLostOnBackEdge(r);
}

// The same in a `for` loop.
BEZEL_TEST(sema_loop_back_edge_narrowing_6114,
           for_body_reassigns_from_a_nullable_spec_7_4) {
    auto r = run(R"bzl(
  public static method T(o: A, alt: A, n: string?): int
    var q: A := o; var s: string? := "ab"; var t := 0; var i := 0;
    for var j in 0..<2 do t := t + s.Length; s := n; end for;
    return t;
  end T;)bzl");
    expectNullKnowledgeLostOnBackEdge(r);
}

// ACCEPT. The body reassigns `q` and never reads it through the narrowing.
BEZEL_TEST(sema_loop_back_edge_narrowing_6114,
           loop_that_reassigns_without_relying_on_the_narrowing_spec_12_7) {
    auto r = run(R"bzl(
  public static method T(o: A, alt: A, n: string?): int
    var q: A := o; var s: string? := "ab"; var t := 0; var i := 0;
    if q is B then while i < 2 do q := alt; i := i + 1; end while; end if;
    return i;
  end T;)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2 2 0\n"));
}

// ACCEPT. The read is under a test made on the same iteration.
BEZEL_TEST(sema_loop_back_edge_narrowing_6114,
           loop_that_tests_again_inside_spec_12_7) {
    auto r = run(R"bzl(
  public static method T(o: A, alt: A, n: string?): int
    var q: A := o; var s: string? := "ab"; var t := 0; var i := 0;
    if q is B then while i < 2 do if q is B then t := t + q.y; end if; q := alt; i := i + 1; end while; end if;
    return t;
  end T;)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("9 18 0\n"));
}

// ACCEPT. The body leaves the loop after the reassignment: no back-edge carries
// the invalidated state to the read.
BEZEL_TEST(sema_loop_back_edge_narrowing_6114,
           read_reassign_then_leave_the_loop_spec_12_7) {
    auto r = run(R"bzl(
  public static method T(o: A, alt: A, n: string?): int
    var q: A := o; var s: string? := "ab"; var t := 0; var i := 0;
    if q is B then while true do t := t + q.y; q := alt; break; end while; end if;
    return t;
  end T;)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("9 9 0\n"));
}

// ACCEPT control.
BEZEL_TEST(sema_loop_back_edge_narrowing_6114,
           loop_that_does_not_touch_the_variable_spec_12_7) {
    auto r = run(R"bzl(
  public static method T(o: A, alt: A, n: string?): int
    var q: A := o; var s: string? := "ab"; var t := 0; var i := 0;
    if q is B then while i < 2 do t := t + q.y; i := i + 1; end while; end if;
    return t;
  end T;)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("18 18 0\n"));
}

// ACCEPT. Every reassignment in the body is from a non-nullable expression, so
// the knowledge holds on the back-edge (the running-maximum idiom).
BEZEL_TEST(sema_loop_back_edge_narrowing_6114,
           loop_that_reassigns_from_a_non_nullable_spec_7_4) {
    auto r = run(R"bzl(
  public static method T(o: A, alt: A, n: string?): int
    var q: A := o; var s: string? := "ab"; var t := 0; var i := 0;
    for var w in ["bcd", "e"] do if w.Length > s.Length then s := w; end if; end for;
    return s.Length;
  end T;)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("3 3 3\n"));
}

// ACCEPT. The body reassigns from a nullable and then makes the variable
// non-null again on every path before the iteration ends.
BEZEL_TEST(sema_loop_back_edge_narrowing_6114,
           loop_that_re_establishes_before_the_back_edge_spec_7_4) {
    auto r = run(R"bzl(
  public static method T(o: A, alt: A, n: string?): int
    var q: A := o; var s: string? := "ab"; var t := 0; var i := 0;
    while i < 2 do t := t + s.Length; s := n; if s = null then s := "xyz"; end if; i := i + 1; end while;
    return t;
  end T;)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("5 6 5\n"));
}

// ACCEPT. The loop condition is the test (§7.4 "a `while` condition"): it is
// re-evaluated on every iteration, so nothing is carried in.
BEZEL_TEST(sema_loop_back_edge_narrowing_6114,
           walk_under_the_loop_condition_spec_7_4) {
    auto r = run(R"bzl(
  public static method T(o: A, alt: A, n: string?): int
    var q: A := o; var s: string? := "ab"; var t := 0; var i := 0;
    var cur: string? := Step(2); var k := 2;
    while cur <> null do t := t + cur.Length; k := k - 1; cur := Step(k); end while;
    return t;
  end T;)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("4 4 4\n"));
}

// ACCEPT. Leaves the loop after the reassignment.
BEZEL_TEST(sema_loop_back_edge_narrowing_6114,
           null_read_reassign_then_leave_the_loop_spec_7_4) {
    auto r = run(R"bzl(
  public static method T(o: A, alt: A, n: string?): int
    var q: A := o; var s: string? := "ab"; var t := 0; var i := 0;
    while true do t := t + s.Length; s := n; break; end while;
    return t;
  end T;)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2 2 2\n"));
}

// ACCEPT. The read is under a null test made on the same iteration.
BEZEL_TEST(sema_loop_back_edge_narrowing_6114,
           null_test_inside_the_loop_spec_7_4) {
    auto r = run(R"bzl(
  public static method T(o: A, alt: A, n: string?): int
    var q: A := o; var s: string? := "ab"; var t := 0; var i := 0;
    while i < 2 do if s <> null then t := t + s.Length; end if; s := n; i := i + 1; end while;
    return t;
  end T;)bzl");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("2 6 2\n"));
}
