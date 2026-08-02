// fix #2516 (§9.2/§9.3/§2.8.3 rule 4, v0.236): an IMPORTED combined-form
// record (positional field + non-static body field) rejects `=`/`<>` at the
// use site — matching the exporter's own local compilation. Sibling of
// #2483 (the body-only twin, fixed in v0.235) and #2517 (the Equals/
// GetHashCode-direct-call twin, fixed in v0.236).
//
// Background: pre-#2468 an imported record's body field was invisible to the
// importer, so `isPositionalRecordEqualityBearing`'s (SymbolCollection.cpp)
// unconditional `return true` for the import case was conservative for a
// PURE-positional record (its positional-field types were not inspectable
// anyway) but ONLY for a pure-positional record. The body-only false-accept
// was the #2483 finding (fixed by #2468 widening ModuleMetadataBuilder's
// `includeForLayout` carriage to records, so body FieldSymbols now cross
// the wire for LAYOUT purposes). The combined-form twin was NOT closed by
// #2483 — the positional classifier in `isPositionalRecordEqualityBearing`
// still bailed out unconditionally for imports, and combined-form records
// with a non-static body field false-accepted `=`/`<>` cross-module even
// though the exporter's own compilation rejects them (§2.8.3 rule 4:
// a derived type property must be consistent across module boundaries).
//
// The fix carries the positional vs body distinction across the wire via a
// new `MetaMember::isPositional` bit (additive 17th MEMBER_BLOCK scalar — a
// pre-#2516 blob has no 17th scalar, defaults to `false`, and a stale
// not-yet-recompiled dependency keeps its pre-fix "treat every imported
// FieldSymbol as positional" classification, exactly per the additive-
// scalar pattern this schema has used for hasSetter/hasGetter/isAutoProperty/
// isEventStatic/isEventAsync). The importer's `convertMember` Field branch
// sets `f->posParam` to a synthetic null-type `ast::Param` (kept alive by
// `SemaContext::importedParams_`) for `mm.isPositional == true`, mirroring
// the LOCAL SymbolCollection.cpp:295-313 path which points posParam at a real
// `td.positionalFields[i]`. `storageComponentType` and the layout path
// (`LlvmBackend_Layout.cpp`'s `memberLayoutType`) guard on
// `posParam->type != nullptr` and fall through to `importedMemberType(m)` —
// so the synthetic Param's null type is never dereferenced for the
// IMPORTED path, only the synthetic's role as a "is positional" sentinel is
// read.
//
// The flip is observed at SEMA level through the §9.2 use-site gate on `=`/
// `<>` (ExprTypecheck_Exprs.cpp, `isPositionalRecordEqualityBearing` consults
// the now-imported body FieldSymbol and returns false on the disqualifying
// non-static body field). Asserting on diagnostic TEXT (not just compile
// failure) per the lane protocol — a crash would also "fail to compile".

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

struct TempFile2516 {
    std::string path;
    explicit TempFile2516(std::string p) : path(std::move(p)) {}
    ~TempFile2516() { std::remove(path.c_str()); }
};

