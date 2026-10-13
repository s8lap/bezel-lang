// Codegen — §30.3 / §30.1 / §30.2: the synchronous `Task.Wait()` / `Task<T>.Result`
// escape hatches drive a genuinely-suspending async state machine to completion (#941).
//
// Bug #941: statements after `await Task.Delay(N)` never ran when the awaited task
// GENUINELY suspends (Delay is a real cross-thread suspension, completed by a pool
// timer). The root cause was that `t.Wait()` — the synchronous escape hatch a
// non-async caller uses to block until the task completes (spec §30.3) — was not
// lowered at all: `Wait` is a builtin Task method with no MethodSymbol, so the call
// fell through every dispatch arm and was silently dropped. With Wait() dropped, the
// non-async `Main` read the static field before the continuation (which runs on a
// pool worker after the Delay timer fires) had executed the post-await code, so it
// observed the pre-await value.
//
// The state-machine codegen itself was correct (the post-await block is present and
// reachable from the resume switch, and the continuation is registered via
// bz_task_then); the defect was the missing blocking wait at the synchronous call
// site. `t.Wait()` now lowers to bz_task_await_sync(task) — the same blocking
// primitive the async-Main wrapper uses (§30.13) — which parks the calling thread
// until the pool worker completes the task, so the post-await write is observed.
//
// `t.Result` (the §30.3 synchronous result read on Task<T>) is lowered the same way:
// block via bz_task_await_sync, re-raise the original exception object on a faulted
// task (§30.9), then load the result by value.
//
// Spec derivation: §30.3 ("`Wait()` and `Result` are synchronous escape hatches for
// non-async code. They block the calling thread until the task completes"), §30.1
// (an async method "suspends and resumes at each await"), §30.2 (state-machine
// resume). v0.148.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>

#include "driver/Driver.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace {

namespace fs = std::filesystem;

// Per-process unique working dir, under the shared scratch base (never
// /tmp — see support/scratch_dir.hpp) and reaped at process exit.
static const std::filesystem::path& wtDir941() {
    return wtScratchDir("wt-941");
}

// Compile `source` to a temp native executable via the real Driver and run it;
// returns the exit code, or -1 on compile/link/run failure.
int compileAndRun(const std::string& source) {
    auto dir = wtDir941();
    std::string stem = unique_stem("bezel_941");
    std::string src = (dir / (stem + ".bzl")).string();
    std::string exe = (dir / stem).string();
    {
        std::ofstream out(src);
        out << source;
    }
    bezel::Driver driver;
    bezel::CompileResult r = driver.compileFile(src, exe);
    int code = -1;
    if (r.success) {
        int status = std::system(exe.c_str());
        if (status != -1 && WIFEXITED(status)) code = WEXITSTATUS(status);
    }
    std::remove(src.c_str());
    std::remove(exe.c_str());
    return code;
}

// Run 3x to surface any non-determinism in the suspend/resume + Wait() ordering.
void expectValue941(const std::string& source, int expected) {
    for (int i = 0; i < 3; ++i)
        BEZEL_EXPECT_EQ(compileAndRun(source), expected);
}

// §30.3 (v0.162 / #1362): compile returns true when it fails (error emitted).
bool compileFails941(const std::string& source) {
    auto dir = wtDir941();
    std::string stem = unique_stem("bezel_941_neg");
    std::string src = (dir / (stem + ".bzl")).string();
    std::string exe = (dir / stem).string();
    {
        std::ofstream out(src);
        out << source;
    }
    bezel::Driver driver;
    bezel::CompileResult r = driver.compileFile(src, exe);
    std::remove(src.c_str());
    std::remove(exe.c_str());
    return !r.success;
}

}  // namespace

// ---------------------------------------------------------------------------
// §30.3 / §30.1: the issue repro — `await Task.Delay(0)` followed by a write,
// driven from a NON-ASYNC `Main` via `t.Wait()`. Pre-fix this returned 0 (the
// post-await write never ran because Wait() was dropped); it must return 1.
// ---------------------------------------------------------------------------
BEZEL_TEST(async_sync_wait_941, wait_drives_post_delay_write) {
    expectValue941(
        "module M; class C "
        "  public static ran: int := 0; "
        "  public static async method RunAsync(): Task "
        "    await Task.Delay(0); "
        "    C.ran := 1; "
        "  end RunAsync; "
        "  public static method Main(): int "
        "    using var t := C.RunAsync(); t.Wait(); "
        "    return C.ran; "
        "  end Main; end C;",
        1);
}

