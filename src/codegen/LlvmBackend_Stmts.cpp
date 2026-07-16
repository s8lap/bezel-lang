#include "codegen/LlvmBackend_impl.hpp"

namespace bezel::codegen {

bool Emitter::lowerStmt(const ast::Stmt& s) {
    if (auto* ld = dynamic_cast<const ast::LocalDecl*>(&s)) {
        lowerLocalDecl(ld, ld->initializer.get(), ld->type.get());
        // #823 / §27.5: a local declared INSIDE a loop body is re-initialised
        // every iteration, so a closure capturing it needs a fresh cell per
        // iteration. Record its slot so boxLocalSlot chooses fresh-each-iteration
        // boxing even when nested control flow (an inner loop / if) puts the init
        // store in a different basic block than the capture site (the current-BB
        // store heuristic would otherwise miss it → a stale first-iteration cell).
        if (!loopStack_.empty()) {
            auto lit = locals_.find(ld);
            if (lit != locals_.end())
                loopBodyDeclaredSlots_.insert(lit->second);
        }
        if (diverged_) return true;  // CG40: diverging initializer
        // §15.3 (#747): a move-only owning inline-array local auto-disposes its
        // slots at scope end, independent of `using var` (§28.10 line 2025).
        // When handled, bypass the using/task/unwind chain (none applies to a
        // move-only value-typed local).
        if (registerInlineOwnedArrayDisposal(ld)) return false;
        // §29.11 (#943): a plain `var` holding a vtable-backed re-iterable
        // IEnumerable<T>/IOwningEnumerable<T> source (a yield-iterator result, or an
        // array/user-class wrapped into one) is disposed once at scope end — the
        // for-in over it clones a fresh enumerator per loop and leaves the source
        // alive so a second loop replays. Checked before the unwind-owned path
        // (these declaration-less iterator types have no owned-class destructor).
        if (registerIterableSourceDisposal(ld)) return false;
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
        //
        // §28.3 v0.190 SB-01 (#1786 / #1870, "calls mint, storage lends"): register
        // this disposal ONLY for a task local that OWNS the frame — one whose
        // initializer is a mint CALL. A task local that merely BORROWS the frame (a
        // bare-identifier ALIAS, or a task-typed STORAGE read — field/property/
        // indexer) is skipped: its owner already disposes the single `bz_task_t*`,
        // so a second `d.task` here would double-free it (→ UAF that corrupts async
        // continuation state → hang). taskLocalInitializerIsBorrowSource mirrors
        // producerState's Owned-vs-Borrow split so codegen and the ownership pass
        // agree on who holds the disposal obligation. A borrow source falls through
        // to the unwind-owned path below (a no-op for task types), registering no
        // disposal. // per §28.3 v0.190 SB-01 (storage lends)
        //
        // The syntactic shape gate above covers the alias / storage-read borrows,
        // but two exotic CALL shapes are ALSO borrows producerState classifies
        // Borrowed while this gate would treat as mints: a bare-type-parameter-
        // return call at a task instantiation (`First<T>(a): T` at `Task<int>`,
        // #1801) and an owning-wrapper method returning a bare receiver owned-
        // field (the #1493 shape). Re-deriving that from the AST here would drift
        // from producerState, so the ownership pass records its Borrowed verdict
        // for those on the decl (`initializerCallIsBorrow`); consulting it keeps
        // codegen and the ownership pass in agreement — the borrow no longer
        // registers a second `d.task` (a double-free of the real owner's frame).
        // // per §28.3 v0.190 SB-01 (#1872)
        else if (taskTypeOfLocal(ld) && !disposeStack_.empty() &&
                 !taskLocalInitializerIsBorrowSource(ld) &&
                 !ld->initializerCallIsBorrow) {
            auto it = locals_.find(ld);
            if (it != locals_.end()) {
                Disposal d{it->second, nullptr};
                d.task = true;
                // §30.15 (v0.179, #1524): a plain `var t := channel.Receive();`
                // never awaited is abandoned exactly like a `using var` one —
                // route through the give-back protocol, not ordinary task
                // disposal (see LlvmBackend_Setup.cpp's using-var registration
                // for the full rationale).
                d.channelReceiveTask = isChannelReceiveInitializer(ld);
                disposeStack_.back().push_back(d);
            }
            // Still register the unwind-owned path for any non-task cleanup
            // (task locals have no owned class under unwindOwnedLocalClass — it
            // returns null for task types — so this is a no-op; called for safety).
            registerUnwindOwnedLocal(ld);
        }
        // §28.3 v0.150 (#1008 Part B): a plain `var` owned local the ownership
        // pass flagged for affine auto-drop (Owned at a normal-exit scope end,
        // never discharged on any path) is disposed at scope end on BOTH the
        // normal and unwind edges — the same scope-end disposal a `using var`
        // gets (registerUsingDisposal handles every kind: class, OwningArray /
        // OwningDictionary collections, etc.). The flag is only set for a never-
        // moved owner, so the slot is always live at scope end and the disposal
        // is exactly-once. registerUnwindOwnedLocal (the unwind-only path) does
        // NOT cover collections, hence routing the flagged local through the
        // full using-disposal dispatcher.
        else if (ld->autoDropAtScopeEnd) registerUsingDisposal(ld);
        // fix #1892 / §17.13 / §28.5: a plain `var` owning-COLLECTION local
        // discharged via a `_ := x;` DISCARD (announced consume). The ownership
        // pass markDisposedByName's it (so autoDropAtScopeEnd is false) and set
        // dischargedByDiscard on the decl. registerUnwindOwnedLocal (the class-
        // only unwind path below) does NOT cover collections, so the container
        // leaked when an operation between acquisition and the discard threw.
        // Route it through the full collection disposal (registerUsingDisposal →
        // bz_array_free/bz_dict_free/bz_llist_free with the element-disposer
        // cascade), which fires on BOTH the normal and unwind edges and is
        // null-safe. The discard codegen (lowerStmt DiscardStmt) disposes the
        // container at the discard point and nulls the slot, so this registration
        // is a no-op on the normal path (null reload) and the sole disposer on the
        // pre-discard-throw exception edge — exactly once, no double-free. The
        // flag is only set for the discard form (not `move`/`.Dispose()`), so this
        // never doubles a transfer whose collection slot codegen does not null.
        else if (ld->dischargedByDiscard &&
                 (collectionTypeOfLocal(ld) || dictTypeOfLocal(ld) ||
                  linkedListTypeOfLocal(ld)))
            registerUsingDisposal(ld);
        // §28.3: a plain `var` owned local NOT flagged for auto-drop (it is
        // discharged on some path) is auto-disposed only on the exception-
        // unwind path (an `unwindOnly` Disposal); its normal-exit discharge is
        // sema-enforced.
        else registerUnwindOwnedLocal(ld);
        return false;
    }
    if (auto* lc = dynamic_cast<const ast::LocalConstDecl*>(&s)) {
        lowerLocalDecl(lc, lc->value.get(), lc->type.get());
        // #823 / §27.5: same loop-body fresh-capture handling as LocalDecl.
        if (!loopStack_.empty()) {
            auto lit = locals_.find(lc);
            if (lit != locals_.end())
                loopBodyDeclaredSlots_.insert(lit->second);
        }
        return diverged_;  // CG40
    }
    if (auto* as = dynamic_cast<const ast::AssignmentStmt*>(&s)) {
        if (as->target && as->value) {
            // per §24.4: `event += handler` / `event -= handler` (inside the
            // declaring class) desugar to Add/Remove — not a real compound
            // assignment. Checked first so the event target is not treated as
            // an lvalue.
            if (tryLowerEventCompound(*as)) return false;
            // §6.4 / #647: `atom.Value := v` — seq-cst store into Atomic<T> /
            // AtomicInteger<T>.  Must come before lowerPropertySet since Atomic<T>
            // has no user PropertyDecl and the fallthrough would silently drop
            // the write.
            if (tryLowerAtomicValueSet(*as)) return false;
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
                    // #813 / §15.1: an inline `[N]T?` element slot is a nullable-
                    // value struct `{i8, T}`; wrap the bare inner value so the
                    // hasValue byte is set (this fast path bypasses the general
                    // CG-nullable wrapping below).
                    builder_.CreateStore(
                        coerceToNullableSlot(elemTy2, rhsOnce), addr2);
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
            llvm::Value* addr = nullptr;
            llvm::Value* rhs = nullptr;
            // #736: a whole-array collection-literal assignment
            // `target := [e0, e1, ...]` to an inline-array lvalue (field or
            // local). A CollectionLiteralExpr is not a general expression value
            // (lowerExpr returns null for it), so this store was silently dropped
            // and the array stayed zero-initialized — field-init and local-decl
            // initializers special-case it, but the assignment statement did not.
            // Mirror the local-decl element-store loop (LlvmBackend_Setup.cpp
            // ~4281). Heap collections (Set/Array/Dict) have `ptr` slots, not an
            // ArrayType elemTy, so the dyn_cast guard excludes them.
            if (as->op == ":=") {
                if (auto* cl = dynamic_cast<const ast::CollectionLiteralExpr*>(
                        as->value.get())) {
                    if (!cl->isDict) {
                        llvm::Type* aelemTy = nullptr;
                        llvm::Value* aaddr = lvalueAddr(*as->target, aelemTy);
                        if (auto* aty = aelemTy
                                ? llvm::dyn_cast<llvm::ArrayType>(aelemTy)
                                : nullptr) {
                            if (aaddr) {
                                llvm::Type* et = aty->getElementType();
                                for (std::size_t i = 0;
                                     i < cl->elements.size() &&
                                     i < aty->getNumElements(); ++i) {
                                    if (!cl->elements[i]) continue;
                                    llvm::Value* ev =
                                        lowerExpr(*cl->elements[i]);
                                    if (!ev) {
                                        // #1386: ICE — lowerExpr returned null
                                        // for an inline-array assignment element;
                                        // the expression kind is unhandled.
                                        ctx_.diagnostics().report(
                                            Severity::Error,
                                            cl->elements[i]->span.begin,
                                            "internal: lowerExpr returned null "
                                            "for inline-array assignment element"
                                            " — #1386");
                                        continue;
                                    }
                                    if (ev->getType() != et &&
                                        ev->getType()->isIntegerTy() &&
                                        et->isIntegerTy()) {
                                        unsigned eb =
                                            ev->getType()->getIntegerBitWidth();
                                        unsigned db = et->getIntegerBitWidth();
                                        ev = db > eb
                                                 ? builder_.CreateSExt(ev, et)
                                                 : builder_.CreateTrunc(ev, et);
                                    }
                                    llvm::Value* gep = builder_.CreateGEP(
                                        aty, aaddr,
                                        {builder_.getInt32(0),
                                         builder_.getInt32((unsigned)i)});
                                    // #813 / §15.1: wrap into the nullable-value
                                    // slot struct for an `[N]T?` literal assign.
                                    builder_.CreateStore(
                                        coerceToNullableSlot(et, ev), gep);
                                }
                                return false;  // handled
                            }
                        } else if (aaddr) {
                            // #820 / §15.4: a HEAP-collection target (`Array<T>`,
                            // `OwningArray<T>`, etc.) lowers to a bare `ptr` slot,
                            // not an inline ArrayType — the inline path above
                            // missed it, so `field := [1,2,3]` lowered the
                            // CollectionLiteralExpr (which yields null as a general
                            // value) and the store was silently dropped, leaving
                            // the slot null → crash on first use. Materialize a
                            // real heap collection and store the pointer, mirroring
                            // the field-initializer path (Setup.cpp ~4117).
                            const sema::TypeRef* tRt =
                                ctx_.exprType(*as->target);
                            if (tRt && isCollectionTypeName(tRt)) {
                                if (llvm::Value* arr =
                                        materializeHeapCollectionLiteral(cl,
                                                                         tRt)) {
                                    // #824 / §28.5 / §28.9: if target is an
                                    // `owned` collection CLASS field, dispose the
                                    // old collection before overwriting. Delegates
                                    // to lowerOwnedCollectionFieldStore which does
                                    // load-old / store-new / bz_array_free(old).
                                    // For locals / borrowed fields / non-owned
                                    // targets the helper returns false and we fall
                                    // through to the plain store below.
                                    if (!lowerOwnedCollectionFieldStore(
                                            *as->target, aaddr, arr))
                                        builder_.CreateStore(arr, aaddr);
                                    return false;  // handled
                                }
                            }
                        }
                    }
                }
            }
            // #579 / §27.5: a recursive lambda assigned to a local refers to
            // that very local in its body — `counter := (n) => ... counter(n-1)`.
            // Lowering the RHS lambda BOXES the captured `counter` slot: it
            // promotes the slot to a shared heap cell and REBINDS locals_[decl]
            // to the new cell-holder alloca (so the closure's env and all later
            // reads route through the cell). If we compute the LHS address FIRST
            // (the normal order below), lvalueAddr resolves `counter` to the
            // ORIGINAL un-boxed slot; the subsequent store writes the finalized
            // closure into that stale slot while the captured cell still holds
            // the pre-box garbage. The lambda then calls through the cell and
            // dereferences the garbage pointer → SIGSEGV.
            //
            // Fix: for a plain `:=` to a local/param identifier (an lvalue with
            // NO side effects of its own — the only "effect" is selecting the
            // storage slot, which does not depend on the RHS value), lower the
            // RHS FIRST so any capture-induced boxing/rebinding completes, then
            // resolve the LHS address through the (now-rebound) slot — which
            // boxedStorage routes to the shared cell. The store then reaches the
            // same cell the closure captured. Observationally identical to the
            // spec eval order for a side-effect-free identifier target.
            //
            // Restricted to `:=` (a plain store); compound `OP=` / `??=` keep
            // the address-then-RHS order so their read-modify-write reads the
            // current value first. Field / static-field / index targets keep the
            // natural left-to-right order (their LHS may have side effects, and
            // they cannot be the lambda being initialized for the first time).
            if (as->op == "??=") {
                // §7.7 (#1190): null-coalescing assignment short-circuits —
                // the RHS must be evaluated ONLY when the LHS is null. Compute
                // the target address now but DEFER `lowerExpr(*as->value)` into
                // the LHS-is-null branch handled immediately below, so a
                // non-null LHS never runs (nor produces) the RHS. Lowering it
                // eagerly here would violate short-circuit and, for an owned
                // RHS, leave an unobserved owned value (leak / double-free).
                addr = lvalueAddr(*as->target, elemTy);
                if (!addr) return false;
            } else if (as->op == ":=" &&
                dynamic_cast<const ast::IdentifierExpr*>(as->target.get()) &&
                localSlot(*as->target) != nullptr) {
                rhs = lowerExpr(*as->value);
                if (!rhs) return false;
                addr = lvalueAddr(*as->target, elemTy);
                if (!addr) return false;
            } else {
                addr = lvalueAddr(*as->target, elemTy);
                if (!addr) return false;
                rhs = lowerExpr(*as->value);
                if (!rhs) return false;
            }
            // §7.7 (#1190): null-coalescing assignment `x ??= e`. Mirror the
            // `??` binary operator's short-circuit (LlvmBackend_Exprs.cpp
            // lowerBinary "??"): null-test the LHS first and lower the RHS
            // inside the null branch ONLY. Self-contained — returns before the
            // `:=`-only coercion/field-store blocks below.
            if (as->op == "??=") {
                llvm::Value* cur = builder_.CreateLoad(elemTy, addr);
                llvm::Value* isNull = nullptr;
                if (cur->getType()->isPointerTy()) {
                    isNull = builder_.CreateICmpEQ(
                        cur, llvm::ConstantPointerNull::get(ptrTy()));
                } else if (cur->getType()->isStructTy()) {
                    // CG-nullable: nullable primitive {i8 hasValue; primType
                    // value}. Null when hasValue == 0.
                    isNull = builder_.CreateNot(nullablePrimHasValue(cur));
                }
                // A non-nullable target can never be null → nothing to do.
                if (!isNull) return false;
                auto* assignBB =
                    llvm::BasicBlock::Create(llvm_, "coaleq.do", currentFn_);
                auto* endBB =
                    llvm::BasicBlock::Create(llvm_, "coaleq.end", currentFn_);
                builder_.CreateCondBr(isNull, assignBB, endBB);
                builder_.SetInsertPoint(assignBB);
                // RHS evaluated here ONLY (the LHS was null) — short-circuit.
                rhs = lowerExpr(*as->value);
                if (!rhs) {
                    builder_.CreateBr(endBB);
                    builder_.SetInsertPoint(endBB);
                    return false;
                }
                // §5.1: numeric widening to the target slot (mirror the `:=`
                // path's coerceNumericToType, issue #408 / #421 / #410).
                if (elemTy)
                    rhs = coerceNumericToType(
                        rhs, elemTy, isUnsignedTargetExpr(*as->value));
                // CG-nullable: wrap a plain primitive RHS into the target's
                // {hasValue, value} struct (or normalize a null literal).
                if (auto* st = llvm::dyn_cast<llvm::StructType>(elemTy)) {
                    if (st->getNumElements() == 2 &&
                        st->getElementType(0) == nullablePrimHasValTy() &&
                        rhs->getType() != elemTy) {
                        if (rhs->getType()->isPointerTy() &&
                            llvm::isa<llvm::ConstantPointerNull>(rhs)) {
                            rhs = llvm::ConstantAggregateZero::get(elemTy);
                        } else if (rhs->getType() == st->getElementType(1)) {
                            llvm::Value* wrapped = builder_.CreateInsertValue(
                                llvm::UndefValue::get(elemTy),
                                llvm::ConstantInt::get(
                                    nullablePrimHasValTy(), 1), {0u});
                            rhs = builder_.CreateInsertValue(wrapped, rhs, {1u});
                        }
                    }
                }
                // #429 / §4.6 / §28.9: `field ??= e` stores into a (currently
                // null) CLASS string field/auto-property only when null, so
                // there is no old value to release — but the field still takes
                // an INDEPENDENT +1 (transfer a producer / retain a borrow),
                // released on the owner's disposal. Restricted to class string
                // field lvalues. The slot is null here, so a plain store is
                // correct after the retain.
                if (isStringFieldLvalue(*as->target) &&
                    rhs->getType()->isPointerTy() &&
                    !isPlusOneStringProducer(as->value.get()))
                    emitStringRetain(rhs);
                builder_.CreateStore(rhs, addr);
                builder_.CreateBr(endBB);
                builder_.SetInsertPoint(endBB);
                return false;
            }
            // §9 / §9.1 (#1759): `slot := this` in a record method stores the
            // whole `this` value into a record-typed slot. `this` lowered to the
            // receiver POINTER (CG29 / §9.2); load the struct so value semantics
            // copy the record rather than storing the 8-byte pointer as the
            // slot's leading bytes (garbage read back as the record's fields). A
            // no-op for every RHS that is not the receiver pointer, and for a
            // slot whose type is not exactly the receiver record's struct.
            if (as->op == ":=")
                rhs = coerceThisRecordValue(rhs, elemTy);
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
                // #824 / §28.5 / §28.9: a store into a CLASS `owned` collection
                // field (OwningArray<T>/OwningStack<T>/OwningQueue<T>/OwningSet<T>/
                // OwningLinkedList<T>/OwningBuffer<T>/OwningDictionary<T> etc.)
                // must dispose the old collection (and its owned elements) BEFORE
                // overwriting the pointer. Without this the old pointer is lost
                // and the old collection leaks. Mirrors the string/closure/box
                // field-store family above; bz_array_free/bz_llist_free are
                // null-safe so the first assignment (ctor-init, null old value)
                // is handled correctly without an explicit guard.
                if (!handled)
                    handled = lowerOwnedCollectionFieldStore(*as->target, addr,
                                                             rhs);
                // §28.7 (#1328): drop-on-overwrite for owned class/interface
                // FIELDS — the field analog of disposeDisplacedOwnedClassLocal.
                // Loads the old object pointer, applies null+identity guards (so a
                // moved-from slot or a self-assign never disposes), then dispatches
                // the old object's Dispose and reclaims its block. The new value is
                // stored after disposal. Must come AFTER the collection helper (which
                // handles OwningArray<T> etc. via bz_array_free, not emitDisposeAndReclaim)
                // and BEFORE the local helper (which only fires for plain locals).
                if (!handled)
                    handled = lowerOwnedClassFieldStore(*as->target, addr, rhs);
                if (!handled) {
                    // §28.7 (#1054): reassigning an owned class local must dispose
                    // the displaced old value before overwriting (the class analog
                    // of the string/closure/box/owned-collection field-store helpers
                    // above). No-op for a borrow local/param, an array element, a
                    // ref param, a static/record field, or a non-class target (none
                    // has an owned-class-local Disposal entry → no double-free).
                    disposeDisplacedOwnedClassLocal(as->target.get(), addr,
                                                    elemTy, rhs);
                    // #914: a narrowed value-nullable RHS (sema proved non-null)
                    // assigned to a non-nullable target — unwrap the {flag,value}
                    // struct to its value. No-op for a nullable target (elemTy is
                    // the struct, not its value element).
                    rhs = unwrapNarrowedNullable(rhs, elemTy);
                    llvm::StoreInst* si = builder_.CreateStore(rhs, addr);
                    // §22.12 rule 2 store atomicity (v0.181, #1556): a store into
                    // a static REFERENCE field is a single pointer-word write the
                    // runtime performs atomically — no torn reference is ever
                    // observed by a racing first-use on another domain (the check-
                    // then-store sequence itself is deliberately NOT atomic;
                    // §22.12). Emit it as an aligned atomic monotonic pointer store
                    // (relaxed: the spec promises no-tearing, not cross-domain
                    // ordering). Scope: any pointer-typed static field — the
                    // admitted mutable statics rule 2 targets, and incidentally the
                    // write-once readonly/owned reference statics (harmless, a
                    // single-word aligned atomic store is functionally identical).
                    // Primitive-scalar statics and every non-static target (locals,
                    // instance/record fields, ref params) keep the plain store.
                    if (rhs->getType()->isPointerTy() &&
                        staticFieldGlobal(*as->target)) {
                        si->setAlignment(module_->getDataLayout().getABITypeAlign(
                            rhs->getType()));
                        si->setAtomic(llvm::AtomicOrdering::Monotonic);
                    }
                    // §27.5 / #673: guarded-boxed (non-for-var) locals keep the
                    // original pre-boxing alloca in sync so pre-emitted code that
                    // loads from it (e.g. a while-loop condition) sees the update.
                    if (llvm::AllocaInst* ns = localSlot(*as->target))
                        syncCellToOrigSlot(ns, rhs);
                }
            } else {
                // fix #610 / §25.1 / §6.6: compound assignment `target OP= value`
                // on a record or class whose type declares a user-defined
                // `static operator <op>` — desugar mechanically to
                //   target := target.operator_op_(target, value)
                // (per §25.1: "the compound forms desugar mechanically to
                // read-modify-write using the underlying binary operator's
                // overload"). Must be checked BEFORE the built-in
                // emitCompoundOp path, which cannot handle struct operands and
                // would SIGABRT by passing the struct type to
                // llvm.sadd.with.overflow.
                std::string op(as->op);
                if (!op.empty() && op.back() == '=') op.pop_back();
                {
                    sema::MethodSymbol* opMethod = nullptr;
                    if (const sema::TypeRef* targetTy =
                            ctx_.exprType(*as->target)) {
                        if (targetTy->kind == sema::TypeRef::Kind::Named &&
                            targetTy->declaration) {
                            for (sema::Symbol* m :
                                 targetTy->declaration->members) {
                                auto* ms =
                                    dynamic_cast<sema::MethodSymbol*>(m);
                                if (ms && ms->isOperator &&
                                    ms->operatorToken == op &&
                                    ms->params.size() == 2) {
                                    opMethod = ms;
                                    break;
                                }
                            }
                        }
                    }
                    if (opMethod) {
                        // §25.1: dispatch to user-defined binary operator.
                        // Load the current value (struct by value) and call
                        // the overload, then store the result back.
                        llvm::Value* cur = builder_.CreateLoad(elemTy, addr);
                        std::vector<llvm::Value*> args{cur, rhs};
                        llvm::Value* res = emitMethodCall(
                            opMethod, args, /*baseQualified=*/false);
                        if (res) {
                            builder_.CreateStore(res, addr);
                            // §27.5 / #673: sync compound-assignment result back
                            // to the original alloca for guarded-boxed locals.
                            if (llvm::AllocaInst* ns = localSlot(*as->target))
                                syncCellToOrigSlot(ns, res);
                        }
                        return false;
                    }
                }
                // §4.6 (#857): string compound concatenation `s += t`. A string
                // is a pointer, not a number — emitCompoundOp below would feed the
                // string pointers to the integer checked-add intrinsic (malformed
                // IR). When the target is a string, `+=` is a concat: mirror the
                // working `s := s + t` path (emitStringConcat + the `:=`
                // retain/release discipline). A non-string RHS (`s += 5`) is
                // converted via the §4.6 `string + T` rule. The concat result is a
                // +1 producer, transferred into a string LOCAL/FIELD slot with the
                // old value released (other lvalues — array element, static, ref/
                // out, `string?` — get a plain store, leak-safe as before).
                if (op == "+") {
                    const sema::TypeRef* tgtTy = ctx_.exprType(*as->target);
                    if (tgtTy &&
                        tgtTy->kind == sema::TypeRef::Kind::Primitive &&
                        tgtTy->primitive == sema::PrimitiveType::String) {
                        llvm::Value* curS = builder_.CreateLoad(elemTy, addr);
                        llvm::Value* rStr = rhs;
                        const sema::TypeRef* rtv = ctx_.exprType(*as->value);
                        if (rtv &&
                            rtv->kind == sema::TypeRef::Kind::Primitive &&
                            rtv->primitive != sema::PrimitiveType::String)
                            rStr = emitPrimitiveToString(rhs, rtv->primitive);
                        llvm::Value* res = emitStringConcat(curS, rStr);
                        // §4.6: a non-string RHS was rendered to a fresh `rStr`
                        // (emitPrimitiveToString, rc=1) — release it after the
                        // concat borrows it.
                        if (rStr != rhs) emitStringRelease(rStr);
                        // #1481 / §4.6 / §28.9: a STRING-typed RHS that is itself
                        // a +1 PRODUCER (`i.ToString()`, a call returning string,
                        // a nested `+`/interpolation, `string.Join`, a
                        // `() -> string` closure call) has `rStr == rhs` and hands
                        // us its single +1 reference; the concat above borrows it,
                        // so release it or it leaks — unboundedly in a loop
                        // (`while … do s += i.ToString(); end`). A borrow RHS (a
                        // named local/field, §28.9) is not a producer and is left
                        // untouched. (Mirrors the string `+` operand discipline.)
                        else if (isPlusOneStringProducer(as->value.get()))
                            emitStringRelease(rhs);
                        bool isLocal = localSlot(*as->target) &&
                                       elemTy->isPointerTy() &&
                                       !tgtTy->nullable &&
                                       currentFn_ == stringReleaseEligibleFn_;
                        bool isField = isStringFieldLvalue(*as->target);
                        builder_.CreateStore(res, addr);
                        if (isLocal || isField)
                            emitStringRelease(curS);  // release the old value
                        if (llvm::AllocaInst* ns = localSlot(*as->target))
                            syncCellToOrigSlot(ns, res);
                        return false;
                    }
                }
                // §6.4 (sweep #6): decimal compound assignment `d += x` / `-=` /
                // `*=` / `/=`. `decimal` is a Kind::Primitive struct, so the
                // user-operator guard above (which requires Kind::Named) misses
                // it; feeding the {i32,i32,i32,i32} struct to the integer
                // emitCompoundOp below emits an integer overflow intrinsic on a
                // struct (IR_FAIL; a `/=` SIGABRTs in LLVM). Route through the
                // decimal arithmetic helper instead.
                if (elemTy == decimalStructType()) {
                    llvm::Value* cur = builder_.CreateLoad(elemTy, addr);
                    if (llvm::Value* res = emitDecimalArith(op, cur, rhs)) {
                        builder_.CreateStore(res, addr);
                        if (llvm::AllocaInst* ns = localSlot(*as->target))
                            syncCellToOrigSlot(ns, res);
                    }
                    return false;
                }
                // CG31: built-in numeric compound assignment — read,
                // apply OP (overflow-checked like plain arithmetic),
                // write back.
                llvm::Value* cur = builder_.CreateLoad(elemTy, addr);
                llvm::Value* res = emitCompoundOp(
                    op, cur, rhs, elemTy->isFloatingPointTy(),
                    isUnsignedTargetExpr(*as->target));
                if (res) {
                    builder_.CreateStore(res, addr);
                    // §27.5 / #673: sync back to original alloca.
                    if (llvm::AllocaInst* ns = localSlot(*as->target))
                        syncCellToOrigSlot(ns, res);
                }
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
    //
    // fix #587 / §20.5: `++`/`--` on a computed property or user-defined indexer
    // slot must call the setter to write the incremented value back.
    // The lvalueAddr fallback returns null for these targets (no backing alloca),
    // so the store was silently dropped. After a null from lvalueAddr two additional
    // paths handle the write-back:
    //   (a) Block-setter computed property (`obj.P++`): mirrors lowerPropertyCompound
    //       — read via getter (lowerExpr), compute ±1, write via lowerPropertySetValue.
    //   (b) User-defined indexer (`v[i]++`): mirrors lowerIndexerCompound — evaluate
    //       receiver and index once, call getter, compute ±1, call setter.
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
                    // #1219 (per #1059 / §25.2 / §14 / §9.5): operator ++/-- on a
                    // GENERIC record is a static method whose parameter and return
                    // types reference the declaring type's type parameter T.  Set
                    // up the per-instantiation substitution from the target's
                    // concrete generic args (e.g. Box<string> → T=string) so
                    // emitMethodCall specializes the operator, emitting/calling the
                    // specialized form whose param type matches the already-
                    // specialized struct (record.Box$spec0).  Without this the
                    // unspecialized bz$Box$op$++ (param type record.Box) is called
                    // with a record.Box$spec0 argument → LLVM IR verifier:
                    // "Call parameter type does not match function signature".
                    auto savedOpSubst = genericSubst_;
                    auto savedOpSema  = genericSubstSemaTy_;
                    if (const sema::TypeRef* tgtTy =
                            ctx_.exprType(*inc->target)) {
                        if (tgtTy->kind == sema::TypeRef::Kind::Named &&
                            tgtTy->declaration == opMethod->parent &&
                            !tgtTy->genericArgs.empty()) {
                            auto* td = dynamic_cast<const ast::TypeDecl*>(
                                tgtTy->declaration->decl);
                            if (td && td->genericParams.size() ==
                                          tgtTy->genericArgs.size()) {
                                std::unordered_map<std::string_view,
                                                   llvm::Type*> ns;
                                std::unordered_map<std::string_view,
                                    const sema::TypeRef*> nsem;
                                for (std::size_t gi = 0;
                                     gi < td->genericParams.size(); ++gi) {
                                    ns[td->genericParams[gi].name] =
                                        lowerTypeRef(tgtTy->genericArgs[gi]);
                                    nsem[td->genericParams[gi].name] =
                                        resolveTypeParamRef(
                                            tgtTy->genericArgs[gi]);
                                }
                                genericSubst_       = std::move(ns);
                                genericSubstSemaTy_ = std::move(nsem);
                            }
                        }
                    }
                    llvm::Value* res =
                        emitMethodCall(opMethod, args, /*baseQualified=*/false);
                    genericSubst_       = std::move(savedOpSubst);
                    genericSubstSemaTy_ = std::move(savedOpSema);
                    if (res) {
                        // §28.7 / §17.6 (#1702): `c++` ≡ `c := T.operator++(c)` —
                        // the store must drop-on-overwrite (dispose the displaced
                        // value). For an owned class/interface FIELD, route through
                        // lowerOwnedClassFieldStore — the field analog of the local
                        // disposal below — which self-contains load-old /
                        // null+identity-guard / dispose-old / store-new (§17.6: the
                        // operator result is produced first, then the displaced
                        // previous value is disposed implicitly, then the new value
                        // is stored). Without this the displaced field value leaked
                        // (the plain path stored only via the local branch, which is
                        // a no-op for a field). Returns false for non-owned-field
                        // targets (locals, ref params, static/record/borrow fields),
                        // which fall through to the LOCAL path below. Mirrors the
                        // plain-assignment path's lowerOwnedClassFieldStore call.
                        if (!lowerOwnedClassFieldStore(*inc->target, addr, res)) {
                            // §28.7 (#913): reassigning an owned class LOCAL disposes
                            // the displaced old value first (the operator already
                            // borrowed `cur` above, so it is safe to dispose now,
                            // before the store). No-op for a borrow local/param (no
                            // disposal entry → no double-free).
                            disposeDisplacedOwnedClassLocal(inc->target.get(), addr,
                                                            elemTy, res);
                            builder_.CreateStore(res, addr);
                        }
                        // §27.5 / #673: sync to original alloca (a no-op for a field
                        // target — localSlot returns null for a non-local).
                        if (llvm::AllocaInst* ns = localSlot(*inc->target))
                            syncCellToOrigSlot(ns, res);
                    }
                } else if (elemTy == decimalStructType()) {
                    // §6.4 (sweep #6): decimal `d++` / `d--`. decimal is a
                    // Kind::Primitive struct; the integer path below would build
                    // ConstantInt of a struct type and feed it to an integer
                    // overflow intrinsic (IR_FAIL). Compute `d ± 1m` via the
                    // decimal arithmetic helper.
                    llvm::Value* cur = builder_.CreateLoad(elemTy, addr);
                    if (llvm::Value* res = emitDecimalArith(
                            inc->op == "++" ? "+" : "-", cur, emitDecimalOne())) {
                        builder_.CreateStore(res, addr);
                        if (llvm::AllocaInst* ns = localSlot(*inc->target))
                            syncCellToOrigSlot(ns, res);
                    }
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
                    if (res) {
                        builder_.CreateStore(res, addr);
                        // §27.5 / #673: sync to original alloca.
                        if (llvm::AllocaInst* ns = localSlot(*inc->target))
                            syncCellToOrigSlot(ns, res);
                    }
                }
            } else {
                // fix #587 / §20.5: lvalueAddr returned null — the target is either
                // a built-in heap-collection indexer, a computed property with a
                // block setter, or a user-defined indexer.  All require a getter
                // read + arithmetic + setter write-back.
                const std::string binOp = (inc->op == "++") ? "+" : "-";
                // --- (c) Built-in heap-collection indexer: `arr[i]++` / `dict[k]++`
                // fix #613 / §25.1 / §4.4 / §4.8: Array<T> and Dictionary<K,V>
                // elements are accessed via bz_array_get/bz_dict_try_get_* (no
                // PropertyDecl, so findIndexer returns null and branch (b) never
                // fires).  Read via lowerCollectionIndexerGet, compute ±1, write
                // back via lowerCollectionIndexerSet — same encode/decode round-trip
                // as the plain `arr[i] := v` path.  Checked first so that (a)/(b)
                // are only reached for non-collection IndexExpr targets.
                if (lowerCollectionIndexerIncrDecr(*inc)) {
                    // handled: write-back emitted by lowerCollectionIndexerIncrDecr
                // --- (a) Block-setter computed property: `obj.P++` / `obj.P--` ---
                // mirrors lowerPropertyCompound (§22.9): read via getter expression,
                // compute new value, write via setter body.
                } else if (computedSetterOf(*inc->target)) {
                    llvm::Value* cur = lowerExpr(*inc->target);  // getter read
                    if (cur) {
                        bool fp = cur->getType()->isFloatingPointTy();
                        llvm::Value* one =
                            fp ? llvm::cast<llvm::Value>(
                                     llvm::ConstantFP::get(cur->getType(), 1.0))
                               : llvm::cast<llvm::Value>(
                                     llvm::ConstantInt::get(cur->getType(), 1));
                        llvm::Value* res = emitCompoundOp(
                            binOp, cur, one, fp,
                            isUnsignedTargetExpr(*inc->target));
                        if (res) lowerPropertySetValue(*inc->target, res);
                    }
                // --- (b) User-defined indexer: `v[i]++` / `v[i]--` ---
                // mirrors lowerIndexerCompound (§20 / §23.4): evaluate receiver and
                // index once, call getter, compute new value, call setter.
                } else if (auto* ix =
                               dynamic_cast<const ast::IndexExpr*>(
                                   inc->target.get())) {
                    if (ix->target && !ix->indices.empty() &&
                        ix->indices[0].value) {
                        auto* targetTy2 = ctx_.exprType(*ix->target);
                        if (targetTy2 &&
                            targetTy2->kind == sema::TypeRef::Kind::Named &&
                            targetTy2->declaration) {
                            auto* ps = selectIndexer(targetTy2->declaration,
                                                     *ix);  // per §23.4 (#1172)
                            auto* pd = ps ? dynamic_cast<const ast::PropertyDecl*>(
                                               ps->decl) : nullptr;
                            if (ps && pd) {
                                // Locate getter and setter bodies.
                                const ast::Expr* getterExpr = nullptr;
                                const ast::PropertyAccessor* getterBlock = nullptr;
                                if (pd->bodyKind ==
                                        ast::PropertyBodyKind::Expression &&
                                    pd->exprBody)
                                    getterExpr = pd->exprBody.get();
                                for (const auto& a : pd->accessors) {
                                    if (!a.isSet) {
                                        if (a.bodyKind ==
                                            ast::PropertyAccessorBodyKind::Block)
                                            getterBlock = &a;
                                        else if (a.exprBody)
                                            getterExpr = a.exprBody.get();
                                        break;
                                    }
                                }
                                const ast::Expr* setterExpr = nullptr;
                                const std::vector<std::unique_ptr<ast::Stmt>>*
                                    setterStmts = nullptr;
                                for (const auto& a : pd->accessors) {
                                    if (a.isSet) {
                                        if (a.exprBody)
                                            setterExpr = a.exprBody.get();
                                        else if (!a.stmts.empty())
                                            setterStmts = &a.stmts;
                                        break;
                                    }
                                }
                                if ((getterExpr || getterBlock) &&
                                    (setterExpr || setterStmts)) {
                                    // Evaluate receiver once.
                                    llvm::Value* obj = lowerExpr(*ix->target);
                                    if (obj) {
                                        // Spill each index param once; reused by
                                        // both getter and setter to avoid double
                                        // evaluation (§23.4 single-evaluation).
                                        std::vector<llvm::AllocaInst*> idxSlots;
                                        idxSlots.reserve(ps->indexerParams.size());
                                        bool ok = true;
                                        for (std::size_t k = 0;
                                             k < ps->indexerParams.size() &&
                                             k < ix->indices.size(); ++k) {
                                            if (!ix->indices[k].value) {
                                                idxSlots.push_back(nullptr);
                                                ok = false; break;
                                            }
                                            llvm::Value* av =
                                                lowerExpr(*ix->indices[k].value);
                                            if (!av) {
                                                idxSlots.push_back(nullptr);
                                                ok = false; break;
                                            }
                                            auto* sl = entryAlloca(av->getType());
                                            builder_.CreateStore(av, sl);
                                            idxSlots.push_back(sl);
                                        }
                                        if (ok) {
                                            // --- Call getter ---
                                            llvm::Value* cur = nullptr;
                                            if (getterBlock) {
                                                llvm::Value* objPtr = obj;
                                                if (!objPtr->getType()
                                                         ->isPointerTy()) {
                                                    auto* tmp = entryAlloca(
                                                        obj->getType());
                                                    builder_.CreateStore(obj, tmp);
                                                    objPtr = tmp;
                                                }
                                                std::vector<llvm::Value*> cArgs;
                                                cArgs.push_back(objPtr);
                                                for (auto* sl : idxSlots) {
                                                    if (!sl) { ok = false; break; }
                                                    cArgs.push_back(
                                                        builder_.CreateLoad(
                                                            sl->getAllocatedType(),
                                                            sl));
                                                }
                                                if (ok) {
                                                    llvm::Function* fn =
                                                        ensureIndexerBlockGetter(
                                                            pd, getterBlock, ps);
                                                    cur = builder_.CreateCall(
                                                        fn->getFunctionType(), fn,
                                                        cArgs);
                                                    emitPendingCheck();
                                                }
                                            } else {
                                                llvm::Value* savedThis =
                                                    currentThis_;
                                                auto savedLocals = locals_;
                                                auto savedParams = paramSlots_;
                                                auto savedSubst =
                                                    setupReceiverGenericSubst(
                                                        ix->target.get(),
                                                        ps->parent);
                                                currentThis_ = obj;
                                                locals_.clear();
                                                paramSlots_.clear();
                                                for (std::size_t k = 0;
                                                     k < ps->indexerParams.size() &&
                                                     k < idxSlots.size(); ++k) {
                                                    if (!idxSlots[k]) continue;
                                                    paramSlots_[ps->indexerParams[k]] =
                                                        idxSlots[k];
                                                }
                                                cur = lowerExpr(*getterExpr);
                                                currentThis_ = savedThis;
                                                locals_ = std::move(savedLocals);
                                                paramSlots_ =
                                                    std::move(savedParams);
                                                genericSubst_ =
                                                    std::move(savedSubst);
                                            }
                                            // --- Compute ±1 and call setter ---
                                            if (cur) {
                                                bool fp =
                                                    cur->getType()
                                                        ->isFloatingPointTy();
                                                llvm::Value* one =
                                                    fp ? llvm::cast<llvm::Value>(
                                                             llvm::ConstantFP::get(
                                                                 cur->getType(),
                                                                 1.0))
                                                       : llvm::cast<llvm::Value>(
                                                             llvm::ConstantInt::get(
                                                                 cur->getType(), 1));
                                                llvm::Value* newVal =
                                                    emitCompoundOp(
                                                        binOp, cur, one, fp,
                                                        isUnsignedTargetExpr(
                                                            *inc->target));
                                                if (newVal) {
                                                    // --- Call setter ---
                                                    llvm::Value* savedThis =
                                                        currentThis_;
                                                    auto savedLocals = locals_;
                                                    auto savedParams = paramSlots_;
                                                    currentThis_ = obj;
                                                    locals_.clear();
                                                    paramSlots_.clear();
                                                    for (std::size_t k = 0;
                                                         k < ps->indexerParams
                                                                 .size() &&
                                                         k < idxSlots.size();
                                                         ++k) {
                                                        if (!idxSlots[k]) continue;
                                                        paramSlots_[ps->indexerParams[k]] =
                                                            idxSlots[k];
                                                    }
                                                    auto* vslot =
                                                        entryAlloca(
                                                            newVal->getType());
                                                    builder_.CreateStore(newVal,
                                                                         vslot);
                                                    locals_[pd] = vslot;
                                                    if (setterExpr)
                                                        lowerExpr(*setterExpr);
                                                    else if (setterStmts)
                                                        lowerScopedBlock(
                                                            *setterStmts);
                                                    currentThis_ = savedThis;
                                                    locals_ =
                                                        std::move(savedLocals);
                                                    paramSlots_ =
                                                        std::move(savedParams);
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        return false;
    }
    if (auto* rs = dynamic_cast<const ast::ReturnStmt*>(&s)) {
        // §22.9 (#739): a `return;` inside an INLINED property setter body ends
        // the setter (a void accessor), not the enclosing method — branch to the
        // accessor-end block rather than emitting a function return. (A setter is
        // void, so a value-bearing return cannot validly occur here.)
        if (inlinedAccessorEnd_ && !rs->value) {
            builder_.CreateBr(inlinedAccessorEnd_);
            return true;
        }
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
        // §30.5 / #1332 (v0.162): detect `return await <call>` with a pointer-typed
        // result inside an async method — Pattern A chained return. If detected, set
        // asyncChainedReturn_ so lowerAwait does NOT free the source task immediately;
        // lowerStmt will chain the source task to the outer task after the await.
        // Guard: no finally in flight (the finally path's emitAsyncComplete call would
        // need separate chaining, deferred; in that case fall back to normal free).
        // Guard: asyncResultTy_ is a pointer type (class/string, not int/bool/struct).
        if (rs->value && smKind_ == SuspendKind::Await && smStateStruct_ &&
            asyncResultTy_ && asyncResultTy_->isPointerTy() && finallyStack_.empty()) {
            if (const auto* awaitUE =
                    dynamic_cast<const ast::UnaryExpr*>(rs->value.get())) {
                if (awaitUE->op == "await" &&
                    dynamic_cast<const ast::CallExpr*>(awaitUE->operand.get()) !=
                        nullptr) {
                    asyncChainedReturn_ = true;
                }
            }
        }
        llvm::Value* v = nullptr;
        if (rs->value) {
            if (auto* cl = dynamic_cast<const ast::CollectionLiteralExpr*>(
                    rs->value.get())) {
                if (currentReturnTypeNode_) {
                    if (const sema::TypeRef* rrt =
                            ctx_.resolvedType(*currentReturnTypeNode_)) {
                        // §15.3 (#781): a `return [e1, …]` whose declared return
                        // type is an INLINE array (`[N owned]T?` / `[N]T`) builds
                        // the `[N x elem]` aggregate and returns it BY VALUE.
                        // lowerExpr returns null for a CollectionLiteralExpr, and
                        // materializeHeapCollectionLiteral only makes a heap
                        // bz_array_t* (a `ptr`, the wrong type for an inline
                        // return) — so without this the method returned an
                        // undef/zero array and the caller's `var a := Make()`
                        // materialized as all-null. Mirrors the inline-array
                        // local-decl build (lowerLocalDecl).
                        if (rrt->kind == sema::TypeRef::Kind::InlineArray &&
                            !cl->isDict) {
                            if (auto* aty = llvm::dyn_cast<llvm::ArrayType>(
                                    lowerTypeRef(rrt))) {
                                llvm::Type* elemTy = aty->getElementType();
                                llvm::AllocaInst* tmp = entryAlloca(aty);
                                for (std::size_t i = 0; i < cl->elements.size();
                                     ++i) {
                                    if (!cl->elements[i]) continue;
                                    llvm::Value* ev = lowerExpr(*cl->elements[i]);
                                    if (!ev) {
                                        // #1386: ICE — lowerExpr returned null
                                        // for an inline-array return element.
                                        ctx_.diagnostics().report(
                                            Severity::Error,
                                            cl->elements[i]->span.begin,
                                            "internal: lowerExpr returned null "
                                            "for inline-array return element"
                                            " — #1386");
                                        continue;
                                    }
                                    if (ev->getType() != elemTy &&
                                        ev->getType()->isIntegerTy() &&
                                        elemTy->isIntegerTy()) {
                                        unsigned eb =
                                            ev->getType()->getIntegerBitWidth();
                                        unsigned db =
                                            elemTy->getIntegerBitWidth();
                                        ev = db > eb
                                                 ? builder_.CreateSExt(ev, elemTy)
                                                 : builder_.CreateTrunc(ev,
                                                                        elemTy);
                                    }
                                    llvm::Value* gep = builder_.CreateGEP(
                                        aty, tmp,
                                        {builder_.getInt32(0),
                                         builder_.getInt32((int)i)});
                                    builder_.CreateStore(ev, gep);
                                }
                                v = builder_.CreateLoad(aty, tmp, "ret.inlarr");
                            }
                        } else if (!cl->isDict)
                            v = materializeHeapCollectionLiteral(cl, rrt);
                        else if (!cl->keys.empty())
                            v = materializeHeapDictLiteral(cl, rrt);
                    }
                }
            }
            if (!v) v = lowerExpr(*rs->value);
        }
        // §30.5 / #1332 (v0.162): read and reset the chained-return flag now that
        // lowerExpr has completed (lowerAwait already used the flag to suppress the
        // source-task free; we must reset before any possible re-entry).
        bool chainedReturn = asyncChainedReturn_;
        asyncChainedReturn_ = false;
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
            // #776 / §30.5: when the async result is a bare local `Task`/`Task<T>`
            // being TRANSFERRED out (e.g. `return t;` in a `Task<Task<T>>` method),
            // null its slot before scope-end disposal. `v` already holds the task
            // pointer; without nulling, emitAllDisposals fire-and-forgets the very
            // task being returned, so the caller's `await` on the returned task
            // hangs (its completion was consumed by the scope-end fire-and-forget).
            if (v && rs->value) {
                if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(
                        rs->value.get())) {
                    const sema::TypeRef* vt = ctx_.exprType(*id);
                    bool isTaskLocal =
                        vt && vt->kind == sema::TypeRef::Kind::Named &&
                        !vt->nullable && vt->astNode && vt->astNode->name &&
                        !vt->astNode->name->parts.empty() &&
                        (vt->headName() == "Task" ||
                         vt->headName() == "OwningTask");
                    if (isTaskLocal) {
                        llvm::Type* sty = nullptr;
                        if (llvm::Value* slot = lvalueAddr(*id, sty))
                            builder_.CreateStore(
                                llvm::ConstantPointerNull::get(ptrTy()), slot);
                    }
                }
            }
            // #721 C1 / §30.9: a `return` exiting a try-with-finally must run the
            // finally first. Stash the unwrapped result and route through the
            // active finally (exit mode 5); the post-finally dispatch (lowerTry)
            // performs emitAsyncComplete + RetVoid. Without this the finally is
            // silently skipped on the async-return edge.
            // fix #1567 / §29.7/§28.10 (v0.182): the async-method return edge
            // obeys the same reverse-establishment rule as the sync return edge
            // above — dispose ONLY the try-body scopes (>= the finally's entry
            // depth) here; the mode-5 dispatch (lowerTry) disposes the enclosing
            // scopes AFTER the finally runs. The pre-v0.182 emitAllDisposals()
            // before the finally freed an enclosing `using var` before a finally
            // that touched it, a use-after-dispose.
            if (!finallyStack_.empty()) {
                auto& fr = finallyStack_.back();
                emitDisposalsInRange(fr.disposeDepthAtEntry, disposeStack_.size());
                if (v && fr.asyncRetSlot)
                    builder_.CreateStore(v, fr.asyncRetSlot);
                builder_.CreateStore(builder_.getInt32(5), fr.exitModSlot);
                builder_.CreateBr(fr.finNormalBB);
                return true;
            }
            // No enclosing finally: dispose all live scopes, then complete.
            emitAllDisposals();
            // §30.5 / #1332 (v0.162): Pattern A chained return — chain the source
            // task (set by emitAwaitSuspend into asyncPendingTaskField_ and NOT freed
            // because asyncChainedReturn_ suppressed bz_task_free) to the outer task
            // BEFORE bz_task_complete. This ordering is required: bz_task_complete
            // notifies continuations which may immediately free the outer task on a
            // racing thread; calling bz_task_set_chained AFTER complete would be a
            // UAF on the outer task pointer.
            if (chainedReturn) {
                // Load source task from the pending-task frame slot (still alive:
                // freeTaskAfter=false kept it from being reclaimed in emitAwaitSuspend).
                llvm::Value* pendField = builder_.CreateStructGEP(
                    smStateStruct_, smSelf_, asyncPendingTaskField_);
                llvm::Value* sourceTask =
                    builder_.CreateLoad(ptrTy(), pendField, "chain.src");
                // Load outer task pointer (set at async-method entry).
                llvm::Value* otField = builder_.CreateStructGEP(
                    smStateStruct_, smSelf_, asyncOuterTaskField_);
                llvm::Value* outerTask =
                    builder_.CreateLoad(ptrTy(), otField, "chain.outer");
                // Transfer source-task lifetime ownership to the outer task's chained
                // slot: bz_task_free(outer) will recursively free source when done.
                auto* setChainedFn = declareC(
                    "bz_task_set_chained", llvm::Type::getVoidTy(llvm_),
                    {ptrTy(), ptrTy()});
                builder_.CreateCall(setChainedFn, {outerTask, sourceTask});
            } else if (v && asyncPatternAResultDisposer_) {
                // §30.5 / #1503: Pattern A (Task<T:class>) NON-chained return —
                // the async body produced a FRESH owned value, so the task owns
                // the result directly (§30.5: "the task owns the result and its
                // destructor disposes it directly"). Attach the per-T disposer
                // to the outer task BEFORE bz_task_complete, same race-safety
                // ordering as the chained-slot branch above (a continuation may
                // free the task the instant it completes). bz_task_free
                // consults this field whenever it eventually runs — whether the
                // task is already settled at that point (an already-done `using
                // var` scope-end / anonymous-await reclaim) or still running
                // (the fire-and-forget completion continuation) — so both the
                // "already done" and "still running at disposal time" cases in
                // §30.5's "Disposing an unconsumed OwningTask<T>" text (which
                // applies here by the same logic, even though that subsection
                // is written for Pattern B) are covered by one mechanism. A
                // borrow taken via a later `await`/`.Result` read does NOT
                // consume/null the slot for Pattern A (only Pattern B's
                // bz_task_take_result does that), so the result is disposed
                // exactly once whether or not it was ever borrowed.
                llvm::Value* otField = builder_.CreateStructGEP(
                    smStateStruct_, smSelf_, asyncOuterTaskField_);
                llvm::Value* outerTask =
                    builder_.CreateLoad(ptrTy(), otField, "patterna.outer");
                auto* setDisposerFn = declareC(
                    "bz_task_set_result_disposer", llvm::Type::getVoidTy(llvm_),
                    {ptrTy(), ptrTy()});
                builder_.CreateCall(setDisposerFn,
                                    {outerTask, asyncPatternAResultDisposer_});
            }
            emitAsyncComplete(v);
            builder_.CreateRetVoid();
            return true;
        }
        // §8.6: box a value-typed `return expr` whose declared return type is
        // Object/interface, so the caller's is/as/virtual dispatch sees a box.
        if (v && rs->value && currentReturnTypeNode_)
            v = boxForTarget(v, ctx_.resolvedType(*currentReturnTypeNode_),
                             rs->value.get());
        // §29.2 / §4.3 (#1310): a value returned as `IEnumerable<T>` must be a
        // vtable-backed enumerator, not a raw bz_array_t* / class instance — the
        // caller's for-in and its scope-end iterEnumDispose both dispatch through
        // the value's vtable (a raw array → SIGSEGV). Wrap it here as the
        // call-argument and local-decl init paths already do. #1916: pass
        // ownLazyPipeline=true — a lazy LINQ pipeline returned as IEnumerable<T>
        // is drained into an OWNING enumerator (the caller owns and disposes the
        // returned value, so an owning wrapper is leak-free).
        if (v && rs->value && currentReturnTypeNode_)
            v = wrapAsIEnumerableForTarget(
                v, ctx_.resolvedType(*currentReturnTypeNode_), rs->value.get(),
                /*ownLazyPipeline=*/true);
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
        // fix #568 / §14.1 / §7.1: when the declared return type is a nullable
        // VALUE type whose concrete form is reached only through the active
        // monomorphization (`T?` with T = a value type — prim/enum/record), the
        // function return type is the tagged {i8 hasValue; T} struct (lowerTypeRef
        // above), but `return null` / `return <bare T value>` produce a null ptr /
        // a bare value. Wrap them into the nullable struct here, where the sema
        // return type is in scope — emitReturn's coercion only matched exact LLVM
        // value types and missed the record/enum payload (specialized struct ≠
        // the param's bare struct → IR-verify failure). A non-generic nullable
        // value return already lowers correctly through the same struct, so this
        // is a no-op there (v already IS the struct).
        if (v && currentReturnTypeNode_) {
            if (const sema::TypeRef* declRet =
                    ctx_.resolvedType(*currentReturnTypeNode_)) {
                // #1767 / §19.2: the returned expression's own sema type is a
                // nullable REFERENCE (`Object?`) — e.g. an all-null `case … end`,
                // whose `null` arms default to `Object?` — while the declared
                // return is a nullable VALUE (`int?`). Such a value lowers to a
                // bare `ptr` (a runtime PHI, not a constant null), which the
                // reference→value coercion below rewrites to the {i8 0, T 0} null
                // constant (sema proves it null, §20). Passing this flag keeps a
                // value-nullable source that mis-lowered to a ptr from being
                // silently nulled (it must still fail the verifier).
                const sema::TypeRef* srcTy =
                    rs->value ? ctx_.exprType(*rs->value) : nullptr;
                bool srcNullableRef = srcTy && srcTy->nullable &&
                                      !isNullableValueTy(srcTy);
                v = coerceReturnToNullableValue(
                    v, declRet,
                    rs->value && isUnsignedTargetExpr(*rs->value),  // #843
                    srcNullableRef);
            }
        }
        // §5.1 (#746 R41-H): a numeric return value narrower than the declared
        // return type is implicitly widened (e.g. `return arr[0]` where
        // arr: Array<byte> in an int-returning method, or `return aByte`).
        // Without this the bare i8/i16 was `ret`'d into a wider integer function
        // (IR-verify failure). coerceNumericToType is a no-op for ptr/struct
        // returns and for an already-matching width.
        if (v && rs->value && currentFn_ &&
            !currentFn_->getReturnType()->isVoidTy())
            v = coerceNumericToType(v, currentFn_->getReturnType(),
                                    isUnsignedTargetExpr(*rs->value));
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
                        [retSlot](const auto& e) {
                            // #1418 / §27.5 / §28.9: if the returned local is a
                            // lambda-CAPTURED value local, its slot is the shared
                            // capture-cell holder. Returning it yields a COPY of the
                            // cell's contents (a value type), NOT ownership of the
                            // cell — so the cell's boxedCellFree must still fire.
                            // Only the ownership-transfer disposals of a returned
                            // `using var` (class dispose etc.) are excluded here; the
                            // runtime-managed cell free is preserved, or the cell
                            // leaks (the #1418 4-byte leak on `return count`).
                            return e.slot == retSlot &&
                                   !e.boxedCellFree && !e.boxedCellFreeDeferred;
                        }),
                    scope.end());
            }
        }
        // CG21c (§31.2): if inside a try-with-finally, stash the return value
        // and route through the finally block instead of ret-ing now.
        // fix #1567 / §29.7/§28.10 (v0.182): cleanup runs in REVERSE
        // establishment order on the return edge, uniformly with the exception
        // edge. Dispose ONLY the try-body scopes (indices >= the innermost
        // finally's entry depth) here — the enclosing scopes established before
        // the try are disposed by the finally's normal-exit dispatch AFTER the
        // finally body runs (see mode=1 in lowerTry). The pre-v0.182 code ran
        // emitAllDisposals() before the finally, disposing an enclosing
        // `using var` BEFORE a finally that may touch it — a use-after-dispose.
        if (!finallyStack_.empty()) {
            auto& fr = finallyStack_.back();
            emitDisposalsInRange(fr.disposeDepthAtEntry, disposeStack_.size());
            if (fr.retValSlot && v)
                builder_.CreateStore(v, fr.retValSlot);
            builder_.CreateStore(builder_.getInt32(1), fr.exitModSlot);
            builder_.CreateBr(fr.finNormalBB);
            return true;
        }
        // No enclosing finally: dispose all live scopes, then ret.
        emitAllDisposals();
        // fix #1607 / §28.3/§28.4 (§3459: `return;` exits a destructor early):
        // an early `return;` inside a user destructor body is a second
        // reachable exit from emitDisposerCore's bz_exc park (see
        // LlvmBackend_Disposal.cpp). Without restoring here, the still
        // in-flight propagating exception parked at disposer entry would stay
        // cleared forever on this path — turning the pre-existing "early
        // return skips owned-field disposal" deferred corner into a strictly
        // worse one (the exception itself silently vanishes, rather than just
        // skipping extra cleanup). Combine via select with any genuinely NEW
        // exception the body itself raised before this point, mirroring the
        // normal-completion restore.
        if (emittingDisposerBody_ && disposerEntryExc_) {
            llvm::Value* excNow = builder_.CreateLoad(ptrTy(), emitExcSlot());
            llvm::Value* isNew = builder_.CreateICmpNE(
                excNow, llvm::ConstantPointerNull::get(ptrTy()));
            llvm::Value* restored = builder_.CreateSelect(
                isNew, excNow, disposerEntryExc_);
            builder_.CreateStore(restored, emitExcSlot());
        }
        emitReturn(currentFn_, v);
        return true;
    }
    if (auto* es = dynamic_cast<const ast::ExpressionStmt*>(&s)) {
        if (es->expr) lowerExpr(*es->expr);  // side effects only (CG3: none)
        return diverged_;  // CG40: a throw-expression terminated the block
    }
    if (auto* ds = dynamic_cast<const ast::DiscardStmt*>(&s)) {
        if (ds->value) {
            if (ds->discardOutOwnedVar) {
                // fix #1892 / §17.13 / §28.5: FIRST, an announced-consume discard
                // of a named owning-COLLECTION local (OwningArray/OwningDictionary/
                // OwningSet/OwningStack/OwningQueue/OwningLinkedList), of ANY decl
                // form (plain `var` OR `using var`). This is handled BEFORE the
                // class reclaim path below because explicitDisposeOwnedReceiver-
                // Slot mis-serves a collection: for a plain-`var` collection it
                // returns null (unwindOwnedLocalClass excludes collections) so the
                // container was never disposed (the #1892 leak), and for a
                // `using var` collection it returns the slot, feeding the collection
                // struct into the CLASS dispatch (emitDisposeAndReclaim reads its
                // first word as a vtable → SEGV). A collection is neither: it frees
                // via a runtime call, cascading disposal into each owned element
                // (§28.5). Dispose the container HERE at the discard point (§17.13:
                // "disposed at the end of the discard statement") via the shared
                // collection-disposal machinery — registerUsingDisposal builds the
                // correct bz_array_free / bz_dict_free / bz_llist_free with the
                // element-disposer cascade, pushed into a one-shot dispose frame and
                // emitted now — then NULL the slot so the collection's own scope-end/
                // unwind disposal (a `using var`'s registerUsingDisposal at its
                // LocalDecl, or a discard-discharged plain `var`'s dischargedByDiscard
                // registration — both fire on both edges and are null-safe) reloads
                // null and no-ops. The container and its elements are disposed
                // EXACTLY ONCE, no double-free; the pre-discard-throw exception edge
                // is covered by that both-edges registration. // per §17.13, §28.5
                const ast::LocalDecl* collLd = nullptr;
                if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(
                        ds->value.get()))
                    if (auto* loc = dynamic_cast<sema::LocalSymbol*>(
                            ctx_.exprSymbol(*id)))
                        if (auto* ld2 = dynamic_cast<const ast::LocalDecl*>(
                                loc->decl))
                            if (collectionTypeOfLocal(ld2) ||
                                dictTypeOfLocal(ld2) ||
                                linkedListTypeOfLocal(ld2))
                                collLd = ld2;
                if (collLd && !diverged_) {
                    disposeStack_.emplace_back();
                    registerUsingDisposal(collLd);
                    emitDisposals(disposeStack_.back());
                    disposeStack_.pop_back();
                    auto lit = locals_.find(collLd);
                    if (lit != locals_.end() && lit->second) {
                        llvm::Type* ety = nullptr;
                        llvm::Value* addr = boxedStorage(lit->second, ety);
                        if (addr && ety && ety->isPointerTy())
                            builder_.CreateStore(
                                llvm::ConstantPointerNull::get(ptrTy()), addr);
                    }
                } else if (!diverged_) {
                    // §17.13 / §28.3 v0.184 (#1387, generalized by #1651): `_ := x`
                    // where `x` is a named owned CLASS/INTERFACE binding (a plain
                    // owned local of any decl form, including the `out owned var`
                    // inline local, or an `owned` parameter) — an announced-consume
                    // §17.13 discharge. Emit null-guarded dispose+reclaim of the
                    // local's slot and null it so any other scope-end / exception-
                    // path disposal registered for the same slot (e.g.
                    // ensureOutVarSlots' unwindOnly disposal) is a no-op after this
                    // point. explicitDisposeOwnedReceiverSlot finds the slot address
                    // — the same resolver `x?.Dispose()` / `x.Dispose()` uses, so it
                    // is already general across LocalSymbol (any decl kind) and
                    // `owned` ParameterSymbol receivers.
                    llvm::Type* slotTy = nullptr;
                    llvm::Value* slotAddr =
                        explicitDisposeOwnedReceiverSlot(*ds->value, slotTy);
                    if (slotAddr) {
                        llvm::Value* obj =
                            builder_.CreateLoad(ptrTy(), slotAddr);
                        auto* nullVal = llvm::ConstantPointerNull::get(ptrTy());
                        auto* isNull  = builder_.CreateICmpEQ(obj, nullVal);
                        auto* dispBB  = llvm::BasicBlock::Create(
                            llvm_, "disc.disp", currentFn_);
                        auto* contBB  = llvm::BasicBlock::Create(
                            llvm_, "disc.cont", currentFn_);
                        builder_.CreateCondBr(isNull, contBB, dispBB);
                        builder_.SetInsertPoint(dispBB);
                        emitDisposeAndReclaim(obj);
                        builder_.CreateStore(nullVal, slotAddr);
                        builder_.CreateBr(contBB);
                        builder_.SetInsertPoint(contBB);
                    }
                }
            } else if (ds->disposeOwnedValue) {
                // §17.13 / §28.3 v0.150 (#1009): the ownership pass classified
                // this discard's value as an owned PRODUCER of a user class/
                // interface — an unbound owned temporary. Dispose it at the end
                // of the discard statement. Open a one-entry dispose scope so the
                // disposal fires on BOTH the normal edge (here) and the unwind
                // edge (if some later cleanup in this statement throws); the
                // produced value is the sole owner (producerState == Owned), so
                // it is disposed EXACTLY once (no leak, no double-free). The
                // dynamicDispose entry dispatches Dispose+reclaim through the
                // object's own vtable (correct for any runtime class/interface),
                // null-guarded.
                disposeStack_.emplace_back();
                llvm::Value* v = lowerExpr(*ds->value);
                if (!diverged_ && v) {
                    llvm::AllocaInst* slot = entryAlloca(ptrTy());
                    builder_.CreateStore(v, slot);
                    Disposal d{slot, nullptr};
                    d.dynamicDispose = true;
                    disposeStack_.back().push_back(d);
                }
                if (!diverged_) emitDisposals(disposeStack_.back());
                disposeStack_.pop_back();
            } else if (ds->disposeInlineOwnedArray) {
                // §17.13 / §15.3 (#1016): a discard `_ := expr` of a move-only
                // owning inline array. Dispose each non-null slot at end-of-
                // statement, mirroring the scope-end inlineOwnedArray disposal
                // path in emitDisposals. MoveOnlyArrayTransferWalker already
                // marked the source as transferred so registerInlineOwnedArray-
                // Disposal skips the scope-end disposal — no double-free.
                // Reach the [N x ptr] alloca through the source expression:
                // unwrap `move expr` to the bare identifier, then use localSlot
                // to find the alloca (the only valid form for an owning inline
                // array expression). // per §17.13, §15.3
                const ast::Expr* src = ds->value.get();
                if (auto* ue = dynamic_cast<const ast::UnaryExpr*>(src))
                    if (ue->op == "move" && ue->operand)
                        src = ue->operand.get();
                llvm::AllocaInst* arrSlot = src ? localSlot(*src) : nullptr;
                if (arrSlot) {
                    if (auto* arrTy = llvm::dyn_cast<llvm::ArrayType>(
                            arrSlot->getAllocatedType())) {
                        unsigned N = static_cast<unsigned>(arrTy->getNumElements());
                        disposeStack_.emplace_back();
                        Disposal d{arrSlot, nullptr};
                        d.inlineOwnedArray = true;
                        d.inlineSlotCount = N;
                        disposeStack_.back().push_back(d);
                        emitDisposals(disposeStack_.back());
                        disposeStack_.pop_back();
                    }
                }
            } else {
                // §17.13 / stdlib §5 / §28 (#1015): a discard of a FRESH-ownerless
                // collection materializer (`_ := items.ToArray()` / `.Sorted()` /
                // `.Reversed()` / `.ToDictionary()`) leaks its freshly-allocated
                // backing — the §28.6/§5.11 two-axis model makes a borrowing
                // Array/Dictionary a borrow of its ELEMENTS, so producerState
                // classifies it Borrowed and #1009's class/interface path above
                // does not fire, yet the BACKING (struct + buffer) is owned by
                // nothing. Reclaim it at end-of-statement by reusing the
                // #1004/#1006/#1013 helper, which frees the array/dict struct with
                // the matching per-element disposer (borrow elements → null, except
                // the Array<string> co-owned +1) and is a NO-OP for any non-
                // materializer discard (its narrow guards) → no UAF / double-free.
                llvm::Value* v = lowerExpr(*ds->value);
                if (!diverged_ && v)
                    freeUnboundOwnedBorrowingArrayTemp(
                        ds->value.get(), v, ctx_.exprType(*ds->value));
            }
        }
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
        // #548 / stdlib §4.8 v0.135: `for var e in dict` — entry iteration over
        // a non-owning Dictionary<K,V>; the loop variable is DictionaryEntry<K,V>.
        // Must run before the generic for-handlers (DictionaryEntry has no user
        // TypeSymbol so the array/IEnumerable paths would fall through).
        if (lowerDictEntryFor(*fors)) return false;
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
        // #639 / stdlib §4.5/§4.7/§4.11 v0.135: `for var x in coll.Drain()`
        // ownership-transferring drain over OwningArray<T>, OwningLinkedList<T>,
        // OwningStack<T>, OwningQueue<T>.
        // Must run BEFORE lowerDictDrainFor and lowerIEnumerableBuiltinFor.
        if (lowerOwnedSeqDrainFor(*fors)) return false;
        // #628 / stdlib §4.9 v0.135: `for var entry in ownedDict.Drain()`
        // ownership-transferring drain over OwningDictionary<K,V>.
        // Must run BEFORE lowerIEnumerableBuiltinFor (which also handles
        // IOwningEnumerable<T> but via vtable dispatch — incorrect for the
        // dict drain case, which uses the bz_dict_drain_next_* runtime).
        if (lowerDictDrainFor(*fors)) return false;
        // §29.2 (B1): `for var x in items` where items is IEnumerable<T>
        // typed as a builtin interface — treat runtime value as bz_array_t*.
        if (lowerIEnumerableBuiltinFor(*fors)) return false;
        // §29.2 (A): `for var x in obj` over user-defined IEnumerable<T>.
        if (lowerIEnumerableFor(*fors)) return false;
        // stdlib §4.13 (#728): `for var x in <LinkedList<T>>` — walk the node
        // chain via the runtime list cursor (not array-backed).
        if (lowerLinkedListFor(*fors)) return false;
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
            // #1083 / §31.2: route through the finally ONLY when the break EXITS
            // the try — i.e. the target loop was already open at try-entry (the
            // try is nested inside the loop). A loop opened INSIDE the try body
            // (loopStack_.size() > loopDepthAtEntry) is intra-try: break it
            // directly, leaving the finally to run later when the try exits.
            // Routing such a break through the finally skipped the rest of the
            // try body and, with the mode slot still set, re-ran the finally.
            if (!finallyStack_.empty() &&
                loopStack_.size() <= finallyStack_.back().loopDepthAtEntry) {
                // fix #1598 / §29.7/§28.10 (v0.182): a break that crosses a
                // finally disposes only the try-BODY scopes (>= this finally's
                // entry depth) here; the enclosing scopes down to the loop's own
                // scope depth — which may include a `using var` established
                // before the try but inside the loop — are disposed by the
                // finally's mode-2 dispatch AFTER the finally body runs
                // (reverse establishment, mirroring the #1567 return fix). The
                // pre-v0.182 code called emitLoopDisposals(loopScopeDepth) HERE,
                // disposing that enclosing `using var` BEFORE the finally ran —
                // a use-after-dispose if the finally touched it.
                auto& fr = finallyStack_.back();
                emitDisposalsInRange(fr.disposeDepthAtEntry, disposeStack_.size());
                builder_.CreateStore(builder_.getInt32(2), fr.exitModSlot);
                builder_.CreateBr(fr.finNormalBB);
            } else {
                // No finally in the way: dispose eagerly down to the loop scope,
                // as before.
                emitLoopDisposals(loopStack_.back().scopeDepth);
                builder_.CreateBr(loopStack_.back().breakTarget);
            }
            return true;
        }
        return false;
    }
    if (dynamic_cast<const ast::ContinueStmt*>(&s)) {
        if (!loopStack_.empty()) {
            // #1083: as above — a continue of a loop opened inside the try is
            // intra-try; only a continue that exits the try routes via finally.
            if (!finallyStack_.empty() &&
                loopStack_.size() <= finallyStack_.back().loopDepthAtEntry) {
                // fix #1598 / §29.7/§28.10 (v0.182): mirrors the break fix above
                // — dispose only the try-body scopes here; the finally's mode-3
                // dispatch disposes the enclosing range (down to the loop scope)
                // AFTER the finally body runs.
                auto& fr = finallyStack_.back();
                emitDisposalsInRange(fr.disposeDepthAtEntry, disposeStack_.size());
                builder_.CreateStore(builder_.getInt32(3), fr.exitModSlot);
                builder_.CreateBr(fr.finNormalBB);
            } else {
                emitLoopDisposals(loopStack_.back().scopeDepth);
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
                // §29.7 / §30.9 (#721 C2): a `yield break` exiting a
                // try-with-finally must run the finally on its way out — just
                // like return/break/continue. Route through the active finally
                // (exit mode 4); the post-finally dispatch (lowerTry) performs
                // the terminal state=-1 + ret. Without this the finally is
                // silently skipped on the yield-break edge.
                if (!finallyStack_.empty()) {
                    // fix #1598 / §29.7/§28.10 (v0.182): dispose only the
                    // try-body scopes (>= this finally's entry depth) before
                    // routing to the finally — the yield-break twin of the
                    // return/break/continue fixes. The enclosing scopes (an
                    // outer `using var` established before the try) are
                    // disposed by the finally's mode-4 dispatch AFTER the
                    // finally body runs, mirroring mode 1's reverse-
                    // establishment order. Pre-fix this site disposed nothing
                    // at all before jumping to the finally.
                    auto& fr = finallyStack_.back();
                    emitDisposalsInRange(fr.disposeDepthAtEntry, disposeStack_.size());
                    builder_.CreateStore(builder_.getInt32(4), fr.exitModSlot);
                    builder_.CreateBr(fr.finNormalBB);
                    return true;
                }
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
                    // #1102B / §7.1: for an `IEnumerable<T?>` iterator the element
                    // slot is the tagged nullable-value struct `{i8,T}`; a bare
                    // `yield return <T>` (or `yield return null`) lowers to the
                    // payload type, which != iterElemTy_, so the store below was
                    // silently dropped and current_val stayed calloc-zeroed (every
                    // consumed element read null). Coerce the value into the
                    // declared element type first — a no-op for non-nullable
                    // elements (coerceReturnToNullableValue returns v unchanged
                    // unless the element is a nullable value type).
                    v = coerceReturnToNullableValue(v, iterElemSemaTy_);
                    if (v->getType() != iterElemTy_ &&
                        v->getType()->isIntegerTy() && iterElemTy_->isIntegerTy()) {
                        // #1596 / §5.1: a narrower integer (e.g. a `byte` span
                        // element) yielded into a wider element type (e.g.
                        // `IEnumerable<int>`) is a WIDENING conversion, not a
                        // truncation — CreateTrunc requires DestTy narrower
                        // than SrcTy, and the verifier rejects the reverse
                        // ("DestTy too big for Trunc"). Widen with zero-extend
                        // for unsigned source primitives (byte, ushort, uint,
                        // ulong, char) and sign-extend otherwise, matching the
                        // value-preserving widening of §5.1; only truncate
                        // when the source is genuinely the wider type.
                        unsigned srcBits = v->getType()->getIntegerBitWidth();
                        unsigned dstBits = iterElemTy_->getIntegerBitWidth();
                        if (srcBits > dstBits) {
                            v = builder_.CreateTrunc(v, iterElemTy_);
                        } else if (srcBits < dstBits) {
                            const sema::TypeRef* srcSemaTy =
                                ctx_.exprType(*ys->value);
                            bool srcUnsigned = srcSemaTy &&
                                isUnsignedPrim(srcSemaTy->primitive);
                            v = srcUnsigned
                                ? builder_.CreateZExt(v, iterElemTy_)
                                : builder_.CreateSExt(v, iterElemTy_);
                        }
                    }
                    if (v->getType() == iterElemTy_) {
                        llvm::Value* cvField = builder_.CreateStructGEP(
                            smStateStruct_, smSelf_, iterCurrentValField_);
                        // §29 (#1120): before storing the new yielded value,
                        // dispose the PREVIOUS one — but only if the enumerator
                        // owned it (current_val_owned == 1, set when the previous
                        // yield was a `new` expression).  Using-var locals (owned
                        // == 0) are disposed by their own scope mechanisms; owned
                        // params (owned == 0) by disposeOwnedParams at iterator-
                        // Dispose.  Null-guard: first yield is a no-op (calloc 0).
                        // After handling the previous value, set the owned flag for
                        // the new value: 1 if the expression is a `new`, else 0.
                        if (smKind_ == SuspendKind::Yield && iterElemIsOwnedClass_) {
                            llvm::Value* ownedFld = builder_.CreateStructGEP(
                                smStateStruct_, smSelf_, iterCurrentValOwnedField_);
                            // --- dispose previous if owned ---
                            llvm::Value* prevVal = builder_.CreateLoad(
                                iterElemTy_, cvField, "ity.prev");
                            llvm::Value* prevIsNull = builder_.CreateICmpEQ(
                                prevVal,
                                llvm::ConstantPointerNull::get(ptrTy()),
                                "ity.prevnull");
                            auto* prevNonNullBB = llvm::BasicBlock::Create(
                                llvm_, "ity.prev.nonnull", currentFn_);
                            auto* prevContBB = llvm::BasicBlock::Create(
                                llvm_, "ity.prev.cont", currentFn_);
                            builder_.CreateCondBr(
                                prevIsNull, prevContBB, prevNonNullBB);
                            builder_.SetInsertPoint(prevNonNullBB);
                            llvm::Value* prevOwned = builder_.CreateLoad(
                                i8(), ownedFld, "ity.prevowned");
                            llvm::Value* prevIsOwned = builder_.CreateICmpNE(
                                prevOwned, builder_.getInt8(0), "ity.previso");
                            auto* dispBB = llvm::BasicBlock::Create(
                                llvm_, "ity.prev.disp", currentFn_);
                            auto* skipBB = llvm::BasicBlock::Create(
                                llvm_, "ity.prev.skip", currentFn_);
                            auto* afterDispBB = llvm::BasicBlock::Create(
                                llvm_, "ity.prev.after", currentFn_);
                            builder_.CreateCondBr(prevIsOwned, dispBB, skipBB);
                            builder_.SetInsertPoint(dispBB);
                            emitDisposeAndReclaim(prevVal);
                            builder_.CreateBr(afterDispBB);
                            builder_.SetInsertPoint(skipBB);
                            builder_.CreateBr(afterDispBB);
                            builder_.SetInsertPoint(afterDispBB);
                            builder_.CreateBr(prevContBB);
                            builder_.SetInsertPoint(prevContBB);
                            // --- set owned flag for new value ---
                            // Enumerator owns it only when the expression is a
                            // direct `new` allocation (a fresh, unattached object).
                            // Variable references (using-var locals, params, etc.)
                            // are borrowed — their existing disposal handles them.
                            bool yieldIsNew =
                                dynamic_cast<const ast::NewExpr*>(
                                    ys->value.get()) != nullptr;
                            builder_.CreateStore(
                                builder_.getInt8(yieldIsNew ? 1 : 0), ownedFld);
                        }
                        builder_.CreateStore(v, cvField);
                    }
                }
            }
            unsigned nextState = ++smNextSuspendState_;
            // §29.7 (#623): snapshot the active finally-body chain so the
            // Dispose function can re-run finally clauses when the iterator
            // is disposed while suspended at this yield point.  The state
            // index stored in the frame when the consumer resumes from this
            // yield is `nextState`; Dispose switches on that value.
            if (!finallyBodyStack_.empty()) {
                smIterFinallyAtState_[nextState] =
                    std::vector<const std::vector<std::unique_ptr<ast::Stmt>>*>(
                        finallyBodyStack_.begin(), finallyBodyStack_.end());
            }
            // #821 / §29.7: snapshot the `using var` (owned-resource) disposals
            // active at this yield so Dispose can run them when the iterator is
            // disposed while suspended here. Each using-var local is spilled to
            // the frame and restored in Dispose; record its decl node (reverse-
            // mapped from the Disposal's slot) so Dispose can rebind the slot.
            // Skip non-using-var entries: caught-exception borrows (never live
            // across a yield) and unwind-only plain-`var` owned locals (§29.7
            // disposes using-var only). Outermost-first; Dispose runs in reverse.
            {
                std::vector<std::pair<const ast::Node*, Disposal>> active;
                // §29.7 (#944): for-in implicit enumerator/source frames active
                // here (iterEnumDispose disposals wired to a frame field). They
                // have no decl node, so they are tracked by FRAME FIELD INDEX in a
                // parallel map; Dispose GEPs the field and disposes via slot 1.
                std::vector<unsigned> activeFrames;
                for (const auto& level : disposeStack_) {
                    for (const Disposal& d : level) {
                        if (d.caughtException || d.unwindOnly) continue;
                        if (d.iterEnumDispose) {
                            auto* a = llvm::dyn_cast_or_null<llvm::AllocaInst>(
                                d.slot);
                            auto fit = a ? smIterEnumFrameSlot_.find(a)
                                         : smIterEnumFrameSlot_.end();
                            if (a && fit != smIterEnumFrameSlot_.end()) {
                                activeFrames.push_back(fit->second);
                                continue;  // tracked by field, not by node
                            }
                            // An iterEnumDispose with no frame field (e.g. a
                            // for-in not inside this suspending iterator body)
                            // cannot be live across this yield — skip.
                            continue;
                        }
                        const ast::Node* node = nullptr;
                        for (const auto& [n, s] : locals_)
                            if (s == d.slot) { node = n; break; }
                        if (node) active.emplace_back(node, d);
                    }
                }
                if (!active.empty())
                    smIterDisposalsAtState_[nextState] = std::move(active);
                if (!activeFrames.empty())
                    smIterEnumFramesAtState_[nextState] = std::move(activeFrames);
            }
            // fix #1567 / §29.7/§28.10 (v0.182): build the ONE establishment-
            // ordered cleanup stack for this suspend state — using-var disposals,
            // for-in enumerator-frame disposals, and finally bodies in the single
            // order they were established. The SYNC iterator Dispose consumes this
            // and runs it in exact reverse (§28.10: one establishment-ordered
            // stack, no category-ordered phases); the category-split maps above
            // remain for the async iterator Dispose (a separate follow-up). A
            // finally established when the dispose-stack had depth d sits, in
            // establishment order, immediately before scope d (after every scope
            // < d, before every scope >= d), so early Dispose runs it AFTER its
            // own try body's using-var disposals and BEFORE the enclosing scopes'
            // — reverse establishment, matching the exception edge. finallyStack_
            // is outermost-first (increasing disposeDepthAtEntry) and parallel to
            // finallyBodyStack_.
            {
                std::vector<IterCleanup> actions;
                std::size_t finIdx = 0;
                auto flushFinallysBefore = [&](std::size_t depth) {
                    while (finIdx < finallyStack_.size() &&
                           finIdx < finallyBodyStack_.size() &&
                           finallyStack_[finIdx].disposeDepthAtEntry == depth) {
                        IterCleanup act;
                        act.kind = IterCleanup::Kind::Finally;
                        act.finallyBody = finallyBodyStack_[finIdx];
                        actions.push_back(act);
                        ++finIdx;
                    }
                };
                for (std::size_t i = 0; i < disposeStack_.size(); ++i) {
                    flushFinallysBefore(i);  // finally@depth i precedes scope i
                    for (const Disposal& d : disposeStack_[i]) {
                        if (d.caughtException || d.unwindOnly) continue;
                        if (d.iterEnumDispose) {
                            auto* alloca =
                                llvm::dyn_cast_or_null<llvm::AllocaInst>(d.slot);
                            auto fit = alloca ? smIterEnumFrameSlot_.find(alloca)
                                              : smIterEnumFrameSlot_.end();
                            if (alloca && fit != smIterEnumFrameSlot_.end()) {
                                IterCleanup act;
                                act.kind = IterCleanup::Kind::EnumFrame;
                                act.frameField = fit->second;
                                actions.push_back(act);
                            }
                            continue;
                        }
                        const ast::Node* node = nullptr;
                        for (const auto& [n, s] : locals_)
                            if (s == d.slot) { node = n; break; }
                        if (node) {
                            IterCleanup act;
                            act.kind = IterCleanup::Kind::Disposal;
                            act.node = node;
                            act.disposal = d;
                            actions.push_back(act);
                        }
                    }
                }
                // Defensive: a finally at depth == disposeStack_.size() sorts
                // after all scopes; in practice the yield is inside the try body,
                // so a body scope exists and no such finally remains.
                flushFinallysBefore(disposeStack_.size());
                if (!actions.empty())
                    smIterCleanupAtState_[nextState] = std::move(actions);
            }
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
    // fix #1598 / §29.7/§28.10 (v0.182): the disposeStack_ depth at the
    // innermost loop's own entry (captured alongside loopBreakBB/loopContBB,
    // same loopStack_.back()). A break/continue that exits straight to this
    // loop (no further outer finally) disposes the enclosing range
    // [loopScopeDepth, bodyScopeIdx) — scopes established before this try's body
    // scope but still inside the loop, e.g. an enclosing `using var` — AFTER the
    // finally body runs, mirroring mode 1/5's enclosing-range dispatch (the body
    // scope itself is disposed at the exit site; see the frame-setup comment).
    std::size_t loopScopeDepth =
        loopStack_.empty() ? 0 : loopStack_.back().scopeDepth;
    // #721 C1: in an async method, `return v` completes the OUTER task with the
    // unwrapped result (asyncResultTy_), not a function `ret`. Allocate a slot of
    // that type so an async return inside this try can stash its value across the
    // finally (exit mode 5). Null for a `Task` method (asyncResultTy_ == null).
    llvm::AllocaInst* asyncRetSlot = nullptr;
    if (hasFin) {
        exitModSlot = entryAlloca(i32());
        builder_.CreateStore(builder_.getInt32(0), exitModSlot);
        if (retTy && !retTy->isVoidTy())
            retValSlot = entryAlloca(retTy);
        if (smKind_ == SuspendKind::Await && smStateStruct_ && asyncResultTy_)
            asyncRetSlot = entryAlloca(asyncResultTy_);
        FinallyFrame frame{finNormalBB, exitModSlot, retValSlot,
                           retTy, afterBB, loopBreakBB, loopContBB};
        frame.asyncRetSlot = asyncRetSlot;
        // #1083: remember how many loops were already open at try-entry. A
        // break/continue of a loop opened INSIDE the try body must not route
        // through this finally (see the break/continue handlers).
        frame.loopDepthAtEntry = loopStack_.size();
        // fix #1263 / §28.3 (reworked to compose with #1567/#1598): for a
        // try-WITH-finally, push the try body's own disposal scope NOW so it
        // sits at an index BELOW thisTryDepth, but capture disposeDepthAtEntry
        // BEFORE the push (= that body-scope index) so the early-exit edges keep
        // the #1567/#1598 reverse-establishment ordering. §28.3: "the explicit
        // finally runs first (with the local still in scope), then the implicit
        // auto-dispose runs as the scope unwinds further." The body scope's
        // disposal point is EDGE-DEPENDENT:
        //  • EXCEPTION edge (finUnwindBB) and NORMAL edge (fallDispBB): the body
        //    scope is disposed AFTER the finally body — §28.3's finally-first
        //    ordering, the only edges #1263 pins. The throw site's
        //    unwindScopeDepth_ (= disposeStack_.size() AFTER this push) sits
        //    ABOVE the body scope, so emitUnwindDisposals skips it; finUnwindBB's
        //    emitUnwindDisposalsInRange(savedDepth, thisTryDepth) and fallDispBB's
        //    emitDisposalsInRange(bodyScopeIdx, thisTryDepth) dispose it after the
        //    finally. Pre-fix, both edges disposed the body scope BEFORE the
        //    finally — a UAF if the finally touched a body local (#1263 saw 214
        //    instead of the spec-mandated 421).
        //  • BREAK/CONTINUE/RETURN/YIELD-BREAK edges: the body scope is disposed
        //    at the EXIT SITE (the site's emitDisposalsInRange(disposeDepthAtEntry,
        //    top) includes it, since disposeDepthAtEntry == bodyScopeIdx), and the
        //    mode dispatches (modes 1-5) use bodyScopeIdx as their upper bound,
        //    disposing only the ENCLOSING scopes — the #1567/#1598 reverse-
        //    establishment chain. Capturing disposeDepthAtEntry AFTER the push (the
        //    original #1263) shifted this index past the body scope, so the inner
        //    finally's mode dispatch no longer reached an OUTER try's body scope:
        //    that scope slipped to the outer finally's dispatch and was disposed
        //    too late — the #1598 nested-finally-chain break-edge regression
        //    (419 became 491). Capturing it before the push restores the index
        //    #1598's dispatch ranges depend on.
        // The body is lowered with lowerBody (no fall-through auto-dispose); the
        // scope is popped at the end of lowerTry, after every dispatch block that
        // disposes it is emitted. A try WITHOUT a finally is unaffected
        // (lowerScopedBlock owns the body scope on both edges as before).
        frame.disposeDepthAtEntry = disposeStack_.size();   // = bodyScopeIdx
        disposeStack_.emplace_back();   // §28.3 body scope (kept on the stack)
        finallyStack_.push_back(frame);
        // §29.7 (#623): track the AST finally-body alongside the frame so
        // a yield inside this try can snapshot the chain for Dispose.
        finallyBodyStack_.push_back(&trys.finallyBody);
        // §30.9 / §31.2 (#438): an in-finally await re-enters the finally
        // mid-body; the exit-mode and return-value slots were set BEFORE the
        // finally (in the normal-exit path) and are read AFTER the suspend (the
        // finally.resume dispatch), so frame-back them across the cross-thread hop.
        if (finallyAwaits) {
            asyncHandlerSpillSlots_.push_back(exitModSlot);
            if (retValSlot) asyncHandlerSpillSlots_.push_back(retValSlot);
            if (asyncRetSlot) asyncHandlerSpillSlots_.push_back(asyncRetSlot);
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
    // fix #1263-rework: the try body's disposal scope sits at thisTryDepth-1
    // (bodyScopeIdx). The mode dispatches (modes 1-5) dispose only the ENCLOSING
    // scopes [.., bodyScopeIdx), leaving the body scope to the EXIT SITE — the
    // #1567/#1598 reverse-establishment chain. finUnwindBB and fallDispBB use
    // thisTryDepth (inclusive of the body scope) — those are the §28.3 finally-
    // first edges (exception + normal). See the frame-setup comment above.
    const std::size_t bodyScopeIdx = thisTryDepth - 1;
    // fix #1263 / §28.3 (see the body-scope push in the frame setup above): for
    // a try-WITH-finally the body scope is already on the stack; lower the body
    // with lowerBody so it is NOT auto-disposed on normal fall-through (the
    // finally's mode-0 dispatch disposes it AFTER the finally body). For a try
    // WITHOUT a finally, lowerScopedBlock owns and disposes the body scope on
    // both edges as before.
    bool bodyTerm = hasFin ? lowerBody(trys.body)
                           : lowerScopedBlock(trys.body);
    unwindTarget_ = savedUnwind;
    unwindScopeDepth_ = savedDepth;
    if (!bodyTerm) builder_.CreateBr(normalExit);

    // Catch dispatch: test arms in order against the in-flight exception.
    builder_.SetInsertPoint(dispatchBB);
    llvm::Value* exc = builder_.CreateLoad(ptrTy(), emitExcSlot());
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
            // §31.2.1 (v0.182 / #1505): a generic type parameter is admissible
            // as an arm type and matches against its SUBSTITUTED type argument
            // per instantiation. Inside a monomorphized body resolveTypeParamRef
            // maps the placeholder (`TException`) to the concrete bound type
            // (`MyFault`). Without it the placeholder ref has no `declaration`
            // and no builtin-name entry, so descriptorForTypeName returns null,
            // the arm's type test is skipped, and dispatch silently falls
            // through to a later `Exception` arm (wrong-arm selection).
            const sema::TypeRef* resolved = resolveTypeParamRef(rt);
            const ast::Type* descNode = ty.get();
            if (resolved && resolved != rt && resolved->astNode)
                descNode = resolved->astNode;
            llvm::GlobalVariable* desc =
                descriptorForTypeName(resolved, descNode);
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
                // §31 (#720 B2): the `when` guard runs DURING catch dispatch,
                // while the in-flight exception is still in the global slot. Any
                // `new`/call inside the guard would otherwise misfire the
                // pending-exception checks — emitNewPendingCheck/emitPendingCheck
                // see the in-flight exception and treat it as a failure of the
                // guard's OWN operation (freeing freshly-allocated objects,
                // short-circuiting the call). Set the in-flight exception aside
                // for the guard: clear the slot before, restore it on the
                // not-thrown continuation. A throw FROM the guard sets a NEW
                // exception that the in-guard pending-checks propagate — it
                // supersedes the in-flight one (consistent with throw-in-finally,
                // #720 B1).
                builder_.CreateStore(
                    llvm::ConstantPointerNull::get(ptrTy()), emitExcSlot());
                // #1108: a `when` guard that THROWS supersedes the in-flight
                // exception (E1); E1 must be freed exactly once. Expose E1 as
                // the caught object LEAVING this arm for the duration of the
                // guard so a guard `throw` routes through emitThrowValue's
                // existing #397/#866 caught-exception free (LlvmBackend_Setup
                // ~2142) — emitted at a LIVE insert point (before the throw's
                // own branch) and already guarded against double-free by
                // `isDirectRethrow` (a guard re-raising the SAME bound object
                // keeps it in flight) and `throwLeavesArm`. This adds NO new
                // free; it reuses the battle-tested path. Without it E1 leaked
                // (the arm's `caughtException` disposal is registered only in
                // armBB below, which is unreachable on the guard-throw edge).
                // Restored immediately after the guard so armBB re-establishes
                // its own caught-exception context from the correct outer state.
                llvm::Value* savedGuardCaught = currentCaughtExc_;
                std::string_view savedGuardCaughtName = currentCaughtName_;
                llvm::BasicBlock* savedGuardArmTarget = caughtExcArmTarget_;
                currentCaughtExc_ = exc;
                currentCaughtName_ =
                    arm.hasBinding ? arm.varName : std::string_view{};
                caughtExcArmTarget_ = unwindBB();
                llvm::Value* g = lowerExpr(*arm.guard);
                currentCaughtExc_ = savedGuardCaught;
                currentCaughtName_ = savedGuardCaughtName;
                caughtExcArmTarget_ = savedGuardArmTarget;
                if (diverged_) {
                    // §31.2.1 / §31.5 (#1097): the guard expression ITSELF threw
                    // (e.g. `catch E when throw new F()` — a `throw` is a
                    // bottom-typed expression valid in any expression position).
                    // The new exception supersedes the in-flight one and
                    // propagates via the throw's own unwind branch, which already
                    // terminated this prep block. Do NOT restore the exception or
                    // branch on the throw's (poison) value — that produced a
                    // `br ptr poison` malformed-IR crash. Reset the divergence
                    // flag for the rest of the catch-dispatch chain; armBB/nextBB
                    // become unreachable from this guard (harmless dead blocks).
                    diverged_ = false;
                } else {
                    if (!g) g = builder_.getInt1(false);
                    // Guard returned normally (did not throw): restore the
                    // in-flight exception so a non-match continues matching /
                    // re-propagates with it. (The matched armBB clears the slot
                    // again below.)
                    builder_.CreateStore(exc, emitExcSlot());
                    builder_.CreateCondBr(g, armBB, nextBB);
                }
            } else {
                builder_.CreateBr(armBB);
            }
        } else {
            builder_.CreateCondBr(matched, armBB, nextBB);
        }

        builder_.SetInsertPoint(armBB);
        builder_.CreateStore(
            llvm::ConstantPointerNull::get(ptrTy()), emitExcSlot());
        llvm::Value* savedCaught = currentCaughtExc_;
        std::string_view savedCaughtName = currentCaughtName_;
        llvm::BasicBlock* savedArmTarget = caughtExcArmTarget_;
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
        // #866: record the target a throw uses to LEAVE this arm. emitThrowValue
        // frees the caught exception (the #397 optimization) only when a throw's
        // unwind target equals this; a throw caught by an INNER try within the arm
        // targets that inner dispatch instead and must NOT free it (the arm
        // continues; the caught exception stays in flight for the arm's own exit).
        // Use the RESOLVED block (unwindBB() maps a null target to the function's
        // propagate boundary) so a single try with no enclosing handler still
        // compares correctly against a throw's resolved target.
        caughtExcArmTarget_ = unwindBB();
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
        caughtExcArmTarget_ = savedArmTarget;
        if (!armTerm) builder_.CreateBr(normalExit);

        builder_.SetInsertPoint(nextBB);
    }
    // No arm matched → exception still pending; run finally (if any)
    // then re-propagate to the enclosing unwind target.
    if (hasFin) {
        builder_.CreateBr(finUnwindBB);
    } else {
        // #822 / §28.3: with no finally, the throw site disposed only down to
        // this try's depth, so owned locals in the scopes BETWEEN this try and
        // the enclosing handler (e.g. an outer `using var` declared before the
        // inner try in the same block) are still live and would LEAK on the
        // unmatched-arm re-propagation path. Dispose that intervening range
        // before re-propagating — the same discharge finUnwindBB performs after
        // its finally body (above).
        emitUnwindDisposalsInRange(savedDepth, thisTryDepth);
        builder_.CreateBr(unwindBB());
    }

    if (hasFin) {
        // Pop the frame — outer frames (if any) are now at back().
        finallyStack_.pop_back();
        if (!finallyBodyStack_.empty()) finallyBodyStack_.pop_back();
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
        // fix #1263 / §28.3: mode=0 (normal fall-through) disposes the try BODY
        // scope (index thisTryDepth-1) AFTER the finally body — the body was
        // lowered with lowerBody (no fall-through auto-dispose) precisely so the
        // finally runs first while the body's owned locals are still alive. The
        // enclosing scopes (< thisTryDepth-1) outlive the try and are NOT
        // disposed here. Pre-fix, lowerScopedBlock disposed the body scope BEFORE
        // the finally — a finally observing an already-disposed local.
        auto* fallDispBB = llvm::BasicBlock::Create(
            llvm_, "finally.falldisp", currentFn_);
        auto* sw = builder_.CreateSwitch(mode, fallDispBB, 3);
        builder_.SetInsertPoint(fallDispBB);
        emitDisposalsInRange(thisTryDepth - 1, thisTryDepth);
        builder_.CreateBr(afterBB);

        // mode=1: return — propagate through outer finally or emit ret.
        auto* retDispBB = llvm::BasicBlock::Create(llvm_,"finally.ret",currentFn_);
        sw->addCase(builder_.getInt32(1), retDispBB);
        builder_.SetInsertPoint(retDispBB);
        if (outerFr) {
            // fix #1567 / §29.7/§28.10 (v0.182): the finally body has now run;
            // dispose the scopes established between THIS finally and the next-
            // outer finally (reverse establishment) BEFORE routing the return
            // onward, then let the outer finally run and dispose its own outer
            // range. This is the normal-exit twin of the exception path's
            // emitUnwindDisposalsInRange(savedDepth, thisTryDepth) below.
            emitDisposalsInRange(outerFr->disposeDepthAtEntry, bodyScopeIdx);
            // Route through outer finally (nested try-with-finally).
            builder_.CreateStore(builder_.getInt32(1), outerFr->exitModSlot);
            if (retValSlot && outerFr->retValSlot) {
                llvm::Value* rv = builder_.CreateLoad(retTy, retValSlot);
                builder_.CreateStore(rv, outerFr->retValSlot);
            }
            builder_.CreateBr(outerFr->finNormalBB);
        } else {
            // fix #1567 / §29.7/§28.10 (v0.182): no enclosing finally — dispose
            // every remaining scope established before this try's body scope
            // (indices [0, bodyScopeIdx)), AFTER the finally body, then ret.
            // Together with the return site's disposal of the try-body scopes
            // (>= bodyScopeIdx) this disposes all live scopes in reverse
            // establishment order with the finally interleaved at its
            // establishment point.
            emitDisposalsInRange(0, bodyScopeIdx);
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
                // fix #1598 / §29.7/§28.10 (v0.182): dispose the scopes
                // established between this finally and the next-outer finally
                // (reverse establishment) before routing onward — the mode-2
                // twin of mode 1/5.
                emitDisposalsInRange(outerFr->disposeDepthAtEntry, bodyScopeIdx);
                builder_.CreateStore(builder_.getInt32(2), outerFr->exitModSlot);
                builder_.CreateBr(outerFr->finNormalBB);
            } else {
                // fix #1598 / §29.7/§28.10 (v0.182): no further outer finally —
                // dispose every remaining scope down to the loop's own scope
                // depth (an enclosing `using var` established before this try
                // but inside the loop) AFTER the finally body, then jump to the
                // break target. The pre-v0.182 code disposed nothing here,
                // leaving that range disposed too early (at the break site,
                // before the finally) instead — a use-after-dispose.
                emitDisposalsInRange(loopScopeDepth, bodyScopeIdx);
                builder_.CreateBr(loopBreakBB);
            }
        }

        // mode=3: continue — propagate through outer finally or jump to cond.
        if (loopContBB) {
            auto* cntDispBB = llvm::BasicBlock::Create(llvm_,"finally.cnt",currentFn_);
            sw->addCase(builder_.getInt32(3), cntDispBB);
            builder_.SetInsertPoint(cntDispBB);
            if (outerFr) {
                // fix #1598 / §29.7/§28.10 (v0.182): mode-3 twin of mode-2 above.
                emitDisposalsInRange(outerFr->disposeDepthAtEntry, bodyScopeIdx);
                builder_.CreateStore(builder_.getInt32(3), outerFr->exitModSlot);
                builder_.CreateBr(outerFr->finNormalBB);
            } else {
                // fix #1598 / §29.7/§28.10 (v0.182): mode-3 twin of mode-2 above.
                emitDisposalsInRange(loopScopeDepth, bodyScopeIdx);
                builder_.CreateBr(loopContBB);
            }
        }

        // mode=4 (#721 C2): yield break — run any OUTER finally too, else
        // perform the terminal iterator exit the yield-break statement deferred
        // so the finally could run first (state=-1; sync → ret false, async →
        // complete with false + ret void). Added only inside an ITERATOR state
        // machine (Yield/AsyncYield) — the only place mode 4 is produced. A
        // plain async METHOD (SuspendKind::Await) also has smStateGEP_ set but
        // returns void, so emitting the iterator return here would be malformed.
        if (smStateGEP_ && (smKind_ == SuspendKind::Yield ||
                            smKind_ == SuspendKind::AsyncYield)) {
            auto* ybDispBB =
                llvm::BasicBlock::Create(llvm_, "finally.yieldbrk", currentFn_);
            sw->addCase(builder_.getInt32(4), ybDispBB);
            builder_.SetInsertPoint(ybDispBB);
            if (outerFr) {
                // fix #1598 / §29.7/§28.10 (v0.182): dispose the scopes
                // established between this finally and the next-outer finally
                // (reverse establishment) before routing onward — the mode-4
                // twin of mode 1/5.
                emitDisposalsInRange(outerFr->disposeDepthAtEntry, bodyScopeIdx);
                builder_.CreateStore(builder_.getInt32(4), outerFr->exitModSlot);
                builder_.CreateBr(outerFr->finNormalBB);
            } else {
                // fix #1598 / §29.7/§28.10 (v0.182): a `yield break` is a
                // terminal exit from the whole MoveNext/Resume function (like
                // `return`), so — mirroring mode 1's emitDisposalsInRange(0,
                // bodyScopeIdx) — dispose every remaining scope established
                // before this try's body scope, AFTER the finally body, then
                // perform the terminal iterator exit. Pre-v0.182 this range was
                // never disposed on the yield-break edge at all.
                emitDisposalsInRange(0, bodyScopeIdx);
                builder_.CreateStore(builder_.getInt32(-1), smStateGEP_);
                if (smKind_ == SuspendKind::AsyncYield) {
                    emitAsyncIterComplete(/*hasValue=*/false);
                    builder_.CreateRetVoid();
                } else {
                    builder_.CreateRet(builder_.getFalse());
                }
            }
        }

        // mode=5 (#721 C1): async-method return — run any OUTER finally too, else
        // complete the outer task with the stashed unwrapped result and ret void
        // from Resume (the async analogue of mode 1). Only inside an async method
        // state machine (SuspendKind::Await), the only place mode 5 is produced.
        if (smKind_ == SuspendKind::Await && smStateStruct_) {
            auto* arDispBB =
                llvm::BasicBlock::Create(llvm_, "finally.asyncret", currentFn_);
            sw->addCase(builder_.getInt32(5), arDispBB);
            builder_.SetInsertPoint(arDispBB);
            if (outerFr) {
                // fix #1567 / §29.7/§28.10 (v0.182): dispose the scopes between
                // this finally and the next-outer finally (reverse establishment)
                // after this finally body, before routing the async return onward
                // — the mode-5 twin of mode 1.
                emitDisposalsInRange(outerFr->disposeDepthAtEntry, bodyScopeIdx);
                builder_.CreateStore(builder_.getInt32(5), outerFr->exitModSlot);
                if (asyncRetSlot && outerFr->asyncRetSlot && asyncResultTy_) {
                    llvm::Value* rv =
                        builder_.CreateLoad(asyncResultTy_, asyncRetSlot);
                    builder_.CreateStore(rv, outerFr->asyncRetSlot);
                }
                builder_.CreateBr(outerFr->finNormalBB);
            } else {
                // fix #1567 / §29.7/§28.10 (v0.182): no enclosing finally —
                // dispose every remaining scope established before this try
                // AFTER the finally body, then complete the outer task.
                emitDisposalsInRange(0, bodyScopeIdx);
                llvm::Value* rv = (asyncRetSlot && asyncResultTy_)
                    ? builder_.CreateLoad(asyncResultTy_, asyncRetSlot)
                    : nullptr;
                emitAsyncComplete(rv);
                builder_.CreateRetVoid();
            }
        }

        // finUnwindBB: exception exit — run finally, then re-propagate.
        builder_.SetInsertPoint(finUnwindBB);
        // §29.7 (#623): if we are inside an iterator state machine, mark the
        // iterator as done (state=-1) BEFORE running the finally body.  The
        // finally will run via this unwinding path; when the consumer's for-loop
        // cleanup subsequently calls Dispose, Dispose must see state=-1 and be a
        // no-op — otherwise the finally would run a second time.  We set -1
        // before the finally (not after) so that any finally-body code that
        // calls MoveNext/Dispose on the same iterator also sees it as done.
        // smStateGEP_ is non-null exactly when we are emitting inside a
        // sync/async iterator MoveNext / Resume function.
        if (smStateGEP_) {
            builder_.CreateStore(builder_.getInt32(-1), smStateGEP_);
        }
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
            llvm::Value* inflight = builder_.CreateLoad(ptrTy(), emitExcSlot());
            builder_.CreateStore(inflight, pendingExcSlot);
            asyncHandlerSpillSlots_.push_back(pendingExcSlot);
            builder_.CreateStore(
                llvm::ConstantPointerNull::get(ptrTy()), emitExcSlot());
            // #1149 (sibling of #1108): a `throw` inside the finally body
            // SUPERSEDES the captured in-flight exception E1 (§31.2). E1 is then
            // abandoned and must be freed exactly once — otherwise it leaks (the
            // direct-throw path terminates the body and bypasses the supersede
            // check below; the finThrew edge only catches a fall-through-with-
            // bz$exc-set, which a direct throw never produces). Expose E1 as the
            // caught object for the duration of the finally body so a `throw`
            // routes through emitThrowValue's existing #397/#866 caught-exception
            // free — emitted at a live insert point before the throw's branch and
            // guarded against double-free by isDirectRethrow/throwLeavesArm. A
            // finally has no exception binding, so currentCaughtName_ stays empty
            // (never a direct-rethrow); caughtExcArmTarget_ = unwindBB() (= the
            // outer unwind target a finally throw branches to) makes throwLeavesArm
            // true. Restored immediately after so the supersede/restore logic and
            // outer scopes see the correct caught context. Mirrors the #1108 fix.
            llvm::Value* savedFinCaught = currentCaughtExc_;
            std::string_view savedFinCaughtName = currentCaughtName_;
            llvm::BasicBlock* savedFinArmTarget = caughtExcArmTarget_;
            currentCaughtExc_ = builder_.CreateLoad(ptrTy(), pendingExcSlot);
            currentCaughtName_ = std::string_view{};
            caughtExcArmTarget_ = unwindBB();
            bool finUTerm = lowerScopedBlock(trys.finallyBody);
            currentCaughtExc_ = savedFinCaught;
            currentCaughtName_ = savedFinCaughtName;
            caughtExcArmTarget_ = savedFinArmTarget;
            if (!finUTerm) {
                // Restore the captured in-flight exception and re-propagate. If
                // the finally itself threw (set bz$exc non-null), that NEW
                // exception supersedes the original (§31.2 finally-throws
                // semantics); keep it. Otherwise restore the original.
                llvm::Value* afterFin = builder_.CreateLoad(ptrTy(), emitExcSlot());
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
                builder_.CreateStore(orig, emitExcSlot());
                builder_.CreateBr(propBB);
                builder_.SetInsertPoint(propBB);
                // #467 / §28.3: same intervening-scope discharge as the
                // synchronous path below. Emitted after the in-flight exception
                // is restored to bz$exc so the enclosing handler sees the right
                // object; the disposals themselves do not touch bz$exc.
                emitUnwindDisposalsInRange(savedDepth, thisTryDepth);
                builder_.CreateBr(outerUnwind);
            }
        } else {
            // #720 / §31.2: the synchronous unwind-path finally. Capture the
            // in-flight exception and CLEAR bz$exc before running the finally —
            // otherwise a `new`/call inside the finally body hits
            // emitNewPendingCheck/emitPendingCheck, sees the still-pending
            // ORIGINAL exception, and unwinds it immediately, BEFORE the finally's
            // own `throw` executes — silently swallowing the finally's exception.
            // After the finally: if it threw (bz$exc non-null), that NEW exception
            // supersedes (§31.2); otherwise restore the original. Mirrors the
            // async branch above, minus the cross-thread frame-spill.
            llvm::AllocaInst* pendingExcSlot = entryAlloca(ptrTy());
            builder_.CreateStore(builder_.CreateLoad(ptrTy(), emitExcSlot()),
                                 pendingExcSlot);
            builder_.CreateStore(
                llvm::ConstantPointerNull::get(ptrTy()), emitExcSlot());
            // #1149: expose the captured in-flight E1 as the caught object for
            // the finally body so a `throw` inside frees E1 exactly once via
            // emitThrowValue's #397/#866 path (§31.2 supersede). See the async
            // branch above for the full rationale + double-free guards.
            llvm::Value* savedFinCaught = currentCaughtExc_;
            std::string_view savedFinCaughtName = currentCaughtName_;
            llvm::BasicBlock* savedFinArmTarget = caughtExcArmTarget_;
            currentCaughtExc_ = builder_.CreateLoad(ptrTy(), pendingExcSlot);
            currentCaughtName_ = std::string_view{};
            caughtExcArmTarget_ = unwindBB();
            bool finBodyTerm = lowerScopedBlock(trys.finallyBody);
            currentCaughtExc_ = savedFinCaught;
            currentCaughtName_ = savedFinCaughtName;
            caughtExcArmTarget_ = savedFinArmTarget;
            if (!finBodyTerm) {
                llvm::Value* afterFin =
                    builder_.CreateLoad(ptrTy(), emitExcSlot());
                llvm::Value* finThrew = builder_.CreateICmpNE(
                    afterFin, llvm::ConstantPointerNull::get(ptrTy()),
                    "fin.threw");
                auto* restoreBB = llvm::BasicBlock::Create(
                    llvm_, "finally.restore", currentFn_);
                auto* propBB = llvm::BasicBlock::Create(
                    llvm_, "finally.reraise", currentFn_);
                builder_.CreateCondBr(finThrew, propBB, restoreBB);
                builder_.SetInsertPoint(restoreBB);
                builder_.CreateStore(
                    builder_.CreateLoad(ptrTy(), pendingExcSlot), emitExcSlot());
                builder_.CreateBr(propBB);
                builder_.SetInsertPoint(propBB);
                // #467 / §28.3: dispose owned locals/using-vars in the scopes
                // between this try and the enclosing handler (the throw site
                // only disposed down to thisTryDepth).
                emitUnwindDisposalsInRange(savedDepth, thisTryDepth);
                builder_.CreateBr(outerUnwind);
            }
        }
        // fix #1263 / §28.3: drop the try body scope pushed in the frame setup.
        // Every block that disposes it (mode 0's fallDispBB, modes 1-5,
        // finUnwindBB, and a throw from the finally body) has been emitted, so
        // the scope's Disposals have been baked into the IR; safe to drop the
        // C++-side entry now.
        disposeStack_.pop_back();
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

    // #1387 Item 1 / §28.11 / §22.4: push a dedicated condition scope so
    // out-inline locals declared in an if/elif CONDITION (e.g.
    // `if s.TryPop(out using var top)`) are disposed at the END of the
    // if-statement, not at the end of the enclosing block.
    //
    // ensureOutVarSlots (called inside lowerExpr for the condition call)
    // registers each out-inline local's disposal into disposeStack_.back().
    // Without this scope they land in the OUTER block's scope and are never
    // disposed until the outer block exits — causing a leak (return 122
    // instead of 121 in the test). With this scope:
    //   • normal-exit (merge): emitDisposals fires at the if's end → correct.
    //   • terminating paths (return/throw): emitAllDisposals includes this
    //     scope → correct (no disposal missed, no double-free).
    // The dynamicDispose entries are null-guarded, so paths where TryPop
    // returned false (slot null) are always safe.  // per §28.3, §22.4, §28.11
    disposeStack_.emplace_back();  // condition scope

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
        // Dispose any out-inline locals from the condition scope at the
        // if-statement's end (normal-exit path). Paths that terminated
        // (return/throw) already emitted these via emitAllDisposals; the
        // null-guard in dynamicDispose makes the slot-null paths safe.
        emitDisposals(disposeStack_.back());
        // fix #1602 / §28.10 / §28.11: emitDisposals above runs with the
        // default isUnwind=false, so any `unwindOnly` entry registered while
        // lowering the condition — the #1466 pre-declared out-owned recipient
        // written by the condition's own call, or an inline `out owned var` /
        // `out using var` recipient (§22.4) not yet discharged — was SKIPPED,
        // not fired: it is a pure exception-edge backstop, not a normal-exit
        // disposal. Popping the condition scope right after would silently
        // drop that still-live entry from disposeStack_ entirely, so a LATER
        // throw — anywhere in the enclosing block, after this if-statement,
        // before the recipient's real discharge point — would never find it,
        // and the recipient would leak (bounded leak, confirmed via valgrind
        // in #1602; never a double-free, since the entry is null-guarded and
        // an explicit discharge nulls the slot before any later dispose
        // could run). Migrate any surviving `unwindOnly` entries into the
        // enclosing scope instead of discarding them: per §22.4, the
        // recipient's true lexical home is "the enclosing block, from the
        // call onward" — it outlives this transient condition scope (#1387
        // Item 1), so its unwind backstop must too. `dynamicDispose` entries
        // need no such migration: they already fired, unconditionally, in
        // the emitDisposals call above (#1387's if-end auto-drop timing is
        // unaffected).
        if (disposeStack_.size() >= 2) {
            std::vector<Disposal>& condScope = disposeStack_.back();
            std::vector<Disposal>& outerScope =
                disposeStack_[disposeStack_.size() - 2];
            for (const Disposal& d : condScope)
                if (d.unwindOnly) outerScope.push_back(d);
        }
        disposeStack_.pop_back();
        return false;
    }
    // All paths terminated; each branch's emitAllDisposals already included
    // the condition scope. Just pop it to restore the stack.
    disposeStack_.pop_back();
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


// §27.5 / #754: lower a for-loop body, pre-boxing the loop variable into a
// fresh per-iteration cell at body entry IF a closure in the body captures it.
//
// Background: `boxLocalSlot(forLoopVar=true)` allocates a fresh cell each
// iteration so a `()=>i` closure captures that iteration's value (§16.5 fresh
// binding). Previously this boxing was emitted lazily, at the capturing lambda
// site, and `lowerLambda` then rebound `locals_[&fs]` to the cell — so every
// later read of the loop variable in the body routes through the cell. When the
// lambda sits inside a conditionally-executed branch (a nested `if`), an
// iteration that skips that branch never allocates a fresh cell, so the
// cell-holder still points at a stale earlier iteration's cell; reads of the
// loop variable later in the body then observe the wrong value and loop control
// flow miscompiles (#754: a counted loop returned 1 instead of 21).
//
// Pre-boxing here, unconditionally at body entry, refreshes the cell on EVERY
// iteration regardless of branch outcomes. The capturing lambda's own
// `boxLocalSlot` call then finds the slot already boxed and reuses the
// body-entry cell (no double-box). The loop COUNTER stays on the original slot
// (the cond/inc blocks read and advance it), and body entry reads that slot to
// fill the fresh cell — so reassigning the loop variable in the body writes
// through the cell without disturbing iteration (capture-by-reference, §27.5).
//
// Iterator (state-machine) methods map the loop variable to a frame field and
// have their own resume/spill handling, so we never rebox there.
bool Emitter::lowerForBody(const ast::ForStmt& fs) {
    // #793 / §27.5: the fresh-per-iteration for-var capture cell (set below) when
    // the for-var is captured ONLY by non-escaping lambdas — eligible to free at
    // the end of each iteration's body.
    llvm::AllocaInst* forVarFreshCell = nullptr;
    if (!smStateStruct_) {
        auto it = locals_.find(&fs);
        if (it != locals_.end()) {
            llvm::AllocaInst* slot = it->second;
            if (slot && !cellVars_.count(slot) && forVarCapturedInBody(fs)) {
                llvm::AllocaInst* nb = nullptr;
                boxLocalSlot(slot, nb, /*forLoopVar=*/true);
                locals_[&fs] = nb;
                // #793: boxLocalSlot(forLoopVar=true) allocates a FRESH cell each
                // iteration, and those cells were never freed (a per-iteration
                // leak, O(iterations)). When collectCellFreeEligible proves the
                // for-var is captured ONLY by non-escaping lambdas (keyed by &fs —
                // it already classifies for-vars), each iteration's cell can be
                // freed at the end of that iteration's body, after the capturing
                // closure has been released. An ESCAPING capture is NOT in
                // cellFreeEligible_, so its cell is never freed here (it outlives
                // the loop — keeps leaking, the safe direction).
                if (currentFn_ == cellFreeEligibleFn_ &&
                    cellFreeEligible_.count(static_cast<const void*>(&fs)))
                    forVarFreshCell = nb;
            }
        }
    }

    // §29.2 / §28.3 v0.157 (#1329): per-iteration null-guarded disposal for an
    // owned iteration variable the ownership pass flagged as not always discharged.
    // The disposal scope is the OUTERMOST (pushed first here, popped last): it
    // wraps everything else including the capture-cell scope and the body's own
    // lowerScopedBlock scope, so LIFO order fires body disposals first, then this
    // one — correct for an object that was owned for the whole iteration. The scope
    // is registered BELOW the loopStack_ depth recorded by the caller (lowerOwnedSeqDrainFor
    // / lowerDictDrainFor / lowerAwaitFor push the loopStack_ entry BEFORE calling us,
    // and emitLoopDisposals unwinds all scopes ABOVE the recorded depth), so break
    // / continue / return via emitLoopDisposals fire this disposal too.
    //
    // dynamicDispose=true: null-guarded vtable Dispose + free. The slot may be
    // null if the body discharged the variable on this iteration (x.Dispose()
    // nulls the slot via explicitDisposeOwnedReceiverSlot; `move x` is patched to
    // null the slot when ownedIterVarAutoDropAtBodyEnd is set). free(null) is safe.
    //
    // Applies to BOTH sync drain-for loops (smStateStruct_==null) and async
    // await-for loops (smStateStruct_!=null). In the async case the iter var slot
    // is an entryAlloca that round-trips smAllocaToField_ on every await suspend
    // in the body, so at body-end the alloca always holds the current iteration's
    // value. boxedStorage() returns the alloca directly (not the frame field), which
    // is correct since dynamicDispose fires WITHIN the current Resume invocation
    // (no suspend between bodyBB and body-end).
    llvm::AllocaInst* iterVarSlot = nullptr;
    if (fs.ownedIterVarAutoDropAtBodyEnd) {
        auto sit = locals_.find(&fs);
        if (sit != locals_.end())
            iterVarSlot = sit->second;
    }

    if (iterVarSlot) {
        // §30.5 / §29.2 (#1888): a drained `OwningTask<T>`/`Task<T>` iteration
        // variable (`for var task in tasks.Drain() do results.Add(await task);
        // end`) is a TASK, not a class object — its frame has no Bezel vtable, so
        // the class-vtable `dynamicDispose` below (emitDisposeAndReclaim) would
        // read a bogus vtable slot and crash. A bound task's frame is never freed
        // by the `await` (freeTaskAfter is only set for an anonymous CallExpr
        // operand, LlvmBackend_Async.cpp lowerAwait); it is freed at scope/body-end
        // via `bz_task_fire_and_forget`, EXACTLY as a plain `var t := M(); await t;`
        // task local is (the by-type `d.task` registration in lowerStmt's LocalDecl
        // arm above).
        // Mirror that here for the iter var: dispose it via the task path
        // (fire-and-forget, idempotent on a spent/complete task), carrying the
        // per-T owned-result disposer for an `OwningTask<T:class>` so a drained-but-
        // never-awaited task also reclaims its produced result (a no-op once the
        // await has taken/nulled it). // per §30.5 / §29.2
        const sema::TypeRef* iterElemTy = nullptr;
        if (fs.iterable) {
            if (const sema::TypeRef* itTy = ctx_.exprType(*fs.iterable))
                if (!itTy->genericArgs.empty() && itTy->genericArgs[0])
                    iterElemTy = resolveTypeParamRef(itTy->genericArgs[0]);
        }
        bool iterVarIsTask = isTaskTypeName(iterElemTy);
        // Push outermost scope: per-iteration iter-var disposal.
        disposeStack_.emplace_back();
        Disposal id{iterVarSlot, nullptr};
        if (iterVarIsTask) {
            id.task = true;
            if (iterElemTy && !iterElemTy->genericArgs.empty() &&
                iterElemTy->genericArgs[0]) {
                ElementPolicy pol =
                    elementPolicyFor(iterElemTy->genericArgs[0],
                                     /*owningFamily=*/true);
                if (pol.cat == ElementPolicy::Category::ClassOwned)
                    id.taskOwnedResultDisposer =
                        pol.disposerThunk;
            }
        } else {
            id.dynamicDispose = true;
        }
        disposeStack_.back().push_back(id);
    }

    bool terminated = false;
    if (!forVarFreshCell) {
        terminated = lowerScopedBlock(fs.body);
    } else {
        // #793: wrap the body in a scope holding the for-var cell free (inner
        // scope, fires before the iter-var outer scope by LIFO order). lowerScopedBlock
        // opens the body's own innermost scope.
        disposeStack_.emplace_back();
        Disposal d{forVarFreshCell, nullptr};
        d.boxedCellFree = true;
        disposeStack_.back().push_back(d);
        terminated = lowerScopedBlock(fs.body);
        if (!terminated) emitDisposals(disposeStack_.back());
        disposeStack_.pop_back();
    }

    if (iterVarSlot) {
        if (!terminated) emitDisposals(disposeStack_.back());
        disposeStack_.pop_back();
    }
    return terminated;
}


// §16.3 (v0.183, #1586): see declaration (LlvmBackend_impl.hpp) for the
// dense-index bijection this implements. `cur` is assumed to already be a
// valid (non-surrogate) char code point, as every value this is applied to
// either came from a char literal endpoint (surrogate literals are compile
// errors, §18.3/§18.8) or was itself produced by a prior call to this
// function.
llvm::Value* Emitter::emitCharRangeStep(llvm::Value* cur, llvm::Value* step) {
    llvm::Value* gapLo = builder_.getInt32(0xD800);
    llvm::Value* gapSize = builder_.getInt32(0x800);
    // Forward map: d = cur < 0xD800 ? cur : cur - 0x800.
    llvm::Value* curAboveGap = builder_.CreateICmpSGE(cur, gapLo, "crs.above");
    llvm::Value* curShifted =
        builder_.CreateSub(cur, gapSize, "crs.shifted");
    llvm::Value* d = builder_.CreateSelect(curAboveGap, curShifted, cur, "crs.d");
    // Step in the dense (gap-free) space — plain addition, ascending or
    // descending per step's sign.
    llvm::Value* d2 = builder_.CreateAdd(d, step, "crs.d2");
    // Inverse map: cp = d2 < 0xD800 ? d2 : d2 + 0x800.
    llvm::Value* d2AboveGap = builder_.CreateICmpSGE(d2, gapLo, "crs.d2above");
    llvm::Value* d2Shifted = builder_.CreateAdd(d2, gapSize, "crs.d2shifted");
    return builder_.CreateSelect(d2AboveGap, d2Shifted, d2, "crs.next");
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
        // §16.3 (v0.183, #1586): a Range<char> steps across the UTF-16
        // surrogate gap instead of the plain arithmetic every other Range<T>
        // uses (see emitCharRangeStep).
        bool isCharRange = iterTy && !iterTy->genericArgs.empty() &&
            iterTy->genericArgs[0] &&
            iterTy->genericArgs[0]->kind == sema::TypeRef::Kind::Primitive &&
            iterTy->genericArgs[0]->primitive == sema::PrimitiveType::Char;

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

        // §16.4: the iteration direction is determined by the step's SIGN.
        // Unlike the inline-literal path below (where the step is a compile-time
        // constant), the step here is loaded from the Range struct and is
        // generally NOT a ConstantInt, so direction must be decided at RUNTIME.
        // A range whose bounds-direction disagrees with its step sign produces
        // an empty iteration — this falls out naturally from selecting the
        // step-sign-derived comparison against the (unchanged) bounds: e.g.
        // `1..5 step -1` takes the descending branch and `1 >= 5` is false on
        // entry, so the body never runs. (The prior code only ever set
        // `descending` for a constant step, so a Range variable always used the
        // ascending comparison — both miscompiling `5..1 step -1` into an empty
        // loop and turning `1..5 step -1` into a non-terminating descent.)
        llvm::Value* stepNeg =
            builder_.CreateICmpSLT(step, builder_.getInt32(0), "rfv.desc");

        // #1243 §21.4: use a hidden counter (rfvCounter) so that assignment to
        // the loop variable in the body does not alter the iteration count.
        // rfvVar is the body-visible slot; it is reloaded from rfvCounter at
        // the top of each iteration so each iteration starts with the correct
        // value regardless of what the body wrote to i.
        llvm::AllocaInst* rfvCounter = entryAlloca(i32());
        llvm::AllocaInst* rfvVar     = entryAlloca(i32());
        locals_[&fs] = rfvVar;
        builder_.CreateStore(lo, rfvCounter);

        auto* condBB = llvm::BasicBlock::Create(llvm_, "rfv.cond", currentFn_);
        auto* bodyBB = llvm::BasicBlock::Create(llvm_, "rfv.body", currentFn_);
        auto* incBB  = llvm::BasicBlock::Create(llvm_, "rfv.inc",  currentFn_);
        auto* endBB  = llvm::BasicBlock::Create(llvm_, "rfv.end",  currentFn_);
        // §16.4 (v0.183, #1623): a computed step of zero raises ArgumentException
        // at GetEnumerator time. The compiler-optimized counted loop bypasses the
        // IEnumerable protocol (§16.5 "Compiler optimization") and must keep
        // observable behavior identical, so replicate the check here — otherwise a
        // step-0 Range variable enters a counted loop that never advances (the
        // induction adds 0) and hangs. A stepless range stores step=1 (see the
        // RangeExpr-in-value-position lowering), so this only fires for an explicit
        // computed step-0.
        emitGuardThrow(builder_.CreateICmpEQ(step, builder_.getInt32(0), "rfv.step0"),
                       "ArgumentException");
        builder_.CreateBr(condBB);

        builder_.SetInsertPoint(condBB);
        llvm::Value* cur = builder_.CreateLoad(i32(), rfvCounter);
        // Direction-aware bound check: pick the ascending (cur <=/< hi) or
        // descending (cur >=/> hi) comparison by the runtime step sign, using
        // the incl flag from the struct for the </<= vs <=/>= choice.
        llvm::Value* ascLE  = builder_.CreateICmpSLE(cur, hi);
        llvm::Value* ascLT  = builder_.CreateICmpSLT(cur, hi);
        llvm::Value* asc    = builder_.CreateSelect(incl, ascLE, ascLT);
        llvm::Value* descGE = builder_.CreateICmpSGE(cur, hi);
        llvm::Value* descGT = builder_.CreateICmpSGT(cur, hi);
        llvm::Value* desc   = builder_.CreateSelect(incl, descGE, descGT);
        llvm::Value* cond   = builder_.CreateSelect(stepNeg, desc, asc, "rfv.cond");
        builder_.CreateCondBr(cond, bodyBB, endBB);

        builder_.SetInsertPoint(bodyBB);
        // #1243: copy hidden counter into the per-iteration loop variable so
        // the body starts each iteration with the right value of i.
        builder_.CreateStore(
            builder_.CreateLoad(i32(), rfvCounter, "rfv.iv"), rfvVar);
        loopStack_.push_back({incBB, endBB, disposeStack_.size()});
        bool term = lowerForBody(fs);
        loopStack_.pop_back();
        if (!term) builder_.CreateBr(incBB);

        builder_.SetInsertPoint(incBB);
        llvm::Value* v = builder_.CreateLoad(i32(), rfvCounter);
        llvm::Value* nextV = isCharRange ? emitCharRangeStep(v, step)
                                          : builder_.CreateAdd(v, step);
        builder_.CreateStore(nextV, rfvCounter);
        builder_.CreateBr(condBB);

        builder_.SetInsertPoint(endBB);
        return true;
    }

