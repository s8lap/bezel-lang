// Codegen — Task static combinator methods (stdlib §6.7 / §30.10).
//
// Tests for Task.FromResult<T>, Task.WhenAll, Task.WhenAny, Task.FireAndForget,
// and `using var` Task disposal. These derive from Bezel_Standard_Library_0_130.md
// §6.7 (lines 2852-2886) and language spec §30.5 / §30.10.
//
// Like async_test.cpp, the async emitter accumulates LLVM module state across
// runs, so these tests compile to native executables and check exit codes.
// ASan/LSan is used to verify no leaks or double-frees.

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
#include "test_harness.hpp"
#include "native_test_harness.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

namespace {

namespace fs = std::filesystem;

// Compile `source` to a temp native executable via the real Driver and run it;
// returns the exit code, or -1 on compile/link/run failure.
int compileAndRun(const std::string& source) {
    static int counter = 0;
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_taskcomb_" + std::to_string(counter++);
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

// LeakSanitizer verification: returns 0=clean, 1=sanitizer fired, -1=skip.
int runUnderLeakSanitizer(const std::string& source) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;  // toolchain unavailable or compile failed

    static int counter = 0;
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_taskcomb_san_" + std::to_string(counter++);
    std::string objPath = (dir / (stem + ".o")).string();
    std::string exe     = (dir / stem).string();

    {
        bezel::DiagnosticEngine diags;
        bezel::Lexer lexer(source, diags);
        bezel::Parser parser(lexer, diags);
        auto unit = parser.parseCompilationUnit();
        if (!unit) return -1;
        bezel::sema::SemaContext ctx(diags);
        bezel::sema::collectSymbols(*unit, ctx);
        bezel::sema::resolveTypes(*unit, ctx);
        bezel::sema::typecheckExpressions(*unit, ctx);
        bezel::sema::checkDefiniteAssignment(*unit, ctx);
        bezel::ownership::checkOwnership(*unit, ctx);
        if (diags.hasErrors()) return -1;
        llvm::LLVMContext llvmCtx;
        auto module = bezel::codegen::emitModule(*unit, ctx, llvmCtx);
        if (!module) return -1;
        std::string err;
        if (!bezel::codegen::emitObjectFile(*module, objPath, err)) return -1;
    }

    auto cleanup = [&] {
        std::remove(objPath.c_str());
        std::remove(exe.c_str());
    };

    std::string linkCmd = "clang++ -fsanitize=address -g " + objPath + " " +
                          rtObj + " -lm -lpthread -lstdc++ -o " + exe +
                          " > /dev/null 2>&1";
    if (std::system(linkCmd.c_str()) != 0) { cleanup(); return -1; }

    std::string runCmd =
        "ASAN_OPTIONS=detect_leaks=1:exitcode=99 " + exe + " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return -1;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 99) return 1;  // sanitizer
    return 0;
}

// Assert correctness AND LSan cleanliness.
void expectValueLeakFree(const std::string& source, int expectedExitCode) {
    BEZEL_EXPECT_EQ(compileAndRun(source), expectedExitCode);
    int lsan = runUnderLeakSanitizer(source);
    if (lsan == -1) return;
    BEZEL_EXPECT_EQ(lsan, 0);
}

}  // namespace

// ============================================================
// stdlib §6.7 line 2855: Task.FromResult<T>(value: T): Task<T>
// ============================================================

// §6.7 / §30.10: `var x := await Task.FromResult(42)` — the task is already
// complete; `await` takes the fast path and returns 42. Main exits with 42.
// Per stdlib §6.7: FromResult produces an already-completed Task<T>.
BEZEL_TEST(task_combinators_6_7, from_result_int_await_sync) {
    expectValueLeakFree(
        "module M; class C "
        "  public static async method Main(): Task<int> "
        "    var x := await Task.FromResult(42); "
        "    return x; "
        "  end Main; end C;",
        42);
}

// §6.7: Task.FromResult with a boolean argument (1-byte value type). Verifies
// that the value store/load round-trip through the result buffer is correct
// for non-int types. Main exits 1 if true, 0 if false.
BEZEL_TEST(task_combinators_6_7, from_result_bool_await_sync) {
    expectValueLeakFree(
        "module M; class C "
        "  public static async method Main(): Task<int> "
        "    var b := await Task.FromResult(true); "
        "    if b then return 1; end; "
        "    return 0; "
        "  end Main; end C;",
        1);
}

