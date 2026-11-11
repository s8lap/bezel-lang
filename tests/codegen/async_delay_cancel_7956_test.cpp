// Codegen + runtime: Task.Delay(ms, token) and the CANCELLATION it must
// observe — stdlib §6.7, spec §30.8 (v0.357).
//
// #7956: the token-aware delay consulted the token only at its OWN deadline,
// so a token cancelled while the delay was waiting left the delay running to
// the deadline: `Task.Delay(5000, token)` under a 1 ms `WithCancelAfter`
// finished after 5 s instead of faulting at the cancellation.  Spec §30.8
// makes the convention explicit — "cancellation typically happens at await
// points: the inner call observes the request and completes with
// OperationCanceledException" — and stdlib §6.7 gives `Task.Delay` a
// `CancellationToken` parameter, so a pending delay must be COMPLETED by the
// cancellation, not merely re-checked when its deadline arrives.
//
// The rows measure the OBSERVATION, not just the fault: each program prints
// the elapsed milliseconds, so the pre-fix behaviour (the fault raised at the
// delay's own deadline, ~5000 ms) is distinguishable from the fixed behaviour
// (the fault raised at the cancellation, ~1 ms).  The elapsed value is read
// back from the program's stdout — the value channel for an observable that
// does not fit an 8-bit exit code.
//
// Rows:
//   A  the cancellation is observed at the cancellation (int overload)  — the bug
//   B  the same, through the TimeSpan overload of Delay
//   C  the same, uncaught — the OCE is the program's failure
//   D  CancellationToken.None — the delay runs to its deadline
//   E  a token that is never cancelled — the delay runs to its deadline
//   F  a token cancelled BEFORE the delay — the delay faults at once
//   G  a linked parent cancelled by its own deadline — the cascade reaches the delay
//   H  the source is disposed while the delay is pending — dispose is a full
//      Cancel() (stdlib §6.1), so the pending delay observes it
//   I  Task.Delay without a token — untouched by the fix
//   J  the cancellation observed 100× in a row (leak / double-completion pin)

#include <cstdlib>
#include <string>

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration)
#include "support/codegen_support.hpp"
#include "support/run_under_asan.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// The program prints `elapsed=<n>`; read the value back (-1 when absent).
long elapsedMs(const std::string& out) {
    const std::string kNeedle = "elapsed=";
    auto pos = out.find(kNeedle);
    if (pos == std::string::npos) return -1;
    return std::strtol(out.c_str() + pos + kNeedle.size(), nullptr, 10);
}

// Native AddressSanitizer / LeakSanitizer probe: the RUNTIME is instrumented
// here (the generated-code ASan sweep does not instrument the runtime), so
// this is the leg that sees a use-after-free in the cancellation path — the
// pending delay's task, its OperationCanceledException and the runtime's
// waiter record all live across the cancellation.
// Returns 0 = clean; 1 = sanitizer fired or the wrong exit code;
// -1 = toolchain unavailable (SKIP); -2/-3/-4 = an infra failure (loud).
int runUnderAsan(const std::string& source, int expectedExitCode) {
    return bezel::test::runUnderAsanValue(source, expectedExitCode,
                                          "bezel_delay_cancel_7956_asan");
}

// A pending delay must observe the cancellation WITHIN this budget.  The
// fixed runtime observes it at the cancellation (~1 ms); the pre-fix runtime
// observed it at the delay's own deadline (5000 ms), so the two are ~5000x
// apart and the threshold is nowhere near either.
constexpr long kPromptBudgetMs = 1000;

// The module preamble: §6.1 (CancellationTokenSource), §6.7 (Task.Delay),
// §8.1 (Stopwatch, for the elapsed measurement).
const char* kPreamble =
    "module M; import System.Threading; import System.Diagnostics; ";

// Wrap an async Main body (returns Task<int>) — the delay rows all await.
std::string progAsync(const std::string& mainBody) {
    return std::string(kPreamble) +
           "class C public static async method Main(): Task<int> " + mainBody +
           " end Main; end C;";
}

} // namespace

// ===========================================================================
// A — the cancellation is observed at the cancellation (§6.7 / §30.8).
// ===========================================================================

