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
#include <filesystem>
#include <fstream>
#include <string>

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
#include "support/aot_support.hpp"  // runAotModules / runAotModulesChecked
#include "test_harness.hpp"

#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"


// ── PORT NOTE (#6386, round 2) ───────────────────────────────────────────────
//
// Two of this file's three oracle legs per row were hand-rolled and are retired:
//
//   * leg (b), `compileImportAndRun1415` — Driver::compileFile IN THE TEST
//     PROCESS, then `std::system(exe)` + `WEXITSTATUS`. Now
//     `test::runAotModules`, which forks the driver step.
//   * leg (c), `xmodLsan1415` — a 60-line hand-rolled ASan probe that emitted
//     B's object with `emitModule` in-process, shelled out to `clang++` to link
//     A's and B's objects against a lane-local ASan runtime, and ran under
//     `detect_leaks=1:exitcode=99:abort_on_error=0`. Now
//     `test::runAotModulesChecked`, which uses the same environment and the same
//     report-text oracle `runAotChecked` uses.
//
// ⚠ THIS IS THE FILE WHERE THE OLD SKIP GUARDS WERE MOST DANGEROUS, AND THAT IS
// WHY IT IS WORTH A PORT. Each row's leg (c) read:
//
//     int lsan = xmodLsan1415(aObj.path, bSrc, expected);
//     if (lsan == -1) return;              // toolchain unavailable → SKIP
//     BEZEL_EXPECT_EQ(lsan, 0);
//
// so "the sanitizer never ran" and "the sanitizer found nothing" were the SAME
// observation — the only assertion was inside the guard's shadow. Combined with
// `if (!std::ifstream(aObj.path).good()) return;` earlier in the row, a green
// T1/T2/T3 could mean the library never compiled, the link never happened, or
// the sanitizer was unavailable. Each leg now asserts `ran` FIRST and
// UNCONDITIONALLY and then the exit code; a leak surfaces as `exitCode == 99`
// and a build/link failure as `ran == false`. Nothing was removed.
//
// ⚠ LEG (c) IS NOW STRICTER THAN IT WAS, IN TWO WAYS. The old probe linked A's
// object as built by `Driver::compileObject` — which forces sanitization OFF for
// a library artifact — against an ASan-instrumented B. `runAotModulesChecked`
// sets `opts.sanitizeAddress`, which `runAotModulesImpl` forces on for the
// dependency build too, so BOTH artifacts are instrumented. And it now sets
// `opts.checkedContracts = true` (§22.16); neither of this file's programs
// declares an `ensures` / `invariant` clause, so no clause evaluation is
// emitted and the observable is unchanged.
//
// NOT PORTED, AND IT CANNOT BE: leg (a), the SEMA warning probe
// (`xmodAutoDropWarns1415`). It is a front-end probe, not an oracle — it calls
// `loadDependency` on a library object at a real path, which `runAotModules`
// does not materialize — so it still compiles the library in the test process
// via `compileToObj1415`. Its two call sites per row are unchanged.
//
// Test names byte-identical (#6386 rule 5). No row added or deleted. No exit
// code here exceeds 255.

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

// Compile module A to an object via the Driver.
//
// ⚠ STILL IN THE TEST PROCESS, AND IT CANNOT BE. The only surviving caller is
// the SEMA warning probe below, which needs the library's dependency metadata
// at a real path so it can `loadDependency`; `runAotModules` does not expose one.
// The two ORACLE legs of each row no longer use this helper -- they go through
// `runAotModules` / `runAotModulesChecked`, which fork the driver.
static bool compileToObj1415(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    bezel::CompileResult r = driver.compileObject(src, obj);
    return r.success;
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
    auto dir = wtDir1415();
    std::string stem = unique_stem("bz1415_t1_");
    TempFile1415 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile1415 aObj((dir / (stem + "_a.o")).string());
    writeSource1415(aSrc.path, kModA);

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
    //
    // ⚠ THIS LEG IS NOT PORTED, AND IT CANNOT BE. `xmodAutoDropWarns1415` is an
    // in-process FRONT-END probe, not an oracle: it needs the library's metadata
    // on disk to `loadDependency`, and `runAotModules` does not materialize an
    // object at a path the caller can see. So the library object is still
    // compiled here, in the test process, for this leg alone. The two oracle
    // legs below no longer depend on it. The ABSORBING GUARD that used to sit
    // here --
    //     if (!std::ifstream(aObj.path).good()) return;
    // -- is gone: it skipped legs (b) and (c) entirely on a library-compile
    // failure, so a green row could mean "never got as far as running".
    BEZEL_EXPECT_TRUE(compileToObj1415(aSrc.path, aObj.path));
    BEZEL_EXPECT_TRUE(xmodAutoDropWarns1415(bSrc, aObj.path, "x"));

    // (b) Native correctness: disposed exactly once → exit 1. `if (!r.builtOk)
    // return;` and `if (r.sanitizerFired) return;` are both GONE: `r.ran` is
    // false in every case either caught, and asserting it FIRST means a build
    // failure no longer suppresses the exit-code assertion on that leg.
    auto r = test::runAotModules({{"LibA", kModA}, {"App", bSrc}}, "App");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);

    // (c) LSan: no leak. The old `int lsan = xmodLsan1415(...); if (lsan == -1)
    // return; BEZEL_EXPECT_EQ(lsan, 0);` is a SKIP-ABSORBING pair: the guard
    // returned before the ONLY assertion whenever the toolchain probe failed,
    // so "the sanitizer never ran" and "the sanitizer found nothing" were the
    // same observation. Now the leg asserts `c.ran` unconditionally and then the
    // exit code: a leak gives `c.exitCode == 99` under
    // `detect_leaks=1:exitcode=99:abort_on_error=0`, which the exit-code
    // assertion catches, and a link/toolchain failure leaves `ran` false.
    auto c = test::runAotModulesChecked({{"LibA", kModA}, {"App", bSrc}}, "App");
    BEZEL_EXPECT_TRUE(c.ran);
    BEZEL_EXPECT_EQ(c.exitCode, 1);
}

