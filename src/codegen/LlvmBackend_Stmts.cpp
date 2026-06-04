#include "codegen/LlvmBackend_impl.hpp"

namespace bezel::codegen {

bool Emitter::lowerStmt(const ast::Stmt& s) {
    if (auto* ld = dynamic_cast<const ast::LocalDecl*>(&s)) {
        lowerLocalDecl(ld, ld->initializer.get(), ld->type.get());
        if (diverged_) return true;  // CG40: diverging initializer
        if (ld->isUsing) registerUsingDisposal(ld);
        // §28.3 (line 4910): a plain `var` owned local is auto-disposed only on
        // the exception-unwind path (an `unwindOnly` Disposal); normal exit
        // requires it to reach a discharge point (sema-enforced).
        else registerUnwindOwnedLocal(ld);
        return false;
    }
    if (auto* lc = dynamic_cast<const ast::LocalConstDecl*>(&s)) {
        lowerLocalDecl(lc, lc->value.get(), lc->type.get());
        return diverged_;  // CG40
    }
    if (auto* as = dynamic_cast<const ast::AssignmentStmt*>(&s)) {
        if (as->target && as->value) {
            // per §24.4: `event += handler` / `event -= handler` (inside the
            // declaring class) desugar to Add/Remove — not a real compound
            // assignment. Checked first so the event target is not treated as
            // an lvalue.
            if (tryLowerEventCompound(*as)) return false;
            // §22.9: `obj.P := v` / `obj.P OP= v` where P is a block-form
            // property runs its setter body (with `value` = v); compound
            // assignment is a read-modify-write through getter + setter.
            if (as->op == ":=") {
                if (lowerPropertySet(*as->target, *as->value))
                    return false;
                // §22.10: indexer setter for `obj[i] := v`.
                // Evaluate RHS ONCE to avoid double side-effects if
                // lowerIndexerSet fails and we fall through to the
                // plain lvalue path below.
                if (auto* ix = dynamic_cast<const ast::IndexExpr*>(
                        as->target.get())) {
                    llvm::Value* rhsOnce = lowerExpr(*as->value);
                    if (!rhsOnce) return false;
                    if (lowerIndexerSet(*ix, rhsOnce)) return false;
                    // issue #388 / stdlib §4.4-§4.9: heap-collection indexer
                    // setter `a[i] := v` / `dict[key] := v` (the OWNING
                    // variants dispose the displaced owner, §4.5 / §4.9).
                    if (lowerCollectionIndexerSet(*ix, rhsOnce)) return false;
                    // Not an indexer property — fall through to plain array
                    // assignment using the already-computed RHS.
                    llvm::Type* elemTy2 = nullptr;
                    llvm::Value* addr2 = lvalueAddr(*as->target, elemTy2);
                    if (!addr2) return false;
                    builder_.CreateStore(rhsOnce, addr2);
                    return false;
                }
            } else if (lowerPropertyCompound(*as)) {
                return false;
            }
            llvm::Type* elemTy = nullptr;
            llvm::Value* addr = lvalueAddr(*as->target, elemTy);
            if (!addr) return false;
            llvm::Value* rhs = lowerExpr(*as->value);
            if (!rhs) return false;
            // §5.1: an int assigned to a wider numeric slot (e.g. `d := n;`
            // with `d: double`, `n: int`) is implicitly widened. Materialize
            // the conversion sema admitted so the slot receives the value, not
            // the source type's raw bits (issue #408). No-op when the rhs type
            // already matches or no §5.1 numeric widening applies.
            if (elemTy)
                rhs = coerceNumericToType(rhs, elemTy);
            // CG-nullable: if target slot is a nullable-prim struct and
            // rhs is a plain primitive, wrap it (or fix null literal).
            if (elemTy && llvm::isa<llvm::StructType>(elemTy)) {
                if (auto* st = llvm::dyn_cast<llvm::StructType>(elemTy)) {
                    if (st->getNumElements() == 2 && st->getElementType(0) == nullablePrimHasValTy() &&
                        rhs->getType() != elemTy) {
                        if (rhs->getType()->isPointerTy() &&
                            llvm::isa<llvm::ConstantPointerNull>(rhs)) {
                            rhs = llvm::ConstantAggregateZero::get(elemTy);
                        } else if (rhs->getType() == st->getElementType(1)) {
                            llvm::Value* res = builder_.CreateInsertValue(
                                llvm::UndefValue::get(elemTy),
                                llvm::ConstantInt::get(nullablePrimHasValTy(), 1), {0u});
                            rhs = builder_.CreateInsertValue(res, rhs, {1u});
                        }
                    }
                }
            }
            if (as->op == ":=") {
                builder_.CreateStore(rhs, addr);
            } else if (as->op == "??=") {
                // §4.7: null-coalescing assignment `x ??= e` — store e
                // into x only when x is currently null.
                llvm::Value* cur = builder_.CreateLoad(elemTy, addr);
                llvm::Value* isNull = nullptr;
                if (cur->getType()->isPointerTy()) {
                    isNull = builder_.CreateICmpEQ(
                        cur, llvm::ConstantPointerNull::get(ptrTy()));
                } else if (cur->getType()->isStructTy()) {
                    // CG-nullable: nullable primitive {i8 hasValue; primType value}.
                    // Null when hasValue == 0.
                    isNull = builder_.CreateNot(nullablePrimHasValue(cur));
                    // Wrap rhs (plain prim) into nullable struct for the store.
                    if (rhs->getType() != elemTy) {
                        if (auto* st = llvm::dyn_cast<llvm::StructType>(elemTy)) {
                            if (st->getNumElements() == 2 &&
                                st->getElementType(0) == nullablePrimHasValTy() &&
                                rhs->getType() == st->getElementType(1)) {
                                llvm::Value* wrapped = builder_.CreateInsertValue(
                                    llvm::UndefValue::get(elemTy),
                                    llvm::ConstantInt::get(nullablePrimHasValTy(), 1), {0u});
                                rhs = builder_.CreateInsertValue(wrapped, rhs, {1u});
                            }
                        }
                    }
                }
                if (isNull) {
                    auto* assignBB = llvm::BasicBlock::Create(llvm_, "coaleq.do", currentFn_);
                    auto* endBB = llvm::BasicBlock::Create(llvm_, "coaleq.end", currentFn_);
                    builder_.CreateCondBr(isNull, assignBB, endBB);
                    builder_.SetInsertPoint(assignBB);
                    builder_.CreateStore(rhs, addr);
                    builder_.CreateBr(endBB);
                    builder_.SetInsertPoint(endBB);
                }
            } else {
                // CG31: compound assignment `target OP= value` — read,
                // apply OP (overflow-checked like plain arithmetic),
                // write back.
                std::string op(as->op);
                if (!op.empty() && op.back() == '=') op.pop_back();
                llvm::Value* cur = builder_.CreateLoad(elemTy, addr);
                llvm::Value* res = emitCompoundOp(
                    op, cur, rhs, elemTy->isFloatingPointTy(),
                    isUnsignedTargetExpr(*as->target));
                if (res) builder_.CreateStore(res, addr);
            }
        }
        return false;
    }
    // CG31: `x++` / `x--` — overflow-checked increment/decrement.
    if (auto* inc = dynamic_cast<const ast::IncrementStmt*>(&s)) {
        if (inc->target) {
            llvm::Type* elemTy = nullptr;
            llvm::Value* addr = lvalueAddr(*inc->target, elemTy);
            if (addr) {
                bool fp = elemTy->isFloatingPointTy();
                llvm::Value* cur = builder_.CreateLoad(elemTy, addr);
                llvm::Value* one =
                    fp ? llvm::cast<llvm::Value>(
                             llvm::ConstantFP::get(elemTy, 1.0))
                       : llvm::cast<llvm::Value>(
                             llvm::ConstantInt::get(elemTy, 1));
                llvm::Value* res = emitCompoundOp(
                    inc->op == "++" ? "+" : "-", cur, one, fp,
                    isUnsignedTargetExpr(*inc->target));
                if (res) builder_.CreateStore(res, addr);
            }
        }
        return false;
    }
    if (auto* rs = dynamic_cast<const ast::ReturnStmt*>(&s)) {
        // The return value is evaluated first, then in-scope `using
        // var`s are disposed (§28.3), then control leaves — so the
        // returned value reflects pre-disposal state.
        llvm::Value* v = rs->value ? lowerExpr(*rs->value) : nullptr;
        if (diverged_) return true;  // CG40: the value expression threw
        // §28.3 ownership transfer: if the return expression is a bare
        // `using var` local, exclude it from disposal — ownership moves
        // to the caller.  Without this, the local would be freed before
        // `ret`, giving the caller a dangling pointer.
        llvm::AllocaInst* retSlot = nullptr;
        if (rs->value) {
            if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(rs->value.get())) {
                sema::Symbol* sym = ctx_.exprSymbol(*rs->value);
                if (auto* loc = dynamic_cast<sema::LocalSymbol*>(sym)) {
                    auto lit = locals_.find(loc->decl);
                    if (lit != locals_.end()) retSlot = lit->second;
                }
            }
        }
        if (retSlot) {
            for (auto& scope : disposeStack_) {
                scope.erase(
                    std::remove_if(scope.begin(), scope.end(),
                        [retSlot](const auto& e) { return e.slot == retSlot; }),
                    scope.end());
            }
        }
        emitAllDisposals();
        // CG21c (§31.2): if inside a try-with-finally, stash the return
        // value and route through the finally block instead of ret-ing now.
        if (!finallyStack_.empty()) {
            auto& fr = finallyStack_.back();
            if (fr.retValSlot && v)
                builder_.CreateStore(v, fr.retValSlot);
            builder_.CreateStore(builder_.getInt32(1), fr.exitModSlot);
            builder_.CreateBr(fr.finNormalBB);
            return true;
        }
        emitReturn(currentFn_, v);
        return true;
    }
    if (auto* es = dynamic_cast<const ast::ExpressionStmt*>(&s)) {
        if (es->expr) lowerExpr(*es->expr);  // side effects only (CG3: none)
        return diverged_;  // CG40: a throw-expression terminated the block
    }
    if (auto* ds = dynamic_cast<const ast::DiscardStmt*>(&s)) {
        if (ds->value) lowerExpr(*ds->value);
        return diverged_;  // CG40
    }
    if (auto* ifs = dynamic_cast<const ast::IfStmt*>(&s)) {
        return lowerIf(*ifs);
    }
    if (auto* ws = dynamic_cast<const ast::WhileStmt*>(&s)) {
        return lowerWhile(*ws);
    }
    if (auto* fors = dynamic_cast<const ast::ForStmt*>(&s)) {
        if (lowerRangeFor(*fors)) return false;
        // issue #370 / §4.8: `for var k in <dict>.Keys` — iterate dict keys.
        // (`<dict>.Keys` has no sema element type, so this must run before the
        // generic for-handlers; it recognizes the AST shape directly.)
        if (lowerDictKeysFor(*fors)) return false;
        // issue #370 / §4.10: `for var x in <set>` over a dict-backed Set<T>.
        // Must precede lowerArrayFor (Set is a collection-type name but is NOT
        // array-backed when its element routes through the generic hash table).
        if (lowerDictBackedSetFor(*fors)) return false;
        // §15.1: `for var x in a` over an inline array.
        if (lowerArrayFor(*fors)) return false;
        // §4.6: `for var c in s` over a string — iterates chars.
        if (lowerStringFor(*fors)) return false;
        // stdlib §5: `for var x in <lazy LINQ pipeline>` — iterate the deferred
        // enumerator without materializing it.
        if (lowerLazyLinqFor(*fors)) return false;
        // §29.2 (B1): `for var x in items` where items is IEnumerable<T>
        // typed as a builtin interface — treat runtime value as bz_array_t*.
        if (lowerIEnumerableBuiltinFor(*fors)) return false;
        // §29.2 (A): `for var x in obj` over user-defined IEnumerable<T>.
        if (lowerIEnumerableFor(*fors)) return false;
        // Non-range, non-array, non-string, non-IEnumerable iterables.
        ctx_.diagnostics().report(
            Severity::Error, s.span.begin,
            "not yet implemented: codegen for this for statement");
        return false;
    }
    // CG11: break / continue jump to the innermost loop's exit /
    // condition block. CG13: they first dispose the loop body's (and
    // any nested) `using var`s. Both terminate the current block.
    // CG21c (§31.2): if inside a try-with-finally, route through finally.
    if (dynamic_cast<const ast::BreakStmt*>(&s)) {
        if (!loopStack_.empty()) {
            emitLoopDisposals(loopStack_.back().scopeDepth);
            if (!finallyStack_.empty()) {
                auto& fr = finallyStack_.back();
                builder_.CreateStore(builder_.getInt32(2), fr.exitModSlot);
                builder_.CreateBr(fr.finNormalBB);
            } else {
                builder_.CreateBr(loopStack_.back().breakTarget);
            }
            return true;
        }
        return false;
    }
    if (dynamic_cast<const ast::ContinueStmt*>(&s)) {
        if (!loopStack_.empty()) {
            emitLoopDisposals(loopStack_.back().scopeDepth);
            if (!finallyStack_.empty()) {
                auto& fr = finallyStack_.back();
                builder_.CreateStore(builder_.getInt32(3), fr.exitModSlot);
                builder_.CreateBr(fr.finNormalBB);
            } else {
                builder_.CreateBr(loopStack_.back().continueTarget);
            }
            return true;
        }
        return false;
    }
    // CG13: a `do … end` block is a disposal scope (§A.6, §28.3).
    if (auto* db = dynamic_cast<const ast::DoEndBlock*>(&s)) {
        return lowerScopedBlock(db->body);
    }
    // CG20: throw — store the exception (a plain `throw;` re-raises the
    // one already in flight) and branch to the unwind target.
    if (auto* th = dynamic_cast<const ast::ThrowStmt*>(&s)) {
        llvm::Value* e = th->value ? lowerExpr(*th->value) : nullptr;
        // #387 / §31.1: pass the thrown expression's AST so emitThrowValue can
        // free the caught exception when this `throw <newExpr>` leaving a catch
        // arm provably does not reference it.
        emitThrowValue(e, th->value.get());
        return true;
    }
    // CG20: try / catch.
    if (auto* trys = dynamic_cast<const ast::TryStmt*>(&s)) {
        return lowerTry(*trys);
    }
    // CG38: statement-position `case`.
    if (auto* cs = dynamic_cast<const ast::CaseStmt*>(&s)) {
        return lowerCaseStmt(*cs);
    }
    // §29.3: yield return / yield break inside an iterator method.
    if (auto* ys = dynamic_cast<const ast::YieldStmt*>(&s)) {
        if (iterStateStruct_ && iterSelf_) {
            // Save all iterator locals to the state struct before yielding.
            for (auto& [allocaSlot, fieldIdx] : iterAllocaToField_) {
                llvm::Value* fptr = builder_.CreateStructGEP(
                    iterStateStruct_, iterSelf_, fieldIdx);
                llvm::Value* curVal = builder_.CreateLoad(
                    allocaSlot->getAllocatedType(), allocaSlot);
                builder_.CreateStore(curVal, fptr);
            }

            if (ys->isBreak) {
                // yield break: set state=-1, return false.
                builder_.CreateStore(builder_.getInt32(-1), iterStateGEP_);
                builder_.CreateRet(builder_.getFalse());
                return true;
            }
            // yield return expr: store to current_val, set next state, return true.
            if (ys->value) {
                llvm::Value* v = lowerExpr(*ys->value);
                if (v) {
                    if (v->getType() != iterElemTy_ &&
                        v->getType()->isIntegerTy() && iterElemTy_->isIntegerTy())
                        v = builder_.CreateTrunc(v, iterElemTy_);
                    if (v->getType() == iterElemTy_) {
                        llvm::Value* cvField = builder_.CreateStructGEP(
                            iterStateStruct_, iterSelf_, iterCurrentValField_);
                        builder_.CreateStore(v, cvField);
                    }
                }
            }
            unsigned nextState = ++iterNextYieldState_;
            builder_.CreateStore(builder_.getInt32((int)nextState), iterStateGEP_);
            builder_.CreateRet(builder_.getTrue());
            // Continue emitting code after this yield in the next state block.
            if (nextState < iterStateBBs_.size()) {
                builder_.SetInsertPoint(iterStateBBs_[nextState]);
                // Restore all iterator locals from the state struct.
                for (auto& [allocaSlot, fieldIdx] : iterAllocaToField_) {
                    llvm::Value* fptr = builder_.CreateStructGEP(
                        iterStateStruct_, iterSelf_, fieldIdx);
                    llvm::Value* savedVal = builder_.CreateLoad(
                        allocaSlot->getAllocatedType(), fptr);
                    builder_.CreateStore(savedVal, allocaSlot);
                }
            }
            return false;  // not terminated (we continue in next state BB)
        }
        return false;
    }
    return false;  // unhandled statement kinds land in later slices
}


