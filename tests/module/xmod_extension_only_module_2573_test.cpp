// Cross-module extension dispatch from an EXTENSION-ONLY dependency (#2573;
// §26.5 step 2, spec v0.236).
//
// §26.5 step 2 is normative: "If the receiver's type declares no instance
// member of that name, the compiler searches the current module AND ALL
// IMPORTED MODULES for extension blocks on x's type ...". #735 added the
// metadata carriage and reconstruction for imported `public extension` blocks,
// but TWO gates still keyed solely off `SemaContext::hasImports()` — which
// reflects `importedByModule_` (imported TYPES), NOT
// `importedExtensionsByModule_`. A dependency whose public surface carries
// `public extension` blocks but NO exported types never populates
// `importedByModule_`, so:
//   - ExprTypecheck.cpp's import-population loop (which fills
//     `importedExtensions_` for lookupExtensionMember) was skipped → the
//     importer rejected `x.Pick()` with "no member `Pick` on primitive type
//     `int`";
//   - and even after that sema gate was lifted, LlvmBackend_Setup.cpp's
//     `declareImportedFunctions()` (which declares the external
//     `bz$ext$int$<M>`) was skipped → the call lowered to a zero/null value
//     (the latent #2491 "return-value expression failed to lower").
//
// Adding an otherwise-unused exported type to the dependency made the call
// resolve — the tell-tale sign of a missing-extension gate, not a real
// visibility rule. The fix widens both gates to also engage when
// `hasImportedExtensions()` (an extension-only dependency).
//
// WHY THESE FAIL WITHOUT THE FIX: with the sema gate unchanged the importer
// emits a member-not-found diagnostic, `compileFile` fails, and importRun2573
// returns -1 — never the asserted value. (With only the sema gate lifted, the
// codegen gate would still make the call degrade to a zero return, so the
// value assertions catch a half-fix too.) They pass only once BOTH gates admit
// an extension-only dependency.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>

#include "driver/Driver.hpp"
#include "test_harness.hpp"
#include "support/unique_stem.hpp"


namespace {

struct TempFile2573 {
    std::string path;
    explicit TempFile2573(std::string p) : path(std::move(p)) {}
    ~TempFile2573() { std::remove(path.c_str()); }
};

void write2573(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileObj2573(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// Compile the importer against `aObj`, run it, and return its exit code.
// -1 = the importer failed to compile/link or did not exit normally (exactly
// what happens without the fix: the member-not-found reject fails compileFile
// before any executable is produced).
int importRun2573(const std::string& bSrc, const std::string& bExe,
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

}  // namespace

// #2573 / §26.5 step 2: the issue's own shape — an extension-ONLY dependency
// (no exported types) whose `public extension int` must be discoverable by the
// importer. `Triple()` reads the primitive receiver by value (§26.1) and
// returns this*3. Before the fix the importer could not resolve `Triple` at
// all (no exported types ⇒ hasImports() false ⇒ importedExtensions_ never
// populated), so this returned -1.
BEZEL_TEST(xmod_extension_only_2573, extension_only_module_primitive_receiver_26_5) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2573_prim_");
    TempFile2573 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2573 aObj((dir / (stem + "_a.o")).string());

    // Extension-only: NO exported type accompanies the block.
    write2573(aSrc.path,
        "module ExtOnly2573; "
        "public extension int "
        "  public method Triple(): int => this * 3; "
        "end;");

    BEZEL_EXPECT_TRUE(compileObj2573(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    TempFile2573 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2573 bExe((dir / (stem + "_b")).string());
    write2573(bSrc.path,
        "module App; "
        "import ExtOnly2573; "
        "class C public static method Main(): int "
        "  var x: int := 7; "
        "  return x.Triple(); "          // 7 * 3 = 21
        "end Main; end C;");

    BEZEL_EXPECT_EQ(importRun2573(bSrc.path, bExe.path, aObj.path), 21);
}

// #2573 / §26.5 step 2 + §26.1: an extension-only dependency's method that
// takes an argument, to exercise both the receiver ABI (primitive `this` by
// value) AND the cross-module argument lowering through the external
// `bz$ext$int$AddN`. 7 + 5 = 12. A wrong-receiver or undeclared-symbol
// lowering changes the result or degrades to zero, so the value pins both
// gates.
BEZEL_TEST(xmod_extension_only_2573, extension_only_module_method_with_arg_26_1) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2573_arg_");
    TempFile2573 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2573 aObj((dir / (stem + "_a.o")).string());

    write2573(aSrc.path,
        "module ExtOnly2573Arg; "
        "public extension int "
        "  public method AddN(n: int): int => this + n; "
        "end;");

    BEZEL_EXPECT_TRUE(compileObj2573(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    TempFile2573 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2573 bExe((dir / (stem + "_b")).string());
    write2573(bSrc.path,
        "module App; "
        "import ExtOnly2573Arg; "
        "class C public static method Main(): int "
        "  var x: int := 7; "
        "  return x.AddN(5); "           // 7 + 5 = 12
        "end Main; end C;");

    BEZEL_EXPECT_EQ(importRun2573(bSrc.path, bExe.path, aObj.path), 12);
}
