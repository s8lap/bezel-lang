// §2.5 / §22.13 (#797): compile-time detection of circular static-initialization
// dependencies. See StaticInitCycles.hpp for the contract.
//
// #1553 / §22.13 rule 1 (v0.181): this pass IS the normative compile-time
// approximation — an edge T→U exists when T's static field initializers or
// static-constructor body reference a static member of U, directly or
// transitively through *statically-resolvable calls* (static methods and static
// property getters reached by direct name within the compilation unit, followed
// by followMethod/followProperty below). Calls through function values (lambda
// bodies are deliberately NOT walked — see expr()), virtual dispatch
// (followCalledStaticMethod resolves only static callees), and code outside the
// compilation unit (imported targets skipped) contribute NO edges. It rejects
// every cycle it can see and is not a completeness guarantee; a cycle routed
// through those uncovered edges compiles and is caught at runtime by the
// same-thread re-entrancy backstop (§22.13 rule 2, LlvmBackend_Setup.cpp).

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
    // fix #4531 / §22.13: when non-null, this walk is also collecting
    // SAME-TYPE static references for the forward-reference rule — see
    // checkSameTypeForwardRefs below. Null for the ordinary cross-type edge
    // walk, which records nothing for `u == owner`.
    std::vector<std::pair<const MemberSymbol*, SourceLocation>>* sameTypeOut =
        nullptr;
    // fix #4531: 0 while walking the initializer expression itself, >0 once
    // inside a followed method / getter body. Only the >0 references are the
    // transitive half of §22.13's forward-reference rule; the depth-0 ones are
    // the direct half, already reported by ExprTypecheck.cpp.
    int followDepth = 0;
    struct DepthGuard {
        DepCollector& c;
        explicit DepthGuard(DepCollector& c_) : c(c_) { ++c.followDepth; }
        ~DepthGuard() { --c.followDepth; }
    };

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
        // fix #4531 / §22.13: a reference to one of `owner`'s OWN statics made
        // from inside a followed body is the transitive half of the same-type
        // forward-reference rule. It is not an edge (edges require T != U,
        // rule 1) — it is a textual-order question, answered by
        // checkSameTypeForwardRefs against the declaration order.
        if (sameTypeOut && u == owner && followDepth > 0)
            sameTypeOut->emplace_back(mem, e->span.begin);
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
        DepthGuard dg{*this};                              // fix #4531
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
        DepthGuard dg{*this};                              // fix #4531
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

