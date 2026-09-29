// fix #4789: resolve cross-module extension member dispatch through imported extension
// module (§26.5, §11.3, §12.1, v0.307)
//
// Discovered in R20 lane investigation of #4681 (s26_5_0016 corpus-setup).
// The conformance case `tests/conformance/rules/cases/s26_5_0016/main.bzl` imports a
// class from one module and calls an extension member from a DIFFERENT module —
// bezelc was rejecting with "no member Describe on type Shape" even with the correct
// --import chain. Spec §26.5 (cross-module extension visibility), v0.307.
//
// Sibling s26_5_0014 (extension in SAME module as receiver class) works —
// bug is specifically the cross-module dispatch path.

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "driver/Driver.hpp"
#include "support/aot_support.hpp"
#include "support/driver_run_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::compileFileForTest;

namespace bezel::sema {

namespace {

struct TempFile4789 {
    std::string path;
    explicit TempFile4789(std::string p) : path(std::move(p)) {}
    ~TempFile4789() { std::remove(path.c_str()); }
};

void writeSource4789(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

struct RunCaptureResult { int code; std::string out; };
RunCaptureResult runCapture(const std::string& exe) {
    std::string out;
    FILE* p = popen((exe + " 2>&1").c_str(), "r");
    if (!p) return {-1, ""};
    char buf[256];
    while (std::size_t n = std::fread(buf, 1, sizeof(buf), p)) out.append(buf, n);
    int status = pclose(p);
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    return {code, out};
}

// Cross-module runner: dep1, then dep2 (depends on dep1), then main (depends on both).
// Returns {compileOk, runRc, runOut, diagText}.
std::tuple<bool, int, std::string, std::string>
compileImportRun4789(const std::string& dep1Src,
                    const std::string& dep2Src,
                    const std::string& mainSrc,
                    const std::string& tag) {
    const auto& dir = wtScratchDir("wt-4789");
    std::string stem = "bz4789_" + tag + "_" + unique_stem("");

    // Dep 1 (e.g. base.bzl)
    TempFile4789 d1Src((dir / (stem + "_d1.bzl")).string());
    TempFile4789 d1Obj((dir / (stem + "_d1.o")).string());
    writeSource4789(d1Src.path, dep1Src);
    {
        bezel::Driver driver;
        auto r = driver.compileObject(d1Src.path, d1Obj.path);
        if (!r.success) return {false, -1, "", "dep1 compile failed"};
    }

    // Dep 2 (e.g. ext.bzl) — depends on dep1
    TempFile4789 d2Src((dir / (stem + "_d2.bzl")).string());
    TempFile4789 d2Obj((dir / (stem + "_d2.o")).string());
    writeSource4789(d2Src.path, dep2Src);
    {
        bezel::Driver driver;
        std::vector<bezel::Dependency> deps;
        bezel::Dependency d;
        d.metadataPath = d1Obj.path;
        d.objectPath = d1Obj.path;
        deps.push_back(d);
        auto r = driver.compileObject(d2Src.path, d2Obj.path, deps);
        if (!r.success) return {false, -1, "", "dep2 compile failed"};
    }

    // Main — imports both deps
    TempFile4789 mainBz((dir / (stem + "_main.bzl")).string());
    TempFile4789 exe((dir / stem).string());
    writeSource4789(mainBz.path, mainSrc);

    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d1, d2;
    d1.metadataPath = d1Obj.path; d1.objectPath = d1Obj.path;
    d2.metadataPath = d2Obj.path; d2.objectPath = d2Obj.path;
    deps.push_back(d1);
    deps.push_back(d2);

    auto appR = compileFileForTest(driver, mainBz.path, exe.path, deps);
    if (!appR.success) return {false, -1, "", "compile failed"};

    auto run = runCapture(exe.path);
    return {true, run.code, run.out, ""};
}

// One-dep variant for same-module control.
std::tuple<bool, int, std::string, std::string>
compileImportRun4789_oneDep(const std::string& depSrc,
                            const std::string& mainSrc,
                            const std::string& tag) {
    const auto& dir = wtScratchDir("wt-4789");
    std::string stem = "bz4789_" + tag + "_" + unique_stem("");

    TempFile4789 d1Src((dir / (stem + "_d1.bzl")).string());
    TempFile4789 d1Obj((dir / (stem + "_d1.o")).string());
    writeSource4789(d1Src.path, depSrc);
    {
        bezel::Driver driver;
        auto r = driver.compileObject(d1Src.path, d1Obj.path);
        if (!r.success) return {false, -1, "", "dep compile failed"};
    }

    TempFile4789 mainBz((dir / (stem + "_main.bzl")).string());
    TempFile4789 exe((dir / stem).string());
    writeSource4789(mainBz.path, mainSrc);

    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = d1Obj.path; d.objectPath = d1Obj.path;
    deps.push_back(d);

    auto appR = compileFileForTest(driver, mainBz.path, exe.path, deps);
    if (!appR.success) return {false, -1, "", "compile failed"};

    auto run = runCapture(exe.path);
    return {true, run.code, run.out, ""};
}

} // anonymous namespace

// ============================================================================
// T1 — §26.5 cross-module extension member dispatch: the core repro from the
// issue. base.bzl (module Shapes): public class Shape. extb.bzl (module ExtB,
// import Shapes): public extension Shape { method Describe(): string => "from-b" }.
// main.bzl (module Main, import Shapes, import ExtB): s.Describe() — should
// resolve to ExtB.Describe and print "from-b".
// ============================================================================
BEZEL_TEST(sema_extension_cross_module_4789,
           basic_cross_module_extension_accepts) {
    const char* dep1 = R"(
module Shapes;
public class Shape
  public constructor() end constructor;
end Shape;
)";
    const char* dep2 = R"(
module ExtB;
import Shapes;
public extension Shape
  public method Describe(): string => "from-b";
end;
)";
    const char* main = R"(
module Main;
import Shapes;
import ExtB;
class Program
  public static method Main()
    var s := new Shape();
    Console.WriteLine(s.Describe());
  end Main;
end Program;
)";
    auto [ok, rc, out, err] = compileImportRun4789(dep1, dep2, main, "basic");
    BEZEL_EXPECT_TRUE(ok);
    BEZEL_EXPECT_EQ(rc, 0);
    BEZEL_EXPECT_EQ(out, "from-b\n");
}

