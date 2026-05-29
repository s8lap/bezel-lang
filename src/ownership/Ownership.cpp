#include "ownership/Ownership.hpp"

#include <string>
#include <unordered_map>
#include <unordered_set>

#include "ast/Node.hpp"
#include "ownership/OwnershipState.hpp"
#include "sema/SemaContext.hpp"
#include "sema/Symbol.hpp"

namespace bezel::ownership {

namespace {

// Per-method ownership analyzer (slice O1). Walks statements linearly
// and tracks each LocalDecl's ownership state via its LocalSymbol.
class MethodOwnership {
public:
    MethodOwnership(sema::SemaContext& ctx) : ctx_(ctx) {}

    // Run the linear walk. `body` is the method/ctor/lambda body's
    // statement list. `paramSyms` (slice O10) is the matching list of
    // ParameterSymbols — `owned` params are introduced as Owned locals
    // at method entry per §28.8 and must reach a discharge point on
    // every normal-exit path (same rule as `var x := producer-of-owned;`).
    void run(const std::vector<std::unique_ptr<ast::Stmt>>& body,
             const std::vector<sema::Symbol*>* paramSyms = nullptr) {
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
        // Scope-exit: every plain `var` owned local must have been
        // discharged. `using var` locals carry implicit discharge.
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

private:
    sema::SemaContext& ctx_;
    std::unordered_map<sema::Symbol*, LocalOwnership> states_;

    void visitStmt(const ast::Stmt& s) {
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

    // Categorize the initializer expression to determine the local's
    // initial ownership state (slice O1 producer table — minimal).
    OwnershipState producerState(const ast::Expr& e) {
        // `new T(...)` is the canonical producer of Owned values.
        if (dynamic_cast<const ast::NewExpr*>(&e)) {
            return OwnershipState::Owned;
        }
        // `null` is the polymorphic-null state (§28.3 null-literal row).
        if (dynamic_cast<const ast::NullLiteralExpr*>(&e)) {
            return OwnershipState::PolyNull;
        }
        // Default: Borrowed (slice O1 doesn't dig deeper — sema later
        // refines by querying method-signature owned-return annotations).
        return OwnershipState::Borrowed;
    }

    void handleLocalDecl(const ast::LocalDecl& ld) {
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
    }

    // Detect `local.Dispose()` patterns and mark the local as Disposed.
    // Also detects `local` consumed as a `return` value (handleReturn
    // path) and `move local` (UnaryExpr op=move on bare identifier).
    //
    // Slice O2: before marking, check the current state — re-disposing
    // or re-moving an already-Disposed/MovedFrom local is a diagnostic
    // (the §28.3 "read of moved-from / disposed" rules).
    void handleDischarge(const ast::Expr& e) {
        if (auto* call = dynamic_cast<const ast::CallExpr*>(&e)) {
            // Walk arguments first — they're reads.
            if (call->callee) checkRead(*call->callee, /*selfTarget=*/true);
            for (const auto& a : call->args) {
                if (a.value) checkRead(*a.value, /*selfTarget=*/false);
            }
            if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(
                    call->callee.get())) {
                if (ma->member == "Dispose") {
                    if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(
                            ma->target.get())) {
                        markDisposedByName(id->name, id->span.begin);
                    }
                }
            }
            return;
        }
        if (auto* un = dynamic_cast<const ast::UnaryExpr*>(&e)) {
            if (un->op == "move") {
                if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(
                        un->operand.get())) {
                    markMovedByName(id->name, id->span.begin);
                    return; // don't re-check the now-moved-from operand
                }
            }
            // Other unary ops — sub-expression is a regular read.
            if (un->operand) checkRead(*un->operand, /*selfTarget=*/false);
            return;
        }
        // Fallback: just check the expression as a read.
        checkRead(e, /*selfTarget=*/false);
    }

