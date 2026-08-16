// Spec-derived regression test for issue #2981.
//
// §12.7 (Type tests: `is`, `is not`, `as`) / §12 (Inheritance and Dispatch) /
// §17.4 / §31.2.1 (Arm patterns) / §2.8.3 rule 4 / §2.5 / §14.1, v0.240.
//
// THE DEFECT — A NULL IN THE MIDDLE OF A DESCRIPTOR ARRAY. `emitIsA` walks an
// object's descriptor chain from index 0 and TERMINATES on the first null
// element (LlvmBackend_Layout.cpp): null is the array's TERMINATOR, not a
// "no descriptor here" placeholder. `declareVtables` Pass C nonetheless pushed
// one for any transitive super with no `descArray_` entry, which silently
// discarded every entry after it.
//
// Reachable through exactly one shape today, and it is a legal one: a LOCAL
// class deriving from an IMPORTED GENERIC instantiation. A generic CLASS exports
// no shared descriptor (§2.5) and the importer declares none for it (#2379
// admits only a generic INTERFACE), so `descArray_.find(base)` misses. The
// emitted chain was
//
//     @"td$A120.D120" = [ptr @"td$A120.D120", ptr null,
//                        ptr @"td$GBase120_int", ptr null]
//
// with the base's per-instantiation descriptor correctly minted by the same
// function's `extraInstOf` loop and sitting at an index nothing could reach.
// Measured on a real two-module build, on this fix's parent:
//
//     self=1  gbase=0  wrong=0        gbase=1 is correct
//
// — a §12.7 silent false on a legal program, with no crash and no diagnostic at
// any severity. §12 makes a `D120` an instance of its base, and §2.8.3 rule 4
// draws no artifact distinction.
//
// THE FIX is the PROPERTY, not the case: a missing super now contributes the
// SELF descriptor rather than a null, so no null can appear before the
// terminator whatever the reason for the miss. The self descriptor is the one
// constant that provably cannot manufacture a FALSE match — it is already
// element 0, so repeating it re-asserts a match the walk makes anyway — and it
// is exactly what the `extraInstOf` loop 20 lines below already does for its own
// mint failure, with the same reason written out.
//
// NOT the same defect as #2977/#2956, and measured rather than assumed: this
// shape reproduced BYTE-IDENTICALLY with those applied and without them. Those
// were `buildImportedGenericVtable` installing the wrong IDENTITY in an
// imported instantiation's own vtable; this is Pass C building a TRUNCATED chain
// for a LOCAL class's descriptor.
//
// Built like its siblings tests/module/xmod_imported_generic_instance_
// descriptor_2977_2956_test.cpp and .../xmod_imported_generic_iface_accessor_
// 2894_test.cpp: a real `bezelc -c lib.bzl` + `bezelc --import lib.o app.bzl` +
// a native run.

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

struct TempFile2981 {
    std::string path;
    explicit TempFile2981(std::string p) : path(std::move(p)) {}
    ~TempFile2981() { std::remove(path.c_str()); }
};

