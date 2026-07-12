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

// ---------- §22.7 (v0.184) construction-time `this`-observation helpers ------
//
// The observation gate (§22.7 rule 1) is satisfied only where every field the
// declaring class must explicitly initialize is definitely assigned. That set
// is exactly §7.9's non-nullable *reference*-typed fields without an
// initializer (fields with an initializer count as assigned at body entry;
// value-typed fields have a defined default). Mirrors ownership's
// isNonNullableReferenceField (the O9 field-DA check) — same classification,
// so the observation gate and the field-DA obligation stay in lock-step.
bool isSevenNineStrictField(const ast::FieldDecl& f, SemaContext& ctx) {
    for (auto m : f.modifiers)
        if (m == "static") return false;
    if (f.initializer) return false;  // assigned at body entry
    if (!f.type) return false;
    auto* rt = ctx.resolvedType(*f.type);
    if (!rt || rt->nullable) return false;
    switch (rt->kind) {
        case TypeRef::Kind::Primitive:
            return rt->primitive == PrimitiveType::String ||
                   rt->primitive == PrimitiveType::Object;
        case TypeRef::Kind::Function:
            return true;
        case TypeRef::Kind::Named:
            if (!rt->declaration) return false;  // built-in / generic — skip
            return rt->declaration->kind == ast::TypeDeclKind::Class ||
                   rt->declaration->kind == ast::TypeDeclKind::Interface;
        default:
            return false;  // Unit, InlineArray, Error → value/unknown
    }
}

// §22.7 (v0.184): a non-nullable-reference *auto-property* without an
// initializer is §7.9-strict backing storage too (§22.7: "auto-property
// assignments and reads … each is the backing field's assignment or read"), so
// it joins the observation gate's needing-set exactly like a field. Indexers,
// static, and initialized auto-properties are excluded (the last is assigned at
// entry). A computed (block/expression-bodied) property has no backing storage
// and is not counted.
bool isSevenNineStrictAutoProp(const ast::PropertyDecl& p, SemaContext& ctx) {
    if (p.bodyKind != ast::PropertyBodyKind::Auto) return false;
    if (p.isIndexer || p.autoInitializer) return false;
    for (auto m : p.modifiers)
        if (m == "static") return false;
    if (!p.type) return false;
    auto* rt = ctx.resolvedType(*p.type);
    if (!rt || rt->nullable) return false;
    switch (rt->kind) {
        case TypeRef::Kind::Primitive:
            return rt->primitive == PrimitiveType::String ||
                   rt->primitive == PrimitiveType::Object;
        case TypeRef::Kind::Function:
            return true;
        case TypeRef::Kind::Named:
            if (!rt->declaration) return false;
            return rt->declaration->kind == ast::TypeDeclKind::Class ||
                   rt->declaration->kind == ast::TypeDeclKind::Interface;
        default:
            return false;
    }
}

// True when `e` is the bare `this`/`base` keyword (stored as an IdentifierExpr
// with the keyword lexeme, per Parser_Exprs §22.3).
bool isNamedIdent(const ast::Expr& e, std::string_view n) {
    auto* id = dynamic_cast<const ast::IdentifierExpr*>(&e);
    return id && id->name == n;
}

// §22.7 rule 2: a `virtual`/`override`/`abstract` member (dispatching family).
bool methodIsVirtualFamily(const MethodSymbol& ms) {
    for (auto m : ms.modifiers)
        if (m == "virtual" || m == "override" || m == "abstract") return true;
    if (auto* md = dynamic_cast<const ast::MethodDecl*>(ms.decl))
        return md->bodyKind == ast::MethodBodyKind::Abstract;
    return false;
}
bool propIsVirtualFamily(const PropertySymbol& ps) {
    for (auto m : ps.modifiers)
        if (m == "virtual" || m == "override" || m == "abstract") return true;
    return false;
}

// §19.3/§19.6: an unguarded `_` (DiscardPattern) or unconstrained `var x`
// (BindingPattern with no type annotation, no `when` guard) matches any value —
// a trivial catch-all arm. Duplicated here rather than reused because
// ExprTypecheck.cpp's identically-named helper has internal (anonymous-
// namespace) linkage in that translation unit. Deliberately narrow — it
// recognizes only the two syntactic forms the spec calls out, not a full
// enum/bool coverage proof — so the CaseStmt join below stays conservative: a
// case lacking this explicit catch-all always carries a live no-match fall-
// through path (§19.6), never accidentally exhaustive. fix #1681
bool caseArmIsUnguardedCatchAll(const ast::CaseArm& arm) {
    if (arm.guard) return false;
    const ast::Pattern* p = arm.pattern.get();
    if (dynamic_cast<const ast::DiscardPattern*>(p)) return true;
    if (auto* bp = dynamic_cast<const ast::BindingPattern*>(p))
        return bp->type == nullptr;
    return false;
}

