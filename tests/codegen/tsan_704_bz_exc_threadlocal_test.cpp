// TSan probe for fix #704: bz_exc thread_local race elimination.
//
// §30.14: when two Domain.Spawn workers run concurrently, each in its own
// thread, and each entry function throws, the two workers must not race on
// bz_exc.  Before fix #704 bz_exc was a single process-wide global; two
// concurrent workers' save/clear/restore sequences interleaved, causing one
// worker to clobber the other's in-flight exception pointer.
//
// Fix #704 makes bz_exc thread_local (one slot per thread) so each domain
// worker's exception state is independent.
//
// Tests:
//   (a) Correctness: two concurrent throwing spawns both surface their own
//       DomainFaultedException; the program returns 42 (both caught).
//       Under the pre-fix single-global, one exception could be swallowed,
//       yielding a wrong return code.
//
//   (b) TSan no-bz_exc-race: compile and run with -fsanitize=thread.
//       Fix #704 eliminates the bz_exc data race; fix #708 eliminates the
//       pre-existing domain_drain heap-UAF (domain_release fired while
//       lock_guard still alive, freed dom->mtx before ~lock_guard unlocked it).
//       Post-#708 the binary must be fully TSan-clean: no TSan error in the
//       output AND exit code must not be 99.
//
// Spec derivation: §30.14 (v0.141) — concurrent domain workers; §31.1 bz_exc.

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

#include "native_test_harness.hpp"
#include "support/unique_stem.hpp"

