// fix #3188 (§22.4 "Inline `out` declarations" / §2.5 "Dependency resolution";
// v0.254) — the `out using var` recipient of an IMPORTED method's
// `out owned T` formal. Umbrella #3152/#3149.
//
// WHAT THE DEFECT WAS — AND HOW MANY COPIES IT HAS. There are SIX inline-`out`
// recipient typing sites in src/sema/ExprTypecheck_Exprs.cpp, three at a method
// call (`CallExpr`) and three at a constructor call (`NewExpr`), and each one
// answers the same question: "what is the callee's formal type at this
// position?" Each was written as a direct `params[pos].type` read off the
// callee's AST declaration, which is null for every non-generic IMPORTED
// callee. They were fixed one at a time:
//
//     CallExpr / out var         -> formalOutParamType2       (#2657)
//     CallExpr / out owned var   -> formalOutParamType2       (#3186)
//     CallExpr / out using var   -> THIS FIX  (#3188)
//     NewExpr  / out var         -> formalCtorOutParamType    (partial #2668)
//     NewExpr  / out owned var   -> #3206
//     NewExpr  / out using var   -> #3206
//
// #3188 calls itself "the third copy"; measurement says it is the fourth of six
// with two more still open, which is why #3206 was filed and lands beside this.
//
// MEASURED PRE-FIX on `ec6a02f`, `bezelc -c lib.bzl -o lib.o` then
// `bezelc --import lib.o -o app app.bzl`, against a character-identical
// single-module twin (the ONLY difference is the module boundary):
//
//     imported  Fact.Make(out using var made)   -> exit **0**, with
//                 "internal: return-value expression failed to lower to a value
//                  (degraded to a zero/null return value) — #2491"
//     twin      Fact.Make(out using var made)   -> exit **211**
//     imported, --sanitize=address              -> LeakSanitizer: Direct leak of
//                 16 byte(s) in 1 object(s), #1 bz$UsLib.Fact$Make
//     twin,     --sanitize=address              -> clean, exit 211
//
// So: a silent wrong VALUE plus a per-call 16-byte leak, announced only by an
// `internal:` note. With no `declType` the recipient is a recipient codegen can
// lower no disposal for, and every member read on it degrades.
//
// ⚠ THE EXIT CODE IS THE ORACLE, NOT "it did not crash". Every running row
// reports `v * 10 + disposals` = **211**: `v` is 21 (the callee's `Res` carries
// `v = 1` and the caller adds 20, so 21 cannot arise from a skipped call, a
// zeroed field, or a stale read) and the disposal count is exactly 1, read in
// `Main` AFTER `Work`'s scope has ended so the `using var` discharge is inside
// the measured window. Every failure mode this fix touches lands on a DIFFERENT
// value: pre-fix **0** (the degraded return), a leak **210**, a double dispose
// **212** — all under the harness's 8-bit exit truncation.
//
// ⚠ ASAN IS A SECOND ORACLE HERE, NOT THE PRIMARY ONE. The dependency artifact
// cannot be instrumented (`--sanitize=address` is rejected together with `-c`),
// so the callee's own code is uninstrumented; what still works is LeakSanitizer,
// because linking the ASan runtime into the EXECUTABLE interposes `malloc`
// process-wide (see tests/support/driver_run_support.hpp). T3 therefore pins the
// leak directly, and T1's disposal counter pins it independently of ASan.
//
// ⚠ OVER-REJECTION / OVER-WIDENING CONTROL. This fix ADDS a source to a type
// lookup, so the risk is not a false reject but a silently WIDER lookup. T4 is
// the row that catches it: `OwningStack<T>.TryPop(out using var it)` is a
// BUILTIN callee with no `MethodSymbol` at all, and the whole reason this
// handler has ~120 lines of receiver-generic-argument derivation below the
// declaration lookup. If `formalOutParamType2` started answering for a builtin,
// that derivation would be bypassed and T4 would move off 211.

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

struct TempFile3188 {
    std::string path;
    explicit TempFile3188(std::string p) : path(std::move(p)) {}
    ~TempFile3188() { std::remove(path.c_str()); }
};

