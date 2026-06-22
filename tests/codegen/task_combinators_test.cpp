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
// stdlib §6.7 line 2884: Task.WhenAny(tasks: Array<Task>): Task<Task>
// ============================================================

// §6.7: Task.WhenAny on two already-completed tasks. The first one completed
// wins; WhenAny itself is also immediately complete. Main exits 8.
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

// §6.7: Task.WhenAny with Task.Delay — genuinely cross-thread (first delay to
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
// Value-only (compileAndRun, not expectValueLeakFree): a FromException task
// disposed without being awaited/caught leaks its exception object — a
// pre-existing gap tracked as #962, independent of the #942 state-property fix.
BEZEL_TEST(task_state_942, from_exception_is_faulted_not_cancelled) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C public static method Main(): int "
        "  using var tf := Task.FromException<int>("
        "      new InvalidOperationException(\"x\")); "
        "  if not tf.IsCompleted then return 7; end; "
        "  if tf.IsCancelled then return 8; end; "
        "  return (1 if tf.IsFaulted else 0); "
        "end Main; end C;"),
        1);
}

// §30.3: a Task.FromCancelled task is completed AND cancelled, and — because
// faulted and cancelled are distinct states — NOT faulted (the error is an
// OperationCanceledException, distinguished by its vtable).
// Value-only for the same pre-existing-leak reason as above (#962).
BEZEL_TEST(task_state_942, from_cancelled_is_cancelled_not_faulted) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C public static method Main(): int "
        "  using var tc := Task.FromCancelled<int>(); "
        "  if not tc.IsCompleted then return 7; end; "
        "  if tc.IsFaulted then return 8; end; "
        "  return (1 if tc.IsCancelled else 0); "
        "end Main; end C;"),
        1);
}
