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
                    rt->headName() == "Shared" &&
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
            // §28.8 / §28.13: is this argument position an owned sink? A ctor
            // `owned` (or `out owned`) formal, or the implicit owned parameter
            // of `new Shared<T>(value)` at position 0. The `owned`/`move`/borrow
            // classification below is identical to the method-call path
            // (recordMovesInArgs); the call-site `owned` marker (§22.3, v0.169)
            // is layered on top of the SAME classification.
            const bool ctorOwnedSink =
                cd && pos < cd->params.size() &&
                (cd->params[pos].modifier == "owned" ||
                 cd->params[pos].modifier == "out owned");
            const bool ownedSink =
                ctorOwnedSink || (isSharedNew && pos == 0);
            // §22.3 (v0.169, #1348): the `owned` marker is only valid on an
            // argument to an `owned` parameter. On any other ctor position it
            // is spurious.
            if (a.modifier == "owned" && !ownedSink) {
                reportSpuriousOwnedMarker(a.value->span.begin,
                                          /*onFreshProducer=*/false);
                recordMovesIn(*a.value);
                ++pos; continue;
            }
            if (ownedSink) {
                if (auto* id =
                        dynamic_cast<const ast::IdentifierExpr*>(a.value.get())) {
                    // §28.8 (#395): a borrow flowing into the owned formal is
                    // rejected regardless of the marker.
                    if (checkOwnedArgSource(*a.value, id->span.begin)) {
                        ++pos; continue;
                    }
                    // A valid owned source. §22.3 (v0.169): if it is a NAMED
                    // OWNED BINDING the call leaves it moved-from, so the marker
                    // is REQUIRED; a fresh producer (not a bare Owned binding)
                    // is unmarked and handled by the fall-through below.
                    if (a.modifier != "owned" && isNamedOwnedBinding(*a.value))
                        reportMissingOwnedMarker(id->name, id->span.begin);
                    markMovedByName(id->name, id->span.begin);
                    ++pos; continue;
                }
                if (exprIsOwnedFieldRead(*a.value)) {
                    // §28.3 / §28.8.1 (#398): a bare owned-field read is a borrow
                    // (only `move obj.f` is owned).
                    checkOwnedArgSource(*a.value, a.value->span.begin);
                    ++pos; continue;
                }
                if (checkOwnedArgOtherBorrow(*a.value)) {
                    // §28.3 / §28.11 (#407): a bare `coll[i]` / `obj.f` borrow.
                    ++pos; continue;
                }
                // Fresh producer (`new`, a call returning `owned T`, an operator
                // result, `move obj.f`). §22.3 (v0.169): the marker is REJECTED
                // here — no live binding is invalidated.
                if (a.modifier == "owned")
                    reportSpuriousOwnedMarker(a.value->span.begin,
                                              /*onFreshProducer=*/true);
                recordMovesIn(*a.value);
                ++pos; continue;
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
        // §28.8: is this argument position an owned sink? A user-declared
        // `owned` formal, or a built-in owning-collection method whose owned
        // arg sits at this position (#395: built-in methods have no AST decl).
        // §28.3 / §28.11 (#860): an `out owned` formal is the owned-OUT
        // direction (the argument RECEIVES ownership, marked Owned post-call by
        // markOutOwnedArgs) — NOT an owned-IN sink; only a plain `owned` formal
        // consumes the argument here.
        const std::vector<ast::Param>* params = nullptr;
        if (calleeMethod)  params = &calleeMethod->params;
        if (calleeCtor)    params = &calleeCtor->params;
        const bool userOwned =
            params && pos < params->size() &&
            (*params)[pos].modifier == "owned";
        const bool ownedSink = userOwned || builtinOwnedSinkArg(callee, pos);

        // §22.3 (v0.169, #1348): the `owned` call-site marker is valid ONLY on
        // an argument to an `owned` parameter. On any other position it is
        // spurious (its transfer semantics do not apply — the argument is not
        // consumed — so treat it as a plain argument after reporting).
        if (a.modifier == "owned" && !ownedSink) {
            reportSpuriousOwnedMarker(a.value->span.begin,
                                      /*onFreshProducer=*/false);
            recordMovesIn(*a.value);
            ++pos; continue;
        }
        // §28.8 owned-sink argument. The `owned`/`move`/borrow/fresh-producer
        // classification is exactly the pre-v0.169 one; the call-site `owned`
        // marker (§22.3, v0.169) is layered on top of the SAME classification.
        if (ownedSink) {
            if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(
                    a.value.get())) {
                // §28.8 (#395): a borrow flowing into the owned formal is
                // rejected regardless of the marker.
                if (checkOwnedArgSource(*a.value, id->span.begin)) {
                    ++pos; continue;
                }
                // A valid owned source. §22.3 (v0.169): a NAMED OWNED BINDING is
                // left moved-from by the call, so the marker is REQUIRED. (A
                // fresh producer never reaches here as a bare identifier.)
                if (a.modifier != "owned" && isNamedOwnedBinding(*a.value))
                    reportMissingOwnedMarker(id->name, id->span.begin);
                markMovedByName(id->name, id->span.begin);
                ++pos; continue;
            }
            if (exprIsOwnedFieldRead(*a.value)) {
                // §28.3 / §28.8.1 (#398): a bare owned-field read is a borrow
                // (only `move obj.f` is owned).
                checkOwnedArgSource(*a.value, a.value->span.begin);
                ++pos; continue;
            }
            if (checkOwnedArgOtherBorrow(*a.value)) {
                // §28.3 / §28.11 (#407): a bare `coll[i]` / `obj.f` borrow.
                ++pos; continue;
            }
            // `move id` to an owned formal: `move b` of a borrow is not an
            // owned value (§28.8/#395). A legitimate `move` of an owned value
            // falls through to recordMovesIn, which records the move.
            if (auto* un = dynamic_cast<const ast::UnaryExpr*>(
                    a.value.get())) {
                if (un->op == "move" &&
                    dynamic_cast<const ast::IdentifierExpr*>(
                        un->operand.get()) &&
                    checkOwnedArgSource(*a.value, un->span.begin)) {
                    ++pos; continue;  // reported; skip the bogus move
                }
            }
            // Fresh producer (`new`, a call returning `owned T`, an operator
            // result, `move obj.f`). §22.3 (v0.169): the marker is REJECTED —
            // no live binding is invalidated.
            if (a.modifier == "owned")
                reportSpuriousOwnedMarker(a.value->span.begin,
                                          /*onFreshProducer=*/true);
            recordMovesIn(*a.value);
            ++pos; continue;
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
            // §22.3 (v0.169, #1348): call-site `owned` marker validation. This
            // is SURFACE-ONLY — it does not change which arguments are consumed
            // (the transfer is modelled by the unchanged logic below); it only
            // requires/rejects the spelling, reusing the existing named-owned-
            // binding / borrow classification. An owned-sink position is a user
            // `owned` formal (isImplicitOwned) or a builtin owned-sink method.
            const bool ownedSink =
                isImplicitOwned(call->args, pos, call->callee.get()) ||
                builtinOwnedSinkArg(call->callee.get(), pos);
            if (a.modifier == "owned" && !ownedSink) {
                // Spurious marker on a non-owned parameter position: reject, and
                // do NOT consume (this position does not take ownership).
                reportSpuriousOwnedMarker(a.value->span.begin,
                                          /*onFreshProducer=*/false);
                checkRead(*a.value, /*selfTarget=*/false);
                ++pos; continue;
            }
            if (ownedSink) {
                const bool namedBinding = isNamedOwnedBinding(*a.value);
                if (a.modifier == "owned") {
                    // Marker present. It is valid on a named owned binding;
                    // rejected on a fresh producer. A borrow carries its own
                    // §28.8 diagnostic (checkOwnedArgSource, in the logic below).
                    if (!namedBinding && !ownedArgSourceIsBorrow(*a.value))
                        reportSpuriousOwnedMarker(a.value->span.begin,
                                                  /*onFreshProducer=*/true);
                } else if (a.modifier.empty() && namedBinding) {
                    // Missing marker on a named owned binding left moved-from.
                    const auto* id =
                        dynamic_cast<const ast::IdentifierExpr*>(a.value.get());
                    reportMissingOwnedMarker(id->name, id->span.begin);
                }
            }
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
            // (`ownedSink` was computed above for the §22.3 marker validation.)
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
                // §30.5 / §30.4 (#834): an `await <bound owned `OwningTask`
                // local>` ARGUMENT consumes its operand task local — an
                // `OwningTask<T>` is single-consumer and the await disposes it
                // (the implicit task-disposal IS the discharge). This is the
                // §29.2 `for var task in src.Drain() do results.Add(await task)
                // end` drain-and-await pattern (stdlib §2.13 `WhenAllOwned<T>`):
                // the owned iteration variable `task` is discharged by the await
                // inside the `Add(owned item)` owned-consuming context. Without
                // recording the consume here the await arg only reached
                // checkRead (which does not consume), so `task` was wrongly
                // reported as dropped without disposal at the loop's end. Gated
                // on the operand's STATIC TYPE being `OwningTask` rather than on
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
                // §28.3 (#1500): the null-forgiving receiver spelling
                // `x!.Dispose()` — required to discharge a NULLABLE owned
                // local (e.g. the result of `move` on a `[N owned]T?` slot,
                // §15.1.1) — parses as a MemberAccessExpr whose target is a
                // PostfixBangExpr wrapping the identifier. Look through the
                // `!` the same way `move x!` already does (lookThroughForce-
                // Unwrap, #1125) so this counts as the explicit-disposal
                // discharge just like the bare `x.Dispose()` form. // per §28.3
                if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(
                        lookThroughForceUnwrap(ma->target.get()))) {
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
            // §22.3 (v0.169, #1348): call-site `owned` marker validation for a
            // constructor call, mirroring the CallExpr path above — surface-only,
            // the consumption logic below is unchanged. An owned sink here is a
            // ctor `owned` (or `out owned`) formal.
            const bool ctorOwnedSink =
                cd && pos < cd->params.size() &&
                (cd->params[pos].modifier == "owned" ||
                 cd->params[pos].modifier == "out owned");
            if (a.modifier == "owned" && !ctorOwnedSink) {
                reportSpuriousOwnedMarker(a.value->span.begin,
                                          /*onFreshProducer=*/false);
                checkRead(*a.value, /*selfTarget=*/false);
                ++pos; continue;
            }
            if (ctorOwnedSink) {
                const bool namedBinding = isNamedOwnedBinding(*a.value);
                if (a.modifier == "owned") {
                    if (!namedBinding && !ownedArgSourceIsBorrow(*a.value))
                        reportSpuriousOwnedMarker(a.value->span.begin,
                                                  /*onFreshProducer=*/true);
                } else if (a.modifier.empty() && namedBinding) {
                    const auto* id =
                        dynamic_cast<const ast::IdentifierExpr*>(a.value.get());
                    reportMissingOwnedMarker(id->name, id->span.begin);
                }
            }
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
    // element (e.g. an OwningArray<R> slot, §28.3) stays a borrow and is still
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
            // §28.14 v0.177 (#1419): a closure argument bound to a VERIFIED
            // call-bounded formal (a user-authored plain function-typed
            // parameter) cannot outlive the call — the callee-side pre-pass
            // rejects every retention. `return Apply(() => res.Val());`
            // therefore escapes nothing, and the caller side is unrestricted.
            // Arguments to BUILTIN callees keep the legacy rejection: the
            // lazy query operators' returned query objects retain the
            // callback until enumeration (stdlib-certified storing
            // signatures, the §30.11 legacy path), so a capture-bearing
            // closure there still escapes with the returned value.
            // // per §28.14, §28.3 (v0.177)
            if (auto* ce = dynamic_cast<const ast::CallExpr*>(&x)) {
                for (std::size_t i = 0; i < ce->args.size(); ++i) {
                    const auto& a = ce->args[i];
                    if (!a.value) continue;
                    if (verifiedCallBoundedFormalAt(ce->callee.get(), nullptr,
                                                    i))
                        continue;
                    findLambdas(*a.value);
                }
                return;
            }
            if (auto* ne = dynamic_cast<const ast::NewExpr*>(&x)) {
                for (std::size_t i = 0; i < ne->args.size(); ++i) {
                    const auto& a = ne->args[i];
                    if (!a.value) continue;
                    if (verifiedCallBoundedFormalAt(nullptr, ne, i)) continue;
                    findLambdas(*a.value);
                }
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
    // §28.14 (v0.175): a captured source frozen by a scoped-callback admission
    // may not be disposed until the bounding scope exits.
    checkScopedFreeze(name, loc, "dispose");
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
    // §28.14 (v0.175): a captured source frozen by a scoped-callback admission
    // may not be moved from until the bounding scope exits.
    checkScopedFreeze(name, loc, "move from");
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

// §22.3 (v0.169, #1348): see the header. A named owned binding is exactly the
// argument form whose transfer into an `owned` sink leaves a LIVE binding
// moved-from — the case the call-site `owned` marker announces. We piggy-back
// on the existing moved-from state machine: a tracked local/parameter in the
// Owned state IS such a binding. Anything that is not a tracked Owned binding
// (a fresh `new`/call/operator producer, a `move obj.f`, a borrow, `this`, or a
// binding already moved/disposed) is not, so the marker is neither required nor
// (for the not-owned states) meaningfully transferable here.
bool MethodOwnership::isNamedOwnedBinding(const ast::Expr& e) {
    const auto* id = dynamic_cast<const ast::IdentifierExpr*>(&e);
    if (!id) return false;
    if (id->name == "this" || id->name == "base") return false;
    const LocalOwnership* st = lookupState(id->name);
    return st && st->state == OwnershipState::Owned;
}

void MethodOwnership::reportMissingOwnedMarker(std::string_view name,
                                               SourceLocation loc) {
    report(Severity::Error, loc,
           std::string("this call consumes `") + std::string(name) +
               "`; write `M(owned " + std::string(name) +
               ")` — passing the named owned binding `" + std::string(name) +
               "` to an `owned` parameter transfers ownership and leaves it "
               "moved-from, which the call site must announce (§22.3, v0.169)");
}

void MethodOwnership::reportSpuriousOwnedMarker(SourceLocation loc,
                                                bool onFreshProducer) {
    if (onFreshProducer)
        report(Severity::Error, loc,
               "the `owned` call-site marker is not written on a fresh producer "
               "(a `new` expression, a call returning `owned T`, an operator "
               "result, or `move obj.f`) — no live binding is invalidated, so "
               "there is nothing to announce; remove `owned` (§22.3, v0.169)");
    else
        report(Severity::Error, loc,
               "the `owned` call-site marker is only valid on an argument to an "
               "`owned` parameter; this parameter position does not take "
               "ownership — remove `owned` (§22.3, v0.169)");
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
            std::string_view cn = tt->headName();
            if (cn != "OwningArray" && cn != "OwningDictionary" &&
                cn != "OwningBuffer")
                return false;
            // per §28.11 / §4.5 / §4.14 (#1209): the indexer setter takes
            // ownership only when the element (or dictionary value) type is a
            // class or interface — value-typed elements (primitive, record,
            // enum) are COPIED into the slot, so the assignment is a copy and
            // does NOT consume the RHS local (no owned-sink obligation).
            // For OwningDictionary<K,V> the stored value is genericArgs[1];
            // for OwningArray<T>/OwningBuffer<T> the element is genericArgs[0].
            const std::size_t elemIdx = (cn == "OwningDictionary") ? 1 : 0;
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
        // OwningArray<OwningTask<T>>) takes ownership of its argument array (and
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
    const std::string_view cn = tt->headName();
    // per §30.15 / §6.9 (#600, #692): ChannelSender<T>.Send(owned item) — and
    // v0.174 (#809) SendAsync(owned item) — transfer ownership only when T is a
    // class or interface type: the channel takes ownership of the heap object
    // and the owned-sink rule must fire so a borrow is rejected. For value-typed
    // T and string the ownership modifiers drop out (§6.9: "Send(item: T)",
    // "SendAsync(item: T)") — the slot is a copy, no heap disposal obligation,
    // so the owned-sink rule must NOT fire.
    if (cn == "ChannelSender" &&
        (ma->member == "Send" || ma->member == "SendAsync") && argPos == 0) {
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
        cn == "OwningArray" || cn == "OwningDictionary" ||
        cn == "OwningSet" || cn == "OwningStack" || cn == "OwningQueue" ||
        cn == "OwningLinkedList";
    if (!isOwning) return false;
    const std::string_view m = ma->member;
    // The owned argument's position per stdlib §4.5 / §4.7 / §4.9.
    if ((m == "Add" || m == "AddFirst" || m == "AddLast" || m == "Push" ||
         m == "Enqueue" || m == "AddRange") &&
        cn != "OwningDictionary")
        return argPos == 0;                       // owned item is arg 0
    if (m == "Add" && cn == "OwningDictionary")
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
    const std::string_view cn = tt->headName();
    const std::string_view m = ma->member;
    // Owning collections — `where T: class`, so the moved-out element is
    // always a disposable reference type (stdlib §4.5 / §4.7 / §4.9).
    if (cn == "OwningStack"  && m == "TryPop")      return argPos == 0;
    if (cn == "OwningQueue"  && m == "TryDequeue")  return argPos == 0;
    if (cn == "OwningLinkedList" &&
        (m == "TryPopFirst" || m == "TryPopLast")) return argPos == 0;
    // OwningSet<T>.TryTake(query, out owned removed: T?) — out-owned at arg 1.
    if (cn == "OwningSet"    && m == "TryTake")     return argPos == 1;
    // OwningDictionary<K,V>.TryPopValue(key, out owned value: V) — arg 1.
    if (cn == "OwningDictionary" && m == "TryPopValue") return argPos == 1;
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

// #1484 / §28.6: a field initializer that stores a fresh owned value into a
// non-`owned` field. See the header. Mirrors handleAssignment's
// targetIsNonOwnedField branch exactly: an `owned` field is the §28.5 storage
// path (accepted); a non-`owned` field whose initializer is a fresh owned
// producer (producerState == Owned — `new`, a call returning `owned T`, a
// `move`, or an owned operator result) leaks and is rejected. A bare borrow
// initializer (producerState == Borrowed) is the legitimate back-reference
// store and is not flagged. The diagnostic wording matches the constructor-body
// counterpart so the two positions read identically. // per §28.6
void MethodOwnership::checkFieldInitializer(const ast::FieldDecl& fd) {
    if (!fd.initializer) return;
    // Scope: INSTANCE fields only. A STATIC field follows the §22.13 static-init
    // model (its initializer runs at first-use, its value lives for the program
    // and is not part of any instance's disposal), NOT the instance-field §28.6
    // leak rule — a non-`owned` `static readonly w: Widget := new Widget()` is a
    // legal static owner (#917/#918/§22.13), whereas the analogous instance form
    // leaks. The constructor-body counterpart this mirrors is likewise an
    // instance-field store (`this.f := …`). Skip static fields here. // per §22.13
    for (std::string_view m : fd.modifiers)
        if (m == "static") return;
    // `owned` field → §28.5 owned-storage path: the containing object disposes
    // the value, so a fresh owned initializer is exactly what it wants. Not here.
    for (std::string_view m : fd.modifiers)
        if (m == "owned") return;
    if (producerState(*fd.initializer) == OwnershipState::Owned) {
        report(Severity::Error, fd.initializer->span.begin,
               std::string("a freshly-produced owned value cannot be stored "
                           "into the non-`owned` field `") +
                   std::string(fd.name) +
                   "` — the field does not own it, so the value would leak; "
                   "declare the field `owned` (§28.5), or hand it to a method "
                   "that takes it `owned` (§28.6)");
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
                if (h == "Array" || h == "OwningArray" || h == "Dictionary" ||
                    h == "OwningDictionary" || h == "List" || h == "LinkedList" ||
                    h == "OwningLinkedList" || h == "Set" || h == "OwningSet" ||
                    h == "Stack" || h == "OwningStack" || h == "Queue" ||
                    h == "OwningQueue" || h == "OwningBuffer")
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
//   "Disposing an unconsumed `OwningTask<T>`"), and cancellation (§30.8,
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
    // `OwningTask<T>` is a Named TypeRef whose single-part name head is the
    // task class. // per §30.3
    const sema::TypeRef* ot = ctx_.exprType(*await.operand);
    if (!ot || ot->kind != sema::TypeRef::Kind::Named || ot->nullable)
        return AwaitTaskKind::NotTask;
    if (!ot->astNode || !ot->astNode->name ||
        ot->astNode->name->parts.size() != 1)
        return AwaitTaskKind::NotTask;
    std::string_view head = ot->headName();
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
        // (`OwningArray<T>` etc. — the result type of `Task.WhenAllOwned`) is a
        // heap/reference type that carries a disposal obligation, so an
        // `OwningTask<OwningArray<T>>` is a valid Pattern B: its awaited result
        // must be `using var`-bound and the awaited operand consumed. These
        // resolve to a Named TypeRef with a NULL declaration (built-in), which
        // the declaration-based check above misses — so the await was wrongly
        // classified NotTask and the bound-local operand was not discharged.
        if (arg->astNode && arg->astNode->name &&
            arg->astNode->name->parts.size() == 1) {
            std::string_view an = arg->headName();
            return an == "OwningArray" || an == "OwningStack" ||
                   an == "OwningQueue" || an == "OwningLinkedList" ||
                   an == "OwningDictionary" || an == "OwningSet";
        }
        return false;
    };
    if (head == "Task") {
        if (ot->genericArgs.empty()) return AwaitTaskKind::UnitTask;
        return resultIsClass() ? AwaitTaskKind::PatternA
                               : AwaitTaskKind::ValueResult;
    }
    if (head == "OwningTask") {
        // `OwningTask<T>` is a compile error for non-class T (sema §14.4); when
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
        // carries ownership (an `owned T` / `OwningTask<T>` return). The
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
                // §30.5: the awaited `OwningTask<T>` must be in an owned-
                // consuming context, else its transferred result is dropped.
                // // per §30.5
                if (consuming.count(ue) == 0) {
                    report(
                        Severity::Error, ue->span.begin,
                        "awaiting an `OwningTask<T>` (Pattern B) must appear in "
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

// §30.5 / §30.4 (#834): true when `e`'s static type is an `OwningTask<...>`
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
        head = ot->headName();
    else if (!ot->importedName.empty())
        head = ot->importedName;
    return head == "OwningTask";
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
            std::string("`OwningTask<T>` `") + std::string(id->name) +
            "` was already awaited (consumed) — an `OwningTask<T>` is "
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

// ===========================================================================
// §28.14 (v0.175, #808) — scoped callback parameters: storage-bounded capture.
//
// See the block comment in Ownership_impl.hpp. Everything below runs as
// SYNTACTIC pre-passes from run() (before the CFG dataflow): the walk order
// is source order, so declaration indices are assigned before any admission
// check that consults them, and each diagnostic is emitted exactly once.
// The freeze map the admission records is enforced later, inside the
// dataflow's transfer functions (markMovedByName / markDisposedByName /
// handleAssignment), gated on source order so mutation BEFORE the admission
// site is untouched.
// ===========================================================================

namespace {
// Arguments of a call-like node, uniform over the three arg-bearing forms.
const std::vector<ast::CallArgument>* scopedArgsOf(const ast::Expr& e) {
    if (auto* ce = dynamic_cast<const ast::CallExpr*>(&e)) return &ce->args;
    if (auto* nc = dynamic_cast<const ast::NullCondCallExpr*>(&e))
        return &nc->args;
    if (auto* ne = dynamic_cast<const ast::NewExpr*>(&e)) return &ne->args;
    return nullptr;
}
}  // namespace

bool MethodOwnership::typeRefIsTaskScope(const sema::TypeRef* rt) const {
    // stdlib §6.10: TaskScope is a compiler-known class. Sema resolves it
    // either to the synthetic builtin TypeSymbol (declaration->name ==
    // "TaskScope") or to a null-declaration Named ref whose AST name is
    // "TaskScope" (annotation path).
    if (!rt || rt->kind != sema::TypeRef::Kind::Named) return false;
    if (rt->declaration) return rt->declaration->name == "TaskScope";
    if (rt->astNode && rt->astNode->name && !rt->astNode->name->parts.empty())
        return rt->headName() == "TaskScope";
    return rt->importedName == "TaskScope";
}

bool MethodOwnership::exprTypeIsTaskScope(const ast::Expr& e) const {
    if (typeRefIsTaskScope(ctx_.exprType(e))) return true;
    // `new TaskScope()` — before sema typing lands on the NewExpr itself.
    if (auto* ne = dynamic_cast<const ast::NewExpr*>(&e))
        return ne->type && ne->type->name && !ne->type->name->parts.empty() &&
               ne->type->name->parts[0] == "TaskScope";
    return false;
}

namespace {
// The three TypeRef name fallbacks shared by the builtin-type matchers
// (cf. typeRefIsTaskScope): builtin TypeSymbol declaration name, annotation
// AST name, imported name.
std::string_view builtinTypeRefNameHead(const sema::TypeRef* rt) {
    if (!rt || rt->kind != sema::TypeRef::Kind::Named) return {};
    if (rt->declaration) return rt->declaration->name;
    if (rt->astNode && rt->astNode->name && !rt->astNode->name->parts.empty())
        return rt->headName();
    return rt->importedName;
}
}  // namespace

bool MethodOwnership::typeRefIsEventSourceLike(const sema::TypeRef* rt) const {
    // §24.3 / stdlib §6.5: the four faces of an event member (or a value of
    // one of those types). All carry the v0.175 `Subscribe(scoped handler)`
    // signature.
    const std::string_view n = builtinTypeRefNameHead(rt);
    return n == "EventSource" || n == "AsyncEventSource" || n == "IEvent" ||
           n == "IAsyncEvent";
}

bool MethodOwnership::typeRefIsCancellationToken(const sema::TypeRef* rt) const {
    return builtinTypeRefNameHead(rt) == "CancellationToken";
}

bool MethodOwnership::builtinScopedCallbackFormal(const ast::Expr* callee,
                                                  std::size_t argPos) {
    // #1420: builtin subscribe-and-store callees whose handler formal the
    // stdlib declares `scoped` (stdlib §6.5 Subscribe, §6.1 Register; spec
    // §24.3, v0.175). Enforcing here applies the §28.14 admission to their
    // capture-bearing handler arguments. The event `Add`/`Remove` (`+=`/`-=`)
    // producer-face forms are deliberately NOT included: §24.3's normative
    // signatures keep them plain, so they remain stdlib-certified (§28.14
    // v0.177 legacy path) until the spec migrates them.
    if (argPos != 0) return false;
    auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(callee);
    if (!ma || !ma->target) return false;
    if (ma->member == "Subscribe") {
        // `<event>.Subscribe(h)` — the target is the event member itself
        // (bare `Clicked` or qualified `btn.Clicked`, both resolving to the
        // EventSymbol) …
        if (dynamic_cast<sema::EventSymbol*>(ctx_.exprSymbol(*ma->target)))
            return true;
        // … or a VALUE whose static type is one of the event faces
        // (`ev: IEvent<int>` parameter, an EventSource-typed borrow, …).
        return typeRefIsEventSourceLike(ctx_.exprType(*ma->target));
    }
    if (ma->member == "Register")
        return typeRefIsCancellationToken(ctx_.exprType(*ma->target));
    return false;
}

// -- pre-scan: declaration order + using-var initializer anchors + admission

void MethodOwnership::scopedPreScan(
    const std::vector<std::unique_ptr<ast::Stmt>>& body) {
    for (const auto& s : body)
        if (s) scopedPreScanStmt(*s);
    // Run the queued §28.14 admissions AFTER the whole walk so the
    // lambda-literal-local and assigned-symbol maps are complete (the
    // declaration-order comparisons are unaffected — see the queue's
    // declaration comment). // per §28.14
    for (const ScopedAdmission& wi : scopedAdmissionQueue_)
        checkScopedAdmission(*wi.callNode, wi.callee, wi.ne, *wi.arg);
    scopedAdmissionQueue_.clear();
}

void MethodOwnership::scopedPreScanStmt(const ast::Stmt& s) {
    if (auto* ld = dynamic_cast<const ast::LocalDecl*>(&s)) {
        // Assign the declaration index BEFORE descending into the initializer:
        // for bounding forms 1/2 the initializer's captures are checked
        // against THIS local's index. // per §28.14
        scopedDeclOrder_[ld] = scopedDeclCounter_++;
        if (ld->initializer) {
            if (ld->isUsing)
                scopedInitOfUsingLocal_[ld->initializer.get()] = ld;
            // A lambda-literal initializer makes this local a verifiable
            // function value at a scoped call site (unless reassigned —
            // scopedAssignedSyms_). // per §28.14
            if (auto* lam = dynamic_cast<const ast::LambdaExpr*>(
                    ld->initializer.get())) {
                if (sema::LocalSymbol* ls = findLocalSymbol(*ld))
                    scopedLambdaLocalInit_[ls] = lam;
            }
            scopedPreScanExpr(*ld->initializer);
        }
        return;
    }
    if (auto* lc = dynamic_cast<const ast::LocalConstDecl*>(&s)) {
        if (lc->value) scopedPreScanExpr(*lc->value);
        return;
    }
    if (auto* rs = dynamic_cast<const ast::ReturnStmt*>(&s)) {
        if (rs->value) scopedPreScanExpr(*rs->value);
        return;
    }
    if (auto* es = dynamic_cast<const ast::ExpressionStmt*>(&s)) {
        if (es->expr) scopedPreScanExpr(*es->expr);
        return;
    }
    if (auto* as = dynamic_cast<const ast::AssignmentStmt*>(&s)) {
        // Record a reassigned local: a lambda-literal local that is ever an
        // assignment target loses its verifiable-function-value status.
        // // per §28.14
        if (auto* tid =
                dynamic_cast<const ast::IdentifierExpr*>(as->target.get())) {
            if (sema::Symbol* tsym = ctx_.exprSymbol(*tid))
                scopedAssignedSyms_.insert(tsym);
        }
        if (as->target) scopedPreScanExpr(*as->target);
        if (as->value) scopedPreScanExpr(*as->value);
        return;
    }
    if (auto* ts = dynamic_cast<const ast::ThrowStmt*>(&s)) {
        if (ts->value) scopedPreScanExpr(*ts->value);
        return;
    }
    if (auto* ifs = dynamic_cast<const ast::IfStmt*>(&s)) {
        if (ifs->ifBranch.condition) scopedPreScanExpr(*ifs->ifBranch.condition);
        for (const auto& bs : ifs->ifBranch.body)
            if (bs) scopedPreScanStmt(*bs);
        for (const auto& el : ifs->elifBranches) {
            if (el.condition) scopedPreScanExpr(*el.condition);
            for (const auto& bs : el.body)
                if (bs) scopedPreScanStmt(*bs);
        }
        for (const auto& bs : ifs->elseBody)
            if (bs) scopedPreScanStmt(*bs);
        return;
    }
    if (auto* ws = dynamic_cast<const ast::WhileStmt*>(&s)) {
        if (ws->condition) scopedPreScanExpr(*ws->condition);
        for (const auto& bs : ws->body)
            if (bs) scopedPreScanStmt(*bs);
        return;
    }
    if (auto* fs = dynamic_cast<const ast::ForStmt*>(&s)) {
        // The iteration variable's LocalSymbol anchors on the ForStmt node.
        scopedDeclOrder_[fs] = scopedDeclCounter_++;
        if (fs->iterable) scopedPreScanExpr(*fs->iterable);
        if (fs->withExpr) scopedPreScanExpr(*fs->withExpr);
        for (const auto& bs : fs->body)
            if (bs) scopedPreScanStmt(*bs);
        return;
    }
    if (auto* db = dynamic_cast<const ast::DoEndBlock*>(&s)) {
        for (const auto& bs : db->body)
            if (bs) scopedPreScanStmt(*bs);
        return;
    }
    if (auto* is = dynamic_cast<const ast::IncrementStmt*>(&s)) {
        if (is->target) scopedPreScanExpr(*is->target);
        return;
    }
    if (auto* ds = dynamic_cast<const ast::DiscardStmt*>(&s)) {
        if (ds->value) scopedPreScanExpr(*ds->value);
        return;
    }
    if (auto* ys = dynamic_cast<const ast::YieldStmt*>(&s)) {
        if (ys->value) scopedPreScanExpr(*ys->value);
        return;
    }
    if (auto* trys = dynamic_cast<const ast::TryStmt*>(&s)) {
        for (const auto& bs : trys->body)
            if (bs) scopedPreScanStmt(*bs);
        for (const auto& arm : trys->catches) {
            if (arm.guard) scopedPreScanExpr(*arm.guard);
            if (arm.body) scopedPreScanStmt(*arm.body);
        }
        for (const auto& bs : trys->finallyBody)
            if (bs) scopedPreScanStmt(*bs);
        return;
    }
    if (auto* cs = dynamic_cast<const ast::CaseStmt*>(&s)) {
        if (cs->scrutinee) scopedPreScanExpr(*cs->scrutinee);
        for (const auto& arm : cs->arms) {
            if (arm.guard) scopedPreScanExpr(*arm.guard);
            if (arm.exprBody) scopedPreScanExpr(*arm.exprBody);
            for (const auto& bs : arm.stmts)
                if (bs) scopedPreScanStmt(*bs);
        }
        return;
    }
    // Break/Continue: nothing to scan.
}

void MethodOwnership::scopedPreScanExpr(const ast::Expr& e) {
    // Call-like nodes: run the §28.14 admission for each arg whose formal is
    // `scoped`, then descend.
    const auto* ne = dynamic_cast<const ast::NewExpr*>(&e);
    const ast::Expr* callee = nullptr;
    if (auto* ce = dynamic_cast<const ast::CallExpr*>(&e))
        callee = ce->callee.get();
    else if (auto* nc = dynamic_cast<const ast::NullCondCallExpr*>(&e))
        callee = nc->callee.get();
    if (const auto* args = scopedArgsOf(e)) {
        for (std::size_t i = 0; i < args->size(); ++i) {
            const auto& a = (*args)[i];
            if (!a.value) continue;
            // An `out`/`ref` argument overwrites its identifier — a lambda-
            // literal local passed that way is no longer verifiable.
            if (!a.modifier.empty() &&
                a.modifier.find("out") != std::string_view::npos) {
                if (auto* oid = dynamic_cast<const ast::IdentifierExpr*>(
                        a.value.get()))
                    if (sema::Symbol* osym = ctx_.exprSymbol(*oid))
                        scopedAssignedSyms_.insert(osym);
            } else if (a.modifier == "ref") {
                if (auto* oid = dynamic_cast<const ast::IdentifierExpr*>(
                        a.value.get()))
                    if (sema::Symbol* osym = ctx_.exprSymbol(*oid))
                        scopedAssignedSyms_.insert(osym);
            }
            if (scopedFormalAt(callee, ne, i))
                scopedAdmissionQueue_.push_back(
                    {&e, callee, ne, a.value.get()});
            scopedPreScanExpr(*a.value);
        }
        if (callee) scopedPreScanExpr(*callee);
        return;
    }
    // Structural recursion over the remaining expression forms.
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
        if (ma->target) scopedPreScanExpr(*ma->target);
        return;
    }
    if (auto* nm = dynamic_cast<const ast::NullCondMemberExpr*>(&e)) {
        if (nm->target) scopedPreScanExpr(*nm->target);
        return;
    }
    if (auto* ix = dynamic_cast<const ast::IndexExpr*>(&e)) {
        if (ix->target) scopedPreScanExpr(*ix->target);
        for (const auto& a : ix->indices)
            if (a.value) scopedPreScanExpr(*a.value);
        return;
    }
    if (auto* be = dynamic_cast<const ast::BinaryExpr*>(&e)) {
        if (be->left) scopedPreScanExpr(*be->left);
        if (be->right) scopedPreScanExpr(*be->right);
        return;
    }
    if (auto* un = dynamic_cast<const ast::UnaryExpr*>(&e)) {
        if (un->operand) scopedPreScanExpr(*un->operand);
        return;
    }
    if (auto* pb = dynamic_cast<const ast::PostfixBangExpr*>(&e)) {
        if (pb->operand) scopedPreScanExpr(*pb->operand);
        return;
    }
    if (auto* tt = dynamic_cast<const ast::TypeTestExpr*>(&e)) {
        if (tt->operand) scopedPreScanExpr(*tt->operand);
        return;
    }
    if (auto* cd = dynamic_cast<const ast::ConditionalExpr*>(&e)) {
        if (cd->condition) scopedPreScanExpr(*cd->condition);
        if (cd->thenValue) scopedPreScanExpr(*cd->thenValue);
        if (cd->elseValue) scopedPreScanExpr(*cd->elseValue);
        return;
    }
    if (auto* th = dynamic_cast<const ast::ThrowExpr*>(&e)) {
        if (th->operand) scopedPreScanExpr(*th->operand);
        return;
    }
    if (auto* rg = dynamic_cast<const ast::RangeExpr*>(&e)) {
        if (rg->lo) scopedPreScanExpr(*rg->lo);
        if (rg->hi) scopedPreScanExpr(*rg->hi);
        if (rg->step) scopedPreScanExpr(*rg->step);
        return;
    }
    if (auto* we = dynamic_cast<const ast::WithExpr*>(&e)) {
        if (we->source) scopedPreScanExpr(*we->source);
        for (const auto& ia : we->assigns)
            if (ia.value) scopedPreScanExpr(*ia.value);
        return;
    }
    if (auto* cl = dynamic_cast<const ast::CollectionLiteralExpr*>(&e)) {
        for (const auto& k : cl->keys)
            if (k) scopedPreScanExpr(*k);
        for (const auto& el : cl->elements)
            if (el) scopedPreScanExpr(*el);
        return;
    }
    if (auto* isx = dynamic_cast<const ast::InterpolatedStringExpr*>(&e)) {
        for (const auto& part : isx->parts)
            if (part.expr) scopedPreScanExpr(*part.expr);
        return;
    }
    if (auto* lam = dynamic_cast<const ast::LambdaExpr*>(&e)) {
        // Walk lambda bodies too: a Spawn call inside a lambda still needs
        // admission, and lambda-internal locals need declaration indices so
        // an inner `using var` scope can be a bounding object.
        if (lam->exprBody) scopedPreScanExpr(*lam->exprBody);
        for (const auto& bs : lam->stmts)
            if (bs) scopedPreScanStmt(*bs);
        return;
    }
    if (auto* cx = dynamic_cast<const ast::CaseExpr*>(&e)) {
        if (cx->scrutinee) scopedPreScanExpr(*cx->scrutinee);
        for (const auto& arm : cx->arms) {
            if (arm.guard) scopedPreScanExpr(*arm.guard);
            if (arm.exprBody) scopedPreScanExpr(*arm.exprBody);
            for (const auto& bs : arm.stmts)
                if (bs) scopedPreScanStmt(*bs);
        }
        return;
    }
    // Literals / identifiers / typeof / nameof / default / unit: no children
    // with calls.
}

bool MethodOwnership::scopedFormalAt(const ast::Expr* callee,
                                     const ast::NewExpr* ne,
                                     std::size_t argPos) {
    // Constructor form: match the ctor by arg count (as recordMovesIn does).
    if (ne) {
        if (!ne->type) return false;
        const sema::TypeRef* rt = ctx_.resolvedType(*ne->type);
        if (!rt || !rt->declaration) return false;
        for (sema::Symbol* m : rt->declaration->members) {
            if (auto* cs = dynamic_cast<sema::ConstructorSymbol*>(m)) {
                auto* d = dynamic_cast<const ast::ConstructorDecl*>(cs->decl);
                if (d && d->params.size() == ne->args.size()) {
                    return argPos < d->params.size() &&
                           d->params[argPos].modifier == "scoped";
                }
            }
        }
        return false;
    }
    if (!callee) return false;
    // User method: the resolved MethodSymbol's AST params carry the modifier.
    if (auto* ms = dynamic_cast<sema::MethodSymbol*>(ctx_.exprSymbol(*callee))) {
        if (auto* md = dynamic_cast<const ast::MethodDecl*>(ms->decl)) {
            return argPos < md->params.size() &&
                   md->params[argPos].modifier == "scoped";
        }
    }
    // Builtin: TaskScope.Spawn(scoped body: () -> Task) — stdlib §6.10.
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(callee)) {
        if (ma->member == "Spawn" && argPos == 0 && ma->target &&
            typeRefIsTaskScope(ctx_.exprType(*ma->target)))
            return true;
    }
    // Builtin: event Subscribe / CancellationToken.Register — the stdlib
    // §6.5 / §6.1 `scoped` handler formals, enforced per #1420.
    if (builtinScopedCallbackFormal(callee, argPos)) return true;
    // Function VALUE callee whose Function type carries a scoped position
    // (`var g: (scoped () -> Task) -> ()` … `g(cb)`).
    if (const sema::TypeRef* ft = ctx_.exprType(*callee)) {
        if (ft->kind == sema::TypeRef::Kind::Function &&
            argPos < ft->funcParams.size())
            return ft->funcParams[argPos].modifier == "scoped";
    }
    return false;
}

const ast::LocalDecl* MethodOwnership::scopedBoundingLocal(
    const ast::Expr& callNode, const ast::Expr* callee,
    const ast::NewExpr* ne) {
    // Form 1 — constructor call: the constructed object. Usable as a bound
    // only when the NewExpr IS the initializer of a using/await-using local.
    if (ne) {
        auto it = scopedInitOfUsingLocal_.find(&callNode);
        return it != scopedInitOfUsingLocal_.end() ? it->second : nullptr;
    }
    if (!callee) return nullptr;
    // Form 2 — a call whose return is `owned` AND is the initializer of a
    // using/await-using local: the returned object.
    if (auto* ms = dynamic_cast<sema::MethodSymbol*>(ctx_.exprSymbol(*callee))) {
        if (auto* md = dynamic_cast<const ast::MethodDecl*>(ms->decl)) {
            if (md->returnOwned) {
                auto it = scopedInitOfUsingLocal_.find(&callNode);
                if (it != scopedInitOfUsingLocal_.end()) return it->second;
            }
        }
    }
    // Form 2 (builtin, #1420): event `Subscribe` / CancellationToken
    // `Register` return `owned Subscription` (stdlib §2.11 / §6.5 / §6.1) —
    // when the call initializes a `using var`/`await using var` local, the
    // returned Subscription is the bounding object; its disposal severs the
    // registration (quiescence by removal, §28.14 / §24.6). // per §28.14
    if (builtinScopedCallbackFormal(callee, 0)) {
        auto it = scopedInitOfUsingLocal_.find(&callNode);
        if (it != scopedInitOfUsingLocal_.end()) return it->second;
    }
    // Form 3 — otherwise the receiver: admissible only when the receiver is a
    // bare identifier naming a using/await-using local.
    const ast::Expr* recv = nullptr;
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(callee))
        recv = ma->target.get();
    else if (auto* nm = dynamic_cast<const ast::NullCondMemberExpr*>(callee))
        recv = nm->target.get();
    if (!recv) return nullptr;
    auto* id = dynamic_cast<const ast::IdentifierExpr*>(recv);
    if (!id || id->name == "this" || id->name == "base") return nullptr;
    auto* ls = dynamic_cast<sema::LocalSymbol*>(ctx_.exprSymbol(*id));
    if (!ls || !ls->isUsing) return nullptr;
    return dynamic_cast<const ast::LocalDecl*>(ls->decl);
}

void MethodOwnership::scopedCollectCaptures(
    const ast::LambdaExpr& lam,
    std::vector<const ast::IdentifierExpr*>& out) {
    if (lam.exprBody) scopedCaptureWalkExpr(*lam.exprBody, out);
    for (const auto& bs : lam.stmts)
        if (bs) scopedCaptureWalkStmt(*bs, out);
}

void MethodOwnership::scopedCaptureWalkStmt(
    const ast::Stmt& s, std::vector<const ast::IdentifierExpr*>& out) {
    if (auto* ld = dynamic_cast<const ast::LocalDecl*>(&s)) {
        if (ld->initializer) scopedCaptureWalkExpr(*ld->initializer, out);
        return;
    }
    if (auto* lc = dynamic_cast<const ast::LocalConstDecl*>(&s)) {
        if (lc->value) scopedCaptureWalkExpr(*lc->value, out);
        return;
    }
    if (auto* rs = dynamic_cast<const ast::ReturnStmt*>(&s)) {
        if (rs->value) scopedCaptureWalkExpr(*rs->value, out);
        return;
    }
    if (auto* es = dynamic_cast<const ast::ExpressionStmt*>(&s)) {
        if (es->expr) scopedCaptureWalkExpr(*es->expr, out);
        return;
    }
    if (auto* as = dynamic_cast<const ast::AssignmentStmt*>(&s)) {
        if (as->target) scopedCaptureWalkExpr(*as->target, out);
        if (as->value) scopedCaptureWalkExpr(*as->value, out);
        return;
    }
    if (auto* ts = dynamic_cast<const ast::ThrowStmt*>(&s)) {
        if (ts->value) scopedCaptureWalkExpr(*ts->value, out);
        return;
    }
    if (auto* ifs = dynamic_cast<const ast::IfStmt*>(&s)) {
        if (ifs->ifBranch.condition)
            scopedCaptureWalkExpr(*ifs->ifBranch.condition, out);
        for (const auto& bs : ifs->ifBranch.body)
            if (bs) scopedCaptureWalkStmt(*bs, out);
        for (const auto& el : ifs->elifBranches) {
            if (el.condition) scopedCaptureWalkExpr(*el.condition, out);
            for (const auto& bs : el.body)
                if (bs) scopedCaptureWalkStmt(*bs, out);
        }
        for (const auto& bs : ifs->elseBody)
            if (bs) scopedCaptureWalkStmt(*bs, out);
        return;
    }
    if (auto* ws = dynamic_cast<const ast::WhileStmt*>(&s)) {
        if (ws->condition) scopedCaptureWalkExpr(*ws->condition, out);
        for (const auto& bs : ws->body)
            if (bs) scopedCaptureWalkStmt(*bs, out);
        return;
    }
    if (auto* fs = dynamic_cast<const ast::ForStmt*>(&s)) {
        if (fs->iterable) scopedCaptureWalkExpr(*fs->iterable, out);
        if (fs->withExpr) scopedCaptureWalkExpr(*fs->withExpr, out);
        for (const auto& bs : fs->body)
            if (bs) scopedCaptureWalkStmt(*bs, out);
        return;
    }
    if (auto* db = dynamic_cast<const ast::DoEndBlock*>(&s)) {
        for (const auto& bs : db->body)
            if (bs) scopedCaptureWalkStmt(*bs, out);
        return;
    }
    if (auto* is = dynamic_cast<const ast::IncrementStmt*>(&s)) {
        if (is->target) scopedCaptureWalkExpr(*is->target, out);
        return;
    }
    if (auto* ds = dynamic_cast<const ast::DiscardStmt*>(&s)) {
        if (ds->value) scopedCaptureWalkExpr(*ds->value, out);
        return;
    }
    if (auto* ys = dynamic_cast<const ast::YieldStmt*>(&s)) {
        if (ys->value) scopedCaptureWalkExpr(*ys->value, out);
        return;
    }
    if (auto* trys = dynamic_cast<const ast::TryStmt*>(&s)) {
        for (const auto& bs : trys->body)
            if (bs) scopedCaptureWalkStmt(*bs, out);
        for (const auto& arm : trys->catches) {
            if (arm.guard) scopedCaptureWalkExpr(*arm.guard, out);
            if (arm.body) scopedCaptureWalkStmt(*arm.body, out);
        }
        for (const auto& bs : trys->finallyBody)
            if (bs) scopedCaptureWalkStmt(*bs, out);
        return;
    }
    if (auto* cs = dynamic_cast<const ast::CaseStmt*>(&s)) {
        if (cs->scrutinee) scopedCaptureWalkExpr(*cs->scrutinee, out);
        for (const auto& arm : cs->arms) {
            if (arm.guard) scopedCaptureWalkExpr(*arm.guard, out);
            if (arm.exprBody) scopedCaptureWalkExpr(*arm.exprBody, out);
            for (const auto& bs : arm.stmts)
                if (bs) scopedCaptureWalkStmt(*bs, out);
        }
        return;
    }
    // Break/Continue: nothing.
}

void MethodOwnership::scopedCaptureWalkExpr(
    const ast::Expr& e, std::vector<const ast::IdentifierExpr*>& out) {
    if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(&e)) {
        out.push_back(id);
        return;
    }
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
        if (ma->target) scopedCaptureWalkExpr(*ma->target, out);
        return;
    }
    if (auto* nm = dynamic_cast<const ast::NullCondMemberExpr*>(&e)) {
        if (nm->target) scopedCaptureWalkExpr(*nm->target, out);
        return;
    }
    if (auto* ce = dynamic_cast<const ast::CallExpr*>(&e)) {
        if (ce->callee) scopedCaptureWalkExpr(*ce->callee, out);
        for (const auto& a : ce->args)
            if (a.value) scopedCaptureWalkExpr(*a.value, out);
        return;
    }
    if (auto* nc = dynamic_cast<const ast::NullCondCallExpr*>(&e)) {
        if (nc->callee) scopedCaptureWalkExpr(*nc->callee, out);
        for (const auto& a : nc->args)
            if (a.value) scopedCaptureWalkExpr(*a.value, out);
        return;
    }
    if (auto* ne = dynamic_cast<const ast::NewExpr*>(&e)) {
        for (const auto& a : ne->args)
            if (a.value) scopedCaptureWalkExpr(*a.value, out);
        return;
    }
    if (auto* ix = dynamic_cast<const ast::IndexExpr*>(&e)) {
        if (ix->target) scopedCaptureWalkExpr(*ix->target, out);
        for (const auto& a : ix->indices)
            if (a.value) scopedCaptureWalkExpr(*a.value, out);
        return;
    }
    if (auto* be = dynamic_cast<const ast::BinaryExpr*>(&e)) {
        if (be->left) scopedCaptureWalkExpr(*be->left, out);
        if (be->right) scopedCaptureWalkExpr(*be->right, out);
        return;
    }
    if (auto* un = dynamic_cast<const ast::UnaryExpr*>(&e)) {
        if (un->operand) scopedCaptureWalkExpr(*un->operand, out);
        return;
    }
    if (auto* pb = dynamic_cast<const ast::PostfixBangExpr*>(&e)) {
        if (pb->operand) scopedCaptureWalkExpr(*pb->operand, out);
        return;
    }
    if (auto* tt = dynamic_cast<const ast::TypeTestExpr*>(&e)) {
        if (tt->operand) scopedCaptureWalkExpr(*tt->operand, out);
        return;
    }
    if (auto* cd = dynamic_cast<const ast::ConditionalExpr*>(&e)) {
        if (cd->condition) scopedCaptureWalkExpr(*cd->condition, out);
        if (cd->thenValue) scopedCaptureWalkExpr(*cd->thenValue, out);
        if (cd->elseValue) scopedCaptureWalkExpr(*cd->elseValue, out);
        return;
    }
    if (auto* th = dynamic_cast<const ast::ThrowExpr*>(&e)) {
        if (th->operand) scopedCaptureWalkExpr(*th->operand, out);
        return;
    }
    if (auto* rg = dynamic_cast<const ast::RangeExpr*>(&e)) {
        if (rg->lo) scopedCaptureWalkExpr(*rg->lo, out);
        if (rg->hi) scopedCaptureWalkExpr(*rg->hi, out);
        if (rg->step) scopedCaptureWalkExpr(*rg->step, out);
        return;
    }
    if (auto* we = dynamic_cast<const ast::WithExpr*>(&e)) {
        if (we->source) scopedCaptureWalkExpr(*we->source, out);
        for (const auto& ia : we->assigns)
            if (ia.value) scopedCaptureWalkExpr(*ia.value, out);
        return;
    }
    if (auto* cl = dynamic_cast<const ast::CollectionLiteralExpr*>(&e)) {
        for (const auto& k : cl->keys)
            if (k) scopedCaptureWalkExpr(*k, out);
        for (const auto& el : cl->elements)
            if (el) scopedCaptureWalkExpr(*el, out);
        return;
    }
    if (auto* isx = dynamic_cast<const ast::InterpolatedStringExpr*>(&e)) {
        for (const auto& part : isx->parts)
            if (part.expr) scopedCaptureWalkExpr(*part.expr, out);
        return;
    }
    if (auto* lam = dynamic_cast<const ast::LambdaExpr*>(&e)) {
        // A nested lambda's free variables are captures of the outer one.
        scopedCollectCaptures(*lam, out);
        return;
    }
    if (auto* cx = dynamic_cast<const ast::CaseExpr*>(&e)) {
        if (cx->scrutinee) scopedCaptureWalkExpr(*cx->scrutinee, out);
        for (const auto& arm : cx->arms) {
            if (arm.guard) scopedCaptureWalkExpr(*arm.guard, out);
            if (arm.exprBody) scopedCaptureWalkExpr(*arm.exprBody, out);
            for (const auto& bs : arm.stmts)
                if (bs) scopedCaptureWalkStmt(*bs, out);
        }
        return;
    }
    // Literals / typeof / nameof / default / unit: no identifiers.
}

void MethodOwnership::checkScopedAdmission(const ast::Expr& callNode,
                                           const ast::Expr* callee,
                                           const ast::NewExpr* ne,
                                           const ast::Expr& arg) {
    // Classify the argument form and enumerate its captures.
    std::vector<const ast::IdentifierExpr*> rawIds;
    if (auto* lam = dynamic_cast<const ast::LambdaExpr*>(&arg)) {
        scopedCollectCaptures(*lam, rawIds);
    } else if (dynamic_cast<const ast::NullLiteralExpr*>(&arg)) {
        return;  // clearing a nullable scoped position — nothing captured
    } else if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(&arg)) {
        sema::Symbol* sym = ctx_.exprSymbol(*id);
        if (auto* ps = dynamic_cast<sema::ParameterSymbol*>(sym)) {
            if (ps->modifier == "scoped" && methodParamSet_.count(sym))
                return;  // forwarding a scoped param — callee-side rule checks it
        }
        if (auto* ms = dynamic_cast<sema::MethodSymbol*>(sym)) {
            // Bare method reference. A static method captures nothing; an
            // instance method captures `this` (which outlives every local).
            (void)ms;
            return;
        }
        // §28.14 v0.177 (#1419): a CALL-BOUNDED parameter (or a local
        // carrying one) may not be forwarded into a `scoped` position — the
        // callee cannot vouch for captures it cannot see. The diagnostic
        // names the spec-stated fix.
        {
            sema::ParameterSymbol* cb = nullptr;
            if (sym && callBoundedParams_.count(sym))
                cb = dynamic_cast<sema::ParameterSymbol*>(sym);
            if (!cb && sym) {
                auto ait = callBoundedAlias_.find(sym);
                if (ait != callBoundedAlias_.end()) cb = ait->second;
            }
            if (cb) {
                report(Severity::Error, arg.span.begin,
                       std::string("parameter `") + std::string(cb->name) +
                           "` is call-bounded and may not be forwarded into "
                           "a `scoped` parameter position — the callee "
                           "cannot vouch for captures it cannot see; declare "
                           "`" + std::string(cb->name) +
                           "` itself `scoped` to store it (§28.14, v0.177)");
                return;
            }
        }
        // Verifiable-subset widening (#1420 triage): a local bound to a
        // LAMBDA LITERAL and never reassigned is verifiable — its captures
        // are the literal's captures, enumerated exactly as if the literal
        // were passed directly. // per §28.14
        auto lit = scopedLambdaLocalInit_.find(sym);
        if (lit != scopedLambdaLocalInit_.end() &&
            scopedAssignedSyms_.count(sym) == 0) {
            scopedCollectCaptures(*lit->second, rawIds);
        } else {
            report(Severity::Error, arg.span.begin,
                   std::string("the captures of the function value `") +
                       std::string(id->name) +
                       "` cannot be verified at this `scoped` call site — "
                       "pass the lambda literal directly, bind it to a local "
                       "that is never reassigned, or pass a capture-free "
                       "function (§28.14, v0.175)");
            return;
        }
    } else if (auto* mref = dynamic_cast<const ast::MemberAccessExpr*>(&arg)) {
        // Method reference `obj.M`: its captured environment is the receiver
        // (§28.3 / §27.6) — enumerate the receiver's identifier roots.
        if (dynamic_cast<sema::MethodSymbol*>(ctx_.exprSymbol(arg)) &&
            mref->target) {
            scopedCaptureWalkExpr(*mref->target, rawIds);
        } else {
            report(Severity::Error, arg.span.begin,
                   "the captures of this function-typed expression cannot be "
                   "verified at this `scoped` call site — pass a lambda "
                   "literal, a method reference, or a capture-free function "
                   "(§28.14, v0.175)");
            return;
        }
    } else {
        report(Severity::Error, arg.span.begin,
               "the captures of this function-typed expression cannot be "
               "verified at this `scoped` call site — pass a lambda literal, "
               "a method reference, or a capture-free function (§28.14, "
               "v0.175)");
        return;
    }

    // Reduce raw identifiers to CAPTURED SOURCES: enclosing-method locals and
    // parameters whose referent's lifetime is scope-bound. Lambda-internal
    // names, statics, value-typed locals (their captured box outlives every
    // scope), and `this`-mediated members (fields/methods — `this` is a
    // caller-frame borrow that outlives every local of this frame) are not
    // order-constrained sources. // per §28.14
    struct Source {
        const ast::IdentifierExpr* id;
        sema::Symbol* sym;          // null for `this`
        const ast::Node* declNode;  // null for params / this
    };
    std::vector<Source> sources;
    bool anyCapture = false;
    std::unordered_set<sema::Symbol*> seen;
    for (const ast::IdentifierExpr* id : rawIds) {
        if (id->name == "this" || id->name == "base") {
            anyCapture = true;  // a borrow of the receiver — needs a bound,
                                // but always precedes every local
            continue;
        }
        sema::Symbol* sym = ctx_.exprSymbol(*id);
        if (!sym || !seen.insert(sym).second) continue;
        if (isCatchBorrow(sym)) {
            report(Severity::Error, id->span.begin,
                   std::string("closure bound to a `scoped` parameter "
                               "captures the caught exception `") +
                       std::string(sym->name) +
                       "` — a caught exception is a borrow whose lifetime is "
                       "the catch arm body; a stored callback outlives it "
                       "(§28.14, §31.1)");
            return;
        }
        if (auto* ps = dynamic_cast<sema::ParameterSymbol*>(sym)) {
            if (!methodParamSet_.count(sym)) continue;  // lambda-internal param
            // §28.14 v0.177 (#1419): a closure CAPTURING a call-bounded
            // function-typed parameter cannot enter storage-bounded
            // admission — the stored closure would extend the callback past
            // its call (the transitive escape judgment).
            if (callBoundedParams_.count(sym)) {
                report(Severity::Error, id->span.begin,
                       std::string("closure bound to a `scoped` parameter "
                                   "captures `") +
                           std::string(sym->name) +
                           "`, a call-bounded function-typed parameter — "
                           "storing the closure would extend the callback "
                           "past its call; declare `" +
                           std::string(sym->name) +
                           "` itself `scoped` to store it (§28.14, v0.177)");
                return;
            }
            anyCapture = true;
            // Parameters precede every local — declaration order holds. An
            // `owned` parameter is still a freezable source (it can be moved).
            if (ps->modifier == "owned")
                sources.push_back({id, sym, nullptr});
            continue;
        }
        if (auto* ls = dynamic_cast<sema::LocalSymbol*>(sym)) {
            // §28.14 v0.177 (#1419): a local CARRYING a call-bounded
            // parameter (bound from it, or from a closure capturing it) is
            // the callback under another name — same rejection as the direct
            // capture above.
            auto ait = callBoundedAlias_.find(sym);
            if (ait != callBoundedAlias_.end()) {
                report(Severity::Error, id->span.begin,
                       std::string("closure bound to a `scoped` parameter "
                                   "captures `") +
                           std::string(sym->name) +
                           "`, which carries the call-bounded parameter `" +
                           std::string(ait->second->name) +
                           "` — storing the closure would extend the "
                           "callback past its call; declare `" +
                           std::string(ait->second->name) +
                           "` itself `scoped` to store it (§28.14, v0.177)");
                return;
            }
            // Value-typed captures live in their (heap) capture box, which the
            // closure keeps alive — no lifetime constraint. Only a capture of
            // a class/interface reference can dangle. Null declType stays
            // conservative (treated as a reference).
            if (ls->declType && !isBorrowableType(ls->declType)) continue;
            anyCapture = true;
            sources.push_back({id, sym, ls->decl});
            continue;
        }
        if (dynamic_cast<sema::FieldSymbol*>(sym) ||
            dynamic_cast<sema::MethodSymbol*>(sym) ||
            dynamic_cast<sema::PropertySymbol*>(sym) ||
            dynamic_cast<sema::EventSymbol*>(sym)) {
            // Implicit `this` capture: needs a bound, precedes every local.
            // (A static member would be admissible without a bound, but the
            // conservative treatment only requires the bound — sound.)
            anyCapture = true;
            continue;
        }
        // TypeSymbol / const / enum constant / module: not a capture.
    }

    if (!anyCapture) return;  // capture-free callbacks are always admitted

    const ast::LocalDecl* bounding = scopedBoundingLocal(callNode, callee, ne);
    if (!bounding) {
        report(Severity::Error, arg.span.begin,
               "a capture-bearing closure is not admitted here: the call's "
               "bounding object does not resolve to a `using var`/`await "
               "using var` local (§28.14 — a plain expression, field, or "
               "non-initializer receiver admits only capture-free "
               "callbacks); bind the receiving object in a `using var` and "
               "call through it");
        return;
    }
    auto bIt = scopedDeclOrder_.find(bounding);
    const int boundIdx =
        bIt != scopedDeclOrder_.end() ? bIt->second : -1;
    for (const Source& src : sources) {
        // SELF-CAPTURE: the callback captures the bounding local ITSELF (the
        // §28.14 re-registration idiom — `scope.Spawn(() => … scope.Spawn(…))`,
        // considered in the #808 memo's adversarial analysis). Sound: the
        // callback is stored IN the bounding object and every invocation
        // happens before its disposal completes; during the drain the object
        // is logically disposed but physically alive (§28.12), so the capture
        // never dangles. No freeze needed — a using var is already unmovable
        // and unreassignable. (The spec's "declared before L" sentence does
        // not name this corner; recorded as an interpretation to ratify.)
        if (src.declNode == static_cast<const ast::Node*>(bounding)) continue;
        if (!src.declNode) {
            // Enclosing-method parameter: precedes every local. Freeze below.
        } else {
            auto sIt = scopedDeclOrder_.find(src.declNode);
            const bool ok = sIt != scopedDeclOrder_.end() && boundIdx >= 0 &&
                            sIt->second < boundIdx;
            if (!ok) {
                report(
                    Severity::Error, src.id->span.begin,
                    std::string("captured source `") +
                        std::string(src.id->name) +
                        "` is not declared before the bounding `using var` `" +
                        std::string(bounding->name) +
                        "` — every captured source must be declared before "
                        "the bounding object so it provably outlives every "
                        "invocation of the stored callback (§28.14, v0.175; "
                        "declare `" +
                        std::string(src.id->name) + "` before `" +
                        std::string(bounding->name) + "`)");
                return;
            }
        }
        // §28.14 freeze: no move / dispose / reassign of the source until the
        // bounding local's scope exits. Keep the FIRST admission site.
        scopedFrozen_.emplace(src.sym,
                              ScopedFreeze{bounding->name,
                                           callNode.span.begin});
    }
}

void MethodOwnership::checkScopedFreeze(std::string_view name,
                                        SourceLocation loc,
                                        std::string_view action) {
    for (const auto& [sym, fr] : scopedFrozen_) {
        if (!sym || sym->name != name) continue;
        // Only mutation AT-OR-AFTER the admission site violates the freeze
        // (source order; a move before the callback was ever admitted is the
        // ordinary §28.3 regime). Loop-carried mutation that lexically
        // precedes the admission is not caught here — recorded residual.
        if (loc.offset < fr.admissionLoc.offset) continue;
        report(Severity::Error, loc,
               std::string("cannot ") + std::string(action) + " `" +
                   std::string(name) +
                   "`: it is captured by a callback stored under a `scoped` "
                   "parameter bounded by `" +
                   std::string(fr.boundingName) +
                   "`, and captured sources are frozen until that scope "
                   "exits (§28.14, v0.175)");
        return;
    }
}

// -- §28.14 callee verification ---------------------------------------------

int MethodOwnership::scopedUseKind(const ast::Expr& e,
                                   std::string_view* nameOut) {
    if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(&e)) {
        sema::Symbol* sym = ctx_.exprSymbol(*id);
        if (auto* ps = dynamic_cast<sema::ParameterSymbol*>(sym)) {
            if (ps->modifier == "scoped" && methodParamSet_.count(sym)) {
                if (nameOut) *nameOut = id->name;
                return 1;
            }
        }
        if (auto* fs = dynamic_cast<sema::FieldSymbol*>(sym)) {
            for (std::string_view mod : fs->modifiers) {
                if (mod == "scoped") {
                    if (nameOut) *nameOut = id->name;
                    return 2;
                }
            }
        }
        return 0;
    }
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
        // `this._f` — a this-qualified scoped-field use.
        auto* tid = dynamic_cast<const ast::IdentifierExpr*>(ma->target.get());
        if (!tid || tid->name != "this") return 0;
        if (auto* fs = dynamic_cast<sema::FieldSymbol*>(ctx_.exprSymbol(e))) {
            for (std::string_view mod : fs->modifiers) {
                if (mod == "scoped") {
                    if (nameOut) *nameOut = ma->member;
                    return 2;
                }
            }
        }
        return 0;
    }
    return 0;
}

