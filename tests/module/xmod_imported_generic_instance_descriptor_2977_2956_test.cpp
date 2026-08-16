// Spec-derived regression test for issues #2977 and #2956 — ONE defect, filed
// twice, and the identity of the two is established here by measurement rather
// than by resemblance (T1 and T2 are each issue's own repro, and one change to
// one call fixed both).
//
// §2.8.3 rule 4 (Transitive dependencies and cross-artifact type identity) /
// §2.8.2 clause 2 / §12.7 (Type tests: `is`, `is not`, `as`) / §17.4 / §19.3 /
// §31.2.1 (Arm patterns) / §31.4 / §12 / §14.1 / §2.5, v0.240.
//
// THE DEFECT. `Emitter::buildImportedGenericVtable` minted vtable slot 0 — the
// object's runtime type identity — as its OWN private, hardcoded two-element
// array `td$<Name>$specN = [self, null]`, and the comment it carried ("a minimal
// descriptor that lets emitIsA / type tests recognize the object") was false in
// both halves:
//
//   * WRONG SELF IDENTITY (#2977). Every type-test SITE resolves its target
//     through `perInstDescriptorForTarget` → `instanceDescriptorFor`, i.e. the
//     #1843 per-monomorphization global `td$<readable-mangle>`. That is a
//     DIFFERENT global, minted by a different function under a different name
//     with a different cache key, and nothing reconciled the two — so the
//     pointer comparison could never succeed. Measured on the fix's parent, on
//     a real two-module build:
//
//         outer_is=0    WRONG: 44 is correct   (`g is GRe14<string>`)
//         obj_is=0      WRONG: 44 is correct   (the same test through `Object`)
//
//     Both spellings, so it is not a devirtualization artifact. The emitted IR
//     names both halves: the object stores `td$GRe14$spec0` and the site compares
//     `@"td$GRe14_string"`.
//
//   * NO ANCESTORS AT ALL (#2956). `[self, null]` lists no bases, so a test
//     against ANY ancestor answered false — including a `catch` arm, dispatched
//     by exactly this descriptor walk. An imported generic exception subclass was
//     therefore UNCATCHABLE in the importing module:
//
//         throw new GEx8<int>(7)   ->   Unhandled exception: GEx8, exit=70
//
//     where the identical declarations compiled as ONE module print `caught` and
//     exit 0.
//
// §2.8.3 rule 4 names the broken operations verbatim: a type "is one type in the
// running process: ... and one runtime type identity — `is` tests, `catch`
// clauses, and §19 patterns over values of the instantiation match identically
// regardless of which artifact produced the value or performs the test." §2.8.2
// clause 2 states the same requirement from the artifact-interface side: "An
// external instantiation behaves identically to a same-module instantiation of
// the same generic at the same arguments."
//
// THE FIX is one call: install `instanceDescriptorFor(ts, args)` — the SAME
// global the test sites resolve — deriving `args` from the active
// `genericSubstSemaTy_`, exactly as the local twin `specializedVtableFor` has
// always done. That supplies BOTH halves at once, which is why the two issues are
// one defect.
//
// #2956's own diagnosis said its USER-ancestor half "needs the same treatment
// #2894 gave the ITMAP, including its layout-moves gate — that half is larger and
// should be sized separately". That is REFUTED, and T4 is the measurement: a
// descriptor is an IDENTITY CHAIN, not code — no ABI, no GEP — so the layout
// question that rightly governs every FUNCTION slot in this vtable (#2894's
// itable, #2943's method slots, #2953's Object slots) has nothing to say about
// it, and the BFS over `ts->baseTypes` plus the `directBuiltinExceptionBase`
// walk both already run importer-side.
//
// Built like its sibling tests/module/xmod_imported_generic_iface_accessor_2894_
// test.cpp: a real `bezelc -c lib.bzl` + `bezelc --import lib.o app.bzl` + a
// native run. Every expected value is non-zero and every observable is a VALUE,
// because this defect's whole signature is a silently `false` type test.

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

struct TempFile2977 {
    std::string path;
    explicit TempFile2977(std::string p) : path(std::move(p)) {}
    ~TempFile2977() { std::remove(path.c_str()); }
};

