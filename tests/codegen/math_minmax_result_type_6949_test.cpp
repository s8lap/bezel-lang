// Regression tests for #6949 -- the result type of `Math.Min/Max/Clamp` at
// int/long/double.
//
// stdlib §2.2: `Min/Max/Clamp<T>(a: T, b: T): T`; spec §14.5 (v0.340) types the
// call as T.  The same-type result-typing switch in typeCall omitted Int, Long
// and Double, so `Math.Max(i, 7)` kept the int placeholder only by accident and
// a member call on it (`.ToString()`) failed to lower (warning #3079) and a
// `var s: string := ...` produced an empty string.

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
std::string stdoutOf(const std::string& body) {
    auto r = runAot("module M; import System; class C "
                    "public static method Main(): int " + body +
                    " return 0; end Main; end C;");
    if (!r.ran) return std::string("<did-not-run>");
    std::string out;
    for (char c : r.stdoutText)
        if (c != '\r') out += c;
    return out;
}
}  // namespace

BEZEL_TEST(math_minmax_result_type_6949, int_to_string_direct) {
    BEZEL_EXPECT_EQ(stdoutOf(
        "var i: int := 5; Console.WriteLine(Math.Max(i, 7).ToString());"),
        std::string("7\n"));
}
BEZEL_TEST(math_minmax_result_type_6949, int_to_string_into_string_local) {
    BEZEL_EXPECT_EQ(stdoutOf(
        "var i: int := 5; var s: string := Math.Max(i, 7).ToString(); "
        "Console.WriteLine(s);"),
        std::string("7\n"));
}
BEZEL_TEST(math_minmax_result_type_6949, int_parenthesised) {
    BEZEL_EXPECT_EQ(stdoutOf(
        "var i: int := 5; Console.WriteLine((Math.Max(i, 7)).ToString());"),
        std::string("7\n"));
}
BEZEL_TEST(math_minmax_result_type_6949, int_var_then_member) {
    BEZEL_EXPECT_EQ(stdoutOf(
        "var i: int := 5; var x := Math.Max(i, 7); "
        "Console.WriteLine(x.ToString());"),
        std::string("7\n"));
}
BEZEL_TEST(math_minmax_result_type_6949, long_to_string) {
    BEZEL_EXPECT_EQ(stdoutOf(
        "var l: long := 5000000000L; "
        "Console.WriteLine(Math.Max(l, 7L).ToString()); "
        "Console.WriteLine(Math.Min(l, 7L).ToString());"),
        std::string("5000000000\n7\n"));
}
BEZEL_TEST(math_minmax_result_type_6949, double_to_string) {
    BEZEL_EXPECT_EQ(stdoutOf(
        "var d: double := 1.5; "
        "Console.WriteLine(Math.Max(d, 0.5).ToString());"),
        std::string("1.5\n"));
}
BEZEL_TEST(math_minmax_result_type_6949, clamp_int_to_string) {
    BEZEL_EXPECT_EQ(stdoutOf(
        "var i: int := 50; Console.WriteLine(Math.Clamp(i, 1, 9).ToString());"),
        std::string("9\n"));
}

// fix #6958 -- an explicit list types the call as T (stdlib §2.2, §14.5).
BEZEL_TEST(math_minmax_result_type_6958, byte_const_operand_prints_unsigned) {
    BEZEL_EXPECT_EQ(stdoutOf(
        "const N: int := 200; var b: byte := byte(3); "
        "Console.WriteLine(Math.Max<byte>(b, N));"),
        std::string("200\n"));
}
BEZEL_TEST(math_minmax_result_type_6958, byte_const_operand_to_string_and_var) {
    BEZEL_EXPECT_EQ(stdoutOf(
        "const N: int := 250; var b: byte := byte(3); "
        "Console.WriteLine(Math.Max<byte>(b, N).ToString()); "
        "var x := Math.Max<byte>(b, N); Console.WriteLine(x);"),
        std::string("250\n250\n"));
}
BEZEL_TEST(math_minmax_result_type_6958, long_with_int_operand_direct) {
    BEZEL_EXPECT_EQ(stdoutOf(
        "var l: long := 5000000000L; var i: int := 7; "
        "Console.WriteLine(Math.Max<long>(i, l)); "
        "Console.WriteLine(Math.Min<long>(i, l));"),
        std::string("5000000000\n7\n"));
}
