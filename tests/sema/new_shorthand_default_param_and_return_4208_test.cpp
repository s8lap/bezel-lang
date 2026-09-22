// Spec-derived tests for fix #4208 — §17.7 "The `new` operator", the
// "written-declared-type positions" family (v0.296), verbatim:
//   "The shorthand is permitted at the written-declared-type positions (i),
//   (ii), (iii) and the `return e;` statement of (v) above: field
//   initializers, typed local-variable declarations, default parameter
//   values, and return statements (when the method has a declared return
//   type)."
//
// Positions (i) field initializer and (ii) typed local declaration have been
// wired since #987/#979 (and #4190 extended their `checkNewExprArgs` guard to
// records). Positions (iii) default parameter value and the `return e;`
// statement of (v) were NEVER wired: the two shorthand call sites in
// ExprTypecheck.cpp (field-initializer ~line 12608, local-decl ~line 13781)
// never ran for a default-value expression or a ReturnStmt operand, so the
// shorthand NewExpr there was left untyped. Codegen's `lowerReturnStmt` /
// default-value materialization then hit the #2491 backstop — "return-value
// expression failed to lower to a value (degraded to a zero/null return
// value)" — and silently produced a zero-initialized value where §17.7 says
// a constructed one belongs. No diagnostic; the program compiled and ran.
// This was found by the R18 review of #4190 and filed as #4208.
//
// DECISION (recorded per CLAUDE.md rule 4 / LANE_RULES §9): §17.7 is
// unambiguous that positions (iii) and the `return e;` statement of (v) ARE
// authorised shorthand positions — this is option (a) from the issue, not a
// spec amendment (option (b), out of bounds for this run). FIX: both new
// positions are wired through the SAME authorities the field-initializer and
// local-decl sites already use — `rejectNewOnEnumOrChoice`,
// `rejectNewOnNonInstantiableKind`, `checkNewExprArgs` (with the #4190
// Class/Record guard), and `checkBuiltinClosedConstructorSurface` — in the
// same order, so the fix inherits their validation rather than adding a
// third/fourth spelling of the same checks (LANE_RULES §6/§25).
//
// EXCLUDED FROM THE GRANT, THEN REJECTED (#4249): §17.7 explicitly carves a
// LAMBDA's ascribed return out of "return statements" ("It is not permitted at
// ... a lambda's ascribed return"), even though a lambda body's own `return`
// reuses the identical `returnCtx_ == ValueMethod` machinery (§27.3/#2101)
// the fix's return-position gate reads. #4208 gated on `!inLambda_` (the
// existing §29.9/#747 lambda-body marker) to hold that carve-out. But
// declining to TYPE the shorthand is not the same as DIAGNOSING it: the
// untyped NewExpr fell through to codegen's #2491 zero/null backstop, so the
// position compiled and then SEGV'd on the null it returned. #4249 (v0.306)
// closes that by REJECTING both spellings of the excluded position — see the
// two rows at the end of this file, whose oracle was strengthened from "not
// 77" to the located §17.7 rejection when #4249 landed.
//
// MEASURED pre-fix (base 6f46a73, this issue's own probes, record and class
// targets both): `method M(p: T := new(3, 4)): int => ...` and `method
// M(): T do return new(3, 4); end M;` both compiled with exit 0 / no
// diagnostic and returned a zero-filled/degraded value at the call site
// instead of the constructed one.
//
// Rows below cover both new positions (default-parameter-value, return)
// crossed with: a record target with correct positional constructor
// arguments (asserting the CONSTRUCTED VALUE, not just that it compiled), a
// wrong-arity reject, a wrong-argument-type reject, a class target with an
// explicit constructor, a class target with an implicit zero-argument
// constructor, a generic record target, and a nullable declared type — plus
// the lambda-exclusion control.

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
// DEFAULT PARAMETER VALUE (§17.7 written-declared-type position (iii))
// =====================================================================

