// Spec-derived regression test for issue #5025 — `Module.<Name>` where `<Name>`
// is a PRIVATE (or otherwise unexported) type of the imported module used to
// silently fall through `ExprTC::resolveMemberAccess` Cases (b)/(c) (src/sema/
// ExprTypecheck_Calls.cpp, lines ~19578–19665). The expression was never typed
// (`exprType` stayed null), codegen's `lowerCallExpr` returned nullptr, and the
// user saw the #2491 backstop warning with a runtime zero — exit 0 with a wrong
// value, indistinguishable from a correct program.
//
// §13.2 (top-level types default to private; `public` is the only declaration
// that exposes a type across a module boundary) / §2.7 (a module prefix names
// exactly that module's exported types — never its privates, never names it
// does not declare) / §2.8.3 rule 4 (an imported artifact carries only its
// public surface).
//
// Pre-fix `importedTypesOf("Mod")` is correctly pre-filtered to public types
// (ModuleMetadataBuilder.cpp:2747, MEASURED: `md.types` contains only exported
// types). The bug is purely in the resolveMemberAccess fall-through path, where
// Case (b) and Case (c) returned without typing `ma` whenever the iteration
// over exported types did not find `ma.member`. The post-fix diagnostic at the
// fall-through is `Severity::Error`, wording reuses the same neutral form
// `TypeResolution_Names.cpp` already emits for the same rule in type position:
// "module `M` has no public type `X` (§2.2 / §13.2)".
//
// ── THE DEFECT (measured on 6bf1ed38 with `bezelc` HEAD-1) ───────────────────
//
//   Lib5025 (private type `Hidden5025`):
//     module Lib5025;
//     private class Hidden5025 public method M(): int => 1; end Hidden5025;
//     public class PublicAPI5025 public method Run(): int => 99; end PublicAPI5025;
//   end Lib5025;
//
//   App:
//     module App5025; import Lib5025;
//     public class C5025
//       public static method Main(): int
//         return Lib5025.Hidden5025.M();   // <-- the illegal access
//       end Main;
//     end C5025;
//
//   $ bezelc -c Lib5025.bzl -o Lib5025.o
//   $ bezelc --import Lib5025.o -o App App5025.bzl
//   $ ./App ; echo $?
//   0                       ← SILENT WRONG: the program ran; `Lib5025.Hidden5025.M()`
//                            ←   never resolved, `lowerCallExpr` returned null,
//                            ←   codegen emitted the #2491 warning, runtime
//                            ←   zero. Without an explicit print, exit 0 reads
//                            ←   as success.
//
// ── TEST MATRIX (5 rows, real `compileObject` + `compileFile` w/ Dependency) ──
//
//   T1  REJECT — `Lib5025.Hidden5025.M()`              (Hidden5025 is private)
//   T2  REJECT — `Lib5025.Helper5025.M()`              (Helper5025 is private,
//                                                      mixed module that also
//                                                      has PublicAPI5025)
//   T3  REJECT — `Lib5025.TaskPrivate5025.M()`         (TaskPrivate5025 is
//                                                      private; module also
//                                                      has public Task5025)
//   T4  ACCEPT + VALUE = 7 — `Lib5025.Task5025.Go()`   (Task5025 is public;
//                                                      method returns 7)
//   T5  ACCEPT + VALUE = 99 — `Lib5025.PublicAPI5025.Run()`
//                                                     (PublicAPI5025 is
//                                                      public; module also
//                                                      has private
//                                                      Helper5025 and
//                                                      TaskPrivate5025 —
//                                                      the mix scenario)
//
// T1–T3 are the FAIL-WITHOUT hinge: pre-fix the import compiles and runs (exit
// 0 with a wrong value); post-fix the import is a clean sema reject with the
// §13.2 wording above. T4–T5 are the accept controls — the SAME fix must not
// over-reject on a public type.

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "driver/Driver.hpp"
#include "test_harness.hpp"
#include "support/unique_stem.hpp"


