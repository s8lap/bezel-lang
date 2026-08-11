// fix #2860 — an IMPORTED GENERIC class's instance field initializers never ran:
// every field read its §7.9 default, and a non-nullable `owned` field was left
// on the null watermark.
// (§2.8.2 clause 2 + "No restrictions beyond visibility", §22.8 step 2, §7.9,
//  §23.1, §14.1, §2.5, v0.240.)
//
// The governing sentence for the value half is §2.8.2 clause 2:
//
//     "Instantiate every public generic type and generic method at arbitrary
//      module-external type arguments satisfying the declared constraints. An
//      external instantiation behaves identically to a same-module
//      instantiation of the same generic at the same arguments."
//
// and for the carriage half, §2.8.2's "No restrictions beyond visibility":
//
//     "An implementation unable to carry some body shape in its interface
//      format must reject the export, or the import, with a diagnostic naming
//      the shape; it must not alter the instantiation's semantics."
//
// THREE independent defects produced the one symptom; all three are measured
// below on real two-module builds.
//
//  (a) `lowerNew`'s `if (ctx_.isImported(t))` branch (LlvmBackend_Exprs2.cpp)
//      exits ONLY by `return obj`, so it swallowed the no-explicit-ctor
//      construction sequence for EVERY imported type. That is right for a
//      NON-generic import — its initializers ran inside the exported
//      `bz$<Type>$init` called earlier in lowerNew — but an imported GENERIC
//      exports no such symbol (§14.1 monomorphization), so `emitInstanceInit`
//      installed the per-instantiation vtable and NOTHING ran §22.8 step 2.
//      `new GInit<string>()` therefore read every field's §7.9 default.
//
//  (b) `emitImportedBaseFieldInit`'s #2524 generic branch (LlvmBackend_Setup.cpp)
//      ran the monomorphized init sequence under whatever substitution happened
//      to be active — the base's own only when the DERIVED class is itself
//      generic and spells the base's parameter names (`D<T>: B<T>`, the shape
//      #2524's test pins). For a NON-generic derived class (`class D: B<string>`)
//      it was EMPTY, so `classStruct(base)` was the T→i32 placeholder layout
//      while the object was allocated with the instantiation's layout (#2840):
//      every store landed at the template offset and the (post-#2840, correct)
//      read saw the default.
//
//  (c) The same replay stored the folded constant VERBATIM, ignoring the slot.
//      `n: int? := 7` wrote a bare i32 over the §23.1 `{i8 hasValue; i32}` slot
//      (measured: the field then read as PRESENT-and-0), and `dec: decimal := 5`
//      read back 1014570924054025338880. Both are now coerced, with a
//      category-blind "the coerced value's type IS the slot's" guard.
//
// GAP DELIBERATELY LEFT LOUD RATHER THAN FIXED, per §2.8.2's "No restrictions
// beyond visibility": an initializer that is not compile-time-constant
// (`owned cell: Cell := new Cell()`, `[1,2,3]`, `Object := 1`) has no carriage
// at all — `renderConstExpr` returns "" and nothing reaches the consumer. T7
// pins that this is now a LOCATED compile error naming the field, not a
// silently null non-nullable field. Running such an initializer importer-side
// is filed as follow-up work.
//
// MEASURED on the parent commit (bd209a2), every cell a real two-module build:
//
//   direct `new GInit<string>()`           i=0, and a `string` field SEGV'd
//   `class D: GInit<string>` (non-generic)  i=0 (the (b) cell)
//   `class D<T>: GInit<T>` (generic)        i=41 — already correct, so T3 is an
//                                           honest non-hinge control
//   `n: int? := 7`                          direct: null; derived: present-and-0
//   `owned cell := new Cell()`              Live==0 in scope, then SEGV on use
//   the same lib imported, never constructed  fine both ways (T8 pins no false
//                                             reject)

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

struct TempFile2860 {
    std::string path;
    explicit TempFile2860(std::string p) : path(std::move(p)) {}
    ~TempFile2860() { std::remove(path.c_str()); }
};

