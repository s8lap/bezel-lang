// Regression tests for the DayOfWeek half of #7536: `DateTime.DayOfWeek` was
// typed `int`, but stdlib §2.5 (v0.350) declares
//
//   public readonly property DayOfWeek: DayOfWeek;
//
// with `public enum DayOfWeek: byte  Sunday, Monday, Tuesday, Wednesday,
// Thursday, Friday, Saturday;`.  The property is the ENUM, with Sunday = 0
// (spec §10.2 auto-increment) -- the value bz_datetime_day_of_week returns.
//
// Fixed here: the property's static type (ExprTypecheck_Calls.cpp, the
// DateTime property block) is the §2.5 enum the Driver prelude injects
// (kTimePreludeSource, see datetime_enums_prelude_7503_test.cpp), the property
// lowers at the enum's i8 width (LlvmBackend_Exprs.cpp), and the typing also
// reaches a DateTime produced by a CALL (`d.AddDays(3).DayOfWeek`), whose
// TypeRef carries the builtin DateTime symbol rather than a null declaration.
//
// NOT fixed (stays #7536 / #7501): `DateTime.Kind` and the constructor's
// `kind` argument -- carrying a kind needs a layout change on DateTime.
//
// THE ORACLE.  Exact stdout for known calendar dates (2024-03-15 was a Friday,
// 2024-03-17 a Sunday, 2000-01-01 a Saturday, 2024-03-18 a Monday).  Before the
// fix `var w: DayOfWeek := dt.DayOfWeek;` is "cannot convert int to
// DayOfWeek", so every row fails to compile and `ran` is false.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

std::string prog(const std::string& decls, const std::string& body) {
    return "module M; " + decls +
           " class C public static method Main(): int " + body +
           " return 0; end Main; end C;";
}

}  // namespace

