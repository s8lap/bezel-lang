// Regression tests for #4516 — §7.1 / §7.2 / §22.6 / §14 (v0.306): the
// §14/#705 signature key must render NULLABILITY, on every side of the
// comparison, and the synthetic reconstruction source must be able to spell it.
//
// THE SPEC. §22.6 opens: "Methods and constructors may be overloaded by
// parameter list. Overload resolution considers number, TYPES, the `ref`/`out`
// modifiers, and the presence of `owned` on an ordinary parameter — overloads
// differing in any of these are distinguishable", and closes with the same
// list ("arity, parameter types, the `ref`/`out` modifiers, and ordinary
// `owned` are the distinguishing facts"). §7.1: "`string` and `string?` are
// distinct types. The compiler enforces the distinction at every assignment,
// parameter pass, and return." §14.2 says the same of a value type ("a
// nullable value type is a distinct type from its non-nullable form (§7.1)").
// So a pair differing only in nullability differs in TYPES and §22.6 admits
// it — the right answer is a richer key, not a diagnostic. MEASURED: a single
// module accepts `Pick(a: T)` beside `Pick(b: T?)` and resolves the call to
// the exact match.
//
// THE DEFECT, in two halves.
//
//   (a) `metaTypeSigText` / `astTypeToSigText` / `typeRefSigText`
//       (src/module/ModuleLoader.cpp) all ended their Named and Primitive arms
//       with "Nullable omitted intentionally", so `T` and `T?` shared one key.
//       `importedMethods[metaMemberSigKey(…)] = …` is an ASSIGNMENT and the
//       ctor/method attachment loops `break` on the first key match, so the
//       exporter's body attached to the WRONG imported overload — a SILENT
//       wrong value across a module boundary, which neither gate oracle
//       catches (it is not a leak and not a crash).
//
//   (b) `metaTypeToSourceText`'s PRIMITIVE arm dropped the `?` as well (its
//       Named and Function arms never did), so the synthetic reconstruction
//       stub replayed `x: int?` as `x: int`. Two overloads differing only in
//       primitive nullability therefore rendered two IDENTICAL stubs — a
//       duplicate-member error that takes the whole reconstruction down — and
//       the importer REJECTED a program the exporter compiled (§14/#662).
//       That arm is why the key could not simply start rendering `?`: the AST
//       side re-parses THIS text, so all four renderers move together.
//
// MEASURED ACROSS THE FIX BOUNDARY (real `bezelc`, a `-c` library plus an
// importing executable; every row's single-module twin is the oracle and is
// 105 on both sides of the fix):
//   row 1  `((int) -> int)?` vs `(int) -> int`   pre-fix 200 — the DECOY's body
//   row 2  `SC<T>?` vs `SC<T>`                   pre-fix 205 — the other body
//   row 3  `int?` vs `int`                       pre-fix §14/#662 rejection
//   row 4  `string?` vs `string`                 pre-fix §14/#662 rejection
//   row 5  `()?` vs `()`                         pre-fix §14/#662 rejection
//   row 6  `T?` vs `T`                           pre-fix "Call parameter type
//                                                 does not match function
//                                                 signature!" (IR verifier)
//   row 7  `(int?) -> int` vs `(int) -> int`     pre-fix §14/#662 rejection
//   row 8  `[2]int?` vs `[2]int`                 pre-fix §14/#662 rejection
//   row 9  the surface control — passes on BOTH sides (see its header)
//
// WHY AN INLINE ARRAY GETS NO `?`. A.5 is `Type ::= TypeNoNullable "?"?` and
// `([2]int)?` does not parse (MEASURED: "expected '->' after function-type
// parameter list"), so an inline array has no nullable form and `nullable` can
// never be true for one. Row 8 pins the element's `?` and, by answering 105
// rather than the decoy's 200, that the array itself did not acquire one.
//
// THE DIRECTION WITH NO ORACLE. Making two DIFFERENT signatures differ is what
// rows 1-8 pin. Making the SAME signature agree on both sides is the defect
// this fix could have introduced, and nothing in the gate would catch it:
// every row is built so a key that drifted on either side stops attachment and
// yields the decoy's 200 or a §14/#662 rejection instead of 105. The full
// sweep behind these rows is 20 shapes run cross-module AND as a single-module
// twin on both compilers; pre-fix 6 OK / 14 drifted, post-fix 18 OK / 2. The
// two residual drifts fail IDENTICALLY on both compilers and are therefore
// pre-existing, filed separately rather than widened into here: passing a
// nullable self-typed argument to an imported constructor is refused with
// §5.1 where the twin accepts it, and a static `int?` field with an
// initializer reads null in a SINGLE module (the imported build now AGREES
// with that twin, where pre-fix it disagreed).
//
// THE TEST. Every row crosses a real `--import` boundary — the key exists only
// on the reconstruction path — and runs the IDENTICAL program text in one
// module as its own oracle, so "the boundary changed the answer" is executable
// rather than asserted. The AOT harness cannot express a module dependency, so
// the rows hand-roll `Driver::compileObject` plus
// `bezel::test::compileFileForTest`, and run the product through
// `bezel::test::runProgramForTest` rather than `std::system` (#4532: under
// `gate.sh --full` LeakSanitizer's exit code otherwise REPLACES the program's
// and every row reports 1 on both sides of the boundary at once). No JIT
// oracle and no skip guard.

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

