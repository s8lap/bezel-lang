#include "ownership/Ownership_impl.hpp"

#include <functional>
#include <string>
#include <vector>

namespace bezel::ownership {

// The MethodOwnership class is declared in Ownership_impl.hpp; this
// translation unit holds the move / borrow / discharge check definitions.
// The statement/flow-walking definitions live in Ownership.cpp alongside
// the public checkOwnership entry point.

namespace {
// §28.8.1 (#1125): the operand of a `move` may be a force-unwrap of a nullable
// owned field/local — `move _slot!` / `move this._slot!`. The null-forgiving
// `!` is a postfix PostfixBangExpr wrapping the field/local reference. The spec
// (line 3529) endorses `move _slot!` followed by `_slot := null` as THE
// canonical nullable-owned-field detach pattern, so the force-unwrap form is a
// move of the underlying field/local exactly like the bare form and carries the
// same §28.8.1 field-restoration obligation. Look through a single leading `!`
// so the move-recognition below sees the field/local identifier. (Undiagnosed,
// the field stays moved-from with no restoration check, and the owner's
// auto-disposer re-disposes the moved-out object → double-free / SIGSEGV.)
const ast::Expr* lookThroughForceUnwrap(const ast::Expr* e) {
    // The null-forgiving `!` is a postfix operator → PostfixBangExpr (§A.7),
    // not a prefix UnaryExpr.
    if (auto* b = dynamic_cast<const ast::PostfixBangExpr*>(e))
        if (b->operand) return b->operand.get();
    return e;
}
} // namespace

void MethodOwnership::recordMovesIn(const ast::Expr& e) {
    if (auto* un = dynamic_cast<const ast::UnaryExpr*>(&e)) {
        if (un->op == "move") {
            // §28.8.1 (#1125): unwrap a force-unwrap `move x!` to the underlying
            // field/local reference — `move _slot!` is a move of `_slot`.
            const ast::Expr* mop = lookThroughForceUnwrap(un->operand.get());
            if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(mop)) {
                // §28.8.1 / #825: a bare `move _field` is syntactically
                // identical to `move this._field` — both leave the field in
                // moved-from state and must be restored before the method exits.
                // exprIsOwnedFieldRead resolves the bare identifier via sema
                // and returns true only for an `owned` field of the current class.
                if (exprIsOwnedFieldRead(*mop)) {
                    // §28.8.1, #897 #898: check-then-mark via recordFieldMove
                    // so a second move of an already-moved-from field errors.
                    recordFieldMove(id->name, un->span.begin);
                    return;
                }
                markMovedByName(id->name, id->span.begin);
                return;
            }
            if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(mop)) {
                if (auto* tid =
                        dynamic_cast<const ast::IdentifierExpr*>(
                            ma->target.get())) {
                    if (tid->name == "this") {
                        // §28.8.1, #897 #898: qualified `move this.field`.
                        recordFieldMove(ma->member, un->span.begin);
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
        // §28.13 / stdlib §2.16 (#603): `new Shared<T>(value)` is a builtin
        // type with no user ConstructorDecl. Its sole constructor takes an
        // `owned value: T` at position 0 (per stdlib §2.16). Detect this so
        // the implicit-transfer path below correctly consumes the argument.
        bool isSharedNew = false;
        if (ne->type) {
            if (auto* rt = ctx_.resolvedType(*ne->type)) {
                if (!rt->declaration && rt->astNode && rt->astNode->name &&
                    !rt->astNode->name->parts.empty() &&
                    rt->astNode->name->parts[0] == "Shared" &&
                    ne->args.size() == 1) {
                    isSharedNew = true;
                }
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
                    // §28.8 (#395): the consumed value must be owned, not a borrow.
                    if (!checkOwnedArgSource(*a.value, id->span.begin))
                        markMovedByName(id->name, id->span.begin);
                    ++pos; continue;
                }
                if (exprIsOwnedFieldRead(*a.value)) {
                    // §28.3 / §28.8.1 (#398): `owned obj.f` of a bare owned-
                    // field read is a borrow (only `move obj.f` is owned).
                    checkOwnedArgSource(*a.value, a.value->span.begin);
                    ++pos; continue;
                }
                if (checkOwnedArgOtherBorrow(*a.value)) {
                    // §28.3 / §28.11 (#407): `owned coll[i]` / `owned obj.f` is a
                    // borrow (bare `this` was handled by the identifier branch).
                    ++pos; continue;
                }
            }
            // §28.8 implicit transfer: bare id to `owned` formal param.
            if (a.modifier.empty() && cd && pos < cd->params.size() &&
                (cd->params[pos].modifier == "owned" ||
                 cd->params[pos].modifier == "out owned")) {
                if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(a.value.get())) {
                    // §28.8 (#395): reject a borrow flowing into the owned formal.
                    if (!checkOwnedArgSource(*a.value, id->span.begin))
                        markMovedByName(id->name, id->span.begin);
                    ++pos; continue;
                }
                if (exprIsOwnedFieldRead(*a.value)) {
                    // §28.3 / §28.8.1 (#398): a bare owned-field read flowing
                    // into an owned ctor param is a borrow, not a transfer.
                    checkOwnedArgSource(*a.value, a.value->span.begin);
                    ++pos; continue;
                }
                if (checkOwnedArgOtherBorrow(*a.value)) {
                    // §28.3 / §28.11 (#407): a bare `coll[i]` / `obj.f` flowing
                    // into an owned ctor param is a borrow, not a transfer — the
                    // double-dispose hazard (findings 18, 20). (Bare `this` was
                    // handled by the identifier branch above.)
                    ++pos; continue;
                }
            }
            // §28.13 / stdlib §2.16 (#603): `new Shared<T>(value)` has an
            // implicit owned parameter at position 0. Apply the same owned-
            // argument checks as a user ctor with `owned value: T` would.
            // per §28.8 / §28.13.
            if (a.modifier.empty() && isSharedNew && pos == 0) {
                if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(a.value.get())) {
                    if (!checkOwnedArgSource(*a.value, id->span.begin))
                        markMovedByName(id->name, id->span.begin);
                    ++pos; continue;
                }
                if (exprIsOwnedFieldRead(*a.value)) {
                    checkOwnedArgSource(*a.value, a.value->span.begin);
                    ++pos; continue;
                }
                if (checkOwnedArgOtherBorrow(*a.value)) {
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
                // §28.8 (#395): the consumed value must be owned, not a borrow.
                if (!checkOwnedArgSource(*a.value, id->span.begin))
                    markMovedByName(id->name, id->span.begin);
                ++pos; continue;
            }
            if (exprIsOwnedFieldRead(*a.value)) {
                // §28.3 / §28.8.1 (#398): `owned obj.f` of a bare owned-field
                // read is a borrow (only `move obj.f` is owned).
                checkOwnedArgSource(*a.value, a.value->span.begin);
                ++pos; continue;
            }
            if (checkOwnedArgOtherBorrow(*a.value)) {
                // §28.3 / §28.11 (#407): `owned coll[i]` / `owned obj.f` is a
                // borrow under an explicit `owned` arg (bare `this` was handled
                // by the identifier branch above).
                ++pos; continue;
            }
        }
        // §28.8 implicit ownership transfer: a bare identifier passed
        // to an `owned` formal parameter transfers ownership without
        // an explicit `owned` or `move` at the call site. The owned-sink
        // formal is either a user-declared `owned` param or a built-in
        // owning-collection method whose owned arg sits at this position
        // (#395: built-in collection methods have no AST declaration).
        {
            const std::vector<ast::Param>* params = nullptr;
            if (calleeMethod)  params = &calleeMethod->params;
            if (calleeCtor)    params = &calleeCtor->params;
            // §28.3 / §28.11 (#860): an `out owned` formal is the owned-OUT
            // direction — the argument local RECEIVES ownership, it is NOT a
            // moved-from owned-IN sink. (It is marked Owned post-call by
            // markOutOwnedArgs.) Only a plain `owned` formal consumes the
            // argument here; including `out owned` would wrongly mark the
            // receiving local moved-from.
            const bool userOwned =
                params && pos < params->size() &&
                (*params)[pos].modifier == "owned";
            const bool ownedSink =
                userOwned || builtinOwnedSinkArg(callee, pos);
            if (ownedSink) {
                if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(
                        a.value.get())) {
                    // §28.8 (#395): reject a borrow flowing into the owned
                    // formal; otherwise record the implicit move.
                    if (!checkOwnedArgSource(*a.value, id->span.begin))
                        markMovedByName(id->name, id->span.begin);
                    ++pos; continue;
                }
                if (exprIsOwnedFieldRead(*a.value)) {
                    // §28.3 / §28.8.1 (#398): a bare owned-field read flowing
                    // implicitly into an owned sink is a borrow, not a transfer.
                    checkOwnedArgSource(*a.value, a.value->span.begin);
                    ++pos; continue;
                }
                if (checkOwnedArgOtherBorrow(*a.value)) {
                    // §28.3 / §28.11 (#407): a bare `coll[i]` / `obj.f` flowing
                    // implicitly into the owned sink is a borrow (bare `this` was
                    // handled by the identifier branch above).
                    ++pos; continue;
                }
                // `move id` to an owned formal: `move b` of a borrow is not an
                // owned value (§28.8/#395). When the source is owned, fall
                // through to recordMovesIn, which records the legitimate move.
                if (auto* un = dynamic_cast<const ast::UnaryExpr*>(
                        a.value.get())) {
                    if (un->op == "move" &&
                        dynamic_cast<const ast::IdentifierExpr*>(
                            un->operand.get()) &&
                        checkOwnedArgSource(*a.value, un->span.begin)) {
                        ++pos; continue;  // reported; skip the bogus move
                    }
                }
            }
        }
        recordMovesIn(*a.value);
        ++pos;
    }
    // §28.3 / §28.11 (#860): after the arguments' owned-IN consumption is
    // recorded, model the caller-side owned-OUT transfer — a local passed as
    // `out owned` RECEIVES ownership of the moved-out element and becomes Owned
    // (so dropping it is flagged by the §28.3 discharge check). Done last so it
    // is not disturbed by the consume walk above; the two directions are
    // disjoint per argument (an arg is either owned-IN or owned-OUT).
    markOutOwnedArgs(args, callee);
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
    // §28.3 / §28.11 (#860): only a plain `owned` formal is an owned-IN
    // consuming sink (the argument is consumed by the call). An `out owned`
    // formal is the owned-OUT direction — the argument local RECEIVES
    // ownership, it is not consumed here — so it must NOT be classified as
    // an implicit owned sink (that mis-marked the receiving local moved-from
    // and mis-classified an `await` in out-position as owned-consuming).
    return mod == "owned";
}

