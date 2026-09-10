// Spec-derived tests for fix #4176 — §3.1 "Type kinds", §3.2 "Reference vs.
// value types" (interface half); §11.7 "Static classes" (static half, and
// `closed`'s own "cannot be instantiated directly" definition); §12.5
// "Abstract classes and members" (abstract/closed half, verbatim: "`new` on
// an abstract or `closed` class is a compile error."); §17.7 "The `new`
// operator" (the target-typed `new()` shorthand itself, whose enumeration —
// "For classes... For records..." — never lists interfaces) (v0.298).
//
// Filed by a reviewer sweeping fix #4170's accept side: #4170 closed the
// §17.7 shorthand gap for Enum/Choice (`ExprTC::rejectNewOnEnumOrChoice`,
// called from the explicit `new T(...)` arm and both shorthand call sites
// in ExprTypecheck.cpp), but a SIBLING check right next to the one #4170
// touched — "Slice S11" in ExprTypecheck_Exprs.cpp, which rejects `new` on
// an abstract class, a `closed` class, a `static` class, and an interface —
// was NEVER reachable from either §17.7 shorthand call site at all: both
// sites gated their `checkNewExprArgs` call on `TypeDeclKind::Class` alone,
// with no path to Slice S11's four-way classification. So:
//
//   using var w: ISomeInterface := new();   // no implementing class in scope
//
// compiled clean (exit 0, no diagnostic) and SEGFAULTed at runtime (exit
// 139) on first virtual dispatch through the mis-constructed reference — no
// vtable was ever installed, because codegen's lowerNew has no interface
// arm at all; nothing sema does before this fix catches the shape.
//
// MEASURED, pre-fix (this issue's own repro, verbatim, direct `bezelc`
// invocation, base commit fbfd7a1):
//   `using var w: IWidgetIF4170 := new(); return w.N();` — compile exit 0,
//   run: "Segmentation fault (core dumped)", exit 139 (signal 11).
//   The EXPLICIT spelling `new IWidgetIF4170()` was ALREADY correctly
//   rejected before this fix (Slice S11 lives in the explicit arm) —
//   "cannot instantiate interface `IWidgetIF4170E` with `new` (§11.2)",
//   exit 1 — so only the shorthand path needed closing.
//
// ABSTRACT-CLASS SCOPE DECISION (LANE_RULES §14176 / brief's "abstract
// classes are the trap"): `new AbstractThing()` through the shorthand hits
// the IDENTICAL SIGSEGV, for the identical reason (Slice S11 unreachable
// from the shorthand) — measured, exit 139. This fix closes it TOO, in the
// SAME commit, by factoring Slice S11's entire four-way classification
// (abstract/closed/static/interface) into one shared predicate,
// `ExprTC::rejectNewOnNonInstantiableKind` (ExprTypecheck_impl.hpp /
// ExprTypecheck_Exprs.cpp, beside `rejectNewOnEnumOrChoice`), rather than
// hand-picking only the Interface arm out of a block that has always lived
// as one four-way decision — CLAUDE.md / LANE_RULES.md §6 ("prefer feeding
// the existing authority" over a second, narrower spelling of the same
// classification). `closed`/`static` class shorthand come along as a
// byproduct: measured, NEITHER crashes pre-fix (both are otherwise fully
// concrete — no unresolved vtable slot, no missing override — so the
// mis-construction was silently ACCEPTED rather than fatal), but both are
// still real, silent over-acceptances this fix now rejects with the same
// diagnostics the explicit spelling has always produced for them.
//
// FIX: `rejectNewOnNonInstantiableKind(ne, rt)` reports and returns true
// iff `rt` names an abstract class, a `closed` class, a `static` class, or
// an interface — the exact logic the explicit `new T(...)` NewExpr-typing
// arm already had inline ("Slice S11") — and is now called from all THREE
// `new`-typing sites: the explicit arm (replacing its inline chain,
// diagnostic text/citations unchanged — pure factoring) and both §17.7
// shorthand sites in ExprTypecheck.cpp (field-initializer and local-decl),
// guarding their existing `checkNewExprArgs` call exactly as
// `rejectNewOnEnumOrChoice` already guards it for Enum/Choice.
//
// Because the pre-fix INTERFACE/ABSTRACT symptom is a runtime SIGSEGV of
// the COMPILED PROGRAM (not a compiler crash), `runAot` reports it via its
// "program terminated by signal 11" synthetic diagnostic at
// `SourceLocation{}` (aot_support.hpp) — `failedAtCompileTimeWithRealLocation`
// (LANE_RULES §18) explicitly excludes this text, so those rows read FALSE
// pre-fix (a runtime crash, not a real compile-time rejection) and TRUE
// post-fix (a real `<path>:<line>:<col>:` diagnostic with the §-cited
// text). The pre-fix CLOSED/STATIC symptom is different again — the
// program compiles AND RUNS successfully (`r.ran == true`) — which also
// reads FALSE through the same helper (its very first line is `if (r.ran)
// return false;`), for the correct, simpler reason: nothing failed at all.
// Message-text is asserted on every reject row regardless, per §18.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

