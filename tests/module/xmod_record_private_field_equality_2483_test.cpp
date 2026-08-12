// fix #2483 (§9.2, v0.236): pin the auto-equality classification of an
// IMPORTED body-only record that carries a private field.
//
// #2468 widened ModuleMetadataBuilder.cpp's `includeForLayout` carriage from
// CLASSES to RECORDS, so a private/internal record body FIELD now crosses the
// module boundary for LAYOUT purposes (name/type/declaration-order position).
// A side effect of that widening — correct, but unstated by #2468's "layout
// only, no ABI change" framing and unexercised by its 6 cases (every one reads
// hidden state through an explicit accessor METHOD, none touches the equality
// surface) — is that `sema::isAutoEqualityBearingBodyRecord`
// (src/sema/SymbolCollection.cpp) now reaches the imported record's private
// `FieldSymbol` and correctly classifies the record as NOT auto-equality-
// bearing: §9.2 names "An explicit field declaration (storage that is not an
// auto-property)" as a disqualifier regardless of the field's visibility.
//
// Before #2468 the private field was absent from the importer's `t->members`,
// so the importer WRONGLY classified the record as auto-hashable — a
// latent false-accept: `=`/`<>` compiled on the imported type even though the
// exporter's own compilation of the same type already rejected them (§2.8.3
// rule 4 — a derived type property must be consistent across module
// boundaries). #2468 closed that false-accept; this test pins the post-#2468
// (correct) behaviour so the classification cannot silently drift back.
//
// The flip is observed at SEMA level through the §9.2 use-site gate on `=`/
// `<>` (src/sema/ExprTypecheck_Exprs.cpp), which consults
// `isAutoEqualityBearingBodyRecord` and rejects before any codegen runs (the
// unrelated body-record construction gap #2434/#2467 is therefore not reached
// on the reject path). Demonstrated mechanically: with #2468's single-line
// `|| t.kind == ast::TypeDeclKind::Record` clause reverted in
// ModuleMetadataBuilder.cpp's `includeForLayout`, T1/T2 below FAIL (the
// imported `=`/`<>` compiles instead of erroring — the false-accept returns);
// with the clause present (HEAD) they PASS.
//
// Authorities: §9.2 "A body record is not auto-hashable … when it
// contains: An explicit field declaration (storage that is not an
// auto-property)" and "The compile error applies to the `=` and `<>`
// operators"; §2.8.3 rule 4 (cross-module consistency of derived type
// properties). Sibling layout pin: xmod_record_private_field_layout_import_2468.

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "driver/Driver.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

struct TempFile2483 {
    std::string path;
    explicit TempFile2483(std::string p) : path(std::move(p)) {}
    ~TempFile2483() { std::remove(path.c_str()); }
};

