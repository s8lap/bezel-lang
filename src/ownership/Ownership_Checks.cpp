#include "ownership/Ownership_impl.hpp"

namespace bezel::ownership {

// The MethodOwnership class is declared in Ownership_impl.hpp; this
// translation unit holds the move / borrow / discharge check definitions.
// The statement/flow-walking definitions live in Ownership.cpp alongside
// the public checkOwnership entry point.

void MethodOwnership::recordMovesIn(const ast::Expr& e) {
    if (auto* un = dynamic_cast<const ast::UnaryExpr*>(&e)) {
        if (un->op == "move") {
            if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(
                    un->operand.get())) {
                markMovedByName(id->name, id->span.begin);
                return;
            }
            if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(
                    un->operand.get())) {
                if (auto* tid =
                        dynamic_cast<const ast::IdentifierExpr*>(
                            ma->target.get())) {
                    if (tid->name == "this") {
                        fieldMovedFrom_[ma->member] = true;
                        fieldMoveLoc_[ma->member] = un->span.begin;
                        return;
                    }
                }
            }
        }
        if (un->operand) recordMovesIn(*un->operand);
        return;
    }
    if (auto* be = dynamic_cast<const ast::BinaryExpr*>(&e)) {
        if (be->left) recordMovesIn(*be->left);
        if (be->right) recordMovesIn(*be->right);
        return;
    }
    if (auto* call = dynamic_cast<const ast::CallExpr*>(&e)) {
        recordMovesInArgs(call->args, call->callee.get());
        return;
    }
    // A constructor call `new T(owned x)` / `new T(move x)` consumes its
    // owned arguments exactly as a method call does — most commonly
    // `using var b := new Box(move r);`.
    if (auto* ne = dynamic_cast<const ast::NewExpr*>(&e)) {
        // §28.8: find the matching constructor to detect implicit owned params.
        // Build a synthetic check by examining the ctor's parameter list directly.
        const ast::ConstructorDecl* cd = nullptr;
        if (ne->type) {
            if (auto* rt = ctx_.resolvedType(*ne->type)) {
                if (rt->declaration) {
                    std::size_t nArgs = ne->args.size();
                    for (sema::Symbol* m : rt->declaration->members) {
                        if (auto* cs = dynamic_cast<sema::ConstructorSymbol*>(m)) {
                            auto* d = dynamic_cast<const ast::ConstructorDecl*>(cs->decl);
                            if (d && d->params.size() == nArgs) { cd = d; break; }
                        }
                    }
                }
            }
        }
        std::size_t pos = 0;
        for (const auto& a : ne->args) {
            if (!a.value) { ++pos; continue; }
            if (a.modifier == "owned") {
                if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(a.value.get())) {
                    markMovedByName(id->name, id->span.begin);
                    ++pos; continue;
                }
            }
            // §28.8 implicit transfer: bare id to `owned` formal param.
            if (a.modifier.empty() && cd && pos < cd->params.size() &&
                (cd->params[pos].modifier == "owned" ||
                 cd->params[pos].modifier == "out owned")) {
                if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(a.value.get())) {
                    markMovedByName(id->name, id->span.begin);
                    ++pos; continue;
                }
            }
            recordMovesIn(*a.value);
            ++pos;
        }
        return;
    }
}

void MethodOwnership::recordMovesInArgs(const std::vector<ast::CallArgument>& args,
                       const ast::Expr* callee) {
    // Resolve callee parameter list for implicit ownership transfer check.
    const ast::MethodDecl*     calleeMethod = nullptr;
    const ast::ConstructorDecl* calleeCtor  = nullptr;
    if (callee) {
        sema::Symbol* sym = ctx_.exprSymbol(*callee);
        if (auto* ms = dynamic_cast<sema::MethodSymbol*>(sym)) {
            calleeMethod = dynamic_cast<const ast::MethodDecl*>(ms->decl);
        } else if (auto* cs = dynamic_cast<sema::ConstructorSymbol*>(sym)) {
            calleeCtor = dynamic_cast<const ast::ConstructorDecl*>(cs->decl);
        }
    }
    std::size_t pos = 0;
    for (const auto& a : args) {
        if (!a.value) { ++pos; continue; }
        // Slice O14: `owned x` argument modifier consumes x.
        if (a.modifier == "owned") {
            if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(
                    a.value.get())) {
                markMovedByName(id->name, id->span.begin);
                ++pos; continue;
            }
        }
        // §28.8 implicit ownership transfer: a bare identifier passed
        // to an `owned` formal parameter transfers ownership without
        // an explicit `owned` or `move` at the call site.
        {
            const std::vector<ast::Param>* params = nullptr;
            if (calleeMethod)  params = &calleeMethod->params;
            if (calleeCtor)    params = &calleeCtor->params;
            if (params && pos < params->size() &&
                ((*params)[pos].modifier == "owned" ||
                 (*params)[pos].modifier == "out owned")) {
                if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(
                        a.value.get())) {
                    markMovedByName(id->name, id->span.begin);
                    ++pos; continue;
                }
            }
        }
        recordMovesIn(*a.value);
        ++pos;
    }
}

