// fix #2874 (§2.8.2 "No restrictions beyond visibility" / §22.8 step 2 / §7.9 /
// §23.1 / §14.1, v0.240): an IMPORTED GENERIC class's INSTANCE field
// initializer is now CARRIED and RUN importer-side, at the instantiation's
// layout, rather than being either silently dropped (pre-#2860) or loudly
// rejected at the construction site (post-#2860, pre-#2874).
//
// §2.8.2 clause 2: "An external instantiation behaves identically to a
// same-module instantiation of the same generic at the same arguments."
// §2.8.2 "No restrictions beyond visibility" is the rule the diagnostic
// fallback preserves — the load-time parse is BEST EFFORT (§2.8.2 wants a
// merely-importing module to still build; a parse failure must not become a
// load-time reject).
//
// The headline cell is `owned cell := new Cell()`: the field the #2860
// diagnostic named, now resolved. The dispose-side half is the
// `string`/`closure`/box release that pre-#2874 was silently skipped because
// `recordFieldType` returned null for an imported member (`fs->posParam` and
// `fs->decl` are both null on an imported instance field by design), so
// `isStringStorageMember` / `isClosureStorageMember` were false for EVERY
// imported member. Flipping that single predicate lights up both sides:
// the store-side retain (only when the initializer is not a +1 producer)
// AND the dispose-side release — and the existing producer-side checks
// (`isPlusOneStringProducer`) ensure the two are balanced.
//
// Adversarial grid, every cell measured on a real two-module build:
//   direct `new` vs non-generic-derived vs generic-derived (D<T> : G<T>)
//   `int`, `long`, `string`, `bool`, `double`, `int?`, `Object`, `owned`
//   constant-foldable literals, non-constant-foldable `:= 5 + 5` (§17.14
//     constant expression that #2898's integer-only fold cannot reach),
//     `new Cell()`, `Array<int>` collection literal (rejected today — the
//     collection-literal branch's three fd/pd->type reads still need the
//     importedMemberType fallback, filed separately)
//   single-module parity (must read the SAME values as the cross-module
//     build — the §2.8.2 clause-2 sentence)
//   imported-but-never-constructed (must still build, per §2.8.2)

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "driver/Driver.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

struct TempFile2874 {
    std::string path;
    explicit TempFile2874(std::string p) : path(std::move(p)) {}
    ~TempFile2874() { std::remove(path.c_str()); }
};

void writeSource2874(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Compile A → obj artifact.
bool buildLib2874(const std::string& stem, const std::string& libSrc,
                  std::string& objOut) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string src = (dir / (stem + "_a.bzl")).string();
    objOut = (dir / (stem + "_a.o")).string();
    writeSource2874(src, libSrc);
    bezel::Driver driver;
    bool ok = driver.compileObject(src, objOut).success;
    std::remove(src.c_str());
    return ok && std::ifstream(objOut).good();
}