void writeSource3188(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// The dependency library.
//   Res  — the counted resource: its destructor bumps a process-global counter
//          the app reads back through GetDisposed(), so "disposed exactly once"
//          is an assertion rather than an inference from "no crash".
//   Fact — `Make(out owned r: Res)` is the `out owned` formal §22.4 admits the
//          `out using var` spelling against; the callee allocates and the CALLER
//          receives the disposal obligation.
const char* kLibDecls3188 =
    "public class Res "
    "  public static _disposed: int := 0; "
    "  public v: int; "
    "  public constructor() "
    "    v := 1; "
    "  end; "
    "  destructor Res._disposed := Res._disposed + 1; end destructor; "
    "  public static method GetDisposed(): int => Res._disposed; "
    "end Res; "
    "public class Fact "
    "  public static method Make(out owned r: Res): bool "
    "    r := new Res(); "
    "    return true; "
    "  end Make; "
    "end Fact;";

std::string libSource3188() {
    return "module OutUsingLib3188; " + std::string(kLibDecls3188);
}

std::string appSource3188(const std::string& body, const std::string& tag) {
    return "module OutUsingApp3188" + tag + "; import OutUsingLib3188; "
           "class C " + body + " end C;";
}

// The single-module twin: character-identical declarations, no `import`
// anywhere. The only difference from an imported row is the module boundary.
std::string sameModuleProgram3188(const std::string& body,
                                  const std::string& tag) {
    return "module OutUsingCtl3188" + tag + "; " + std::string(kLibDecls3188) +
           " class C " + body + " end C;";
}

// Compile the dependency, compile the app against it, run the app in a SEPARATE
// process (per tests/README.md). Returns the child's exit status, or:
//   -1 compile/link/run failure (the app did not build)
//   -2 the dependency itself could not be produced (no toolchain)
// `asan` forces generated-code ASan/LSan on the APP compile — the dependency
// object cannot be instrumented, but the interposed allocator still sees its
// allocations, which is what the 16-byte leak row needs.
int runImported3188(const std::string& body, const std::string& tag,
                    std::string* capturedStderr = nullptr, bool asan = false) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bz3188_" + tag + "_" + unique_stem("");

    TempFile3188 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile3188 aObj((dir / (stem + "_a.o")).string());
    writeSource3188(aSrc.path, libSource3188());
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success) return -2;
    }
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile3188 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3188 bExe((dir / (stem + "_b")).string());
    TempFile3188 bErr((dir / (stem + "_b.err")).string());
    writeSource3188(bSrc.path, appSource3188(body, tag));

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

// Compile and run a SINGLE-module program — the differential control twin.
int runSingleModule3188(const std::string& src, const std::string& tag,
                        std::string* capturedStderr = nullptr,
                        bool asan = false) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bz3188ctl_" + tag + "_" + unique_stem("");
    TempFile3188 sSrc((dir / (stem + ".bzl")).string());
    TempFile3188 sExe((dir / stem).string());
    TempFile3188 sErr((dir / (stem + ".err")).string());
    writeSource3188(sSrc.path, src);

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

bool mentions3188(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

// The body every "it runs" row shares. `made`'s disposal is the `using var`
// scope-end discharge at the end of `Work`; `Main` reads the counter after it.
const char* kWorkBody3188 =
    "  static method Work(): int "
    "    var ok := Fact.Make(out using var made); "
    "    if not ok then "
    "      return 0; "
    "    end if; "
    "    return made.v + 20; "
    "  end Work; "
    "  public static method Main(): int "
    "    var q := C.Work(); "
    "    return q * 10 + Res.GetDisposed(); "
    "  end Main;";

}  // namespace

// ===========================================================================
// T1 — §22.4 / §2.5: the `out using var` recipient of an IMPORTED method is
// typed, carries its value, and is disposed EXACTLY ONCE.
//
// Pre-fix: exit 0 (the degraded `#2491` return) with the object leaked.
// ===========================================================================
BEZEL_TEST(xmod_method_out_using_var_3188,
           t1_imported_out_using_recipient_typed_and_disposed_22_4) {
    int rc = runImported3188(kWorkBody3188, "t1");
    if (rc == -2) return;  // no toolchain for the dependency
    BEZEL_EXPECT_EQ(rc, 211);
}

// ===========================================================================
// T1b — the SAME-MODULE twin of T1 (§2.8.3 rule 4). The two spellings must
// agree; this is the differential control that makes T1 a cross-module claim
// rather than an `out using var` claim.
// ===========================================================================
BEZEL_TEST(xmod_method_out_using_var_3188,
           t1b_same_module_out_using_recipient_22_4) {
    int rc = runSingleModule3188(sameModuleProgram3188(kWorkBody3188, "T1b"),
                                 "t1b");
    BEZEL_EXPECT_EQ(rc, 211);
}

