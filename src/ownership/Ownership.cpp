#include "ownership/Ownership.hpp"
#include "ownership/Ownership_impl.hpp"

#include <string>
#include <unordered_map>
#include <unordered_set>

#include "ast/Node.hpp"
#include "ownership/OwnershipState.hpp"
#include "sema/SemaContext.hpp"
#include "sema/Symbol.hpp"

namespace bezel::ownership {

// The MethodOwnership class is declared in Ownership_impl.hpp; its move /
// borrow / discharge check definitions live in Ownership_Checks.cpp. This
// file holds the statement/flow-walking definitions plus the public
// checkOwnership entry point and its supporting free functions.

void MethodOwnership::run(const std::vector<std::unique_ptr<ast::Stmt>>& body,
         const std::vector<sema::Symbol*>* paramSyms) {
    if (paramSyms) {
        for (sema::Symbol* sym : *paramSyms) {
            auto* ps = dynamic_cast<sema::ParameterSymbol*>(sym);
            if (!ps) continue;
            if (ps->modifier == "owned" ||
                ps->modifier == "out owned") {
                LocalOwnership lo;
                lo.state = OwnershipState::Owned;
                lo.implicitDischarge = false;
                lo.obligationOrigin = ps->decl;
                states_[sym] = lo;
            }
        }
    }
    for (const auto& s : body) {
        if (s) visitStmt(*s);
    }
    // Slice O11 (simple): if the body's last statement is an
    // unconditional terminator that does not reach scope exit on
    // any normal path (throw or — for a method whose return type
    // is `()` — return), the §28.3 "exception-path discharge" rule
    // covers any still-undischarged owned locals via the implicit
    // try/finally during stack unwinding. Skip the undischarged
    // check in that case.
    //
    // The check is necessarily approximate without a CFG: we only
    // honor it for the lexically-last statement being a ThrowStmt.
    // A throw inside an if-branch doesn't suppress the check (the
    // other branches still normally exit). Future ownership CFG
    // refactor sharpens this to "every normal-exit path ends in
    // throw/return".
    // Scope-exit: every plain `var` owned local must have been
    // discharged. `using var` locals carry implicit discharge.
    // O11 (simple) suppresses *this* check when the body's last
    // statement is throw — exception unwinding's implicit
    // try/finally handles the discharge. The field-move check
    // below is NOT suppressed: the enclosing object survives the
    // throw and its field invariant must still hold.
    if (!bodyEndsInThrow(body)) {
        for (const auto& [sym, st] : states_) {
            if (st.state == OwnershipState::Owned && !st.implicitDischarge) {
                ctx_.diagnostics().report(
                    Severity::Error,
                    st.obligationOrigin ? st.obligationOrigin->span.begin
                                        : SourceLocation{},
                    std::string("owned value in `") +
                    std::string(sym->name) +
                    "` would be dropped here without disposal "
                    "(§28.3 — consider `using var`, explicit "
                    "`.Dispose()`, `move`, or `return`)");
            }
        }
    }
    // Slice O8 / §28.8.1: a `move this.field` leaves the field in
    // moved-from state. The field must be reassigned before the
    // surrounding method exits — otherwise the field's invariant
    // is broken on method return.
    for (const auto& [name, moved] : fieldMovedFrom_) {
        if (moved) {
            ctx_.diagnostics().report(
                Severity::Error,
                fieldMoveLoc_.count(name)
                    ? fieldMoveLoc_[name]
                    : SourceLocation{},
                std::string("field `") + std::string(name) +
                "` left in moved-from state at method exit "
                "(§28.8.1 — reassign the field before exiting)");
        }
    }
}

bool MethodOwnership::bodyEndsInThrow(
    const std::vector<std::unique_ptr<ast::Stmt>>& body) {
    for (auto it = body.rbegin(); it != body.rend(); ++it) {
        if (!*it) continue;
        return dynamic_cast<const ast::ThrowStmt*>(it->get()) != nullptr;
    }
    return false;
}

void MethodOwnership::visitStmt(const ast::Stmt& s) {
    if (auto* ld = dynamic_cast<const ast::LocalDecl*>(&s)) {
        handleLocalDecl(*ld);
        return;
    }
    if (auto* es = dynamic_cast<const ast::ExpressionStmt*>(&s)) {
        if (es->expr) handleDischarge(*es->expr);
        return;
    }
    if (auto* ds = dynamic_cast<const ast::DiscardStmt*>(&s)) {
        if (ds->value) handleDischarge(*ds->value);
        return;
    }
    if (auto* as = dynamic_cast<const ast::AssignmentStmt*>(&s)) {
        handleAssignment(*as);
        return;
    }
    if (auto* rs = dynamic_cast<const ast::ReturnStmt*>(&s)) {
        if (rs->value) handleReturn(*rs->value);
        return;
    }
    // Slice O4: if-then-else handling with branch-join semantics
    // per §28.3. Each branch is walked with a snapshot of the
    // pre-branch state; the post-branch states are joined into the
    // current state. Owned/Borrowed mismatch at the join is a
    // diagnostic.
    if (auto* ifs = dynamic_cast<const ast::IfStmt*>(&s)) {
        // Snapshot the state entering the if-statement.
        auto entry = states_;
        // Walk the if-branch.
        for (const auto& bs : ifs->ifBranch.body) if (bs) visitStmt(*bs);
        auto branchEndStates = std::vector{states_};
        // For each elif: restore entry, walk, snapshot.
        for (const auto& el : ifs->elifBranches) {
            states_ = entry;
            for (const auto& bs : el.body) if (bs) visitStmt(*bs);
            branchEndStates.push_back(states_);
        }
        // Else branch (or implicit fall-through == entry).
        if (ifs->hasElse) {
            states_ = entry;
            for (const auto& bs : ifs->elseBody) if (bs) visitStmt(*bs);
            branchEndStates.push_back(states_);
        } else {
            // No else: the false edge falls through with entry state.
            branchEndStates.push_back(entry);
        }
        // Join all branches. Start from the first; reduce in pairs.
        auto joined = branchEndStates.front();
        for (std::size_t i = 1; i < branchEndStates.size(); ++i) {
            joinInto(joined, branchEndStates[i], ifs->span.begin);
        }
        states_ = std::move(joined);
        return;
    }
    if (auto* ws = dynamic_cast<const ast::WhileStmt*>(&s)) {
        for (const auto& bs : ws->body) if (bs) visitStmt(*bs);
        return;
    }
    if (auto* fs = dynamic_cast<const ast::ForStmt*>(&s)) {
        for (const auto& bs : fs->body) if (bs) visitStmt(*bs);
        return;
    }
    if (auto* dob = dynamic_cast<const ast::DoEndBlock*>(&s)) {
        for (const auto& bs : dob->body) if (bs) visitStmt(*bs);
        return;
    }
    if (auto* ts = dynamic_cast<const ast::TryStmt*>(&s)) {
        for (const auto& bs : ts->body) if (bs) visitStmt(*bs);
        for (const auto& arm : ts->catches) {
            if (arm.body) visitStmt(*arm.body);
        }
        for (const auto& bs : ts->finallyBody) if (bs) visitStmt(*bs);
        return;
    }
}

OwnershipState MethodOwnership::producerState(const ast::Expr& e) {
    // `new T(...)` is the canonical producer of Owned values — but only
    // for reference types (classes). §9: a record is a value type with
    // no disposal obligation (§9.7 forbids record destructors and owned
    // fields), so `new Record(...)` yields a plain value, not an owned
    // resource. (Enums and other value kinds likewise.)
    if (auto* ne = dynamic_cast<const ast::NewExpr*>(&e)) {
        if (ne->type) {
            if (auto* rt = ctx_.resolvedType(*ne->type)) {
                if (rt->kind == sema::TypeRef::Kind::Named &&
                    rt->declaration &&
                    rt->declaration->kind != ast::TypeDeclKind::Class &&
                    rt->declaration->kind != ast::TypeDeclKind::Interface) {
                    return OwnershipState::Borrowed;  // value type: no obligation
                }
            }
        }
        return OwnershipState::Owned;
    }
    // `null` is the polymorphic-null state (§28.3 null-literal row).
    if (dynamic_cast<const ast::NullLiteralExpr*>(&e)) {
        return OwnershipState::PolyNull;
    }
    // Slice O13: a call to a method with an `owned T` return type
    // produces an Owned value. The callee MethodSymbol resolution
    // comes from sema; we look at its MethodDecl::returnOwned to
    // decide. A null callee symbol (overload not yet pickable, or
    // a call on a value with no symbol) falls through to Borrowed.
    if (auto* ce = dynamic_cast<const ast::CallExpr*>(&e)) {
        if (ce->callee) {
            if (auto* sym = ctx_.exprSymbol(*ce->callee)) {
                if (auto* ms = dynamic_cast<sema::MethodSymbol*>(sym)) {
                    auto* md = dynamic_cast<const ast::MethodDecl*>(
                        ms->decl);
                    if (md && md->returnOwned) {
                        return OwnershipState::Owned;
                    }
                }
            }
        }
    }
    // Default: Borrowed.
    return OwnershipState::Borrowed;
}

void MethodOwnership::handleLocalDecl(const ast::LocalDecl& ld) {
    // Find the LocalSymbol that ExprTypecheck created for this decl.
    sema::LocalSymbol* lsym = findLocalSymbol(ld);
    if (!lsym) return;
    LocalOwnership lo;
    lo.obligationOrigin = &ld;
    if (ld.isUsing) {
        // `using var` is always Owned and implicitly discharged.
        lo.state = OwnershipState::Owned;
        lo.implicitDischarge = true;
    } else if (ld.initializer) {
        lo.state = producerState(*ld.initializer);
    } else {
        lo.state = OwnershipState::Uninitialized;
    }
    states_[lsym] = lo;
    // Slice O8: if the initializer involves `move this.field`, mark
    // the field as moved-from (the move side-effect happens at
    // initializer evaluation time, regardless of the local's `using`
    // status). This lets `using var old := move this._log;` correctly
    // record that `_log` is now empty.
    if (ld.initializer) {
        recordMovesIn(*ld.initializer);
    }
}

void MethodOwnership::handleAssignment(const ast::AssignmentStmt& as) {
    // Slice O4: `x := EXPR` where x is a tracked local updates x's
    // state to match the producer of EXPR.
    if (as.op != ":=") return;
    if (!as.target || !as.value) return;
    // Slice O15: side-effects in the RHS — `x := move y` should
    // transition y to MovedFrom. recordMovesIn handles `move local`,
    // `move this.field`, and `f(owned z)` in any position.
    recordMovesIn(*as.value);
    if (const auto* id =
            dynamic_cast<const ast::IdentifierExpr*>(as.target.get())) {
        for (auto& [sym, st] : states_) {
            if (sym->name != id->name) continue;
            st.state = producerState(*as.value);
            st.obligationOrigin = &as;
            return;
        }
        return;
    }
    // Slice O8: `this.field := value` clears the moved-from mark
    // on the field. (We track only `this.field` shape — `other.f`
    // and indexer-set are sema's broader concern.)
    if (const auto* ma =
            dynamic_cast<const ast::MemberAccessExpr*>(as.target.get())) {
        if (const auto* tid =
                dynamic_cast<const ast::IdentifierExpr*>(
                    ma->target.get())) {
            if (tid->name == "this") {
                fieldMovedFrom_[ma->member] = false;
            }
        }
    }
}

void MethodOwnership::handleReturn(const ast::Expr& e) {
    // For slice O1, treat `return x;` (bare identifier) as a
    // discharge. The "is the method's return type owned T?" check
    // is sema's; here we just satisfy the discharge requirement
    // pessimistically.
    if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(&e)) {
        markDisposedByName(id->name, id->span.begin);
        return;
    }
    // Slice O7: borrow-escape check. `return localRef.M(...)` /
    // `return localRef.f` returns a borrow whose source is the
    // local. If the local is a `using var` (scope-bound), the
    // borrow outlives its source — error per §28.3.
    checkBorrowEscapesOnReturn(e);
    // For `move x` return arm: covered by handleDischarge inside
    // the expression — but we also call it here.
    handleDischarge(e);
}

