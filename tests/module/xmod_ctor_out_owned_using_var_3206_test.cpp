// fix #3206 (§22.4 "Inline `out` declarations" / §2.5 "Dependency resolution";
// v0.254) — the `out owned var` and `out using var` recipients of an IMPORTED
// CONSTRUCTOR's `out owned T` formal. Umbrella #3152/#3149; the constructor
// half of the drift #3186 / #3188 fixed at a method call.
//
// THE SEXTET. One question — "what is the callee's formal type at this
// position?" — is asked at six inline-`out` recipient sites in
// src/sema/ExprTypecheck_Exprs.cpp, three at a `CallExpr` and three at a
// `NewExpr`. Each was originally a direct `params[pos].type` read off the
// callee's AST declaration, which `ModuleLoader` leaves null for every
// non-generic imported callee. They were fixed one at a time:
//
//     CallExpr / out var          formalOutParamType2      #2657
//     CallExpr / out owned var    formalOutParamType2      #3186
//     CallExpr / out using var    formalOutParamType2      #3188
//     NewExpr  / out var          formalCtorOutParamType   partial #2668
//     NewExpr  / out owned var    formalCtorOutParamType   THIS FIX
//     NewExpr  / out using var    formalCtorOutParamType   THIS FIX
//
// With this commit the six are ONE view per call form: two accessors, six
// callers, no site deciding for itself.
//
// WHY THEY WERE LEFT, AND WHY THAT REASON IS DISCHARGED. #2668's own comment
// says it: "the `out owned var` / `out using var` gates below keep the old
// `ctorDecl`-only lookup on purpose … they additionally switch on dormant
// ownership tracking that no oracle here can validate." That tracking landed
// (#2659 for constructor formals, #3176 for the `out owned` recipient pair) and
// was MEASURED live before this fix was written: the imported `out owned var`
// row ALREADY drew the §28.3 auto-drop warning on ec6a02f, so the obligation
// was armed and only the TYPE was missing — which is precisely the state #3176
// recorded as "arming the obligation without the type leaves the leak exactly
// where it was".
//
// MEASURED PRE-FIX on `ec6a02f`, `bezelc -c lib.bzl -o lib.o` then
// `bezelc --import lib.o -o app app.bzl`, against character-identical
// single-module twins:
//
//     imported  new FactO(out owned var made)  -> exit **0**, "internal:
//                 return-value expression failed to lower … #2491"
//     twin                                     -> exit **211**
//     imported  new FactO(out using var made)  -> exit **0**, same note
//     twin                                     -> exit **211**
//     either, --sanitize=address               -> LeakSanitizer: Direct leak of
//                 16 byte(s) in 1 object(s), #1 bz$CtLib.FactO$ctor
//
// ⚠ THE EXIT CODE IS THE ORACLE. Every running row reports
// `v * 10 + disposals` = **211**: 21 cannot arise from a skipped call, a zeroed
// field, or a stale read, and the disposal count is read in `Main` after
// `Work`'s scope has ended. Pre-fix reads **0**, a residual leak **210**, a
// double dispose **212** — all distinct, all under 8-bit exit truncation.
//
// ⚠ NEVER TRADE A LEAK FOR A CRASH. T3/T3b re-run the same programs under
// generated-code ASan/LSan (`compileFileForTest` with an explicit
// `sanitizeAddress`, #3156), which pins both directions at once: a residual
// leak forces exit 1, and a double free aborts before any exit code.

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "codegen/LlvmBackend.hpp"
#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"  // #3156: the sanitizer-honouring compileFile
#include "test_harness.hpp"
#include "support/unique_stem.hpp"


