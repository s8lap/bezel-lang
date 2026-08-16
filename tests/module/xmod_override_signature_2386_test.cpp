// fix #2386 (§12.1 / §2.8.2 / §2.8.3, v0.235): `Resolver::checkOverrideSignatureMatch`
// (method case) and its property-specific delegate
// `checkPropertyOverrideSignatureMatch` (`src/sema/TypeResolution_Checks.cpp`) gated
// their whole §12.1 signature comparison on both the derived AND the base member
// having a live `ast::MethodDecl`/`ast::PropertyDecl` (`if (dDecl && bDecl) { ... }`).
// `base` is resolved via a BFS over `cur->baseTypes` (the override-handling call site,
// ~line 2732 pre-fix) with NO same-module restriction, so `base` readily belongs to an
// IMPORTED ancestor class. `derived` is always LOCAL (from `t`'s own module); `base`
// is not. `ModuleLoader` never attaches an `ast::MethodDecl`/`ast::PropertyDecl` to a
// body-less imported declaration (§2.8.2/§2.8.3 — the artifact interface carries no
// AST), so `bDecl` was unconditionally null for ANY override of a virtual/abstract
// member declared on an imported base class — silently skipping the ENTIRE §12.1
// return-type/`owned`-return/property-type/accessor-presence comparison. A SECOND,
// independent gap in the same functions (not gated on `dDecl`/`bDecl` at all, but the
// same root defect class): the parameter-type loops read `bp->declType` directly
// (always null for an imported parameter — `ModuleLoader::convertParams` stashes the
// resolved type on `resolvedDeclType` instead, there being no `ast::Type` to attach),
// silently skipping the parameter-type comparison via `if (dt && btSubst && ...)`.
//
// This is the SEVENTH/EIGHTH confirmed instance of #2374's audit pattern (an
// `astNode`/`decl`-only gate silently mishandling an imported symbol), and the
// §12.1 OVERRIDE-MATCHING twin of #2373 (method conformance) / #2380 (property
// conformance) — both landed earlier the same day in this same file, §11.2
// interface-conformance checking rather than §12.1 override-matching.
//
// CONFIRMED SYMPTOM (verified by direct bezelc probe before writing this test, then
// mechanically by reverting the src/ fix via a saved patch and re-running this exact
// filter — see the commit message for the full mechanical-proof transcript):
//
//   * METHOD case (as reported in #2386): overriding an imported
//     `public abstract method Get(): int;` with `override method Get(): string =>
//     "garbage marker string";` compiled clean (exit 0). Dispatched through the
//     base-typed reference (`AbsBase2386`), `b.Get()` — statically typed `int` — read
//     a garbage int off the string-returning body's actual result: observed
//     `v=4610944`, EXACTLY the value #2386 reported.
//   * PROPERTY case: #2386 reported this shape compiling clean but had not yet
//     exercised the runtime value ("not yet exercised through a call site"). This fix
//     confirms it: overriding an imported `public abstract property Val: int;` with
//     `override property Val: string => _s;` also dispatches to a garbage int read
//     through the base-typed reference — observed `v=4610848`.
//   * PARAMETER-TYPE case (found during verification, independent of the
//     `dDecl`/`bDecl` gate above): overriding an imported
//     `public abstract method Set(x: int);` with `override method Set(x: string)
//     end Set;` also compiled clean pre-fix.
//   * LOCAL-BASE CONTROL: the identical method/property mismatches against a
//     SAME-MODULE base class were, and remain, correctly REJECTED both before and
//     after this fix — confirming the defect is specifically the cross-module path,
//     not a general hole in §12.1 matching.
//
// Two further gaps in these same two functions were found during verification and
// are DELIBERATELY NOT fixed here (each filed separately, per CLAUDE.md rule 4 —
// decline-with-reasons rather than invent an unsupported comparison):
//   * `checkPropertyOverrideSignatureMatch`'s accessor-presence (setter-exists) check
//     stays gated on `bDecl` — the module-metadata schema carries no accessor-
//     presence bit for a Property member, so it cannot be reconstructed for an
//     imported base the way property TYPE now can. Filed as #2390.
//   * `checkOverrideSignatureMatch`'s §14.2 generic-constraint-restatement check
//     (an override's `where` clauses on its OWN generic parameters) stays gated on
//     `bDecl` — `GenericParamSymbol` has no field to hold a reconstructed constraint
//     set even though `ModuleMetadataBuilder` already exports one. Filed as #2391.
//
// A THIRD, unrelated bug in the opposite direction (a false REJECT, not a false
// ACCEPT — a distinct root cause, so filed separately rather than folded in here) was
// also found during verification: a CORRECT cross-module override that faithfully
// repeats an imported base's `when true`/`when false` out-nullability postcondition
// (§22.4) is wrongly rejected, because `ModuleLoader::convertParams` never
// reconstructs `ParameterSymbol::outNullPostcond` for an imported parameter (it stays
// at the default `None`). Filed as #2392; not exercised by this file (unrelated
// signature dimension, no test here would pin a fix this file's diff doesn't make).

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

