// fix #2523 (§12.1 / §12.5 / §2.8.2 / §2.8.3 rule 4, v0.236): the accessor-
// presence leg of `Resolver::checkPropertyOverrideSignatureMatch`
// (`src/sema/TypeResolution_Checks.cpp`) carried a §12.5 abstract-shorthand
// correction that over-fired for an IMPORTED base indexer:
// `MetaMember::hasSetter` (the codegen-aligned bit, #2400) is true for an
// abstract Auto-shorthand indexer (`abstract property [i]: T;`, §12.5 =
// get-only — SPURIOUS, harmless for codegen's vtable-slot allocation but
// semantically wrong) AND for an abstract block-form indexer with a real
// `set` arm (`abstract property [i]: T { get; set; end; }` — CORRECT, a real
// read-write obligation). The correction (lines ~2675-2683) treated EVERY
// abstract indexer as the get-only shorthand:
//   if (baseAbstract && (base->isIndexer ||
//                        ctx_.importedPropertyIsAutoStorage(base))) {
//       bHasSetter = false;
//   }
// The `base->isIndexer` clause was a coarse substitute for the Auto-
// shorthand signal — `importedPropertyIsAutoStorage` (#2478) was
// property-only (the bit excluded indexers, mirroring `isAutoPropertySymbol`
// in storage-layout terms), so an abstract indexer had no finer
// classification available, and the local/imported asymmetry for the
// exotic abstract-block-form-indexer-with-set shape was the #2390 residual.
// The LOCAL twin (`bDecl` non-null, `propertyDeclHasSetter` reads the AST
// directly) correctly ACCEPTS the matching block-form-with-set override —
// only the cross-module path rejected. Fix: extend `MetaMember::isAutoProperty`
// (ModuleMetadataBuilder.cpp) to also cover Auto-body INDEXERS, dropping the
// `!prop->isIndexer` exclusion #2478 added in pure-storage terms. The
// importer-side `ctx_.importedPropertyIsAutoStorage(base)` now returns true
// iff the base is in Auto shorthand form — whether property or indexer —
// and the override check tightens to just
// `baseAbstract && ctx_.importedPropertyIsAutoStorage(base)`, dropping the
// coarse `base->isIndexer` clause. Storage consumers are UNAFFECTED
// (`LlvmBackend_impl.hpp`'s `isAutoPropertyStorage` helper filters
// `ps->isIndexer` explicitly before consulting the side table, so an
// imported Auto-body indexer never appears in `collectInstanceStorage`/
// `collectRecordStorage`). `sema::isAutoPropertySymbol` is deliberately
// LEFT UNCHANGED (still excludes indexers) — its many call sites (definite
// assignment, record equality, ARC) treat the predicate as "real storage
// member", which an indexer isn't; only this wire-bit repurposes it.
//
// CONFIRMED SYMPTOM (verified on HEAD 2765f74 with the issue's own two-
// module repro — a `public abstract property [i: int]: int { get; set; end; }`
// in a lib compiled to `-c` + a `public override property [i: int]: int { get;
// set; end; }` in an `--import` app): rejected with
//   app.bzl:4:3: override of `indexer` does not match the base member's
//             accessor set/get presence (§12.1)
// exit 1. The LOCAL twin (same source, no `import`) accepts. Post-fix:
// accepts and links; the cross-module dispatch emits a separate
// pre-existing `#2491` warning that the issue does not depend on (a
// codegen emitReturn-fallback hazard in §2.8.2 cross-module abstract-class
// indexer dispatch, filed separately — the iface-indexer twin in #2529
// runs correctly).
//
// Related: #2390 (property-side override fix, the parent of this residual),
// #2478 (the `isAutoProperty` bit + storage-layout consumers), #2400
// (`hasSetter` codegen-aligned bit + the property-side fallback
// `importedPropertyHasSetter`), #2529 (iface-indexer accessor-presence
// sibling, the indexer twin of #2380's ifaceProps fallback), #2491
// (pre-existing cross-module abstract-class indexer dispatch codegen
// warning this fix's accept unmasks — out of scope, filed separately).

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