bool Emitter::lowerTry(const ast::TryStmt& trys) {
    bool hasFin = trys.hasFinally;
    // CG21b/CG21c: a `finally` runs on every exit from the try.
    // Normal flow-through and return/break/continue all go through
    // finNormalBB; exception exits go through finUnwindBB.
    // An exit-mode slot (i32) tracks which kind of exit happened so
    // finNormalBB's resume block can dispatch to the right successor.
    llvm::BasicBlock* outerUnwind =
        hasFin ? (unwindTarget_ ? unwindTarget_ : propagateBB()) : nullptr;
    auto* dispatchBB =
        llvm::BasicBlock::Create(llvm_, "catch.dispatch", currentFn_);
    auto* afterBB = llvm::BasicBlock::Create(llvm_, "try.after", currentFn_);
    llvm::BasicBlock* finNormalBB =
        hasFin ? llvm::BasicBlock::Create(llvm_, "finally", currentFn_)
               : nullptr;
    llvm::BasicBlock* finUnwindBB =
        hasFin ? llvm::BasicBlock::Create(llvm_, "finally.unwind",
                                          currentFn_)
               : nullptr;
    // Where a normal exit (body/arm fall-through) goes.
    llvm::BasicBlock* normalExit = hasFin ? finNormalBB : afterBB;

    // CG21c: per-try-with-finally exit mode + return value storage.
    llvm::AllocaInst* exitModSlot = nullptr;
    llvm::AllocaInst* retValSlot  = nullptr;
    llvm::Type* retTy = currentFn_ ? currentFn_->getReturnType() : nullptr;
    llvm::BasicBlock* loopBreakBB = loopStack_.empty()
        ? nullptr : loopStack_.back().breakTarget;
    llvm::BasicBlock* loopContBB  = loopStack_.empty()
        ? nullptr : loopStack_.back().continueTarget;
    if (hasFin) {
        exitModSlot = entryAlloca(i32());
        builder_.CreateStore(builder_.getInt32(0), exitModSlot);
        if (retTy && !retTy->isVoidTy())
            retValSlot = entryAlloca(retTy);
        finallyStack_.push_back({finNormalBB, exitModSlot, retValSlot,
                                 retTy, afterBB,
                                 loopBreakBB, loopContBB});
    }

    llvm::BasicBlock* savedUnwind = unwindTarget_;
    std::size_t savedDepth = unwindScopeDepth_;
    unwindTarget_ = dispatchBB;
    unwindScopeDepth_ = disposeStack_.size();
    bool bodyTerm = lowerScopedBlock(trys.body);
    unwindTarget_ = savedUnwind;
    unwindScopeDepth_ = savedDepth;
    if (!bodyTerm) builder_.CreateBr(normalExit);

    // Catch dispatch: test arms in order against the in-flight exception.
    builder_.SetInsertPoint(dispatchBB);
    llvm::Value* exc = builder_.CreateLoad(ptrTy(), excGlobal());
    for (const auto& arm : trys.catches) {
        auto* armBB = llvm::BasicBlock::Create(llvm_, "catch.arm",
                                               currentFn_);
        auto* nextBB = llvm::BasicBlock::Create(llvm_, "catch.next",
                                                currentFn_);
        llvm::Value* matched = builder_.getInt1(false);
        for (const auto& ty : arm.types) {
            if (!ty) continue;
            sema::TypeRef* rt = ctx_.resolvedType(*ty);
            llvm::GlobalVariable* desc =
                descriptorForTypeName(rt, ty.get());
            if (!desc) continue;
            matched = builder_.CreateOr(matched, emitIsA(exc, desc));
        }
        bool needsPrep =
            arm.guard || (arm.hasBinding && arm.body);
        if (needsPrep) {
            auto* prepBB = llvm::BasicBlock::Create(llvm_, "catch.prep",
                                                    currentFn_);
            builder_.CreateCondBr(matched, prepBB, nextBB);
            builder_.SetInsertPoint(prepBB);
            if (arm.hasBinding && arm.body) {
                llvm::AllocaInst* slot = entryAlloca(ptrTy());
                builder_.CreateStore(exc, slot);
                locals_[arm.body.get()] = slot;
            }
            if (arm.guard) {
                llvm::Value* g = lowerExpr(*arm.guard);
                if (!g) g = builder_.getInt1(false);
                builder_.CreateCondBr(g, armBB, nextBB);
            } else {
                builder_.CreateBr(armBB);
            }
        } else {
            builder_.CreateCondBr(matched, armBB, nextBB);
        }

        builder_.SetInsertPoint(armBB);
        builder_.CreateStore(
            llvm::ConstantPointerNull::get(ptrTy()), excGlobal());
        llvm::Value* savedCaught = currentCaughtExc_;
        std::string_view savedCaughtName = currentCaughtName_;
        currentCaughtExc_ = exc;
        // #387 / §31.1: the name (if any) this arm binds the caught exception
        // to, so a `throw <newExpr>` from the body can tell whether the new
        // exception's expression references the caught object. A bare-type arm
        // ("" name) can never reference it → such throws always free it.
        currentCaughtName_ = arm.hasBinding ? arm.varName : std::string_view{};
        // #383 / §31.1: ownership of the exception transfers to the runtime,
        // which disposes it after the matching catch arm completes. Register
        // the caught object as a `caughtException` disposal in a dedicated
        // scope around the arm body: a NORMAL handled exit (fall-through,
        // return, break, continue) frees it via the disposal machinery
        // (emitDisposeAndReclaim — destructor + base chain + free, #380); the
        // UNWIND path (a re-raise `throw;` or `throw expr` from the arm body)
        // SKIPS it (emitDisposals' isUnwind), keeping it in flight so the
        // enclosing handler frees it exactly once. The binding is a borrow
        // (§31.1) that may not escape; disposal runs after the body's last
        // use (return values are evaluated before disposal — §28.3).
        disposeStack_.emplace_back();
        {
            llvm::AllocaInst* excSlot = entryAlloca(ptrTy());
            builder_.CreateStore(exc, excSlot);
            Disposal d{excSlot, nullptr};
            d.caughtException = true;
            disposeStack_.back().push_back(d);
        }
        // An exception thrown by the arm body must still run finally.
        if (hasFin) { unwindTarget_ = finUnwindBB; unwindScopeDepth_ =
                          disposeStack_.size(); }
        bool armTerm = false;
        if (arm.body) armTerm = lowerStmt(*arm.body);
        if (hasFin) { unwindTarget_ = savedUnwind;
                      unwindScopeDepth_ = savedDepth; }
        // On a normal handled fall-through, dispose (free) the caught
        // exception via the scope, then continue to the normal exit. A
        // terminating arm body (return/break/continue) already freed it via
        // emitAllDisposals/emitLoopDisposals; a re-raise/throw skipped it.
        if (!armTerm) emitDisposals(disposeStack_.back());
        disposeStack_.pop_back();
        currentCaughtExc_ = savedCaught;
        currentCaughtName_ = savedCaughtName;
        if (!armTerm) builder_.CreateBr(normalExit);

        builder_.SetInsertPoint(nextBB);
    }
    // No arm matched → exception still pending; run finally (if any)
    // then re-propagate to the enclosing unwind target.
    builder_.CreateBr(hasFin ? finUnwindBB : unwindBB());

    if (hasFin) {
        // Pop the frame — outer frames (if any) are now at back().
        finallyStack_.pop_back();
        FinallyFrame* outerFr = finallyStack_.empty()
            ? nullptr : &finallyStack_.back();

        // finNormalBB: run finally body, then dispatch based on exit mode.
        builder_.SetInsertPoint(finNormalBB);
        bool finTerm = lowerScopedBlock(trys.finallyBody);
        auto* resumeBB = llvm::BasicBlock::Create(llvm_, "finally.resume",
                                                  currentFn_);
        if (!finTerm) builder_.CreateBr(resumeBB);

        // Build the dispatch block after finally.
        builder_.SetInsertPoint(resumeBB);
        llvm::Value* mode = builder_.CreateLoad(i32(), exitModSlot);
        // Default (mode=0): fall through to afterBB.
        auto* sw = builder_.CreateSwitch(mode, afterBB, 3);

        // mode=1: return — propagate through outer finally or emit ret.
        auto* retDispBB = llvm::BasicBlock::Create(llvm_,"finally.ret",currentFn_);
        sw->addCase(builder_.getInt32(1), retDispBB);
        builder_.SetInsertPoint(retDispBB);
        if (outerFr) {
            // Route through outer finally (nested try-with-finally).
            builder_.CreateStore(builder_.getInt32(1), outerFr->exitModSlot);
            if (retValSlot && outerFr->retValSlot) {
                llvm::Value* rv = builder_.CreateLoad(retTy, retValSlot);
                builder_.CreateStore(rv, outerFr->retValSlot);
            }
            builder_.CreateBr(outerFr->finNormalBB);
        } else {
            llvm::Value* rv = (retValSlot && retTy && !retTy->isVoidTy())
                ? builder_.CreateLoad(retTy, retValSlot) : nullptr;
            emitReturn(currentFn_, rv);
        }

        // mode=2: break — propagate through outer finally or jump to break.
        if (loopBreakBB) {
            auto* brkDispBB = llvm::BasicBlock::Create(llvm_,"finally.brk",currentFn_);
            sw->addCase(builder_.getInt32(2), brkDispBB);
            builder_.SetInsertPoint(brkDispBB);
            if (outerFr) {
                builder_.CreateStore(builder_.getInt32(2), outerFr->exitModSlot);
                builder_.CreateBr(outerFr->finNormalBB);
            } else {
                builder_.CreateBr(loopBreakBB);
            }
        }

        // mode=3: continue — propagate through outer finally or jump to cond.
        if (loopContBB) {
            auto* cntDispBB = llvm::BasicBlock::Create(llvm_,"finally.cnt",currentFn_);
            sw->addCase(builder_.getInt32(3), cntDispBB);
            builder_.SetInsertPoint(cntDispBB);
            if (outerFr) {
                builder_.CreateStore(builder_.getInt32(3), outerFr->exitModSlot);
                builder_.CreateBr(outerFr->finNormalBB);
            } else {
                builder_.CreateBr(loopContBB);
            }
        }

        // finUnwindBB: exception exit — run finally, then re-propagate.
        builder_.SetInsertPoint(finUnwindBB);
        if (!lowerScopedBlock(trys.finallyBody))
            builder_.CreateBr(outerUnwind);
    }

    builder_.SetInsertPoint(afterBB);
    return false;
}


