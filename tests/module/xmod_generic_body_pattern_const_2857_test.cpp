// fix #2857 — a compile-time-const LOCAL used as a `case` pattern constant in an
// IMPORTED generic body fails to compile (§19.3 Patterns / #1853, §17.14
// compile-time classification, §2.8.2 The artifact interface is a semantic
// requirement, §14.1 Generic declarations, §2.5 Dependency resolution — v0.240).
//
// §2.8.2 clause 2 is the governing rule, verbatim:
//
//   "**Instantiate** every public generic type and generic method at arbitrary
//    module-external type arguments satisfying the declared constraints. An
//    external instantiation behaves identically to a same-module instantiation
//    of the same generic at the same arguments."
//
// ModuleLoader re-parses an imported generic body's exported source in a FRESH
// SemaContext and carries the result into the importer's context. It carried
// FOUR of the five AST-keyed side tables: allExprSymbols() (remapped),
// allResolvedTypes() and allExprTypes() (verbatim, #783), and — since #2839 —
// allLambdaParams(). `patternSymbols_` was the fifth, and nothing copied it.
//
// sema writes that table (ExprTypecheck.cpp) for exactly one shape: a bare-name
// `NamedPattern` that resolves to a compile-time-classified LOCAL `const` used
// as a `case` pattern constant. A Pattern is not an Expr, so `exprSymbols_`
// cannot key it. codegen reads it in `constValueForPatternName` to fold the
// const's value at the case site; with no entry in the importer's context it got
// null and REJECTED the arm outright.
//
// MEASURED on the parent commit, real two-module build:
//     app.bzl:8:7: the `case`/`when` pattern `K` is not supported by the backend:
//     it did not resolve to a type, enum constant, or named constant to match
//     against (§19.3)
//   -> bezelc rc=1, NO binary produced.
// The identical declarations compiled as ONE module returned 105.
//
// ⚠ Unlike its two silent siblings (#2818 — every local reads 0; #2839 — the
// lambda's value is lost) this defect fails LOUDLY. That does not make it
// benign: it is still a §2.8.2 clause 2 divergence, and a program that compiles
// as one module must not fail to compile through the artifact boundary.
//
// ⚠ THE VACUITY TRAP THIS SUITE IS BUILT AROUND — and it is a real one.
// A bare-name pattern has a second possible reading: a *binding* pattern that
// matches ANY scrutinee and binds it to the name. Under that reading the arm
// fires unconditionally. So a test that only asserts `Pick(9) == 105` proves
// nothing — it passes whether `K` folded to the constant 9 or was treated as a
// catch-all. EVERY path below therefore also asserts a NON-MATCHING scrutinee
// (`Pick(3) == 102`), which is the assertion that distinguishes the two, and
// which a catch-all reading fails. Do not delete the non-matching cases.
//
// ⚠ A second vacuity guard, inherited from #2839/#2818: when reconstruction
// declines, a call can fall back to the EXPORTER's compiled function and return
// the right answer by accident. Every generic class here carries a `T`-typed
// field FIRST and is probed by an INSTANCE call, so #2821's layout gate rejects
// an unreconstructed body at compile time rather than silently falling back.
// Do not remove the leading `T` field.
//
// SCOPE, stated rather than implied. The fix carries `patternSymbols_` in all
// four reconstruction paths. Three are EXERCISED here (method, constructor,
// destructor). The fourth — the static field initializer path — is carried for
// consistency but has NO KNOWN REACHABLE SHAPE: `setPatternSymbol` fires only
// for a compile-time-const `LocalSymbol`, and a field initializer is an
// expression with no local declarations in scope. It is carried anyway so the
// four paths cannot drift apart, and so the shape is already handled if a future
// revision makes locals reachable there. This is recorded in
// semacontext_ast_side_table_snapshot_test.cpp rather than left to be rediscovered.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "test_harness.hpp"
#include "support/unique_stem.hpp"


