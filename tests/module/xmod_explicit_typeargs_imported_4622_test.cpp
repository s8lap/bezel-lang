// Spec-derived regression test for issue #4622 — §14.5's EXPLICIT
// type-argument list does not bind the type parameter for an IMPORTED
// generic method, so the call's result type comes back unresolved (`?`).
//
// spec §14.5 "Type-argument inference for generic methods", v0.307. §2.8.3
// rule 2 (an imported artifact must not alter an instantiation's semantics
// relative to the same declarations in one module).
//
// ── THE RULE ───────────────────────────────────────────────────────────────
//
// §14.5: "An explicit type-argument list ... binds the declared type
// parameters in declaration order." "When a full explicit list is supplied,
// no inference runs."
//
// ── THE GAP ────────────────────────────────────────────────────────────────
//
// This is the sibling of #4611 (fixed), which covers §14.5 PHASE 1
// (argument-driven inference, no explicit list) for an imported generic
// method. #4611's fix deliberately left the EXPLICIT-LIST arm of the same
// `else if (!md && ctx_.isImported(ms))` branch in
// `ExprTC::visitCallExpr`/`typeCall` (src/sema/ExprTypecheck_Calls.cpp)
// alone: when the call spelled `<T, ...>`, the branch fell through with
// `callTy` still the raw, unsubstituted declared return type — the local
// (`md && md->returnType`) arm's explicit-list substitution
// (`explicitSubst[md->genericParams[gi].name] = ...` + `substituteReturnType`)
// has no imported-side counterpart at all. The declared return `T` was
// installed verbatim, unbound, and rendered `?`.
//
// ── MEASURED, on this lane's pre-fix HEAD, with real two-module builds ────
//    (`bezelc -c dep.bzl -o dep.o` then `bezelc --import dep.o -o app app.bzl`)
//
//   app.bzl:4:12: operator `+` is not defined for type `?` (§25); declare
//   `static method operator +(a: ?, b: ?)` inside `?` to overload it
//
// The byte-identical SINGLE-MODULE program compiled and ran, exit 42.
//
// ── WHAT EACH ROW PINS ─────────────────────────────────────────────────────
//
//   T1 ★ THE BUG (one type parameter). `U4622.Echo4622<int>(41)` against an
//        imported `Echo4622<T>(x: T): T` -> must COMPILE and RUN, exit 42.
//        Pre-fix: rejected with the `?` operator diagnostic above.
//   T2   DIFFERENTIAL CONTROL for T1: the byte-identical declarations and
//        call in ONE module — exit 42 before AND after, so the module
//        boundary is proved to be the whole difference.
//   T3 ★ THE BUG (two type parameters). `U4622.Pick4622<int, int>(41, 7)`
//        against an imported `Pick4622<TFirst, TSecond>(a, b): TFirst` ->
//        exit 42. Both arguments are `int` deliberately: a non-i32-width
//        cross-module generic-method argument fails IR verification for an
//        unrelated, independently-measured reason (#1943) and would pin the
//        wrong bug.
//   T4   DIFFERENTIAL CONTROL for T3.
//   T5   NO-REGRESSION. An imported generic method whose type parameter does
//        NOT appear in the return type (`Count4622<T>(x: T): int`), called
//        with an explicit list, must still compile and run — the new
//        substitution is a no-op there.
//   T6   DIFFERENTIAL CONTROL for T5.
//   T7   NO-REGRESSION. A MISMATCHED explicit-list arity
//        (`Echo4622<int, int>(41)` against a 1-type-parameter method) must
//        still be reported by #4589's existing both-sources arity check —
//        this fix's new binding loop sits right after that check and must
//        not disturb it, nor read past either vector's end when the counts
//        disagree.
//   T8   DIFFERENTIAL CONTROL for T7: the identical mismatch, same
//        diagnostic, in one module.
//
// Every reject row matches the SPECIFIC diagnostic substring rather than
// `failedAtCompileTimeWithRealLocation`, which answers true on both sides of
// any crash-shaped failure. Every accept row asserts the exact RUNTIME
// VALUE, not merely "it compiled" — this is an under-acceptance fix, so
// "compiles" alone would not distinguish a correct binding from a wrong one.

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

struct TempFile4622 {
    std::string path;
    explicit TempFile4622(std::string p) : path(std::move(p)) {}
    ~TempFile4622() { std::remove(path.c_str()); }
};

