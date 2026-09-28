// fix-forward #4952 cross-module regression test: an IMPORTED GENERIC class
// `Box<T>` implements `IBox<T>` with a `Name: string` property; the app
// instantiates `Box<int>` from the library and accesses `.Name` through
// `IBox<int>` — dynamic dispatch through the itable.
//
// ROOT CAUSE (reviewer finding, a5 pool, R22 non-author review of add8ee2a).
// `buildImportedGenericVtable` (LlvmBackend_Setup.cpp ~7980) installs itable
// getter slots via a `fill` lambda that calls `ensurePropertyAccessorFn` to get
// the accessor Function* and installs it RAW:
//
//     itabElems[si] = llvm::cast<llvm::Constant>(acc);
//
// Every other vtable/itable fill site that can hold a string getter applies
// `ensureVirtualStringGetterOwnershipThunk` to `acc` before installing it
// (lines ~5328, ~5478, ~8662, ~8928, ~8952, ~9045, ~9048).
//
// Meanwhile `isPlusOneStringProducer`'s dynamic-slot arm classifies EVERY
// non-nullable string property read through a slot as +1 and WILL release it.
// Net: the thunk is missing but the consumer treats the result as +1,
// producing an over-release UAF (LSan: "fatal: refcount over-release (string)",
// exit 134).
//
// THE FIX applies the thunk at the `buildImportedGenericVtable` fill site,
// mirroring the local vtable/itable paths.
//
// per §28.3 "Unbound owned temporaries" (v0.307): an unbound owned temporary
//   must be disposed at its K8 boundary.
// per §28.9 "Runtime-managed types" (v0.307): `string` is runtime-managed; a
//   string-producing property getter through a dynamic slot must normalize to +1.
//
// ASAN ORACLE: `runCrossModuleChecked` (mirrors the #4260 cross-module pattern,
// tests/module/cross_module_import_test.cpp:585) compiles the library with
// `driver.compileObject` (plain) and the app with `opts.sanitizeAddress = true`
// before running. The app binary is then executed via `spawnCaptured` with
// `ASAN_OPTIONS=detect_leaks=1` so LSan catches the refcount over-release.
// Without the fix: exit 134 + "fatal: refcount over-release (string)" in
// sanitizer output.  With the fix: exit 0, ASan/LSan clean, no report.
//
// FAIL-WITHOUT (mechanical, see commit message):
//   step (a) fcf6657b only (src-only revert of thunk fix): exit 134, LSan
//     "fatal: refcount over-release (string)" in sanitizer output.
//   step (c) fcf6657b + b340e1bf (full fix): exit 0, no sanitizer report.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "support/driver_run_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace bezel::codegen {
namespace {

namespace aot_detail = bezel::test::aot_detail;

const std::filesystem::path& wtDir4952x() {
    return wtScratchDir("wt-4952x-codegen");
}

struct TempFile {
    std::string path;
    explicit TempFile(const std::string& p) : path(p) {}
    ~TempFile() { std::remove(path.c_str()); }
};

void writeSource(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Mirrors CrossModuleAsanResult / runCrossModuleChecked from
// tests/module/cross_module_import_test.cpp:577-629.
// Uses the same spawnCaptured path that makes LSan detect the refcount
// over-release (separate out/san files, detect_leaks=1).
struct CrossModuleAsanResult {
    bool builtOk = false;
    bool ran = false;
    bool sanitizerFired = false;
    int exitCode = -1;
    std::string sanitizerText;
    std::string stdoutText;
};

CrossModuleAsanResult runCrossModuleChecked(const std::string& libSrc,
                                           const std::string& appSrc) {
    CrossModuleAsanResult result;
    auto dir = wtDir4952x();
    std::string stem = unique_stem("bezel_4952x_");
    TempFile aSrc((dir / (stem + "_lib.bzl")).string());
    TempFile aObj((dir / (stem + "_lib.o")).string());
    TempFile bSrc((dir / (stem + "_app.bzl")).string());
    TempFile bExe((dir / stem).string());
    TempFile sanOut((dir / (stem + ".san.txt")).string());
    TempFile stdOut((dir / (stem + ".out.txt")).string());

    writeSource(aSrc.path, libSrc);
    writeSource(bSrc.path, appSrc);

    bezel::Driver driver;
    bezel::CompileResult ra = driver.compileObject(aSrc.path, aObj.path);
    if (!ra.success) return result;

    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);

    codegen::CodegenOptions opts;
    opts.sanitizeAddress = true;
    bezel::CompileResult rb =
        driver.compileFile(bSrc.path, bExe.path, deps, opts);
    if (!rb.success) return result;
    result.builtOk = true;

    bool timedOut = false;
    int status = aot_detail::spawnCaptured(
        {bExe.path}, stdOut.path, sanOut.path, aot_detail::aotTimeoutSec(),
        timedOut, {{"ASAN_OPTIONS", "detect_leaks=1:exitcode=99:abort_on_error=0"}});
    if (timedOut) return result;

    std::string sanText = aot_detail::slurp(sanOut.path);
    bool crashed = WIFSIGNALED(status);
    result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    result.ran = WIFEXITED(status);
    result.sanitizerFired = crashed || aot_detail::hasSanitizerReport(sanText);
    result.sanitizerText = std::move(sanText);
    result.stdoutText = aot_detail::slurp(stdOut.path);
    return result;
}

// --- Library source: generic Box<T> implementing IBox<T> with a Name property ---

// IBox<T> declares Name: string (non-nullable, producer body).
const char* kLibSrc =
    "module BoxLib; "
    "public interface IBox<T> "
    "  method Get(): T; "
    "  readonly property Name: string; "
    "end IBox; "
    "public class Box<T>: IBox<T> "
    "  private _t: T; "
    "  private _id: int; "
    "  public constructor(t: T, id: int) _t := t; _id := id; end constructor; "
    "  public method Get(): T => _t; "
    // Each read produces a FRESH owned string ("{_id}" — a distinct
    // interpolation result per Box instance, used as a sink discriminator).
    "  public property Name: string => $\"{_id}\"; "
    "end Box;";

// --- App source: new Box<int> from library, read .Name through IBox<int> itable ---

// _id = 42 for the Box<int>(42, 42) instance.
// 42 as string has Length 2.  200 iterations * 2 = 400.
const char* kAppSrc =
    "module App; "
    "import BoxLib; "
    "public class Program "
    "  public static method Main(): int "
    "    using var box := new BoxLib.Box<int>(42, 42); "
    "    var ib: BoxLib.IBox<int> := box; "
    "    var i: int := 0; "
    "    var sink: int := 0; "
    "    while i < 200 do "
    "      var s: string := ib.Name; "  // dynamic dispatch through itable
    "      sink := sink + s.Length; "     // borrow read — does not affect refcount
    "      i := i + 1; "
    "    end; "
    "    Console.WriteLine(sink); "
    "    return 0; "
    "  end Main; "
    "end Program;";

}  // namespace
}  // namespace bezel::codegen