// §2.5 (v0.350): the property's type is the enum -- it binds to a `DayOfWeek`
// variable and compares with the enum's constants.
BEZEL_TEST(datetime_dayofweek_enum_7536, stdlib_2_5_property_is_the_enum_friday) {
    auto r = runAot(prog("",
        "var dt := new DateTime(2024, 3, 15); "
        "var w: DayOfWeek := dt.DayOfWeek; "
        "Console.WriteLine(w = DayOfWeek.Friday); "
        "Console.WriteLine(w = DayOfWeek.Monday); "
        "Console.WriteLine(dt.DayOfWeek = DayOfWeek.Friday); "
        "Console.WriteLine(int(byte(w))); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\nFalse\nTrue\n5\n");
}

// §2.5 / §10.2 (v0.350): Sunday = 0 ... Saturday = 6 for known dates, read
// through the enum (a `case` over the property is exhaustive without `_`).
BEZEL_TEST(datetime_dayofweek_enum_7536, stdlib_2_5_sunday_is_zero_known_dates) {
    auto r = runAot(
        "module M; "
        "class C "
        "  public static method Name(d: DayOfWeek): string "
        "    return (case d of "
        "      DayOfWeek.Sunday => \"Sun\", DayOfWeek.Monday => \"Mon\", "
        "      DayOfWeek.Tuesday => \"Tue\", DayOfWeek.Wednesday => \"Wed\", "
        "      DayOfWeek.Thursday => \"Thu\", DayOfWeek.Friday => \"Fri\", "
        "      DayOfWeek.Saturday => \"Sat\"); "
        "  end Name; "
        "  public static method Main(): int "
        "    var sun := new DateTime(2024, 3, 17); "
        "    var mon := new DateTime(2024, 3, 18); "
        "    var sat := new DateTime(2000, 1, 1); "
        "    Console.WriteLine(Name(sun.DayOfWeek)); "
        "    Console.WriteLine(int(byte(sun.DayOfWeek))); "
        "    Console.WriteLine(Name(mon.DayOfWeek)); "
        "    Console.WriteLine(int(byte(mon.DayOfWeek))); "
        "    Console.WriteLine(Name(sat.DayOfWeek)); "
        "    Console.WriteLine(int(byte(sat.DayOfWeek))); "
        "    Console.WriteLine(Name(sun.AddDays(3).DayOfWeek)); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "Sun\n0\nMon\n1\nSat\n6\nWed\n");
}

// §2.5 (v0.350): the property is passable to a `DayOfWeek` parameter and
// usable in an `if` (the common "is it a weekend" shape).
BEZEL_TEST(datetime_dayofweek_enum_7536, stdlib_2_5_property_as_argument_and_condition) {
    auto r = runAot(
        "module M; "
        "class C "
        "  public static method IsWeekend(d: DayOfWeek): bool "
        "    return d = DayOfWeek.Saturday or d = DayOfWeek.Sunday; "
        "  end IsWeekend; "
        "  public static method Main(): int "
        "    var fri := new DateTime(2024, 3, 15); "
        "    var sat := new DateTime(2024, 3, 16); "
        "    Console.WriteLine(IsWeekend(fri.DayOfWeek)); "
        "    Console.WriteLine(IsWeekend(sat.DayOfWeek)); "
        "    if sat.DayOfWeek = DayOfWeek.Saturday then "
        "      Console.WriteLine(\"saturday\"); "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "False\nTrue\nsaturday\n");
}

// Control, not spec-pinned: a user top-level `enum DayOfWeek` shadows the
// prelude (§2.3), so no stdlib enum exists in the module and the property
// keeps the ordinal as an `int`.  This row only proves the shadowed program
// still compiles and reads the right ordinal (Friday = 5); it does NOT claim
// `int` is the spec's type there (stdlib's DayOfWeek stays unnameable in that
// module -- reported as a sibling gap).
BEZEL_TEST(datetime_dayofweek_enum_7536, control_user_dayofweek_shadow_keeps_ordinal) {
    auto r = runAot(prog(
        "enum DayOfWeek Mine, Other; end DayOfWeek; ",
        "var dt := new DateTime(2024, 3, 15); "
        "var n: int := dt.DayOfWeek; "
        "Console.WriteLine(n); "
        "Console.WriteLine(int(byte(DayOfWeek.Other))); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "5\n1\n");
}

// §2.5 (v0.350), the CHAINED form: the receiver is a DateTime produced by a call,
// and the property is read directly as a call ARGUMENT to a `DayOfWeek`
// parameter.  Before the fix the member access stayed untyped and the i32
// helper result was passed to the i8 parameter ("Call parameter type does not
// match function signature", IR verification failed).  2024-03-17 + 3 days is
// a Wednesday; 2024-03-17 + 100 days (100 mod 7 = 2) is a Tuesday (ordinal 2).
BEZEL_TEST(datetime_dayofweek_enum_7536, stdlib_2_5_chained_receiver_as_argument) {
    auto r = runAot(
        "module M; "
        "class C "
        "  public static method IsMid(d: DayOfWeek): bool "
        "    return d = DayOfWeek.Wednesday; "
        "  end IsMid; "
        "  public static method Main(): int "
        "    var sun := new DateTime(2024, 3, 17); "
        "    Console.WriteLine(IsMid(sun.AddDays(3).DayOfWeek)); "
        "    Console.WriteLine(IsMid(sun.AddDays(4).DayOfWeek)); "
        "    var later := sun.AddDays(100); "
        "    Console.WriteLine(IsMid(later.DayOfWeek)); "
        "    Console.WriteLine(int(byte(later.DayOfWeek))); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\nFalse\nFalse\n2\n");
}

// §2.5 (v0.350): the chained receiver's OTHER component properties are typed
// `int` too, so a type-mismatched use is rejected rather than silently
// accepted (the chained DateTime property block was unreachable before).
BEZEL_TEST(datetime_dayofweek_enum_7536, stdlib_2_5_chained_receiver_int_property_is_typed) {
    auto r = runAot(prog("",
        "var sun := new DateTime(2024, 3, 17); "
        "var s: string := sun.AddDays(3).Year; "));
    // `!r.ran` alone is ambiguous (crash / signal / timeout also report it);
    // pin a genuine rejection with a real location and the specific text.
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "not implicitly convertible to the target type"));
}

// §2.5 (v0.350): the property stored into a class FIELD of type
// DayOfWeek, and into a `DayOfWeek?`.
BEZEL_TEST(datetime_dayofweek_enum_7536, stdlib_2_5_property_stored_into_field) {
    auto r = runAot(
        "module M; "
        "class Holder "
        "  public Day: DayOfWeek; "
        "  public Last: DayOfWeek := DayOfWeek.Sunday; "
        "  public constructor(d: DayOfWeek) Day := d; end constructor; "
        "end Holder; "
        "class C "
        "  public static method Main(): int "
        "    var fri := new DateTime(2024, 3, 15); "
        "    var h := new Holder(fri.DayOfWeek); "
        "    var h2 := new Holder(fri.AddDays(2).DayOfWeek); "
        "    h2.Day := fri.AddDays(1).DayOfWeek; "
        "    h.Last := fri.AddDays(3).DayOfWeek; "
        "    var opt: DayOfWeek? := fri.DayOfWeek; "
        "    Console.WriteLine(h.Day = DayOfWeek.Friday); "
        "    Console.WriteLine(h2.Day = DayOfWeek.Saturday); "
        "    Console.WriteLine(opt = DayOfWeek.Friday); "
        "    Console.WriteLine(int(byte(h.Day))); "
        "    Console.WriteLine(h.Last = DayOfWeek.Monday); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "True\nTrue\nTrue\n5\nTrue\n");
}

// §2.5 (v0.350): `case` directly over the property (no intermediate), and the
// read repeated 200 times over a sweep of consecutive days: every weekday
// ordinal must come out 0..6 in order (a wrong-width load or an i32 leak into
// the i8 slot would corrupt neighbours across iterations).
BEZEL_TEST(datetime_dayofweek_enum_7536, stdlib_2_5_case_over_property_loop_sweep) {
    auto r = runAot(
        "module M; "
        "class C "
        "  public static method Main(): int "
        "    var start := new DateTime(2024, 3, 17); "
        "    var sum := 0; "
        "    var sundays := 0; "
        "    var i := 0; "
        "    while i < 200 do "
        "      var d := start.AddDays(i); "
        "      var tag := (case d.DayOfWeek of "
        "        DayOfWeek.Sunday => 1, DayOfWeek.Monday => 0, "
        "        DayOfWeek.Tuesday => 0, DayOfWeek.Wednesday => 0, "
        "        DayOfWeek.Thursday => 0, DayOfWeek.Friday => 0, "
        "        DayOfWeek.Saturday => 0); "
        "      sundays := sundays + tag; "
        "      sum := sum + int(byte(d.DayOfWeek)); "
        "      if int(byte(d.DayOfWeek)) <> (i mod 7) then return 99; end; "
        "      i := i + 1; "
        "    end; "
        "    Console.WriteLine(sundays); "
        "    Console.WriteLine(sum); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // 200 days from a Sunday: ceil(200/7) = 29 Sundays; sum = 28 full weeks
    // (28 * 21) + the last 4 days (0+1+2+3) = 588 + 6 = 594.
    BEZEL_EXPECT_EQ(r.stdoutText, "29\n594\n");
}
