// fix #2415 (§13.6, §23.3, §2.8.2 clause 1 / §2.8.3 rule 4; v0.235): a
// settable IMPORTED property's setter VISIBILITY (`private set` /
// `protected set` / `internal set`) was not enforced at the cross-module
// assignment site — a visibility bypass.
//
// This is the #2374 defect class, found during verify-first for #2412
// (getter-only imported property assignment silently accepted). #2412's fix
// added an `else` branch to ExprTypecheck.cpp's `:=` assignment-target check
// for the `!pd` (imported PropertySymbol) case, falling back to
// `ctx_.importedPropertyHasSetter(ps)` to reject a write to a getter-only
// imported property. That fallback recovered only WHETHER a setter exists —
// not the setter's EFFECTIVE VISIBILITY, because the whole `sv`/`bad`
// visibility-computation block (ExprTypecheck.cpp, the `pd`-gated branch)
// reads `pd->accessors[i].visibility` / `pd->autoAccessorVis` directly from
// an `ast::PropertyDecl`, and an imported PropertySymbol has none (`ps->decl`
// is unconditionally null for an import — ModuleLoader never reconstructs a
// body-less imported property's decl).
//
// §13.6 (v0.235): "A property or indexer's `get` or `set` may carry its own
// visibility, which must not be *less* restrictive than the property's
// overall visibility... An accessor without explicit visibility inherits the
// property's overall visibility." §13.1's four levels (same section):
// `private` = "the declaring class only"; `protected` = "the declaring class
// and all classes derived from it" (no module qualifier); `internal` =
// "code in the same module". Combined with §2.8.2 clause 1 (the artifact
// interface must be sufficient to type-check every well-formed use of the
// module's public surface) and §2.8.3 rule 4 (one runtime type identity /
// one set of rules regardless of which artifact produced the value), the
// same rule must apply whether the property is reached locally or through an
// `import`.
//
// THE METADATA QUESTION (per this fix's brief): pre-fix, `MetaMember` carried
// `hasSetter` (#2400, a bool: does a setter exist) but NO setter-visibility
// scalar at all — `out.visibility` on a MetaMember is the member's OWN
// (property-level) visibility (`toMetaVisibility(m.visibility, inInterface)`,
// ModuleMetadataBuilder.cpp), never the more-restrictive ACCESSOR-level
// override §13.6 allows. So the answer is: setter visibility was NOT carried
// in the metadata at all — this fix adds it (a new `MetaMember::
// setterVisibility` scalar, mirroring hasSetter's own #2400 precedent
// exactly: hashMember / copy-ctor / copy-assign / operator== / emitMember /
// readMemberBody all updated together, additive within schema v7 so no
// version bump).
//
// Fix: `ModuleMetadataBuilder.cpp`'s property-member export now also computes
// the setter's EFFECTIVE visibility (accessor override if present, else the
// property's own declared visibility — mirroring ExprTypecheck.cpp's local
// `sv` computation verbatim) and stores it as `MetaMember::
// setterVisibility`. `ModuleLoader.cpp` publishes it through a new
// `SemaContext::importedPropertySetterVisibility()` side table. The `else`
// branch #2412 added in ExprTypecheck.cpp now applies the §13.1 access rules
// to that recovered visibility: `private` is an unconditional reject (the
// declaring type, an imported TypeSymbol, can never equal `currentType_`, a
// type compiled in this module); `protected` reuses the existing `isSubtype`
// check (already proven to walk a reconstructed imported base by #2400's own
// inherited-setter itable tests) so a LOCAL subclass of the imported
// declaring type is still accepted; `internal` is also an unconditional
// reject (the importing module is by definition a different module than the
// declaring one — `ctx_.isImported(ps)` is unconditionally true for every
// `ps` reaching this branch, mirroring the local check's own `internal`
// rule).
//
// CONFIRMED (verify-first, before any fix): compiled the exact repro from the
// filed issue (`private set` imported property, `d.Name := 999;`) — compiled
// CLEAN (exit 0) pre-fix. The identical same-module shape was, correctly,
// already rejected with "cannot assign to property `Name` — its setter is
// not accessible here (§13.6)".
//
// Mechanically verified fail-without/pass-with: saved the src/ fix as a
// scratch-copied patch, reverted via `git checkout HEAD --` (NOT `git
// stash` — shared worktree refs/stash), rebuilt, confirmed T1/T2/T3/T5
// compiled CLEAN (the bug) while T4/T6/T7 (accept shapes) and T8 (local
// control) were unaffected either way; reapplied the byte-identical patch via
// `git apply`, rebuilt, confirmed all pass with the asserted
// diagnostics/values.

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

