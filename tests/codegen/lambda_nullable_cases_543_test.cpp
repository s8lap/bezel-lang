// Fix #543 — two IR-verification-failure cases for lambdas returning T?,
// not covered by #541.
//
// BUG-1: expr-body lambda returning T? via a CONDITIONAL expression.
//   `var f: (int) -> int? := (x: int) => (x if x > 0 else null);`
//   emits `ret {i8, i32}` (the nullable struct, correct) BUT the lambda
//   function's LLVM return TYPE is `ptr` (the class-reference pattern),
//   so they mismatch → IR verification failure.
//   Root cause: in lowerLambda, retTy is set from ctx_.exprType(*lam.exprBody);
//   for a conditional whose null-branch makes sema type it as a reference/Object,
//   lowerTypeRef returns `ptr` even though the conditional body already produces
//   {i8,T}. The expectedFunctionType_ override only fires when retTy == i32(), not
//   when retTy is `ptr`.
//   Fix: when expectedFunctionType_->funcReturn is a nullable value type, it should
//   override the body-derived retTy unconditionally (not only when retTy == i32).
//
// BUG-2: block-body lambda returning a nullable RECORD (`Point?`), with a
//   `return null;` path.
//   `var f: (int) -> Point? := (x: int) => do
//      if x > 0 then return new Point(x, x); end;
//      return null;
//   end;`
//   The `return null;` emits `ret ptr null` but the function returns
//   `{i8, %record.Point}` → IR verification failure.
//   Root cause: inside a block-body lambda, currentReturnTypeNode_ refers to the
//   OUTER function's return type (or nullptr), so the null literal is not typed as
//   Point? by the sema context. lowerExpr(null) falls back to ConstantPointerNull.
//   emitReturn(fn, ConstantPointerNull) only wraps if the type mismatch is
//   detected AND both the fn return type is a 2-element nullable struct AND v is a
//   ConstantPointerNull — this should work, but the wrapping path has a gap for
//   non-null returns (return new Point) which emitReturn also needs to wrap.
//
// Spec derivation: §7 (nullable types), §7.1 (nullable value types lower to
// {i8 hasValue; T value}), §7.2 (null literal), §17.14 (lambda expressions),
// §27 (closures).

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

// Helper: run Main body in a standard module/class shell.
int runMain(const std::string& body) {
    return run(
        "module M; class C public static method Main(): int " +
        body + " end Main; end C;");
}

// Helper: module with a two-field Point record and a Main body.
int runPoint(const std::string& mainBody) {
    return run(
        "module M; record Point(X: int, Y: int); "
        "class C public static method Main(): int " + mainBody +
        " end Main; end C;");
}

// Helper: module with a Box record and Apply + Main.
int runApplyNullableIntConditional(const std::string& mainBody) {
    return run(
        "module M; class C "
        "  public static method Apply(x: int, f: (int) -> int?): int? "
        "    return f(x); "
        "  end Apply; "
        "  public static method Main(): int " +
        mainBody +
        " end Main; end C;");
}

}  // namespace

// ---------------------------------------------------------------------------
// BUG-1: §543 / §7 / §17.14 — expr-body lambda, conditional returning T?
// ---------------------------------------------------------------------------

// A lambda `(x: int) => (x if x > 0 else null)` typed (int)->int?.
// Before fix: lowerLambda derives retTy=ptr from the conditional body's
// sema type (Object?), but the body value is {i8,i32}. IR verifier rejects
// `ret {i8,i32}` in a `ptr`-returning function.
// After fix: retTy = {i8,i32} (from expectedFunctionType_), body is already
// the right type, ret succeeds.
// x=5 → has-value, value=5.
BEZEL_TEST(lambda_nullable_cases_543, bug1_expr_conditional_nonnull_int_spec_7_17) {
    BEZEL_EXPECT_EQ(runApplyNullableIntConditional(
        "  var r: int? := Apply(5, lambda(x: int) => (x if x > 0 else null)); "
        "  if r = null then return -1; end; "
        "  return r!;"),
        5);
}

// Same conditional lambda, null path: x=-1 → null.
BEZEL_TEST(lambda_nullable_cases_543, bug1_expr_conditional_null_int_spec_7_17) {
    BEZEL_EXPECT_EQ(runApplyNullableIntConditional(
        "  var r: int? := Apply(-1, lambda(x: int) => (x if x > 0 else null)); "
        "  if r <> null then return -1; end; "
        "  return 0;"),
        0);
}

// Same pattern but for bool? (§7.1: bool? → {i8, i1}).
BEZEL_TEST(lambda_nullable_cases_543, bug1_expr_conditional_bool_nullable_spec_7_17) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Apply(x: int, f: (int) -> bool?): bool? "
        "    return f(x); "
        "  end Apply; "
        "  public static method Main(): int "
        "    var r: bool? := Apply(1, lambda(x: int) => (true if x > 0 else null)); "
        "    if r = null then return -1; end; "
        "    return int(r!); "
        "  end Main; end C;"),
        1);
}