// fix #1753 / §17.x, §22.3, §22.7: join the per-branch definitely-assigned
// sets of an if/try/case, intersecting *only* the branches that do NOT provably
// diverge (throw/return). A branch flagged `terminated[i]` reaches no path to
// the join point, so — per "definitely assigned on every REACHING path" — it
// must contribute nothing to the join; including it wrongly shrinks the
// intersection and over-rejects (`if c then x:=1 else return end; _:=x`, or the
// field analog before a `this`-observation). A branch that falls through is
// still intersected, so a genuinely-unassigned read on some live path is still
// caught (no under-reject). If *every* branch diverges the join is unreachable;
// fall back to the whole-branch intersection (prior behavior) so no dead-code
// read that previously passed regresses — soundness is unaffected there because
// no reachable path observes the result. `entry` is used only for the
// degenerate no-branch case.
std::unordered_set<std::string_view> joinLiveBranches(
    const std::vector<std::unordered_set<std::string_view>>& outs,
    const std::vector<bool>& terminated,
    const std::unordered_set<std::string_view>& entry) {
    std::unordered_set<std::string_view> result;
    bool any = false;
    for (std::size_t i = 0; i < outs.size(); ++i) {
        if (i < terminated.size() && terminated[i]) continue;  // diverging arm
        if (!any) {
            result = outs[i];
            any = true;
        } else {
            std::unordered_set<std::string_view> tmp;
            for (auto n : result)
                if (outs[i].count(n)) tmp.insert(n);
            result = std::move(tmp);
        }
    }
    if (any) return result;
    if (outs.empty()) return entry;
    // All branches diverge → unreachable join; preserve prior full intersection.
    result = outs.front();
    for (std::size_t i = 1; i < outs.size(); ++i) {
        std::unordered_set<std::string_view> tmp;
        for (auto n : result)
            if (outs[i].count(n)) tmp.insert(n);
        result = std::move(tmp);
    }
    return result;
}

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

    // §22.7 (v0.184): walk a constructor body with construction-time
    // `this`-observation checking layered onto the ordinary local
    // definite-assignment walk. `klass` is the declaring class (for the
    // §7.9-strict field set, the sealed/abstract status, and rule 2). The walk
    // is the same flow-sensitive pass as `run`; the extra state is
    // `ctorMode_`/`assignedFields_`/`fieldsNeedingInit_` (see visitStmt/
    // visitExpr for where observations are gated).
    void runConstructor(const ast::ConstructorDecl& cd,
                        const ast::TypeDecl& klass) {
        ctorMode_ = true;
        bool sealed = false, abstract = false;
        for (auto m : klass.modifiers) {
            if (m == "sealed") sealed = true;
            if (m == "abstract") abstract = true;
        }
        classSealedNonAbstract_ = sealed && !abstract;
        for (const auto& m : klass.members) {
            if (auto* f = dynamic_cast<const ast::FieldDecl*>(m.get())) {
                if (isSevenNineStrictField(*f, ctx_))
                    fieldsNeedingInit_.push_back(f->name);
            } else if (auto* p =
                           dynamic_cast<const ast::PropertyDecl*>(m.get())) {
                if (isSevenNineStrictAutoProp(*p, ctx_))
                    fieldsNeedingInit_.push_back(p->name);
            }
        }
        // Seed out-params (constructors normally have none; kept for parity
        // with run() so an out-carrying ctor, if ever admitted, is checked).
        for (const auto& p : cd.params) {
            if (!p.name.empty() && p.modifier.rfind("out", 0) == 0 &&
                p.modifier.rfind("out var", 0) != 0) {
                outParams_.insert(p.name);
                declared_.insert(p.name);
            }
        }
        // §22.7 rule 1, chaining position: the `base(...)`/`this(...)` call is
        // the body's first statement (Node.hpp: CtorChain surfaces as an
        // ExpressionStmt of a CallExpr on `this`/`base`). Its arguments run
        // before any field of the declaring class is assigned, so any
        // observation of `this` there is an error. After a `this(...)` chain the
        // chained-to constructor has discharged every §7.9-strict field, so the
        // gate is satisfied on body entry; after `base(...)` (or the implicit
        // `base()`) the field initializers run but the needing-set is still
        // unassigned (the body must assign it).
        std::size_t startIdx = 0;
        if (const ast::CallExpr* chain = getChainingCall(cd)) {
            startIdx = 1;
            inChainingPosition_ = true;
            for (const auto& a : chain->args) visitCallArg(a);
            inChainingPosition_ = false;
            if (auto* cid = dynamic_cast<const ast::IdentifierExpr*>(
                    chain->callee.get())) {
                if (cid->name == "this")
                    for (auto n : fieldsNeedingInit_) assignedFields_.insert(n);
            }
        }
        for (std::size_t i = startIdx; i < cd.body.size(); ++i) {
            if (cd.body[i]) visitStmt(*cd.body[i]);
        }
        if (!terminated_) checkOutParams(/*loc=*/nullptr);
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

    // §22.7 (v0.184) construction-time `this`-observation checking. Active only
    // while walking a constructor body (runConstructor). The observation gate
    // (rule 1) needs field-level definite assignment threaded flow-sensitively,
    // exactly parallel to `assigned_` for locals: `assignedFields_` is the set
    // of §7.9-strict fields definitely assigned on every path reaching the
    // current point; `fieldsNeedingInit_` is the (declaration-ordered) list of
    // §7.9-strict fields the class must explicitly initialize. A synchronous
    // observation of `this` is admitted only where fieldsNeedingInit_ ⊆
    // assignedFields_.
    bool ctorMode_ = false;
    // True while visiting the arguments of the `base(...)`/`this(...)` chaining
    // call, where no field is assigned yet — any observation of `this` there is
    // an error (§22.7 rule 1, chaining position).
    bool inChainingPosition_ = false;
    // The declaring class is `sealed` and not `abstract` — the sole exemption
    // to §22.7 rule 2 (virtual dispatch from a constructor body).
    bool classSealedNonAbstract_ = false;
    std::vector<std::string_view> fieldsNeedingInit_;
    std::unordered_set<std::string_view> assignedFields_;

    // §22.4 (v0.181): short-circuit-sensitive definite-assignment state for a
    // boolean sub-expression. `afterTrue` / `afterFalse` are the sets of
    // definitely-assigned names on the paths where the expression evaluated to
    // `true` and to `false` respectively. They differ exactly where a
    // short-circuit operator skips a call: `A and Call(out var x)` assigns `x`
    // only on the true continuation; `A or Call(out var x)` only on the false
    // continuation.
    struct CondState {
        std::unordered_set<std::string_view> afterTrue;
        std::unordered_set<std::string_view> afterFalse;
    };

    // Set intersection helper (a local is definitely assigned only where it is
    // assigned on *every* incoming path).
    static std::unordered_set<std::string_view> intersectSets(
        const std::unordered_set<std::string_view>& a,
        const std::unordered_set<std::string_view>& b) {
        std::unordered_set<std::string_view> r;
        for (auto n : a)
            if (b.count(n)) r.insert(n);
        return r;
    }

    // §22.4 (v0.181): whether an inline `out var` recipient must be tracked
    // path-sensitively for definite assignment. Only a non-nullable
    // reference-typed inline out var is unsafe to read on a path where the
    // introducing call never ran; value-typed and nullable inline out vars are
    // default-initialized (§7.9) and readable everywhere they are in scope, so
    // they are left untracked. The decision is recorded on the LocalSymbol at
    // type-check time (from the DECLARED out-param type, before its nullability
    // was stripped), because `declType` alone can no longer distinguish a
    // declared `T?` reference out var from a `T` one.
    bool inlineOutVarNeedsDA(const ast::IdentifierExpr& id) const {
        auto* loc = dynamic_cast<const LocalSymbol*>(ctx_.exprSymbol(id));
        return loc && loc->outVarNeedsDefiniteAssignment;
    }

    // §22.4 (v0.181): evaluate `e` for its effect on `assigned_` with
    // short-circuit precision, returning the definitely-assigned sets on its
    // true and false continuations. Reads inside `e` are diagnosed in place; an
    // inline `out var` assignment inside a short-circuited operand lands only on
    // the branch where the introducing call actually ran. On return `assigned_`
    // is left at the intersection of the two branch sets — the state that holds
    // regardless of `e`'s value, correct for using `e` as a plain
    // sub-expression or statement.
    CondState evalCond(const ast::Expr& e) {
        if (auto* be = dynamic_cast<const ast::BinaryExpr*>(&e)) {
            if (be->op == "and") {
                CondState a = be->left ? evalCond(*be->left)
                                       : CondState{assigned_, assigned_};
                // The right operand is evaluated only when the left is true.
                assigned_ = a.afterTrue;
                CondState b = be->right ? evalCond(*be->right)
                                        : CondState{assigned_, assigned_};
                CondState r;
                r.afterTrue = b.afterTrue;  // whole is true ⇒ both ran and true
                r.afterFalse =
                    intersectSets(a.afterFalse, b.afterFalse);  // either false
                assigned_ = intersectSets(r.afterTrue, r.afterFalse);
                return r;
            }
            if (be->op == "or") {
                CondState a = be->left ? evalCond(*be->left)
                                       : CondState{assigned_, assigned_};
                // The right operand is evaluated only when the left is false.
                assigned_ = a.afterFalse;
                CondState b = be->right ? evalCond(*be->right)
                                        : CondState{assigned_, assigned_};
                CondState r;
                r.afterTrue =
                    intersectSets(a.afterTrue, b.afterTrue);  // either true
                r.afterFalse = b.afterFalse;  // whole is false ⇒ both ran, false
                assigned_ = intersectSets(r.afterTrue, r.afterFalse);
                return r;
            }
        }
        if (auto* ue = dynamic_cast<const ast::UnaryExpr*>(&e)) {
            if (ue->op == "not") {
                CondState a = ue->operand ? evalCond(*ue->operand)
                                          : CondState{assigned_, assigned_};
                // `not` swaps the two continuations; their intersection
                // (`assigned_`) is unchanged.
                return CondState{a.afterFalse, a.afterTrue};
            }
        }
        // Leaf / non-short-circuit expression: it evaluates unconditionally, so
        // its assignments and reads apply on both continuations.
        visitExpr(e);
        return CondState{assigned_, assigned_};
    }

    // True when `e` is a short-circuit boolean operator whose operands must be
    // evaluated path-sensitively (§22.4).
    static bool isShortCircuitExpr(const ast::Expr& e) {
        if (auto* be = dynamic_cast<const ast::BinaryExpr*>(&e))
            return be->op == "and" || be->op == "or";
        if (auto* ue = dynamic_cast<const ast::UnaryExpr*>(&e))
            return ue->op == "not";
        return false;
    }

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
            // §22.7 rule 1 (v0.184): a `:=` store to a field or auto-property of
            // `this` marks that field definitely assigned toward the
            // observation gate, and is itself NOT an observation (do not recurse
            // into the `this` receiver). A computed-property `:=` target falls
            // through to visitExpr below, where its setter invocation is gated.
            if (ctorMode_ && as->op == ":=" && as->target) {
                std::string_view fname;
                if (isThisFieldOrAutoPropTarget(*as->target, fname)) {
                    assignedFields_.insert(fname);
                    return;
                }
            }
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
            //
            // §22.4 (v0.181): the guard is evaluated short-circuit-sensitively.
            // The `then` branch is reached only when the guard is TRUE, so it
            // starts from the guard's after-true set (an inline `out var` in an
            // `and`-conjunct is assigned there); each `elif` guard, the `else`
            // body, and the no-else fall-through are reached only when the
            // preceding guards were FALSE, so they thread the guard's after-false
            // set — the path on which a short-circuited call never ran.
            bool entryTerminated = terminated_;
            terminated_ = false;
            auto entryAssigned = assigned_;  // fix #1753 join fallback
            CondState c0 = ifs->ifBranch.condition
                               ? evalCond(*ifs->ifBranch.condition)
                               : CondState{assigned_, assigned_};
            auto consumedLazyEntry = consumedLazy_;
            // §22.7 (v0.184): field-assignment state is threaded in parallel to
            // `assigned_`. A guard expression assigns no field (assignments are
            // statements), so every branch body starts from the same entry
            // field-state; the post-`if` state is the intersection of the branch
            // field-outs (a field is assigned after the `if` only where assigned
            // on every reaching path). `fieldEntry` is that per-branch start.
            auto fieldEntry = assignedFields_;
            // `falseState` threads the assigned set on the path where every
            // guard checked so far was false.
            auto falseState = std::move(c0.afterFalse);
            assigned_ = std::move(c0.afterTrue);
            for (const auto& bs : ifs->ifBranch.body) {
                if (bs) visitStmt(*bs);
            }
            std::vector<std::unordered_set<std::string_view>> branchOuts;
            branchOuts.push_back(std::move(assigned_));
            std::vector<std::unordered_set<std::string_view>> consumedLazyOuts;
            consumedLazyOuts.push_back(std::move(consumedLazy_));
            std::vector<std::unordered_set<std::string_view>> fieldOuts;
            fieldOuts.push_back(std::move(assignedFields_));
            std::vector<bool> branchTerminated;
            branchTerminated.push_back(terminated_);
            for (const auto& el : ifs->elifBranches) {
                assigned_ = falseState;
                consumedLazy_ = consumedLazyEntry;
                assignedFields_ = fieldEntry;
                terminated_ = false;
                CondState ci = el.condition
                                   ? evalCond(*el.condition)
                                   : CondState{assigned_, assigned_};
                falseState = std::move(ci.afterFalse);
                assigned_ = std::move(ci.afterTrue);
                for (const auto& bs : el.body) {
                    if (bs) visitStmt(*bs);
                }
                branchOuts.push_back(std::move(assigned_));
                consumedLazyOuts.push_back(std::move(consumedLazy_));
                fieldOuts.push_back(std::move(assignedFields_));
                branchTerminated.push_back(terminated_);
            }
            if (ifs->hasElse) {
                assigned_ = falseState;
                consumedLazy_ = consumedLazyEntry;
                assignedFields_ = fieldEntry;
                terminated_ = false;
                for (const auto& bs : ifs->elseBody) {
                    if (bs) visitStmt(*bs);
                }
                branchOuts.push_back(std::move(assigned_));
                consumedLazyOuts.push_back(std::move(consumedLazy_));
                fieldOuts.push_back(std::move(assignedFields_));
                branchTerminated.push_back(terminated_);
            } else {
                branchOuts.push_back(std::move(falseState)); // no-else: guard-false path
                consumedLazyOuts.push_back(consumedLazyEntry); // no-else: no new consumption
                fieldOuts.push_back(fieldEntry); // no-else: guard-false path assigns no field
                branchTerminated.push_back(false);     // no-else = path live
            }
            // Intersect assigned / field sets over the LIVE branches only —
            // a branch that provably diverges (throw/return) reaches no join
            // and must not shrink the intersection (fix #1753, §17.x/§22.7).
            assigned_ =
                joinLiveBranches(branchOuts, branchTerminated, entryAssigned);
            // Intersect field-assignment sets (§22.7), same live-branch join.
            assignedFields_ =
                joinLiveBranches(fieldOuts, branchTerminated, fieldEntry);
            // Intersect consumedLazy_ sets (#1226). Kept as a whole-branch
            // intersection: lazy single-consumption (#1226) is a distinct
            // obligation from definite-assignment and out of this fix's scope.
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
            // §22.4 (v0.181): the guard is evaluated short-circuit-sensitively
            // and at least once. The body runs only when the guard is TRUE, so
            // it starts from the guard's after-true set (an inline `out var` in
            // an `and`-conjunct is assigned there). A `while` exits with its
            // guard FALSE (or ran zero times), so the post-loop state is the
            // guard's after-false set. Body-internal assignments do not survive
            // (the loop may run zero times), so they are not propagated past.
            // consumedLazy_ (#1226) is likewise restored to its post-guard
            // state: a zero-trip loop consumes nothing inside the body.
            bool entryTerminated = terminated_;
            terminated_ = false;
            CondState c = ws->condition ? evalCond(*ws->condition)
                                        : CondState{assigned_, assigned_};
            auto consumedLazyEntry = consumedLazy_;
            assigned_ = std::move(c.afterTrue);
            // §22.7 (v0.184): a zero-trip loop assigns no field, so body field
            // assignments do not survive past the loop (an observation after the
            // loop cannot assume the body ran). Observations *inside* the body
            // still see the body's growing field-state (a single iteration).
            auto fieldEntry = assignedFields_;
            for (const auto& bs : ws->body) {
                if (bs) visitStmt(*bs);
            }
            assigned_ = std::move(c.afterFalse);
            consumedLazy_ = std::move(consumedLazyEntry);
            assignedFields_ = std::move(fieldEntry);
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
            // §22.7 (v0.184): like `while`, a zero-trip `for` body assigns no
            // field, so body field assignments do not survive past the loop.
            auto fieldEntry = assignedFields_;
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
            assignedFields_ = std::move(fieldEntry);
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
            // §22.7 (v0.184): the try-body may abort at any point (before a
            // field assignment completes), and each catch arm starts from the
            // entry field-state; a field is assigned after the `try` only where
            // assigned on every arm (the try-body-completed path and every
            // catch path). `fieldEntry` is that per-arm start.
            auto fieldEntry = assignedFields_;
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
            std::vector<std::unordered_set<std::string_view>> fieldOuts;
            fieldOuts.push_back(std::move(assignedFields_));
            std::vector<bool> armTerminated;
            armTerminated.push_back(terminated_);
            for (const auto& arm : ts->catches) {
                assigned_ = entry;
                consumedLazy_ = consumedLazyEntry;
                assignedFields_ = fieldEntry;
                terminated_ = false;
                if (arm.body) visitStmt(*arm.body);
                outs.push_back(std::move(assigned_));
                consumedLazyOuts.push_back(std::move(consumedLazy_));
                fieldOuts.push_back(std::move(assignedFields_));
                armTerminated.push_back(terminated_);
            }
            // Intersect assigned / field sets over the LIVE arms only — the
            // try-body-completed path or a catch arm that provably diverges
            // (throw/return) reaches no post-try join, so it must not shrink
            // the intersection (fix #1753, §17.x/§22.7).
            assigned_ = joinLiveBranches(outs, armTerminated, entry);
            // Intersect field-assignment sets (§22.7), same live-arm join.
            assignedFields_ =
                joinLiveBranches(fieldOuts, armTerminated, fieldEntry);
            // Intersect consumedLazy_ sets (#1226). Kept as a whole-arm
            // intersection: lazy single-consumption is a distinct obligation
            // from definite-assignment, out of this fix's scope.
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
        if (auto* cs = dynamic_cast<const ast::CaseStmt*>(&s)) {
            // §19 / §22.3 / §22.7 (fix #1681, rebased onto #1679's field-DA):
            // a statement-position `case` is a branching construct exactly
            // like `if`/`try` above — each arm is a mutually exclusive path
            // from the pre-case state, joined afterward by intersecting the
            // assigned sets (local AND field, §22.7) over the LIVE arms
            // (fix #1753). This construct was previously entirely unvisited
            // here (no dynamic_cast branch existed), so every statement inside
            // a case arm — `return`s, `out`-param assignments, and
            // `this.f := ...` field assignments — was invisible to
            // definite-assignment: an exhaustively-assigning case body was
            // wrongly REJECTED (the fall-off / observation check fired since
            // no arm's assignment was ever recorded), and a `return` in one
            // arm that never assigned an `out` param (or a field before a
            // `this`-observation) was wrongly ACCEPTED whenever an assignment
            // before/after the case masked the gap.
            //
            // §19.6: a statement-position case with no provable catch-all arm
            // has an implicit "no match" path that "falls through with no
            // action" — a live branch carrying the entry state unchanged,
            // joined in below exactly like `if`'s no-else branch.
            if (cs->scrutinee) visitExpr(*cs->scrutinee);
            bool entryTerminated = terminated_;
            auto entryAssigned = assigned_;
            auto entryConsumedLazy = consumedLazy_;
            auto fieldEntry = assignedFields_;
            std::vector<std::unordered_set<std::string_view>> armOuts;
            std::vector<std::unordered_set<std::string_view>> armConsumedLazyOuts;
            std::vector<std::unordered_set<std::string_view>> armFieldOuts;
            std::vector<bool> armTerminated;
            bool hasCatchAll = false;
            for (const auto& arm : cs->arms) {
                assigned_ = entryAssigned;
                consumedLazy_ = entryConsumedLazy;
                assignedFields_ = fieldEntry;
                terminated_ = false;
                // Pattern bindings (`var t`) are non-destructive fresh names
                // (§19 pattern binding), always definitely assigned within the
                // arm by construction, so they are intentionally not added to
                // declared_/assigned_ here (a read simply passes checkRead's
                // declared_ gate silently, same as a field or outer local;
                // §19.3).
                if (arm.guard) visitExpr(*arm.guard);
                if (caseArmIsUnguardedCatchAll(arm)) hasCatchAll = true;
                if (arm.bodyKind == ast::CaseArmBodyKind::Expression) {
                    if (arm.exprBody) visitExpr(*arm.exprBody);
                } else {
                    for (const auto& bs : arm.stmts) {
                        if (bs) visitStmt(*bs);
                    }
                }
                armOuts.push_back(std::move(assigned_));
                armConsumedLazyOuts.push_back(std::move(consumedLazy_));
                armFieldOuts.push_back(std::move(assignedFields_));
                armTerminated.push_back(terminated_);
            }
            if (!hasCatchAll) {
                // §19.6 implicit no-match fall-through: a live path that
                // assigns nothing — carries the entry state unchanged.
                armOuts.push_back(entryAssigned);
                armConsumedLazyOuts.push_back(entryConsumedLazy);
                armFieldOuts.push_back(fieldEntry);
                armTerminated.push_back(false);
            }
            // Join over the LIVE arms only (fix #1753): a `return`/`throw` arm
            // reaches no post-case join, so it must not shrink the local- or
            // field-DA intersection. consumedLazy_ stays a whole-arm
            // intersection (lazy single-consumption is out of DA scope).
            assigned_ = joinLiveBranches(armOuts, armTerminated, entryAssigned);
            assignedFields_ =
                joinLiveBranches(armFieldOuts, armTerminated, fieldEntry);
            if (!armConsumedLazyOuts.empty()) {
                auto consumedOut = armConsumedLazyOuts.front();
                for (std::size_t i = 1; i < armConsumedLazyOuts.size(); ++i) {
                    std::unordered_set<std::string_view> tmp;
                    for (auto n : consumedOut) {
                        if (armConsumedLazyOuts[i].count(n)) tmp.insert(n);
                    }
                    consumedOut = std::move(tmp);
                }
                consumedLazy_ = std::move(consumedOut);
            } else {
                consumedLazy_ = std::move(entryConsumedLazy);
            }
            bool allTerminated = true;
            for (bool t : armTerminated) {
                if (!t) { allTerminated = false; break; }
            }
            terminated_ = entryTerminated || allTerminated;
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
        // §22.4 (v0.181): a short-circuit boolean operator evaluates its
        // operands path-sensitively. Route it through evalCond, which leaves
        // `assigned_` at the state that holds regardless of the operator's
        // value (the intersection of its true/false continuations) — so a
        // conditionally-assigned inline `out var` does not leak past the
        // operator as definitely assigned.
        if (isShortCircuitExpr(e)) {
            (void)evalCond(e);
            return;
        }
        if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(&e)) {
            // §22.7 rule 1 (v0.184): a bare `this` reached here is `this` used
            // as a value — an argument, a stored/returned reference, an
            // `is`/`is not`/`as` operand — i.e. a synchronous observation.
            // Receiver-`this` uses (`this.f`, `this.M(...)`) are intercepted by
            // the parent MemberAccess/Call handlers and never recurse into this
            // leaf. `base` alone is not a value (only `base.member`), so only
            // `this` is gated. per §22.7
            if (ctorMode_ && id->name == "this") {
                gateObservation(id->span.begin, "reference to `this`");
                return;
            }
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
            // §22.7 (v0.184): `this.member`/`base.member` — a computed-property
            // read is an observation; a field/auto-property read is not.
            // Handled (and the `this`/`base` receiver not recursed into) here.
            if (visitCtorMemberAccess(*ma)) return;
            if (ma->target) visitExpr(*ma->target);
            return;
        }
        if (auto* nc = dynamic_cast<const ast::NullCondMemberExpr*>(&e)) {
            if (nc->target) visitExpr(*nc->target);
            return;
        }
        if (auto* ce = dynamic_cast<const ast::CallExpr*>(&e)) {
            // §22.7 rule 1/2 (v0.184): a receiver-`this` call (`this.M(...)`,
            // `base.M(...)`, or an implicit-`this` bare `M(...)`) is a
            // construction-time observation; handle it (and its args) here.
            if (visitCtorCall(*ce)) return;
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
                // §22.4 (v0.181): an inline `out var x` declares a fresh local
                // that is definitely assigned *only on paths where this call
                // actually evaluated*. Marking it in `assigned_` here records
                // the assignment on the current (call-ran) path; the
                // short-circuit machinery (evalCond) drops it again on any
                // branch where the call was skipped. To make a later read on
                // such a skipped path an error, the name must also be tracked in
                // `declared_` — but only when it is a non-nullable reference
                // type (§7.9); value-typed and nullable inline out vars are
                // default-initialized and readable on every path, so they stay
                // untracked (as before). The owning inline forms (`out owned
                // var` / `out using var`) carry a per-path disposal obligation
                // handled by the ownership pass and are not DA-tracked here.
                // A plain `out x` / `out owned x` targets a pre-declared local
                // (already in `declared_`) and is just marked assigned
                // (§22.3 / #747).
                if (a.modifier == "out var" && inlineOutVarNeedsDA(*id))
                    declared_.insert(id->name);
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

    // ---- §22.7 (v0.184) construction-time observation helpers (ctor mode) ----

    // The `base(...)`/`this(...)` chaining call, if this constructor has one —
    // the body's first ExpressionStmt of a CallExpr on `this`/`base`
    // (Node.hpp). Null otherwise (implicit `base()`).
    static const ast::CallExpr* getChainingCall(const ast::ConstructorDecl& cd) {
        if (cd.body.empty() || !cd.body.front()) return nullptr;
        auto* es =
            dynamic_cast<const ast::ExpressionStmt*>(cd.body.front().get());
        if (!es || !es->expr) return nullptr;
        auto* call = dynamic_cast<const ast::CallExpr*>(es->expr.get());
        if (!call || !call->callee) return nullptr;
        auto* id =
            dynamic_cast<const ast::IdentifierExpr*>(call->callee.get());
        if (id && (id->name == "this" || id->name == "base")) return call;
        return nullptr;
    }

    // §22.7 rule 1: report a synchronous `this`-observation that is not admitted
    // at the current program point — in the chaining position (no field yet
    // assigned) it is always an error; otherwise it is an error iff some
    // §7.9-strict field is not definitely assigned on every reaching path.
    // `what` names the observation form for the diagnostic. Returns nothing;
    // one diagnostic per unadmitted observation.
    void gateObservation(const SourceLocation& loc, const std::string& what) {
        if (!ctorMode_) return;
        if (inChainingPosition_) {
            ctx_.diagnostics().report(
                Severity::Error, loc,
                "cannot observe `this` (" + what +
                ") in the chaining position — `base(...)`/`this(...)` arguments "
                "run before any field of the declaring class is assigned "
                "(§22.7, v0.184)");
            return;
        }
        for (auto n : fieldsNeedingInit_) {
            if (!assignedFields_.count(n)) {
                ctx_.diagnostics().report(
                    Severity::Error, loc,
                    "cannot observe `this` (" + what +
                    ") before non-nullable field `" + std::string(n) +
                    "` is definitely assigned on every path — the observation "
                    "could read an unset field, breaking §7.9's guarantee that "
                    "a fully-constructed object has every non-nullable field "
                    "set (§22.7, v0.184)");
                return;
            }
        }
    }

    // §22.7 rule 2: a dispatching call on `this` to a `virtual`/`override`/
    // `abstract` member from a non-sealed (or abstract) constructor body is a
    // compile error — during construction the runtime type may be a derived
    // class whose field initializers have not run. `base.M()` is non-dispatching
    // and exempt (caller passes viaBase=true). Returns true iff it reported (so
    // the caller skips the rule-1 gate for the same site).
    bool checkVirtualDispatch(Symbol* calleeSym, bool viaBase,
                              const SourceLocation& loc,
                              std::string_view mname) {
        if (viaBase || classSealedNonAbstract_) return false;
        bool virt = false;
        if (auto* ms = dynamic_cast<MethodSymbol*>(calleeSym))
            virt = methodIsVirtualFamily(*ms);
        else if (auto* ps = dynamic_cast<PropertySymbol*>(calleeSym))
            virt = propIsVirtualFamily(*ps);
        if (!virt) return false;
        ctx_.diagnostics().report(
            Severity::Error, loc,
            "cannot call `virtual`/`override`/`abstract` member `" +
            std::string(mname) +
            "` on `this` from a constructor body of a non-sealed class — "
            "dispatch could reach a derived override over not-yet-initialized "
            "fields (§22.7, v0.184); make the class `sealed` (and not "
            "`abstract`), or call through `base.`");
        return true;
    }

    // True when `e` targets a field or auto-property of `this` (`this.f` or a
    // bare `f`) — a direct store that is NOT an observation and that counts
    // toward the §22.7 rule-1 gate (§22.7: direct field and auto-property
    // assignments keep their definite-assignment rules and the assignment
    // counts toward the gate). A computed (block/expression-bodied) property
    // target is excluded here — its setter is user code, an observation gated
    // elsewhere.
    bool isThisFieldOrAutoPropTarget(const ast::Expr& e, std::string_view& out) {
        Symbol* sym = nullptr;
        std::string_view name;
        if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
            if (!ma->target || !isNamedIdent(*ma->target, "this")) return false;
            sym = ctx_.exprSymbol(*ma);
            name = ma->member;
        } else if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(&e)) {
            sym = ctx_.exprSymbol(*id);
            name = id->name;
        } else {
            return false;
        }
        if (dynamic_cast<FieldSymbol*>(sym)) {
            out = name;
            return true;
        }
        if (auto* ps = dynamic_cast<PropertySymbol*>(sym))
            if (!ps->isIndexer && isAutoPropertySymbol(ps)) {
                out = name;
                return true;
            }
        return false;
    }

    // §22.7 rule 1: a `this`-receiver call in a constructor body is an
    // observation. Detects the two receiver-`this` call shapes (`this.M(...)`/
    // `base.M(...)` and the implicit-`this` bare `M(...)`), applies rule 2
    // (virtual dispatch) then, unless rule 2 fired, the rule-1 gate, and visits
    // the arguments for their own observations and local DA. Returns true iff
    // the call was a receiver-`this` call it handled (so the caller returns).
    bool visitCtorCall(const ast::CallExpr& ce) {
        if (!ctorMode_ || !ce.callee) return false;
        bool viaThis = false, viaBase = false;
        std::string_view mname;
        if (auto* ma =
                dynamic_cast<const ast::MemberAccessExpr*>(ce.callee.get())) {
            if (ma->target && isNamedIdent(*ma->target, "this")) {
                viaThis = true;
                mname = ma->member;
            } else if (ma->target && isNamedIdent(*ma->target, "base")) {
                viaBase = true;
                mname = ma->member;
            }
        } else if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(
                       ce.callee.get())) {
            // A bare call to a non-static instance method is an implicit-`this`
            // call (an observation). `this(...)`/`base(...)` chaining calls are
            // handled in runConstructor and never reach here as body statements.
            if (id->name != "this" && id->name != "base") {
                if (auto* ms =
                        dynamic_cast<MethodSymbol*>(ctx_.exprSymbol(*id))) {
                    bool isStatic = false;
                    for (auto m : ms->modifiers)
                        if (m == "static") isStatic = true;
                    if (!isStatic) {
                        viaThis = true;
                        mname = id->name;
                    }
                }
            }
        }
        if (!viaThis && !viaBase) return false;
        Symbol* calleeSym = ctx_.exprSymbol(*ce.callee);
        bool reported =
            checkVirtualDispatch(calleeSym, viaBase, ce.span.begin, mname);
        if (!reported)
            gateObservation(ce.span.begin,
                            "instance call `" + std::string(mname) + "`");
        // Visit arguments for their own observations / DA / lazy tracking —
        // but not the callee (its `this`/`base` receiver is this observation,
        // already accounted for).
        for (const auto& a : ce.args) visitCallArg(a);
        return true;
    }

    // §22.7 rule 1: a read of a member of `this`/`base` in a constructor body.
    // A field or auto-property read is backing storage — NOT an observation
    // (its own §7.9/§28.8.1 DA rules apply). A computed (block/expression-
    // bodied) property read is an accessor invocation — an observation (rule 1,
    // and rule 2 if virtual). Returns true iff `e` was a `this`/`base` member
    // access it handled (so the caller does not recurse into the receiver and
    // wrongly flag it as a bare-`this` observation).
    bool visitCtorMemberAccess(const ast::MemberAccessExpr& ma) {
        if (!ctorMode_ || !ma.target) return false;
        bool viaThis = isNamedIdent(*ma.target, "this");
        bool viaBase = isNamedIdent(*ma.target, "base");
        if (!viaThis && !viaBase) return false;
        if (auto* ps = dynamic_cast<PropertySymbol*>(ctx_.exprSymbol(ma))) {
            if (!ps->isIndexer && !isAutoPropertySymbol(ps)) {
                bool reported = checkVirtualDispatch(ps, viaBase, ma.span.begin,
                                                     ma.member);
                if (!reported)
                    gateObservation(ma.span.begin,
                                    "property `" + std::string(ma.member) + "`");
            }
        }
        // Field / auto-property / method-group / unresolved: not an
        // observation; do not recurse into the `this`/`base` receiver.
        return true;
    }
};

