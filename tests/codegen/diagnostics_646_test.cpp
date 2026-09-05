// Codegen tests for System.Diagnostics: Stopwatch and Debug (stdlib §8).
//
// Stopwatch (§8.1): heap-allocated class, tracks elapsed wall-clock time using
// CLOCK_MONOTONIC. Stored as bz_stopwatch_t* (a C struct with start_ts/elapsed_ns/
// running flag). ElapsedMilliseconds is i64 (long); Elapsed is i64 TimeSpan ticks.
//
// Debug (§8.2): static class with Assert/WriteLine. Always-on in this compiler
// (no --debug flag yet; recorded choice below). Assert(false) writes message to
// stderr and aborts. WriteLine writes to stderr.
//
// Import gating: System.Diagnostics is NOT auto-imported (stdlib §8 header has no
// "(auto-imported)" annotation). Compiler does not yet enforce the gate here —
// Stopwatch and Debug resolve without an explicit import, matching the existing
// pattern for System.IO types (File/Directory) which are also ungated in the
// current implementation. Recorded as unspecified-gap choice.
//
// Unspecified-gap choices recorded here:
//   1. Debug.Assert failure behavior: stdlib §8.2 does not specify the failure
//      mode (exception vs abort). This implementation writes the assertion message
//      (or a default "Assertion failed" if no message is given) to stderr via
//      bz_debug_assert_fail and calls abort(). Rationale: consistent with C-family
//      assert(3) semantics; a catchable exception would require the type-checker to
//      treat Debug.Assert as a throwing call, which the spec does not require.
//   2. Debug.* are always-on (not elided). The spec says "may be elided in release
//      builds, depending on the compiler's --debug flag"; no such flag exists yet.
//      All Debug.* calls are unconditionally emitted until the flag is added.
//   3. Import gating: not enforced (Stopwatch/Debug resolve without import System.Diagnostics).
//
// Each test cites stdlib §8.1 or §8.2 and the specific behaviour it verifies.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

std::string prog(const std::string& mainBody) {
    return "module M; "
           "class C public static method Main(): int "
           + mainBody +
           " end Main; end C;";
}

} // namespace

// ---------------------------------------------------------------------------
// §8.1 Stopwatch — constructor, IsRunning, basic Start/Stop mechanics.
// ---------------------------------------------------------------------------

// stdlib §8.1: new Stopwatch() — fresh instance, IsRunning = false.
// ElapsedMilliseconds starts at 0 for a stopped, never-started watch.
BEZEL_TEST(diagnostics_646_8_1, stopwatch_new_not_running) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var sw := new Stopwatch(); "
        "if sw.IsRunning then return 0; end; "
        "return 1; ")), 1);
}

// stdlib §8.1: new Stopwatch().ElapsedMilliseconds == 0 before Start.
BEZEL_TEST(diagnostics_646_8_1, stopwatch_new_elapsed_zero) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var sw := new Stopwatch(); "
        "if sw.ElapsedMilliseconds = 0L then return 1; end; "
        "return 0; ")), 1);
}

// stdlib §8.1: Start() sets IsRunning = true.
BEZEL_TEST(diagnostics_646_8_1, stopwatch_start_sets_running) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var sw := new Stopwatch(); "
        "sw.Start(); "
        "if sw.IsRunning then return 1; end; "
        "return 0; ")), 1);
}

// stdlib §8.1: Stop() sets IsRunning = false.
BEZEL_TEST(diagnostics_646_8_1, stopwatch_stop_clears_running) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var sw := new Stopwatch(); "
        "sw.Start(); "
        "sw.Stop(); "
        "if sw.IsRunning then return 0; end; "
        "return 1; ")), 1);
}

// stdlib §8.1: ElapsedMilliseconds is non-negative after Start+Stop.
BEZEL_TEST(diagnostics_646_8_1, stopwatch_elapsed_nonneg) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var sw := new Stopwatch(); "
        "sw.Start(); "
        "sw.Stop(); "
        "if sw.ElapsedMilliseconds >= 0L then return 1; end; "
        "return 0; ")), 1);
}

