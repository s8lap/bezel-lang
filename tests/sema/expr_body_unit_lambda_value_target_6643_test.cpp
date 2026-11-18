// fix #6643 (§27.3 *Which block-bodied lambdas return a value* /
// *Admission at a target-typed non-call position*, §5.1, v0.359):
//
//   "Expression-bodied lambdas need no inference rule: the body expression's
//    type is the return type, and a body of type `()` makes a procedure
//    lambda."
//
//   "A target type, when present, is checked against this classification — a
//    procedure-bodied lambda does not convert to a value-returning function
//    type, nor the reverse."
//
// #6161 fixed the BLOCK-bodied spelling (`lambda() => do ... end`) at a value
// slot: its check lives inside the block-body branch of the lambda
// return-type inference, so an EXPRESSION body — the very spelling §27.3
// names first — was never judged. An expression body whose type is `()`
// (sema records the UNIT type for a method with no declared return type, and
// leaves a unit-returning BUILT-IN call — `Console.WriteLine`, `Console.Write`,
// `Array<T>.Add` — untyped altogether) was admitted at a `() -> int` slot,
// and the callee read a return value the body never produced.
//
// §5.1 has no `()` -> `int` row, so the conversion check does not catch it
// either: `checkAssignable` has no arm for a `Kind::Unit` source and falls
// through to its final catch-all.
//
// MEASURED on the base (b279e8daf): the issue's repro compiles, prints `x`,
// and then a garbage integer that changes from run to run.
//
// Every row below is a REJECTION row: each one is RED on the base (the
// program compiles there) and GREEN with the fix. The base-green controls —
// a unit target admitting a procedure lambda, a `throw` body at a value slot
// (§17.12's bottom type), a value body at a value slot — are in the lane's
// corpus-candidates directory, not here.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

// The §27.3 verdict both shapes of unit body produce.
#define UNIT_BODY_MSG                                                        \
    "a procedure-bodied lambda (a body of type `()`) does not convert to a " \
    "function type whose return type is a value"

// REJECT: the issue's exact repro, first half — the lambda is a CALL
// ARGUMENT at a `() -> int` parameter (§22.6 *Lambda-literal arguments*).
BEZEL_TEST(expr_body_unit_lambda_value_target_6643,
           expr_body_unit_lambda_at_call_argument_rejected_spec_27_3) {
    auto r = runAot(
        "module T;\n"
        "public static class G\n"
        "  public static method Over(f: () -> int): int => f();\n"
        "end;\n"
        "public static class Program\n"
        "  public static method Main(args: Array<string>): int\n"
        "    Console.WriteLine(G.Over(lambda() => Console.WriteLine(\"x\")));\n"
        "    return 0;\n"
        "  end;\n"
        "end;\n");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, ":7:42: " UNIT_BODY_MSG));
}

// REJECT: the issue's exact repro, second half — the lambda is the
// INITIALIZER of a declared `() -> int` local (§27.3 *Admission at a
// target-typed non-call position*).
BEZEL_TEST(expr_body_unit_lambda_value_target_6643,
           expr_body_unit_lambda_at_typed_local_rejected_spec_27_3) {
    auto r = runAot(
        "module T;\n"
        "public static class Program\n"
        "  public static method Main(args: Array<string>): int\n"
        "    var g: () -> int := lambda() => Console.WriteLine(\"y\");\n"
        "    return 0;\n"
        "  end;\n"
        "end;\n");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, ":4:37: " UNIT_BODY_MSG));
}

// REJECT: the same shape through a different unit-returning built-in —
// `Array<T>.Add` is left untyped by sema exactly like `Console.WriteLine`
// ("Add/Push/Enqueue/Set/RemoveAt/Clear -> unit (void)",
// ExprTypecheck_Calls.cpp).
BEZEL_TEST(expr_body_unit_lambda_value_target_6643,
           expr_body_unit_array_add_at_value_local_rejected_spec_27_3) {
    auto r = runAot(
        "module T;\n"
        "public static class Program\n"
        "  public static method Main(args: Array<string>): int\n"
        "    var xs: Array<int> := [1];\n"
        "    var g: () -> int := lambda() => xs.Add(1);\n"
        "    return 0;\n"
        "  end;\n"
        "end;\n");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, ":5:37: " UNIT_BODY_MSG));
}

// REJECT: the UNIT-TYPED spelling of the same defect — a call to a method
// with NO declared return type. §27.1: "A method without a return type has
// function type returning `()`", so the body's type IS the unit type and the
// lambda is a procedure lambda by §27.3's own sentence.
BEZEL_TEST(expr_body_unit_lambda_value_target_6643,
           expr_body_unit_typed_method_call_at_value_local_rejected_spec_27_3) {
    auto r = runAot(
        "module T;\n"
        "public static class Q\n"
        "  public static method Do()\n"
        "    Console.WriteLine(\"q\");\n"
        "  end Do;\n"
        "end;\n"
        "public static class Program\n"
        "  public static method Main(args: Array<string>): int\n"
        "    var g: () -> int := lambda() => Q.Do();\n"
        "    return 0;\n"
        "  end;\n"
        "end;\n");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, ":9:37: " UNIT_BODY_MSG));
}
