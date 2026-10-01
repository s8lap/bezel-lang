// Regression + control tests for fix #1415 (§28.3 affine auto-drop of a
// never-discharged CROSS-MODULE owned-returning call result).
//
// Same drift class as #1388: the sema ownership pass's `producerState` classified
// an owned-returning call ONLY via the callee MethodSymbol's `ast::MethodDecl::
// returnOwned`. For an IMPORTED method `ms->decl` is null (ModuleLoader attaches
// no AST MethodDecl); the owned-return flag lives in `SemaContext::
// importedReturnOwned_`, which `producerState` never consulted. So a plain
// `var x := Factory.MakeCell(7);` (MakeCell `: owned Cell` in another module)
// was NEVER tracked Owned: no §28.3 affine warning, no `autoDropAtScopeEnd`, no
// scope-end disposal — a SILENT cross-module leak.
//
// Codegen already handles this exact case: `unwindOwnedLocalClass` /
// `usingVarOwnsClassObjectDynamically` both do `(md && md->returnOwned) ||
// (!md && ctx_.importedReturnOwned(ms))`. The fix makes `producerState` mirror
// that same fallback, so the fact lives in one place.
//
// TEST MATRIX (module A exports an owned-returning factory + a borrow-returning
// accessor; module B binds the result to a plain `var`):
//   T1  never-discharged imported OWNED return — §28.3 auto-drop WARNING fires
//       (sema), and the object is disposed at scope end (LSan clean; the exit
//       code confirms the destructor ran).
//   T2  DISCHARGED (`using var`) imported owned return — NO warning, LSan clean.
//   T3  BORROW CANARY: imported method returning a NON-owned (borrow) class ref,
//       bound to a plain `var` — NO warning (importedReturnOwned is false), and
//       NOT disposed by B (no double-free; the owner disposes it once).
//
// Spec citations: §28.3, §28.10 (cross-module owned return), §28.8.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "codegen/LlvmBackend.hpp"
#include "driver/Driver.hpp"
#include "lexer/Lexer.hpp"
#include "module/ModuleLoader.hpp"
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

// Per-process unique working dir under the shared scratch base. The hardcoded
// shared /var/tmp/bezel-scratch/wt-1415/ path was the cause of the #3002-class
// xmod COVERAGE SHORT: every shard that ran this test wrote its dep.bzl /
// rt_asan.o to the same directory, so concurrent attempts overwrote each other's
// source file mid-compileObject child-read, garbled the in-flight read, and the
// shard crashed with SIGBUS. Process-unique stem (atomic counter + ::getpid)
// gives every shard its own directory under the same parent.
const std::filesystem::path& wtDir1415() {
    return wtScratchDir("wt-1415");
}


namespace fs = std::filesystem;

struct TempFile1415 {
    std::string path;
    explicit TempFile1415(const std::string& p) : path(p) {}
    ~TempFile1415() { std::remove(path.c_str()); }
};

