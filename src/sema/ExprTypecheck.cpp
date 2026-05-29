#include "sema/ExprTypecheck.hpp"

#include "ast/Node.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeRef.hpp"

namespace bezel::sema {

namespace {

class ExprTC {
public:
    ExprTC(SemaContext& ctx) : ctx_(ctx) {}

    void run(const ast::SourceFile& src) {
        for (const auto& decl : src.topDecls) {
            if (!decl) continue;
            visitTopDecl(*decl);
        }
    }

private:
    SemaContext& ctx_;

    void visitTopDecl(const ast::TopDecl& d) {
        if (auto* td = dynamic_cast<const ast::TypeDecl*>(&d)) {
            for (const auto& ec : td->enumConstants) {
                if (ec.value) visitExpr(*ec.value);
            }
            for (const auto& m : td->members) {
                if (m) visitMember(*m);
            }
        } else if (auto* ext = dynamic_cast<const ast::ExtensionDecl*>(&d)) {
            for (const auto& m : ext->members) if (m) visitMember(*m);
        }
        // TypeAlias, ExternBlock have no Expressions in their declarations
        // (aside from default values in extern method params, which we'd
        // pick up via Param iteration — extern doesn't admit defaults
        // per the grammar).
    }

    void visitMember(const ast::Member& m) {
        if (auto* fd = dynamic_cast<const ast::FieldDecl*>(&m)) {
            if (fd->initializer) visitExpr(*fd->initializer);
        } else if (auto* md = dynamic_cast<const ast::MethodDecl*>(&m)) {
            for (const auto& p : md->params) {
                if (p.defaultValue) visitExpr(*p.defaultValue);
            }
            if (md->bodyKind == ast::MethodBodyKind::Expression &&
                md->exprBody) {
                visitExpr(*md->exprBody);
            } else if (md->bodyKind == ast::MethodBodyKind::Block) {
                for (const auto& s : md->stmts) {
                    if (s) visitStmt(*s);
                }
            }
        } else if (auto* od = dynamic_cast<const ast::OperatorDecl*>(&m)) {
            for (const auto& p : od->params) {
                if (p.defaultValue) visitExpr(*p.defaultValue);
            }
            if (od->bodyKind == ast::MethodBodyKind::Expression &&
                od->exprBody) {
                visitExpr(*od->exprBody);
            } else if (od->bodyKind == ast::MethodBodyKind::Block) {
                for (const auto& s : od->stmts) {
                    if (s) visitStmt(*s);
                }
            }
        } else if (auto* cd = dynamic_cast<const ast::ConstructorDecl*>(&m)) {
            for (const auto& p : cd->params) {
                if (p.defaultValue) visitExpr(*p.defaultValue);
            }
            for (const auto& s : cd->body) {
                if (s) visitStmt(*s);
            }
        } else if (auto* dd = dynamic_cast<const ast::DestructorDecl*>(&m)) {
            for (const auto& s : dd->body) {
                if (s) visitStmt(*s);
            }
        } else if (auto* pd = dynamic_cast<const ast::PropertyDecl*>(&m)) {
            if (pd->exprBody) visitExpr(*pd->exprBody);
            if (pd->autoInitializer) visitExpr(*pd->autoInitializer);
            for (const auto& a : pd->accessors) {
                if (a.exprBody) visitExpr(*a.exprBody);
                for (const auto& s : a.stmts) if (s) visitStmt(*s);
            }
        } else if (auto* cd = dynamic_cast<const ast::ConstDecl*>(&m)) {
            if (cd->value) visitExpr(*cd->value);
        } else if (auto* nt = dynamic_cast<const ast::NestedTypeMember*>(&m)) {
            if (nt->decl) {
                for (const auto& innerM : nt->decl->members) {
                    if (innerM) visitMember(*innerM);
                }
            }
        }
        // Event/EventDecl, indexer accessors handled by PropertyDecl path.
    }

