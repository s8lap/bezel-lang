#include "common/OwnershipFacts.hpp"
#include "common/BuiltinRegistry.hpp"

#include <string_view>

#include "ast/Node.hpp"
#include "sema/SemaContext.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeRef.hpp"

// #1414 Stage 4 Phase A — the shared ownership-facts core (see the header).
//
// The bodies below are the pre-refactor codegen predicates
// (Emitter::isFreshOwnedClassProducer / Emitter::isBuiltinOwnedTransferCall,
// LlvmBackend_Setup.cpp) relocated VERBATIM so both the ownership pass and the
// Emitter reach one copy.  Not one line of classification logic changed: this
// is a behaviour-preserving extraction, gated by the full suite.  The known
// drift (#1388 / #1415) is Phase B and is left untouched here.

namespace bezel::common {

const ast::MethodDecl* calleeMethodDecl(const ast::CallExpr& call,
                                        const sema::SemaContext& ctx) {
    // The callee → MethodSymbol → user MethodDecl navigation that both
    // consumers use to reach the `returnOwned` authority.  A null callee, a
    // callee with no MethodSymbol (a builtin, or a call through a value /
    // function), or a MethodSymbol with no AST MethodDecl (an imported method —
    // §28.10 / #707) all yield null.
    if (!call.callee) return nullptr;
    auto* ms = dynamic_cast<sema::MethodSymbol*>(ctx.exprSymbol(*call.callee));
    if (!ms) return nullptr;
    return dynamic_cast<const ast::MethodDecl*>(ms->decl);
}

bool isFreshOwnedClassProducer(const ast::Expr* e, const sema::SemaContext& ctx,
                               const TypeParamResolver& resolveParam) {
    // #1039 / #1053: a literal `new T(...)` or a call to a method declared to
    // return `owned T` — both transfer a fresh +1 the caller solely owns. A
    // call WITHOUT an `owned` return transfers no ownership: its result is a
    // BORROW (e.g. `return this.field`), and reclaiming it would free a value
    // owned elsewhere (use-after-free). `returnOwned` (ast::MethodDecl) is the
    // sound discriminator. A variable / field / `this` / property receiver is
    // neither a NewExpr nor a call, so it is never a fresh producer.
    //
    // `resolveParam` is the documented shared resolver hook (see header); the
    // weakest-copy shape does not resolve the receiver type-parameter, so the
    // result is byte-identical to the pre-refactor codegen predicate.
    (void)resolveParam;
    if (!e) return false;
    bool producer = false;
    if (dynamic_cast<const ast::NewExpr*>(e)) {
        producer = true;
    } else if (auto* call = dynamic_cast<const ast::CallExpr*>(e)) {
        if (const ast::MethodDecl* md = calleeMethodDecl(*call, ctx))
            producer = md->returnOwned;
    }
    if (!producer) return false;
    // The receiver type must be a heap CLASS: records/structs are value types
    // with no heap block or vtable disposer; interfaces/enums are not `new`-of-
    // class temporaries. A null declaration (builtin not exported as a TypeDecl)
    // is conservatively excluded — leak, but sound.
    const sema::TypeRef* rt = ctx.exprType(*e);
    if (!rt || rt->kind != sema::TypeRef::Kind::Named || !rt->declaration)
        return false;
    const auto* rtd = dynamic_cast<const ast::TypeDecl*>(rt->declaration->decl);
    return rtd && rtd->kind == ast::TypeDeclKind::Class;
}

bool isBuiltinOwnedTransferCall(const ast::CallExpr& call,
                                const sema::SemaContext& ctx) {
    // §4.5/§4.7/§4.9/§4.11/§2.14/§29.1: builtin methods that TRANSFER an owned
    // class element to the caller. These have no MethodSymbol (sema sets no
    // exprSymbol on the builtin callee), so recognition keys on the callee name
    // plus — where a borrowing counterpart with the same name exists — an
    // OWNING receiver type (a borrowing Stack.Pop returns a BORROW; disposing
    // it would double-free). The receiver-name guards are per-method operand
    // pairings, not a reusable name table, so they stay inline here.
    auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(call.callee.get());
    if (!ma) return false;
    const auto& nm = ma->member;
    std::string_view rcn;
    if (ma->target) {
        const sema::TypeRef* rt = ctx.exprType(*ma->target);
        // #1462 / §27.8: see through a transparent type alias
        // (`type OA := OwningArray<R>`) — the alias-resolved TypeRef keeps the
        // use-site astNode (`OA`) for diagnostics, so a bare astNode read would
        // miss the owning-collection head and the popped element would never be
        // tracked Owned (no §28.3 auto-drop → leak).
        if (rt && !rt->aliasResolvedHead.empty())
            rcn = rt->aliasResolvedHead;
        else if (rt && rt->astNode && rt->astNode->name &&
                 !rt->astNode->name->parts.empty())
            rcn = rt->headName();
        else if (rt && !rt->importedName.empty())
            rcn = rt->importedName;
    }
    // §4.7/§4.9/§4.11 (#1388 review): the positional/keyed removals transfer an
    // owned element ONLY on an element-owning COLLECTION receiver. Sema leniently
    // accepts these transfer-named calls on BORROWING collections too (Array /
    // Dictionary hold borrows), so without the owning-receiver gate the caller's
    // §28.3 auto-drop would dispose a value it does not own — a double-free.
    // Gate on the registry's owning-collection set (not a hand-list — the miss-
    // a-name bug class this refactor exists to kill). // per §5.11
    if ((nm == "PopLast" || nm == "PopFirst" || nm == "PopAt" ||
         nm == "PopValue") &&
        isOwningCollectionHead(rcn))
        return true;
    // §2.14: `T.Deserialize(d)` is a fresh owned producer for a class target
    // (the receiver is the TYPE, not a collection); a value-type target is
    // filtered out by the caller's class/interface-result guard.
    if (nm == "Deserialize")
        return true;
    if ((nm == "Pop" || nm == "Dequeue") &&
        (rcn == "OwningStack" || rcn == "OwningQueue"))
        return true;
    // #1349 / §29.1 v0.161: TakeCurrent on the owning enumerators (vtable
    // slot 3) transfers the yielded element.
    if (nm == "TakeCurrent" &&
        (rcn == "IOwningEnumerator" || rcn == "IOwningAsyncEnumerator"))
        return true;
    return false;
}

namespace {

// Recursively scan every `return EXPR;` reachable in statement `s` (the same
// statement forms that can carry a nested return as
// MethodOwnership::scanReturnsForWrapperReceiverBorrow, Ownership_Checks.cpp —
// a nested closure carries its body as an Expr, never spliced into the Stmt
// list, so it is structurally never descended into here). `sawReturn` records
// whether any return was seen at all; `allFresh` is cleared the moment a
// return whose value is not a fresh owned producer is found. Once `allFresh`
// is false the walk short-circuits. // fix #1499 (mirrors §28.3 callee scan)
void scanReturnsAllFreshOwned(const ast::Stmt* s, const sema::SemaContext& ctx,
                              const TypeParamResolver& resolveParam,
                              bool& sawReturn, bool& allFresh) {
    if (!s || !allFresh) return;
    if (const auto* rs = dynamic_cast<const ast::ReturnStmt*>(s)) {
        sawReturn = true;
        // A borrow return (`return _shared;`, `return p;`, `return this;`,
        // `return this.f;`, `return alias;`) is NOT a fresh owned producer, so
        // it clears allFresh — the whole method is then not a fresh-owned
        // factory and the call stays a borrow (§28.10; the #1499 regression).
        if (!rs->value ||
            !isFreshOwnedClassProducer(rs->value.get(), ctx, resolveParam))
            allFresh = false;
        return;
    }
    if (const auto* is = dynamic_cast<const ast::IfStmt*>(s)) {
        for (auto& b : is->ifBranch.body)
            scanReturnsAllFreshOwned(b.get(), ctx, resolveParam, sawReturn,
                                     allFresh);
        for (auto& br : is->elifBranches)
            for (auto& b : br.body)
                scanReturnsAllFreshOwned(b.get(), ctx, resolveParam, sawReturn,
                                         allFresh);
        for (auto& b : is->elseBody)
            scanReturnsAllFreshOwned(b.get(), ctx, resolveParam, sawReturn,
                                     allFresh);
        return;
    }
    if (const auto* ws = dynamic_cast<const ast::WhileStmt*>(s)) {
        for (auto& b : ws->body)
            scanReturnsAllFreshOwned(b.get(), ctx, resolveParam, sawReturn,
                                     allFresh);
        return;
    }
    if (const auto* fs = dynamic_cast<const ast::ForStmt*>(s)) {
        for (auto& b : fs->body)
            scanReturnsAllFreshOwned(b.get(), ctx, resolveParam, sawReturn,
                                     allFresh);
        return;
    }
    if (const auto* d = dynamic_cast<const ast::DoEndBlock*>(s)) {
        for (auto& b : d->body)
            scanReturnsAllFreshOwned(b.get(), ctx, resolveParam, sawReturn,
                                     allFresh);
        return;
    }
    if (const auto* ts = dynamic_cast<const ast::TryStmt*>(s)) {
        for (auto& b : ts->body)
            scanReturnsAllFreshOwned(b.get(), ctx, resolveParam, sawReturn,
                                     allFresh);
        for (auto& c : ts->catches)
            scanReturnsAllFreshOwned(c.body.get(), ctx, resolveParam, sawReturn,
                                     allFresh);
        for (auto& b : ts->finallyBody)
            scanReturnsAllFreshOwned(b.get(), ctx, resolveParam, sawReturn,
                                     allFresh);
        return;
    }
    if (const auto* cs = dynamic_cast<const ast::CaseStmt*>(s)) {
        for (auto& arm : cs->arms)
            for (auto& a : arm.stmts)
                scanReturnsAllFreshOwned(a.get(), ctx, resolveParam, sawReturn,
                                         allFresh);
        return;
    }
    // ExpressionStmt / AssignmentStmt / DiscardStmt / YieldStmt (iterator
    // methods are handled by the for-loop desugaring, not this rule) /
    // IncrementStmt / BreakStmt / ContinueStmt / ThrowStmt / LocalDecl: none
    // can carry a nested `return` of the enclosing method.
}

}  // namespace

bool staticFactoryReturnsFreshOwned(const ast::MethodDecl& md,
                                    const sema::TypeRef* resultTy,
                                    const sema::SemaContext& ctx,
                                    const TypeParamResolver& resolveParam) {
    // (1) STATIC only. §28.3: an INSTANCE method's non-`owned` class return has
    // a structural source (the receiver), so it is a borrow — never covered by
    // the sourceless-return rule.
    bool isStatic = false;
    for (std::string_view mod : md.modifiers)
        if (mod == "static") { isStatic = true; break; }
    if (!isStatic) return false;
    // (2) The resolved result must be a plain class/interface. A value-builtin
    // (DateTime/Guid/TimeSpan/…) carries kind==Class but has no disposal
    // obligation (mirrors the sibling isClassOrInterfaceResult guards).
    if (!resultTy || resultTy->kind != sema::TypeRef::Kind::Named ||
        !resultTy->declaration)
        return false;
    if (resultTy->declaration->isValueBuiltin) return false;
    if (resultTy->declaration->kind != ast::TypeDeclKind::Class &&
        resultTy->declaration->kind != ast::TypeDeclKind::Interface)
        return false;
    // §28.6 / §5.11: an owning-collection / owning-wrapper name-head result
    // carries its OWN, more specific documented-source borrow assumption (the
    // `Owning*` name denotes ELEMENT ownership, orthogonal to the container
    // `owned` qualifier). Excluded here so the two rules never fight over the
    // same call; a fresh owning value must be returned through an `owned`
    // return type (§28.3 borrow-lifetime rules), enforced at the definition.
    if (isOwningTypeHead(resultTy->headName())) return false;
    // (3) The DISTINGUISHING check (the #1499 soundness core): classify owned
    // ONLY when EVERY return path produces a fresh owned value. A borrow on any
    // path (a field read like `_shared`, a parameter, `this`, a plain alias)
    // leaves the call a borrow — never disposed by the caller (§28.10). An
    // abstract/iterator/no-return body (sawReturn stays false) is likewise not
    // a fresh-owned factory.
    bool sawReturn = false;
    bool allFresh = true;
    if (md.bodyKind == ast::MethodBodyKind::Expression) {
        sawReturn = md.exprBody != nullptr;
        allFresh = md.exprBody &&
                   isFreshOwnedClassProducer(md.exprBody.get(), ctx,
                                             resolveParam);
    } else {
        for (auto& s : md.stmts)
            scanReturnsAllFreshOwned(s.get(), ctx, resolveParam, sawReturn,
                                     allFresh);
    }
    return sawReturn && allFresh;
}

}  // namespace bezel::common
