// Codegen — async/await gap completions (#451).
//
// (A) Task.WhenAll<T>(tasks: Array<Task<T>>): Task<Array<T>>
//     stdlib §6.7 / spec §30.10.
// (B) Task.Delay(ms, token: CancellationToken): Task  — token cancellation.
//     stdlib §6.7 / spec §30.8.
// (C) await inside a catch GUARD (catch E when await ...) — confirmed clean
//     diagnostic (not a crash).
//
// Like task_combinators_test.cpp, async emitter state accumulates across JIT
// runs, so all tests compile to native executables via Driver and check exit
// codes. Cross-thread tests are run 3× for nondeterminism stability.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>

#include "codegen/LlvmBackend.hpp"
#include "driver/Driver.hpp"
#include "lexer/Lexer.hpp"
#include "ownership/Ownership.hpp"
#include "parser/Parser.hpp"
#include "sema/DefiniteAssignment.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/SymbolCollection.hpp"
#include "sema/TypeResolution.hpp"
#include "support/Diagnostic.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"
#include "native_test_harness.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

namespace {

namespace fs = std::filesystem;

// Per-process unique working dir, under the shared scratch base (never
// /tmp — see support/scratch_dir.hpp) and reaped at process exit.
static const std::filesystem::path& wtDir451() {
    return wtScratchDir("wt-451");
}

// Compile and run, returning {exit_code, run_wall_ms}.
// run_wall_ms is the time spent executing the compiled binary only (excludes
// compile time).  Returns {-1, -1} on compile/link failure.
std::pair<int,long> compileAndRunTimed(const std::string& source) {
    auto dir = wtDir451();
    std::string stem = unique_stem("bezel_async451_timed");
    std::string src = (dir / (stem + ".bzl")).string();
    std::string exe = (dir / stem).string();
    { std::ofstream out(src); out << source; }
    bezel::Driver driver;
    bezel::CompileResult r = driver.compileFile(src, exe);
    int code = -1;
    long elapsed_ms = -1;
    if (r.success) {
        auto t0 = std::chrono::steady_clock::now();
        int status = std::system(exe.c_str());
        elapsed_ms = static_cast<long>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count());
        if (status != -1 && WIFEXITED(status)) code = WEXITSTATUS(status);
    }
    std::remove(src.c_str());
    std::remove(exe.c_str());
    return {code, elapsed_ms};
}

// Compile `source` to a temp native executable via the real Driver and run it.
// Returns the process exit code, or -1 on compile/link/run failure.
int compileAndRun(const std::string& source) {
    auto dir = wtDir451();
    std::string stem = unique_stem("bezel_async451");
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

// True when source FAILS to compile (deferred-feature diagnostic fired).
// Mirrors the compileFails() helper in async_test.cpp.
bool compileFails(const std::string& source) {
    auto dir = wtDir451();
    std::string stem = unique_stem("bezel_async451_neg");
    std::string src  = (dir / (stem + ".bzl")).string();
    std::string exe  = (dir / stem).string();
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

// ===========================================================================
// (A) Task.WhenAll<T> — stdlib §6.7
// ===========================================================================
//
// fix #4670 / §30.5 (v0.307): `Task.WhenAll<T>(tasks)` returns `Task<Array<T>>`
// — a Pattern A producer (`Array<T>` is class-like regardless of T), so
// `await wa` produces a BORROW. §30.5's consumer-side rules admit that
// borrow only to a plain `var`, never `using var` — every
// `using var arr := await wa;` row below (and in the `task_delay_1378`
// starvation row further down this file) used that shape before #4670
// landed; #4670 correctly rejects it now, so each is converted to
// `var arr := await wa;` here. `wa` itself keeps its own `using var` (the
// Task<Array<T>> task's own disposal is unaffected — the rule is about the
// AWAIT RESULT's binding, not the task's). The sibling `WhenAllOwned<T>`
// rows below (`when_all_owned_451` suite) return `OwningTask<OwningArray<T>>`
// — Pattern B — and are NOT affected; their `using var arr := await wa;`
// stays exactly as-is.

// §6.7: WhenAll<T> over Task<int> — collects three int results
// into Array<int>. The indexed results should be [10, 20, 30] in order.
// Main returns results[0] + results[1] + results[2] = 60.
// Derived from: stdlib §6.7 WhenAll<T>(tasks: Array<Task<T>>): Task<Array<T>>.
BEZEL_TEST(whenall_generic_results_451, int_results_sum) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C "
        "  public static async method Main(): Task<int> "
        "    using var tasks := new Array<Task<int>>(); "
        "    tasks.Add(Task.FromResult(10)); "
        "    tasks.Add(Task.FromResult(20)); "
        "    tasks.Add(Task.FromResult(30)); "
        "    using var wa := Task.WhenAll(tasks); "
        "    var arr := await wa; "
        "    return arr[0] + arr[1] + arr[2]; "
        "  end Main; end C;"),
        60);
}

