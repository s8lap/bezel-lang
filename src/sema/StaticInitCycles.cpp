// §2.5 / §22.13 (#797): compile-time detection of circular static-initialization
// dependencies. See StaticInitCycles.hpp for the contract.

#include "sema/StaticInitCycles.hpp"

#include <algorithm>
#include <functional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ast/Node.hpp"
#include "sema/SemaContext.hpp"
#include "sema/Symbol.hpp"
#include "support/Diagnostic.hpp"

namespace bezel::sema {

namespace {

bool hasStaticModifier(const MemberSymbol* m) {
    return std::find(m->modifiers.begin(), m->modifiers.end(),
                     std::string_view("static")) != m->modifiers.end();
}

// Walks a type's static field initializers / static-constructor body and
// collects the OTHER local types whose static members it directly references —
// each such reference is a static-initialization dependency edge.
struct DepCollector {
    const SemaContext& ctx;
    const TypeSymbol* owner;            // the type whose static-init we are walking
    // edge target -> a representative source location of a reference creating it
    std::unordered_map<const TypeSymbol*, SourceLocation>& out;
    // #804: interprocedural recursion guard — each static method / property
    // getter whose body we have already folded into `owner`'s edge set. Keyed on
    // the callee symbol so a self- or mutually-recursive static call graph
    // terminates. Scoped to one collectDeps walk (one owner).
    std::unordered_set<const Symbol*> visited;
    // #804: while walking the TOP node of an assignment / increment TARGET, a
    // static property runs its SETTER, not its getter, so suppress getter-body
    // following there (recording the edge is still correct). Erring toward a
    // missed transitive edge (a safe false negative) over a false one.
    bool suppressGetterFollow = false;

    // A reference to a static FIELD / METHOD / PROPERTY of another local type
    // triggers that type's static initialization, so it is a dependency edge.
    // Const members (compile-time values) and enum constants carry no runtime
    // init, so they are not edges. Self-references (own statics) are textual-order
    // concerns (definite assignment), not cycles. Imported targets are deferred
    // (their static-init dependencies are not visible cross-module).
    void recordRef(const ast::Expr* e) {
        if (!e) return;
        Symbol* s = ctx.exprSymbol(*e);
        if (!s) return;
        auto* mem = dynamic_cast<MemberSymbol*>(s);
        if (!mem || !mem->parent) return;
        if (!hasStaticModifier(mem)) return;
        bool relevant = dynamic_cast<FieldSymbol*>(s) ||
                        dynamic_cast<MethodSymbol*>(s) ||
                        dynamic_cast<PropertySymbol*>(s);
        if (!relevant) return;
        const TypeSymbol* u = mem->parent;
        if (u != owner && !ctx.isImported(u))
            out.emplace(u, e->span.begin); // first reference's location wins
        // #804: a READ of a static property runs its getter body — fold the
        // statics it references into `owner`'s edge set (transitive dependency).
        // A method reference is NOT followed here (referencing a method value
        // does not run its body); a called method is followed at the CallExpr.
        if (!suppressGetterFollow)
            if (auto* prop = dynamic_cast<PropertySymbol*>(s))
                followProperty(prop);
    }

    // #804: fold a called static method's body into `owner`'s dependency set —
    // statics it references run during `owner`'s static init (interprocedural
    // reachability). An imported method has no local AST decl → naturally
    // skipped (cross-module cycles are the deferred dimension). Recursion-guarded.
    void followMethod(const MethodSymbol* m) {
        if (!m || !visited.insert(m).second) return;
        auto* md = dynamic_cast<const ast::MethodDecl*>(m->decl);
        if (!md) return;
        if (md->bodyKind == ast::MethodBodyKind::Expression)
            expr(md->exprBody.get());
        else
            for (auto& st : md->stmts) stmt(st.get());
    }

