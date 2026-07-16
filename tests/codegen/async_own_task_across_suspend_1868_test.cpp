// Codegen/runtime — issue #1868 (§30.5 / §28.12 / §28.3, v0.190 SB-01 D2):
// a plain owned TASK local held live ACROSS a suspend point — including the
// NEVER-awaited case D2 newly admits — is disposed EXACTLY ONCE at scope end,
// on every reachable terminal edge.
//
// Background. Under v0.190 SB-01, a Task-family CALL mints an OWNED task frame
// (SB01-A). A plain `var t := Producer();` in an async method is thus an owned
// task local. §30.5 line 7341 rules that such a local "keeps its ordinary §28.3
// discharge (auto-drop at scope end)". SB01-C1 (#1786) admitted this for the
// BORROW-awaited case; SB01-D2 (#1868) extends the admission to the NEVER-awaited
// case — a dangling owned task held live across a *different* await — which
// C1 alone left conservatively rejected.
//
// Why admitting it is sound (the D2 argument). An owned task local's scope-end
// discharge is the codegen TASK path (bz_task_fire_and_forget,
// LlvmBackend_Stmts.cpp #445 / LlvmBackend_Disposal.cpp's `d.task` arm),
// registered on the disposal stack and fired on EVERY edge on which the async
// frame's resume function re-enters and terminates:
//   - normal fall-off / `return`            -> emitDisposals
//   - synchronous throw / faulted-await     -> emitUnwindDisposals (then async.fault)
//   - cancellation                          -> the runtime FAULTS the awaited task
//                                              (bz_task_delay_cancelable ->
//                                              bz_task_complete_error_obj), resuming
//                                              the frame at the suspend point and
//                                              unwinding through emitUnwindDisposals.
// The frame is freed only from inside its own resume function (emitAsyncComplete
// / the async.fault boundary), AFTER those disposals run — there is no runtime
// path that frees the frame without re-entering the resume function. So an owned
// task suspend-local needs no separate frame-teardown "completion edge" disposal
// (unlike a plain owned CLASS suspend-local, which stays the §28.3 reject); its
// await-ness was never the soundness criterion.
//
// fail-without/pass-with. Pre-D2 every program below was REJECTED at compile
// time (`owned value in `t` would be dropped here without disposal`), so runAot
// never ran and each expectation (Cell.n == 0) failed. With D2 they compile,
// run, and dispose the never-awaited task's produced Cell exactly once.
//
// Verification uses the `Cell` static-counter idiom (ctor ++, dtor --): a leak
// leaves Cell.n > 0; a double-free/double-dispose underflows it or crashes.
// `Cell.n == 0` after the owning scope exits is the dispose-exactly-once
// assertion. The whole suite is additionally swept by scripts/asan_sweep.sh
// (this fix touches disposal machinery). // per §30.5, §28.12, §28.3 (v0.190)

#include "support/aot_support.hpp"   // runAot — AOT test harness
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

// A Cell with a static live-count (ctor ++, dtor --) — the standard leak /
// double-free oracle (async_owned_task_result_1127_test.cpp's kCell).
const char* kCell =
    "class Cell public static n: int := 0; "
    "  public constructor() Cell.n:=Cell.n+1; end constructor; "
    "  destructor Cell.n:=Cell.n-1; end destructor; end Cell; ";
}  // namespace

// ── D2 (a): never-awaited owned Task<Cell> held live across a DIFFERENT await,
// fast producer (already-done at scope end) — disposed exactly once. The
// `do…end` scope-ends the task (fire-and-forget disposes synchronously for a
// done task) before `return Cell.n` reads the count. // per §30.5, §28.12
BEZEL_TEST(async_own_task_across_suspend_1868, never_awaited_fast_disposed_once) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Threading; " + std::string(kCell) +
        "class C "
        "  public static async method LoadAsync(): Task<Cell> "
        "    return new Cell(); "
        "  end LoadAsync; "
        "  public static async method Main(): Task<int> "
        "    do "
        "      var t := C.LoadAsync(); "   // owned Task<Cell>, never awaited
        "      await Task.Delay(5); "       // a DIFFERENT suspend — t live across it
        "    end; "                         // scope end: t (and its Cell) disposed
        "    return Cell.n; "               // expect 0
        "  end Main; "
        "end C;"),
        0);
}

