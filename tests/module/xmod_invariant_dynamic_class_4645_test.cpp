// Regression tests for #4645 across a module boundary — §22.16 (v0.338), the
// DYNAMIC class invariant at a member a subclass inherits.
//
// §22.16 "The visibility boundary and the `this`-reentrancy exemption":
//
//   "A call whose receiver is `this` — explicit or implicit — from inside any
//    instance member of the class or a subclass enters the member's
//    **unchecked face**: no invariant evaluation at that call's entry or
//    exit."
//
//   "**Dispatch checks the dynamic class's invariant**: an external call
//    through a base-typed reference evaluates the full dynamic class invariant
//    — base and derived conjunction — at the dynamic target's checked face."
//
// The fix evaluates, at an exporter member's instrumentation points, the
// clauses of every subclass the exporter's compilation names, behind an is-a
// test and a known-class gate (src/codegen/LlvmBackend_Setup.cpp,
// `dynamicInvariantArms` / `emitInvariantCheck`). Two boundary facts follow:
//
//   row 1  An exported subclass `LD` that declares a clause, instantiated in
//          the IMPORTER and called through a base-typed reference: the
//          exporter names `LD`, so its clause is evaluated at the inherited
//          `B.Bump` (99). Pre-fix: 42.
//   row 2  An importer-declared `E: LD` that breaks `LD`'s clause around a
//          `this`-receiver call to the inherited `Bump`: §22.16 exempts the
//          call (42). The importer cannot reach the exporter's private
//          unchecked face (the #4597 residual), so an is-a test alone matched
//          `E` against `LD`'s arm and raised a false violation (99) where the
//          pre-fix compiler gave 42; the known-class gate keeps `E` (unnamed
//          by the exporter) at the pre-fix behaviour.
//   row 3  Release mode evaluates nothing (42), the same program as row 1.
//
// The AOT harness compiles ONE module, so these rows use the established
// two-module shape (tests/module/xmod_imported_base_invariant_4592_test.cpp):
// `Driver::compileObject` for the library, `compileFileForTest` for the
// importer, `runProgramForTest` for the product. No JIT oracle is used.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "codegen/LlvmBackend.hpp"
#include "driver/Driver.hpp"
#include "support/driver_run_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

namespace fs = std::filesystem;

struct TempFile4645 {
    std::string path;
    explicit TempFile4645(std::string p) : path(std::move(p)) {}
    ~TempFile4645() { std::remove(path.c_str()); }
};

void writeSource4645(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Build `libSrc` as a `-c` library object in `opts`'s mode, build `mainSrc`
// importing it in the same mode, run it, and return its exit code.
// Distinct negative sentinels, so a broken scaffold never reads as a pass:
//   -2 library did not build, -3 importer did not build/link,
//   -4 the program did not exit normally.
int buildImportAndRun4645(const std::string& libSrc, const std::string& mainSrc,
                          const codegen::CodegenOptions& opts, const char* tag) {
    const fs::path& dir = wtScratchDir(tag);
    const std::string id = unique_stem(tag);
    TempFile4645 libSrcF((dir / (id + "_lib.bzl")).string());
    TempFile4645 libObjF((dir / (id + "_lib.o")).string());
    TempFile4645 appSrcF((dir / (id + "_app.bzl")).string());
    TempFile4645 appExeF((dir / id).string());
    writeSource4645(libSrcF.path, libSrc);
    writeSource4645(appSrcF.path, mainSrc);

    bezel::Driver driver;
    codegen::CodegenOptions libOpts = opts;
    libOpts.isMainModule = false;
    if (!driver.compileObject(libSrcF.path, libObjF.path, {}, libOpts).success)
        return -2;
    if (!std::ifstream(libObjF.path).good()) return -2;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = libObjF.path;
    d.objectPath = libObjF.path;
    deps.push_back(d);

    if (!bezel::test::compileFileForTest(driver, appSrcF.path, appExeF.path,
                                          deps, opts)
             .success)
        return -3;

    int status = bezel::test::runProgramForTest(appExeF.path);
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -4;
}

codegen::CodegenOptions checkedContractsOpts4645() {
    codegen::CodegenOptions o;
    o.checkedContracts = true;  // exactly what `bezelc --checked-contracts` sets
    return o;
}

// The library: `B` declares no clause; the exported subclass `LD` adds one.
const char* kLib4645 =
    "module DynInvLib4645; "
    "public class B "
    "  public _x: int := 0; "
    "  public method Bump(v: int): int _x := _x + v; return _x; end Bump; "
    "end B; "
    "public class LD: B "
    "  invariant(_x >= 0); "
    "end LD; ";

// Row 1 / row 3: an `LD` built in the importer, called through `B`.
const char* kAppExternal4645 =
    "module DynInvApp4645; "
    "import DynInvLib4645; "
    "class Prog "
    "  public static method Main(): int "
    "    var r: int := 0; "
    "    using var d := new LD(); "
    "    var b: B := d; "
    "    try b.Bump(0 - 5); r := 42; "
    "    catch ContractViolationException => r := 99; "
    "    end; "
    "    return r; "
    "  end Main; "
    "end Prog; ";

// Row 2: an importer-declared subclass passing through a broken state around
// a `this`-receiver call to the inherited `Bump`, then restoring it.
const char* kAppThisReentry4645 =
    "module DynInvAppE4645; "
    "import DynInvLib4645; "
    "class E: LD "
    "  public method Work(): int "
    "    _x := 0 - 7; "
    "    var t := Bump(1); "
    "    _x := 3; "
    "    return t; "
    "  end Work; "
    "end E; "
    "class Prog "
    "  public static method Main(): int "
    "    var r: int := 0; "
    "    using var e := new E(); "
    "    try e.Work(); r := 42; "
    "    catch ContractViolationException => r := 99; "
    "    end; "
    "    return r; "
    "  end Main; "
    "end Prog; ";

}  // namespace

BEZEL_TEST(xmod_invariant_dynamic_class_4645,
           exported_subclass_clause_checked_at_inherited_member_spec_22_16) {
    BEZEL_EXPECT_EQ(99, buildImportAndRun4645(kLib4645, kAppExternal4645,
                                              checkedContractsOpts4645(),
                                              "xmod4645_ext"));
}

BEZEL_TEST(xmod_invariant_dynamic_class_4645,
           importer_subclass_this_receiver_inherited_call_unchecked_spec_22_16) {
    BEZEL_EXPECT_EQ(42, buildImportAndRun4645(kLib4645, kAppThisReentry4645,
                                              checkedContractsOpts4645(),
                                              "xmod4645_this"));
}

BEZEL_TEST(xmod_invariant_dynamic_class_4645,
           release_mode_evaluates_no_subclass_clause_spec_22_16) {
    BEZEL_EXPECT_EQ(42, buildImportAndRun4645(kLib4645, kAppExternal4645,
                                              codegen::CodegenOptions{},
                                              "xmod4645_rel"));
}

}  // namespace bezel::module