// Same pattern but for long? (§7.1: long? → {i8, i64}).
BEZEL_TEST(lambda_nullable_cases_543, bug1_expr_conditional_long_nullable_spec_7_17) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Apply(x: int, f: (int) -> long?): long? "
        "    return f(x); "
        "  end Apply; "
        "  public static method Main(): int "
        "    var r: long? := Apply(7, lambda(x: int) => (long(x) if x > 0 else null)); "
        "    if r = null then return -1; end; "
        "    return int(r!); "
        "  end Main; end C;"),
        7);
}

// expr-body conditional for int? stored in a local var binding (var f: (int)->int?).
// This exercises the lowerLocalDecl path (fix #541) combined with BUG-1.
BEZEL_TEST(lambda_nullable_cases_543, bug1_expr_conditional_local_var_spec_17) {
    BEZEL_EXPECT_EQ(runMain(
        "  var f: (int) -> int? := lambda(x: int) => (x * 2 if x > 0 else null); "
        "  var r: int? := f(4); "
        "  if r = null then return -1; end; "
        "  return r!;"),
        8);
}

// ---------------------------------------------------------------------------
// BUG-2: §543 / §7 / §17.14 — block-body lambda, return null in nullable record
// ---------------------------------------------------------------------------

// A lambda `(int) -> Point?` with two paths: return new Point or return null.
// Before fix: `return null;` emits `ret ptr null` into a {i8,%record.Point}-typed
// function → IR fail. `return new Point(...)` may also mismatch (bare record
// into nullable-record-returning fn).
// After fix: both paths produce correctly-typed nullable struct values.
// x=5 → has-value path, X+Y = 10.
BEZEL_TEST(lambda_nullable_cases_543, bug2_block_nullable_record_nonnull_spec_7_17) {
    BEZEL_EXPECT_EQ(runPoint(
        "  var f: (int) -> Point? := lambda(x: int) => do "
        "    if x > 0 then return new Point(x, x); end; "
        "    return null; "
        "  end; "
        "  var r: Point? := f(5); "
        "  if r = null then return -1; end; "
        "  return r!.X + r!.Y;"),
        10);
}

// x=-1 → null path.
BEZEL_TEST(lambda_nullable_cases_543, bug2_block_nullable_record_null_spec_7_17) {
    BEZEL_EXPECT_EQ(runPoint(
        "  var f: (int) -> Point? := lambda(x: int) => do "
        "    if x > 0 then return new Point(x, x); end; "
        "    return null; "
        "  end; "
        "  var r: Point? := f(-1); "
        "  if r <> null then return -1; end; "
        "  return 0;"),
        0);
}

// Block-body lambda for int? with return value + return null (primitive).
// §7.1 / §17.14: block-body return of primitive in nullable context.
BEZEL_TEST(lambda_nullable_cases_543, bug2_block_nullable_int_primitive_spec_7_1) {
    BEZEL_EXPECT_EQ(runMain(
        "  var f: (int) -> int? := lambda(x: int) => do "
        "    if x > 0 then return x * 3; end; "
        "    return null; "
        "  end; "
        "  var r1: int? := f(4); "
        "  var r2: int? := f(-1); "
        "  if r1 = null then return -1; end; "
        "  if r2 <> null then return -2; end; "
        "  return r1!;"),
        12);
}

// ---------------------------------------------------------------------------
// CONTROLS: #541 cases must still pass (regression guard)
// ---------------------------------------------------------------------------

// §541 control: lambda `(k: string) => 7` in (string)->int? context (literal body).
BEZEL_TEST(lambda_nullable_cases_543, control_541_int_literal_still_works_spec_7) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Apply(k: string, f: (string) -> int?): int? "
        "    return f(k); "
        "  end Apply; "
        "  public static method Main(): int "
        "    var r: int? := Apply(\"k\", lambda(k: string) => 7); "
        "    if r = null then return -1; end; "
        "    return r!; "
        "  end Main; end C;"),
        7);
}

// §541 control: lambda stored in local var typed (string)->int?.
BEZEL_TEST(lambda_nullable_cases_543, control_541_stored_lambda_still_works_spec_17) {
    BEZEL_EXPECT_EQ(runMain(
        "  var fn: (string) -> int? := lambda(k: string) => 42; "
        "  var r: int? := fn(\"x\"); "
        "  if r = null then return -1; end; "
        "  return r!;"),
        42);
}

// §17.14 CONTROL: non-nullable lambda unaffected.
BEZEL_TEST(lambda_nullable_cases_543, control_non_nullable_lambda_unaffected_spec_17) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public static method Apply(n: int, f: (int) -> int): int "
        "    return f(n); "
        "  end Apply; "
        "  public static method Main(): int "
        "    return Apply(5, lambda(x: int) => x * 2); "
        "  end Main; end C;"),
        10);
}

// §7.1 CONTROL: direct `var x: int? := 7` promotion unaffected.
BEZEL_TEST(lambda_nullable_cases_543, control_direct_nullable_assign_spec_7_1) {
    BEZEL_EXPECT_EQ(runMain(
        "  var x: int? := 7; "
        "  if x = null then return -1; end; "
        "  return x!;"),
        7);
}
