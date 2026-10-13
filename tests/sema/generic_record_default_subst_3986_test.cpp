// fix #3986 (§17.11 "The default operator", v0.294): `ExprTC::defaultIllFormed`
// (src/sema/ExprTypecheck_Exprs.cpp), `Record` arm, never substituted a
// generic record's OWN type parameter against the concrete instantiation
// being checked. A positional field typed as a bare type parameter (`V: T`
// in `record Box<T>(V: T)`) resolves, via `ctx_.resolvedType`, to the
// declaration-site, unsubstituted TypeRef for `T` — a Named TypeRef with a
// null `declaration` — which the function's own generic-parameter early
// return (`if (!t->declaration) return false;`) unconditionally treats as
// well-formed. So `default(Box<X>)` was accepted regardless of what `X`
// actually was, including a non-nullable class type, for which §17.11 makes
// `default` a compile error.
//
// THE DEFECT — measured on this fix's parent commit (origin/main before this
// fix; `bezelc -o out repro.bzl`), purely LOCAL, no import involved at all:
//
//     module GenLocal;
//     class WidgetGL
//       public Count: int;
//       public constructor(c: int) Count := c; end constructor;
//       public method Render(): int return Count; end Render;
//     end WidgetGL;
//     record BoxGL<T>(V: T);
//     class ProgramGL
//       public static method Main(): int
//         var x := default(BoxGL<WidgetGL>);
//         return x.V.Render();
//       end Main;
//     end ProgramGL;
//
//   compiled at exit 0 with NO diagnostic, then SIGSEGV'd at runtime (exit
//   139) — `x.V` is a null pointer manufactured by codegen's default(T)
//   record lowering, which recursively default-constructs each field on the
//   assumption sema already rejected any field for which that has no valid
//   zero value.
//
// This is a SEPARATE, non-import-specific defect from #3982 (an imported
// record's `TypeSymbol::decl` being null made the SAME arm's AST-side cast
// fail open for EVERY imported record, generic or not) — the repro above
// has no import at all, and this fix's own diff does not touch the `!td`
// (imported-record) branch, so it neither depends on nor conflicts with
// #3982's fix, whichever lands first.
//
// THE FIX threads a `common::RecordSubstEnv` (OwnershipFacts.hpp, #2145 —
// the SAME instantiation-site substitution environment the §9.7
// borrow-bearing-record walk and the §35.4/§35.11 choice-payload walk
// already use for identically-shaped generic substitution) through the
// Record arm's positional-field walk: `seedRecordSubstEnv` builds one frame
// from the record's own AST generic parameters against `t`'s (the concrete
// instantiation's) `genericArgs`, and `resolveRecordFieldSubst` substitutes
// each field's resolved-but-still-bare type through it before the recursive
// `defaultIllFormed` call. No new predicate — both primitives already exist
// and are used elsewhere in this same file family (Sendability.cpp).
//
// SHAPE SWEEP (measured before/after; see the commit body for exact
// FAIL-WITHOUT/PASS-WITH counts and filters run):
//   - bare type param instantiated at a non-nullable class type -> REJECT
//     (the defect; `generic_record_class_param_rejected` below)
//   - non-generic twin, same field shape                        -> REJECT
//     (unaffected control; `nongeneric_twin_control_rejected`)
//   - bare type param instantiated at a value type (`int`)       -> ACCEPT,
//     runtime value 0 (`generic_record_value_param_accepted_zero`)
//   - bare type param instantiated at a nullable class type      -> ACCEPT,
//     runtime value null (`generic_record_nullable_param_accepted_null`)
//   - nested (self-referential) generic instantiation, inner leaf
//     non-nullable class                                          -> REJECT
//     (`nested_generic_record_rejected`)
//   - nested (self-referential) generic instantiation, inner leaf
//     value type                                                  -> ACCEPT,
//     value 0 (`nested_generic_record_value_accepted_zero`)
//   - generic record as an inline array element type              -> REJECT
//     (`inline_array_of_generic_record_rejected`)
//   - two type parameters, only one bound to a non-nullable class -> REJECT
//     (`multi_param_one_bad_rejected`)
//   - two type parameters, the reference one bound nullable       -> ACCEPT
//     (`multi_param_all_ok_accepted`)
//   - DELEGATING generic record (`Outer<T>(In: Inner<T>)`, a
//     DIFFERENT record type wrapping the parameter), inner leaf
//     non-nullable class                                          -> REJECT
//     (`delegating_generic_record_rejected` -- fix #3995, see below)
//   - same delegating shape, inner leaf value type                -> ACCEPT,
//     value 0 (`delegating_generic_record_value_leaf_accepted_zero`)
//   - generic CHOICE, for comparison (§35.10 rejects unconditionally,
//     independent of substitution -- unaffected by this fix)      -> REJECT
//     (`generic_choice_control_rejected`)
//   - generic CLASS, for comparison (a class is always a non-nullable
//     reference type, independent of substitution -- unaffected)  -> REJECT
//     (`generic_class_control_rejected`)
//
// fix #3995 (§17.11/§9.7/§3.6 rule 2, v0.294; found by review of `bb17e8a`,
// this fix's own merge with #3982): the two DELEGATING rows above were
// ADDED after the fact because the original merge's rationale comment
// claimed (wrongly) that composing an "incoming" substitution environment
// across nested `defaultIllFormed` frames was unreachable -- true for the
// SELF-referential nested shape above (`Box<Box<Widget>>`, whose inner
// frame's own generic argument is already a concrete literal from the
// source), but FALSE for a delegating shape where a DIFFERENT record type
// wraps the parameter (`record Inner<T>(V: T); record Outer<T>(In:
// Inner<T>)`): `Inner<T>`'s own generic argument is a BARE MENTION of
// OUTER's `T`, which only resolves if the recursive call into the inner
// frame is seeded with the OUTER frame's environment as `incoming` --
// exactly the composition `common::isBorrowBearingRecordCommon` already
// performs for the identically-shaped §9.7 walk (OwnershipFacts.cpp).
// MEASURED: `record InnerDG1<T>(V: T); record OuterDG1<T>(In:
// InnerDG1<T>); default(OuterDG1<WidgetDG1>)` compiled at exit 0 and
// SIGSEGV'd (exit 139) on `bb17e8a` (this fix's merge with #3982, BEFORE
// #3995's composition fix) -- reproduced identically against `91e353f`
// (#3982 alone), confirming the gap was not introduced by this file's own
// merge and was always latent in both fixes' independent, narrower shapes.
//
// FAIL-WITHOUT/PASS-WITH (proven mechanically): with the composed-`incoming`
// version of `defaultIllFormed` reverted to the pre-#3995 (bb17e8a) shape --
// `seedRecordSubstEnv`/the InlineArray recursion always seeded/reseeded
// with `nullptr` rather than a threaded `incoming` -- and this test file
// kept, `generic_record_default_subst_3986` FAILS on exactly
// `delegating_generic_record_rejected` (compiles clean instead of being
// rejected) with every other row, including the SELF-referential nested
// rows above, unaffected. With the Record arm further reverted all the way
// to this fix's #3982-only parent (the bare `dynamic_cast` +
// unsubstituted `td->positionalFields` walk), every `*_rejected` row whose
// only problem is an unsubstituted generic field additionally FAILS
// (`generic_record_class_param_rejected`, `nested_generic_record_rejected`,
// `inline_array_of_generic_record_rejected`, `multi_param_one_bad_rejected`,
// `delegating_generic_record_rejected`), while `nongeneric_twin_control_
// rejected`, `generic_choice_control_rejected`, and
// `generic_class_control_rejected` (none of which depend on either fix)
// and every `*_accepted*`/`*_accepted_zero*` row are unaffected at every
// revert depth. Restoring the fix and relinking flips every `*_rejected`
// row to PASS with no change to the others.
//
// Filters run (no --area): `generic_record_default_subst_3986` (this file's
// own suite), `default_t`, `17_11`, `generic_record`, `subst`, `record` --
// see the commit body for exact pass counts and which binaries each hit.

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation, diagnosticMessageContains
#include "support/aot_support.hpp"         // runAot
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