// --- §22.13 same-type forward references reached THROUGH a call body --------
//
// fix #4531, per §22.13 (v0.306): "if the initializer expression for a
// `static readonly` field F reads a `static readonly` field G of the same type
// declared *after* F in the source — **directly, or transitively through the
// bodies of statically-resolvable calls** (the same call set as the cross-type
// dependency graph below — static methods and static property getters reached
// by direct name within the compilation unit, §2.5; function values, virtual
// dispatch, and code outside the compilation unit contribute nothing) — the
// reference is rejected at compile time."
//
// ExprTypecheck.cpp's `checkStaticReadonlyForwardRefImpl` implements only the
// DIRECT half: it walks the initializer expression and stops at a call. The
// transitive half belongs HERE, for two reasons:
//
//   * it needs exactly this file's call set — the `followCalledStaticMethod` /
//     `followMethod` / `followProperty` walk that §22.13 defines by pointing at
//     "the same call set as the cross-type dependency graph"; and
//   * it must run after typecheck. The helper is routinely declared *after*
//     the initializer that calls it (t7's `Helper` is the last member of its
//     type), so during ExprTypecheck's own member-by-member pass the helper
//     body's `exprSymbol` entries do not exist yet.
//
// Until this check existed the shape fell through to the RUNTIME, where fix
// #4486 correctly stopped it raising: §22.13 rule 2's backstop is scoped to
// cross-type re-entrancy ("a touch of T is *re-entrant* when that innermost
// initialization belongs to a different type — the T → U → T signature — and
// is *ordinary execution* when it is T's own"), and the paragraph says so
// again in its own parenthesis ("A type's *own* initialization code observing
// its own later statics is governed by the forward-reference rule and its
// recorded residual above, not by this backstop"). The residual it is governed
// by is scoped just as tightly — it covers a read "routed through a function
// value, virtual dispatch, or a module boundary", which a direct static call
// within the compilation unit is not. So there is no runtime rule that admits
// this program: the rule that governs it is the compile-time one, and the
// defect was that the compile-time check did not implement its own transitive
// clause.
//
// This predicate mirrors ExprTypecheck.cpp's `ExprTC::staticReadonlyStorageName`
// — the two must agree about which members §22.13's textual-order rule ranges
// over: a `static readonly` field, or a `static readonly` AUTO-property whose
// §23.1 backing field the same textual-order pass writes (#3774). Never an
// indexer, and never a getter-bearing property, which recomputes on every read
// and owns no static-init-written storage.
bool isStaticReadonlyStorage(const Symbol* s) {
    auto hasMod = [](const std::vector<std::string_view>& mods,
                     std::string_view m) {
        return std::find(mods.begin(), mods.end(), m) != mods.end();
    };
    if (auto* fs = dynamic_cast<const FieldSymbol*>(s))
        return hasMod(fs->modifiers, "static") &&
               hasMod(fs->modifiers, "readonly");
    if (auto* ps = dynamic_cast<const PropertySymbol*>(s)) {
        if (ps->isIndexer) return false;
        // The declaration is the ONLY channel for "is this a static
        // AUTO-property": `PropertySymbol::isAutoProperty` is an
        // INSTANCE-storage predicate that SymbolCollection and
        // ModuleMetadataBuilder both clear on `static`, so it is false for
        // every static auto-property. ExprTypecheck.cpp's
        // `staticReadonlyStorageName` reads the decl here for the same reason,
        // under the same marker.
        //
        // SEAM-ACCESSOR: local-only by construction — the sole caller walks
        // `t->members` of a type it already tested `!ctx.isImported(t)`. #3152
        auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl);
        if (!pd || pd->isIndexer || pd->bodyKind != ast::PropertyBodyKind::Auto)
            return false;
        return hasMod(ps->modifiers, "static") &&
               hasMod(ps->modifiers, "readonly");
    }
    return false;
}

// The one initializer expression a static-readonly storage member has: a
// field's `:= expr`, or (§23.1) an auto-property's. A getter body is not an
// initializer — it runs on demand, after static init.
const ast::Expr* staticReadonlyInitializerOf(const Symbol* s) {
    // An initializer EXPRESSION exists only in the AST — there is no
    // symbol-side channel for it, and there cannot be one across a module
    // boundary (an imported member's initializer already ran in its own
    // compilation, and is not a SAME-TYPE reference here in any case).
    if (auto* fs = dynamic_cast<const FieldSymbol*>(s)) {
        // SEAM-ACCESSOR: local-only by the same construction as above. #3152
        auto* fd = dynamic_cast<const ast::FieldDecl*>(fs->decl);
        return fd ? fd->initializer.get() : nullptr;
    }
    if (auto* ps = dynamic_cast<const PropertySymbol*>(s)) {
        // SEAM-ACCESSOR: ditto, for §23.1's auto-property initializer. #3152
        auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl);
        return pd ? pd->autoInitializer.get() : nullptr;
    }
    return nullptr;
}