    void visitStmt(const ast::Stmt& s) {
        if (auto* ld = dynamic_cast<const ast::LocalDecl*>(&s)) {
            if (ld->initializer) visitExpr(*ld->initializer);
        } else if (auto* lc =
                       dynamic_cast<const ast::LocalConstDecl*>(&s)) {
            if (lc->value) visitExpr(*lc->value);
        } else if (auto* rs = dynamic_cast<const ast::ReturnStmt*>(&s)) {
            if (rs->value) visitExpr(*rs->value);
        } else if (auto* es = dynamic_cast<const ast::ExpressionStmt*>(&s)) {
            if (es->expr) visitExpr(*es->expr);
        } else if (auto* as = dynamic_cast<const ast::AssignmentStmt*>(&s)) {
            if (as->target) visitExpr(*as->target);
            if (as->value) visitExpr(*as->value);
        } else if (auto* ds = dynamic_cast<const ast::DiscardStmt*>(&s)) {
            if (ds->value) visitExpr(*ds->value);
        } else if (auto* ts = dynamic_cast<const ast::ThrowStmt*>(&s)) {
            if (ts->value) visitExpr(*ts->value);
        } else if (auto* ifs = dynamic_cast<const ast::IfStmt*>(&s)) {
            if (ifs->ifBranch.condition) visitExpr(*ifs->ifBranch.condition);
            for (const auto& bs : ifs->ifBranch.body) {
                if (bs) visitStmt(*bs);
            }
            for (const auto& el : ifs->elifBranches) {
                if (el.condition) visitExpr(*el.condition);
                for (const auto& bs : el.body) if (bs) visitStmt(*bs);
            }
            for (const auto& bs : ifs->elseBody) if (bs) visitStmt(*bs);
        } else if (auto* ws = dynamic_cast<const ast::WhileStmt*>(&s)) {
            if (ws->condition) visitExpr(*ws->condition);
            for (const auto& bs : ws->body) if (bs) visitStmt(*bs);
        } else if (auto* fs = dynamic_cast<const ast::ForStmt*>(&s)) {
            if (fs->iterable) visitExpr(*fs->iterable);
            if (fs->withExpr) visitExpr(*fs->withExpr);
            for (const auto& bs : fs->body) if (bs) visitStmt(*bs);
        } else if (auto* dob = dynamic_cast<const ast::DoEndBlock*>(&s)) {
            for (const auto& bs : dob->body) if (bs) visitStmt(*bs);
        } else if (auto* trys = dynamic_cast<const ast::TryStmt*>(&s)) {
            for (const auto& bs : trys->body) if (bs) visitStmt(*bs);
            for (const auto& arm : trys->catches) {
                if (arm.guard) visitExpr(*arm.guard);
                if (arm.body) visitStmt(*arm.body);
            }
            for (const auto& bs : trys->finallyBody) if (bs) visitStmt(*bs);
        } else if (auto* ys = dynamic_cast<const ast::YieldStmt*>(&s)) {
            if (ys->value) visitExpr(*ys->value);
        } else if (auto* is = dynamic_cast<const ast::IncrementStmt*>(&s)) {
            if (is->target) visitExpr(*is->target);
        } else if (auto* cs = dynamic_cast<const ast::CaseStmt*>(&s)) {
            if (cs->scrutinee) visitExpr(*cs->scrutinee);
            for (const auto& arm : cs->arms) {
                if (arm.guard) visitExpr(*arm.guard);
                if (arm.exprBody) visitExpr(*arm.exprBody);
                for (const auto& bs : arm.stmts) if (bs) visitStmt(*bs);
            }
        }
        // Break, Continue have no expressions.
    }

    void visitExpr(const ast::Expr& e) {
        // Slice 3 main contract: every literal gets a type.
        if (dynamic_cast<const ast::IntLiteralExpr*>(&e)) {
            ctx_.setExprType(&e, ctx_.primitiveTypeRef(PrimitiveType::Int));
            return;
        }
        if (dynamic_cast<const ast::FloatLiteralExpr*>(&e)) {
            ctx_.setExprType(
                &e, ctx_.primitiveTypeRef(PrimitiveType::Double));
            return;
        }
        if (dynamic_cast<const ast::CharLiteralExpr*>(&e)) {
            ctx_.setExprType(&e, ctx_.primitiveTypeRef(PrimitiveType::Char));
            return;
        }
        if (dynamic_cast<const ast::StringLiteralExpr*>(&e) ||
            dynamic_cast<const ast::VerbatimStringExpr*>(&e) ||
            dynamic_cast<const ast::InterpolatedStringExpr*>(&e)) {
            ctx_.setExprType(
                &e, ctx_.primitiveTypeRef(PrimitiveType::String));
            return;
        }
        if (dynamic_cast<const ast::BoolLiteralExpr*>(&e)) {
            ctx_.setExprType(&e, ctx_.primitiveTypeRef(PrimitiveType::Bool));
            return;
        }
        if (dynamic_cast<const ast::NullLiteralExpr*>(&e)) {
            // null is context-typed; absent the context we annotate with
            // nullable Object. Later passes refine when there's a target
            // type to constrain against.
            ctx_.setExprType(
                &e, ctx_.primitiveTypeRef(PrimitiveType::Object,
                                          /*nullable=*/true));
            return;
        }
        if (dynamic_cast<const ast::UnitExpr*>(&e)) {
            // Build a unit TypeRef on first use (no primitive entry).
            auto* unit = ctx_.makeTypeRef();
            unit->kind = TypeRef::Kind::Unit;
            ctx_.setExprType(&e, unit);
            return;
        }
        // For non-literal expressions, recurse into children so their
        // own literals get typed. The expression itself gets no type
        // (later slices wire it up).
        visitChildren(e);
    }

