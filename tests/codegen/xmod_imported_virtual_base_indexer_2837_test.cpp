// Codegen — fix #2837: cross-module `base[i]` on an IMPORTED VIRTUAL base
// indexer routes through the OWNER's NON-virtual direct entry, not the
// vtable-dispatch wrapper that would run the override.
//
// Spec citation: §12.3 (`base.Member`), §23.4 (Indexers), §26.5
// (resolution), v0.282.
//
// ── THE DEFECT ─────────────────────────────────────────────────────────────
//
// The single-module form of `base[i]` on a virtual indexer is closed by
// #2832: a non-virtual dispatch to the OWNER's body. The cross-module form
// is what this issue tracks. Without the ABI addition, the importer has a
// single entry point for an imported virtual indexer — the
// `bz$<Owner>$vt$idxget$<arity>$<slot>` vtable-dispatch wrapper (#2545),
// which loads the vtable slot at call time and forwards — so any call
// through it from `base[i]` runs the OVERRIDE (where §12.3 forbids it).
//
// #2832's interim fix rejected the shape with a clean compile error
// ("cannot lower `base[...]` against the imported virtual indexer on
// `<Owner>` — §12.3 requires the base class's own implementation, but the
// cross-module indexer ABI exposes no direct entry for an ABSTRACT virtual
// member"), turning the silent wrong value into a clean compile error.
// #2837 is the ABI addition that closes the gap.
//
// ── THE FIX ───────────────────────────────────────────────────────────────
//
// #3231's M6 widening: the EXPORTER side (`emitExportedIndexerGetters`
// and `-Setters`) was widened to emit ExternalLinkage
// `bz$<Class>$idxget$<arity>` / `$idxset$<arity>` wrappers for VIRTUAL,
// non-override, non-abstract indexers — the OWNER's body, the same
// direct-entry ABI the non-virtual case uses. The IMPORTER side
// (`declareImportedIndexerGetters` / `-Setters`) now declares the matching
// external in a NEW direct-entry map (`importedIndexerGetterDirectFns_`
// and `importedIndexerSetterDirectFns_`), populated alongside the
// existing dispatch wrapper.
//
// The CALL SIDE (`lowerIndexerGet` / `-Set` / `-Compound`, all three —
// `lowerIndexerCompoundCore` is the third caller) routes `base[i]` (and
// `base[i] := v` and `base[i] OP= v`) through the new direct-entry map
// BEFORE the `rejectImportedVirtualBaseIndexer` gate. When the OWNER
// body exists (non-override, non-abstract), the call goes directly to
// the OWNER's implementation. When the OWNER body is absent
// (an ABSTRACT virtual indexer), the direct-entry map is empty and the
// reject path fires — which is the §12.5-correct behaviour: an abstract
// base member has no implementation to call from `base[i]` either way.
//
// ── WHAT THIS TEST PINS ───────────────────────────────────────────────────
//
// The issue's own shape, verbatim — a cross-module base indexer with the
// OWNER's body, overridden in the derived class, accessed via `base[i]`
// from a derived method. The pre-#3231 measurement was 201 (the override's
// value); §12.3 requires 101 (the OWNER's value). The post-#3231
// measurement is 101 — the OWNER's own body, non-virtually.
//
// All three access shapes (read, write, compound) are pinned, plus the
// accept controls (the same virtual imported base read through `this[i]`
// and through an ordinary typed receiver, both of which must continue to
// dispatch virtually — the fix must not regress the inherited-but-not-
// overridden, the itable-fill, or the non-`base` access paths), plus the
// reject control (an ABSTRACT imported virtual base indexer via `base[i]`
// still rejects — §12.5-correct, no OWNER body exists either way).
//
// Pre-#3231 measurement, verbatim from the issue body (6eaaecf):
//   d.ViaBase(1) printed 201 (the override) where §12.3 requires 101.
// Post-#3231 measurement:
//   d.ViaBase(1) printed 101 (the OWNER's body).

#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <vector>


