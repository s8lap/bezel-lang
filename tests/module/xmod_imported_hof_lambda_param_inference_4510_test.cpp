// Regression tests for #4510 — §2.8.2 clause 1 / §27.4 / §27.1 (v0.306):
// a call-argument lambda whose parameter types are OMITTED must take them from
// the callee's formal on BOTH sides of a module boundary.
//
// THE DEFECT. `ExprTC`'s fix-#829 seeding — the thread that puts the callee's
// function-typed formal into `pendingLambdaParamTypes_` so an unannotated
// `lambda(g) => …` can type its parameter — was keyed on `calleeMd2`, the
// callee's `ast::MethodDecl`:
//
//     if (calleeMd2 && calleeMd2->genericParams.empty() && …)
//         if (auto* pt = ctx_.resolvedType(*calleeMd2->params[pos2].type)) …
//
// An IMPORTED method has NO `ast::MethodDecl` — `ModuleLoader` reconstructs a
// bare `sema::MethodSymbol` from the artifact's metadata and the formal types
// survive only as `sema::ParameterSymbol::resolvedDeclType`. So across a module
// boundary the lambda argument got no target type at all, and the parameter
// stayed untyped: `declType` null, `resolvedDeclType` null.
//
// IT WAS SILENT for a specific reason. §27.4's own "no target type" rejection
// (#3756) is suppressed for a call argument — `hadArgContext` is set by every
// call-argument lambda precisely because a formal slot always exists — so the
// one diagnostic that would have caught an untyped parameter is switched off
// exactly where this thread was supposed to supply the type. What ran instead
// was `lowerLambda`'s `llvm::Type* ty = i32()` DEFAULT.
//
// THAT DEFAULT IS WHY THE ISSUE'S REPRO IS NESTED. For a one-level `(int) -> R`
// formal the i32 default is accidentally CORRECT, so the whole defect class
// hides behind `int`. Measured pre-fix, each against its own single-module
// twin, with the real `bezelc` (a `-c` library plus an importing executable):
//   ((int) -> int) -> int, `lambda(g) => g(9)`        twin 10   imported 0
//   ((int) -> int) -> int, `lambda(g) => g(9) + 100`  twin 110  imported 0
//   the same as an INSTANCE method                    twin 10   imported 0
//   (string) -> int, `lambda(s) => s.Length`          twin 5    imported 0
//   (int, string) -> int, `lambda(a, s) => …`         twin 7    imported 0
// The nested row is not a special case — it is the first shape whose parameter
// is not i32-shaped. `BEZEL_DUMP_IR=1` on the issue's repro shows the mechanism
// exactly: the twin emits the outer lambda as `i32 (ptr, ptr)` whose body is an
// indirect call through the closure, while the importer emits `i32 (ptr, i32)`
// whose body is a bare `ret i32 0` — the parameter was lowered as an `int` and
// the call through it was never emitted at all. The result is therefore the
// lambda's default zero, not garbage read from freed or uninitialised storage.
//
// THE FIX gives the site the same BOTH-SOURCES accessor every sibling thread on
// this path already had — `formalOutParamType2` for `out var` recipients
// (#2657/#3186) and `formalCtorOutParamType` on the `new` path (#3772), which
// is why an imported CONSTRUCTOR taking the identical `((int) -> int) -> int`
// formal already answered 10 while the imported METHOD answered 0. §2.8.2
// clause 1 is the authority: the artifact interface must be sufficient to
// type-check every well-formed use of the public surface *with full
// signatures*, and §27.4 admits the omitted annotation whenever the target type
// is known from context — crossing a module boundary does not change whether it
// is known.
//
// THE TEST. Every row crosses a real `--import` boundary, because the imported
// symbol is the only way to reach a callee with no `ast::MethodDecl`, and runs
// the IDENTICAL program text in ONE module as its own oracle, so "the boundary
// changed the answer" is executable rather than asserted. The AOT harness
// cannot express a module dependency, so the rows hand-roll
// `Driver::compileObject` plus `bezel::test::compileFileForTest`, and run the
// product through `bezel::test::runProgramForTest` rather than `std::system`
// (#4532) so the run gets the sweep's own sanitizer options instead of
// inheriting LeakSanitizer's exit code under `gate.sh --full`. No JIT oracle.
//
// ROWS 6-8 ARE THE DIRECTION WITH NO ORACLE — behaviour the fix must NOT
// change, and which nothing else in the suite pins across a boundary: the
// annotated spelling that was the issue's documented workaround, a lambda that
// never calls its parameter (the outer closure was always built correctly), and
// a function-typed formal on a GENERIC class, whose class-level substitution
// (#1140) and whose generic-callee skip (#505) both had to survive being moved
// behind the new accessor.

