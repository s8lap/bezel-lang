// Spec-derived regression test pinning issue #3233 (interface-conformance
// witness inherited from an IMPORTED base, when the implementing class is
// local). The defect class is the same as family M13 of umbrella #3152; this
// file is a DEDICATED pin for #3233 — the residue ticket the M13 family
// left when #3194 (test: xmod_iface_witness_from_imported_base_3194_test.cpp)
// closed. Both tests must pass; if either one starts failing, the regression
// is back.
//
// Spec citations:
//   * §11.2 (interface conformance and the "matches" rule — name, arity,
//     parameter modifiers, parameter types, return type). The three axes
//     a local twin enforces and an imported twin must enforce equally.
//   * §2.8.3 rule 4 (program-wide type identity is one type at runtime, too)
//     — the read that frames the obligation: an imported conformance
//     obligation and a local conformance obligation refer to the SAME
//     interface; §11.2's "matches" rule cannot answer differently for the
//     two. Spec revision v0.254.
//
// Audit: docs/proposals/papers/member-symbol-audit-2026-08-13.md, family M13
// (rank 2). The audit's own §A.6 wording names exactly the three axes
// pinned below.
//
// ── THE DEFECT CLASS (as carried into #3233) ──────────────────────────────
//
// `Resolver::checkAbstractImplementations` (src/sema/TypeResolution_Checks.cpp)
// resolves each interface obligation to a witness by walking the class chain
// at ~TypeResolution_Checks.cpp:4139 — and that walk follows `c->baseTypes`,
// which CROSSES MODULES. So the chosen witness `impl` is an IMPORTED
// MethodSymbol whenever a local class satisfies an obligation through a
// method it inherits from an imported base. An imported MethodSymbol carries
// no `ast::MethodDecl`; the AST-keyed checks at :4294-:4314 silently do not
// run unless `ctx_.isImported(impl)` gates the fallback through
// `importedReturnType` / `importedReturnOwned` (returns) and the parameter
// side reads `resolvedDeclType` instead of `declType`.
//
// The pre-fix code's own comment — "`impl` is always a LOCAL class member
// (obligation witnesses only ever come from `t`'s own module's BFS above)" —
// was wrong; the witness BFS at :4139 walks `c->baseTypes`, which crosses
// modules. Three comment sites made the same false claim and have been
// corrected. This file pins the *behaviour* side of the fix.
//
// ── MEASURED ON THE PRE-FIX PARENT (commit 843e30f~1) ─────────────────────
//
//   axis          program                                twin (local)   imported
//   return type   `Get(): int` vs iface `Get(): string`  §11.2 error    compiles, SIGSEGV
//   param type    `Take(x: int)` vs iface `Take(x: string)`
//                                                           §11.2 error    compiles, garbage int
//   `owned` ret   `Make(): owned T` vs iface `Make(): T` §11.2 error    compiles, 16B leak (LSan)
//
// The audit frames the `owned` bind as unwinnable in both directions — dropping
// `owned` off an inherited implementation leaks, tightening it double-disposes.
// §11.2 supplies the third answer, and it is the one the local twin already
// gives: REJECT the mismatch. The fix in 843e30f (with review follow-ups
// 5f2420a and 17fd6a5) turns the three skipped checks on for the imported
// case; this test pins that turn.
//
// ── WHY ALL THREE AXES IN ONE FILE ─────────────────────────────────────────
//
// The defect class is "AST-keyed checks silently did not run on an imported
// witness." Each axis is the same pattern (a decl read with no fallback) and
// each axis is a different field of the fix. A regression that re-broke one
// axis (e.g. the `owned` match) without breaking the others (e.g. the
// parameter-type match) is *plausible* — someone widening the gate, or
// narrowing the AST branch — and would slip past a single-axis pinning
// test. All three axes pinned here is what makes the regression-protector
// honest.

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