bool MethodOwnership::scopedRestrictedUse(const ast::Expr& e,
                                          std::string_view context) {
    std::string_view name;
    int kind = scopedUseKind(e, &name);
    if (kind == 0) return false;
    if (kind == 1) {
        report(Severity::Error, e.span.begin,
               std::string("`") + std::string(name) +
                   "` is a `scoped` parameter — it may only be invoked, "
                   "stored into a `scoped` field of `this`, or forwarded to "
                   "a `scoped` parameter of a call on `this`; it may not " +
                   std::string(context) + " (§28.14, v0.175)");
    } else {
        report(Severity::Error, e.span.begin,
               std::string("`") + std::string(name) +
                   "` is a `scoped` field — it may be invoked, overwritten "
                   "from a `scoped` parameter, or cleared to null, but never "
                   "read out as an escaping value; it may not " +
                   std::string(context) + " (§28.14, v0.175)");
    }
    return true;
}

void MethodOwnership::scopedCalleeScan(
    const std::vector<std::unique_ptr<ast::Stmt>>& body) {
    for (const auto& s : body)
        if (s) scopedCalleeWalkStmt(*s);
}

void MethodOwnership::scopedCalleeWalkStmt(const ast::Stmt& s) {
    if (auto* as = dynamic_cast<const ast::AssignmentStmt*>(&s)) {
        // Store INTO a scoped field of `this`: RHS must be a scoped parameter
        // of this method, or the null literal (clear). // per §28.14
        if (as->target && scopedUseKind(*as->target) == 2) {
            if (as->op != ":=") {
                report(Severity::Error, as->target->span.begin,
                       "a `scoped` field admits only plain assignment from a "
                       "`scoped` parameter or `null` (§28.14, v0.175)");
                return;
            }
            if (as->value) {
                if (dynamic_cast<const ast::NullLiteralExpr*>(as->value.get()))
                    return;  // clear — always admitted
                if (scopedUseKind(*as->value) == 1) return;  // store the param
                report(Severity::Error, as->value->span.begin,
                       "a `scoped` field may only be assigned a `scoped` "
                       "parameter of the enclosing method/constructor, or "
                       "cleared to `null` — any other value's captures were "
                       "never admitted against this object's bound (§28.14, "
                       "v0.175)");
            }
            return;
        }
        // Store into ANOTHER object's scoped field (`x._f := …`): the callee
        // may store only into the bounding object ITSELF. // per §28.14
        if (auto* ma =
                dynamic_cast<const ast::MemberAccessExpr*>(as->target.get())) {
            if (auto* fsym =
                    dynamic_cast<sema::FieldSymbol*>(ctx_.exprSymbol(*ma))) {
                bool isScopedField = false;
                for (std::string_view mod : fsym->modifiers)
                    if (mod == "scoped") isScopedField = true;
                auto* tid =
                    dynamic_cast<const ast::IdentifierExpr*>(ma->target.get());
                const bool thisTarget = tid && tid->name == "this";
                if (isScopedField && !thisTarget) {
                    report(Severity::Error, as->target->span.begin,
                           "a callback may be stored only into a `scoped` "
                           "field of the bounding object ITSELF — storing "
                           "into another object's `scoped` field extends the "
                           "callback past its bound (§28.14, v0.175)");
                    return;
                }
            }
        }
        // A scoped PARAMETER as assignment target (reassignment).
        if (as->target && scopedUseKind(*as->target) == 1) {
            report(Severity::Error, as->target->span.begin,
                   "a `scoped` parameter may not be reassigned (§28.14, "
                   "v0.175)");
            return;
        }
        if (as->target) scopedCalleeWalkExpr(*as->target);
        if (as->value) scopedCalleeWalkExpr(*as->value);
        return;
    }
    if (auto* rs = dynamic_cast<const ast::ReturnStmt*>(&s)) {
        if (rs->value) {
            if (scopedRestrictedUse(*rs->value, "be returned")) return;
            scopedCalleeWalkExpr(*rs->value);
        }
        return;
    }
    if (auto* ld = dynamic_cast<const ast::LocalDecl*>(&s)) {
        if (ld->initializer) {
            if (scopedRestrictedUse(*ld->initializer,
                                    "be bound to a local"))
                return;
            scopedCalleeWalkExpr(*ld->initializer);
        }
        return;
    }
    if (auto* lc = dynamic_cast<const ast::LocalConstDecl*>(&s)) {
        if (lc->value) scopedCalleeWalkExpr(*lc->value);
        return;
    }
    if (auto* es = dynamic_cast<const ast::ExpressionStmt*>(&s)) {
        if (es->expr) scopedCalleeWalkExpr(*es->expr);
        return;
    }
    if (auto* ts = dynamic_cast<const ast::ThrowStmt*>(&s)) {
        if (ts->value) scopedCalleeWalkExpr(*ts->value);
        return;
    }
    if (auto* ifs = dynamic_cast<const ast::IfStmt*>(&s)) {
        if (ifs->ifBranch.condition)
            scopedCalleeWalkExpr(*ifs->ifBranch.condition);
        for (const auto& bs : ifs->ifBranch.body)
            if (bs) scopedCalleeWalkStmt(*bs);
        for (const auto& el : ifs->elifBranches) {
            if (el.condition) scopedCalleeWalkExpr(*el.condition);
            for (const auto& bs : el.body)
                if (bs) scopedCalleeWalkStmt(*bs);
        }
        for (const auto& bs : ifs->elseBody)
            if (bs) scopedCalleeWalkStmt(*bs);
        return;
    }
    if (auto* ws = dynamic_cast<const ast::WhileStmt*>(&s)) {
        if (ws->condition) scopedCalleeWalkExpr(*ws->condition);
        for (const auto& bs : ws->body)
            if (bs) scopedCalleeWalkStmt(*bs);
        return;
    }
    if (auto* fs = dynamic_cast<const ast::ForStmt*>(&s)) {
        if (fs->iterable) scopedCalleeWalkExpr(*fs->iterable);
        if (fs->withExpr) scopedCalleeWalkExpr(*fs->withExpr);
        for (const auto& bs : fs->body)
            if (bs) scopedCalleeWalkStmt(*bs);
        return;
    }
    if (auto* db = dynamic_cast<const ast::DoEndBlock*>(&s)) {
        for (const auto& bs : db->body)
            if (bs) scopedCalleeWalkStmt(*bs);
        return;
    }
    if (auto* is = dynamic_cast<const ast::IncrementStmt*>(&s)) {
        if (is->target) scopedCalleeWalkExpr(*is->target);
        return;
    }
    if (auto* ds = dynamic_cast<const ast::DiscardStmt*>(&s)) {
        if (ds->value) {
            if (scopedRestrictedUse(*ds->value, "be discarded")) return;
            scopedCalleeWalkExpr(*ds->value);
        }
        return;
    }
    if (auto* ys = dynamic_cast<const ast::YieldStmt*>(&s)) {
        if (ys->value) {
            if (scopedRestrictedUse(*ys->value, "be yielded")) return;
            scopedCalleeWalkExpr(*ys->value);
        }
        return;
    }
    if (auto* trys = dynamic_cast<const ast::TryStmt*>(&s)) {
        for (const auto& bs : trys->body)
            if (bs) scopedCalleeWalkStmt(*bs);
        for (const auto& arm : trys->catches) {
            if (arm.guard) scopedCalleeWalkExpr(*arm.guard);
            if (arm.body) scopedCalleeWalkStmt(*arm.body);
        }
        for (const auto& bs : trys->finallyBody)
            if (bs) scopedCalleeWalkStmt(*bs);
        return;
    }
    if (auto* cs = dynamic_cast<const ast::CaseStmt*>(&s)) {
        if (cs->scrutinee) scopedCalleeWalkExpr(*cs->scrutinee);
        for (const auto& arm : cs->arms) {
            if (arm.guard) scopedCalleeWalkExpr(*arm.guard);
            if (arm.exprBody) scopedCalleeWalkExpr(*arm.exprBody);
            for (const auto& bs : arm.stmts)
                if (bs) scopedCalleeWalkStmt(*bs);
        }
        return;
    }
}

