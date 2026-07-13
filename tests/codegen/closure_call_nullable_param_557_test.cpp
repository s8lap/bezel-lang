// Fix #557 — emitClosureCall does not promote T → T? for nullable value-type
// parameters, emitting a bare scalar (e.g. i32 5) where the declared param
// type is the tagged {i8, T} struct, which the LLVM IR verifier rejects.
//
// Root cause: the coercion loop in emitClosureCall (~L5828) handled only
// numeric-width and float/int conversions, not the T → {i8,T} wrapping
// that regular (non-closure) call sites perform via the CG-nullable block in
// lowerCallArgs (LlvmBackend_Assign.cpp ~L1673).
//
// Fix: after the existing coercions, add a §7.5/§22.1 nullable promotion
// step that checks whether the declared param type is isNullableValueTy AND
// the actual arg has the plain T type (the struct's element[1]) — if so, call
// wrapNullableValue.
//
// Spec derivation:
//   §7.5 — implicit widening from T to T? at call sites
//   §22.1 — argument passing rules
//   §17.14 — lambda / closure expressions
//   §27 — closures

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

int runMain(const std::string& body) {
    return run(
        "module P36; class Program "
        "  public static method Main(): int " +
        body +
        " end Main; end Program;");
}

}  // namespace

// ---------------------------------------------------------------------------
// §557 — closure call with non-nullable int argument to int? parameter
// ---------------------------------------------------------------------------

// Exact repro from issue #557:
// `var fn: (int?) -> int := (x: int?) => x ?? -1; var result := fn(5);`
// Before fix: "Call parameter type does not match function signature!
//   i32 5 ... {i8, i32}" — LLVM IR verify failure.
// After fix: fn(5) promotes 5 to {i8=1, i32=5}, ?? extracts 5. Returns 5.
// per §7.5 / §22.1
BEZEL_TEST(closure_call_nullable_param_557,
           literal_int_arg_to_nullable_int_param_spec_7_5) {
    BEZEL_EXPECT_EQ(runMain(
        "  var fn: (int?) -> int := (x: int?) => x ?? -1; "
        "  var result: int := fn(5); "
        "  return result;"),
        5);
}

// Same closure, argument is a local non-nullable variable (not a literal).
// per §7.5 / §22.1
BEZEL_TEST(closure_call_nullable_param_557,
           variable_int_arg_to_nullable_int_param_spec_7_5) {
    BEZEL_EXPECT_EQ(runMain(
        "  var fn: (int?) -> int := (x: int?) => x ?? -1; "
        "  var n: int := 42; "
        "  var result: int := fn(n); "
        "  return result;"),
        42);
}

// Already-nullable arg: fn(null) → default -1. Regression: a T? arg must
// pass through unchanged (not double-wrapped). -1 does not fit an 8-bit exit
// code, so this builds the full program directly and observes stdout
// (runMain's body-only wrapper can't route through runAotOut).
// per §7.5 / §22.1
BEZEL_TEST(closure_call_nullable_param_557,
           null_arg_to_nullable_int_param_spec_7_5) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module P36; class Program "
        "  public static method Main(): int "
        "  var fn: (int?) -> int := (x: int?) => x ?? -1; "
        "  var result: int := fn(null); "
        "  Console.WriteLine(result); return 0;"
        " end Main; end Program;"),
        "-1");
}

// Multi-arg closure: (int?, int, bool?) -> int.
// First arg: int? with non-nullable int literal — needs promotion.
// Second arg: plain int — no promotion needed.
// Third arg: bool? with non-nullable bool literal — needs promotion.
// per §7.5 / §22.1
BEZEL_TEST(closure_call_nullable_param_557,
           multi_arg_mixed_nullable_nonnullable_spec_7_5_22_1) {
    BEZEL_EXPECT_EQ(runMain(
        "  var fn: (int?, int, bool?) -> int := "
        "    (a: int?, b: int, c: bool?) => do "
        "      var sum: int := a ?? 0; "
        "      var extra: int := (b if c ?? false else 0); "
        "      return sum + extra; "
        "    end; "
        "  var result: int := fn(10, 5, true); "
        "  return result;"),
        15);
}

// Non-nullable lambda (no promotion needed) — regression guard.
// Calling fn(3) where fn: (int) -> int must be unaffected.
// per §22.1
BEZEL_TEST(closure_call_nullable_param_557,
           non_nullable_param_unaffected_regression_spec_22_1) {
    BEZEL_EXPECT_EQ(runMain(
        "  var fn: (int) -> int := (x: int) => x * 2; "
        "  return fn(3);"),
        6);
}

// bool? param: non-nullable bool literal argument promoted to {i8, i1}.
// per §7.5 / §7.1 (bool? → {i8, i1})
BEZEL_TEST(closure_call_nullable_param_557,
           bool_literal_arg_to_nullable_bool_param_spec_7_5) {
    BEZEL_EXPECT_EQ(runMain(
        "  var fn: (bool?) -> int := (x: bool?) => (1 if x ?? false else 0); "
        "  return fn(true);"),
        1);
}
