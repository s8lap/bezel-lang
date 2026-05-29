#include "sema/ExprTypecheck.hpp"

#include <memory>
#include <unordered_map>

#include "ast/Node.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeRef.hpp"

namespace bezel::sema {

namespace {

// Per-walker scope for identifier lookups inside method bodies (slice 3b).
struct Scope {
    std::unordered_map<std::string_view, Symbol*> names;
    Scope* parent = nullptr;
};

class ExprTC {
public:
    ExprTC(SemaContext& ctx) : ctx_(ctx) {}

    void run(const ast::SourceFile& src) {
        moduleScope_ = std::make_unique<Scope>();
        if (ctx_.module()) {
            for (Symbol* child : ctx_.module()->children) {
                if (!child->name.empty()) {
                    moduleScope_->names[child->name] = child;
                }
            }
        }
        for (const auto& decl : src.topDecls) {
            if (!decl) continue;
            visitTopDecl(*decl);
        }
    }

private:
    SemaContext& ctx_;
    std::unique_ptr<Scope> moduleScope_;
    Scope* currentScope_ = nullptr;
    TypeSymbol* currentType_ = nullptr;

    Symbol* lookupName(std::string_view name) {
        for (Scope* s = currentScope_; s; s = s->parent) {
            auto it = s->names.find(name);
            if (it != s->names.end()) return it->second;
        }
        if (currentType_) {
            for (Symbol* m : currentType_->members) {
                if (m && m->name == name) return m;
            }
        }
        return nullptr;
    }

    void visitTopDecl(const ast::TopDecl& d) {
        if (auto* td = dynamic_cast<const ast::TypeDecl*>(&d)) {
            TypeSymbol* tsym = nullptr;
            if (ctx_.module()) {
                for (Symbol* child : ctx_.module()->children) {
                    if (auto* ts = dynamic_cast<TypeSymbol*>(child)) {
                        if (ts->decl == td) { tsym = ts; break; }
                    }
                }
            }
            TypeSymbol* save = currentType_;
            currentType_ = tsym;
            for (const auto& ec : td->enumConstants) {
                if (ec.value) visitExpr(*ec.value);
            }
            for (const auto& m : td->members) {
                if (m) visitMember(*m);
            }
            currentType_ = save;
        } else if (auto* ext = dynamic_cast<const ast::ExtensionDecl*>(&d)) {
            for (const auto& m : ext->members) if (m) visitMember(*m);
        }
    }

    // Param-symbol lookup for the in-progress method/ctor on currentType_.
    const std::vector<Symbol*>* paramSymbolsOf(const ast::Member& m) const {
        if (!currentType_) return nullptr;
        for (Symbol* child : currentType_->members) {
            if (!child->decl) continue;
            if (child->decl == &m) {
                if (auto* ms = dynamic_cast<MethodSymbol*>(child))
                    return &ms->params;
                if (auto* cs = dynamic_cast<ConstructorSymbol*>(child))
                    return &cs->params;
            }
        }
        return nullptr;
    }

    // Build a method scope (params on top of module scope), then visit
    // body. Locals are registered into this scope as visitStmt walks
    // LocalDecls.
    void runMethodBody(
        const std::vector<ast::Param>& params,
        const std::vector<std::unique_ptr<ast::Stmt>>* stmts,
        const ast::Expr* exprBody,
        const std::vector<Symbol*>* paramSyms) {
        Scope methodScope;
        methodScope.parent = moduleScope_.get();
        if (paramSyms) {
            for (std::size_t i = 0;
                 i < params.size() && i < paramSyms->size(); ++i) {
                if ((*paramSyms)[i] && !params[i].name.empty()) {
                    methodScope.names[params[i].name] = (*paramSyms)[i];
                }
            }
        }
        Scope* save = currentScope_;
        currentScope_ = &methodScope;
        for (const auto& p : params) {
            if (p.defaultValue) visitExpr(*p.defaultValue);
        }
        if (exprBody) visitExpr(*exprBody);
        if (stmts) {
            for (const auto& s : *stmts) {
                if (s) visitStmt(*s);
            }
        }
        currentScope_ = save;
    }