void MethodOwnership::scopedCalleeWalkExpr(const ast::Expr& e) {
    // Admitted context 1: INVOCATION — the scoped param / field is the callee.
    if (auto* ce = dynamic_cast<const ast::CallExpr*>(&e)) {
        const bool calleeIsScoped =
            ce->callee && scopedUseKind(*ce->callee) != 0;
        if (!calleeIsScoped && ce->callee) scopedCalleeWalkExpr(*ce->callee);
        for (std::size_t i = 0; i < ce->args.size(); ++i) {
            const auto& a = ce->args[i];
            if (!a.value) continue;
            std::string_view name;
            int kind = scopedUseKind(*a.value, &name);
            if (kind == 1) {
                // Admitted context 3: FORWARDING to a scoped parameter of a
                // call whose bounding object is the same object (`this`).
                bool formalScoped =
                    scopedFormalAt(ce->callee.get(), nullptr, i);
                bool onThis = false;
                if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(
                        ce->callee.get())) {
                    auto* tid = dynamic_cast<const ast::IdentifierExpr*>(
                        ma->target.get());
                    onThis = tid && tid->name == "this";
                } else if (auto* cid =
                               dynamic_cast<const ast::IdentifierExpr*>(
                                   ce->callee.get())) {
                    // Bare method name: an instance method of this class —
                    // receiver is the implicit `this`.
                    if (auto* ms = dynamic_cast<sema::MethodSymbol*>(
                            ctx_.exprSymbol(*cid))) {
                        onThis = true;
                        for (std::string_view mod : ms->modifiers)
                            if (mod == "static") onThis = false;
                    }
                }
                if (formalScoped && onThis) continue;  // admitted forward
                report(Severity::Error, a.value->span.begin,
                       std::string("`") + std::string(name) +
                           "` is a `scoped` parameter — it may be forwarded "
                           "only to another `scoped` parameter of a call on "
                           "the same bounding object (`this`) (§28.14, "
                           "v0.175)");
                continue;
            }
            if (kind == 2) {
                report(Severity::Error, a.value->span.begin,
                       std::string("`") + std::string(name) +
                           "` is a `scoped` field — it is never readable-out "
                           "as an escaping value, including as a call "
                           "argument (§28.14, v0.175)");
                continue;
            }
            scopedCalleeWalkExpr(*a.value);
        }
        return;
    }
    if (auto* nc = dynamic_cast<const ast::NullCondCallExpr*>(&e)) {
        // `_f?(...)` — null-conditional invocation of a nullable scoped field.
        const bool calleeIsScoped =
            nc->callee && scopedUseKind(*nc->callee) != 0;
        if (!calleeIsScoped && nc->callee) scopedCalleeWalkExpr(*nc->callee);
        for (const auto& a : nc->args) {
            if (!a.value) continue;
            if (scopedRestrictedUse(*a.value, "be passed here")) continue;
            scopedCalleeWalkExpr(*a.value);
        }
        return;
    }
    // Admitted context 2: null comparison (`_f <> null`, `cb = null`) — a
    // narrowing test, not an escaping read.
    if (auto* be = dynamic_cast<const ast::BinaryExpr*>(&e)) {
        if ((be->op == "=" || be->op == "<>") && be->left && be->right) {
            const bool leftNull =
                dynamic_cast<const ast::NullLiteralExpr*>(be->left.get()) !=
                nullptr;
            const bool rightNull =
                dynamic_cast<const ast::NullLiteralExpr*>(be->right.get()) !=
                nullptr;
            if (leftNull != rightNull) {
                const ast::Expr& other = leftNull ? *be->right : *be->left;
                if (scopedUseKind(other) != 0) return;  // admitted test
            }
        }
        if (be->left) scopedCalleeWalkExpr(*be->left);
        if (be->right) scopedCalleeWalkExpr(*be->right);
        return;
    }
    if (auto* ne = dynamic_cast<const ast::NewExpr*>(&e)) {
        for (const auto& a : ne->args) {
            if (!a.value) continue;
            // Forwarding into a CONSTRUCTOR is never same-object (the new
            // object is a different bounding object). // per §28.14
            if (scopedRestrictedUse(*a.value,
                                    "be passed to a constructor"))
                continue;
            scopedCalleeWalkExpr(*a.value);
        }
        return;
    }
    if (dynamic_cast<const ast::IdentifierExpr*>(&e) ||
        dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
        if (scopedUseKind(e) != 0) {
            scopedRestrictedUse(e, "be read as a value");
            return;
        }
        if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
            if (ma->target) scopedCalleeWalkExpr(*ma->target);
        }
        return;
    }
    if (auto* nm = dynamic_cast<const ast::NullCondMemberExpr*>(&e)) {
        if (nm->target) scopedCalleeWalkExpr(*nm->target);
        return;
    }
    if (auto* ix = dynamic_cast<const ast::IndexExpr*>(&e)) {
        if (ix->target) scopedCalleeWalkExpr(*ix->target);
        for (const auto& a : ix->indices)
            if (a.value) scopedCalleeWalkExpr(*a.value);
        return;
    }
    if (auto* un = dynamic_cast<const ast::UnaryExpr*>(&e)) {
        if (un->operand) {
            if (un->op == "move" &&
                scopedRestrictedUse(*un->operand, "be moved"))
                return;
            scopedCalleeWalkExpr(*un->operand);
        }
        return;
    }
    if (auto* pb = dynamic_cast<const ast::PostfixBangExpr*>(&e)) {
        if (pb->operand) scopedCalleeWalkExpr(*pb->operand);
        return;
    }
    if (auto* tt = dynamic_cast<const ast::TypeTestExpr*>(&e)) {
        if (tt->operand) scopedCalleeWalkExpr(*tt->operand);
        return;
    }
    if (auto* cd = dynamic_cast<const ast::ConditionalExpr*>(&e)) {
        if (cd->condition) scopedCalleeWalkExpr(*cd->condition);
        if (cd->thenValue) scopedCalleeWalkExpr(*cd->thenValue);
        if (cd->elseValue) scopedCalleeWalkExpr(*cd->elseValue);
        return;
    }
    if (auto* th = dynamic_cast<const ast::ThrowExpr*>(&e)) {
        if (th->operand) scopedCalleeWalkExpr(*th->operand);
        return;
    }
    if (auto* rg = dynamic_cast<const ast::RangeExpr*>(&e)) {
        if (rg->lo) scopedCalleeWalkExpr(*rg->lo);
        if (rg->hi) scopedCalleeWalkExpr(*rg->hi);
        if (rg->step) scopedCalleeWalkExpr(*rg->step);
        return;
    }
    if (auto* we = dynamic_cast<const ast::WithExpr*>(&e)) {
        if (we->source) scopedCalleeWalkExpr(*we->source);
        for (const auto& ia : we->assigns)
            if (ia.value) scopedCalleeWalkExpr(*ia.value);
        return;
    }
    if (auto* cl = dynamic_cast<const ast::CollectionLiteralExpr*>(&e)) {
        for (const auto& k : cl->keys)
            if (k) scopedCalleeWalkExpr(*k);
        for (const auto& el : cl->elements)
            if (el) scopedCalleeWalkExpr(*el);
        return;
    }
    if (auto* isx = dynamic_cast<const ast::InterpolatedStringExpr*>(&e)) {
        for (const auto& part : isx->parts)
            if (part.expr) scopedCalleeWalkExpr(*part.expr);
        return;
    }
    if (auto* lam = dynamic_cast<const ast::LambdaExpr*>(&e)) {
        // A lambda CAPTURING a scoped param/field would let it escape through
        // the closure — every use inside the body is checked as a bare use.
        if (lam->exprBody) scopedCalleeWalkExpr(*lam->exprBody);
        for (const auto& bs : lam->stmts)
            if (bs) scopedCalleeWalkStmt(*bs);
        return;
    }
    if (auto* cx = dynamic_cast<const ast::CaseExpr*>(&e)) {
        if (cx->scrutinee) scopedCalleeWalkExpr(*cx->scrutinee);
        for (const auto& arm : cx->arms) {
            if (arm.guard) scopedCalleeWalkExpr(*arm.guard);
            if (arm.exprBody) scopedCalleeWalkExpr(*arm.exprBody);
            for (const auto& bs : arm.stmts)
                if (bs) scopedCalleeWalkStmt(*bs);
        }
        return;
    }
}