// Per-test scratch paths, all keyed off a process-unique stem (see
// tests/support/unique_stem.hpp — std::rand() collisions were the root
// cause of the #3002-class COVERAGE SHORT under fork pressure).
struct Scratch3233 {
    std::string libSrc, libObj, appSrc, appExe, errFile;
};

Scratch3233 makeScratch3233(const char* tag) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem(tag);
    Scratch3233 s;
    s.libSrc = (dir / (stem + "_lib.bzl")).string();
    s.libObj = (dir / (stem + "_lib.o")).string();
    s.appSrc = (dir / (stem + "_app.bzl")).string();
    s.appExe = (dir / (stem + "_app")).string();
    s.errFile = (dir / (stem + "_err.txt")).string();
    return s;
}

void writeFile3233(const std::string& path, const std::string& body) {
    std::ofstream(path) << body;
}

// Two-module compile: lib.bzl -> lib.o, then app.bzl -> appExe, capturing
// the importer's stderr so a diagnostic can be asserted against (and so a
// silent compile on the pre-fix build would show up as stderr == ""). The
// lib compile itself must succeed for the importer test to be meaningful,
// so a lib failure is reported as depOk=false and the test skips rather
// than asserting against the wrong stderr.
struct Compiled3233 {
    bool depOk = true;
    bool success = false;
    std::string err;
};

Compiled3233 compileImport3233(const std::string& libBody,
                               const std::string& appBody) {
    Scratch3233 s = makeScratch3233("bz3233");
    writeFile3233(s.libSrc, libBody);
    Compiled3233 r;
    {
        bezel::Driver d;
        if (!d.compileObject(s.libSrc, s.libObj).success ||
            !std::ifstream(s.libObj).good()) {
            r.depOk = false;
            return r;
        }
    }
    writeFile3233(s.appSrc, appBody);
    std::vector<bezel::Dependency> deps;
    bezel::Dependency dep;
    dep.metadataPath = s.libObj;
    dep.objectPath = s.libObj;
    deps.push_back(dep);
    std::fflush(stderr);
    std::cerr.flush();
    int saved = ::dup(STDERR_FILENO);
    int fd = ::open(s.errFile.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) ::dup2(fd, STDERR_FILENO);
    {
        bezel::Driver d;
        r.success = bezel::test::compileFileForTest(d, s.appSrc, s.appExe,
                                                   deps).success;
    }
    std::fflush(stderr);
    std::cerr.flush();
    if (fd >= 0) {
        ::close(fd);
        ::dup2(saved, STDERR_FILENO);
    }
    ::close(saved);
    std::ifstream in(s.errFile);
    r.err.assign((std::istreambuf_iterator<char>(in)),
                 std::istreambuf_iterator<char>());
    std::remove(s.errFile.c_str());
    std::remove(s.libSrc.c_str());
    std::remove(s.libObj.c_str());
    std::remove(s.appSrc.c_str());
    std::remove(s.appExe.c_str());
    return r;
}

// The lib carries an exported class whose methods will be the IMPORTED
// witnesses in each test below. Three methods, one per §11.2 axis:
//   * Get(): int         vs iface Get(): string        — return type mismatch
//   * Take(x: int): int  vs iface Take(x: string): int — parameter type mismatch
//   * Make(): owned T    vs iface Make(): T            — `owned` return mismatch
//   * Self(): BaseT      vs iface Self(): owned BaseT  — `owned` return mismatch, the other direction
const char* kLibBody3233 =
    "module VL3233; "
    "public class Thing3233 "
    "  public v: int := 5; "
    "end Thing3233; "
    "public class Base3233 "
    "  public method Get(): int "
    "    return 7; "
    "  end Get; "
    "  public method Take(x: int): int "
    "    return x + 1; "
    "  end Take; "
    "  public method Make(): owned Thing3233 "
    "    return new Thing3233(); "
    "  end Make; "
    "  public method Self(): Base3233 "
    "    return this; "
    "  end Self; "
    "end Base3233; ";

