// Cross-module exception propagation — fix #492.
//
// §31.1 / §31.2 / §2.6: a `throw` in a separately-compiled library module must
// propagate to the importing module's exception machinery. Before the fix, each
// separately-compiled module defined its OWN `bz$exc` global (BSS, InternalLinkage)
// so the library's throw wrote the library's copy while the importer's checks read
// a still-null copy — the exception silently disappeared and the call returned 0.
//
// The fix makes `bz$exc` a single shared symbol defined in the runtime
// (bezel_runtime.cpp) and referenced via an external declaration in every module's
// codegen, so all modules read/write the same global.
//
// Tests (spec-derived, per-milestone discipline):
//   (a) cross-module throw UNCAUGHT → exit 70 (runtime's bz_unhandled_exception)
//   (b) cross-module throw CAUGHT by importer's try/catch → handler runs (exit N)
//   (c) single-module throw UNCAUGHT → exit 70 (control: must still work)
//   (d) single-module throw CAUGHT → handler runs (control)
//   (e) cross-module throw of a user-defined exception type, CAUGHT, with a field
//       value that is readable in the catch arm

#include "driver/Driver.hpp"
#include "test_harness.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <vector>

namespace bezel::module {
namespace {

namespace fs = std::filesystem;

// The runtime's abnormal-termination exit code (EX_SOFTWARE, BSD sysexits.h).
// §33.3 / §22.13: uncaught exception escaping Main → bz_unhandled_exception() →
// exit(70).
constexpr int kUnhandledExit = 70;

struct TempFile {
    std::string path;
    explicit TempFile(const std::string& p) : path(p) {}
    ~TempFile() { std::remove(path.c_str()); }
};

void writeSource(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Compile lib to an object, then compile main importing lib → executable, run
// and return the exit code.  Returns -1 on any compile/link/run failure.
int buildAndRun(const std::string& libSrc, const std::string& mainSrc,
                const std::string& tag) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_exc492_" + tag + "_" + std::to_string(std::rand());
    TempFile lSrc((dir / (stem + "_lib.bzl")).string());
    TempFile lObj((dir / (stem + "_lib.o")).string());
    TempFile mSrc((dir / (stem + "_main.bzl")).string());
    TempFile mExe((dir / stem).string());

    writeSource(lSrc.path, libSrc);
    writeSource(mSrc.path, mainSrc);

    bezel::Driver driver;
    bezel::CompileResult rLib = driver.compileObject(lSrc.path, lObj.path);
    if (!rLib.success) return -1;

    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = lObj.path;
    d.objectPath   = lObj.path;
    deps.push_back(d);

    bezel::CompileResult rMain = driver.compileFile(mSrc.path, mExe.path, deps);
    if (!rMain.success) return -1;

    int status = std::system(mExe.path.c_str());
    if (status == -1 || !WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

// Single-module build for the control tests.
int buildAndRunSingle(const std::string& src, const std::string& tag) {
    auto dir = fs::temp_directory_path();
    std::string stem = "bezel_exc492_single_" + tag + "_" + std::to_string(std::rand());
    TempFile mSrc((dir / (stem + ".bzl")).string());
    TempFile mExe((dir / stem).string());

    writeSource(mSrc.path, src);

    bezel::Driver driver;
    bezel::CompileResult r = driver.compileFile(mSrc.path, mExe.path);
    if (!r.success) return -1;

    int status = std::system(mExe.path.c_str());
    if (status == -1 || !WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

}  // namespace

// ── (c) control: single-module throw UNCAUGHT → exit 70 ─────────────────────
// §22.13 / §31.1: a throw with no handler in Main must exit abnormally.
// This exercises the single-module path to confirm the fix does not regress it.
BEZEL_TEST(cross_module_exception, single_module_throw_uncaught_exits_70_spec_31_1) {
    int code = buildAndRunSingle(
        "module M; "
        "class Boom end Boom; "
        "class C "
        "  public static method Main(): int "
        "    throw new Boom(); "
        "  end Main; "
        "end C;",
        "single_uncaught");
    BEZEL_EXPECT_EQ(code, kUnhandledExit);
}

// ── (d) control: single-module throw CAUGHT → handler runs ──────────────────
// §31.2: a caught throw does not escape; Main completes normally with the
// handler's return value.
BEZEL_TEST(cross_module_exception, single_module_throw_caught_handler_runs_spec_31_2) {
    int code = buildAndRunSingle(
        "module M; "
        "class Boom end Boom; "
        "class C "
        "  public static method Main(): int "
        "    try "
        "      throw new Boom(); "
        "    catch Boom => return 55; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;",
        "single_caught");
    BEZEL_EXPECT_EQ(code, 55);
}

// ── (a) cross-module throw UNCAUGHT → exit 70 ───────────────────────────────
// §31.1 / §2.6 / fix #492: AlwaysRaise() throws in the library; Main calls it
// with no enclosing handler. Before the fix, bz$exc was per-module so the throw
// was a no-op and exit 100 was returned (x+100 where x==0). After the fix the
// exception propagates and bz_unhandled_exception terminates with exit 70.
BEZEL_TEST(cross_module_exception, cross_module_throw_uncaught_exits_70_spec_31_1) {
    int code = buildAndRun(
        // lib_throw.bzl
        "module LibThrow; "
        "public class Boom end Boom; "
        "public class Raiser "
        "  public static method AlwaysRaise(): int "
        "    throw new Boom(); "
        "  end AlwaysRaise; "
        "end Raiser;",
        // main.bzl
        "module Main; "
        "import LibThrow; "
        "class C "
        "  public static method Main(): int "
        "    var x := Raiser.AlwaysRaise(); "
        "    return x + 100; "     // must NOT execute; if it does → exit 100
        "  end Main; "
        "end C;",
        "uncaught");
    // Before fix: code == 100 (throw was a no-op).
    // After fix : code == kUnhandledExit (70).
    BEZEL_EXPECT_EQ(code, kUnhandledExit);
}

// ── (b) cross-module throw CAUGHT by importer's try/catch ───────────────────
// §31.2 / §2.6 / fix #492: AlwaysRaise() throws in the library; Main wraps the
// call in a try/catch and should observe the exception. Before the fix the catch
// arm never triggered (bz$exc was still null in Main's module) so the "no
// exception" path ran and return 99 was observed. After the fix the catch runs
// and return 77 is observed.
BEZEL_TEST(cross_module_exception, cross_module_throw_caught_in_importer_spec_31_2) {
    int code = buildAndRun(
        // lib.bzl
        "module LibCatch; "
        "public class AppErr end AppErr; "
        "public class Thrower "
        "  public static method Raise(): int "
        "    throw new AppErr(); "
        "  end Raise; "
        "end Thrower;",
        // main.bzl
        "module Main; "
        "import LibCatch; "
        "class C "
        "  public static method Main(): int "
        "    try "
        "      var dummy := Thrower.Raise(); "
        "      return 99; "       // must NOT execute
        "    catch AppErr => return 77; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;",
        "caught");
    // Before fix: code == 99 (catch never triggered).
    // After fix : code == 77.
    BEZEL_EXPECT_EQ(code, 77);
}

// ── (e) cross-module throw of user exception with a message field ─────────────
// §31.1 / §31.3 / fix #492: the thrown exception object carries a field value
// readable in the catch arm. This exercises that the in-flight pointer itself is
// correctly shared (not just a non-null sentinel).
BEZEL_TEST(cross_module_exception, cross_module_throw_with_field_readable_spec_31_3) {
    int code = buildAndRun(
        // lib.bzl: exception class carries a `code` field
        "module LibField; "
        "public class CodeErr "
        "  public code: int; "
        "  public constructor(c: int) code := c; end constructor; "
        "  destructor end destructor; "
        "end CodeErr; "
        "public class FieldThrower "
        "  public static method RaiseCode(n: int): int "
        "    throw new CodeErr(n); "
        "  end RaiseCode; "
        "end FieldThrower;",
        // main.bzl
        "module Main; "
        "import LibField; "
        "class C "
        "  public static method Main(): int "
        "    try "
        "      var dummy := FieldThrower.RaiseCode(33); "
        "      return 0; "        // must NOT execute
        "    catch var e: CodeErr => return e.code; "  // should return 33
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;",
        "field");
    // After fix: code == 33 (the field value from the cross-module exception object).
    BEZEL_EXPECT_EQ(code, 33);
}

// ── (f) fix #805: cross-module throw caught by a BUILT-IN exception type ───────
// §31.2 / §2.5: an exception thrown by an imported module and caught in the
// importer by a BUILT-IN exception type (`catch Exception`) is now matched. The
// catch lowers emitIsA against the built-in type descriptor `btd$Exception`;
// before this fix that descriptor was per-module (InternalLinkage), so the
// importer's `btd$Exception` address never equalled the address embedded in the
// imported exception object's vtable → the catch never fired (uncaught, exit 70).
// LinkOnceODR linkage merges the descriptor to one symbol so the pointer match
// succeeds. (The existing user-type-catch tests above already worked — user
// class descriptors are shared via the import mechanism; built-in descriptors
// were the gap.)
BEZEL_TEST(cross_module_exception, cross_module_throw_caught_by_builtin_Exception_805) {
    int code = buildAndRun(
        "module ELib; "
        "public class Thrower "
        "  public static method Go(): int => throw new Exception(\"x\"); "
        "end Thrower;",
        "module Main; "
        "import ELib; "
        "class C public static method Main(): int "
        "  try return Thrower.Go(); catch Exception => return 55; end; "
        "end Main; end C;",
        "builtin_base");
    BEZEL_EXPECT_EQ(code, 55);
}

// §31.2 / §12: a DERIVED built-in exception (ArgumentException) thrown
// cross-module is caught as its base `Exception` — the descriptor chain
// (btd$ArgumentException -> btd$Exception, all shared) is walked cross-module.
BEZEL_TEST(cross_module_exception, cross_module_derived_builtin_caught_as_base_805) {
    int code = buildAndRun(
        "module ELib; "
        "public class T "
        "  public static method ThrowArg(): int => "
        "    throw new ArgumentException(\"a\"); "
        "end T;",
        "module Main; "
        "import ELib; "
        "class C public static method Main(): int "
        "  try return T.ThrowArg(); catch Exception => return 11; end; "
        "end Main; end C;",
        "builtin_derived");
    BEZEL_EXPECT_EQ(code, 11);
}

// §31.2 (no false catch): a built-in exception thrown cross-module is NOT caught
// by a non-matching built-in catch type — it propagates uncaught (exit 70). Pins
// that the shared-descriptor change did not make isA over-match.
BEZEL_TEST(cross_module_exception, cross_module_builtin_non_matching_not_caught_805) {
    int code = buildAndRun(
        "module ELib; "
        "public class T "
        "  public static method ThrowInvalid(): int => "
        "    throw new InvalidOperationException(\"i\"); "
        "end T;",
        "module Main; "
        "import ELib; "
        "class C public static method Main(): int "
        "  try return T.ThrowInvalid(); catch ArgumentException => return 33; end; "
        "end Main; end C;",
        "builtin_nonmatch");
    BEZEL_EXPECT_EQ(code, kUnhandledExit);
}

}  // namespace bezel::module
