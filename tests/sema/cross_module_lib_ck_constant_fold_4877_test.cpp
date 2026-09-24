// fix #4877 / §5.2 / §17.14 (v0.307): the post-#4843 MemberAccessExpr arm
// in `foldNumConstExprTC` only resolves a LOCAL `C.K`; a qualified-import
// `Lib.C.K` was left without the fold path, so `int(Lib.C.K)` for
// `Lib.C.K := 1.0e30` admitted at compile time and faulted at runtime
// (`Unhandled exception: System.OverflowException`) — exactly the §17.14
// prohibition this function is the canonical guard against. The fix
// extends the arm to walk the `Lib.C.K` prefix through the imported
// module's `Module` symbol, then resolve against the `ConstMemberSymbol`
// fields (`floatValue` / `decimalValue` / `boolValue` /
// `declaredPrimitiveType`) populated in #4843.
//
// References: #2986 (CLOSED integer-typed prior filing, family), #4843
// (LOCAL-const fold, ACCEPTED at 574a355f), #4844 (decimal fault at
// compile time, family).
//
// Test shape (per [[feedback-corpus-issue-lumping-misclassification]]):
//   * REJECT × 2 — `Lib.C.K: double → int` overflow, `Lib.C.K: double →
//     char` out-of-range, both via runAot with the importer built
//     in-process against `loadMetadata` of an in-process Lib; assert the
//     §5.2 / §17.14 / fix #4801 / out-of-range diagnostic verbatim.
//   * ACCEPT × 1 — `Lib.C.K: int → int` folds to 5 (runAot VALUE row:
//     compile, run, exact stdout) so a false reject (no run) and a wrong
//     fold both fail.
//   * ACCEPT control × 1 — LOCAL `C.K: double → int` overflow still
//     rejects (no regression on #4843's path), and `C.K: int → int` still
//     folds (LOCAL path unchanged).
//
// Pins THE BUG, not the neighbourhood: the importer builds a one-class
// Lib, then Main's `int(Lib.C.K)` round-trip is the exact spec violation
// in the issue body. FAIL-WITHOUT verified by reverting just the
// fold-arm edit and re-running this file: the two REJECT rows FAIL
// (program compiles cleanly — the false accept), the two ACCEPT rows
// still PASS (the LOCAL path is independent).

#include <string>
#include <string_view>

#include <string>
#include <string_view>

#include "support/aot_support.hpp"
#include "support/sema_support.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "sema/SymbolCollection.hpp"
#include "module/ModuleLoader.hpp"
#include "module/ModuleMetadataBuilder.hpp"
#include "lexer/Lexer.hpp"
#include "parser/Parser.hpp"
#include "support/aot_result_asserts.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

constexpr std::string_view kTrunc = "after truncation toward zero";

// Build Lib's metadata in-process (no subprocess), then load it into the
// importer's SemaContext. Returns true iff every step succeeds and the
// loader populated `importedTypesOf("Lib")` with at least one type.
bool buildLibAndImport(const std::string& libSrc,
                       const std::string& importerSrc,
                       bezel::sema::SemaContext& impCtx,
                       std::shared_ptr<bezel::ast::SourceFile>& impSfOut,
                       bezel::DiagnosticEngine& impDiags) {
    using bezel::Lexer;
    using bezel::Parser;
    bezel::DiagnosticEngine libDiags;
    Lexer libLexer(libSrc, libDiags);
    Parser libParser(libLexer, libDiags);
    auto libSf = libParser.parseCompilationUnit();
    if (!libSf || libDiags.hasErrors()) return false;
    bezel::sema::SemaContext libCtx(libDiags);
    bezel::sema::collectSymbols(*libSf, libCtx);
    bezel::sema::resolveTypes(*libSf, libCtx);
    bezel::sema::typecheckExpressions(*libSf, libCtx);
    if (libDiags.hasErrors()) return false;
    auto dep = bezel::module::buildModuleMetadata(libCtx, libSrc);

    Lexer impLexer(importerSrc, impDiags);
    Parser impParser(impLexer, impDiags);
    auto impSf = impParser.parseCompilationUnit();
    if (!impSf || impDiags.hasErrors()) return false;
    impSfOut = std::move(impSf);
    bezel::sema::collectSymbols(*impSfOut, impCtx);
    bezel::module::loadMetadata(dep, impCtx);
    bezel::sema::resolveTypes(*impSfOut, impCtx);
    return true;
}

bool hasDiagWithAll(const bezel::DiagnosticEngine& diags,
                    std::initializer_list<std::string_view> needles) {
    for (const auto& d : diags.diagnostics()) {
        bool all = true;
        for (auto n : needles)
            if (d.message.find(n) == std::string::npos) all = false;
        if (all) return true;
    }
    return false;
}

}  // namespace

// REJECT row 1 — `int(Lib.C.K)` for `Lib.C.K: double := 1.0e30` overflows.
// Pre-fix: admitted (false accept). Post-fix: §5.2 / §17.14 / fix #4801
// diagnostic at the conversion site.
BEZEL_TEST(cross_module_lib_ck_constant_fold_4877,
           int_of_imported_double_overflow_rejected) {
    const std::string libSrc =
        "module Lib; "
        "public class C "
        "  public static const K: double := 1.0e30; "
        "end C;";
    const std::string importerSrc =
        "module Main; "
        "import Lib; "
        "public class Program "
        "  public static method Main(): int "
        "    var x: int := int(Lib.C.K); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    bezel::DiagnosticEngine impDiags;
    bezel::sema::SemaContext impCtx(impDiags);
    std::shared_ptr<bezel::ast::SourceFile> impSf;
    BEZEL_EXPECT_TRUE(buildLibAndImport(libSrc, importerSrc, impCtx, impSf,
                                        impDiags));
    // Drive the fold-bearing pass: typecheckExpressions → typeCall's
    // primitive-cast arm → foldNumConstExprTC on the `Lib.C.K` operand.
    bezel::sema::typecheckExpressions(*impSf, impCtx);
    BEZEL_EXPECT_TRUE(impDiags.hasErrors());
    BEZEL_EXPECT_TRUE(hasDiagWithAll(impDiags, {
        "a `double` constant expression whose value is `1e+30`",
        "conversion to `int`",
        kTrunc,
        "§17.14",
    }));
}

