// Fix #4360: TaskScope.CancelAfter + await using disposal causes UAF —
// the timer callback captures sc->state and fires after the scope is freed.
//
// Bug: bz_task_scope_dispose_async does NOT bump sc->state->timer_gen before
// freeing the scope (compare with bz_cts_dispose which bumps timer_gen so
// pending CancelAfter timers no-op on a disposed CTS).
//
// Scenario (no children):
//   1. CancelAfter(1ms) → timer retains sc->state (rc=2)
//   2. Join() completes (pending==0, no drain)
//   3. await using disposal → signals state, releases it (rc=1), frees scope
//   4. timer fires (after disposal) → accesses freed sc->state → UAF
//
// To keep a child alive until after Join so the drain is non-trivial:
//   - Spawn a child that completes quickly
//   - Join() waits for it, returns
//   - CancelAfter(1ms) scheduled
//   - await using scope-end disposal → DisposeAsync (no children, completes inline)
//   - Task.Delay(50) → timer fires during/after DisposeAsync → UAF
//
// Test: spawn + join + cancel_after + await using dispose + delay.
// ASan detects UAF on the timer callback's signal+release.
//
// stdlib §6.10 (v0.175, #808) / spec §30.14 / #4360.

#include "support/aot_support.hpp"   // runAot
#include "support/codegen_support.hpp"
#include "support/run_under_asan.hpp"  // runUnderAsan
#include "test_harness.hpp"
#include "native_test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

// ASan probe: 0 = clean; 1 = ASan/LSan fired; -1 = infra unavailable (SKIP).
int runUnderAsan(const std::string& source) {
    return bezel::test::runUnderAsan(source, "bezel_4360_asan");
}

}  // namespace

// stdlib §6.10 / spec §30.14 / #4360: CancelAfter after Join(), then scope
// disposal via await using — the timer fires into freed sc->state.
// ASan must detect the UAF on the timer callback's bz_cancel_state_signal.
BEZEL_TEST(task_scope_cancel_after_dispose_4360,
           cancel_after_after_join_then_dispose_scope_uae) {
    int r = runUnderAsan(
        "module M; import System.Threading;"
        "class C "
        "  public static async method Test(): Task "
        "    await using var scope := new TaskScope(); "
        "    scope.Spawn(async lambda() => do await Task.Delay(1); end); "
        "    await scope.Join(); "
        "    scope.CancelAfter(new TimeSpan(10000)); "   // 1ms, fires after DisposeAsync
        "    await Task.Delay(50); "                  // enough time for timer to fire
        "  end Test; "
        "  public static async method Main(): Task<int> "
        "    await C.Test(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (r != -1) BEZEL_EXPECT_EQ(r, 0);
}

// stdlib §6.10 / spec §30.14: await using TaskScope with a child completes
// after Join — verifies basic spawn+join correctness.
BEZEL_TEST(task_scope_cancel_after_dispose_4360,
           await_using_task_scope_spawn_join_returns) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Threading;"
        "class C "
        "  public static async method Main(): Task<int> "
        "    await using var scope := new TaskScope(); "
        "    scope.Spawn(async lambda() => do await Task.Delay(1); end); "
        "    await scope.Join(); "
        "    return 42; "
        "  end Main; "
        "end C;"), 42);
}