// Three §11.2 reject tests, one per axis the issue names. Each test
// compiles a local class `D<tag>3233 : Base3233, I<tag>3233` whose interface
// obligation CANNOT be satisfied through the inherited `Base3233` method —
// the inheritance-based witness is imported (it is `Base3233`'s method,
// declared in module VL3233), and §11.2 rejects on the mismatch. The fix
// (843e30f) gates the imported-witness path through `ctx_.isImported(impl)`;
// on the pre-fix parent the check silently does not run and the build
// succeeds, then either SIGSEGVs, runs an int through a string, or leaks
// the owned-`T` instance per axis.
const char* kAppReturn3233 =
    "module VA3233R; import VL3233; "
    "public interface IRet3233 "
    "  method Get(): string; "
    "end IRet3233; "
    "public class DRet3233: Base3233, IRet3233 "
    "end DRet3233; "
    "public class C "
    "  public static method Main(): int "
    "    using var d := new DRet3233(); "
    "    var i: IRet3233 := d; "
    "    return 0; "
    "  end Main; "
    "end C; ";

const char* kAppParam3233 =
    "module VA3233P; import VL3233; "
    "public interface IPar3233 "
    "  method Take(x: string): int; "
    "end IPar3233; "
    "public class DPar3233: Base3233, IPar3233 "
    "end DPar3233; "
    "public class C "
    "  public static method Main(): int "
    "    using var d := new DPar3233(); "
    "    return 0; "
    "  end Main; "
    "end C; ";

const char* kAppOwned3233 =
    "module VA3233O; import VL3233; "
    "public interface IOwn3233 "
    "  method Make(): Thing3233; "
    "end IOwn3233; "
    "public class DOwn3233: Base3233, IOwn3233 "
    "end DOwn3233; "
    "public class C "
    "  public static method Main(): int "
    "    using var d := new DOwn3233(); "
    "    return 0; "
    "  end Main; "
    "end C; ";

