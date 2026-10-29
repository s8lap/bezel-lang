// Spec-derived regression test for issue #7819 -- an out-of-range component
// at the DateTime / DateOnly constructor must raise ArgumentOutOfRangeException
// instead of being silently normalised (campaign builtin-call-admission, lane
// C2-44).
//
// --- THE NORMATIVE SENTENCES (CHECKED in docs/, v0.350) ----------------------
//
// stdlib §2.5, the DateTime constructor: "public constructor(year: int, month:
//   int, day: int, hour: int, minute: int, second: int, millisecond: int := 0,
//   kind: ...);  // ArgumentOutOfRangeException on invalid components".
// stdlib §2.5, the DateTime properties the components read back through:
//   "Month: int; // 1..12", "Day: int; // 1..31", "Hour: int; // 0..23",
//   "Minute: int; // 0..59", "Second: int; // 0..59".
// stdlib §2.5, DateOnly: "public constructor(year: int, month: int, day: int);
//   // ArgumentOutOfRangeException on invalid"; `DateTime.Date: DateOnly` is
//   "the date components", so DateOnly's Month and Day carry DateTime's ranges.
// stdlib §2.5 closing paragraph: "the constructors' component validation
//   keeps ArgumentOutOfRangeException" (a band-1 argument defect, not the
//   OverflowException of the Add* family).
//
// NOT CHECKED (the stdlib states no range -- reported, not decided): Year,
// Millisecond, the day-of-month-for-the-month rule (Feb 30), and the
// TimeOnly constructor (it declares no throw).
//
// --- THE DEFECT (MEASURED on staging 3a9c389b) -------------------------------
//
// `new DateTime(2020,1,-1,1,1,1).Day` printed 30 and `new DateOnly(2020,1,40)
// .Day` printed 9: the C `timegm` normalised the component.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotOut;

namespace {

// Prints "threw" if the construction raises ArgumentOutOfRangeException,
// otherwise `read` (an expression over `x`).
std::string probe(const std::string& vars, const std::string& ctor,
                  const std::string& read) {
    return "module M; import System; "
           "class C "
           "  public static method Main(): int " +
           vars +
           "    try "
           "      var x := " + ctor + "; "
           "      Console.WriteLine(" + read + "); "
           "    catch var e: ArgumentOutOfRangeException => "
           "      Console.WriteLine(\"threw\") "
           "    end; "
           "    return 0; "
           "  end Main; "
           "end C;";
}

std::string dt(const std::string& vars, const std::string& args) {
    return probe(vars, "new DateTime(" + args + ")", "x.Day");
}

}  // namespace

// The issue's d1: a negative day through a variable (not a literal).
BEZEL_TEST(datetime_ctor_component_range_7819, datetime_negative_day_throws) {
    BEZEL_EXPECT_EQ(runAotOut(dt("var d: int := -1; ", "2020, 1, d, 1, 1, 1")),
                    "threw");
}

BEZEL_TEST(datetime_ctor_component_range_7819, datetime_each_component_out_of_range) {
    // month 0 and 13, day 0 and 32, hour 24, minute 60, second 60.
    BEZEL_EXPECT_EQ(runAotOut(dt("var v: int := 0; ", "2020, v, 1, 0, 0, 0")), "threw");
    BEZEL_EXPECT_EQ(runAotOut(dt("var v: int := 13; ", "2020, v, 1, 0, 0, 0")), "threw");
    BEZEL_EXPECT_EQ(runAotOut(dt("var v: int := 0; ", "2020, 1, v, 0, 0, 0")), "threw");
    BEZEL_EXPECT_EQ(runAotOut(dt("var v: int := 32; ", "2020, 1, v, 0, 0, 0")), "threw");
    BEZEL_EXPECT_EQ(runAotOut(dt("var v: int := 24; ", "2020, 1, 1, v, 0, 0")), "threw");
    BEZEL_EXPECT_EQ(runAotOut(dt("var v: int := 60; ", "2020, 1, 1, 0, v, 0")), "threw");
    BEZEL_EXPECT_EQ(runAotOut(dt("var v: int := 60; ", "2020, 1, 1, 0, 0, v")), "threw");
}

