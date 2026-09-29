// Codegen — async/await state-machine (§30, Phase 2b). An `async` method's body
// is lowered into a suspend/resume state machine (the kind-agnostic scaffolding
// of §29/§30 Phase 2a, parameterized for `await`): the original method becomes
// the ENTRY that allocates a Task and a heap frame and drives a Resume
// continuation; each `await` is a suspend point (§30.2) with an already-done
// fast path; `return e;` completes the Task (§30.1); a faulted awaited task
// propagates the fault up the await chain (§30.9 basic).
//
// These tests cannot use the plain JIT harness (the iterator/async emitter
// accumulates LLVM module state across runs — the same reason iterator_test.cpp
// is excluded from the JIT suite). They drive the REAL Driver pipeline to a
// native executable and check its exit code, mirroring native_exe_test.cpp /
// box_refcount_test.cpp. A second leg links against an ASan-built runtime and
// runs under LeakSanitizer to verify the frame/task reclaim is leak- and
// corruption-free on the synchronous completion path (skipped when no toolchain).
//
// SCOPE (what is exercised here): the synchronous / already-done cut —
// `await SomeAsyncCall()` where the producer completes inline. The supported
// shapes are straight-line sequential awaits, awaits in if/else branches, awaits
// embedded in larger expressions, locals live across an await, `Task` (no value)
// awaits, basic fault propagation, AND — added by #438 — `await` inside loop
// bodies (while / range-for / nested), including loop-carried accumulation and
// pre-/post-loop locals live across the loop's suspends. The #438 remainder adds
// `await` inside a try BODY, a CATCH ARM body, and a FINALLY (the handler-context
// allocas — caught-exception binding/disposal, finally exit-mode/return-value,
// and the in-flight exception on the finally-unwind path — are frame-backed
// across a cross-thread suspend). DEFERRED (not exercised; rejected with a
// diagnostic or out of scope): `await` inside a catch GUARD (`catch … when
// await …`, the guard runs during dispatch before the handler scope exists —
// rejected with a clean diagnostic), atomic refcounts, and async iterators
// (which reject suspend-in-try wholesale, a separate boundary).

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

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

#include "native_test_harness.hpp"
#include "support/force_sanitize.hpp"  // #4569: forceSanitizeAddress

namespace {

namespace fs = std::filesystem;

// Per-process unique working dir, under the shared scratch base (never
// /tmp — see support/scratch_dir.hpp) and reaped at process exit.
static const std::filesystem::path& wtDirAsync() {
    return wtScratchDir("wt-async");
}

// Compile `source` to a temp native executable via the real Driver and run it;
// returns the exit code, or -1 on compile/link/run failure.
int compileAndRun(const std::string& source) {
    auto dir = wtDirAsync();
    std::string stem = unique_stem("bezel_async");
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

// True when the source FAILS to compile (a deferred-feature diagnostic, e.g.
// await-in-loop). Used to pin the boundary of the increment.
bool compileFails(const std::string& source) {
    auto dir = wtDirAsync();
    std::string stem = unique_stem("bezel_async_neg");
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

// Sanitizer verification for a single program under the given -fsanitize=<san>
// (e.g. "address" for ASan/LSan, "thread" for TSan/#439). Returns:
//   0  — ran clean: no leak / UAF / double-free / data race reported.
//   1  — sanitizer reported a problem: test failure.
//  -1  — toolchain unavailable / build step failed: SKIP (not a failure).
// The runtime is rebuilt under the same sanitizer and linked with the program's
// object so the sanitizer instruments both the generated code and the runtime
// (where the thread pool, refcounts, and task state live).
int runUnderSanitizer(const std::string& source, const std::string& san,
                      const char* runOpts) {
    if (std::system("clang++ --version > /dev/null 2>&1") != 0) return -1;

    auto dir = wtDirAsync();
    std::string stem = "bezel_async_san_" + san + "_" + unique_stem("");
    std::string objPath = (dir / (stem + ".o")).string();
    std::string rtObj   = (dir / (stem + "_rt.o")).string();
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
        // #4569: a PER-CALL decision, not the rename the other 39 files in
        // the #3917 burn-down took. This runner is parameterised on `san` and
        // builds its link line as `-fsanitize=` + san, so OR-ing the sweep in
        // unconditionally (what `emitObjectFileForTest` does) would hand an
        // ASan-instrumented object to a `-fsanitize=thread` link -- wrong,
        // not merely useless. Thread the sweep only on the "address" path.
        // The explicit fourth argument is also what exempts this call from
        // check_uninstrumented_emit_runners' by-arity bypassing_calls count.
        const bool sweepThisObject =
            san == "address" && bezel::test::forceSanitizeAddress();
        if (!bezel::codegen::emitObjectFile(*module, objPath, err,
                                            sweepThisObject))
            return bezel::test::asanBuildFailure("emitObjectFile");
    }

    auto cleanup = [&] {
        std::remove(objPath.c_str());
        std::remove(rtObj.c_str());
        std::remove(exe.c_str());
    };
    if (!::bezel::native_test::buildRuntimeSanitizerObject(san, rtObj)) {
        cleanup();
        return -1;
    }

    std::string linkCmd = "clang++ -fsanitize=" + san + " -g " + objPath + " " +
                          rtObj + " -lm -lpthread -lstdc++ -o " + exe +
                          " > /dev/null 2>&1";
    if (std::system(linkCmd.c_str()) != 0) {
        cleanup();
        return bezel::test::asanLinkFailure("link");
    }

    std::string runCmd =
        std::string(runOpts) + " " + exe + " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return bezel::test::asanExecFailure("system");
    if (WIFEXITED(status) && WEXITSTATUS(status) == 99) return 1;  // sanitizer
    return 0;                                                      // clean
}

// LeakSanitizer verification (ASan exitcode=99 on any leak/UAF/double-free).
int runUnderLeakSanitizer(const std::string& source) {
    return runUnderSanitizer(source, "address",
                             "ASAN_OPTIONS=detect_leaks=1:exitcode=99");
}

// ThreadSanitizer verification (#439): exit 99 on any reported data race.
int runUnderThreadSanitizer(const std::string& source) {
    return runUnderSanitizer(source, "thread", "TSAN_OPTIONS=exitcode=99");
}

// Assert correctness (exit code) AND, when the toolchain is present, that LSan
// reports no leak / UAF / double-free on the synchronous completion path.
void expectValueLeakFree(const std::string& source, int expectedExitCode) {
    BEZEL_EXPECT_EQ(compileAndRun(source), expectedExitCode);
    int lsan = runUnderLeakSanitizer(source);
    if (lsan == -1) return;       // toolchain unavailable: skip the LSan leg
    BEZEL_EXPECT_EQ(lsan, 0);     // 0 == LSan clean
}

// #439 cross-thread assertion: a program whose await suspends on a genuinely-
// incomplete task (Task.Delay) and resumes on a thread-pool worker. Run the
// program 3x to catch nondeterministic cross-thread bugs (the result must be
// stable), then — when the toolchain is present — verify it is ASan-clean (no
// leak / UAF / double-free across the thread boundary) AND TSan-clean (no data
// race in the scheduler / task state / atomic refcounts). The 3x repetition + the
// two sanitizers together are the bar for the cross-thread path (§30.13 / #439).
void expectValueCrossThread(const std::string& source, int expectedExitCode) {
    for (int i = 0; i < 3; ++i)
        BEZEL_EXPECT_EQ(compileAndRun(source), expectedExitCode);
    int lsan = runUnderLeakSanitizer(source);
    if (lsan != -1) BEZEL_EXPECT_EQ(lsan, 0);   // 0 == ASan/LSan clean
    int tsan = runUnderThreadSanitizer(source);
    if (tsan != -1) BEZEL_EXPECT_EQ(tsan, 0);   // 0 == TSan clean (no race)
}

}  // namespace

// §30.1 / §30.2: `return await SomeCompletedTask;`. Producer is an async method
// whose body completes synchronously (no await), so its Task is already done at
// the await — the already-done fast path returns the result inline. The awaiter
// adds 1: 42 + 1 = 43.
BEZEL_TEST(async_30_2, return_await_completed_task) {
    expectValueLeakFree(
        "module M; class C "
        "  public static async method Producer(): Task<int> "
        "    return 42; "
        "  end Producer; "
        "  public static async method Main(): Task<int> "
        "    var x := await Producer(); "
        "    return x + 1; "
        "  end Main; end C;",
        43);
}

// §30.2: a local live ACROSS an await must be preserved by the frame spill/
// restore. `seed` (5) is declared before the first await and read after the
// second; two awaits each pull from a producer. 5 + 20 + 30 = 55.
BEZEL_TEST(async_30_2, local_live_across_await) {
    expectValueLeakFree(
        "module M; class C "
        "  public static async method Producer(n: int): Task<int> "
        "    return n * 10; "
        "  end Producer; "
        "  public static async method Main(): Task<int> "
        "    var seed := 5; "
        "    var a := await Producer(2); "
        "    var b := await Producer(3); "
        "    return seed + a + b; "
        "  end Main; end C;",
        55);
}

// §30.1: `async method G(): Task` — no result value. The method awaits a
// no-value task and completes without a result; the awaiter observes unit and
// proceeds. The surrounding Task<int> Main returns 9 to prove control flowed
// through the no-value await and back.
BEZEL_TEST(async_30_1, await_task_no_value) {
    expectValueLeakFree(
        "module M; class C "
        "  public static async method DoWork(): Task "
        "    return; "
        "  end DoWork; "
        "  public static async method G(): Task "
        "    await DoWork(); "
        "  end G; "
        "  public static async method Main(): Task<int> "
        "    await G(); "
        "    return 9; "
        "  end Main; end C;",
        9);
}

// §30.2: `await` inside an if/else branch (non-loop control flow). The flat
// resume model handles branch-local awaits; `flag` picks Producer(2) → 200,
// plus 1 = 201.
BEZEL_TEST(async_30_2, await_in_if_branch) {
    expectValueLeakFree(
        "module M; class C "
        "  public static async method Producer(n: int): Task<int> "
        "    return n * 100; "
        "  end Producer; "
        "  public static async method Pick(flag: bool): Task<int> "
        "    var r := 0; "
        "    if flag then r := await Producer(2); "
        "    else r := await Producer(3); end; "
        "    return r + 1; "
        "  end Pick; "
        "  public static async method Main(): Task<int> "
        "    return await Pick(true); "
        "  end Main; end C;",
        201);
}

// §30.9 (basic): a synchronous `throw` inside an async body faults that
// method's Task; awaiting the faulted Task propagates the fault up the await
// chain. With no enclosing handler, Main's Task faults and the process exits
// non-zero via the unhandled-exception boundary (exit code 70, §33.3). Awaiting
// inside a try/catch (to CATCH the fault) is deferred, so this verifies the
// PROPAGATION-to-process-boundary path, not handler resumption.
BEZEL_TEST(async_30_9, awaiting_faulted_task_propagates) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C "
        "  public static async method Boom(): Task<int> "
        "    throw new InvalidOperationException(); "
        "  end Boom; "
        "  public static async method Main(): Task<int> "
        "    return await Boom(); "
        "  end Main; end C;"),
        70);
}