    if (!range || !range->lo || !range->hi) return false;
    llvm::Value* lo = lowerExpr(*range->lo);
    llvm::Value* hi = lowerExpr(*range->hi);
    if (!lo || !hi) return false;

    // §16.3 (v0.183, #1586): the literal-range counted loop below must match
    // emitCharRangeStep's surrogate-gap-skipping increment for Range<char>,
    // same as the Range-variable path above.
    auto* litIterTy = ctx_.exprType(*fs.iterable);
    bool isCharRange = litIterTy && !litIterTy->genericArgs.empty() &&
        litIterTy->genericArgs[0] &&
        litIterTy->genericArgs[0]->kind == sema::TypeRef::Kind::Primitive &&
        litIterTy->genericArgs[0]->primitive == sema::PrimitiveType::Char;

    // §16.4: resolve the step (default = 1; negative steps descend).
    llvm::Value* stepVal = llvm::ConstantInt::get(lo->getType(), 1);
    bool descending = false;
    if (range->step) {
        llvm::Value* s = lowerExpr(*range->step);
        if (!s) return false;
        auto* cs = llvm::dyn_cast<llvm::ConstantInt>(s);
        if (cs && cs->isZero()) return false;  // zero constant step: empty, defer
        if (!cs) {
            // §16.4 (sweep #8): a RUNTIME-computed step (`for var i in 0..10 step s`).
            // The constant-step path below picks the loop direction at compile
            // time; a non-constant step must pick it at runtime by the step's
            // sign. Emit a direction-aware loop, mirroring the Range-VARIABLE path
            // above (previously this case bailed to the "not implemented" catch-all).
            if (s->getType() != lo->getType())
                s = builder_.CreateSExtOrTrunc(s, lo->getType());
            // §16.4 (v0.183, #1623): a computed step of zero raises ArgumentException
            // at GetEnumerator time. The constant-step-0 case is handled above
            // (cs && cs->isZero()); a RUNTIME step-0 reaches here and must raise
            // instead of entering the counted loop, which would add 0 forever and
            // hang. The optimized counted loop must keep GetEnumerator's observable
            // behavior (§16.5 "Compiler optimization").
            emitGuardThrow(builder_.CreateICmpEQ(
                               s, llvm::ConstantInt::get(lo->getType(), 0), "rf.step0"),
                           "ArgumentException");
            llvm::Value* stepNeg = builder_.CreateICmpSLT(
                s, llvm::ConstantInt::get(lo->getType(), 0), "rf.desc");
            // #1243 §21.4: hidden counter + body-visible var (per rfv path above).
            llvm::AllocaInst* rfCounter = entryAlloca(lo->getType());
            llvm::AllocaInst* rfVar     = entryAlloca(lo->getType());
            locals_[&fs] = rfVar;
            builder_.CreateStore(lo, rfCounter);
            auto* condBB = llvm::BasicBlock::Create(llvm_, "rf.cond", currentFn_);
            auto* bodyBB = llvm::BasicBlock::Create(llvm_, "rf.body", currentFn_);
            auto* incBB  = llvm::BasicBlock::Create(llvm_, "rf.inc",  currentFn_);
            auto* endBB  = llvm::BasicBlock::Create(llvm_, "rf.end",  currentFn_);
            builder_.CreateBr(condBB);
            builder_.SetInsertPoint(condBB);
            llvm::Value* cur = builder_.CreateLoad(lo->getType(), rfCounter);
            llvm::Value* asc = range->inclusive
                ? builder_.CreateICmpSLE(cur, hi)
                : builder_.CreateICmpSLT(cur, hi);
            llvm::Value* desc = range->inclusive
                ? builder_.CreateICmpSGE(cur, hi)
                : builder_.CreateICmpSGT(cur, hi);
            llvm::Value* cond =
                builder_.CreateSelect(stepNeg, desc, asc, "rf.cnd");
            builder_.CreateCondBr(cond, bodyBB, endBB);
            builder_.SetInsertPoint(bodyBB);
            // #1243: copy hidden counter into the per-iteration loop variable.
            builder_.CreateStore(
                builder_.CreateLoad(lo->getType(), rfCounter, "rf.iv"), rfVar);
            loopStack_.push_back({incBB, endBB, disposeStack_.size()});
            bool term = lowerForBody(fs);
            loopStack_.pop_back();
            if (!term) builder_.CreateBr(incBB);
            builder_.SetInsertPoint(incBB);
            llvm::Value* v = builder_.CreateLoad(lo->getType(), rfCounter);
            llvm::Value* nextV =
                isCharRange ? emitCharRangeStep(v, s) : builder_.CreateAdd(v, s);
            builder_.CreateStore(nextV, rfCounter);
            builder_.CreateBr(condBB);
            builder_.SetInsertPoint(endBB);
            return true;
        }
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
        bool term = lowerForBody(fs);
        loopStack_.pop_back();
        if (!term) builder_.CreateBr(incBB);
        builder_.SetInsertPoint(incBB);
        llvm::Value* v = builder_.CreateLoad(slot->getAllocatedType(), slot);
        llvm::Value* next = isCharRange ? emitCharRangeStep(v, stepVal)
                                         : builder_.CreateAdd(v, stepVal);
        builder_.CreateStore(next, slot);
        builder_.CreateBr(condBB);
        builder_.SetInsertPoint(endBB);
        return true;
    }