BEZEL_TEST(sema_new_shorthand_default_param_and_return_4208,
           accept_default_param_record_ctor_args_constructs_value_spec_17_7) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "public record PointDefParam4208(X: int, Y: int); "
        "class ProgramDefParam4208 "
        "  public static method Sum(p: PointDefParam4208 := new(3, 4)): int "
        "    return p.X * 10 + p.Y; "
        "  end Sum; "
        "  public static method Main(): int "
        "    return Sum(); "
        "  end Main; "
        "end ProgramDefParam4208;"),
        34);
}

BEZEL_TEST(sema_new_shorthand_default_param_and_return_4208,
           reject_default_param_record_wrong_arity_spec_17_7_9_1) {
    auto r = runAot(
        "module M; "
        "public record PointBadArityDefParam4208(X: int, Y: int); "
        "class ProgramBadArityDefParam4208 "
        "  public static method Sum(p: PointBadArityDefParam4208 := new(3)): "
        "int "
        "    return p.X + p.Y; "
        "  end Sum; "
        "  public static method Main(): int "
        "    return Sum(); "
        "  end Main; "
        "end ProgramBadArityDefParam4208;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "not supplied"));
}

BEZEL_TEST(sema_new_shorthand_default_param_and_return_4208,
           reject_default_param_record_wrong_arg_type_spec_17_7_5_1) {
    auto r = runAot(
        "module M; "
        "public record PointBadTypeDefParam4208(X: int, Y: int); "
        "class ProgramBadTypeDefParam4208 "
        "  public static method Sum(p: PointBadTypeDefParam4208 := "
        "new(\"hello\", \"world\")): int "
        "    return p.X + p.Y; "
        "  end Sum; "
        "  public static method Main(): int "
        "    return Sum(); "
        "  end Main; "
        "end ProgramBadTypeDefParam4208;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "not implicitly convertible to the target type"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "§5.1"));
}

BEZEL_TEST(sema_new_shorthand_default_param_and_return_4208,
           accept_default_param_class_ctor_args_constructs_value_spec_17_7) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "class BoxDefParam4208 "
        "  public property V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "end BoxDefParam4208; "
        "class ProgramBoxDefParam4208 "
        "  public static method Get(b: BoxDefParam4208 := new(9)): int "
        "    return b.V; "
        "  end Get; "
        "  public static method Main(): int "
        "    return Get(); "
        "  end Main; "
        "end ProgramBoxDefParam4208;"),
        9);
}

BEZEL_TEST(
    sema_new_shorthand_default_param_and_return_4208,
    accept_default_param_class_zero_arg_ctor_constructs_value_spec_17_7) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "class WidgetDefParam4208 "
        "  public property Tag: int := 77; "
        "end WidgetDefParam4208; "
        "class ProgramWidgetDefParam4208 "
        "  public static method Get(w: WidgetDefParam4208 := new()): int "
        "    return w.Tag; "
        "  end Get; "
        "  public static method Main(): int "
        "    return Get(); "
        "  end Main; "
        "end ProgramWidgetDefParam4208;"),
        77);
}

BEZEL_TEST(
    sema_new_shorthand_default_param_and_return_4208,
    accept_default_param_generic_record_target_constructs_value_spec_17_7) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "public record BoxGenDefParam4208<T>(V: T); "
        "class ProgramBoxGenDefParam4208 "
        "  public static method Get(b: BoxGenDefParam4208<int> := new(7)): "
        "int "
        "    return b.V; "
        "  end Get; "
        "  public static method Main(): int "
        "    return Get(); "
        "  end Main; "
        "end ProgramBoxGenDefParam4208;"),
        7);
}

BEZEL_TEST(
    sema_new_shorthand_default_param_and_return_4208,
    accept_default_param_nullable_declared_type_constructs_value_spec_17_7) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "class WidgetNullDefParam4208 "
        "  public property Tag: int := 55; "
        "end WidgetNullDefParam4208; "
        "class ProgramWidgetNullDefParam4208 "
        "  public static method Get(w: WidgetNullDefParam4208? := new()): "
        "int "
        "    return w!.Tag; "
        "  end Get; "
        "  public static method Main(): int "
        "    return Get(); "
        "  end Main; "
        "end ProgramWidgetNullDefParam4208;"),
        55);
}

