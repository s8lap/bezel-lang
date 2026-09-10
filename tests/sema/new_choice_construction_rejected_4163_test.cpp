// Spec-derived tests for fix #4163 — §35.1 "Declaration", §35.2 "Members",
// §35.3 "Construction and payload ownership" (v0.295).
//
// `new ChoiceType(args)` was silently ACCEPTED with no diagnostic, even
// though the spec is explicit that a choice cannot be constructed with
// `new`:
//
//   §35.1: "Choices cannot have destructors, constructors, instance fields,
//   block-form or storage-backed properties, indexers, or nested types —
//   the same restriction set as enums (§10.5), for the same reasons, plus
//   one: a choice's instance storage *is* its cases' payloads, declared in
//   the case list and reachable only through matching."
//
//   §35.2: "A choice has no user constructors, instance fields, or
//   storage-backed properties by design: a choice's construction *is* case
//   selection — a user constructor would have to pick a case, which the
//   qualified case form already spells ..."
//
//   §35.3: "Construction is qualified, resolving exactly as enum constants
//   do (§10.1): `FetchOutcome.NotModified` for a payload-free case (a value
//   expression, not a call), `FetchOutcome.Failed(504, "upstream timeout")`
//   for a payload case."
//
// MEASURED consequence of the pre-fix gap (issue #4163's own repro,
// verbatim): `new ChoiceNCL(new WidgetNCL())` compiled at exit 0 with zero
// diagnostic, and a `case` statement run over a `new ChoiceType(7)` value
// SIGSEGV'd the compiler at compile time (codegen's `case`-statement
// lowering assumes every choice-typed scrutinee it sees was produced by a
// real case construction — a `new`-expression construction has no case
// identity at all, and carries no discriminant).
//
// FIX: `ExprTypecheck_Exprs.cpp`'s `NewExpr` typechecking (the same block
// that already rejects `new` on an enum, §5.4/§10/#960) gets a sibling
// Choice-kind arm: any `new` on a Choice-declared type is rejected outright,
// naming the qualified `Type.Case(args)` remedy. This mirrors the existing
// enum precedent exactly and closes the SIGSEGV at the source — no such
// value can ever reach a `case` statement's codegen any more.
//
// Because the pre-fix #4163 symptom for the `case`-statement row is a
// compiler SIGSEGV rather than a clean diagnostic-free compile, that row's
// assertion uses `failedAtCompileTimeWithRealLocation` (aot_result_asserts.hpp)
// PAIRED WITH message-text (LANE_RULES §18) — a bare `!r.ran` or the location
// helper alone cannot distinguish "compiler crashed" from "rejected for the
// right reason": `runAot`'s crash path reports "runAot: compiler CRASHED
// (signal N) ..." at SourceLocation{}, which `failedAtCompileTimeWithRealLocation`
// itself already excludes, so this row is expected to read FALSE pre-fix and
// TRUE post-fix with the §35.1/§35.2/§35.3 message text present.
//
// NOTE: §35.3 also admits a bare case name in a "declared-result position"
// (e.g. `return Ok(v);` where the declared return type is `Result<int,
// string>`) as an alternative to the qualified form. That spelling is a
// SEPARATE, already-tracked implementation gap in this compiler (issue
// #2354, "declared-result target-typing of choice-case constructions") —
// verified by execution: even the spec's own §35.3 `ParsePercent` example
// does not compile on this tree (`unresolved identifier` for the bare `Ok`/
// `Error` case names). This fix's diagnostic therefore names only the
// qualified spelling as the remedy, and this file's accept-side controls
// use the qualified form exclusively.

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
// REJECT: the issue's own repros.
// =====================================================================

