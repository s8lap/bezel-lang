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
// pre-/post-loop locals live across the loop's suspends. DEFERRED (not
// exercised; rejected with a diagnostic or out of scope): `await` inside
// try/catch/finally (§30.9, the new boundary — rejected with a clean
// diagnostic), a real cross-thread suspend, atomic refcounts, and async
// iterators.

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

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

namespace {

namespace fs = std::filesystem;

// Compile `source` to a temp native executable via the real Driver and run it;
// returns the exit code, or -1 on compile/link/run failure.
int compileAndRun(const std::string& source) {
    static int counter = 0;
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_async_" + std::to_string(counter++);
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
    static int counter = 0;
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_async_neg_" + std::to_string(counter++);
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

std::string runtimeSourcePath() {
    fs::path here(__FILE__);                       // tests/codegen/<this>
    fs::path root = here.parent_path()             // tests/codegen
                        .parent_path()             // tests
                        .parent_path();            // repo root
    return (root / "src" / "runtime" / "bezel_runtime.cpp").string();
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

    static int counter = 0;
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_async_san_" + san + "_" + std::to_string(counter++);
    std::string objPath = (dir / (stem + ".o")).string();
    std::string rtObj   = (dir / (stem + "_rt.o")).string();
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
        std::remove(rtObj.c_str());
        std::remove(exe.c_str());
    };
    std::string rtCmd =
        "clang++ -std=c++20 -g -fsanitize=" + san + " -pthread -c " +
        runtimeSourcePath() + " -o " + rtObj + " > /dev/null 2>&1";
    if (std::system(rtCmd.c_str()) != 0) { cleanup(); return -1; }

    std::string linkCmd = "clang++ -fsanitize=" + san + " -g " + objPath + " " +
                          rtObj + " -lm -lpthread -lstdc++ -o " + exe +
                          " > /dev/null 2>&1";
    if (std::system(linkCmd.c_str()) != 0) { cleanup(); return -1; }

    std::string runCmd =
        std::string(runOpts) + " " + exe + " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return -1;
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

// §30.9 deferred tail (#438): `await` inside a CATCH ARM body is still NOT
// supported — resuming into a catch handler (re-establishing the caught-exception
// disposal scope across a suspend) is a further increment. Rejected with a clear
// diagnostic and no malformed IR.
BEZEL_TEST(async_30_9, await_in_catch_arm_is_rejected) {
    BEZEL_EXPECT_TRUE(compileFails(
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
        "  end Main; end C;"));
}

// §30.9 deferred tail (#438): `await` inside a `finally` is likewise rejected.
BEZEL_TEST(async_30_9, await_in_finally_is_rejected) {
    BEZEL_EXPECT_TRUE(compileFails(
        "module M; class C "
        "  public static async method Producer(n: int): Task<int> "
        "    return n; "
        "  end Producer; "
        "  public static async method Main(): Task<int> "
        "    var x := 1; "
        "    try x := 1; finally x := await Producer(5); end; "
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
// cross-thread-correct shape; an await embedded in a larger expression
// (`total + await ...`) relies on stack-slot dominance repair that does not yet
// survive a real cross-thread suspend (a deferred increment).
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
