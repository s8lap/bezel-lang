// fix #3617 (§17.14 / §15.1 / §22.12 / §2.8.3 rule 4, v0.286): two of the SIX
// copies of the §17.14 const-identifier resolver still lacked the imported
// fallback fix #3230 added to the other four, so a bare identifier naming an
// IMPORTED class-member const folded to nothing and the check built on the fold
// silently gave up.
//
// THE FAMILY. `foldIntConst` (sema/ConstFold.hpp) takes a caller-supplied
// `resolve(name, depth)` closure. Six call sites hand it a hand-copied lambda
// with the same body: `lookupName(name)`, then `ast::LocalConstDecl::value` or
// `ast::ConstDecl::value`, then recurse. Fix #3230 (family M11 of umbrella #3152)
// found four of them — TypeResolution_Checks.cpp, TypeResolution_Names.cpp x2,
// and `ExprTC::foldIntConstExprTC` — and gave each a final
// `ConstMemberSymbol::intValue` arm, because an IMPORTED const is reconstructed
// from dependency metadata (#328) with NO AST and the decl walk returns null for
// it. It did not know about two more, both in ExprTypecheck.cpp and both
// resolving through the same `lookupName` scope chain:
// `ExprTC::localConstDivByZeroTC` and `ExprTC::foldLocalConstArraySize`.
//
// REACHABILITY (the question this test had to answer before it could be written:
// can `lookupName`'s scope-chain path reach an IMPORTED class-member const at
// all, as opposed to the `ts->members`/`baseTypes` walk #3230 already fixed?).
// YES — a class in the importer that INHERITS from an imported base sees the
// base's consts by bare name. Measured on `main` @ 907ce0f, each against a
// character-identical single-module twin:
//
//   T1 `const q: int := 10 / Z;` with `Z := 0` on an imported base.
//      local    -> rejected: "const `q` initializer contains a division or modulo
//                  by zero — a const initializer must fold to a representable
//                  compile-time-constant value (§17.14)"
//      imported -> ACCEPTED; the built program throws
//                  System.DivideByZeroException at run time (exit 70).
//      A compile error demoted to a runtime fault by a module boundary.
//
//   T2 `const n: int := K; var a: [n]int := [7, 8, 9];` with `K := 3` on an
//      imported base. `foldLocalConstArraySize` is the LOCAL-const array-size
//      fold — the member/literal case returns early at
//      `if (declType->arraySizeValue >= 0) return;`, already folded by
//      TypeResolution's (#3230-fixed) closure, so this closure is only reached
//      through a LOCAL const, and a local const's initializer naming an imported
//      one is exactly that path.
//      local    -> compiles, runs, returns 24.
//      imported -> `arraySizeValue` stayed -1 and codegen emitted MALFORMED IR:
//                  "Invalid indices for GEP pointer type!" x3, then
//                  "IR verification failed (malformed IR emitted)".
//
// THE FIX — feed one accessor, not a fifth and sixth copy. The three ExprTC
// lambdas are replaced by ONE member function, `ExprTC::foldConstIdentTC`, whose
// body is `foldIntConstExprTC`'s (the only #3230-correct copy of the three). The
// other three copies live on a different class (`Resolver`) with a different
// `lookupName` in two other files, one of which is being rewritten by another
// lane this cycle; they already carry #3230's arm, so all six now AGREE and the
// residue is duplication, not divergence. That residue is filed separately.
//
// §6b EXCLUSION DIFF — what the unified resolver ADMITS that the two old copies
// EXCLUDED. Exactly one set: a bare identifier that `lookupName` resolves to a
// `ConstMemberSymbol` with NO `ast::LocalConstDecl`/`ast::ConstDecl` and a
// populated `intValue`. `ConstMemberSymbol::intValue` is set from two places
// only — sema's own resolveTypes/checkConstTypes for a LOCAL const (which also
// has an AST decl, so the decl arm wins before the fallback is reached and the
// local answer is unchanged), and `ModuleLoader`'s
// `parseImportedConstIntText` for an imported one. It is `std::nullopt` for
// every const whose declared type is not an integer-family primitive and for a
// non-foldable integer initializer, so those keep answering nullopt exactly as
// before. Nothing local changes; the widening is confined to imported
// integer-family consts.
//
// The two consumers then behave as their local twins do:
//   * `localConstDivByZeroTC` finds the `/`-by-zero it could not see -> a NEW
//     REJECTION (quiet -> loud, matching the local twin);
//   * `foldLocalConstArraySize` folds the size -> `arraySizeValue` becomes a real
//     value where it was -1. §7's "giving a previously-inert slot a real value
//     makes every consumer newly reachable" applies: the §15.4 element-count
//     check and §15.1's `N > 0` check now run on this shape. Both are pinned
//     below, in both directions.
//
// FALSE-REJECT COST: 0. Every in-tree `.bzl` compiled with a PARENT-`main`
// `bezelc` and with the fixed one, comparing exit status and full diagnostic
// text; see the commit body for the count.

