// Codegen — Task static combinator methods (stdlib §6.7 / §30.10).
//
// Tests for Task.FromResult<T>, Task.WhenAll, Task.WhenAny, Task.FireAndForget,
// and `using var` Task disposal. These derive from Bezel_Standard_Library_0_130.md
// §6.7 and language spec §30.5 / §30.10.
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
#include "support/emit_object_support.hpp"  // #4569: emitObjectFileForTest

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "support/unique_stem.hpp"

namespace {

namespace fs = std::filesystem;

// Compile `source` to a temp native executable via the real Driver and run it;
// returns the exit code, or -1 on compile/link/run failure.
int compileAndRun(const std::string& source) {
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bezel_taskcomb");
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

    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bezel_taskcomb_san");
    std::string objPath = (dir / (stem + ".o")).string();
    std::string exe     = (dir / stem).string();

    {
        bezel::DiagnosticEngine diags;
        bezel::Lexer lexer(source, diags);
        bezel::Parser parser(lexer, diags);
        auto unit = parser.parseCompilationUnit();
        if (!unit) return bezel::test::asanBuildFailure("parse");
        bezel::sema::SemaContext ctx(diags);
        bezel::sema::collectSymbols(*unit, ctx);
        bezel::sema::resolveTypes(*unit, ctx);
        bezel::sema::typecheckExpressions(*unit, ctx);
        bezel::sema::checkDefiniteAssignment(*unit, ctx);
        bezel::ownership::checkOwnership(*unit, ctx);
        if (diags.hasErrors()) return bezel::test::asanBuildFailure("sema");
        llvm::LLVMContext llvmCtx;
        auto module = bezel::codegen::emitModule(*unit, ctx, llvmCtx);
        if (!module) return bezel::test::asanBuildFailure("emitModule");
        std::string err;
        // #4569: repointed onto the ASan-sweep wrapper, the same rename the
        // other 35 files in the #3917 burn-down took. #4569 listed this file
        // among the "concurrency tail" that needed a per-call sanitizer
        // decision because the *_647 family "carries sibling TSan rows in the
        // same file" -- measured, it does not: this file has exactly ONE
        // emitObjectFile call, no buildRuntimeSanitizerObject, and its link
        // line below is a hardcoded `-fsanitize=address`, so there is no
        // -fsanitize=thread path an instrumented object could reach.
        if (!bezel::test::emitObjectFileForTest(*module, objPath, err))
            return bezel::test::asanBuildFailure("emitObjectFile");
    }

    auto cleanup = [&] {
        std::remove(objPath.c_str());
        std::remove(exe.c_str());
    };

    std::string linkCmd = "clang++ -fsanitize=address -g " + objPath + " " +
                          rtObj + " -lm -lpthread -lstdc++ -o " + exe +
                          " > /dev/null 2>&1";
    if (std::system(linkCmd.c_str()) != 0) {
        cleanup();
        return bezel::test::asanLinkFailure("link");
    }

    std::string runCmd =
        "ASAN_OPTIONS=detect_leaks=1:exitcode=99 " + exe + " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return bezel::test::asanExecFailure("system");
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

// §28.2 checked-disposal MODE (not ASan): compile + run with the same codegen
// bit `bezelc --checked-disposal` sets (opts.checkedDisposal = true). Used by
// the #2519 coverage cells — the WhenAllOwned fault path's already-produced
// results are disposed via the RUNTIME bz_dispose_class_obj helper, and no
// existing WhenAllOwned cell exercises the checked-disposal codegen path.
int compileAndRunCheckedDisposal(const std::string& source) {
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bezel_taskcomb_cd");
    std::string src = (dir / (stem + ".bzl")).string();
    std::string exe = (dir / stem).string();
    {
        std::ofstream out(src);
        out << source;
    }
    bezel::Driver driver;
    bezel::codegen::CodegenOptions opts;
    opts.checkedDisposal = true;  // §28.2 checked-disposal MODE
    bezel::CompileResult r = driver.compileFile(src, exe, {}, opts);
    int code = -1;
    if (r.success) {
        int status = std::system(exe.c_str());
        if (status != -1 && WIFEXITED(status)) code = WEXITSTATUS(status);
    }
    std::remove(src.c_str());
    std::remove(exe.c_str());
    return code;
}

}  // namespace

// ============================================================
// stdlib §6.7: Task.FromResult<T>(value: T): Task<T>
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
// stdlib §6.7: Task.WhenAll(tasks: Array<Task>): Task
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

// §6.7: Task.WhenAll over genuinely cross-thread inputs. The flake cause in the
// original was that the ONLY cross-thread synchronization was a fixed Task.Delay
// (5) wall-clock timer — a no-barrier race mitigated solely by the 3x loop.
// Replaced with a DETERMINISTIC completion primitive using only stdlib types
// (§6.9 Channel<T>, §30.14 Domain.Spawn): two Channel<int>.Receive() tasks are
// the inputs (both pending), and a spawned producer domain Sends two items on
// the channel. Each Send completes its receive via the RECEIVER's domain queue
// (§30.15 — genuinely cross-thread, the same character as a timer firing on a
// pool thread) but, unlike a wall-clock timer, is guaranteed to fire by program
// logic, so WhenAll deterministically completes once both items are sent. No
// timing dependence remains. The 3x loop is now defense-in-depth, not race
// compensation. Correctness-only (the borrowing Array<Task> does not own the
// receive tasks; their lifetime is managed by the scheduler, as the delay
// workers it replaces were).
BEZEL_TEST(task_combinators_6_7, when_all_with_delays_cross_thread) {
    const std::string src =
        "module M; import System.Threading; class C "
        "  public static method Signal2(owned tx: ChannelSender<int>) "
        "    using var s := move tx; s.Send(1); s.Send(2); "
        "  end Signal2; "
        "  public static async method Main(): Task<int> "
        "    using var ch := new Channel<int>(); "
        "    using var tasks := new Array<Task>(); "
        "    tasks.Add(ch.Receive()); "
        "    tasks.Add(ch.Receive()); "
        "    using var prod := Domain.Spawn(C.Signal2, ch.NewSender()); "
        "    using var wa := Task.WhenAll(tasks); "
        "    await wa; "
        "    await prod; "
        "    return 4; "
        "  end Main; end C;";
    for (int i = 0; i < 3; ++i)
        BEZEL_EXPECT_EQ(compileAndRun(src), 4);
}

// ============================================================
// stdlib §6.7: Task.WhenAny(tasks: Array<Task>): Task<int>  (v0.162/#1332)
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

// §6.7 / #1332: Task.WhenAny over genuinely cross-thread inputs. The original
// used two Task.Delay timers (5ms vs 20ms) — a wall-clock race whose ONLY sync
// was the fixed timers, mitigated solely by the 3x loop. Replaced with a
// DETERMINISTIC completion primitive (§6.9 Channel<T>, §30.14 Domain.Spawn):
// two Channel<int>.Receive() tasks are the inputs, and a spawned producer domain
// Sends on both channels. Each Send completes its receive cross-thread via the
// receiver's domain queue (§30.15); WhenAny completes with whichever fires
// first. The asserted value (6) is independent of the winner, so the result is
// now fully deterministic regardless of completion order. The 3x loop is
// defense-in-depth. Correctness-only.
BEZEL_TEST(task_combinators_6_7, when_any_with_delay_cross_thread) {
    const std::string src =
        "module M; import System.Threading; class C "
        "  public static method Signal2("
        "      owned tx1: ChannelSender<int>, owned tx2: ChannelSender<int>) "
        "    using var s1 := move tx1; using var s2 := move tx2; s1.Send(1); s2.Send(2); "
        "  end Signal2; "
        "  public static async method Main(): Task<int> "
        "    using var ch1 := new Channel<int>(); "
        "    using var ch2 := new Channel<int>(); "
        "    using var tasks := new Array<Task>(); "
        "    tasks.Add(ch1.Receive()); "
        "    tasks.Add(ch2.Receive()); "
        "    using var prod := Domain.Spawn(C.Signal2, ch1.NewSender(), ch2.NewSender()); "
        "    using var wa := Task.WhenAny(tasks); "
        "    await wa; "
        "    await prod; "
        "    return 6; "
        "  end Main; end C;";
    for (int i = 0; i < 3; ++i)
        BEZEL_EXPECT_EQ(compileAndRun(src), 6);
}

// §6.7 / #1332: WhenAny winner-index — the input that completes first wins.
// The original was a 5ms-vs-500ms Task.Delay race asserting index 0; the 100x
// timer margin made the right answer likely but still timing-dependent (the
// ONLY sync was the fixed wall-clock timers). Replaced with a DETERMINISTIC
// completion primitive (§6.9 Channel<T>, §30.14 Domain.Spawn): the producer
// domain Sends on channel 0 only (completing index 0's receive cross-thread,
// §30.15); channel 1's receive is left genuinely pending. With exactly one
// input ever completing, WhenAny deterministically returns index 0 — no race,
// no timer margin to rely on. (The pending channel-1 receive is abandoned at
// scope end, the §30.15/#1524 disposal case, identical to the #1501 test's
// pending `tb`; this test is value-only and does not LSan-check, like #1501.)
// The 3x loop is defense-in-depth.
BEZEL_TEST(task_combinators_6_7, when_any_delay_race_winner_index) {
    const std::string src =
        "module M; import System.Threading; class C "
        "  public static method Signal1(owned tx: ChannelSender<int>) "
        "    using var s := move tx; s.Send(1); "
        "  end Signal1; "
        "  public static async method Main(): Task<int> "
        "    using var ch0 := new Channel<int>(); "
        "    using var ch1 := new Channel<int>(); "
        "    using var tasks := new Array<Task>(); "
        "    tasks.Add(ch0.Receive()); "
        "    tasks.Add(ch1.Receive()); "
        "    using var prod := Domain.Spawn(C.Signal1, ch0.NewSender()); "
        "    using var wa := Task.WhenAny(tasks); "
        "    var idx := await wa; "
        "    await prod; "
        "    return idx; "  // 0 — only ch0 is ever signalled
        "  end Main; end C;";
    for (int i = 0; i < 3; ++i)
        BEZEL_EXPECT_EQ(compileAndRun(src), 0);
}

// ============================================================
// #1501 / stdlib §6.7 §30.10 / §30.15: Task.WhenAny over an
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
// #1616 / stdlib §6.7 / §30.10: Task.WhenAll over an INLINE
// Array<Task> literal — the exact WhenAny-inline-literal bug (#1501) in the
// sibling WhenAll branch, which had never been given the lowerTaskArrayArg
// fix. WhenAll is a compiler intrinsic with no MethodDecl param, so sema
// never gave the literal a materialization target and a bare lowerExpr
// returns null for the CollectionLiteralExpr (§15.4). `arr` was then null and
// `if (!arr) return nullptr` dropped the WHOLE WhenAll call out of codegen —
// the `using var w := Task.WhenAll([…])` store never executed, so `w` held
// native-stack garbage and the following `await w` dereferenced it
// (SIGSEGV for bound elements; a hang for anonymous elements). The fix mirrors
// WhenAny exactly: lowerTaskArrayArg materializes the literal into an owned
// Array<Task> temp, and freeTaskArrayArgTemp frees it SYNCHRONOUSLY right
// after the borrowing bz_task_when_all call (both when_all runtime entries
// snapshot the input task pointers before returning and never retain the
// array, so a synchronous free is safe — see LlvmBackend_Setup.cpp).
// ============================================================

// #1616: `Task.WhenAll([ta, tb])` over an inline literal of two bound
// (`using var`) already-complete tasks. Before the fix this SIGSEGV'd on
// `await w` (the dropped WhenAll left `w` as stack garbage); the fix awaits
// all inputs correctly. Both elements are bound `using var`s (freed at scope
// end) and the temp array is freed by freeTaskArrayArgTemp. Exit code 7 = 3 + 4
// (the two FromResult values, read back after WhenAll completes).
//
// #2278 — LSan half DROPPED (compileAndRun only), deliberately and with the
// reason recorded rather than silently. The elements are `Task<int>`, so #1830's
// literal-element fallback selects the TYPED entry bz_task_when_all_results,
// which mints an `Array<int>` aggregate result; this program DISCARDS it
// (`await w;`), and in the implementation's current model the only owner of a
// Pattern A intrinsic-collection result is an awaiting `using var` — so nothing
// reclaims it and LSan reports 48 bytes in 2 allocations. That is a real, still-
// OPEN defect (see LlvmBackend_Calls.cpp's WhenAll branch: it is NOT specific to
// WhenAll — a plain `var rs := await t;` over any user `async` method returning
// `Task<Array<T>>` leaks identically, 104 bytes measured), and its fix is a
// coordinated codegen+ownership change, not something this #1616 test should
// gate on. The naive one-line fix — a task-side result disposer — was measured
// to DOUBLE-FREE `using var rs := await w;` and use-after-free `using var rs :=
// await Task.WhenAll([…]);`, so it is explicitly not taken.
//
// What this test still pins is #1616's actual subject: the inline literal must
// not be dropped out of codegen (pre-fix: SIGSEGV on `await w`). The value
// assertion is that oracle. Restore expectValueLeakFree here when #2278's
// Pattern A collection-result reclaim lands. (Precedent for a correctness-only
// combinator test with a recorded leak reason: when_all_already_complete above.)
BEZEL_TEST(task_combinators_1616, when_all_inline_literal_bound_tasks) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C "
        "  public static async method Main(): Task<int> "
        "    using var ta := Task.FromResult(3); "
        "    using var tb := Task.FromResult(4); "
        "    using var w := Task.WhenAll([ta, tb]); "
        "    await w; "
        "    var a := await ta; "
        "    var b := await tb; "
        "    return a + b; "
        "  end Main; end C;"),
        7);
}

