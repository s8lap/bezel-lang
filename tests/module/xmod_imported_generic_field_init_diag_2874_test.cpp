// #2874 (§2.8.2 "No restrictions beyond visibility" / §17.14, v0.240) — the
// DIAGNOSTIC half. #2860 rejects the construction of an imported generic whose
// instance field initializer has no carriage in the artifact interface, which is
// what §2.8.2 sanctions:
//
//     "An implementation unable to carry some body shape in its interface
//      format must reject the export, or the import, with a diagnostic naming
//      the shape; it must not alter the instantiation's semantics."
//
// #2874 (carry AND RUN) lands the strictly better outcome: most initializers
// are now reconstructed at LOAD time and run importer-side, leaving the §2.8.2
// construction-site diagnostic as the FALLBACK for shapes the reconstruction
// genuinely cannot handle (a parse/sema failure). This file exercises the
// diagnostic against a deliberately-uncarriable fixture: an initializer that
// references an INTERNAL helper class — the helper is visible to the
// library's own compile (the field initializer text is exported verbatim) but
// NOT in the metadata (only public types are exported, §13.2), so the
// importer's synthetic-source resolveTypes fails and the diagnostic fires.
//
// What is fixed here is the diagnostic itself, on two counts that a user meets
// before ever meeting the limitation:
//
//  (1) IT WAS UNLOCATED ON THE ORDINARY PATH. `importedFieldInitDiagLoc_` was
//      set only on `lowerNew`'s no-explicit-ctor branch. An imported generic
//      WITH an explicit constructor reaches `emitOwnFieldInit` through
//      `inlineCtorBody`, so the diagnostic came out at `0:0`. MEASURED on
//      2ddd5b3, the same uncarriable initializer twice: `app.bzl:5:20:` for the
//      no-ctor spelling, `app.bzl:0:0:` for the explicit-ctor one. An explicit
//      constructor is the ordinary shape, so the LOCATED case was the exception.
//
//  (2) IT ASSERTED SOMETHING FALSE. The text claimed "only a compile-time-
//      constant initializer crosses the module boundary and this one does not".
//      §17.14's own list of constant expressions admits "integer, floating-point,
//      and character literals" and "the binary arithmetic (`+`, `-`, `*`, `/`,
//      `mod`) ... operators applied to constant operands of primitive or `string`
//      type", so `:= 1.5 + 0.5` IS a compile-time-constant expression — it simply
//      does not FOLD on the carriage path, because #2898's fold runs through
//      `sema::foldIntConst` and is integer-only. The message now says what is
//      true (it did not fold) and names the issue that tracks the difference.
//
// Neither change relaxes the rejection: every row below that is still rejected
// by the fallback path, and the carriable-initializer row still builds and runs.

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "driver/Driver.hpp"
#include "test_harness.hpp"
#include "support/unique_stem.hpp"