    // Recursively descend an expression and, at each IdentifierExpr that
    // names a tracked local, verify the local's ownership state is
    // readable. `selfTarget=true` for the `x` in `x.Dispose()` — we
    // tolerate that as the discharge use site; the post-marking handles
    // it. For everything else, MovedFrom / Disposed is a diagnostic.
    void checkRead(const ast::Expr& e, bool selfTarget) {
        if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(&e)) {
            if (selfTarget) return;  // tolerated for discharge target
            auto* st = lookupState(id->name);
            if (!st) return;
            if (st->state == OwnershipState::MovedFrom) {
                ctx_.diagnostics().report(
                    Severity::Error, id->span.begin,
                    std::string("owned value in `") + std::string(id->name) +
                    "` was already moved out at this point (§28.3)");
                st->state = OwnershipState::Poisoned;
            } else if (st->state == OwnershipState::Disposed) {
                ctx_.diagnostics().report(
                    Severity::Error, id->span.begin,
                    std::string("read of disposed local `") +
                    std::string(id->name) + "` (§28.3)");
                st->state = OwnershipState::Poisoned;
            }
            return;
        }
        if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
            if (ma->target) checkRead(*ma->target, /*selfTarget=*/false);
            return;
        }
        if (auto* be = dynamic_cast<const ast::BinaryExpr*>(&e)) {
            if (be->left) checkRead(*be->left, false);
            if (be->right) checkRead(*be->right, false);
            return;
        }
        if (auto* call = dynamic_cast<const ast::CallExpr*>(&e)) {
            // Treat the callee as a member-access-target for discharge
            // tolerance (so `x.M(x)` doesn't flag x).
            if (call->callee) checkRead(*call->callee, true);
            for (const auto& a : call->args) {
                if (a.value) checkRead(*a.value, false);
            }
            return;
        }
        if (auto* un = dynamic_cast<const ast::UnaryExpr*>(&e)) {
            if (un->operand) checkRead(*un->operand, false);
            return;
        }
        // Other expression forms — not exhaustive; later slices extend.
    }

    void handleAssignment(const ast::AssignmentStmt& as) {
        // Slice O4: `x := EXPR` where x is a tracked local updates x's
        // state to match the producer of EXPR. Other assignment shapes
        // (`x.f := …`, `x[i] := …`) are handled in later slices.
        if (as.op != ":=") return;
        if (!as.target || !as.value) return;
        const auto* id =
            dynamic_cast<const ast::IdentifierExpr*>(as.target.get());
        if (!id) return;
        for (auto& [sym, st] : states_) {
            if (sym->name != id->name) continue;
            st.state = producerState(*as.value);
            st.obligationOrigin = &as;
            return;
        }
    }

    void handleReturn(const ast::Expr& e) {
        // For slice O1, treat `return x;` (bare identifier) as a
        // discharge. The "is the method's return type owned T?" check
        // is sema's; here we just satisfy the discharge requirement
        // pessimistically.
        if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(&e)) {
            markDisposedByName(id->name, id->span.begin);
            return;
        }
        // For `move x` return arm: covered by handleDischarge inside
        // the expression — but we also call it here.
        handleDischarge(e);
    }

    // §28.3 join. Per the spec the join is asymmetric: matching states
    // carry forward (with their auxiliary data, e.g. Owned's discharge
    // obligation), but mismatched owned-vs-borrowed is rejected (the
    // spec's "silently demoting an owned obligation to a borrowed state
    // at the join would leak the resource"). Mutates `into` in place.
    void joinInto(std::unordered_map<sema::Symbol*, LocalOwnership>& into,
                  const std::unordered_map<sema::Symbol*,
                                           LocalOwnership>& other,
                  SourceLocation joinLoc) {
        for (auto& [sym, st] : into) {
            auto it = other.find(sym);
            if (it == other.end()) continue;
            const LocalOwnership& o = it->second;
            if (st.state == o.state) continue;
            // PolyNull absorbs: PolyNull ⊓ X = X.
            if (st.state == OwnershipState::PolyNull) {
                st = o;
                continue;
            }
            if (o.state == OwnershipState::PolyNull) continue;
            // Owned vs Borrowed is the spec-rejected case.
            const bool ownedBorrowedClash =
                (st.state == OwnershipState::Owned &&
                 o.state == OwnershipState::Borrowed) ||
                (st.state == OwnershipState::Borrowed &&
                 o.state == OwnershipState::Owned);
            if (ownedBorrowedClash) {
                ctx_.diagnostics().report(
                    Severity::Error, joinLoc,
                    std::string("branch ownership conflict for `") +
                    std::string(sym->name) +
                    "`: one branch produced an owned value, the other a "
                    "borrowed value (§28.3 — equalize the branches before "
                    "the merge per the two Patterns)");
                st.state = OwnershipState::Poisoned;
                continue;
            }
            // Other mismatches (Disposed/MovedFrom on one side) — poison.
            st.state = OwnershipState::Poisoned;
        }
    }

    sema::LocalSymbol* findLocalSymbol(const ast::LocalDecl& ld) {
        // Walk all collected symbols and find the LocalSymbol whose
        // decl matches. Sema slice 3b created these on demand.
        for (const auto& s : ctx_.symbols()) {
            if (auto* ls = dynamic_cast<sema::LocalSymbol*>(s.get())) {
                if (ls->decl == &ld) return ls;
            }
        }
        return nullptr;
    }

    LocalOwnership* lookupState(std::string_view name) {
        for (auto& [sym, st] : states_) {
            if (sym->name == name) return &st;
        }
        return nullptr;
    }

    void markDisposedByName(std::string_view name, SourceLocation loc) {
        auto* st = lookupState(name);
        if (!st) return;
        if (st->state == OwnershipState::Disposed) {
            ctx_.diagnostics().report(
                Severity::Error, loc,
                std::string("local `") + std::string(name) +
                "` was already disposed (§28.3 — `.Dispose()` is "
                "idempotent but re-calling it on a known-disposed local "
                "indicates a bug)");
            st->state = OwnershipState::Poisoned;
            return;
        }
        if (st->state == OwnershipState::MovedFrom) {
            ctx_.diagnostics().report(
                Severity::Error, loc,
                std::string("cannot dispose `") + std::string(name) +
                "`: it was already moved out (§28.3)");
            st->state = OwnershipState::Poisoned;
            return;
        }
        st->state = OwnershipState::Disposed;
    }

    void markMovedByName(std::string_view name, SourceLocation loc) {
        auto* st = lookupState(name);
        if (!st) return;
        if (st->state == OwnershipState::MovedFrom) {
            ctx_.diagnostics().report(
                Severity::Error, loc,
                std::string("owned value in `") + std::string(name) +
                "` was already moved out at this point (§28.3)");
            st->state = OwnershipState::Poisoned;
            return;
        }
        if (st->state == OwnershipState::Disposed) {
            ctx_.diagnostics().report(
                Severity::Error, loc,
                std::string("cannot move from disposed local `") +
                std::string(name) + "` (§28.3)");
            st->state = OwnershipState::Poisoned;
            return;
        }
        if (st->implicitDischarge) {
            ctx_.diagnostics().report(
                Severity::Error, loc,
                std::string("cannot move out of `using var` `") +
                std::string(name) +
                "`; `using var` is scope-bound (§28.3)");
            st->state = OwnershipState::Poisoned;
            return;
        }
        st->state = OwnershipState::MovedFrom;
    }
};

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

void checkTopDecl(const ast::TopDecl& d, sema::SemaContext& ctx) {
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

bool checkOwnership(const ast::SourceFile& src, sema::SemaContext& ctx) {
    for (const auto& d : src.topDecls) {
        if (d) checkTopDecl(*d, ctx);
    }
    return !ctx.diagnostics().hasErrors();
}

} // namespace bezel::ownership
