// fix #2839 — a LAMBDA inside an IMPORTED generic class's method or constructor
// body silently evaluates to 0 (§2.8.2 The artifact interface is a semantic
// requirement, §14.1 Generic declarations, §27.3 Lambda expressions, §27.5
// Closures, §2.5 Dependency resolution — v0.240).
//
// §2.8.2 clause 2 is the governing rule, verbatim:
//
//   "**Instantiate** every public generic type and generic method at arbitrary
//    module-external type arguments satisfying the declared constraints. An
//    external instantiation behaves identically to a same-module instantiation
//    of the same generic at the same arguments."
//
// To honour that, ModuleLoader re-parses an imported generic body's exported
// source (#783 methodBodyText / #662c ctorBodyText) in a FRESH SemaContext and
// carries the result into the importer's context. It carried THREE of the four
// AST-keyed side tables: allExprSymbols() (remapped), allResolvedTypes() and
// allExprTypes() (verbatim). `lambdaParams_` — sema's per-`ast::LambdaExpr`
// parameter-symbol list — was carried by neither loop, in either path.
//
// Consequence: in the importer, codegen's lowerLambda read
// `ctx_.lambdaParams(&lam)` and got NULL, so it declared the closure function
// with ZERO user parameters and bound no argument to any ParameterSymbol. The
// lambda's value was lost and the enclosing expression degraded to 0. There is a
// second half: a lambda parameter IS a `sema::ParameterSymbol`, so the remap
// loop's ParameterSymbol arm searched for it among the enclosing method's/ctor's
// parameters, never found it, and dropped the body's every READ of it too. Both
// halves are fixed together — either alone leaves the lambda broken.
//
// Measured cross-module divergence, every cell below built as two real modules
// and compared against the identical source compiled as ONE module.
//
// ⚠ VACUITY — how "the body was actually reconstructed" is established here,
// not assumed. When reconstruction declines, the call falls back to the
// EXPORTER's compiled function, which can return the RIGHT answer by accident
// (that is what made #2818's first test draft pass without its fix). Two
// independent guards are built into these programs:
//   (a) Every generic class carries a `T`-typed field FIRST, and every method
//       probed by an instance call. #2821's layout gate rejects, at compile
//       time with a located diagnostic, an INSTANCE call to an imported generic
//       method with no attached ast::MethodDecl whose instantiation layout
//       differs from the placeholder layout — which `Box<string>` does. So a
//       cell that COMPILES AND RUNS has an attached, reconstructed body.
//   (b) Every asserted value is `bump + lambdaResult` with a non-zero `bump`
//       computed WITHOUT the lambda. Three outcomes are therefore distinct:
//       `bump + lambdaResult` = fixed, `bump` alone = the #2839 defect, and
//       anything else (or a compile failure) = the body was not reconstructed.
//       On the parent commit every cell here returns exactly `bump`.
// Do not remove the `bump` term or the leading `T` field when editing.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

struct TempFile2839 {
    std::string path;
    explicit TempFile2839(const std::string& p) : path(p) {}
    ~TempFile2839() { std::remove(path.c_str()); }
};

