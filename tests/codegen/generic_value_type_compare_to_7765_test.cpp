// Issue #7765 (spec §14.2, stdlib §2.5 *DateTime / TimeSpan / DateOnly /
// TimeOnly*, v0.350) -- a generic `where T: IComparable<T>` body that calls
// `a.CompareTo(b)` on a TYPE-PARAMETER receiver orders by the substituted
// value type's own `CompareTo`.
//
// THE GOVERNING TEXT.
//
//   * spec §14.2 (v0.350): a type parameter's constraint is the interface
//     the substitution must satisfy, and the body may call the constraint
//     member on a value of the parameter's type.
//   * stdlib §2.5 (v0.350): `DateTime`, `TimeSpan`, `DateOnly` and `TimeOnly`
//     each declare `IComparable<Self>`, so each of the four SATISFIES
//     `where T: IComparable<T>` -- and each has an order of its own (for a
//     `DateTime` it is TICK order, "CompareTo = tick order (Kind not
//     normalized, matching .NET)").
//   * stdlib §2.2 (v0.350): `Math.Min` / `Max` / `Clamp` order by `T`'s own
//     `CompareTo` -- the same order a generic body must reproduce.
//
// THE DEFECT. The `DateTime` / `TimeSpan` / `DateOnly` / `TimeOnly` instance
// arms read the receiver's type at the CALL SITE, so a receiver that is a
// bare TYPE PARAMETER (`a` in `Cmp<T>(a: T, b: T)`) was not recognised as
// any of them: the arm did not fire, the member call fell through the
// dispatch, and the body's `return a.CompareTo(b);` degraded to a
// zero-valued return -- "return-value expression failed to lower to a value
// (degraded to a zero/null return value)" -- so the generic comparison
// ALWAYS returned 0.
//
// MEASURED (the issue's own repro): `Cmp<DateTime>(u, l)` returned 0 where
// the direct `u.CompareTo(l)` returns -1. A generic `Max<T>` over a
// `DateTime` therefore returned the ZERO value and not the later date.
//
// THE FIX. The four arms read the receiver's type through the ACTIVE
// SUBSTITUTION -- the same `resolveTypeParamRef` the `Math` arm's DateTime
// key uses (`isDateTimeValueRef`) -- so the receiver's own type, not the
// spelling at the call site, decides the arm. The order is then the value
// type's own: TICK order for a `DateTime` (its `Kind` rides in the word's
// top two bits and does not take part), the plain tick count for a
// `TimeSpan`, the day number for a `DateOnly`, the millisecond count for a
// `TimeOnly`.
//
// ORACLE. `runAot` (the AOT path). Every row asserts the EXACT STDOUT -- a
// value oracle, never "no diagnostic was produced" -- so the order a
// generic body computes is pinned by what the call RETURNS.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotOut;

namespace {

// The generic comparison under test: `a.CompareTo(b)` where `a` and `b` are
// of the type parameter, so the receiver's type is only known through the
// substitution.
std::string genericCmpProg(const std::string& body) {
    return "module M; import System; "
           "public class Cmp<T> "
           "  where T: IComparable<T> "
           "  public static method Compare(a: T, b: T): int "
           "    return a.CompareTo(b); "
           "  end Compare; "
           "end Cmp; "
           "public class Program "
           "  public static method Main(): int " + body +
           "    return 0; "
           "  end Main; "
           "end Program;";
}

}  // namespace

// ===========================================================================
// ROW 1 -- the issue's own shape: a generic comparison at `T = DateTime`.
// The direct `u.CompareTo(l)` beside it is the ORDER the generic call must
// reproduce.
//
// Pre-fix: the generic calls returned 0 (the body's return expression
//           degraded to a zero-valued return); the direct ones were correct.
// Post-fix: "-1", "1", "-1", "1" -- the generic calls agree with the
//           direct ones.
// ===========================================================================
BEZEL_TEST(generic_value_type_compare_to_7765,
           a_generic_comparison_at_a_datetime_substitution_orders_by_ticks) {
    BEZEL_EXPECT_EQ(runAotOut(genericCmpProg(
        "    var u := new DateTime(2020, 1, 2, 3, 5, 6, 0, DateTimeKind.Utc); "
        "    var l := new DateTime(2020, 1, 2, 3, 5, 7, 0, DateTimeKind.Local); "
        "    var n := new DateTime(2020, 1, 2, 3, 5, 6); "
        "    var n2 := new DateTime(2020, 1, 2, 3, 5, 7); "
        "    Console.WriteLine(Cmp<DateTime>.Compare(u, l)); "
        "    Console.WriteLine(Cmp<DateTime>.Compare(l, u)); "
        "    Console.WriteLine(Cmp<DateTime>.Compare(n, n2)); "
        "    Console.WriteLine(Cmp<DateTime>.Compare(n2, n)); "
        )),
        "-1\n1\n-1\n1");
}

