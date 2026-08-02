// fix #2529 (§11.2 / §23.4 / §2.8.2 clause 1 / §2.8.3 rule 4, v0.236): the
// iface-indexer obligations walker (`Resolver::checkAbstractImplementations`
// in src/sema/TypeResolution_Checks.cpp) had an explicit `if (!ipd) continue;`
// skip at the head of its loop — `ips->decl` is null for any IMPORTED
// interface indexer (ModuleLoader's `case MetaMemberKind::Indexer:`
// reconstructs `indexerParams` as ParameterSymbols but attaches no
// `ast::PropertyDecl`, because "the getter BODY is not reconstructed here"
// — there is no syntactic AST to attach, #2414), so the entire check
// (parameter-list match, §2438 missing-indexer diagnostic, AND the
// §11.2/§23.4 getter-only-vs-read-write setter diagnostic from #2417) was
// SILENTLY skipped for an imported interface indexer. The LOCAL twin — same
// source, no `import` — was, and remains, correctly REJECTED with the
// §11.2/§23.4 diagnostic; only the cross-module path silently accepted a
// getter-only impl of a read-write obligation. Verified on HEAD (5ca8606)
// with the issue's own two-module repro: `b[1]` on a `property [i: int]:
// int` interface with a getter-only local impl returned `_v + i = 8` (the
// getter ran, no diagnostic, exit 0); the LOCAL twin at the same commit
// produced "class `Impl...` implements interface `IIdx...`'s indexer with a
// getter-only implementation, but the interface's indexer is declared
// read-write — an implementation of a read-write interface indexer must
// provide a setter (§11.2 / §23.4)".
//
// This is the indexer sibling of #2390's property-side fix (which closed
// the imported-base override's accessor-presence gate by consuming the
// #2400 `MetaMember::hasSetter` bit through `ctx_.importedPropertyHasSetter`)
// and the indexer twin of #2380's `ifaceProps` type-compare fallback to
// `ctx_.importedMemberType`. The fix extends the iface-indexer loop's
// accessor-presence read with the same MetaMember::hasSetter fallback the
// loop's candidate-side check (#2449) already uses for an imported
// CANDIDATE (`cpd ? propertyDeclHasSetter(cpd) :
// ctx_.importedPropertyHasSetter(candidate)`), and adds analogous
// `ifaceParamCount`/`ifaceParamTy` fallbacks for the interface's own
// parameter list (mirroring the candidate-side `candParamCount`/`candParamTy`
// fallbacks #2449 already in place). No new metadata bit is needed — every
// imported indexer obligation is fully reconstructable from what #2414
// (`PropertySymbol::indexerParams`, ParameterSymbols with
// `resolvedDeclType`) and #2400 (`MetaMember::hasSetter` ->
// `propHasSetter_` -> `ctx_.importedPropertyHasSetter`) already publish.
// The `if (!ipd) continue;` is REMOVED entirely — every code path below
// the `ifaceHasSetter`/`ifaceParamCount`/`ifaceParamTy` helpers falls
// back to the imported side, so an imported obligation can no longer be
// skipped.
//
// CONFIRMED SYMPTOM (verified by direct bezelc probe on HEAD before
// writing this test, then mechanically by reverting the src/ fix and
// re-running this filter — see the commit message): the issue's own
// `lib.bzl`/`app.bzl` two-module repro compiles clean (exit 0) and runs
// to `8` (the getter ran, the missing setter was simply never checked)
// without the fix; post-fix, compile FAILS with the §11.2/§23.4
// diagnostic text. The LOCAL twin remains rejected in both cases (T2
// control).
//
// Related: #2417 (the original accessor-presence gate, local-only),
// #2390 (the property-side sibling — accessor-presence of an override of
// an IMPORTED base property), #2414 (ModuleLoader's Indexer case
// reconstruction), #2400 (`MetaMember::hasSetter` metadata bit +
// `propHasSetter_` side table), #2380 (the `ifaceProps` type-compare
// fallback that the iface-indexer loop above finally mirrors).

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

struct TempFile2529 {
    std::string path;
    explicit TempFile2529(std::string p) : path(std::move(p)) {}
    ~TempFile2529() { std::remove(path.c_str()); }
};