struct TempFile4516 {
    std::string path;
    explicit TempFile4516(std::string p) : path(std::move(p)) {}
    ~TempFile4516() { std::remove(path.c_str()); }
};

void writeSource4516(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Distinct negative sentinels so a scaffold failure can never read as a pass;
// every row asserts an exact non-negative value, so any of these FAILS loudly.
//   -2  the library half did not build
//   -3  the compiled program did not build (the PRE-FIX result of rows 3-8)
//   -4  the program could not be run to a normal exit
constexpr int kLibBuildFailed4516 = -2;
constexpr int kAppBuildFailed4516 = -3;
constexpr int kAbnormalExit4516 = -4;

// Build `libSrc` as a `-c` library object, build `mainSrc` as an executable
// importing it, run it, return the exit code.
int buildImportAndRun4516(const std::string& libSrc, const std::string& mainSrc,
                          const char* tag) {
    const fs::path& dir = wtScratchDir(tag);
    const std::string id = unique_stem(tag);
    TempFile4516 libSrcF((dir / (id + "_lib.bzl")).string());
    TempFile4516 libObjF((dir / (id + "_lib.o")).string());
    TempFile4516 appSrcF((dir / (id + "_app.bzl")).string());
    TempFile4516 appExeF((dir / id).string());
    writeSource4516(libSrcF.path, libSrc);
    writeSource4516(appSrcF.path, mainSrc);

    bezel::Driver driver;
    if (!driver.compileObject(libSrcF.path, libObjF.path).success)
        return kLibBuildFailed4516;
    if (!std::ifstream(libObjF.path).good()) return kLibBuildFailed4516;

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
        return kAppBuildFailed4516;

    int status = bezel::test::runProgramForTest(appExeF.path);
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status)
                                               : kAbnormalExit4516;
}

// THE ORACLE. The same program text, compiled as ONE module. The signature key
// is consulted only when a body is reconstructed from metadata, so this path
// never reaches it and states what the language says the program means.
int buildSingleAndRun4516(const std::string& src, const char* tag) {
    const fs::path& dir = wtScratchDir(tag);
    const std::string id = unique_stem(tag);
    TempFile4516 srcF((dir / (id + "_one.bzl")).string());
    TempFile4516 exeF((dir / id).string());
    writeSource4516(srcF.path, src);

    bezel::Driver driver;
    if (!bezel::test::compileFileForTest(driver, srcF.path, exeF.path, {},
                                          codegen::CodegenOptions{})
             .success)
        return kAppBuildFailed4516;

    int status = bezel::test::runProgramForTest(exeF.path);
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status)
                                               : kAbnormalExit4516;
}

