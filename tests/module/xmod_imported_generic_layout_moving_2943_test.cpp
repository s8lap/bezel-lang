// Spec-derived regression test for issue #2943.
//
// §2.8.3 rule 4 (one runtime type identity per monomorphization) / §12.1
// (Inheritance and dispatch) / §13 (Interfaces) / §11.2 / §23 (Properties and
// Indexers) / §14.1 (Generic declarations) / §2.5, v0.240.
//
// THE DEFECT. #2894 populated the per-instantiation itmap of an IMPORTED
// GENERIC class, but GATED THE WHOLE TABLE on the instantiation's storage
// layout being identical to the erased `T -> i32` template the exporter
// compiled against. An instantiation whose layout MOVES kept the pre-#2894
// behaviour — an empty itmap — so ANY interface dispatch on it faulted.
// Measured on this fix's parent:
//
//     interface METHOD   on a layout-moving imported generic -> exit 139
//     interface PROPERTY on the same shape                   -> exit 139
//
// THE ISSUE'S PREMISE IS HALF RIGHT, AND THE HALF THAT IS WRONG IS WHERE THE
// FIX LIVES. #2943 states the arm needs "importer-side monomorphization of
// accessor bodies — the #428 core". That is true of the ACCESSOR slots and only
// of them: ModuleLoader attaches no `ast::PropertyDecl` to an imported property
// (#2387), so there is no body to re-lower and the only installable function
// would be the exporter's template accessor — which would read `_w` out of the
// `_t2` slot and return a plausible WRONG number with no crash to announce it
// (#2896 measured 44 where 149 was correct). It is NOT true of the METHOD
// slots: ModuleLoader DOES attach a re-parsed `ast::MethodDecl` from the
// exported `methodBodyText` (§14.1 / #783), so `buildSpecializedMethodFn`
// compiles the body afresh under THIS substitution, against the layout the
// objects actually have.
//
// So the table is now built for EVERY instantiation and the layout question is
// asked PER SLOT KIND — of the slot that can answer it — rather than once for
// the whole table:
//   * METHOD slot, layout moves        -> re-lowered specialization (T1/T2/T4/T5)
//   * METHOD slot, no reconstructed body -> stays null (nothing to guess with)
//   * ACCESSOR slot, layout moves      -> stays null; #2894's refusal, kept (T3)
//   * either slot, layout identical    -> unchanged from #2894 (T6)
//
// §2.8.3 rule 4 is the authority for building the table at all: an instantiation
// minted importer-side must dispatch exactly as a same-module one does, "one
// runtime type identity ... regardless of which artifact produced the value".
//
// Built the same way as its siblings tests/module/xmod_imported_generic_iface_
// accessor_2894_test.cpp and xmod_imported_generic_indexer_2946_test.cpp: a real
// `bezelc -c` + `--import` + a native run. Values are kept < 256 where the
// observable is the exit status.

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

struct TempFile2943 {
    std::string path;
    explicit TempFile2943(std::string p) : path(std::move(p)) {}
    ~TempFile2943() { std::remove(path.c_str()); }
};

