// Spec-derived regression tests for issue #2597 — a narrowing fact rooted in a
// `const` LOCAL was dropped at the lambda boundary, rejecting code §7.4
// explicitly declares well-typed.
//
// §7.4 (v0.236, "Member access on nullable values"), verbatim: "A `const`
// local's narrowing (§21.1), once established, is **never invalidated**: no
// invalidating form can apply to it — it cannot be reassigned, cannot be passed
// as `ref` or `out`, and cannot be mutably captured (assignment to a `const` is
// a compile error everywhere, lambda bodies included). A null test on a `const`
// local therefore narrows it for the remainder of its scope, across calls and
// `await`s."
//
// The #2259 fix (the immediately preceding work in this area) dropped ALL
// local-rooted facts on the way INTO a lambda body, because §27.5 capture is by
// REFERENCE — "Modifications to a captured variable inside the lambda are
// visible outside, and vice versa" — so the enclosing scope can rebind a
// captured `var`'s cell between closure creation and invocation, invalidating a
// fact the body already type-checked against (#2259 SIGSEGV'd on exactly that).
// That reasoning is correct for every root EXCEPT a `const`, which has no such
// writer. #2259's own code comment recorded the carve-out as sound but skipped
// it for uniformity with the field set; this is that carve-out.
//
// POLARITY NOTE. This fix LIFTS a rejection, so the dominant risk is admitting
// something unsound — the REJECT block below is therefore the load-bearing half
// of this file, not the ACCEPT block. Those tests pass both before and after the
// fix by design: they exist to prove the fix did not widen acceptance past the
// const carve-out. In particular the two "premise" tests (assignment to a const
// inside a lambda body, and a const as a `ref`/`out` argument) pin the exact
// enforcement this fix's soundness argument depends on — if either ever stopped
// being an error, preserving const facts across the boundary would become
// unsound, and those tests would fail loudly rather than silently.

