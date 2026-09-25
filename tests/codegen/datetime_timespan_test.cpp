// Codegen for DateTime and TimeSpan (stdlib §2.5).
//
// DateTime and TimeSpan are builtin value types stored as scalar int64 tick
// counts (100-ns ticks since 0001-01-01 00:00:00 UTC, per .NET semantics).
// The Bezel standard library definition is in:
//   Bezel_Standard_Library_0_130.md §2.5
//
// Deferred members (with reasons):
//   - DateTime.Parse / TryParse: needs string parsing infra (would require a
//     full ISO-8601 / format-string parser — deferred, per CLAUDE.md §4).
//   - DateTime.ToString(format): needs format-string expansion engine.
//   - DateTime.Kind / ToUtc / ToLocal: Kind is not persisted in the scalar;
//     timezone conversion requires platform TZ APIs (deferred).
//   - TimeSpan * double operator: operator overload desugaring; deferred.
//
// Each test cites the stdlib section and the specific behaviour it verifies.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

std::string prog(const std::string& mainBody) {
    return "module M; "
           "class C public static method Main(): int "
           + mainBody +
           " end Main; end C;";
}

} // namespace

// ---------------------------------------------------------------------------
// §2.5 DateTime.Now / UtcNow — stable invariant tests.
// We test only properties that are always true: UtcNow ticks > 0 (the
// epoch), and (a second UtcNow call) - (first) >= 0.
// ---------------------------------------------------------------------------

// stdlib §2.5: UtcNow ticks must be > 0 (after the epoch 0001-01-01).
// The POSIX epoch is tick 621355968000000000; any current UTC time is larger.
// Migrated to `.Ticks` form per #4917 (DateTime declares no relational
// operators per stdlib §2.5, v0.307 — `.Ticks` returns long and is the
// canonical way to inspect a DateTime's tick count).
BEZEL_TEST(codegen_datetime_2_5, utcnow_ticks_positive) {
    BEZEL_EXPECT_EQ(run(prog(
        "var t := DateTime.UtcNow; "
        "if t.Ticks > 621355968000000000L then return 1; end; "
        "return 0; ")), 1);
}

// stdlib §2.5: two successive UtcNow calls are non-decreasing.
// Migrated to `.Ticks` form per #4917 (TimeSpan declares no relational
// operators — `(t2 - t1).Ticks` returns long).
BEZEL_TEST(codegen_datetime_2_5, utcnow_monotone) {
    BEZEL_EXPECT_EQ(run(prog(
        "var t1 := DateTime.UtcNow; "
        "var t2 := DateTime.UtcNow; "
        "if (t2 - t1).Ticks >= 0L then return 1; end; "
        "return 0; ")), 1);
}

// ---------------------------------------------------------------------------
// §2.5 DateTime constructor + Year/Month/Day properties.
// Known date: 2024-03-15 (a Friday).
// ---------------------------------------------------------------------------

// stdlib §2.5: new DateTime(year, month, day).Year == year.
BEZEL_TEST(codegen_datetime_2_5, ctor_year_2024) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "var dt := new DateTime(2024, 3, 15); "
        "Console.WriteLine(dt.Year); return 0; ")), "2024");
}

// stdlib §2.5: new DateTime(year, month, day).Month == month.
BEZEL_TEST(codegen_datetime_2_5, ctor_month_march) {
    BEZEL_EXPECT_EQ(run(prog(
        "var dt := new DateTime(2024, 3, 15); "
        "return dt.Month; ")), 3);
}

// stdlib §2.5: new DateTime(year, month, day).Day == day.
BEZEL_TEST(codegen_datetime_2_5, ctor_day_15) {
    BEZEL_EXPECT_EQ(run(prog(
        "var dt := new DateTime(2024, 3, 15); "
        "return dt.Day; ")), 15);
}

// stdlib §2.5: new DateTime(y,m,d,h,min,s).Hour == h.
BEZEL_TEST(codegen_datetime_2_5, ctor_hour) {
    BEZEL_EXPECT_EQ(run(prog(
        "var dt := new DateTime(2024, 3, 15, 13, 45, 30); "
        "return dt.Hour; ")), 13);
}

// stdlib §2.5: new DateTime(y,m,d,h,min,s).Minute == min.
BEZEL_TEST(codegen_datetime_2_5, ctor_minute) {
    BEZEL_EXPECT_EQ(run(prog(
        "var dt := new DateTime(2024, 3, 15, 13, 45, 30); "
        "return dt.Minute; ")), 45);
}

