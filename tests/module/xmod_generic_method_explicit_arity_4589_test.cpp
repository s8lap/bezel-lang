// Spec-derived regression test for issue #4589 — §14.5/#1687's "explicit
// type-argument lists are all-or-nothing" arity check is skipped entirely for
// an IMPORTED generic method.
//
// spec §14.5 "Type-argument inference for generic methods" ("Explicit lists
// are all-or-nothing"), v0.306. §2.8.3 rule 2 (an imported artifact must not
// alter an instantiation's semantics relative to the same declarations in one
// module).
//
// ── THE RULE ───────────────────────────────────────────────────────────────
//
// §14.5: "An explicit type-argument list … binds the declared type parameters
// in declaration order and must supply exactly as many arguments as the
// method declares type parameters. A shorter or a longer list is a compile
// error; there are no partial lists."
//
// ── THE GAP ────────────────────────────────────────────────────────────────
//
// `ExprTC::visitCallExpr`'s §14.5/#1687 arity check
// (src/sema/ExprTypecheck_Calls.cpp) was gated end-to-end on `md`, the
// AST-side `ast::MethodDecl*`:
//
//   auto* md = dynamic_cast<const ast::MethodDecl*>(ms->decl);
//   if (md && !md->genericParams.empty()) { ... arity check ... }
//
// `ModuleLoader` never assigns `MethodSymbol::decl` for an imported method —
// the importing compilation has no AST for a dependency's method bodies at
// all — so `md` is null for EVERY imported generic method and the check's
// entire body never ran. There was no `else if` consulting the symbol side
// (`MethodSymbol::genericParams`, populated from `MetaMember::genericParams`
// at import time — the same channel #4590's neighbouring §14.5 phase-3 fix,
// in this same file, already relies on).
//
// ── MEASURED, on this lane's HEAD (= origin/main + 15 landed fixes,
//    cafdfc52 base) with real two-module builds ───────────────────────────
//   (`bezelc -c dep.bzl -o dep.o` then `bezelc --import dep.o -o app app.bzl`)
//
// `Lib.Create4589<Widget4589, int>()` against an imported
// `Create4589<T>(): owned T` (ONE declared type parameter, TWO supplied)
// COMPILED CLEAN (zero diagnostics at any severity) and RAN, where the
// byte-identical single-module program is rejected with the §14.5 arity
// diagnostic. Confirmed for both the excess-list and the partial-list shapes
// (#1687's two directions).
//
// ── WHAT EACH ROW PINS ─────────────────────────────────────────────────────
//
//   T1 ★ THE BUG (excess). Imported generic method, explicit list LONGER
//        than its declared arity (2 args for 1 param) -> the SPECIFIC §14.5
//        diagnostic. Pre-fix: compiles clean and runs.
//   T2   DIFFERENTIAL CONTROL for T1. The byte-identical declarations and
//        call in ONE module — rejected with the SAME text before AND after,
//        so the module boundary is proved to be the whole difference.
//   T3 ★ THE BUG (partial). Imported generic method with TWO declared type
//        parameters, explicit list SHORTER (1 of 2) -> the SPECIFIC §14.5
//        diagnostic. Pre-fix: compiles clean and runs.
//   T4   DIFFERENTIAL CONTROL for T3.
//   T5   ANTI-FALSE-REJECT. A FULL, correct-arity explicit list against an
//        imported generic method must still compile AND RUN, exact value.
//        This is the real risk of an under-rejection fix: over-correcting to
//        reject a correct-arity call.
//   T6   ANTI-FALSE-REJECT. NO explicit list at all (ordinary argument-driven
//        inference, §14.5 phase 1) against a SINGLE-type-parameter imported
//        generic method must still compile AND RUN, exact value — the new
//        branch must not fire when there is no explicit list to measure.
//        Uses a TARGET-TYPED local (`var x: int := …`), matching #4590's own
//        T5, because plain `var x := …` (no target, no explicit list) against
//        an IMPORTED generic method was measured to hit an unrelated,
//        pre-existing argument-driven-inference defect across the module
//        boundary (reproduces identically with and without this fix, for
//        both a one- and a two-type-parameter method) — outside #4589's
//        scope, which is the EXPLICIT-list arity check only. Using the
//        untyped form here would pin the wrong bug.
//
// Every reject row matches the SPECIFIC diagnostic substring rather than
// `failedAtCompileTimeWithRealLocation`, which answers true on both sides of
// any crash-shaped failure (and the pre-fix behaviour here is not a crash —
// it is silent, clean acceptance — so it would be doubly vacuous).

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"
#include "test_harness.hpp"
#include "support/unique_stem.hpp"