// §6.7: Task.FromResult in a local bound to a `using var`. The task is freed
// at scope end by the using-var disposal (bz_task_free) — exactly once. No
// await: the using-var local is not consumed. LSan verifies no leak.
BEZEL_TEST(task_combinators_6_7, from_result_using_var_disposed) {
    expectValueLeakFree(
        "module M; class C "
        "  public static async method Main(): Task<int> "
        "    using var t := Task.FromResult(7); "
        "    return 7; "
        "  end Main; end C;",
        7);
}

// §6.7 / §30.5: `using var t := Task.FromResult(...)` and then `await t`.
// After the await, `t` is NOT freed by lowerAwait (lowerAwait only frees
// anonymous CallExpr operands; `t` is an IdentExpr). The using-var scope-end
// then frees it (bz_task_free) — exactly once. LSan verifies no double-free
// and no leak. Exit code is 55 (chosen to avoid collision with sanitizer
// sentinel exit codes like 99).
BEZEL_TEST(task_combinators_6_7, from_result_using_var_await_then_dispose) {
    expectValueLeakFree(
        "module M; class C "
        "  public static async method Main(): Task<int> "
        "    using var t := Task.FromResult(55); "
        "    var x := await t; "
        "    return x; "
        "  end Main; end C;",
        55);
}

// ============================================================
// stdlib §6.7 line 2872: Task.WhenAll(tasks: Array<Task>): Task
// ============================================================

// §6.7: Task.WhenAll on an array of already-completed tasks (from
// Task.FromResult). WhenAll completes immediately when all are already done.
// Main exits 5. NOTE: individual tasks added to Array<Task> are borrowed (the
// array does not own them); their lifetime tracking is a separate ownership
// concern. This test validates correctness only (not LSan-clean by design since
// the borrowing Array<Task> cannot free its elements).
BEZEL_TEST(task_combinators_6_7, when_all_already_complete) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C "
        "  public static async method Main(): Task<int> "
        "    using var tasks := new Array<Task>(); "
        "    tasks.Add(Task.FromResult(1)); "
        "    tasks.Add(Task.FromResult(2)); "
        "    tasks.Add(Task.FromResult(3)); "
        "    using var wa := Task.WhenAll(tasks); "
        "    await wa; "
        "    return 5; "
        "  end Main; end C;"),
        5);
}

// §30.9 / §30.4 (#855): a fault inside a task passed to Task.WhenAll IS caught by
// a try/catch around `await Task.WhenAll(...)` — the WhenAll await re-raises the
// aggregate's exception into the awaiting frame's active catch. The tasks are held
// by an OWNING OwningArray<Task> (the correct idiom: a borrowing Array<Task> does
// not own the tasks, so storing fresh producers there leaks them and their
// unobserved fault surfaces as unhandled — the #1020 footgun, NOT an async-routing
// bug). Verified: the original #855 repro used a borrowing `Array<Task>`; with an
// owning collection both the WhenAll await and a direct element await catch (rc=55).
// Value-only (correctness): the faulted-task error-object disposal interaction with
// the catch arm is tracked by #854 (the double-await/error_obj reclaim lane).
BEZEL_TEST(task_combinators_6_7, when_all_owned_array_faulted_task_caught_855) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C "
        "  public static async method Boom(): Task "
        "    throw new InvalidOperationException(\"f\"); end Boom; "
        "  public static async method Main(): Task<int> "
        "    using var tasks := new OwningArray<Task>(); "
        "    tasks.Add(Boom()); "
        "    var x := 0; "
        "    try await Task.WhenAll(tasks); catch Exception => x := 55; end; "
        "    return x; "
        "  end Main; end C;"),
        55);
}

