// Spec-derived regression test for issue #2946.
//
// §23.4 (Indexers) / §13 (Interfaces) / §11.2 / §2.5 / §2.8.3 rule 4 (one
// runtime type identity per monomorphization) / §14.1 (Generic declarations),
// v0.240.
//
// THE DEFECT — an EXPORTER/IMPORTER ASYMMETRY, the INDEXER twin of the one
// #2894 had to close for named properties. `emitExportedIndexerGetters` /
// `-Setters` carried their own `genericParams.empty()` gate, so no
// `bz$<Class>$idxget$<arity>` / `$idxset$<arity>` symbol was ever defined for a
// public GENERIC class — while `declareImportedIndexerGetters`/`-Setters`
// carried no such gate and DECLARED the stub anyway. Two distinct failures
// followed, both measured on this fix's parent with a real two-module build
// (`bezelc -c lib.bzl`, then `bezelc --import lib.o app.bzl`):
//
//   * through an INTERFACE (T1/T2): #2894 now builds the per-instantiation
//     itmap, but the indexer slot was excluded from its fill precisely because
//     filling it would have been an unresolved relocation in an initialized
//     global. So the itable lookup succeeds and the NULL slot is called:
//
//         AddressSanitizer: SEGV on unknown address 0x0 (pc 0x0) ... in main
//         Segmentation fault (core dumped)      exit=139
//
//   * DIRECTLY, with no interface anywhere in the program (T3) — this one is
//     NOT in the issue and was found while probing the axis grid. The importer
//     emits an ordinary cross-module call to the declared-but-never-defined
//     stub, so the program does not even LINK:
//
//         undefined reference to `bz$LIdx4.GDirect4$idxget$1'
//         undefined reference to `bz$LIdx4.GDirect4$idxset$1'
//
//     i.e. a public generic class's indexer had no cross-module surface at all,
//     interface or not. That is the same defect one step earlier in the
//     pipeline, and it is fixed by the same exporter half.
//
// THE FIX has two halves, exactly as #2894's did:
//   * EXPORTER: `emitExportedIndexer{Getters,Setters}` now also emit for a
//     public GENERIC class. The body is the ERASED `T -> i32` template
//     accessor, which is what the importer is permitted to call under the gate
//     below. (Third widening of this same gate: Class-only → Class+Record was
//     #2474/#2489, → +generic is this one.)
//   * IMPORTER: `buildImportedGenericVtable` fills the interface indexer slot
//     via `indexerItableAccessor` — the same helper Pass D uses for a local
//     class — under TWO independent gates: the instantiation's layout must
//     match the erased template, and the resolved accessor's ABI must still fit
//     the types lowered under THIS substitution.
//
// THE ABI GATE IS PER-TYPE, NOT PER-ARITY, and that is the point. An indexer's
// ABI is `T get(ptr this, idx...)`, so an INDEX PARAMETER is its own way for
// the ABI to move — `property [i: T]: int` moves one while leaving both the
// storage and the indexer's own type identical. Checking only the parameter
// COUNT would be a correct predicate over an incomplete population, the shape
// this family keeps producing.
//
// Built the same way as its sibling tests/module/xmod_imported_generic_iface_
// accessor_2894_test.cpp: a real `bezelc -c` + `--import` + a native run.
// Values are kept < 256 where the observable is the exit status.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "test_harness.hpp"
#include "support/unique_stem.hpp"