// Issue #4163's first repro, verbatim shape: `new ChoiceType(args)` with a
// class-typed payload argument, used as a plain local initializer. Pre-fix
// this compiled clean at exit 0 with zero diagnostic.
BEZEL_TEST(sema_new_choice_construction_rejected_4163,
           new_choice_with_class_payload_rejected_spec_35_1) {
    auto r = runAot(
        "module M; "
        "class WidgetNCL "
        "  public method Render(): int return 42; end Render; "
        "end WidgetNCL; "
        "public choice ChoiceNCL Has(H: WidgetNCL), Empty(); end ChoiceNCL; "
        "class ProgramNCL "
        "  public static method Main(): int "
        "    var x := new ChoiceNCL(new WidgetNCL()); "
        "    return 0; "
        "  end Main; "
        "end ProgramNCL;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "`new` cannot construct a choice"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "§35.1/§35.2/§35.3"));
}

// Issue #4163's second repro, verbatim shape: a `case` statement run over a
// `new ChoiceType(7)` value. Pre-fix this SIGSEGV'd the COMPILER (measured:
// "Segmentation fault (core dumped)", exit 139, direct `bezelc` invocation)
// — a live crash, not a clean rejection, hence the paired
// failedAtCompileTimeWithRealLocation + message-text assertion rather than a
// bare `!r.ran` (LANE_RULES §18 / issue #3892).
BEZEL_TEST(sema_new_choice_construction_rejected_4163,
           new_choice_then_case_statement_no_longer_crashes_rejected_spec_35_1) {
    auto r = runAot(
        "module M; "
        "public choice ChoiceNCX Has(H: int), Empty(); end ChoiceNCX; "
        "class ProgramNCX "
        "  public static method Main(): int "
        "    var x := new ChoiceNCX(7); "
        "    case x of "
        "      Has(var h) => Console.WriteLine(h.ToString()), "
        "      Empty()    => Console.WriteLine(\"Empty\") "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end ProgramNCX;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "`new` cannot construct a choice"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "§35.1/§35.2/§35.3"));
}

// =====================================================================
// REJECT: the accept-side sweep for the shape of the rejection itself —
// payload-free case, wrong-arity argument list (previously accepted
// SILENTLY by guessing across every declared case's arity), and a generic
// (stdlib) choice.
// =====================================================================

BEZEL_TEST(sema_new_choice_construction_rejected_4163,
           new_choice_payload_free_case_rejected_spec_35_1) {
    auto r = runAot(
        "module M; "
        "public choice ChoiceNPF Has(H: int), Empty(); end ChoiceNPF; "
        "class ProgramNPF "
        "  public static method Main(): int "
        "    var x := new ChoiceNPF(); "
        "    return 0; "
        "  end Main; "
        "end ProgramNPF;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "`new` cannot construct a choice"));
}

// Before the fix, `checkNewExprArgs` had no Choice-kind arm at all, so a
// `new`-expression whose argument count matched NO declared case still
// compiled clean (nothing there ever inspected arity for a choice target —
// only `checkStaticReadonlyRecordInit`'s narrow, disposal-only Choice arm
// ever walked case arities, and only for a `static readonly` field
// initializer). This pins that the new rejection fires independent of
// argument shape.
BEZEL_TEST(sema_new_choice_construction_rejected_4163,
           new_choice_wrong_arity_rejected_spec_35_1) {
    auto r = runAot(
        "module M; "
        "public choice ChoiceWA Has(H: int), Empty(); end ChoiceWA; "
        "class ProgramWA "
        "  public static method Main(): int "
        "    var x := new ChoiceWA(1, 2, 3); "
        "    return 0; "
        "  end Main; "
        "end ProgramWA;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "`new` cannot construct a choice"));
}

// A GENERIC choice — the stdlib's own `Result<T, E>` (stdlib §2.20) — gets
// the identical rejection; the Choice-kind check does not special-case
// arity/generics.
BEZEL_TEST(sema_new_choice_construction_rejected_4163,
           new_generic_stdlib_choice_rejected_spec_35_1) {
    auto r = runAot(
        "module M; "
        "class ProgramGN "
        "  public static method Main(): int "
        "    var x := new Result<int, string>(3); "
        "    return 0; "
        "  end Main; "
        "end ProgramGN;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "`new` cannot construct a choice"));
}

// =====================================================================
// ACCEPT-SIDE SWEEP (LANE_RULES §15): every legitimate construction form
// this fix must NOT touch keeps compiling and running.
// =====================================================================

// The real spelling: a qualified case CALL with a payload, matched by a
// `case` statement — the exact legitimate analogue of the crashing repro
// above (only the construction spelling differs).
BEZEL_TEST(sema_new_choice_construction_rejected_4163,
           accept_qualified_case_call_with_payload_compiles_and_runs_spec_35_3) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "class WidgetL "
        "  public method Render(): int return 42; end Render; "
        "end WidgetL; "
        "public choice ChoiceL Has(H: WidgetL), Empty(); end ChoiceL; "
        "class ProgramL "
        "  public static method Main(): int "
        "    var w := new WidgetL(); "
        "    var x := ChoiceL.Has(w); "
        "    return (case x of Has(var h) => h.Render(), Empty() => 0); "
        "  end Main; "
        "end ProgramL;"),
        42);
}