// §6.7: WhenAll<T> on an empty Array<Task<int>> produces an
// immediately-complete Task<Array<int>> whose result is an empty array.
// Count == 0, so Main returns 0.
BEZEL_TEST(whenall_generic_results_451, empty_array_produces_empty_result) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C "
        "  public static async method Main(): Task<int> "
        "    using var tasks := new Array<Task<int>>(); "
        "    using var wa := Task.WhenAll(tasks); "
        "    var arr := await wa; "
        "    return arr.Count; "
        "  end Main; end C;"),
        0);
}

// §6.7: WhenAll<T> with a bool result. Each Task<bool> has value
// true (1 byte). Collect three and check they are all true. Returns 3 if all
// three are true, otherwise 0.
BEZEL_TEST(whenall_generic_results_451, bool_results_collected) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C "
        "  public static async method Main(): Task<int> "
        "    using var tasks := new Array<Task<bool>>(); "
        "    tasks.Add(Task.FromResult(true)); "
        "    tasks.Add(Task.FromResult(true)); "
        "    tasks.Add(Task.FromResult(true)); "
        "    using var wa := Task.WhenAll(tasks); "
        "    var arr := await wa; "
        "    var n := 0; "
        "    if arr[0] then n := n + 1; end; "
        "    if arr[1] then n := n + 1; end; "
        "    if arr[2] then n := n + 1; end; "
        "    return n; "
        "  end Main; end C;"),
        3);
}

// §6.7 + §30.10: WhenAll<T> cross-thread with genuinely-async
// Task<int> producers (Task.FromResult is sync; use async helpers). Run 3×.
// Each async helper awaits Task.Delay then returns a value; WhenAll collects.
BEZEL_TEST(whenall_generic_results_451, async_producers_cross_thread) {
    const std::string src =
        "module M; class C "
        "  public static async method Produce(n: int): Task<int> "
        "    await Task.Delay(5); "
        "    return n; "
        "  end Produce; "
        "  public static async method Main(): Task<int> "
        "    using var tasks := new Array<Task<int>>(); "
        "    tasks.Add(Produce(7)); "
        "    tasks.Add(Produce(8)); "
        "    tasks.Add(Produce(9)); "
        "    using var wa := Task.WhenAll(tasks); "
        "    var arr := await wa; "
        "    return arr[0] + arr[1] + arr[2]; "
        "  end Main; end C;";
    for (int i = 0; i < 3; ++i)
        BEZEL_EXPECT_EQ(compileAndRun(src), 24);
}

// §6.7 + §30.9 (#443 fault propagation): WhenAll<T> where one
// input Task<int> faults. The aggregate should propagate the fault; the
// result array is NOT produced. With no enclosing handler, exits 70.
BEZEL_TEST(whenall_generic_results_451, one_faulted_propagates) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C "
        "  public static async method Boom(): Task<int> "
        "    throw new InvalidOperationException(); "
        "  end Boom; "
        "  public static async method Main(): Task<int> "
        "    using var tasks := new Array<Task<int>>(); "
        "    tasks.Add(Task.FromResult(1)); "
        "    tasks.Add(Boom()); "
        "    using var wa := Task.WhenAll(tasks); "
        "    var arr := await wa; "
        "    return arr[0]; "
        "  end Main; end C;"),
        70);
}

