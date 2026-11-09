// Spec-derived regression test for issue #7819 -- the DateTime / DateOnly /
// TimeOnly CONSTRUCTORS must raise ArgumentOutOfRangeException on an
// out-of-range component instead of silently normalising (wrapping) it.
// (campaign:builtin-call-admission, lane r41-fix-7819)
//
// --- THE NORMATIVE SENTENCES (CHECKED in docs/, v0.355) ---------------------
//
// stdlib §2.5, "Component ranges" (docs/Bezel_Standard_Library.md:433):
//
//   "The constructors of `DateTime`, `DateOnly` and `TimeOnly` raise
//    `ArgumentOutOfRangeException` when a component falls outside its range:
//    `year` 1..9999, `month` 1..12, `day` 1 to the length of that month in
//    that year of the proleptic Gregorian calendar that ISO 8601 uses
//    (February 29 exists only in a leap year), `hour` 0..23, `minute` 0..59,
//    `second` 0..59 and `millisecond` 0..999.  `DateOnly` takes the date
//    ranges and `TimeOnly` the time ranges."
//
// stdlib §2.5, the DateTime constructor declaration: "public constructor(year:
//   int, month: int, day: int, hour: int, minute: int, second: int,
//   millisecond: int := 0, kind: DateTimeKind := DateTimeKind.Unspecified);
//   // ArgumentOutOfRangeException on invalid components".
// stdlib §2.5, DateOnly: "public constructor(year: int, month: int, day: int);
//   // ArgumentOutOfRangeException on invalid".
// stdlib §2.5, TimeOnly: "public constructor(hour: int, minute: int, second:
//   int := 0, millisecond: int := 0);  // ArgumentOutOfRangeException on
//   invalid components".
// stdlib §2.5: the raise "is declared here, deriving from `ArgumentException`
//   so that an `ArgumentException` catch arm covers the whole
//   argument-validation family".
//
// --- THE DEFECT (MEASURED on the base) --------------------------------------
//
// The C `timegm` behind `bz_datetime_parts_to_ticks` NORMALISES an
// out-of-range component instead of rejecting it, so the constructor read
// back a WRAPPED value: a `DateTime` day of -1 read back as 30, a
// `DateOnly(2020,1,40)` read back as 9, a `TimeOnly` millisecond of 65535
// read back as 0, and a `DateTime` millisecond of 2000000 was folded into the
// tick count silently. The issue's own probes: d1.bzl, d2.bzl, d5.bzl,
// a9.bzl.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotOut;

namespace {

// Prints "AOOR" if the construction raises ArgumentOutOfRangeException,
// otherwise "ok" followed by `read` (an expression over `x`).
std::string probe(const std::string& vars, const std::string& ctor,
                  const std::string& read) {
    return "module M; import System; "
           "class C "
           "  public static method Main(): int "
           "    " + vars +
           "    try "
           "      var x := " + ctor + "; "
           "      Console.WriteLine(\"ok\"); "
           "      Console.WriteLine(" + read + "); "
           "    catch var e: ArgumentOutOfRangeException => "
           "      Console.WriteLine(\"AOOR\") "
           "    end; "
           "    return 0; "
           "  end Main; "
           "end C;";
}

std::string dt(const std::string& vars, const std::string& args) {
    return probe(vars, "new DateTime(" + args + ")", "x.Day");
}

std::string do_(const std::string& vars, const std::string& args) {
    return probe(vars, "new DateOnly(" + args + ")", "x.Day");
}

std::string to(const std::string& vars, const std::string& args) {
    return probe(vars, "new TimeOnly(" + args + ")", "x.Millisecond");
}

}  // namespace

// The issue's FOUR probes. Each must raise; each reads back a WRAPPED value
// on the base (30, 0, 9 and the folded millisecond).
BEZEL_TEST(datetime_range_ctor_7819, issue_probe_d1_datetime_negative_day) {
    BEZEL_EXPECT_EQ(runAotOut(dt("var d: int := -1; ", "2020, 1, d, 1, 1, 1")),
                    "AOOR");
}

BEZEL_TEST(datetime_range_ctor_7819, issue_probe_d2_timeonly_millisecond) {
    BEZEL_EXPECT_EQ(runAotOut(to("var d: int := 65535; ", "1, 2, 3, d")),
                    "AOOR");
}

BEZEL_TEST(datetime_range_ctor_7819, issue_probe_d5_dateonly_day_40) {
    BEZEL_EXPECT_EQ(runAotOut(do_("var d: int := 40; ", "2020, 1, d")),
                    "AOOR");
}

BEZEL_TEST(datetime_range_ctor_7819, issue_probe_a9_datetime_millisecond) {
    BEZEL_EXPECT_EQ(
        runAotOut(probe("var d: int := 2000000; ",
                         "new DateTime(2020, 1, 1, 1, 1, 1, d)",
                         "x.Millisecond")),
        "AOOR");
}