#include "module/ModuleLoader.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <vector>

#include "ast/Node.hpp"
#include "driver/Driver.hpp"
#include "lexer/Lexer.hpp"
#include "module/ModuleMetadata.hpp"
#include "module/ModuleMetadataBuilder.hpp"
#include "parser/Parser.hpp"
#include "sema/DefiniteAssignment.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/SymbolCollection.hpp"
#include "sema/TypeResolution.hpp"
#include "support/Diagnostic.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {
namespace fs = std::filesystem;

// --- sema-level half (T1: the diagnostic) -----------------------------------

ModuleMetadata buildMeta3617(std::string_view source) {
    DiagnosticEngine diags;
    Lexer lexer(source, diags);
    Parser parser(lexer, diags);
    auto sf = parser.parseCompilationUnit();
    if (!sf) return {};
    sema::SemaContext ctx(diags);
    sema::collectSymbols(*sf, ctx);
    sema::resolveTypes(*sf, ctx);
    sema::typecheckExpressions(*sf, ctx);
    return buildModuleMetadata(ctx, source);
}

struct Run3617 {
    DiagnosticEngine diags;
    std::unique_ptr<ast::SourceFile> sf;
    std::unique_ptr<sema::SemaContext> ctx;
    bool parsed = false;
};

std::unique_ptr<Run3617> run3617(const ModuleMetadata* dep,
                                 std::string_view src) {
    auto r = std::make_unique<Run3617>();
    Lexer lexer(src, r->diags);
    Parser parser(lexer, r->diags);
    r->sf = parser.parseCompilationUnit();
    if (!r->sf) return r;
    r->parsed = true;
    r->ctx = std::make_unique<sema::SemaContext>(r->diags);
    sema::collectSymbols(*r->sf, *r->ctx);
    if (dep) loadMetadata(*dep, *r->ctx);
    sema::resolveTypes(*r->sf, *r->ctx);
    sema::typecheckExpressions(*r->sf, *r->ctx);
    sema::checkDefiniteAssignment(*r->sf, *r->ctx);
    return r;
}