// stdlib §2.5: new DateTime(y,m,d,h,min,s).Second == sec.
BEZEL_TEST(codegen_datetime_2_5, ctor_second) {
    BEZEL_EXPECT_EQ(run(prog(
        "var dt := new DateTime(2024, 3, 15, 13, 45, 30); "
        "return dt.Second; ")), 30);
}

// stdlib §2.5: DayOfWeek for 2024-03-15 (Friday = 5).
// Per §2.5 DayOfWeek enum: Sunday=0, Monday=1, ..., Saturday=6.
BEZEL_TEST(codegen_datetime_2_5, ctor_day_of_week_friday) {
    BEZEL_EXPECT_EQ(run(prog(
        "var dt := new DateTime(2024, 3, 15); "
        "return dt.DayOfWeek; ")), 5);
}

// stdlib §2.5: DayOfYear for 2024-03-15.
// Jan=31, Feb=29 (2024 is a leap year), Mar 1-15=15, total=31+29+15=75.
BEZEL_TEST(codegen_datetime_2_5, ctor_day_of_year_75) {
    BEZEL_EXPECT_EQ(run(prog(
        "var dt := new DateTime(2024, 3, 15); "
        "return dt.DayOfYear; ")), 75);
}

// ---------------------------------------------------------------------------
// §2.5 Leap-year boundary: 2024-02-29 (2024 is a leap year).
// ---------------------------------------------------------------------------

// stdlib §2.5: 2024-02-29 is valid (2024 % 4 == 0, % 100 != 0 → leap).
// Year/Month/Day round-trip.
BEZEL_TEST(codegen_datetime_2_5, leap_year_feb29_round_trip) {
    BEZEL_EXPECT_EQ(run(prog(
        "var dt := new DateTime(2024, 2, 29); "
        "if dt.Year = 2024 and dt.Month = 2 and dt.Day = 29 then return 1; end; "
        "return 0; ")), 1);
}

// stdlib §2.5: AddYears on Feb 29 to a non-leap year clamps to Feb 28.
// 2024-02-29 + 1 year → 2025-02-28 (2025 is not a leap year).
BEZEL_TEST(codegen_datetime_2_5, add_years_feb29_to_non_leap) {
    BEZEL_EXPECT_EQ(run(prog(
        "var dt := new DateTime(2024, 2, 29); "
        "var dt2 := dt.AddYears(1); "
        "if dt2.Year = 2025 and dt2.Month = 2 and dt2.Day = 28 then return 1; end; "
        "return 0; ")), 1);
}

// ---------------------------------------------------------------------------
// §2.5 DateTime arithmetic: Add / AddDays / AddHours / AddSeconds.
// ---------------------------------------------------------------------------

// stdlib §2.5: DateTime.Add(TimeSpan) shifts the date by the span's ticks.
// 2024-01-01 + TimeSpan.FromDays(10) → 2024-01-11.
BEZEL_TEST(codegen_datetime_2_5, add_timespan_10_days) {
    BEZEL_EXPECT_EQ(run(prog(
        "var dt := new DateTime(2024, 1, 1); "
        "var span := TimeSpan.FromDays(10.0); "
        "var dt2 := dt.Add(span); "
        "return dt2.Day; ")), 11);
}

// stdlib §2.5: DateTime.AddDays(double).
// 2024-01-01 + 31 days → 2024-02-01.
BEZEL_TEST(codegen_datetime_2_5, add_days_31) {
    BEZEL_EXPECT_EQ(run(prog(
        "var dt := new DateTime(2024, 1, 1); "
        "var dt2 := dt.AddDays(31.0); "
        "if dt2.Year = 2024 and dt2.Month = 2 and dt2.Day = 1 then return 1; end; "
        "return 0; ")), 1);
}

// stdlib §2.5: DateTime.AddMonths — crosses year boundary.
// 2024-11-30 + 2 months → 2025-01-30.
BEZEL_TEST(codegen_datetime_2_5, add_months_crosses_year) {
    BEZEL_EXPECT_EQ(run(prog(
        "var dt := new DateTime(2024, 11, 30); "
        "var dt2 := dt.AddMonths(2); "
        "if dt2.Year = 2025 and dt2.Month = 1 and dt2.Day = 30 then return 1; end; "
        "return 0; ")), 1);
}

