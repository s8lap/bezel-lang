// Spec-derived regression test for issue #6074 — the CROSS-MODULE half.
//
// §12.1 / §27.8 (overload resolution and override matching by parameter
// signature), v0.338: a virtual method or indexer whose parameters differ only
// in generic ARGUMENTS (`Array<int>` vs `Array<string>`) is two overloads, each
// with its own vtable slot. The single-module rows live in
// tests/codegen/generic_argument_slot_key_6074_test.cpp.
//
// This row is the imported side: the library `Lib` declares the overload pair
// and is compiled to an object with `Driver::compileObject`; the importer
// overrides only the `Array<int>` member and calls both overloads through a
// base-typed receiver. Expected output is `20 11 2 5 1`:
//   Foo(Array<int>)     -> 20 (override)
//   Foo(Array<string>)  -> 11 (base, its own slot)
//   [Array<int>]        -> 2  (override)
//   [Array<string>]     -> 5  (base, its own slot)
//   Bar(int)            -> 1  (non-generic control)
// Stdout is captured to a file and compared exactly; no exit-code oracle.
//
// ABI NOTE (dc ruling, #6074): a library compiled before this fix has collided
// slots for generic-argument overloads, so it must be rebuilt before it is
// imported by a post-fix compiler. This row always builds both sides fresh.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "codegen/LlvmBackend.hpp"
#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"  // #3156: the ONE sanitizer-honouring compileFile wrapper
#include "test_harness.hpp"
#include "support/unique_stem.hpp"

namespace bezel::module {
namespace {

struct TempFile6074 {
    std::string path;
    explicit TempFile6074(std::string p) : path(std::move(p)) {}
    ~TempFile6074() { std::remove(path.c_str()); }
};

void writeSource6074(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Builds the library object and the importer (generated-code ASan on for the
// importer), runs the importer and returns its captured stdout. Returns false
// with `err` set when a build step fails.
bool compileImportAndRun6074(const std::string& libSrc, const std::string& appSrc,
                             std::string& stdoutText, std::string& err) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz6074_");
    TempFile6074 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile6074 aObj((dir / (stem + "_a.o")).string());
    writeSource6074(aSrc.path, libSrc);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success) {
            err = "library compileObject failed";
            return false;
        }
    }

    TempFile6074 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile6074 bExe((dir / stem).string());
    TempFile6074 outFile((dir / (stem + "_out.txt")).string());
    writeSource6074(bSrc.path, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    codegen::CodegenOptions opts;
    opts.sanitizeAddress = true;
    if (!bezel::test::compileFileForTest(driver, bSrc.path, bExe.path, deps, opts).success) {
        err = "importer compileFile failed";
        return false;
    }
    std::string cmd = bExe.path + " > " + outFile.path;
    int status = std::system(cmd.c_str());
    if (status == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        err = "importer did not exit 0";
        return false;
    }
    std::ifstream in(outFile.path);
    std::ostringstream ss;
    ss << in.rdbuf();
    stdoutText = ss.str();
    return true;
}

}  // namespace

// §12.1 / §27.8, v0.338: cross-module generic-argument overloads. Pre-fix (both
// sides on the collided slot keys) the output is not `20 11 2 5 1`.
BEZEL_TEST(generic_argument_slot_key_6074_module,
           two_module_generic_argument_overloads_12_1_v0338) {
    const char* lib =
        "module Lib; "
        "public class B "
        "  public virtual method Foo(x: Array<int>): int "
        "    return 10; "
        "  end Foo; "
        "  public virtual method Foo(x: Array<string>): int "
        "    return 11; "
        "  end Foo; "
        "  public virtual method Bar(x: int): int "
        "    return 1; "
        "  end Bar; "
        "  public virtual property [k: Array<int>]: int "
        "    get => 1; "
        "  end; "
        "  public virtual property [k: Array<string>]: int "
        "    get => 5; "
        "  end; "
        "end B; ";
    const char* app =
        "module App; import Lib; "
        "public class Dv: B "
        "  public override method Foo(x: Array<int>): int "
        "    return 20; "
        "  end Foo; "
        "  public override property [k: Array<int>]: int "
        "    get => 2; "
        "  end; "
        "end Dv; "
        "public class M "
        "  public static method Main() "
        "    var b: B := new Dv(); "
        "    var a := new Array<int>(1); "
        "    var s := new Array<string>(1); "
        "    Console.WriteLine(b.Foo(a).ToString()); "
        "    Console.WriteLine(b.Foo(s).ToString()); "
        "    Console.WriteLine(b[a].ToString()); "
        "    Console.WriteLine(b[s].ToString()); "
        "    Console.WriteLine(b.Bar(3).ToString()); "
        "  end Main; "
        "end M;";
    std::string out, err;
    BEZEL_EXPECT_TRUE(compileImportAndRun6074(lib, app, out, err));
    BEZEL_EXPECT_EQ(out, std::string("20\n11\n2\n5\n1\n"));
}

}  // namespace bezel::module
