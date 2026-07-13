// Codegen tests for stdlib wave-1 bug fixes: #648, #649, #652.
//
// #648 — DateTime.Parse(s: string): DateTime and
//         DateTime.TryParse(s: string, out result: DateTime): bool
//         DateTime.ToString(format: string): string
//         (stdlib §2.5)
//
// #649 — string.IndexOf(value: string, start: int): int — 2-arg overload
//         ignoring the start offset before the fix.
//         (stdlib §9.6)
//
// #652 — Domain.Name: string property returning garbage before the fix.
//         (stdlib §6.8)
//
// All tests use the JIT harness (runJit) and return int exit codes.  String
// comparisons are expressed as `(1 if s = "expected" else 0)` so the program
// returns 1 on success and 0 on failure.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
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
// §2.5 / #648 — DateTime.Parse round-trip: parse then read year/month/day.
// We parse "2024-03-15" and check that Year/Month/Day round-trip correctly.
// ---------------------------------------------------------------------------

// stdlib §2.5: Parse("yyyy-MM-dd").Year == 2024.
BEZEL_TEST(codegen_stdlib_648_datetime_parse, parse_date_year) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "var dt := DateTime.Parse(\"2024-03-15\"); "
        "Console.WriteLine(dt.Year); return 0; ")), "2024");
}

// stdlib §2.5: Parse("yyyy-MM-dd").Month == 3.
BEZEL_TEST(codegen_stdlib_648_datetime_parse, parse_date_month) {
    BEZEL_EXPECT_EQ(run(prog(
        "var dt := DateTime.Parse(\"2024-03-15\"); "
        "return dt.Month; ")), 3);
}

// stdlib §2.5: Parse("yyyy-MM-dd").Day == 15.
BEZEL_TEST(codegen_stdlib_648_datetime_parse, parse_date_day) {
    BEZEL_EXPECT_EQ(run(prog(
        "var dt := DateTime.Parse(\"2024-03-15\"); "
        "return dt.Day; ")), 15);
}

// stdlib §2.5: Parse("yyyy-MM-ddTHH:mm:ss").Hour == 13.
BEZEL_TEST(codegen_stdlib_648_datetime_parse, parse_datetime_hour) {
    BEZEL_EXPECT_EQ(run(prog(
        "var dt := DateTime.Parse(\"2024-03-15T13:45:30\"); "
        "return dt.Hour; ")), 13);
}

// stdlib §2.5: Parse("yyyy-MM-ddTHH:mm:ss").Minute == 45.
BEZEL_TEST(codegen_stdlib_648_datetime_parse, parse_datetime_minute) {
    BEZEL_EXPECT_EQ(run(prog(
        "var dt := DateTime.Parse(\"2024-03-15T13:45:30\"); "
        "return dt.Minute; ")), 45);
}

// stdlib §2.5: Parse("yyyy-MM-ddTHH:mm:ss").Second == 30.
BEZEL_TEST(codegen_stdlib_648_datetime_parse, parse_datetime_second) {
    BEZEL_EXPECT_EQ(run(prog(
        "var dt := DateTime.Parse(\"2024-03-15T13:45:30\"); "
        "return dt.Second; ")), 30);
}

// stdlib §2.5: Parse with space separator ("yyyy-MM-dd HH:mm:ss") — year.
BEZEL_TEST(codegen_stdlib_648_datetime_parse, parse_space_sep_year) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "var dt := DateTime.Parse(\"2024-03-15 13:45:30\"); "
        "Console.WriteLine(dt.Year); return 0; ")), "2024");
}

// stdlib §2.5: Parse with space separator — hour.
BEZEL_TEST(codegen_stdlib_648_datetime_parse, parse_space_sep_hour) {
    BEZEL_EXPECT_EQ(run(prog(
        "var dt := DateTime.Parse(\"2024-03-15 13:45:30\"); "
        "return dt.Hour; ")), 13);
}

// stdlib §2.5: Parse("yyyy-MM-ddTHH:mm:ss.fff") with milliseconds — year.
BEZEL_TEST(codegen_stdlib_648_datetime_parse, parse_ms_year) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "var dt := DateTime.Parse(\"2024-03-15T13:45:30.123\"); "
        "Console.WriteLine(dt.Year); return 0; ")), "2024");
}

// stdlib §2.5: Parse("yyyy-MM-ddTHH:mm:ss.fff") — Millisecond == 123.
BEZEL_TEST(codegen_stdlib_648_datetime_parse, parse_ms_millisecond) {
    BEZEL_EXPECT_EQ(run(prog(
        "var dt := DateTime.Parse(\"2024-03-15T13:45:30.123\"); "
        "return dt.Millisecond; ")), 123);
}

// stdlib §2.5: Construct via ctor then round-trip via ToString("yyyy-MM-dd").
// The formatted output must equal the original date string.
BEZEL_TEST(codegen_stdlib_648_datetime_tostring, format_yyyy_MM_dd) {
    BEZEL_EXPECT_EQ(run(prog(
        "var dt := new DateTime(2024, 3, 15); "
        "var s := dt.ToString(\"yyyy-MM-dd\"); "
        "return (1 if s = \"2024-03-15\" else 0); ")), 1);
}