void writeSource2860(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj2860(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// Compile B importing A → exe, run it, return the exit code (-1 on any
// compile/link/run failure; a SIGSEGV is WIFSIGNALED and also reports -1, which
// is what every pre-fix reference-field cell did).
int compileImportAndRun2860(const std::string& bSrc, const std::string& bExe,
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
int compileSingleAndRun2860(const std::string& src, const std::string& exe) {
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    if (!driver.compileFile(src, exe, deps).success) return -1;
    int status = std::system(exe.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// Compile B importing A, capturing the driver's stderr (fd 2) into `err`.
// Returns the compile's success flag. Same shape as the #2821 gate test: fd 2
// is redirected so both std::cerr diagnostics and llvm::errs() text are caught.
// The protocol requires asserting on diagnostic TEXT for a reject test — a
// bare "compile failed" cannot tell a genuine rejection from a crash.
bool compileImportCaptureErr2860(const std::string& bSrc,
                                 const std::string& bExe,
                                 const std::string& aObj, std::string& err) {
    namespace fs = std::filesystem;
    std::string errPath =
        (fs::temp_directory_path() /
         ("bz2860_err_" + std::to_string(std::rand()) + ".txt"))
            .string();

    std::fflush(stderr);
    std::cerr.flush();
    int saved = ::dup(STDERR_FILENO);
    int fd = ::open(errPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    bool success = false;
    if (fd >= 0) ::dup2(fd, STDERR_FILENO);
    {
        bezel::Driver driver;
        std::vector<bezel::Dependency> deps;
        bezel::Dependency d;
        d.metadataPath = aObj;
        d.objectPath = aObj;
        deps.push_back(d);
        success = driver.compileFile(bSrc, bExe, deps).success;
    }
    std::fflush(stderr);
    std::cerr.flush();
    if (fd >= 0) {
        ::close(fd);
        ::dup2(saved, STDERR_FILENO);
    }
    ::close(saved);

    std::ifstream in(errPath);
    err.assign((std::istreambuf_iterator<char>(in)),
               std::istreambuf_iterator<char>());
    std::remove(errPath.c_str());
    return success;
}

// The library. `v: T` comes FIRST so the instantiation shifts every later
// offset — that is what makes the (b) placeholder-layout cell observable rather
// than accidentally aligned. `GNoT2860` is the same shape WITHOUT a
// type-parameter-typed field (so the layout is placeholder-identical and only
// the (a) "nothing ran" defect can show), and `NG2860` is the NON-generic
// control that isolates "generic" from "imported".
const char* kLib2860 =
    "module Lib2860; "
    "public class GInit2860<T> "
    "  public v: T; "
    "  public i: int := 41; "
    "  public l: long := 1234567890123L; "
    "  public s: string := \"hi\"; "
    "  public b: bool := true; "
    "  public n: int? := 7; "
    "end GInit2860; "
    "public class GNoT2860<T> "
    "  public i: int := 41; "
    "end GNoT2860; "
    "public class NG2860 "
    "  public i: int := 41; "
    "end NG2860;";

// A library whose generic class carries a NON-constant-foldable instance field
// initializer — the §2.8.2 "cannot carry this shape" case (T7/T8).
const char* kLibOwned2860 =
    "module LibOwn2860; "
    "public class Cell2860 "
    "  public tag: int := 3; "
    "end Cell2860; "
    "public class GOwn2860<T> "
    "  public v: T; "
    "  public owned cell: Cell2860 := new Cell2860(); "
    "  public tail: int := 55; "
    "end GOwn2860; "
    "public class Plain2860 "
    "  public tail: int := 66; "
    "end Plain2860;";

// Build the dependency object once per test; returns false if the library did
// not compile (the caller then skips, matching the #2840 test's discipline).
bool buildLib2860(const std::string& stem, const char* libSrc,
                  std::string& objOut) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string src = (dir / (stem + "_a.bzl")).string();
    objOut = (dir / (stem + "_a.o")).string();
    writeSource2860(src, libSrc);
    bool ok = compileToObj2860(src, objOut);
    std::remove(src.c_str());
    return ok && std::ifstream(objOut).good();
}

}  // namespace

// ===========================================================================
// T1 — the headline cell: a DIRECT `new ImportedGeneric<string>()`. Every field
// must read its declared initial value. Without the fix nothing ran §22.8 step 2
// at all: `i` read 0 and the `string` field was null, so the program SEGV'd
// (reported as -1) rather than returning 0. §2.8.2 clause 2 / §22.8 step 2.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_field_init_2860,
           t1_direct_new_runs_field_initializers_2_8_2) {
    namespace fs = std::filesystem;
    std::string stem = "bz2860_t1_" + std::to_string(std::rand());
    std::string aObj;
    BEZEL_EXPECT_TRUE(buildLib2860(stem, kLib2860, aObj));
    TempFile2860 aObjFile(aObj);
    if (!std::ifstream(aObj).good()) return;

    std::string bSrc =
        "module App2860T1; import Lib2860; "
        "public class C "
        "  public static method Main(): int "
        "    using var g := new GInit2860<string>(); "
        "    var bad := 0; "
        "    if g.i <> 41 then bad := bad + 1; end if; "
        "    if g.l <> 1234567890123L then bad := bad + 2; end if; "
        "    if g.s <> \"hi\" then bad := bad + 4; end if; "
        "    if g.b <> true then bad := bad + 8; end if; "
        "    return bad; "
        "  end Main; "
        "end C;";
    auto dir = fs::temp_directory_path();
    TempFile2860 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2860 bExe((dir / (stem + "_b")).string());
    writeSource2860(bSrcFile.path, bSrc);
    BEZEL_EXPECT_EQ(0, compileImportAndRun2860(bSrcFile.path, bExe.path, aObj));
}

// ===========================================================================
// T2 — defect (b): a NON-GENERIC class deriving from the imported generic. The
// base's own field initializers run through emitImportedBaseFieldInit, which
// had no substitution to lay the base out with, so every store landed at the
// T→i32 placeholder offset. Without the fix `i` reads 0 and the string field is
// null (SEGV → -1). §2.8.2 clause 2 / §14.1 / §22.8 step 2.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_field_init_2860,
           t2_non_generic_derived_of_generic_base_14_1) {
    namespace fs = std::filesystem;
    std::string stem = "bz2860_t2_" + std::to_string(std::rand());
    std::string aObj;
    BEZEL_EXPECT_TRUE(buildLib2860(stem, kLib2860, aObj));
    TempFile2860 aObjFile(aObj);
    if (!std::ifstream(aObj).good()) return;

    std::string bSrc =
        "module App2860T2; import Lib2860; "
        "public class D2860 : GInit2860<string> "
        "  public own: int := 9; "
        "end D2860; "
        "public class C "
        "  public static method Main(): int "
        "    using var d := new D2860(); "
        "    var bad := 0; "
        "    if d.i <> 41 then bad := bad + 1; end if; "
        "    if d.l <> 1234567890123L then bad := bad + 2; end if; "
        "    if d.s <> \"hi\" then bad := bad + 4; end if; "
        "    if d.own <> 9 then bad := bad + 8; end if; "
        "    return bad; "
        "  end Main; "
        "end C;";
    auto dir = fs::temp_directory_path();
    TempFile2860 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2860 bExe((dir / (stem + "_b")).string());
    writeSource2860(bSrcFile.path, bSrc);
    BEZEL_EXPECT_EQ(0, compileImportAndRun2860(bSrcFile.path, bExe.path, aObj));
}

// ===========================================================================
// T3 — CONTROL: a GENERIC derived class (`D<T> : GInit2860<T>`), the shape
// #2524's own test pins. The active substitution already bound the base's
// parameter name, so `i` and `s` were CORRECT before this fix and must stay so.
// It is deliberately restricted to those two fields: the nullable field is a
// hinge on this path too (T4), so including it here would make this cell a
// second hinge rather than the honest non-hinge control it is meant to be.
// §2.8.2 clause 2 / §14.1.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_field_init_2860,
           t3_generic_derived_control_unchanged_2524) {
    namespace fs = std::filesystem;
    std::string stem = "bz2860_t3_" + std::to_string(std::rand());
    std::string aObj;
    BEZEL_EXPECT_TRUE(buildLib2860(stem, kLib2860, aObj));
    TempFile2860 aObjFile(aObj);
    if (!std::ifstream(aObj).good()) return;

    std::string bSrc =
        "module App2860T3; import Lib2860; "
        "public class DG2860<T> : GInit2860<T> "
        "  public own: int := 9; "
        "end DG2860; "
        "public class C "
        "  public static method Main(): int "
        "    using var d := new DG2860<string>(); "
        "    var bad := 0; "
        "    if d.i <> 41 then bad := bad + 1; end if; "
        "    if d.s <> \"hi\" then bad := bad + 2; end if; "
        "    if d.own <> 9 then bad := bad + 4; end if; "
        "    return bad; "
        "  end Main; "
        "end C;";
    auto dir = fs::temp_directory_path();
    TempFile2860 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2860 bExe((dir / (stem + "_b")).string());
    writeSource2860(bSrcFile.path, bSrc);
    BEZEL_EXPECT_EQ(0, compileImportAndRun2860(bSrcFile.path, bExe.path, aObj));
}

// ===========================================================================
// T4 — defect (c): the §23.1 nullable-VALUE slot. `n: int? := 7` lowers to a
// tagged `{i8 hasValue; i32 value}`; the replay stored the bare i32 over it.
// Measured without the fix: the DIRECT receiver read null (nothing ran), the
// DERIVED one read PRESENT-and-0 — a field that lies about its own presence.
// Both must now read a present 7. §23.1 / §7.2 / §22.8 step 2.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_field_init_2860,
           t4_nullable_value_field_roundtrips_23_1) {
    namespace fs = std::filesystem;
    std::string stem = "bz2860_t4_" + std::to_string(std::rand());
    std::string aObj;
    BEZEL_EXPECT_TRUE(buildLib2860(stem, kLib2860, aObj));
    TempFile2860 aObjFile(aObj);
    if (!std::ifstream(aObj).good()) return;

    std::string bSrc =
        "module App2860T4; import Lib2860; "
        "public class DN2860 : GInit2860<string> "
        "end DN2860; "
        "public class C "
        "  public static method Main(): int "
        "    using var g := new GInit2860<string>(); "
        "    using var d := new DN2860(); "
        "    var bad := 0; "
        "    if g.n = null then bad := bad + 1; "
        "    else if g.n <> 7 then bad := bad + 2; end if; end if; "
        "    if d.n = null then bad := bad + 4; "
        "    else if d.n <> 7 then bad := bad + 8; end if; end if; "
        "    return bad; "
        "  end Main; "
        "end C;";
    auto dir = fs::temp_directory_path();
    TempFile2860 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2860 bExe((dir / (stem + "_b")).string());
    writeSource2860(bSrcFile.path, bSrc);
    BEZEL_EXPECT_EQ(0, compileImportAndRun2860(bSrcFile.path, bExe.path, aObj));
}