bool MethodOwnership::isImplicitOwned(const std::vector<ast::CallArgument>& args,
                     std::size_t pos, const ast::Expr* callee) {
    if (!callee) return false;
    sema::Symbol* sym = ctx_.exprSymbol(*callee);
    const ast::MethodDecl*     md = nullptr;
    const ast::ConstructorDecl* cd = nullptr;
    if (auto* ms = dynamic_cast<sema::MethodSymbol*>(sym))
        md = dynamic_cast<const ast::MethodDecl*>(ms->decl);
    else if (auto* cs = dynamic_cast<sema::ConstructorSymbol*>(sym))
        cd = dynamic_cast<const ast::ConstructorDecl*>(cs->decl);
    const std::vector<ast::Param>* params =
        md ? &md->params : (cd ? &cd->params : nullptr);
    if (!params || pos >= params->size()) return false;
    const std::string_view mod = (*params)[pos].modifier;
    return mod == "owned" || mod == "out owned";
}

void MethodOwnership::handleDischarge(const ast::Expr& e) {
    if (auto* call = dynamic_cast<const ast::CallExpr*>(&e)) {
        if (call->callee) checkRead(*call->callee, /*selfTarget=*/true);
        // Slice O14 + §28.8: argument-mode handling.
        std::size_t pos = 0;
        for (const auto& a : call->args) {
            if (!a.value) { ++pos; continue; }
            bool consumed = false;
            if (a.modifier == "owned") {
                if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(
                        a.value.get())) {
                    markMovedByName(id->name, id->span.begin);
                    consumed = true;
                }
            } else if (auto* un = dynamic_cast<const ast::UnaryExpr*>(
                           a.value.get())) {
                if (un->op == "move") {
                    if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(
                            un->operand.get())) {
                        markMovedByName(id->name, id->span.begin);
                        consumed = true;
                    } else if (auto* ma =
                                   dynamic_cast<const ast::MemberAccessExpr*>(
                                       un->operand.get())) {
                        // `M(move this.field)` — field-move at call site.
                        if (auto* tid =
                                dynamic_cast<const ast::IdentifierExpr*>(
                                    ma->target.get())) {
                            if (tid->name == "this") {
                                fieldMovedFrom_[ma->member] = true;
                                fieldMoveLoc_[ma->member] = un->span.begin;
                                consumed = true;
                            }
                        }
                    }
                }
            }
            // §28.8 implicit transfer: bare id to an `owned` formal param.
            if (!consumed && a.modifier.empty() &&
                isImplicitOwned(call->args, pos, call->callee.get())) {
                if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(
                        a.value.get())) {
                    markMovedByName(id->name, id->span.begin);
                    consumed = true;
                }
            }
            if (!consumed) checkRead(*a.value, /*selfTarget=*/false);
            ++pos;
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
            // Slice O8: `move this.field` — mark the field as
            // moved-from. The field must be reassigned before
            // method exit.
            if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(
                    un->operand.get())) {
                if (auto* tid =
                        dynamic_cast<const ast::IdentifierExpr*>(
                            ma->target.get())) {
                    if (tid->name == "this") {
                        fieldMovedFrom_[ma->member] = true;
                        fieldMoveLoc_[ma->member] = un->span.begin;
                        return;
                    }
                }
            }
        }
        // Other unary ops — sub-expression is a regular read.
        if (un->operand) checkRead(*un->operand, /*selfTarget=*/false);
        return;
    }
    // §28.8 implicit ownership transfer for constructor calls `new T(x)`.
    if (auto* ne = dynamic_cast<const ast::NewExpr*>(&e)) {
        // Find the matching constructor by arity to get its param list.
        const ast::ConstructorDecl* cd = nullptr;
        if (ne->type) {
            if (auto* rt = ctx_.resolvedType(*ne->type)) {
                if (rt->declaration) {
                    std::size_t nArgs = ne->args.size();
                    for (sema::Symbol* m : rt->declaration->members) {
                        if (auto* cs = dynamic_cast<sema::ConstructorSymbol*>(m)) {
                            auto* d = dynamic_cast<const ast::ConstructorDecl*>(cs->decl);
                            if (d && d->params.size() == nArgs) { cd = d; break; }
                        }
                    }
                }
            }
        }
        std::size_t pos = 0;
        for (const auto& a : ne->args) {
            if (!a.value) { ++pos; continue; }
            bool consumed = false;
            if (a.modifier == "owned") {
                if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(a.value.get())) {
                    markMovedByName(id->name, id->span.begin);
                    consumed = true;
                }
            } else if (auto* un = dynamic_cast<const ast::UnaryExpr*>(a.value.get())) {
                if (un->op == "move") {
                    if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(un->operand.get())) {
                        markMovedByName(id->name, id->span.begin);
                        consumed = true;
                    }
                }
            }
            // §28.8: bare id to an `owned` formal param = implicit transfer.
            if (!consumed && a.modifier.empty() && cd && pos < cd->params.size() &&
                (cd->params[pos].modifier == "owned" ||
                 cd->params[pos].modifier == "out owned")) {
                if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(a.value.get())) {
                    markMovedByName(id->name, id->span.begin);
                    consumed = true;
                }
            }
            if (!consumed) checkRead(*a.value, /*selfTarget=*/false);
            ++pos;
        }
        return;
    }
    // Fallback: just check the expression as a read.
    checkRead(e, /*selfTarget=*/false);
}