// -- §28.14 v0.177 (#1419): the call-bounded default -------------------------
//
// Callee-side verification for PLAIN function-typed parameters. See the
// declaration block in Ownership_impl.hpp for the rule statement. The walk is
// syntactic (like scopedCalleeScan) so each diagnostic fires exactly once,
// and it maintains its own source-order carrier-alias map so the escape
// judgment follows local bindings transitively. // per §28.14, v0.177

sema::ParameterSymbol* MethodOwnership::callBoundedSourceOf(
    const ast::Expr& e) {
    if (callBoundedParams_.empty() && callBoundedAlias_.empty())
        return nullptr;
    if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(&e)) {
        sema::Symbol* sym = ctx_.exprSymbol(*id);
        if (!sym) return nullptr;
        if (callBoundedParams_.count(sym))
            return dynamic_cast<sema::ParameterSymbol*>(sym);
        auto it = callBoundedAlias_.find(sym);
        return it != callBoundedAlias_.end() ? it->second : nullptr;
    }
    if (auto* lam = dynamic_cast<const ast::LambdaExpr*>(&e)) {
        // A closure CARRIES the callback when it captures the parameter or a
        // carrier local — the §28.14 "judged by these same rules, applied
        // transitively" clause.
        std::vector<const ast::IdentifierExpr*> ids;
        scopedCollectCaptures(*lam, ids);
        for (const ast::IdentifierExpr* cid : ids) {
            sema::Symbol* sym = ctx_.exprSymbol(*cid);
            if (!sym) continue;
            if (callBoundedParams_.count(sym))
                return dynamic_cast<sema::ParameterSymbol*>(sym);
            auto it = callBoundedAlias_.find(sym);
            if (it != callBoundedAlias_.end()) return it->second;
        }
        return nullptr;
    }
    if (auto* cd = dynamic_cast<const ast::ConditionalExpr*>(&e)) {
        if (cd->thenValue)
            if (sema::ParameterSymbol* s = callBoundedSourceOf(*cd->thenValue))
                return s;
        return cd->elseValue ? callBoundedSourceOf(*cd->elseValue) : nullptr;
    }
    if (auto* cl = dynamic_cast<const ast::CollectionLiteralExpr*>(&e)) {
        // A local collection literal holding the callback is a local binding;
        // the CARRIER classification makes its later escape the sink.
        for (const auto& k : cl->keys)
            if (k)
                if (sema::ParameterSymbol* s = callBoundedSourceOf(*k))
                    return s;
        for (const auto& el : cl->elements)
            if (el)
                if (sema::ParameterSymbol* s = callBoundedSourceOf(*el))
                    return s;
        return nullptr;
    }
    if (auto* pb = dynamic_cast<const ast::PostfixBangExpr*>(&e))
        return pb->operand ? callBoundedSourceOf(*pb->operand) : nullptr;
    return nullptr;
}

