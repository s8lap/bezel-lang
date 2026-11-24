#pragma once

// fix #9102 (§22.3 / §23.4 / §23.5 / §28.14 / §28.9, v0.359): the INDEXER's
// index-parameter shape, and the ONE predicate that decides whether an index
// argument binds a CALL-BOUNDED indexer parameter.
//
// TWO questions about an indexer's index-parameter list are asked from two
// translation units, and they must agree:
//
//   1. the LOWERING (LlvmBackend_Assign.cpp): "what is the k-th index
//      parameter's TYPE / MODIFIER / ARITY?" — the indexer getter and setter
//      build the accessor call's signature from it, and an IMPORTED indexer
//      has no `ast::PropertyDecl` to read;
//   2. the ESCAPE WALK (LlvmBackend_Setup.cpp): "is the index argument at
//      position `k` a NON-CONSUMING READ?" — §28.14's call-bounded default
//      makes a plain function-typed index parameter a borrow for the
//      duration of the call, so the CALLER keeps its own reference and
//      releases it at scope end (the METHOD twin's shape).
//
// This header is the ONE place both read, so the two cannot drift.

#include <string_view>

#include "ast/Node.hpp"
#include "sema/SemaContext.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeResolution.hpp"

namespace bezel::codegen {

// fix #2414 (§22.10/§23.4/§2.8.2 clause 1/§2.8.3 rule 4, v0.235): an
// indexer's k-th index-parameter TYPE / total ARITY, resolved from either
// its LOCAL ast::PropertyDecl (`ps->decl`) or, for an IMPORTED indexer (no
// ast::PropertyDecl to read — ModuleLoader.cpp's Indexer case reconstructs
// `indexerParams` as ParameterSymbols carrying `resolvedDeclType` instead),
// the reconstructed ParameterSymbol directly. Every consumer below
// (findIndexer, selectIndexer, and lowerIndexerGet/-Set's signature-building)
// goes through these two functions instead of reading `pd` directly, so a
// LOCAL and an IMPORTED indexer are selected/typed identically — before this
// fix, every one of these sites unconditionally required `pd`, silently
// excluding every imported indexer candidate from overload resolution
// (confirmed: `selectIndexer` returned null for any imported class's
// indexer, well before codegen even reached the getter/setter dispatch).
// Mirrors the identically-shaped fallback added to
// ExprTypecheck_Exprs.cpp's sema-side overload matcher.
inline const sema::TypeRef* indexerParamType(const sema::SemaContext& ctx,
                                             const sema::PropertySymbol* ps,
                                             std::size_t k) {
    if (auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl)) {
        return (k < pd->indexerParams.size() && pd->indexerParams[k].type)
                   ? ctx.resolvedType(*pd->indexerParams[k].type)
                   : nullptr;
    }
    if (k < ps->indexerParams.size()) {
        if (auto* p =
                dynamic_cast<sema::ParameterSymbol*>(ps->indexerParams[k]))
            return p->resolvedDeclType;
    }
    return nullptr;
}

inline std::size_t indexerParamCount(const sema::PropertySymbol* ps) {
    if (auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl))
        return pd->indexerParams.size();
    return ps->indexerParams.size();
}

// fix #9102 (§23.4 / §23.5 / §28.14, v0.359): the MODIFIER of an indexer's
// k-th index parameter, read the same dual way `indexerParamType` reads its
// TYPE — from a LOCAL indexer's `ast::PropertyDecl::indexerParams`, and from
// the reconstructed `ParameterSymbol` for an IMPORTED one (ModuleLoader's
// Indexer case; the exporter's `convertParams` copies `ast::Param::modifier`
// verbatim into `MetaParam::modifier`, so `scoped` survives the boundary).
//
// It matters because §28.14's two parameter kinds are told apart by exactly
// this modifier: a plain function-typed parameter is CALL-BOUNDED (the callee
// receives the callback for the duration of the call only) while a `scoped`
// one is STORAGE-BOUNDED (the callee MAY store it). Empty when the parameter
// is not knowable — the caller then declines.
inline std::string_view indexerParamModifier(const sema::PropertySymbol* ps,
                                             std::size_t k) {
    if (auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl)) {
        return (k < pd->indexerParams.size()) ? pd->indexerParams[k].modifier
                                              : std::string_view();
    }
    if (k < ps->indexerParams.size()) {
        if (auto* p =
                dynamic_cast<sema::ParameterSymbol*>(ps->indexerParams[k]))
            return p->modifier;
    }
    return std::string_view();
}

