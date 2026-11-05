// Pin tests for issue #7777: DateTime's Add*/Add/Subtract/`+`/`-` never
// raise OverflowException; `MaxValue.AddDays(1.0)` silently wraps.
//
// stdlib §2.5 (v0.355) states the contract, quoted verbatim from
// `docs/Bezel_Standard_Library.md`:
//
//   "The `Add*` family, `Add`, `Subtract(span: TimeSpan)`, and
//    `DateTime.FromTicks` raise `OverflowException` when the result falls
//    outside `MinValue..MaxValue`: the condition is a data-dependent overflow
//    of the tick domain -- language spec
//    [§31.4](Bezel_Language_Spec.md#sec-standard-exception-types)'s
//    `OverflowException` class, the fault `TimeSpan`'s arithmetic above
//    already raises -- a band-4 catchable fault, not a band-1 argument
//    defect; the constructors' component validation keeps
//    `ArgumentOutOfRangeException`."
//
//   "`DateTime.MinValue` is `0001-01-01T00:00:00` (`Ticks` 0) and `MaxValue`
//    is the last tick of `9999-12-31` (`Ticks` 3155378975999999999), so every
//    valid set of components names a value between them."
//
// and of the `+`/`-` operators with a TimeSpan ("Equality and `Kind`"):
//
//   "The `Add*` family, `Add`, `Subtract(span)` and the `+`/`-` operators with
//    a `TimeSpan` keep the `DateTime` operand's `Kind`; `Subtract(other)` and
//    `a - b` subtract the two `Ticks` with no normalization."
//
// The implementation added the ticks with a wrapping `add`/`sub` and no range
// test, so a result past MaxValue (or before MinValue) came back as an
// arbitrary date. MEASURED on r30-c1 (this lane's base):
//
//   DateTime.MaxValue.AddDays(1.0)          -> 10000-01-01T23:59:59
//   DateTime.MinValue.AddSeconds(-1.0)      -> 14614-11-08T13:24:01
//   new DateTime(2024,1,1).AddYears(10000)  -> 12024-01-01T00:00:00
//
// Every row below is on the CHECKED (ASan/LSan) AOT oracle and pins the
// program's exact stdout. Each catch arm prints a DISTINCT marker, so a row
// that prints "OVF" proves OverflowException itself was raised.
//
// Cite: stdlib §2.5, v0.355; spec §31.4 (OverflowException), v0.355.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::runAotChecked;

namespace {

std::string prog(const std::string& mainBody) {
    return "module M; "
           "class C public static method Main(): int "
           + mainBody +
           " return 0; end Main; end C;";
}

// The catch arms: a DISTINCT marker per exception type, so "OVF" proves
// OverflowException was raised and "OTHER" proves some other fault was.
const char* kCatch =
    "catch OverflowException => Console.WriteLine(\"OVF\"); "
    "catch Exception => Console.WriteLine(\"OTHER\"); end; ";

// runAotChecked + the VALUE channel: the program's stdout with ONE trailing
// line-ending stripped (runAotOut's rule), or a sentinel when the program was
// rejected / crashed / tripped the sanitizer.
std::string checkedOut(const std::string& src) {
    auto r = runAotChecked(src);
    if (!r.ran) return "<did-not-run>";
    std::string out = r.stdoutText;
    if (!out.empty() && out.back() == '\n') {
        out.pop_back();
        if (!out.empty() && out.back() == '\r') out.pop_back();
    }
    return out;
}

// An expression inside a try, printing either the exception marker or the
// expression's text.
std::string raising(const std::string& expr) {
    return "try var r := " + expr + "; "
           "Console.WriteLine($\"no raise {r}\"); " + kCatch;
}

}  // namespace

// ---------------------------------------------------------------------------
// The issue's own repros.
// ---------------------------------------------------------------------------