namespace {

// Top-level walker — visits every method/ctor body in the source.
void checkTopDecl(const ast::TopDecl& d, sema::SemaContext& ctx);

// Find the sema symbol that wraps a given member AST and return its
// param-list, or nullptr if the member doesn't have one (or wasn't
// matched by symbol collection).
const std::vector<sema::Symbol*>* paramSymsOf(
    const ast::Member& m, sema::SemaContext& ctx) {
    for (const auto& s : ctx.symbols()) {
        if (s->decl != &m) continue;
        if (auto* ms = dynamic_cast<sema::MethodSymbol*>(s.get()))
            return &ms->params;
        if (auto* cs = dynamic_cast<sema::ConstructorSymbol*>(s.get()))
            return &cs->params;
    }
    return nullptr;
}

void checkMember(const ast::Member& m, sema::SemaContext& ctx) {
    if (auto* md = dynamic_cast<const ast::MethodDecl*>(&m)) {
        if (md->bodyKind == ast::MethodBodyKind::Block) {
            MethodOwnership mo(ctx);
            mo.run(md->stmts, paramSymsOf(m, ctx));
        }
    } else if (auto* cd = dynamic_cast<const ast::ConstructorDecl*>(&m)) {
        MethodOwnership mo(ctx);
        mo.run(cd->body, paramSymsOf(m, ctx));
    } else if (auto* dd = dynamic_cast<const ast::DestructorDecl*>(&m)) {
        MethodOwnership mo(ctx);
        mo.run(dd->body);
    } else if (auto* od = dynamic_cast<const ast::OperatorDecl*>(&m)) {
        if (od->bodyKind == ast::MethodBodyKind::Block) {
            MethodOwnership mo(ctx);
            mo.run(od->stmts);
        }
    } else if (auto* nt = dynamic_cast<const ast::NestedTypeMember*>(&m)) {
        if (nt->decl) {
            for (const auto& innerM : nt->decl->members) {
                if (innerM) checkMember(*innerM, ctx);
            }
        }
    }
    // PropertyDecl accessors and other forms join in later O slices.
}

// ---------- Slice O9: owned-field definite-assignment (§7.9) --------
//
// Per §7.9 / Appendix C.1, every non-nullable field — which owned
// class-typed fields are — must be assigned by a field initializer or
// in every constructor. This slice enforces that for `owned` fields
// specifically. A `this(...)` chained constructor transfers the
// responsibility to the chained target. `base(...)` chaining does not
// — the derived class's fields are still the derived ctor's job.
//
// This is a syntactic over-approximation: we accept any
// `this.<field> := …` assignment anywhere in the body (not necessarily
// on every CFG path). The full DA check awaits the CFG-based ownership
// refactor; for now a constructor that initializes only on some paths
// passes here but would fail under the stricter pass once landed.

// §7.9: a field of non-nullable *reference* type (class, interface,
// string, function, non-nullable Object) has no default value and must
// be assigned by a field initializer or in every constructor. Value
// types (records, enums, primitives other than string/Object, inline
// arrays, unit) have well-defined defaults and are exempt. A static
// field is exempt here (static-init rules, §22.13). Generic-parameter
// and unresolved types are conservatively treated as non-reference
// (not flagged). `owned` fields are a subset (class-typed) and are
// covered by this rule.
bool isNonNullableReferenceField(const ast::FieldDecl& f,
                                 sema::SemaContext& ctx) {
    for (auto m : f.modifiers) {
        if (m == "static") return false;
    }
    if (!f.type) return false;
    auto* rt = ctx.resolvedType(*f.type);
    if (!rt || rt->nullable) return false;
    switch (rt->kind) {
        case sema::TypeRef::Kind::Primitive:
            return rt->primitive == sema::PrimitiveType::String ||
                   rt->primitive == sema::PrimitiveType::Object;
        case sema::TypeRef::Kind::Function:
            return true;
        case sema::TypeRef::Kind::Named:
            if (!rt->declaration) return false;  // built-in/generic — skip
            return rt->declaration->kind == ast::TypeDeclKind::Class ||
                   rt->declaration->kind == ast::TypeDeclKind::Interface;
        default:
            return false;  // Unit, InlineArray, Error → value/unknown
    }
}

// True if `e` assigns a field of the current instance — either
// `this.<member>` (explicit) or a bare `<field>` that resolves to a field
// of the declaring class (implicit `this`). §7.9 requires the field to be
// "assigned"; it does not mandate `this.` qualification, and the bare form
// resolves to the field in sema and codegen alike.
bool isDirectThisFieldRef(const ast::Expr& e, std::string_view& outName,
                          sema::SemaContext& ctx) {
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
        auto* id = dynamic_cast<const ast::IdentifierExpr*>(ma->target.get());
        if (!id || id->name != "this") return false;
        outName = ma->member;
        return true;
    }
    if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(&e)) {
        if (auto* fs =
                dynamic_cast<sema::FieldSymbol*>(ctx.exprSymbol(*id))) {
            outName = fs->name;
            return true;
        }
    }
    return false;
}