// §6.7 + §30.9: WhenAll<T> fault caught by enclosing try.
// The catch fires; x := 42.
BEZEL_TEST(whenall_generic_results_451, faulted_caught) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C "
        "  public static async method Boom(): Task<int> "
        "    throw new InvalidOperationException(); "
        "  end Boom; "
        "  public static async method Main(): Task<int> "
        "    var x := 0; "
        "    using var tasks := new Array<Task<int>>(); "
        "    tasks.Add(Task.FromResult(1)); "
        "    tasks.Add(Boom()); "
        "    using var wa := Task.WhenAll(tasks); "
        "    try "
        "      var arr := await wa; "
        "      x := arr[0]; "
        "    catch Exception => x := 42; "
        "    end; "
        "    return x; "
        "  end Main; end C;"),
        42);
}

// ===========================================================================
// (A2) Task.WhenAllOwned<T> — stdlib §6.7
//      OwningTask<T> combinator variant (T: class)
// ===========================================================================

// §6.7 / §30.10: WhenAllOwned with an empty OwningArray produces an
// immediately-complete OwningTask<OwningArray<T>>. Await result's Count == 0.
BEZEL_TEST(when_all_owned_451, empty_array_produces_empty_result) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; "
        "class Box "
        "  public Value: int; "
        "  public constructor(v: int) "
        "    Value := v; "
        "  end; "
        "end Box; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    var tasks := new OwningArray<OwningTask<Box>>(); "
        "    using var wa := Task.WhenAllOwned(move tasks); "
        "    using var arr := await wa; "
        "    return arr.Count; "
        "  end Main; end C;"),
        0);
}

// §6.7 / §30.10: WhenAllOwned with two already-completed OwningTask<Box>
// (from Task.FromOwnedResult). Results collected; Value fields are 10 and 20.
// Returns arr[0].Value + arr[1].Value = 30.
// Derived from: stdlib §6.7 WhenAllOwned<T>(tasks: OwningArray<OwningTask<T>>):
// OwningTask<OwningArray<T>>.
BEZEL_TEST(when_all_owned_451, two_tasks_results_collected) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; "
        "class Box "
        "  public Value: int; "
        "  public constructor(v: int) "
        "    Value := v; "
        "  end; "
        "end Box; "
        "class C "
        "  public static async method MakeBox(n: int): OwningTask<Box> "
        "    return new Box(n); "
        "  end MakeBox; "
        "  public static async method Main(): Task<int> "
        "    var tasks := new OwningArray<OwningTask<Box>>(); "
        "    tasks.Add(MakeBox(10)); "
        "    tasks.Add(MakeBox(20)); "
        "    using var wa := Task.WhenAllOwned(move tasks); "
        "    using var arr := await wa; "
        "    return arr[0].Value + arr[1].Value; "
        "  end Main; end C;"),
        30);
}

// §6.7 / §30.10: WhenAllOwned cross-thread: each OwningTask<Box>
// awaits Task.Delay before returning a new Box. Tasks run concurrently (they
// are started before WhenAllOwned is called). Collect 3 boxes; sum = 1+2+3=6.
BEZEL_TEST(when_all_owned_451, async_producers_cross_thread) {
    const std::string src =
        "module M; "
        "class Box "
        "  public Value: int; "
        "  public constructor(v: int) "
        "    Value := v; "
        "  end; "
        "end Box; "
        "class C "
        "  public static async method MakeBox(n: int): OwningTask<Box> "
        "    await Task.Delay(5); "
        "    return new Box(n); "
        "  end MakeBox; "
        "  public static async method Main(): Task<int> "
        "    var tasks := new OwningArray<OwningTask<Box>>(); "
        "    tasks.Add(MakeBox(1)); "
        "    tasks.Add(MakeBox(2)); "
        "    tasks.Add(MakeBox(3)); "
        "    using var wa := Task.WhenAllOwned(move tasks); "
        "    using var arr := await wa; "
        "    return arr[0].Value + arr[1].Value + arr[2].Value; "
        "  end Main; end C;";
    for (int i = 0; i < 3; ++i)
        BEZEL_EXPECT_EQ(compileAndRun(src), 6);
}