// stdlib §2.5: DateTime.AddHours.
// 2024-01-01 00:00:00 + 25 hours → 2024-01-02 01:00:00.
BEZEL_TEST(codegen_datetime_2_5, add_hours_cross_day) {
    BEZEL_EXPECT_EQ(run(prog(
        "var dt := new DateTime(2024, 1, 1, 0, 0, 0); "
        "var dt2 := dt.AddHours(25.0); "
        "if dt2.Day = 2 and dt2.Hour = 1 then return 1; end; "
        "return 0; ")), 1);
}

// stdlib §2.5: DateTime.AddSeconds.
// 2024-01-01 00:00:50 + 15 seconds → 2024-01-01 00:01:05.
BEZEL_TEST(codegen_datetime_2_5, add_seconds) {
    BEZEL_EXPECT_EQ(run(prog(
        "var dt := new DateTime(2024, 1, 1, 0, 0, 50); "
        "var dt2 := dt.AddSeconds(15.0); "
        "if dt2.Minute = 1 and dt2.Second = 5 then return 1; end; "
        "return 0; ")), 1);
}

// ---------------------------------------------------------------------------
// §2.5 DateTime - DateTime → TimeSpan (Subtract instance method).
// ---------------------------------------------------------------------------

// stdlib §2.5: (DateTime B).Subtract(DateTime A) = TimeSpan of the difference.
// 2024-01-11 - 2024-01-01 = 10 days.
BEZEL_TEST(codegen_datetime_2_5, subtract_datetime_yields_timespan) {
    BEZEL_EXPECT_EQ(run(prog(
        "var a := new DateTime(2024, 1, 1); "
        "var b := new DateTime(2024, 1, 11); "
        "var span := b.Subtract(a); "
        "return span.Days; ")), 10);
}

// stdlib §2.5: DateTime - TimeSpan → DateTime (Subtract with TimeSpan arg).
// 2024-01-11 - 10 days → 2024-01-01.
BEZEL_TEST(codegen_datetime_2_5, subtract_timespan_yields_datetime) {
    BEZEL_EXPECT_EQ(run(prog(
        "var b := new DateTime(2024, 1, 11); "
        "var span := TimeSpan.FromDays(10.0); "
        "var a := b.Subtract(span); "
        "return a.Day; ")), 1);
}

// ---------------------------------------------------------------------------
// §2.5 TimeSpan static factories and component round-trip.
// ---------------------------------------------------------------------------

// stdlib §2.5: TimeSpan.FromDays(d).TotalDays == d (within double precision).
// Use 1 day; TotalDays should be 1.0. We test by checking Days == 1.
BEZEL_TEST(codegen_timespan_2_5, from_days_component) {
    BEZEL_EXPECT_EQ(run(prog(
        "var ts := TimeSpan.FromDays(1.0); "
        "return ts.Days; ")), 1);
}

// stdlib §2.5: TimeSpan.FromHours(48).Days == 2, .Hours == 0.
BEZEL_TEST(codegen_timespan_2_5, from_hours_48_days_component) {
    BEZEL_EXPECT_EQ(run(prog(
        "var ts := TimeSpan.FromHours(48.0); "
        "return ts.Days; ")), 2);
}

// stdlib §2.5: TimeSpan.FromHours(1.5).Minutes == 30.
BEZEL_TEST(codegen_timespan_2_5, from_hours_fractional_minutes) {
    BEZEL_EXPECT_EQ(run(prog(
        "var ts := TimeSpan.FromHours(1.5); "
        "return ts.Minutes; ")), 30);
}

// stdlib §2.5: TimeSpan.FromMinutes(90).Hours == 1.
BEZEL_TEST(codegen_timespan_2_5, from_minutes_90_hours) {
    BEZEL_EXPECT_EQ(run(prog(
        "var ts := TimeSpan.FromMinutes(90.0); "
        "return ts.Hours; ")), 1);
}

// stdlib §2.5: TimeSpan.FromSeconds(3661) = 1h 1m 1s.
// Days=0, Hours=1, Minutes=1, Seconds=1.
BEZEL_TEST(codegen_timespan_2_5, from_seconds_3661_components) {
    BEZEL_EXPECT_EQ(run(prog(
        "var ts := TimeSpan.FromSeconds(3661.0); "
        "if ts.Hours = 1 and ts.Minutes = 1 and ts.Seconds = 1 then return 1; end; "
        "return 0; ")), 1);
}

