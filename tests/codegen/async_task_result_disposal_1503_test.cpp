// Codegen/runtime — issue #1503 (§30.5 "Result lifetime and task disposal"):
// disposing a Task never disposed its unconsumed class-typed RESULT.
//
// §30.5 makes three related promises about a class-typed Task's produced
// result:
//   spec:6380  "The result's lifetime is tied to the task — disposing the
//               task disposes the result." (Pattern A, Task<T:class>)
//   spec:6511  "disposing the task disposes the result if one was produced"
//               (§30.5 "Disposing an unconsumed OwningTask<T>", Pattern B)
//   spec:6521  "the `Dispose()` call attaches a continuation that disposes
//               the produced result whenever it appears" (Pattern B, task
//               not yet settled at disposal time)
//
// Pre-fix, none of these held for a class-typed result:
//   - Pattern A (`Task<T:class>`): bz_task_free only ever did `free(task->
//     result)` — freeing the malloc'd buffer that HOLDS the class pointer,
//     never disposing the pointee. A `using var` Task<T> local dropped
//     (awaited-as-borrow or not) always leaked its produced T.
//   - Pattern A chained slot (`return await <Pattern A source>`): the SAME
//     gap in the source task's own disposal — bz_task_free already freed the
//     chained_source transitively (#1332), but the source's own result was
//     never disposed for the same reason as the direct case above.
//   - Pattern B (`OwningTask<T:class>`) dropped before settling: the
//     scope-end codegen called bz_task_dispose_owned_result SYNCHRONOUSLY,
//     which only ever inspected the task's result buffer at that exact
//     instant. A task still running at drop time had no result yet (a
//     no-op), and the per-T disposer was never retained anywhere — so the
//     eventual result, produced later on a pool thread, leaked when
//     bz_task_fire_and_forget's completion continuation called plain
//     bz_task_free.
//
// The fix (LlvmBackend_Async.cpp / LlvmBackend_Stmts.cpp /
// LlvmBackend_Disposal.cpp / bezel_runtime_concurrency.cpp):
//   - `bz_task_t` gained a `result_disposer` field + `bz_task_set_result_
//     disposer` setter; `bz_task_free` now disposes the result's pointee
//     (once, guarded against fault/already-consumed) whenever that field is
//     set, whether bz_task_free runs immediately or later via the
//     fire-and-forget completion continuation — satisfying the "attaches a
//     continuation" text for a not-yet-settled task.
//   - Pattern A: the async body's non-chained `return <owned>` completion
//     site (LlvmBackend_Stmts.cpp) attaches the per-T disposer to the outer
//     task before bz_task_complete. A chained return leaves the outer task's
//     own disposer unset (it does not own the result directly); the SOURCE
//     task gets its own disposer set at ITS OWN non-chained return, and
//     bz_task_free's existing transitive chained_source walk (#1332) reaches
//     it exactly as before.
//   - Pattern B: the consumer's `using var` scope-end drop now calls
//     bz_task_set_result_disposer instead of the old eager-only dispose, so
//     the disposer survives to whichever bz_task_free call actually reclaims
//     the task.
//
// Verification uses the `Cell` static-counter idiom (ctor ++, dtor --,
// established by tests/codegen/async_owned_task_result_1127_test.cpp): a
// leak leaves Cell.n > 0; a double-free/double-dispose underflows it or
// crashes. `Cell.n == 0` after the task in question is disposed is the
// dispose-exactly-once assertion for all cases below. v0.177 (per issue).

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

// Cell: a class with a static counter (ctor ++, dtor --) for leak detection.
// Same idiom as async_owned_task_result_1127_test.cpp's kCell.
const char* kCell =
    "class Cell public static n: int := 0; "
    "  public constructor() Cell.n:=Cell.n+1; end constructor; "
    "  destructor Cell.n:=Cell.n-1; end destructor; end Cell; ";
}  // namespace