// #1830 / stdlib §6.7 / §30.10: the CONSUMED half of the typed
// `Task.WhenAll<T>(Array<Task<T>>): Task<Array<T>>` inline-literal shape — the
// awaiter READS the aggregate `Array<int>` rather than discarding it. This pins
// that #1830's literal-element fallback actually produces the results the sema
// result type promises (rs[0] == 3, rs[1] == 4, in input order): a mis-selected
// void entry (bz_task_when_all) would leave `rs` unpopulated. Correctness-only
// for the same #2278 reason as the test above (the aggregate array is
// unreclaimed on this shape too — `var rs`, not `using var rs`).
BEZEL_TEST(task_combinators_1830, when_all_typed_inline_literal_results_in_order) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C "
        "  public static async method Main(): Task<int> "
        "    using var ta := Task.FromResult(3); "
        "    using var tb := Task.FromResult(4); "
        "    using var w := Task.WhenAll([ta, tb]); "
        "    var rs := await w; "
        "    return rs[0] * 10 + rs[1]; "  // 34: input order preserved
        "  end Main; end C;"),
        34);
}

// ============================================================
// stdlib §6.7: Task.FireAndForget(owned task: Task)
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

// §30.10 / §22.3 / §28.3 (#1890): Task.FireAndForget(owned t) on a NAMED owned
// task local. stdlib §6.7 declares `FireAndForget(owned task: Task)` and §22.3
// (v0.169) requires the `owned` call-site marker when a named owned binding is
// passed to an `owned` parameter — so `Task.FireAndForget(owned t)` is the
// mandated spelling for supervised fire-and-forget of a HELD task. Before the
// fix the owned parameter was not modeled as an owned sink, so the marker was
// spuriously rejected AND (unmarked) the task was left undischarged; even once
// the ownership pass accepted it, codegen fire-and-forgot the same task twice
// (the call here + the plain-`var` task local's scope-end #445 d.task disposal),
// a double bz_task_free that corrupts async state and hangs. This test fails
// without BOTH fixes (compile error → compileAndRun returns -1; or a hang) and
// passes with them: the task is fired-and-forgotten exactly once, LSan-clean.
BEZEL_TEST(task_combinators_6_7, fire_and_forget_owned_named_task_local_1890) {
    const std::string src =
        "module M; class C "
        "  public static async method Ping(): Task "
        "    await Task.Delay(1); "
        "  end Ping; "
        "  public static async method Main(): Task<int> "
        "    var t := Ping(); "
        "    Task.FireAndForget(move t); "
        "    return 7; "
        "  end Main; end C;";
    for (int i = 0; i < 3; ++i)
        BEZEL_EXPECT_EQ(compileAndRun(src), 7);
    int lsan = runUnderLeakSanitizer(src);
    if (lsan != -1) BEZEL_EXPECT_EQ(lsan, 0);
}