// §30.9 / §30.4 (#855): awaiting an owning-collection element that faulted is also
// caught (the same correct await-re-raise routing, independent of WhenAll).
BEZEL_TEST(task_combinators_6_7, await_owned_array_element_faulted_task_caught_855) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C "
        "  public static async method Boom(): Task "
        "    throw new InvalidOperationException(\"f\"); end Boom; "
        "  public static async method Main(): Task<int> "
        "    using var tasks := new OwningArray<Task>(); "
        "    tasks.Add(Boom()); "
        "    var x := 0; "
        "    try await tasks[0]; catch Exception => x := 55; end; "
        "    return x; "
        "  end Main; end C;"),
        55);
}

// §30.4 / §30.9 (#854): awaiting the SAME BOUND faulted Task twice (each in its
// own try/catch) no longer SIGSEGVs. The re-raised error_obj is BORROWED from the
// task (codegen marks it), so the first catch does NOT reclaim it and the second
// await re-reads a LIVE object — both arms catch, rc = 44 + 44 = 88 (was crash
// 139). Value-only: the residual error_obj leak for a multi-awaited bound task is
// the site-4 follow-up (#854 comment); the crash + the §30.4 multi-await value
// are the load-bearing fix.
BEZEL_TEST(task_combinators_6_7, double_await_bound_faulted_task_no_crash_854) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C "
        "  public static async method Boom(): Task<int> "
        "    throw new InvalidOperationException(\"x\"); end Boom; "
        "  public static async method Main(): Task<int> "
        "    var t := Boom(); "
        "    var a := 0; var b := 0; "
        "    try a := await t; catch InvalidOperationException => a := 44; end; "
        "    try b := await t; catch InvalidOperationException => b := 44; end; "
        "    return a + b; "
        "  end Main; end C;"),
        88);
}

// §30.4 (#854) regression: double-await of a COMPLETED (non-faulted) bound task
// still returns the result twice (the borrowed-exc path is fault-only).
BEZEL_TEST(task_combinators_6_7, double_await_completed_task_ok_854) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C "
        "  public static async method Calc(): Task<int> return 7; end Calc; "
        "  public static async method Main(): Task<int> "
        "    var t := Calc(); "
        "    var a := await t; var b := await t; "
        "    return a + b; "
        "  end Main; end C;"),
        14);
}

// §6.7: Task.WhenAll on an empty array completes immediately.
// Empty array means no input tasks → no leak concern.
BEZEL_TEST(task_combinators_6_7, when_all_empty_array) {
    expectValueLeakFree(
        "module M; class C "
        "  public static async method Main(): Task<int> "
        "    using var tasks := new Array<Task>(); "
        "    using var wa := Task.WhenAll(tasks); "
        "    await wa; "
        "    return 3; "
        "  end Main; end C;",
        3);
}

// §6.7: Task.WhenAll with Task.Delay tasks — genuinely cross-thread.
// WhenAll completes after ALL delays elapse. Run 3x for stability.
// NOTE: individual Task.Delay tasks in the Array<Task> are borrowed by the
// array and cannot be freed by it (Array<Task> is a borrowing collection).
// Their lifetime is managed by the scheduler infrastructure (the delay workers
// run to completion). This test validates cross-thread correctness only.
BEZEL_TEST(task_combinators_6_7, when_all_with_delays_cross_thread) {
    const std::string src =
        "module M; class C "
        "  public static async method Main(): Task<int> "
        "    using var tasks := new Array<Task>(); "
        "    tasks.Add(Task.Delay(5)); "
        "    tasks.Add(Task.Delay(5)); "
        "    using var wa := Task.WhenAll(tasks); "
        "    await wa; "
        "    return 4; "
        "  end Main; end C;";
    for (int i = 0; i < 3; ++i)
        BEZEL_EXPECT_EQ(compileAndRun(src), 4);
}

// ============================================================
// stdlib §6.7 line 2884: Task.WhenAny(tasks: Array<Task>): Task<int>  (v0.162/#1332)
// ============================================================
// WhenAny now produces Task<int> (winner INDEX), not Task<Task> (winner task ptr).
// Task<Task> was unconstructible under §30.5; an int index is unambiguous and safe.