void writeSource2839(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj2839(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

int compileImportAndRun2839(const std::string& bSrc,
                            const std::string& bExe,
                            const std::string& aObj) {
    bezel::Driver driver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = aObj;
    d.objectPath = aObj;
    deps.push_back(d);
    if (!driver.compileFile(bSrc, bExe, deps).success) return -1;
    int status = std::system(bExe.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

// Build A -> object, then B importing A -> exe, run, return the exit code.
// -1 on any compile/link/run failure; -2 when the DEPENDENCY object could not be
// produced (a toolchain problem, never the defect under test) so a genuine 0 is
// never mistaken for a skip.
int runXmod2839(const std::string& stem, const std::string& aSrcText,
                const std::string& bSrcText) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string tag = stem + "_" + std::to_string(std::rand());
    TempFile2839 aSrc((dir / (tag + "_a.bzl")).string());
    TempFile2839 aObj((dir / (tag + "_a.o")).string());
    writeSource2839(aSrc.path, aSrcText);
    if (!compileToObj2839(aSrc.path, aObj.path)) return -2;
    if (!std::ifstream(aObj.path).good()) return -2;
    TempFile2839 bSrc((dir / (tag + "_b.bzl")).string());
    TempFile2839 bExe((dir / (tag + "_b")).string());
    writeSource2839(bSrc.path, bSrcText);
    return compileImportAndRun2839(bSrc.path, bExe.path, aObj.path);
}

// Compile ONE source as a single module and run it — the §2.8.2 clause 2
// control: "An external instantiation behaves identically to a same-module
// instantiation of the same generic at the same arguments."
int runSingle2839(const std::string& stem, const std::string& srcText) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string tag = stem + "_" + std::to_string(std::rand());
    TempFile2839 src((dir / (tag + ".bzl")).string());
    TempFile2839 exe((dir / (tag + "_x")).string());
    writeSource2839(src.path, srcText);
    bezel::Driver driver;
    if (!driver.compileFile(src.path, exe.path, {}).success) return -1;
    int status = std::system(exe.path.c_str());
    return (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
}

}  // namespace

// ===========================================================================
// T1 — the issue's OWN repro, method path (`parseAndAttachMethodBody`,
// §14.1 / §27.3). A lambda declared in an imported generic class's method body
// and called on the return path. Must be 7; was 0.
// ===========================================================================
BEZEL_TEST(xmod_generic_body_lambda_2839, t1_issue_repro_method_body_27_3) {
    const char* lib =
        "module LG20; "
        "public class Box20<T> "
        "  public v: T; "
        "  public tag: int; "
        "  public method Compute(): int "
        "    var f: (int) -> int := lambda(x: int) => x + 1; "
        "    return f(this.tag); "
        "  end Compute; "
        "end Box20;";
    int rc = runXmod2839(
        "bz2839_t1", lib,
        "module Main; import LG20; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Box20<string>(); "
        "    b.v := \"hello\"; "
        "    b.tag := 6; "
        "    return b.Compute(); "
        "  end Main; "
        "end C;");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 7);
}

// ===========================================================================
// T2 — the issue's OWN repro, CONSTRUCTOR path (`parseAndAttachCtorBody`,
// §14 / #662c). A lambda in an imported generic class's ctor body. `b.tag`
// must read 7; it read 0.
// ===========================================================================
BEZEL_TEST(xmod_generic_body_lambda_2839, t2_issue_repro_ctor_body_27_3) {
    int rc = runXmod2839(
        "bz2839_t2",
        "module LG21; "
        "public class Box21<T> "
        "  public v: T; "
        "  public tag: int; "
        "  public constructor(n: int) "
        "    var f: (int) -> int := lambda(x: int) => x + 1; "
        "    tag := f(n); "
        "  end constructor; "
        "end Box21;",
        "module Main; import LG21; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Box21<string>(6); "
        "    b.v := \"hello\"; "
        "    return b.tag; "
        "  end Main; "
        "end C;");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 7);
}

// ===========================================================================
// T3 — CAPTURE AXES, method path, with the vacuity discriminator (b) above:
// each answer is `bump + lambdaResult`, `bump` computed without the lambda.
//   a. the lambda captures the enclosing METHOD'S PARAMETER   -> 100 + 15 = 115
//   b. the lambda captures a BODY-LOCAL (§27.5)               -> 100 + 14 = 114
//   c. the lambda takes a param and reads a FIELD via `this`  -> 100 +  8 = 108
// Before the fix all three return exactly 100 — the `bump` term alone.
// ===========================================================================
BEZEL_TEST(xmod_generic_body_lambda_2839, t3a_capture_method_parameter_27_5) {
    int rc = runXmod2839(
        "bz2839_t3a",
        "module Lib2839A; "
        "public class Box<T> "
        "  public v: T; "
        "  public method Add(n: int): int "
        "    var bump := n * 20; "
        "    var f: (int) -> int := lambda(x: int) => x + n; "
        "    return bump + f(10); "
        "  end Add; "
        "end Box;",
        "module Main; import Lib2839A; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Box<string>(); "
        "    return b.Add(5); "
        "  end Main; "
        "end C;");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 115);
}

BEZEL_TEST(xmod_generic_body_lambda_2839, t3b_capture_body_local_27_5) {
    int rc = runXmod2839(
        "bz2839_t3b",
        "module Lib2839B; "
        "public class Box<T> "
        "  public v: T; "
        "  public method L(): int "
        "    var k := 4; "
        "    var bump := 100; "
        "    var f: (int) -> int := lambda(x: int) => x + k; "
        "    return bump + f(10); "
        "  end L; "
        "end Box;",
        "module Main; import Lib2839B; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Box<string>(); "
        "    return b.L(); "
        "  end Main; "
        "end C;");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 114);
}