bool MethodOwnership::exprReachableBeyondCall(const ast::Expr& recv) {
    const ast::IdentifierExpr* root = identifierRoot(recv);
    if (!root) return true;  // unrooted (temporary/complex) — conservative
    if (root->name == "this" || root->name == "base") return true;
    sema::Symbol* sym = ctx_.exprSymbol(*root);
    if (!sym) return true;  // unresolved — conservative
    if (dynamic_cast<sema::LocalSymbol*>(sym))
        return beyondCallRooted_.count(sym) != 0;  // purely local unless
                                                   // rooted beyond the call
    // Parameter / field / method-result / static-member roots all reach
    // storage that outlives the call.
    return true;
}

bool MethodOwnership::builtinCollectionStoreCallee(const ast::Expr* callee) {
    auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(callee);
    if (!ma || !ma->target) return false;
    const std::string_view m = ma->member;
    const bool storing =
        m == "Add" || m == "Insert" || m == "AddRange" || m == "TryAdd" ||
        m == "AddFirst" || m == "AddLast" || m == "AddBefore" ||
        m == "AddAfter" || m == "Push" || m == "Enqueue";
    if (!storing) return false;
    // Built-in collections resolve to a Named TypeRef with a NULL declaration
    // carrying the class name on the AST node (cf. builtinOwnedSinkArg). A
    // user type (non-null declaration) never matches — its own formals carry
    // their own §28.14 obligations.
    const sema::TypeRef* tt = ctx_.exprType(*ma->target);
    if (!tt || tt->kind != sema::TypeRef::Kind::Named || tt->declaration)
        return false;
    std::string_view cn;
    if (tt->astNode && tt->astNode->name && !tt->astNode->name->parts.empty())
        cn = tt->headName();
    else
        cn = tt->importedName;
    return cn == "Array" || cn == "Dictionary" || cn == "SortedDictionary" ||
           cn == "Set" || cn == "Stack" || cn == "Queue" ||
           cn == "LinkedList" || cn == "OwningArray" ||
           cn == "OwningDictionary" || cn == "OwningSet" ||
           cn == "OwningStack" || cn == "OwningQueue" ||
           cn == "OwningLinkedList";
}