// §30.2 (#438): `await` inside a `while` loop body, accumulating into a
// loop-carried local. The await is embedded in a larger expression
// (`total + await ...`), so the left operand `total` is loaded BEFORE the
// suspend and the `+` runs AFTER the resume — the case that broke SSA dominance
// in the flat resume model. fixupSuspendDominance demotes the offending
// temporary to a stack slot so the resume verifies; the loop-carried `total`
// round-trips through the frame. 1 + 2 + 3 = 6.
BEZEL_TEST(async_30_2, await_in_while_loop_accumulates) {
    expectValueLeakFree(
        "module M; class C "
        "  public static async method Producer(n: int): Task<int> "
        "    return n; "
        "  end Producer; "
        "  public static async method Main(): Task<int> "
        "    var total := 0; var i := 1; "
        "    while i <= 3 do "
        "      total := total + await Producer(i); "
        "      i := i + 1; "
        "    end; "
        "    return total; "
        "  end Main; end C;",
        6);
}

// §30.2 / §16.4 (#438): `await` inside a `for var i in lo..hi` range loop. The
// loop variable `i` is a frame-backed suspend-local (the iterator path's
// machinery, reused here); each iteration awaits a producer and accumulates.
// Producer doubles its arg: 2 + 4 + 6 = 12.
BEZEL_TEST(async_30_2, await_in_range_for_loop) {
    expectValueLeakFree(
        "module M; class C "
        "  public static async method Producer(n: int): Task<int> "
        "    return n * 2; "
        "  end Producer; "
        "  public static async method Main(): Task<int> "
        "    var total := 0; "
        "    for var i in 1..3 do "
        "      total := total + await Producer(i); "
        "    end; "
        "    return total; "
        "  end Main; end C;",
        12);
}

// §30.2 (#438): a loop-carried local (`total`) AND a pre-loop local (`seed`)
// BOTH live across the in-loop await. `seed` is set before the loop and read on
// every iteration; `total` accumulates. Both must survive the suspend (frame
// round-trip) and the back-edge. seed=10: (10+1)+(10+2)+(10+3) = 36.
BEZEL_TEST(async_30_2, preloop_and_loop_carried_locals_across_await) {
    expectValueLeakFree(
        "module M; class C "
        "  public static async method Producer(n: int): Task<int> "
        "    return n; "
        "  end Producer; "
        "  public static async method Main(): Task<int> "
        "    var seed := 10; "
        "    var total := 0; var i := 1; "
        "    while i <= 3 do "
        "      total := total + seed + await Producer(i); "
        "      i := i + 1; "
        "    end; "
        "    return total; "
        "  end Main; end C;",
        36);
}

// §30.2 (#438): a value computed BEFORE a loop (that contains an await) and used
// AFTER the loop. `pre` (50) is set before the loop, never touched inside it, and
// added to the accumulated total after the loop completes. Verifies the pre-loop
// value survives the loop's suspend/resume cycles. (1+2+3) + 50 = 56.
BEZEL_TEST(async_30_2, value_before_loop_used_after_loop_with_await) {
    expectValueLeakFree(
        "module M; class C "
        "  public static async method Producer(n: int): Task<int> "
        "    return n; "
        "  end Producer; "
        "  public static async method Main(): Task<int> "
        "    var pre := 50; "
        "    var total := 0; var i := 1; "
        "    while i <= 3 do "
        "      total := total + await Producer(i); "
        "      i := i + 1; "
        "    end; "
        "    return total + pre; "
        "  end Main; end C;",
        56);
}

// §30.2 (#438): nested loops, with the await in the inner loop body. Both loop
// variables (`i`, `j`) and the accumulator round-trip across the suspend.
// total += i*j for i,j in {1,2}x{1,2}: 1 + 2 + 2 + 4 = 9.
BEZEL_TEST(async_30_2, await_in_nested_loops) {
    expectValueLeakFree(
        "module M; class C "
        "  public static async method Producer(n: int): Task<int> "
        "    return n; "
        "  end Producer; "
        "  public static async method Main(): Task<int> "
        "    var total := 0; var i := 1; "
        "    while i <= 2 do "
        "      var j := 1; "
        "      while j <= 2 do "
        "        total := total + await Producer(i * j); "
        "        j := j + 1; "
        "      end; "
        "      i := i + 1; "
        "    end; "
        "    return total; "
        "  end Main; end C;",
        9);
}

// §30.2 (#438): an `await` inside an if-branch that is itself inside a loop. Only
// iterations where the branch is taken await; the value still threads through the
// resume correctly. i in 1..4, awaits only when i > 2: 3 + 4 = 7.
BEZEL_TEST(async_30_2, await_in_branch_inside_loop) {
    expectValueLeakFree(
        "module M; class C "
        "  public static async method Producer(n: int): Task<int> "
        "    return n; "
        "  end Producer; "
        "  public static async method Main(): Task<int> "
        "    var total := 0; var i := 1; "
        "    while i <= 4 do "
        "      if i > 2 then total := total + await Producer(i); end; "
        "      i := i + 1; "
        "    end; "
        "    return total; "
        "  end Main; end C;",
        7);
}

// §30.2 (#438): two awaits in a single loop-body expression. Both suspend points
// resume correctly within the iteration; the loop-carried accumulator is right.
// total += i + i each iteration: 2*(1+2+3) = 12.
BEZEL_TEST(async_30_2, two_awaits_per_loop_iteration) {
    expectValueLeakFree(
        "module M; class C "
        "  public static async method Producer(n: int): Task<int> "
        "    return n; "
        "  end Producer; "
        "  public static async method Main(): Task<int> "
        "    var total := 0; var i := 1; "
        "    while i <= 3 do "
        "      total := total + await Producer(i) + await Producer(i); "
        "      i := i + 1; "
        "    end; "
        "    return total; "
        "  end Main; end C;",
        12);
}

// §30.2 (#438) regression: an `await` embedded in a larger expression OUTSIDE any
// loop (`total + await f()`). This straight-line case has the SAME pre-suspend
// temporary as the loop case (the left operand loaded before the await), so it
// is fixed by the same dominance repair. 10 + 5 = 15.
BEZEL_TEST(async_30_2, await_embedded_in_expression_straight_line) {
    expectValueLeakFree(
        "module M; class C "
        "  public static async method Producer(n: int): Task<int> "
        "    return n; "
        "  end Producer; "
        "  public static async method Main(): Task<int> "
        "    var total := 10; "
        "    total := total + await Producer(5); "
        "    return total; "
        "  end Main; end C;",
        15);
}

// §30.9 / §31.2 (#438): `await` inside a try BODY with a normal (non-faulting)
// awaited task. The try completes, the catch is NOT entered, and the awaited
// value is correct. The resume block sits inside the try's protected region so
// the post-await code keeps the catch dispatch as its unwind target. 5 + 1 = 6.
BEZEL_TEST(async_30_9, await_in_try_normal_completion) {
    expectValueLeakFree(
        "module M; class C "
        "  public static async method Producer(n: int): Task<int> "
        "    return n; "
        "  end Producer; "
        "  public static async method Main(): Task<int> "
        "    var x := 0; "
        "    try x := await Producer(5); catch Exception => x := 100; end; "
        "    return x + 1; "
        "  end Main; end C;",
        6);
}

// §30.9 / §31.2 (#438): awaiting a FAULTED task inside a try BODY is CAUGHT by
// the enclosing catch — not propagated to the process boundary. `Boom` faults
// its task with a synchronous throw; the await re-raises (§30.9) into the catch,
// which sets x := 7. Without the fix the fault would propagate (exit 70).
BEZEL_TEST(async_30_9, await_faulted_task_caught_by_catch) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C "
        "  public static async method Boom(): Task<int> "
        "    throw new InvalidOperationException(); "
        "  end Boom; "
        "  public static async method Main(): Task<int> "
        "    var x := 0; "
        "    try x := await Boom(); catch Exception => x := 7; end; "
        "    return x; "
        "  end Main; end C;"),
        7);
}

// §30.9 / §31.4 (#438): a faulted await is matched by a TYPE-SPECIFIC catch arm
// (InvalidOperationException), confirming the original exception object — not a
// generic message — is carried through the task so `is`-based arm matching works.
BEZEL_TEST(async_30_9, await_faulted_task_caught_by_type) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C "
        "  public static async method Boom(): Task<int> "
        "    throw new InvalidOperationException(); "
        "  end Boom; "
        "  public static async method Main(): Task<int> "
        "    var x := 0; "
        "    try x := await Boom(); "
        "    catch ArgumentException => x := 1; "
        "    catch InvalidOperationException => x := 9; end; "
        "    return x; "
        "  end Main; end C;"),
        9);
}

// §30.9 / §31.2 (#438): a faulted await whose type does NOT match any arm
// re-propagates past the try to the process boundary (exit 70). The mismatched
// arm leaves the exception in flight; the function fault boundary completes the
// outer task with the error.
BEZEL_TEST(async_30_9, await_faulted_task_unmatched_arm_propagates) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C "
        "  public static async method Boom(): Task<int> "
        "    throw new InvalidOperationException(); "
        "  end Boom; "
        "  public static async method Main(): Task<int> "
        "    var x := 0; "
        "    try x := await Boom(); catch ArgumentException => x := 1; end; "
        "    return x; "
        "  end Main; end C;"),
        70);
}

// §30.9 / §31.2 (#438): `await` inside a try BODY with a `finally`. The awaited
// value is correct AND the finally runs on the normal completion path. The
// finally adds 100 to the result via a side effect: 5 then +100 = 105.
BEZEL_TEST(async_30_9, await_in_try_finally_runs_on_normal) {
    expectValueLeakFree(
        "module M; class C "
        "  public static async method Producer(n: int): Task<int> "
        "    return n; "
        "  end Producer; "
        "  public static async method Main(): Task<int> "
        "    var x := 0; var ran := 0; "
        "    try x := await Producer(5); finally ran := 100; end; "
        "    return x + ran; "
        "  end Main; end C;",
        105);
}