void MethodOwnership::handleDischarge(const ast::Expr& e) {
    // §31.1 / §28.8 / §17.12 (issue #406): a `throw` in EXPRESSION position
    // (`x ?? throw new Wrap(b)`, `case ... => throw new Wrap(b)`) is a
    // `ThrowExpr` wrapping its operand, whereas a statement-level `throw` is a
    // `ThrowStmt` whose value is the operand directly. Both consume the operand
    // as an owned value transferred to the runtime's exception machinery, so the
    // operand's `new`/call arguments must be walked through the same owned-
    // argument check (§28.8 transfer) as in any other operand position. Recurse
    // into the operand; a bare re-raise `throw` (null operand, §31.2.4) carries
    // nothing to check. // per §31.1, §28.8, §17.12
    if (auto* te = dynamic_cast<const ast::ThrowExpr*>(&e)) {
        // §31.1 / #430: `throw new Wrap(ex)` in expression position (arm body
        // expression form or `??` chain) must check the same catch-borrow-escapes-
        // via-new-expr rule as the ThrowStmt path in visitStmt.  The ThrowStmt path
        // calls this check explicitly; mirror it here so that expression-form arm
        // bodies (`=> throw new Wrap(ex)` without `;`) are also covered. per §31.1
        if (te->operand) {
            if (const auto* ne =
                    dynamic_cast<const ast::NewExpr*>(te->operand.get())) {
                checkCatchBorrowEscapesViaNewExpr(*ne, ne->span.begin);
            }
            handleDischarge(*te->operand);
        }
        return;
    }
    // §19 / §28.8 (#1284): a `case` EXPRESSION (`var r := case b Box(a) => …`,
    // `return case … end`). The expression-position counterpart of the CaseStmt
    // handler in visitStmt (#1280): walk the scrutinee, each arm's guard, and each
    // arm's value expression, so a borrow handed to an `owned` sink inside a
    // case-expression arm (`Box(a) => Prog.Sink(a)`) is caught (§28.8 double-free).
    // Each arm is a mutually-exclusive branch: visit it from the pre-case state and
    // restore afterward (arm-local discharge does not leak into the post-case
    // state). Nothing else here consumes the CaseExpr as an owned value.
    if (auto* ce = dynamic_cast<const ast::CaseExpr*>(&e)) {
        if (ce->scrutinee) checkRead(*ce->scrutinee, /*selfTarget=*/false);
        BlockState saved = states_;
        for (const auto& arm : ce->arms) {
            states_ = saved;
            if (arm.guard) checkRead(*arm.guard, /*selfTarget=*/false);
            if (arm.bodyKind == ast::CaseArmBodyKind::Expression) {
                if (arm.exprBody) handleDischarge(*arm.exprBody);
            } else {
                for (const auto& bs : arm.stmts) if (bs) visitStmt(*bs);
            }
        }
        states_ = std::move(saved);
        return;
    }
    if (auto* call = dynamic_cast<const ast::CallExpr*>(&e)) {
        // §28.6 *Borrowed owning collections* (#1008 Part C): reject an
        // ownership-transferring / disposing op (Dispose/Drain/Pop*/Add/…) on a
        // BORROW of an owning collection (the owner disposes it — a borrower's
        // call would double-free or steal). The owner's own field / owned local
        // keeps the full API.
        if (call->callee)
            if (auto* fma = dynamic_cast<const ast::MemberAccessExpr*>(
                    call->callee.get()))
                checkBorrowedOwningCollectionCapability(*fma);
        // §28.4 / §28.5 (#1230): explicit `.Dispose()` of an owned field of
        // THIS object inside a destructor body double-frees: the compiler-
        // generated Dispose() runs the destructor body first and then
        // auto-disposes all owned-bearing fields (§28.4). Reject this pattern
        // with a sema error so it never reaches codegen.
        //
        // Scope: bare `_f.Dispose()` (IdentifierExpr resolving to a
        // FieldSymbol) or `this._f.Dispose()` (MemberAccessExpr whose target
        // is the identifier `this`) — own fields only. A `.Dispose()` on a
        // local (`using var x := …; x.Dispose();`), a using-var, a parameter,
        // or a field of another object are NOT rejected here; neither is a
        // `.Dispose()` on an owned field in a normal (non-destructor) method.
        // per §28.4 / §28.5
        if (isDestructor_ && call->callee) {
            if (auto* cma = dynamic_cast<const ast::MemberAccessExpr*>(
                    call->callee.get())) {
                if (cma->member == "Dispose" && cma->target) {
                    const ast::Expr* recv = cma->target.get();
                    bool isOwnedThisField = false;
                    if (exprIsOwnedFieldRead(*recv)) {
                        if (dynamic_cast<const ast::IdentifierExpr*>(recv)) {
                            // Bare `_f` — implicit `this`.
                            isOwnedThisField = true;
                        } else if (auto* fma2 =
                                       dynamic_cast<const ast::MemberAccessExpr*>(
                                           recv)) {
                            if (auto* tid =
                                    dynamic_cast<const ast::IdentifierExpr*>(
                                        fma2->target.get()))
                                isOwnedThisField = (tid->name == "this");
                        }
                    }
                    if (isOwnedThisField) {
                        report(
                            Severity::Error, cma->span.begin,
                            "explicit `.Dispose()` of an owned field inside a "
                            "destructor double-frees: the compiler-generated "
                            "Dispose() runs the destructor body and then "
                            "auto-disposes all owned fields (§28.4). Remove the "
                            "explicit call — the field is disposed automatically "
                            "(§28.5). To take ownership of the field early and "
                            "suppress the auto-cleanup, move it out first: "
                            "`using var t := move _field;`");
                        return;
                    }
                }
            }
        }
        // §28.2: a further Dispose() call on an already-disposed instance is
        // handled by markDisposedByName (further below), which emits the
        // "already disposed" error.  We must NOT also fire the §28.2
        // use-after-dispose WARNING for `x.Dispose()` when x is already
        // Disposed — that would be a noisy second diagnostic for the same
        // site.  However, for other ownership states (Uninitialized,
        // MovedFrom) the callee read-check MUST fire so that reading an
        // uninitialized or moved-from local via `.Dispose()` is diagnosed.
        // Strategy: if this is `x.Dispose()` and x is currently Disposed,
        // skip checkRead (markDisposedByName will diagnose); otherwise call
        // checkRead normally. // per §28.2
        bool skipCalleeReadForDispose = false;
        if (call->callee) {
            if (auto* cma = dynamic_cast<const ast::MemberAccessExpr*>(
                    call->callee.get())) {
                if (cma->member == "Dispose") {
                    if (auto* tid = dynamic_cast<const ast::IdentifierExpr*>(
                            cma->target.get())) {
                        // Only skip read-check if the local is already Disposed
                        // (the double-dispose case that markDisposedByName handles).
                        for (auto& [s, lo] : states_) {
                            if (s->name == tid->name) {
                                skipCalleeReadForDispose =
                                    (lo.state == OwnershipState::Disposed);
                                break;
                            }
                        }
                    }
                }
            }
        }
        if (call->callee && !skipCalleeReadForDispose)
            checkRead(*call->callee, /*selfTarget=*/true);
        // Slice O14 + §28.8: argument-mode handling.
        std::size_t pos = 0;
        for (const auto& a : call->args) {
            if (!a.value) { ++pos; continue; }
            bool consumed = false;
            // §28.3 / §28.11 (#860 / #892): an `out owned` arg is a WRITE
            // target — the call deposits an owned value into the local; it does
            // NOT read the local. Skip read-checking it (a stale Disposed state
            // from a prior loop iteration would otherwise produce a spurious
            // use-after-dispose warning). The actual Owned marking is done by
            // markOutOwnedArgs at the end of this CallExpr arm; detect the
            // out-owned position here via the call-site modifier and the formal
            // parameter / builtin dispatch so `consumed=true` suppresses the
            // unwanted read. User `out owned` is reliable; `builtinOutOwnedArg`
            // covers builtin TryPop / TryDequeue / etc. // per §28.3, §28.11
            if (a.modifier == "out owned var" ||  // per §22.4 / v0.160
                builtinOutOwnedArg(call->callee.get(), pos)) {
                consumed = true;  // write target: skip read-check; markOutOwnedArgs handles it
            } else if (a.modifier == "owned") {
                if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(
                        a.value.get())) {
                    // §28.8 (#395): an `owned`-modified arg must be owned, not
                    // a borrow. On violation, report and skip the bogus move;
                    // either way the arg is handled (do not re-read it below).
                    if (!checkOwnedArgSource(*a.value, id->span.begin))
                        markMovedByName(id->name, id->span.begin);
                    consumed = true;
                } else if (exprIsOwnedFieldRead(*a.value)) {
                    // §28.3 / §28.8.1 (#398): `owned obj.f` on a bare owned-
                    // field read is still a borrow (only `move obj.f` reads the
                    // field as owned). Report; there is no local to mark.
                    checkOwnedArgSource(*a.value, a.value->span.begin);
                    consumed = true;
                } else if (checkOwnedArgOtherBorrow(*a.value)) {
                    // §28.3 / §28.11 (#407): `owned coll[i]` / `owned obj.f` is a
                    // borrow under an explicit `owned` arg modifier — still
                    // rejected (only `move`/`new`/owned-call). (Bare `this` was
                    // handled by the identifier branch above.)
                    consumed = true;
                }
            } else if (auto* un = dynamic_cast<const ast::UnaryExpr*>(
                           a.value.get())) {
                if (un->op == "move") {
                    if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(
                            un->operand.get())) {
                        // §28.8 (#395): `move b` of a borrow is not an owned
                        // value — reject rather than silently consuming it.
                        if (!checkOwnedArgSource(*a.value, id->span.begin))
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
                                // §28.8.1, #897 #898: `M(move this.field)`.
                                recordFieldMove(ma->member, un->span.begin);
                                consumed = true;
                            }
                        }
                    }
                }
            }
            // §28.8 implicit transfer: an arg flowing into an owned formal —
            // a user-declared `owned` param (isImplicitOwned) or a built-in
            // owning-collection owned-sink method (#395, builtinOwnedSinkArg,
            // which has no AST declaration for isImplicitOwned to read).
            const bool ownedSink =
                isImplicitOwned(call->args, pos, call->callee.get()) ||
                builtinOwnedSinkArg(call->callee.get(), pos);
            // (A `move id` argument — `coll.Add(move b)` — was already handled
            // by the `move` arm above, which checks the source for every call;
            // it reaches here already consumed. The bare-id form is handled
            // here.)
            if (!consumed && a.modifier.empty() && ownedSink) {
                if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(
                        a.value.get())) {
                    // §28.8 (#395): reject a borrow flowing into the owned
                    // formal; otherwise record the implicit move.
                    if (!checkOwnedArgSource(*a.value, id->span.begin))
                        markMovedByName(id->name, id->span.begin);
                    consumed = true;
                } else if (exprIsOwnedFieldRead(*a.value)) {
                    // §28.3 / §28.8.1 (#398): a bare owned-field read implicitly
                    // flowing into an owned sink is a borrow, not an owned
                    // transfer (only `move obj.f` is owned). Reject.
                    checkOwnedArgSource(*a.value, a.value->span.begin);
                    consumed = true;
                } else if (checkOwnedArgOtherBorrow(*a.value)) {
                    // §28.3 / §28.11 (#407): a bare `coll[i]` indexer-get or a
                    // non-`owned` field read `obj.f` flowing into the owned sink
                    // is a borrow (it would be double-disposed). Bare `this`
                    // (#403) is handled by the identifier branch above.
                    consumed = true;
                }
            }
            if (!consumed) {
                // §30.5 / §30.4 (#834): an `await <bound owned `OwnedTask`
                // local>` ARGUMENT consumes its operand task local — an
                // `OwnedTask<T>` is single-consumer and the await disposes it
                // (the implicit task-disposal IS the discharge). This is the
                // §29.2 `for var task in src.Drain() do results.Add(await task)
                // end` drain-and-await pattern (stdlib §2.13 `WhenAllOwned<T>`):
                // the owned iteration variable `task` is discharged by the await
                // inside the `Add(owned item)` owned-consuming context. Without
                // recording the consume here the await arg only reached
                // checkRead (which does not consume), so `task` was wrongly
                // reported as dropped without disposal at the loop's end. Gated
                // on the operand's STATIC TYPE being `OwnedTask` rather than on
                // awaitTaskKind == PatternB, which cannot prove the result type
                // argument T is a concrete class inside a generic body; the
                // operand consume does not depend on the result kind.
                // recordAwaitOperandConsume itself acts only on a bare owned
                // local, so a borrow/parameter operand is unaffected.
                // // per §30.5, §30.4, §29.2
                if (auto* un = dynamic_cast<const ast::UnaryExpr*>(
                        a.value.get());
                    un && isAsync_ && un->op == "await" && un->operand &&
                    operandIsOwnedTaskType(*un->operand)) {
                    recordAwaitOperandConsume(*un->operand, un->span.begin);
                }
                // #835 / §28.8: an argument that is itself a `new`/call (a nested
                // producer, e.g. `f(new Box(_c))`) must have ITS OWN owned-argument
                // transfers checked — recurse so a borrow flowing into the inner
                // producer's `owned` parameter is rejected, exactly as a top-level
                // `new Box(_c)` is. Otherwise a plain read.
                else if (dynamic_cast<const ast::NewExpr*>(a.value.get()) ||
                    dynamic_cast<const ast::CallExpr*>(a.value.get()))
                    handleDischarge(*a.value);
                else
                    checkRead(*a.value, /*selfTarget=*/false);
            }
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
        // §28.2 / §28.3 (#860): a null-conditional dispose `local?.Dispose()`
        // parses as a CallExpr whose callee is a NullCondMemberExpr
        // (`local ?. Dispose`). It is the canonical null-safe discharge of a
        // conservatively-Owned local (e.g. an `out owned` item that is null on
        // the Try* false path): `?.` no-ops when null and disposes otherwise.
        // Recognise it the same as a plain `.Dispose()`. // per §28.2, §28.3
        if (auto* nm = dynamic_cast<const ast::NullCondMemberExpr*>(
                call->callee.get())) {
            if (nm->member == "Dispose") {
                if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(
                        nm->target.get())) {
                    markDisposedByName(id->name, id->span.begin);
                }
            }
        }
        // §28.3 / §28.11 (#860): model the caller-side owned-OUT transfer for a
        // bare-statement call `s.TryPop(out owned item);` (the initializer /
        // assignment-RHS forms route through recordMovesInArgs instead). After
        // the owned-IN consumption above, mark each `out owned` local Owned.
        markOutOwnedArgs(call->args, call->callee.get());
        return;
    }
    if (auto* un = dynamic_cast<const ast::UnaryExpr*>(&e)) {
        if (un->op == "move") {
            // §28.8.1 (#1125): unwrap a force-unwrap `move x!` to the underlying
            // field/local reference — `move _slot!` is a move of `_slot`.
            const ast::Expr* mop = lookThroughForceUnwrap(un->operand.get());
            if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(mop)) {
                // §28.8.1 / #825: a bare `move _field` (in return/assignment/
                // expr position) is the same operation as `move this._field` —
                // it leaves the field moved-from. Check via sema symbol before
                // falling through to the local-move path.
                if (exprIsOwnedFieldRead(*mop)) {
                    // §28.8.1, #897 #898: check-then-mark.
                    recordFieldMove(id->name, un->span.begin);
                    return;
                }
                markMovedByName(id->name, id->span.begin);
                return; // don't re-check the now-moved-from operand
            }
            // Slice O8: `move this.field` — mark the field as
            // moved-from. The field must be reassigned before
            // method exit.
            if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(mop)) {
                if (auto* tid =
                        dynamic_cast<const ast::IdentifierExpr*>(
                            ma->target.get())) {
                    if (tid->name == "this") {
                        // §28.8.1, #897 #898: qualified `move this.field`.
                        recordFieldMove(ma->member, un->span.begin);
                        return;
                    }
                }
            }
        }
        // Async-readiness 3c (§30.5 line ~6110): a Pattern B `await <bound
        // owned task local>` consumes its operand task local. Record the
        // consume before the generic read so a straight-line second await of
        // the same local is a read-after-consume (the diagnostic is emitted by
        // recordAwaitOperandConsume, not by the read). This handles the
        // bare-statement and nested-expression await positions; the
        // owned-consuming binding positions are handled at the binding site
        // (handleLocalDecl / handleAssignment / handleReturn). // per §30.5
        if (isAsync_ && un->op == "await" && un->operand &&
            awaitTaskKind(*un) == AwaitTaskKind::PatternB) {
            recordAwaitOperandConsume(*un->operand, un->span.begin);
            return;  // operand was the task local; consume is its handling
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
                    // §28.8 (#395): an `owned`-modified arg must be owned.
                    if (!checkOwnedArgSource(*a.value, id->span.begin))
                        markMovedByName(id->name, id->span.begin);
                    consumed = true;
                } else if (exprIsOwnedFieldRead(*a.value)) {
                    // §28.3 / §28.8.1 (#398): `owned obj.f` of a bare owned-
                    // field read is a borrow (only `move obj.f` is owned).
                    checkOwnedArgSource(*a.value, a.value->span.begin);
                    consumed = true;
                } else if (checkOwnedArgOtherBorrow(*a.value)) {
                    // §28.3 / §28.11 (#407): `owned coll[i]` / `owned obj.f` is a
                    // borrow (bare `this` was handled by the identifier branch).
                    consumed = true;
                }
            } else if (auto* un = dynamic_cast<const ast::UnaryExpr*>(a.value.get())) {
                if (un->op == "move") {
                    if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(un->operand.get())) {
                        // §28.8 (#395): `move b` of a borrow is not an owned value.
                        if (!checkOwnedArgSource(*a.value, id->span.begin))
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
                    // §28.8 (#395): reject a borrow flowing into the owned formal.
                    if (!checkOwnedArgSource(*a.value, id->span.begin))
                        markMovedByName(id->name, id->span.begin);
                    consumed = true;
                } else if (exprIsOwnedFieldRead(*a.value)) {
                    // §28.3 / §28.8.1 (#398): a bare owned-field read flowing
                    // into an owned ctor param is a borrow, not a transfer.
                    checkOwnedArgSource(*a.value, a.value->span.begin);
                    consumed = true;
                } else if (checkOwnedArgOtherBorrow(*a.value)) {
                    // §28.3 / §28.11 (#407): a bare `coll[i]` / `obj.f` flowing
                    // into an owned ctor param is a borrow, not a transfer — the
                    // double-dispose hazard (findings 18, 20). (Bare `this` was
                    // handled by the identifier branch above.)
                    consumed = true;
                }
            }
            if (!consumed) {
                // #835 / §28.8: a constructor argument that is itself a `new`/call
                // (`new Outer(new Box(_c))`) must have ITS OWN owned-argument
                // transfers checked — recurse so a borrow flowing into the inner
                // producer's `owned` parameter is rejected, exactly as a top-level
                // `new Box(_c)` is (without this the inner producer's args were
                // only `checkRead`, so the borrow→owned violation was a silent
                // accept → double-free / SIGSEGV).
                if (dynamic_cast<const ast::NewExpr*>(a.value.get()) ||
                    dynamic_cast<const ast::CallExpr*>(a.value.get()))
                    handleDischarge(*a.value);
                else
                    checkRead(*a.value, /*selfTarget=*/false);
            }
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
    // §2.5 / §2.6 / §6.1 / §28 (issue #434): value-type builtins
    // (DateTime, TimeSpan, Guid, CancellationToken) carry kind==Class so
    // that sema/codegen member resolution works, but they are scalars with
    // no heap allocation and no disposal obligation. Exclude them from the
    // ownership-tracking set. // per §28
    if (rt->declaration->isValueBuiltin) return false;
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
            // §28.2 v0.131: emit a compile-time warning (not an error) when a
            // local is used after an explicit Dispose() that dominates the use
            // site on every path — the statically-decidable subset of
            // use-after-dispose.  "Should" in the spec maps to Warning severity
            // (the runtime checked mode, §28.2 checked mode, handles the error
            // path).  If we have the dispose-site location, emit a Note pointing
            // at it so the programmer can see both sites in a single diagnostic
            // sequence.
            report(
                Severity::Warning, id->span.begin,
                std::string("use of `") + std::string(id->name) +
                "` after a dominating explicit Dispose() call — "
                "use-after-dispose is undefined behaviour in unchecked mode "
                "and raises DisposedAccessException in checked mode (§28.2)");
            if (st->explicitlyDisposed && st->explicitDisposeLoc.line != 0) {
                report(
                    Severity::Note, st->explicitDisposeLoc,
                    std::string("explicit Dispose() of `") +
                    std::string(id->name) + "` was here");
            }
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
    switch (rt->kind) {
        case sema::TypeRef::Kind::Primitive:
            // §28.9 / §28.3: string and Object are runtime-managed reference types
            // whose lifetime is not tied to any single owner — returning one from a
            // borrow is safe (the spec's "runtime-managed returns" exception).
            // All other primitives are value types and are also never borrows.
            // So ALL primitive reads are non-borrowable.
            return false;
        case sema::TypeRef::Kind::Function:
            // §28.9: function values (closures, method references) are
            // runtime-managed — the function object's lifetime is managed by the
            // runtime, not by the caller's scope. Not a borrow.
            return false;
        case sema::TypeRef::Kind::Named:
            if (!rt->declaration) return false;  // generic param → not conservative
            // §2.5 / §2.6 / §6.1 / §28 (issue #434): value-type builtins are
            // scalars/structs — they are not reference types and not borrowable.
            if (rt->declaration->isValueBuiltin) return false;
            // Only class and interface references are borrow-bearing.
            // NOTE: nullable class/interface refs are also borrow-bearing — if the
            // nullable flag is set on a Named class type, we must still return true
            // to catch nullable-reference borrows.  But nullable primitives/records
            // are NOT borrows, so the old unconditional `if (rt->nullable) return
            // true` was over-broad; the nullable check now lives here only.
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
    // #791 / §28.9 / §27.6: a method/property REFERENCE `recv.M` (a member access
    // resolving to a method or property, used as a function value) captures `recv`
    // as a BORROW in its closure cell — `recv.GetN` ≡ `() => recv.GetN()`. It is
    // therefore a borrow of the receiver, NOT a value copy, even though its type
    // is a function. (isBorrowableType is false for a function type, treating a
    // bare function pointer as a value — correct for a non-capturing lambda / free
    // function, but a member reference always captures its receiver.) So if the
    // borrow's source (the receiver root) is a scope-bound local, returning or
    // storing the reference lets the borrow outlive its source (a UAF at the
    // deferred call), which the escape checks must catch.
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
        (void)ma;
        if (const sema::TypeRef* t = ctx_.exprType(e)) {
            if (t->kind == sema::TypeRef::Kind::Function) {
                sema::Symbol* sym = ctx_.exprSymbol(e);
                // A METHOD reference always captures the receiver.
                if (dynamic_cast<const sema::MethodSymbol*>(sym))
                    return true;
                // A PROPERTY reference captures the receiver too, but only the
                // PROMOTED form (a non-function property used as a function value,
                // §27.6) — distinguish it from reading a property whose VALUE type
                // is itself a function (a value read, not a capturing ref) by
                // checking the property's declared type is not a function.
                if (auto* ps = dynamic_cast<const sema::PropertySymbol*>(sym)) {
                    if (ps->isIndexer) return true;  // conservative
                    bool declIsFunction = false;
                    if (auto* pd =
                            dynamic_cast<const ast::PropertyDecl*>(ps->decl)) {
                        if (pd->type) {
                            const sema::TypeRef* dt = ctx_.resolvedType(*pd->type);
                            declIsFunction =
                                dt && dt->kind == sema::TypeRef::Kind::Function;
                        }
                    }
                    if (!declIsFunction) return true;
                }
            }
        }
    }
    // §27.7 (#1136): calling a function VALUE (`h._fn()` where `_fn: () -> int`)
    // produces the function's declared RETURN type — a value-typed return (int,
    // a record, an enum) is a copy, never a borrow of the receiver. Sema does not
    // always type the result of a function-value invocation, so the typed branch
    // below misses it and the conservative default would wrongly flag the call as
    // a borrow escape of the `using var` receiver. Resolve the callee's function
    // type and gate on its return type (a class/borrow-typed return stays a
    // borrow candidate and is still checked). Checked before the
    // possibly-untyped `exprType(e)` branch.
    if (auto* call = dynamic_cast<const ast::CallExpr*>(&e)) {
        if (call->callee) {
            if (const sema::TypeRef* ct = ctx_.exprType(*call->callee))
                if (ct->kind == sema::TypeRef::Kind::Function && ct->funcReturn)
                    return isBorrowableType(ct->funcReturn);
        }
    }
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
    // §23.4 / §28.3 (FINDING [6]) defense-in-depth: a USER-DEFINED indexer read
    // `obj[i]` whose element type is value-typed is a copy, not a borrow of the
    // target (the same rule as a value-typed field/method projection above).
    // Sema now types such an IndexExpr (so the typed branch above usually
    // decides it), but if it is still untyped here, resolve the indexer's
    // declared return type directly and gate on isBorrowableType: a value-typed
    // element (int/record/enum/…) is a copy → not a borrow; a CLASS/borrow-typed
    // element (e.g. an OwnedArray<R> slot, §28.3) stays a borrow and is still
    // rejected. Built-in collection indexers carry a null `declaration`, so they
    // do not reach this user-type branch — their element typing is handled in
    // sema's IndexExpr path and consumed by the typed branch above.
    if (auto* ix = dynamic_cast<const ast::IndexExpr*>(&e)) {
        if (ix->target) {
            if (const sema::TypeRef* tt = ctx_.exprType(*ix->target)) {
                if (tt->kind == sema::TypeRef::Kind::Named && tt->declaration) {
                    for (const sema::Symbol* m : tt->declaration->members) {
                        auto* ps = dynamic_cast<const sema::PropertySymbol*>(m);
                        if (!ps || !ps->isIndexer) continue;
                        auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl);
                        if (pd && pd->type)
                            return isBorrowableType(ctx_.resolvedType(*pd->type));
                        break;
                    }
                }
            }
        }
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
            // §31.1 (issue #392): a returned/escaping closure capturing a
            // catch-bound exception variable. The caught exception is a borrow
            // whose lifetime is the catch arm body; capturing it does not
            // transfer ownership, and a closure that escapes the arm outlives
            // the exception (the runtime disposes it after the arm completes)
            // — a use-after-free. The catch binding is not in `states_`, so it
            // is recognized here directly. The binding is always a reference to
            // a heap exception instance, so no value-typed gate is needed.
            if (isCatchBorrow(sym)) {
                report(
                    Severity::Error, id->span.begin,
                    std::string("returned closure captures the caught "
                                "exception `") +
                    std::string(sym->name) +
                    "` — a caught exception is a borrow whose lifetime is the "
                    "catch arm body (the runtime disposes it after the arm "
                    "completes); a closure that escapes the arm would outlive "
                    "it (use-after-free). (§31.1; copy the data you need out "
                    "of it into an owned value the closure can capture "
                    "instead)");
                return;  // one diagnostic per return is enough
            }
            // §27.5 / §28.6 (#1217): a `ref` or `out` parameter is a pointer
            // into the CALLER's stack frame (§22.3). The pointer is valid only
            // for the duration of this method call. A closure that captures a
            // ref/out param by pointer and ESCAPES (is returned) will hold a
            // dangling pointer once the call returns — a use-after-free on the
            // next invocation. Reject here. A non-escaping ref/out-capturing
            // closure invoked locally (per #1207) is not in this return path
            // and remains accepted.
            if (isRefOutParam(sym)) {
                auto* ps = dynamic_cast<sema::ParameterSymbol*>(sym);
                report(
                    Severity::Error, id->span.begin,
                    std::string("returned closure captures `") +
                    std::string(sym->name) +
                    "` which is a `" + std::string(ps->modifier) + "` parameter"
                    " — a ref/out parameter is a pointer into the caller's stack"
                    " frame; a closure that captures it and escapes the method"
                    " holds a dangling pointer once the call returns"
                    " (use-after-free). (§27.5 / §28.6 / #1217; pass the value"
                    " by copy into the closure instead, e.g."
                    " `var snap := " +
                    std::string(sym->name) + "; () => snap`)");
                return;  // one diagnostic per return is enough
            }
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
                // §2.5 / §2.6 / §6.1 / §28 (issue #434): value-type builtins
                // are scalars/structs; they never carry a borrow. // per §28
                if (ft->declaration->isValueBuiltin) return false;
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

