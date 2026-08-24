// Program entry-point scoping — §33.1/§2.4 (v0.181, issue #1563).
//
// §33.1: "Every Bezel executable has exactly one entry point: a `public
//   static method Main` declared on a class in the program's **main
//   module**. ... The **main module** is the module named as the build's
//   executable target (§2.4): the module for which the build requests an
//   executable artifact. Only classes declared in the main module are
//   searched for entry points: a qualifying signature in a dependency
//   module — including a co-member of the main module's import SCC (§2.5),
//   which is compiled *with* the main module but is not it — is an ordinary
//   method, never an entry-point candidate."
// §2.4: "An executable ... Its main module — the module named as the
//   executable target — must contain exactly one qualifying `Main` method
//   ... A library is a shared native library. It exposes its `public` types
//   and methods."
// §33.6: "A library module has no entry point; the compiler produces a
//   shared library artifact."
//
// Before the fix, codegen's Emitter::isMain() named ANY static `Main`
// method the native process-entry symbol `main`, regardless of whether the
// compilation unit being emitted was the executable's main module or a
// library artifact (`-c` / Driver::compileObject — the mechanism a
// dependency module is compiled through, §2.5, issue #328 inc 3). So a
// dependency module that happened to also declare a qualifying `Main`
// produced a `.o` that itself defined the native `main` symbol; linking it
// into an executable whose own main module also defines `main` failed with
// a linker "multiple definition of `main`" error. In effect, the
// entry-point search was not scoped to the main module at all — it was
// scoped to whatever object files ended up on the link line.
//
// These tests drive the real `bezel::Driver` end-to-end (compileObject for
// the dependency's library artifact, compileFile+Dependency for the
// importer's executable): parse, sema, codegen, native link, and process
// execution — the only way to observe both the link-time symptom and the
// runtime effect (which `Main` the OS actually invokes).

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>

#include "driver/Driver.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace {

namespace fs = std::filesystem;

// Per-process unique working dir, under the shared scratch base (never
// /tmp — see support/scratch_dir.hpp) and reaped at process exit.
static const std::filesystem::path& wtDir1563() {
    return wtScratchDir("wt-1563");
}

// Write `source` to a fresh temp .bzl path and return that path.
std::string writeSource(const std::string& source, const std::string& stem) {
    std::string path = (wtDir1563() / (stem + ".bzl")).string();
    std::ofstream out(path);
    out << source;
    return path;
}

// Run the native binary at `exe` and return its exit code, or -1 if it could
// not be run or did not exit normally.
int runAndGetExitCode(const std::string& exe) {
    int status = std::system(exe.c_str());
    if (status != -1 && WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

}  // namespace

// §33.6: "A library module has no entry point." Compiling a class with a
// fully qualifying `Main` (§33.2) through the library path
// (Driver::compileObject, i.e. `-c`) must succeed without any entry-point
// requirement — the same path a dependency module is compiled through
// before being handed to an importer (§2.5, issue #328 inc 3).
BEZEL_TEST(codegen_entry_point_main_module_1563,
           dependency_with_qualifying_main_compiles_as_library_33_6) {
    std::string src = writeSource(
        "module Dep1563A; "
        "class DepProgram public static method Main(): int => 99; "
        "end DepProgram;",
        "bezel_1563_dep_lib_" + unique_stem(""));
    std::string obj = src + ".o";

    bezel::Driver driver;
    bezel::CompileResult r = driver.compileObject(src, obj);
    BEZEL_EXPECT_TRUE(r.success);

    std::remove(src.c_str());
    std::remove(obj.c_str());
}

// §33.1/§2.4 (#1563): the executable's own main module's `Main` is THE
// entry point; a dependency module's qualifying `Main` — compiled as a
// library artifact and linked in via a Dependency (the `--import` machinery,
// §2.5) — is an ordinary method. It must not claim the native `main` symbol,
// must not conflict with the main module's `Main` at link time, and must
// never run. The dependency's Main returns 99; the main module's Main
// returns 5 — running the linked binary must observe exit code 5, proving
// the main module's Main (and only it) is the process entry point.
BEZEL_TEST(codegen_entry_point_main_module_1563,
           main_module_main_wins_over_dependency_main_33_1_2_4) {
    std::string stem = "bezel_1563_scope_" + unique_stem("");

    // The dependency module: its own qualifying `Main` (§33.2) — must become
    // an ordinary method once compiled as a library artifact (§33.6/§2.5).
    std::string depSrc = writeSource(
        "module Dep1563B; "
        "class DepProgram public static method Main(): int => 99; "
        "end DepProgram;",
        stem + "_dep");
    std::string depObj = depSrc + ".o";

    bezel::Driver driver;
    bezel::CompileResult depResult = driver.compileObject(depSrc, depObj);
    BEZEL_EXPECT_TRUE(depResult.success);

    // The main module: imports the dependency (unused beyond the `import`,
    // §2.2) and declares its own distinct qualifying `Main`.
    std::string appSrc = writeSource(
        "module App1563B; "
        "import Dep1563B; "
        "class Program public static method Main(): int => 5; "
        "end Program;",
        stem + "_app");
    std::string appExe = appSrc + ".exe";

    bezel::Dependency dep;
    dep.metadataPath = depObj;
    dep.objectPath = depObj;
    bezel::CompileResult appResult = driver.compileFile(appSrc, appExe, {dep});

    // Before #1563's fix, this link failed: both DepProgram.Main and
    // Program.Main were lowered to the native `main` symbol, so the linker
    // reported "multiple definition of `main`" and compileFile returned
    // success == false.
    BEZEL_EXPECT_TRUE(appResult.success);

    if (appResult.success) {
        // Only the main module's Main (returns 5) may run; the dependency's
        // Main (returns 99) is inert.
        BEZEL_EXPECT_EQ(runAndGetExitCode(appExe), 5);
    }

    std::remove(depSrc.c_str());
    std::remove(depObj.c_str());
    std::remove(appSrc.c_str());
    std::remove(appExe.c_str());
}