namespace bezel::module {
namespace {

struct TempFile2874 {
    std::string path;
    explicit TempFile2874(std::string p) : path(std::move(p)) {}
    ~TempFile2874() { std::remove(path.c_str()); }
};

void writeSource2874(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Compile B importing A, capturing the driver's stderr (fd 2) into `err` —
// same shape as the #2860 test's helper, because the property under test IS the
// diagnostic text and its location, which a bare success/failure flag cannot
// see. Returns the compile's success flag.
bool compileImportCaptureErr2874(const std::string& bSrc,
                                 const std::string& bExe,
                                 const std::string& aObj, std::string& err) {
    namespace fs = std::filesystem;
    std::string errPath =
        (fs::temp_directory_path() /
         ("bz2874_err_" + unique_stem("") + ".txt"))
            .string();

    std::fflush(stderr);
    std::cerr.flush();
    int saved = ::dup(STDERR_FILENO);
    int fd = ::open(errPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    bool success = false;
    if (fd >= 0) ::dup2(fd, STDERR_FILENO);
    {
        bezel::Driver driver;
        std::vector<bezel::Dependency> deps;
        bezel::Dependency d;
        d.metadataPath = aObj;
        d.objectPath = aObj;
        deps.push_back(d);
        success = driver.compileFile(bSrc, bExe, deps).success;
    }
    std::fflush(stderr);
    std::cerr.flush();
    if (fd >= 0) {
        ::close(fd);
        ::dup2(saved, STDERR_FILENO);
    }
    ::close(saved);

    std::ifstream in(errPath);
    err.assign((std::istreambuf_iterator<char>(in)),
               std::istreambuf_iterator<char>());
    std::remove(errPath.c_str());
    return success;
}

bool buildLib2874(const std::string& stem, const std::string& libSrc,
                  std::string& objOut) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string src = (dir / (stem + "_a.bzl")).string();
    objOut = (dir / (stem + "_a.o")).string();
    writeSource2874(src, libSrc);
    bezel::Driver driver;
    bool ok = driver.compileObject(src, objOut).success;
    std::remove(src.c_str());
    return ok;
}

// The app sources below are written with explicit newlines so the LINE the
// diagnostic must name is fixed and readable: the construction is always on
// line 5 unless a row says otherwise.
std::string app2874(const std::string& moduleName, const std::string& lib,
                    const std::string& body) {
    return "module " + moduleName + ";\n" +          // 1
           "import " + lib + ";\n" +                 // 2
           "public class C\n" +                      // 3
           "  public static method Main(): int\n" +  // 4
           body +                                    // 5...
           "  end Main;\n"
           "end C;\n";
}

}  // namespace

// --- (1) the construction site must be LOCATED ------------------------------

// The row the fix is for: the imported generic has an EXPLICIT CONSTRUCTOR, so
// construction runs through `inlineCtorBody`. Before the fix the diagnostic came
// out at `0:0`; it must now name the `new`'s own line. The fixture uses an
// internal helper class as the uncarriable element so the diagnostic still
// fires post-#2874 (most shapes now RUN importer-side; only genuine
// reconstruction failures fall through to the diagnostic).
BEZEL_TEST(xmod_imported_generic_field_init_diag_2874,
           explicit_ctor_construction_site_is_located_spec_2_8_2) {
    std::string stem = unique_stem("bz2874_loc_");
    std::string aObj;
    BEZEL_EXPECT_TRUE(buildLib2874(stem,
        "module Lib2874A; "
        "internal class InternalA2874 "
        "  public static method foo(): owned Array<int> => new Array<int>(); "
        "end InternalA2874; "
        "public class GA2874<T> "
        "  public v: T; public owned items: Array<int> := InternalA2874.foo(); "
        "  public constructor(x: T) v := x; end constructor; end GA2874;",
        aObj));
    TempFile2874 aObjFile(aObj);
    if (!std::ifstream(aObj).good()) return;

    auto dir = std::filesystem::temp_directory_path();
    TempFile2874 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2874 bExe((dir / (stem + "_b")).string());
    writeSource2874(bSrc.path,
        app2874("App2874A", "Lib2874A",
                "    using var g := new GA2874<int>(1);\n"
                "    return 0;\n"));

    std::string err;
    bool ok = compileImportCaptureErr2874(bSrc.path, bExe.path, aObj, err);
    // Still rejected — this fix sharpens the diagnostic, it does not relax it.
    BEZEL_EXPECT_TRUE(!ok);
    BEZEL_EXPECT_TRUE(err.find("cannot run the initializer of its field") !=
                      std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("items") != std::string::npos);
    // LOCATED on the `new`: line 5, column 20 of the app source above.
    BEZEL_EXPECT_TRUE(err.find(".bzl:5:20:") != std::string::npos);
    // and specifically NOT the unlocated default this row used to produce.
    BEZEL_EXPECT_TRUE(err.find(".bzl:0:0:") == std::string::npos);
}

// CONTROL, passes both ways: the no-explicit-ctor spelling was already located
// (its branch set the location), and must stay so. Together with the row above
// this is what shows the axis was "does the imported generic have an explicit
// constructor", not "is the initializer uncarriable". Same Internal-helper
// fixture so the diagnostic still fires.
BEZEL_TEST(xmod_imported_generic_field_init_diag_2874,
           no_ctor_construction_site_stays_located_spec_2_8_2) {
    std::string stem = unique_stem("bz2874_noctor_");
    std::string aObj;
    BEZEL_EXPECT_TRUE(buildLib2874(stem,
        "module Lib2874B; "
        "internal class InternalB2874 "
        "  public static method foo(): owned Array<int> => new Array<int>(); "
        "end InternalB2874; "
        "public class GB2874<T> "
        "  public v: T?; public owned items: Array<int> := InternalB2874.foo(); "
        "end GB2874;",
        aObj));
    TempFile2874 aObjFile(aObj);
    if (!std::ifstream(aObj).good()) return;

    auto dir = std::filesystem::temp_directory_path();
    TempFile2874 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2874 bExe((dir / (stem + "_b")).string());
    writeSource2874(bSrc.path,
        app2874("App2874B", "Lib2874B",
                "    using var g := new GB2874<string>();\n"
                "    return 0;\n"));

    std::string err;
    bool ok = compileImportCaptureErr2874(bSrc.path, bExe.path, aObj, err);
    BEZEL_EXPECT_TRUE(!ok);
    BEZEL_EXPECT_TRUE(err.find(".bzl:5:20:") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find(".bzl:0:0:") == std::string::npos);
}

// The location is per-CONSTRUCTION, not per-compilation. Two `new`s of two
// imported generics, one carriable and one not, on two different lines — and the
// carriable one comes FIRST, so a location that leaked from the previous
// construction (or was never restored) would name the wrong line. Run in both
// orders so neither a stale nor a never-set location can pass. The "Bad"
// generic uses an Internal-helper fixture so the diagnostic fires post-#2874;
// the "Ok" generic uses a carriable literal so its construction succeeds and
// only the second construction's diagnostic is asserted.
BEZEL_TEST(xmod_imported_generic_field_init_diag_2874,
           each_construction_gets_its_own_location_spec_2_8_2) {
    std::string stem = unique_stem("bz2874_two_");
    std::string aObj;
    BEZEL_EXPECT_TRUE(buildLib2874(stem,
        "module Lib2874C; "
        "internal class InternalC2874 "
        "  public static method foo(): owned Array<int> => new Array<int>(); "
        "end InternalC2874; "
        "public class Ok2874<T> public v: T; public tag: int := 107; "
        "  public constructor(x: T) v := x; end constructor; end Ok2874; "
        "public class Bad2874<T> public v: T; "
        "  public owned items: Array<int> := InternalC2874.foo(); "
        "  public constructor(x: T) v := x; end constructor; end Bad2874;",
        aObj));
    TempFile2874 aObjFile(aObj);
    if (!std::ifstream(aObj).good()) return;

    auto dir = std::filesystem::temp_directory_path();
    {
        TempFile2874 bSrc((dir / (stem + "_ok_first.bzl")).string());
        TempFile2874 bExe((dir / (stem + "_ok_first")).string());
        writeSource2874(bSrc.path,
            app2874("App2874C1", "Lib2874C",
                    "    using var a := new Ok2874<int>(1);\n"     // line 5
                    "    using var b := new Bad2874<int>(2);\n"    // line 6
                    "    return a.tag;\n"));
        std::string err;
        BEZEL_EXPECT_TRUE(!compileImportCaptureErr2874(bSrc.path, bExe.path,
                                                       aObj, err));
        BEZEL_EXPECT_TRUE(err.find(".bzl:6:20:") != std::string::npos);
        BEZEL_EXPECT_TRUE(err.find(".bzl:5:20:") == std::string::npos);
    }
    {
        TempFile2874 bSrc((dir / (stem + "_bad_first.bzl")).string());
        TempFile2874 bExe((dir / (stem + "_bad_first")).string());
        writeSource2874(bSrc.path,
            app2874("App2874C2", "Lib2874C",
                    "    using var b := new Bad2874<int>(2);\n"    // line 5
                    "    using var a := new Ok2874<int>(1);\n"     // line 6
                    "    return a.tag;\n"));
        std::string err;
        BEZEL_EXPECT_TRUE(!compileImportCaptureErr2874(bSrc.path, bExe.path,
                                                       aObj, err));
        BEZEL_EXPECT_TRUE(err.find(".bzl:5:20:") != std::string::npos);
        BEZEL_EXPECT_TRUE(err.find(".bzl:6:20:") == std::string::npos);
    }
}

// --- (2) the diagnostic must not assert something false ---------------------

// `:= 1.5 + 0.5` IS a §17.14 compile-time-constant expression by that section's
// own list; it is the CARRIAGE that cannot fold it (#2898's fold is
// integer-only). Pre-#2874 the diagnostic said "not a compile-time constant"
// (false). Post-#2874, with the carriage half working, the construction
// SUCCEEDS for this exact shape — lowerExpr on the reconstructed AST emits
// `fadd` and the slot stores 2.0. So this row tests the same shape's NEW
// behaviour: the field reads 2.0, not the §7.9 default. The diagnostic's
// "did not fold" text is preserved (it now names a strictly smaller failure
// surface — only genuine parse/sema failures reach it), but the fold-only
// limitation is no longer one of them.
BEZEL_TEST(xmod_imported_generic_field_init_diag_2874,
           unfoldable_constant_expression_now_runs_spec_17_14) {
    std::string stem = unique_stem("bz2874_fold_");
    std::string aObj;
    BEZEL_EXPECT_TRUE(buildLib2874(stem,
        "module Lib2874D; public class GD2874<T> "
        "  public v: T; public d: double := 1.5 + 0.5; "
        "  public constructor(x: T) v := x; end constructor; end GD2874;",
        aObj));
    TempFile2874 aObjFile(aObj);
    if (!std::ifstream(aObj).good()) return;

    auto dir = std::filesystem::temp_directory_path();
    TempFile2874 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2874 bExe((dir / (stem + "_b")).string());
    writeSource2874(bSrc.path,
        app2874("App2874D", "Lib2874D",
                "    using var g := new GD2874<int>(1);\n"
                "    if g.d <> 2.0 then return 1; end if;\n"
                "    return 0;\n"));

    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj;
    d.objectPath = aObj;
    deps.push_back(d);
    BEZEL_EXPECT_TRUE(driver.compileFile(bSrc.path, bExe.path, deps).success);
    int status = std::system(bExe.path.c_str());
    BEZEL_EXPECT_TRUE(status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

// --- OVER-ACCEPTANCE control ------------------------------------------------

// Setting the diagnostic location for EVERY construction path (rather than one
// branch) must not turn any construction into a reject. A carriable initializer
// on an explicit-constructor imported generic still builds and still reads its
// declared value. Passes both ways; the expected value is deliberately not 0.
BEZEL_TEST(xmod_imported_generic_field_init_diag_2874,
           carriable_initializer_still_builds_and_runs_spec_2_8_2) {
    std::string stem = unique_stem("bz2874_ok_");
    std::string aObj;
    BEZEL_EXPECT_TRUE(buildLib2874(stem,
        "module Lib2874E; public class GE2874<T> "
        "  public v: T; public tag: int := 107; "
        "  public constructor(x: T) v := x; end constructor; end GE2874;",
        aObj));
    TempFile2874 aObjFile(aObj);
    if (!std::ifstream(aObj).good()) return;

    auto dir = std::filesystem::temp_directory_path();
    TempFile2874 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2874 bExe((dir / (stem + "_b")).string());
    writeSource2874(bSrc.path,
        app2874("App2874E", "Lib2874E",
                "    using var g := new GE2874<int>(1);\n"
                "    return g.tag;\n"));

    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj;
    d.objectPath = aObj;
    deps.push_back(d);
    BEZEL_EXPECT_TRUE(driver.compileFile(bSrc.path, bExe.path, deps).success);
    int status = std::system(bExe.path.c_str());
    BEZEL_EXPECT_EQ(
        (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1, 107);
}

}  // namespace bezel::module
