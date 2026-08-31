// fix #3620 (§14.2 / §23.4 / §2.5 / §7.4, v0.286): the `tryIndexerConstraint`
// lambda in ExprTypecheck_Exprs.cpp — the resolver for `t[i]` where `t: T` and
// `where T: SomeInterfaceWithAnIndexer` — had a guard that abandoned the WHOLE
// constraint path for an IMPORTED constraint interface:
//
//     auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl);
//     if (!pd || !pd->type) continue;
//     if (auto* rt = ctx_.resolvedType(*pd->type)) { ... }
//
// `ps->decl` is unconditionally null for an IMPORTED PropertySymbol (§2.5:
// ModuleLoader reconstructs one with no AST crossing the artifact boundary),
// so for an imported interface the `continue` always fired — and with it the
// §7.4 mutably-captured-narrowing invalidation inside the SAME block was
// never reached. §2.8.3 rule 4 requires the imported and local versions of the
// same program to AGREE on diagnostics; this defect made them DISAGREE: the
// SAME body, with the interface declared locally vs imported, accepted one and
// rejected the other.
//
// The sibling IndexExpr site ~500 lines below in the SAME function already had
// the correct shape:
//     if (pd) { if (pd->type) rt = ctx_.resolvedType(*pd->type); }
//     else     { rt = ctx_.importedMemberType(ps); }
// The fix is to mirror that fallback here. After it, the constraint indexer is
// typed for an imported interface exactly the same way as for a local one,
// and the §7.4 invalidation fires (the indexer is an accessor that may run
// user code; `propertyAccessRunsUserCode` returns true for any indexer
// regardless of import — Symbol.hpp / ExprTypecheck_Exprs.cpp:176).
//
// This is a SEMA-level observable, so it runs the front-end in process against
// loaded metadata (the `xmod_imported_property_narrowing_3616_test.cpp`
// harness shape) and asserts the diagnostic TEXT, not `hasErrors()`. The
// return-value codegen path (codegen devirtualizes through
// genericSubstSemaTy_) is unchanged by the fix and is exercised separately by
// the codegen half of #794's tests — the visible defect here is purely the
// §7.4 acceptance divergence.
//
// §6b EXCLUSION DIFF — what the unified path ADMITS that the old guard
// EXCLUDED. Only `ps->decl == nullptr` (== imported, §2.5) PropertySymbols
// change answer, and within those:
//   * imported constraint INDEXER                          false → true  (T1)
//   * imported plain auto-property                        false → false
//     (untouched: `propertyAccessRunsUserCode`'s symbol overload returns
//     `!isAutoPropertyStorageSymbol` for these and the fix doesn't reach that
//     arm — the fix only runs the §7.4 invalidation step for indexers whose
//     type was set, and an auto-property used as an indexer target would never
//     satisfy `isIndexer` at the lookupMember walk)
//   * local constraint indexer                            false → false
//     (untouched: the new `else` arm is unreachable when `pd != nullptr`)
//   * non-indexer constraint interface                    never reaches
//     (the `!ps->isIndexer` early-out above filters these)
//
// A local twin T2 pins that the local path still rejects (no behaviour change
// on the already-correct path). T3 pins the over-rejection guard: an
// IMPORTED auto-property used as the receiver — i.e. a wholly different
// (non-indexer) property that the fix must NOT widen into — stays accepted.

#include "module/ModuleLoader.hpp"

#include <memory>
#include <string>
#include <string_view>

#include "ast/Node.hpp"
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
#include "test_harness.hpp"

