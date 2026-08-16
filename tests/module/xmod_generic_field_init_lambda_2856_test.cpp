// fix #2856 — a LAMBDA inside an IMPORTED generic class's STATIC FIELD
// INITIALIZER silently evaluates to 0 (§22.8 Field initializers, §22.12 Static
// members, §22.13 Static initialization order, §2.8.2 The artifact interface is
// a semantic requirement, §27.3 Lambda expressions, §14.1 Generic declarations,
// §2.5 Dependency resolution — v0.240).
//
// §22.8 admits the shape verbatim:
//
//   "Field initializers may reference: constants, literals, static members of
//    any class, `new` expressions, lambdas that capture only the above,
//    primary-constructor parameters (§22.10), and `typeof` expressions."
//
// §2.8.2 clause 2 states the rule that was broken, verbatim:
//
//   "**Instantiate** every public generic type and generic method at arbitrary
//    module-external type arguments satisfying the declared constraints. An
//    external instantiation behaves identically to a same-module instantiation
//    of the same generic at the same arguments."
//
// ModuleLoader re-parses an imported generic class's static field initializer
// (#2351 `fieldInitText`) in a FRESH SemaContext and carries the result into the
// importer's context. It carried three of the AST-keyed side tables —
// allExprSymbols() (remapped, with a verbatim `else` for every other kind per
// #2375), allResolvedTypes() and allExprTypes() (verbatim) — but never
// `lambdaParams_`, sema's per-`ast::LambdaExpr` parameter-symbol list. #2839
// added that one-line copy to the METHOD and CONSTRUCTOR paths only.
//
// Consequence: codegen's lowerLambda read `ctx_.lambdaParams(&lam)`, got NULL,
// declared the closure function with ZERO user parameters, bound no argument to
// any ParameterSymbol, and the lambda's value was lost — silently, at no
// severity.
//
// ⚠ VACUITY — how "the initializer was actually reconstructed" is established
// here, and it is MEASURED, not argued. Unlike the method/ctor paths (where a
// declined reconstruction falls back to the exporter's compiled function and can
// return the right answer by accident, which is what fooled #2818's first
// draft), parseAndAttachFieldInit has NO fallback and says so: a static field
// whose initializer cannot be reconstructed is a HARD, LOCATED compile error in
// the importing build —
//
//   "cannot import static field 'Box.Seed' from module 'GFX': its initializer
//    could not be reconstructed across the module boundary (...); per spec
//    §2.8.2 the import is rejected rather than silently observing an
//    uninitialized value"
//
// — which T9 below exercises directly. So a cell that COMPILES AND RUNS has a
// reconstructed initializer, full stop. On top of that, every asserted value
// except T1 (the issue's own repro) is `100 + lambdaResult`, with the `100`
// computed WITHOUT the lambda, so "fixed", "lambda dropped" (= exactly 100) and
// "not reconstructed" (= a compile error) are three distinct outcomes. Do not
// remove the `100 +` term when editing.

#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "test_harness.hpp"
#include "support/unique_stem.hpp"


