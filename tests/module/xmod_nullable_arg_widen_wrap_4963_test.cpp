// Regression test for issue #4963 — §5.1 "Nullable conversions" (`S → T → T?`)
// at the IMPORTED-callee call arms: an `int` argument bound to a `long?`
// formal of a method declared in another module (§2.5).
//
// The imported-callee arms of lowerCallDispatch (the `.` form and the `?.`
// form) build their own argument list (#1221: no ast::Param to hand to
// buildOrderedArgs) and mirror its nullable-value wrap (#3133). That mirror
// wrapped only an argument whose LLVM type was EXACTLY the payload type, so
// #4922's widen-then-wrap never reached these arms: `XL.F(5)` against an
// imported `F(a: long?)` passed a bare `i32` to a `{ i8, i64 }` parameter.
//
// spec §5.1 "Implicit conversions" (int → long, uint → long) and "Nullable
// conversions" (`S` converts to `T?` via `S → T → T?`), §2.5, §22.6 stage 3,
// v0.307.

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "codegen/LlvmBackend.hpp"
#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"  // the sanitizer-honouring compileFile
#include "test_harness.hpp"
#include "support/unique_stem.hpp"

namespace bezel::module {
namespace {
namespace fs = std::filesystem;

struct TempFile4963 {
    std::string path;
    explicit TempFile4963(std::string p) : path(std::move(p)) {}
    ~TempFile4963() { std::remove(path.c_str()); }
};

void writeSource4963(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

struct XmodRun4963 {
    bool libBuilt = false;
    bool appBuilt = false;
    bool ran = false;
    int exitCode = -1;
    std::string stdoutText;
};

// Build the library to an object, build the app against it (§2.5), run the
// executable and capture its stdout. Every failure is reported in the
// result; nothing is skipped.
XmodRun4963 buildImportRun4963(const std::string& libSrc,
                               const std::string& appSrc) {
    XmodRun4963 out;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz4963_");
    TempFile4963 lSrc((dir / (stem + "_lib.bzl")).string());
    TempFile4963 lObj((dir / (stem + "_lib.o")).string());
    writeSource4963(lSrc.path, libSrc);
    {
        bezel::Driver driver;
        out.libBuilt = driver.compileObject(lSrc.path, lObj.path).success &&
                       std::ifstream(lObj.path).good();
    }
    if (!out.libBuilt) return out;
    TempFile4963 aSrc((dir / (stem + "_app.bzl")).string());
    TempFile4963 aExe((dir / stem).string());
    writeSource4963(aSrc.path, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = lObj.path;
    d.objectPath = lObj.path;
    deps.push_back(d);
    codegen::CodegenOptions opts;
    out.appBuilt = bezel::test::compileFileForTest(driver, aSrc.path, aExe.path,
                                                   deps, opts).success;
    if (!out.appBuilt) return out;
    std::string cmd = aExe.path + " 2>/dev/null";
    FILE* f = popen(cmd.c_str(), "r");
    if (!f) return out;
    std::array<char, 256> buf{};
    while (true) {
        std::size_t n = std::fread(buf.data(), 1, buf.size(), f);
        if (n == 0) break;
        out.stdoutText.append(buf.data(), n);
    }
    int rc = pclose(f);
    if (rc != -1 && WIFEXITED(rc)) {
        out.ran = true;
        out.exitCode = WEXITSTATUS(rc);
    }
    return out;
}

const char* kLib4963 =
    "module XLib4963; "
    "public class XL4963 "
    "  public constructor() end constructor; "
    "  public static method F(a: long?): long return a ?? -1; end F; "
    "  public method G(a: long?): long return a ?? -2; end G; "
    "end XL4963;";

}  // namespace

// per §5.1 / §2.5 (v0.307), fix #4963 — an `int` literal, a negative `int`
// local and a `uint` above 2^31 (zero-extended) at an imported static
// method's `long?` formal (the `.` imported-callee arm); an `int` at an
// imported instance method's `long?` formal through `.` and through `?.`
// (the `?.` imported-callee arm).
BEZEL_TEST(xmod_nullable_arg_widen_wrap_4963,
           imported_int_at_long_nullable_dot_and_nullcond_5_1) {
    auto r = buildImportRun4963(
        kLib4963,
        "module XApp4963; import XLib4963; "
        "class P "
        "  public static method Main(): int "
        "    var n: int := -9; var u: uint := 4000000000; "
        "    Console.WriteLine(XL4963.F(5)); "
        "    Console.WriteLine(XL4963.F(n)); "
        "    Console.WriteLine(XL4963.F(u)); "
        "    using var x := new XL4963(); "
        "    Console.WriteLine(x.G(6)); "
        "    var y: XL4963? := x; "
        "    Console.WriteLine(y?.G(n) ?? 0); "
        "    return 0; "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_TRUE(r.libBuilt);
    BEZEL_EXPECT_TRUE(r.appBuilt);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("5\n-9\n4000000000\n6\n-9\n"));
}

}  // namespace bezel::module
