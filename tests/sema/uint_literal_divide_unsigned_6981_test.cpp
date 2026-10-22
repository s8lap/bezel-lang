// fix #6981 (§5.1 "Constant narrowing", §6.2, v0.340).
//
// §6.2: an integer literal operand adopts the other operand's type, so
// `uint / 2` is an unsigned division. The adopting literal stayed typed
// `int`; the operation's signedness is unsigned only when BOTH operand types
// are, so `4000000000u / 2` lowered as `sdiv` and printed 4147483648.
// Same root cause as #6984 (the adopter is now re-typed in
// reportNoImplicitConversionBetweenOperands).
//
// Probes: /var/tmp/bezel-scratch/r34-6984/p/r2.bzl.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using namespace bezel::test;

namespace {

std::string prints(const std::string& body) {
    auto r = runAot("module M;\nclass P\n\npublic static method Main(): int\n  " +
                    body + "\n  return 0; end Main; end P;\n");
    if (!r.ran) return "<did not run>";
    if (r.exitCode != 0) return "<exit " + std::to_string(r.exitCode) + ">";
    return r.stdoutText;
}

}  // namespace

BEZEL_TEST(uint_literal_divide_unsigned_6981,
           uint_divide_and_mod_by_literal_are_unsigned_spec_6_2) {
    BEZEL_EXPECT_EQ(prints("var u: uint := 4000000000U; "
                           "Console.WriteLine(u / 2); "
                           "Console.WriteLine(u mod 7); "
                           "Console.WriteLine(u / 3);"),
                    std::string("2000000000\n3\n1333333333\n"));
}

BEZEL_TEST(uint_literal_divide_unsigned_6981,
           ulong_divide_and_mod_by_literal_are_unsigned_spec_6_2) {
    BEZEL_EXPECT_EQ(prints("var l: ulong := 18000000000000000000UL; "
                           "Console.WriteLine(l / 2); "
                           "Console.WriteLine(l mod 7); "
                           "Console.WriteLine(l / 3);"),
                    std::string("9000000000000000000\n4\n6000000000000000000\n"));
}

// ushort / byte promote to int (§5.1 widening): already correct, pinned.
BEZEL_TEST(uint_literal_divide_unsigned_6981,
           ushort_and_byte_divide_by_literal_spec_6_2) {
    BEZEL_EXPECT_EQ(prints("var s: ushort := 60000; Console.WriteLine(s / 2); "
                           "var b: byte := 250; Console.WriteLine(b / 2); "
                           "Console.WriteLine(b mod 7);"),
                    std::string("30000\n125\n5\n"));
}

BEZEL_TEST(uint_literal_divide_unsigned_6981,
           compound_divide_by_literal_unsigned_spec_6_6) {
    BEZEL_EXPECT_EQ(prints("var u: uint := 4000000000U; u /= 2; "
                           "Console.WriteLine(u); "
                           "var l: ulong := 18000000000000000000UL; l /= 2; "
                           "Console.WriteLine(l);"),
                    std::string("2000000000\n9000000000000000000\n"));
}
