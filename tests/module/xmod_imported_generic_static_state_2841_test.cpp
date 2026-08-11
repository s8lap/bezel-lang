// fix #2841 — an imported generic class's static method whose body could NOT be
// reconstructed importer-side read and wrote a static cell belonging to NO
// instantiation, silently.
// (§22.12 Static members, §2.8.3 rule 4, §2.8.2 rule 2, §14.1, v0.240.)
//
// The governing sentences, verbatim:
//
//   §22.12: "Each distinct instantiation has its own static state."
//   §2.8.3 rule 4: a type — "in particular a constructed generic type, whose
//     monomorphized code may be emitted by several consumers' compilations — is
//     one type in the running process: exactly one copy of its static state and
//     one recorded initialization".
//   §2.8.2 rule 2: "An external instantiation behaves identically to a
//     same-module instantiation of the same generic at the same arguments."
//
// The importer honours that: staticFieldGlobal redirects every static-field
// access on a generic class to a PER-INSTANTIATION global (`bz$Box$string$_c`,
// `bz$Box$int$_c`). The EXPORTER's compiled `bz$Box$Bump`, emitted once against
// the generic placeholder, references the placeholder-level `bz$Box$_c` — a
// cell belonging to no instantiation. When #783 body reconstruction declines
// (the body reaches a sibling type or a module function, no body text was
// exported, or the re-parse failed) the call lands on that one function.
//
// MEASURED on real two-module builds:
//   * two instantiations SHARE — `Box<string>.Bump()` then `Box<int>.Bump()`
//     printed `a=1 b=2`; the identical declarations compiled as ONE module
//     print `a=1 b=1`.
//   * ONE instantiation is SPLIT IN TWO — `Box<int>` alone, `Bump()` then a
//     direct read of `Box<int>._count`, printed `a=1 direct=0`. This is why
//     there is no layout-identity escape hatch on the static side the way
//     #2821 has one on the instance side: static-cell identity is keyed on the
//     mangled instantiation name, not on the lowered layout, so `<int>` — where
//     the instantiation IS the placeholder layout — diverges too.
//
// The fix is the #783 gate the design note promises ("keeps the clean gate —
// never a wrong value"), stated as the property that matters: reject the call
// when the class (or a GENERIC class ancestor) declares per-instantiation
// static state the invisible body could touch. Importer-side monomorphization
// (#428/#783 family) is the real fix; a located compile error is the honest
// interim.
//
// ⚠ VACUITY HAZARDS — read before editing any source below.
//  (1) `buildMethodStub`'s `=> 0` defect (#2820) ALSO suppresses reconstruction
//      whenever a sibling declares a reference return type. Every "must be
//      rejected" case below forces the decline the one way immune to it: the
//      target method's OWN body names a SIBLING TYPE, which the single-class
//      synthetic source can never reproduce.
//  (2) The hinge is NOT "the compile fails" — without the fix these programs
//      COMPILE and RUN, returning wrong values. The assertions are `!success`
//      AND the diagnostic text carrying `#2841` (a coarse rejects() cannot tell
//      a genuine rejection from a crash).
//  (3) The controls must be non-vacuous in the other direction: T3's body
//      touches ONLY its own members, so it genuinely reconstructs and the gate
//      must not fire; verified by it RUNNING and returning the right value.
//
// TEST MATRIX (every case a real two-module build unless named single-module):
//   T1  two instantiations, unreconstructible body, mutable static: REJECTED.
//   T2  ONE instantiation + a direct read of the same static: REJECTED. This is
//       the cell-splitting case, which no layout test could catch.
//   T3  CONTROL — the same shape whose body touches only its own members, so
//       #783 reconstruction SUCCEEDS: runs, `a=1 b=1` (returns 11).
//   T4  CONTROL — a generic class with NO static state (#2821's T5 shape):
//       runs, returns 7. A blanket static gate would falsely reject this.
//   T5  CONTROL — a NON-generic imported class with a mutable static and an
//       unreconstructible body: one cell program-wide, runs, returns 12.
//   T6  CONTROL — the only static is `readonly` of pure-bits type: the two
//       cells hold equal values, runs, returns 55.
//   T7  CONTROL — the static is owned by a NON-generic base of the generic
//       class, so it is not per-instantiation: runs, returns 7.
//   T8  CONTROL — direct per-instantiation static field access with no call at
//       all: unaffected and already correct, returns 87.
//   T9  a `readonly` static of CLASS type: the two cells hold two distinct
//       REFERENTS (measured: an importer-side write through `Ro<string>.Inst.n`
//       invisible to the exporter-compiled getter), so REJECTED.
//   T10 SINGLE-MODULE control of T1/T2: §2.8.2 rule 2's other side — the same
//       declarations in one module give 1, 1, and 1 (returns 111).

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "driver/Driver.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

