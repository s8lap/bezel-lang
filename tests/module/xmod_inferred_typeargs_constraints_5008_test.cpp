// Spec-derived regression test for issue #5008 — the #4624 residual: an
// IMPORTED generic method's `where`-constraints (§14.2) and §30.1 async
// borrow-bearing re-check are UNCHECKED against an ARGUMENT-INFERRED type
// substitution (no explicit `<T>`).
//
// spec §14.5 "Type-argument inference for generic methods", phase 1
// ("Argument-driven unification"), §14.2 (generic method `where`-
// constraints, re-checked at every call site), §30.1 (async borrow-bearing
// re-applied at the instantiation site), v0.307.
// §2.8.3 rule 2 (an imported artifact must not alter an instantiation's
// semantics relative to the same declarations in one module).
//
// ── THE RULE ───────────────────────────────────────────────────────────────
//
// §14.5 closing paragraph: "Constraints. §14.2 constraint satisfaction is
// checked after inference completes, on the full substitution, exactly as
// for a written type-argument list."
//
// §14.2: a generic method's `where T: I` constraint is enforced again at
// EVERY call site, not only at the declaration. §30.1 / #1595: "for a generic
// async method the check is re-applied after substitution, so an
// instantiation whose type argument makes a parameter borrow-bearing is
// rejected at the instantiation site."
//
// ── THE GAP ────────────────────────────────────────────────────────────────
//
// #4624 wired `checkMethodGenericConstraints` and
// `checkAsyncInstantiationBorrowBearing` into the IMPORTED arm's
// **explicit**-list branch (`ExprTC::typeCall`'s `!md && ctx_.isImported(ms)`
// arm, src/sema/ExprTypecheck_Calls.cpp ~L5685). #4611 wired argument-driven
// inference (§14.5 phase 1) into the IMPORTED arm's inference branch
// (~L5560), so a call like `U.Echo(41)` now binds T = `int`. Both fixes
// stopped short of calling the two checks against the argument-INFERRED
// substitution: the explicit arm is the only call site that calls them, and
// the inference arm substitutes the return type and stops. The local arm
// (~L5210) already does the right thing — the imported arm must mirror it.
//
// ── THE FIX ────────────────────────────────────────────────────────────────
//
// Immediately after the `if (consistent && !argSubst.empty()) callTy =
// substituteReturnType(rt, argSubst);` line in the imported argument-INFERRED
// branch (~L5600), call `checkMethodGenericConstraints(argSubst)` and
// `checkAsyncInstantiationBorrowBearing(argSubst)`. Both closures were
// widened by #4624 to read through both-sources accessors (they no longer
// require `md`), so the calls are drop-in.
//
// ── MEASURED, on this lane's pre-fix HEAD (b30c6f12), with real two-module
//    builds (`bezelc -c dep.bzl -o dep.o` then
//    `bezelc --import dep.o -o app app.bzl`) ─────────────────────────────
//
//   WC1 (where-constraint violation): compiles cleanly and RUNS — the
//   constraint is never checked across the module boundary for the inferred
//   argument type. Post-fix: rejected with the §14.2 "does not satisfy the
//   constraint" diagnostic, identically to the single-module twin (WC2) and
//   to #4624's explicit-list twin (the same shape, different call spelling).
//
//   AB1 (async borrow-bearing violation): compiles cleanly. Post-fix:
//   rejected with the §30.1 "makes parameter `x` borrow-bearing" diagnostic,
//   identically to the single-module twin (AB2).
//
// ── WHAT EACH ROW PINS ─────────────────────────────────────────────────────
//
//   WC1 ★ THE BUG. `U5008.Gen5008(n)` (no explicit `<T>`) against an imported
//        `Gen5008<T>(v: T): int where T: IShape5008`, `n: NotShape5008` NOT
//        implementing `IShape5008` -> must be REJECTED (§14.2).
//   WC2   DIFFERENTIAL CONTROL for WC1: the byte-identical declarations and
//        call in ONE module -> rejected before AND after this fix, so the
//        module boundary is proved to be the whole difference WC1 measures.
//   WC3   NO-REGRESSION ACCEPT. `U5008.GenNum5008(5)` (no explicit `<T>`)
//        against an imported `GenNum5008<T>(v: T): int where T: INumber<T>`,
//        `5` self-determined as `int`, conforming to the builtin `INumber<T>`
//        abstraction -> must COMPILE and RUN, exit 4.
//   WC4   DIFFERENTIAL CONTROL for WC3.
//   AB1 ★ THE BUG. `U5008.Work5008(h)` (no explicit `<T>`) against an
//        imported `async method Work5008<T>(x: T): Task<T>`, `h: Holder5008`
//        a record wrapping a class reference (borrow-bearing, §35.4) -> must
//        be REJECTED (§30.1).
//   AB2   DIFFERENTIAL CONTROL for AB1.
//   AB3   NO-REGRESSION ACCEPT. `U5008.Work5008(5)` -> `int` is never
//        borrow-bearing -> must compile cleanly (no spurious rejection).
//   AB4   DIFFERENTIAL CONTROL for AB3.
//
// Every reject row matches the SPECIFIC diagnostic substring rather than
// `failedAtCompileTimeWithRealLocation`, which answers true on both sides of
// any crash-shaped failure. WC3/WC4 assert the exact RUNTIME VALUE, not
// merely "it compiled" — this is an under-rejection fix, so "rejected" alone
// would not distinguish a correct new check from an over-broad one that also
// wrongly rejects the conforming case.

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

