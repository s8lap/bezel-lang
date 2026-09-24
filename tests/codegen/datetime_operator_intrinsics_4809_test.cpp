// Regression tests for the R20 batch-1 gate failure
// codegen_datetime_2_5.utcnow_monotone, a fallout of fix #4809.
//
// #4809 typed `DateTime.UtcNow` (and the other §2.5 static-property reads)
// as the builtin `DateTime`. Before it, the read was UNTYPED, so
// `t2 - t1` over two UtcNow reads never reached sema's §25 "operator `-` is
// not defined for type" reject for Named operands. Once the read was typed,
// that reject fired on an operator stdlib §2.5 DECLARES:
//
//   operator -(a: DateTime, b: DateTime): TimeSpan
//   operator -(d: DateTime, t: TimeSpan): DateTime
//   operator +(d: DateTime, t: TimeSpan): DateTime
//   operator +(t: TimeSpan, d: DateTime): DateTime
//   operator +(a: TimeSpan, b: TimeSpan): TimeSpan
//   operator -(a: TimeSpan, b: TimeSpan): TimeSpan
//
// The fix types the four DateTime operators as compiler intrinsics (DateTime
// and TimeSpan are scalar i64 tick counts), beside the existing TimeSpan
// +/- intercept. Every row runs the program and asserts the computed VALUE,
// so a row that is accepted but lowered to the wrong type still fails.
//
// Cite: stdlib §2.5 (v0.307); spec §25 (operators), §5.1, v0.307.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

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

}  // namespace

// stdlib §2.5, v0.307: DateTime - DateTime → TimeSpan, on the exact shape
// the gate failed on (two DateTime.UtcNow reads). The result's `Ticks` is a
// TimeSpan member; successive UtcNow reads are non-decreasing.
BEZEL_TEST(datetime_operator_intrinsics_4809,
           utcnow_minus_utcnow_is_timespan_stdlib_2_5) {
    BEZEL_EXPECT_EQ(run(prog(
        "var t1 := DateTime.UtcNow; "
        "var t2 := DateTime.UtcNow; "
        "var d := t2 - t1; "
        "if d.Ticks >= 0L then return 1; end; "
        "return 0; ")), 1);
}

// stdlib §2.5, v0.307: the result of DateTime - DateTime is a TimeSpan, so
// it binds to a TimeSpan-annotated variable (§5.1 identity).
BEZEL_TEST(datetime_operator_intrinsics_4809,
           utcnow_difference_binds_timespan_stdlib_2_5) {
    BEZEL_EXPECT_EQ(run(prog(
        "var t1 := DateTime.UtcNow; "
        "var t2 := DateTime.UtcNow; "
        "var d: TimeSpan := t2 - t1; "
        "if d.TotalSeconds < 60.0 then return 1; end; "
        "return 0; ")), 1);
}

// stdlib §2.5, v0.307: DateTime - DateTime over constructed dates.
// 2024-01-11 - 2024-01-01 = 10 days.
BEZEL_TEST(datetime_operator_intrinsics_4809,
           datetime_minus_datetime_days_stdlib_2_5) {
    BEZEL_EXPECT_EQ(run(prog(
        "var a := new DateTime(2024, 1, 1); "
        "var b := new DateTime(2024, 1, 11); "
        "var s := b - a; "
        "return s.Days; ")), 10);
}

// stdlib §2.5, v0.307: DateTime - TimeSpan → DateTime.
// 2024-01-11 - 10 days = 2024-01-01.
BEZEL_TEST(datetime_operator_intrinsics_4809,
           datetime_minus_timespan_stdlib_2_5) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "var b := new DateTime(2024, 1, 11); "
        "var r := b - TimeSpan.FromDays(10.0); "
        "Console.WriteLine($\"{r.Year}-{r.Month}-{r.Day}\"); return 0; ")),
        "2024-1-1");
}

// stdlib §2.5, v0.307: DateTime - TimeSpan on the UtcNow shape; subtracting
// TimeSpan.Zero leaves the tick count unchanged.
BEZEL_TEST(datetime_operator_intrinsics_4809,
           utcnow_minus_timespan_zero_stdlib_2_5) {
    BEZEL_EXPECT_EQ(run(prog(
        "var t := DateTime.UtcNow; "
        "var u := t - TimeSpan.Zero; "
        "if u.Ticks = t.Ticks then return 1; end; "
        "return 0; ")), 1);
}

// stdlib §2.5, v0.307: DateTime + TimeSpan → DateTime.
BEZEL_TEST(datetime_operator_intrinsics_4809,
           datetime_plus_timespan_stdlib_2_5) {
    BEZEL_EXPECT_EQ(run(prog(
        "var a := new DateTime(2024, 1, 1); "
        "var r := a + TimeSpan.FromDays(10.0); "
        "return r.Day; ")), 11);
}

// stdlib §2.5, v0.307: TimeSpan + DateTime → DateTime (the result is a
// DateTime, not the left operand's TimeSpan).
BEZEL_TEST(datetime_operator_intrinsics_4809,
           timespan_plus_datetime_stdlib_2_5) {
    BEZEL_EXPECT_EQ(run(prog(
        "var a := new DateTime(2024, 1, 1); "
        "var r := TimeSpan.FromDays(10.0) + a; "
        "return r.Day; ")), 11);
}

// stdlib §2.5, v0.307: TimeSpan - TimeSpan and TimeSpan + TimeSpan →
// TimeSpan. 10d - 3d = 7d; 7d + 3d = 10d.
BEZEL_TEST(datetime_operator_intrinsics_4809,
           timespan_plus_minus_timespan_stdlib_2_5) {
    BEZEL_EXPECT_EQ(run(prog(
        "var s := TimeSpan.FromDays(10.0); "
        "var u := TimeSpan.FromDays(3.0); "
        "var r := s - u; "
        "var q := r + u; "
        "return r.Days * 10 + q.Days; ")), 80);
}