#include <cstdio>
#include <cstdlib>
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

struct TempFile4510 {
    std::string path;
    explicit TempFile4510(std::string p) : path(std::move(p)) {}
    ~TempFile4510() { std::remove(path.c_str()); }
};

void writeSource4510(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Distinct negative sentinels so a scaffold failure can never read as a pass;
// every row asserts an exact non-negative value, so any of these FAILS loudly.
//   -2  the library half did not build
//   -3  the compiled program did not build
//   -4  the program could not be run to a normal exit
constexpr int kLibBuildFailed4510 = -2;
constexpr int kAppBuildFailed4510 = -3;
constexpr int kAbnormalExit4510 = -4;

// Build `libSrc` as a `-c` library object, build `mainSrc` as an executable
// importing it, run it, return the exit code.
int buildImportAndRun4510(const std::string& libSrc, const std::string& mainSrc,
                          const char* tag) {
    const fs::path& dir = wtScratchDir(tag);
    const std::string id = unique_stem(tag);
    TempFile4510 libSrcF((dir / (id + "_lib.bzl")).string());
    TempFile4510 libObjF((dir / (id + "_lib.o")).string());
    TempFile4510 appSrcF((dir / (id + "_app.bzl")).string());
    TempFile4510 appExeF((dir / id).string());
    writeSource4510(libSrcF.path, libSrc);
    writeSource4510(appSrcF.path, mainSrc);

    bezel::Driver driver;
    if (!driver.compileObject(libSrcF.path, libObjF.path).success)
        return kLibBuildFailed4510;
    if (!std::ifstream(libObjF.path).good()) return kLibBuildFailed4510;

    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = libObjF.path;
    d.objectPath = libObjF.path;
    deps.push_back(d);

    // compileFileForTest, not compileFile, so `gate.sh --full`'s
    // BEZEL_AOT_FORCE_SANITIZE sweep reaches the importer half (#3653).
    if (!bezel::test::compileFileForTest(driver, appSrcF.path, appExeF.path,
                                          deps, codegen::CodegenOptions{})
             .success)
        return kAppBuildFailed4510;

    // runProgramForTest, not std::system: under `gate.sh --full` the ambient
    // environment leaves LeakSanitizer ON and its exit code then REPLACES this
    // program's (#4532). The helper gives the run the sweep's own options
    // (detect_leaks=0:abort_on_error=0, exactly what `runAot` uses per #1822)
    // and fails the test loudly, with the report, if the sanitizer fires.
    int status = bezel::test::runProgramForTest(appExeF.path);
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status)
                                               : kAbnormalExit4510;
}

// THE ORACLE. The same program text, compiled as ONE module. There the callee
// has a real `ast::MethodDecl`, so the fix-#829 thread was never in doubt; this
// is what the language says the program means.
int buildSingleAndRun4510(const std::string& src, const char* tag) {
    const fs::path& dir = wtScratchDir(tag);
    const std::string id = unique_stem(tag);
    TempFile4510 srcF((dir / (id + "_one.bzl")).string());
    TempFile4510 exeF((dir / id).string());
    writeSource4510(srcF.path, src);

    bezel::Driver driver;
    if (!bezel::test::compileFileForTest(driver, srcF.path, exeF.path, {},
                                          codegen::CodegenOptions{})
             .success)
        return kAppBuildFailed4510;

    int status = bezel::test::runProgramForTest(exeF.path);
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status)
                                               : kAbnormalExit4510;
}

// Every row is built from ONE class text used verbatim in both halves, so the
// twin and the imported build cannot drift: only the module boundary differs.
std::string lib4510(const std::string& mod, const std::string& cls) {
    return "module " + mod + "Lib4510; " + cls;
}
std::string app4510(const std::string& mod, const std::string& main) {
    return "module " + mod + "App4510; import " + mod + "Lib4510; " + main;
}
std::string one4510(const std::string& mod, const std::string& cls,
                    const std::string& main) {
    return "module " + mod + "One4510; " + cls + main;
}