// stdlib §2.5: ToString("yyyy-MM-ddTHH:mm:ss") — full ISO-8601.
BEZEL_TEST(codegen_stdlib_648_datetime_tostring, format_iso8601) {
    BEZEL_EXPECT_EQ(run(prog(
        "var dt := new DateTime(2024, 3, 15, 13, 45, 30); "
        "var s := dt.ToString(\"yyyy-MM-ddTHH:mm:ss\"); "
        "return (1 if s = \"2024-03-15T13:45:30\" else 0); ")), 1);
}

// stdlib §2.5: ToString("yyyy") — year only.
BEZEL_TEST(codegen_stdlib_648_datetime_tostring, format_year_only) {
    BEZEL_EXPECT_EQ(run(prog(
        "var dt := new DateTime(2024, 3, 15); "
        "var s := dt.ToString(\"yyyy\"); "
        "return (1 if s = \"2024\" else 0); ")), 1);
}

// stdlib §2.5: ToString("MM/dd/yyyy") — US date format (/ emitted verbatim).
BEZEL_TEST(codegen_stdlib_648_datetime_tostring, format_us_date) {
    BEZEL_EXPECT_EQ(run(prog(
        "var dt := new DateTime(2024, 3, 15); "
        "var s := dt.ToString(\"MM/dd/yyyy\"); "
        "return (1 if s = \"03/15/2024\" else 0); ")), 1);
}

// stdlib §2.5: ToString("HH:mm:ss") — time only.
BEZEL_TEST(codegen_stdlib_648_datetime_tostring, format_time_only) {
    BEZEL_EXPECT_EQ(run(prog(
        "var dt := new DateTime(2024, 3, 15, 13, 45, 30); "
        "var s := dt.ToString(\"HH:mm:ss\"); "
        "return (1 if s = \"13:45:30\" else 0); ")), 1);
}

// stdlib §2.5: Parse round-trip — Parse(ToString("yyyy-MM-dd")).Year.
// Construct, format, parse back and check Year survives the round-trip.
BEZEL_TEST(codegen_stdlib_648_datetime_tostring, round_trip_year) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "var dt := new DateTime(2024, 3, 15); "
        "var s := dt.ToString(\"yyyy-MM-dd\"); "
        "var dt2 := DateTime.Parse(s); "
        "Console.WriteLine(dt2.Year); return 0; ")), "2024");
}

// stdlib §2.5: Parse round-trip — Month survives.
BEZEL_TEST(codegen_stdlib_648_datetime_tostring, round_trip_month) {
    BEZEL_EXPECT_EQ(run(prog(
        "var dt := new DateTime(2024, 3, 15); "
        "var s := dt.ToString(\"yyyy-MM-dd\"); "
        "var dt2 := DateTime.Parse(s); "
        "return dt2.Month; ")), 3);
}

// stdlib §2.5: Parse round-trip — Day survives.
BEZEL_TEST(codegen_stdlib_648_datetime_tostring, round_trip_day) {
    BEZEL_EXPECT_EQ(run(prog(
        "var dt := new DateTime(2024, 3, 15); "
        "var s := dt.ToString(\"yyyy-MM-dd\"); "
        "var dt2 := DateTime.Parse(s); "
        "return dt2.Day; ")), 15);
}

// stdlib §2.5: TryParse valid input → returns true (1).
BEZEL_TEST(codegen_stdlib_648_datetime_tryparse, valid_input_returns_true) {
    BEZEL_EXPECT_EQ(run(prog(
        "var result: DateTime; "
        "var ok := DateTime.TryParse(\"2024-03-15\", out result); "
        "return (1 if ok else 0); ")), 1);
}

// stdlib §2.5: TryParse valid input → out param Year == 2024.
BEZEL_TEST(codegen_stdlib_648_datetime_tryparse, valid_input_year) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "var result: DateTime; "
        "var ok := DateTime.TryParse(\"2024-03-15\", out result); "
        "if ok then Console.WriteLine(result.Year); return 0; end; "
        "return 0; ")), "2024");
}

// stdlib §2.5: TryParse invalid input → returns false (0).
BEZEL_TEST(codegen_stdlib_648_datetime_tryparse, invalid_input_returns_false) {
    BEZEL_EXPECT_EQ(run(prog(
        "var result: DateTime; "
        "var ok := DateTime.TryParse(\"not-a-date\", out result); "
        "return (0 if ok else 1); ")), 1);
}

// ---------------------------------------------------------------------------
// §9.6 / #649 — string.IndexOf(value: string, start: int): int
// The 2-arg overload must respect the start offset.
// ---------------------------------------------------------------------------