// Asserts a clean, real-location §17.11 rejection with the exact
// non-defaultable-field wording (not a crash, not a timeout, not any other
// `!r.ran` outcome per aot_result_asserts.hpp's own four-outcome warning).
void expectDefaultIllFormedRejected(const std::string& program) {
    auto r = runAot(program);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r,
        "`default(T)` is a compile error for a record any of whose fields "
        "is a non-nullable reference type"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "\xC2\xA7""17.11"));
}

}  // namespace

// ── The issue's exact repro (purely local, no import) ─────────────────────

BEZEL_TEST(generic_record_default_subst_3986,
           generic_record_class_param_rejected) {
    expectDefaultIllFormedRejected(
        "module GenLocal; "
        "class WidgetGL "
        "  public Count: int; "
        "  public constructor(c: int) Count := c; end constructor; "
        "  public method Render(): int return Count; end Render; "
        "end WidgetGL; "
        "record BoxGL<T>(V: T); "
        "class ProgramGL "
        "  public static method Main(): int "
        "    var x := default(BoxGL<WidgetGL>); "
        "    return x.V.Render(); "
        "  end Main; "
        "end ProgramGL;");
}

// ── Control: the identical field shape with NO generics at all — already
// correctly rejected before this fix; must remain rejected after. ─────────