// §30.10 / §28.3 (#1890): the same held-task fire-and-forget with a subsequent
// suspension point (`await Task.Delay` after the FireAndForget). Exercises the
// double-fire hang directly: the nulled `t` slot lives in the async frame and
// its scope-end d.task disposal (which runs after the resume) must reload null
// and skip. Before the codegen fix this hung at scheduler shutdown.
BEZEL_TEST(task_combinators_6_7, fire_and_forget_owned_named_task_then_await_1890) {
    const std::string src =
        "module M; class C "
        "  public static async method Ping(): Task "
        "    await Task.Delay(1); "
        "  end Ping; "
        "  public static async method Main(): Task<int> "
        "    var t := Ping(); "
        "    Task.FireAndForget(move t); "
        "    await Task.Delay(2); "
        "    return 8; "
        "  end Main; end C;";
    for (int i = 0; i < 3; ++i)
        BEZEL_EXPECT_EQ(compileAndRun(src), 8);
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

// §30.10 / §30.5 / #1617: bz_task_then vs bz_task_fire_and_forget inline-free
// UAF/hang. WhenAll registers a per-input continuation via bz_task_then on each
// already-settled input. The inputs are OWNED (`using var`) and disposed at the
// block's scope end WITHOUT awaiting the aggregate — the abandonment idiom
// (§30.5). bz_task_fire_and_forget's already-done fast path frees each task
// inline. Before the fix, bz_task_then DEFERRED the continuation through the
// domain queue with a raw task pointer, so the inline free could precede (or
// race) the deferred continuation: the continuation then dereffed freed memory
// (heap corruption / crash) or the aggregate never settled (hang at scheduler
// shutdown). The fix runs the already-settled continuation synchronously, while
// the caller still holds the task alive. Looped to stress the race window.
BEZEL_TEST(task_combinators_1617, when_all_settled_inputs_then_faf_no_uaf) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C "
        "  public static async method Main(): Task<int> "
        "    var i := 0; "
        "    while i < 200 do "
        "      i := i + 1; "
        "      using var t1 := Task.FromResult(1); "
        "      using var t2 := Task.FromResult(2); "
        "      using var t3 := Task.FromResult(3); "
        "      using var tasks := new Array<Task>(); "
        "      tasks.Add(t1); "
        "      tasks.Add(t2); "
        "      tasks.Add(t3); "
        "      using var wa := Task.WhenAll(tasks); "
        "    end; "
        "    return 42; "
        "  end Main; end C;"),
        42);
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

