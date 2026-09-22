// Regression tests for #4508 — §22.6 / §27.1 / §15.1 / §3.1 / §14 (v0.306):
// the §14/#705 signature key must give a Function, an InlineArray and the Unit
// type DISTINCT text, on EVERY side of the comparison.
//
// THE DEFECT. `metaTypeSigText` (src/module/ModuleLoader.cpp) rendered
// Primitive and Named and ended:
//
//     return "?";  // Unit, Function, InlineArray: placeholder
//
// That text is the §14/#705 key the importer matches an exporter's carried
// ctor/method BODY against, so every function type, every inline array and the
// unit type were one key. §22.6 makes overloads differing only at such a
// parameter legal (§27.1: `(int) -> int` and `(bool) -> int` are distinct
// types; §15.1: `[2]int` is a type of its own), and the attachment loops key on
// exactly this text — `importedMethods[metaMemberSigKey(…)] = …` is an
// ASSIGNMENT and the ctor/method loops `break` on the first key match — so the
// exporter's body was attached to the WRONG imported overload.
//
// The Unit half failed the other way: both mirrors already spelled it "()"
// while this side said "?", so a unit-typed parameter made the keys MISMATCH
// and the importer rejected a program the exporter compiled (§14/#662).
//
// WHY IT SURFACED NOW. The collision in `metaTypeSigText` is pre-existing, but
// #4480 moved the other side of the comparison. Before it the reconstruction
// stub rendered a function-typed parameter as `Object`, so `astTypeToSigText`
// keyed "Object" and the two sides mismatched — which harmlessly prevented any
// lookup from hitting. After it the stub renders `(int) -> int`, which keys
// "?" — so the two sides now MATCH, and the collision starts choosing.
//
// MEASURED ACROSS THE FIX BOUNDARY (real `bezelc`, a `-c` library plus an
// importing executable; every row's single-module twin is the oracle and is
// 105 on both sides of the fix):
//   row 1  two `(…) -> int` overloads   pre-fix 205 (the other overload's body)
//   row 2  function beside `[2]int`     pre-fix "Call parameter type does not
//                                       match function signature!" — the wrong
//                                       body's `[2 x i32]` parameter against a
//                                       `ptr` argument, an IR-verifier failure
//   row 3  `[2]int` beside `[3]int`     pre-fix the same IR-verifier failure
//   row 4  a unit-typed parameter       pre-fix "constructing an imported
//                                       generic type … requires the constructor
//                                       body to have been exported … §14/#662"
//   rows 5-7 the same-signature spellings  pre-fix 200 (the decoy body)
//
// THE FIX renders all three kinds, in `metaTypeSigText` AND in both mirrors —
// `astTypeToSigText` (the re-parsed stub's AST) and `typeRefSigText` (the
// imported ParameterSymbol's `resolvedDeclType`) — in ONE commit, because the
// three are compared against each other and moving one alone would silently
// stop attachment matching instead.
//
// ROWS 5-7 ARE THE DIRECTION WITH NO ORACLE. Making two DIFFERENT signatures
// differ is what rows 1-4 pin; making the SAME signature agree across the
// boundary is the defect this fix could have introduced, and nothing in the
// gate would have caught it. Each of these rows spells a function type in a
// form the reconstruction stub NORMALISES — a parameter name the stub drops
// (§27.1: names are decoration), a `ref` modifier and a `()` return the stub
// keeps (§27.1: modifiers are part of the type), a nested function type — and
// drives BOTH sides of BOTH comparisons at once: the ctor-body attachment
// (metaTypeSigText vs typeRefSigText) and the self-call overload remap inside
// that body (metaMemberSigKey vs freshMethodSigKey/astTypeToSigText). A key
// that drifted on either side yields the decoy's 200 or a §14/#662 rejection.
//
// The full sweep behind those rows is 13 spellings (parameter name, `ref`
// modifier, unit return, nested function type, generic-parameter parameter,
// trailing comma, nullable function type, `scoped` inner parameter, a second
// public class in the library, literal extent, class-element array, plus the
// two below); 11 go from wrong-or-rejected to the twin's answer, and the other
// two fail IDENTICALLY on both sides of the fix and are therefore pre-existing,
// not pinned here: `[MAX]int` with a const-named extent SEGVs the importer, and
// a function type with an `owned` return (`(int) -> owned Bx`), like an owning
// inline array (`[2 owned]Bx`), is refused with §14/#662 before any key is
// consulted. Both are filed separately rather than widened into here.
//
// THE TEST. Every row crosses a real `--import` boundary — the key exists only
// on the reconstruction path — and runs the IDENTICAL program text in one
// module as its own oracle, so "the boundary changed the answer" is executable
// rather than asserted. The AOT harness cannot express a module dependency, so
// the rows hand-roll `Driver::compileObject` plus
// `bezel::test::compileFileForTest`, and run the product through
// `bezel::test::runProgramForTest` rather than `std::system` (#4532). No JIT
// oracle is used.
//
// ORACLE NOTE — WHY THE RUN GOES THROUGH `runProgramForTest`. Rows 5-7 pass a
// lambda to the constructor of a GENERIC class, and that leaks the 48-byte
// closure (#4534) — in the single-module twin exactly as across the boundary,
// and identically on a `bezelc` built from this batch's merge-base 7f29b8ce, so
// it is neither this fix's doing nor the boundary's. Under `gate.sh --full` a
// hand-rolled `std::system(exe)` inherited LeakSanitizer's defaults and its
// `exitcode` REPLACED the program's: all six assertions below reported 1
// instead of 105, on both sides of the boundary at once, which is precisely the
// shape that reads as "the twin oracle disagrees" and is not. The sweep's own
// run options (`detect_leaks=0:abort_on_error=0`, what every `runAot` test uses
// per #1822) leave each row asserting the value it exists to assert, while a
// real redzone error still fails the row loudly with its report.

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

