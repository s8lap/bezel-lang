// Regression tests for #2240 — the stdlib §2.5 temporal `ToString(format)`
// surface (stdlib §2.5 / §2.18, v0.357; spec §18.5 "Interpolated strings").
//
// The v0.227 stdlib revision (the issue's §2.5 clarification) DECLARED the
// members the §2.18 format contract dispatches to:
//
//   public method ToString(format: string): string;   // DateOnly, TimeOnly
//   public method ToString(): string;                 // TimeSpan (the "c" form)
//   public method ToString(format: string): string;   // TimeSpan
//
// Before this fix the DIRECT member call `d.ToString("yyyy-MM-dd")` had no
// codegen arm for `DateOnly`/`TimeOnly`: the call fell through every arm to
// codegen's §8.1 gate and was REJECTED with
//
//   member `ToString` on `DateOnly` is declared via the universal Object
//   contract (language spec §8.1) but is not yet implemented by this compiler
//   for this receiver (tracked #2457)
//
// The rows below therefore cover three things:
//
//   1. the members stdlib §2.5 declares but the compiler did not implement
//      (`DateOnly.ToString(format)`, `TimeOnly.ToString(format)`);
//   2. the members stdlib §2.5 declares and the compiler DID implement
//      (`TimeSpan.ToString()`, `TimeSpan.ToString(format)`, and `DateTime`'s
//      pair) — pinned here so the surface cannot regress silently;
//   3. the §2.18 RENDERING of a temporal pattern, including the `TimeOnly`
//      millisecond field: §2.18's "Temporal field widths" paragraph says `fff`
//      renders "the millisecond component" as three zero-padded digits, and
//      `ss` renders the second component. Before this fix a `TimeOnly` with a
//      non-zero millisecond rendered "13:05:10.-993" through BOTH spellings,
//      while the type's own no-format `ToString()` rendered "13:05:09"
//      (stdlib §2.5's "HH:mm:ss") — the two could not both be right.
//
// §18.5's "Format dispatch" makes an interpolation hole a `ToString(format)`
// call, so the hole and the direct call must render the SAME string; one row
// below checks that they agree.
//
// Each program encodes pass/fail into its exit code, or (where the observable
// is a string) into its stdout, which `runAot` captures in `stdoutText`.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// Runs `prog` and returns its exit code (-999 when the program did not run).
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

// Runs `prog` and returns its captured stdout ("" when it did not run).
std::string out(const std::string& prog) {
    auto r = runAot(prog);
    if (!r.ran) return "<did-not-run>";
    return r.stdoutText;
}

// A program whose observable is a rendered string: the expected text is
// compared against the program's own stdout, so a fix that misreads the
// format argument (or renders the wrong field) cannot pass.
std::string render(const std::string& body) {
    return "module M; class C public static method Main() " + body +
           " end Main; end C;";
}

// A program whose observable is an exit code (a `Main(): int`).
std::string renderInt(const std::string& body) {
    return "module M; class C public static method Main(): int " + body +
           " end Main; end C;";
}

}  // namespace

// ── DateOnly.ToString(format) (stdlib §2.5 / §2.18) ──────────────────────────

// stdlib §2.5 declares `DateOnly.ToString(format: string): string` ("format
// vocabulary: §2.18"). The pattern `yyyy-MM-dd` renders the date's own
// components: year 2026, month 02, day 14 (§2.18's "Temporal field widths").
BEZEL_TEST(temporal_tostring_format_2240, dateonly_tostring_format) {
    BEZEL_EXPECT_EQ(out(render(
        "var d := new DateOnly(2026, 2, 14); "
        "Console.WriteLine(d.ToString(\"yyyy-MM-dd\"));"
    )), "2026-02-14\n");
}

// §2.18's field-width rule: each specifier renders a fixed-width, zero-padded
// field of its own letter count, and the non-specifier characters are
// literals copied verbatim.
BEZEL_TEST(temporal_tostring_format_2240, dateonly_tostring_format_fields) {
    BEZEL_EXPECT_EQ(out(render(
        "var d := new DateOnly(2026, 2, 14); "
        "Console.WriteLine(d.ToString(\"yyyy/MM/dd\")); "
        "Console.WriteLine(d.ToString(\"dd.MM.yyyy\")); "
        "Console.WriteLine(d.ToString(\"yyyyMMdd\"));"
    )), "2026/02/14\n14.02.2026\n20260214\n");
}

