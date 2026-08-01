// fix #2391 (§14.2 / §2.8.2 / §2.8.3, v0.236): the §14.2 (v0.182, #1579)
// generic-constraint-restatement check in
// `Resolver::checkOverrideSignatureMatch`
// (`src/sema/TypeResolution_Checks.cpp`) — which requires an `override` of a
// generic method to either omit its `where` clauses (inheriting the base's
// constraint set exactly) or restate an equivalent set, rejecting a differing
// (added/removed/narrowed/widened) clause — was gated on `bDecl` (the base's
// `ast::MethodDecl*`). `bDecl` is unconditionally null when the base is an
// IMPORTED generic method: ModuleLoader never attaches an AST to a body-less
// imported declaration (§2.8.2/§2.8.3: the artifact interface carries no
// AST). So a locally-declared override of an IMPORTED generic virtual/
// abstract method could restate a `where T: ...` clause that narrows,
// widens, adds, or drops relative to the imported base's REAL constraint
// set, and it would compile clean — the same defect family as the rest of
// #2386 (return type, parameter types), #2390 (accessor presence), #2392
// (when-postcond).
//
// This is the imported-base twin of #2374's astNode-gate audit, and the
// METHOD twin of #2390 (property accessor-presence). #2386 deliberately
// DEFERRED this check (its metadata schema then carried no constraint
// payload). The constraint payload has been carried since #1579/v0.182
// (`MetaGenericParam::constraintTypes`/`specialConstraints`, populated by
// `ModuleMetadataBuilder::convertGenericParams`); the gap was purely on the
// import side — `ModuleLoader::convertGenericParams` only read `name`/
// `variance` off each `MetaGenericParam` (the `GenericParamSymbol` carries
// nothing else), dropping the constraint data. This fix adds a per-method
// side table `ctx_.importedGenericConstraints(MethodSymbol*)` populated at
// import time from the same metadata, mirroring the `importedReturnType` /
// `importedMemberType` / `importedPropertyHasSetter` shape, and a parallel
// `derivedConstraintMatchesImported` comparison helper (the imported analog
// of `constraintSetsEquivalent`) consulted when `bDecl == nullptr`.
//
// CONFIRMED SYMPTOM (verified by direct bezelc probe on origin/main before
// writing this test, then mechanically by reverting the src/ fix and re-running
// this filter — see the commit message): overriding an IMPORTED
// `public virtual method Send<T>(x: T): int where T: class` with
// `public override method Send<T>(x: T): int where T: value` compiled clean
// (exit 0). The identical mismatch against a SAME-MODULE base was, and
// remains, correctly REJECTED — confirming the defect is the cross-module
// path, not a general hole in §14.2 constraint matching.
//
// Related: #2386 (return-type/param-type twins, same function), #2390
// (accessor-presence twin), #2392 (when-postcond twin), #2374 (the astNode-gate
// audit family), #1579 (the original §14.2 override-restatement rule, local
// only).

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

