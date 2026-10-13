// Spec-derived regression test for issue #3177 — family M4 ("member TYPE
// resolution at a use site") of the imported-symbol classification seam
// (umbrella #3152). §9.5 (non-destructive update `with`) / §5.1 (assignability)
// / §9.1 (positional records) / §2.5 + §2.8.3 rule 4, v0.254.
// Audit: docs/proposals/papers/member-symbol-audit-2026-08-13.md, Appendix A.4 rows
// `Exprs.cpp:8573` (marked LATENT) and `:8594`.
//
// ── MEASURED, on ec6a02f, with a real two-module build ─────────────────────
//
//   `r with(A := "oops")` where the record is LOCAL     -> §5.1 REJECT, exit 1
//   the same record IMPORTED                            -> COMPILES CLEAN,
//                                                          exit 0, and prints
//                                                          A=4438432|B=0
//
// `4438432` is the string literal's ADDRESS read back as the `int` component;
// `B`, untouched and holding 2, reads back 0. So this is not a missing warning
// on a harmless program — §5.1 went unenforced and a pointer landed in an
// integer slot with no diagnostic at any severity.
//
// ── CAUSE, AND WHY IT WENT LIVE ────────────────────────────────────────────
//
// The `with` arm resolved its TARGET type by casting `Symbol::decl` to
// `ast::PropertyDecl` (auto-property arm) or by reading `posParam->type` then
// `ast::FieldDecl` (field arm). None of the three answers for an imported
// member — `decl` is null, and #2516's synthetic `posParam` carries a null
// `type` — so `fieldTy` stayed null and `checkAssignable` was SKIPPED ENTIRELY.
//
// The audit recorded the auto-property arm as ALREADY-CORRECT *(unreachable)*
// with a ⚠ LATENT note: the storage gate 30 lines above it
// (`isAutoPropertySymbol`) rejected every imported auto-property as "computed"
// before the type check could run, so the hole could not be reached. #3152 then
// fixed that gate (`isAutoPropertyStorageSymbol`), which is exactly the
// predicted LATENT-to-LIVE transition. It is live on this base; T3 measures it.
//
// Both arms now read `sema::storageComponentType`, the one both-sources
// accessor.

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

namespace fs = std::filesystem;

// Per-process unique working dir under the shared scratch base. An earlier
// partial pass added a LOCAL copy of unique_stem + g_temp_stem_counter (now
// superseded by the canonical tests/support/unique_stem.hpp header included
// above) and dropped the counter, but kept fs::temp_directory_path() — which
// resolves to /tmp tmpfs. Parallel shards racing on the same /tmp path still
// overwrote each other's _a.bzl / _a.o / _b.bzl mid-compileObject. This commit
// replaces the temp_directory_path() call sites with wtDir3177w() (per-process
// /var/tmp/bezel-scratch subdir) and removes the local helper in favour of the
// canonical header. popen() is kept — per-stem uniqueness already mitigates
// the file-overwrite race; full fork isolation is a separate concern.
const std::filesystem::path& wtDir3177w() {
    return wtScratchDir("wt-3177w");
}

struct TempFile3177W {
    std::string path;
    explicit TempFile3177W(std::string p) : path(std::move(p)) {}
    ~TempFile3177W() { std::remove(path.c_str()); }
};