// Every row is built from ONE class text used verbatim in both halves, so the
// twin and the imported build cannot drift: only the module boundary differs.
std::string lib4516(const std::string& mod, const std::string& cls) {
    return "module " + mod + "Lib4516; " + cls;
}
std::string app4516(const std::string& mod, const std::string& main) {
    return "module " + mod + "App4516; import " + mod + "Lib4516; " + main;
}
std::string one4516(const std::string& mod, const std::string& cls,
                    const std::string& main) {
    return "module " + mod + "One4516; " + cls + main;
}

// ── Row 1: THE ISSUE'S HEADLINE — `((int) -> int)?` beside `(int) -> int`
// (§27.2's parenthesised nullable form of a §27.1 function type). Both keyed
// "(int)->int", so the DECOY's body was attached and the imported call
// answered 200 where the twin answers 105.
const std::string kFnClass4516 =
    "public class FnC4516<T> "
    "  public Tag: int; "
    "  public constructor(f: ((int) -> int)?) Tag := Use(f); end constructor; "
    "  public method Use(f: ((int) -> int)?): int return 105; end Use; "
    "  public method Use(g: (int) -> int): int return 200; end Use; "
    "end FnC4516; ";
const std::string kFnMain4516 =
    "class FnMain4516 "
    "  public static method Main(): int "
    "    var q: ((int) -> int)? := null; "
    "    using var c := new FnC4516<int>(q); "
    "    return c.Tag; "
    "  end Main; "
    "end FnMain4516; ";

// ── Row 2: a NAMED type and its nullable form (§7.1: "`string` and `string?`
// are distinct types"). The self type is used so the #2842 stub-safety rule
// (a stub naming a non-self Named type is skipped) does not mask the row. The
// call selects the NON-nullable member, so the pre-fix answer 205 is the other
// overload's body arriving through method-body attachment.
const std::string kNmClass4516 =
    "public class NmC4516<T> "
    "  public constructor() end constructor; "
    "  public method Pick(a: NmC4516<T>): int return 100; end Pick; "
    "  public method Pick(b: NmC4516<T>?): int return 200; end Pick; "
    "end NmC4516; ";
const std::string kNmMain4516 =
    "class NmMain4516 "
    "  public static method Main(): int "
    "    using var c := new NmC4516<int>(); "
    "    using var d := new NmC4516<int>(); "
    "    return c.Pick(d) + 5; "
    "  end Main; "
    "end NmMain4516; ";

// ── Row 3: `int?` beside `int` (§7.2 nullable value types). This is half (b):
// pre-fix BOTH stubs rendered `int`, the synthetic source declared the same
// method twice, and the importer refused the program with §14/#662.
const std::string kInClass4516 =
    "public class InC4516<T> "
    "  public Tag: int; "
    "  public constructor(x: int?) Tag := Use(x); end constructor; "
    "  public method Use(a: int?): int return 105; end Use; "
    "  public method Use(b: int): int return 200; end Use; "
    "end InC4516; ";
const std::string kInMain4516 =
    "class InMain4516 "
    "  public static method Main(): int "
    "    var q: int? := null; "
    "    using var c := new InC4516<int>(q); "
    "    return c.Tag; "
    "  end Main; "
    "end InMain4516; ";

// ── Row 4: `string?` beside `string` — §7.1's own example, and a PrimitiveKind
// on the wire rather than a Named one, so it takes the same arm row 3 does.
const std::string kStClass4516 =
    "public class StC4516<T> "
    "  public Tag: int; "
    "  public constructor(x: string?) Tag := Use(x); end constructor; "
    "  public method Use(a: string?): int return 105; end Use; "
    "  public method Use(b: string): int return 200; end Use; "
    "end StC4516; ";
const std::string kStMain4516 =
    "class StMain4516 "
    "  public static method Main(): int "
    "    var q: string? := null; "
    "    using var c := new StC4516<int>(q); "
    "    return c.Tag; "
    "  end Main; "
    "end StMain4516; ";

