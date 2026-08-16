// fix #2414 (§22.10 / §23.4 / §2.8.2 clause 1 / §2.8.3 rule 4, v0.235):
// `ModuleLoader::convertMember` (ModuleLoader.cpp) had NO
// `case MetaMemberKind::Indexer:` — the comment there read "Indexer / Event
// are part of the schema but deferred." `ModuleMetadataBuilder.cpp` DOES
// export an indexer PropertySymbol (`out.kind = prop->isIndexer ?
// MetaMemberKind::Indexer : MetaMemberKind::Property`), but the importer
// silently dropped it: an imported type's indexer was simply absent from the
// reconstructed `TypeSymbol::members` list, unlike an ordinary Property
// (which at least gets a real PropertySymbol + side-table type, #503).
//
// CONFIRMED SYMPTOM (probed before writing this test, per the issue): the
// issue's own repro — a class `Box` with `property [i: int]: int` — compiles
// clean and runs clean (no crash) but `b[3]` returns `252`, not the correct
// `3`. Root-caused by code read: with no indexer symbol at all,
// sema's user-defined-indexer typing (ExprTypecheck_Exprs.cpp, §23.4) never
// even enters its indexer branch (`anyIndexer` stays false), so the
// IndexExpr is left completely UNTYPED, and codegen's `lowerIndexerGet`
// (`selectIndexer` finds nothing, `ps == nullptr`) falls through to whatever
// stale/uninitialized fallback path SOMETHING downstream reads.
//
// THE FIX: this file's own convertMember Indexer case reconstructs a real
// indexer PropertySymbol (isIndexer=true, indexerParams from mm.params, the
// element type on the existing fieldTypes_ side table). That alone is
// necessary but not sufficient — the reconstructed symbol still has no
// ast::PropertyDecl (`ps->decl == nullptr`, exactly like Property, #503's own
// documented limitation), and three further consumers gated on that decl
// unconditionally:
//   1. sema candidate matching/typing (ExprTypecheck_Exprs.cpp): resolving
//      the indexer OVERLOAD and typing the IndexExpr both dynamic_cast the
//      candidate's `->decl` to an ast::PropertyDecl with no fallback.
//   2. codegen's overload selector (`findIndexer`/`selectIndexer`,
//      LlvmBackend_Assign.cpp): same `!pd -> skip` gate.
//   3. codegen's actual GET/SET lowering (`lowerIndexerGet`/`lowerIndexerSet`):
//      same `!pd -> return null/false` gate, unconditionally, even on the
//      itable/vtable dispatch paths (which read `pd->type`/`pd->indexerParams`
//      for signature info, not just the inline body).
// This fix adds a decl-null fallback at each of those three sites (reading
// the element/param types off the reconstructed side tables — fieldTypes_ /
// each ParameterSymbol's resolvedDeclType — instead of the AST), PLUS a new
// cross-module ABI for a NON-VIRTUAL, non-interface indexer (the issue's own
// confirmed repro shape): `emitExportedIndexerGetters`/`-Setters` (exporter,
// LlvmBackend_Setup.cpp, mirroring `emitExportedPropertyGetters`/`-Setters`
// but reusing the existing `ensureIndexerGetterFn`/`-SetterFn` body
// builders) and `declareImportedIndexerGetters`/`-Setters` (importer,
// mirroring the Property twins) — a `bz$<Class>$idxget$<arity>`/
// `$idxset$<arity>` external ABI, keyed by CLASS + ARITY (not the full
// index-parameter type signature — see the file-level limitation note below).
//
// SCOPE / WHAT REMAINS BROKEN (probed, NOT fixed here, filed separately):
// The itable-dispatched shape (an indexer reached through a LOCAL class
// satisfying an IMPORTED interface's indexer obligation) was the deferral
// #2414 filed as #2422, now LANDED — T5/T6/T7 below assert its runtime VALUE
// (get, set-then-get, multi-dim), and T4 remains as the compile-only
// accept-control. Not addressed by either #2414 or #2422: overloaded indexers
// of the SAME arity but different parameter TYPES exported cross-module
// collide on the same `bz$<Class>$idxget$<arity>` symbol (a documented
// simplification, not exercised by any test here — no spec example needs it);
// and a GENERIC interface indexer whose element type is a type parameter
// (e.g. `IFlag<T>` read as `bool`) erases to i32 in the itable-dispatch
// return type for BOTH local and imported interfaces — pre-existing, filed
// separately (not a #2422 regression: the local twin fails identically).
//
// CONSTANTS: kept < 256 throughout — `runAot`'s (and a plain child-process)
// exit status is WEXITSTATUS-masked to 8 bits (see #2401's own test file for
// the mistake this guards against).

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "driver/Driver.hpp"
#include "test_harness.hpp"
#include "support/unique_stem.hpp"