void writeSource2943(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj2943(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// -2 = the dependency toolchain is unavailable (test skips); -1 = the app did
// not build/link OR did not exit normally (a SIGSEGV lands here too — which is
// exactly what T3 is allowed to observe and no other case is).
int compileImportAndRun2943(const std::string& libSrc,
                            const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2943_" + std::to_string(std::rand());
    TempFile2943 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2943 aObj((dir / (stem + "_a.o")).string());
    writeSource2943(aSrc.path, libSrc);
    if (!compileToObj2943(aSrc.path, aObj.path)) return -2;
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile2943 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2943 bExe((dir / stem).string());
    writeSource2943(bSrc.path, appSrc);
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

// ⚠ MAINTENANCE NOTE — THE PREMISE OF EVERY CASE BELOW, AND HOW IT EXPIRES.
//
// Each generic class here declares TWO `T`-typed fields before its concrete
// storage. That is what makes the instantiation's layout MOVE at `T = string`
// relative to the erased `T -> i32` template: with a SINGLE `T` field the i32
// placeholder and the `ptr` share one 8-byte-aligned slot, `_w` lands at the
// same offset either way, and every case here silently becomes vacuous — it
// would pass with AND without the fix (#2906/#2907's measured trap; #2894 and
// #2928 carry the same note).
//
// So: do NOT narrow these classes to one `T` field, and do not move `_t`/`_t2`
// after `_w`. T6 is the declared layout-IDENTICAL control and is the one case
// that is SUPPOSED to be on the unchanged path; every other case here depends
// on the layouts genuinely differing.

// ===========================================================================
// T1 — AN INTERFACE METHOD on a layout-moving imported generic instantiation.
// Pre-fix: exit 139, a dispatch through the empty itmap. The method body reads
// `_w` at the COMPOSED offset, so 42 is only reachable if the slot holds a body
// compiled for the layout the object actually has. §13 / §11.2 / §2.8.3 rule 4.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_layout_moving_2943, iface_method_layout_moving_spec_13) {
    const char* lib =
        "module GMvLib2943T1; "
        "public interface IDo2943T1 "
        "  method Twice(): int; "
        "end IDo2943T1; "
        "public class GMove2943T1<T>: IDo2943T1 "
        "  public _t: T; "
        "  public _t2: T; "
        "  public _w: int := 21; "
        "  public method Twice(): int return _w * 2; end Twice; "
        "end GMove2943T1;";
    int r = compileImportAndRun2943(
        lib,
        "module GMvApp2943T1; import GMvLib2943T1; "
        "public class M2943T1 "
        "  public static method Main(): int "
        "    using var g := new GMove2943T1<string>(); "
        "    var d: IDo2943T1 := g; "
        "    return d.Twice(); "
        "  end Main; "
        "end M2943T1;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 42);
}

// ===========================================================================
// T2 — THE SAME SLOT, READING ACROSS THE MOVED REGION. The body reads an `int`
// AND a `string` field that both sit after the two `T`-typed ones, so a body
// compiled against the placeholder layout would read them at offsets 8 bytes
// apart from the truth. Asserting a value composed from BOTH is the difference
// between "the dispatch no longer crashes" and "it reached the right storage"
// (#2896: a correct gate alone would have turned a crash into 44 where 149 was
// right). §13 / §4.6 / §22.8.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_layout_moving_2943, iface_method_reads_across_moved_storage_spec_22_8) {
    const char* lib =
        "module GMvLib2943T2; "
        "public interface IDo2943T2 "
        "  method Score(): int; "
        "end IDo2943T2; "
        "public class GMove2943T2<T>: IDo2943T2 "
        "  public _t: T; "
        "  public _t2: T; "
        "  public _w: int := 21; "
        "  public note: string := \"keep\"; "
        "  public method Score(): int return _w * 2 + note.Length; end Score; "
        "end GMove2943T2;";
    int r = compileImportAndRun2943(
        lib,
        "module GMvApp2943T2; import GMvLib2943T2; "
        "public class M2943T2 "
        "  public static method Main(): int "
        "    using var g := new GMove2943T2<string>(); "
        "    var d: IDo2943T2 := g; "
        "    return d.Score(); "
        "  end Main; "
        "end M2943T2;");
    if (r == -2) return;
    // 21 * 2 + 4
    BEZEL_EXPECT_EQ(r, 46);
}

// ===========================================================================
// T3 — THE REFUSAL THAT REMAINS, and the issue's own repro. An interface
// PROPERTY on a layout-moving imported generic is still not dispatchable: the
// loader reconstructs no `ast::PropertyDecl` for an imported property (#2387),
// so there is no body to re-lower and the only installable function would be
// the exporter's template accessor — reading `_w` out of the `_t2` slot.
//
// ⚠ MAINTENANCE NOTE. Pinned as a RANGE, for #2894 T3's reason. It accepts
// EITHER honest outcome:
//   * -1, today's behaviour: the accessor slot stays null and the program
//     faults (note the fault MOVED with this fix — the itmap is now built and
//     the lookup succeeds, so the call is through a null SLOT rather than a
//     null table — same severity, same class);
//   * 108, the correct answer, if importer-side monomorphization of ACCESSOR
//     bodies (the #428 core) later lands.
// It fails on ANY OTHER value, which is the regression it exists to catch: a
// change that installs the erased accessor here returns a plausible-looking
// WRONG number with no crash to announce it. Do NOT "simplify" this to
// EXPECT_EQ(-1) — that would fail the day the residual is genuinely fixed.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_layout_moving_2943, iface_property_still_refused_spec_14_1) {
    const char* lib =
        "module GMvLib2943T3; "
        "public interface IHas2943T3 "
        "  property W: int get; set; end; "
        "end IHas2943T3; "
        "public class GMove2943T3<T>: IHas2943T3 "
        "  public _t: T; "
        "  public _t2: T; "
        "  public _w: int := 0; "
        "  public property W: int "
        "    get => _w + 100; "
        "    set => do _w := value + 1; end; "
        "  end; "
        "end GMove2943T3;";
    int r = compileImportAndRun2943(
        lib,
        "module GMvApp2943T3; import GMvLib2943T3; "
        "public class M2943T3 "
        "  public static method Main(): int "
        "    using var g := new GMove2943T3<string>(); "
        "    var h: IHas2943T3 := g; "
        "    if h.W <> 100 then return 91; end; "
        "    h.W := 7; "
        "    if g._w <> 8 then return 92; end; "
        "    return h.W; "
        "  end Main; "
        "end M2943T3;");
    if (r == -2) return;
    BEZEL_EXPECT_TRUE(r == -1 || r == 108);
}

// ===========================================================================
// T4 — A MIXED INTERFACE: one method (fillable) and one property (refused) in
// the SAME itable, with only the method dispatched. This is the case a
// whole-table gate cannot express, and the reason the gate had to move from
// per-TABLE to per-SLOT-KIND. A null accessor slot sitting beside it must not
// stop the method slot from being correct.
//
// The property is deliberately never read here. Through the INTERFACE it is
// T3's refused arm (it faults); on the CONCRETE receiver it is #2884's
// compile-time decline ("property `W` on the imported generic type … is not yet
// monomorphized"), verified by probe — so a `g.W` in this program would fail
// the BUILD and this case would be measuring #2884's diagnostic rather than the
// method slot. Reading the backing FIELD instead is the access #2884's own
// diagnostic recommends. §13 / §23 / §2.8.3 rule 4.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_layout_moving_2943, mixed_iface_method_works_beside_refused_accessor_spec_13) {
    const char* lib =
        "module GMvLib2943T4; "
        "public interface IMix2943T4 "
        "  method Twice(): int; "
        "  property W: int get; end; "
        "end IMix2943T4; "
        "public class GMove2943T4<T>: IMix2943T4 "
        "  public _t: T; "
        "  public _t2: T; "
        "  public _w: int := 21; "
        "  public method Twice(): int return _w * 2; end Twice; "
        "  public property W: int get => _w + 100; end; "
        "end GMove2943T4;";
    int r = compileImportAndRun2943(
        lib,
        "module GMvApp2943T4; import GMvLib2943T4; "
        "public class M2943T4 "
        "  public static method Main(): int "
        "    using var g := new GMove2943T4<string>(); "
        "    var m: IMix2943T4 := g; "
        "    if g._w <> 21 then return 91; end; "
        "    return m.Twice(); "
        "  end Main; "
        "end M2943T4;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 42);
}

// ===========================================================================
// T5 — RE-ENTRANCY. The dispatched method CONSTRUCTS ITS OWN INSTANTIATION, so
// emitting its specialized body re-enters `buildImportedGenericVtable` for the
// same instantiation. That function wrote its cache only on the way OUT, so the
// re-entry used to miss and build a SECOND vtable for one monomorphization —
// §2.8.3 rule 4's "one runtime type identity" broken by construction. The fix
// registers the global before any body emission (#2906's change, applied to
// this function's identical hazard).
//
// ⚠ THE ASSERTED VALUE COMES FROM THE OUTER `this`, DELIBERATELY. A separate,
// PRE-EXISTING defect — measured on this commit's parent and filed with its own
// issue — is that `new G<T>()` inside an IMPORTED generic class's re-lowered
// body does not run the class's field initializers, so `inner._w` reads 0 where
// 21 is correct. Asserting on the inner object would therefore pin somebody
// else's bug and would break when it is fixed. `inner` is still constructed
// (that is what forces the re-entry); only the outer read is asserted.
// §2.8.3 rule 4 / §14.1.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_layout_moving_2943, reentrant_construction_one_identity_spec_2_8_3) {
    const char* lib =
        "module GMvLib2943T5; "
        "public interface IRe2943T5 "
        "  method Nested(): int; "
        "end IRe2943T5; "
        "public class GRe2943T5<T>: IRe2943T5 "
        "  public _t: T; "
        "  public _t2: T; "
        "  public _w: int := 21; "
        "  public method Nested(): int "
        "    using var inner := new GRe2943T5<T>(); "
        "    return _w * 2; "
        "  end Nested; "
        "end GRe2943T5;";
    int r = compileImportAndRun2943(
        lib,
        "module GMvApp2943T5; import GMvLib2943T5; "
        "public class M2943T5 "
        "  public static method Main(): int "
        "    using var g := new GRe2943T5<string>(); "
        "    var d: IRe2943T5 := g; "
        "    return d.Nested(); "
        "  end Main; "
        "end M2943T5;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 42);
}

// ===========================================================================
// T6 — CONTROL. A layout-IDENTICAL instantiation must stay on exactly the path
// #2894 gave it: the exporter's own compiled body in the slot, no
// specialization. `GSame2943T6<T>` has no `T`-typed storage at all, so its
// layout matches the erased template at EVERY instantiation — instantiated here
// at `string`, which shows once more that the gate is a LAYOUT property and not
// "T must be int". Passes with and without the fix. §2.5 / §13.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_layout_moving_2943, layout_identical_instantiation_unchanged_spec_2_5) {
    const char* lib =
        "module GMvLib2943T6; "
        "public interface IDo2943T6 "
        "  method Twice(): int; "
        "  property W: int get; end; "
        "end IDo2943T6; "
        "public class GSame2943T6<T>: IDo2943T6 "
        "  public _w: int := 21; "
        "  public method Twice(): int return _w * 2; end Twice; "
        "  public property W: int get => _w + 100; end; "
        "end GSame2943T6;";
    int r = compileImportAndRun2943(
        lib,
        "module GMvApp2943T6; import GMvLib2943T6; "
        "public class M2943T6 "
        "  public static method Main(): int "
        "    using var g := new GSame2943T6<string>(); "
        "    var d: IDo2943T6 := g; "
        "    if d.W <> 121 then return 91; end; "
        "    return d.Twice(); "
        "  end Main; "
        "end M2943T6;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 42);
}

// ===========================================================================
// T7 — TWO INSTANTIATIONS live at once, one layout-moving and one not, both
// reached through the same interface type. §2.8.3 rule 4 gives each
// monomorphization its own runtime identity, so each must carry its OWN itmap:
// the layout-moving one holds a re-lowered specialization and the other holds
// the exporter's body, and a fill cached across them would hand one
// instantiation's table to the other.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_layout_moving_2943, two_instantiations_each_get_a_table_spec_2_8_3) {
    const char* lib =
        "module GMvLib2943T7; "
        "public interface IDo2943T7 "
        "  method Twice(): int; "
        "end IDo2943T7; "
        "public class GMove2943T7<T>: IDo2943T7 "
        "  public _t: T; "
        "  public _t2: T; "
        "  public _w: int := 21; "
        "  public method Twice(): int return _w * 2; end Twice; "
        "end GMove2943T7;";
    int r = compileImportAndRun2943(
        lib,
        "module GMvApp2943T7; import GMvLib2943T7; "
        "public class M2943T7 "
        "  public static method Main(): int "
        "    using var gs := new GMove2943T7<string>(); "
        "    using var gi := new GMove2943T7<int>(); "
        "    var ds: IDo2943T7 := gs; "
        "    var di: IDo2943T7 := gi; "
        "    if ds.Twice() <> 42 then return 91; end; "
        "    if di.Twice() <> 42 then return 92; end; "
        "    return 42; "
        "  end Main; "
        "end M2943T7;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 42);
}

// ===========================================================================
// T8 — issue #2978: an ordinary VIRTUAL METHOD slot in an imported generic's
// per-instantiation vtable must hold the body re-lowered for that instance.
//
// The otherwise-unused local `D2978<U> : G2978<string>` is load-bearing. Its
// own `U` is not derivable from the static `G2978<string>` receiver type, so
// `emitVirtualGenericDispatch` must abandon its descriptor-keyed call-site
// dispatch. The actual receiver remains the imported `G2978<string>` object;
// the call therefore reads that object's vtable slot directly. Before the fix
// the slot held the exporter's erased `T -> i32` body, which read `_w` from the
// old offset and returned 100. The re-lowered body reads the composed offset and
// returns 121. §12 / §14.1 / §2.8.3 rule 4, v0.240.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_virtual_vtable_slot_2978,
           undetermined_local_subclass_forces_imported_slot_spec_12) {
    const char* lib =
        "module GVirtualLib2978; "
        "public class G2978<T> "
        "  public _t: T; "
        "  public _t2: T; "
        "  public _w: int := 21; "
        "  public virtual method V(): int return _w + 100; end V; "
        "end G2978;";
    int r = compileImportAndRun2943(
        lib,
        "module GVirtualApp2978; import GVirtualLib2978; "
        "class D2978<U>: G2978<string> "
        "  public override method V(): int return 7; end V; "
        "end D2978; "
        "public class M2978 "
        "  public static method Main(): int "
        "    using var g := new G2978<string>(); "
        "    using var d := new D2978<int>(); "
        "    var b: G2978<string> := g; "
        "    return b.V(); "
        "  end Main; "
        "end M2978;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 121);
}
}  // namespace bezel::module
