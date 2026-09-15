// fix #4164 (§9.7 "Records and disposal", v0.298 — text unchanged since
// v0.295): fix #4133 seeded a `common::RecordSubstEnv` from a generic
// record/choice's own parameters against its concrete instantiation and
// substituted each field/payload's DECLARED type through it — but only when
// that declared type was ITSELF a bare type-parameter reference (`V: T`).
// `checkStaticReadonlyRecordInit` (src/sema/TypeResolution_Checks.cpp) never
// threaded that substitution environment into its own recursive descent
// (fix #3973's nested-slot walk, `checkNestedBorrowBearingSlot`): every
// nested frame re-seeded its own environment from a hardcoded
// `/*incoming=*/nullptr`. So a field whose declared type is a COMPOUND
// generic type that itself mentions the ENCLOSING record's own bare
// parameter (`I: InnerCG<T>` as a field of `record OuterCG<T>(I:
// InnerCG<T>)`) recursed with `T` still unresolved: the nested frame's own
// `seedRecordSubstEnv` mapped `InnerCG`'s parameter to the STILL-BARE `T`
// (no `incoming` to resolve it through), and the leaf field `V: T` then
// resolved to a bare, un-substitutable parameter with nothing to bind it —
// so `staticReadonlyFieldIsReferenceTyped` could never classify it as
// reference-typed, no matter how reference-heavy the outermost concrete
// instantiation actually was.
//
// MEASURED (issue's own repro, verbatim):
//
//     module MCompGap;
//     record InnerCG<T>(V: T);
//     record OuterCG<T>(I: InnerCG<T>);
//     class WidgetCG
//       public method Render(): int return 42; end Render;
//     end WidgetCG;
//     class HolderCG
//       public static readonly F: OuterCG<WidgetCG> :=
//         new OuterCG<WidgetCG>(new InnerCG<WidgetCG>(new WidgetCG()));
//     end HolderCG;
//     class ProgramCG public static method Main(): int return 0; end Main; end ProgramCG;
//
//   compiled at exit 0 with ZERO diagnostic — `F` is a static readonly field
//   holding a live, non-null class reference (`WidgetCG`), reached through
//   two levels of generic record, with no §9.7 enforcement at all.
//
// ── FIX ─────────────────────────────────────────────────────────────────
// Thread an `incoming` `const common::RecordSubstEnv*` parameter through
// `checkStaticReadonlyRecordInit`, `checkNestedBorrowBearingSlot`, and
// `checkStaticReadonlyChoiceCaseInit`'s recursive calls (null at the top of
// the recursion — the static readonly field's own declared type carries its
// own concrete `genericArgs` there already). Each frame now seeds its OWN
// environment through `incoming` instead of a hardcoded `nullptr` — exactly
// the "one composition hop" `common::RecordSubstEnv::Entry::env` exists for
// (`seedRecordSubstEnv`'s own header contract), and exactly what
// `common::isBorrowBearingRecordCommon`'s walk — the ORIGINAL §9.7
// dangling-borrow consumer of this same machinery — already does when it
// recurses into a Record-kind field. The per-field loops now capture the
// FULL `RecordSubstEnv::Entry` (both `.ty` and `.env`) returned by
// `common::resolveRecordFieldSubst` instead of discarding `.env`, and pass
// that `.env` down as the nested frame's `incoming` — for a compound field
// like `InnerCG<T>`, `resolveRecordFieldSubst` returns `{ty: InnerCG<T>
// (unchanged), env: <the current frame's env, which already binds T>}`,
// and `seedRecordSubstEnv` on the next frame resolves `InnerCG<T>`'s own
// bare `T` argument through that `incoming` env before it is ever stored —
// so the enclosing binding survives the hop with no new TypeRef synthesis
// required.
//
// ── SHAPE SWEEP (§15 accept-side sweep) ────────────────────────────────────
//   REJECT:
//     - the issue's own headline: a generic record's compound generic field
//       (`OuterCG<T>(I: InnerCG<T>)`), reference instantiation
//     - the identical composition gap through a compound CHOICE-typed
//       nested field (`OuterCGB<T>(I: OptCGB<T>)`, real `Type.Case(...)`
//       construction), proving the fix also applies at
//       `checkStaticReadonlyChoiceCaseInit`
//     - THREE levels of composition (`Outer3<T>(M: Mid3<T>)`,
//       `Mid3<T>(I: Inner3<T>)`, `Inner3<T>(V: T)`), proving `incoming`
//       composes across more than one hop, not just the issue's two
//   ACCEPT:
//     - the identical two-level compound-field shape instantiated at a
//       VALUE type (int) — the pre-existing, already-correct behaviour for
//       an uninstantiated/no-op substitution must not regress
//     - the identical shape instantiated at a NULLABLE reference type,
//       correctly null-sourced
//     - the three-level chain instantiated at a value type (int)
//
// Spec citations verified verbatim against docs/Bezel_Language_Spec.md at
// v0.298: §9.7 "Records and disposal" (the static-field bullet's text is
// unchanged since v0.295, when fix #4133/#3973/#4134 pinned their own
// citations at this section — confirmed via docs/Bezel_Changelog.md's
// v0.296/v0.297/v0.298 entries, none of which touch §9.7).

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation, diagnosticMessageContains
#include "support/aot_support.hpp"         // runAot
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

