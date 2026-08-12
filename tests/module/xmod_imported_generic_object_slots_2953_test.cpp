// Spec-derived regression test for issue #2953.
//
// §8.5 (`ToString`) / §8.3 (`Equals` and `GetHashCode`) / §12 (Inheritance and
// Dispatch) / §14.1 (Generic declarations) / §18.5 / §2.5 / §2.8.3 rule 4,
// v0.240.
//
// THE DEFECT. The FIXED vtable prefix — slot 3 `ToString`, slot 4 `Equals`,
// slot 5 `GetHashCode` — of an IMPORTED GENERIC instantiation was filled from
// the SHARED template thunks, each of which calls `methodFns_[user]`: for an
// imported method that is the ExternalLinkage declaration of the exporter's
// ERASED `T -> i32` template body. On a layout-moving instantiation that body
// GEPs offsets the object does not have.
//
// WORSE IN KIND THAN ITS SINGLE-MODULE TWIN #2947, which is why it is its own
// fix: #2947 SEGV'd, so it announced itself. Here the wrong offset lands inside
// the object's own allocation and nothing faults. ALL THREE ARMS MEASURED on
// the fix's parent — #2953 labelled `Equals`/`GetHashCode` "inferred, not
// probed", so they were probed rather than assumed — in ONE two-module run:
//
//     field=21    the storage IS 21 at the composed offset
//     s=w=0       slot 3, WRONG: "w=21" is correct
//     eq=0        slot 4, WRONG: the body is `_w = 21`, so 1 is correct
//     hash=23     slot 5, WRONG: the body is `_w + 23`, so 44 is correct
//
// `hash=23` is the sharpest of the three: not a null, not a crash, an
// arithmetically plausible number computed from a field read as 0.
//
// THE FIX asks #2947's question (#2019's predicate) of the class that DECLARES
// the override — `instanceLayoutDiffersFromErasedTemplate(user->parent)`, the
// class whose `classStruct` the forwarded-to body GEPs — and on true wraps
// `buildSpecializedMethodFn`'s re-lowered body in a fresh `...$specN` thunk,
// through the SAME body emitters #2947 factored out, so the Equals i1-coercion
// and the GetHashCode width coercion cannot diverge between template and
// specialization. It adds #2943's second gate, the one this cross-module twin
// needs and #2947 did not: `dynamic_cast<const ast::MethodDecl*>(user->decl)` —
// the loader must have reconstructed a body (§14.1 / #783). A slot with no
// reconstructed body, or whose specialization's SIGNATURE moves, KEEPS TODAY'S
// CONTENT rather than getting a guess.
//
// Built the same way as its siblings tests/module/xmod_imported_generic_layout_
// moving_2943_test.cpp and xmod_imported_generic_iface_accessor_2894_test.cpp:
// a real `bezelc -c` + `--import` + a native run. Values are kept < 256 because
// the observable is the exit status, and NEVER 0 — this defect's whole
// signature is a silent 0, so an expected value of 0 would prove nothing.

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

struct TempFile2953 {
    std::string path;
    explicit TempFile2953(std::string p) : path(std::move(p)) {}
    ~TempFile2953() { std::remove(path.c_str()); }
};