    // #1243 §21.4: introduce a separate body-visible loop variable (varSlot)
    // so that assignment to i in the body does not alter the hidden counter
    // (slot).  The SM path above already returned; that path keeps using
    // 'slot' directly because it must survive yield via smAllocaToField_ and
    // adding a second frame field would require smLocalFields_ to reserve an
    // extra entry per range ForStmt (deferred to a follow-up).
    llvm::AllocaInst* varSlot = entryAlloca(lo->getType());
    locals_[&fs] = varSlot;

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
    // #1243: copy hidden counter into the per-iteration loop variable so the
    // body sees the correct value of i and its mutations stay local.
    builder_.CreateStore(
        builder_.CreateLoad(slot->getAllocatedType(), slot, "for.iv"), varSlot);
    loopStack_.push_back({incBB, endBB, disposeStack_.size()});
    bool term = lowerForBody(fs);
    loopStack_.pop_back();
    if (!term) builder_.CreateBr(incBB);

    // inc: counter := counter + step (body mutations to varSlot are ignored).
    // §16.3 (v0.183, #1586): for a Range<char>, step across the surrogate gap
    // instead of plain addition (emitCharRangeStep).
    builder_.SetInsertPoint(incBB);
    llvm::Value* v = builder_.CreateLoad(slot->getAllocatedType(), slot);
    llvm::Value* next = isCharRange ? emitCharRangeStep(v, stepVal)
                                     : builder_.CreateAdd(v, stepVal);
    builder_.CreateStore(next, slot);
    builder_.CreateBr(condBB);