// §30.9 (#721 C1): a `return` exiting a try-with-finally in an ASYNC method must
// run the finally on the way out. Before the fix the async-return path completed
// the task and returned from Resume without consulting the finally, so the
// finally was silently skipped. Valued return (Task<int>): 7 + 1 = 8.
BEZEL_TEST(async_30_9, async_return_in_try_runs_finally_721) {
    expectValueLeakFree(
        "module M; class C "
        "  public static ran: int := 0; "
        "  public static async method DoWork(): Task<int> "
        "    try return 7; finally C.ran := 1; end; "
        "  end DoWork; "
        "  public static async method Main(): Task<int> "
        "    using var t := DoWork(); var r := await t; "
        "    return r + C.ran; "
        "  end Main; end C;",
        8);
}

// §30.9 (#721 C1): valueless `return` in a `Task` (no result) method — the
// finally still runs (no value to carry across). ran = 1.
BEZEL_TEST(async_30_9, async_valueless_return_in_try_runs_finally_721) {
    expectValueLeakFree(
        "module M; class C "
        "  public static ran: int := 0; "
        "  public static async method DoWork(): Task "
        "    try return; finally C.ran := 1; end; "
        "  end DoWork; "
        "  public static async method Main(): Task<int> "
        "    using var t := DoWork(); await t; "
        "    return C.ran; "
        "  end Main; end C;",
        1);
}

// §30.9 (#721 C1): a `return` in an inner try runs BOTH the inner and outer
// finally (exit mode 5 propagates through the outer finally, value preserved).
BEZEL_TEST(async_30_9, async_return_runs_nested_finally_721) {
    expectValueLeakFree(
        "module M; class C "
        "  public static a: int := 0; "
        "  public static b: int := 0; "
        "  public static async method DoWork(): Task<int> "
        "    try try return 5; finally C.a := 1; end; finally C.b := 1; end; "
        "  end DoWork; "
        "  public static async method Main(): Task<int> "
        "    using var t := DoWork(); var r := await t; "
        "    return r + C.a * 10 + C.b * 100; "
        "  end Main; end C;",
        115);
}

// §30.9 / §31.2 (#438): try/catch/finally together with an await in the body —
// normal completion runs the body and the finally (not the catch). 3 + 50 = 53.
BEZEL_TEST(async_30_9, await_in_try_catch_finally_normal) {
    expectValueLeakFree(
        "module M; class C "
        "  public static async method Producer(n: int): Task<int> "
        "    return n; "
        "  end Producer; "
        "  public static async method Main(): Task<int> "
        "    var x := 0; var ran := 0; "
        "    try x := await Producer(3); "
        "    catch Exception => x := 999; "
        "    finally ran := 50; end; "
        "    return x + ran; "
        "  end Main; end C;",
        53);
}

// §30.9 / §31.2 (#438): a faulted await caught by the catch, with a finally that
// also runs. The catch sets x := 8, the finally adds 30: 8 + 30 = 38.
BEZEL_TEST(async_30_9, await_faulted_caught_finally_runs) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C "
        "  public static async method Boom(): Task<int> "
        "    throw new InvalidOperationException(); "
        "  end Boom; "
        "  public static async method Main(): Task<int> "
        "    var x := 0; var ran := 0; "
        "    try x := await Boom(); "
        "    catch Exception => x := 8; "
        "    finally ran := 30; end; "
        "    return x + ran; "
        "  end Main; end C;"),
        38);
}

// §30.9 / §31.2 (#438): a try BODY with an await NESTED inside another try BODY.
// The inner await faults; the inner catch handles it. Confirms nested protected
// regions across a suspend. inner sets x := 4, outer adds 20: 24.
BEZEL_TEST(async_30_9, await_in_nested_try) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C "
        "  public static async method Boom(): Task<int> "
        "    throw new InvalidOperationException(); "
        "  end Boom; "
        "  public static async method Main(): Task<int> "
        "    var x := 0; "
        "    try "
        "      try x := await Boom(); catch Exception => x := 4; end; "
        "      x := x + 20; "
        "    catch Exception => x := 999; end; "
        "    return x; "
        "  end Main; end C;"),
        24);
}

// ===========================================================================
// §30.9 / §31.2 (#438 remainder): `await` inside a CATCH ARM and a FINALLY.
//
// The resume block of an in-handler await is lowered IN PLACE inside the
// handler scope, so it inherits the correct compile-time unwind context
// (currentCaughtExc_, the caught-exception disposal scope, the finally
// exit-mode dispatch). The runtime values that must survive the suspend — the
// caught-exception object pointer (binding + disposal slot), the finally
// exit-mode / return-value slots, and the in-flight exception on the
// finally-unwind path (which is captured out of the process-global bz$exc into
// a frame slot) — are FRAME-BACKED across a cross-thread resume (lowerTry
// registers them; fixupSuspendDominance heap-spills them, like a named local).
// Only an `await` in a catch GUARD remains deferred (it runs during dispatch,
// before the handler scope exists).
// ===========================================================================

// §30.9 / §31.2 (#438): `await` inside a CATCH ARM. A faulted body is caught,
// then the catch arm awaits a recovery producer and uses its result. The arm
// recovers x := 5. Synchronous (already-done) producer path.
BEZEL_TEST(async_30_9, await_in_catch_arm_recovers) {
    expectValueLeakFree(
        "module M; class C "
        "  public static async method Producer(n: int): Task<int> "
        "    return n; "
        "  end Producer; "
        "  public static async method Boom(): Task<int> "
        "    throw new InvalidOperationException(); "
        "  end Boom; "
        "  public static async method Main(): Task<int> "
        "    var x := 0; "
        "    try x := await Boom(); "
        "    catch Exception => do x := await Producer(5); end; end; "
        "    return x; "
        "  end Main; end C;",
        5);
}

// §30.9 / §31.1 / §31.2 (#438): the CAUGHT EXCEPTION is used AFTER an await in
// the catch arm. The binding `e` (a borrow whose lifetime is the arm body,
// which spans the suspend) must survive the suspend — its object pointer is
// frame-backed. Boom faults with message "boom"; after `await Producer(3)` the
// arm reads e.Message and adds 10 to the recovered value: 3 + 10 = 13.
BEZEL_TEST(async_30_9, await_in_catch_arm_uses_caught_exception_after_await) {
    expectValueLeakFree(
        "module M; class C "
        "  public static async method Producer(n: int): Task<int> "
        "    return n; "
        "  end Producer; "
        "  public static async method Boom(): Task<int> "
        "    throw new InvalidOperationException(\"boom\"); "
        "  end Boom; "
        "  public static async method Main(): Task<int> "
        "    var x := 0; "
        "    try x := await Boom(); "
        "    catch var e: Exception => do "
        "      var r := await Producer(3); "
        "      if e.Message = \"boom\" then x := r + 10; else x := r; end; "
        "    end; end; "
        "    return x; "
        "  end Main; end C;",
        13);
}

// §30.9 / §31.2 (#438): `await` inside a FINALLY that runs after a NORMAL-
// completing body. The body awaits (x := 5); the finally awaits a producer and
// adds 100: 5 + 100 = 105.
BEZEL_TEST(async_30_9, await_in_finally_runs_after_normal_body) {
    expectValueLeakFree(
        "module M; class C "
        "  public static async method Producer(n: int): Task<int> "
        "    return n; "
        "  end Producer; "
        "  public static async method Main(): Task<int> "
        "    var x := 0; var ran := 0; "
        "    try x := await Producer(5); "
        "    finally ran := await Producer(100); end; "
        "    return x + ran; "
        "  end Main; end C;",
        105);
}

// §30.9 / §31.2 (#438): `await` inside a FINALLY that runs after a FAULTED body,
// then the original exception RE-PROPAGATES past the finally and is caught by an
// enclosing try. The finally awaits Producer(7) on the exception path; the
// in-flight exception (captured out of the process-global into a frame slot
// across the suspend) is restored and re-raised, caught by the outer arm which
// adds 1: 7 + 1 = 8. Confirms the finally runs on the faulted path AND the
// exception survives the in-finally suspend.
BEZEL_TEST(async_30_9, await_in_finally_runs_after_faulted_body_reraises) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C "
        "  public static async method Producer(n: int): Task<int> "
        "    return n; "
        "  end Producer; "
        "  public static async method Boom(): Task<int> "
        "    throw new InvalidOperationException(); "
        "  end Boom; "
        "  public static async method Main(): Task<int> "
        "    var ran := 0; "
        "    try "
        "      try ran := await Boom(); "
        "      finally ran := ran + await Producer(7); end; "
        "    catch Exception => ran := ran + 1; end; "
        "    return ran; "
        "  end Main; end C;"),
        8);
}

// §30.9 / §31.2 (#438): a faulted await CAUGHT by a catch arm that itself awaits,
// with a finally that also awaits. Both handlers suspend. Boom faults; the catch
// arm awaits Producer(8) → x := 8; the finally awaits Producer(30) → ran := 30:
// 8 + 30 = 38. (A single-statement catch arm body — not a `do … end` block —
// because the parser requires a `;`-terminated arm body before `finally`.)
BEZEL_TEST(async_30_9, await_in_catch_and_finally_together) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C "
        "  public static async method Producer(n: int): Task<int> "
        "    return n; "
        "  end Producer; "
        "  public static async method Boom(): Task<int> "
        "    throw new InvalidOperationException(); "
        "  end Boom; "
        "  public static async method Main(): Task<int> "
        "    var x := 0; var ran := 0; "
        "    try x := await Boom(); "
        "    catch Exception => x := await Producer(8); "
        "    finally ran := await Producer(30); end; "
        "    return x + ran; "
        "  end Main; end C;"),
        38);
}

// §30.9 / §31.2 (#438): MULTIPLE awaits and named locals inside one catch arm.
// The recovery arm awaits twice and binds the results to named locals `a`/`b`,
// which (declared inside the catch arm) must be frame-backed across the suspends
// — collectSuspendLocals recurses into the handler region. a := 3, b := 4,
// x := 7.
BEZEL_TEST(async_30_9, multiple_awaits_and_locals_in_catch_arm) {
    expectValueLeakFree(
        "module M; class C "
        "  public static async method P(n: int): Task<int> "
        "    return n; "
        "  end P; "
        "  public static async method Boom(): Task<int> "
        "    throw new InvalidOperationException(); "
        "  end Boom; "
        "  public static async method Main(): Task<int> "
        "    var x := 0; "
        "    try x := await Boom(); "
        "    catch Exception => do "
        "      var a := await P(3); "
        "      var b := await P(4); "
        "      x := a + b; "
        "    end; end; "
        "    return x; "
        "  end Main; end C;",
        7);
}