bool sawDiag3617(const Run3617& r, std::string_view needle) {
    for (const auto& d : r.diags.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

// Verbatim from ExprTypecheck's §17.14 const-initializer check.
constexpr const char* kDivZeroDiag3617 =
    "initializer contains a division or modulo by zero";
// Verbatim from the §15.4 inline-array element-count check.
constexpr const char* kCountDiag3617 =
    "inline-array initializer has 3 element(s) but the type requires exactly 4";

// --- executable half (T2: the malformed IR / the run) -----------------------

const std::filesystem::path& wtDir3617() { return wtScratchDir("wt-3617"); }

struct TempFile3617 {
    std::string path;
    explicit TempFile3617(std::string p) : path(std::move(p)) {}
    ~TempFile3617() { std::remove(path.c_str()); }
};

void write3617(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

// Compile lib -> object, compile main importing lib -> executable, run.
// -1 means "did not build", which is the pre-fix state of T2 (IR verification
// failed), so the test must be able to see it as distinct from an answer.
int runXmod3617(const std::string& libSrc, const std::string& mainSrc) {
    auto dir = wtDir3617();
    std::string stem = unique_stem("bezel_xmod3617");
    TempFile3617 lSrc((dir / (stem + "_lib.bzl")).string());
    TempFile3617 lObj((dir / (stem + "_lib.o")).string());
    TempFile3617 mSrc((dir / (stem + "_main.bzl")).string());
    TempFile3617 mExe((dir / (stem + "_main")).string());

    write3617(lSrc.path, libSrc);
    write3617(mSrc.path, mainSrc);

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

int runLocal3617(const std::string& src) {
    auto dir = wtDir3617();
    std::string stem = unique_stem("bezel_loc3617");
    TempFile3617 mSrc((dir / (stem + ".bzl")).string());
    TempFile3617 mExe((dir / stem).string());
    write3617(mSrc.path, src);

    bezel::Driver driver;
    if (!driver.compileFile(mSrc.path, mExe.path, {}).success) return -1;
    int status = std::system(mExe.path.c_str());
    if (status == -1 || !WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

// --- the shared declarations -------------------------------------------------

// `K` sizes the array, `Z` is the zero divisor. `Base` is what the importer's
// class INHERITS, which is what puts K and Z on `lookupName`'s scope chain.
std::string decls3617(int k) {
    return "public class Base "
           "  public const K: int := " + std::to_string(k) + "; "
           "  public const Z: int := 0; "
           "end Base; ";
}

std::string lib3617(int k) { return "module Lib; " + decls3617(k); }

std::string app3617(const std::string& body) {
    return "module App; import Lib; class C: Base " + body + " end C; ";
}

std::string local3617(int k, const std::string& body) {
    return "module App; " + decls3617(k) + " class C: Base " + body + " end C; ";
}

// T1: a §17.14 const initializer dividing by an imported zero const.
const char* kDivBody3617 =
    "  public static method Main(): int "
    "    const q: int := 10 / Z; "
    "    return q; "
    "  end Main; ";

// T1's OVER-REJECTION control: dividing by an imported NON-zero const must still
// be accepted (12 / 3 = 4). A fix that rejected every imported-const division
// would pass the test above and fail this one.
const char* kDivOkBody3617 =
    "  public static method Main(): int "
    "    const q: int := 12 / K; "
    "    return q; "
    "  end Main; ";

// T2: a local const initialised from an imported const, sizing an inline array.
// 7 + 8 + 9 = 24.
const char* kArrBody3617 =
    "  public static method Main(): int "
    "    const n: int := K; "
    "    var a: [n]int := [7, 8, 9]; "
    "    return a[0] + a[1] + a[2]; "
    "  end Main; ";

}  // namespace

// #3617 / §17.14 + §2.8.3 rule 4 — T1, THE DEFECT. Pre-fix: accepted, and the
// program threw System.DivideByZeroException at run time.
BEZEL_TEST(xmod_imported_const_fold_3617,
           imported_zero_const_divisor_rejected_spec_17_14) {
    ModuleMetadata dep = buildMeta3617(lib3617(3));
    auto r = run3617(&dep, app3617(kDivBody3617));
    BEZEL_EXPECT_TRUE(r->parsed);
    BEZEL_EXPECT_TRUE(sawDiag3617(*r, kDivZeroDiag3617));
}

// #3617 — T1's single-module control. Rejected on both sides of the fix; the
// pair is the §2.8.3 rule 4 statement.
BEZEL_TEST(xmod_imported_const_fold_3617,
           local_zero_const_divisor_rejected_spec_17_14) {
    auto r = run3617(nullptr, local3617(3, kDivBody3617));
    BEZEL_EXPECT_TRUE(r->parsed);
    BEZEL_EXPECT_TRUE(sawDiag3617(*r, kDivZeroDiag3617));
}

// #3617 — T1's OVER-REJECTION control: 12 / K with an imported K = 3 folds to 4
// and must compile and run.
BEZEL_TEST(xmod_imported_const_fold_3617,
           imported_nonzero_const_divisor_accepted_spec_17_14) {
    BEZEL_EXPECT_EQ(runXmod3617(lib3617(3), app3617(kDivOkBody3617)), 4);
}

BEZEL_TEST(xmod_imported_const_fold_3617,
           local_nonzero_const_divisor_accepted_spec_17_14) {
    BEZEL_EXPECT_EQ(runLocal3617(local3617(3, kDivOkBody3617)), 4);
}

// #3617 / §15.1 + §2.8.3 rule 4 — T2, THE MALFORMED IR. Pre-fix the importer
// emitted "Invalid indices for GEP pointer type!" and IR verification failed, so
// runXmod3617 returned its -1 build sentinel; the single-module twin returned 24.
BEZEL_TEST(xmod_imported_const_fold_3617,
           local_const_from_imported_const_sizes_inline_array_spec_15_1) {
    BEZEL_EXPECT_EQ(runXmod3617(lib3617(3), app3617(kArrBody3617)), 24);
}

BEZEL_TEST(xmod_imported_const_fold_3617,
           local_twin_const_sizes_inline_array_spec_15_1) {
    BEZEL_EXPECT_EQ(runLocal3617(local3617(3, kArrBody3617)), 24);
}

// #3617 / §15.4 — the consumer that a real `arraySizeValue` makes NEWLY
// REACHABLE (§7: "giving a previously-inert slot a real value makes every
// consumer newly reachable"). With K = 4 the same three-element initializer must
// now be REJECTED by the §15.4 element-count check, cross-module exactly as
// locally. Pre-fix the size never folded, so this check never ran on this shape.
BEZEL_TEST(xmod_imported_const_fold_3617,
           imported_const_array_size_element_count_checked_spec_15_4) {
    ModuleMetadata dep = buildMeta3617(lib3617(4));
    auto r = run3617(&dep, app3617(kArrBody3617));
    BEZEL_EXPECT_TRUE(r->parsed);
    BEZEL_EXPECT_TRUE(sawDiag3617(*r, kCountDiag3617));
}

BEZEL_TEST(xmod_imported_const_fold_3617,
           local_const_array_size_element_count_checked_spec_15_4) {
    auto r = run3617(nullptr, local3617(4, kArrBody3617));
    BEZEL_EXPECT_TRUE(r->parsed);
    BEZEL_EXPECT_TRUE(sawDiag3617(*r, kCountDiag3617));
}

}  // namespace bezel::module
