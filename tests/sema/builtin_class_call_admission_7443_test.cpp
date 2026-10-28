// Spec-derived regression test for issue #7443.
//
// stdlib §2.5 (v0.345) declares the CLASS-SPELLING static members:
//
//   public static method FromTicks(ticks: long): TimeSpan;
//   public static method FromTicks(ticks: long, kind: DateTimeKind := ...): DateTime;
//   public static method FromUnixTimeSeconds(seconds: long): DateTime;
//   public static method FromDays(days: double): TimeSpan;
//   public method AddYears(years: int): DateTime;
//   public method AddDays(days: double): DateTime;
//
// §5.1 (v0.345) lists `uint -> long` and `long -> float/double/decimal` —
// never `ulong -> long`, and never a binary float to an integer. Its
// never-implicit bullet covers "between signed and unsigned types of the
// same or narrower width", and `ulong` and `long` are the SAME width.
//
// BEFORE THE FIX (measured on 7626bb4d4e), each row = the call, the observed
// behaviour:
//
//   TimeSpan.FromTicks(vul)          -> ADMITTED and SILENTLY WRAPPED
//                                      ("ts ticks -446744073709551616");
//   DateTime.FromTicks(vul)          -> ADMITTED and silently wrapped;
//   DateTime.FromUnixTimeSeconds(vul)-> ADMITTED and silently wrapped;
//   TimeSpan.FromTicks(1.5)          -> the compiler ABORTED ("SExt only
//                                      operates on integer" — MALFORMED IR);
//   DateTime.FromTicks(1.5)          -> the same abort;
//   DateTime.FromUnixTimeSeconds(1.5)-> the same abort;
//   DateTime.AddYears(vuint)         -> ADMITTED and silently truncated.
//
// THE ORACLE. `runAot` — the strict AOT path. A rejection row asserts the
// FULL diagnostic at a REAL source location: the declared form (stdlib §2.5's
// own words), the parameter name, the argument's type and the §-rule it
// misses. A row that asserts only "the compiler did not abort" would be
// satisfied by a silent miscompile, so it is not used here.
//
// THE CONTROLS are the ones the issue names: the `uint` (ADMITTED by §5.1's
// widening table) and the `long` must keep compiling, and the `double`
// parameters (`FromDays`, `AddDays`) must keep accepting a `double`.
#include <string>
#include <vector>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

std::string prog(const std::string& decls, const std::string& stmt) {
    return "module M;\n"
           "class C\n"
           "  public static method Main(): int\n" +
           decls + "    " + stmt + "\n"
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

bool hasError(const JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

// A rejection row: the program must be REJECTED at compile time, with the
// named diagnostic, at a real source location.
void expectRejected(const std::string& decls, const std::string& stmt,
                    const std::string& needle) {
    auto r = runAot(prog(decls, stmt));
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(r, needle));
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
}

// A control row: the program must compile, run, and print exactly `out`.
void expectStdout(const std::string& decls, const std::string& stmt,
                  const std::string& out) {
    auto r = runAot(prog(decls, stmt));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
}

const char kUlong[] = "a `ulong` reaches a `long` parameter only as a "
                       "compile-time constant (§5.1)";
const char kDouble[] = "a `double` is never implicitly convertible to a "
                        "`long` (§5.1)";
const char kUint[] = "a `uint` reaches an `int` parameter only as a "
                     "compile-time constant (§5.1)";

}  // namespace

// ── the `ulong` at a `long` parameter: ADMITTED BEFORE, SILENTLY WRAPPED ───

BEZEL_TEST(sema_builtin_class_call_admission_7443,
           timespan_fromticks_ulong_is_rejected) {
    expectRejected(
        "    var v: ulong := 18000000000000000000;\n",
        "var ts := TimeSpan.FromTicks(v); Console.WriteLine(ts.Ticks);",
        "no matching overload of `TimeSpan.FromTicks` — stdlib §2.5 declares "
        "`FromTicks(ticks: long): TimeSpan`, so the `ticks` argument must be "
        "a `long` — " + std::string(kUlong));
}

BEZEL_TEST(sema_builtin_class_call_admission_7443,
           datetime_fromticks_ulong_is_rejected) {
    expectRejected(
        "    var v: ulong := 18000000000000000000;\n",
        "var d := DateTime.FromTicks(v); Console.WriteLine(d.Ticks);",
        "no matching overload of `DateTime.FromTicks` — stdlib §2.5 declares "
        "`FromTicks(ticks: long, kind: DateTimeKind := "
        "DateTimeKind.Unspecified): DateTime`, so the `ticks` argument must "
        "be a `long` — " + std::string(kUlong));
}