// stdlib §2.5: TimeSpan.FromMilliseconds(1000) = 1 second.
BEZEL_TEST(codegen_timespan_2_5, from_milliseconds_1000_seconds) {
    BEZEL_EXPECT_EQ(run(prog(
        "var ts := TimeSpan.FromMilliseconds(1000.0); "
        "return ts.Seconds; ")), 1);
}

// stdlib §2.5: TimeSpan.Zero.Days == 0.
BEZEL_TEST(codegen_timespan_2_5, zero_days_is_zero) {
    BEZEL_EXPECT_EQ(run(prog(
        "var ts := TimeSpan.Zero; "
        "return ts.Days; ")), 0);
}

// ---------------------------------------------------------------------------
// §2.5 TimeSpan Total* properties (double).
// ---------------------------------------------------------------------------

// stdlib §2.5: TotalSeconds for a 2-minute span == 120.
// We test by converting to int and checking.
BEZEL_TEST(codegen_timespan_2_5, total_seconds_2min) {
    BEZEL_EXPECT_EQ(run(prog(
        "var ts := TimeSpan.FromMinutes(2.0); "
        "var total := int(ts.TotalSeconds); "
        "return total; ")), 120);
}

// stdlib §2.5: TotalMinutes for a 3-hour span == 180.
BEZEL_TEST(codegen_timespan_2_5, total_minutes_3hours) {
    BEZEL_EXPECT_EQ(run(prog(
        "var ts := TimeSpan.FromHours(3.0); "
        "var total := int(ts.TotalMinutes); "
        "return total; ")), 180);
}

// stdlib §2.5: TotalDays for a 48-hour span == 2.
BEZEL_TEST(codegen_timespan_2_5, total_days_48hours) {
    BEZEL_EXPECT_EQ(run(prog(
        "var ts := TimeSpan.FromHours(48.0); "
        "var total := int(ts.TotalDays); "
        "return total; ")), 2);
}

// ---------------------------------------------------------------------------
// §2.5 DateTime comparison (scalar i64 integer comparisons).
// ---------------------------------------------------------------------------

// stdlib §2.5: two DateTimes from the same components are equal (same ticks).
BEZEL_TEST(codegen_datetime_2_5, same_date_equals) {
    BEZEL_EXPECT_EQ(run(prog(
        "var a := new DateTime(2024, 6, 1); "
        "var b := new DateTime(2024, 6, 1); "
        "if a = b then return 1; end; "
        "return 0; ")), 1);
}

// stdlib §2.5: earlier date is less than later date.
BEZEL_TEST(codegen_datetime_2_5, earlier_less_than_later) {
    BEZEL_EXPECT_EQ(run(prog(
        "var a := new DateTime(2024, 1, 1); "
        "var b := new DateTime(2024, 6, 1); "
        "if a < b then return 1; end; "
        "return 0; ")), 1);
}

// ---------------------------------------------------------------------------
// §2.5 DateTime.MinValue / MaxValue static constants.
// ---------------------------------------------------------------------------

// stdlib §2.5: MinValue < Now (MinValue is the earliest representable tick).
// We test this as a relational comparison since MinValue.Year (year 1 CE)
// requires gmtime to handle a very large negative POSIX offset, which is
// platform-dependent. The comparison against UtcNow is always stable.
BEZEL_TEST(codegen_datetime_2_5, minvalue_less_than_now) {
    BEZEL_EXPECT_EQ(run(prog(
        "var minv := DateTime.MinValue; "
        "var now := DateTime.UtcNow; "
        "if minv < now then return 1; end; "
        "return 0; ")), 1);
}

// stdlib §2.5: MaxValue > MinValue.
BEZEL_TEST(codegen_datetime_2_5, maxvalue_greater_than_minvalue) {
    BEZEL_EXPECT_EQ(run(prog(
        "var minv := DateTime.MinValue; "
        "var maxv := DateTime.MaxValue; "
        "if maxv > minv then return 1; end; "
        "return 0; ")), 1);
}