bool Emitter::lowerIf(const ast::IfStmt& ifs) {
    llvm::BasicBlock* mergeBB = nullptr;
    auto merge = [&]() -> llvm::BasicBlock* {
        if (!mergeBB) mergeBB = llvm::BasicBlock::Create(
            llvm_, "if.end", currentFn_);
        return mergeBB;
    };

    // The if-branch and each elif form a (condition, body) test chain.
    std::vector<const ast::IfStmt::Branch*> chain;
    chain.push_back(&ifs.ifBranch);
    for (const auto& el : ifs.elifBranches) chain.push_back(&el);

    bool allPathsTerminate = true;
    for (const ast::IfStmt::Branch* br : chain) {
        llvm::Value* cond =
            br->condition ? lowerExpr(*br->condition) : nullptr;
        auto* bodyBB = llvm::BasicBlock::Create(llvm_, "if.body", currentFn_);
        auto* nextBB = llvm::BasicBlock::Create(llvm_, "if.next", currentFn_);
        if (cond) builder_.CreateCondBr(cond, bodyBB, nextBB);
        else builder_.CreateBr(nextBB);  // defensive

        builder_.SetInsertPoint(bodyBB);
        if (!lowerScopedBlock(br->body)) {
            allPathsTerminate = false;
            builder_.CreateBr(merge());
        }
        builder_.SetInsertPoint(nextBB);  // next test / else goes here
    }

    // The final fall-through block is the else (or an empty pass-through).
    if (ifs.hasElse) {
        if (!lowerScopedBlock(ifs.elseBody)) {
            allPathsTerminate = false;
            builder_.CreateBr(merge());
        }
    } else {
        allPathsTerminate = false;
        builder_.CreateBr(merge());
    }

    if (mergeBB) {
        builder_.SetInsertPoint(mergeBB);
        return false;
    }
    return allPathsTerminate;
}


bool Emitter::lowerWhile(const ast::WhileStmt& ws) {
    auto* condBB = llvm::BasicBlock::Create(llvm_, "while.cond", currentFn_);
    auto* bodyBB = llvm::BasicBlock::Create(llvm_, "while.body", currentFn_);
    auto* endBB = llvm::BasicBlock::Create(llvm_, "while.end", currentFn_);

    builder_.CreateBr(condBB);
    builder_.SetInsertPoint(condBB);
    llvm::Value* cond = ws.condition ? lowerExpr(*ws.condition) : nullptr;
    if (cond) builder_.CreateCondBr(cond, bodyBB, endBB);
    else builder_.CreateBr(endBB);

    builder_.SetInsertPoint(bodyBB);
    // CG13: the loop body is a disposal scope; break/continue unwind
    // down to this depth (the scope index the body will occupy).
    loopStack_.push_back({condBB, endBB, disposeStack_.size()});
    bool bodyTerminated = lowerScopedBlock(ws.body);
    loopStack_.pop_back();
    if (!bodyTerminated) builder_.CreateBr(condBB);  // back-edge

    builder_.SetInsertPoint(endBB);
    return false;  // control may always exit the loop
}