// §30.9 / §31.1 / §31.2 (#438): a bare re-raise (`throw;`) AFTER an await in a
// catch arm. The caught-exception object (currentCaughtExc_) is re-stored into
// the in-flight slot after the suspend — it must survive the suspend (a register
// spilled to the heap frame by fixupSuspendDominance). The inner arm logs (await
// Log(1)) then re-raises; the outer arm catches and adds 40: 1 + 40 = 41.
BEZEL_TEST(async_30_9, reraise_after_await_in_catch_arm) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C "
        "  public static async method Log(n: int): Task<int> "
        "    return n; "
        "  end Log; "
        "  public static async method Boom(): Task<int> "
        "    throw new InvalidOperationException(); "
        "  end Boom; "
        "  public static async method Main(): Task<int> "
        "    var logged := 0; "
        "    try "
        "      try logged := await Boom(); "
        "      catch var e: Exception => do "
        "        logged := await Log(1); "
        "        throw; "
        "      end; end; "
        "    catch Exception => logged := logged + 40; end; "
        "    return logged; "
        "  end Main; end C;"),
        41);
}

// §30.9 / §31.2 (#438): `return` from a catch arm that awaits, with a finally
// that also awaits. The return value (await P(9) = 9) is routed through the
// finally (frame-backed exit-mode + return-value slots) which awaits before the
// actual return. 9.
BEZEL_TEST(async_30_9, return_from_await_in_catch_through_await_in_finally) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C "
        "  public static async method P(n: int): Task<int> "
        "    return n; "
        "  end P; "
        "  public static async method Boom(): Task<int> "
        "    throw new InvalidOperationException(); "
        "  end Boom; "
        "  public static async method Helper(): Task<int> "
        "    var ran := 0; "
        "    try "
        "      var x := await Boom(); "
        "      return x; "
        "    catch Exception => return await P(9); "
        "    finally ran := await P(0); end; "
        "  end Helper; "
        "  public static async method Main(): Task<int> "
        "    return await Helper(); "
        "  end Main; end C;"),
        9);
}

// §30.9 / §31.2 (#438): a FINALLY-only await (the try body does NOT await). The
// finally body's await still needs its own resume state. x := 5 (sync body),
// finally awaits P(100): 5 + 100 = 105.
BEZEL_TEST(async_30_9, await_only_in_finally_body_not_in_try_body) {
    expectValueLeakFree(
        "module M; class C "
        "  public static async method P(n: int): Task<int> "
        "    return n; "
        "  end P; "
        "  public static async method Main(): Task<int> "
        "    var x := 0; var ran := 0; "
        "    try x := 5; finally ran := await P(100); end; "
        "    return x + ran; "
        "  end Main; end C;",
        105);
}

// §30.9 / §31.2 (#438) cross-thread: MULTIPLE awaits + named locals in a catch
// arm on the REAL cross-thread suspend path. Both `a` and `b` round-trip the
// heap frame across two worker resumes. 3 + 4 = 7. Run 3x, ASan/TSan-clean.
BEZEL_TEST(async_30_13, multiple_awaits_in_catch_arm_cross_thread) {
    expectValueCrossThread(
        "module M; class C "
        "  public static async method P(n: int): Task<int> "
        "    await Task.Delay(5); "
        "    return n; "
        "  end P; "
        "  public static async method Boom(): Task<int> "
        "    throw new InvalidOperationException(); "
        "  end Boom; "
        "  public static async method Main(): Task<int> "
        "    var x := 0; "
        "    try x := await Boom(); "
        "    catch Exception => do "
        "      var a := await P(3); "
        "      var b := await P(4); "
        "      x := a + b; "
        "    end; end; "
        "    return x; "
        "  end Main; end C;",
        7);
}

// §30.9 / §31.2 (#438) cross-thread: a bare re-raise after an await in a catch
// arm, on the REAL cross-thread suspend path. The caught-exception object must
// survive the worker resume to be re-raised. Caught by the outer arm: 1 + 40 = 41.
BEZEL_TEST(async_30_13, reraise_after_await_in_catch_arm_cross_thread) {
    expectValueCrossThread(
        "module M; class C "
        "  public static async method Log(n: int): Task<int> "
        "    await Task.Delay(5); "
        "    return n; "
        "  end Log; "
        "  public static async method Boom(): Task<int> "
        "    await Task.Delay(5); "
        "    throw new InvalidOperationException(); "
        "  end Boom; "
        "  public static async method Main(): Task<int> "
        "    var logged := 0; "
        "    try "
        "      try logged := await Boom(); "
        "      catch var e: Exception => do "
        "        logged := await Log(1); "
        "        throw; "
        "      end; end; "
        "    catch Exception => logged := logged + 40; end; "
        "    return logged; "
        "  end Main; end C;",
        41);
}

// §30.9 / §31.2 (#438) cross-thread: `await` in a FINALLY that runs after a
// FAULTED body, then re-propagates the original exception, on the REAL cross-
// thread suspend path. The in-flight exception is captured into a frame slot
// across the in-finally suspend and re-raised; the outer arm catches it: the
// finally added 7, the outer arm adds 1 → 8. Run 3x, ASan/TSan-clean.
BEZEL_TEST(async_30_13, await_in_finally_faulted_reraise_cross_thread) {
    expectValueCrossThread(
        "module M; class C "
        "  public static async method P(n: int): Task<int> "
        "    await Task.Delay(5); "
        "    return n; "
        "  end P; "
        "  public static async method Boom(): Task<int> "
        "    await Task.Delay(5); "
        "    throw new InvalidOperationException(); "
        "  end Boom; "
        "  public static async method Main(): Task<int> "
        "    var ran := 0; "
        "    try "
        "      try ran := await Boom(); "
        "      finally ran := ran + await P(7); end; "
        "    catch Exception => ran := ran + 1; end; "
        "    return ran; "
        "  end Main; end C;",
        8);
}

// §30.9 / §31.2 (#438) cross-thread: an `await` inside a CATCH ARM on the REAL
// cross-thread suspend path (Task.Delay). The caught exception is used after the
// await; both the exception pointer and the catch result round-trip the heap
// frame across the worker resume. 5 + 30 = 35. Run 3x, ASan/TSan-clean.
BEZEL_TEST(async_30_13, await_in_catch_arm_cross_thread) {
    expectValueCrossThread(
        "module M; class C "
        "  public static async method Compute(n: int): Task<int> "
        "    await Task.Delay(5); "
        "    return n; "
        "  end Compute; "
        "  public static async method Boom(): Task<int> "
        "    await Task.Delay(5); "
        "    throw new InvalidOperationException(\"ct\"); "
        "  end Boom; "
        "  public static async method Main(): Task<int> "
        "    var x := 0; "
        "    try x := await Boom(); "
        "    catch var e: Exception => do "
        "      var r := await Compute(5); "
        "      if e.Message = \"ct\" then x := r + 30; else x := r; end; "
        "    end; end; "
        "    return x; "
        "  end Main; end C;",
        35);
}

// §30.9 / §31.2 (#438) cross-thread: an `await` inside a FINALLY on the REAL
// cross-thread suspend path. The body awaits (x := 5); the finally awaits across
// a worker resume and adds 100: 5 + 100 = 105. Run 3x, ASan/TSan-clean.
BEZEL_TEST(async_30_13, await_in_finally_cross_thread) {
    expectValueCrossThread(
        "module M; class C "
        "  public static async method Compute(n: int): Task<int> "
        "    await Task.Delay(5); "
        "    return n; "
        "  end Compute; "
        "  public static async method Main(): Task<int> "
        "    var x := 0; var ran := 0; "
        "    try x := await Compute(5); "
        "    finally ran := await Compute(100); end; "
        "    return x + ran; "
        "  end Main; end C;",
        105);
}

// §30.9 deferred tail (#438): `await` inside a catch GUARD is STILL deferred —
// the guard runs during catch dispatch, before the handler scope (and the
// caught-exception state) exist, so resuming there would need the dispatch
// sequence re-established. Rejected with a clean diagnostic, no malformed IR.
BEZEL_TEST(async_30_9, await_in_catch_guard_is_rejected) {
    BEZEL_EXPECT_TRUE(compileFails(
        "module M; class C "
        "  public static async method Check(): Task<bool> "
        "    return true; "
        "  end Check; "
        "  public static async method Boom(): Task<int> "
        "    throw new InvalidOperationException(); "
        "  end Boom; "
        "  public static async method Main(): Task<int> "
        "    var x := 0; "
        "    try x := await Boom(); "
        "    catch var e: Exception when await Check() => x := 1; end; "
        "    return x; "
        "  end Main; end C;"));
}

// ===========================================================================
// #439 — TRULY CONCURRENT async: the scheduler runs, continuations run on a
// thread pool, and `await Task.Delay(...)` is a GENUINELY-incomplete task whose
// resume fires on a worker thread (§30.13). These tests use Task.Delay — the
// real stdlib §6.7 / §30.10 primitive (Task.Run does NOT exist in the Bezel
// spec, so it is not used). Each verifies the cross-thread suspend/resume path:
// the await registers its continuation and returns to the caller; a pool worker
// completes the delay and re-enters the state machine; the outer task completes;
// the top-level (async Main / bz_task_await_sync) blocks until done; the
// scheduler shuts down so the process exits without hung threads. expectValue-
// CrossThread runs each 3x (determinism) and verifies ASan- and TSan-clean.
// ===========================================================================

// §30.13 (#439): the simplest cross-thread suspend. `await Task.Delay(ms)` is a
// no-value await on a task that is NOT done at the await point — the worker
// sleeps then completes it, firing the resume on the pool. Main returns 7 to
// prove control flowed through the suspend and back across the thread boundary.
BEZEL_TEST(async_30_13, await_task_delay_then_value) {
    expectValueCrossThread(
        "module M; class C "
        "  public static async method Main(): Task<int> "
        "    await Task.Delay(5); "
        "    return 7; "
        "  end Main; end C;",
        7);
}

