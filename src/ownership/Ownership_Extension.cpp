// Ownership_Extension.cpp — the §26.7 extension-receiver store check.
//
// fix #7968 (§26.7 / §28.3 / §28.9, v0.355).
//
// §26.7 "Ownership in extensions" (docs/Bezel_Language_Spec.md:6193) says of an
// extension member's receiver:
//
//   "The receiver is always a borrow; extensions cannot store `this`, return
//    it as `owned`, or dispose it."
//
// The rule is the extension-side tightening of §28.3's field-store bullet.
// §28.3 clause 2 admits a receiver-sourced borrow into a field ("borrows
// sourced from a parameter that is **not `owned`** — a plain or `scoped`
// parameter — or **from the receiver** are admissible"), so `this.o := this`
// in a DECLARED member is admitted under that clause's residual trust. §26.7
// forbids the same store in an EXTENSION body, where `this` is the extension's
// own receiver-parameter borrow.
//
// MEASURED (r39-fix-7968, on base 671722b8): an `extension Object` body that
// stored its receiver into an `Object?` field of another object compiled
// clean and then died at run time —
//   * `h.o := this` with a `StringBuilder` receiver → "bezel: fatal: refcount
//     over-release (box) — two releases for one reference; terminating per
//     §28.9" (rc=134), because `Emitter::collectBoxStorageFields` cannot see a
//     store made from an extension body and so arms the field's disposer with
//     `bz_box_release` on a class instance; and
//   * `h.Keep(this)` with an `OwningArray` / user-class receiver → an ASan
//     SEGV / OOM, the stored borrow outliving its source.
//
// The store this file REJECTS is the one the extension body performs itself:
// an ASSIGNMENT of the receiver (or of a binding that holds it) into a MEMBER
// SLOT — `h.o := this`, `h.o := alias`, `h.items[0] := this`.
//
// The CALL-ARGUMENT form — `h.Keep(this)`, where a callee stores the value it
// receives — is NOT this file's business, and is deliberately left admitted:
//
//   * §28.3 clause 3 (the residual) reads: "The caller's check reads only the
//     callee's signature (derives: K2.3 + K4.3 + K8.1): an unbound owned
//     temporary or a borrow of a scope-bound local, passed to a plain
//     parameter that the callee stores — directly, through a field path rooted
//     at it (`this.keep := x.child`), or as a borrow a call on it returns
//     (`this.keep := x.Self()`) — is admitted at the call, for a local,
//     virtual, interface or imported callee alike." The extension body is the
//     CALLER there, so its `this` is admitted at the call exactly as any other
//     plain parameter's value is; the callee's own check (§28.3 clause 2)
//     admits the store, because the callee sees a plain parameter.
//   * The conformance corpus pins that admission twice:
//     `tests/conformance/rules/cases/s28_3_0119.bzl` (`expect: accept`) — an
//     `extension Object` whose body calls `k.Put(this)` into an `Object?`
//     field — and `tests/conformance/rules/cases/s8_6_0011.bzl`
//     (`expect: run`, stdout `1`) — an `extension IShow` whose body calls
//     `k.Put(this)` into an `IShow?` field. Both are the very shape this
//     issue reports, and both must keep compiling.
//   * §26.7's own sentence names three operations the extension body performs
//     ON the receiver — a store, a return-as-`owned`, a dispose — and a call
//     is not one of them: the callee performs the store, and §28.3 clause 3
//     makes that the callee's business.
//
// So the residual trust clause, not this rule, governs the call-argument
// shape; the crash it can leave behind is the same one a program with no
// extension at all leaves behind (a plain method storing a scope-bound local
// through a callee), and §28.2's backstop is where that belongs.
//
// §26.7's rule is about a REFERENCE receiver. A value-kind receiver (§9.4) is
// COPIED by a store, so §9.4's own-storage rule — not this one — governs it;
// this check therefore fires only for a class-, interface- or `Object`-headed
// extension.
//
// A local bound to the receiver (`var a := this; h.o := a;`) is the SAME
// store, laundered through a binding, so the receiver's aliases are tracked
// (a fixpoint over the body's local declarations) and treated as the receiver
// itself.

