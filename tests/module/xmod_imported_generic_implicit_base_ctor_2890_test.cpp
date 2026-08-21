// fix #2890 — an implicit base chain into an IMPORTED GENERIC base's
// parameterless constructor called the EXPORTER's placeholder-layout
// `bz$<Base>$ctor` against an object the importer allocated with the
// INSTANTIATION's layout. Every store in that body landed at the
// placeholder's offsets (an unresolved type parameter lowers to i32 —
// lowerTypeRef's value-kind fallback), the first owned-field initialization
// corrupted an adjacent field's header, and a SEGV followed with no
// diagnostic at any severity.
//
// (§2.5 Artifact resolution, §22.8 "Field initializers" step 1 "The base or
// chained constructor call", §14.1 "Generic declarations", §12 "Inheritance
// and Dispatch", §2.8.2, v0.240 — unchanged in v0.270 per Bezel_Changelog.)
//
// ROOT CAUSE: `emitSpecializedImplicitBaseCtor` (LlvmBackend_Setup.cpp ~11602)
// refuses to act unless it can inline the base ctor's body under the base
// instantiation's substitution. Its first obligation is that there is a real
// body to inline:
//
//   if (!dynamic_cast<const ast::ConstructorDecl*>(target->decl)) return false;

// An IMPORTED base ctor's `decl` is always null (#2374-class gap: the artifact
// interface ships no AST across the module boundary), so the helper declines
// and the original `declareImportedCtor + CreateCall` runs the EXPORTER's
// compiled body — emitted against `T -> i32` — on an object laid out for the
// instantiation. This is the IMPORTED sibling of #2882 (which closes the same
// gap for OWN generic bases by inlining the live body). #2882's gate keeps
// `T = int` and any base with no parameter-typed storage on the existing
// call with no behaviour change; #2890's gate reuses that exact layout-
// identity question for the imported case.
//
// THE FIX: when the imported base is a generic instantiation whose storage
// layout differs from the placeholder's, emit a LOCATED compile error (the
// exact fallback the #783 design note promises and the #2821 method-call
// gate already uses) instead of silently mis-executing. The accept-controls
// are the same as #2882's:
//
//   - `T = int` (instantiation IS the placeholder): the exporter's compiled
//     function reads exactly this object's offsets, so the existing call is
//     sound and must keep compiling.
//   - any imported generic base with no parameter-typed storage: the layout
//     cannot shift, same story.
//
// VACUITY HAZARD — read before editing any source below.
//
// (1) The hinge is NOT "the compile fails". Without the fix the issue's exact
//     repro COMPILES, RUNS, and SEGV's inside `d.cell.tag`. The T1
//     assertion is therefore `!success` AND the diagnostic text carrying
//     `#2890`.
// (2) The T2/T3 assertions are POSITIVE: a T-shifting program that does not
//     reach this code path (T2: own generic base, T3: imported but no T-typed
//     storage) must STILL compile and run, with the asserted value as
//     proof. A test that "rejects everywhere" passes trivially.

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
#include "support/unique_stem.hpp"