struct TempFile2841 {
    std::string path;
    explicit TempFile2841(std::string p) : path(std::move(p)) {}
    ~TempFile2841() { std::remove(path.c_str()); }
};

void writeSource2841(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj2841(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

int compileImportAndRun2841(const std::string& bSrc, const std::string& bExe,
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

int compileSingleAndRun2841(const std::string& src, const std::string& exe) {
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    if (!driver.compileFile(src, exe, deps).success) return -1;
    int status = std::system(exe.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// Compile B importing A, capturing the driver's stderr (fd 2) into `err`, so a
// rejection can be asserted on its TEXT and not merely on failure.
bool compileImportCaptureErr2841(const std::string& bSrc,
                                 const std::string& bExe,
                                 const std::string& aObj, std::string& err) {
    namespace fs = std::filesystem;
    std::string errPath =
        (fs::temp_directory_path() /
         ("bz2841_err_" + std::to_string(std::rand()) + ".txt"))
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

// The library. Every "must reject" class's static body names the SIBLING TYPE
// `Helper2841`, the reconstruction-decline route immune to #2820.
const char* kLib2841 =
    "module Lib2841; "
    "public class Helper2841 "
    "  public static method Zero(): int "
    "    return 0; "
    "  end Zero; "
    "  public static method Seven(): int "
    "    return 7; "
    "  end Seven; "
    "end Helper2841; "
    "public class Box2841<T> "
    "  public static _count: int := 0; "
    "  public static method Bump(): int "
    "    _count := _count + 1; "
    "    return _count + Helper2841.Zero(); "
    "  end Bump; "
    "end Box2841; "
    "public class Recon2841<T> "
    "  public static _count: int := 0; "
    "  public static method Bump(): int "
    "    _count := _count + 1; "
    "    return _count; "
    "  end Bump; "
    "end Recon2841; "
    "public class NoStatic2841<T> "
    "  public v: T; "
    "  public static method Make(): int "
    "    return Helper2841.Seven(); "
    "  end Make; "
    "end NoStatic2841; "
    "public class Plain2841 "
    "  public static _count: int := 0; "
    "  public static method Bump(): int "
    "    _count := _count + 1; "
    "    return _count + Helper2841.Zero(); "
    "  end Bump; "
    "end Plain2841; "
    "public class Ro2841<T> "
    "  public static readonly Base: int := 5; "
    "  public static method Get(): int "
    "    return Base + Helper2841.Zero(); "
    "  end Get; "
    "end Ro2841; "
    "public class StatOwner2841 "
    "  public static _n: int := 0; "
    "  public static method Inc(): int "
    "    _n := _n + 1; "
    "    return _n; "
    "  end Inc; "
    "end StatOwner2841; "
    "public class GenChild2841<T> : StatOwner2841 "
    "  public static method Go(): int "
    "    return Helper2841.Seven(); "
    "  end Go; "
    "end GenChild2841; "
    "public class Dir2841<T> "
    "  public static _c: int := 3; "
    "end Dir2841; "
    "public class Cell2841 "
    "  public n: int := 41; "
    "end Cell2841; "
    "public class RoRef2841<T> "
    "  public static readonly Inst: Cell2841 := new Cell2841(); "
    "  public static method Get(): int "
    "    return Inst.n + Helper2841.Zero(); "
    "  end Get; "
    "end RoRef2841;";

// Build the library once per test (each test is process-isolated).
bool buildLib2841(const std::string& stem, std::string& objPathOut) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    static thread_local std::vector<std::shared_ptr<TempFile2841>> keep;
    auto aSrc = std::make_shared<TempFile2841>(
        (dir / (stem + "_a.bzl")).string());
    auto aObj = std::make_shared<TempFile2841>(
        (dir / (stem + "_a.o")).string());
    keep.push_back(aSrc);
    keep.push_back(aObj);
    writeSource2841(aSrc->path, kLib2841);
    if (!compileToObj2841(aSrc->path, aObj->path)) return false;
    if (!std::ifstream(aObj->path).good()) return false;
    objPathOut = aObj->path;
    return true;
}

// Run one "must be REJECTED with #2841" case.
void expectRejected2841(const std::string& stem, const std::string& bSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string aObj;
    BEZEL_EXPECT_TRUE(buildLib2841(stem, aObj));
    if (aObj.empty()) return;
    TempFile2841 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2841 bExe((dir / (stem + "_b")).string());
    writeSource2841(bSrcFile.path, bSrc);
    std::string err;
    bool success =
        compileImportCaptureErr2841(bSrcFile.path, bExe.path, aObj, err);
    // Hinge part 1: without the fix this COMPILES (and then answers wrongly).
    BEZEL_EXPECT_TRUE(!success);
    // Hinge part 2: the rejection is THIS gate's, named.
    BEZEL_EXPECT_TRUE(err.find("#2841") != std::string::npos);
}

// Run one "must still COMPILE and answer `expect`" case.
void expectRuns2841(const std::string& stem, const std::string& bSrc,
                    int expect) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string aObj;
    BEZEL_EXPECT_TRUE(buildLib2841(stem, aObj));
    if (aObj.empty()) return;
    TempFile2841 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2841 bExe((dir / (stem + "_b")).string());
    writeSource2841(bSrcFile.path, bSrc);
    BEZEL_EXPECT_EQ(expect,
                    compileImportAndRun2841(bSrcFile.path, bExe.path, aObj));
}

}  // namespace

// ===========================================================================
// T1 — two instantiations of an imported generic class with an unreconstructed
// static body. WITHOUT the fix this compiles and prints `a=1 b=2`: both calls
// mutated the exporter's single placeholder cell, violating §22.12's "Each
// distinct instantiation has its own static state".
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_static_state_2841,
           t1_two_instantiations_sharing_one_cell_rejected_22_12) {
    expectRejected2841(
        "bz2841_t1_" + std::to_string(std::rand()),
        "module App2841T1; import Lib2841; "
        "public class C "
        "  public static method Main(): int "
        "    var a := Box2841<string>.Bump(); "
        "    var b := Box2841<int>.Bump(); "
        "    return a * 10 + b; "
        "  end Main; "
        "end C;");
}

// ===========================================================================
// T2 — ONE instantiation, split in two. `Box2841<int>.Bump()` mutates the
// exporter's placeholder cell while the importer's own `Box2841<int>._count`
// read goes to the per-instantiation cell: WITHOUT the fix this compiles and
// prints `a=1 direct=0`. `<int>` is exactly the case an instance-layout test
// would call sound, which is why the static gate is storage-valued, not
// layout-valued. §22.12 / §2.8.3 rule 4.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_static_state_2841,
           t2_single_instantiation_split_cell_rejected_2_8_3) {
    expectRejected2841(
        "bz2841_t2_" + std::to_string(std::rand()),
        "module App2841T2; import Lib2841; "
        "public class C "
        "  public static method Main(): int "
        "    var a := Box2841<int>.Bump(); "
        "    var direct := Box2841<int>._count; "
        "    return a * 10 + direct; "
        "  end Main; "
        "end C;");
}

// ===========================================================================
// T3 — CONTROL: the identical shape whose static body touches ONLY its own
// members, so #783 reconstruction SUCCEEDS and the importer monomorphizes it
// per instantiation. The gate must not fire, and the answer must be right:
// `a=1 b=1` → 11. This is what makes T1's rejection a statement about the
// FALLBACK and not about generic statics in general. §14.1 / §22.12.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_static_state_2841,
           t3_reconstructible_body_still_runs_per_instantiation_14_1) {
    expectRuns2841(
        "bz2841_t3_" + std::to_string(std::rand()),
        "module App2841T3; import Lib2841; "
        "public class C "
        "  public static method Main(): int "
        "    var a := Recon2841<string>.Bump(); "
        "    var b := Recon2841<int>.Bump(); "
        "    return a * 10 + b; "
        "  end Main; "
        "end C;",
        11);
}