// `enclosing` is the type whose members are being walked (null for extension
// members and top-level free code). §22.7 construction-time `this`-observation
// checking runs only for a CLASS's non-static constructors — the same scope as
// the O9 field-DA obligation (Ownership.cpp): records/interfaces/enums have no
// base-first-construction/virtual-dispatch footgun, so their constructors get
// the ordinary local-DA walk unchanged.
void checkMember(const ast::Member& m, SemaContext& ctx,
                 const ast::TypeDecl* enclosing) {
    if (auto* md = dynamic_cast<const ast::MethodDecl*>(&m)) {
        if (md->bodyKind == ast::MethodBodyKind::Block) {
            MethodDA da(ctx);
            da.run(md->params, md->stmts);
        }
    } else if (auto* cd = dynamic_cast<const ast::ConstructorDecl*>(&m)) {
        MethodDA da(ctx);
        if (!cd->isStatic && enclosing &&
            enclosing->kind == ast::TypeDeclKind::Class) {
            da.runConstructor(*cd, *enclosing);  // §22.7 observation checking
        } else {
            da.run(cd->params, cd->body);
        }
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
                if (innerM) checkMember(*innerM, ctx, nt->decl.get());
            }
        }
    }
}

void checkTopDecl(const ast::TopDecl& d, SemaContext& ctx) {
    if (auto* td = dynamic_cast<const ast::TypeDecl*>(&d)) {
        for (const auto& m : td->members) {
            if (m) checkMember(*m, ctx, td);
        }
    } else if (auto* ext = dynamic_cast<const ast::ExtensionDecl*>(&d)) {
        for (const auto& m : ext->members) {
            if (m) checkMember(*m, ctx, /*enclosing=*/nullptr);
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