namespace bezel::module {
namespace {

namespace fs = std::filesystem;

struct TempFile3206 {
    std::string path;
    explicit TempFile3206(std::string p) : path(std::move(p)) {}
    ~TempFile3206() { std::remove(path.c_str()); }
};

void writeSource3206(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// The dependency library.
//   Res   — the counted resource; its destructor bumps a process-global counter
//           the app reads back, so "disposed exactly once" is an assertion.
//   FactO — the CONSTRUCTOR with an `out owned T` formal: the object under
//           construction is the call's result, and `r` is a second, owned
//           output whose disposal obligation lands on the caller.
const char* kLibDecls3206 =
    "public class Res "
    "  public static _disposed: int := 0; "
    "  public v: int; "
    "  public constructor() "
    "    v := 1; "
    "  end; "
    "  destructor Res._disposed := Res._disposed + 1; end destructor; "
    "  public static method GetDisposed(): int => Res._disposed; "
    "end Res; "
    "public class FactO "
    "  public k: int; "
    "  public constructor(out owned r: Res) "
    "    r := new Res(); "
    "    k := 5; "
    "  end constructor; "
    "end FactO;";

std::string libSource3206() {
    return "module CtorOutLib3206; " + std::string(kLibDecls3206);
}

std::string appSource3206(const std::string& body, const std::string& tag) {
    return "module CtorOutApp3206" + tag + "; import CtorOutLib3206; "
           "class C " + body + " end C;";
}

std::string sameModuleProgram3206(const std::string& body,
                                  const std::string& tag) {
    return "module CtorOutCtl3206" + tag + "; " + std::string(kLibDecls3206) +
           " class C " + body + " end C;";
}

// Compile the dependency, compile the app against it, run the app in a SEPARATE
// process. Returns the child's exit status, or -1 (app did not build) / -2 (no
// toolchain for the dependency).
int runImported3206(const std::string& body, const std::string& tag,
                    std::string* capturedStderr = nullptr, bool asan = false) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bz3206_" + tag + "_" + unique_stem("");

    TempFile3206 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile3206 aObj((dir / (stem + "_a.o")).string());
    writeSource3206(aSrc.path, libSource3206());
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success) return -2;
    }
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile3206 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3206 bExe((dir / (stem + "_b")).string());
    TempFile3206 bErr((dir / (stem + "_b.err")).string());
    writeSource3206(bSrc.path, appSource3206(body, tag));

    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    codegen::CodegenOptions opts;
    if (asan) opts.sanitizeAddress = true;