void writeSource2529(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj2529(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// Compile B importing A's compiled object -> exe, capturing the driver's
// stderr (fd 2) into `err`. Mirrors xmod_override_signature_2386_test.cpp's
// compileImportCaptureErr2386 / xmod_iface_property_setter_2406_test.cpp's
// `BEZEL_TEST` setup.
bool compileImportCaptureErr2529(const std::string& bSrc,
                                 const std::string& bExe,
                                 const std::vector<std::string>& aObjs,
                                 std::string& err) {
    namespace fs = std::filesystem;
    std::string errPath = (fs::temp_directory_path() /
                           ("bz2529_err_" + std::to_string(std::rand()) +
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

// Compile B importing A -> exe, run, return exit code (-1 on compile/link/
// run failure, -2 if the dependency itself could not be produced).
int compileImportAndRun2529(const std::string& libSrc,
                            const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2529_" + std::to_string(std::rand());
    TempFile2529 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2529 aObj((dir / (stem + "_a.o")).string());
    writeSource2529(aSrc.path, libSrc);
    if (!compileToObj2529(aSrc.path, aObj.path)) return -2;
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile2529 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2529 bExe((dir / stem).string());
    writeSource2529(bSrc.path, appSrc);
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
// T1 — the issue's own repro: an IMPORTED read-write interface INDEXER
// implemented by a getter-only LOCAL class. Pre-fix: `ips->decl` is null for
// the imported interface indexer, the `if (!ipd) continue;` skip fires, the
// §11.2/§23.4 setter-presence diagnostic is never produced — the program
// compiles clean and `b[1]` returns 8. Post-fix: compile FAILS with the same
// §11.2/§23.4 diagnostic text the LOCAL twin emits (#2417). The property
// TYPE matches (both `int`), the parameter count matches (both `[int]`), and
// the only dimension the check looks at is accessor presence — pinning the
// fix specifically.
// ===========================================================================
BEZEL_TEST(xmod_iface_indexer_setter_2529,
           imported_readwrite_iface_getter_only_impl_rejected_11_2_23_4) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2529_t1_" + std::to_string(std::rand());
    TempFile2529 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2529 aObj((dir / (stem + "_a.o")).string());
    writeSource2529(aSrc.path,
                    "module ImportedRWIdxLib2529T1; "
                    "public interface IRWIdx2529T1 "
                    "  property [i: int]: int; "  // read-write (no `readonly`)
                    "end IRWIdx2529T1;");
    BEZEL_EXPECT_TRUE(compileToObj2529(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;  // toolchain unavailable

    std::string bSrc =
        "module AppGetterOnlyImpl2529T1; import ImportedRWIdxLib2529T1; "
        "public class GetterOnlyImpl2529T1: IRWIdx2529T1 "
        "  private _v: int := 7; "
        "  public property [i: int]: int => _v + i; "  // getter-only
        "end GetterOnlyImpl2529T1; "
        "public class C "
        "  public static method Main(): int "
        "    using var b: IRWIdx2529T1 := new GetterOnlyImpl2529T1(); "
        "    return b[1]; "
        "  end Main; "
        "end C;";
    TempFile2529 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2529 bExe((dir / (stem + "_b")).string());
    writeSource2529(bSrcFile.path, bSrc);

    std::string err;
    bool success = compileImportCaptureErr2529(bSrcFile.path, bExe.path,
                                               {aObj.path}, err);
    BEZEL_EXPECT_TRUE(!success);
    BEZEL_EXPECT_TRUE(err.find("11.2") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("23.4") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("indexer") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("getter-only") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("read-write") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("setter") != std::string::npos);
}

// ===========================================================================
// T2 — LOCAL-BASE CONTROL: the identical mismatch against a SAME-MODULE
// interface (#2417's own shape, retained verbatim). Guards that the fix
// (which only adds the imported-side fallbacks) leaves the already-working
// local path untouched. Re-runs the local twin to confirm the diagnostic
// text matches T1's exactly (proves a single source of truth: the iface-
// indexer obligations walker produces one diagnostic for both shapes).
// ===========================================================================
BEZEL_TEST(xmod_iface_indexer_setter_2529,
           local_readwrite_iface_getter_only_impl_still_rejected_11_2_23_4) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2529_t2_" + std::to_string(std::rand());
    std::string src =
        "module LocalRWIdx2529T2; "
        "public interface IRWIdxLocal2529T2 "
        "  property [i: int]: int; "
        "end IRWIdxLocal2529T2; "
        "public class GetterOnlyImplLocal2529T2: IRWIdxLocal2529T2 "
        "  private _v: int := 7; "
        "  public property [i: int]: int => _v + i; "
        "end GetterOnlyImplLocal2529T2; "
        "public class C public static method Main(): int => 0; end C;";
    TempFile2529 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2529 bExe((dir / (stem + "_b")).string());
    writeSource2529(bSrcFile.path, src);

    std::string err;
    bool success =
        compileImportCaptureErr2529(bSrcFile.path, bExe.path, {}, err);
    BEZEL_EXPECT_TRUE(!success);
    BEZEL_EXPECT_TRUE(err.find("11.2") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("23.4") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("getter-only") != std::string::npos);
}

// ===========================================================================
// T3 — BLOCK-FORM getter-only (regression guard for the
// expression-bodied-vs-block-form shape distinction, identical to #2417's
// block-form reject case). An imported read-write interface indexer, the
// implementing LOCAL class providing a block-form `get` arm and NO `set`
// arm. The §11.2/§23.4 diagnostic must fire here too — the fix's
// `ifaceHasSetter` predicate reads `propertyDeclHasSetter` for a local AST
// (would have answered `false` for a block-form-without-set, as it did
// before #2529) but, for the IMPORTED interface side, must read the
// reconstructed `MetaMember::hasSetter` bit (a block-form interface indexer
// with no `set` arm exports `hasSetter=false`, so the candidate's
// getter-only shape still fails). The match logic is identical to T1's;
// this case confirms the shape dimension is independent of expression-bodied
// vs block-form.
// ===========================================================================
BEZEL_TEST(xmod_iface_indexer_setter_2529,
           imported_readwrite_iface_block_form_getter_only_rejected_11_2_23_4) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2529_t3_" + std::to_string(std::rand());
    TempFile2529 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2529 aObj((dir / (stem + "_a.o")).string());
    writeSource2529(aSrc.path,
                    "module ImportedRWIdxLib2529T3; "
                    "public interface IRWIdx2529T3 "
                    "  property [i: int]: int; "
                    "end IRWIdx2529T3;");
    BEZEL_EXPECT_TRUE(compileToObj2529(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    std::string bSrc =
        "module AppBlockForm2529T3; import ImportedRWIdxLib2529T3; "
        "public class BlockFormGetterOnly2529T3: IRWIdx2529T3 "
        "  private _v: int := 10; "
        "  public property [i: int]: int "
        "    get => _v + i; "
        "  end; "
        "end BlockFormGetterOnly2529T3; "
        "public class C public static method Main(): int => 0; end C;";
    TempFile2529 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2529 bExe((dir / (stem + "_b")).string());
    writeSource2529(bSrcFile.path, bSrc);

    std::string err;
    bool success = compileImportCaptureErr2529(bSrcFile.path, bExe.path,
                                               {aObj.path}, err);
    BEZEL_EXPECT_TRUE(!success);
    BEZEL_EXPECT_TRUE(err.find("11.2") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("23.4") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("getter-only") != std::string::npos);
}

// ===========================================================================
// T4 — ACCEPT + RUN + VALUE (false-reject guard, read-write vs read-write):
// a CORRECT get+set impl of an IMPORTED read-write interface indexer still
// compiles and dispatches the right value across the module boundary. This
// is the shape a naive fallback would FALSE-REJECT: the importer's
// `ifaceHasSetter` MUST answer `true` for a read-write imported interface
// indexer (it does — `MetaMember::hasSetter` is set on the builder side for
// the interface declaration, #2400), so the candidate's setter-vs-interface-
// setter check both pass. Compile-and-run with the canonical read-write
// shape: set to 42 with index 5, then read back index 0, both through the
// imported interface reference, expected 37 (`_v := 42 - 5 = 37`).
// ===========================================================================
BEZEL_TEST(xmod_iface_indexer_setter_2529,
           imported_readwrite_iface_readwrite_impl_runs_correct_value) {
    int r = compileImportAndRun2529(
        "module ImportedRWIdxLib2529T4; "
        "public interface IRWIdx2529T4 "
        "  property [i: int]: int; "
        "end IRWIdx2529T4;",
        "module AppRWImpl2529T4; import ImportedRWIdxLib2529T4; "
        "public class RWImpl2529T4: IRWIdx2529T4 "
        "  private _v: int := 10; "
        "  public property [i: int]: int "
        "    get => _v + i; "
        "    set => _v := value - i; "
        "  end; "
        "end RWImpl2529T4; "
        "public class C "
        "  public static method Main(): int "
        "    using var b: IRWIdx2529T4 := new RWImpl2529T4(); "
        "    b[5] := 42; "
        "    return b[0]; "
        "  end Main; "
        "end C;");
    if (r == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 37);
}

// ===========================================================================
// T5 — ACCEPT + RUN + VALUE (false-reject guard, getter-only vs readonly
// obligation): a CORRECT getter-only impl of an IMPORTED `readonly`
// interface indexer still compiles and dispatches the right value. This is
// the canonical "getter-only vs readonly" idiom the §11.2 one-directional
// relaxation explicitly permits; the fix MUST continue to accept it
// because `ifaceHasSetter()` answers `false` for the readonly interface
// (`MetaMember::hasSetter` is unset on a `readonly` declaration, #2400),
// so the inner setter-presence check is skipped — satisfied. Compile-and-
// run with the getter-only shape, expected 15 (`_v + 5 = 10 + 5`).
// ===========================================================================
BEZEL_TEST(xmod_iface_indexer_setter_2529,
           imported_readonly_iface_getter_only_impl_runs_correct_value) {
    int r = compileImportAndRun2529(
        "module ImportedReadonlyIdxLib2529T5; "
        "public interface IReadonlyIdx2529T5 "
        "  readonly property [i: int]: int; "
        "end IReadonlyIdx2529T5;",
        "module AppReadonlyImpl2529T5; "
        "import ImportedReadonlyIdxLib2529T5; "
        "public class ReadonlyImpl2529T5: IReadonlyIdx2529T5 "
        "  private _v: int := 10; "
        "  public property [i: int]: int => _v + i; "
        "end ReadonlyImpl2529T5; "
        "public class C "
        "  public static method Main(): int "
        "    using var b: IReadonlyIdx2529T5 := "
        "        new ReadonlyImpl2529T5(); "
        "    return b[5]; "
        "  end Main; "
        "end C;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 15);
}

// ===========================================================================
// T6 — ACCEPT + RUN + VALUE (false-reject guard, parameter-list shape
// correctness): an IMPORTED interface indexer with a NON-TRIVIAL parameter
// signature (`[s: string, i: int]: int`, two parameters) is implemented by
// a LOCAL class whose indexer matches the parameter COUNT and TYPES. The
// fix's `ifaceParamCount`/`ifaceParamTy` fallbacks must read the
// reconstructed `PropertySymbol::indexerParams` (ParameterSymbols with
// `resolvedDeclType`) — not the local AST (which would be null on the
// imported side). A naive local-AST-only read would reject the obligation
// outright (zero local params vs two actual), which would be a false
// reject of a perfectly correct shape. The candidate side uses two
// parameters; `b["hi", 5]` resolves through it to return `_v + 5 = 10 +
// 5 = 15`. Isolates the `ifaceParamCount`/`ifaceParamTy` fallback from
// the `ifaceHasSetter` fallback — T1/T3/T4 exercise the latter; this
// exercises the former.
// ===========================================================================
BEZEL_TEST(xmod_iface_indexer_setter_2529,
           imported_iface_indexer_two_param_match_runs_correct_value) {
    int r = compileImportAndRun2529(
        "module ImportedTwoParamIdxLib2529T6; "
        "public interface ITwoParamIdx2529T6 "
        "  property [s: string, i: int]: int; "
        "end ITwoParamIdx2529T6;",
        "module AppTwoParam2529T6; "
        "import ImportedTwoParamIdxLib2529T6; "
        "public class TwoParamImpl2529T6: ITwoParamIdx2529T6 "
        "  private _v: int := 10; "
        "  public property [s: string, i: int]: int "
        "    get => _v + i; "
        "    set => _v := value - i; "
        "  end; "
        "end TwoParamImpl2529T6; "
        "public class C "
        "  public static method Main(): int "
        "    using var b: ITwoParamIdx2529T6 := "
        "        new TwoParamImpl2529T6(); "
        "    b[\"hi\", 5] := 42; "
        "    return b[\"\", 0]; "
        "  end Main; "
        "end C;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 37);
}

}  // namespace bezel::module