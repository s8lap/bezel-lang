// Spec-derived regression test for issue #4591 — "resolveConstIdentInScope's
// module/enclosing-scope arm has no imported-const fallback". §2.7 "Name
// resolution for dotted names" rule 1 / §17.14 "Constant expressions"; spec
// v0.306.
//
// ── THE ISSUE'S CLAIM, AND WHAT INVESTIGATION FOUND ──────────────────────────
//
// `Resolver::resolveConstIdentInScope` (TypeResolution_Names.cpp) has two
// lookup arms for a bare `const` identifier: arm one walks
// `currentOuterType_` and its class base chain (fixed for the imported case
// by #3230 — `cms->intValue` fallback when `cms->decl` has no AST
// `ast::ConstDecl`, which is the imported shape); arm two is a generic
// `lookupName(name, scope)` scope-chain walk with NO such fallback. #4591
// flagged the code-shape asymmetry — same function, thirteen lines apart —
// but explicitly left reachability unverified: "does `const K := 5;` at
// file/module scope even exist... or is this arm reached only for e.g. an
// enclosing `static class`'s own same-module consts?"
//
// VERIFIED (this lane, with a live two-module build via `build/bezelc`,
// before writing this test): arm two can NEVER observe a `ConstMemberSymbol`
// at all, imported or local, so it needs no fallback of its own.
//
//   1. Per §2.7 rule 1, a bare name reaches a `const` only through "the
//      enclosing type's accessible members ... inherited included" — exactly
//      arm one's walk. "The current module's declarations" and "unambiguous
//      imported bare names", the two sources rule 1 lists AFTER the enclosing
//      type, are never `const`: `ast::ConstDecl` is an A.4 Member production
//      (reachable only inside a type body); A.2's `TopDecl` grammar admits no
//      bare `const`, so nothing module-scoped is ever a const in the first
//      place.
//   2. Grepped exhaustively: no `Resolver::Scope::names[...] = ...`
//      insertion, anywhere in TypeResolution_Names.cpp / _Checks.cpp /
//      TypeResolution.cpp, ever stores a `ConstMemberSymbol` — only
//      `TypeSymbol` / `TypeAliasSymbol` / `NestedTypeSymbol` / a
//      generic-param `nullptr` marker. `lookupName(name, scope)` therefore
//      cannot return one, so `dynamic_cast<const ast::ConstDecl*>(s->decl)`
//      can never succeed on the path arm two's `s` takes.
//   3. Measured directly with `build/bezelc`: a bare reference to another
//      class's const — same-module unrelated class, a nested type
//      referencing its ENCLOSING type's const, and a cross-module unrelated
//      importing class — is rejected with "unresolved identifier `K`" at
//      general expression resolution, for both a local and an imported `K`.
//      That rejection happens whether or not `K`'s declared type would even
//      be foldable, i.e. before arm two's absent fallback could matter.
//
// This test PINS that reachability finding (T1/T1L/T2 below) so a future
// change that starts registering `const` symbols into a `Resolver::Scope`
// without ALSO giving arm two the `cms->intValue` fallback is caught as a
// new silent-wrong-fold defect, exactly the shape #3230 fixed for arm one —
// rather than resurfacing unnoticed. T3 is the control: arm one's already-
// fixed inheritance path must keep folding an imported const cleanly.
//
// FIX SHIPPED WITH THIS TEST: a comment on arm two (matching the "#3152:
// AST-ONLY on purpose" convention, e.g. TypeResolution_Checks.cpp:1780)
// recording points 1-3 above, so a future reader does not have to re-derive
// them. No functional code changed — there is no live imported-const
// fallback gap in arm two to fix, so this is not a FAIL-WITHOUT-provable bug
// fix; it is a verified-and-documented non-bug, with these rows protecting
// the verification against regression.

#include "module/ModuleLoader.hpp"

#include <memory>
#include <string>
#include <string_view>

#include "ast/Node.hpp"
#include "module/ModuleMetadata.hpp"
#include "module/ModuleMetadataBuilder.hpp"
#include "lexer/Lexer.hpp"
#include "parser/Parser.hpp"
#include "sema/DefiniteAssignment.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/SymbolCollection.hpp"
#include "sema/TypeResolution.hpp"
#include "support/Diagnostic.hpp"
#include "test_harness.hpp"

