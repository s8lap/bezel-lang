// Spec-derived regression test for issue #7501 -- the `DateTime` constructor's
// `kind` PARAMETER was SILENTLY DROPPED, and a legal §22.5 named-argument
// shape was REFUSED.
//
// THE TWO REAL DEFECTS (the issue filed three gaps; the third was not a
// defect -- see THE THIRD GAP below).
//
// (1) THE `kind` PARAMETER WAS SILENTLY DROPPED.  stdlib §2.5 declares
//
//     public constructor(year: int, month: int, day: int);
//     public constructor(year: int, month: int, day: int, hour: int,
//                        minute: int, second: int,
//                        millisecond: int := 0,
//                        kind: DateTimeKind := DateTimeKind.Unspecified);
//
//     and the constructor's codegen arm read parameters 0..6 ONLY, so the
//     eighth argument -- the `kind` -- was NEVER LOWERED.  A construction
//     that supplied it LOST it without a diagnostic.  A silently discarded
//     argument is a WRONG ANSWER, not a rejection.
//
// (2) A LEGAL §22.5 SHAPE WAS REFUSED.  spec §22.5 "Optional parameters and
//     named arguments" (v0.350): "named arguments may appear in any order
//     after positional arguments", and a parameter that DECLARES A DEFAULT
//     may stay unbound.  stdlib §2.5 declares
//
//     public constructor(hour: int, minute: int, second: int := 0,
//                        millisecond: int := 0);      // TimeOnly
//
//     so `new TimeOnly(millisecond := 5, minute := 2, hour := 1)` leaves
//     `second` to its default and binds `millisecond` -- a legal shape.  It
//     was refused with "a named argument that binds to a parameter other than
//     its own position is not supported on a built-in constructor -- `second`
//     is parameter 3 but is left to its default while a later argument is
//     supplied".
//
// THE THIRD GAP IS NOT A DEFECT, AND THIS FILE SAYS NOTHING ABOUT IT.
//
// The issue's second half claimed that a NAMED `DateTime` at FOUR or FIVE
// arguments must be accepted.  stdlib §2.5 declares NO four- or five-
// argument `DateTime` -- `hour`, `minute` and `second` have NO defaults, so
// §22.5's "every parameter that declares no default must be bound" REFUSES
// that shape CORRECTLY.  Adding a row for it would ADMIT AN UNDECLARED
// CONSTRUCTOR, so there is deliberately NO such row here.
//
// THE ORACLE.
//
// Every row prints a VALUE and asserts the exact text: the `kind` the
// constructed value carries, or the components of the constructed `TimeOnly`.
// A construction that DROPS its `kind` prints "False" (the kind is not the
// one that was supplied); a construction that is REFUSED prints no value at
// all.  Both are RED on the pre-fix tree.  There is no negative assertion on
// a diagnostic anywhere in this file: the rows assert the CORRECT observable.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotOut;

namespace {

std::string prog(const std::string& body) {
    return "module M; import System; "
           "public class C "
           "  public static method Main(): int " +
           body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

}  // namespace

// ===========================================================================
// GAP (1) -- THE `kind` PARAMETER REACHES THE VALUE.
//
// stdlib §2.5 declares `kind: DateTimeKind := DateTimeKind.Unspecified` as
// the constructor's eighth parameter, and §2.5 declares the `Kind` property
// that reads it back.  A construction that supplies `DateTimeKind.Utc` must
// CARRY it -- the answer is "True".
// ===========================================================================
BEZEL_TEST(datetime_kind_named_ctor_7501,
           datetime_kind_reaches_the_value_in_the_positional_spelling) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    var d := new DateTime(2024, 2, 1, 5, 6, 7, 8, "
            "DateTimeKind.Utc); "
            "    Console.WriteLine(d.Kind = DateTimeKind.Utc); ")),
        std::string("True"));
}