// §30.13 (#439): a value computed AFTER a genuinely-async suspend. `Compute`
// delays then returns 42 (the value is produced on the worker that resumed it);
// Main awaits it and adds 1. The named result local `x` round-trips through the
// heap frame across the cross-thread suspend. 42 + 1 = 43.
BEZEL_TEST(async_30_13, await_delayed_compute_returns_value) {
    expectValueCrossThread(
        "module M; class C "
        "  public static async method Compute(): Task<int> "
        "    await Task.Delay(5); "
        "    return 42; "
        "  end Compute; "
        "  public static async method Main(): Task<int> "
        "    var x := await Compute(); "
        "    return x + 1; "
        "  end Main; end C;",
        43);
}

// §30.13 (#439): NESTED async on the pool — a task whose body itself awaits
// another genuinely-incomplete task. `Outer` awaits `Inner` (which delays and
// returns 5), then delays again, then returns v + 10. Each suspend resumes on a
// worker; the frame carries `v` across both thread boundaries. 5 + 10 = 15.
BEZEL_TEST(async_30_13, nested_async_on_pool) {
    expectValueCrossThread(
        "module M; class C "
        "  public static async method Inner(): Task<int> "
        "    await Task.Delay(5); "
        "    return 5; "
        "  end Inner; "
        "  public static async method Outer(): Task<int> "
        "    var v := await Inner(); "
        "    await Task.Delay(5); "
        "    return v + 10; "
        "  end Outer; "
        "  public static async method Main(): Task<int> "
        "    var r := await Outer(); "
        "    return r; "
        "  end Main; end C;",
        15);
}

// §30.10 (#439): Task.Delay(TimeSpan) overload. TimeSpan.FromMilliseconds(5)
// yields a tick count the runtime converts back to ms. Confirms the second Delay
// overload resolves and drives the same cross-thread path. Returns 8.
BEZEL_TEST(async_30_10, await_task_delay_timespan) {
    expectValueCrossThread(
        "module M; class C "
        "  public static async method Main(): Task<int> "
        "    await Task.Delay(TimeSpan.FromMilliseconds(5)); "
        "    return 8; "
        "  end Main; end C;",
        8);
}

// §30.13 (#439) STRESS: spawn many genuinely-async pool tasks and await them,
// accumulating the sum. The loop awaits 20 Task.Delay-backed producers in
// sequence; each suspends and resumes on a worker, so the state machine
// re-enters the loop 20 times across thread boundaries. The result must be the
// stable total (1+2+...+20 = 210) on every one of the 3 runs, ASan-clean (no
// leak / UAF / double-free as 20 frames + tasks are allocated and reclaimed
// across threads) and TSan-clean (no data race in the pool / task state /
// atomic refcounts under repeated cross-thread completion). The await result is
// bound to a NAMED local (`r`) which round-trips through the heap frame — the
// cross-thread-correct shape; the embedded-expression shape (`total + await ...`)
// is exercised cross-thread by the #439-followup tests below.
BEZEL_TEST(async_30_13, concurrency_stress_sum_of_pool_tasks) {
    expectValueCrossThread(
        "module M; class C "
        "  public static async method Compute(n: int): Task<int> "
        "    await Task.Delay(2); "
        "    return n; "
        "  end Compute; "
        "  public static async method Main(): Task<int> "
        "    var total := 0; var i := 1; "
        "    while i <= 20 do "
        "      var r := await Compute(i); "
        "      total := total + r; "
        "      i := i + 1; "
        "    end; "
        "    return total; "
        "  end Main; end C;",
        210);
}

// ===========================================================================
// #439-FOLLOWUP — cross-thread embedded-await temporaries (§30.2 / §30.13).
//
// An `await` embedded in a LARGER expression (`var x := total + await f();`)
// loads its pre-suspend operand (`total`) BEFORE the suspend and consumes it
// AFTER the resume. Across a REAL cross-thread suspend the resume runs on a
// worker thread with a fresh stack, so the pre-suspend temporary must live in
// the HEAP FRAME (the calloc'd state-machine frame, shared across the suspend),
// not in a function-local stack slot. These tests use Task.Delay-backed
// producers so the awaited task is GENUINELY incomplete at the await point —
// the suspend path is taken and the resume fires on the pool. Before the
// cross-suspend-temp-to-frame fix the pre-suspend operand is garbage after the
// thread hop; after it the value is correct and stable across 3 runs, ASan- and
// TSan-clean.
// ===========================================================================

// #439-followup: the canonical case. `total` (10) is loaded before the await;
// `Compute(5)` delays then returns 5 on a worker; the `+` runs after the
// cross-thread resume. The pre-suspend `total` must survive the thread hop via
// the heap frame. 10 + 5 = 15.
BEZEL_TEST(async_30_13, embedded_await_preserves_operand_cross_thread) {
    expectValueCrossThread(
        "module M; class C "
        "  public static async method Compute(n: int): Task<int> "
        "    await Task.Delay(5); "
        "    return n; "
        "  end Compute; "
        "  public static async method Main(): Task<int> "
        "    var total := 10; "
        "    var x := total + await Compute(5); "
        "    return x; "
        "  end Main; end C;",
        15);
}

// #439-followup: MULTIPLE awaits embedded in one expression cross-thread. Both
// `Compute(3)` and `Compute(4)` are genuinely-incomplete; the running sum
// (`seed` + first result) is a pre-suspend temporary across the SECOND await's
// thread hop and must round-trip the frame. 100 + 3 + 4 = 107.
BEZEL_TEST(async_30_13, multiple_embedded_awaits_one_expr_cross_thread) {
    expectValueCrossThread(
        "module M; class C "
        "  public static async method Compute(n: int): Task<int> "
        "    await Task.Delay(5); "
        "    return n; "
        "  end Compute; "
        "  public static async method Main(): Task<int> "
        "    var seed := 100; "
        "    var x := seed + await Compute(3) + await Compute(4); "
        "    return x; "
        "  end Main; end C;",
        107);
}

// #439-followup: a pre-await LOCAL used AFTER the await in an expression, across
// a cross-thread suspend. `lhs` (40) is a named local set before the await, and
// the await result is added to it. Even though `lhs` is named, the partial
// expression value (the loaded `lhs`) is a pre-suspend temporary in the resumed
// `+`; the frame round-trip must keep it correct across the thread hop. 40+2=42.
BEZEL_TEST(async_30_13, preawait_local_used_after_await_cross_thread) {
    expectValueCrossThread(
        "module M; class C "
        "  public static async method Compute(n: int): Task<int> "
        "    await Task.Delay(5); "
        "    return n; "
        "  end Compute; "
        "  public static async method Main(): Task<int> "
        "    var lhs := 40; "
        "    var r := lhs + await Compute(2); "
        "    return r; "
        "  end Main; end C;",
        42);
}

// #439-followup: a loop-carried accumulator with the await EMBEDDED in the
// loop-body expression, across cross-thread suspends. Each iteration loads
// `total` (pre-suspend), awaits a genuinely-incomplete `Compute(i)` (resumes on
// a worker), then adds. The pre-suspend `total` operand must survive each
// thread hop via the frame (the loop-carried local already does; the loaded
// partial is the anonymous cross-suspend temp). 1+2+3 = 6.
BEZEL_TEST(async_30_13, embedded_await_in_loop_accumulates_cross_thread) {
    expectValueCrossThread(
        "module M; class C "
        "  public static async method Compute(n: int): Task<int> "
        "    await Task.Delay(2); "
        "    return n; "
        "  end Compute; "
        "  public static async method Main(): Task<int> "
        "    var total := 0; var i := 1; "
        "    while i <= 3 do "
        "      total := total + await Compute(i); "
        "      i := i + 1; "
        "    end; "
        "    return total; "
        "  end Main; end C;",
        6);
}

// ===========================================================================
// §30.7 (#440) — ASYNC ITERATORS (IAsyncEnumerable<T> + `await for`).
//
// An async iterator is BOTH `async` and contains `yield`, returning
// IAsyncEnumerable<T>. Its state machine can YIELD a value AND AWAIT a task
// between yields: MoveNextAsync returns a Task<bool> (the consuming `await for`
// awaits it each iteration). The fused yield+await machine reuses the §29/§30
// suspend/resume scaffolding (SuspendKind::AsyncYield). These tests use
// Task.Delay so the awaits GENUINELY suspend on the pool — the cross-thread
// path. expectValueCrossThread runs each 3x and verifies ASan- and TSan-clean
// (the enumerator frame + per-advance Task<bool> are reclaimed across threads).
// ===========================================================================

// §30.7 (#440): the core case — an async iterator yields 1,2,3 with
// `await Task.Delay(...)` between yields; consumed by `await for ... sum += x`.
// The awaits actually suspend on the pool; MoveNextAsync returns a Task<bool>
// the consuming Main awaits each iteration. 1+2+3 = 6.
BEZEL_TEST(async_30_7, async_iterator_yield_with_await_between) {
    expectValueCrossThread(
        "module M; class C "
        "  public static async method Gen(): IAsyncEnumerable<int> "
        "    yield return 1; "
        "    await Task.Delay(5); "
        "    yield return 2; "
        "    await Task.Delay(5); "
        "    yield return 3; "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    var sum := 0; "
        "    await for var x in Gen() do "
        "      sum := sum + x; "
        "    end; "
        "    return sum; "
        "  end Main; end C;",
        6);
}

// §30.7 (#440): an async iterator whose yielded value is COMPUTED via an await
// (`var v := await Compute(i); yield return v;`). Each Compute delays then
// returns i*10 on a worker; the named result `v` round-trips the heap frame
// across the cross-thread suspend before being yielded. (1+2+3)*10 = 60.
BEZEL_TEST(async_30_7, async_iterator_value_computed_via_await) {
    expectValueCrossThread(
        "module M; class C "
        "  public static async method Compute(n: int): Task<int> "
        "    await Task.Delay(5); "
        "    return n * 10; "
        "  end Compute; "
        "  public static async method Gen(): IAsyncEnumerable<int> "
        "    var i := 1; "
        "    while i <= 3 do "
        "      var v := await Compute(i); "
        "      yield return v; "
        "      i := i + 1; "
        "    end; "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    var sum := 0; "
        "    await for var x in Gen() do "
        "      sum := sum + x; "
        "    end; "
        "    return sum; "
        "  end Main; end C;",
        60);
}