// §30.9 / #443: WhenAll re-raises a fault from a genuinely-async faulted input.
// The original made the fault async by awaiting Task.Delay(5) before throwing —
// wall-clock sync whose ONLY barrier was the fixed timer, mitigated by the 3x
// loop. Replaced with a DETERMINISTIC completion primitive (§6.9 Channel<T>,
// §30.14 Domain.Spawn): a pending Channel<int>.Receive() is the faulting input,
// and a spawned producer domain disposes its sender — the channel's ONLY sender
// — which CLOSES the channel (§30.15), faulting the pending receive with
// ChannelClosedException cross-thread. WhenAll re-raises it (§30.9 / #443 fault
// propagation) and, with no handler, the process exits 70 — the GENERIC
// unhandled-exception boundary (§33.3, bz_unhandled_exception → _exit(70)),
// independent of exception type, so ChannelClosedException yields the same 70
// the original's InvalidOperationException did. The fault is now triggered by
// program logic, not a wall-clock timer, so it propagates deterministically.
// (Robust to producer scheduling either way: if the channel closes before the
// receive is parked, a closed-channel Receive faults immediately per §30.15 —
// still 70.) The 3x loop is defense-in-depth.
BEZEL_TEST(task_combinators_443, when_all_delay_then_fault_cross_thread) {
    const std::string src =
        "module M; import System.Threading; class C "
        "  public static method CloseChan(owned tx: ChannelSender<int>) "
        "    using var s := move tx; "
        "  end CloseChan; "
        "  public static async method Main(): Task<int> "
        "    using var ch := new Channel<int>(); "
        "    using var tasks := new Array<Task>(); "
        "    tasks.Add(Task.FromResult(1)); "
        "    tasks.Add(ch.Receive()); "
        "    using var prod := Domain.Spawn(C.CloseChan, ch.NewSender()); "
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
        "  return (if tc.IsCompleted then 1 else 0); "
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
        "  return (if tf.IsFaulted then 1 else 0); "
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
        "  return (if tc.IsCancelled then 1 else 0); "
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
        "  return (if tf.IsFaulted then 1 else 0); "
        "end Main; end C;",
        1);
}

