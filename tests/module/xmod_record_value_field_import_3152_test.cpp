// Regression test for the imported-symbol classification seam (umbrella
// #3152), TypeResolution_Checks.cpp's `recordValueContainsSelf` (§9,
// "a record that transitively contains itself by value has infinite size").
//
// `recordValueContainsSelf` walks a record's value-typed fields looking for a
// cycle back to the record it started from. The field-type extraction used
// to be an inline `if (fs->posParam) ... else if (auto* fd =
// dynamic_cast<const ast::FieldDecl*>(fs->decl)) ...` — fix #3650 collapsed
// it onto the shared `sema::fieldDeclTypeOf` accessor
// (src/sema/FieldSymbolAccessors.hpp). `fieldDeclTypeOf` returns null for an
// IMPORTED field (an imported FieldSymbol's `decl` is null per #3149 — there
// is no `ast::FieldDecl` to reconstruct for an import), so a local record
// with a value-typed field of an IMPORTED record type stops the walk at that
// field without exploring the imported type's own fields.
//
// THIS IS SOUND, NOT A GAP, TODAY: Driver's `compileObject`/`compileFile`
// dependency model is acyclic-only (src/driver/Driver.hpp, `compileObject`'s
// doc: "`deps` lets a dependency itself import further dependencies
// (acyclic)"), so no path leaving into an imported type can currently lead
// back to the local record being checked, and any self-cycle wholly inside
// the dependency was already rejected when THAT module was compiled (the
// identical check, run there with every symbol local). This is a DRIVER
// LIMITATION, not a language guarantee — §2.8.1 rule 4 (v0.286) states
// plainly that "import cycles between distinct modules remain legal and
// compile as an SCC" (§2.5), so `recordValueContainsSelf` MUST be revisited
// (and this test's coverage re-examined for an SCC-cycle case) once cyclic-
// import compilation lands. This test exists because the file had NO
// existing cross-module coverage for this specific accessor call site — it
// pins the POSITIVE case (a local record embedding an imported VALUE record
// as a field compiles cleanly, no false reject) and its local-declaration
// twin, so a future change that makes the walk over-eager (treating "cannot
// see past the import" as "assume a cycle") is caught immediately.
//
// §9 (record value semantics) / §2.8.1 rule 4 (import cycles / SCC
// compilation, v0.286) / §2.8.3 rule 4 (imported artifacts behave like
// local ones) / §9.1 (positional vs. body field), v0.286.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {
namespace fs = std::filesystem;

const std::filesystem::path& wtDir3152() { return wtScratchDir("wt-3152rv"); }

struct TempFile3152 {
    std::string path;
    explicit TempFile3152(std::string p) : path(std::move(p)) {}
    ~TempFile3152() { std::remove(path.c_str()); }
};

void write3152(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Compile lib -> object, compile main importing lib -> executable, run.
// Returns the exit code, or -1 when either compile or the link fails.
int runXmod3152(const std::string& libSrc, const std::string& mainSrc) {
    auto dir = wtDir3152();
    std::string stem = unique_stem("bezel_xmod3152");
    TempFile3152 lSrc((dir / (stem + "_lib.bzl")).string());
    TempFile3152 lObj((dir / (stem + "_lib.o")).string());
    TempFile3152 mSrc((dir / (stem + "_main.bzl")).string());
    TempFile3152 mExe((dir / (stem + "_main")).string());

    write3152(lSrc.path, libSrc);
    write3152(mSrc.path, mainSrc);

    bezel::Driver driver;
    if (!driver.compileObject(lSrc.path, lObj.path).success) return -1;

    bezel::Dependency dep;
    dep.metadataPath = lObj.path;
    dep.objectPath   = lObj.path;
    std::vector<bezel::Dependency> deps{dep};

    if (!driver.compileFile(mSrc.path, mExe.path, deps).success) return -1;

    int status = std::system(mExe.path.c_str());
    if (status == -1 || !WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

// The single-module CONTROL: the same declarations and body in one file.
int runLocal3152(const std::string& src) {
    auto dir = wtDir3152();
    std::string stem = unique_stem("bezel_loc3152");
    TempFile3152 mSrc((dir / (stem + ".bzl")).string());
    TempFile3152 mExe((dir / stem).string());
    write3152(mSrc.path, src);

    bezel::Driver driver;
    if (!driver.compileFile(mSrc.path, mExe.path, {}).success) return -1;
    int status = std::system(mExe.path.c_str());
    if (status == -1 || !WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

// `Boxed3152` is a plain §9 VALUE record (no cycle) — a BODY field, so
// `Boxed3152`'s own FieldSymbol for `V` exercises the `fieldDeclTypeOf`
// AST-cast arm when Boxed3152 is declared locally, and its null-decl arm
// when Boxed3152 is imported and `Holder3152`'s own field walk descends
// into it.
const char* kDecls3152 =
    "public record Boxed3152 "
    "  public V: int; "
    "  public constructor(v: int) V := v; end constructor; "
    "end Boxed3152; "
    "public record Holder3152(B: Boxed3152, Tag: int); ";

std::string lib3152() { return std::string("module Lib3152; ") + kDecls3152; }

std::string app3152(const std::string& body) {
    return "module App3152; import Lib3152; class C " + body + " end C; ";
}

std::string local3152(const std::string& body) {
    return std::string("module App3152Local; ") + kDecls3152 +
           " class C " + body + " end C; ";
}

const char* kBody3152 =
    "  public static method Main(): int "
    "    var h := new Holder3152(new Boxed3152(41), 1); "
    "    return h.B.V + h.Tag; "
    "  end Main; ";

}  // namespace

// T1 — a local record (`Holder3152`) with a value-typed field of an IMPORTED
// record type (`Boxed3152`) compiles and runs cleanly: the module boundary
// stops `recordValueContainsSelf`'s walk at `B`'s field without asserting a
// (nonexistent) cycle back to `Holder3152`.
BEZEL_TEST(xmod_record_value_field_import_3152,
           imported_value_record_field_compiles_and_runs) {
    BEZEL_EXPECT_EQ(runXmod3152(lib3152(), app3152(kBody3152)), 42);
}

// The single-module control: identical declarations and body, all local.
// Pins that moving the accessor to `fieldDeclTypeOf` did not change the
// local answer.
BEZEL_TEST(xmod_record_value_field_import_3152,
           local_value_record_field_compiles_and_runs_control) {
    BEZEL_EXPECT_EQ(runLocal3152(local3152(kBody3152)), 42);
}

}  // namespace bezel::module