namespace bezel::module {
namespace {

struct TempFile2890 {
    std::string path;
    explicit TempFile2890(std::string p) : path(std::move(p)) {}
    ~TempFile2890() { std::remove(path.c_str()); }
};

void writeSource2890(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj2890(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// Compile B importing A → exe, run it, return the exit code (-1 on any
// compile/link/run failure; -2 if the dependency itself could not be
// produced). Mirrors #2387/#2414/#2449/#2452/#2839's helpers.
int compileImportAndRun2890(const std::string& bSrc, const std::string& bExe,
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

// Compile B importing A, capturing stderr into `err`. Returns success flag.
// Mirrors #2821's `compileImportCaptureErr2821`.
bool compileImportCaptureErr2890(const std::string& bSrc,
                                 const std::string& bExe,
                                 const std::string& aObj, std::string& err) {
    namespace fs = std::filesystem;
    std::string errPath =
        (fs::temp_directory_path() /
         ("bz2890_err_" + unique_stem("") + ".txt"))
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

// The shared library, verbatim from the issue's repro (file:line in #2890's
// own body): a type-parameter field AHEAD of an owned class field, so a
// non-placeholder-compatible T shifts `cell`.
const char* kLib2890 =
    "module Lib2890; "
    "public class Cell "
    "  public tag: int := 3; "
    "end Cell; "
    "public class GCtor<T> "
    "  public v: T; "
    "  public owned cell: Cell; "
    "  public a: int; "
    "  public constructor() "
    "    cell := new Cell(); "
    "    a := 41; "
    "  end constructor; "
    "end GCtor;";

}  // namespace

// ===========================================================================
// T1 — the issue's exact repro. `class DCtor : GCtor<string>` with NO derived
// constructor: the implicit base chain calls the exporter's placeholder-layout
// `bz$GCtor$ctor` against a string-laid-out object. Pre-fix: SEGV on
// `d.cell.tag` (the issue: "AddressSanitizer: SEGV on unknown address
// 0x000000000035 ... in main"). Post-fix: REJECTED with the located #2890
// diagnostic. §2.5 / §22.8 step 1 / §14.1 / §12.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_implicit_base_ctor_2890,
           t1_issue_repro_segv_becomes_located_reject_22_8_14_1) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2890_t1_");
    TempFile2890 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2890 aObj((dir / (stem + "_a.o")).string());
    writeSource2890(aSrc.path, kLib2890);
    BEZEL_EXPECT_TRUE(compileToObj2890(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    std::string bSrc =
        "module App2890T1; import Lib2890; "
        "public class DCtor : GCtor<string> "
        "end DCtor; "
        "public class C "
        "  public static method Main(): int "
        "    using var d := new DCtor(); "
        "    return d.a + d.cell.tag; "
        "  end Main; "
        "end C;";
    TempFile2890 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2890 bExe((dir / (stem + "_b")).string());
    writeSource2890(bSrcFile.path, bSrc);

    std::string err;
    bool success =
        compileImportCaptureErr2890(bSrcFile.path, bExe.path, aObj.path, err);
    // Hinge part 1: without the fix this COMPILES (and SEGV's at runtime).
    BEZEL_EXPECT_TRUE(!success);
    // Hinge part 2: the rejection is this gate's, named — not some unrelated
    // failure (protocol: a coarse "it failed" cannot tell a reject from a
    // crash or a missing toolchain).
    BEZEL_EXPECT_TRUE(err.find("#2890") != std::string::npos);
}

// ===========================================================================
// T2 — accept-control: SAME `GCtor<T>` at `<int>`. The instantiation IS the
// i32 placeholder, so the exporter's compiled function reads exactly this
// object's offsets. Must still compile, link, run, and return 44 (= 41 + 3).
// A regression to "reject everywhere" would fail here. §14.1 / §12.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_implicit_base_ctor_2890,
           t2_accept_control_int_placeholder_matches_14_1) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2890_t2_");
    TempFile2890 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2890 aObj((dir / (stem + "_a.o")).string());
    writeSource2890(aSrc.path, kLib2890);
    BEZEL_EXPECT_TRUE(compileToObj2890(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    std::string bSrc =
        "module App2890T2; import Lib2890; "
        "public class DCtor : GCtor<int> "
        "end DCtor; "
        "public class C "
        "  public static method Main(): int "
        "    using var d := new DCtor(); "
        "    return d.a + d.cell.tag; "
        "  end Main; "
        "end C;";
    TempFile2890 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2890 bExe((dir / (stem + "_b")).string());
    writeSource2890(bSrcFile.path, bSrc);

    int r = compileImportAndRun2890(bSrcFile.path, bExe.path, aObj.path);
    if (r == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 44);
}

// ===========================================================================
// T3 — accept-control: an IMPORTED generic base whose parameterless ctor
// body has NO parameter-typed storage. The layout cannot shift, so the
// placeholder fallback is sound. Must still compile and return 7 (= 7 + 0).
// §14.1 / §12.
// ===========================================================================
BEZEL_TEST(xmod_imported_generic_implicit_base_ctor_2890,
           t3_accept_control_no_param_typed_storage_14_1) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2890_t3_");
    std::string libSrc =
        "module Lib2890T3; "
        "public class GBare<T> "
        "  public tag: int; "
        "  public constructor() "
        "    tag := 7; "
        "  end constructor; "
        "end GBare;";
    TempFile2890 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2890 aObj((dir / (stem + "_a.o")).string());
    writeSource2890(aSrc.path, libSrc);
    BEZEL_EXPECT_TRUE(compileToObj2890(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    std::string bSrc =
        "module App2890T3; import Lib2890T3; "
        "public class DCtor : GBare<string> "
        "end DCtor; "
        "public class C "
        "  public static method Main(): int "
        "    using var d := new DCtor(); "
        "    return d.tag; "
        "  end Main; "
        "end C;";
    TempFile2890 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2890 bExe((dir / (stem + "_b")).string());
    writeSource2890(bSrcFile.path, bSrc);

    int r = compileImportAndRun2890(bSrcFile.path, bExe.path, aObj.path);
    if (r == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 7);
}

}  // namespace bezel::module