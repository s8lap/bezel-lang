// Spec-derived regression test for #6232, the IMPORTED arm — §35.3, §35.11
// (v0.338).
//
// `resolveMemberAccess` types a payload-free case of an imported choice in
// its own arm (#3144): an imported member returns before the same-module arm
// runs. That arm dropped the qualifier's type arguments exactly as the
// same-module one did, so `InnerU<Own>.No` naming an IMPORTED generic choice
// was typed as the bare declaration, the local it initialized was never
// enrolled for disposal, and a whole value assigned over it leaked.
//
// The program returns its destructor count as the exit code (100 iterations,
// so the 8-bit exit status holds it): the leak is `0`, the fix `100`. The
// LOCAL twin — the same declarations in one module — pins the same-module
// arm through the same harness (it failed the same way before the fix).
//
// per §35.3, §35.11, §35.4, v0.338

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"  // compileFileForTest
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

namespace fs = std::filesystem;

struct TempFile6232 {
    std::string path;
    explicit TempFile6232(std::string p) : path(std::move(p)) {}
    ~TempFile6232() { std::remove(path.c_str()); }
};

constexpr int kDepFailed6232 = -2;  // the exporter itself failed
constexpr int kRejected6232 = -1;   // the importer did not compile
constexpr int kSignalBase6232 = 1000;

int runChild6232(const std::string& exe) {
    pid_t pid = ::fork();
    if (pid == 0) {
        ::execl(exe.c_str(), exe.c_str(), (char*)nullptr);
        ::_exit(127);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    if (WIFSIGNALED(status)) return kSignalBase6232 + WTERMSIG(status);
    return WIFEXITED(status) ? WEXITSTATUS(status) : kRejected6232;
}

// With `libSrc` non-empty: compile it to an object and `appSrc` against it.
// Empty: `appSrc` is the whole program. Runs the result in a child process.
int buildAndRun6232(const std::string& libSrc, const std::string& appSrc,
                    const std::string& tag) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bz6232_" + tag + "_" + unique_stem("");
    TempFile6232 lSrc((dir / (stem + "_lib.bzl")).string());
    TempFile6232 lObj((dir / (stem + "_lib.o")).string());
    std::vector<bezel::Dependency> deps;
    if (!libSrc.empty()) {
        std::ofstream(lSrc.path) << libSrc;
        bezel::Driver driver;
        if (!driver.compileObject(lSrc.path, lObj.path).success)
            return kDepFailed6232;
        if (!std::ifstream(lObj.path).good()) return kDepFailed6232;
        bezel::Dependency d;
        d.metadataPath = lObj.path;
        d.objectPath = lObj.path;
        deps.push_back(d);
    }
    TempFile6232 aSrc((dir / (stem + "_app.bzl")).string());
    TempFile6232 aExe((dir / stem).string());
    std::ofstream(aSrc.path) << appSrc;
    bezel::Driver driver;
    const bool compiled =
        deps.empty()
            ? bezel::test::compileFileForTest(driver, aSrc.path, aExe.path)
                  .success
            : bezel::test::compileFileForTest(driver, aSrc.path, aExe.path,
                                              deps)
                  .success;
    if (!compiled) return kRejected6232;
    if (!std::ifstream(aExe.path).good()) return kRejected6232;
    return runChild6232(aExe.path);
}

const char* const kDecls6232 = R"(
public class Stats public static Drops: int := 0; end Stats;
public class Disp
  public N: int;
  public constructor(n: int) N := n; end constructor;
  destructor Stats.Drops := Stats.Drops + 1; end destructor;
end Disp;
public choice Own Held(owned r: Disp), Empty; end Own;
public choice InnerU<T> Has(v: T), No; end InnerU;
)";

const char* const kProgram6232 = R"(
class Program
  static method Step(k: int): int
    var w1 := InnerU<Own>.No;
    var w2 := InnerU<Own>.Has(Own.Held(new Disp(k)));
    w1 := w2;
    return 1;
  end Step;
  public static method Main(): int
    for var k in 0..<100 do Program.Step(k); end;
    return Stats.Drops;
  end Main;
end Program;
)";

}  // namespace

// The imported arm: `InnerU` and `Own` come from another module. Before the
// fix the exit code was 0 (nothing disposed).
BEZEL_TEST(xmod_generic_payload_free_case_type_args_6232,
           imported_generic_payload_free_case_then_binding_assigned_disposes_spec_35_11) {
    BEZEL_EXPECT_EQ(
        buildAndRun6232(std::string("module Lib;") + kDecls6232,
                        std::string("module App;\nimport Lib;") + kProgram6232,
                        "imp"),
        100);
}

// The local twin: the same-module arm. Before the fix the exit code was 0 too.
BEZEL_TEST(xmod_generic_payload_free_case_type_args_6232,
           local_twin_generic_payload_free_case_then_binding_assigned_disposes_spec_35_11) {
    BEZEL_EXPECT_EQ(
        buildAndRun6232("", std::string("module App;") + kDecls6232 + kProgram6232,
                        "loc"),
        100);
}

}  // namespace bezel::module