namespace bezel::module {
namespace {

struct TempFile2414 {
    std::string path;
    explicit TempFile2414(std::string p) : path(std::move(p)) {}
    ~TempFile2414() { std::remove(path.c_str()); }
};

void writeSource2414(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj2414(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// Compile lib -> object, compile app (importing lib) -> exe, run the exe in a
// SEPARATE process (fork-isolated per tests/README.md's AOT harness
// discipline), and return its exit code (-1 on compile/link/run failure, -2
// if the dependency itself could not be produced). Mirrors #2401's
// compileImportAndRun2401.
int compileImportAndRun2414(const std::string& libSrc,
                            const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2414_");
    TempFile2414 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2414 aObj((dir / (stem + "_a.o")).string());
    writeSource2414(aSrc.path, libSrc);
    if (!compileToObj2414(aSrc.path, aObj.path)) return -2;
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile2414 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2414 bExe((dir / stem).string());
    writeSource2414(bSrc.path, appSrc);
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

bool compileImportSucceeds2414(const std::string& libSrc,
                               const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2414_ok_");
    TempFile2414 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2414 aObj((dir / (stem + "_a.o")).string());
    writeSource2414(aSrc.path, libSrc);
    if (!compileToObj2414(aSrc.path, aObj.path)) return false;
    if (!std::ifstream(aObj.path).good()) return false;

    TempFile2414 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2414 bExe((dir / (stem + "_b")).string());
    writeSource2414(bSrc.path, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    return driver.compileFile(bSrc.path, bExe.path, deps).success;
}

}  // namespace

// ===========================================================================
// T1 — the exact #2414 repro: a GET through a plain (non-interface) imported
// class's indexer. Pre-fix: compiles/runs clean but returns 252. Post-fix:
// 3 (`_v (0) + i (3)`, before any set — matches the issue's own expectation).
// ===========================================================================
BEZEL_TEST(xmod_indexer_import_2414, get_through_imported_class_indexer) {
    int r = compileImportAndRun2414(
        "module IdxLib2414T1; "
        "public class Box2414T1 "
        "  private _v: int := 0; "
        "  public property [i: int]: int "
        "    get => _v + i; "
        "    set => _v := value - i; "
        "  end; "
        "end Box2414T1;",
        "module App2414T1; import IdxLib2414T1; "
        "public class C "
        "  public static method Main(): int "
        "    using var b := new Box2414T1(); "
        "    return b[3]; "
        "  end Main; "
        "end C;");
    if (r == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 3);
}

// ===========================================================================
// T2 — SET then GET (write-reachable, per the protocol's "read AND write"
// requirement): `b[5] := 9` sets `_v := 9 - 5 = 4`; a subsequent `b[2]` reads
// `_v (4) + 2 = 6`. Guards against a fix that reconstructs the getter ABI
// but not the setter (mirrors #2400's own T5 "direct shape" distinction).
// ===========================================================================
BEZEL_TEST(xmod_indexer_import_2414, set_then_get_through_imported_class_indexer) {
    int r = compileImportAndRun2414(
        "module IdxLib2414T2; "
        "public class Box2414T2 "
        "  private _v: int := 0; "
        "  public property [i: int]: int "
        "    get => _v + i; "
        "    set => _v := value - i; "
        "  end; "
        "end Box2414T2;",
        "module App2414T2; import IdxLib2414T2; "
        "public class C "
        "  public static method Main(): int "
        "    using var b := new Box2414T2(); "
        "    b[5] := 9; "
        "    return b[2]; "
        "  end Main; "
        "end C;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 6);
}

// ===========================================================================
// T3 — CONTROL: a LOCAL (no import at all) class indexer, matching the shape
// of T1/T2 exactly. Guards against the fix regressing ordinary same-module
// indexer access (already covered by tests/*/interface_indexer_401 and
// collection_indexer_388-style suites — this pins the plain-class shape
// specifically, in the same file as the cross-module cases it mirrors).
// ===========================================================================
BEZEL_TEST(xmod_indexer_import_2414, local_class_indexer_control_unaffected) {
    bezel::Driver driver;
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2414_t3_");
    TempFile2414 src((dir / (stem + ".bzl")).string());
    TempFile2414 exe((dir / stem).string());
    writeSource2414(src.path,
                    "module LocalCtl2414T3; "
                    "public class BoxL2414T3 "
                    "  private _v: int := 0; "
                    "  public property [i: int]: int "
                    "    get => _v + i; "
                    "    set => _v := value - i; "
                    "  end; "
                    "end BoxL2414T3; "
                    "public class C "
                    "  public static method Main(): int "
                    "    using var b := new BoxL2414T3(); "
                    "    b[5] := 9; "
                    "    return b[2]; "
                    "  end Main; "
                    "end C;");
    BEZEL_EXPECT_TRUE(driver.compileFile(src.path, exe.path, {}).success);
    int status = std::system(exe.path.c_str());
    int r = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(r, 6);
}

// ===========================================================================
// T4 — ACCEPT-CONTROL (compiles only — mirrors #2401 T5's documented
// deferral pattern): an indexer reached through a LOCAL class satisfying an
// IMPORTED interface's indexer obligation (itable-dispatched shape). The
// runtime VALUE for this shape is now asserted by T5/T6/T7 below (fix #2422);
// this cell stays compile-only to pin that the shape still compiles clean
// independently, so a future regression in the itable-slot-count machinery
// (the #2406-style layout-shift hazard) is at least caught as a compile
// failure here even before value-level coverage runs.
//
// Updated after #2529 (§11.2 / §23.4): the original test used a getter-only
// impl of an imported read-write interface indexer — a shape #2414 needed to
// accept at the time because the iface-indexer obligations walker
// (`checkAbstractImplementations` in src/sema/TypeResolution_Checks.cpp)
// silently SKIPPED the §11.2/§23.4 accessor-presence check for imported
// interface indexers (it gated on `ips->decl`, which is null for any
// imported PropertySymbol — ModuleLoader's Indexer case reconstructs
// `indexerParams` but attaches no `ast::PropertyDecl`). #2529's fix closes
// that gap (mirroring #2380's `ifaceProps` type-compare fallback and the
// candidate-side #2449 fallback the same loop already uses); the original
// test would now (correctly) reject. To preserve T4's accept-control role
// — pin that the itable-dispatched indexer-overload resolution path
// (`#2414`'s actual subject) still compiles cross-module — the test now
// uses a getter+setter impl, the legitimate read-write indexer shape, and
// the getter-only-vs-readwrite reject for an imported interface indexer is
// pinned by the new #2529 tests (`xmod_iface_indexer_setter_2529`).
// ===========================================================================
BEZEL_TEST(xmod_indexer_import_2414,
           indexer_through_imported_interface_still_compiles_accept_control) {
    bool ok = compileImportSucceeds2414(
        "module IdxIfaceLib2414T4; "
        "public interface IIdx2414T4 "
        "  property [i: int]: int; "
        "end IIdx2414T4;",
        "module App2414T4; import IdxIfaceLib2414T4; "
        "public class Impl2414T4: IIdx2414T4 "
        "  private _v: int := 7; "
        "  public property [i: int]: int "
        "    get => _v + i; "
        "    set => _v := value - i; "
        "  end; "
        "end Impl2414T4; "
        "public class C "
        "  public static method Main(): int "
        "    using var obj := new Impl2414T4(); "
        "    var b: IIdx2414T4 := obj; "
        "    return b[1]; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(ok);
}

// ===========================================================================
// T5 — fix #2422 (§23.4 / §11.2 / §2.8.2 clause 1, v0.236): the VALUE-level
// twin of T4 — a GET reached through a LOCAL class satisfying an IMPORTED
// interface's indexer obligation (itable-dispatched, the #2414 deferral).
// Pre-fix: compiles/runs clean but the IndexExpr failed to lower — the
// interface-dispatch branch in `lowerIndexerGet` read `pd->type`/
// `pd->indexerParams` unconditionally, and an IMPORTED interface's indexer
// has no ast::PropertyDecl (`pd == null`), so the `!pd` gate just above it
// returned nullptr and the call degraded to a zero return (#2491): exit 0,
// not 8. Post-fix: `8` (`_v (7) + i (1)`), the issue's own confirmed
// expectation. Confirmed mechanically: patch out → r==0; patch in → r==8.
// ===========================================================================
BEZEL_TEST(xmod_indexer_import_2414, get_through_imported_interface_indexer_2422) {
    int r = compileImportAndRun2414(
        "module IdxIfaceLib2422T5; "
        "public interface IIdx2422T5 "
        "  property [i: int]: int; "
        "end IIdx2422T5;",
        "module App2422T5; import IdxIfaceLib2422T5; "
        "public class Impl2422T5: IIdx2422T5 "
        "  private _v: int := 7; "
        "  public property [i: int]: int "
        "    get => _v + i; "
        "    set => _v := value - i; "
        "  end; "
        "end Impl2422T5; "
        "public class C "
        "  public static method Main(): int "
        "    using var obj := new Impl2422T5(); "
        "    var b: IIdx2422T5 := obj; "
        "    return b[1]; "
        "  end Main; "
        "end C;");
    if (r == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 8);
}

// ===========================================================================
// T6 — fix #2422 (§23.4 / §11.2): the SETTER twin of T5 — SET then GET
// through the IMPORTED interface (itable-dispatched both ways). `b[5] := 9`
// sets `_v := 9 - 5 = 4`; a subsequent `b[2]` reads `_v (4) + 2 = 6`. Guards
// against a fix that reconstructs the itable-dispatch getter but not the
// setter. Pre-fix: r==0 (both the set and the read degraded); post-fix: 6.
// ===========================================================================
BEZEL_TEST(xmod_indexer_import_2414, set_then_get_through_imported_interface_indexer_2422) {
    int r = compileImportAndRun2414(
        "module IdxIfaceLib2422T6; "
        "public interface IIdx2422T6 "
        "  property [i: int]: int; "
        "end IIdx2422T6;",
        "module App2422T6; import IdxIfaceLib2422T6; "
        "public class Impl2422T6: IIdx2422T6 "
        "  private _v: int := 7; "
        "  public property [i: int]: int "
        "    get => _v + i; "
        "    set => _v := value - i; "
        "  end; "
        "end Impl2422T6; "
        "public class C "
        "  public static method Main(): int "
        "    using var obj := new Impl2422T6(); "
        "    var b: IIdx2422T6 := obj; "
        "    b[5] := 9; "
        "    return b[2]; "
        "  end Main; "
        "end C;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 6);
}

// ===========================================================================
// T7 — fix #2422 (§23.4 / §11.2): a MULTI-DIM indexer reached through an
// IMPORTED interface — `g[3, 4]` returns `(3 * 10) + 4 = 34`. Guards that the
// fix's `indexerParamCount`/`indexerParamType` fallback scales past arity 1
// (the itable slot carries a `T get(ptr this, int, int)` signature, built by
// iterating the reconstructed index-param side table).
// ===========================================================================
BEZEL_TEST(xmod_indexer_import_2414, multidim_get_through_imported_interface_indexer_2422) {
    int r = compileImportAndRun2414(
        "module IdxIfaceLib2422T7; "
        "public interface IGrid2422T7 "
        "  property [i: int, j: int]: int; "
        "end IGrid2422T7;",
        "module App2422T7; import IdxIfaceLib2422T7; "
        "public class Grid2422T7: IGrid2422T7 "
        "  private _v: int := 0; "
        "  public property [i: int, j: int]: int "
        "    get => (i * 10) + j; "
        "    set => _v := value; "
        "  end; "
        "end Grid2422T7; "
        "public class C "
        "  public static method Main(): int "
        "    using var obj := new Grid2422T7(); "
        "    var g: IGrid2422T7 := obj; "
        "    return g[3, 4]; "
        "  end Main; "
        "end C;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 34);
}

// ==========================================================================
// T8 — fix #2528 (§23.4 / §11.2 / §14.1, v0.236): a generic interface
// indexer's concrete element type must survive imported itable dispatch. The
// interface metadata carries `T`, while the receiver is `IFlag<bool>`; the
// branch discriminator proves the call is typed as bool/i1, not erased i32.
// Pre-fix this is the imported twin of the local malformed-IR repro.
// ==========================================================================
BEZEL_TEST(xmod_indexer_import_2414,
           generic_interface_bool_indexer_2528) {
    int r = compileImportAndRun2414(
        "module IdxIfaceLib2528T8; "
        "public interface IFlag2528T8<T> "
        "  property [i: int]: T; "
        "end IFlag2528T8;",
        "module App2528T8; import IdxIfaceLib2528T8; "
        "public class Flags2528T8: IFlag2528T8<bool> "
        "  private _b: bool := false; "
        "  public property [i: int]: bool "
        "    get => i > 2; "
        "    set => _b := value; "
        "  end; "
        "end Flags2528T8; "
        "public class C "
        "  public static method Main(): int "
        "    using var obj := new Flags2528T8(); "
        "    var f: IFlag2528T8<bool> := obj; "
        "    const g: bool := f[5]; "
        "    if g then return 1; else return 0; end if; "
        "  end Main; "
        "end C;");
    if (r == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 1);
}

}  // namespace bezel::module