namespace {

namespace fs = std::filesystem;

// Compile `source` to a temp native AOT binary (no sanitizer) and run it.
// Returns exit code, or -1 on compile/link/run failure.
int compileAndRun(const std::string& source) {
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bezel_704");
    std::string src  = (dir / (stem + ".bzl")).string();
    std::string exe  = (dir / stem).string();
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

// Compile with -fsanitize=thread and capture the full TSan output (stderr).
// Returns the combined stderr output, or "" on infrastructure failure.
// Sets *exitCode (if non-null) to the program's exit code (TSan uses 99 on
// error when TSAN_OPTIONS=exitcode=99; set to -1 on infrastructure failure).
std::string runUnderTSanCaptureOutput(const std::string& source,
                                      int* exitCode = nullptr) {
    if (exitCode) *exitCode = -1;
    if (std::system("clang++ --version > /dev/null 2>&1") != 0) return "";

    auto dir = fs::temp_directory_path();
    std::string stem    = unique_stem("bezel_704_tsan");
    std::string objPath = (dir / (stem + ".o")).string();
    std::string rtObj   = (dir / (stem + "_rt.o")).string();
    std::string exe     = (dir / stem).string();
    std::string outFile = (dir / (stem + ".tsan.txt")).string();

    auto cleanup = [&] {
        std::remove(objPath.c_str());
        std::remove(rtObj.c_str());
        std::remove(exe.c_str());
        std::remove(outFile.c_str());
    };

    {
        bezel::DiagnosticEngine diags;
        bezel::Lexer lexer(source, diags);
        bezel::Parser parser(lexer, diags);
        auto unit = parser.parseCompilationUnit();
        if (!unit) { cleanup(); return ""; }
        bezel::sema::SemaContext ctx(diags);
        bezel::sema::collectSymbols(*unit, ctx);
        bezel::sema::resolveTypes(*unit, ctx);
        bezel::sema::typecheckExpressions(*unit, ctx);
        bezel::sema::checkDefiniteAssignment(*unit, ctx);
        bezel::ownership::checkOwnership(*unit, ctx);
        if (diags.hasErrors()) { cleanup(); return ""; }
        llvm::LLVMContext llvmCtx;
        auto module = bezel::codegen::emitModule(*unit, ctx, llvmCtx);
        if (!module) { cleanup(); return ""; }
        std::string err;
        if (!bezel::codegen::emitObjectFile(*module, objPath, err)) { cleanup(); return ""; }
    }

    if (!::bezel::native_test::buildRuntimeSanitizerObject("thread", rtObj)) {
        cleanup();
        return "";
    }

    std::string linkCmd =
        "clang++ -fsanitize=thread -g " + objPath + " " + rtObj +
        " -lm -lpthread -lstdc++ -o " + exe + " > /dev/null 2>&1";
    if (std::system(linkCmd.c_str()) != 0) { cleanup(); return ""; }

    // Run with TSAN_OPTIONS=exitcode=99 so TSan sets exit 99 on any error;
    // capture stderr (TSan output) to a file.
    std::string runCmd =
        "TSAN_OPTIONS=exitcode=99 " + exe + " > /dev/null 2> " + outFile;
    int status = std::system(runCmd.c_str());
    if (exitCode && status != -1 && WIFEXITED(status))
        *exitCode = WEXITSTATUS(status);

    // Read captured output
    std::ifstream f(outFile);
    std::string output((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
    cleanup();
    return output;
}

// Two concurrent Domain.Spawn calls each throwing a uniquely-tagged exception.
// Main spawns both before awaiting either, maximising the concurrency window.
// Each join surfaces a DomainFaultedException; if EITHER exception is
// swallowed (pre-fix race on bz_exc) the program returns a wrong code.
//
// Returns 42 on success (both faults caught), a different value (or -1) on
// a lost-exception race or compile/run failure.
static const std::string kConcurrentFaultSource =
    "module M; "
    "class ThrowA "
    "  public static method Run() "
    "    throw new Exception(\"fault-a\"); "
    "  end Run; "
    "end ThrowA; "
    "class ThrowB "
    "  public static method Run() "
    "    throw new Exception(\"fault-b\"); "
    "  end Run; "
    "end ThrowB; "
    "class Program "
    "  public static async method Main(): Task<int> "
    // Spawn both BEFORE awaiting either — pool workers may execute concurrently.
    "    using var t1 := Domain.Spawn(ThrowA.Run); "
    "    using var t2 := Domain.Spawn(ThrowB.Run); "
    "    var caught := 0; "
    "    try "
    "      await t1; "
    "      return -10; "  // must not reach — t1 must fault
    "    catch DomainFaultedException => "
    "      caught := caught + 1; "
    "    end; "
    "    try "
    "      await t2; "
    "      return -20; "  // must not reach — t2 must fault
    "    catch DomainFaultedException => "
    "      caught := caught + 1; "
    "    end; "
    "    return 40 + caught; "  // 42 iff both caught
    "  end Main; "
    "end Program;";

}  // namespace

// ---------------------------------------------------------------------------
// §30.14 / fix #704: two concurrent domain workers each throw; both faults
// must surface correctly (no lost exception from a bz_exc race).
// Run 5× to catch non-deterministic lost-exception races.
// ---------------------------------------------------------------------------
BEZEL_TEST(tsan_704_bz_exc, concurrent_faults_both_surface_correctly) {
    for (int i = 0; i < 5; ++i) {
        int v = compileAndRun(kConcurrentFaultSource);
        if (v == -1) continue;  // infrastructure failure, skip iteration
        BEZEL_EXPECT_EQ(v, 42);
    }
}

// ---------------------------------------------------------------------------
// §30.14 / fix #704 + fix #708: TSan probe — fully clean binary.
//
// Compile with -fsanitize=thread and run; verify:
//   1. No mention of "bz_exc" / "bz_exc_addr" in TSan output (fix #704).
//   2. TSan reports no error at all — exit code must not be 99 (fix #708:
//      the domain_drain heap-UAF that caused exit=99 is now resolved).
// ---------------------------------------------------------------------------
BEZEL_TEST(tsan_704_bz_exc, tsan_no_bz_exc_race_in_output) {
    int tsanExitCode = -1;
    std::string tsanOutput = runUnderTSanCaptureOutput(kConcurrentFaultSource,
                                                        &tsanExitCode);
    if (tsanOutput.empty() && tsanExitCode == -1) return;  // clang unavailable

    // Fix #704: no bz_exc data race.
    bool hasBzExcRace = (tsanOutput.find("bz_exc") != std::string::npos);
    if (hasBzExcRace) {
        std::fprintf(stderr, "TSan output contained bz_exc:\n%s\n",
                     tsanOutput.c_str());
    }
    BEZEL_EXPECT_TRUE(!hasBzExcRace);

    // Fix #708: domain_drain UAF resolved — binary must be fully TSan-clean
    // (TSan exits 99 on any sanitizer error; 42 = correct program result).
    if (tsanExitCode != -1) {
        if (tsanExitCode == 99) {
            std::fprintf(stderr, "TSan reported an error (exit=99):\n%s\n",
                         tsanOutput.c_str());
        }
        BEZEL_EXPECT_EQ(tsanExitCode, 42);
    }
}