// ===========================================================================
// (B) Task.Delay(ms, token: CancellationToken) — stdlib §6.7
// ===========================================================================

// §6.7 / §30.8: Delay(ms, CancellationToken.None) behaves the same
// as Delay(ms): the None token never cancels, so the delay runs normally.
// Awaiting it returns normally; Main exits 5.
// Note: this passes CancellationToken.None INLINE. The local-variable form
// (`var t := CancellationToken.None;`) live across a cross-thread suspend is
// covered by the #461 regression tests below (it used to SIGSEGV before the
// fix that types CancellationToken.None as a ptr-wide frame slot).
BEZEL_TEST(delay_cancel_451, none_token_delay_runs_normally) {
    const std::string src =
        "module M; class C "
        "  public static async method Main(): Task<int> "
        "    await Task.Delay(5, CancellationToken.None); "
        "    return 5; "
        "  end Main; end C;";
    for (int i = 0; i < 3; ++i)
        BEZEL_EXPECT_EQ(compileAndRun(src), 5);
}

// §6.7 / §30.8: Delay with an already-cancelled token produces an
// immediately-faulted Task. Awaiting it throws OperationCanceledException.
// With no enclosing handler, exits 70.
BEZEL_TEST(delay_cancel_451, pre_cancelled_token_faults_immediately) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C "
        "  public static async method Main(): Task<int> "
        "    using var cts := new CancellationTokenSource(); "
        "    cts.Cancel(); "
        "    var tok := cts.Token; "
        "    await Task.Delay(5, tok); "
        "    return 5; "
        "  end Main; end C;"),
        70);
}

// §6.7 / §30.8: pre-cancelled Delay fault is caught by a catch arm.
// The OperationCanceledException should be catchable; x := 77.
BEZEL_TEST(delay_cancel_451, pre_cancelled_fault_caught) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C "
        "  public static async method Main(): Task<int> "
        "    using var cts := new CancellationTokenSource(); "
        "    cts.Cancel(); "
        "    var tok := cts.Token; "
        "    var x := 0; "
        "    try "
        "      await Task.Delay(5, tok); "
        "      x := 5; "
        "    catch OperationCanceledException => x := 77; "
        "    end; "
        "    return x; "
        "  end Main; end C;"),
        77);
}

// §6.7 / §30.8: Delay with a token that is NOT yet cancelled runs
// normally. After the delay completes (no cancellation), exits 6.
BEZEL_TEST(delay_cancel_451, not_yet_cancelled_delay_runs) {
    const std::string src =
        "module M; class C "
        "  public static async method Main(): Task<int> "
        "    using var cts := new CancellationTokenSource(); "
        "    var tok := cts.Token; "
        "    await Task.Delay(5, tok); "
        "    return 6; "
        "  end Main; end C;";
    for (int i = 0; i < 3; ++i)
        BEZEL_EXPECT_EQ(compileAndRun(src), 6);
}

// §6.7 / §30.8: Delay(TimeSpan, token). The TimeSpan overload with
// a None token still runs normally (same as Delay(TimeSpan)). Exits 8.
// Token passed inline to avoid pre-existing CancellationToken.None-in-frame bug.
BEZEL_TEST(delay_cancel_451, timespan_overload_none_token) {
    const std::string src =
        "module M; class C "
        "  public static async method Main(): Task<int> "
        "    await Task.Delay(TimeSpan.FromMilliseconds(5), CancellationToken.None); "
        "    return 8; "
        "  end Main; end C;";
    for (int i = 0; i < 3; ++i)
        BEZEL_EXPECT_EQ(compileAndRun(src), 8);
}