// §6.7 / #1332: Task.WhenAny on two already-completed tasks. The first one completed
// wins; WhenAny itself is also immediately complete. `await wa` yields an int (the
// winner's index in the tasks array, 0-based). Main exits 8.
// NOTE: individual tasks in the Array<Task> are borrowed (no disposal from array).
// Correctness-only test (not LSan-clean by design).
BEZEL_TEST(task_combinators_6_7, when_any_already_complete) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C "
        "  public static async method Main(): Task<int> "
        "    using var tasks := new Array<Task>(); "
        "    tasks.Add(Task.FromResult(10)); "
        "    tasks.Add(Task.FromResult(20)); "
        "    using var wa := Task.WhenAny(tasks); "
        "    await wa; "
        "    return 8; "
        "  end Main; end C;"),
        8);
}

// §6.7 / #1332: WhenAny index — verify `await wa` produces the winner index (0 or 1).
// With two already-completed tasks, the first one (index 0) wins. Main exits with 0.
// NOTE: individual tasks in the Array<Task> are borrowed (no disposal from array).
// Correctness-only test.
BEZEL_TEST(task_combinators_6_7, when_any_yields_winner_index) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C "
        "  public static async method Main(): Task<int> "
        "    using var tasks := new Array<Task>(); "
        "    tasks.Add(Task.FromResult(10)); "
        "    tasks.Add(Task.FromResult(20)); "
        "    using var wa := Task.WhenAny(tasks); "
        "    var idx := await wa; "
        "    return idx; "          // 0 — first task completed first (already-done)
        "  end Main; end C;"),
        0);
}

// §6.7 / #1332: Task.WhenAny with Task.Delay — genuinely cross-thread (first delay to
// fire wins). Run 3x for nondeterminism stability. Correctness-only test.
BEZEL_TEST(task_combinators_6_7, when_any_with_delay_cross_thread) {
    const std::string src =
        "module M; class C "
        "  public static async method Main(): Task<int> "
        "    using var tasks := new Array<Task>(); "
        "    tasks.Add(Task.Delay(5)); "
        "    tasks.Add(Task.Delay(20)); "
        "    using var wa := Task.WhenAny(tasks); "
        "    await wa; "
        "    return 6; "
        "  end Main; end C;";
    for (int i = 0; i < 3; ++i)
        BEZEL_EXPECT_EQ(compileAndRun(src), 6);
}

// §6.7 / #1332: WhenAny Delay-race index — shorter delay wins index 0 (5ms vs 500ms).
// The first-done task is index 0; winner index is 0. Run 3x for stability.
BEZEL_TEST(task_combinators_6_7, when_any_delay_race_winner_index) {
    const std::string src =
        "module M; class C "
        "  public static async method Main(): Task<int> "
        "    using var tasks := new Array<Task>(); "
        "    tasks.Add(Task.Delay(5)); "
        "    tasks.Add(Task.Delay(500)); "
        "    using var wa := Task.WhenAny(tasks); "
        "    var idx := await wa; "
        "    return idx; "  // 0 — the 5ms delay always wins over 500ms
        "  end Main; end C;";
    for (int i = 0; i < 3; ++i)
        BEZEL_EXPECT_EQ(compileAndRun(src), 0);
}

// ============================================================
// #1501 / stdlib §6.7 (line 2884) §30.10 / §30.15: Task.WhenAny over an
// INLINE Array<Task> literal of Channel<T>.Receive() tasks — the natural
// "select" idiom. Before the fix, `Task.WhenAny([ta, tb])` (an inline
// CollectionLiteralExpr argument, as opposed to every pre-existing test
// above which binds the array to a named `using var tasks` local first)
// silently lowered to null: WhenAny is a compiler intrinsic with no
// MethodDecl param, so sema never gave the literal a materialization
// target, and a bare lowerExpr returns null for a CollectionLiteralExpr
// per §15.4 (it is not a general expression value). The whole
// `using var w := Task.WhenAny([…])` store then never executed, leaving
// `w` holding whatever garbage was already on the native stack; the
// subsequent `await w` called bz_task_is_done on that garbage pointer and
// SIGSEGV'd/aborted before any output — 100% reproducible (issue #1501).
// ============================================================

