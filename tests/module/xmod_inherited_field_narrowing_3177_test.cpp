// Spec-derived regression test for issue #3177 — family M4 ("member TYPE
// resolution at a use site") of the imported-symbol classification seam
// (umbrella #3152). §7.4 (flow narrowing) / §2.5 + §2.8.3 rule 4 (imported
// artifacts), v0.254. Audit: docs/proposals/papers/member-symbol-audit-2026-08-13.md,
// Appendix A.3 rows `Calls.cpp:12429` / `:12435`, and the ⚠ coupling note in
// §8 step 4.
//
// ── MEASURED, on ec6a02f, with a real two-module build ─────────────────────
//
//   protected _f: string?  declared on a LOCAL base   -> §7.4 REJECT, exit 1
//   the same base IMPORTED, same derived class        -> COMPILES CLEAN,
//                                                        SIGSEGV (exit 139)
//
// §7.4 forbids narrowing a nullable FIELD at the use site (TOCTOU: another
// thread may reassign it between the test and the read) — "Mutable fields and
// properties are unchanged: copy to a local first". The gate that enforces it
// first asks whether the field's declared type is nullable at all, and it asked
// by casting `Symbol::decl` to `ast::FieldDecl` / `ast::PropertyDecl`.
// `ModuleLoader` assigns no `decl` for an imported member, so for a member
// INHERITED FROM AN IMPORTED BASE the answer was "no type", the gate did not
// fire, and the null dereference reached the hardware. This is a false ACCEPT,
// not a missing warning.
//
// ── THE COUPLING THIS TEST EXISTS TO PIN (T3/T4) ───────────────────────────
//
// §7.4 (v0.214) carves out `readonly` fields: "A nullable readonly field —
// instance or static — participates in flow narrowing, because the blanket
// refusal's premise (unseen writers) does not exist for it". The carve-out is
// implemented as a narrowing KEY minted by `ExprTC::fieldTypeOf` and consulted
// by the gate above. `fieldTypeOf` read the AST the same way — so widening the
// GATE alone would have converted the measured false accept into a FALSE REJECT
// of the legal `if (_r <> null) … _r.Length …` over an imported `readonly`
// field. T3 is what makes that failure mode visible; both halves moved to
// `sema::storageComponentType` in one commit.

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

// Per-process unique working dir under the shared scratch base. The previous
// fs::temp_directory_path() (== /tmp tmpfs) was race-prone even with
// unique_stem: parallel shards racing on the SAME /tmp parent dir overwrote
// each other's _a.bzl / _a.o / _b.bzl mid-compileObject. The
// /var/tmp/bezel-scratch parent fixes that class of flake (#3002-class
// COVERAGE SHORT). popen() at L131/176 is kept — per-stem uniqueness already
// mitigates the file-overwrite race; full fork isolation is a separate concern.
const std::filesystem::path& wtDir3177n() {
    return wtScratchDir("wt-3177n");
}

struct TempFile3177N {
    std::string path;
    explicit TempFile3177N(std::string p) : path(std::move(p)) {}
    ~TempFile3177N() { std::remove(path.c_str()); }
};

