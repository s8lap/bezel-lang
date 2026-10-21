// Regression tests for #6297 — `Math.Abs`, `Math.Floor`, `Math.Ceiling`,
// `Math.Truncate` and `Math.Sign` with a `decimal` argument, and the `Min` /
// `Max` / `Clamp` results that are decimal.
//
// WHAT THE DOCUMENTS REQUIRE.  stdlib §2.2 ("`Math`", v0.339) declares
//     public static pure method Abs(x: decimal): decimal;
//     public static pure method Floor(x: decimal): decimal;    // toward negative infinity;
//     public static pure method Ceiling(x: decimal): decimal;  //   toward positive infinity;
//     public static pure method Truncate(x: decimal): decimal; //   toward zero
// each to an integral value, exact and never faulting, and
//     public static method Sign<T>(x: T): int where T: INumber<T>;
// with `decimal` conforming to INumber<T> (stdlib §2.19).  The printed form is the
// no-format ToString() of stdlib §9.3: scale is not part of a decimal's value, so
// `1.50m` renders `1.5` and `0m` renders `0`.  The expectations below use it.
//
// THE DEFECT.  A decimal lowers to the STRUCT %bz.decimal, and none of these
// members had a decimal arm.  Abs emitted `icmp` and `sub` on the struct, and the
// rounding members emitted `sitofp` on it, so the module failed verification
//     Both operands to ICmp instruction are not of the same type!
//     SIToFP source must be integer or integer vector
// MEASURED on f9c549e0.  Sema also typed the calls wrongly: Abs(decimal) kept its
// int placeholder, the rounding members their double, and Min / Max / Clamp were
// untyped, so `Console.WriteLine(Math.Abs(x))` and `var y := Math.Min(x, 2m)`
// mis-lowered.
//
// THE FIX.  Codegen gains decimal arms that call the runtime: Abs negates through
// bz_decimal_neg where the value is negative; Sign compares with zero through
// bz_decimal_cmp; Floor, Ceiling and Truncate call the new bz_decimal_floor /
// bz_decimal_ceiling / bz_decimal_truncate helpers, which round the 96-bit mantissa
// exactly.  Sema types the call as decimal when every value argument is decimal.
//
// PINNED ON STDOUT.  An exit code is 8 bits, and these results include magnitudes
// such as 79228162514264337593543950335 that do not fit one, so each program prints
// its results and the whole stdout is asserted.  The operands cover negative,
// zero, fractional and large values, including the range endpoints (stdlib §9.3
// MinValue / MaxValue).

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
// Plain runAot throughout: decimal is a value type and none of these programs
// allocates, so generated-code ASan has nothing to observe here.
//
// The captured stdout with every '\r' dropped, so a Windows Console.NewLine
// ("\r\n") compares equal to the Unix one.  A rejected, crashed or timed-out
// program yields the sentinel, never a partial output.
std::string stdoutOf(const std::string& source) {
    auto r = runAot(source);
    if (!r.ran) return std::string("<did-not-run>");
    std::string out;
    for (char c : r.stdoutText)
        if (c != '\r') out += c;
    return out;
}

// A `Main` whose statements are `body`.  Each `Console.WriteLine` is one line.
std::string program(const std::string& body) {
    return "module Conf; public class Program public static method Main() " +
           body + " end Main; end Program;";
}
}  // namespace