struct TempFile4508 {
    std::string path;
    explicit TempFile4508(std::string p) : path(std::move(p)) {}
    ~TempFile4508() { std::remove(path.c_str()); }
};

void writeSource4508(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Distinct negative sentinels so a scaffold failure can never read as a pass;
// every row asserts an exact non-negative value, so any of these FAILS loudly.
//   -2  the library half did not build
//   -3  the compiled program did not build (the PRE-FIX result of rows 2-4)
//   -4  the program could not be run to a normal exit
constexpr int kLibBuildFailed4508 = -2;
constexpr int kAppBuildFailed4508 = -3;
constexpr int kAbnormalExit4508 = -4;

// Build `libSrc` as a `-c` library object, build `mainSrc` as an executable
// importing it, run it, return the exit code.
int buildImportAndRun4508(const std::string& libSrc, const std::string& mainSrc,
                          const char* tag) {
    const fs::path& dir = wtScratchDir(tag);
    const std::string id = unique_stem(tag);
    TempFile4508 libSrcF((dir / (id + "_lib.bzl")).string());
    TempFile4508 libObjF((dir / (id + "_lib.o")).string());
    TempFile4508 appSrcF((dir / (id + "_app.bzl")).string());
    TempFile4508 appExeF((dir / id).string());
    writeSource4508(libSrcF.path, libSrc);
    writeSource4508(appSrcF.path, mainSrc);

    bezel::Driver driver;
    if (!driver.compileObject(libSrcF.path, libObjF.path).success)
        return kLibBuildFailed4508;
    if (!std::ifstream(libObjF.path).good()) return kLibBuildFailed4508;

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
        return kAppBuildFailed4508;

    // runProgramForTest, not std::system: under `gate.sh --full` the
    // ambient environment leaves LeakSanitizer ON, and its exit code
    // then REPLACES this program's (#4532). The helper gives the run the
    // sweep's own options (detect_leaks=0:abort_on_error=0, exactly what
    // `runAot` uses per #1822) and fails the test loudly, with the report,
    // if the sanitizer actually fires.
    int status = bezel::test::runProgramForTest(appExeF.path);
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status)
                                               : kAbnormalExit4508;
}

// THE ORACLE. The same program text, compiled as ONE module. The signature key
// is consulted only when a body is reconstructed from metadata, so this path
// never reaches it and states what the language says the program means.
int buildSingleAndRun4508(const std::string& src, const char* tag) {
    const fs::path& dir = wtScratchDir(tag);
    const std::string id = unique_stem(tag);
    TempFile4508 srcF((dir / (id + "_one.bzl")).string());
    TempFile4508 exeF((dir / id).string());
    writeSource4508(srcF.path, src);

    bezel::Driver driver;
    if (!bezel::test::compileFileForTest(driver, srcF.path, exeF.path, {},
                                          codegen::CodegenOptions{})
             .success)
        return kAppBuildFailed4508;

    // runProgramForTest, not std::system: under `gate.sh --full` the
    // ambient environment leaves LeakSanitizer ON, and its exit code
    // then REPLACES this program's (#4532). The helper gives the run the
    // sweep's own options (detect_leaks=0:abort_on_error=0, exactly what
    // `runAot` uses per #1822) and fails the test loudly, with the report,
    // if the sanitizer actually fires.
    int status = bezel::test::runProgramForTest(exeF.path);
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status)
                                               : kAbnormalExit4508;
}