// ===========================================================================
// ROW 2 -- the same substitution through a generic `Max`, which is the shape
// stdlib §2.2's `Math.Max` order is written in: the generic body picks the
// later of the two dates.
//
// Pre-fix: the generic `Max` returned the ZERO value (the degraded
//           return), so the program printed 1 (the zero date's year).
// Post-fix: "2025".
// ===========================================================================
BEZEL_TEST(generic_value_type_compare_to_7765,
           a_generic_max_over_a_datetime_substitution_picks_the_later_date) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; import System; "
        "public class Max<T> "
        "  where T: IComparable<T> "
        "  public static method Pick(a: T, b: T): T "
        "    return (if a.CompareTo(b) > 0 then a else b); "
        "  end Pick; "
        "end Max; "
        "public class Program "
        "  public static method Main(): int "
        "    var m := Max<DateTime>.Pick(new DateTime(2024, 1, 1), "
        "                                new DateTime(2025, 1, 1)); "
        "    Console.WriteLine(m.Year); "
        "    return 0; "
        "  end Main; "
        "end Program;"),
        "2025");
}

// ===========================================================================
// ROW 3 -- the same substitution at `T = TimeSpan`: a plain signed tick
// count (stdlib §2.5).
//
// Pre-fix: the generic calls returned 0.
// Post-fix: "-1", "1".
// ===========================================================================
BEZEL_TEST(generic_value_type_compare_to_7765,
           a_generic_comparison_at_a_timespan_substitution_orders_by_ticks) {
    BEZEL_EXPECT_EQ(runAotOut(genericCmpProg(
        "    Console.WriteLine(Cmp<TimeSpan>.Compare(TimeSpan.FromTicks(5L), "
        "                                                 TimeSpan.FromTicks(9L))); "
        "    Console.WriteLine(Cmp<TimeSpan>.Compare(TimeSpan.FromTicks(9L), "
        "                                                 TimeSpan.FromTicks(5L))); "
        )),
        "-1\n1");
}

// ===========================================================================
// CONTROL -- the DIRECT comparison on two concrete values: the arm this
// change widens was already correct for a receiver whose type is written
// out, and it must stay correct.
//
// Green on both arms: "-1", "1".
// ===========================================================================
BEZEL_TEST(generic_value_type_compare_to_7765,
           the_direct_comparison_on_concrete_values_is_unchanged) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; import System; "
        "public class Program "
        "  public static method Main(): int "
        "    var u := new DateTime(2020, 1, 2, 3, 5, 6, 0, DateTimeKind.Utc); "
        "    var l := new DateTime(2020, 1, 2, 3, 5, 7, 0, DateTimeKind.Local); "
        "    Console.WriteLine(u.CompareTo(l)); "
        "    Console.WriteLine(l.CompareTo(u)); "
        "    return 0; "
        "  end Main; "
        "end Program;"),
        "-1\n1");
}

// ===========================================================================
// CONTROL -- the other substitutions the generic comparison already handled:
// a PRIMITIVE and a `string`. They were correct before this change and must
// stay correct.
//
// Green on both arms: "-1", "-1".
// ===========================================================================
BEZEL_TEST(generic_value_type_compare_to_7765,
           the_primitive_and_string_substitutions_are_unchanged) {
    BEZEL_EXPECT_EQ(runAotOut(genericCmpProg(
        "    Console.WriteLine(Cmp<int>.Compare(3, 9)); "
        "    Console.WriteLine(Cmp<string>.Compare(\"a\", \"b\")); "
        )),
        "-1\n-1");
}