// stdlib §2.2 `Abs(x: decimal): decimal`.  Negative, zero (both signs), fractional,
// the magnitude endpoint -MaxValue (§9.3) and the smallest step at scale 28.
BEZEL_TEST(math_decimal_members_6297, abs_decimal_values) {
    BEZEL_EXPECT_EQ(stdoutOf(program(R"(
        Console.WriteLine(Math.Abs(-1.5m));
        Console.WriteLine(Math.Abs(1.5m));
        Console.WriteLine(Math.Abs(0m));
        Console.WriteLine(Math.Abs(-0.0m));
        Console.WriteLine(Math.Abs(-79228162514264337593543950335m));
        Console.WriteLine(Math.Abs(-0.0000000000000000000000000001m));
    )")),
        std::string("1.5\n1.5\n0\n0\n"
                    "79228162514264337593543950335\n"
                    "0.0000000000000000000000000001\n"));
}

// The result of `Abs` is decimal, so a `var` takes it as decimal, a nested
// `Floor` feeds `Abs`, and a decimal variable argument behaves as a literal does.
BEZEL_TEST(math_decimal_members_6297, abs_decimal_result_is_decimal) {
    BEZEL_EXPECT_EQ(stdoutOf(program(R"(
        var x: decimal := -2.25m;
        var y := Math.Abs(x);
        Console.WriteLine(y);
        var z: decimal := Math.Abs(Math.Floor(-2.5m));
        Console.WriteLine(z);
        if Math.Abs(x) = 2.25m then Console.WriteLine(1); end if;
    )")),
        std::string("2.25\n3\n1\n"));
}

// stdlib §2.2 `Floor(x: decimal): decimal`, toward negative infinity.  A negative
// and a positive fraction, zero, a negative value just below zero, a negative
// fraction with a 28-digit scale, and the integral endpoints.
BEZEL_TEST(math_decimal_members_6297, floor_decimal_values) {
    BEZEL_EXPECT_EQ(stdoutOf(program(R"(
        Console.WriteLine(Math.Floor(-1.5m));
        Console.WriteLine(Math.Floor(1.5m));
        Console.WriteLine(Math.Floor(-2.0m));
        Console.WriteLine(Math.Floor(0.9m));
        Console.WriteLine(Math.Floor(-0.1m));
        Console.WriteLine(Math.Floor(-79228162514264337593543950.335m));
        Console.WriteLine(Math.Floor(-7.9228162514264337593543950335m));
        Console.WriteLine(Math.Floor(79228162514264337593543950335m));
    )")),
        std::string("-2\n1\n-2\n0\n-1\n"
                    "-79228162514264337593543951\n"
                    "-8\n"
                    "79228162514264337593543950335\n"));
}

// stdlib §2.2 `Ceiling(x: decimal): decimal`, toward positive infinity.  The
// mirror of Floor: a negative value rounds up to zero without a negative zero.
BEZEL_TEST(math_decimal_members_6297, ceiling_decimal_values) {
    BEZEL_EXPECT_EQ(stdoutOf(program(R"(
        Console.WriteLine(Math.Ceiling(-1.5m));
        Console.WriteLine(Math.Ceiling(1.5m));
        Console.WriteLine(Math.Ceiling(-0.9m));
        Console.WriteLine(Math.Ceiling(2.0m));
        Console.WriteLine(Math.Ceiling(0.1m));
        Console.WriteLine(Math.Ceiling(79228162514264337593543950.335m));
        Console.WriteLine(Math.Ceiling(-7.9228162514264337593543950335m));
        Console.WriteLine(Math.Ceiling(-79228162514264337593543950335m));
    )")),
        std::string("-1\n2\n0\n2\n1\n"
                    "79228162514264337593543951\n"
                    "-7\n"
                    "-79228162514264337593543950335\n"));
}

// stdlib §2.2 `Truncate(x: decimal): decimal`, toward zero.  Truncate never moves
// the value away from zero, so a negative fraction stops at the integer above it.
BEZEL_TEST(math_decimal_members_6297, truncate_decimal_values) {
    BEZEL_EXPECT_EQ(stdoutOf(program(R"(
        Console.WriteLine(Math.Truncate(-1.5m));
        Console.WriteLine(Math.Truncate(1.5m));
        Console.WriteLine(Math.Truncate(-0.9m));
        Console.WriteLine(Math.Truncate(2.99m));
        Console.WriteLine(Math.Truncate(-2.99m));
        Console.WriteLine(Math.Truncate(-7.9228162514264337593543950335m));
        Console.WriteLine(Math.Truncate(79228162514264337593543950.335m));
    )")),
        std::string("-1\n1\n0\n2\n-2\n-7\n"
                    "79228162514264337593543950\n"));
}

// stdlib §2.2 `Sign<T>(x: T): int` with T = decimal (§2.19).  The sign is an int,
// so the call sits in ordinary int arithmetic.
BEZEL_TEST(math_decimal_members_6297, sign_decimal_values) {
    BEZEL_EXPECT_EQ(stdoutOf(program(R"(
        Console.WriteLine(Math.Sign(-1.5m));
        Console.WriteLine(Math.Sign(0.0m));
        Console.WriteLine(Math.Sign(-0.0m));
        Console.WriteLine(Math.Sign(2.5m));
        Console.WriteLine(Math.Sign(-79228162514264337593543950.335m));
        Console.WriteLine(Math.Sign(-79228162514264337593543950335m));
        Console.WriteLine(Math.Sign(-1.5m) * 10 + Math.Sign(2.5m));
    )")),
        std::string("-1\n0\n0\n1\n-1\n-1\n-9\n"));
}

// stdlib §2.2 `Min<T>` / `Max<T>` / `Clamp<T>` at T = decimal.  The result is
// decimal, so it is usable in a `var` and prints as a decimal value.
BEZEL_TEST(math_decimal_members_6297, min_max_clamp_decimal_results) {
    BEZEL_EXPECT_EQ(stdoutOf(program(R"(
        var x: decimal := 2.5m;
        var m := Math.Min(x, 2m);
        Console.WriteLine(m);
        var n := Math.Max(x, 2m);
        Console.WriteLine(n);
        Console.WriteLine(Math.Min(2.5m, 1.5m));
        Console.WriteLine(Math.Max(-3.25m, 2.0m));
        Console.WriteLine(Math.Clamp(0.5m, 1.0m, 9.0m));
        Console.WriteLine(Math.Clamp(5.0m, 1.0m, 9.0m));
        Console.WriteLine(Math.Clamp(-7.5m, -1.0m, 1.0m));
    )")),
        std::string("2\n2.5\n1.5\n2\n1\n5\n-1\n"));
}

// CONTROL: the integer and double members keep their own lowering.  A decimal
// arm must not capture them.
BEZEL_TEST(math_decimal_members_6297, control_int_and_double_unchanged) {
    BEZEL_EXPECT_EQ(stdoutOf(program(R"(
        Console.WriteLine(Math.Abs(-7));
        Console.WriteLine(Math.Min(7, 3) * 10 + Math.Max(7, 3));
        var d: double := Math.Floor(-2.5);
        if d = -3.0 then Console.WriteLine(1); end if;
    )")),
        std::string("7\n37\n1\n"));
}
