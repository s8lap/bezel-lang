// Codegen tests for DateOnly and TimeOnly — issue #1357 / stdlib §2.5 v0.165.
//
// DateOnly: constructor(year,month,day); properties Year/Month/Day; CompareTo;
//           ToString; ToDateTime(time); Parse; TryParse.
// TimeOnly: constructor(hour,minute,second[,ms]); properties Hour/Minute/Second/
//           Millisecond; CompareTo; ToString; Parse; TryParse.
// DateTime.Date → DateOnly; DateTime.TimeOfDay → TimeOnly.
//
// Note: Bezel uses `=` for equality (not `==`), `and`/`or`/`not` for boolean ops.
// Each test cites the stdlib section and specific behaviour it verifies.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

std::string prog(const std::string& body) {
    return "module M; class C public static method Main(): int " + body +
           " end Main; end C;";
}

}  // namespace

// ── DateOnly ──────────────────────────────────────────────────────────────────

// stdlib §2.5 / #1357: DateOnly constructor and Year property.
BEZEL_TEST(codegen_dateonly_timeonly_1357, dateonly_ctor_year) {
    BEZEL_EXPECT_EQ(run(prog(
        "var d := new DateOnly(2025, 6, 15); "
        "if d.Year = 2025 then return 1; end; "
        "return 0;"
    )), 1);
}

// stdlib §2.5 / #1357: DateOnly.Month property.
BEZEL_TEST(codegen_dateonly_timeonly_1357, dateonly_ctor_month) {
    BEZEL_EXPECT_EQ(run(prog(
        "var d := new DateOnly(2025, 6, 15); "
        "if d.Month = 6 then return 1; end; "
        "return 0;"
    )), 1);
}

// stdlib §2.5 / #1357: DateOnly.Day property.
BEZEL_TEST(codegen_dateonly_timeonly_1357, dateonly_ctor_day) {
    BEZEL_EXPECT_EQ(run(prog(
        "var d := new DateOnly(2025, 6, 15); "
        "if d.Day = 15 then return 1; end; "
        "return 0;"
    )), 1);
}

// stdlib §2.5 / #1357: DateOnly.CompareTo — equal dates → 0.
BEZEL_TEST(codegen_dateonly_timeonly_1357, dateonly_compareto_equal) {
    BEZEL_EXPECT_EQ(run(prog(
        "var a := new DateOnly(2025, 1, 1); "
        "var b := new DateOnly(2025, 1, 1); "
        "if a.CompareTo(b) = 0 then return 1; end; "
        "return 0;"
    )), 1);
}

// stdlib §2.5 / #1357: DateOnly.CompareTo — earlier < later → negative.
BEZEL_TEST(codegen_dateonly_timeonly_1357, dateonly_compareto_earlier_negative) {
    BEZEL_EXPECT_EQ(run(prog(
        "var a := new DateOnly(2024, 12, 31); "
        "var b := new DateOnly(2025, 1, 1); "
        "if a.CompareTo(b) < 0 then return 1; end; "
        "return 0;"
    )), 1);
}

// stdlib §2.5 / #1357: DateOnly.ToString — returns non-empty string.
BEZEL_TEST(codegen_dateonly_timeonly_1357, dateonly_tostring_nonempty) {
    BEZEL_EXPECT_EQ(run(prog(
        "var d := new DateOnly(2025, 6, 15); "
        "var s := d.ToString(); "
        "if s.Length > 0 then return 1; end; "
        "return 0;"
    )), 1);
}

// stdlib §2.5 / #1357: DateOnly.Parse — "2025-06-15" → correct Year.
BEZEL_TEST(codegen_dateonly_timeonly_1357, dateonly_parse_year) {
    BEZEL_EXPECT_EQ(run(prog(
        "var d := DateOnly.Parse(\"2025-06-15\"); "
        "if d.Year = 2025 then return 1; end; "
        "return 0;"
    )), 1);
}

// stdlib §2.5 / #1357: DateOnly.TryParse — valid input → true, correct month.
BEZEL_TEST(codegen_dateonly_timeonly_1357, dateonly_tryparse_valid) {
    BEZEL_EXPECT_EQ(run(prog(
        "var d: DateOnly; "
        "var ok := DateOnly.TryParse(\"2025-06-15\", out d); "
        "if ok and d.Month = 6 then return 1; end; "
        "return 0;"
    )), 1);
}

// stdlib §2.5 / #1357: DateOnly.TryParse — invalid → false.
BEZEL_TEST(codegen_dateonly_timeonly_1357, dateonly_tryparse_invalid) {
    BEZEL_EXPECT_EQ(run(prog(
        "var d: DateOnly; "
        "var ok := DateOnly.TryParse(\"not-a-date\", out d); "
        "if not ok then return 1; end; "
        "return 0;"
    )), 1);
}