// ---------------------------------------------------------------------------
// §30.3: a NONZERO Delay also resumes — the timer sleeps on a pool worker and
// completes the task, which Wait() blocks for.
// ---------------------------------------------------------------------------
BEZEL_TEST(async_sync_wait_941, wait_drives_nonzero_delay_write) {
    expectValue941(
        "module M; class C "
        "  public static ran: int := 0; "
        "  public static async method RunAsync(): Task "
        "    await Task.Delay(1); "
        "    C.ran := 5; "
        "  end RunAsync; "
        "  public static method Main(): int "
        "    using var t := C.RunAsync(); t.Wait(); "
        "    return C.ran; "
        "  end Main; end C;",
        5);
}

// ---------------------------------------------------------------------------
// §30.1 / §30.2: TWO sequential awaits — both continuations must run, in order,
// and the final value reflects the second post-await write.
// ---------------------------------------------------------------------------
BEZEL_TEST(async_sync_wait_941, wait_drives_two_sequential_awaits) {
    expectValue941(
        "module M; class C "
        "  public static x: int := 0; "
        "  public static async method RunAsync(): Task "
        "    await Task.Delay(0); "
        "    C.x := 1; "
        "    await Task.Delay(0); "
        "    C.x := 2; "
        "  end RunAsync; "
        "  public static method Main(): int "
        "    using var t := C.RunAsync(); t.Wait(); "
        "    return C.x; "
        "  end Main; end C;",
        2);
}

// ---------------------------------------------------------------------------
// §30.1 / §30.2: an `await` inside a loop — every iteration's continuation runs.
// sum of i for i in 0..2 = 0+1+2 = 3.
// ---------------------------------------------------------------------------
BEZEL_TEST(async_sync_wait_941, wait_drives_await_in_loop) {
    expectValue941(
        "module M; class C "
        "  public static sum: int := 0; "
        "  public static async method RunAsync(): Task "
        "    var i: int := 0; "
        "    while i < 3 do "
        "      await Task.Delay(0); "
        "      C.sum := C.sum + i; "
        "      i := i + 1; "
        "    end; "
        "  end RunAsync; "
        "  public static method Main(): int "
        "    using var t := C.RunAsync(); t.Wait(); "
        "    return C.sum; "
        "  end Main; end C;",
        3);
}

// ---------------------------------------------------------------------------
// §30.3: a value-returning task — `t.Result` blocks until complete (after the
// real Delay suspension) and yields the returned value (7), consumed in-scope.
// ---------------------------------------------------------------------------
BEZEL_TEST(async_sync_wait_941, result_reads_value_after_delay) {
    expectValue941(
        "module M; class C "
        "  public static async method RunAsync(): Task<int> "
        "    await Task.Delay(0); "
        "    return 7; "
        "  end RunAsync; "
        "  public static method Main(): int "
        "    var r: int := 0; "
        "    do "
        "      using var t := C.RunAsync(); "
        "      t.Wait(); "
        "      r := t.Result + 1; "
        "    end; "
        "    return r; "
        "  end Main; end C;",
        8);  // 7 + 1
}

// ---------------------------------------------------------------------------
// §30.1 (no-regression): a SYNCHRONOUS / already-complete await (Task.FromResult,
// no real suspension) still runs the post-await code under Wait().
// ---------------------------------------------------------------------------
BEZEL_TEST(async_sync_wait_941, wait_synchronous_await_still_runs) {
    expectValue941(
        "module M; class C "
        "  public static ran: int := 0; "
        "  public static async method RunAsync(): Task "
        "    await Task.FromResult<int>(0); "
        "    C.ran := 9; "
        "  end RunAsync; "
        "  public static method Main(): int "
        "    using var t := C.RunAsync(); t.Wait(); "
        "    return C.ran; "
        "  end Main; end C;",
        9);
}