#include "ast/Node.hpp"
#include "ownership/Ownership.hpp"
#include "ownership/StmtWalker.hpp"
#include "sema/SemaContext.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeRef.hpp"
#include "support/Diagnostic.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace bezel::ownership {

namespace {

// The local bindings that hold the extension's receiver. Keyed by the
// LocalDecl node — sema records it as the LocalSymbol's `decl`
// (ExprTypecheck.cpp's LocalDecl arm: `lsym->decl = ld;`) — so a same-named
// binding in another scope can never be mistaken for the alias.
using AliasDecls = std::unordered_set<const ast::Node*>;

// The walk state: the receiver's aliases and whether a diagnostic was
// already reported (one report per body is enough — the compiler reports the
// first offending site).
struct ExtensionScan {
    AliasDecls aliases;
    bool reported = false;
};

// §26.7 (fix #7968): does `e` evaluate to the extension's receiver, or to a
// binding that holds it?
//
// The pass-through arms are the ones §28.3 already treats as the same
// borrow: `!` (§17.8), `as` (§17.4), `??` (§28.3 K2.2's union), a
// conditional (§28.3's "either operand may be the value selected") and a
// `do … end` binding atom (§17.15). A MEMBER READ off the receiver
// (`this.f`) is NOT the receiver — it is a read of the receiver's storage —
// and a CALL result is not the receiver either.
bool isReceiverExpr(const ast::Expr* e, const AliasDecls& aliases,
                    const sema::SemaContext& ctx) {
    if (!e) return false;
    if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(e)) {
        if (id->name == "this") return true;
        const sema::Symbol* s = ctx.exprSymbol(*id);
        return s && aliases.count(s->decl) != 0;
    }
    if (auto* pb = dynamic_cast<const ast::PostfixBangExpr*>(e))
        return isReceiverExpr(pb->operand.get(), aliases, ctx);
    if (auto* tt = dynamic_cast<const ast::TypeTestExpr*>(e))
        return tt->op == "as" && isReceiverExpr(tt->operand.get(), aliases, ctx);
    if (auto* co = dynamic_cast<const ast::ConditionalExpr*>(e))
        return isReceiverExpr(co->thenValue.get(), aliases, ctx) ||
               isReceiverExpr(co->elseValue.get(), aliases, ctx);
    if (auto* bin = dynamic_cast<const ast::BinaryExpr*>(e))
        return bin->op == "??" &&
               (isReceiverExpr(bin->left.get(), aliases, ctx) ||
                isReceiverExpr(bin->right.get(), aliases, ctx));
    if (auto* bx = dynamic_cast<const ast::BindExpr*>(e))
        return isReceiverExpr(bx->body.get(), aliases, ctx);
    return false;
}

// Is `target` a MEMBER SLOT — a field / property / indexer write? The
// assignment-target classification §28.3's field-store bullet uses: a
// qualified member write, an indexer write, or a bare identifier that
// resolves to a field.
bool isMemberSlotTarget(const ast::Expr* target, const sema::SemaContext& ctx) {
    if (!target) return false;
    if (dynamic_cast<const ast::MemberAccessExpr*>(target)) return true;
    if (dynamic_cast<const ast::IndexExpr*>(target)) return true;
    if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(target)) {
        const sema::Symbol* s = ctx.exprSymbol(*id);
        return s && dynamic_cast<const sema::FieldSymbol*>(s) != nullptr;
    }
    return false;
}

// The §26.7 diagnostic for the MEMBER-SLOT store.
std::string memberSlotDiagnostic() {
    return "an extension member must not store its receiver: §26.7 — \"The "
           "receiver is always a borrow; extensions cannot store `this`, "
           "return it as `owned`, or dispose it.\" — the extension's `this` "
           "is a borrow of the receiver, and a member slot outlives the "
           "extension's activation, so storing it there would leave a "
           "dangling borrow behind (§28.3's field-store bullet admits this "
           "store in a DECLARED member's body — an extension body is the "
           "stricter case §26.7 names); store a value you own instead";
}