// §30.7 (#440): `yield return await Compute(i)` — the await is EMBEDDED in the
// yielded expression, so its pre-suspend temporary lives in the iterator's heap
// spill area (fixupSuspendDominance), surviving the cross-thread resume. A
// `yield break` terminates early (i > 3). Yields 1,2,3 → 6.
BEZEL_TEST(async_30_7, async_iterator_embedded_await_and_yield_break) {
    expectValueCrossThread(
        "module M; class C "
        "  public static async method Compute(n: int): Task<int> "
        "    await Task.Delay(5); "
        "    return n; "
        "  end Compute; "
        "  public static async method Gen(): IAsyncEnumerable<int> "
        "    for var i in 1..5 do "
        "      if i > 3 then yield break; end; "
        "      yield return await Compute(i); "
        "    end; "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    var sum := 0; "
        "    await for var x in Gen() do "
        "      sum := sum + x; "
        "    end; "
        "    return sum; "
        "  end Main; end C;",
        6);
}

// §30.7 (#440): the iterator's yielded values feed a computation in the
// `await for` body that ITSELF awaits — the loop variable `x` is live across the
// body's await (a frame-backed suspend-local). Gen yields 1,2,3 (sync, no inner
// await); the body awaits Double(x) and accumulates. (1+2+3)*2 = 12.
BEZEL_TEST(async_30_7, await_for_body_awaits_uses_loop_var) {
    expectValueCrossThread(
        "module M; class C "
        "  public static async method Double(n: int): Task<int> "
        "    await Task.Delay(5); "
        "    return n * 2; "
        "  end Double; "
        "  public static async method Gen(): IAsyncEnumerable<int> "
        "    yield return 1; yield return 2; yield return 3; "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    var sum := 0; "
        "    await for var x in Gen() do "
        "      sum := sum + await Double(x); "
        "    end; "
        "    return sum; "
        "  end Main; end C;",
        12);
}

// §30.7 (#440): an async iterator parameterized + with a longer run (stress the
// per-advance Task<bool> alloc/free + frame reuse across many advances). Gen(n)
// yields 1..n each after a delay; the body sums them. n=10 → 55. The 3x repeat +
// ASan/TSan legs verify no leak / race as 10 advances allocate and free a
// per-advance task each, with the single frame reused throughout.
BEZEL_TEST(async_30_7, async_iterator_parameterized_stress) {
    expectValueCrossThread(
        "module M; class C "
        "  public static async method Gen(n: int): IAsyncEnumerable<int> "
        "    var i := 1; "
        "    while i <= n do "
        "      await Task.Delay(2); "
        "      yield return i; "
        "      i := i + 1; "
        "    end; "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    var sum := 0; "
        "    await for var x in Gen(10) do "
        "      sum := sum + x; "
        "    end; "
        "    return sum; "
        "  end Main; end C;",
        55);
}

// §30.8 / §30.7 (#765): `await for ... with token` (cancellation) is now LOWERED
// (was deferred per #440). For a compiler-generated async iterator the source IS
// the enumerator, so the token is evaluated then discarded (honoring it for
// cancellation, §30.8, remains deferred). Compiles and iterates correctly.
BEZEL_TEST(async_30_8, await_for_with_cancellation_token_lowered) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; class C "
        "  public static async method Gen(): IAsyncEnumerable<int> "
        "    yield return 1; "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    var ct := CancellationToken.None; "
        "    var sum := 0; "
        "    await for var x in Gen() with ct do "
        "      sum := sum + x; "
        "    end; "
        "    return sum; "
        "  end Main; end C;"),
        1);
}

// §30.7 (#440) DEFERRED: a `yield` inside a try region in an async iterator —
// resuming the fused state machine into the protected scope across a suspend is
// unimplemented. Rejected with a clean diagnostic (compile FAILS) rather than
// emitting code that crashes at runtime.
BEZEL_TEST(async_30_7, yield_in_try_in_async_iterator_deferred) {
    BEZEL_EXPECT_TRUE(compileFails(
        "module M; class C "
        "  public static async method Gen(): IAsyncEnumerable<int> "
        "    try "
        "      yield return 1; "
        "    catch Exception => yield return 2; "
        "    end; "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    var sum := 0; "
        "    await for var x in Gen() do sum := sum + x; end; "
        "    return sum; "
        "  end Main; end C;"));
}

// §30.7 (#440) DEFERRED: an `await` inside a try region in an async iterator is
// likewise rejected (the catch-arm await needs the handler scope re-established
// at resume).
BEZEL_TEST(async_30_7, await_in_try_in_async_iterator_deferred) {
    BEZEL_EXPECT_TRUE(compileFails(
        "module M; class C "
        "  public static async method Compute(): Task<int> "
        "    return 1; "
        "  end Compute; "
        "  public static async method Gen(): IAsyncEnumerable<int> "
        "    try "
        "      yield return 1; "
        "    catch Exception => yield return await Compute(); "
        "    end; "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    var sum := 0; "
        "    await for var x in Gen() do sum := sum + x; end; "
        "    return sum; "
        "  end Main; end C;"));
}

// ===========================================================================
// §30.9 / #441 (countAwaits TryStmt fix): cross-thread await inside a try body.
// These tests use a GENUINELY-incomplete producer (Task.Delay) so the await
// ACTUALLY suspends cross-thread; the resume fires on a pool worker into the
// try-protected region. Before the fix, countAwaits missed TryStmt so numAwaits
// was too small, the resume switch lacked the in-try state, the default branch
// (return) fired, bz_task_complete was never called, and bz_task_await_sync
// hung forever. After the fix each test COMPLETES with the right value.
// ===========================================================================

// §30.9 / #441: try + cross-thread await (normal completion). Producer delays
// then returns 42; the await suspends, resumes on a worker inside the try body,
// and x is set to 42. The catch is NOT entered. Result 43 proves the resume
// reached the post-await return. Was a hang before the fix.
BEZEL_TEST(async_30_9_441, await_in_try_cross_thread_normal) {
    expectValueCrossThread(
        "module M; class C "
        "  public static async method Producer(n: int): Task<int> "
        "    await Task.Delay(5); "
        "    return n; "
        "  end Producer; "
        "  public static async method Main(): Task<int> "
        "    var x := 0; "
        "    try x := await Producer(42); catch Exception => x := 999; end; "
        "    return x + 1; "
        "  end Main; end C;",
        43);
}

// §30.9 / #441: try/finally + cross-thread await (normal completion). Producer
// delays then returns 5; the finally runs after the try body. x=5, ran=100:
// result 105. Verifies the finally runs even when the resume is cross-thread.
// Was a hang before the fix.
BEZEL_TEST(async_30_9_441, await_in_try_finally_cross_thread) {
    expectValueCrossThread(
        "module M; class C "
        "  public static async method Producer(n: int): Task<int> "
        "    await Task.Delay(5); "
        "    return n; "
        "  end Producer; "
        "  public static async method Main(): Task<int> "
        "    var x := 0; var ran := 0; "
        "    try x := await Producer(5); finally ran := 100; end; "
        "    return x + ran; "
        "  end Main; end C;",
        105);
}

// ===========================================================================
// §30.7 / #442 (IAsyncEnumerable routing by return type): an async method
// declared to return IAsyncEnumerable<T> but whose body has NO `yield` must be
// routed to emitAsyncIteratorMethod (not emitAsyncMethod). Before the fix such
// a method was emitted as a bz_task* result; an `await for` that loaded a vtable
// from the task pointer crashed with SIGSEGV.
// ===========================================================================

// §30.7 / #442: empty async IAsyncEnumerable body (no yield, no await). The
// iterator is immediately exhausted; `await for` runs zero iterations. Main
// returns 0. Before the fix this was a SIGSEGV.
BEZEL_TEST(async_30_7_442, async_iasyncenumerable_empty_body_zero_iters) {
    expectValueLeakFree(
        "module M; class C "
        "  public static async method Gen(): IAsyncEnumerable<int> "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    var sum := 0; "
        "    await for var x in Gen() do sum := sum + x; end; "
        "    return sum; "
        "  end Main; end C;",
        0);
}

// §30.7 / #442: async IAsyncEnumerable body with an await but no yield (the
// await completes before exhaustion). The iterator produces zero values but
// the await itself must not crash. sum stays 0; Main returns 7.
// Before the fix the routing to emitAsyncMethod caused a SIGSEGV on the await.
BEZEL_TEST(async_30_7_442, async_iasyncenumerable_await_only_no_yield) {
    expectValueLeakFree(
        "module M; class C "
        "  public static async method Noop(): Task "
        "    return; "
        "  end Noop; "
        "  public static async method Gen(): IAsyncEnumerable<int> "
        "    await Noop(); "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    var sum := 0; "
        "    await for var x in Gen() do sum := sum + x; end; "
        "    return sum + 7; "
        "  end Main; end C;",
        7);
}

// §30.7 / #442: async IAsyncEnumerable with a yield AFTER an await — this has
// a yield so would have been routed correctly pre-fix, but is included as a
// regression guard to ensure the return-type routing doesn't break the normal
// path (yield after await emitted correctly). sum = 42.
BEZEL_TEST(async_30_7_442, async_iasyncenumerable_yield_after_await_regression) {
    expectValueLeakFree(
        "module M; class C "
        "  public static async method Produce(n: int): Task<int> "
        "    return n; "
        "  end Produce; "
        "  public static async method Gen(): IAsyncEnumerable<int> "
        "    var v := await Produce(42); "
        "    yield return v; "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    var sum := 0; "
        "    await for var x in Gen() do sum := sum + x; end; "
        "    return sum; "
        "  end Main; end C;",
        42);
}

// ===========================================================================
// §28.12 / §30 — `await using var` (async resource disposal).
//
// `await using var r := <expr>;` binds `r` and, at scope end on EVERY exit
// path, invokes `r.DisposeAsync()` (which returns a Task), awaits that Task
// (a proper suspend point), then reclaims the block (physical reclamation,
// §28.12 step 2). This section tests:
//
//   - Normal path (sync and cross-thread DisposeAsync): disposed == 1 after scope.
//   - Scope timing: DisposeAsync NOT called inside scope, called at scope exit.
//   - Fault path (exception in body): sync-dispose fallback (destructor runs,
//     disposed == 1 via the sync path — §28.12: class should have a destructor
//     for the synchronous fallback; the async await-across-active-unwind path is
//     a deferred increment, reported as the sync-dispose fallback decision).
//   - Body with explicit awaits: `await using` + explicit awaits in the body both
//     work; the dispose-await is a properly counted suspend point (#441 analogue).
//   - Regression: synchronous `using var` and async-without-using are unaffected.
//
// The test class AsyncRes:
//   - `disposed` static counter: DisposeAsync increments by 10; destructor by 1.
//   - DisposeAsync (the async path) is distinguishable from Dispose (sync, +1) vs
//     DisposeAsync (+10), so we can tell which path ran.
//   - For cross-thread tests DisposeAsync awaits Task.Delay so it genuinely
//     suspends and resumes on the pool.
//
// Implementation note: DisposeAsync is found via findMethodByName + methodFns_ at
// registration time (the concrete class `AsyncRes` is known from `new AsyncRes()`).
// The fault-path fallback uses the synchronous Dispose() (spec §28.12: "class
// should have a destructor for the synchronous fallback"). The dispose-await's
// state block is counted by countAwaits (+1 per `isAwaitUsing` LocalDecl) and
// any unreached blocks (sync-fallback path) are terminated after lowerBody.
// ===========================================================================

