// Spec-derived tests for fix #4199 — §14.5 phase 1 (v0.298): a generic
// method's value parameter whose declared type nests a user generic record
// TWO levels deep (`Box<Box<T>>`) failed to unify `T` against a matching
// concrete argument, even with no type alias involved anywhere.
//
// §14.5 phase 1 states (Argument-driven unification): "a constructed
// parameter type G<P1, ..., Pn> unifies component-wise and invariantly
// against an argument of the same generic type, or against the unique
// instantiation of G that the argument's type implements or extends" —
// stated once, but recursive by construction: each component Pi may ITSELF
// be a constructed generic type, in which case the same rule applies again
// to unify it against the argument's matching component.
//
// ROOT CAUSE (measured, not the issue's own suspected area): sema's
// call-argument ASSIGNABILITY check — `ExprTC::checkAssignable` ->
// `variantGenericAssignable` (src/sema/ExprTypecheck.cpp) — is what rejects
// the call, BEFORE codegen is ever reached. It is a *different* operation
// from the argument-inferred substitution builder (`ExprTC::unifyArgSubst`,
// src/sema/ExprTypecheck_Calls.cpp), which already recurses correctly
// through arbitrary nesting depth and was NOT the defective function.
//
// `variantGenericAssignable`'s own per-argument loop recognizes a bare
// unbound method type parameter (a "free" Named TypeRef: no declaration, no
// generic args of its own) at its OWN immediate `to->genericArgs[i]` slot
// and skips the identity/variance check for it (§14.1, issue #1180) — which
// is why ONE level of nesting (`Box<T>` vs `Box<int>`) already worked: the
// top-level loop sees the bare `T` directly. But when that slot is itself a
// COMPOUND constructed type (`Box<T>` inside `Box<Box<T>>`), the check falls
// through to `typeArgIdentical`'s recursive identity walk for the record's
// (invariant) position — and THAT function had no knowledge of a free type
// variable at all, at any depth: comparing `int` (Primitive) against `T`
// (Named, free) failed on a bare Kind mismatch in `typeArgSameBase` before
// ever asking whether `T` was a placeholder. So the recursion did not stop
// structurally — `typeArgIdentical` genuinely walks `Box<Box<int>>` down to
// its `int`/`T` leaf — it stopped because the leaf-level comparison had no
// free-variable carve-out, only the immediate-caller loop did.
//
// THE FIX (src/sema/ExprTypecheck.cpp): extract the existing free-variable
// recognition into a shared `isFreeGenericTypeVarRef` helper and consult it
// at the TOP of `typeArgIdentical`, so the carve-out applies at every
// recursion depth, not only at `variantGenericAssignable`'s own top-level
// loop. `variantGenericAssignable`'s loop is updated to call the same
// helper (LANE_RULES §6 — one definition, not two drifting copies).
//
// This does not weaken the SEPARATE checks that constrain what a free
// variable may actually bind to: `unifyArgSubst`'s own conflict detection
// (§14.1/#896's "Agreement" rule — two arguments binding the same T to
// different concrete types is still a compile error) is untouched, and a
// genuine structural mismatch at the SAME nesting depth (e.g. `Box<Array<
// int>>` against a declared `Box<Box<T>>` parameter — Array is not Box) is
// still rejected, because neither side of that comparison is ever a free
// variable.
//
// FAIL-WITHOUT / PASS-WITH: measured via a `src/sema/ExprTypecheck.cpp`
// revert + `bezelc` + `bezel_tests_codegen` rebuild + re-run cycle, both
// directions against the same rebuilt binaries (base: this lane's parent,
// r18-integration `628c4c5`; see the lane report for the exact commands and
// logs). 2 OF 5 rows fail without the fix and pass with it:
//   - `two_level_nesting_binds_through_both_wraps` — FAILS pre-fix (the
//     issue's exact repro): "the argument `b` to call to `UnwrapOuter` has a
//     type that is not implicitly convertible to the target type (§5.1)".
//     PASSES post-fix, returning the correct doubly-wrapped value.
//   - `three_level_nesting_binds_through_all_wraps` — FAILS pre-fix with the
//     SAME diagnostic (the recursion chokes at the SECOND level regardless
//     of how many further levels exist beyond it). PASSES post-fix — this
//     row exists to show the fix is a genuine unbounded recursion, not a
//     depth-2 special case.
// The other three rows pass identically on both sides, each for a stated
// reason (LANE_RULES §20):
//   - `conflicting_binding_through_two_levels_still_rejected` — an EARLIER
//     draft of this comment predicted this row's diagnostic TEXT would
//     differ pre/post fix (pre-fix rejecting via the buggy two-level
//     assignability check on argument `a` alone; post-fix via the separate
//     §14.1/#896 Agreement check). Measured, that prediction was WRONG: the
//     Agreement check — which builds its own argument-inferred substitution
//     via `unifyArgSubst` (a DIFFERENT, already-fully-recursive function;
//     see the ROOT CAUSE note above) and detects the T=int/T=string
//     conflict there — runs and reports BEFORE `checkArgsAgainstParams`'s
//     per-argument `checkAssignable` calls are ever reached, so the SAME
//     "cannot be bound to both" diagnostic fires on both sides, verified by
//     compiling this row's program directly against both the pre-fix and
//     post-fix `bezelc` binaries. Kept as a regression control proving the
//     fix does not disturb the Agreement check's independent authority.
//   - `one_level_nesting_control_unaffected` is a REGRESSION CONTROL for the
//     shape issue #1180 already fixed — `variantGenericAssignable`'s own
//     top-level loop already handled it before this fix, and still does.
//   - `mismatched_outer_declaration_still_rejected` is a REJECT-SIDE
//     CONTROL: `Box<Array<int>>` against a declared `Box<Box<T>>` parameter
//     reaches `typeArgIdentical(Array<int>, Box<T>)`, where NEITHER side is
//     a free variable (`Array<int>` has non-empty generic args; `Box<T>` has
//     a real declaration) — so the new carve-out never fires and
//     `typeArgSameBase`'s existing declaration-mismatch rejection is
//     unchanged.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