// ===========================================================================
// T5 — CONTROL: the NON-generic import (`NG2860`) and a class derived from it.
// Their initializers run inside the exported `bz$<Type>$init`, which this
// change does not touch; both must be correct BOTH ways. This is the cell that
// isolates "generic" as the discriminant. §2.5 / §22.8 step 2.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_field_init_2860,
           t5_non_generic_import_unchanged_2_5) {
    namespace fs = std::filesystem;
    std::string stem = "bz2860_t5_" + std::to_string(std::rand());
    std::string aObj;
    BEZEL_EXPECT_TRUE(buildLib2860(stem, kLib2860, aObj));
    TempFile2860 aObjFile(aObj);
    if (!std::ifstream(aObj).good()) return;

    std::string bSrc =
        "module App2860T5; import Lib2860; "
        "public class DNG2860 : NG2860 "
        "end DNG2860; "
        "public class C "
        "  public static method Main(): int "
        "    using var n := new NG2860(); "
        "    using var d := new DNG2860(); "
        "    var bad := 0; "
        "    if n.i <> 41 then bad := bad + 1; end if; "
        "    if d.i <> 41 then bad := bad + 2; end if; "
        "    return bad; "
        "  end Main; "
        "end C;";
    auto dir = fs::temp_directory_path();
    TempFile2860 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2860 bExe((dir / (stem + "_b")).string());
    writeSource2860(bSrcFile.path, bSrc);
    BEZEL_EXPECT_EQ(0, compileImportAndRun2860(bSrcFile.path, bExe.path, aObj));
}

