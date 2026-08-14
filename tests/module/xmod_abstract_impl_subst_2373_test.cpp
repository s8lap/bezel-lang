// fix #2373 (§12.5 / §11.2 / §2.8.2, v0.234): `Resolver::checkAbstractImplementations`
// — the §12.5 INSTANCE-member twin of #2359's `checkStaticInterfaceConformance::
// subst()` fix — silently skipped its whole substituted-signature comparison for
// any IMPORTED interface/abstract member, admitting an implementation whose
// parameter or return type does not match at all.
//
// #2373 (filed as a follow-up to #2359, "found, deliberately not fixed") named
// ONE gate: `substTypeRef`'s bare-type-parameter check —
//   `!d->declaration && d->astNode && d->astNode->name && ... size() == 1`
// — a silent no-op for an imported TypeRef, which carries no `astNode` at all
// (only `importedName`, ModuleLoader::convert, §2.8.2/§2.8.3). VERIFYING that
// gate in isolation surfaced a SECOND, more severe bug in the same function that
// #2373 had not identified: the per-parameter and return-type comparisons never
// even REACH `substTypeRef` for an imported member, because
//   * `TypeRef* it = resolvedOf(ip->declType);` reads an imported parameter's
//     type from its (always-null) syntactic `declType` instead of
//     `resolvedDeclType` (ModuleLoader::convertParams never attaches an
//     ast::Type — there is none to attach), so `it` is null and the whole
//     per-parameter check is skipped via `if (!it || !ct) continue;`; and
//   * `auto* imDecl = dynamic_cast<const ast::MethodDecl*>(im->decl);` is
//     unconditionally null for an imported member (ModuleLoader never attaches
//     an ast::MethodDecl to a body-less imported declaration), which skips
//     BOTH the `owned`-return check and the return-type-match block entirely.
// This is the SAME root defect class (#2374's audit: an `astNode`/`declType`-
// only gate silently mishandling an imported TypeRef/symbol) at three sites in
// one function, not one. #2294 already established and reviewed the fix shape
// for the exact STATIC-member twin (`checkStaticInterfaceConformance`): read
// `resolvedDeclType` when `declType` is null, and read
// `ctx_.importedReturnType`/`importedReturnOwned` when the member has no
// ast::MethodDecl. This fix ports that established shape to the INSTANCE-member
// checker, plus #2373's own `substTypeRef` `headName()` fallback (mirroring
// #2359 bug 2d) so a bare type parameter substitutes correctly once the reads
// above stop bailing out early.
//
// CONFIRMED SYMPTOM (verified by probe before writing this test, then
// mechanically by reverting the src/ fix and re-running this exact filter):
// pre-fix, a LOCAL class conforming to an IMPORTED interface with ANY
// parameter- or return-type mismatch — generic (`IBox<T>`'s `Get(): T` bound to
// `IBox<int>`, implemented by a `Get(): string`) or perfectly ORDINARY
// (`ISimpleRet`'s `Get(): int`, implemented by a `Get(): string`) — compiled
// clean and, when actually called through the interface-typed reference,
// dispatched to the wrong-typed body and read garbage off the stack/registers
// (observed `v=-943274556` for both the generic and non-generic reproductions,
// via `Console.WriteLine` after a direct probe compile+run — not part of this
// gtest, since the fix now REJECTS all three shapes at compile time and they
// never reach the codegen call site at all).
//
// A SEPARATE, PRE-EXISTING bug was found and is NOT fixed here (out of #2373's
// scope, filed as its own issue): a CORRECT implementation of a cross-module
// GENERIC interface member (`IBox<int>`'s `Get(): T` implemented correctly as
// `Get(): int => 42`) is accepted by sema (both before and after this fix) but
// dispatches to WRONG VALUES at runtime regardless of this fix — reproduced
// identically on both sides of the revert, and confirmed absent for the
// equivalent SAME-MODULE program (which returns the correct 42). This is a
// codegen defect in cross-module generic-interface method dispatch, adjacent
// to the §14.1/#783 "not yet monomorphized" guard (which already rejects the
// REFERENCE-type-argument case with a clean diagnostic, but silently mis-
// computes the VALUE-type-argument case instead of also guarding it). Because
// of this, the "correct generic implementation" ACCEPT control below asserts
// only that the program COMPILES (proving the fix does not introduce a false
// REJECT) — the value-observable ACCEPT+RUN control instead uses a NON-GENERIC
// imported interface member, which exercises the exact same `resolvedOf`/
// `im->decl` read-path fix (every imported member, generic or not, has a null
// `declType`/`decl`) without going through the separately-broken generic
// codegen path.

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
#include "support/driver_run_support.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

