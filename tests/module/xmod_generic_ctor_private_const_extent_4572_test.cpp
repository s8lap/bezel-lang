// fix #4572 (§15.1 "Inline arrays" / §22.12 "Static members" / §14.1 "Generic
// declarations" / §2.8.3 rule 3 "Transitive closure"; spec v0.306) — a
// PRIVATE class-level `const` used as an inline-array EXTENT on an IMPORTED
// GENERIC constructor's PUBLIC parameter list could not be reconstructed by
// the importer, so the construction was rejected where the single-module
// twin compiles. Split out of #4514, which fixed the identical PUBLIC-const
// shape and replaced this shape's SIGSEGV with the clean diagnostic this
// issue's row asserted.
//
// ── THE REPRO (issue #4572, MEASURED on this branch's parent) ─────────────
//
//     module MpLib;
//     public class MpC<T>
//       private const MAX: int := 2;
//       public Tag: int;
//       public constructor(a: [MAX]int)
//         Tag := a[0];
//       end constructor;
//     end MpC;
//
//     bezelc -c lib.bzl -o lib.o                  # rc=0
//     bezelc --import lib.o -o app app.bzl
//     app.bzl:7:33: the inline-array parameter `a` has an extent that is not
//     a compile-time constant at this call site, so the argument cannot be
//     laid out (§15.1); for an IMPORTED declaration this means the
//     dependency's metadata did not carry the folded extent (§2.8.3 rule 3,
//     #4514)
//
// The single-module twin compiles and exits 7.
//
// ── THE FIX ─────────────────────────────────────────────────────────────
//
// `ModuleMetadataBuilder`'s member-export loop (src/module/
// ModuleMetadataBuilder.cpp) already widened `out.members` for a generic
// class/record beyond `isExportedMember` twice — private/internal METHODS
// (#695, `includeForCtorBody`) and private/internal FIELDS (#1224,
// `includeForMethodBody`) — so a verbatim body/signature carried across the
// wire can resolve the names it mentions. There was no equivalent widening
// for CONSTS, so a private const was simply absent from `td.members`, and
// `buildConstStubs` (ModuleLoader.cpp, added by #4514) had nothing to emit:
// `[MAX]int` re-parsed with `MAX` unbound, `TypeResolution` left
// `arraySizeValue` at -1, and #4514's codegen guard reported the diagnostic
// above instead of reading an unset `llvm::Value*`.
//
// `constructor(a: [MAX]int)` IS a public signature (§13.1); the extent is
// part of the type (§15.1); and the VISIBILITY of the const that names the
// extent is an implementation detail of the exporter, not a property of the
// signature (§2.8.3 rule 3: "the artifact interface must carry enough to
// reconstruct every public signature") — exactly the reasoning #1224 already
// applied to a private FIELD referenced from a re-parsed body. The fix adds
// a third widening, `includeForConst`, mirroring the two precedents: only
// name + type + valueText cross (via the same `convertMember` a public const
// already uses) — no storage, no ABI change, since §22.12 emits no global
// for a const.
//
// ── THE ORACLE ─────────────────────────────────────────────────────────────
//
// The importing COMPILE is fork-isolated so a regression that reintroduces
// the #4514 crash is an observable value (`kCompilerCrashed4572`) rather
// than a dead test shard. T1/T2 assert the corrected VALUE (not merely "it
// compiled") with a single-module twin as the differential control — the
// import boundary is the only difference between the two sides of each pair.

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
#include "support/unique_stem.hpp"