bool Emitter::lowerRangeFor(const ast::ForStmt& fs) {
    auto* range = dynamic_cast<const ast::RangeExpr*>(fs.iterable.get());
    if (!range || !range->lo || !range->hi) return false;
    llvm::Value* lo = lowerExpr(*range->lo);
    llvm::Value* hi = lowerExpr(*range->hi);
    if (!lo || !hi) return false;

    // §16.4: resolve the step (default = 1; negative steps descend).
    llvm::Value* stepVal = llvm::ConstantInt::get(lo->getType(), 1);
    bool descending = false;
    if (range->step) {
        llvm::Value* s = lowerExpr(*range->step);
        if (!s) return false;
        auto* cs = llvm::dyn_cast<llvm::ConstantInt>(s);
        if (!cs || cs->isZero()) return false;  // non-const / zero: defer
        stepVal = cs;
        descending = cs->getValue().isNegative();
    }

    llvm::AllocaInst* slot = entryAlloca(lo->getType());
    // §29.3: in an iterator method, map the loop variable to a struct field.
    if (iterStateStruct_ && iterSelf_) {
        auto fit = iterLocalFields_.find(&fs);
        if (fit != iterLocalFields_.end())
            iterAllocaToField_[slot] = fit->second;
    }
    builder_.CreateStore(lo, slot);
    // Anchor on the ForStmt node — sema gave `lsym->decl = fs` so that
    // identifier `i` in the body resolves through `locals_[fs]`.
    locals_[&fs] = slot;

    // §29.3: in an iterator method, re-evaluate `hi` inside `condBB`
    // rather than pre-computing it into an alloca. Pre-computed allocas
    // are uninitialized in resumed states (state≥1), causing the upper
    // bound to be garbage. Re-evaluating in condBB is correct because
    // all params/locals it depends on ARE restored from the state struct.
    if (iterStateStruct_) {
        auto* condBB = llvm::BasicBlock::Create(llvm_, "for.cond", currentFn_);
        auto* bodyBB = llvm::BasicBlock::Create(llvm_, "for.body", currentFn_);
        auto* incBB  = llvm::BasicBlock::Create(llvm_, "for.inc",  currentFn_);
        auto* endBB  = llvm::BasicBlock::Create(llvm_, "for.end",  currentFn_);
        builder_.CreateBr(condBB);
        builder_.SetInsertPoint(condBB);
        llvm::Value* cur = builder_.CreateLoad(slot->getAllocatedType(), slot);
        // Re-evaluate hi here so it picks up any saved/restored values.
        llvm::Value* hiVal = lowerExpr(*range->hi);
        if (!hiVal) hiVal = hi;  // fallback to pre-computed value
        llvm::Value* cond;
        if (descending) {
            cond = range->inclusive
                       ? builder_.CreateICmpSGE(cur, hiVal)
                       : builder_.CreateICmpSGT(cur, hiVal);
        } else {
            cond = range->inclusive
                       ? builder_.CreateICmpSLE(cur, hiVal)
                       : builder_.CreateICmpSLT(cur, hiVal);
        }
        builder_.CreateCondBr(cond, bodyBB, endBB);
        builder_.SetInsertPoint(bodyBB);
        loopStack_.push_back({incBB, endBB, disposeStack_.size()});
        bool term = lowerScopedBlock(fs.body);
        loopStack_.pop_back();
        if (!term) builder_.CreateBr(incBB);
        builder_.SetInsertPoint(incBB);
        llvm::Value* v = builder_.CreateLoad(slot->getAllocatedType(), slot);
        llvm::Value* next = builder_.CreateAdd(v, stepVal);
        builder_.CreateStore(next, slot);
        builder_.CreateBr(condBB);
        builder_.SetInsertPoint(endBB);
        return true;
    }

    auto* condBB = llvm::BasicBlock::Create(llvm_, "for.cond", currentFn_);
    auto* bodyBB = llvm::BasicBlock::Create(llvm_, "for.body", currentFn_);
    auto* incBB  = llvm::BasicBlock::Create(llvm_, "for.inc",  currentFn_);
    auto* endBB  = llvm::BasicBlock::Create(llvm_, "for.end",  currentFn_);
    builder_.CreateBr(condBB);

    // cond: direction-aware bound check.
    // Ascending: i <= hi (inclusive `..`) or i < hi (half-open `..<`).
    // Descending: i >= hi (inclusive `..`) or i > hi (half-open `..<`).
    builder_.SetInsertPoint(condBB);
    llvm::Value* cur = builder_.CreateLoad(slot->getAllocatedType(), slot);
    llvm::Value* cond;
    if (descending) {
        cond = range->inclusive
                   ? builder_.CreateICmpSGE(cur, hi)
                   : builder_.CreateICmpSGT(cur, hi);
    } else {
        cond = range->inclusive
                   ? builder_.CreateICmpSLE(cur, hi)
                   : builder_.CreateICmpSLT(cur, hi);
    }
    builder_.CreateCondBr(cond, bodyBB, endBB);

    // body: `continue` targets incBB, `break` targets endBB.
    builder_.SetInsertPoint(bodyBB);
    loopStack_.push_back({incBB, endBB, disposeStack_.size()});
    bool term = lowerScopedBlock(fs.body);
    loopStack_.pop_back();
    if (!term) builder_.CreateBr(incBB);

    // inc: i := i + step.
    builder_.SetInsertPoint(incBB);
    llvm::Value* v = builder_.CreateLoad(slot->getAllocatedType(), slot);
    llvm::Value* next = builder_.CreateAdd(v, stepVal);
    builder_.CreateStore(next, slot);
    builder_.CreateBr(condBB);

    builder_.SetInsertPoint(endBB);
    return true;
}


bool Emitter::lowerArrayFor(const ast::ForStmt& fs) {
    if (!fs.iterable) return false;
    // Handle collection literal `for var x in [e1, e2, ...] do`:
    // Lower each element into a temp alloca and iterate.
    if (auto* cl = dynamic_cast<const ast::CollectionLiteralExpr*>(
            fs.iterable.get())) {
        if (!cl->isDict && !cl->elements.empty()) {
            // Determine element type from first element.
            llvm::Value* first = lowerExpr(*cl->elements[0]);
            if (!first) return false;
            llvm::Type* elemTy = first->getType();
            std::size_t N = cl->elements.size();
            auto* at = llvm::ArrayType::get(elemTy, N);
            auto* tmp = entryAlloca(at);
            // Fill the temp array.
            builder_.CreateStore(first,
                builder_.CreateGEP(at, tmp, {builder_.getInt32(0), builder_.getInt32(0)}));
            for (std::size_t i = 1; i < N; ++i) {
                llvm::Value* ev = lowerExpr(*cl->elements[i]);
                if (!ev) continue;
                builder_.CreateStore(ev,
                    builder_.CreateGEP(at, tmp, {builder_.getInt32(0), builder_.getInt32(i)}));
            }
            // Now iterate as a normal array.
            uint64_t n = N;
            auto* idxSlot2 = entryAlloca(i32());
            builder_.CreateStore(builder_.getInt32(0), idxSlot2);
            auto* varSlot2 = entryAlloca(elemTy);
            locals_[&fs] = varSlot2;
            auto* condBB2 = llvm::BasicBlock::Create(llvm_, "clit.cond", currentFn_);
            auto* bodyBB2 = llvm::BasicBlock::Create(llvm_, "clit.body", currentFn_);
            auto* endBB2 = llvm::BasicBlock::Create(llvm_, "clit.end", currentFn_);
            builder_.CreateBr(condBB2);
            builder_.SetInsertPoint(condBB2);
            llvm::Value* idx2 = builder_.CreateLoad(i32(), idxSlot2);
            builder_.CreateCondBr(
                builder_.CreateICmpSLT(idx2, builder_.getInt32((uint32_t)n)),
                bodyBB2, endBB2);
            builder_.SetInsertPoint(bodyBB2);
            llvm::Value* elem2 = builder_.CreateLoad(elemTy,
                builder_.CreateGEP(at, tmp,
                    {builder_.getInt32(0), idx2}));
            builder_.CreateStore(elem2, varSlot2);
            lowerScopedBlock(fs.body);
            llvm::Value* inc2 = builder_.CreateAdd(
                builder_.CreateLoad(i32(), idxSlot2), builder_.getInt32(1));
            builder_.CreateStore(inc2, idxSlot2);
            builder_.CreateBr(condBB2);
            builder_.SetInsertPoint(endBB2);
            return true;
        }
    }
    // §3: for var x in array (runtime Array<T> object).
    {
        auto* iterTy = ctx_.exprType(*fs.iterable);
        if (isCollectionTypeName(iterTy)) {
            llvm::Value* arrObj = lowerExpr(*fs.iterable);
            if (!arrObj) return false;
            // Determine element type from generic arg.
            llvm::Type* elemTy = i32();
            int64_t esz = 8;
            if (iterTy && !iterTy->genericArgs.empty() && iterTy->genericArgs[0]) {
                elemTy = lowerTypeRef(iterTy->genericArgs[0]);
                esz = collectionElemSize(iterTy);
            }
            auto* lenFn = declareC("bz_array_len", i64(), {ptrTy()});
            auto* ptrAtFn = declareC("bz_array_ptr_at", ptrTy(), {ptrTy(), i64(), i64()});
            llvm::Value* len = builder_.CreateCall(lenFn, {arrObj});
            auto* idxSlotR = entryAlloca(i64());
            builder_.CreateStore(builder_.getInt64(0), idxSlotR);
            auto* varSlotR = entryAlloca(elemTy);
            locals_[&fs] = varSlotR;
            auto* condBBR = llvm::BasicBlock::Create(llvm_, "rt.cond", currentFn_);
            auto* bodyBBR = llvm::BasicBlock::Create(llvm_, "rt.body", currentFn_);
            auto* incBBR  = llvm::BasicBlock::Create(llvm_, "rt.inc",  currentFn_);
            auto* endBBR  = llvm::BasicBlock::Create(llvm_, "rt.end",  currentFn_);
            builder_.CreateBr(condBBR);
            builder_.SetInsertPoint(condBBR);
            llvm::Value* idxR = builder_.CreateLoad(i64(), idxSlotR);
            builder_.CreateCondBr(builder_.CreateICmpSLT(idxR, len), bodyBBR, endBBR);
            builder_.SetInsertPoint(bodyBBR);
            llvm::Value* idxR2 = builder_.CreateLoad(i64(), idxSlotR);
            llvm::Value* ep = builder_.CreateCall(ptrAtFn,
                {arrObj, idxR2, builder_.getInt64(esz)});
            llvm::Value* elem = builder_.CreateLoad(elemTy, ep);
            builder_.CreateStore(elem, varSlotR);
            loopStack_.push_back({incBBR, endBBR, disposeStack_.size()});
            lowerScopedBlock(fs.body);
            loopStack_.pop_back();
            llvm::Value* idxR3 = builder_.CreateLoad(i64(), idxSlotR);
            builder_.CreateStore(builder_.CreateAdd(idxR3, builder_.getInt64(1)), idxSlotR);
            builder_.CreateBr(condBBR);
            builder_.SetInsertPoint(endBBR);
            return true;
        }
    }
    // Try the lvalue path first (local/field/parameter — no spill needed).
    llvm::Type* arrSlotTy = nullptr;
    llvm::Value* arrAddr = lvalueAddr(*fs.iterable, arrSlotTy);
    // If the iterable is an expression (e.g. a method call returning [N]T),
    // evaluate it and spill the array value to a temp alloca so we have an
    // address to GEP through.
    if (!arrAddr || !arrSlotTy) {
        // Check exprType to see if the iterable is an InlineArray.
        auto* tt = ctx_.exprType(*fs.iterable);
        if (!tt || tt->kind != sema::TypeRef::Kind::InlineArray) return false;
        llvm::Value* v = lowerExpr(*fs.iterable);
        if (!v) return false;
        auto* at = llvm::dyn_cast<llvm::ArrayType>(v->getType());
        if (!at) return false;
        llvm::AllocaInst* tmp = entryAlloca(at);
        builder_.CreateStore(v, tmp);
        arrAddr = tmp;
        arrSlotTy = at;
    }
    auto* arrTy = llvm::dyn_cast<llvm::ArrayType>(arrSlotTy);
    if (!arrTy) return false;
    uint64_t N = arrTy->getNumElements();
    if (N == 0) return true;  // empty array — no iterations
    llvm::Type* elemTy = arrTy->getElementType();

    // Index counter (i32).
    auto* idxSlot = entryAlloca(i32());
    builder_.CreateStore(builder_.getInt32(0), idxSlot);

    // Element value slot — what the body sees as `x`.
    auto* varSlot = entryAlloca(elemTy);
    locals_[&fs] = varSlot;

    auto* condBB = llvm::BasicBlock::Create(llvm_, "far.cond", currentFn_);
    auto* bodyBB = llvm::BasicBlock::Create(llvm_, "far.body", currentFn_);
    auto* incBB  = llvm::BasicBlock::Create(llvm_, "far.inc",  currentFn_);
    auto* endBB  = llvm::BasicBlock::Create(llvm_, "far.end",  currentFn_);
    builder_.CreateBr(condBB);

    // cond: i < N.
    builder_.SetInsertPoint(condBB);
    llvm::Value* idx = builder_.CreateLoad(i32(), idxSlot);
    llvm::Value* cond = builder_.CreateICmpSLT(
        idx, builder_.getInt32(static_cast<uint32_t>(N)));
    builder_.CreateCondBr(cond, bodyBB, endBB);

    // body: load a[i] into varSlot so the body sees x = a[i].
    builder_.SetInsertPoint(bodyBB);
    llvm::Value* cur = builder_.CreateLoad(i32(), idxSlot);
    llvm::Value* elemAddr = builder_.CreateGEP(
        arrTy, arrAddr, {builder_.getInt64(0), cur});
    llvm::Value* elem = builder_.CreateLoad(elemTy, elemAddr);
    builder_.CreateStore(elem, varSlot);

    loopStack_.push_back({incBB, endBB, disposeStack_.size()});
    bool term = lowerScopedBlock(fs.body);
    loopStack_.pop_back();
    if (!term) builder_.CreateBr(incBB);

    // inc: i := i + 1.
    builder_.SetInsertPoint(incBB);
    llvm::Value* v = builder_.CreateLoad(i32(), idxSlot);
    builder_.CreateStore(
        builder_.CreateAdd(v, builder_.getInt32(1)), idxSlot);
    builder_.CreateBr(condBB);

    builder_.SetInsertPoint(endBB);
    return true;
}