void reportStore(ExtensionScan& scan, sema::SemaContext& ctx,
                 SourceLocation loc, const std::string& msg) {
    if (scan.reported) return;
    scan.reported = true;
    ctx.diagnostics().report(Severity::Error, loc, msg);
}

// Forward declarations — the statement walk and the expression walk call each
// other (an expression may hold a lambda body, which is a statement list).
void walkStmtForReceiverStore(const ast::Stmt& s, ExtensionScan& scan,
                              sema::SemaContext& ctx);
void walkExprForReceiverStore(const ast::Expr* e, ExtensionScan& scan,
                              sema::SemaContext& ctx);

// The per-statement action: an assignment into a MEMBER SLOT is a store
// (§28.3's field-store bullet), and the extension body is the store's site.
// Every statement's EXPRESSION positions are walked too, so a lambda (or a
// `case` arm) nested in the body is reached.
void handleStmtForReceiverStore(const ast::Stmt& s, ExtensionScan& scan,
                                 sema::SemaContext& ctx) {
    if (auto* as = dynamic_cast<const ast::AssignmentStmt*>(&s)) {
        if (as->op == ":=" && as->target && as->value &&
            isMemberSlotTarget(as->target.get(), ctx) &&
            isReceiverExpr(as->value.get(), scan.aliases, ctx)) {
            reportStore(scan, ctx, as->value->span.begin, memberSlotDiagnostic());
        }
        walkExprForReceiverStore(as->target.get(), scan, ctx);
        walkExprForReceiverStore(as->value.get(), scan, ctx);
        return;
    }
    if (auto* ld = dynamic_cast<const ast::LocalDecl*>(&s)) {
        walkExprForReceiverStore(ld->initializer.get(), scan, ctx);
        return;
    }
    if (auto* lc = dynamic_cast<const ast::LocalConstDecl*>(&s)) {
        walkExprForReceiverStore(lc->value.get(), scan, ctx);
        return;
    }
    if (auto* es = dynamic_cast<const ast::ExpressionStmt*>(&s)) {
        walkExprForReceiverStore(es->expr.get(), scan, ctx);
        return;
    }
    if (auto* rs = dynamic_cast<const ast::ReturnStmt*>(&s)) {
        walkExprForReceiverStore(rs->value.get(), scan, ctx);
        return;
    }
    if (auto* ys = dynamic_cast<const ast::YieldStmt*>(&s)) {
        walkExprForReceiverStore(ys->value.get(), scan, ctx);
        return;
    }
    if (auto* ts = dynamic_cast<const ast::ThrowStmt*>(&s)) {
        walkExprForReceiverStore(ts->value.get(), scan, ctx);
        return;
    }
    if (auto* ifs = dynamic_cast<const ast::IfStmt*>(&s)) {
        walkExprForReceiverStore(ifs->ifBranch.condition.get(), scan, ctx);
        for (const auto& el : ifs->elifBranches)
            walkExprForReceiverStore(el.condition.get(), scan, ctx);
        return;
    }
    if (auto* ws = dynamic_cast<const ast::WhileStmt*>(&s)) {
        walkExprForReceiverStore(ws->condition.get(), scan, ctx);
        return;
    }
    if (auto* fs = dynamic_cast<const ast::ForStmt*>(&s)) {
        walkExprForReceiverStore(fs->iterable.get(), scan, ctx);
        walkExprForReceiverStore(fs->withExpr.get(), scan, ctx);
        return;
    }
    if (auto* cs = dynamic_cast<const ast::CaseStmt*>(&s)) {
        walkExprForReceiverStore(cs->scrutinee.get(), scan, ctx);
        for (const auto& arm : cs->arms)
            walkExprForReceiverStore(arm.guard.get(), scan, ctx);
        return;
    }
}