void writeSource2977(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj2977(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// -2 = the dependency toolchain is unavailable (test skips); -1 = the app did
// not build/link OR did not exit normally. An UNHANDLED exception exits 70
// through the normal path, so #2956's pre-fix symptom is a distinguishable
// VALUE (70) and not the -1 bucket.
int compileImportAndRun2977(const std::string& libSrc,
                            const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2977_");
    TempFile2977 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2977 aObj((dir / (stem + "_a.o")).string());
    writeSource2977(aSrc.path, libSrc);
    if (!compileToObj2977(aSrc.path, aObj.path)) return -2;
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile2977 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2977 bExe((dir / stem).string());
    writeSource2977(bSrc.path, appSrc);
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

// The exported generic used by T1/T3/T5. TWO `T`-typed fields before `_w` are
// load-bearing across this whole file: with a single one the i32 placeholder and
// the `ptr` share an 8-byte-aligned slot and the erased and composed layouts
// COINCIDE (#2906/#2907's measured vacuity trap), so at `T = string` this
// instantiation genuinely moves. It is here precisely to show that the fix is
// NOT layout-gated — a descriptor carries no code — and T5 reads `_w` back
// through an `as` to prove the object really is the moved layout.
const char* kLibGRe =
    "module DescLib2977; "
    "public class GRe2977<T> "
    "  public _t: T; "
    "  public _t2: T; "
    "  public _w: int := 21; "
    "end GRe2977;";

}  // namespace

// ===========================================================================
// T1 — #2977's OWN REPRO. `g is GRe2977<string>` through a concrete-typed
// receiver AND through an `Object`-typed one. Pre-fix BOTH answered false and
// the program returned 0; §2.8.3 rule 4 names `is` tests first.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_instance_descriptor_2977, is_test_matches_own_instantiation_spec_2_8_3_rule_4) {
    int r = compileImportAndRun2977(
        kLibGRe,
        "module DescApp2977T1; import DescLib2977; "
        "public class M2977T1 "
        "  public static method Main(): int "
        "    using var g := new GRe2977<string>(); "
        "    var n: int := 0; "
        "    if g is GRe2977<string> then n := 40; end if; "
        "    var o: Object := g; "
        "    if o is GRe2977<string> then n := n + 4; end if; "
        "    return n; "
        "  end Main; "
        "end M2977T1;");
    if (r == -2) return;
    // 40 (concrete receiver) + 4 (Object receiver). Pre-fix: 0.
    BEZEL_EXPECT_EQ(r, 44);
}

// ===========================================================================
// T2 — #2956's OWN REPRO: an imported generic EXCEPTION subclass, thrown and
// caught by `catch var e: Exception`. §31.2.1: "A bare type: `IOException`.
// Matches any thrown exception that is an instance of the type." Pre-fix the
// program died `Unhandled exception: GEx8` and exited 70.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_instance_descriptor_2977, imported_generic_exception_is_catchable_spec_31_2_1) {
    int r = compileImportAndRun2977(
        "module DescLib2977T2; "
        "public class GEx2977T2<T>: Exception "
        "  public _t: T; "
        "  public _t2: T; "
        "  public V: int; "
        "  public constructor(n: int) base(\"g\"); "
        "    V := n; "
        "  end constructor; "
        "end GEx2977T2;",
        "module DescApp2977T2; import DescLib2977T2; "
        "public class M2977T2 "
        "  public static method Main(): int "
        "    var n: int := 0; "
        "    try "
        "      throw new GEx2977T2<int>(7); "
        "    catch var e: Exception => n := 41; "
        "    end; "
        "    return n + 1; "
        "  end Main; "
        "end M2977T2;");
    if (r == -2) return;
    // Pre-fix: 70 (the process died unhandled before reaching the return).
    BEZEL_EXPECT_EQ(r, 42);
}

// ===========================================================================
// T3 — THE INVARIANCE THE FIX MUST NOT BUY THE MATCHES WITH, in both
// directions. §12.9's invariance and the #1843/#2001 type-confusion class: a
// `GRe2977<string>` must match `GRe2977<string>` and must NOT match
// `GRe2977<int>`, and vice versa, with both instantiations live at once.
//
// This is the case that fails if a future change "fixes" the match by handing
// the object a shared, declaration-level descriptor: that would make the
// positive tests pass and the two negative ones pass a WRONG-layout `as` right
// behind them. Each wrong answer returns its own code so the failure names
// which half broke.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_instance_descriptor_2977, sibling_instantiations_stay_distinct_spec_12_9) {
    int r = compileImportAndRun2977(
        kLibGRe,
        "module DescApp2977T3; import DescLib2977; "
        "public class M2977T3 "
        "  public static method Main(): int "
        "    using var gs := new GRe2977<string>(); "
        "    using var gi := new GRe2977<int>(); "
        "    var os: Object := gs; "
        "    var oi: Object := gi; "
        "    if os is not GRe2977<string> then return 91; end if; "
        "    if os is GRe2977<int> then return 92; end if; "
        "    if oi is not GRe2977<int> then return 93; end if; "
        "    if oi is GRe2977<string> then return 94; end if; "
        "    return 44; "
        "  end Main; "
        "end M2977T3;");
    if (r == -2) return;
    // Pre-fix: 91 — the object's own instantiation did not even match itself.
    BEZEL_EXPECT_EQ(r, 44);
}