struct TempFile2523 {
    std::string path;
    explicit TempFile2523(std::string p) : path(std::move(p)) {}
    ~TempFile2523() { std::remove(path.c_str()); }
};

void writeSource2523(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj2523(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// Compile B importing A -> exe, capturing stderr into `err`. Mirrors
// xmod_override_accessor_presence_2390_test.cpp's
// compileImportCaptureErr2390.
bool compileImportCaptureErr2523(const std::string& bSrc,
                                 const std::string& bExe,
                                 const std::vector<std::string>& aObjs,
                                 std::string& err) {
    namespace fs = std::filesystem;
    std::string errPath = (fs::temp_directory_path() /
                           ("bz2523_err_" + unique_stem("") +
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

}  // namespace

// ===========================================================================
// T1 — the issue's own repro: an IMPORTED abstract BLOCK-form indexer with
// an explicit `set` arm (`abstract property [i]: int { get; set; end; }`)
// overridden by a LOCAL block-form indexer with both `get` and `set` arms
// (matching read-write signature). Pre-fix, the §12.5 abstract-shorthand
// correction over-fired (`base->isIndexer || ...` always true for an
// indexer) and `bHasSetter` was wrongly overridden from `true` to `false`,
// producing the false `accessor set/get presence (§12.1)` reject. Post-fix,
// `importedPropertyIsAutoStorage(base)` correctly returns `false` for this
// block-form shape (the `MetaMember::isAutoProperty` bit is set only for
// Auto bodyKind, §2523), the correction does not fire, both sides agree
// they have a setter, the override ACCEPTS. Compile-only — the cross-
// module abstract-class indexer dispatch has a separate pre-existing #2491
// codegen warning this test does not pin.
// ===========================================================================
BEZEL_TEST(xmod_indexer_blockform_override_2523,
           imported_blockform_with_set_correct_blockform_override_accepts) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2523_t1_");
    TempFile2523 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2523 aObj((dir / (stem + "_a.o")).string());
    writeSource2523(aSrc.path,
                    "module BlockFormIdxLib2523T1; "
                    "public abstract class AbsBlockFormIdxBase2523T1 "
                    "  public abstract property [i: int]: int "
                    "    get; "
                    "    set; "
                    "  end; "
                    "end AbsBlockFormIdxBase2523T1;");
    BEZEL_EXPECT_TRUE(compileToObj2523(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;  // toolchain unavailable

    std::string bSrc =
        "module AppBlockFormIdxOverride2523T1; import BlockFormIdxLib2523T1; "
        "public class BlockFormIdxOverride2523T1: AbsBlockFormIdxBase2523T1 "
        "  private _n: int := 0; "
        "  public override property [i: int]: int "
        "    get => _n + i; "
        "    set => _n := value; "
        "  end; "
        "end BlockFormIdxOverride2523T1; "
        "public class C public static method Main(): int => 0; end C;";
    TempFile2523 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2523 bExe((dir / (stem + "_b")).string());
    writeSource2523(bSrcFile.path, bSrc);

    std::string err;
    bool success = compileImportCaptureErr2523(bSrcFile.path, bExe.path,
                                               {aObj.path}, err);
    BEZEL_EXPECT_TRUE(success);
    BEZEL_EXPECT_TRUE(err.find("accessor set/get presence") == std::string::npos);
}

// ===========================================================================
// T2 — LOCAL-BASE CONTROL: the matching block-form-with-set override against
// a SAME-MODULE abstract block-form indexer was, and remains, correctly
// accepted. Guards that the fix (which only tightens the imported-base
// fallback) leaves the already-working local path untouched.
// ===========================================================================
BEZEL_TEST(xmod_indexer_blockform_override_2523,
           local_blockform_with_set_correct_blockform_override_accepts) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2523_t2_");
    std::string bSrc =
        "module LocalBlockFormIdx2523T2; "
        "public abstract class AbsBlockFormIdxBaseLocal2523T2 "
        "  public abstract property [i: int]: int "
        "    get; "
        "    set; "
        "  end; "
        "end AbsBlockFormIdxBaseLocal2523T2; "
        "public class BlockFormIdxOverrideLocal2523T2: AbsBlockFormIdxBaseLocal2523T2 "
        "  private _n: int := 0; "
        "  public override property [i: int]: int "
        "    get => _n + i; "
        "    set => _n := value; "
        "  end; "
        "end BlockFormIdxOverrideLocal2523T2; "
        "public class C public static method Main(): int => 0; end C;";
    TempFile2523 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2523 bExe((dir / (stem + "_b")).string());
    writeSource2523(bSrcFile.path, bSrc);

    std::string err;
    bool success =
        compileImportCaptureErr2523(bSrcFile.path, bExe.path, {}, err);
    BEZEL_EXPECT_TRUE(success);
    BEZEL_EXPECT_TRUE(err.find("accessor set/get presence") == std::string::npos);
}

// ===========================================================================
// T3 — REJECT (false-accept guard, accessor mismatch): an IMPORTED abstract
// block-form indexer WITH a `set` arm overridden by a get-only LOCAL
// expression-bodied indexer is REJECTED — the override drops a setter the
// base declares (§12.1 accessor-presence leg, the symmetric inverse of T1).
// Without the fix this was already rejected (the §12.5 correction only
// flipped `true` → `false`, never the other way), so this test pins that
// the fix does not over-relax the inverse direction.
// ===========================================================================
BEZEL_TEST(xmod_indexer_blockform_override_2523,
           imported_blockform_with_set_getonly_override_rejected_12_1) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2523_t3_");
    TempFile2523 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2523 aObj((dir / (stem + "_a.o")).string());
    writeSource2523(aSrc.path,
                    "module BlockFormIdxLib2523T3; "
                    "public abstract class AbsBlockFormIdxBase2523T3 "
                    "  public abstract property [i: int]: int "
                    "    get; "
                    "    set; "
                    "  end; "
                    "end AbsBlockFormIdxBase2523T3;");
    BEZEL_EXPECT_TRUE(compileToObj2523(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    std::string bSrc =
        "module AppGetterOnlyIdxOverride2523T3; import BlockFormIdxLib2523T3; "
        "public class BadGetterOnlyIdxOverride2523T3: AbsBlockFormIdxBase2523T3 "
        "  private _n: int := 0; "
        "  public override property [i: int]: int => _n + i; "
        "end BadGetterOnlyIdxOverride2523T3; "
        "public class C public static method Main(): int => 0; end C;";
    TempFile2523 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2523 bExe((dir / (stem + "_b")).string());
    writeSource2523(bSrcFile.path, bSrc);

    std::string err;
    bool success = compileImportCaptureErr2523(bSrcFile.path, bExe.path,
                                               {aObj.path}, err);
    BEZEL_EXPECT_TRUE(!success);
    BEZEL_EXPECT_TRUE(err.find("12.1") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("override of `indexer`") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("accessor set/get presence") != std::string::npos);
}

// ===========================================================================
// T4 — REJECT (canonical idiom preserved): an IMPORTED abstract
// §12.5-shorthand indexer (`abstract property [i]: T;`, get-only) overridden
// by a block-form LOCAL indexer with an added `set` arm is REJECTED — the
// override ADDS a setter the base lacks. This is the symmetric inverse of
// the canonical `abstract property`/`abstract indexer` + get-only override
// idiom (#1614's lit cases, #2390's T4). Without the fix, the §12.5
// correction over-fired for ALL abstract indexers (treating them as
// shorthand regardless of body shape); with the fix, it fires ONLY for
// Auto-body abstract indexers, leaving a block-form abstract indexer with a
// real `set` arm alone (and therefore this symmetric-direction reject still
// triggers correctly).
// ===========================================================================
BEZEL_TEST(xmod_indexer_blockform_override_2523,
           imported_shorthand_indexer_blockform_override_rejected_12_1) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2523_t4_");
    TempFile2523 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2523 aObj((dir / (stem + "_a.o")).string());
    writeSource2523(aSrc.path,
                    "module ShorthandIdxLib2523T4; "
                    "public abstract class AbsShorthandIdxBase2523T4 "
                    "  public abstract property [i: int]: int; "
                    "end AbsShorthandIdxBase2523T4;");
    BEZEL_EXPECT_TRUE(compileToObj2523(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    std::string bSrc =
        "module AppBlockFormIdxOverride2523T4; import ShorthandIdxLib2523T4; "
        "public class BadBlockFormIdxOverride2523T4: AbsShorthandIdxBase2523T4 "
        "  private _n: int := 0; "
        "  public override property [i: int]: int "
        "    get => _n + i; "
        "    set => _n := value; "
        "  end; "
        "end BadBlockFormIdxOverride2523T4; "
        "public class C public static method Main(): int => 0; end C;";
    TempFile2523 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2523 bExe((dir / (stem + "_b")).string());
    writeSource2523(bSrcFile.path, bSrc);

    std::string err;
    bool success = compileImportCaptureErr2523(bSrcFile.path, bExe.path,
                                               {aObj.path}, err);
    BEZEL_EXPECT_TRUE(!success);
    BEZEL_EXPECT_TRUE(err.find("12.1") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("override of `indexer`") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("accessor set/get presence") != std::string::npos);
}

// ===========================================================================
// T5 — ACCEPT (false-reject guard, canonical idiom): a get-only LOCAL
// expression-bodied override of an IMPORTED abstract §12.5-shorthand
// indexer still compiles. This is the exact shape a too-aggressive
// fallback would FALSE-REJECT: the #2400 `MetaMember::hasSetter` bit
// spuriously reports a setter for the abstract §12.5 shorthand base, so
// the fix MUST apply the §12.5 abstract-Auto correction (mirroring the
// local predicate) or this canonical idiom regresses. Now that
// `MetaMember::isAutoProperty` is set for Auto-body indexers (#2523),
// `importedPropertyIsAutoStorage(base)` returns `true` and the correction
// fires correctly. Mirrors #2390's T4 / #2529's canonical-idiom shape.
// ===========================================================================
BEZEL_TEST(xmod_indexer_blockform_override_2523,
           imported_shorthand_indexer_getonly_override_accepts) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2523_t5_");
    TempFile2523 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2523 aObj((dir / (stem + "_a.o")).string());
    writeSource2523(aSrc.path,
                    "module ShorthandIdxLib2523T5; "
                    "public abstract class AbsShorthandIdxBase2523T5 "
                    "  public abstract property [i: int]: int; "
                    "end AbsShorthandIdxBase2523T5;");
    BEZEL_EXPECT_TRUE(compileToObj2523(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    std::string bSrc =
        "module AppGetOnlyIdxOverride2523T5; import ShorthandIdxLib2523T5; "
        "public class GoodGetOnlyIdxOverride2523T5: AbsShorthandIdxBase2523T5 "
        "  private _n: int := 0; "
        "  public override property [i: int]: int => _n + i; "
        "end GoodGetOnlyIdxOverride2523T5; "
        "public class C public static method Main(): int => 0; end C;";
    TempFile2523 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2523 bExe((dir / (stem + "_b")).string());
    writeSource2523(bSrcFile.path, bSrc);

    std::string err;
    bool success = compileImportCaptureErr2523(bSrcFile.path, bExe.path,
                                               {aObj.path}, err);
    BEZEL_EXPECT_TRUE(success);
    BEZEL_EXPECT_TRUE(err.find("accessor set/get presence") == std::string::npos);
}

}  // namespace bezel::module