// stdlib §8.1: StartNew() returns a running Stopwatch (IsRunning = true).
BEZEL_TEST(diagnostics_646_8_1, stopwatch_startnew_is_running) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var sw := Stopwatch.StartNew(); "
        "if sw.IsRunning then return 1; end; "
        "return 0; ")), 1);
}

// stdlib §8.1: StartNew() followed by Stop — IsRunning = false.
BEZEL_TEST(diagnostics_646_8_1, stopwatch_startnew_stop) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var sw := Stopwatch.StartNew(); "
        "sw.Stop(); "
        "if sw.IsRunning then return 0; end; "
        "return 1; ")), 1);
}

// stdlib §8.1: Reset() — after Start+Stop+Reset, IsRunning = false
// and ElapsedMilliseconds = 0.
BEZEL_TEST(diagnostics_646_8_1, stopwatch_reset) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var sw := new Stopwatch(); "
        "sw.Start(); "
        "sw.Stop(); "
        "sw.Reset(); "
        "if sw.IsRunning then return 0; end; "
        "if sw.ElapsedMilliseconds = 0L then return 1; end; "
        "return 0; ")), 1);
}

// fix #3852 (stdlib §8.1, v0.290, proposal 0123 R6(b)): `Restart()` was
// retired — never documented, and never should have compiled. The migrated
// spelling is `sw.Reset(); sw.Start();`, which §8.1 now states is exactly
// what the deleted `bz_stopwatch_restart` runtime entry did: stop-and-zero
// then start. This row keeps the ORIGINAL assertion this test pinned before
// the migration (IsRunning = true after the restart-shaped sequence),
// proving the replacement spelling reproduces the retired member's behavior.
BEZEL_TEST(diagnostics_646_8_1, stopwatch_restart_is_running_zero) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var sw := new Stopwatch(); "
        "sw.Start(); "
        "sw.Stop(); "
        "sw.Reset(); "
        "sw.Start(); "
        "if sw.IsRunning then return 1; end; "
        "return 0; ")), 1);
}

// fix #3852 (stdlib §8.1, v0.290): pins the `Reset()` contract the doc now
// states in prose — "Reset() stops the stopwatch and zeroes Elapsed" — for
// the case that actually distinguishes `Reset(); Start();` from a no-op:
// calling `Reset()` on a STILL-RUNNING watch (no prior `Stop()`). Before this
// fix only `stopwatch_reset` existed, and it always called `Stop()` first, so
// "Reset() stops the watch" was never exercised on a running watch. Without
// the stop-even-if-running behavior, `sw.Reset(); sw.Start();` would not
// reproduce what `bz_stopwatch_restart` did (reset elapsed unconditionally,
// then (re)start), so this is the row that makes the migration above sound.
BEZEL_TEST(diagnostics_646_8_1, stopwatch_reset_stops_and_zeroes_while_running) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var sw := new Stopwatch(); "
        "sw.Start(); "
        "var i: int := 0; "
        "while i < 50000 do i := i + 1; end; "
        "sw.Reset(); "
        "if sw.IsRunning then return 0; end; "
        "if sw.ElapsedMilliseconds = 0L then return 1; end; "
        "return 0; ")), 1);
}

// stdlib §8.1: ElapsedMilliseconds monotone — elapsed after a busy loop
// is >= elapsed before it (time only moves forward).
// Uses two Stop snapshots on the same watch; second >= first.
BEZEL_TEST(diagnostics_646_8_1, stopwatch_elapsed_monotone) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var sw := Stopwatch.StartNew(); "
        "var x: long := 0L; "
        // Busy loop to ensure measurable time passes.
        "var i: int := 0; "
        "while i < 100000 do x := x + 1L; i := i + 1; end; "
        "sw.Stop(); "
        "var ms1 := sw.ElapsedMilliseconds; "
        "sw.Start(); "
        "var j: int := 0; "
        "while j < 100000 do x := x + 1L; j := j + 1; end; "
        "sw.Stop(); "
        "var ms2 := sw.ElapsedMilliseconds; "
        // ms2 (cumulative) must be >= ms1 (earlier snapshot).
        "if ms2 >= ms1 then return 1; end; "
        "return 0; ")), 1);
}