    void visitChildren(const ast::Expr& e) {
        if (auto* be = dynamic_cast<const ast::BinaryExpr*>(&e)) {
            if (be->left) visitExpr(*be->left);
            if (be->right) visitExpr(*be->right);
        } else if (auto* ue = dynamic_cast<const ast::UnaryExpr*>(&e)) {
            if (ue->operand) visitExpr(*ue->operand);
        } else if (auto* ma =
                       dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
            if (ma->target) visitExpr(*ma->target);
        } else if (auto* nm =
                       dynamic_cast<const ast::NullCondMemberExpr*>(&e)) {
            if (nm->target) visitExpr(*nm->target);
        } else if (auto* ce = dynamic_cast<const ast::CallExpr*>(&e)) {
            if (ce->callee) visitExpr(*ce->callee);
            for (const auto& a : ce->args) {
                if (a.value) visitExpr(*a.value);
            }
        } else if (auto* nc = dynamic_cast<const ast::NullCondCallExpr*>(&e)) {
            if (nc->callee) visitExpr(*nc->callee);
            for (const auto& a : nc->args) {
                if (a.value) visitExpr(*a.value);
            }
        } else if (auto* ix = dynamic_cast<const ast::IndexExpr*>(&e)) {
            if (ix->target) visitExpr(*ix->target);
            for (const auto& a : ix->indices) {
                if (a.value) visitExpr(*a.value);
            }
        } else if (auto* bg = dynamic_cast<const ast::PostfixBangExpr*>(&e)) {
            if (bg->operand) visitExpr(*bg->operand);
        } else if (auto* ne = dynamic_cast<const ast::NewExpr*>(&e)) {
            for (const auto& a : ne->args) {
                if (a.value) visitExpr(*a.value);
            }
        } else if (auto* ce = dynamic_cast<const ast::ConditionalExpr*>(&e)) {
            if (ce->condition) visitExpr(*ce->condition);
            if (ce->thenValue) visitExpr(*ce->thenValue);
            if (ce->elseValue) visitExpr(*ce->elseValue);
        } else if (auto* te = dynamic_cast<const ast::TypeTestExpr*>(&e)) {
            if (te->operand) visitExpr(*te->operand);
        } else if (auto* th = dynamic_cast<const ast::ThrowExpr*>(&e)) {
            if (th->operand) visitExpr(*th->operand);
        } else if (auto* lam = dynamic_cast<const ast::LambdaExpr*>(&e)) {
            if (lam->exprBody) visitExpr(*lam->exprBody);
            for (const auto& s : lam->stmts) if (s) visitStmt(*s);
        } else if (auto* ce = dynamic_cast<const ast::CaseExpr*>(&e)) {
            if (ce->scrutinee) visitExpr(*ce->scrutinee);
            for (const auto& arm : ce->arms) {
                if (arm.guard) visitExpr(*arm.guard);
                if (arm.exprBody) visitExpr(*arm.exprBody);
                for (const auto& s : arm.stmts) if (s) visitStmt(*s);
            }
        } else if (auto* we = dynamic_cast<const ast::WithExpr*>(&e)) {
            if (we->source) visitExpr(*we->source);
            for (const auto& ia : we->assigns) {
                if (ia.value) visitExpr(*ia.value);
            }
        } else if (auto* cl =
                       dynamic_cast<const ast::CollectionLiteralExpr*>(&e)) {
            for (const auto& k : cl->keys) if (k) visitExpr(*k);
            for (const auto& v : cl->elements) if (v) visitExpr(*v);
        }
        // Identifier / This / Base / TypeOf / NameOf / Default —
        // no expression children to recurse into.
    }
};

} // namespace

bool typecheckExpressions(const ast::SourceFile& src, SemaContext& ctx) {
    ExprTC tc(ctx);
    tc.run(src);
    return !ctx.diagnostics().hasErrors();
}

} // namespace bezel::sema