BEZEL_TEST(xmod_generic_body_lambda_2839, t3c_capture_field_via_this_27_5) {
    int rc = runXmod2839(
        "bz2839_t3c",
        "module Lib2839C; "
        "public class Box<T> "
        "  public v: T; "
        "  public tag: int; "
        "  public method F(): int "
        "    var bump := 100; "
        "    var f: (int) -> int := lambda(x: int) => this.tag + x; "
        "    return bump + f(1); "
        "  end F; "
        "end Box;",
        "module Main; import Lib2839C; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Box<string>(); "
        "    b.tag := 7; "
        "    return b.F(); "
        "  end Main; "
        "end C;");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 108);
}

// ===========================================================================
// T4 — NESTED lambdas (§27.3): a lambda whose `do`-body declares and calls
// another. Both entries must reach the importer, and the inner one must bind
// the OUTER lambda's parameter as its argument. 100 + (5*2 + 1) = 111; was 100
// (with an `internal: … #2491` degraded-return warning).
// ===========================================================================
BEZEL_TEST(xmod_generic_body_lambda_2839, t4_nested_lambdas_27_3) {
    int rc = runXmod2839(
        "bz2839_t4",
        "module Lib2839D; "
        "public class Box<T> "
        "  public v: T; "
        "  public method N(a: int): int "
        "    var bump := 100; "
        "    var outer: (int) -> int := lambda(x: int) => do "
        "      var inner: (int) -> int := lambda(y: int) => y * 2; "
        "      return inner(x) + 1; "
        "    end; "
        "    return bump + outer(a); "
        "  end N; "
        "end Box;",
        "module Main; import Lib2839D; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Box<string>(); "
        "    return b.N(5); "
        "  end Main; "
        "end C;");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 111);
}

// ===========================================================================
// T5 — the lambda returns a REFERENCE type (§27.3 return-type inference over
// `string`). This shape did not merely produce a wrong value: the importer
// FAILED TO COMPILE with `internal: lowerExpr returned null for
// interpolated-string expression part — #1386`, because the interpolation's
// operand was the dropped lambda parameter. "v7".Length = 2.
// ===========================================================================
BEZEL_TEST(xmod_generic_body_lambda_2839, t5_lambda_returns_reference_type_27_3) {
    int rc = runXmod2839(
        "bz2839_t5",
        "module Lib2839E; "
        "public class Box<T> "
        "  public v: T; "
        "  public method S(): int "
        "    var f: (int) -> string := lambda(x: int) => $\"v{x}\"; "
        "    return f(7).Length; "
        "  end S; "
        "end Box;",
        "module Main; import Lib2839E; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Box<string>(); "
        "    return b.S(); "
        "  end Main; "
        "end C;");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 2);
}

// ===========================================================================
// T6 — the OTHER consumer of `ctx_.lambdaParams` (LlvmBackend_Calls3.cpp): a
// lambda passed as a call ARGUMENT rather than bound to a local. `.Select` over
// an `Array<int>` inside the imported generic body. (1+2+3)*10 = 60.
// ===========================================================================
BEZEL_TEST(xmod_generic_body_lambda_2839, t6_lambda_as_call_argument_27_3) {
    int rc = runXmod2839(
        "bz2839_t6",
        "module Lib2839F; "
        "public class Box<T> "
        "  public v: T; "
        "  public method Sum3(): int "
        "    var xs: Array<int> := [1, 2, 3]; "
        "    return xs.Select(lambda(x: int) => x * 10).Sum(); "
        "  end Sum3; "
        "end Box;",
        "module Main; import Lib2839F; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Box<string>(); "
        "    return b.Sum3(); "
        "  end Main; "
        "end C;");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 60);
}

// ===========================================================================
// T7 — SHAPE GRID, all four measured cross-module in one test so a regression
// in any one of them is loud:
//   a. a `ref` lambda parameter (§27.1 lambda parameter modifiers) — the
//      by-pointer ABI is keyed on the same ParameterSymbol (refParams_).
//   b. a MULTI-parameter lambda — proves the positional binding, not just
//      "some parameter arrived".
//   c. a STATIC method of the imported generic (no receiver; #2821's layout
//      gate deliberately does not cover statics, so this cell's own value is
//      the only oracle).
//   d. an imported generic RECORD's constructor (buildSyntheticClassSource
//      reconstructs records through the same ctor path).
// Before the fix: 107 -> 100, 123 -> 100, 115 -> 100, 123 -> 100.
// ===========================================================================
BEZEL_TEST(xmod_generic_body_lambda_2839, t7a_ref_lambda_parameter_27_1) {
    int rc = runXmod2839(
        "bz2839_t7a",
        "module Lib2839G; "
        "public class Box<T> "
        "  public v: T; "
        "  public method Bump(seed: int): int "
        "    var acc := seed; "
        "    var f: (ref int) -> () := lambda(ref n: int) => do "
        "      n := n + 3; "
        "    end; "
        "    f(ref acc); "
        "    return 100 + acc; "
        "  end Bump; "
        "end Box;",
        "module Main; import Lib2839G; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Box<string>(); "
        "    return b.Bump(4); "
        "  end Main; "
        "end C;");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 107);
}

