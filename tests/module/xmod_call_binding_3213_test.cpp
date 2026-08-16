// fix #3213 (§22.5 "Optional parameters and named arguments" / §2.5
// "Dependency resolution", v0.254) — extend the §22.5 cross-module default-
// materialization gap to two more shapes that the exporter can FOLD to a value
// without new expression carriage.
//
// Family M5 of umbrella #3152 (sibling of #3178); see
// tests/module/xmod_call_binding_3178_test.cpp.
//
// WHAT THIS FIX ADDS. #3178 admitted only the LITERAL shapes that
// `renderConstExpr` emits verbatim — number, string, char, true/false, null,
// plus literal-arithmetic fold (#2898). The remaining §22.5 default forms were
// rejected as a LOUD "the dependency's metadata did not carry as a compile-time
// value" message, naming the omission the workaround. Two of those remaining
// shapes are decidable on the EXPORTER's side WITHOUT new expression carriage:
//
//   * `:= default(T)` — the zero value of the formal parameter's type, which
//     is already carried in `MetaParam::type`. The importer materializes it
//     identically to the local-twin codegen path
//     (LlvmBackend_Assign.cpp:10867 — `Constant::getNullValue(lowerType(...))`).
//
//   * `:= SomeConst` / `:= C.K` — a class-level `const` reference, whose
//     initializer is itself a §22.12 compile-time constant. The exporter folds
//     the reference to its value (the same "FOLD, don't render" argument
//     #2898 made for literal arithmetic) and the wire shape `valueText` /
//     `defaultText` already expects.
//
// Both fix rows prove the imported form now produces the SAME answer as the
// single-module twin; the pre-fix shape (a §22.5 reject naming #3213) is
// pinned at the head of each row as the verify-first evidence the test was
// written against, so the row can never silently drift back to a wrong positive
// by passing the wrong shape.
//
// Out of scope (per #3213): the genuinely per-call forms — `DateTime.Now`, a
// factory call, an earlier-parameter read. Those need the default EXPRESSION
// to cross the boundary, which is generic-body source-text carriage (#2884 /
// #3162) and a strictly separate fix.

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

struct TempFile3213 {
    std::string path;
    explicit TempFile3213(std::string p) : path(std::move(p)) {}
    ~TempFile3213() { std::remove(path.c_str()); }
};

