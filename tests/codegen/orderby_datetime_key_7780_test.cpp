// Codegen -- stdlib section 5 *Ordering* (v0.357), stdlib section 2.5 (v0.357),
// issue 7780: `OrderBy` over a `DateTime` key yielded the wrong order.
//
// stdlib section 5:
//
//     public method OrderBy<TKey>(key: (T) -> TKey): IOrderedEnumerable<T>
//         where TKey: IComparable<TKey>;
//
// and stdlib section 2.5 declares `DateTime` (and `TimeSpan`, `DateOnly`,
// `TimeOnly`) `IComparable<Self>`: a `DateTime` key ranks by
// `DateTime.CompareTo`, which is the order of `Ticks`. The lowering called the
// key function through a `(T) -> int` signature, so a `DateTime` key (a
// two-field struct) and a `long` `Ticks` key were both ranked by 32 bits of
// whatever came back -- the same root as issue 8384, whose fix ranks every key
// by its own type's comparison.
//
// The issue's title also names `Array<DateTime>.Min()` / `.Max()` "typed int".
// That half no longer reproduces on the base this fix was made on (the control
// row below passes with and without the fix); it is pinned here so the issue's
// whole report stays covered.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

void expectOut(const JitResult& r, const std::string& out) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

}  // namespace

// THE ISSUE'S REPRO (p13_orderby.bzl), verbatim: four `DateTime`s of mixed
// `Kind`, ordered by themselves and by their `Ticks`. Both orders are
// 2020, 2021, 2026, 2027.
BEZEL_TEST(orderby_datetime_key_7780, issue_repro_datetime_and_ticks_keys_stdlib_5_2_5) {
    expectOut(runAotChecked(R"BZL(
module M;
class C
  public static method Main(): int
    var a: Array<DateTime> := [new DateTime(2026, 1, 1, 0,0,0,0, DateTimeKind.Local), new DateTime(2020, 1, 1), new DateTime(2027, 1, 1, 0,0,0,0,DateTimeKind.Utc), new DateTime(2021, 1, 1)];
    var o := a.OrderBy(lambda(x) => x).ToArray();
    Console.WriteLine(o[0].Year); Console.WriteLine(o[1].Year); Console.WriteLine(o[2].Year); Console.WriteLine(o[3].Year);
    var p := a.OrderBy(lambda(x) => x.Ticks).ToArray();
    Console.WriteLine(p[0].Year); Console.WriteLine(p[1].Year); Console.WriteLine(p[2].Year); Console.WriteLine(p[3].Year);
    return 0;
  end Main;
end C;
)BZL"),
              "2020\n2021\n2026\n2027\n2020\n2021\n2026\n2027\n");
}

// The whole family over a `DateTime` key, by lambda and by method reference,
// and a `DateTime` as the SECOND key of a chain (the first key ties every row
// of a month together). `DateOnly` and `TimeOnly` are the section's other
// `IComparable<Self>` value records.
BEZEL_TEST(orderby_datetime_key_7780, datetime_key_through_the_whole_family_stdlib_5_2_5) {
    expectOut(runAotChecked(R"BZL(
module M;
class C
  static method When(d: DateTime): DateTime => d;
  static method Show(label: string, xs: IEnumerable<DateTime>)
    Console.Write(label);
    for var d in xs do Console.Write($"{d.Year}-{d.Month} "); end;
    Console.WriteLine();
  end Show;
  public static method Main(): int
    var a: Array<DateTime> := [new DateTime(2026, 3, 1), new DateTime(2020, 7, 1), new DateTime(2027, 3, 1), new DateTime(2021, 7, 1), new DateTime(2019, 3, 1)];
    Show("asc: ", a.OrderBy(lambda(x) => x));
    Show("desc: ", a.OrderByDescending(lambda(x) => x));
    Show("asc-mref: ", a.OrderBy(When));
    Show("desc-mref: ", a.OrderByDescending(When));
    Show("month,date: ", a.OrderBy(lambda(x) => x.Month).ThenBy(lambda(x) => x));
    Show("month,date-desc: ", a.OrderBy(lambda(x) => x.Month).ThenByDescending(When));
    Show("month-desc,date: ", a.OrderByDescending(lambda(x) => x.Month).ThenBy(When));
    var ds: Array<DateOnly> := [new DateOnly(2026, 1, 2), new DateOnly(2020, 5, 6), new DateOnly(2021, 3, 4)];
    Console.Write("dateonly: ");
    for var d in ds.OrderBy(lambda(x) => x) do Console.Write($"{d.Year} "); end;
    for var d in ds.OrderByDescending(lambda(x) => x) do Console.Write($"{d.Year} "); end;
    Console.WriteLine();
    var ts: Array<TimeOnly> := [new TimeOnly(13, 30, 0), new TimeOnly(1, 2, 3), new TimeOnly(23, 59, 59)];
    Console.Write("timeonly: ");
    for var t in ts.OrderBy(lambda(x) => x) do Console.Write($"{t.Hour} "); end;
    for var t in ts.OrderByDescending(lambda(x) => x) do Console.Write($"{t.Hour} "); end;
    Console.WriteLine();
    return 0;
  end Main;
end C;
)BZL"),
              "asc: 2019-3 2020-7 2021-7 2026-3 2027-3 \n"
              "desc: 2027-3 2026-3 2021-7 2020-7 2019-3 \n"
              "asc-mref: 2019-3 2020-7 2021-7 2026-3 2027-3 \n"
              "desc-mref: 2027-3 2026-3 2021-7 2020-7 2019-3 \n"
              "month,date: 2019-3 2026-3 2027-3 2020-7 2021-7 \n"
              "month,date-desc: 2027-3 2026-3 2019-3 2021-7 2020-7 \n"
              "month-desc,date: 2020-7 2021-7 2019-3 2026-3 2027-3 \n"
              "dateonly: 2020 2021 2026 2026 2021 2020 \n"
              "timeonly: 1 13 23 23 13 1 \n");
}

// CONTROL (green with and without the fix): the issue's `Min()` / `Max()` half.
// `Array<DateTime>.Min()` / `.Max()` are `DateTime`s -- `.Year` reads off them.
BEZEL_TEST(orderby_datetime_key_7780, control_datetime_min_max_are_datetimes_stdlib_5) {
    expectOut(runAotChecked(R"BZL(
module M;
class C
  public static method Main(): int
    var a: Array<DateTime> := [new DateTime(2026, 1, 1, 0,0,0,0, DateTimeKind.Local), new DateTime(2020, 1, 1), new DateTime(2027, 1, 1, 0,0,0,0,DateTimeKind.Utc), new DateTime(2021, 1, 1)];
    var mn := a.Min();
    var mx := a.Max();
    Console.WriteLine(mn.Year);
    Console.WriteLine(mx.Year);
    return 0;
  end Main;
end C;
)BZL"),
              "2020\n2027\n");
}