BEZEL_TEST(generic_record_default_subst_3986, nongeneric_twin_control_rejected) {
    expectDefaultIllFormedRejected(
        "module NG; "
        "class WidgetNG "
        "  public Count: int; "
        "  public constructor(c: int) Count := c; end constructor; "
        "end WidgetNG; "
        "record BoxNG(V: WidgetNG); "
        "class ProgramNG "
        "  public static method Main(): int "
        "    var x := default(BoxNG); "
        "    return x.V.Count; "
        "  end Main; "
        "end ProgramNG;");
}

// ── A bare type parameter instantiated at a value type stays defaultable —
// the substituted field type is `int`, which has a real zero value. ───────

BEZEL_TEST(generic_record_default_subst_3986,
           generic_record_value_param_accepted_zero) {
    auto r = runAot(
        "module V1; "
        "record BoxV1<T>(V: T); "
        "class ProgramV1 "
        "  public static method Main(): int "
        "    var x := default(BoxV1<int>); "
        "    return x.V; "
        "  end Main; "
        "end ProgramV1;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ── A bare type parameter instantiated at a NULLABLE class type stays
// defaultable — the substituted field type is `WidgetN1?`, whose zero is
// `null`. ───────────────────────────────────────────────────────────────

BEZEL_TEST(generic_record_default_subst_3986,
           generic_record_nullable_param_accepted_null) {
    auto r = runAot(
        "module N1; "
        "class WidgetN1 "
        "  public Count: int; "
        "  public constructor(c: int) Count := c; end constructor; "
        "end WidgetN1; "
        "record BoxN1<T>(V: T); "
        "class ProgramN1 "
        "  public static method Main(): int "
        "    var x := default(BoxN1<WidgetN1?>); "
        "    if x.V = null then return 0; end if; "
        "    return 1; "
        "  end Main; "
        "end ProgramN1;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ── Nested generic instantiation: `Box<Box<Widget>>`. Each recursive
// `defaultIllFormed` frame's own `genericArgs` are concrete literals from
// the source, so the fix's per-frame (no composed "incoming" env)
// substitution resolves all the way to the non-defaultable leaf. ─────────

BEZEL_TEST(generic_record_default_subst_3986, nested_generic_record_rejected) {
    expectDefaultIllFormedRejected(
        "module NE1; "
        "class WidgetNE1 "
        "  public Count: int; "
        "  public constructor(c: int) Count := c; end constructor; "
        "end WidgetNE1; "
        "record BoxNE1<T>(V: T); "
        "class ProgramNE1 "
        "  public static method Main(): int "
        "    var x := default(BoxNE1<BoxNE1<WidgetNE1>>); "
        "    return x.V.V.Count; "
        "  end Main; "
        "end ProgramNE1;");
}

// ── Same nesting shape, inner leaf a value type: stays defaultable, and the
// runtime value is the fully-zeroed nested record. ─────────────────────────

BEZEL_TEST(generic_record_default_subst_3986,
           nested_generic_record_value_accepted_zero) {
    auto r = runAot(
        "module NE2; "
        "record BoxNE2<T>(V: T); "
        "class ProgramNE2 "
        "  public static method Main(): int "
        "    var x := default(BoxNE2<BoxNE2<int>>); "
        "    return x.V.V; "
        "  end Main; "
        "end ProgramNE2;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ── fix #3995: DELEGATING generic instantiation -- a DIFFERENT record type
// wraps the parameter (`record InnerDG1<T>(V: T); record
// OuterDG1<T>(In: InnerDG1<T>)`), unlike the self-referential nesting
// above. `InnerDG1<T>`'s own generic argument is a BARE MENTION of
// OuterDG1's `T`, which resolves only if `defaultIllFormed`'s recursive
// call into the inner frame is seeded with the OUTER frame's environment
// as `incoming` -- exactly the composition
// `common::isBorrowBearingRecordCommon` performs for the identically-shaped
// §9.7 walk. Before #3995 this compiled clean and SIGSEGV'd identically to
// the issue's own repro. ────────────────────────────────────────────────

BEZEL_TEST(generic_record_default_subst_3986, delegating_generic_record_rejected) {
    expectDefaultIllFormedRejected(
        "module DG1; "
        "class WidgetDG1 "
        "  public Count: int; "
        "  public constructor(c: int) Count := c; end constructor; "
        "  public method Render(): int return Count; end Render; "
        "end WidgetDG1; "
        "record InnerDG1<T>(V: T); "
        "record OuterDG1<T>(In: InnerDG1<T>); "
        "class ProgramDG1 "
        "  public static method Main(): int "
        "    var x := default(OuterDG1<WidgetDG1>); "
        "    return x.In.V.Render(); "
        "  end Main; "
        "end ProgramDG1;");
}

// ── Same delegating shape, inner leaf a value type: stays defaultable, and
// the runtime value is the fully-zeroed delegating record. ────────────────

BEZEL_TEST(generic_record_default_subst_3986,
           delegating_generic_record_value_leaf_accepted_zero) {
    auto r = runAot(
        "module DG2; "
        "record InnerDG2<T>(V: T); "
        "record OuterDG2<T>(In: InnerDG2<T>); "
        "class ProgramDG2 "
        "  public static method Main(): int "
        "    var x := default(OuterDG2<int>); "
        "    return x.In.V; "
        "  end Main; "
        "end ProgramDG2;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ── A generic record used as an inline array's element type: the
// InlineArray recursion arm (line ~401) calls `defaultIllFormed` on the
// element type, which must ALSO see the substituted field. ────────────────

BEZEL_TEST(generic_record_default_subst_3986,
           inline_array_of_generic_record_rejected) {
    // The top-level type here is the InlineArray itself, not the record, so
    // the diagnostic's `detail` clause is the InlineArray wording (see
    // ExprTypecheck_Exprs.cpp's `default` diagnostic, ~line 4596) — but
    // reaching it AT ALL depends on this fix: `defaultIllFormed` recurses
    // into the array's element type (line ~401), which is the Record arm
    // this fix changes, so the pre-fix compiler accepted this program too.
    auto r = runAot(
        "module IA1; "
        "class WidgetIA1 "
        "  public Count: int; "
        "  public constructor(c: int) Count := c; end constructor; "
        "end WidgetIA1; "
        "record BoxIA1<T>(V: T); "
        "class ProgramIA1 "
        "  public static method Main(): int "
        "    var arr := default([2]BoxIA1<WidgetIA1>); "
        "    return arr[0].V.Count; "
        "  end Main; "
        "end ProgramIA1;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r,
        "`default(T)` is a compile error for an inline array whose element "
        "type is a non-nullable reference type"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "\xC2\xA7""17.11"));
}

// ── Two type parameters: only the SECOND is bound to a non-nullable
// reference type. The walk must reject on the offending field even though
// the first field substitutes to a perfectly defaultable value type. ──────

BEZEL_TEST(generic_record_default_subst_3986, multi_param_one_bad_rejected) {
    expectDefaultIllFormedRejected(
        "module MP1; "
        "class WidgetMP1 "
        "  public Count: int; "
        "  public constructor(c: int) Count := c; end constructor; "
        "end WidgetMP1; "
        "record MultiMP1<A,B>(X: A, Y: B); "
        "class ProgramMP1 "
        "  public static method Main(): int "
        "    var x := default(MultiMP1<int, WidgetMP1>); "
        "    return x.X + x.Y.Count; "
        "  end Main; "
        "end ProgramMP1;");
}

// ── Same two-parameter record, both substitutions now defaultable
// (value type + nullable reference type): accepted, with both fields at
// their per-kind zero value. ───────────────────────────────────────────────

BEZEL_TEST(generic_record_default_subst_3986, multi_param_all_ok_accepted) {
    auto r = runAot(
        "module MP2; "
        "class WidgetMP2 "
        "  public Count: int; "
        "  public constructor(c: int) Count := c; end constructor; "
        "end WidgetMP2; "
        "record MultiMP2<A,B>(X: A, Y: B); "
        "class ProgramMP2 "
        "  public static method Main(): int "
        "    var x := default(MultiMP2<int, WidgetMP2?>); "
        "    if x.X = 0 and x.Y = null then return 0; end if; "
        "    return 1; "
        "  end Main; "
        "end ProgramMP2;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ── Generic CHOICE, for comparison: §35.10 rejects `default(C)`
// unconditionally for EVERY choice, independent of substitution — this arm
// (`if (k == ast::TypeDeclKind::Choice) return true;`) is untouched by this
// fix and must keep rejecting. ─────────────────────────────────────────────

BEZEL_TEST(generic_record_default_subst_3986, generic_choice_control_rejected) {
    auto r = runAot(
        "module GC1; "
        "class WidgetGC1 "
        "  public Count: int; "
        "  public constructor(c: int) Count := c; end constructor; "
        "end WidgetGC1; "
        "choice OptGC1<T> "
        "  Some(V: T), None; "
        "end OptGC1; "
        "class ProgramGC1 "
        "  public static method Main(): int "
        "    var x := default(OptGC1<WidgetGC1>); "
        "    return 0; "
        "  end Main; "
        "end ProgramGC1;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`default(T)` is a compile error for a choice type"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "\xC2\xA7""17.11"));
}

// ── Generic CLASS, for comparison: a class is always a non-nullable
// reference type regardless of its own generic parameters — this arm
// (`if (k == Class || k == Interface) return !isValueBuiltin;`) is untouched
// by this fix and must keep rejecting. ─────────────────────────────────────

BEZEL_TEST(generic_record_default_subst_3986, generic_class_control_rejected) {
    auto r = runAot(
        "module GCL1; "
        "class HolderGCL1<T> "
        "  public V: T; "
        "  public constructor(v: T) V := v; end constructor; "
        "end HolderGCL1; "
        "class ProgramGCL1 "
        "  public static method Main(): int "
        "    var x := default(HolderGCL1<int>); "
        "    return 0; "
        "  end Main; "
        "end ProgramGCL1;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`default(T)` is a compile error for a non-nullable reference "
           "type"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "\xC2\xA7""17.11"));
}

// ── #5820 (§17.11 v0.338, generic-body list bullet 1; §3.6 rule 2): an
// UNCONSTRAINED `default(T)` is rejected at every non-defaultable substitution,
// reference types included. Spec text, docs/Bezel_Language_Spec.md v0.338:
//   "For unconstrained `T`, `default(T)` is permitted and has type `T`: the
//    rule below rejects at the instantiation site every substitution that is
//    not **defaultable** — a non-nullable reference type (a class, an
//    interface, `Object`, `string`, a function type, a task type), a choice,
//    an enum with no zero-valued constant, a constructor-only record, a record
//    or inline array with a non-defaultable component …"

namespace {

void expectUnconstrainedDefaultRejected(const std::string& program) {
    auto r = runAot(program);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "is not defaultable (\xC2\xA7""17.11)"));
}

}  // namespace

// Method-level unconstrained T, instantiated at `string` (the issue's repro).
BEZEL_TEST(generic_default_unconstrained_5820,
           method_string_substitution_rejected_spec_17_11) {
    expectUnconstrainedDefaultRejected(
        "module U5820a; "
        "static class Util5820a "
        "  public static method Z<T>(): int "
        "    var t := default(T); "
        "    return 7; "
        "  end Z; "
        "end Util5820a; "
        "class P5820a "
        "  public static method Main(): int "
        "    return Util5820a.Z<string>(); "
        "  end Main; "
        "end P5820a;");
}

// Same shape at a class substitution: a non-nullable class is non-defaultable.
BEZEL_TEST(generic_default_unconstrained_5820,
           method_class_substitution_rejected_spec_17_11) {
    expectUnconstrainedDefaultRejected(
        "module U5820b; "
        "class Widget5820b "
        "  public Count: int; "
        "  public constructor(c: int) Count := c; end constructor; "
        "end Widget5820b; "
        "static class Util5820b "
        "  public static method Z<T>(): int "
        "    var t := default(T); "
        "    return 7; "
        "  end Z; "
        "end Util5820b; "
        "class P5820b "
        "  public static method Main(): int "
        "    return Util5820b.Z<Widget5820b>(); "
        "  end Main; "
        "end P5820b;");
}

// Admitted substitution: a nullable reference type is defaultable (null).
// Must RUN (§17.11: `null` exactly at a nullable substitution).
BEZEL_TEST(generic_default_unconstrained_5820,
           method_nullable_string_substitution_runs_spec_17_11) {
    auto r = runAot(
        "module U5820c; "
        "static class Util5820c "
        "  public static method Z<T>(): int "
        "    var t := default(T); "
        "    if t = null then return 5; end if; "
        "    return 6; "
        "  end Z; "
        "end Util5820c; "
        "class P5820c "
        "  public static method Main(): int "
        "    return Util5820c.Z<string?>(); "
        "  end Main; "
        "end P5820c;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// Admitted substitution: a value type (`int`) — the zero value, not null.
BEZEL_TEST(generic_default_unconstrained_5820,
           method_int_substitution_runs_zero_spec_17_11) {
    auto r = runAot(
        "module U5820d; "
        "static class Util5820d "
        "  public static method Z<T>(): owned T "
        "    return default(T); "
        "  end Z; "
        "end Util5820d; "
        "class P5820d "
        "  public static method Main(): int "
        "    return Util5820d.Z<int>() + 7; "
        "  end Main; "
        "end P5820d;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}
