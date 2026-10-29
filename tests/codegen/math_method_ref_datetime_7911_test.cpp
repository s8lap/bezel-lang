// Spec-derived regression test for issue #7911 (campaign builtin-call-admission,
// lane r37-c2-47-mathadmit): `Math.Max(DateTime, DateTime)` inside an unascribed
// lambda body failed to lower (#1386), and `Math.Max<DateTime>` as a METHOD
// REFERENCE was refused with "type argument does not satisfy the constraint".
//
// ─── THE NORMATIVE SENTENCES (CHECKED in docs/, v0.350) ─────────────────────
//
// stdlib §2.2: `Min<T>`, `Max<T>`, `Clamp<T>` are `where T: IComparable<T>`.
// stdlib §2.5: `DateTime`, `TimeSpan` (and `DateOnly`, `TimeOnly`) declare
//   `IComparable<Self>`, so each satisfies the constraint.
// spec §27.6: `Math.Max<T>` with its type-argument list, not the callee of a
//   call, is a function value `(T, T) -> T` (`Clamp`: `(T, T, T) -> T`), and
//   the formation is an instantiation site checked against the constraint.
// spec §14.2: a nullable type argument never satisfies an interface
//   constraint (`Math.Max<DateTime?>` stays refused).
//
// Scope note: after the #7900 / #7902 commits the unascribed-lambda rows already
// compile; the method-reference rows are what this commit's admission changes.
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
           "  static method ApplyD(f: (DateTime, DateTime) -> DateTime, x: DateTime, y: DateTime): DateTime "
           "    return f(x, y); "
           "  end ApplyD; "
           "  static method ApplyT(f: (TimeSpan, TimeSpan) -> TimeSpan, x: TimeSpan, y: TimeSpan): TimeSpan "
           "    return f(x, y); "
           "  end ApplyT; "
           "  public static method Main(): int " +
           body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

}  // namespace

// The issue's lambda row: Math.Max over DateTime inside a lambda body.
BEZEL_TEST(math_method_ref_datetime_7911, lambda_body_max_over_datetime_2_2) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var a := new DateTime(2020, 1, 3); "
                        "    var b := new DateTime(2020, 1, 1); "
                        "    var f := lambda(x: DateTime, y: DateTime) => Math.Max(x, y); "
                        "    Console.WriteLine(f(a, b).Day); "
                        "    Console.WriteLine(ApplyD(f, b, a).Day);")),
                    "3\n3");
}

// The issue's method-reference row: `Math.Max<DateTime>` passed as a value.
BEZEL_TEST(math_method_ref_datetime_7911, method_reference_datetime_max_min_27_6) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var a := new DateTime(2020, 1, 3); "
                        "    var b := new DateTime(2020, 1, 1); "
                        "    Console.WriteLine(ApplyD(Math.Max<DateTime>, a, b).Day); "
                        "    Console.WriteLine(ApplyD(Math.Min<DateTime>, a, b).Day);")),
                    "3\n1");
}

// The method reference bound to a local, then called.
BEZEL_TEST(math_method_ref_datetime_7911, method_reference_bound_to_local_27_6) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var a := new DateTime(2020, 1, 3); "
                        "    var b := new DateTime(2020, 1, 1); "
                        "    var f := Math.Max<DateTime>; "
                        "    Console.WriteLine(f(a, b).Day);")),
                    "3");
}

// The TimeSpan family (the #7900 type) as a method reference.
BEZEL_TEST(math_method_ref_datetime_7911, method_reference_timespan_2_5) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var a := new TimeSpan(100); "
                        "    var b := new TimeSpan(300); "
                        "    Console.WriteLine(ApplyT(Math.Max<TimeSpan>, a, b).Ticks); "
                        "    Console.WriteLine(ApplyT(Math.Min<TimeSpan>, a, b).Ticks);")),
                    "300\n100");
}

// A method reference over a Kind-carrying DateTime returns the winning
// argument unchanged (Kind is not normalized, stdlib §2.5).
BEZEL_TEST(math_method_ref_datetime_7911, method_reference_keeps_winner_kind_2_5) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
                        "    var u := new DateTime(2020, 1, 3, 0, 0, 0, 0, DateTimeKind.Utc); "
                        "    var l := new DateTime(2020, 1, 1, 0, 0, 0, 0, DateTimeKind.Local); "
                        "    var w := ApplyD(Math.Max<DateTime>, l, u); "
                        "    Console.WriteLine(w.Kind = DateTimeKind.Utc);")),
                    "True");
}

// CONTROL (mirror): a nullable type argument never satisfies the constraint.
BEZEL_TEST(math_method_ref_datetime_7911, nullable_type_argument_refused_14_2) {
    const JitResult r = runAot(prog(
        "    var f := Math.Max<DateTime?>;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "does not satisfy the constraint"));
}

// CONTROL (mirror): the bare reference names no type argument (§27.6).
BEZEL_TEST(math_method_ref_datetime_7911, bare_method_reference_refused_27_6) {
    const JitResult r = runAot(prog(
        "    var f := Math.Max;"));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
}