bool MethodOwnership::isCatchBorrow(sema::Symbol* sym) const {
    return sym && catchBorrows_.count(sym) != 0;
}

bool MethodOwnership::isEscapeBoundLocal(sema::Symbol* sym) const {
    // §28.3 owned scope-bound local OR §31.1 catch-bound exception variable.
    // Both are scope-bound borrows that may not outlive their scope: the owned
    // local is disposed at scope end; the caught exception is disposed by the
    // runtime when the arm completes. A borrow of either may not escape.
    return isOwnedTrackedLocal(sym) || isCatchBorrow(sym);
}

bool MethodOwnership::isRefOutParam(sema::Symbol* sym) const {
    // §27.5 / §28.6 (#1217): a plain ref/out parameter (not "out owned" —
    // that is an ownership-transfer parameter handled separately). A ref/out
    // param is a pointer into the CALLER's stack frame; capturing it in a
    // closure that escapes the call produces a dangling pointer. // per §22.3
    if (!sym) return false;
    auto* ps = dynamic_cast<sema::ParameterSymbol*>(sym);
    return ps && (ps->modifier == "ref" || ps->modifier == "out");
}

// §27.5 / §28.6 (#1217): when a lambda is stored into a FIELD or written
// through an OUT-PARAM assignment target, and the lambda captures a ref/out
// parameter, the captured pointer lives on the caller's stack; the field or
// out-param location outlives the call — a use-after-free on the next
// invocation. Reports the first offending ref/out capture found. // per §27.5
void MethodOwnership::checkRefOutLambdaCaptureOnStore(
    const ast::AssignmentStmt& as) {
    if (as.op != ":=" || !as.target || !as.value) return;

    // Identify whether the target is an escaping location (field or out-param).
    // Mirrors the Dest classification in checkBorrowStoreEscape, restricted to
    // the two locations that truly outlive the method call.
    enum class Dest { None, Field, OutParam };
    Dest dest = Dest::None;
    std::string_view destName;

    if (dynamic_cast<const ast::MemberAccessExpr*>(as.target.get())) {
        // `obj.f := …` / `this.f := …` — the field outlives the method.
        dest = Dest::Field;
        if (auto* ma =
                dynamic_cast<const ast::MemberAccessExpr*>(as.target.get()))
            destName = ma->member;
    } else if (const auto* id =
                   dynamic_cast<const ast::IdentifierExpr*>(as.target.get())) {
        sema::Symbol* sym = ctx_.exprSymbol(*id);
        if (dynamic_cast<sema::FieldSymbol*>(sym)) {
            // Bare unqualified field name (implicit `this._field` form, §1082).
            dest = Dest::Field;
            destName = id->name;
        } else if (auto* ps = dynamic_cast<sema::ParameterSymbol*>(sym)) {
            if (ps->modifier == "out" || ps->modifier == "ref" ||
                ps->modifier == "out owned") {
                // `p := …` — the out/ref param's storage is the CALLER's frame,
                // which also outlives this method.
                dest = Dest::OutParam;
                destName = id->name;
            }
        }
    }
    if (dest == Dest::None) return;

    // Find lambdas in the RHS.
    std::vector<const ast::LambdaExpr*> lambdas;
    std::function<void(const ast::Expr&)> findLambdas =
        [&](const ast::Expr& x) {
            if (auto* lam = dynamic_cast<const ast::LambdaExpr*>(&x)) {
                lambdas.push_back(lam);
                return;
            }
            if (auto* cd = dynamic_cast<const ast::ConditionalExpr*>(&x)) {
                if (cd->thenValue) findLambdas(*cd->thenValue);
                if (cd->elseValue) findLambdas(*cd->elseValue);
                return;
            }
        };
    findLambdas(*as.value);
    if (lambdas.empty()) return;

    for (const ast::LambdaExpr* lam : lambdas) {
        std::vector<const ast::IdentifierExpr*> ids;
        collectLambdaIdentifiers(*lam, ids);
        for (const ast::IdentifierExpr* id : ids) {
            sema::Symbol* sym = ctx_.exprSymbol(*id);
            if (!isRefOutParam(sym)) continue;
            auto* ps = dynamic_cast<sema::ParameterSymbol*>(sym);
            if (dest == Dest::Field) {
                report(
                    Severity::Error, id->span.begin,
                    std::string("closure stored in field `") +
                    std::string(destName) +
                    "` captures `" + std::string(sym->name) +
                    "` which is a `" + std::string(ps->modifier) + "` parameter"
                    " — a ref/out parameter is a pointer into the caller's stack"
                    " frame; storing the closure in a field lets it outlive the"
                    " call and dereference a dangling pointer (use-after-free)."
                    " (§27.5 / §28.6 / #1217; pass the value by copy into the"
                    " closure instead, e.g. `var snap := " +
                    std::string(sym->name) + "; () => snap`)");
            } else {
                report(
                    Severity::Error, id->span.begin,
                    std::string("closure written through `") +
                    std::string(destName) +
                    "` captures `" + std::string(sym->name) +
                    "` which is a `" + std::string(ps->modifier) + "` parameter"
                    " — a ref/out parameter is a pointer into the caller's stack"
                    " frame; writing the closure through an out/ref parameter"
                    " lets it outlive the call and dereference a dangling pointer"
                    " (use-after-free)."
                    " (§27.5 / §28.6 / #1217; pass the value by copy instead)");
            }
            return;  // one diagnostic per assignment is enough
        }
    }
}

