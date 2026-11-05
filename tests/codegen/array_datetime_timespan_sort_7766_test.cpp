// Regression tests for issue #7766: `Array<DateTime>.Sort()` was REFUSED with
// "DateTime does not implement IComparable<DateTime>".
//
// WHAT THE DOCUMENTS REQUIRE (v0.355).
//   * stdlib §2.5 declares
//         public record DateTime: IComparable<DateTime>, IEquatable<DateTime>
//         public record TimeSpan: IComparable<TimeSpan>, IEquatable<TimeSpan>
//     with "CompareTo = tick order" (DateTime: Kind not normalized).
//   * stdlib §4.4 declares `Array<T>.Sort()` / `Sorted()` `where T:
//     IComparable<T>`, "checked at the call's instantiation".
//   * spec §14.2: `where T: IFoo` admits "records and enums implementing it".
// So T = DateTime / TimeSpan satisfies the constraint and the calls must be
// admitted and must order by CompareTo (ticks).
//
// Rows print the elements' Ticks, so a wrong comparator (the i32 `bz$intcmp`
// sorts a 64-bit word by its low half) shows as a wrong order, not merely a
// compile result. A refused program yields "<did-not-run>".

#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::runAotChecked;

namespace {
std::string outChecked(const std::string& program) {
    auto r = runAotChecked(program);
    if (!r.ran) return std::string("<did-not-run>");
    return r.stdoutText;
}
std::string prog(const std::string& body) {
    return "module M; class C public static method Main(): int " + body +
           " return 0; end Main; end C;";
}
}  // namespace

// DateTime.Sort(): ticks 3 * 2^32 (low half 0) vs small values: an i32 compare
// would put the big value first; tick order puts it last.
BEZEL_TEST(array_datetime_timespan_sort_7766, datetime_sort_in_place_orders_by_ticks) {
    BEZEL_EXPECT_EQ(
        outChecked(prog(
            "var a: Array<DateTime> := [DateTime.FromTicks(12884901888L), "
            "DateTime.FromTicks(7L), DateTime.FromTicks(5L)]; "
            "a.Sort(); "
            "for var x in a do Console.WriteLine(x.Ticks); end;")),
        std::string("5\n7\n12884901888\n"));
}

BEZEL_TEST(array_datetime_timespan_sort_7766, datetime_sorted_copy_orders_by_ticks_and_keeps_source) {
    BEZEL_EXPECT_EQ(
        outChecked(prog(
            "var a: Array<DateTime> := [DateTime.FromTicks(12884901888L), "
            "DateTime.FromTicks(7L), DateTime.FromTicks(5L)]; "
            "var s := a.Sorted(); "
            "for var x in s do Console.WriteLine(x.Ticks); end; "
            "Console.WriteLine(a[0].Ticks);")),
        std::string("5\n7\n12884901888\n12884901888\n"));
}

// TimeSpan.Sort() is SIGNED: a negative span sorts first.
BEZEL_TEST(array_datetime_timespan_sort_7766, timespan_sort_is_signed_tick_order) {
    BEZEL_EXPECT_EQ(
        outChecked(prog(
            "var a: Array<TimeSpan> := [TimeSpan.FromTicks(3L), "
            "TimeSpan.FromTicks(-9L), TimeSpan.FromTicks(0L)]; "
            "a.Sort(); "
            "for var x in a do Console.WriteLine(x.Ticks); end;")),
        std::string("-9\n0\n3\n"));
}

// The issue's own repro, printed through the DateTime rendering.
BEZEL_TEST(array_datetime_timespan_sort_7766, issue_repro_sorts_dates_chronologically) {
    BEZEL_EXPECT_EQ(
        outChecked(prog(
            "var lst: Array<DateTime> := [new DateTime(2020,1,3), "
            "new DateTime(2020,1,1), new DateTime(2020,1,2)]; "
            "lst.Sort(); "
            "for var x in lst do Console.WriteLine(x); end;")),
        std::string("2020-01-01T00:00:00\n2020-01-02T00:00:00\n2020-01-03T00:00:00\n"));
}