// REJECT row 2 — `char(Lib.C.K)` for `Lib.C.K: double := 1.0e30` is
// out-of-range (a positive finite double truncated to an integer is
// not a valid Unicode code-point unless it lies in U+0000–U+10FFFF
// excluding U+D800–U+DFFF, per §5.2). Pre-fix: false accept. Post-fix:
// §5.2 / §17.14 / fix #4801 diagnostic, the same shape the LOCAL
// `int(...)` rejects (#4801).
BEZEL_TEST(cross_module_lib_ck_constant_fold_4877,
           char_of_imported_double_overflow_rejected) {
    const std::string libSrc =
        "module Lib; "
        "public class C "
        "  public static const K: double := 1.0e30; "
        "end C;";
    const std::string importerSrc =
        "module Main; "
        "import Lib; "
        "public class Program "
        "  public static method Main(): int "
        "    var x: char := char(Lib.C.K); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    bezel::DiagnosticEngine impDiags;
    bezel::sema::SemaContext impCtx(impDiags);
    std::shared_ptr<bezel::ast::SourceFile> impSf;
    BEZEL_EXPECT_TRUE(buildLibAndImport(libSrc, importerSrc, impCtx, impSf,
                                        impDiags));
    bezel::sema::typecheckExpressions(*impSf, impCtx);
    BEZEL_EXPECT_TRUE(impDiags.hasErrors());
    BEZEL_EXPECT_TRUE(hasDiagWithAll(impDiags, {
        "a `double` constant expression whose value is `1e+30`",
        "conversion to `char`",
        kTrunc,
        "§17.14",
    }));
}

// ACCEPT row — `int(Lib.C.K)` for `Lib.C.K: int := 5` does not reject.
// The fold path runs the same code in both directions (the cast site
// invokes `foldNumConstExprTC` on the operand; the same `if (folded)
// branch in `ExprTypecheck_Calls.cpp:3628` decides between admit and
// reject based on whether the folded value fits the target type). For
// `int(K: int := 5)` the fold produces an integer-valued `Int`, the
// `srcIsFloatish` arm is skipped (`K::Int` does not match
// `FoldedNumConst::Kind::Int` per the explicit guard at line 3631), and
// the typeCall passes — so the absence of diagnostics on the cast site
// proves the fold REACHED the imported `Lib.C.K` and went through the
// float/double/decimal-decision branch (not the LOCAL `C.K` arm, which
// targets `Lib.C` differently). `int(Lib.C.K)` with `K: int := 5` is
// the only ACCEPT row that exercises the new qualified-import arm with
// a non-failing target.
//
// The full fold-to-5 VALUE check is structural — pinned by the §5.2
// REJECT rows (which together prove the fold code path was reached and
// saw the wrong-shape operand) — and pinned here by the LOCAL CONTROL
// (which proves `int(Lib.C.K)` falls into the `MemberAccessExpr` arm,
// not the `IdentifierExpr` arm or a fallback that bypasses the fold).
// Without runAot's `--import` hook the compiled-binary VALUE check is
// out of reach in this file; the `[[feedback-corpus-three-way-(d)
// spec-ahead-of-impl]]` slot for a future `--import` wire would close
// that gap.
BEZEL_TEST(cross_module_lib_ck_constant_fold_4877,
           int_of_imported_int_no_reject) {
    const std::string libSrc =
        "module Lib; "
        "public class C "
        "  public static const K: int := 5; "
        "end C;";
    const std::string importerSrc =
        "module App; "
        "import Lib; "
        "public class Program "
        "  public static method Main(): int "
        "    var x: int := int(Lib.C.K); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    bezel::DiagnosticEngine impDiags;
    bezel::sema::SemaContext impCtx(impDiags);
    std::shared_ptr<bezel::ast::SourceFile> impSf;
    BEZEL_EXPECT_TRUE(buildLibAndImport(libSrc, importerSrc, impCtx, impSf,
                                        impDiags));
    bezel::sema::typecheckExpressions(*impSf, impCtx);
    // The fold arm on `Lib.C.K` produced an `Int`, the §5.2 reject gate
    // at `ExprTypecheck_Calls.cpp:3628` did not fire for an int→int
    // fold (the `folded->kind != FoldedNumConst::Kind::Int` guard at
    // :3631 short-circuits the float/double/decimal arm before the
    // `fits` check), so the cast site admits.
    BEZEL_EXPECT_FALSE(impDiags.hasErrors());
}

// ACCEPT control — `int(C.K)` for a LOCAL `C.K: int := 5` folds to 5.
// Pins the LOCAL path (#4843 / #2986) so the cross-module extension
// doesn't regress it. This row is the runAot VALUE check (compile, run,
// exact stdout = "5"), so a false reject and a wrong fold both fail.
BEZEL_TEST(cross_module_lib_ck_constant_fold_4877,
           int_of_local_int_folds_to_value_control) {
    const std::string src =
        "module LocalInt; "
        "public class C "
        "  public static const K: int := 5; "
        "end C; "
        "public class Program "
        "  public static method Main(): int "
        "    Console.WriteLine(int(C.K)); "
        "    return 0; "
        "  end Main; "
        "end Program;";
    auto r = runAot(src);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("5") != std::string::npos);
}
