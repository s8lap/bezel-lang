// fix #4514 (§15.1 "Inline arrays" / §22.12 "Static members" / §14.1 "Generic
// declarations" / §2.8.3 rule 3 "Transitive closure"; spec v0.306) — a
// `const`-NAMED inline-array extent on an IMPORTED GENERIC constructor's
// parameter SEGV'd the importing compile.
//
// ── THE REPRO ──────────────────────────────────────────────────────────────
//
//     module MxLib;
//     public class MxC<T>
//       public const MAX: int := 2;
//       public Tag: int;
//       public constructor(a: [MAX]int) Tag := a[0]; end constructor;
//     end MxC;
//
//     bezelc -c lib.bzl -o lib.o          # rc=0
//     bezelc --import lib.o -o app app.bzl
//     Segmentation fault (core dumped)    # rc=139
//
// The single-module twin compiles and exits 7. Replacing `[MAX]int` with the
// LITERAL `[2]int` builds cross-module and exits 7 — so the extent being a
// `const` NAME is the trigger, not inline-array parameters in general.
//
// ── TWO DEFECTS, ONE REPRO ─────────────────────────────────────────────────
//
// (1) THE RECONSTRUCTION GAP (src/module/ModuleLoader.cpp). Each of the four
//     synthetic reconstruction sources declares the class's FIELDS, PROPERTIES
//     and METHOD STUBS so a VERBATIM text carried across the wire can resolve
//     the names it mentions — and never declared its `const` members. The
//     constructor's own declaration text is carried verbatim, so `[MAX]int`
//     re-parsed with `MAX` bound to nothing. §15.1's extent expression is NOT
//     visited by ExprTC (TypeResolution folds it in place and leaves it alone
//     when it cannot), so this failed SILENTLY: no diagnostic, no
//     `semaDiag->hasErrors()`, and the §14.1/#783 soundness gate therefore
//     ATTACHED a constructor decl whose parameter's `arraySizeValue` was -1.
//     Fixed by `buildConstStubs`, called from all four builders.
//
// (2) THE CRASH (src/codegen/LlvmBackend_Assign.cpp). `buildOrderedArgs`
//     declared `llvm::Value* v;` UNINITIALIZED. Its §15.4 inline-array arm
//     assigns `v` only when `lowerTypeRef` of the formal yields an
//     `llvm::ArrayType`; for an unfolded extent that function returns the bare
//     `i32` placeholder instead ("what keeps an unfoldable size failing
//     loudly"), the arm fell through, and every later read of `v` touched an
//     indeterminate value. MEASURED under gdb on the repro: `v = 0xffffffff`,
//     SIGSEGV at `v->getType()` (LlvmBackend_Assign.cpp, the `isVoidTy()`
//     guard), from `lowerNew`. `v` is now null-initialized AND the declining
//     arm reports a SPECIFIC diagnostic naming the parameter and the unfolded
//     extent, so the shape fails loudly at compile time instead of crashing.
//
// Fixing (1) alone would have left (2) as a latent crash for every FUTURE
// reconstruction gap, which is why both ship. T3 is what pins (2) on its own:
// a PRIVATE `const` extent, which the metadata does not carry at all (the
// generic-class widenings cover private METHODS (#695) and private FIELDS
// (#1224), not consts), so it still cannot reconstruct — and must now produce
// that diagnostic rather than a SIGSEGV.
//
// ── THE ORACLE ─────────────────────────────────────────────────────────────
//
// The importing COMPILE is fork-isolated here, so "the compiler died on a
// signal" is an observable value (`kCompilerCrashed`) instead of taking the
// test binary down with it. That matters twice over: it makes the pre-fix
// behaviour a FAILING assertion rather than a crashed shard, and it lets T3
// assert the EXACT diagnostic text — per CLAUDE.md's vacuous-oracle catalogue,
// `failedAtCompileTime`-shaped helpers pass on BOTH sides of a crash fix
// because the crash's own backstop diagnostic carries a real location.

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

struct TempFile4514 {
    std::string path;
    explicit TempFile4514(std::string p) : path(std::move(p)) {}
    ~TempFile4514() { std::remove(path.c_str()); }
};