void writeSource3177N(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj3177N(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

struct ImportRun3177N {
    bool dependencyCompiled = false;   // ANTI-VACUITY guard
    bool appCompiled = false;
    std::string error;                 // the importer's stderr
    int exitCode = -1;                 // only meaningful when appCompiled
    std::string out;
};

// Compile lib -> obj, then the importer with stderr captured; if it compiled,
// run it and capture stdout + exit status. One helper covers both the REJECT
// tests (which read `error`) and the ACCEPT tests (which read `out`), so an
// accept test can never silently pass on a program that failed to build.
ImportRun3177N importCompileAndRun3177N(const std::string& libSrc,
                                        const std::string& appSrc) {
    auto dir = wtDir3177n();
    std::string stem = unique_stem("bz3177n_");
    TempFile3177N aSrc((dir / (stem + "_a.bzl")).string());
    TempFile3177N aObj((dir / (stem + "_a.o")).string());
    writeSource3177N(aSrc.path, libSrc);
    ImportRun3177N r;
    r.dependencyCompiled = compileToObj3177N(aSrc.path, aObj.path) &&
                           std::ifstream(aObj.path).good();
    if (!r.dependencyCompiled) return r;

    TempFile3177N bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3177N bExe((dir / stem).string());
    writeSource3177N(bSrc.path, appSrc);
    std::string errPath =
        (dir / ("bz3177n_err_" + unique_stem("") + ".txt")).string();
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
    else if (WIFSIGNALED(status)) r.exitCode = -3;  // the pre-fix SIGSEGV
    else if (!WIFEXITED(status))  r.exitCode = -1;
    else                          r.exitCode = WEXITSTATUS(status);
    return r;
}

// The single-module twin: compile with stderr captured, run if it compiled.
ImportRun3177N localCompileAndRun3177N(const std::string& src) {
    auto dir = wtDir3177n();
    std::string stem = unique_stem("bz3177n_loc_");
    TempFile3177N sFile((dir / (stem + ".bzl")).string());
    TempFile3177N eFile((dir / stem).string());
    writeSource3177N(sFile.path, src);
    ImportRun3177N r;
    r.dependencyCompiled = true;  // no dependency in the local twin
    std::string errPath =
        (dir / ("bz3177n_lerr_" + unique_stem("") + ".txt")).string();
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
    if (!r.appCompiled) return r;
    std::string runCmd = eFile.path + " 2>/dev/null";
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

// The §7.4 field-narrowing reject, keyed on wording specific enough that no
// other diagnostic can satisfy it.
bool says74FieldReject3177N(const std::string& err) {
    return err.find("cannot narrow `T?` accessed through a field") !=
               std::string::npos &&
           err.find("copy to a local first") != std::string::npos;
}

// The dependency for T1: a MUTABLE nullable field on the base.
const char* kLibMut3177N =
    "module DepNarrowMut3177; "
    "public class BaseNM3177 "
    "  protected _f: string?; "
    "  public constructor() _f := null; end constructor; "
    "end BaseNM3177;";

// The dependency for T3: a READONLY nullable field on the base, assigned once
// in the base's own constructor (§22.8's single-assignment window).
const char* kLibRo3177N =
    "module DepNarrowRo3177; "
    "public class BaseNR3177 "
    "  protected readonly _r: string?; "
    "  public constructor() _r := \"abcd\"; end constructor; "
    "end BaseNR3177;";

}  // namespace

// ===========================================================================
// ★ T1 — FAILS WITHOUT THE FIX. A bare-identifier read of a nullable field
// INHERITED FROM AN IMPORTED BASE must hit the §7.4 TOCTOU gate. Pre-fix this
// program compiled clean and died with SIGSEGV on the null dereference.
// // per §7.4 / §2.5 / §2.8.3 rule 4, v0.254
// ===========================================================================
BEZEL_TEST(xmod_inherited_field_narrowing_3177,
           imported_base_nullable_field_is_not_narrowable_7_4) {
    auto r = importCompileAndRun3177N(
        kLibMut3177N,
        "module AppNarrowMut3177; import DepNarrowMut3177; "
        "public class DerNM3177: BaseNM3177 "
        "  public constructor() end constructor; "
        "  public method Bad(): int "
        "    return _f.Length; "
        "  end Bad; "
        "end DerNM3177; "
        "public class CNM3177 "
        "  public static method Main(): int "
        "    using var d := new DerNM3177(); "
        "    return d.Bad(); "
        "  end Main; "
        "end CNM3177;");
    BEZEL_EXPECT_TRUE(r.dependencyCompiled);  // anti-vacuity
    if (!r.dependencyCompiled) return;
    BEZEL_EXPECT_FALSE(r.appCompiled);
    BEZEL_EXPECT_TRUE(says74FieldReject3177N(r.error));
}

// ===========================================================================
// ★ T2 — THE DIFFERENTIAL CONTROL. The identical program with the base
// declared in the SAME module. It rejected before the fix and must still
// reject: it is what pins that the module boundary was the entire difference.
// // per §7.4
// ===========================================================================
BEZEL_TEST(xmod_inherited_field_narrowing_3177,
           local_base_nullable_field_still_rejected_control_7_4) {
    auto r = localCompileAndRun3177N(
        "module LocalNarrowMut3177; "
        "public class BaseNM3177L "
        "  protected _f: string?; "
        "  public constructor() _f := null; end constructor; "
        "end BaseNM3177L; "
        "public class DerNM3177L: BaseNM3177L "
        "  public constructor() end constructor; "
        "  public method Bad(): int "
        "    return _f.Length; "
        "  end Bad; "
        "end DerNM3177L; "
        "public class CNM3177L "
        "  public static method Main(): int "
        "    using var d := new DerNM3177L(); "
        "    return d.Bad(); "
        "  end Main; "
        "end CNM3177L;");
    BEZEL_EXPECT_FALSE(r.appCompiled);
    BEZEL_EXPECT_TRUE(says74FieldReject3177N(r.error));
}

// ===========================================================================
// ★ T3 — THE COUPLING GUARD. §7.4's `readonly` carve-out must still ACCEPT
// this program, and produce the right value. If the gate above were widened
// without also widening the narrowing-key minting in `ExprTC::fieldTypeOf`,
// this legal program would become a FALSE REJECT — the exact failure mode the
// audit's §8 step 4 warns about. `_r` is "abcd", so the guarded read is 4.
// // per §7.4 v0.214 (`readonly` fields narrow) / §22.8 / §2.5
// ===========================================================================
BEZEL_TEST(xmod_inherited_field_narrowing_3177,
           imported_base_readonly_nullable_field_still_narrows_7_4) {
    auto r = importCompileAndRun3177N(
        kLibRo3177N,
        "module AppNarrowRo3177; import DepNarrowRo3177; "
        "public class DerNR3177: BaseNR3177 "
        "  public constructor() end constructor; "
        "  public method Len(): int "
        "    if _r <> null then "
        "      return _r.Length; "
        "    end if; "
        "    return -1; "
        "  end Len; "
        "end DerNR3177; "
        "public class CNR3177 "
        "  public static method Main(): int "
        "    using var d := new DerNR3177(); "
        "    Console.WriteLine(\"len=\" + d.Len().ToString()); "
        "    return 0; "
        "  end Main; "
        "end CNR3177;");
    BEZEL_EXPECT_TRUE(r.dependencyCompiled);  // anti-vacuity
    if (!r.dependencyCompiled) return;
    BEZEL_EXPECT_TRUE(r.appCompiled);         // NOT a false reject
    if (!r.appCompiled) return;
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.out, std::string("len=4\n"));
}

// ===========================================================================
// ★ T4 — the single-module twin of T3. §2.8.3 rule 4's consistency anchor:
// the two spellings must agree. // per §7.4 v0.214
// ===========================================================================
BEZEL_TEST(xmod_inherited_field_narrowing_3177,
           local_base_readonly_nullable_field_narrows_control_7_4) {
    auto r = localCompileAndRun3177N(
        "module LocalNarrowRo3177; "
        "public class BaseNR3177L "
        "  protected readonly _r: string?; "
        "  public constructor() _r := \"abcd\"; end constructor; "
        "end BaseNR3177L; "
        "public class DerNR3177L: BaseNR3177L "
        "  public constructor() end constructor; "
        "  public method Len(): int "
        "    if _r <> null then "
        "      return _r.Length; "
        "    end if; "
        "    return -1; "
        "  end Len; "
        "end DerNR3177L; "
        "public class CNR3177L "
        "  public static method Main(): int "
        "    using var d := new DerNR3177L(); "
        "    Console.WriteLine(\"len=\" + d.Len().ToString()); "
        "    return 0; "
        "  end Main; "
        "end CNR3177L;");
    BEZEL_EXPECT_TRUE(r.appCompiled);
    if (!r.appCompiled) return;
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.out, std::string("len=4\n"));
}

}  // namespace bezel::module
