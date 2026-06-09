#include "codegen/LlvmBackend_impl.hpp"

namespace bezel::codegen {

bool Emitter::lowerStmt(const ast::Stmt& s) {
    if (auto* ld = dynamic_cast<const ast::LocalDecl*>(&s)) {
        lowerLocalDecl(ld, ld->initializer.get(), ld->type.get());
        if (diverged_) return true;  // CG40: diverging initializer
        if (ld->isUsing) registerUsingDisposal(ld);
        // #445 / §30.5: a plain `var` task-typed local is freed at scope end via
        // bz_task_fire_and_forget — the same disposal a `using var` task gets.
        // lowerAwait never frees a named-local task (freeTaskAfter is only set for
        // an anonymous CallExpr operand), so a plain `var t := AsyncMethod();` was
        // silently leaked. Registering the disposal here closes the gap for both
        // the awaited case (`var t := M(); await t;`) and the unawaited case
        // (`var t := M();` with t never awaited). Double-free safety: the await
        // site does NOT free a named local → fire-and-forget at scope end is the
        // single free site; using-var takes the `isUsing` branch above, so this
        // branch never fires for it. (per §30.5 / §28)
        else if (taskTypeOfLocal(ld) && !disposeStack_.empty()) {
            auto it = locals_.find(ld);
            if (it != locals_.end()) {
                Disposal d{it->second, nullptr};
                d.task = true;
                disposeStack_.back().push_back(d);
            }
            // Still register the unwind-owned path for any non-task cleanup
            // (task locals have no owned class under unwindOwnedLocalClass — it
            // returns null for task types — so this is a no-op; called for safety).
            registerUnwindOwnedLocal(ld);
        }
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
                // #481 / §12.1 / §12.5 / §13 / §23: a VIRTUAL or INTERFACE
                // property setter dispatches dynamically through the receiver's
                // vtable/itable. Checked before the static setter / auto-property
                // store paths (which would run the *static* type's setter and
                // miscompile an override).
                {
                    auto* tps = dynamic_cast<sema::PropertySymbol*>(
                        ctx_.exprSymbol(*as->target));
                    if (tps && !tps->isIndexer &&
                        (propSetSlot_.count(tps) ||
                         ifacePropertySetIndex_.count(tps))) {
                        llvm::Value* rhsV = lowerExpr(*as->value);
                        if (lowerVirtualPropertySet(*as->target, rhsV))
                            return false;
                    }
                }
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
                    if (lowerCollectionIndexerSet(*ix, rhsOnce, as->value.get()))
                        return false;
                    // Not an indexer property — fall through to plain array
                    // assignment using the already-computed RHS.
                    llvm::Type* elemTy2 = nullptr;
                    llvm::Value* addr2 = lvalueAddr(*as->target, elemTy2);
                    if (!addr2) return false;
                    builder_.CreateStore(rhsOnce, addr2);
                    return false;
                }
            } else {
                // #481: a VIRTUAL/INTERFACE property compound assignment
                // `obj.P OP= v` is a read-modify-write — both the getter read
                // and the setter write dispatch dynamically.
                auto* tps = dynamic_cast<sema::PropertySymbol*>(
                    ctx_.exprSymbol(*as->target));
                if (tps && !tps->isIndexer &&
                    (propSetSlot_.count(tps) ||
                     ifacePropertySetIndex_.count(tps)) &&
                    (propGetSlot_.count(tps) ||
                     ifacePropertyGetIndex_.count(tps))) {
                    llvm::Value* cur = lowerExpr(*as->target);  // dyn getter
                    llvm::Value* rhs = lowerExpr(*as->value);
                    if (cur && rhs) {
                        std::string op(as->op);
                        if (!op.empty() && op.back() == '=') op.pop_back();
                        llvm::Value* res = emitCompoundOp(
                            op, cur, rhs, cur->getType()->isFloatingPointTy(),
                            isUnsignedTargetExpr(*as->target));
                        if (res && lowerVirtualPropertySet(*as->target, res))
                            return false;
                    }
                }
                if (lowerPropertyCompound(*as)) return false;
                // §20 / §23.4: user-defined indexer compound assignment
                // `a[i] OP= v`. Must come after lowerPropertyCompound (which
                // handles non-indexer computed properties) and before the generic
                // lvalueAddr fallback (which handles only built-in inline arrays
                // and returns null for user-indexer targets, silently dropping
                // the write). The getter and setter are called with the receiver
                // and index evaluated exactly once.
                if (lowerIndexerCompound(*as)) return false;
                // fix #508 / §4.4 / §4.5 / §17.15: heap-collection Array<T>
                // compound assignment `arr[i] OP= v`. Must come after
                // lowerIndexerCompound (user-defined indexer) and before the
                // lvalueAddr fallback (which returns null for a bz_array_t*
                // receiver, causing the write to be silently dropped). Evaluates
                // receiver and index once; reads via bz_array_get, applies OP,
                // writes back via bz_array_set / bz_array_set_disposing.
                if (lowerCollectionIndexerCompound(*as)) return false;
            }
            llvm::Type* elemTy = nullptr;
            llvm::Value* addr = lvalueAddr(*as->target, elemTy);
            if (!addr) return false;
            llvm::Value* rhs = lowerExpr(*as->value);
            if (!rhs) return false;
            // §8.6: a value-typed RHS assigned to an Object/interface lvalue is
            // boxed (so a later is/as/dispatch on the target sees a box). Keyed
            // off the target's static type; a no-op for reference RHS (#399).
            if (as->op == ":=")
                rhs = boxForTarget(rhs, ctx_.exprType(*as->target),
                                   as->value.get());
            // §5.1: an int assigned to a wider numeric slot (e.g. `d := n;`
            // with `d: double`, `n: int`) is implicitly widened. Materialize
            // the conversion sema admitted so the slot receives the value, not
            // the source type's raw bits (issue #408). No-op when the rhs type
            // already matches or no §5.1 numeric widening applies.
            // issue #421 / #410: thread the RHS signedness so an unsigned source
            // (`d := someUint` with `d: double`) widens via UIToFP/ZExt, not the
            // signed default — otherwise a large uint would store as negative.
            if (elemTy)
                rhs = coerceNumericToType(rhs, elemTy,
                                          isUnsignedTargetExpr(*as->value));
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
                // #429 / §4.6 / §28.9: a store into a CLASS string field /
                // auto-property takes an independent +1 (retain-new or transfer-
                // producer / release-old), released on the owner's disposal.
                // Reassigning a string LOCAL slot likewise retains-new / releases-
                // old so the slot keeps an independent +1 (a later field disposal
                // that releases the same string cannot then dangle the local — no
                // UAF). Local reassignment refcounting is gated on the analyzed
                // function (stringReleaseEligibleFn_) so it matches the bind-time
                // arming convention; a lambda/thunk body (not analyzed) keeps the
                // plain store. Both helpers return true when they handled the
                // store; otherwise fall through to the plain store (array
                // elements, ref params, static / record fields, string? slots,
                // and non-string targets are unchanged).
                bool handled =
                    lowerStringFieldStore(*as->target, addr, rhs,
                                          as->value.get());
                if (!handled && currentFn_ == stringReleaseEligibleFn_)
                    handled = lowerStringLocalReassign(*as->target, addr, elemTy,
                                                       rhs, as->value.get());
                // #429 (closure FIELD-STORE category) / §27.5 / §28.9: a store
                // into a CLASS closure (function-typed) field / auto-property
                // takes an independent +1 (retain-new or transfer-producer /
                // release-old), released on the owner's disposal.  Parallel to
                // lowerStringFieldStore above.
                if (!handled)
                    handled = lowerClosureFieldStore(*as->target, addr, rhs,
                                                     as->value.get());
                // #429 (box FIELD-STORE category) / §8.6 / §28.9: a store into
                // a CLASS Object/interface field whose every store (per the
                // collectBoxStorageFields pre-pass) uses a boxable value source
                // takes an independent +1 (retain-new or transfer-producer /
                // release-old), released on the owner's disposal. Parallel to
                // lowerClosureFieldStore above. Gated on isBoxFieldLvalue which
                // checks both the target type (Object/interface class field) and
                // the source type (boxable value or box-return call), so a
                // class-reference store to an Object field never reaches here.
                if (!handled)
                    handled = lowerBoxFieldStore(*as->target, addr, rhs,
                                                 as->value.get());
                if (!handled)
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
                    // #429 / §4.6 / §28.9: `field ??= e` stores into a (currently
                    // null) CLASS string field/auto-property only when null, so
                    // there is no old value to release — but the field still takes
                    // an INDEPENDENT +1 (transfer a producer / retain a borrow),
                    // released on the owner's disposal. Without this a borrow `e`
                    // would be released by disposal without a matching retain (a
                    // double-free). Restricted to class string field lvalues (a
                    // string local / record field / array element keeps the plain
                    // store). The slot is null here, so a plain store is correct
                    // after the retain.
                    if (isStringFieldLvalue(*as->target) &&
                        rhs->getType()->isPointerTy() &&
                        !isPlusOneStringProducer(as->value.get()))
                        emitStringRetain(rhs);
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
    // §17.6 / §25: when the target's type is a user-defined record or class
    // that declares `static method operator ++` (resp. `--`), dispatch to that
    // operator method and write the result back:
    //   x++ ≡ x := T.operator++(x)   (per §17.6).
    // For built-in numeric operands keep the existing x := x + 1 path.
    if (auto* inc = dynamic_cast<const ast::IncrementStmt*>(&s)) {
        if (inc->target) {
            // §25: look for a user-defined operator on the target's Named type.
            const std::string_view opTok = inc->op; // "++" or "--"
            sema::MethodSymbol* opMethod = nullptr;
            if (const sema::TypeRef* targetTy = ctx_.exprType(*inc->target)) {
                if (targetTy->kind == sema::TypeRef::Kind::Named &&
                    targetTy->declaration) {
                    for (sema::Symbol* m : targetTy->declaration->members) {
                        auto* ms = dynamic_cast<sema::MethodSymbol*>(m);
                        if (ms && ms->isOperator &&
                            ms->operatorToken == opTok &&
                            ms->params.size() == 1) {
                            opMethod = ms;
                            break;
                        }
                    }
                }
            }
            llvm::Type* elemTy = nullptr;
            llvm::Value* addr = lvalueAddr(*inc->target, elemTy);
            if (addr) {
                if (opMethod) {
                    // §17.6 / §25: dispatch to user-defined operator ++/--.
                    // Load the current value (struct by value, class as pointer),
                    // call the operator method, store the result back.
                    llvm::Value* cur = builder_.CreateLoad(elemTy, addr);
                    std::vector<llvm::Value*> args{cur};
                    llvm::Value* res =
                        emitMethodCall(opMethod, args, /*baseQualified=*/false);
                    if (res) builder_.CreateStore(res, addr);
                } else {
                    // Built-in numeric increment/decrement.
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
        }
        return false;
    }
    if (auto* rs = dynamic_cast<const ast::ReturnStmt*>(&s)) {
        // The return value is evaluated first, then in-scope `using
        // var`s are disposed (§28.3), then control leaves — so the
        // returned value reflects pre-disposal state.
        // #465 / §15.4: `return [e1, …]` whose declared return type is a HEAP
        // collection (`Array<T>` etc., §15.2). lowerExpr returns null for a
        // CollectionLiteralExpr, so the return would otherwise yield a null/zero
        // array (caller crashes). Materialize a real heap collection here (same
        // bz_array_alloc + bz_array_add as `new`/`.Add`); ownership transfers to
        // the caller as the §28.3 owned result.
        // #538 / stdlib §4.8: same problem for `return ["k1":v1, …]` — a dict
        // literal also falls through lowerExpr → null.  The `!cl->isDict` guard
        // below is widened to also handle dict literals via the same
        // materializeHeapDictLiteral helper that #534 introduced for local-decls.
        llvm::Value* v = nullptr;
        if (rs->value) {
            if (auto* cl = dynamic_cast<const ast::CollectionLiteralExpr*>(
                    rs->value.get())) {
                if (currentReturnTypeNode_) {
                    if (const sema::TypeRef* rrt =
                            ctx_.resolvedType(*currentReturnTypeNode_)) {
                        if (!cl->isDict)
                            v = materializeHeapCollectionLiteral(cl, rrt);
                        else if (!cl->keys.empty())
                            v = materializeHeapDictLiteral(cl, rrt);
                    }
                }
            }
            if (!v) v = lowerExpr(*rs->value);
        }
        if (diverged_) return true;  // CG40: the value expression threw
        // §30.1: inside an async method's Resume function, `return expression;`
        // completes the OUTER task with the value (and `return;` / fall-off
        // completes with no value), then returns void from Resume — it does NOT
        // box for or `ret` the entry function's Task ptr return type. Handled
        // before the §8.6 boxing / ownership-transfer / finally machinery, which
        // are all keyed off the (Task-typed) declared return and don't apply to
        // the unwrapped result value. (await-in-try / return-through-finally in
        // async bodies is deferred; finallyStack_ is empty for the minimal cut.)
        if (smKind_ == SuspendKind::Await && smStateStruct_) {
            emitAllDisposals();
            emitAsyncComplete(v);
            builder_.CreateRetVoid();
            return true;
        }
        // §8.6: box a value-typed `return expr` whose declared return type is
        // Object/interface, so the caller's is/as/virtual dispatch sees a box.
        if (v && rs->value && currentReturnTypeNode_)
            v = boxForTarget(v, ctx_.resolvedType(*currentReturnTypeNode_),
                             rs->value.get());
        // #429 (RETURN-escape category) / §4.6 / §28.9 / §28.3: a method whose
        // declared return type is (non-nullable) `string` mints EXACTLY +1 for
        // the returned value, transferring ownership of one reference to the
        // caller. The caller treats the result as a +1 producer
        // (isPlusOneStringProducer) and balances it with a transfer-then-release,
        // so the round trip neither leaks (the +1 is released) nor dangles (the
        // +1 keeps a borrowed value alive past the callee). The value is either
        // TRANSFERRED (it already carries a +1 the callee owns) or RETAINED (a
        // pure borrow, so a fresh +1 is minted); bz_string_retain is a no-op on a
        // sentinel/literal/null. We TRANSFER (no retain) when the returned
        // expression already owns a +1:
        //   * a +1 PRODUCER expression (`+`/interpolation/range-slice, or a
        //     nested string-returning call), whose single reference flows out; or
        //   * an IDENTIFIER naming a non-nullable string LOCAL that owns a +1 —
        //     a producer-initialized local (transferred the producer's +1 into
        //     its slot) or a borrow local (retained +1 at bind, stringBorrowAll_).
        //     Such a returned local is escaped (escape walk) so it is NOT released
        //     at scope end, and is excluded from disposal by the retSlot path
        //     below — its single +1 flows out to the caller instead of leaking.
        // Otherwise (a param read, a field read `this.f`, a literal, an alias not
        // owning a +1) the value is a BORROW → RETAIN to mint the +1.
        if (v && rs->value && currentReturnTypeNode_ &&
            v->getType()->isPointerTy()) {
            const sema::TypeRef* declRet =
                ctx_.resolvedType(*currentReturnTypeNode_);
            if (declRet && !declRet->nullable &&
                declRet->kind == sema::TypeRef::Kind::Primitive &&
                declRet->primitive == sema::PrimitiveType::String &&
                !isPlusOneStringProducer(rs->value.get()) &&
                !returnedStringLocalOwnsPlusOne(rs->value.get()))
                emitStringRetain(v);
        }
        // #429 (closure RETURN category) / §27.5 / §28.9 / §28.3: a method
        // whose declared return type is a function type (Kind::Function) mints
        // EXACTLY +1 for the returned closure cell, transferring ownership of
        // one reference to the caller. Mirrors the string RETURN-escape logic
        // immediately above, but for closures via emitClosureRetain. We
        // TRANSFER (no retain) when the returned expression already owns a +1:
        //   * a +1 PRODUCER expression (a lambda, a method-ref, or a user-
        //     defined closure-returning call — isPlusOneClosureProducer or
        //     isUserMethodClosureReturnCall), whose single reference flows out;
        //   * an IDENTIFIER naming a closure-typed LOCAL that owns a +1 in its
        //     slot (returnedClosureLocalOwnsPlusOne — its single +1 flows out).
        // Otherwise (a param read, a field read, an identifier borrow not owning
        // a +1) the value is a BORROW → RETAIN via emitClosureRetain to mint
        // the +1. bz_closure_retain is null-safe (a null closure is a no-op).
        if (v && rs->value && currentReturnTypeNode_ &&
            v->getType()->isPointerTy()) {
            const sema::TypeRef* declRet =
                ctx_.resolvedType(*currentReturnTypeNode_);
            if (declRet && declRet->kind == sema::TypeRef::Kind::Function &&
                !isPlusOneClosureProducer(rs->value.get()) &&
                !isUserMethodClosureReturnCall(rs->value.get()) &&
                !returnedClosureLocalOwnsPlusOne(rs->value.get()))
                emitClosureRetain(v);
        }
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
        // §30.7: `await for var x in asyncSeq do ... end` — async iteration.
        // Must run before the synchronous for-handlers, which would misread the
        // async-enumerator source.
        if (fors->isAwait) {
            if (lowerAwaitFor(*fors)) return false;
            ctx_.diagnostics().report(
                Severity::Error, s.span.begin,
                "not yet implemented: codegen for this `await for` statement "
                "(only `await for` over an async-iterator source is supported "
                "(§30.7); `with` cancellation / owned-async iteration are "
                "deferred)");
            return false;
        }
        if (lowerRangeFor(*fors)) return false;
        // issue #370 / §4.8: `for var k in <dict>.Keys` — iterate dict keys.
        // (`<dict>.Keys` has no sema element type, so this must run before the
        // generic for-handlers; it recognizes the AST shape directly.)
        if (lowerDictKeysFor(*fors)) return false;
        // issue #537 / §4.8: `for var v in <dict>.Values` — iterate dict values.
        // Mirrors lowerDictKeysFor but snapshots values (genericArgs[1]) instead
        // of keys; must also run before the generic for-handlers.
        if (lowerDictValuesFor(*fors)) return false;
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
    // §29.3: yield return / yield break inside an iterator method. This is the
    // `Yield` suspend point: it spills the suspend-locals into the shared
    // state-machine frame, transitions the state index, and returns from
    // MoveNext. The frame plumbing (smStateStruct_/smSelf_/smAllocaToField_/
    // smStateGEP_/smStateBBs_/smNextSuspendState_) is the kind-agnostic
    // scaffolding from LlvmBackend_Iterators.cpp; the value-producing pieces
    // (current_val store, ret true/false) are yield-specific. The §30 `await`
    // suspend point (Phase 2b) reuses the same spill/restore + state-index
    // machinery here with await-specific suspend/resume actions (register a
    // task continuation, return the in-progress Task; resume reads the result).
    if (auto* ys = dynamic_cast<const ast::YieldStmt*>(&s)) {
        // §29.3 (Yield): a synchronous iterator's `yield` returns a bool from
        // MoveNext. §30.7 (AsyncYield): an ASYNC iterator's `yield` completes the
        // per-advance Task<bool> and returns VOID from the Resume continuation
        // (the same suspend/spill machinery; only the suspend ACTION differs).
        bool isAsyncYield = (smKind_ == SuspendKind::AsyncYield);
        if (smStateStruct_ && smSelf_ &&
            (smKind_ == SuspendKind::Yield || isAsyncYield)) {
            // Save all suspend-locals to the state frame before yielding.
            for (auto& [allocaSlot, fieldIdx] : smAllocaToField_) {
                llvm::Value* fptr = builder_.CreateStructGEP(
                    smStateStruct_, smSelf_, fieldIdx);
                llvm::Value* curVal = builder_.CreateLoad(
                    allocaSlot->getAllocatedType(), allocaSlot);
                builder_.CreateStore(curVal, fptr);
            }

            if (ys->isBreak) {
                // yield break: set state=-1; sync → ret false, async → complete
                // the per-advance task with false (exhausted) + ret void.
                builder_.CreateStore(builder_.getInt32(-1), smStateGEP_);
                if (isAsyncYield) {
                    emitAsyncIterComplete(/*hasValue=*/false);
                    builder_.CreateRetVoid();
                } else {
                    builder_.CreateRet(builder_.getFalse());
                }
                return true;
            }
            // yield return expr: store to current_val, set next state; sync →
            // ret true, async → complete the per-advance task with true + ret void.
            if (ys->value) {
                llvm::Value* v = lowerExpr(*ys->value);
                if (v) {
                    if (v->getType() != iterElemTy_ &&
                        v->getType()->isIntegerTy() && iterElemTy_->isIntegerTy())
                        v = builder_.CreateTrunc(v, iterElemTy_);
                    if (v->getType() == iterElemTy_) {
                        llvm::Value* cvField = builder_.CreateStructGEP(
                            smStateStruct_, smSelf_, iterCurrentValField_);
                        builder_.CreateStore(v, cvField);
                    }
                }
            }
            unsigned nextState = ++smNextSuspendState_;
            builder_.CreateStore(builder_.getInt32((int)nextState), smStateGEP_);
            if (isAsyncYield) {
                emitAsyncIterComplete(/*hasValue=*/true);
                builder_.CreateRetVoid();
            } else {
                builder_.CreateRet(builder_.getTrue());
            }
            // Continue emitting code after this yield in the next state block.
            if (nextState < smStateBBs_.size()) {
                builder_.SetInsertPoint(smStateBBs_[nextState]);
                // Restore all iterator locals from the state struct.
                for (auto& [allocaSlot, fieldIdx] : smAllocaToField_) {
                    llvm::Value* fptr = builder_.CreateStructGEP(
                        smStateStruct_, smSelf_, fieldIdx);
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
    // §30.9 / §31.2 (#438): in an async state machine, an `await` inside a catch
    // arm or the finally is a real suspend point. The resume re-enters this
    // handler scope mid-handler, and a cross-thread resume runs on a worker with
    // a fresh stack — so any handler-context alloca STORED before the in-handler
    // await and READ after it (the catch-arm exception binding, the synthetic
    // caught-exception disposal slot, the finally exit-mode / return-value slots)
    // must round-trip the HEAP frame, not a function-local stack slot. We register
    // those slots in asyncHandlerSpillSlots_; fixupSuspendDominance later
    // force-promotes each into the frame spill area (repointing the alloca to a
    // heap GEP). Only the slots of a try region that actually contains an
    // in-handler await are frame-backed (a try whose catch/finally has no await
    // keeps its handler allocas on the stack, the cheap path).
    const bool asyncSM = (smKind_ == SuspendKind::Await ||
                          smKind_ == SuspendKind::AsyncYield) &&
                         smStateStruct_ && smSelf_;
    // Whether THIS try's catch arms / finally suspend on an `await` is decided
    // per-arm below (armAwaits) and, for the finally, here.
    const bool finallyAwaits =
        asyncSM && hasFin && containsAwait(trys.finallyBody);
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
        // §30.9 / §31.2 (#438): an in-finally await re-enters the finally
        // mid-body; the exit-mode and return-value slots were set BEFORE the
        // finally (in the normal-exit path) and are read AFTER the suspend (the
        // finally.resume dispatch), so frame-back them across the cross-thread hop.
        if (finallyAwaits) {
            asyncHandlerSpillSlots_.push_back(exitModSlot);
            if (retValSlot) asyncHandlerSpillSlots_.push_back(retValSlot);
        }
    }

    llvm::BasicBlock* savedUnwind = unwindTarget_;
    std::size_t savedDepth = unwindScopeDepth_;
    unwindTarget_ = dispatchBB;
    unwindScopeDepth_ = disposeStack_.size();
    // #467 / §28.3: the dispose-stack depth at THIS try's entry. The scopes in
    // [savedDepth, thisTryDepth) lie between this try and its enclosing handler
    // (`outerUnwind`). A throw out of this try's body only disposes scopes down
    // to thisTryDepth; on the exception path through this try's finally those
    // intervening scopes (e.g. an outer `using var`) are still live and must be
    // disposed when re-propagating to outerUnwind — see finUnwindBB below.
    std::size_t thisTryDepth = unwindScopeDepth_;
    bool bodyTerm = lowerScopedBlock(trys.body);
    unwindTarget_ = savedUnwind;
    unwindScopeDepth_ = savedDepth;
    if (!bodyTerm) builder_.CreateBr(normalExit);

    // Catch dispatch: test arms in order against the in-flight exception.
    builder_.SetInsertPoint(dispatchBB);
    llvm::Value* exc = builder_.CreateLoad(ptrTy(), excGlobal());
    for (const auto& arm : trys.catches) {
        // §30.9 / §31.2 (#438): does THIS arm suspend on an `await` (guard or
        // body)? If so its handler-context allocas must survive a cross-thread
        // resume — frame-back them below.
        const bool armAwaits =
            asyncSM &&
            ((arm.guard && exprContainsAwait(arm.guard.get())) ||
             stmtContainsAwait(arm.body.get()));
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
                // The binding is read in the arm body, possibly AFTER an in-arm
                // await (§31.1: the borrow's lifetime is the whole arm body, which
                // spans the suspend) — frame-back it so the caught object pointer
                // survives the cross-thread resume.
                if (armAwaits) asyncHandlerSpillSlots_.push_back(slot);
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
            // §30.9 / §31.2 (#438): the caught-exception disposal runs on the
            // arm's NORMAL fall-through — which, with an in-arm await, is AFTER the
            // resume. emitDisposeAndReclaim reads the object pointer from excSlot,
            // so the slot must survive the cross-thread hop: frame-back it.
            if (armAwaits) asyncHandlerSpillSlots_.push_back(excSlot);
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
        if (finallyAwaits) {
            // §30.9 / §31.2 (#438): an `await` in the finally on the EXCEPTION
            // path suspends with an exception in flight. `bz$exc` is a single
            // process-global (not thread-local), so it would be clobbered by any
            // other task that runs on this thread while we are suspended. Capture
            // the in-flight exception object into a FRAME-BACKED slot (so it
            // survives the cross-thread resume), clear the global for the duration
            // of the finally, then restore it before re-propagating to the outer
            // handler — which expects to read the original object from bz$exc.
            llvm::AllocaInst* pendingExcSlot = entryAlloca(ptrTy());
            llvm::Value* inflight = builder_.CreateLoad(ptrTy(), excGlobal());
            builder_.CreateStore(inflight, pendingExcSlot);
            asyncHandlerSpillSlots_.push_back(pendingExcSlot);
            builder_.CreateStore(
                llvm::ConstantPointerNull::get(ptrTy()), excGlobal());
            bool finUTerm = lowerScopedBlock(trys.finallyBody);
            if (!finUTerm) {
                // Restore the captured in-flight exception and re-propagate. If
                // the finally itself threw (set bz$exc non-null), that NEW
                // exception supersedes the original (§31.2 finally-throws
                // semantics); keep it. Otherwise restore the original.
                llvm::Value* afterFin = builder_.CreateLoad(ptrTy(), excGlobal());
                llvm::Value* finThrew = builder_.CreateICmpNE(
                    afterFin, llvm::ConstantPointerNull::get(ptrTy()),
                    "fin.threw");
                auto* restoreBB =
                    llvm::BasicBlock::Create(llvm_, "finally.restore", currentFn_);
                auto* propBB =
                    llvm::BasicBlock::Create(llvm_, "finally.reraise", currentFn_);
                builder_.CreateCondBr(finThrew, propBB, restoreBB);
                builder_.SetInsertPoint(restoreBB);
                llvm::Value* orig = builder_.CreateLoad(ptrTy(), pendingExcSlot);
                builder_.CreateStore(orig, excGlobal());
                builder_.CreateBr(propBB);
                builder_.SetInsertPoint(propBB);
                // #467 / §28.3: same intervening-scope discharge as the
                // synchronous path below. Emitted after the in-flight exception
                // is restored to bz$exc so the enclosing handler sees the right
                // object; the disposals themselves do not touch bz$exc.
                emitUnwindDisposalsInRange(savedDepth, thisTryDepth);
                builder_.CreateBr(outerUnwind);
            }
        } else if (!lowerScopedBlock(trys.finallyBody)) {
            // #467 / §28.3 (lines 4914, 4922): the explicit finally has now run;
            // as the exception unwinds further out to the enclosing handler
            // (outerUnwind), dispose any owned locals / using-vars that live in
            // the scopes BETWEEN this try and that handler — the throw site only
            // disposed scopes down to thisTryDepth, so [savedDepth, thisTryDepth)
            // is still live. (No-op when this try sits directly in its handler's
            // scope, i.e. savedDepth == thisTryDepth.)
            emitUnwindDisposalsInRange(savedDepth, thisTryDepth);
            builder_.CreateBr(outerUnwind);
        }
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

    // fix #499 / §9.8 / §16.5: `for var i in r` where r is a Range-typed
    // variable (not an inline RangeExpr literal). Load the stored struct and
    // synthesise the same counted-loop that lowerRangeFor produces for a
    // literal range — using the struct's start/end/step/flags fields.
    if (!range && fs.iterable) {
        auto* iterTy = ctx_.exprType(*fs.iterable);
        if (!isRangeType(iterTy)) return false;  // not a Range — try other for-handlers

        llvm::Value* rv = lowerExpr(*fs.iterable);
        if (!rv) return false;

        llvm::Value* lo64 = builder_.CreateExtractValue(rv, {0u}, "rfv.lo");
        llvm::Value* hi64 = builder_.CreateExtractValue(rv, {1u}, "rfv.hi");
        llvm::Value* step64 = builder_.CreateExtractValue(rv, {2u}, "rfv.step");
        llvm::Value* flags8 = builder_.CreateExtractValue(rv, {3u}, "rfv.flg");
        llvm::Value* incl   = builder_.CreateTrunc(flags8, i1(), "rfv.incl");

        // Truncate to i32 for the loop variable (int Range). This matches the
        // behavior of the inline-literal path which uses lo->getType() = i32.
        llvm::Value* lo   = builder_.CreateTrunc(lo64, i32(), "rfv.lo32");
        llvm::Value* hi   = builder_.CreateTrunc(hi64, i32(), "rfv.hi32");
        llvm::Value* step = builder_.CreateTrunc(step64, i32(), "rfv.step32");

        // Determine direction from step sign (use the i32 form).
        bool descending = false;
        if (auto* cs = llvm::dyn_cast<llvm::ConstantInt>(step))
            descending = cs->getValue().isNegative();

        llvm::AllocaInst* slot = entryAlloca(i32());
        locals_[&fs] = slot;
        builder_.CreateStore(lo, slot);

        auto* condBB = llvm::BasicBlock::Create(llvm_, "rfv.cond", currentFn_);
        auto* bodyBB = llvm::BasicBlock::Create(llvm_, "rfv.body", currentFn_);
        auto* incBB  = llvm::BasicBlock::Create(llvm_, "rfv.inc",  currentFn_);
        auto* endBB  = llvm::BasicBlock::Create(llvm_, "rfv.end",  currentFn_);
        builder_.CreateBr(condBB);

        builder_.SetInsertPoint(condBB);
        llvm::Value* cur = builder_.CreateLoad(i32(), slot);
        // Direction-aware bound check using the incl flag from the struct.
        llvm::Value* cond;
        if (descending) {
            llvm::Value* geLE = builder_.CreateICmpSGE(cur, hi);
            llvm::Value* geLT = builder_.CreateICmpSGT(cur, hi);
            cond = builder_.CreateSelect(incl, geLE, geLT, "rfv.cond");
        } else {
            llvm::Value* leLE = builder_.CreateICmpSLE(cur, hi);
            llvm::Value* leLT = builder_.CreateICmpSLT(cur, hi);
            cond = builder_.CreateSelect(incl, leLE, leLT, "rfv.cond");
        }
        builder_.CreateCondBr(cond, bodyBB, endBB);

        builder_.SetInsertPoint(bodyBB);
        loopStack_.push_back({incBB, endBB, disposeStack_.size()});
        bool term = lowerScopedBlock(fs.body);
        loopStack_.pop_back();
        if (!term) builder_.CreateBr(incBB);

        builder_.SetInsertPoint(incBB);
        llvm::Value* v = builder_.CreateLoad(i32(), slot);
        builder_.CreateStore(builder_.CreateAdd(v, step), slot);
        builder_.CreateBr(condBB);

        builder_.SetInsertPoint(endBB);
        return true;
    }

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
    if (smStateStruct_ && smSelf_) {
        auto fit = smLocalFields_.find(&fs);
        if (fit != smLocalFields_.end())
            smAllocaToField_[slot] = fit->second;
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
    if (smStateStruct_) {
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
            // The loop's continue-target is incBBR; the body falls through to
            // it unless it already terminated (e.g. a trailing return/throw).
            // Emit the increment INTO incBBR (not the body block) so a
            // `continue` that branches to incBBR lands on a real, terminated
            // block — emitting the increment in bodyBBR left incBBR without a
            // terminator (malformed IR), mirroring the far.inc path below.
            loopStack_.push_back({incBBR, endBBR, disposeStack_.size()});
            bool termR = lowerScopedBlock(fs.body);
            loopStack_.pop_back();
            if (!termR) builder_.CreateBr(incBBR);
            builder_.SetInsertPoint(incBBR);
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
    initStringRc(strObj);  // #422: producer's reference, rc := 1
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


// issue #537 / §4.8: `for var v in <dict>.Values` — iterate a Dictionary's
// values.  The runtime snapshot helpers bz_dict_values_str (string-key table)
// and bz_dict_values_gen (generic-key table) return a malloc'd void** array of
// the live slot values — the same void* encoding used by Add/Set (inttoptr for
// scalars, direct pointer for strings/classes, heap-box pointer for records).
// The loop decodes each slot value into the V LLVM type using the same logic as
// TryGetValue and GetOrAdd.
bool Emitter::lowerDictValuesFor(const ast::ForStmt& fs) {
    if (!fs.iterable) return false;
    auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(fs.iterable.get());
    if (!ma || !ma->target || ma->member != "Values") return false;
    auto* dictTy = ctx_.exprType(*ma->target);
    if (!isDictTypeName(dictTy)) return false;
    // genericArgs: [0] = K, [1] = V
    if (dictTy->genericArgs.size() < 2 || !dictTy->genericArgs[1]) return false;
    const sema::TypeRef* valTy = dictTy->genericArgs[1];

    // Determine if V is a record (heap-boxed in the dict), a scalar, or a ptr.
    // Any Named-kind TypeRef with a Record declaration is boxed (issue D4).
    const sema::TypeSymbol* valRec =
        (valTy && valTy->kind == sema::TypeRef::Kind::Named &&
         valTy->declaration &&
         valTy->declaration->kind == ast::TypeDeclKind::Record)
            ? valTy->declaration : nullptr;
    // issue #539 / §4.8 / §7: nullable value types (int?, bool?, enum?,
    // nullable record) lower to a tagged {i8 hasValue; T} struct and are
    // heap-boxed by encodeValue.  Decode them the same way records are:
    // load the struct back from the box pointer in the slot.
    bool valNullableVal = valTy && isNullableValueTy(valTy);

    llvm::Type* valLlvm = lowerTypeRef(valTy);
    if (!valLlvm) return false;

    llvm::Value* dictObj = lowerExpr(*ma->target);
    if (!dictObj) return false;

    // Snapshot the values array.  Choose the string-key or generic-key helper
    // based on whether the key (genericArgs[0]) routes through the generic table.
    auto* valsOutSlot = entryAlloca(ptrTy());
    auto* cntSlot     = entryAlloca(i64());

    bool useGen = genericBackingType(dictTy, 0, /*isSet=*/false, nullptr) != nullptr;
    if (useGen) {
        // Generic-key dict: bz_dict_values_gen(dict, void*** vals_out, int64_t* n)
        auto* fn = declareC("bz_dict_values_gen", llvm::Type::getVoidTy(llvm_),
                            {ptrTy(), ptrTy(), ptrTy()});
        builder_.CreateCall(fn, {dictObj, valsOutSlot, cntSlot});
    } else {
        // String-key dict: bz_dict_values_str(dict, void*** vals_out, int64_t* n)
        auto* fn = declareC("bz_dict_values_str", llvm::Type::getVoidTy(llvm_),
                            {ptrTy(), ptrTy(), ptrTy()});
        builder_.CreateCall(fn, {dictObj, valsOutSlot, cntSlot});
    }
    llvm::Value* valsArr = builder_.CreateLoad(ptrTy(), valsOutSlot); // void**
    llvm::Value* cnt     = builder_.CreateLoad(i64(), cntSlot);
    auto* freeFn = declareC("free", llvm::Type::getVoidTy(llvm_), {ptrTy()});

    auto* idxSlot = entryAlloca(i64());
    builder_.CreateStore(builder_.getInt64(0), idxSlot);

    // Loop variable slot — stores the decoded V value.  For all types
    // (including records) the slot holds the value itself (a struct for records,
    // not a pointer).  fix #563: the previous code used ptrTy() for records and
    // stored the raw heap-box pointer; fieldAddr then GEP'd into the
    // pointer-holding alloca rather than the record struct, yielding garbage
    // field reads.  Mirror emitGenericKeyTableFor: load the struct out of the
    // box and store the struct value into a slot typed as valLlvm.
    // issue #539: nullable value types lower to a tagged struct — same: valLlvm.
    auto* varSlot = entryAlloca(valLlvm);
    locals_[&fs] = varSlot;

    auto* condBB = llvm::BasicBlock::Create(llvm_, "dv.cond", currentFn_);
    auto* bodyBB = llvm::BasicBlock::Create(llvm_, "dv.body", currentFn_);
    auto* incBB  = llvm::BasicBlock::Create(llvm_, "dv.inc",  currentFn_);
    auto* endBB  = llvm::BasicBlock::Create(llvm_, "dv.end",  currentFn_);
    builder_.CreateBr(condBB);

    builder_.SetInsertPoint(condBB);
    llvm::Value* idx = builder_.CreateLoad(i64(), idxSlot);
    builder_.CreateCondBr(builder_.CreateICmpSLT(idx, cnt), bodyBB, endBB);

    builder_.SetInsertPoint(bodyBB);
    llvm::Value* idxB   = builder_.CreateLoad(i64(), idxSlot);
    llvm::Value* vpPtr  = builder_.CreateGEP(ptrTy(), valsArr, idxB);
    llvm::Value* slotV  = builder_.CreateLoad(ptrTy(), vpPtr);  // raw void*

    // Decode void* → V following the same encoding as TryGetValue / GetOrAdd.
    //   record value: slotV is the heap-box pointer → load the record struct
    //     out of the box (fix #563; mirrors emitGenericKeyTableFor and the
    //     direct-indexer get in lowerCollectionIndexerGet §4.8).  The box is
    //     owned by the dict and must not be freed — we only read its bytes.
    //   nullable value type (issue #539): slotV is the heap-box pointer of the
    //     tagged {i8,T} struct → load the struct from the box.
    //   scalar (int/long/bool/char/enum/float/double): slotV was inttoptr'd →
    //     ptrtoint back to i64, then trunc/bitcast to the target type.
    //   pointer (string / class object): slotV IS the value.
    llvm::Value* decoded;
    if (valRec) {
        // fix #563 / §4.8: load the record struct from the heap box.  The
        // runtime heap-boxes records (malloc + memcpy) so they fit in the void*
        // value slot; decode by loading the struct type out of the box pointer,
        // exactly as lowerCollectionIndexerGet does for dict[key] on a record V.
        decoded = builder_.CreateLoad(valLlvm, slotV, "dv.rec");
    } else if (valNullableVal && valLlvm->isStructTy()) {
        // Nullable value type: unbox the tagged struct from the heap box.
        // The box is non-null even when hasValue=0 (the null tag is in the
        // bytes, not the pointer).
        decoded = builder_.CreateLoad(valLlvm, slotV, "dv.nvval");
    } else if (!valLlvm->isPointerTy()) {
        // Scalar: undo the inttoptr encoding.
        llvm::Value* asI64 = builder_.CreatePtrToInt(slotV, i64());
        if (valLlvm->isDoubleTy())
            decoded = builder_.CreateBitCast(asI64, valLlvm);
        else if (valLlvm->isFloatTy())
            decoded = builder_.CreateBitCast(
                builder_.CreateTrunc(asI64, i32()), valLlvm);
        else if (valLlvm->isIntegerTy(64))
            decoded = asI64;
        else
            decoded = builder_.CreateTrunc(asI64, valLlvm);
    } else {
        // Pointer (string / class object): the void* IS the value.
        decoded = slotV;
    }
    builder_.CreateStore(decoded, varSlot);

    loopStack_.push_back({incBB, endBB, disposeStack_.size()});
    bool term = lowerScopedBlock(fs.body);
    loopStack_.pop_back();
    if (!term) builder_.CreateBr(incBB);

    builder_.SetInsertPoint(incBB);
    llvm::Value* idxI = builder_.CreateLoad(i64(), idxSlot);
    builder_.CreateStore(builder_.CreateAdd(idxI, builder_.getInt64(1)), idxSlot);
    builder_.CreateBr(condBB);

    builder_.SetInsertPoint(endBB);
    builder_.CreateCall(freeFn, {valsArr});  // free the snapshot array itself
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
    if (!iterTy || iterTy->kind != sema::TypeRef::Kind::Named) return false;
    // §2.5 / §29.3 (issue #495): for a LOCALLY-declared iterable the name
    // comes from astNode; for an IMPORTED-type iterable (cross-module call
    // returning IEnumerable<T>) astNode is null and the name is in importedName
    // (set by ModuleLoader from the metadata namedName field).  Declaration must
    // be null in both cases — a non-null declaration means a user class whose
    // GetEnumerator is handled by lowerIEnumerableFor.
    if (iterTy->declaration) return false;
    std::string_view n;
    if (iterTy->astNode && iterTy->astNode->name &&
        !iterTy->astNode->name->parts.empty()) {
        n = iterTy->astNode->name->parts[0];
    } else if (!iterTy->importedName.empty()) {
        n = iterTy->importedName;
    } else {
        return false;
    }
    // Only handle the iteration-interface family.
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
        // Helper: is `rt` a concrete user class (has a declaration pointer)?
        auto checkNamed = [&](sema::TypeRef* rt) {
            return rt && rt->kind == sema::TypeRef::Kind::Named && rt->declaration;
        };
        // §29.2 (#437): a parameter/local typed as the pure `IEnumerable<T>`
        // interface uses vtable dispatch. buildOrderedArgs wraps any Array<T>
        // argument in a vtable-backed enumerator at the call site, so the
        // callee sees a uniform vtable-backed object regardless of whether
        // the caller passed an Array or an iterator state machine. Richer
        // interfaces (IReadOnlyList etc.) also support indexer access and are
        // NOT wrapped at the call site; they keep the array-backed protocol.
        auto checkPureIEnumerable = [&](sema::TypeRef* rt) -> bool {
            if (!rt || rt->kind != sema::TypeRef::Kind::Named) return false;
            if (rt->declaration) return false;  // user class → handled by checkNamed
            if (!rt->astNode || !rt->astNode->name ||
                rt->astNode->name->parts.empty())
                return false;
            return rt->astNode->name->parts[0] == "IEnumerable";
        };
        if (auto* par = dynamic_cast<sema::ParameterSymbol*>(sym)) {
            if (par->declType) {
                auto* rt = ctx_.resolvedType(*par->declType);
                if (checkNamed(rt) || checkPureIEnumerable(rt))
                    isArrayBacked = false;
            }
        } else if (auto* loc = dynamic_cast<sema::LocalSymbol*>(sym)) {
            if (checkNamed(loc->declType) || checkPureIEnumerable(loc->declType))
                isArrayBacked = false;
        }
    }

    if (isArrayBacked) {
        // Use direct bz_array_t protocol.
        // per §9: a record element type lowers to an LLVM StructType whose
        // allocation size may exceed 8 bytes; use DataLayout for structs
        // so the bz_array_ptr_at stride matches the actual element layout
        // (same fix as collectionElemSize in LlvmBackend_Calls.cpp).
        int64_t esz = 8;
        if (elemTy->isIntegerTy(32) || elemTy->isFloatTy()) esz = 4;
        else if (elemTy->isIntegerTy(1) || elemTy->isIntegerTy(8)) esz = 1;
        else if (elemTy->isIntegerTy(16)) esz = 2;
        else if (elemTy->isStructTy())
            esz = (int64_t)module_->getDataLayout().getTypeAllocSize(elemTy);
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
                        // §31.1/§31.2 (FINDING [4]): a throw inside the
                        // standalone block getter must propagate, not be
                        // swallowed — check the pending flag like a method call.
                        emitPendingCheck();
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


// §30.7: lower `await for var x in src do ... end`. The source is an
// IAsyncEnumerable<T> / IAsyncEnumerator<T>; for an async-iterator method call
// (`Gen()`) the value IS the enumerator (the generated frame, whose vtable has
// [1]=Dispose, [2]=MoveNextAsync, [3]=Current). Each iteration: call
// MoveNextAsync → Task<bool>, AWAIT it (a real suspend point in the consuming
// async method's own state machine, reusing emitAwaitSuspend), and on a true
// result read Current and run the body. The enumerator is disposed at loop exit.
//
// The enumerator pointer is kept as an SSA value computed once before the loop;
// because the loop head (which uses it) is re-entered by the consuming method's
// resume dispatch across each iteration's await suspend, fixupSuspendDominance
// promotes it into the heap frame so it survives a real cross-thread resume —
// exactly the mechanism the embedded-await pre-suspend operands already rely on.
bool Emitter::lowerAwaitFor(const ast::ForStmt& fs) {
    if (!fs.isAwait || !fs.iterable) return false;
    // §30.8 cancellation (`await for ... with token`) is deferred — the token
    // would be passed to GetAsyncEnumerator(token); our minimal cut iterates an
    // async-iterator-method result directly. Fall through to the diagnostic.
    if (fs.withExpr) return false;
    // The consuming context must be an async state machine (the await suspend
    // lives in its Resume function). sema rejects `await for` outside async; this
    // is a defensive guard so a stray case produces a diagnostic, not bad IR.
    if ((smKind_ != SuspendKind::Await && smKind_ != SuspendKind::AsyncYield) ||
        !smStateStruct_ || !smSelf_ || !asyncResumeFn_)
        return false;

    auto* iterTy = ctx_.exprType(*fs.iterable);
    if (!iterTy) return false;
    // Element type T from the source's first generic arg (IAsyncEnumerable<T>).
    llvm::Type* elemTy = i32();
    if (!iterTy->genericArgs.empty() && iterTy->genericArgs[0])
        elemTy = lowerTypeRef(iterTy->genericArgs[0]);

    // Evaluate the source to the enumerator pointer (an SSA value, used directly
    // below — see the function comment on why it must NOT be a stack slot).
    llvm::Value* enumPtr = lowerExpr(*fs.iterable);
    if (!enumPtr) return false;

    // Loop variable slot — the body sees `x` here.
    auto* varSlot = entryAlloca(elemTy);
    // §30.7: if the loop variable is live across an await in the BODY, it is a
    // suspend-local — frame-back it like range-for does.
    if (smStateStruct_ && smSelf_) {
        auto fit = smLocalFields_.find(&fs);
        if (fit != smLocalFields_.end())
            smAllocaToField_[varSlot] = fit->second;
    }
    locals_[&fs] = varSlot;

    // Vtable-dispatched MoveNextAsync (slot 2) / Current (slot 3) / Dispose
    // (slot 1) on the enumerator frame.
    auto* moveNextTy = llvm::FunctionType::get(ptrTy(), {ptrTy()}, false);
    auto* currentTy = llvm::FunctionType::get(elemTy, {ptrTy()}, false);
    auto* disposeTy =
        llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_), {ptrTy()}, false);

    auto* condBB = llvm::BasicBlock::Create(llvm_, "afor.cond", currentFn_);
    auto* bodyBB = llvm::BasicBlock::Create(llvm_, "afor.body", currentFn_);
    auto* endBB  = llvm::BasicBlock::Create(llvm_, "afor.end",  currentFn_);
    builder_.CreateBr(condBB);

    // cond: task = MoveNextAsync(enum); hasValue = await task.
    builder_.SetInsertPoint(condBB);
    llvm::Value* vt = builder_.CreateLoad(ptrTy(), enumPtr);
    llvm::Value* mnSlotPtr =
        builder_.CreateGEP(ptrTy(), vt, {builder_.getInt32(2)});
    llvm::Value* mnFnPtr = builder_.CreateLoad(ptrTy(), mnSlotPtr);
    llvm::Value* task =
        builder_.CreateCall(moveNextTy, mnFnPtr, {enumPtr}, "afor.task");
    // §30.7: await the per-advance task (a single-use producer task — free it
    // after its bool is read). This is a suspend point in the consuming method.
    llvm::Value* hasValue =
        emitAwaitSuspend(task, i1(), /*freeTaskAfter=*/true);
    if (!hasValue) return false;
    builder_.CreateCondBr(hasValue, bodyBB, endBB);

    // body: x := Current(enum); run the user body.
    builder_.SetInsertPoint(bodyBB);
    llvm::Value* vt2 = builder_.CreateLoad(ptrTy(), enumPtr);
    llvm::Value* curSlotPtr =
        builder_.CreateGEP(ptrTy(), vt2, {builder_.getInt32(3)});
    llvm::Value* curFnPtr = builder_.CreateLoad(ptrTy(), curSlotPtr);
    llvm::Value* curVal =
        builder_.CreateCall(currentTy, curFnPtr, {enumPtr}, "afor.cur");
    builder_.CreateStore(curVal, varSlot);

    loopStack_.push_back({condBB, endBB, disposeStack_.size()});
    bool term = lowerScopedBlock(fs.body);
    loopStack_.pop_back();
    if (!term) builder_.CreateBr(condBB);

    // end: dispose the enumerator (vtable slot 1) — frees the iterator frame.
    builder_.SetInsertPoint(endBB);
    llvm::Value* vt3 = builder_.CreateLoad(ptrTy(), enumPtr);
    llvm::Value* dispSlotPtr =
        builder_.CreateGEP(ptrTy(), vt3, {builder_.getInt32(1)});
    llvm::Value* dispFnPtr = builder_.CreateLoad(ptrTy(), dispSlotPtr);
    builder_.CreateCall(disposeTy, dispFnPtr, {enumPtr});
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
    // §7 / issue #447: pass the scrutinee's resolved TypeRef so patternMatches
    // can detect a nullable VALUE type ({i8 hasValue; T value} struct) and
    // apply hasValue-based matching instead of the pointer-based paths.
    const sema::TypeRef* scrutTy = ctx_.exprType(*cs.scrutinee);
    auto* endBB =
        llvm::BasicBlock::Create(llvm_, "casestmt.end", currentFn_);
    for (const auto& arm : cs.arms) {
        llvm::Value* pm = patternMatches(arm.pattern.get(), scrut, enumTy,
                                         isStr, scrutTy);
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