    builder_.SetInsertPoint(endBB);
    return true;
}


// stdlib §4.13 / §29.2 (#728): `for var x in <LinkedList<T>>` (and
// OwningLinkedList<T>). A linked list is NOT array-backed, so lowerArrayFor's
// isCollectionTypeName guard excludes it and it would otherwise hit the
// not-yet-implemented error. Walk the node chain with the runtime cursor
// (bz_llist_first_node / bz_llist_node_value_ptr / bz_llist_node_next) already
// used by the First/Last/Find lowering. Mirrors the span/array loop structure
// (cond/body/inc/end + loopStack_ for break/continue).
bool Emitter::lowerLinkedListFor(const ast::ForStmt& fs) {
    if (!fs.iterable) return false;
    auto* iterTy = ctx_.exprType(*fs.iterable);
    if (!isLinkedListTypeName(iterTy)) return false;
    llvm::Value* listObj = lowerExpr(*fs.iterable);
    if (!listObj) return false;
    llvm::Type* elemTy = i32();
    if (iterTy && !iterTy->genericArgs.empty() && iterTy->genericArgs[0])
        elemTy = lowerTypeRef(iterTy->genericArgs[0]);
    auto* firstFn = declareC("bz_llist_first_node", ptrTy(), {ptrTy()});
    auto* valFn = declareC("bz_llist_node_value_ptr", ptrTy(), {ptrTy()});
    auto* nextFn = declareC("bz_llist_node_next", ptrTy(), {ptrTy()});
    // §29.12 / issue #1232: capture modification epoch before first node fetch.
    auto* getModLlistFn = declareC("bz_llist_get_mod", i64(), {ptrTy()});
    llvm::Value* llEpochVal = builder_.CreateCall(getModLlistFn, {listObj}, "ll.epoch");
    auto* llEpochSlot = entryAlloca(i64());
    builder_.CreateStore(llEpochVal, llEpochSlot);
    auto* nodeSlot = entryAlloca(ptrTy());
    builder_.CreateStore(builder_.CreateCall(firstFn, {listObj}), nodeSlot);
    auto* varSlot = entryAlloca(elemTy);
    locals_[&fs] = varSlot;
    // #747 R42 + §29.12/#1232: spill node, loop-var, and epoch into the async
    // frame so they survive a yield/await suspend in the body.
    registerForLoopSpillSlots(fs, {nodeSlot, varSlot, llEpochSlot});
    auto* condBB = llvm::BasicBlock::Create(llvm_, "ll.cond", currentFn_);
    auto* bodyBB = llvm::BasicBlock::Create(llvm_, "ll.body", currentFn_);
    auto* incBB = llvm::BasicBlock::Create(llvm_, "ll.inc", currentFn_);
    auto* endBB = llvm::BasicBlock::Create(llvm_, "ll.end", currentFn_);
    builder_.CreateBr(condBB);
    builder_.SetInsertPoint(condBB);
    // §29.12 / issue #1232: check epoch on every MoveNext — mismatch → InvalidOperationException.
    {
        llvm::Value* curMod = builder_.CreateCall(getModLlistFn, {listObj}, "ll.curmod");
        llvm::Value* epV    = builder_.CreateLoad(i64(), llEpochSlot, "ll.epv");
        emitGuardThrow(builder_.CreateICmpNE(curMod, epV, "ll.modchk"),
                       "InvalidOperationException");
    }
    llvm::Value* node = builder_.CreateLoad(ptrTy(), nodeSlot);
    builder_.CreateCondBr(
        builder_.CreateICmpNE(node, llvm::ConstantPointerNull::get(ptrTy())),
        bodyBB, endBB);
    builder_.SetInsertPoint(bodyBB);
    llvm::Value* nodeB = builder_.CreateLoad(ptrTy(), nodeSlot);
    llvm::Value* vp = builder_.CreateCall(valFn, {nodeB});
    builder_.CreateStore(builder_.CreateLoad(elemTy, vp), varSlot);
    loopStack_.push_back({incBB, endBB, disposeStack_.size()});
    bool term = lowerForBody(fs);
    loopStack_.pop_back();
    if (!term) builder_.CreateBr(incBB);
    builder_.SetInsertPoint(incBB);
    llvm::Value* nodeC = builder_.CreateLoad(ptrTy(), nodeSlot);
    builder_.CreateStore(builder_.CreateCall(nextFn, {nodeC}), nodeSlot);
    builder_.CreateBr(condBB);
    builder_.SetInsertPoint(endBB);
    return true;
}