sema::Symbol* MethodOwnership::borrowSourceForExpr(const ast::Expr& rhs) {
    // §31.1 (issue #392) — a re-borrow of a catch-bound exception variable.
    // `var b := ex;` binds `b` to the in-flight exception's borrow; the source
    // is the catch binding `ex`. This is checked BEFORE the §28.3 borrow-vs-
    // copy gate below: a built-in exception type resolves to a Named TypeRef
    // with no declaration (it has no user TypeDecl), which `isBorrowableType`
    // treats as a value type — so `returnedExprMayBorrow(ex)` is false even
    // though the binding is always a reference to a heap exception instance
    // (§31.1: `new SomeException(...)` produces an owned class instance). We
    // therefore recognize a bare catch-binding identifier directly, without
    // routing through that gate. A value/string projection off the exception
    // (`ex.Message`) is a MemberAccessExpr whose root resolves to `ex` but is
    // a copy, not the instance — it is NOT matched here (only the bare
    // identifier is), so copying data out of the exception stays legal.
    //
    // The same bare-identifier branch also follows a multi-hop chain rooted at
    // a catch binding (`var b := ex; var c := b;`): a holder local whose
    // tracked source is a catch borrow re-borrows the same exception, so
    // re-binding it (or escaping it) re-borrows `ex`. Both legs bypass the
    // `returnedExprMayBorrow` gate below because a built-in exception type is
    // mis-classified as a value type by `isBorrowableType` (no user TypeDecl);
    // the catch binding and its re-borrows are always reference borrows of the
    // heap exception instance. // per §31.1
    if (const auto* id = dynamic_cast<const ast::IdentifierExpr*>(&rhs)) {
        if (sema::Symbol* sym = ctx_.exprSymbol(*id)) {
            if (isCatchBorrow(sym)) return sym;
            auto it = states_.find(sym);
            if (it != states_.end() && it->second.source &&
                isCatchBorrow(it->second.source->sym))
                return it->second.source->sym;
        }
    }
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
                        isEscapeBoundLocal(it->second.source->sym))
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
                if (isEscapeBoundLocal(sym)) return sym;
            }
        }
        // Shape 2b (§31.1 / issue #430) — a CLASS construction (not a record)
        // whose constructor takes a catch borrow through a NON-OWNED parameter.
        // The constructed class instance may store the borrow in a non-owned
        // field (§28.3 forbids this but the ctor is not analysed
        // interprocedurally here). Binding it to a holder local propagates the
        // source so a later throw or return of the holder is caught by the
        // throw/return-time checks. Restricted to:
        //   (a) class types only (records are handled above; primitives etc. carry
        //       no borrow obligation);
        //   (b) BARE identifier arguments only — a value projection like
        //       `ex.Message` (MemberAccessExpr) is a copy, not the borrow, and
        //       must not propagate the source;
        //   (c) CATCH BORROWS only (not owned scope-bound locals): an owned local
        //       passed to a class ctor is a legitimate ownership-transfer pattern
        //       (the ctor's owned param takes the local's ownership); only the
        //       catch binding, which is never owned by the caller, triggers this.
        //   (d) NON-OWNED parameter positions only — owned-param positions are
        //       handled at construction time by checkOwnedArgSource (#404) and
        //       propagating a borrow source there too would be redundant.
        // // per §31.1, §28.3
        // §2.5/§2.6/§6.1/§28 (issue #434): value-type builtins are scalars;
        // they cannot hold a borrow of a catch variable. // per §28
        if (rt && rt->kind == sema::TypeRef::Kind::Named && rt->declaration &&
            !rt->declaration->isValueBuiltin &&
            rt->declaration->kind == ast::TypeDeclKind::Class) {
            // Resolve the constructor to identify owned vs non-owned params.
            const ast::ConstructorDecl* cd2 = nullptr;
            {
                std::size_t nArgs = ne->args.size();
                for (sema::Symbol* m : rt->declaration->members) {
                    if (auto* cs = dynamic_cast<sema::ConstructorSymbol*>(m)) {
                        auto* d = dynamic_cast<const ast::ConstructorDecl*>(
                            cs->decl);
                        if (d && d->params.size() == nArgs) {
                            cd2 = d;
                            break;
                        }
                    }
                }
            }
            std::size_t pos = 0;
            for (const ast::CallArgument& a : ne->args) {
                if (!a.value) { ++pos; continue; }
                const bool paramIsOwned =
                    cd2 && pos < cd2->params.size() &&
                    (cd2->params[pos].modifier == "owned" ||
                     cd2->params[pos].modifier == "out owned");
                if (!paramIsOwned) {
                    const auto* id =
                        dynamic_cast<const ast::IdentifierExpr*>(a.value.get());
                    if (id) {
                        sema::Symbol* sym = ctx_.exprSymbol(*id);
                        if (isCatchBorrow(sym)) return sym;
                    }
                }
                ++pos;
            }
        }
    }
    // Shape 3 — a closure capturing an Owned local (§28.9) or a catch-bound
    // exception variable (§31.1). Capturing an owned class-typed local — or
    // the in-flight exception — does not transfer ownership; the capture is a
    // borrow whose source is the enclosing scope (for the catch binding, the
    // arm body). Binding the closure to a holder local propagates that source,
    // so a later escape of the holder is recognized.
    if (const auto* lam = dynamic_cast<const ast::LambdaExpr*>(&rhs)) {
        std::vector<const ast::IdentifierExpr*> ids;
        collectLambdaIdentifiers(*lam, ids);
        for (const ast::IdentifierExpr* id : ids) {
            sema::Symbol* sym = ctx_.exprSymbol(*id);
            if (isEscapeBoundLocal(sym)) return sym;
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
    // The source must still be a live scope-bound obligation at the return
    // point: an owned scope-bound local (§28.9 / §9.7) or a catch-bound
    // exception variable (§31.1, issue #392). If an owned source was already
    // disposed / moved out, there is no live borrow to dangle (and a separate
    // diagnostic would have covered misuse); a catch binding is always live
    // within its arm.
    if (!isEscapeBoundLocal(st->source->sym)) return;
    std::string_view srcName = st->source->sym->name;
    if (isCatchBorrow(st->source->sym)) {
        // §31.1: the holder re-borrows the in-flight exception. Returning it
        // lets the exception instance escape the arm whose completion the
        // runtime disposes it after — a use-after-free.
        report(
            Severity::Error, id->span.begin,
            std::string("returned `") + std::string(id->name) +
            "` carries a borrow of the caught exception `" +
            std::string(srcName) +
            "` — a caught exception is a borrow whose lifetime is the catch "
            "arm body (the runtime disposes it after the arm completes), so "
            "it may not escape the arm (§31.1; copy the data you need out of "
            "it — e.g. its message — into an owned value instead)");
        return;
    }
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

// ---------------------------------------------------------------------------
// §31.1 (issue #392) — catch-bound exception variables are scope-bound borrows.
// ---------------------------------------------------------------------------

void MethodOwnership::collectCatchBorrowsStmt(const ast::Stmt& s) {
    // Recurse into the control-flow forms that can lexically contain a try
    // statement (mirrors collectLocalDeclsStmt's structure). The ownership
    // CFG flattens these, but here we only need to find the catch arms and
    // their binding symbols, so a syntactic walk is sufficient and simplest.
    if (auto* ifs = dynamic_cast<const ast::IfStmt*>(&s)) {
        for (const auto& bs : ifs->ifBranch.body) if (bs) collectCatchBorrowsStmt(*bs);
        for (const auto& el : ifs->elifBranches)
            for (const auto& bs : el.body) if (bs) collectCatchBorrowsStmt(*bs);
        for (const auto& bs : ifs->elseBody) if (bs) collectCatchBorrowsStmt(*bs);
        return;
    }
    if (auto* ws = dynamic_cast<const ast::WhileStmt*>(&s)) {
        for (const auto& bs : ws->body) if (bs) collectCatchBorrowsStmt(*bs);
        return;
    }
    if (auto* fs = dynamic_cast<const ast::ForStmt*>(&s)) {
        for (const auto& bs : fs->body) if (bs) collectCatchBorrowsStmt(*bs);
        return;
    }
    if (auto* dob = dynamic_cast<const ast::DoEndBlock*>(&s)) {
        for (const auto& bs : dob->body) if (bs) collectCatchBorrowsStmt(*bs);
        return;
    }
    if (auto* ts = dynamic_cast<const ast::TryStmt*>(&s)) {
        for (const auto& bs : ts->body) if (bs) collectCatchBorrowsStmt(*bs);
        for (const auto& arm : ts->catches) {
            // §31.2.1 binding pattern `var ex: T`. The catch binding's
            // LocalSymbol was created by ExprTypecheck anchored on the arm's
            // body node (`bnd->decl = arm.body.get()`). Find it by that anchor.
            if (arm.hasBinding && !arm.varName.empty() && arm.body) {
                for (const auto& sym : ctx_.symbols()) {
                    auto* ls = dynamic_cast<sema::LocalSymbol*>(sym.get());
                    if (ls && ls->decl == arm.body.get() &&
                        ls->name == arm.varName) {
                        catchBorrows_.insert(ls);
                        break;
                    }
                }
            }
            // A catch arm body may itself contain a nested try.
            if (arm.body) collectCatchBorrowsStmt(*arm.body);
        }
        for (const auto& bs : ts->finallyBody) if (bs) collectCatchBorrowsStmt(*bs);
        return;
    }
}

void MethodOwnership::collectCatchBorrows(
    const std::vector<std::unique_ptr<ast::Stmt>>& body) {
    for (const auto& s : body) if (s) collectCatchBorrowsStmt(*s);
}

// §28.3 / §28.9 (#861): record each LocalDecl's block-nesting depth. The method
// body is depth 0; each nested do-end / if-branch / loop / try-arm body adds one.
// Mirrors collectCatchBorrowsStmt's statement coverage so no scope-introducing
// form is missed. A consistent over-count (e.g. a catch arm whose body is itself
// a do-end) is harmless: only the monotonic ordering along a single nesting path
// matters, and that is preserved. We do NOT recurse into lambda expression
// bodies — locals declared inside a closure are closure-scoped, not method-scope
// locals, and are never the target/source of this method's escape check.
void MethodOwnership::collectLocalDepthsStmt(const ast::Stmt& s, int depth) {
    if (auto* ld = dynamic_cast<const ast::LocalDecl*>(&s)) {
        localDepth_[ld] = depth;
        return;
    }
    if (auto* ifs = dynamic_cast<const ast::IfStmt*>(&s)) {
        for (const auto& bs : ifs->ifBranch.body)
            if (bs) collectLocalDepthsStmt(*bs, depth + 1);
        for (const auto& el : ifs->elifBranches)
            for (const auto& bs : el.body)
                if (bs) collectLocalDepthsStmt(*bs, depth + 1);
        for (const auto& bs : ifs->elseBody)
            if (bs) collectLocalDepthsStmt(*bs, depth + 1);
        return;
    }
    if (auto* ws = dynamic_cast<const ast::WhileStmt*>(&s)) {
        for (const auto& bs : ws->body)
            if (bs) collectLocalDepthsStmt(*bs, depth + 1);
        return;
    }
    if (auto* fs = dynamic_cast<const ast::ForStmt*>(&s)) {
        for (const auto& bs : fs->body)
            if (bs) collectLocalDepthsStmt(*bs, depth + 1);
        return;
    }
    if (auto* dob = dynamic_cast<const ast::DoEndBlock*>(&s)) {
        for (const auto& bs : dob->body)
            if (bs) collectLocalDepthsStmt(*bs, depth + 1);
        return;
    }
    if (auto* ts = dynamic_cast<const ast::TryStmt*>(&s)) {
        for (const auto& bs : ts->body)
            if (bs) collectLocalDepthsStmt(*bs, depth + 1);
        for (const auto& arm : ts->catches)
            if (arm.body) collectLocalDepthsStmt(*arm.body, depth + 1);
        for (const auto& bs : ts->finallyBody)
            if (bs) collectLocalDepthsStmt(*bs, depth + 1);
        return;
    }
}

void MethodOwnership::collectLocalDepths(
    const std::vector<std::unique_ptr<ast::Stmt>>& body) {
    for (const auto& s : body) if (s) collectLocalDepthsStmt(*s, 0);
}

void MethodOwnership::checkCatchBorrowEscapesOnReturn(const ast::Expr& e) {
    // §31.1: `return ex;` where `ex` is the bare catch-bound exception
    // variable. Returning the binding (as `owned`, the only way a class
    // instance is returned) lets the in-flight exception instance escape the
    // arm whose completion the runtime disposes it after — a use-after-free.
    // Only the BARE identifier is flagged: a value/string projection off the
    // exception (`return ex.Message;`) is a copy, not the instance, and is the
    // spec's documented escape hatch ("copy the relevant data out of it").
    const auto* id = dynamic_cast<const ast::IdentifierExpr*>(&e);
    if (!id) return;
    sema::Symbol* sym = ctx_.exprSymbol(*id);
    if (!isCatchBorrow(sym)) return;
    report(
        Severity::Error, id->span.begin,
        std::string("caught exception `") + std::string(id->name) +
        "` may not be returned — a caught exception is a borrow whose lifetime "
        "is the catch arm body (the runtime disposes it after the arm "
        "completes), so the exception instance cannot escape the arm (§31.1; "
        "copy the data you need out of it — e.g. its message — into an owned "
        "value and return that, or re-raise with `throw;`)");
}

// ---------------------------------------------------------------------------
// §31.1 (issue #430) — catch borrow escaping via a NON-OWNED constructor
// parameter of an object that is THROWN or RETURNED from the catch arm.
//
// The existing #392/#404 checks already reject a catch borrow flowing into an
// `owned` field (directly or through an `owned` ctor param). The gap #430
// closes: a `new T(ex)` where T's constructor takes ex through a NON-OWNED
// borrow parameter — the ctor can store ex into a non-owned field (§28.3
// forbids this inside the ctor body too, but interprocedural analysis is not
// performed here). If the constructed object then ESCAPES the catch arm (via
// throw or return), ex may outlive the arm after which the runtime frees it.
//
// PRAGMATIC SOUND APPROXIMATION: reject when a catch borrow `ex` is passed
// as a BARE IDENTIFIER argument to a constructor at a NON-OWNED parameter
// position, AND the constructed object is in an escaping position (thrown or
// returned). The bare-identifier restriction avoids false positives on value-
// typed projections like `ex.Message` (a MemberAccessExpr, not a bare
// identifier); method-call results (`ex.Code()`) and other expression forms
// are similarly skipped. The non-owned-param restriction avoids double-
// reporting with the existing #404 `checkOwnedArgSource` path.
//
// This is slightly conservative (it would also reject a ctor that takes ex as
// a borrow but does not store it in a field), but that is sound under §28.3:
// "A borrowed local may not be assigned to any field of any object — neither
// owned nor non-owned", so the ctor body ALSO must not store ex in `_inner`.
// Both the ctor body violation and the throw-escape are jointly unsound, and
// #430 addresses the observable symptom at the escape site.
// ---------------------------------------------------------------------------

// §31.2.4 / #811 (v0.162): true when `n` names a built-in exception type.
// Mirrors isBuiltinExceptionTypeName in ExprTypecheck_Exprs.cpp (kept in sync).
static bool ownershipIsBuiltinExceptionName(std::string_view n) {
    return n == "Exception"              ||
           n == "ArgumentException"      ||
           n == "ArgumentNullException"  ||
           n == "ArithmeticException"    ||
           n == "OverflowException"      ||
           n == "DivideByZeroException"  ||
           n == "InvalidOperationException" ||
           n == "NotImplementedException"   ||
           n == "NullReferenceException"    ||
           n == "IndexOutOfRangeException"  ||
           n == "KeyNotFoundException"      ||
           n == "MatchFailureException"     ||
           n == "OperationCanceledException"||
           n == "TypeInitializationException" ||
           n == "FormatException"           ||
           n == "StackOverflowException"    ||
           n == "TimeoutException"          ||
           n == "IOException"               ||
           n == "FileNotFoundException"     ||
           n == "DirectoryNotFoundException"||
           n == "EndOfStreamException"      ||
           n == "PathTooLongException";
}

// §31.2.4 / #811 (v0.162): true when `rt` is a built-in exception type or a
// user class that (transitively) derives from one. Mirrors
// inheritsBuiltinException in ExprTypecheck_Exprs.cpp.
static bool ownershipIsExceptionClassRef(const sema::TypeRef* rt) {
    if (!rt || rt->kind != sema::TypeRef::Kind::Named) return false;
    if (!rt->declaration) {
        // Builtin exceptions have no sema TypeDecl — match by name.
        if (rt->astNode && rt->astNode->name && !rt->astNode->name->parts.empty())
            return ownershipIsBuiltinExceptionName(rt->astNode->name->parts.back());
        if (!rt->importedName.empty())
            return ownershipIsBuiltinExceptionName(rt->importedName);
        return false;
    }
    for (const sema::TypeSymbol* cur = rt->declaration; cur;) {
        if (cur->kind != ast::TypeDeclKind::Class) return false;
        if (auto* td = dynamic_cast<const ast::TypeDecl*>(cur->decl)) {
            for (const auto& bt : td->baseTypes) {
                if (!bt || !bt->name || bt->name->parts.empty()) continue;
                if (ownershipIsBuiltinExceptionName(bt->name->parts.back()))
                    return true;
            }
        }
        const sema::TypeSymbol* next = nullptr;
        for (const sema::TypeSymbol* b : cur->baseTypes) {
            if (b && b->kind == ast::TypeDeclKind::Class && b->decl) {
                next = b;
                break;
            }
        }
        cur = next;
    }
    return false;
}

sema::Symbol* MethodOwnership::catchBorrowAmongNonOwnedCtorArgs(
    const ast::NewExpr& ne) const {
    // Resolve the constructor declaration to identify owned vs non-owned params.
    const ast::ConstructorDecl* cd = nullptr;
    if (ne.type) {
        if (const auto* rt = ctx_.resolvedType(*ne.type)) {
            if (rt->declaration) {
                std::size_t nArgs = ne.args.size();
                for (sema::Symbol* m : rt->declaration->members) {
                    if (auto* cs = dynamic_cast<sema::ConstructorSymbol*>(m)) {
                        auto* d = dynamic_cast<const ast::ConstructorDecl*>(
                            cs->decl);
                        if (d && d->params.size() == nArgs) {
                            cd = d;
                            break;
                        }
                    }
                }
            }
        }
    }
    // §31.2.4 / #811 (v0.162): when the CONSTRUCTED type is itself an
    // exception, an exception-typed argument is the CAUSE position of the
    // snapshot-chaining constructor — "ex borrowed for the ctor call; snapshot
    // taken, instance not retained" (the spec's §31.2.4 wrap-and-rethrow
    // example is exactly `throw new DataAccessException("...", ex)` from a
    // catch arm). The builtin base ctor copies the chain and retains nothing;
    // a user subclass can only read or forward the borrow (§28.3 makes storing
    // a borrow in a field inexpressible), so the escape is not a dangle.
    const bool constructedIsException =
        ne.type && ownershipIsExceptionClassRef(ctx_.resolvedType(*ne.type));
    std::size_t pos = 0;
    for (const auto& a : ne.args) {
        if (!a.value) { ++pos; continue; }
        // Skip args destined for `owned` parameters — the existing #404
        // `checkOwnedArgSource` path already rejects those. We only add a
        // new diagnostic for NON-owned parameter positions, which the existing
        // check does not cover. // per §31.1, §28.8
        const bool paramIsOwned =
            cd && pos < cd->params.size() &&
            (cd->params[pos].modifier == "owned" ||
             cd->params[pos].modifier == "out owned");
        // §31.2.4 cause position: skip (see constructedIsException above).
        if (constructedIsException &&
            ownershipIsExceptionClassRef(ctx_.exprType(*a.value))) {
            ++pos;
            continue;
        }
        if (!paramIsOwned) {
            // Only flag BARE identifier arguments — a value/string projection
            // like `ex.Message` is a MemberAccessExpr, not a bare identifier,
            // so it is a copy of the value, not the exception borrow itself,
            // and must not be rejected. // per §31.1
            const auto* id =
                dynamic_cast<const ast::IdentifierExpr*>(a.value.get());
            if (id) {
                sema::Symbol* sym = ctx_.exprSymbol(*id);
                if (!sym) { ++pos; continue; }
                // Direct catch borrow: `new Wrap(ex)` where ex is the binding.
                if (isCatchBorrow(sym)) return sym;
                // Re-borrow holder: `var b := ex; new Wrap(b)`. The holder b
                // carries ex as its source (set by borrowSourceForExpr).
                auto it = states_.find(sym);
                if (it != states_.end() && it->second.source &&
                    isCatchBorrow(it->second.source->sym))
                    return it->second.source->sym;
            }
        }
        ++pos;
    }
    return nullptr;
}

void MethodOwnership::checkCatchBorrowEscapesViaNewExpr(
    const ast::NewExpr& ne, SourceLocation loc) {
    // §31.1 / #430: the NewExpr is in an ESCAPING position (thrown or
    // returned). If any non-owned-param argument is a catch borrow, the
    // constructed object carries that borrow into the escaped value →
    // use-after-free when the runtime disposes ex after the arm completes.
    sema::Symbol* src = catchBorrowAmongNonOwnedCtorArgs(ne);
    if (!src) return;
    report(
        Severity::Error, loc,
        std::string("caught exception `") + std::string(src->name) +
        "` may not be passed to a constructor of an object that escapes the "
        "catch arm — a caught exception is a borrow whose lifetime is the catch "
        "arm body (the runtime disposes it after the arm completes); the "
        "constructed object escaping the arm would leave any stored reference to "
        "it dangling (§31.1 / #430; copy the data you need out of the exception "
        "— e.g. its message — before constructing the wrapper, or re-raise with "
        "`throw;`)");
}

bool MethodOwnership::checkBorrowStoreEscape(const ast::AssignmentStmt& as) {
    if (as.op != ":=") return false;
    if (!as.target || !as.value) return false;

    // Identify the kind of escaping destination, and the diagnostic flavour.
    // Three destinations outlive a method-scope local:
    enum class Dest { None, Field, OutParam, OuterLocal };
    Dest dest = Dest::None;
    sema::LocalSymbol* targetLocal = nullptr;  // for the OuterLocal case (#861)
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
        } else if (dynamic_cast<sema::FieldSymbol*>(sym)) {
            // §28.3 (#1082): a BARE field name — an implicit `this._field`
            // instance field, or an unqualified static field — is a field
            // write, exactly like the qualified `this.f` / `obj.f` / `C.f`
            // MemberAccessExpr form above. The field's object (or the static
            // storage) outlives this method scope, so storing a borrow of a
            // scope-bound owned local in it is the same §28.3 lifetime
            // violation. Without this the bare form silently escaped the check
            // — a use-after-free of the disposed local (the qualified form was
            // correctly rejected, the unqualified one was not). The owned-field
            // move-on-read skip below (targetIsOwnedField) already recognises a
            // bare-identifier owned field, so a legal `_ownedField := r` is
            // still admitted.
            dest = Dest::Field;
        } else if (auto* ls = dynamic_cast<sema::LocalSymbol*>(sym)) {
            // §28.3 / §28.9 (#861): the target is a plain LOCAL. Assigning a
            // closure that captures an owned scope-bound local to a local in an
            // ENCLOSING block lets the closure outlive the captured local (a
            // use-after-dispose / SIGSEGV when the closure is called after the
            // inner block exits). Whether the target outlives the source is
            // decided by their block depths once `src` is known below — so
            // remember the candidate target here.
            targetLocal = ls;
        }
    }
    if (dest == Dest::None && !targetLocal) return false;

    // §28.3 / §28.8 (issue #402): a BARE read of an owned LOCAL stored into an
    // OWNED field is a move-on-read into an owned slot ("Reading an owned local
    // x in a position that requires an owned value … transfers ownership out of
    // x"), NOT a borrow store. This is the one case where checkBorrowStoreEscape
    // would otherwise wrongly flag a legal owned-field transfer (the RHS aliases
    // the owned local that is itself being moved). Restrict the skip to a bare
    // identifier directly resolving to an Owned-state tracked local: a re-borrow
    // alias (`var b := r; f.x := b;`) is Borrowed-state — still a borrow store,
    // still flagged below — and a catch-bound exception borrow is not in
    // `states_` at all, so the §31.1 store-into-owned-field diagnosis is
    // untouched. handleOwnedFieldAssignment records the move for the legal
    // transfer. // per §28.3, §28.8
    if (dest == Dest::Field && targetIsOwnedField(*as.target)) {
        if (const auto* rid =
                dynamic_cast<const ast::IdentifierExpr*>(as.value.get())) {
            if (const LocalOwnership* rst = lookupState(rid->name))
                if (rst->state == OwnershipState::Owned) return false;
        }
    }

    // The RHS must borrow an owned scope-bound LOCAL (the value that dies at
    // this scope's end). recordMovesIn has already run in handleAssignment, so
    // a `move`d source is now MovedFrom, not Owned, and is correctly not seen
    // as a live borrow source — `box.f := move r;` / `o := move r;` transfers,
    // it does not store a borrow.
    sema::Symbol* src = ownedLocalBorrowSource(*as.value);
    if (!src) return false;

    // §28.3 / §28.9 (#861): resolve a plain-local target into an OuterLocal
    // escape. Require (a) the RHS to be a closure (a LambdaExpr — a closure
    // BORROWS its captures, so this is a borrow store, never a move; gating on a
    // lambda avoids any move-vs-borrow ambiguity of a bare local read), and
    // (b) the target local's declaring block to strictly ENCLOSE the source's
    // (a smaller nesting depth). Both `targetLocal` and `src` are in lexical
    // scope at the assignment, so their blocks lie on a single nesting path and
    // the depths are comparable; the smaller depth is the longer-lived block. A
    // target at the same or a deeper block dies no later than the source, so it
    // is not an escape. If either depth is unknown (e.g. a for-loop variable —
    // not a LocalDecl statement), we conservatively do NOT flag it.
    if (dest == Dest::None && targetLocal) {
        std::function<bool(const ast::Expr&)> hasLambda =
            [&](const ast::Expr& e) -> bool {
                if (dynamic_cast<const ast::LambdaExpr*>(&e)) return true;
                if (auto* cd = dynamic_cast<const ast::ConditionalExpr*>(&e))
                    return (cd->thenValue && hasLambda(*cd->thenValue)) ||
                           (cd->elseValue && hasLambda(*cd->elseValue));
                return false;
            };
        auto depthOf = [&](const ast::Node* declNode) -> int {
            auto it = localDepth_.find(declNode);
            return it == localDepth_.end() ? -1 : it->second;
        };
        int td = targetLocal->decl ? depthOf(targetLocal->decl) : -1;
        int sd = src->decl ? depthOf(src->decl) : -1;
        if (hasLambda(*as.value) && td >= 0 && sd >= 0 && td < sd)
            dest = Dest::OuterLocal;
        else
            return false;
    }

    // §31.1 (issue #392): the borrow source is a catch-bound exception
    // variable. A caught exception's lifetime is the catch arm body; storing
    // a borrow of it into a field (or through an out/ref param) lets the
    // exception instance outlive the arm, after which the runtime disposes it
    // — a use-after-free. This is the `_lastError := ex;` error from Guide 02
    // §3. Distinct message from the owned-local case so the §31.1 rule is
    // pointed to directly.
    if (isCatchBorrow(src)) {
        if (dest == Dest::OuterLocal) {
            report(
                Severity::Error, as.span.begin,
                std::string("caught exception `") + std::string(src->name) +
                "` may not be captured by a closure assigned to `" +
                std::string(targetLocal->name) +
                "` — a caught exception is a borrow whose lifetime is the catch "
                "arm body (the runtime disposes it after the arm completes), and "
                "`" + std::string(targetLocal->name) +
                "` lives in an enclosing block that outlives the arm, so calling "
                "the closure later would read a disposed exception (§31.1; copy "
                "the data you need out of it into an owned value instead)");
        } else if (dest == Dest::Field) {
            report(
                Severity::Error, as.span.begin,
                std::string("caught exception `") + std::string(src->name) +
                "` may not be assigned to a field — a caught exception is a "
                "borrow whose lifetime is the catch arm body (the runtime "
                "disposes it after the arm completes), and a field outlives "
                "the arm, so the stored reference would dangle (§31.1; copy "
                "the data you need out of it — e.g. its message — into an "
                "owned value instead)");
        } else {
            report(
                Severity::Error, as.span.begin,
                std::string("caught exception `") + std::string(src->name) +
                "` may not be written through an out/ref parameter — a caught "
                "exception is a borrow whose lifetime is the catch arm body "
                "(the runtime disposes it after the arm completes), and the "
                "parameter's location lives in the caller's frame, which "
                "outlives the arm, so the written reference would dangle "
                "(§31.1; copy the data you need out of it into an owned value "
                "instead)");
        }
        return true;
    }

    if (dest == Dest::OuterLocal) {
        report(
            Severity::Error, as.span.begin,
            std::string("closure capturing `") + std::string(src->name) +
            "` is assigned to `" + std::string(targetLocal->name) +
            "`, which outlives it — `" + std::string(src->name) +
            "` is an owned scope-bound local disposed at the end of its block, "
            "but `" + std::string(targetLocal->name) +
            "` lives in an enclosing block, so calling the closure after the "
            "inner block exits reads a disposed value (§28.3 / §28.9; move an "
            "owned value into the closure, or keep `" + std::string(src->name) +
            "` in a block that outlives `" + std::string(targetLocal->name) +
            "`)");
    } else if (dest == Dest::Field) {
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
    return true;
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
        // §28.2: `Dispose()` is idempotent — a further call "remains a no-op,
        // not an error, in both modes." A statically-provable redundant
        // Dispose() is exactly the use-after-dispose case §28.2 prescribes a
        // compile-time WARNING for, not a hard error; rejecting it would refuse
        // the valid idempotent pattern the language guarantees. Warn (not
        // error) and keep the state Disposed — do NOT Poison it — so the rest
        // of the analysis proceeds undisturbed.
        report(
            Severity::Warning, loc,
            std::string("local `") + std::string(name) +
            "` is disposed again here — the second `.Dispose()` is a no-op "
            "(disposal is idempotent, §28.2); the redundant call usually "
            "indicates a bug");
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
    st->explicitlyDisposed = true;
    st->explicitDisposeLoc = loc;
    // §29.8 (#945): record the discharge so the iterator owned-param auto-dispose
    // check skips a parameter the body explicitly disposes on some path.
    for (auto& [s, stt] : states_)
        if (s->name == name) { everDischarged_.insert(s); break; }
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
    // §29.8 (#945): record the discharge so the iterator owned-param auto-dispose
    // check skips a parameter the body moves out on some path.
    for (auto& [s, stt] : states_)
        if (s->name == name) { everDischarged_.insert(s); break; }
}

// §28.8.1, #897 #898: the field analogue of markMovedByName. Mirrors the
// locals' read/move-after-move diagnostic (markMovedByName above, which reports
// "was already moved out at this point (§28.3)" when the local is MovedFrom):
// BEFORE marking the field moved-from, check whether it is ALREADY moved-from
// on the current path (`fieldMovedFrom_`, the path-sensitive #867 per-block
// field facts seeded from the converged CFG entry). If so, this `move x.f` has
// no ownership left to transfer — it is a use-after-move that, undiagnosed,
// double-owns the underlying object (#897: a straight-line second move reads the
// same field pointer → double-dispose → use-after-free). Report a §28.8.1 error
// at the move site. Because #867 made the facts path-sensitive and the loop
// fixpoint OR-merges the moved-from fact along the back-edge, this also rejects
// a loop that moves the field but restores it only conditionally (#898): on the
// moved-from back-edge path the 2nd-iteration move sees fieldMovedFrom_=true.
void MethodOwnership::recordFieldMove(std::string_view name,
                                      SourceLocation loc) {
    auto it = fieldMovedFrom_.find(name);
    if (it != fieldMovedFrom_.end() && it->second) {
        report(Severity::Error, loc,
               std::string("owned field `") + std::string(name) +
                   "` is already moved-from at this point; cannot move it "
                   "again — restore it (`this." + std::string(name) +
                   " := ...`) before moving again (§28.8.1)");
        // Leave the field moved-from; the move-site error stands. (No Poisoned
        // state for fields — the moved-from fact already reflects the worst
        // path and the at-exit restoration check, #867, remains accurate.)
        return;
    }
    fieldMovedFrom_[name] = true;
    fieldMoveLoc_[name] = loc;
}

// §28.3 / §28.11 (issue #860): a tracked local received ownership of a value
// moved OUT through an `out owned` argument; mark it Owned so the end-of-scope
// discharge check requires it to be disposed / moved / returned.
void MethodOwnership::markOwnedByName(std::string_view name, SourceLocation loc,
                                      const ast::Node* origin) {
    auto* st = lookupState(name);
    if (!st) return;                       // not a tracked local: nothing to do
    // A `using var` is scope-bound and implicitly discharged; do not disturb
    // its obligation. (A pre-declared `var` target — the modelled form — is
    // never implicitDischarge, so this only guards the unusual case.)
    if (st->implicitDischarge) return;
    // Conservatively mark Owned. On the Try* false-return path the local is
    // unchanged (null), but marking Owned unconditionally is sound: the
    // canonical null-safe consumer `item?.Dispose()` discharges it on both
    // paths, and an undischarged local is correctly flagged (§28.3). A leading
    // explicit-Dispose or move on the same statement runs after this in the
    // dataflow and transitions the state as usual.
    st->state = OwnershipState::Owned;
    st->obligationOrigin = origin;
    st->source.reset();
    (void)loc;
}

// §28.11 indexer-set owned check (issue #395). See the header for the contract.
bool MethodOwnership::indexerSetterTakesOwned(const ast::Expr& target) {
    const auto* ix = dynamic_cast<const ast::IndexExpr*>(&target);
    if (!ix || !ix->target) return false;
    const sema::TypeRef* tt = ctx_.exprType(*ix->target);
    if (!tt || tt->kind != sema::TypeRef::Kind::Named) return false;
    // Built-in owning collections: their indexer setter takes owned by
    // definition (stdlib §4.5 / §4.9). They resolve to a Named TypeRef with a
    // null declaration carrying the class name on the AST node.
    if (!tt->declaration) {
        if (tt->astNode && tt->astNode->name &&
            !tt->astNode->name->parts.empty()) {
            std::string_view cn = tt->astNode->name->parts[0];
            if (cn != "OwnedArray" && cn != "OwnedDictionary" &&
                cn != "OwnedBuffer")
                return false;
            // per §28.11 / §4.5 / §4.14 (#1209): the indexer setter takes
            // ownership only when the element (or dictionary value) type is a
            // class or interface — value-typed elements (primitive, record,
            // enum) are COPIED into the slot, so the assignment is a copy and
            // does NOT consume the RHS local (no owned-sink obligation).
            // For OwnedDictionary<K,V> the stored value is genericArgs[1];
            // for OwnedArray<T>/OwnedBuffer<T> the element is genericArgs[0].
            const std::size_t elemIdx = (cn == "OwnedDictionary") ? 1 : 0;
            if (tt->genericArgs.size() > elemIdx && tt->genericArgs[elemIdx]) {
                const sema::TypeRef* elemT = tt->genericArgs[elemIdx];
                return elemT->kind == sema::TypeRef::Kind::Named &&
                       elemT->declaration &&
                       (elemT->declaration->kind == ast::TypeDeclKind::Class ||
                        elemT->declaration->kind ==
                            ast::TypeDeclKind::Interface);
            }
            // Element type unresolved or a generic parameter: fall through to
            // false (conservative — mirrors ChannelSender's §6.9 gate in
            // builtinOwnedSinkArg).
            return false;
        }
        return false;
    }
    // User-declared type: find an indexer property whose set accessor is
    // declared `set(owned)`. Arity-match the index argument count so an
    // overloaded indexer set is identified precisely.
    const std::size_t nIdx = ix->indices.size();
    for (sema::Symbol* m : tt->declaration->members) {
        const auto* pd = dynamic_cast<const ast::PropertyDecl*>(m ? m->decl
                                                                  : nullptr);
        if (!pd || !pd->isIndexer) continue;
        if (pd->indexerParams.size() != nIdx) continue;
        for (const auto& acc : pd->accessors) {
            if (acc.isSet && acc.setterParamMod == "owned") return true;
        }
    }
    return false;
}

// §28.8 built-in owned-sink methods (issue #395). See the header for the
// contract. The owned-argument *index* differs per method, so the table maps
// (collection family, method name) → owned-arg position.
bool MethodOwnership::builtinOwnedSinkArg(const ast::Expr* callee,
                                          std::size_t argPos) {
    const auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(callee);
    if (!ma || !ma->target) return false;
    // per §30.14/§30.15 (#600): Domain.Spawn's trailing argument is moved into
    // the spawned domain — an owned-consuming position (stdlib §6.8: Spawn(entry,
    // owned arg)). `Domain` is a static builtin whose identifier may carry no
    // exprType, so match it syntactically before the typed path below.
    if (const auto* tid =
            dynamic_cast<const ast::IdentifierExpr*>(ma->target.get())) {
        // per §30.14 (#693): Domain.Spawn transfers ALL extra args (pos >= 1)
        // into the spawned domain — each is an owned-consuming position.
        if (tid->name == "Domain" && ma->member == "Spawn")
            return argPos >= 1;
        // stdlib §6.7 / §30.10 (#1121): Task.WhenAllOwned<T>(owned tasks:
        // OwnedArray<OwnedTask<T>>) takes ownership of its argument array (and
        // its tasks) — the documented `await Task.WhenAllOwned(tasks)` form. The
        // owned-arg position (0) is an owned sink, so passing a bare owned local
        // discharges its disposal obligation (an implicit §28.8 transfer, exactly
        // as the explicit `move tasks` already does). `Task` is a static builtin
        // whose identifier carries no exprType, so match it syntactically.
        if (tid->name == "Task" && ma->member == "WhenAllOwned")
            return argPos == 0;
    }
    const sema::TypeRef* tt = ctx_.exprType(*ma->target);
    // Built-in collections resolve to a Named TypeRef with a null declaration
    // and carry the class name on the AST node. A user type (non-null
    // declaration) is handled by the precise declaration-based path instead.
    if (!tt || tt->kind != sema::TypeRef::Kind::Named || tt->declaration)
        return false;
    if (!tt->astNode || !tt->astNode->name ||
        tt->astNode->name->parts.empty())
        return false;
    const std::string_view cn = tt->astNode->name->parts[0];
    // per §30.15 / §6.9 (#600, #692): ChannelSender<T>.Send(owned item)
    // transfers ownership only when T is a class or interface type — the
    // channel takes ownership of the heap object and the owned-sink rule
    // must fire so a borrow is rejected. For value-typed T and string the
    // ownership modifiers drop out (§6.9: "Send(item: T)") — the slot is a
    // copy, no heap disposal obligation, so the owned-sink rule must NOT fire.
    if (cn == "ChannelSender" && ma->member == "Send" && argPos == 0) {
        // Inspect the first generic argument (T) of ChannelSender<T>.
        // If T is a class or interface type, the sink applies; otherwise skip.
        if (!tt->genericArgs.empty() && tt->genericArgs[0]) {
            const sema::TypeRef* elemT = tt->genericArgs[0];
            if (elemT->kind == sema::TypeRef::Kind::Named &&
                elemT->declaration &&
                (elemT->declaration->kind == ast::TypeDeclKind::Class ||
                 elemT->declaration->kind == ast::TypeDeclKind::Interface))
                return true;  // class/interface T → owned-sink applies
        }
        // Value-typed T (or unresolved/generic T): ownership modifiers drop
        // out per §6.9 — not an owned sink.
        return false;
    }
    const bool isOwning =
        cn == "OwnedArray" || cn == "OwnedDictionary" ||
        cn == "OwnedSet" || cn == "OwnedStack" || cn == "OwnedQueue" ||
        cn == "OwnedLinkedList";
    if (!isOwning) return false;
    const std::string_view m = ma->member;
    // The owned argument's position per stdlib §4.5 / §4.7 / §4.9.
    if ((m == "Add" || m == "AddFirst" || m == "AddLast" || m == "Push" ||
         m == "Enqueue" || m == "AddRange") &&
        cn != "OwnedDictionary")
        return argPos == 0;                       // owned item is arg 0
    if (m == "Add" && cn == "OwnedDictionary")
        return argPos == 1;                       // Add(key, owned value)
    if (m == "Insert")
        return argPos == 1;                       // Insert(index, owned item)
    if (m == "AddBefore" || m == "AddAfter")
        return argPos == 1;                       // Add*(node, owned item)
    return false;
}

// §28.3 / §28.11 built-in out-owned methods (issue #860). See the header for
// the contract. The owned-OUT argument *index* differs per method, so this
// maps (receiver-type family, method name) → out-owned-arg position. Mirrors
// builtinOwnedSinkArg's MemberAccessExpr/receiver-type-name-head dispatch.
bool MethodOwnership::builtinOutOwnedArg(const ast::Expr* callee,
                                         std::size_t argPos) {
    const auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(callee);
    if (!ma || !ma->target) return false;
    const sema::TypeRef* tt = ctx_.exprType(*ma->target);
    // Built-in collections / handles resolve to a Named TypeRef with a null
    // declaration and carry the type name on the AST node. A user type (non-
    // null declaration) is handled by the declaration-based path instead.
    if (!tt || tt->kind != sema::TypeRef::Kind::Named || tt->declaration)
        return false;
    if (!tt->astNode || !tt->astNode->name ||
        tt->astNode->name->parts.empty())
        return false;
    const std::string_view cn = tt->astNode->name->parts[0];
    const std::string_view m = ma->member;
    // Owning collections — `where T: class`, so the moved-out element is
    // always a disposable reference type (stdlib §4.5 / §4.7 / §4.9).
    if (cn == "OwnedStack"  && m == "TryPop")      return argPos == 0;
    if (cn == "OwnedQueue"  && m == "TryDequeue")  return argPos == 0;
    if (cn == "OwnedLinkedList" &&
        (m == "TryPopFirst" || m == "TryPopLast")) return argPos == 0;
    // OwnedSet<T>.TryTake(query, out owned removed: T?) — out-owned at arg 1.
    if (cn == "OwnedSet"    && m == "TryTake")     return argPos == 1;
    // OwnedDictionary<K,V>.TryPopValue(key, out owned value: V) — arg 1.
    if (cn == "OwnedDictionary" && m == "TryPopValue") return argPos == 1;
    // Weak<T>.TryUpgrade(out owned s: Shared<T>?) — arg 0 (stdlib §2.16). The
    // accepted call-site form is `out using var s`, which declares a fresh
    // local the dataflow does not track; recognised here for completeness.
    if (cn == "Weak"        && m == "TryUpgrade")  return argPos == 0;
    // Channel<T>.TryReceive(out owned item: T?) — arg 0, but only when T is a
    // class/interface: for value-typed T and string the ownership modifiers
    // drop out (stdlib §6.9), so there is no owned-out transfer to model.
    if (cn == "Channel" && m == "TryReceive" && argPos == 0) {
        if (!tt->genericArgs.empty() && tt->genericArgs[0]) {
            const sema::TypeRef* elemT = tt->genericArgs[0];
            return elemT->kind == sema::TypeRef::Kind::Named &&
                   elemT->declaration &&
                   (elemT->declaration->kind == ast::TypeDeclKind::Class ||
                    elemT->declaration->kind == ast::TypeDeclKind::Interface);
        }
        return false;
    }
    return false;
}

// §28.3 / §28.11 caller-side out-owned transfer (issue #860). See the header.
void MethodOwnership::markOutOwnedArgs(
        const std::vector<ast::CallArgument>& args,
        const ast::Expr* callee) {
    // Resolve the callee's parameter list so a USER method's `out owned`
    // formal is recognised (the built-in collections have no AST declaration
    // and are handled by builtinOutOwnedArg below).
    const std::vector<ast::Param>* params = nullptr;
    if (callee) {
        sema::Symbol* sym = ctx_.exprSymbol(*callee);
        if (auto* ms = dynamic_cast<sema::MethodSymbol*>(sym)) {
            if (auto* md = dynamic_cast<const ast::MethodDecl*>(ms->decl))
                params = &md->params;
        }
    }
    std::size_t pos = 0;
    for (const auto& a : args) {
        if (!a.value) { ++pos; continue; }
        // The argument is an out-owned transfer when the formal is genuinely
        // `out owned`. We do NOT trust the call-site modifier alone: a user
        // may write a modifier on a plain `out` (borrow-form) builtin and sema
        // does not always reject it, so keying off the modifier alone would
        // over-fire on a value-typed `out`
        // (e.g. `Stack<int>.TryPop(out var item)`, no disposal obligation).
        const bool userOutOwned =
            params && pos < params->size() &&
            (*params)[pos].modifier == "out owned";
        const bool builtinOutOwned = builtinOutOwnedArg(callee, pos);
        if (userOutOwned || builtinOutOwned) {
            if (auto* id =
                    dynamic_cast<const ast::IdentifierExpr*>(a.value.get())) {
                // §22.4 / v0.160 (#1305): `out owned var x` declares a FRESH
                // local inline.  The LocalSymbol was registered in
                // currentScope_ by ExprTypecheck_Exprs.cpp, but the ownership
                // dataflow has not yet seen any LocalDecl for it, so it is not
                // in `states_`.  Add it here with Owned state and record it in
                // `everOwnedPlainLocals_` so the end-of-method discharge check
                // catches it even if its enclosing block exits before the method
                // return. per §22.4, §28.3
                if (a.modifier == "out owned var") {
                    if (sema::Symbol* sym = ctx_.exprSymbol(*id)) {
                        if (states_.find(sym) == states_.end()) {
                            LocalOwnership lo;
                            lo.state = OwnershipState::Owned;
                            lo.obligationOrigin = id;
                            states_[sym] = lo;
                            everOwnedPlainLocals_.insert(sym);
                        }
                    }
                } else {
                    // Pre-declared local (plain `out x`) or `out using var x`
                    // (implicitly discharged — markOwnedByName's
                    // implicitDischarge guard skips it safely): mark Owned.
                    markOwnedByName(id->name, id->span.begin, id);
                }
            }
        }
        ++pos;
    }
}

// §28.3 (issue #398): is `e` a read of an `owned` field? See the header.
bool MethodOwnership::exprIsOwnedFieldRead(const ast::Expr& e) const {
    auto isOwnedField = [](sema::Symbol* sym) -> bool {
        auto* fs = dynamic_cast<sema::FieldSymbol*>(sym);
        if (!fs) return false;
        for (std::string_view m : fs->modifiers)
            if (m == "owned") return true;
        return false;
    };
    // Explicit member access `obj.f` — sema attaches the member's FieldSymbol
    // to the MemberAccessExpr (ExprTypecheck). // per §28.3
    if (dynamic_cast<const ast::MemberAccessExpr*>(&e))
        return isOwnedField(ctx_.exprSymbol(e));
    // Bare identifier resolving to an owned field of the declaring class
    // (implicit `this`). A method-scope local/parameter is not a FieldSymbol,
    // so this only matches a genuine field read. // per §28.3
    if (dynamic_cast<const ast::IdentifierExpr*>(&e))
        return isOwnedField(ctx_.exprSymbol(e));
    return false;
}

// §28.8 owned-sink check (issue #395). See the header for the full contract.
bool MethodOwnership::ownedArgSourceIsBorrow(const ast::Expr& argValue) {
    const ast::Expr* e = &argValue;
    // Unwrap a leading `move`: `move b` of a borrow is no more an owned value
    // than the bare `b`. (`move this.f` / `move coll[i]` unwrap to a member /
    // index expression, NOT a bare identifier, so they fall through to the
    // "cannot classify → not a borrow" tail and are correctly accepted.)
    bool unwrappedMove = false;
    if (auto* un = dynamic_cast<const ast::UnaryExpr*>(e)) {
        if (un->op == "move" && un->operand) {
            e = un->operand.get();
            unwrappedMove = true;
        }
    }
    // §28.3 / §28.8.1 (issue #398): a *bare* read of an `owned` field — no
    // enclosing `move` — is Borrowed ("Field access `obj.f` where `f` is
    // `owned` | Borrowed (source: `obj`)"), so flowing it into an owned sink is
    // the double-dispose hazard (the sink and the field's owner both dispose
    // the value). `move obj.f` IS the legal owned producer (§28.8.1: "the only
    // way to read an owned field's value as an owned value"), so the `move`
    // form is excluded here. This is checked before the bare-identifier path
    // below so an implicit-`this` owned-field identifier is classified as a
    // field read, not mistaken for a local/parameter. // per §28.3, §28.8.1
    if (!unwrappedMove && exprIsOwnedFieldRead(*e)) return true;
    // §28.3 / §28.8.1 / §28.11 (issue #407): a *bare* indexer-get `coll[i]` —
    // no enclosing `move` — is Borrowed ("Indexer access `coll[i]` | Borrowed
    // (source: `coll`)", §28.3 producers; for owning collections §28.11 states
    // the indexer-get "produces a borrow of the slot's value"). Flowing it into
    // an owned sink double-disposes: the slot's owner (the collection) and the
    // sink both dispose the value. `move coll[i]` IS the legal owned producer
    // (§28.8.1 / §28.11) — it arrives wrapped in `move` and was unwrapped above,
    // so `unwrappedMove` is true and this branch is correctly skipped. The
    // type gate excludes a value-typed slot (no ownership to double-dispose);
    // `isBorrowableType` is conservative (unknown → borrowable), and no legal
    // owned producer is a bare IndexExpr, so flagging an untyped one is sound.
    // // per §28.3, §28.8.1, §28.11
    if (!unwrappedMove && dynamic_cast<const ast::IndexExpr*>(e)) {
        return isBorrowableType(ctx_.exprType(*e));
    }
    // §28.3 (issue #407): a *bare* read of ANY class-typed field `obj.f` — owned
    // OR non-owned — is Borrowed ("Field access `obj.f` where `f` is not `owned`
    // | Borrowed (source: `obj`)", and likewise for the `owned` case handled by
    // `exprIsOwnedFieldRead` above). §28.3 borrow-lifetime rules forbid assigning
    // any borrow to a field and §28.8 forbids passing one where an owned value is
    // required; the non-`owned`-field read was previously missed (only the owned-
    // field form was classified). `move obj.f` is owned only for an `owned` field
    // (§28.8.1); a non-`owned` field has no `move` form, so the `unwrappedMove`
    // guard merely keeps a stray `move obj.f` of an owned field on its own
    // (already-flagged) path. The type gate excludes value-typed fields. The
    // bare-identifier implicit-`this` field read is handled by the identifier
    // path below via its resolved FieldSymbol. // per §28.3, §28.8
    if (!unwrappedMove) {
        if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(e)) {
            if (sema::Symbol* msym = ctx_.exprSymbol(*ma)) {
                if (dynamic_cast<sema::FieldSymbol*>(msym))
                    return isBorrowableType(ctx_.exprType(*e));
            }
        }
    }
    const auto* id = dynamic_cast<const ast::IdentifierExpr*>(e);
    if (!id) return false;  // new / call / other member / literal: not flagged
    // §28.3 (issue #403): a bare `this` is Borrowed ("`this` | Borrowed (source:
    // caller)"). A class can never transfer ownership of itself, so `this` is
    // never a legal owned producer; handing it to an owned sink lets the sink
    // dispose the live receiver (the caller still owns it) — a double-dispose.
    // `this` carries no resolved Symbol (sema leaves it unresolved), so the
    // ParameterSymbol/state branches below never see it; recognize it by name.
    // // per §28.3, §28.8
    if (id->name == "this") return true;
    // A tracked local: only the Borrowed state is the owned-sink hazard. Owned
    // (the legal transfer) and the error-producing states (MovedFrom/Disposed/
    // Uninitialized — diagnosed by their own checks) are NOT flagged here.
    if (auto* st = lookupState(id->name)) {
        return st->state == OwnershipState::Borrowed;
    }
    // Not a tracked local. Resolve the symbol to classify the two remaining
    // borrow forms a bare identifier can take.
    sema::Symbol* sym = ctx_.exprSymbol(*id);
    // §31.1 (issue #404): a catch-bound exception variable. The caught
    // exception is a borrow whose lifetime is the catch arm body — "may be
    // passed to other methods (which further borrow for the duration of the
    // call) but may not be assigned to an `owned` field, returned as `owned`,
    // or captured by a closure that may outlive the arm." An `owned`-consuming
    // sink (an `owned` parameter / constructor argument per §28.8, or an
    // owning-collection `set(owned)` slot per §28.11) takes responsibility for
    // disposing the value and may retain it past the call — the same retention
    // §31.1 forbids; the runtime disposes the exception after the arm, so the
    // sink's later disposal is a use-after-free / double-dispose. The catch
    // binding is a method-scope LocalSymbol that is NOT in `states_`, so neither
    // the `lookupState` Borrowed branch above nor the ParameterSymbol branch
    // below sees it; recognize it directly via `isCatchBorrow`. (`move ex`
    // unwrapped to the bare `ex` above is no more an owned value than the bare
    // binding — the runtime, not this scope, owns it.) // per §31.1, §28.8
    if (isCatchBorrow(sym)) return true;
    // A bare identifier resolving to a non-`owned` parameter is a borrowed
    // parameter (source: the caller's frame, §28.3 producers "Parameter `p`
    // (no modifier) | Borrowed (source: caller)") — handing it to an `owned`
    // sink is the double-dispose hazard.
    if (auto* ps = dynamic_cast<sema::ParameterSymbol*>(sym)) {
        return ps->modifier != "owned" && ps->modifier != "out owned";
    }
    // §28.3 (issue #407): a bare identifier resolving to a class-typed field of
    // the declaring class (implicit `this`) is a field read `this.f`, Borrowed
    // per §28.3 ("Field access `obj.f` where `f` is not `owned` | Borrowed"; the
    // `owned`-field case was already handled by `exprIsOwnedFieldRead` above).
    // Same hazard as the explicit `obj.f` MemberAccessExpr form. The type gate
    // excludes value-typed fields. // per §28.3, §28.8
    if (dynamic_cast<sema::FieldSymbol*>(sym)) {
        return isBorrowableType(ctx_.exprType(*id));
    }
    // §19.3 / §28.8 (#1280): a record-DESTRUCTURING pattern binding (`case b
    // Box(a) => Prog.Sink(a)`) is a non-destructive BORROW of the matched field —
    // the scrutinee's owner still owns the value; the binding does not. Its
    // LocalSymbol's `decl` is a `BindingPattern` (the naked binding's synthesized
    // anchor per #1271, or an explicit `var x`), which distinguishes it from a
    // `var`-declared local (decl = LocalDecl, tracked in `states_` above). It is
    // NOT tracked in `states_`, so the `lookupState` Borrowed branch missed it and
    // it fell through to "not a borrow" — letting an `owned` sink dispose a value
    // the real owner also disposes (a double-free). Flag it as a borrow when the
    // bound field is a reference type (a record field is always non-owning, §9.7).
    if (auto* loc = dynamic_cast<sema::LocalSymbol*>(sym)) {
        if (loc->decl &&
            dynamic_cast<const ast::BindingPattern*>(loc->decl))
            return isBorrowableType(ctx_.exprType(*id));
    }
    // Anything else we cannot positively classify as a borrow: conservatively
    // NOT flagged, so a valid transfer is never over-rejected. // per §28.8
    return false;
}

