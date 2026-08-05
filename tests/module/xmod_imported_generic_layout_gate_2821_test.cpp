// fix #2821 — the §14.1/#783 gate missed a method that reads a NON-`T` field at
// a `T`-SHIFTED offset, so an unattached imported generic body fell back to the
// exporter's PLACEHOLDER-compiled function and returned garbage, silently.
// (§14.1 Generic declarations, §2.5 Dependency resolution, §2.8.3, v0.240.)
//
// §14.1 exports a generic class's method body source (schema v5 methodBodyText,
// #783) so the IMPORTER can monomorphize it per instantiation. When that
// reconstruction does NOT happen — a body that reaches a sibling type or a
// module function, no exported body text, a failed re-parse — the call falls
// back to the EXPORTER's compiled function, which was emitted against the
// generic PLACEHOLDER layout (every type parameter lowered to i32).
//
// The pre-existing sema gate that is supposed to keep that fallback from being
// observed (ExprTypecheck_Calls.cpp) fires only when the METHOD'S RETURN TYPE
// substitutes to a REFERENCE type. That is a syntactic marker on the signature,
// not the property that matters. A method can read a plain, non-`T` field whose
// OFFSET is `T`-dependent, because a `T`-typed field declared earlier in the
// class has a different size under the placeholder (i32) than under the
// instantiation (ptr, or i64 for `long`). Such a method never mentions `T`, so
// the gate never fired:
//
//   public class Box<T>
//     public v: T;      // T-typed field FIRST
//     public tag: int;  // plain int field AFTER it
//     public method GetTag(): int  return this.tag + Helper.Zero(); end GetTag;
//   end Box;
//
//   Box<string>: GetTag() returned 4610688, not 7. No diagnostic at any
//   severity, from ordinary code on a documented supported surface.
//
// The fix gates on the property actually needed, measured on the REAL lowered
// layout (LlvmBackend_Calls2.cpp, emitMethodCall): for an INSTANCE call to an
// imported generic class's method with no reconstructed body, compare each
// instance-storage slot's LLVM type under the active substitution with its type
// under the EMPTY (placeholder) one. Identical ⇒ the exporter's function reads
// the very offsets this object has ⇒ sound, keep compiling. Different ⇒ reject
// with a located §14.1/#783/#2821 diagnostic — the honest fallback #783's design
// note promises ("never a wrong value").
//
// ⚠ VACUITY HAZARD — read before editing any source below. There are TWO ways to
// make this test pass for the wrong reason:
//  (1) `buildMethodStub`'s `=> 0` defect (#2820) ALSO suppresses reconstruction,
//      for the whole class, whenever any sibling declares a reference (or
//      narrow/unsigned integer) return type. A test that relied on such a
//      sibling to reach the fallback would stop exercising this gate the moment
//      #2820 is fixed. So T1/T4 force the fallback the ONLY way that is immune
//      to #2820: the target method's OWN body names a SIBLING TYPE, which the
//      single-class synthetic source can never reproduce.
//  (2) The hinge is NOT "the compile fails". Without the fix these programs
//      COMPILE and RUN — returning a wrong value. The assertions below are
//      therefore `!success` AND the diagnostic text carrying `#2821`.
//
// TEST MATRIX:
//   T1  the issue's shape (T-field first, plain-int method, sibling-type body):
//       REJECTED with #2821. Without the fix: compiles, runs, returns garbage.
//   T2  the SAME class at `<int>`: the instantiation IS the placeholder, so it
//       must still compile and return 7 (no false reject).
//   T3  a generic class with NO type-parameter-typed storage at `<string>`: the
//       layout cannot shift, so it must still compile and return 7.
//   T4  a WIDER VALUE type argument (`long`): rejected. The old
//       reference-type-only gate never fired for a value argument — this pins
//       that the new gate is layout-valued, not reference-valued.
//   T5  a STATIC method on the T-shifted class: still compiles and returns 7.
//       A static call is handed no object, so the instance layout is not what
//       its compiled body reads; the gate is scoped to instance calls.
//   T6  CONTROL: the same T-shifted class whose body IS reconstructible (it
//       touches only its own members): compiles and returns 7 at `<string>` —
//       the gate must not fire when #783 monomorphization succeeded.
//   T7  CONTROL: a NON-generic imported class with a reference field and an
//       unreconstructible body: entirely outside the gate; returns 7.

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

