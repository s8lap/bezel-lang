// Spec-derived regression test for issue #7783 — a NAMED argument of a
// built-in constructor that binds a parameter PAST the number of arguments
// written (campaign builtin-call-admission, lane C2-37). Extends fix #7692
// (builtin_narrowed_ctor_7692_test.cpp), whose named-argument check read the
// parameter position against the written count.
//
// ─── THE NORMATIVE SENTENCES (CHECKED in docs/, v0.350) ─────────────────────
//
// stdlib §2.5 declares `DateTime(year: int, month: int, day: int, hour: int,
//   minute: int, second: int, millisecond: int := 0, kind: DateTimeKind :=
//   DateTimeKind.Unspecified)` and `TimeOnly(hour: int, minute: int, second:
//   int := 0, millisecond: int := 0)`: every parameter is non-nullable.
// spec §22.5: a named argument binds the parameter it names, whatever its
//   written position; a defaulted parameter may be skipped.
// spec §7 / §7.3: a `T?` reaches no non-nullable parameter; an un-narrowed
//   nullable (and the literal `null`) there is a compile error ("the argument
//   ... is nullable but the target type is non-nullable").
// spec §7.4 / §22.6: a read of a binding §7.4 has narrowed non-null is of its
//   non-nullable type, so a narrowed read is a LEGAL argument.
//
// ─── THE DEFECT (MEASURED on staging b1dcf5eb) ──────────────────────────────
//
// `new DateTime(2020,1,1,1,1,1, kind := k)` (null `k: DateTimeKind?`) printed
// 2020, and `new TimeOnly(1, 2, millisecond := null)` ran: the §7 query was
// asked at `position < argCount` with the WRITTEN count, so a named argument
// at parameter 7 (resp. 3) of a call writing 7 (resp. 3) arguments was never
// claimed. The narrowed forms (`millisecond := ms` over `ms: int? := 5`)
// aborted the compiler in the LLVM verifier ("Trunc only operates on
// integer").

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

std::string paramProg(const std::string& type, const std::string& body) {
    return "module M; import System; "
           "class C "
           "  public static method Main(): int "
           "    return F(null); "
           "  end Main; "
           "  public static method F(x: " + type + "): int " +
           body +
           "    return 0; "
           "  end F; "
           "end C;";
}

void expectCtorRefusedSrc(const std::string& src, const char* head) {
    auto r = runAot(src);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, (std::string("the argument to `") + head +
            "` constructor is nullable but the target type is non-nullable")
               .c_str()));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
}

void expectCtorRefused(const std::string& type, const std::string& stmt,
                       const char* head) {
    expectCtorRefusedSrc(paramProg(type, stmt), head);
}

}  // namespace

// ===========================================================================
// REFUSE: an un-narrowed nullable at a named parameter past the written count.
// ===========================================================================

// The issue's v02 (a null `DateTimeKind?` at `kind`, parameter 7 of 7 written).
BEZEL_TEST(builtin_ctor_named_past_count_7783, datetime_kind_named_nullable_refused) {
    expectCtorRefusedSrc(prog(
        "    var k: DateTimeKind? := DateTimeKind.Utc; k := null; "
        "    var dt := new DateTime(2020, 1, 1, 1, 1, 1, kind := k); "
        "    Console.WriteLine(dt.Year);"), "DateTime");
    expectCtorRefused("DateTimeKind?",
        "    var dt := new DateTime(2020, 1, 1, 1, 1, 1, kind := x);", "DateTime");
}

// The issue's v10 (the literal `null` at `millisecond`, parameter 3 of 3
// written) and its un-narrowed variable form.
BEZEL_TEST(builtin_ctor_named_past_count_7783, timeonly_millisecond_named_null_refused) {
    expectCtorRefusedSrc(prog(
        "    var t := new TimeOnly(1, 2, millisecond := null); "
        "    Console.WriteLine(t.Millisecond);"), "TimeOnly");
    expectCtorRefused("int?",
        "    var t := new TimeOnly(1, 2, millisecond := x);", "TimeOnly");
    // The issue's v04: narrowed by the initializer, then reassigned null.
    expectCtorRefusedSrc(prog(
        "    var ms: int? := 5; ms := null; "
        "    var t := new TimeOnly(1, 2, millisecond := ms); "
        "    Console.WriteLine(t.Millisecond);"), "TimeOnly");
}

