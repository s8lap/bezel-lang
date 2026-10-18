// Regression tests for #4488 — §28.2 / §22.16 / §33.6 (v0.306): the checked
// MODES must cross an `--import` boundary.
//
// THE DEFECT. `Driver::compileObject` (the `-c` library path) default-
// constructed the codegen options it handed to `codegen::emitModule`:
//
//     codegen::CodegenOptions libOpts;        // every field back to default
//     libOpts.isMainModule = false;
//
// so every flag the caller passed — `checkedDisposal` and `checkedContracts`
// among them — was dropped on the floor. `bezelc -c --checked-disposal lib.bzl`
// accepted the flag and produced an UNCHECKED object; an executable built
// `--checked-disposal` against it then ran the dependency's code with no
// guards while appearing to be in checked mode.
//
// WHY THIS IS AN ORACLE DEFECT, NOT A DRIVER NIT. Both modes are, in the
// spec's own words, "a **per-build** choice between two conformance modes"
// (§28.2 "Use after disposal; checked disposal mode"; §22.16 "`ensures` and
// `invariant` run under checked-contracts mode") — a *build*, not a
// translation unit. Their instrumentation is EMITTED into the code (§28.2's
// disposed-flag entry guard; §22.16's clause evaluations), so nothing at link
// time can put it back: a dependency compiled without it simply has no checks.
// The contrast is `sanitizeAddress`, which is *linked*: the executable's ASan
// runtime interposes `malloc` process-wide, so dropping it from the library
// costs only redzone instrumentation and leak detection still covers the
// dependency (this is why the fix deliberately does NOT forward that one —
// row 4 pins the override).
//
// THE FIX (this commit). `compileObject` DERIVES `libOpts` from the caller's
// `opts` and overrides exactly the two fields whose meaning is specific to a
// library artifact (`isMainModule` per §33.6, `sanitizeAddress` per the link
// argument above). Deriving rather than default-constructing is the structural
// half: a field added to `CodegenOptions` later is carried onto this path by
// default, so the next emitted flag cannot be forgotten the way these two were.
//
// MEASURED ACROSS THE FIX BOUNDARY (the CLI repro from the issue, both halves
// built `--checked-disposal`, one module importing the other):
//     pre-fix    exit 0   — `GetValue()` on the disposed instance RETURNED 42
//     post-fix   exit 1   — DisposedAccessException, caught by the arm
// The in-module twin of the same program returns 1 on both, which is exactly
// why the gap was invisible: checked mode looked like it worked.
//
// THE TEST. Every row crosses a REAL `--import` boundary — an in-module test
// cannot pin this bug, because the flag was only dropped on the library path.
// The AOT harness (`runAot`/`runAotChecked`) compiles ONE module and cannot
// express a dependency, so these rows hand-roll `Driver::compileObject` for
// the library plus `bezel::test::compileFileForTest` for the importer, the
// established two-module shape (tests/module/xmod_3277_forin_userclass_dispose
// _test.cpp, tests/module/cross_module_import_test.cpp), and run the product
// through `bezel::test::runProgramForTest` rather than `std::system` (#4532).
// No JIT oracle is used.
//
// ORACLE NOTE — WHY THE RUN GOES THROUGH `runProgramForTest`. A checked-
// disposal program LEAKS BY DESIGN: §28.2 (v0.135, issues #621/#627)
// quarantines a disposed object instead of freeing it, so the guard at a later
// member access has something to read — `bz_array_mark_disposed` /
// `bz_closure_mark_disposed` / `bz_shared_mark_disposed`, whose own comments
// call it "a deliberate memory leak in checked builds". Under `gate.sh --full`
// a hand-rolled `std::system(exe)` inherited LeakSanitizer's defaults, so that
// designed quarantine forced the exit status to 1 and BOTH failure directions
// showed up at once:
//   * row 3's `checked` half reported 1 where the program returns 42 — an
//     oracle asserting something §28.2 contradicts;
//   * row 1 asserts exactly 1, so it passed VACUOUSLY, green whatever its
//     program did.
// The sweep's own run options (`detect_leaks=0:abort_on_error=0`, what every
// `runAot` test uses per #1822) restore both rows to asserting the §28.2
// behaviour they exist to pin, while a real redzone error still fails the row
// loudly with its report (#4532).

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "codegen/LlvmBackend.hpp"
#include "driver/Driver.hpp"
#include "support/aot_support.hpp"  // runAotModules
#include "support/driver_run_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

