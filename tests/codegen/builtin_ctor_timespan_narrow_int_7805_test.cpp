// Spec-derived regression test for issue #7805 — `new TimeSpan(ticks: long)`
// with an UNSIGNED narrow argument sign-extended it, so `new TimeSpan(b)` with
// `b: byte := 200` read -56 ticks instead of 200 (campaign
// builtin-call-admission, lane R41).
//
// ─── THE NORMATIVE SENTENCES (CHECKED in docs/, v0.355) ─────────────────────
//
// stdlib §2.5 declares `TimeSpan`'s constructor as `constructor(ticks: long)` —
// the RAW tick count ("10,000,000 to the second"). The one-argument form is
// therefore a §5.1 implicit WIDENING of whatever integer the caller passes.
//
// spec §5.1 (Implicit conversions): a narrower integer type widens to a wider
//   one PRESERVING THE VALUE, and the widening follows the SOURCE's own
//   signedness. `byte` 200 is 200; `ushort` 65535 is 65535; `uint`
//   4294967295 is 4294967295. A SIGNED source keeps its sign: `sbyte` -1 is
//   -1, `int` -5 is -5.
//
// #7863 (the house rule, stated in the `ctorIntTo` helper's own comment in
//   src/codegen/LlvmBackend_Exprs2.cpp): "Extension follows the ARGUMENT's own
//   signedness, so `byte` 200 reads 200 (not -56)."
//
// ─── THE DEFECT (MEASURED by the R37 C2-37 lane) ───────────────────────────
//
// The ONE-argument `TimeSpan` arm in the NewExpr lowering sign-extended ANY
// non-i64 argument (`builder_.CreateSExt(v, i64())`), so an unsigned narrow
// source read as negative: `byte` 200 (0xC8) sign-extended from 8 bits to 64
// is 0xFFFFFFFFFFFFFFC8 = -56.
//
// The THREE-argument `TimeSpan` arm, TWENTY LINES ABOVE in the SAME `if
// (colName == "TimeSpan")` block, already used the shared `ctorIntTo` helper
// and was therefore already CORRECT — this file pins that twin too, so a
// "fix" that regresses it is caught here.
//
// #7784 added `ctorIntToI32` for the DateTime / DateOnly / TimeOnly arms
// ONLY — those arms are NOT this issue's scope and are pinned here as
// controls.

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotOut;

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

// runAotChecked returns ran=false on any sanitizer fire (ASan use-after-free /
// LSan leak); map that to a sentinel so a single EXPECT_EQ pins BOTH memory-
// cleanliness AND the computed value.
int runChecked(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// ── #7805: the UNSIGNED narrow argument is ZERO-extended ────────────────────

// The issue's exact program: `byte` 200 is 200 ticks, not -56.
BEZEL_TEST(builtin_ctor_timespan_narrow_int_7805, byte_200_is_200_ticks) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var b: byte := 200; "
        "    var ts := new TimeSpan(b); "
        "    Console.WriteLine(ts.Ticks);")),
        "200");
}

// The issue's exact program, verbatim — the DateTime line is byte-identical.
BEZEL_TEST(builtin_ctor_timespan_narrow_int_7805, issue_repro_program) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; import System; "
        "class C "
        "  public static method Main(): int "
        "    var b: byte := 200; "
        "    var ts := new TimeSpan(b); "
        "    Console.WriteLine(ts.Ticks); "
        "    var dt := new DateTime(2020, 1, 1, 0, 0, 0, 0, DateTimeKind.Utc); "
        "    Console.WriteLine(dt.Kind); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "200\nSystem.DateTimeKind");
}

// `ushort` 65535 is 65535 ticks, not -1.
BEZEL_TEST(builtin_ctor_timespan_narrow_int_7805, ushort_65535_is_65535_ticks) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var u: ushort := 65535; "
        "    var ts := new TimeSpan(u); "
        "    Console.WriteLine(ts.Ticks);")),
        "65535");
}

// `uint` 4294967295 is 4294967295 ticks, not -1.
BEZEL_TEST(builtin_ctor_timespan_narrow_int_7805, uint_max_is_4294967295_ticks) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var u: uint := 4294967295; "
        "    var ts := new TimeSpan(u); "
        "    Console.WriteLine(ts.Ticks);")),
        "4294967295");
}

// ── THE CONTROLS: a SIGNED source keeps its sign (§5.1) ────────────────────

// `int` -5 is -5 ticks — the signed path must still sign-extend.
BEZEL_TEST(builtin_ctor_timespan_narrow_int_7805, int_minus_5_control) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var s: int := -5; "
        "    var ts := new TimeSpan(s); "
        "    Console.WriteLine(ts.Ticks);")),
        "-5");
}

// `sbyte` -1 is -1 ticks — a narrow SIGNED source is sign-extended.
BEZEL_TEST(builtin_ctor_timespan_narrow_int_7805, sbyte_minus_1_control) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var s: sbyte := -1; "
        "    var ts := new TimeSpan(s); "
        "    Console.WriteLine(ts.Ticks);")),
        "-1");
}

// `short` -300 is -300 ticks.
BEZEL_TEST(builtin_ctor_timespan_narrow_int_7805, short_minus_300_control) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var s: short := -300; "
        "    var ts := new TimeSpan(s); "
        "    Console.WriteLine(ts.Ticks);")),
        "-300");
}

// `long` -5 is -5 ticks — a 64-bit source skips the extension entirely.
BEZEL_TEST(builtin_ctor_timespan_narrow_int_7805, long_minus_5_control) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var l: long := -5; "
        "    var ts := new TimeSpan(l); "
        "    Console.WriteLine(ts.Ticks);")),
        "-5");
}