bool MethodOwnership::verifiedCallBoundedFormalAt(const ast::Expr* callee,
                                                  const ast::NewExpr* ne,
                                                  std::size_t argPos) {
    // A formal is a VERIFIED call-bounded position when it belongs to a
    // user-authored method/constructor (its body runs this same pre-pass),
    // its modifier is empty, and its declared type is a function type.
    // // per §28.14, v0.177
    auto paramIsPlainFunction = [&](const ast::Param& p) {
        if (!p.modifier.empty() || !p.type) return false;
        if (p.type->kind == ast::TypeKind::Function) return true;
        const sema::TypeRef* rt = ctx_.resolvedType(*p.type);
        return rt && rt->kind == sema::TypeRef::Kind::Function;
    };
    if (ne) {
        if (!ne->type) return false;
        const sema::TypeRef* rt = ctx_.resolvedType(*ne->type);
        if (!rt || !rt->declaration) return false;
        for (sema::Symbol* m : rt->declaration->members) {
            if (auto* cs = dynamic_cast<sema::ConstructorSymbol*>(m)) {
                auto* d = dynamic_cast<const ast::ConstructorDecl*>(cs->decl);
                if (d && d->params.size() == ne->args.size())
                    return argPos < d->params.size() &&
                           paramIsPlainFunction(d->params[argPos]);
            }
        }
        return false;
    }
    if (!callee) return false;
    if (auto* ms = dynamic_cast<sema::MethodSymbol*>(ctx_.exprSymbol(*callee))) {
        if (auto* md = dynamic_cast<const ast::MethodDecl*>(ms->decl))
            return argPos < md->params.size() &&
                   paramIsPlainFunction(md->params[argPos]);
    }
    return false;
}

