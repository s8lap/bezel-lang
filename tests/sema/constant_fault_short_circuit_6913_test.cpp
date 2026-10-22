// fix #6913 (§17.14 "Evaluation", §17.2 and §20 short-circuiting, v0.340).
//
// §17.14 (v0.340): a constant expression is evaluated "under the same
// semantics the expression has at run time -- ... §20's and the boolean
// operators' short-circuiting (an operand that would not be evaluated at run
// time is not evaluated at compile time and contributes no fault)".
//
// Before the fix the #6834 negated-unsigned-literal fault was reported in an
// operand that a constant short-circuits: `(if false then -3U else 1U)`,
// `false and (-3U > 0U)` and `true or (-3U > 0U)` were compile errors (the
// last two at location 0:0). Each prints its value now. An operand that IS
// evaluated still faults, at a real location.
//
// Probes: /var/tmp/bezel-scratch/r34-6869/p/ (i6913_*, b17, b21, l1-l4).

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
           diagnosticMessageContains(r, "has no representable result");
}

std::string prints(const std::string& body) {
    auto r = runAot(mainProg(body));
    if (!r.ran) return "<did not run>";
    if (r.exitCode != 0) return "<exit " + std::to_string(r.exitCode) + ">";
    return r.stdoutText;
}

}  // namespace

// The issue's two repros, plus the `or` mirror.
BEZEL_TEST(constant_fault_short_circuit_6913,
           unevaluated_operand_contributes_no_fault_spec_17_14) {
    BEZEL_EXPECT_EQ(prints("var y: uint := (if false then -3U else 1U); "
                           "Console.WriteLine(y);"),
                    std::string("1\n"));
    BEZEL_EXPECT_EQ(prints("var y := false and (-3U > 0U); "
                           "Console.WriteLine(y);"),
                    std::string("False\n"));
    BEZEL_EXPECT_EQ(prints("var y := true or (-3U > 0U); "
                           "Console.WriteLine(y);"),
                    std::string("True\n"));
}

// The deciding constant may be any bool constant expression: `not`, a
// comparison, a `bool` const reference, the `else` branch of a true condition.
BEZEL_TEST(constant_fault_short_circuit_6913,
           deciding_constant_forms_spec_17_14) {
    BEZEL_EXPECT_EQ(prints("var y := not true and (-3U > 0U); "
                           "Console.WriteLine(y);"),
                    std::string("False\n"));
    BEZEL_EXPECT_EQ(prints("var y := 1 > 2 and (-3U > 0U); "
                           "Console.WriteLine(y);"),
                    std::string("False\n"));
    BEZEL_EXPECT_EQ(prints("const F: bool := false; var y := F and "
                           "(-3U > 0U); Console.WriteLine(y);"),
                    std::string("False\n"));
    BEZEL_EXPECT_EQ(prints("var y: uint := (if true then 1U else -3U); "
                           "Console.WriteLine(y);"),
                    std::string("1\n"));
}

// Controls: an operand that is evaluated (a selected branch, a non-constant
// left operand, no short-circuit at all) still faults, at a real location --
// a negated unsigned operand of a comparison used to report at 0:0.
BEZEL_TEST(constant_fault_short_circuit_6913,
           evaluated_operand_still_faults_at_real_location) {
    BEZEL_EXPECT_TRUE(rejected("var y: uint := (if true then -3U else 1U); "
                               "Console.WriteLine(y);"));
    BEZEL_EXPECT_TRUE(rejected("var y := true and (-3U > 0U); "
                               "Console.WriteLine(y);"));
    BEZEL_EXPECT_TRUE(rejected("var b := true; var y := b and (-3U > 0U); "
                               "Console.WriteLine(y);"));
    BEZEL_EXPECT_TRUE(rejected("var y := -3U > 0U; Console.WriteLine(y);"));
}