// ===========================================================================
// THE SAME, FOR THE OTHER TWO KINDS.
//
// stdlib §2.5 declares `public enum DateTimeKind: byte
// Unspecified, Utc, Local;`.  A `kind` that is dropped is indistinguishable
// from `Unspecified`, so the `Utc` row alone would pass if the parameter
// were dropped and the default substituted.  These two rows make the
// drop observable for every declared member of the enum.
// ===========================================================================
BEZEL_TEST(datetime_kind_named_ctor_7501,
           datetime_local_kind_reaches_the_value) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    var d := new DateTime(2024, 2, 1, 5, 6, 7, 8, "
            "DateTimeKind.Local); "
            "    Console.WriteLine(d.Kind = DateTimeKind.Local); ")),
        std::string("True"));
}

BEZEL_TEST(datetime_kind_named_ctor_7501,
           datetime_unspecified_kind_reaches_the_value) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    var d := new DateTime(2024, 2, 1, 5, 6, 7, 8, "
            "DateTimeKind.Unspecified); "
            "    Console.WriteLine(d.Kind = DateTimeKind.Unspecified); ")),
        std::string("True"));
}

// ===========================================================================
// GAP (1) -- THE ALL-NAMED SPELLING.
//
// spec §22.5: a named argument binds THE PARAMETER IT NAMES, in any order
// after the positional ones.  The `kind` names the parameter `kind`, and the
// construction must carry it.
// ===========================================================================
BEZEL_TEST(datetime_kind_named_ctor_7501,
           datetime_kind_reaches_the_value_in_the_all_named_spelling) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    var d := new DateTime(year := 2024, month := 2, day := 1, "
            "hour := 5, minute := 6, second := 7, millisecond := 8, "
            "kind := DateTimeKind.Utc); "
            "    Console.WriteLine(d.Kind = DateTimeKind.Utc); ")),
        std::string("True"));
}

// ===========================================================================
// GAP (1) -- THE MIXED SPELLING, WITH A DEFAULTED PARAMETER LEFT UNBOUND.
//
// `millisecond` declares a default (`millisecond: int := 0`), so leaving it
// unbound while `kind` is supplied is a legal §22.5 shape; the `kind` must
// still be carried.
// ===========================================================================
BEZEL_TEST(datetime_kind_named_ctor_7501,
           datetime_kind_reaches_the_value_in_the_mixed_spelling) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    var d := new DateTime(2024, 2, 1, 5, 6, 7, "
            "kind := DateTimeKind.Utc); "
            "    Console.WriteLine(d.Kind = DateTimeKind.Utc); ")),
        std::string("True"));
}

// ===========================================================================
// THE CONTROL -- THE THREE-ARGUMENT CONSTRUCTOR.
//
// stdlib §2.5's first `DateTime` constructor declares NO `kind`, so the
// value's kind is `Unspecified` by the record's own default.  This row is
// green on both arms and shows that the rows above are not merely "some
// DateTime carries some kind".
// ===========================================================================
BEZEL_TEST(datetime_kind_named_ctor_7501,
           datetime_without_a_kind_is_unspecified) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    var d := new DateTime(2024, 2, 1); "
            "    Console.WriteLine(d.Kind = DateTimeKind.Unspecified); ")),
        std::string("True"));
}

// ===========================================================================
// GAP (2) -- A NAMED ARGUMENT MAY SKIP A DEFAULTED PARAMETER.
//
// stdlib §2.5's `TimeOnly` declares `second: int := 0` and
// `millisecond: int := 0`, so a named spelling that leaves `second` unbound
// is legal under §22.5.  The row asserts the CONSTRUCTED VALUE's four
// components, so it is RED both when the call is refused (no value at all)
// and when it is accepted with the wrong binding.
// ===========================================================================
BEZEL_TEST(datetime_kind_named_ctor_7501,
           timeonly_named_args_may_skip_a_defaulted_parameter) {
    BEZEL_EXPECT_EQ(
        runAotOut(prog(
            "    var t := new TimeOnly(millisecond := 5, minute := 2, "
            "hour := 1); "
            "    Console.WriteLine($\"{t.Hour} {t.Minute} {t.Second} "
            "{t.Millisecond}\"); ")),
        std::string("1 2 0 5"));
}