// §2.18's strict-fault: a malformed specifier run (a run of one of the
// specifier letters `y M d H m s f` whose length is not the field width)
// raises FormatException — it does not render.
BEZEL_TEST(temporal_tostring_format_2240, dateonly_tostring_format_malformed_run_faults) {
    BEZEL_EXPECT_EQ(run(renderInt(
        "var result := 0; "
        "var d := new DateOnly(2026, 2, 14); "
        "try "
        "  var s := d.ToString(\"yyy\"); "
        "  result := 99; "
        "catch FormatException => result := 7; "
        "end; "
        "return result;"
    )), 7);
}

// §18.5's "Format dispatch": the interpolation hole IS a `ToString(format)`
// call, so the two spellings must render the same string.
BEZEL_TEST(temporal_tostring_format_2240, dateonly_hole_and_call_agree) {
    BEZEL_EXPECT_EQ(out(render(
        "var d := new DateOnly(2026, 2, 14); "
        "Console.WriteLine(d.ToString(\"yyyy-MM-dd\")); "
        "Console.WriteLine($\"{d:yyyy-MM-dd}\");"
    )), "2026-02-14\n2026-02-14\n");
}

// ── TimeOnly.ToString(format) (stdlib §2.5 / §2.18) ──────────────────────────

// stdlib §2.5 declares `TimeOnly.ToString(format: string): string`. The
// pattern renders the time of day's own components — hour 13, minute 05,
// second 09 (§2.18's field-width rule).
BEZEL_TEST(temporal_tostring_format_2240, timeonly_tostring_format) {
    BEZEL_EXPECT_EQ(out(render(
        "var t := new TimeOnly(13, 5, 9, 7); "
        "Console.WriteLine(t.ToString(\"HH:mm:ss\"));"
    )), "13:05:09\n");
}

// §2.18's "Temporal field widths": `fff` renders "the millisecond
// component" — three zero-padded digits (007), not the borrowed
// second the pre-fix widening produced ("13:05:10.-993").
BEZEL_TEST(temporal_tostring_format_2240, timeonly_tostring_format_millisecond_field) {
    BEZEL_EXPECT_EQ(out(render(
        "var t := new TimeOnly(13, 5, 9, 7); "
        "Console.WriteLine(t.ToString(\"HH:mm:ss.fff\"));"
    )), "13:05:09.007\n");
}

// §2.5 declares the no-format `ToString()` as "HH:mm:ss" and the format
// spelling as the §2.18 vocabulary; for the "HH:mm:ss" pattern the two must
// agree (stdlib §2.5 / §2.18).
BEZEL_TEST(temporal_tostring_format_2240, timeonly_tostring_format_agrees_with_noformat) {
    BEZEL_EXPECT_EQ(out(render(
        "var t := new TimeOnly(13, 5, 9, 7); "
        "Console.WriteLine(t.ToString()); "
        "Console.WriteLine(t.ToString(\"HH:mm:ss\"));"
    )), "13:05:09\n13:05:09\n");
}

// §18.5's "Format dispatch": the hole and the direct call are the same call.
BEZEL_TEST(temporal_tostring_format_2240, timeonly_hole_and_call_agree) {
    BEZEL_EXPECT_EQ(out(render(
        "var t := new TimeOnly(13, 5, 9, 7); "
        "Console.WriteLine(t.ToString(\"HH:mm:ss.fff\")); "
        "Console.WriteLine($\"{t:HH:mm:ss.fff}\");"
    )), "13:05:09.007\n13:05:09.007\n");
}

// §2.18's strict-fault on a malformed specifier run.
BEZEL_TEST(temporal_tostring_format_2240, timeonly_tostring_format_malformed_run_faults) {
    BEZEL_EXPECT_EQ(run(renderInt(
        "var result := 0; "
        "var t := new TimeOnly(13, 5, 9, 7); "
        "try "
        "  var s := t.ToString(\"MMM\"); "
        "  result := 99; "
        "catch FormatException => result := 7; "
        "end; "
        "return result;"
    )), 7);
}

