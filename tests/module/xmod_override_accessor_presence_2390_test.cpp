// fix #2390 (§12.1 / §23 / §2.8.2 / §2.8.3, v0.236): the accessor-presence
// (getter/setter) leg of `Resolver::checkPropertyOverrideSignatureMatch`
// (`src/sema/TypeResolution_Checks.cpp`) was the last §12.1 override-signature
// dimension still gated on the base's `ast::PropertyDecl` (`if (bDecl && ...)`),
// so an override of a property inherited from an IMPORTED base — whose
// PropertySymbol never carries an `ast::PropertyDecl` (ModuleLoader's Property
// case reconstructs a body-less declaration; §2.8.2/§2.8.3: the artifact
// interface carries no AST) — silently SKIPPED the accessor-presence match.
// A mismatched-accessor override (base get-only, override adds a setter; or
// the inverse) compiled clean across the module boundary, even though it
// changes the vtable slot shape (codegen reserves a `$set` slot only when a
// property has a setter, LlvmBackend_Setup.cpp), leaving a slot unfilled or
// orphaned. The LOCAL twin was, correctly, already rejected.
//
// This is the imported-base twin of the #2374 astNode-gate family, and the
// accessor-presence residual that #2386 (which fixed the property-TYPE
// comparison for the identical imported-base shape) deliberately deferred:
// #2386's metadata schema carried no accessor-presence bit, so #2386 left the
// accessor leg gated on `bDecl` and filed #2390. #2400 then ADDED
// `MetaMember::hasSetter` (plus the loader side table
// `ctx_.importedPropertyHasSetter`); this fix consumes it as the `bDecl`-null
// fallback — the accessor-presence analog of #2386's `ctx_.importedMemberType`
// fallback for the property type, in the same `if (dDecl)` block.
//
// One refinement over a naive bit-for-bit fallback: the #2400 bit mirrors
// CODEGEN's `propertyHasSetter` predicate, which SPURIOUSLY reports a setter
// for an abstract Auto-shorthand member (`abstract property P: T;` /
// `abstract property [i]: T;`, §12.5 = get-only) — harmless for codegen's
// vtable slot allocation (an abstract class is never instantiated), but not
// for this semantic match, where it would false-reject the canonical
// `abstract property`/`abstract indexer` + get-only override idiom (the LOCAL
// twins of #1614's lit cases, and #2386's own T7). The fix applies the same
// §12.5 correction sema's local `propertyDeclHasSetter` does: an abstract
// Auto-shorthand base member is get-only. For a property, the #2478
// `importedPropertyIsAutoStorage` bit identifies the Auto-shorthand shape
// precisely (so a block-form abstract property WITH a real `set` arm keeps the
// setter — the local path accepts that exotic shape, and so does this one).
// For an INDEXER the metadata cannot distinguish the Auto shorthand from a
// block form (`isAutoProperty` excludes indexers by design, #2478), so an
// abstract indexer is treated as the get-only shorthand — correct for the
// canonical idiom; the exotic abstract-block-indexer-with-set twin is a filed
// metadata-completeness residual, not a regression (it has no LOCAL test
// either beyond the parse-level p_absblock probe, and the common case is the
// shorthand).
//
// CONFIRMED SYMPTOM (verified by direct bezelc probe on origin/main before
// writing this test, then mechanically by reverting the src/ fix and re-running
// this filter — see the commit message): overriding an IMPORTED
// `public abstract property Val: int;` (get-only §12.5 shorthand) with
// `public override property Val: int { get => _n; set => _n := value; }`
// compiled clean (exit 0). The identical mismatch against a SAME-MODULE base
// was, and remains, correctly REJECTED — confirming the defect is the
// cross-module path, not a general hole in §12.1 accessor matching.
//
// Related: #2386 (property-TYPE twin, same function), #2400 (the hasSetter
// metadata bit + codegen consumer this fix reads), #2380 (the §11.2
// conformance-check sibling), #2374 (the astNode-gate audit family).

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

struct TempFile2390 {
    std::string path;
    explicit TempFile2390(std::string p) : path(std::move(p)) {}
    ~TempFile2390() { std::remove(path.c_str()); }
};

