// fix #2294 / §11.8 / §11.2 / §2.8.2: a class implementing an **imported**
// interface's `static` members was ALWAYS rejected with a false "different
// signature" diagnostic, making §11.8 conformance unusable across a module
// boundary.
//
// Root cause: `Resolver::checkStaticInterfaceConformance`
// (src/sema/TypeResolution_Checks.cpp) read the INTERFACE member's signature
// exclusively from its AST declaration. An imported member has no syntactic AST
// at all (ModuleLoader::convertMember) — its resolved return type rides
// SemaContext's imported side table and its parameters carry
// `ParameterSymbol::resolvedDeclType` instead of `declType`. So for every
// imported static-interface member the check saw a null return node (which it
// conflated with UNIT-returning) and null parameter types, and no correct
// witness could ever match.
//
// Which tests FAIL without the fix (verified by reverting the fix and
// re-running this filter: 3 passed, 2 failed):
//   * `imported_value_return_member_conforms` and
//     `imported_parameterized_member_conforms` FAIL — the importer's compile is
//     REJECTED outright (-1, never the program's value). They are the two
//     regression tests proper: the first pins the return half of the fix (a
//     null AST return node was read as "the interface member returns unit"),
//     the second the parameter half (`resolvedOf(ip->declType)` was null).
//   * `imported_unit_return_member_conforms` PASSES pre-fix and is a CONTROL,
//     not a regression test: a genuinely unit-returning imported member
//     accidentally matched before, because null-return-node and unit-return
//     coincided. It pins that the fix — which now derives "declares a return"
//     from the imported TYPE — keeps unit members matching unit witnesses
//     rather than flipping the classification the other way.
//   * the two REJECT tests PASS pre-fix (everything was rejected then) and are
//     the no-false-accept guard: the fix widens where the signature is READ
//     FROM, and must not widen WHAT MATCHES.
//
// These need the FULL compile + link + run pipeline across a real `--import`
// boundary (the bug is in cross-module symbol reconstruction), so they use
// compileObject + compileFile-with-deps, exactly as xmod_static_init_798 does.

#include <cstdio>
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

// Per-process unique working dir under the shared scratch base. The previous
// fs::temp_directory_path() (== /tmp tmpfs) + non-atomic counter++ stem was
// race-prone: parallel shards overwrote each other's _a.bzl / _a.o / _b.bzl
// mid-compileObject and garbled the in-flight child read. The atomic
// unique_stem + /var/tmp/bezel-scratch parent fixes that class of flake
// (#3002-class COVERAGE SHORT). Note: an earlier partial pass had already
// added `#include "support/unique_stem.hpp"` and concatenated a no-op
// unique_stem("") onto the end of the counter-derived stem — but the leading
// counter++-derived prefix was the actual collision hazard. This commit
// drops the counter entirely and uses unique_stem as the sole source.
const std::filesystem::path& wtDir2294() {
    return wtScratchDir("wt-2294");
}

struct TempFile2294 {
    std::string path;
    explicit TempFile2294(std::string p) : path(std::move(p)) {}
    ~TempFile2294() { std::remove(path.c_str()); }
};

