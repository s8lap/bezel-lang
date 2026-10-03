// fix #5306 (§14.1 "Generic declarations" / §27.6 "Method references" /
// §27.7.1 "Equality of function values", v0.313), the CROSS-MODULE face:
// a property of an IMPORTED generic class referenced in function-value
// position at two instantiations whose layouts differ.
//
// `makePropertyThunk` builds the `pthunk$N` in the IMPORTING module, against
// the importer's own `class.Holder$specN` for each instantiation, and used to
// memoize it per `sema::PropertySymbol*` — so the first instantiation's
// thunk served the second. The property here is the `int`-typed sibling `w`:
// its thunk SIGNATURE is `i32 (ptr)` at every instantiation, so only the BODY
// differs (`w` sits at byte 12 in `Holder<int>`'s {ptr, i8, i32, i32} and at
// byte 24 in `Holder<string>`'s {ptr, i8, ptr, i32}); a memo keyed by the
// thunk's function type alone would still be wrong here.
//
// MEASURED (bezelc `--import`, `BEZEL_DUMP_IR=1`): post-fix two `pthunk`s and
// `11` / `22`. On ef32951c: one `pthunk`, `11` then a garbage value (the upper
// half of `v`'s string pointer). The `T`-typed `v` itself is not used: an
// imported generic class's T-typed property is rejected at the access today
// ("not yet monomorphized", #2884), which is unrelated to this fix.

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {
namespace fs = std::filesystem;

struct TempFile5306 {
    std::string path;
    explicit TempFile5306(std::string p) : path(std::move(p)) {}
    ~TempFile5306() { std::remove(path.c_str()); }
};

void writeSource5306(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// -2: the library did not build (toolchain); -1: the app did not build or
// the program did not exit normally; otherwise the exit status.
int compileImportRunCapture5306(const std::string& libSrc,
                                const std::string& appSrc,
                                std::string* outStdout) {
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz5306_");
    TempFile5306 lSrc((dir / (stem + "_lib.bzl")).string());
    TempFile5306 lObj((dir / (stem + "_lib.o")).string());
    writeSource5306(lSrc.path, libSrc);
    bezel::Driver driver;
    if (!driver.compileObject(lSrc.path, lObj.path).success) return -2;
    if (!std::ifstream(lObj.path).good()) return -2;

    TempFile5306 aSrc((dir / (stem + "_app.bzl")).string());
    TempFile5306 aExe((dir / stem).string());
    writeSource5306(aSrc.path, appSrc);
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = lObj.path;
    d.objectPath = lObj.path;
    deps.push_back(d);
    if (!bezel::test::compileFileForTest(driver, aSrc.path, aExe.path, deps)
             .success)
        return -1;

    std::string cmd = aExe.path + " 2>/dev/null";
    FILE* f = popen(cmd.c_str(), "r");
    if (!f) return -1;
    std::array<char, 256> buf{};
    std::string captured;
    while (true) {
        std::size_t n = std::fread(buf.data(), 1, buf.size(), f);
        if (n == 0) break;
        captured.append(buf.data(), n);
    }
    int rc = pclose(f);
    if (outStdout) *outStdout = captured;
    if (rc == -1 || !WIFEXITED(rc)) return -1;
    return WEXITSTATUS(rc);
}

const char* kLib5306 =
    "module HLib5306;\n"
    "public class Holder<T>\n"
    "  public property v: T;\n"
    "  public property w: int;\n"
    "  public constructor(x: T, y: int)\n"
    "    v := x;\n"
    "    w := y;\n"
    "  end constructor;\n"
    "end Holder;\n";

}  // namespace

BEZEL_TEST(xmod_generic_property_thunk_5306,
           imported_generic_sibling_property_thunk_per_instantiation) {
    std::string out;
    int rc = compileImportRunCapture5306(
        kLib5306,
        "module HApp5306;\n"
        "import HLib5306;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    var hi := new Holder<int>(3, 11);\n"
        "    var wi: () -> int := hi.w;\n"
        "    var hs := new Holder<string>(\"z\", 22);\n"
        "    var ws: () -> int := hs.w;\n"
        "    Console.WriteLine(wi());\n"
        "    Console.WriteLine(ws());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n",
        &out);
    BEZEL_EXPECT_TRUE(rc != -2);  // the library must build: no silent skip
    BEZEL_EXPECT_EQ(rc, 0);
    BEZEL_EXPECT_EQ(out, std::string("11\n22\n"));
}

}  // namespace bezel::module
