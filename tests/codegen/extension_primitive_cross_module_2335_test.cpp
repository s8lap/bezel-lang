// fix #2335 (§2.8.3 rule 3 / §26, v0.228): extension methods on primitive
// targets now carry the extension's own declaring module in their mangled name,
// so `public extension int { method Foo(): ... }` in two different modules
// produces distinct `bz$ext$<module>.int$Foo` symbols rather than colliding
// at link time.
//
// Before the fix, two modules each declaring `public extension int { method Foo() }`
// both emitted `bz$ext$int$Foo` with InternalLinkage, causing "multiple definition"
// linker errors when both objects were linked together.
//
// After the fix, each extension's mangled name includes its own module:
//   Mod1 emits `bz$ext$Mod1.int$Foo`
//   Mod2 emits `bz$ext$Mod2.int$Foo`
// These are distinct, and the linker accepts both.
//
// The fix tracks the extension's declaring module name in `ExtensionSymbol::moduleName`
// (populated from `moduleDottedName(ctx.module())` locally and from
// `md.moduleName` when imported), stored in a reverse-map
// `importedExtensionsByDecl_` so `Emitter::functionName` can reach it for
// primitive/builtin targets that have no TypeSymbol of their own.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace {
namespace fs = std::filesystem;

const std::filesystem::path& wtDir2335() {
    return wtScratchDir("wt-2335");
}

struct TempFile2335 {
    std::string path;
    explicit TempFile2335(std::string p) : path(std::move(p)) {}
    ~TempFile2335() { std::remove(path.c_str()); }
};

void writeSource2335(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Compile one module to object, return whether compilation succeeded.
// The object file path is transferred to the caller — it is NOT a TempFile
// in this function (the source TempFile is destroyed here, but the object
// path is transferred via std::move before the destructor runs).
bool compileModule(const std::string& src, const std::string& stem,
                   const std::vector<std::string>& importPaths,
                   std::string* outObjPath) {
    auto dir = wtDir2335();
    TempFile2335 sSrc((dir / (stem + ".bzl")).string());
    // Object path: NOT a TempFile.  The caller manages its lifetime.
    std::string objPath = (dir / (stem + ".o")).string();
    writeSource2335(sSrc.path, src);

    bezel::Driver driver;
    bool ok;
    if (!importPaths.empty()) {
        std::vector<bezel::Dependency> deps;
        for (const auto& ip : importPaths) {
            bezel::Dependency dep;
            dep.metadataPath = ip;
            dep.objectPath = ip;
            deps.push_back(dep);
        }
        ok = driver.compileObject(sSrc.path, objPath, deps).success;
    } else {
        ok = driver.compileObject(sSrc.path, objPath).success;
    }
    *outObjPath = std::move(objPath);
    return ok;
}

// Compile main importing given objects, return exit code or -1 on link failure.
int compileAndLink(const std::string& mainSrc,
                   const std::vector<std::string>& importObjs) {
    auto dir = wtDir2335();
    std::string stem = unique_stem("bezel_2335");
    TempFile2335 mSrc((dir / (stem + ".bzl")).string());
    TempFile2335 mExe((dir / stem).string());
    writeSource2335(mSrc.path, mainSrc);

    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    for (const auto& ip : importObjs) {
        bezel::Dependency dep;
        dep.metadataPath = ip;
        dep.objectPath = ip;
        deps.push_back(dep);
    }
    if (!driver.compileFile(mSrc.path, mExe.path, deps).success) return -1;

    int status = std::system(mExe.path.c_str());
    if (status == -1 || !WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

// Module 1: `public extension int { public method Foo(): int => 42; }`
const char* kMod1Src =
    "module Mod1; "
    "public extension int "
    "  public method Foo(): int => 42; "
    "end; ";

// Module 2: `public extension int { public method Foo(): int => 99; }`
const char* kMod2Src =
    "module Mod2; "
    "public extension int "
    "  public method Foo(): int => 99; "
    "end; ";

// Main imports both modules and calls Foo from Mod1 via explicit import.
const char* kMainSrc =
    "module Main; "
    "import Mod1; "
    "class Program "
    "  public static method Main(): int "
    "    var x: int := 5; "
    "    return x.Foo(); "  // resolved to Mod1.Foo via import
    "  end Main; "
    "end Program; ";
}  // namespace

// #2335 / §2.8.3 rule 3 / §26 — two modules each declare
// `public extension int { method Foo() }` with the SAME body signature.
// Without the fix, linking both objects produces:
//   bezelc: link error: multiple definition of `bz$ext$int$Foo`
// With the fix, each produces a distinct mangled name:
//   Mod1 emits bz$ext$Mod1.int$Foo
//   Mod2 emits bz$ext$Mod2.int$Foo
// The link succeeds and the program runs (returns 42 from Mod1.Foo).
BEZEL_TEST(extension_primitive_cross_module_2335, two_modules_same_extension_int_Foo_links) {
    std::string obj1, obj2;
    BEZEL_EXPECT_TRUE(compileModule(kMod1Src, "mod1", {}, &obj1));
    BEZEL_EXPECT_TRUE(compileModule(kMod2Src, "mod2", {}, &obj2));

    // Linking both objects succeeds only if the extension methods have
    // distinct mangled names. Pre-fix: "multiple definition of bz$ext$int$Foo".
    int result = compileAndLink(kMainSrc, {obj1, obj2});
    BEZEL_EXPECT_TRUE(result != -1);  // -1 means compile or link failure
    BEZEL_EXPECT_EQ(result, 42);     // Mod1.Foo returns 42
}
