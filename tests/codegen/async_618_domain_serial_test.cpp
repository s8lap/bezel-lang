// Codegen — §30.13 serial-per-domain execution (#618).
//
// These tests verify that the domain-based scheduler enforces at-most-one-active
// continuation per domain.  They are structured to FAIL or FLAKE deterministically
// on the old 4-thread concurrent pool (where multiple workers can run in parallel
// and race on unsynchronized shared state), and to PASS reliably on the new
// domain-serializing scheduler.
//
// Test (b) — §30.13 serialization stress:
//   N tasks each do:
//     1. await Task.Delay(ms)   — suspend; timer completes on a pool thread
//     2. read static counter C.val
//     3. write C.val := old + 1
//   Under serial-per-domain, steps 2-3 are never concurrent (at most one
//   continuation of the domain runs at a time), so the final counter is exactly N.
//   Under the old 4-worker concurrent pool, all N resumptions can race: multiple
//   workers may simultaneously read the same stale value and all write the same
//   incremented value, producing a count < N.
//
// Test (c) — §30.13 run-to-completion ordering:
//   A single task does TWO sequential writes within one continuation (no await
//   between them).  A second task, after its own await, observes both writes.
//   Under serial-per-domain, the second task always sees BOTH writes from the
//   first task (the first task's continuation completes without interruption).
//   This test is a run-to-completion sanity check rather than a race detector,
//   but its correct result (both writes visible) depends on the serial guarantee.
//
// All tests use expectValueCrossThread (3x repetition + ASan/TSan) to surface
// non-determinism that would indicate a race under the old scheduler.
//
// Spec derivation: §30.13 (v0.131+):
//   "at any instant, at most one continuation belonging to a domain is running,
//    and a continuation, once started, runs without interruption from the domain's
//    other work until it suspends at an `await`, completes, or propagates an
//    exception (run-to-completion)."
//
// Note on the stress test (#618b):
//   Under the OLD (pre-#618) concurrent pool, all N Delay timers fire in a small
//   window; up to 4 workers simultaneously try to execute the post-await increment.
//   Each worker reads the same stale counter value and writes it+1, so N parallel
//   increments collapse to fewer effective increments.  The test expects exactly N;
//   the old scheduler reliably produces < N for N >= 4 with overlapping delays.
//   Under the new domain-based scheduler, even though all N continuations are
//   ready, the domain's `active` flag ensures only one runs at a time — so each
//   increment sees the result of the previous, and the final value is N.

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
    std::string stem = "bezel_618_" + std::to_string(counter++);
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

std::string runtimeSourcePath() {
    fs::path here(__FILE__);
    fs::path root = here.parent_path().parent_path().parent_path();
    return (root / "src" / "runtime" / "bezel_runtime.cpp").string();
}

// Sanitizer verification (mirrors async_test.cpp::runUnderSanitizer).
int runUnderSanitizer(const std::string& source, const std::string& san,
                      const char* runOpts) {
    if (std::system("clang++ --version > /dev/null 2>&1") != 0) return -1;

    static int counter = 0;
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_618_san_" + san + "_" + std::to_string(counter++);
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

    std::string runCmd = std::string(runOpts) + " " + exe + " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return -1;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 99) return 1;  // sanitizer hit
    return 0;                                                       // clean
}

// Run 3x for determinism + ASan + TSan (mirrors async_test.cpp expectValueCrossThread).
void expectValueCrossThread618(const std::string& source, int expected) {
    for (int i = 0; i < 3; ++i)
        BEZEL_EXPECT_EQ(compileAndRun(source), expected);
    int lsan = runUnderSanitizer(source, "address",
                                 "ASAN_OPTIONS=detect_leaks=1:exitcode=99");
    if (lsan != -1) BEZEL_EXPECT_EQ(lsan, 0);
    int tsan = runUnderSanitizer(source, "thread",
                                 "TSAN_OPTIONS=exitcode=99");
    if (tsan != -1) BEZEL_EXPECT_EQ(tsan, 0);
}

}  // namespace