// #1501: `Task.WhenAny([ta, tb])` where ta is an already-done Receive (a
// value pre-sent on channel `a`) and tb is a genuinely pending Receive on
// an otherwise-idle channel `b` — the exact repro shape from the issue.
// Must not crash; the already-done ta wins (index 0); `await ta` then
// yields the sent value. `tb` is left pending and disposed (still pending)
// at Main's scope end — the §30.15 v0.179 D14 abandonment case (tracked
// separately at #1524; this test only asserts #1501's crash is gone, not
// that abandonment redelivers the message). Looped: the crash this guards
// was a race — WhenAny's continuation on the already-done `ta` is enqueued
// (not run inline), so `await w` almost always genuinely suspends the async
// state machine before the temp array (and `tb`) get disposed, which is
// exactly the interleaving that exposed the bug (garbage stack reads across
// the suspend/resume boundary, and — in an earlier, reverted fix attempt
// that deferred the temp array's free to scope-end instead of freeing it
// synchronously — a flaky heap-corrupting double free).
BEZEL_TEST(task_combinators_1501, when_any_inline_literal_over_channel_receive) {
    const std::string src =
        "module M; import System.Threading; class C "
        "  public static async method Main(): Task<int> "
        "    using var a := new Channel<int>(); "
        "    using var b := new Channel<int>(); "
        "    using var tx := a.NewSender(); "
        "    tx.Send(42); "
        "    using var ta := a.Receive(); "
        "    using var tb := b.Receive(); "
        "    using var w := Task.WhenAny([ta, tb]); "
        "    var idx := await w; "
        "    if idx = 0 then "
        "      var v := await ta; "
        "      return v; "
        "    end; "
        "    return -1; "
        "  end Main; end C;";
    for (int i = 0; i < 20; ++i)
        BEZEL_EXPECT_EQ(compileAndRun(src), 42);
}

// #1501: same idiom, LSan-checked once for leak/corruption cleanliness. The
// literal's temp Array<Task> is freed synchronously right after
// bz_task_when_any borrows it (not via scope-end disposal — seeing this stay
// clean also guards against reintroducing a deferred-free that leaks or
// double-frees the temp). Both channels are pre-sent (both Receives are
// already-done) so NEITHER task is still pending at its channel's scope-end
// dispose — a still-pending Receive left abandoned at dispose is the
// separately-tracked §30.15 v0.179 D14 leak (#1524; confirmed present even
// with no WhenAny involved at all), which would make this LSan-clean
// assertion fail for a reason unrelated to #1501.
BEZEL_TEST(task_combinators_1501, when_any_inline_literal_over_channel_receive_lsan) {
    expectValueLeakFree(
        "module M; import System.Threading; class C "
        "  public static async method Main(): Task<int> "
        "    using var a := new Channel<int>(); "
        "    using var b := new Channel<int>(); "
        "    using var tx := a.NewSender(); "
        "    using var ty := b.NewSender(); "
        "    tx.Send(42); "
        "    ty.Send(99); "
        "    using var ta := a.Receive(); "
        "    using var tb := b.Receive(); "
        "    using var w := Task.WhenAny([ta, tb]); "
        "    var idx := await w; "
        "    if idx = 0 then "
        "      var v := await ta; "
        "      return v; "
        "    end; "
        "    return -1; "
        "  end Main; end C;",
        42);
}

// ============================================================
// stdlib §6.7 line 2885: Task.FireAndForget(owned task: Task)
// ============================================================

// §6.7: Task.FireAndForget with an already-completed task. The task is freed
// by the FireAndForget continuation (registered on an already-done task, so
// the continuation fires immediately). LSan verifies the task is freed.
BEZEL_TEST(task_combinators_6_7, fire_and_forget_completed_task) {
    expectValueLeakFree(
        "module M; class C "
        "  public static async method Main(): Task<int> "
        "    Task.FireAndForget(Task.FromResult(1)); "
        "    return 2; "
        "  end Main; end C;",
        2);
}

// §6.7: Task.FireAndForget with a Task.Delay (genuinely async). Ownership
// transfers; the caller does not await. The process exits after bz_scheduler_
// shutdown (async Main entry drains the pool). Verifies no hang and no leak.
BEZEL_TEST(task_combinators_6_7, fire_and_forget_delay_task_cross_thread) {
    const std::string src =
        "module M; class C "
        "  public static async method Main(): Task<int> "
        "    Task.FireAndForget(Task.Delay(1)); "
        "    return 9; "
        "  end Main; end C;";
    for (int i = 0; i < 3; ++i)
        BEZEL_EXPECT_EQ(compileAndRun(src), 9);
    int lsan = runUnderLeakSanitizer(src);
    if (lsan != -1) BEZEL_EXPECT_EQ(lsan, 0);
}