// Reports every same-type forward reference in `t` that is reachable only
// through a followed body. Returns false if it reported anything.
bool checkSameTypeForwardRefs(SemaContext& ctx, const TypeSymbol* t,
                              std::set<std::pair<std::uint32_t, std::string>>&
                                  alreadyReported) {
    // `t->members` is declaration order — the same list ExprTypecheck.cpp's
    // `indexOf` walks to answer "declared *after* F in the source".
    std::unordered_map<const Symbol*, int> index;
    for (std::size_t i = 0; i < t->members.size(); ++i)
        index.emplace(t->members[i], static_cast<int>(i));

    bool clean = true;
    for (std::size_t i = 0; i < t->members.size(); ++i) {
        const Symbol* f = t->members[i];
        if (!isStaticReadonlyStorage(f)) continue;
        const ast::Expr* init = staticReadonlyInitializerOf(f);
        if (!init) continue;
        // A fresh collector per F: `visited` is the interprocedural recursion
        // guard for ONE root walk, so sharing it across roots would silently
        // drop the second root's view of a helper the first already folded.
        std::unordered_map<const TypeSymbol*, SourceLocation> edgesUnused;
        std::vector<std::pair<const MemberSymbol*, SourceLocation>> refs;
        DepCollector c{ctx, t, edgesUnused};
        c.sameTypeOut = &refs;
        c.expr(init);

        std::unordered_set<const Symbol*> reportedForF;
        for (const auto& [g, loc] : refs) {
            auto it = index.find(g);
            if (it == index.end() || it->second <= static_cast<int>(i)) continue;
            if (!isStaticReadonlyStorage(g)) continue;
            if (!reportedForF.insert(g).second) continue;
            // Same wording as ExprTypecheck.cpp's direct arm: one rule, one
            // voice. Reported at the read inside the followed body, which is
            // where the forward reference actually happens.
            std::string msg =
                std::string("static readonly `") +
                std::string(dynamic_cast<const MemberSymbol*>(f)->name) +
                "` reads static readonly `" + std::string(g->name) +
                "`, which is declared later in the same type — a forward "
                "reference between static readonly fields is not allowed; "
                "reorder the declarations or use a static constructor (§22.13)";
            // One diagnostic per (location, message) across the whole unit: a
            // generic type's template and any instantiation sharing that AST
            // would otherwise report the identical read twice.
            if (!alreadyReported.emplace(loc.offset, msg).second) continue;
            clean = false;
            ctx.diagnostics().report(Severity::Error, loc, msg);
        }
    }
    return clean;
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
    // fix #4531 / §22.13: the transitive half of the SAME-TYPE
    // forward-reference rule — a `static readonly` initializer reading a
    // later-declared `static readonly` sibling through the body of a
    // statically-resolvable call. Runs over every local type, including one
    // whose only static init is an auto-property initializer (which
    // `typeHasStaticInit` below does not count, because such a member creates
    // no cross-type EDGE bookkeeping of its own).
    bool sameTypeClean = true;
    {
        std::set<std::pair<std::uint32_t, std::string>> reported;
        for (const auto& sym : ctx.symbols()) {
            auto* t = dynamic_cast<const TypeSymbol*>(sym.get());
            if (!t || ctx.isImported(t)) continue;
            if (!checkSameTypeForwardRefs(ctx, t, reported))
                sameTypeClean = false;
        }
    }

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
    if (deps.empty()) return sameTypeClean;

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

    return clean && sameTypeClean;
}

// fix #4865 / §17.14 / §22.12 (v0.307): a class-level `const` initializer must
// be a compile-time constant whose value is well-defined; a self/mutually-
// referential initializer has no value. Sema §17.14 folds terminate on their
// depth bound, but codegen's `constMemberValue` re-LOWERS the value through
// `lowerExpr(*cd->value)`, which for a bare-identifier or qualified const
// reference re-resolves to its own AST const member and re-lowers that —
// unbounded recursion that overflows the process stack and SIGSEGVs the
// compiler. The same shape over two classes (`A.K := B.K; B.K := A.K`) and
// over integer typed pairs (`I1 := I2 + 1; I2 := I1 + 1`) all share the bug.
//
// Build a DIRECTED graph over LOCAL class-level ConstMemberSymbols: an edge
// K1 -> K2 iff K1's initializer (transitively, through expression sub-trees)
// references K2 by ConstMemberSymbol. Imported consts fold via the textual
// `importedConstValue` path and don't enter the graph. DFS rejects each
// distinct cycle once. Reference location is the SOURCE location of the
// const reference expression (a const reference is the only thing the §17.14
// fold cannot terminate on — it terminates on its own depth bound, but the
// identifier node's source location identifies the back-edge in the graph).
namespace {
struct ConstRefCollector {
    const SemaContext& ctx;
    const ConstMemberSymbol* owner;   // the const whose initializer we walk
    // Edges from `owner` to the LOCAL ConstMemberSymbols its initializer
    // references. Each edge carries the source location of the back-reference
    // expression so the diagnostic can name the offending site.
    std::unordered_map<const ConstMemberSymbol*, SourceLocation>& out;
    // Visited guard against recursion through nested expression sub-trees.
    // Const initializers are §17.14 expressions — bounded depth in practice —
    // but a cycle through an IdentifierExpr that names a different const
    // re-enters `collectConstRefs`, not `expr`, so this set guards that.
    std::unordered_set<const ConstMemberSymbol*> visited;