// EVERY component the stdlib declares, one row each: year, month, day, hour,
// minute, second, millisecond -- on `DateTime`.
BEZEL_TEST(datetime_range_ctor_7819, datetime_year_out_of_range) {
    BEZEL_EXPECT_EQ(runAotOut(dt("var v: int := 0; ", "v, 1, 1, 0, 0, 0")),
                    "AOOR");
    BEZEL_EXPECT_EQ(runAotOut(dt("var v: int := 10000; ", "v, 1, 1, 0, 0, 0")),
                    "AOOR");
    BEZEL_EXPECT_EQ(runAotOut(dt("var v: int := -1; ", "v, 1, 1, 0, 0, 0")),
                    "AOOR");
}

BEZEL_TEST(datetime_range_ctor_7819, datetime_month_out_of_range) {
    BEZEL_EXPECT_EQ(runAotOut(dt("var v: int := 0; ", "2020, v, 1, 0, 0, 0")),
                    "AOOR");
    BEZEL_EXPECT_EQ(runAotOut(dt("var v: int := 13; ", "2020, v, 1, 0, 0, 0")),
                    "AOOR");
}

BEZEL_TEST(datetime_range_ctor_7819, datetime_day_out_of_range) {
    BEZEL_EXPECT_EQ(runAotOut(dt("var v: int := 0; ", "2020, 1, v, 0, 0, 0")),
                    "AOOR");
    BEZEL_EXPECT_EQ(runAotOut(dt("var v: int := 32; ", "2020, 1, v, 0, 0, 0")),
                    "AOOR");
}

BEZEL_TEST(datetime_range_ctor_7819, datetime_time_components_out_of_range) {
    BEZEL_EXPECT_EQ(runAotOut(dt("var v: int := 24; ", "2020, 1, 1, v, 0, 0")),
                    "AOOR");
    BEZEL_EXPECT_EQ(runAotOut(dt("var v: int := 60; ", "2020, 1, 1, 0, v, 0")),
                    "AOOR");
    BEZEL_EXPECT_EQ(runAotOut(dt("var v: int := 60; ", "2020, 1, 1, 0, 0, v")),
                    "AOOR");
    BEZEL_EXPECT_EQ(runAotOut(dt("var v: int := 1000; ", "2020, 1, 1, 0, 0, 0, v")),
                    "AOOR");
}

// The day is bounded by the LENGTH OF THAT MONTH IN THAT YEAR (the proleptic
// Gregorian calendar ISO 8601 uses), so February 29 exists only in a leap
// year. A guard that does not know about leap years is wrong.
BEZEL_TEST(datetime_range_ctor_7819, datetime_day_bounded_by_the_month) {
    // February 30 never exists.
    BEZEL_EXPECT_EQ(runAotOut(dt("var v: int := 30; ", "2020, 2, v, 0, 0, 0")),
                    "AOOR");
    // April has 30 days.
    BEZEL_EXPECT_EQ(runAotOut(dt("var v: int := 31; ", "2020, 4, v, 0, 0, 0")),
                    "AOOR");
    // A non-leap year's February 29 does not exist ...
    BEZEL_EXPECT_EQ(runAotOut(dt("var v: int := 29; ", "2021, 2, v, 0, 0, 0")),
                    "AOOR");
    // ... a leap year's does (divisible by 4, not a century year) ...
    BEZEL_EXPECT_EQ(runAotOut(dt("var v: int := 29; ", "2020, 2, v, 0, 0, 0")),
                    "ok\n29");
    // ... and a century year only when divisible by 400.
    BEZEL_EXPECT_EQ(runAotOut(dt("var v: int := 29; ", "1900, 2, v, 0, 0, 0")),
                    "AOOR");
    BEZEL_EXPECT_EQ(runAotOut(dt("var v: int := 29; ", "2000, 2, v, 0, 0, 0")),
                    "ok\n29");
}

// The same DATE ranges on `DateOnly` -- "DateOnly takes the date ranges".
BEZEL_TEST(datetime_range_ctor_7819, dateonly_date_ranges) {
    BEZEL_EXPECT_EQ(runAotOut(do_("var v: int := 0; ", "v, 1, 1")), "AOOR");
    BEZEL_EXPECT_EQ(runAotOut(do_("var v: int := 10000; ", "v, 1, 1")), "AOOR");
    BEZEL_EXPECT_EQ(runAotOut(do_("var v: int := 0; ", "2020, v, 1")), "AOOR");
    BEZEL_EXPECT_EQ(runAotOut(do_("var v: int := 13; ", "2020, v, 1")), "AOOR");
    BEZEL_EXPECT_EQ(runAotOut(do_("var v: int := 0; ", "2020, 1, v")), "AOOR");
    BEZEL_EXPECT_EQ(runAotOut(do_("var v: int := 40; ", "2020, 1, v")), "AOOR");
    BEZEL_EXPECT_EQ(runAotOut(do_("var v: int := 29; ", "2021, 2, v")), "AOOR");
    BEZEL_EXPECT_EQ(runAotOut(do_("var v: int := 29; ", "2020, 2, v")), "ok\n29");
}