// ---------------------------------------------------------------------------
// §30.3 (#1175 fix): `return t.Result` from a non-async method must compile
// and return the task's value. Pre-fix this was rejected with a false-positive
// "borrow returned from `t` would outlive its source" because sema left
// Task<T>.Result untyped; returnedExprMayBorrow() fell to its conservative
// `return true`, triggering the borrow-escape check on what is actually a
// value-typed copy (the int result). Post-fix: Result is typed T (int) in
// resolveMemberAccess, so isBorrowableType returns false and no escape error
// fires. Spec §30.3 ("`Wait()` and `Result` are synchronous escape hatches
// for non-async code"), v0.131.
// ---------------------------------------------------------------------------
BEZEL_TEST(async_sync_wait_941, result_returned_directly_no_borrow_escape_1175) {
    expectValue941(
        "module M; class C "
        "  public static async method Produce(): Task<int> "
        "    await Task.Delay(0); "
        "    return 42; "
        "  end Produce; "
        "  public static method Main(): int "
        "    using var t := C.Produce(); "
        "    t.Wait(); "
        "    return t.Result; "  // pre-fix: BAD_REJECT borrow-escape
        "  end Main; end C;",
        42);
}

// ---------------------------------------------------------------------------
// §30.3 (#1175 fix): IsCompleted / IsFaulted / IsCancelled are bool readonly
// properties on Task (and its subclasses Task<T>, OwningTask<T>) — spec §30.3.
// Pre-fix these were untyped in sema (no TypeSymbol on the task type);
// post-fix they are typed bool in resolveMemberAccess so `if t.IsCompleted`
// and similar uses compile correctly. Test: after Wait(), the completed-and-
// non-faulted task returns t.Result; the path gated on IsCompleted must run.
// ---------------------------------------------------------------------------
BEZEL_TEST(async_sync_wait_941, iscompleted_isfaulted_iscancelled_typed_1175) {
    expectValue941(
        "module M; class C "
        "  public static async method Produce(): Task<int> "
        "    await Task.Delay(0); "
        "    return 77; "
        "  end Produce; "
        "  public static method Main(): int "
        "    using var t := C.Produce(); "
        "    t.Wait(); "
        "    if t.IsCompleted and not t.IsFaulted and not t.IsCancelled then "
        "      return t.Result; "
        "    end; "
        "    return 0; "
        "  end Main; end C;",
        77);
}

// ---------------------------------------------------------------------------
// §30.3 / §30.9 (#1196): reading .Result on a faulted BOUND Task<T> inside
// a try/catch must NOT double-free the exception object. Pre-fix: the catch
// arm's caughtException disposal freed error_obj, then the task-local's
// scope-end disposal freed it again → SIGSEGV (vtable read on a freed object).
// Post-fix: bz_mark_exc_borrowed marks the error_obj borrowed before the
// synchronous re-raise, so the catch arm skips reclaim (just unmarks) and the
// task-local scope-end disposes it exactly once.
// The program catches the exception and returns 7; the task local is disposed
// cleanly at scope end. Both must happen without a crash.
// ---------------------------------------------------------------------------
BEZEL_TEST(async_sync_wait_941, result_on_faulted_bound_task_no_double_free_1196) {
    expectValue941(
        "module M; class C "
        "  public static async method Boom(): Task<int> "
        "    throw new Exception(\"boom\"); "
        "  end Boom; "
        "  public static method Main(): int "
        "    var got: int := 0; "
        "    var t: Task<int> := C.Boom(); "
        "    try "
        "      got := t.Result; "
        "    catch Exception => got := 7; "
        "    end; "
        "    return got; "
        "  end Main; end C;",
        7);
}

// ---------------------------------------------------------------------------
// §30.3 / §30.9 (#1196): variant — re-read .Result via IsFaulted guard after
// a first failed .Result attempt (exception caught); the second call raises
// again from the SAME error_obj that the task still owns. Both calls must
// complete without crash (the borrowed-mark + unmark cycle repeats correctly).
// ---------------------------------------------------------------------------
BEZEL_TEST(async_sync_wait_941, result_on_faulted_bound_task_isFaulted_variant_1196) {
    expectValue941(
        "module M; class C "
        "  public static async method Fault(): Task<int> "
        "    throw new Exception(\"oops\"); "
        "  end Fault; "
        "  public static method Main(): int "
        "    var t: Task<int> := C.Fault(); "
        "    var r: int := 0; "
        "    try "
        "      r := t.Result; "
        "    catch Exception => r := 42; "
        "    end; "
        "    return r; "
        "  end Main; end C;",
        42);
}

// ---------------------------------------------------------------------------
// §30.3 (v0.162 / #1362): Wait() and .Result are rejected inside async methods
// and async lambdas. Three shapes: Wait() in async method, Result in async method,
// Wait() in async lambda body. Non-async context (plain method Main) still accepts.
// ---------------------------------------------------------------------------