// ---------------------------------------------------------------------------
// §30.13 (#618b) SERIALIZATION STRESS — unsynchronized shared counter.
//
// 8 tasks each await Task.Delay(3ms) then do an unsynchronized read-modify-write
// on a static counter in their own continuation (no await between read and write).
// Under serial-per-domain, at most one continuation runs at a time: each sees the
// previous task's result, so the final counter is exactly 8.
//
// Reasoning for why the old scheduler would fail:
//   With 4 pool workers, up to 4 resumptions from the 8 expiring Delay timers can
//   land on different workers simultaneously.  If 4 workers all execute the
//   read-modify-write concurrently, each reading counter=0 and writing counter=1,
//   the net effect of 4 increments is a count of 1 (not 4).  The test expects 8;
//   the old scheduler reliably produces < 8 when delays overlap.  (This is
//   verified by reasoning: the code contains no synchronization on the counter,
//   so concurrent reads yield the same stale value, and all writes land the same
//   new value — classic lost-update race.)
//
// Run 3x + ASan + TSan: determinism and no memory errors.
// ---------------------------------------------------------------------------
BEZEL_TEST(async_618_domain_serial, serialization_stress_unsync_counter) {
    expectValueCrossThread618(
        "module M; class C "
        "  public static counter: int := 0; "
        "  public static async method Worker(id: int): Task "
        "    await Task.Delay(3); "
        // After the await, this is ONE continuation — no further suspend points.
        // Read, increment, write — atomic at the continuation level.
        "    var old := C.counter; "
        "    C.counter := old + 1; "
        "  end Worker; "
        "  public static async method Main(): Task<int> "
        "    var t1 := Worker(1); "
        "    var t2 := Worker(2); "
        "    var t3 := Worker(3); "
        "    var t4 := Worker(4); "
        "    var t5 := Worker(5); "
        "    var t6 := Worker(6); "
        "    var t7 := Worker(7); "
        "    var t8 := Worker(8); "
        "    await t1; await t2; await t3; await t4; "
        "    await t5; await t6; await t7; await t8; "
        "    return C.counter; "
        "  end Main; end C;",
        8);  // 8 serially-applied increments: final counter = 8
}

// ---------------------------------------------------------------------------
// §30.13 (#618c) RUN-TO-COMPLETION — two writes within one continuation.
//
// Task A's post-await continuation writes step1 := 1, then step2 := 1 (no await
// between).  Task B's post-await continuation reads step1 and step2 and returns
// their sum.  Under serial-per-domain, by the time Task B runs, Task A has already
// completed its full continuation (both writes are visible).  The result is
// step1 + step2 = 2.
//
// Under a concurrent scheduler, Task B could run while Task A is between its two
// writes, observing step1=1, step2=0 → sum=1.  This doesn't happen under the
// serial-per-domain guarantee: a continuation runs to its next suspend point
// (or completion) without interruption.
//
// The test uses a longer Delay for Task B to ensure Task A's continuation runs
// first under the serial draining order (Task A's delay expires first).
// Run 3x + ASan + TSan.
// ---------------------------------------------------------------------------
BEZEL_TEST(async_618_domain_serial, run_to_completion_two_writes_one_continuation) {
    expectValueCrossThread618(
        "module M; class C "
        "  public static step1: int := 0; "
        "  public static step2: int := 0; "
        // Task A: short delay, then two writes in one continuation.
        "  public static async method TaskA(): Task "
        "    await Task.Delay(2); "
        // Both writes happen in this ONE continuation (no await between).
        "    C.step1 := 1; "
        "    C.step2 := 1; "
        "  end TaskA; "
        // Task B: longer delay, then reads both values.
        "  public static async method TaskB(): Task<int> "
        "    await Task.Delay(20); "
        "    return C.step1 + C.step2; "
        "  end TaskB; "
        "  public static async method Main(): Task<int> "
        "    var ta := TaskA(); "
        "    var tb := TaskB(); "
        "    await ta; "
        "    var result := await tb; "
        "    return result; "
        "  end Main; end C;",
        2);  // step1 + step2 = 1 + 1 = 2 (both writes complete before Task B reads)
}

// ---------------------------------------------------------------------------
// §30.13 (#618) BEHAVIOURAL SANITY — existing single-task async still works.
//
// Verify that the domain-based scheduler does not regress the basic already-done
// fast path and the cross-thread path for single-task programs.  This mirrors
// async_test.cpp's baseline cases but is re-run here after the #618 change to
// confirm the domain layer is transparent for single-domain single-task programs.
// ---------------------------------------------------------------------------
BEZEL_TEST(async_618_domain_serial, single_task_delay_still_works) {
    expectValueCrossThread618(
        "module M; class C "
        "  public static async method Main(): Task<int> "
        "    await Task.Delay(2); "
        "    return 42; "
        "  end Main; end C;",
        42);
}
