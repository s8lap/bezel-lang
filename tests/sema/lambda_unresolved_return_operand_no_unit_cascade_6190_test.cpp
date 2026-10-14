// fix #6190 (item 2; §27.3 "Which block-bodied lambdas return a value", v0.338):
// a lambda is a procedure lambda exactly when no path executes a value-carrying
// `return e;`. A `return e;` whose operand failed to resolve (diagnosed on its
// own) is still value-carrying: pre-fix the type scan saw no resolved return
// type, fell to the unit fallback #6107 introduced, and the lambda's call result
// `f()` then cascaded a spurious "operator `+` is not defined for a unit (`()`)
// operand" (§3.5) on top of the real diagnostics.

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

// REJECT with exactly the primary diagnostic: `y` is unresolved; no unit cascade.
BEZEL_TEST(lambda_unresolved_return_operand_no_unit_cascade_6190,
           unresolved_value_return_operand_does_not_make_unit_lambda_spec_27_3) {
    auto r = runAot(
        "module T;\n"
        "public static class Program\n"
        "  public static method Main(args: Array<string>): int\n"
        "    var f := lambda() => do return y; end;\n"
        "    var n := 1 + f();\n"
        "    return 0;\n"
        "  end;\n"
        "end;\n");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "unresolved identifier `y`"));
    BEZEL_EXPECT_TRUE(!diagnosticMessageContains(r, "is not defined for a unit"));
}

// The lit 1589 shape (a value `return` in a nested `finally` is itself a §31.2
// error): the §31.2 diagnostic is reported and the `n + f()` operand is not a
// unit.
BEZEL_TEST(lambda_unresolved_return_operand_no_unit_cascade_6190,
           return_in_nested_finally_in_lambda_no_unit_cascade_spec_31_2) {
    auto r = runAot(
        "module M;\n"
        "class C\n"
        "  public static method Foo(): int\n"
        "    var n := 0;\n"
        "    try n := n + 1; finally\n"
        "      var f := lambda() => do try var x := 1; finally return x; end; end;\n"
        "      n := n + f();\n"
        "    end;\n"
        "    return n;\n"
        "  end Foo;\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`return` inside a `finally` block is a compile error"));
    BEZEL_EXPECT_TRUE(!diagnosticMessageContains(r, "is not defined for a unit"));
}