// ===========================================================================
// #1026 / §2.5: a CHAINED-receiver DateTime instance method — calling the method
// directly on a non-identifier receiver (here a `new DateTime(...)` NewExpr, or
// any property/call result) rather than binding it to a local first.
//
// Root cause: tryLowerBuiltinClassCall (LlvmBackend_Calls.cpp) returned early at
// the static-call guard `if (!id) return nullptr` (which required an
// IdentifierExpr receiver to key the static `cls`-named handlers) BEFORE the
// DateTime instance handlers. A chained receiver is a MemberAccessExpr / NewExpr,
// so the instance call lowered to a no-op / wrong value (AddDays not applied).
// Fix: the guard no longer returns early — it gives the static handlers an empty
// `cls` (matching no class) for a non-identifier receiver so they fall through to
// the instance handlers. Same root cause as #1025 (cancellation).
// ===========================================================================

// #1026: chained `new DateTime(2024,3,15).AddDays(2).Day` == 17 (Mar 15 + 2).
// Before the fix this returned a wrong value (AddDays dropped on the chained
// NewExpr receiver). The bound form `var dt := …; dt.AddDays(2)` already worked.
BEZEL_TEST(codegen_datetime_1026, chained_new_add_days_applied) {
    BEZEL_EXPECT_EQ(run(prog(
        "var d := new DateTime(2024, 3, 15).AddDays(2); "
        "return d.Day; ")), 17);
}

// #1026: chained AddMonths on a NewExpr receiver — Mar + 2 months = May (5).
BEZEL_TEST(codegen_datetime_1026, chained_new_add_months_applied) {
    BEZEL_EXPECT_EQ(run(prog(
        "var d := new DateTime(2024, 3, 15).AddMonths(2); "
        "return d.Month; ")), 5);
}

// #1026 control: the bound form still produces the same result (regression guard
// that the guard change did not perturb the identifier-receiver path).
BEZEL_TEST(codegen_datetime_1026, bound_add_days_unchanged) {
    BEZEL_EXPECT_EQ(run(prog(
        "var dt := new DateTime(2024, 3, 15); "
        "var d := dt.AddDays(2); "
        "return d.Day; ")), 17);
}

// ===========================================================================
// #1114: DateTime.ToString() no-arg — ISO 8601 "yyyy-MM-ddTHH:mm:ss" (stdlib §2.5)
//
// The explicit no-arg call `d.ToString()` returned a null/empty bz_string
// (SIGSEGV on .Length), and `d.ToString().Length` returned 0.  Fix routes
// the no-arg case to bz_datetime_to_string_fmt with the ISO 8601 default format
// "yyyy-MM-ddTHH:mm:ss" — the same runtime helper ToString(format) uses.
// ===========================================================================

// #1114: bound no-arg ToString().Length returns 19 (was SIGSEGV/139).
BEZEL_TEST(datetime_noarg_tostring_iso_1114, bound_length_19) {
    BEZEL_EXPECT_EQ(run(prog(
        "var d := new DateTime(2024, 3, 15); "
        "var s := d.ToString(); "
        "return s.Length; ")), 19);
}

// #1114: inline no-arg ToString().Length returns 19 (was 0).
BEZEL_TEST(datetime_noarg_tostring_iso_1114, inline_length_19) {
    BEZEL_EXPECT_EQ(run(prog(
        "var d := new DateTime(2024, 3, 15); "
        "return d.ToString().Length; ")), 19);
}

// #1114: content check — char at index 10 is 'T' (code 84) in "2024-03-15T00:00:00".
BEZEL_TEST(datetime_noarg_tostring_iso_1114, content_T_at_index_10) {
    BEZEL_EXPECT_EQ(run(prog(
        "var d := new DateTime(2024, 3, 15); "
        "var s := d.ToString(); "
        "return s[10]; ")), 84);  // 'T' = 84
}

// #1114: no-arg agrees with explicit format ToString("yyyy-MM-ddTHH:mm:ss").
BEZEL_TEST(datetime_noarg_tostring_iso_1114, agrees_with_explicit_iso_format) {
    BEZEL_EXPECT_EQ(run(prog(
        "var d := new DateTime(2024, 3, 15); "
        "var a := d.ToString(); "
        "var b := d.ToString(\"yyyy-MM-ddTHH:mm:ss\"); "
        "return (if a = b then 1 else 0); ")), 1);
}

// #1114 regression: ToString(format) still works after the fix.
BEZEL_TEST(datetime_noarg_tostring_iso_1114, tostring_format_still_works) {
    BEZEL_EXPECT_EQ(run(prog(
        "var d := new DateTime(2024, 3, 15); "
        "var s := d.ToString(\"yyyy-MM-dd\"); "
        "return s.Length; ")), 10);
}