// ===========================================================================
// T6 — SINGLE-MODULE parity control. §2.8.2 clause 2 requires the external
// instantiation to behave identically to the same-module one; this is the
// same-module side of that equation, and it was already correct — so it is an
// honest control, not a hinge. Keeping it here is what makes T1/T2's numbers
// mean "the boundary lost them", not "the language never had them".
//
// The `string` field of the cross-module library is deliberately NOT repeated
// here: a SINGLE-MODULE generic class shaped `{ v: T; i: int; s: string }`
// SEGVs on disposal at `T = string` on this base, both with and without this
// change (measured on the parent commit; `{ v: T; s: string }` and
// `{ v: T; i: int }` are both fine, so it is the disposer's offset for the
// field AFTER a T-typed one). That is a separate, pre-existing single-module
// disposal defect, FILED rather than folded in — and a control must not be
// red for a reason it is not controlling for.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_field_init_2860,
           t6_single_module_parity_2_8_2) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2860_t6_" + std::to_string(std::rand());
    std::string src =
        "module App2860Single; "
        "public class GInit2860<T> "
        "  public v: T; "
        "  public i: int := 41; "
        "  public n: int? := 7; "
        "end GInit2860; "
        "public class C "
        "  public static method Main(): int "
        "    using var g := new GInit2860<string>(); "
        "    var bad := 0; "
        "    if g.i <> 41 then bad := bad + 1; end if; "
        "    if g.n = null then bad := bad + 4; "
        "    else if g.n <> 7 then bad := bad + 8; end if; end if; "
        "    return bad; "
        "  end Main; "
        "end C;";
    TempFile2860 sFile((dir / (stem + ".bzl")).string());
    TempFile2860 sExe((dir / (stem + "_exe")).string());
    writeSource2860(sFile.path, src);
    BEZEL_EXPECT_EQ(0, compileSingleAndRun2860(sFile.path, sExe.path));
}