namespace bezel::module {
namespace {

struct TempFile2391 {
    std::string path;
    explicit TempFile2391(std::string p) : path(std::move(p)) {}
    ~TempFile2391() { std::remove(path.c_str()); }
};

void writeSource2391(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj2391(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// Compile B importing A, capturing the driver's stderr (fd 2) into `err`.
// Mirrors xmod_override_accessor_presence_2390_test.cpp's helper.
bool compileImportCaptureErr2391(const std::string& bSrc,
                                 const std::string& bExe,
                                 const std::vector<std::string>& aObjs,
                                 std::string& err) {
    namespace fs = std::filesystem;
    std::string errPath = (fs::temp_directory_path() /
                           ("bz2391_err_" + std::to_string(std::rand()) +
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
int compileImportAndRun2391(const std::string& libSrc,
                            const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2391_" + std::to_string(std::rand());
    TempFile2391 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2391 aObj((dir / (stem + "_a.o")).string());
    writeSource2391(aSrc.path, libSrc);
    if (!compileToObj2391(aSrc.path, aObj.path)) return -2;
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile2391 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2391 bExe((dir / stem).string());
    writeSource2391(bSrc.path, appSrc);
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

// True iff the §14.2 override-restates-constraint diagnostic was emitted.
bool saw14_2RestateError(const std::string& err) {
    return err.find("14.2") != std::string::npos &&
           err.find("restates a `where") != std::string::npos;
}

}  // namespace

// ===========================================================================
// T1 — the confirmed false-accept shape from #2391: an IMPORTED generic
// virtual method declared `where T: class`, overridden with `where T: value`
// (a conflicting special-constraint kind) is REJECTED for §14.2
// constraint-restatement mismatch. Pre-fix, `bDecl` (base's ast::MethodDecl*)
// was null for the imported base, so the `if (dDecl && bDecl && ...)` gate
// skipped the constraint-restatement comparison and the override compiled
// clean. The return type, parameter type, and parameter count all match, so
// only the §14.2 leg catches it — pinning the fix specifically.
// ===========================================================================
BEZEL_TEST(xmod_override_generic_constraints_2391,
           imported_class_base_value_override_rejected_14_2) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2391_t1_" + std::to_string(std::rand());
    TempFile2391 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2391 aObj((dir / (stem + "_a.o")).string());
    writeSource2391(aSrc.path,
                    "module ImportedClassLib2391T1; "
                    "public class Base2391T1 "
                    "  public virtual method Send<T>(x: T): int "
                    "    where T: class "
                    "    return 0; "
                    "  end Send; "
                    "end Base2391T1;");
    BEZEL_EXPECT_TRUE(compileToObj2391(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;  // toolchain unavailable

    std::string bSrc =
        "module AppValueOverride2391T1; import ImportedClassLib2391T1; "
        "public class Bad : Base2391T1 "
        "  public override method Send<T>(x: T): int "
        "    where T: value "
        "    return 1; "
        "  end Send; "
        "end Bad; "
        "public class C public static method Main(): int => 0; end C;";
    TempFile2391 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2391 bExe((dir / (stem + "_b")).string());
    writeSource2391(bSrcFile.path, bSrc);

    std::string err;
    bool success = compileImportCaptureErr2391(bSrcFile.path, bExe.path,
                                               {aObj.path}, err);
    BEZEL_EXPECT_TRUE(!success);
    BEZEL_EXPECT_TRUE(saw14_2RestateError(err));
}

// ===========================================================================
// T2 — LOCAL-BASE CONTROL: the identical mismatch against a SAME-MODULE base
// was, and remains, correctly rejected. Guards that the fix (which only adds
// the imported-base fallback) leaves the already-working local path
// (`constraintSetsEquivalent`, the AST-vs-AST comparison) untouched.
// ===========================================================================
BEZEL_TEST(xmod_override_generic_constraints_2391,
           local_class_base_value_override_still_rejected_14_2) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2391_t2_" + std::to_string(std::rand());
    std::string bSrc =
        "module LocalClassLib2391T2; "
        "public class BaseL2391T2 "
        "  public virtual method Send<T>(x: T): int "
        "    where T: class "
        "    return 0; "
        "  end Send; "
        "end BaseL2391T2; "
        "public class BadL : BaseL2391T2 "
        "  public override method Send<T>(x: T): int "
        "    where T: value "
        "    return 1; "
        "  end Send; "
        "end BadL; "
        "public class C public static method Main(): int => 0; end C;";
    TempFile2391 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2391 bExe((dir / (stem + "_b")).string());
    writeSource2391(bSrcFile.path, bSrc);

    std::string err;
    bool success =
        compileImportCaptureErr2391(bSrcFile.path, bExe.path, {}, err);
    BEZEL_EXPECT_TRUE(!success);
    BEZEL_EXPECT_TRUE(saw14_2RestateError(err));
}

// ===========================================================================
// T3 — INVERSE direction: an IMPORTED generic base with NO `where` clause on
// `T`, overridden with an ADDED `where T: class` is REJECTED. The override
// ADDS a constraint the base does not have — the §14.2 "differing clause —
// added ... relative to the inherited set" case. Confirms the empty-set
// comparison on the imported side (imported base's reconstructed constraint
// is empty for this parameter; derived restates a non-empty clause).
// ===========================================================================
BEZEL_TEST(xmod_override_generic_constraints_2391,
           imported_unconstrained_base_class_added_override_rejected_14_2) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2391_t3_" + std::to_string(std::rand());
    TempFile2391 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2391 aObj((dir / (stem + "_a.o")).string());
    writeSource2391(aSrc.path,
                    "module ImportedUnconstrainedLib2391T3; "
                    "public class Base2391T3 "
                    "  public virtual method Send<T>(x: T): int "
                    "    return 0; "
                    "  end Send; "
                    "end Base2391T3;");
    BEZEL_EXPECT_TRUE(compileToObj2391(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    std::string bSrc =
        "module AppAddedOverride2391T3; "
        "import ImportedUnconstrainedLib2391T3; "
        "public class Bad : Base2391T3 "
        "  public override method Send<T>(x: T): int "
        "    where T: class "
        "    return 1; "
        "  end Send; "
        "end Bad; "
        "public class C public static method Main(): int => 0; end C;";
    TempFile2391 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2391 bExe((dir / (stem + "_b")).string());
    writeSource2391(bSrcFile.path, bSrc);

    std::string err;
    bool success = compileImportCaptureErr2391(bSrcFile.path, bExe.path,
                                               {aObj.path}, err);
    BEZEL_EXPECT_TRUE(!success);
    BEZEL_EXPECT_TRUE(saw14_2RestateError(err));
}

// ===========================================================================
// T4 — ACCEPT (false-reject guard, the canonical idiom): an override that
// OMITS its `where` clauses, inheriting the imported base's `where T: class`
// constraint set exactly, still compiles. This is the §14.2 "recommended
// spelling"; a naive fallback that mis-defaulted the imported constraint set
// would false-reject this. Mirrors #2390's T4 / #2386's T7 accept-control
// shape. (The §14.2 check fires only when the override RESTATES a clause;
// omitted → inherit exactly → no comparison. The cross-module dispatch of a
// generic virtual through an imported base-typed reference is exercised by
// the same-module virtual_generic_override_580 test family — out of scope
// here.)
// ===========================================================================
BEZEL_TEST(xmod_override_generic_constraints_2391,
           omitted_clause_inherits_imported_class_constraint_compiles) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2391_t4_" + std::to_string(std::rand());
    TempFile2391 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2391 aObj((dir / (stem + "_a.o")).string());
    writeSource2391(aSrc.path,
                    "module ImportedClassLib2391T4; "
                    "public class Base2391T4 "
                    "  public virtual method Send<T>(x: T): int "
                    "    where T: class "
                    "    return 0; "
                    "  end Send; "
                    "end Base2391T4;");
    BEZEL_EXPECT_TRUE(compileToObj2391(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    std::string bSrc =
        "module AppOmitOverride2391T4; import ImportedClassLib2391T4; "
        "public class Good : Base2391T4 "
        "  public override method Send<T>(x: T): int "
        "    return 1; "
        "  end Send; "
        "end Good; "
        "public class C public static method Main(): int => 0; end C;";
    TempFile2391 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2391 bExe((dir / (stem + "_b")).string());
    writeSource2391(bSrcFile.path, bSrc);

    std::string err;
    bool success = compileImportCaptureErr2391(bSrcFile.path, bExe.path,
                                               {aObj.path}, err);
    BEZEL_EXPECT_TRUE(success);
    BEZEL_EXPECT_TRUE(err.find("14.2") == std::string::npos);
}

// ===========================================================================
// T5 — ACCEPT (false-reject guard, identical restatement): an override that
// RESTATES the imported base's `where T: class` clause verbatim still
// compiles. The §14.2 "restated in an equivalent form" branch. Compile-only
// (the dispatch is the same as T4; the new code path here is the imported-
// side multiset-match accepting a non-empty derived clause against a non-
// empty imported clause).
// ===========================================================================
BEZEL_TEST(xmod_override_generic_constraints_2391,
           imported_class_base_restate_identical_class_compiles) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2391_t5_" + std::to_string(std::rand());
    TempFile2391 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2391 aObj((dir / (stem + "_a.o")).string());
    writeSource2391(aSrc.path,
                    "module ImportedClassLib2391T5; "
                    "public class Base2391T5 "
                    "  public virtual method Send<T>(x: T): int "
                    "    where T: class "
                    "    return 0; "
                    "  end Send; "
                    "end Base2391T5;");
    BEZEL_EXPECT_TRUE(compileToObj2391(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    std::string bSrc =
        "module AppRestateOverride2391T5; import ImportedClassLib2391T5; "
        "public class Good : Base2391T5 "
        "  public override method Send<T>(x: T): int "
        "    where T: class "
        "    return 1; "
        "  end Send; "
        "end Good; "
        "public class C public static method Main(): int => 0; end C;";
    TempFile2391 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2391 bExe((dir / (stem + "_b")).string());
    writeSource2391(bSrcFile.path, bSrc);

    std::string err;
    bool success = compileImportCaptureErr2391(bSrcFile.path, bExe.path,
                                               {aObj.path}, err);
    BEZEL_EXPECT_TRUE(success);
    BEZEL_EXPECT_TRUE(err.find("14.2") == std::string::npos);
}

// ===========================================================================
// T6 — ACCEPT (false-reject guard, multi-clause equivalent restatement): an
// override that RESTATES a COMPOUND imported base clause (`where T: class,
// INum` — a special kind AND a type-constraint) in the SAME ORDER-INSSENSITIVE
// form (`where T: INum, class`) still compiles. Confirms the imported-side
// multiset match handles BOTH the special-kind bag and the type-constraint
// TypeRef bag (the two-bag structure of `derivedConstraintMatchesImported`),
// not just the single-kind case. Compile-only.
// ===========================================================================
BEZEL_TEST(xmod_override_generic_constraints_2391,
           imported_compose_base_restate_reordered_compiles) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2391_t6_" + std::to_string(std::rand());
    TempFile2391 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2391 aObj((dir / (stem + "_a.o")).string());
    writeSource2391(aSrc.path,
                    "module ImportedComposeLib2391T6; "
                    "public interface INum2391T6 "
                    "  method Val(): int; "
                    "end INum2391T6; "
                    "public class Base2391T6 "
                    "  public virtual method Send<T>(x: T): int "
                    "    where T: class, INum2391T6 "
                    "    return 0; "
                    "  end Send; "
                    "end Base2391T6;");
    BEZEL_EXPECT_TRUE(compileToObj2391(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    // Restate in REORDERED form: type-constraint first, then special — same
    // multiset, must ACCEPT (constraintSetsEquivalent is order-insensitive;
    // derivedConstraintMatchesImported is too).
    std::string bSrc =
        "module AppReorderOverride2391T6; import ImportedComposeLib2391T6; "
        "public class Good : Base2391T6 "
        "  public override method Send<T>(x: T): int "
        "    where T: INum2391T6, class "
        "    return 1; "
        "  end Send; "
        "end Good; "
        "public class C public static method Main(): int => 0; end C;";
    TempFile2391 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2391 bExe((dir / (stem + "_b")).string());
    writeSource2391(bSrcFile.path, bSrc);

    std::string err;
    bool success = compileImportCaptureErr2391(bSrcFile.path, bExe.path,
                                               {aObj.path}, err);
    BEZEL_EXPECT_TRUE(success);
    BEZEL_EXPECT_TRUE(err.find("14.2") == std::string::npos);
}

// ===========================================================================
// T7 — REJECT (compose-drops-element): an override of an IMPORTED base that
// had `where T: class, INum` (special + type), restated as `where T: INum`
// (dropping the `class` special), is REJECTED — a §14.2 "removed" narrowing
// relative to the inherited set. Confirms the imported-side multiset match
// detects a missing special kind across the compound type+special mix.
// ===========================================================================
BEZEL_TEST(xmod_override_generic_constraints_2391,
           imported_compose_base_drops_class_rejected_14_2) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2391_t7_" + std::to_string(std::rand());
    TempFile2391 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2391 aObj((dir / (stem + "_a.o")).string());
    writeSource2391(aSrc.path,
                    "module ImportedComposeLib2391T7; "
                    "public interface INum2391T7 "
                    "  method Val(): int; "
                    "end INum2391T7; "
                    "public class Base2391T7 "
                    "  public virtual method Send<T>(x: T): int "
                    "    where T: class, INum2391T7 "
                    "    return 0; "
                    "  end Send; "
                    "end Base2391T7;");
    BEZEL_EXPECT_TRUE(compileToObj2391(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    std::string bSrc =
        "module AppDropOverride2391T7; import ImportedComposeLib2391T7; "
        "public class Bad : Base2391T7 "
        "  public override method Send<T>(x: T): int "
        "    where T: INum2391T7 "
        "    return 1; "
        "  end Send; "
        "end Bad; "
        "public class C public static method Main(): int => 0; end C;";
    TempFile2391 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2391 bExe((dir / (stem + "_b")).string());
    writeSource2391(bSrcFile.path, bSrc);

    std::string err;
    bool success = compileImportCaptureErr2391(bSrcFile.path, bExe.path,
                                               {aObj.path}, err);
    BEZEL_EXPECT_TRUE(!success);
    BEZEL_EXPECT_TRUE(saw14_2RestateError(err));
}

}  // namespace bezel::module