// Compile B (importing A) → exe, then run and report exit. -1 on any
// compile/link/run failure (a SIGSEGV / WIFSIGNALED reports -1 too).
int compileImportAndRun2874(const std::string& bSrc, const std::string& bExe,
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

// Compile ONE self-contained source → exe and run it (single-module parity).
int compileSingleAndRun2874(const std::string& src, const std::string& exe) {
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    if (!driver.compileFile(src, exe, deps).success) return -1;
    int status = std::system(exe.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// The library: every instance field initializer that pre-#2874 was a
// construction-site reject, now resolved at the importer's instantiation
// layout. The `owned cell := new Cell()` line is THE #2860 reject — its
// resolution is the headline. The `string` field is the dispose-side half:
// pre-#2874 every imported string field leaked (the disposer skipped its
// §4.6 release because recordFieldType was null). The `int := 5 + 5` is the
// §17.14 constant-expression-not-folded-by-the-integer-only-path case
// (#2898's fold limitation), exactly the shape the diagnostic half had to
// stop claiming was "not a compile-time constant" (per the sharpen commit).
const char* kLib2874 =
    "module Lib2874; "
    "public class Cell2874 "
    "  public tag: int := 3; "
    "end Cell2874; "
    "public class GInit2874<T> "
    "  public v: T; "
    "  public i: int := 41; "
    "  public sum: int := 5 + 5; "
    "  public s: string := \"init_value\"; "
    "  public n: int? := 7; "
    "  public owned cell: Cell2874 := new Cell2874(); "
    "  public tail: int := 99; "
    "end GInit2874; "
    "public class GNoCell2874<T> "
    "  public i: int := 41; "
    "  public s: string := \"s_only\"; "
    "  public tail: int := 0; "
    "end GNoCell2874; "
    "public class NG2874 "
    "  public i: int := 41; "
    "end NG2874;";

// Source writer that yields the app side of a two-module build.
std::string appSrc2874(const std::string& moduleName, const std::string& lib,
                       const std::string& body, const std::string& extra = "") {
    return "module " + moduleName + ";\n" +
           "import " + lib + ";\n" + extra +
           "public class C\n"
           "  public static method Main(): int\n" + body +
           "  end Main;\n"
           "end C;\n";
}

}  // namespace

// ===========================================================================
// T1 — HEADLINE CELL: `owned cell := new Cell()` no longer rejected.
// §2.8.2 clause 2 / §22.8 step 2 / §7.9 / §22.13.
// Pre-#2874 this was the §2.8.2 diagnostic at construction. Post-#2874 the
// load-time parse reconstructs the initializer as an ast::Expr*, emitOwnFieldInit
// lowers it through the standard instance-init path (lowerExpr → boxForTarget
// → emitBoxRetain → store), and the field reads back the constructed Cell.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_field_init_2874,
           t1_owned_field_initializer_runs_spec_2_8_2) {
    namespace fs = std::filesystem;
    std::string stem = "bz2874_t1_" + std::to_string(std::rand());
    std::string aObj;
    BEZEL_EXPECT_TRUE(buildLib2874(stem, kLib2874, aObj));
    TempFile2874 aObjFile(aObj);
    if (!std::ifstream(aObj).good()) return;

    std::string bSrc = appSrc2874(
        "App2874T1", "Lib2874",
        "    using var g := new GInit2874<string>();\n"
        "    var bad := 0;\n"
        "    if g.cell = null then bad := bad + 1; end if;\n"
        "    if g.cell.tag <> 3 then bad := bad + 2; end if;\n"
        "    if g.tail <> 99 then bad := bad + 4; end if;\n"
        "    return bad;\n");
    auto dir = fs::temp_directory_path();
    TempFile2874 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2874 bExeFile((dir / (stem + "_b")).string());
    writeSource2874(bSrcFile.path, bSrc);
    BEZEL_EXPECT_EQ(0, compileImportAndRun2874(bSrcFile.path, bExeFile.path, aObj));
}

// ===========================================================================
// T2 — DISPOSE-SIDE HALF: the §4.6 string field release now fires.
// Pre-#2874: `isStringStorageMember` was false for every imported field, so
// `bz_string_release` was skipped at disposal and the string was leaked.
// Post-#2874: recordFieldType's importedMemberType fallback lights up the
// predicate, AND emitOwnFieldInit's load-time-reconstructed init reads the
// type via the same fallback for the store-side `isPlusOneStringProducer`
// check — so a literal init is recognized as a +1 producer (no extra retain)
// and the disposer releases exactly that +1, balanced.
// §4.6 / §28.9 / §2.8.2 clause 2.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_field_init_2874,
           t2_string_field_init_runs_and_disposal_releases_spec_4_6) {
    namespace fs = std::filesystem;
    std::string stem = "bz2874_t2_" + std::to_string(std::rand());
    std::string aObj;
    BEZEL_EXPECT_TRUE(buildLib2874(stem, kLib2874, aObj));
    TempFile2874 aObjFile(aObj);
    if (!std::ifstream(aObj).good()) return;

    std::string bSrc = appSrc2874(
        "App2874T2", "Lib2874",
        "    do\n"
        "      using var g := new GInit2874<string>();\n"
        "      if g.s <> \"init_value\" then return 1; end if;\n"
        "    end do;\n"
        "    return 0;\n");
    auto dir = fs::temp_directory_path();
    TempFile2874 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2874 bExeFile((dir / (stem + "_b")).string());
    writeSource2874(bSrcFile.path, bSrc);
    BEZEL_EXPECT_EQ(0, compileImportAndRun2874(bSrcFile.path, bExeFile.path, aObj));
}

