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
//   * an out-argument is passed to a call (§22.3 — definitely-assigned after),
//   * a `for` loop's iteration variable is introduced.
//
// Out-parameters of the current method are seeded into declared_ (not
// assigned_) so they start unassigned. They must be assigned on every
// exit path (§22.3 callee-side check — fix #925).
//
// Reads consult the set; an IdentifierExpr that names a local declared
// in the current scope set (or any nested scope along the walk) but
// *not* in the assigned set is reported.
class MethodDA {
public:
    explicit MethodDA(SemaContext& ctx) : ctx_(ctx) {}

    // Run the walker. `params` lists the method's parameters. By-value
    // and `ref` params are definitely-assigned on entry (the caller
    // bound them). `out` params are NOT assigned on entry — the callee
    // must assign them on every exit path (§22.3); they are seeded into
    // `declared_` (so reads are tracked) but not into `assigned_`.
    void run(const std::vector<ast::Param>& params,
             const std::vector<std::unique_ptr<ast::Stmt>>& body) {
        // Seed out-params as declared-but-unassigned.
        for (const auto& p : params) {
            if (!p.name.empty() && p.modifier.rfind("out", 0) == 0 &&
                p.modifier.rfind("out var", 0) != 0) {
                // "out" or "out owned" — callee must assign. "out var"
                // is a caller-side inline-decl modifier, not a callee
                // param mode, so it's excluded here (no method body uses
                // "out var" as a param modifier; the check is belt-and-
                // suspenders).
                outParams_.insert(p.name);
                declared_.insert(p.name);
                // Intentionally NOT inserted into assigned_ — starts unassigned.
            }
        }
        for (const auto& s : body) {
            if (s) visitStmt(*s);
        }
        // §22.3 fall-off check: if control can reach the end of the method
        // without a return/throw, every out-param must be assigned.
        if (!terminated_) checkOutParams(/*span=*/nullptr);
    }

    // No-param overload for destructors (which have no params).
    void run(const std::vector<std::unique_ptr<ast::Stmt>>& body) {
        for (const auto& s : body) {
            if (s) visitStmt(*s);
        }
        // No out-params to check.
    }

private:
    SemaContext& ctx_;
    // declared_ records every local introduced in this method (by name).
    // assigned_ records those that are also known-assigned at the
    // current point. A read of a name in declared_ but not assigned_
    // is a diagnostic.
    std::unordered_set<std::string_view> declared_;
    std::unordered_set<std::string_view> assigned_;
    // out-parameter names that must be assigned on every exit path (§22.3).
    std::unordered_set<std::string_view> outParams_;
    // #1226 / §29.11: lazy LINQ bound vars that are definitely consumed at
    // the current program point (consumed = enumerated by a for-in or a
    // terminal op).  A second for-in over a definitely-consumed lazy var is
    // a compile error.  Tracking uses intersection semantics at branch joins
    // (parallel to assigned_): a var is "definitely consumed" only when it
    // is consumed on every path reaching the current point.
    std::unordered_set<std::string_view> consumedLazy_;
    // terminated_ = true when the current path ends unconditionally via
    // return/throw — used to suppress the fall-off out-param check on
    // unreachable tails (conservative: only set for top-level or all-
    // branch termination; partial-branch termination leaves it false so
    // the fall-off check runs on the live path).
    bool terminated_ = false;

    // §29.11 / #1226: true when `e`'s resolved type is the lazy LINQ
    // sentinel (Named TypeRef, no declaration, no astNode, no importedName,
    // at least one genericArg).  Must match isLazyEnumerableType() in codegen.
    bool isLazyEnumerableExprType(const ast::Expr& e) const {
        const TypeRef* ty = ctx_.exprType(e);
        return ty && ty->kind == TypeRef::Kind::Named && !ty->declaration &&
               !ty->astNode && !ty->genericArgs.empty() && ty->importedName.empty();
    }

