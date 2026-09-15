// fix #4133 (§9.7 "Records and disposal", v0.295): `checkStaticReadonlyRecordInit`
// (src/sema/TypeResolution_Checks.cpp) reads a positional field/payload's
// type off the DECLARATION side via `storageComponentType` (records) or
// `ctx.resolvedType(*pf.type)` (choices). For a GENERIC record/choice field
// declared as a bare type parameter (`V: T` in `record Box<T>(V: T)`), that
// declaration-side type is always the unsubstituted `T` itself, no matter
// what a given static readonly field's concrete instantiation (`Box<Widget>`)
// actually binds `T` to. `staticReadonlyFieldIsReferenceTyped` can then never
// classify the field as reference-typed, because a bare, declaration-less
// `Named` TypeRef with head "T" matches none of its recognized builtin
// reference-type names — so the field is (wrongly, in the unsafe direction)
// always treated as value-typed, EVEN AT DEPTH 1 (no nesting or recursion
// needed for this gap to manifest; unaffected either way by fix #3973's
// nested-slot recursion).
//
// MEASURED (issue's own repro, verbatim):
//
//     module MSweep6;
//     record BoxS6<T>(V: T);
//     class WidgetS6
//       public method Render(): int return 42; end Render;
//     end WidgetS6;
//     class HolderS6
//       public static readonly F: BoxS6<WidgetS6> := new BoxS6<WidgetS6>(new WidgetS6());
//     end HolderS6;
//     class ProgramS6 public static method Main(): int return 0; end Main; end ProgramS6;
//
//   compiled at exit 0 with ZERO diagnostic of any kind — `F` is a static
//   readonly field holding a live, non-null class reference (`WidgetS6`)
//   with no §9.7 enforcement at all.
//
// ── FIX ─────────────────────────────────────────────────────────────────
// Seed a `common::RecordSubstEnv` (src/common/OwnershipFacts.hpp) from the
// record/choice's own generic parameters against the concrete
// instantiation's `genericArgs` — the SAME machinery fix #4002 uses for the
// identically-shaped `where T: new()` gap (this file's `where T: new()` arm)
// — and substitute each field/payload's declaration-side type through it
// before classifying. Applied uniformly to all three sites that read a
// field/payload's declaration-side type in this function family: the record
// arm and the NewExpr-driven choice arm of `checkStaticReadonlyRecordInit`,
// and fix #4134's `checkStaticReadonlyChoiceCaseInit` (the real
// `Type.Case(...)` choice-construction path).
//
// ── SHAPE SWEEP (§15 accept-side sweep; see commit body for FAIL-WITHOUT /
//    PASS-WITH counts and filters run) ─────────────────────────────────────
//   REJECT:
//     - depth-1 generic record, reference instantiation, non-null (headline)
//     - depth-1 generic CHOICE, reference instantiation, non-null (the
//       identical gap in `Type.Case(...)`-constructed generic choices)
//     - the enclosing type declaring the static readonly field is a
//       `record`, not a `class`
//     - depth-2: a generic record nested inside a plain record, itself
//       instantiated at a reference type — proves the substitution composes
//       with fix #3973's own nested-slot recursion
//   ACCEPT:
//     - the same generic record instantiated at a VALUE type (int) — the
//       pre-existing, already-correct behaviour must not regress
//     - a generic record instantiated at a NULLABLE reference type,
//       correctly null-sourced
//     - a generic CHOICE instantiated at a value type (int)
//     - depth-2 nested generic record, instantiated at a value type
//
// Spec citations verified verbatim against docs/Bezel_Language_Spec.md at
// v0.295: §9.7 "Records and disposal".

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation, diagnosticMessageContains
#include "support/aot_support.hpp"         // runAot
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

// =========================================================================
// REJECT: a generic record/choice field instantiated at a reference type
// must be enforced, even at depth 1.
// =========================================================================

// REJECT (headline, depth 1): the issue's own measured repro, renamed for
// this file. No nesting/recursion involved at all.
BEZEL_TEST(static_readonly_generic_field_reference_instantiation_4133,
           depth1_generic_record_reference_instantiation_rejected_spec_9_7_issue_4133) {
    const char* prog =
        "module M4133A; "
        "record Box4133A<T>(V: T); "
        "class Widget4133A "
        "  public method Render(): int return 42; end Render; "
        "end Widget4133A; "
        "class Holder4133A "
        "  public static readonly F: Box4133A<Widget4133A> := "
        "    new Box4133A<Widget4133A>(new Widget4133A()); "
        "end Holder4133A; "
        "class Program4133A public static method Main(): int return 0; "
        "end Main; end Program4133A;";
    auto r = runAot(prog);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "has record value with non-null reference-typed field `V`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "\xC2\xA7""9.7 requires the null-source form"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "`F`"));
}