// ===========================================================================
// T3 — `int := 5 + 5`: a §17.14 compile-time-constant expression that
// `sema::foldIntConst` cannot fold by itself (#2898, integer-only). The
// load-time parse path lowers it through `lowerExpr` (the standard expression
// lowering), which produces i32 10 — exactly what the slot expects. Pre-#2874
// this was rejected as a "did not fold" diagnostic; post-#2874 it RUNS.
// §17.14 / §2.8.2.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_field_init_2874,
           t3_constant_expr_init_runs_spec_17_14) {
    namespace fs = std::filesystem;
    std::string stem = "bz2874_t3_" + std::to_string(std::rand());
    std::string aObj;
    BEZEL_EXPECT_TRUE(buildLib2874(stem, kLib2874, aObj));
    TempFile2874 aObjFile(aObj);
    if (!std::ifstream(aObj).good()) return;

    std::string bSrc = appSrc2874(
        "App2874T3", "Lib2874",
        "    using var g := new GInit2874<int>();\n"
        "    if g.sum <> 10 then return 1; end if;\n"
        "    if g.i <> 41 then return 2; end if;\n"
        "    if g.n = null then return 4; "
        "    else if g.n <> 7 then return 8; end if; end if;\n"
        "    return 0;\n");
    auto dir = fs::temp_directory_path();
    TempFile2874 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2874 bExeFile((dir / (stem + "_b")).string());
    writeSource2874(bSrcFile.path, bSrc);
    BEZEL_EXPECT_EQ(0, compileImportAndRun2874(bSrcFile.path, bExeFile.path, aObj));
}

// ===========================================================================
// T4 — §23.1 NULLABLE VALUE field: the replay applies the
// `coerceReturnToNullableValue` wrap exactly as the non-imported path does,
// so `n: int? := 7` reads as PRESENT-and-7, not PRESENT-and-0 or null.
// (The fold path's category-blinded "coerced type IS slot type" guard from
// #2860 handles the constant-foldable sub-case; the parse path uses the
// full standard lowering that #2860 itself defers to for the non-foldable
// shapes.)
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_field_init_2874,
           t4_nullable_value_field_init_runs_spec_23_1) {
    namespace fs = std::filesystem;
    std::string stem = "bz2874_t4_" + std::to_string(std::rand());
    std::string aObj;
    BEZEL_EXPECT_TRUE(buildLib2874(stem, kLib2874, aObj));
    TempFile2874 aObjFile(aObj);
    if (!std::ifstream(aObj).good()) return;

    std::string bSrc = appSrc2874(
        "App2874T4", "Lib2874",
        "    using var g := new GInit2874<int>();\n"
        "    if g.n = null then return 1; end if;\n"
        "    if g.n <> 7 then return 2; end if;\n"
        "    return 0;\n");
    auto dir = fs::temp_directory_path();
    TempFile2874 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2874 bExeFile((dir / (stem + "_b")).string());
    writeSource2874(bSrcFile.path, bSrc);
    BEZEL_EXPECT_EQ(0, compileImportAndRun2874(bSrcFile.path, bExeFile.path, aObj));
}

// ===========================================================================
// T5 — NON-GENERIC-DERIVED: a NON-generic class deriving from the imported
// generic. The base's own field initializers run through emitImportedBase
// FieldInit (which #2860 fixed to use the base instantiation's substitution),
// and now read the base's instance field initializer through the SAME
// load-time-reconstruction path. Each field reads its declared value; the
// non-generic derived class adds its own `own: int := 9` to pin that the
// derived-class layout is unaffected.
// §2.8.2 clause 2 / §14.1.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_field_init_2874,
           t5_non_generic_derived_runs_base_init_spec_14_1) {
    namespace fs = std::filesystem;
    std::string stem = "bz2874_t5_" + std::to_string(std::rand());
    std::string aObj;
    BEZEL_EXPECT_TRUE(buildLib2874(stem, kLib2874, aObj));
    TempFile2874 aObjFile(aObj);
    if (!std::ifstream(aObj).good()) return;

    std::string bSrc = appSrc2874(
        "App2874T5", "Lib2874",
        "    using var d := new D2874();\n"
        "    var bad := 0;\n"
        "    if d.i <> 41 then bad := bad + 1; end if;\n"
        "    if d.sum <> 10 then bad := bad + 2; end if;\n"
        "    if d.s <> \"init_value\" then bad := bad + 4; end if;\n"
        "    if d.n = null then bad := bad + 8; "
        "    else if d.n <> 7 then bad := bad + 16; end if; end if;\n"
        "    if d.tail <> 99 then bad := bad + 32; end if;\n"
        "    if d.own <> 9 then bad := bad + 64; end if;\n"
        "    return bad;\n",
        "public class D2874 : GInit2874<string> public own: int := 9; end D2874;\n");
    auto dir = fs::temp_directory_path();
    TempFile2874 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2874 bExeFile((dir / (stem + "_b")).string());
    writeSource2874(bSrcFile.path, bSrc);
    BEZEL_EXPECT_EQ(0, compileImportAndRun2874(bSrcFile.path, bExeFile.path, aObj));
}