// fix #9102 (§22.3 / §23.4 / §23.5 / §28.14 / §28.9, v0.359): the
// INDEX-argument twin of `closureArgBindsMethodOwnTypeParamBorrow` — does the
// index argument at position `k` of `ix` bind a CALL-BOUNDED indexer
// parameter?
//
// THE DEFECT THIS ADMITS. `ClosureEscapeWalker::expr`'s `IndexExpr` arm walked
// EVERY index argument with `safeRead=false`, i.e. classified it as an ESCAPE.
// A closure LOCAL passed as the index argument (`var g := lambda() => 7;
// fl[g];`) was therefore classified as escaping, `collectClosureReleaseEligible`
// did not arm its scope-end release, and the local's own +1 was never dropped —
// 12800 bytes in 200 allocations over the issue's own 200-iteration repro, in
// EVERY indexer path (the single-module one included).
//
// THE PROOF. §23.5 (v0.359): "`ref` and `out` parameters are not allowed in
// indexer parameter lists" — so an indexer's function-typed parameter is
// PLAIN, and §28.14 (v0.359)'s call-bounded default applies to it exactly as
// it applies to a method's plain function-typed formal: "the callee receives
// the callback for the duration of the call only … It may not be stored in a
// field of any object". The caller keeps its own reference and releases it at
// scope end — the METHOD twin's shape.
//
// THE CLAUSES, AND WHY EACH ONE FAILS CLOSED (→ ESCAPE → a leak, never an
// early release and never a double free).
//   * the index argument must be POSITIONAL and unmodified: a `move`d
//     argument TRANSFERS the value (§22.3), so the caller's local owns
//     nothing afterwards and no scope-end release may fire for it; a named or
//     discarded argument is not a parameter pass at all (§23.5: index
//     arguments are positional-only);
//   * the parameter must be PLAIN — a `scoped` parameter is storage-bounded
//     (§28.14: the callee MAY store the callback), and `owned`/`ref`/`out`
//     transfer or alias it;
//   * the parameter must be FUNCTION-TYPED — the §28.14 default is stated for
//     a function-typed parameter, and a bare type parameter is `Kind::Named`
//     at every instantiation (#1921/#2248);
//   * the indexer's ELEMENT type must NOT be function-typed — an accessor that
//     RETURNS its own callback would carry it out of the call, the same
//     `return` sink `isCallBoundedClosureArgPosition` declines for a method
//     whose declared return type is a function type. (That accessor is not
//     rejected by the compiler today: #9084's gap — an indexer accessor body
//     is never §28.14-verified. The METHOD twin is protected from it by
//     sema's `callBoundedScan`, which runs for every user-authored method
//     body but NOT for an indexer accessor. So this clause is what keeps the
//     relief away from the `return` sink; the `store` sink remains #9084's.)
//
// THE INDEXER IS SEM'S OWN RESOLUTION. `ctx.exprSymbol(ix)` is the
// OVERLOAD-RESOLVED PropertySymbol sema attached to the IndexExpr (#2530) —
// "the indexer the access actually binds to" — so this predicate reads the
// same parameter the call passes to, for a LOCAL and an IMPORTED indexer
// alike, and for a `this`/`base`-rooted access too. A receiver with no
// user-defined indexer (an `Array<T>`, a dictionary) has no PropertySymbol
// attached and is declined.
inline bool indexArgBindsCallBoundedIndexerParam(
        const sema::SemaContext& ctx, const ast::IndexExpr& ix,
        std::size_t k) {
    if (!ix.target || k >= ix.indices.size()) return false;
    const ast::CallArgument& arg = ix.indices[k];
    if (!arg.value) return false;
    if (!arg.modifier.empty() || !arg.name.empty() || arg.isDiscard)
        return false;
    const auto* ps =
            dynamic_cast<const sema::PropertySymbol*>(ctx.exprSymbol(ix));
    if (!ps || !ps->isIndexer) return false;
    if (k >= indexerParamCount(ps)) return false;
    if (!indexerParamModifier(ps, k).empty()) return false;
    const sema::TypeRef* pt = indexerParamType(ctx, ps, k);
    if (!pt || pt->kind != sema::TypeRef::Kind::Function) return false;
    if (pt->isResolvedTypeParam()) return false;
    const sema::TypeRef* et = ctx.exprType(ix);
    if (et) {
        if (et->kind == sema::TypeRef::Kind::Function) return false;
        if (et->isResolvedTypeParam()) return false;
    }
    return true;
}

}  // namespace bezel::codegen
