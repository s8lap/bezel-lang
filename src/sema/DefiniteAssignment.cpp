#include "sema/DefiniteAssignment.hpp"

#include <algorithm>
#include <string>
#include <unordered_set>
#include <vector>

#include "ast/Node.hpp"
#include "sema/SemaContext.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeRef.hpp"

namespace bezel::sema {

namespace {

// Per-method walker. The state is the set of locally-declared names
// that have been definitely assigned at the current program point.
// Names enter the set when:
//   * a LocalDecl with an initializer is processed,
//   * an AssignmentStmt sets a previously-declared local,
//   * an out-parameter is bound (slice DA1 doesn't model out-args yet),
//   * a `for` loop's iteration variable is introduced.
//
// Reads consult the set; an IdentifierExpr that names a local declared
// in the current scope set (or any nested scope along the walk) but
// *not* in the assigned set is reported.
class MethodDA {
public:
    explicit MethodDA(SemaContext& ctx) : ctx_(ctx) {}

    // Run the walker. `paramSyms` (when provided) lists the method's
    // params — they're treated as definitely-assigned at entry (the
    // caller bound them).
    void run(const std::vector<std::unique_ptr<ast::Stmt>>& body) {
        for (const auto& s : body) {
            if (s) visitStmt(*s);
        }
    }

private:
    SemaContext& ctx_;
    // declared_ records every local introduced in this method (by name).
    // assigned_ records those that are also known-assigned at the
    // current point. A read of a name in declared_ but not assigned_
    // is a diagnostic.
    std::unordered_set<std::string_view> declared_;
    std::unordered_set<std::string_view> assigned_;

    void visitStmt(const ast::Stmt& s) {
        if (auto* ld = dynamic_cast<const ast::LocalDecl*>(&s)) {
            if (ld->initializer) visitExpr(*ld->initializer);
            if (!ld->name.empty()) {
                declared_.insert(ld->name);
                if (ld->initializer) {
                    assigned_.insert(ld->name);
                } else if (ld->type) {
                    // §15.1: inline arrays are zero-initialized when declared
                    // without an initializer (the declaration is the init);
                    // treat them as definitely assigned.
                    if (auto* rt = ctx_.resolvedType(*ld->type)) {
                        if (rt->kind == sema::TypeRef::Kind::InlineArray)
                            assigned_.insert(ld->name);
                    }
                }
            }
            return;
        }
        if (auto* lc = dynamic_cast<const ast::LocalConstDecl*>(&s)) {
            if (lc->value) visitExpr(*lc->value);
            if (!lc->name.empty()) {
                declared_.insert(lc->name);
                assigned_.insert(lc->name);  // const requires initializer
            }
            return;
        }
        if (auto* as = dynamic_cast<const ast::AssignmentStmt*>(&s)) {
            // RHS first — that's a read before the write.
            if (as->value) visitExpr(*as->value);
            // Target as IdentifierExpr → local gets assigned. Other
            // targets (member/index) read their root path first.
            if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(
                    as->target.get())) {
                if (declared_.count(id->name)) {
                    assigned_.insert(id->name);
                }
                // Bare ident that's NOT a declared local — could be a
                // field-on-this or a typo. Sema slice 3d's silent fall-
                // through covers the first case; here we don't flag.
            } else if (as->target) {
                visitExpr(*as->target);
            }
            return;
        }
        if (auto* es = dynamic_cast<const ast::ExpressionStmt*>(&s)) {
            if (es->expr) visitExpr(*es->expr);
            return;
        }
        if (auto* ds = dynamic_cast<const ast::DiscardStmt*>(&s)) {
            if (ds->value) visitExpr(*ds->value);
            return;
        }
        if (auto* rs = dynamic_cast<const ast::ReturnStmt*>(&s)) {
            if (rs->value) visitExpr(*rs->value);
            return;
        }
        if (auto* tr = dynamic_cast<const ast::ThrowStmt*>(&s)) {
            if (tr->value) visitExpr(*tr->value);
            return;
        }
        if (auto* ifs = dynamic_cast<const ast::IfStmt*>(&s)) {
            // Each branch starts from the entry-assigned set; at join
            // the intersection becomes the new assigned set.
            if (ifs->ifBranch.condition) visitExpr(*ifs->ifBranch.condition);
            auto entry = assigned_;
            for (const auto& bs : ifs->ifBranch.body) {
                if (bs) visitStmt(*bs);
            }
            std::vector<std::unordered_set<std::string_view>> branchOuts;
            branchOuts.push_back(std::move(assigned_));
            for (const auto& el : ifs->elifBranches) {
                assigned_ = entry;
                if (el.condition) visitExpr(*el.condition);
                for (const auto& bs : el.body) {
                    if (bs) visitStmt(*bs);
                }
                branchOuts.push_back(std::move(assigned_));
            }
            if (ifs->hasElse) {
                assigned_ = entry;
                for (const auto& bs : ifs->elseBody) {
                    if (bs) visitStmt(*bs);
                }
                branchOuts.push_back(std::move(assigned_));
            } else {
                branchOuts.push_back(entry);  // no-else fall-through
            }
            // Intersect.
            auto out = branchOuts.front();
            for (std::size_t i = 1; i < branchOuts.size(); ++i) {
                std::unordered_set<std::string_view> tmp;
                for (auto n : out) {
                    if (branchOuts[i].count(n)) tmp.insert(n);
                }
                out = std::move(tmp);
            }
            assigned_ = std::move(out);
            return;
        }
        if (auto* ws = dynamic_cast<const ast::WhileStmt*>(&s)) {
            if (ws->condition) visitExpr(*ws->condition);
            // Conservative: body may not execute (zero-trip), so any
            // assignments inside don't propagate to post-loop. Walk to
            // diagnose reads but restore the entry set afterwards.
            auto entry = assigned_;
            for (const auto& bs : ws->body) {
                if (bs) visitStmt(*bs);
            }
            assigned_ = std::move(entry);
            return;
        }
        if (auto* fs = dynamic_cast<const ast::ForStmt*>(&s)) {
            if (fs->iterable) visitExpr(*fs->iterable);
            if (fs->withExpr) visitExpr(*fs->withExpr);
            auto entry = assigned_;
            if (!fs->varName.empty()) {
                declared_.insert(fs->varName);
                assigned_.insert(fs->varName);
            }
            for (const auto& bs : fs->body) {
                if (bs) visitStmt(*bs);
            }
            assigned_ = std::move(entry);
            return;
        }
        if (auto* dob = dynamic_cast<const ast::DoEndBlock*>(&s)) {
            // `do ... end` is a single straight-line block.
            for (const auto& bs : dob->body) {
                if (bs) visitStmt(*bs);
            }
            return;
        }
        if (auto* ts = dynamic_cast<const ast::TryStmt*>(&s)) {
            auto entry = assigned_;
            for (const auto& bs : ts->body) {
                if (bs) visitStmt(*bs);
            }
            // Try-body may abort at any point → join with entry for
            // the post-try set; catch arms each start from entry.
            std::vector<std::unordered_set<std::string_view>> outs;
            outs.push_back(std::move(assigned_));
            for (const auto& arm : ts->catches) {
                assigned_ = entry;
                if (arm.body) visitStmt(*arm.body);
                outs.push_back(std::move(assigned_));
            }
            auto post = outs.front();
            for (std::size_t i = 1; i < outs.size(); ++i) {
                std::unordered_set<std::string_view> tmp;
                for (auto n : post) {
                    if (outs[i].count(n)) tmp.insert(n);
                }
                post = std::move(tmp);
            }
            assigned_ = std::move(post);
            for (const auto& bs : ts->finallyBody) {
                if (bs) visitStmt(*bs);
            }
            return;
        }
        if (auto* is = dynamic_cast<const ast::IncrementStmt*>(&s)) {
            if (is->target) visitExpr(*is->target);
            return;
        }
        if (auto* ys = dynamic_cast<const ast::YieldStmt*>(&s)) {
            if (ys->value) visitExpr(*ys->value);
            return;
        }
    }