namespace bezel::module {
namespace {

ModuleMetadata buildMeta4591(std::string_view source) {
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

struct Run4591 {
    DiagnosticEngine diags;
    std::unique_ptr<ast::SourceFile> sf;
    std::unique_ptr<sema::SemaContext> ctx;
    bool parsed = false;
};

std::unique_ptr<Run4591> run4591(const ModuleMetadata* dep,
                                 std::string_view src) {
    auto r = std::make_unique<Run4591>();
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

bool sawDiag4591(const Run4591& r, std::string_view needle) {
    for (const auto& d : r.diags.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

constexpr const char* kUnresolvedK4591 = "unresolved identifier `K`";

// --- T1: cross-module unrelated importing class, bare `K` -------------------
// `Consumer` does NOT inherit `Lib` — §2.7 rule 1 gives it no path to `Lib`'s
// member const by bare name, imported or not.

const char* kLibT1_4591 =
    "module Lib4591T1; "
    "public class Lib "
    "  public const K: int := 41; "
    "  public constructor() end constructor; "
    "end Lib; ";

std::string appT1_4591() {
    return std::string(
        "module App4591T1; import Lib4591T1; "
        "public class Consumer "
        "  public constructor() end constructor; "
        "  public static method Main(): int "
        "    var b: byte := K; "
        "    return int(b); "
        "  end Main; "
        "end Consumer;");
}

BEZEL_TEST(xmod_const_scope_arm2_unreachable_4591,
           t1_imported_unrelated_class_bare_const_unresolved_2_7) {
    ModuleMetadata dep = buildMeta4591(kLibT1_4591);
    auto r = run4591(&dep, appT1_4591());
    BEZEL_EXPECT_TRUE(r->parsed);
    BEZEL_EXPECT_TRUE(sawDiag4591(*r, kUnresolvedK4591));
}

// --- T1L: same-module local twin --------------------------------------------

std::string twinT1_4591() {
    return
        "module Twin4591T1; "
        "public class Lib "
        "  public const K: int := 41; "
        "  public constructor() end constructor; "
        "end Lib; "
        "public class Consumer "
        "  public constructor() end constructor; "
        "  public static method Main(): int "
        "    var b: byte := K; "
        "    return int(b); "
        "  end Main; "
        "end Consumer;";
}

BEZEL_TEST(xmod_const_scope_arm2_unreachable_4591,
           t1_twin_local_unrelated_class_bare_const_unresolved_2_7) {
    auto r = run4591(nullptr, twinT1_4591());
    BEZEL_EXPECT_TRUE(r->parsed);
    BEZEL_EXPECT_TRUE(sawDiag4591(*r, kUnresolvedK4591));
}

// --- T2: nested type referencing its ENCLOSING type's const by bare name ---
// §11.6: a nested class's access to its enclosing class's members requires
// holding an INSTANCE of the enclosing class; a bare unqualified `K` from a
// nested type's own body is not that, and `currentOuterType_` (arm one) is
// rebound to the NESTED type while its own body resolves (TypeResolution_
// Names.cpp's resolveTypeDecl save/restore), so it does not see `Outer`'s
// members either. This is local-only: nesting is not a cross-module shape.

std::string twinT2_4591() {
    return
        "module Twin4591T2; "
        "public class Outer "
        "  public const K: int := 41; "
        "  public class Inner "
        "    public constructor() end constructor; "
        "    public static method Main(): int "
        "      var b: byte := K; "
        "      return int(b); "
        "    end Main; "
        "  end Inner; "
        "  public constructor() end constructor; "
        "end Outer;";
}

BEZEL_TEST(xmod_const_scope_arm2_unreachable_4591,
           t2_nested_type_enclosing_const_unresolved_11_6) {
    auto r = run4591(nullptr, twinT2_4591());
    BEZEL_EXPECT_TRUE(r->parsed);
    BEZEL_EXPECT_TRUE(sawDiag4591(*r, kUnresolvedK4591));
}

// --- T3: CONTROL — arm one's inheritance path (#3230) must stay clean ------
// `Consumer: Lib` inherits the imported const, so `currentOuterType_`'s own
// base-chain walk (arm one) finds it and folds through `cms->intValue`. A
// regression here — not in this test's own subject, arm two — would mean
// #3230 broke, which this row exists to catch.

const char* kLibT3_4591 =
    "module Lib4591T3; "
    "public class Lib "
    "  public const K: int := 41; "
    "  public constructor() end constructor; "
    "end Lib; ";

std::string appT3_4591() {
    return std::string(
        "module App4591T3; import Lib4591T3; "
        "public class Consumer: Lib "
        "  public constructor() end constructor; "
        "  public static method Main(): int "
        "    var b: byte := K; "
        "    return int(b); "
        "  end Main; "
        "end Consumer;");
}

BEZEL_TEST(xmod_const_scope_arm2_unreachable_4591,
           t3_control_imported_inherited_const_folds_clean_17_14) {
    ModuleMetadata dep = buildMeta4591(kLibT3_4591);
    auto r = run4591(&dep, appT3_4591());
    BEZEL_EXPECT_TRUE(r->parsed);
    BEZEL_EXPECT_TRUE(!sawDiag4591(*r, kUnresolvedK4591));
}

}  // namespace
}  // namespace bezel::module