void writeSource2981(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj2981(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// -2 = the dependency toolchain is unavailable (test skips); -1 = the app did
// not build/link OR did not exit normally. An unhandled exception exits 70
// through the normal path, so it stays a distinguishable VALUE.
int compileImportAndRun2981(const std::string& libSrc,
                            const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2981_");
    TempFile2981 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2981 aObj((dir / (stem + "_a.o")).string());
    writeSource2981(aSrc.path, libSrc);
    if (!compileToObj2981(aSrc.path, aObj.path)) return -2;
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile2981 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2981 bExe((dir / stem).string());
    writeSource2981(bSrc.path, appSrc);
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
// T1 — THE ISSUE'S OWN REPRO. `D2981T1: GBase2981T1<int>` is a local class over
// an imported generic base; `is GBase2981T1<int>` must be TRUE (§12/§12.7), the
// sibling instantiation `GBase2981T1<string>` must stay FALSE, and the class's
// own test must be unaffected. All three in one program, so a partial
// regression cannot hide behind a passing sibling.
//
// TWO `T`-typed fields on the base are load-bearing: with a single one the i32
// placeholder and the `ptr` share an 8-byte-aligned slot and the erased and
// composed layouts coincide (#2906/#2907's measured vacuity trap), so at
// `T = int` vs `T = string` this base genuinely moves.
// ===========================================================================
BEZEL_TEST(xmod_local_subclass_of_imported_generic_base_2981, local_subclass_matches_its_imported_generic_base_spec_12_7) {
    int r = compileImportAndRun2981(
        "module BaseLib2981T1; "
        "public class GBase2981T1<T> "
        "  public _t: T; "
        "  public _t2: T; "
        "  public _w: int := 21; "
        "end GBase2981T1;",
        "module BaseApp2981T1; import BaseLib2981T1; "
        "public class D2981T1: GBase2981T1<int> "
        "  public _d: int := 3; "
        "end D2981T1; "
        "public class M2981T1 "
        "  public static method Main(): int "
        "    using var d := new D2981T1(); "
        "    var o: Object := d; "
        "    if o is not D2981T1 then return 91; end if; "
        "    if o is not GBase2981T1<int> then return 92; end if; "
        "    if o is GBase2981T1<string> then return 93; end if; "
        "    return 44; "
        "  end Main; "
        "end M2981T1;");
    if (r == -2) return;
    // Pre-fix: 92 — the null at index 1 cut the chain before the base entry.
    BEZEL_EXPECT_EQ(r, 44);
}

// ===========================================================================
// T2 — THE SAME TRUNCATION, ON THE ARM THAT MAKES IT A SOUNDNESS BUG RATHER
// THAN A WRONG ANSWER. Pass C appends the BUILT-IN exception ancestors (#596,
// `btd$Exception`) AFTER the user supers, so a null among the supers swallowed
// those too: a local exception class over an imported generic exception base was
// not caught by ANY arm and the process died unhandled (exit 70).
//
// The precise arm is offered first so both the base-instantiation entry and the
// built-in entry are exercised, and `e.W` is read through the binding so this
// pins "the arm matched AND bound an object whose storage is where the importer
// thinks it is", not merely "an arm matched".
// ===========================================================================
BEZEL_TEST(xmod_local_subclass_of_imported_generic_base_2981, local_exception_subclass_of_imported_generic_base_is_catchable_spec_31_2_1) {
    int r = compileImportAndRun2981(
        "module BaseLib2981T2; "
        "public class GExB2981T2<T>: Exception "
        "  public _t: T; "
        "  public _t2: T; "
        "  public W: int; "
        "  public constructor(n: int) base(\"gb\"); "
        "    W := n; "
        "  end constructor; "
        "end GExB2981T2;",
        "module BaseApp2981T2; import BaseLib2981T2; "
        "public class DEx2981T2: GExB2981T2<int> "
        "  public constructor(n: int) base(n); "
        "  end constructor; "
        "end DEx2981T2; "
        "public class M2981T2 "
        "  public static method Main(): int "
        "    var a: int := 0; "
        "    try "
        "      throw new DEx2981T2(7); "
        "    catch var e: GExB2981T2<int> => a := e.W; "
        "    catch var e2: Exception => a := 90; "
        "    end; "
        "    if a <> 7 then return 91; end if; "
        "    using var d := new DEx2981T2(3); "
        "    var o: Object := d; "
        "    if o is not GExB2981T2<int> then return 92; end if; "
        "    if o is not Exception then return 93; end if; "
        "    return a + 37; "
        "  end Main; "
        "end M2981T2;");
    if (r == -2) return;
    // 7 (the precise arm's bound `W`) + 37.
    BEZEL_EXPECT_EQ(r, 44);
}

// ===========================================================================
// T3 — CONTROL. A local class over an imported NON-generic base, and a local
// class over an imported INTERFACE, both of which DO have a `descArray_` entry
// and were always correct. The fix replaces a null with a self-duplicate, and a
// self-duplicate can only ever re-assert element 0's match — this case is what
// says the shapes that never had a missing super are untouched, including the
// negative test that must stay negative.
// ===========================================================================
BEZEL_TEST(xmod_local_subclass_of_imported_generic_base_2981, nongeneric_imported_base_and_interface_unchanged_spec_2_5) {
    int r = compileImportAndRun2981(
        "module BaseLib2981T3; "
        "public interface IMark2981T3 "
        "  method Twice(): int; "
        "end IMark2981T3; "
        "public class Plain2981T3 "
        "  public _b: int := 5; "
        "end Plain2981T3; "
        "public class Other2981T3 "
        "  public _o: int := 6; "
        "end Other2981T3;",
        "module BaseApp2981T3; import BaseLib2981T3; "
        "public class D2981T3: Plain2981T3, IMark2981T3 "
        "  public _d: int := 3; "
        "  public method Twice(): int return 2; end Twice; "
        "end D2981T3; "
        "public class M2981T3 "
        "  public static method Main(): int "
        "    using var d := new D2981T3(); "
        "    var o: Object := d; "
        "    if o is not D2981T3 then return 91; end if; "
        "    if o is not Plain2981T3 then return 92; end if; "
        "    if o is not IMark2981T3 then return 93; end if; "
        "    if o is Other2981T3 then return 94; end if; "
        "    return 44; "
        "  end Main; "
        "end M2981T3;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 44);
}

}  // namespace bezel::module