void MethodOwnership::reportCallBounded(SourceLocation loc,
                                        std::string_view paramName,
                                        std::string_view sink) {
    report(Severity::Error, loc,
           std::string("parameter `") + std::string(paramName) +
               "` is call-bounded; declare it `scoped` to store it (§28.14, "
               "v0.177) — a plain function-typed parameter may be invoked, "
               "bound to locals, and passed to another plain function-typed "
               "parameter, but may not be " +
               std::string(sink));
}

void MethodOwnership::callBoundedScan(
    const std::vector<std::unique_ptr<ast::Stmt>>& body) {
    if (callBoundedParams_.empty()) return;
    for (const auto& s : body)
        if (s) callBoundedWalkStmt(*s);
}

void MethodOwnership::callBoundedWalkStmt(const ast::Stmt& s) {
    if (auto* ld = dynamic_cast<const ast::LocalDecl*>(&s)) {
        if (ld->initializer) {
            // Binding to a local is allowed — record the carrier so a later
            // escape of the alias is judged transitively. // per §28.14
            if (sema::ParameterSymbol* src =
                    callBoundedSourceOf(*ld->initializer)) {
                if (sema::LocalSymbol* ls = findLocalSymbol(*ld))
                    callBoundedAlias_[ls] = src;
            }
            // A local rooted at a parameter / field / call result reaches
            // beyond-call storage — a collection reached through it is not a
            // "purely local collection".
            if (dynamic_cast<const ast::IdentifierExpr*>(
                    ld->initializer.get()) ||
                dynamic_cast<const ast::MemberAccessExpr*>(
                    ld->initializer.get()) ||
                dynamic_cast<const ast::IndexExpr*>(ld->initializer.get())) {
                if (exprReachableBeyondCall(*ld->initializer))
                    if (sema::LocalSymbol* ls = findLocalSymbol(*ld))
                        beyondCallRooted_.insert(ls);
            }
            callBoundedWalkExpr(*ld->initializer);
        }
        return;
    }
    if (auto* lc = dynamic_cast<const ast::LocalConstDecl*>(&s)) {
        if (lc->value) callBoundedWalkExpr(*lc->value);
        return;
    }
    if (auto* as = dynamic_cast<const ast::AssignmentStmt*>(&s)) {
        sema::ParameterSymbol* src =
            (as->op == ":=" && as->value) ? callBoundedSourceOf(*as->value)
                                          : nullptr;
        // (`+=`/`-=` never move a function value into storage; the event
        // `+=` form is the stdlib-certified Add path — §24.3, v0.177.)
        if (src && as->target) {
            // Sink: store into a FIELD of any object (qualified, bare, or
            // another object's). A `scoped`-field target is reported by the
            // scoped machinery with the assignment-specific message — skip
            // the duplicate here.
            const bool fieldTarget =
                dynamic_cast<const ast::MemberAccessExpr*>(as->target.get()) !=
                    nullptr ||
                (dynamic_cast<const ast::IdentifierExpr*>(as->target.get()) &&
                 dynamic_cast<sema::FieldSymbol*>(
                     ctx_.exprSymbol(*as->target)));
            if (fieldTarget) {
                if (scopedUseKind(*as->target) != 2)
                    reportCallBounded(as->span.begin, src->name,
                                      "stored in a field of any object");
                return;
            }
            if (auto* tid = dynamic_cast<const ast::IdentifierExpr*>(
                    as->target.get())) {
                if (auto* ps = dynamic_cast<sema::ParameterSymbol*>(
                        ctx_.exprSymbol(*tid))) {
                    if (ps->modifier == "out" || ps->modifier == "ref" ||
                        ps->modifier == "out owned") {
                        // Sink: an out/ref write hands the callback beyond
                        // the call (the caller's frame) — the return sink in
                        // another syntax.
                        reportCallBounded(
                            as->span.begin, src->name,
                            "written through an out/ref parameter (it "
                            "escapes to the caller's frame)");
                        return;
                    }
                }
                // Reassigning a plain local: the target becomes a carrier.
                if (auto* ls = dynamic_cast<sema::LocalSymbol*>(
                        ctx_.exprSymbol(*tid)))
                    callBoundedAlias_[ls] = src;
            }
            if (auto* ix =
                    dynamic_cast<const ast::IndexExpr*>(as->target.get())) {
                // Sink: indexer store into a collection reachable beyond the
                // call (`xs[i] := f` where xs is a param / field / borrow).
                if (ix->target && exprReachableBeyondCall(*ix->target)) {
                    reportCallBounded(
                        as->span.begin, src->name,
                        "stored into a collection reachable beyond the call");
                    return;
                }
            }
        }
        if (as->target) callBoundedWalkExpr(*as->target);
        if (as->value) callBoundedWalkExpr(*as->value);
        return;
    }
    if (auto* rs = dynamic_cast<const ast::ReturnStmt*>(&s)) {
        if (rs->value) {
            if (sema::ParameterSymbol* src = callBoundedSourceOf(*rs->value)) {
                reportCallBounded(rs->value->span.begin, src->name,
                                  "returned");
                return;
            }
            callBoundedWalkExpr(*rs->value);
        }
        return;
    }
    if (auto* ys = dynamic_cast<const ast::YieldStmt*>(&s)) {
        if (ys->value) {
            if (sema::ParameterSymbol* src = callBoundedSourceOf(*ys->value)) {
                reportCallBounded(ys->value->span.begin, src->name,
                                  "yielded (the iterator's consumer outlives "
                                  "the call)");
                return;
            }
            callBoundedWalkExpr(*ys->value);
        }
        return;
    }
    if (auto* es = dynamic_cast<const ast::ExpressionStmt*>(&s)) {
        if (es->expr) callBoundedWalkExpr(*es->expr);
        return;
    }
    if (auto* ts = dynamic_cast<const ast::ThrowStmt*>(&s)) {
        if (ts->value) callBoundedWalkExpr(*ts->value);
        return;
    }
    if (auto* ds = dynamic_cast<const ast::DiscardStmt*>(&s)) {
        if (ds->value) callBoundedWalkExpr(*ds->value);
        return;
    }
    if (auto* ifs = dynamic_cast<const ast::IfStmt*>(&s)) {
        if (ifs->ifBranch.condition)
            callBoundedWalkExpr(*ifs->ifBranch.condition);
        for (const auto& bs : ifs->ifBranch.body)
            if (bs) callBoundedWalkStmt(*bs);
        for (const auto& el : ifs->elifBranches) {
            if (el.condition) callBoundedWalkExpr(*el.condition);
            for (const auto& bs : el.body)
                if (bs) callBoundedWalkStmt(*bs);
        }
        for (const auto& bs : ifs->elseBody)
            if (bs) callBoundedWalkStmt(*bs);
        return;
    }
    if (auto* ws = dynamic_cast<const ast::WhileStmt*>(&s)) {
        if (ws->condition) callBoundedWalkExpr(*ws->condition);
        for (const auto& bs : ws->body)
            if (bs) callBoundedWalkStmt(*bs);
        return;
    }
    if (auto* fs = dynamic_cast<const ast::ForStmt*>(&s)) {
        if (fs->iterable) callBoundedWalkExpr(*fs->iterable);
        if (fs->withExpr) callBoundedWalkExpr(*fs->withExpr);
        for (const auto& bs : fs->body)
            if (bs) callBoundedWalkStmt(*bs);
        return;
    }
    if (auto* db = dynamic_cast<const ast::DoEndBlock*>(&s)) {
        for (const auto& bs : db->body)
            if (bs) callBoundedWalkStmt(*bs);
        return;
    }
    if (auto* is = dynamic_cast<const ast::IncrementStmt*>(&s)) {
        if (is->target) callBoundedWalkExpr(*is->target);
        return;
    }
    if (auto* trys = dynamic_cast<const ast::TryStmt*>(&s)) {
        for (const auto& bs : trys->body)
            if (bs) callBoundedWalkStmt(*bs);
        for (const auto& arm : trys->catches) {
            if (arm.guard) callBoundedWalkExpr(*arm.guard);
            if (arm.body) callBoundedWalkStmt(*arm.body);
        }
        for (const auto& bs : trys->finallyBody)
            if (bs) callBoundedWalkStmt(*bs);
        return;
    }
    if (auto* cs = dynamic_cast<const ast::CaseStmt*>(&s)) {
        if (cs->scrutinee) callBoundedWalkExpr(*cs->scrutinee);
        for (const auto& arm : cs->arms) {
            if (arm.guard) callBoundedWalkExpr(*arm.guard);
            if (arm.exprBody) callBoundedWalkExpr(*arm.exprBody);
            for (const auto& bs : arm.stmts)
                if (bs) callBoundedWalkStmt(*bs);
        }
        return;
    }
    // Break / Continue: nothing to check.
}

void MethodOwnership::callBoundedWalkExpr(const ast::Expr& e) {
    const auto* ne = dynamic_cast<const ast::NewExpr*>(&e);
    const ast::Expr* callee = nullptr;
    const std::vector<ast::CallArgument>* args = nullptr;
    if (auto* ce = dynamic_cast<const ast::CallExpr*>(&e)) {
        callee = ce->callee.get();
        args = &ce->args;
    } else if (auto* nc = dynamic_cast<const ast::NullCondCallExpr*>(&e)) {
        callee = nc->callee.get();
        args = &nc->args;
    } else if (ne) {
        args = &ne->args;
    }
    if (args) {
        const bool collStore =
            !ne && builtinCollectionStoreCallee(callee) &&
            [&] {
                auto* ma =
                    dynamic_cast<const ast::MemberAccessExpr*>(callee);
                return ma && ma->target &&
                       exprReachableBeyondCall(*ma->target);
            }();
        for (std::size_t i = 0; i < args->size(); ++i) {
            const auto& a = (*args)[i];
            if (!a.value) continue;
            if (sema::ParameterSymbol* src = callBoundedSourceOf(*a.value)) {
                // Forward into a `scoped` position: reported by the §28.14
                // admission (checkScopedAdmission), which sees the same call
                // — skip the duplicate here.
                if (scopedFormalAt(callee, ne, i)) {
                    // (admission owns the diagnostic)
                } else if (collStore) {
                    // Sink: builtin-collection mutator on a receiver
                    // reachable beyond the call.
                    reportCallBounded(
                        a.value->span.begin, src->name,
                        "stored into a collection reachable beyond the call");
                    continue;
                }
                // Any other argument position: plain→plain forwarding, or a
                // builtin callee whose retention is signature-certified —
                // allowed.
            }
            callBoundedWalkExpr(*a.value);
        }
        // Walk a non-bare callee (receiver chains may contain calls);
        // invoking the callback itself (`f()`) is the admitted use.
        if (callee && !dynamic_cast<const ast::IdentifierExpr*>(callee))
            callBoundedWalkExpr(*callee);
        return;
    }
    // Structural recursion (no sinks below — sinks are positional above).
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
        if (ma->target) callBoundedWalkExpr(*ma->target);
        return;
    }
    if (auto* nm = dynamic_cast<const ast::NullCondMemberExpr*>(&e)) {
        if (nm->target) callBoundedWalkExpr(*nm->target);
        return;
    }
    if (auto* ix = dynamic_cast<const ast::IndexExpr*>(&e)) {
        if (ix->target) callBoundedWalkExpr(*ix->target);
        for (const auto& a : ix->indices)
            if (a.value) callBoundedWalkExpr(*a.value);
        return;
    }
    if (auto* be = dynamic_cast<const ast::BinaryExpr*>(&e)) {
        if (be->left) callBoundedWalkExpr(*be->left);
        if (be->right) callBoundedWalkExpr(*be->right);
        return;
    }
    if (auto* un = dynamic_cast<const ast::UnaryExpr*>(&e)) {
        if (un->operand) callBoundedWalkExpr(*un->operand);
        return;
    }
    if (auto* pb = dynamic_cast<const ast::PostfixBangExpr*>(&e)) {
        if (pb->operand) callBoundedWalkExpr(*pb->operand);
        return;
    }
    if (auto* tt = dynamic_cast<const ast::TypeTestExpr*>(&e)) {
        if (tt->operand) callBoundedWalkExpr(*tt->operand);
        return;
    }
    if (auto* cd = dynamic_cast<const ast::ConditionalExpr*>(&e)) {
        if (cd->condition) callBoundedWalkExpr(*cd->condition);
        if (cd->thenValue) callBoundedWalkExpr(*cd->thenValue);
        if (cd->elseValue) callBoundedWalkExpr(*cd->elseValue);
        return;
    }
    if (auto* th = dynamic_cast<const ast::ThrowExpr*>(&e)) {
        if (th->operand) callBoundedWalkExpr(*th->operand);
        return;
    }
    if (auto* rg = dynamic_cast<const ast::RangeExpr*>(&e)) {
        if (rg->lo) callBoundedWalkExpr(*rg->lo);
        if (rg->hi) callBoundedWalkExpr(*rg->hi);
        if (rg->step) callBoundedWalkExpr(*rg->step);
        return;
    }
    if (auto* we = dynamic_cast<const ast::WithExpr*>(&e)) {
        if (we->source) callBoundedWalkExpr(*we->source);
        for (const auto& ia : we->assigns)
            if (ia.value) callBoundedWalkExpr(*ia.value);
        return;
    }
    if (auto* cl = dynamic_cast<const ast::CollectionLiteralExpr*>(&e)) {
        for (const auto& k : cl->keys)
            if (k) callBoundedWalkExpr(*k);
        for (const auto& el : cl->elements)
            if (el) callBoundedWalkExpr(*el);
        return;
    }
    if (auto* isx = dynamic_cast<const ast::InterpolatedStringExpr*>(&e)) {
        for (const auto& part : isx->parts)
            if (part.expr) callBoundedWalkExpr(*part.expr);
        return;
    }
    if (auto* lam = dynamic_cast<const ast::LambdaExpr*>(&e)) {
        // Walk the lambda BODY: a store that executes when the closure runs
        // (`() => do _f := f; end`) is still a store of the call-bounded
        // value. The lambda VALUE's own escape is judged at its use sites
        // via callBoundedSourceOf.
        if (lam->exprBody) callBoundedWalkExpr(*lam->exprBody);
        for (const auto& bs : lam->stmts)
            if (bs) callBoundedWalkStmt(*bs);
        return;
    }
    if (auto* cx = dynamic_cast<const ast::CaseExpr*>(&e)) {
        if (cx->scrutinee) callBoundedWalkExpr(*cx->scrutinee);
        for (const auto& arm : cx->arms) {
            if (arm.guard) callBoundedWalkExpr(*arm.guard);
            if (arm.exprBody) callBoundedWalkExpr(*arm.exprBody);
            for (const auto& bs : arm.stmts)
                if (bs) callBoundedWalkStmt(*bs);
        }
        return;
    }
    // Literals / identifiers / typeof / nameof / default / unit: no sinks.
}