// The expression walk: an EXPRESSION may hold STATEMENTS — a lambda body's
// statement list, a `case` arm's block body — and a store of the receiver
// there is a store in the extension's body too (the closure captures the
// receiver, so the store is even later than the extension's activation). The
// walk descends into every expression position to reach those statement
// lists.
void walkExprForReceiverStore(const ast::Expr* e, ExtensionScan& scan,
                              sema::SemaContext& ctx) {
    if (!e) return;
    if (auto* lam = dynamic_cast<const ast::LambdaExpr*>(e)) {
        if (lam->isBlockBody) {
            for (const auto& s : lam->stmts)
                if (s) walkStmtForReceiverStore(*s, scan, ctx);
        }
        walkExprForReceiverStore(lam->exprBody.get(), scan, ctx);
        return;
    }
    if (auto* ce = dynamic_cast<const ast::CaseExpr*>(e)) {
        walkExprForReceiverStore(ce->scrutinee.get(), scan, ctx);
        for (const auto& arm : ce->arms) {
            walkExprForReceiverStore(arm.guard.get(), scan, ctx);
            walkExprForReceiverStore(arm.exprBody.get(), scan, ctx);
            for (const auto& st : arm.stmts)
                walkStmtForReceiverStore(*st, scan, ctx);
        }
        return;
    }
    if (auto* ce = dynamic_cast<const ast::CallExpr*>(e)) {
        walkExprForReceiverStore(ce->callee.get(), scan, ctx);
        for (const auto& a : ce->args)
            walkExprForReceiverStore(a.value.get(), scan, ctx);
        return;
    }
    if (auto* ce = dynamic_cast<const ast::NullCondCallExpr*>(e)) {
        for (const auto& a : ce->args)
            walkExprForReceiverStore(a.value.get(), scan, ctx);
        return;
    }
    if (auto* b = dynamic_cast<const ast::BinaryExpr*>(e)) {
        walkExprForReceiverStore(b->left.get(), scan, ctx);
        walkExprForReceiverStore(b->right.get(), scan, ctx);
        return;
    }
    if (auto* u = dynamic_cast<const ast::UnaryExpr*>(e)) {
        walkExprForReceiverStore(u->operand.get(), scan, ctx);
        return;
    }
    if (auto* p = dynamic_cast<const ast::PostfixBangExpr*>(e)) {
        walkExprForReceiverStore(p->operand.get(), scan, ctx);
        return;
    }
    if (auto* m = dynamic_cast<const ast::MemberAccessExpr*>(e)) {
        walkExprForReceiverStore(m->target.get(), scan, ctx);
        return;
    }
    if (auto* m = dynamic_cast<const ast::NullCondMemberExpr*>(e)) {
        walkExprForReceiverStore(m->target.get(), scan, ctx);
        return;
    }
    if (auto* ix = dynamic_cast<const ast::IndexExpr*>(e)) {
        walkExprForReceiverStore(ix->target.get(), scan, ctx);
        for (const auto& a : ix->indices)
            walkExprForReceiverStore(a.value.get(), scan, ctx);
        return;
    }
    if (auto* t = dynamic_cast<const ast::TypeTestExpr*>(e)) {
        walkExprForReceiverStore(t->operand.get(), scan, ctx);
        return;
    }
    if (auto* co = dynamic_cast<const ast::ConditionalExpr*>(e)) {
        walkExprForReceiverStore(co->condition.get(), scan, ctx);
        walkExprForReceiverStore(co->thenValue.get(), scan, ctx);
        walkExprForReceiverStore(co->elseValue.get(), scan, ctx);
        return;
    }
    if (auto* n = dynamic_cast<const ast::NewExpr*>(e)) {
        for (const auto& a : n->args)
            walkExprForReceiverStore(a.value.get(), scan, ctx);
        return;
    }
    if (auto* w = dynamic_cast<const ast::WithExpr*>(e)) {
        walkExprForReceiverStore(w->source.get(), scan, ctx);
        for (const auto& a : w->assigns)
            walkExprForReceiverStore(a.value.get(), scan, ctx);
        return;
    }
    if (auto* cl = dynamic_cast<const ast::CollectionLiteralExpr*>(e)) {
        for (const auto& el : cl->elements)
            walkExprForReceiverStore(el.get(), scan, ctx);
        for (const auto& k : cl->keys)
            walkExprForReceiverStore(k.get(), scan, ctx);
        return;
    }
    if (auto* is = dynamic_cast<const ast::InterpolatedStringExpr*>(e)) {
        for (const auto& part : is->parts)
            walkExprForReceiverStore(part.expr.get(), scan, ctx);
        return;
    }
    if (auto* bx = dynamic_cast<const ast::BindExpr*>(e)) {
        for (const auto& c : bx->clauses)
            walkStmtForReceiverStore(*c, scan, ctx);
        walkExprForReceiverStore(bx->body.get(), scan, ctx);
        return;
    }
    if (auto* th = dynamic_cast<const ast::ThrowExpr*>(e)) {
        walkExprForReceiverStore(th->operand.get(), scan, ctx);
        return;
    }
}