// #7956 / stdlib §6.7 / spec §30.8: `Task.Delay(5000, token)` under a 1 ms
// `WithCancelAfter` must complete with OperationCanceledException AT THE
// CANCELLATION (the inner call observes the request), not at the delay's own
// deadline.  The catch arm runs (exit 42) and the elapsed time is the
// observation: ~1 ms fixed, ~5000 ms before the fix.
BEZEL_TEST(delay_cancel_7956, cancel_after_observed_at_the_cancellation) {
    auto r = runAot(progAsync(
        "using var sw := Stopwatch.StartNew(); "
        "using var cts := CancellationTokenSource.WithCancelAfter("
        "    TimeSpan.FromMilliseconds(1.0)); "
        "var x := 0; "
        "try "
        "  await Task.Delay(5000, cts.Token); "
        "  x := 1; "
        "catch OperationCanceledException => "
        "  x := 42; "
        "end; "
        "Console.WriteLine($\"x={x} elapsed={sw.ElapsedMilliseconds}\"); "
        "return x; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);          // the OCE was raised and caught
    long ms = elapsedMs(r.stdoutText);
    BEZEL_EXPECT_TRUE(ms >= 0);
    BEZEL_EXPECT_TRUE(ms < kPromptBudgetMs);  // observed at the cancellation
}

// #7956 / §6.7 / §30.8: the same shape through the TimeSpan overload of
// `Task.Delay` — the overload must observe the cancellation the same way.
BEZEL_TEST(delay_cancel_7956, cancel_after_observed_at_the_cancellation_timespan) {
    auto r = runAot(progAsync(
        "using var sw := Stopwatch.StartNew(); "
        "using var cts := CancellationTokenSource.WithCancelAfter("
        "    TimeSpan.FromMilliseconds(1.0)); "
        "var x := 0; "
        "try "
        "  await Task.Delay(TimeSpan.FromMilliseconds(5000), cts.Token); "
        "  x := 1; "
        "catch OperationCanceledException => "
        "  x := 42; "
        "end; "
        "Console.WriteLine($\"x={x} elapsed={sw.ElapsedMilliseconds}\"); "
        "return x; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
    long ms = elapsedMs(r.stdoutText);
    BEZEL_EXPECT_TRUE(ms >= 0);
    BEZEL_EXPECT_TRUE(ms < kPromptBudgetMs);
}

// #7956 / §6.7 / §30.8: with no handler the cancellation is the program's
// failure — the unhandled OperationCanceledException exits 70 (the fault is
// raised at the cancellation, so the program does not sit on the delay's
// deadline).
BEZEL_TEST(delay_cancel_7956, cancel_after_fault_is_uncaught) {
    auto r = runAot(progAsync(
        "using var cts := CancellationTokenSource.WithCancelAfter("
        "    TimeSpan.FromMilliseconds(1.0)); "
        "await Task.Delay(5000, cts.Token); "
        "return 5; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 70);
}

// ===========================================================================
// B — the controls: the shapes around the fix must be UNCHANGED.
// ===========================================================================

// §6.7 / §30.8: CancellationToken.None never cancels, so the delay runs to
// its deadline and returns normally (exit 5).
BEZEL_TEST(delay_cancel_7956, none_token_delay_runs_to_its_deadline) {
    auto r = runAot(progAsync(
        "await Task.Delay(5, CancellationToken.None); "
        "return 5; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// §6.7 / §30.8: a token that is never cancelled leaves the delay alone — the
// delay runs to its deadline and returns normally (exit 6).
BEZEL_TEST(delay_cancel_7956, never_cancelled_token_delay_runs_to_its_deadline) {
    auto r = runAot(progAsync(
        "using var cts := new CancellationTokenSource(); "
        "var tok := cts.Token; "
        "await Task.Delay(5, tok); "
        "return 6; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 6);
}

// §6.7 / §30.8: a token cancelled BEFORE the delay is called yields an
// immediately-faulted task — with no handler the OCE is unhandled (exit 70).
BEZEL_TEST(delay_cancel_7956, pre_cancelled_token_faults_immediately) {
    auto r = runAot(progAsync(
        "using var cts := new CancellationTokenSource(); "
        "cts.Cancel(); "
        "var tok := cts.Token; "
        "await Task.Delay(5, tok); "
        "return 5; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 70);
}

// §6.7 / §30.8: the same pre-cancelled delay, with a handler — the OCE is
// catchable (exit 77).
BEZEL_TEST(delay_cancel_7956, pre_cancelled_fault_is_catchable) {
    auto r = runAot(progAsync(
        "using var cts := new CancellationTokenSource(); "
        "cts.Cancel(); "
        "var tok := cts.Token; "
        "var x := 0; "
        "try "
        "  await Task.Delay(5, tok); "
        "  x := 5; "
        "catch OperationCanceledException => "
        "  x := 77; "
        "end; "
        "return x; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 77);
}

// §6.1 / §30.8: a LINKED source is cancelled when its parent is (§6.1: "a
// parent already cancelled at creation cancels the linked source ... the
// linked source enters the cancelled state when ANY parent token cancels");
// the cascade must reach the pending delay, which observes it at the
// cancellation (exit 42, elapsed ~1 ms).
BEZEL_TEST(delay_cancel_7956, linked_parent_cancellation_reaches_the_delay) {
    auto r = runAot(progAsync(
        "using var sw := Stopwatch.StartNew(); "
        "using var parent := CancellationTokenSource.WithCancelAfter("
        "    TimeSpan.FromMilliseconds(1.0)); "
        "using var child := CancellationTokenSource.CreateLinked("
        "    parent.Token, CancellationToken.None); "
        "var x := 0; "
        "try "
        "  await Task.Delay(5000, child.Token); "
        "  x := 1; "
        "catch OperationCanceledException => "
        "  x := 42; "
        "end; "
        "Console.WriteLine($\"x={x} elapsed={sw.ElapsedMilliseconds}\"); "
        "return x; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
    long ms = elapsedMs(r.stdoutText);
    BEZEL_EXPECT_TRUE(ms >= 0);
    BEZEL_EXPECT_TRUE(ms < kPromptBudgetMs);
}

// §6.1: "Disposal performs a full Cancel(): registered callbacks that have
// not yet run are invoked synchronously on the disposing thread".  A delay
// waiting on that token observes the disposal like any other cancellation —
// it completes with OperationCanceledException (exit 42), it does not run to
// its deadline.
BEZEL_TEST(delay_cancel_7956, source_disposal_cancels_a_pending_delay) {
    auto r = runAot(progAsync(
        "using var sw := Stopwatch.StartNew(); "
        "using var cts := new CancellationTokenSource(); "
        "using var t := Task.Delay(5000, cts.Token); "
        "cts.Dispose(); "
        "var x := 0; "
        "try "
        "  await t; "
        "  x := 1; "
        "catch OperationCanceledException => "
        "  x := 42; "
        "end; "
        "Console.WriteLine($\"x={x} elapsed={sw.ElapsedMilliseconds}\"); "
        "return x; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
    long ms = elapsedMs(r.stdoutText);
    BEZEL_EXPECT_TRUE(ms >= 0);
    BEZEL_EXPECT_TRUE(ms < kPromptBudgetMs);
}

// §6.7: `Task.Delay(ms)` without a token has no cancellation to observe —
// the fix must not touch it (exit 8).
BEZEL_TEST(delay_cancel_7956, delay_without_a_token_runs_normally) {
    auto r = runAot(progAsync(
        "await Task.Delay(5); "
        "return 8; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 8);
}

// ===========================================================================
// C — the cancellation observed 100× in a row.
//
// The delay task, its OperationCanceledException and the runtime's waiter
// record are all released on the cancellation path; a leak or a double
// completion shows up here (and under ASan, which the lane runs for this
// fix: the waiter record is a new heap allocation whose lifetime spans the
// cancellation and the delay's deadline).
// ===========================================================================

BEZEL_TEST(delay_cancel_7956, repeated_cancellation_is_clean) {
    auto r = runAot(progAsync(
        "var n := 0; "
        "for var i in 0..<100 do "
        "  using var cts := CancellationTokenSource.WithCancelAfter("
        "      TimeSpan.FromMilliseconds(1.0)); "
        "  try "
        "    await Task.Delay(100, cts.Token); "
        "  catch OperationCanceledException => "
        "    n := n + 1; "
        "  end; "
        "end for; "
        "Console.WriteLine($\"cancelled={n}\"); "
        "return n; "));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 100);   // every iteration observed the cancellation
}

// ===========================================================================
// D — the native ASan/LSan probe.
//
// The generated-code ASan sweep (BEZEL_AOT_FORCE_SANITIZE=address) does NOT
// instrument the runtime; this native probe does.  The program observes 20
// cancellations in a row: every pending delay is completed by the cancellation
// path while its deadline timer entry is still pending, so the waiter record
// and the delay task are released from two different holders — no leak, no
// use-after-free.
// ===========================================================================

BEZEL_TEST(delay_cancel_7956_asan, cancelled_delays_no_leak_no_uaf) {
    int a = runUnderAsan(progAsync(
        "var n := 0; "
        "for var i in 0..<20 do "
        "  using var cts := CancellationTokenSource.WithCancelAfter("
        "      TimeSpan.FromMilliseconds(1.0)); "
        "  try "
        "    await Task.Delay(200, cts.Token); "
        "  catch OperationCanceledException => "
        "    n := n + 1; "
        "  end; "
        "end for; "
        "return n; "), 20);
    BEZEL_EXPECT_TRUE(a >= 0);   // -1 = ASan runtime missing: a FAIL, never a SKIP (#3682)
    BEZEL_EXPECT_EQ(a, 0);
}