void writeSource2483(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj2483(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// Mirrors #2468's compileImportCaptureErr: compile lib -> object, then compile
// app (importing lib) WITHOUT running, capturing the driver's stderr into `err`
// and returning compile success. Used here to assert the SEMA-level §9.2 use-
// site error text (T1/T2 reject path) and its absence on the positive control.
bool compileImportCaptureErr2483(const std::string& libSrc,
                                 const std::string& appSrc, std::string& err) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2483_" + std::to_string(std::rand());
    TempFile2483 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2483 aObj((dir / (stem + "_a.o")).string());
    writeSource2483(aSrc.path, libSrc);
    if (!compileToObj2483(aSrc.path, aObj.path)) return false;
    if (!std::ifstream(aObj.path).good()) return false;

    TempFile2483 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2483 bExe((dir / (stem + "_b")).string());
    writeSource2483(bSrc.path, appSrc);

    std::string errPath =
        (dir / ("bz2483_err_" + std::to_string(std::rand()) + ".txt")).string();
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
        d.metadataPath = aObj.path;
        d.objectPath = aObj.path;
        deps.push_back(d);
        success = driver.compileFile(bSrc.path, bExe.path, deps).success;
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

// Single-module twin of the above: compile one source (no import) capturing
// stderr. Used for the LOCAL consistency anchor (T3) — the exporter's own
// compilation of the same record shape.
bool compileSingleCaptureErr2483(const std::string& src, std::string& err) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2483s_" + std::to_string(std::rand());
    TempFile2483 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2483 aExe((dir / (stem + "_a")).string());
    writeSource2483(aSrc.path, src);

    std::string errPath =
        (dir / ("bz2483_err_" + std::to_string(std::rand()) + ".txt")).string();
    std::fflush(stderr);
    std::cerr.flush();
    int saved = ::dup(STDERR_FILENO);
    int fd = ::open(errPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    bool success = false;
    if (fd >= 0) ::dup2(fd, STDERR_FILENO);
    {
        bezel::Driver driver;
        success = driver.compileFile(aSrc.path, aExe.path, {}).success;
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

bool hasAutoEqError2483(const std::string& err) {
    return err.find("is not auto-hashable") != std::string::npos;
}

// A body-only record with a PRIVATE explicit field — the §9.2 disqualifier
// whose visibility to the importer is exactly what #2468's widening flipped.
const char* kPrivateFieldLib2483 =
    "module RecLib2483; "
    "public record Rec2483 "
    "  public property X: int; "
    "  private _cache: int := 0; "
    "  public constructor(x: int) X := x; end constructor; "
    "end Rec2483;";

}  // namespace

// ===========================================================================
// T1 — CORE PIN (§9.2, §2.8.3 rule 4): an IMPORTED body-only record with a
// private field rejects `=` at the use site. Pre-#2468 the private field was
// invisible to the importer, so `isAutoEqualityBearingBodyRecord` wrongly
// returned true and this compiled (false-accept); post-#2468 (HEAD) the field
// is layout-visible and the §9.2 use-site error fires, matching the exporter's
// own compilation. Asserting on the diagnostic TEXT (not just compile failure)
// per the lane protocol — a crash would also "fail to compile".
// ===========================================================================
BEZEL_TEST(xmod_record_private_field_equality_2483,
           imported_equals_use_site_error_spec_9_2) {
    std::string err;
    bool ok = compileImportCaptureErr2483(
        kPrivateFieldLib2483,
        "module App2483T1; import RecLib2483; "
        "public class C2483T1 "
        "  public static method Main(): int "
        "    var a := new Rec2483(5); "
        "    var b := new Rec2483(5); "
        "    if a = b then return 1; else return 0; end if; "
        "  end Main; "
        "end C2483T1;",
        err);
    // Sema rejects at the `=` use site; the unrelated construction gap
    // (#2434/#2467) is not reached on the reject path.
    BEZEL_EXPECT_FALSE(ok);
    BEZEL_EXPECT_TRUE(hasAutoEqError2483(err));
}

// ===========================================================================
// T2 — TWIN of T1 for `<>` (the §9.2 use-site error covers BOTH `=` and `<>`;
// they share the gate in ExprTypecheck_Exprs.cpp). Same flip, same pin.
// ===========================================================================
BEZEL_TEST(xmod_record_private_field_equality_2483,
           imported_not_equals_use_site_error_spec_9_2) {
    std::string err;
    bool ok = compileImportCaptureErr2483(
        kPrivateFieldLib2483,
        "module App2483T2; import RecLib2483; "
        "public class C2483T2 "
        "  public static method Main(): int "
        "    var a := new Rec2483(5); "
        "    var b := new Rec2483(6); "
        "    if a <> b then return 1; else return 0; end if; "
        "  end Main; "
        "end C2483T2;",
        err);
    BEZEL_EXPECT_FALSE(ok);
    BEZEL_EXPECT_TRUE(hasAutoEqError2483(err));
}

// ===========================================================================
// T3 — LOCAL consistency anchor (§2.8.3 rule 4): the exporter's OWN
// compilation of the same record shape also rejects `=`. This is the ground
// truth the imported classification is now consistent with (it always was
// correct locally; the false-accept was import-only). Pinned alongside T1 so
// the cross-module consistency claim is self-contained and testable.
// ===========================================================================
BEZEL_TEST(xmod_record_private_field_equality_2483,
           local_twin_equals_use_site_error_spec_9_2) {
    std::string err;
    bool ok = compileSingleCaptureErr2483(
        "module Local2483T3; "
        "record Rec2483L "
        "  public property X: int; "
        "  private _cache: int := 0; "
        "  public constructor(x: int) X := x; end constructor; "
        "end Rec2483L; "
        "class CL "
        "  public static method Main(): int "
        "    var a := new Rec2483L(5); "
        "    var b := new Rec2483L(5); "
        "    if a = b then return 1; else return 0; end if; "
        "  end Main; "
        "end CL;",
        err);
    BEZEL_EXPECT_FALSE(ok);
    BEZEL_EXPECT_TRUE(hasAutoEqError2483(err));
}

// ===========================================================================
// T4 — POSITIVE CONTROL (regression guard against over-rejection): an
// IMPORTED body-only record with ONLY an auto-property (no disqualifying
// field) IS auto-hashable — `=` compiles without the §9.2 use-site
// error. This proves T1/T2's reject is the field-disqualifier firing, not a
// blanket rejection of imported body records. (The cross-module construction
// gap #2434/#2467 may emit `internal:` diagnostics here, but the §9.2
// `auto-hashable` message must be ABSENT — sema accepted `=`.)
// ===========================================================================
BEZEL_TEST(xmod_record_private_field_equality_2483,
           imported_autoproperty_record_equals_accepted_control) {
    std::string err;
    compileImportCaptureErr2483(
        "module RecLib2483Pos; "
        "public record Rec2483Pos "
        "  public property X: int; "
        "  public constructor(x: int) X := x; end constructor; "
        "end Rec2483Pos;",
        "module App2483T4; import RecLib2483Pos; "
        "public class C2483T4 "
        "  public static method Main(): int "
        "    var a := new Rec2483Pos(5); "
        "    var b := new Rec2483Pos(5); "
        "    if a = b then return 1; else return 0; end if; "
        "  end Main; "
        "end C2483T4;",
        err);
    BEZEL_EXPECT_FALSE(hasAutoEqError2483(err));
}

}  // namespace bezel::module