// ── Row 1: THE ISSUE'S REPRO — a static method whose formal is a NESTED
// function type (§27.1), called with a parameter-type-inferring lambda (§27.4).
// `g` must be typed `(int) -> int`, so `g(9)` calls the library's own
// `lambda(y: int) => y + 1` and the answer is 10. Pre-fix the imported call
// answered 0: `g` was lowered `i32` and `g(9)` was never emitted.
const std::string kHsClass4510 =
    "public class HsC4510 "
    "  public static method ApplyHof(f: ((int) -> int) -> int): int "
    "    return f(lambda(y: int) => y + 1); "
    "  end ApplyHof; "
    "end HsC4510; ";
const std::string kHsMain4510 =
    "class HsMain4510 "
    "  public static method Main(): int "
    "    return HsC4510.ApplyHof(lambda(g) => g(9)); "
    "  end Main; "
    "end HsMain4510; ";

// ── Row 2: the same call with `+ 100` in the lambda body. This separates the
// two readings of row 1's zero: if only `g`'s VALUE were wrong the answer would
// still be at least 100, so a 0 here proves the whole body was dropped rather
// than evaluated against bad storage. Twin and fixed importer both answer 110.
const std::string kHpClass4510 =
    "public class HpC4510 "
    "  public static method ApplyHof(f: ((int) -> int) -> int): int "
    "    return f(lambda(y: int) => y + 1); "
    "  end ApplyHof; "
    "end HpC4510; ";
const std::string kHpMain4510 =
    "class HpMain4510 "
    "  public static method Main(): int "
    "    return HpC4510.ApplyHof(lambda(g) => g(9) + 100); "
    "  end Main; "
    "end HpMain4510; ";

// ── Row 3: the same defect on an INSTANCE method — the accessor the fix
// installs is reached through `calleeMs2` either way, and a receiver-qualified
// call takes a different route to `calleeSym2` than a static one.
const std::string kHiClass4510 =
    "public class HiC4510 "
    "  public constructor() end constructor; "
    "  public method ApplyHof(f: ((int) -> int) -> int): int "
    "    return f(lambda(y: int) => y + 1); "
    "  end ApplyHof; "
    "end HiC4510; ";
const std::string kHiMain4510 =
    "class HiMain4510 "
    "  public static method Main(): int "
    "    using var c := new HiC4510(); "
    "    return c.ApplyHof(lambda(g) => g(9)); "
    "  end Main; "
    "end HiMain4510; ";

// ── Row 4: ONE level, but a `string` parameter (§27.1 / §27.4). This is the row
// that shows the defect is not about nesting: any formal whose parameter type
// is not i32-shaped loses, and `(int) -> R` only ever passed because
// `lowerLambda`'s fallback happens to be `i32`. Pre-fix the imported call
// answered 0 against the twin's 5.
const std::string kStClass4510 =
    "public class StC4510 "
    "  public static method ApplyStr(f: (string) -> int): int "
    "    return f(\"abcde\"); "
    "  end ApplyStr; "
    "end StC4510; ";
const std::string kStMain4510 =
    "class StMain4510 "
    "  public static method Main(): int "
    "    return StC4510.ApplyStr(lambda(s) => s.Length); "
    "  end Main; "
    "end StMain4510; ";

// ── Row 5: TWO inferred parameters of different types (§27.4). The thread is
// positional, so this pins that every slot is seeded, not just the first;
// pre-fix the imported call answered 0 against the twin's 7 (3 + "abcd".Length).
const std::string kMuClass4510 =
    "public class MuC4510 "
    "  public static method ApplyMulti(f: (int, string) -> int): int "
    "    return f(3, \"abcd\"); "
    "  end ApplyMulti; "
    "end MuC4510; ";
const std::string kMuMain4510 =
    "class MuMain4510 "
    "  public static method Main(): int "
    "    return MuC4510.ApplyMulti(lambda(a, s) => a + s.Length); "
    "  end Main; "
    "end MuMain4510; ";

// ── Row 6: NO-ORACLE DIRECTION — the ANNOTATED spelling, which is what the
// issue documented as the workaround and what was already correct on both sides
// (10). It must stay correct: the fix now runs the seeding thread on the
// imported path too, and a thread that threaded the WRONG type would break the
// annotated row (the annotation wins per §27.4, but the target-return slot and
// the ownership marker travel on the same thread).
const std::string kAnClass4510 =
    "public class AnC4510 "
    "  public static method ApplyHof(f: ((int) -> int) -> int): int "
    "    return f(lambda(y: int) => y + 1); "
    "  end ApplyHof; "
    "end AnC4510; ";
