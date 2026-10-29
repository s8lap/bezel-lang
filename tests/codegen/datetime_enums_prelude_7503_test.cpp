// Regression tests for #7503 (and the enum half of #7536): the stdlib §2.5
// enums `DayOfWeek` and `DateTimeKind` did not exist as types.
//
// WHAT THE DOCUMENTS REQUIRE.  Stdlib §2.5 (v0.350) declares, verbatim:
//
//   public enum DayOfWeek: byte
//     Sunday, Monday, Tuesday, Wednesday, Thursday, Friday, Saturday;
//   end DayOfWeek;
//
//   public enum DateTimeKind: byte
//     Unspecified, Utc, Local;
//   end DateTimeKind;
//
// Spec §2.3 (v0.350): `System` is auto-imported and every always-available name
// but the primitives may be shadowed by a current-module declaration (§2.2).
// Spec §10.2: constants auto-increment from 0 in declaration order; §5.2: the
// underlying-type conversion `byte(e)`.
//
// THE DEFECT.  Neither name resolved: `DateTimeKind.Utc` was "unresolved
// identifier `DateTimeKind`" and `DayOfWeek` was unresolved the same way, so
// the type of `DateTime`'s `kind` parameter could not be named by anyone.
//
// THE FIX.  Driver.cpp injects both declarations as a verbatim Bezel prelude
// (kTimePreludeSource), the mechanism the System.IO enums already use.
//
// THE ORACLE.  Every row asserts the program's EXACT stdout (and that it ran,
// exit 0): before the fix none of them COMPILES, so `ran` is false.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

std::string prog(const std::string& decls, const std::string& body) {
    return "module M; " + decls +
           " class C public static method Main(): int " + body +
           " return 0; end Main; end C;";
}

}  // namespace