namespace bezel::module {
namespace {

struct TempFile2856 {
    std::string path;
    explicit TempFile2856(const std::string& p) : path(p) {}
    ~TempFile2856() { std::remove(path.c_str()); }
};

void writeSource2856(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

bool compileToObj2856(const std::string& src, const std::string& obj) {
    bezel::Driver driver;
    return driver.compileObject(src, obj).success;
}

int compileImportAndRun2856(const std::string& bSrc,
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
int runXmod2856(const std::string& stem, const std::string& aSrcText,
                const std::string& bSrcText) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string tag = stem + "_" + unique_stem("");
    TempFile2856 aSrc((dir / (tag + "_a.bzl")).string());
    TempFile2856 aObj((dir / (tag + "_a.o")).string());
    writeSource2856(aSrc.path, aSrcText);
    if (!compileToObj2856(aSrc.path, aObj.path)) return -2;
    if (!std::ifstream(aObj.path).good()) return -2;
    TempFile2856 bSrc((dir / (tag + "_b.bzl")).string());
    TempFile2856 bExe((dir / (tag + "_b")).string());
    writeSource2856(bSrc.path, bSrcText);
    return compileImportAndRun2856(bSrc.path, bExe.path, aObj.path);
}

// Compile B (importing A's object) with the driver's stderr (fd 2) captured into
// `err`, so a REJECT can be asserted on its diagnostic TEXT rather than merely
// on "the build failed" — a crash fails too, and T10's whole job is to prove a
// CLEAN reject. Mirrors compileImportCaptureErr2412.
bool compileImportCaptureErr2856(const std::string& bSrc,
                                 const std::string& bExe,
                                 const std::string& aObj, std::string& err) {
    namespace fs = std::filesystem;
    std::string errPath = (fs::temp_directory_path() /
                           ("bz2856_err_" + unique_stem("") +
                            ".txt"))
                              .string();

    std::fflush(stderr);
    std::cerr.flush();
    int saved = ::dup(STDERR_FILENO);
    int fd = ::open(errPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    bool success = false;
    if (fd >= 0) ::dup2(fd, STDERR_FILENO);
    {
        bezel::Driver driver;
        std::vector<bezel::Dependency> deps;
        bezel::Dependency d;
        d.metadataPath = aObj;
        d.objectPath = aObj;
        deps.push_back(d);
        success = driver.compileFile(bSrc, bExe, deps).success;
    }
    std::fflush(stderr);
    std::cerr.flush();
    if (fd >= 0) {
        ::close(fd);
        ::dup2(saved, STDERR_FILENO);
    }
    ::close(saved);

    std::ifstream in(errPath);
    err.assign((std::istreambuf_iterator<char>(in)),
               std::istreambuf_iterator<char>());
    std::remove(errPath.c_str());
    return success;
}

}  // namespace

// ===========================================================================
// T1 — the issue's OWN repro (§22.8 / §22.12). An immediately-invoked lambda in
// an imported generic class's static field initializer. Must be 7; was 0.
// The single-module compilation of the identical declarations returns 7, which
// is what §2.8.2 clause 2 requires the cross-module build to match.
// ===========================================================================
BEZEL_TEST(xmod_generic_field_init_lambda_2856, t1_issue_repro_22_8) {
    int rc = runXmod2856(
        "bz2856_t1",
        "module GF1x; "
        "public class Box<T> "
        "  public v: T; "
        "  public static readonly Seed: int := (lambda(x: int) => x + 1)(6); "
        "  public method Get(): int "
        "    return Box<T>.Seed; "
        "  end Get; "
        "end Box;",
        "module Main; import GF1x; "
        "class C "
        "  public static method Main(): int "
        "    return Box<string>.Seed; "
        "  end Main; "
        "end C;");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 7);
}

// ===========================================================================
// T2 — the same shape with the three-way discriminator described in the header:
// `100 + (lambda(x: int) => x + 1)(6)`. 107 = fixed, 100 = the #2856 defect
// (the lambda call yielded 0), a compile error = the initializer was never
// reconstructed. On the parent commit this returns exactly 100.
// ===========================================================================
BEZEL_TEST(xmod_generic_field_init_lambda_2856, t2_discriminated_22_8) {
    int rc = runXmod2856(
        "bz2856_t2",
        "module Lib2856B; "
        "public class Box<T> "
        "  public v: T; "
        "  public static readonly Seed: int := "
        "    100 + (lambda(x: int) => x + 1)(6); "
        "end Box;",
        "module Main; import Lib2856B; "
        "class C "
        "  public static method Main(): int "
        "    return Box<string>.Seed; "
        "  end Main; "
        "end C;");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 107);
}

// ===========================================================================
// T3 — §22.8's "static members of any class": the lambda body reads a SIBLING
// public class's static field, which #2375 makes resolve to the REAL imported
// FieldSymbol. 100 + (2 + 40) = 142; was 100.
// ===========================================================================
BEZEL_TEST(xmod_generic_field_init_lambda_2856, t3_lambda_reads_static_member_22_8) {
    int rc = runXmod2856(
        "bz2856_t3",
        "module Lib2856C; "
        "public class Consts2856 "
        "  public static readonly Base: int := 40; "
        "end Consts2856; "
        "public class Box<T> "
        "  public v: T; "
        "  public static readonly Seed: int := "
        "    100 + (lambda(x: int) => x + Consts2856.Base)(2); "
        "end Box;",
        "module Main; import Lib2856C; "
        "class C "
        "  public static method Main(): int "
        "    return Box<string>.Seed; "
        "  end Main; "
        "end C;");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 142);
}

