// fix #6915 (§17.14 constant evaluation, §6.1 checked arithmetic, v0.340).
//
// §17.14 (v0.340): "A constant expression is evaluated at compile time under
// the same semantics the expression has at run time -- §6.1 checked integer
// arithmetic", and an evaluation fault -- integer overflow -- "is a compile
// error at the constant expression, never a deferred runtime fault". §4.2:
// "`-(2147483647 + 1)` is an overflowing constant expression and a compile
// error there".
//
// Before the fix a constant `+ - * /` that overflowed its result type was
// rejected only as a whole `const` initializer whose folded value did not fit
// the declared type; anywhere else (`x > 1U - 2U`, `u < 2147483647 + 1`, a
// `long` const initialized with the `int` sum `2147483647 + 1`) it compiled
// and threw OverflowException at run time.
//
// Probes: /var/tmp/bezel-scratch/r34-6869/p/ (i6915_*, a21, a35, m*).

#include <string>

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"         // runAot
#include "test_harness.hpp"

using namespace bezel::test;

namespace {

std::string mainProg(const std::string& body) {
    return "module M;\nclass P public static method Main(): int\n  " + body +
           "\n  return 0; end Main; end P;\n";
}

bool rejected(const std::string& body) {
    auto r = runAot(mainProg(body));
    return failedAtCompileTimeWithRealLocation(r) && !r.ran &&
           diagnosticMessageContains(r, "constant evaluation overflows");
}

std::string prints(const std::string& body) {
    auto r = runAot(mainProg(body));
    if (!r.ran) return "<did not run>";
    if (r.exitCode != 0) return "<exit " + std::to_string(r.exitCode) + ">";
    return r.stdoutText;
}

}  // namespace

// The issue's two repros.
BEZEL_TEST(constant_expression_overflow_6915,
           overflowing_constant_operand_rejected_spec_17_14) {
    BEZEL_EXPECT_TRUE(
        rejected("var x: int := 5; Console.WriteLine(x > 1U - 2U);"));
    BEZEL_EXPECT_TRUE(
        rejected("var u: ulong := 1; Console.WriteLine(u < 2147483647 + 1);"));
}

// Each checked operator, at each width, in any position.
BEZEL_TEST(constant_expression_overflow_6915,
           each_checked_operator_rejected_spec_6_1) {
    BEZEL_EXPECT_TRUE(
        rejected("var x: int := 2147483647 + 1; Console.WriteLine(x);"));
    BEZEL_EXPECT_TRUE(rejected(
        "var x: long := 5; Console.WriteLine(x + (2147483647 + 1));"));
    BEZEL_EXPECT_TRUE(rejected("var y := 2147483647 * 2; Console.WriteLine(y);"));
    BEZEL_EXPECT_TRUE(
        rejected("var y := int.MinValue / -1; Console.WriteLine(y);"));
    BEZEL_EXPECT_TRUE(rejected(
        "var y := 9223372036854775807L + 1L; Console.WriteLine(y);"));
    BEZEL_EXPECT_TRUE(rejected(
        "var y := 18446744073709551615UL + 1UL; Console.WriteLine(y);"));
    BEZEL_EXPECT_TRUE(rejected(
        "const A: byte := 200; var z := A + A; Console.WriteLine(z);"));
    // §4.2: an intervening operation is not the boundary carve-out.
    BEZEL_EXPECT_TRUE(
        rejected("var y := -(2147483647 + 1); Console.WriteLine(y);"));
    // The `int` sum overflows whatever the declared type of the const.
    BEZEL_EXPECT_TRUE(rejected(
        "const B: long := 2147483647 + 1; Console.WriteLine(B);"));
}

// Controls: in-range constants, a non-constant operand (the run-time check
// stays), and a short-circuited operand (§17.14: contributes no fault).
BEZEL_TEST(constant_expression_overflow_6915,
           in_range_runtime_and_short_circuited_unchanged) {
    BEZEL_EXPECT_EQ(prints("var x: int := 5; Console.WriteLine(x > 2U - 1U);"),
                    std::string("True\n"));
    BEZEL_EXPECT_EQ(prints("var y := 2147483646 + 1; Console.WriteLine(y);"),
                    std::string("2147483647\n"));
    BEZEL_EXPECT_EQ(prints("var y := int.MinValue / 1; Console.WriteLine(y);"),
                    std::string("-2147483648\n"));
    BEZEL_EXPECT_EQ(prints("var a: int := 2147483647; try var y := a + 1; "
                           "Console.WriteLine(y); catch OverflowException => "
                           "Console.WriteLine(\"ovf\"); end try;"),
                    std::string("ovf\n"));
    BEZEL_EXPECT_EQ(prints("var y := false and (2147483647 + 1 > 0); "
                           "Console.WriteLine(y);"),
                    std::string("False\n"));
    BEZEL_EXPECT_EQ(prints("var y := (if false then 2147483647 + 1 else 0); "
                           "Console.WriteLine(y);"),
                    std::string("0\n"));
}

// A class-level const: an inferred `int` initializer that overflows, and a
// nested overflow inside an explicitly typed initializer whose folded total
// happens to fit (the declaration's own range check reads the unchecked
// total, so only the evaluation of the inner node catches it).
BEZEL_TEST(constant_expression_overflow_6915,
           class_const_initializer_overflow_rejected_spec_17_14) {
    for (const char* member :
         {"private const K := 2147483647 + 1;",
          "private const K: int := (2147483647 + 1) - 1;"}) {
        auto r = runAot(std::string("module M;\nclass P\n") + member +
                        "\npublic static method Main(): int\n"
                        "  Console.WriteLine(K);\n"
                        "  return 0; end Main; end P;\n");
        BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r) && !r.ran &&
                          diagnosticMessageContains(
                              r, "constant evaluation overflows"));
    }
}