void write2294(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Compile `libSrc` to an object, then compile+run `appSrc` against it.
// Returns the program's exit code, -1 on importer compile/link/run failure,
// or -2 when the dependency itself could not be produced (toolchain absent).
int runImported2294(const std::string& libSrc, const std::string& appSrc) {
    auto dir = wtDir2294();
    std::string stem = unique_stem("bz2294");
    TempFile2294 aSrc((dir / (stem + "_a.bzl")).string());
    TempFile2294 aObj((dir / (stem + "_a.o")).string());
    write2294(aSrc.path, libSrc);
    {
        bezel::Driver driver;
        if (!driver.compileObject(aSrc.path, aObj.path).success) return -2;
    }
    if (!std::ifstream(aObj.path).good()) return -2;

    TempFile2294 bSrc((dir / (stem + "_b.bzl")).string());
    TempFile2294 bExe((dir / stem).string());
    write2294(bSrc.path, appSrc);
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj.path;
    d.objectPath = aObj.path;
    deps.push_back(d);
    if (!driver.compileFile(bSrc.path, bExe.path, deps).success) return -1;
    int status = std::system(bExe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// True when the IMPORTER's compile fails (the dependency must still build).
// Returns false-with-skip semantics via the -2 sentinel from runImported2294's
// dependency stage, which callers check.
int importerCompileStatus2294(const std::string& libSrc,
                              const std::string& appSrc) {
    return runImported2294(libSrc, appSrc);
}

// One dependency module reused by every case: a value-returning static member,
// a parameterized one, and a UNIT-returning one (the shape the pre-fix code
// conflated every imported member with).
const char* kLib =
    "module SiLib2294; "
    "public interface ISeed "
    "  static method Seed(): int; "
    "end ISeed; "
    "public interface IPing "
    "  static method Ping(n: int): int; "
    "end IPing; "
    "public interface IVoid "
    "  static method Touch(); "
    "end IVoid; ";
}  // namespace

// §11.8 (the fix): a witness for an imported static-interface member with a
// VALUE return is accepted, and `MyThing.Seed()` dispatches to it — 7.
// Pre-fix: the importer's compile was rejected ("different signature") because
// the null AST return node was read as "interface returns unit" while the
// witness returns `int`. -> -1, never 7.
BEZEL_TEST(xmod_static_interface_2294, imported_value_return_member_conforms) {
    int r = runImported2294(
        kLib,
        "module SiApp; "
        "import SiLib2294; "
        "public class MyThing: ISeed "
        "  public static method Seed(): int => 7; "
        "end MyThing; "
        "public class C public static method Main(): int "
        "  => MyThing.Seed(); "
        "end C;");
    if (r == -2) return;  // dependency toolchain unavailable — skip
    BEZEL_EXPECT_EQ(r, 7);
}

// §11.8 (the fix): a witness for an imported static-interface member with a
// PARAMETER is accepted and dispatches — Ping(21) = 42.
// Pre-fix: `resolvedOf(ip->declType)` was null for the imported parameter (its
// type rides `resolvedDeclType`), so the param loop bailed and the witness was
// rejected. -> -1, never 42.
BEZEL_TEST(xmod_static_interface_2294, imported_parameterized_member_conforms) {
    int r = runImported2294(
        kLib,
        "module SiApp; "
        "import SiLib2294; "
        "public class Doubler: IPing "
        "  public static method Ping(n: int): int => n * 2; "
        "end Doubler; "
        "public class C public static method Main(): int "
        "  => Doubler.Ping(21); "
        "end C;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 42);
}

// §11.8 CONTROL (passes pre-fix as well): a genuinely UNIT-returning imported
// static-interface member is matched by a unit-returning witness, and the
// witness runs — `_hit` = 5. This is the one case the pre-fix code got right,
// for the wrong reason: a null AST return node and a unit return coincided.
// The fix derives "declares a return" from the imported TYPE instead, so this
// pins that unit members still match unit witnesses — i.e. that the fix did not
// flip the classification the other way and start rejecting them.
BEZEL_TEST(xmod_static_interface_2294, imported_unit_return_member_conforms) {
    int r = runImported2294(
        kLib,
        "module SiApp; "
        "import SiLib2294; "
        "public class Toucher: IVoid "
        "  public static _hit: int := 0; "
        "  public static method Touch() "
        "    Toucher._hit := 5; "
        "  end Touch; "
        "end Toucher; "
        "public class C public static method Main(): int "
        "  Toucher.Touch(); "
        "  return Toucher._hit; "
        "end Main; end C;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, 5);
}

// §11.2 / §11.8 (no false ACCEPT): the witness returns `string` where the
// imported interface declares `int`. The fix widens where the interface
// signature is READ FROM, never what MATCHES — this must still be rejected.
BEZEL_TEST(xmod_static_interface_2294,
           imported_member_return_type_mismatch_still_rejected) {
    int r = importerCompileStatus2294(
        kLib,
        "module SiApp; "
        "import SiLib2294; "
        "public class Bad: ISeed "
        "  public static method Seed(): string => \"x\"; "
        "end Bad; "
        "public class C public static method Main(): int => 0; end C;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, -1);  // importer compile rejected
}

// §11.2 / §11.8 (no false ACCEPT): the witness takes `string` where the
// imported interface declares `int`. Guards the parameter half of the fix.
BEZEL_TEST(xmod_static_interface_2294,
           imported_member_param_type_mismatch_still_rejected) {
    int r = importerCompileStatus2294(
        kLib,
        "module SiApp; "
        "import SiLib2294; "
        "public class Bad: IPing "
        "  public static method Ping(n: string): int => 1; "
        "end Bad; "
        "public class C public static method Main(): int => 0; end C;");
    if (r == -2) return;
    BEZEL_EXPECT_EQ(r, -1);  // importer compile rejected
}