// §2.5 / §10.2 (v0.350): the members of both enums exist, in the declared
// order, with ordinals 0.. (Sunday = 0 ... Saturday = 6; Unspecified = 0,
// Utc = 1, Local = 2).
BEZEL_TEST(datetime_enums_prelude_7503, stdlib_2_5_members_and_ordinals) {
    auto r = runAot(prog("",
        "Console.WriteLine(nameof(DayOfWeek.Sunday)); "
        "Console.WriteLine(nameof(DayOfWeek.Saturday)); "
        "Console.WriteLine(int(byte(DayOfWeek.Sunday))); "
        "Console.WriteLine(int(byte(DayOfWeek.Monday))); "
        "Console.WriteLine(int(byte(DayOfWeek.Wednesday))); "
        "Console.WriteLine(int(byte(DayOfWeek.Saturday))); "
        "Console.WriteLine(nameof(DateTimeKind.Unspecified)); "
        "Console.WriteLine(nameof(DateTimeKind.Utc)); "
        "Console.WriteLine(nameof(DateTimeKind.Local)); "
        "Console.WriteLine(int(byte(DateTimeKind.Unspecified))); "
        "Console.WriteLine(int(byte(DateTimeKind.Utc))); "
        "Console.WriteLine(int(byte(DateTimeKind.Local))); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
        "Sunday\nSaturday\n0\n1\n3\n6\n"
        "Unspecified\nUtc\nLocal\n0\n1\n2\n");
}

// §2.5 (v0.350): the members compare with `=` / `<>`.
BEZEL_TEST(datetime_enums_prelude_7503, stdlib_2_5_members_compare) {
    auto r = runAot(prog("",
        "var k: DateTimeKind := DateTimeKind.Utc; "
        "var d: DayOfWeek := DayOfWeek.Friday; "
        "Console.WriteLine(k = DateTimeKind.Utc); "
        "Console.WriteLine(k = DateTimeKind.Local); "
        "Console.WriteLine(k <> DateTimeKind.Unspecified); "
        "Console.WriteLine(d = DayOfWeek.Friday); "
        "Console.WriteLine(d = DayOfWeek.Saturday); "
        "Console.WriteLine(d <> DayOfWeek.Sunday); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
        "True\nFalse\nTrue\nTrue\nFalse\nTrue\n");
}

// §2.5 / §10.6 / §19.6 (v0.350): both enums are usable as a parameter type,
// a return type, a parameter default (`kind: DateTimeKind :=
// DateTimeKind.Unspecified`, the stdlib's own spelling) and as the scrutinee of
// an exhaustive `case` expression.
BEZEL_TEST(datetime_enums_prelude_7503, stdlib_2_5_parameter_return_default_and_case) {
    auto r = runAot(
        "module M; "
        "class C "
        "  public static method Describe(k: DateTimeKind): string "
        "    return (case k of "
        "      DateTimeKind.Unspecified => \"unspec\", "
        "      DateTimeKind.Utc => \"utc\", "
        "      DateTimeKind.Local => \"local\"); "
        "  end Describe; "
        "  public static method IsWeekend(d: DayOfWeek): bool "
        "    return (case d of "
        "      DayOfWeek.Saturday => true, "
        "      DayOfWeek.Sunday => true, "
        "      _ => false); "
        "  end IsWeekend; "
        "  public static method Stamp(k: DateTimeKind := DateTimeKind.Unspecified): DateTimeKind "
        "    return k; "
        "  end Stamp; "
        "  public static method Main(): int "
        "    Console.WriteLine(Describe(DateTimeKind.Unspecified)); "
        "    Console.WriteLine(Describe(DateTimeKind.Utc)); "
        "    Console.WriteLine(Describe(DateTimeKind.Local)); "
        "    Console.WriteLine(IsWeekend(DayOfWeek.Sunday)); "
        "    Console.WriteLine(IsWeekend(DayOfWeek.Wednesday)); "
        "    Console.WriteLine(Describe(Stamp())); "
        "    Console.WriteLine(Describe(Stamp(DateTimeKind.Local))); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText,
        "unspec\nutc\nlocal\nTrue\nFalse\nunspec\nlocal\n");
}

// §5.2 / §10.4 (v0.350): the underlying `byte` conversion works in both
// directions: `byte(e)` and the compiler-provided `FromUnderlyingOrNull`
// (null for an ordinal no constant has).
BEZEL_TEST(datetime_enums_prelude_7503, stdlib_2_5_underlying_byte_conversion) {
    auto r = runAot(prog("",
        "var b: byte := byte(DayOfWeek.Wednesday); "
        "Console.WriteLine(int(b)); "
        "var back: DayOfWeek? := DayOfWeek.FromUnderlyingOrNull(b); "
        "Console.WriteLine(back = DayOfWeek.Wednesday); "
        "var none: DayOfWeek? := DayOfWeek.FromUnderlyingOrNull(byte(9)); "
        "Console.WriteLine(none = null); "
        "var kb: DateTimeKind? := DateTimeKind.FromUnderlyingOrNull(byte(2)); "
        "Console.WriteLine(kb = DateTimeKind.Local); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "3\nTrue\nTrue\nTrue\n");
}

// §2.3 / §2.2 (v0.350): a current-module top-level `enum DateTimeKind` SHADOWS
// the auto-imported one, as the existing System.IO prelude enums do (control
// row: the same shape on `SeekOrigin`).  The user's own members are the only
// ones in scope and the program compiles and runs.
BEZEL_TEST(datetime_enums_prelude_7503, spec_2_3_user_enum_datetimekind_shadows_prelude) {
    auto r = runAot(prog(
        "enum DateTimeKind Red, Green; end DateTimeKind; ",
        "var k: DateTimeKind := DateTimeKind.Green; "
        "Console.WriteLine(nameof(DateTimeKind.Green)); "
        "Console.WriteLine(int(byte(DateTimeKind.Green))); "
        "Console.WriteLine(k = DateTimeKind.Green); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "Green\n1\nTrue\n");
}

// §2.3 / §2.2 (v0.350): a user CLASS named `DayOfWeek` shadows the prelude
// enum too (the shadowing is by name, whatever the declaration's kind).
BEZEL_TEST(datetime_enums_prelude_7503, spec_2_3_user_class_dayofweek_shadows_prelude) {
    auto r = runAot(prog(
        "class DayOfWeek "
        "  public static method Name(): string return \"mine\"; end Name; "
        "end DayOfWeek; ",
        "Console.WriteLine(DayOfWeek.Name()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "mine\n");
}

// Control: today's shadowing of the System.IO prelude enums, which the
// §2.5 prelude must behave like.
BEZEL_TEST(datetime_enums_prelude_7503, control_user_enum_seekorigin_shadows_prelude) {
    auto r = runAot(prog(
        "enum SeekOrigin Alpha, Beta; end SeekOrigin; ",
        "Console.WriteLine(nameof(SeekOrigin.Beta)); "
        "Console.WriteLine(int(byte(SeekOrigin.Beta))); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "Beta\n1\n");
}
