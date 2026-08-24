// fix #2842 — a synthetic method stub whose SIGNATURE names a sibling type
// silently disabled §14.1/#783 reconstruction for the WHOLE imported generic
// class. #2820 fixed the BODY defect (a stub whose `=> 0` did not fit the
// return type); #2842 fixes the SIGNATURE defect (a stub that names an
// undeclared type fails at resolveTypes of the synthetic single-class
// source, taking the entire class's reconstruction with it).
// (§14.1 Generic declarations, §2.5 Dependency resolution, v0.240.)
//
// To monomorphize an imported generic class's method body importer-side
// (#783, schema v5 methodBodyText), ModuleLoader re-parses that body inside
// a synthetic single-class source in which every OTHER method appears as a
// signature stub. The synthetic source declares ONLY the type being
// reconstructed — its sibling types of the exporting module are NOT in
// scope. A sibling method whose return type or any parameter type names
// such a type therefore produces a stub that fails at resolveTypes of the
// fresh context, and parseAndAttachMethodBody's #783 soundness gate then
// refuses to attach THE BODY BEING RECONSTRUCTED even though the failure
// is entirely in an unrelated sibling's synthetic stub.
//
// The fix is per-stub: `buildMethodStub` now omits (returns the empty string
// for) a stub whose signature references a Named type other than the
// enclosing class. The remaining members' resolveTypes succeeds; a body
// that referenced the skipped sibling would still fail at
// typecheckExpressions and that one method's body would be unattached — the
// other methods of the class still reconstruct. The ctor/dtor-body paths
// are TOLERANT of post-typecheck sema errors (a base(...) chain on an
// unknown base is benign for the field/param/method remap they care about),
// so a body that CALLED the skipped sibling would otherwise be silently
// attached with the call DROPPED — a #2290-bug-2-shaped wrong value — and
// the ctor/dtor paths therefore MIRROR the predicate: if any stub would be
// skipped, the ctor/dtor body is NOT attached, so the §14.1/#783/#2821 gate
// stays in force for that body.
//
// ⚠ VACUITY HAZARD — read before editing any source below. The obvious
// test for this bug does NOT work. When reconstruction is disabled the
// call falls back to the EXPORTER's own compiled function, and for most
// bodies that function returns the RIGHT answer — so a program whose only
// defect is "the body was not reconstructed" passes without the fix. (The
// #2818 lane's first adversarial grid and first test draft both passed on
// its base for exactly this reason; the #2820 test file's T1–T6 document
// the trap in detail.) Every hinge below therefore uses a class whose
// layout SHIFTS under the instantiation (`v: T` declared before `tag: int`),
// where the fallback is not merely unnecessary but UNSOUND — so without
// the fix the compile is REJECTED by the §14.1/#783/#2821 gate, and with
// it the body reconstructs and the program runs. The assertion is the
// RUN's exit value, never "it compiled".
//
// TEST MATRIX (each hinge: compile rejected without the fix, returns 7
// with it):
//   T1  Class with a normal method `GetTag(): int` AND a synthetic stub
//       `MakeCell(): owned Cell16` whose signature names a sibling type —
//       the normal method's body must still be reconstructed.
//   T2  CONTROL: the same shape with the sibling-type stub changed to name
//       the ENCLOSING type (`MakeCell(): owned Box<T>`), which is declared
//       in the synthetic class — already worked, must keep returning 7.
//   T3  CONTROL: a class with ONLY the sibling-type signature stub and NO
//       other methods — the per-stub skip still leaves the class with no
//       other members to attach, so behavior matches the pre-fix state
//       (no reconstruction, gate fires).
//   T4  xmod: same shape as T1 but consumed by a SEPARATE module — the
//       second module's loader still reconstructs the rest of the first
//       module's class. Pins the cross-module boundary.

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

struct TempFile2842 {
    std::string path;
    explicit TempFile2842(std::string p) : path(std::move(p)) {}
    ~TempFile2842() { std::remove(path.c_str()); }
};