void writeSource2953(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj2953(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// -2 = the dependency toolchain is unavailable (test skips); -1 = the app did
// not build/link OR did not exit normally (a SIGSEGV lands here too).
int compileImportAndRun2953(const std::string& libSrc,
                            const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2953_" + std::to_string(std::rand());
    TempFile2953 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2953 aObj((dir / (stem + "_a.o")).string());
    writeSource2953(aSrc.path, libSrc);
    if (!compileToObj2953(aSrc.path, aObj.path)) return -2;
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile2953 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2953 bExe((dir / stem).string());
    writeSource2953(bSrc.path, appSrc);
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

// ⚠ MAINTENANCE NOTE — TWO PREMISES, BOTH SILENT IF LOST.
//
// (1) THE LAYOUT MOVES. Every generic class below declares TWO `T`-typed fields
// BEFORE its concrete storage. That is what makes the instantiation's layout
// move at `T = string` relative to the erased `T -> i32` template: with a
// SINGLE `T` field the i32 placeholder and the `ptr` share one 8-byte-aligned
// slot, `_w` lands at the same offset either way, and every case here silently
// becomes vacuous — it would pass with AND without the fix (#2906/#2907's
// measured trap; #2894, #2928, #2943 and #2947 all carry this same note). So:
// do NOT narrow these classes to one `T` field, and do not move `_t`/`_t2`
// after `_w`. T5 and T6 are the DECLARED layout-identical / no-override
// controls; every other case depends on the layouts genuinely differing.
//
// (2) THE RECEIVER DISPATCHES. T1-T4 use an `Object`-typed receiver (or an
// `Object`-typed interpolation hole) because that unambiguously reads the fixed
// vtable slot. Devirtualization is this family's recurring vacuity trap —
// #2906/#2947 both measured a shape that passed with AND without the fix — so do
// NOT "simplify" `var o: Object := g; o.ToString()` to `g.ToString()` in those
// cases even though T7 measures that the concrete spelling ALSO reads the slot
// on an imported generic. The two are pinned separately on purpose: T7's
// behaviour is a measurement, not a premise the other cases may lean on.

// ===========================================================================
// T0 — THE PREMISE CANARY. `_w` must be readable as 21 through an ordinary,
// NON-dispatched read on the concrete imported instantiation, i.e. at the
// COMPOSED offset. If this ever fails, cross-module field access itself has
// regressed and every case below is measuring something other than the vtable
// slot. §22.8 / §14.1 / §2.5.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_object_slots_2953, layout_premise_canary_spec_22_8) {
    const char* lib =
        "module GObjLib2953T0; "
        "public class GObj2953T0<T> "
        "  public _t: T; "
        "  public _t2: T; "
        "  public _w: int := 21; "
        "  public override method ToString(): string "
        "    return \"w=\" + _w.ToString(); "
        "  end ToString; "
        "end GObj2953T0;";
    int r = compileImportAndRun2953(
        lib,
        "module GObjApp2953T0; import GObjLib2953T0; "
        "public class M2953T0 "
        "  public static method Main(): int "
        "    using var g := new GObj2953T0<string>(); "
        "    if g._w <> 21 then return 91; end; "
        "    return 44; "
        "  end Main; "
        "end M2953T0;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 44);
}

// ===========================================================================
// T1 — THE ISSUE'S OWN REPRO: slot 3 `ToString` through an `Object`-typed
// receiver on a layout-moving IMPORTED generic. Pre-fix the program printed
// `s=w=0` beside `field=21` — the storage was right and the slot's body read it
// at the erased-template offset.
//
// The assertion is on the EXACT STRING, not on "it did not crash": nothing
// crashed pre-fix. §8.5 — "`ToString` is invoked implicitly inside string
// interpolation (§18.5) via virtual dispatch" — and §12.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_object_slots_2953, tostring_slot_object_receiver_spec_8_5) {
    const char* lib =
        "module GObjLib2953T1; "
        "public class GObj2953T1<T> "
        "  public _t: T; "
        "  public _t2: T; "
        "  public _w: int := 21; "
        "  public override method ToString(): string "
        "    return \"w=\" + _w.ToString(); "
        "  end ToString; "
        "end GObj2953T1;";
    int r = compileImportAndRun2953(
        lib,
        "module GObjApp2953T1; import GObjLib2953T1; "
        "public class M2953T1 "
        "  public static method Main(): int "
        "    using var g := new GObj2953T1<string>(); "
        "    var o: Object := g; "
        "    var s: string := o.ToString(); "
        "    if s <> \"w=21\" then return 91; end; "
        "    return 44; "
        "  end Main; "
        "end M2953T1;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 44);
}

// ===========================================================================
// T2 — THE ARM #2953 LABELLED "inferred, not probed": slot 4 `Equals` and slot
// 5 `GetHashCode` on the same layout-moving instantiation, both dispatched from
// an `Object`-typed receiver. Measured on the fix's parent, not assumed:
// `Equals` returned false where the body's `_w = 21` is true, and
// `GetHashCode` returned 23 where `_w + 23` is 44.
//
// §8.3: "A type that overrides `Equals` must also override `GetHashCode`. The
// compiler enforces this." — so one class necessarily carries both, and this
// case exercises both slots of one vtable at once. The hash is asserted against
// the EXACT 44: 23 is a perfectly plausible number, and a fix that merely made
// it non-zero would not be evidence it is right.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_object_slots_2953, equals_and_hash_slots_object_receiver_spec_8_3) {
    const char* lib =
        "module GObjLib2953T2; "
        "public class GObj2953T2<T> "
        "  public _t: T; "
        "  public _t2: T; "
        "  public _w: int := 21; "
        "  public override method Equals(other: Object?): bool "
        "    return _w = 21; "
        "  end Equals; "
        "  public override method GetHashCode(): int "
        "    return _w + 23; "
        "  end GetHashCode; "
        "end GObj2953T2;";
    int r = compileImportAndRun2953(
        lib,
        "module GObjApp2953T2; import GObjLib2953T2; "
        "public class M2953T2 "
        "  public static method Main(): int "
        "    using var g := new GObj2953T2<string>(); "
        "    var o: Object := g; "
        "    if not o.Equals(o) then return 91; end; "
        "    return o.GetHashCode(); "
        "  end Main; "
        "end M2953T2;");
    if (r == -2) return;
    // 21 + 23, through the composed layout.
    BEZEL_EXPECT_EQ(r, 44);
}

// ===========================================================================
// T3 — READING ACROSS THE MOVED REGION, not just past it. The overridden
// `ToString` reads an `int` AND a `string` field that both sit after the two
// `T`-typed ones, and the asserted value is composed from BOTH. That is the
// difference between "the slot no longer answers 0" and "it reached the right
// storage" — #2896 measured 44 where 149 was correct on exactly that
// distinction. §8.5 / §4.6 / §22.8.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_object_slots_2953, tostring_slot_reads_across_moved_storage_spec_22_8) {
    const char* lib =
        "module GObjLib2953T3; "
        "public class GObj2953T3<T> "
        "  public _t: T; "
        "  public _t2: T; "
        "  public _w: int := 21; "
        "  public note: string := \"keep\"; "
        "  public override method ToString(): string "
        "    return \"w=\" + _w.ToString() + \",n=\" + note; "
        "  end ToString; "
        "end GObj2953T3;";
    int r = compileImportAndRun2953(
        lib,
        "module GObjApp2953T3; import GObjLib2953T3; "
        "public class M2953T3 "
        "  public static method Main(): int "
        "    using var g := new GObj2953T3<string>(); "
        "    var o: Object := g; "
        "    var s: string := o.ToString(); "
        "    if s <> \"w=21,n=keep\" then return 91; end; "
        "    return 44; "
        "  end Main; "
        "end M2953T3;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 44);
}

// ===========================================================================
// T4 — AN INTERPOLATION HOLE, which is what §8.5 names as slot 3's implicit
// consumer: "`ToString` is invoked implicitly inside string interpolation
// (§18.5) via virtual dispatch". The hole is `Object`-typed, so it genuinely
// dispatches; the concrete-typed spelling is T7's declared control, and #2947
// measured that the two differ. §8.5 / §18.5.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_object_slots_2953, interpolation_hole_object_typed_spec_18_5) {
    const char* lib =
        "module GObjLib2953T4; "
        "public class GObj2953T4<T> "
        "  public _t: T; "
        "  public _t2: T; "
        "  public _w: int := 21; "
        "  public override method ToString(): string "
        "    return \"w=\" + _w.ToString(); "
        "  end ToString; "
        "end GObj2953T4;";
    int r = compileImportAndRun2953(
        lib,
        "module GObjApp2953T4; import GObjLib2953T4; "
        "public class M2953T4 "
        "  public static method Main(): int "
        "    using var g := new GObj2953T4<string>(); "
        "    var o: Object := g; "
        "    var s: string := $\"[{o}]\"; "
        "    if s <> \"[w=21]\" then return 91; end; "
        "    return 44; "
        "  end Main; "
        "end M2953T4;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 44);
}

// ===========================================================================
// T5 — DECLARED CONTROL: a layout-IDENTICAL instantiation. `T = int` makes the
// composed layout equal the erased template, so the shared thunk is still
// honest and the slot must stay exactly as #2894 left it. Passes with AND
// without the fix; it exists so a future edit that specializes unconditionally
// is caught. §2.5 / §14.1.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_object_slots_2953, layout_identical_instantiation_unchanged_spec_14_1) {
    const char* lib =
        "module GObjLib2953T5; "
        "public class GObj2953T5<T> "
        "  public _t: T; "
        "  public _t2: T; "
        "  public _w: int := 21; "
        "  public override method ToString(): string "
        "    return \"w=\" + _w.ToString(); "
        "  end ToString; "
        "end GObj2953T5;";
    int r = compileImportAndRun2953(
        lib,
        "module GObjApp2953T5; import GObjLib2953T5; "
        "public class M2953T5 "
        "  public static method Main(): int "
        "    using var g := new GObj2953T5<int>(); "
        "    var o: Object := g; "
        "    var s: string := o.ToString(); "
        "    if s <> \"w=21\" then return 91; end; "
        "    return 44; "
        "  end Main; "
        "end M2953T5;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 44);
}

// ===========================================================================
// T6 — DECLARED CONTROL: a layout-moving instantiation with NO override. Its
// slot 3 holds the §8.5 compiler-generated default — "For classes: the type's
// qualified name" — which reads no field and therefore has no layout to be
// wrong about. The specialization must not touch it. Passes with AND without
// the fix. §8.5.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_object_slots_2953, no_override_keeps_default_tostring_spec_8_5) {
    const char* lib =
        "module GObjLib2953T6; "
        "public class GObj2953T6<T> "
        "  public _t: T; "
        "  public _t2: T; "
        "  public _w: int := 21; "
        "end GObj2953T6;";
    int r = compileImportAndRun2953(
        lib,
        "module GObjApp2953T6; import GObjLib2953T6; "
        "public class M2953T6 "
        "  public static method Main(): int "
        "    using var g := new GObj2953T6<string>(); "
        "    var o: Object := g; "
        "    var s: string := o.ToString(); "
        "    if s.Length = 0 then return 91; end; "
        // #2934 (§8.5, v0.252): was "GObj2953T6" — an IMPORTED type's
        // §8.5 default carries its EXPORTER's module path (#1488's
        // TypeSymbol::moduleName, restored by ModuleLoader).
        //
        // ⚠ #3119 RESIDUAL, pinned deliberately. The receiver is
        // `GObj2953T6<string>`, and stdlib §2.13 rule 3 — which §8.5 pins this
        // default to — requires a generic instantiation to render its type
        // ARGUMENTS: the conforming answer is
        // "GObjLib2953T6.GObj2953T6<string>". #2934 shipped only the
        // module-path half of the pin (the emission sites hold a
        // `TypeSymbol*`, which carries generic PARAMETERS, not the
        // instantiation's arguments). This expectation therefore encodes
        // TODAY'S INCOMPLETE ANSWER on purpose, so that when #3119 lands this
        // test fails with a breadcrumb naming it rather than as a mystery —
        // the fix is to append "<string>" here.
        "    if s <> \"GObjLib2953T6.GObj2953T6\" then return 92; end; "
        "    return 44; "
        "  end Main; "
        "end M2953T6;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 44);
}

// ===========================================================================
// T7 — A CONCRETE-TYPED RECEIVER, AND IT IS *NOT* A CONTROL HERE. MEASURED, and
// the measurement corrects an assumption inherited from #2947.
//
// #2947 established, for a SAME-MODULE generic, that a concrete-typed
// interpolation hole resolves the override statically and never reads the fixed
// slot — the devirtualization vacuity that would have made its test pass with
// AND without the fix. That is a true statement about that shape. It is NOT true
// of `g.ToString()` on an IMPORTED generic instantiation: measured on this fix's
// parent, the concrete-receiver spelling returned `w=0` exactly as the
// `Object`-typed one did, so this call DOES reach vtable slot 3. (Contrast a
// user `virtual` method on the same class, which #2896 routes to a
// per-instantiation body by the receiver's descriptor and which was already
// correct — probed separately.)
//
// So this case is pinned as a real arm, not a declared control: it fails without
// the fix. It is kept distinct from T1 because "same defect, different call
// syntax" is precisely the thing an inherited claim gets wrong. §12 / §12.1.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_object_slots_2953, concrete_receiver_also_reads_slot_spec_12) {
    const char* lib =
        "module GObjLib2953T7; "
        "public class GObj2953T7<T> "
        "  public _t: T; "
        "  public _t2: T; "
        "  public _w: int := 21; "
        "  public override method ToString(): string "
        "    return \"w=\" + _w.ToString(); "
        "  end ToString; "
        "end GObj2953T7;";
    int r = compileImportAndRun2953(
        lib,
        "module GObjApp2953T7; import GObjLib2953T7; "
        "public class M2953T7 "
        "  public static method Main(): int "
        "    using var g := new GObj2953T7<string>(); "
        "    var s: string := g.ToString(); "
        "    if s <> \"w=21\" then return 91; end; "
        "    return 44; "
        "  end Main; "
        "end M2953T7;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 44);
}

// ===========================================================================
// T8 — TWO INSTANTIATIONS OF ONE IMPORTED GENERIC LIVE AT ONCE, one
// layout-moving and one layout-identical, each dispatched through its own
// `Object`-typed receiver. Each per-instantiation vtable must carry ITS OWN
// slot content: a specialization keyed on the class rather than on the
// (class, substitution) pair would give both the same body and one of the two
// answers would be wrong. §2.8.3 rule 4 / §14.1.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_object_slots_2953, two_instantiations_distinct_slots_spec_2_8_3) {
    const char* lib =
        "module GObjLib2953T8; "
        "public class GObj2953T8<T> "
        "  public _t: T; "
        "  public _t2: T; "
        "  public _w: int := 21; "
        "  public override method GetHashCode(): int "
        "    return _w + 23; "
        "  end GetHashCode; "
        "  public override method Equals(other: Object?): bool "
        "    return _w = 21; "
        "  end Equals; "
        "end GObj2953T8;";
    int r = compileImportAndRun2953(
        lib,
        "module GObjApp2953T8; import GObjLib2953T8; "
        "public class M2953T8 "
        "  public static method Main(): int "
        "    using var gs := new GObj2953T8<string>(); "
        "    using var gi := new GObj2953T8<int>(); "
        "    var os: Object := gs; "
        "    var oi: Object := gi; "
        "    if os.GetHashCode() <> 44 then return 91; end; "
        "    if oi.GetHashCode() <> 44 then return 92; end; "
        "    return 44; "
        "  end Main; "
        "end M2953T8;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 44);
}

// ===========================================================================
// T9 — GENERIC ARITY 2. The gate is a LAYOUT property, not "one type
// parameter": `<A, B>` at `<string, string>` moves the storage the same way,
// and the specialized thunk must be selected per (class, substitution).
// §14.1 / §2.8.3 rule 4.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_object_slots_2953, arity_two_layout_moving_spec_14_1) {
    const char* lib =
        "module GObjLib2953T9; "
        "public class GObj2953T9<A, B> "
        "  public _a: A; "
        "  public _b: B; "
        "  public _w: int := 21; "
        "  public override method ToString(): string "
        "    return \"w=\" + _w.ToString(); "
        "  end ToString; "
        "end GObj2953T9;";
    int r = compileImportAndRun2953(
        lib,
        "module GObjApp2953T9; import GObjLib2953T9; "
        "public class M2953T9 "
        "  public static method Main(): int "
        "    using var g := new GObj2953T9<string, string>(); "
        "    var o: Object := g; "
        "    var s: string := o.ToString(); "
        "    if s <> \"w=21\" then return 91; end; "
        "    return 44; "
        "  end Main; "
        "end M2953T9;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 44);
}

}  // namespace bezel::module