// The three-argument form has its own arm of the same lowering.
BEZEL_TEST(datetime_ctor_component_range_7819, datetime_three_arg_form_throws) {
    BEZEL_EXPECT_EQ(runAotOut(dt("var v: int := 13; ", "2020, v, 1")), "threw");
    BEZEL_EXPECT_EQ(runAotOut(dt("var v: int := 40; ", "2020, 1, v")), "threw");
}

// ACCEPT controls: the stated bounds themselves are valid, so the guard cannot
// be off by one in either direction (and cannot be an unconditional throw).
BEZEL_TEST(datetime_ctor_component_range_7819, datetime_boundaries_are_valid) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; import System; "
        "class C "
        "  public static method Main(): int "
        "    var lo := new DateTime(2020, 1, 1, 0, 0, 0); "
        "    var hi := new DateTime(2020, 12, 31, 23, 59, 59); "
        "    Console.WriteLine(lo.ToString()); "
        "    Console.WriteLine(hi.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "2020-01-01T00:00:00\n2020-12-31T23:59:59");
}

// The DateTimeKind-bearing eight-argument form and named arguments (reordered)
// reach the same guard through the bound argument.
BEZEL_TEST(datetime_ctor_component_range_7819, datetime_named_reordered_throws) {
    BEZEL_EXPECT_EQ(runAotOut(dt("var v: int := 40; ",
                                 "year := 2020, day := v, month := 1")),
                    "threw");
    BEZEL_EXPECT_EQ(runAotOut(dt("var v: int := 3; ",
                                 "year := 2020, day := v, month := 1")),
                    "3");
}

// The issue's d5: DateOnly day 40.
BEZEL_TEST(datetime_ctor_component_range_7819, dateonly_out_of_range_throws) {
    BEZEL_EXPECT_EQ(runAotOut(probe("var v: int := 40; ",
                                    "new DateOnly(2020, 1, v)", "x.Day")),
                    "threw");
    BEZEL_EXPECT_EQ(runAotOut(probe("var v: int := 0; ",
                                    "new DateOnly(2020, 1, v)", "x.Day")),
                    "threw");
    BEZEL_EXPECT_EQ(runAotOut(probe("var v: int := 13; ",
                                    "new DateOnly(2020, v, 1)", "x.Month")),
                    "threw");
    BEZEL_EXPECT_EQ(runAotOut(probe("var v: int := 0; ",
                                    "new DateOnly(2020, v, 1)", "x.Month")),
                    "threw");
}

BEZEL_TEST(datetime_ctor_component_range_7819, dateonly_boundaries_are_valid) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; import System; "
        "class C "
        "  public static method Main(): int "
        "    var a := new DateOnly(2020, 1, 1); "
        "    var b := new DateOnly(2020, 12, 31); "
        "    Console.WriteLine(a.ToString()); "
        "    Console.WriteLine(b.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "2020-01-01\n2020-12-31");
}

// The raise is an ArgumentException too (stdlib §2.5: the AOORE derives from
// ArgumentException), and it is catchable as one.
BEZEL_TEST(datetime_ctor_component_range_7819, caught_as_argument_exception) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; import System; "
        "class C "
        "  public static method Main(): int "
        "    var d: int := 99; "
        "    try "
        "      var x := new DateTime(2020, 1, d, 0, 0, 0); "
        "      Console.WriteLine(x.Day); "
        "    catch var e: ArgumentException => "
        "      Console.WriteLine(\"arg\") "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "arg");
}

// Loop-100 throw/catch under the ASan/LSan oracle: the raise must not leak.
BEZEL_TEST(datetime_ctor_component_range_7819, throw_catch_loop_100_no_leak) {
    JitResult r = runAotChecked(
        "module M; import System; "
        "class C "
        "  public static method Main(): int "
        "    var n: int := 0; var d: int := 99; "
        "    for var i in 0..<100 do "
        "      try "
        "        var x := new DateTime(2020, 1, d, 0, 0, 0); "
        "        n := n + x.Day; "
        "      catch var e: ArgumentOutOfRangeException => "
        "        n := n + 1; "
        "      end; "
        "      try "
        "        var y := new DateOnly(2020, 13, 1); "
        "        n := n + y.Day; "
        "      catch var e2: ArgumentOutOfRangeException => "
        "        n := n + 1; "
        "      end; "
        "    end; "
        "    Console.WriteLine(n); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "200\n");
}
