// Spec-derived regression test for issue #6704 — the CROSS-MODULE half.
//
// §22.10 (the inheritance clause's base call), §31.4, v0.339. A primary-
// constructor class `class C(msg: string): Exception(msg)` must forward its
// clause argument into the inherited §31.4 Message slot when C is IMPORTED and
// when a LOCAL class derives from it (clause over an imported base). Before the
// fix `Emitter::emitImplicitBase` returned at the null `baseClassOf` and the
// slot stayed empty.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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

struct TempFile6704 {
    std::string path;
    explicit TempFile6704(std::string p) : path(std::move(p)) {}
    ~TempFile6704() { std::remove(path.c_str()); }
};

void writeSource6704(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// -2 = the dependency toolchain is unavailable (test skips); -1 = the app did
// not build/link or did not exit normally. Generated-code ASan is on for the
// importer WITH LeakSanitizer — the leak is this issue, and LSan reports it by
// forcing a non-zero exit, so `exit == 0` IS the under-release assertion. The
// dependency object is compiled without instrumentation (`compileObject` takes
// no sanitize option), which is why the exporter's own OOB write was invisible
// before #2950 and why the layout half had to land first.
int compileImportAndRunAsan6704(const std::string& libSrc,
                                const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz6704_");
    TempFile6704 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile6704 aObj((dir / (stem + "_a.o")).string());
    writeSource6704(aSrc.path, libSrc);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success) return -2;
    }
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile6704 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile6704 bExe((dir / stem).string());
    writeSource6704(bSrc.path, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    codegen::CodegenOptions opts;
    opts.sanitizeAddress = true;
    if (!bezel::test::compileFileForTest(driver, bSrc.path, bExe.path, deps, opts).success)
        return -1;
    int status = std::system(bExe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

}  // namespace

BEZEL_TEST(xmod_primary_ctor_builtin_exception_base_6704,
           imported_primary_ctor_exception_forwards_message_spec_22_10) {
    const char* lib =
        "module ExLib6704; "
        "public class LibErr(msg: string): Exception(msg) end LibErr;";
    int r = compileImportAndRunAsan6704(
        lib,
        "module ExApp6704; import ExLib6704; "
        "public class LocalOver(m: string): LibErr(m) end LocalOver; "
        "public class AppErr(msg: string): Exception(msg) end AppErr; "
        "public class C6704 "
        "  public static method Main(): int "
        "    var t: int := 0; "
        "    var i: int := 0; "
        "    while i < 20 do "
        "      i := i + 1; "
        "      using var e: LibErr := new LibErr(\"m\" + i.ToString()); "
        "      using var l: LocalOver := new LocalOver(\"l\" + i.ToString()); "
        "      using var a: AppErr := new AppErr(\"a\" + i.ToString()); "
        "      t := t + e.Message.Length + l.Message.Length + a.Message.Length; "
        "    end while; "
        // 3 x (9x2 + 11x3) = 3 x 51 = 153
        "    if t <> 153 then return 3; end if; "
        "    return 0; "
        "  end Main; "
        "end C6704;");
    if (r == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 0);
}

}  // namespace bezel::module
