#include "ownership/Ownership_impl.hpp"

#include <functional>
#include <string>
#include <vector>

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

// O3b: true if `sym` is an owned-relevant local — a plain `var` (not a
// parameter) whose declared type is a non-nullable class or interface
// (the disposable kinds §28.3 tracks). Used to gate the Uninitialized-
// read diagnostic so it does not fire on value-typed locals (int, etc.),
// whose definite-assignment is sema's concern, not ownership's.
static bool isOwnedRelevantLocal(sema::Symbol* sym) {
    auto* ls = dynamic_cast<sema::LocalSymbol*>(sym);
    if (!ls || !ls->declType) return false;
    const sema::TypeRef* rt = ls->declType;
    if (rt->nullable) return false;
    if (rt->kind != sema::TypeRef::Kind::Named) return false;
    if (!rt->declaration) return false;
    return rt->declaration->kind == ast::TypeDeclKind::Class ||
           rt->declaration->kind == ast::TypeDeclKind::Interface;
}

void MethodOwnership::checkRead(const ast::Expr& e, bool selfTarget) {
    if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(&e)) {
        if (selfTarget) return;  // tolerated for discharge target
        // Find the tracked local by name so we can also inspect its
        // declared type for the Uninitialized-read gate below.
        sema::Symbol* sym = nullptr;
        LocalOwnership* st = nullptr;
        for (auto& [s, lo] : states_) {
            if (s->name == id->name) { sym = s; st = &lo; break; }
        }
        if (!st) return;
        if (st->state == OwnershipState::MovedFrom) {
            report(
                Severity::Error, id->span.begin,
                std::string("owned value in `") + std::string(id->name) +
                "` was already moved out at this point (§28.3)");
            st->state = OwnershipState::Poisoned;
        } else if (st->state == OwnershipState::Disposed) {
            report(
                Severity::Error, id->span.begin,
                std::string("read of disposed local `") +
                std::string(id->name) + "` (§28.3)");
            st->state = OwnershipState::Poisoned;
        } else if (st->state == OwnershipState::Uninitialized &&
                   isOwnedRelevantLocal(sym)) {
            // §28.3: a local declared without an initializer is
            // uninitialized and may not be read until assigned. Under O3b
            // this state reaches a read site when a loop body assigns the
            // local but the loop's zero-trip path does not, e.g.
            // `var x: Stream; while c do x := new Stream(); end;
            // x.Dispose();` — on the path where the body never ran, x is
            // still uninitialized at the read. (The gate to owned-relevant
            // locals keeps value-typed definite-assignment, which is
            // sema's concern, out of the ownership pass.)
            report(
                Severity::Error, id->span.begin,
                std::string("local `") + std::string(id->name) +
                "` may be read while uninitialized here: it is assigned "
                "only on a loop path that may not execute (§28.3 — an "
                "uninitialized local may not be read until assigned)");
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

// §28.3: decide whether a returned expression could carry a borrow that
// aliases into its identifier root. A *value-typed* return (the spec's
// "value-typed returns") is a copy and can never outlive its source, so
// it is never a borrow escape and is reported here as may-borrow = false.
bool MethodOwnership::returnedExprMayBorrow(const ast::Expr& e) const {
    // Prefer sema's resolved type for the returned expression: user-type
    // field/property/method-call results are typed here (e.g. `r.V`/`r.Size()`
    // returning int → value type → not a borrow). isBorrowableType is
    // conservative (unknown → borrowable).
    if (const sema::TypeRef* t = ctx_.exprType(e))
        return isBorrowableType(t);
    // Sema left the returned expression untyped. The one form this reaches
    // for a valid program is a bare property access on a built-in collection
    // whose value-typed projections sema types only on the *call* form
    // (Array/Stack/Queue/Set/LinkedList/Dictionary/StringBuilder/span). Those
    // projections — `Length`/`Count` (`int`/`long`, stdlib §3/§9.6/§15) and
    // `IsEmpty` (`bool`) — are value-typed copies, never a reference into the
    // collection, so returning one is not a §28.3 borrow escape. An element-
    // returning access (`Get`/`Peek`/…) is a CallExpr, which sema types, so
    // it does not reach this fallback; nor does a record/field access on a
    // user type. Be conservative for anything else still untyped.
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
        if (ma->member == "Length" || ma->member == "Count" ||
            ma->member == "IsEmpty")
            return false;  // value-typed projection → copy, not a borrow
    }
    return true;  // unknown → conservative (still a borrow candidate)
}

void MethodOwnership::checkBorrowEscapesOnReturn(const ast::Expr& e) {
    const ast::IdentifierExpr* root = identifierRoot(e);
    if (!root) return;
    // A value-typed return is a copy, not a borrow of the local — it
    // cannot outlive its source (§28.3: a borrowed local "may not be
    // returned from a method at all unless ... value-typed returns ...";
    // a value-typed projection is a copy, so it is never a borrow escape).
    if (!returnedExprMayBorrow(e)) return;
    for (const auto& [sym, st] : states_) {
        if (sym->name != root->name) continue;
        if (!st.implicitDischarge) continue;  // only `using var`
        report(
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

// ---------------------------------------------------------------------------
// §28.9 closure-escape and §9.7 borrow-bearing-record escape (issue #360).
//
// The direct-return forms of two soundness holes: a returned closure that
// captures an owned scope-bound local (the closure outlives the local's
// scope, so it reads the value after the local's `using var`/owned discharge
// at scope exit — a use-after-dispose), and a returned borrow-bearing record
// that carries a borrow of such a local (same lifetime violation through a
// record field). Indirect escapes (store into a field, pass to an out-param)
// are out of scope for this slice and remain deferred.
// ---------------------------------------------------------------------------

void MethodOwnership::collectIdentifiersIn(
    const ast::Expr& e,
    std::vector<const ast::IdentifierExpr*>& out) const {
    if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(&e)) {
        out.push_back(id);
        return;
    }
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
        if (ma->target) collectIdentifiersIn(*ma->target, out);
        return;
    }
    if (auto* nm = dynamic_cast<const ast::NullCondMemberExpr*>(&e)) {
        if (nm->target) collectIdentifiersIn(*nm->target, out);
        return;
    }
    if (auto* ce = dynamic_cast<const ast::CallExpr*>(&e)) {
        if (ce->callee) collectIdentifiersIn(*ce->callee, out);
        for (const auto& a : ce->args)
            if (a.value) collectIdentifiersIn(*a.value, out);
        return;
    }
    if (auto* nc = dynamic_cast<const ast::NullCondCallExpr*>(&e)) {
        if (nc->callee) collectIdentifiersIn(*nc->callee, out);
        for (const auto& a : nc->args)
            if (a.value) collectIdentifiersIn(*a.value, out);
        return;
    }
    if (auto* ix = dynamic_cast<const ast::IndexExpr*>(&e)) {
        if (ix->target) collectIdentifiersIn(*ix->target, out);
        for (const auto& a : ix->indices)
            if (a.value) collectIdentifiersIn(*a.value, out);
        return;
    }
    if (auto* be = dynamic_cast<const ast::BinaryExpr*>(&e)) {
        if (be->left) collectIdentifiersIn(*be->left, out);
        if (be->right) collectIdentifiersIn(*be->right, out);
        return;
    }
    if (auto* un = dynamic_cast<const ast::UnaryExpr*>(&e)) {
        if (un->operand) collectIdentifiersIn(*un->operand, out);
        return;
    }
    if (auto* pb = dynamic_cast<const ast::PostfixBangExpr*>(&e)) {
        if (pb->operand) collectIdentifiersIn(*pb->operand, out);
        return;
    }
    if (auto* tt = dynamic_cast<const ast::TypeTestExpr*>(&e)) {
        if (tt->operand) collectIdentifiersIn(*tt->operand, out);
        return;
    }
    if (auto* cd = dynamic_cast<const ast::ConditionalExpr*>(&e)) {
        if (cd->thenValue) collectIdentifiersIn(*cd->thenValue, out);
        if (cd->condition) collectIdentifiersIn(*cd->condition, out);
        if (cd->elseValue) collectIdentifiersIn(*cd->elseValue, out);
        return;
    }
    if (auto* ne = dynamic_cast<const ast::NewExpr*>(&e)) {
        for (const auto& a : ne->args)
            if (a.value) collectIdentifiersIn(*a.value, out);
        return;
    }
    if (auto* th = dynamic_cast<const ast::ThrowExpr*>(&e)) {
        if (th->operand) collectIdentifiersIn(*th->operand, out);
        return;
    }
    // A nested lambda's captures are themselves captures of the enclosing
    // lambda, so descend into it.
    if (auto* lam = dynamic_cast<const ast::LambdaExpr*>(&e)) {
        collectLambdaIdentifiers(*lam, out);
        return;
    }
    // Other expression forms carry no identifier that could be a tracked
    // method-scope local capture (literals, default/typeof/nameof, …).
}

void MethodOwnership::collectIdentifiersInStmt(
    const ast::Stmt& s,
    std::vector<const ast::IdentifierExpr*>& out) const {
    if (auto* es = dynamic_cast<const ast::ExpressionStmt*>(&s)) {
        if (es->expr) collectIdentifiersIn(*es->expr, out);
        return;
    }
    if (auto* rs = dynamic_cast<const ast::ReturnStmt*>(&s)) {
        if (rs->value) collectIdentifiersIn(*rs->value, out);
        return;
    }
    if (auto* ld = dynamic_cast<const ast::LocalDecl*>(&s)) {
        if (ld->initializer) collectIdentifiersIn(*ld->initializer, out);
        return;
    }
    if (auto* as = dynamic_cast<const ast::AssignmentStmt*>(&s)) {
        if (as->target) collectIdentifiersIn(*as->target, out);
        if (as->value) collectIdentifiersIn(*as->value, out);
        return;
    }
    if (auto* ds = dynamic_cast<const ast::DiscardStmt*>(&s)) {
        if (ds->value) collectIdentifiersIn(*ds->value, out);
        return;
    }
    // Control-flow statements inside a lambda block body (if/while/for/…)
    // are not walked: this bounded slice diagnoses the direct
    // `() => localRef.field` / single-statement-body forms exercised by the
    // reproducer. Deeper block-body capture analysis is deferred (and would
    // only add rejections, never relax one).
}

void MethodOwnership::collectLambdaIdentifiers(
    const ast::LambdaExpr& lam,
    std::vector<const ast::IdentifierExpr*>& out) const {
    if (lam.exprBody) {
        collectIdentifiersIn(*lam.exprBody, out);
    } else {
        for (const auto& st : lam.stmts)
            if (st) collectIdentifiersInStmt(*st, out);
    }
}

void MethodOwnership::checkClosureEscapesOnReturn(const ast::Expr& e) {
    // Gather every lambda appearing in the returned expression. The
    // returned expression is most commonly the lambda itself
    // (`return () => r.V;`), but it may also wrap it (e.g. a conditional).
    std::vector<const ast::LambdaExpr*> lambdas;
    std::function<void(const ast::Expr&)> findLambdas =
        [&](const ast::Expr& x) {
            if (auto* lam = dynamic_cast<const ast::LambdaExpr*>(&x)) {
                lambdas.push_back(lam);
                return;  // its captures are walked below, not here
            }
            if (auto* cd = dynamic_cast<const ast::ConditionalExpr*>(&x)) {
                if (cd->thenValue) findLambdas(*cd->thenValue);
                if (cd->elseValue) findLambdas(*cd->elseValue);
                return;
            }
            if (auto* ce = dynamic_cast<const ast::CallExpr*>(&x)) {
                for (const auto& a : ce->args)
                    if (a.value) findLambdas(*a.value);
                return;
            }
            if (auto* ne = dynamic_cast<const ast::NewExpr*>(&x)) {
                for (const auto& a : ne->args)
                    if (a.value) findLambdas(*a.value);
                return;
            }
        };
    findLambdas(e);
    if (lambdas.empty()) return;

    for (const ast::LambdaExpr* lam : lambdas) {
        std::vector<const ast::IdentifierExpr*> ids;
        collectLambdaIdentifiers(*lam, ids);
        for (const ast::IdentifierExpr* id : ids) {
            sema::Symbol* sym = ctx_.exprSymbol(*id);
            if (!sym) continue;
            // Resolve against the dataflow-tracked method-scope locals. A
            // captured lambda parameter is not in `states_`, so it is
            // naturally excluded; only an enclosing-method local/owned-param
            // can match.
            auto it = states_.find(sym);
            if (it == states_.end()) continue;
            const LocalOwnership& st = it->second;
            // §28.9: capturing an *owned* local (a `using var` or an
            // owned-producing local / `owned` parameter) is a borrow whose
            // source is the local's enclosing scope; the returned closure
            // outlives that scope. Only the Owned state carries this
            // scope-bound discharge obligation — a value-typed or
            // runtime-managed (string/function) local is never Owned, so
            // capturing one is not constrained and is not diagnosed here.
            if (st.state != OwnershipState::Owned) continue;
            // Belt-and-braces: confirm the captured local's declared type is
            // a class/interface reference (the only thing a §28.9 borrow can
            // alias). An Owned non-reference would be surprising, but the
            // type gate keeps the check from ever firing on a value-typed
            // local even if a future producer marked one Owned.
            const sema::TypeRef* declTy = nullptr;
            if (auto* ls = dynamic_cast<sema::LocalSymbol*>(sym))
                declTy = ls->declType;
            else if (auto* ps = dynamic_cast<sema::ParameterSymbol*>(sym)) {
                if (ps->declType) declTy = ctx_.resolvedType(*ps->declType);
                else declTy = ps->resolvedDeclType;
            }
            // Null declType → unknown; isBorrowableType is conservative
            // (true), but we additionally already gated on Owned, so this
            // only admits genuine owned-reference captures.
            if (declTy && !isBorrowableType(declTy)) continue;
            report(
                Severity::Error, id->span.begin,
                std::string("returned closure captures `") +
                std::string(sym->name) +
                "` whose source is disposed at scope end — capturing an "
                "owned local does not transfer ownership; the capture is a "
                "borrow of the enclosing scope and the returned closure "
                "would outlive it (use-after-dispose). (§28.9 closure-"
                "capture escape; keep the owned location in a longer-lived "
                "scope, or restructure so the closure does not escape)");
            return;  // one diagnostic per return is enough
        }
    }
}

bool MethodOwnership::isBorrowBearingRecord(const sema::TypeRef* rt) const {
    if (!rt || rt->kind != sema::TypeRef::Kind::Named || !rt->declaration)
        return false;
    const auto* decl =
        dynamic_cast<const ast::TypeDecl*>(rt->declaration->decl);
    if (!decl || decl->kind != ast::TypeDeclKind::Record) return false;

    // §9.7: a record is borrow-bearing when it holds at least one
    // borrow-carrying field of a non-runtime-managed type: a class/
    // interface reference field (nullable or not), an inline array whose
    // element is a reference type, or another borrow-bearing record
    // (transitivity). string / function / boxed and value-typed fields do
    // not make a record borrow-bearing.
    auto fieldIsBorrowCarrying = [&](const sema::TypeRef* ft) -> bool {
        if (!ft) return false;
        switch (ft->kind) {
            case sema::TypeRef::Kind::Named: {
                if (!ft->declaration) return false;  // generic param
                if (ft->declaration->kind == ast::TypeDeclKind::Class ||
                    ft->declaration->kind == ast::TypeDeclKind::Interface)
                    return true;
                if (ft->declaration->kind == ast::TypeDeclKind::Record)
                    return isBorrowBearingRecord(ft);  // transitive
                return false;  // enum
            }
            case sema::TypeRef::Kind::InlineArray:
                return ft->arrayElement &&
                       !ft->arrayOwned &&
                       isBorrowableType(ft->arrayElement);
            case sema::TypeRef::Kind::Primitive:
                // string / Object / numeric: runtime-managed or value type;
                // never a borrow source. (Object is boxed when holding a
                // value; the §9.7 list excludes boxed value types.)
                return false;
            default:
                return false;  // Unit, Function (runtime-managed), Error
        }
    };

    // Positional record fields (§9.1).
    for (const ast::Param& f : decl->positionalFields) {
        if (!f.type) continue;
        if (fieldIsBorrowCarrying(ctx_.resolvedType(*f.type))) return true;
    }
    // Body-record reference fields / auto-properties (§9.2).
    for (const auto& m : decl->members) {
        if (auto* fd = dynamic_cast<const ast::FieldDecl*>(m.get())) {
            if (fd->type &&
                fieldIsBorrowCarrying(ctx_.resolvedType(*fd->type)))
                return true;
        }
    }
    return false;
}

void MethodOwnership::checkRecordBorrowEscapesOnReturn(const ast::Expr& e) {
    // Direct form: `return new Rec(...);` where Rec is a borrow-bearing
    // record (§9.7). The static type of the returned expression must be a
    // borrow-bearing record, and at least one constructor argument must
    // alias a scope-bound (`using var` / owned) local — the borrow whose
    // source is disposed at this method's exit.
    const auto* ne = dynamic_cast<const ast::NewExpr*>(&e);
    if (!ne) return;
    const sema::TypeRef* rt =
        ne->type ? ctx_.resolvedType(*ne->type) : ctx_.exprType(e);
    if (!isBorrowBearingRecord(rt)) return;

    for (const ast::CallArgument& a : ne->args) {
        if (!a.value) continue;
        const ast::IdentifierExpr* root = identifierRoot(*a.value);
        if (!root) continue;
        sema::Symbol* sym = ctx_.exprSymbol(*root);
        const LocalOwnership* st = nullptr;
        if (sym) {
            auto it = states_.find(sym);
            if (it != states_.end()) st = &it->second;
        }
        if (!st) {
            // Fallback for an argument identifier sema left unbound: match
            // by name against a tracked owned local (parallels the
            // by-name match in checkBorrowEscapesOnReturn).
            for (const auto& [s, lo] : states_) {
                if (s->name == root->name) { st = &lo; break; }
            }
        }
        if (!st || st->state != OwnershipState::Owned) continue;
        report(
            Severity::Error, ne->span.begin,
            std::string("returned borrow-bearing record carries a borrow of "
                        "`") +
            std::string(root->name) +
            "` whose source is disposed at scope end — a borrow-bearing "
            "record may not outlive the source of any borrow it carries, "
            "and returning it from this method does (§9.7 record borrow-"
            "lifetime rule; bind the source in the caller's scope and build "
            "the record there instead)");
        return;  // one diagnostic per return is enough
    }
}

bool MethodOwnership::isOwnedTrackedLocal(sema::Symbol* sym) const {
    if (!sym) return false;
    auto it = states_.find(sym);
    return it != states_.end() &&
           it->second.state == OwnershipState::Owned;
}

sema::Symbol* MethodOwnership::borrowSourceForExpr(const ast::Expr& rhs) {
    // Shape 1 — a re-borrow rooted at an Owned local. Per §28.3 producers:
    // a local read `r` of an owned local in a borrow-consuming position is a
    // borrow (source: `r`); a field/indexer access `r.f` / `r[i]` is a
    // borrow (source: `r`, the access target's root). identifierRoot peels
    // member/index chains down to the root identifier. If that root resolves
    // to a still-Owned scope-bound local, the re-borrow's source is that
    // local. (A bare-identifier `var b := r;` and a reference field
    // `var b := r.f;` both land here.) // per §28.3
    //
    // SOUNDNESS GATE — value-typed copies are never borrows. A projection
    // like `var n := r.V;` where V is `int` is a copy, not a borrow of `r`
    // (§28.3 "value-typed returns"), so it must NOT be recorded as aliasing
    // `r`. returnedExprMayBorrow is exactly the borrow-vs-copy predicate
    // (sema-typed; value-typed projection → false), so gating on it keeps a
    // value-typed local from ever being flagged later. // per §28.3
    if (dynamic_cast<const ast::IdentifierExpr*>(&rhs) ||
        dynamic_cast<const ast::MemberAccessExpr*>(&rhs) ||
        dynamic_cast<const ast::NullCondMemberExpr*>(&rhs) ||
        dynamic_cast<const ast::IndexExpr*>(&rhs)) {
        if (returnedExprMayBorrow(rhs)) {
            if (const ast::IdentifierExpr* root = identifierRoot(rhs)) {
                sema::Symbol* sym = ctx_.exprSymbol(*root);
                if (isOwnedTrackedLocal(sym)) return sym;
                // Multi-hop alias chain (#386 case 3): the re-borrow's root is
                // not itself owned but is a tracked local that already carries
                // a borrow source — it was bound from a re-borrow of an owned
                // local one or more statements earlier. Re-borrowing it
                // re-borrows the same owned source: `var b := r; var c := b;`
                // makes `c`'s source `r`. Follow the chain to the underlying
                // owned source, provided that source is still a live owned
                // obligation here. The earlier link's `source` was itself set
                // by this function, so deeper chains compose transitively (the
                // n-th link points straight at the owned root). // per §28.3
                if (sym) {
                    auto it = states_.find(sym);
                    if (it != states_.end() && it->second.source &&
                        isOwnedTrackedLocal(it->second.source->sym))
                        return it->second.source->sym;
                }
            }
        }
    }
    // Shape 2 — a borrow-bearing-record construction whose constructor arg
    // aliases an Owned local (§9.7). The constructed record value carries a
    // borrow of that local; binding it to a holder local propagates the
    // source. Mirrors checkRecordBorrowEscapesOnReturn's arg scan but here
    // the record is bound to a local rather than returned directly.
    if (const auto* ne = dynamic_cast<const ast::NewExpr*>(&rhs)) {
        const sema::TypeRef* rt =
            ne->type ? ctx_.resolvedType(*ne->type) : ctx_.exprType(rhs);
        if (isBorrowBearingRecord(rt)) {
            for (const ast::CallArgument& a : ne->args) {
                if (!a.value) continue;
                const ast::IdentifierExpr* root = identifierRoot(*a.value);
                if (!root) continue;
                sema::Symbol* sym = ctx_.exprSymbol(*root);
                if (isOwnedTrackedLocal(sym)) return sym;
            }
        }
    }
    // Shape 3 — a closure capturing an Owned local (§28.9). Capturing an
    // owned class-typed local does not transfer ownership; the capture is a
    // borrow whose source is the local's enclosing scope. Binding the closure
    // to a holder local propagates that source.
    if (const auto* lam = dynamic_cast<const ast::LambdaExpr*>(&rhs)) {
        std::vector<const ast::IdentifierExpr*> ids;
        collectLambdaIdentifiers(*lam, ids);
        for (const ast::IdentifierExpr* id : ids) {
            sema::Symbol* sym = ctx_.exprSymbol(*id);
            if (isOwnedTrackedLocal(sym)) return sym;
        }
    }
    return nullptr;
}

void MethodOwnership::checkIndirectEscapeOnReturn(const ast::Expr& e) {
    // §28.9 / §9.7 indirect form: `return holder;` where `holder` is a bare
    // local whose tracked borrow-source aliases a still-owned scope-bound
    // local. The returned expression must be the bare holder identifier — a
    // value projection off the holder (`holder.SomeInt`, `holder()`) returns
    // a copy, not the borrow, and is intentionally not flagged here (those
    // are gated by returnedExprMayBorrow on the direct paths). Restricting to
    // the bare-identifier shape keeps the locally-invoked-closure control
    // (`return f();`) accepted: there the returned expression is a CallExpr,
    // not the bare `f`.
    const auto* id = dynamic_cast<const ast::IdentifierExpr*>(&e);
    if (!id) return;
    sema::Symbol* sym = ctx_.exprSymbol(*id);
    const LocalOwnership* st = nullptr;
    if (sym) {
        auto it = states_.find(sym);
        if (it != states_.end()) st = &it->second;
    }
    if (!st || !st->source) return;
    // The source must still be a live owned scope-bound obligation at the
    // return point. If it was already disposed / moved out, there is no live
    // borrow to dangle (and a separate diagnostic would have covered misuse).
    if (!isOwnedTrackedLocal(st->source->sym)) return;
    std::string_view srcName = st->source->sym->name;
    report(
        Severity::Error, id->span.begin,
        std::string("returned `") + std::string(id->name) +
        "` carries a borrow of `" + std::string(srcName) +
        "` whose source is disposed at scope end — the value aliases an "
        "owned scope-bound local and returning it lets the borrow outlive "
        "that local (§28.9 / §9.7 borrow-lifetime rule; bind the source in "
        "the caller's scope and build the aliasing value there instead)");
}

sema::Symbol* MethodOwnership::ownedLocalBorrowSource(const ast::Expr& rhs) {
    // Reuse the full borrow-source computation (covering the re-borrow,
    // borrow-bearing-record, closure-capture, and multi-hop shapes), then
    // restrict the result to an owned scope-bound *local*. An owned parameter
    // is excluded: assigning an owned parameter into an owned field is the
    // §28.8 ownership-transfer pattern, not a borrow store. // per §28.3, §28.8
    sema::Symbol* src = borrowSourceForExpr(rhs);
    if (!src) return nullptr;
    if (!dynamic_cast<sema::LocalSymbol*>(src)) return nullptr;
    return src;
}

void MethodOwnership::checkBorrowStoreEscape(const ast::AssignmentStmt& as) {
    if (as.op != ":=") return;
    if (!as.target || !as.value) return;

    // Identify the kind of escaping destination, and the diagnostic flavour.
    // Two destinations outlive a method-scope local:
    enum class Dest { None, Field, OutParam };
    Dest dest = Dest::None;
    if (dynamic_cast<const ast::MemberAccessExpr*>(as.target.get())) {
        // `obj.f := …` / `this.f := …` — a field/property write. The field's
        // object outlives the local; §28.3 forbids storing a borrow in any
        // field ("class-typed fields that hold a borrow are not expressible in
        // this version").
        dest = Dest::Field;
    } else if (const auto* id =
                   dynamic_cast<const ast::IdentifierExpr*>(as.target.get())) {
        // `p := …` where `p` is an out / ref / out-owned parameter — the
        // parameter's storage is the caller's frame, which outlives this
        // method. Writing a borrow of a local through it is the same lifetime
        // violation as returning the borrow (§28.3; §22.3 `out` produces a
        // borrow location for the caller). A plain (non-out/ref) parameter is
        // not an output location, so it is not a destination here.
        sema::Symbol* sym = ctx_.exprSymbol(*id);
        if (auto* ps = dynamic_cast<sema::ParameterSymbol*>(sym)) {
            if (ps->modifier == "out" || ps->modifier == "ref" ||
                ps->modifier == "out owned")
                dest = Dest::OutParam;
        }
    }
    if (dest == Dest::None) return;

    // The RHS must borrow an owned scope-bound LOCAL (the value that dies at
    // this scope's end). recordMovesIn has already run in handleAssignment, so
    // a `move`d source is now MovedFrom, not Owned, and is correctly not seen
    // as a live borrow source — `box.f := move r;` / `o := move r;` transfers,
    // it does not store a borrow.
    sema::Symbol* src = ownedLocalBorrowSource(*as.value);
    if (!src) return;

    if (dest == Dest::Field) {
        report(
            Severity::Error, as.span.begin,
            std::string("borrow of `") + std::string(src->name) +
            "` stored into a field would outlive its source — `" +
            std::string(src->name) +
            "` is an owned scope-bound local disposed at scope end, and a "
            "field of the target object outlives it (§28.3 — a borrow may not "
            "be assigned to any field of any object; store an owned value with "
            "`move`, or keep the borrow's source alive as long as the field)");
    } else {
        report(
            Severity::Error, as.span.begin,
            std::string("borrow of `") + std::string(src->name) +
            "` written through an out/ref parameter would outlive its source "
            "— `" + std::string(src->name) +
            "` is an owned scope-bound local disposed at scope end, and the "
            "parameter's location lives in the caller's frame, which outlives "
            "this method (§28.3 borrow-lifetime rule; produce an owned value "
            "with `move` for an `out owned` parameter, or return/keep the "
            "source in a longer-lived scope)");
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
              SourceLocation joinLoc, bool emitClash, bool unionKeys) {
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
        // §28.3: "a local declared `var x: T;` without an initializer is
        // uninitialized and may not be read until assigned." At a loop-
        // carried merge (`unionKeys`), when one incoming path leaves the
        // local uninitialized — most notably the zero-trip path of a loop
        // whose body assigns it — the merged state stays Uninitialized: a
        // subsequent read is then a §28.3 violation (the O3b zero-trip-leak
        // signal, see runDataflow), and the obligation the assigned path
        // carried is not silently honoured for the never-assigned path.
        //
        // This is intentionally scoped to loop merges. The if/elif/else
        // merge keeps O4's existing treatment (Owned ⊓ Uninitialized →
        // Poisoned, silent — a subsequent read diagnoses), which the O4
        // branch-join tests pin (own_join.no_else_with_owned_in_then_is_
        // clash). O3b changes loop behavior only.
        if (unionKeys && (st.state == OwnershipState::Uninitialized ||
                          o.state == OwnershipState::Uninitialized)) {
            st.state = OwnershipState::Uninitialized;
            continue;
        }
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
                report(
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
        // §28.6 SOUNDNESS (issue #338): the discharge obligation is
        // PER-PATH. When one incoming edge still holds the obligation
        // (Owned) while the other discharged it (Disposed via an explicit
        // `.Dispose()`, or MovedFrom via a move / owned-transfer), the meet
        // must NOT drop the obligation: on the still-Owned path the value
        // reaches the merge undischarged, and "an owned local must reach an
        // owned slot before it goes out of scope ... on a *normal-exit*
        // path" (§28.3). Demoting this merge to Poisoned (which the rest of
        // the checker reads as "no remaining obligation") was unsound — it
        // accepted a program that disposes on only SOME branches, a real
        // leak on the others. The fix RETAINS the Owned state (carrying the
        // Owned side's obligation origin and implicit-discharge flag) so the
        // value surfaces at the end-of-scope leak check on the path that
        // never discharged it.
        //
        // This deliberately does NOT introduce a double-dispose error for
        // the dispose-on-ALL-paths case: there both edges are Disposed,
        // `st.state == o.state` short-circuits at the top of the loop and
        // never reaches here. Exception edges are unaffected: the unwound
        // owned local is *removed* from the incoming state before the meet
        // (dischargeOnExceptionEdge), so it is never the Disposed/Owned
        // operand of this rule. // per §28.3, §28.6
        const bool ownedDischargedSplit =
            (st.state == OwnershipState::Owned &&
             (o.state == OwnershipState::Disposed ||
              o.state == OwnershipState::MovedFrom)) ||
            ((st.state == OwnershipState::Disposed ||
              st.state == OwnershipState::MovedFrom) &&
             o.state == OwnershipState::Owned);
        if (ownedDischargedSplit) {
            // Keep the Owned record so obligationOrigin / implicitDischarge
            // are the ones that drive the end-of-scope diagnostic. If the
            // Owned side is `other`, copy its record over `into`'s.
            if (st.state != OwnershipState::Owned) st = o;
            continue;
        }
        // Other mismatches (e.g. Uninitialized on one side at a non-loop
        // merge) — poison. The Owned ⊓ Uninitialized non-loop case retains
        // the existing O4 treatment (silent poison; a subsequent read
        // diagnoses), which the O4 branch-join tests pin.
        st.state = OwnershipState::Poisoned;
    }
    // O3b union-key meet (loop back-edge / zero-trip-skip merges only):
    // a local present on only one incoming path is carried into the
    // result verbatim. For a loop-body-scoped owned local that reaches
    // the loop's after-block on the body-exit path but does not exist on
    // the zero-trip skip path, this preserves the §28.3 discharge
    // obligation so the end-of-method leak check still fires. Carrying
    // in Disposed/Borrowed/MovedFrom states is harmless (no obligation).
    if (unionKeys) {
        for (const auto& [sym, o] : other) {
            if (into.find(sym) == into.end()) into[sym] = o;
        }
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
        report(
            Severity::Error, loc,
            std::string("local `") + std::string(name) +
            "` was already disposed (§28.3 — `.Dispose()` is "
            "idempotent but re-calling it on a known-disposed local "
            "indicates a bug)");
        st->state = OwnershipState::Poisoned;
        return;
    }
    if (st->state == OwnershipState::MovedFrom) {
        report(
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
        report(
            Severity::Error, loc,
            std::string("owned value in `") + std::string(name) +
            "` was already moved out at this point (§28.3)");
        st->state = OwnershipState::Poisoned;
        return;
    }
    if (st->state == OwnershipState::Disposed) {
        report(
            Severity::Error, loc,
            std::string("cannot move from disposed local `") +
            std::string(name) + "` (§28.3)");
        st->state = OwnershipState::Poisoned;
        return;
    }
    if (st->implicitDischarge) {
        report(
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