void writeSource4622(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

struct Compiled4622 {
    bool depOk = true;  // false => dependency toolchain unavailable
    bool success = false;
    std::string err;
};

// Run one compile with fd 2 redirected to a file, so BOTH the driver's
// std::cerr diagnostics and any llvm::errs() text are captured (the shape
// tests/module/xmod_phase1_arg_inference_4611_test.cpp established).
template <typename Fn>
std::string captureStderr4622(Fn&& fn) {
    std::string errPath =
        (fs::temp_directory_path() /
         ("bz4622_err_" + unique_stem("") + ".txt")).string();
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
Compiled4622 compileImport4622(const std::string& libSrc,
                               const std::string& appSrc) {
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz4622_");
    TempFile4622 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile4622 aObj((dir / (stem + "_a.o")).string());
    writeSource4622(aSrc.path, libSrc);
    Compiled4622 r;
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success ||
            !std::ifstream(aObj.path).good()) {
            r.depOk = false;
            return r;
        }
    }
    TempFile4622 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile4622 bExe((dir / stem).string());
    writeSource4622(bSrc.path, appSrc);
    r.err = captureStderr4622([&] {
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
Compiled4622 compileLocal4622(const std::string& src) {
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz4622l_");
    TempFile4622 aSrc((dir / (stem + ".bzl")).string());
    TempFile4622 aExe((dir / stem).string());
    writeSource4622(aSrc.path, src);
    Compiled4622 r;
    r.err = captureStderr4622([&] {
        bezel::Driver driver;
        r.success =
            bezel::test::compileFileForTest(driver, aSrc.path, aExe.path)
                .success;
    });
    return r;
}

// Build the dependency, compile+link the importer, RUN it, return its exit
// code (-2 dependency unavailable, -1 compile/link/run failure).
int buildImportAndRun4622(const std::string& libSrc,
                          const std::string& mainSrc) {
    auto dir = fs::temp_directory_path();
    std::string id = unique_stem("bz4622r_");
    TempFile4622 libSrcF((dir / (id + "_lib.bzl")).string());
    TempFile4622 libObjF((dir / (id + "_lib.o")).string());
    TempFile4622 mainSrcF((dir / (id + "_main.bzl")).string());
    TempFile4622 mainExeF((dir / id).string());
    writeSource4622(libSrcF.path, libSrc);
    writeSource4622(mainSrcF.path, mainSrc);

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

// Compile and RUN a single-module program, same return convention (minus the
// -2 case, there being no dependency).
int buildLocalAndRun4622(const std::string& src) {
    auto dir = fs::temp_directory_path();
    std::string id = unique_stem("bz4622lr_");
    TempFile4622 srcF((dir / (id + ".bzl")).string());
    TempFile4622 exeF((dir / id).string());
    writeSource4622(srcF.path, src);
    bezel::Driver driver;
    if (!bezel::test::compileFileForTest(driver, srcF.path, exeF.path).success)
        return -1;
    int status = bezel::test::runProgramForTest(exeF.path);
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// ---------------------------------------------------------------------------
// The dependency. One module carrying every shape the rows exercise.
// ---------------------------------------------------------------------------
//
//   U4622.Echo4622<T>(x: T): T                       explicit list, 1 param
//   U4622.Pick4622<TFirst, TSecond>(a, b): TFirst    explicit list, 2 params
//   U4622.Count4622<T>(x: T): int                    T not in the return
const char* kDeclsBody4622 =
    "public static class U4622 "
    "  public static method Echo4622<T>(x: T): T "
    "    return x; "
    "  end Echo4622; "
    "  public static method Pick4622<TFirst, TSecond>(a: TFirst, b: TSecond): "
    "      TFirst "
    "    return a; "
    "  end Pick4622; "
    "  public static method Count4622<T>(x: T): int "
    "    return 7; "
    "  end Count4622; "
    "end U4622; ";

std::string dep4622() {
    return std::string("module Dep4622; ") + kDeclsBody4622;
}

// The importing module, parameterised by the body of `Main`.
std::string app4622(const std::string& tag, const std::string& mainBody) {
    return "module App4622" + tag + "; import Dep4622; " +
           "class C4622 public static method Main(): int " + mainBody +
           " end Main; end C4622;";
}

// The SINGLE-MODULE twin: the same declarations and the same `Main` body,
// with no module boundary anywhere.
std::string local4622(const std::string& tag, const std::string& mainBody) {
    return "module Loc4622" + tag + "; " + kDeclsBody4622 +
           "class C4622 public static method Main(): int " + mainBody +
           " end Main; end C4622;";
}

bool mentions4622(const Compiled4622& c, const char* needle) {
    return c.err.find(needle) != std::string::npos;
}

// §14.5 "there are no partial lists", as #4589's arity check emits it.
const char* kArityMismatchNeedle4622 =
    "must supply exactly as many arguments as the method declares type "
    "parameters";

// The `Main` bodies, shared between each cross-module row and its twin.
const char* kEchoBody4622 =
    "var x := U4622.Echo4622<int>(41); return x + 1;";
const char* kPickBody4622 =
    "var x := U4622.Pick4622<int, int>(41, 7); return x + 1;";
const char* kCountBody4622 =
    "var n := U4622.Count4622<int>(3); return n + 1;";
// Two explicit type args against a 1-type-parameter method — a mismatch.
const char* kArityMismatchBody4622 =
    "var x := U4622.Echo4622<int, int>(41); return x + 1;";

}  // namespace

// ===========================================================================
// T1 — ★ THE BUG (one type parameter). §14.5: the explicit list `<int>`
// binds `T` to `int`, so `U4622.Echo4622<int>(41)` is an `int` and `x + 1`
// is 42. Across a module boundary the binding never happened and the call
// typed as the unbound `T` — rejected with "operator `+` is not defined for
// type `?`".
// // per §14.5 / §2.8.3 rule 2, v0.307
// ===========================================================================
BEZEL_TEST(xmod_explicit_typeargs_imported_4622,
           imported_single_type_param_explicit_list_binds) {
    int rc = buildImportAndRun4622(dep4622(), app4622("T1", kEchoBody4622));
    if (rc == -2) return;  // dependency toolchain unavailable
    BEZEL_EXPECT_EQ(rc, 42);
}

// ===========================================================================
// T2 — DIFFERENTIAL CONTROL for T1. The byte-identical declarations and call
// in ONE module: 42 before AND after, so the module boundary is proved to be
// the whole difference T1 measures.
// // per §14.5, v0.307
// ===========================================================================
BEZEL_TEST(xmod_explicit_typeargs_imported_4622,
           same_module_single_type_param_explicit_list_twin_runs_identically) {
    BEZEL_EXPECT_EQ(buildLocalAndRun4622(local4622("T2", kEchoBody4622)), 42);
}

// ===========================================================================
// T3 — ★ THE BUG (two type parameters). The explicit list `<int, int>` binds
// `TFirst` and `TSecond` in declaration order; the declared return `TFirst`
// is therefore `int`. Both arguments are `int` deliberately — see the header
// note on #1943.
// // per §14.5 / §2.8.3 rule 2, v0.307
// ===========================================================================
BEZEL_TEST(xmod_explicit_typeargs_imported_4622,
           imported_two_type_params_explicit_list_binds) {
    int rc = buildImportAndRun4622(dep4622(), app4622("T3", kPickBody4622));
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 42);
}

// ===========================================================================
// T4 — DIFFERENTIAL CONTROL for T3.
// // per §14.5, v0.307
// ===========================================================================
BEZEL_TEST(xmod_explicit_typeargs_imported_4622,
           same_module_two_type_params_explicit_list_twin_runs_identically) {
    BEZEL_EXPECT_EQ(buildLocalAndRun4622(local4622("T4", kPickBody4622)), 42);
}

// ===========================================================================
// T5 — NO-REGRESSION. The imported method's type parameter does not appear
// in its return type, so the explicit-list substitution is a no-op and the
// call is an `int` either way, both before and after this fix.
// // per §14.5, v0.307
// ===========================================================================
BEZEL_TEST(xmod_explicit_typeargs_imported_4622,
           imported_type_param_absent_from_return_still_runs) {
    int rc = buildImportAndRun4622(dep4622(), app4622("T5", kCountBody4622));
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 8);
}

// ===========================================================================
// T6 — DIFFERENTIAL CONTROL for T5.
// // per §14.5, v0.307
// ===========================================================================
BEZEL_TEST(xmod_explicit_typeargs_imported_4622,
           same_module_type_param_absent_from_return_twin_runs_identically) {
    BEZEL_EXPECT_EQ(buildLocalAndRun4622(local4622("T6", kCountBody4622)), 8);
}

// ===========================================================================
// T7 — NO-REGRESSION. A MISMATCHED explicit-list arity (2 type arguments
// against a 1-type-parameter method) must still be rejected by #4589's
// existing both-sources arity check (§14.5 "there are no partial lists")
// across the module boundary — this fix's new binding loop sits right after
// that check and must not disturb it, nor read past either vector's end.
// // per §14.5 "Explicit lists are all-or-nothing", v0.307
// ===========================================================================
BEZEL_TEST(xmod_explicit_typeargs_imported_4622,
           imported_explicit_list_arity_mismatch_still_rejected) {
    auto r = compileImport4622(dep4622(), app4622("T7", kArityMismatchBody4622));
    if (!r.depOk) return;
    BEZEL_EXPECT_FALSE(r.success);
    BEZEL_EXPECT_TRUE(mentions4622(r, kArityMismatchNeedle4622));
}

// ===========================================================================
// T8 — DIFFERENTIAL CONTROL for T7: the identical mismatch, same diagnostic,
// in one module.
// // per §14.5, v0.307
// ===========================================================================
BEZEL_TEST(xmod_explicit_typeargs_imported_4622,
           same_module_explicit_list_arity_mismatch_twin_rejects_identically) {
    auto r = compileLocal4622(local4622("T8", kArityMismatchBody4622));
    BEZEL_EXPECT_FALSE(r.success);
    BEZEL_EXPECT_TRUE(mentions4622(r, kArityMismatchNeedle4622));
}

}  // namespace bezel::module