bool MethodOwnership::checkOwnedArgSource(const ast::Expr& argValue,
                                          SourceLocation loc) {
    if (!ownedArgSourceIsBorrow(argValue)) return false;
    report(
        Severity::Error, loc,
        "a borrow cannot be passed where an `owned` value is required: "
        "an `owned` sink takes responsibility for disposing the value, but a "
        "borrow is owned elsewhere (§28.8 — transfer an owned value with "
        "`move`, `new`, or a call returning `owned T`)");
    return true;
}

// §28.3 / §28.8 (issue #407): owned-sink catch-all. See the header.
bool MethodOwnership::checkOwnedArgOtherBorrow(const ast::Expr& argValue) {
    // A bare identifier (including bare `this`) and a bare owned-field read are
    // handled by the caller's dedicated branches; only the *other* class-typed
    // borrow forms — `coll[i]` (§28.3 / §28.11) and a field read `obj.f`
    // (§28.3) — are routed here. Restrict to those two AST shapes so a `move`-
    // wrapped or owned-producer argument the caller already handled is never
    // re-examined; `checkOwnedArgSource` then applies the full classifier
    // (which also re-checks the type gate and the move-unwrap). // per §28.3
    const ast::Expr* e = &argValue;
    if (auto* un = dynamic_cast<const ast::UnaryExpr*>(e)) {
        if (un->op == "move") return false;  // legal owned producer (§28.8.1)
    }
    if (!dynamic_cast<const ast::IndexExpr*>(e) &&
        !dynamic_cast<const ast::MemberAccessExpr*>(e))
        return false;
    return checkOwnedArgSource(argValue, argValue.span.begin);
}

