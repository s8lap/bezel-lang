// Codegen — async lambdas (§30.6, issue #488). An `async` lambda is the
// intersection of two existing machineries: a CLOSURE (§27.5 — a heap
// `{fn_ptr, env_ptr, rc}` cell whose body may capture outer variables) whose
// invocation drives an ASYNC STATE MACHINE (§30.1/§30.2 — the suspend/resume
// scaffolding of §29/§30 Phase 2a, parameterized for `await`). The lambda's
// function type carries a `Task` / `Task<T>` / `OwningTask<T>` return type
// (§30.6); calling it returns the outer Task, which the caller `await`s.
//
// These tests cannot use the plain JIT harness (the async emitter accumulates
// LLVM module state across runs — same constraint as iterator_test.cpp /
// async_test.cpp). They drive the REAL Driver pipeline to a native executable
// and check its exit code, mirroring async_test.cpp. A second leg links against
// an ASan-built runtime and runs under LeakSanitizer to verify the closure +
// frame + task reclaim is leak- and corruption-free on the synchronous
// completion path.
//
// SCOPE (what is exercised here — the SUPPORTED async-lambda subset): the
// synchronous / already-done cut, mirroring named async methods —
//   - an expression-bodied async lambda `async (x) => await P(x)` (§30.6 ex 2)
//   - a block-bodied async lambda `async (x) => do … return await …; end`
//     (§30.6 ex 1)
//   - an async lambda CAPTURING an outer local (the capture round-trips the
//     closure env AND the async frame, since it is live across the await)
//   - an async lambda passed to a method taking a function type `(T)->Task<U>`
//   - a no-value `Task` async lambda
//   - the awaited result feeding the caller (Pattern A `Task<int>`)

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

namespace {

namespace fs = std::filesystem;

// Per-process unique working dir, under the shared scratch base (never
// /tmp — see support/scratch_dir.hpp) and reaped at process exit.
static const std::filesystem::path& wtDirAsynclam() {
    return wtScratchDir("wt-asynclam");
}

// Compile `source` to a temp native executable via the real Driver and run it;
// returns the exit code, or -1 on compile/link/run failure.
int compileAndRun(const std::string& source) {
    auto dir = wtDirAsynclam();
    std::string stem = unique_stem("bezel_asynclam");
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

// LeakSanitizer verification (ASan exit 99 on any leak / UAF / double-free).
// Returns 0 (clean), 1 (sanitizer reported), or -1 (toolchain unavailable: skip).
int runUnderLeakSanitizer(const std::string& source) {
    if (std::system("clang++ --version > /dev/null 2>&1") != 0) return -1;

    auto dir = wtDirAsynclam();
    std::string stem = unique_stem("bezel_asynclam_lsan");
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
        if (!bezel::codegen::emitObjectFile(*module, objPath, err))
            return bezel::test::asanBuildFailure("emitObjectFile");
    }

    auto cleanup = [&] {
        std::remove(objPath.c_str());
        std::remove(rtObj.c_str());
        std::remove(exe.c_str());
    };
    if (!::bezel::native_test::buildRuntimeSanitizerObject("address", rtObj)) {
        cleanup();
        return -1;
    }
    std::string linkCmd =
        "clang++ -fsanitize=address -g " + objPath + " " + rtObj +
        " -lm -lpthread -lstdc++ -o " + exe + " > /dev/null 2>&1";
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
    return 0;                                                      // clean
}

// Assert correctness (exit code) AND, when the toolchain is present, that LSan
// reports no leak / UAF / double-free on the synchronous completion path. Used
// for the async-lambda shapes whose every allocation (the closure cell + env,
// the outer Task, the heap frame) is reclaimed — i.e. the lambda is stored in a
// local (whose scope-end closure-release frees the cell+env) and does not
// capture an outer variable.
void expectValueLeakFree(const std::string& source, int expectedExitCode) {
    BEZEL_EXPECT_EQ(compileAndRun(source), expectedExitCode);
    int lsan = runUnderLeakSanitizer(source);
    if (lsan == -1) return;       // toolchain unavailable: skip the LSan leg
    BEZEL_EXPECT_EQ(lsan, 0);     // 0 == LSan clean
}

// Assert exit-code correctness only. Used for the async-lambda shapes that trip
// a PRE-EXISTING closure leak SHARED WITH SYNCHRONOUS closures and orthogonal to
// the async state machine (verified on main HEAD against the same shapes):
//   (a) a CAPTURING closure leaks its §27.5 captured-variable shared cell — a
//       plain `var f := () => seed + 5` in a synchronous method leaks it too;
//   (b) an inline lambda passed directly as a function ARGUMENT leaks its
//       transient closure cell — `Apply((x) => x*10, 5)` in a synchronous
//       method leaks it too (only LINQ argument paths release the transient).
// The async-lambda machinery itself (task + frame + closure cell/env) is
// leak-clean here; these residual leaks are tracked separately, not by #488.
void expectValue(const std::string& source, int expectedExitCode) {
    BEZEL_EXPECT_EQ(compileAndRun(source), expectedExitCode);
}

}  // namespace