// ===========================================================================
// T4 — THE ANCESTOR CHAIN, which #2956 called a separately-sizeable, larger
// half needing #2894's layout-moves gate. It is neither: an imported generic
// class's imported base CLASS and imported INTERFACE both appear in the chain
// that `instanceDescriptorFor`'s BFS over `ts->baseTypes` builds, and a
// descriptor has no layout to be wrong about. All three tests are pinned in one
// program so a partial regression cannot hide behind a passing sibling.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_instance_descriptor_2977, imported_user_base_and_interface_ancestors_match_spec_12) {
    int r = compileImportAndRun2977(
        "module DescLib2977T4; "
        "public interface IHas2977T4 "
        "  method Twice(): int; "
        "end IHas2977T4; "
        "public class Base2977T4 "
        "  public _b: int := 5; "
        "end Base2977T4; "
        "public class GD2977T4<T>: Base2977T4, IHas2977T4 "
        "  public _t: T; "
        "  public _t2: T; "
        "  public _w: int := 21; "
        "  public method Twice(): int return 2; end Twice; "
        "end GD2977T4;",
        "module DescApp2977T4; import DescLib2977T4; "
        "public class M2977T4 "
        "  public static method Main(): int "
        "    using var g := new GD2977T4<string>(); "
        "    var o: Object := g; "
        "    if o is not Base2977T4 then return 91; end if; "
        "    if o is not IHas2977T4 then return 92; end if; "
        "    if o is not GD2977T4<string> then return 93; end if; "
        "    if o is GD2977T4<int> then return 94; end if; "
        "    return 44; "
        "  end Main; "
        "end M2977T4;");
    if (r == -2) return;
    // Pre-fix: 91 — `[self, null]` listed no ancestors at all.
    BEZEL_EXPECT_EQ(r, 44);
}

// ===========================================================================
// T5 — THE THIRD OPERATION §2.8.3 rule 4 NAMES: a §19 `case` pattern, plus the
// `as` that follows a successful test. The `as` reads `_w` back at 21, so this
// case distinguishes "the test now answers true" from "the test answers true
// and the object is still the layout it was minted with" — the difference that
// matters on a layout-MOVING instantiation (two `T` fields at `T = string`).
// §19.3 type patterns with binding / §17.4.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_instance_descriptor_2977, case_pattern_and_as_over_imported_instantiation_spec_19_3) {
    int r = compileImportAndRun2977(
        kLibGRe,
        "module DescApp2977T5; import DescLib2977; "
        "public class M2977T5 "
        "  public static method Main(): int "
        "    using var g := new GRe2977<string>(); "
        "    var o: Object := g; "
        "    var n := (case o of var a: GRe2977<int> => 5, "
        "                        var b: GRe2977<string> => 23, "
        "                        _ => 9); "
        "    if n <> 23 then return 91; end if; "
        "    var c := o as GRe2977<string>; "
        "    var w: int := 0; "
        "    if c <> null then w := c._w; end if; "
        "    if w <> 21 then return 92; end if; "
        "    return n + w; "
        "  end Main; "
        "end M2977T5;");
    if (r == -2) return;
    // 23 (the correct arm) + 21 (`_w` read back through the `as`). Pre-fix: 91
    // — the `case` fell through to `_` and answered 9.
    BEZEL_EXPECT_EQ(r, 44);
}

// ===========================================================================
// T6 — THE PRECISE catch arm over an imported generic exception, its bound
// field read, and the SIBLING instantiation that must NOT catch. §31.2.1's arm
// ordering makes this observable: a `GEx<string>` is offered to a
// `GEx<int>` arm first, and must fall through it to the `Exception` arm.
// The `e.V` read is what separates "the arm matched" from "the arm matched and
// bound an object whose storage is where the importer thinks it is".
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_instance_descriptor_2977, precise_catch_matches_only_its_own_instantiation_spec_31_2_1) {
    const char* lib =
        "module DescLib2977T6; "
        "public class GEx2977T6<T>: Exception "
        "  public _t: T; "
        "  public _t2: T; "
        "  public V: int; "
        "  public constructor(n: int) base(\"g\"); "
        "    V := n; "
        "  end constructor; "
        "end GEx2977T6;";
    int r = compileImportAndRun2977(
        lib,
        "module DescApp2977T6; import DescLib2977T6; "
        "public class M2977T6 "
        "  public static method Main(): int "
        "    var n: int := 0; "
        "    try "
        "      throw new GEx2977T6<string>(7); "
        "    catch var e: GEx2977T6<string> => n := e.V; "
        "    catch var e2: Exception => n := 90; "
        "    end; "
        "    if n <> 7 then return 91; end if; "
        "    var sib: int := 0; "
        "    try "
        "      throw new GEx2977T6<string>(3); "
        "    catch var e3: GEx2977T6<int> => sib := 91; "
        "    catch var e4: Exception => sib := 37; "
        "    end; "
        "    return n + sib; "
        "  end Main; "
        "end M2977T6;");
    if (r == -2) return;
    // 7 (the precise arm's bound `V`) + 37 (the sibling arm correctly declined).
    // Pre-fix: 70 — neither arm matched and the process died unhandled.
    BEZEL_EXPECT_EQ(r, 44);
}