// ===========================================================================
// T4 — CONTROL: a generic class with NO static state at all (#2821's T5 shape).
// A blanket "reject every unreconstructed static call" gate would falsely
// reject this working program. §22.12.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_static_state_2841,
           t4_generic_without_static_state_not_rejected_22_12) {
    expectRuns2841(
        "bz2841_t4_" + std::to_string(std::rand()),
        "module App2841T4; import Lib2841; "
        "public class C "
        "  public static method Main(): int "
        "    return NoStatic2841<string>.Make(); "
        "  end Main; "
        "end C;",
        7);
}

// ===========================================================================
// T5 — CONTROL: a NON-generic imported class with a mutable static and the same
// unreconstructible body. §22.12's per-instantiation rule has nothing to say
// here: there is one cell program-wide and the exporter's reference is that
// cell. `a=1 b=2` → 12, correct and unchanged.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_static_state_2841,
           t5_non_generic_imported_static_unchanged_22_12) {
    expectRuns2841(
        "bz2841_t5_" + std::to_string(std::rand()),
        "module App2841T5; import Lib2841; "
        "public class C "
        "  public static method Main(): int "
        "    var a := Plain2841.Bump(); "
        "    var b := Plain2841.Bump(); "
        "    return a * 10 + b; "
        "  end Main; "
        "end C;",
        12);
}