// ── PORT NOTE (#6386, round 2) — ROWS 1-3 ONLY, AND ROW 4 IS DECLINED ─────────
//
// Rows 1-3 now call `test::runAotModules` with the same `CodegenOptions` the
// hand-rolled `buildImportAndRun4488` passed, instead of two of its own
// `compileObject` / `compileFileForTest` calls plus `std::system`. Each row
// asserts `r.ran` FIRST and unconditionally, then the exit code; the retired
// runner's four negative sentinels (-2 library, -3 importer, -4 run) collapse
// into one fact that also says WHICH, on `r.diagnostics`. The helper and its
// sentinel scheme are KEPT — row 4 still uses them.
//
// ⚠ ROW 4 IS DELIBERATELY NOT PORTED, AND IT CANNOT BE. Its entire subject is
// the NON-forwarding of `sanitizeAddress` on the `-c` library path: it passes
// `libOpts.sanitizeAddress = true` to `compileObject` and requires it to be
// IGNORED, because an instrumented `-c` object has no ASan runtime behind it at
// a plain link. `runAotModules` applies ONE `opts` to EVERY module — it passes
// `opts.sanitizeAddress` through `runAotModulesImpl` into `forkedCompileModule`
// for each dependency — so porting row 4 would make the very forwarding it
// forbids, and the row would go green on the opposite behaviour. This is
// decline-with-reasons, not an oversight: `buildImportAndRun4488` remains for
// row 4 alone.
//
// ⚠ ROW 2 IS THE ONE PORTED PROGRAM IN THIS LANE THAT DECLARES A CONTRACT.
// Every other ported program has no `ensures` / `invariant`, so `runAotModules`
// (which does NOT set `checkedContracts`, unlike `runAotModulesChecked`) is
// behaviour-identical to the old runner for them. `kCcLib4488` declares
// `ensures (false)`, so the bit is load-bearing here. Row 2 therefore uses the
// PLAIN `runAotModules` with `checkedContractsOpts()` passed EXPLICITLY, rather
// than switching to `runAotModulesChecked` and having the option implied by the
// oracle's name. Rows 1 and 3 use `checkedDisposalOpts()`, which
// `runAotModules` threads through unchanged.
//
// Rows 1 and 3 also gain a new `ran` assertion each that the old runner had no
// way to make — its -2/-3/-4 sentinels were compared against an expected
// POSITIVE value, so a scaffold failure could not be told from a wrong answer.
// Both remain PLAIN-oracle rows: no leak oracle is added.
//
// Spec: §28.2, §22.16, §2.5, §2.6, v0.306, v0.338.