    void visitExpr(const ast::Expr& e) {
        if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(&e)) {
            checkRead(*id);
            return;
        }
        // Recurse into children. We share this with the ownership
        // analyzer's shape but don't need its discharge logic.
        if (auto* be = dynamic_cast<const ast::BinaryExpr*>(&e)) {
            if (be->left) visitExpr(*be->left);
            if (be->right) visitExpr(*be->right);
            return;
        }
        if (auto* ue = dynamic_cast<const ast::UnaryExpr*>(&e)) {
            if (ue->operand) visitExpr(*ue->operand);
            return;
        }
        if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
            if (ma->target) visitExpr(*ma->target);
            return;
        }
        if (auto* nc = dynamic_cast<const ast::NullCondMemberExpr*>(&e)) {
            if (nc->target) visitExpr(*nc->target);
            return;
        }
        if (auto* ce = dynamic_cast<const ast::CallExpr*>(&e)) {
            if (ce->callee) visitExpr(*ce->callee);
            for (const auto& a : ce->args) {
                visitCallArg(a);
            }
            return;
        }
        if (auto* nc = dynamic_cast<const ast::NullCondCallExpr*>(&e)) {
            if (nc->callee) visitExpr(*nc->callee);
            for (const auto& a : nc->args) {
                visitCallArg(a);
            }
            return;
        }
        if (auto* ix = dynamic_cast<const ast::IndexExpr*>(&e)) {
            if (ix->target) visitExpr(*ix->target);
            for (const auto& a : ix->indices) {
                if (a.value) visitExpr(*a.value);
            }
            return;
        }
        if (auto* bg = dynamic_cast<const ast::PostfixBangExpr*>(&e)) {
            if (bg->operand) visitExpr(*bg->operand);
            return;
        }
        if (auto* ne = dynamic_cast<const ast::NewExpr*>(&e)) {
            for (const auto& a : ne->args) {
                if (a.value) visitExpr(*a.value);
            }
            return;
        }
        if (auto* cd = dynamic_cast<const ast::ConditionalExpr*>(&e)) {
            if (cd->condition) visitExpr(*cd->condition);
            if (cd->thenValue) visitExpr(*cd->thenValue);
            if (cd->elseValue) visitExpr(*cd->elseValue);
            return;
        }
        if (auto* te = dynamic_cast<const ast::TypeTestExpr*>(&e)) {
            if (te->operand) visitExpr(*te->operand);
            return;
        }
        if (auto* th = dynamic_cast<const ast::ThrowExpr*>(&e)) {
            if (th->operand) visitExpr(*th->operand);
            return;
        }
        if (auto* we = dynamic_cast<const ast::WithExpr*>(&e)) {
            if (we->source) visitExpr(*we->source);
            for (const auto& ia : we->assigns) {
                if (ia.value) visitExpr(*ia.value);
            }
            return;
        }
        if (auto* lam = dynamic_cast<const ast::LambdaExpr*>(&e)) {
            // Lambda body is its own DA scope — we don't analyze it
            // recursively here; that's an exercise for a later slice
            // (capture model and closure-DA are non-trivial).
            (void)lam;
            return;
        }
        if (auto* cl = dynamic_cast<const ast::CollectionLiteralExpr*>(&e)) {
            for (const auto& k : cl->keys) {
                if (k) visitExpr(*k);
            }
            for (const auto& v : cl->elements) {
                if (v) visitExpr(*v);
            }
            return;
        }
        // Literals, This/Base/TypeOf/NameOf/Default — no child to walk.
    }

    // §22.3 (#747 R43): an `out` argument is an OUTPUT location, not a read —
    // "the variable need not be initialized before the call", and the callee
    // assigns it on every exit path, so a bare local passed as `out x` is
    // DEFINITELY ASSIGNED after the call. Mark it assigned and do not treat it
    // as a read. (Previously every call argument was visited as a read, so a
    // pre-declared `var x: int; M(out x)` drew a false "read of unassigned"
    // both at the argument position and at every post-call read. The inline
    // `out var x` form was unaffected only because its name is not tracked in
    // declared_.) A `ref` argument, by contrast, IS a read (the variable must
    // be assigned before the call) and is visited normally. An `out` target
    // that is a member/index location (`out obj.f`, `out arr[i]`) still reads
    // its sub-expressions (the receiver and index), so only a bare identifier
    // target is treated as a pure write.
    void visitCallArg(const ast::CallArgument& a) {
        if (!a.value) return;
        if (a.modifier.rfind("out", 0) == 0) {
            if (auto* id =
                    dynamic_cast<const ast::IdentifierExpr*>(a.value.get())) {
                assigned_.insert(id->name);
                return;  // pure write — not a read
            }
        }
        visitExpr(*a.value);
    }

    void checkRead(const ast::IdentifierExpr& id) {
        if (id.name == "this" || id.name == "base") return;
        // Only flag locals declared in this method that haven't been
        // assigned. Fields, params, and outer scopes pass silently.
        if (declared_.count(id.name) && !assigned_.count(id.name)) {
            ctx_.diagnostics().report(
                Severity::Error, id.span.begin,
                std::string("read of unassigned local `") +
                std::string(id.name) +
                "` (definite-assignment rule — every read must follow "
                "an assignment on every reaching path)");
        }
    }
};

