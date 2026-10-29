// Spec-derived regression test for issue #7784 — a `byte` / `sbyte` / `short`
// / `ushort` argument (or its §7.4-narrowed nullable) at an `int` parameter of
// a built-in DateTime / DateOnly / TimeOnly constructor (campaign
// builtin-call-admission, lane C2-37).
//
// ─── THE NORMATIVE SENTENCES (CHECKED in docs/, v0.350) ─────────────────────
//
// stdlib §2.5 declares `DateTime(year: int, month: int, day: int, hour: int,
//   minute: int, second: int, millisecond: int := 0, kind: ...)`,
//   `DateOnly(year: int, month: int, day: int)` and `TimeOnly(hour: int,
//   minute: int, second: int := 0, millisecond: int := 0)`.
// spec §5.1: an implicit widening conversion takes a narrower integer type to
//   a wider one (`byte`/`sbyte`/`short`/`ushort` to `int`), preserving the
//   value; so a `byte` or `short` argument is legal at an `int` parameter and
//   the callee reads the VALUE it denotes (`byte` 200 is 200, not -56).
// spec §7.4 / §22.6: a read of a binding §7.4 has narrowed non-null is of its
//   non-nullable type (`byte?` narrowed is a `byte`).
//
// ─── THE DEFECT (MEASURED on staging b1dcf5eb) ──────────────────────────────
//
// `new DateTime(2020, 1, b, 1, 1, 1)` with `b: byte` (and `short`) aborted
// the compiler in the LLVM verifier ("DestTy too big for Trunc": the
// constructor lowering emitted `trunc i8 to i32`), and the narrowed `byte?` /
// `short?` forms aborted the same way ("Trunc only operates on integer").

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotOut;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::diagnosticMessageContains;

namespace {

std::string prog(const std::string& body) {
    return "module M; import System; "
           "class C "
           "  public static method Main(): int " +
           body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

}  // namespace

// The issue's q01: a `byte` at `day`.
BEZEL_TEST(builtin_ctor_narrow_int_args_7784, datetime_byte_arg) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var l: byte := 5; var dt := new DateTime(2020, 1, l, 1, 1, 1); "
        "    Console.WriteLine(dt.Day);")),
        "5");
}

// A `byte` / `short` / `sbyte` / `ushort` at EVERY position it can hold.
BEZEL_TEST(builtin_ctor_narrow_int_args_7784, datetime_every_position_narrow_types) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var y: short := 2001; var mo: byte := 2; var d: sbyte := 3; "
        "    var h: ushort := 4; var mi: byte := 5; var s: short := 6; "
        "    var ms: ushort := 7; "
        "    var dt := new DateTime(y, mo, d, h, mi, s, ms); "
        "    Console.WriteLine(dt.ToString()); Console.WriteLine(dt.Millisecond);")),
        "2001-02-03T04:05:06\n7");
}

// ZERO-extension: a `byte` above the signed midpoint is its value (200), not
// -56, at `millisecond` of both constructors.
BEZEL_TEST(builtin_ctor_narrow_int_args_7784, byte_above_127_is_zero_extended) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var b: byte := 200; "
        "    var dt := new DateTime(2020, 1, 1, 0, 0, 0, b); "
        "    Console.WriteLine(dt.Millisecond); "
        "    var t := new TimeOnly(0, 0, 0, b); "
        "    Console.WriteLine(t.Millisecond);")),
        "200\n200");
}

// The issue's q04 / q03: DateOnly with a `short` and a narrowed `short?`.
BEZEL_TEST(builtin_ctor_narrow_int_args_7784, dateonly_short_and_narrowed_short) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var l: short := 5; var dt := new DateOnly(2020, 1, l); "
        "    Console.WriteLine(dt.Day); "
        "    var n: short? := 6; var d2 := new DateOnly(2020, 1, n); "
        "    Console.WriteLine(d2.Day);")),
        "5\n6");
}

// The issue's q05 / o03: narrowed `byte?`.
BEZEL_TEST(builtin_ctor_narrow_int_args_7784, timeonly_and_datetime_narrowed_byte) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var l: byte? := 5; var t := new TimeOnly(l, 3); "
        "    Console.WriteLine(t.Hour); "
        "    var dt := new DateTime(2020, 1, l, 1, 1, 1); "
        "    Console.WriteLine(dt.Day);")),
        "5\n5");
}

// Named arguments (§22.5), reordered, over narrow types.
BEZEL_TEST(builtin_ctor_narrow_int_args_7784, narrow_args_named) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var m: byte := 7; var h: short := 1; "
        "    var t := new TimeOnly(minute := m, hour := h); "
        "    Console.WriteLine(t.ToString()); "
        "    var dt := new DateTime(year := 2000, day := 4, month := m); "
        "    Console.WriteLine(dt.ToString());")),
        "01:07:00\n2000-07-04T00:00:00");
}

// Control: `int` arguments are unchanged.
BEZEL_TEST(builtin_ctor_narrow_int_args_7784, int_args_control) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var d: int := 9; var dt := new DateTime(2020, 1, d, 1, 1, 1); "
        "    Console.WriteLine(dt.Day);")),
        "9");
}

// An UN-narrowed `byte?` is still the §7 refusal, not a crash.
BEZEL_TEST(builtin_ctor_narrow_int_args_7784, unnarrowed_nullable_byte_refused) {
    auto r = runAot(
        "module M; import System; "
        "class C "
        "  public static method Main(): int return F(null); end Main; "
        "  public static method F(x: byte?): int "
        "    var dt := new DateTime(2020, 1, x, 1, 1, 1); return dt.Day; "
        "  end F; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "the argument to `DateTime` constructor is nullable but the target "
           "type is non-nullable"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
}