// ── D2 (a): slow producer — the never-awaited task is STILL RUNNING at scope
// end, so fire-and-forget must register a continuation that disposes the
// produced Cell when it later settles. Main waits past the producer's delay,
// then reads the count. Pins the not-yet-settled arm of §30.5's "attaches a
// continuation that disposes the produced result whenever it appears".
BEZEL_TEST(async_own_task_across_suspend_1868, never_awaited_slow_fire_and_forget_disposed) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Threading; " + std::string(kCell) +
        "class C "
        "  public static async method LoadAsyncSlow(): Task<Cell> "
        "    await Task.Delay(30); "
        "    return new Cell(); "
        "  end LoadAsyncSlow; "
        "  public static async method Main(): Task<int> "
        "    do "
        "      var t := C.LoadAsyncSlow(); "  // not done at scope end
        "      await Task.Delay(5); "
        "    end; "
        "    await Task.Delay(80); "          // let the abandoned task settle + dispose
        "    return Cell.n; "                 // expect 0
        "  end Main; "
        "end C;"),
        0);
}

// ── D2 edge: EXCEPTION path. The never-awaited owned task is live across a
// suspend when a later `throw` unwinds the frame. emitUnwindDisposals must
// dispose it on the way to the async.fault boundary — exactly once, no
// double-free with the frame free. // per §28.3 exception-path discharge
BEZEL_TEST(async_own_task_across_suspend_1868, never_awaited_throw_edge_disposed_once) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Threading; " + std::string(kCell) +
        "class C "
        "  public static async method LoadAsync(): Task<Cell> "
        "    return new Cell(); "
        "  end LoadAsync; "
        "  public static async method Worker(): Task<int> "
        "    var t := C.LoadAsync(); "        // never awaited
        "    await Task.Delay(5); "            // suspend — t live across it
        "    throw new InvalidOperationException(\"boom\"); "  // unwinds the frame
        "  end Worker; "
        "  public static async method Main(): Task<int> "
        "    try "
        "      var r := await C.Worker(); "
        "      _ := r; "
        "    catch Exception => _ := Cell.n; "
        "    end; "
        "    return Cell.n; "                  // expect 0 (t disposed on unwind)
        "  end Main; "
        "end C;"),
        0);
}

// ── D2 edge: CANCELLATION path. The never-awaited owned task is live when the
// frame is cancelled at a later `await Task.Delay(long, token)`. The runtime
// delivers cancellation as a fault on the delay task, resuming the frame and
// unwinding through emitUnwindDisposals — which disposes the abandoned task
// exactly once. This is the §30.8 cancellation edge D2's admission relies on.
BEZEL_TEST(async_own_task_across_suspend_1868, never_awaited_cancellation_edge_disposed_once) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Threading; " + std::string(kCell) +
        "class C "
        "  public static async method LoadAsync(): Task<Cell> "
        "    return new Cell(); "
        "  end LoadAsync; "
        "  public static async method Worker(token: CancellationToken): Task<int> "
        "    var t := C.LoadAsync(); "         // never awaited
        "    await Task.Delay(10000, token); " // cancelled here — frame unwinds
        "    return 0; "
        "  end Worker; "
        "  public static async method Main(): Task<int> "
        "    using var cts := CancellationTokenSource.WithCancelAfter("
        "        TimeSpan.FromMilliseconds(20)); "
        "    try "
        "      var r := await C.Worker(cts.Token); "
        "      _ := r; "
        "    catch Exception => _ := Cell.n; "
        "    end; "
        "    return Cell.n; "                  // expect 0 (t disposed on cancel-unwind)
        "  end Main; "
        "end C;"),
        0);
}

// ── D2 (b) companion: BORROW-awaited owned task ALSO held live across a
// DIFFERENT later await (the residual case #1868 names). The await at the top is
// its use; the task then stays live across `await Task.Delay` and is disposed at
// scope end. Exactly once. Pins that the borrow-await + across-a-different-await
// combination is leak-clean, not only the never-awaited shape. // per §30.5
BEZEL_TEST(async_own_task_across_suspend_1868, borrow_awaited_then_across_different_await_disposed) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Threading; " + std::string(kCell) +
        "class C "
        "  public static async method LoadAsync(): Task<Cell> "
        "    return new Cell(); "
        "  end LoadAsync; "
        "  public static async method Main(): Task<int> "
        "    do "
        "      var t := C.LoadAsync(); "
        "      var c := await t; "             // borrow-await (its use)
        "      _ := c; "
        "      await Task.Delay(5); "          // t still live across this suspend
        "    end; "                            // scope end: t disposed once
        "    return Cell.n; "                  // expect 0
        "  end Main; "
        "end C;"),
        0);
}

// ── D2 OwningTask (Pattern B) — the leak #1868's pre-gate review caught. A
// never-awaited `OwningTask<Cell>` held live across a suspend OWNS its produced
// Cell (§30.5 "Disposing an unconsumed OwningTask<T>"). The plain-`var` auto-drop
// path must attach the per-T result disposer (mirroring `using var`) or the Cell
// leaks. Fast producer (done at scope end): disposed exactly once. // per §30.5, §28.3
BEZEL_TEST(async_own_task_across_suspend_1868, never_awaited_owningtask_fast_disposed_once) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Threading; " + std::string(kCell) +
        "class C "
        "  public static async method MakeOwned(): OwningTask<Cell> "
        "    return new Cell(); "
        "  end MakeOwned; "
        "  public static async method Main(): Task<int> "
        "    do "
        "      var t := C.MakeOwned(); "        // owned OwningTask<Cell>, never awaited
        "      await Task.Delay(5); "            // different suspend — t live across it
        "    end; "                              // scope end: t + its Cell disposed once
        "    return Cell.n; "                    // expect 0
        "  end Main; "
        "end C;"),
        0);
}

