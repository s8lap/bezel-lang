// Spec-derived regression test for issue #4611 — §14.5 PHASE 1 (plain
// argument-driven inference, with no target type and no explicit
// type-argument list) does not bind the type parameter for an IMPORTED
// generic method, so the call's result type comes back unresolved (`?`).
//
// spec §14.5 "Type-argument inference for generic methods", phase 1
// ("Argument-driven unification"), v0.307. §2.8.3 rule 2 (an imported
// artifact must not alter an instantiation's semantics relative to the same
// declarations in one module).
//
// ── THE RULE ───────────────────────────────────────────────────────────────
//
// §14.5 phase 1: "Each argument whose type is self-determined … is unified
// structurally against its parameter type … a parameter type that is a bare
// type parameter `X` binds `X` to the argument's static type".
//
// ── THE GAP ────────────────────────────────────────────────────────────────
//
// `ExprTC::visitCallExpr`'s phase-1 loop (src/sema/ExprTypecheck_Calls.cpp)
// lives inside `if (md && md->returnType)`, where `md` is the AST-side
// `ast::MethodDecl*`:
//
//   auto* md = dynamic_cast<const ast::MethodDecl*>(ms->decl);
//
// `ModuleLoader` never assigns `MethodSymbol::decl` for an imported method —
// the importing compilation has no AST for a dependency's method bodies at
// all — so `md` is null for EVERY imported generic method and the call falls
// into the separate `else if (!md && ctx_.isImported(ms))` arm, which set the
// expression type to the raw `ctx_.importedReturnType(ms)` with NO
// substitution of the method's own type parameters. The declared return `T`
// was installed verbatim, unbound, and rendered `?`.
//
// This is the third instance of the same AST-only gate as #4590 (§14.5
// phase 3) and #4589 (the explicit-list arity check). Phase 1 needs strictly
// more than #4589 did: not just the type-parameter COUNT but the imported
// FORMAL PARAMETER TYPES, reached through the both-sources accessor
// `formalParamResolvedType` (`ParameterSymbol::resolvedDeclType`, which the
// loader does populate) — verified to carry enough, so no wire change.
//
// ── MEASURED, on origin/main 53280062, with real two-module builds ────────
//    (`bezelc -c dep.bzl -o dep.o` then `bezelc --import dep.o -o app app.bzl`)
//
//   app.bzl:5:12: operator `+` is not defined for type `?` (§25); declare
//   `static method operator +(a: ?, b: ?)` inside `?` to overload it
//
// The byte-identical SINGLE-MODULE program compiled and ran, exit 42.
// Reproduced for a one-type-parameter and a two-type-parameter imported
// generic method alike. The `?` is the tell: the parameter never bound, so
// the failure surfaces at the first USE of the value rather than at the
// inference site.
//
// ── WHAT EACH ROW PINS ─────────────────────────────────────────────────────
//
//   T1 ★ THE BUG (one type parameter). `U4611.Echo4611(41)` against an
//        imported `Echo4611<T>(x: T): T`, no target, no explicit list ->
//        must COMPILE and RUN, exit 42. Pre-fix: rejected with the `?`
//        operator diagnostic above.
//   T2   DIFFERENTIAL CONTROL for T1. The byte-identical declarations and
//        call in ONE module — exit 42 before AND after, so the module
//        boundary is proved to be the whole difference.
//   T3 ★ THE BUG (two type parameters). `U4611.Pick4611(41, 7)` against an
//        imported `Pick4611<TFirst, TSecond>(a: TFirst, b: TSecond): TFirst`
//        -> exit 42. Both arguments are `int` so the call stays on the
//        placeholder-matching instantiation #1943's codegen decline admits;
//        a `string` second argument is declined there for an unrelated,
//        independently-measured reason (importer-side monomorphization) and
//        would pin the wrong bug.
//   T4   DIFFERENTIAL CONTROL for T3.
//   T5   ANTI-FALSE-ACCEPT. A genuinely UN-inferable call — a type parameter
//        that appears only in the RETURN type, called with no target — must
//        still be REJECTED, with its own specific §14.1/§A.7 diagnostic
//        (#4590's phase-3 reporting). The real risk of an under-acceptance
//        fix is over-correcting into a silent accept of the case the spec
//        says needs a target.
//   T6   DIFFERENTIAL CONTROL for T5: the same rejection in one module.
//   T7   NO-REGRESSION. An imported generic method whose type parameter does
//        NOT appear in the return type (`Count4611<T>(x: T): int`) must still
//        compile and run — the new substitution is a no-op there.
//   T8   NO-REGRESSION. The TARGET-TYPED spelling (`var x: int := …`), which
//        already worked before this fix, must still run and give 42 — the new
//        phase-1 arm must not disturb the path #4589's own T6 relies on.
//
// Every reject row matches the SPECIFIC diagnostic substring rather than
// `failedAtCompileTimeWithRealLocation`, which answers true on both sides of
// any crash-shaped failure. Every accept row asserts the exact RUNTIME VALUE,
// not merely "it compiled" — this is an under-acceptance fix, so "compiles"
// alone would not distinguish a correct binding from a wrong one.

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