// §28.3 / §28.5 / §28.8 (issue #402): is the assignment target an `owned`
// field? `exprIsOwnedFieldRead` recognizes both `this.f` / `obj.f` (a
// MemberAccessExpr whose symbol is an `owned` FieldSymbol) and a bare implicit-
// `this` owned-field identifier. A property setter is not a FieldSymbol, so a
// property target is correctly excluded — only true storage fields can be
// `owned` (§28.5). // per §28.3, §28.5
bool MethodOwnership::targetIsOwnedField(const ast::Expr& target) const {
    return exprIsOwnedFieldRead(target);
}

// #1020 / §28.6 (v0.153): a non-`owned` (borrow) field target. See the header.
bool MethodOwnership::targetIsNonOwnedField(const ast::Expr& target) const {
    // Only a member access `obj.f` or a bare identifier resolving to a field
    // (implicit `this`) names a field; sema attaches the FieldSymbol to it.
    if (!dynamic_cast<const ast::MemberAccessExpr*>(&target) &&
        !dynamic_cast<const ast::IdentifierExpr*>(&target))
        return false;
    auto* fs = dynamic_cast<sema::FieldSymbol*>(ctx_.exprSymbol(target));
    if (!fs) return false;
    for (std::string_view m : fs->modifiers)
        if (m == "owned") return false;  // owned field → §28.5 path, not here
    return true;
}

// #926 / §28.6 (v0.153): a getter may not return a fresh owned value. See header.
void MethodOwnership::checkGetterFreshOwnedReturn(const ast::Expr& e) {
    if (producerState(e) == OwnershipState::Owned) {
        report(Severity::Error, e.span.begin,
               "a property or indexer getter may not return a freshly-produced "
               "owned value — a getter is a borrow-producing position "
               "(properties and indexers do not carry `owned`), so the value "
               "would escape with no owner and leak; expose the factory as a "
               "method returning `owned T` (§28.6)");
    }
}

void MethodOwnership::scanGetterReturnsStmt(const ast::Stmt* s) {
    if (!s) return;
    if (auto* rs = dynamic_cast<const ast::ReturnStmt*>(s)) {
        if (rs->value) checkGetterFreshOwnedReturn(*rs->value);
    } else if (auto* is = dynamic_cast<const ast::IfStmt*>(s)) {
        scanGetterReturns(is->ifBranch.body);
        for (const auto& eb : is->elifBranches) scanGetterReturns(eb.body);
        if (is->hasElse) scanGetterReturns(is->elseBody);
    } else if (auto* ws = dynamic_cast<const ast::WhileStmt*>(s)) {
        scanGetterReturns(ws->body);
    } else if (auto* fr = dynamic_cast<const ast::ForStmt*>(s)) {
        scanGetterReturns(fr->body);
    } else if (auto* ts = dynamic_cast<const ast::TryStmt*>(s)) {
        scanGetterReturns(ts->body);
        for (const auto& c : ts->catches) scanGetterReturnsStmt(c.body.get());
        if (ts->hasFinally) scanGetterReturns(ts->finallyBody);
    } else if (auto* db = dynamic_cast<const ast::DoEndBlock*>(s)) {
        scanGetterReturns(db->body);
    }
}

void MethodOwnership::scanGetterReturns(
        const std::vector<std::unique_ptr<ast::Stmt>>& stmts) {
    for (const auto& s : stmts) scanGetterReturnsStmt(s.get());
}

// #1066 / §9.7 (v0.153): a borrow-carrying field type. Used ONLY by
// checkRecordFromTemporary (isBorrowBearingRecord keeps its own predicate, so
// the #360 return-escape behaviour is unchanged). Unlike that predicate, this
// one also recognizes the BUILTIN heap reference types (Array<T>,
// Dictionary<K,V>, … — which carry no user TypeDecl, so declaration is null) as
// borrow-carrying; the §9.7 repro `record Bag(Items: Array<int>)` is exactly
// such a field.
bool MethodOwnership::fieldTypeIsBorrowCarrying(const sema::TypeRef* ft) const {
    if (!ft) return false;
    switch (ft->kind) {
        case sema::TypeRef::Kind::Named: {
            if (ft->declaration) {
                if (ft->declaration->isValueBuiltin) return false;
                if (ft->declaration->kind == ast::TypeDeclKind::Class ||
                    ft->declaration->kind == ast::TypeDeclKind::Interface)
                    return true;
                if (ft->declaration->kind == ast::TypeDeclKind::Record)
                    return isBorrowBearingRecord(ft);  // transitive
                return false;  // enum
            }
            // Builtin generic with no user TypeDecl: a heap reference collection
            // carries a borrow. The value-type builtins (DateTime / TimeSpan /
            // Guid / CancellationToken) and `string` are not heap collections and
            // are excluded by name.
            if (ft->astNode && ft->astNode->name &&
                !ft->astNode->name->parts.empty()) {
                std::string_view h = ft->astNode->name->parts.back();
                if (h == "Array" || h == "OwnedArray" || h == "Dictionary" ||
                    h == "OwnedDictionary" || h == "List" || h == "LinkedList" ||
                    h == "OwnedLinkedList" || h == "Set" || h == "OwnedSet" ||
                    h == "Stack" || h == "OwnedStack" || h == "Queue" ||
                    h == "OwnedQueue" || h == "OwnedBuffer")
                    return true;
            }
            return false;
        }
        case sema::TypeRef::Kind::InlineArray:
            return ft->arrayElement && !ft->arrayOwned &&
                   isBorrowableType(ft->arrayElement);
        case sema::TypeRef::Kind::Primitive:
            return false;  // string / Object / numeric — not a borrow source
        default:
            return false;  // Unit, Function, enum, Error
    }
}

// #1066 / §9.7 (v0.153): a temporary (non-place) expression. See header.
bool MethodOwnership::exprIsTemporary(const ast::Expr& e) const {
    // A "place" — a named local/parameter/field, a field/property access, or an
    // index slot — has storage that can outlive the construction. Anything else
    // (a literal, a `new`, a call result, an operator result) is a temporary
    // whose lifetime ends at the enclosing expression.
    if (dynamic_cast<const ast::IdentifierExpr*>(&e)) return false;
    if (dynamic_cast<const ast::MemberAccessExpr*>(&e)) return false;
    if (dynamic_cast<const ast::IndexExpr*>(&e)) return false;
    return true;
}

// #1066 / §9.7 (v0.153): reject `new Record(<temporary>)` binding a temporary to
// a borrow-carrying field. See header.
void MethodOwnership::checkRecordFromTemporary(const ast::NewExpr& ne) {
    const sema::TypeRef* rt =
        ne.type ? ctx_.resolvedType(*ne.type) : ctx_.exprType(ne);
    if (!rt || rt->kind != sema::TypeRef::Kind::Named || !rt->declaration)
        return;
    const auto* decl =
        dynamic_cast<const ast::TypeDecl*>(rt->declaration->decl);
    if (!decl || decl->kind != ast::TypeDeclKind::Record) return;
    // Positional records (§9.1): argument i binds to positional field i.
    const auto& fields = decl->positionalFields;
    for (std::size_t i = 0; i < ne.args.size() && i < fields.size(); ++i) {
        const ast::Param& f = fields[i];
        if (!f.type) continue;
        if (!fieldTypeIsBorrowCarrying(ctx_.resolvedType(*f.type))) continue;
        const ast::Expr* arg = ne.args[i].value.get();
        if (!arg || !exprIsTemporary(*arg)) continue;
        // §9.7 null-source form: a `null` argument stores no borrow (the field
        // is null), so it is not a dangling borrow and is explicitly permitted.
        if (dynamic_cast<const ast::NullLiteralExpr*>(arg)) continue;
        report(
            Severity::Error, ne.span.begin,
            std::string("a borrow-bearing record may not be constructed from a "
                        "temporary bound to its borrow-carrying field `") +
            std::string(f.name) +
            "` — the temporary's lifetime ends at this construction "
            "expression, so the record would immediately outlive that borrow's "
            "source (a dangling borrow); bind the source to a named local whose "
            "scope outlives the record and pass that (§9.7 / §28.3 borrow-"
            "lifetime rule)");
        return;  // one diagnostic per construction is enough
    }
}

