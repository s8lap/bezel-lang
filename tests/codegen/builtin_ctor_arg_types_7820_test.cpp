// Spec-derived regression test for issue #7820 -- a `long` / `uint` / `double`
// argument at an `int` parameter of a built-in constructor must be a compile
// error, not a silent truncation (campaign builtin-call-admission, lane C2-44).
//
// --- THE NORMATIVE SENTENCES (CHECKED in docs/, v0.350) ----------------------
//
// spec §5.1 (implicit conversions): there is no implicit narrowing -- an
//   integer converts implicitly only to a type that holds every value of it
//   (`byte`/`short`/`int` to `long`), signed and unsigned of the same or
//   narrower width never convert, and a compile-time integer CONSTANT that
//   fits the target converts ("constant narrowing").
// stdlib §2.5 declares `DateTime(year: int, month: int, day: int, hour: int,
//   minute: int, second: int, millisecond: int := 0, kind: ...)`,
//   `DateOnly(year: int, month: int, day: int)` and `TimeOnly(hour: int,
//   minute: int, second: int := 0, millisecond: int := 0)`; `StringBuilder(
//   capacity: int)`, `OwningBuffer<T>(length: int)`, `Semaphore(initialCount:
//   int[, maxCount: int])`, `Channel<T>(capacity: int)` and `Random(seed:
//   long)` declare the other parameters this file reads.
//
// --- THE DEFECT (MEASURED on staging 3a9c389b) -------------------------------
//
// `new TimeOnly(1,2,3, m)` over `m: long := 4294967297` printed millisecond 1,
// over `m: uint` compiled, `new DateOnly(2020,1,d)` over a `double` aborted the
// compiler in the LLVM verifier ("trunc double to i32"), and the same hole
// admitted `new StringBuilder(n)`, `new Semaphore(n)`, `new Channel<int>(n)` and
// `new OwningBuffer<int>(n)` over a `long`: the built-in constructors were never
// compared with their declared parameter types.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotOut;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::diagnosticMessageContains;

namespace {

std::string prog(const std::string& body) {
    return "module M; import System; import System.Text; "
           "import System.Threading; "
           "class C "
           "  public static method Main(): int " +
           body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

// The program must be refused with the `argument of the `head` constructor`
// diagnostic, located, and without reaching the LLVM verifier.
void expectRefused(const std::string& body, const char* param,
                   const char* head) {
    auto r = runAot(prog(body));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, (std::string("the `") + param + "` argument of the `" + head +
            "` constructor has a type that is not implicitly convertible "
            "to the target type (\xC2\xA7""5.1)").c_str()));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
}

}  // namespace

// The issue's a8: a `long` whose low 32 bits are 1 (read back as 1).
BEZEL_TEST(builtin_ctor_arg_types_7820, timeonly_long_millisecond_refused) {
    expectRefused(
        "    var m: long := 4294967297; var x := new TimeOnly(1, 2, 3, m); "
        "    Console.WriteLine(x.Millisecond);",
        "millisecond", "TimeOnly");
}

// The issue's a7: a `uint` (signed/unsigned never converts implicitly).
BEZEL_TEST(builtin_ctor_arg_types_7820, timeonly_uint_millisecond_refused) {
    expectRefused(
        "    var m: uint := 5; var x := new TimeOnly(1, 2, 3, m); "
        "    Console.WriteLine(x.Millisecond);",
        "millisecond", "TimeOnly");
}

// Every other int-typed position of the three date/time constructors.
BEZEL_TEST(builtin_ctor_arg_types_7820, datetime_dateonly_long_uint_double_refused) {
    expectRefused("    var n: long := 5; var x := new DateTime(2020, n, 3); ",
                  "month", "DateTime");
    expectRefused("    var n: uint := 5; var x := new DateOnly(2020, 1, n); ",
                  "day", "DateOnly");
    expectRefused("    var n: long := 5; "
                  "var x := new DateTime(n, 1, 1, 0, 0, 0); ",
                  "year", "DateTime");
    expectRefused("    var n: double := 5.0; var x := new DateOnly(2020, 1, n); ",
                  "day", "DateOnly");
    expectRefused("    var n: long := 5; "
                  "var x := new DateTime(2020, 1, 1, 1, 1, 1, n); ",
                  "millisecond", "DateTime");
}