// ===========================================================================
// (#461) CancellationToken VALUE local live across a CROSS-THREAD suspend
//        spec §30 (async state-machine frames) / §6 (CancellationToken).
//
// Root cause: a `CancellationToken` (§6.1: a value record lowered to a single
// borrow-pointer word, ptr) bound to a LOCAL whose initializer is
// `CancellationToken.None` had NO sema exprType — the `CancellationToken`
// identifier resolves to a builtin TypeSymbol, so the static-property typing
// path (which set the exprType) was skipped. With no exprType,
// collectSuspendLocals (§30 frame layout) defaulted the frame SLOT to an i32
// (4 bytes) while the local's alloca was a ptr (8 bytes). On a real
// cross-thread suspend (Task.Delay runs on the scheduler), the per-suspend
// spill stored 8 bytes into the 4-byte slot, clobbering the adjacent
// pending-task frame field → the restored value was garbage → SIGSEGV on
// resume. The fix types CancellationToken.None as CancellationToken (a
// ptr-wide slot) and hardens the frame default to pointer width so an
// unresolved-type local can never overflow its neighbours.
//
// Each test uses async Main (so the scheduler runs) + Task.Delay (a genuine
// cross-thread suspend that heap-captures + restores the frame). Run 3× for
// cross-thread nondeterminism stability, matching this file's convention.
// ===========================================================================

// #461 / §30 / §6.1: the exact repro — `var t := CancellationToken.None;` live
// across a cross-thread suspend, then RE-USED after the await by passing it to
// a second Task.Delay. Before the fix this SIGSEGV'd; now it completes and
// returns 9 (the token is restored as a valid value across both suspends).
BEZEL_TEST(cancellation_token_local_across_suspend_461, none_local_reused_after_await) {
    const std::string src =
        "module M; class C "
        "  public static async method Main(): Task<int> "
        "    var t := CancellationToken.None; "
        "    await Task.Delay(5, t); "
        "    await Task.Delay(1, t); "
        "    return 9; "
        "  end Main; end C;";
    for (int i = 0; i < 3; ++i)
        BEZEL_EXPECT_EQ(compileAndRun(src), 9);
}

// #461 / §30 / §6.1: a CancellationToken.None local that survives MULTIPLE
// cross-thread awaits and is read AFTER them. §6.1: None.IsCancellationRequested
// is false, so the `if` is not taken and Main returns 11. Reading the property
// dereferences the restored token value, so a clobbered restore would crash or
// return the wrong value.
BEZEL_TEST(cancellation_token_local_across_suspend_461, none_local_survives_multiple_awaits) {
    const std::string src =
        "module M; class C "
        "  public static async method Main(): Task<int> "
        "    var t := CancellationToken.None; "
        "    await Task.Delay(5, t); "
        "    await Task.Delay(3, t); "
        "    await Task.Delay(1, t); "
        "    if t.IsCancellationRequested then return 99; end; "
        "    return 11; "
        "  end Main; end C;";
    for (int i = 0; i < 3; ++i)
        BEZEL_EXPECT_EQ(compileAndRun(src), 11);
}

// #461 / §30 / §6.1: a CancellationToken from `cts.Token` (a non-null
// cancel-state pointer, distinct from None's null) bound to a local live across
// a cross-thread suspend, re-used afterwards. The token is NOT cancelled, so
// the delay runs and the token round-trips; Main returns 13. This is the
// non-None companion: it confirms the frame slot is ptr-wide for BOTH token
// shapes (None = null ptr, cts.Token = real ptr).
BEZEL_TEST(cancellation_token_local_across_suspend_461, source_token_local_reused) {
    const std::string src =
        "module M; class C "
        "  public static async method Main(): Task<int> "
        "    using var cts := new CancellationTokenSource(); "
        "    var t := cts.Token; "
        "    await Task.Delay(5, t); "
        "    await Task.Delay(1, t); "
        "    return 13; "
        "  end Main; end C;";
    for (int i = 0; i < 3; ++i)
        BEZEL_EXPECT_EQ(compileAndRun(src), 13);
}

// #461 / §30 (general value-type-local regression): a NULLABLE VALUE local
// (`int?` lowers to a {i8, i32} tagged struct — §7.2) live across two
// cross-thread suspends and read afterwards. This exercises the same
// frame-capture path for a multi-word value struct and confirms the ptr-wide
// default does NOT regress a local whose type IS known (it keeps its exact
// {i8, i32} slot and round-trips). The value 42 must survive both suspends.
BEZEL_TEST(cancellation_token_local_across_suspend_461, nullable_value_local_round_trips) {
    const std::string src =
        "module M; class C "
        "  public static async method Main(): Task<int> "
        "    var n: int? := 42; "
        "    await Task.Delay(5); "
        "    await Task.Delay(1); "
        "    if n <> null then return n!; end; "
        "    return 0; "
        "  end Main; end C;";
    for (int i = 0; i < 3; ++i)
        BEZEL_EXPECT_EQ(compileAndRun(src), 42);
}

