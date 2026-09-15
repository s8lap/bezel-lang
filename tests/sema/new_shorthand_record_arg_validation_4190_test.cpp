// Spec-derived tests for fix #4190 — §17.7 "The `new` operator" (the
// target-typed `new()` shorthand: "A field initializer or typed local
// declaration may use the `new()` shorthand when the type is unambiguous");
// §9.1 "Positional records" ("A primary constructor whose parameters are
// the positional fields, in declaration order"), verbatim, v0.298.
//
// Filed by a reviewer of #4176: the two §17.7 shorthand call sites in
// ExprTypecheck.cpp (field-initializer, local-decl) guarded their
// `checkNewExprArgs` call with `fty->declaration->kind ==
// ast::TypeDeclKind::Class` and NOTHING ELSE — no `Record` arm at all. The
// explicit `new T(...)` NewExpr-typing arm in ExprTypecheck_Exprs.cpp has
// run `checkNewExprArgs` for BOTH Class and Record since #1105 (§9.1) and
// #4059 (§9.1/§9.2/§22.7) closed the body-record zero-fill hole — but
// neither fix's reach ever extended to the shorthand's own Class-only
// guard, so a record constructed through `new(args)` skipped BOTH arity
// and per-argument type checking entirely. #4176 (filed and fixed
// immediately before this issue was found) closed the sibling gap in the
// SAME two guards for the abstract/closed/static/interface classification
// but left this one exactly as it was — it is a different, unrelated
// missing arm at the same two call sites.
//
// MEASURED, pre-fix (this issue's own repro, base commit fbfd7a1 +
// #4176's `293fa34`; unaffected by #4176's fix — both give the same
// result):
//   `record PointBADARGS4176B(X: int, Y: int); var p: PointBADARGS4176B :=
//   new(3);` — compile exit 0, no diagnostic; run exit 100 (`p.X <> 3`) —
//   the single positional argument `3` was not even bound to the first
//   field `X`.
//   `record PointBADTYPE4176(X: int, Y: int); var p: PointBADTYPE4176 :=
//   new("hello", "world");` — compile exit 0, no diagnostic; run exit 0
//   (`p.X + p.Y == 0`) — wrong-typed args silently accepted, both fields
//   left at their zero default.
// The explicit spelling (`new PointBADARGS4176B(3)`) already correctly
// rejects both shapes — `checkNewExprArgs` has always had the right Record
// arm; it just was not reachable from the shorthand.
//
// FIX: both shorthand guards in ExprTypecheck.cpp now admit
// `ast::TypeDeclKind::Record` alongside `Class`, so `checkNewExprArgs`
// runs for a record target exactly as it already did for the explicit
// spelling — same function, same diagnostics, same §9.1/§22.5/§5.1
// citations. No new predicate: this is purely widening an existing
// guard's condition to reach an authority (`checkNewExprArgs`) that
// already answers the Record case correctly.
//
// Rows below cover: wrong arity (local-decl, field-initializer), wrong
// argument type (local-decl), a body record with an explicit constructor
// (wrong arity), a body-only record with no constructor at all (the
// #4059 zero-fill reject, now reachable from the shorthand too), and
// accept-side controls (correct positional args, reordered named args,
// a class control) proving the fix does not touch legitimate shorthand
// construction (LANE_RULES §15).

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
// REJECT: wrong arity, local-decl shorthand — the issue's own repro.
// =====================================================================

