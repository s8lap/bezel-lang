// Codegen — §30.14 / stdlib §6.8 (#1491): a non-capturing lambda spawn entry
// with a VALUE result must deliver that result through the join.
//
// Regression for #1491: `Domain.Spawn(() => 42)` resolves to
// `Spawn<R>(entry: () -> R): owned Task<R>` with R = int (stdlib §6.8, Pattern A
// value/string result — Bezel_Standard_Library.md §6.8). Awaiting the join must
// yield the entry's actual result (42), not 0.
//
// Root cause (fixed): the codegen spawn adapter for a *lambda* entry (as opposed
// to a static method reference) invoked the lambda but discarded its return value
// and always returned null to bz_domain_spawn, so the join task completed with a
// null/0 result. The static-method-reference adapter already marshalled the value
// result via inttoptr(zext(v)); the lambda adapter now does the same. A silent
// wrong value on the flagship parallelism API (§30.14) is worse than a rejection,
// hence a run test that asserts the concrete joined value.
//
// These run through the in-process JIT (bezel::test::runJit) — the load-bearing
// correctness check — and additionally through an ASan-linked native binary.
// The exit code is the int returned by `Main(): Task<int>`.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "native_test_harness.hpp"
#include "test_harness.hpp"

#include <filesystem>
#include <cstdlib>
#include <sys/wait.h>

#include "lexer/Lexer.hpp"
#include "ownership/Ownership.hpp"
#include "parser/Parser.hpp"
#include "sema/DefiniteAssignment.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/SymbolCollection.hpp"
#include "sema/TypeResolution.hpp"
#include "support/Diagnostic.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

#include "codegen/LlvmBackend.hpp"

namespace fs = std::filesystem;
using bezel::test::runAot;

namespace {

// Per-process unique working dir, under the shared scratch base (never
// /tmp — see support/scratch_dir.hpp) and reaped at process exit.
static const std::filesystem::path& wtDir1491() {
    return wtScratchDir("wt-1491");
}

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// Compile, link with ASan, run.  Returns 0 (clean), 1 (sanitizer), -1 (skip).
// Mirrors tests/codegen/shared_sync_689_contention_test.cpp::runUnderASan.
int runUnderASan(const std::string& source) {
    const std::string& rtObj = ::bezel::native_test::asanRuntimeObject();
    if (rtObj.empty()) return -1;

    auto dir = wtDir1491();
    std::string stem = unique_stem("bezel_1491_asan");
    std::string objPath = (dir / (stem + ".o")).string();
    std::string exe     = (dir / stem).string();

    auto cleanup = [&] {
        std::remove(objPath.c_str());
        std::remove(exe.c_str());
    };

    {
        bezel::DiagnosticEngine diags;
        bezel::Lexer lexer(source, diags);
        bezel::Parser parser(lexer, diags);
        auto unit = parser.parseCompilationUnit();
        if (!unit) return -2;
        bezel::sema::SemaContext ctx(diags);
        bezel::sema::collectSymbols(*unit, ctx);
        bezel::sema::resolveTypes(*unit, ctx);
        bezel::sema::typecheckExpressions(*unit, ctx);
        bezel::sema::checkDefiniteAssignment(*unit, ctx);
        bezel::ownership::checkOwnership(*unit, ctx);
        if (diags.hasErrors()) return -2;
        llvm::LLVMContext llvmCtx;
        auto module = bezel::codegen::emitModule(*unit, ctx, llvmCtx);
        if (!module) return -2;
        std::string err;
        if (!bezel::codegen::emitObjectFile(*module, objPath, err)) return -2;
    }

    std::string linkCmd = "clang++ -fsanitize=address -g " + objPath + " " +
                          rtObj + " -lm -lpthread -lstdc++ -o " + exe +
                          " > /dev/null 2>&1";
    if (std::system(linkCmd.c_str()) != 0) { cleanup(); return -3; }

    // detect_leaks=1: also run LSan on top of ASan. exitcode=99 makes a
    // sanitizer finding distinguishable from the program's own exit code.
    std::string runCmd =
        "ASAN_OPTIONS=detect_leaks=1:exitcode=99 " + exe + " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return -4;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 99) return 1;
    return 0;
}

// JIT logic check (load-bearing) + ASan memory check.
void expectCleanAndEq(const std::string& src, int expectedExit) {
    BEZEL_EXPECT_EQ(run(src), expectedExit);
    int san = runUnderASan(src);
    if (san != -1) BEZEL_EXPECT_EQ(san, 0);  // 0 = ASan/LSan clean
}

}  // namespace

// ──────────────────────────────────────────────────────────────────────────────
// #1491: int-returning no-arg lambda entry — the headline miscompile.
// `() => 42` : () -> int → Spawn<R>(() -> R): owned Task<R>, R = int.
// await must yield 42 (was 0 before the fix). stdlib §6.8 / §30.14.
// ──────────────────────────────────────────────────────────────────────────────
BEZEL_TEST(domain_spawn_lambda_result_1491, int_result_flows_through_join) {
    std::string src =
        "module M; "
        "import System.Threading; "
        "class Program "
        "  public static async method Main(): Task<int> "
        "    using var t := Domain.Spawn(lambda() => 42); "
        "    var v := await t; "
        "    return v; "        // exit == joined result; must be 42, not 0.
        "  end Main; "
        "end Program;";
    expectCleanAndEq(src, 42);
}

// A different sendable value type (bool → Task<bool>, Pattern A). true must
// cross the boundary as true. stdlib §6.8 / §30.14.
BEZEL_TEST(domain_spawn_lambda_result_1491, bool_result_flows_through_join) {
    std::string src =
        "module M; "
        "import System.Threading; "
        "class Program "
        "  public static async method Main(): Task<int> "
        "    using var t := Domain.Spawn(lambda() => true); "
        "    var b := await t; "
        "    if b then return 7; else return 0; end; "
        "  end Main; "
        "end Program;";
    expectCleanAndEq(src, 7);
}

// A small int value (7) — distinct from 42 to catch a stuck/constant result.
BEZEL_TEST(domain_spawn_lambda_result_1491, small_int_result_flows_through_join) {
    std::string src =
        "module M; "
        "import System.Threading; "
        "class Program "
        "  public static async method Main(): Task<int> "
        "    using var t := Domain.Spawn(lambda() => 7); "
        "    var v := await t; "
        "    return v; "
        "  end Main; "
        "end Program;";
    expectCleanAndEq(src, 7);
}

// The unit `() -> ()` entry form must still work: the entry runs (visible through
// a process-global static set on the join's happens-before edge) and the join
// completes. Guards against a regression from the value-marshalling change.
// stdlib §6.8 (`Spawn(entry: () -> ()): owned Task`) / §30.14.
BEZEL_TEST(domain_spawn_lambda_result_1491, unit_entry_still_runs_and_joins) {
    std::string src =
        "module M; "
        "import System.Threading; "
        "class Counter "
        "  public static hits: int := 0; "
        "end Counter; "
        "class Program "
        "  public static async method Main(): Task<int> "
        "    using var t := Domain.Spawn(lambda() => do Counter.hits := 5; end); "
        "    await t; "
        "    return Counter.hits; "  // 5 iff the unit entry ran before the join.
        "  end Main; "
        "end Program;";
    expectCleanAndEq(src, 5);
}