// Every row is built from ONE class text used verbatim in both halves, so the
// twin and the imported build cannot drift: only the module boundary differs.
std::string lib4508(const std::string& mod, const std::string& cls) {
    return "module " + mod + "Lib4508; " + cls;
}
std::string app4508(const std::string& mod, const std::string& main) {
    return "module " + mod + "App4508; import " + mod + "Lib4508; " + main;
}
std::string one4508(const std::string& mod, const std::string& cls,
                    const std::string& main) {
    return "module " + mod + "One4508; " + cls + main;
}

// ── Row 1: THE ISSUE'S REPRO — two overloads differing only at a
// function-typed parameter (§22.6 / §27.1). `lambda(x: int) => 5` has type
// `(int) -> int`, so §22.6 selects the FIRST overload and the answer is 105.
// Both parameters keyed "(?)", so the second overload's body was attached to
// the first and the imported call answered 205.
const std::string kSkClass4508 =
    "public class SkC4508<T> "
    "  public constructor() end constructor; "
    "  public method Pick(f: (int) -> int): int return 100 + f(1); end Pick; "
    "  public method Pick(g: (bool) -> int): int return 200 + g(true); end Pick; "
    "end SkC4508; ";
const std::string kSkMain4508 =
    "class SkMain4508 "
    "  public static method Main(): int "
    "    using var c := new SkC4508<int>(); "
    "    return c.Pick(lambda(x: int) => 5); "
    "  end Main; "
    "end SkMain4508; ";

// ── Row 2: a function type beside an INLINE ARRAY (§27.1 / §15.1). The two
// kinds also shared "(?)", and here the mis-attached body's `[2 x i32]`
// parameter meets a `ptr` argument: an IR-verifier failure, not a wrong value.
const std::string kIaClass4508 =
    "public class IaC4508<T> "
    "  public constructor() end constructor; "
    "  public method Pick(f: (int) -> int): int return 100 + f(1); end Pick; "
    "  public method Pick(a: [2]int): int return 200 + a[0]; end Pick; "
    "end IaC4508; ";
const std::string kIaMain4508 =
    "class IaMain4508 "
    "  public static method Main(): int "
    "    using var c := new IaC4508<int>(); "
    "    return c.Pick(lambda(x: int) => 5); "
    "  end Main; "
    "end IaMain4508; ";

// ── Row 3: two inline arrays differing only in EXTENT (§15.1). This is why the
// key carries the extent and not just the kind: `[2]int` and `[3]int` are
// distinct types, so rendering `[]int` for both would leave exactly the
// collision this fix is about.
const std::string kExClass4508 =
    "public class ExC4508<T> "
    "  public constructor() end constructor; "
    "  public method Pick(a: [2]int): int return 100 + a[0] + a[1]; end Pick; "
    "  public method Pick(b: [3]int): int return 200; end Pick; "
    "end ExC4508; ";
const std::string kExMain4508 =
    "class ExMain4508 "
    "  public static method Main(): int "
    "    using var c := new ExC4508<int>(); "
    "    return c.Pick([2, 3]); "
    "  end Main; "
    "end ExMain4508; ";

// ── Row 4: a UNIT-typed parameter (§3.1 / A.5 `TypeNoNullable ::= … | "(" ")"`).
// The failure here is the OPPOSITE of rows 1-3: both mirrors already said
// "()" while this side said "?", so the keys MISMATCHED, the constructor body
// never attached, and the importer refused the program with §14/#662.
const std::string kUnClass4508 =
    "public class UnC4508<T> "
    "  public constructor(u: ()) end constructor; "
    "  public method Pick(f: (int) -> int): int return 100 + f(1); end Pick; "
    "  public method Pick(u: ()): int return 200; end Pick; "
    "end UnC4508; ";
const std::string kUnMain4508 =
    "class UnMain4508 "
    "  public static method Main(): int "
    "    using var c := new UnC4508<int>(()); "
    "    return c.Pick(lambda(x: int) => 5); "
    "  end Main; "
    "end UnMain4508; ";

