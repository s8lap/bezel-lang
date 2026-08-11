// Cross-module — fix #3044 (§5.4 / §10.4, v0.242): the checked enum cast and
// the compiler-provided `FromUnderlyingOrNull` must work identically for an
// IMPORTED enum.
//
// This is the arm a same-module test never reaches. An imported enum has no
// `ast::TypeDecl` on the importer's side, so its declared-value set cannot be
// re-derived from source: it comes from the reconstructed
// `EnumConstantSymbol::value` the dependency metadata carries (issue #328). The
// checked cast, `FromUnderlyingOrNull`, and §19.6/§21.7's exhaustiveness all
// read that same set — if it were empty or zeroed on this side, the cast would
// either reject every value or accept every value, and neither would be visible
// from a single-module test.

#include "module/ModuleLoader.hpp"

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

struct TempFile {
    std::string path;
    explicit TempFile(const std::string& p) : path(p) {}
    ~TempFile() { std::remove(path.c_str()); }
};

void writeSource(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Build `libSrc` as a library object, then compile+link `appSrc` against it and
// run the result, returning the process exit code (or -1 if any step failed).
// Exit codes are 8-bit, so every expected value below is kept under 256.
int buildAndRun(const std::string& stem, const std::string& libSrc,
                const std::string& appSrc) {
    auto dir = std::filesystem::temp_directory_path();
    TempFile a((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile b((dir / (stem + "_b.bzl")).string());
    TempFile bExe((dir / (stem + "_b")).string());

    writeSource(a.path, libSrc);
    writeSource(b.path, appSrc);

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileObject(a.path, aObj.path);
    if (!ra.success) return -1;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    bezel::CompileResult rb = driver.compileFile(b.path, bExe.path, deps);
    if (!rb.success) return -1;
    int status = std::system(bExe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// As above, but reports whether compiling the APP succeeded — for the §5.4
// constant arm, whose verdict is a compile error rather than an exit code.
bool appCompiles(const std::string& stem, const std::string& libSrc,
                 const std::string& appSrc) {
    auto dir = std::filesystem::temp_directory_path();
    TempFile a((dir / (stem + "_a.bzl")).string());
    TempFile aObj((dir / (stem + "_a.o")).string());
    TempFile b((dir / (stem + "_b.bzl")).string());
    TempFile bObj((dir / (stem + "_b.o")).string());

    writeSource(a.path, libSrc);
    writeSource(b.path, appSrc);

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileObject(a.path, aObj.path);
    if (!ra.success) return false;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    return driver.compileObject(b.path, bObj.path, deps).success;
}

// Sparse on purpose: 10, 20, 30 leaves gaps an in-range value can fall into.
const char* kLib =
    "module PLib; public enum P Low := 10, Mid := 20, High := 30; end P;";

}  // namespace

// §5.4 runtime arm, imported enum: an undeclared value raises OverflowException.
BEZEL_TEST(xmod_enum_checked_cast_3044, imported_undeclared_raises_spec_5_4) {
    BEZEL_EXPECT_EQ(
        buildAndRun("bezel_3044_raise", kLib,
            "module PApp; import PLib; class C "
            "  public static method Main(): int "
            "    try var n: int := 25; var p: P := P(n); return int(p); "
            "    catch OverflowException => return 42; end; "
            "  end Main; end C;"),
        42);
}

// OVER-REJECTION GUARD: every DECLARED value of the imported enum still casts —
// this is what fails loudly if the importer's declared-value set were empty or
// zeroed rather than the metadata-carried one.
BEZEL_TEST(xmod_enum_checked_cast_3044, imported_declared_values_accepted_spec_5_4) {
    BEZEL_EXPECT_EQ(
        buildAndRun("bezel_3044_ok", kLib,
            "module PApp; import PLib; class C "
            "  public static method Main(): int "
            "    var a: int := 10; var b: int := 20; var c: int := 30; "
            "    return int(P(a)) - int(P(b)) + int(P(c)) + 22; "  // 10-20+30+22
            "  end Main; end C;"),
        42);
}

// §5.4 constant arm, imported enum: a constant equal to no declared constant is
// a compile error at the cast site, on the importer's side.
BEZEL_TEST(xmod_enum_checked_cast_3044, imported_constant_undeclared_rejected_spec_5_4) {
    BEZEL_EXPECT_FALSE(
        appCompiles("bezel_3044_creject", kLib,
            "module PApp; import PLib; class C "
            "  public static method Main(): int var p: P := P(25); "
            "  return int(p); end Main; end C;"));
}

// The same shape with a DECLARED constant must still compile — the pair bounds
// the claim to declaredness rather than to "constants of imported enums".
BEZEL_TEST(xmod_enum_checked_cast_3044, imported_constant_declared_accepted_spec_5_4) {
    BEZEL_EXPECT_TRUE(
        appCompiles("bezel_3044_caccept", kLib,
            "module PApp; import PLib; class C "
            "  public static method Main(): int var p: P := P(20); "
            "  return int(p); end Main; end C;"));
}

// §10.4 / stdlib §9.9: the compiler-provided branch tier over an imported enum.
BEZEL_TEST(xmod_enum_checked_cast_3044, imported_from_underlying_or_null_spec_10_4) {
    BEZEL_EXPECT_EQ(
        buildAndRun("bezel_3044_fun", kLib,
            "module PApp; import PLib; class C "
            "  public static method Main(): int "
            "    var gap: int := 25; var hit: int := 30; "
            "    var a := P.FromUnderlyingOrNull(gap); "
            "    var b := P.FromUnderlyingOrNull(hit); "
            "    return int(a = null) * 12 + int(b!); "        // 12 + 30
            "  end Main; end C;"),
        42);
}

}  // namespace bezel::module