// ============================================================
// §30.5 / stdlib §6.7: using-var Task disposal
// ============================================================

// §30.5: A `using var` Task that is NOT awaited is freed at scope end.
// bz_task_free is called exactly once (at scope exit). LSan verifies no leak.
BEZEL_TEST(task_combinators_30_5, using_var_not_awaited_no_leak) {
    expectValueLeakFree(
        "module M; class C "
        "  public static async method Main(): Task<int> "
        "    using var t := Task.Delay(0); "
        "    return 11; "
        "  end Main; end C;",
        11);
}

// §30.5: A `using var` WhenAll task freed at scope end (not awaited). Verifies
// the disposal path fires for WhenAll's returned task. The individual tasks added
// to Array<Task> are borrowed (not LSan-clean since they're not freed), but the
// WhenAll aggregate task IS freed via using-var disposal. Correctness-only test.
BEZEL_TEST(task_combinators_30_5, using_var_when_all_not_awaited_no_leak) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C "
        "  public static async method Main(): Task<int> "
        "    using var tasks := new Array<Task>(); "
        "    tasks.Add(Task.FromResult(1)); "
        "    using var wa := Task.WhenAll(tasks); "
        "    return 12; "
        "  end Main; end C;"),
        12);
}

// ===========================================================================
// §30.9 / #443 (WhenAll fault propagation): bz_task_when_all must propagate
// faults from any input task to the aggregate. Before the fix the completion
// lambda always called bz_task_complete(aggregate, null, 0) (success) even
// when an input task was faulted, so awaiting the aggregate appeared to succeed
// and any fault was silently swallowed.
// ===========================================================================

// §30.9 / #443: WhenAll where ONE input task faults and the other succeeds.
// Awaiting the aggregate should re-raise the fault. With no enclosing handler
// the process exits 70 (unhandled exception boundary, §33.3). Before the fix
// this exited 5 (success — the fault was swallowed).
BEZEL_TEST(task_combinators_443, when_all_one_faulted_raises) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C "
        "  public static async method Boom(): Task "
        "    throw new InvalidOperationException(); "
        "  end Boom; "
        "  public static async method Main(): Task<int> "
        "    using var tasks := new Array<Task>(); "
        "    tasks.Add(Task.FromResult(1)); "
        "    tasks.Add(Boom()); "
        "    using var wa := Task.WhenAll(tasks); "
        "    await wa; "
        "    return 5; "
        "  end Main; end C;"),
        70);
}

// §30.9 / #443: WhenAll where the fault is CAUGHT by a try around the await.
// The catch should fire; x := 9.
BEZEL_TEST(task_combinators_443, when_all_one_faulted_caught) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C "
        "  public static async method Boom(): Task "
        "    throw new InvalidOperationException(); "
        "  end Boom; "
        "  public static async method Main(): Task<int> "
        "    var x := 0; "
        "    using var tasks := new Array<Task>(); "
        "    tasks.Add(Task.FromResult(1)); "
        "    tasks.Add(Boom()); "
        "    using var wa := Task.WhenAll(tasks); "
        "    try await wa; catch Exception => x := 9; end; "
        "    return x; "
        "  end Main; end C;"),
        9);
}

// §30.9 / #443: WhenAll all-success is unaffected by the fault-propagation fix
// (regression guard). Two already-completed tasks; WhenAll completes with
// success; await returns normally; Main exits 5.
BEZEL_TEST(task_combinators_443, when_all_all_success_unaffected) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C "
        "  public static async method Main(): Task<int> "
        "    using var tasks := new Array<Task>(); "
        "    tasks.Add(Task.FromResult(1)); "
        "    tasks.Add(Task.FromResult(2)); "
        "    using var wa := Task.WhenAll(tasks); "
        "    await wa; "
        "    return 5; "
        "  end Main; end C;"),
        5);
}

