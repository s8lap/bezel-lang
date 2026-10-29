// Spec-derived regression test for issue #7902 (campaign builtin-call-admission,
// lane r37-c2-47-mathadmit): a generic `Math.Max(n1, n2)` / `Math.Min` /
// `Math.Clamp` over NARROWED `DateTime?` reads was refused with the #2786
// non-numeric-operand diagnostic, and `Console.WriteLine(Math.Min(dt1, dt2))`
// over non-nullable DateTimes printed the raw integer word.
//
// ─── THE NORMATIVE SENTENCES (CHECKED in docs/, v0.350) ─────────────────────
//
// stdlib §2.2: `Min<T>(a: T, b: T): T`, `Max<T>`, `Clamp<T>(value, low, high)`,
//   each `where T: IComparable<T>`; stdlib §2.5 declares `IComparable<Self>` on
//   DateTime and TimeSpan (so each satisfies the constraint).
// spec §7.4 / §22.6: a narrowed read of `T?` is of the non-nullable type `T`.
// spec §14.2: nullable types do not themselves implement the interfaces of
//   their non-nullable counterparts (an UN-narrowed `DateTime?` stays refused).
// stdlib §2.5 (v0.350): ordering is tick order; `Kind` is not normalized, so
//   the result of Min/Max is the winning ARGUMENT, Kind included.
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
           "  static method K(d: DateTime): string "
           "    if d.Kind = DateTimeKind.Utc then return \"Utc\"; end; "
           "    if d.Kind = DateTimeKind.Local then return \"Local\"; end; "
           "    return \"Unspecified\"; "
           "  end K; "
           "  public static method Main(): int " +
           body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

}  // namespace

// The issue's repro: Max over two narrowed `DateTime?` locals; the winner's
// Kind survives (Min/Max return the original word).
BEZEL_TEST(math_datetime_narrowed_7902, narrowed_max_min_keep_winner_kind_2_2_7_4) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var a := new DateTime(2020, 1, 3, 0, 0, 0, 0, DateTimeKind.Utc); "
                        "    var b := new DateTime(2020, 1, 1, 0, 0, 0, 0, DateTimeKind.Local); "
                        "    var n1: DateTime? := a; "
                        "    var n2: DateTime? := b; "
                        "    if n1 <> null and n2 <> null then "
                        "        var hi := Math.Max(n1, n2); "
                        "        var lo := Math.Min(n1, n2); "
                        "        Console.WriteLine(K(hi)); "
                        "        Console.WriteLine(K(lo)); "
                        "        Console.WriteLine(hi.Day); "
                        "        Console.WriteLine(lo.Day); "
                        "    end;")),
                    "Utc\nLocal\n3\n1");
}

// One narrowed operand, in either position, mixed with a non-nullable one.
BEZEL_TEST(math_datetime_narrowed_7902, narrowed_mixed_with_plain_either_position_14_5) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var a := new DateTime(2020, 1, 3); "
                        "    var b := new DateTime(2020, 1, 1); "
                        "    var n: DateTime? := a; "
                        "    if n <> null then "
                        "        Console.WriteLine(Math.Min(n, b).Day); "
                        "        Console.WriteLine(Math.Min(b, n).Day); "
                        "        Console.WriteLine(Math.Max(n, b).Day); "
                        "        Console.WriteLine(Math.Max(b, n).Day); "
                        "    end;")),
                    "1\n1\n3\n3");
}

// Clamp over narrowed reads in all three positions.
BEZEL_TEST(math_datetime_narrowed_7902, narrowed_clamp_2_2) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var v := new DateTime(2020, 1, 9); "
                        "    var l := new DateTime(2020, 1, 2); "
                        "    var h := new DateTime(2020, 1, 5); "
                        "    var nv: DateTime? := v; "
                        "    var nl: DateTime? := l; "
                        "    var nh: DateTime? := h; "
                        "    if nv <> null and nl <> null and nh <> null then "
                        "        Console.WriteLine(Math.Clamp(nv, nl, nh).Day); "
                        "        Console.WriteLine(Math.Clamp(l, nl, nh).Day); "
                        "        Console.WriteLine(Math.Clamp(nl, h, h).Day); "
                        "    end;")),
                    "5\n2\n5");
}

// The issue's second symptom: the result renders as a DateTime, not as the raw
// integer word, and is assignable to a `DateTime` variable.
BEZEL_TEST(math_datetime_narrowed_7902, plain_min_renders_as_datetime_not_raw_integer_2_2) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var u3 := new DateTime(2020, 1, 3); "
                        "    var l1 := new DateTime(2020, 1, 1, 0, 0, 0, 0, DateTimeKind.Local); "
                        "    Console.WriteLine(Math.Min(u3, l1).Year); "
                        "    var y: DateTime := Math.Min(u3, l1); "
                        "    Console.WriteLine(y.Month); "
                        "    var s := Math.Max(u3, l1).ToString(); "
                        "    Console.WriteLine(s.Length > 8);")),
                    "2020\n1\nTrue");
}

// TimeSpan? narrowed.
BEZEL_TEST(math_datetime_narrowed_7902, narrowed_timespan_max_2_2) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var a: TimeSpan? := new TimeSpan(100); "
                        "    var b: TimeSpan? := new TimeSpan(300); "
                        "    if a <> null and b <> null then "
                        "        Console.WriteLine(Math.Max(a, b).Ticks); "
                        "        Console.WriteLine(Math.Min(a, b).Ticks); "
                        "    end;")),
                    "300\n100");
}

// A declaration-narrowing `var m: DateTime? := u3` read right after is narrowed.
BEZEL_TEST(math_datetime_narrowed_7902, declaration_narrowing_accepted_7_4) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var u3 := new DateTime(2020, 1, 3); "
                        "    var l1 := new DateTime(2020, 1, 1); "
                        "    var m: DateTime? := u3; "
                        "    Console.WriteLine(Math.Min(m, l1).Day);")),
                    "1");
}

// CONTROL (mirror): an UN-narrowed nullable parameter is still refused.
BEZEL_TEST(math_datetime_narrowed_7902, nullable_parameter_stays_refused_14_2) {
    const JitResult r = runAot(
        "module M; "
        "class C "
        "  static method F(a: DateTime?, b: DateTime?): int "
        "    var m := Math.Max(a, b); "
        "    return 0; "
        "  end F; "
        "  public static method Main(): int return 0; end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "nullable type `DateTime?`"));
}

// CONTROL (mirror): the narrowing is invalidated by an assignment of null.
BEZEL_TEST(math_datetime_narrowed_7902, invalidated_narrowing_refused_7_4) {
    const JitResult r = runAot(prog(
        "    var a := new DateTime(2020, 1, 3); "
        "    var n: DateTime? := a; "
        "    n := null; "
        "    var m := Math.Max(n, a);"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}