// =====================================================================
// RETURN STATEMENT (§17.7 written-declared-type position, the `return e;`
// statement of (v))
// =====================================================================

BEZEL_TEST(sema_new_shorthand_default_param_and_return_4208,
           accept_return_record_ctor_args_constructs_value_spec_17_7) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "public record PointReturn4208(X: int, Y: int); "
        "class ProgramReturn4208 "
        "  public static method Make(): PointReturn4208 "
        "    return new(3, 4); "
        "  end Make; "
        "  public static method Main(): int "
        "    var p: PointReturn4208 := Make(); "
        "    return p.X * 10 + p.Y; "
        "  end Main; "
        "end ProgramReturn4208;"),
        34);
}

BEZEL_TEST(sema_new_shorthand_default_param_and_return_4208,
           reject_return_record_wrong_arity_spec_17_7_9_1) {
    auto r = runAot(
        "module M; "
        "public record PointReturnBadArity4208(X: int, Y: int); "
        "class ProgramReturnBadArity4208 "
        "  public static method Make(): PointReturnBadArity4208 "
        "    return new(3); "
        "  end Make; "
        "  public static method Main(): int "
        "    var p: PointReturnBadArity4208 := Make(); "
        "    return p.X + p.Y; "
        "  end Main; "
        "end ProgramReturnBadArity4208;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "not supplied"));
}

BEZEL_TEST(sema_new_shorthand_default_param_and_return_4208,
           reject_return_record_wrong_arg_type_spec_17_7_5_1) {
    auto r = runAot(
        "module M; "
        "public record PointReturnBadType4208(X: int, Y: int); "
        "class ProgramReturnBadType4208 "
        "  public static method Make(): PointReturnBadType4208 "
        "    return new(\"hello\", \"world\"); "
        "  end Make; "
        "  public static method Main(): int "
        "    var p: PointReturnBadType4208 := Make(); "
        "    return p.X + p.Y; "
        "  end Main; "
        "end ProgramReturnBadType4208;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "not implicitly convertible to the target type"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "§5.1"));
}

BEZEL_TEST(sema_new_shorthand_default_param_and_return_4208,
           accept_return_class_ctor_args_constructs_value_spec_17_7) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "class BoxReturn4208 "
        "  public property V: int; "
        "  public constructor(v: int) V := v; end constructor; "
        "end BoxReturn4208; "
        "class ProgramBoxReturn4208 "
        "  public static method Make(): owned BoxReturn4208 "
        "    return new(9); "
        "  end Make; "
        "  public static method Main(): int "
        "    using var b: BoxReturn4208 := Make(); "
        "    return b.V; "
        "  end Main; "
        "end ProgramBoxReturn4208;"),
        9);
}

BEZEL_TEST(sema_new_shorthand_default_param_and_return_4208,
           accept_return_class_zero_arg_ctor_constructs_value_spec_17_7) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "class WidgetReturn4208 "
        "  public property Tag: int := 77; "
        "end WidgetReturn4208; "
        "class ProgramWidgetReturn4208 "
        "  public static method Make(): owned WidgetReturn4208 "
        "    return new(); "
        "  end Make; "
        "  public static method Main(): int "
        "    using var w: WidgetReturn4208 := Make(); "
        "    return w.Tag; "
        "  end Main; "
        "end ProgramWidgetReturn4208;"),
        77);
}

BEZEL_TEST(sema_new_shorthand_default_param_and_return_4208,
           accept_return_generic_record_target_constructs_value_spec_17_7) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "public record BoxGenReturn4208<T>(V: T); "
        "class ProgramBoxGenReturn4208 "
        "  public static method Make(): BoxGenReturn4208<int> "
        "    return new(7); "
        "  end Make; "
        "  public static method Main(): int "
        "    var b: BoxGenReturn4208<int> := Make(); "
        "    return b.V; "
        "  end Main; "
        "end ProgramBoxGenReturn4208;"),
        7);
}