namespace bezel::module {
namespace {

namespace fs = std::filesystem;

struct TempFile4589 {
    std::string path;
    explicit TempFile4589(std::string p) : path(std::move(p)) {}
    ~TempFile4589() { std::remove(path.c_str()); }
};

void writeSource4589(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

struct Compiled4589 {
    bool depOk = true;  // false => dependency toolchain unavailable
    bool success = false;
    std::string err;
};

// Run one compile with fd 2 redirected to a file, so BOTH the driver's
// std::cerr diagnostics and any llvm::errs() text are captured (the shape
// tests/module/xmod_return_target_inference_4590_test.cpp established).
template <typename Fn>
std::string captureStderr4589(Fn&& fn) {
    std::string errPath =
        (fs::temp_directory_path() /
         ("bz4589_err_" + unique_stem("") + ".txt")).string();
    std::fflush(stderr);
    std::cerr.flush();
    int saved = ::dup(STDERR_FILENO);
    int fd = ::open(errPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) ::dup2(fd, STDERR_FILENO);
    fn();
    std::fflush(stderr);
    std::cerr.flush();
    if (fd >= 0) {
        ::close(fd);
        ::dup2(saved, STDERR_FILENO);
    }
    ::close(saved);
    std::ifstream in(errPath);
    std::string out((std::istreambuf_iterator<char>(in)),
                    std::istreambuf_iterator<char>());
    std::remove(errPath.c_str());
    return out;
}

// Build `libSrc` to an object, then compile `appSrc` against it.
Compiled4589 compileImport4589(const std::string& libSrc,
                               const std::string& appSrc) {
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz4589_");
    TempFile4589 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile4589 aObj((dir / (stem + "_a.o")).string());
    writeSource4589(aSrc.path, libSrc);
    Compiled4589 r;
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success ||
            !std::ifstream(aObj.path).good()) {
            r.depOk = false;
            return r;
        }
    }
    TempFile4589 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile4589 bExe((dir / stem).string());
    writeSource4589(bSrc.path, appSrc);
    r.err = captureStderr4589([&] {
        bezel::Driver driver;
        std::vector<bezel::Dependency> deps;
        bezel::Dependency d;
        d.metadataPath = aObj.path;
        d.objectPath = aObj.path;
        deps.push_back(d);
        r.success =
            bezel::test::compileFileForTest(driver, bSrc.path, bExe.path, deps)
                .success;
    });
    return r;
}

// The one-module twin of the above — same capture, no dependency.
Compiled4589 compileLocal4589(const std::string& src) {
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz4589l_");
    TempFile4589 aSrc((dir / (stem + ".bzl")).string());
    TempFile4589 aExe((dir / stem).string());
    writeSource4589(aSrc.path, src);
    Compiled4589 r;
    r.err = captureStderr4589([&] {
        bezel::Driver driver;
        r.success =
            bezel::test::compileFileForTest(driver, aSrc.path, aExe.path)
                .success;
    });
    return r;
}

// Build the dependency, compile+link the importer, RUN it, return its exit
// code (-2 dependency unavailable, -1 compile/link/run failure).
int buildImportAndRun4589(const std::string& libSrc,
                          const std::string& mainSrc) {
    auto dir = fs::temp_directory_path();
    std::string id = unique_stem("bz4589r_");
    TempFile4589 libSrcF((dir / (id + "_lib.bzl")).string());
    TempFile4589 libObjF((dir / (id + "_lib.o")).string());
    TempFile4589 mainSrcF((dir / (id + "_main.bzl")).string());
    TempFile4589 mainExeF((dir / id).string());
    writeSource4589(libSrcF.path, libSrc);
    writeSource4589(mainSrcF.path, mainSrc);

    bezel::Driver driver;
    if (!driver.compileObject(libSrcF.path, libObjF.path).success) return -2;
    if (!std::ifstream(libObjF.path).good()) return -2;

    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = libObjF.path;
    d.objectPath = libObjF.path;
    deps.push_back(d);

    if (!bezel::test::compileFileForTest(driver, mainSrcF.path, mainExeF.path,
                                         deps).success)
        return -1;
    int status = bezel::test::runProgramForTest(mainExeF.path);
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// ---------------------------------------------------------------------------
// The dependency. One module carrying every shape the rows exercise.
// ---------------------------------------------------------------------------
//
//   U4589.Create4589<T>(): owned T              ONE type param   (T1,T2,T5)
//   U4589.Pick4589<TFirst,TSecond>(a,b): TFirst  TWO type params (T3,T4)
//   U4589.Echo4589<T>(x: T): T                   ONE type param   (T6)
const char* kDeclsBody4589 =
    "public class Widget4589 "
    "  public constructor() end constructor; "
    "  destructor end destructor; "
    "end Widget4589; "
    "public static class U4589 "
    "  public static method Create4589<T>(): owned T where T: class, new() "
    "    return new T(); "
    "  end Create4589; "
    "  public static method Pick4589<TFirst, TSecond>(a: TFirst, b: TSecond): "
    "      TFirst "
    "    return a; "
    "  end Pick4589; "
    "  public static method Echo4589<T>(x: T): T "
    "    return x; "
    "  end Echo4589; "
    "end U4589; ";

std::string dep4589() {
    return std::string("module Dep4589; ") + kDeclsBody4589;
}

// The importing module, parameterised by the body of `Main`.
std::string app4589(const std::string& tag, const std::string& mainBody) {
    return "module App4589" + tag + "; import Dep4589; " +
           "class C4589 public static method Main(): int " + mainBody +
           " end Main; end C4589;";
}

// The SINGLE-MODULE twin: the same declarations and the same `Main` body,
// with no module boundary anywhere.
std::string local4589(const std::string& tag, const std::string& mainBody) {
    return "module Loc4589" + tag + "; " + kDeclsBody4589 +
           "class C4589 public static method Main(): int " + mainBody +
           " end Main; end C4589;";
}

bool mentions4589(const Compiled4589& c, const char* needle) {
    return c.err.find(needle) != std::string::npos;
}

// §14.5, as the arity check emits it — the excess-list shape (2 supplied,
// 1 declared).
const char* kExcessHead4589 =
    "call to `Create4589` supplies 2 explicit type argument(s), but "
    "`Create4589` declares 1 type parameter(s)";
// The partial-list shape (1 supplied, 2 declared).
const char* kPartialHead4589 =
    "call to `Pick4589` supplies 1 explicit type argument(s), but "
    "`Pick4589` declares 2 type parameter(s)";
const char* kAllOrNothingTail4589 =
    "there are no partial lists";

// The `Main` bodies, shared between each cross-module row and its twin.
const char* kExcessBody4589 =
    "using var w := U4589.Create4589<Widget4589, int>(); return 9;";
const char* kPartialBody4589 =
    "var x := U4589.Pick4589<int>(1, \"s\"); return x;";

}  // namespace

// ===========================================================================
// T1 — ★ THE BUG (excess list). An imported generic method declares ONE type
// parameter; the call supplies TWO explicit type arguments. §14.5 says this
// is a compile error naming the mismatch; across a module boundary the
// diagnostic never fired, so the program COMPILED CLEAN.
// // per §14.5 "Explicit lists are all-or-nothing" / #1687, v0.306
// ===========================================================================
BEZEL_TEST(xmod_generic_method_explicit_arity_4589,
           imported_excess_type_argument_list_is_rejected) {
    auto r = compileImport4589(dep4589(), app4589("T1", kExcessBody4589));
    if (!r.depOk) return;  // dependency toolchain unavailable
    BEZEL_EXPECT_FALSE(r.success);
    BEZEL_EXPECT_TRUE(mentions4589(r, kExcessHead4589));
    BEZEL_EXPECT_TRUE(mentions4589(r, kAllOrNothingTail4589));
}

// ===========================================================================
// T2 — DIFFERENTIAL CONTROL for T1. The byte-identical declarations and call
// in ONE module. Rejected with the SAME text before AND after, so the module
// boundary is proved to be the whole difference T1 measures.
// // per §14.5 / #1687, v0.306
// ===========================================================================
BEZEL_TEST(xmod_generic_method_explicit_arity_4589,
           same_module_excess_twin_rejects_identically) {
    auto r = compileLocal4589(local4589("T2", kExcessBody4589));
    BEZEL_EXPECT_FALSE(r.success);
    BEZEL_EXPECT_TRUE(mentions4589(r, kExcessHead4589));
    BEZEL_EXPECT_TRUE(mentions4589(r, kAllOrNothingTail4589));
}

// ===========================================================================
// T3 — ★ THE BUG (partial list). An imported generic method declares TWO
// type parameters; the call supplies only ONE explicit type argument. §14.5:
// "a shorter … list is a compile error; there are no partial lists."
// // per §14.5 / #1687, v0.306
// ===========================================================================
BEZEL_TEST(xmod_generic_method_explicit_arity_4589,
           imported_partial_type_argument_list_is_rejected) {
    auto r = compileImport4589(dep4589(), app4589("T3", kPartialBody4589));
    if (!r.depOk) return;
    BEZEL_EXPECT_FALSE(r.success);
    BEZEL_EXPECT_TRUE(mentions4589(r, kPartialHead4589));
    BEZEL_EXPECT_TRUE(mentions4589(r, kAllOrNothingTail4589));
}

// ===========================================================================
// T4 — DIFFERENTIAL CONTROL for T3.
// // per §14.5 / #1687, v0.306
// ===========================================================================
BEZEL_TEST(xmod_generic_method_explicit_arity_4589,
           same_module_partial_twin_rejects_identically) {
    auto r = compileLocal4589(local4589("T4", kPartialBody4589));
    BEZEL_EXPECT_FALSE(r.success);
    BEZEL_EXPECT_TRUE(mentions4589(r, kPartialHead4589));
    BEZEL_EXPECT_TRUE(mentions4589(r, kAllOrNothingTail4589));
}

// ===========================================================================
// T5 — ANTI-FALSE-REJECT. A FULL, correct-arity explicit type-argument list
// against an imported generic method must still compile and RUN. The real
// risk of an under-rejection fix is over-correcting into a false reject of
// the arity-correct case.
// // per §14.5, v0.306
// ===========================================================================
BEZEL_TEST(xmod_generic_method_explicit_arity_4589,
           imported_correct_arity_explicit_list_still_accepted) {
    int rc = buildImportAndRun4589(
        dep4589(),
        app4589("T5", "using var w := U4589.Create4589<Widget4589>(); "
                      "return 9;"));
    if (rc == -2) return;  // dependency toolchain unavailable
    BEZEL_EXPECT_EQ(rc, 9);
}

// ===========================================================================
// T6 — ANTI-FALSE-REJECT. NO explicit type-argument list at all (ordinary
// argument-driven inference, §14.5 phase 1) against an imported generic
// method must still compile and RUN — the new both-sources branch must not
// fire when `callTypeArgs` is null/empty. Uses the single-type-parameter
// `Echo4589<T>` (see the header note above `kDeclsBody4589` for why not the
// two-parameter `Pick4589`).
// // per §14.5 phase 1, v0.306
// ===========================================================================
BEZEL_TEST(xmod_generic_method_explicit_arity_4589,
           imported_no_type_argument_list_still_inferred) {
    int rc = buildImportAndRun4589(
        dep4589(),
        app4589("T6",
                "var x: int := U4589.Echo4589(41); return x + 1;"));
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 42);
}

}  // namespace bezel::module