// Returns true if the constructor body's first statement is a
// `this(...)` call (chained-ctor delegation). Per spec the chained
// target carries the field-initialization burden.
bool ctorChainsToThis(const ast::ConstructorDecl& cd) {
    if (cd.body.empty() || !cd.body.front()) return false;
    auto* es = dynamic_cast<const ast::ExpressionStmt*>(cd.body.front().get());
    if (!es || !es->expr) return false;
    auto* call = dynamic_cast<const ast::CallExpr*>(es->expr.get());
    if (!call || !call->callee) return false;
    auto* id = dynamic_cast<const ast::IdentifierExpr*>(call->callee.get());
    return id && id->name == "this";
}

// Walk the body collecting every owned-field name that gets an
// outright assignment `this.<f> := …` somewhere in it. This walks
// recursively into control-flow blocks; over-approximates by counting
// any path-assignment as definitely-assigned.
void collectThisFieldAssignments(
    const std::vector<std::unique_ptr<ast::Stmt>>& body,
    std::unordered_set<std::string_view>& assigned, sema::SemaContext& ctx);

void collectThisFieldAssignmentsStmt(
    const ast::Stmt& s,
    std::unordered_set<std::string_view>& assigned, sema::SemaContext& ctx) {
    if (auto* as = dynamic_cast<const ast::AssignmentStmt*>(&s)) {
        if (as->op == ":=" && as->target) {
            std::string_view name;
            if (isDirectThisFieldRef(*as->target, name, ctx)) {
                assigned.insert(name);
            }
        }
        return;
    }
    if (auto* ifs = dynamic_cast<const ast::IfStmt*>(&s)) {
        collectThisFieldAssignments(ifs->ifBranch.body, assigned, ctx);
        for (const auto& el : ifs->elifBranches)
            collectThisFieldAssignments(el.body, assigned, ctx);
        collectThisFieldAssignments(ifs->elseBody, assigned, ctx);
        return;
    }
    if (auto* ws = dynamic_cast<const ast::WhileStmt*>(&s)) {
        collectThisFieldAssignments(ws->body, assigned, ctx);
        return;
    }
    if (auto* fs = dynamic_cast<const ast::ForStmt*>(&s)) {
        collectThisFieldAssignments(fs->body, assigned, ctx);
        return;
    }
    if (auto* dob = dynamic_cast<const ast::DoEndBlock*>(&s)) {
        collectThisFieldAssignments(dob->body, assigned, ctx);
        return;
    }
    if (auto* ts = dynamic_cast<const ast::TryStmt*>(&s)) {
        collectThisFieldAssignments(ts->body, assigned, ctx);
        for (const auto& arm : ts->catches) {
            if (arm.body)
                collectThisFieldAssignmentsStmt(*arm.body, assigned, ctx);
        }
        collectThisFieldAssignments(ts->finallyBody, assigned, ctx);
        return;
    }
}