// ============================================================================
// T2 — same-module extension (s26_5_0014 sibling) — regression guard.
// ============================================================================
BEZEL_TEST(sema_extension_cross_module_4789,
           same_module_control_accepts) {
    const char* lib = R"(
module Lib;
public class Widget
  public property Size: int;
  public constructor(size: int)
    Size := size;
  end constructor;
end Widget;
public extension Widget
  public method Doubled(): int => this.Size * 2;
end;
)";
    const char* main = R"(
module App;
import Lib;
class Program
  public static method Main()
    using var w := new Widget(6);
    Console.WriteLine(w.Doubled());
  end Main;
end Program;
)";
    auto [ok, rc, out, err] = compileImportRun4789_oneDep(lib, main, "same");
    BEZEL_EXPECT_TRUE(ok);
    BEZEL_EXPECT_EQ(rc, 0);
    BEZEL_EXPECT_EQ(out, "12\n");
}

// ============================================================================
// T3 — calling a method that does NOT exist on any imported extension should
// reject with "no member" diagnostic.
// ============================================================================
BEZEL_TEST(sema_extension_cross_module_4789,
           missing_extension_rejects) {
    const char* dep1 = R"(
module Shapes;
public class Shape
  public constructor() end constructor;
end Shape;
)";
    const char* dep2 = R"(
module ExtB;
import Shapes;
public extension Shape
  public method Describe(): string => "from-b";
end;
)";
    const char* main = R"(
module Main;
import Shapes;
import ExtB;
class Program
  public static method Main()
    var s := new Shape();
    Console.WriteLine(s.Nonexistent());
  end Main;
end Program;
)";
    const auto& dir = wtScratchDir("wt-4789");
    std::string stem = "bz4789_missing_" + unique_stem("");

    TempFile4789 d1Src((dir / (stem + "_d1.bzl")).string());
    TempFile4789 d1Obj((dir / (stem + "_d1.o")).string());
    writeSource4789(d1Src.path, dep1);
    {
        bezel::Driver driver;
        auto r = driver.compileObject(d1Src.path, d1Obj.path);
        BEZEL_EXPECT_TRUE(r.success);
    }

    TempFile4789 d2Src((dir / (stem + "_d2.bzl")).string());
    TempFile4789 d2Obj((dir / (stem + "_d2.o")).string());
    writeSource4789(d2Src.path, dep2);
    {
        bezel::Driver driver;
        std::vector<bezel::Dependency> deps;
        bezel::Dependency d;
        d.metadataPath = d1Obj.path; d.objectPath = d1Obj.path;
        deps.push_back(d);
        auto r = driver.compileObject(d2Src.path, d2Obj.path, deps);
        BEZEL_EXPECT_TRUE(r.success);
    }

    TempFile4789 mainBz((dir / (stem + "_main.bzl")).string());
    TempFile4789 exe((dir / stem).string());
    writeSource4789(mainBz.path, main);

    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d1, d2;
    d1.metadataPath = d1Obj.path; d1.objectPath = d1Obj.path;
    d2.metadataPath = d2Obj.path; d2.objectPath = d2Obj.path;
    deps.push_back(d1);
    deps.push_back(d2);

    auto appR = compileFileForTest(driver, mainBz.path, exe.path, deps);
    BEZEL_EXPECT_FALSE(appR.success);
}

} // namespace bezel::sema
