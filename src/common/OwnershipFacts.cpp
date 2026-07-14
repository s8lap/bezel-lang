#include "common/OwnershipFacts.hpp"
#include "common/BuiltinRegistry.hpp"

#include <string_view>
#include <unordered_set>
#include <vector>

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

namespace {

// One arm of an if/case expression is an OWNED arm iff it is a fresh owned
// producer or a nested owned-meet if/case. // §28.3 arm-meet (v0.184, #1661)
bool armIsOwnedProducer(const ast::Expr* arm, const sema::SemaContext& ctx,
                        const TypeParamResolver& resolveParam) {
    if (!arm) return false;
    if (dynamic_cast<const ast::ConditionalExpr*>(arm) ||
        dynamic_cast<const ast::CaseExpr*>(arm))
        return conditionalCaseArmMeetIsOwned(*arm, ctx, resolveParam);
    return isFreshOwnedClassProducer(arm, ctx, resolveParam);
}

// The shared arm-meet decision (see the header). Every non-absorbing arm must
// be an owned producer; at least one must exist.
bool armsMeetOwned(const std::vector<const ast::Expr*>& arms,
                   const sema::SemaContext& ctx,
                   const TypeParamResolver& resolveParam) {
    bool sawOwned = false;
    for (const ast::Expr* arm : arms) {
        if (!arm) return false;
        // A `throw` arm is bottom-typed and a `null`-literal arm has no value
        // to own — both absorb to the other arms' state (§28.3, §17.12).
        if (dynamic_cast<const ast::ThrowExpr*>(arm)) continue;
        if (dynamic_cast<const ast::NullLiteralExpr*>(arm)) continue;
        if (!armIsOwnedProducer(arm, ctx, resolveParam)) return false;
        sawOwned = true;
    }
    return sawOwned;
}

}  // namespace

