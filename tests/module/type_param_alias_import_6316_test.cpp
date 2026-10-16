// #6316 (§27.8, §12.1, v0.338): an IMPORTED abstract base whose property type is
// an alias of an alias (`type Id2<T> := Id<T>`) loses the alias-expansion marker
// across the artifact boundary, so the override's value type `Q` must not be
// compared by spelling against the expansion's alias-declaration head. The valid
// override must keep compiling and running. Built like
// tests/module/iface_property_refarg_import_2931_test.cpp.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"
#include "test_harness.hpp"
#include "support/unique_stem.hpp"

namespace bezel::module {
namespace {

struct TempFile6316 {
    std::string path;
    explicit TempFile6316(std::string p) : path(std::move(p)) {}
    ~TempFile6316() { std::remove(path.c_str()); }
};

void writeSource6316(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// -2 = dependency toolchain unavailable; -1 = app did not build / run.
int compileImportAndRun6316(const std::string& libSrc,
                            const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz6316_");
    TempFile6316 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile6316 aObj((dir / (stem + "_a.o")).string());
    writeSource6316(aSrc.path, libSrc);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success) return -2;
    }
    if (!std::ifstream(aObj.path).good()) return -2;
    TempFile6316 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile6316 bExe((dir / stem).string());
    writeSource6316(bSrc.path, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    if (!bezel::test::compileFileForTest(driver, bSrc.path, bExe.path, deps).success) return -1;
    int status = std::system(bExe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

}  // namespace

BEZEL_TEST(type_param_alias_import_6316, imported_alias_of_alias_property_override_accepted_spec_27_8) {
    const char* lib =
        "module AliasLib6316; "
        "type Id<T> := T; "
        "type Id2<T> := Id<T>; "
        "public abstract class B6316<Y> "
        "  public constructor() end constructor; "
        "  public abstract property V: Id2<Y>; "
        "end B6316;";
    int r = compileImportAndRun6316(
        lib,
        "module AliasApp6316; import AliasLib6316; "
        "public class E6316<Q>: B6316<Q> "
        "  public constructor() end constructor; "
        "  public override property V: Q => default(Q); "
        "end E6316; "
        "public class Program6316 "
        "  public static method Main(): int "
        "    var e := new E6316<int>(); "
        "    return e.V + 7; "
        "  end Main; "
        "end Program6316;");
    if (r == -2) return;  // toolchain unavailable
    BEZEL_EXPECT_EQ(r, 7);
}

}  // namespace bezel::module