// Shared class bodies for the await-using tests. All classes are inline in
// the program strings for simplicity; the counter is a static int so the
// exit code can carry the disposal count.

// §28.12 (#443): the simplest case — `await using var r` in an async method,
// synchronous DisposeAsync (no actual await inside it). The scope ends, the
// compiler emits `await r.DisposeAsync()` followed by `free(r)`. DisposeAsync
// increments the static counter by 10; destructor by 1. On the normal path
// DisposeAsync is called → disposed == 10; exit code 10 proves the ASYNC path
// ran (not the sync/destructor path which would give 1).
BEZEL_TEST(async_28_12, await_using_disposes_via_disposeasync_sync) {
    expectValueLeakFree(
        "module M; "
        "class AsyncRes: IAsyncDisposable "
        "  public static disposed: int := 0; "
        "  public async method DisposeAsync(): Task "
        "    AsyncRes.disposed := AsyncRes.disposed + 10; "
        "  end DisposeAsync; "
        "  destructor AsyncRes.disposed := AsyncRes.disposed + 1; end destructor; "
        "end AsyncRes; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    do "
        "      await using var r := new AsyncRes(); "
        "    end; "
        "    return AsyncRes.disposed; "
        "  end Main; "
        "end C;",
        10);
}

// §28.12 (#443): scope timing — DisposeAsync is NOT called inside the scope
// (the counter is 0 during the body), and IS called exactly once at scope exit
// (the counter is 10 after). `during_scope` captures the counter mid-body;
// `after_scope` captures it post-disposal. Exit code = during_scope * 100 +
// after_scope; 10 proves the async path ran once and not during the body.
BEZEL_TEST(async_28_12, await_using_disposal_timing) {
    expectValueLeakFree(
        "module M; "
        "class AsyncRes: IAsyncDisposable "
        "  public static disposed: int := 0; "
        "  public async method DisposeAsync(): Task "
        "    AsyncRes.disposed := AsyncRes.disposed + 10; "
        "  end DisposeAsync; "
        "  destructor AsyncRes.disposed := AsyncRes.disposed + 1; end destructor; "
        "end AsyncRes; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    var during_scope := -1; "
        "    do "
        "      await using var r := new AsyncRes(); "
        "      during_scope := AsyncRes.disposed; "
        "    end; "
        "    var after_scope := AsyncRes.disposed; "
        "    if during_scope <> 0 then return 99; end; "
        "    return after_scope; "
        "  end Main; "
        "end C;",
        10);
}

// §28.12 (#443): cross-thread DisposeAsync. The DisposeAsync method uses
// `await Task.Delay(5)` so it genuinely suspends on the pool. Main awaits the
// DisposeAsync Task cross-thread; the resume fires on a worker. Disposed == 10
// proves the async path ran. Run 3x, ASan-clean, TSan-clean.
BEZEL_TEST(async_28_12, await_using_crossthread_disposes_via_disposeasync) {
    expectValueCrossThread(
        "module M; "
        "class AsyncRes: IAsyncDisposable "
        "  public static disposed: int := 0; "
        "  public async method DisposeAsync(): Task "
        "    await Task.Delay(5); "
        "    AsyncRes.disposed := AsyncRes.disposed + 10; "
        "  end DisposeAsync; "
        "  destructor AsyncRes.disposed := AsyncRes.disposed + 1; end destructor; "
        "end AsyncRes; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    do "
        "      await using var r := new AsyncRes(); "
        "    end; "
        "    return AsyncRes.disposed; "
        "  end Main; "
        "end C;",
        10);
}

// §28.12 (#443): fault path — a synchronous throw inside an `await using` scope.
// Per §28.12, the class should have a destructor for the synchronous fallback;
// the awaiting-across-active-unwind path is a deferred increment. The sync
// Dispose (destructor, +1) runs on the unwind path; DisposeAsync (+10) does NOT.
// The caught exception leaves the destructor running once → disposed == 1.
// Exit code 1 proves the SYNC path ran (not the async path which would give 10).
BEZEL_TEST(async_28_12, await_using_fault_path_sync_dispose_fallback) {
    BEZEL_EXPECT_EQ(compileAndRun(
        "module M; "
        "class AsyncRes: IAsyncDisposable "
        "  public static disposed: int := 0; "
        "  public async method DisposeAsync(): Task "
        "    AsyncRes.disposed := AsyncRes.disposed + 10; "
        "  end DisposeAsync; "
        "  destructor AsyncRes.disposed := AsyncRes.disposed + 1; end destructor; "
        "end AsyncRes; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    var result := 0; "
        "    try "
        "      do "
        "        await using var r := new AsyncRes(); "
        "        throw new InvalidOperationException(); "
        "      end; "
        "    catch Exception => "
        "      result := AsyncRes.disposed; "
        "    end; "
        "    return result; "
        "  end Main; "
        "end C;"),
        1);
}

// §28.12 (#443): `await using var` scope body also contains explicit awaits.
// The dispose-await is a properly-counted suspend point (countAwaits +1 per
// isAwaitUsing); the body's explicit awaits are also counted. All suspend
// points resume correctly. Sum = 5+3 = 8; disposed (async, +10) == 10.
// Exit code = sum * 10 + disposed/10 = 80 + 1 = 81.
BEZEL_TEST(async_28_12, await_using_with_awaits_in_scope_body) {
    expectValueCrossThread(
        "module M; "
        "class AsyncRes: IAsyncDisposable "
        "  public static disposed: int := 0; "
        "  public async method DisposeAsync(): Task "
        "    await Task.Delay(2); "
        "    AsyncRes.disposed := AsyncRes.disposed + 10; "
        "  end DisposeAsync; "
        "  destructor AsyncRes.disposed := AsyncRes.disposed + 1; end destructor; "
        "end AsyncRes; "
        "class C "
        "  public static async method Producer(n: int): Task<int> "
        "    await Task.Delay(2); "
        "    return n; "
        "  end Producer; "
        "  public static async method Main(): Task<int> "
        "    var sum := 0; "
        "    do "
        "      await using var r := new AsyncRes(); "
        "      var x := await Producer(5); "
        "      var y := await Producer(3); "
        "      sum := x + y; "
        "    end; "
        "    return sum * 10 + AsyncRes.disposed / 10; "
        "  end Main; "
        "end C;",
        81);
}

// §28.12 (#443): two `await using var` in the same scope — both disposed, in
// reverse declaration order, each via their own DisposeAsync call. Total
// disposed == 20 (two resources * 10 each). ASan-clean.
BEZEL_TEST(async_28_12, two_await_using_vars_both_disposed) {
    expectValueLeakFree(
        "module M; "
        "class AsyncRes: IAsyncDisposable "
        "  public static disposed: int := 0; "
        "  public async method DisposeAsync(): Task "
        "    AsyncRes.disposed := AsyncRes.disposed + 10; "
        "  end DisposeAsync; "
        "  destructor AsyncRes.disposed := AsyncRes.disposed + 1; end destructor; "
        "end AsyncRes; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    do "
        "      await using var a := new AsyncRes(); "
        "      await using var b := new AsyncRes(); "
        "    end; "
        "    return AsyncRes.disposed; "
        "  end Main; "
        "end C;",
        20);
}

// §28.12 (#443) regression: synchronous `using var` in an async method is
// unaffected by the `await using` changes — disposed via the sync Dispose path.
// Destructor increments by 1; disposed == 1. No async path taken.
BEZEL_TEST(async_28_12, sync_using_var_in_async_method_unaffected) {
    expectValueLeakFree(
        "module M; "
        "class Res "
        "  public static disposed: int := 0; "
        "  destructor Res.disposed := Res.disposed + 1; end destructor; "
        "end Res; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    do using var r := new Res(); end; "
        "    return Res.disposed; "
        "  end Main; "
        "end C;",
        1);
}

// §28.12 (#443) regression: async method without `await using` is unaffected.
// Plain awaits still work; no disposal side-effects.
BEZEL_TEST(async_28_12, async_without_await_using_unaffected) {
    expectValueLeakFree(
        "module M; class C "
        "  public static async method Producer(n: int): Task<int> "
        "    return n; "
        "  end Producer; "
        "  public static async method Main(): Task<int> "
        "    var x := await Producer(42); "
        "    return x; "
        "  end Main; end C;",
        42);
}

// ===========================================================================
// #445 — plain `var t := AsyncMethod(); await t;` leaks the bz_task.
//
// A named-local task bound with a PLAIN `var` (not `using var`) was not freed
// by either path:
//   - lowerAwait only freed anonymous CallExpr operands (`await Producer()`),
//     never a named local (`await t`).
//   - The using-var disposal path was gated on `ld->isUsing`, which a plain
//     `var` never sets.
//
// Fix: treat a plain `var` local whose type is Task/Task<T>/OwningTask<T> the
// same as a `using var` task — register a scope-end `bz_task_fire_and_forget`
// disposal. The await site NEVER frees a named local (no change there), so
// exactly-once is guaranteed across all four shapes:
//   - anonymous producer  → freed at await site (existing, unchanged)
//   - using var task      → freed at scope end via fire-and-forget (existing)
//   - plain var task awaited → freed at scope end via fire-and-forget (NEW)
//   - plain var task NOT awaited → freed at scope end via fire-and-forget (NEW)
//
// §30.5 / §28: a Task's Dispose "does not block waiting for completion";
// bz_task_fire_and_forget implements this (free immediately if done, else
// registers a continuation to free when the task eventually completes).
// ===========================================================================

// #445 / §30.5: `var t := Producer(); var x := await t;` — the canonical leak.
// LSan verifies the task is freed exactly once at scope end (not at the await
// site). The result must be correct (42) and the program leak-free.
BEZEL_TEST(async_30_5_445, plain_var_task_awaited_is_freed_at_scope_end) {
    expectValueLeakFree(
        "module M; class C "
        "  public static async method Producer(): Task<int> "
        "    return 42; "
        "  end Producer; "
        "  public static async method Main(): Task<int> "
        "    var t := Producer(); "
        "    var x := await t; "
        "    return x; "
        "  end Main; end C;",
        42);
}