    void visitMember(const ast::Member& m) {
        if (auto* fd = dynamic_cast<const ast::FieldDecl*>(&m)) {
            if (fd->initializer) visitExpr(*fd->initializer);
        } else if (auto* md = dynamic_cast<const ast::MethodDecl*>(&m)) {
            const std::vector<std::unique_ptr<ast::Stmt>>* stmts =
                (md->bodyKind == ast::MethodBodyKind::Block) ? &md->stmts
                                                             : nullptr;
            const ast::Expr* expr =
                (md->bodyKind == ast::MethodBodyKind::Expression)
                    ? md->exprBody.get() : nullptr;
            runMethodBody(md->params, stmts, expr, paramSymbolsOf(m));
        } else if (auto* od = dynamic_cast<const ast::OperatorDecl*>(&m)) {
            const std::vector<std::unique_ptr<ast::Stmt>>* stmts =
                (od->bodyKind == ast::MethodBodyKind::Block) ? &od->stmts
                                                             : nullptr;
            const ast::Expr* expr =
                (od->bodyKind == ast::MethodBodyKind::Expression)
                    ? od->exprBody.get() : nullptr;
            runMethodBody(od->params, stmts, expr, paramSymbolsOf(m));
        } else if (auto* cd = dynamic_cast<const ast::ConstructorDecl*>(&m)) {
            runMethodBody(cd->params, &cd->body, nullptr,
                          paramSymbolsOf(m));
        } else if (auto* dd = dynamic_cast<const ast::DestructorDecl*>(&m)) {
            std::vector<ast::Param> none;
            runMethodBody(none, &dd->body, nullptr, nullptr);
        } else if (auto* pd = dynamic_cast<const ast::PropertyDecl*>(&m)) {
            std::vector<ast::Param> none;
            if (pd->exprBody)
                runMethodBody(none, nullptr, pd->exprBody.get(), nullptr);
            if (pd->autoInitializer)
                visitExpr(*pd->autoInitializer);
            for (const auto& a : pd->accessors) {
                if (a.exprBody)
                    runMethodBody(none, nullptr, a.exprBody.get(), nullptr);
                if (!a.stmts.empty())
                    runMethodBody(none, &a.stmts, nullptr, nullptr);
            }
        } else if (auto* cd = dynamic_cast<const ast::ConstDecl*>(&m)) {
            if (cd->value) visitExpr(*cd->value);
        } else if (auto* nt = dynamic_cast<const ast::NestedTypeMember*>(&m)) {
            if (nt->decl) {
                TypeSymbol* nested = nullptr;
                if (currentType_) {
                    for (Symbol* mem : currentType_->members) {
                        if (auto* nts =
                                dynamic_cast<NestedTypeSymbol*>(mem)) {
                            if (nts->type &&
                                nts->type->decl == nt->decl.get()) {
                                nested = nts->type; break;
                            }
                        }
                    }
                }
                TypeSymbol* save = currentType_;
                currentType_ = nested;
                for (const auto& innerM : nt->decl->members) {
                    if (innerM) visitMember(*innerM);
                }
                currentType_ = save;
            }
        }
    }

