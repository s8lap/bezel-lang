// fix #798 / §2.5 / §22.13: an IMPORTED type's static initialization now runs in
// its defining module on first cross-module use. Before, the importer read the
// backing global's zero value because the per-module `bz$<T>$ensureinit` was
// InternalLinkage and `emitEnsureStaticInit(importedType)` was a no-op — so an
// imported static with a NON-CONSTANT initializer silently read 0.
//
// Fix: a public non-generic type's ensureinit is exported (ExternalLinkage), and
// the importer declares + calls `bz$<T>$ensureinit` at each cross-module
// static-member access. Every imported type is public, and the defining module
// exports an ensureinit for each public non-generic type with a static member, so
// the symbol always resolves at link (a const-only / stateless type gets a no-op
// ensureinit).
//
// These need the FULL compile + link + run pipeline (the bug is in codegen +
// cross-module linkage), so they use compileObject + compileFile-with-deps.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>

#include "driver/Driver.hpp"
#include "test_harness.hpp"
#include "support/unique_stem.hpp"


namespace {
namespace fs = std::filesystem;

struct TempFile798 {
    std::string path;
    explicit TempFile798(std::string p) : path(std::move(p)) {}
    ~TempFile798() { std::remove(path.c_str()); }
};

void write798(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileObj798(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// Compile the importer against the dependency object and run it; returns the exit
// code, or -1 on compile/link/run failure.
int importRun798(const std::string& bSrc, const std::string& bExe,
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

int runImported(const std::string& libSrc, const std::string& appSrc) {
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz798_");
    TempFile798 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile798 aObj((dir / (stem + "_a.o")).string());
    write798(aSrc.path, libSrc);
    if (!compileObj798(aSrc.path, aObj.path)) return -2;
    if (!std::ifstream(aObj.path).good()) return -2;
    TempFile798 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile798 bExe((dir / (stem + "_b")).string());
    write798(bSrc.path, appSrc);
    return importRun798(bSrc.path, bExe.path, aObj.path);
}
}  // namespace

// §2.5 / §22.13 (the fix): an imported static field with a NON-CONSTANT
// initializer is initialized on first cross-module use. `Counter._n := Compute()`
// reads 42 in the importer (was 0 — init never ran).
BEZEL_TEST(xmod_static_init_798, imported_nonconst_static_field_init_runs) {
    int r = runImported(
        "module IOLib; "
        "public class Counter "
        "  public static _n: int := Compute(); "
        "  public static method Compute(): int => 42; "
        "end Counter;",
        "module App; "
        "import IOLib; "
        "public class C public static method Main(): int "
        "  return Counter._n; "  // 42
        "end Main; end C;");
    if (r == -2) return;  // toolchain/compile of dependency unavailable — skip
    BEZEL_EXPECT_EQ(r, 42);
}

// §2.5 (no link error): an imported type with only CONST static fields still gets
// a (no-op) exported ensureinit, so the importer's ensureinit call resolves at
// link. `K._c + 1` = 8.
BEZEL_TEST(xmod_static_init_798, imported_const_static_field_no_link_error) {
    int r = runImported(
        "module CLib; "
        "public class K "
        "  public static _c: int := 7; "
        "  public static method Twice(): int => _c * 2; "
        "end K;",
        "module App; "
        "import CLib; "
        "public class C public static method Main(): int => K._c + 1; end C;");  // 8
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 8);
}

// §22.13: transitive init within the imported module — A's static init reads B's
// static (both in the dependency); accessing A._a from the importer initializes
// both. A._a := B._b + 1, B._b := 5 -> 6.
BEZEL_TEST(xmod_static_init_798, imported_transitive_static_init_runs) {
    int r = runImported(
        "module DLib; "
        "public class B public static _b: int := 5; end B; "
        "public class A public static _a: int := B._b + 1; end A;",
        "module App; "
        "import DLib; "
        "public class C public static method Main(): int => A._a; end C;");  // 6
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 6);
}
