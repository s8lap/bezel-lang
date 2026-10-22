// fix #6914 (§5.1 "Constant narrowing", §6.2, §17.14, v0.340).
//
// §5.1 (v0.340): "An expression of integer type that is a compile-time
// constant (§17.14) implicitly converts to any integer type whose range
// contains its value ... The rule is value-based, not type-based: the source
// may be a compile-time constant of any integer type ... across signedness
// (... a `uint`-typed constant `7` is admissible where `int` is expected)",
// and it "applies at every position where this section's implicit
// conversions apply". §17.14 makes a `const` reference (class-level, local,
// `int.MaxValue`) a constant expression.
//
// Before the fix the operand-pair escape admitted only a literal-derived
// operand, so `private const K: int := 7; ... u > K` (u: ulong) was rejected
// with "operator `>` has no implicit conversion between operand types".
//
// Probes: /var/tmp/bezel-scratch/r34-6869/p/ (i6914*, a09-a11, a30).

#include <string>

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"         // runAot
#include "test_harness.hpp"

using namespace bezel::test;

namespace {

std::string prog(const std::string& members, const std::string& body) {
    return "module M;\nclass P\n" + members +
           "\npublic static method Main(): int\n  " + body +
           "\n  return 0; end Main; end P;\n";
}

bool rejected(const std::string& members, const std::string& body,
              const char* needle) {
    auto r = runAot(prog(members, body));
    return failedAtCompileTimeWithRealLocation(r) && !r.ran &&
           diagnosticMessageContains(r, needle);
}

std::string prints(const std::string& members, const std::string& body) {
    auto r = runAot(prog(members, body));
    if (!r.ran) return "<did not run>";
    if (r.exitCode != 0) return "<exit " + std::to_string(r.exitCode) + ">";
    return r.stdoutText;
}

}  // namespace

// The issue's repro, and the local, wrapper-constant and qualified forms.
BEZEL_TEST(named_constant_operand_narrowing_6914,
           named_constant_adopts_other_operand_type_spec_5_1) {
    BEZEL_EXPECT_EQ(prints("private const K: int := 7;",
                           "var u: ulong := 10; Console.WriteLine(u > K);"),
                    std::string("True\n"));
    BEZEL_EXPECT_EQ(prints("", "const K: int := 7; var u: ulong := 10; "
                               "Console.WriteLine(u > K);"),
                    std::string("True\n"));
    BEZEL_EXPECT_EQ(prints("", "var u: ulong := 1; "
                               "Console.WriteLine(u < int.MaxValue);"),
                    std::string("True\n"));
    BEZEL_EXPECT_EQ(prints("public const K: int := 7;",
                           "var u: ulong := 10; Console.WriteLine(u = P.K);"),
                    std::string("False\n"));
}

// §5.1's own example direction (a `uint` constant where `int` is expected),
// and an arithmetic position (§6.2 promotes "using §5.1").
BEZEL_TEST(named_constant_operand_narrowing_6914,
           across_signedness_and_arithmetic_spec_5_1) {
    BEZEL_EXPECT_EQ(prints("", "const KU: uint := 7; var x: int := -5; "
                               "Console.WriteLine(x < KU);"),
                    std::string("True\n"));
    BEZEL_EXPECT_EQ(prints("", "const K: int := 7; var u: ulong := 10; "
                               "Console.WriteLine(u + K);"),
                    std::string("17\n"));
    BEZEL_EXPECT_EQ(prints("", "const K: int := 7; var u: uint := 10; "
                               "Console.WriteLine(u - K);"),
                    std::string("3\n"));
    // The constant adopts `ulong`: the comparison is unsigned, so a `ulong`
    // above LLONG_MAX is greater than 7 on either side of the operator.
    BEZEL_EXPECT_EQ(prints("private const K: int := 7;",
                           "var u: ulong := 18446744073709551615UL; "
                           "Console.WriteLine(u > K); "
                           "Console.WriteLine(K < u);"),
                    std::string("True\nTrue\n"));
    // Same width, local const: without the re-type the compare was signed
    // (4000000000 read as a negative `int`) and printed False.
    BEZEL_EXPECT_EQ(prints("", "const K: int := 7; var u: uint := 4000000000U; "
                               "Console.WriteLine(u > K); "
                               "Console.WriteLine(u - K);"),
                    std::string("True\n3999999993\n"));
}

// Controls: a constant that does not fit is the §5.1 range error; a
// non-constant operand (a variable, a runtime const) stays the §6.2 error.
BEZEL_TEST(named_constant_operand_narrowing_6914,
           out_of_range_and_non_constant_operands_rejected) {
    BEZEL_EXPECT_TRUE(rejected("private const K: int := -7;",
                               "var u: ulong := 10; Console.WriteLine(u > K);",
                               "the constant `-7` does not fit `ulong`"));
    BEZEL_EXPECT_TRUE(rejected("", "const K: int := -7; var u: uint := 10; "
                                   "Console.WriteLine(u > K);",
                               "the constant `-7` does not fit `uint`"));
    BEZEL_EXPECT_TRUE(rejected("", "var i: int := 7; var u: ulong := 10; "
                                   "Console.WriteLine(u > i);",
                               "has no implicit conversion between operand "
                               "types `ulong` and `int`"));
    BEZEL_EXPECT_TRUE(rejected("", "var a: int := 7; const R: int := a; "
                                   "var u: ulong := 10; "
                                   "Console.WriteLine(u > R);",
                               "has no implicit conversion between operand "
                               "types `ulong` and `int`"));
    BEZEL_EXPECT_TRUE(rejected("private static readonly K: int := 7;",
                               "var u: ulong := 10; Console.WriteLine(u > K);",
                               "has no implicit conversion between operand "
                               "types `ulong` and `int`"));
}