// §30.9 / #443: WhenAll with a genuinely-async (Task.Delay) faulted input, run
// cross-thread. The fault propagates to the aggregate and the process exits 70.
// Run 3x for nondeterminism stability.
BEZEL_TEST(task_combinators_443, when_all_delay_then_fault_cross_thread) {
    const std::string src =
        "module M; class C "
        "  public static async method BoomAfterDelay(): Task "
        "    await Task.Delay(5); "
        "    throw new InvalidOperationException(); "
        "  end BoomAfterDelay; "
        "  public static async method Main(): Task<int> "
        "    using var tasks := new Array<Task>(); "
        "    tasks.Add(Task.FromResult(1)); "
        "    tasks.Add(BoomAfterDelay()); "
        "    using var wa := Task.WhenAll(tasks); "
        "    await wa; "
        "    return 5; "
        "  end Main; end C;";
    for (int i = 0; i < 3; ++i)
        BEZEL_EXPECT_EQ(compileAndRun(src), 70);
}

// ============================================================
// §30.3 (#942): Task.IsCompleted / IsFaulted / IsCancelled property reads
// ============================================================

// §30.3: a Task.FromResult task is already completed → IsCompleted is true.
// Regression for #942 (all three state properties returned false unconditionally).
BEZEL_TEST(task_state_942, from_result_is_completed) {
    expectValueLeakFree(
        "module M; class C public static method Main(): int "
        "  using var tc := Task.FromResult<int>(42); "
        "  return (1 if tc.IsCompleted else 0); "
        "end Main; end C;",
        1);
}

// §30.3: a successful task is neither faulted nor cancelled.
BEZEL_TEST(task_state_942, from_result_not_faulted_not_cancelled) {
    expectValueLeakFree(
        "module M; class C public static method Main(): int "
        "  using var tc := Task.FromResult<int>(42); "
        "  if tc.IsFaulted then return 9; end; "
        "  if tc.IsCancelled then return 8; end; "
        "  return 0; "
        "end Main; end C;",
        0);
}

// §30.3: a Task.FromException task is completed AND faulted, but NOT cancelled
// (its error is not an OperationCanceledException).
// #962 RESOLVED: a FromException task disposed without being awaited/caught used
// to leak its exception object, but the #854 bound-faulted-task scope-end
// disposal (LlvmBackend_Setup.cpp ~L12203 — bz_task_is_error → dispose error_obj
// before fire-and-forget) now reclaims it. Verified LSan-clean (this assertion is
// now expectValueLeakFree, upgraded from value-only).
BEZEL_TEST(task_state_942, from_exception_is_faulted_not_cancelled) {
    expectValueLeakFree(
        "module M; class C public static method Main(): int "
        "  using var tf := Task.FromException<int>("
        "      new InvalidOperationException(\"x\")); "
        "  if not tf.IsCompleted then return 7; end; "
        "  if tf.IsCancelled then return 8; end; "
        "  return (1 if tf.IsFaulted else 0); "
        "end Main; end C;",
        1);
}

// §30.3: a Task.FromCancelled task is completed AND cancelled, and — because
// faulted and cancelled are distinct states — NOT faulted (the error is an
// OperationCanceledException, distinguished by its vtable).
// #962 RESOLVED (see above): now leak-checked.
BEZEL_TEST(task_state_942, from_cancelled_is_cancelled_not_faulted) {
    expectValueLeakFree(
        "module M; class C public static method Main(): int "
        "  using var tc := Task.FromCancelled<int>(); "
        "  if not tc.IsCompleted then return 7; end; "
        "  if tc.IsFaulted then return 8; end; "
        "  return (1 if tc.IsCancelled else 0); "
        "end Main; end C;",
        1);
}

// §30.3 / #962 regression: a FromException task with a USER exception type
// (carrying a destructor) disposed without await is also reclaimed leak-free.
BEZEL_TEST(task_state_942, from_exception_user_type_unawaited_leak_free_962) {
    expectValueLeakFree(
        "module M; "
        "class MyErr: Exception public constructor(m: string) base(m); end constructor; end MyErr; "
        "class C public static method Main(): int "
        "  using var tf := Task.FromException<int>(new MyErr(\"boom\")); "
        "  return (1 if tf.IsFaulted else 0); "
        "end Main; end C;",
        1);
}