int runProg(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// =====================================================================
// REJECT: interface, via the §17.7 shorthand — the issue's own repro.
// =====================================================================

BEZEL_TEST(sema_new_shorthand_interface_abstract_construction_rejected_4176,
           new_interface_local_decl_shorthand_no_longer_segfaults_rejected_spec_3_1_3_2_17_7) {
    auto r = runAot(
        "module M; "
        "public interface IWidgetIF4176 "
        "  method N(): int; "
        "end IWidgetIF4176; "
        "class ProgramIF4176 "
        "  public static method Main(): int "
        "    using var w: IWidgetIF4176 := new(); "
        "    return w.N(); "
        "  end Main; "
        "end ProgramIF4176;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "cannot instantiate interface"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "§11.2"));
}

BEZEL_TEST(sema_new_shorthand_interface_abstract_construction_rejected_4176,
           new_interface_field_initializer_shorthand_rejected_spec_3_1_3_2_17_7) {
    auto r = runAot(
        "module M; "
        "public interface IWidgetFieldIF4176 "
        "  method N(): int; "
        "end IWidgetFieldIF4176; "
        "class HolderFieldIF4176 "
        "  public owned W: IWidgetFieldIF4176 := new(); "
        "end HolderFieldIF4176; "
        "class ProgramFieldIF4176 "
        "  public static method Main(): int "
        "    using var h: HolderFieldIF4176 := new HolderFieldIF4176(); "
        "    return 0; "
        "  end Main; "
        "end ProgramFieldIF4176;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "cannot instantiate interface"));
}

BEZEL_TEST(sema_new_shorthand_interface_abstract_construction_rejected_4176,
           new_interface_local_decl_shorthand_via_type_alias_rejected_spec_3_1_3_2_17_7) {
    auto r = runAot(
        "module M; "
        "public interface IAliasedIfaceIF4176 "
        "  method N(): int; "
        "end IAliasedIfaceIF4176; "
        "type AliasToIfaceIF4176 := IAliasedIfaceIF4176; "
        "class ProgramAliasIF4176 "
        "  public static method Main(): int "
        "    using var w: AliasToIfaceIF4176 := new(); "
        "    return w.N(); "
        "  end Main; "
        "end ProgramAliasIF4176;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "cannot instantiate interface"));
}

// =====================================================================
// REJECT: abstract class, via the §17.7 shorthand — the SAME SIGSEGV,
// through the SAME unreachable-Slice-S11 gap; in scope for this fix (see
// file header's scope decision).
// =====================================================================

BEZEL_TEST(sema_new_shorthand_interface_abstract_construction_rejected_4176,
           new_abstract_class_local_decl_shorthand_no_longer_segfaults_rejected_spec_12_5) {
    auto r = runAot(
        "module M; "
        "public abstract class AbstractWidgetABS4176 "
        "  public abstract method N(): int; "
        "end AbstractWidgetABS4176; "
        "class ProgramAbsIF4176 "
        "  public static method Main(): int "
        "    using var w: AbstractWidgetABS4176 := new(); "
        "    return w.N(); "
        "  end Main; "
        "end ProgramAbsIF4176;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "cannot instantiate abstract class"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "§12.5"));
}

BEZEL_TEST(sema_new_shorthand_interface_abstract_construction_rejected_4176,
           new_abstract_class_field_initializer_shorthand_rejected_spec_12_5) {
    auto r = runAot(
        "module M; "
        "public abstract class AbstractFieldABS4176 "
        "  public abstract method N(): int; "
        "end AbstractFieldABS4176; "
        "class HolderFieldABS4176 "
        "  public owned W: AbstractFieldABS4176 := new(); "
        "end HolderFieldABS4176; "
        "class ProgramFieldABS4176 "
        "  public static method Main(): int "
        "    using var h: HolderFieldABS4176 := new HolderFieldABS4176(); "
        "    return 0; "
        "  end Main; "
        "end ProgramFieldABS4176;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "cannot instantiate abstract class"));
}

// =====================================================================
// REJECT: `closed` and `static` classes, via the §17.7 shorthand — the
// SAME unreachable-Slice-S11 gap, but a MILDER pre-fix symptom: neither
// crashes (both are otherwise fully concrete), so the mis-construction was
// silently ACCEPTED rather than fatal. Folded into the same shared
// predicate as a byproduct of reusing Slice S11 wholesale (file header).
// =====================================================================

BEZEL_TEST(sema_new_shorthand_interface_abstract_construction_rejected_4176,
           new_closed_class_local_decl_shorthand_no_longer_silently_accepted_rejected_spec_11_7_12_5) {
    auto r = runAot(
        "module M; "
        "public closed class ClosedWidgetCLO4176 "
        "  public method N(): int "
        "    return 5; "
        "  end N; "
        "end ClosedWidgetCLO4176; "
        "class ProgramCloIF4176 "
        "  public static method Main(): int "
        "    using var w: ClosedWidgetCLO4176 := new(); "
        "    return w.N(); "
        "  end Main; "
        "end ProgramCloIF4176;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "cannot instantiate `closed` class"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "§12.5"));
}

BEZEL_TEST(sema_new_shorthand_interface_abstract_construction_rejected_4176,
           new_static_class_local_decl_shorthand_no_longer_silently_accepted_rejected_spec_11_7) {
    auto r = runAot(
        "module M; "
        "public static class StaticWidgetSTA4176 "
        "  public static method N(): int "
        "    return 3; "
        "  end N; "
        "end StaticWidgetSTA4176; "
        "class ProgramStaIF4176 "
        "  public static method Main(): int "
        "    using var w: StaticWidgetSTA4176 := new(); "
        "    return StaticWidgetSTA4176.N(); "
        "  end Main; "
        "end ProgramStaIF4176;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "cannot instantiate static class"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "§11.7"));
}

// =====================================================================
// CONTROL: the EXPLICIT `new T(...)` spelling for all four kinds was
// ALREADY rejected before this fix (Slice S11 always lived in the explicit
// arm) — this fix's shared-predicate refactor must not change any of their
// messages or behavior. Deliberate controls; pass identically on both
// sides (LANE_RULES §20).
// =====================================================================

BEZEL_TEST(sema_new_shorthand_interface_abstract_construction_rejected_4176,
           control_explicit_new_interface_still_rejected_unchanged_spec_11_2) {
    auto r = runAot(
        "module M; "
        "public interface IWidgetCTLIF4176 "
        "  method N(): int; "
        "end IWidgetCTLIF4176; "
        "class ProgramCtlIF4176 "
        "  public static method Main(): int "
        "    using var w: IWidgetCTLIF4176 := new IWidgetCTLIF4176(); "
        "    return w.N(); "
        "  end Main; "
        "end ProgramCtlIF4176;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "cannot instantiate interface"));
}

BEZEL_TEST(sema_new_shorthand_interface_abstract_construction_rejected_4176,
           control_explicit_new_abstract_class_still_rejected_unchanged_spec_12_5) {
    auto r = runAot(
        "module M; "
        "public abstract class AbstractCtlABS4176 "
        "  public abstract method N(): int; "
        "end AbstractCtlABS4176; "
        "class ProgramCtlABS4176 "
        "  public static method Main(): int "
        "    using var w: AbstractCtlABS4176 := new AbstractCtlABS4176(); "
        "    return w.N(); "
        "  end Main; "
        "end ProgramCtlABS4176;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "cannot instantiate abstract class"));
}

BEZEL_TEST(sema_new_shorthand_interface_abstract_construction_rejected_4176,
           control_explicit_new_closed_class_still_rejected_unchanged_spec_12_5) {
    auto r = runAot(
        "module M; "
        "public closed class ClosedCtlCLO4176 "
        "  public method N(): int "
        "    return 5; "
        "  end N; "
        "end ClosedCtlCLO4176; "
        "class ProgramCtlCLO4176 "
        "  public static method Main(): int "
        "    using var w: ClosedCtlCLO4176 := new ClosedCtlCLO4176(); "
        "    return w.N(); "
        "  end Main; "
        "end ProgramCtlCLO4176;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "cannot instantiate `closed` class"));
}

BEZEL_TEST(sema_new_shorthand_interface_abstract_construction_rejected_4176,
           control_explicit_new_static_class_still_rejected_unchanged_spec_11_7) {
    auto r = runAot(
        "module M; "
        "public static class StaticCtlSTA4176 "
        "  public static method N(): int "
        "    return 3; "
        "  end N; "
        "end StaticCtlSTA4176; "
        "class ProgramCtlSTA4176 "
        "  public static method Main(): int "
        "    var w := new StaticCtlSTA4176(); "
        "    return 0; "
        "  end Main; "
        "end ProgramCtlSTA4176;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "cannot instantiate static class"));
}

// =====================================================================
// ACCEPT-SIDE SWEEP (LANE_RULES §15): the legitimate shorthand `new()`
// uses this fix must NOT touch. Basic class/record/generic shapes (already
// swept for the sibling Enum/Choice predicate by #4170's own test file)
// plus the shapes SPECIFIC to this predicate: a class implementing the
// interface this fix now rejects (rejecting `new IFoo()` must not disturb
// `new FooImpl()`), a type alias to a concrete class, a nullable class
// target, and a `where T: new()` generic factory (whose `T` resolves to a
// Named TypeRef with a NULL declaration — the predicate's very first
// check, `rt->declaration` non-null, already excludes it, but measured
// anyway per the brief).
// =====================================================================

BEZEL_TEST(sema_new_shorthand_interface_abstract_construction_rejected_4176,
           accept_class_local_decl_shorthand_compiles_and_runs_spec_17_7) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "class WidgetOKACC4176 "
        "  public _v: int := 7; "
        "  public method N(): int "
        "    return _v; "
        "  end N; "
        "end WidgetOKACC4176; "
        "class ProgramAccClsLocal4176 "
        "  public static method Main(): int "
        "    using var w: WidgetOKACC4176 := new(); "
        "    return w.N(); "
        "  end Main; "
        "end ProgramAccClsLocal4176;"),
        7);
}

BEZEL_TEST(sema_new_shorthand_interface_abstract_construction_rejected_4176,
           accept_record_local_decl_shorthand_compiles_and_runs_spec_17_7) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "public record PointOKACC4176(X: int, Y: int); "
        "class ProgramAccRecLocal4176 "
        "  public static method Main(): int "
        "    var p: PointOKACC4176 := new(3, 4); "
        "    return p.X + p.Y; "
        "  end Main; "
        "end ProgramAccRecLocal4176;"),
        7);
}

BEZEL_TEST(sema_new_shorthand_interface_abstract_construction_rejected_4176,
           accept_generic_class_local_decl_shorthand_compiles_and_runs_spec_17_7) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "class BoxOKACC4176<T> "
        "  public property V: T?; "
        "end BoxOKACC4176; "
        "class ProgramAccGenLocal4176 "
        "  public static method Main(): int "
        "    var b: BoxOKACC4176<int> := new(); "
        "    return (if b.V <> null then 1 else 0); "
        "  end Main; "
        "end ProgramAccGenLocal4176;"),
        0);
}

// The interface's own IMPLEMENTOR must construct cleanly through the
// shorthand — rejecting `new IWidgetImplIF4176()` must not disturb
// `new WidgetImplIF4176()`.
BEZEL_TEST(sema_new_shorthand_interface_abstract_construction_rejected_4176,
           accept_class_implementing_the_rejected_interface_shorthand_compiles_and_runs_spec_17_7) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "public interface IWidgetImplIF4176 "
        "  method N(): int; "
        "end IWidgetImplIF4176; "
        "class WidgetImplIF4176: IWidgetImplIF4176 "
        "  public method N(): int "
        "    return 11; "
        "  end N; "
        "end WidgetImplIF4176; "
        "class ProgramAccImpl4176 "
        "  public static method Main(): int "
        "    using var w: WidgetImplIF4176 := new(); "
        "    return w.N(); "
        "  end Main; "
        "end ProgramAccImpl4176;"),
        11);
}

