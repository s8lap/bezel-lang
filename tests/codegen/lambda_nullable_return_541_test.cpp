// Fix #541 — lambda whose body is a non-null underlying value but whose
// EXPECTED return type is a nullable value type `T?` emits malformed IR:
//   ret i32 7  into a  {i8, i32}-returning closure
// which the LLVM verifier rejects with:
//   "Function return type does not match operand type of return inst!"
//
// Root cause: lowerLambda (LlvmBackend_Exprs.cpp) correctly derives
// `retTy = {i8, i32}` for a `(K) -> int?` context via the expected-function-
// type fallback (lines 3626-3630), but when it emits the expr-body `ret` it
// passes the raw lowered value (`i32 7`) directly — no nullable promotion.
// The promotion that `var x: int? := 7` applies (wrapNullableValue) was not
// applied here.
//
// Fix: in lowerLambda's expr-body emit path, when `retTy` is a nullable-value
// struct (`{i8, T}`) and the lowered body value is a bare `T`, wrap it with
// wrapNullableValue before the `ret`.  Mirror the null-literal case with
// nullableValueNull.
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

// Helper: wrap Main body in standard module/class shell.
int runMain(const std::string& body) {
    return run(
        "module M; class C public static method Main(): int " +
        body + " end Main; end C;");
}

// Helper: run a program that calls a method accepting (K)->int? lambda.
int runApplyNullableInt(const std::string& mainBody) {
    return run(
        "module M; class C "
        "  public static method Apply(k: string, f: (string) -> int?): int? "
        "    return f(k); "
        "  end Apply; "
        "  public static method Main(): int " +
        mainBody +
        " end Main; end C;");
}

// Helper: run a program that calls a method accepting ()->double? lambda.
int runApplyNullableDouble(const std::string& mainBody) {
    return run(
        "module M; class C "
        "  public static method Apply(f: () -> double?): double? "
        "    return f(); "
        "  end Apply; "
        "  public static method Main(): int " +
        mainBody +
        " end Main; end C;");
}


bool runs(const std::string& body) {
    auto r = runAot(
        std::string("module M; class C public static method Main(): int ") +
        body + " end Main; end C;");
    return r.ran;
}

}  // namespace

// ---------------------------------------------------------------------------
// §541 / §7 / §17.14: lambda returning int literal in (string)->int? context
// ---------------------------------------------------------------------------

// A lambda `(k: string) => 7` where `(string) -> int?` is expected.
// Before fix: LLVM verifier rejects `ret i32 7` in a `{i8,i32}`-returning
// closure → ran=false → exitCode = -999.
// After fix: the body value is wrapped → hasValue=1 → Apply returns int?
// with value 7 → unwrap with `!` → Main returns 7.
BEZEL_TEST(lambda_nullable_return_541, int_literal_in_nullable_int_context_spec_7_17) {
    BEZEL_EXPECT_EQ(runApplyNullableInt(
        "  var r: int? := Apply(\"k\", lambda(k: string) => 7); "
        "  if r = null then return -1; end; "
        "  return r!;"),
        7);
}

// A lambda `(k: string) => k.Length` where `(string) -> int?` is expected.
// The body is an expression (not a literal), should also be promoted.
// §7.1 / §17.14: any non-null T value must be wrapped to int? when the
// lambda return is int?.
BEZEL_TEST(lambda_nullable_return_541, expr_body_in_nullable_int_context_spec_7_17) {
    BEZEL_EXPECT_EQ(runApplyNullableInt(
        "  var r: int? := Apply(\"hello\", lambda(k: string) => k.Length); "
        "  if r = null then return -1; end; "
        "  return r!;"),
        5);
}

// ---------------------------------------------------------------------------
// §541 / §7 / §17.14: lambda returning int literal in var-bound (T)->int? context
// ---------------------------------------------------------------------------

