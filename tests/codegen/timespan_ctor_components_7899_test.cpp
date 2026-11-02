// Regression test for issue #7899 (campaign builtin-call-admission, lane
// r38-p17-7899): `new TimeSpan(1,2,3).ToString()` printed "00:00:00.0000001".
//
// ─── THE NORMATIVE SENTENCES (CHECKED in docs/, v0.350) ─────────────────────
//
// stdlib §2.5 (docs/Bezel_Standard_Library.md:339-381) declares the
//   `TimeSpan` record: `public readonly property Ticks: long;`, the integer
//   components `Days/Hours/Minutes/Seconds/Milliseconds: int`, the
//   `TotalDays/TotalHours/TotalMinutes/TotalSeconds/TotalMilliseconds: double`
//   family, and the static factories `FromDays/FromHours/FromMinutes/
//   FromSeconds/FromMilliseconds` plus `FromTicks(ticks: long)`.
//
// stdlib §2.5 (docs/Bezel_Standard_Library.md:420): "A **tick** is 100
//   nanoseconds — 10,000,000 to the second, 864,000,000,000 to the day. It is
//   the unit of `Ticks` on both types and the resolution of every value either
//   can hold; the `c` format's `.fffffff` tail ([§2.18]) is that unit spelled
//   out." and "the integer components (`Days`, `Hours`, `Minutes`, `Seconds`,
//   `Milliseconds`) are the span's decomposition, each within its stated range
//   and carrying the span's sign."
//
// stdlib §2.18 (docs/Bezel_Standard_Library.md:1168): the `c` constant form
//   renders `[-][d.]HH:mm:ss[.fffffff]`; "So `TimeSpan.FromMinutes(90.5)`
//   renders `01:30:30`, ... `TimeSpan.Zero` renders `00:00:00`".
//
// ─── THE DEFECT ─────────────────────────────────────────────────────────────
//
// `Emitter::lowerNew` (src/codegen/LlvmBackend_Exprs2.cpp) had ONE arm for
// `new TimeSpan(...)`: it read `ne.args[0]` and used it as the RAW TICK
// COUNT. The three-argument component form therefore built a span of ONE TICK
// — the hours argument — and dropped the minutes and the seconds SILENTLY:
//
//   `new TimeSpan(1,2,3).ToString()` printed "00:00:00.0000001",
//   `new TimeSpan(1,2,3).Ticks` was 1,
//   and every component (`Hours`/`Minutes`/`Seconds`) read 0.
//
// The fix computes the component form's tick count
// (h*36_000_000_000 + m*600_000_000 + s*10_000_000) and leaves the
// ONE-argument form (`new TimeSpan(ticks: long)`, the raw tick count)
// UNCHANGED — the raw-tick row below is that form's CONTROL.
//
// Every row's oracle is the program's OUTPUT, byte-for-byte (a wrong VALUE,
// never a crash-or-not oracle).

#include <string>

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

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

// The same shape, with the `System.Diagnostics` import `Stopwatch` needs.
std::string progDiag(const std::string& body) {
    return "module M; "
           "import System.Diagnostics; "
           "class C "
           "  public static method Main(): int " +
           body +
           "    return 0; "
           "  end Main; "
           "end C;";
}

}  // namespace