void checkMember(const ast::Member& m, SemaContext& ctx) {
    if (auto* md = dynamic_cast<const ast::MethodDecl*>(&m)) {
        if (md->bodyKind == ast::MethodBodyKind::Block) {
            MethodDA da(ctx);
            da.run(md->stmts);
        }
    } else if (auto* cd = dynamic_cast<const ast::ConstructorDecl*>(&m)) {
        MethodDA da(ctx);
        da.run(cd->body);
    } else if (auto* dd = dynamic_cast<const ast::DestructorDecl*>(&m)) {
        MethodDA da(ctx);
        da.run(dd->body);
    } else if (auto* od = dynamic_cast<const ast::OperatorDecl*>(&m)) {
        if (od->bodyKind == ast::MethodBodyKind::Block) {
            MethodDA da(ctx);
            da.run(od->stmts);
        }
    } else if (auto* nt = dynamic_cast<const ast::NestedTypeMember*>(&m)) {
        if (nt->decl) {
            for (const auto& innerM : nt->decl->members) {
                if (innerM) checkMember(*innerM, ctx);
            }
        }
    }
}

void checkTopDecl(const ast::TopDecl& d, SemaContext& ctx) {
    if (auto* td = dynamic_cast<const ast::TypeDecl*>(&d)) {
        for (const auto& m : td->members) {
            if (m) checkMember(*m, ctx);
        }
    } else if (auto* ext = dynamic_cast<const ast::ExtensionDecl*>(&d)) {
        for (const auto& m : ext->members) {
            if (m) checkMember(*m, ctx);
        }
    }
}

} // namespace

bool checkDefiniteAssignment(const ast::SourceFile& src, SemaContext& ctx) {
    for (const auto& d : src.topDecls) {
        if (d) checkTopDecl(*d, ctx);
    }
    return !ctx.diagnostics().hasErrors();
}

} // namespace bezel::sema