    void expr(const ast::Expr* e) {
        if (!e) return;
        // Record a reference at THIS node (an IdentifierExpr or
        // MemberAccessExpr that resolves to a class-level const).
        recordRef(e);
        if (auto* m = dynamic_cast<const ast::MemberAccessExpr*>(e)) {
            expr(m->target.get()); return;
        }
        if (auto* m = dynamic_cast<const ast::NullCondMemberExpr*>(e)) {
            expr(m->target.get()); return;
        }
        if (auto* c = dynamic_cast<const ast::CallExpr*>(e)) {
            expr(c->callee.get());
            for (auto& a : c->args) expr(a.value.get());
            return;
        }
        if (auto* c = dynamic_cast<const ast::NullCondCallExpr*>(e)) {
            expr(c->callee.get());
            for (auto& a : c->args) expr(a.value.get());
            return;
        }
        if (auto* ix = dynamic_cast<const ast::IndexExpr*>(e)) {
            expr(ix->target.get());
            for (auto& a : ix->indices) expr(a.value.get());
            return;
        }
        if (auto* b = dynamic_cast<const ast::BinaryExpr*>(e)) {
            expr(b->left.get()); expr(b->right.get()); return;
        }
        if (auto* u = dynamic_cast<const ast::UnaryExpr*>(e)) {
            expr(u->operand.get()); return;
        }
        if (auto* p = dynamic_cast<const ast::PostfixBangExpr*>(e)) {
            expr(p->operand.get()); return;
        }
        if (auto* t = dynamic_cast<const ast::TypeTestExpr*>(e)) {
            expr(t->operand.get()); return;
        }
        if (auto* n = dynamic_cast<const ast::NewExpr*>(e)) {
            for (auto& a : n->args) expr(a.value.get());
            return;
        }
        if (auto* tr = dynamic_cast<const ast::ThrowExpr*>(e)) {
            expr(tr->operand.get()); return;
        }
        if (auto* q = dynamic_cast<const ast::ConditionalExpr*>(e)) {
            expr(q->condition.get()); expr(q->thenValue.get());
            expr(q->elseValue.get()); return;
        }
        // §17.1 mandatory parenthesization (#1086): source-level parens leave
        // no ParenExpr node — the tree shape encodes grouping. The walk
        // already descends into the inner expression via the
        // `parenthesized` flag on Expr; nothing else to do here.
    }

    void recordRef(const ast::Expr* e) {
        if (!e) return;
        Symbol* s = ctx.exprSymbol(*e);
        if (!s) return;
        auto* cm = dynamic_cast<ConstMemberSymbol*>(s);
        if (!cm || !cm->parent) return;
        // Imported consts are folded via textual value, not the AST path —
        // they cannot appear as a re-entry target of `constMemberValue` for
        // this source unit, so an edge to one is a false positive.
        if (ctx.isImported(cm)) return;
        // Record this DIRECT reference (the const named at THIS expression
        // node) — the graph is built over DIRECT initializer references, not
        // transitive ones. The DFS walks the graph to find cycles, so a
        // 3-node chain (`K1 := K2 + 1; K2 := K3 + 1; K3 := K1 + 1`) is one
        // 3-edge cycle, not three independent 2-edge ones. Each reference
        // is recorded at most once per source const; the `visited` gate
        // keeps multi-reference initializers from recording the same edge
        // twice (e.g. `K1 + K1`).
        if (!visited.insert(cm).second) return;
        out.emplace(cm, e->span.begin);
    }
};

// Collect every LOCAL class-level const symbol with an AST ConstDecl. Keyed on
// the symbol so the DFS walks the actual symbol graph, not string lookups.
std::vector<const ConstMemberSymbol*> collectLocalConstMembers(
        const SemaContext& ctx) {
    std::vector<const ConstMemberSymbol*> out;
    for (const auto& sym : ctx.symbols()) {
        auto* cm = dynamic_cast<const ConstMemberSymbol*>(sym.get());
        if (!cm || !cm->parent) continue;
        if (ctx.isImported(cm)) continue;
        // SEAM-DEBT: #4911 imported consts skipped on the line above
        auto* cd = dynamic_cast<const ast::ConstDecl*>(cm->decl);
        if (!cd) continue;
        out.push_back(cm);
    }
    return out;
}

void collectConstRefs(
        const SemaContext& ctx,
        const ConstMemberSymbol* cm,
        std::unordered_map<const ConstMemberSymbol*, SourceLocation>& edges) {
    ConstRefCollector c{ctx, cm, edges, {}};
    // SEAM-DEBT: #4911 local-only const (collectLocalConstMembers skips imported)
    auto* cd = dynamic_cast<const ast::ConstDecl*>(cm->decl);
    if (cd && cd->value) c.expr(cd->value.get());
}
}  // namespace