void Emitter::registerForLoopSpillSlots(
        const ast::ForStmt& fs,
        std::initializer_list<llvm::AllocaInst*> slots) {
    // §29.3 / §30.7 (#747 R42) — see the declaration comment. All three suspend
    // kinds now use the side spill area + the fixupSuspendDominance pass that
    // consumes asyncHandlerSpillSlots_: Await / AsyncYield (async resume) and
    // Yield (sync MoveNext — emitIteratorMethod sets asyncSpillPtrField_ and runs
    // the fixup when any slot is registered).
    if (!smStateStruct_) return;
    if (smKind_ != SuspendKind::Await && smKind_ != SuspendKind::AsyncYield &&
        smKind_ != SuspendKind::Yield)
        return;
    if (!containsYield(fs.body) && !containsAwait(fs.body)) return;
    for (llvm::AllocaInst* s : slots)
        if (s) asyncHandlerSpillSlots_.push_back(s);
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
                if (!ev) {
                    // #1386: ICE — lowerExpr returned null for a for-in
                    // collection literal element.
                    ctx_.diagnostics().report(
                        Severity::Error,
                        cl->elements[i]->span.begin,
                        "internal: lowerExpr returned null for for-in "
                        "collection literal element — #1386");
                    continue;
                }
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
            lowerForBody(fs);
            llvm::Value* inc2 = builder_.CreateAdd(
                builder_.CreateLoad(i32(), idxSlot2), builder_.getInt32(1));
            builder_.CreateStore(inc2, idxSlot2);
            builder_.CreateBr(condBB2);
            builder_.SetInsertPoint(endBB2);
            return true;
        }
    }
    // §4.15 (#549): for var x in span — direct indexed loop over ArraySpan<T>
    // or ReadOnlyArraySpan<T> (per spec §29.2, desugared as indexed loop over
    // span.Start .. span.Start+span.Length with bz_array_ptr_at on Source).
    {
        auto* iterTy = ctx_.exprType(*fs.iterable);
        if (isSpanLikeTypeName(iterTy) || isIReadOnlySpanType(iterTy)) {
            llvm::Value* spanVal = nullptr;
            if (isIReadOnlySpanType(iterTy)) {
                // §29.2 / §4.16 (#1262): an IReadOnlySpan<T> value is a box
                // { vtable, rc, bz.arrayspan } with the span struct at byte
                // offset 16 (#970). Unbox it and iterate it with the SAME direct
                // indexed loop as a concrete span (spec §29.2 mandates the
                // indexed-loop desugaring). The unboxed `src` field is the same
                // bz_array_t* the concrete path uses, so bz_array_ptr_at applies.
                llvm::Value* boxPtr = lowerExpr(*fs.iterable);
                if (!boxPtr) return false;
                llvm::Value* spanGep = builder_.CreateGEP(
                    llvm::Type::getInt8Ty(llvm_), boxPtr,
                    builder_.getInt64(16), "irs.spanp");
                spanVal = builder_.CreateLoad(arraySpanStructType(), spanGep,
                                              "irs.span");
            } else {
                spanVal = lowerExpr(*fs.iterable);
                if (!spanVal) return false;
            }
            auto* spanSt = arraySpanStructType();
            auto* spanSlot = entryAlloca(spanSt);
            builder_.CreateStore(spanVal, spanSlot);
            llvm::Value* src  = builder_.CreateLoad(ptrTy(),
                builder_.CreateStructGEP(spanSt, spanSlot, 0), "sp.src");
            llvm::Value* base = builder_.CreateLoad(i32(),
                builder_.CreateStructGEP(spanSt, spanSlot, 1), "sp.st");
            llvm::Value* slen = builder_.CreateLoad(i32(),
                builder_.CreateStructGEP(spanSt, spanSlot, 2), "sp.len");
            llvm::Type* elemTy = i32();
            int64_t esz = 8;
            if (iterTy && !iterTy->genericArgs.empty() && iterTy->genericArgs[0]) {
                if (auto* et = lowerTypeRef(iterTy->genericArgs[0])) elemTy = et;
                esz = collectionElemSize(iterTy);
            }
            auto* ptrAtFn = declareC("bz_array_ptr_at", ptrTy(), {ptrTy(), i64(), i64()});
            auto* idxSlot = entryAlloca(i32());
            builder_.CreateStore(builder_.getInt32(0), idxSlot);
            auto* varSlot = entryAlloca(elemTy);
            locals_[&fs] = varSlot;
            auto* condBB = llvm::BasicBlock::Create(llvm_, "sp.cond", currentFn_);
            auto* bodyBB = llvm::BasicBlock::Create(llvm_, "sp.body", currentFn_);
            auto* incBB  = llvm::BasicBlock::Create(llvm_, "sp.inc",  currentFn_);
            auto* endBB  = llvm::BasicBlock::Create(llvm_, "sp.end",  currentFn_);
            builder_.CreateBr(condBB);
            builder_.SetInsertPoint(condBB);
            llvm::Value* idx32 = builder_.CreateLoad(i32(), idxSlot);
            builder_.CreateCondBr(
                builder_.CreateICmpSLT(idx32, slen), bodyBB, endBB);
            builder_.SetInsertPoint(bodyBB);
            llvm::Value* idx32b = builder_.CreateLoad(i32(), idxSlot);
            llvm::Value* absIdx = builder_.CreateAdd(
                builder_.CreateSExt(base, i64()),
                builder_.CreateSExt(idx32b, i64()), "sp.ai");
            llvm::Value* ep = builder_.CreateCall(ptrAtFn,
                {src, absIdx, builder_.getInt64(esz)});
            llvm::Value* elem = builder_.CreateLoad(elemTy, ep);
            builder_.CreateStore(elem, varSlot);
            loopStack_.push_back({incBB, endBB, disposeStack_.size()});
            bool termSp = lowerForBody(fs);
            loopStack_.pop_back();
            if (!termSp) builder_.CreateBr(incBB);
            builder_.SetInsertPoint(incBB);
            llvm::Value* idx32c = builder_.CreateLoad(i32(), idxSlot);
            builder_.CreateStore(
                builder_.CreateAdd(idx32c, builder_.getInt32(1)), idxSlot);
            builder_.CreateBr(condBB);
            builder_.SetInsertPoint(endBB);
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
            // §29.12 / #1232: capture mod epoch before the loop starts.
            auto* getModFn = declareC("bz_array_get_mod", i64(), {ptrTy()});
            llvm::Value* epochVal = builder_.CreateCall(getModFn, {arrObj}, "arr.epoch");
            auto* epochSlot = entryAlloca(i64());
            builder_.CreateStore(epochVal, epochSlot);
            llvm::Value* len = builder_.CreateCall(lenFn, {arrObj});
            auto* idxSlotR = entryAlloca(i64());
            builder_.CreateStore(builder_.getInt64(0), idxSlotR);
            auto* varSlotR = entryAlloca(elemTy);
            locals_[&fs] = varSlotR;
            // #747 R42: spill the index + loop-var + epoch into the async frame
            // so they survive a yield/await suspend in the body (else garbage on
            // resume). epochSlot must also be spilled: if a resume reads a stale
            // epoch, it would spuriously throw InvalidOperationException (§29.12).
            registerForLoopSpillSlots(fs, {idxSlotR, varSlotR, epochSlot});
            auto* condBBR = llvm::BasicBlock::Create(llvm_, "rt.cond", currentFn_);
            auto* bodyBBR = llvm::BasicBlock::Create(llvm_, "rt.body", currentFn_);
            auto* incBBR  = llvm::BasicBlock::Create(llvm_, "rt.inc",  currentFn_);
            auto* endBBR  = llvm::BasicBlock::Create(llvm_, "rt.end",  currentFn_);
            builder_.CreateBr(condBBR);
            builder_.SetInsertPoint(condBBR);
            // §29.12 / #1232: check for concurrent modification on every MoveNext.
            // If the collection was mutated since the loop started, throw
            // InvalidOperationException before reading the next element.
            {
                llvm::Value* curMod = builder_.CreateCall(getModFn, {arrObj}, "arr.curmod");
                llvm::Value* epV    = builder_.CreateLoad(i64(), epochSlot, "arr.epv");
                emitGuardThrow(builder_.CreateICmpNE(curMod, epV, "arr.modchk"),
                               "InvalidOperationException");
                // After emitGuardThrow the insert point is the continuation block.
            }
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
            bool termR = lowerForBody(fs);
            loopStack_.pop_back();
            if (!termR) builder_.CreateBr(incBBR);
            builder_.SetInsertPoint(incBBR);
            llvm::Value* idxR3 = builder_.CreateLoad(i64(), idxSlotR);
            builder_.CreateStore(builder_.CreateAdd(idxR3, builder_.getInt64(1)), idxSlotR);
            builder_.CreateBr(condBBR);
            builder_.SetInsertPoint(endBBR);
            // #1055: a fresh `owned` Array materializer iterated directly as the
            // for-in source (`for var p in s.Split(',') do … end`) is owned by
            // nothing and leaks. Reclaim it here at the loop exit, which both
            // normal completion and `break` reach (a `return` inside the body
            // diverges before this point — the source leaks, the safe direction).
            // No-op unless the iterable is exactly a fresh ownerless array
            // materializer (its narrow guards) — a bound / `move`d / aliased
            // iterable is never matched, so no UAF / double-free; the iteration
            // variable is a borrow, so the elements are reclaimed exactly once.
            freeUnboundOwnedBorrowingArrayTemp(fs.iterable.get(), arrObj, iterTy);
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
        if (!v) {
            // #1386: ICE — lowerExpr returned null for a confirmed inline-array
            // for-in iterable; the expression kind is not yet handled by codegen.
            ctx_.diagnostics().report(
                Severity::Error, fs.iterable->span.begin,
                "internal: lowerExpr returned null for inline-array for-in "
                "iterable — #1386");
            return false;
        }
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
    bool term = lowerForBody(fs);
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
                                     const sema::TypeRef* elemTyRef,
                                     bool sorted) {
    if (!tableObj || !elemTyRef) return false;
    llvm::Type* elemTy = lowerTypeRef(elemTyRef);
    if (!elemTy) return false;

    // §29.12 / #1232: capture the dict/set's mod epoch before taking the snapshot.
    auto* getModDictFn = declareC("bz_dict_get_mod", i64(), {ptrTy()});
    llvm::Value* dictEpochVal = builder_.CreateCall(getModDictFn, {tableObj}, "dk.epoch");
    auto* dictEpochSlot = entryAlloca(i64());
    builder_.CreateStore(dictEpochVal, dictEpochSlot);

    auto* keysOutSlot = entryAlloca(ptrTy());
    auto* cntSlot     = entryAlloca(i64());
    auto* keysFn = declareC("bz_dict_keys_gen", llvm::Type::getVoidTy(llvm_),
                            {ptrTy(), ptrTy(), ptrTy()});
    builder_.CreateCall(keysFn, {tableObj, keysOutSlot, cntSlot});
    llvm::Value* keysArr = builder_.CreateLoad(ptrTy(), keysOutSlot);  // void**
    llvm::Value* cnt     = builder_.CreateLoad(i64(), cntSlot);

    // fix #1426 / stdlib §4.18: for SortedSet, sort the snapshot in ascending
    // key order before iterating. O(n log n) — hash-backing limitation; real
    // ordered rep rides #545.  buildSortedKeyComparator is null for unsupported
    // key types (record/decimal) — leave unsorted in that case (correctness note
    // in the commit message; follow-up needed for those types).
    if (sorted) {
        llvm::Function* cmpFn = buildSortedKeyComparator(elemTyRef);
        if (cmpFn) {
            auto* sortFn = declareC("bz_sort_keys_gen", llvm::Type::getVoidTy(llvm_),
                                    {ptrTy(), i64(), ptrTy()});
            builder_.CreateCall(sortFn, {keysArr, cnt, cmpFn});
        }
        // If cmpFn is null, fall through unsorted (unsupported elem type).
    }

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
    // §29.12 / #1232: check for concurrent modification on every MoveNext.
    {
        llvm::Value* curMod = builder_.CreateCall(getModDictFn, {tableObj}, "dk.curmod");
        llvm::Value* epV    = builder_.CreateLoad(i64(), dictEpochSlot, "dk.epv");
        emitGuardThrow(builder_.CreateICmpNE(curMod, epV, "dk.modchk"),
                       "InvalidOperationException");
    }
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
    bool term = lowerForBody(fs);
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
// OwningSet<T>. The set's element routes through the generic hash table (records,
// primitives/enums, AND strings — see lowerNew / genericBackingType), so the
// array-backed for-loop (bz_array_*) would read garbage. Enumerate the table's
// keys (= the set's elements) instead.
bool Emitter::lowerDictBackedSetFor(const ast::ForStmt& fs) {
    if (!fs.iterable) return false;
    auto* iterTy = ctx_.exprType(*fs.iterable);
    if (!iterTy || !iterTy->astNode || !iterTy->astNode->name ||
        iterTy->astNode->name->parts.empty() || iterTy->declaration)
        return false;
    auto n = iterTy->headName();
    if (n != "Set" && n != "OwningSet" &&
        // stdlib §4.18 (#1426): SortedSet<T> uses the same generic-key hash table.
        // Route through emitGenericKeyTableFor with sorted=true so elements are
        // visited in ascending order per §4.18's ordered-iteration contract.
        n != "SortedSet") return false;
    const sema::TypeRef* elemTy =
        genericBackingType(iterTy, 0, /*isSet=*/true, nullptr);
    if (!elemTy) return false;  // not dict-backed → let lowerArrayFor handle it
    llvm::Value* setObj = lowerExpr(*fs.iterable);
    if (!setObj) return false;
    bool sorted = (n == "SortedSet");
    return emitGenericKeyTableFor(fs, setObj, elemTy, sorted);
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

    // §29.12 / #1232: capture dict mod epoch before taking the string-key snapshot.
    auto* getModDictFnDk = declareC("bz_dict_get_mod", i64(), {ptrTy()});
    llvm::Value* dkEpochVal = builder_.CreateCall(getModDictFnDk, {dictObj}, "dks.epoch");
    auto* dkEpochSlot = entryAlloca(i64());
    builder_.CreateStore(dkEpochVal, dkEpochSlot);

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
    // #1058 / §28/§4.8: initialise to null so the endBB break-path release is
    // null-safe when the loop body is never entered (cnt == 0).
    builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), varSlot);

    auto* condBB = llvm::BasicBlock::Create(llvm_, "dk.cond", currentFn_);
    auto* bodyBB = llvm::BasicBlock::Create(llvm_, "dk.body", currentFn_);
    auto* incBB  = llvm::BasicBlock::Create(llvm_, "dk.inc",  currentFn_);
    auto* endBB  = llvm::BasicBlock::Create(llvm_, "dk.end",  currentFn_);
    builder_.CreateBr(condBB);

    builder_.SetInsertPoint(condBB);
    // §29.12 / #1232: check for concurrent modification on every MoveNext.
    {
        llvm::Value* curMod = builder_.CreateCall(getModDictFnDk, {dictObj}, "dks.curmod");
        llvm::Value* epV    = builder_.CreateLoad(i64(), dkEpochSlot, "dks.epv");
        emitGuardThrow(builder_.CreateICmpNE(curMod, epV, "dks.modchk"),
                       "InvalidOperationException");
    }
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
    bool term = lowerForBody(fs);
    loopStack_.pop_back();
    if (!term) builder_.CreateBr(incBB);

    builder_.SetInsertPoint(incBB);
    llvm::Value* idxI = builder_.CreateLoad(i64(), idxSlot);
    // #1058 / §28/§4.8: release the per-iteration string key object (rc 1→0 →
    // free dataCpy + strObj header).  Null varSlot so the endBB release below
    // is a no-op on normal completion (bz_string_release is null-safe).
    {
        llvm::Value* so = builder_.CreateLoad(ptrTy(), varSlot);
        emitStringRelease(so);
        builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), varSlot);
    }
    // Free this iteration's strndup'd key bytes from bz_dict_keys_str.
    llvm::Value* kpPtr2 = builder_.CreateGEP(ptrTy(), keysArr, idxI);
    llvm::Value* kp2    = builder_.CreateLoad(ptrTy(), kpPtr2);
    builder_.CreateCall(freeFn, {kp2});
    builder_.CreateStore(builder_.CreateAdd(idxI, builder_.getInt64(1)), idxSlot);
    builder_.CreateBr(condBB);

    builder_.SetInsertPoint(endBB);
    // #1058 / §28/§4.8: on `break` incBB is skipped — release the in-progress
    // iteration's strObj now.  On normal completion varSlot was nulled by incBB,
    // so bz_string_release(null) is a no-op (no double-free).
    {
        llvm::Value* so = builder_.CreateLoad(ptrTy(), varSlot);
        emitStringRelease(so);  // null-safe
    }
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

    // §29.12 / #1232: capture dict mod epoch before taking the values snapshot.
    auto* getModDictFnDv = declareC("bz_dict_get_mod", i64(), {ptrTy()});
    llvm::Value* dvEpochVal = builder_.CreateCall(getModDictFnDv, {dictObj}, "dv.epoch");
    auto* dvEpochSlot = entryAlloca(i64());
    builder_.CreateStore(dvEpochVal, dvEpochSlot);

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
    // §29.12 / #1232: check for concurrent modification on every MoveNext.
    {
        llvm::Value* curMod = builder_.CreateCall(getModDictFnDv, {dictObj}, "dv.curmod");
        llvm::Value* epV    = builder_.CreateLoad(i64(), dvEpochSlot, "dv.epv");
        emitGuardThrow(builder_.CreateICmpNE(curMod, epV, "dv.modchk"),
                       "InvalidOperationException");
    }
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
    bool term = lowerForBody(fs);
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