// ===========================================================================
// T4 — a MULTI-parameter lambda (§27.3). The dropped table cost the closure its
// whole user parameter list, not just one entry. 100 + 3*5 = 115; was 100.
// ===========================================================================
BEZEL_TEST(xmod_generic_field_init_lambda_2856, t4_multi_parameter_lambda_27_3) {
    int rc = runXmod2856(
        "bz2856_t4",
        "module Lib2856D; "
        "public class Box<T> "
        "  public v: T; "
        "  public static readonly Seed: int := "
        "    100 + (lambda(a: int, b: int) => a * b)(3, 5); "
        "end Box;",
        "module Main; import Lib2856D; "
        "class C "
        "  public static method Main(): int "
        "    return Box<string>.Seed; "
        "  end Main; "
        "end C;");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 115);
}

// ===========================================================================
// T5 — NESTED lambdas (§27.3): both entries must reach the importer, and the
// inner one must bind the OUTER lambda's parameter as its argument.
// 100 + (5*2 + 1) = 111; was 100.
// ===========================================================================
BEZEL_TEST(xmod_generic_field_init_lambda_2856, t5_nested_lambdas_27_3) {
    int rc = runXmod2856(
        "bz2856_t5",
        "module Lib2856E; "
        "public class Box<T> "
        "  public v: T; "
        "  public static readonly Seed: int := "
        "    100 + (lambda(x: int) => (lambda(y: int) => y * 2)(x) + 1)(5); "
        "end Box;",
        "module Main; import Lib2856E; "
        "class C "
        "  public static method Main(): int "
        "    return Box<string>.Seed; "
        "  end Main; "
        "end C;");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 111);
}

// ===========================================================================
// T6 — the lambda returns a REFERENCE type (§27.3 return-type inference over
// `string`). This shape did not merely produce a wrong value: the IMPORTING
// build FAILED with `internal: lowerExpr returned null for interpolated-string
// expression part — #1386`, because the interpolation's operand was the dropped
// lambda parameter. 100 + "v7".Length = 102.
// ===========================================================================
BEZEL_TEST(xmod_generic_field_init_lambda_2856, t6_lambda_returns_reference_type_27_3) {
    int rc = runXmod2856(
        "bz2856_t6",
        "module Lib2856F; "
        "public class Box<T> "
        "  public v: T; "
        "  public static readonly Seed: int := "
        "    100 + (lambda(x: int) => $\"v{x}\")(7).Length; "
        "end Box;",
        "module Main; import Lib2856F; "
        "class C "
        "  public static method Main(): int "
        "    return Box<string>.Seed; "
        "  end Main; "
        "end C;");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 102);
}

// ===========================================================================
// T7 — the lambda is STORED in a function-typed static field (§22.12) and
// invoked by the IMPORTER, not by the initializer. The closure crosses the
// boundary as a value, so this pins that the carried parameter list is the
// closure's own ABI and not an artifact of where the call happens to be
// written. 100 + 7 = 107; was 100.
// ===========================================================================
BEZEL_TEST(xmod_generic_field_init_lambda_2856, t7_stored_closure_called_by_importer_22_12) {
    int rc = runXmod2856(
        "bz2856_t7",
        "module Lib2856G; "
        "public class Box<T> "
        "  public v: T; "
        "  public static readonly F: (int) -> int := lambda(x: int) => x + 1; "
        "end Box;",
        "module Main; import Lib2856G; "
        "class C "
        "  public static method Main(): int "
        "    return 100 + Box<string>.F(6); "
        "  end Main; "
        "end C;");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 107);
}