void MethodOwnership::checkRead(const ast::Expr& e, bool selfTarget) {
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

bool MethodOwnership::isBorrowableType(const sema::TypeRef* rt) {
    if (!rt) return true;            // unknown → conservative
    if (rt->nullable) return true;   // nullable reference
    switch (rt->kind) {
        case sema::TypeRef::Kind::Primitive:
            return rt->primitive == sema::PrimitiveType::String ||
                   rt->primitive == sema::PrimitiveType::Object;
        case sema::TypeRef::Kind::Function:
            return true;
        case sema::TypeRef::Kind::Named:
            if (!rt->declaration) return false;  // generic param → not conservative
            return rt->declaration->kind == ast::TypeDeclKind::Class ||
                   rt->declaration->kind == ast::TypeDeclKind::Interface;
        case sema::TypeRef::Kind::Error:
            return true;             // unresolved → conservative
        default:
            return false;            // Unit, InlineArray, enum, record
    }
}

void MethodOwnership::checkBorrowEscapesOnReturn(const ast::Expr& e) {
    const ast::IdentifierExpr* root = identifierRoot(e);
    if (!root) return;
    // A value-typed return is a copy, not a borrow of the local — it
    // cannot outlive its source (§28.3 governs borrows).
    if (!isBorrowableType(ctx_.exprType(e))) return;
    for (const auto& [sym, st] : states_) {
        if (sym->name != root->name) continue;
        if (!st.implicitDischarge) continue;  // only `using var`
        ctx_.diagnostics().report(
            Severity::Error, root->span.begin,
            std::string("borrow returned from `") +
            std::string(root->name) +
            "` would outlive its source — `using var` `" +
            std::string(root->name) +
            "` is disposed at scope end (§28.3 borrow-lifetime "
            "rule; bind the source in the caller's scope and "
            "return the borrow from there instead)");
        return;
    }
}

const ast::IdentifierExpr* MethodOwnership::identifierRoot(const ast::Expr& e) {
    if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(&e))
        return id;
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e))
        return ma->target ? identifierRoot(*ma->target) : nullptr;
    if (auto* nm = dynamic_cast<const ast::NullCondMemberExpr*>(&e))
        return nm->target ? identifierRoot(*nm->target) : nullptr;
    if (auto* ce = dynamic_cast<const ast::CallExpr*>(&e))
        return ce->callee ? identifierRoot(*ce->callee) : nullptr;
    if (auto* ix = dynamic_cast<const ast::IndexExpr*>(&e))
        return ix->target ? identifierRoot(*ix->target) : nullptr;
    return nullptr;
}

void MethodOwnership::joinInto(std::unordered_map<sema::Symbol*, LocalOwnership>& into,
              const std::unordered_map<sema::Symbol*,
                                       LocalOwnership>& other,
              SourceLocation joinLoc, bool emitClash) {
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
            // `emitClash` is false for the synthetic function-exit meet —
            // see the declaration in Ownership_impl.hpp. The old walker
            // never met at exit; emitting there would add a diagnostic.
            if (emitClash) {
                ctx_.diagnostics().report(
                    Severity::Error, joinLoc,
                    std::string("branch ownership conflict for `") +
                    std::string(sym->name) +
                    "`: one branch produced an owned value, the other a "
                    "borrowed value (§28.3 — equalize the branches before "
                    "the merge per the two Patterns)");
            }
            st.state = OwnershipState::Poisoned;
            continue;
        }
        // Other mismatches (Disposed/MovedFrom on one side) — poison.
        st.state = OwnershipState::Poisoned;
    }
}

sema::LocalSymbol* MethodOwnership::findLocalSymbol(const ast::LocalDecl& ld) {
    // Walk all collected symbols and find the LocalSymbol whose
    // decl matches. Sema slice 3b created these on demand.
    for (const auto& s : ctx_.symbols()) {
        if (auto* ls = dynamic_cast<sema::LocalSymbol*>(s.get())) {
            if (ls->decl == &ld) return ls;
        }
    }
    return nullptr;
}

LocalOwnership* MethodOwnership::lookupState(std::string_view name) {
    for (auto& [sym, st] : states_) {
        if (sym->name == name) return &st;
    }
    return nullptr;
}

void MethodOwnership::markDisposedByName(std::string_view name, SourceLocation loc) {
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

void MethodOwnership::markMovedByName(std::string_view name, SourceLocation loc) {
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

} // namespace bezel::ownership