// =========================================================================
// REJECT: a compound generic field mentioning the ENCLOSING generic's own
// bare parameter must compose the substitution through nesting, not just a
// bare field directly.
// =========================================================================

// REJECT (headline): the issue's own measured repro, renamed for this file.
BEZEL_TEST(static_readonly_nested_compound_generic_subst_4164,
           depth2_compound_generic_record_field_reference_instantiation_rejected_spec_9_7_issue_4164) {
    const char* prog =
        "module M4164A; "
        "record InnerCG4164A<T>(V: T); "
        "record OuterCG4164A<T>(I: InnerCG4164A<T>); "
        "class WidgetCG4164A "
        "  public method Render(): int return 42; end Render; "
        "end WidgetCG4164A; "
        "class HolderCG4164A "
        "  public static readonly F: OuterCG4164A<WidgetCG4164A> := "
        "    new OuterCG4164A<WidgetCG4164A>(new InnerCG4164A<WidgetCG4164A>(new WidgetCG4164A())); "
        "end HolderCG4164A; "
        "class ProgramCG4164A public static method Main(): int return 0; "
        "end Main; end ProgramCG4164A;";
    auto r = runAot(prog);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "has record value with non-null reference-typed field `V`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "\xC2\xA7""9.7 requires the null-source form"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "`F`"));
}

// REJECT (compound CHOICE-typed nested field): the identical composition
// gap through `checkNestedBorrowBearingSlot`'s Choice arm and fix #4134's
// `checkStaticReadonlyChoiceCaseInit` — the nested slot's own type is a
// generic choice, constructed via the real `Type.Case(...)` spelling.
BEZEL_TEST(static_readonly_nested_compound_generic_subst_4164,
           depth2_compound_generic_choice_field_reference_instantiation_rejected_spec_9_7_issue_4164) {
    const char* prog =
        "module M4164B; "
        "public choice OptCG4164B<T> Some(V: T), None(); end OptCG4164B; "
        "record OuterCG4164B<T>(I: OptCG4164B<T>); "
        "class WidgetCG4164B "
        "  public method Render(): int return 42; end Render; "
        "end WidgetCG4164B; "
        "class HolderCG4164B "
        "  public static readonly F: OuterCG4164B<WidgetCG4164B> := "
        "    new OuterCG4164B<WidgetCG4164B>(OptCG4164B<WidgetCG4164B>.Some(new WidgetCG4164B())); "
        "end HolderCG4164B; "
        "class ProgramCG4164B public static method Main(): int return 0; "
        "end Main; end ProgramCG4164B;";
    auto r = runAot(prog);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "has choice value with non-null reference-typed payload `V`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "\xC2\xA7""9.7 requires the null-source form"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "`F`"));
}