namespace bezel::module {
namespace {

namespace fs = std::filesystem;

struct TempFile4488 {
    std::string path;
    explicit TempFile4488(std::string p) : path(std::move(p)) {}
    ~TempFile4488() { std::remove(path.c_str()); }
};

void writeSource4488(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Build `libSrc` as a `-c` library OBJECT with `libOpts` (the path #4488 is
// about), then build `mainSrc` as an executable importing it with `appOpts`,
// run it, and return the program's exit code.
//
// Distinct negative sentinels, so a broken scaffold can NEVER read as a pass:
// every row below asserts an exact non-negative value.
//   -2  the library half did not build (or produced no object)
//   -3  the importer half did not build/link
//   -4  the program could not be run to a normal exit
int buildImportAndRun4488(const std::string& libSrc, const std::string& mainSrc,
                          const codegen::CodegenOptions& libOpts,
                          const codegen::CodegenOptions& appOpts,
                          const char* tag) {
    const fs::path& dir = wtScratchDir(tag);
    const std::string id = unique_stem(tag);
    TempFile4488 libSrcF((dir / (id + "_lib.bzl")).string());
    TempFile4488 libObjF((dir / (id + "_lib.o")).string());
    TempFile4488 appSrcF((dir / (id + "_app.bzl")).string());
    TempFile4488 appExeF((dir / id).string());
    writeSource4488(libSrcF.path, libSrc);
    writeSource4488(appSrcF.path, mainSrc);

    bezel::Driver driver;
    // THE CALL UNDER TEST: the flags in `libOpts` are what #4488 dropped.
    if (!driver.compileObject(libSrcF.path, libObjF.path, {}, libOpts).success)
        return -2;
    if (!std::ifstream(libObjF.path).good()) return -2;

    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = libObjF.path;
    d.objectPath = libObjF.path;
    deps.push_back(d);

    // compileFileForTest, not compileFile, so `gate.sh --full`'s
    // BEZEL_AOT_FORCE_SANITIZE sweep reaches the importer half (#3653).
    if (!bezel::test::compileFileForTest(driver, appSrcF.path, appExeF.path,
                                          deps, appOpts)
             .success)
        return -3;

    // runProgramForTest, not std::system: under `gate.sh --full` the
    // ambient environment leaves LeakSanitizer ON, and its exit code
    // then REPLACES this program's (#4532). The helper gives the run the
    // sweep's own options (detect_leaks=0:abort_on_error=0, exactly what
    // `runAot` uses per #1822) and fails the test loudly, with the report,
    // if the sanitizer actually fires.
    int status = bezel::test::runProgramForTest(appExeF.path);
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -4;
}

// ── §28.2 sources ────────────────────────────────────────────────────────
// The library owns the class, so the §28.2 disposed-flag guard at
// `GetValue`'s entry is emitted by the LIBRARY build — the exact codegen
// #4488 skipped. The importer creates, disposes, and then re-uses the
// instance: the straight-line use-after-dispose §28.2 makes a warning and
// checked mode makes a `DisposedAccessException`.
const char* kCdLib4488 =
    "module CdLib4488; "
    "public class CdRes4488 "
    "  public method GetValue(): int return 42; end GetValue; "
    "end CdRes4488; ";

const char* kCdAppUseAfterDispose4488 =
    "module CdApp4488; "
    "import CdLib4488; "
    "class CdMain4488 "
    "  public static method Main(): int "
    "    using var obj := new CdRes4488(); "
    "    obj.Dispose(); "
    "    try "
    "      var v := obj.GetValue(); "
    "      return 0; "
    "    catch DisposedAccessException => return 1; "
    "    end; "
    "  end Main; "
    "end CdMain4488; ";

// The CORRECT program (never uses a disposed instance) over the same library.
const char* kCdAppCorrect4488 =
    "module CdOk4488; "
    "import CdLib4488; "
    "class CdOkMain4488 "
    "  public static method Main(): int "
    "    using var obj := new CdRes4488(); "
    "    return obj.GetValue(); "
    "  end Main; "
    "end CdOkMain4488; ";

// ── §22.16 sources ───────────────────────────────────────────────────────
// The `ensures` clause is on a LIBRARY method, so its evaluation is emitted
// by the library build — the second emitted flag `compileObject` dropped.
const char* kCcLib4488 =
    "module CcLib4488; "
    "public class CcRes4488 "
    "  public method M(): int "
    "    ensures (false) "
    "    return 1; "
    "  end M; "
    "end CcRes4488; ";

const char* kCcApp4488 =
    "module CcApp4488; "
    "import CcLib4488; "
    "class CcMain4488 "
    "  public static method Main(): int "
    "    var c := new CcRes4488(); "
    "    var r: int := 0; "
    "    try "
    "      r := c.M(); "
    "    catch ContractViolationException => r := 99; "
    "    end; "
    "    return r; "
    "  end Main; "
    "end CcMain4488; ";

codegen::CodegenOptions checkedDisposalOpts() {
    codegen::CodegenOptions o;
    o.checkedDisposal = true;   // exactly what `bezelc --checked-disposal` sets
    return o;
}

codegen::CodegenOptions checkedContractsOpts() {
    codegen::CodegenOptions o;
    o.checkedContracts = true;  // exactly what `bezelc --checked-contracts` sets
    return o;
}

}  // namespace

// ===========================================================================
// Row 1 — THE BUG (§28.2, v0.306). Both halves of the build are in checked
// disposal mode. An instance member call on the disposed instance must raise
// DisposedAccessException, which the catch arm turns into exit 1.
//
// The guard lives at the entry of `CdRes4488.GetValue`, which is compiled by
// the LIBRARY half, so this row fails on the pre-fix compiler with exit 0 —
// the disposed instance answers 42 and the `return 0` arm is taken. Measured
// pre-fix: 0. Post-fix: 1.
// ===========================================================================
BEZEL_TEST(xmod_checked_mode_forwarding_4488,
           checked_disposal_crosses_import_boundary_spec_28_2) {
    auto r = test::runAotModules(
        {{"CdLib4488", kCdLib4488},
         {"CdApp4488", kCdAppUseAfterDispose4488}},
        "CdApp4488", checkedDisposalOpts());

    BEZEL_EXPECT_TRUE(r.ran);
    // 1 == the imported method's §28.2 entry guard fired and the arm caught
    // DisposedAccessException. 0 == the pre-#4488 unchecked library.
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// ===========================================================================
// Row 2 — the second emitted flag (§22.16, v0.306). Same boundary, checked-
// CONTRACTS mode: the library method's `ensures (false)` must be evaluated at
// its §22.16 instrumentation point and raise ContractViolationException,
// which the catch arm turns into 99.
//
// This row is not a duplicate of row 1: it pins a DIFFERENT field of
// CodegenOptions across the same boundary, which is what makes "derive the
// options" rather than "forward checkedDisposal" the tested behaviour.
// Measured pre-fix: 1 (the clause was never evaluated). Post-fix: 99.
//
// ⚠ THIS IS THE ONE ROW IN THE AREA WHERE `runAotModulesChecked` WOULD HAVE
// CHANGED THE OBSERVABLE. Unlike every other ported program, `kCcApp4488`'s
// library DOES declare an `ensures (false)` clause, so the `checkedContracts`
// bit is load-bearing here rather than inert. The port uses the PLAIN
// `runAotModules` with `checkedContractsOpts()` passed explicitly instead, so
// the option is stated by the row rather than implied by the oracle's name.
// ===========================================================================
BEZEL_TEST(xmod_checked_mode_forwarding_4488,
           checked_contracts_crosses_import_boundary_spec_22_16) {
    auto r = test::runAotModules(
        {{"CcLib4488", kCcLib4488}, {"CcApp4488", kCcApp4488}},
        "CcApp4488", checkedContractsOpts());

    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 99);
}

// ===========================================================================
// Row 3 — the §28.2 equivalence control. "The two modes give identical
// observable behavior for every program that never uses a disposed instance."
// The same library, now instrumented, must not change a correct program's
// answer: 42 in checked mode and 42 in the release default.
//
// This is what makes row 1 a real pin rather than "checked mode broke the
// import path": the instrumented library still links, still carries a
// loadable `.bezel.meta`, and still computes the right value.
// ===========================================================================
BEZEL_TEST(xmod_checked_mode_forwarding_4488,
           correct_program_identical_in_both_modes_spec_28_2) {
    auto checked = test::runAotModules(
        {{"CdLib4488", kCdLib4488}, {"CdApp4488", kCdAppCorrect4488}},
        "CdApp4488", checkedDisposalOpts());
    BEZEL_EXPECT_TRUE(checked.ran);
    BEZEL_EXPECT_EQ(checked.exitCode, 42);

    const codegen::CodegenOptions release;  // §28.2 unchecked (release) mode
    auto unchecked = test::runAotModules(
        {{"CdLib4488", kCdLib4488}, {"CdApp4488", kCdAppCorrect4488}},
        "CdApp4488", release);
    BEZEL_EXPECT_TRUE(unchecked.ran);
    BEZEL_EXPECT_EQ(unchecked.exitCode, 42);
}

// ===========================================================================
// Row 4 — the deliberate NON-forward (#4488). `sanitizeAddress` is the one
// field the fix keeps overriding to false on the library path: it is an
// executable-build option whose instrumentation needs the final link to pull
// in the ASan runtime, and `compileObject` performs no link (main.cpp rejects
// `-c --sanitize=address` for the same reason). A future change that forwards
// EVERY field would emit an ASan-instrumented library object here, and the
// importer half would then fail — this row reports -3 — because an
// instrumented `-c` object has no ASan runtime behind it at this plain link
// and (per the note in tests/codegen/nested_lambda_capture_content_release
// _3752_test.cpp) does not carry a loadable `.bezel.meta` either. Which of the
// two fires first is not measured here and the row does not depend on it.
//
// ORACLE NOTE: under `gate.sh --full` the importer is linked
// `-fsanitize=address` anyway (compileFileForTest honours
// BEZEL_AOT_FORCE_SANITIZE), so that link would succeed and this row would
// pass. It is therefore a PLAIN-oracle pin — #2633's "neither oracle
// dominates" in miniature, and the reason the gate runs both.
// ===========================================================================
BEZEL_TEST(xmod_checked_mode_forwarding_4488,
           sanitize_address_is_not_forwarded_to_the_library_object) {
    codegen::CodegenOptions libOpts;
    libOpts.sanitizeAddress = true;         // must be IGNORED on the -c path
    const codegen::CodegenOptions appOpts;  // plain: no ASan runtime at link
    const int rc = buildImportAndRun4488(kCdLib4488, kCdAppCorrect4488, libOpts,
                                         appOpts, "wt-4488-san");
    BEZEL_EXPECT_EQ(42, rc);
}

}  // namespace bezel::module