// ===========================================================================
// T7 — THE SAME-MODULE CONTROL, which is the PREMISE of every case above rather
// than a fourth symptom: §2.8.2 clause 2 says "An external instantiation behaves
// identically to a same-module instantiation of the same generic at the same
// arguments", so the one-module spelling of T1+T3 is what "correct" means here.
// It passed before this fix and must keep passing: the fix touches
// `buildImportedGenericVtable`, which a same-module instantiation never enters.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_instance_descriptor_2977, same_module_control_unchanged_spec_2_8_2) {
    int r = compileImportAndRun2977(
        "module DescLib2977T7; public class Unused2977T7 public _u: int := 1; end Unused2977T7;",
        "module DescApp2977T7; import DescLib2977T7; "
        "public class GLocal2977T7<T> "
        "  public _t: T; "
        "  public _t2: T; "
        "  public _w: int := 21; "
        "end GLocal2977T7; "
        "public class M2977T7 "
        "  public static method Main(): int "
        "    using var g := new GLocal2977T7<string>(); "
        "    var o: Object := g; "
        "    if o is not GLocal2977T7<string> then return 91; end if; "
        "    if o is GLocal2977T7<int> then return 92; end if; "
        "    return 44; "
        "  end Main; "
        "end M2977T7;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 44);
}

// ===========================================================================
// T8 — TWO INSTANTIATIONS THAT SHARE AN LLVM LAYOUT, which is a DIFFERENT
// question from T3 and the one an intermediate version of this fix got wrong.
//
// T3's `GRe2977<string>` and `GRe2977<int>` lower to different field-type
// vectors (`ptr` vs `i32`), so `specializedVtables_`'s layout key already
// separated them. `GRe2977<Dog>` and `GRe2977<Cat>` do NOT: every class type
// erases to `ptr`, so both instantiations hash to ONE key and shared a single
// vtable — harmless while slot 0 was a private `[self, null]` matching nothing,
// but with a real identity installed the second instantiation inherited the
// FIRST one's descriptor. Measured on the intermediate version:
//
//     dogIsDog=1  dogIsCat=0  catIsCat=0  catIsDog=1
//
// `catIsDog=1` is a §12.9 invariance break in the FALSE-POSITIVE direction —
// strictly worse than the false negatives #2977/#2956 report, because the `as`
// behind it binds a `Cat` at the `Dog` static type (the #1843/#2001 type-
// confusion class). The fix keys the imported vtable by the descriptor global
// where one exists, exactly as `specializedVtableFor` has always done locally.
//
// ⚠ MAINTENANCE NOTE. This case's premise is that the two instantiations still
// SHARE a lowered layout. It cannot silently expire into vacuity: were `Dog2977`
// and `Cat2977` ever to lower differently, the layout key alone would separate
// them and this case would merely duplicate T3 — so the two classes are
// deliberately identical in shape (one `int` field each) and must stay so.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_instance_descriptor_2977, layout_sharing_instantiations_do_not_share_an_identity_spec_12_9) {
    int r = compileImportAndRun2977(
        "module DescLib2977T8; "
        "public class Dog2977T8 public d: int := 1; end Dog2977T8; "
        "public class Cat2977T8 public c: int := 2; end Cat2977T8; "
        "public class GRe2977T8<T> "
        "  public _t: T; "
        "  public _t2: T; "
        "  public _w: int := 21; "
        "end GRe2977T8;",
        "module DescApp2977T8; import DescLib2977T8; "
        "public class M2977T8 "
        "  public static method Main(): int "
        "    using var gd := new GRe2977T8<Dog2977T8>(); "
        "    using var gc := new GRe2977T8<Cat2977T8>(); "
        "    var od: Object := gd; "
        "    var oc: Object := gc; "
        "    if od is not GRe2977T8<Dog2977T8> then return 91; end if; "
        "    if od is GRe2977T8<Cat2977T8> then return 92; end if; "
        "    if oc is not GRe2977T8<Cat2977T8> then return 93; end if; "
        "    if oc is GRe2977T8<Dog2977T8> then return 94; end if; "
        "    return 44; "
        "  end Main; "
        "end M2977T8;");
    if (r == -2) return;
    // Pre-fix: 91 (nothing matched). Intermediate-fix: 93 (the second
    // instantiation carried the first's identity, and 94 was one step behind).
    BEZEL_EXPECT_EQ(r, 44);
}

}  // namespace bezel::module