// #4199 repro (issue's own program, verbatim structure): a generic method
// parameter type nests a user generic record TWO levels deep.
BEZEL_TEST(generic_method_nested_record_unification_4199,
           two_level_nesting_binds_through_both_wraps) {
    auto r = runAot(
        "module M; "
        "record Box<T>(Value: T); "
        "class Util "
        "  public static method UnwrapOuter<T>(b: Box<Box<T>>): Box<T> "
        "    return b.Value; "
        "  end UnwrapOuter; "
        "end Util; "
        "class C "
        "  public static method Main(): int "
        "    var p: Box<Box<int>> := new Box<Box<int>>(new Box<int>(19)); "
        "    var inner := Util.UnwrapOuter(p); "
        "    return inner.Value; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 19);
}

// Robustness: THREE levels of nesting, to prove the fix is a genuine
// unbounded recursion (isFreeGenericTypeVarRef consulted at every
// `typeArgIdentical` recursion depth) rather than a depth-2 special case.
BEZEL_TEST(generic_method_nested_record_unification_4199,
           three_level_nesting_binds_through_all_wraps) {
    auto r = runAot(
        "module M; "
        "record Box<T>(Value: T); "
        "class Util "
        "  public static method UnwrapOuter<T>(b: Box<Box<Box<T>>>): "
        "Box<Box<T>> "
        "    return b.Value; "
        "  end UnwrapOuter; "
        "end Util; "
        "class C "
        "  public static method Main(): int "
        "    var p: Box<Box<Box<int>>> := new Box<Box<Box<int>>>(new "
        "Box<Box<int>>(new Box<int>(37))); "
        "    var mid := Util.UnwrapOuter(p); "
        "    return mid.Value.Value; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 37);
}

// Regression control (issue #1180's original shape): ONE level of nesting
// already worked before this fix — `variantGenericAssignable`'s own
// top-level loop sees the bare `T` directly at `to->genericArgs[0]` and
// skips the check there. Passes identically pre- and post-fix; kept so a
// future change to the shared `isFreeGenericTypeVarRef` helper cannot
// silently regress the shallow case while fixing a deeper one.
BEZEL_TEST(generic_method_nested_record_unification_4199,
           one_level_nesting_control_unaffected) {
    auto r = runAot(
        "module M; "
        "record Box<T>(Value: T); "
        "class Util "
        "  public static method Unwrap<T>(b: Box<T>): T "
        "    return b.Value; "
        "  end Unwrap; "
        "end Util; "
        "class C "
        "  public static method Main(): int "
        "    var p: Box<int> := new Box<int>(19); "
        "    return Util.Unwrap(p); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    if (r.ran) BEZEL_EXPECT_EQ(r.exitCode, 19);
}

// Reject-side control: the OUTER argument's generic component (`Array<int>`)
// does not match the parameter's declared component (`Box<T>`) at all — a
// genuine structural mismatch, not an unbound-variable slot. Neither side of
// this specific `typeArgIdentical` comparison is a free variable (`Array<
// int>` has a non-empty generic-arg list; `Box<T>` has a real declaration),
// so the new carve-out must not fire, and the existing declaration-mismatch
// rejection in `typeArgSameBase` must still apply. Confirms the fix does not
// widen acceptance beyond genuinely free positions.
BEZEL_TEST(generic_method_nested_record_unification_4199,
           mismatched_outer_declaration_still_rejected) {
    auto r = runAot(
        "module M; "
        "record Box<T>(Value: T); "
        "class Util "
        "  public static method UnwrapOuter<T>(b: Box<Box<T>>): Box<T> "
        "    return b.Value; "
        "  end UnwrapOuter; "
        "end Util; "
        "class C "
        "  public static method Main(): int "
        "    var p: Box<Array<int>> := new Box<Array<int>>(new Array<int>(1, "
        "fill := 5)); "
        "    var inner := Util.UnwrapOuter(p); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "cannot infer the type argument"));
}

// §14.1/#896 Agreement control: T is nested TWO levels deep in BOTH
// parameters of the same method, bound to two DIFFERENT concrete types
// across the two arguments. This must still be rejected after the fix — the
// fix makes the per-argument ASSIGNABILITY check accept a free variable at
// any depth, but the separate cross-argument Agreement check (unrelated
// code, `ExprTC::unifyArgSubst`'s `inferConflicts`) is the one responsible
// for catching an inconsistent binding, and remains untouched.
//
// MEASURED (not assumed, per LANE_RULES §20): this row's diagnostic is the
// SAME "cannot be bound to both" text on both sides of the fix — the
// Agreement check runs and reports before `checkArgsAgainstParams`'s
// per-argument `checkAssignable` calls (this file's own two-level-rejecting
// bug) are ever reached, so the buggy assignability check never gets a
// chance to fire its own diagnostic here. Confirmed by compiling this row's
// program directly against both the pre-fix and post-fix `bezelc` binaries;
// an earlier draft of this comment predicted a text difference and was
// wrong. Passes identically on both sides — a genuine regression control,
// not a FAIL-WITHOUT row.
BEZEL_TEST(generic_method_nested_record_unification_4199,
           conflicting_binding_through_two_levels_still_rejected) {
    auto r = runAot(
        "module M; "
        "record Box<T>(Value: T); "
        "class Util "
        "  public static method Both<T>(a: Box<Box<T>>, b: Box<Box<T>>): T "
        "    return a.Value.Value; "
        "  end Both; "
        "end Util; "
        "class C "
        "  public static method Main(): int "
        "    var a: Box<Box<int>> := new Box<Box<int>>(new Box<int>(1)); "
        "    var b: Box<Box<string>> := new Box<Box<string>>(new "
        "Box<string>(\"x\")); "
        "    var v := Util.Both(a, b); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "cannot be bound to both"));
}
