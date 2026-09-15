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
// THE FIX (src/sema/ExprTypecheck.cpp): extract the free-variable
// recognition into a shared `isFreeGenericTypeVarRef` helper and consult it
// at the TOP of `typeArgIdentical`, so the carve-out applies at every
// recursion depth, not only at `variantGenericAssignable`'s own top-level
// loop. `variantGenericAssignable`'s loop is updated to call the same
// helper (LANE_RULES §6 — one definition, not two drifting copies).
//
// ⚠ NEEDS-FIX CORRECTION (review round 1). The FIRST version of
// `isFreeGenericTypeVarRef` (commit `d3032aa`) decided "is this a free type
// variable" from TypeRef SHAPE ALONE — Named, null declaration, no generic
// args, non-empty name — on the theory that only a rigid type-parameter
// reference has that shape. That is false: `TypeResolution_Names.cpp`'s
// always-available-builtins table resolves ~50 stdlib names (`Task`,
// `Exception`, `IDisposable`, `Mutex`, `DateTime`, `StringBuilder`, ...) to
// EXACTLY that shape too — `isFullyGroundTypeRef`'s own comment a few
// hundred lines above already names this ambiguity and resolves it
// conservatively (treats BOTH as non-ground). The shape-only version
// silently misclassified a null-declaration builtin as an unbound method
// type parameter and accepted a program that must be rejected — turning a
// clean §5.1 compile-time reject into sema wrongly ACCEPTING the call,
// which then reached codegen and crashed with an LLVM IR verification
// failure (a real reject became an ICE). See
// `builtin_nested_two_levels_not_misidentified_as_free_type_param` and
// `builtin_one_level_not_misidentified_as_free_type_param` below.
//
// THE CORRECTED FIX decides "free" from GROUND TRUTH instead: a new
// `ExprTC::currentScopeGenericParamNames()` (enclosing type's/method's/
// extension's own declared generic parameters — for the pattern-
// exhaustiveness call sites that rely on a RIGID enclosing-body parameter
// being treated as free, §19.6/#1913/#1920/#1927) or the CALLEE method's
// own `genericParams` at a call-argument check (`checkArgsAgainstParams`,
// and its cross-module sibling `checkImportedCallArity`) — never inferred
// from a TypeRef's own shape. `isFreeGenericTypeVarRef`, `typeArgIdentical`,
// `typeArgCovariantAssignable`, `variantGenericAssignable`, and
// `ExprTC::checkAssignable` itself all now carry an explicit
// `freeNames`/`freeTypeParamNames` parameter (null = none — the
// ground-truth-conservative default every OTHER `checkAssignable` call site
// keeps unchanged).
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
// FAIL-WITHOUT / PASS-WITH: measured via `src/sema/ExprTypecheck.cpp`-only
// revert + `bezelc` + `bezel_tests_codegen` rebuild + re-run cycles, against
// TWO baselines (both explicitly named per row, LANE_RULES §26):
//   - r18-integration `628c4c5` — this lane's parent, "before the RUN" —
//     via `git checkout 628c4c5 -- src/sema/ExprTypecheck.cpp` in-worktree;
//   - the superseded shape-only fix `d3032aa` — "before THIS correction" —
//     via a separate isolated worktree (LANE_RULES §19) built at that SHA.
// 3 OF 7 rows fail against `628c4c5` and pass with the corrected fix:
//   - `two_level_nesting_binds_through_both_wraps` — FAILS pre-fix (the
//     issue's exact repro): "the argument `b` to call to `UnwrapOuter` has a
//     type that is not implicitly convertible to the target type (§5.1)".
//     PASSES post-fix, returning the correct doubly-wrapped value.
//   - `three_level_nesting_binds_through_all_wraps` — FAILS pre-fix with the
//     SAME diagnostic (the recursion chokes at the SECOND level regardless
//     of how many further levels exist beyond it). PASSES post-fix — this
//     row exists to show the fix is a genuine unbounded recursion, not a
//     depth-2 special case.
//   - `builtin_one_level_not_misidentified_as_free_type_param` — FAILS
//     against `628c4c5` TOO (a pre-existing defect tracing to #1180's
//     ORIGINAL inline carve-out, not introduced by #4199 or either of its
//     fixes): sema wrongly accepts `Box<Task>` vs `Box<int>`, codegen
//     crashes with the same IR verification failure. PASSES on the
//     corrected fix. This is a genuine additional bug closed as a necessary
//     side effect of fixing #4199 correctly — the free-variable predicate is
//     shared at every recursion depth by design, so it could not be fixed
//     at depth 2 without also being fixed at depth 1.
// `builtin_nested_two_levels_not_misidentified_as_free_type_param` is
// DELIBERATELY NOT counted in the 3-of-7: it already PASSES against
// `628c4c5` (which rejects the program, but for the WRONG reason — no
// free-variable concept existed yet at that nesting depth at all). It FAILS
// only against the superseded shape-only fix `d3032aa` and is the direct
// regression pin for the NEEDS-FIX defect itself — see its own comment for
// the full three-way (`628c4c5` / `d3032aa` / corrected-fix) measurement.
// The remaining three rows pass identically on both sides, each for a stated
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

