// Codegen tests for DateTime new members — issue #1357 / stdlib §2.5 v0.165.
//
// New members:
//   - DateTime.CompareTo(other: DateTime): int  (IComparable<DateTime>)
//   - DateTime.ToUnixTimeSeconds(): long
//   - DateTime.FromUnixTimeSeconds(seconds: long): DateTime
//   - DateTime.Date: DateOnly
//   - DateTime.TimeOfDay: TimeOnly
//
// Note: Bezel uses `=` for equality (not `==`), `and`/`or`/`not` for boolean ops.
// Each test cites the stdlib section and specific behaviour it verifies.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& src) {
    auto r = runJit(src);
    return r.ran ? r.exitCode : -999;
}

std::string prog(const std::string& body) {
    return "module M; class C public static method Main(): int " + body +
           " end Main; end C;";
}

}  // namespace

// stdlib §2.5 / #1357: DateTime.CompareTo — equal datetimes → 0.
BEZEL_TEST(codegen_datetime_compar_unix_1357, compareto_equal_returns_zero) {
    BEZEL_EXPECT_EQ(run(prog(
        "var dt := DateTime.FromTicks(1000000L); "
        "var r := dt.CompareTo(DateTime.FromTicks(1000000L)); "
        "if r = 0 then return 1; end; "
        "return 0;"
    )), 1);
}

// stdlib §2.5 / #1357: DateTime.CompareTo — earlier < later → negative.
BEZEL_TEST(codegen_datetime_compar_unix_1357, compareto_earlier_negative) {
    BEZEL_EXPECT_EQ(run(prog(
        "var a := DateTime.FromTicks(1000000L); "
        "var b := DateTime.FromTicks(2000000L); "
        "var r := a.CompareTo(b); "
        "if r < 0 then return 1; end; "
        "return 0;"
    )), 1);
}

// stdlib §2.5 / #1357: DateTime.CompareTo — later > earlier → positive.
BEZEL_TEST(codegen_datetime_compar_unix_1357, compareto_later_positive) {
    BEZEL_EXPECT_EQ(run(prog(
        "var a := DateTime.FromTicks(2000000L); "
        "var b := DateTime.FromTicks(1000000L); "
        "var r := a.CompareTo(b); "
        "if r > 0 then return 1; end; "
        "return 0;"
    )), 1);
}

// stdlib §2.5 / #1357: DateTime.ToUnixTimeSeconds — POSIX epoch (tick 621355968000000000)
// should give seconds = 0.
BEZEL_TEST(codegen_datetime_compar_unix_1357, to_unix_time_seconds_epoch) {
    BEZEL_EXPECT_EQ(run(prog(
        // POSIX epoch = 621355968000000000 ticks
        "var epoch := DateTime.FromTicks(621355968000000000L); "
        "var secs := epoch.ToUnixTimeSeconds(); "
        "if secs = 0L then return 1; end; "
        "return 0;"
    )), 1);
}

// stdlib §2.5 / #1357: DateTime.FromUnixTimeSeconds — round-trips with ToUnixTimeSeconds.
BEZEL_TEST(codegen_datetime_compar_unix_1357, from_unix_time_seconds_roundtrip) {
    BEZEL_EXPECT_EQ(run(prog(
        "var dt := DateTime.FromUnixTimeSeconds(86400L); "  // 1 day after POSIX epoch
        "var secs := dt.ToUnixTimeSeconds(); "
        "if secs = 86400L then return 1; end; "
        "return 0;"
    )), 1);
}

// stdlib §2.5 / #1357: DateTime.Date — date part of a known datetime.
// Build 2025-01-15 00:00:00 via FromTicks, then extract .Date and check Year.
BEZEL_TEST(codegen_datetime_compar_unix_1357, date_property_year) {
    BEZEL_EXPECT_EQ(run(prog(
        "var dt := new DateTime(2025, 1, 15); "
        "var d := dt.Date; "
        "if d.Year = 2025 then return 1; end; "
        "return 0;"
    )), 1);
}

// stdlib §2.5 / #1357: DateTime.TimeOfDay — time part (hour component).
// Uses the 7-arg ctor (year,month,day,hour,minute,second,ms) to avoid
// DateTimeKind which is not yet wired up in the compiler.
BEZEL_TEST(codegen_datetime_compar_unix_1357, time_of_day_hour) {
    BEZEL_EXPECT_EQ(run(prog(
        "var dt := new DateTime(2025, 1, 15, 14, 30, 0, 0); "
        "var t := dt.TimeOfDay; "
        "if t.Hour = 14 then return 1; end; "
        "return 0;"
    )), 1);
}