// issue #370 / §4.8 / §4.10: emit a counted loop over a snapshot of a generic
// hash table's keys. bz_dict_keys_gen returns a malloc'd array of `n` pointers,
// each a malloc'd copy of one key's raw bytes; we load each key with the
// element type's LLVM type and bind it to the loop variable, then free the copy
// and the array. The snapshot decouples iteration from the live table (a body
// that mutates the set/dict does not invalidate the loop), matching the
// existing bz_dict_keys_str-based behavior for string keys.
bool Emitter::emitGenericKeyTableFor(const ast::ForStmt& fs,
                                     llvm::Value* tableObj,
                                     const sema::TypeRef* elemTyRef) {
    if (!tableObj || !elemTyRef) return false;
    llvm::Type* elemTy = lowerTypeRef(elemTyRef);
    if (!elemTy) return false;

    auto* keysOutSlot = entryAlloca(ptrTy());
    auto* cntSlot     = entryAlloca(i64());
    auto* keysFn = declareC("bz_dict_keys_gen", llvm::Type::getVoidTy(llvm_),
                            {ptrTy(), ptrTy(), ptrTy()});
    builder_.CreateCall(keysFn, {tableObj, keysOutSlot, cntSlot});
    llvm::Value* keysArr = builder_.CreateLoad(ptrTy(), keysOutSlot);  // void**
    llvm::Value* cnt     = builder_.CreateLoad(i64(), cntSlot);
    auto* freeFn = declareC("free", llvm::Type::getVoidTy(llvm_), {ptrTy()});

    auto* idxSlot = entryAlloca(i64());
    builder_.CreateStore(builder_.getInt64(0), idxSlot);
    auto* varSlot = entryAlloca(elemTy);
    locals_[&fs] = varSlot;

    auto* condBB = llvm::BasicBlock::Create(llvm_, "gk.cond", currentFn_);
    auto* bodyBB = llvm::BasicBlock::Create(llvm_, "gk.body", currentFn_);
    auto* incBB  = llvm::BasicBlock::Create(llvm_, "gk.inc",  currentFn_);
    auto* endBB  = llvm::BasicBlock::Create(llvm_, "gk.end",  currentFn_);
    builder_.CreateBr(condBB);

    builder_.SetInsertPoint(condBB);
    llvm::Value* idx = builder_.CreateLoad(i64(), idxSlot);
    builder_.CreateCondBr(builder_.CreateICmpSLT(idx, cnt), bodyBB, endBB);

    builder_.SetInsertPoint(bodyBB);
    llvm::Value* idxB = builder_.CreateLoad(i64(), idxSlot);
    // keysArr[idxB] is a void* to the key bytes; load the key with elemTy.
    llvm::Value* keyPtrPtr = builder_.CreateGEP(ptrTy(), keysArr, idxB);
    llvm::Value* keyPtr    = builder_.CreateLoad(ptrTy(), keyPtrPtr);
    llvm::Value* keyVal    = builder_.CreateLoad(elemTy, keyPtr);
    builder_.CreateStore(keyVal, varSlot);
    loopStack_.push_back({incBB, endBB, disposeStack_.size()});
    bool term = lowerScopedBlock(fs.body);
    loopStack_.pop_back();
    if (!term) builder_.CreateBr(incBB);

    builder_.SetInsertPoint(incBB);
    llvm::Value* idxI = builder_.CreateLoad(i64(), idxSlot);
    // Free this iteration's key-bytes copy before advancing.
    llvm::Value* keyPtrPtr2 = builder_.CreateGEP(ptrTy(), keysArr, idxI);
    llvm::Value* keyPtr2    = builder_.CreateLoad(ptrTy(), keyPtrPtr2);
    builder_.CreateCall(freeFn, {keyPtr2});
    builder_.CreateStore(builder_.CreateAdd(idxI, builder_.getInt64(1)), idxSlot);
    builder_.CreateBr(condBB);

    builder_.SetInsertPoint(endBB);
    builder_.CreateCall(freeFn, {keysArr});  // free the snapshot array itself
    return true;
}


// issue #370 / §4.10: `for var x in <set>` over a dict-backed Set<T> /
// OwnedSet<T>. The set's element routes through the generic hash table (records,
// primitives/enums, AND strings — see lowerNew / genericBackingType), so the
// array-backed for-loop (bz_array_*) would read garbage. Enumerate the table's
// keys (= the set's elements) instead.
bool Emitter::lowerDictBackedSetFor(const ast::ForStmt& fs) {
    if (!fs.iterable) return false;
    auto* iterTy = ctx_.exprType(*fs.iterable);
    if (!iterTy || !iterTy->astNode || !iterTy->astNode->name ||
        iterTy->astNode->name->parts.empty() || iterTy->declaration)
        return false;
    auto n = iterTy->astNode->name->parts[0];
    if (n != "Set" && n != "OwnedSet") return false;
    const sema::TypeRef* elemTy =
        genericBackingType(iterTy, 0, /*isSet=*/true, nullptr);
    if (!elemTy) return false;  // not dict-backed → let lowerArrayFor handle it
    llvm::Value* setObj = lowerExpr(*fs.iterable);
    if (!setObj) return false;
    return emitGenericKeyTableFor(fs, setObj, elemTy);
}