namespace bezel::module {
namespace {

struct TempFile5025 {
    std::string path;
    explicit TempFile5025(std::string p) : path(std::move(p)) {}
    ~TempFile5025() { std::remove(path.c_str()); }
};

void writeSource5025(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Compile a library source -> object. Returns true on success.
bool compileLibToObj5025(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

// Capture compile-stderr from a `driver.compileFile(bSrc, bExe, deps)` call
// into a string, so the test can assert on the diagnostic's wording without
// depending on the harness's stdout/stderr interleaving.
std::string captureImportStderr5025(
    const std::string& bSrc, const std::string& bExe, const std::string& aObj) {
    namespace fs = std::filesystem;
    std::string errPath =
        (fs::temp_directory_path() /
         ("bz5025_err_" + unique_stem("") + ".txt"))
            .string();
    std::fflush(stderr);
    std::cerr.flush();
    int saved = ::dup(STDERR_FILENO);
    int fd = ::open(errPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) ::dup2(fd, STDERR_FILENO);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj;
    d.objectPath = aObj;
    deps.push_back(d);
    driver.compileFile(bSrc, bExe, deps);
    std::fflush(stderr);
    std::cerr.flush();
    if (fd >= 0) {
        ::close(fd);
        ::dup2(saved, STDERR_FILENO);
    }
    ::close(saved);
    std::ifstream in(errPath);
    std::string out((std::istreambuf_iterator<char>(in)),
                    std::istreambuf_iterator<char>());
    std::remove(errPath.c_str());
    return out;
}

// TRUE when the import compile FAILS (the fail-closed gate fires). This is
// the fail-without/pass-with hinge: WITHOUT the #5025 fix the compile
// SUCCEEDS (and runs with the #2491 silent-zero); WITH the fix the compile is
// a clean sema reject.
bool compileImportFails5025(const std::string& bSrc, const std::string& bExe,
                            const std::string& aObj) {
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj;
    d.objectPath = aObj;
    deps.push_back(d);
    return !driver.compileFile(bSrc, bExe, deps).success;
}

// Compile importer + run, return exit code (-1 on any failure).
int compileImportAndRun5025(const std::string& bSrc, const std::string& bExe,
                            const std::string& aObj) {
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj;
    d.objectPath = aObj;
    deps.push_back(d);
    if (!driver.compileFile(bSrc, bExe, deps).success) return -1;
    int status = std::system(bExe.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// The single library used by all 5 rows. Three non-exported types
// (Hidden5025, Helper5025, TaskPrivate5025 — all declared `internal` per
// §13.2, the ONLY non-public visibility admitted at top level; see
// `Resolver::checkTopLevelVisibility`) and two public types (Task5025,
// PublicAPI5025), exercising:
//   * a public+internal mix (PublicAPI5025 + Helper5025),
//   * a same-prefix public/private pair (Task5025 + TaskPrivate5025 —
//     distinct declarations, different lexical names; the gate's no-match
//     arm must reject only the internal one).
//
// `importedTypesOf("Lib5025")` filters on `isExportedType`, which is true
// ONLY for `public` (ModuleMetadataBuilder.cpp:41-43), so the internal
// types are precisely the names that hit the resolveMemberAccess fall-through
// without ever appearing in `md.types` — the exact shape the bug was filed
// against.
const char* kLib5025 =
    "module Lib5025; "
    "internal class Hidden5025 "
    "  public method M(): int => 1; "
    "end Hidden5025; "
    "internal class Helper5025 "
    "  public method M(): int => 2; "
    "end Helper5025; "
    "internal class TaskPrivate5025 "
    "  public method M(): int => 8; "
    "end TaskPrivate5025; "
    // The accept rows (T4/T5) call `Lib.Type.Method()` through the type name,
    // which requires the method to be `static`. (An instance method accessed
    // through its type lowers to a zero call under codegen and is the wrong
    // shape for the accept oracle — the probe confirmed this on the pre-fix
    // compiler with #2491 and exit 0 instead of the expected 7/99.) The
    // private M()s above are NEVER reached (the type-level gate fires first).
    "public class Task5025 "
    "  public static method Go(): int => 7; "
    "end Task5025; "
    "public class PublicAPI5025 "
    "  public static method Run(): int => 99; "
    "end PublicAPI5025; ";

// Each importer body is a single Main that references the library exactly once,
// so the §13.2 diagnostic (when it fires) reports the user's exact spelling.
const char* kApp1Hidden =
    "module App5025_Hidden; import Lib5025; "
    "public class C5025 "
    "  public static method Main(): int "
    "    return Lib5025.Hidden5025.M(); "
    "  end Main; "
    "end C5025; ";

const char* kApp2Helper =
    "module App5025_Helper; import Lib5025; "
    "public class C5025 "
    "  public static method Main(): int "
    "    return Lib5025.Helper5025.M(); "
    "  end Main; "
    "end C5025; ";

const char* kApp3TaskPrivate =
    "module App5025_TaskPrivate; import Lib5025; "
    "public class C5025 "
    "  public static method Main(): int "
    "    return Lib5025.TaskPrivate5025.M(); "
    "  end Main; "
    "end C5025; ";

const char* kApp4TaskPublic =
    "module App5025_TaskPublic; import Lib5025; "
    "public class C5025 "
    "  public static method Main(): int "
    "    return Lib5025.Task5025.Go(); "
    "  end Main; "
    "end C5025; ";

const char* kApp5MixedPublic =
    "module App5025_Mixed; import Lib5025; "
    "public class C5025 "
    "  public static method Main(): int "
    "    return Lib5025.PublicAPI5025.Run(); "
    "  end Main; "
    "end C5025; ";

// The neutral diagnostic wording, copied verbatim from the spec-derived
// wording `TypeResolution_Names.cpp:3322` already emits for the same rule in
// type position — the brief's "REUSE existing binary wording" instruction.
const char* kDiagNoPublicTypeHidden =
    "module `Lib5025` has no public type `Hidden5025`";
const char* kDiagNoPublicTypeHelper =
    "module `Lib5025` has no public type `Helper5025`";
const char* kDiagNoPublicTypeTaskPrivate =
    "module `Lib5025` has no public type `TaskPrivate5025`";

}  // namespace

// ===========================================================================
// T1 — REJECT. `Lib5025.Hidden5025.M()` accesses a private type of Lib5025.
// Pre-fix: compiles, runs, returns the #2491 backstop's zero (exit 0). The
// fail-without hinge is the COMPILE failing cleanly post-fix (NOT running).
// ===========================================================================
BEZEL_TEST(imported_private_member_5025, hidden_rejected) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz5025_t1_");
    TempFile5025 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile5025 aObj((dir / (stem + "_a.o")).string());
    writeSource5025(aSrc.path, kLib5025);
    BEZEL_EXPECT_TRUE(compileLibToObj5025(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;
    TempFile5025 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile5025 bExe((dir / stem).string());
    writeSource5025(bSrc.path, kApp1Hidden);
    std::string err = captureImportStderr5025(bSrc.path, bExe.path, aObj.path);
    BEZEL_EXPECT_TRUE(compileImportFails5025(bSrc.path, bExe.path, aObj.path));
    BEZEL_EXPECT_TRUE(err.find(kDiagNoPublicTypeHidden) != std::string::npos);
}

// ===========================================================================
// T2 — REJECT. `Lib5025.Helper5025.M()` — the same shape as T1 but in a
// module that ALSO exports a public type (`PublicAPI5025`). Pins that the
// §13.2 gate is not over-firing on the public type or under-firing on the
// private one.
// ===========================================================================
BEZEL_TEST(imported_private_member_5025, helper_private_rejected) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz5025_t2_");
    TempFile5025 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile5025 aObj((dir / (stem + "_a.o")).string());
    writeSource5025(aSrc.path, kLib5025);
    BEZEL_EXPECT_TRUE(compileLibToObj5025(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;
    TempFile5025 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile5025 bExe((dir / stem).string());
    writeSource5025(bSrc.path, kApp2Helper);
    std::string err = captureImportStderr5025(bSrc.path, bExe.path, aObj.path);
    BEZEL_EXPECT_TRUE(compileImportFails5025(bSrc.path, bExe.path, aObj.path));
    BEZEL_EXPECT_TRUE(err.find(kDiagNoPublicTypeHelper) != std::string::npos);
}

// ===========================================================================
// T3 — REJECT. `Lib5025.TaskPrivate5025.M()` — a private class with a
// `Task`-style name (not the same lexical name as `Task5025`, but it shares
// the prefix and could trip a reader into thinking it is the same). The
// gate's no-match arm must reject `TaskPrivate5025` AND still accept
// `Task5025` (T4).
// ===========================================================================
BEZEL_TEST(imported_private_member_5025, task_private_rejected) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz5025_t3_");
    TempFile5025 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile5025 aObj((dir / (stem + "_a.o")).string());
    writeSource5025(aSrc.path, kLib5025);
    BEZEL_EXPECT_TRUE(compileLibToObj5025(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;
    TempFile5025 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile5025 bExe((dir / stem).string());
    writeSource5025(bSrc.path, kApp3TaskPrivate);
    std::string err = captureImportStderr5025(bSrc.path, bExe.path, aObj.path);
    BEZEL_EXPECT_TRUE(compileImportFails5025(bSrc.path, bExe.path, aObj.path));
    BEZEL_EXPECT_TRUE(err.find(kDiagNoPublicTypeTaskPrivate) != std::string::npos);
}

// ===========================================================================
// T4 — ACCEPT + VALUE row. `Lib5025.Task5025.Go()` returns 7. This is the
// negative control — the SAME library, the SAME `Module.<Name>` shape, just
// the public `Task5025`. Must compile and run; exit 7.
// ===========================================================================
BEZEL_TEST(imported_private_member_5025, task_public_accepted) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz5025_t4_");
    TempFile5025 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile5025 aObj((dir / (stem + "_a.o")).string());
    writeSource5025(aSrc.path, kLib5025);
    BEZEL_EXPECT_TRUE(compileLibToObj5025(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;
    TempFile5025 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile5025 bExe((dir / stem).string());
    writeSource5025(bSrc.path, kApp4TaskPublic);
    BEZEL_EXPECT_EQ(
        compileImportAndRun5025(bSrc.path, bExe.path, aObj.path), 7);
}

// ===========================================================================
// T5 — ACCEPT + VALUE row. `Lib5025.PublicAPI5025.Run()` returns 99. The
// "public type in a module that ALSO has private ones" mix scenario. Pins
// that the gate does not over-reject when the access is to a legitimate
// public type of a module that has private neighbours.
// ===========================================================================
BEZEL_TEST(imported_private_member_5025, mixed_module_public_accepted) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem("bz5025_t5_");
    TempFile5025 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile5025 aObj((dir / (stem + "_a.o")).string());
    writeSource5025(aSrc.path, kLib5025);
    BEZEL_EXPECT_TRUE(compileLibToObj5025(aSrc.path, aObj.path));
    if (!std::ifstream(aObj.path).good()) return;
    TempFile5025 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile5025 bExe((dir / stem).string());
    writeSource5025(bSrc.path, kApp5MixedPublic);
    BEZEL_EXPECT_EQ(
        compileImportAndRun5025(bSrc.path, bExe.path, aObj.path), 99);
}

}  // namespace bezel::module