// Every spelling that binds past the written count: all-named, a skipped
// `second`, and the DateTime `millisecond` skipped before `kind`.
BEZEL_TEST(builtin_ctor_named_past_count_7783, named_past_count_other_spellings_refused) {
    expectCtorRefused("int?",
        "    var t := new TimeOnly(hour := 1, minute := 2, millisecond := x);",
        "TimeOnly");
    expectCtorRefused("int?",
        "    var t := new TimeOnly(1, 2, 3, millisecond := x);", "TimeOnly");
    expectCtorRefused("int?",
        "    var t := new TimeOnly(1, minute := 2, millisecond := x);", "TimeOnly");
    expectCtorRefused("DateTimeKind?",
        "    var dt := new DateTime(2020, 1, 1, 1, 1, second := 1, kind := x);",
        "DateTime");
    expectCtorRefused("DateTimeKind?",
        "    var dt := new DateTime(year := 2020, month := 1, day := 1, "
        "hour := 1, minute := 1, second := 1, kind := x);", "DateTime");
}

// Controls the check already made: the named argument at a position BELOW the
// written count, and the positional twin.
BEZEL_TEST(builtin_ctor_named_past_count_7783, named_within_count_still_refused) {
    expectCtorRefused("int?",
        "    var dt := new DateTime(2020, 1, 1, 1, 1, 1, millisecond := x);",
        "DateTime");
    expectCtorRefused("int?",
        "    var t := new TimeOnly(1, 2, second := x);", "TimeOnly");
    expectCtorRefused("int?",
        "    var t := new TimeOnly(1, 2, 3, x);", "TimeOnly");
}

// ===========================================================================
// ACCEPT: the same shapes with a non-nullable or §7.4-narrowed argument.
// ===========================================================================

// The issue's v03: a narrowed `int?` at `millisecond` (past the written count)
// aborted the verifier; the value must be the payload.
BEZEL_TEST(builtin_ctor_named_past_count_7783, timeonly_millisecond_named_narrowed) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var ms: int? := 5; "
        "    var t := new TimeOnly(1, 2, millisecond := ms); "
        "    Console.WriteLine(t.Millisecond); "
        "    var u := new TimeOnly(hour := 3, minute := 4, millisecond := ms); "
        "    Console.WriteLine(u.Hour); Console.WriteLine(u.Millisecond);")),
        "5\n3\n5");
}

BEZEL_TEST(builtin_ctor_named_past_count_7783, timeonly_millisecond_named_plain) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var t := new TimeOnly(1, 2, millisecond := 9); "
        "    Console.WriteLine(t.Millisecond); Console.WriteLine(t.Second);")),
        "9\n0");
}

// A narrowed `DateTimeKind?` at `kind`, and the remedies `!` / `??`.
BEZEL_TEST(builtin_ctor_named_past_count_7783, datetime_kind_named_narrowed_and_remedies) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var k: DateTimeKind? := DateTimeKind.Utc; "
        "    var dt := new DateTime(2020, 1, 1, 1, 1, 1, kind := k); "
        "    Console.WriteLine(dt.Year); "
        "    var j: DateTimeKind? := DateTimeKind.Utc; j := null; "
        "    var e := new DateTime(2021, 1, 1, 1, 1, 1, "
        "                          kind := j ?? DateTimeKind.Utc); "
        "    Console.WriteLine(e.Year);")),
        "2020\n2021");
}

// A null test narrows a parameter (§7.4) past the written count.
BEZEL_TEST(builtin_ctor_named_past_count_7783, timeonly_millisecond_named_null_test_narrowed) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; import System; "
        "class C "
        "  public static method Main(): int "
        "    return F(7); "
        "  end Main; "
        "  public static method F(x: int?): int "
        "    if x <> null then "
        "      var t := new TimeOnly(1, 2, millisecond := x); "
        "      Console.WriteLine(t.Millisecond); "
        "    end; "
        "    return 0; "
        "  end F; "
        "end C;"),
        "7");
}
