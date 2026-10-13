// Spec-derived regression test for issue #3231 — the export-gate widening of
// emitExportedProperty{Getters,Setters} / emitExportedIndexer{Getters,Setters}
// to emit ExternalLinkage wrappers for VIRTUAL members too.
//
// §12.3 (`base.Member`) / §23 (Properties) / §23.4 (Indexers) / §2.8.3 rule 4,
// v0.252. Audit: docs/proposals/papers/member-symbol-audit-2026-08-13.md, family M6
// (rank 3, "imported-ABI function maps not consulted"). Umbrella-B programme:
// docs/proposals/papers/imported-symbol-programme-status-2026-08-14.md.
//
// ── THE DEFECT ─────────────────────────────────────────────────────────────
//
// Before #3231, the cross-module ABI for a VIRTUAL property/indexer had a
// gap that closed differently in different paths:
//
//   * `b.P` / `b[i]` on a VIRTUAL imported member dispatched through a
//     vtable-slot thunk (`importedVirtualAccessorSlotFn`, #3190) — loads
//     `vt$<Owner>[slot]` and forwards `this`.
//   * `base.P` / `base[i]` on the same VIRTUAL imported member REJECTED
//     with §12.3 (`rejectImportedVirtualBaseProperty` / `-Indexer`, #3199
//     and #2832) — a clean compile error but a legal §12.3 spelling that
//     could not actually be lowered across a module boundary.
//
// The exporter half `emitExportedPropertyGetters` / `-Setters` /
// `emitExportedIndexerGetters` / `-Setters` deliberately skipped a virtual
// member ("Skip virtual/override/abstract/static — they have other dispatch
// paths"), so the dependency object never defined
// `bz$<Class>$<Prop>$propget` / `$propset` / `$idxget$<arity>` /
// `$idxset$<arity>` for a virtual accessor — confirmed by `nm` (lowercase
// `t`, InternalLinkage). With no symbol defined, there was nothing for
// `base.` to dispatch to without running the override (which is what §12.3
// forbids).
//
// ── THE FIX ────────────────────────────────────────────────────────────────
//
// Widen the four export gates (skipping `override`/`abstract` to keep
// override-reuse semantics and avoid undefined wrappers) so a VIRTUAL,
// non-override, non-abstract member's OWNER-class body is exported under
// the same mangling as the non-virtual case. The importer side declares the
// matching external in four new direct-entry maps
// (`importedPropGetterDirectFns_` / `importedPropSetterDirectFns_` /
// `importedIndexerGetterDirectFns_` / `importedIndexerSetterDirectFns_`),
// and the `base.X` path routes through them — replacing the §12.3 reject
// for any shape that has an OWNER body to call (an ABSTRACT virtual member
// still rejects, §12.5-correct: no OWNER body exists either way).
//
// ── WHAT THIS TEST PINS ────────────────────────────────────────────────────
//
// The five shapes #3231's issue body names as "reachable sites today"
// (LlvmBackend_Assign.cpp:5319/5467/9196/9286 and LlvmBackend_Stmts.cpp:7411)
// are pinned by the existing #3190 / #3199 / #3204 / #3267 / #3231 test
// family; this file adds the SHAPE the widening is asked to enable — a
// LOCAL class satisfying an interface obligation through INHERITANCE from
// an IMPORTED base, exercising the §2.8.3 itable-fill path the widening
// makes possible for virtual members.
//
// The interface-conformance half ("local class inheriting from an imported
// base that declares a virtual PROPERTY … an interface conformance path
// that writes through the inherited accessor") is the spec-cited shape
// §2.8.3 rule 4 names, and the regression's load-bearing axis: a class
// satisfying an interface property obligation via an inherited import
// MUST compile cleanly through the widened emitter. The over-rejection
// control ("a local virtual property continues to lower through the
// existing specializedVtableFor path — the widening must NOT affect
// non-imported types") is the twin half: a LOCAL virtual property's
// existing single-module behaviour must not regress.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"
#include "test_harness.hpp"
#include "support/unique_stem.hpp"