BEZEL_TEST(sema_new_shorthand_interface_abstract_construction_rejected_4176,
           accept_class_via_type_alias_local_decl_shorthand_compiles_and_runs_spec_17_7) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "class AliasedWidgetOKACC4176 "
        "  public method N(): int "
        "    return 23; "
        "  end N; "
        "end AliasedWidgetOKACC4176; "
        "type AliasToWidgetOKACC4176 := AliasedWidgetOKACC4176; "
        "class ProgramAccAlias4176 "
        "  public static method Main(): int "
        "    using var w: AliasToWidgetOKACC4176 := new(); "
        "    return w.N(); "
        "  end Main; "
        "end ProgramAccAlias4176;"),
        23);
}

BEZEL_TEST(sema_new_shorthand_interface_abstract_construction_rejected_4176,
           accept_nullable_class_target_local_decl_shorthand_compiles_and_runs_spec_17_7) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "class NullableTargetOKACC4176 "
        "  public method N(): int "
        "    return 13; "
        "  end N; "
        "end NullableTargetOKACC4176; "
        "class ProgramAccNullable4176 "
        "  public static method Main(): int "
        "    using var w: NullableTargetOKACC4176? := new(); "
        "    if w is null then "
        "      return 0; "
        "    end if; "
        "    return w.N(); "
        "  end Main; "
        "end ProgramAccNullable4176;"),
        13);
}

BEZEL_TEST(sema_new_shorthand_interface_abstract_construction_rejected_4176,
           accept_where_new_generic_type_param_unaffected_spec_14_1_14_2) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "class FactoryTargetOKACC4176 "
        "  public method N(): int "
        "    return 17; "
        "  end N; "
        "end FactoryTargetOKACC4176; "
        "class GenericFactoryOKACC4176<T> where T: new() "
        "  public static method Make(): owned T "
        "    var x: T := new(); "
        "    return x; "
        "  end Make; "
        "end GenericFactoryOKACC4176; "
        "class ProgramAccWhereNew4176 "
        "  public static method Main(): int "
        "    using var f: FactoryTargetOKACC4176 := "
        "      GenericFactoryOKACC4176<FactoryTargetOKACC4176>.Make(); "
        "    return f.N(); "
        "  end Main; "
        "end ProgramAccWhereNew4176;"),
        17);
}