BEZEL_TEST(sema_builtin_class_call_admission_7443,
           datetime_fromunixtimeseconds_ulong_is_rejected) {
    expectRejected(
        "    var v: ulong := 18000000000000000000;\n",
        "var d := DateTime.FromUnixTimeSeconds(v); Console.WriteLine(d.Ticks);",
        "no matching overload of `DateTime.FromUnixTimeSeconds` — stdlib "
        "§2.5 declares `FromUnixTimeSeconds(seconds: long): DateTime`, so "
        "the `seconds` argument must be a `long` — " + std::string(kUlong));
}

// ── the `double` at a `long` parameter: the compiler ABORTED ──────────────

BEZEL_TEST(sema_builtin_class_call_admission_7443,
           timespan_fromticks_double_is_rejected) {
    expectRejected(
        "",
        "var ts := TimeSpan.FromTicks(1.5); Console.WriteLine(ts.Ticks);",
        "no matching overload of `TimeSpan.FromTicks` — stdlib §2.5 declares "
        "`FromTicks(ticks: long): TimeSpan`, so the `ticks` argument must be "
        "a `long` — " + std::string(kDouble));
}

BEZEL_TEST(sema_builtin_class_call_admission_7443,
           datetime_fromticks_double_is_rejected) {
    expectRejected(
        "",
        "var d := DateTime.FromTicks(1.5); Console.WriteLine(d.Ticks);",
        "no matching overload of `DateTime.FromTicks` — stdlib §2.5 declares "
        "`FromTicks(ticks: long, kind: DateTimeKind := "
        "DateTimeKind.Unspecified): DateTime`, so the `ticks` argument must "
        "be a `long` — " + std::string(kDouble));
}

BEZEL_TEST(sema_builtin_class_call_admission_7443,
           datetime_fromunixtimeseconds_double_is_rejected) {
    expectRejected(
        "",
        "var d := DateTime.FromUnixTimeSeconds(1.5); "
        "Console.WriteLine(d.Ticks);",
        "no matching overload of `DateTime.FromUnixTimeSeconds` — stdlib "
        "§2.5 declares `FromUnixTimeSeconds(seconds: long): DateTime`, so "
        "the `seconds` argument must be a `long` — " + std::string(kDouble));
}

// ── the `uint` at an `int` parameter (the `DateTime.Add*` family) ─────────

BEZEL_TEST(sema_builtin_class_call_admission_7443,
           datetime_addyears_uint_is_rejected) {
    expectRejected(
        "    var d := DateTime.FromTicks(0L);\n"
        "    var v: uint := 1;\n",
        "var e := d.AddYears(v); Console.WriteLine(e.Ticks);",
        "no matching overload of `DateTime.AddYears` — stdlib §2.5 declares "
        "`AddYears(years: int): DateTime`, so the `years` argument must be "
        "an `int` — " + std::string(kUint));
}

// ── CONTROLS: the admissible arguments keep compiling, with their values ───

// §5.1's widening table: `uint -> long` is permitted ("Widening from
// unsigned to a strictly larger signed type ... is permitted because no
// value is lost"), so the `uint` must be ADMITTED and correct.
BEZEL_TEST(sema_builtin_class_call_admission_7443,
           timespan_fromticks_uint_control) {
    expectStdout(
        "",
        "var ts := TimeSpan.FromTicks(4000000000U); "
        "Console.WriteLine(ts.Ticks);",
        "4000000000\n");
}

// The admitting twin: a `long` literal.
BEZEL_TEST(sema_builtin_class_call_admission_7443,
           timespan_fromticks_long_control) {
    expectStdout(
        "",
        "var ts := TimeSpan.FromTicks(4000000000L); "
        "Console.WriteLine(ts.Ticks);",
        "4000000000\n");
}

// §2.5 declares `FromDays(days: double)` — a `double` is the declared type
// there, and it must keep compiling.
BEZEL_TEST(sema_builtin_class_call_admission_7443,
           timespan_fromdays_double_control) {
    expectStdout(
        "",
        "var ts := TimeSpan.FromDays(1.5); Console.WriteLine(ts.Ticks);",
        "1296000000000\n");
}

// §2.5 declares `AddDays(days: double)` — an `int` widens to it.
BEZEL_TEST(sema_builtin_class_call_admission_7443,
           datetime_adddays_int_control) {
    expectStdout(
        "",
        "var d := DateTime.FromTicks(0L); var e := d.AddDays(1); "
        "Console.WriteLine(e.Ticks);",
        "864000000000\n");
}

// §2.5 declares `AddYears(years: int)` — an `int` is the declared type.
BEZEL_TEST(sema_builtin_class_call_admission_7443,
           datetime_addyears_int_control) {
    expectStdout(
        "",
        "var d := DateTime.FromTicks(0L); var e := d.AddYears(1); "
        "Console.WriteLine(e.Ticks);",
        "315360000000000\n");
}