void writeSource4514(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Return conventions shared by both runners:
//   -2  the DEPENDENCY could not be produced (no toolchain) — skip the row.
//   -3  the importing COMPILE did not exit normally: it was killed by a signal.
//       This is the pre-fix behaviour of the shapes under test (SIGSEGV).
//   -1  the importing compile failed with diagnostics, or the app did not run.
//   >=0 the app's own exit status.
constexpr int kDepUnavailable4514 = -2;
constexpr int kCompilerCrashed4514 = -3;

// Compile `src` (against `deps`) to `exePath` in a FORKED child, so a compiler
// crash is reported rather than fatal. The child's stderr is redirected into
// `errPath`. Returns true when the child exited 0; sets `*crashed` when the
// child died on a signal.
bool compileIsolated4514(const std::string& src, const std::string& exePath,
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

std::string readAll4514(const std::string& path) {
    std::ifstream in(path);
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

// Compile the dependency, compile the importer against it (fork-isolated), run
// the program in a separate process. `capturedStderr`, when non-null, receives
// the importing compile's stderr — the only way to assert diagnostic TEXT.
int runImported4514(const std::string& libSrc, const std::string& appSrc,
                    const std::string& tag,
                    std::string* capturedStderr = nullptr) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bz4514_" + tag + "_" + unique_stem("");

    TempFile4514 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile4514 aObj((dir / (stem + "_a.o")).string());
    writeSource4514(aSrc.path, libSrc);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success)
            return kDepUnavailable4514;
    }
    if (!std::ifstream(aObj.path).good()) return kDepUnavailable4514;

    TempFile4514 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile4514 bExe((dir / (stem + "_b")).string());
    TempFile4514 bErr((dir / (stem + "_b.err")).string());
    writeSource4514(bSrc.path, appSrc);

    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);

    bool crashed = false;
    bool ok = compileIsolated4514(bSrc.path, bExe.path, bErr.path, deps,
                                  &crashed);
    if (capturedStderr) *capturedStderr = readAll4514(bErr.path);
    if (crashed) return kCompilerCrashed4514;
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
int runSingleModule4514(const std::string& src, const std::string& tag) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bz4514L_" + tag + "_" + unique_stem("");
    TempFile4514 sSrc((dir / (stem + ".bzl")).string());
    TempFile4514 sExe((dir / stem).string());
    TempFile4514 sErr((dir / (stem + ".err")).string());
    writeSource4514(sSrc.path, src);

    bool crashed = false;
    bool ok = compileIsolated4514(sSrc.path, sExe.path, sErr.path, {},
                                  &crashed);
    if (crashed) return kCompilerCrashed4514;
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
// Used VERBATIM as the dependency module's body and, spliced in, as part of the
// single-module twin, so a row can only pass by the two sides agreeing.

// The issue's own class: a PUBLIC class-level const as the extent.
std::string declsPublicConst4514(const std::string& tag) {
    return "public class MxC" + tag + "<T> "
           "  public const MAX: int := 2; "
           "  public Tag: int; "
           "  public constructor(a: [MAX]int) "
           "    Tag := a[0]; "
           "  end constructor; "
           "end MxC" + tag + ";";
}

// A different extent and a const whose value is NOT the argument count of the
// issue's repro — so nothing can pass by hardcoding 2. The ctor also READS the
// const in its body, which is the other half of the reconstruction gap: a
// verbatim ctor body that names a class-level const.
std::string declsConstBody4514(const std::string& tag) {
    return "public class MxB" + tag + "<T> "
           "  public const N: int := 3; "
           "  public Tag: int; "
           "  public constructor(a: [N]int) "
           "    Tag := a[0] + a[1] + a[2] + N; "
           "  end constructor; "
           "end MxB" + tag + ";";
}

// The LITERAL-extent control: identical but for `[2]int`. Built cross-module
// before this fix and must keep building.
std::string declsLiteral4514(const std::string& tag) {
    return "public class MxL" + tag + "<T> "
           "  public Tag: int; "
           "  public constructor(a: [2]int) "
           "    Tag := a[0]; "
           "  end constructor; "
           "end MxL" + tag + ";";
}

// A const read from a METHOD body, with no inline array anywhere. This is the
// ANTI-REGRESSION shape: before this fix `K` did not resolve in the synthetic
// method source, the §14.1/#783 soundness gate declined to attach the body, and
// the importer fell back to the dependency's own compiled template — which for
// `T = int` happens to answer correctly. Declaring the const makes the body
// resolve and ATTACH, so codegen now monomorphizes it, and the reference must
// therefore also REMAP into the main context. MEASURED with the stubs in place
// and the remap missing: 0, with `internal: return-value expression failed to
// lower` (#2491). The stub and the remap are one change; this row is what says
// so. It passes on both sides of the fix by construction — that is the point.
std::string declsMethodConst4514(const std::string& tag) {
    return "public class McC" + tag + "<T> "
           "  public const K: int := 11; "
           "  public Tag: int; "
           "  public method Bump(): int "
           "    return Tag + K; "
           "  end Bump; "
           "end McC" + tag + ";";
}