// ===========================================================================
// T2 — §11.3 / §12.1: the recipient has a REAL type, proven by asking it for a
// member that does not exist.
//
// T1's 211 could in principle be reached by a recipient typed only well enough
// to lay out a load. This is the sharp row: pre-fix `made` had NO type at all,
// so `made.Nope` — declared nowhere — compiled CLEAN across the boundary while
// the twin said `no member Nope on type Res`. A MISSING diagnostic cannot be
// caught by an exit code, so it needs its own row and an assertion on the text.
// ===========================================================================
BEZEL_TEST(xmod_method_out_using_var_3188,
           t2_imported_out_using_recipient_has_a_real_type_11_3) {
    std::string err;
    int rc = runImported3188(
        "  public static method Main(): int "
        "    var ok := Fact.Make(out using var made); "
        "    if not ok then "
        "      return 0; "
        "    end if; "
        "    return made.Nope; "
        "  end Main;",
        "t2", &err);
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, -1);  // must NOT build
    BEZEL_EXPECT_TRUE(mentions3188(err, "no member `Nope` on type `Res`"));
}

// ===========================================================================
// T2b — the SAME-MODULE twin of T2.
// ===========================================================================
BEZEL_TEST(xmod_method_out_using_var_3188,
           t2b_same_module_out_using_recipient_type_11_3) {
    std::string err;
    int rc = runSingleModule3188(
        sameModuleProgram3188(
            "  public static method Main(): int "
            "    var ok := Fact.Make(out using var made); "
            "    if not ok then "
            "      return 0; "
            "    end if; "
            "    return made.Nope; "
            "  end Main;",
            "T2b"),
        "t2b", &err);
    BEZEL_EXPECT_EQ(rc, -1);
    BEZEL_EXPECT_TRUE(mentions3188(err, "no member `Nope` on type `Res`"));
}

// ===========================================================================
// T3 — §28.3 / the LEAK row, under generated-code ASan/LSan.
//
// The issue's second measured symptom was a 16-byte LeakSanitizer report per
// call, allocated in the DEPENDENCY (`bz$…Fact$Make`) and never freed because
// the untyped recipient carried no disposal. LSan forces the process exit
// status to 1 on a leak, so the assertion is that the program still reports
// 211 with the sanitizer linked in — which pins BOTH directions at once: a
// residual leak reads 1, and a double free (the way a disposal fix goes wrong)
// aborts under ASan rather than reaching any exit code at all.
// ===========================================================================
BEZEL_TEST(xmod_method_out_using_var_3188,
           t3_imported_out_using_recipient_no_leak_no_double_free_28_3) {
    int rc = runImported3188(kWorkBody3188, "t3", /*capturedStderr=*/nullptr,
                             /*asan=*/true);
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 211);
}

// ===========================================================================
// T3b — the SAME-MODULE twin of T3, also under ASan/LSan.
// ===========================================================================
BEZEL_TEST(xmod_method_out_using_var_3188,
           t3b_same_module_out_using_recipient_no_leak_28_3) {
    int rc = runSingleModule3188(sameModuleProgram3188(kWorkBody3188, "T3b"),
                                 "t3b", /*capturedStderr=*/nullptr,
                                 /*asan=*/true);
    BEZEL_EXPECT_EQ(rc, 211);
}

// ===========================================================================
// T4 — OVER-WIDENING CONTROL (§28.11 / stdlib §2.16): a BUILTIN `out using var`
// callee still reaches the receiver-generic-argument derivation.
//
// `OwningStack<T>.TryPop` has no `ast::MethodDecl` AND no `sema::MethodSymbol`,
// so `formalOutParamType2` must keep answering null for it and the ~120 lines
// of builtin derivation below the declaration lookup must keep running. This is
// the row that fails if the new source is too broad — the failure mode of a
// unification fix is a WIDER lookup, not a rejection, and no reject-direction
// row would notice.
//
// Same 211 accounting: `it!.v + 20` is 21 and the popped element is disposed
// once at the `out using var`'s scope end.
// ===========================================================================
BEZEL_TEST(xmod_method_out_using_var_3188,
           t4_builtin_out_using_recipient_still_derived_28_11) {
    int rc = runSingleModule3188(
        sameModuleProgram3188(
            "  static method Work(): int "
            "    using var st := new OwningStack<Res>(); "
            "    st.Push(new Res()); "
            "    if st.TryPop(out using var it) then "
            "      return it!.v + 20; "
            "    end if; "
            "    return 0; "
            "  end Work; "
            "  public static method Main(): int "
            "    var q := C.Work(); "
            "    return q * 10 + Res.GetDisposed(); "
            "  end Main;",
            "T4"),
        "t4");
    BEZEL_EXPECT_EQ(rc, 211);
}

}  // namespace bezel::module