void writeSource2516(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj2516(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// Mirrors `xmod_record_private_field_equality_2483`'s helper exactly: compile
// lib -> object, then compile app (importing lib) WITHOUT running, capturing
// the driver's stderr into `err` and returning compile success. Used here
// to assert the SEMA-level §9.2 use-site error text (T1/T2 reject path) and
// its absence on the positive controls (T4/T5/T6/T7).
bool compileImportCaptureErr2516(const std::string& libSrc,
                                 const std::string& appSrc,
                                 std::string& err) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2516_" + std::to_string(std::rand());
    TempFile2516 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2516 aObj((dir / (stem + "_a.o")).string());
    writeSource2516(aSrc.path, libSrc);
    if (!compileToObj2516(aSrc.path, aObj.path)) return false;
    if (!std::ifstream(aObj.path).good()) return false;

    TempFile2516 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2516 bExe((dir / (stem + "_b")).string());
    writeSource2516(bSrc.path, appSrc);

    std::string errPath =
        (dir / ("bz2516_err_" + std::to_string(std::rand()) + ".txt")).string();
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

bool hasAutoEqError2516(const std::string& err) {
    return err.find("is not auto-equality-bearing") != std::string::npos;
}

// Single-module twin of the imported tests (T3 / T8): compile one source
// (no import) capturing stderr. Used for the LOCAL consistency anchors.
bool compileSingleCaptureErr2516(const std::string& src, std::string& err) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2516s_" + std::to_string(std::rand());
    TempFile2516 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2516 aExe((dir / (stem + "_a")).string());
    writeSource2516(aSrc.path, src);

    std::string errPath =
        (dir / ("bz2516_err_" + std::to_string(std::rand()) + ".txt")).string();
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

// Combined-form record with one positional field and one PRIVATE body field
// — the exact #2516 repro shape. §9.2 v0.182 disqualifies a non-static body
// field regardless of visibility (§9.2 "An explicit field declaration
// (storage that is not an auto-property)"), so the exporter rejects `=`/
// `<>` on this record locally; #2516 closes the same classification for
// the importer.
const char* kCombinedFormPrivateBodyLib2516 =
    "module RecLib2516; "
    "public record Rec2516 "
    "  (x: int) "
    "  private _cache: int := 0; "
    "end Rec2516;";

// Pure-positional control: NO body field, so the §9.2 auto-equality rule
// accepts `=`/`<>` locally AND cross-module.
const char* kPurePositionalLib2516 =
    "module RecLib2516Pos; "
    "public record RecPos2516(x: int) "
    "end RecPos2516;";

// Combined-form with only a static body field: the static field is ignored
// (§9.2 v0.182 #1572: it carries no instance storage), so `=`/`<>` should
// still be auto-equality-bearing both locally and cross-module.
const char* kCombinedFormStaticBodyLib2516 =
    "module RecLib2516Static; "
    "public record RecStatic2516 "
    "  (x: int) "
    "  public static _cache: int := 0; "
    "end RecStatic2516;";

// Combined-form with only an auto-property body: the auto-property's type
// must be admissible (§9.2) but it is, so `=`/`<>` should be auto-equality-
// bearing locally AND cross-module.
const char* kCombinedFormAutoPropLib2516 =
    "module RecLib2516AP; "
    "public record RecAP2516 "
    "  (x: int) "
    "  public property Y: int := 0; "
    "end RecAP2516;";

}  // namespace

// ===========================================================================
// T1 — CORE PIN (§9.2, §2.8.3 rule 4): an IMPORTED combined-form record
// with a private body field rejects `=` at the use site. Pre-#2516 the
// positional classifier's unconditional `return true` for the import case
// silently accepted `=` even though the exporter's own compilation of the
// same record shape rejected it; post-#2516 (HEAD) the importer carries the
// body FieldSymbol's disqualifier through the `isPositional` metadata bit,
// the `posParam` sentinel, and `isPositionalRecordEqualityBearing`'s body-
// field disqualifier scan — so the §9.2 use-site error fires, matching the
// exporter's own compilation. Demonstrated mechanically: with the import
// bailout restored (and the MetaMember::isPositional bit + importer
// `posParam` sentinel + storageComponentType / memberLayoutType guards
// reverted), T1 FAILS (the imported `=` compiles instead of erroring).
// Asserting on the diagnostic TEXT (not just compile failure) per the lane
// protocol — a crash would also "fail to compile".
// ===========================================================================
BEZEL_TEST(xmod_record_combined_form_equality_2516,
           imported_equals_use_site_error_spec_9_2) {
    std::string err;
    bool ok = compileImportCaptureErr2516(
        kCombinedFormPrivateBodyLib2516,
        "module App2516T1; import RecLib2516; "
        "public class C2516T1 "
        "  public static method Main(): int "
        "    var a := new Rec2516(5); "
        "    var b := new Rec2516(5); "
        "    if a = b then return 1; else return 0; end if; "
        "  end Main; "
        "end C2516T1;",
        err);
    BEZEL_EXPECT_FALSE(ok);
    BEZEL_EXPECT_TRUE(hasAutoEqError2516(err));
}

// ===========================================================================
// T2 — TWIN of T1 for `<>` (the §9.2 use-site error covers BOTH `=` and
// `<>`; they share the gate in ExprTypecheck_Exprs.cpp).
// ===========================================================================
BEZEL_TEST(xmod_record_combined_form_equality_2516,
           imported_not_equals_use_site_error_spec_9_2) {
    std::string err;
    bool ok = compileImportCaptureErr2516(
        kCombinedFormPrivateBodyLib2516,
        "module App2516T2; import RecLib2516; "
        "public class C2516T2 "
        "  public static method Main(): int "
        "    var a := new Rec2516(5); "
        "    var b := new Rec2516(6); "
        "    if a <> b then return 1; else return 0; end if; "
        "  end Main; "
        "end C2516T2;",
        err);
    BEZEL_EXPECT_FALSE(ok);
    BEZEL_EXPECT_TRUE(hasAutoEqError2516(err));
}

// ===========================================================================
// T3 — LOCAL consistency anchor (§2.8.3 rule 4): the exporter's OWN
// compilation of the same record shape also rejects `=`. Ground truth the
// imported classification is now consistent with — it always was correct
// locally; the false-accept was import-only.
// ===========================================================================
BEZEL_TEST(xmod_record_combined_form_equality_2516,
           local_twin_equals_use_site_error_spec_9_2) {
    std::string err;
    bool ok = compileSingleCaptureErr2516(
        "module Local2516T3; "
        "record Rec2516L "
        "  (x: int) "
        "  private _cache: int := 0; "
        "end Rec2516L; "
        "class CL2516T3 "
        "  public static method Main(): int "
        "    var a := new Rec2516L(5); "
        "    var b := new Rec2516L(5); "
        "    if a = b then return 1; else return 0; end if; "
        "  end Main; "
        "end CL2516T3;",
        err);
    BEZEL_EXPECT_FALSE(ok);
    BEZEL_EXPECT_TRUE(hasAutoEqError2516(err));
}

// ===========================================================================
// T4 — POSITIVE CONTROL (regression guard against over-rejection): an
// IMPORTED pure-positional record (no body field) IS auto-equality-bearing
// — `=` compiles without the §9.2 use-site error. This proves T1/T2's
// reject is the body-field disqualifier firing, not a blanket rejection of
// imported positional records. The cross-module construction gap #2434/
// #2467 may emit `internal:` diagnostics here, but the §9.2
// `auto-equality-bearing` message must be ABSENT — sema accepted `=`.
// ===========================================================================
BEZEL_TEST(xmod_record_combined_form_equality_2516,
           imported_pure_positional_equals_accepted_control) {
    std::string err;
    compileImportCaptureErr2516(
        kPurePositionalLib2516,
        "module App2516T4; import RecLib2516Pos; "
        "public class C2516T4 "
        "  public static method Main(): int "
        "    var a := new RecPos2516(5); "
        "    var b := new RecPos2516(5); "
        "    if a = b then return 1; else return 0; end if; "
        "  end Main; "
        "end C2516T4;",
        err);
    BEZEL_EXPECT_FALSE(hasAutoEqError2516(err));
}

// ===========================================================================
// T5 — POSITIVE CONTROL: an IMPORTED combined-form record with ONLY a static
// body field (`public static _cache`) is auto-equality-bearing (the static
// body field carries no instance storage — §9.2 v0.182 / #1572). Pin: a
// combined-form record with body members but no disqualifying non-static
// body field must compile `=` cross-module, otherwise #2516's fix would
// have over-rejected every combined-form record regardless of body shape.
// ===========================================================================
BEZEL_TEST(xmod_record_combined_form_equality_2516,
           imported_combined_form_static_body_accepted_control) {
    std::string err;
    compileImportCaptureErr2516(
        kCombinedFormStaticBodyLib2516,
        "module App2516T5; import RecLib2516Static; "
        "public class C2516T5 "
        "  public static method Main(): int "
        "    var a := new RecStatic2516(5); "
        "    var b := new RecStatic2516(5); "
        "    if a = b then return 1; else return 0; end if; "
        "  end Main; "
        "end C2516T5;",
        err);
    BEZEL_EXPECT_FALSE(hasAutoEqError2516(err));
}

// ===========================================================================
// T6 — POSITIVE CONTROL: an IMPORTED combined-form record with ONLY an
// admissible auto-property body is auto-equality-bearing. Pin: a body
// auto-property is not a disqualifier per §9.2, and the importer
// correctly carries the auto-property's admissibility through the
// `fieldTypes_` side table so the §9.2 use-site gate stays clean.
// ===========================================================================
BEZEL_TEST(xmod_record_combined_form_equality_2516,
           imported_combined_form_autoprop_accepted_control) {
    std::string err;
    compileImportCaptureErr2516(
        kCombinedFormAutoPropLib2516,
        "module App2516T6; import RecLib2516AP; "
        "public class C2516T6 "
        "  public static method Main(): int "
        "    var a := new RecAP2516(5); "
        "    var b := new RecAP2516(5); "
        "    if a = b then return 1; else return 0; end if; "
        "  end Main; "
        "end C2516T6;",
        err);
    BEZEL_EXPECT_FALSE(hasAutoEqError2516(err));
}

}  // namespace bezel::module