// §30.6 (example 2): an EXPRESSION-bodied async lambda — `async (url) => await
// CountBytesAsync(url)`. The lambda is stored in a `(int) -> Task<int>` local,
// invoked, and the result awaited. Producer(3) → 30; +1 = 31.
BEZEL_TEST(async_lambda_30_6, expr_body_awaited) {
    expectValueLeakFree(
        "module M; class C "
        "  public static async method Producer(n: int): Task<int> "
        "    return n * 10; "
        "  end Producer; "
        "  public static async method Main(): Task<int> "
        "    var f: (int) -> Task<int> := async lambda(x) => await Producer(x); "
        "    using var t := f(3); "
        "    var r := await t; "
        "    return r + 1; "
        "  end Main; end C;",
        31);
}

// §30.6 (example 1): a BLOCK-bodied async lambda — `async (x) => do … return
// await …; end`. Producer(4) → 40; +2 = 42.
BEZEL_TEST(async_lambda_30_6, block_body_awaited) {
    expectValueLeakFree(
        "module M; class C "
        "  public static async method Producer(n: int): Task<int> "
        "    return n * 10; "
        "  end Producer; "
        "  public static async method Main(): Task<int> "
        "    var f: (int) -> Task<int> := async lambda(x) => do "
        "      var p := await Producer(x); "
        "      return p + 2; "
        "    end; "
        "    using var t := f(4); "
        "    return await t; "
        "  end Main; end C;",
        42);
}

// §30.6 / §27.5: an async lambda that CAPTURES an outer local. `seed` (100) is
// captured by the closure env; because it is read AFTER the await it must also
// round-trip the async frame. Producer(2) → 20; 100 + 20 = 120. (exit-code only:
// the captured-variable cell leak is a pre-existing §27.5 issue shared with
// synchronous capturing closures — see expectValue's note.)
BEZEL_TEST(async_lambda_30_6, captures_outer_local) {
    expectValue(
        "module M; class C "
        "  public static async method Producer(n: int): Task<int> "
        "    return n * 10; "
        "  end Producer; "
        "  public static async method Main(): Task<int> "
        "    var seed := 100; "
        "    var f: (int) -> Task<int> := async lambda(x) => do "
        "      var d := await Producer(x); "
        "      return seed + d; "
        "    end; "
        "    using var t := f(2); "
        "    return await t; "
        "  end Main; end C;",
        120);
}

// §30.6: an async lambda passed to a method taking a function type
// `(int) -> Task<int>`. The method invokes it and awaits the result.
// Producer(5) → 50; +3 = 53. (exit-code only: the inline-lambda-argument
// transient closure leak is pre-existing and shared with synchronous lambdas
// passed as arguments — see expectValue's note.)
BEZEL_TEST(async_lambda_30_6, passed_as_function_argument) {
    expectValue(
        "module M; class C "
        "  public static async method Producer(n: int): Task<int> "
        "    return n * 10; "
        "  end Producer; "
        "  public static async method Apply(g: (int) -> Task<int>, n: int): Task<int> "
        "    using var t := g(n); "
        "    var r := await t; "
        "    return r + 3; "
        "  end Apply; "
        "  public static async method Main(): Task<int> "
        "    return await Apply(async lambda(x) => await Producer(x), 5); "
        "  end Main; end C;",
        53);
}

// §30.6: a no-value `Task` async lambda. The lambda awaits a no-value producer
// and completes; the caller awaits it and proceeds. The 7 proves control flowed
// through the lambda's no-value await and back.
BEZEL_TEST(async_lambda_30_6, no_value_task_lambda) {
    expectValueLeakFree(
        "module M; class C "
        "  public static async method DoWork(): Task "
        "    return; "
        "  end DoWork; "
        "  public static async method Main(): Task<int> "
        "    var f: () -> Task := async lambda() => await DoWork(); "
        "    using var t := f(); "
        "    await t; "
        "    return 7; "
        "  end Main; end C;",
        7);
}

// §30.6: an async lambda with NO captures and NO await (synchronous body)
// returning Task<int>. The lambda's body completes inline; the caller awaits the
// already-done task. 42.
BEZEL_TEST(async_lambda_30_6, synchronous_body_no_await) {
    expectValueLeakFree(
        "module M; class C "
        "  public static async method Main(): Task<int> "
        "    var f: () -> Task<int> := async lambda() => 42; "
        "    using var t := f(); "
        "    return await t; "
        "  end Main; end C;",
        42);
}