// Issue #7777's repro: `DateTime.MaxValue.AddDays(1.0)` -- one day past
// MaxValue -- used to print 10000-01-01T23:59:59.
BEZEL_TEST(datetime_add_overflow_7777, maxvalue_add_days_raises_overflow) {
    BEZEL_EXPECT_EQ(checkedOut(prog(raising("DateTime.MaxValue.AddDays(1.0)"))),
                    "OVF");
}

// The other side of the domain: one second before MinValue.
BEZEL_TEST(datetime_add_overflow_7777, minvalue_add_seconds_raises_overflow) {
    BEZEL_EXPECT_EQ(
        checkedOut(prog(raising("DateTime.MinValue.AddSeconds(-1.0)"))), "OVF");
}

// The CALENDAR path: `AddYears(10000)` leaves the year domain 1..9999, so
// the result has no DateTime representation.
BEZEL_TEST(datetime_add_overflow_7777, add_years_past_max_raises_overflow) {
    BEZEL_EXPECT_EQ(
        checkedOut(prog(raising("new DateTime(2024, 1, 1).AddYears(10000)"))),
        "OVF");
}

// `AddMonths` past the domain, and `AddYears` before it (the year 0 does
// not exist).
BEZEL_TEST(datetime_add_overflow_7777, add_months_past_max_raises_overflow) {
    BEZEL_EXPECT_EQ(
        checkedOut(prog(raising("new DateTime(2024, 1, 1).AddMonths(120000)"))),
        "OVF");
}

BEZEL_TEST(datetime_add_overflow_7777, add_years_before_min_raises_overflow) {
    BEZEL_EXPECT_EQ(
        checkedOut(prog(raising("DateTime.MinValue.AddYears(-1)"))), "OVF");
}

// `Add(span)` and `Subtract(span)` -- the TimeSpan-carrying members.
BEZEL_TEST(datetime_add_overflow_7777, add_span_past_max_raises_overflow) {
    BEZEL_EXPECT_EQ(
        checkedOut(prog(raising("DateTime.MaxValue.Add(TimeSpan.FromTicks(1L))"))),
        "OVF");
}

BEZEL_TEST(datetime_add_overflow_7777, subtract_span_before_min_raises_overflow) {
    BEZEL_EXPECT_EQ(
        checkedOut(prog(raising("DateTime.MinValue.Subtract(TimeSpan.FromTicks(1L))"))),
        "OVF");
}

// The `+` / `-` OPERATOR spellings -- the operator form of `Add` /
// `Subtract(span)`.
BEZEL_TEST(datetime_add_overflow_7777, plus_operator_past_max_raises_overflow) {
    BEZEL_EXPECT_EQ(
        checkedOut(prog(raising("DateTime.MaxValue + TimeSpan.FromTicks(1L)"))),
        "OVF");
}

BEZEL_TEST(datetime_add_overflow_7777, minus_operator_before_min_raises_overflow) {
    BEZEL_EXPECT_EQ(
        checkedOut(prog(raising("DateTime.MinValue - TimeSpan.FromTicks(1L)"))),
        "OVF");
}

BEZEL_TEST(datetime_add_overflow_7777, timespan_plus_datetime_past_max_raises_overflow) {
    BEZEL_EXPECT_EQ(
        checkedOut(prog(raising("TimeSpan.FromTicks(1L) + DateTime.MaxValue"))),
        "OVF");
}

// A NaN offset: the offset is computed over `double`, and a NaN product is
// out of the tick domain like any other.
BEZEL_TEST(datetime_add_overflow_7777, add_days_nan_raises_overflow) {
    BEZEL_EXPECT_EQ(
        checkedOut(prog(raising("DateTime.MaxValue.AddDays(0.0 / 0.0)"))), "OVF");
}

// ---------------------------------------------------------------------------
// In-range controls: the boundary values and the exact results.
// ---------------------------------------------------------------------------

