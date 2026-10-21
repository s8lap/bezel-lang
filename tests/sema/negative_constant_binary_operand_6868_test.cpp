// fix #6868 (§5.1 *Constant narrowing*, §6.2 operand promotion, v0.340).
//
// §5.1 (v0.340): "A constant whose value does not fit the target's range is a
// **compile error** at the conversion site -- never a silent truncation." A
// binary or comparison operator over an operand pair with no implicit
// conversion either way (§6.2: ulong/long, uint/int, ...) admits a
// literal-derived operand only by constant narrowing; the escape never asked
// whether the value fits, so `ulong + -3` was 18446744073709551614.
//
// Left unchanged (widening pairs: the wider type decides, §6.2): not tested as
// a rejection here. Probes: /var/tmp/bezel-scratch/r34-6868/.

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
           diagnosticMessageContains(r, "does not fit");
}

std::string prints(const std::string& body) {
    auto r = runAot(mainProg(body));
    if (!r.ran) return "<did not run>";
    if (r.exitCode != 0) return "<exit " + std::to_string(r.exitCode) + ">";
    return r.stdoutText;
}

}  // namespace

// The issue's repro and its operator / operand-order / type siblings.
BEZEL_TEST(negative_constant_binary_operand_6868,
           negative_constant_against_unsigned_operand_rejected_spec_5_1) {
    BEZEL_EXPECT_TRUE(rejected(
        "var t: ulong := 1; var r := t + -3; Console.WriteLine(r);"));
    BEZEL_EXPECT_TRUE(rejected(
        "var t: ulong := 1; var r := -3 + t; Console.WriteLine(r);"));
    BEZEL_EXPECT_TRUE(rejected(
        "var t: ulong := 1; var r := t - -3; Console.WriteLine(r);"));
    BEZEL_EXPECT_TRUE(rejected(
        "var t: ulong := 1; var r := t * -3; Console.WriteLine(r);"));
    BEZEL_EXPECT_TRUE(rejected(
        "var t: ulong := 1; var r := t & -1; Console.WriteLine(r);"));
    BEZEL_EXPECT_TRUE(rejected(
        "var t: uint := 1; var r := t + -3; Console.WriteLine(r);"));
    // int operand, uint constant above int's range.
    BEZEL_EXPECT_TRUE(rejected(
        "var t: int := 1; var r := t + 3_000_000_000U; Console.WriteLine(r);"));
}

// Comparison and equality share the helper.
BEZEL_TEST(negative_constant_binary_operand_6868,
           negative_constant_in_comparison_rejected_spec_5_1) {
    BEZEL_EXPECT_TRUE(rejected(
        "var t: ulong := 1; if t < -1 then Console.WriteLine(1); end if;"));
    BEZEL_EXPECT_TRUE(rejected(
        "var t: uint := 1; if -1 < t then Console.WriteLine(1); end if;"));
    BEZEL_EXPECT_TRUE(rejected(
        "var t: ulong := 1; if t = -1 then Console.WriteLine(1); end if;"));
    BEZEL_EXPECT_TRUE(rejected(
        "var t: ulong := 1; if t <> -1 then Console.WriteLine(1); end if;"));
}

// Controls (19a): in-range constants, -0, the widening pairs, byte arithmetic.
BEZEL_TEST(negative_constant_binary_operand_6868,
           in_range_constants_and_widening_pairs_unchanged) {
    BEZEL_EXPECT_EQ(prints("var t: ulong := 5; Console.WriteLine(t + 3); "
                           "Console.WriteLine(t + 3_000_000_000UL); "
                           "Console.WriteLine(t - 0); Console.WriteLine(t + -0);"
                           " Console.WriteLine(t & 7);"),
                    std::string("8\n3000000005\n5\n5\n5\n"));
    BEZEL_EXPECT_EQ(prints("var t: uint := 5; Console.WriteLine(t + 4294967290U);"
                           " Console.WriteLine(t * 2);"),
                    std::string("4294967295\n10\n"));
    // byte + int widens to int: -3 is a legitimate operand.
    BEZEL_EXPECT_EQ(prints("var b: byte := 5; Console.WriteLine(b + -3); "
                           "Console.WriteLine(b > -3);"),
                    std::string("2\nTrue\n"));
    // int / long, uint / long widening: unchanged.
    BEZEL_EXPECT_EQ(prints("var i: int := 5; var l: long := 7; "
                           "Console.WriteLine(i + -3); "
                           "Console.WriteLine(l + -3_000_000_000L); "
                           "var u: uint := 7; "
                           "Console.WriteLine(u + -3_000_000_000L);"),
                    std::string("2\n-2999999993\n-2999999993\n"));
    BEZEL_EXPECT_EQ(prints("var t: ulong := 1; if t > 0 then "
                           "Console.WriteLine(1); end if; "
                           "if t = 1 then Console.WriteLine(2); end if;"),
                    std::string("1\n2\n"));
}