// ===========================================================================
// T8 — CONTROL: a ZERO-parameter lambda. A null table and an empty parameter
// list are indistinguishable to lowerLambda, so this shape was ALWAYS correct
// and must stay correct — it is why the defect looked narrower than it is.
// 108 before AND after.
// ===========================================================================
BEZEL_TEST(xmod_generic_field_init_lambda_2856, t8_control_zero_parameter_lambda_27_3) {
    int rc = runXmod2856(
        "bz2856_t8",
        "module Lib2856H; "
        "public class Box<T> "
        "  public v: T; "
        "  public static readonly Seed: int := 100 + (lambda() => 8)(); "
        "end Box;",
        "module Main; import Lib2856H; "
        "class C "
        "  public static method Main(): int "
        "    return Box<string>.Seed; "
        "  end Main; "
        "end C;");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 108);
}

// ===========================================================================
// T9 — CONTROL: a NON-generic imported class. Only a GENERIC class's field
// initializer is re-parsed importer-side (the pass-3 gate in loadMetadata); a
// non-generic class's static field is initialized by the EXPORTER's own emitted
// sfinit, so this shape never had the defect and must not acquire one.
// 107 before AND after — it isolates "generic" from "imported".
// ===========================================================================
BEZEL_TEST(xmod_generic_field_init_lambda_2856, t9_control_non_generic_class_22_12) {
    int rc = runXmod2856(
        "bz2856_t9",
        "module Lib2856I; "
        "public class Plain2856 "
        "  public static readonly Seed: int := "
        "    100 + (lambda(x: int) => x + 1)(6); "
        "end Plain2856;",
        "module Main; import Lib2856I; "
        "class C "
        "  public static method Main(): int "
        "    return Plain2856.Seed; "
        "  end Main; "
        "end C;");
    if (rc == -2) return;
    BEZEL_EXPECT_EQ(rc, 107);
}

// ===========================================================================
// T10 — the VACUITY ORACLE itself, asserted rather than assumed: an initializer
// that genuinely CANNOT be reconstructed (it names an `internal` sibling, which
// §13.2 keeps out of the exported metadata) is rejected with a located
// diagnostic naming the field — it is NOT silently zero and NOT a fallback to
// some exporter-side value. This is what licenses "a cell above that compiled
// and ran had a reconstructed initializer". Unaffected by the fix (rejects both
// before and after); pinned so a later change cannot quietly turn the reject
// into a silent zero and make T1–T7 vacuous.
// ===========================================================================
BEZEL_TEST(xmod_generic_field_init_lambda_2856, t10_unreconstructible_is_a_hard_reject_2_8_2) {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path();
    std::string tag = "bz2856_t10_" + unique_stem("");
    TempFile2856 aSrc((dir / (tag + "_a.bzl")).string());
    TempFile2856 aObj((dir / (tag + "_a.o")).string());
    writeSource2856(aSrc.path,
                    "module Lib2856J; "
                    "internal class Hidden2856 "
                    "  public static readonly K: int := 5; "
                    "end Hidden2856; "
                    "public class Box<T> "
                    "  public v: T; "
                    "  public static readonly Seed: int := 100 + Hidden2856.K; "
                    "end Box;");
    if (!compileToObj2856(aSrc.path, aObj.path)) return;
    TempFile2856 bSrc((dir / (tag + "_b.bzl")).string());
    TempFile2856 bExe((dir / (tag + "_b")).string());
    writeSource2856(bSrc.path,
                    "module Main; import Lib2856J; "
                    "class C "
                    "  public static method Main(): int "
                    "    return Box<string>.Seed; "
                    "  end Main; "
                    "end C;");
    std::string err;
    bool ok = compileImportCaptureErr2856(bSrc.path, bExe.path, aObj.path, err);
    BEZEL_EXPECT_EQ(ok, false);
    // Assert on the diagnostic TEXT, not merely on "it failed": a crash would
    // also fail, and this test's whole job is to prove a CLEAN reject.
    BEZEL_EXPECT_TRUE(err.find("cannot import static field") != std::string::npos);
    BEZEL_EXPECT_TRUE(err.find("Box.Seed") != std::string::npos);
    if (err.find("cannot import static field") == std::string::npos)
        std::cout << "    stderr was: " << err << "\n";
}

}  // namespace bezel::module