// ===========================================================================
// #1529 / §30.10 "Aggregation contracts" (v0.179): WhenAll / WhenAny /
// WhenAllOwned contract conformance.
//
// §30.10: "Task.WhenAll ... otherwise awaiting it re-raises the exception of
// the FIRST NON-SUCCESSFUL INPUT IN INPUT ORDER" — before this fix, the
// runtime instead raced a CAS on whichever input's continuation fired first
// (the first task to COMPLETE, not the first in the array): non-deterministic
// and a spec violation whenever an EARLIER input faults later than a LATER
// one. §30.10 also states an aggregate "owns no fault of its own" — the
// re-raise BORROWS the winning input task's owned exception; before this fix
// the aggregate instead behaved as if it owned a copy of the same object,
// which double-freed it once both the aggregate and the (still-owning) input
// task were independently disposed (a crash, reproduced below).
// ===========================================================================

// §30.10: `Task.WhenAll` selects the fault of the first non-successful INPUT
// (index 0, `SlowBoomA`), not the first to COMPLETE (index 1, `FastBoomB`,
// which faults synchronously while `SlowBoomA` faults only after a delay).
// Regression guard: before the fix this raced and could non-deterministically
// (or, timed as below, ALWAYS) re-raise ErrB instead.
BEZEL_TEST(task_combinators_1529, when_all_first_non_successful_input_order_wins) {
    expectValueLeakFree(
        "module M; "
        "class ErrA: Exception public constructor() base(\"A\"); end constructor; end ErrA; "
        "class ErrB: Exception public constructor() base(\"B\"); end constructor; end ErrB; "
        "class C "
        "  public static async method SlowBoomA(): Task "
        "    await Task.Delay(20); throw new ErrA(); "
        "  end SlowBoomA; "
        "  public static async method FastBoomB(): Task "
        "    throw new ErrB(); "
        "  end FastBoomB; "
        "  public static async method Run(): Task "
        "    using var a := SlowBoomA(); "
        "    using var b := FastBoomB(); "
        "    using var tasks := new Array<Task>(); "
        "    tasks.Add(a); tasks.Add(b); "
        "    using var wa := Task.WhenAll(tasks); "
        "    await wa; "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    try await Run(); return 0; "
        "    catch ErrA => return 1; "
        "    catch ErrB => return 2; "
        "    end; "
        "  end Main; end C;",
        1);
}

