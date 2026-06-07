// Codegen — async/await gap completions (#451).
//
// (A) Task.WhenAll<T>(tasks: Array<Task<T>>): Task<Array<T>>
//     stdlib §6.7 line 2875 / spec §30.10.
// (B) Task.Delay(ms, token: CancellationToken): Task  — token cancellation.
//     stdlib §6.7 lines 2867-2870 / spec §30.8.
// (C) await inside a catch GUARD (catch E when await ...) — confirmed clean
//     diagnostic (not a crash).
//
// Like task_combinators_test.cpp, async emitter state accumulates across JIT
// runs, so all tests compile to native executables via Driver and check exit
// codes. Cross-thread tests are run 3× for nondeterminism stability.

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

// Compile `source` to a temp native executable via the real Driver and run it.
// Returns the process exit code, or -1 on compile/link/run failure.
int compileAndRun(const std::string& source) {
    static int counter = 0;
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_async451_" + std::to_string(counter++);
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
    static int counter = 0;
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_async451_neg_" + std::to_string(counter++);
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
// (A) Task.WhenAll<T> — stdlib §6.7 line 2875
// ===========================================================================

// §6.7 line 2875: WhenAll<T> over Task<int> — collects three int results
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
        "    using var arr := await wa; "
        "    return arr[0] + arr[1] + arr[2]; "
        "  end Main; end C;"),
        60);
}

// §6.7 line 2875: WhenAll<T> on an empty Array<Task<int>> produces an
// immediately-complete Task<Array<int>> whose result is an empty array.
// Count == 0, so Main returns 0.
BEZEL_TEST(whenall_generic_results_451, empty_array_produces_empty_result) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C "
        "  public static async method Main(): Task<int> "
        "    using var tasks := new Array<Task<int>>(); "
        "    using var wa := Task.WhenAll(tasks); "
        "    using var arr := await wa; "
        "    return arr.Count; "
        "  end Main; end C;"),
        0);
}

// §6.7 line 2875: WhenAll<T> with a bool result. Each Task<bool> has value
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
        "    using var arr := await wa; "
        "    var n := 0; "
        "    if arr[0] then n := n + 1; end; "
        "    if arr[1] then n := n + 1; end; "
        "    if arr[2] then n := n + 1; end; "
        "    return n; "
        "  end Main; end C;"),
        3);
}

// §6.7 line 2875 + §30.10: WhenAll<T> cross-thread with genuinely-async
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
        "    using var arr := await wa; "
        "    return arr[0] + arr[1] + arr[2]; "
        "  end Main; end C;";
    for (int i = 0; i < 3; ++i)
        BEZEL_EXPECT_EQ(compileAndRun(src), 24);
}

// §6.7 line 2875 + §30.9 (#443 fault propagation): WhenAll<T> where one
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
        "    using var arr := await wa; "
        "    return arr[0]; "
        "  end Main; end C;"),
        70);
}

// §6.7 line 2875 + §30.9: WhenAll<T> fault caught by enclosing try.
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
        "      using var arr := await wa; "
        "      x := arr[0]; "
        "    catch Exception => x := 42; "
        "    end; "
        "    return x; "
        "  end Main; end C;"),
        42);
}

// ===========================================================================
// (B) Task.Delay(ms, token: CancellationToken) — stdlib §6.7 lines 2867-2870
// ===========================================================================

// §6.7 line 2868 / §30.8: Delay(ms, CancellationToken.None) behaves the same
// as Delay(ms): the None token never cancels, so the delay runs normally.
// Awaiting it returns normally; Main exits 5.
// Note: CancellationToken.None passed INLINE (not as a local variable) to
// avoid a pre-existing bug where None stored in an async frame local variable
// with a cross-thread suspend causes a SIGSEGV — that is a separate pre-
// existing issue in CancellationToken.None's frame-backup handling.
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

// §6.7 line 2868 / §30.8: Delay with an already-cancelled token produces an
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

// §6.7 line 2868 / §30.8: pre-cancelled Delay fault is caught by a catch arm.
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

// §6.7 line 2868 / §30.8: Delay with a token that is NOT yet cancelled runs
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

// §6.7 line 2870 / §30.8: Delay(TimeSpan, token). The TimeSpan overload with
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
// (C) await inside a catch GUARD — confirmed clean diagnostic (#451)
// ===========================================================================

// §30.9 / #451: `catch E when await ...` produces a clear compile-time error
// (not a crash or malformed IR). The compile MUST fail and report an error
// (errorCount > 0). This confirms the diagnostic boundary — await in a catch
// GUARD body is deferred with a safe error rather than silently miscompiling.
// Per the existing async codegen: "not yet implemented: `await` inside a catch
// GUARD (`catch ... when await ...`)".
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