    // §29.11 / #1353: mark a bound lazy local as consumed (or report a
    // compile error if it was already consumed). `context` briefly describes
    // the consumption point for the diagnostic ("enumeration", "LINQ chain",
    // "IEnumerable<T> argument"). per §29.11 v0.165
    void markLazyConsumed(const ast::IdentifierExpr& id,
                          const char* context) {
        if (consumedLazy_.count(id.name)) {
            ctx_.diagnostics().report(
                Severity::Error, id.span.begin,
                std::string("bound lazy query `") + std::string(id.name) +
                "` was already consumed — a lazy LINQ pipeline is "
                "single-consumption (§29.11); use `.ToArray()` to "
                "materialise it for multiple uses (consumed again via " +
                context + ")");
        } else {
            consumedLazy_.insert(id.name);
        }
    }

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
            // §22.3: every out-param must be assigned at each return.
            checkOutParams(&rs->span.begin);
            terminated_ = true;
            return;
        }
        if (auto* tr = dynamic_cast<const ast::ThrowStmt*>(&s)) {
            if (tr->value) visitExpr(*tr->value);
            // A throw is an exit path; per §22.3 out-params need not be
            // assigned before a throw (the callee did not return normally).
            terminated_ = true;
            return;
        }
        if (auto* ifs = dynamic_cast<const ast::IfStmt*>(&s)) {
            // Each branch starts from the entry-assigned set; at join
            // the intersection becomes the new assigned set.
            // consumedLazy_ uses the same intersection semantics (#1226/§29.11):
            // a var is "definitely consumed" only when consumed on every path.
            if (ifs->ifBranch.condition) visitExpr(*ifs->ifBranch.condition);
            auto entry = assigned_;
            auto consumedLazyEntry = consumedLazy_;
            bool entryTerminated = terminated_;
            terminated_ = false;
            for (const auto& bs : ifs->ifBranch.body) {
                if (bs) visitStmt(*bs);
            }
            std::vector<std::unordered_set<std::string_view>> branchOuts;
            branchOuts.push_back(std::move(assigned_));
            std::vector<std::unordered_set<std::string_view>> consumedLazyOuts;
            consumedLazyOuts.push_back(std::move(consumedLazy_));
            std::vector<bool> branchTerminated;
            branchTerminated.push_back(terminated_);
            for (const auto& el : ifs->elifBranches) {
                assigned_ = entry;
                consumedLazy_ = consumedLazyEntry;
                terminated_ = false;
                if (el.condition) visitExpr(*el.condition);
                for (const auto& bs : el.body) {
                    if (bs) visitStmt(*bs);
                }
                branchOuts.push_back(std::move(assigned_));
                consumedLazyOuts.push_back(std::move(consumedLazy_));
                branchTerminated.push_back(terminated_);
            }
            if (ifs->hasElse) {
                assigned_ = entry;
                consumedLazy_ = consumedLazyEntry;
                terminated_ = false;
                for (const auto& bs : ifs->elseBody) {
                    if (bs) visitStmt(*bs);
                }
                branchOuts.push_back(std::move(assigned_));
                consumedLazyOuts.push_back(std::move(consumedLazy_));
                branchTerminated.push_back(terminated_);
            } else {
                branchOuts.push_back(entry);           // no-else fall-through
                consumedLazyOuts.push_back(consumedLazyEntry); // no-else: no new consumption
                branchTerminated.push_back(false);     // no-else = path live
            }
            // Intersect assigned sets.
            auto out = branchOuts.front();
            for (std::size_t i = 1; i < branchOuts.size(); ++i) {
                std::unordered_set<std::string_view> tmp;
                for (auto n : out) {
                    if (branchOuts[i].count(n)) tmp.insert(n);
                }
                out = std::move(tmp);
            }
            assigned_ = std::move(out);
            // Intersect consumedLazy_ sets (#1226).
            auto consumedOut = consumedLazyOuts.front();
            for (std::size_t i = 1; i < consumedLazyOuts.size(); ++i) {
                std::unordered_set<std::string_view> tmp;
                for (auto n : consumedOut) {
                    if (consumedLazyOuts[i].count(n)) tmp.insert(n);
                }
                consumedOut = std::move(tmp);
            }
            consumedLazy_ = std::move(consumedOut);
            // Post-if path is terminated only if all branches terminated.
            bool allTerminated = true;
            for (bool t : branchTerminated) {
                if (!t) { allTerminated = false; break; }
            }
            terminated_ = entryTerminated || allTerminated;
            return;
        }
        if (auto* ws = dynamic_cast<const ast::WhileStmt*>(&s)) {
            if (ws->condition) visitExpr(*ws->condition);
            // Conservative: body may not execute (zero-trip), so any
            // assignments inside don't propagate to post-loop. Walk to
            // diagnose reads but restore the entry set afterwards.
            // consumedLazy_ (#1226) is also restored: the loop may run 0
            // times, so we cannot claim a lazy var inside the body is
            // definitely consumed after the loop.
            auto entry = assigned_;
            auto consumedLazyEntry = consumedLazy_;
            bool entryTerminated = terminated_;
            terminated_ = false;
            for (const auto& bs : ws->body) {
                if (bs) visitStmt(*bs);
            }
            assigned_ = std::move(entry);
            consumedLazy_ = std::move(consumedLazyEntry);
            terminated_ = entryTerminated; // loop may not execute
            return;
        }
        if (auto* fs = dynamic_cast<const ast::ForStmt*>(&s)) {
            // #1226 / §29.11: check and mark lazy-enumerable iterable as
            // consumed.  A second for-in over the same bound lazy local is
            // a compile error (the runtime would raise InvalidOperationException
            // — the static check surfaces it earlier with a better message).
            if (fs->iterable) {
                if (const auto* id = dynamic_cast<const ast::IdentifierExpr*>(
                        fs->iterable.get())) {
                    if (isLazyEnumerableExprType(*fs->iterable)) {
                        // §29.11 point 1: enumeration (for-in) consumes the
                        // bound lazy. Use markLazyConsumed for consistency.
                        markLazyConsumed(*id, "for-in enumeration");
                    }
                }
                visitExpr(*fs->iterable);
            }
            if (fs->withExpr) visitExpr(*fs->withExpr);
            auto entry = assigned_;
            // Save consumedLazy_ AFTER marking the iterable (so the iterable
            // mark persists after the loop) but BEFORE entering the body (so
            // body-internal lazy consumptions are rolled back conservatively —
            // a body that runs 0 times does not consume any inner lazy vars).
            auto consumedLazyEntry = consumedLazy_;
            bool entryTerminated = terminated_;
            terminated_ = false;
            if (!fs->varName.empty()) {
                declared_.insert(fs->varName);
                assigned_.insert(fs->varName);
            }
            for (const auto& bs : fs->body) {
                if (bs) visitStmt(*bs);
            }
            assigned_ = std::move(entry);
            consumedLazy_ = std::move(consumedLazyEntry); // body-marks removed
            terminated_ = entryTerminated; // loop may not execute
            return;
        }
        if (auto* dob = dynamic_cast<const ast::DoEndBlock*>(&s)) {
            // `do ... end` is a single straight-line block; terminated_
            // propagates naturally (a throw/return inside terminates the path).
            for (const auto& bs : dob->body) {
                if (bs) visitStmt(*bs);
            }
            return;
        }
        if (auto* ts = dynamic_cast<const ast::TryStmt*>(&s)) {
            auto entry = assigned_;
            auto consumedLazyEntry = consumedLazy_;
            bool entryTerminated = terminated_;
            terminated_ = false;
            for (const auto& bs : ts->body) {
                if (bs) visitStmt(*bs);
            }
            // Try-body may abort at any point → join with entry for
            // the post-try set; catch arms each start from entry.
            std::vector<std::unordered_set<std::string_view>> outs;
            outs.push_back(std::move(assigned_));
            std::vector<std::unordered_set<std::string_view>> consumedLazyOuts;
            consumedLazyOuts.push_back(std::move(consumedLazy_));
            std::vector<bool> armTerminated;
            armTerminated.push_back(terminated_);
            for (const auto& arm : ts->catches) {
                assigned_ = entry;
                consumedLazy_ = consumedLazyEntry;
                terminated_ = false;
                if (arm.body) visitStmt(*arm.body);
                outs.push_back(std::move(assigned_));
                consumedLazyOuts.push_back(std::move(consumedLazy_));
                armTerminated.push_back(terminated_);
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
            // Intersect consumedLazy_ sets (#1226).
            auto consumedPost = consumedLazyOuts.front();
            for (std::size_t i = 1; i < consumedLazyOuts.size(); ++i) {
                std::unordered_set<std::string_view> tmp;
                for (auto n : consumedPost) {
                    if (consumedLazyOuts[i].count(n)) tmp.insert(n);
                }
                consumedPost = std::move(tmp);
            }
            consumedLazy_ = std::move(consumedPost);
            // Post-try path is terminated only if all arms terminated.
            bool allTerminated = true;
            for (bool t : armTerminated) {
                if (!t) { allTerminated = false; break; }
            }
            terminated_ = entryTerminated || allTerminated;
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
            // §29.11 point 2 (#1353): applying a further deferred LINQ
            // operator to a bound lazy local consumes that local.  Detect
            // by: call result type is lazy-enumerable AND the callee is a
            // MemberAccessExpr whose receiver is a lazy-enumerable identifier.
            // Example: `var q2 := q.Select(f)` — q is consumed here.
            // per §29.11 v0.165
            if (isLazyEnumerableExprType(e)) {
                if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(
                        ce->callee.get())) {
                    if (ma->target) {
                        if (const auto* rid =
                                dynamic_cast<const ast::IdentifierExpr*>(
                                    ma->target.get())) {
                            if (isLazyEnumerableExprType(*ma->target)) {
                                // Check DA assignment (read) first, then
                                // mark the bound lazy as consumed.
                                checkRead(*rid);
                                markLazyConsumed(*rid, "LINQ chain operator");
                                // Visit the call's own args (lambda/predicates)
                                // — but not the callee identifier again.
                                for (const auto& a : ce->args) {
                                    visitCallArg(a);
                                }
                                return;
                            }
                        }
                    }
                }
            }
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
        // §29.11 point 3 (#1353): passing a bound lazy query as an argument
        // (not ref/out) consumes it — the single-enumeration right transfers
        // to the callee.  In well-typed Bezel the parameter type must be
        // IEnumerable<T> (or a supertype), so any non-ref/non-out lazy arg
        // is a Point 3 consumption. per §29.11 v0.165
        if (a.modifier != "ref") {  // not a ref arg (ref is still a read)
            if (const auto* id =
                    dynamic_cast<const ast::IdentifierExpr*>(a.value.get())) {
                if (isLazyEnumerableExprType(*a.value)) {
                    checkRead(*id);  // DA assignment check
                    markLazyConsumed(*id, "IEnumerable<T> argument");
                    return;  // consumed — treated as a read+consume
                }
            }
        }
        visitExpr(*a.value);
    }

    // §22.3: check that every out-parameter is in assigned_ at the current
    // program point. Reports an error for each unassigned one. `loc` is the
    // source location to attach the diagnostic (null → no location).
    void checkOutParams(const SourceLocation* loc) {
        for (auto name : outParams_) {
            if (!assigned_.count(name)) {
                SourceLocation sl;
                if (loc) sl = *loc;
                ctx_.diagnostics().report(
                    Severity::Error, sl,
                    std::string("out parameter `") + std::string(name) +
                    "` is not definitely assigned on every exit path "
                    "(§22.3 — the callee must assign every `out` parameter "
                    "before returning)");
            }
        }
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
            da.run(md->params, md->stmts);
        }
    } else if (auto* cd = dynamic_cast<const ast::ConstructorDecl*>(&m)) {
        MethodDA da(ctx);
        da.run(cd->params, cd->body);
    } else if (auto* dd = dynamic_cast<const ast::DestructorDecl*>(&m)) {
        MethodDA da(ctx);
        da.run(dd->body);  // destructors have no params
    } else if (auto* od = dynamic_cast<const ast::OperatorDecl*>(&m)) {
        if (od->bodyKind == ast::MethodBodyKind::Block) {
            MethodDA da(ctx);
            da.run(od->params, od->stmts);
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