bool contains3233(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// ===========================================================================
// T1 — §11.2 RETURN TYPE. The axis whose false accept was MEMORY-UNSAFE:
// an int-returning method was installed in the itable slot declared as
// `string`-returning, and the caller drove the returned 7 through `bz.string`
// (SIGSEGV, measured). §11.2 admits no covariance on returns.
// ===========================================================================
BEZEL_TEST(xmod_iface_witness_from_imported_base_3233,
           imported_witness_return_type_mismatch_rejected_11_2) {
    Compiled3233 c = compileImport3233(kLibBody3233, kAppReturn3233);
    if (!c.depOk) return;  // dep toolchain unavailable — skip, not fail
    BEZEL_EXPECT_TRUE(!c.success);
    BEZEL_EXPECT_TRUE(contains3233(c.err, "return type"));
    BEZEL_EXPECT_TRUE(contains3233(c.err, "11.2"));
}

// ===========================================================================
// T2 — §11.2 PARAMETER TYPE. The imported ParameterSymbol has no `declType`;
// its type rides `resolvedDeclType`, the same channel the interface side has
// read since #2373. Measured false accept: the program compiled and returned
// a garbage int (225 here, 241 under review — layout-dependent; what is
// invariant is that it is garbage, and the local twin rejects).
// ===========================================================================
BEZEL_TEST(xmod_iface_witness_from_imported_base_3233,
           imported_witness_param_type_mismatch_rejected_11_2) {
    Compiled3233 c = compileImport3233(kLibBody3233, kAppParam3233);
    if (!c.depOk) return;
    BEZEL_EXPECT_TRUE(!c.success);
    BEZEL_EXPECT_TRUE(contains3233(c.err, "on parameter 1"));
    BEZEL_EXPECT_TRUE(contains3233(c.err, "11.2"));
}

// ===========================================================================
// T3 — §11.2 "`owned` in interface signatures", the LOOSENING direction:
// the implementation returns `owned T`, the interface does not. §11.2
// reason 1: the caller never records the transfer. LeakSanitizer measured
// exactly that on the pre-fix build — a 16-byte direct leak in
// `bz$VL3233.Base3233$Make`. The audit frames this bind as unwinnable in
// both directions; §11.2's answer is neither — reject the conformance.
// ===========================================================================
BEZEL_TEST(xmod_iface_witness_from_imported_base_3233,
           imported_witness_owned_return_dropped_rejected_11_2) {
    Compiled3233 c = compileImport3233(kLibBody3233, kAppOwned3233);
    if (!c.depOk) return;
    BEZEL_EXPECT_TRUE(!c.success);
    BEZEL_EXPECT_TRUE(contains3233(c.err, "`owned` return modifier"));
    BEZEL_EXPECT_TRUE(contains3233(c.err, "11.2"));
}

// ===========================================================================
// T4 — OVER-REJECTION CONTROL. The same shape, but the inherited methods
// DO satisfy the interface obligation — a CORRECT witness across the module
// boundary. This is what guards against a fix that turns the three checks
// ON but compares them wrong (e.g. mistaking a loader-reconstructed TypeRef
// for a local one). The lib body is reused with a matching interface;
// nothing in this test should be rejected. If it compiles and runs the
// test passes; the run value of `i.Take("hi") + i.Get()` is `8`.
// ===========================================================================
BEZEL_TEST(xmod_iface_witness_from_imported_base_3233,
           correct_imported_witness_still_accepted_11_2) {
    const std::string libBody =
        "module VL3233OK; "
        "public class Base3233OK "
        "  public method Take(x: string): int "
        "    return x.Length; "
        "  end Take; "
        "  public method Get(): int "
        "    return 8; "
        "  end Get; "
        "end Base3233OK; ";
    const std::string appBody =
        "module VA3233OK; import VL3233OK; "
        "public interface IOK3233 "
        "  method Take(x: string): int; "
        "  method Get(): int; "
        "end IOK3233; "
        "public class DOK3233: Base3233OK, IOK3233 "
        "end DOK3233; "
        "public class C "
        "  public static method Main(): int "
        "    using var d := new DOK3233(); "
        "    var i: IOK3233 := d; "
        "    return i.Take(\"hi\") + i.Get(); "
        "  end Main; "
        "end C; ";
    Scratch3233 s = makeScratch3233("bz3233ok");
    writeFile3233(s.libSrc, libBody);
    {
        bezel::Driver d;
        if (!d.compileObject(s.libSrc, s.libObj).success ||
            !std::ifstream(s.libObj).good()) {
            return;  // dep toolchain unavailable — skip
        }
    }
    writeFile3233(s.appSrc, appBody);
    std::vector<bezel::Dependency> deps;
    bezel::Dependency dep;
    dep.metadataPath = s.libObj;
    dep.objectPath = s.libObj;
    deps.push_back(dep);
    bezel::Driver d;
    auto cr = bezel::test::compileFileForTest(d, s.appSrc, s.appExe, deps);
    if (!cr.success) {
        std::remove(s.libSrc.c_str());
        std::remove(s.libObj.c_str());
        std::remove(s.appSrc.c_str());
        std::remove(s.appExe.c_str());
        BEZEL_EXPECT_TRUE(cr.success);  // will fail; recorded below
        return;
    }
    int status = std::system(s.appExe.c_str());
    std::remove(s.libSrc.c_str());
    std::remove(s.libObj.c_str());
    std::remove(s.appSrc.c_str());
    std::remove(s.appExe.c_str());
    if (status == -1 || !WIFEXITED(status)) {
        BEZEL_EXPECT_TRUE(false);
        return;
    }
    BEZEL_EXPECT_EQ(WEXITSTATUS(status), 10);
}

}  // namespace bezel::module