bool conditionalCaseArmMeetIsOwned(const ast::Expr& e,
                                   const sema::SemaContext& ctx,
                                   const TypeParamResolver& resolveParam) {
    if (const auto* c = dynamic_cast<const ast::ConditionalExpr*>(&e))
        return armsMeetOwned({c->thenValue.get(), c->elseValue.get()}, ctx,
                             resolveParam);
    if (const auto* ce = dynamic_cast<const ast::CaseExpr*>(&e)) {
        std::vector<const ast::Expr*> arms;
        for (const ast::CaseArm& arm : ce->arms) {
            // A block-body arm's value expression is not structurally
            // inspectable here — bail conservatively to a borrow meet.
            if (arm.bodyKind != ast::CaseArmBodyKind::Expression) return false;
            arms.push_back(arm.exprBody.get());
        }
        if (arms.empty()) return false;
        return armsMeetOwned(arms, ctx, resolveParam);
    }
    return false;
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
    // fix #1476 / stdlib §4.10: OwningSet<T>.Take(query: T) removes the
    // element equal to `query` and transfers its ownership to the caller —
    // the same owned-transfer shape as PopLast/PopFirst/PopAt/PopValue above.
    // Gated to the OwningSet receiver: TryTake's analogous transfer flows
    // through its `out owned removed: T?` PARAMETER, not this call's own
    // return value (bool) — that path is tracked separately by
    // isOutOwnedArgPosition, already wired for OwningSet.TryTake.
    if (nm == "Take" && rcn == "OwningSet")
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

namespace {

// The three TypeRef name fallbacks shared by the builtin-type matchers
// (mirrors MethodOwnership's own `builtinTypeRefNameHead`,
// Ownership_Checks.cpp — that copy is a private `namespace {}`-local helper
// with no free-function form to share, so this scoped duplicate exists only
// so `isBorrowBearingRecordOrSpan` below can name a Named TypeRef's builtin
// head without depending on the ownership pass, which is a layer ABOVE
// common/): builtin TypeSymbol declaration name, annotation AST name,
// imported name.
std::string_view borrowSpanNameHead(const sema::TypeRef* rt) {
    if (!rt || rt->kind != sema::TypeRef::Kind::Named) return {};
    if (rt->declaration) return rt->declaration->name;
    if (rt->astNode && rt->astNode->name && !rt->astNode->name->parts.empty())
        return rt->headName();
    return rt->importedName;
}

// Mirrors MethodOwnership::isBorrowableType (Ownership_Checks.cpp), used here
// only for the InlineArray element-type leg of `isBorrowBearingRecordCommon`'s
// local `fieldIsBorrowCarrying` field classification below — true when `rt`
// is a class/interface reference (the only element kind an inline-array
// field can hold that itself carries a borrow).
bool isReferenceTypeForRecordField(const sema::TypeRef* rt) {
    if (!rt) return true;  // unknown → conservative
    switch (rt->kind) {
        case sema::TypeRef::Kind::Primitive:
        case sema::TypeRef::Kind::Function:
            return false;
        case sema::TypeRef::Kind::Named:
            if (!rt->declaration) return false;  // generic param
            if (rt->declaration->isValueBuiltin) return false;
            return rt->declaration->kind == ast::TypeDeclKind::Class ||
                   rt->declaration->kind == ast::TypeDeclKind::Interface;
        case sema::TypeRef::Kind::Error:
            return true;  // unresolved → conservative
        default:
            return false;  // Unit, InlineArray, enum, record
    }
}

// §9.7: true when `rt` is itself a borrow-bearing record (recursion target of
// the field walk below) — mirrors MethodOwnership::isBorrowBearingRecordImpl
// (Ownership_Checks.cpp) exactly, so the declaration-site and instantiation-
// site checks classify identically (#1595). `seen` is the #1611 cycle guard
// for a self- or mutually-referential STATIC field.
bool isBorrowBearingRecordCommon(const sema::TypeRef* rt,
                                 const sema::SemaContext& ctx,
                                 std::unordered_set<const ast::TypeDecl*>& seen) {
    if (!rt || rt->kind != sema::TypeRef::Kind::Named || !rt->declaration)
        return false;
    const auto* decl =
        dynamic_cast<const ast::TypeDecl*>(rt->declaration->decl);
    if (!decl || decl->kind != ast::TypeDeclKind::Record) return false;
    if (!seen.insert(decl).second) return false;

    auto fieldIsBorrowCarrying = [&](const sema::TypeRef* ft) -> bool {
        if (!ft) return false;
        switch (ft->kind) {
            case sema::TypeRef::Kind::Named: {
                if (!ft->declaration) return false;  // generic param
                if (ft->declaration->isValueBuiltin) return false;
                if (ft->declaration->kind == ast::TypeDeclKind::Class ||
                    ft->declaration->kind == ast::TypeDeclKind::Interface)
                    return true;
                if (ft->declaration->kind == ast::TypeDeclKind::Record)
                    return isBorrowBearingRecordCommon(ft, ctx, seen);
                return false;  // enum
            }
            case sema::TypeRef::Kind::InlineArray:
                return ft->arrayElement && !ft->arrayOwned &&
                       isReferenceTypeForRecordField(ft->arrayElement);
            default:
                return false;  // Primitive, Unit, Function, Error
        }
    };

    // Positional record fields (§9.1) — never static, so no exclusion needed.
    for (const ast::Param& f : decl->positionalFields) {
        if (!f.type) continue;
        if (fieldIsBorrowCarrying(ctx.resolvedType(*f.type))) return true;
    }
    // Body-record reference fields (§9.2); static fields are per-TYPE storage
    // and excluded (mirrors Resolver::recordValueContainsSelf's own exclusion).
    for (const auto& m : decl->members) {
        if (auto* fd = dynamic_cast<const ast::FieldDecl*>(m.get())) {
            bool isStatic = false;
            for (std::string_view mod : fd->modifiers)
                if (mod == "static") { isStatic = true; break; }
            if (isStatic) continue;
            if (fd->type && fieldIsBorrowCarrying(ctx.resolvedType(*fd->type)))
                return true;
        }
    }
    return false;
}

// §28.3 (v0.194, #1801) helpers for the bare-type-parameter source set. Kept in
// this shared core so the ownership pass reaches ONE copy.

// Mirrors MethodOwnership::isBorrowableType (Ownership_Checks.cpp): true when a
// resolved argument type is a borrow-bearing class/interface reference. A
// value type, a value-builtin, a function value, `string`/`Object`
// (runtime-managed primitives), and a bare generic-param arg type (null
// declaration) are NOT borrowable — such an argument carries no lifetime
// constraint into the result (§28.3: "any argument … whose substituted type is
// a value or runtime-managed type[] contributes no constraint"). Unknown /
// unresolved → conservative (borrowable, so the source is kept).
bool bareArgIsBorrowable(const sema::TypeRef* rt) {
    if (!rt) return true;  // unknown → conservative
    switch (rt->kind) {
        case sema::TypeRef::Kind::Primitive:
        case sema::TypeRef::Kind::Function:
            return false;
        case sema::TypeRef::Kind::Named:
            if (!rt->declaration) return false;  // bare generic param
            if (rt->declaration->isValueBuiltin) return false;
            return rt->declaration->kind == ast::TypeDeclKind::Class ||
                   rt->declaration->kind == ast::TypeDeclKind::Interface;
        case sema::TypeRef::Kind::Error:
            return true;  // unresolved → conservative
        default:
            return false;  // Unit, InlineArray, enum, record
    }
}

// The identifier root of an access chain (`buf.f[i].g` → `buf`, a bare `a` →
// `a`) resolved to its symbol — mirrors MethodOwnership::identifierRoot +
// ctx.exprSymbol, replicated here (that peel is an ownership-pass member, a
// layer above common/). Null for a non-identifier-rooted expression.
sema::Symbol* bareRootSymbolOf(const ast::Expr& e, const sema::SemaContext& ctx) {
    const ast::Expr* cur = &e;
    while (cur) {
        if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(cur))
            return ctx.exprSymbol(*id);
        if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(cur))
            cur = ma->target.get();
        else if (auto* nm = dynamic_cast<const ast::NullCondMemberExpr*>(cur))
            cur = nm->target.get();
        else if (auto* c = dynamic_cast<const ast::CallExpr*>(cur))
            cur = c->callee.get();
        else if (auto* ix = dynamic_cast<const ast::IndexExpr*>(cur))
            cur = ix->target.get();
        else
            return nullptr;
    }
    return nullptr;
}

