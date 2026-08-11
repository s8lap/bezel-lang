// fix #2840 — a class deriving from a generic base got the T→i32 PLACEHOLDER
// layout for the inherited type-parameter-typed field, so writing it clobbered
// the following field (or aborted the compiler outright).
// (§2.8.2 rule 2, §12 Inheritance and Dispatch, §14.1 Generic declarations,
//  §2.5 Dependency resolution, v0.240.)
//
// §2.8.2 rule 2 is the governing sentence for the cross-module half:
//
//     "Instantiate every public generic type and generic method at arbitrary
//      module-external type arguments satisfying the declared constraints. An
//      external instantiation behaves identically to a same-module
//      instantiation of the same generic at the same arguments."
//
// TWO independent placeholder leaks produced one symptom, and BOTH must be
// closed — fixing either alone is not merely incomplete, it is WORSE (see the
// measured 4-cell matrix below):
//
//   (a) `classStruct`'s base-chain walk (LlvmBackend_Layout.cpp) read the
//       base's type-parameter NAMES off `base->decl`. An IMPORTED base has no
//       ast::TypeDecl (ModuleLoader::reconstructTypeSymbol never sets `decl`),
//       so the walk broke out and the DERIVED class fell back to the
//       unspecialized layout — `v: T` a 4-byte slot in an object the rest of
//       codegen believes is 8-byte-wide there. The parameter names ARE
//       available on the imported TypeSymbol (`genericParams`, populated from
//       the metadata), and the very same `td ? td->genericParams :
//       ts->genericParams` fallback was already written twice in
//       LlvmBackend_Calls2.cpp (§14.1/#783 and §22.12/#684).
//
//   (b) `fieldAddr`'s inherited-field branch required the RECEIVER to be a
//       generic instantiation (`!targetTy->genericArgs.empty()`). A NON-generic
//       derived class (`class D : Base<long>`) has no generic args, so `d.v`
//       and `d.tag` skipped the ancestor substitution entirely and GEP'd
//       `classStruct(Base)` under the EMPTY substitution — the i32 template
//       layout. This half is NOT cross-module-specific: it reproduces in a
//       single module, and it is why the qualified access misbehaved while the
//       bare-identifier twin (`implicitFieldAddr`, fixed for this exact shape
//       by #989) was already correct.
//
// MEASURED 4-cell matrix, cross-module, `class D : GBase<long> { own: int }`
// writing `tag`, `own`, then `v` and reading all three back:
//
//   neither fix : v=3197704724 tag=28 own=9   (tag clobbered by v's high half)
//   (a) only    : v=3197704724 tag=28 own=9   (same — the access path dominates)
//   (b) only    : *** glibc heap corruption abort *** (the base field is now
//                 GEP'd at the CORRECT offset, past the end of the derived
//                 object the unspecialized layout still allocated)
//   both        : v=123456789012 tag=7 own=9  correct
//
// At `T = string` the "neither" cell is not even a wrong value: the compiler
// aborts with a raw LLVM assertion and no source location.
//
// TEST MATRIX (every case a real two-module build unless named single-module):
//   T1  xmod, `D : GBase<long>` + own field: all three fields read back
//       correctly. Without the fix: tag=28, own's offset shifted.
//   T2  xmod, `D : GBase<string>`: correct. Without the fix: compiler abort.
//   T3  xmod, `D : GBase<int>` — the instantiation IS the placeholder, so this
//       must be correct BOTH ways (no false reject / no behaviour change).
//   T4  xmod, a NON-generic imported base — outside the change entirely.
//   T5  SINGLE-MODULE control of T1: §2.8.2 rule 2 parity, and the pin for
//       half (b), which is not a module-boundary defect.
//   T6  xmod, a two-level chain `Leaf : Mid : GBase<long>` where Mid is the
//       non-generic link that binds the type argument.

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

struct TempFile2840 {
    std::string path;
    explicit TempFile2840(std::string p) : path(std::move(p)) {}
    ~TempFile2840() { std::remove(path.c_str()); }
};