// ── Row 5: `()?` beside `()` — A.5's `Type ::= TypeNoNullable "?"?` over the
// unit type of §3.1. #4508 gave Unit its own key text; this row pins that the
// nullable form is a DIFFERENT key and that the stub can spell it.
const std::string kUnClass4516 =
    "public class UnC4516<T> "
    "  public Tag: int; "
    "  public constructor(u: ()?) Tag := Use(u); end constructor; "
    "  public method Use(a: ()?): int return 105; end Use; "
    "  public method Use(b: ()): int return 200; end Use; "
    "end UnC4516; ";
const std::string kUnMain4516 =
    "class UnMain4516 "
    "  public static method Main(): int "
    "    var q: ()? := null; "
    "    using var c := new UnC4516<int>(q); "
    "    return c.Tag; "
    "  end Main; "
    "end UnMain4516; ";

// ── Row 6: `T?` beside `T` on the class's own type parameter (§7.10). Pre-fix
// this one did not merely answer the decoy — the mis-attached body's parameter
// type met the wrong argument and the IR verifier refused the module with
// "Call parameter type does not match function signature!".
const std::string kTpClass4516 =
    "public class TpC4516<T> "
    "  public Tag: int; "
    "  public constructor(x: T?) Tag := Use(x); end constructor; "
    "  public method Use(a: T?): int return 105; end Use; "
    "  public method Use(b: T): int return 200; end Use; "
    "end TpC4516; ";
const std::string kTpMain4516 =
    "class TpMain4516 "
    "  public static method Main(): int "
    "    var q: int? := null; "
    "    using var c := new TpC4516<int>(q); "
    "    return c.Tag; "
    "  end Main; "
    "end TpMain4516; ";

// ── Row 7: nullability NESTED inside a function type — `(int?) -> int` beside
// `(int) -> int` (§27.1). The renderers recurse, and this row is what catches
// one of them recursing past the nullability wrapper on any of the four sides.
const std::string kNsClass4516 =
    "public class NsC4516<T> "
    "  public Tag: int; "
    "  public constructor(f: (int?) -> int) Tag := Use(f); end constructor; "
    "  public method Use(f: (int?) -> int): int return 105; end Use; "
    "  public method Use(g: (int) -> int): int return 200; end Use; "
    "end NsC4516; ";
const std::string kNsMain4516 =
    "class NsMain4516 "
    "  public static method Main(): int "
    "    using var c := new NsC4516<int>(lambda(x: int?) => 1); "
    "    return c.Tag; "
    "  end Main; "
    "end NsMain4516; ";

// ── Row 8: an inline array's ELEMENT nullability — `[2]int?` beside `[2]int`
// (§15.1 / A.5). Two things at once: the element's `?` must reach the key on
// every side, and the ARRAY must not acquire one — A.5 gives an inline array no
// nullable form, so a renderer that appended `?` to the array itself would key
// a spelling no other side can produce and attachment would stop.
const std::string kIaClass4516 =
    "public class IaC4516<T> "
    "  public Tag: int; "
    "  public constructor(a: [2]int?) Tag := Use(a); end constructor; "
    "  public method Use(a: [2]int?): int return 105; end Use; "
    "  public method Use(b: [2]int): int return 200; end Use; "
    "end IaC4516; ";
const std::string kIaMain4516 =
    "class IaMain4516 "
    "  public static method Main(): int "
    "    var z: int? := null; "
    "    using var c := new IaC4516<int>([z, z]); "
    "    return c.Tag; "
    "  end Main; "
    "end IaMain4516; ";