void writeSource3177W(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj3177W(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

struct Run3177W {
    bool dependencyCompiled = false;   // ANTI-VACUITY guard
    bool appCompiled = false;
    std::string error;
    int exitCode = -1;
    std::string out;
};

Run3177W importCompileAndRun3177W(const std::string& libSrc,
                                  const std::string& appSrc) {
    auto dir = wtDir3177w();
    std::string stem = unique_stem("bz3177w");
    TempFile3177W aSrc((dir / (stem + "_a.bzl")).string());
    TempFile3177W aObj((dir / (stem + "_a.o")).string());
    writeSource3177W(aSrc.path, libSrc);
    Run3177W r;
    r.dependencyCompiled = compileToObj3177W(aSrc.path, aObj.path) &&
                           std::ifstream(aObj.path).good();
    if (!r.dependencyCompiled) return r;

    TempFile3177W bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3177W bExe((dir / stem).string());
    writeSource3177W(bSrc.path, appSrc);
    std::string errPath =
        (dir / (unique_stem("bz3177w_err") + ".txt")).string();
    std::fflush(stderr);
    std::cerr.flush();
    int saved = ::dup(STDERR_FILENO);
    int fd = ::open(errPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) ::dup2(fd, STDERR_FILENO);
    {
        bezel::Driver driver;
        std::vector<bezel::Dependency> deps;
        bezel::Dependency d;
        d.metadataPath = aObj.path;
        d.objectPath = aObj.path;
        deps.push_back(d);
        r.appCompiled =
            bezel::test::compileFileForTest(driver, bSrc.path, bExe.path, deps)
                .success;
    }
    std::fflush(stderr);
    std::cerr.flush();
    if (fd >= 0) { ::close(fd); ::dup2(saved, STDERR_FILENO); }
    ::close(saved);
    std::ifstream in(errPath);
    r.error.assign((std::istreambuf_iterator<char>(in)),
                   std::istreambuf_iterator<char>());
    std::remove(errPath.c_str());
    if (!r.appCompiled) return r;

    std::string runCmd = bExe.path + " 2>/dev/null";
    FILE* p = popen(runCmd.c_str(), "r");
    if (!p) { r.exitCode = -2; return r; }
    char buf[256];
    while (std::size_t n = std::fread(buf, 1, sizeof(buf), p))
        r.out.append(buf, n);
    int status = pclose(p);
    if (status == -1)             r.exitCode = -2;
    else if (WIFSIGNALED(status)) r.exitCode = -3;
    else if (!WIFEXITED(status))  r.exitCode = -1;
    else                          r.exitCode = WEXITSTATUS(status);
    return r;
}

Run3177W localCompile3177W(const std::string& src) {
    auto dir = wtDir3177w();
    std::string stem = unique_stem("bz3177w_loc");
    TempFile3177W sFile((dir / (stem + ".bzl")).string());
    TempFile3177W eFile((dir / stem).string());
    writeSource3177W(sFile.path, src);
    Run3177W r;
    r.dependencyCompiled = true;
    std::string errPath =
        (dir / (unique_stem("bz3177w_lerr") + ".txt")).string();
    std::fflush(stderr);
    std::cerr.flush();
    int saved = ::dup(STDERR_FILENO);
    int fd = ::open(errPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) ::dup2(fd, STDERR_FILENO);
    {
        bezel::Driver driver;
        r.appCompiled =
            bezel::test::compileFileForTest(driver, sFile.path, eFile.path, {})
                .success;
    }
    std::fflush(stderr);
    std::cerr.flush();
    if (fd >= 0) { ::close(fd); ::dup2(saved, STDERR_FILENO); }
    ::close(saved);
    std::ifstream in(errPath);
    r.error.assign((std::istreambuf_iterator<char>(in)),
                   std::istreambuf_iterator<char>());
    std::remove(errPath.c_str());
    return r;
}

// The §5.1 reject the `with` arms issue, keyed on the exact wording so that no
// other diagnostic (a missing member, a visibility reject) can satisfy it.
bool says51WithReject3177W(const std::string& err) {
    return err.find("the replacement value for `A` has a type that is not "
                    "implicitly convertible to the target type") !=
           std::string::npos;
}

// The dependency: a §9.1 positional record and a §9.2 body record.
const char* kLib3177W =
    "module DepWith3177; "
    "public record Rpos3177(A: int, B: int); "
    "public record Rbody3177 "
    "  public property A: int := 1; "
    "  public property B: int := 2; "
    "end Rbody3177;";

}  // namespace

// ===========================================================================
// ★ T1 — FAILS WITHOUT THE FIX. `with` on an IMPORTED §9.1 positional record,
// replacement value of the wrong type. Pre-fix: compiled clean, exit 0, and the
// string's address landed in the `int` component (`A=4438432|B=0`).
// // per §9.5 / §5.1 / §9.1 / §2.5 / §2.8.3 rule 4, v0.254
// ===========================================================================
BEZEL_TEST(xmod_with_replacement_type_3177,
           imported_positional_record_with_target_is_type_checked_9_5) {
    auto r = importCompileAndRun3177W(
        kLib3177W,
        "module AppWithPos3177; import DepWith3177; "
        "public class CW3177 "
        "  public static method Main(): int "
        "    var r := new Rpos3177(1, 2); "
        "    var r2 := r with(A := \"oops\"); "
        "    return r2.B; "
        "  end Main; "
        "end CW3177;");
    BEZEL_EXPECT_TRUE(r.dependencyCompiled);  // anti-vacuity
    if (!r.dependencyCompiled) return;
    BEZEL_EXPECT_FALSE(r.appCompiled);
    BEZEL_EXPECT_TRUE(says51WithReject3177W(r.error));
}

// ===========================================================================
// ★ T2 — THE DIFFERENTIAL CONTROL for T1: the identical program with the
// record declared in the SAME module. It rejected before the fix and must
// still reject. // per §9.5 / §5.1
// ===========================================================================
BEZEL_TEST(xmod_with_replacement_type_3177,
           local_positional_record_with_target_still_rejected_control_9_5) {
    auto r = localCompile3177W(
        "module LocalWithPos3177; "
        "public record Rpos3177L(A: int, B: int); "
        "public class CW3177L "
        "  public static method Main(): int "
        "    var r := new Rpos3177L(1, 2); "
        "    var r2 := r with(A := \"oops\"); "
        "    return r2.B; "
        "  end Main; "
        "end CW3177L;");
    BEZEL_EXPECT_FALSE(r.appCompiled);
    BEZEL_EXPECT_TRUE(says51WithReject3177W(r.error));
}

// ===========================================================================
// ★ T3 — FAILS WITHOUT THE FIX. The §9.2 AUTO-PROPERTY arm, which the audit
// recorded as LATENT (unreachable behind the AST-only storage gate) and which
// #3152's storage-gate fix made live. // per §9.5 / §5.1 / §9.2 / §2.5
// ===========================================================================
BEZEL_TEST(xmod_with_replacement_type_3177,
           imported_body_record_auto_property_with_target_is_type_checked_9_5) {
    auto r = importCompileAndRun3177W(
        kLib3177W,
        "module AppWithBody3177; import DepWith3177; "
        "public class CWB3177 "
        "  public static method Main(): int "
        "    var r := new Rbody3177(); "
        "    var r2 := r with(A := \"oops\"); "
        "    return r2.B; "
        "  end Main; "
        "end CWB3177;");
    BEZEL_EXPECT_TRUE(r.dependencyCompiled);  // anti-vacuity
    if (!r.dependencyCompiled) return;
    BEZEL_EXPECT_FALSE(r.appCompiled);
    BEZEL_EXPECT_TRUE(says51WithReject3177W(r.error));
}

// ===========================================================================
// ★ T4 — THE OVER-REJECTION GUARD. A WELL-TYPED `with` over the same imported
// record must still compile and produce the §9.5 copy semantics: the named
// component takes the replacement, every other keeps the source's value. A
// widened predicate that rejected here would be the mirror-image defect.
// // per §9.5 ("All other fields keep their values from the source record")
// ===========================================================================
BEZEL_TEST(xmod_with_replacement_type_3177,
           imported_record_well_typed_with_still_accepted_9_5) {
    auto r = importCompileAndRun3177W(
        kLib3177W,
        "module AppWithOk3177; import DepWith3177; "
        "public class CWO3177 "
        "  public static method Main(): int "
        "    var r := new Rpos3177(1, 2); "
        "    var r2 := r with(A := 40); "
        "    var b := new Rbody3177(); "
        "    var b2 := b with(A := 7); "
        "    Console.WriteLine(\"posA=\" + r2.A.ToString() + "
        "                      \"|posB=\" + r2.B.ToString() + "
        "                      \"|bodyA=\" + b2.A.ToString() + "
        "                      \"|bodyB=\" + b2.B.ToString()); "
        "    return 0; "
        "  end Main; "
        "end CWO3177;");
    BEZEL_EXPECT_TRUE(r.dependencyCompiled);  // anti-vacuity
    if (!r.dependencyCompiled) return;
    BEZEL_EXPECT_TRUE(r.appCompiled);         // NOT over-rejected
    if (!r.appCompiled) return;
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.out, std::string("posA=40|posB=2|bodyA=7|bodyB=2\n"));
}

}  // namespace bezel::module
