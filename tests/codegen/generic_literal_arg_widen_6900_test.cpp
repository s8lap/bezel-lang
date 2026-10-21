// Regression test for issue #6900 (§14.1 / §5.1 / §5.2, v0.340).
//
// A generic method called with an EXPLICIT primitive type argument and bare
// integer literals (`U.Add<long>(2000000000, 2000000000)`) lowered the
// literals as `int` (i32) before `T` was bound, and the specialization took
// its signature from the argument types, so the body ran at i32:
// MEASURED on c0a1928b, `Add<long>` threw OverflowException, `Add<double>(1, 2)`
// failed IR verification, `Div<double>(1, 2)` printed 0.
//
// §14.1: an instantiation at explicit type arguments behaves as the generic
// written out at those types; §5.1: an `int` argument converts to a `long`,
// `double`, `float` or `decimal` parameter; §5.2: an in-range integer literal
// converts to `byte`/`ulong`.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;

// Rows ulong_literals / byte_literals are guards: MEASURED they already pass
// without the fix (the literal is typed at the parameter's width there).

namespace {

std::string runMain(const std::string& body) {
    const std::string src =
        "module App;\n"
        "public class U\n"
        "  public static method Add<T>(a: T, b: T): T where T: INumber<T> => a + b;\n"
        "  public static method Div<T>(a: T, b: T): T where T: INumber<T> => a / b;\n"
        "  public static method Mix<T>(a: T, b: T): T where T: INumber<T> => a * b - a;\n"
        "end U;\n"
        "public class Program\n"
        "  public static method Main(): int\n" + body +
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n";
    JitResult r = bezel::test::runAot(src);
    if (!r.ran || r.exitCode != 0)
        return "<<fail ran=" + std::to_string(r.ran) +
               " rc=" + std::to_string(r.exitCode) + ">>";
    return r.stdoutText;
}

}  // namespace

BEZEL_TEST(generic_literal_arg_widen_6900, long_above_int_max_sum_14_1) {
    BEZEL_EXPECT_EQ(
        runMain("    Console.WriteLine(U.Add<long>(2000000000, 2000000000));\n"),
        std::string("4000000000\n"));
}

BEZEL_TEST(generic_literal_arg_widen_6900, long_negative_and_zero_14_1) {
    BEZEL_EXPECT_EQ(
        runMain("    Console.WriteLine(U.Add<long>(-2000000000, -2000000000));\n"
                "    Console.WriteLine(U.Add<long>(0, 0));\n"
                "    Console.WriteLine(U.Add<long>(2147483647, 1));\n"
                "    Console.WriteLine(U.Add<long>(3000000000L, 1));\n"),
        std::string("-4000000000\n0\n2147483648\n3000000001\n"));
}

BEZEL_TEST(generic_literal_arg_widen_6900, ulong_literals_14_1) {
    BEZEL_EXPECT_EQ(
        runMain("    Console.WriteLine(U.Add<ulong>(2000000000, 2000000000));\n"
                "    Console.WriteLine(U.Add<ulong>(0, 0));\n"
                "    Console.WriteLine(U.Mix<ulong>(2000000000, 5));\n"),
        std::string("4000000000\n0\n8000000000\n"));
}

BEZEL_TEST(generic_literal_arg_widen_6900, double_add_div_14_1) {
    BEZEL_EXPECT_EQ(
        runMain("    Console.WriteLine(U.Add<double>(1, 2));\n"
                "    Console.WriteLine(U.Div<double>(1, 2));\n"
                "    Console.WriteLine(U.Div<double>(-1, 2));\n"
                "    Console.WriteLine(U.Add<double>(0, 0));\n"
                "    Console.WriteLine(U.Add<double>(3000000000L, 1));\n"),
        std::string("3\n0.5\n-0.5\n0\n3000000001\n"));
}

BEZEL_TEST(generic_literal_arg_widen_6900, float_add_div_14_1) {
    BEZEL_EXPECT_EQ(
        runMain("    Console.WriteLine(U.Add<float>(1, 2));\n"
                "    Console.WriteLine(U.Div<float>(1, 2));\n"
                "    Console.WriteLine(U.Div<float>(-1, 4));\n"),
        std::string("3\n0.5\n-0.25\n"));
}

BEZEL_TEST(generic_literal_arg_widen_6900, decimal_add_div_14_1) {
    BEZEL_EXPECT_EQ(
        runMain("    Console.WriteLine(U.Add<decimal>(1, 2));\n"
                "    Console.WriteLine(U.Div<decimal>(1, 4));\n"
                "    Console.WriteLine(U.Add<decimal>(-5, 0));\n"),
        std::string("3\n0.25\n-5\n"));
}

BEZEL_TEST(generic_literal_arg_widen_6900, byte_literals_14_1) {
    BEZEL_EXPECT_EQ(
        runMain("    Console.WriteLine(U.Add<byte>(100, 50));\n"
                "    Console.WriteLine(U.Add<byte>(0, 0));\n"),
        std::string("150\n0\n"));
}

BEZEL_TEST(generic_literal_arg_widen_6900, mixed_variable_and_literal_14_1) {
    BEZEL_EXPECT_EQ(
        runMain("    var x: long := 3000000000L;\n"
                "    var d: double := 0.5;\n"
                "    Console.WriteLine(U.Add<long>(x, 1));\n"
                "    Console.WriteLine(U.Add<double>(d, 1));\n"
                "    Console.WriteLine(U.Mix<long>(2000000000, 3));\n"),
        std::string("3000000001\n1.5\n4000000000\n"));
}