void writeSource2390(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj2390(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// Compile B importing A, capturing the driver's stderr (fd 2) into `err`.
// Mirrors xmod_override_signature_2386_test.cpp's compileImportCaptureErr2386.
bool compileImportCaptureErr2390(const std::string& bSrc,
                                 const std::string& bExe,
                                 const std::vector<std::string>& aObjs,
                                 std::string& err) {
    namespace fs = std::filesystem;
    std::string errPath = (fs::temp_directory_path() /
                           ("bz2390_err_" + std::to_string(std::rand()) +
                            ".txt"))
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
        for (const auto& aObj : aObjs) {
            bezel::Dependency d;
            d.metadataPath = aObj;
            d.objectPath = aObj;
            deps.push_back(d);
        }
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

// Compile B importing A -> exe, run, return exit code (-1 on compile/link/run
// failure, -2 if the dependency itself could not be produced).
int compileImportAndRun2390(const std::string& libSrc,
                            const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2390_" + std::to_string(std::rand());
    TempFile2390 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2390 aObj((dir / (stem + "_a.o")).string());
    writeSource2390(aSrc.path, libSrc);
    if (!compileToObj2390(aSrc.path, aObj.path)) return -2;
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile2390 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2390 bExe((dir / stem).string());
    writeSource2390(bSrc.path, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    if (!driver.compileFile(bSrc.path, bExe.path, deps).success) return -1;
    int status = std::system(bExe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

}  // namespace

// ===========================================================================
// T1 — the confirmed false-accept shape from #2390: an IMPORTED abstract
// get-only (§12.5 shorthand) base property overridden with a get+set
// (block-form) property is REJECTED for accessor-presence mismatch (§12.1).
// Pre-fix, `bDecl` (base's ast::PropertyDecl*) was null for the imported base,
// so the `if (bDecl && ...)` gate skipped the accessor-presence comparison and
// the override compiled clean. The property TYPE matches (both `int`), so only
// the accessor-presence leg catches it — pinning the fix specifically.
// ===========================================================================
BEZEL_TEST(xmod_override_accessor_presence_2390,
           imported_getonly_base_setter_added_override_rejected_12_1) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2390_t1_" + std::to_string(std::rand());
    TempFile2390 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2390 aObj((dir / (stem + "_a.o")).string());
    writeSource2390(aSrc.path,
                    "module ImportedGetOnlyLib2390T1; "
                    "public abstract class AbsGetOnlyBase2390T1 "
                    "  public abstract property Val: int; "
                    "end AbsGetOnlyBase2390T1;");
    BEZEL_EXPECT_TRUE(compileToObj2390(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;  // toolchain unavailable

    std::string bSrc =
        "module AppSetterOverride2390T1; import ImportedGetOnlyLib2390T1; "
        "public class BadSetterOverride2390T1: AbsGetOnlyBase2390T1 "
        "  private _n: int := 0; "
        "  public override property Val: int "
        "    get => _n; "
        "    set => _n := value; "
        "  end; "
        "end BadSetterOverride2390T1; "
        "public class C public static method Main(): int => 0; end C;";
    TempFile2390 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2390 bExe((dir / (stem + "_b")).string());
    writeSource2390(bSrcFile.path, bSrc);

    std::string err;
    bool success = compileImportCaptureErr2390(bSrcFile.path, bExe.path,
                                               {aObj.path}, err);
    BEZEL_EXPECT_TRUE(!success);
    BEZEL_EXPECT_TRUE(err.find("12.1") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("override of `Val`") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("accessor set/get presence") != std::string::npos);
}

// ===========================================================================
// T2 — LOCAL-BASE CONTROL: the identical mismatch against a SAME-MODULE base
// was, and remains, correctly rejected. Guards that the fix (which only adds
// the imported-base fallback) leaves the already-working local path untouched.
// ===========================================================================
BEZEL_TEST(xmod_override_accessor_presence_2390,
           local_getonly_base_setter_added_override_still_rejected_12_1) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2390_t2_" + std::to_string(std::rand());
    std::string bSrc =
        "module LocalGetOnly2390T2; "
        "public abstract class AbsGetOnlyBaseLocal2390T2 "
        "  public abstract property Val: int; "
        "end AbsGetOnlyBaseLocal2390T2; "
        "public class BadSetterOverrideLocal2390T2: AbsGetOnlyBaseLocal2390T2 "
        "  private _n: int := 0; "
        "  public override property Val: int "
        "    get => _n; "
        "    set => _n := value; "
        "  end; "
        "end BadSetterOverrideLocal2390T2; "
        "public class C public static method Main(): int => 0; end C;";
    TempFile2390 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2390 bExe((dir / (stem + "_b")).string());
    writeSource2390(bSrcFile.path, bSrc);

    std::string err;
    bool success =
        compileImportCaptureErr2390(bSrcFile.path, bExe.path, {}, err);
    BEZEL_EXPECT_TRUE(!success);
    BEZEL_EXPECT_TRUE(err.find("12.1") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("accessor set/get presence") != std::string::npos);
}

// ===========================================================================
// T3 — INVERSE direction: an IMPORTED virtual read-write (get+set) base
// property overridden with a get-only property is REJECTED (the override
// DROPS a setter the base declares). The §12.1 accessor-presence match is
// symmetric. Uses `virtual` (not abstract) so the base has a genuine setter
// (no §12.5 abstract-shorthand correction involved) — a clean builder-bit
// mismatch.
// ===========================================================================
BEZEL_TEST(xmod_override_accessor_presence_2390,
           imported_settable_base_setter_dropped_override_rejected_12_1) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2390_t3_" + std::to_string(std::rand());
    TempFile2390 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2390 aObj((dir / (stem + "_a.o")).string());
    writeSource2390(aSrc.path,
                    "module ImportedSettableLib2390T3; "
                    "public class VirtSettableBase2390T3 "
                    "  private _n: int := 0; "
                    "  public virtual property Val: int "
                    "    get => _n; "
                    "    set => _n := value; "
                    "  end; "
                    "end VirtSettableBase2390T3;");
    BEZEL_EXPECT_TRUE(compileToObj2390(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    std::string bSrc =
        "module AppGetterOnlyOverride2390T3; import ImportedSettableLib2390T3; "
        "public class BadGetterOnlyOverride2390T3: VirtSettableBase2390T3 "
        "  public override property Val: int => 0; "
        "end BadGetterOnlyOverride2390T3; "
        "public class C public static method Main(): int => 0; end C;";
    TempFile2390 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2390 bExe((dir / (stem + "_b")).string());
    writeSource2390(bSrcFile.path, bSrc);

    std::string err;
    bool success = compileImportCaptureErr2390(bSrcFile.path, bExe.path,
                                               {aObj.path}, err);
    BEZEL_EXPECT_TRUE(!success);
    BEZEL_EXPECT_TRUE(err.find("12.1") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("accessor set/get presence") != std::string::npos);
}

// ===========================================================================
// T4 — ACCEPT + RUN + VALUE (false-reject guard, canonical idiom): a CORRECT
// get-only override of an IMPORTED abstract get-only base property still
// compiles and dispatches the right value across the module boundary. This is
// the shape a naive fallback would FALSE-REJECT: the #2400 builder bit
// spuriously reports a setter for the abstract §12.5 shorthand base, so the
// fix must apply the §12.5 abstract-Auto correction (mirroring the local
// predicate) or this canonical idiom regresses. Mirrors #2386's T7.
// ===========================================================================
BEZEL_TEST(xmod_override_accessor_presence_2390,
           abstract_getonly_base_correct_getonly_override_runs_and_dispatches) {
    int r = compileImportAndRun2390(
        "module ImportedGetOnlyLib2390T4; "
        "public abstract class AbsGetOnlyBase2390T4 "
        "  public abstract property Val: int; "
        "end AbsGetOnlyBase2390T4;",
        "module AppCorrectGetOnly2390T4; import ImportedGetOnlyLib2390T4; "
        "public class GoodGetOnlyOverride2390T4: AbsGetOnlyBase2390T4 "
        "  private _n: int := 77; "
        "  public override property Val: int => _n; "
        "end GoodGetOnlyOverride2390T4; "
        "public class C "
        "  public static method Main(): int "
        "    using var b: AbsGetOnlyBase2390T4 := "
        "        new GoodGetOnlyOverride2390T4(); "
        "    return b.Val - 77; "
        "  end Main; "
        "end C;");
    if (r == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 0);
}

// ===========================================================================
// T5 — ACCEPT (false-reject guard, settable match): a CORRECT get+set
// override of an IMPORTED virtual get+set base property still compiles.
// Confirms the accessor-presence match ACCEPTS when both sides genuinely have
// a setter (no spurious-bit correction fires for a non-abstract settable
// base). Compile-only (the cross-module settable-virtual dispatch through the
// setter vtable slot is #2400's codegen territory, exercised separately).
// ===========================================================================
BEZEL_TEST(xmod_override_accessor_presence_2390,
           settable_base_correct_settable_override_compiles) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2390_t5_" + std::to_string(std::rand());
    TempFile2390 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2390 aObj((dir / (stem + "_a.o")).string());
    writeSource2390(aSrc.path,
                    "module ImportedSettableLib2390T5; "
                    "public class VirtSettableBase2390T5 "
                    "  private _n: int := 0; "
                    "  public virtual property Val: int "
                    "    get => _n; "
                    "    set => _n := value; "
                    "  end; "
                    "end VirtSettableBase2390T5;");
    BEZEL_EXPECT_TRUE(compileToObj2390(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    std::string bSrc =
        "module AppCorrectSettable2390T5; import ImportedSettableLib2390T5; "
        "public class GoodSettableOverride2390T5: VirtSettableBase2390T5 "
        "  private _m: int := 0; "
        "  public override property Val: int "
        "    get => _m; "
        "    set => _m := value; "
        "  end; "
        "end GoodSettableOverride2390T5; "
        "public class C public static method Main(): int => 0; end C;";
    TempFile2390 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2390 bExe((dir / (stem + "_b")).string());
    writeSource2390(bSrcFile.path, bSrc);

    std::string err;
    bool success = compileImportCaptureErr2390(bSrcFile.path, bExe.path,
                                               {aObj.path}, err);
    BEZEL_EXPECT_TRUE(success);
    BEZEL_EXPECT_TRUE(err.find("accessor set/get presence") == std::string::npos);
}

}  // namespace bezel::module