// ===========================================================================
// T7 — §2.8.2 "No restrictions beyond visibility". `owned cell: Cell2860 :=
// new Cell2860()` pre-#2874 had no carriage in the artifact interface: nothing
// reached the consumer at all. The construction compiled and left the
// non-nullable owned field on the §7.9 null watermark — measured: `Cell.Live`
// stayed 0 inside the `using var` scope and the first `g.cell.tag` read SEGV'd.
// §2.8.2 requires a located compile error naming the field in that case.
// Post-#2874 the load-time parse-and-attach reconstructs `new Cell2860()` and
// RUNS it (Cell.Live is 1, `g.cell.tag` reads 3, ASan-clean). The diagnostic
// still fires for shapes the parse genuinely cannot reconstruct — an
// initializer that references an INTERNAL helper class (visible to the
// library's own compile but absent from the exported metadata, §13.2) fails
// the importer's resolveTypes and triggers the diagnostic. This row pins that
// the diagnostic is still a load-time-equivalent-fallback rather than gone:
// the same named-diagnostic, location, and §2.8.2 contract, just for the
// shapes that ACTUALLY fail to cross the boundary.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_field_init_2860,
           t7_uncarriable_initializer_is_a_named_diagnostic_2_8_2) {
    namespace fs = std::filesystem;
    std::string stem = "bz2860_t7_" + std::to_string(std::rand());
    std::string aObj;
    BEZEL_EXPECT_TRUE(buildLib2860(stem,
        "module LibOwn2860; "
        "public class Cell2860 public tag: int := 3; end Cell2860; "
        "internal class Internal2860 "
        "  public static method make(): owned Cell2860 => new Cell2860(); "
        "end Internal2860; "
        "public class GOwn2860<T> "
        "  public v: T; "
        "  public owned cell: Cell2860 := Internal2860.make(); "
        "  public tail: int := 55; "
        "end GOwn2860; "
        "public class Plain2860 public tail: int := 66; end Plain2860;",
        aObj));
    TempFile2860 aObjFile(aObj);
    if (!std::ifstream(aObj).good()) return;

    std::string bSrc =
        "module App2860T7; import LibOwn2860; "
        "public class C "
        "  public static method Main(): int "
        "    using var g := new GOwn2860<string>(); "
        "    return g.tail; "
        "  end Main; "
        "end C;";
    auto dir = fs::temp_directory_path();
    TempFile2860 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2860 bExe((dir / (stem + "_b")).string());
    writeSource2860(bSrcFile.path, bSrc);
    std::string err;
    bool ok = compileImportCaptureErr2860(bSrcFile.path, bExe.path, aObj, err);
    BEZEL_EXPECT_TRUE(!ok);
    BEZEL_EXPECT_TRUE(err.find("cannot run the initializer of its field") !=
                      std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("cell") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("GOwn2860") != std::string::npos);
}