// A `long` beyond the 32-bit range is exact — no truncation on the way in.
BEZEL_TEST(builtin_ctor_timespan_narrow_int_7805, long_large_is_exact) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var l: long := 9000000000000000000L; "
        "    var ts := new TimeSpan(l); "
        "    Console.WriteLine(ts.Ticks);")),
        "9000000000000000000");
}

// ── THE THREE-ARGUMENT TWIN (stdlib §2.5, #7899) — MUST NOT CHANGE ────────

// `new TimeSpan(hours, minutes, seconds)` = 1h + 2m + 3s = 37230000000 ticks.
// This arm was ALREADY correct (it used `ctorIntTo`); it is pinned so that a
// fix to the one-argument arm cannot regress it.
BEZEL_TEST(builtin_ctor_timespan_narrow_int_7805, three_arg_twin_components) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var ts := new TimeSpan(1, 2, 3); "
        "    Console.WriteLine(ts.Ticks);")),
        "37230000000");
}

// The three-argument twin with UNSIGNED narrow components is ZERO-extended
// too — the same house rule, already in force in that arm.
BEZEL_TEST(builtin_ctor_timespan_narrow_int_7805, three_arg_twin_narrow_unsigned) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var b: byte := 200; var u: ushort := 60000; "
        "    var ts := new TimeSpan(b, u, b); "
        "    Console.WriteLine(ts.Ticks);")),
        "43202000000000");
}

// ── THE DateTime / DateOnly / TimeOnly CONTROLS (#7784) — MUST NOT CHANGE ──

// `DateTime`'s `Kind` is unchanged by the #7805 fix (that arm uses
// `ctorIntToI32` since #7784 and is NOT this issue's scope).
BEZEL_TEST(builtin_ctor_timespan_narrow_int_7805, datetime_kind_control) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var dt := new DateTime(2020, 1, 1, 0, 0, 0, 0, DateTimeKind.Utc); "
        "    Console.WriteLine(dt.Kind);")),
        "System.DateTimeKind");
}

// `DateOnly` / `TimeOnly` are unchanged too.
BEZEL_TEST(builtin_ctor_timespan_narrow_int_7805, dateonly_timeonly_control) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "    var d := new DateOnly(2020, 1, 1); "
        "    Console.WriteLine(d.ToString()); "
        "    var t := new TimeOnly(1, 2, 3); "
        "    Console.WriteLine(t.ToString());")),
        "2020-01-01\n01:02:03");
}

// ── THE FAMILY: the other built-in constructor arms are UNCHANGED ──────────

// The one-argument capacity/seed arms of the OTHER built-in constructors
// already extend by the argument's own signedness (#7863). This row pins
// that the #7805 fix is a ONE-ARM fix and did not widen.
BEZEL_TEST(builtin_ctor_timespan_narrow_int_7805, sibling_ctor_arms_unchanged) {
    BEZEL_EXPECT_EQ(runAotOut(
        "module M; import System; import System.Collections.Generic; "
        "import System.Threading; "
        "class C "
        "  public static method Main(): int "
        "    var b: byte := 200; "
        "    using var a := new Array<int>(b); "
        "    Console.WriteLine(a.Capacity); "
        "    var semOk := 0; "
        "    try using var s := new Semaphore(b); semOk := 1; "
        "    catch ArgumentException => do semOk := -1; end end; "
        "    Console.WriteLine(semOk); "
        "    var u: uint := 4000000000; "
        "    var r1 := new Random(u); var r2 := new Random(4000000000L); "
        "    Console.WriteLine(r1.Next() = r2.Next()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "200\n1\nTrue");
}

// ── M10: no new leak / no new UAF / no new abort (ASan-clean) ─────────────

// The whole matrix, compiled with the generated code under AddressSanitizer
// and LeakSanitizer: a clean run is part of the fix's contract.
BEZEL_TEST(builtin_ctor_timespan_narrow_int_7805, asan_clean_matrix) {
    BEZEL_EXPECT_EQ(runChecked(prog(
        "    var b: byte := 200; var u: ushort := 65535; "
        "    var w: uint := 4294967295; var s: int := -5; "
        "    var sb: sbyte := -1; var l: long := -5; "
        "    Console.WriteLine(new TimeSpan(b).Ticks); "
        "    Console.WriteLine(new TimeSpan(u).Ticks); "
        "    Console.WriteLine(new TimeSpan(w).Ticks); "
        "    Console.WriteLine(new TimeSpan(s).Ticks); "
        "    Console.WriteLine(new TimeSpan(sb).Ticks); "
        "    Console.WriteLine(new TimeSpan(l).Ticks); "
        "    Console.WriteLine(new TimeSpan(1, 2, 3).Ticks); "
        "    Console.WriteLine(new DateTime(2020, 1, 1, 0, 0, 0, 0, "
        "                             DateTimeKind.Utc).Kind); "
        "    return 0; ")),
        0);
}

// The ASan run of the issue's exact program: 200 ticks, then the Kind line.
BEZEL_TEST(builtin_ctor_timespan_narrow_int_7805, asan_clean_issue_repro) {
    auto r = runAotChecked(
        "module M; import System; "
        "class C "
        "  public static method Main(): int "
        "    var b: byte := 200; "
        "    var ts := new TimeSpan(b); "
        "    Console.WriteLine(ts.Ticks); "
        "    var dt := new DateTime(2020, 1, 1, 0, 0, 0, 0, DateTimeKind.Utc); "
        "    Console.WriteLine(dt.Kind); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("200\nSystem.DateTimeKind") !=
                      std::string::npos);
}