BEZEL_TEST(xmod_generic_body_lambda_2839, t7b_multi_parameter_lambda_27_3) {
    int rc = runXmod2839(
        "bz2839_t7b",
        "module Lib2839H; "
        "public class Box<T> "
        "  public v: T; "
        "  public method Pair(a: int, b: int): int "
        "    var f: (int, int) -> int := lambda(x: int, y: int) => x * 10 + y; "
        "    return 100 + f(a, b); "
        "  end Pair; "
        "end Box;",
        "module Main; import Lib2839H; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Box<string>(); "
        "    return b.Pair(2, 3); "
        "  end Main; "
        "end C;");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 123);
}

BEZEL_TEST(xmod_generic_body_lambda_2839, t7c_static_method_of_generic_14_1) {
    int rc = runXmod2839(
        "bz2839_t7c",
        "module Lib2839I; "
        "public class Box<T> "
        "  public v: T; "
        "  public static method SAdd(n: int): int "
        "    var bump := 100; "
        "    var f: (int) -> int := lambda(x: int) => x + n; "
        "    return bump + f(10); "
        "  end SAdd; "
        "end Box;",
        "module Main; import Lib2839I; "
        "class C "
        "  public static method Main(): int "
        "    return Box<string>.SAdd(5); "
        "  end Main; "
        "end C;");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 115);
}

BEZEL_TEST(xmod_generic_body_lambda_2839, t7d_generic_record_ctor_body_9_2) {
    int rc = runXmod2839(
        "bz2839_t7d",
        "module Lib2839J; "
        "public record Pt<T>(x: int, y: int) "
        "  public sum: int; "
        "  public constructor(x: int, y: int) "
        "    var f: (int, int) -> int := lambda(a: int, b: int) => a * 10 + b; "
        "    sum := 100 + f(x, y); "
        "  end constructor; "
        "end Pt;",
        "module Main; import Lib2839J; "
        "class C "
        "  public static method Main(): int "
        "    var p := new Pt<int>(2, 3); "
        "    return p.sum; "
        "  end Main; "
        "end C;");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 123);
}

// ===========================================================================
// T8 — the two CONTROLS that isolate the axis, and that this change must not
// disturb.
//   a. the SAME declarations compiled as ONE module (§2.8.2 clause 2's
//      same-module instantiation) — 7 before and after; this is what made the
//      cross-module 0 a pure artifact-boundary divergence rather than a lambda
//      bug.
//   b. a NON-generic imported class with the identical body. Only a GENERIC
//      body is re-parsed and remapped, so this path never touched
//      lambdaParams_ — 7 before and after.
// ===========================================================================
BEZEL_TEST(xmod_generic_body_lambda_2839, t8a_single_module_control_2_8_2) {
    int rc = runSingle2839(
        "bz2839_t8a",
        "module Main; "
        "public class Box20<T> "
        "  public v: T; "
        "  public tag: int; "
        "  public method Compute(): int "
        "    var f: (int) -> int := lambda(x: int) => x + 1; "
        "    return f(this.tag); "
        "  end Compute; "
        "end Box20; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Box20<string>(); "
        "    b.v := \"hello\"; "
        "    b.tag := 6; "
        "    return b.Compute(); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_EQ(rc, 7);
}

BEZEL_TEST(xmod_generic_body_lambda_2839, t8b_non_generic_imported_control_2_5) {
    int rc = runXmod2839(
        "bz2839_t8b",
        "module Lib2839K; "
        "public class Plain "
        "  public tag: int; "
        "  public method Compute(): int "
        "    var f: (int) -> int := lambda(x: int) => x + 1; "
        "    return f(this.tag); "
        "  end Compute; "
        "end Plain;",
        "module Main; import Lib2839K; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Plain(); "
        "    b.tag := 6; "
        "    return b.Compute(); "
        "  end Main; "
        "end C;");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 7);
}

}  // namespace bezel::module