// #548 / stdlib §4.8 v0.135: `for var e in dict` — single-pass borrowing entry
// iteration over a non-owning Dictionary<K,V>.  The loop variable `e` has
// the synthetic element type DictionaryEntry<K,V> (set by sema above); this
// function detects that type on the loop var and lowers the iteration using
// parallel key+value snapshot arrays exactly as lowerDictKeysFor and
// lowerDictValuesFor do.
//
// Struct layout: per-iteration entry alloca is a 2-field struct {K key; V val}.
// Field 0 = Key (LLVM type for K), field 1 = Value (LLVM type for V).
// e.Key/e.Value reads are handled in lowerExpr via GEP on that struct
// (before the generic fieldAddr path, which knows nothing of DictionaryEntry).
//
// The value is a borrow (per §28.11): no retain/dispose on the value slot.
// The key string (string-key dict) is ref-counted: the copied string object
// produced by bz_dict_keys_str is retained and lives exactly for the loop body,
// then released in the increment block (mirrors lowerDictKeysFor's free pass).
//
// The snapshot arrays themselves (void** from bz_dict_values_str and char**/i64*
// from bz_dict_keys_str) are malloc'd by the runtime and freed at endBB.
bool Emitter::lowerDictEntryFor(const ast::ForStmt& fs) {
    if (!fs.iterable) return false;
    // Detect: iterable type is Dictionary<K,V>.
    // (lowerDictKeysFor/ValuesFor handle MemberAccessExpr targets; this handles
    //  the bare-dictionary-expression case.)
    auto* dictTy = ctx_.exprType(*fs.iterable);
    if (!dictTy || !isDictTypeName(dictTy)) return false;
    if (dictTy->genericArgs.size() < 2 ||
        !dictTy->genericArgs[0] || !dictTy->genericArgs[1]) return false;
    // Confirm element type is DictionaryEntry<K,V> (set by sema).
    // Without this guard a `.Keys`/`.Values` access would be a
    // MemberAccessExpr (caught earlier) or a Set/Array iteration; checking
    // that the iterable is NOT a MemberAccessExpr suffices, but the sema
    // element-type check is the definitive gate: if sema typed the loop var
    // as DictionaryEntry the for-stmt IS an entry iteration.
    // (If fs.iterable is a MemberAccessExpr, lowerDictKeysFor/ValuesFor
    //  already returned true above — we never reach here for those.)
    if (dynamic_cast<const ast::MemberAccessExpr*>(fs.iterable.get()))
        return false;

    const sema::TypeRef* keyTy = dictTy->genericArgs[0];
    const sema::TypeRef* valTy = dictTy->genericArgs[1];

    // Determine record / nullable-val flags for value (mirrors lowerDictValuesFor).
    const sema::TypeSymbol* valRec =
        (valTy->kind == sema::TypeRef::Kind::Named &&
         valTy->declaration &&
         valTy->declaration->kind == ast::TypeDeclKind::Record)
            ? valTy->declaration : nullptr;
    bool valNullableVal = isNullableValueTy(valTy);

    llvm::Type* keyLlvm = lowerTypeRef(keyTy);
    llvm::Type* valLlvm = lowerTypeRef(valTy);
    if (!keyLlvm || !valLlvm) return false;

    llvm::Value* dictObj = lowerExpr(*fs.iterable);
    if (!dictObj) return false;

    // §29.12 / #1232: capture dict mod epoch before taking the key+value snapshot.
    auto* getModDictFnDe = declareC("bz_dict_get_mod", i64(), {ptrTy()});
    llvm::Value* deEpochVal = builder_.CreateCall(getModDictFnDe, {dictObj}, "de.epoch");
    auto* deEpochSlot = entryAlloca(i64());
    builder_.CreateStore(deEpochVal, deEpochSlot);

    // Snapshot keys + values in parallel.
    auto* freeFn = declareC("free", llvm::Type::getVoidTy(llvm_), {ptrTy()});
    bool useGen = genericBackingType(dictTy, 0, /*isSet=*/false, nullptr) != nullptr;

    // --- key snapshot ---
    auto* keysOutSlot = entryAlloca(ptrTy()); // char** (string) or void** (gen)
    auto* lensOutSlot = entryAlloca(ptrTy()); // i64*   (string only; unused gen)
    auto* cntSlot     = entryAlloca(i64());

    // --- value snapshot ---
    auto* valsOutSlot = entryAlloca(ptrTy()); // void**

    using K = sema::TypeRef::Kind;
    bool isStringKey = keyTy->kind == K::Primitive &&
                       keyTy->primitive == sema::PrimitiveType::String;

    if (useGen) {
        // Generic-key dict: bz_dict_keys_gen(dict, void*** keys_out, int64_t* n)
        auto* kfn = declareC("bz_dict_keys_gen", llvm::Type::getVoidTy(llvm_),
                             {ptrTy(), ptrTy(), ptrTy()});
        builder_.CreateCall(kfn, {dictObj, keysOutSlot, cntSlot});
        auto* vfn = declareC("bz_dict_values_gen", llvm::Type::getVoidTy(llvm_),
                             {ptrTy(), ptrTy(), ptrTy()});
        builder_.CreateCall(vfn, {dictObj, valsOutSlot, entryAlloca(i64())});
    } else {
        // String-key dict: bz_dict_keys_str + bz_dict_values_str
        auto* kfn = declareC("bz_dict_keys_str", llvm::Type::getVoidTy(llvm_),
                             {ptrTy(), ptrTy(), ptrTy(), ptrTy()});
        builder_.CreateCall(kfn,
                            {dictObj, keysOutSlot, lensOutSlot, cntSlot});
        auto* vfn = declareC("bz_dict_values_str", llvm::Type::getVoidTy(llvm_),
                             {ptrTy(), ptrTy(), ptrTy()});
        builder_.CreateCall(vfn, {dictObj, valsOutSlot, entryAlloca(i64())});
    }
    llvm::Value* keysArr = builder_.CreateLoad(ptrTy(), keysOutSlot);
    llvm::Value* lensArr = isStringKey
                               ? builder_.CreateLoad(ptrTy(), lensOutSlot)
                               : nullptr;
    llvm::Value* valsArr = builder_.CreateLoad(ptrTy(), valsOutSlot);
    llvm::Value* cnt     = builder_.CreateLoad(i64(), cntSlot);

    // fix #1426 / stdlib §4.17: for SortedDictionary, sort keys and values
    // together in ascending key order before the loop.  O(n log n) per
    // enumeration (hash-backing limitation; ordered rep rides #545).
    if (dictTy && dictTy->astNode && dictTy->astNode->name &&
        !dictTy->astNode->name->parts.empty() &&
        dictTy->headName() == "SortedDictionary") {
        if (useGen) {
            const sema::TypeRef* kTy =
                genericBackingType(dictTy, 0, /*isSet=*/false, nullptr);
            llvm::Function* cmpFn = kTy ? buildSortedKeyComparator(kTy) : nullptr;
            if (cmpFn) {
                auto* sortKvFn = declareC("bz_sort_kv_gen",
                    llvm::Type::getVoidTy(llvm_),
                    {ptrTy(), ptrTy(), i64(), ptrTy()});
                builder_.CreateCall(sortKvFn, {keysArr, valsArr, cnt, cmpFn});
            }
        } else {
            // String-key: sort keys (char**), lens (i64*), and values together.
            auto* sortKvStrFn = declareC("bz_sort_kv_str",
                llvm::Type::getVoidTy(llvm_),
                {ptrTy(), ptrTy(), ptrTy(), i64()});
            llvm::Value* lensForSort = lensArr
                ? lensArr
                : llvm::ConstantPointerNull::get(ptrTy());
            builder_.CreateCall(sortKvStrFn,
                {keysArr, lensForSort, valsArr, cnt});
        }
    }

    auto* idxSlot = entryAlloca(i64());
    builder_.CreateStore(builder_.getInt64(0), idxSlot);

    // Loop variable alloca: 2-field struct {K key; V value}.
    // The struct layout matches what lowerExpr's DictionaryEntry intercept
    // expects (field 0 = Key, field 1 = Value).
    llvm::StructType* entryStTy =
        llvm::StructType::get(llvm_, {keyLlvm, valLlvm});
    auto* varSlot = entryAlloca(entryStTy);
    locals_[&fs] = varSlot;
    // #1058 / §28/§4.8: for string keys the key field (field 0) carries a
    // ref-counted bz_string_t* built per iteration.  Initialise to null so
    // the endBB break-path release (see below) is null-safe when the loop body
    // is never entered (cnt == 0).
    if (isStringKey)
        builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()),
                             builder_.CreateStructGEP(entryStTy, varSlot, 0));

    auto* condBB = llvm::BasicBlock::Create(llvm_, "de.cond", currentFn_);
    auto* bodyBB = llvm::BasicBlock::Create(llvm_, "de.body", currentFn_);
    auto* incBB  = llvm::BasicBlock::Create(llvm_, "de.inc",  currentFn_);
    auto* endBB  = llvm::BasicBlock::Create(llvm_, "de.end",  currentFn_);
    builder_.CreateBr(condBB);

    builder_.SetInsertPoint(condBB);
    {
        // §29.12 / #1232: check for concurrent modification on every MoveNext.
        {
            llvm::Value* curMod = builder_.CreateCall(getModDictFnDe, {dictObj}, "de.curmod");
            llvm::Value* epV    = builder_.CreateLoad(i64(), deEpochSlot, "de.epv");
            emitGuardThrow(builder_.CreateICmpNE(curMod, epV, "de.modchk"),
                           "InvalidOperationException");
        }
        llvm::Value* idx = builder_.CreateLoad(i64(), idxSlot);
        builder_.CreateCondBr(builder_.CreateICmpSLT(idx, cnt), bodyBB, endBB);
    }

    builder_.SetInsertPoint(bodyBB);
    {
        llvm::Value* idxB = builder_.CreateLoad(i64(), idxSlot);

        // --- decode key ---
        llvm::Value* keyDecoded;
        if (isStringKey) {
            // Same logic as lowerDictKeysFor: malloc+memcpy the key bytes
            // into a new string object and ref-count it.
            auto* mallocFn = declareC("malloc", ptrTy(), {i64()});
            auto* memcpyFn = declareC("memcpy", ptrTy(), {ptrTy(), ptrTy(), i64()});
            auto* callocFn = declareC("calloc", ptrTy(), {i64(), i64()});
            auto* i8Ty = llvm::Type::getInt8Ty(llvm_);
            uint64_t strSz =
                module_->getDataLayout().getTypeAllocSize(stringType());
            llvm::Value* kpPtr = builder_.CreateGEP(ptrTy(), keysArr, idxB);
            llvm::Value* kp    = builder_.CreateLoad(ptrTy(), kpPtr); // char*
            llvm::Value* lpPtr = builder_.CreateGEP(i64(), lensArr, idxB);
            llvm::Value* kl    = builder_.CreateLoad(i64(), lpPtr);
            llvm::Value* dataSz  = builder_.CreateAdd(kl, builder_.getInt64(1));
            llvm::Value* dataCpy = builder_.CreateCall(mallocFn, {dataSz});
            builder_.CreateCall(memcpyFn, {dataCpy, kp, kl});
            llvm::Value* nulPtr = builder_.CreateGEP(i8Ty, dataCpy, kl);
            builder_.CreateStore(llvm::ConstantInt::get(i8Ty, 0), nulPtr);
            llvm::Value* strObj = builder_.CreateCall(callocFn,
                {builder_.getInt64(1), builder_.getInt64((int64_t)strSz)});
            builder_.CreateStore(dataCpy,
                builder_.CreateStructGEP(stringType(), strObj, 0));
            builder_.CreateStore(kl,
                builder_.CreateStructGEP(stringType(), strObj, 1));
            builder_.CreateStore(llvm::ConstantInt::get(i8Ty, 1),
                builder_.CreateStructGEP(stringType(), strObj, 2));
            initStringRc(strObj);
            keyDecoded = strObj;
        } else if (useGen) {
            // Generic key: mirrors emitGenericKeyTableFor exactly.
            // bz_dict_keys_gen returns void** where each element is a malloc'd
            // copy of the key's raw bytes (NOT inttoptr-encoded — the raw bytes
            // are loaded directly as keyLlvm). The copy must be freed in incBB
            // (same free pass as emitGenericKeyTableFor performs).
            llvm::Value* kpPtr = builder_.CreateGEP(ptrTy(), keysArr, idxB);
            llvm::Value* keyPtr = builder_.CreateLoad(ptrTy(), kpPtr);
            keyDecoded = builder_.CreateLoad(keyLlvm, keyPtr, "de.kgen");
        } else {
            // Non-string non-generic key — should not happen; emit null.
            keyDecoded = llvm::Constant::getNullValue(keyLlvm);
        }

        // --- decode value (mirrors lowerDictValuesFor) ---
        llvm::Value* vpPtr = builder_.CreateGEP(ptrTy(), valsArr, idxB);
        llvm::Value* slotV = builder_.CreateLoad(ptrTy(), vpPtr);
        llvm::Value* valDecoded;
        if (valRec) {
            valDecoded = builder_.CreateLoad(valLlvm, slotV, "de.vrec");
        } else if (valNullableVal && valLlvm->isStructTy()) {
            valDecoded = builder_.CreateLoad(valLlvm, slotV, "de.vnv");
        } else if (!valLlvm->isPointerTy()) {
            llvm::Value* asI64 = builder_.CreatePtrToInt(slotV, i64());
            if (valLlvm->isDoubleTy())
                valDecoded = builder_.CreateBitCast(asI64, valLlvm);
            else if (valLlvm->isFloatTy())
                valDecoded = builder_.CreateBitCast(
                    builder_.CreateTrunc(asI64, i32()), valLlvm);
            else if (valLlvm->isIntegerTy(64))
                valDecoded = asI64;
            else
                valDecoded = builder_.CreateTrunc(asI64, valLlvm);
        } else {
            valDecoded = slotV;
        }

        // Store key at field 0, value at field 1 of the entry struct.
        builder_.CreateStore(keyDecoded,
            builder_.CreateStructGEP(entryStTy, varSlot, 0));
        builder_.CreateStore(valDecoded,
            builder_.CreateStructGEP(entryStTy, varSlot, 1));
    }

    loopStack_.push_back({incBB, endBB, disposeStack_.size()});
    bool term = lowerForBody(fs);
    loopStack_.pop_back();
    if (!term) builder_.CreateBr(incBB);

    builder_.SetInsertPoint(incBB);
    {
        llvm::Value* idxI = builder_.CreateLoad(i64(), idxSlot);
        if (isStringKey) {
            // #1058 / §28/§4.8: release the per-iteration string key object
            // (rc 1→0 → free dataCpy + strObj header).  Null field 0 so the
            // endBB release below is a no-op on normal completion.
            llvm::Value* so = builder_.CreateLoad(
                ptrTy(), builder_.CreateStructGEP(entryStTy, varSlot, 0));
            emitStringRelease(so);
            builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()),
                builder_.CreateStructGEP(entryStTy, varSlot, 0));
            // Free the strndup'd char* data for this iteration (mirrors
            // lowerDictKeysFor's per-slot free in the increment block).
            llvm::Value* kpPtr2 = builder_.CreateGEP(ptrTy(), keysArr, idxI);
            llvm::Value* kp2    = builder_.CreateLoad(ptrTy(), kpPtr2);
            builder_.CreateCall(freeFn, {kp2});
        } else if (useGen) {
            // Free the malloc'd raw-bytes key copy for this iteration
            // (mirrors emitGenericKeyTableFor's per-slot free).
            llvm::Value* kpPtr2 = builder_.CreateGEP(ptrTy(), keysArr, idxI);
            llvm::Value* kp2    = builder_.CreateLoad(ptrTy(), kpPtr2);
            builder_.CreateCall(freeFn, {kp2});
        }
        builder_.CreateStore(
            builder_.CreateAdd(idxI, builder_.getInt64(1)), idxSlot);
        builder_.CreateBr(condBB);
    }

    builder_.SetInsertPoint(endBB);
    // #1058 / §28/§4.8: on `break` incBB is skipped — release the in-progress
    // iteration's strObj now.  On normal completion field 0 was nulled by incBB,
    // so bz_string_release(null) is a no-op (no double-free).
    if (isStringKey) {
        llvm::Value* so = builder_.CreateLoad(
            ptrTy(), builder_.CreateStructGEP(entryStTy, varSlot, 0));
        emitStringRelease(so);  // null-safe
    }
    // #1057: the increment block frees each per-iteration key box (string /
    // generic keys are separately malloc'd), but a `break` out of the loop skips
    // the boxes from the break index onward — they leak. Free the remaining
    // boxes (idxSlot .. cnt) here so every box is reclaimed on EVERY exit. On
    // normal completion idxSlot == cnt, so this loop frees nothing (no double-
    // free of the boxes the increment block already freed).
    if (isStringKey || useGen) {
        auto* clCondBB = llvm::BasicBlock::Create(llvm_, "de.cl.cond", currentFn_);
        auto* clBodyBB = llvm::BasicBlock::Create(llvm_, "de.cl.body", currentFn_);
        auto* clEndBB  = llvm::BasicBlock::Create(llvm_, "de.cl.end",  currentFn_);
        builder_.CreateBr(clCondBB);
        builder_.SetInsertPoint(clCondBB);
        llvm::Value* ci = builder_.CreateLoad(i64(), idxSlot);
        builder_.CreateCondBr(builder_.CreateICmpSLT(ci, cnt), clBodyBB, clEndBB);
        builder_.SetInsertPoint(clBodyBB);
        llvm::Value* ckp = builder_.CreateLoad(
            ptrTy(), builder_.CreateGEP(ptrTy(), keysArr, ci));
        builder_.CreateCall(freeFn, {ckp});
        builder_.CreateStore(
            builder_.CreateAdd(ci, builder_.getInt64(1)), idxSlot);
        builder_.CreateBr(clCondBB);
        builder_.SetInsertPoint(clEndBB);
    }
    builder_.CreateCall(freeFn, {keysArr});
    if (isStringKey)
        builder_.CreateCall(freeFn, {lensArr});
    builder_.CreateCall(freeFn, {valsArr});
    // #1055: a fresh `owned Dictionary` materializer iterated directly as the
    // for-in source (`for var e in src.ToDictionary(k) do … end`) is owned by
    // nothing and leaks. Reclaim it here at the loop exit, which both normal
    // completion and `break` reach (a `return` in the body diverges before this
    // — leaks, the safe direction). No-op unless the iterable is exactly a fresh
    // ownerless dict materializer (its narrow guard) — a bound / `move`d /
    // aliased dict is never matched, so no UAF / double-free. The temp key/value
    // extraction arrays are freed above first; this frees the dict itself.
    freeUnboundOwnedBorrowingArrayTemp(fs.iterable.get(), dictObj,
                                       ctx_.exprType(*fs.iterable));
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
    lowerForBody(fs);
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