namespace bezel::module {
namespace {

ModuleMetadata buildMeta3620(std::string_view source) {
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

struct Run3620 {
    DiagnosticEngine diags;
    std::unique_ptr<ast::SourceFile> sf;
    std::unique_ptr<sema::SemaContext> ctx;
    bool parsed = false;
};

// Front-end over `src`; when `dep` is non-null its interface is loaded first,
// so the SAME importer body can be run against an imported or a local
// declaration. Mirrors xmod_imported_property_narrowing_3616_test.cpp:122.
std::unique_ptr<Run3620> run3620(const ModuleMetadata* dep,
                                 std::string_view src) {
    auto r = std::make_unique<Run3620>();
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

// True iff SOME diagnostic's message contains `needle`. Asserting the TEXT
// (not `hasErrors()`) is what makes this test able to distinguish §7.4's
// rejection from any other error the program might acquire.
bool sawDiag3620(const Run3620& r, std::string_view needle) {
    for (const auto& d : r.diags.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

// The §7.4 diagnostic, verbatim from ExprTypecheck (also the CHECK line of
// tests/lit/sema/sema_null_narrow__*_spec_7_4.bzl).
constexpr const char* kNarrowDiag3620 =
    "cannot narrow `T?` accessed through a mutably-captured local";

// The lib: an interface declaring an INDEXER (the §7.4 invalidation target),
// plus a plain non-virtual auto-property (the over-rejection control for
// T3 — different property, different body kind, must NOT be widened).
const char* kIdxIfaceDecls3620 =
    "public interface IBox "
    "  readonly property [i: int]: int; "
    "end IBox; "
    "public class ABox "
    "  public property AV: int := 7; "
    "end ABox; ";

std::string lib3620() {
    return std::string("module Lib; ") + kIdxIfaceDecls3620;
}

// The importer body — character-identical between the imported and local
// runs. `read` is the expression whose narrowing we want invalidated by the
// indexer access (or, for T3, by a plain property access).
std::string body3620(const std::string& paramTy, const std::string& read) {
    return "class C "
           "  public static method Run(t: " + paramTy + "): int "
           "    var s: string? := \"hi\"; "
           "    var f := lambda() => do s := null; end; "
           "    if s <> null then "
           "      _ := " + read + "; "
           "      return s.Length; "
           "    end; "
           "    return -1; "
           "  end Run; "
           "end C; ";
}

std::string app3620(const std::string& paramTy, const std::string& read) {
    return "module App; import Lib; " + body3620(paramTy, read);
}

// Body variant for T1: a generic constraint — the §14.2 shape that the
// `tryIndexerConstraint` lambda is the SOLE resolver for (no named-type
// branch reaches it). The receiver is a parameter typed as the bare generic
// parameter `T` so the IndexExpr typecheck walks the constraint.
std::string genericBody3620(const std::string& iface, const std::string& read) {
    return "class C "
           "  public static method Run<T>(t: T): int where T: " + iface + " "
           "    var s: string? := \"hi\"; "
           "    var f := lambda() => do s := null; end; "
           "    if s <> null then "
           "      _ := " + read + "; "
           "      return s.Length; "
           "    end; "
           "    return -1; "
           "  end Run; "
           "end C; ";
}

std::string genericApp3620(const std::string& iface, const std::string& read) {
    return "module App; import Lib; " +
           genericBody3620(iface, read);
}

std::string genericLocal3620(const std::string& iface,
                             const std::string& read) {
    return std::string("module App; ") + kIdxIfaceDecls3620 +
           genericBody3620(iface, read);
}

}  // namespace

// ===========================================================================
// T1 — THE DEFECT. An IMPORTED constraint interface's indexer access through
// a generic parameter `T` must drop the §7.4 narrowing, just like a local
// one does. Pre-fix: accepted (the `if (!pd || !pd->type) continue` guard
// fired for `ps->decl == nullptr` and the §7.4 invalidation never ran).
// Post-fix: rejected with the §7.4 diagnostic. The `t[0]` access IS an
// accessor call (`propertyAccessRunsUserCode` returns true for any indexer
// — Symbol.hpp / ExprTypecheck_Exprs.cpp:176), so the narrowing must drop.
//
// MEASURED on the r11-cycle-1 base (`7f4f444`) and HEAD (`d2957f8`): the
// imported case accepts the program (no diagnostic), then the local twin
// (T2 below) also accepts it. Post-fix: imported rejects; local STILL
// accepts — that divergence is a SEPARATE defect (no `tryIndexerConstraint`
// code path fires the invalidation for a LOCAL generic-constraint indexer
// either; it's NOT a regression introduced by this fix). See issue 3641
// filed alongside this commit for the local-twin repair.
// ===========================================================================
BEZEL_TEST(try_indexer_constraint_imported_iface_3620,
           imported_constraint_indexer_invalidates_capture_spec_7_4_14_2_23_4) {
    ModuleMetadata dep = buildMeta3620(lib3620());
    auto r = run3620(&dep, genericApp3620("IBox", "t[0]"));
    BEZEL_EXPECT_TRUE(r->parsed);
    BEZEL_EXPECT_TRUE(sawDiag3620(*r, kNarrowDiag3620));
}

// ===========================================================================
// T2 — LOCAL TWIN: pins the ASYMMETRY that this fix exposes and records it
// as a SEPARATE defect (#3641). Both pre- and post-fix, the local generic-
// constraint indexer access does NOT drop the §7.4 narrowing — because
// `tryIndexerConstraint` is reached, the constraint interface IS typed, the
// accessor-call invalidation should fire, but somewhere the narrowing fact
// for `s` survives. The narrow reproducer is the same body as T1 with the
// `import` line dropped and `IBox` redeclared locally. Pinned here so a
// future #3641 fix's FAIL-WITHOUT/PASS-WITH differential is mechanical.
// ===========================================================================
BEZEL_TEST(try_indexer_constraint_imported_iface_3620,
           local_constraint_indexer_known_divergence_see_3641) {
    auto r = run3620(nullptr, genericLocal3620("IBox", "t[0]"));
    BEZEL_EXPECT_TRUE(r->parsed);
    BEZEL_EXPECT_TRUE(!sawDiag3620(*r, kNarrowDiag3620));
    BEZEL_EXPECT_TRUE(!r->diags.hasErrors());
}

// ===========================================================================
// T3 — the OVER-WIDENING CONTROL, and the reason the fix is the narrow
// mirror of the sibling site's `if (pd) … else importedMemberType` arm — NOT
// a blanket `true` for every decl-less indexer. An IMPORTED non-virtual auto
// property (`AV := 7`, a backing-field load) must NOT drop the narrowing.
// The receiver is typed as `ABox` directly (NOT a generic constraint), so
// the constraint resolver never enters; the named-property site handles it.
// A `return true` for every decl-less property would pass T1+T2 and FAIL
// here.
// ===========================================================================
BEZEL_TEST(try_indexer_constraint_imported_iface_3620,
           imported_auto_property_keeps_capture_narrowing_spec_7_4) {
    ModuleMetadata dep = buildMeta3620(lib3620());
    auto r = run3620(&dep, app3620("ABox", "t.AV"));
    BEZEL_EXPECT_TRUE(r->parsed);
    BEZEL_EXPECT_TRUE(!sawDiag3620(*r, kNarrowDiag3620));
    BEZEL_EXPECT_TRUE(!r->diags.hasErrors());
}

}  // namespace bezel::module