// issue #370 / §4.8: `for var k in <dict>.Keys` — iterate a Dictionary's keys.
// `<dict>.Keys` carries no sema element type, so this recognizes the AST shape
// (a MemberAccessExpr named "Keys" over a Dictionary-typed target) directly. A
// string-key dict enumerates via bz_dict_keys_str (the string fast path); a
// non-string (generic-table) key dict enumerates via bz_dict_keys_gen.
bool Emitter::lowerDictKeysFor(const ast::ForStmt& fs) {
    if (!fs.iterable) return false;
    auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(fs.iterable.get());
    if (!ma || !ma->target || ma->member != "Keys") return false;
    auto* dictTy = ctx_.exprType(*ma->target);
    if (!isDictTypeName(dictTy)) return false;
    if (dictTy->genericArgs.empty() || !dictTy->genericArgs[0]) return false;
    const sema::TypeRef* keyTy = dictTy->genericArgs[0];

    llvm::Value* dictObj = lowerExpr(*ma->target);
    if (!dictObj) return false;

    // Non-string keys live in the generic table — reuse the shared enumerator.
    if (const sema::TypeRef* genKey =
            genericBackingType(dictTy, 0, /*isSet=*/false, nullptr))
        return emitGenericKeyTableFor(fs, dictObj, genKey);

    // String keys: the dedicated string-key table. bz_dict_keys_str returns
    // parallel (char* data, int64 len) arrays; rebuild a string object per key
    // and bind it to the loop variable (a ptr). Mirrors the Dictionary.Keys
    // string-array construction in lowerExpr.
    using K = sema::TypeRef::Kind;
    bool isString = keyTy->kind == K::Primitive &&
                    keyTy->primitive == sema::PrimitiveType::String;
    if (!isString) return false;  // other shapes not wired

    auto* keysOutSlot = entryAlloca(ptrTy());
    auto* lensOutSlot = entryAlloca(ptrTy());
    auto* cntSlot     = entryAlloca(i64());
    auto* keysFn = declareC("bz_dict_keys_str", llvm::Type::getVoidTy(llvm_),
                            {ptrTy(), ptrTy(), ptrTy(), ptrTy()});
    builder_.CreateCall(keysFn, {dictObj, keysOutSlot, lensOutSlot, cntSlot});
    llvm::Value* keysArr = builder_.CreateLoad(ptrTy(), keysOutSlot);  // char**
    llvm::Value* lensArr = builder_.CreateLoad(ptrTy(), lensOutSlot);  // i64*
    llvm::Value* cnt     = builder_.CreateLoad(i64(), cntSlot);
    auto* mallocFn = declareC("malloc", ptrTy(), {i64()});
    auto* memcpyFn = declareC("memcpy", ptrTy(), {ptrTy(), ptrTy(), i64()});
    auto* callocFn = declareC("calloc", ptrTy(), {i64(), i64()});
    auto* freeFn   = declareC("free", llvm::Type::getVoidTy(llvm_), {ptrTy()});
    auto* i8Ty = llvm::Type::getInt8Ty(llvm_);
    uint64_t strSz = module_->getDataLayout().getTypeAllocSize(stringType());

    auto* idxSlot = entryAlloca(i64());
    builder_.CreateStore(builder_.getInt64(0), idxSlot);
    auto* varSlot = entryAlloca(ptrTy());  // loop var is a string-object pointer
    locals_[&fs] = varSlot;

    auto* condBB = llvm::BasicBlock::Create(llvm_, "dk.cond", currentFn_);
    auto* bodyBB = llvm::BasicBlock::Create(llvm_, "dk.body", currentFn_);
    auto* incBB  = llvm::BasicBlock::Create(llvm_, "dk.inc",  currentFn_);
    auto* endBB  = llvm::BasicBlock::Create(llvm_, "dk.end",  currentFn_);
    builder_.CreateBr(condBB);

    builder_.SetInsertPoint(condBB);
    llvm::Value* idx = builder_.CreateLoad(i64(), idxSlot);
    builder_.CreateCondBr(builder_.CreateICmpSLT(idx, cnt), bodyBB, endBB);

    builder_.SetInsertPoint(bodyBB);
    llvm::Value* idxB = builder_.CreateLoad(i64(), idxSlot);
    llvm::Value* kpPtr = builder_.CreateGEP(ptrTy(), keysArr, idxB);
    llvm::Value* kp    = builder_.CreateLoad(ptrTy(), kpPtr);     // char*
    llvm::Value* lpPtr = builder_.CreateGEP(i64(), lensArr, idxB);
    llvm::Value* kl    = builder_.CreateLoad(i64(), lpPtr);       // i64 len
    llvm::Value* dataSz  = builder_.CreateAdd(kl, builder_.getInt64(1));
    llvm::Value* dataCpy = builder_.CreateCall(mallocFn, {dataSz});
    builder_.CreateCall(memcpyFn, {dataCpy, kp, kl});
    llvm::Value* nulPtr = builder_.CreateGEP(i8Ty, dataCpy, kl);
    builder_.CreateStore(llvm::ConstantInt::get(i8Ty, 0), nulPtr);
    llvm::Value* strObj = builder_.CreateCall(callocFn,
        {builder_.getInt64(1), builder_.getInt64((int64_t)strSz)});
    builder_.CreateStore(dataCpy, builder_.CreateStructGEP(stringType(), strObj, 0));
    builder_.CreateStore(kl, builder_.CreateStructGEP(stringType(), strObj, 1));
    builder_.CreateStore(llvm::ConstantInt::get(i8Ty, 1),
        builder_.CreateStructGEP(stringType(), strObj, 2));
    builder_.CreateStore(strObj, varSlot);
    loopStack_.push_back({incBB, endBB, disposeStack_.size()});
    bool term = lowerScopedBlock(fs.body);
    loopStack_.pop_back();
    if (!term) builder_.CreateBr(incBB);

    builder_.SetInsertPoint(incBB);
    llvm::Value* idxI = builder_.CreateLoad(i64(), idxSlot);
    // Free this iteration's strndup'd key bytes from bz_dict_keys_str.
    llvm::Value* kpPtr2 = builder_.CreateGEP(ptrTy(), keysArr, idxI);
    llvm::Value* kp2    = builder_.CreateLoad(ptrTy(), kpPtr2);
    builder_.CreateCall(freeFn, {kp2});
    builder_.CreateStore(builder_.CreateAdd(idxI, builder_.getInt64(1)), idxSlot);
    builder_.CreateBr(condBB);

    builder_.SetInsertPoint(endBB);
    builder_.CreateCall(freeFn, {keysArr});
    builder_.CreateCall(freeFn, {lensArr});
    return true;
}


bool Emitter::lowerStringFor(const ast::ForStmt& fs) {
    if (!fs.iterable) return false;
    auto* tt = ctx_.exprType(*fs.iterable);
    if (!tt || tt->kind != sema::TypeRef::Kind::Primitive ||
        tt->primitive != sema::PrimitiveType::String)
        return false;

    llvm::Value* strPtr = lowerExpr(*fs.iterable);
    if (!strPtr) return false;
    llvm::Value* len = loadStringLen(strPtr);          // i64 code-point count

    // Index counter (i64).
    auto* idxSlot = entryAlloca(i64());
    builder_.CreateStore(builder_.getInt64(0), idxSlot);

    // Loop variable slot — the body sees `c` as the current char (i32).
    auto* varSlot = entryAlloca(i32());
    locals_[&fs] = varSlot;

    auto* condBB = llvm::BasicBlock::Create(llvm_, "sfor.cond", currentFn_);
    auto* bodyBB = llvm::BasicBlock::Create(llvm_, "sfor.body", currentFn_);
    auto* incBB  = llvm::BasicBlock::Create(llvm_, "sfor.inc",  currentFn_);
    auto* endBB  = llvm::BasicBlock::Create(llvm_, "sfor.end",  currentFn_);
    builder_.CreateBr(condBB);

    builder_.SetInsertPoint(condBB);
    llvm::Value* idx = builder_.CreateLoad(i64(), idxSlot);
    llvm::Value* cond = builder_.CreateICmpSLT(idx, len, "sfor.cond");
    builder_.CreateCondBr(cond, bodyBB, endBB);

    builder_.SetInsertPoint(bodyBB);
    llvm::Value* idxB = builder_.CreateLoad(i64(), idxSlot);
    builder_.CreateStore(loadCodePoint(strPtr, idxB), varSlot);
    loopStack_.push_back({incBB, endBB, disposeStack_.size()});
    lowerScopedBlock(fs.body);
    loopStack_.pop_back();
    if (!builder_.GetInsertBlock()->getTerminator())
        builder_.CreateBr(incBB);

    builder_.SetInsertPoint(incBB);
    llvm::Value* idxInc = builder_.CreateLoad(i64(), idxSlot);
    builder_.CreateStore(builder_.CreateAdd(idxInc, builder_.getInt64(1)), idxSlot);
    builder_.CreateBr(condBB);

    builder_.SetInsertPoint(endBB);
    return true;
}


