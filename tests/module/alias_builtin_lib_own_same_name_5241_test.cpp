// Module — #5241 (follow-up to #4698; spec §2.2, v0.313): "The alias is an
// additional *qualifier*, not a rename". A library that declares its OWN public
// `Stopwatch` and also exposes `D.Stopwatch` (import System.Diagnostics as D)
// in a public signature must export the STDLIB type for the qualified spelling:
// module metadata used to record the bare name, so the importer rebound it to
// the library's own `Stopwatch` ("no member IsRunning on type Stopwatch").
//
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
    std::string stem = "bezel_5241_" + tag + "_" + unique_stem("");
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
    "module Lib; "
    "import System.Diagnostics as D; "
    "public class Stopwatch "
    "  public constructor() end constructor; "
    "  public method Ping(): int return 42; end Ping; "
    "end Stopwatch; "
    "public class Holder "
    "  public static method Make(): owned D.Stopwatch "
    "    return D.Stopwatch.StartNew(); "
    "  end Make; "
    "  public static method Take(s: D.Stopwatch): bool "
    "    return s.IsRunning; "
    "  end Take; "
    "  public static method Mine(): owned Stopwatch "
    "    return new Stopwatch(); "
    "  end Mine; "
    "end Holder;";

}  // namespace

BEZEL_TEST(alias_builtin_lib_own_same_name_5241,
          exported_alias_qualified_builtin_is_stdlib_not_lib_type_spec_2_2) {
    RunResult r = buildAndRun(
        kLib,
        "module App; "
        "import Lib; "
        "class Program "
        "  public static method Main(): int "
        "    using var s := Holder.Make(); "
        "    Console.WriteLine(s.IsRunning); "
        "    Console.WriteLine(Holder.Take(s)); "
        "    return 0; "
        "  end Main; "
        "end Program;",
        "exported");
    BEZEL_EXPECT_EQ(r.built, true);
    BEZEL_EXPECT_EQ(r.out, std::string("True\nTrue\n"));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// Control: the bare `Stopwatch` in the library's signature still means the
// library's own public class.
BEZEL_TEST(alias_builtin_lib_own_same_name_5241,
          exported_bare_name_still_means_lib_type_spec_2_2) {
    RunResult r = buildAndRun(
        kLib,
        "module App; "
        "import Lib; "
        "class Program "
        "  public static method Main(): int "
        "    using var m := Holder.Mine(); "
        "    Console.WriteLine(m.Ping()); "
        "    return 0; "
        "  end Main; "
        "end Program;",
        "bare");
    BEZEL_EXPECT_EQ(r.built, true);
    BEZEL_EXPECT_EQ(r.out, std::string("42\n"));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

}  // namespace bezel::module