// stdlib §2.5 / #1357: DateOnly.ToDateTime(time: TimeOnly) — merges into DateTime.
// 2025-01-15 + 14:30:00 → DateTime.Hour should be 14.
BEZEL_TEST(codegen_dateonly_timeonly_1357, dateonly_to_datetime_hour) {
    BEZEL_EXPECT_EQ(run(prog(
        "var d := new DateOnly(2025, 1, 15); "
        "var t := new TimeOnly(14, 30, 0); "
        "var dt := d.ToDateTime(t); "
        "if dt.Hour = 14 then return 1; end; "
        "return 0;"
    )), 1);
}

// ── TimeOnly ──────────────────────────────────────────────────────────────────

// stdlib §2.5 / #1357: TimeOnly constructor and Hour property.
BEZEL_TEST(codegen_dateonly_timeonly_1357, timeonly_ctor_hour) {
    BEZEL_EXPECT_EQ(run(prog(
        "var t := new TimeOnly(14, 30, 0); "
        "if t.Hour = 14 then return 1; end; "
        "return 0;"
    )), 1);
}

// stdlib §2.5 / #1357: TimeOnly.Minute property.
BEZEL_TEST(codegen_dateonly_timeonly_1357, timeonly_ctor_minute) {
    BEZEL_EXPECT_EQ(run(prog(
        "var t := new TimeOnly(14, 30, 0); "
        "if t.Minute = 30 then return 1; end; "
        "return 0;"
    )), 1);
}

// stdlib §2.5 / #1357: TimeOnly.Second property.
BEZEL_TEST(codegen_dateonly_timeonly_1357, timeonly_ctor_second) {
    BEZEL_EXPECT_EQ(run(prog(
        "var t := new TimeOnly(14, 30, 45); "
        "if t.Second = 45 then return 1; end; "
        "return 0;"
    )), 1);
}

// stdlib §2.5 / #1357: TimeOnly.Millisecond property.
BEZEL_TEST(codegen_dateonly_timeonly_1357, timeonly_ctor_millisecond) {
    BEZEL_EXPECT_EQ(run(prog(
        "var t := new TimeOnly(14, 30, 45, 123); "
        "if t.Millisecond = 123 then return 1; end; "
        "return 0;"
    )), 1);
}

// stdlib §2.5 / #1357: TimeOnly.CompareTo — equal → 0.
BEZEL_TEST(codegen_dateonly_timeonly_1357, timeonly_compareto_equal) {
    BEZEL_EXPECT_EQ(run(prog(
        "var a := new TimeOnly(12, 0, 0); "
        "var b := new TimeOnly(12, 0, 0); "
        "if a.CompareTo(b) = 0 then return 1; end; "
        "return 0;"
    )), 1);
}

// stdlib §2.5 / #1357: TimeOnly.CompareTo — earlier time < later → negative.
BEZEL_TEST(codegen_dateonly_timeonly_1357, timeonly_compareto_earlier_negative) {
    BEZEL_EXPECT_EQ(run(prog(
        "var a := new TimeOnly(10, 0, 0); "
        "var b := new TimeOnly(12, 0, 0); "
        "if a.CompareTo(b) < 0 then return 1; end; "
        "return 0;"
    )), 1);
}

// stdlib §2.5 / #1357: TimeOnly.ToString — returns non-empty "HH:mm:ss".
BEZEL_TEST(codegen_dateonly_timeonly_1357, timeonly_tostring_nonempty) {
    BEZEL_EXPECT_EQ(run(prog(
        "var t := new TimeOnly(14, 30, 45); "
        "var s := t.ToString(); "
        "if s.Length = 8 then return 1; end; "  // "HH:mm:ss" = 8 chars
        "return 0;"
    )), 1);
}

// stdlib §2.5 / #1357: TimeOnly.Parse — "14:30:45" → Hour = 14.
BEZEL_TEST(codegen_dateonly_timeonly_1357, timeonly_parse_hour) {
    BEZEL_EXPECT_EQ(run(prog(
        "var t := TimeOnly.Parse(\"14:30:45\"); "
        "if t.Hour = 14 then return 1; end; "
        "return 0;"
    )), 1);
}

// stdlib §2.5 / #1357: TimeOnly.TryParse — valid → true, correct minute.
BEZEL_TEST(codegen_dateonly_timeonly_1357, timeonly_tryparse_valid) {
    BEZEL_EXPECT_EQ(run(prog(
        "var t: TimeOnly; "
        "var ok := TimeOnly.TryParse(\"14:30:45\", out t); "
        "if ok and t.Minute = 30 then return 1; end; "
        "return 0;"
    )), 1);
}

// stdlib §2.5 / #1357: TimeOnly.TryParse — invalid → false.
BEZEL_TEST(codegen_dateonly_timeonly_1357, timeonly_tryparse_invalid) {
    BEZEL_EXPECT_EQ(run(prog(
        "var t: TimeOnly; "
        "var ok := TimeOnly.TryParse(\"not-a-time\", out t); "
        "if not ok then return 1; end; "
        "return 0;"
    )), 1);
}