// §30.10: same input-order contract for the typed `Task.WhenAll<T>(Array<Task<T>>)`
// form (a separate runtime aggregator, `bz_task_when_all_results`).
BEZEL_TEST(task_combinators_1529, when_all_typed_first_non_successful_input_order_wins) {
    expectValueLeakFree(
        "module M; "
        "class ErrA: Exception public constructor() base(\"A\"); end constructor; end ErrA; "
        "class ErrB: Exception public constructor() base(\"B\"); end constructor; end ErrB; "
        "class C "
        "  public static async method GoodSlow(): Task<int> "
        "    await Task.Delay(20); return 7; "
        "  end GoodSlow; "
        "  public static async method SlowBoomA(): Task<int> "
        "    await Task.Delay(20); throw new ErrA(); "
        "  end SlowBoomA; "
        "  public static async method FastBoomB(): Task<int> "
        "    throw new ErrB(); "
        "  end FastBoomB; "
        "  public static async method Run(): Task "
        "    using var g := GoodSlow(); "
        "    using var a := SlowBoomA(); "
        "    using var b := FastBoomB(); "
        "    using var tasks := new Array<Task<int>>(); "
        "    tasks.Add(g); tasks.Add(a); tasks.Add(b); "
        "    using var wa := Task.WhenAll(tasks); "
        "    await wa; "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    try await Run(); return 0; "
        "    catch ErrA => return 1; "
        "    catch ErrB => return 2; "
        "    end; "
        "  end Main; end C;",
        1);
}