namespace bezel::module {
namespace {

// Returned when the DEPENDENCY (exporter) object could not be produced. Every
// call site asserts `!= kDepFailed` before using the result.
//
// ⚠ WHY THAT ASSERTION EXISTS. The sibling suites treat kDepFailed as a silent
// skip, on the reasoning that a failed dependency build is a toolchain problem
// and never the defect under test. That is true — but it also means a test whose
// EXPORTER SOURCE IS MALFORMED passes green while exercising nothing. That is
// not hypothetical: T10 below was written with `enum Color Red, Green, Blue end`
// (missing the `;` the grammar requires after the constant list), its exporter
// failed to compile, and the case reported OK having tested absolutely nothing.
// A broken exporter is now RED. Do not weaken this back to a bare early return.
constexpr int kDepFailed = -2;

struct TempFile2857 {
    std::string path;
    explicit TempFile2857(const std::string& p) : path(p) {}
    ~TempFile2857() { std::remove(path.c_str()); }
};

void writeSource2857(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj2857(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

int compileImportAndRun2857(const std::string& bSrc, const std::string& bExe,
                            const std::string& aObj) {
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj;
    d.objectPath = aObj;
    deps.push_back(d);
    // A COMPILE FAILURE is this defect's symptom, so -1 here is the red state,
    // not a harness problem. kDepFailed (-2) is reserved for the dependency
    // build, which is never the thing under test.
    if (!driver.compileFile(bSrc, bExe, deps).success) return -1;
    int status = std::system(bExe.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

int runXmod2857(const std::string& stem, const std::string& aSrcText,
                const std::string& bSrcText) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string tag = stem + "_" + unique_stem("");
    TempFile2857 aSrc((dir / (tag + "_a.bzl")).string());
    TempFile2857 aObj((dir / (tag + "_a.o")).string());
    writeSource2857(aSrc.path, aSrcText);
    if (!compileToObj2857(aSrc.path, aObj.path)) return kDepFailed;
    if (!std::ifstream(aObj.path).good()) return kDepFailed;
    TempFile2857 bSrc((dir / (tag + "_b.bzl")).string());
    TempFile2857 bExe((dir / (tag + "_b")).string());
    writeSource2857(bSrc.path, bSrcText);
    return compileImportAndRun2857(bSrc.path, bExe.path, aObj.path);
}

// Compile ONE source as a single module and run it — the §2.8.2 clause 2
// control that defines what the cross-module answer MUST equal.
int runSingle2857(const std::string& stem, const std::string& srcText) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string tag = stem + "_" + unique_stem("");
    TempFile2857 src((dir / (tag + ".bzl")).string());
    TempFile2857 exe((dir / (tag + "_x")).string());
    writeSource2857(src.path, srcText);
    bezel::Driver driver;
    if (!driver.compileFile(src.path, exe.path, {}).success) return -1;
    int status = std::system(exe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// --- METHOD path (parseAndAttachMethodBody, §14.1/#783) --------------------

std::string methodLib2857(const std::string& mod) {
    return "module " + mod + "; "
           "public class Box<T> "
           "  public v: T; "                       // vacuity guard (a): layout
           "  public method Pick(sel: int): int "
           "    const K := 9; "
           "    var r := 0; "
           "    case sel of "
           "      K => r := 5, "
           "      _ => r := 2 "
           "    end; "
           "    return 100 + r; "
           "  end Pick; "
           "end Box;";
}

std::string methodApp2857(const std::string& mod, int arg) {
    return "module Main; import " + mod + "; "
           "class C "
           "  public static method Main(): int "
           "    using var b := new Box<string>(); "
           "    return b.Pick(" + std::to_string(arg) + "); "
           "  end Main; "
           "end C;";
}

}  // namespace

// ===========================================================================
// T1 — THE ISSUE'S OWN REPRO, method path. `K` must fold to 9 and MATCH.
// Was: rc=1, "pattern `K` is not supported by the backend". Now 105.
// ===========================================================================
BEZEL_TEST(xmod_generic_body_pattern_const_2857,
           t1_issue_repro_method_matching_scrutinee_spec_19_3) {
    int rc = runXmod2857("bz2857_t1", methodLib2857("LP2857A"),
                         methodApp2857("LP2857A", 9));
    BEZEL_EXPECT_TRUE(rc != kDepFailed);  // see kDepFailed's note: never silent
    if (rc == kDepFailed) return;
    BEZEL_EXPECT_EQ(rc, 105);
}

// ===========================================================================
// T2 — ★ THE DISCRIMINATOR. Same body, NON-matching scrutinee. If `K` were read
// as a catch-all binding pattern rather than folded to the constant 9, this
// would also answer 105. It must answer 102.
// ===========================================================================
BEZEL_TEST(xmod_generic_body_pattern_const_2857,
           t2_method_non_matching_scrutinee_not_a_catchall_spec_19_3) {
    int rc = runXmod2857("bz2857_t2", methodLib2857("LP2857B"),
                         methodApp2857("LP2857B", 3));
    BEZEL_EXPECT_TRUE(rc != kDepFailed);  // see kDepFailed's note: never silent
    if (rc == kDepFailed) return;
    BEZEL_EXPECT_EQ(rc, 102);
}

// ===========================================================================
// T3 — §2.8.2 clause 2 CONTROL: the identical declarations as ONE module. This
// is what the cross-module answer must equal, and it is the assertion that
// makes T1/T2 a divergence claim rather than a bare expectation.
// ===========================================================================
BEZEL_TEST(xmod_generic_body_pattern_const_2857,
           t3_single_module_control_defines_the_answer_spec_2_8_2) {
    int one = runSingle2857(
        "bz2857_t3",
        "module Main; "
        "public class Box<T> "
        "  public v: T; "
        "  public method Pick(sel: int): int "
        "    const K := 9; "
        "    var r := 0; "
        "    case sel of K => r := 5, _ => r := 2 end; "
        "    return 100 + r; "
        "  end Pick; "
        "end Box; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Box<string>(); "
        "    return b.Pick(9); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_EQ(one, 105);

    int two = runXmod2857("bz2857_t3x", methodLib2857("LP2857C"),
                          methodApp2857("LP2857C", 9));
    BEZEL_EXPECT_TRUE(two != kDepFailed);  // never a silent skip
    if (two == kDepFailed) return;
    BEZEL_EXPECT_EQ(two, one);
}

// ===========================================================================
// T4 — CONSTRUCTOR path (parseAndAttachCtorBody, §14/#662c). The case runs in
// the ctor and stores through a field. Matching scrutinee -> tag 5.
// ===========================================================================
BEZEL_TEST(xmod_generic_body_pattern_const_2857,
           t4_ctor_body_matching_spec_19_3) {
    const char* lib =
        "module LP2857D; "
        "public class Box<T> "
        "  public v: T; "
        "  public tag: int; "
        "  public constructor(n: int) "
        "    const K := 9; "
        "    case n of "
        "      K => this.tag := 5, "
        "      _ => this.tag := 2 "
        "    end; "
        "  end constructor; "
        "end Box;";
    int rc = runXmod2857(
        "bz2857_t4", lib,
        "module Main; import LP2857D; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Box<string>(9); "
        "    return 100 + b.tag; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(rc != kDepFailed);  // see kDepFailed's note: never silent
    if (rc == kDepFailed) return;
    BEZEL_EXPECT_EQ(rc, 105);
}

// ===========================================================================
// T5 — CONSTRUCTOR path, the catch-all discriminator.
// ===========================================================================
BEZEL_TEST(xmod_generic_body_pattern_const_2857,
           t5_ctor_body_non_matching_not_a_catchall_spec_19_3) {
    const char* lib =
        "module LP2857E; "
        "public class Box<T> "
        "  public v: T; "
        "  public tag: int; "
        "  public constructor(n: int) "
        "    const K := 9; "
        "    case n of "
        "      K => this.tag := 5, "
        "      _ => this.tag := 2 "
        "    end; "
        "  end constructor; "
        "end Box;";
    int rc = runXmod2857(
        "bz2857_t5", lib,
        "module Main; import LP2857E; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Box<string>(3); "
        "    return 100 + b.tag; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(rc != kDepFailed);  // see kDepFailed's note: never silent
    if (rc == kDepFailed) return;
    BEZEL_EXPECT_EQ(rc, 102);
}

// ===========================================================================
// T6 — DESTRUCTOR path (parseAndAttachDtorBody, §28.4). Observed through a
// static the destructor writes, as in #2855's suite. Matching -> 105.
// ===========================================================================
BEZEL_TEST(xmod_generic_body_pattern_const_2857,
           t6_dtor_body_matching_spec_19_3) {
    const char* lib =
        "module LP2857F; "
        "public class Box<T> "
        "  public v: T; "
        "  public tag: int; "
        "  public static Trace: int; "
        "  destructor "
        "    const K := 9; "
        "    case this.tag of "
        "      K => Trace := 105, "
        "      _ => Trace := 102 "
        "    end; "
        "  end destructor; "
        "end Box;";
    int rc = runXmod2857(
        "bz2857_t6", lib,
        "module Main; import LP2857F; "
        "class C "
        "  public static method Main(): int "
        "    do "
        "      using var b := new Box<string>(); "
        "      b.tag := 9; "
        "    end; "
        "    return Box<string>.Trace; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(rc != kDepFailed);  // see kDepFailed's note: never silent
    if (rc == kDepFailed) return;
    BEZEL_EXPECT_EQ(rc, 105);
}

// ===========================================================================
// T7 — DESTRUCTOR path, the catch-all discriminator.
// ===========================================================================
BEZEL_TEST(xmod_generic_body_pattern_const_2857,
           t7_dtor_body_non_matching_not_a_catchall_spec_19_3) {
    const char* lib =
        "module LP2857G; "
        "public class Box<T> "
        "  public v: T; "
        "  public tag: int; "
        "  public static Trace: int; "
        "  destructor "
        "    const K := 9; "
        "    case this.tag of "
        "      K => Trace := 105, "
        "      _ => Trace := 102 "
        "    end; "
        "  end destructor; "
        "end Box;";
    int rc = runXmod2857(
        "bz2857_t7", lib,
        "module Main; import LP2857G; "
        "class C "
        "  public static method Main(): int "
        "    do "
        "      using var b := new Box<string>(); "
        "      b.tag := 3; "
        "    end; "
        "    return Box<string>.Trace; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(rc != kDepFailed);  // see kDepFailed's note: never silent
    if (rc == kDepFailed) return;
    BEZEL_EXPECT_EQ(rc, 102);
}

// ===========================================================================
// T8 — the §19.3 EXPRESSION form `(case … of …)` (grammar A.6 `CaseExpr`) in a
// method body. Same table, different syntactic position — a fix that only
// handled the statement form would miss this.
// ===========================================================================
BEZEL_TEST(xmod_generic_body_pattern_const_2857,
           t8_case_expression_form_spec_19_3) {
    const char* lib =
        "module LP2857H; "
        "public class Box<T> "
        "  public v: T; "
        "  public method Pick(sel: int): int "
        "    const K := 9; "
        "    return 100 + (case sel of K => 5, _ => 2); "
        "  end Pick; "
        "end Box;";
    int match = runXmod2857("bz2857_t8", lib, methodApp2857("LP2857H", 9));
    BEZEL_EXPECT_TRUE(match != kDepFailed);  // never a silent skip
    if (match == kDepFailed) return;
    BEZEL_EXPECT_EQ(match, 105);
    int miss = runXmod2857("bz2857_t8b", lib, methodApp2857("LP2857H", 3));
    BEZEL_EXPECT_TRUE(miss != kDepFailed);  // never a silent skip
    if (miss == kDepFailed) return;
    BEZEL_EXPECT_EQ(miss, 102);
}

// ===========================================================================
// T9 — ACCEPT-CONTROL: a §17.14 RUNTIME const is still REJECTED as a pattern
// constant, cross-module as in-module. The fix carries a table; it must not
// widen what qualifies as a pattern constant. This is the over-acceptance guard.
// ===========================================================================
BEZEL_TEST(xmod_generic_body_pattern_const_2857,
           t9_runtime_const_still_rejected_spec_17_14) {
    const char* lib =
        "module LP2857I; "
        "public class Box<T> "
        "  public v: T; "
        "  public method Pick(sel: int, n: int): int "
        "    const K := n + 1; "
        "    var r := 0; "
        "    case sel of K => r := 5, _ => r := 2 end; "
        "    return 100 + r; "
        "  end Pick; "
        "end Box;";
    // The exporter itself must not compile: a runtime const is not a valid
    // pattern constant (§17.14/§19.3), independent of any module boundary.
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string tag = "bz2857_t9_" + unique_stem("");
    TempFile2857 aSrc((dir / (tag + "_a.bzl")).string());
    TempFile2857 aObj((dir / (tag + "_a.o")).string());
    writeSource2857(aSrc.path, lib);
    BEZEL_EXPECT_TRUE(!compileToObj2857(aSrc.path, aObj.path));
}

// ===========================================================================
// T10 — ACCEPT-CONTROL: a plain integer-LITERAL pattern in an imported generic
// body keeps working. A literal pattern needs no symbol at all, so it never
// consulted `patternSymbols_` — pinned so the fix is shown not to have disturbed
// the pattern kind that was already fine, in both the match and non-match
// directions.
//
// ⚠ WHY NOT AN ENUM-CONSTANT CONTROL, which would be the more natural
// neighbour. It is not expressible here, for a reason unrelated to this fix and
// already tracked: naming the enum puts a SIBLING TYPE in the method's
// signature (or body), which disables body reconstruction outright — the
// importer then hits #2821's layout gate and refuses the call with
// "…is not yet monomorphized for this instantiation … a body that reaches a
// sibling type or a module function cannot be reconstructed across the
// boundary". That is #2842. MEASURED here: the exporter compiles clean and the
// IMPORTER fails, so an enum-pattern control would have asserted #2842's
// behaviour rather than this fix's. Filed nothing new — it is #2842's exact
// shape. Revisit this control when #2842 lands.
// ===========================================================================
BEZEL_TEST(xmod_generic_body_pattern_const_2857,
           t10_literal_pattern_untouched_spec_19_3) {
    const char* lib =
        "module LP2857J; "
        "public class Box<T> "
        "  public v: T; "
        "  public method Pick(sel: int): int "
        "    var r := 0; "
        "    case sel of "
        "      9 => r := 5, "
        "      _ => r := 2 "
        "    end; "
        "    return 100 + r; "
        "  end Pick; "
        "end Box;";
    int hit = runXmod2857("bz2857_t10", lib, methodApp2857("LP2857J", 9));
    BEZEL_EXPECT_TRUE(hit != kDepFailed);  // never a silent skip
    if (hit == kDepFailed) return;
    BEZEL_EXPECT_EQ(hit, 105);
    int miss = runXmod2857("bz2857_t10b", lib, methodApp2857("LP2857J", 3));
    BEZEL_EXPECT_TRUE(miss != kDepFailed);  // never a silent skip
    if (miss == kDepFailed) return;
    BEZEL_EXPECT_EQ(miss, 102);
}

}  // namespace bezel::module
