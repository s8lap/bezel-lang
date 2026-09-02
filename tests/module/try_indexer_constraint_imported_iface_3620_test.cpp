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

// fix #3652 accept-row: the SAME shape as genericBody3620 but with NO
// mutating lambda, so `s` is never mutably captured — the §7.4 narrowing
// must survive the `t[0]` accessor call and the program must compile clean.
// Pins that the #3652 fix invalidates ONLY when a mutable capture is
// actually present, not unconditionally on every constraint indexer access.
std::string genericBodyNoLambda3652(const std::string& iface,
                                    const std::string& read) {
    return "class C "
           "  public static method Run<T>(t: T): int where T: " + iface + " "
           "    var s: string? := \"hi\"; "
           "    if s <> null then "
           "      _ := " + read + "; "
           "      return s.Length; "
           "    end; "
           "    return -1; "
           "  end Run; "
           "end C; ";
}

std::string genericLocalNoLambda3652(const std::string& iface,
                                     const std::string& read) {
    return std::string("module App; ") + kIdxIfaceDecls3620 +
           genericBodyNoLambda3652(iface, read);
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
// either; it's NOT a regression introduced by this fix). See issue #3652
// (renamed here from a stale #3641 cross-citation, #3679) — fixed alongside
// this comment update; see T2 below for the root cause and the repair.
// ===========================================================================
BEZEL_TEST(try_indexer_constraint_imported_iface_3620,
           imported_constraint_indexer_invalidates_capture_spec_7_4_14_2_23_4) {
    ModuleMetadata dep = buildMeta3620(lib3620());
    auto r = run3620(&dep, genericApp3620("IBox", "t[0]"));
    BEZEL_EXPECT_TRUE(r->parsed);
    BEZEL_EXPECT_TRUE(sawDiag3620(*r, kNarrowDiag3620));
}

// ===========================================================================
// T2 — LOCAL TWIN, fixed by #3652 (renamed from
// local_constraint_indexer_known_divergence_see_3641, whose #3641 citation
// was stale/wrong — #3641 is an unrelated §12.5 abstract-indexer-override
// defect; see #3679). Was previously asserting the ASYMMETRY: pre- and
// post-#3620, the local generic-constraint indexer access failed to drop
// the §7.4 narrowing even though `tryIndexerConstraint` IS reached and the
// constraint interface IS typed for the local case.
//
// ROOT CAUSE (measured with temporary fprintf instrumentation in
// `ExprTC::invalidateCapturedNarrowings` and `propertyAccessRunsUserCode`,
// then removed): NOT an ordering defect — `computeMutablyCaptured` is
// already a full pre-pass over the whole method body, run in
// `ExprTC::runMethodBody` BEFORE any statement is typechecked (§7.4,
// #1566), so `mutablyCapturedNames_` already contained `s` (size=1,
// contains("s")=1) by the time `t[0]` was typechecked. The #3652 issue's
// filed hypothesis ("typechecks before lambda-visit fills
// mutablyCapturedNames_") does not hold on this tree — REFUTED by
// measurement.
//
// The real defect: `propertyAccessRunsUserCode(const ast::PropertyDecl*
// pd)` (ExprTypecheck_Exprs.cpp) switches on `pd->bodyKind` without ever
// checking `pd->isIndexer`. §23.4 fixes an indexer's body forms to
// expression-bodied, block-form, or BODY-LESS (an interface member, or an
// abstract indexer) — there is no auto/backing-field indexer form — but the
// parser's body-less-property fallback stamps the same default
// `bodyKind == Auto` used for an ordinary auto-property. `IBox`'s
// `readonly property [i: int]: int;` therefore measured
// `bodyKind=Auto, accessors=0, modifiers=[readonly]` (no `abstract` token —
// an interface member is implicitly abstract, never spelled), so the
// pre-fix `Auto` case's field-access answer misclassified a REAL
// interface-dispatched indexer read as a backing-field load and returned
// false. The SYMBOL overload already had the correct answer for exactly
// this shape on its imported (`ps->decl == nullptr`) arm
// (`if (ps->isIndexer) return true;`) — which is why T1 (imported) rejected
// post-#3620 while T2 (local) did not: the local arm never reached that
// isIndexer carve-out at all, because `propertyAccessRunsUserCode(pd)`
// (the AST-only overload the SYMBOL overload delegates to for a non-null
// `ps->decl`) had no isIndexer check of its own.
//
// Fixed by adding an unconditional `if (pd->isIndexer) return true;` ahead
// of the switch in the AST-only overload, mirroring the SYMBOL overload's
// existing carve-out and its own comment ("§23.4 — an indexer is never
// auto, so its accessor is always user code").
// ===========================================================================
BEZEL_TEST(try_indexer_constraint_imported_iface_3620,
           local_constraint_indexer_invalidates_capture_spec_7_4_3652) {
    auto r = run3620(nullptr, genericLocal3620("IBox", "t[0]"));
    BEZEL_EXPECT_TRUE(r->parsed);
    BEZEL_EXPECT_TRUE(sawDiag3620(*r, kNarrowDiag3620));
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

// ===========================================================================
// T4 — fix #3652 ACCEPT ROW. The identical local generic-constraint-indexer
// shape as T2, but with NO mutating lambda in the body — `s` is never
// mutably captured, so the §7.4 narrowing must survive the `t[0]` accessor
// call and the program must compile clean. Guards against an over-broad fix
// that would invalidate a constraint-indexer-adjacent narrowing
// unconditionally rather than only when a real mutable capture exists.
// ===========================================================================
BEZEL_TEST(try_indexer_constraint_imported_iface_3620,
           local_constraint_indexer_no_capture_keeps_narrowing_spec_7_4_3652) {
    auto r = run3620(nullptr, genericLocalNoLambda3652("IBox", "t[0]"));
    BEZEL_EXPECT_TRUE(r->parsed);
    BEZEL_EXPECT_TRUE(!sawDiag3620(*r, kNarrowDiag3620));
    BEZEL_EXPECT_TRUE(!r->diags.hasErrors());
}

}  // namespace bezel::module