// The payload-free case value form (§35.3: "a value expression, not a
// call").
BEZEL_TEST(sema_new_choice_construction_rejected_4163,
           accept_qualified_payload_free_case_compiles_and_runs_spec_35_3) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "public choice ChoiceP Has(H: int), Empty(); end ChoiceP; "
        "class ProgramP "
        "  public static method Main(): int "
        "    var x := ChoiceP.Empty(); "
        "    return (case x of Has(var h) => h, Empty() => 0); "
        "  end Main; "
        "end ProgramP;"),
        0);
}

// A generic (stdlib) choice constructed the real way must be entirely
// unaffected.
BEZEL_TEST(sema_new_choice_construction_rejected_4163,
           accept_qualified_generic_choice_compiles_and_runs_spec_35_3) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "class ProgramG "
        "  public static method Main(): int "
        "    var r := Result<int, string>.Ok(7); "
        "    return (case r of Ok(var v) => v, Error(var e) => 0); "
        "  end Main; "
        "end ProgramG;"),
        7);
}

// Control: `new` on a RECORD (positional fields, the sibling construction
// form this fix must not disturb) is unaffected.
BEZEL_TEST(sema_new_choice_construction_rejected_4163,
           control_new_record_still_compiles_and_runs) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "public record PointR(X: int, Y: int); "
        "class ProgramR "
        "  public static method Main(): int "
        "    var p := new PointR(1, 2); "
        "    return p.X + p.Y; "
        "  end Main; "
        "end ProgramR;"),
        3);
}

// Control: `new` on a CLASS with an explicit constructor is unaffected.
BEZEL_TEST(sema_new_choice_construction_rejected_4163,
           control_new_class_still_compiles_and_runs) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "class BoxC "
        "  public property V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "end BoxC; "
        "class ProgramC "
        "  public static method Main(): int "
        "    var b := new BoxC(5); "
        "    return b.V; "
        "  end Main; "
        "end ProgramC;"),
        5);
}

// Control: `new` on an ENUM is a PRE-EXISTING, separately-fixed rejection
// (§5.4/§10, #960) — this fix must not change its message or behavior.
BEZEL_TEST(sema_new_choice_construction_rejected_4163,
           control_new_enum_still_rejected_pre_existing_spec_5_4_10) {
    auto r = runAot(
        "module M; "
        "public enum ColorE4163: int Red, Green; end ColorE4163; "
        "class ProgramE4163 "
        "  public static method Main(): int "
        "    var x := new ColorE4163(1); "
        "    return 0; "
        "  end Main; "
        "end ProgramE4163;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "`new` cannot construct an enum"));
}

// Control: `new` on an INTERFACE is a PRE-EXISTING, separately-fixed
// rejection (§11.2) — this fix must not change its message or behavior.
BEZEL_TEST(sema_new_choice_construction_rejected_4163,
           control_new_interface_still_rejected_pre_existing_spec_11_2) {
    auto r = runAot(
        "module M; "
        "public interface IWidget4163 "
        "  method Render(): int; "
        "end IWidget4163; "
        "class ProgramI4163 "
        "  public static method Main(): int "
        "    var x := new IWidget4163(); "
        "    return 0; "
        "  end Main; "
        "end ProgramI4163;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "cannot instantiate interface"));
}