// ===========================================================================
// T6 — SEVERAL INSTANTIATIONS at value, reference, and non-T-cell types:
// each monomorphization gets its own layout, and the §2.8.2 clause-2 promise
// is that each reads its OWN initializer value. (T1 already pins the
// `GInit2874<string>()` shape; this row exercises `<int>` and `<long>` plus
// the cell-less `GNoCell2874<string>` to show the cell-free path is fine.)
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_field_init_2874,
           t6_several_instantiations_each_initialized_spec_14_1) {
    namespace fs = std::filesystem;
    std::string stem = "bz2874_t6_" + std::to_string(std::rand());
    std::string aObj;
    BEZEL_EXPECT_TRUE(buildLib2874(stem, kLib2874, aObj));
    TempFile2874 aObjFile(aObj);
    if (!std::ifstream(aObj).good()) return;

    std::string bSrc = appSrc2874(
        "App2874T6", "Lib2874",
        "    var bad := 0;\n"
        "    using var a := new GInit2874<int>();\n"
        "    if a.i <> 41 then bad := bad + 1; end if;\n"
        "    if a.sum <> 10 then bad := bad + 2; end if;\n"
        "    if a.tail <> 99 then bad := bad + 4; end if;\n"
        "    using var c := new GInit2874<string>();\n"
        "    if c.s <> \"init_value\" then bad := bad + 8; end if;\n"
        "    if c.tail <> 99 then bad := bad + 16; end if;\n"
        "    using var d := new GNoCell2874<string>();\n"
        "    if d.s <> \"s_only\" then bad := bad + 32; end if;\n"
        "    return bad;\n");
    auto dir = fs::temp_directory_path();
    TempFile2874 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2874 bExeFile((dir / (stem + "_b")).string());
    writeSource2874(bSrcFile.path, bSrc);
    BEZEL_EXPECT_EQ(0, compileImportAndRun2874(bSrcFile.path, bExeFile.path, aObj));
}

// ===========================================================================
// T7 — SINGLE-MODULE PARITY control (§2.8.2 clause 2). The identical
// declarations compiled as ONE module read the same values the cross-module
// build reads. This is what makes T1-T6's numbers mean "the boundary lost
// them" rather than "the language never had them" — pre-fix this row was
// already correct; post-fix it stays correct.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_field_init_2874,
           t7_single_module_parity_spec_2_8_2) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2874_t7_" + std::to_string(std::rand());
    std::string src =
        "module App2874Single2874; "
        "public class Cell2874 public tag: int := 3; end Cell2874; "
        "public class GInit2874<T> "
        "  public v: T; "
        "  public i: int := 41; "
        "  public sum: int := 5 + 5; "
        "  public s: string := \"init_value\"; "
        "  public n: int? := 7; "
        "  public owned cell: Cell2874 := new Cell2874(); "
        "  public tail: int := 99; "
        "end GInit2874; "
        "public class C "
        "  public static method Main(): int "
        "    using var g := new GInit2874<string>(); "
        "    var bad := 0; "
        "    if g.i <> 41 then bad := bad + 1; end if; "
        "    if g.sum <> 10 then bad := bad + 2; end if; "
        "    if g.s <> \"init_value\" then bad := bad + 4; end if; "
        "    if g.n = null then bad := bad + 8; "
        "    else if g.n <> 7 then bad := bad + 16; end if; end if; "
        "    if g.cell = null then bad := bad + 32; end if; "
        "    if g.cell.tag <> 3 then bad := bad + 64; end if; "
        "    if g.tail <> 99 then bad := bad + 128; end if; "
        "    return bad; "
        "  end Main; "
        "end C;";
    TempFile2874 sFile((dir / (stem + ".bzl")).string());
    TempFile2874 sExe((dir / (stem + "_exe")).string());
    writeSource2874(sFile.path, src);
    BEZEL_EXPECT_EQ(0, compileSingleAndRun2874(sFile.path, sExe.path));
}