namespace bezel::codegen {
namespace {

struct TempFile2837 {
    std::string path;
    explicit TempFile2837(std::string p) : path(std::move(p)) {}
    ~TempFile2837() { std::remove(path.c_str()); }
};

void writeSource2837(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Compile lib -> object, compile app (importing lib) -> exe, run it in a
// separate process, return its exit code. -1 on compile/link/run failure,
// -2 if the dependency itself could not be produced (toolchain
// unavailable).
int compileImportAndRun2837(const std::string& libSrc,
                            const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2837_");
    TempFile2837 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2837 aObj((dir / (stem + "_a.o")).string());
    writeSource2837(aSrc.path, libSrc);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success ||
            !std::ifstream(aObj.path).good())
            return -2;
    }
    TempFile2837 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2837 bExe((dir / stem).string());
    writeSource2837(bSrc.path, appSrc);
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

// Single-module twin runner: the SAME declarations with the module
// boundary removed. The twin is the oracle — the cross-module build must
// agree with the single-module build.
int compileLocalAndRun2837(const std::string& src) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2837l_");
    TempFile2837 aSrc((dir / (stem + ".bzl")).string());
    TempFile2837 aExe((dir / stem).string());
    writeSource2837(aSrc.path, src);
    bezel::Driver driver;
    if (!bezel::test::compileFileForTest(driver, aSrc.path, aExe.path).success)
        return -1;
    int status = std::system(aExe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// The OWNER-class declaration: a virtual indexer with a concrete body.
// `_v` starts at 3, so `base[1]` reads 3 + 100 + 1 = 104 (OWNER's body)
// and `base[0] := v` writes `_v := v - 0 = v` (OWNER's setter).
//
// The derived class overrides both accessors with the FORMULA
// `500 + i + _w` (getter) / `_w := value` (setter) so that
// pre-#3231 (which routed through the vtable-slot wrapper that loads the
// derived slot at call time), `d.ViaBase(1)` would return
// `500 + 1 + 0 = 501` instead of the OWNER's `3 + 100 + 1 = 104`.
// Post-#3231 the OWNER-direct route returns 104, NOT 501.
const char* kLib2837 =
    "public class B2837 "
    "  protected _v: int := 3; "
    "  public virtual property [i: int]: int "
    "    get => _v + 100 + i; "
    "    set => _v := value - i; "
    "  end; "
    "end B2837; ";

}  // namespace

// ===========================================================================
// T1 — the issue's HEADLINE repro. A LOCAL class `D2837` inherits from the
// IMPORTED base `B2837`, OVERRIDES the virtual indexer, and exposes
// `ViaBase(i)` that returns `base[i]`. §12.3 requires the OWNER's body:
// 104 (3 + 100 + 1), not 501 (500 + 1 + 0 — the override).
//
// Pre-#3231: 501 (the override, dispatched through the vtable-slot wrapper).
// Post-#3231: 104 (the OWNER's body, via the direct-entry map).
// ===========================================================================
BEZEL_TEST(xmod_imported_virtual_base_indexer_2837,
           base_indexer_read_runs_the_owner_body_12_3) {
    const std::string appBody =
        "public class D2837: B2837 "
        "  private _w: int := 0; "
        "  public override property [i: int]: int "
        "    get => 500 + i + _w; "
        "    set => _w := value; "
        "  end; "
        "  public method ViaBase(i: int): int => base[i]; "
        "end D2837; "
        "public class C "
        "  public static method Main(): int "
        "    using var d := new D2837(); "
        "    return d.ViaBase(1); "
        "  end Main; "
        "end C; ";
    int localTwin = compileLocalAndRun2837(
        std::string("module Twin2837T1; ") + kLib2837 + appBody);
    BEZEL_EXPECT_EQ(localTwin, 104);
    int imported = compileImportAndRun2837(
        std::string("module Lib2837T1; ") + kLib2837,
        std::string("module App2837T1; import Lib2837T1; ") + appBody);
    if (imported == -2) return;  // dependency toolchain unavailable
    BEZEL_EXPECT_TRUE(imported >= 0);
    BEZEL_EXPECT_FALSE(imported == 139);
    // §12.3: base[i] must return the OWNER's body (104), NOT the override
    // (501), and the cross-module build must agree with the single-module
    // twin.
    BEZEL_EXPECT_EQ(imported, 104);
    BEZEL_EXPECT_EQ(imported, localTwin);
}

// ===========================================================================
// T2 — the WRITE half. `base[i] := v` must run the OWNER's setter
// (`_v := v - i`), not the override's (`_w := v`). Pre-#3231 the
// vtable-slot wrapper loaded the derived's slot, and the write went to
// `_w`, leaving `_v` untouched (the read-back then sees the OWNER's
// pre-write `_v = 3` and prints `3 + 100 + 0 = 103`). Post-#3231 the
// direct-entry writes the OWNER's slot, so `_v := 9` and the read-back
// sees `9 + 100 + 0 = 109`.
// ===========================================================================
BEZEL_TEST(xmod_imported_virtual_base_indexer_2837,
           base_indexer_write_runs_the_owner_setter_12_3) {
    const std::string appBody =
        "public class D2837: B2837 "
        "  private _w: int := 0; "
        "  public override property [i: int]: int "
        "    get => 500 + i + _w; "
        "    set => _w := value; "
        "  end; "
        "  public method WriteAndRead(): int "
        "    base[0] := 9; "
        "    return base[0]; "
        "  end; "
        "end D2837; "
        "public class C "
        "  public static method Main(): int "
        "    using var d := new D2837(); "
        "    return d.WriteAndRead(); "
        "  end Main; "
        "end C; ";
    int localTwin = compileLocalAndRun2837(
        std::string("module Twin2837T2; ") + kLib2837 + appBody);
    BEZEL_EXPECT_EQ(localTwin, 109);  // OWNER: _v := 9 - 0 = 9; then 9 + 100 + 0
    int imported = compileImportAndRun2837(
        std::string("module Lib2837T2; ") + kLib2837,
        std::string("module App2837T2; import Lib2837T2; ") + appBody);
    if (imported == -2) return;
    BEZEL_EXPECT_TRUE(imported >= 0);
    BEZEL_EXPECT_EQ(imported, 109);
    BEZEL_EXPECT_EQ(imported, localTwin);
}

// ===========================================================================
// T3 — the COMPOUND half (`lowerIndexerCompoundCore`, the third caller of
// the fix path). `base[0] += 5` reads through OWNER's getter (3 + 100 + 0
// = 103), adds 5 (108), writes through OWNER's setter (`_v := 108 - 0 =
// 108`). Pre-#3231 this read+write through the vtable-slot wrapper both
// dispatched to the override, so `_w := 500 + 0 + 0 + 5 = 505` and the
// read-back of `_w` printed `500 + 0 + 505 = 1005`. Post-#3231 the OWNER's
// slot is the source AND target of the compound; the read-back of `_v`
// after `base[0] += 5` returns 108.
// ===========================================================================
// Note: the compound read uses `base[0]` (which dispatches via the
// direct-entry route after #3231) and the compound write goes through
// the OWNER's setter via the direct-entry map. After `base[0] += 5`, the
// OWNER's `_v` is `_v := (3 + 100 + 0) + 5 - 0 = 108`; the next read
// returns `_v + 100 + 0 = 208`.
// (208 not 108: the OWNER's read-back is post-write, and the OWNER's
// getter reads `_v + 100 + i`.)
BEZEL_TEST(xmod_imported_virtual_base_indexer_2837,
           base_indexer_compound_runs_the_owner_accessors_12_3) {
    const std::string appBody =
        "public class D2837: B2837 "
        "  private _w: int := 0; "
        "  public override property [i: int]: int "
        "    get => 500 + i + _w; "
        "    set => _w := value; "
        "  end; "
        "  public method Bump(): int "
        "    base[0] += 5; "
        "    return base[0]; "
        "  end; "
        "end D2837; "
        "public class C "
        "  public static method Main(): int "
        "    using var d := new D2837(); "
        "    return d.Bump(); "
        "  end Main; "
        "end C; ";
    int localTwin = compileLocalAndRun2837(
        std::string("module Twin2837T3; ") + kLib2837 + appBody);
    // OWNER write: _v := (3 + 100 + 0) + 5 - 0 = 108.
    // OWNER read: 108 + 100 + 0 = 208.
    BEZEL_EXPECT_EQ(localTwin, 208);
    int imported = compileImportAndRun2837(
        std::string("module Lib2837T3; ") + kLib2837,
        std::string("module App2837T3; import Lib2837T3; ") + appBody);
    if (imported == -2) return;
    BEZEL_EXPECT_TRUE(imported >= 0);
    BEZEL_EXPECT_EQ(imported, 208);
    BEZEL_EXPECT_EQ(imported, localTwin);
}

// ===========================================================================
// T4 — ACCEPT CONTROL / §12.1 — `this[i]` on a derived receiver MUST
// dispatch VIRTUALLY (the override wins). Pre-#3231 it already did; post-
// #3231 the fix must not regress it. `this[1]` returns `500 + 1 + 0 = 501`
// on a fresh `D2837`, the override's body. Pinned so the widening cannot
// silently re-route a non-`base` indexer access through the OWNER-direct
// map.
// ===========================================================================
BEZEL_TEST(xmod_imported_virtual_base_indexer_2837,
           this_indexer_still_dispatches_virtually_12_1) {
    const std::string appBody =
        "public class D2837: B2837 "
        "  private _w: int := 0; "
        "  public override property [i: int]: int "
        "    get => 500 + i + _w; "
        "    set => _w := value; "
        "  end; "
        "  public method ViaThis(i: int): int => this[i]; "
        "end D2837; "
        "public class C "
        "  public static method Main(): int "
        "    using var d := new D2837(); "
        "    return d.ViaThis(1); "
        "  end Main; "
        "end C; ";
    int localTwin = compileLocalAndRun2837(
        std::string("module Twin2837T4; ") + kLib2837 + appBody);
    BEZEL_EXPECT_EQ(localTwin, 501);
    int imported = compileImportAndRun2837(
        std::string("module Lib2837T4; ") + kLib2837,
        std::string("module App2837T4; import Lib2837T4; ") + appBody);
    if (imported == -2) return;
    BEZEL_EXPECT_EQ(imported, 501);
    BEZEL_EXPECT_EQ(imported, localTwin);
}

// ===========================================================================
// T5 — ACCEPT CONTROL / §12.1 — an ordinary typed receiver (`b: B2837`)
// MUST dispatch VIRTUALLY too, picking the override at runtime via the
// vtable slot. Pre-#3231 it already did; post-#3231 the fix must not
// regress it. This pins the route: the direct-entry map is consulted ONLY
// when `indexTargetIsBase(ix)` — the `is-base-rooted` predicate — not
// for any base-typed receiver.
// ===========================================================================
BEZEL_TEST(xmod_imported_virtual_base_indexer_2837,
           base_typed_receiver_still_dispatches_virtually_12_1) {
    const std::string appBody =
        "public class D2837: B2837 "
        "  private _w: int := 0; "
        "  public override property [i: int]: int "
        "    get => 500 + i + _w; "
        "    set => _w := value; "
        "  end; "
        "end D2837; "
        "public class C "
        "  public static method Main(): int "
        "    using var d := new D2837(); "
        "    var b: B2837 := d; "
        "    return b[1]; "
        "  end Main; "
        "end C; ";
    int localTwin = compileLocalAndRun2837(
        std::string("module Twin2837T5; ") + kLib2837 + appBody);
    BEZEL_EXPECT_EQ(localTwin, 501);  // override wins via vtable dispatch
    int imported = compileImportAndRun2837(
        std::string("module Lib2837T5; ") + kLib2837,
        std::string("module App2837T5; import Lib2837T5; ") + appBody);
    if (imported == -2) return;
    BEZEL_EXPECT_EQ(imported, 501);
    BEZEL_EXPECT_EQ(imported, localTwin);
}

// ===========================================================================
// T6 — REJECT CONTROL / §12.5 — an ABSTRACT imported virtual base indexer
// has no OWNER body for `base[i]` to call. The widening does NOT add a
// stub for an abstract member (it deliberately skips override/abstract),
// so the direct-entry map is empty for this shape, and the
// `rejectImportedVirtualBaseIndexer` gate fires (clean compile error).
//
// §12.5-correct: an abstract base member has no implementation to call
// from `base[i]` either way, so the rejection is the right answer (it is
// identical to the single-module form's behaviour). This test is the
// over-rejection control — the fix must NOT silently add an empty body
// or a wrong-slot fallback that would compile this and return garbage.
//
// The shape is the issue's family: a LOCAL class `D2837T6` derives from
// the IMPORTED abstract base `AB2837T6` which declares an abstract
// virtual indexer; `D2837T6` overrides the indexer with a concrete body;
// `D2837T6.ViaBase()` returns `base[i]`. Without an OWNER body, the
// direct-entry map is empty and the gate fires.
// ===========================================================================
BEZEL_TEST(xmod_imported_virtual_base_indexer_2837,
           abstract_imported_virtual_base_indexer_via_base_rejected_12_5) {
    const std::string libSrc =
        "public abstract class AB2837T6 "
        "  public abstract property [i: int]: int; "
        "end AB2837T6; ";
    const std::string appBody =
        "public class D2837T6: AB2837T6 "
        "  public override property [i: int]: int => i + 200; "
        "  public method ViaBase(i: int): int => base[i]; "
        "end D2837T6; "
        "public class C "
        "  public static method Main(): int "
        "    using var d := new D2837T6(); "
        "    return d.ViaBase(1); "
        "  end Main; "
        "end C; ";
    int imported = compileImportAndRun2837(
        std::string("module Lib2837T6; ") + libSrc,
        std::string("module App2837T6; import Lib2837T6; ") + appBody);
    if (imported == -2) return;
    // The fix path's gate fires — we get -1 (compile failure) rather than a
    // value, and NOT 201 (which would be the override, dispatched
    // accidentally through a vtable-slot wrapper). Pin both halves: the
    // compile FAILS, and the failure is the abstract-base reject, not the
    // 139-SIGSEGV of a silent wrong value.
    BEZEL_EXPECT_EQ(imported, -1);
}

}  // namespace bezel::codegen