// ── D2 OwningTask slow producer: still running at scope end — the per-T disposer
// must survive on the task so bz_task_free's fire-and-forget continuation
// disposes the Cell when it later appears (§30.5 "attaches a continuation that
// disposes the produced result whenever it appears"). This is the exact slow-path
// arm the plain-`var` path previously dropped. // per §30.5
BEZEL_TEST(async_own_task_across_suspend_1868, never_awaited_owningtask_slow_disposed) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Threading; " + std::string(kCell) +
        "class C "
        "  public static async method MakeOwnedSlow(): OwningTask<Cell> "
        "    await Task.Delay(30); "
        "    return new Cell(); "
        "  end MakeOwnedSlow; "
        "  public static async method Main(): Task<int> "
        "    do "
        "      var t := C.MakeOwnedSlow(); "     // not done at scope end
        "      await Task.Delay(5); "
        "    end; "
        "    await Task.Delay(80); "             // let the abandoned task settle + dispose
        "    return Cell.n; "                    // expect 0
        "  end Main; "
        "end C;"),
        0);
}

// ── D2 OwningTask EXCEPTION edge: the never-awaited OwningTask is live across a
// suspend when a later throw unwinds the frame; emitUnwindDisposals disposes it
// (and its owned Cell) exactly once. // per §28.3 exception-path discharge
BEZEL_TEST(async_own_task_across_suspend_1868, never_awaited_owningtask_throw_edge_disposed_once) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Threading; " + std::string(kCell) +
        "class C "
        "  public static async method MakeOwned(): OwningTask<Cell> "
        "    return new Cell(); "
        "  end MakeOwned; "
        "  public static async method Worker(): Task<int> "
        "    var t := C.MakeOwned(); "           // never awaited
        "    await Task.Delay(5); "               // suspend — t live across it
        "    throw new InvalidOperationException(\"boom\"); "
        "  end Worker; "
        "  public static async method Main(): Task<int> "
        "    try "
        "      var r := await C.Worker(); "
        "      _ := r; "
        "    catch Exception => _ := Cell.n; "
        "    end; "
        "    return Cell.n; "                     // expect 0
        "  end Main; "
        "end C;"),
        0);
}

// ── D2 OwningTask CANCELLATION edge: cancelled at a later await; the runtime
// faults the delay task, resumes the frame, and unwinds through
// emitUnwindDisposals — disposing the never-awaited OwningTask and its Cell once.
BEZEL_TEST(async_own_task_across_suspend_1868, never_awaited_owningtask_cancellation_edge_disposed_once) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Threading; " + std::string(kCell) +
        "class C "
        "  public static async method MakeOwned(): OwningTask<Cell> "
        "    return new Cell(); "
        "  end MakeOwned; "
        "  public static async method Worker(token: CancellationToken): Task<int> "
        "    var t := C.MakeOwned(); "           // never awaited
        "    await Task.Delay(10000, token); "   // cancelled here — frame unwinds
        "    return 0; "
        "  end Worker; "
        "  public static async method Main(): Task<int> "
        "    using var cts := CancellationTokenSource.WithCancelAfter("
        "        TimeSpan.FromMilliseconds(20)); "
        "    try "
        "      var r := await C.Worker(cts.Token); "
        "      _ := r; "
        "    catch Exception => _ := Cell.n; "
        "    end; "
        "    return Cell.n; "                     // expect 0
        "  end Main; "
        "end C;"),
        0);
}

// ── Regression guard for the `using var` OwningTask path (which already attached
// the disposer): the shared helper must not have changed it. A `using var`
// OwningTask<Cell> across a suspend disposes its Cell exactly once (no
// double-free with the plain-`var` path — they are distinct locals). // per §30.5
BEZEL_TEST(async_own_task_across_suspend_1868, using_var_owningtask_across_suspend_disposed_once) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Threading; " + std::string(kCell) +
        "class C "
        "  public static async method MakeOwned(): OwningTask<Cell> "
        "    return new Cell(); "
        "  end MakeOwned; "
        "  public static async method Main(): Task<int> "
        "    do "
        "      using var t := C.MakeOwned(); "
        "      await Task.Delay(5); "
        "    end; "
        "    return Cell.n; "                     // expect 0
        "  end Main; "
        "end C;"),
        0);
}