// ── TimeSpan.ToString() / TimeSpan.ToString(format) (stdlib §2.5 / §2.18) ───
// These two members stdlib §2.5 declares were ALREADY implemented; the rows
// below pin them so the surface cannot regress silently.

// §2.18's "The `TimeSpan` constant form": `c` renders
// `[-][d.]HH:mm:ss[.fffffff]`, and the no-format `ToString()` is that one
// rendering. `TimeSpan.FromMinutes(90.5)` renders "01:30:30".
BEZEL_TEST(temporal_tostring_format_2240, timespan_tostring_noformat_is_the_c_form) {
    BEZEL_EXPECT_EQ(out(render(
        "var ts := TimeSpan.FromMinutes(90.5); "
        "Console.WriteLine(ts.ToString()); "
        "Console.WriteLine(ts.ToString(\"c\"));"
    )), "01:30:30\n01:30:30\n");
}

// §2.18's constant form, part by part: the whole days appear when the
// magnitude is at least one day ("1.12:00:00"), the sub-second remainder is
// seven zero-padded digits when it is non-zero ("00:00:00.0015000"), and the
// sign is at most one ("-00:01:30").
BEZEL_TEST(temporal_tostring_format_2240, timespan_tostring_c_form_parts) {
    BEZEL_EXPECT_EQ(out(render(
        "Console.WriteLine(TimeSpan.FromDays(1.5).ToString(\"c\")); "
        "Console.WriteLine(TimeSpan.FromMilliseconds(1.5).ToString(\"c\")); "
        "Console.WriteLine((-TimeSpan.FromSeconds(90.0)).ToString(\"c\")); "
        "Console.WriteLine(TimeSpan.Zero.ToString(\"c\"));"
    )), "1.12:00:00\n00:00:00.0015000\n-00:01:30\n00:00:00\n");
}

// §2.18's constant form at the tick extreme: `MinValue` renders
// "-10675199.02:48:05.4775808".
BEZEL_TEST(temporal_tostring_format_2240, timespan_tostring_c_form_minvalue) {
    BEZEL_EXPECT_EQ(out(render(
        "Console.WriteLine(TimeSpan.MinValue.ToString());"
    )), "-10675199.02:48:05.4775808\n");
}

// §2.18's strict-fault: `c` is the only `TimeSpan` specifier, so any other
// raises FormatException.
BEZEL_TEST(temporal_tostring_format_2240, timespan_tostring_format_unknown_specifier_faults) {
    BEZEL_EXPECT_EQ(run(renderInt(
        "var result := 0; "
        "var ts := TimeSpan.FromSeconds(1.0); "
        "try "
        "  var s := ts.ToString(\"yyyy\"); "
        "  result := 99; "
        "catch FormatException => result := 7; "
        "end; "
        "return result;"
    )), 7);
}

// ── DateTime.ToString(format) (stdlib §2.5 / §2.18) ──────────────────────────
// Also already implemented; pinned here for the same reason as TimeSpan.

BEZEL_TEST(temporal_tostring_format_2240, datetime_tostring_format) {
    BEZEL_EXPECT_EQ(out(render(
        "var dt := new DateTime(2024, 3, 15, 1, 2, 3); "
        "Console.WriteLine(dt.ToString(\"yyyy-MM-dd HH:mm:ss\")); "
        "Console.WriteLine(dt.ToString(\"yyyy/MM/dd\"));"
    )), "2024-03-15 01:02:03\n2024/03/15\n");
}

// §2.5 declares the no-format `DateTime.ToString()` as the round-trippable
// ISO 8601 form "yyyy-MM-ddTHH:mm:ss".
BEZEL_TEST(temporal_tostring_format_2240, datetime_tostring_noformat_is_iso8601) {
    BEZEL_EXPECT_EQ(out(render(
        "var dt := new DateTime(2024, 3, 15, 1, 2, 3); "
        "Console.WriteLine(dt.ToString());"
    )), "2024-03-15T01:02:03\n");
}