// ===========================================================================
// T2 — DISCHARGED (`using var`) imported owned return: no warning, LSan clean.
// ===========================================================================
BEZEL_TEST(xmod_owned_return_autodrop_1415, t2_imported_owned_using_var_no_warn_28_3) {
    auto dir = wtDir1415();
    std::string stem = unique_stem("bz1415_t2_");
    TempFile1415 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile1415 aObj((dir / (stem + "_a.o")).string());
    writeSource1415(aSrc.path, kModA);

    std::string bSrc =
        "module App; import LibA; "
        "public class Program "
        "  public static method Main(): int "
        "    using var x := Factory.MakeCell(9); "  // discharged
        "    return x.Val; "  // 9
        "  end Main; "
        "end Program;";

    // No auto-drop warning (using var is explicitly discharged). See T1's leg
    // (a) note: this front-end probe still compiles the library object itself.
    BEZEL_EXPECT_TRUE(compileToObj1415(aSrc.path, aObj.path));
    BEZEL_EXPECT_FALSE(xmodAutoDropWarns1415(bSrc, aObj.path, "x"));

    auto r = test::runAotModules({{"LibA", kModA}, {"App", bSrc}}, "App");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 9);

    // The `using var` is what makes this leg leak-free, so it is the leg most at
    // risk of silently losing its sanitizer if a guard returns early — which is
    // why `c.ran` is asserted before the exit code, unconditionally.
    auto c = test::runAotModulesChecked({{"LibA", kModA}, {"App", bSrc}}, "App");
    BEZEL_EXPECT_TRUE(c.ran);
    BEZEL_EXPECT_EQ(c.exitCode, 9);
}

// ===========================================================================
// T3 — BORROW CANARY: imported borrow-returning method bound to a plain `var`
// must NOT warn (importedReturnOwned false) and must NOT be disposed by B.
// ===========================================================================
BEZEL_TEST(xmod_owned_return_autodrop_1415, t3_imported_borrow_return_not_owned_28_3) {
    auto dir = wtDir1415();
    std::string stem = unique_stem("bz1415_t3_");
    TempFile1415 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile1415 aObj((dir / (stem + "_a.o")).string());
    writeSource1415(aSrc.path, kModA);

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

    // No auto-drop warning (borrow return carries no obligation). See T1's leg
    // (a) note: this front-end probe still compiles the library object itself.
    BEZEL_EXPECT_TRUE(compileToObj1415(aSrc.path, aObj.path));
    BEZEL_EXPECT_FALSE(xmodAutoDropWarns1415(bSrc, aObj.path, "c"));

    auto r = test::runAotModules({{"LibA", kModA}, {"App", bSrc}}, "App");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 55);

    // This is the BORROW CANARY, so its sanitizer leg is the one that would go
    // RED on a double-free (exit 99). Asserting `c.ran` first, unconditionally,
    // is what keeps "the sanitizer did not run" from reading as "the sanitizer
    // found nothing" — the old `if (lsan == -1) return;` did exactly that.
    auto c = test::runAotModulesChecked({{"LibA", kModA}, {"App", bSrc}}, "App");
    BEZEL_EXPECT_TRUE(c.ran);
    BEZEL_EXPECT_EQ(c.exitCode, 55);
}

}  // namespace bezel::module