struct TempFile2386 {
    std::string path;
    explicit TempFile2386(std::string p) : path(std::move(p)) {}
    ~TempFile2386() { std::remove(path.c_str()); }
};

void writeSource2386(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj2386(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// Compile B importing A, capturing the driver's stderr (fd 2) into `err`. Mirrors
// xmod_abstract_impl_prop_2380_test.cpp's compileImportCaptureErr2380 (itself
// mirroring xmod_abstract_impl_subst_2373_test.cpp / xmod_generic_method_arg_
// marshalling_1943_test.cpp).
bool compileImportCaptureErr2386(const std::string& bSrc,
                                 const std::string& bExe,
                                 const std::vector<std::string>& aObjs,
                                 std::string& err) {
    namespace fs = std::filesystem;
    std::string errPath = (fs::temp_directory_path() /
                           ("bz2386_err_" + unique_stem("") +
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
int compileImportAndRun2386(const std::string& libSrc,
                            const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2386_");
    TempFile2386 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2386 aObj((dir / (stem + "_a.o")).string());
    writeSource2386(aSrc.path, libSrc);
    if (!compileToObj2386(aSrc.path, aObj.path)) return -2;
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile2386 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2386 bExe((dir / stem).string());
    writeSource2386(bSrc.path, appSrc);
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
// T1 — METHOD case, as reported in #2386: overriding an IMPORTED abstract
// method with a mismatched RETURN TYPE is REJECTED (§12.1). Pre-fix: `bDecl`
// (base's ast::MethodDecl*) is null for an imported base, so
// `if (dDecl && bDecl)` skipped the whole return-type comparison.
// ===========================================================================
BEZEL_TEST(xmod_override_signature_2386,
           method_return_type_mismatch_against_imported_base_rejected_12_1) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2386_t1_");
    TempFile2386 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2386 aObj((dir / (stem + "_a.o")).string());
    writeSource2386(aSrc.path,
                    "module ImportedAbstractLib2386T1; "
                    "public abstract class AbsBase2386T1 "
                    "  public abstract method Get(): int; "
                    "end AbsBase2386T1;");
    BEZEL_EXPECT_TRUE(compileToObj2386(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;  // toolchain unavailable

    std::string bSrc =
        "module AppOverride2386T1; import ImportedAbstractLib2386T1; "
        "public class BadOverride2386T1: AbsBase2386T1 "
        "  public override method Get(): string => \"garbage marker string\"; "
        "end BadOverride2386T1; "
        "public class C public static method Main(): int => 0; end C;";
    TempFile2386 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2386 bExe((dir / (stem + "_b")).string());
    writeSource2386(bSrcFile.path, bSrc);

    std::string err;
    bool success = compileImportCaptureErr2386(bSrcFile.path, bExe.path,
                                               {aObj.path}, err);
    BEZEL_EXPECT_TRUE(!success);
    BEZEL_EXPECT_TRUE(err.find("12.1") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("override of `Get`") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("return type") != std::string::npos);
}

// ===========================================================================
// T2 — PROPERTY case, as reported in #2386: overriding an IMPORTED abstract
// property with a mismatched PROPERTY TYPE is REJECTED (§12.1). Pre-fix:
// `bDecl` (base's ast::PropertyDecl*) is null for an imported base, so
// `if (dDecl && bDecl)` skipped the whole property-type comparison.
// ===========================================================================
BEZEL_TEST(xmod_override_signature_2386,
           property_type_mismatch_against_imported_base_rejected_12_1) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2386_t2_");
    TempFile2386 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2386 aObj((dir / (stem + "_a.o")).string());
    writeSource2386(aSrc.path,
                    "module ImportedAbstractPropLib2386T2; "
                    "public abstract class AbsPropBase2386T2 "
                    "  public abstract property Val: int; "
                    "end AbsPropBase2386T2;");
    BEZEL_EXPECT_TRUE(compileToObj2386(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    std::string bSrc =
        "module AppPropOverride2386T2; import ImportedAbstractPropLib2386T2; "
        "public class BadPropOverride2386T2: AbsPropBase2386T2 "
        "  private _s: string := \"x\"; "
        "  public override property Val: string => _s; "
        "end BadPropOverride2386T2; "
        "public class C public static method Main(): int => 0; end C;";
    TempFile2386 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2386 bExe((dir / (stem + "_b")).string());
    writeSource2386(bSrcFile.path, bSrc);

    std::string err;
    bool success = compileImportCaptureErr2386(bSrcFile.path, bExe.path,
                                               {aObj.path}, err);
    BEZEL_EXPECT_TRUE(!success);
    BEZEL_EXPECT_TRUE(err.find("12.1") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("override of `Val`") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("property type") != std::string::npos);
}

// ===========================================================================
// T3 — METHOD PARAMETER-TYPE case: a SECOND, independent gap in the same
// function found during verification — the parameter-type loop reads
// `bp->declType` directly (always null for an imported parameter; the
// resolved type rides `resolvedDeclType` instead, ModuleLoader::
// convertParams), silently skipping the comparison via
// `if (dt && btSubst && ...)`. Not gated on `dDecl`/`bDecl` at all, so
// distinct from T1's gate but the same #2374 defect class.
// ===========================================================================
BEZEL_TEST(xmod_override_signature_2386,
           method_parameter_type_mismatch_against_imported_base_rejected_12_1) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2386_t3_");
    TempFile2386 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2386 aObj((dir / (stem + "_a.o")).string());
    writeSource2386(aSrc.path,
                    "module ImportedAbstractParamLib2386T3; "
                    "public abstract class AbsParamBase2386T3 "
                    "  public abstract method Set(x: int); "
                    "end AbsParamBase2386T3;");
    BEZEL_EXPECT_TRUE(compileToObj2386(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    std::string bSrc =
        "module AppParamOverride2386T3; import ImportedAbstractParamLib2386T3; "
        "public class BadParamOverride2386T3: AbsParamBase2386T3 "
        "  public override method Set(x: string) end Set; "
        "end BadParamOverride2386T3; "
        "public class C public static method Main(): int => 0; end C;";
    TempFile2386 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2386 bExe((dir / (stem + "_b")).string());
    writeSource2386(bSrcFile.path, bSrc);

    std::string err;
    bool success = compileImportCaptureErr2386(bSrcFile.path, bExe.path,
                                               {aObj.path}, err);
    BEZEL_EXPECT_TRUE(!success);
    BEZEL_EXPECT_TRUE(err.find("12.1") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("override of `Set`") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("parameter types") != std::string::npos);
}

// ===========================================================================
// T4 — LOCAL-BASE CONTROL (method): the identical mismatch against a
// SAME-MODULE base class was, and remains, correctly rejected. Guards that
// the fix (which only touches the imported-base fallback reads) leaves the
// already-working local path untouched.
// ===========================================================================
BEZEL_TEST(xmod_override_signature_2386,
           method_return_type_mismatch_against_local_base_still_rejected_12_1) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2386_t4_");
    std::string bSrc =
        "module LocalMethod2386T4; "
        "public abstract class AbsBaseLocal2386T4 "
        "  public abstract method Get(): int; "
        "end AbsBaseLocal2386T4; "
        "public class BadOverrideLocal2386T4: AbsBaseLocal2386T4 "
        "  public override method Get(): string => \"x\"; "
        "end BadOverrideLocal2386T4; "
        "public class C public static method Main(): int => 0; end C;";
    TempFile2386 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2386 bExe((dir / (stem + "_b")).string());
    writeSource2386(bSrcFile.path, bSrc);

    std::string err;
    bool success =
        compileImportCaptureErr2386(bSrcFile.path, bExe.path, {}, err);
    BEZEL_EXPECT_TRUE(!success);
    BEZEL_EXPECT_TRUE(err.find("12.1") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("return type") != std::string::npos);
}

// ===========================================================================
// T5 — LOCAL-BASE CONTROL (property): same as T4, for the property case.
// ===========================================================================
BEZEL_TEST(xmod_override_signature_2386,
           property_type_mismatch_against_local_base_still_rejected_12_1) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2386_t5_");
    std::string bSrc =
        "module LocalProp2386T5; "
        "public abstract class AbsPropBaseLocal2386T5 "
        "  public abstract property Val: int; "
        "end AbsPropBaseLocal2386T5; "
        "public class BadPropOverrideLocal2386T5: AbsPropBaseLocal2386T5 "
        "  private _s: string := \"x\"; "
        "  public override property Val: string => _s; "
        "end BadPropOverrideLocal2386T5; "
        "public class C public static method Main(): int => 0; end C;";
    TempFile2386 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2386 bExe((dir / (stem + "_b")).string());
    writeSource2386(bSrcFile.path, bSrc);

    std::string err;
    bool success =
        compileImportCaptureErr2386(bSrcFile.path, bExe.path, {}, err);
    BEZEL_EXPECT_TRUE(!success);
    BEZEL_EXPECT_TRUE(err.find("12.1") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("property type") != std::string::npos);
}

// ===========================================================================
// T6 — ACCEPT + RUN + VALUE (method): a CORRECT override of an imported
// abstract method still compiles and dispatches to the right value across
// the module boundary. Guards against the fix over-widening the return-type/
// parameter-type fallback into a false REJECT of a correct override.
// ===========================================================================
BEZEL_TEST(xmod_override_signature_2386,
           method_correct_override_of_imported_base_runs_and_dispatches) {
    int r = compileImportAndRun2386(
        "module ImportedAbstractLib2386T6; "
        "public abstract class AbsBase2386T6 "
        "  public abstract method Get(): int; "
        "end AbsBase2386T6;",
        "module AppOverride2386T6; import ImportedAbstractLib2386T6; "
        "public class GoodOverride2386T6: AbsBase2386T6 "
        "  public override method Get(): int => 99; "
        "end GoodOverride2386T6; "
        "public class C "
        "  public static method Main(): int "
        "    using var b: AbsBase2386T6 := new GoodOverride2386T6(); "
        "    return b.Get() - 99; "
        "  end Main; "
        "end C;");
    if (r == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 0);
}

// ===========================================================================
// T7 — ACCEPT + RUN + VALUE (property): a CORRECT override of an imported
// abstract property still compiles and dispatches to the right value across
// the module boundary.
// ===========================================================================
BEZEL_TEST(xmod_override_signature_2386,
           property_correct_override_of_imported_base_runs_and_dispatches) {
    int r = compileImportAndRun2386(
        "module ImportedAbstractPropLib2386T7; "
        "public abstract class AbsPropBase2386T7 "
        "  public abstract property Val: int; "
        "end AbsPropBase2386T7;",
        "module AppPropOverride2386T7; import ImportedAbstractPropLib2386T7; "
        "public class GoodPropOverride2386T7: AbsPropBase2386T7 "
        "  private _n: int := 55; "
        "  public override property Val: int => _n; "
        "end GoodPropOverride2386T7; "
        "public class C "
        "  public static method Main(): int "
        "    using var b: AbsPropBase2386T7 := new GoodPropOverride2386T7(); "
        "    return b.Val - 55; "
        "  end Main; "
        "end C;");
    if (r == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 0);
}

// ===========================================================================
// T8 — TWO-LEVEL IMPORTED ANCESTRY: the abstract method lives on an imported
// GRANDPARENT; the local class derives from an imported (non-abstract-
// overriding) MIDDLE class that itself derives from the grandparent. `base`
// is still resolved to the grandparent's imported MethodSymbol by the
// override-handling BFS — confirms the fix holds when the imported ancestor
// is not the immediate base type.
// ===========================================================================
BEZEL_TEST(xmod_override_signature_2386,
           method_mismatch_against_two_level_imported_ancestry_rejected_12_1) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz2386_t8_");
    TempFile2386 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2386 aObj((dir / (stem + "_a.o")).string());
    writeSource2386(aSrc.path,
                    "module TwoLevelLib2386T8; "
                    "public abstract class GrandBase2386T8 "
                    "  public abstract method Get(): int; "
                    "end GrandBase2386T8; "
                    "public abstract class MidBase2386T8: GrandBase2386T8 "
                    "end MidBase2386T8;");
    BEZEL_EXPECT_TRUE(compileToObj2386(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    std::string bSrc =
        "module TwoLevelApp2386T8; import TwoLevelLib2386T8; "
        "public class BadTwoLevel2386T8: MidBase2386T8 "
        "  public override method Get(): string => \"x\"; "
        "end BadTwoLevel2386T8; "
        "public class C public static method Main(): int => 0; end C;";
    TempFile2386 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2386 bExe((dir / (stem + "_b")).string());
    writeSource2386(bSrcFile.path, bSrc);

    std::string err;
    bool success = compileImportCaptureErr2386(bSrcFile.path, bExe.path,
                                               {aObj.path}, err);
    BEZEL_EXPECT_TRUE(!success);
    BEZEL_EXPECT_TRUE(err.find("12.1") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("override of `Get`") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("return type") != std::string::npos);
}

}  // namespace bezel::module