// A PRIVATE const extent. The wire carries no private const (the generic-class
// widenings cover private methods and private fields only), so this shape
// cannot reconstruct even with the loader fix — and is therefore the row that
// pins the CODEGEN guard on its own.
std::string declsPrivateConst4514(const std::string& tag) {
    return "public class MxP" + tag + "<T> "
           "  private const MAX: int := 2; "
           "  public Tag: int; "
           "  public constructor(a: [MAX]int) "
           "    Tag := a[0]; "
           "  end constructor; "
           "end MxP" + tag + ";";
}

std::string lib4514(const std::string& tag, const std::string& decls) {
    return "module MxLib4514" + tag + "; " + decls;
}
std::string app4514(const std::string& tag, const std::string& body) {
    return "module MxApp4514" + tag + "; import MxLib4514" + tag + "; "
           "class C" + tag + " " + body + " end C" + tag + ";";
}
std::string twin4514(const std::string& tag, const std::string& decls,
                     const std::string& body) {
    return "module MxTwin4514" + tag + "; " + decls +
           " class C" + tag + " " + body + " end C" + tag + ";";
}

}  // namespace

// ===========================================================================
// T1 — §15.1 / §22.12 / §2.8.3 rule 3: THE ISSUE'S REPRO. Constructing an
// imported generic class whose constructor takes `[MAX]int`, where `MAX` is
// that class's own `public const`.
//
// Pre-fix: the importing compile SIGSEGV'd (`kCompilerCrashed4514`).
// Post-fix: it compiles and the program exits 7, exactly as the twin does.
// ===========================================================================
BEZEL_TEST(xmod_generic_ctor_const_extent_4514,
           t1_imported_generic_ctor_const_extent_15_1) {
    const std::string body =
        "  public static method Main(): int "
        "    using var c := new MxCT1<int>([7, 8]); "
        "    return c.Tag; "
        "  end Main;";
    int r = runImported4514(lib4514("T1", declsPublicConst4514("T1")),
                            app4514("T1", body), "T1");
    if (r == kDepUnavailable4514) return;
    BEZEL_EXPECT_EQ(r, 7);
}

BEZEL_TEST(xmod_generic_ctor_const_extent_4514,
           t1_twin_same_program_in_one_module_15_1) {
    const std::string body =
        "  public static method Main(): int "
        "    using var c := new MxCT1L<int>([7, 8]); "
        "    return c.Tag; "
        "  end Main;";
    int r = runSingleModule4514(
        twin4514("T1L", declsPublicConst4514("T1L"), body), "T1L");
    BEZEL_EXPECT_EQ(r, 7);
}

// ===========================================================================
// T2 — §22.12 / §14.1: a different extent (3) AND the constructor's BODY reads
// the const. The body half is the other face of the same gap: `parseAndAttach-
// CtorBody` TOLERATES typecheck errors ("we only need the exprSymbols"), so a
// body naming an undeclared `N` attached anyway.
//
// `a[0]+a[1]+a[2]+N` = 10+20+30+3 = 63. A wrong extent cannot reach 63, and
// neither can a dropped `N`.
// ===========================================================================
BEZEL_TEST(xmod_generic_ctor_const_extent_4514,
           t2_const_read_in_ctor_body_and_extent_22_12) {
    const std::string body =
        "  public static method Main(): int "
        "    using var c := new MxBT2<int>([10, 20, 30]); "
        "    return c.Tag; "
        "  end Main;";
    int r = runImported4514(lib4514("T2", declsConstBody4514("T2")),
                            app4514("T2", body), "T2");
    if (r == kDepUnavailable4514) return;
    BEZEL_EXPECT_EQ(r, 63);
}

BEZEL_TEST(xmod_generic_ctor_const_extent_4514,
           t2_twin_same_program_in_one_module_22_12) {
    const std::string body =
        "  public static method Main(): int "
        "    using var c := new MxBT2L<int>([10, 20, 30]); "
        "    return c.Tag; "
        "  end Main;";
    int r = runSingleModule4514(
        twin4514("T2L", declsConstBody4514("T2L"), body), "T2L");
    BEZEL_EXPECT_EQ(r, 63);
}

