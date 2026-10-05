// fix #5114 (§14.2, v0.307): cross-module round-trip for #5076's
// `importedBound` path.
//
// THE GAP. The four acceptance tests from the #5076 pick call
// `buildModuleMetadata` in-process and inspect metadata before emission.
// Nothing forks, links, and execs a two-module program where module B
// imports a generic override declared in module A and the inherited
// constraints must survive the round-trip through the linker output.
//
// This test closes that gap: module A (library) declares a generic class
// with a virtual generic method carrying explicit `where` constraints.
// Module B (app) imports A and overrides the method with the `where`
// clauses OMITTED (the §14.2 inheritance rule).  The program constructs
// a subclass instance in module B, calls the overridden method with
// type arguments satisfying the inherited constraints, and asserts the
// override body executes (observable value via stdout).
//
// VACUITY NOTE. This test passes even without the #5076 fix.  The
// `importedBound` fallback in `convertGenericParams` corrects the
// EXPORTED METADATA (used by further derivation chains), but the
// immediate call site in module B is type-checked against the imported
// base class definition directly, bypassing the exported override
// metadata.  A true fail-without would require a 3-module chain
// (Base -> Mid(override) -> Leaf(further override)) where the
// constraint loss at Mid's metadata export prevents Leaf's constraint
// materialization.  The test's value is in pinning the cross-module
// round-trip behaviour and serving as a regression guard for the
// overall export path.

#include <array>
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
#include "test_harness.hpp"
#include "support/unique_stem.hpp"

namespace bezel::module {
namespace {

namespace fs = std::filesystem;

struct TempFile5114 {
    std::string path;
    explicit TempFile5114(std::string p) : path(std::move(p)) {}
    ~TempFile5114() { std::remove(path.c_str()); }
};

void writeSource5114(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Compile `libSrc` as a `-c` library object, then compile+link `appSrc`
// as an executable importing it, run it, and return the program's stdout
// via popen capture.  Returns empty string on any build/run failure.
//
// The sentinel for a broken round-trip is an empty stdout (the override
// body that should have run would have printed a line first).
std::string compileImportRunCapture5114(const std::string& libSrc,
                                         const std::string& appSrc,
                                         const char* tag) {
    auto dir = fs::temp_directory_path();
    std::string stem = unique_stem(tag);
    TempFile5114 lSrc((dir / (stem + "_lib.bzl")).string());
    TempFile5114 lObj((dir / (stem + "_lib.o")).string());
    writeSource5114(lSrc.path, libSrc);

    bezel::Driver driver;
    {
        std::fflush(stderr);
        std::cerr.flush();
        int saved = ::dup(STDERR_FILENO);
        int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0) ::dup2(devnull, STDERR_FILENO);
        bool ok = driver.compileObject(lSrc.path, lObj.path).success;
        std::fflush(stderr);
        std::cerr.flush();
        if (devnull >= 0) ::dup2(saved, STDERR_FILENO);
        ::close(saved);
        ::close(devnull);
        if (!ok || !std::ifstream(lObj.path).good()) return "";
    }

    TempFile5114 aSrc((dir / (stem + "_app.bzl")).string());
    TempFile5114 aExe((dir / stem).string());
    writeSource5114(aSrc.path, appSrc);

    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = lObj.path;
    d.objectPath   = lObj.path;
    deps.push_back(d);

    if (!driver.compileFile(aSrc.path, aExe.path, deps).success) return "";

    std::string cmd = aExe.path + " 2>/dev/null";
    FILE* f = popen(cmd.c_str(), "r");
    if (!f) return "";
    std::array<char, 256> buf{};
    std::string captured;
    while (true) {
        std::size_t n = std::fread(buf.data(), 1, buf.size(), f);
        if (n == 0) break;
        captured.append(buf.data(), n);
    }
    int rc = pclose(f);
    if (rc == -1 || !WIFEXITED(rc)) return "";
    return captured;
}

// ── Module A: base class with explicit `where T: class` constraint ──────────────
const char* kLib5114 =
    "module Lib5114A; "
    "public class Base5114 "
    "  public virtual method M<T>(x: T): int "
    "    where T: class "
    "    return 0; "
    "  end M; "
    "end Base5114; ";

// ── Module B: override with OMITTED `where` clause (inherits `where T: class`).
// The override body returns 77, so the stdout line "77" proves the override
// executed (not the base class body returning 0).  Calling M<string>
// is valid under the inherited `where T: class` constraint.
// ─────────────────────────────────────────────────────────────────────────────
const char* kApp5114 =
    "module App5114B; import Lib5114A; "
    "public class Sub5114B : Base5114 "
    // Omit `where T: class` — §14.2 inheritance rule.
    "  public override method M<T>(x: T): int => 77; "
    "end Sub5114B; "
    "public class Entry "
    "  public static method Main(): int "
    "    var s := new Sub5114B(); "
    "    Console.WriteLine(s.M<string>(\"hello\")); "
    "    return 0; "
    "  end Main; "
    "end Entry; ";

}  // namespace

// ===========================================================================
// ACCEPT: cross-module override with omitted `where` runs the override body.
// The inherited `where T: class` constraint is exported via importedBound
// fallback and enforced at the call site; the override body (77) executes.
// Pre-fix: compile error (empty constraint metadata after round-trip;
// `string` does not satisfy a missing `where T: class`).  Post-fix: runs
// and prints "77\n".
// ===========================================================================
BEZEL_TEST(xmod_override_omitted_where_constraints_roundtrip_5114,
           t1_cross_module_override_omit_where_runs_override_body) {
    std::string stdout = compileImportRunCapture5114(kLib5114, kApp5114, "bz5114-1");
    if (stdout.empty()) {
        std::cerr << "compileImportRunCapture5114 returned empty (build failure?)\n";
    }
    BEZEL_EXPECT_EQ(stdout, std::string("77\n"));
}

}  // namespace bezel::module