namespace bezel::module {
namespace {

namespace fs = std::filesystem;

struct TempFile4572 {
    std::string path;
    explicit TempFile4572(std::string p) : path(std::move(p)) {}
    ~TempFile4572() { std::remove(path.c_str()); }
};

void writeSource4572(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Return conventions (mirrors xmod_generic_ctor_const_extent_4514_test.cpp):
//   -2  the DEPENDENCY could not be produced (no toolchain) — skip the row.
//   -3  the importing COMPILE did not exit normally: it was killed by a
//       signal — the #4514 crash shape; must never reoccur here.
//   -1  the importing compile failed with diagnostics, or the app did not run.
//   >=0 the app's own exit status.
constexpr int kDepUnavailable4572 = -2;
constexpr int kCompilerCrashed4572 = -3;

bool compileIsolated4572(const std::string& src, const std::string& exePath,
                         const std::string& errPath,
                         const std::vector<bezel::Dependency>& deps,
                         bool* crashed) {
    *crashed = false;
    std::fflush(stdout);
    std::fflush(stderr);
    pid_t pid = ::fork();
    if (pid == 0) {
        int sink = ::open(errPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (sink >= 0) {
            ::dup2(sink, STDERR_FILENO);
            ::close(sink);
        }
        bezel::Driver driver;
        bool ok = bezel::test::compileFileForTest(driver, src, exePath, deps)
                      .success;
        std::fflush(stderr);
        ::_exit(ok ? 0 : 1);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    if (WIFSIGNALED(status)) {
        *crashed = true;
        return false;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

std::string readAll4572(const std::string& path) {
    std::ifstream in(path);
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

int runImported4572(const std::string& libSrc, const std::string& appSrc,
                    const std::string& tag,
                    std::string* capturedStderr = nullptr) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bz4572_" + tag + "_" + unique_stem("");

    TempFile4572 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile4572 aObj((dir / (stem + "_a.o")).string());
    writeSource4572(aSrc.path, libSrc);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success)
            return kDepUnavailable4572;
    }
    if (!std::ifstream(aObj.path).good()) return kDepUnavailable4572;

    TempFile4572 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile4572 bExe((dir / (stem + "_b")).string());
    TempFile4572 bErr((dir / (stem + "_b.err")).string());
    writeSource4572(bSrc.path, appSrc);

    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);

    bool crashed = false;
    bool ok = compileIsolated4572(bSrc.path, bExe.path, bErr.path, deps,
                                  &crashed);
    if (capturedStderr) *capturedStderr = readAll4572(bErr.path);
    if (crashed) return kCompilerCrashed4572;
    if (!ok) return -1;
    if (!std::ifstream(bExe.path).good()) return -1;

    pid_t pid = ::fork();
    if (pid == 0) {
        ::execl(bExe.path.c_str(), bExe.path.c_str(), (char*)nullptr);
        ::_exit(127);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    if (WIFSIGNALED(status)) return 1000 + WTERMSIG(status);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// The single-module twin — the differential control. Same conventions.
int runSingleModule4572(const std::string& src, const std::string& tag) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bz4572L_" + tag + "_" + unique_stem("");
    TempFile4572 sSrc((dir / (stem + ".bzl")).string());
    TempFile4572 sExe((dir / stem).string());
    TempFile4572 sErr((dir / (stem + ".err")).string());
    writeSource4572(sSrc.path, src);

    bool crashed = false;
    bool ok = compileIsolated4572(sSrc.path, sExe.path, sErr.path, {},
                                  &crashed);
    if (crashed) return kCompilerCrashed4572;
    if (!ok) return -1;
    if (!std::ifstream(sExe.path).good()) return -1;

    pid_t pid = ::fork();
    if (pid == 0) {
        ::execl(sExe.path.c_str(), sExe.path.c_str(), (char*)nullptr);
        ::_exit(127);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    if (WIFSIGNALED(status)) return 1000 + WTERMSIG(status);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// ── The declarations under test ────────────────────────────────────────────

// The issue's own class (#4572): a PRIVATE class-level const as the extent,
// identical to #4514's repro except for the const's visibility.
std::string declsPrivateConst4572(const std::string& tag) {
    return "public class MpC" + tag + "<T> "
           "  private const MAX: int := 2; "
           "  public Tag: int; "
           "  public constructor(a: [MAX]int) "
           "    Tag := a[0]; "
           "  end constructor; "
           "end MpC" + tag + ";";
}

// A different extent (3, not 2) AND the constructor's BODY also reads the
// private const — so nothing can pass by hardcoding the issue's own extent,
// and the body-read half of the reconstruction gap is exercised alongside
// the extent half. `a[0]+a[1]+a[2]+N` with N=3: 10+20+30+3 = 63.
std::string declsPrivateConstBody4572(const std::string& tag) {
    return "public class MpB" + tag + "<T> "
           "  private const N: int := 3; "
           "  public Tag: int; "
           "  public constructor(a: [N]int) "
           "    Tag := a[0] + a[1] + a[2] + N; "
           "  end constructor; "
           "end MpB" + tag + ";";
}

std::string lib4572(const std::string& tag, const std::string& decls) {
    return "module MpLib4572" + tag + "; " + decls;
}
std::string app4572(const std::string& tag, const std::string& body) {
    return "module MpApp4572" + tag + "; import MpLib4572" + tag + "; "
           "class C" + tag + " " + body + " end C" + tag + ";";
}
std::string twin4572(const std::string& tag, const std::string& decls,
                     const std::string& body) {
    return "module MpTwin4572" + tag + "; " + decls +
           " class C" + tag + " " + body + " end C" + tag + ";";
}

}  // namespace

// ===========================================================================
// T1 — §15.1 / §2.8.3 rule 3: THE ISSUE'S EXACT REPRO. Constructing an
// imported generic class whose constructor takes `[MAX]int`, where `MAX` is
// that class's own PRIVATE `const`.
//
// Pre-fix: rejected with the §15.1/#4514 diagnostic (rc=-1, no crash — #4514
// already closed the SIGSEGV; this issue is the residual reconstruction gap).
// Post-fix: compiles and the program exits 7, exactly as the twin does.
// ===========================================================================
BEZEL_TEST(xmod_generic_ctor_private_const_extent_4572,
           t1_imported_generic_ctor_private_const_extent_15_1) {
    const std::string body =
        "  public static method Main(): int "
        "    using var c := new MpCT1<int>([7, 8]); "
        "    return c.Tag; "
        "  end Main;";
    int r = runImported4572(lib4572("T1", declsPrivateConst4572("T1")),
                            app4572("T1", body), "T1");
    if (r == kDepUnavailable4572) return;
    BEZEL_EXPECT_TRUE(r != kCompilerCrashed4572);
    BEZEL_EXPECT_EQ(r, 7);
}

BEZEL_TEST(xmod_generic_ctor_private_const_extent_4572,
           t1_twin_same_program_in_one_module_15_1) {
    const std::string body =
        "  public static method Main(): int "
        "    using var c := new MpCT1L<int>([7, 8]); "
        "    return c.Tag; "
        "  end Main;";
    int r = runSingleModule4572(
        twin4572("T1L", declsPrivateConst4572("T1L"), body), "T1L");
    BEZEL_EXPECT_EQ(r, 7);
}

// ===========================================================================
// T2 — §22.12 / §14.1: the anti-false-reject control. A DIFFERENT extent (3,
// not the issue's 2) AND the constructor's BODY also reads the private const
// — so this row cannot pass by the compiler hardcoding the issue's own
// numbers, and it exercises the body-read half of the reconstruction gap
// alongside the parameter-extent half.
//
// `a[0]+a[1]+a[2]+N` = 10+20+30+3 = 63. A wrong extent cannot reach 63, and
// neither can a dropped `N`.
// ===========================================================================
BEZEL_TEST(xmod_generic_ctor_private_const_extent_4572,
           t2_private_const_read_in_ctor_body_and_extent_22_12) {
    const std::string body =
        "  public static method Main(): int "
        "    using var c := new MpBT2<int>([10, 20, 30]); "
        "    return c.Tag; "
        "  end Main;";
    int r = runImported4572(lib4572("T2", declsPrivateConstBody4572("T2")),
                            app4572("T2", body), "T2");
    if (r == kDepUnavailable4572) return;
    BEZEL_EXPECT_TRUE(r != kCompilerCrashed4572);
    BEZEL_EXPECT_EQ(r, 63);
}

BEZEL_TEST(xmod_generic_ctor_private_const_extent_4572,
           t2_twin_same_program_in_one_module_22_12) {
    const std::string body =
        "  public static method Main(): int "
        "    using var c := new MpBT2L<int>([10, 20, 30]); "
        "    return c.Tag; "
        "  end Main;";
    int r = runSingleModule4572(
        twin4572("T2L", declsPrivateConstBody4572("T2L"), body), "T2L");
    BEZEL_EXPECT_EQ(r, 63);
}

}  // namespace bezel::module
