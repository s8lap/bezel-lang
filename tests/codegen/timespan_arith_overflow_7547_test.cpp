// Pin tests for issue #7547: TimeSpan arithmetic overflow.
//
// stdlib §2.5 (v0.350) declares exactly these TimeSpan operators:
//   operator +(a, b: TimeSpan): TimeSpan
//   operator -(a, b: TimeSpan): TimeSpan
//   operator -(t: TimeSpan): TimeSpan      // OverflowException on MinValue
// and states that TimeSpan arithmetic raises OverflowException outside the
// tick range ("the fault TimeSpan's arithmetic above already raises",
// §2.5 Parse and range contracts). It declares NO `Add` / `Subtract` /
// `Negate` member and NO `<` overload (ordering is CompareTo only).
//
// These rows pin the declared operators on the AOT oracle with exact stdout.
// A distinct marker per exception type proves OverflowException (not some
// other fault) is what is raised.
//
// Cite: stdlib §2.5, v0.350; spec §31.4 (OverflowException), v0.350.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::runAotOut;

namespace {

std::string prog(const std::string& mainBody) {
    return "module M; "
           "class C public static method Main(): int "
           + mainBody +
           " return 0; end Main; end C;";
}

}  // namespace

BEZEL_TEST(timespan_arith_overflow_7547, plus_past_max_raises_overflow) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "var a := TimeSpan.FromTicks(long.MaxValue); "
        "var b := TimeSpan.FromTicks(7L); "
        "try var c := a + b; Console.WriteLine($\"no raise {c}\"); "
        "catch OverflowException => Console.WriteLine(\"OVF\"); "
        "catch Exception => Console.WriteLine(\"OTHER\"); end; ")),
        "OVF");
}

BEZEL_TEST(timespan_arith_overflow_7547, minus_past_min_raises_overflow) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "var a := TimeSpan.FromTicks(long.MinValue); "
        "var b := TimeSpan.FromTicks(7L); "
        "try var c := a - b; Console.WriteLine($\"no raise {c}\"); "
        "catch OverflowException => Console.WriteLine(\"OVF\"); "
        "catch Exception => Console.WriteLine(\"OTHER\"); end; ")),
        "OVF");
}

BEZEL_TEST(timespan_arith_overflow_7547, negate_min_raises_overflow) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "var a := TimeSpan.FromTicks(long.MinValue); "
        "try var c := -a; Console.WriteLine($\"no raise {c}\"); "
        "catch OverflowException => Console.WriteLine(\"OVF\"); "
        "catch Exception => Console.WriteLine(\"OTHER\"); end; ")),
        "OVF");
}

BEZEL_TEST(timespan_arith_overflow_7547, maxvalue_plus_raises_overflow) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "var m := TimeSpan.MaxValue; "
        "var a := TimeSpan.FromSeconds(2.0); "
        "try var c := m + a; Console.WriteLine($\"no raise {c}\"); "
        "catch OverflowException => Console.WriteLine(\"OVF\"); "
        "catch Exception => Console.WriteLine(\"OTHER\"); end; ")),
        "OVF");
}

BEZEL_TEST(timespan_arith_overflow_7547, in_range_arithmetic_is_exact) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "var d := TimeSpan.FromTicks(5L) + TimeSpan.FromTicks(7L) "
        "- TimeSpan.FromTicks(2L); "
        "var e := TimeSpan.FromTicks(long.MaxValue) - "
        "TimeSpan.FromTicks(long.MaxValue); "
        "Console.WriteLine($\"{d.Ticks} {e.Ticks}\"); ")),
        "10 0");
}