// ── Row 9: THE SURFACE CONTROL. Unlike rows 1-8 this row passes on BOTH sides
// of the fix — it is not a pin on the defect but a guard on its BLAST RADIUS.
// `metaTypeToSourceText` is the synthetic RECONSTRUCTION SOURCE renderer, used
// for every field, property, indexer, record component, parameter and return
// in a stub, and half (b) of this fix widened it: a nullable primitive now
// reaches the stub as `int?`/`string?` where it used to reach it as
// `int`/`string`. This row puts a nullable primitive at four of those surfaces
// at once — an indexer's parameter AND its return, a record's positional
// component, a field, and a sibling method's return — while the reconstruction
// that actually matters (a constructor body calling an overloaded method) runs
// beside them. Its answer is 105 before and after; it fails if the widened
// source text ever stops parsing or stops typechecking.
const std::string kSfClass4516 =
    "public record SfR4516(Name: string?, N: int); "
    "public class SfC4516<T> "
    "  public Tag: int; "
    "  public F: string? := null; "
    "  public property [i: int?] : string? => null; "
    "  public constructor(f: (int) -> int) Tag := Use(f); end constructor; "
    "  public method Sib(n: int): int? return null; end Sib; "
    "  public method Use(f: (int) -> int): int return 105; end Use; "
    "  public method Use(g: (bool) -> int): int return 200; end Use; "
    "end SfC4516; ";
const std::string kSfMain4516 =
    "class SfMain4516 "
    "  public static method Main(): int "
    "    var r := new SfR4516(null, 1); "
    "    using var c := new SfC4516<int>(lambda(x: int) => 1); "
    "    return c.Tag + r.N - 1; "
    "  end Main; "
    "end SfMain4516; ";

}  // namespace

// ===========================================================================
// Row 1 — THE ISSUE'S HEADLINE (§27.1 / §27.2 / §22.6, v0.306). Pre-fix the
// imported call answered 200, the DECOY overload's body, while the identical
// single-module program answered 105.
// ===========================================================================
BEZEL_TEST(xmod_sig_key_nullability_4516,
           nullable_function_type_is_a_distinct_overload_key_spec_27_2) {
    BEZEL_EXPECT_EQ(105, buildSingleAndRun4516(
                             one4516("Fn", kFnClass4516, kFnMain4516),
                             "wt-4516-fn-one"));
    BEZEL_EXPECT_EQ(105, buildImportAndRun4516(
                             lib4516("Fn", kFnClass4516),
                             app4516("Fn", kFnMain4516), "wt-4516-fn-xmod"));
}

// ===========================================================================
// Row 2 — a Named type and its nullable form (§7.1, v0.306). Pre-fix 205: the
// other overload's body, attached through the importedMethods map.
// ===========================================================================
BEZEL_TEST(xmod_sig_key_nullability_4516,
           nullable_named_type_is_a_distinct_overload_key_spec_7_1) {
    BEZEL_EXPECT_EQ(105, buildSingleAndRun4516(
                             one4516("Nm", kNmClass4516, kNmMain4516),
                             "wt-4516-nm-one"));
    BEZEL_EXPECT_EQ(105, buildImportAndRun4516(
                             lib4516("Nm", kNmClass4516),
                             app4516("Nm", kNmMain4516), "wt-4516-nm-xmod"));
}

// ===========================================================================
// Row 3 — `int?` beside `int` (§7.2, v0.306). Half (b): pre-fix the stub could
// not spell the `?`, the two stubs collided, and the importer REFUSED a
// program the exporter compiled (§14/#662).
// ===========================================================================
BEZEL_TEST(xmod_sig_key_nullability_4516,
           nullable_primitive_reaches_the_stub_and_the_key_spec_7_2) {
    BEZEL_EXPECT_EQ(105, buildSingleAndRun4516(
                             one4516("In", kInClass4516, kInMain4516),
                             "wt-4516-in-one"));
    BEZEL_EXPECT_EQ(105, buildImportAndRun4516(
                             lib4516("In", kInClass4516),
                             app4516("In", kInMain4516), "wt-4516-in-xmod"));
}

// ===========================================================================
// Row 4 — `string?` beside `string`, §7.1's own example (v0.306).
// ===========================================================================
BEZEL_TEST(xmod_sig_key_nullability_4516,
           nullable_string_is_a_distinct_overload_key_spec_7_1) {
    BEZEL_EXPECT_EQ(105, buildSingleAndRun4516(
                             one4516("St", kStClass4516, kStMain4516),
                             "wt-4516-st-one"));
    BEZEL_EXPECT_EQ(105, buildImportAndRun4516(
                             lib4516("St", kStClass4516),
                             app4516("St", kStMain4516), "wt-4516-st-xmod"));
}

