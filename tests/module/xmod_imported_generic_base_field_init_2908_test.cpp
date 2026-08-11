// Spec-derived regression test for the IMPORTED-BASE arm of issue #2908.
//
// §22.8 (Field initializers) / §14.1 (Generic declarations) / §12 (Inheritance
// and Dispatch) / §2.8.2 (Artifact interface) / §2.5, v0.240.
//
// WHY THIS FILE EXISTS SEPARATELY FROM #2908's OWN TEST. #2908's fix threads the
// class the §22.8 construction sequence STARTED at (the most-derived "root")
// through `emitImplicitBase`'s recursion and hands it to three consumers:
// `emitLocalBaseFieldInit`, `emitSpecializedImplicitBaseCtor`, and
// `emitImportedBaseFieldInit`. Its own commit message says of the third, in its
// SCOPE section, verbatim:
//
//     `emitImportedBaseFieldInit` takes the same threaded root, on the argument
//     that its use of the parameter is identical — but I did NOT build a
//     cross-module repro for that arm, so its improvement is inferred from code
//     shape, not measured.
//
// That inference is now MEASURED, and it was right — the arm is load-bearing,
// and until this file nothing pinned it. Built with a real two-module build
// (`bezelc -c lib.bzl`, then `bezelc --import lib.o app.bzl`, then a native
// run), against a compiler built from #2908's PARENT and from #2908 itself:
//
//     pre-#2908:   ba=0   bb=0   mb=7  lz=9   -> exit 16   (WRONG, and SILENT)
//     with #2908:  ba=41  bb=17  mb=7  lz=9   -> exit 74   (correct)
//
// `ba` and `bb` are the IMPORTED generic base's own §22.8 field initializers.
// Pre-fix they were applied at the ERASED `T -> i32` offsets inside an object
// allocated at the composed ones, so the stores landed on the inherited
// `bv`/`bv2` slots and the real fields kept their §7.9 defaults — a silent wrong
// value on a legal program, with no diagnostic at any severity. This is the
// cross-module face of exactly what #2908's single-module cases measure.
//
// ⚠ MAINTENANCE NOTE — THE PREMISE, AND HOW IT EXPIRES. The case is only about
// anything while `Base3<string>`'s instance layout genuinely MOVES relative to
// the erased template. That is why the base declares TWO type-parameter-typed
// fields ahead of the concrete ones: with a SINGLE `bv: T`, the i32 placeholder
// and the `ptr` occupy the same 8-byte-aligned slot, `ba`/`bb` land at identical
// offsets under both layouts, and this test silently becomes vacuous — it would
// pass with and without the fix. #2906 hit exactly that trap with #2907's filed
// repro (measured: the right answer on the unfixed parent). Do NOT narrow the
// base to one `T` field, and do not move `bv`/`bv2` after `ba`.
//
// The base deliberately carries NO `owned` field with a non-constant
// initializer: #2860 rejects constructing an imported generic whose field
// initializer is not a compile-time constant, so such a shape never reaches this
// path at all (measured — it is a clean §2.8.2 decline, not a crash).
//
// The chain is #2908's own shape: a NON-GENERIC leaf over a GENERIC middle link
// over the generic base. A non-generic leaf is what makes the walk need no seed
// at all, and is the population #2889's composition never contained.

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

struct TempFile2908 {
    std::string path;
    explicit TempFile2908(std::string p) : path(std::move(p)) {}
    ~TempFile2908() { std::remove(path.c_str()); }
};

void writeSource2908(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj2908(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// -2 = the dependency toolchain is unavailable (test skips); -1 = the app did
// not build/link or did not exit normally.
int compileImportAndRun2908(const std::string& libSrc,
                            const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2908x_" + std::to_string(std::rand());
    TempFile2908 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2908 aObj((dir / (stem + "_a.o")).string());
    writeSource2908(aSrc.path, libSrc);
    if (!compileToObj2908(aSrc.path, aObj.path)) return -2;
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile2908 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2908 bExe((dir / stem).string());
    writeSource2908(bSrc.path, appSrc);
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
// T1 — THE ARM #2908 LEFT INFERRED. A NON-GENERIC leaf over a GENERIC middle
// link over an IMPORTED GENERIC base: the base's own §22.8 field initializers
// must run at the COMPOSED offsets of the object that was actually allocated.
// Pre-#2908: 16 (`ba`/`bb` both left at their §7.9 default 0). With #2908: 74.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_base_field_init_2908, imported_generic_base_inits_at_composed_offsets_spec_22_8) {
    const char* lib =
        "module LB2908T1; "
        "public class Base2908T1<T> "
        "  public bv: T; "
        "  public bv2: T; "
        "  public ba: int := 41; "
        "  public bb: int := 17; "
        "end Base2908T1;";
    int r = compileImportAndRun2908(
        lib,
        "module AB2908T1; import LB2908T1; "
        "public class Mid2908T1<U> : Base2908T1<string> "
        "  public mv: U; "
        "  public mb: int := 7; "
        "end Mid2908T1; "
        "public class Leaf2908T1 : Mid2908T1<int> "
        "  public lz: int := 9; "
        "end Leaf2908T1; "
        "public class M2908T1 "
        "  public static method Main(): int "
        "    using var l := new Leaf2908T1(); "
        "    if l.ba <> 41 then return 91; end; "
        "    if l.bb <> 17 then return 92; end; "
        "    if l.mb <> 7 then return 93; end; "
        "    if l.lz <> 9 then return 94; end; "
        "    return l.ba + l.bb + l.mb + l.lz; "
        "  end Main; "
        "end M2908T1;");
    if (r == -2) return;
    // 41 + 17 + 7 + 9
    BEZEL_EXPECT_EQ(r, 74);
}

// ===========================================================================
// T2 — the same chain with a GENERIC leaf. This is #2908's declared ACCEPT
// CONTROL translated across the module boundary: #2889 already composes the
// whole chain into the active substitution when the NEW'd class is itself
// generic, so this shape was correct before #2908 and must stay correct.
// Passing with AND without the fix is the point of the case.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_base_field_init_2908, generic_leaf_over_imported_base_unchanged_spec_14_1) {
    const char* lib =
        "module LB2908T2; "
        "public class Base2908T2<T> "
        "  public bv: T; "
        "  public bv2: T; "
        "  public ba: int := 41; "
        "  public bb: int := 17; "
        "end Base2908T2;";
    int r = compileImportAndRun2908(
        lib,
        "module AB2908T2; import LB2908T2; "
        "public class Mid2908T2<U> : Base2908T2<string> "
        "  public mv: U; "
        "  public mb: int := 7; "
        "end Mid2908T2; "
        "public class GLeaf2908T2<V> : Mid2908T2<int> "
        "  public gv: V; "
        "  public lz: int := 9; "
        "end GLeaf2908T2; "
        "public class M2908T2 "
        "  public static method Main(): int "
        "    using var l := new GLeaf2908T2<int>(); "
        "    if l.ba <> 41 then return 91; end; "
        "    if l.bb <> 17 then return 92; end; "
        "    return l.ba + l.bb + l.mb + l.lz; "
        "  end Main; "
        "end M2908T2;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 74);
}
}  // namespace bezel::module
