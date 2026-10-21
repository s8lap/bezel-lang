// Spec-derived regression test for issue #6741, cross-module half.
//
// §22.10 (v0.340): a primary-ctor class "chains to `base(...)` before executing
// field initializers". Here the ROOT primary-ctor class (user or `Exception`
// based) is IMPORTED and the explicit-ctor class `CL` and the primary-ctor
// class `DL` over it are LOCAL: `DL`'s construction reaches `CL`'s constructor
// through the inheritance clause, whose `base(x + "!")` must store the
// imported root's positional slot. (Imported EXPLICIT-ctor middle classes are a
// separate, pre-existing defect: #6743.)

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

struct TempFile6741 {
    std::string path;
    explicit TempFile6741(std::string p) : path(std::move(p)) {}
    ~TempFile6741() { std::remove(path.c_str()); }
};

void writeSource6741(const std::string& path, const std::string& src) {
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
int compileImportAndRunAsan6741(const std::string& libSrc,
                                const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz6741_");
    TempFile6741 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile6741 aObj((dir / (stem + "_a.o")).string());
    writeSource6741(aSrc.path, libSrc);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success) return -2;
    }
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile6741 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile6741 bExe((dir / stem).string());
    writeSource6741(bSrc.path, appSrc);
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

BEZEL_TEST(xmod_chain_through_explicit_ctor_primary_base_6741,
           imported_root_local_explicit_and_primary_classes_spec_22_10) {
    const char* lib =
        "module RootLib6741; "
        "public class A(m: string) "
        "  public Msg: string := m; "
        "end A; "
        "public class AE(m: string): Exception(m) end AE;";
    int r = compileImportAndRunAsan6741(
        lib,
        "module RootApp6741; import RootLib6741; "
        "public class CL: A "
        "  public constructor(x: string) base(x + \"!\"); end constructor; "
        "end CL; "
        "public class DL(m: string): CL(m) end DL; "
        "public class CE: AE "
        "  public constructor(x: string) base(x + \"?\"); end constructor; "
        "end CE; "
        "public class DE(m: string): CE(m) end DE; "
        "public class C6741 "
        "  public static method Main(): int "
        "    var t: int := 0; "
        "    var i: int := 0; "
        "    while i < 20 do "
        "      i := i + 1; "
        "      using var d: DL := new DL(\"d\" + i.ToString()); "
        "      t := t + d.Msg.Length; "
        "      try throw new DE(\"e\" + i.ToString()); "
        "      catch var x: AE => t := t + x.Message.Length; "
        "      end; "
        "    end while; "
        // DL: "d<i>!" = 3 (i<10) / 4 ; DE: "e<i>?" same => 2 x (9x3 + 11x4) = 142
        "    if t <> 142 then return 3; end if; "
        "    return 0; "
        "  end Main; "
        "end C6741;");
    if (r == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 0);
}

}  // namespace bezel::module