namespace bezel::module {
namespace {

struct TempFile2946 {
    std::string path;
    explicit TempFile2946(std::string p) : path(std::move(p)) {}
    ~TempFile2946() { std::remove(path.c_str()); }
};

void writeSource2946(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj2946(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// -2 = the dependency toolchain is unavailable (test skips); -1 = the app did
// not build/link OR did not exit normally (a SIGSEGV lands here too — which is
// exactly what T4 is allowed to observe and no other case is).
int compileImportAndRun2946(const std::string& libSrc,
                            const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2946_");
    TempFile2946 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2946 aObj((dir / (stem + "_a.o")).string());
    writeSource2946(aSrc.path, libSrc);
    if (!compileToObj2946(aSrc.path, aObj.path)) return -2;
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile2946 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2946 bExe((dir / stem).string());
    writeSource2946(bSrc.path, appSrc);
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
// T1 — THE ISSUE'S OWN REPRO: a getter-only interface INDEXER on an imported
// generic, read through the interface type. The issue states 42 is correct.
// Pre-fix: exit 139, a call through the null itable slot. §23.4 / §13 / §11.2.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_indexer_2946, iface_indexer_get_spec_23_4) {
    const char* lib =
        "module GIdxLib2946T1; "
        "public interface IAt2946T1 "
        "  property [i: int]: int get; end; "
        "end IAt2946T1; "
        "public class GArr2946T1<T>: IAt2946T1 "
        "  public _w: int := 40; "
        "  public property [i: int]: int "
        "    get => _w + i; "
        "  end; "
        "end GArr2946T1;";
    int r = compileImportAndRun2946(
        lib,
        "module GIdxApp2946T1; import GIdxLib2946T1; "
        "public class M2946T1 "
        "  public static method Main(): int "
        "    using var g := new GArr2946T1<int>(); "
        "    var r: IAt2946T1 := g; "
        "    return r[2]; "
        "  end Main; "
        "end M2946T1;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 42);
}

// ===========================================================================
// T2 — THE SETTER ARM, at an instantiation (`T = string`) that would move the
// layout if the class had any `T`-typed storage — which it deliberately does
// NOT. That is what shows the gate is a LAYOUT property and not "T must be
// int" (#2894's T2 makes the same point for a named property).
//
// The neighbouring `string` field is read back after the write: the difference
// between "the setter no longer crashes" and "the setter stored at the right
// offset". A wrong-offset store would clobber `note` and leave `_w` stale.
// §23.4 / §23.3 / §4.6.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_indexer_2946, iface_indexer_get_set_spec_23_4) {
    const char* lib =
        "module GIdxLib2946T2; "
        "public interface IAt2946T2 "
        "  property [i: int]: int get; set; end; "
        "end IAt2946T2; "
        "public class GArr2946T2<T>: IAt2946T2 "
        "  public _w: int := 40; "
        "  public note: string := \"keep\"; "
        "  public property [i: int]: int "
        "    get => _w + i; "
        "    set => do _w := value + i; end; "
        "  end; "
        "end GArr2946T2;";
    int r = compileImportAndRun2946(
        lib,
        "module GIdxApp2946T2; import GIdxLib2946T2; "
        "public class M2946T2 "
        "  public static method Main(): int "
        "    using var g := new GArr2946T2<string>(); "
        "    var r: IAt2946T2 := g; "
        "    if r[2] <> 42 then return 91; end; "
        "    r[3] := 5; "
        "    if g._w <> 8 then return 92; end; "
        "    if g.note.Length <> 4 then return 93; end; "
        "    return r[2]; "
        "  end Main; "
        "end M2946T2;");
    if (r == -2) return;
    // _w := 5 + 3 = 8, then r[2] = 8 + 2.
    BEZEL_EXPECT_EQ(r, 10);
}

// ===========================================================================
// T3 — NO INTERFACE ANYWHERE. A public generic class's indexer used DIRECTLY
// across the module boundary. This is not in the issue; it was found probing
// the axis grid, and it is the same exporter gap one step earlier: pre-fix the
// program did not LINK (`undefined reference to bz$…$idxget$1` /
// `$idxset$1`), because the importer emits an ordinary cross-module call to a
// stub the exporter never defined.
//
// It is pinned here rather than filed separately because the EXPORTER half of
// #2946's fix is exactly what closes it, and a future narrowing of that gate
// would silently reintroduce a link failure that no interface test would
// catch. §2.5 / §23.4 / §14.1.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_indexer_2946, direct_cross_module_indexer_links_spec_2_5) {
    const char* lib =
        "module GIdxLib2946T3; "
        "public class GDirect2946T3<T> "
        "  public _w: int := 40; "
        "  public property [i: int]: int "
        "    get => _w + i; "
        "    set => do _w := value + i; end; "
        "  end; "
        "end GDirect2946T3;";
    int r = compileImportAndRun2946(
        lib,
        "module GIdxApp2946T3; import GIdxLib2946T3; "
        "public class M2946T3 "
        "  public static method Main(): int "
        "    using var g := new GDirect2946T3<int>(); "
        "    if g[2] <> 42 then return 91; end; "
        "    g[3] := 5; "
        "    return g[2]; "
        "  end Main; "
        "end M2946T3;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 10);
}

// ===========================================================================
// T4 — THE REFUSAL BOUNDARY, pinned as a RANGE for #2894 T3's reason.
// `GMove2946T4<T>` declares TWO `T`-typed fields before `_w`, so at
// `T = string` the instantiation's layout MOVES relative to the erased template
// the exporter compiled the indexer's accessor against. The fill is REFUSED
// there, because the only alternative — installing the template accessor
// anyway — turns a crash into a silent wrong answer (#2896 measured 44 where
// 149 was correct).
//
// ⚠ MAINTENANCE NOTE. This case accepts EITHER outcome that is honest:
//   * -1, today's behaviour: the slot stays null and the program faults;
//   * 42, the correct answer, if importer-side monomorphization of accessor
//     bodies (the #428 core, tracked for the property twin as #2943) later
//     lands and makes the fill valid here.
// It fails on ANY OTHER value — which is the regression it exists to catch: a
// change that installs the erased accessor for a layout-moving instantiation
// returns a plausible-looking WRONG number with no crash to announce it. Do NOT
// "simplify" this to EXPECT_EQ(-1); that would fail the day the residual is
// genuinely fixed.
//
// The premise is that the two layouts genuinely differ, and TWO `T` fields are
// load-bearing: with a single one the i32 placeholder and the `ptr` share an
// 8-byte-aligned slot and `_w` lands at the same offset either way — the
// #2906/#2907 vacuity trap. Do not narrow it.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_indexer_2946, layout_moving_indexer_not_silently_wrong_spec_14_1) {
    const char* lib =
        "module GIdxLib2946T4; "
        "public interface IAt2946T4 "
        "  property [i: int]: int get; end; "
        "end IAt2946T4; "
        "public class GMove2946T4<T>: IAt2946T4 "
        "  public _t: T; "
        "  public _t2: T; "
        "  public _w: int := 40; "
        "  public property [i: int]: int "
        "    get => _w + i; "
        "  end; "
        "end GMove2946T4;";
    int r = compileImportAndRun2946(
        lib,
        "module GIdxApp2946T4; import GIdxLib2946T4; "
        "public class M2946T4 "
        "  public static method Main(): int "
        "    using var g := new GMove2946T4<string>(); "
        "    var r: IAt2946T4 := g; "
        "    return r[2]; "
        "  end Main; "
        "end M2946T4;");
    if (r == -2) return;
    BEZEL_EXPECT_TRUE(r == -1 || r == 42);
}

// ===========================================================================
// T5 — CONTROL, for the EXPORTER half. A public NON-generic imported class with
// an interface indexer was already correct and must stay correct: widening the
// `genericParams.empty()` gate must not disturb the shape that gate was never
// about. Passes with and without the fix. §2.5 / §13 / §23.4.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_indexer_2946, nongeneric_imported_iface_indexer_unchanged_spec_2_5) {
    const char* lib =
        "module GIdxLib2946T5; "
        "public interface IAt2946T5 "
        "  property [i: int]: int get; set; end; "
        "end IAt2946T5; "
        "public class Plain2946T5: IAt2946T5 "
        "  public _w: int := 40; "
        "  public property [i: int]: int "
        "    get => _w + i; "
        "    set => do _w := value + i; end; "
        "  end; "
        "end Plain2946T5;";
    int r = compileImportAndRun2946(
        lib,
        "module GIdxApp2946T5; import GIdxLib2946T5; "
        "public class M2946T5 "
        "  public static method Main(): int "
        "    using var g := new Plain2946T5(); "
        "    var r: IAt2946T5 := g; "
        "    if r[2] <> 42 then return 91; end; "
        "    r[3] := 5; "
        "    return r[2]; "
        "  end Main; "
        "end M2946T5;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 10);
}

// ===========================================================================
// T6 — TWO INSTANTIATIONS of one imported generic class, live at once and both
// reached through the interface type. §2.8.3 rule 4 gives each monomorphization
// its own runtime identity, so each must carry its OWN itmap; a fill cached
// across instantiations would hand one instantiation's table to the other.
// §2.8.3 rule 4 / §13.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_indexer_2946, two_instantiations_each_get_a_table_spec_2_8_3) {
    const char* lib =
        "module GIdxLib2946T6; "
        "public interface IAt2946T6 "
        "  property [i: int]: int get; set; end; "
        "end IAt2946T6; "
        "public class GArr2946T6<T>: IAt2946T6 "
        "  public _w: int := 40; "
        "  public property [i: int]: int "
        "    get => _w + i; "
        "    set => do _w := value + i; end; "
        "  end; "
        "end GArr2946T6;";
    int r = compileImportAndRun2946(
        lib,
        "module GIdxApp2946T6; import GIdxLib2946T6; "
        "public class M2946T6 "
        "  public static method Main(): int "
        "    using var gi := new GArr2946T6<int>(); "
        "    using var gs := new GArr2946T6<string>(); "
        "    var ri: IAt2946T6 := gi; "
        "    var rs: IAt2946T6 := gs; "
        "    ri[1] := 0; "
        "    rs[2] := 3; "
        "    if gi._w <> 1 then return 91; end; "
        "    if gs._w <> 5 then return 92; end; "
        "    return ri[1] + rs[2]; "
        "  end Main; "
        "end M2946T6;");
    if (r == -2) return;
    // (1 + 1) + (5 + 2)
    BEZEL_EXPECT_EQ(r, 9);
}
}  // namespace bezel::module
