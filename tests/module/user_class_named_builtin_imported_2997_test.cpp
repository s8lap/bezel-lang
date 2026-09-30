// Regression tests for issue #2997 (language spec §11.3 user-declared class
// names / §28.3 scope-bound auto-drop / §28.9 runtime-managed types, v0.240):
// an IMPORTED user class whose name collides with a built-in's, bound to a
// `using var`, was released with the BUILT-IN's C release — the cross-module
// residual of #2991.
//
//   -- Lib.bzl
//   module Lib;
//   public class Payload public readonly property N: int := 7; end Payload;
//   public class Stopwatch
//     private owned _p: Payload := new Payload();
//     public readonly property Tag: int := 3;
//   end Stopwatch;
//   -- App.bzl
//   module App; import Lib;
//   class C public static method Main(): int
//     var i := 0; var acc := 0;
//     while i < 200 do
//       using var s := new Stopwatch();   // -> bz_stopwatch_free(userobj)
//       acc := acc + s.Tag;               //    the owned Payload leaks; the
//       i := i + 1;                       //    object is reclaimed but never
//     end;                                //    cascaded.
//     if acc <> 600 then return 9; end; return 0;
//   end Main; end C;
//
// §11.3 reserves no names, so a dependency module is free to `public class
// Stopwatch` and an importer's `using var s := new Stopwatch();` is a legal
// program containing no built-in at all. This is memory unsafety reachable
// from a legal cross-module program with no diagnostic.
//
// ── WHY #2991'S FIX DID NOT REACH THIS, AND THE ROOT CAUSE ──────────────────
// #2991 closed the SINGLE-module family by routing every NewExpr probe in
// registerUsingDisposal through `Emitter::newExprBuiltinHead`, which requires a
// DECLARATION-LESS resolved type. That excludes a LOCAL user class (it carries a
// `declaration` with a real `ast::TypeDecl`) and costs nothing for genuine
// built-ins (a `new` site's type node never resolves to the `builtinClassSym`
// shape). But five arms — Stopwatch / FileStream / CancellationTokenSource /
// Random / Atomic — also probe the initializer's exprType through
// `Emitter::builtinTypeName`, which CANNOT require declaration-less: catching
// the `builtinClassSym` result of `Stopwatch.StartNew()` /
// `CancellationTokenSource.WithCancelAfter(d)` is that probe's whole purpose
// (#463).
//
// And for an IMPORTED user class that third probe is identical to a built-in.
// The module loader (ModuleLoader.cpp loadMetadataPass1) reconstructs a
// dependency's class as a `sema::TypeSymbol` with NO `ast::TypeDecl` (there is
// no AST for another module's source) — EXACTLY the shape a `builtinClassSym`
// has (non-null declaration, null `decl`). So `builtinTypeName` returned the
// imported class's own name and every probe routed through it treated the
// imported user class as the built-in.
//
// ── THE FIX: ONE FACT, ASKED ONCE ───────────────────────────────────────────
// The fact that tells them apart is one the compiler already computes:
// TypeSymbol::moduleName. The loader populates it from the dependency's metadata
// for EVERY imported type (#1488); a synthetic builtinClassSym
// (SemaContext::builtinClassSym) leaves it empty. So `builtinTypeName`'s shape
// (b) now requires an EMPTY moduleName in addition to a null `decl`. Answering
// it in this one chokepoint fixes all five residual arms at once AND every other
// site that reads `builtinTypeName` for a builtin/user decision.
//
// ── THE MEASURED GRID ON HEAD (8eab3f8, BEFORE THE FIX) ─────────────────────
// Same 200-iteration `using var` loop, imported user class carrying
// `private owned _p: Payload`, under generated-code ASan/LSan:
//   Stopwatch                3200 B leaked / 200 allocs (exit 1)
//   FileStream               3200 B leaked / 200 allocs (exit 1)
//   CancellationTokenSource  3200 B leaked / 200 allocs (exit 1)
//   Random                   3200 B leaked / 200 allocs (exit 1)
//   Atomic                   3200 B leaked / 200 allocs (exit 1)
// After the fix all five are exit 0 / no leak; the genuine builtins (incl. the
// factory-produced `Stopwatch.StartNew()` that the third probe exists to catch)
// stay released — pinned by the controls below.