// ===========================================================================
// T8 — CONTROL for T7's guard: the SAME library is imported, but the generic
// with the uncarriable initializer is never constructed. The diagnostic is
// scoped to the construction site precisely so this still builds and runs — a
// gate that rejected the whole import would break working programs, which is
// worse than the bug it fixes. Passes BOTH ways. §2.8.2 / §2.5.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_field_init_2860,
           t8_uncarriable_initializer_never_constructed_still_builds_2_5) {
    namespace fs = std::filesystem;
    std::string stem = "bz2860_t8_" + std::to_string(std::rand());
    std::string aObj;
    BEZEL_EXPECT_TRUE(buildLib2860(stem, kLibOwned2860, aObj));
    TempFile2860 aObjFile(aObj);
    if (!std::ifstream(aObj).good()) return;

    std::string bSrc =
        "module App2860T8; import LibOwn2860; "
        "public class C "
        "  public static method Main(): int "
        "    using var p := new Plain2860(); "
        "    if p.tail <> 66 then return 1; end if; "
        "    return 0; "
        "  end Main; "
        "end C;";
    auto dir = fs::temp_directory_path();
    TempFile2860 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2860 bExe((dir / (stem + "_b")).string());
    writeSource2860(bSrcFile.path, bSrc);
    BEZEL_EXPECT_EQ(0, compileImportAndRun2860(bSrcFile.path, bExe.path, aObj));
}

// ===========================================================================
// T9 — SEVERAL instantiations of the same imported generic in one program, at a
// value, a wider value and a reference type argument. Each monomorphization
// gets its own layout, so this pins that the replay is per-instantiation rather
// than accidentally sharing one struct. Without the fix every field read its
// §7.9 default (and the `<string>` cell SEGV'd). §2.8.2 clause 2 / §14.1.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_field_init_2860,
           t9_several_instantiations_each_initialized_14_1) {
    namespace fs = std::filesystem;
    std::string stem = "bz2860_t9_" + std::to_string(std::rand());
    std::string aObj;
    BEZEL_EXPECT_TRUE(buildLib2860(stem, kLib2860, aObj));
    TempFile2860 aObjFile(aObj);
    if (!std::ifstream(aObj).good()) return;

    std::string bSrc =
        "module App2860T9; import Lib2860; "
        "public class C "
        "  public static method Main(): int "
        "    using var a := new GInit2860<int>(); "
        "    using var b := new GInit2860<long>(); "
        "    using var c := new GInit2860<string>(); "
        "    using var d := new GNoT2860<string>(); "
        "    var bad := 0; "
        "    if a.i <> 41 then bad := bad + 1; end if; "
        "    if b.i <> 41 then bad := bad + 2; end if; "
        "    if c.i <> 41 then bad := bad + 4; end if; "
        "    if d.i <> 41 then bad := bad + 8; end if; "
        "    if a.s <> \"hi\" then bad := bad + 16; end if; "
        "    if c.l <> 1234567890123L then bad := bad + 32; end if; "
        "    return bad; "
        "  end Main; "
        "end C;";
    auto dir = fs::temp_directory_path();
    TempFile2860 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2860 bExe((dir / (stem + "_b")).string());
    writeSource2860(bSrcFile.path, bSrc);
    BEZEL_EXPECT_EQ(0, compileImportAndRun2860(bSrcFile.path, bExe.path, aObj));
}

}  // namespace bezel::module