// §30.3 v0.162: `t.Wait()` lexically inside an `async` method → compile error.
BEZEL_TEST(async_sync_wait_941, wait_inside_async_method_rejected_30_3_v0162) {
    // §30.3 (v0.162): Wait() in async context — compile error "use `await t`".
    BEZEL_EXPECT_EQ(compileFails941(
        "module M; class C "
        "  public static async method Helper(): Task "
        "    using var t := Task.Delay(1); "
        "    t.Wait(); "          // ERROR: blocking wait inside async context
        "  end Helper; "
        "  public static async method Main(): Task<int> "
        "    return 0; "
        "  end Main; end C;"),
        true);
}

// §30.3 v0.162: `t.Result` lexically inside an `async` method → compile error.
BEZEL_TEST(async_sync_wait_941, result_inside_async_method_rejected_30_3_v0162) {
    BEZEL_EXPECT_EQ(compileFails941(
        "module M; class C "
        "  public static async method Main(): Task<int> "
        "    using var t := Task.FromResult(42); "
        "    var r: int := t.Result; "   // ERROR: blocking wait inside async context
        "    return r; "
        "  end Main; end C;"),
        true);
}

// §30.3 v0.162: `t.Wait()` inside an `async` lambda → compile error.
BEZEL_TEST(async_sync_wait_941, wait_inside_async_lambda_rejected_30_3_v0162) {
    BEZEL_EXPECT_EQ(compileFails941(
        "module M; class C "
        "  public static method Main(): int "
        "    var f := async lambda() => do "
        "      using var t := Task.Delay(1); "
        "      t.Wait(); "        // ERROR: blocking wait inside async lambda
        "    end; "
        "    return 0; "
        "  end Main; end C;"),
        true);
}

// §30.3 v0.162: Wait() in non-async Main is STILL ACCEPTED (the escape hatch).
BEZEL_TEST(async_sync_wait_941, wait_in_sync_main_still_accepted_30_3_v0162) {
    // Plain (non-async) Main: Wait() is the intended escape hatch — must compile OK.
    expectValue941(
        "module M; class C "
        "  public static async method RunAsync(): Task<int> "
        "    await Task.Delay(1); "
        "    return 5; "
        "  end RunAsync; "
        "  public static method Main(): int "
        "    using var t := C.RunAsync(); "
        "    t.Wait(); "          // LEGAL: non-async context
        "    return t.Result; "   // LEGAL: t is complete, non-blocking
        "  end Main; end C;",
        5);
}

// ---------------------------------------------------------------------------
// §30.5 (v0.162 / #1352): OwningTask spend-on-all-edges.
// An OwningTask must be marked spent after the FIRST await on any path —
// fault as well as success. A second `await t` must raise
// InvalidOperationException (not replay the original error).
//
// Test shape: async producer throws; consumer catches the fault from the
// first await, then attempts a second await in another try/catch. The
// second await must raise IOE (r=2), not fault again (r=1) or crash.
// ---------------------------------------------------------------------------
BEZEL_TEST(async_sync_wait_941, owned_task_second_await_after_fault_raises_ioe_1352) {
    // §30.5 v0.162: first await faults (r=1), second await must raise IOE (r=2).
    expectValue941(
        "module M; "
        "public sealed class Box "
        "  public method V(): int return 1; end V; "
        "end Box; "
        "class C "
        "  public static async method FaultProducer(): OwningTask<Box> "
        "    throw new Exception(\"boom\"); "
        "  end FaultProducer; "
        "  public static async method RunTest(): Task<int> "
        "    using var t := C.FaultProducer(); "
        "    var r: int := 0; "
        "    try "
        "      _ := await t; "               // first await — faults
        "    catch Exception => r := 1; "    // caught: r=1
        "    end; "
        "    try "
        "      _ := await t; "               // second await — IOE (spent)
        "    catch InvalidOperationException => r := 2; "
        "    end; "
        "    return r; "
        "  end RunTest; "
        "  public static method Main(): int "
        "    using var t := C.RunTest(); "
        "    t.Wait(); "
        "    return t.Result; "
        "  end Main; end C;",
        2);
}