// ── Row 5: SAME SIGNATURE, DIFFERENT SPELLING — a parameter NAME inside the
// function type (§27.1, v0.306: the modifier is "part of the type" while the
// section's own table spells every function type without names). The exporter
// writes `(x: int) -> int`; the reconstruction stub drops the name. A key that
// rendered names would key "(x:int)->int" against the stub's "(int)->int" and
// attachment would stop — silently, with no oracle but this row.
const std::string kNmClass4508 =
    "public class NmC4508<T> "
    "  public Tag: int; "
    "  public constructor(f: (x: int) -> int) Tag := Use(f); end constructor; "
    "  public method Use(f: (x: int) -> int): int return 100 + f(1); end Use; "
    "  public method Use(g: (y: bool) -> int): int return 200; end Use; "
    "end NmC4508; ";
const std::string kNmMain4508 =
    "class NmMain4508 "
    "  public static method Main(): int "
    "    using var c := new NmC4508<int>(lambda(v: int) => 5); "
    "    return c.Tag; "
    "  end Main; "
    "end NmMain4508; ";

// ── Row 6: SAME SIGNATURE, DIFFERENT SPELLING — a `ref` MODIFIER and a `()`
// return (§27.1: "`(T) -> ()` and `(owned T) -> ()` are distinct function
// types", so the modifier must be rendered; A.5's unit spelling must survive
// nested inside a function type). The decoy differs from the selected overload
// ONLY by that modifier, so a key that dropped it would answer 200.
const std::string kRfClass4508 =
    "public class RfC4508<T> "
    "  public Tag: int; "
    "  public constructor(f: (ref int) -> ()) Tag := Use(f); end constructor; "
    "  public method Use(f: (ref int) -> ()): int "
    "    var n: int := 104; f(ref n); return n; "
    "  end Use; "
    "  public method Use(g: (int) -> ()): int return 200; end Use; "
    "end RfC4508; ";
const std::string kRfMain4508 =
    "class RfMain4508 "
    "  public static method Main(): int "
    "    using var c := new RfC4508<int>(lambda(ref v) => do v := v + 1; end); "
    "    return c.Tag; "
    "  end Main; "
    "end RfMain4508; ";

// ── Row 7: SAME SIGNATURE, DIFFERENT SPELLING — a NESTED function type
// (§27.1). The renderer recurses on all three sides; this row is what catches
// one of them recursing differently (or bottoming out at the old "?").
const std::string kNsClass4508 =
    "public class NsC4508<T> "
    "  public Tag: int; "
    "  public constructor(f: ((int) -> int) -> int) Tag := Use(f); end constructor; "
    "  public method Use(f: ((int) -> int) -> int): int "
    "    return 100 + f(lambda(x: int) => 5); "
    "  end Use; "
    "  public method Use(g: (int) -> int): int return 200; end Use; "
    "end NsC4508; ";
const std::string kNsMain4508 =
    "class NsMain4508 "
    "  public static method Main(): int "
    "    using var c := new NsC4508<int>(lambda(h: (int) -> int) => h(1)); "
    "    return c.Tag; "
    "  end Main; "
    "end NsMain4508; ";

}  // namespace

// ===========================================================================
// Row 1 — THE BUG (§22.6 / §27.1, v0.306). Pre-fix the imported call answered
// 205, the OTHER overload's body, while the identical single-module program
// answered 105.
// ===========================================================================
BEZEL_TEST(xmod_sig_key_kind_collision_4508,
           function_typed_overloads_attach_their_own_bodies_spec_22_6) {
    BEZEL_EXPECT_EQ(105, buildSingleAndRun4508(
                             one4508("Sk", kSkClass4508, kSkMain4508),
                             "wt-4508-sk-one"));
    BEZEL_EXPECT_EQ(105, buildImportAndRun4508(
                             lib4508("Sk", kSkClass4508),
                             app4508("Sk", kSkMain4508), "wt-4508-sk-xmod"));
}

