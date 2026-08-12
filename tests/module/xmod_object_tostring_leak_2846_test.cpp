// Spec-derived regression test for issue #2846 — the CROSS-MODULE half.
//
// §8.5 (`ToString`) / §8.6 (Boxing) / §2.5 (imported artifacts) / §28.9
// (Runtime-managed types), v0.240.
//
// The single-module half lives in
// tests/codegen/object_tostring_result_leak_2846_test.cpp; this file covers the
// one shape that needs a real two-module build to observe, and it is not a mere
// duplicate: the first draft of the fix, gated only on the `P::Object`
// PRIMITIVE receiver type, fixed every single-module shape and left this one
// leaking. In a module that IMPORTS another, `var o: Object := …` resolves to a
// **Named** TypeRef carrying the loaded `Object` TypeSymbol rather than the
// `P::Object` primitive, so the primitive-only gate never matched.
//
// MEASURED with a real two-module build (`bezelc -c lib.bzl` then
// `bezelc --sanitize=address --import lib.o app.bzl`), 20 iterations:
//   pre-fix                962 bytes leaked in 40 allocations (bz$boxtostr$Pt)
//   primitive-only gate    962 bytes leaked in 40 allocations  (unchanged!)
//   shipped fix            clean, and byte-identical output to the same program
//                          written as a single module
//
// The programs are LOOP-AMPLIFIED with a churn allocation for the reason the
// codegen half documents: a one-shot `Main` is not a reliable LSan oracle.
// Generated-code ASan/LSan is enabled on the importer through
// `CodegenOptions::sanitizeAddress`, and LeakSanitizer forces the process exit
// code to 1 on any leak — so `exit == 0` IS the leak assertion here.
//
// Related: #2796 (imported record/enum boxing — what made an imported record
// reach a box thunk at all, and so what makes this shape newly reachable).

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

namespace bezel::module {
namespace {

struct TempFile2846 {
    std::string path;
    explicit TempFile2846(std::string p) : path(std::move(p)) {}
    ~TempFile2846() { std::remove(path.c_str()); }
};

void writeSource2846(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// -2 = the dependency toolchain is unavailable (test skips); -1 = the app did
// not build or did not exit normally. Mirrors the #2836 sibling helper, with
// generated-code ASan/LSan turned on for the importer (this test's whole point
// is the leak, and LSan reports it by forcing a non-zero exit).
int compileImportAndRunAsan2846(const std::string& libSrc,
                                const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2846_" + std::to_string(std::rand());
    TempFile2846 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2846 aObj((dir / (stem + "_a.o")).string());
    writeSource2846(aSrc.path, libSrc);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success) return -2;
    }
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile2846 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2846 bExe((dir / stem).string());
    writeSource2846(bSrc.path, appSrc);
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

// ===========================================================================
// T1 — an IMPORTED record boxed into `Object`, `ToString()` in a 20-iteration
// loop. §8.6: "When a value type is used where `Object` … is expected, the
// value is boxed … Boxed values are runtime-managed (§28.9)." The produced
// string is runtime-managed too and must be reclaimed. Pre-fix: 962 bytes in 40
// allocations from `bz$boxtostr$Pt`, so LSan forced exit 1.
//
// The program also verifies the RENDERING (§8.5's positional-record form) so a
// "fix" that stopped producing the string would not pass: t is the exact sum of
// the rendered lengths.
// ===========================================================================
BEZEL_TEST(xmod_object_tostring_leak_2846, imported_record_box_tostring) {
    const char* lib =
        "module TsLib2846T1; "
        "public record Pt2846T1(X: int, Y: int); ";
    int r = compileImportAndRunAsan2846(
        lib,
        "module App2846T1; import TsLib2846T1; "
        "public class C2846T1 "
        "  public static method Main(): int "
        "    var t: int := 0; "
        "    var i: int := 0; "
        "    while i < 20 do "
        "      i := i + 1; "
        "      var p: Pt2846T1 := new Pt2846T1(i, i); "
        "      var o: Object := p; "
        "      var s: string := o.ToString(); "
        "      var churn: string := \"c\" + i.ToString(); "
        "      t := t + s.Length + churn.Length; "
        "    end while; "
        "    if t <> 473 then return 3; end if; "
        "    return 0; "
        "  end Main; "
        "end C2846T1;");
    if (r == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 0);
}

// ===========================================================================
// T2 — the IMPORTED ENUM sibling. §8.5: "for enums: the type's qualified name"
// — an immortal literal, so this one never leaked and must not now
// OVER-release: a bare release on a sentinel is a no-op by construction
// (bz_string_release returns early on BZ_STRING_RC_SENTINEL), and this pins
// that the widened recognition did not disturb it. The imported CLASS default
// is here for the same reason.
// ===========================================================================
BEZEL_TEST(xmod_object_tostring_leak_2846, imported_enum_and_class_defaults) {
    const char* lib =
        "module TsLib2846T2; "
        "public enum Color2846T2 Red, Green; end Color2846T2; "
        "public class Plain2846T2 public N: int := 0; end Plain2846T2; ";
    int r = compileImportAndRunAsan2846(
        lib,
        "module App2846T2; import TsLib2846T2; "
        "public class C2846T2 "
        "  public static method Main(): int "
        "    var t: int := 0; "
        "    var i: int := 0; "
        "    while i < 20 do "
        "      i := i + 1; "
        "      using var k := new Plain2846T2(); "
        "      var oe: Object := Color2846T2.Green; "
        "      var ok: Object := k; "
        "      var churn: string := \"c\" + i.ToString(); "
        "      t := t + oe.ToString().Length + ok.ToString().Length "
        "             + churn.Length; "
        "    end while; "
        "    if t < 1 then return 3; end if; "
        "    return 0; "
        "  end Main; "
        "end C2846T2;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 0);
}

}  // namespace bezel::module