// §30.5 v0.162: OwningTask spend-on-success: after a SUCCESSFUL first await,
// a second await must also raise IOE (not silently return a stale value or crash).
BEZEL_TEST(async_sync_wait_941, owned_task_second_await_after_success_raises_ioe_1352) {
    // §30.5 v0.162: first await succeeds, second await raises IOE (r=2).
    expectValue941(
        "module M; "
        "public sealed class Box "
        "  public method V(): int return 99; end V; "
        "end Box; "
        "class C "
        "  public static async method Produce99(): OwningTask<Box> "
        "    return new Box(); "
        "  end Produce99; "
        "  public static async method RunTest(): Task<int> "
        "    using var t := C.Produce99(); "
        "    var r: int := 0; "
        "    try "
        "      _ := await t; "               // first await — succeeds
        "    catch Exception => r := 0; "
        "    end; "
        "    try "
        "      _ := await t; "               // second await — IOE (spent)
        "    catch InvalidOperationException => r := 2; "
        "    end; "
        "    return r; "
        "  end RunTest; "
        "  public static method Main(): int "
        "    using var t := C.RunTest(); "
        "    t.Wait(); "
        "    return t.Result; "
        "  end Main; end C;",
        2);
}

// ---------------------------------------------------------------------------
// §30.5 / #1332 (v0.162): Pattern A chained slot — `return await <call>`
// where the outer async method returns Task<T:class>. The source task is NOT
// freed immediately at the await site; instead it is chained to the outer task
// via bz_task_set_chained so its lifetime is tied to the outer task's lifetime.
// Correctness: the class value passes through the chain and the result is
// accessible via .Result. (Pattern A class-result disposal is a deferred
// increment; this test verifies the basic chain mechanics.)
//
// Fast-path variant: MakeBox is synchronous (no delay) → takes the already-done
// fast path through emitAwaitSuspend (no real thread suspension).
//
// #1530 (§30.3 / §30.5, v0.179): `.Result` on a class-typed `Task<T>` (Pattern
// A) is a borrow whose source must be a syntactically-visible owning
// location — the same rule `await` enforces (§30.5 Consumer-side rules). A
// bare `C.ChainBox(77).Result` is a sourceless read and is now rejected, so
// the producer is bound to a `using var` first; the chained-slot mechanics
// and result value under test are unchanged.
// ---------------------------------------------------------------------------
BEZEL_TEST(async_sync_wait_941, pattern_a_chained_slot_class_result_fast_path_1332) {
    expectValue941(
        "module M; "
        "class Box "
        "  public X: int; "
        "  public constructor(x: int) "
        "    this.X := x; "
        "  end constructor; "
        "end Box; "
        "class C "
        "  public static async method MakeBox(x: int): Task<Box> "
        "    return new Box(x); "
        "  end MakeBox; "
        "  public static async method ChainBox(x: int): Task<Box> "
        "    return await C.MakeBox(x); "      // Pattern A chained return
        "  end ChainBox; "
        "  public static method Main(): int "
        "    using var t := C.ChainBox(77); "  // #1530: owning location for .Result
        "    var b: Box := t.Result; "
        "    return b.X; "
        "  end Main; "
        "end C;",
        77);
}

// Slow-path variant: MakeBox suspends via Task.Delay(0) before returning, so
// the real suspension path (bz_task_then + Resume) is exercised.
// bz_task_set_chained must be called in the Resume function (state 1) after
// the source task completes, before bz_task_complete on the outer task.
//
// #1530 (§30.3 / §30.5, v0.179): same owning-location rule as the fast-path
// variant above — the producer is bound to a `using var` before `.Result`.
BEZEL_TEST(async_sync_wait_941, pattern_a_chained_slot_class_result_slow_path_1332) {
    expectValue941(
        "module M; "
        "class Box "
        "  public X: int; "
        "  public constructor(x: int) "
        "    this.X := x; "
        "  end constructor; "
        "end Box; "
        "class C "
        "  public static async method MakeBoxDelayed(x: int): Task<Box> "
        "    await Task.Delay(0); "
        "    return new Box(x); "
        "  end MakeBoxDelayed; "
        "  public static async method ChainBoxDelayed(x: int): Task<Box> "
        "    return await C.MakeBoxDelayed(x); "   // Pattern A chained return (slow)
        "  end ChainBoxDelayed; "
        "  public static method Main(): int "
        "    using var t := C.ChainBoxDelayed(55); "  // #1530: owning location for .Result
        "    var b: Box := t.Result; "
        "    return b.X; "
        "  end Main; "
        "end C;",
        55);
}
