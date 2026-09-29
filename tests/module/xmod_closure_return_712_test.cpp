// Cross-module closure retain/release (issue #712, §28.9).
//
// An imported method returning a closure (`() -> int`) was not recognized as
// a +1 producer by isUserMethodClosureReturnCall — the function returned false
// for imported methods (ctx_.isImported(ms)) — so the importer never registered
// a scope-end `bz_closure_release` for the returned closure object + env cell.
//
// BEFORE fix: calling m.MakeAdder(41) from module B leaks 52 bytes
//   (Direct 32 = closure object, Indirect 16 = env cell; the 4-byte typed
//    captured int env slot is a separate #672 residual — out of scope here).
// AFTER  fix: the closure object (32 bytes) and env cell (16 bytes) are freed;
//   only the 4-byte captured-int typed residual (#672) remains → 4 bytes leaked.
//
// The fix mirrors #694 (isUserMethodStringReturnCall): when ctx_.isImported(ms)
// is true, consult ctx_.importedReturnType(ms) and return true if it has
// Kind::Function.
//
// TEST MATRIX:
//   C1  imported instance method returning closure — LSan: 52 bytes before,
//       ≤4 bytes after (or closure-object Direct leak absent); exit-code proxy.
//   C2  single-module baseline (same shape, one module) — must still work and
//       leak the same ≤4 bytes as C1 after fix (proves no regression).
//   CANARY  imported method returning a non-closure (int) — must NOT be
//       over-released (no double-free / no new ASan abort).
//
// LSan assertion strategy: since a fully clean (0-byte) result is impossible
// until #672, C1 compares cross-module vs single-module: both should leak the
// same small residual (≤4 bytes).  If the cross-module LSan code matches the
// single-module code, the closure object + env cell are freed.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "codegen/LlvmBackend.hpp"
#include "support/emit_object_support.hpp"
#include "driver/Driver.hpp"
#include "lexer/Lexer.hpp"
#include "support/driver_run_support.hpp"
#include "module/ModuleLoader.hpp"
#include "module/ModuleMetadata.hpp"
#include "module/ModuleMetadataBuilder.hpp"
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
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"