// A NAMED argument is checked at the parameter it names (§22.5), in any order.
BEZEL_TEST(builtin_ctor_arg_types_7820, named_argument_checked_at_its_parameter) {
    expectRefused(
        "    var m: long := 5; "
        "var x := new TimeOnly(hour := 1, minute := 2, millisecond := m); ",
        "millisecond", "TimeOnly");
    expectRefused(
        "    var m: long := 5; "
        "var x := new TimeOnly(minute := 2, hour := m); ",
        "hour", "TimeOnly");
}

// The hole is every built-in constructor with a declared scalar parameter,
// not the date types alone.
BEZEL_TEST(builtin_ctor_arg_types_7820, other_builtin_constructors_refused) {
    expectRefused("    var n: long := 5; var sb := new StringBuilder(n); ",
                  "capacity", "StringBuilder");
    expectRefused("    var n: double := 5.0; var sb := new StringBuilder(n); ",
                  "capacity", "StringBuilder");
    expectRefused("    var n: long := 5; using var s := new Semaphore(n); ",
                  "initialCount", "Semaphore");
    expectRefused("    var n: long := 5; using var s := new Channel<int>(n); ",
                  "capacity", "Channel");
    expectRefused("    var n: double := 5.0; using var s := new Channel<int>(n); ",
                  "capacity", "Channel");
    expectRefused("    var n: long := 5; using var b := new OwningBuffer<int>(n); ",
                  "length", "OwningBuffer");
    expectRefused("    var n: double := 5.0; var r := new Random(n); ",
                  "seed", "Random");
}

// ACCEPT controls: without these the check could refuse every argument.
BEZEL_TEST(builtin_ctor_arg_types_7820, int_and_narrower_arguments_still_accepted) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var i: int := 9; var b: byte := 3; var s: short := 4; "
        "    var t := new TimeOnly(i, b, 1, s); "
        "    Console.WriteLine(t.Hour); Console.WriteLine(t.Minute); "
        "    Console.WriteLine(t.Millisecond); "
        "    var d := new DateOnly(2020, 1, i); Console.WriteLine(d.Day); "
        "    var sb := new StringBuilder(i); Console.WriteLine(sb.Length); "
        "    using var ch := new Channel<int>(i); Console.WriteLine(1);")),
        "9\n3\n4\n9\n0\n1");
}

// §5.1 constant narrowing: an in-range `long` constant converts; one that
// does not fit is the §5.1 compile error.
BEZEL_TEST(builtin_ctor_arg_types_7820, constant_narrowing_in_range_accepted) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var t := new TimeOnly(1, 2, 3, 5L); "
        "    Console.WriteLine(t.Millisecond);")),
        "5");
    auto r = runAot(prog(
        "    var t := new TimeOnly(1, 2, 3, 5000000000L); "));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}

// `StringBuilder` has the `string` overload at the same position: the string
// form and an int are both still accepted, and widening reaches `Random`'s
// `long` seed.
BEZEL_TEST(builtin_ctor_arg_types_7820, overloaded_position_and_widening_accepted) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var sb := new StringBuilder(\"ab\"); Console.WriteLine(sb.Length); "
        "    var n: int := 7; var u: uint := 7; var l: long := 7; "
        "    var r1 := new Random(n); var r2 := new Random(u); "
        "    var r3 := new Random(l); "
        "    Console.WriteLine(r1.Next(1, 2) + r2.Next(1, 2) + r3.Next(1, 2));")),
        "2\n3");
}

// A named, REORDERED, int-typed binding is accepted and read by name.
BEZEL_TEST(builtin_ctor_arg_types_7820, named_int_arguments_accepted) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var m: int := 7; var t := new TimeOnly(minute := 2, hour := m); "
        "    Console.WriteLine(t.ToString());")),
        "07:02:00");
}
