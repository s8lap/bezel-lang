// fix #2392 (§22.4 / §12.1 / §2.8.2 / §2.8.3, v0.235): a correct cross-module
// override that faithfully repeats an IMPORTED base method's `when true` /
// `when false` out-nullability postcondition (§22.4, v0.183, #1585) was
// wrongly REJECTED. `Resolver::checkOverrideSignatureMatch`
// (`src/sema/TypeResolution_Checks.cpp`) compares
// `dp->outNullPostcond != bp->outNullPostcond` unconditionally (not gated on
// `dDecl`/`bDecl` — unlike #2386's two gaps in the same functions). `bp` (the
// base parameter) is a `sema::ParameterSymbol`; for an IMPORTED base method,
// `ModuleLoader::convertParams` (`src/module/ModuleLoader.cpp`) built each
// parameter's `ParameterSymbol` setting only `name`/`modifier`/`scope`/
// `resolvedDeclType` — `outNullPostcond` was never set, so it stayed at its
// default `ast::OutNullWhen::None` regardless of what the exporting module's
// method actually declared. Every override of an imported base method with a
// `when true`/`when false` `out` parameter therefore failed with:
//
//   override of `TryGet` does not match the base member's `when`
//   nullability postcondition (§12.1)
//
// even when the override repeated the clause exactly as §12.1 requires.
//
// FOUND during #2386's verification (a false ACCEPT in the same override-
// matching code path); this is the OPPOSITE direction — a false REJECT — and
// a distinct root cause (no `dDecl`/`bDecl` gate involved at all), so filed
// and fixed separately. Also the OTHER-DIRECTION instance of #2374's audit
// pattern: an imported symbol carries a side table (`resolvedDeclType` via
// `ModuleLoader`) but not the AST/field the local path actually reads
// (`ParameterSymbol::outNullPostcond`) — most #2374 instances produce a false
// ACCEPT or a wrong value; this one produces a false REJECT of a CORRECT
// program, from the same missing-reconstruction root cause.
//
// FIX: `MetaParam` (`src/module/ModuleMetadata.hpp`) gains a new
// `outNullWhen` byte (0/1/2 mirroring `ast::OutNullWhen`), exported by
// `ModuleMetadataBuilder.cpp`'s `convertParam` (mirrors how `modifier` is
// exported) and consumed by `ModuleLoader::convertParams` to set the
// reconstructed `ParameterSymbol::outNullPostcond` directly (mirrors how
// `resolvedDeclType` is already reconstructed there). Purely additive to the
// wire format (a 2nd trailing scalar in `PARAM_BLOCK`'s `REC_SCALARS`
// record) — no `kSchemaVersion` bump; see that constant's comment in
// `ModuleMetadata.hpp` for the full compatibility argument. A stale pre-#2392
// dependency artifact keeps producing the false reject until ITS module is
// recompiled — accepted per §2.8.5's explicit non-goal of ABI stability
// across recompilation (this project's model is recompile-the-world).
//
// CONFIRMED by direct `bezelc -c`/`--import` probe before writing this test
// (see the commit message for the exact pre-fix diagnostic and the
// mechanical before/after transcript: scratch-copy the fix to a patch,
// `git checkout` the source back to pre-fix, rebuild, confirm T1/T5 FAIL,
// reapply, rebuild, confirm they PASS — never `git stash`).
//
// BONUS EFFECT (not separately filed — same fixed data, no separate
// mechanism): `ExprTC::narrowFromOutPostcond`
// (`src/sema/ExprTypecheck.cpp`) also reads `ParameterSymbol::outNullPostcond`
// off the call's RESOLVED `MethodSymbol` to drive caller-side `out`-recipient
// narrowing (§22.4). A call statically resolved against an imported method
// declaration (e.g. through a base-typed reference) was, before this fix,
// never narrowed either — the same missing reconstruction. T5 below exercises
// this: the caller reads the narrowed `out` recipient WITHOUT `!` on a call
// whose statically-resolved target is the IMPORTED abstract declaration.

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

