// Spec-derived regression test for issue #2894.
//
// §2.8.3 rule 4 (one runtime type identity per monomorphization) / §12.1
// (Inheritance and dispatch) / §13 (Interfaces) / §11.2 / §23 (Properties and
// Indexers) / §14.1 (Generic declarations) / §2.5, v0.240.
//
// THE DEFECT. `buildImportedGenericVtable` built the per-instantiation itmap as
// a LITERAL one-element `[null]` — "no interfaces" — for EVERY imported generic
// instantiation, whatever the class actually implements. So an instantiation of
// an imported `GIface<T>: IHas` minted in the importing module carried a
// dispatch table answering "I implement nothing": `bz_itable_lookup` returned
// null and the very next `load ptr` from it faulted. Measured with a real
// two-module build on the fix's parent:
//
//     Segmentation fault (core dumped)     exit=139
//     AddressSanitizer: SEGV on unknown address 0x000000000000 ... in main
//
// The equivalent NON-generic program is correct — `property_compound_import_2862`
// pins that — so this is specific to a generic class's per-instantiation table.
//
// §2.8.3 rule 4 requires one runtime type identity per monomorphization
// "regardless of which artifact produced the value", and §11.2's
// interface-matching contract draws no artifact distinction, so an instantiation
// minted importer-side must dispatch exactly as a same-module one does.
//
// THE FIX has two halves, and needed both:
//   * IMPORTER: build the real itmap/itable from the interface slot numbering
//     #2401/#2506 already mirror for imported interfaces, so the importer
//     computes the SAME index k the exporter assigned.
//   * EXPORTER: emitExportedProperty{Getters,Setters} now also emit for a public
//     GENERIC class. The importer's declare half never carried a generic gate,
//     so the DECLARATION already existed — the asymmetry was invisible only
//     because nothing created a RELOCATION to it. An itable is an INITIALIZED
//     GLOBAL, so the first version of this fix produced `undefined reference to
//     bz$L5.GIface$W$propget` at link time; the importer-side fill therefore
//     mirrors the exporter's own gate rather than trusting that a declaration
//     implies a definition.
//
// GATED ON THE LAYOUT PROPERTY. Every function that can go in these slots was
// compiled BY THE EXPORTER against the erased `T -> i32` template layout. When
// an instantiation's storage layout MOVES relative to that template, those
// bodies GEP offsets the object does not have — so the fill is refused there
// and that instantiation keeps today's exact behaviour. Installing them anyway
// would convert a crash into a SILENT WRONG VALUE, which is strictly worse
// (#2896 measured 44 where 149 was correct). T3 is that boundary, pinned.
//
// NOT the same defect as #2928, measured rather than assumed: #2894 still
// exited 139 with #2928's fix applied. #2928 is a WRONG BODY in a filled slot;
// this is an EMPTY slot.
//
// Built the same way as its sibling tests/module/property_compound_import_2862_
// test.cpp: a real `bezelc -c lib.bzl` + `bezelc --import lib.o app.bzl` + a
// native run. Values are kept < 256 where the observable is the exit status.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

struct TempFile2894 {
    std::string path;
    explicit TempFile2894(std::string p) : path(std::move(p)) {}
    ~TempFile2894() { std::remove(path.c_str()); }
};