// #639 / stdlib §4.5/§4.7/§4.11 v0.135: `for var x in coll.Drain()` —
// ownership-transferring drain over OwningArray<T>, OwningLinkedList<T>,
// OwningStack<T>, OwningQueue<T>.
//
// Detection: the iterable expression has type IOwningEnumerable<T> (set by sema
// for the four collection names above) with genericArgs[0] NOT being a
// DrainedEntry (which would be the OwningDictionary case, handled below).
// Additionally, the iterable must be a Drain() call whose receiver has one of
// the four recognised type names.
//
// Drain order per §28.11 / stdlib §4.5/§4.7/§4.11:
//   OwningArray<T>       — front-to-back (bz_array_drain_next)
//   OwningQueue<T>       — FIFO = front-to-back (bz_array_drain_next)
//   OwningStack<T>       — LIFO = top-first (bz_array_drain_next_lifo)
//   OwningLinkedList<T>  — head-to-tail (bz_llist_drain_next)
//
// Runtime shape: a single call to the appropriate drain function per loop
// iteration.  The function pops one element pointer into *val_out and
// decrements the collection's count.  Returns false when the collection is
// empty.  The loop variable alloca holds the popped pointer; lowerExpr reads it
// as the owned T value.
//
// Partial drain: break exits the loop; remaining elements stay in the
// collection and are disposed by the collection's own Dispose() at scope-end.
// Exception mid-drain: same — unwinding invokes the collection's disposer via
// the standard using-var cleanup chain.
bool Emitter::lowerOwnedSeqDrainFor(const ast::ForStmt& fs) {
    if (!fs.iterable) return false;
    auto* iterTy = ctx_.exprType(*fs.iterable);
    if (!iterTy || iterTy->kind != sema::TypeRef::Kind::Named) return false;

    // Must be IOwningEnumerable<T>.
    std::string_view iterName;
    if (iterTy->astNode && iterTy->astNode->name &&
        !iterTy->astNode->name->parts.empty())
        iterName = iterTy->headName();
    else if (!iterTy->importedName.empty())
        iterName = iterTy->importedName;
    if (iterName != "IOwningEnumerable") return false;

    // Must have one generic arg T that is NOT DrainedEntry (that's the dict path).
    if (iterTy->genericArgs.empty() || !iterTy->genericArgs[0]) return false;
    const sema::TypeRef* elemTy = iterTy->genericArgs[0];
    if (elemTy->kind == sema::TypeRef::Kind::Named &&
        elemTy->importedName == "DrainedEntry")
        return false;  // let lowerDictDrainFor handle it

    // The iterable must be a Drain() call on one of the five collection types.
    // fix #1475 / stdlib §4.10: OwningSet<T> is included alongside OwningArray/
    // OwningLinkedList/OwningStack/OwningQueue. OwningSet<T> requires
    // `where T: class` (stdlib §4.10), so its element is never eligible for the
    // generic-key dict backing (genericBackingType returns null for every class
    // type, per #1474) — every valid OwningSet<T> is therefore ARRAY-backed,
    // constructed via the same bz_array_alloc allocation and manipulated by the
    // same bz_array_* entry points as OwningArray (see the Add/TryAdd/Remove/
    // Contains/Count/Clear handlers in LlvmBackend_Calls.cpp, which already
    // operate on an OwningSet receiver through bz_array_len/bz_array_ptr_at/
    // bz_array_add directly). Draining it is thus mechanically identical to
    // draining an OwningArray: bz_array_drain_next pops index 0 each iteration,
    // transferring ownership of the popped element to the loop body — order is
    // unspecified by §4.10 (a set has no ordering contract), so front-to-back
    // is a legitimate, simple choice.
    const ast::Expr* collExpr = nullptr;
    std::string_view collName;
    if (auto* ce = dynamic_cast<const ast::CallExpr*>(fs.iterable.get())) {
        if (auto* ma =
                dynamic_cast<const ast::MemberAccessExpr*>(ce->callee.get())) {
            if (ma->member == "Drain" && ma->target) {
                collExpr = ma->target.get();
                const sema::TypeRef* ct = ctx_.exprType(*collExpr);
                if (ct && ct->kind == sema::TypeRef::Kind::Named) {
                    if (ct->astNode && ct->astNode->name &&
                        !ct->astNode->name->parts.empty())
                        collName = ct->headName();
                    else if (!ct->importedName.empty())
                        collName = ct->importedName;
                }
            }
        }
    }
    if (!collExpr) return false;
    if (collName != "OwningArray" && collName != "OwningLinkedList" &&
        collName != "OwningStack" && collName != "OwningQueue" &&
        collName != "OwningSet")
        return false;

    // Evaluate the collection object.
    llvm::Value* collObj = lowerExpr(*collExpr);
    if (!collObj) return false;

    // The element type is always a class pointer (ptrTy()) for owned collections.
    // #990 / §4.5 / §14.2: in a GENERIC collection (`OwningArray<T>` with T an
    // unresolved class type parameter) `elemTy` is the bare param `T`.  When this
    // method is lowered as the generic TEMPLATE body (no active substitution),
    // lowerTypeRef(T) returns the i32 placeholder, so the element-slot store below
    // bitcast the popped `ptr` to i32 — malformed IR (IR_FAIL).  Resolve `T`
    // through the active per-instantiation substitution first; if it stays an
    // unresolved param, the owning collection's element is a class object by the
    // stdlib invariant (only classes are owned elements — `where T: class`), so
    // lower it as a class pointer (ptrTy()).  The store path below uses `rawPtr`
    // directly when elemLlvm == ptrTy(), so no bitcast is emitted.
    const sema::TypeRef* resolvedElemTy = resolveTypeParamRef(elemTy);
    llvm::Type* elemLlvm;
    if (resolvedElemTy && resolvedElemTy != elemTy)
        elemLlvm = lowerTypeRef(resolvedElemTy);
    else if (elemTy->kind == sema::TypeRef::Kind::Named &&
             elemTy->declaration == nullptr)
        // Unresolved bare type parameter for an owning collection: a class ptr.
        elemLlvm = ptrTy();
    else
        elemLlvm = lowerTypeRef(elemTy);
    if (!elemLlvm) return false;

    // Alloca for the loop variable (holds the popped owned pointer).
    auto* varSlot = entryAlloca(elemLlvm);
    // §30.7 / §29.2 (#1888): if the drained loop variable is live across an
    // `await` in the BODY (the canonical Pattern B drain-and-await:
    // `for var task in tasks.Drain() do results.Add(await task); end`), it is a
    // suspend-local — frame-back its slot exactly as range-for
    // (LlvmBackend_Stmts.cpp) and await-for (lowerAwaitFor) already do.
    // collectSuspendLocals (LlvmBackend_Iterators.cpp) reserves the frame field,
    // keyed on the ForStmt node. WITHOUT this the slot is a bare stack alloca
    // that is NOT spilled/restored across the body's await suspend, so on resume
    // (a fresh Resume invocation with a fresh stack) it holds a stale/garbage
    // task pointer — the body-end iter-var disposal then dereferences a dangling
    // (freed) task (`pthread_mutex` ESRCH / SEGV in bz$async$*$Resume). Framing
    // it makes the slot hold the current iteration's live task at body-end, so
    // the disposal reclaims it exactly once. // per §30.7, §29.2
    if (smStateStruct_ && smSelf_) {
        auto fit = smLocalFields_.find(&fs);
        if (fit != smLocalFields_.end())
            smAllocaToField_[varSlot] = fit->second;
    }
    locals_[&fs] = varSlot;

    // Alloca for the raw element output from the drain function.
    auto* valOutSlot = entryAlloca(ptrTy());

    // Choose the drain runtime function name based on the collection kind.
    const char* drainFnName;
    if (collName == "OwningLinkedList") {
        drainFnName = "bz_llist_drain_next";      // pops head → head-to-tail
    } else if (collName == "OwningStack") {
        drainFnName = "bz_array_drain_next_lifo"; // pops tail → LIFO
    } else {
        // OwningArray and OwningQueue: both drain front-to-back. OwningSet
        // (fix #1475) shares the same array backing and has no ordering
        // contract to honour, so it drains front-to-back too.
        drainFnName = "bz_array_drain_next";      // pops index 0 → FIFO/front-to-back
    }
    // bz_array_drain_next / bz_array_drain_next_lifo / bz_llist_drain_next
    //   (void* coll, void** val_out) → bool (i1)
    auto* drainFn = declareC(drainFnName, i1(), {ptrTy(), ptrTy()});

    auto* condBB = llvm::BasicBlock::Create(llvm_, "sd.cond", currentFn_);
    auto* bodyBB = llvm::BasicBlock::Create(llvm_, "sd.body", currentFn_);
    auto* incBB  = llvm::BasicBlock::Create(llvm_, "sd.inc",  currentFn_);
    auto* endBB  = llvm::BasicBlock::Create(llvm_, "sd.end",  currentFn_);
    builder_.CreateBr(condBB);

    // cond: call the drain function; it pops one element into valOutSlot.
    builder_.SetInsertPoint(condBB);
    llvm::Value* popped = builder_.CreateCall(drainFn,
        {collObj, valOutSlot}, "sd.popped");
    builder_.CreateCondBr(popped, bodyBB, endBB);

    // body: load the popped element pointer and store into the loop var slot.
    builder_.SetInsertPoint(bodyBB);
    {
        llvm::Value* rawPtr = builder_.CreateLoad(ptrTy(), valOutSlot);
        // The drain functions return a void* holding the class object pointer.
        // If elemLlvm is ptrTy() we use rawPtr directly; otherwise bitcast.
        llvm::Value* elemVal =
            (elemLlvm == ptrTy())
            ? rawPtr
            : builder_.CreateBitCast(rawPtr, elemLlvm, "sd.elem");
        builder_.CreateStore(elemVal, varSlot);
    }

    loopStack_.push_back({incBB, endBB, disposeStack_.size()});
    bool term = lowerForBody(fs);
    loopStack_.pop_back();
    if (!term) builder_.CreateBr(incBB);

    // inc: jump back to cond (the collection is already smaller after each pop).
    builder_.SetInsertPoint(incBB);
    builder_.CreateBr(condBB);

    builder_.SetInsertPoint(endBB);
    return true;
}


// #628 / stdlib §4.9 v0.135: `for var entry in ownedDict.Drain()` —
// ownership-transferring drain iteration over OwningDictionary<K,V>.
//
// Detection: the iterable expression has type IOwningEnumerable<DrainedEntry<K,V>>
// (set by sema for OwningDictionary<K,V>.Drain()). The first generic arg of
// IOwningEnumerable<T> is T = DrainedEntry<K,V> with importedName=="DrainedEntry".
//
// Loop variable layout: a 2-field struct alloca {K key; V* value} — mirroring
// the DictionaryEntry layout used by lowerDictEntryFor. Field 0 = Key, field 1 =
// Value. lowerExpr intercepts e.Key/e.Value GEPs on these (the DrainedEntry
// handler mirrors the DictionaryEntry handler in LlvmBackend_Exprs.cpp).
//
// Ownership: the loop var's Value field IS the owned class pointer, transferred
// out of the dict on each iteration. The key is a fresh copy (string: malloc'd
// bz_string_t; generic: raw bytes). Both are the loop body's responsibility.
// The for-loop owns the entry alloca; using var e := entry discharges the entry
// (and its Value) at the end of the iteration body.
//
// Partial drain: break exits the loop leaving remaining entries in the dict.
// The dict's Dispose() (scope-end) will dispose them via bz_dict_free.
//
// Per §4.9: OwningDictionary makes no insertion-order guarantee; iteration walks
// the order[] array in the natural (insertion-recorded) order as a convenience —
// the spec does not prohibit this, and the runtime note says the order depends
// on insertion-and-mutation history (not a strict contract). For correctness,
// what matters is: every live entry is yielded exactly once.
bool Emitter::lowerDictDrainFor(const ast::ForStmt& fs) {
    if (!fs.iterable) return false;
    auto* iterTy = ctx_.exprType(*fs.iterable);
    if (!iterTy || iterTy->kind != sema::TypeRef::Kind::Named) return false;
    // Must be IOwningEnumerable<DrainedEntry<K,V>>.
    // Check the type name is IOwningEnumerable.
    std::string_view iterName;
    if (iterTy->astNode && iterTy->astNode->name &&
        !iterTy->astNode->name->parts.empty())
        iterName = iterTy->headName();
    else if (!iterTy->importedName.empty())
        iterName = iterTy->importedName;
    if (iterName != "IOwningEnumerable") return false;
    // Must have one generic arg = DrainedEntry<K,V>.
    if (iterTy->genericArgs.empty() || !iterTy->genericArgs[0]) return false;
    const sema::TypeRef* entryTy = iterTy->genericArgs[0];
    if (entryTy->kind != sema::TypeRef::Kind::Named ||
        entryTy->importedName != "DrainedEntry" ||
        entryTy->genericArgs.size() < 2 ||
        !entryTy->genericArgs[0] || !entryTy->genericArgs[1])
        return false;

    const sema::TypeRef* keyTy = entryTy->genericArgs[0];
    const sema::TypeRef* valTy = entryTy->genericArgs[1];

    // The iterable must be a Drain() call on an OwningDictionary.
    // Recover the dictionary object pointer by re-evaluating the target of
    // the Drain() CallExpr.
    const ast::Expr* dictExpr = nullptr;
    if (auto* ce = dynamic_cast<const ast::CallExpr*>(fs.iterable.get())) {
        if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(ce->callee.get())) {
            if (ma->member == "Drain" && ma->target)
                dictExpr = ma->target.get();
        }
    }
    if (!dictExpr) return false;

    // Confirm the target has type OwningDictionary<K,V>.
    const sema::TypeRef* dictTy = ctx_.exprType(*dictExpr);
    if (!isDictTypeName(dictTy)) return false;

    // Evaluate the dictionary object.
    llvm::Value* dictObj = lowerExpr(*dictExpr);
    if (!dictObj) return false;

    llvm::Type* keyLlvm = lowerTypeRef(keyTy);
    llvm::Type* valLlvm = lowerTypeRef(valTy);  // V* (pointer to owned class)
    if (!keyLlvm || !valLlvm) return false;

    // Determine key mode: string (key_size == 0) vs generic.
    bool useGen = genericBackingType(dictTy, 0, /*isSet=*/false, nullptr) != nullptr;

    // Alloca for the drain cursor (i64, starts at 0).
    auto* cursorSlot = entryAlloca(i64());
    builder_.CreateStore(builder_.getInt64(0), cursorSlot);

    // Loop variable alloca: {K key; V* value} struct (same layout as
    // DictionaryEntry<K,V> in lowerDictEntryFor).
    llvm::StructType* entryStTy = llvm::StructType::get(llvm_, {keyLlvm, valLlvm});
    auto* varSlot = entryAlloca(entryStTy);
    locals_[&fs] = varSlot;

    // Alloca for temp key/val outputs from the runtime pop function.
    auto* keyOutSlot = entryAlloca(ptrTy());   // char** (str) or void** (gen)
    auto* keyLenSlot = entryAlloca(i64());     // int64_t (str key_len only)
    auto* valOutSlot = entryAlloca(ptrTy());   // void** for value

    auto* condBB = llvm::BasicBlock::Create(llvm_, "dd.cond", currentFn_);
    auto* bodyBB = llvm::BasicBlock::Create(llvm_, "dd.body", currentFn_);
    auto* incBB  = llvm::BasicBlock::Create(llvm_, "dd.inc",  currentFn_);
    auto* endBB  = llvm::BasicBlock::Create(llvm_, "dd.end",  currentFn_);
    builder_.CreateBr(condBB);

    // cond: call bz_dict_drain_next_str / bz_dict_drain_next_gen.
    // Returns bool (i1): true = entry popped, false = drain complete.
    builder_.SetInsertPoint(condBB);
    llvm::Value* cursor = builder_.CreateLoad(i64(), cursorSlot);
    // We pass &cursor as a pointer (already in cursorSlot).
    llvm::Value* popped;
    if (!useGen) {
        // bz_dict_drain_next_str(dict, int64_t* cursor,
        //                        char** key_out, int64_t* key_len_out,
        //                        void** val_out) → bool
        auto* drainFn = declareC("bz_dict_drain_next_str", i1(),
                                 {ptrTy(), ptrTy(), ptrTy(), ptrTy(), ptrTy()});
        popped = builder_.CreateCall(drainFn,
            {dictObj, cursorSlot, keyOutSlot, keyLenSlot, valOutSlot},
            "dd.popped");
    } else {
        // bz_dict_drain_next_gen(dict, int64_t* cursor,
        //                        void** key_out, void** val_out) → bool
        auto* drainFn = declareC("bz_dict_drain_next_gen", i1(),
                                 {ptrTy(), ptrTy(), ptrTy(), ptrTy()});
        popped = builder_.CreateCall(drainFn,
            {dictObj, cursorSlot, keyOutSlot, valOutSlot},
            "dd.popped");
    }
    builder_.CreateCondBr(popped, bodyBB, endBB);

    // body: decode key+val into the entry struct slot.
    builder_.SetInsertPoint(bodyBB);
    {
        llvm::Value* rawKeyPtr = builder_.CreateLoad(ptrTy(), keyOutSlot);
        llvm::Value* rawValPtr = builder_.CreateLoad(ptrTy(), valOutSlot);

        // Decode key.
        llvm::Value* keyDecoded;
        using K = sema::TypeRef::Kind;
        bool isStringKey = keyTy->kind == K::Primitive &&
                           keyTy->primitive == sema::PrimitiveType::String;
        if (isStringKey) {
            // rawKeyPtr is a malloc'd char* (the key bytes; null-terminated).
            // Build a bz_string_t object from it — same pattern as
            // lowerDictEntryFor's string-key decode.
            auto* mallocFn  = declareC("malloc", ptrTy(), {i64()});
            auto* callocFn  = declareC("calloc", ptrTy(), {i64(), i64()});
            auto* i8Ty      = llvm::Type::getInt8Ty(llvm_);
            uint64_t strSz  = module_->getDataLayout().getTypeAllocSize(stringType());
            // rawKeyPtr is char*; key_len_out holds the byte length.
            llvm::Value* kl = builder_.CreateLoad(i64(), keyLenSlot);
            // Allocate and null-terminate the data buffer.
            llvm::Value* dataSz  = builder_.CreateAdd(kl, builder_.getInt64(1));
            // The char* from bz_dict_drain_next_str is already a malloc'd copy;
            // use it directly as the string data buffer (no extra malloc+memcpy).
            // Null-terminate: write '\0' at offset kl.
            llvm::Value* nulPtr = builder_.CreateGEP(i8Ty, rawKeyPtr, kl);
            builder_.CreateStore(llvm::ConstantInt::get(i8Ty, 0), nulPtr);
            // Allocate the bz_string_t header.
            llvm::Value* strObj = builder_.CreateCall(callocFn,
                {builder_.getInt64(1), builder_.getInt64((int64_t)strSz)});
            builder_.CreateStore(rawKeyPtr,
                builder_.CreateStructGEP(stringType(), strObj, 0));
            builder_.CreateStore(kl,
                builder_.CreateStructGEP(stringType(), strObj, 1));
            builder_.CreateStore(llvm::ConstantInt::get(i8Ty, 1),
                builder_.CreateStructGEP(stringType(), strObj, 2));
            initStringRc(strObj);
            keyDecoded = strObj;
            (void)dataSz;
        } else if (useGen) {
            // Generic key: rawKeyPtr is a malloc'd copy of the key bytes.
            // Load the key value out of the raw bytes.
            keyDecoded = builder_.CreateLoad(keyLlvm, rawKeyPtr, "dd.kgen");
            // Free the raw bytes copy immediately (the value is loaded).
            auto* freeFn = declareC("free", llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(freeFn, {rawKeyPtr});
        } else {
            // Non-string, non-generic key — should not happen.
            keyDecoded = llvm::Constant::getNullValue(keyLlvm);
        }

        // Value: rawValPtr IS the owned class pointer (void* stored directly).
        // V is always a class (OwningDictionary<K,V> requires V: class), so
        // valLlvm == ptrTy(). No further decoding needed.
        llvm::Value* valDecoded = rawValPtr;

        // Store key+val into the loop variable struct.
        builder_.CreateStore(keyDecoded,
            builder_.CreateStructGEP(entryStTy, varSlot, 0));
        builder_.CreateStore(valDecoded,
            builder_.CreateStructGEP(entryStTy, varSlot, 1));
    }

    loopStack_.push_back({incBB, endBB, disposeStack_.size()});
    bool term = lowerForBody(fs);
    loopStack_.pop_back();
    if (!term) builder_.CreateBr(incBB);

    // inc: just jump back to cond (cursor is already advanced inside the
    // drain runtime function).
    builder_.SetInsertPoint(incBB);
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
        n = iterTy->headName();
    } else if (!iterTy->importedName.empty()) {
        n = iterTy->importedName;
    } else {
        return false;
    }
    // Only handle the iteration-interface family.
    if (n != "IEnumerable" && n != "IReadOnlyCollection" && n != "IReadOnlyList" &&
        n != "ICollection" && n != "IList" &&
        n != "IOwningEnumerable")
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
    // Invariant (fix #1177 / §4.5 / §12.1): lowerLocalDecl wraps ANY
    // bz_array_t-backed collection (Array, OwningArray, OwningBuffer,
    // Stack/OwningStack, Queue/OwningQueue) and (Owned)LinkedList with
    // a vtable-backed IEnumerable object whenever the local is declared as
    // IEnumerable<T> (see LlvmBackend_Setup.cpp).  So when this path reaches
    // the vtable-dispatch branch below for an IEnumerable<T> local, the slot
    // already holds a proper vtable-backed wrapper and slot 4 (GetEnumerator)
    // is always valid — no longer limited to yield-iterator results.
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
        // interface uses vtable dispatch. buildOrderedArgs wraps any array-
        // backed collection argument (Array, OwningArray, OwningBuffer,
        // Stack/OwningStack, Queue/OwningQueue, LinkedList/OwningLinkedList)
        // at the call site, and lowerLocalDecl (#1177) wraps them at the
        // local-variable initializer site, so every IEnumerable<T>-typed
        // identifier always holds a proper vtable-backed wrapper object.
        // Richer interfaces (IReadOnlyList etc.) also support indexer access
        // and are NOT wrapped; they keep the array-backed protocol.
        auto checkPureIEnumerable = [&](sema::TypeRef* rt) -> bool {
            if (!rt || rt->kind != sema::TypeRef::Kind::Named) return false;
            if (rt->declaration) return false;  // user class → handled by checkNamed
            if (!rt->astNode || !rt->astNode->name ||
                rt->astNode->name->parts.empty())
                return false;
            return rt->headName() == "IEnumerable";
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
        } else if (dynamic_cast<sema::FieldSymbol*>(sym) ||
                   dynamic_cast<sema::PropertySymbol*>(sym)) {
            // §29.2 / §28.5 (#1642): a bare field/property source (`for var x in
            // seq` where `seq` is an owned field or a property) holds a vtable-
            // backed iterator object for the pure iterator interfaces
            // IEnumerable<T> / IOwningEnumerable<T> — the same coercion invariant
            // (#1310) that makes a local/param vtable-backed applies to storage
            // members. Without this the source defaults to the bz_array protocol
            // and reads the iterator frame as an array (a 0-element silent
            // miscompile). Richer interfaces (IReadOnlyList, etc.) keep the
            // indexer-based array protocol, exactly as for a local/param.
            if (n == "IEnumerable" || n == "IOwningEnumerable")
                isArrayBacked = false;
        }
    } else if (dynamic_cast<const ast::MemberAccessExpr*>(fs.iterable.get())) {
        // §29.2 / §28.5 (#1642): an explicit `this.seq` / `obj.seq` field or
        // property read — the same object-owned, vtable-backed iterator source as
        // the bare-identifier field/property case above (exprSymbol resolves the
        // member access to its FieldSymbol / PropertySymbol).
        sema::Symbol* sym = ctx_.exprSymbol(*fs.iterable);
        if ((dynamic_cast<sema::FieldSymbol*>(sym) ||
             dynamic_cast<sema::PropertySymbol*>(sym)) &&
            (n == "IEnumerable" || n == "IOwningEnumerable"))
            isArrayBacked = false;
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
        registerForLoopSpillSlots(fs, {idxSlot, varSlot});  // #747 R42
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
        lowerForBody(fs);
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

    // §29.11 (#943): obtain a FRESH, INDEPENDENT enumerator from the source by
    // calling GetEnumerator() (vtable slot 4) — the source value (e.g. the
    // `IEnumerable<int>` produced by a yield method) is the IEnumerABLE, not the
    // enumerator. The loop advances and disposes ITS OWN enumerator; the source is
    // left untouched so a second `for` loop over the same value replays from the
    // start instead of dereferencing the exhausted-and-freed first enumerator (the
    // use-after-free SIGSEGV this fix targets). Every sync IEnumerable vtable
    // producer (yield iterator, array wrapper, user-class wrapper) now carries a
    // GetEnumerator at slot 4 that returns a distinct fresh enumerator, so this
    // slot-4 read is always in-bounds and always yields an independent enumerator.
    //
    // Source ownership: the source value (the IEnumerABLE) is disposed exactly
    // once at its own lifetime end — at scope end for a named local/param
    // (registered in lowerLocalDecl, §29.11), or here as a one-shot temporary when
    // the for-in iterates a freshly produced value (a CallExpr/NewExpr result with
    // no owning binding). The per-loop enumerator is always disposed below.
    auto* srcSlot = entryAlloca(ptrTy());
    builder_.CreateStore(enumObj, srcSlot);
    {
        llvm::Value* srcVt = builder_.CreateLoad(ptrTy(), enumObj);
        llvm::Value* geSlotPtr =
            builder_.CreateGEP(ptrTy(), srcVt, {builder_.getInt32(4)});
        llvm::Value* geFnPtr = builder_.CreateLoad(ptrTy(), geSlotPtr);
        auto* geTy = llvm::FunctionType::get(ptrTy(), {ptrTy()}, false);
        enumObj = builder_.CreateCall(geTy, geFnPtr, {enumObj}, "ib.getenum");
    }

    // Store the (fresh) enumerator in a slot.
    auto* enumSlot = entryAlloca(ptrTy());
    builder_.CreateStore(enumObj, enumSlot);

    // §29.7 (#944): when this for-in is inside a yielding iterator, the
    // enumerator is an owned resource scoped to the loop body — like a `using
    // var`. If the enclosing iterator is abandoned (Dispose) while suspended at a
    // yield INSIDE this loop, the enumerator is still live and the state-machine
    // Dispose must dispose it (§29.7: "Dispose on every using var active at the
    // point of suspension"). To make it reachable from the Dispose function —
    // which does not re-lower the loop body — it round-trips through a DEDICATED
    // frame field (reserved by collectSuspendLocals, keyed on the iterable node),
    // NOT only the alloca-keyed spill area Dispose cannot reach. Wire the
    // enumerator alloca to that field (smAllocaToField_ → the per-yield spill
    // saves/restores it) and record slot→field in smIterEnumFrameSlot_ so the
    // yield snapshot logs this frame in smIterEnumFramesAtState_ for every suspend
    // state inside the loop. The for-in's OWN loop-exit disposal (emitDisposals at
    // endBB) and break/return paths dispose it on a normal exit; the state-machine
    // Dispose disposes it ONLY when suspended inside the loop — exactly once on
    // every exit (discriminated by the suspend state).
    bool enumHasFrameField = false;
    if (smStateStruct_ && smSelf_) {
        auto fit = smLocalFields_.find(fs.iterable.get());
        if (fit != smLocalFields_.end()) {
            smAllocaToField_[enumSlot] = fit->second;
            smIterEnumFrameSlot_[enumSlot] = fit->second;
            enumHasFrameField = true;
        }
    }

    // §29.7 (#623): register the enumerator for disposal on all exit paths
    // (break / return / exception / normal exhaustion).  Use a dedicated scope
    // around the loop so emitLoopDisposals(outerDepth) on `break` disposes it.
    // The iterator vtable has Dispose at slot 1 (not slot 2 like class vtables),
    // so we use iterEnumDispose rather than dynamicDispose.
    std::size_t outerDepth = disposeStack_.size();
    disposeStack_.emplace_back();
    {
        Disposal d{enumSlot, nullptr};
        d.iterEnumDispose = true;
        disposeStack_.back().push_back(d);
    }
    // §29.11 (#943): a freshly produced source value with no owning binding (a
    // CallExpr/NewExpr temporary, e.g. `for var x in P.Seq() do`) is owned by this
    // loop — dispose it once when the loop ends, alongside the enumerator. A named
    // local/param source (an IdentifierExpr) is disposed at its own scope end, so
    // it is NOT disposed here (that would double-free).
    // §29.2 / §28.5 (#1642): a for-in source that is a NAMED owned binding — a
    // local/param, or a field/property owned by the enclosing object — is
    // disposed at ITS OWN lifetime end (the local's scope end, or the object's
    // owned-field cascade, §28.5), NOT here. Per §29.2 the loop BORROWS such a
    // source (pins it, `using var __source` becomes a no-op borrow) and does not
    // assume its disposal obligation; only a freshly produced temporary (a
    // CallExpr/NewExpr result with no owning binding) is loop-owned and disposed
    // here. exprSymbol resolves both a bare identifier (`seq`) and a member
    // access (`this.seq` / `obj.seq`) to the underlying binding symbol, so a
    // field/property source (whether spelled bare or `this.`-qualified) is
    // correctly recognized and NOT double-freed against the field cascade.
    bool sourceIsNamedLocal = false;
    if (sema::Symbol* sym = ctx_.exprSymbol(*fs.iterable)) {
        sourceIsNamedLocal = dynamic_cast<sema::LocalSymbol*>(sym) != nullptr ||
                             dynamic_cast<sema::ParameterSymbol*>(sym) != nullptr ||
                             dynamic_cast<sema::FieldSymbol*>(sym) != nullptr ||
                             dynamic_cast<sema::PropertySymbol*>(sym) != nullptr;
    }
    if (!sourceIsNamedLocal) {
        Disposal ds{srcSlot, nullptr};
        ds.iterEnumDispose = true;
        disposeStack_.back().push_back(ds);
        // §29.7 (#944): the loop-owned source IEnumERABLE is a second implicit
        // frame live across a suspend inside the loop (independent of the
        // enumerator clone, §29.11). Wire it to its dedicated frame field
        // (forInSrcFields, reserved by collectSuspendLocals) so the enclosing
        // iterator's Dispose disposes it too when abandoned mid-loop — else the
        // source frame leaks even though its using-vars were freed via the
        // enumerator's Dispose. A named-local source is NOT wired here: it is
        // disposed at its own scope end via the using/local machinery.
        if (enumHasFrameField) {
            auto sit = smForInSrcFields_.find(fs.iterable.get());
            if (sit != smForInSrcFields_.end()) {
                smAllocaToField_[srcSlot] = sit->second;
                smIterEnumFrameSlot_[srcSlot] = sit->second;
            }
        }
    }

    // Use vtable-based MoveNext (slot 2) and Current (slot 3).
    // Layout: [0]=td, [1]=Dispose, [2]=MoveNext, [3]=Current
    auto* moveNextTy = llvm::FunctionType::get(i1(), {ptrTy()}, false);
    auto* currentTy = llvm::FunctionType::get(elemTy, {ptrTy()}, false);

    auto* varSlot = entryAlloca(elemTy);
    locals_[&fs] = varSlot;
    // §29.7 (#944): when the enumerator round-trips through its OWN dedicated
    // frame field (enumHasFrameField — wired into smAllocaToField_ above so the
    // per-yield spill saves/restores it and Dispose can reach it), it must NOT
    // also be registered in the alloca-keyed spill area — that would create two
    // independent round-trip mechanisms for the same slot (the spill-area copy
    // would shadow the frame-field copy the Dispose path reads). The loop var is
    // unaffected and keeps the spill-area round-trip.
    if (enumHasFrameField)
        registerForLoopSpillSlots(fs, {varSlot});            // #747 R42
    else
        registerForLoopSpillSlots(fs, {enumSlot, varSlot});  // #747 R42

    // §28.3 / §29.2 (#895): for an `IOwningEnumerable<T>` the loop variable OWNS
    // each yielded element — sema requires it to be discharged on a normal body
    // exit (a `for var x in Gen() do end` with no discharge is a compile error).
    // On the EXCEPTION path, though, the current element must be auto-disposed
    // implicitly (§28.3). Register a null-guarded unwindOnly disposal for the
    // loop variable's slot so a throw out of the body disposes the current
    // element exactly once. The enumerator's own Dispose (registered above) frees
    // the iterator's remaining state, not the already-yielded current element, so
    // there is no double-free. Null-init the slot so a throw in the first
    // MoveNext (before any element is read) finds null and skips; an explicit
    // discharge in the body nulls the slot → skip; on normal exhaustion the
    // unwindOnly disposal skips (every element was discharged in the body).
    if (n == "IOwningEnumerable" && elemTy->isPointerTy() &&
        !disposeStack_.empty()) {
        builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), varSlot);
        Disposal dv{varSlot, nullptr};
        dv.unwindOnly = true;
        disposeStack_.back().push_back(dv);
    }

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
    // §29.2/§31 (#630): MoveNext runs the iterator body; an exception thrown
    // there sets bz$exc and returns false (propagateBB in MoveNext).  Without
    // this pending-check the consumer never sees the exception and the for-loop
    // silently ends.  emitPendingCheck is the standard CG20 post-call gate:
    // if bz$exc is non-null, unwind (disposing the enumerator via the scope
    // on the disposeStack_) and propagate to the enclosing handler.
    emitPendingCheck();
    builder_.CreateCondBr(moved, bodyBB, endBB);

    // body: call vtable slot 3 (Current for IEnumerable / TakeCurrent for
    // IOwningEnumerable — §29.1 v0.161 #1349). emitPendingCheck() after the
    // call handles the IOE that TakeCurrent raises on protocol misuse (double-
    // take etc.); for IEnumerable Current the check is a harmless no-op.
    builder_.SetInsertPoint(bodyBB);
    llvm::Value* obj2 = builder_.CreateLoad(ptrTy(), enumSlot);
    llvm::Value* vtable2 = builder_.CreateLoad(ptrTy(), obj2);
    llvm::Value* curSlotPtr = builder_.CreateGEP(ptrTy(), vtable2, {builder_.getInt32(3)});
    llvm::Value* curFnPtr = builder_.CreateLoad(ptrTy(), curSlotPtr);
    llvm::Value* curVal = builder_.CreateCall(currentTy, curFnPtr, {obj2}, "ib.cur");
    emitPendingCheck();  // TakeCurrent may raise IOE (#1349)
    builder_.CreateStore(curVal, varSlot);

    // §29.7 (#623): loopScopeDepth = outerDepth (BEFORE enumerator scope push)
    // so that emitLoopDisposals on `break` disposes both the body scope AND
    // the enumerator scope (calls Dispose on the iterator frame).
    loopStack_.push_back({condBB, endBB, outerDepth});
    lowerForBody(fs);
    loopStack_.pop_back();
    if (!builder_.GetInsertBlock()->getTerminator())
        builder_.CreateBr(condBB);

    // Normal exhaustion: endBB. Dispose the enumerator (Dispose runs the
    // pending finally blocks and frees the frame), then pop the scope.
    builder_.SetInsertPoint(endBB);
    emitDisposals(disposeStack_.back());
    disposeStack_.pop_back();
    return true;
}


