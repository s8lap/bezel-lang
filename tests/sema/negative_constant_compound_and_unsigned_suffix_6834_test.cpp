// fix #6834 (§5.1 *Constant narrowing*, §4.2 integer suffixes, §6.1 checked
// unary `-`, §6.6 compound assignment, §17.14 constant evaluation, v0.340).
//
// §5.1 (v0.340): "A constant whose value does not fit the target's range is a
// **compile error** at the conversion site -- never a silent truncation."
// §17.14: a fault in a constant expression (integer overflow) "is a compile
// error at the constant expression, never a deferred runtime fault"; §6.1
// makes unary `-` checked, and `-m` is not representable in `uint`/`ulong`
// for m > 0.
//
// Before the fix `u += -3_000_000_000` (u: ulong) stored 18446744070709551617
// and `-3_000_000_000U` / `-3_000_000_000UL` stored a wrapped value.
//
// Probes: /var/tmp/bezel-scratch/r34-6834/.

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

bool rejected(const std::string& body, const char* needle) {
    auto r = runAot(mainProg(body));
    return failedAtCompileTimeWithRealLocation(r) && !r.ran &&
           diagnosticMessageContains(r, needle);
}

std::string prints(const std::string& body) {
    auto r = runAot(mainProg(body));
    if (!r.ran) return "<did not run>";
    if (r.exitCode != 0) return "<exit " + std::to_string(r.exitCode) + ">";
    return r.stdoutText;
}

}  // namespace

// The issue's compound-assignment repro.
BEZEL_TEST(negative_constant_compound_and_unsigned_suffix_6834,
           compound_negative_constant_at_unsigned_target_rejected_spec_5_1) {
    BEZEL_EXPECT_TRUE(rejected(
        "var u: ulong := 1; u += -3_000_000_000; Console.WriteLine(u);",
        "does not fit"));
    BEZEL_EXPECT_TRUE(rejected(
        "var u: ulong := 1; u -= -3_000_000_000; Console.WriteLine(u);",
        "does not fit"));
    BEZEL_EXPECT_TRUE(rejected(
        "var u: ulong := 1; u *= -3; Console.WriteLine(u);", "does not fit"));
    BEZEL_EXPECT_TRUE(rejected(
        "var u: uint := 1; u += -3; Console.WriteLine(u);", "does not fit"));
    BEZEL_EXPECT_TRUE(rejected(
        "var u: ulong := 1; u |= -1; Console.WriteLine(u);", "does not fit"));
}

// Controls: in-range constants and non-adopting pairs are unchanged.
BEZEL_TEST(negative_constant_compound_and_unsigned_suffix_6834,
           compound_in_range_and_widening_pairs_unchanged) {
    BEZEL_EXPECT_EQ(prints("var u: ulong := 1; u += 3_000_000_000; "
                           "Console.WriteLine(u);"),
                    std::string("3000000001\n"));
    BEZEL_EXPECT_EQ(prints("var u: uint := 5; u -= 3; Console.WriteLine(u);"),
                    std::string("2\n"));
    // byte + int widens: R = int, checked write-back (§6.6) -- not an adoption.
    BEZEL_EXPECT_EQ(prints("var b: byte := 5; b += -3; Console.WriteLine(b);"),
                    std::string("2\n"));
    BEZEL_EXPECT_EQ(prints("var i: int := 5; i += -3; Console.WriteLine(i);"),
                    std::string("2\n"));
}

// The issue's suffix repros: U at uint, UL at ulong.
BEZEL_TEST(negative_constant_compound_and_unsigned_suffix_6834,
           negated_unsigned_suffixed_literal_rejected_spec_6_1_17_14) {
    BEZEL_EXPECT_TRUE(rejected(
        "var u: uint := -3_000_000_000U; Console.WriteLine(u);",
        "has no representable result"));
    BEZEL_EXPECT_TRUE(rejected(
        "var u: ulong := -3_000_000_000UL; Console.WriteLine(u);",
        "has no representable result"));
    BEZEL_EXPECT_TRUE(rejected(
        "var u: uint := -3U; Console.WriteLine(u);",
        "has no representable result"));
    BEZEL_EXPECT_TRUE(rejected(
        "var u: ulong := -(3UL); Console.WriteLine(u);",
        "has no representable result"));
}

// Controls: -0U is 0; positive suffixed constants; signed suffix L.
BEZEL_TEST(negative_constant_compound_and_unsigned_suffix_6834,
           zero_and_positive_suffixed_literals_unchanged) {
    BEZEL_EXPECT_EQ(prints("var a: uint := -0U; var b: ulong := -0UL; "
                           "Console.WriteLine(a); Console.WriteLine(b);"),
                    std::string("0\n0\n"));
    BEZEL_EXPECT_EQ(prints("var a: uint := 4294967295U; var b: ulong := "
                           "3_000_000_000UL; Console.WriteLine(a); "
                           "Console.WriteLine(b);"),
                    std::string("4294967295\n3000000000\n"));
    BEZEL_EXPECT_EQ(prints("var a: long := -3_000_000_000L; "
                           "Console.WriteLine(a);"),
                    std::string("-3000000000\n"));
}