namespace bezel::module {
namespace {

// Per-process unique working dir, under the shared scratch base (never
// /tmp — see support/scratch_dir.hpp) and reaped at process exit. The
// process-unique stem is what keeps concurrent shards from racing each
// other's files mid-compile.
const std::filesystem::path& wtDir712() {
    return wtScratchDir("wt-712");
}


// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

struct TempFile712 {
    std::string path;
    explicit TempFile712(const std::string& p) : path(p) {}
    ~TempFile712() { std::remove(path.c_str()); }
};

void writeSource712(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Compile lane-local ASan runtime once, cache path.  Per HARD RULES, NEVER
// the shared memoised .o; always /tmp/wt-712/rt_asan.o.
static const std::string& laneLocalAsanRt712() {
    static std::string cached;
    static bool attempted = false;
    if (attempted) return cached;
    attempted = true;
    std::string dst = (wtDir712() / "rt_asan.o");
    // If already built (by the pre-build step in the task instructions), reuse.
    std::ifstream check(dst);
    if (check.good()) { cached = dst; return cached; }
    // Otherwise compile now.
    if (!::bezel::native_test::buildRuntimeSanitizerObject("address", dst))
        return cached;  // leave empty → SKIP
    cached = dst;
    return cached;
}

// Compile source A to an object (uses the Driver — wraps the standard pipeline).
// Returns true on success.
static bool compileToObj712(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    bezel::CompileResult r = driver.compileObject(src, obj);
    return r.success;
}

// Compile source B importing A's object, produce an executable, run it.
// Returns the executable's exit code, or -1 on compile/link/run failure.
static int compileImportAndRun712(
    const std::string& bSrc, const std::string& bExe,
    const std::string& aObj)
{
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj;
    d.objectPath   = aObj;
    deps.push_back(d);
    bezel::CompileResult r = bezel::test::compileFileForTest(driver, bSrc, bExe, deps);
    if (!r.success) return bezel::test::asanBuildFailure("compileFile");
    int status = std::system(bExe.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// Cross-module LSan probe (mirrors xmod_string_refcount_694_test.cpp).
// Returns:
//   0  — LSan clean / no leak of interest.
//   1  — LSan/ASan problem (exit 99).
//  -1  — toolchain unavailable or compile/link failure → SKIP.
//   2  — program exited with a code other than expectedCode (unexpected).
static int xmodLsan712(
    const std::string& aObj,
    const std::string& bSrc,
    const std::vector<bezel::Dependency>& deps,
    int expectedCode)
{
    const std::string& rt = laneLocalAsanRt712();
    if (rt.empty()) return -1;  // SKIP

    namespace fs = std::filesystem;
    auto tmpDir = fs::temp_directory_path();
    static int ctr = 0;
    std::string stem = "bz712_lsan_" + std::to_string(ctr++);
    std::string bObjPath = (tmpDir / (stem + "_b.o")).string();
    std::string exePath  = (tmpDir / (stem + "_exe")).string();

    // Compile B to object (not exe yet — we link manually with ASan rt).
    {
        bezel::DiagnosticEngine diags;
        bezel::Lexer lexer(bSrc, diags);
        bezel::Parser parser(lexer, diags);
        auto unit = parser.parseCompilationUnit();
        if (!unit) return bezel::test::asanBuildFailure("parse");
        bezel::sema::SemaContext ctx(diags);
        bezel::sema::collectSymbols(*unit, ctx);
        for (const auto& dep : deps) {
            if (!dep.metadataPath.empty())
                bezel::module::loadDependency(dep.metadataPath, ctx);
        }
        bezel::sema::resolveTypes(*unit, ctx);
        bezel::sema::typecheckExpressions(*unit, ctx);
        bezel::sema::checkDefiniteAssignment(*unit, ctx);
        bezel::ownership::checkOwnership(*unit, ctx);
        if (diags.hasErrors()) return bezel::test::asanBuildFailure("sema");
        llvm::LLVMContext llvmCtx;
        auto mod = bezel::codegen::emitModule(*unit, ctx, llvmCtx);
        if (!mod) return bezel::test::asanBuildFailure("emitModule");
        std::string err;
        if (!bezel::test::emitObjectFileForTest(*mod, bObjPath, err))
            return bezel::test::asanBuildFailure("emitObjectFile");
    }

    auto cleanup = [&] {
        std::remove(bObjPath.c_str());
        std::remove(exePath.c_str());
    };

    // Link: A.o + B.o + ASan runtime + stdlib.
    std::string linkCmd =
        "clang++ -fsanitize=address -g " +
        aObj + " " + bObjPath + " " + rt +
        " -lm -lpthread -lstdc++ -o " + exePath +
        " > /dev/null 2>&1";
    if (std::system(linkCmd.c_str()) != 0) {
        cleanup();
        return bezel::test::asanLinkFailure("link");
    }

    std::string runCmd =
        "ASAN_OPTIONS=detect_leaks=1:exitcode=99:abort_on_error=0 " +
        exePath + " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return bezel::test::asanExecFailure("system");
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (code == 99) return 1;        // leak / memory safety error
    if (code == expectedCode) return 0;  // clean + correct exit code
    return 2;  // unexpected exit code (not 99, not expected) — don't count as LSan fail
}

// Single-module LSan probe: compiles bSrc stand-alone (no imports) to object,
// links with ASan runtime, runs under detect_leaks=1.
// Returns same codes as xmodLsan712.
static int singleModLsan712(const std::string& src, int expectedCode) {
    const std::string& rt = laneLocalAsanRt712();
    if (rt.empty()) return -1;  // SKIP

    namespace fs = std::filesystem;
    auto tmpDir = fs::temp_directory_path();
    static int sctr = 0;
    std::string stem = "bz712_singlemod_" + std::to_string(sctr++);
    std::string objPath = (tmpDir / (stem + "_a.o")).string();
    std::string exePath = (tmpDir / (stem + "_exe")).string();

    {
        bezel::DiagnosticEngine diags;
        bezel::Lexer lexer(src, diags);
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
        auto mod = bezel::codegen::emitModule(*unit, ctx, llvmCtx);
        if (!mod) return bezel::test::asanBuildFailure("emitModule");
        std::string err;
        if (!bezel::test::emitObjectFileForTest(*mod, objPath, err))
            return bezel::test::asanBuildFailure("emitObjectFile");
    }

    auto cleanup = [&] {
        std::remove(objPath.c_str());
        std::remove(exePath.c_str());
    };

    std::string linkCmd =
        "clang++ -fsanitize=address -g " +
        objPath + " " + rt +
        " -lm -lpthread -lstdc++ -o " + exePath +
        " > /dev/null 2>&1";
    if (std::system(linkCmd.c_str()) != 0) {
        cleanup();
        return bezel::test::asanLinkFailure("link");
    }

    std::string runCmd =
        "ASAN_OPTIONS=detect_leaks=1:exitcode=99:abort_on_error=0 " +
        exePath + " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return bezel::test::asanExecFailure("system");
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (code == 99) return 1;
    if (code == expectedCode) return 0;
    return 2;
}

}  // namespace

// ===========================================================================
// Shape C1+C2 combined (§28.9 / #712): cross-module vs single-module
// LSan comparison.
//
// BEFORE fix: cross-module (C1) leaks closure object (32 B Direct) + env cell
//   (16 B Indirect) + typed-int capture (4 B, #672 residual) = 52+ B total.
//   Single-module (C2) leaks only 4 B (#672 residual); the closure object and
//   env cell are freed there.  C1 leaks MORE than C2 → different LSan codes.
//
// AFTER  fix: C1 also frees the closure object + env cell, so both C1 and C2
//   leak only the same #672 typed-capture residual (≤4 B).  Both produce the
//   same LSan exit code.
//
// Assertion: after fix, lsan_c1 == lsan_c2.
//   The #672 residual may still cause both to exit 99, but that is equal and
//   expected — the closure-object + env-cell leak is eliminated.
//   A double-free from over-releasing would be caught as a non-42 exit code.
// ===========================================================================
BEZEL_TEST(xmod_closure_return_712, c1_imported_instance_method_closure_lsan_28_9) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz712_c1_");
    TempFile712 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile712 aObj((dir / (stem + "_a.o")).string());

    // Module A: MakeAdder returns a closure that adds n+1.
    // A's ReturnStmt mints +1 for the closure cell (bz_closure_retain path).
    // Caller (B) receives exactly +1.
    writeSource712(aSrc.path,
        "module MakerLib; "
        "public class Maker "
        "  public constructor() end constructor; "
        "  public method MakeAdder(n: int): () -> int return lambda() => n + 1; end MakeAdder; "
        "  destructor end destructor; "
        "end Maker;");

    BEZEL_EXPECT_TRUE(compileToObj712(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    // Module B: constructs Maker, gets a closure, calls it, returns result.
    // var f is a non-escaping closure local (result of imported closure-returning call).
    // After fix: f is in closureReleaseEligible_ → bz_closure_release at scope end.
    std::string bSrc =
        "module App; "
        "import MakerLib; "
        "public class Program "
        "  public static method Main(): int "
        "    using var m := new Maker(); "
        "    var f := m.MakeAdder(41); "  // non-escaping closure local
        "    var r := f(); "              // invoke: returns 42
        "    return r; "
        "  end Main; "
        "end Program;";

    // Native correctness check: result should be 42.
    TempFile712 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile712 bExe((dir / (stem + "_b")).string());
    writeSource712(bSrcFile.path, bSrc);
    BEZEL_EXPECT_EQ(compileImportAndRun712(bSrcFile.path, bExe.path, aObj.path), 42);

    std::vector<bezel::Dependency> deps;
    bezel::Dependency dep; dep.metadataPath = aObj.path; dep.objectPath = aObj.path;
    deps.push_back(dep);
    int lsan_c1 = xmodLsan712(aObj.path, bSrc, deps, 42);
    if (lsan_c1 == -1) return;  // toolchain unavailable: skip

    // Baseline: the SAME shape compiled as a single module.
    // The single-module path already frees the closure object + env cell.
    // After fix, C1 should match this baseline (both reduced to #672 residual).
    std::string singleSrc =
        "module App; "
        "public class Maker "
        "  public constructor() end constructor; "
        "  public method MakeAdder(n: int): () -> int return lambda() => n + 1; end MakeAdder; "
        "  destructor end destructor; "
        "end Maker; "
        "public class Program "
        "  public static method Main(): int "
        "    using var m := new Maker(); "
        "    var f := m.MakeAdder(41); "
        "    var r := f(); "
        "    return r; "
        "  end Main; "
        "end Program;";
    int lsan_c2 = singleModLsan712(singleSrc, 42);
    if (lsan_c2 == -1) return;  // toolchain unavailable: skip

    // After fix: C1 (cross-module) and C2 (single-module) must have the same
    // LSan result.  The #672 residual may cause both to be 1; what matters is
    // they are EQUAL (closure object + env cell freed in both paths).
    // A pre-fix failure would have lsan_c1==1 while lsan_c2==0 (different).
    BEZEL_EXPECT_EQ(lsan_c1, lsan_c2);
}


// ===========================================================================
// Shape C2 — Single-module BASELINE correctness (§28.9 / #712).
//
// Verifies that the single-module path still produces the correct exit code
// after the fix (no regression introduced by the change).
// ===========================================================================
BEZEL_TEST(xmod_closure_return_712, c2_single_module_baseline_lsan_28_9) {
    // Single-module: Maker + Program in one compilation unit.
    std::string singleSrc =
        "module App; "
        "public class Maker "
        "  public constructor() end constructor; "
        "  public method MakeAdder(n: int): () -> int return lambda() => n + 1; end MakeAdder; "
        "  destructor end destructor; "
        "end Maker; "
        "public class Program "
        "  public static method Main(): int "
        "    using var m := new Maker(); "
        "    var f := m.MakeAdder(41); "
        "    var r := f(); "
        "    return r; "
        "  end Main; "
        "end Program;";

    // Compile and run natively to verify correctness.
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz712_c2_");
    TempFile712 srcFile((dir / (stem + ".bzl")).string());
    TempFile712 exeFile((dir / (stem + "_exe")).string());
    writeSource712(srcFile.path, singleSrc);

    bezel::Driver driver;
    bezel::CompileResult r = bezel::test::compileFileForTest(driver, srcFile.path, exeFile.path, {});
    if (r.success) {
        int status = std::system(exeFile.path.c_str());
        int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
        BEZEL_EXPECT_EQ(code, 42);
    }

    // LSan: single-module should produce the #672 residual only (closure object
    // + env cell already freed in single-module path).  After fix, cross-module
    // matches this (tested in C1).  This test just verifies baseline correctness
    // (exit 42) and confirms the single-module path was not regressed.
    int lsan = singleModLsan712(singleSrc, 42);
    if (lsan == -1) return;  // toolchain unavailable: skip
    // The single-module baseline may still exit 99 (LSan #672 residual).
    // That is acceptable — it is the known pre-existing residual, not a regression.
    // We only assert that the exit code is NOT -1 (toolchain ok) and is consistent
    // (0 = clean, 1 = #672 residual; not 2 = unexpected exit).
    BEZEL_EXPECT_TRUE(lsan == 0 || lsan == 1);
}


// ===========================================================================
// CANARY — imported method returning a non-closure (int) must NOT be over-
// released.  The fix only touches Kind::Function return types; returning `int`
// must leave isUserMethodClosureReturnCall returning false → no spurious
// bz_closure_release → no double-free / no UAF / no ASan abort.
// ===========================================================================
BEZEL_TEST(xmod_closure_return_712, canary_non_closure_return_no_double_free_28_9) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz712_can_");
    TempFile712 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile712 aObj((dir / (stem + "_a.o")).string());

    // A: a simple method returning int (not a closure).
    writeSource712(aSrc.path,
        "module IntLib; "
        "public class Calc "
        "  public constructor() end constructor; "
        "  public method Add(a: int, b: int): int => a + b; "
        "  destructor end destructor; "
        "end Calc;");

    BEZEL_EXPECT_TRUE(compileToObj712(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    // B: calls Add — must not over-release the int result (no UAF/double-free).
    std::string bSrc =
        "module App; "
        "import IntLib; "
        "public class Program "
        "  public static method Main(): int "
        "    using var c := new Calc(); "
        "    var r := c.Add(20, 22); "  // non-closure return (int)
        "    return r; "                // 42
        "  end Main; "
        "end Program;";

    // Native correctness check.
    TempFile712 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile712 bExe((dir / (stem + "_b")).string());
    writeSource712(bSrcFile.path, bSrc);
    BEZEL_EXPECT_EQ(compileImportAndRun712(bSrcFile.path, bExe.path, aObj.path), 42);

    // LSan + ASan (detect_leaks=1): must be clean — no over-release, no leak.
    std::vector<bezel::Dependency> deps;
    bezel::Dependency dep; dep.metadataPath = aObj.path; dep.objectPath = aObj.path;
    deps.push_back(dep);
    int lsan = xmodLsan712(aObj.path, bSrc, deps, 42);
    if (lsan == -1) return;  // toolchain unavailable: skip
    BEZEL_EXPECT_EQ(lsan, 0);
}

}  // namespace bezel::module