BEZEL_TEST(sema_new_shorthand_record_arg_validation_4190,
           reject_local_decl_shorthand_too_few_args_spec_17_7_9_1) {
    auto r = runAot(
        "module M; "
        "public record PointBADARGS4190(X: int, Y: int); "
        "class ProgramBadArgs4190 "
        "  public static method Main(): int "
        "    var p: PointBADARGS4190 := new(3); "
        "    return p.X + p.Y; "
        "  end Main; "
        "end ProgramBadArgs4190;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "not supplied"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "§22.5"));
}

BEZEL_TEST(sema_new_shorthand_record_arg_validation_4190,
           reject_local_decl_shorthand_too_many_args_spec_17_7_9_1) {
    auto r = runAot(
        "module M; "
        "public record PointTOOMANY4190(X: int, Y: int); "
        "class ProgramTooMany4190 "
        "  public static method Main(): int "
        "    var p: PointTOOMANY4190 := new(3, 4, 5); "
        "    return p.X + p.Y; "
        "  end Main; "
        "end ProgramTooMany4190;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// =====================================================================
// REJECT: wrong argument type, local-decl shorthand.
// =====================================================================

BEZEL_TEST(sema_new_shorthand_record_arg_validation_4190,
           reject_local_decl_shorthand_wrong_arg_type_spec_17_7_5_1) {
    auto r = runAot(
        "module M; "
        "public record PointBADTYPE4190(X: int, Y: int); "
        "class ProgramBadType4190 "
        "  public static method Main(): int "
        "    var p: PointBADTYPE4190 := new(\"hello\", \"world\"); "
        "    return p.X + p.Y; "
        "  end Main; "
        "end ProgramBadType4190;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "not implicitly convertible to the target type"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "§5.1"));
}

// =====================================================================
// REJECT: wrong arity, field-initializer shorthand.
// =====================================================================

BEZEL_TEST(sema_new_shorthand_record_arg_validation_4190,
           reject_field_initializer_shorthand_too_few_args_spec_17_7_9_1) {
    auto r = runAot(
        "module M; "
        "public record PointFieldBad4190(X: int, Y: int); "
        "class HolderFieldBad4190 "
        "  private _p: PointFieldBad4190 := new(3); "
        "  public method GetSum(): int => _p.X + _p.Y; "
        "end HolderFieldBad4190; "
        "class ProgramFieldBad4190 "
        "  public static method Main(): int "
        "    var h: HolderFieldBad4190 := new HolderFieldBad4190(); "
        "    return h.GetSum(); "
        "  end Main; "
        "end ProgramFieldBad4190;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "not supplied"));
}

// =====================================================================
// REJECT: a body record with an explicit constructor — wrong arity
// through the shorthand (§9.3 combined form is out of scope here; this is
// a pure body-record explicit-constructor case).
// =====================================================================

BEZEL_TEST(sema_new_shorthand_record_arg_validation_4190,
           reject_body_record_explicit_ctor_wrong_arity_spec_17_7_9_1) {
    auto r = runAot(
        "module M; "
        "public record BodyRecBad4190 "
        "  public readonly property Width: int; "
        "  public readonly property Height: int; "
        "  public constructor(w: int, h: int) "
        "    Width := w; "
        "    Height := h; "
        "  end constructor; "
        "end BodyRecBad4190; "
        "class ProgramBodyBad4190 "
        "  public static method Main(): int "
        "    var r: BodyRecBad4190 := new(3); "
        "    return r.Width + r.Height; "
        "  end Main; "
        "end ProgramBodyBad4190;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "not supplied"));
}

// =====================================================================
// REJECT: a body-only record with NO declared constructor — the #4059
// zero-fill hole, now reachable from the shorthand too.
// =====================================================================

BEZEL_TEST(sema_new_shorthand_record_arg_validation_4190,
           reject_body_only_record_no_ctor_shorthand_spec_17_7_9_1_9_2) {
    auto r = runAot(
        "module M; "
        "public record NoCtorRec4190 "
        "  public readonly property Foo: int; "
        "end NoCtorRec4190; "
        "class ProgramNoCtor4190 "
        "  public static method Main(): int "
        "    var r: NoCtorRec4190 := new(); "
        "    return r.Foo; "
        "  end Main; "
        "end ProgramNoCtor4190;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(
        diagnosticMessageContains(r, "requires a declared constructor"));
}

// =====================================================================
// ACCEPT-SIDE SWEEP (LANE_RULES §15): the legitimate shorthand `new()`
// uses this fix must NOT touch.
// =====================================================================

BEZEL_TEST(sema_new_shorthand_record_arg_validation_4190,
           accept_local_decl_shorthand_correct_args_spec_17_7) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "public record PointOK4190(X: int, Y: int); "
        "class ProgramOK4190 "
        "  public static method Main(): int "
        "    var p: PointOK4190 := new(3, 4); "
        "    return p.X * 10 + p.Y; "
        "  end Main; "
        "end ProgramOK4190;"),
        34);
}

BEZEL_TEST(sema_new_shorthand_record_arg_validation_4190,
           accept_local_decl_shorthand_reordered_named_args_spec_17_7_22_5) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "public record PointNamed4190(X: int, Y: int); "
        "class ProgramNamed4190 "
        "  public static method Main(): int "
        "    var p: PointNamed4190 := new(Y := 4, X := 3); "
        "    return p.X * 10 + p.Y; "
        "  end Main; "
        "end ProgramNamed4190;"),
        34);
}

BEZEL_TEST(sema_new_shorthand_record_arg_validation_4190,
           accept_field_initializer_shorthand_correct_args_spec_17_7) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "public record PointFieldOK4190(X: int, Y: int); "
        "class HolderFieldOK4190 "
        "  private _p: PointFieldOK4190 := new(3, 4); "
        "  public method GetSum(): int => _p.X * 10 + _p.Y; "
        "end HolderFieldOK4190; "
        "class ProgramFieldOK4190 "
        "  public static method Main(): int "
        "    var h: HolderFieldOK4190 := new HolderFieldOK4190(); "
        "    return h.GetSum(); "
        "  end Main; "
        "end ProgramFieldOK4190;"),
        34);
}

BEZEL_TEST(sema_new_shorthand_record_arg_validation_4190,
           accept_body_record_explicit_ctor_correct_args_spec_17_7_9_1) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "public record BodyRecOK4190 "
        "  public readonly property Width: int; "
        "  public readonly property Height: int; "
        "  public constructor(w: int, h: int) "
        "    Width := w; "
        "    Height := h; "
        "  end constructor; "
        "end BodyRecOK4190; "
        "class ProgramBodyOK4190 "
        "  public static method Main(): int "
        "    var r: BodyRecOK4190 := new(3, 4); "
        "    return r.Width * 10 + r.Height; "
        "  end Main; "
        "end ProgramBodyOK4190;"),
        34);
}

// Control: a CLASS through the same shorthand path must still be checked
// exactly as before (this fix widens the Record admission; it must not
// disturb the pre-existing Class arm at either call site).
BEZEL_TEST(sema_new_shorthand_record_arg_validation_4190,
           control_class_local_decl_shorthand_still_rejects_wrong_arity_spec_17_7) {
    auto r = runAot(
        "module M; "
        "class BoxClassCtl4190 "
        "  public property V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "end BoxClassCtl4190; "
        "class ProgramClassCtl4190 "
        "  public static method Main(): int "
        "    var b: BoxClassCtl4190 := new(); "
        "    return b.V; "
        "  end Main; "
        "end ProgramClassCtl4190;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "not supplied"));
}

BEZEL_TEST(sema_new_shorthand_record_arg_validation_4190,
           control_class_local_decl_shorthand_accepts_correct_args_spec_17_7) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "class BoxClassOkCtl4190 "
        "  public property V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "end BoxClassOkCtl4190; "
        "class ProgramClassOkCtl4190 "
        "  public static method Main(): int "
        "    var b: BoxClassOkCtl4190 := new(5); "
        "    return b.V; "
        "  end Main; "
        "end ProgramClassOkCtl4190;"),
        5);
}