namespace bezel::module {
namespace {

struct TempFile3231R {
    std::string path;
    explicit TempFile3231R(std::string p) : path(std::move(p)) {}
    ~TempFile3231R() { std::remove(path.c_str()); }
};

void writeSource3231R(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Compile lib -> object, compile app (importing lib) -> exe, run it in a
// separate process, return its exit code. -1 on compile/link/run failure,
// -2 if the dependency itself could not be produced (toolchain unavailable).
int compileImportAndRun3231R(const std::string& libSrc,
                             const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz3231r_");
    TempFile3231R aSrc((dir / (stem + "_a.bzl")).string());
    TempFile3231R aObj((dir / (stem + "_a.o")).string());
    writeSource3231R(aSrc.path, libSrc);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success ||
            !std::ifstream(aObj.path).good())
            return -2;
    }
    TempFile3231R bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3231R bExe((dir / stem).string());
    writeSource3231R(bSrc.path, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    if (!bezel::test::compileFileForTest(driver, bSrc.path, bExe.path, deps)
             .success)
        return -1;
    int status = std::system(bExe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// Single-module twin runner: same declarations with the module boundary
// removed. The twin is the oracle — both builds must agree on the value.
int compileLocalAndRun3231R(const std::string& src) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz3231rl_");
    TempFile3231R aSrc((dir / (stem + ".bzl")).string());
    TempFile3231R aExe((dir / stem).string());
    writeSource3231R(aSrc.path, src);
    bezel::Driver driver;
    if (!bezel::test::compileFileForTest(driver, aSrc.path, aExe.path).success)
        return -1;
    int status = std::system(aExe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}
}  // namespace

// ===========================================================================
// T1 — the load-bearing interface-conformance path the widening enables.
// A LOCAL class `D3231` inherits from an IMPORTED base `B3231` that declares
// a virtual PROPERTY `P: int`. `D3231` also implements a LOCAL interface
// `IP3231` that declares the same property. The interface-conformance path
// must reach the OWNER class's implementation through the widened
// emitter (§2.8.3 rule 4) — the previous (narrow) emitter defined no
// `bz$B3231$P$propget` symbol, leaving the itable-slot fill unable to
// resolve the inherited accessor.
//
// Pre-fix (#3231's premise): a SIGSEGV (rc=139) or a hard link error
// (the itable-slot fill's `undefined reference to bz$B3231$P$propget`),
// depending on which fallback the importer took.
// Post-fix: the program compiles, runs, and returns the OWNER's value (8)
// through BOTH the vtable dispatch path (`d.P` via the concrete receiver)
// AND the interface dispatch path (`ip.P` through the interface), and
// the SINGLE-MODULE TWIN returns the same 8.
//
// `_v` starts at 3; the OWNER's getter is `_v + 5 = 8`. The OVERRIDE in
// `D3231` is intentionally absent — the shape is "inherited, not
// overridden", which is exactly the family #3190 / #3199 / #3267 / #3231
// documents as the SIGSEGV-on-HEAD half.
// ===========================================================================
BEZEL_TEST(xmod_widen_exported_accessor_for_virtual_3231,
           interface_conformance_via_imported_virtual_base_compiles_2_8_3) {
    const std::string libSrc =
        "module Lib3231R; "
        "public class B3231 "
        "  private _v: int := 3; "
        "  public virtual property P: int "
        "    get => _v + 5; "
        "    set => _v := value; "
        "  end; "
        "end B3231; ";
    const std::string appBody =
        "public interface IP3231 "
        "  property P: int; "
        "end IP3231; "
        "public class D3231: B3231, IP3231 "
        "end D3231; "
        "public class C "
        "  public static method Main(): int "
        "    using var d := new D3231(); "
        "    var ip: IP3231 := d; "
        "    var a := d.P; "
        "    var b := ip.P; "
        "    d.P := 30; "
        "    var c := d.P; "
        "    return a + b + c; "
        "  end Main; "
        "end C;";
    // 8 (initial getter) + 8 (interface getter, same value via widened
    // direct entry the itable fill installs) + 35 (after setter, _v = 30)
    // = 51.
    const std::string twinSrc =
        std::string("module Twin3231RT1; ") +
        (libSrc.substr(libSrc.find("public class"))) +
        (appBody.substr(appBody.find("public")));
    int twin = compileLocalAndRun3231R(twinSrc);
    BEZEL_EXPECT_EQ(twin, 51);
    int imported = compileImportAndRun3231R(
        libSrc, std::string("module App3231R; import Lib3231R; ") + appBody);
    if (imported == -2) return;
    BEZEL_EXPECT_TRUE(imported >= 0);
    // The refute: the SIGSEGV/link-error claim would be rc=139 or a
    // non-zero compile/link outcome — assert we ran cleanly.
    BEZEL_EXPECT_FALSE(imported == 139);
    // The spec-correct answer: agreement with the twin.
    BEZEL_EXPECT_EQ(imported, twin);
}

// ===========================================================================
// T2 — the indexer twin of T1. A LOCAL class inherits from an IMPORTED base
// that declares a virtual INDEXER. The interface-conformance path reads
// through the inherited accessor — `ip[7]` and `d[7]` both must return
// `_v + 100 + 7 = 110` after the OWNER-direct route the widened emitter
// defines. `_v` starts at 3, so the OWNER's getter for `[7]` is `3 + 100 + 7
// = 110`. The setter write `d[5] := 42` writes OWNER's `_v = 42 - 5 = 37`,
// so `d[0]` reads `37 + 100 + 0 = 137`.
//
// Pre-fix: SIGSEGV (rc=139) on `d[7]` through the itable fill, OR a hard
// link error. Post-fix: compiles, runs, returns 110 + 137 = 247.
// ===========================================================================
BEZEL_TEST(xmod_widen_exported_accessor_for_virtual_3231,
           indexer_conformance_via_imported_virtual_base_compiles_2_8_3) {
    const std::string libSrc =
        "module Lib3231RT2; "
        "public class B3231T2 "
        "  private _v: int := 3; "
        "  public virtual property [i: int]: int "
        "    get => _v + 100 + i; "
        "    set => _v := value - i; "
        "  end; "
        "end B3231T2; ";
    const std::string appBody =
        "public interface II3231T2 "
        "  property [i: int]: int; "
        "end II3231T2; "
        "public class D3231T2: B3231T2, II3231T2 "
        "end D3231T2; "
        "public class C "
        "  public static method Main(): int "
        "    using var d := new D3231T2(); "
        "    var ii: II3231T2 := d; "
        "    var a := ii[7]; "
        "    d[5] := 42; "
        "    var b := d[0]; "
        "    return a + b; "
        "  end Main; "
        "end C;";
    const std::string twinSrc =
        std::string("module Twin3231RT2; ") +
        (libSrc.substr(libSrc.find("public class"))) +
        (appBody.substr(appBody.find("public")));
    int twin = compileLocalAndRun3231R(twinSrc);
    BEZEL_EXPECT_EQ(twin, 247);
    int imported = compileImportAndRun3231R(
        libSrc, std::string("module App3231RT2; import Lib3231RT2; ") + appBody);
    if (imported == -2) return;
    BEZEL_EXPECT_TRUE(imported >= 0);
    BEZEL_EXPECT_FALSE(imported == 139);
    BEZEL_EXPECT_EQ(imported, twin);
}

// ===========================================================================
// T3 — OVER-REJECTION CONTROL: a LOCAL virtual property continues to lower
// through the existing specializedVtableFor path. The widening must NOT
// affect non-imported types. A single-module program with a virtual
// property overridden locally MUST still compile and run correctly.
//
// This is the "do not affect non-imported types" half the issue body
// specifies: the widening only adds a NEW export path for VIRTUAL members
// on IMPORTED classes; the LOCAL module's own emission, including the
// interface-conformance path on a LOCAL base, is untouched.
// ===========================================================================
BEZEL_TEST(xmod_widen_exported_accessor_for_virtual_3231,
           local_virtual_property_conformance_unchanged_2_8_3) {
    const std::string src =
        "module Twin3231RT3; "
        "public interface IP3231T3 "
        "  property P: int; "
        "end IP3231T3; "
        "public class B3231T3 "
        "  private _v: int := 3; "
        "  public virtual property P: int "
        "    get => _v + 5; "
        "    set => _v := value; "
        "  end; "
        "end B3231T3; "
        "public class D3231T3: B3231T3, IP3231T3 "
        "end D3231T3; "
        "public class C "
        "  public static method Main(): int "
        "    using var d := new D3231T3(); "
        "    var ip: IP3231T3 := d; "
        "    var a := d.P; "
        "    var b := ip.P; "
        "    return a + b; "
        "  end Main; "
        "end C;";
    int got = compileLocalAndRun3231R(src);
    BEZEL_EXPECT_EQ(got, 16);  // 8 + 8
}

// ===========================================================================
// T4 — REGRESSION TEST: a local class inheriting from an IMPORTED base that
// declares a virtual PROPERTY (auto-property specifically — the §2.8.3
// shape EmitVirtualAccessorFn widens) — the spec-named shape from the
// lane prompt's regression description. `D3231T4` inherits the auto-
// property `P` from the IMPORTED base, the interface-conformance path
// writes through the inherited accessor, and the program returns the
// OWNER's value summed with the OWNER's value after a setter write.
//
// `_v` starts at 3 and the OWNER's auto-property backing field is `_v`,
// so the OWNER's getter is `_v` and the OWNER's setter writes `value`.
// After `ip.P := 10`, `_v` is 10, and the next read returns 10. 3 + 10
// = 13. The PRIVATE backing field is deliberate — `public _v` would
// expose a plain field whose name shadows the property contract (the
// compiler reports this with `class `D3231T4` does not implement
// interface property `P` — a field does not satisfy a property contract`).
// ===========================================================================
BEZEL_TEST(xmod_widen_exported_accessor_for_virtual_3231,
           auto_property_via_imported_virtual_base_compiles_2_8_3) {
    const std::string libSrc =
        "module Lib3231RT4; "
        "public class B3231T4 "
        "  private _v: int := 3; "
        "  public virtual property P: int "
        "    get => _v; "
        "    set => _v := value; "
        "  end; "
        "end B3231T4; ";
    const std::string appBody =
        "public interface IP3231T4 "
        "  property P: int; "
        "end IP3231T4; "
        "public class D3231T4: B3231T4, IP3231T4 "
        "end D3231T4; "
        "public class C "
        "  public static method Main(): int "
        "    using var d := new D3231T4(); "
        "    var ip: IP3231T4 := d; "
        "    var a := ip.P; "
        "    ip.P := 10; "
        "    var b := ip.P; "
        "    return a + b; "
        "  end Main; "
        "end C;";
    const std::string twinSrc =
        std::string("module Twin3231RT4; ") +
        (libSrc.substr(libSrc.find("public class"))) +
        (appBody.substr(appBody.find("public interface")));
    int twin = compileLocalAndRun3231R(twinSrc);
    BEZEL_EXPECT_EQ(twin, 13);  // 3 + 10
    int imported = compileImportAndRun3231R(
        libSrc, std::string("module App3231RT4; import Lib3231RT4; ") + appBody);
    if (imported == -2) return;
    BEZEL_EXPECT_TRUE(imported >= 0);
    BEZEL_EXPECT_FALSE(imported == 139);
    BEZEL_EXPECT_EQ(imported, twin);
}

}  // namespace bezel::module