const std::string kAnMain4510 =
    "class AnMain4510 "
    "  public static method Main(): int "
    "    return AnC4510.ApplyHof(lambda(g: (int) -> int) => g(9)); "
    "  end Main; "
    "end AnMain4510; ";

// ── Row 7: NO-ORACLE DIRECTION — an inferring lambda that never CALLS its
// parameter (77 on both sides, before and after). It pins the other half of the
// issue's evidence: the outer closure was always constructed correctly, so a
// "fix" that changed how the lambda value itself is formed would show here.
const std::string kCoClass4510 =
    "public class CoC4510 "
    "  public static method ApplyHof(f: ((int) -> int) -> int): int "
    "    return f(lambda(y: int) => y + 1); "
    "  end ApplyHof; "
    "end CoC4510; ";
const std::string kCoMain4510 =
    "class CoMain4510 "
    "  public static method Main(): int "
    "    return CoC4510.ApplyHof(lambda(g) => 77); "
    "  end Main; "
    "end CoMain4510; ";

// ── Row 8: NO-ORACLE DIRECTION — a function-typed formal `(T) -> int` on a
// GENERIC class instantiated at `string` (§14 / #1140). Two behaviours the fix
// had to carry across unchanged meet here: the class-level substitution that
// turns `(T) -> int` into `(string) -> int` for inference, and the #505 skip
// that leaves a generic CALLEE alone. Both already worked across the boundary
// (7 on both sides, before and after) because the class-level generic path
// types the parameter by a different route; moving the lookup behind the new
// accessor must not disturb it.
const std::string kGcClass4510 =
    "public class GcC4510<T> "
    "  public constructor() end constructor; "
    "  public method Apply(f: (T) -> int, v: T): int "
    "    return f(v); "
    "  end Apply; "
    "end GcC4510; ";
const std::string kGcMain4510 =
    "class GcMain4510 "
    "  public static method Main(): int "
    "    using var b := new GcC4510<string>(); "
    "    return b.Apply(lambda(s) => s.Length, \"abcdefg\"); "
    "  end Main; "
    "end GcMain4510; ";

}  // namespace

// ===========================================================================
// Row 1 — THE BUG (§2.8.2 clause 1 / §27.4 / §27.1, v0.306). Pre-fix the
// imported call answered 0 while the identical single-module program answered
// 10.
// ===========================================================================
BEZEL_TEST(xmod_imported_hof_lambda_param_inference_4510,
           imported_nested_function_formal_infers_lambda_param_spec_27_4) {
    BEZEL_EXPECT_EQ(10, buildSingleAndRun4510(
                            one4510("Hs", kHsClass4510, kHsMain4510),
                            "wt-4510-hs-one"));
    BEZEL_EXPECT_EQ(10, buildImportAndRun4510(
                            lib4510("Hs", kHsClass4510),
                            app4510("Hs", kHsMain4510), "wt-4510-hs-xmod"));
}

// ===========================================================================
// Row 2 — the body is DROPPED, not miscomputed (§27.4, v0.306). `g(9) + 100`
// cannot be under 100 if the body ran at all; pre-fix the imported call
// answered 0 against the twin's 110.
// ===========================================================================
BEZEL_TEST(xmod_imported_hof_lambda_param_inference_4510,
           imported_hof_lambda_body_is_emitted_not_dropped_spec_27_4) {
    BEZEL_EXPECT_EQ(110, buildSingleAndRun4510(
                             one4510("Hp", kHpClass4510, kHpMain4510),
                             "wt-4510-hp-one"));
    BEZEL_EXPECT_EQ(110, buildImportAndRun4510(
                             lib4510("Hp", kHpClass4510),
                             app4510("Hp", kHpMain4510), "wt-4510-hp-xmod"));
}

