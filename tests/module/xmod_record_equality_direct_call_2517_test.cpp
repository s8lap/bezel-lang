// fix #2517 (§9.2, v0.236): pin the §9.2 use-site gate for direct
// `.Equals(Object?)`/`.Equals(T)`/`.GetHashCode()` calls on an IMPORTED
// non-auto-hashable body record.
//
// The §9.2 use-site gate at `src/sema/ExprTypecheck_Calls.cpp` (the
// `is not available on record ... it is not auto-hashable (§9.2/§9.3)`
// error) was nested INSIDE the `else if (targetType->kind == Record || Enum
// || Choice)` branch — reached only when `targetType->decl != nullptr` (a
// local AST TypeDecl). For IMPORTED records, `targetType->decl == nullptr`
// (the loader's `reconstructTypeSymbol` never sets it, §11.3/issue #501), so
// they took the `if (!targetType->decl)` builtin-class branch — intended for
// Console, Math, Guid, … — fell through with no match, then past the "no
// member" reject (which exempts Equals/GetHashCode as universal §8.1 members),
// and compiled silently. A local twin correctly rejected; an imported twin
// false-accepted. Hoisting the gate to fire BEFORE the null-decl branch
// makes it apply to BOTH null-decl (imported) and non-null-decl (local)
// records (and choices per §35.7). Sibling structural defect of #2374
// (`astNode`-gated gate) and #2516 (positional/combined-form classifier).
//
// The fix's own diagnostic-text assertion (`is not auto-hashable`)
// is what discriminates a genuine §9.2 reject from a crash / silent no-op:
// per the lane protocol, `runAot`'s `WIFSIGNALED` path synthesizes an
// `Error` diagnostic for a runtime SIGSEGV too, so a coarse `rejects()`
// helper is INSUFFICIENT. The unrelated body-record construction gap
// (#2434/#2467) emits `internal: lowerLocalDecl's initializer expression
// failed to lower ...` — present AFTER the fix as well, but distinct from
// the §9.2 use-site error which we assert on.

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
#include "support/unique_stem.hpp"