// REJECT (generic CHOICE, depth 1): the identical gap in a generic choice's
// payload, constructed via the real `Type.Case(...)` spelling (fix #4134's
// checkStaticReadonlyChoiceCaseInit).
BEZEL_TEST(static_readonly_generic_field_reference_instantiation_4133,
           depth1_generic_choice_reference_instantiation_rejected_spec_9_7_issue_4133) {
    const char* prog =
        "module M4133B; "
        "public choice Opt4133B<T> Some(V: T), None(); end Opt4133B; "
        "class Widget4133B "
        "  public method Render(): int return 42; end Render; "
        "end Widget4133B; "
        "class Holder4133B "
        "  public static readonly F: Opt4133B<Widget4133B> := "
        "    Opt4133B<Widget4133B>.Some(new Widget4133B()); "
        "end Holder4133B; "
        "class Program4133B public static method Main(): int return 0; "
        "end Main; end Program4133B;";
    auto r = runAot(prog);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "has choice value with non-null reference-typed payload `V`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "\xC2\xA7""9.7 requires the null-source form"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "`F`"));
}

// REJECT (enclosing type is a RECORD): §9.7 governs any type declaring a
// static readonly field, not just classes.
BEZEL_TEST(static_readonly_generic_field_reference_instantiation_4133,
           record_enclosing_type_reference_instantiation_rejected_spec_9_7_issue_4133) {
    const char* prog =
        "module M4133C; "
        "record Box4133C<T>(V: T); "
        "class Widget4133C "
        "  public method Render(): int return 42; end Render; "
        "end Widget4133C; "
        "record Holder4133C "
        "  public static readonly F: Box4133C<Widget4133C> := "
        "    new Box4133C<Widget4133C>(new Widget4133C()); "
        "end Holder4133C; "
        "class Program4133C public static method Main(): int return 0; "
        "end Main; end Program4133C;";
    auto r = runAot(prog);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "has record value with non-null reference-typed field `V`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "\xC2\xA7""9.7 requires the null-source form"));
}

// REJECT (depth 2, composes with fix #3973's recursion): a generic record
// nested INSIDE a plain (non-generic) record, itself instantiated at a
// reference type.
BEZEL_TEST(static_readonly_generic_field_reference_instantiation_4133,
           depth2_nested_generic_record_reference_instantiation_rejected_spec_9_7_issue_4133) {
    const char* prog =
        "module M4133D; "
        "record Box4133D<T>(V: T); "
        "record Wrap4133D(B: Box4133D<Widget4133D>); "
        "class Widget4133D "
        "  public method Render(): int return 42; end Render; "
        "end Widget4133D; "
        "class Holder4133D "
        "  public static readonly F: Wrap4133D := "
        "    new Wrap4133D(new Box4133D<Widget4133D>(new Widget4133D())); "
        "end Holder4133D; "
        "class Program4133D public static method Main(): int return 0; "
        "end Main; end Program4133D;";
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

// ACCEPT (value instantiation): the SAME generic record instantiated at a
// plain value type (int) — the pre-existing, already-correct behaviour.
BEZEL_TEST(static_readonly_generic_field_reference_instantiation_4133,
           generic_record_value_instantiation_accepted) {
    const char* prog =
        "module M4133E; "
        "record Box4133E<T>(V: T); "
        "class Holder4133E "
        "  public static readonly F: Box4133E<int> := new Box4133E<int>(7); "
        "end Holder4133E; "
        "class Program4133E public static method Main(): int return Holder4133E.F.V; "
        "end Main; end Program4133E;";
    auto r = runAot(prog);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// ACCEPT (nullable reference instantiation, null-source): the generic field
// instantiated at a NULLABLE class type, correctly initialized to `null`.
BEZEL_TEST(static_readonly_generic_field_reference_instantiation_4133,
           generic_record_nullable_reference_instantiation_null_source_accepted) {
    const char* prog =
        "module M4133F; "
        "record Box4133F<T>(V: T); "
        "class Widget4133F "
        "  public method Render(): int return 42; end Render; "
        "end Widget4133F; "
        "class Holder4133F "
        "  public static readonly F: Box4133F<Widget4133F?> := "
        "    new Box4133F<Widget4133F?>(null); "
        "end Holder4133F; "
        "class Program4133F public static method Main(): int return 8; "
        "end Main; end Program4133F;";
    auto r = runAot(prog);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 8);
}

// ACCEPT (generic choice, value instantiation): the same generic choice
// used in the reject-side control, instantiated at `int`.
BEZEL_TEST(static_readonly_generic_field_reference_instantiation_4133,
           generic_choice_value_instantiation_accepted) {
    const char* prog =
        "module M4133G; "
        "public choice Opt4133G<T> Some(V: T), None(); end Opt4133G; "
        "class Holder4133G "
        "  public static readonly F: Opt4133G<int> := Opt4133G<int>.Some(9); "
        "end Holder4133G; "
        "class Program4133G public static method Main(): int return 10; "
        "end Main; end Program4133G;";
    auto r = runAot(prog);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 10);
}

// ACCEPT (depth 2, nested generic record, value instantiation): the
// reject-side depth-2 control's shape, but instantiated at a value type.
BEZEL_TEST(static_readonly_generic_field_reference_instantiation_4133,
           depth2_nested_generic_record_value_instantiation_accepted) {
    const char* prog =
        "module M4133H; "
        "record Box4133H<T>(V: T); "
        "record Wrap4133H(B: Box4133H<int>); "
        "class Holder4133H "
        "  public static readonly F: Wrap4133H := new Wrap4133H(new Box4133H<int>(3)); "
        "end Holder4133H; "
        "class Program4133H public static method Main(): int return Holder4133H.F.B.V; "
        "end Main; end Program4133H;";
    auto r = runAot(prog);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3);
}
