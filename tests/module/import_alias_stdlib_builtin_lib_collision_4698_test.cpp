// Module — #4698 review row (spec §2.2 / §2.7, v0.313): under `import
// System.Diagnostics as D;`, `D.Stopwatch` names the stdlib §8.1 built-in even
// when another imported module (`Lib`) exports a public `Stopwatch`. §2.2: "The
// alias is an additional *qualifier*, not a rename" and a bare `Stopwatch`
// would mean Lib's type; the qualified spelling must not fall back to it.
//
// Oracle: the exit code. Lib's `Stopwatch` has no `IsRunning`, so binding the
// qualified name to it is a compile failure (-1); the built-in's `IsRunning`
// after `StartNew()` is true.

#include "driver/Driver.hpp"
#include "test_harness.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <vector>
#include "support/unique_stem.hpp"

namespace bezel::module {
namespace {

namespace fs = std::filesystem;

struct TempFile {
    std::string path;
    explicit TempFile(const std::string& p) : path(p) {}
    ~TempFile() { std::remove(path.c_str()); }
};

void writeSource(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Compile libSrc to an object, compile mainSrc against it, link, run; return
// the exit code (-1 on any compile/link/run failure).
int buildAndRunOneLib(const std::string& libSrc, const std::string& mainSrc,
                      const std::string& tag) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_4698_" + tag + "_" + unique_stem("");
    TempFile lSrc((dir / (stem + "_lib.bzl")).string());
    TempFile lObj((dir / (stem + "_lib.o")).string());
    TempFile mSrc((dir / (stem + "_main.bzl")).string());
    TempFile mExe((dir / stem).string());
    writeSource(lSrc.path, libSrc);
    writeSource(mSrc.path, mainSrc);

    bezel::Driver driver;
    bezel::CompileResult rl = driver.compileObject(lSrc.path, lObj.path);
    if (!rl.success) return -1;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency dl;
    dl.metadataPath = lObj.path;
    dl.objectPath = lObj.path;
    deps.push_back(dl);
    bezel::CompileResult rm = driver.compileFile(mSrc.path, mExe.path, deps);
    if (!rm.success) return -1;

    int status = std::system(mExe.path.c_str());
    if (status == -1 || !WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

}  // namespace

BEZEL_TEST(import_alias_stdlib_builtin_lib_collision_4698,
          alias_reaches_builtin_not_imported_lib_type_spec_2_2) {
    int code = buildAndRunOneLib(
        "module Lib; "
        "public class Stopwatch "
        "  public constructor() end constructor; "
        "  public static method StartNew(): owned Stopwatch return new Stopwatch(); end StartNew; "
        "end Stopwatch;",
        "module App; "
        "import Lib; "
        "import System.Diagnostics as D; "
        "class Program "
        "  public static method Main(): int "
        "    var ok := 0; "
        "    using var s := D.Stopwatch.StartNew(); "
        "    if s.IsRunning then ok := ok + 1; end; "
        "    using var t: D.Stopwatch := new D.Stopwatch(); "
        "    if not t.IsRunning then ok := ok + 2; end; "
        "    return ok; "
        "  end Main; "
        "end Program;",
        "lib_collision");
    BEZEL_EXPECT_EQ(code, 3);
}

}  // namespace bezel::module