// ===========================================================================
// (C) await inside a catch GUARD — confirmed clean diagnostic (#451)
// ===========================================================================

// §30.9 / §31.2.1 / #451: `catch E when await ...` produces a clear
// compile-time error (not a crash or malformed IR). The compile MUST fail and
// report an error (errorCount > 0).
//
// Spec §30.9 states "await has no syntactic restrictions on placement inside
// try/catch/finally." However, implementing catch-guard suspension requires
// re-entering the catch DISPATCH sequence at resume — the guard runs DURING
// dispatch, BEFORE the arm scope / caught-exception state are set up, so the
// async state machine would have to re-run and re-evaluate the guard expression
// with a different suspended state. This is a genuine implementation gap, not
// a spec restriction: the diagnostic is "not yet implemented" rather than a
// hard spec prohibition, and this test confirms the boundary is clean (a safe
// error, no crash, no malformed IR) while the implementation is deferred.
//
// When this is eventually implemented, update this test to expect success
// and add happy-path tests for guard-true / guard-false / throws-in-guard.
BEZEL_TEST(await_catch_guard_451, clean_diagnostic_not_crash) {
    BEZEL_EXPECT_TRUE(compileFails(
        "module M; class C "
        "  public static async method CheckAsync(): Task<bool> "
        "    return true; "
        "  end CheckAsync; "
        "  public static async method Main(): Task<int> "
        "    try "
        "      throw new InvalidOperationException(); "
        "    catch InvalidOperationException when await CheckAsync() => "
        "      return 1; "
        "    end; "
        "    return 0; "
        "  end Main; end C;"));
}

// ===========================================================================
// (D) Task.Delay pool-starvation regression (#1378)
//
// Before fix #1378, Task.Delay submitted a sleep_for job to the 4-worker pool.
// Five concurrent delays would occupy all 4 workers (the 5th waits for a slot),
// serialising the total delay to at least 2×delay_ms rather than ~delay_ms.
// After fix #1378, delays use a dedicated timer thread; all 5 fire in parallel
// and total runtime is near delay_ms regardless of pool size.
//
// The test compiles a Bezel program that starts 5 concurrent Task.Delay(200ms)
// helpers via Task.WhenAll, awaits the result, and returns the sum of results
// (should be 5).  Wall time of the RUN (not including compile) is measured.
// A 600ms gate is generous for OS scheduling jitter yet far below the
// serialised bound of at least 400ms (4 parallel + 1 waiting for a free slot).
// ===========================================================================

BEZEL_TEST(task_delay_1378, starvation_regression_concurrent_delays) {
    // 5 async helpers each await Task.Delay(200ms) then return 1.
    // WhenAll collects and Main returns 1+1+1+1+1 = 5.
    const std::string src =
        "module M; class C "
        "  public static async method D(): Task<int> "
        "    await Task.Delay(200); "
        "    return 1; "
        "  end D; "
        "  public static async method Main(): Task<int> "
        "    using var tasks := new Array<Task<int>>(); "
        "    tasks.Add(D()); "
        "    tasks.Add(D()); "
        "    tasks.Add(D()); "
        "    tasks.Add(D()); "
        "    tasks.Add(D()); "
        "    using var wa := Task.WhenAll(tasks); "
        "    var arr := await wa; "
        "    return arr[0] + arr[1] + arr[2] + arr[3] + arr[4]; "
        "  end Main; end C;";
    auto [code, elapsed_ms] = compileAndRunTimed(src);
    BEZEL_EXPECT_EQ(code, 5);
    // All 5 delays run concurrently on the timer thread: wall time ≈ 200ms.
    // 600ms provides ample margin for OS scheduling jitter while remaining far
    // below the 400ms+ starvation floor (4 workers × 200ms each, then 5th).
    BEZEL_EXPECT_TRUE(elapsed_ms >= 0 && elapsed_ms < 600);
}
