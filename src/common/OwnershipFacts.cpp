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
        if (rt && rt->astNode && rt->astNode->name &&
            !rt->astNode->name->parts.empty())
            rcn = rt->astNode->name->parts[0];
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

}  // namespace bezel::common
