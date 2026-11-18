// fix #6642 (§22.6 *Lambda-literal arguments* and *Parameter count at a
// function-typed position*, v0.359):
//
//   "A lambda literal has no static type of its own; applicability and
//    ranking at that argument position are decided by its inferred return
//    type. ... for a procedure slot (`-> ()`), [§27.3]'s value/procedure
//    classification decides instead — an expression-bodied lambda whose body
//    is not of type `()` is applicable at a procedure slot (`-> ()`) ... and
//    ranks converting there, while a unit-typed body or a `: ()` ascription is
//    exact and a block-bodied lambda that returns a value is inapplicable:
//    `M(f: () -> ())` beside `M(f: () -> int)` on `M(lambda() => 42)`
//    resolves to the second, exact where the first converts"
//
//   "a candidate whose function-typed slot has a different number of
//    parameter slots from the argument's (... a lambda's parameter count) is
//    inapplicable at that position"
//
// and §22.7: constructor overloading follows the same resolution rules.
//
// Pre-fix `resolveOverload` returned `Undecidable` for any function-typed
// argument, `checkNewExprArgs` published no constructor, and codegen's
// `ctorForArgs` proxy lowered the FIRST DECLARED one. The fix grades a lambda
// literal's column inside the same grid as every other argument, so the
// other columns still veto a candidate and the ordinary better-or-equal /
// co-equal / ambiguity rules decide.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

std::string program6642(const std::string& first, const std::string& second,
                        const std::string& newExpr) {
    return "module T;\n"
           "public class K\n"
           "  public constructor" + first + "\n"
           "  public constructor" + second + "\n"
           "end;\n"
           "public static class Program\n"
           "  public static method Main(args: Array<string>): int\n"
           "    var k := " + newExpr + ";\n"
           "    return 0;\n"
           "  end;\n"
           "end;\n";
}

}  // namespace

// The issue's repro: the value lambda is exact at `() -> int` and converts at
// `() -> ()`; the block-bodied value lambda is inapplicable at `() -> ()`.
// Both declaration orders; the selected constructor calls the lambda.
BEZEL_TEST(constructor_overload_value_lambda_6642,
           value_lambda_selects_int_slot_constructor_both_orders_spec_22_6) {
    const std::string unit = "(f: () -> ()) Console.WriteLine(\"A\"); end;";
    const std::string val =
        "(f: () -> int) Console.WriteLine(\"B:\" + f().ToString()); end;";
    for (int valFirst = 0; valFirst < 2; ++valFirst) {
        const std::string& a = valFirst ? val : unit;
        const std::string& b = valFirst ? unit : val;
        auto e = runAot(program6642(a, b, "new K(lambda() => 41 + 1)"));
        BEZEL_EXPECT_TRUE(e.ran);
        BEZEL_EXPECT_EQ(e.stdoutText, std::string("B:42\n"));
        auto k = runAot(
            program6642(a, b, "new K(lambda() => do return 7; end)"));
        BEZEL_EXPECT_TRUE(k.ran);
        BEZEL_EXPECT_EQ(k.stdoutText, std::string("B:7\n"));
    }
}

// *Parameter count*: a one-parameter lambda is inapplicable at the
// zero-parameter `() -> int` slot, so the `(int) -> ()` constructor is the only
// applicable one, whichever is declared first.
BEZEL_TEST(constructor_overload_value_lambda_6642,
           lambda_parameter_count_rules_out_constructor_both_orders_spec_22_6) {
    const std::string one = "(f: (int) -> ()) Console.WriteLine(\"A\"); end;";
    const std::string zero = "(f: () -> int) Console.WriteLine(\"B\"); end;";
    for (int zeroFirst = 0; zeroFirst < 2; ++zeroFirst) {
        auto r = runAot(program6642(zeroFirst ? zero : one,
                                    zeroFirst ? one : zero,
                                    "new K(lambda(x: int) => x + 1)"));
        BEZEL_EXPECT_TRUE(r.ran);
        BEZEL_EXPECT_EQ(r.stdoutText, std::string("A\n"));
    }
}