void collectThisFieldAssignments(
    const std::vector<std::unique_ptr<ast::Stmt>>& body,
    std::unordered_set<std::string_view>& assigned, sema::SemaContext& ctx) {
    for (const auto& s : body) {
        if (s) collectThisFieldAssignmentsStmt(*s, assigned, ctx);
    }
}

void checkOwnedFieldDA(const ast::TypeDecl& td, sema::SemaContext& ctx) {
    // Gather non-nullable reference fields without initializers (§7.9).
    // Owned fields are a subset.
    std::vector<const ast::FieldDecl*> ownedNoInit;
    for (const auto& m : td.members) {
        auto* f = dynamic_cast<const ast::FieldDecl*>(m.get());
        if (!f) continue;
        if (!isNonNullableReferenceField(*f, ctx)) continue;
        if (f->initializer) continue;
        ownedNoInit.push_back(f);
    }
    if (ownedNoInit.empty()) return;
    // Collect ctors.
    std::vector<const ast::ConstructorDecl*> ctors;
    for (const auto& m : td.members) {
        if (auto* cd = dynamic_cast<const ast::ConstructorDecl*>(m.get())) {
            if (!cd->isStatic) ctors.push_back(cd);
        }
    }
    // No ctors at all: each owned-without-init field is reported at
    // its declaration site as needing initialization.
    if (ctors.empty()) {
        for (auto* f : ownedNoInit) {
            ctx.diagnostics().report(
                Severity::Error, f->span.begin,
                std::string("non-nullable field `") + std::string(f->name) +
                "` needs an initializer or a constructor that assigns "
                "it (§7.9)");
        }
        return;
    }
    // Per-ctor: walk body and check assignments cover ownedNoInit.
    for (auto* cd : ctors) {
        if (ctorChainsToThis(*cd)) continue; // delegated
        std::unordered_set<std::string_view> assigned;
        collectThisFieldAssignments(cd->body, assigned, ctx);
        for (auto* f : ownedNoInit) {
            if (!assigned.count(f->name)) {
                ctx.diagnostics().report(
                    Severity::Error, cd->span.begin,
                    std::string("constructor does not initialize "
                                "non-nullable field `") +
                    std::string(f->name) +
                    "` (§7.9 — every constructor's normal-exit path "
                    "must assign it, or chain to `this(…)`)");
            }
        }
    }
}

void checkTopDecl(const ast::TopDecl& d, sema::SemaContext& ctx) {
    if (auto* td = dynamic_cast<const ast::TypeDecl*>(&d)) {
        // O9: per-class owned-field DA check (classes only).
        if (td->kind == ast::TypeDeclKind::Class) {
            checkOwnedFieldDA(*td, ctx);
        }
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

bool checkOwnership(const ast::SourceFile& src, sema::SemaContext& ctx) {
    for (const auto& d : src.topDecls) {
        if (d) checkTopDecl(*d, ctx);
    }
    return !ctx.diagnostics().hasErrors();
}

} // namespace bezel::ownership