struct TempFile4611 {
    std::string path;
    explicit TempFile4611(std::string p) : path(std::move(p)) {}
    ~TempFile4611() { std::remove(path.c_str()); }
};

void writeSource4611(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

struct Compiled4611 {
    bool depOk = true;  // false => dependency toolchain unavailable
    bool success = false;
    std::string err;
};

// Run one compile with fd 2 redirected to a file, so BOTH the driver's
// std::cerr diagnostics and any llvm::errs() text are captured (the shape
// tests/module/xmod_return_target_inference_4590_test.cpp established).
template <typename Fn>
std::string captureStderr4611(Fn&& fn) {
    std::string errPath =
        (fs::temp_directory_path() /
         ("bz4611_err_" + unique_stem("") + ".txt")).string();
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
Compiled4611 compileImport4611(const std::string& libSrc,
                               const std::string& appSrc) {
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz4611_");
    TempFile4611 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile4611 aObj((dir / (stem + "_a.o")).string());
    writeSource4611(aSrc.path, libSrc);
    Compiled4611 r;
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success ||
            !std::ifstream(aObj.path).good()) {
            r.depOk = false;
            return r;
        }
    }
    TempFile4611 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile4611 bExe((dir / stem).string());
    writeSource4611(bSrc.path, appSrc);
    r.err = captureStderr4611([&] {
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
Compiled4611 compileLocal4611(const std::string& src) {
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz4611l_");
    TempFile4611 aSrc((dir / (stem + ".bzl")).string());
    TempFile4611 aExe((dir / stem).string());
    writeSource4611(aSrc.path, src);
    Compiled4611 r;
    r.err = captureStderr4611([&] {
        bezel::Driver driver;
        r.success =
            bezel::test::compileFileForTest(driver, aSrc.path, aExe.path)
                .success;
    });
    return r;
}

// Build the dependency, compile+link the importer, RUN it, return its exit
// code (-2 dependency unavailable, -1 compile/link/run failure).
int buildImportAndRun4611(const std::string& libSrc,
                          const std::string& mainSrc) {
    auto dir = fs::temp_directory_path();
    std::string id = unique_stem("bz4611r_");
    TempFile4611 libSrcF((dir / (id + "_lib.bzl")).string());
    TempFile4611 libObjF((dir / (id + "_lib.o")).string());
    TempFile4611 mainSrcF((dir / (id + "_main.bzl")).string());
    TempFile4611 mainExeF((dir / id).string());
    writeSource4611(libSrcF.path, libSrc);
    writeSource4611(mainSrcF.path, mainSrc);

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
int buildLocalAndRun4611(const std::string& src) {
    auto dir = fs::temp_directory_path();
    std::string id = unique_stem("bz4611lr_");
    TempFile4611 srcF((dir / (id + ".bzl")).string());
    TempFile4611 exeF((dir / id).string());
    writeSource4611(srcF.path, src);
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
//   U4611.Echo4611<T>(x: T): T                      phase 1, 1 param (T1,T2,T8)
//   U4611.Pick4611<TFirst,TSecond>(a,b): TFirst     phase 1, 2 params (T3,T4)
//   U4611.Count4611<T>(x: T): int                   T not in the return (T7)
//   U4611.Create4611<T>(): owned T                  RETURN-ONLY T   (T5,T6)
const char* kDeclsBody4611 =
    "public class Widget4611 "
    "  public constructor() end constructor; "
    "  destructor end destructor; "
    "end Widget4611; "
    "public static class U4611 "
    "  public static method Echo4611<T>(x: T): T "
    "    return x; "
    "  end Echo4611; "
    "  public static method Pick4611<TFirst, TSecond>(a: TFirst, b: TSecond): "
    "      TFirst "
    "    return a; "
    "  end Pick4611; "
    "  public static method Count4611<T>(x: T): int "
    "    return 7; "
    "  end Count4611; "
    "  public static method Create4611<T>(): owned T where T: class, new() "
    "    return new T(); "
    "  end Create4611; "
    "end U4611; ";

std::string dep4611() {
    return std::string("module Dep4611; ") + kDeclsBody4611;
}

// The importing module, parameterised by the body of `Main`.
std::string app4611(const std::string& tag, const std::string& mainBody) {
    return "module App4611" + tag + "; import Dep4611; " +
           "class C4611 public static method Main(): int " + mainBody +
           " end Main; end C4611;";
}

// The SINGLE-MODULE twin: the same declarations and the same `Main` body,
// with no module boundary anywhere.
std::string local4611(const std::string& tag, const std::string& mainBody) {
    return "module Loc4611" + tag + "; " + kDeclsBody4611 +
           "class C4611 public static method Main(): int " + mainBody +
           " end Main; end C4611;";
}

bool mentions4611(const Compiled4611& c, const char* needle) {
    return c.err.find(needle) != std::string::npos;
}

// §14.1/§A.7, as `reportNoTargetInferenceIfNeeded` emits it (T5/T6).
const char* kNoTargetHead4611 =
    "cannot infer the type argument of `Create4611`'s call";
const char* kNoTargetTail4611 = "a target type is required";

// The `Main` bodies, shared between each cross-module row and its twin.
const char* kEchoBody4611 =
    "var x := U4611.Echo4611(41); return x + 1;";
const char* kPickBody4611 =
    "var x := U4611.Pick4611(41, 7); return x + 1;";
const char* kNoTargetBody4611 =
    "using var w := U4611.Create4611(); return 9;";

}  // namespace

// ===========================================================================
// T1 — ★ THE BUG (one type parameter). §14.5 phase 1: the argument `41` binds
// `T` to `int`, so `U4611.Echo4611(41)` is an `int` and `x + 1` is 42. Across
// a module boundary the binding never happened and the call typed as the
// unbound `T` — rejected with "operator `+` is not defined for type `?`".
// // per §14.5 phase 1 / §2.8.3 rule 2, v0.307
// ===========================================================================
BEZEL_TEST(xmod_phase1_arg_inference_4611,
           imported_single_type_param_infers_from_argument) {
    int rc = buildImportAndRun4611(dep4611(), app4611("T1", kEchoBody4611));
    if (rc == -2) return;  // dependency toolchain unavailable
    BEZEL_EXPECT_EQ(rc, 42);
}

// ===========================================================================
// T2 — DIFFERENTIAL CONTROL for T1. The byte-identical declarations and call
// in ONE module: 42 before AND after, so the module boundary is proved to be
// the whole difference T1 measures.
// // per §14.5 phase 1, v0.307
// ===========================================================================
BEZEL_TEST(xmod_phase1_arg_inference_4611,
           same_module_single_type_param_twin_runs_identically) {
    BEZEL_EXPECT_EQ(buildLocalAndRun4611(local4611("T2", kEchoBody4611)), 42);
}

// ===========================================================================
// T3 — ★ THE BUG (two type parameters). `TFirst` binds from `41` and
// `TSecond` from `7`; the declared return `TFirst` is therefore `int`.
// Both arguments are `int` deliberately — see the header note on #1943.
// // per §14.5 phase 1 / §2.8.3 rule 2, v0.307
// ===========================================================================
BEZEL_TEST(xmod_phase1_arg_inference_4611,
           imported_two_type_params_infer_from_arguments) {
    int rc = buildImportAndRun4611(dep4611(), app4611("T3", kPickBody4611));
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 42);
}

// ===========================================================================
// T4 — DIFFERENTIAL CONTROL for T3.
// // per §14.5 phase 1, v0.307
// ===========================================================================
BEZEL_TEST(xmod_phase1_arg_inference_4611,
           same_module_two_type_params_twin_runs_identically) {
    BEZEL_EXPECT_EQ(buildLocalAndRun4611(local4611("T4", kPickBody4611)), 42);
}

// ===========================================================================
// T5 — ANTI-FALSE-ACCEPT. A type parameter that appears ONLY in the return
// type cannot be solved by phase 1, and with no target type §14.5 phase 3
// has nothing to unify against: the call must still be REJECTED with the
// specific §14.1/§A.7 diagnostic (#4590). The new phase-1 arm must not turn
// this into a silent accept.
// // per §14.5 phase 3 / §14.1 / §A.7, v0.307
// ===========================================================================
BEZEL_TEST(xmod_phase1_arg_inference_4611,
           imported_return_only_type_param_without_target_still_rejected) {
    auto r = compileImport4611(dep4611(), app4611("T5", kNoTargetBody4611));
    if (!r.depOk) return;
    BEZEL_EXPECT_FALSE(r.success);
    BEZEL_EXPECT_TRUE(mentions4611(r, kNoTargetHead4611));
    BEZEL_EXPECT_TRUE(mentions4611(r, kNoTargetTail4611));
}

// ===========================================================================
// T6 — DIFFERENTIAL CONTROL for T5: the same rejection, same text, in one
// module.
// // per §14.5 phase 3 / §14.1 / §A.7, v0.307
// ===========================================================================
BEZEL_TEST(xmod_phase1_arg_inference_4611,
           same_module_return_only_twin_rejects_identically) {
    auto r = compileLocal4611(local4611("T6", kNoTargetBody4611));
    BEZEL_EXPECT_FALSE(r.success);
    BEZEL_EXPECT_TRUE(mentions4611(r, kNoTargetHead4611));
    BEZEL_EXPECT_TRUE(mentions4611(r, kNoTargetTail4611));
}

// ===========================================================================
// T7 — NO-REGRESSION. The imported method's type parameter does not appear in
// its return type, so phase 1 solves `T` but the substitution is a no-op and
// the call is an `int` either way. Must still compile and run, exact value.
// // per §14.5 phase 1, v0.307
// ===========================================================================
BEZEL_TEST(xmod_phase1_arg_inference_4611,
           imported_type_param_absent_from_return_still_runs) {
    int rc = buildImportAndRun4611(
        dep4611(),
        app4611("T7", "var n := U4611.Count4611(3); return n + 1;"));
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 8);
}

// ===========================================================================
// T8 — NO-REGRESSION. The TARGET-TYPED spelling already worked before this
// fix (it is what #4589's own T6 had to route through to avoid this bug);
// it must keep working, with the same value.
// // per §14.5 phase 1 / "Target-typed completion", v0.307
// ===========================================================================
BEZEL_TEST(xmod_phase1_arg_inference_4611,
           imported_target_typed_spelling_still_runs) {
    int rc = buildImportAndRun4611(
        dep4611(),
        app4611("T8", "var x: int := U4611.Echo4611(41); return x + 1;"));
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 42);
}

}  // namespace bezel::module
