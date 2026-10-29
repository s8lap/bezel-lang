// Spec-derived regression test for issue #7900 (campaign builtin-call-admission,
// lane r37-c2-47-mathadmit): `Math.Min` / `Math.Max` / `Math.Clamp` over a
// `TimeSpan` (and over a `DateTime`) that came from a stdlib call was refused
// with "type argument `TimeSpan` does not satisfy the constraint `where T:
// IComparable<T>`".
//
// ─── THE NORMATIVE SENTENCES (CHECKED in docs/, v0.350) ─────────────────────
//
// stdlib §2.2: `Min<T>(a: T, b: T): T where T: IComparable<T>`, `Max<T>` and
//   `Clamp<T>(value: T, low: T, high: T): T where T: IComparable<T>`.
// stdlib §2.5: `public record TimeSpan: IComparable<TimeSpan>, IEquatable<
//   TimeSpan>` and `public record DateTime: IComparable<DateTime>, ...`
//   (likewise `DateOnly`, `TimeOnly`). So each satisfies §2.2's constraint
//   (spec §14.2: a type satisfies an interface constraint when it declares the
//   interface in its extension chain).
//
// ─── THE DEFECT ─────────────────────────────────────────────────────────────
//
// A `TimeSpan` produced by a stdlib CALL (`TimeSpan.FromSeconds(1)`, the
// `DateTime.AddDays` family) carries the builtin class symbol as its
// declaration, a Class with no base list, so sema's `IComparable` subtype test
// refused it. A `TimeSpan` from `new TimeSpan(...)` or an annotation carries no
// declaration and slipped past the test.
//
// The oracle is the program's OUTPUT (accept rows) or a located diagnostic
// (refuse rows).

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
    return "module M; "
           "class C "
           "  public static method Main(): int " +
           body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

}  // namespace

// The issue's repro shape: Min / Max / Clamp over stdlib-produced TimeSpans.
BEZEL_TEST(math_timespan_ordered_7900, from_seconds_min_max_clamp_2_2_2_5) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var s1 := TimeSpan.FromSeconds(1); "
                        "    var s2 := TimeSpan.FromSeconds(2); "
                        "    var s3 := TimeSpan.FromSeconds(3); "
                        "    var lo := Math.Min(s1, s2); "
                        "    var hi := Math.Max(s1, s2); "
                        "    var cl := Math.Clamp(s3, s1, s2); "
                        "    var mid := Math.Clamp(s2, s1, s3); "
                        "    Console.WriteLine(lo.TotalSeconds); "
                        "    Console.WriteLine(hi.TotalSeconds); "
                        "    Console.WriteLine(cl.TotalSeconds); "
                        "    Console.WriteLine(mid.TotalSeconds);")),
                    "1\n2\n2\n2");
}

// TimeSpan is a SIGNED tick count: a negative span orders below zero.
BEZEL_TEST(math_timespan_ordered_7900, negative_span_orders_below_positive_2_5) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var neg := TimeSpan.FromSeconds(-5); "
                        "    var pos := TimeSpan.FromSeconds(3); "
                        "    var lo := Math.Min(neg, pos); "
                        "    var hi := Math.Max(neg, pos); "
                        "    var lo2 := Math.Min(pos, neg); "
                        "    Console.WriteLine(lo.TotalSeconds); "
                        "    Console.WriteLine(hi.TotalSeconds); "
                        "    Console.WriteLine(lo2.TotalSeconds);")),
                    "-5\n3\n-5");
}

// A stdlib-produced span (first position) mixed with a constructed one.
BEZEL_TEST(math_timespan_ordered_7900, from_call_mixed_with_constructed_2_5) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var s := TimeSpan.FromSeconds(1); "
                        "    var t := new TimeSpan(100); "
                        "    var a := Math.Max(s, t); "
                        "    var c := Math.Min(s, t); "
                        "    Console.WriteLine(a.Ticks); "
                        "    Console.WriteLine(c.Ticks);")),
                    "10000000\n100");
}

// The explicit list `Math.Max<TimeSpan>` over stdlib-produced spans.
BEZEL_TEST(math_timespan_ordered_7900, explicit_list_from_call_2_2) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var s1 := TimeSpan.FromSeconds(1); "
                        "    var s2 := TimeSpan.FromSeconds(2); "
                        "    var m := Math.Max<TimeSpan>(s1, s2); "
                        "    Console.WriteLine(m.TotalSeconds);")),
                    "2");
}

// The same family on DateTime: a value produced by `AddDays` (stdlib call).
BEZEL_TEST(math_timespan_ordered_7900, datetime_from_call_min_max_2_5) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var d1 := new DateTime(2020, 1, 1); "
                        "    var d2 := d1.AddDays(2); "
                        "    var hi := Math.Max(d2, d1); "
                        "    var lo := Math.Min(d2, d1); "
                        "    Console.WriteLine(hi.Day); "
                        "    Console.WriteLine(lo.Day);")),
                    "3\n1");
}

// CONTROL (mirror): `T` must agree, so a TimeSpan and a DateTime do not mix.
BEZEL_TEST(math_timespan_ordered_7900, mixed_timespan_and_datetime_refused_14_5) {
    const JitResult r = runAot(prog(
        "    var s := TimeSpan.FromSeconds(1); "
        "    var d := new DateTime(2020, 1, 1); "
        "    var a := Math.Max(s, d);"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "inconsistent type-argument"));
}