// ===========================================================================
// T6 — CONTROL: the class's only static is `readonly` of pure-bits type. The
// two cells are independently initialized but hold equal values for any
// T-independent initializer, so this compiles and answers 5 at both
// instantiations (55). A T-DEPENDENT initializer does diverge — that is
// #2295/#803's per-instantiation-sfinit miscompile, which reproduces
// single-module too, and is deliberately not this gate's business. §22.12
// rule 3.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_static_state_2841,
           t6_readonly_value_static_not_rejected_22_12_rule_3) {
    expectRuns2841(
        "bz2841_t6_" + std::to_string(std::rand()),
        "module App2841T6; import Lib2841; "
        "public class C "
        "  public static method Main(): int "
        "    var a := Ro2841<string>.Get(); "
        "    var b := Ro2841<int>.Get(); "
        "    return a * 10 + b; "
        "  end Main; "
        "end C;",
        55);
}

// ===========================================================================
// T7 — CONTROL: the generic class's static is inherited from a NON-generic
// base, so staticFieldGlobal never mangles it per instantiation — one cell,
// and the exporter's reference is that cell. Returns 7. §22.12.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_static_state_2841,
           t7_static_owned_by_non_generic_base_not_rejected_22_12) {
    expectRuns2841(
        "bz2841_t7_" + std::to_string(std::rand()),
        "module App2841T7; import Lib2841; "
        "public class C "
        "  public static method Main(): int "
        "    return GenChild2841<string>.Go(); "
        "  end Main; "
        "end C;",
        7);
}

// ===========================================================================
// T8 — CONTROL: direct per-instantiation static field access with no method
// call at all. This path is already correct (it is the side of the divergence
// that is RIGHT) and the gate must not touch it: 3, then 4 after a write, and a
// sibling instantiation still 3 → 87. §22.12 / §2.8.3 rule 4.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_static_state_2841,
           t8_direct_static_field_access_unaffected_22_12) {
    expectRuns2841(
        "bz2841_t8_" + std::to_string(std::rand()),
        "module App2841T8; import Lib2841; "
        "public class C "
        "  public static method Main(): int "
        "    var a := Dir2841<string>._c; "
        "    Dir2841<string>._c := Dir2841<string>._c + 1; "
        "    var b := Dir2841<string>._c; "
        "    var c := Dir2841<int>._c; "
        "    return a * 100 + b * 10 + c; "
        "  end Main; "
        "end C;",
        87);
}

// ===========================================================================
// T9 — a `readonly` static of CLASS type is NOT exempt: the two cells hold two
// distinct REFERENTS, not two equal values. Measured without the fix: an
// importer-side `RoRef2841<string>.Inst.n := 7` was invisible to the
// exporter-compiled `Get()`, which kept returning 41. §22.12 / §2.8.3 rule 4.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_static_state_2841,
           t9_readonly_reference_static_rejected_2_8_3) {
    expectRejected2841(
        "bz2841_t9_" + std::to_string(std::rand()),
        "module App2841T9; import Lib2841; "
        "public class C "
        "  public static method Main(): int "
        "    var a := RoRef2841<string>.Get(); "
        "    RoRef2841<string>.Inst.n := 7; "
        "    var b := RoRef2841<string>.Get(); "
        "    return a + b; "
        "  end Main; "
        "end C;");
}

// ===========================================================================
// T10 — SINGLE-MODULE control of T1/T2. §2.8.2 rule 2 makes the external
// instantiation's behaviour the same-module instantiation's behaviour, and this
// is that same-module side: two instantiations each get their own cell, and a
// direct read agrees with the method's own view — 1, 1, 1 → 111. It is the
// oracle T1/T2 are measured against. §22.12 / §2.8.2 rule 2.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_static_state_2841,
           t10_single_module_per_instantiation_oracle_22_12) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2841_t10_" + std::to_string(std::rand());
    std::string src =
        "module App2841Single; "
        "public class Helper2841 "
        "  public static method Zero(): int "
        "    return 0; "
        "  end Zero; "
        "end Helper2841; "
        "public class Box2841<T> "
        "  public static _count: int := 0; "
        "  public static method Bump(): int "
        "    _count := _count + 1; "
        "    return _count + Helper2841.Zero(); "
        "  end Bump; "
        "end Box2841; "
        "public class C "
        "  public static method Main(): int "
        "    var a := Box2841<string>.Bump(); "
        "    var b := Box2841<int>.Bump(); "
        "    var direct := Box2841<int>._count; "
        "    return a * 100 + b * 10 + direct; "
        "  end Main; "
        "end C;";
    TempFile2841 sFile((dir / (stem + ".bzl")).string());
    TempFile2841 sExe((dir / (stem + "_exe")).string());
    writeSource2841(sFile.path, src);
    BEZEL_EXPECT_EQ(111, compileSingleAndRun2841(sFile.path, sExe.path));
}

}  // namespace bezel::module