    void visitStmt(const ast::Stmt& s) {
        if (auto* ld = dynamic_cast<const ast::LocalDecl*>(&s)) {
            // Initializer evaluates BEFORE the local comes into scope.
            if (ld->initializer) visitExpr(*ld->initializer);
            if (currentScope_ && !ld->name.empty()) {
                auto* lsym = ctx_.makeSymbol<LocalSymbol>();
                lsym->name = ld->name;
                lsym->decl = ld;
                lsym->isUsing = ld->isUsing;
                lsym->isAwaitUsing = ld->isAwaitUsing;
                currentScope_->names[ld->name] = lsym;
            }
        } else if (auto* lc =
                       dynamic_cast<const ast::LocalConstDecl*>(&s)) {
            if (lc->value) visitExpr(*lc->value);
            if (currentScope_ && !lc->name.empty()) {
                auto* lsym = ctx_.makeSymbol<LocalSymbol>();
                lsym->name = lc->name;
                lsym->decl = lc;
                lsym->isConst = true;
                currentScope_->names[lc->name] = lsym;
            }
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
        // Slice 3b: IdentifierExpr name lookup. `this` / `base` are
        // implicit-meaning identifiers — leave them unresolved
        // (typecheck handles their semantics in a future slice).
        if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(&e)) {
            if (id->name == "this" || id->name == "base") return;
            Symbol* sym = lookupName(id->name);
            if (sym) {
                ctx_.setExprSymbol(&e, sym);
            } else if (currentScope_) {
                ctx_.diagnostics().report(
                    Severity::Error, id->span.begin,
                    std::string("unresolved identifier `") +
                    std::string(id->name) + "`");
            }
            return;
        }
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
        // own literals get typed. The expression itself may also get a
        // type from the operator's result rules (slice 3c).
        visitChildren(e);
        // Slice 3c: binary / unary operator result typing.
        if (auto* be = dynamic_cast<const ast::BinaryExpr*>(&e)) {
            typeBinary(*be);
        }
        if (auto* ue = dynamic_cast<const ast::UnaryExpr*>(&e)) {
            typeUnary(*ue);
        }
    }

    // §17.1 result-type table for binary operators. Slice 3c covers the
    // common cases with no operator-overload resolution: equality and
    // relational ops produce bool; arithmetic / shift / bitwise produce
    // the left operand's type (when the operands type-match); logical
    // ops `and` / `or` produce bool; `??` produces the non-nullable
    // result type of the LHS; range ops produce a Range value (slice 3c
    // leaves Range as Error).
    void typeBinary(const ast::BinaryExpr& be) {
        const auto& op = be.op;
        if (op == "=" || op == "<>" || op == "<" || op == ">" ||
            op == "<=" || op == ">=" || op == "and" || op == "or") {
            ctx_.setExprType(&be, ctx_.primitiveTypeRef(PrimitiveType::Bool));
            return;
        }
        // `in` is a membership test → bool.
        if (op == "in") {
            ctx_.setExprType(&be, ctx_.primitiveTypeRef(PrimitiveType::Bool));
            return;
        }
        // Arithmetic / shift / bitwise: result type follows the left
        // operand's type when both sides are present and known.
        if (op == "+" || op == "-" || op == "*" || op == "/" ||
            op == "mod" || op == "&" || op == "|" || op == "^" ||
            op == "<<" || op == ">>") {
            TypeRef* lt = be.left ? ctx_.exprType(*be.left) : nullptr;
            if (lt) ctx_.setExprType(&be, lt);
            return;
        }
        // `??` null-coalescing: result type is the non-nullable form of
        // the LHS (sema-3c approximation: take RHS type if known, else
        // LHS).
        if (op == "??") {
            TypeRef* rt = be.right ? ctx_.exprType(*be.right) : nullptr;
            if (rt) ctx_.setExprType(&be, rt);
            return;
        }
        // Range ops, etc.: leave un-typed (later slices handle).
    }

    // §17.1 result-type table for unary operators (slice 3c).
    void typeUnary(const ast::UnaryExpr& ue) {
        const auto& op = ue.op;
        if (op == "not") {
            ctx_.setExprType(&ue, ctx_.primitiveTypeRef(PrimitiveType::Bool));
            return;
        }
        if (op == "+" || op == "-" || op == "~") {
            TypeRef* ot = ue.operand ? ctx_.exprType(*ue.operand) : nullptr;
            if (ot) ctx_.setExprType(&ue, ot);
            return;
        }
        // `await`, `move` — result-type rules deeper (Task<T> -> T,
        // move keeps the operand's type). Slice 3c doesn't type them.
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