// The exact boundary: `MaxValue` with a zero offset is MaxValue, and
// `MinValue` with a zero offset is MinValue.
BEZEL_TEST(datetime_add_overflow_7777, maxvalue_with_zero_offset_is_maxvalue) {
    BEZEL_EXPECT_EQ(checkedOut(prog(
        "var m := DateTime.MaxValue; "
        "var z := TimeSpan.Zero; "
        "var a := m.AddDays(0.0); var b := m.Add(z); var c := m + z; "
        "Console.WriteLine($\"{a.Ticks} {b.Ticks} {c.Ticks}\"); ")),
        "3155378975999999999 3155378975999999999 3155378975999999999");
}

BEZEL_TEST(datetime_add_overflow_7777, minvalue_with_zero_offset_is_minvalue) {
    BEZEL_EXPECT_EQ(checkedOut(prog(
        "var m := DateTime.MinValue; "
        "var z := TimeSpan.Zero; "
        "var a := m.AddSeconds(0.0); var b := m.Subtract(z); var c := m - z; "
        "Console.WriteLine($\"{a.Ticks} {b.Ticks} {c.Ticks}\"); ")),
        "0 0 0");
}

// The clock-unit members, in range: `AddDays(1.5)` is a day and a half.
BEZEL_TEST(datetime_add_overflow_7777, add_days_in_range_is_exact) {
    BEZEL_EXPECT_EQ(checkedOut(prog(
        "Console.WriteLine(new DateTime(2024, 1, 1).AddDays(1.5)); "
        "Console.WriteLine(new DateTime(2024, 1, 1).AddHours(2.0)); "
        "Console.WriteLine(new DateTime(2024, 1, 1).AddMinutes(30.0)); "
        "Console.WriteLine(new DateTime(2024, 1, 1).AddSeconds(45.0)); "
        "Console.WriteLine(new DateTime(2024, 1, 1).AddMilliseconds(250.0).Millisecond); ")),
        "2024-01-02T12:00:00\n2024-01-01T02:00:00\n2024-01-01T00:30:00\n"
        "2024-01-01T00:00:45\n250");
}

// The calendar members, in range: Feb 29 clamps to Feb 28 in a non-leap
// year, and Jan 31 clamps to Feb 29 in a leap year.
BEZEL_TEST(datetime_add_overflow_7777, add_years_and_months_in_range_are_exact) {
    BEZEL_EXPECT_EQ(checkedOut(prog(
        "Console.WriteLine(new DateTime(2024, 2, 29).AddYears(1)); "
        "Console.WriteLine(new DateTime(2024, 1, 31).AddMonths(1)); "
        "Console.WriteLine(new DateTime(2024, 1, 31).AddMonths(13)); ")),
        "2025-02-28T00:00:00\n2024-02-29T00:00:00\n2025-02-28T00:00:00");
}

// The TimeSpan-carrying members, in range.
BEZEL_TEST(datetime_add_overflow_7777, add_and_subtract_span_in_range_are_exact) {
    BEZEL_EXPECT_EQ(checkedOut(prog(
        "Console.WriteLine(new DateTime(2024, 1, 1).Add(TimeSpan.FromDays(10.0))); "
        "Console.WriteLine(new DateTime(2024, 1, 11).Subtract(TimeSpan.FromDays(10.0))); ")),
        "2024-01-11T00:00:00\n2024-01-01T00:00:00");
}

// The operator spellings, in range -- the positive control for the two
// operator raise rows above.
BEZEL_TEST(datetime_add_overflow_7777, operators_in_range_are_exact) {
    BEZEL_EXPECT_EQ(checkedOut(prog(
        "Console.WriteLine(new DateTime(2024, 1, 1) + TimeSpan.FromDays(10.0)); "
        "Console.WriteLine(new DateTime(2024, 1, 11) - TimeSpan.FromDays(10.0)); "
        "Console.WriteLine(TimeSpan.FromDays(10.0) + new DateTime(2024, 1, 1)); ")),
        "2024-01-11T00:00:00\n2024-01-01T00:00:00\n2024-01-11T00:00:00");
}