// ============================================================================
// NEEDS-FIX regression rows (review round 1) — see the file header's ground-
// truth rewrite note. `isFreeGenericTypeVarRef`'s ORIGINAL (superseded) test
// was SHAPE-only: "Named TypeRef, null declaration, no generic args,
// non-empty name". That is ALSO the exact shape ~50 null-declaration stdlib
// builtins resolve to (`TypeResolution_Names.cpp`'s always-available-
// builtins table: `Task`, `Exception`, `IDisposable`, `Mutex`, `DateTime`,
// ... — `isFullyGroundTypeRef`'s own comment, a few hundred lines above
// `variantGenericAssignable` in ExprTypecheck.cpp, already names this exact
// ambiguity and resolves it conservatively). The shape-only fix silently
// misclassified a null-declaration builtin as an unbound method type
// parameter and accepted it against an unrelated concrete argument type —
// turning a clean compile-time REJECT into sema wrongly ACCEPTING the call,
// which then reached codegen and crashed with an LLVM IR verification
// failure (a real reject became an ICE).
// ============================================================================

// The exact #4199-shaped (two-level) builtin-misclassification repro.
// MEASURED against three trees (not assumed, LANE_RULES §26):
//   - `628c4c5` (this run's base, before #4199 touched anything): REJECTS,
//     for the WRONG reason — `typeArgIdentical` had no free-variable concept
//     at all yet, so it rejected EVERY nested mismatch including the
//     legitimate `Box<Box<T>>` case (that IS #4199's original bug).
//   - the superseded shape-only #4199 fix (this file's first commit,
//     `d3032aa`): sema WRONGLY ACCEPTS, then codegen crashes ("Call
//     parameter type does not match function signature!" / IR verification
//     failed).
//   - this (ground-truth) fix: REJECTS again, for the RIGHT reason this
//     time — `Task` is never a member of the `freeNames` set built from an
//     actual method/type declaration's generic parameters.
BEZEL_TEST(generic_method_nested_record_unification_4199,
           builtin_nested_two_levels_not_misidentified_as_free_type_param) {
    auto r = runAot(
        "module M; "
        "record Box<T>(Value: T); "
        "class Util "
        "  public static method TakesBoxBoxTask(b: Box<Box<Task>>): int "
        "    return 0; "
        "  end TakesBoxBoxTask; "
        "end Util; "
        "class C "
        "  public static method Main(): int "
        "    var p: Box<Box<int>> := new Box<Box<int>>(new Box<int>(5)); "
        "    return Util.TakesBoxBoxTask(p); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "not implicitly convertible to the target type"));
}

// Depth-1 sibling of the row above, and the reviewer's central point applied
// in reverse: "An axis that is already broken one level down gives you no
// signal that you broke it one level up" — a depth-2-only control gives no
// signal about depth-1 either. MEASURED (isolated worktree, LANE_RULES §19):
// `628c4c5` — well before #4199 touched anything, tracing back to #1180's
// ORIGINAL inline shape-only carve-out — already wrongly ACCEPTS `Box<Task>`
// against `Box<int>` in sema and crashes CODEGEN with the identical IR
// verification failure. This is a genuinely PRE-EXISTING defect, not
// introduced by #4199 or its fix, that this ground-truth rewrite closes as a
// necessary side effect of fixing #4199 correctly (the same predicate is
// shared at every recursion depth by design — see `isFreeGenericTypeVarRef`).
// Kept as its own row so a future change to that predicate cannot silently
// regress the shallow case while only a deep one is tested.
BEZEL_TEST(generic_method_nested_record_unification_4199,
           builtin_one_level_not_misidentified_as_free_type_param) {
    auto r = runAot(
        "module M; "
        "record Box<T>(Value: T); "
        "class Util "
        "  public static method TakesBoxTask(b: Box<Task>): int "
        "    return 0; "
        "  end TakesBoxTask; "
        "end Util; "
        "class C "
        "  public static method Main(): int "
        "    var p: Box<int> := new Box<int>(5); "
        "    return Util.TakesBoxTask(p); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "not implicitly convertible to the target type"));
}