#include "sema/ExprTypecheck.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// True iff some reported diagnostic's message contains `needle`.
bool hasMsg(const bezel::test::SemaResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// ACCEPT — the defect #2597 reported. Each of these was REJECTED before the
// fix with "requires narrowing" (or the member-not-found equivalent).
// ---------------------------------------------------------------------------

// §7.4 (#2597): the issue's exact repro. `t` is a `const` local narrowed
// non-null before the lambda; the body reads `t.Length`. §7.4's const clause
// makes this well-typed — `t` cannot be rebound by anyone, so §27.5's "vice
// versa" rebind (the whole reason #2259 drops facts) cannot apply to it.
// Verified end-to-end beyond this type-check: the program compiles, links, and
// returns 4 for "abcd".
BEZEL_TEST(sema_const_narrowing_lambda_2597,
           const_local_null_fact_rides_into_lambda_spec_7_4) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method Run(s: string?): int "
        "    if s = null then return 0; end; "
        "    const t: string? := s; "
        "    if t <> null then "
        "      var f := lambda(): int => do return t.Length; end; "
        "      return f(); "
        "    end; "
        "    return 0; "
        "  end Run; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §7.4 (#2597): the expression-body lambda form of the same shape.
BEZEL_TEST(sema_const_narrowing_lambda_2597,
           const_local_null_fact_rides_into_expr_body_lambda_spec_7_4) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method Run(s: string?): int "
        "    if s = null then return 0; end; "
        "    const t: string? := s; "
        "    if t <> null then "
        "      var f := lambda() => t.Length; "
        "      return f(); "
        "    end; "
        "    return 0; "
        "  end Run; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §12.7 (#2597): the TYPE-classification set (`narrowings_`) gets the same
// carve-out — a `const` local narrowed to `Derived` by an `is` test keeps that
// classification inside the lambda body, so `b.M()` dispatches. A const cannot
// be reassigned, so its runtime type cannot change after the test.
BEZEL_TEST(sema_const_narrowing_lambda_2597,
           const_local_type_fact_rides_into_lambda_spec_12_7) {
    auto r = parseSemaAll(
        "module M; "
        "class Base end Base; "
        "class Derived : Base "
        "  public method M(): int return 7; end M; "
        "end Derived; "
        "class C "
        "  public static method Run(b: Base): int "
        "    const c: Base := b; "
        "    if c is Derived then "
        "      var f := lambda(): int => do return c.M(); end; "
        "      return f(); "
        "    end; "
        "    return 0; "
        "  end Run; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §7.4 (#2597): the FIELD set (`fieldNarrowed_`) gets the carve-out on its
// ROOT. §7.4: the tracked shape is "`local._f` where `local` is itself
// narrowing-tracked; a fact rides its root". With a `const` root the root
// cannot be reassigned, and `_f` is `readonly` — which "outside its
// construction window … has no writer on any path, through any alias" — so the
// whole path is final and the fact survives the boundary. Verified end-to-end:
// compiles, links, returns 4.
BEZEL_TEST(sema_const_narrowing_lambda_2597,
           const_rooted_readonly_field_fact_rides_into_lambda_spec_7_4) {
    auto r = parseSemaAll(
        "module M; "
        "class Holder "
        "  public readonly _f: string?; "
        "  public constructor(v: string?) _f := v; end constructor; "
        "end Holder; "
        "class C "
        "  public static method Run(h: Holder): int "
        "    const t: Holder := h; "
        "    if t._f <> null then "
        "      var g := lambda(): int => do return t._f.Length; end; "
        "      return g(); "
        "    end; "
        "    return 0; "
        "  end Run; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §7.4 (#2597): the fact survives TWO lambda boundaries — a const-rooted fact
// preserved on the way into an outer body is still present entering a nested
// one. Covers the nesting-depth axis of the save/restore block.
BEZEL_TEST(sema_const_narrowing_lambda_2597,
           const_local_fact_rides_into_nested_lambda_spec_7_4) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method Run(s: string?): int "
        "    if s = null then return 0; end; "
        "    const t: string? := s; "
        "    if t <> null then "
        "      var f := lambda(): int => do "
        "        var g := lambda(): int => do return t.Length; end; "
        "        return g(); "
        "      end; "
        "      return f(); "
        "    end; "
        "    return 0; "
        "  end Run; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// REJECT — the load-bearing half. A fix that LIFTS a rejection must not lift
// any of these. Each passed before the fix and must still pass after it.
// ---------------------------------------------------------------------------

// §27.5 / #2259: a `var` local's fact must STILL be dropped — this is #2259's
// exact SIGSEGV repro (the enclosing scope rebinds `s` to null between closure
// creation and invocation). The const carve-out must not reach it.
BEZEL_TEST(sema_const_narrowing_lambda_2597,
           var_local_fact_still_dropped_spec_27_5) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method Run(s: string?): int "
        "    var f: (() -> int)? := null; "
        "    if s = null then return 0; end; "
        "    var t: string? := s; "
        "    if t <> null then "
        "      f := lambda(): int => do return t.Length; end; "
        "    end; "
        "    t := null; "
        "    return f!(); "
        "  end Run; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "requires narrowing"));
}

// §27.5 / #2259: a captured PARAMETER is mutable (assignable) and is a
// ParameterSymbol, not a `const` LocalSymbol — its fact must still be dropped.
// This is the axis where a predicate written as "not a mutable local" rather
// than "is a const local" would have silently gone wrong.
BEZEL_TEST(sema_const_narrowing_lambda_2597,
           captured_parameter_fact_still_dropped_spec_27_5) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method Run(s: string?): int "
        "    if s <> null then "
        "      var f := lambda() => s.Length; "
        "      return f(); "
        "    end; "
        "    return 0; "
        "  end Run; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "requires narrowing"));
}

// §12.7 / #2259: a `var` local's TYPE fact must still be dropped — `b.M()`
// inside the body is rejected because `b` reverts to `Base`.
BEZEL_TEST(sema_const_narrowing_lambda_2597,
           var_local_type_fact_still_dropped_spec_12_7) {
    auto r = parseSemaAll(
        "module M; "
        "class Base end Base; "
        "class Derived : Base "
        "  public method M(): int return 7; end M; "
        "end Derived; "
        "class C "
        "  public static method Run(b: Base): int "
        "    if b is Derived then "
        "      var f := lambda(): int => do return b.M(); end; "
        "      return f(); "
        "    end; "
        "    return 0; "
        "  end Run; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "no member `M` on type `Base`"));
}

// §7.4 / #2116: a readonly-field fact rooted in a `var` local must still be
// dropped — the ROOT can be reassigned to a different object between closure
// creation and invocation, so the field read is not final even though the field
// itself is `readonly`. This is the field-set half of the unsoundness guard.
BEZEL_TEST(sema_const_narrowing_lambda_2597,
           var_rooted_readonly_field_fact_still_dropped_spec_7_4) {
    auto r = parseSemaAll(
        "module M; "
        "class Holder "
        "  public readonly _f: string?; "
        "  public constructor(v: string?) _f := v; end constructor; "
        "end Holder; "
        "class C "
        "  public static method Run(h: Holder, h2: Holder): int "
        "    var t: Holder := h; "
        "    if t._f <> null then "
        "      var g := lambda(): int => do return t._f.Length; end; "
        "      t := h2; "
        "      return g(); "
        "    end; "
        "    return 0; "
        "  end Run; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "requires narrowing"));
}

// §7.4: a MUTABLE field is never narrowed at all, inside a lambda or out —
// "The compiler narrows local variables and `readonly` fields; it never narrows
// mutable fields or properties." The carve-out must not have opened this.
BEZEL_TEST(sema_const_narrowing_lambda_2597,
           mutable_field_still_not_narrowed_in_lambda_spec_7_4) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  private _s: string?; "
        "  public constructor(s: string?) _s := s; end constructor; "
        "  public method Run(): int "
        "    if _s <> null then "
        "      var f := lambda(): int => do return _s.Length; end; "
        "      return f(); "
        "    end; "
        "    return 0; "
        "  end Run; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "accessed through a field"));
}

// §7.4 / #2113: the OUT-side restore is unchanged — a fact established INSIDE
// the lambda body must not leak to the enclosing scope. Here `t` is a const
// narrowed only inside the body; the enclosing `t.Length` after the lambda must
// still be rejected. Guards the half of the save/restore block this fix did not
// touch but sits directly beside.
BEZEL_TEST(sema_const_narrowing_lambda_2597,
           fact_established_inside_body_does_not_leak_out_spec_7_4) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method Run(s: string?): int "
        "    const t: string? := s; "
        "    var f := lambda(): int => do "
        "      if t <> null then return t.Length; end; "
        "      return 0; "
        "    end; "
        "    return f() + t.Length; "
        "  end Run; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "requires narrowing"));
}

// §7.4: a `const` local that was never narrowed before the lambda has no fact
// to ride in — the body must still reject the bare member access. Guards
// against the carve-out being read as "const locals are always non-null".
BEZEL_TEST(sema_const_narrowing_lambda_2597,
           unnarrowed_const_local_still_rejected_in_lambda_spec_7_4) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method Run(s: string?): int "
        "    const t: string? := s; "
        "    var f := lambda(): int => do return t.Length; end; "
        "    return f(); "
        "  end Run; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "requires narrowing"));
}

// ---------------------------------------------------------------------------
// PREMISE tests — these pin the enforcement this fix's soundness rests on.
// §7.4 justifies preserving a const fact because "no invalidating form can
// apply to it". If either enforcement below ever regressed, preserving the
// fact would become unsound; these fail loudly if that happens.
// ---------------------------------------------------------------------------

// §21.1: assignment to a `const` local is an error INSIDE a lambda body, which
// is what makes a const impossible to mutably capture (§7.4's parenthetical
// "lambda bodies included"). Without this, the preserved fact could be
// invalidated by the closure itself.
BEZEL_TEST(sema_const_narrowing_lambda_2597,
           premise_assign_to_const_inside_lambda_rejected_spec_21_1) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method Run(s: string?): int "
        "    const t: string? := s; "
        "    if t <> null then "
        "      var f := lambda(): int => do t := null; return 0; end; "
        "      return f(); "
        "    end; "
        "    return 0; "
        "  end Run; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "cannot assign to const local"));
}

// §22.3 / §21.1: a `const` local may not be a `ref`/`out` argument — the other
// invalidating form §7.4 names. An `out` argument would let a callee rebind the
// cell after the fact was established.
BEZEL_TEST(sema_const_narrowing_lambda_2597,
           premise_const_local_as_out_argument_rejected_spec_22_3) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method Sink(out n: string?): int "
        "    n := null; "
        "    return 0; "
        "  end Sink; "
        "  public static method Run(s: string?): int "
        "    const t: string? := s; "
        "    if t <> null then "
        "      return Sink(out t); "
        "    end; "
        "    return 0; "
        "  end Run; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "may not be a const local"));
}
