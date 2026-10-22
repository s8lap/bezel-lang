// Regression tests for #6974 -- `Math.Min/Max/Clamp<decimal>` with a non-decimal
// operand failed the IR verifier (`icmp sgt i32 2, %bz.decimal`), and
// `Math.Max<decimal>('a', 'd')` printed `d`.
//
// Spec §14.5 (v0.340): with an explicit list each argument converts to T at its
// conversion site; §5.1: int/long/uint/ulong/byte/char etc. widen implicitly to
// decimal. stdlib §2.2: `Max<T>(a: T, b: T): T`, so the call is a decimal.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
std::string out(const std::string& body) {
    auto r = runAot("module M; import System; class C public static method Main(): int " +
                    body + " return 0; end Main; end C;");
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}
}  // namespace

BEZEL_TEST(math_minmax_decimal_6974, issue_repro_int_literal_with_decimal) {
    BEZEL_EXPECT_EQ(out("Console.WriteLine(Math.Max<decimal>(2, 3.5m));"),
                    std::string("3.5\n"));
}
BEZEL_TEST(math_minmax_decimal_6974, int_variable_wins_and_loses) {
    BEZEL_EXPECT_EQ(out("var i: int := 7; "
                        "Console.WriteLine(Math.Max<decimal>(i, 3.5m)); "
                        "Console.WriteLine(Math.Min<decimal>(i, 3.5m));"),
                    std::string("7\n3.5\n"));
}
BEZEL_TEST(math_minmax_decimal_6974, char_operands_are_decimal_code_points) {
    BEZEL_EXPECT_EQ(out("Console.WriteLine(Math.Max<decimal>('a', 'd')); "
                        "var c: char := 'd'; "
                        "Console.WriteLine(Math.Min<decimal>(c, 'a') + 0.5m);"),
                    std::string("100\n97.5\n"));
}
BEZEL_TEST(math_minmax_decimal_6974, unsigned_operands_above_signed_max) {
    BEZEL_EXPECT_EQ(out("var b: byte := 200; var u: uint := 4000000000u; "
                        "var ul: ulong := 18000000000000000000ul; "
                        "Console.WriteLine(Math.Min<decimal>(b, 300.25m)); "
                        "Console.WriteLine(Math.Max<decimal>(u, 1m)); "
                        "Console.WriteLine(Math.Max<decimal>(ul, u));"),
                    std::string("200\n4000000000\n18000000000000000000\n"));
}
BEZEL_TEST(math_minmax_decimal_6974, same_typed_integer_operands_result_is_decimal) {
    BEZEL_EXPECT_EQ(out("var r := Math.Max<decimal>(2, 3); "
                        "Console.WriteLine(r + 0.5m); "
                        "var b: byte := 200; "
                        "Console.WriteLine(Math.Min<decimal>(b, b) / 8);"),
                    std::string("3.5\n25\n"));
}
BEZEL_TEST(math_minmax_decimal_6974, clamp_mixed_operands) {
    BEZEL_EXPECT_EQ(out("var i: int := 7; "
                        "Console.WriteLine(Math.Clamp<decimal>(i, 1, 5m)); "
                        "Console.WriteLine(Math.Clamp<decimal>(50, 1m, 5)); "
                        "Console.WriteLine(Math.Clamp<decimal>(-3, 1m, 5));"),
                    std::string("5\n5\n1\n"));
}
BEZEL_TEST(math_minmax_decimal_6974, all_decimal_control) {
    BEZEL_EXPECT_EQ(out("Console.WriteLine(Math.Max<decimal>(2m, 3.5m)); "
                        "Console.WriteLine(Math.Max(2m, 3.5m));"),
                    std::string("3.5\n3.5\n"));
}