// #7899 / stdlib §2.5 + §2.18 (v0.350): the three-argument constructor's
// `ToString()` renders the component form "HH:mm:ss" — "01:02:03" for
// (1 hour, 2 minutes, 3 seconds). The pre-fix stdout was "00:00:00.0000001"
// (one tick, the hours argument read as a tick count).
BEZEL_TEST(timespan_ctor_components_7899,
           three_arg_ctor_to_string_renders_hh_mm_ss) {
    JitResult r = runAot(prog(
        "    var t: TimeSpan := new TimeSpan(1,2,3); "
        "    Console.WriteLine(t.ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "01:02:03\n");
}

// #7899 / stdlib §2.5 (v0.350): the integer components are "the span's
// decomposition" — `Hours` 1, `Minutes` 2, `Seconds` 3, `Milliseconds` 0,
// `Days` 0 — and `Ticks` is the sum in 100-ns ticks: 3723 s = 37_230_000_000.
// The pre-fix stdout was "0\n0\n0\n0\n0\n1\n" (every component 0, one tick).
BEZEL_TEST(timespan_ctor_components_7899,
           three_arg_ctor_components_decompose_the_span) {
    JitResult r = runAot(prog(
        "    var t: TimeSpan := new TimeSpan(1,2,3); "
        "    Console.WriteLine($\"{t.Days}\"); "
        "    Console.WriteLine($\"{t.Hours}\"); "
        "    Console.WriteLine($\"{t.Minutes}\"); "
        "    Console.WriteLine($\"{t.Seconds}\"); "
        "    Console.WriteLine($\"{t.Milliseconds}\"); "
        "    Console.WriteLine($\"{t.Ticks}\"); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "0\n1\n2\n3\n0\n37230000000\n");
}

// #7899 / stdlib §2.5 (v0.350) — THE CONTROL: the ONE-argument form is the
// raw tick count and the fix must not change it. `new TimeSpan(37230000000L)`
// is the same span the component form builds, so it renders "01:02:03" — and
// it did so BEFORE the fix, which is what makes it the control row.
BEZEL_TEST(timespan_ctor_components_7899,
           one_arg_ctor_is_still_the_raw_tick_count) {
    JitResult r = runAot(prog(
        "    var t: TimeSpan := new TimeSpan(37230000000L); "
        "    Console.WriteLine(t.ToString()); "
        "    Console.WriteLine($\"{t.Ticks}\"); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "01:02:03\n37230000000\n");
}

// #7899 / stdlib §2.5 (v0.350) — THE CONTROL: the sibling `DateTime`
// constructor is untouched by this fix and keeps its own rendered form.
BEZEL_TEST(timespan_ctor_components_7899,
           datetime_constructor_is_unchanged) {
    JitResult r = runAot(prog(
        "    var a: DateTime := new DateTime(2020,1,2,3,4,5); "
        "    Console.WriteLine(a.ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "2020-01-02T03:04:05\n");
}

// #7899 / stdlib §2.5 (v0.350): "`Total<Unit>` is the whole span measured in
// that unit, fractional part included" — 3723 s is 3723 s, 3_723_000 ms,
// 62.05 min, 1.0341666666666667 h and 0.043090277777777776 days. The pre-fix
// stdout was the tick fractions of a one-tick span.
BEZEL_TEST(timespan_ctor_components_7899,
           total_properties_measure_the_constructed_span) {
    JitResult r = runAot(prog(
        "    var t: TimeSpan := new TimeSpan(1,2,3); "
        "    Console.WriteLine($\"{t.TotalSeconds}\"); "
        "    Console.WriteLine($\"{t.TotalMilliseconds}\"); "
        "    Console.WriteLine($\"{t.TotalMinutes}\"); "
        "    Console.WriteLine($\"{t.TotalHours}\"); "
        "    Console.WriteLine($\"{t.TotalDays}\"); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    "3723\n3723000\n62.05\n1.0341666666666667\n"
                    "0.043090277777777776\n");
}

// #7899 / stdlib §2.5 + §7.5 (v0.350): the `.` receiver, the `!` unwrap and
// the `?.` null-conditional receiver all read the SAME span, so all three
// render "01:02:03" and the `?.` component read is 1. The `?.` form has NO
// arithmetic of its own — it lowers the same payload — so the constructor fix
// carries it too.
BEZEL_TEST(timespan_ctor_components_7899,
           dot_bang_and_nullcond_receivers_read_the_same_span) {
    JitResult r = runAot(prog(
        "    var t: TimeSpan := new TimeSpan(1,2,3); "
        "    var tn: TimeSpan? := new TimeSpan(1,2,3); "
        "    Console.WriteLine(t.ToString()); "
        "    Console.WriteLine(tn!.ToString()); "
        // stdlib §2.1: no `Console.WriteLine` overload takes a nullable
        // parameter, so the `?.` call's `string?` result goes through an
        // interpolation hole (which renders a present value as itself).
        "    Console.WriteLine($\"{tn?.ToString()}\"); "
        "    Console.WriteLine($\"{tn?.Hours}\"); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "01:02:03\n01:02:03\n01:02:03\n1\n");
}

// #7899 / stdlib §2.5 (v0.350): the constructor's span is the SAME span the
// `From<Unit>` factories build — 1 h + 2 min + 3 s is 3723 s — and the
// operators add and subtract tick counts: the sum with a one-second span
// renders "01:02:04" and the difference "01:02:02".
BEZEL_TEST(timespan_ctor_components_7899,
           ctor_span_equals_the_factory_sum_and_the_operators_apply) {
    JitResult r = runAot(prog(
        "    var t: TimeSpan := new TimeSpan(1,2,3); "
        "    var f := TimeSpan.FromHours(1.0) + TimeSpan.FromMinutes(2.0)"
        "              + TimeSpan.FromSeconds(3.0); "
        "    var one := TimeSpan.FromSeconds(1.0); "
        "    var sum := TimeSpan.FromTicks(t.Ticks) + one; "
        "    var dif := TimeSpan.FromTicks(t.Ticks) - one; "
        "    Console.WriteLine($\"{f.Ticks}\"); "
        "    Console.WriteLine($\"{t.Ticks}\"); "
        "    Console.WriteLine(sum.ToString()); "
        "    Console.WriteLine(dif.ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    "37230000000\n37230000000\n01:02:04\n01:02:02\n");
}

// #7899 / stdlib §2.5 + §9.2 (v0.350): the record's equality is tick
// equality — two constructions of the same span are equal, and a different
// span is not. The pre-fix code compared two ONE-TICK spans (both
// constructions read their HOURS argument as the tick count), so the
// different span compared EQUAL as well: the pre-fix stdout was
// "eq-same\neq-diff" and the post-fix stdout is "eq-same" alone.
BEZEL_TEST(timespan_ctor_components_7899,
           equality_compares_the_constructed_ticks) {
    JitResult r = runAot(prog(
        "    var t1: TimeSpan := new TimeSpan(1,2,3); "
        "    var t2: TimeSpan := new TimeSpan(1,2,3); "
        "    var t3: TimeSpan := new TimeSpan(1,0,0); "
        "    if t1 = t2 then Console.WriteLine(\"eq-same\"); end; "
        "    if t1 = t3 then Console.WriteLine(\"eq-diff\"); end; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "eq-same\n");
}

// #7899 / stdlib §2.5 + §2.18 (v0.350) — THE CONTROL: the static factories,
// `Zero` and the unary minus are untouched by the fix and keep the renderings
// stdlib §2.18 states ("`TimeSpan.FromMinutes(90.5)` renders `01:30:30`",
// "`TimeSpan.Zero` renders `00:00:00`", "`-TimeSpan.FromSeconds(90.0)`
// renders `-00:01:30`").
BEZEL_TEST(timespan_ctor_components_7899,
           factories_zero_and_negation_are_unchanged) {
    JitResult r = runAot(prog(
        "    Console.WriteLine(TimeSpan.Zero.ToString()); "
        "    Console.WriteLine(TimeSpan.FromHours(1.5).ToString()); "
        "    Console.WriteLine(TimeSpan.FromMinutes(90.5).ToString()); "
        "    Console.WriteLine(TimeSpan.FromDays(1.5).ToString()); "
        "    Console.WriteLine(TimeSpan.FromMilliseconds(1.5).ToString()); "
        "    Console.WriteLine((-TimeSpan.FromSeconds(90.0)).ToString()); "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText,
                    "00:00:00\n01:30:00\n01:30:30\n1.12:00:00\n"
                    "00:00:00.0015000\n-00:01:30\n");
}

// #7899 / stdlib §2.5 + §6.3 (v0.350): a `TimeSpan` reaches a timeout-taking
// stdlib member as a MILLISECOND count (`Semaphore.TryAcquire(timeout:
// TimeSpan)` — src/codegen/LlvmBackend_Calls3.cpp divides the tick value by
// 10 000). A one-second span is therefore a 1000 ms timeout, and the attempt
// on a semaphore with no permit left waits it out. The pre-fix code built a
// ONE-TICK span, so the timeout was 0 ms and the attempt returned at once.
BEZEL_TEST(timespan_ctor_components_7899,
           a_constructed_span_is_a_real_timeout_for_the_stdlib) {
    JitResult r = runAot(progDiag(
        "    using var sem := new Semaphore(1); "
        "    using var tok := sem.Acquire(); "
        "    using var sw := Stopwatch.StartNew(); "
        "    using var none := sem.TryAcquire(new TimeSpan(0,0,1)); "
        "    if sw.ElapsedMilliseconds >= 900 then "
        "      Console.WriteLine(\"timeout-honored\"); "
        "    end; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, "timeout-honored\n");
}
