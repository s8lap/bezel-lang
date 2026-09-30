// Cross-module regression rows for #4975 (§22.4 "Inline `out` declarations" /
// §28.9 "Runtime-managed types" / §28.11 "Out-owned recipients" / §12.8
// "Constructor chaining", v0.313): a `base(out using var x)` / `base(out owned
// var x)` chain call whose TARGET constructor lives in an IMPORTED base class
// and declares an `out owned` formal. The single-module rows live in
// tests/codegen/chain_call_out_owned_var_recipient_4975_test.cpp; ef5e6856
// moved the chain-constructor arity / parameter-type reads to imported-safe
// accessors, and these rows pin that the recipient's disposal obligation
// (§28.9) is still discharged when the constructor is only known through
// module metadata (no re-parsed declaration).
//
// The oracle is `Counted.Live()` after 50 constructions: a recipient that is
// never disposed reads 50, a double disposal reads a negative number, and the
// discharged obligation reads 0. A single construction would leave one object
// reachable at exit on either side (one-shot vacuity), hence the loop.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

std::filesystem::path scratch4975() {
    return wtScratchDir("wt-4975-module");
}

struct TempFile4975 {
    std::string path;
    explicit TempFile4975(const std::string& p) : path(p) {}
    ~TempFile4975() { std::remove(path.c_str()); }
};

void writeSource4975(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

std::pair<int, std::string> runCapture4975(const std::string& exe) {
    std::string out;
    FILE* p = popen((exe + " 2>&1").c_str(), "r");
    if (!p) return {-1, out};
    char buf[256];
    while (std::size_t n = std::fread(buf, 1, sizeof(buf), p)) out.append(buf, n);
    int status = pclose(p);
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    return {code, out};
}

// The imported base: `Base`'s constructor hands back a fresh `Counted` through
// an `out owned` formal. `Counted.Live()` is the live-instance count.
const char* kLib4975 =
    "module ChLib4975; "
    "public class Counted "
    "  private static readonly _n: AtomicInteger<int> := "
    "    new AtomicInteger<int>(0); "
    "  public static method Live(): int => _n.Value; "
    "  public constructor() _n.Value := _n.Value + 1; end constructor; "
    "  destructor _n.Value := _n.Value - 1; end destructor; "
    "end Counted; "
    "public class Base "
    "  public constructor(out owned r: Counted) "
    "    r := new Counted(); "
    "  end constructor; "
    "end Base; ";

// Builds the library, then the app against it, runs the app in a separate
// process and returns its exit code and combined output. `code == -2` means
// the library did not build, `-1` the app did not.
std::pair<int, std::string> runApp4975(const std::string& recipientSpelling,
                                       const std::string& tag) {
    auto dir = scratch4975();
    std::string stem = unique_stem(("bz4975_" + tag + "_").c_str());
    TempFile4975 libSrc((dir / (stem + "_lib.bzl")).string());
    TempFile4975 libObj((dir / (stem + "_lib.o")).string());
    writeSource4975(libSrc.path, kLib4975);
    {
        bezel::Driver driver;
        if (!driver.compileObject(libSrc.path, libObj.path).success)
            return {-2, ""};
    }
    TempFile4975 appSrc((dir / (stem + "_app.bzl")).string());
    TempFile4975 appExe((dir / (stem + "_app")).string());
    const std::string app =
        "module ChApp4975" + tag + "; "
        "import ChLib4975; "
        "class Derived : Base "
        "  public constructor() "
        "    base(" + recipientSpelling + "); "
        "  end constructor; "
        "end Derived; "
        "class Program "
        "  public static method Main(): int "
        "    var i := 0; "
        "    while i < 50 do "
        "      var d := new Derived(); "
        "      d.Dispose(); "
        "      i := i + 1; "
        "    end; "
        "    Console.WriteLine(Counted.Live()); "
        "    return 0; "
        "  end Main; "
        "end Program; ";
    writeSource4975(appSrc.path, app);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = libObj.path;
    d.objectPath = libObj.path;
    deps.push_back(d);
    // compileFileForTest so the gate's generated-code ASan sweep reaches this
    // hand-rolled runner too (support/driver_run_support.hpp).
    if (!bezel::test::compileFileForTest(driver, appSrc.path, appExe.path, deps)
             .success)
        return {-1, ""};
    return runCapture4975(appExe.path);
}

}  // namespace

// per §22.4 / §28.9 / §28.11 / §12.8 (v0.313), issue #4975: an `out using var`
// recipient of an imported base constructor's `out owned` formal is disposed
// unconditionally when the derived constructor exits (§28.9).
BEZEL_TEST(chain_call_imported_out_owned_recipient_4975,
           out_using_var_recipient_of_imported_base_ctor_disposed_x50) {
    auto [code, out] = runApp4975("out using var recipient", "u");
    BEZEL_EXPECT_EQ(code, 0);
    BEZEL_EXPECT_EQ(out, std::string("0\n"));
}

// The `out owned var` twin: the recipient is an owned local of the derived
// constructor, so its obligation is discharged at the constructor's exit too
// (§28.11).
BEZEL_TEST(chain_call_imported_out_owned_recipient_4975,
           out_owned_var_recipient_of_imported_base_ctor_disposed_x50) {
    auto [code, out] = runApp4975("out owned var recipient", "o");
    BEZEL_EXPECT_EQ(code, 0);
    BEZEL_EXPECT_EQ(out, std::string("0\n"));
}

}  // namespace bezel::module