bool Emitter::lowerIEnumerableBuiltinFor(const ast::ForStmt& fs) {
    if (!fs.iterable) return false;
    auto* iterTy = ctx_.exprType(*fs.iterable);
    if (!iterTy || iterTy->kind != sema::TypeRef::Kind::Named ||
        iterTy->declaration || !iterTy->astNode || !iterTy->astNode->name ||
        iterTy->astNode->name->parts.empty())
        return false;
    // Only handle the iteration-interface family.
    auto n = iterTy->astNode->name->parts[0];
    if (n != "IEnumerable" && n != "IReadOnlyCollection" && n != "IReadOnlyList" &&
        n != "ICollection" && n != "IList" &&
        n != "OwnedEnumerable")
        return false;
    // Must have a generic arg to determine element type.
    if (iterTy->genericArgs.empty() || !iterTy->genericArgs[0]) return false;

    llvm::Value* enumObj = lowerExpr(*fs.iterable);
    if (!enumObj) return false;

    llvm::Type* elemTy = lowerTypeRef(iterTy->genericArgs[0]);

    // Determine if the value is a bz_array_t (variable/param backed by Array)
    // or an iterator state machine (result of a call to an iterator method).
    //
    // Heuristic: if the iterable is a CallExpr → assume iterator (vtable-backed).
    // If it's an identifier (parameter/local) → assume array-backed (bz_array_t*).
    // If it's a local variable declared as Array<T> → definitely array.
    //
    // This heuristic is correct for:
    //   for var x in Odds(10) do   ← CallExpr → vtable
    //   for var x in items do       ← identifier → array
    // Determine protocol based on expression kind:
    // - CallExpr → vtable dispatch (iterator state machine result)
    // - NewExpr → vtable dispatch (user class implementing IEnumerable)
    // - IdentifierExpr typed as concrete user class → vtable dispatch
    // - Everything else → array protocol (assumes bz_array_t*)
    //
    // Limitation: `for x in items` where items: IEnumerable<T> and the actual
    // value is a user class (not Array<T>) does not work correctly. Full
    // IEnumerable<T> interface dispatch requires vtable registration of
    // GetEnumerator which is not yet implemented for builtin interfaces.
    bool isArrayBacked = true;  // default: array protocol
    if (dynamic_cast<const ast::CallExpr*>(fs.iterable.get())) {
        isArrayBacked = false;  // iterator method call → vtable
    } else if (dynamic_cast<const ast::NewExpr*>(fs.iterable.get())) {
        isArrayBacked = false;  // new C() → vtable
    } else if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(fs.iterable.get())) {
        sema::Symbol* sym = ctx_.exprSymbol(*id);
        // If typed as a concrete user class (not builtin IEnumerable), use vtable.
        auto checkNamed = [&](sema::TypeRef* rt) {
            return rt && rt->kind == sema::TypeRef::Kind::Named && rt->declaration;
        };
        if (auto* par = dynamic_cast<sema::ParameterSymbol*>(sym)) {
            if (par->declType) {
                auto* rt = ctx_.resolvedType(*par->declType);
                if (checkNamed(rt)) isArrayBacked = false;
            }
        } else if (auto* loc = dynamic_cast<sema::LocalSymbol*>(sym)) {
            if (checkNamed(loc->declType)) isArrayBacked = false;
        }
    }

    if (isArrayBacked) {
        // Use direct bz_array_t protocol.
        int64_t esz = 8;
        if (elemTy->isIntegerTy(32) || elemTy->isFloatTy()) esz = 4;
        else if (elemTy->isIntegerTy(1) || elemTy->isIntegerTy(8)) esz = 1;
        else if (elemTy->isIntegerTy(16)) esz = 2;
        auto* lenFn = declareC("bz_array_len", i64(), {ptrTy()});
        auto* ptrAtFn = declareC("bz_array_ptr_at", ptrTy(), {ptrTy(), i64(), i64()});
        llvm::Value* len = builder_.CreateCall(lenFn, {enumObj});
        auto* idxSlot = entryAlloca(i64());
        builder_.CreateStore(builder_.getInt64(0), idxSlot);
        auto* varSlot = entryAlloca(elemTy);
        locals_[&fs] = varSlot;
        auto* condBB = llvm::BasicBlock::Create(llvm_, "ib.cond", currentFn_);
        auto* bodyBB = llvm::BasicBlock::Create(llvm_, "ib.body", currentFn_);
        auto* incBB  = llvm::BasicBlock::Create(llvm_, "ib.inc",  currentFn_);
        auto* endBB  = llvm::BasicBlock::Create(llvm_, "ib.end",  currentFn_);
        builder_.CreateBr(condBB);
        builder_.SetInsertPoint(condBB);
        llvm::Value* idx = builder_.CreateLoad(i64(), idxSlot);
        builder_.CreateCondBr(builder_.CreateICmpSLT(idx, len), bodyBB, endBB);
        builder_.SetInsertPoint(bodyBB);
        llvm::Value* idx2 = builder_.CreateLoad(i64(), idxSlot);
        llvm::Value* ep = builder_.CreateCall(ptrAtFn, {enumObj, idx2, builder_.getInt64(esz)});
        llvm::Value* elem = builder_.CreateLoad(elemTy, ep);
        builder_.CreateStore(elem, varSlot);
        loopStack_.push_back({incBB, endBB, disposeStack_.size()});
        lowerScopedBlock(fs.body);
        loopStack_.pop_back();
        if (!builder_.GetInsertBlock()->getTerminator())
            builder_.CreateBr(incBB);
        builder_.SetInsertPoint(incBB);
        llvm::Value* oldIdx = builder_.CreateLoad(i64(), idxSlot);
        builder_.CreateStore(builder_.CreateAdd(oldIdx, builder_.getInt64(1)), idxSlot);
        builder_.CreateBr(condBB);
        builder_.SetInsertPoint(endBB);
        return true;
    }

    // Store the enumerator in a slot.
    auto* enumSlot = entryAlloca(ptrTy());
    builder_.CreateStore(enumObj, enumSlot);

    // Use vtable-based MoveNext (slot 2) and Current (slot 3).
    // Layout: [0]=td, [1]=Dispose, [2]=MoveNext, [3]=Current
    auto* moveNextTy = llvm::FunctionType::get(i1(), {ptrTy()}, false);
    auto* currentTy = llvm::FunctionType::get(elemTy, {ptrTy()}, false);

    auto* varSlot = entryAlloca(elemTy);
    locals_[&fs] = varSlot;

    auto* condBB = llvm::BasicBlock::Create(llvm_, "ib.cond", currentFn_);
    auto* bodyBB = llvm::BasicBlock::Create(llvm_, "ib.body", currentFn_);
    auto* endBB  = llvm::BasicBlock::Create(llvm_, "ib.end",  currentFn_);
    builder_.CreateBr(condBB);

    // cond: call vtable slot 2 (MoveNext)
    builder_.SetInsertPoint(condBB);
    llvm::Value* obj = builder_.CreateLoad(ptrTy(), enumSlot);
    llvm::Value* vtable = builder_.CreateLoad(ptrTy(), obj);
    llvm::Value* mnSlotPtr = builder_.CreateGEP(ptrTy(), vtable, {builder_.getInt32(2)});
    llvm::Value* mnFnPtr = builder_.CreateLoad(ptrTy(), mnSlotPtr);
    llvm::Value* moved = builder_.CreateCall(moveNextTy, mnFnPtr, {obj}, "ib.moved");
    builder_.CreateCondBr(moved, bodyBB, endBB);

    // body: call vtable slot 3 (Current)
    builder_.SetInsertPoint(bodyBB);
    llvm::Value* obj2 = builder_.CreateLoad(ptrTy(), enumSlot);
    llvm::Value* vtable2 = builder_.CreateLoad(ptrTy(), obj2);
    llvm::Value* curSlotPtr = builder_.CreateGEP(ptrTy(), vtable2, {builder_.getInt32(3)});
    llvm::Value* curFnPtr = builder_.CreateLoad(ptrTy(), curSlotPtr);
    llvm::Value* curVal = builder_.CreateCall(currentTy, curFnPtr, {obj2}, "ib.cur");
    builder_.CreateStore(curVal, varSlot);

    loopStack_.push_back({condBB, endBB, disposeStack_.size()});
    lowerScopedBlock(fs.body);
    loopStack_.pop_back();
    if (!builder_.GetInsertBlock()->getTerminator())
        builder_.CreateBr(condBB);
    builder_.SetInsertPoint(endBB);
    return true;
}


bool Emitter::lowerIEnumerableFor(const ast::ForStmt& fs) {
    if (!fs.iterable) return false;
    auto* iterTy = ctx_.exprType(*fs.iterable);
    if (!iterTy || iterTy->kind != sema::TypeRef::Kind::Named) return false;
    auto* ts = iterTy->declaration;
    if (!ts) return false;  // builtin/interface without concrete class — skip

    // Find GetEnumerator on ts (walks base types).
    sema::MethodSymbol* getEnumSym = findMethodByName(ts, "GetEnumerator");
    if (!getEnumSym) return false;

    // Find the enumerator class. GetEnumerator's return type is typically
    // the builtin `IEnumerator<T>` (null declaration). We need the concrete
    // class that implements it — search all compiled TypeSymbols for a class
    // whose base types include a name starting with "IEnumerator".
    auto* getEnumDecl = dynamic_cast<const ast::MethodDecl*>(getEnumSym->decl);
    if (!getEnumDecl || !getEnumDecl->returnType) return false;
    auto* enumTyRef = ctx_.resolvedType(*getEnumDecl->returnType);
    if (!enumTyRef) return false;

    // Try to get the concrete TypeSymbol from the return type's declaration.
    auto* enumTs = (enumTyRef->kind == sema::TypeRef::Kind::Named)
                   ? enumTyRef->declaration : nullptr;

    // If null (builtin IEnumerator<T>), search for a user class that
    // implements it by checking all compiled TypeSymbols.
    // If enumTs is null (builtin IEnumerator<T> interface), find the
    // concrete class structurally: any user class with both "MoveNext"
    // and ("Current" or a property) in its compiled methods.
    if (!enumTs) {
        for (const auto& sym : ctx_.symbols()) {
            auto* candidate = dynamic_cast<const sema::TypeSymbol*>(sym.get());
            if (!candidate || candidate->kind != ast::TypeDeclKind::Class) continue;
            if (candidate == ts) continue;  // skip the iterable itself
            bool hasMoveNext = false, hasCurrent = false;
            for (auto* m : candidate->members) {
                auto* ms = dynamic_cast<sema::MethodSymbol*>(m);
                if (!ms || !methodFns_.count(ms)) continue;
                if (ms->name == "MoveNext") hasMoveNext = true;
                if (ms->name == "Current") hasCurrent = true;
            }
            // Also check PropertySymbol for Current
            if (!hasCurrent) {
                for (auto* m : candidate->members) {
                    auto* ps = dynamic_cast<sema::PropertySymbol*>(m);
                    if (ps && ps->name == "Current") { hasCurrent = true; break; }
                }
            }
            if (hasMoveNext && hasCurrent) {
                enumTs = const_cast<sema::TypeSymbol*>(candidate);
                break;
            }
        }
    }
    if (!enumTs) return false;  // couldn't find concrete enumerator class

    // Find MoveNext on the enumerator type.
    sema::MethodSymbol* moveNextSym = findMethodByName(enumTs, "MoveNext");
    if (!moveNextSym) return false;

    // Find Current — may be a PropertySymbol (readonly property) or MethodSymbol.
    sema::MethodSymbol* currentSym = findMethodByName(enumTs, "Current");
    sema::PropertySymbol* currentPropSym = nullptr;
    if (!currentSym) {
        for (auto* m : enumTs->members) {
            auto* ps = dynamic_cast<sema::PropertySymbol*>(m);
            if (ps && ps->name == "Current") { currentPropSym = ps; break; }
        }
    }

    // Evaluate the iterable to get the source object pointer.
    llvm::Value* srcPtr = lowerExpr(*fs.iterable);
    if (!srcPtr) return false;

    // Call GetEnumerator() → enumerator object (ptr).
    std::vector<llvm::Value*> geArgs = {srcPtr};
    llvm::Value* enumPtr = emitMethodCall(getEnumSym, geArgs, false);
    if (!enumPtr) return false;

    // Store the enumerator in a slot so it can be disposed at loop exit.
    auto* enumSlot = entryAlloca(ptrTy());
    builder_.CreateStore(enumPtr, enumSlot);

    // Register enumerator for disposal via disposeStack_ so `break`,
    // early `return`, or normal loop exit all call Dispose().
    // We push a Disposal entry with the enumerator's TypeSymbol.
    if (!disposeStack_.empty() && disposerFns_.count(enumTs))
        disposeStack_.back().push_back({enumSlot, enumTs});

    // Determine the loop variable's element type.
    llvm::Type* elemTy = ptrTy();  // default: reference type
    if (currentSym) {
        auto* curDecl = dynamic_cast<const ast::MethodDecl*>(currentSym->decl);
        if (curDecl && curDecl->returnType) {
            if (auto* rt = ctx_.resolvedType(*curDecl->returnType))
                elemTy = lowerTypeRef(rt);
        }
    } else if (currentPropSym) {
        auto* pd = dynamic_cast<const ast::PropertyDecl*>(currentPropSym->decl);
        if (pd && pd->type) {
            if (auto* rt = ctx_.resolvedType(*pd->type))
                elemTy = lowerTypeRef(rt);
        }
    }
    // Fall back to first generic arg of enumerator's return type.
    if (elemTy == ptrTy() && !enumTyRef->genericArgs.empty() && enumTyRef->genericArgs[0]) {
        llvm::Type* ga = lowerTypeRef(enumTyRef->genericArgs[0]);
        if (ga != ptrTy()) elemTy = ga;
    }

    // Loop variable slot — the body sees `x` here.
    auto* varSlot = entryAlloca(elemTy);
    locals_[&fs] = varSlot;

    auto* condBB = llvm::BasicBlock::Create(llvm_, "ien.cond", currentFn_);
    auto* bodyBB = llvm::BasicBlock::Create(llvm_, "ien.body", currentFn_);
    auto* endBB  = llvm::BasicBlock::Create(llvm_, "ien.end",  currentFn_);
    builder_.CreateBr(condBB);

    // cond: __enum.MoveNext() → bool.
    builder_.SetInsertPoint(condBB);
    llvm::Value* enumObj = builder_.CreateLoad(ptrTy(), enumSlot);
    std::vector<llvm::Value*> mnArgs = {enumObj};
    llvm::Value* moved = emitMethodCall(moveNextSym, mnArgs, false);
    if (!moved) {
        // If MoveNext can't be resolved via vtable, fall back to false.
        moved = builder_.getFalse();
    }
    builder_.CreateCondBr(moved, bodyBB, endBB);

    // body: x := __enum.Current; then run the user body.
    builder_.SetInsertPoint(bodyBB);
    {
        llvm::Value* enumObj2 = builder_.CreateLoad(ptrTy(), enumSlot);
        llvm::Value* curVal = nullptr;
        if (currentSym) {
            // Method-form Current.
            std::vector<llvm::Value*> curArgs = {enumObj2};
            curVal = emitMethodCall(currentSym, curArgs, false);
        } else if (currentPropSym) {
            // Property-form Current — evaluate the getter with enumObj2 as this.
            auto* pd = dynamic_cast<const ast::PropertyDecl*>(currentPropSym->decl);
            if (pd) {
                // Save/restore currentThis_.
                llvm::Value* savedThis = currentThis_;
                currentThis_ = enumObj2;
                // Try expression getter (get => expr).
                if (const ast::Expr* getterExpr = computedGetterOf(currentPropSym)) {
                    curVal = lowerExpr(*getterExpr);
                }
                // Try block getter (get => do...end).
                else if (const ast::PropertyAccessor* acc = blockGetterOf(currentPropSym)) {
                    llvm::Function* getterFn = ensurePropertyBlockGetter(pd, acc);
                    if (getterFn) {
                        auto* fnTy = getterFn->getFunctionType();
                        curVal = builder_.CreateCall(fnTy, getterFn, {enumObj2});
                    }
                }
                currentThis_ = savedThis;
            }
        }
        if (curVal) {
            // Coerce to elemTy if sizes differ.
            if (curVal->getType() != elemTy && elemTy->isIntegerTy() &&
                curVal->getType()->isIntegerTy())
                curVal = builder_.CreateTrunc(curVal, elemTy);
            if (curVal->getType() == elemTy)
                builder_.CreateStore(curVal, varSlot);
        }
    }

    loopStack_.push_back({condBB, endBB, disposeStack_.size()});
    bool term = lowerScopedBlock(fs.body);
    loopStack_.pop_back();
    if (!term) builder_.CreateBr(condBB);

    builder_.SetInsertPoint(endBB);
    return true;
}