// #445 / §30.5: `using var t := Producer(); var x := await t;` — the
// already-correct case. Verifies no double-free is introduced by the fix:
// the using-var path still fires exactly once at scope end, and the await
// site does NOT also free a named-local task. LSan-clean.
BEZEL_TEST(async_30_5_445, using_var_task_awaited_no_double_free) {
    expectValueLeakFree(
        "module M; class C "
        "  public static async method Producer(): Task<int> "
        "    return 7; "
        "  end Producer; "
        "  public static async method Main(): Task<int> "
        "    using var t := Producer(); "
        "    var x := await t; "
        "    return x; "
        "  end Main; end C;",
        7);
}

// #445 / §30.5: `var x := await Producer();` (anonymous) — the other
// already-correct case. The task is freed at the await site. No regression:
// LSan-clean, result correct.
BEZEL_TEST(async_30_5_445, anonymous_task_freed_at_await_no_regression) {
    expectValueLeakFree(
        "module M; class C "
        "  public static async method Producer(): Task<int> "
        "    return 55; "
        "  end Producer; "
        "  public static async method Main(): Task<int> "
        "    var x := await Producer(); "
        "    return x; "
        "  end Main; end C;",
        55);
}

// #445 / §30.5: plain `var t := Producer();` created but NEVER awaited —
// the task must be freed at scope end (fire-and-forget). Without the fix
// this would also leak. Result 0 (Main returns 0; t is unused beyond binding).
BEZEL_TEST(async_30_5_445, plain_var_task_never_awaited_freed_at_scope_end) {
    expectValueLeakFree(
        "module M; class C "
        "  public static async method Producer(): Task<int> "
        "    return 99; "
        "  end Producer; "
        "  public static async method Main(): Task<int> "
        "    var t := Producer(); "
        "    return 0; "
        "  end Main; end C;",
        0);
}

// #445 / §30.5: cross-thread plain-var task — `Producer` delays on the pool,
// then `var t := Producer(); var x := await t;` awaits it cross-thread. The
// task must be freed at scope end (not at the await site) and exactly once
// across the real thread boundary. ASan- and TSan-clean.
BEZEL_TEST(async_30_5_445, plain_var_task_awaited_cross_thread_clean) {
    expectValueCrossThread(
        "module M; class C "
        "  public static async method Producer(): Task<int> "
        "    await Task.Delay(5); "
        "    return 21; "
        "  end Producer; "
        "  public static async method Main(): Task<int> "
        "    var t := Producer(); "
        "    var x := await t; "
        "    return x; "
        "  end Main; end C;",
        21);
}

// #445 / §30.5: two plain-var tasks, both awaited in sequence. Each must be
// freed at its own scope end; no double-free, no leak. 10 + 20 = 30.
BEZEL_TEST(async_30_5_445, two_plain_var_tasks_both_awaited_clean) {
    expectValueLeakFree(
        "module M; class C "
        "  public static async method Compute(n: int): Task<int> "
        "    return n; "
        "  end Compute; "
        "  public static async method Main(): Task<int> "
        "    var t1 := Compute(10); "
        "    var t2 := Compute(20); "
        "    var a := await t1; "
        "    var b := await t2; "
        "    return a + b; "
        "  end Main; end C;",
        30);
}

// #445 / §30.5: two plain-var tasks cross-thread — both Producers delay on the
// pool. `var t1 := Compute(10); var t2 := Compute(20);` then both awaited.
// Both tasks must be freed at scope end exactly once; the results are correct.
// ASan- and TSan-clean (two heap frames + tasks across two thread boundaries).
// This exercises the fix in a cross-thread context with multiple named locals.
BEZEL_TEST(async_30_5_445, two_plain_var_tasks_awaited_cross_thread_clean) {
    expectValueCrossThread(
        "module M; class C "
        "  public static async method Compute(n: int): Task<int> "
        "    await Task.Delay(3); "
        "    return n; "
        "  end Compute; "
        "  public static async method Main(): Task<int> "
        "    var t1 := Compute(10); "
        "    var t2 := Compute(20); "
        "    var a := await t1; "
        "    var b := await t2; "
        "    return a + b; "
        "  end Main; end C;",
        30);
}

// v0.190 SB-01 (§28.3 "Task-family calls produce owned frames" + §30.5, #1786):
// caller-side mint + exactly-once disposal. Under v0.190 a `Task`-family CALL
// MINTS an OWNED task at the call site (SB01-A), so `var t := Producer();` is a
// plain OWNED task local — where pre-v0.190 it was a borrow. Being borrow-awaited
// (§30.5 line 7341), it keeps its ordinary §28.3 discharge and auto-drops at
// scope end. This test pins that the new Owned classification does NOT introduce
// a SECOND disposal on top of the codegen task path (#445): the task frame is
// freed EXACTLY ONCE (LSan / ASan verify no leak AND no double-free). The
// codegen else-if chain routes the plain-var task through the single #445
// `d.task` disposal (taskTypeOfLocal), skipping the auto-drop registerUsing-
// Disposal — so the Owned+autoDrop flag is disposal-inert here. Result 42.
// // per §28.3, §30.5 (v0.190 SB-01)
BEZEL_TEST(async_30_5_445, v0190_minted_plain_var_task_disposed_exactly_once) {
    expectValueLeakFree(
        "module M; class C "
        "  public static async method Producer(): Task<int> "
        "    return 42; "
        "  end Producer; "
        "  public static async method Main(): Task<int> "
        "    var t := Producer(); "
        "    var x := await t; "
        "    return x; "
        "  end Main; end C;",
        42);
}

// v0.190 SB-01 FUNCTION-VALUE arm (§28.3 "Task-family calls produce owned
// frames" — "an invocation through a function value"; #1865). A call through a
// function VALUE whose declared return slot is a plain `Task<T>` type MINTS an
// OWNED task at the call site (the SB01-A residual this fix closes). Awaited as a
// borrow (§30.5), the plain-var task auto-drops at scope end. This pins that the
// function-value mint reaches the SAME single codegen task disposal (#445 `d.task`)
// as an instance/static Task call — the frame is freed EXACTLY ONCE (LSan/ASan:
// no leak, no double-free), the Owned+autoDrop flag disposal-inert here. The
// classification is the ONLY change; the codegen disposal path is untouched
// (SB01-D1). Result 42. // per §28.3, §30.5 (v0.190 SB-01), #1865
BEZEL_TEST(async_30_5_445, v0190_minted_funcval_task_disposed_exactly_once) {
    expectValueLeakFree(
        "module M; class C "
        "  public static async method Producer(): Task<int> "
        "    return 42; "
        "  end Producer; "
        "  public static async method Main(): Task<int> "
        "    var mk: () -> Task<int> := Producer; "
        "    var t := mk(); "
        "    var x := await t; "
        "    return x; "
        "  end Main; end C;",
        42);
}

// ===========================================================================
// §30.2 / §14.1 (#444 / #509): `await` requires a Task / Task<T> / OwningTask<T>
// operand.  Before #509, `Task.FromResult<int>(42)` was misparsed (§A.7 had no
// call-site generic type argument syntax) as a chained comparison yielding bool,
// causing `await bool` (i1) to SIGSEGV in lowerAwait.  After #509, the parser
// correctly handles `Method<T>(…)` as an explicit-type-arg call:
// `Task.FromResult<int>(42)` now returns `Task<int>` and the `await` is valid.
// ===========================================================================

// §14.1 / §30.2 (#509 fix for #444 repro): `Task.FromResult<int>(42)` is now
// parsed correctly as an explicit-type-arg call (not a comparison).  The
// `await` of its `Task<int>` result must compile and return 42.
BEZEL_TEST(async_444, await_call_site_type_arg_misparsed_as_comparison_is_error) {
    expectValueLeakFree(
        "module M; class C "
        "  public static async method Main(): Task<int> "
        "    var x := await Task.FromResult<int>(42); "
        "    return x; "
        "  end Main; end C;",
        42);
}

// #738 / stdlib §6.7: Task.FromException<T>(exc) produces an already-faulted
// Task<T>; awaiting it re-raises the original exception. Previously unimplemented
// in codegen — the local was typed i32 and `bz_task_is_done` got an i32 (IR_FAIL).
BEZEL_TEST(async_30_10, from_exception_await_reraises_738) {
    expectValueLeakFree(
        "module M; class AppErr: Exception end AppErr; class C "
        "  public static async method Main(): Task<int> "
        "    using var t := Task.FromException<int>(new AppErr()); "
        "    try var v := await t; return 99; "
        "    catch AppErr => return 5; end; "
        "  end Main; end C;",
        5);
}

// #738 / stdlib §6.7: Task.FromCancelled<T>() produces a faulted Task<T> carrying
// an OperationCanceledException; awaiting re-raises it. (Was sema-typed but had
// no codegen → the await hung.)
BEZEL_TEST(async_30_10, from_cancelled_await_reraises_oce_738) {
    expectValueLeakFree(
        "module M; class C "
        "  public static async method Main(): Task<int> "
        "    using var t := Task.FromCancelled<int>(); "
        "    try var v := await t; return 99; "
        "    catch OperationCanceledException => return 8; end; "
        "  end Main; end C;",
        8);
}

// §30.2 / §A.7 (#444): `await` on a plain integer literal is also rejected
// with a clean error (the general guard for await-of-non-task).
BEZEL_TEST(async_444, await_int_literal_non_task_is_error) {
    BEZEL_EXPECT_TRUE(compileFails(
        "module M; class C "
        "  public static async method Main(): Task<int> "
        "    var x := await 42; "
        "    return x; "
        "  end Main; end C;"));
}

// §30.3 (#444) no-over-rejection: `Task.FromResult(42)` (inference form, no
// type args) still compiles and produces the correct value.
BEZEL_TEST(async_444, await_task_from_result_inference_form_works) {
    expectValueLeakFree(
        "module M; class C "
        "  public static async method Main(): Task<int> "
        "    var x := await Task.FromResult(42); "
        "    return x; "
        "  end Main; end C;",
        42);
}

// §17.1 / §A.7 (#444) no-over-rejection: a genuine comparison `3 < 5`
// assigns to a bool local and still compiles and runs correctly.
BEZEL_TEST(async_444, genuine_comparison_bool_still_works) {
    expectValueLeakFree(
        "module M; class C "
        "  public static async method Main(): Task<int> "
        "    var b := 3 < 5; "
        "    if b then return 1; else return 0; end; "
        "  end Main; end C;",
        1);
}