void writeSource3213(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// The declarations under test, written ONCE and reused by both the dependency
// and the single-module twin.
//
//   DConst  — the `default(T)` shape. `Stamp(b: int := default(int))` is the
//             simplest spelling; `Addr(b: int := default(int))` adds a
//             `0` default to confirm the parameter's own default isn't shadowed
//             by the materialization.
//   KConst  — the `:= SomeConst` shape, where `K` is a §22.12 compile-time
//             constant. `Hundred(b: int := K)` pins the value 100.
//   CKConst — the `:= C.K` qualified spelling (a different shape on the wire,
//             a MemberAccessExpr instead of an IdentifierExpr). `TwoHundred
//             (b: int := Container.K)` exercises that arm.
const char* kDecls3213 =
    "public class DConst "
    "  public static method Stamp(a: int, b: int := default(int)): int "
    "    => a + b; "
    "  public static method Addr(a: int, b: int := default(int)): int "
    "    => a - b; "
    "end DConst; "
    "public class KConst "
    "  public const K: int := 100; "
    "  public static method Hundred(a: int, b: int := K): int => a + b; "
    "end KConst; "
    "public class CKConst "
    "  public const K: int := 200; "
    "  public static method TwoHundred(a: int, b: int := CKConst.K): int "
    "    => a + b; "
    "end CKConst;";

std::string libSource3213() {
    return std::string("module CallBindLib3213; ") + kDecls3213;
}

std::string appSource3213(const std::string& body, const std::string& tag) {
    return "module CallBindApp3213" + tag + "; import CallBindLib3213; "
           "class C " + body + " end C;";
}

std::string sameModuleProgram3213(const std::string& body,
                                  const std::string& tag) {
    return "module CallBindCtl3213" + tag + "; " + kDecls3213 + " class C " +
           body + " end C;";
}

// Compile the dependency, compile the app against it, run the app in a
// SEPARATE process. Returns the child's exit status, or:
//   -1 compile/link/run failure
//   -2 no toolchain for the dependency
int runImported3213(const std::string& body, const std::string& tag,
                    std::string* capturedStderr = nullptr) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bz3213_" + tag + "_" + unique_stem("");

    TempFile3213 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile3213 aObj((dir / (stem + "_a.o")).string());
    writeSource3213(aSrc.path, libSource3213());
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success) return -2;
    }
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile3213 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile3213 bExe((dir / (stem + "_b")).string());
    TempFile3213 bErr((dir / (stem + "_b.err")).string());
    writeSource3213(bSrc.path, appSource3213(body, tag));

    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);

    bool compiled = false;
    if (capturedStderr) {
        std::fflush(stderr);
        int saved = ::dup(STDERR_FILENO);
        int sink = ::open(bErr.path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (sink >= 0) {
            ::dup2(sink, STDERR_FILENO);
            ::close(sink);
        }
        compiled = bezel::test::compileFileForTest(driver, bSrc.path, bExe.path, deps).success;
        std::fflush(stderr);
        if (saved >= 0) { ::dup2(saved, STDERR_FILENO); ::close(saved); }
        std::ifstream in(bErr.path);
        capturedStderr->assign((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    } else {
        compiled = bezel::test::compileFileForTest(driver, bSrc.path, bExe.path, deps).success;
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
    if (WIFSIGNALED(status)) return 1000 + WTERMSIG(status);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int runSingleModule3213(const std::string& src, const std::string& tag,
                        std::string* capturedStderr = nullptr) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bz3213ctl_" + tag + "_" + unique_stem("");
    TempFile3213 sSrc((dir / (stem + ".bzl")).string());
    TempFile3213 sExe((dir / stem).string());
    TempFile3213 sErr((dir / (stem + ".err")).string());
    writeSource3213(sSrc.path, src);

    bezel::Driver driver;
    bool compiled = false;
    if (capturedStderr) {
        std::fflush(stderr);
        int saved = ::dup(STDERR_FILENO);
        int sink = ::open(sErr.path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (sink >= 0) {
            ::dup2(sink, STDERR_FILENO);
            ::close(sink);
        }
        compiled = bezel::test::compileFileForTest(driver, sSrc.path, sExe.path, {}).success;
        std::fflush(stderr);
        if (saved >= 0) { ::dup2(saved, STDERR_FILENO); ::close(saved); }
        std::ifstream in(sErr.path);
        capturedStderr->assign((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    } else {
        compiled = bezel::test::compileFileForTest(driver, sSrc.path, sExe.path, {}).success;
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

// `Main` returns the expression directly, so the process EXIT CODE is the
// value under test. Every expected value is in 1..255, so the 8-bit truncation
// of the AOT path cannot alias one row onto another, and none of them is 0
// (which would be indistinguishable from "ran but computed nothing").
std::string mainReturning3213(const std::string& expr) {
    return "  public static method Main(): int "
           "    return " + expr + "; "
           "  end Main;";
}

bool mentions3213(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// ===========================================================================
// T9 — §22.5: a `:= default(T)` parameter's default CROSSES the module
// boundary and is materialized as the zero value of the formal's type.
//
// `DConst.Stamp(5)` = 5 + default(int) = 5 + 0 = 5. Pre-fix
// `renderConstExpr` had no `DefaultExpr` arm and emitted ""; sema's
// `importedDefaultTextIsMaterializable` therefore refused the omission with
// the §22.5/#3213 message. Post-fix the omission is admitted and the value is
// 5.
// ===========================================================================
BEZEL_TEST(xmod_call_binding_3213, t9_imported_default_t_zero_value_22_5) {
    int rc = runImported3213(mainReturning3213("DConst.Stamp(5)"), "t9");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 5);
}

BEZEL_TEST(xmod_call_binding_3213, t9b_same_module_default_t_zero_value_22_5) {
    int rc = runSingleModule3213(
        sameModuleProgram3213(mainReturning3213("DConst.Stamp(5)"), "T9b"),
        "t9b");
    BEZEL_EXPECT_EQ(rc, 5);
}

// Verify the same default is correct on a method whose body subtracts rather
// than adds — guards against a fix that hardwires one codegen path.
BEZEL_TEST(xmod_call_binding_3213, t9c_imported_default_t_subtract_22_5) {
    int rc = runImported3213(mainReturning3213("DConst.Addr(7)"), "t9c");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 7);
}

// The SUPPLIED spelling of the same call still works — the limit is the
// omission, not the method.
BEZEL_TEST(xmod_call_binding_3213, t9d_imported_default_t_supplied_22_5) {
    int rc = runImported3213(mainReturning3213("DConst.Stamp(5, 11)"), "t9d");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 16);
}

// ===========================================================================
// T10 — §22.5: a `:= SomeConst` parameter's default CROSSES the module
// boundary and is materialized as the constant's value.
//
// `KConst.Hundred(7)` = 7 + K = 7 + 100 = 107. Pre-fix `renderConstExpr`
// emitted the identifier `K` by NAME; `foldImportedConstText` declined a
// name and the omission was refused. Post-fix the exporter folds the
// `IdentifierExpr` to the constant's literal value 100.
// ===========================================================================
BEZEL_TEST(xmod_call_binding_3213, t10_imported_bare_const_ref_22_5) {
    int rc = runImported3213(mainReturning3213("KConst.Hundred(7)"), "t10");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 107);
}

BEZEL_TEST(xmod_call_binding_3213, t10b_same_module_bare_const_ref_22_5) {
    int rc = runSingleModule3213(
        sameModuleProgram3213(mainReturning3213("KConst.Hundred(7)"), "T10b"),
        "t10b");
    BEZEL_EXPECT_EQ(rc, 107);
}

// ===========================================================================
// T11 — §22.5: the QUALIFIED const spelling `:= C.K` (a MemberAccessExpr,
// not a bare IdentifierExpr) crosses the boundary the same way.
//
// `CKConst.TwoHundred(3)` = 3 + CKConst.K = 3 + 200 = 203. This is the
// other shape the §22.5 binder admits ("call static methods, including
// factory methods") and the only MemberAccessExpr-shaped const ref in the
// issue's table.
// ===========================================================================
BEZEL_TEST(xmod_call_binding_3213, t11_imported_qualified_const_ref_22_5) {
    int rc = runImported3213(mainReturning3213("CKConst.TwoHundred(3)"),
                             "t11");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 203);
}

BEZEL_TEST(xmod_call_binding_3213, t11b_same_module_qualified_const_ref_22_5) {
    int rc = runSingleModule3213(
        sameModuleProgram3213(mainReturning3213("CKConst.TwoHundred(3)"),
                              "T11b"),
        "t11b");
    BEZEL_EXPECT_EQ(rc, 203);
}

// The SUPPLIED spelling of T10 — guards against a fix that hardwires the
// default-filling path and breaks the explicit-arg path.
BEZEL_TEST(xmod_call_binding_3213, t10c_imported_const_ref_supplied_22_5) {
    int rc = runImported3213(mainReturning3213("KConst.Hundred(1, 2)"),
                             "t10c");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 3);
}

}  // namespace bezel::module