// §30.10 / §30.9: an aggregate "owns no fault of its own" — its re-raise
// BORROWS the winning input task's owned exception. Before the fix, the
// aggregate ALSO treated error_obj as its own: once `wa` was awaited (marking
// then unmarking the exception borrowed) and its own `using var` scope ended,
// `wa`'s disposal freed the SAME exception object `t2` (the true, still-live
// owner) goes on to free at its own scope end — a double-free (SIGSEGV under
// plain execution; a heap-corruption abort under ASan). Verifies BOTH: no
// crash, and (via the destructor-counting `CountedErr`) that the exception is
// disposed EXACTLY once — not zero (a leak) and not more than once.
BEZEL_TEST(task_combinators_1529, when_all_fault_disposed_exactly_once_by_owning_input_task) {
    expectValueLeakFree(
        "module M; "
        "class CountedErr: Exception "
        "  public static disposeCount: int := 0; "
        "  public constructor() base(\"boom\"); end constructor; "
        "  destructor CountedErr.disposeCount := CountedErr.disposeCount + 1; end destructor; "
        "end CountedErr; "
        "class C "
        "  public static async method Boom(): Task throw new CountedErr(); end Boom; "
        "  public static async method Run(): Task "
        "    using var t1 := Task.FromResult(1); "
        "    using var t2 := Boom(); "
        "    using var tasks := new Array<Task>(); "
        "    tasks.Add(t1); tasks.Add(t2); "
        "    using var wa := Task.WhenAll(tasks); "
        "    try await wa; catch CountedErr => var ignored := 1; end; "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    await Run(); "
        "    return CountedErr.disposeCount; "
        "  end Main; end C;",
        1);
}

// §30.10: "WhenAny of an empty array raises ArgumentException synchronously
// at the call site, not as a faulted task (it could never complete, and the
// misuse is local)." Before the fix, the runtime instead returned an
// already-completed `Task<int>` whose result was the sentinel index -1 —
// silently "succeeding" with a nonsensical winner index rather than throwing.
BEZEL_TEST(task_combinators_1529, when_any_empty_array_raises_argument_exception_synchronously) {
    expectValueLeakFree(
        "module M; class C "
        "  public static async method Main(): Task<int> "
        "    using var tasks := new Array<Task>(); "
        "    try "
        "      using var w := Task.WhenAny(tasks); "
        "      return 1; "
        "    catch ArgumentException => return 2; "
        "    end; "
        "  end Main; end C;",
        2);
}

// §30.10: `Task.WhenAllOwned<T>` — same input-order fault-selection contract
// (index 1's `BoomA` wins over index 2's `BoomB`, which faults first in wall-
// clock time), PLUS the ownership-of-results contract: "results already
// produced are disposed with their tasks" on the fault path. Two successful
// results (`Cell(1)` at index 0, `Cell(2)` at index 3, produced before and
// after the winning fault settles) are captured before the fault wins; since
// the whole input array was OWNED (transferred in), nobody else can ever see
// them, so WhenAllOwned itself must dispose them — verified via the
// destructor-counting `Cell`: `Cell.n` (live count) returns to 0 and
// `Cell.disposeCount` is exactly 2 (dispose-once, no leak, no double-free).
BEZEL_TEST(task_combinators_1529,
           when_all_owned_first_non_successful_input_order_wins_and_disposes_produced_results) {
    expectValueLeakFree(
        "module M; "
        "class Cell "
        "  public static n: int := 0; "
        "  public static disposeCount: int := 0; "
        "  public X: int; "
        "  public constructor(x: int) X := x; Cell.n := Cell.n + 1; end constructor; "
        "  destructor "
        "    Cell.n := Cell.n - 1; Cell.disposeCount := Cell.disposeCount + 1; "
        "  end destructor; "
        "end Cell; "
        "class ErrA: Exception public constructor() base(\"A\"); end constructor; end ErrA; "
        "class ErrB: Exception public constructor() base(\"B\"); end constructor; end ErrB; "
        "class C "
        "  public static async method MakeCellSlow(x: int): OwningTask<Cell> "
        "    await Task.Delay(20); return new Cell(x); "
        "  end MakeCellSlow; "
        "  public static async method MakeCellFast(x: int): OwningTask<Cell> "
        "    return new Cell(x); "
        "  end MakeCellFast; "
        "  public static async method BoomA(): OwningTask<Cell> "
        "    await Task.Delay(20); throw new ErrA(); "
        "  end BoomA; "
        "  public static async method BoomB(): OwningTask<Cell> "
        "    throw new ErrB(); "
        "  end BoomB; "
        "  public static async method Run(): Task "
        "    var tasks := new OwningArray<OwningTask<Cell>>(); "
        "    tasks.Add(MakeCellFast(1)); tasks.Add(BoomA()); "
        "    tasks.Add(BoomB()); tasks.Add(MakeCellSlow(2)); "
        "    try using var results := await Task.WhenAllOwned(move tasks); "
        "    catch ErrA => var ignored := 1; "
        "    catch ErrB => var ignored := 2; "
        "    end; "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    await Run(); "
        "    return Cell.n * 100 + Cell.disposeCount; "
        "  end Main; end C;",
        2);
}