// §28.3: does the declared parameter type `t` MENTION the returned type
// parameter `name`? A bare `T`, and any nesting that reaches it (`List<T>`,
// `[N]T`, `(T) -> ()`), qualifies — so an argument that carries `T` as an
// element/component is a source of the returned `T`.
bool bareTypeMentionsParam(const ast::Type& t, std::string_view name) {
    if (t.kind == ast::TypeKind::Named && t.name &&
        t.name->parts.size() == 1 && t.name->parts[0] == name)
        return true;
    for (const auto& ga : t.genericArgs)
        if (ga && bareTypeMentionsParam(*ga, name)) return true;
    if (t.arrayElement && bareTypeMentionsParam(*t.arrayElement, name))
        return true;
    if (t.funcReturn && bareTypeMentionsParam(*t.funcReturn, name)) return true;
    for (const auto& fp : t.funcParams)
        if (fp.type && bareTypeMentionsParam(*fp.type, name)) return true;
    return false;
}

// The single-part head name of a method's declared return type, or empty.
std::string_view bareReturnHead(const ast::MethodDecl& md) {
    if (md.returnType && md.returnType->name &&
        md.returnType->name->parts.size() == 1)
        return md.returnType->name->parts[0];
    return {};
}

}  // namespace

bool isBorrowBearingRecordOrSpan(const sema::TypeRef* rt,
                                 const sema::SemaContext& ctx) {
    if (!rt) return false;
    // Builtin span VIEWS carry a borrow of their backing Array/OwningBuffer
    // but have no user Record TypeDecl, so the record walk below misses them.
    // CancellationToken (§30.8) is a value builtin with no Record decl and is
    // not in this list — correctly not flagged; likewise the string-backed
    // spans (runtime-managed `string`, §9.7).
    const std::string_view n = borrowSpanNameHead(rt);
    if (n == "Span" || n == "ReadOnlySpan" ||
        n == "ArraySpan" || n == "ReadOnlyArraySpan")
        return true;
    std::unordered_set<const ast::TypeDecl*> seen;
    return isBorrowBearingRecordCommon(rt, ctx, seen);
}