// §9.6: IndexOf(sub, 0) with start=0 behaves like 1-arg IndexOf — finds first.
BEZEL_TEST(codegen_stdlib_649_indexof_start, start_zero_finds_first) {
    BEZEL_EXPECT_EQ(run(prog(
        "var s := \"abcabc\"; "
        "return s.IndexOf(\"a\", 0); ")), 0);
}

// §9.6: IndexOf(sub, 1) skips the match at position 0, finds match at 3.
BEZEL_TEST(codegen_stdlib_649_indexof_start, start_one_skips_first) {
    BEZEL_EXPECT_EQ(run(prog(
        "var s := \"abcabc\"; "
        "return s.IndexOf(\"a\", 1); ")), 3);
}

// §9.6: IndexOf(sub, start) where start == exact position of the only match.
BEZEL_TEST(codegen_stdlib_649_indexof_start, start_at_match) {
    BEZEL_EXPECT_EQ(run(prog(
        "var s := \"Hello World\"; "
        "return s.IndexOf(\"World\", 6); ")), 6);
}

// §9.6: IndexOf(sub, start) where start is one past the match — returns -1.
BEZEL_TEST(codegen_stdlib_649_indexof_start, start_past_last_match) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "var s := \"Hello World\"; "
        "Console.WriteLine(s.IndexOf(\"World\", 7)); return 0; ")), "-1");
}

// §9.6: IndexOf(sub, start) where start equals string length — returns -1.
BEZEL_TEST(codegen_stdlib_649_indexof_start, start_at_len_returns_neg1) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "var s := \"abc\"; "
        "Console.WriteLine(s.IndexOf(\"a\", 3)); return 0; ")), "-1");
}

// §9.6: IndexOf(empty, start) where needle is empty — returns start (clamped).
BEZEL_TEST(codegen_stdlib_649_indexof_start, empty_needle_returns_start) {
    BEZEL_EXPECT_EQ(run(prog(
        "var s := \"abc\"; "
        "return s.IndexOf(\"\", 2); ")), 2);
}

// §9.6: IndexOf(sub, start) not found at all — returns -1.
BEZEL_TEST(codegen_stdlib_649_indexof_start, not_found_returns_neg1) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "var s := \"Hello World\"; "
        "Console.WriteLine(s.IndexOf(\"xyz\", 0)); return 0; ")), "-1");
}

// §9.6: IndexOf with multi-occurrence string, scan from second occurrence.
BEZEL_TEST(codegen_stdlib_649_indexof_start, third_occurrence) {
    BEZEL_EXPECT_EQ(run(prog(
        "var s := \"aaa\"; "
        "return s.IndexOf(\"a\", 2); ")), 2);
}

// §9.6: IndexOf(sub, start) — start skips all occurrences → -1.
BEZEL_TEST(codegen_stdlib_649_indexof_start, start_after_all_occurrences) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "var s := \"aaa\"; "
        "Console.WriteLine(s.IndexOf(\"a\", 3)); return 0; ")), "-1");
}

// ---------------------------------------------------------------------------
// §6.8 / #652 — Domain.Name: string property.
// The spec (§6.8) provides a static class Domain with Spawn methods.  The
// probe and matrix.tsv test the Name property on a newly constructed domain.
// ---------------------------------------------------------------------------

// §6.8: new Domain("test").Name equals "test".
BEZEL_TEST(codegen_stdlib_652_domain_name, name_matches_ctor_arg) {
    BEZEL_EXPECT_EQ(run(prog(
        "var d := new Domain(\"test\"); "
        "var n := d.Name; "
        "var ok := (1 if n = \"test\" else 0); "
        "d.Dispose(); "
        "return ok; ")), 1);
}

// §6.8: Domain Name length == length of the string passed to ctor.
BEZEL_TEST(codegen_stdlib_652_domain_name, name_length) {
    BEZEL_EXPECT_EQ(run(prog(
        "var d := new Domain(\"hello\"); "
        "var n := d.Name; "
        "var len := n.Length; "
        "d.Dispose(); "
        "return len; ")), 5);
}

// §6.8: Domain with empty-string name — Name returns "".
BEZEL_TEST(codegen_stdlib_652_domain_name, empty_name) {
    BEZEL_EXPECT_EQ(run(prog(
        "var d := new Domain(\"\"); "
        "var n := d.Name; "
        "var ok := (1 if n = \"\" else 0); "
        "d.Dispose(); "
        "return ok; ")), 1);
}

// §6.8: Domain name is a fresh string (not a dangling pointer to a literal).
// Verify by constructing two domains with different names and comparing both.
BEZEL_TEST(codegen_stdlib_652_domain_name, two_domains_distinct_names) {
    BEZEL_EXPECT_EQ(run(prog(
        "var d1 := new Domain(\"alpha\"); "
        "var d2 := new Domain(\"beta\"); "
        "var n1 := d1.Name; "
        "var n2 := d2.Name; "
        "var ok := (1 if n1 = \"alpha\" and n2 = \"beta\" else 0); "
        "d1.Dispose(); "
        "d2.Dispose(); "
        "return ok; ")), 1);
}