// ===========================================================================
// T3 ★ THE CODEGEN-GUARD ROW — §15.1 / §2.8.3 rule 3. A PRIVATE const extent
// is not on the wire at all, so the parameter's extent STILL cannot be
// reconstructed with the loader fix in place. That is exactly the residual
// shape the uninitialized `llvm::Value* v` turned into a SIGSEGV.
//
// The oracle is deliberately the SPECIFIC diagnostic, not "it failed at compile
// time": the crash's own backstop line carries a real location, so a generic
// "did it fail?" check passes on BOTH sides of this fix (CLAUDE.md's
// vacuous-oracle catalogue).
//
//   pre-fix : kCompilerCrashed4514 (SIGSEGV in Emitter::buildOrderedArgs)
//   post-fix: -1, with the §15.1 / #4514 diagnostic naming the parameter
// ===========================================================================
BEZEL_TEST(xmod_generic_ctor_const_extent_4514,
           t3_unreconstructable_extent_diagnoses_instead_of_crashing_15_1) {
    const std::string body =
        "  public static method Main(): int "
        "    using var c := new MxPT3<int>([7, 8]); "
        "    return c.Tag; "
        "  end Main;";
    std::string err;
    int r = runImported4514(lib4514("T3", declsPrivateConst4514("T3")),
                            app4514("T3", body), "T3", &err);
    if (r == kDepUnavailable4514) return;
    // The compiler must not die on a signal.
    BEZEL_EXPECT_TRUE(r != kCompilerCrashed4514);
    // It must reject, and it must say WHY — naming the parameter, §15.1, and
    // the issue. Each substring is load-bearing: the generic `#1386`
    // "lowerExpr returned null" line the null `v` would otherwise reach carries
    // none of them.
    BEZEL_EXPECT_EQ(r, -1);
    BEZEL_EXPECT_TRUE(err.find("inline-array parameter `a`") !=
                      std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("not a compile-time constant") !=
                      std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("\xC2\xA7""15.1") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("#4514") != std::string::npos);
}

// ===========================================================================
// T4 — the LITERAL-extent control. `[2]int` built cross-module BEFORE this fix
// (it is the issue's own narrowing control) and must keep building. It is the
// row that would catch a fix that broke inline-array parameters in general.
// ===========================================================================
BEZEL_TEST(xmod_generic_ctor_const_extent_4514,
           t4_literal_extent_control_still_builds_15_1) {
    const std::string body =
        "  public static method Main(): int "
        "    using var c := new MxLT4<int>([7, 8]); "
        "    return c.Tag; "
        "  end Main;";
    int r = runImported4514(lib4514("T4", declsLiteral4514("T4")),
                            app4514("T4", body), "T4");
    if (r == kDepUnavailable4514) return;
    BEZEL_EXPECT_EQ(r, 7);
}

// ===========================================================================
// T5 — §22.12 / §14.1: the ANTI-REGRESSION row. An imported generic's METHOD
// body reads a class-level const, with no inline array in sight.
//
// This row is green on BOTH sides of the fix, deliberately: pre-fix the body
// failed the soundness gate and the importer used the dependency's own
// template (correct here); post-fix the body resolves, attaches and is
// monomorphized, so the const reference has to reach `mainCtx` for codegen to
// fold it. It is the row that failed while `buildConstStubs` was in place and
// the `ConstMemberSymbol` remap arm was not — 0, not 16.
//
// `Tag(5) + K(11)` = 16.
// ===========================================================================
BEZEL_TEST(xmod_generic_ctor_const_extent_4514,
           t5_const_read_from_method_body_still_folds_22_12) {
    const std::string body =
        "  public static method Main(): int "
        "    using var c := new McCT5<int>(); "
        "    c.Tag := 5; "
        "    return c.Bump(); "
        "  end Main;";
    int r = runImported4514(lib4514("T5", declsMethodConst4514("T5")),
                            app4514("T5", body), "T5");
    if (r == kDepUnavailable4514) return;
    BEZEL_EXPECT_EQ(r, 16);
}

BEZEL_TEST(xmod_generic_ctor_const_extent_4514,
           t5_twin_same_program_in_one_module_22_12) {
    const std::string body =
        "  public static method Main(): int "
        "    using var c := new McCT5L<int>(); "
        "    c.Tag := 5; "
        "    return c.Bump(); "
        "  end Main;";
    int r = runSingleModule4514(
        twin4514("T5L", declsMethodConst4514("T5L"), body), "T5L");
    BEZEL_EXPECT_EQ(r, 16);
}

}  // namespace bezel::module