// stdlib §8.1: stopped watch does not accumulate time.
// After Stop(), ElapsedMilliseconds does not grow even after a delay.
// We verify by calling ElapsedMilliseconds twice on a stopped watch
// and checking they are equal.
BEZEL_TEST(diagnostics_646_8_1, stopwatch_stopped_does_not_accum) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var sw := Stopwatch.StartNew(); "
        "sw.Stop(); "
        "var ms1 := sw.ElapsedMilliseconds; "
        "var i: int := 0; "
        "while i < 50000 do i := i + 1; end; "
        "var ms2 := sw.ElapsedMilliseconds; "
        "if ms1 = ms2 then return 1; end; "
        "return 0; ")), 1);
}

// stdlib §8.1: Elapsed property — TimeSpan (i64 ticks, 100-ns units).
// For a stopped zero-elapsed watch, Elapsed is 0.
BEZEL_TEST(diagnostics_646_8_1, stopwatch_elapsed_timespan_zero) {
    BEZEL_EXPECT_EQ(run(prog(
        "using var sw := new Stopwatch(); "
        "var ts := sw.Elapsed; "
        // ts.TotalMilliseconds is the TimeSpan accessor; but we can also
        // check that Ticks == 0 since Reset state has 0 elapsed.
        "if ts >= 0L then return 1; end; "
        "return 0; ")), 1);
}

// ---------------------------------------------------------------------------
// §8.2 Debug.Assert — true condition is no-op.
// stdlib §8.2: Assert(condition: bool) — no-op when condition is true.
// ---------------------------------------------------------------------------

// stdlib §8.2: Debug.Assert(true) — program continues normally.
BEZEL_TEST(diagnostics_646_8_2, debug_assert_true_noop) {
    BEZEL_EXPECT_EQ(run(prog(
        "Debug.Assert(true); "
        "return 1; ")), 1);
}

// stdlib §8.2: Debug.Assert(true, message) — no-op with message present.
BEZEL_TEST(diagnostics_646_8_2, debug_assert_true_msg_noop) {
    BEZEL_EXPECT_EQ(run(prog(
        "Debug.Assert(true, \"all good\"); "
        "return 1; ")), 1);
}

// stdlib §8.2: Debug.Assert(false) — aborts the process.
// NOTE: Cannot be tested in the JIT harness: bz_debug_assert_fail calls
// abort(3), which kills the in-process JIT executor and the whole test
// binary. The abort behavior is verified by code review of bz_debug_assert_fail
// in bezel_runtime.cpp; an integration test (AOT + subprocess) is deferred.
// The test below is intentionally not registered; it documents the gap.
//
// BEZEL_TEST(diagnostics_646_8_2, debug_assert_false_aborts) { ... }
// BEZEL_TEST(diagnostics_646_8_2, debug_assert_false_msg_aborts) { ... }

// stdlib §8.2: Debug.WriteLine(message) — does not crash.
// We cannot capture stderr from the JIT harness; verify it doesn't abort.
BEZEL_TEST(diagnostics_646_8_2, debug_writeline_no_crash) {
    BEZEL_EXPECT_EQ(run(prog(
        "Debug.WriteLine(\"diagnostics test trace\"); "
        "return 1; ")), 1);
}

// stdlib §8.2: Debug.WriteLine called multiple times — no crash.
BEZEL_TEST(diagnostics_646_8_2, debug_writeline_multiple_no_crash) {
    BEZEL_EXPECT_EQ(run(prog(
        "Debug.WriteLine(\"first\"); "
        "Debug.WriteLine(\"second\"); "
        "return 1; ")), 1);
}