void writeSource2894(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj2894(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// -2 = the dependency toolchain is unavailable (test skips); -1 = the app did
// not build/link OR did not exit normally (a SIGSEGV lands here too — which is
// exactly what T3 is allowed to observe and no other case is).
int compileImportAndRun2894(const std::string& libSrc,
                            const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2894_" + std::to_string(std::rand());
    TempFile2894 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2894 aObj((dir / (stem + "_a.o")).string());
    writeSource2894(aSrc.path, libSrc);
    if (!compileToObj2894(aSrc.path, aObj.path)) return -2;
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile2894 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2894 bExe((dir / stem).string());
    writeSource2894(bSrc.path, appSrc);
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
// T1 — THE ISSUE'S OWN REPRO, values as the issue states them: `iface_get=100`
// and `iface_set=8` are what it calls correct. Pre-fix: exit 139.
// `GIface<int>` keeps the erased template layout, so the exporter's accessor is
// valid for it. §13 / §11.2 / §2.8.3 rule 4.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_iface_accessor_2894, iface_property_get_set_spec_13) {
    const char* lib =
        "module GIfLib2894T1; "
        "public interface IHas2894T1 "
        "  property W: int get; set; end; "
        "end IHas2894T1; "
        "public class GIface2894T1<T>: IHas2894T1 "
        "  public _t: T; "
        "  public _w: int := 0; "
        "  public property W: int "
        "    get => _w + 100; "
        "    set => do _w := value + 1; end; "
        "  end; "
        "end GIface2894T1;";
    int r = compileImportAndRun2894(
        lib,
        "module GIfApp2894T1; import GIfLib2894T1; "
        "public class M2894T1 "
        "  public static method Main(): int "
        "    using var g := new GIface2894T1<int>(); "
        "    var r: IHas2894T1 := g; "
        "    if r.W <> 100 then return 91; end; "
        "    r.W := 7; "
        "    if g._w <> 8 then return 92; end; "
        "    return r.W; "
        "  end Main; "
        "end M2894T1;");
    if (r == -2) return;
    // 8 + 100 — the getter re-read through the interface after the setter wrote.
    BEZEL_EXPECT_EQ(r, 108);
}

// ===========================================================================
// T2 — A CLASS WITH NO TYPE-PARAMETER-TYPED STORAGE, so its layout is identical
// to the erased template at EVERY instantiation — instantiated at `string`, a
// type that would otherwise move it. This is the case that shows the gate is a
// LAYOUT property and not "T must be int".
//
// It also pins the interface METHOD slot (the itmap was empty, so METHOD
// dispatch through an imported generic was equally dead), and reads back the
// neighbouring `string` field the setter's store would have clobbered had it
// landed at the wrong offset — the difference between "no longer crashes" and
// "writes to the right place". §13 / §23.3 / §4.6.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_iface_accessor_2894, iface_method_and_property_no_tparam_storage_spec_2_8_3) {
    const char* lib =
        "module GIfLib2894T2; "
        "public interface IBoth2894T2 "
        "  method Twice(): int; "
        "  property W: int get; set; end; "
        "end IBoth2894T2; "
        "public class GBox2894T2<T>: IBoth2894T2 "
        "  public _w: int := 0; "
        "  public note: string := \"keep\"; "
        "  public method Twice(): int return _w * 2; end Twice; "
        "  public property W: int "
        "    get => _w + 100; "
        "    set => do _w := value + 1; end; "
        "  end; "
        "end GBox2894T2;";
    int r = compileImportAndRun2894(
        lib,
        "module GIfApp2894T2; import GIfLib2894T2; "
        "public class M2894T2 "
        "  public static method Main(): int "
        "    using var g := new GBox2894T2<string>(); "
        "    var r: IBoth2894T2 := g; "
        "    if r.W <> 100 then return 91; end; "
        "    r.W := 7; "
        "    if g._w <> 8 then return 92; end; "
        "    if r.Twice() <> 16 then return 93; end; "
        "    if g.note.Length <> 4 then return 94; end; "
        "    return 0; "
        "  end Main; "
        "end M2894T2;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 0);
}

// ===========================================================================
// T3 — THE REFUSAL BOUNDARY, and the reason it is pinned as a RANGE rather than
// a value. `GMove2894T3<T>` declares TWO `T`-typed fields before `_w`, so at
// `T = string` the instantiation's layout MOVES relative to the erased template
// the exporter compiled `W`'s accessor against. The fix REFUSES to fill the
// itable there, because the only alternative — installing the template accessor
// anyway — turns a crash into a silent wrong answer.
//
// ⚠ MAINTENANCE NOTE. This case accepts EITHER outcome that is honest:
//   * -1, today's behaviour: the empty itmap is kept and the program faults;
//   * 108, the correct answer, if importer-side monomorphization of accessor
//     bodies (the #428 core) later lands and makes the fill valid here.
// It fails on ANY OTHER value, which is precisely the regression it exists to
// catch: a future change that installs the erased accessor for a layout-moving
// instantiation would return a plausible-looking WRONG number (the getter would
// read `_w` from the `_t2` slot), and no crash would announce it. Do not
// "simplify" this to EXPECT_EQ(-1): that would fail the day the residual is
// genuinely fixed, and the residual is filed as its own issue.
//
// The premise of the case is that the two layouts genuinely differ; TWO `T`
// fields are load-bearing, because with a single one the i32 placeholder and the
// `ptr` share an 8-byte-aligned slot and `_w` lands at the same offset either
// way — #2906/#2907's measured vacuity trap.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_iface_accessor_2894, layout_moving_instantiation_not_silently_wrong_spec_14_1) {
    const char* lib =
        "module GIfLib2894T3; "
        "public interface IHas2894T3 "
        "  property W: int get; set; end; "
        "end IHas2894T3; "
        "public class GMove2894T3<T>: IHas2894T3 "
        "  public _t: T; "
        "  public _t2: T; "
        "  public _w: int := 0; "
        "  public property W: int "
        "    get => _w + 100; "
        "    set => do _w := value + 1; end; "
        "  end; "
        "end GMove2894T3;";
    int r = compileImportAndRun2894(
        lib,
        "module GIfApp2894T3; import GIfLib2894T3; "
        "public class M2894T3 "
        "  public static method Main(): int "
        "    using var g := new GMove2894T3<string>(); "
        "    var r: IHas2894T3 := g; "
        "    if r.W <> 100 then return 91; end; "
        "    r.W := 7; "
        "    if g._w <> 8 then return 92; end; "
        "    return r.W; "
        "  end Main; "
        "end M2894T3;");
    if (r == -2) return;
    BEZEL_EXPECT_TRUE(r == -1 || r == 108);
}

// ===========================================================================
// T4 — CONTROL, for the EXPORTER half of the fix. A public NON-generic imported
// class with an interface property was already correct and must stay correct:
// dropping the exporter's `genericParams.empty()` gate must not disturb the
// shape that gate was never about. §2.5 / §13.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_iface_accessor_2894, nongeneric_imported_iface_property_unchanged_spec_2_5) {
    const char* lib =
        "module GIfLib2894T4; "
        "public interface IHas2894T4 "
        "  property W: int get; set; end; "
        "end IHas2894T4; "
        "public class Plain2894T4: IHas2894T4 "
        "  public _w: int := 0; "
        "  public property W: int "
        "    get => _w + 100; "
        "    set => do _w := value + 1; end; "
        "  end; "
        "end Plain2894T4;";
    int r = compileImportAndRun2894(
        lib,
        "module GIfApp2894T4; import GIfLib2894T4; "
        "public class M2894T4 "
        "  public static method Main(): int "
        "    using var g := new Plain2894T4(); "
        "    var r: IHas2894T4 := g; "
        "    if r.W <> 100 then return 91; end; "
        "    r.W := 7; "
        "    if g._w <> 8 then return 92; end; "
        "    return r.W; "
        "  end Main; "
        "end M2894T4;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 108);
}

// ===========================================================================
// T5 — TWO INSTANTIATIONS of one imported generic class, live at once and both
// reached through the interface type. §2.8.3 rule 4 gives each monomorphization
// its own runtime identity, so each must carry its OWN itmap: a fill cached
// across instantiations would hand one instantiation's table to the other.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_iface_accessor_2894, two_instantiations_each_get_a_table_spec_2_8_3) {
    const char* lib =
        "module GIfLib2894T5; "
        "public interface IHas2894T5 "
        "  property W: int get; set; end; "
        "end IHas2894T5; "
        "public class GBox2894T5<T>: IHas2894T5 "
        "  public _w: int := 0; "
        "  public property W: int "
        "    get => _w + 100; "
        "    set => do _w := value + 1; end; "
        "  end; "
        "end GBox2894T5;";
    int r = compileImportAndRun2894(
        lib,
        "module GIfApp2894T5; import GIfLib2894T5; "
        "public class M2894T5 "
        "  public static method Main(): int "
        "    using var gi := new GBox2894T5<int>(); "
        "    using var gs := new GBox2894T5<string>(); "
        "    var ri: IHas2894T5 := gi; "
        "    var rs: IHas2894T5 := gs; "
        "    ri.W := 1; "
        "    rs.W := 40; "
        "    if gi._w <> 2 then return 91; end; "
        "    if gs._w <> 41 then return 92; end; "
        "    return ri.W + rs.W - 100; "
        "  end Main; "
        "end M2894T5;");
    if (r == -2) return;
    // (2 + 100) + (41 + 100) - 100
    BEZEL_EXPECT_EQ(r, 143);
}
}  // namespace bezel::module