bool checkConstInitCycles(const ast::SourceFile& /*src*/, SemaContext& ctx) {
    auto locals = collectLocalConstMembers(ctx);
    if (locals.empty()) return true;

    using EdgeMap = std::unordered_map<const ConstMemberSymbol*, SourceLocation>;
    std::unordered_map<const ConstMemberSymbol*, EdgeMap> deps;
    for (const ConstMemberSymbol* cm : locals) {
        EdgeMap e;
        collectConstRefs(ctx, cm, e);
        if (!e.empty()) deps[cm] = std::move(e);
    }
    if (deps.empty()) return true;

    std::unordered_map<const ConstMemberSymbol*, int> color;  // 0 white, 1 gray, 2 black
    std::vector<const ConstMemberSymbol*> path;
    // Cycle reports dedupe by MEMBER SET so the SAME cycle is reported once
    // even when both endpoints start their own DFS, and also record the sets
    // for a SUPERSET check: a length-2 cycle is the real defect, and the
    // length-1 self-edges its traversal creates are subsets of it. We report
    // a cycle only when no already-reported cycle's member set is a proper
    // superset of it — so the user sees the smallest cycle that closes the
    // graph, not every back-edge individually.
    std::vector<std::set<const ConstMemberSymbol*>> reportedCycles;
    bool clean = true;

    auto reportCycle = [&](const std::vector<const ConstMemberSymbol*>& cyc,
                           SourceLocation edgeLoc) {
        std::set<const ConstMemberSymbol*> key(cyc.begin(), cyc.end());
        for (const auto& prev : reportedCycles) {
            if (prev.size() > key.size() && std::includes(
                    prev.begin(), prev.end(), key.begin(), key.end()))
                return;   // `prev` is a proper superset — already covered.
        }
        reportedCycles.push_back(key);
        clean = false;
        std::string msg = "cyclic class-level const initializer: ";
        for (const ConstMemberSymbol* c : cyc) {
            std::string ownerName = c->parent
                ? std::string(c->parent->name)
                : std::string("?");
            msg += ownerName + "." +
                   std::string(c->name) + " -> ";
        }
        std::string headName = cyc.front()->parent
            ? std::string(cyc.front()->parent->name)
            : std::string("?");
        msg += headName + "." +
               std::string(cyc.front()->name);
        msg += " (§22.12 / §17.14: a class-level const's "
               "initializer must be a compile-time constant "
               "expression with a well-defined value)";
        // Report at the reference that closes the cycle.
        ctx.diagnostics().report(Severity::Error, edgeLoc, msg);
    };

    std::function<void(const ConstMemberSymbol*)> dfs =
        [&](const ConstMemberSymbol* t) {
        color[t] = 1;
        path.push_back(t);
        auto it = deps.find(t);
        if (it != deps.end()) {
            for (const auto& [u, edgeLoc] : it->second) {
                if (color[u] == 1) {
                    auto pit = std::find(path.begin(), path.end(), u);
                    std::vector<const ConstMemberSymbol*> cyc(pit, path.end());
                    reportCycle(cyc, edgeLoc);
                } else if (color[u] == 0) {
                    dfs(u);
                }
            }
        }
        color[t] = 2;
        path.pop_back();
    };

    for (const ConstMemberSymbol* cm : locals)
        if (color[cm] == 0) dfs(cm);

    return clean;
}

}  // namespace bezel::sema