// ---------------------------------------------------------------------------
// §30.5 Pattern A owned slot — spec:6380 "disposing the task disposes the
// result". `LoadAsync(): Task<Cell>` produces a Cell synchronously (no
// `await` inside — the "fast path", already-done at the `using var` scope
// end). `await t` only ever BORROWS the result for Task<T> (§30.5 consumer-
// side rules: "Every awaiter receives a borrow of the same underlying
// result"); it must not discharge the task's own ownership. When `t`'s
// `using var` scope ends, the task — and with it the Cell it produced — must
// be disposed exactly once. Pre-fix: Cell.n stayed 1 forever (leak).
// ---------------------------------------------------------------------------
BEZEL_TEST(async_task_result_disposal_1503, pattern_a_owned_slot_disposed_30_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; " + std::string(kCell) +
        "class C "
        "  public static async method LoadAsync(): Task<Cell> "
        "    return new Cell(); "
        "  end LoadAsync; "
        "  public static async method Main(): Task<int> "
        "    do using var t := C.LoadAsync(); var c := await t; end; "
        // expect 0: t (and its borrowed-by-c result) disposed at the
        // do-block's scope end
        "    return Cell.n; "
        "  end Main; "
        "end C;"),
        0);
}

// Same shape, but nobody ever reads the result at all (no `await`/`.Result`
// call) — the task is dropped purely for its (absent) side effects. Still
// must dispose the produced Cell at scope end.
BEZEL_TEST(async_task_result_disposal_1503,
          pattern_a_owned_slot_disposed_unread_30_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; " + std::string(kCell) +
        "class C "
        "  public static async method LoadAsync(): Task<Cell> "
        "    return new Cell(); "
        "  end LoadAsync; "
        "  public static method Main(): int "
        "    do using var t := C.LoadAsync(); end; "  // never awaited/read
        "    return Cell.n; "                          // expect 0
        "  end Main; "
        "end C;"),
        0);
}

// Slow-path variant: LoadAsync suspends via Task.Delay before returning, so
// the produced-value path runs through bz_task_complete inside the Resume
// continuation (a pool thread) rather than synchronously inline. The
// disposer is attached at that SAME non-chained return site regardless of
// which path completes it, so this must dispose identically to the fast
// path above.
BEZEL_TEST(async_task_result_disposal_1503,
          pattern_a_owned_slot_disposed_slow_path_30_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Threading; " + std::string(kCell) +
        "class C "
        "  public static async method LoadAsyncDelayed(): Task<Cell> "
        "    await Task.Delay(5); "
        "    return new Cell(); "
        "  end LoadAsyncDelayed; "
        "  public static async method Main(): Task<int> "
        "    do using var t := C.LoadAsyncDelayed(); var c := await t; end; "
        "    return Cell.n; "  // expect 0
        "  end Main; "
        "end C;"),
        0);
}

// ---------------------------------------------------------------------------
// §30.5 Pattern A chained slot — spec:6380/#1332: `return await <Pattern A
// source>` moves ownership of the SOURCE task into the outer task's chained
// slot; "disposing the task disposes the result" holds TRANSITIVELY. The
// outer task's own destructor disposes the chained SOURCE task (already
// implemented, #1332); the source task's OWN produced Cell must then be
// disposed as part of that chain — the piece #1503 adds. Fast-path (source
// completes synchronously) and slow-path (source suspends via Task.Delay,
// so bz_task_set_chained runs from the Resume continuation, #1332's own
// slow-path concern) are both covered, mirroring
// pattern_a_chained_slot_class_result_{fast,slow}_path_1332 in
// async_sync_wait_941_test.cpp (which check VALUE flow-through only; these
// check disposal count).
// ---------------------------------------------------------------------------
BEZEL_TEST(async_task_result_disposal_1503,
          pattern_a_chained_slot_disposed_fast_path_30_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; " + std::string(kCell) +
        "class C "
        "  public static async method MakeCell(): Task<Cell> "
        "    return new Cell(); "
        "  end MakeCell; "
        "  public static async method ChainCell(): Task<Cell> "
        "    return await C.MakeCell(); "  // Pattern A chained return
        "  end ChainCell; "
        "  public static async method Main(): Task<int> "
        "    do using var t := C.ChainCell(); var c := await t; end; "
        // expect 0: t's chained source (MakeCell's task) is disposed
        // transitively, disposing the Cell it directly owns
        "    return Cell.n; "
        "  end Main; "
        "end C;"),
        0);
}