bool Emitter::lowerIEnumerableFor(const ast::ForStmt& fs) {
    if (!fs.iterable) return false;
    auto* iterTy = ctx_.exprType(*fs.iterable);
    if (!iterTy || iterTy->kind != sema::TypeRef::Kind::Named) return false;
    auto* ts = iterTy->declaration;
    if (!ts) return false;  // builtin/interface without concrete class — skip

    // #1776 / §21.4 / §29.2: the source's STATIC type is a user INTERFACE that
    // (transitively) extends IEnumerable<T> — e.g. `interface IMyColl:
    // IEnumerable<int>`. Such an interface declares no GetEnumerator of its own,
    // so findMethodByName(ts, "GetEnumerator") below returns null and this
    // handler bailed, dropping the statement through to the codegen NYI (the
    // #1776 gap; sema §21.4 correctly admits the interface as iterable — #1775).
    // GetEnumerator is inherited from the *builtin* IEnumerable, which carries
    // no itable/vtable dispatch slot (unlike IComparable/IEquatable it is not a
    // synthesized itable-participating interface — SemaContext::
    // builtinInterfaceSym), so the object exposes no dynamic handle to it.
    //
    // Devirtualize when every class implementing this interface resolves
    // GetEnumerator to the SAME method (the common case: a single implementing
    // class, or a base class whose subclasses inherit its non-virtual
    // GetEnumerator). The runtime object is then necessarily an instance of
    // that class (or a subclass sharing the identical GetEnumerator), so a
    // direct static call on the interface-typed receiver is sound: rewrite `ts`
    // to the implementing class and fall into the ordinary concrete-class path
    // below (emitMethodCall issues the direct call; the receiver pointer IS-A
    // the class at runtime). If the implementers resolve GetEnumerator to two
    // or more DISTINCT methods, no single static call is sound — leave `ts` as
    // the interface so findMethodByName fails and the statement falls through to
    // the NYI (a compile error, never a miscompile); that truly-polymorphic
    // case needs itable dispatch over a builtin IEnumerable descriptor (#1776).
    if (ts->kind == ast::TypeDeclKind::Interface &&
        !findMethodByName(ts, "GetEnumerator")) {
        sema::MethodSymbol* uniqueGetEnum = nullptr;
        bool ambiguous = false;
        for (const auto& sym : ctx_.symbols()) {
            auto* cand = dynamic_cast<const sema::TypeSymbol*>(sym.get());
            if (!cand || cand->kind != ast::TypeDeclKind::Class) continue;
            std::vector<const sema::TypeSymbol*> supers;
            collectTypeAndBases(cand, supers);
            bool implementsIface = false;
            for (auto* s : supers) if (s == ts) { implementsIface = true; break; }
            if (!implementsIface) continue;
            sema::MethodSymbol* ge = findMethodByName(cand, "GetEnumerator");
            if (!ge) continue;  // abstract implementer with no compiled body
            if (!uniqueGetEnum) uniqueGetEnum = ge;
            else if (uniqueGetEnum != ge) { ambiguous = true; break; }
        }
        if (ambiguous || !uniqueGetEnum) return false;  // → NYI (sound)
        ts = uniqueGetEnum->parent;  // devirtualize to the implementing class
    }

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

    // §29.2 / §28.3 (#972): push a dedicated disposal scope for the enumerator
    // so that break / early return / normal exhaustion / exception ALL call
    // Dispose() exactly once.  Mirror lowerIEnumerableBuiltinFor: record the
    // outer depth (used by loopStack_ so `break` disposes the enumerator scope
    // too), push a fresh scope, register the enumerator in it.  At normal-exit
    // (endBB) we call emitDisposals + pop_back, just like the builtin sibling.
    // Previously we added to disposeStack_.back() (the enclosing scope) which
    // caused break/exception to double-dispose (the enclosing scope already ran
    // it) AND normal exhaustion to never dispose (endBB had no emitDisposals
    // call) — see issue #972.
    std::size_t outerDepth = disposeStack_.size();
    disposeStack_.emplace_back();
    if (disposerFns_.count(enumTs))
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
    registerForLoopSpillSlots(fs, {enumSlot, varSlot});  // #747 R42

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

    // §29.7 (#972): loopScopeDepth = outerDepth (BEFORE enumerator scope push)
    // so that emitLoopDisposals on `break` disposes both the body scope AND
    // the enumerator scope (calls Dispose on the user class enumerator).
    loopStack_.push_back({condBB, endBB, outerDepth});
    bool term = lowerForBody(fs);
    loopStack_.pop_back();
    if (!term) builder_.CreateBr(condBB);

    // §29.2 / §28.3 (#972): normal exhaustion — dispose the enumerator and pop
    // the dedicated scope, mirroring lowerIEnumerableBuiltinFor's endBB handling.
    builder_.SetInsertPoint(endBB);
    emitDisposals(disposeStack_.back());
    disposeStack_.pop_back();
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
    // §30.7/§30.8 (#765): an optional `with token` clause is now lowered. For a
    // compiler-generated async iterator the source value IS the enumerator (the
    // enumPtr below is the source directly — there is no separate
    // GetAsyncEnumerator(token) call), so the token is evaluated for its
    // side-effects/ownership and then discarded; the rest of the loop is
    // identical to the no-`with` form. Honoring the token for cancellation
    // (§30.8) remains deferred, exactly as on a direct GetAsyncEnumerator path.
    // The token expression is lowered after the source is evaluated, below.
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

    // Evaluate the source to the enumerator pointer and store it in a slot.
    // #792 / §29.7/§30.7: the slot is frame-backed (asyncHandlerSpillSlots_) so it
    // survives the loop's own per-iteration await suspends AND is reachable on the
    // unwind path — mirroring the synchronous iterator-for (lowerIEnumerableBuiltinFor).
    // This replaces the prior SSA-promoted enumerator: holding it in a frame slot
    // lets the disposal scope below dispose the frame on EVERY exit edge.
    llvm::Value* enumPtr = lowerExpr(*fs.iterable);
    if (!enumPtr) return false;
    auto* enumSlot = entryAlloca(ptrTy());
    builder_.CreateStore(enumPtr, enumSlot);
    asyncHandlerSpillSlots_.push_back(enumSlot);  // survive the condBB await suspend
    // §30.8 (#765/#790): evaluate the optional `with` cancellation token. Its
    // value is a pointer to the source's cancellation-state cell (null for
    // CancellationToken.None). It is observed at each iteration's suspend point
    // (condBB below): a cancelled token raises OperationCanceledException.
    llvm::Value* tokenVal = fs.withExpr ? lowerExpr(*fs.withExpr) : nullptr;

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

    // #792 / §29.7 (#623): register the enumerator frame for disposal on ALL exit
    // paths (break / return / exception-unwind / normal exhaustion) via a dedicated
    // scope around the loop — the iterator frame's Dispose is at vtable slot 1, so
    // use iterEnumDispose. Previously the frame was disposed only at endBB (normal
    // completion), leaking it when the body threw / returned / broke out.
    std::size_t outerDepth = disposeStack_.size();
    disposeStack_.emplace_back();
    {
        Disposal d{enumSlot, nullptr};
        d.iterEnumDispose = true;
        disposeStack_.back().push_back(d);
    }

    // Vtable-dispatched MoveNextAsync (slot 2) / Current (slot 3); Dispose
    // (slot 1) is handled by the disposal scope above (iterEnumDispose).
    auto* moveNextTy = llvm::FunctionType::get(ptrTy(), {ptrTy()}, false);
    auto* currentTy = llvm::FunctionType::get(elemTy, {ptrTy()}, false);

    auto* condBB = llvm::BasicBlock::Create(llvm_, "afor.cond", currentFn_);
    auto* bodyBB = llvm::BasicBlock::Create(llvm_, "afor.body", currentFn_);
    auto* endBB  = llvm::BasicBlock::Create(llvm_, "afor.end",  currentFn_);
    builder_.CreateBr(condBB);

    // cond: task = MoveNextAsync(enum); hasValue = await task.
    builder_.SetInsertPoint(condBB);
    // §30.8 (#790): cooperative cancellation — before advancing the iterator,
    // observe the `with` token. When it is cancelled, dispose the enumerator
    // frame and raise OperationCanceledException; the consuming method observes
    // cancellation at this suspend point (§30.8). CancellationToken.None (a null
    // state cell) never cancels.
    if (tokenVal) {
        auto* isCancFn = declareC("bz_ctoken_is_cancelled", i32(), {ptrTy()});
        llvm::Value* cancelled = builder_.CreateICmpNE(
            builder_.CreateCall(isCancFn, {tokenVal}, "afor.canc"),
            builder_.getInt32(0));
        auto* cancelBB =
            llvm::BasicBlock::Create(llvm_, "afor.cancel", currentFn_);
        auto* proceedBB =
            llvm::BasicBlock::Create(llvm_, "afor.proceed", currentFn_);
        builder_.CreateCondBr(cancelled, cancelBB, proceedBB);
        // cancelBB: throw a catchable OperationCanceledException. The iterator
        // frame is disposed by the disposal scope on the resulting unwind (#792 —
        // no longer an explicit per-cancel-path dispose). emitThrowValue
        // terminates the block (branch to unwind).
        builder_.SetInsertPoint(cancelBB);
        auto vit = builtinVtable_.find("OperationCanceledException");
        if (vit != builtinVtable_.end()) {
            llvm::Value* oce = builder_.CreateCall(
                callocFn(),
                {builder_.getInt64(1), builder_.getInt64(kExcObjSize)}, "afor.oce");
            builder_.CreateStore(vit->second, oce);
            emitThrowValue(oce);
        } else {
            builder_.CreateBr(proceedBB);  // no OCE vtable available — fall back
        }
        builder_.SetInsertPoint(proceedBB);
    }
    llvm::Value* enumObj = builder_.CreateLoad(ptrTy(), enumSlot);
    llvm::Value* vt = builder_.CreateLoad(ptrTy(), enumObj);
    llvm::Value* mnSlotPtr =
        builder_.CreateGEP(ptrTy(), vt, {builder_.getInt32(2)});
    llvm::Value* mnFnPtr = builder_.CreateLoad(ptrTy(), mnSlotPtr);
    llvm::Value* task =
        builder_.CreateCall(moveNextTy, mnFnPtr, {enumObj}, "afor.task");
    // §30.7: await the per-advance task (a single-use producer task — free it
    // after its bool is read). This is a suspend point in the consuming method.
    llvm::Value* hasValue =
        emitAwaitSuspend(task, i1(), /*freeTaskAfter=*/true);
    if (!hasValue) return false;
    builder_.CreateCondBr(hasValue, bodyBB, endBB);

    // body: x := Current(enum) for IAsyncEnumerable / TakeCurrent(enum) for
    // IOwningAsyncEnumerable (§29.1 v0.161 #1349; same vtable slot 3).
    builder_.SetInsertPoint(bodyBB);
    llvm::Value* enumObj2 = builder_.CreateLoad(ptrTy(), enumSlot);
    llvm::Value* vt2 = builder_.CreateLoad(ptrTy(), enumObj2);
    llvm::Value* curSlotPtr =
        builder_.CreateGEP(ptrTy(), vt2, {builder_.getInt32(3)});
    llvm::Value* curFnPtr = builder_.CreateLoad(ptrTy(), curSlotPtr);
    llvm::Value* curVal =
        builder_.CreateCall(currentTy, curFnPtr, {enumObj2}, "afor.cur");
    emitPendingCheck();  // TakeCurrent may raise IOE on OwnedAsync path (#1349)
    builder_.CreateStore(curVal, varSlot);

    // #792 / §29.7: loopScopeDepth = outerDepth (BEFORE the enumerator scope) so
    // `break`/`return` out of the loop disposes the iterator frame too.
    loopStack_.push_back({condBB, endBB, outerDepth});
    bool term = lowerForBody(fs);
    loopStack_.pop_back();
    if (!term) builder_.CreateBr(condBB);

    // end: normal exhaustion. Dispose the enumerator frame (vtable slot 1, via the
    // scope's iterEnumDispose — runs any pending finally + frees the frame), then
    // pop the scope. #792: this single scope now covers normal exit AND the
    // break/return/unwind paths (previously only this normal-exit dispose existed).
    builder_.SetInsertPoint(endBB);
    emitDisposals(disposeStack_.back());
    disposeStack_.pop_back();
    return true;
}


bool Emitter::lowerLazyLinqFor(const ast::ForStmt& fs) {
    if (!fs.iterable) return false;
    auto* iterTy = ctx_.exprType(*fs.iterable);
    // Only a lazy LINQ pipeline result (the structural sentinel) is handled
    // here; iterator state machines and Array<T> go through other paths.
    if (!isLazyEnumerableType(iterTy)) return false;

    // #752: route through lowerToLazyEnumerator (not lowerExpr) so a bare
    // `GroupBy(...)` iterable — which lowerExpr cannot lower to a usable
    // enumerator — is materialized into a lazy ARRAY source over its
    // IGrouping elements, exactly as the Count()/Any()/Sum() consumers do.
    // For an ordinary lazy pipeline (Where/Select/…) lowerToLazyEnumerator
    // returns lowerExpr(iterable) unchanged, so this is behaviour-preserving.
    sema::TypeRef* lazyElemRef = nullptr;
    // #1914: a bare `for var g in seq.GroupBy(sel)` drains the fresh groups and
    // disposes the enumerator at loop end — so it OWNS the groups temporary (each
    // grouping consumed inside the loop, freed only after). Pass ownFreshGroupings
    // so the lazy source frees the groupings + array on dispose (no leak); the
    // borrowing default is kept for every re-owning / snapshot-reading consumer.
    llvm::Value* lazy = lowerToLazyEnumerator(*fs.iterable, lazyElemRef,
                                              /*ownFreshGroupings=*/true);
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
    // contBB: join point for the normal loop-exit path (endBB → contBB).
    // Code after the for-in continues here.
    auto* contBB = llvm::BasicBlock::Create(llvm_, "lz.cont", currentFn_);

    // #1226 / §29.11: a bound lazy LINQ var is single-consumption (affine).
    // After the first for-loop the slot is nulled (see endBB).  On a second
    // for-loop over the same bound variable the slot holds null; detect this
    // and throw InvalidOperationException — a silent-empty second iteration
    // (the prior #1218 behaviour) is unsound: it masks the programming error
    // without a useful signal.  A fresh inline lazy (iterable is a call
    // expression, not a named local) can never be null here.
    llvm::Value* lazyIsNull = builder_.CreateICmpEQ(
        lazy, llvm::ConstantPointerNull::get(ptrTy()), "lz.isnull");
    emitGuardThrow(lazyIsNull, "InvalidOperationException");
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
    lowerForBody(fs);
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
    // #1218 / §5: null the bound variable's slot so a subsequent access —
    // a second for-loop or a chained terminal — safely sees null (the null-guard
    // above skips the loop body cleanly rather than dereferencing freed memory).
    // Only fires when the iterable is a named local or parameter (IdentifierExpr);
    // a fresh inline lazy (CallExpr / chained operator) has no owning slot to null.
    // Per §29.11 pattern: localSlot handles both unboxed and cell-boxed variables.
    if (llvm::AllocaInst* boundSlot = localSlot(*fs.iterable)) {
        llvm::Type* ety = nullptr;
        llvm::Value* addr = boxedStorage(boundSlot, ety);
        builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), addr);
    }
    builder_.CreateBr(contBB);

    builder_.SetInsertPoint(contBB);
    return true;
}


bool Emitter::lowerCaseStmt(const ast::CaseStmt& cs) {
    // §19 / §A (#727): a scrutinee-less `case cond => ... end` statement is an
    // ordered boolean-condition chain (like if/elif); each arm's pattern is a
    // bool condition and `_` is the catch-all.
    llvm::Value* scrut = nullptr;
    const sema::TypeSymbol* enumTy = nullptr;
    bool isStr = false;
    const sema::TypeRef* scrutTy = nullptr;
    if (cs.scrutinee) {
        scrut = lowerExpr(*cs.scrutinee);
        if (!scrut) return false;
        enumTy = enumTypeOf(*cs.scrutinee);
        isStr = isStringExprType(*cs.scrutinee);
        scrutTy = ctx_.exprType(*cs.scrutinee);
    }
    auto* endBB =
        llvm::BasicBlock::Create(llvm_, "casestmt.end", currentFn_);
    for (const auto& arm : cs.arms) {
        llvm::Value* pm = nullptr;
        if (!cs.scrutinee) {
            // #727: scrutinee-less arm — `_` always matches; otherwise the arm's
            // pattern is one or more bool conditions (comma = OR).
            if (dynamic_cast<const ast::DiscardPattern*>(arm.pattern.get())) {
                pm = builder_.getInt1(true);
            } else if (auto* cp = dynamic_cast<const ast::ConstantPattern*>(
                           arm.pattern.get())) {
                for (const auto& c : cp->constants) {
                    if (!c) continue;
                    llvm::Value* cond = lowerExpr(*c);
                    if (!cond) continue;
                    if (!cond->getType()->isIntegerTy(1))
                        cond = builder_.CreateICmpNE(
                            cond, llvm::ConstantInt::get(cond->getType(), 0));
                    pm = pm ? builder_.CreateOr(pm, cond) : cond;
                }
            }
        } else {
            pm = patternMatches(arm.pattern.get(), scrut, enumTy, isStr, scrutTy);
        }
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


// §6.4 / issue #647: `atom.Value := v` — seq-cst store into Atomic<T> /
// AtomicInteger<T> via bz_atomic_store(atom, val_as_u64, cell_size).
// Returns true when handled (so the main assignment dispatcher returns early).
bool Emitter::tryLowerAtomicValueSet(const ast::AssignmentStmt& as) {
    if (!as.target || !as.value || as.op != ":=") return false;
    auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(as.target.get());
    if (!ma || !ma->target || ma->member != "Value") return false;
    auto* recvTy = ctx_.exprType(*ma->target);
    std::string_view rbn = builtinTypeName(recvTy);
    if (rbn != "Atomic" && rbn != "AtomicInteger") return false;

    // Evaluate the atom pointer and the new value.
    llvm::Value* atom = lowerExpr(*ma->target);
    if (!atom) return true;  // handled (to suppress fallthrough to lvalueAddr)
    llvm::Value* newVal = lowerExpr(*as.value);
    if (!newVal) return true;

    // Determine cell_size.
    int cs = 4;
    if (recvTy && !recvTy->genericArgs.empty() && recvTy->genericArgs[0]) {
        const sema::TypeRef* tArg = recvTy->genericArgs[0];
        if (tArg->kind == sema::TypeRef::Kind::Primitive) {
            using P = sema::PrimitiveType;
            switch (tArg->primitive) {
                case P::Bool: case P::Byte: case P::SByte: cs = 1; break;
                case P::Short: case P::UShort: cs = 2; break;
                case P::Long: case P::ULong:  cs = 8; break;
                default: cs = 4; break;
            }
        } else {
            cs = 8;  // IntPtr/UIntPtr
        }
    }

    // Widen newVal to i64 for bz_atomic_store's uint64_t parameter.
    llvm::Value* v64 = newVal;
    if (newVal->getType()->isIntegerTy() && !newVal->getType()->isIntegerTy(64))
        v64 = builder_.CreateZExt(newVal, i64(), "aset.v64");

    auto* storeFn = declareC("bz_atomic_store",
                              llvm::Type::getVoidTy(llvm_),
                              {ptrTy(), i64(), i32()});
    builder_.CreateCall(storeFn,
        {atom, v64, builder_.getInt32(cs)});
    return true;
}


}  // namespace bezel::codegen