BEZEL_TEST(sema_new_shorthand_default_param_and_return_4208,
           accept_return_nullable_declared_type_constructs_value_spec_17_7) {
    BEZEL_EXPECT_EQ(runProg(
        "module M; "
        "class WidgetNullReturn4208 "
        "  public property Tag: int := 55; "
        "end WidgetNullReturn4208; "
        "class ProgramWidgetNullReturn4208 "
        "  public static method Make(): owned WidgetNullReturn4208? "
        "    return new(); "
        "  end Make; "
        "  public static method Main(): int "
        "    using var w: WidgetNullReturn4208? := Make(); "
        "    return w!.Tag; "
        "  end Main; "
        "end ProgramWidgetNullReturn4208;"),
        55);
}

// =====================================================================
// §17.7's EXCLUDED declared-result position: a lambda's ascribed return.
// §17.7 (v0.306) grants the shorthand positions (i), (ii), (iii) and the
// `return e;` STATEMENT of (v), then excludes this one verbatim: "It is
// **not** permitted at an expression body or at a lambda's ascribed
// return". Member (v)'s third declared-result position is itself "a
// `return` operand or expression body checked against a lambda's
// return-type ascription" (§27.3), so BOTH spellings pinned below — the
// block body's `return new();` and the expression body `=> new()` — are
// excluded by that ONE clause.
//
// HISTORY / why the oracle changed (#4249). #4208 held the carve-out by
// declining to TYPE the shorthand here (its `!inLambda_` gate), and this
// row originally asserted only `77 != runProg(...)` — i.e. merely that the
// program did not produce the constructed value. That oracle was WEAK:
// declining to type the shorthand is not the same as DIAGNOSING it, so the
// untyped NewExpr fell through to codegen's #2491 zero/null backstop, the
// program COMPILED (an `internal:` Warning at most, and for the
// expression-body spelling no diagnostic at all), and then SEGV'd on the
// first member read of the null it returned — exit 139, measured 60/60 at
// e672e7e1. `runAot` reports a signal-killed program as `ran=false`, so the
// row went GREEN over a memory-safety crash reachable from ordinary Bezel
// source: exactly the failure mode the row existed to catch. #4249 fixes
// that by rejecting both spellings where §17.7 says they are not permitted.
//
// These rows therefore assert the LOCATED §17.7 compile-time rejection the
// spec requires, which strictly dominates the old "not 77" oracle (a
// compile-time rejection gives `ran=false`, so 77 is unreachable). The
// oracle is STRENGTHENED, never weakened. The first row keeps its original
// name so its gate identity is preserved.
// =====================================================================

BEZEL_TEST(sema_new_shorthand_default_param_and_return_4208,
           control_lambda_ascribed_return_shorthand_unaffected_spec_17_7) {
    auto r = runAot(
        "module M; "
        "class WidgetLambdaCtl4208 "
        "  public property Tag: int := 77; "
        "end WidgetLambdaCtl4208; "
        "class ProgramWidgetLambdaCtl4208 "
        "  public static method Main(): int "
        "    var f := lambda(): WidgetLambdaCtl4208 => do return new(); "
        "end; "
        "    var w: WidgetLambdaCtl4208 := f(); "
        "    return w.Tag; "
        "  end Main; "
        "end ProgramWidgetLambdaCtl4208;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "not permitted at a lambda's return"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "§17.7"));
}

// The expression-body spelling of the SAME §17.7 clause ("not permitted at
// an expression body or at a lambda's ascribed return"). Pre-#4249 this one
// compiled with NO diagnostic whatsoever — not even #2491's `internal:`
// Warning the block-body spelling emitted — and SEGV'd identically.
BEZEL_TEST(sema_new_shorthand_default_param_and_return_4208,
           reject_lambda_expression_body_shorthand_spec_17_7_4249) {
    auto r = runAot(
        "module M; "
        "class WidgetLambdaExpr4249 "
        "  public property Tag: int := 77; "
        "end WidgetLambdaExpr4249; "
        "class ProgramWidgetLambdaExpr4249 "
        "  public static method Main(): int "
        "    var f := lambda(): WidgetLambdaExpr4249 => new(); "
        "    var w: WidgetLambdaExpr4249 := f(); "
        "    return w.Tag; "
        "  end Main; "
        "end ProgramWidgetLambdaExpr4249;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "not permitted at a lambda's return"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "§17.7"));
}