// ===========================================================================
// Row 5 — `()?` beside `()` (§3.1 / A.5, v0.306).
// ===========================================================================
BEZEL_TEST(xmod_sig_key_nullability_4516,
           nullable_unit_type_is_a_distinct_overload_key_spec_3_1) {
    BEZEL_EXPECT_EQ(105, buildSingleAndRun4516(
                             one4516("Un", kUnClass4516, kUnMain4516),
                             "wt-4516-un-one"));
    BEZEL_EXPECT_EQ(105, buildImportAndRun4516(
                             lib4516("Un", kUnClass4516),
                             app4516("Un", kUnMain4516), "wt-4516-un-xmod"));
}

// ===========================================================================
// Row 6 — `T?` beside `T` on the class's own type parameter (§7.10, v0.306).
// Pre-fix this produced malformed IR, not a wrong value.
// ===========================================================================
BEZEL_TEST(xmod_sig_key_nullability_4516,
           nullable_type_parameter_is_a_distinct_overload_key_spec_7_10) {
    BEZEL_EXPECT_EQ(105, buildSingleAndRun4516(
                             one4516("Tp", kTpClass4516, kTpMain4516),
                             "wt-4516-tp-one"));
    BEZEL_EXPECT_EQ(105, buildImportAndRun4516(
                             lib4516("Tp", kTpClass4516),
                             app4516("Tp", kTpMain4516), "wt-4516-tp-xmod"));
}

// ===========================================================================
// Row 7 — nullability NESTED inside a function type (§27.1, v0.306). Catches a
// renderer that recurses past the nullability wrapper.
// ===========================================================================
BEZEL_TEST(xmod_sig_key_nullability_4516,
           nullable_inside_a_function_type_reaches_the_key_spec_27_1) {
    BEZEL_EXPECT_EQ(105, buildSingleAndRun4516(
                             one4516("Ns", kNsClass4516, kNsMain4516),
                             "wt-4516-ns-one"));
    BEZEL_EXPECT_EQ(105, buildImportAndRun4516(
                             lib4516("Ns", kNsClass4516),
                             app4516("Ns", kNsMain4516), "wt-4516-ns-xmod"));
}

// ===========================================================================
// Row 8 — an inline array's ELEMENT nullability (§15.1 / A.5, v0.306). Also
// pins that the ARRAY itself gets no `?`: A.5 gives it no nullable form.
// ===========================================================================
BEZEL_TEST(xmod_sig_key_nullability_4516,
           inline_array_element_nullability_reaches_the_key_spec_15_1) {
    BEZEL_EXPECT_EQ(105, buildSingleAndRun4516(
                             one4516("Ia", kIaClass4516, kIaMain4516),
                             "wt-4516-ia-one"));
    BEZEL_EXPECT_EQ(105, buildImportAndRun4516(
                             lib4516("Ia", kIaClass4516),
                             app4516("Ia", kIaMain4516), "wt-4516-ia-xmod"));
}

// ===========================================================================
// Row 9 — THE SURFACE CONTROL (§22.8 / §24 / §9.1, v0.306). Passes on BOTH
// sides of the fix by design: it guards the widened `metaTypeToSourceText`
// surface (an indexer's parameter and return, a record positional component, a
// field, a sibling method's return — all nullable) rather than pinning the
// defect. See its header above.
// ===========================================================================
BEZEL_TEST(xmod_sig_key_nullability_4516,
           nullable_stub_surfaces_still_reconstruct_spec_22_8) {
    BEZEL_EXPECT_EQ(105, buildSingleAndRun4516(
                             one4516("Sf", kSfClass4516, kSfMain4516),
                             "wt-4516-sf-one"));
    BEZEL_EXPECT_EQ(105, buildImportAndRun4516(
                             lib4516("Sf", kSfClass4516),
                             app4516("Sf", kSfMain4516), "wt-4516-sf-xmod"));
}

}  // namespace bezel::module