// -- stdlib §6.10: TaskScope consumption ------------------------------------

void MethodOwnership::taskScopeUsageScanExpr(const ast::Expr& e,
                                             std::string_view name,
                                             bool& sawAwaitJoin,
                                             bool& sawDelegation) {
    // `await <name>.Join()`
    if (auto* un = dynamic_cast<const ast::UnaryExpr*>(&e)) {
        if (un->op == "await" && un->operand) {
            if (auto* ce =
                    dynamic_cast<const ast::CallExpr*>(un->operand.get())) {
                if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(
                        ce->callee.get())) {
                    auto* tid = dynamic_cast<const ast::IdentifierExpr*>(
                        ma->target.get());
                    if (tid && tid->name == name && ma->member == "Join")
                        sawAwaitJoin = true;
                }
            }
        }
        if (un->operand)
            taskScopeUsageScanExpr(*un->operand, name, sawAwaitJoin,
                                   sawDelegation);
        return;
    }
    // A bare-`name` call ARGUMENT delegates the scope (the callee may join
    // it) — the missing-Join error is then not provable here.
    auto scanArgs = [&](const std::vector<ast::CallArgument>& args) {
        for (const auto& a : args) {
            if (!a.value) continue;
            if (auto* id =
                    dynamic_cast<const ast::IdentifierExpr*>(a.value.get()))
                if (id->name == name) sawDelegation = true;
            taskScopeUsageScanExpr(*a.value, name, sawAwaitJoin,
                                   sawDelegation);
        }
    };
    if (auto* ce = dynamic_cast<const ast::CallExpr*>(&e)) {
        if (ce->callee)
            taskScopeUsageScanExpr(*ce->callee, name, sawAwaitJoin,
                                   sawDelegation);
        scanArgs(ce->args);
        return;
    }
    if (auto* nc = dynamic_cast<const ast::NullCondCallExpr*>(&e)) {
        if (nc->callee)
            taskScopeUsageScanExpr(*nc->callee, name, sawAwaitJoin,
                                   sawDelegation);
        scanArgs(nc->args);
        return;
    }
    if (auto* ne = dynamic_cast<const ast::NewExpr*>(&e)) {
        scanArgs(ne->args);
        return;
    }
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
        if (ma->target)
            taskScopeUsageScanExpr(*ma->target, name, sawAwaitJoin,
                                   sawDelegation);
        return;
    }
    if (auto* be = dynamic_cast<const ast::BinaryExpr*>(&e)) {
        if (be->left)
            taskScopeUsageScanExpr(*be->left, name, sawAwaitJoin,
                                   sawDelegation);
        if (be->right)
            taskScopeUsageScanExpr(*be->right, name, sawAwaitJoin,
                                   sawDelegation);
        return;
    }
    if (auto* lam = dynamic_cast<const ast::LambdaExpr*>(&e)) {
        // A lambda capturing the scope may Join it (a worker helper): treat
        // as delegation — not provably missing.
        std::vector<const ast::IdentifierExpr*> ids;
        scopedCollectCaptures(*lam, ids);
        for (const ast::IdentifierExpr* id : ids)
            if (id->name == name) sawDelegation = true;
        return;
    }
    if (auto* cd = dynamic_cast<const ast::ConditionalExpr*>(&e)) {
        if (cd->condition)
            taskScopeUsageScanExpr(*cd->condition, name, sawAwaitJoin,
                                   sawDelegation);
        if (cd->thenValue)
            taskScopeUsageScanExpr(*cd->thenValue, name, sawAwaitJoin,
                                   sawDelegation);
        if (cd->elseValue)
            taskScopeUsageScanExpr(*cd->elseValue, name, sawAwaitJoin,
                                   sawDelegation);
        return;
    }
}

void MethodOwnership::taskScopeUsageScanStmt(const ast::Stmt& s,
                                             std::string_view name,
                                             bool& sawAwaitJoin,
                                             bool& sawDelegation) {
    if (auto* ld = dynamic_cast<const ast::LocalDecl*>(&s)) {
        if (ld->initializer)
            taskScopeUsageScanExpr(*ld->initializer, name, sawAwaitJoin,
                                   sawDelegation);
        return;
    }
    if (auto* es = dynamic_cast<const ast::ExpressionStmt*>(&s)) {
        if (es->expr)
            taskScopeUsageScanExpr(*es->expr, name, sawAwaitJoin,
                                   sawDelegation);
        return;
    }
    if (auto* as = dynamic_cast<const ast::AssignmentStmt*>(&s)) {
        if (as->value)
            taskScopeUsageScanExpr(*as->value, name, sawAwaitJoin,
                                   sawDelegation);
        return;
    }
    if (auto* rs = dynamic_cast<const ast::ReturnStmt*>(&s)) {
        if (rs->value)
            taskScopeUsageScanExpr(*rs->value, name, sawAwaitJoin,
                                   sawDelegation);
        return;
    }
    if (auto* ifs = dynamic_cast<const ast::IfStmt*>(&s)) {
        if (ifs->ifBranch.condition)
            taskScopeUsageScanExpr(*ifs->ifBranch.condition, name,
                                   sawAwaitJoin, sawDelegation);
        for (const auto& bs : ifs->ifBranch.body)
            if (bs) taskScopeUsageScanStmt(*bs, name, sawAwaitJoin,
                                           sawDelegation);
        for (const auto& el : ifs->elifBranches) {
            if (el.condition)
                taskScopeUsageScanExpr(*el.condition, name, sawAwaitJoin,
                                       sawDelegation);
            for (const auto& bs : el.body)
                if (bs) taskScopeUsageScanStmt(*bs, name, sawAwaitJoin,
                                               sawDelegation);
        }
        for (const auto& bs : ifs->elseBody)
            if (bs) taskScopeUsageScanStmt(*bs, name, sawAwaitJoin,
                                           sawDelegation);
        return;
    }
    if (auto* ws = dynamic_cast<const ast::WhileStmt*>(&s)) {
        if (ws->condition)
            taskScopeUsageScanExpr(*ws->condition, name, sawAwaitJoin,
                                   sawDelegation);
        for (const auto& bs : ws->body)
            if (bs) taskScopeUsageScanStmt(*bs, name, sawAwaitJoin,
                                           sawDelegation);
        return;
    }
    if (auto* fs = dynamic_cast<const ast::ForStmt*>(&s)) {
        if (fs->iterable)
            taskScopeUsageScanExpr(*fs->iterable, name, sawAwaitJoin,
                                   sawDelegation);
        for (const auto& bs : fs->body)
            if (bs) taskScopeUsageScanStmt(*bs, name, sawAwaitJoin,
                                           sawDelegation);
        return;
    }
    if (auto* db = dynamic_cast<const ast::DoEndBlock*>(&s)) {
        for (const auto& bs : db->body)
            if (bs) taskScopeUsageScanStmt(*bs, name, sawAwaitJoin,
                                           sawDelegation);
        return;
    }
    if (auto* trys = dynamic_cast<const ast::TryStmt*>(&s)) {
        for (const auto& bs : trys->body)
            if (bs) taskScopeUsageScanStmt(*bs, name, sawAwaitJoin,
                                           sawDelegation);
        for (const auto& arm : trys->catches)
            if (arm.body) taskScopeUsageScanStmt(*arm.body, name,
                                                 sawAwaitJoin, sawDelegation);
        for (const auto& bs : trys->finallyBody)
            if (bs) taskScopeUsageScanStmt(*bs, name, sawAwaitJoin,
                                           sawDelegation);
        return;
    }
    if (auto* ds = dynamic_cast<const ast::DiscardStmt*>(&s)) {
        if (ds->value)
            taskScopeUsageScanExpr(*ds->value, name, sawAwaitJoin,
                                   sawDelegation);
        return;
    }
}

void MethodOwnership::checkTaskScopeConsumption(
    const std::vector<std::unique_ptr<ast::Stmt>>& body) {
    // Collect every await-using TaskScope local, then check §6.10's
    // mandatory `await Join()` on the provable subset: no `await L.Join()`
    // anywhere AND `L` is never delegated (passed / captured) — then the
    // normal path certainly lacks the join and the error is deterministic at
    // compile time; otherwise the runtime IOE from DisposeAsync backstops.
    std::function<void(const ast::Stmt&)> scan = [&](const ast::Stmt& s) {
        if (auto* ld = dynamic_cast<const ast::LocalDecl*>(&s)) {
            const bool isTS =
                (ld->type && typeRefIsTaskScope(ctx_.resolvedType(*ld->type))) ||
                (ld->initializer && exprTypeIsTaskScope(*ld->initializer));
            if (isTS && ld->isUsing && !ld->isAwaitUsing) {
                report(Severity::Error, ld->span.begin,
                       std::string("`") + std::string(ld->name) +
                           "` is a TaskScope — it may only be consumed by "
                           "`await using` (stdlib §6.10, v0.175): a "
                           "synchronous `using var` cannot await the drain "
                           "of its children; write `await using var " +
                           std::string(ld->name) + " := …`");
            } else if (isTS && ld->isAwaitUsing) {
                bool sawAwaitJoin = false, sawDelegation = false;
                for (const auto& bs : body)
                    if (bs) taskScopeUsageScanStmt(*bs, ld->name,
                                                   sawAwaitJoin,
                                                   sawDelegation);
                if (!sawAwaitJoin && !sawDelegation) {
                    report(Severity::Error, ld->span.begin,
                           std::string("`await ") + std::string(ld->name) +
                               ".Join()` is missing on the normal path — "
                               "Join is the sole re-raise site for child "
                               "faults and is mandatory before the scope is "
                               "disposed (stdlib §6.10, v0.175)");
                }
            } else if (isTS && !ld->isUsing) {
                report(Severity::Error, ld->span.begin,
                       std::string("`") + std::string(ld->name) +
                           "` is a TaskScope bound to a plain `var` — a "
                           "TaskScope may only be consumed by `await using` "
                           "(stdlib §6.10, v0.175); dropping it undisposed "
                           "or disposing it synchronously cannot await the "
                           "drain of its children");
            }
            return;
        }
        if (auto* ds = dynamic_cast<const ast::DiscardStmt*>(&s)) {
            if (ds->value && exprTypeIsTaskScope(*ds->value)) {
                report(Severity::Error, ds->value->span.begin,
                       "a TaskScope may not be discarded — it may only be "
                       "consumed by `await using` (stdlib §6.10, v0.175)");
            }
            return;
        }
        if (auto* ifs = dynamic_cast<const ast::IfStmt*>(&s)) {
            for (const auto& bs : ifs->ifBranch.body) if (bs) scan(*bs);
            for (const auto& el : ifs->elifBranches)
                for (const auto& bs : el.body) if (bs) scan(*bs);
            for (const auto& bs : ifs->elseBody) if (bs) scan(*bs);
            return;
        }
        if (auto* ws = dynamic_cast<const ast::WhileStmt*>(&s)) {
            for (const auto& bs : ws->body) if (bs) scan(*bs);
            return;
        }
        if (auto* fs = dynamic_cast<const ast::ForStmt*>(&s)) {
            for (const auto& bs : fs->body) if (bs) scan(*bs);
            return;
        }
        if (auto* db = dynamic_cast<const ast::DoEndBlock*>(&s)) {
            for (const auto& bs : db->body) if (bs) scan(*bs);
            return;
        }
        if (auto* trys = dynamic_cast<const ast::TryStmt*>(&s)) {
            for (const auto& bs : trys->body) if (bs) scan(*bs);
            for (const auto& arm : trys->catches)
                if (arm.body) scan(*arm.body);
            for (const auto& bs : trys->finallyBody) if (bs) scan(*bs);
            return;
        }
        if (auto* es = dynamic_cast<const ast::ExpressionStmt*>(&s)) {
            // `scope.Dispose()` — a synchronous dispose of a TaskScope.
            if (auto* ce =
                    dynamic_cast<const ast::CallExpr*>(es->expr.get())) {
                if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(
                        ce->callee.get())) {
                    if (ma->member == "Dispose" && ma->target &&
                        exprTypeIsTaskScope(*ma->target)) {
                        report(Severity::Error, es->expr->span.begin,
                               "a TaskScope may not be disposed "
                               "synchronously — its drain must be awaited; "
                               "it may only be consumed by `await using` "
                               "(stdlib §6.10, v0.175)");
                    }
                }
            }
            return;
        }
    };
    for (const auto& s : body)
        if (s) scan(*s);
}

} // namespace bezel::ownership