    // #804: fold a read static property's GETTER body into `owner`'s set.
    void followProperty(const PropertySymbol* p) {
        if (!p || !visited.insert(p).second) return;
        auto* pd = dynamic_cast<const ast::PropertyDecl*>(p->decl);
        if (!pd) return;
        if (pd->bodyKind == ast::PropertyBodyKind::Expression) {
            expr(pd->exprBody.get());
        } else if (pd->bodyKind == ast::PropertyBodyKind::Block) {
            for (auto& acc : pd->accessors) {
                if (acc.isSet) continue;   // getter only
                if (acc.bodyKind == ast::PropertyAccessorBodyKind::Expression)
                    expr(acc.exprBody.get());
                else
                    for (auto& st : acc.stmts) stmt(st.get());
            }
        }
        // Auto-property getter is a plain field read — no body to fold.
    }

    // #804: if a call's callee resolves to a static method, fold its body.
    void followCalledStaticMethod(const ast::Expr* callee) {
        if (!callee) return;
        Symbol* s = ctx.exprSymbol(*callee);
        if (auto* meth = dynamic_cast<MethodSymbol*>(s))
            if (hasStaticModifier(meth)) followMethod(meth);
    }

    // #804: a write target's top node — record the property edge but do NOT run
    // its getter; still walk the receiver as reads (whose getters DO run).
    void writeTarget(const ast::Expr* e) {
        if (auto* m = dynamic_cast<const ast::MemberAccessExpr*>(e)) {
            if (dynamic_cast<PropertySymbol*>(ctx.exprSymbol(*e))) {
                bool sav = suppressGetterFollow;
                suppressGetterFollow = true;
                recordRef(e);              // edge only (getter-follow suppressed)
                suppressGetterFollow = sav;
                expr(m->target.get());     // receiver sub-reads follow normally
                return;
            }
        }
        expr(e);
    }

    void expr(const ast::Expr* e) {
        if (!e) return;
        // Record a reference at THIS node (an identifier or member access that
        // resolves to another type's static member) before recursing.
        recordRef(e);
        if (auto* m = dynamic_cast<const ast::MemberAccessExpr*>(e)) {
            expr(m->target.get()); return; }
        if (auto* m = dynamic_cast<const ast::NullCondMemberExpr*>(e)) {
            expr(m->target.get()); return; }
        if (auto* c = dynamic_cast<const ast::CallExpr*>(e)) {
            expr(c->callee.get());
            for (auto& a : c->args) expr(a.value.get());
            followCalledStaticMethod(c->callee.get());
            return; }
        if (auto* c = dynamic_cast<const ast::NullCondCallExpr*>(e)) {
            expr(c->callee.get());
            for (auto& a : c->args) expr(a.value.get());
            followCalledStaticMethod(c->callee.get());
            return; }
        if (auto* ix = dynamic_cast<const ast::IndexExpr*>(e)) {
            expr(ix->target.get());
            for (auto& a : ix->indices) expr(a.value.get());
            return; }
        if (auto* b = dynamic_cast<const ast::BinaryExpr*>(e)) {
            expr(b->left.get()); expr(b->right.get()); return; }
        if (auto* u = dynamic_cast<const ast::UnaryExpr*>(e)) {
            expr(u->operand.get()); return; }
        if (auto* p = dynamic_cast<const ast::PostfixBangExpr*>(e)) {
            expr(p->operand.get()); return; }
        if (auto* t = dynamic_cast<const ast::TypeTestExpr*>(e)) {
            expr(t->operand.get()); return; }
        if (auto* c = dynamic_cast<const ast::ConditionalExpr*>(e)) {
            expr(c->condition.get()); expr(c->thenValue.get());
            expr(c->elseValue.get()); return; }
        if (auto* t = dynamic_cast<const ast::ThrowExpr*>(e)) {
            expr(t->operand.get()); return; }
        if (auto* r = dynamic_cast<const ast::RangeExpr*>(e)) {
            expr(r->lo.get()); expr(r->hi.get()); expr(r->step.get());
            return; }
        if (auto* n = dynamic_cast<const ast::NewExpr*>(e)) {
            for (auto& a : n->args) expr(a.value.get());
            return; }
        if (auto* w = dynamic_cast<const ast::WithExpr*>(e)) {
            expr(w->source.get());
            for (auto& a : w->assigns) expr(a.value.get());
            return; }
        if (auto* cl = dynamic_cast<const ast::CollectionLiteralExpr*>(e)) {
            for (auto& el : cl->elements) expr(el.get());
            for (auto& k : cl->keys) expr(k.get());
            return; }
        if (auto* is = dynamic_cast<const ast::InterpolatedStringExpr*>(e)) {
            for (auto& part : is->parts) expr(part.expr.get());
            return; }
        if (auto* ce = dynamic_cast<const ast::CaseExpr*>(e)) {
            expr(ce->scrutinee.get());
            for (auto& arm : ce->arms) {
                expr(arm.guard.get());
                expr(arm.exprBody.get());
                for (auto& s : arm.stmts) stmt(s.get());
            }
            return; }
        // Lambda bodies are NOT walked: a lambda defers execution, so a static
        // referenced inside one does not run during this type's static init.
        // Literals and other leaves: nothing to do.
    }