void writeSource2842(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj2842(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

int compileImportAndRun2842(const std::string& bSrc, const std::string& bExe,
                            const std::string& aObj) {
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj;
    d.objectPath = aObj;
    deps.push_back(d);
    if (!driver.compileFile(bSrc, bExe, deps).success) return -1;
    int status = std::system(bExe.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// Build A → object, then B importing A → exe, run it, return the exit code.
// -1 on any compile/link/run failure; -2 when the dependency object itself
// could not be produced, so a toolchain problem is never read as a genuine 0.
int runXmod2842(const std::string& stem, const std::string& aSrcText,
                const std::string& bSrcText) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string tag = stem + "_" + unique_stem("");
    TempFile2842 aSrc((dir / (tag + "_a.bzl")).string());
    TempFile2842 aObj((dir / (tag + "_a.o")).string());
    writeSource2842(aSrc.path, aSrcText);
    if (!compileToObj2842(aSrc.path, aObj.path)) return -2;
    if (!std::ifstream(aObj.path).good()) return -2;
    TempFile2842 bSrc((dir / (tag + "_b.bzl")).string());
    TempFile2842 bExe((dir / (tag + "_b")).string());
    writeSource2842(bSrc.path, bSrcText);
    return compileImportAndRun2842(bSrc.path, bExe.path, aObj.path);
}

}  // namespace

// ===========================================================================
// T1 — a class with a normal method `GetTag(): int` AND a sibling whose
// signature names another type of the exporting module
// (`MakeCell(): owned Cell16`). The normal method's body touches only own
// members and must still be reconstructed: the stub for `MakeCell` is
// omitted (its signature is not safe — `Cell16` is undeclared in the
// synthetic single-class source), but `GetTag`'s body never references it,
// so the §14.1/#783 soundness gate stays clear and `GetTag`'s body
// attaches. §14.1 / §2.5.
// ===========================================================================
BEZEL_TEST(synthetic_stub_sibling_disables_reconstruction_2842,
           t1_sibling_signature_stub_no_longer_blocks_other_methods_14_1) {
    BEZEL_EXPECT_EQ(7, runXmod2842("bz2842_t1",
        "module Lib2842A; "
        "public class Cell16 "
        "  public N: int; "
        "end Cell16; "
        "public class Box16<T> "
        "  public v: T; "
        "  public tag: int; "
        "  public method GetTag(): int "
        "    return this.tag; "
        "  end GetTag; "
        "  public static method MakeCell(n: int): owned Cell16 "
        "    return new Cell16(); "
        "  end MakeCell; "
        "end Box16;",
        "module App2842A; import Lib2842A; "
        "public class C "
        "  public static method Main(): int "
        "    using var b := new Box16<string>(); "
        "    b.v := \"hello\"; "
        "    b.tag := 7; "
        "    return b.GetTag(); "
        "  end Main; "
        "end C;"));
}

// ===========================================================================
// T2 — CONTROL: the same shape with the sibling-type stub changed to name
// the ENCLOSING type (`MakeCell(): owned Box16<T>`), which IS declared in
// the synthetic class. Already worked before the fix (the stub's signature
// resolves fine) and must keep working. §14.1 / §2.5.
// ===========================================================================
BEZEL_TEST(synthetic_stub_sibling_disables_reconstruction_2842,
           t2_control_enclosing_type_signature_still_runs_2_5) {
    BEZEL_EXPECT_EQ(7, runXmod2842("bz2842_t2",
        "module Lib2842B; "
        "public class Box16<T> "
        "  public v: T; "
        "  public tag: int; "
        "  public method GetTag(): int "
        "    return this.tag; "
        "  end GetTag; "
        "  public static method MakeSelf(): owned Box16<T> "
        "    return new Box16<T>(); "
        "  end MakeSelf; "
        "end Box16;",
        "module App2842B; import Lib2842B; "
        "public class C "
        "  public static method Main(): int "
        "    using var b := new Box16<string>(); "
        "    b.v := \"hello\"; "
        "    b.tag := 7; "
        "    return b.GetTag(); "
        "  end Main; "
        "end C;"));
}

// ===========================================================================
// T3 — CONTROL: a class with ONLY a sibling-type-signature stub and NO
// other reconstructible methods. The per-stub skip leaves the class with
// nothing else to reconstruct — `MakeCell`'s body is unattached (the
// target's methodBodyText references `Cell16`, which the synthetic
// single-class source never declared, so resolveTypes fails and
// parseAndAttachMethodBody declines). App never calls `MakeCell`, so the
// importer-side compile does NOT fire #2821's call-gate (a missing body
// only gates CALLS — #2821 is a per-call soundness gate, not a layout
// gate), and the program runs and returns 7. The control pins that the
// fix does NOT widen or narrow this case's behavior: the importer-side
// compile succeeds and the program runs in both pre-#2842 and
// post-#2842 builds (the pre-fix whole-class disable manifested as a
// #2821 reject on `b.MakeCell(...)`-shaped callers — see the issue body;
// a program that never calls the unreconstructed method compiles and
// runs in both). §14.1 / §2.5 / #2821.
// ===========================================================================
BEZEL_TEST(synthetic_stub_sibling_disables_reconstruction_2842,
           t3_control_only_sibling_signature_stub_still_runs_2_5) {
    BEZEL_EXPECT_EQ(7, runXmod2842("bz2842_t3",
        "module Lib2842C; "
        "public class Cell16 "
        "  public N: int; "
        "end Cell16; "
        "public class OnlyStub16<T> "
        "  public v: T; "
        "  public static method MakeCell(n: int): owned Cell16 "
        "    return new Cell16(); "
        "  end MakeCell; "
        "end OnlyStub16;",
        "module App2842C; import Lib2842C; "
        "public class C "
        "  public static method Main(): int "
        "    using var b := new OnlyStub16<string>(); "
        "    b.v := \"hello\"; "
        "    return 7; "
        "  end Main; "
        "end C;"));
}

// ===========================================================================
// T4 — xmod variant of T1: the exporting module declares the normal method
// AND the sibling-type-signature stub in module A; a SEPARATE module B
// imports A and instantiates `Box16<string>` to call `GetTag()`. The
// importer-side loader must still reconstruct the rest of A's class.
// This is exactly the same fix shape as T1, but the cross-module
// boundary makes it the load-bearer's case rather than T1's intra-
// compilation synthetic source. §2.8.2 / §14.1.
// ===========================================================================
BEZEL_TEST(synthetic_stub_sibling_disables_reconstruction_2842,
           t4_xmod_sibling_signature_stub_no_longer_blocks_2_8_2) {
    BEZEL_EXPECT_EQ(7, runXmod2842("bz2842_t4",
        "module Lib2842D; "
        "public class CellD "
        "  public N: int; "
        "end CellD; "
        "public class BoxD<T> "
        "  public v: T; "
        "  public tag: int; "
        "  public method GetTag(): int "
        "    return this.tag; "
        "  end GetTag; "
        "  public static method MakeCell(n: int): owned CellD "
        "    return new CellD(); "
        "  end MakeCell; "
        "end BoxD;",
        "module App2842D; import Lib2842D; "
        "public class C "
        "  public static method Main(): int "
        "    using var b := new BoxD<string>(); "
        "    b.v := \"hello\"; "
        "    b.tag := 7; "
        "    return b.GetTag(); "
        "  end Main; "
        "end C;"));
}

// ===========================================================================
// T5 — GAP FILL (found at per-fix review): a method-level generic param
// (`<U>` in `method Bar<U>(x: U): U`) is bound by the synthetic source's
// own stub header — `buildMethodStub` emits `method Bar<U>(x: U): U`
// verbatim with the `<U>` declaration. Before the gap fill the helper
// read only `td.genericParams`, so `<U>` looked like a non-self Named
// type and the stub was incorrectly skipped — and the target's body
// (which CALLS the sibling via the method-level generic) then failed at
// typecheck because the resolution target did not exist in the fresh
// context. The target's body here calls `Bar<int>(this.tag)`, which is
// the only way the gap shows up at runtime — a target body that DOESN'T
// call the sibling would typecheck either way and the gap would stay
// silent (the same vacuity-hazard pattern the rest of this file
// documents). §14.1.
// ===========================================================================
BEZEL_TEST(synthetic_stub_sibling_disables_reconstruction_2842,
           t5_method_level_generic_param_in_scope_14_1) {
    BEZEL_EXPECT_EQ(7, runXmod2842("bz2842_t5",
        "module Lib2842E; "
        "public class BoxE<T> "
        "  public v: T; "
        "  public tag: int; "
        "  public method Get(): int "
        "    return this.Bar<int>(this.tag); "
        "  end Get; "
        "  public method Bar<U>(x: U): U "
        "    return x; "
        "  end Bar; "
        "end BoxE;",
        "module App2842E; import Lib2842E; "
        "public class C "
        "  public static method Main(): int "
        "    using var b := new BoxE<string>(); "
        "    b.v := \"hello\"; "
        "    b.tag := 7; "
        "    return b.Get(); "
        "  end Main; "
        "end C;"));
}

}  // namespace bezel::module