// §28.11 (issue #780): an owning inline-array slot `a[i]`. See the header for
// the contract.
bool MethodOwnership::targetIsOwningInlineArraySlot(
    const ast::Expr& target) const {
    const auto* ix = dynamic_cast<const ast::IndexExpr*>(&target);
    if (!ix || !ix->target) return false;
    const sema::TypeRef* tt = ctx_.exprType(*ix->target);
    return tt && tt->kind == sema::TypeRef::Kind::InlineArray && tt->arrayOwned;
}

// §28.3 / §28.5 / §28.8 (issue #402): an owned-field assignment. See the header
// for the full contract.
void MethodOwnership::handleOwnedFieldAssignment(const ast::AssignmentStmt& as,
                                                 bool borrowAlreadyFlagged,
                                                 bool intoOwningArraySlot) {
    if (!as.value) return;
    // The RHS must be an OWNED value (§28.3 producers). `ownedArgSourceIsBorrow`
    // is the same borrow classifier the owned-parameter / owned-sink checks
    // (#395 / #398) use: it returns true for a bare borrow parameter, a
    // Borrowed-state local, and a bare (un-`move`d) owned-field read — every
    // borrow form a class-typed RHS can take. A borrow stored into the owned
    // field would be double-disposed (the field's owner AND the borrow's real
    // owner both dispose it). #386's checkBorrowStoreEscape already diagnoses
    // the *owned-scope-bound-local* borrow store into any field; skip those so
    // the same store is not reported twice. // per §28.3, §28.5, §28.8
    if (ownedArgSourceIsBorrow(*as.value)) {
        if (!borrowAlreadyFlagged) {
            report(
                Severity::Error, as.value->span.begin,
                intoOwningArraySlot
                    ? "a borrow cannot be stored into an owning array slot: the "
                      "array disposes each owned element when the array is "
                      "disposed, but a borrow is owned elsewhere and its real "
                      "owner disposes it too — a double-free (§28.11 / §28.3 — "
                      "store an owned value with `new`, `move`, an owned-"
                      "returning call, or a consumed `owned` parameter)"
                    : "a borrow cannot be stored into an `owned` field: the "
                      "field's owner disposes the value when the object is "
                      "disposed, but a borrow is owned elsewhere and its real "
                      "owner disposes it too — a double-free (§28.3 / §28.5 / "
                      "§28.8 — store an owned value with `new`, `move`, an "
                      "owned-returning call, or a consumed `owned` parameter)");
        }
        return;
    }
    // The RHS is a legal owned value — record the ownership transfer INTO the
    // field. `move x` / `move obj.f` / `f(owned z)` / `new T(owned z)` side-
    // effects were already recorded by recordMovesIn in handleAssignment; here
    // only the BARE-identifier case remains (`this.f := x;`, no explicit
    // `move`), which §28.3 treats as a move-on-read because the field is an
    // owned slot ("Reading an owned local x in a position that requires an
    // owned value … transfers ownership out of x"). // per §28.3, §28.8
    const auto* id = dynamic_cast<const ast::IdentifierExpr*>(as.value.get());
    if (!id) return;
    auto* st = lookupState(id->name);
    if (!st) return;
    if (st->state != OwnershipState::Owned) return;
    // A bare owned-PARAMETER read discharges the parameter (the §28.8
    // `_resource := resource;` ownership-transfer pattern: the caller
    // relinquished ownership and the field now holds it). A bare owned-LOCAL
    // read moves out of the local, leaving it moved-from (markMovedByName also
    // correctly rejects moving out of a `using var`, which is scope-bound).
    sema::Symbol* sym = ctx_.exprSymbol(*id);
    if (dynamic_cast<sema::ParameterSymbol*>(sym)) {
        markDisposedByName(id->name, id->span.begin);
    } else {
        markMovedByName(id->name, id->span.begin);
    }
}

// ===========================================================================
// Async-readiness 3c: across-suspend ownership + Pattern A/B at the await site
// ===========================================================================
//
// THE MODEL (recorded decision; see issue #425 "async-ownership model (3c)"):
//
//   An owned local in an async method is classified suspend-local (its live
//   range crosses at least one `await`) or activation-local (it does not). A
//   suspend-local lives in the heap-allocated state-machine frame and must be
//   disposed on EVERY completion path: normal return, fault/exception,
//   abandonment (the Task dropped without being awaited to completion — §30.5
//   "Disposing an unconsumed `OwnedTask<T>`"), and cancellation (§30.8,
//   observed at an await as `OperationCanceledException` — §30.9).
//
//   This is represented by:
//     (a) a SuspendLocal ANNOTATION on the owned local's LocalOwnership
//         (the `suspendLocal` flag), computed by live-range-across-await; and
//     (b) SYNTHETIC COMPLETION EDGES in the CFG (CFGBlock::completionSuccs),
//         added from each await-bearing block to the function exit, which the
//         dataflow gives the SAME frame-disposal treatment it already gives
//         the §28.3 exception-path edges — a still-owned suspend-local on a
//         completion edge is dropped from the meet (the frame disposed it),
//         not reported as a leak.
//
//   The lattice (OwnershipState) is DELIBERATELY UNCHANGED. A suspend-local's
//   static disposal obligations are identical to an ordinary owned local's —
//   it must be discharged on the normal path (the existing §28.3 check) and is
//   auto-disposed by the frame on the abandonment/cancellation/exception
//   completion edges (the existing exception-path-discharge machinery, now
//   also fed the synthetic completion edges). Adding a new lattice value
//   (e.g. `SuspendOwned`) was the alternative considered and REJECTED: it
//   would force every join/meet, every transfer function, and every existing
//   diagnostic to learn about the new state for no behavioural gain — the
//   "disposed on every completion path" property is already expressible as
//   "discharged on the normal path + discharged on each completion edge,"
//   which the annotation (for codegen's benefit) + synthetic edges (for the
//   analysis) capture without disturbing the proven lattice. Keeping the
//   lattice fixed is also what makes this increment ADDITIVE: a synchronous
//   body sees neither the annotation nor the synthetic edges.
//
//   The annotation is the contract the async-codegen milestone implements
//   against (which locals go in the frame; which completion paths dispose
//   them). For the ownership pass itself it is informational — the
//   diagnostics flow from the unchanged lattice plus the completion edges.

MethodOwnership::AwaitTaskKind
MethodOwnership::awaitTaskKind(const ast::UnaryExpr& await) const {
    if (!await.operand) return AwaitTaskKind::NotTask;
    // 3b attached the operand's resolved task type. A `Task` / `Task<T>` /
    // `OwnedTask<T>` is a Named TypeRef whose single-part name head is the
    // task class. // per §30.3
    const sema::TypeRef* ot = ctx_.exprType(*await.operand);
    if (!ot || ot->kind != sema::TypeRef::Kind::Named || ot->nullable)
        return AwaitTaskKind::NotTask;
    if (!ot->astNode || !ot->astNode->name ||
        ot->astNode->name->parts.size() != 1)
        return AwaitTaskKind::NotTask;
    std::string_view head = ot->astNode->name->parts.front();
    // True when the result type argument T is a CLASS — the gate that selects
    // Pattern A/B over the no-obligation value/string/runtime-managed forms
    // (§30.5 producer-side rules). A null declaration (built-in / generic
    // argument) is conservatively treated as NOT a class, so the site rule is
    // not over-applied to an instantiation the pass cannot resolve. // per §30.5
    auto resultIsClass = [&]() -> bool {
        if (ot->genericArgs.empty() || !ot->genericArgs.front()) return false;
        const sema::TypeRef* arg = ot->genericArgs.front();
        if (arg->kind != sema::TypeRef::Kind::Named) return false;
        if (arg->declaration)
            return arg->declaration->kind == ast::TypeDeclKind::Class;
        // §30.5 / stdlib §3.1 (#1122): a built-in OWNING collection result
        // (`OwnedArray<T>` etc. — the result type of `Task.WhenAllOwned`) is a
        // heap/reference type that carries a disposal obligation, so an
        // `OwnedTask<OwnedArray<T>>` is a valid Pattern B: its awaited result
        // must be `using var`-bound and the awaited operand consumed. These
        // resolve to a Named TypeRef with a NULL declaration (built-in), which
        // the declaration-based check above misses — so the await was wrongly
        // classified NotTask and the bound-local operand was not discharged.
        if (arg->astNode && arg->astNode->name &&
            arg->astNode->name->parts.size() == 1) {
            std::string_view an = arg->astNode->name->parts.front();
            return an == "OwnedArray" || an == "OwnedStack" ||
                   an == "OwnedQueue" || an == "OwnedLinkedList" ||
                   an == "OwnedDictionary" || an == "OwnedSet";
        }
        return false;
    };
    if (head == "Task") {
        if (ot->genericArgs.empty()) return AwaitTaskKind::UnitTask;
        return resultIsClass() ? AwaitTaskKind::PatternA
                               : AwaitTaskKind::ValueResult;
    }
    if (head == "OwnedTask") {
        // `OwnedTask<T>` is a compile error for non-class T (sema §14.4); when
        // it reaches here for class T it is Pattern B.
        return resultIsClass() ? AwaitTaskKind::PatternB
                               : AwaitTaskKind::NotTask;
    }
    return AwaitTaskKind::NotTask;
}

bool MethodOwnership::awaitOperandIsOwningLocation(
    const ast::Expr& taskExpr) {
    // §30.5 Pattern A admitted owning locations. Classified by the operand's
    // DECLARATION (not by the transient dataflow state) so the result is
    // stable regardless of where in the analysis it is queried:
    //
    //   - a `using var`/`await using var`-bound local of `Task<T>` — the
    //     canonical form (LocalSymbol::isUsing). Its scope-end disposal bounds
    //     the borrow the await produces.
    //   - an `owned`/`out owned` parameter (its lifetime spans the body); a
    //     plain BORROW parameter is admitted too — its source is the caller's
    //     frame, which outlives the body, so the borrow cannot dangle.
    //   - a for-loop iteration variable `for var task in tasks` (a LocalSymbol
    //     whose declaration is NOT a `using` LocalDecl and not the producer-
    //     bound plain `var`): the loop var borrows from the iterated
    //     collection, which bounds it.
    //   - an `owned` field of `this` (exprIsOwnedFieldRead).
    //   - an owning-collection indexer slot `coll[i]` (indexerSetterTakesOwned).
    //
    // A plain `var t := <producer>()` (a non-`using` LocalDecl with an
    // initializer) is NOT an owning location — it is the bare-producer form the
    // rule rejects, just expressed through one binding; the borrow it would
    // yield has no bounded source. // per §30.5
    if (const auto* id = dynamic_cast<const ast::IdentifierExpr*>(&taskExpr)) {
        sema::Symbol* sym = ctx_.exprSymbol(*id);
        if (dynamic_cast<sema::ParameterSymbol*>(sym)) {
            // Any parameter (owned, out-owned, or plain borrow) is an owning
            // location: its storage outlives the body. // per §30.5
            return true;
        }
        if (auto* ls = dynamic_cast<sema::LocalSymbol*>(sym)) {
            if (ls->isUsing) return true;  // `using var` task local
            // A for-loop iteration variable's LocalSymbol declaration is not a
            // `LocalDecl` (it is introduced by the ForStmt); admit it. A plain
            // `var t := producer()` is a LocalDecl WITH an initializer — NOT an
            // owning location.
            const auto* ld = dynamic_cast<const ast::LocalDecl*>(ls->decl);
            if (!ld) return true;            // iteration variable (or similar)
            if (!ld->initializer) return true;  // `var t: Task<T>;` slot, owned by scope
            return false;                    // plain producer-bound `var`
        }
        // Not a local/parameter (e.g. a bare field) — owned-field read?
        return exprIsOwnedFieldRead(taskExpr);
    }
    // `obj.f` owned-field read, or `this.f`.
    if (dynamic_cast<const ast::MemberAccessExpr*>(&taskExpr)) {
        return exprIsOwnedFieldRead(taskExpr);
    }
    // `coll[i]` on an owning collection.
    if (dynamic_cast<const ast::IndexExpr*>(&taskExpr)) {
        return indexerSetterTakesOwned(taskExpr);
    }
    // Anything else — a bare producer call, an arbitrary expression — is NOT
    // a syntactically-visible owning location. // per §30.5
    return false;
}

const ast::UnaryExpr* MethodOwnership::awaitRoot(const ast::Expr& e) {
    if (auto* ue = dynamic_cast<const ast::UnaryExpr*>(&e))
        if (ue->op == "await") return ue;
    return nullptr;
}

void MethodOwnership::collectOwnedConsumingAwaits(
    const std::vector<std::unique_ptr<ast::Stmt>>& body,
    std::unordered_set<const ast::UnaryExpr*>& out) {
    for (const auto& s : body)
        if (s) collectOwnedConsumingAwaitsStmt(*s, out);
}

void MethodOwnership::collectOwnedConsumingAwaitsStmt(
    const ast::Stmt& s, std::unordered_set<const ast::UnaryExpr*>& out) {
    // §30.5 owned-consuming contexts where a Pattern B `await` is the
    // IMMEDIATE value. Recurse into nested blocks so awaits in branches/loops
    // are classified too.
    if (auto* ld = dynamic_cast<const ast::LocalDecl*>(&s)) {
        // `using var x := await ...`, `await using var x := await ...`, and
        // plain `var x := await ...` are all owned-consuming (§30.5: a
        // `using var`/`await using var` RHS, and `var x := <owned>` which
        // introduces an owned local). // per §30.5
        if (ld->initializer)
            if (auto* a = awaitRoot(*ld->initializer)) out.insert(a);
        return;
    }
    if (auto* as = dynamic_cast<const ast::AssignmentStmt*>(&s)) {
        // `target := await ...` is owned-consuming when the target is an owned
        // storage location — a local (re-assigned owned local), an `owned`
        // field, or an owning-collection slot. We admit a plain local target
        // (parallels `var x := <owned>`) and the owned-field / owned-slot
        // targets the §28.3/§28.11 rules already treat as owned sinks.
        // // per §30.5, §28.3, §28.11
        if (as->op == ":=" && as->value) {
            if (auto* a = awaitRoot(*as->value)) {
                bool ownedTarget =
                    as->target &&
                    (dynamic_cast<const ast::IdentifierExpr*>(as->target.get()) ||
                     targetIsOwnedField(*as->target) ||
                     indexerSetterTakesOwned(*as->target));
                if (ownedTarget) out.insert(a);
            }
        }
        return;
    }
    if (auto* rs = dynamic_cast<const ast::ReturnStmt*>(&s)) {
        // `return await ...` — owned-consuming when the method's return type
        // carries ownership (an `owned T` / `OwnedTask<T>` return). The
        // enclosing method is the one being analysed; an async method whose
        // result is Pattern B forwards the owned result (§30.5 line ~6143).
        // We admit `return await ...` unconditionally: a `return` of an owned
        // value is an owned-consuming position by definition, and a Pattern A
        // `return await Producer()` is also explicitly admitted (§30.5 line
        // ~6068) — recorded in returnPositionAwaits_ so the Pattern A site
        // check admits the bare-producer operand in that one position.
        // // per §30.5
        if (rs->value)
            if (auto* a = awaitRoot(*rs->value)) {
                out.insert(a);
                returnPositionAwaits_.insert(a);
            }
        return;
    }
    if (auto* ys = dynamic_cast<const ast::YieldStmt*>(&s)) {
        // `yield return await ...` in an owned-yielding iterator (§30.5 line
        // ~6106). Admitted as an owned-consuming position. // per §30.5
        if (ys->value)
            if (auto* a = awaitRoot(*ys->value)) out.insert(a);
        return;
    }
    if (auto* es = dynamic_cast<const ast::ExpressionStmt*>(&s)) {
        // `coll.Add(await ...)` / `M(await ...)` where the formal is `owned`:
        // the `await` is an argument bound to an owned parameter — an
        // owned-consuming position (§30.5). Recognise the owned-argument shape.
        if (es->expr) collectOwnedConsumingInExpr(*es->expr, out);
        return;
    }
    // Recurse into control-flow blocks.
    if (auto* ifs = dynamic_cast<const ast::IfStmt*>(&s)) {
        collectOwnedConsumingAwaits(ifs->ifBranch.body, out);
        for (const auto& el : ifs->elifBranches)
            collectOwnedConsumingAwaits(el.body, out);
        collectOwnedConsumingAwaits(ifs->elseBody, out);
        return;
    }
    if (auto* ws = dynamic_cast<const ast::WhileStmt*>(&s)) {
        collectOwnedConsumingAwaits(ws->body, out);
        return;
    }
    if (auto* fs = dynamic_cast<const ast::ForStmt*>(&s)) {
        collectOwnedConsumingAwaits(fs->body, out);
        return;
    }
    if (auto* dob = dynamic_cast<const ast::DoEndBlock*>(&s)) {
        collectOwnedConsumingAwaits(dob->body, out);
        return;
    }
    if (auto* ts = dynamic_cast<const ast::TryStmt*>(&s)) {
        collectOwnedConsumingAwaits(ts->body, out);
        for (const auto& arm : ts->catches)
            if (arm.body) collectOwnedConsumingAwaitsStmt(*arm.body, out);
        collectOwnedConsumingAwaits(ts->finallyBody, out);
        return;
    }
}