// The statement walk. `forEachNestedStmt` (StmtWalker.hpp) owns the
// statement grammar — every container kind is reached through it, so a store
// nested in an `if` / `while` / `for` / `do` / `try` / `case` body is seen.
void walkStmtForReceiverStore(const ast::Stmt& s, ExtensionScan& scan,
                              sema::SemaContext& ctx) {
    forEachNestedStmt(s, [&](const ast::Stmt& inner) {
        handleStmtForReceiverStore(inner, scan, ctx);
    });
}

// The receiver's ALIASES: the local declarations whose initializer is the
// receiver (or an alias of it). A fixpoint over the body's declarations, so
// `var a := this; var b := a;` marks both.
void collectAliases(const std::vector<std::unique_ptr<ast::Stmt>>& stmts,
                    ExtensionScan& scan, const sema::SemaContext& ctx) {
    // Collect the candidate declarations first (every LocalDecl with an
    // initializer), then iterate to a fixpoint over the alias relation.
    std::vector<const ast::LocalDecl*> decls;
    for (const auto& s : stmts) {
        if (!s) continue;
        forEachNestedStmt(*s, [&](const ast::Stmt& inner) {
            if (auto* ld = dynamic_cast<const ast::LocalDecl*>(&inner))
                if (ld->initializer) decls.push_back(ld);
        });
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (const ast::LocalDecl* ld : decls) {
            if (scan.aliases.count(ld)) continue;
            if (!isReceiverExpr(ld->initializer.get(), scan.aliases, ctx)) continue;
            scan.aliases.insert(ld);
            changed = true;
        }
    }
}

// Is the extension's receiver a REFERENCE receiver? §26.7's store rule is
// about a borrow; a value-kind receiver (§9.4) is COPIED by a store, so §9.4's
// own-storage rule — not this one — governs it.
bool receiverIsReference(const ast::ExtensionDecl& ext, sema::SemaContext& ctx) {
    if (!ext.targetType) return false;
    const sema::TypeRef* t = ctx.resolvedType(*ext.targetType);
    if (!t) return false;
    switch (t->kind) {
        case sema::TypeRef::Kind::Primitive:
            // §4: `Object` is the root reference type.
            return t->primitive == sema::PrimitiveType::Object;
        case sema::TypeRef::Kind::Named: {
            if (!t->declaration) return false;
            if (t->declaration->isValueBuiltin) return false;
            return t->declaration->kind == ast::TypeDeclKind::Class ||
                   t->declaration->kind == ast::TypeDeclKind::Interface;
        }
        default:
            return false;
    }
}

} // namespace

// fix #7968 (§26.7 / §28.3 / §28.9, v0.355): the extension-receiver store
// check. See the file header for the rule and the store position.
void checkExtensionReceiverStore(const ast::MethodDecl& md,
                                 const ast::ExtensionDecl& ext,
                                 sema::SemaContext& ctx) {
    if (!receiverIsReference(ext, ctx)) return;
    ExtensionScan scan;
    if (md.bodyKind == ast::MethodBodyKind::Block) {
        collectAliases(md.stmts, scan, ctx);
        for (const auto& s : md.stmts)
            if (s) walkStmtForReceiverStore(*s, scan, ctx);
    } else if (md.bodyKind == ast::MethodBodyKind::Expression && md.exprBody) {
        // An expression body has no local declarations of its own, so the
        // alias set is empty; it may still hold a lambda (a closure over the
        // receiver), so its expression positions are walked.
        walkExprForReceiverStore(md.exprBody.get(), scan, ctx);
    }
}

} // namespace bezel::ownership