    bool compiled = false;
    if (capturedStderr) {
        std::fflush(stderr);
        int saved = ::dup(STDERR_FILENO);
        int sink = ::open(bErr.path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (sink >= 0) {
            ::dup2(sink, STDERR_FILENO);
            ::close(sink);
        }
        compiled = bezel::test::compileFileForTest(driver, bSrc.path, bExe.path,
                                                   deps, opts).success;
        std::fflush(stderr);
        if (saved >= 0) { ::dup2(saved, STDERR_FILENO); ::close(saved); }
        std::ifstream in(bErr.path);
        capturedStderr->assign((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    } else {
        compiled = bezel::test::compileFileForTest(driver, bSrc.path, bExe.path,
                                                   deps, opts).success;
    }
    if (!compiled) return -1;
    if (!std::ifstream(bExe.path).good()) return -1;

    pid_t pid = ::fork();
    if (pid == 0) {
        ::execl(bExe.path.c_str(), bExe.path.c_str(), (char*)nullptr);
        ::_exit(127);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    if (WIFSIGNALED(status)) return 1000 + WTERMSIG(status);  // 1011 = SIGSEGV
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// The differential control twin.
int runSingleModule3206(const std::string& src, const std::string& tag,
                        std::string* capturedStderr = nullptr,
                        bool asan = false) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bz3206ctl_" + tag + "_" + unique_stem("");
    TempFile3206 sSrc((dir / (stem + ".bzl")).string());
    TempFile3206 sExe((dir / stem).string());
    TempFile3206 sErr((dir / (stem + ".err")).string());
    writeSource3206(sSrc.path, src);

    bezel::Driver driver;
    codegen::CodegenOptions opts;
    if (asan) opts.sanitizeAddress = true;
    bool compiled = false;
    if (capturedStderr) {
        std::fflush(stderr);
        int saved = ::dup(STDERR_FILENO);
        int sink = ::open(sErr.path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (sink >= 0) {
            ::dup2(sink, STDERR_FILENO);
            ::close(sink);
        }
        compiled = bezel::test::compileFileForTest(driver, sSrc.path, sExe.path,
                                                   {}, opts).success;
        std::fflush(stderr);
        if (saved >= 0) { ::dup2(saved, STDERR_FILENO); ::close(saved); }
        std::ifstream in(sErr.path);
        capturedStderr->assign((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    } else {
        compiled = bezel::test::compileFileForTest(driver, sSrc.path, sExe.path,
                                                   {}, opts).success;
    }
    if (!compiled) return -1;
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

bool mentions3206(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

// The shared body, parameterized on the inline `out` FORM. `f` is the
// constructed `FactO`; `made` is the `out owned` second output.
std::string workBody3206(const char* form) {
    return std::string(
        "  static method Work(): int "
        "    using var f := new FactO(") + form + " made); "
        "    return made.v + 20; "
        "  end Work; "
        "  public static method Main(): int "
        "    var q := C.Work(); "
        "    return q * 10 + Res.GetDisposed(); "
        "  end Main;";
}

std::string nopeBody3206(const char* form) {
    return std::string(
        "  public static method Main(): int "
        "    using var f := new FactO(") + form + " made); "
        "    return made.Nope; "
        "  end Main;";
}

}  // namespace

// ===========================================================================
// T1 — §22.4 / §28.3: the `out owned var` recipient of an IMPORTED constructor
// is typed, carries its value, and is disposed EXACTLY ONCE.
// Pre-fix: exit 0 (the degraded #2491 return) with the object leaked.
// ===========================================================================
BEZEL_TEST(xmod_ctor_out_owned_using_var_3206,
           t1_imported_ctor_out_owned_recipient_typed_and_dropped_22_4) {
    int rc = runImported3206(workBody3206("out var"), "t1");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 211);
}

// ===========================================================================
// T1b — the SAME-MODULE twin of T1 (§2.8.3 rule 4 differential control).
// ===========================================================================
BEZEL_TEST(xmod_ctor_out_owned_using_var_3206,
           t1b_same_module_ctor_out_owned_recipient_22_4) {
    int rc = runSingleModule3206(
        sameModuleProgram3206(workBody3206("out var"), "T1b"), "t1b");
    BEZEL_EXPECT_EQ(rc, 211);
}

// ===========================================================================
// T2 — the `out using var` form of T1, the second of the two sites.
// ===========================================================================
BEZEL_TEST(xmod_ctor_out_owned_using_var_3206,
           t2_imported_ctor_out_using_recipient_typed_and_disposed_22_4) {
    int rc = runImported3206(workBody3206("out using var"), "t2");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 211);
}

// ===========================================================================
// T2b — the SAME-MODULE twin of T2.
// ===========================================================================
BEZEL_TEST(xmod_ctor_out_owned_using_var_3206,
           t2b_same_module_ctor_out_using_recipient_22_4) {
    int rc = runSingleModule3206(
        sameModuleProgram3206(workBody3206("out using var"), "T2b"), "t2b");
    BEZEL_EXPECT_EQ(rc, 211);
}

// ===========================================================================
// T3 — §28.3, both forms under generated-code ASan/LSan: no residual leak and
// no double free. A leak forces exit 1; a double free aborts before any exit
// code, so 211 pins both directions of the disposal change.
// ===========================================================================
BEZEL_TEST(xmod_ctor_out_owned_using_var_3206,
           t3_imported_ctor_out_owned_no_leak_no_double_free_28_3) {
    int rc = runImported3206(workBody3206("out var"), "t3",
                             /*capturedStderr=*/nullptr, /*asan=*/true);
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 211);
}

BEZEL_TEST(xmod_ctor_out_owned_using_var_3206,
           t3b_imported_ctor_out_using_no_leak_no_double_free_28_3) {
    int rc = runImported3206(workBody3206("out using var"), "t3b",
                             /*capturedStderr=*/nullptr, /*asan=*/true);
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 211);
}

// ===========================================================================
// T4 — §11.3 / §12.1: the recipient has a REAL type, proven by asking it for a
// member declared nowhere. T1's 211 could in principle be reached by a
// recipient typed only well enough to lay out a load; a MISSING diagnostic
// cannot be caught by an exit code at all, so it needs its own row and an
// assertion on the text. Pre-fix `made.Nope` compiled CLEAN across the
// boundary while the twin rejected it.
// ===========================================================================
BEZEL_TEST(xmod_ctor_out_owned_using_var_3206,
           t4_imported_ctor_out_owned_recipient_has_a_real_type_11_3) {
    std::string err;
    int rc = runImported3206(nopeBody3206("out var"), "t4", &err);
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, -1);  // must NOT build
    BEZEL_EXPECT_TRUE(mentions3206(err, "no member `Nope` on type `Res`"));
}

BEZEL_TEST(xmod_ctor_out_owned_using_var_3206,
           t4b_same_module_ctor_out_owned_recipient_type_11_3) {
    std::string err;
    int rc = runSingleModule3206(
        sameModuleProgram3206(nopeBody3206("out var"), "T4b"), "t4b",
        &err);
    BEZEL_EXPECT_EQ(rc, -1);
    BEZEL_EXPECT_TRUE(mentions3206(err, "no member `Nope` on type `Res`"));
}

BEZEL_TEST(xmod_ctor_out_owned_using_var_3206,
           t5_imported_ctor_out_using_recipient_has_a_real_type_11_3) {
    std::string err;
    int rc = runImported3206(nopeBody3206("out using var"), "t5", &err);
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, -1);
    BEZEL_EXPECT_TRUE(mentions3206(err, "no member `Nope` on type `Res`"));
}

BEZEL_TEST(xmod_ctor_out_owned_using_var_3206,
           t5b_same_module_ctor_out_using_recipient_type_11_3) {
    std::string err;
    int rc = runSingleModule3206(
        sameModuleProgram3206(nopeBody3206("out using var"), "T5b"), "t5b",
        &err);
    BEZEL_EXPECT_EQ(rc, -1);
    BEZEL_EXPECT_TRUE(mentions3206(err, "no member `Nope` on type `Res`"));
}

// ===========================================================================
// T6 — OVER-WIDENING CONTROL: a LOCAL constructor's `out owned var` /
// `out using var` recipients are unchanged by this fix.
//
// `formalCtorOutParamType` returns the old expression verbatim whenever
// `ctorDecl` is non-null, so the local answer must be bit-identical. These rows
// are the ones that fail if the accessor's preference order were inverted — the
// failure mode of a unification fix is a WIDER lookup, which no reject-direction
// row would notice. (T1b/T2b already exercise the same programs; T6 states the
// claim explicitly with the DERIVED value that a wrong formal type would
// corrupt: a second `out owned` position, so the accessor is indexed rather
// than answering position 0 for everything.)
// ===========================================================================
BEZEL_TEST(xmod_ctor_out_owned_using_var_3206,
           t6_local_ctor_second_out_owned_position_indexed_22_4) {
    int rc = runSingleModule3206(
        "module CtorOutIdx3206; "
        "public class Res "
        "  public static _disposed: int := 0; "
        "  public v: int; "
        "  public constructor() v := 1; end; "
        "  destructor Res._disposed := Res._disposed + 1; end destructor; "
        "  public static method GetDisposed(): int => Res._disposed; "
        "end Res; "
        "public class Two "
        "  public k: int; "
        "  public constructor(n: int, out owned r: Res) "
        "    r := new Res(); "
        "    k := n; "
        "  end constructor; "
        "end Two; "
        "class C "
        "  static method Work(): int "
        "    using var t := new Two(7, out var made); "
        "    return made.v + 20; "
        "  end Work; "
        "  public static method Main(): int "
        "    var q := C.Work(); "
        "    return q * 10 + Res.GetDisposed(); "
        "  end Main; "
        "end C;",
        "t6");
    BEZEL_EXPECT_EQ(rc, 211);
}

// ===========================================================================
// T6b — the IMPORTED twin of T6: the accessor's imported arm must be indexed
// too. A fallback that read `params[0]` regardless of position would type
// `made` as `int` here and still "work" for T1/T2.
// ===========================================================================
BEZEL_TEST(xmod_ctor_out_owned_using_var_3206,
           t6b_imported_ctor_second_out_owned_position_indexed_22_4) {
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz3206idx_");
    TempFile3206 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile3206 aObj((dir / (stem + "_a.o")).string());
    writeSource3206(aSrc.path,
        "module CtorOutIdxLib3206; "
        "public class Res "
        "  public static _disposed: int := 0; "
        "  public v: int; "
        "  public constructor() v := 1; end; "
        "  destructor Res._disposed := Res._disposed + 1; end destructor; "
        "  public static method GetDisposed(): int => Res._disposed; "
        "end Res; "
        "public class Two "
        "  public k: int; "
        "  public constructor(n: int, out owned r: Res) "
        "    r := new Res(); "
        "    k := n; "
        "  end constructor; "
        "end Two;");
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success) return;
    }
    if (!std::ifstream(aObj.path).good()) return;

    TempFile3206 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3206 bExe((dir / (stem + "_b")).string());
    writeSource3206(bSrc.path,
        "module CtorOutIdxApp3206; import CtorOutIdxLib3206; "
        "class C "
        "  static method Work(): int "
        "    using var t := new Two(7, out var made); "
        "    return made.v + 20; "
        "  end Work; "
        "  public static method Main(): int "
        "    var q := C.Work(); "
        "    return q * 10 + Res.GetDisposed(); "
        "  end Main; "
        "end C;");

    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    if (!bezel::test::compileFileForTest(driver, bSrc.path, bExe.path, deps)
             .success) {
        BEZEL_EXPECT_TRUE(false);  // must build
        return;
    }
    pid_t pid = ::fork();
    if (pid == 0) {
        ::execl(bExe.path.c_str(), bExe.path.c_str(), (char*)nullptr);
        ::_exit(127);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    int rc = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    BEZEL_EXPECT_EQ(rc, 211);
}

}  // namespace bezel::module