// §30.5: an `await` passed as an `owned` argument is in an owned-consuming
// context. Walk a call expression's arguments and record any await bound to
// an `owned` formal (user `owned` param or a built-in owning-collection
// owned-sink method). Recurse into the callee/receiver so a chained call's
// inner await is examined too. // per §30.5
void MethodOwnership::collectOwnedConsumingInExpr(
    const ast::Expr& e, std::unordered_set<const ast::UnaryExpr*>& out) {
    if (auto* call = dynamic_cast<const ast::CallExpr*>(&e)) {
        std::size_t pos = 0;
        for (const auto& a : call->args) {
            if (a.value) {
                if (auto* aw = awaitRoot(*a.value)) {
                    const bool ownedSink =
                        a.modifier == "owned" ||
                        isImplicitOwned(call->args, pos, call->callee.get()) ||
                        builtinOwnedSinkArg(call->callee.get(), pos);
                    if (ownedSink) out.insert(aw);
                }
                collectOwnedConsumingInExpr(*a.value, out);
            }
            ++pos;
        }
        if (call->callee) collectOwnedConsumingInExpr(*call->callee, out);
        return;
    }
    if (auto* ne = dynamic_cast<const ast::NewExpr*>(&e)) {
        // `new T(owned await ...)` — a ctor `owned` param argument.
        const ast::ConstructorDecl* cd = nullptr;
        if (ne->type) {
            if (auto* rt = ctx_.resolvedType(*ne->type)) {
                if (rt->declaration) {
                    std::size_t nArgs = ne->args.size();
                    for (sema::Symbol* m : rt->declaration->members) {
                        if (auto* cs =
                                dynamic_cast<sema::ConstructorSymbol*>(m)) {
                            auto* d = dynamic_cast<const ast::ConstructorDecl*>(
                                cs->decl);
                            if (d && d->params.size() == nArgs) { cd = d; break; }
                        }
                    }
                }
            }
        }
        std::size_t pos = 0;
        for (const auto& a : ne->args) {
            if (a.value) {
                if (auto* aw = awaitRoot(*a.value)) {
                    bool ownedSink = a.modifier == "owned" ||
                        (cd && pos < cd->params.size() &&
                         (cd->params[pos].modifier == "owned" ||
                          cd->params[pos].modifier == "out owned"));
                    if (ownedSink) out.insert(aw);
                }
                collectOwnedConsumingInExpr(*a.value, out);
            }
            ++pos;
        }
        return;
    }
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
        if (ma->target) collectOwnedConsumingInExpr(*ma->target, out);
        return;
    }
}

void MethodOwnership::checkAwaitSites(
    const std::vector<std::unique_ptr<ast::Stmt>>& body) {
    std::unordered_set<const ast::UnaryExpr*> consuming;
    returnPositionAwaits_.clear();
    collectOwnedConsumingAwaits(body, consuming);  // also fills returnPositionAwaits_
    for (const auto& s : body)
        if (s) checkAwaitSitesStmt(*s, consuming);
}

void MethodOwnership::checkAwaitSitesStmt(
    const ast::Stmt& s,
    const std::unordered_set<const ast::UnaryExpr*>& consuming) {
    // Walk every statement's expressions for awaits, and recurse into nested
    // blocks. Mirrors the structural recursion the other whole-body walks use.
    if (auto* ld = dynamic_cast<const ast::LocalDecl*>(&s)) {
        if (ld->initializer) checkAwaitSitesExpr(*ld->initializer, consuming);
        return;
    }
    if (auto* es = dynamic_cast<const ast::ExpressionStmt*>(&s)) {
        if (es->expr) checkAwaitSitesExpr(*es->expr, consuming);
        return;
    }
    if (auto* ds = dynamic_cast<const ast::DiscardStmt*>(&s)) {
        if (ds->value) checkAwaitSitesExpr(*ds->value, consuming);
        return;
    }
    if (auto* as = dynamic_cast<const ast::AssignmentStmt*>(&s)) {
        if (as->value) checkAwaitSitesExpr(*as->value, consuming);
        if (as->target) checkAwaitSitesExpr(*as->target, consuming);
        return;
    }
    if (auto* rs = dynamic_cast<const ast::ReturnStmt*>(&s)) {
        if (rs->value) checkAwaitSitesExpr(*rs->value, consuming);
        return;
    }
    if (auto* th = dynamic_cast<const ast::ThrowStmt*>(&s)) {
        if (th->value) checkAwaitSitesExpr(*th->value, consuming);
        return;
    }
    if (auto* ys = dynamic_cast<const ast::YieldStmt*>(&s)) {
        if (ys->value) checkAwaitSitesExpr(*ys->value, consuming);
        return;
    }
    if (auto* ifs = dynamic_cast<const ast::IfStmt*>(&s)) {
        if (ifs->ifBranch.condition)
            checkAwaitSitesExpr(*ifs->ifBranch.condition, consuming);
        for (const auto& bs : ifs->ifBranch.body)
            if (bs) checkAwaitSitesStmt(*bs, consuming);
        for (const auto& el : ifs->elifBranches) {
            if (el.condition) checkAwaitSitesExpr(*el.condition, consuming);
            for (const auto& bs : el.body)
                if (bs) checkAwaitSitesStmt(*bs, consuming);
        }
        for (const auto& bs : ifs->elseBody)
            if (bs) checkAwaitSitesStmt(*bs, consuming);
        return;
    }
    if (auto* ws = dynamic_cast<const ast::WhileStmt*>(&s)) {
        if (ws->condition) checkAwaitSitesExpr(*ws->condition, consuming);
        for (const auto& bs : ws->body)
            if (bs) checkAwaitSitesStmt(*bs, consuming);
        return;
    }
    if (auto* fs = dynamic_cast<const ast::ForStmt*>(&s)) {
        if (fs->iterable) checkAwaitSitesExpr(*fs->iterable, consuming);
        for (const auto& bs : fs->body)
            if (bs) checkAwaitSitesStmt(*bs, consuming);
        return;
    }
    if (auto* dob = dynamic_cast<const ast::DoEndBlock*>(&s)) {
        for (const auto& bs : dob->body)
            if (bs) checkAwaitSitesStmt(*bs, consuming);
        return;
    }
    if (auto* ts = dynamic_cast<const ast::TryStmt*>(&s)) {
        for (const auto& bs : ts->body)
            if (bs) checkAwaitSitesStmt(*bs, consuming);
        for (const auto& arm : ts->catches)
            if (arm.body) checkAwaitSitesStmt(*arm.body, consuming);
        for (const auto& bs : ts->finallyBody)
            if (bs) checkAwaitSitesStmt(*bs, consuming);
        return;
    }
}

void MethodOwnership::checkAwaitSitesExpr(
    const ast::Expr& e,
    const std::unordered_set<const ast::UnaryExpr*>& consuming) {
    // Diagnose this await (if it is one), then recurse into sub-expressions so
    // a nested await (`Process(await t)`, `a + await t`) is examined too.
    if (auto* ue = dynamic_cast<const ast::UnaryExpr*>(&e)) {
        if (ue->op == "await") {
            AwaitTaskKind k = awaitTaskKind(*ue);
            if (k == AwaitTaskKind::PatternA) {
                // §30.5: the awaited `Task<T>` (class T) must be in an owning
                // location — OR the await is a `return await Producer()`, the
                // one admitted bare-producer Pattern A position (§30.5 line
                // ~6068, recorded in returnPositionAwaits_). // per §30.5
                if (returnPositionAwaits_.count(ue) == 0 &&
                    (!ue->operand ||
                     !awaitOperandIsOwningLocation(*ue->operand))) {
                    report(
                        Severity::Error, ue->span.begin,
                        "awaiting a class-typed `Task<T>` (Pattern A) requires "
                        "the awaited task to occupy a syntactically-visible "
                        "owning location — a `using var`-bound task local, an "
                        "`owned` parameter, an `owned` field, or an owning-"
                        "collection slot — so the borrowed result has a source "
                        "(§30.5; bind the producer first: `using var t := "
                        "Producer(); var r := await t;`)");
                }
            } else if (k == AwaitTaskKind::PatternB) {
                // §30.5: the awaited `OwnedTask<T>` must be in an owned-
                // consuming context, else its transferred result is dropped.
                // // per §30.5
                if (consuming.count(ue) == 0) {
                    report(
                        Severity::Error, ue->span.begin,
                        "awaiting an `OwnedTask<T>` (Pattern B) must appear in "
                        "an owned-consuming context — a `using var`/`var` "
                        "binding, an `owned` argument, an owned-field/owned-slot "
                        "assignment, or an owned `return` — otherwise the "
                        "ownership-transferring result is dropped without "
                        "disposal (§30.5; the canonical form is `using var x := "
                        "await ...`)");
                }
            }
            // UnitTask / ValueResult / NotTask carry no obligation at the site.
        }
        if (ue->operand) checkAwaitSitesExpr(*ue->operand, consuming);
        return;
    }
    if (auto* be = dynamic_cast<const ast::BinaryExpr*>(&e)) {
        if (be->left) checkAwaitSitesExpr(*be->left, consuming);
        if (be->right) checkAwaitSitesExpr(*be->right, consuming);
        return;
    }
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
        if (ma->target) checkAwaitSitesExpr(*ma->target, consuming);
        return;
    }
    if (auto* nm = dynamic_cast<const ast::NullCondMemberExpr*>(&e)) {
        if (nm->target) checkAwaitSitesExpr(*nm->target, consuming);
        return;
    }
    if (auto* call = dynamic_cast<const ast::CallExpr*>(&e)) {
        if (call->callee) checkAwaitSitesExpr(*call->callee, consuming);
        for (const auto& a : call->args)
            if (a.value) checkAwaitSitesExpr(*a.value, consuming);
        return;
    }
    if (auto* nc = dynamic_cast<const ast::NullCondCallExpr*>(&e)) {
        if (nc->callee) checkAwaitSitesExpr(*nc->callee, consuming);
        for (const auto& a : nc->args)
            if (a.value) checkAwaitSitesExpr(*a.value, consuming);
        return;
    }
    if (auto* ix = dynamic_cast<const ast::IndexExpr*>(&e)) {
        if (ix->target) checkAwaitSitesExpr(*ix->target, consuming);
        for (const auto& a : ix->indices)
            if (a.value) checkAwaitSitesExpr(*a.value, consuming);
        return;
    }
    if (auto* bg = dynamic_cast<const ast::PostfixBangExpr*>(&e)) {
        if (bg->operand) checkAwaitSitesExpr(*bg->operand, consuming);
        return;
    }
    if (auto* ne = dynamic_cast<const ast::NewExpr*>(&e)) {
        for (const auto& a : ne->args)
            if (a.value) checkAwaitSitesExpr(*a.value, consuming);
        return;
    }
    if (auto* cd = dynamic_cast<const ast::ConditionalExpr*>(&e)) {
        if (cd->condition) checkAwaitSitesExpr(*cd->condition, consuming);
        if (cd->thenValue) checkAwaitSitesExpr(*cd->thenValue, consuming);
        if (cd->elseValue) checkAwaitSitesExpr(*cd->elseValue, consuming);
        return;
    }
    if (auto* te = dynamic_cast<const ast::TypeTestExpr*>(&e)) {
        if (te->operand) checkAwaitSitesExpr(*te->operand, consuming);
        return;
    }
    // A LambdaExpr is its own async scope (§30.6) — its awaits are checked when
    // its body is analysed, not here. Other leaf expressions hold no awaits.
}

// §30.5 / §30.4 (#834): true when `e`'s static type is an `OwnedTask<...>`
// (name-head), the single-consumer owning task whose await consumes it. Used
// to record the operand consume of an `await task` independently of the
// result-kind gate in awaitTaskKind, which cannot see through a generic result
// type argument T (the `WhenAllOwned<T>` drain-and-await pattern). A Named
// TypeRef carries its name on the AST node or, for an imported/generic
// argument, in importedName; check both.
bool MethodOwnership::operandIsOwnedTaskType(const ast::Expr& e) {
    const sema::TypeRef* ot = ctx_.exprType(e);
    if (!ot || ot->kind != sema::TypeRef::Kind::Named || ot->nullable)
        return false;
    std::string_view head;
    if (ot->astNode && ot->astNode->name &&
        ot->astNode->name->parts.size() == 1)
        head = ot->astNode->name->parts.front();
    else if (!ot->importedName.empty())
        head = ot->importedName;
    return head == "OwnedTask";
}

void MethodOwnership::recordAwaitOperandConsume(
    const ast::Expr& awaitOperand, SourceLocation loc) {
    // §30.5 line ~6110: the Pattern B await is owned-consuming with respect to
    // its operand. When the operand is a bare owned task LOCAL, the await
    // consumes (disposes) it. Marking it Disposed makes a straight-line SECOND
    // `await t` of the same bound local a read of a consumed value — the
    // statically-detectable slice of the §30.4 single-consumer rule. Only the
    // bare-identifier owned-local form is handled (an owned PARAMETER or a
    // producer-call operand has no single local to spend a second time in
    // straight-line code; the general aliased / cross-method second await is
    // not statically preventable per Appendix C.6). // per §30.5, §30.4
    const auto* id = dynamic_cast<const ast::IdentifierExpr*>(&awaitOperand);
    if (!id) return;
    LocalOwnership* st = lookupState(id->name);
    if (!st) return;
    if (st->state == OwnershipState::Disposed ||
        st->state == OwnershipState::MovedFrom) {
        // The task local was already consumed by an earlier await — this is the
        // double-consume. // per §30.5, §30.4
        report(
            Severity::Error, loc,
            std::string("`OwnedTask<T>` `") + std::string(id->name) +
            "` was already awaited (consumed) — an `OwnedTask<T>` is "
            "single-consumer; awaiting it again moves out an already-moved "
            "result (§30.5 / §30.4)");
        st->state = OwnershipState::Poisoned;
        return;
    }
    // Mark the owned task local consumed by the await's implicit disposal
    // (§30.5: the implicit task-disposal IS the discharge). A `using var` task
    // local goes to Disposed without complaint (markDisposedByName tolerates
    // the using-var implicit-discharge state); a plain owned local likewise.
    if (st->state == OwnershipState::Owned)
        st->state = OwnershipState::Disposed;
}

void MethodOwnership::annotateSuspendLocals(const flow::CFG& cfg) {
    // §30.5 / §30.9: an owned local is a suspend-local when its live range
    // crosses an `await`. The CFG (3a) records, per block, the await sites it
    // contains (`awaitSites`). A precise live-range computation is the codegen
    // milestone's concern; for the annotation it is sufficient — and sound for
    // the "store in the frame / dispose on every completion path" decision — to
    // mark every owned local whose DECLARATION lexically precedes an await and
    // whose discharge does not precede that await within the same block. Since
    // the annotation is informational for the ownership pass (the diagnostics
    // flow from the lattice + completion edges), we use a conservative
    // syntactic approximation: a local declared before any await-bearing block
    // (in CFG order) is a candidate suspend-local. The flag is set on the
    // declaration's LocalOwnership as it is created in handleLocalDecl; here we
    // pre-seed the set of locals that are "in scope at some await" for that
    // handler to consult. Kept deliberately simple: over-marking a local as a
    // suspend-local is safe (it only over-includes a local in the frame), and
    // the precise liveness is the codegen milestone's contract.
    //
    // Implementation: collect every LocalDecl that textually appears before an
    // await in the body's statement stream, by walking blocks and noting,
    // once any await has been seen, that subsequently-encountered locals are
    // NOT suspend-locals for an EARLIER await but locals seen before remain.
    // We mark on the actual LocalOwnership lazily in the dataflow; to keep this
    // additive and avoid threading state, we record the declaring nodes in a
    // member set the handler reads. (No behaviour depends on it beyond the
    // annotation, so the approximation does not affect diagnostics.)
    suspendLocalDecls_.clear();
    bool sawAwait = false;
    // Walk blocks in allocation order; a LocalDecl encountered before the first
    // await-bearing position is potentially live across it. We collect decls in
    // the prefix up to (and including the block of) each await. The simplest
    // sound over-approximation: any local whose decl is in a block at or before
    // an await-bearing block is a suspend-local candidate.
    // First pass: find whether the body has any await at all.
    for (const auto& bp : cfg.blocks())
        if (flow::CFG::hasAwaitSites(bp.get())) { sawAwait = true; break; }
    if (!sawAwait) return;  // no suspend points → no suspend-locals
    // Collect, per block, the LocalDecls it declares; then for each await-
    // bearing block, every local declared in a block that can reach it is a
    // suspend-local candidate. Using "declared in allocation order before the
    // last await block" as the over-approximation keeps this simple and sound.
    std::size_t lastAwaitIdx = 0, idx = 0;
    std::vector<const flow::CFGBlock*> order;
    for (const auto& bp : cfg.blocks()) order.push_back(bp.get());
    for (idx = 0; idx < order.size(); ++idx)
        if (flow::CFG::hasAwaitSites(order[idx])) lastAwaitIdx = idx;
    for (idx = 0; idx <= lastAwaitIdx && idx < order.size(); ++idx) {
        for (const ast::Stmt* st : order[idx]->stmts) {
            if (auto* ld = dynamic_cast<const ast::LocalDecl*>(st))
                suspendLocalDecls_.insert(ld);
        }
    }
}

void MethodOwnership::addCompletionEdges(flow::CFG& cfg) {
    // §30.5 / §30.8 / §30.9: from each await-bearing block, the task can
    // complete via abandonment or cancellation, on which the state-machine
    // frame disposes its suspend-locals. Represent that with a synthetic
    // completion edge from the await-bearing block to the function exit, so the
    // dataflow gives a still-owned suspend-local arriving on that edge the same
    // frame-disposal treatment as the §28.3 exception-path edges (dropped from
    // the meet, not reported as a leak).
    //
    // The edge is added ONLY from an await-bearing block that does NOT already
    // reach the exit by a normal (fall-off / return) edge. Rationale: a block
    // that already has a normal edge to exit has its NORMAL-completion path
    // analysed there, which is the strictest obligation — the author must
    // discharge owned locals on it. The abandonment/cancellation completion at
    // such a terminal block is subsumed (the frame would dispose whatever is
    // live, but the normal path already requires it). Adding a *parallel*
    // completion edge to exit there would create two same-target edges of
    // different kinds, which the set-membership edge-kind predicate cannot tell
    // apart — it would mis-classify the normal edge as a discharge edge and
    // MASK a real normal-path leak. So the synthetic edge is added only where
    // it is meaningful and unambiguous: an await block whose control flows
    // ONWARD (into later blocks), where the completion edge represents "the
    // task could be abandoned/cancelled at this await before reaching the later
    // disposal," and contributes a pure-discharge (subset) state to the exit
    // meet. There it changes no leak diagnostic (the onward normal path still
    // governs), but it materialises the abandonment/cancellation exit in the
    // CFG as the recorded model the async-codegen milestone consumes.
    // // per §30.5, §30.8, §30.9
    flow::CFGBlock* exit = cfg.exit();
    auto hasNormalEdgeToExit = [&](const flow::CFGBlock* b) {
        for (const flow::CFGBlock* s : b->succs)
            if (s == exit && !flow::CFG::isExceptionEdge(b, exit) &&
                !flow::CFG::isCompletionEdge(b, exit))
                return true;
        return false;
    };
    for (const auto& bp : cfg.blocks()) {
        flow::CFGBlock* b = bp.get();
        if (b == exit) continue;
        if (flow::CFG::hasAwaitSites(b) && !hasNormalEdgeToExit(b))
            cfg.addCompletionEdge(b, exit);
    }
}

} // namespace bezel::ownership