// The same TIME ranges on `TimeOnly` -- "TimeOnly takes the time ranges".
BEZEL_TEST(datetime_range_ctor_7819, timeonly_time_ranges) {
    BEZEL_EXPECT_EQ(runAotOut(to("var v: int := 24; ", "v, 0, 0")), "AOOR");
    BEZEL_EXPECT_EQ(runAotOut(to("var v: int := -1; ", "v, 0, 0")), "AOOR");
    BEZEL_EXPECT_EQ(runAotOut(to("var v: int := 60; ", "0, v, 0")), "AOOR");
    BEZEL_EXPECT_EQ(runAotOut(to("var v: int := 60; ", "0, 0, v")), "AOOR");
    BEZEL_EXPECT_EQ(runAotOut(to("var v: int := 1000; ", "0, 0, 0, v")), "AOOR");
    BEZEL_EXPECT_EQ(runAotOut(to("var v: int := -1; ", "0, 0, 0, v")), "AOOR");
}

// A COMPUTED component is validated too: a guard that only constant-folds
// its argument would let these through.
BEZEL_TEST(datetime_range_ctor_7819, computed_components_are_validated) {
    BEZEL_EXPECT_EQ(runAotOut(do_("var d: int := 0; ", "2020, 1, d + 40")),
                    "AOOR");
    BEZEL_EXPECT_EQ(runAotOut(to("var d: int := 1000; ", "1, 2, 3, d * 2")),
                    "AOOR");
    BEZEL_EXPECT_EQ(runAotOut(dt("var y: int := 2021; ", "y, 2, 29, 0, 0, 0")),
                    "AOOR");
}

// ACCEPT controls: the stated bounds THEMSELVES are valid, so the guard
// cannot be off by one in either direction (and cannot be an unconditional
// throw). A guard that made these fail would reject every valid date.
BEZEL_TEST(datetime_range_ctor_7819, boundaries_are_valid) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; import System; "
        "class C "
        "  public static method Main(): int "
        "    var a := new DateTime(1, 1, 1, 0, 0, 0, 0); "
        "    var b := new DateTime(9999, 12, 31, 23, 59, 59, 999); "
        "    var c := new DateOnly(2020, 1, 31); "
        "    var d := new DateOnly(2020, 12, 31); "
        "    var e := new DateOnly(2020, 2, 29); "
        "    var f := new TimeOnly(0, 0, 0, 0); "
        "    var g := new TimeOnly(23, 59, 59, 999); "
        "    Console.WriteLine(a.Year); "
        "    Console.WriteLine(b.Year); "
        "    Console.WriteLine(b.ToString()); "
        "    Console.WriteLine(c.Day); "
        "    Console.WriteLine(d.Day); "
        "    Console.WriteLine(e.Day); "
        "    Console.WriteLine(f.Millisecond); "
        "    Console.WriteLine(g.Millisecond); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "1\n9999\n9999-12-31T23:59:59\n31\n31\n29\n0\n999");
}

// The raise is an `ArgumentException` too (stdlib §2.5: the AOORE derives
// from `ArgumentException`), so it is catchable as one.
BEZEL_TEST(datetime_range_ctor_7819, caught_as_argument_exception) {
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

// The three-argument DateTime form and the NAMED (reordered) spelling reach
// the same guard through the §22.5 binding.
BEZEL_TEST(datetime_range_ctor_7819, three_arg_and_named_forms) {
    BEZEL_EXPECT_EQ(runAotOut(dt("var v: int := 13; ", "2020, v, 1")), "AOOR");
    BEZEL_EXPECT_EQ(runAotOut(dt("var v: int := 40; ", "2020, 1, v")), "AOOR");
    BEZEL_EXPECT_EQ(runAotOut(dt("var v: int := 40; ",
                                 "year := 2020, day := v, month := 1")),
                    "AOOR");
    BEZEL_EXPECT_EQ(runAotOut(dt("var v: int := 3; ",
                                 "year := 2020, day := v, month := 1")),
                    "ok\n3");
}

// Loop-100 throw/catch under the ASan/LSan oracle: the raise must not leak.
BEZEL_TEST(datetime_range_ctor_7819, throw_catch_loop_100_no_leak) {
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
        "      try "
        "        var z := new TimeOnly(1, 2, 3, 70000); "
        "        n := n + z.Millisecond; "
        "      catch var e3: ArgumentOutOfRangeException => "
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
    BEZEL_EXPECT_EQ(r.stdoutText, "300\n");
}