struct TempFile2373 {
    std::string path;
    explicit TempFile2373(std::string p) : path(std::move(p)) {}
    ~TempFile2373() { std::remove(path.c_str()); }
};

void writeSource2373(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj2373(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// Compile B importing A, capturing the driver's stderr (fd 2) into `err`. The
// fail-without/pass-with hinge for every REJECT case below: WITHOUT the fix
// the importer's compile SUCCEEDS (the check silently skips the mismatch);
// WITH the fix it fails and stderr carries the specific §11.2/§12.5 diagnostic
// text asserted by each test. Mirrors
// tests/module/xmod_generic_method_arg_marshalling_1943_test.cpp's
// compileImportCaptureErr.
bool compileImportCaptureErr2373(const std::string& bSrc,
                                 const std::string& bExe,
                                 const std::string& aObj, std::string& err) {
    namespace fs = std::filesystem;
    std::string errPath = (fs::temp_directory_path() /
                           ("bz2373_err_" + std::to_string(std::rand()) +
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
        bezel::Dependency d;
        d.metadataPath = aObj;
        d.objectPath = aObj;
        deps.push_back(d);
        success = bezel::test::compileFileForTest(driver, bSrc, bExe, deps).success;
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
int compileImportAndRun2373(const std::string& libSrc,
                            const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2373_" + std::to_string(std::rand());
    TempFile2373 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2373 aObj((dir / (stem + "_a.o")).string());
    writeSource2373(aSrc.path, libSrc);
    if (!compileToObj2373(aSrc.path, aObj.path)) return -2;
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile2373 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2373 bExe((dir / stem).string());
    writeSource2373(bSrc.path, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    if (!bezel::test::compileFileForTest(driver, bSrc.path, bExe.path, deps).success) return -1;
    int status = std::system(bExe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// True iff B (importing A) compiles clean.
bool compileImportSucceeds2373(const std::string& libSrc,
                               const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2373_ok_" + std::to_string(std::rand());
    TempFile2373 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2373 aObj((dir / (stem + "_a.o")).string());
    writeSource2373(aSrc.path, libSrc);
    if (!compileToObj2373(aSrc.path, aObj.path)) return false;
    if (!std::ifstream(aObj.path).good()) return false;

    TempFile2373 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2373 bExe((dir / stem).string());
    writeSource2373(bSrc.path, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    return bezel::test::compileFileForTest(driver, bSrc.path, bExe.path, deps).success;
}

}  // namespace

// ===========================================================================
// T1 — generic RETURN-type mismatch through an imported generic interface is
// REJECTED (§11.2 / §12.5). Pre-fix: `imDecl` is null for the imported `Get`
// (no ast::MethodDecl on an imported member), so the whole return-type check
// was skipped and `WrongBox` was silently accepted.
// ===========================================================================
BEZEL_TEST(xmod_abstract_impl_subst_2373,
           generic_return_type_mismatch_rejected_11_2) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2373_t1_" + std::to_string(std::rand());
    TempFile2373 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2373 aObj((dir / (stem + "_a.o")).string());
    writeSource2373(aSrc.path,
                    "module IBoxLib2373T1; "
                    "public interface IBox<T> method Get(): T; end IBox;");
    BEZEL_EXPECT_TRUE(compileToObj2373(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;  // toolchain unavailable

    std::string bSrc =
        "module App2373T1; import IBoxLib2373T1; "
        "public class WrongBox: IBox<int> "
        "  public method Get(): string => \"oops\"; "
        "end WrongBox; "
        "public class C public static method Main(): int => 0; end C;";
    TempFile2373 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2373 bExe((dir / (stem + "_b")).string());
    writeSource2373(bSrcFile.path, bSrc);

    std::string err;
    bool success =
        compileImportCaptureErr2373(bSrcFile.path, bExe.path, aObj.path, err);
    BEZEL_EXPECT_TRUE(!success);
    BEZEL_EXPECT_TRUE(err.find("11.2") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("return type") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("IBox") != std::string::npos);
}

// ===========================================================================
// T2 — generic PARAMETER-type mismatch through an imported generic interface
// is REJECTED (§11.2 / §12.5). Pre-fix: `ip->declType` is null for the
// imported `Accept`'s parameter (ModuleLoader::convertParams populates
// `resolvedDeclType` instead), so `it` was null and the whole per-parameter
// check was skipped via `if (!it || !ct) continue;`.
// ===========================================================================
BEZEL_TEST(xmod_abstract_impl_subst_2373,
           generic_param_type_mismatch_rejected_11_2) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2373_t2_" + std::to_string(std::rand());
    TempFile2373 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2373 aObj((dir / (stem + "_a.o")).string());
    writeSource2373(
        aSrc.path,
        "module IAcceptLib2373T2; "
        "public interface IAccept<T> method Accept(x: T): int; end IAccept;");
    BEZEL_EXPECT_TRUE(compileToObj2373(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    std::string bSrc =
        "module App2373T2; import IAcceptLib2373T2; "
        "public class BadAccepter: IAccept<int> "
        "  public method Accept(x: string): int => 1; "
        "end BadAccepter; "
        "public class C public static method Main(): int => 0; end C;";
    TempFile2373 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2373 bExe((dir / (stem + "_b")).string());
    writeSource2373(bSrcFile.path, bSrc);

    std::string err;
    bool success =
        compileImportCaptureErr2373(bSrcFile.path, bExe.path, aObj.path, err);
    BEZEL_EXPECT_TRUE(!success);
    BEZEL_EXPECT_TRUE(err.find("11.2") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("parameter") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("IAccept") != std::string::npos);
}

// ===========================================================================
// T3 — a perfectly ORDINARY (non-generic) return-type mismatch through an
// imported interface is REJECTED. Isolates the `im->decl`-is-always-null-for-
// an-imported-member bug from #2373's generic-substitution angle: this shape
// never touches `substTypeRef` at all (no type parameter is involved anywhere)
// and was STILL silently accepted pre-fix, because the return-type-match block
// bailed out on `imDecl == nullptr` before ever comparing types.
// ===========================================================================
BEZEL_TEST(xmod_abstract_impl_subst_2373,
           nongeneric_return_type_mismatch_rejected_11_2) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2373_t3_" + std::to_string(std::rand());
    TempFile2373 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2373 aObj((dir / (stem + "_a.o")).string());
    writeSource2373(aSrc.path,
                    "module ISimpleRetLib2373T3; "
                    "public interface ISimpleRet method Get(): int; "
                    "end ISimpleRet;");
    BEZEL_EXPECT_TRUE(compileToObj2373(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    std::string bSrc =
        "module App2373T3; import ISimpleRetLib2373T3; "
        "public class BadImpl: ISimpleRet "
        "  public method Get(): string => \"x\"; "
        "end BadImpl; "
        "public class C public static method Main(): int => 0; end C;";
    TempFile2373 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2373 bExe((dir / (stem + "_b")).string());
    writeSource2373(bSrcFile.path, bSrc);

    std::string err;
    bool success =
        compileImportCaptureErr2373(bSrcFile.path, bExe.path, aObj.path, err);
    BEZEL_EXPECT_TRUE(!success);
    BEZEL_EXPECT_TRUE(err.find("11.2") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("return type") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("ISimpleRet") != std::string::npos);
}

// ===========================================================================
// T4 — REJECT-CONTROL: a genuinely MISSING implementation of an imported
// interface member is still rejected with the pre-existing §12.5 diagnostic.
// Guards that the fix (which widens WHERE the interface signature is read
// from) does not also widen WHAT counts as satisfying the obligation in the
// first place.
// ===========================================================================
BEZEL_TEST(xmod_abstract_impl_subst_2373,
           generic_missing_implementation_still_rejected_12_5) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2373_t4_" + std::to_string(std::rand());
    TempFile2373 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2373 aObj((dir / (stem + "_a.o")).string());
    writeSource2373(aSrc.path,
                    "module IBoxLib2373T4; "
                    "public interface IBox<T> method Get(): T; end IBox;");
    BEZEL_EXPECT_TRUE(compileToObj2373(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    std::string bSrc =
        "module App2373T4; import IBoxLib2373T4; "
        "public class NoImpl: IBox<int> end NoImpl; "
        "public class C public static method Main(): int => 0; end C;";
    TempFile2373 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2373 bExe((dir / (stem + "_b")).string());
    writeSource2373(bSrcFile.path, bSrc);

    std::string err;
    bool success =
        compileImportCaptureErr2373(bSrcFile.path, bExe.path, aObj.path, err);
    BEZEL_EXPECT_TRUE(!success);
    BEZEL_EXPECT_TRUE(err.find("12.5") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("does not implement") != std::string::npos);
}

// ===========================================================================
// T5 — ACCEPT-CONTROL (compiles only — see the file header for why the value
// is not asserted): a CORRECT implementation of an imported GENERIC
// interface's member still compiles clean. Guards against the fix over-
// widening `substTypeRef`'s gate into a false REJECT of a correct witness
// (the #2359-bug-2d shape #2373 originally anticipated).
// ===========================================================================
BEZEL_TEST(xmod_abstract_impl_subst_2373,
           generic_correct_implementation_still_compiles_11_2) {
    bool ok = compileImportSucceeds2373(
        "module IBoxLib2373T5; "
        "public interface IBox<T> method Get(): T; end IBox;",
        "module App2373T5; import IBoxLib2373T5; "
        "public class RightBox: IBox<int> "
        "  public method Get(): int => 42; "
        "end RightBox; "
        "public class C public static method Main(): int "
        "  using var b: IBox<int> := new RightBox(); "
        "  return 0; "  // value not asserted; see file header (codegen #.. gap)
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(ok);
}

// ===========================================================================
// T6 — ACCEPT + RUN + VALUE: a correct implementation of an imported
// NON-generic interface member still compiles AND dispatches to the right
// body across the module boundary — 99. This is the value-observable control
// for the SAME read-path fix (every imported member, generic or not, has a
// null `declType`/`decl`) without the separately-broken generic dispatch path.
// ===========================================================================
BEZEL_TEST(xmod_abstract_impl_subst_2373,
           nongeneric_correct_implementation_runs_and_dispatches) {
    int r = compileImportAndRun2373(
        "module ISimpleLib2373T6; "
        "public interface ISimple method Get(): int; end ISimple;",
        "module App2373T6; import ISimpleLib2373T6; "
        "public class Impl: ISimple "
        "  public method Get(): int => 99; "
        "end Impl; "
        "public class C public static method Main(): int "
        "  using var b: ISimple := new Impl(); "
        "  return b.Get(); "
        "end Main; end C;");
    if (r == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 99);
}

}  // namespace bezel::module