bool Emitter::lowerLazyLinqFor(const ast::ForStmt& fs) {
    if (!fs.iterable) return false;
    auto* iterTy = ctx_.exprType(*fs.iterable);
    // Only a lazy LINQ pipeline result (the structural sentinel) is handled
    // here; iterator state machines and Array<T> go through other paths.
    if (!isLazyEnumerableType(iterTy)) return false;

    llvm::Value* lazy = lowerExpr(*fs.iterable);
    if (!lazy) return false;
    llvm::Type* elemTy = lowerTypeRef(iterTy->genericArgs[0]);
    if (!elemTy) elemTy = i32();

    // Store the enumerator in a slot; iterate via its vtable.
    // Lazy enumerator vtable: [0]=td, [1]=Dispose, [2]=MoveNext(->i32),
    // [3]=Current(->void* element-bytes pointer).
    auto* enumSlot = entryAlloca(ptrTy());
    builder_.CreateStore(lazy, enumSlot);

    auto* moveNextTy = llvm::FunctionType::get(i32(), {ptrTy()}, false);
    auto* currentTy  = llvm::FunctionType::get(ptrTy(), {ptrTy()}, false);

    auto* varSlot = entryAlloca(elemTy);
    locals_[&fs] = varSlot;

    auto* condBB = llvm::BasicBlock::Create(llvm_, "lz.cond", currentFn_);
    auto* bodyBB = llvm::BasicBlock::Create(llvm_, "lz.body", currentFn_);
    auto* endBB  = llvm::BasicBlock::Create(llvm_, "lz.end",  currentFn_);
    builder_.CreateBr(condBB);

    // cond: MoveNext (vtable slot 2) → i32 (1 = advanced).
    builder_.SetInsertPoint(condBB);
    llvm::Value* obj = builder_.CreateLoad(ptrTy(), enumSlot);
    llvm::Value* vtable = builder_.CreateLoad(ptrTy(), obj);
    llvm::Value* mnSlot = builder_.CreateGEP(ptrTy(), vtable, {builder_.getInt32(2)});
    llvm::Value* mnFn = builder_.CreateLoad(ptrTy(), mnSlot);
    llvm::Value* moved = builder_.CreateCall(moveNextTy, mnFn, {obj}, "lz.moved");
    llvm::Value* movedB = builder_.CreateICmpNE(moved, builder_.getInt32(0));
    builder_.CreateCondBr(movedB, bodyBB, endBB);

    // body: Current (vtable slot 3) → void*; load the element value.
    builder_.SetInsertPoint(bodyBB);
    llvm::Value* obj2 = builder_.CreateLoad(ptrTy(), enumSlot);
    llvm::Value* vtable2 = builder_.CreateLoad(ptrTy(), obj2);
    llvm::Value* curSlot = builder_.CreateGEP(ptrTy(), vtable2, {builder_.getInt32(3)});
    llvm::Value* curFn = builder_.CreateLoad(ptrTy(), curSlot);
    llvm::Value* curPtr = builder_.CreateCall(currentTy, curFn, {obj2}, "lz.curp");
    llvm::Value* curVal = builder_.CreateLoad(elemTy, curPtr, "lz.cur");
    builder_.CreateStore(curVal, varSlot);

    loopStack_.push_back({condBB, endBB, disposeStack_.size()});
    lowerScopedBlock(fs.body);
    loopStack_.pop_back();
    if (!builder_.GetInsertBlock()->getTerminator())
        builder_.CreateBr(condBB);

    builder_.SetInsertPoint(endBB);
    // Dispose the enumerator chain (vtable slot 1).
    llvm::Value* dObj = builder_.CreateLoad(ptrTy(), enumSlot);
    llvm::Value* dVt = builder_.CreateLoad(ptrTy(), dObj);
    llvm::Value* dSlot = builder_.CreateGEP(ptrTy(), dVt, {builder_.getInt32(1)});
    llvm::Value* dFn = builder_.CreateLoad(ptrTy(), dSlot);
    auto* disposeTy = llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_),
                                              {ptrTy()}, false);
    builder_.CreateCall(disposeTy, dFn, {dObj});
    return true;
}


bool Emitter::lowerCaseStmt(const ast::CaseStmt& cs) {
    if (!cs.scrutinee) return false;  // scrutinee-less form deferred
    llvm::Value* scrut = lowerExpr(*cs.scrutinee);
    if (!scrut) return false;
    const sema::TypeSymbol* enumTy = enumTypeOf(*cs.scrutinee);
    bool isStr = isStringExprType(*cs.scrutinee);
    auto* endBB =
        llvm::BasicBlock::Create(llvm_, "casestmt.end", currentFn_);
    for (const auto& arm : cs.arms) {
        llvm::Value* pm = patternMatches(arm.pattern.get(), scrut, enumTy,
                                         isStr);
        if (!pm) {  // deferred pattern kind — degrade to no-op
            builder_.CreateBr(endBB);
            builder_.SetInsertPoint(endBB);
            return false;
        }
        auto* bodyBB =
            llvm::BasicBlock::Create(llvm_, "casestmt.arm", currentFn_);
        auto* nextBB =
            llvm::BasicBlock::Create(llvm_, "casestmt.next", currentFn_);
        // CG38b: a BindingPattern arm exposes the scrutinee value via a
        // local slot keyed on the BindingPattern AST node — the same node
        // sema used as lsym->decl, so localSlot() resolves the identifier.
        // The slot must be filled before the guard (which may reference the
        // bound name) and before the arm body. The alloca goes in the
        // function entry block; the store is emitted in the current
        // (pattern-test) block, before we branch to the guard or body.
        if (auto* bp = dynamic_cast<const ast::BindingPattern*>(
                           arm.pattern.get())) {
            llvm::AllocaInst* slot = entryAlloca(scrut->getType());
            builder_.CreateStore(scrut, slot);
            locals_[bp] = slot;
        }
        if (arm.guard) {
            auto* guardBB = llvm::BasicBlock::Create(
                llvm_, "casestmt.guard", currentFn_);
            builder_.CreateCondBr(pm, guardBB, nextBB);
            builder_.SetInsertPoint(guardBB);
            llvm::Value* g = lowerExpr(*arm.guard);
            if (!g) return false;
            builder_.CreateCondBr(g, bodyBB, nextBB);
        } else {
            builder_.CreateCondBr(pm, bodyBB, nextBB);
        }
        builder_.SetInsertPoint(bodyBB);
        bool armTerm = lowerScopedBlock(arm.stmts);
        if (!armTerm) builder_.CreateBr(endBB);
        builder_.SetInsertPoint(nextBB);
    }
    // No arm matched → fall through (§19.6).
    builder_.CreateBr(endBB);
    builder_.SetInsertPoint(endBB);
    return false;
}


}  // namespace bezel::codegen
