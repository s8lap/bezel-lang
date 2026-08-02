// fix #2412 (§23.3, §2.8.2/§2.8.3 rule 4, §11.2; v0.235): assigning to a
// getter-only IMPORTED property compiled clean instead of being rejected.
//
// ExprTypecheck.cpp's `:=` assignment-target check (the §23.3/§23.1 "no
// setter" / "readonly outside a constructor" gate, added by #747 R42) resolved
// the target to a `PropertySymbol* ps`, then unconditionally did:
//
//     if (auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl)) {
//         ... hasSetter / isReadonlyAuto / setter-visibility checks ...
//     }
//     // no `else` — an imported ps (ps->decl always null, ModuleLoader never
//     // reconstructs a body-less imported property's ast::PropertyDecl)
//     // silently skipped the WHOLE check.
//
// Filed as a further confirmed instance of #2374's audit pattern (an
// `astNode`/`decl`-only gate silently mishandling any imported symbol), found
// during #2400's (itable property-setter null-slot fix) verify-first pass.
//
// CONFIRMED MATRIX (verified by probe before writing this test, then
// mechanically by reverting the src/ fix and re-running this exact filter):
//   - `d.Name := 55;` where `Name` is an imported expression-bodied
//     (getter-only) property: pre-fix, compiled clean (exit 0). Reading the
//     property back afterward (`d.Name := 55; return d.Name;`) returned the
//     UNTOUCHED initializer value (1), proving the write was a silent no-op
//     at runtime — not a crash, not a wrong-but-plausible value.
//   - The SAME-MODULE equivalent (identical shape, no import) was already,
//     correctly, rejected at compile time with:
//       "cannot assign to read-only property `Name` — it has no setter
//       (§23.3)"
//     confirming the gap is specifically the cross-module path, not a general
//     §23.3 enforcement bug (T5 below pins this control).
//
// Fix: an `else` branch on the `!pd` (imported) case, falling back to
// `ctx_.importedPropertyHasSetter(ps)` — the side table #2400 added (from the
// new `MetaMember::hasSetter` metadata bit, ModuleLoader.cpp's
// `propHasSetter_`/`setImportedPropertyHasSetter`, already published before
// sema runs). `ModuleMetadataBuilder.cpp`'s export-time computation
// (mirroring `LlvmBackend_impl.hpp`'s local `propertyHasSetter` exactly)
// produces `hasSetter == false` for BOTH a genuinely getter-only property
// (expression-bodied or block-form get-only) AND a `readonly` auto-property
// — so one fallback message ("cannot assign to read-only property ... — it
// has no setter (§23.3)") correctly covers all three source shapes for the
// imported case: from outside the declaring module, none of them is ever a
// valid assignment target (a readonly auto-property's local §23.1
// constructor exception is inherently unreachable across a module boundary).
// T1/T2/T3 below pin each of the three shapes.
//
// Scope note (recorded per CLAUDE.md rule 4, not silently applied): a
// SETTABLE imported property's setter accessor VISIBILITY (§13.6 — e.g.
// `private set`) is NOT checked by this fix; the `pd`-gated branch's
// visibility computation (lines around `sv`/`bad` in ExprTypecheck.cpp)
// still has no imported-side fallback, so a private-setter imported property
// is currently still writable from any importer (confirmed by probe:
// `d.Name := 999;` on a `private set` imported property compiles clean).
// That is a DIFFERENT, further #2374-family instance — filed separately as
// issue #2415, not folded into this fix (which is scoped exactly to #2412's
// hasSetter gap).
//
// Mechanically verified fail-without/pass-with: saved the src/ fix as a
// scratch-copied patch, reverted via `git checkout HEAD --` (NOT `git
// stash` — shared worktree refs/stash), rebuilt, confirmed T1/T2/T3 compiled
// CLEAN (the bug) and T5's local control still correctly rejected; reapplied
// the byte-identical patch via `git apply`, rebuilt, confirmed all pass with
// the asserted diagnostics/values.

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "driver/Driver.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

struct TempFile2412 {
    std::string path;
    explicit TempFile2412(std::string p) : path(std::move(p)) {}
    ~TempFile2412() { std::remove(path.c_str()); }
};

