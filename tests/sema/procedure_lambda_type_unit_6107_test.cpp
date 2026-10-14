// fix #6107 (item 1; §27.3 "Which block-bodied lambdas return a value", v0.338):
//
//   "A block-bodied lambda is a **procedure lambda** — its type is `(…) -> ()`
//    — exactly when no path through its body executes a value-carrying
//    `return e;`: its exits are bare `return;` and falling off the end. ...
//    A target type, when present, is checked against this classification — a
//    procedure-bodied lambda does not convert to a value-returning function
//    type, nor the reverse."
//
// Pre-fix sema typed an un-ascribed block lambda with no value-carrying
// `return` as `() -> int` (the inference fallback), so `var w := lambda() =>
// do end;` was accepted at a `() -> int` slot and REJECTED at the `() -> ()`
// slot it actually has.
//
// Not pinned here (separate, pre-existing gaps measured on the same base; see
// the issue filed by this lane): a direct lambda literal at a mismatched
// target (`var f: () -> int := lambda() => do end;`) and the result of a
// `()`-returning call at a value type (`var n: int := w();`) are both still
// accepted.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

// REJECT: the procedure lambda's formed type is `() -> ()`, which does not
// convert to `() -> int` (§27.3, v0.338). Line 5, col 25 is the initializer `w`.
BEZEL_TEST(procedure_lambda_type_unit_6107,
           no_return_block_lambda_not_convertible_to_value_returning_type_spec_27_3) {
    auto r = runAot(
        "module T;\n"
        "public static class Program\n"
        "  public static method Main(args: Array<string>): int\n"
        "    var w := lambda() => do end;\n"
        "    var f: () -> int := w;\n"
        "    return 0;\n"
        "  end;\n"
        "end;\n");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, ":5:25: the initializer for `f` has a type that is not implicitly "
           "convertible to the target type"));
}

// REJECT, bare-`return;` spelling of the same procedure lambda.
BEZEL_TEST(procedure_lambda_type_unit_6107,
           bare_return_block_lambda_not_convertible_to_value_returning_type_spec_27_3) {
    auto r = runAot(
        "module T;\n"
        "public static class Program\n"
        "  public static method Main(args: Array<string>): int\n"
        "    var w := lambda() => do return; end;\n"
        "    var f: () -> int := w;\n"
        "    return 0;\n"
        "  end;\n"
        "end;\n");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, ":5:25: the initializer for `f` has a type that is not implicitly "
           "convertible to the target type"));
}

// ACCEPT: the same procedure lambda converts to its own `() -> ()` type, runs,
// and a value-returning block lambda still has its inferred `() -> int`.
BEZEL_TEST(procedure_lambda_type_unit_6107,
           procedure_lambda_converts_to_unit_function_type_and_value_lambda_stays_int_spec_27_3) {
    auto r = runAot(
        "module T;\n"
        "public static class Program\n"
        "  public static method Main(args: Array<string>): int\n"
        "    var w := lambda() => do return; end;\n"
        "    var f: () -> () := w;\n"
        "    f();\n"
        "    var v := lambda() => do return 7; end;\n"
        "    var g: () -> int := v;\n"
        "    Console.WriteLine(g());\n"
        "    return 0;\n"
        "  end;\n"
        "end;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("7") != std::string::npos);
}