// Bind a lambda to a local variable typed `(string) -> int?`, then invoke it.
// The nullable promotion must fire even when the lambda is stored then called.
// §17.14: lambdas stored in locals have the same function type as their
// declaration context.
BEZEL_TEST(lambda_nullable_return_541, lambda_stored_in_nullable_fn_var_spec_17) {
    BEZEL_EXPECT_EQ(runMain(
        "  var fn: (string) -> int? := lambda(k: string) => 42; "
        "  var r: int? := fn(\"x\"); "
        "  if r = null then return -1; end; "
        "  return r!;"),
        42);
}

// ---------------------------------------------------------------------------
// §541 / §7.2 / §17.14: lambda returning null in int? context
// ---------------------------------------------------------------------------

// A lambda `(k: string) => null` in (string)->int? context must produce the
// tagged-null struct {i8 0, i32 0}.
// §7.2: the `null` literal in a nullable value type context lowers to the
// all-zero tagged struct (hasValue=0).
BEZEL_TEST(lambda_nullable_return_541, null_literal_in_nullable_int_context_spec_7_2) {
    BEZEL_EXPECT_EQ(runApplyNullableInt(
        "  var r: int? := Apply(\"k\", lambda(k: string) => null); "
        "  if r <> null then return -1; end; "
        "  return 0;"),
        0);
}

// ---------------------------------------------------------------------------
// §541 / §7 / §17.14: double? context
// ---------------------------------------------------------------------------

// A lambda `() => 3` in ()->double? context.
// The literal 3 should be promoted to double? (hasValue=1, value≈3.0).
// We test by returning 0 on success (value non-null) or -1 if null.
BEZEL_TEST(lambda_nullable_return_541, int_literal_in_nullable_double_context_spec_7_17) {
    BEZEL_EXPECT_EQ(runApplyNullableDouble(
        "  var r: double? := Apply(lambda() => 3); "
        "  if r = null then return -1; end; "
        "  return 0;"),
        0);
}

// ---------------------------------------------------------------------------
// §541 / stdlib §4.8: Dictionary<string, int?>.GetOrAdd with nullable factory
// ---------------------------------------------------------------------------

// Dictionary<string,int?>.GetOrAdd("key", (k: string) => 7) must produce the
// value 7 wrapped in int?, not null.
// §4.8 stdlib: GetOrAdd(key, factory:(K)->V) stores and returns the factory
// result when the key is absent.
// §7 / §541: the factory lambda returns int (not int?) → must be wrapped.
BEZEL_TEST(lambda_nullable_return_541, dict_getOrAdd_nullable_int_factory_spec_4_8_7) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var d := new Dictionary<string, int?>(); "
        "  var v: int? := d.GetOrAdd(\"key\", lambda(k: string) => 7); "
        "  if v = null then return -1; end; "
        "  return v!; "
        "end Main; end C;"),
        7);
}

// ---------------------------------------------------------------------------
// CONTROLS: changes should not break existing non-nullable lambda behaviour
// ---------------------------------------------------------------------------

// §17.14 CONTROL: lambda returning int where int (non-nullable) is expected.
// Must still work correctly (no spurious wrapping).
BEZEL_TEST(lambda_nullable_return_541, control_non_nullable_lambda_unaffected_spec_17) {
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

// §7.1 CONTROL: `var x: int? := 7` promotion (not via lambda).
// Must still work (the direct assignment wrapping should be unaffected).
BEZEL_TEST(lambda_nullable_return_541, control_direct_nullable_assign_spec_7_1) {
    BEZEL_EXPECT_EQ(runMain(
        "  var x: int? := 7; "
        "  if x = null then return -1; end; "
        "  return x!;"),
        7);
}

// §7.11 / §486 CONTROL: lifted operator (int? + int?) still works.
// The nullable-widening fix must not disturb lifted-operator codegen.
BEZEL_TEST(lambda_nullable_return_541, control_lifted_operator_spec_7_11) {
    BEZEL_EXPECT_EQ(runMain(
        "  var a: int? := 3; "
        "  var b: int? := 4; "
        "  var c: int? := a + b; "
        "  if c = null then return -1; end; "
        "  return c!;"),
        7);
}