    void stmt(const ast::Stmt* s) {
        if (!s) return;
        if (auto* ld = dynamic_cast<const ast::LocalDecl*>(s)) {
            expr(ld->initializer.get()); return; }
        if (auto* lc = dynamic_cast<const ast::LocalConstDecl*>(s)) {
            expr(lc->value.get()); return; }
        if (auto* r = dynamic_cast<const ast::ReturnStmt*>(s)) {
            expr(r->value.get()); return; }
        if (auto* es = dynamic_cast<const ast::ExpressionStmt*>(s)) {
            expr(es->expr.get()); return; }
        if (auto* ts = dynamic_cast<const ast::ThrowStmt*>(s)) {
            expr(ts->value.get()); return; }
        if (auto* as = dynamic_cast<const ast::AssignmentStmt*>(s)) {
            writeTarget(as->target.get()); expr(as->value.get()); return; }
        if (auto* inc = dynamic_cast<const ast::IncrementStmt*>(s)) {
            writeTarget(inc->target.get()); return; }
        if (auto* ds = dynamic_cast<const ast::DiscardStmt*>(s)) {
            expr(ds->value.get()); return; }
        if (auto* ys = dynamic_cast<const ast::YieldStmt*>(s)) {
            expr(ys->value.get()); return; }
        if (auto* is = dynamic_cast<const ast::IfStmt*>(s)) {
            expr(is->ifBranch.condition.get());
            for (auto& b : is->ifBranch.body) stmt(b.get());
            for (auto& br : is->elifBranches) {
                expr(br.condition.get());
                for (auto& b : br.body) stmt(b.get());
            }
            for (auto& b : is->elseBody) stmt(b.get());
            return; }
        if (auto* ws = dynamic_cast<const ast::WhileStmt*>(s)) {
            expr(ws->condition.get());
            for (auto& b : ws->body) stmt(b.get());
            return; }
        if (auto* fs = dynamic_cast<const ast::ForStmt*>(s)) {
            expr(fs->iterable.get()); expr(fs->withExpr.get());
            for (auto& b : fs->body) stmt(b.get());
            return; }
        if (auto* d = dynamic_cast<const ast::DoEndBlock*>(s)) {
            for (auto& b : d->body) stmt(b.get());
            return; }
        if (auto* ts = dynamic_cast<const ast::TryStmt*>(s)) {
            for (auto& b : ts->body) stmt(b.get());
            for (auto& c : ts->catches) {
                expr(c.guard.get());
                stmt(c.body.get());
            }
            for (auto& b : ts->finallyBody) stmt(b.get());
            return; }
        if (auto* cs = dynamic_cast<const ast::CaseStmt*>(s)) {
            expr(cs->scrutinee.get());
            for (auto& arm : cs->arms) {
                expr(arm.guard.get());
                expr(arm.exprBody.get());
                for (auto& a : arm.stmts) stmt(a.get());
            }
            return; }
        // BreakStmt / ContinueStmt: no sub-expressions.
    }
};

// Compute the static-init dependency set for one type (its static field
// initializers + static-constructor body).
void collectDeps(const SemaContext& ctx, const TypeSymbol* t,
                 std::unordered_map<const TypeSymbol*, SourceLocation>& out) {
    DepCollector c{ctx, t, out, /*visited=*/{}, /*suppressGetterFollow=*/false};
    for (const Symbol* m : t->members) {
        if (auto* fs = dynamic_cast<const FieldSymbol*>(m)) {
            if (!hasStaticModifier(fs)) continue;
            if (auto* fd = dynamic_cast<const ast::FieldDecl*>(fs->decl))
                c.expr(fd->initializer.get());
        } else if (auto* cs = dynamic_cast<const ConstructorSymbol*>(m)) {
            if (!cs->isStatic) continue;
            if (auto* cd = dynamic_cast<const ast::ConstructorDecl*>(cs->decl))
                for (auto& st : cd->body) c.stmt(st.get());
        }
    }
}

bool typeHasStaticInit(const TypeSymbol* t) {
    for (const Symbol* m : t->members) {
        if (auto* fs = dynamic_cast<const FieldSymbol*>(m)) {
            if (hasStaticModifier(fs)) {
                auto* fd = dynamic_cast<const ast::FieldDecl*>(fs->decl);
                if (fd && fd->initializer) return true;
            }
        } else if (auto* cs = dynamic_cast<const ConstructorSymbol*>(m)) {
            if (cs->isStatic) return true;
        }
    }
    return false;
}

}  // namespace

bool checkStaticInitCycles(const ast::SourceFile& /*src*/, SemaContext& ctx) {
    // Build the direct static-init dependency graph over LOCAL types. Each edge
    // carries a representative source location (a reference that creates it).
    using EdgeMap = std::unordered_map<const TypeSymbol*, SourceLocation>;
    std::vector<const TypeSymbol*> sources;
    std::unordered_map<const TypeSymbol*, EdgeMap> deps;
    for (const auto& sym : ctx.symbols()) {
        auto* t = dynamic_cast<const TypeSymbol*>(sym.get());
        if (!t || ctx.isImported(t)) continue;
        if (!typeHasStaticInit(t)) continue;
        EdgeMap d;
        collectDeps(ctx, t, d);
        if (!d.empty()) {
            deps[t] = std::move(d);
            sources.push_back(t);
        }
    }
    if (deps.empty()) return true;

    // DFS for a back-edge to a node on the current path = a cycle. Report each
    // distinct cycle once (keyed on its member set).
    std::unordered_map<const TypeSymbol*, int> color;  // 0 white, 1 gray, 2 black
    std::vector<const TypeSymbol*> path;
    std::set<std::set<const TypeSymbol*>> reported;
    bool clean = true;

    std::function<void(const TypeSymbol*)> dfs = [&](const TypeSymbol* t) {
        color[t] = 1;
        path.push_back(t);
        auto it = deps.find(t);
        if (it != deps.end()) {
            for (const auto& [u, edgeLoc] : it->second) {
                if (color[u] == 1) {
                    // Cycle: u .. t (current) and back to u.
                    auto pit = std::find(path.begin(), path.end(), u);
                    std::vector<const TypeSymbol*> cyc(pit, path.end());
                    std::set<const TypeSymbol*> key(cyc.begin(), cyc.end());
                    if (reported.insert(key).second) {
                        clean = false;
                        std::string msg =
                            "circular static-initialization dependency: ";
                        for (const TypeSymbol* c : cyc)
                            msg += std::string(c->name) + " -> ";
                        msg += std::string(cyc.front()->name);
                        msg += " (§2.5: circular static-initialization "
                               "dependencies are not permitted)";
                        // Report at the reference that closes the cycle (the
                        // back-edge t -> u); expression spans are populated even
                        // though TypeSymbol declLoc is not.
                        ctx.diagnostics().report(
                            Severity::Error, edgeLoc, msg);
                    }
                } else if (color[u] == 0) {
                    dfs(u);
                }
            }
        }
        color[t] = 2;
        path.pop_back();
    };

    for (const TypeSymbol* t : sources)
        if (color[t] == 0) dfs(t);

    return clean;
}

}  // namespace bezel::sema