void writeSource1415(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Lane-local ASan runtime, built once under /var/tmp/bezel-scratch (DISK-backed
// per CLAUDE.md — never /tmp, which is tmpfs). Empty return → SKIP.
static const std::string& laneLocalAsanRt1415() {
    static std::string cached;
    static bool attempted = false;
    if (attempted) return cached;
    attempted = true;
    std::string dst = (wtDir1415() / "rt_asan.o").string();
    std::error_code ec;
    fs::create_directories(wtDir1415(), ec);
    std::ifstream check(dst);
    if (check.good()) { cached = dst; return cached; }
    if (!::bezel::native_test::buildRuntimeSanitizerObject("address", dst))
        return cached;  // leave empty → SKIP
    cached = dst;
    return cached;
}

// Compile module A to an object via the Driver.
static bool compileToObj1415(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    bezel::CompileResult r = driver.compileObject(src, obj);
    return r.success;
}

// Compile module B (importing A's object) to an executable and run it.
// Returns the exit code, or -1 on compile/link/run failure.
static int compileImportAndRun1415(const std::string& bSrc,
                                   const std::string& bExe,
                                   const std::string& aObj) {
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj;
    d.objectPath   = aObj;
    deps.push_back(d);
    bezel::CompileResult r = driver.compileFile(bSrc, bExe, deps);
    if (!r.success) return bezel::test::asanBuildFailure("compileFile");
    int status = std::system(bExe.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// Sema-only cross-module pipeline: parse B, load A's metadata, run through
// ownership, and report whether the §28.3 auto-drop WARNING fired for `name`
// (and no hard errors). Mirrors ownership_support::hasAutoDropWarning but with
// the dependency loaded so imported symbols resolve.
static bool xmodAutoDropWarns1415(const std::string& bSrc,
                                  const std::string& aObj,
                                  const std::string& name) {
    bezel::DiagnosticEngine diags;
    bezel::Lexer lexer(bSrc, diags);
    bezel::Parser parser(lexer, diags);
    auto unit = parser.parseCompilationUnit();
    if (!unit) return false;
    bezel::sema::SemaContext ctx(diags);
    bezel::sema::collectSymbols(*unit, ctx);
    bezel::module::loadDependency(aObj, ctx);
    bezel::sema::resolveTypes(*unit, ctx);
    bezel::sema::typecheckExpressions(*unit, ctx);
    bezel::sema::checkDefiniteAssignment(*unit, ctx);
    bezel::ownership::checkOwnership(*unit, ctx);
    if (diags.hasErrors()) return false;
    for (const auto& d : diags.diagnostics()) {
        if (d.severity != bezel::Severity::Warning) continue;
        if (d.message.find("is implicitly disposed at the end of") ==
            std::string::npos)
            continue;
        if (d.message.find("`" + name + "`") != std::string::npos) return true;
    }
    return false;
}

// Cross-module LSan probe: compile B importing A, link BOTH objects with the
// lane-local ASan runtime, run under detect_leaks=1.
//   0  — clean + correct (expected exit code).
//   1  — leak / memory-safety error (exit 99).
//  -1  — toolchain unavailable / build failure → SKIP.
static int xmodLsan1415(const std::string& aObj, const std::string& bSrc,
                        int expectedCode) {
    const std::string& rt = laneLocalAsanRt1415();
    if (rt.empty()) return -1;

    auto tmpDir = fs::path(wtDir1415());
    std::string stem = unique_stem("bz1415_lsan");
    std::string bObjPath = (tmpDir / (stem + "_b.o")).string();
    std::string exePath  = (tmpDir / (stem + "_exe")).string();

    {
        bezel::DiagnosticEngine diags;
        bezel::Lexer lexer(bSrc, diags);
        bezel::Parser parser(lexer, diags);
        auto unit = parser.parseCompilationUnit();
        if (!unit) return bezel::test::asanBuildFailure("parse");
        bezel::sema::SemaContext ctx(diags);
        bezel::sema::collectSymbols(*unit, ctx);
        bezel::module::loadDependency(aObj, ctx);
        bezel::sema::resolveTypes(*unit, ctx);
        bezel::sema::typecheckExpressions(*unit, ctx);
        bezel::sema::checkDefiniteAssignment(*unit, ctx);
        bezel::ownership::checkOwnership(*unit, ctx);
        if (diags.hasErrors()) return bezel::test::asanBuildFailure("sema");
        llvm::LLVMContext llvmCtx;
        auto mod = bezel::codegen::emitModule(*unit, ctx, llvmCtx);
        if (!mod) return bezel::test::asanBuildFailure("emitModule");
        std::string err;
        if (!bezel::codegen::emitObjectFile(*mod, bObjPath, err))
            return bezel::test::asanBuildFailure("emitObjectFile");
    }

    auto cleanup = [&] {
        std::remove(bObjPath.c_str());
        std::remove(exePath.c_str());
    };

    std::string linkCmd = "clang++ -fsanitize=address -g " + aObj + " " +
                          bObjPath + " " + rt +
                          " -lm -lpthread -lstdc++ -o " + exePath +
                          " > /dev/null 2>&1";
    if (std::system(linkCmd.c_str()) != 0) {
        cleanup();
        return bezel::test::asanLinkFailure("link");
    }

    std::string runCmd =
        "ASAN_OPTIONS=detect_leaks=1:exitcode=99:abort_on_error=0 " + exePath +
        " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return bezel::test::asanExecFailure("system");
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (code == 99) return 1;
    if (code == expectedCode) return 0;
    return -3;  // unexpected exit code — skip
}

// Module A: a Cell class whose destructor bumps a static counter (readable from
// B via GetDisposed), plus an owned-returning factory and a borrow accessor.
const char* kModA =
    "module LibA; "
    "public class Cell "
    "  public static _disposed: int := 0; "
    "  public Val: int; "
    "  public constructor(v: int) Val := v; end constructor; "
    "  destructor Cell._disposed := Cell._disposed + 1; end destructor; "
    "  public static method GetDisposed(): int => Cell._disposed; "
    "end Cell; "
    "public class Factory "
    "  public static method MakeCell(v: int): owned Cell "
    "    return new Cell(v); "
    "  end MakeCell; "
    "end Factory; "
    "public class Holder "
    "  public owned Cell1: Cell; "
    "  public constructor() Cell1 := new Cell(55); end constructor; "
    "  public method Borrow(): Cell => Cell1; "  // borrow — NOT owned
    "  destructor end destructor; "
    "end Holder;";

}  // namespace

// ===========================================================================
// T1 — never-discharged imported OWNED return: §28.3 auto-drop fires + disposes.
// ===========================================================================
BEZEL_TEST(xmod_owned_return_autodrop_1415, t1_imported_owned_never_discharged_28_3) {
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz1415_t1_");
    TempFile1415 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile1415 aObj((dir / (stem + "_a.o")).string());
    writeSource1415(aSrc.path, kModA);
    BEZEL_EXPECT_TRUE(compileToObj1415(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    // B: `var x := Factory.MakeCell(7);` never discharged. After the fix the
    // auto-drop disposes it at scope end; GetDisposed() (read AFTER the inner
    // block) confirms the destructor ran exactly once.
    std::string bSrc =
        "module App; import LibA; "
        "public class Program "
        "  public static method Main(): int "
        "    do "
        "      var x := Factory.MakeCell(7); "  // never discharged → auto-drop
        "    end; "
        "    return Cell.GetDisposed(); "  // 1 = disposed
        "  end Main; "
        "end Program;";

    // (a) SEMA: the auto-drop warning fires for `x`.
    BEZEL_EXPECT_TRUE(xmodAutoDropWarns1415(bSrc, aObj.path, "x"));

    // (b) Native correctness: disposed exactly once → exit 1.
    TempFile1415 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile1415 bExe((dir / (stem + "_b")).string());
    writeSource1415(bSrcFile.path, bSrc);
    BEZEL_EXPECT_EQ(compileImportAndRun1415(bSrcFile.path, bExe.path, aObj.path), 1);

    // (c) LSan: no leak.
    int lsan = xmodLsan1415(aObj.path, bSrc, 1);
    if (lsan == -1) return;  // toolchain unavailable — skip
    BEZEL_EXPECT_EQ(lsan, 0);
}

// ===========================================================================
// T2 — DISCHARGED (`using var`) imported owned return: no warning, LSan clean.
// ===========================================================================
BEZEL_TEST(xmod_owned_return_autodrop_1415, t2_imported_owned_using_var_no_warn_28_3) {
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz1415_t2_");
    TempFile1415 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile1415 aObj((dir / (stem + "_a.o")).string());
    writeSource1415(aSrc.path, kModA);
    BEZEL_EXPECT_TRUE(compileToObj1415(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    std::string bSrc =
        "module App; import LibA; "
        "public class Program "
        "  public static method Main(): int "
        "    using var x := Factory.MakeCell(9); "  // discharged
        "    return x.Val; "  // 9
        "  end Main; "
        "end Program;";

    // No auto-drop warning (using var is explicitly discharged).
    BEZEL_EXPECT_FALSE(xmodAutoDropWarns1415(bSrc, aObj.path, "x"));

    TempFile1415 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile1415 bExe((dir / (stem + "_b")).string());
    writeSource1415(bSrcFile.path, bSrc);
    BEZEL_EXPECT_EQ(compileImportAndRun1415(bSrcFile.path, bExe.path, aObj.path), 9);

    int lsan = xmodLsan1415(aObj.path, bSrc, 9);
    if (lsan == -1) return;
    BEZEL_EXPECT_EQ(lsan, 0);
}

// ===========================================================================
// T3 — BORROW CANARY: imported borrow-returning method bound to a plain `var`
// must NOT warn (importedReturnOwned false) and must NOT be disposed by B.
// ===========================================================================
BEZEL_TEST(xmod_owned_return_autodrop_1415, t3_imported_borrow_return_not_owned_28_3) {
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz1415_t3_");
    TempFile1415 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile1415 aObj((dir / (stem + "_a.o")).string());
    writeSource1415(aSrc.path, kModA);
    BEZEL_EXPECT_TRUE(compileToObj1415(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    // B: `var c := h.Borrow();` is a BORROW — h (a using var) owns and disposes
    // the Cell once. If the fix wrongly disposed the borrow, ASan would catch a
    // double-free (exit 99).
    std::string bSrc =
        "module App; import LibA; "
        "public class Program "
        "  public static method Main(): int "
        "    using var h := new Holder(); "
        "    var c := h.Borrow(); "  // borrow — must NOT be disposed by B
        "    return c.Val; "  // 55
        "  end Main; "
        "end Program;";

    // No auto-drop warning (borrow return carries no obligation).
    BEZEL_EXPECT_FALSE(xmodAutoDropWarns1415(bSrc, aObj.path, "c"));

    TempFile1415 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile1415 bExe((dir / (stem + "_b")).string());
    writeSource1415(bSrcFile.path, bSrc);
    BEZEL_EXPECT_EQ(compileImportAndRun1415(bSrcFile.path, bExe.path, aObj.path), 55);

    int lsan = xmodLsan1415(aObj.path, bSrc, 55);
    if (lsan == -1) return;
    BEZEL_EXPECT_EQ(lsan, 0);  // 0 = LSan clean (no leak, no double-free)
}

}  // namespace bezel::module
