// Module — #5249 (spec §14.1, v0.313): an exported `class Box<Stopwatch>`
// (type parameter named like a builtin) instantiated at `int` from an importer
// must not free the int field as a Stopwatch in the instantiated destructor.
// Two-module mechanics as in nested_user_type_builtin_home_path_5255_test.cpp.
// Oracle: exact stdout + exit code of the linked importer program.


#include "driver/Driver.hpp"
#include "test_harness.hpp"

#include <array>
#include <cstdio>
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

struct RunResult {
    bool built = false;
    int exitCode = -1;
    std::string out;
};

// Compile libSrc to an object, compile mainSrc against it, link, run capturing
// stdout.
RunResult buildAndRun(const std::string& libSrc, const std::string& mainSrc,
                      const std::string& tag) {
    RunResult res;
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_5249_" + tag + "_" + unique_stem("");
    TempFile lSrc((dir / (stem + "_lib.bzl")).string());
    TempFile lObj((dir / (stem + "_lib.o")).string());
    TempFile mSrc((dir / (stem + "_main.bzl")).string());
    TempFile mExe((dir / stem).string());
    writeSource(lSrc.path, libSrc);
    writeSource(mSrc.path, mainSrc);

    bezel::Driver driver;
    bezel::CompileResult rl = driver.compileObject(lSrc.path, lObj.path);
    if (!rl.success) return res;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency dl;
    dl.metadataPath = lObj.path;
    dl.objectPath = lObj.path;
    deps.push_back(dl);
    bezel::CompileResult rm = driver.compileFile(mSrc.path, mExe.path, deps);
    if (!rm.success) return res;
    res.built = true;

    FILE* p = popen(mExe.path.c_str(), "r");
    if (!p) return res;
    std::array<char, 256> buf;
    while (std::fgets(buf.data(), buf.size(), p)) res.out += buf.data();
    int status = pclose(p);
    if (WIFEXITED(status)) res.exitCode = WEXITSTATUS(status);
    return res;
}


const char* kLib =
    "module LibB; "
    "public class Box<Stopwatch> "
    "  public v: Stopwatch; "
    "  public constructor(x: Stopwatch) v := x; end constructor; "
    "end Box;";

}  // namespace

// R3: cross-module instantiation at int, 200x.
BEZEL_TEST(type_param_named_like_builtin_5249,
          spec_14_1_cross_module_type_param_named_stopwatch_int) {
    RunResult r = buildAndRun(
        kLib,
        "module App; import LibB; "
        "class Program public static method Main(): int "
        "  var s := 0; "
        "  for var i in 0..199 do "
        "    var b := new Box<int>(5); "
        "    s := s + b.v; "
        "  end for; "
        "  Console.WriteLine(s); "
        "  return 0; "
        "end Main; end Program;",
        "r3");
    BEZEL_EXPECT_EQ(r.built, true);
    BEZEL_EXPECT_EQ(r.out, std::string("1000\n"));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

}  // namespace bezel::module