struct TempFile5008 {
    std::string path;
    explicit TempFile5008(std::string p) : path(std::move(p)) {}
    ~TempFile5008() { std::remove(path.c_str()); }
};

void writeSource5008(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

struct Compiled5008 {
    bool depOk = true;  // false => dependency toolchain unavailable
    bool success = false;
    std::string err;
};

// Run one compile with fd 2 redirected to a file, so BOTH the driver's
// std::cerr diagnostics and any llvm::errs() text are captured (the shape
// tests/module/xmod_phase1_arg_inference_4611_test.cpp established).
template <typename Fn>
std::string captureStderr5008(Fn&& fn) {
    std::string errPath =
        (fs::temp_directory_path() /
         ("bz5008_err_" + unique_stem("") + ".txt")).string();
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
Compiled5008 compileImport5008(const std::string& libSrc,
                               const std::string& appSrc) {
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz5008_");
    TempFile5008 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile5008 aObj((dir / (stem + "_a.o")).string());
    writeSource5008(aSrc.path, libSrc);
    Compiled5008 r;
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success ||
            !std::ifstream(aObj.path).good()) {
            r.depOk = false;
            return r;
        }
    }
    TempFile5008 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile5008 bExe((dir / stem).string());
    writeSource5008(bSrc.path, appSrc);
    r.err = captureStderr5008([&] {
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
Compiled5008 compileLocal5008(const std::string& src) {
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz5008l_");
    TempFile5008 aSrc((dir / (stem + ".bzl")).string());
    TempFile5008 aExe((dir / stem).string());
    writeSource5008(aSrc.path, src);
    Compiled5008 r;
    r.err = captureStderr5008([&] {
        bezel::Driver driver;
        r.success =
            bezel::test::compileFileForTest(driver, aSrc.path, aExe.path)
                .success;
    });
    return r;
}

// Build the dependency, compile+link the importer, RUN it, return its exit
// code (-2 dependency unavailable, -1 compile/link/run failure).
int buildImportAndRun5008(const std::string& libSrc,
                          const std::string& mainSrc) {
    auto dir = fs::temp_directory_path();
    std::string id = unique_stem("bz5008r_");
    TempFile5008 libSrcF((dir / (id + "_lib.bzl")).string());
    TempFile5008 libObjF((dir / (id + "_lib.o")).string());
    TempFile5008 mainSrcF((dir / (id + "_main.bzl")).string());
    TempFile5008 mainExeF((dir / id).string());
    writeSource5008(libSrcF.path, libSrc);
    writeSource5008(mainSrcF.path, mainSrc);

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
int buildLocalAndRun5008(const std::string& src) {
    auto dir = fs::temp_directory_path();
    std::string id = unique_stem("bz5008lr_");
    TempFile5008 srcF((dir / (id + ".bzl")).string());
    TempFile5008 exeF((dir / id).string());
    writeSource5008(srcF.path, src);
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
//   IShape5008 / Square5008 (conforms) / NotShape5008 (does not conform)
//   Res5008 / Holder5008(R: Res5008) — a record wrapping a class reference,
//     borrow-bearing per §35.4 (the same shape
//     tests/lit/sema/async_generic_instantiation_1595__param_explicit_record_rejected.bzl
//     uses for the single-module rejection).
//   U5008.Gen5008<T>(v: T): int where T: IShape5008   §14.2 inferred check
//   U5008.GenNum5008<T>(v: T): int where T: INumber<T> §14.2 inferred accept
//   U5008.Work5008<T>(x: T): Task<T>, async            §30.1 inferred check
//
// Note: no `<...>` at the call sites below — that is the whole point of this
// fix. The argument's static type infers T, then §14.2 and §30.1 must run
// against that inferred substitution.
const char* kDeclsBody5008 =
    "public interface IShape5008 method Area(): int; end IShape5008; "
    "public class Square5008: IShape5008 "
    "  public method Area(): int => 4; "
    "end Square5008; "
    "public class NotShape5008 end NotShape5008; "
    "public class Res5008 end Res5008; "
    "public record Holder5008(R: Res5008); "
    "public static class U5008 "
    // `Gen5008`'s body returns a constant rather than dispatching
    // `v.Area()` through the constrained type parameter: WC1/WC2 only
    // exercise REJECTION (the constraint check runs and fails before
    // codegen is ever reached), and compiling `v.Area()` inside a
    // generic method's shared/erased cross-module template independently
    // degrades to a zero/null return with an internal `#2491` codegen
    // warning (pre-existing, orthogonal to §14.2/§30.1 — verified by
    // removing the dispatch here, which silences it) — unrelated noise
    // this test must not carry.
    "  public static method Gen5008<T>(v: T): int where T: IShape5008 "
    "    return 1; "
    "  end Gen5008; "
    // fix #1943 family (§14.1 / §2.8.3): calling an imported generic method
    // with a type argument whose representation is not the compiled `int`
    // (i32) placeholder is not yet monomorphized across the module
    // boundary — an unrelated, independently-measured pre-existing gap
    // (see xmod_generic_method_arg_marshalling_1943_test.cpp). The
    // WC3/WC4 no-regression ACCEPT+RUN control below therefore uses an
    // `int` type argument against a satisfied BUILTIN-abstraction
    // constraint (`INumber<T>`, stdlib §2.19 / §11.8) instead of the
    // user-interface `IShape5008` conformance WC1/WC2 exercise — that
    // stays within the cross-module ABI's supported width while still
    // exercising `checkMethodGenericConstraints`'s accept path for an
    // IMPORTED method's inferred type-argument substitution.
    "  public static method GenNum5008<T>(v: T): int where T: INumber<T> "
    "    return 4; "
    "  end GenNum5008; "
    "  public static async method Work5008<T>(x: T): Task<T> "
    "    return x; "
    "  end Work5008; "
    "end U5008; ";

std::string dep5008() {
    return std::string("module Dep5008; ") + kDeclsBody5008;
}

// The importing module, parameterised by the body of `Main`.
std::string app5008(const std::string& tag, const std::string& mainBody) {
    return "module App5008" + tag + "; import Dep5008; " +
           "public class C5008 public static method Main(): int " +
           mainBody + " end Main; end C5008;";
}

// The SINGLE-MODULE twin: the same declarations and the same `Main` body,
// with no module boundary anywhere.
std::string local5008(const std::string& tag, const std::string& mainBody) {
    return "module Loc5008" + tag + "; " + kDeclsBody5008 +
           "public class C5008 public static method Main(): int " +
           mainBody + " end Main; end C5008;";
}

bool mentions5008(const Compiled5008& c, const char* needle) {
    return c.err.find(needle) != std::string::npos;
}

// §14.2, as `checkMethodGenericConstraints`'s `report` lambda emits it for
// a "type" (interface/class bound) constraint.
const char* kConstraintNeedle5008 =
    "does not satisfy the constraint `where T: IShape5008`";

// §30.1 / §35.4, as `checkAsyncInstantiationBorrowBearing` emits it for a
// borrow-bearing PARAMETER.
const char* kAsyncBorrowNeedle5008 =
    "instantiating `async` method `Work5008` with this type argument makes "
    "parameter `x` borrow-bearing";

// The `Main` bodies, shared between each cross-module row and its twin.
// NB: NO `<...>` after the method name — these are argument-INFERRED calls.
const char* kConstraintRejectBody5008 =
    "var n := new NotShape5008(); return U5008.Gen5008(n);";
const char* kConstraintAcceptBody5008 =
    "var x := U5008.GenNum5008(5); return x;";
const char* kAsyncRejectBody5008 =
    "var res := new Res5008(); var h := new Holder5008(res); "
    "var t := U5008.Work5008(h); return 0;";
const char* kAsyncAcceptBody5008 =
    "using var t := U5008.Work5008(5); return 0;";

}  // namespace

// ===========================================================================
// WC1 — ★ THE BUG. §14.2: `NotShape5008` does not implement `IShape5008`, so
// the argument-INFERRED call `U5008.Gen5008(n)` (T inferred to
// `NotShape5008`) must be rejected.
// Pre-fix: accepted silently across the module boundary (the constraint was
// never checked for an imported method's inferred substitution).
// // per §14.2 / §14.5 phase 1 / §2.8.3 rule 2, v0.307
// ===========================================================================
BEZEL_TEST(xmod_inferred_typeargs_constraints_5008,
           imported_where_constraint_violation_inferred_rejected) {
    auto r = compileImport5008(dep5008(),
                               app5008("WC1", kConstraintRejectBody5008));
    if (!r.depOk) return;  // dependency toolchain unavailable
    BEZEL_EXPECT_FALSE(r.success);
    BEZEL_EXPECT_TRUE(mentions5008(r, kConstraintNeedle5008));
}

// ===========================================================================
// WC2 — DIFFERENTIAL CONTROL for WC1. The byte-identical declarations and
// call in ONE module: rejected before AND after this fix, so the module
// boundary is proved to be the whole difference WC1 measures.
// // per §14.2, v0.307
// ===========================================================================
BEZEL_TEST(xmod_inferred_typeargs_constraints_5008,
           same_module_where_constraint_violation_twin_rejects_identically) {
    auto r = compileLocal5008(local5008("WC2", kConstraintRejectBody5008));
    BEZEL_EXPECT_FALSE(r.success);
    BEZEL_EXPECT_TRUE(mentions5008(r, kConstraintNeedle5008));
}

// ===========================================================================
// WC3 — NO-REGRESSION ACCEPT. `int` conforms to the builtin `INumber<T>`
// abstraction, so the argument-INFERRED call `U5008.GenNum5008(5)` (T
// inferred to `int`) must still compile and RUN, exit 4 — proving the
// newly-wired constraint check does not spuriously reject a satisfied
// constraint for an imported method's inferred substitution.
// (Uses a builtin-interface-satisfied `int` rather than a class type to stay
// within the pre-existing #1943-family cross-module monomorphization-width
// limit — see the comment on `GenNum5008` in `kDeclsBody5008`.)
// // per §14.2, v0.307
// ===========================================================================
BEZEL_TEST(xmod_inferred_typeargs_constraints_5008,
           imported_where_constraint_conforming_inferred_runs) {
    int rc = buildImportAndRun5008(dep5008(),
                                   app5008("WC3", kConstraintAcceptBody5008));
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 4);
}

// ===========================================================================
// WC4 — DIFFERENTIAL CONTROL for WC3.
// // per §14.2, v0.307
// ===========================================================================
BEZEL_TEST(xmod_inferred_typeargs_constraints_5008,
           same_module_where_constraint_conforming_twin_runs_identically) {
    BEZEL_EXPECT_EQ(
        buildLocalAndRun5008(local5008("WC4", kConstraintAcceptBody5008)), 4);
}

// ===========================================================================
// AB1 — ★ THE BUG. §30.1/§35.4: `Holder5008` wraps a class reference
// (`Res5008`), making it borrow-bearing; the argument-INFERRED call
// `U5008.Work5008(h)` (T inferred to `Holder5008`) must be rejected at the
// instantiation site. Pre-fix: accepted silently across the module boundary.
// // per §30.1 / §35.4 / §14.5 phase 1 / §2.8.3 rule 2, v0.307
// ===========================================================================
BEZEL_TEST(xmod_inferred_typeargs_constraints_5008,
           imported_async_borrow_bearing_inferred_rejected) {
    auto r =
        compileImport5008(dep5008(), app5008("AB1", kAsyncRejectBody5008));
    if (!r.depOk) return;
    BEZEL_EXPECT_FALSE(r.success);
    BEZEL_EXPECT_TRUE(mentions5008(r, kAsyncBorrowNeedle5008));
}

// ===========================================================================
// AB2 — DIFFERENTIAL CONTROL for AB1.
// // per §30.1 / §35.4, v0.307
// ===========================================================================
BEZEL_TEST(xmod_inferred_typeargs_constraints_5008,
           same_module_async_borrow_bearing_twin_rejects_identically) {
    auto r = compileLocal5008(local5008("AB2", kAsyncRejectBody5008));
    BEZEL_EXPECT_FALSE(r.success);
    BEZEL_EXPECT_TRUE(mentions5008(r, kAsyncBorrowNeedle5008));
}

// ===========================================================================
// AB3 — NO-REGRESSION ACCEPT. `int` is never borrow-bearing, so the
// argument-INFERRED call must still compile cleanly — no spurious rejection
// from the newly-wired check.
// // per §30.1, v0.307
// ===========================================================================
BEZEL_TEST(xmod_inferred_typeargs_constraints_5008,
           imported_async_non_borrow_bearing_inferred_still_compiles) {
    auto r =
        compileImport5008(dep5008(), app5008("AB3", kAsyncAcceptBody5008));
    if (!r.depOk) return;
    BEZEL_EXPECT_TRUE(r.success);
}

// ===========================================================================
// AB4 — DIFFERENTIAL CONTROL for AB3.
// // per §30.1, v0.307
// ===========================================================================
BEZEL_TEST(xmod_inferred_typeargs_constraints_5008,
           same_module_async_non_borrow_bearing_twin_compiles_identically) {
    auto r = compileLocal5008(local5008("AB4", kAsyncAcceptBody5008));
    BEZEL_EXPECT_TRUE(r.success);
}

}  // namespace bezel::module
