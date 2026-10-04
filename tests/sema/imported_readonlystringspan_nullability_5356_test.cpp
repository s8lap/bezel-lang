// Regression test for issue #5356 (sema, v0.315 / §3.3, fix #5356).
//
// stdlib §3.3 (v0.313): `ReadOnlyStringSpan` methods (IndexOf / Contains /
// StartsWith / EndsWith / SequenceEqual) declare NON-nullable `string` or
// `char` parameters. The sema layer's `checkBuiltinSpanCallArgs` gate
// (ExprTypecheck_Calls.cpp:17073) validates these in the same way the
// user-method path validates a nullable argument against a non-nullable
// formal: the §7 assignability check fires and emits the "is nullable but
// the target type is non-nullable (§7)" diagnostic.
//
// THE DEFECT — the hand-rolled shape predicate.
//
// `checkBuiltinSpanCallArgs` resolves the receiver's builtin name using a
// hand-rolled predicate that handles only TWO of the THREE TypeRef shapes
// that can denote a builtin Named type:
//
//   a) declaration-less with `astNode->name` — a LOCAL written occurrence
//   b) `declaration` = a synthetic builtin-class stub
//   c) declaration-less with `astNode == nullptr` and name in `importedName`
//      — an IMPORTED builtin.
//
// Shape (c) exists because `ModuleLoader::convert()` builds the TypeRef for
// a name read out of a dependency's `.bezel.meta`; there is no AST for
// another module's source, so `astNode` is null. The hand-rolled predicate
// requires `astNode` to be non-null, so shape (c) falls through: `spanName`
// stays empty, and the function returns early without performing the
// nullability check.
//
// This is the same third-shape gap that #5313 fixed for `builtinInstName`
// and four other sites. The fix here mirrors that solution: call
// `recvTy->headName()` directly, which already falls back to `importedName`,
// instead of checking `astNode` first.
//
// CONSEQUENCE: a `null`-typed argument to an imported `ReadOnlyStringSpan`
// method compiles silently instead of emitting the §7 diagnostic. The
// violation produces a downstream error rather than a wrong program, but the
// early, specific diagnostic is lost.
//
// FIX. After the fix, `spanName` is resolved correctly for shape (c), and
// the nullability check fires as it does for the local case.
//
// ── TEST ───────────────────────────────────────────────────────────────────
//
// Two-module AOT (Driver::compileObject for the library, then
// Driver::compileFile for the app, both forked). The strict AOT path
// exercises the same type-resolution and call-typechecking that the local
// case uses.
//
// REJECT row (T1): `Hub.GetSpan().IndexOf(null)` — the null argument should
// trigger the §7 nullability diagnostic. Without the fix, no diagnostic is
// emitted and the program compiles (incorrectly).
//
// ACCEPT row (T2): `Hub.GetSpan().IndexOf("x")` — a non-null argument
// should compile successfully (control: proves the fix doesn't break the
// valid path).

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"  // compileFileForTest
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace bezel::sema {
namespace {

namespace fs = std::filesystem;

// ── Scratch dir ──────────────────────────────────────────────────────────────

static const fs::path& wtDir5356() {
    return wtScratchDir("wt-5356");
}

// ── Temp file ───────────────────────────────────────────────────────────────

struct TempFile5356 {
    std::string path;
    explicit TempFile5356(const std::string& p) : path(p) {}
    ~TempFile5356() { std::remove(path.c_str()); }
};

void writeSource(const std::string& path, const std::string& src) {
    std::ofstream(path) << src;
}

// ── Two-module build ────────────────────────────────────────────────────────
//
// Returns {success, stderr}. success=true means compile succeeded.
// stderr contains the captured compiler output (diagnostics).

std::pair<bool, std::string> buildTwoModule(const std::string& libSrc,
                                             const std::string& appSrc,
                                             bool rejectExpected) {
    auto dir = wtDir5356();
    std::string stem = "bz5356_" + unique_stem("");
    TempFile5356 libSrcF((dir / (stem + "_lib.bzl")).string());
    TempFile5356 libObjF((dir / (stem + "_lib.o")).string());
    writeSource(libSrcF.path, libSrc);

    // Build library
    {
        bezel::Driver driver;
        if (!driver.compileObject(libSrcF.path, libObjF.path).success)
            return {false, "lib compile failed"};
    }
    if (!std::ifstream(libObjF.path).good())
        return {false, "lib object not created"};

    TempFile5356 appSrcF((dir / (stem + "_app.bzl")).string());
    TempFile5356 appExeF((dir / stem).string());
    TempFile5356 appErrF((dir / (stem + "_app.err")).string());
    writeSource(appSrcF.path, appSrc);

    // Build app with lib dependency
    {
        bezel::Driver driver;
        std::vector<bezel::Dependency> deps;
        bezel::Dependency d;
        d.metadataPath = libObjF.path;
        d.objectPath = libObjF.path;
        deps.push_back(d);
        codegen::CodegenOptions opts;

        std::fflush(stderr);
        int saved = ::dup(STDERR_FILENO);
        int sink = ::open(appErrF.path.c_str(),
                          O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (sink >= 0) { ::dup2(sink, STDERR_FILENO); ::close(sink); }

        bool ok = bezel::test::compileFileForTest(
                      driver, appSrcF.path, appExeF.path, deps, opts).success;

        std::fflush(stderr);
        if (saved >= 0) { ::dup2(saved, STDERR_FILENO); ::close(saved); }

        std::ifstream errIn(appErrF.path);
        std::ostringstream errSs;
        errSs << errIn.rdbuf();
        std::string errOut = errSs.str();

        if (rejectExpected) {
            // REJECT row: compile should fail; return {ok, errOut} so that
            // the caller can check !ok to verify the reject.
            return {ok, errOut};
        } else {
            // ACCEPT row: compile should succeed
            return {ok, errOut};
        }
    }
}

// ── Diagnostic helpers ─────────────────────────────────────────────────────

bool hasDiag(const std::string& stderr, const char* needle) {
    return stderr.find(needle) != std::string::npos;
}

// ── Modules ─────────────────────────────────────────────────────────────────

// Lib module: a public class with a static factory returning ReadOnlyStringSpan.
// This puts the return type in shape (c): an imported builtin TypeRef.
const std::string kLibSrc =
    "module Lib; "
    "public class Hub "
    "  public static method GetSpan(): ReadOnlyStringSpan "
    "    return \"hello\".AsSpan(); "
    "  end GetSpan; "
    "end Hub; ";

// App module: calls Hub.GetSpan().IndexOf with a null argument.
const std::string kRejectAppSrc =
    "module App; "
    "import Lib; "
    "class C "
    "  public static method Main(): int "
    "    var n: string? := null; "
    "    return Hub.GetSpan().IndexOf(n); "
    "  end Main; "
    "end C; ";

// App module: calls Hub.GetSpan().IndexOf with a non-null argument (control).
const std::string kAcceptAppSrc =
    "module App; "
    "import Lib; "
    "class C "
    "  public static method Main(): int "
    "    return Hub.GetSpan().IndexOf(\"x\"); "
    "  end Main; "
    "end C; ";

}  // namespace

// T1 (REJECT): IndexOf(null) on an IMPORTED ReadOnlyStringSpan should be
// rejected with the §7 nullability diagnostic. Without the fix, no diagnostic
// is emitted and the program compiles (incorrectly).
BEZEL_TEST(sema_imported_readonlystringspan_nullability_5356,
           reject_imported_indexof_null_argument) {
    auto [ok, err] = buildTwoModule(kLibSrc, kRejectAppSrc, true);
    // Must be rejected
    BEZEL_EXPECT_TRUE(!ok);
    // The specific §7 / §3.3 diagnostic must be present
    BEZEL_EXPECT_TRUE(hasDiag(err, "is nullable but the target type is non-nullable"));
    BEZEL_EXPECT_TRUE(hasDiag(err, "ReadOnlyStringSpan.IndexOf"));
    BEZEL_EXPECT_TRUE(hasDiag(err, "(§7)"));
}

// T2 (ACCEPT): IndexOf("x") on an IMPORTED ReadOnlyStringSpan should
// compile successfully (control: the fix does not break the valid path).
BEZEL_TEST(sema_imported_readonlystringspan_nullability_5356,
           accept_imported_indexof_non_null_argument) {
    auto [ok, err] = buildTwoModule(kLibSrc, kAcceptAppSrc, false);
    BEZEL_EXPECT_TRUE(ok);
}

}  // namespace bezel::sema