void writeSource2412(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj2412(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// Compile B (importing A's compiled object), capturing the driver's stderr
// (fd 2) into `err`. Mirrors xmod_abstract_impl_prop_2380_test.cpp's
// compileImportCaptureErr2380 (itself mirroring
// xmod_generic_method_arg_marshalling_1943_test.cpp).
bool compileImportCaptureErr2412(const std::string& bSrc,
                                 const std::string& bExe,
                                 const std::string& aObj, std::string& err) {
    namespace fs = std::filesystem;
    std::string errPath = (fs::temp_directory_path() /
                           ("bz2412_err_" + std::to_string(std::rand()) +
                            ".txt"))
                              .string();

    std::fflush(stderr);
    std::cerr.flush();
    int saved = ::dup(STDERR_FILENO);
    int fd = ::open(errPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    bool success = false;
    if (fd >= 0) ::dup2(fd, STDERR_FILENO);
    {
        bezel::Driver driver;
        std::vector<bezel::Dependency> deps;
        bezel::Dependency d;
        d.metadataPath = aObj;
        d.objectPath = aObj;
        deps.push_back(d);
        success = driver.compileFile(bSrc, bExe, deps).success;
    }
    std::fflush(stderr);
    std::cerr.flush();
    if (fd >= 0) {
        ::close(fd);
        ::dup2(saved, STDERR_FILENO);
    }
    ::close(saved);

    std::ifstream in(errPath);
    err.assign((std::istreambuf_iterator<char>(in)),
               std::istreambuf_iterator<char>());
    std::remove(errPath.c_str());
    return success;
}

// Compile B importing A -> exe, run in a SEPARATE process (fork-isolated per
// tests/README.md), return exit code (-1 on compile/link/run failure, -2 if
// the dependency itself could not be produced).
int compileImportAndRun2412(const std::string& libSrc,
                            const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2412_" + std::to_string(std::rand());
    TempFile2412 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2412 aObj((dir / (stem + "_a.o")).string());
    writeSource2412(aSrc.path, libSrc);
    if (!compileToObj2412(aSrc.path, aObj.path)) return -2;
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile2412 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2412 bExe((dir / stem).string());
    writeSource2412(bSrc.path, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    if (!driver.compileFile(bSrc.path, bExe.path, deps).success) return -1;
    int status = std::system(bExe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

}  // namespace

// ===========================================================================
// T1 — the exact #2412 repro: an EXPRESSION-BODIED (§23.2) getter-only
// IMPORTED property assigned through the imported class's own type (direct,
// non-itable shape). Pre-fix: compiled clean. Asserts the diagnostic TEXT
// (not just failure) so an unrelated compile error cannot masquerade as a
// pass.
// ===========================================================================
BEZEL_TEST(xmod_getter_only_prop_2412,
           expression_bodied_getter_only_imported_property_rejected) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2412_t1_" + std::to_string(std::rand());
    TempFile2412 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2412 aObj((dir / (stem + "_a.o")).string());
    writeSource2412(aSrc.path,
                    "module GetterOnlyLib2412T1; "
                    "public class GetterOnlyClass2412T1 "
                    "  public property Name: int => 1; "
                    "end GetterOnlyClass2412T1;");
    BEZEL_EXPECT_TRUE(compileToObj2412(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;  // toolchain unavailable

    std::string bSrc =
        "module App2412T1; import GetterOnlyLib2412T1; "
        "public class C "
        "  public static method Main(): int "
        "    using var d: GetterOnlyClass2412T1 := new GetterOnlyClass2412T1(); "
        "    d.Name := 55; "
        "    return 0; "
        "  end Main; "
        "end C;";
    TempFile2412 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2412 bExe((dir / (stem + "_b")).string());
    writeSource2412(bSrcFile.path, bSrc);

    std::string err;
    bool success =
        compileImportCaptureErr2412(bSrcFile.path, bExe.path, aObj.path, err);
    BEZEL_EXPECT_TRUE(!success);
    BEZEL_EXPECT_TRUE(err.find("cannot assign to read-only property") !=
                       std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("`Name`") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("it has no setter") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("23.3") != std::string::npos);
}

// ===========================================================================
// T2 — a BLOCK-FORM get-only (§23.3, `get => ...;` with no `set`) IMPORTED
// property is also rejected. Isolates the PropertyBodyKind::Block shape from
// T1's Expression shape — both collapse to `hasSetter == false` at export
// time (ModuleMetadataBuilder.cpp), but exercise different builder branches.
// ===========================================================================
BEZEL_TEST(xmod_getter_only_prop_2412,
           block_form_get_only_imported_property_rejected) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2412_t2_" + std::to_string(std::rand());
    TempFile2412 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2412 aObj((dir / (stem + "_a.o")).string());
    writeSource2412(aSrc.path,
                    "module GetterOnlyLib2412T2; "
                    "public class GetterOnlyClass2412T2 "
                    "  private _n: int := 1; "
                    "  public property Name: int "
                    "    get => _n; "
                    "  end Name; "
                    "end GetterOnlyClass2412T2;");
    BEZEL_EXPECT_TRUE(compileToObj2412(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    std::string bSrc =
        "module App2412T2; import GetterOnlyLib2412T2; "
        "public class C "
        "  public static method Main(): int "
        "    using var d: GetterOnlyClass2412T2 := new GetterOnlyClass2412T2(); "
        "    d.Name := 55; "
        "    return 0; "
        "  end Main; "
        "end C;";
    TempFile2412 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2412 bExe((dir / (stem + "_b")).string());
    writeSource2412(bSrcFile.path, bSrc);

    std::string err;
    bool success =
        compileImportCaptureErr2412(bSrcFile.path, bExe.path, aObj.path, err);
    BEZEL_EXPECT_TRUE(!success);
    BEZEL_EXPECT_TRUE(err.find("cannot assign to read-only property") !=
                       std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("`Name`") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("23.3") != std::string::npos);
}

// ===========================================================================
// T3 — a `readonly` AUTO-property (§23.1) IMPORTED is rejected when assigned
// from outside its declaring module. A local `readonly` auto-property is
// assignable inside a field initializer or the declaring class's own
// constructor (§23.1) — but that exception is inherently unreachable across
// a module boundary, so the imported case correctly collapses to the same
// "no setter" rejection as T1/T2 (the metadata's single `hasSetter` bit
// cannot and need not distinguish the two source shapes for an import).
// ===========================================================================
BEZEL_TEST(xmod_getter_only_prop_2412,
           readonly_auto_property_imported_rejected) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2412_t3_" + std::to_string(std::rand());
    TempFile2412 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2412 aObj((dir / (stem + "_a.o")).string());
    writeSource2412(aSrc.path,
                    "module ROAutoLib2412T3; "
                    "public class ROAutoClass2412T3 "
                    "  public readonly property Name: int := 7; "
                    "end ROAutoClass2412T3;");
    BEZEL_EXPECT_TRUE(compileToObj2412(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    std::string bSrc =
        "module App2412T3; import ROAutoLib2412T3; "
        "public class C "
        "  public static method Main(): int "
        "    using var d: ROAutoClass2412T3 := new ROAutoClass2412T3(); "
        "    d.Name := 55; "
        "    return 0; "
        "  end Main; "
        "end C;";
    TempFile2412 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2412 bExe((dir / (stem + "_b")).string());
    writeSource2412(bSrcFile.path, bSrc);

    std::string err;
    bool success =
        compileImportCaptureErr2412(bSrcFile.path, bExe.path, aObj.path, err);
    BEZEL_EXPECT_TRUE(!success);
    BEZEL_EXPECT_TRUE(err.find("cannot assign to read-only property") !=
                       std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("`Name`") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("23.3") != std::string::npos);
}

// ===========================================================================
// T4 — ACCEPT-CONTROL (compile-AND-run, value-asserting): a genuinely
// READ-WRITE imported property (get/set block form) is still assignable, and
// the write is visibly observed on read-back. Discriminator 155 is far from
// both the field's initializer (1) and 0, and safely under the 256-exit-code
// mask (`return d.Name - 155;` yields 0 either way, but a broken write would
// yield a large nonzero value, e.g. 1 - 155 = -154, WEXITSTATUS-masked to a
// value far from 0). Guards against the fix over-widening the reject into
// genuinely settable imported properties.
// ===========================================================================
BEZEL_TEST(xmod_getter_only_prop_2412,
           read_write_imported_property_still_assignable_and_runs) {
    int r = compileImportAndRun2412(
        "module RWLib2412T4; "
        "public class RWClass2412T4 "
        "  private _n: int := 1; "
        "  public property Name: int "
        "    get => _n; "
        "    set => _n := value; "
        "  end Name; "
        "end RWClass2412T4;",
        "module App2412T4; import RWLib2412T4; "
        "public class C "
        "  public static method Main(): int "
        "    using var d: RWClass2412T4 := new RWClass2412T4(); "
        "    d.Name := 155; "
        "    return d.Name - 155; "
        "  end Main; "
        "end C;");
    if (r == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 0);
}

// ===========================================================================
// T5 — REJECT-CONTROL: the SAME-MODULE (local) equivalent of T1's shape is
// still, correctly, rejected — unaffected by the fix (the fix only adds an
// `else` for the imported/`!pd` case; the local `pd`-gated branch is
// untouched). Drives the Driver directly (no dependency object needed);
// mirrors the exact repro from #2412's filed issue text.
// ===========================================================================
BEZEL_TEST(xmod_getter_only_prop_2412,
           local_getter_only_property_still_rejected_control) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2412_t5_" + std::to_string(std::rand());
    TempFile2412 src((dir / (stem + ".bzl")).string());
    TempFile2412 exe((dir / stem).string());
    writeSource2412(src.path,
                    "module LocalMod2412T5; "
                    "public class GetterOnlyClass2412T5 "
                    "  public property Name: int => 1; "
                    "end GetterOnlyClass2412T5; "
                    "public class C "
                    "  public static method Main(): int "
                    "    using var d: GetterOnlyClass2412T5 := "
                    "new GetterOnlyClass2412T5(); "
                    "    d.Name := 55; "
                    "    return 0; "
                    "  end Main; "
                    "end C;");

    std::string errPath = (dir / (stem + "_err.txt")).string();
    std::fflush(stderr);
    std::cerr.flush();
    int saved = ::dup(STDERR_FILENO);
    int fd = ::open(errPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    bool success = false;
    if (fd >= 0) ::dup2(fd, STDERR_FILENO);
    {
        bezel::Driver driver;
        success = driver.compileFile(src.path, exe.path, {}).success;
    }
    std::fflush(stderr);
    std::cerr.flush();
    if (fd >= 0) {
        ::close(fd);
        ::dup2(saved, STDERR_FILENO);
    }
    ::close(saved);
    std::ifstream in(errPath);
    std::string err((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    std::remove(errPath.c_str());

    BEZEL_EXPECT_TRUE(!success);
    BEZEL_EXPECT_TRUE(err.find("cannot assign to read-only property") !=
                       std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("`Name`") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("23.3") != std::string::npos);
}

}  // namespace bezel::module
