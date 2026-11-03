// Spec-derived regression test for issue #7536 — the stdlib §2.5 enums
// `DateTimeKind` and `DayOfWeek` did not exist as TYPES, and `DateTime.Kind`
// degraded to zero.
//
// THE SPEC.
//
//   stdlib §2.5 "DateTime, TimeSpan, DateOnly, TimeOnly" (v0.350)
//   declares:
//
//     public enum DateTimeKind: byte Unspecified, Utc, Local;
//     public enum DayOfWeek: byte Sunday, Monday, ..., Saturday;
//
//   and the `DateTime` members that use them:
//
//     public constructor(year: int, month: int, day: int, hour: int,
//                        minute: int, second: int,
//                        millisecond: int := 0,
//                        kind: DateTimeKind := DateTimeKind.Unspecified);
//     public readonly property Kind: DateTimeKind;
//     public readonly property DayOfWeek: DayOfWeek;
//
//   §2.3 (v0.350) is the shadowing rule: a user top-level type of the same
//   name shadows the stdlib's.
//
// THE DEFECT.
//
// Neither enum name resolved ("unresolved identifier `DateTimeKind`"), so
// the type of `DateTime`'s `kind` parameter could not be named at all; and
// reading `d.Kind` warned "codegen cannot read member 'Kind' on receiver of
// type 'DateTime'" and DEGRADED TO 0 — so a `DateTime` built with
// `DateTimeKind.Utc` was indistinguishable from one built with
// `DateTimeKind.Unspecified`.
//
// THE ORACLE IS POSITIVE.
//
//   * the enums are NAMEABLE — a program that spells `DateTimeKind.Utc` and
//     `DayOfWeek.Friday` compiles and runs;
//   * `d.Kind` READS BACK the kind the value carries, which is observable
//     because the three declared members are three different values;
//   * `d.DayOfWeek` is the stdlib `DayOfWeek` ENUM, so it can be compared
//     with `DayOfWeek.Friday` — the comparison's answer is the oracle.
//
// No row asserts that a diagnostic is absent.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotOut;

namespace {

std::string prog(const std::string& body) {
    return "module M; "
           "class C "
           "  public static method Main(): int " +
           body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

}  // namespace

// ===========================================================================
// THE `kind` REACHES THE VALUE — stdlib §2.5's constructor parameter
// `kind: DateTimeKind := DateTimeKind.Unspecified` and the `Kind` property
// that reads it back.
//
// The three rows are the three declared members of the enum.  The
// `Unspecified` row is what makes a DROPPED kind observable: a dropped kind
// is indistinguishable from the default, so the `Utc` row alone would not
// catch it.
// ===========================================================================
BEZEL_TEST(datetime_kind_dayofweek_types_7536,
           datetime_kind_utc_reaches_the_value) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var d := new DateTime(2024, 2, 1, 5, 6, 7, 8, DateTimeKind.Utc); "
        "    Console.WriteLine(d.Kind = DateTimeKind.Utc);")),
        std::string("True"));
}

BEZEL_TEST(datetime_kind_dayofweek_types_7536,
           datetime_kind_local_reaches_the_value) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var d := new DateTime(2024, 2, 1, 5, 6, 7, 8, DateTimeKind.Local); "
        "    Console.WriteLine(d.Kind = DateTimeKind.Local);")),
        std::string("True"));
}

// ===========================================================================
// THE CONTROL — the THREE-ARGUMENT constructor has no `kind` parameter, so
// the value's kind is the declared default, `Unspecified`.
// ===========================================================================
BEZEL_TEST(datetime_kind_dayofweek_types_7536,
           datetime_kind_defaults_to_unspecified) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var d := new DateTime(2024, 2, 1); "
        "    Console.WriteLine(d.Kind = DateTimeKind.Unspecified);")),
        std::string("True"));
}

// ===========================================================================
// `DayOfWeek` IS THE STDLIB ENUM — 2024-02-02 was a Friday, 2024-02-01 a
// Thursday, so the property can be COMPARED with `DayOfWeek.Friday` and
// the comparison's answer is the oracle.  (Pre-fix the property was typed
// `int`, so it could not be bound to a `DayOfWeek` value at all.)
// ===========================================================================
BEZEL_TEST(datetime_kind_dayofweek_types_7536,
           datetime_dayofweek_is_the_stdlib_dayofweek_enum) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var d := new DateTime(2024, 2, 2); "
        "    Console.WriteLine(d.DayOfWeek = DayOfWeek.Friday);")),
        std::string("True"));
}

BEZEL_TEST(datetime_kind_dayofweek_types_7536,
           datetime_dayofweek_answers_the_other_day_too) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var d := new DateTime(2024, 2, 1); "
        "    Console.WriteLine(d.DayOfWeek = DayOfWeek.Thursday); "
        "    Console.WriteLine(d.DayOfWeek = DayOfWeek.Friday);")),
        std::string("True\nFalse"));
}

// ===========================================================================
// THE ENUMS ARE NAMEABLE ON THEIR OWN — stdlib §2.5 declares them, so a
// program may name `DayOfWeek.Friday` and `DateTimeKind.Utc` without a
// `DateTime` in sight.
// ===========================================================================
BEZEL_TEST(datetime_kind_dayofweek_types_7536,
           the_dayofweek_enum_is_nameable) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    Console.WriteLine(DayOfWeek.Friday = DayOfWeek.Friday); "
        "    Console.WriteLine(DayOfWeek.Friday = DayOfWeek.Thursday);")),
        std::string("True\nFalse"));
}

BEZEL_TEST(datetime_kind_dayofweek_types_7536,
           the_datetimekind_enum_is_nameable) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    Console.WriteLine(DateTimeKind.Utc = DateTimeKind.Utc); "
        "    Console.WriteLine(DateTimeKind.Utc = DateTimeKind.Local);")),
        std::string("True\nFalse"));
}