namespace bezel::module {
namespace {

struct TempFile2517 {
    std::string path;
    explicit TempFile2517(std::string p) : path(std::move(p)) {}
    ~TempFile2517() { std::remove(path.c_str()); }
};

void writeSource2517(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj2517(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// Compile lib → object, then compile app (importing lib) WITHOUT running,
// capturing the driver's stderr into `err` and returning compile success.
// Mirrors `xmod_record_private_field_equality_2483`'s helper exactly.
bool compileImportCaptureErr2517(const std::string& libSrc,
                                 const std::string& appSrc,
                                 std::string& err) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2517_");
    TempFile2517 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2517 aObj((dir / (stem + "_a.o")).string());
    writeSource2517(aSrc.path, libSrc);
    if (!compileToObj2517(aSrc.path, aObj.path)) return false;
    if (!std::ifstream(aObj.path).good()) return false;

    TempFile2517 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2517 bExe((dir / (stem + "_b")).string());
    writeSource2517(bSrc.path, appSrc);

    std::string errPath =
        (dir / ("bz2517_err_" + unique_stem("") + ".txt")).string();
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
// stderr. Used for the LOCAL consistency anchor (T3/T4) — the exporter's own
// compilation of the same record shape.
bool compileSingleCaptureErr2517(const std::string& src, std::string& err) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2517s_");
    TempFile2517 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2517 aExe((dir / (stem + "_a")).string());
    writeSource2517(aSrc.path, src);

    std::string errPath =
        (dir / ("bz2517_err_" + unique_stem("") + ".txt")).string();
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

// §9.2 use-site error text — identical for record and choice, used as the
// discriminator (per the lane protocol, a crash / SIGSEGV also triggers a
// `runAot`-level `Error` — assert on the actual diagnostic wording).
bool hasAutoEqError2517(const std::string& err) {
    return err.find("is not auto-hashable") != std::string::npos;
}

// §9.2 disqualifier: a body-only record with an EXPLICIT public field. The
// §9.2 admissibility rule ("An explicit field declaration (storage that is
// not an auto-property)" is a disqualifier regardless of visibility — see
// xmod_record_private_field_equality_2483 for the PRIVATE twin pin).
const char* kPubFieldLib2517 =
    "module RecLib2517; "
    "public record Rec2517 "
    "  public property X: int; "
    "  public _extra: int := 0; "
    "  public constructor(x: int) X := x; end constructor; "
    "end Rec2517;";

}  // namespace

// ===========================================================================
// T1 — CORE PIN (#2517 / §9.2): an IMPORTED body-only record with a public
// explicit field rejects `.Equals(...)` at the use site. Pre-#2517 the
// §9.2 gate was nested inside the `else if (Record || …)` branch, which is
// reached only when `decl != nullptr`; the imported record's null `decl`
// took the `if (!decl)` builtin-class branch and silently compiled the
// `.Equals` call (bypass). Post-#2517 (this fix) the gate is hoisted and
// fires for both null-decl and non-null-decl records. Demonstrated
// mechanically: with the hoisted gate block reverted in
// `src/sema/ExprTypecheck_Calls.cpp`, T1 below FAILS (no §9.2 diagnostic).
// ===========================================================================
BEZEL_TEST(xmod_record_equality_direct_call_2517,
           imported_equals_use_site_error_spec_9_2) {
    std::string err;
    bool ok = compileImportCaptureErr2517(
        kPubFieldLib2517,
        "module App2517T1; import RecLib2517; "
        "public class C2517T1 "
        "  public static method Main(): int "
        "    var a := new Rec2517(5); "
        "    var b := new Rec2517(5); "
        "    if a.Equals(b) then return 1; else return 0; end if; "
        "  end Main; "
        "end C2517T1;",
        err);
    BEZEL_EXPECT_FALSE(ok);
    BEZEL_EXPECT_TRUE(hasAutoEqError2517(err));
}

// ===========================================================================
// T2 — TWIN of T1 for `.GetHashCode()`. §9.2 covers BOTH `.Equals(...)` and
// `.GetHashCode()` as direct-call use-site errors (per the issue: "The
// compile error applies to the `=` and `<>` operators AND to direct calls
// of `.Equals(Object?)` and `.GetHashCode()` on the record."). Pre-#2517
// the gate was bypassed identically; post-fix the twin fires.
// ===========================================================================
BEZEL_TEST(xmod_record_equality_direct_call_2517,
           imported_gethashcode_use_site_error_spec_9_2) {
    std::string err;
    bool ok = compileImportCaptureErr2517(
        kPubFieldLib2517,
        "module App2517T2; import RecLib2517; "
        "public class C2517T2 "
        "  public static method Main(): int "
        "    var a := new Rec2517(5); "
        "    return a.GetHashCode(); "
        "  end Main; "
        "end C2517T2;",
        err);
    BEZEL_EXPECT_FALSE(ok);
    BEZEL_EXPECT_TRUE(hasAutoEqError2517(err));
}

// ===========================================================================
// T3 — LOCAL consistency anchor (§2.8.3 rule 4): the exporter's OWN
// compilation of the same record shape also rejects `.Equals(...)`. This is
// the ground-truth the imported classification is now consistent with. The
// LOCAL gate pre-existed and worked correctly; this test pins the hoisted
// gate against that local behaviour so it cannot silently drift.
// ===========================================================================
BEZEL_TEST(xmod_record_equality_direct_call_2517,
           local_twin_equals_use_site_error_spec_9_2) {
    std::string err;
    bool ok = compileSingleCaptureErr2517(
        "module Local2517T3; "
        "record Rec2517L "
        "  public property X: int; "
        "  public _extra: int := 0; "
        "  public constructor(x: int) X := x; end constructor; "
        "end Rec2517L; "
        "class CL2517T3 "
        "  public static method Main(): int "
        "    var a := new Rec2517L(5); "
        "    var b := new Rec2517L(5); "
        "    if a.Equals(b) then return 1; else return 0; end if; "
        "  end Main; "
        "end CL2517T3;",
        err);
    BEZEL_EXPECT_FALSE(ok);
    BEZEL_EXPECT_TRUE(hasAutoEqError2517(err));
}

// ===========================================================================
// T4 — LOCAL twin of T2 for `.GetHashCode()`. Same pin as T3 for the
// GetHashCode half of the §9.2 use-site rule.
// ===========================================================================
BEZEL_TEST(xmod_record_equality_direct_call_2517,
           local_twin_gethashcode_use_site_error_spec_9_2) {
    std::string err;
    bool ok = compileSingleCaptureErr2517(
        "module Local2517T4; "
        "record Rec2517L "
        "  public property X: int; "
        "  public _extra: int := 0; "
        "  public constructor(x: int) X := x; end constructor; "
        "end Rec2517L; "
        "class CL2517T4 "
        "  public static method Main(): int "
        "    var a := new Rec2517L(5); "
        "    return a.GetHashCode(); "
        "  end Main; "
        "end CL2517T4;",
        err);
    BEZEL_EXPECT_FALSE(ok);
    BEZEL_EXPECT_TRUE(hasAutoEqError2517(err));
}

// ===========================================================================
// T5 — POSITIVE CONTROL (regression guard against over-rejection): an
// IMPORTED body-only record with ONLY an auto-property (no disqualifying
// field) IS auto-hashable — `.Equals(...)` compiles without the
// §9.2 use-site error. This proves T1/T2's reject is the field-disqualifier
// firing, not a blanket rejection of imported body records. (The
// cross-module construction gap #2434/#2467 may emit `internal:` diagnostics
// here, but the §9.2 `auto-hashable` message must be ABSENT — sema
// accepted `.Equals`.)
// ===========================================================================
BEZEL_TEST(xmod_record_equality_direct_call_2517,
           imported_autoproperty_record_equals_accepted_control) {
    std::string err;
    compileImportCaptureErr2517(
        "module RecLib2517Pos; "
        "public record Rec2517Pos "
        "  public property X: int; "
        "  public constructor(x: int) X := x; end constructor; "
        "end Rec2517Pos;",
        "module App2517T5; import RecLib2517Pos; "
        "public class C2517T5 "
        "  public static method Main(): int "
        "    var a := new Rec2517Pos(5); "
        "    var b := new Rec2517Pos(5); "
        "    if a.Equals(b) then return 1; else return 0; end if; "
        "  end Main; "
        "end C2517T5;",
        err);
    BEZEL_EXPECT_FALSE(hasAutoEqError2517(err));
}

// ===========================================================================
// T6 — POSITIVE CONTROL for `.GetHashCode()`: an IMPORTED auto-property-only
// record accepts `.GetHashCode()`. Mirrors T5 for the GetHashCode half.
// ===========================================================================
BEZEL_TEST(xmod_record_equality_direct_call_2517,
           imported_autoproperty_record_gethashcode_accepted_control) {
    std::string err;
    compileImportCaptureErr2517(
        "module RecLib2517Pos; "
        "public record Rec2517Pos "
        "  public property X: int; "
        "  public constructor(x: int) X := x; end constructor; "
        "end Rec2517Pos;",
        "module App2517T6; import RecLib2517Pos; "
        "public class C2517T6 "
        "  public static method Main(): int "
        "    var a := new Rec2517Pos(5); "
        "    return a.GetHashCode(); "
        "  end Main; "
        "end C2517T6;",
        err);
    BEZEL_EXPECT_FALSE(hasAutoEqError2517(err));
}

}  // namespace bezel::module