BEZEL_TEST(async_task_result_disposal_1503,
          pattern_a_chained_slot_disposed_slow_path_30_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Threading; " + std::string(kCell) +
        "class C "
        "  public static async method MakeCellDelayed(): Task<Cell> "
        "    await Task.Delay(5); "
        "    return new Cell(); "
        "  end MakeCellDelayed; "
        "  public static async method ChainCellDelayed(): Task<Cell> "
        "    return await C.MakeCellDelayed(); "  // chained return (slow)
        "  end ChainCellDelayed; "
        "  public static async method Main(): Task<int> "
        "    do using var t := C.ChainCellDelayed(); var c := await t; end; "
        "    return Cell.n; "  // expect 0
        "  end Main; "
        "end C;"),
        0);
}

// ---------------------------------------------------------------------------
// §30.5 "Disposing an unconsumed OwningTask<T>" — spec:6511/6521: an
// OwningTask<T:class> dropped (using var, no await) before its result has
// been transferred disposes the produced result "if one was produced"; when
// the task has NOT completed at the point of disposal, "the Dispose() call
// attaches a continuation that disposes the produced result whenever it
// appears, then returns immediately" (non-blocking).
//
// AcquireAsync suspends via Task.Delay BEFORE producing its Cell, so the
// `using var t` drop below happens while the task is still genuinely
// in-flight (task->result not yet set) — the exact gap #1503 fixes (the
// pre-fix eager, call-time-only dispose saw nothing to do and forgot the
// disposer entirely, leaking the Cell produced later on the pool thread).
// Main then awaits a second Task.Delay purely to give the background
// completion + attached disposal time to run before checking Cell.n — the
// drop itself is still the non-blocking, "returns immediately" §30.5 shape;
// nothing here makes Dispose() itself block.
// ---------------------------------------------------------------------------
BEZEL_TEST(async_task_result_disposal_1503,
          pattern_b_abandoned_inflight_disposed_30_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Threading; " + std::string(kCell) +
        "class C "
        "  public static async method AcquireAsync(): OwningTask<Cell> "
        "    await Task.Delay(10); "      // still running at the drop below
        "    return new Cell(); "
        "  end AcquireAsync; "
        "  public static async method Main(): Task<int> "
        "    do using var t := C.AcquireAsync(); end; "  // dropped, never awaited
        // let the background completion + attached disposal continuation run
        "    await Task.Delay(150); "
        "    return Cell.n; "  // expect 0 (not a leak)
        "  end Main; "
        "end C;"),
        0);
}

// Regression: the SAME in-flight-at-drop shape, but the result IS consumed
// (via `await`) before the `using var` scope that owns the task ends. The
// scope-end code now unconditionally calls bz_task_set_result_disposer for
// any OwningTask<T:class> local (not just an unconsumed one); this must
// remain a no-op when bz_task_take_result already nulled the result slot at
// the consuming await — Cell.n must reach 0 from the ONE dispose via `c`,
// not from a second dispose via the task, which would underflow/crash.
BEZEL_TEST(async_task_result_disposal_1503,
          pattern_b_inflight_consumed_single_dispose_30_5) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Threading; " + std::string(kCell) +
        "class C "
        "  public static async method AcquireAsync(): OwningTask<Cell> "
        "    await Task.Delay(10); "
        "    return new Cell(); "
        "  end AcquireAsync; "
        "  public static async method Consume(): Task<int> "
        "    using var t := C.AcquireAsync(); "
        "    using var c := await t; "  // consumes the result; task->result nulled
        "    return Cell.n; "           // 1 while c is alive
        "  end Consume; "
        "  public static async method Main(): Task<int> "
        "    await C.Consume(); "
        // expect 0: single dispose via `c`; no second dispose from t's own
        // scope end
        "    return Cell.n; "
        "  end Main; "
        "end C;"),
        0);
}