// ===========================================================================
// Row 3 — the same defect on an imported INSTANCE method (§2.8.2 clause 1,
// v0.306). Pre-fix 0 against the twin's 10.
// ===========================================================================
BEZEL_TEST(xmod_imported_hof_lambda_param_inference_4510,
           imported_instance_method_hof_formal_infers_lambda_param_spec_2_8_2) {
    BEZEL_EXPECT_EQ(10, buildSingleAndRun4510(
                            one4510("Hi", kHiClass4510, kHiMain4510),
                            "wt-4510-hi-one"));
    BEZEL_EXPECT_EQ(10, buildImportAndRun4510(
                            lib4510("Hi", kHiClass4510),
                            app4510("Hi", kHiMain4510), "wt-4510-hi-xmod"));
}

// ===========================================================================
// Row 4 — ONE level, a `string` parameter (§27.4 / §27.1, v0.306). The defect
// is the missing target type, not the nesting: pre-fix 0 against the twin's 5.
// ===========================================================================
BEZEL_TEST(xmod_imported_hof_lambda_param_inference_4510,
           imported_string_parameter_formal_infers_lambda_param_spec_27_1) {
    BEZEL_EXPECT_EQ(5, buildSingleAndRun4510(
                           one4510("St", kStClass4510, kStMain4510),
                           "wt-4510-st-one"));
    BEZEL_EXPECT_EQ(5, buildImportAndRun4510(
                           lib4510("St", kStClass4510),
                           app4510("St", kStMain4510), "wt-4510-st-xmod"));
}

// ===========================================================================
// Row 5 — every positional slot is seeded, not just the first (§27.4, v0.306).
// Pre-fix 0 against the twin's 7.
// ===========================================================================
BEZEL_TEST(xmod_imported_hof_lambda_param_inference_4510,
           imported_multi_parameter_formal_seeds_every_slot_spec_27_4) {
    BEZEL_EXPECT_EQ(7, buildSingleAndRun4510(
                           one4510("Mu", kMuClass4510, kMuMain4510),
                           "wt-4510-mu-one"));
    BEZEL_EXPECT_EQ(7, buildImportAndRun4510(
                           lib4510("Mu", kMuClass4510),
                           app4510("Mu", kMuMain4510), "wt-4510-mu-xmod"));
}

// ===========================================================================
// Row 6 — no-oracle direction: the ANNOTATED spelling, the issue's documented
// workaround, which was correct on both sides before the fix (§27.4, v0.306).
// ===========================================================================
BEZEL_TEST(xmod_imported_hof_lambda_param_inference_4510,
           annotated_lambda_parameter_keeps_working_spec_27_4) {
    BEZEL_EXPECT_EQ(10, buildSingleAndRun4510(
                            one4510("An", kAnClass4510, kAnMain4510),
                            "wt-4510-an-one"));
    BEZEL_EXPECT_EQ(10, buildImportAndRun4510(
                            lib4510("An", kAnClass4510),
                            app4510("An", kAnMain4510), "wt-4510-an-xmod"));
}

// ===========================================================================
// Row 7 — no-oracle direction: an inferring lambda that never calls its
// parameter, which was already correct on both sides (§27.5, v0.306). The
// outer closure was never the defect and must stay that way.
// ===========================================================================
BEZEL_TEST(xmod_imported_hof_lambda_param_inference_4510,
           inferring_lambda_that_never_calls_its_param_is_unchanged_spec_27_5) {
    BEZEL_EXPECT_EQ(77, buildSingleAndRun4510(
                            one4510("Co", kCoClass4510, kCoMain4510),
                            "wt-4510-co-one"));
    BEZEL_EXPECT_EQ(77, buildImportAndRun4510(
                            lib4510("Co", kCoClass4510),
                            app4510("Co", kCoMain4510), "wt-4510-co-xmod"));
}

// ===========================================================================
// Row 8 — no-oracle direction: a function-typed formal on a GENERIC class
// (§14 / §27.4, v0.306). The class-level substitution (#1140) and the
// generic-callee skip (#505) both had to survive moving the lookup behind the
// both-sources accessor; 7 on both sides, before and after.
// ===========================================================================
BEZEL_TEST(xmod_imported_hof_lambda_param_inference_4510,
           generic_class_function_formal_is_unchanged_spec_14) {
    BEZEL_EXPECT_EQ(7, buildSingleAndRun4510(
                           one4510("Gc", kGcClass4510, kGcMain4510),
                           "wt-4510-gc-one"));
    BEZEL_EXPECT_EQ(7, buildImportAndRun4510(
                           lib4510("Gc", kGcClass4510),
                           app4510("Gc", kGcMain4510), "wt-4510-gc-xmod"));
}

}  // namespace bezel::module