// =============================================================================
// Cross-module regression: imported generic class `Box<T>` implementing
// `IBox<T>` with a string property, accessed through the interface itable.
// WITHOUT the fix: over-release UAF (LSan "fatal: refcount over-release (string)",
// exit 134).  WITH the fix: ASan/LSan clean, exit 0, prints 400.
// =============================================================================

BEZEL_TEST(imported_generic_interface_string_property_4952_crossmod,
           cross_module_box_t_ibox_t_name_via_itable_no_overrelease) {
    using namespace bezel::codegen;

    CrossModuleAsanResult r = runCrossModuleChecked(kLibSrc, kAppSrc);
    BEZEL_EXPECT_TRUE(r.builtOk);
    if (!r.builtOk) return;
    BEZEL_EXPECT_TRUE(r.ran);

    // Soundness oracle — the load-bearing half. Without the fix, the thunk
    // is missing from buildImportedGenericVtable's itable fill, so
    // isPlusOneStringProducer classifies the read as +1 and releases the
    // string — but the raw getter returned the string without retaining it,
    // so LSan catches the over-release: "fatal: refcount over-release (string)".
    // With the fix, no sanitizer report appears and exit is 0.
    BEZEL_EXPECT_TRUE(r.sanitizerText.find("refcount over-release") ==
                      std::string::npos);
    BEZEL_EXPECT_TRUE(r.sanitizerText.find("AddressSanitizer") ==
                      std::string::npos);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