// REJECT (three levels of composition): proves `incoming` threads through
// MORE than one hop, not just the issue's own two-level repro.
BEZEL_TEST(static_readonly_nested_compound_generic_subst_4164,
           depth3_compound_generic_record_chain_reference_instantiation_rejected_spec_9_7_issue_4164) {
    const char* prog =
        "module M4164C; "
        "record Inner4164C<T>(V: T); "
        "record Mid4164C<T>(I: Inner4164C<T>); "
        "record Outer4164C<T>(M: Mid4164C<T>); "
        "class Widget4164C "
        "  public method Render(): int return 42; end Render; "
        "end Widget4164C; "
        "class Holder4164C "
        "  public static readonly F: Outer4164C<Widget4164C> := "
        "    new Outer4164C<Widget4164C>(new Mid4164C<Widget4164C>(new Inner4164C<Widget4164C>(new Widget4164C()))); "
        "end Holder4164C; "
        "class Program4164C public static method Main(): int return 0; "
        "end Main; end Program4164C;";
    auto r = runAot(prog);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "has record value with non-null reference-typed field `V`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "\xC2\xA7""9.7 requires the null-source form"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "`F`"));
}

// =========================================================================
// ACCEPT-SIDE SWEEP (§15 of the lane rules): every legitimate shape below
// must keep compiling — a false reject is worse than the bug this fixes.
// =========================================================================

// ACCEPT (value instantiation): the headline's exact compound-field shape,
// instantiated at a plain value type (int) instead of a reference type.
BEZEL_TEST(static_readonly_nested_compound_generic_subst_4164,
           depth2_compound_generic_record_field_value_instantiation_accepted) {
    const char* prog =
        "module M4164D; "
        "record InnerCG4164D<T>(V: T); "
        "record OuterCG4164D<T>(I: InnerCG4164D<T>); "
        "class HolderCG4164D "
        "  public static readonly F: OuterCG4164D<int> := "
        "    new OuterCG4164D<int>(new InnerCG4164D<int>(11)); "
        "end HolderCG4164D; "
        "class ProgramCG4164D public static method Main(): int "
        "  return HolderCG4164D.F.I.V; "
        "end Main; end ProgramCG4164D;";
    auto r = runAot(prog);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 11);
}

// ACCEPT (nullable reference instantiation, null-source): the headline's
// compound-field shape instantiated at a NULLABLE class type, correctly
// initialized to `null` at the leaf.
BEZEL_TEST(static_readonly_nested_compound_generic_subst_4164,
           depth2_compound_generic_record_field_nullable_reference_null_source_accepted) {
    const char* prog =
        "module M4164E; "
        "record InnerCG4164E<T>(V: T); "
        "record OuterCG4164E<T>(I: InnerCG4164E<T>); "
        "class WidgetCG4164E "
        "  public method Render(): int return 42; end Render; "
        "end WidgetCG4164E; "
        "class HolderCG4164E "
        "  public static readonly F: OuterCG4164E<WidgetCG4164E?> := "
        "    new OuterCG4164E<WidgetCG4164E?>(new InnerCG4164E<WidgetCG4164E?>(null)); "
        "end HolderCG4164E; "
        "class ProgramCG4164E public static method Main(): int return 12; "
        "end Main; end ProgramCG4164E;";
    auto r = runAot(prog);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 12);
}

// ACCEPT (three-level chain, value instantiation): the depth-3 REJECT
// control's exact shape, instantiated at `int`.
BEZEL_TEST(static_readonly_nested_compound_generic_subst_4164,
           depth3_compound_generic_record_chain_value_instantiation_accepted) {
    const char* prog =
        "module M4164F; "
        "record Inner4164F<T>(V: T); "
        "record Mid4164F<T>(I: Inner4164F<T>); "
        "record Outer4164F<T>(M: Mid4164F<T>); "
        "class Holder4164F "
        "  public static readonly F: Outer4164F<int> := "
        "    new Outer4164F<int>(new Mid4164F<int>(new Inner4164F<int>(13))); "
        "end Holder4164F; "
        "class Program4164F public static method Main(): int "
        "  return Holder4164F.F.M.I.V; "
        "end Main; end Program4164F;";
    auto r = runAot(prog);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 13);
}