// The count rule picks the second-declared candidate too (`() -> int` first).
BEZEL_TEST(constructor_overload_value_lambda_6642,
           lambda_parameter_count_selects_second_constructor_spec_22_6) {
    auto r = runAot(program6642(
        "(f: () -> int) Console.WriteLine(\"A\"); end;",
        "(f: (int) -> int) Console.WriteLine(\"B:\" + f(4).ToString()); end;",
        "new K(lambda(x: int) => x + 1)"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("B:5\n"));
}

// The lambda column ties (both exact), so the `int` argument column decides:
// `int` is exact at `int` and converts at `long`.
BEZEL_TEST(constructor_overload_value_lambda_6642,
           tied_lambda_column_leaves_other_column_to_decide_spec_22_6) {
    auto r = runAot(program6642(
        "(a: long, f: () -> int) Console.WriteLine(\"A\"); end;",
        "(a: int, f: () -> int) Console.WriteLine(\"B\"); end;",
        "new K(1, lambda() => 1)"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("B\n"));
}

// A converting lambda cell wins when the other candidate is inapplicable
// there (`int` does not convert to `string`); the winner receives the
// lambda's value widened to `long`. Pre-fix the first-declared `-> string`
// constructor was lowered and IR verification failed.
BEZEL_TEST(constructor_overload_value_lambda_6642,
           converting_lambda_cell_selects_only_applicable_constructor_spec_22_6) {
    auto r = runAot(program6642(
        "(f: () -> string) Console.WriteLine(\"B:\" + f()); end;",
        "(f: () -> long) Console.WriteLine(\"A:\" + f().ToString()); end;",
        "new K(lambda() => 41 + 1)"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("A:42\n"));
}

// §22.6: "If no candidate is unambiguously better ... the call is ambiguous
// and is a compile error ... it never silently selects one." Each candidate is
// better at one column (`int` exact vs `long`; `() -> int` exact vs `() ->
// ()`), and two converting lambda cells (`-> ()` discards, `int` widens to
// `long`) are co-equal.
BEZEL_TEST(constructor_overload_value_lambda_6642,
           lambda_column_ambiguity_is_reported_spec_22_6) {
    auto crossed = runAot(program6642(
        "(a: int, f: () -> ()) Console.WriteLine(\"A\"); end;",
        "(a: long, f: () -> int) Console.WriteLine(\"B\"); end;",
        "new K(1, lambda() => 1)"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(crossed));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        crossed, ":8:14: ambiguous call to `new K`: no overload is "
                 "unambiguously better (§22.6)"));
    auto coequal = runAot(program6642(
        "(f: () -> ()) Console.WriteLine(\"A\"); end;",
        "(f: () -> long) Console.WriteLine(\"B\"); end;",
        "new K(lambda() => 1)"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(coequal));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        coequal, ":8:14: ambiguous call to `new K`: no overload is "
                 "unambiguously better (§22.6)"));
}

// Boundary control (green on the base): the non-lambda column still vetoes a
// candidate the lambda cell alone would prefer. `"hello"` does not convert to
// `int`, so the `string` constructor is the only applicable one although its
// lambda cell converts. RED under the rejected 1110c58ff, which ranked the
// lambda column alone and lowered the `int` constructor (printing the string
// pointer as an integer).
BEZEL_TEST(constructor_overload_value_lambda_6642,
           other_column_vetoes_lambda_exact_candidate_control_spec_22_6) {
    auto r = runAot(program6642(
        "(s: string, f: () -> ()) Console.WriteLine(\"A:\" + s); end;",
        "(i: int, f: () -> int) Console.WriteLine(\"B:\" + i.ToString()); end;",
        "new K(\"hello\", lambda() => 1)"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("A:hello\n"));
}