#include "driver/Driver.hpp"   // Driver::compileObject / compileFile (AOT path)
#include "test_harness.hpp"
#include "support/driver_run_support.hpp"  // #3156: the ONE sanitizer-honouring compileFile wrapper

#include <fcntl.h>
#include <unistd.h>
#include <cstdlib>
#include <iterator>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <vector>
#include "support/unique_stem.hpp"


namespace bezel::module {
namespace {

namespace fs = std::filesystem;

struct TempFile {
    std::string path;
    explicit TempFile(const std::string& p) : path(p) {}
    ~TempFile() { std::remove(path.c_str()); }
};

void writeSource(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Compile ONE library module to an object, then compile `mainSrc` importing it
// under generated-code AddressSanitizer (opts.sanitizeAddress — the same recipe
// runAotChecked uses, so a missed owned-field cascade surfaces as a
// LeakSanitizer report with a non-zero exit), link, run, and return the exit
// code. -1 on any compile/link/run failure. Mirrors buildAndRunTwoLibs in
// cross_module_type_name_collision_1488_test.cpp, narrowed to one dependency
// and extended with sanitizeAddress (the leak oracle this family needs).
int buildLibAndRunChecked(const std::string& libSrc, const std::string& mainSrc,
                          const std::string& tag,
                          std::string* capturedStderr = nullptr) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_2997_" + tag + "_" + unique_stem("");
    TempFile libSrcF((dir / (stem + "_lib.bzl")).string());
    TempFile libObjF((dir / (stem + "_lib.o")).string());
    TempFile mainSrcF((dir / (stem + "_main.bzl")).string());
    TempFile mainExeF((dir / stem).string());

    writeSource(libSrcF.path, libSrc);
    writeSource(mainSrcF.path, mainSrc);

    bezel::Driver driver;
    bezel::CompileResult rLib =
        driver.compileObject(libSrcF.path, libObjF.path);
    if (!rLib.success) return -1;

    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = libObjF.path;
    d.objectPath = libObjF.path;
    deps.push_back(d);

    // sanitizeAddress: a wrong release that reclaims the object but skips the
    // owned-field cascade leaves 200 Payloads on the heap — LSan exits non-zero.
    bezel::codegen::CodegenOptions opts;
    opts.sanitizeAddress = true;
    // Optionally capture the importer compile's stderr (diagnostic text).
    TempFile errF((dir / (stem + "_main.err")).string());
    int saved = -1;
    if (capturedStderr) {
        std::fflush(stderr);
        saved = ::dup(STDERR_FILENO);
        int sink = ::open(errF.path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (sink >= 0) { ::dup2(sink, STDERR_FILENO); ::close(sink); }
    }
    bezel::CompileResult rMain = bezel::test::compileFileForTest(driver, 
        mainSrcF.path, mainExeF.path, deps, opts);
    if (capturedStderr) {
        std::fflush(stderr);
        if (saved >= 0) { ::dup2(saved, STDERR_FILENO); ::close(saved); }
        std::ifstream in(errF.path);
        capturedStderr->assign((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    }
    if (!rMain.success) return -1;

    int status = std::system(mainExeF.path.c_str());
    if (status == -1 || !WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

// Single-module build under generated-code ASan, for the genuine-builtin
// controls (the built-in has no dependency module). Same leak oracle.
int buildSingleChecked(const std::string& src, const std::string& tag) {
    auto dir = fs::temp_directory_path();
    std::string stem =
        "bezel_2997_single_" + tag + "_" + unique_stem("");
    TempFile srcF((dir / (stem + ".bzl")).string());
    TempFile exeF((dir / stem).string());
    writeSource(srcF.path, src);

    bezel::Driver driver;
    bezel::codegen::CodegenOptions opts;
    opts.sanitizeAddress = true;
    bezel::CompileResult r =
        bezel::test::compileFileForTest(driver, srcF.path, exeF.path, {}, opts);
    if (!r.success) return -1;

    int status = std::system(exeF.path.c_str());
    if (status == -1 || !WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

// A library exporting `public class <Name>` carrying BOTH observables that
// matter (mirrors #2991's userClassNamed):
//   * `private owned _p: Payload` — the LEAK oracle. Only the user class's own
//     disposal cascades into an owned field, so if a built-in's C release runs
//     instead the 200 Payloads leak (this is what the plain `free()` built-in
//     releases — Stopwatch/Random/Atomic — hid pre-fix: right exit, lost
//     cascade). For FileStream/CancellationTokenSource the wrong release also
//     read user bytes as a FILE*/token list and SEGV'd/aborted.
//   * `public readonly property Tag` — the VALUE oracle, so "exit 0" cannot be
//     reached by a program that silently did nothing.
std::string libExportingUserClass(const std::string& moduleName,
                                  const char* className) {
    return "module " + moduleName + "; "
        "public class Payload "
        "  public readonly property N: int := 7; "
        "end Payload; "
        "public class " + std::string(className) + " "
        "  private owned _p: Payload := new Payload(); "
        "  public readonly property Tag: int := 3; "
        "end " + std::string(className) + ";";
}

// An importer that runs the colliding user class through a 200-iteration
// `using var` loop. 200 iterations because a per-iteration disposal miss shows
// up as 200 leaked allocations, which no stray single allocation can mimic.
std::string mainImportingUserClass(const std::string& mainModule,
                                   const std::string& importModule,
                                   const char* className,
                                   bool qualify = false) {
    // `qualify`: spell the type `<importModule>.<className>` (§2.3 / §2.2,
    // v0.309, fix #5140: a bare always-available name shadowed by an imported
    // public is ambiguous, so `Random` must be module-qualified).
    std::string typeName = qualify ? importModule + "." + className
                                   : std::string(className);
    return "module " + mainModule + "; "
        "import " + importModule + "; "
        "class C public static method Main(): int "
        "  var i := 0; var acc := 0; "
        "  while i < 200 do "
        "    using var s := new " + typeName + "(); "
        "    acc := acc + s.Tag; "
        "    i := i + 1; "
        "  end; "
        "  if acc <> 600 then return 9; end; "
        "  return 0; "
        "end Main; end C;";
}

// Runs one colliding name cross-module and reports WHICH name failed.
void expectImportedUserClassDisposed(const char* className,
                                     bool qualify = false) {
    std::string libMod = std::string("Lib") + className;
    std::string mainMod = std::string("App") + className;
    int code = buildLibAndRunChecked(
        libExportingUserClass(libMod, className),
        mainImportingUserClass(mainMod, libMod, className, qualify),
        className);
    if (code != 0) {
        std::cout << "    #2997: imported user class named `" << className
                  << "` was not disposed as a user class (exit=" << code
                  << ")\n";
    }
    BEZEL_EXPECT_EQ(0, code);
}

}  // namespace

// ── The five residual names: each took the built-in's C release pre-fix ──────
// Before the fix each leaked 3200 B in 200 allocations under ASan/LSan (exit
// 1); FileStream/CancellationTokenSource additionally SEGV'd/aborted through
// the third probe on some spellings. All five reach their arm through
// builtinTypeName(initializer exprType), the probe #2991 could not close.

BEZEL_TEST(user_class_named_builtin_imported_2997,
           imported_stopwatch_runs_user_disposal_not_bz_stopwatch_free_spec_11_3) {
    // The issue's verbatim repro. Pre-fix on 8eab3f8: 3200 B leaked in 200
    // allocs — the owned Payload never cascaded because bz_stopwatch_free is a
    // plain free() of the object.
    expectImportedUserClassDisposed("Stopwatch");
}

BEZEL_TEST(user_class_named_builtin_imported_2997,
           imported_filestream_runs_user_disposal_not_bz_filestream_dispose_spec_28_3) {
    // Pre-fix: bz_filestream_dispose read the user object's bytes as a FILE*
    // (SEGV/abort on some builds); under ASan on 8eab3f8 the leak form (exit 1).
    expectImportedUserClassDisposed("FileStream");
}

BEZEL_TEST(user_class_named_builtin_imported_2997,
           imported_cts_runs_user_disposal_not_bz_cts_dispose_spec_28_3) {
    // Pre-fix: bz_cts_dispose walked user bytes as a token-observer list.
    expectImportedUserClassDisposed("CancellationTokenSource");
}

BEZEL_TEST(user_class_named_builtin_imported_2997,
           imported_random_runs_user_disposal_spec_28_9) {
    // Pre-fix: #2973's own guard did not reach the imported spelling for the
    // same reason (builtinTypeName saw the imported class as the built-in).
    // Module-qualified (`LibRandom.Random`): the bare spelling is ambiguous
    // under §2.3 (v0.309, fix #5140) — pinned by the next row.
    expectImportedUserClassDisposed("Random", /*qualify=*/true);
}

BEZEL_TEST(user_class_named_builtin_imported_2997,
           imported_random_bare_name_is_ambiguous_rejected_spec_2_3) {
    // The old fixture shape: bare `new Random()` with an imported public
    // `Random` in scope is rejected (§2.3 / §2.2, v0.309; fix #5140).
    std::string diagText;
    int code = buildLibAndRunChecked(
        libExportingUserClass("LibRandom", "Random"),
        mainImportingUserClass("AppRandomBare", "LibRandom", "Random"),
        "RandomBare", &diagText);
    BEZEL_EXPECT_EQ(-1, code);
    BEZEL_EXPECT_TRUE(diagText.find("bare name `Random` is ambiguous") !=
                      std::string::npos);
}

BEZEL_TEST(user_class_named_builtin_imported_2997,
           imported_atomic_runs_user_disposal_spec_28_9) {
    // Pre-fix: same as Random — #2973's vtableless-builtin arm matched the
    // imported user class through builtinTypeName and skipped the cascade.
    expectImportedUserClassDisposed("Atomic");
}

// ── The other direction: no GENUINE built-in may stop being released ─────────
// fix #2997 narrowed builtinTypeName's shape (b) to require an EMPTY moduleName,
// so the risk it carries is the opposite one — a real built-in that stops
// matching its arm and leaks. The third probe exists precisely to catch
// FACTORY-PRODUCED builtins (whose result type is a builtinClassSym with empty
// moduleName), so this is the canonical control: if shape (b) over-narrows,
// Stopwatch.StartNew() leaks here.

BEZEL_TEST(user_class_named_builtin_imported_2997,
           genuine_stopwatch_factory_still_released_at_scope_exit_spec_28_3) {
    // Stopwatch.StartNew()'s result type carries declaration =
    // builtinClassSym("Stopwatch") with an EMPTY moduleName, so builtinTypeName
    // shape (b) still returns "Stopwatch" after the fix — the third probe keeps
    // catching it. Pre-and-post fix this is exit 0; pinned so a future
    // "simplification" that drops the moduleName guard backwards (rejecting real
    // builtins) is caught.
    int code = buildSingleChecked(
        "module GenSW; import System.Diagnostics; class C public static method Main(): int "
        "  var i := 0; "
        "  while i < 50 do "
        "    using var s := Stopwatch.StartNew(); "
        "    i := i + 1; "
        "  end; "
        "  return 0; "
        "end Main; end C;",
        "genuine_sw_factory");
    BEZEL_EXPECT_EQ(0, code);
}

BEZEL_TEST(user_class_named_builtin_imported_2997,
           genuine_new_builtins_still_released_at_scope_exit_spec_28_3) {
    // The `new` spellings for the names this issue covers (genuine built-ins,
    // single module). These reach their arm through newExprBuiltinHead (#2991)
    // rather than the third probe, but pin that the chain as a whole still
    // releases the real builtins alongside the fix.
    auto loop = [](const char* decl) {
        return std::string(
            "module GenB; import System.Diagnostics; class C public static method Main(): int "
            "  var i := 0; while i < 50 do ") + decl +
            " i := i + 1; end; return 0; end Main; end C;";
    };
    for (const char* d : {"using var s := new Stopwatch();",
                          "using var s := new CancellationTokenSource();",
                          "using var s := new Random();"}) {
        int code = buildSingleChecked(loop(d), "genuine_new");
        if (code != 0)
            std::cout << "    #2997 genuine-builtin control regressed: " << d
                      << " (exit=" << code << ")\n";
        BEZEL_EXPECT_EQ(0, code);
    }
}

// ── Non-colliding control: an imported user class whose name is NOT a built-in ─
// Was never affected (no arm matches "Widget"); pinned so the fix did not
// regress ordinary cross-module user-class disposal.
BEZEL_TEST(user_class_named_builtin_imported_2997,
           imported_non_colliding_user_class_disposes_normally_control_spec_28_3) {
    expectImportedUserClassDisposed("Widget");
}

}  // namespace bezel::module
