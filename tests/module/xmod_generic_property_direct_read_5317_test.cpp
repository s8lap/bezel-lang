// fix #5317 (§14.1 / §2.8.3 rule 4 / §23.1, v0.313): a direct read of a
// non-T sibling auto-property of an IMPORTED generic class at a layout-moving
// instantiation returned a wrong value. Sema's #2884 decline admitted the access
// because the property's own type does not mention T (§2884 arm 3, relaxed by
// #2945/#3365), with the stated premise that the access is layout-safe or
// monomorphized per instantiation. Codegen still called the exporter's erased
// exported accessor ($propget stub), which GEPs the template layout (T lowered
// as the i32 placeholder). At Holder<string>, w sits at a different offset than
// the template layout, so the read returned pointer bits.
//
// MEASURED (bezelc --import, r23-c28): hi.w correct (11), hs.w wrong
// (4442640 garbage, not 22). Post-fix: 11 and 22. The thunk path
// (var ws := hs.w, tested by xmod_generic_property_thunk_5306_test) was
// already correct; this is the direct-read twin.

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

struct TempFile5317 {
    std::string path;
    explicit TempFile5317(std::string p) : path(std::move(p)) {}
    ~TempFile5317() { std::remove(path.c_str()); }
};

void writeSource5317(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// -2: the library did not build (toolchain); -1: the app did not build or
// the program did not exit normally; otherwise the exit status.
int compileImportRunCapture5317(const std::string& libSrc,
                                const std::string& appSrc,
                                std::string* outStdout) {
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz5317_");
    TempFile5317 lSrc((dir / (stem + "_lib.bzl")).string());
    TempFile5317 lObj((dir / (stem + "_lib.o")).string());
    writeSource5317(lSrc.path, libSrc);
    bezel::Driver driver;
    if (!driver.compileObject(lSrc.path, lObj.path).success) return -2;
    if (!std::ifstream(lObj.path).good()) return -2;

    TempFile5317 aSrc((dir / (stem + "_app.bzl")).string());
    TempFile5317 aExe((dir / stem).string());
    writeSource5317(aSrc.path, appSrc);
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

const char* kLib5317 =
    "module HLib5317;\n"
    "public class Holder<T>\n"
    "  public property v: T;\n"
    "  public property w: int;\n"
    "  public constructor(x: T, y: int)\n"
    "    v := x;\n"
    "    w := y;\n"
    "  end constructor;\n"
    "end Holder;\n";

}  // namespace

BEZEL_TEST(xmod_generic_property_direct_read_5317,
           imported_generic_sibling_property_direct_read_per_instantiation) {
    std::string out;
    int rc = compileImportRunCapture5317(
        kLib5317,
        "module HApp5317;\n"
        "import HLib5317;\n"
        "public class Program\n"
        "  public static method Main(): int\n"
        "    var hi := new Holder<int>(3, 11);\n"
        "    var hs := new Holder<string>(\"z\", 22);\n"
        "    Console.WriteLine(hi.w);\n"
        "    Console.WriteLine(hs.w);\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Program;\n",
        &out);
    BEZEL_EXPECT_TRUE(rc != -2);  // the library must build: no silent skip
    BEZEL_EXPECT_EQ(rc, 0);
    BEZEL_EXPECT_EQ(out, std::string("11\n22\n"));
}

}  // namespace bezel::module