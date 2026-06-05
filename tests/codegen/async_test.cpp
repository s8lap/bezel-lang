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
// SCOPE of this increment (what is exercised here): the synchronous / already-
// done cut — `await SomeAsyncCall()` where the producer completes inline. The
// supported shapes are straight-line sequential awaits, awaits in if/else
// branches, locals live across an await, `Task` (no value) awaits, and basic
// fault propagation. DEFERRED (not exercised; rejected with a diagnostic or out
// of scope): `await` inside a loop body (for/while), `await` inside try/catch/
// finally, a real cross-thread suspend, atomic refcounts, and async iterators.

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

// LeakSanitizer verification for a single program. Returns:
//   0  — ran clean: no leak, no use-after-free / double-free.
//   1  — sanitizer reported a problem: test failure.
//  -1  — toolchain unavailable / build step failed: SKIP (not a failure).
int runUnderLeakSanitizer(const std::string& source) {
    if (std::system("clang++ --version > /dev/null 2>&1") != 0) return -1;

    static int counter = 0;
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_async_lsan_" + std::to_string(counter++);
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
        "clang++ -std=c++20 -g -fsanitize=address -pthread -c " +
        runtimeSourcePath() + " -o " + rtObj + " > /dev/null 2>&1";
    if (std::system(rtCmd.c_str()) != 0) { cleanup(); return -1; }

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
    return 0;                                                      // clean
}

// Assert correctness (exit code) AND, when the toolchain is present, that LSan
// reports no leak / UAF / double-free on the synchronous completion path.
void expectValueLeakFree(const std::string& source, int expectedExitCode) {
    BEZEL_EXPECT_EQ(compileAndRun(source), expectedExitCode);
    int lsan = runUnderLeakSanitizer(source);
    if (lsan == -1) return;       // toolchain unavailable: skip the LSan leg
    BEZEL_EXPECT_EQ(lsan, 0);     // 0 == LSan clean
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

// §30.2 deferred tail: `await` inside a loop body is NOT yet supported (the flat
// resume model breaks SSA dominance across the loop back-edge). It must be
// rejected with a clear diagnostic — and crucially must NOT emit malformed IR.
// This pins the increment boundary.
BEZEL_TEST(async_30_2, await_in_loop_is_rejected) {
    BEZEL_EXPECT_TRUE(compileFails(
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
        "  end Main; end C;"));
}