bool declaredReturnIsBareTypeParam(const ast::MethodDecl& md,
                                   const sema::SemaContext& ctx) {
    // `owned T` is the ordinary ownership-transferring return (§28.8), not a
    // bare parameter — it is classified Owned, outside this rule.
    if (md.returnOwned) return false;
    if (!md.returnType) return false;
    // A bare type parameter resolves to a Named TypeRef with a NULL declaration
    // (TypeResolution_Names.cpp's isGeneric branch). Nullability is stripped —
    // the `nullable` flag lives on the TypeRef, not the head — so `T?` qualifies.
    const sema::TypeRef* rt = ctx.resolvedType(*md.returnType);
    if (!rt || rt->kind != sema::TypeRef::Kind::Named ||
        rt->declaration != nullptr)
        return false;
    // Distinguish a bare type parameter from a 0-arity builtin (`Task`, `Object`
    // via its class form, …), which ALSO resolves to a null-declaration Named
    // ref: require the head to be one of `md`'s own (method-level) generic
    // parameters. The enclosing-type-parameter case is matched at the call site
    // (the caller has the callee symbol's parent type). // per §28.3, v0.194
    const std::string_view head = bareReturnHead(md);
    if (head.empty()) return false;
    for (const ast::GenericParam& g : md.genericParams)
        if (g.name == head) return true;
    return false;
}

std::vector<sema::Symbol*> bareTypeParamCallSourceRoots(
    const ast::CallExpr& ce, const ast::MethodDecl& md,
    const sema::SemaContext& ctx) {
    std::vector<sema::Symbol*> roots;
    const std::string_view retParam = bareReturnHead(md);
    if (retParam.empty()) return roots;

    bool isStatic = false;
    for (std::string_view m : md.modifiers)
        if (m == "static") { isStatic = true; break; }

    // Receiver source: for an instance / extension callee the returned borrow
    // may alias the receiver (§28.3 "the call's receiver … together with …").
    // A STATIC call has no value receiver — its callee target is a type name,
    // never a consumable binding — so it contributes none.
    if (!isStatic && ce.callee)
        if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(ce.callee.get()))
            if (ma->target)
                if (sema::Symbol* s = bareRootSymbolOf(*ma->target, ctx))
                    roots.push_back(s);

    // Positional plain (non-`owned`) parameters whose declared type mentions the
    // returned parameter; keep only borrowable arguments. `owned` and `out`
    // parameters are never sources (the former is consumed into the callee and
    // dies there; the latter is an output). Named/discard/out-of-range args are
    // skipped (positional keying only, §28.3).
    for (std::size_t i = 0; i < md.params.size(); ++i) {
        const ast::Param& p = md.params[i];
        if (p.modifier.find("owned") != std::string_view::npos) continue;
        if (p.modifier.find("out") != std::string_view::npos) continue;
        if (!p.type || !bareTypeMentionsParam(*p.type, retParam)) continue;
        if (i >= ce.args.size()) continue;
        const ast::CallArgument& arg = ce.args[i];
        if (arg.isDiscard || !arg.value || !arg.name.empty()) continue;
        if (!bareArgIsBorrowable(ctx.exprType(*arg.value))) continue;
        if (sema::Symbol* s = bareRootSymbolOf(*arg.value, ctx))
            roots.push_back(s);
    }
    return roots;
}

}  // namespace bezel::common