struct TempFile2415 {
    std::string path;
    explicit TempFile2415(std::string p) : path(std::move(p)) {}
    ~TempFile2415() { std::remove(path.c_str()); }
};

void writeSource2415(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj2415(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// Compile B (importing A's compiled object), capturing the driver's stderr
// (fd 2) into `err`. Mirrors xmod_getter_only_prop_2412_test.cpp's
// compileImportCaptureErr2412.
bool compileImportCaptureErr2415(const std::string& bSrc,
                                 const std::string& bExe,
                                 const std::string& aObj, std::string& err) {
    namespace fs = std::filesystem;
    std::string errPath = (fs::temp_directory_path() /
                           ("bz2415_err_" + std::to_string(std::rand()) +
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
int compileImportAndRun2415(const std::string& libSrc,
                            const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2415_" + std::to_string(std::rand());
    TempFile2415 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2415 aObj((dir / (stem + "_a.o")).string());
    writeSource2415(aSrc.path, libSrc);
    if (!compileToObj2415(aSrc.path, aObj.path)) return -2;
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile2415 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2415 bExe((dir / stem).string());
    writeSource2415(bSrc.path, appSrc);
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
// T1 — the exact #2415 repro: `private set` (block form), IMPORTED, direct
// (non-itable) shape. Pre-fix: compiled clean. Asserts the diagnostic TEXT
// (not just failure) so an unrelated compile error cannot masquerade as a
// pass.
// ===========================================================================
BEZEL_TEST(xmod_prop_setter_visibility_2415,
           private_set_block_form_imported_property_rejected) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2415_t1_" + std::to_string(std::rand());
    TempFile2415 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2415 aObj((dir / (stem + "_a.o")).string());
    writeSource2415(aSrc.path,
                    "module PrivSetLib2415T1; "
                    "public class PrivSetClass2415T1 "
                    "  private _n: int := 1; "
                    "  public property Name: int "
                    "    get => _n; "
                    "    private set => _n := value; "
                    "  end Name; "
                    "end PrivSetClass2415T1;");
    BEZEL_EXPECT_TRUE(compileToObj2415(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;  // toolchain unavailable

    std::string bSrc =
        "module App2415T1; import PrivSetLib2415T1; "
        "public class C "
        "  public static method Main(): int "
        "    using var d: PrivSetClass2415T1 := new PrivSetClass2415T1(); "
        "    d.Name := 999; "
        "    return 0; "
        "  end Main; "
        "end C;";
    TempFile2415 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2415 bExe((dir / (stem + "_b")).string());
    writeSource2415(bSrcFile.path, bSrc);

    std::string err;
    bool success =
        compileImportCaptureErr2415(bSrcFile.path, bExe.path, aObj.path, err);
    BEZEL_EXPECT_TRUE(!success);
    BEZEL_EXPECT_TRUE(err.find("cannot assign to property") !=
                       std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("`Name`") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("not accessible") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("13.6") != std::string::npos);
}

// ===========================================================================
// T2 — the AUTO-PROPERTY shorthand `private set` (§13.6: "The auto-property
// shorthand `public property Name: string private set;` is equivalent to the
// block form"), IMPORTED. Isolates the PropertyBodyKind::Auto /
// hasAutoAccessorVis branch from T1's Block-form shape — both must collapse
// to the same rejection at export time (ModuleMetadataBuilder.cpp) but
// exercise different builder branches. Uses the exact §23.1 spec example
// syntax (`public property Id: int private set;`).
// ===========================================================================
BEZEL_TEST(xmod_prop_setter_visibility_2415,
           private_set_auto_property_shorthand_imported_property_rejected) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2415_t2_" + std::to_string(std::rand());
    TempFile2415 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2415 aObj((dir / (stem + "_a.o")).string());
    writeSource2415(aSrc.path,
                    "module PrivSetAutoLib2415T2; "
                    "public class PrivSetAutoClass2415T2 "
                    "  public property Id: int private set; "
                    "end PrivSetAutoClass2415T2;");
    BEZEL_EXPECT_TRUE(compileToObj2415(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    std::string bSrc =
        "module App2415T2; import PrivSetAutoLib2415T2; "
        "public class C "
        "  public static method Main(): int "
        "    using var d: PrivSetAutoClass2415T2 := new "
        "PrivSetAutoClass2415T2(); "
        "    d.Id := 999; "
        "    return 0; "
        "  end Main; "
        "end C;";
    TempFile2415 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2415 bExe((dir / (stem + "_b")).string());
    writeSource2415(bSrcFile.path, bSrc);

    std::string err;
    bool success =
        compileImportCaptureErr2415(bSrcFile.path, bExe.path, aObj.path, err);
    BEZEL_EXPECT_TRUE(!success);
    BEZEL_EXPECT_TRUE(err.find("cannot assign to property") !=
                       std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("`Id`") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("13.6") != std::string::npos);
}

// ===========================================================================
// T3 — `protected set`, IMPORTED, from an UNRELATED local class (no
// inheritance relationship at all). §13.1: protected = "the declaring class
// and all classes derived from it" — an unrelated importer class qualifies
// as neither, so this must be rejected exactly like `private set`.
// ===========================================================================
BEZEL_TEST(xmod_prop_setter_visibility_2415,
           protected_set_imported_property_rejected_from_unrelated_class) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2415_t3_" + std::to_string(std::rand());
    TempFile2415 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2415 aObj((dir / (stem + "_a.o")).string());
    writeSource2415(aSrc.path,
                    "module ProtSetLib2415T3; "
                    "public class ProtSetBase2415T3 "
                    "  private _n: int := 1; "
                    "  public property Name: int "
                    "    get => _n; "
                    "    protected set => _n := value; "
                    "  end Name; "
                    "end ProtSetBase2415T3;");
    BEZEL_EXPECT_TRUE(compileToObj2415(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    std::string bSrc =
        "module App2415T3; import ProtSetLib2415T3; "
        "public class C "
        "  public static method Main(): int "
        "    using var d: ProtSetBase2415T3 := new ProtSetBase2415T3(); "
        "    d.Name := 5; "
        "    return 0; "
        "  end Main; "
        "end C;";
    TempFile2415 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2415 bExe((dir / (stem + "_b")).string());
    writeSource2415(bSrcFile.path, bSrc);

    std::string err;
    bool success =
        compileImportCaptureErr2415(bSrcFile.path, bExe.path, aObj.path, err);
    BEZEL_EXPECT_TRUE(!success);
    BEZEL_EXPECT_TRUE(err.find("cannot assign to property") !=
                       std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("`Name`") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("13.6") != std::string::npos);
}

// ===========================================================================
// T4 — ACCEPT: `protected set`, IMPORTED, from a LOCAL DERIVED class's OWN
// method. §13.1 grants protected access to "the declaring class and all
// classes derived from it" — with no module qualifier, so a subclass
// declared in the IMPORTING module must still be able to use the inherited
// setter. This is the false-reject axis the fix's brief specifically calls
// out ("protected set from a derived class in the importing module — check
// what §13 actually grants"). Compiles AND RUNS, asserting the written-then-
// read value round-trips (guards against a fix that over-widens the reject
// into this legitimate shape).
// ===========================================================================
BEZEL_TEST(xmod_prop_setter_visibility_2415,
           protected_set_imported_property_accepted_from_local_subclass) {
    int r = compileImportAndRun2415(
        "module ProtSetLib2415T4; "
        "public class ProtSetBase2415T4 "
        "  private _n: int := 1; "
        "  public property Name: int "
        "    get => _n; "
        "    protected set => _n := value; "
        "  end Name; "
        "end ProtSetBase2415T4;",
        "module App2415T4; import ProtSetLib2415T4; "
        "public class Derived2415T4: ProtSetBase2415T4 "
        "  public method SetIt(v: int) => this.Name := v; "
        "end Derived2415T4; "
        "public class C "
        "  public static method Main(): int "
        "    using var d: Derived2415T4 := new Derived2415T4(); "
        "    d.SetIt(42); "
        "    return d.Name - 42; "
        "  end Main; "
        "end C;");
    if (r == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 0);
}

// ===========================================================================
// T5 — `internal set`, IMPORTED, from the importing module. §13.1: internal
// = "code in the same module" — the importing module is BY DEFINITION a
// different module than the declaring one, so this must always be rejected
// (confirms the issue's own suggested-fix-direction hypothesis: "internal
// set might already coincidentally reduce to always inaccessible" — verified
// true here).
// ===========================================================================
BEZEL_TEST(xmod_prop_setter_visibility_2415,
           internal_set_imported_property_always_rejected) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2415_t5_" + std::to_string(std::rand());
    TempFile2415 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2415 aObj((dir / (stem + "_a.o")).string());
    writeSource2415(aSrc.path,
                    "module IntSetLib2415T5; "
                    "public class IntSetClass2415T5 "
                    "  private _n: int := 1; "
                    "  public property Name: int "
                    "    get => _n; "
                    "    internal set => _n := value; "
                    "  end Name; "
                    "end IntSetClass2415T5;");
    BEZEL_EXPECT_TRUE(compileToObj2415(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    std::string bSrc =
        "module App2415T5; import IntSetLib2415T5; "
        "public class C "
        "  public static method Main(): int "
        "    using var d: IntSetClass2415T5 := new IntSetClass2415T5(); "
        "    d.Name := 5; "
        "    return 0; "
        "  end Main; "
        "end C;";
    TempFile2415 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2415 bExe((dir / (stem + "_b")).string());
    writeSource2415(bSrcFile.path, bSrc);

    std::string err;
    bool success =
        compileImportCaptureErr2415(bSrcFile.path, bExe.path, aObj.path, err);
    BEZEL_EXPECT_TRUE(!success);
    BEZEL_EXPECT_TRUE(err.find("cannot assign to property") !=
                       std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("`Name`") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("13.6") != std::string::npos);
}

// ===========================================================================
// T6 — ACCEPT-CONTROL: an explicit `public set`, IMPORTED, direct shape.
// §13.6: an accessor visibility equal to the property's own is "permitted,
// though redundant". Compiles AND RUNS, asserting the round-trip value.
// Guards against the fix rejecting a genuinely unrestricted setter.
// ===========================================================================
BEZEL_TEST(xmod_prop_setter_visibility_2415,
           explicit_public_set_imported_property_still_assignable_and_runs) {
    int r = compileImportAndRun2415(
        "module PubSetLib2415T6; "
        "public class PubSetClass2415T6 "
        "  private _n: int := 1; "
        "  public property Name: int "
        "    get => _n; "
        "    public set => _n := value; "
        "  end Name; "
        "end PubSetClass2415T6;",
        "module App2415T6; import PubSetLib2415T6; "
        "public class C "
        "  public static method Main(): int "
        "    using var d: PubSetClass2415T6 := new PubSetClass2415T6(); "
        "    d.Name := 321; "
        "    return d.Name - 321; "
        "  end Main; "
        "end C;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 0);
}

// ===========================================================================
// T7 — ACCEPT-CONTROL: an ORDINARY read-write property with NO accessor
// visibility override at all (`set => ...;`, no keyword — the property's own
// `public` visibility applies unchanged), IMPORTED, direct shape. Guards
// against the "no override" fallback path (`sv = pd->visibility` at export
// time, no accessor loop match) regressing the already-working #2412 T4
// shape. Compiles AND RUNS, asserting the round-trip value.
// ===========================================================================
BEZEL_TEST(xmod_prop_setter_visibility_2415,
           unrestricted_setter_no_accessor_override_imported_still_runs) {
    int r = compileImportAndRun2415(
        "module RWLib2415T7; "
        "public class RWClass2415T7 "
        "  private _n: int := 1; "
        "  public property Name: int "
        "    get => _n; "
        "    set => _n := value; "
        "  end Name; "
        "end RWClass2415T7;",
        "module App2415T7; import RWLib2415T7; "
        "public class C "
        "  public static method Main(): int "
        "    using var d: RWClass2415T7 := new RWClass2415T7(); "
        "    d.Name := 210; "
        "    return d.Name - 210; "
        "  end Main; "
        "end C;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 0);
}

// ===========================================================================
// T8 — REJECT-CONTROL: the SAME-MODULE (local) equivalent of T1's shape is
// still, correctly, rejected — unaffected by the fix (the fix only adds to
// the `else`/imported branch; the local `pd`-gated branch, already handling
// §13.6 since #747 R42, is untouched). Drives the Driver directly (no
// dependency object needed); mirrors the exact repro from #2415's filed
// issue text ("The SAME-MODULE equivalent... is correctly rejected").
// ===========================================================================
BEZEL_TEST(xmod_prop_setter_visibility_2415,
           local_private_set_property_still_rejected_control) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2415_t8_" + std::to_string(std::rand());
    TempFile2415 src((dir / (stem + ".bzl")).string());
    TempFile2415 exe((dir / stem).string());
    writeSource2415(src.path,
                    "module LocalMod2415T8; "
                    "public class PrivSetClass2415T8 "
                    "  private _n: int := 1; "
                    "  public property Name: int "
                    "    get => _n; "
                    "    private set => _n := value; "
                    "  end Name; "
                    "end PrivSetClass2415T8; "
                    "public class C "
                    "  public static method Main(): int "
                    "    using var d: PrivSetClass2415T8 := "
                    "new PrivSetClass2415T8(); "
                    "    d.Name := 999; "
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
    BEZEL_EXPECT_TRUE(err.find("cannot assign to property") !=
                       std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("`Name`") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("13.6") != std::string::npos);
}

}  // namespace bezel::module