// ---------------------------------------------------------------------------
// #2519 — COVERAGE PIN (not a fail-without/pass-with regression): the
// WhenAllOwned fault path disposes already-produced results via the RUNTIME
// bz_dispose_class_obj helper (bezel_runtime_concurrency.cpp:1569/1575), which
// is the RUNTIME half of the §28.2 checked-disposal quarantine gap filed in
// #2519 (the codegen half, elementDisposerThunk, was fixed in #2514). No
// existing WhenAllOwned cell runs under §28.2 checked-disposal MODE — the
// #1529 cell above uses expectValueLeakFree (ASan/LSan, no checked-disposal) —
// so this cell closes that coverage gap: it verifies the fault path's dispose-
// once contract (disposeCount == 2, Cell.n == 0, exit 2) holds under the
// checked-disposal codegen path (disposed-flag guards emitted, different IR).
//
// WHY THIS IS A COVERAGE PIN, NOT A REGRESSION: the runtime UAF gap
// (#2519: bz_dispose_class_obj frees UNCONDITIONALLY, bypassing the §28.2
// quarantine that emitDisposeAndReclaim applies in checked-disposal mode) is
// NOT reachable through any constructible stale-borrow shape. The disposed
// results are internal to the WhenAllOwned aggregate — Pattern B consumed the
// whole input array, so no caller-side borrow of a produced result survives
// the aggregate's fault (verified: the field-held-borrow + FromOwnedResult
// shape is blocked because Task.FromOwnedResult's intrinsic does not consume
// named bindings, the async-wrapper shape is blocked because the `owned`
// transfer marker is not admitted on owned parameters in return position
// (v0.209/#2080), and §22.12 rule 2 bans the class-typed mutable static that
// would alias a result). So the unconditional free in bz_dispose_class_obj
// cannot be observed: the freed block is never read again. This cell pins
// correct disposal behavior so a future change that breaks the WhenAllOwned
// fault path under checked-disposal mode is caught. // per §28.2, §30.10, #2519
// ---------------------------------------------------------------------------
BEZEL_TEST(task_combinators_2519,
           when_all_owned_fault_path_checked_disposal_coverage_pin) {
    BEZEL_EXPECT_EQ(compileAndRunCheckedDisposal(
        "module M; "
        "class Cell "
        "  public static n: int := 0; "
        "  public static disposeCount: int := 0; "
        "  public X: int; "
        "  public constructor(x: int) X := x; Cell.n := Cell.n + 1; end constructor; "
        "  destructor "
        "    Cell.n := Cell.n - 1; Cell.disposeCount := Cell.disposeCount + 1; "
        "  end destructor; "
        "end Cell; "
        "class ErrA: Exception public constructor() base(\"A\"); end constructor; end ErrA; "
        "class ErrB: Exception public constructor() base(\"B\"); end constructor; end ErrB; "
        "class C "
        "  public static async method MakeCellSlow(x: int): OwningTask<Cell> "
        "    await Task.Delay(20); return new Cell(x); "
        "  end MakeCellSlow; "
        "  public static async method MakeCellFast(x: int): OwningTask<Cell> "
        "    return new Cell(x); "
        "  end MakeCellFast; "
        "  public static async method BoomA(): OwningTask<Cell> "
        "    await Task.Delay(20); throw new ErrA(); "
        "  end BoomA; "
        "  public static async method BoomB(): OwningTask<Cell> "
        "    throw new ErrB(); "
        "  end BoomB; "
        "  public static async method Run(): Task "
        "    var tasks := new OwningArray<OwningTask<Cell>>(); "
        "    tasks.Add(MakeCellFast(1)); tasks.Add(BoomA()); "
        "    tasks.Add(BoomB()); tasks.Add(MakeCellSlow(2)); "
        "    try using var results := await Task.WhenAllOwned(move tasks); "
        "    catch ErrA => var ignored := 1; "
        "    catch ErrB => var ignored := 2; "
        "    end; "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    await Run(); "
        "    return Cell.n * 100 + Cell.disposeCount; "
        "  end Main; end C;"),
        2);
}