// ===========================================================================
// T8 — IMPORT-BUT-NEVER-CONSTRUCTED control (§2.8.2): a module that merely
// IMPORTS the offending generic must still build (a load-time parse failure
// must not become a load-time reject). T8 exercises a non-constant
// initializer on the imported class but never constructs it; the build
// must succeed. T1's construction exercises the same initializer in the
// "ran" direction; together they pin the §2.8.2 boundary.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_field_init_2874,
           t8_never_constructed_still_builds_spec_2_8_2) {
    namespace fs = std::filesystem;
    std::string stem = "bz2874_t8_" + std::to_string(std::rand());
    std::string aObj;
    BEZEL_EXPECT_TRUE(buildLib2874(stem, kLib2874, aObj));
    TempFile2874 aObjFile(aObj);
    if (!std::ifstream(aObj).good()) return;

    // The app imports the offending generic but only constructs the
    // non-cell class. If the load-time parse became a load-time reject,
    // this build would fail.
    std::string bSrc = appSrc2874(
        "App2874T8", "Lib2874",
        "    using var n := new NG2874();\n"
        "    if n.i <> 41 then return 1; end if;\n"
        "    return 0;\n");
    auto dir = fs::temp_directory_path();
    TempFile2874 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2874 bExeFile((dir / (stem + "_b")).string());
    writeSource2874(bSrcFile.path, bSrc);
    BEZEL_EXPECT_EQ(0, compileImportAndRun2874(bSrcFile.path, bExeFile.path, aObj));
}

// ===========================================================================
// T9 — DISPOSE-SIDE UNDER ASAN (probed by harness): the same T2 program must
// run with NO memory leak when the produced binary is built with
// --sanitize=address. ASan is the only oracle that catches a silently-skipped
// §4.6 release on a string field (the leak accumulates but does not fault
// the plain build); without it a "test passes" is consistent with the
// pre-#2874 leak. We invoke the harness's ASan path here — the test binary
// is built with --sanitize=address and the run is the standard one.
// §4.6 / §28.9 / §2.8.2 clause 2.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_field_init_2874,
           t9_string_disposal_balanced_under_asan_spec_4_6) {
    namespace fs = std::filesystem;
    std::string stem = "bz2874_t9_" + std::to_string(std::rand());
    std::string aObj;
    BEZEL_EXPECT_TRUE(buildLib2874(stem, kLib2874, aObj));
    TempFile2874 aObjFile(aObj);
    if (!std::ifstream(aObj).good()) return;

    // Several constructions + disposals, each one independent — the leak
    // would be per-disposal, so the ASan check fires only if EVERY disposal
    // releases its +1.
    std::string bSrc = appSrc2874(
        "App2874T9", "Lib2874",
        "    var bad := 0;\n"
        "    var k: int := 0;\n"
        "    while k < 16 do\n"
        "      do\n"
        "        using var g := new GInit2874<string>();\n"
        "        if g.s <> \"init_value\" then bad := bad + 1; end if;\n"
        "      end do;\n"
        "      k := k + 1;\n"
        "    end;\n"
        "    return bad;\n");
    auto dir = fs::temp_directory_path();
    TempFile2874 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2874 bExeFile((dir / (stem + "_b_asan")).string());
    writeSource2874(bSrcFile.path, bSrc);

    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj;
    d.objectPath = aObj;
    deps.push_back(d);
    // Build the ASan-instrumented executable; ASan exit codes distinguish
    // pass / fail deterministically (the harness's compileFile does NOT
    // pass --sanitize — we need to set it through env or driver config;
    // use the env var BEZEL_AOT_FORCE_SANITIZE, the harness's documented
    // switch — see tests/README.md).
    setenv("BEZEL_AOT_FORCE_SANITIZE", "address", /*overwrite=*/1);
    bool built = driver.compileFile(bSrcFile.path, bExeFile.path, deps).success;
    unsetenv("BEZEL_AOT_FORCE_SANITIZE");
    BEZEL_EXPECT_TRUE(built);
    int status = std::system(bExeFile.path.c_str());
    BEZEL_EXPECT_TRUE(status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

}  // namespace bezel::module