struct TempFile2821 {
    std::string path;
    explicit TempFile2821(std::string p) : path(std::move(p)) {}
    ~TempFile2821() { std::remove(path.c_str()); }
};

void writeSource2821(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj2821(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// Compile B importing A → exe, run it, return the exit code (-1 on any
// compile/link/run failure). Used by the must-still-work controls.
int compileImportAndRun2821(const std::string& bSrc, const std::string& bExe,
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

// Compile B importing A, capturing the driver's stderr (fd 2) into `err`.
// Returns the compile's success flag. fd 2 is redirected so both the driver's
// std::cerr diagnostics and any llvm::errs() text are captured.
bool compileImportCaptureErr2821(const std::string& bSrc,
                                 const std::string& bExe,
                                 const std::string& aObj, std::string& err) {
    namespace fs = std::filesystem;
    std::string errPath =
        (fs::temp_directory_path() /
         ("bz2821_err_" + std::to_string(std::rand()) + ".txt"))
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

// The T-SHIFTED library: `v: T` is declared BEFORE `tag: int`, so `tag`'s offset
// under a reference (or `long`) instantiation is not the offset the exporter's
// placeholder-compiled body computes. `GetTag`'s body names the SIBLING TYPE
// `Helper2821`, which the single-class synthetic source cannot reproduce, so
// #783 reconstruction genuinely declines — independently of the #2820 stub
// defect. `Make` is the static counterpart used by T5.
const char* kLibShift2821 =
    "module Lib2821; "
    "public class Helper2821 "
    "  public static method Zero(): int "
    "    return 0; "
    "  end Zero; "
    "  public static method Seven(): int "
    "    return 7; "
    "  end Seven; "
    "end Helper2821; "
    "public class Box2821<T> "
    "  public v: T; "
    "  public tag: int; "
    "  public method GetTag(): int "
    "    return this.tag + Helper2821.Zero(); "
    "  end GetTag; "
    "  public static method Make(): int "
    "    return Helper2821.Seven(); "
    "  end Make; "
    "end Box2821;";

}  // namespace

// ===========================================================================
// T1 — the issue's shape. `Box2821<string>.GetTag()` is REJECTED with #2821.
// WITHOUT the fix this compiles, runs, and returns garbage (4610688 & 0xff).
// §14.1 / §2.5.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_layout_gate_2821,
           t1_shifted_offset_reference_arg_rejected_14_1) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2821_t1_" + std::to_string(std::rand());
    TempFile2821 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2821 aObj((dir / (stem + "_a.o")).string());
    writeSource2821(aSrc.path, kLibShift2821);
    BEZEL_EXPECT_TRUE(compileToObj2821(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    std::string bSrc =
        "module App2821T1; import Lib2821; "
        "public class C "
        "  public static method Main(): int "
        "    using var b := new Box2821<string>(); "
        "    b.v := \"hello\"; "
        "    b.tag := 7; "
        "    return b.GetTag(); "
        "  end Main; "
        "end C;";
    TempFile2821 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2821 bExe((dir / (stem + "_b")).string());
    writeSource2821(bSrcFile.path, bSrc);

    std::string err;
    bool success =
        compileImportCaptureErr2821(bSrcFile.path, bExe.path, aObj.path, err);
    // Hinge part 1: without the fix this COMPILES (and then returns garbage).
    BEZEL_EXPECT_TRUE(!success);
    // Hinge part 2: the rejection is this gate's, named — not some unrelated
    // failure (protocol: a coarse "it failed" cannot tell a reject from a
    // crash).
    BEZEL_EXPECT_TRUE(err.find("#2821") != std::string::npos);
}

// ===========================================================================
// T2 — the SAME class at `<int>`: the instantiation IS the i32 placeholder, so
// the exporter's compiled body reads the very offsets this object has. Must
// still compile and return 7. §14.1.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_layout_gate_2821,
           t2_int_instantiation_still_compiles_and_runs_14_1) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2821_t2_" + std::to_string(std::rand());
    TempFile2821 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2821 aObj((dir / (stem + "_a.o")).string());
    writeSource2821(aSrc.path, kLibShift2821);
    BEZEL_EXPECT_TRUE(compileToObj2821(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    std::string bSrc =
        "module App2821T2; import Lib2821; "
        "public class C "
        "  public static method Main(): int "
        "    using var b := new Box2821<int>(); "
        "    b.v := 3; "
        "    b.tag := 7; "
        "    return b.GetTag(); "
        "  end Main; "
        "end C;";
    TempFile2821 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2821 bExe((dir / (stem + "_b")).string());
    writeSource2821(bSrcFile.path, bSrc);
    BEZEL_EXPECT_EQ(7, compileImportAndRun2821(bSrcFile.path, bExe.path,
                                               aObj.path));
}

// ===========================================================================
// T3 — a generic class with NO type-parameter-typed storage: its layout is the
// same for every instantiation, so `<string>` must still compile and return 7.
// This is the shape a class-wide "any reference instantiation" gate would have
// falsely rejected. §14.1 / §2.5.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_layout_gate_2821,
           t3_no_type_param_storage_not_rejected_14_1) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2821_t3_" + std::to_string(std::rand());
    TempFile2821 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2821 aObj((dir / (stem + "_a.o")).string());
    writeSource2821(aSrc.path,
        "module Lib2821F; "
        "public class Helper2821F "
        "  public static method Zero(): int "
        "    return 0; "
        "  end Zero; "
        "end Helper2821F; "
        "public class Flat2821<T> "
        "  public tag: int; "
        "  public method GetTag(): int "
        "    return this.tag + Helper2821F.Zero(); "
        "  end GetTag; "
        "end Flat2821;");
    BEZEL_EXPECT_TRUE(compileToObj2821(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    std::string bSrc =
        "module App2821T3; import Lib2821F; "
        "public class C "
        "  public static method Main(): int "
        "    using var b := new Flat2821<string>(); "
        "    b.tag := 7; "
        "    return b.GetTag(); "
        "  end Main; "
        "end C;";
    TempFile2821 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2821 bExe((dir / (stem + "_b")).string());
    writeSource2821(bSrcFile.path, bSrc);
    BEZEL_EXPECT_EQ(7, compileImportAndRun2821(bSrcFile.path, bExe.path,
                                               aObj.path));
}

// ===========================================================================
// T4 — a WIDER VALUE type argument (`long`) shifts the layout exactly as a
// reference argument does, and the old reference-type-only gate never fired for
// it. Rejected with #2821 — the gate is layout-valued, not reference-valued.
// WITHOUT the fix this compiles and returns 5, not 7. §14.1 / §2.8.3.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_layout_gate_2821,
           t4_wider_value_type_argument_rejected_14_1) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2821_t4_" + std::to_string(std::rand());
    TempFile2821 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2821 aObj((dir / (stem + "_a.o")).string());
    writeSource2821(aSrc.path, kLibShift2821);
    BEZEL_EXPECT_TRUE(compileToObj2821(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    std::string bSrc =
        "module App2821T4; import Lib2821; "
        "public class C "
        "  public static method Main(): int "
        "    using var b := new Box2821<long>(); "
        "    b.v := 5; "
        "    b.tag := 7; "
        "    return b.GetTag(); "
        "  end Main; "
        "end C;";
    TempFile2821 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2821 bExe((dir / (stem + "_b")).string());
    writeSource2821(bSrcFile.path, bSrc);

    std::string err;
    bool success =
        compileImportCaptureErr2821(bSrcFile.path, bExe.path, aObj.path, err);
    BEZEL_EXPECT_TRUE(!success);
    BEZEL_EXPECT_TRUE(err.find("#2821") != std::string::npos);
}

// ===========================================================================
// T5 — a STATIC method on the very same T-shifted class, with the very same
// unreconstructible body shape: still compiles and returns 7. A static call is
// handed no object, so the instance layout is not what its compiled body reads.
// §14.1 / §22.12.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_layout_gate_2821,
           t5_static_method_not_rejected_14_1) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2821_t5_" + std::to_string(std::rand());
    TempFile2821 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2821 aObj((dir / (stem + "_a.o")).string());
    writeSource2821(aSrc.path, kLibShift2821);
    BEZEL_EXPECT_TRUE(compileToObj2821(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    std::string bSrc =
        "module App2821T5; import Lib2821; "
        "public class C "
        "  public static method Main(): int "
        "    return Box2821<string>.Make(); "
        "  end Main; "
        "end C;";
    TempFile2821 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2821 bExe((dir / (stem + "_b")).string());
    writeSource2821(bSrcFile.path, bSrc);
    BEZEL_EXPECT_EQ(7, compileImportAndRun2821(bSrcFile.path, bExe.path,
                                               aObj.path));
}

// ===========================================================================
// T6 — CONTROL: the identical T-shifted class whose `GetTag` body touches ONLY
// its own members, so #783 reconstruction succeeds and the body is monomorphized
// importer-side. `<string>` compiles and returns 7 — the gate must never fire on
// a reconstructed body. §14.1.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_layout_gate_2821,
           t6_reconstructible_body_still_monomorphizes_14_1) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2821_t6_" + std::to_string(std::rand());
    TempFile2821 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2821 aObj((dir / (stem + "_a.o")).string());
    writeSource2821(aSrc.path,
        "module Lib2821R; "
        "public class Box2821R<T> "
        "  public v: T; "
        "  public tag: int; "
        "  public method GetTag(): int "
        "    return this.tag; "
        "  end GetTag; "
        "end Box2821R;");
    BEZEL_EXPECT_TRUE(compileToObj2821(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    std::string bSrc =
        "module App2821T6; import Lib2821R; "
        "public class C "
        "  public static method Main(): int "
        "    using var b := new Box2821R<string>(); "
        "    b.v := \"hello\"; "
        "    b.tag := 7; "
        "    return b.GetTag(); "
        "  end Main; "
        "end C;";
    TempFile2821 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2821 bExe((dir / (stem + "_b")).string());
    writeSource2821(bSrcFile.path, bSrc);
    BEZEL_EXPECT_EQ(7, compileImportAndRun2821(bSrcFile.path, bExe.path,
                                               aObj.path));
}

// ===========================================================================
// T7 — CONTROL: a NON-generic imported class with a reference field and the very
// same unreconstructible body: no type parameters, no placeholder, nothing to
// gate. Compiles and returns 7. §2.5.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_layout_gate_2821,
           t7_non_generic_imported_class_untouched_2_5) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2821_t7_" + std::to_string(std::rand());
    TempFile2821 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2821 aObj((dir / (stem + "_a.o")).string());
    writeSource2821(aSrc.path,
        "module Lib2821P; "
        "public class Helper2821P "
        "  public static method Zero(): int "
        "    return 0; "
        "  end Zero; "
        "end Helper2821P; "
        "public class Plain2821 "
        "  public v: string?; "
        "  public tag: int; "
        "  public method GetTag(): int "
        "    return this.tag + Helper2821P.Zero(); "
        "  end GetTag; "
        "end Plain2821;");
    BEZEL_EXPECT_TRUE(compileToObj2821(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    std::string bSrc =
        "module App2821T7; import Lib2821P; "
        "public class C "
        "  public static method Main(): int "
        "    using var b := new Plain2821(); "
        "    b.v := \"hello\"; "
        "    b.tag := 7; "
        "    return b.GetTag(); "
        "  end Main; "
        "end C;";
    TempFile2821 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2821 bExe((dir / (stem + "_b")).string());
    writeSource2821(bSrcFile.path, bSrc);
    BEZEL_EXPECT_EQ(7, compileImportAndRun2821(bSrcFile.path, bExe.path,
                                               aObj.path));
}

}  // namespace bezel::module