struct TempFile2392 {
    std::string path;
    explicit TempFile2392(std::string p) : path(std::move(p)) {}
    ~TempFile2392() { std::remove(path.c_str()); }
};

void writeSource2392(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj2392(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// Compile B importing A, capturing the driver's stderr (fd 2) into `err`.
// Mirrors xmod_override_signature_2386_test.cpp's
// compileImportCaptureErr2386.
bool compileImportCaptureErr2392(const std::string& bSrc,
                                 const std::string& bExe,
                                 const std::vector<std::string>& aObjs,
                                 std::string& err) {
    namespace fs = std::filesystem;
    std::string errPath = (fs::temp_directory_path() /
                           ("bz2392_err_" + std::to_string(std::rand()) +
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
int compileImportAndRun2392(const std::string& libSrc,
                            const std::string& appSrc) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2392_" + std::to_string(std::rand());
    TempFile2392 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2392 aObj((dir / (stem + "_a.o")).string());
    writeSource2392(aSrc.path, libSrc);
    if (!compileToObj2392(aSrc.path, aObj.path)) return -2;
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile2392 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2392 bExe((dir / stem).string());
    writeSource2392(bSrc.path, appSrc);
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
// T1 — as reported in #2392: a CORRECT override of an IMPORTED abstract
// method's `out ... when true` parameter compiles clean. Pre-fix: `bp->
// outNullPostcond` (the imported base parameter) always read `None`, so
// `dp->outNullPostcond != bp->outNullPostcond` (`WhenTrue != None`) false-
// rejected this exact program with "does not match the base member's `when`
// nullability postcondition (§12.1)".
// ===========================================================================
BEZEL_TEST(xmod_override_when_postcond_2392,
           method_matching_when_true_override_of_imported_base_accepted_12_1) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2392_t1_" + std::to_string(std::rand());
    TempFile2392 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2392 aObj((dir / (stem + "_a.o")).string());
    writeSource2392(aSrc.path,
                    "module ImportedOutNullLib2392T1; "
                    "public abstract class AbsOutNullBase2392T1 "
                    "  public abstract method TryGet(out conn: string? when "
                    "true): bool; "
                    "end AbsOutNullBase2392T1;");
    BEZEL_EXPECT_TRUE(compileToObj2392(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;  // toolchain unavailable

    std::string bSrc =
        "module AppOutNullGood2392T1; import ImportedOutNullLib2392T1; "
        "public class GoodOutNull2392T1: AbsOutNullBase2392T1 "
        "  public override method TryGet(out conn: string? when true): bool "
        "    conn := \"x\"; "
        "    return true; "
        "  end TryGet; "
        "end GoodOutNull2392T1; "
        "public class C public static method Main(): int => 0; end C;";
    TempFile2392 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2392 bExe((dir / (stem + "_b")).string());
    writeSource2392(bSrcFile.path, bSrc);

    std::string err;
    bool success = compileImportCaptureErr2392(bSrcFile.path, bExe.path,
                                               {aObj.path}, err);
    BEZEL_EXPECT_TRUE(success);
    BEZEL_EXPECT_TRUE(err.find("nullability postcondition") ==
                      std::string::npos);
}

// ===========================================================================
// T2 — REJECT-CONTROL: a genuinely MISMATCHED postcondition (override omits
// `when true` entirely) against the SAME imported base is still correctly
// rejected, asserting the diagnostic text. Guards that the fix does not
// remove or weaken the check — only fixes the imported-base VALUE it reads.
// ===========================================================================
BEZEL_TEST(xmod_override_when_postcond_2392,
           method_missing_when_true_override_of_imported_base_rejected_12_1) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2392_t2_" + std::to_string(std::rand());
    TempFile2392 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2392 aObj((dir / (stem + "_a.o")).string());
    writeSource2392(aSrc.path,
                    "module ImportedOutNullLib2392T2; "
                    "public abstract class AbsOutNullBase2392T2 "
                    "  public abstract method TryGet(out conn: string? when "
                    "true): bool; "
                    "end AbsOutNullBase2392T2;");
    BEZEL_EXPECT_TRUE(compileToObj2392(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;

    std::string bSrc =
        "module AppOutNullBad2392T2; import ImportedOutNullLib2392T2; "
        "public class BadOutNull2392T2: AbsOutNullBase2392T2 "
        "  public override method TryGet(out conn: string?): bool "
        "    conn := \"x\"; "
        "    return true; "
        "  end TryGet; "
        "end BadOutNull2392T2; "
        "public class C public static method Main(): int => 0; end C;";
    TempFile2392 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2392 bExe((dir / (stem + "_b")).string());
    writeSource2392(bSrcFile.path, bSrc);

    std::string err;
    bool success = compileImportCaptureErr2392(bSrcFile.path, bExe.path,
                                               {aObj.path}, err);
    BEZEL_EXPECT_TRUE(!success);
    BEZEL_EXPECT_TRUE(err.find("12.1") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("override of `TryGet`") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("nullability postcondition") !=
                      std::string::npos);
}

// ===========================================================================
// T3 — LOCAL-BASE CONTROL (accept): the identical matching-postcondition
// override against a SAME-MODULE base class was, and remains, accepted.
// Guards that the fix (an import-side reconstruction change) leaves the
// already-working local path untouched.
// ===========================================================================
BEZEL_TEST(xmod_override_when_postcond_2392,
           method_matching_when_true_override_of_local_base_accepted_12_1) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2392_t3_" + std::to_string(std::rand());
    std::string bSrc =
        "module LocalOutNull2392T3; "
        "public abstract class AbsOutNullBaseLocal2392T3 "
        "  public abstract method TryGet(out conn: string? when true): bool; "
        "end AbsOutNullBaseLocal2392T3; "
        "public class GoodOutNullLocal2392T3: AbsOutNullBaseLocal2392T3 "
        "  public override method TryGet(out conn: string? when true): bool "
        "    conn := \"x\"; "
        "    return true; "
        "  end TryGet; "
        "end GoodOutNullLocal2392T3; "
        "public class C public static method Main(): int => 0; end C;";
    TempFile2392 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2392 bExe((dir / (stem + "_b")).string());
    writeSource2392(bSrcFile.path, bSrc);

    std::string err;
    bool success =
        compileImportCaptureErr2392(bSrcFile.path, bExe.path, {}, err);
    BEZEL_EXPECT_TRUE(success);
    BEZEL_EXPECT_TRUE(err.find("nullability postcondition") ==
                      std::string::npos);
}

// ===========================================================================
// T4 — LOCAL-BASE CONTROL (reject): a genuinely mismatched postcondition
// against a SAME-MODULE base class was, and remains, correctly rejected.
// ===========================================================================
BEZEL_TEST(xmod_override_when_postcond_2392,
           method_missing_when_true_override_of_local_base_rejected_12_1) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = "bz2392_t4_" + std::to_string(std::rand());
    std::string bSrc =
        "module LocalOutNullBad2392T4; "
        "public abstract class AbsOutNullBaseLocalBad2392T4 "
        "  public abstract method TryGet(out conn: string? when true): bool; "
        "end AbsOutNullBaseLocalBad2392T4; "
        "public class BadOutNullLocal2392T4: AbsOutNullBaseLocalBad2392T4 "
        "  public override method TryGet(out conn: string?): bool "
        "    conn := \"x\"; "
        "    return true; "
        "  end TryGet; "
        "end BadOutNullLocal2392T4; "
        "public class C public static method Main(): int => 0; end C;";
    TempFile2392 bSrcFile((dir / (stem + "_b.bzl")).string());
    TempFile2392 bExe((dir / (stem + "_b")).string());
    writeSource2392(bSrcFile.path, bSrc);

    std::string err;
    bool success =
        compileImportCaptureErr2392(bSrcFile.path, bExe.path, {}, err);
    BEZEL_EXPECT_TRUE(!success);
    BEZEL_EXPECT_TRUE(err.find("12.1") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("nullability postcondition") !=
                      std::string::npos);
}

// ===========================================================================
// T5 — ACCEPT + RUN + VALUE, AND caller-side narrowing actually holds at
// runtime (the requested "postcondition actually holds at runtime" check).
// The call site invokes `TryGet` through a BASE-TYPED reference (static type
// = the IMPORTED abstract class), so the statically-resolved MethodSymbol IS
// the imported declaration whose `outNullPostcond` this fix reconstructs.
// `c.Length` inside the `then` branch reads the narrowed `out` recipient
// WITHOUT a `!` null-forgiving operator — if narrowing did not fire (or if
// the override had been false-rejected pre-fix), this source would not even
// compile (§7.4 member access on a nullable type requires `!`). Verifies the
// runtime value too: TryGet assigns "hello" (Length 5); Main returns
// `c.Length - 5`, expected 0.
// ===========================================================================
BEZEL_TEST(xmod_override_when_postcond_2392,
           method_correct_override_of_imported_base_runs_and_narrows_12_1) {
    int r = compileImportAndRun2392(
        "module ImportedOutNullLib2392T5; "
        "public abstract class AbsOutNullBase2392T5 "
        "  public abstract method TryGet(out conn: string? when true): "
        "bool; "
        "end AbsOutNullBase2392T5;",
        "module AppOutNullGood2392T5; import ImportedOutNullLib2392T5; "
        "public class GoodOutNull2392T5: AbsOutNullBase2392T5 "
        "  public override method TryGet(out conn: string? when true): "
        "bool "
        "    conn := \"hello\"; "
        "    return true; "
        "  end TryGet; "
        "end GoodOutNull2392T5; "
        "public class C "
        "  public static method Main(): int "
        "    using var b: AbsOutNullBase2392T5 := new GoodOutNull2392T5(); "
        "    if b.TryGet(out var c) then "
        "      return c.Length - 5; "
        "    end; "
        "    return -1; "
        "  end Main; "
        "end C;");
    if (r == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 0);
}

// ===========================================================================
// T6 — `when false` variant, ACCEPT + RUN: exercises the OTHER enum value
// (ast::OutNullWhen::WhenFalse == 2) through the same wire path, so the fix
// is proved for both non-None values, not just WhenTrue (== 1). The base
// declares `when false` on a `Contains`-style method (postcondition holds on
// the FALSE-returning path); the narrowed read happens in the `else` arm.
// ===========================================================================
BEZEL_TEST(xmod_override_when_postcond_2392,
           method_correct_override_of_imported_base_when_false_runs_12_1) {
    int r = compileImportAndRun2392(
        "module ImportedOutNullLib2392T6; "
        "public abstract class AbsOutNullBase2392T6 "
        "  public abstract method TryFail(out reason: string? when false): "
        "bool; "
        "end AbsOutNullBase2392T6;",
        "module AppOutNullGood2392T6; import ImportedOutNullLib2392T6; "
        "public class GoodOutNull2392T6: AbsOutNullBase2392T6 "
        "  public override method TryFail(out reason: string? when false): "
        "bool "
        "    reason := \"boom\"; "
        "    return false; "
        "  end TryFail; "
        "end GoodOutNull2392T6; "
        "public class C "
        "  public static method Main(): int "
        "    using var b: AbsOutNullBase2392T6 := new GoodOutNull2392T6(); "
        "    if b.TryFail(out var reason) then "
        "      return -1; "
        "    else "
        "      return reason.Length - 4; "
        "    end; "
        "  end Main; "
        "end C;");
    if (r == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 0);
}

}  // namespace bezel::module