void writeSource2840(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj2840(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// Compile B importing A → exe, run it, return the exit code (-1 on any
// compile/link/run failure).
int compileImportAndRun2840(const std::string& bSrc, const std::string& bExe,
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

// Compile ONE self-contained source → exe and run it (the single-module
// control; no dependency artifacts).
int compileSingleAndRun2840(const std::string& src, const std::string& exe) {
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    if (!driver.compileFile(src, exe, deps).success) return -1;
    int status = std::system(exe.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// The library: a generic base whose type-parameter field comes FIRST (so the
// instantiation shifts every later offset), plus a NON-generic base as the
// control that isolates "generic" from "inherited".
const char* kLib2840 =
    "module Lib2840; "
    "public class GBase2840<T> "
    "  public v: T; "
    "  public tag: int := 0; "
    "end GBase2840; "
    "public class NGBase2840 "
    "  public s: int := 0; "
    "  public tag: int := 0; "
    "end NGBase2840;";

// Every probe returns a BITMASK of which reads came back wrong, so a passing
// run is `0` and a failing one says WHICH field diverged (the protocol's
// "prove correct VALUES", not "does not crash").
const char* kMainLong2840 =
    "module App2840L; import Lib2840; "
    "public class D : GBase2840<long> "
    "  public own: int := 0; "
    "end D; "
    "public class C "
    "  public static method Main(): int "
    "    using var d := new D(); "
    "    d.tag := 7; "
    "    d.own := 9; "
    "    d.v := 123456789012L; "
    "    var bad := 0; "
    "    if d.v <> 123456789012L then bad := bad + 1; end if; "
    "    if d.tag <> 7 then bad := bad + 2; end if; "
    "    if d.own <> 9 then bad := bad + 4; end if; "
    "    return bad; "
    "  end Main; "
    "end C;";

}  // namespace

// ===========================================================================
// T1 — the load-bearing case: an importer-side class deriving from an IMPORTED
// generic base at a WIDER value type argument. Every field must read back what
// was written. Without the fix `tag` reads 28 — the high half of the `long`
// stored 4 bytes too early — so this returns 3, not 0. §2.8.2 rule 2 / §14.1.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_base_layout_2840,
           t1_inherited_long_field_roundtrips_2_8_2) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2840_t1_" + std::to_string(std::rand());
    TempFile2840 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2840 aObj((dir / (stem + "_a.o")).string());
    writeSource2840(aSrc.path, kLib2840);
    BEZEL_EXPECT_TRUE(compileToObj2840(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    TempFile2840 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2840 bExe((dir / (stem + "_b")).string());
    writeSource2840(bSrc.path, kMainLong2840);
    BEZEL_EXPECT_EQ(0, compileImportAndRun2840(bSrc.path, bExe.path,
                                               aObj.path));
}

// ===========================================================================
// T2 — the same shape at a REFERENCE type argument. Without the fix the
// compiler aborts on malformed IR (an i32 used as a `%bz.string` GEP base) with
// no source location, so this reports -1 rather than 0. §14.1 / §2.5.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_base_layout_2840,
           t2_inherited_string_field_roundtrips_14_1) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2840_t2_" + std::to_string(std::rand());
    TempFile2840 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2840 aObj((dir / (stem + "_a.o")).string());
    writeSource2840(aSrc.path, kLib2840);
    BEZEL_EXPECT_TRUE(compileToObj2840(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    std::string bSrc =
        "module App2840S; import Lib2840; "
        "public class D : GBase2840<string> "
        "  public own: int := 0; "
        "end D; "
        "public class C "
        "  public static method Main(): int "
        "    using var d := new D(); "
        "    d.tag := 7; "
        "    d.own := 9; "
        "    d.v := \"hello\"; "
        "    var bad := 0; "
        "    if d.v <> \"hello\" then bad := bad + 1; end if; "
        "    if d.tag <> 7 then bad := bad + 2; end if; "
        "    if d.own <> 9 then bad := bad + 4; end if; "
        "    return bad; "
        "  end Main; "
        "end C;";
    TempFile2840 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2840 bExe((dir / (stem + "_b")).string());
    writeSource2840(bSrcFile.path, bSrc);
    BEZEL_EXPECT_EQ(0, compileImportAndRun2840(bSrcFile.path, bExe.path,
                                               aObj.path));
}

// ===========================================================================
// T3 — CONTROL: `<int>` IS the placeholder, so nothing shifts and this must be
// correct both before and after. It pins that the change does not perturb the
// one instantiation that was already sound. §14.1.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_base_layout_2840,
           t3_int_instantiation_unchanged_14_1) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2840_t3_" + std::to_string(std::rand());
    TempFile2840 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2840 aObj((dir / (stem + "_a.o")).string());
    writeSource2840(aSrc.path, kLib2840);
    BEZEL_EXPECT_TRUE(compileToObj2840(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    std::string bSrc =
        "module App2840I; import Lib2840; "
        "public class D : GBase2840<int> "
        "  public own: int := 0; "
        "end D; "
        "public class C "
        "  public static method Main(): int "
        "    using var d := new D(); "
        "    d.tag := 7; "
        "    d.own := 9; "
        "    d.v := 5; "
        "    var bad := 0; "
        "    if d.v <> 5 then bad := bad + 1; end if; "
        "    if d.tag <> 7 then bad := bad + 2; end if; "
        "    if d.own <> 9 then bad := bad + 4; end if; "
        "    return bad; "
        "  end Main; "
        "end C;";
    TempFile2840 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2840 bExe((dir / (stem + "_b")).string());
    writeSource2840(bSrcFile.path, bSrc);
    BEZEL_EXPECT_EQ(0, compileImportAndRun2840(bSrcFile.path, bExe.path,
                                               aObj.path));
}

// ===========================================================================
// T4 — CONTROL: a NON-generic imported base. Inheritance across the module
// boundary without a type parameter is outside both halves of the change and
// must keep working. §12.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_base_layout_2840,
           t4_non_generic_imported_base_unchanged_12) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2840_t4_" + std::to_string(std::rand());
    TempFile2840 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2840 aObj((dir / (stem + "_a.o")).string());
    writeSource2840(aSrc.path, kLib2840);
    BEZEL_EXPECT_TRUE(compileToObj2840(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    std::string bSrc =
        "module App2840N; import Lib2840; "
        "public class D : NGBase2840 "
        "  public own: int := 0; "
        "end D; "
        "public class C "
        "  public static method Main(): int "
        "    using var d := new D(); "
        "    d.tag := 7; "
        "    d.own := 9; "
        "    d.s := 5; "
        "    var bad := 0; "
        "    if d.s <> 5 then bad := bad + 1; end if; "
        "    if d.tag <> 7 then bad := bad + 2; end if; "
        "    if d.own <> 9 then bad := bad + 4; end if; "
        "    return bad; "
        "  end Main; "
        "end C;";
    TempFile2840 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2840 bExe((dir / (stem + "_b")).string());
    writeSource2840(bSrcFile.path, bSrc);
    BEZEL_EXPECT_EQ(0, compileImportAndRun2840(bSrcFile.path, bExe.path,
                                               aObj.path));
}

// ===========================================================================
// T5 — SINGLE-MODULE control of T1. §2.8.2 rule 2 requires the external
// instantiation to behave identically to the same-module one, and this is the
// same-module side of that equation. It ALSO pins half (b) of the fix, which is
// not a module-boundary defect at all: without the fix this returns 3 exactly
// as the cross-module T1 does. §2.8.2 rule 2 / §12.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_base_layout_2840,
           t5_single_module_parity_2_8_2) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2840_t5_" + std::to_string(std::rand());
    std::string src =
        "module App2840Single; "
        "public class GBase2840<T> "
        "  public v: T; "
        "  public tag: int := 0; "
        "end GBase2840; "
        "public class D : GBase2840<long> "
        "  public own: int := 0; "
        "end D; "
        "public class C "
        "  public static method Main(): int "
        "    using var d := new D(); "
        "    d.tag := 7; "
        "    d.own := 9; "
        "    d.v := 123456789012L; "
        "    var bad := 0; "
        "    if d.v <> 123456789012L then bad := bad + 1; end if; "
        "    if d.tag <> 7 then bad := bad + 2; end if; "
        "    if d.own <> 9 then bad := bad + 4; end if; "
        "    return bad; "
        "  end Main; "
        "end C;";
    TempFile2840 sFile((dir / (stem + ".bzl")).string());
    TempFile2840 sExe((dir / (stem + "_exe")).string());
    writeSource2840(sFile.path, src);
    BEZEL_EXPECT_EQ(0, compileSingleAndRun2840(sFile.path, sExe.path));
}

// ===========================================================================
// T6 — the base chain reaches the imported generic through an intermediate
// NON-generic link (`Leaf : Mid : GBase2840<long>`), the #1101-B composition
// shape. Both halves must compose across the extra level. Without the fix this
// returns 24 (`tag` and `own2` both misread). §12 / §14.1.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_base_layout_2840,
           t6_two_level_chain_through_non_generic_link_12) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2840_t6_" + std::to_string(std::rand());
    TempFile2840 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2840 aObj((dir / (stem + "_a.o")).string());
    writeSource2840(aSrc.path, kLib2840);
    BEZEL_EXPECT_TRUE(compileToObj2840(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    std::string bSrc =
        "module App2840C; import Lib2840; "
        "public class Mid : GBase2840<long> "
        "end Mid; "
        "public class Leaf : Mid "
        "  public own2: int := 0; "
        "end Leaf; "
        "public class C "
        "  public static method Main(): int "
        "    using var l := new Leaf(); "
        "    l.tag := 3; "
        "    l.own2 := 4; "
        "    l.v := 987654321098L; "
        "    var bad := 0; "
        "    if l.v <> 987654321098L then bad := bad + 8; end if; "
        "    if l.tag <> 3 then bad := bad + 16; end if; "
        "    if l.own2 <> 4 then bad := bad + 32; end if; "
        "    return bad; "
        "  end Main; "
        "end C;";
    TempFile2840 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2840 bExe((dir / (stem + "_b")).string());
    writeSource2840(bSrcFile.path, bSrc);
    BEZEL_EXPECT_EQ(0, compileImportAndRun2840(bSrcFile.path, bExe.path,
                                               aObj.path));
}

}  // namespace bezel::module