// ===========================================================================
// Row 2 — a function type beside an inline array (§27.1 / §15.1, v0.306).
// Pre-fix the importer emitted malformed IR and the build failed.
// ===========================================================================
BEZEL_TEST(xmod_sig_key_kind_collision_4508,
           function_and_inline_array_overloads_are_distinguished_spec_15_1) {
    BEZEL_EXPECT_EQ(105, buildSingleAndRun4508(
                             one4508("Ia", kIaClass4508, kIaMain4508),
                             "wt-4508-ia-one"));
    BEZEL_EXPECT_EQ(105, buildImportAndRun4508(
                             lib4508("Ia", kIaClass4508),
                             app4508("Ia", kIaMain4508), "wt-4508-ia-xmod"));
}

// ===========================================================================
// Row 3 — two inline arrays differing only in extent (§15.1, v0.306). Pins the
// extent as PART of the key; rendering only the kind would leave this pair
// colliding exactly as before.
// ===========================================================================
BEZEL_TEST(xmod_sig_key_kind_collision_4508,
           inline_array_overloads_differing_in_extent_spec_15_1) {
    BEZEL_EXPECT_EQ(105, buildSingleAndRun4508(
                             one4508("Ex", kExClass4508, kExMain4508),
                             "wt-4508-ex-one"));
    BEZEL_EXPECT_EQ(105, buildImportAndRun4508(
                             lib4508("Ex", kExClass4508),
                             app4508("Ex", kExMain4508), "wt-4508-ex-xmod"));
}

// ===========================================================================
// Row 4 — a unit-typed parameter (§3.1 / A.5, v0.306). The opposite failure:
// pre-fix the keys MISMATCHED, so the constructor body never attached and the
// importer refused the program (§14/#662).
// ===========================================================================
BEZEL_TEST(xmod_sig_key_kind_collision_4508,
           unit_typed_parameter_matches_its_mirror_spec_3_1) {
    BEZEL_EXPECT_EQ(105, buildSingleAndRun4508(
                             one4508("Un", kUnClass4508, kUnMain4508),
                             "wt-4508-un-one"));
    BEZEL_EXPECT_EQ(105, buildImportAndRun4508(
                             lib4508("Un", kUnClass4508),
                             app4508("Un", kUnMain4508), "wt-4508-un-xmod"));
}

// ===========================================================================
// Row 5 — the no-oracle direction: a function-type parameter NAME, which the
// reconstruction stub drops (§27.1, v0.306). Guards against a key that starts
// rendering names and silently stops matching.
// ===========================================================================
BEZEL_TEST(xmod_sig_key_kind_collision_4508,
           function_type_parameter_name_does_not_change_the_key_spec_27_1) {
    BEZEL_EXPECT_EQ(105, buildSingleAndRun4508(
                             one4508("Nm", kNmClass4508, kNmMain4508),
                             "wt-4508-nm-one"));
    BEZEL_EXPECT_EQ(105, buildImportAndRun4508(
                             lib4508("Nm", kNmClass4508),
                             app4508("Nm", kNmMain4508), "wt-4508-nm-xmod"));
}

// ===========================================================================
// Row 6 — the no-oracle direction: a `ref` modifier and a `()` return, which
// the stub KEEPS (§27.1, v0.306). The decoy differs only by that modifier, so
// this row fails both ways — a key that drops it answers 200, a key that
// renders it differently on one side stops matching.
// ===========================================================================
BEZEL_TEST(xmod_sig_key_kind_collision_4508,
           function_type_modifier_and_unit_return_keep_matching_spec_27_1) {
    BEZEL_EXPECT_EQ(105, buildSingleAndRun4508(
                             one4508("Rf", kRfClass4508, kRfMain4508),
                             "wt-4508-rf-one"));
    BEZEL_EXPECT_EQ(105, buildImportAndRun4508(
                             lib4508("Rf", kRfClass4508),
                             app4508("Rf", kRfMain4508), "wt-4508-rf-xmod"));
}

// ===========================================================================
// Row 7 — the no-oracle direction: a NESTED function type (§27.1, v0.306).
// All three renderers recurse; this row catches one of them recursing
// differently or bottoming out at the old placeholder.
// ===========================================================================
BEZEL_TEST(xmod_sig_key_kind_collision_4508,
           nested_function_type_keeps_matching_spec_27_1) {
    BEZEL_EXPECT_EQ(105, buildSingleAndRun4508(
                             one4508("Ns", kNsClass4508, kNsMain4508),
                             "wt-4508-ns-one"));
    BEZEL_EXPECT_EQ(105, buildImportAndRun4508(
                             lib4508("Ns", kNsClass4508),
                             app4508("Ns", kNsMain4508), "wt-4508-ns-xmod"));
}

}  // namespace bezel::module
