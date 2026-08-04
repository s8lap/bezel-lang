#include "codegen/LlvmBackend_impl.hpp"

#include "common/OwnershipFacts.hpp"  // §28.1 per-instantiation owned-marker erasure (#2047)

// This translation unit holds the definitions of Emitter's §28 disposal /
// unwind / auto-drop member functions, extracted mechanically from
// LlvmBackend_Setup.cpp (definition-motion only, no behavior change;
// #1414 stage 5-replacement) to carve the disposal machinery out of that
// ~15.4k-line TU.  The Emitter class is declared in LlvmBackend_impl.hpp and
// its declarations are unchanged; only definitions moved here.
//
// Contents (all §28 machinery): the per-type disposer body / core emission
// (findDestructor, emitDisposer*, emitOwnedFieldDisposals, emitSpecializedDisposer);
// the exception-cleanup / unwind-disposal path (excGlobal, emitExcSlot,
// propagateBB, unwindBB, emitUnwindDisposals*, emitPendingCheck*, emitDynamicDispose,
// emitDisposeAndReclaim); the element/shared/weak/grouping/subscription/box
// disposer thunks and emitBaseDisposeChain; the drop-on-overwrite disposal
// (disposeDisplacedOwnedClassLocal, §28.7); and the scope-exit disposal driver
// (emitDisposals, emitAllDisposals, emitLoopDisposals).
//
// `common::isOwningPrefixedName` (used by the collection-element disposer paths)
// is declared in common/BuiltinRegistry.hpp, transitively included via
// LlvmBackend_impl.hpp.

namespace bezel::codegen {


// §28.4: at most one destructor per class. Returns the AST DestructorDecl for
// `ts` (or nullptr if none — emitDisposerBody then skips the destructor-body
// step and runs only owned-field disposal + base-chain Dispose).
const ast::DestructorDecl* Emitter::findDestructor(const sema::TypeSymbol* ts) {
    for (const sema::Symbol* m : ts->members) {
        if (dynamic_cast<const sema::DestructorSymbol*>(m)) {
            return dynamic_cast<const ast::DestructorDecl*>(m->decl);
        }
    }
    return nullptr;
}


void Emitter::emitDisposerBody(const sema::TypeSymbol* ts, llvm::Function* fn) {
    // §28.2 / §28.4 / §28.6: `fn` is the PUBLIC, idempotent Dispose (vtable slot
    // 1). It guards a single call to the UNGUARDED core (disposerCoreFns_[ts])
    // with the per-object run-once flag, so the destructor body, owned-field
    // auto-disposal and base chain execute AT MOST ONCE per object no matter how
    // many dispose sites reach this Dispose (explicit `.Dispose()`, scope-end
    // auto-dispose, owned-field auto-dispose, exception unwind). The block free
    // is NOT performed here; it is done once by the disposal site that holds the
    // owning slot (emitDisposals / emitOwnedFieldDisposals / the caught-exception
    // and unwind paths), so a re-dispatch never reads an already-freed object.
    llvm::Function* coreFn = disposerCoreFns_[ts];
    emitDisposerCore(ts, coreFn);

    currentFn_ = fn;
    currentThis_ = &*fn->args().begin();
    locals_.clear();
    paramSlots_.clear();
    refParams_.clear();
    disposeStack_.clear();
    loopStack_.clear(); finallyStack_.clear();
    propagateBB_ = nullptr;
    unwindTarget_ = nullptr;
    currentCaughtExc_ = nullptr;
    currentCaughtName_ = {};
    unwindScopeDepth_ = 0;
    builder_.SetInsertPoint(llvm::BasicBlock::Create(llvm_, "entry", fn));
    // §28.2: load the run-once flag; if already set, this is a repeat disposal —
    // a no-op (§28.2: "subsequent calls are no-ops"). Else set it BEFORE running
    // the core so a re-entrant dispose during the core (e.g. a destructor that
    // disposes `this` transitively) still short-circuits.
    llvm::StructType* st = classStruct(ts);
    unsigned fi = disposedFlagIndex(ts);
    llvm::Value* flagAddr =
        builder_.CreateStructGEP(st, currentThis_, fi, "disposed.flag");
    llvm::Value* already = builder_.CreateICmpNE(
        builder_.CreateLoad(disposedFlagTy(), flagAddr),
        llvm::ConstantInt::get(disposedFlagTy(), 0), "already.disposed");
    auto* doBB = llvm::BasicBlock::Create(llvm_, "dispose.do", fn);
    auto* retBB = llvm::BasicBlock::Create(llvm_, "dispose.ret", fn);
    builder_.CreateCondBr(already, retBB, doBB);
    builder_.SetInsertPoint(doBB);
    builder_.CreateStore(llvm::ConstantInt::get(disposedFlagTy(), 1), flagAddr);
    builder_.CreateCall(coreFn, {currentThis_});
    builder_.CreateBr(retBB);
    builder_.SetInsertPoint(retBB);
    builder_.CreateRetVoid();
}


void Emitter::emitDisposerCore(const sema::TypeSymbol* ts, llvm::Function* fn) {
    currentFn_ = fn;
    currentThis_ = &*fn->args().begin();
    locals_.clear();
    paramSlots_.clear();
    refParams_.clear();
    disposeStack_.clear();
    loopStack_.clear(); finallyStack_.clear();
    propagateBB_ = nullptr;
    unwindTarget_ = nullptr;
    currentCaughtExc_ = nullptr;
    currentCaughtName_ = {};
    unwindScopeDepth_ = 0;
    builder_.SetInsertPoint(llvm::BasicBlock::Create(llvm_, "entry", fn));
    bool terminated = false;
    // fix #1607: the exception parked across the destructor body (see below),
    // kept in a local (disposerEntryExc_ itself is reset to null once the body
    // finishes) so the normal-completion restore after the `if` below can still
    // reach it.
    llvm::Value* parkedDtorExc = nullptr;
    if (const ast::DestructorDecl* dd = findDestructor(ts)) {
        // #383 / §28.8.1: suppress dynamic disposal of `using var u := move
        // this._f;` inside the destructor — the auto field-dispose below would
        // dispose the same (moved-from) field, double-freeing it.
        //
        // fix #1468 / §28.3 "Exception-path discharge" / §28.4: capture bz_exc as
        // it stands at disposer entry. When this destructor runs as an owned
        // local/field is disposed DURING stack unwinding, bz_exc already holds the
        // PROPAGATING exception here. The body's pending-checks (emitPendingCheck)
        // compare against this captured value so they fire only for a NEW
        // exception the body itself threw — not the pre-existing propagating one,
        // which would otherwise misfire, abandon the rest of the destructor, and
        // leak the object plus its whole owned subtree. The load sits in the entry
        // block (the current insert point), so it dominates every body block. On
        // the normal, non-unwind disposal path this value is null at runtime, so
        // the comparison in emitPendingCheck reduces to the plain `exc != null`.
        disposerEntryExc_ =
            builder_.CreateLoad(ptrTy(), emitExcSlot(), "disposer.entry.exc");
        // fix #1607 / §28.3 "Exception-path discharge" / §28.4: the AND-trick
        // above (emitPendingCheck comparing against disposerEntryExc_) only
        // covers pending-checks emitted DIRECTLY inside this destructor body
        // (gated on emittingDisposerBody_). It does NOT cover a callee compiled
        // as an ORDINARY function — e.g. `destructor S.Bump(); end destructor;`
        // where Bump is a plain static method whose own body touches a user
        // static. Bump is compiled once with emittingDisposerBody_ == false, so
        // its own §22.13 static-init guard's emitPendingCheck reads the raw
        // bz_exc slot with no adjustment: seeing the propagating exception
        // still parked there, it misreads it as "Bump itself just threw" and
        // returns before running the rest of Bump's body (S.x never
        // increments) — the residual #1468 leak this fixes.
        //
        // PARK bz_exc for the run of the destructor body: clear it here so
        // every callee (Bump, and anything it transitively calls) sees a
        // clean exception context, and restore it — combined via select with
        // any genuinely NEW exception the body raised — at every reachable
        // exit from the body (the normal fallthrough below, and an early
        // `return;` inside the body, per §3459: LlvmBackend_Stmts.cpp's plain
        // ReturnStmt path). On the normal, non-unwind disposal path this
        // value is null at runtime, so clear/restore is a no-op there.
        parkedDtorExc = disposerEntryExc_;
        builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()),
                             emitExcSlot());
        emittingDisposerBody_ = true;
        // fix #1546 / §28.4 (v0.180 throwing-destructor defined behavior):
        // "Before it propagates, the remaining steps of the generated
        // Dispose() still run: the remaining owned-field disposals ...
        // and the base-class chain complete, and physical reclamation
        // still occurs." Without a local redirect, a `throw` (or a
        // propagating call) directly inside the destructor body branches
        // straight to this function's unwindBB()/propagateBB() — which,
        // for a non-`main` function, is an immediate `ret void` — skipping
        // every step below. Install a local unwind target for the body
        // only: a throw inside it lands in dtorThrewBB (after
        // emitThrowValue / emitPendingCheck has already stored the new
        // exception into bz_exc and run any unwind disposals the body's
        // OWN scopes need — e.g. a `using var` inside the destructor),
        // which then falls into the SAME post-body cleanup used for
        // normal completion, so the field cascade / base chain / reclaim
        // run on the throwing path exactly as on the non-throwing one.
        // (An early `return;` inside the body is a separate, pre-existing
        // deferred corner — see the comment below — unaffected by this.)
        llvm::BasicBlock* savedUnwindTarget = unwindTarget_;
        auto* dtorThrewBB =
            llvm::BasicBlock::Create(llvm_, "dtor.threw", fn);
        unwindTarget_ = dtorThrewBB;
        bool bodyFellThrough = !lowerScopedBlock(dd->body);
        unwindTarget_ = savedUnwindTarget;
        emittingDisposerBody_ = false;
        disposerEntryExc_ = nullptr;
        if (bodyFellThrough) {
            // Normal completion (no throw reached dtorThrewBB on this
            // path): join with any throwing path via the same cleanup
            // block below instead of running the shared cleanup twice.
            builder_.CreateBr(dtorThrewBB);
        }
        builder_.SetInsertPoint(dtorThrewBB);
        terminated = false;  // fall through into the shared cleanup below,
                              // on BOTH the thrown and the fell-through path
    }
    // §28.4: after the destructor body, dispose owned fields (reverse
    // declaration order), then chain to the base class's CORE disposer.
    // (A destructor that returns early skips this — destructors with
    // both an early return and owned fields are a deferred corner.)
    if (!terminated) {
        // fix #1607: restore the parked exception (see above) before running
        // the owned-field cascade / base chain, which may themselves recurse
        // into another type's emitDisposerCore (the "transitive owned-field
        // compounding" case #1468's tests already cover) — that nested call
        // must see the correctly-restored propagating value at ITS OWN entry.
        if (parkedDtorExc) {
            llvm::Value* excNow =
                builder_.CreateLoad(ptrTy(), emitExcSlot(), "postdtor.exc");
            llvm::Value* isNew = builder_.CreateICmpNE(
                excNow, llvm::ConstantPointerNull::get(ptrTy()));
            // fix #2358 / §28.4 + §31.2 (finally-supersession discipline, cited
            // by §28.4's "the later exception supersedes the earlier one"
            // multi-value-cleanup-pass rule): when the destructor body ITSELF
            // threw (isNew) while an exception was ALREADY propagating at this
            // disposal's entry (parkedDtorExc non-null AT RUNTIME — an
            // "exception-path discharge", §28.3), the new exception supersedes
            // the parked one. Per §31.2, "the superseded exception is disposed
            // by the runtime exactly once, as if its handling had completed" —
            // dispose+reclaim it HERE, exactly as emitPendingCheckIf /
            // emitNewPendingCheck already do for the sibling finally/catch-guard
            // supersede (#1216/#1149), including the #1288/§30.9 deferred-fault
            // clear so a superseded task-owned fault's single-dispose invariant
            // still holds. Without this, parkedDtorExc was silently dropped on
            // the floor by the select below — losing the original exception's
            // disposal and leaking its object (the reported #2358 48-byte leak).
            // Gated on BOTH conditions at runtime (not just isNew) so the
            // ordinary non-unwind throwing-destructor path (parkedDtorExc ==
            // null here — no exception was in flight) takes the plain-select
            // path unchanged: freeing a null parkedDtorExc would be wrong.
            llvm::Value* hadPending = builder_.CreateICmpNE(
                parkedDtorExc, llvm::ConstantPointerNull::get(ptrTy()));
            llvm::Value* supersedes = builder_.CreateAnd(isNew, hadPending);
            auto* supersedeBB =
                llvm::BasicBlock::Create(llvm_, "dtor.supersede", currentFn_);
            auto* restoreBB =
                llvm::BasicBlock::Create(llvm_, "dtor.restore", currentFn_);
            builder_.CreateCondBr(supersedes, supersedeBB, restoreBB);
            builder_.SetInsertPoint(supersedeBB);
            builder_.CreateCall(
                declareC("bz_clear_exc_deferred", llvm::Type::getVoidTy(llvm_),
                         {ptrTy()}),
                {parkedDtorExc});
            emitDisposeAndReclaim(parkedDtorExc);
            builder_.CreateBr(restoreBB);
            builder_.SetInsertPoint(restoreBB);
            llvm::Value* restored =
                builder_.CreateSelect(isNew, excNow, parkedDtorExc);
            builder_.CreateStore(restored, emitExcSlot());
        }
        emitOwnedFieldDisposals(ts);
        emitBaseDisposeChain(ts);
        builder_.CreateRetVoid();
    }
}


// §28.7 / §30.5 (#2173 / #1889): drop-on-overwrite / scope-end of an owned
// Task-family FIELD. The full using-var task drop sequence MUST run on the
// OLD task pointer at fieldAddr (load-old already done by the caller; the
// slot also already null-guarded by the caller — for the field-store case,
// the caller has nulled the field while retaining both the OLD task in `oldVal`
// and the already-evaluated new task in SSA; for the scope-end case, the caller
// has loaded the field and confirmed it is non-null). The sequence is identical
// the using-var task disposal (the disposal sequence in this file's
// emitDisposals -> if (it->task) block), EXTRACTED here so a single
// maintenance burden covers both the field-store path (#2173) and the
// scope-end path (#1889). Specifically:
//
//   1. Error-obj prelude (the §30.9 fault-ownership check): a faulted task
//      owns its fault exception and disposes it exactly once at task
//      disposal. We dispose the fault now (or defer if it is in flight per
//      the #1288 / #1197 scope-end protocol), or skip unconditionally for
//      a borrowed aggregate (§30.10 / #1529).
//
//   2. Per-T disposer attach (§30.5 / #1258 / #1503): for an OwningTask
//      <T:class> field, the disposer thunk for T is attached via
//      bz_task_set_result_disposer so an unconsumed Owned T result is freed
//      whenever bz_task_free reclaims the task (whether synchronously now
//      or later via fire-and-forget's completion continuation).
//
//   3. Fire-and-forget (§30.5 / §30.10): bz_task_fire_and_forget
//      registers a continuation that calls bz_task_free when the task is
//      done. In checked mode, bz_task_fire_and_forget_checked is used so
//      an abandoned continuation touching disposed state raises
//      DisposedAccessException (instead of UAF).
//
// `target` is the field expression (used to look up the field symbol and
// hence the declared Task/Task<T>/OwningTask<T> type); for the field-store
// case the original `target` is passed in by lowerOwnedTaskFieldStore, for
// the scope-end case the field's expression is reconstructed from the
// FieldSymbol pointer (its `identifier` is on the field itself).
//
// `fieldAddr` is the slot address (struct field GEP); `oldVal` is the
// already-loaded task pointer at that slot. The helper does NOT null the
// slot — the scope-end caller nulls the slot after the call (the re-
// entrant auto-dispose guard mirrors the subscriptionField / eventSource-
// Field pattern lower in this file); the field-store caller nulled the slot
// before this call and stores the already-evaluated new value afterward.
void Emitter::emitOwnedTaskDisposal(const sema::FieldSymbol* fs,
                                    llvm::Value* oldVal) {
    emitTaskDisposal(fs ? taskFamilyTypeFromField(fs) : nullptr, oldVal);
}

// §28.3 / §30.5 (#1905): the full Task-family disposal protocol for an
// already-evaluated owned task value.  Field disposal delegates here above;
// unbound Task temporaries consumed by synchronous `Wait()` use the same
// protocol at their full-expression boundary.  Sharing this sequence is
// load-bearing for fault-object ownership and unconsumed OwningTask<T> results —
// a bare bz_task_free would reclaim only the task struct.
void Emitter::emitTaskDisposal(const sema::TypeRef* taskTy,
                               llvm::Value* oldVal) {
    if (!oldVal) return;
    if (oldVal == llvm::ConstantPointerNull::get(ptrTy())) return;
    // §30.5 / §29.2 (#1891): "the iteration variable is owned; the §28.3
    // discharge rules apply on every body-exit edge" — for an await-for
    // binding, TakeCurrent transferred the element into the for-slot, so
    // explicit `x.Dispose()` (or the auto-drop at iteration end) is this
    // scope's sole reclaim path.  The async path was previously omitted; run
    // the dispose+free the destructor call did not.
    // Per §30.15 a Channel<T>.Receive() task uses bz_channel_receive_abandon
    // instead. A plain field typed Task<T> / OwningTask<T> cannot
    // syntactically be a Channel<T>.Receive() call (the field's type is the
    // task type, not a call), so the receive-abandon branch is unreachable
    // here. Defensive note: a `using var` assigned `Channel<T>.Receive()`
    // gets a *local* not a field, so emitOwnedTaskDisposal is never called
    // for it.
    //
    // §30.9 fault ownership (v0.179): a faulted task OWNS its fault
    // exception and disposes it exactly once at task disposal. The task
    // struct is freed by bz_task_free / fire-and-forget but NOT the Bezel
    // exception object; only Bezel codegen can run its destructor. Three
    // paths:
    //   • BORROWED aggregate (§30.10 / #1529): a WhenAll/WhenAll<T>
    //     aggregate never owns its error_obj — skip unconditionally.
    //   • NOT in flight (not borrowed): dispose the exception now.
    //   • IN FLIGHT (borrowed by an awaiter mid-dispatch): defer disposal
    //     to the catch arm's terminal act (#1288 / #1197).
    // (A still-running async task is not yet errored here → skipped; a
    // non-faulted task has a null error_obj → skipped. bz_task_free below
    // ignores error_obj, so the now-dangling task->error_obj is never read.)
    {
        auto* isErrFn = declareC("bz_task_is_error", i1(), {ptrTy()});
        llvm::Value* err = builder_.CreateCall(isErrFn, {oldVal}, "otask.err");
        auto* edBB = llvm::BasicBlock::Create(llvm_, "otask.errdisp",
                                              currentFn_);
        auto* eaBB = llvm::BasicBlock::Create(llvm_, "otask.errcont",
                                              currentFn_);
        builder_.CreateCondBr(err, edBB, eaBB);
        builder_.SetInsertPoint(edBB);
        auto* fbFn = declareC("bz_task_fault_borrowed",
                              llvm::Type::getInt8Ty(llvm_), {ptrTy()});
        llvm::Value* fb = builder_.CreateCall(fbFn, {oldVal}, "otask.faultborrowed");
        llvm::Value* isFB = builder_.CreateICmpNE(
            fb, llvm::ConstantInt::get(llvm::Type::getInt8Ty(llvm_), 0));
        auto* borrowedAggBB = llvm::BasicBlock::Create(
            llvm_, "otask.eo.aggborrowed", currentFn_);
        auto* checkBB = llvm::BasicBlock::Create(llvm_, "otask.eo.check",
                                                 currentFn_);
        builder_.CreateCondBr(isFB, borrowedAggBB, checkBB);
        builder_.SetInsertPoint(borrowedAggBB);
        builder_.CreateBr(eaBB);
        builder_.SetInsertPoint(checkBB);
        auto* eoFn = declareC("bz_task_error_obj", ptrTy(), {ptrTy()});
        llvm::Value* eo = builder_.CreateCall(eoFn, {oldVal}, "otask.errobj");
        llvm::Value* eoNull = builder_.CreateICmpEQ(
            eo, llvm::ConstantPointerNull::get(ptrTy()));
        auto* doBB = llvm::BasicBlock::Create(llvm_, "otask.eodisp",
                                              currentFn_);
        builder_.CreateCondBr(eoNull, eaBB, doBB);
        builder_.SetInsertPoint(doBB);
        auto* isBorrowedFn = declareC("bz_is_exc_borrowed",
                                      llvm::Type::getInt8Ty(llvm_), {ptrTy()});
        llvm::Value* bv = builder_.CreateCall(isBorrowedFn, {eo},
                                              "otask.eo.borrowed");
        llvm::Value* isB = builder_.CreateICmpNE(
            bv, llvm::ConstantInt::get(llvm::Type::getInt8Ty(llvm_), 0));
        auto* deferBB = llvm::BasicBlock::Create(llvm_, "otask.eo.defer",
                                                 currentFn_);
        auto* dispBB  = llvm::BasicBlock::Create(llvm_, "otask.eo.dispnow",
                                                 currentFn_);
        builder_.CreateCondBr(isB, deferBB, dispBB);
        builder_.SetInsertPoint(deferBB);
        auto* deferFn = declareC("bz_defer_exc_dispose",
                                 llvm::Type::getVoidTy(llvm_), {ptrTy()});
        builder_.CreateCall(deferFn, {eo});
        builder_.CreateBr(eaBB);
        builder_.SetInsertPoint(dispBB);
        emitDisposeAndReclaim(eo);
        builder_.CreateBr(eaBB);
        builder_.SetInsertPoint(eaBB);
    }
    // §30.5 / #1258 / #1503: attach the per-T disposer for the
    // produced-but-unconsumed owned class result of an OwningTask<T:class>
    // field BEFORE the task is reclaimed below. Detected by the field's
    // declared type via taskFamilyTypeFromField — mirrors the using-var
    // path's taskOwnedResultDisposer field, which is set at registration
    // time from the local's initializer type. For a Task<T> / Task / value-
    // result OwningTask<T>, the disposer is null and the conditional
    // set_result_disposer call is skipped.
    llvm::Value* taskOwnedResultDisposer = nullptr;
    if (taskTy) {
        auto taskName = [](const sema::TypeRef* rt) -> std::string_view {
            if (!rt) return {};
            if (!rt->declaration) {
                if (rt->astNode && rt->astNode->name &&
                    !rt->astNode->name->parts.empty())
                    return rt->headName();
            } else if (!rt->declaration->decl) {
                return rt->declaration->name;
            }
            return {};
        };
        if (taskName(taskTy) == "OwningTask" &&
            !taskTy->genericArgs.empty() &&
            taskTy->genericArgs[0] &&
            taskTy->genericArgs[0]->kind == sema::TypeRef::Kind::Named &&
            taskTy->genericArgs[0]->declaration &&
            (taskTy->genericArgs[0]->declaration->kind ==
                 ast::TypeDeclKind::Class ||
             taskTy->genericArgs[0]->declaration->kind ==
                 ast::TypeDeclKind::Interface)) {
            llvm::Function* thunk =
                elementPolicyFor(taskTy->genericArgs[0],
                                 /*owningFamily=*/true).disposerThunk;
            if (thunk) {
                taskOwnedResultDisposer =
                    llvm::cast<llvm::Value>(thunk);
            }
        }
    }
    if (taskOwnedResultDisposer) {
        auto* setDispFn = declareC("bz_task_set_result_disposer",
                                   llvm::Type::getVoidTy(llvm_),
                                   {ptrTy(), ptrTy()});
        builder_.CreateCall(setDispFn, {oldVal, taskOwnedResultDisposer});
    }
    // §30.10 (v0.179 / #1520): in checked disposal mode, route this
    // abandonment through bz_task_fire_and_forget_checked instead of the
    // plain entry point. See the using-var task disposal for the full
    // rationale (a still-running task's continuation may resume past
    // this point and touch state this scope's OTHER disposals just tore
    // down, and DisposedAccessException is the §30.10 source-outlives-
    // settlement diagnostic).
    if (checkedDisposal_) {
        llvm::Value* daVt = llvm::ConstantPointerNull::get(ptrTy());
        { auto vit = builtinVtable_.find("DisposedAccessException");
          if (vit != builtinVtable_.end()) daVt = vit->second; }
        auto* fn = declareC("bz_task_fire_and_forget_checked",
                            llvm::Type::getVoidTy(llvm_),
                            {ptrTy(), ptrTy()});
        builder_.CreateCall(fn, {oldVal, daVt});
    } else {
        auto* fn = declareC("bz_task_fire_and_forget",
                            llvm::Type::getVoidTy(llvm_), {ptrTy()});
        builder_.CreateCall(fn, {oldVal});
    }
}


void Emitter::emitOwnedFieldDisposals(const sema::TypeSymbol* ts) {
    // Each owned field carries either a class/interface object (disposed via
    // its vtable) or — #362 / stdlib §4.5 — a heap collection (freed via
    // bz_array_free, cascading into class elements for an owning collection),
    // or — §28.13 / stdlib §2.16 (#620) — a Shared<T>/Weak<T> handle (released
    // via bz_shared_release / bz_weak_release),
    // or — §28.5/§24.4 (#879) — a Subscription handle (unsubscribed via
    // bz_subscription_dispose, which is null-safe).
    struct OwnedField {
        const sema::FieldSymbol* fs;
        const sema::TypeRef* collTy;  // non-null → collection field
        // §28.13 (#620): true for Shared<T> owned field (bz_shared_release),
        // false for ordinary class/exception/collection field.
        bool sharedField = false;
        // §28.13 (#620): true for Weak<T> owned field (bz_weak_release).
        bool weakField = false;
        // §28.5/§24.4 (#879): true for Subscription (or Subscription?) owned
        // field (bz_subscription_dispose — null-safe, so no guard needed).
        bool subscriptionField = false;
        // #1643 / §29.7: true for an owned IEnumerable<T>/IOwningEnumerable<T>
        // field — an iterator-vtable object whose Dispose is at slot 1 (which
        // frees the frame itself), dispatched below via the iterEnumDispose
        // path, NOT the class-vtable slot-2 dynamicDispose.
        bool iterField = false;
        // §6.5 / §24.3 (#1646): true for an owned EventSource<T>/AsyncEventSource<T>
        // field — a bz_event_source_t* freed via bz_event_source_free (null-safe),
        // which reclaims the subscriber list and the struct. No vtable.
        bool eventSourceField = false;
        // §28.6 / §30.5 (#1889): true for an owned Task / Task<T> / OwningTask<T>
        // field — their Dispose() runs the full using-var task protocol via
        // emitOwnedTaskDisposal (error_obj prelude + per-T result_disposer +
        // bz_task_fire_and_forget[_checked]), the field analog of the using-var
        // task drop. isOwnedReferenceField rejects Task-family (they have no user
        // TypeDecl / no vtable), so a separate branch is needed. Mirrors the
        // iterField / eventSourceField / sharedField / weakField / subscription
        // companions for the other builtin-handle owned-field families.
        bool taskField = false;
    };
    std::vector<OwnedField> owned;
    for (const sema::Symbol* m : ts->members) {
        auto* fs = dynamic_cast<const sema::FieldSymbol*>(m);
        if (!fs) continue;
        if (fieldIndex_.find(fs) == fieldIndex_.end()) continue;
        if (isOwnedReferenceField(fs)) {
            owned.push_back({fs, nullptr});
        } else if (isOwnedBuiltinExceptionField(fs)) {
            // #405 / §28.5 / §31.1: an `owned` field of BUILTIN exception type.
            // It has no user TypeSymbol (so isOwnedReferenceField rejects it),
            // but it is an ordinary owned heap class instance and is disposed
            // through the same dynamic-dispatch path as a class field below
            // (vtable slot 2 = bz$exDisposeMsg, which #1033 made release the
            // owned Message/ParamName strings before the block is reclaimed).
            owned.push_back({fs, nullptr});
        } else if (const sema::TypeRef* ct = ownedCollectionFieldType(fs)) {
            owned.push_back({fs, ct});
        } else if (isOwnedSubscriptionField(fs)) {
            // §28.5/§24.4 (#879): an `owned` field of Subscription (or
            // Subscription?) type.  Subscription is a compiler-known opaque
            // handle with no vtable; unsubscribe via bz_subscription_dispose
            // (null-safe: `if (!subp) return;` — so Subscription? is safe too).
            OwnedField of{fs, nullptr};
            of.subscriptionField = true;
            owned.push_back(of);
        } else if (isOwnedIteratorInterfaceField(fs)) {
            // #1643 / §29.7: an `owned` IEnumerable<T>/IOwningEnumerable<T>
            // field. Its runtime value is an iterator-vtable object (Dispose at
            // slot 1, which frees the frame itself); disposed below via the
            // slot-1 path, NOT the class-vtable slot-2 emitDisposeAndReclaim.
            OwnedField of{fs, nullptr};
            of.iterField = true;
            owned.push_back(of);
        } else if (isOwnedEventSourceField(fs)) {
            // §6.5 / §24.3 (#1646): an `owned` EventSource<T>/AsyncEventSource<T>
            // field (the event pattern's source, now user-constructible). A
            // bz_event_source_t* with no vtable; freed via bz_event_source_free
            // (null-safe), which reclaims the subscriber list + struct.
            OwnedField of{fs, nullptr};
            of.eventSourceField = true;
            owned.push_back(of);
        } else if (taskFamilyTypeFromField(fs)) {
            // §28.6 / §30.5 (#1889): an `owned` Task / Task<T> / OwningTask<T>
            // FIELD. isOwnedReferenceField returns false for Task-family (they
            // have no user TypeDecl / no vtable, and the isTaskTypeName check
            // is what excludes them), so a separate branch is needed — the
            // sibling branches above (subscriptionField, iterField,
            // eventSourceField, sharedField, weakField) follow the same shape
            // for their respective builtin-handle types. The dispose sequence
            // (error_obj prelude + per-T result_disposer + fire-and-forget
            // [_checked]) is the using-var task disposal protocol, dispatched
            // in the iteration loop below via emitOwnedTaskDisposal.
            //
            // #2260: the `owned` test lives INSIDE taskFamilyTypeFromField
            // (LlvmBackend_Setup.cpp), so this arm — like every sibling above —
            // fires only for an OWNING slot. A NON-`owned` Task-family field is
            // a spec-legal BORROW (§28.5 / §22.12 rule 2(ii)) whose owner is
            // elsewhere; disposing it here was a double free.
            OwnedField of{fs, nullptr};
            of.taskField = true;
            owned.push_back(of);
        } else {
            // §28.13 / stdlib §2.16 (#620): an `owned` field of Shared<T> or
            // Weak<T> type.  These are compiler-known ref-counted heap objects
            // with no user TypeDecl / no vtable; release via the appropriate
            // runtime function instead of the vtable-Dispose path.
            bool isWeak = false;
            if (isOwnedSharedWeakField(fs, &isWeak)) {
                OwnedField of{fs, nullptr};
                if (isWeak) of.weakField = true;
                else        of.sharedField = true;
                owned.push_back(of);
            }
        }
    }
    for (auto it = owned.rbegin(); it != owned.rend(); ++it) {
        llvm::StructType* st = classStruct(ts);
        unsigned idx = fieldIndex_[it->fs];
        llvm::Value* addr = builder_.CreateStructGEP(st, currentThis_, idx);
        llvm::Value* val = builder_.CreateLoad(ptrTy(), addr);
        // #362 / stdlib §4.5: an owned collection field frees its backing and
        // cascades disposal into its class elements (for an owning collection).
        // bz_array_free / bz_llist_free are null-safe, so no field null-check needed.
        if (it->collTy) {
            // #602 / §28.11 v0.135: derive element disposer directly from
            // elementPolicyFor (covering class, interface, Shared/Weak elements).
            // A borrowing Array/Stack/Queue<string> co-owns a +1 per element
            // (retainRelease=true); use the string-release thunk for that row.
            // The two paths are mutually exclusive (string is not a class).
            //
            // §28.5/§28.11, #893 #894: OwningDictionary<K,V> owns its V values
            // (genericArgs[1]), NOT its K keys (genericArgs[0]).  Free via
            // bz_dict_free(ptr, valueDisposer) so class V elements are disposed.
            // Array/Stack/Queue/LinkedList/Buffer own their single generic element
            // (genericArgs[0]) and use bz_array_free / bz_llist_free as before.
            bool isDict = isDictTypeName(it->collTy);
            const sema::TypeRef* collElem =
                isDict
                    ? ((it->collTy->genericArgs.size() > 1)
                           ? it->collTy->genericArgs[1]
                           : nullptr)
                    : ((!it->collTy->genericArgs.empty())
                           ? it->collTy->genericArgs[0]
                           : nullptr);
            // #816: in a per-instantiation (specialized) disposer the element may
            // be a generic type PARAMETER (`owned _arr: OwningArray<T>`); resolve it
            // to the concrete substituted type so elementPolicyFor classifies it
            // (and builds the element disposer thunk) — otherwise a bare `T`
            // element has a null declaration → Value policy → elements leak. A
            // no-op outside a substitution (the shared disposer keeps the bare T).
            collElem = resolveTypeParamRef(collElem);
            bool collIsOwning =
                it->collTy->astNode && it->collTy->astNode->name &&
                !it->collTy->astNode->name->parts.empty() &&
                (common::isOwningPrefixedName(it->collTy->headName()) ||
                 // #2606 / stdlib §4.5: IOwningList<T> is the OWNING list
                 // interface — its elements are owned by the holder — but
                 // isOwningPrefixedName("IOwningList") is false (the head starts
                 // with "I", not "Owning"), so consult the interface recognizer
                 // to mark the family owning (matches owned OwningArray<T>).
                 isOwningListInterfaceName(it->collTy));
            ElementPolicy fieldCollPol = elementPolicyFor(collElem, collIsOwning);
            llvm::Function* elemDisp = fieldCollPol.disposerThunk;
            if (!elemDisp && !isDict &&
                    fieldCollPol.cat == ElementPolicy::Category::RuntimeManaged &&
                    collElem && collElem->kind == sema::TypeRef::Kind::Primitive &&
                    collElem->primitive == sema::PrimitiveType::String &&
                    !collElem->nullable &&
                    it->collTy->astNode && it->collTy->astNode->name &&
                    !it->collTy->astNode->name->parts.empty() &&
                    [](std::string_view n){
                        // #2603 / issue #401: IList<T>/IReadOnlyList<T> are
                        // array-backed + borrowing (same bz_array_t* + per-
                        // element string +1 as Array<T>), so a string-element
                        // field of those interface types releases its elements
                        // through the SAME stringElementReleaseThunk. IOwningList
                        // is owning and never reaches this string gate (owning-
                        // family disposer runs via elementPolicyFor instead).
                        return n == "Array" || n == "Stack" || n == "Queue" ||
                               n == "IList" || n == "IReadOnlyList";
                    }(it->collTy->headName()))
                elemDisp = stringElementReleaseThunk();
            llvm::Value* disp = elemDisp
                ? llvm::cast<llvm::Value>(elemDisp)
                : llvm::ConstantPointerNull::get(ptrTy());
            // per §4.6/§4.7: LinkedList fields free via bz_llist_free.
            // §28.5/§28.11, #893: OwningDictionary fields free via bz_dict_free.
            bool isLL = isLinkedListTypeName(it->collTy);
            // #2608 / stdlib §4.3: an `owned` field typed by a BORROWING list
            // interface (IReadOnlyList<T>/IList<T>) is array-backed, but those
            // interfaces are the read-only view shared by BOTH hierarchies, so
            // the dynamic backing may be a borrowing `Array<T>` (elements must
            // NOT be disposed) or an owning `OwningArray<T>` (elements MUST be).
            // The static type cannot tell them apart, and guessing "owning" here
            // would double-free the borrowing case. When no static element
            // disposer was derived, defer to the disposer the OWNING collection
            // stamped into its header at construction: bz_array_free_dynamic.
            // An untagged (borrowing) backing yields NULL there, i.e. exactly
            // the `bz_array_free(val, null)` this replaces — never a UAF.
            if (!isLL && !isDict && !elemDisp &&
                isBorrowingListInterfaceName(it->collTy)) {
                auto* dynFn = declareC("bz_array_free_dynamic",
                                       llvm::Type::getVoidTy(llvm_), {ptrTy()});
                builder_.CreateCall(dynFn, {val});
                continue;
            }
            const char* freeName = isLL   ? "bz_llist_free"
                                  : isDict ? "bz_dict_free"
                                  :          "bz_array_free";
            auto* fn = declareC(freeName,
                                llvm::Type::getVoidTy(llvm_), {ptrTy(), ptrTy()});
            builder_.CreateCall(fn, {val, disp});
            continue;
        }
        // §28.5/§24.4 (#879): owned Subscription (or Subscription?) field —
        // unsubscribe via bz_subscription_dispose (null-safe: first thing it
        // does is `if (!subp) return;`, so Subscription? left null is safe).
        // Null the field after so a re-entrant dispose is a no-op.
        if (it->subscriptionField) {
            auto* dispFn = declareC("bz_subscription_dispose",
                                    llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(dispFn, {val});
            builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), addr);
            continue;
        }
        // §6.5 / §24.3 (#1646): owned EventSource<T>/AsyncEventSource<T> field —
        // free the subscriber list + struct via bz_event_source_free (null-safe:
        // `if (!s) return;`, so a ctor that threw before the field was assigned is
        // safe). This is the same runtime free the keyword-form EventSymbol
        // backing-field loop later in this function uses (#480). The freed source
        // does NOT cascade into the borrowed handler closures (those are owned by
        // their Subscriptions). Null the field after so a re-entrant dispose is a
        // no-op.
        if (it->eventSourceField) {
            auto* freeFn = declareC("bz_event_source_free",
                                    llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(freeFn, {val});
            builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), addr);
            continue;
        }
        // §28.13 / stdlib §2.16 (#620): owned Shared<T>/Weak<T> field —
        // release via bz_shared_release / bz_weak_release (null-safe, no vtable).
        // Null the field after so a re-entrant dispose is a no-op.
        if (it->sharedField || it->weakField) {
            const char* relName = it->weakField ? "bz_weak_release"
                                                : "bz_shared_release";
            auto* relFn = declareC(relName, llvm::Type::getVoidTy(llvm_),
                                   {ptrTy()});
            builder_.CreateCall(relFn, {val});
            builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), addr);
            continue;
        }
        // #1643 / §29.7: an owned IEnumerable<T>/IOwningEnumerable<T> field
        // holds an iterator-vtable object. Dispatch Dispose via vtable slot 1
        // (the iterator format [0]=td,[1]=Dispose,[2]=MoveNext,[3]=Current),
        // which runs the state machine's pending finally blocks and FREES THE
        // FRAME ITSELF (§29.7) — so NO separate free/reclaim here (unlike the
        // class-vtable slot-2 path below, where Dispose only runs the destructor
        // and emitDisposeAndReclaim must also free). Calling slot 2 here would
        // invoke MoveNext, not Dispose, and leak the frame. Null-guard (a never-
        // assigned or moved-from slot holds null) + null-after (the #396 slot-
        // null precedent) so a re-entrant / second pass is a no-op. Mirrors the
        // scope-end iterEnumDispose path (emitDisposals) and the iterator
        // Dispose-body EnumFrame cleanup (emitIteratorMethod).
        if (it->iterField) {
            llvm::Value* isNull = builder_.CreateICmpEQ(
                val, llvm::ConstantPointerNull::get(ptrTy()));
            auto* iterDispBB = llvm::BasicBlock::Create(
                llvm_, "owned.iter.dispose", currentFn_);
            auto* iterContBB = llvm::BasicBlock::Create(
                llvm_, "owned.iter.cont", currentFn_);
            builder_.CreateCondBr(isNull, iterContBB, iterDispBB);
            builder_.SetInsertPoint(iterDispBB);
            // Dispatch vtable slot 1 (Dispose) — frees the frame.
            llvm::Value* vt = builder_.CreateLoad(ptrTy(), val);
            llvm::Value* dispSlot =
                builder_.CreateGEP(ptrTy(), vt, {builder_.getInt32(1)});
            llvm::Value* dispFnPtr = builder_.CreateLoad(ptrTy(), dispSlot);
            auto* dispTy = llvm::FunctionType::get(
                llvm::Type::getVoidTy(llvm_), {ptrTy()}, false);
            builder_.CreateCall(dispTy, dispFnPtr, {val});
            builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), addr);
            builder_.CreateBr(iterContBB);
            builder_.SetInsertPoint(iterContBB);
            continue;
        }
        // §28.6 / §30.5 (#1889): an owned Task / Task<T> / OwningTask<T>
        // FIELD. The full using-var task protocol runs (error_obj prelude,
        // per-T result_disposer attach, bz_task_fire_and_forget[_checked])
        // via emitOwnedTaskDisposal — the field analog of the using-var task
        // drop. Mirrors the iterField branch above (null-guard + null-after
        // so a re-entrant / second pass is a no-op, per the #396 slot-null
        // precedent). The field-store path at #2173 shares the same helper
        // (lowerOwnedTaskFieldStore calls emitOwnedTaskDisposal too).
        if (it->taskField) {
            llvm::Value* isNull = builder_.CreateICmpEQ(
                val, llvm::ConstantPointerNull::get(ptrTy()));
            auto* taskDispBB = llvm::BasicBlock::Create(
                llvm_, "owned.task.dispose", currentFn_);
            auto* taskContBB = llvm::BasicBlock::Create(
                llvm_, "owned.task.cont", currentFn_);
            builder_.CreateCondBr(isNull, taskContBB, taskDispBB);
            builder_.SetInsertPoint(taskDispBB);
            emitOwnedTaskDisposal(it->fs, val);
            // Null the slot AFTER the disposal so a re-entrant / second
            // pass is a no-op (the #396 slot-null precedent; mirrors
            // iterField / eventSourceField / sharedField / weakField
            // / subscriptionField below).
            builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), addr);
            builder_.CreateBr(taskContBB);
            builder_.SetInsertPoint(taskContBB);
            continue;
        }
        llvm::Value* isNull = builder_.CreateICmpEQ(
            val, llvm::ConstantPointerNull::get(ptrTy()));
        auto* dispBB = llvm::BasicBlock::Create(llvm_, "owned.dispose",
                                                currentFn_);
        auto* contBB = llvm::BasicBlock::Create(llvm_, "owned.cont",
                                                currentFn_);
        builder_.CreateCondBr(isNull, contBB, dispBB);
        builder_.SetInsertPoint(dispBB);
        // §28.4 step 2: dispatch the field object's idempotent Dispose and
        // reclaim its block once, then null the field. §28.5 (line 5066): a
        // destructor body may explicitly `field.Dispose()` BEFORE this auto-
        // dispose — that explicit call only dispatched the field's idempotent
        // Dispose (it did not free), so reclaiming here frees the field exactly
        // once (no double-free). Nulling the field after makes any further
        // disposal a guarded no-op (the #396 slot-null precedent), so a second
        // §28.4-step-2 pass (or a re-entrant dispose) finds null and skips.
        emitDisposeAndReclaim(val);
        builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), addr);
        builder_.CreateBr(contBB);
        builder_.SetInsertPoint(contBB);
    }

    // §15.1.1 line 1957 / §15.3 (#747): a move-only OWNING inline-array FIELD
    // ([N owned]T? / [N owned]T) — "for class fields, the enclosing class's
    // destructor invokes the disposal." Each non-null slot's object is disposed
    // via its own vtable, then nulled (the #396 slot-null precedent guards a
    // re-entrant pass). A slot left moved-from via `move this.field[i]` was
    // nulled at the move (lowerMoveFromCollectionSlot), so the null-guard skips
    // it — no double-free. Unlike the owned-reference fields above there is no
    // single object to dispatch on; the field is a [N x ptr] sub-aggregate.
    {
        llvm::StructType* st = classStruct(ts);
        for (const sema::Symbol* m : ts->members) {
            auto* fs = dynamic_cast<const sema::FieldSymbol*>(m);
            if (!fs) continue;
            if (fieldIndex_.find(fs) == fieldIndex_.end()) continue;
            const sema::TypeRef* ft = nullptr;
            if (auto* fd = dynamic_cast<const ast::FieldDecl*>(fs->decl)) {
                if (fd->type) ft = ctx_.resolvedType(*fd->type);
            }
            if (!ft || ft->kind != sema::TypeRef::Kind::InlineArray ||
                !ft->arrayOwned)
                continue;
            unsigned n = ft->arraySizeValue >= 0
                ? (unsigned)ft->arraySizeValue : 0;
            if (n == 0) continue;
            unsigned idx = fieldIndex_[fs];
            llvm::Value* fieldAddr =
                builder_.CreateStructGEP(st, currentThis_, idx, "ioaf.addr");
            auto* arrTy = llvm::ArrayType::get(ptrTy(), n);
            for (unsigned i = 0; i < n; ++i) {
                llvm::Value* slotAddr = builder_.CreateInBoundsGEP(
                    arrTy, fieldAddr,
                    {builder_.getInt32(0), builder_.getInt32(i)}, "ioaf.slot");
                llvm::Value* obj =
                    builder_.CreateLoad(ptrTy(), slotAddr, "ioaf.val");
                llvm::Value* isNull = builder_.CreateICmpEQ(
                    obj, llvm::ConstantPointerNull::get(ptrTy()));
                auto* dispBB = llvm::BasicBlock::Create(llvm_, "ioaf.dispose",
                                                        currentFn_);
                auto* contBB = llvm::BasicBlock::Create(llvm_, "ioaf.cont",
                                                        currentFn_);
                builder_.CreateCondBr(isNull, contBB, dispBB);
                builder_.SetInsertPoint(dispBB);
                emitDisposeAndReclaim(obj);
                builder_.CreateStore(
                    llvm::ConstantPointerNull::get(ptrTy()), slotAddr);
                builder_.CreateBr(contBB);
                builder_.SetInsertPoint(contBB);
            }
        }
    }

    // #429 / §4.6 / §28.9: release every STRING field / auto-property of THIS
    // class (own members only; an inherited slot is released by the base CORE
    // disposer via emitBaseDisposeChain — so visiting only ts->members avoids a
    // double-release). A class string field holds an INDEPENDENT +1 (retained at
    // store via lowerStringFieldStore / armed in emitInstanceInit's initializer
    // path); on disposal that +1 is released exactly once, freeing the
    // data+header at rc 0. bz_string_release is null-safe (a never-assigned
    // string? slot is null in the calloc'd object) and a no-op on a sentinel
    // (a field holding a literal is never freed). After releasing, NULL the slot
    // so a re-entrant / double dispose finds null and skips (the #396 slot-null
    // precedent) — the §28.6 run-once flag already guards the public Dispose, and
    // this nulling guards any base-core re-entry. A local that aliases this field
    // value holds its OWN +1 (#429 borrow-retain), so this release never dangles
    // it (no UAF). A RECORD has no disposer (§9.7) and its string fields are
    // borrows (§9) — never reached here.
    {
        llvm::StructType* sst = classStruct(ts);
        for (const sema::Symbol* m : ts->members) {
            bool isField = dynamic_cast<const sema::FieldSymbol*>(m) != nullptr;
            if (!isField && !isAutoProperty(m)) continue;
            if (fieldIndex_.find(m) == fieldIndex_.end()) continue;
            if (!isStringStorageMember(m)) continue;
            unsigned idx = fieldIndex_[m];
            llvm::Value* addr = builder_.CreateStructGEP(sst, currentThis_, idx,
                                                         "strf.addr");
            llvm::Value* val = builder_.CreateLoad(ptrTy(), addr, "strf.val");
            emitStringRelease(val);
            builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), addr);
        }
    }

    // #429 (closure FIELD-STORE category) / §27.5 / §28.9: release every CLOSURE
    // field / auto-property of THIS class (own members only; an inherited slot
    // is released by the base CORE disposer via emitBaseDisposeChain — so
    // visiting only ts->members avoids a double-release). A class closure field
    // holds an INDEPENDENT +1 (retained at store via lowerClosureFieldStore /
    // emitInstanceInit's initializer path); on disposal that +1 is released
    // exactly once via bz_closure_release, freeing the cell + env block at rc 0.
    // bz_closure_release is null-safe (a never-assigned closure field slot is
    // null in the calloc'd object) and does NOT touch captured values (they are
    // borrows, §28.9 — no double-dispose of a capture). After releasing, NULL
    // the slot so a re-entrant / double dispose finds null and is a no-op (the
    // #396 slot-null precedent, parallel to the string-field nulling above).
    {
        llvm::StructType* cst = classStruct(ts);
        for (const sema::Symbol* m : ts->members) {
            bool isField = dynamic_cast<const sema::FieldSymbol*>(m) != nullptr;
            if (!isField && !isAutoProperty(m)) continue;
            if (fieldIndex_.find(m) == fieldIndex_.end()) continue;
            if (!isClosureStorageMember(m)) continue;
            unsigned idx = fieldIndex_[m];
            llvm::Value* addr = builder_.CreateStructGEP(cst, currentThis_, idx,
                                                         "clf.addr");
            llvm::Value* val = builder_.CreateLoad(ptrTy(), addr, "clf.val");
            emitClosureRelease(val);
            builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), addr);
        }
    }

    // #429 (box FIELD-STORE category) / §8.6 / §28.9: release every Object/
    // interface field / auto-property of THIS class (own members only; an
    // inherited slot is released by the base CORE disposer via
    // emitBaseDisposeChain — visiting only ts->members avoids double-release).
    // A box storage field holds an INDEPENDENT +1 (taken at store via
    // lowerBoxFieldStore / emitInstanceInit); on disposal that +1 is released
    // EXACTLY ONCE via bz_box_release, freeing the box wrapper at rc 0. Safety:
    //   - isBoxStorageMember gates on boxStorageFields_, which is populated ONLY
    //     for fields where collectBoxStorageFields + lowerBoxFieldStore confirmed
    //     EVERY store uses a boxable value source. A field that ever received a
    //     class-reference store is NOT in boxStorageFields_ → NOT released here.
    //     This is the cardinal box-vs-class discriminator: bz_box_release is
    //     NEVER called on a class instance. (Corrupting offset 8 of a class
    //     instance — the disposed-flag word — would cause double-dispose / UAF.)
    //   - bz_box_release is null-safe (a calloc-zeroed null field is a no-op).
    //   - After releasing, NULL the slot so a re-entrant / double dispose finds
    //     null and is a no-op (the #396 slot-null precedent, parallel to the
    //     string-field and closure-field nulling above).
    //   - The boxed value is a value-type leaf (no owned sub-objects): the
    //     release frees ONLY the wrapper — no cascade.
    {
        llvm::StructType* bst = classStruct(ts);
        for (const sema::Symbol* m : ts->members) {
            bool isField = dynamic_cast<const sema::FieldSymbol*>(m) != nullptr;
            if (!isField && !isAutoProperty(m)) continue;
            if (fieldIndex_.find(m) == fieldIndex_.end()) continue;
            if (!isBoxStorageMember(m)) continue;
            unsigned idx = fieldIndex_[m];
            llvm::Value* addr = builder_.CreateStructGEP(bst, currentThis_, idx,
                                                         "boxf.addr");
            llvm::Value* val = builder_.CreateLoad(ptrTy(), addr, "boxf.val");
            emitBoxRelease(val);
            builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), addr);
        }
    }

    // #480 / §24.1 / §24.5 / §28 (#1176): free each instance event's backing
    // EventSource. emitInstanceInit allocated one bz_event_source_new per
    // instance EventSymbol (sync and async); those allocations are NOT visited
    // by the FieldSymbol loop above because EventSymbol does not inherit from
    // FieldSymbol and the dynamic_cast returns null.  Collect all instance-event
    // symbols that have a fieldIndex_ entry (non-static events, the same
    // predicate as collectInstanceStorage) and emit a null-guarded
    // bz_event_source_free call for each, then store null back so a re-entrant
    // dispose is a no-op.  Ordering relative to FieldSymbol disposals is not
    // safety-critical: EventSource.free (handler array + struct) does not
    // cascade into any owned sub-objects tracked by this class.
    llvm::StructType* st = classStruct(ts);
    auto* freeFn = declareC("bz_event_source_free",
                            llvm::Type::getVoidTy(llvm_), {ptrTy()});
    for (const sema::Symbol* m : ts->members) {
        auto* es = dynamic_cast<const sema::EventSymbol*>(m);
        if (!es) continue;
        auto* ed = dynamic_cast<const ast::EventDecl*>(es->decl);
        if (!ed || ed->isStatic) continue;  // instance events (sync + async)
        auto idxIt = fieldIndex_.find(es);
        if (idxIt == fieldIndex_.end()) continue;
        unsigned idx = idxIt->second;
        llvm::Value* addr = builder_.CreateStructGEP(st, currentThis_, idx,
                                                     "evt.addr");
        llvm::Value* val  = builder_.CreateLoad(ptrTy(), addr, "evt.src");
        // Null-guard: calloc zeroes the slot; if the ctor threw before
        // emitInstanceInit allocated this source the slot is still null.
        llvm::Value* isNull = builder_.CreateICmpEQ(
            val, llvm::ConstantPointerNull::get(ptrTy()));
        auto* freeBB = llvm::BasicBlock::Create(llvm_, "evt.free",  currentFn_);
        auto* contBB = llvm::BasicBlock::Create(llvm_, "evt.cont",  currentFn_);
        builder_.CreateCondBr(isNull, contBB, freeBB);
        builder_.SetInsertPoint(freeBB);
        builder_.CreateCall(freeFn, {val});
        // Null the slot so a second disposal pass (re-entrant or double-dispose)
        // finds null and skips, mirroring the FieldSymbol disposal pattern above.
        builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), addr);
        builder_.CreateBr(contBB);
        builder_.SetInsertPoint(contBB);
    }
}


// §28.5: does `ts` declare at least one `owned` field whose type is one of its
// own generic type parameters (`private owned _slot: T`)? Such a field is
// classified — and so auto-disposed — at INSTANTIATION (the substituted type),
// not in the shared type-erased disposer, so a class with one needs a per-
// instantiation specialized Dispose/vtable.
bool Emitter::classHasOwnedTypeParamField(const sema::TypeSymbol* ts) {
    if (!ts) return false;
    for (const sema::Symbol* m : ts->members) {
        auto* fs = dynamic_cast<const sema::FieldSymbol*>(m);
        if (!fs) continue;
        // (a) a direct owned type-parameter field (`owned _f: T`).
        if (isOwnedTypeParamFieldShape(fs)) return true;
        // #816 (b): a direct owned generic OWNING-collection field whose element
        // is a type parameter (`owned _arr: OwningArray<T>`) — its elements are
        // only disposed by a per-instantiation disposer.
        if (isOwnedTypeParamCollectionField(fs)) return true;
    }
    // #816 (c): a class whose (transitive) BASE has an owned type-parameter field
    // (`class Child<T> : Base<T>` where Base has `owned _f: T`) must ALSO get a
    // specialized disposer, so its emitBaseDisposeChain can invoke the base's
    // SUBSTITUTED disposer (the shared base disposer is type-erased and skips the
    // base's owned-T field — the leak in #816 repro (a)).
    for (const sema::TypeSymbol* base : ts->baseTypes)
        if (base && base->kind == ast::TypeDeclKind::Class &&
            classHasOwnedTypeParamField(base))
            return true;
    return false;
}


// §28.5: build (once per instantiation) the specialized core + public disposer
// for the active monomorphization of `ts`. Mirrors emitDisposerCore /
// emitDisposerBody but is emitted with the current genericSubst_ active, so an
// owned type-parameter field is laid out (the spec struct) and classified (the
// substituted concrete type) correctly. `suffix` disambiguates the function
// names across instantiations. Returns the public idempotent Dispose.
//
// State: the caller (lowerNew, mid-body) owns builder_/currentFn_/etc.; we save
// the full disposer-emit state and the builder insert point and restore them so
// the caller can continue emitting where it left off.
llvm::Function* Emitter::emitSpecializedDisposer(const sema::TypeSymbol* ts,
                                                 const std::string& suffix) {
    auto* fnType = llvm::FunctionType::get(
        llvm::Type::getVoidTy(llvm_), {ptrTy()}, /*isVarArg=*/false);
    auto* pubFn = llvm::Function::Create(
        fnType, llvm::Function::InternalLinkage,
        "bz$" + std::string(ts->name) + "$Dispose" + suffix, module_.get());
    auto* coreFn = llvm::Function::Create(
        fnType, llvm::Function::InternalLinkage,
        "bz$" + std::string(ts->name) + "$DisposeCore" + suffix, module_.get());

    // Save all state the disposer-emit clobbers, plus the builder insert point.
    llvm::Function* savedFn = currentFn_;
    llvm::Value* savedThis = currentThis_;
    auto savedLocals = locals_;
    auto savedParamSlots = paramSlots_;
    auto savedRefParams = refParams_;
    auto savedDisposeStack = disposeStack_;
    auto savedLoopStack = loopStack_;
    auto savedFinallyStack = finallyStack_;
    llvm::BasicBlock* savedPropagate = propagateBB_;
    llvm::BasicBlock* savedUnwind = unwindTarget_;
    llvm::Value* savedCaughtExc = currentCaughtExc_;
    std::string_view savedCaughtName = currentCaughtName_;
    std::size_t savedScopeDepth = unwindScopeDepth_;
    llvm::IRBuilder<>::InsertPoint savedIP = builder_.saveIP();

    // The core (destructor body + owned-field disposal + base chain) and the
    // public run-once wrapper — identical to emitDisposerCore / emitDisposerBody
    // but specialized: genericSubst_ stays active so classStruct(ts) resolves to
    // the spec struct and isOwnedReferenceField sees the concrete T.
    emitDisposerCore(ts, coreFn);

    currentFn_ = pubFn;
    currentThis_ = &*pubFn->args().begin();
    locals_.clear();
    paramSlots_.clear();
    refParams_.clear();
    disposeStack_.clear();
    loopStack_.clear(); finallyStack_.clear();
    propagateBB_ = nullptr;
    unwindTarget_ = nullptr;
    currentCaughtExc_ = nullptr;
    currentCaughtName_ = {};
    unwindScopeDepth_ = 0;
    builder_.SetInsertPoint(llvm::BasicBlock::Create(llvm_, "entry", pubFn));
    llvm::StructType* st = classStruct(ts);
    unsigned fi = disposedFlagIndex(ts);
    llvm::Value* flagAddr =
        builder_.CreateStructGEP(st, currentThis_, fi, "disposed.flag");
    llvm::Value* already = builder_.CreateICmpNE(
        builder_.CreateLoad(disposedFlagTy(), flagAddr),
        llvm::ConstantInt::get(disposedFlagTy(), 0), "already.disposed");
    auto* doBB = llvm::BasicBlock::Create(llvm_, "dispose.do", pubFn);
    auto* retBB = llvm::BasicBlock::Create(llvm_, "dispose.ret", pubFn);
    builder_.CreateCondBr(already, retBB, doBB);
    builder_.SetInsertPoint(doBB);
    builder_.CreateStore(llvm::ConstantInt::get(disposedFlagTy(), 1), flagAddr);
    builder_.CreateCall(coreFn, {currentThis_});
    builder_.CreateBr(retBB);
    builder_.SetInsertPoint(retBB);
    builder_.CreateRetVoid();

    // Restore the caller's state and insert point.
    currentFn_ = savedFn;
    currentThis_ = savedThis;
    locals_ = std::move(savedLocals);
    paramSlots_ = std::move(savedParamSlots);
    refParams_ = std::move(savedRefParams);
    disposeStack_ = std::move(savedDisposeStack);
    loopStack_ = std::move(savedLoopStack);
    finallyStack_ = std::move(savedFinallyStack);
    propagateBB_ = savedPropagate;
    unwindTarget_ = savedUnwind;
    currentCaughtExc_ = savedCaughtExc;
    currentCaughtName_ = savedCaughtName;
    unwindScopeDepth_ = savedScopeDepth;
    builder_.restoreIP(savedIP);
    return pubFn;
}


llvm::GlobalVariable* Emitter::excGlobal() {
    if (!excGlobal_) {
        // fix #492 / §31.1 / §2.6: emit an ExternalLinkage DECLARATION (no
        // initializer) so all separately-compiled modules reference the SAME
        // "bz_exc" symbol defined once in bezel_runtime.cpp. Before this fix
        // each module defined its own InternalLinkage (BSS) copy under the name
        // "bz$exc"; a cross-module `throw` wrote the library's copy while the
        // importer's pending-check read a still-null copy — the exception
        // silently disappeared.
        //
        // An external declaration (nullptr initializer) in LLVM means "defined
        // elsewhere"; the linker resolves all references to the runtime's single
        // definition. The name "bz_exc" must match the name in bezel_runtime.cpp.
        // (Renamed from "bz$exc" — a `$` in an external symbol name is a
        // clang-only extension that triggers -Wdollar-in-identifier-extension in
        // code that declares it in a header. "bz_exc" is a valid C identifier.)
        //
        // fix #704: bz_exc is now thread_local in the runtime, but generated
        // code accesses it via emitExcSlot() → call bz_exc_addr() → load/store
        // through the returned pointer.  This GlobalVariable is kept only as an
        // anchor (it is no longer referenced by any load/store in generated IR);
        // emitExcSlot() is the sole code path that touches the slot at runtime.
        excGlobal_ = new llvm::GlobalVariable(
            *module_, ptrTy(), /*isConstant=*/false,
            llvm::GlobalValue::ExternalLinkage,
            /*Initializer=*/nullptr,  // external declaration — defined in runtime
            "bz_exc");
    }
    return excGlobal_;
}


llvm::Value* Emitter::emitExcSlot() {
    // fix #704 / §30.14: return a ptr to the calling thread's bz_exc slot by
    // calling bz_exc_addr().  Using a plain extern "C" function (not a TLS
    // GlobalVariable) lets the LLVM ORC JIT resolve the symbol via
    // DynamicLibrarySearchGenerator — the host process exports "bz_exc_addr"
    // as a regular function, whereas a TLS GlobalVariable would require the
    // JIT to emit __emutls_v.bz_exc references which are NOT exported by the
    // host process (the host uses real, not emulated, TLS).
    // Per-call (not cached): each load/store gets a fresh call so the slot
    // address is always current (required when called from a thread-pool
    // worker that may switch stacks).
    if (!excAddrFn_) {
        // Declare: ptr bz_exc_addr() — returns &bz_exc for the current thread
        auto* fnTy = llvm::FunctionType::get(ptrTy(),
                                             llvm::ArrayRef<llvm::Type*>{},
                                             /*isVarArg=*/false);
        // fix #2148 follow-up: DEDUPE on an existing declaration. The
        // iterator/async owning-`TakeCurrent` error path independently declares
        // `bz_exc_addr` via module_->getOrInsertFunction (LlvmBackend_Iterators.cpp,
        // LlvmBackend_Async.cpp). A plain llvm::Function::Create does NOT check for
        // an existing global — so when emitExcSlot ran SECOND (that TakeCurrent
        // path emitted first), LLVM renamed this new function to "bz_exc_addr.1",
        // which emitExcSlot then cached and every emitExcSlot caller referenced —
        // an UNDEFINED symbol at link (the runtime exports only "bz_exc_addr"),
        // breaking any owning iterator whose emission order put TakeCurrent first.
        // The type is identical (ptr()) at all three sites, so getOrInsertFunction
        // returns the one canonical Function regardless of which site ran first.
        // per §30.14 / fix #704
        excAddrFn_ = llvm::cast<llvm::Function>(
            module_->getOrInsertFunction("bz_exc_addr", fnTy).getCallee());
    }
    return builder_.CreateCall(excAddrFn_, {}, "exc.slot");
}


llvm::BasicBlock* Emitter::propagateBB() {
    if (!propagateBB_) {
        auto* save = builder_.GetInsertBlock();
        propagateBB_ = llvm::BasicBlock::Create(llvm_, "propagate",
                                                currentFn_);
        builder_.SetInsertPoint(propagateBB_);
        // per §33.3 / §22.13 (line 3845): propagateBB is the per-function
        // fallback when an exception reaches a frame with no enclosing handler
        // (unwindTarget_ unset). For an inner frame the exception is still live
        // in bz$exc, so propagating == returning (emitImplicitReturn leaves
        // bz$exc set and the caller's emitPendingCheck continues the unwind).
        // For `main` (the entry point — the only function named "main", §33.2),
        // this block IS the process boundary: the exception is genuinely
        // uncaught. §33.3 reserves exit code 0 for NORMAL completion (and a
        // legitimate `return 0`); §22.13 distinguishes "Main returning normally"
        // from "Main returning by an uncaught exception that the runtime
        // intercepts". Emitting `ret i32 0` here (the old emitImplicitReturn for
        // an i32 function) conflated the two — every uncaught exception reported
        // success. Instead, hand off to the runtime's abnormal-termination
        // interceptor, which writes to stderr and exits non-zero. This fires
        // ONLY for main's no-handler boundary: inner frames and every
        // caught/handled path (unwindTarget_ set → not propagateBB) are
        // untouched, so finally + owned-local unwind disposal still run.
        if (currentFn_->getName() == "main") {
            // #1366 / §33.3: hand the in-flight exception (and the built-in
            // exception vtable table, so the runtime reads §31.4 fields only on
            // objects that have that layout) to the runtime for the report +
            // exit(70).
            llvm::Value* exc =
                builder_.CreateLoad(ptrTy(), emitExcSlot(), "uncaught.exc");
            auto* fn = declareC("bz_unhandled_exception",
                                llvm::Type::getVoidTy(llvm_),
                                {ptrTy(), ptrTy()});
            fn->setDoesNotReturn();
            builder_.CreateCall(fn, {exc, builtinExcVtableTablePtr()});
            builder_.CreateUnreachable();
        } else {
            // §22.16 (v0.203, #2053) Slice 4b: emitImplicitReturn is the
            // redirect-aware variant (it routes through the armed ensures
            // epilogue when one is armed). But here we're synthesizing the
            // per-function fallback block for the exception path — there is
            // NO body here, just a `ret` to unwind the frame. Forcing the
            // redirect here would route the exception path back into the
            // ensures epilogue and re-evaluate clauses that have already
            // fired (or, worse, loop the frame). Disable the redirect for
            // this one emission so the propagate block emits a clean `ret`.
            llvm::Function* savedEnsuresFn = ensuresFn_;
            llvm::BasicBlock* savedEnsuresExitBB = ensuresExitBB_;
            llvm::AllocaInst* savedEnsuresResultSlot = ensuresResultSlot_;
            const ast::Type* savedEnsuresRetNode = ensuresReturnTypeNode_;
            const std::vector<ast::ContractClause>* savedEnsuresClauses =
                ensuresClauses_;
            std::string savedEnsuresTypeName = std::move(ensuresTypeName_);
            std::string savedEnsuresMemberName = std::move(ensuresMemberName_);
            ensuresFn_ = nullptr;
            ensuresExitBB_ = nullptr;
            ensuresResultSlot_ = nullptr;
            ensuresReturnTypeNode_ = nullptr;
            ensuresClauses_ = nullptr;
            ensuresTypeName_.clear();
            ensuresMemberName_.clear();
            emitImplicitReturn(currentFn_);
            ensuresFn_ = savedEnsuresFn;
            ensuresExitBB_ = savedEnsuresExitBB;
            ensuresResultSlot_ = savedEnsuresResultSlot;
            ensuresReturnTypeNode_ = savedEnsuresRetNode;
            ensuresClauses_ = savedEnsuresClauses;
            ensuresTypeName_ = std::move(savedEnsuresTypeName);
            ensuresMemberName_ = std::move(savedEnsuresMemberName);
        }
        if (save) builder_.SetInsertPoint(save);
    }
    return propagateBB_;
}


llvm::BasicBlock* Emitter::unwindBB() {
    return unwindTarget_ ? unwindTarget_ : propagateBB();
}


void Emitter::emitUnwindDisposals() {
    // #383: the unwind path — a `caughtException` entry is skipped (the
    // exception stays in flight; the enclosing handler frees it).
    for (std::size_t i = disposeStack_.size(); i-- > unwindScopeDepth_;)
        emitDisposals(disposeStack_[i], /*isUnwind=*/true);
}


void Emitter::emitUnwindDisposalsInRange(std::size_t lo, std::size_t hi) {
    // #467 / §28.3 (lines 4914, 4922): dispose the scopes in disposeStack_
    // indices [lo, hi), innermost (higher index) first — the reverse-declaration,
    // inner-before-outer order the §28.3 exception-path discharge requires. Used
    // when a `try`-with-`finally` re-propagates an exception to its enclosing
    // handler after running its own finally: the throw site only disposed scopes
    // down to the inner try's depth (unwindScopeDepth_), so any scope that lies
    // BETWEEN the inner try and the enclosing handler (e.g. an outer `using var`
    // declared in the same block as, but textually before, the inner try) is
    // still live and must be disposed here — AFTER the explicit finally body, as
    // the scope unwinds further outward (§28.3: "the explicit finally runs first
    // ... then the implicit auto-dispose runs as the scope unwinds further").
    // All entries are disposed on the isUnwind path; caughtException entries are
    // skipped — they stay in flight for the enclosing handler.
    for (std::size_t i = hi; i-- > lo;)
        emitDisposals(disposeStack_[i], /*isUnwind=*/true);
}


void Emitter::emitPendingCheck() {
    llvm::Value* exc = builder_.CreateLoad(ptrTy(), emitExcSlot());
    llvm::Value* pending = builder_.CreateICmpNE(
        exc, llvm::ConstantPointerNull::get(ptrTy()));
    // fix #1468 / §28.3 "Exception-path discharge" / §28.4: inside a USER
    // destructor body that runs while an exception unwinds the stack, bz_exc
    // ALREADY holds the propagating exception when the body begins (§28.3: an
    // owned local/field live at the throw is disposed during unwinding). A
    // pending-check for an operation in the body — most commonly the §22.13
    // static-init guard emitted when the destructor touches a user static member
    // — must fire ONLY for a NEW exception that this operation itself threw, never
    // for the pre-existing propagating one. Firing on the pre-existing exception
    // branches straight to the enclosing unwind/return, abandoning the rest of
    // the destructor body, its owned-field auto-dispose (§28.4 step 2) and its
    // base chain — leaking the object and its whole owned subtree (the reported
    // unsound leak). So AND in "different from the exception captured at disposer
    // entry": the check fires iff in-flight AND new. When the entry value is null
    // (the normal, non-unwind disposal path — always the case for a destructor-
    // less type or a scope that exits without a throw) this reduces exactly to
    // the plain `exc != null`, so this path is behaviour-identical there. The
    // gate is emittingDisposerBody_, so ONLY user destructor-body emission is
    // affected — the general dispatch and every built-in/runtime disposer (e.g.
    // event-subscription and CancellationTokenSource cleanup) are untouched.
    if (emittingDisposerBody_ && disposerEntryExc_) {
        llvm::Value* isNew = builder_.CreateICmpNE(exc, disposerEntryExc_);
        pending = builder_.CreateAnd(pending, isNew);
    }
    emitPendingCheckIf(pending);
}

// #792 / §22.13: the shared route block for a conditional pending check. Branch
// to the unwind target (disposing unwound using-vars first, with the #1216
// supersede of a captured in-flight exception) iff `cond` is true, else
// continue. emitPendingCheck computes its condition (bz_exc != null, ANDed with
// the #1468 disposer "is-new" gate) and delegates here; emitEnsureStaticInit
// passes a "the static-init guard RAISED a new exception" condition so a
// pre-existing in-flight exception is not misrouted at the static-access site.
void Emitter::emitPendingCheckIf(llvm::Value* cond) {
    auto* unwind =
        llvm::BasicBlock::Create(llvm_, "call.unwind", currentFn_);
    auto* cont = llvm::BasicBlock::Create(llvm_, "call.cont", currentFn_);
    builder_.CreateCondBr(cond, unwind, cont);
    builder_.SetInsertPoint(unwind);
    // #1216 (sibling of #1149 / §31.2): if a call inside a finally body (or
    // catch arm) propagates an exception E2 via this pending-check, and there
    // is a captured in-flight exception E1 exposed as currentCaughtExc_ (set by
    // the #1149 / #1108 finally / catch-guard supersede fix), E1 is SUPERSEDED
    // by E2 and must be freed exactly once HERE — the #1149 emitThrowValue path
    // handles direct `throw` but is never reached on a call-propagation edge.
    // Guard mirrors emitThrowValue's #397/#866 logic: throwLeavesArm ensures we
    // only free when this unwind target is the arm/finally's own outer boundary
    // (a throw caught by an INNER try within the finally must not free E1 —
    // the finally continues and E1 stays in flight). No isDirectRethrow guard
    // is needed: a call-propagated exception (bz$exc set by the callee) is
    // never the same object as currentCaughtExc_; they are distinct heap objects.
    bool throwLeavesArm =
        caughtExcArmTarget_ && unwindBB() == caughtExcArmTarget_;
    if (currentCaughtExc_ && throwLeavesArm) {
        // §30.9 (v0.179): if E1 is a task-owned fault whose disposal was deferred
        // (its owning task was disposed while E1 was in flight, #1288), its
        // handling ends HERE via the supersede — the catch-arm's handling-
        // completion drain will not fire on this unwind edge — so clear the
        // deferred record before disposing to keep the single dispose here and
        // leave no stale pointer in the deferred set. A no-op for a handler-owned
        // E1 (the common #1216 case).
        builder_.CreateCall(
            declareC("bz_clear_exc_deferred", llvm::Type::getVoidTy(llvm_),
                     {ptrTy()}),
            {currentCaughtExc_});
        emitDisposeAndReclaim(currentCaughtExc_);
    }
    emitUnwindDisposals();
    builder_.CreateBr(unwindBB());
    builder_.SetInsertPoint(cont);
}


void Emitter::emitNewPendingCheck(llvm::Value* obj) {
    // §22.9 / §28.3 (FINDING [4]): same as emitPendingCheck, but on the unwind
    // edge ALSO reclaim the in-flight instance block `obj` whose constructor
    // threw. The ctor's own cleanup disposed the owned fields it had assigned and
    // the base subobject (it does not free `this`); freeing the block here — the
    // allocating site — reclaims it exactly once. This branch is taken only when
    // the ctor set the pending exception, so the free never reaches a normally-
    // constructed object (which is reclaimed later by its owning slot).
    llvm::Value* exc = builder_.CreateLoad(ptrTy(), emitExcSlot());
    llvm::Value* pending = builder_.CreateICmpNE(
        exc, llvm::ConstantPointerNull::get(ptrTy()));
    auto* unwind =
        llvm::BasicBlock::Create(llvm_, "new.unwind", currentFn_);
    auto* cont = llvm::BasicBlock::Create(llvm_, "new.cont", currentFn_);
    builder_.CreateCondBr(pending, unwind, cont);
    builder_.SetInsertPoint(unwind);
    builder_.CreateCall(freeFn(), {obj});  // reclaim the instance block once
    // #1216: same as emitPendingCheck — if a constructor inside a finally body
    // (or catch arm) throws and supersedes E1, free E1 exactly once here.
    // See emitPendingCheck for the full rationale and double-free guards.
    bool throwLeavesArm =
        caughtExcArmTarget_ && unwindBB() == caughtExcArmTarget_;
    if (currentCaughtExc_ && throwLeavesArm) {
        // §30.9 (v0.179): see emitPendingCheck — clear any deferred-dispose
        // record for a superseded task-owned fault before its single dispose.
        builder_.CreateCall(
            declareC("bz_clear_exc_deferred", llvm::Type::getVoidTy(llvm_),
                     {ptrTy()}),
            {currentCaughtExc_});
        emitDisposeAndReclaim(currentCaughtExc_);
    }
    emitUnwindDisposals();
    builder_.CreateBr(unwindBB());
    builder_.SetInsertPoint(cont);
}


llvm::BasicBlock* Emitter::newArgUnwindCleanup(llvm::Value* obj) {
    // §28.3 (#1968): build the unwind cleanup for an in-flight `new T(...)` whose
    // CONSTRUCTOR ARGUMENT throws DURING evaluation — before the constructor
    // runs. `obj` is calloc'd but uninitialized (every field zero, the ctor never
    // ran) and bound to no scope, so the ordinary unwind disposal never reclaims
    // it. RAW-free it here (a plain `free`, NOT a dispose — no destructor / owned-
    // field disposal, because no field is initialized), then continue to the
    // enclosing unwind exactly as the arg-throw would have without this cleanup.
    // Capturing `unwindBB()` BEFORE the caller overrides `unwindTarget_` preserves
    // the enclosing catch/propagate boundary; the pending-check at the arg site
    // has already run emitUnwindDisposals() for the enclosing scopes (this cleanup
    // does not repeat them), so a branch here simply adds the missing `obj` free.
    auto* savedInsert = builder_.GetInsertBlock();
    llvm::BasicBlock* enclosingUnwind = unwindBB();
    auto* cleanupBB =
        llvm::BasicBlock::Create(llvm_, "new.argthrow.cleanup", currentFn_);
    builder_.SetInsertPoint(cleanupBB);
    builder_.CreateCall(freeFn(), {obj});  // reclaim the uninitialized block once
    builder_.CreateBr(enclosingUnwind);
    if (savedInsert) builder_.SetInsertPoint(savedInsert);
    return cleanupBB;
}


llvm::BasicBlock* Emitter::closureArgUnwindCleanup(
    const std::vector<llvm::Value*>& closures) {
    // fix #1073 / §28.3 "Exception-path discharge" / §28.9: the EXCEPTION-edge
    // half of the §28.14 call-bounded closure ARGUMENT-PASS release
    // (LlvmBackend_Calls2.cpp). The producer's +1 on an inline-closure argument
    // is dropped after the call on the normal edge; when the callee (or an
    // operation emitted inside emitMethodCall, e.g. a static-init guard)
    // PROPAGATES instead, `emitPendingCheck` branches away before that release
    // and the cell + env would leak on the caught/propagate path. Install this
    // block as `unwindTarget_` for the duration of emitMethodCall so every
    // unwind edge it emits drops those same temporaries on the way out.
    //
    // EXACTLY-ONCE. The block's only predecessors are the unwind edges emitted
    // WHILE it is installed; each of them is a `CreateCondBr` whose other side
    // is the `call.cont` continuation carrying the normal-path release. The two
    // are therefore mutually exclusive on any concrete execution: an exception
    // edge branches out of the expression for good (it ends in a branch to the
    // enclosing catch/propagate boundary and never rejoins `cont`), and the
    // normal edge is only reachable when no pending exception fired. Nested
    // `try` lowering inside an inlined callee body saves/restores
    // `unwindTarget_` around its own catch dispatch, so a HANDLED throw resumes
    // on the normal edge and reaches the post-call release exactly as it would
    // without this block.
    //
    // Capturing `unwindBB()` BEFORE the caller overrides `unwindTarget_`
    // preserves the enclosing catch/propagate boundary; the pending check has
    // already run emitUnwindDisposals() for the enclosing scopes, so this
    // cleanup only adds the missing closure releases (mirrors
    // newArgUnwindCleanup above). // per §28.14, §28.3, §28.9
    auto* savedInsert = builder_.GetInsertBlock();
    llvm::BasicBlock* enclosingUnwind = unwindBB();
    auto* cleanupBB =
        llvm::BasicBlock::Create(llvm_, "clarg.throw.cleanup", currentFn_);
    builder_.SetInsertPoint(cleanupBB);
    for (llvm::Value* cl : closures) emitClosureRelease(cl);  // null-safe
    builder_.CreateBr(enclosingUnwind);
    if (savedInsert) builder_.SetInsertPoint(savedInsert);
    return cleanupBB;
}


void Emitter::emitDynamicDispose(llvm::Value* obj) {
    // §28.2 / §28.4: dispatch the object's PUBLIC, idempotent Dispose through
    // its vtable (slot 2, CG35-itable) — which runs the destructor body, owned-field
    // disposals and the full base chain, AT MOST ONCE per object (the run-once
    // flag, §28.6). This does NOT free the object's heap block.
    //
    // #380 / §28: "dispose" still means run the destructor AND reclaim the block
    // (no GC). But the free is now performed by the disposal SITE (see
    // emitDisposeAndReclaim), not here. Reason: explicit `.Dispose()` (§28.5
    // line 5066) plus the scope-end / owned-field auto-dispose reach Dispose
    // TWICE on one object; if the first dispatch freed the block, the second
    // dispatch's vtable load would read freed memory (use-after-free) regardless
    // of any flag. Splitting "dispatch" (idempotent, repeatable) from "reclaim"
    // (done once, by the owning slot's auto-dispose) keeps repeated disposal
    // safe. vtable layout (CG35-itable + §8.6/§8.5 #417): [0]=td$, [1]=itmap$,
    // [2]=Dispose, [3]=ToString, [4+]=virtuals. Dispose stays at the FIXED slot 2.
    llvm::Value* vptr = builder_.CreateLoad(ptrTy(), obj);
    llvm::Value* slot = builder_.CreateGEP(ptrTy(), vptr,
                                           {builder_.getInt32(2)});
    llvm::Value* disposeFn = builder_.CreateLoad(ptrTy(), slot);
    auto* fnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_),
                                         {ptrTy()}, /*isVarArg=*/false);
    builder_.CreateCall(fnTy, disposeFn, {obj});
}


void Emitter::emitDisposeAndReclaim(llvm::Value* obj) {
    // #380 / §28: an auto-dispose site that OWNS the slot/field holding `obj`
    // (using-var scope-end, owned-field auto-dispose, exception unwind, the
    // caught-exception normal-exit free). It is the LAST dispose of this object
    // — sema guarantees no use-after-dispose, and any earlier explicit
    // `.Dispose()` only dispatched (it did not free). So dispatch the idempotent
    // Dispose (a no-op if already run) and THEN reclaim the block exactly once.
    // The caller is responsible for null-ing its slot afterward where a second
    // dispatch through the same slot is reachable. free(null) is a no-op.
    emitDynamicDispose(obj);
    // §8.3 / #2058: reap the side-table entry BEFORE the heap free (the object
    // is still live here, so the erase cannot read freed memory). Bounds the
    // identity-hash side-table for long-running services; the per-domain
    // deterministic counter is untouched, so §30.13 cross-run determinism
    // holds. Fires in BOTH release and §28.2 checked-disposal modes — a
    // quarantined disposed instance is logically dead (member access is
    // DisposedAccessException, §31.4), so its identity hash need not be
    // preserved. A never-hashed object (the common case) is a cheap mutex
    // lock + failed find inside the runtime helper.
    builder_.CreateCall(
        declareC("bz_obj_identity_hash_forget",
                 llvm::Type::getVoidTy(llvm_), {ptrTy()}),
        {obj});
    // §31.4/#811/#1389: reclaim the CauseChain snapshot HERE, at the terminal
    // reclaim site, immediately before the block itself is freed — NOT inside
    // the shared vtable Dispose (bz$exDisposeMsg, setupBuiltinExceptions). The
    // shared disposer releases Message/ParamName this same release-and-null
    // way, and freeing CauseChain there looks like the same idiom, but it is
    // NOT safe there: a still-borrowed (not-yet-reclaimed) exception's
    // CauseChain can be READ after ITS Dispose has already dispatched on some
    // path (e.g. `throw new X(msg, ex)` inside `ex`'s own catch arm reads
    // `ex.CauseChain` — bz_exc_causechain_link's `nested` argument — to build
    // the new snapshot). Freeing CauseChain from the shared disposer produced
    // a use-after-free there (diagnosed via gdb: bz_exc_causechain_link
    // reading a `nested` pointer into an already-freed/reused block) across
    // the several non-single-site exception disposal paths (catch-arm
    // dispose, scope-end dispose, async fault propagation). THIS site is
    // different: by ownership discipline it runs EXACTLY ONCE per object,
    // strictly after every live borrow/read of it has concluded (the true end
    // of the object's life) — it cannot race a still-in-flight CauseChain
    // read, so it is safe to free here.
    //
    // Gate: `bz_exc_reclaim_causechain` checks obj's vtable against the same
    // built-in-exception vtable table bz_unhandled_exception gates on (only a
    // built-in exception type installs bz$exDisposeMsg / carries the
    // CauseChain slot at BZ_EXC_CAUSE_OFFSET); a no-op for every other class.
    //
    // Scope: this covers every reclaim that funnels through this function —
    // catch-arm dispose, using-var / owned-field scope-end dispose, and a
    // faulted task's scope-end fault-exception dispose. It deliberately does
    // NOT cover the handful of pure-runtime "vtable-Dispose-then-free" sites
    // that bypass codegen entirely (bz_dispose_class_obj —
    // Task.FireAndForget's abandoned-task continuation and the
    // WhenAll/WhenAllOwned aggregate fault/result cleanup — and
    // bz_domain_spawn's DomainFaultedException wrap-and-dispose of the
    // original fault): those run in bezel_runtime_concurrency.cpp with no
    // access to the codegen-built vtable table this gate needs. Filed as a
    // follow-up (see the issue tracker) rather than fixed silently here.
    builder_.CreateCall(
        declareC("bz_exc_reclaim_causechain", llvm::Type::getVoidTy(llvm_),
                 {ptrTy(), ptrTy()}),
        {obj, builtinExcVtableTablePtr()});
    // §28.2 v0.134 / issue #601: in checked disposal mode the block is
    // quarantined (not freed) after disposal so that the per-instance disposed
    // flag at slot 1 (or 2) remains readable when a subsequent member-entry
    // guard loads it. This is a deliberate memory leak in checked (debug)
    // builds; the spec explicitly endorses deferred reclamation here. In
    // unchecked (release) mode the block is freed immediately as before.
    // (The CauseChain reclaim above runs unconditionally, ahead of this
    // branch, matching checked disposal's own deliberate-leak posture for
    // everything else the block owns — only the block header itself is kept
    // readable for the disposed-flag guard.)
    if (!checkedDisposal_)
        builder_.CreateCall(freeFn(), {obj});
}

// #1833 (b) / #1467 / §22.4 / §28.3: dispose every `out _` discard-owned
// scratch slot recorded by buildOrderedArgs in the `[base, end)` range,
// on the normal-return path (`!diverged_`). Byte-identical pattern to
// lowerCall's end-of-statement disposal pass — extracted here so lowerNew
// can drive its own recorded slots the same way. On the divergent path
// (the ctor threw) no scratch is written, so no slot reaches a non-null
// value: the unwind reclaims the heap block via the enclosing obj
// (emitNewPendingCheck / the inlineCtorBody cleanup), not through these
// scratch slots, so skipping them on `diverged_` is sound. The
// `resize(base)` trims the stack back so a NESTED discard inside an arg
// builds its own scope (the lowerCall-side discipline).
void Emitter::disposeNewDiscardOwnedOutSlots(std::size_t base) {
    if (diverged_) {
        discardOwnedOutSlots_.resize(base);
        return;
    }
    for (std::size_t di = base; di < discardOwnedOutSlots_.size(); ++di) {
        llvm::AllocaInst* slot = discardOwnedOutSlots_[di];
        llvm::Value* obj = builder_.CreateLoad(ptrTy(), slot, "new.outdisc.obj");
        llvm::Value* isNull = builder_.CreateICmpEQ(
            obj, llvm::ConstantPointerNull::get(ptrTy()));
        auto* dispBB = llvm::BasicBlock::Create(
            llvm_, "new.outdisc.dispose", currentFn_);
        auto* contBB = llvm::BasicBlock::Create(
            llvm_, "new.outdisc.cont", currentFn_);
        builder_.CreateCondBr(isNull, contBB, dispBB);
        builder_.SetInsertPoint(dispBB);
        emitDisposeAndReclaim(obj);
        builder_.CreateStore(
            llvm::ConstantPointerNull::get(ptrTy()), slot);
        builder_.CreateBr(contBB);
        builder_.SetInsertPoint(contBB);
    }
    discardOwnedOutSlots_.resize(base);
}


void Emitter::emitOwningChoiceActiveCaseDisposal(
        llvm::AllocaInst* slot, const sema::TypeSymbol* choiceTy,
        const sema::TypeRef* choiceRef) {
    // §35.4 (v0.200, #2045 stage 3) "Disposal is defined by the active
    // case." Reads the runtime TAG (field 0 of the choice struct), switches
    // on it, and for each case whose payload has at least one `owned`-marked
    // field, GEPs into that case's payload slot (field 1+caseIndex), extracts
    // each owned payload pointer, null-guards, and dispatches Dispose+reclaim
    // via the runtime vtable — the concrete class is the runtime payload
    // type, statically unknown at the choice site, so dynamic dispatch
    // (emitDisposeAndReclaim, which loads the vtable slot 2 and reclaims the
    // block) is the only safe site. Payload-free / value-payload cases
    // dispose nothing, so their switch arm is a direct branch to the join.
    //
    // WHY DYNAMIC DISPATCH (§35.4 + §8.6): the choice's payload type is the
    // STATIC field type — a class, an interface, or a generic parameter
    // (§28.1's per-instantiation erasure of `owned`). The runtime TYPE of
    // the payload is the concrete class the user stored (a subclass of the
    // declared type, or an interface's concrete implementation). The vtable
    // slot 2 holds the most-derived Dispose; emitDisposeAndReclaim loads it
    // and reclaims the block. This is the SAME path class-field auto-
    // disposal uses (§28.4 / CG18) — exactly the right invariant.
    //
    // WHY NO NULL-AFTER (yet): unlike a class field, the choice struct is a
    // value-typed slot on the stack. After dispose, the slot is dead (its
    // payloads are gone); a re-entrant dispose through the same slot would
    // read the same tag and re-dispatch on the (now-disposed) owned field,
    // which is freed memory. emitDisposals fires this at scope-end on a
    // single exit edge, so a second pass through the same Disposal entry is
    // structurally impossible (the slot goes out of scope). The null-after
    // pattern would matter for a heap-resident choice or for a nested
    // owning match (which is the v0.200 stage 4 follow-up) — keep that
    // for the future; the current invariant (single fire, fresh tag)
    // means the dispose is exactly-once.
    if (!slot || !choiceTy) return;
    // §14.1 / §28.1 (#2047): a generic owning choice (`Result<C, string>`) is
    // disposed from a NON-generic body (Main), so no substitution is active and
    // choiceStruct/lowerTypeRef would otherwise yield the i32-placeholder layout
    // (a tag/payload type mismatch → an ICmp assertion). Install the
    // instantiation's substitution so the specialized payload struct is used and
    // each owned field's instantiated type is decidable for §28.1 erasure.
    RecordSubstGuard choiceSubst(this, choiceRef);
    const auto* choiceDecl =
        dynamic_cast<const ast::TypeDecl*>(choiceTy->decl);
    auto cases = choiceCasesOf(choiceTy);
    if (cases.empty()) return;
    // The choice struct is { i32 tag, case0_payload, case1_payload, … }
    // (see choiceStruct). The tag is field 0 (i32); the i-th case's payload
    // struct is field 1+i.
    llvm::StructType* st = choiceStruct(choiceTy);
    if (!st) return;
    llvm::Value* choiceVal = builder_.CreateLoad(st, slot, "choice.val");
    llvm::Value* tag = builder_.CreateExtractValue(
        choiceVal, {0}, "choice.tag");
    auto* joinBB =
        llvm::BasicBlock::Create(llvm_, "ocd.join", currentFn_);
    auto* switchBB =
        llvm::BasicBlock::Create(llvm_, "ocd.switch", currentFn_);
    builder_.CreateBr(switchBB);
    builder_.SetInsertPoint(switchBB);
    auto* sw = builder_.CreateSwitch(
        tag, joinBB, static_cast<unsigned>(cases.size()));
    // Walk every case in declaration order, building one switch arm per
    // case that has at least one owned payload field.
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const sema::ChoiceCaseSymbol* cc = cases[i];
        if (!cc || !cc->caseDecl) continue;
        // §28.1 + §35.4: only `owned`-marked payload fields are owning slots;
        // plain (unmarked) class payloads are borrows (the borrow-bearing
        // classification), value payloads are value-typed (no dispose), and
        // strings are runtime-managed. Walk the AST payload list, collect
        // indices of owned fields, and emit one dispose site per owned
        // field, in payload declaration order (§35.4 "in payload
        // declaration order").
        std::vector<unsigned> ownedFieldIdxs;
        for (std::size_t j = 0; j < cc->caseDecl->payload.size(); ++j) {
            const ast::Param& pf = cc->caseDecl->payload[j];
            if (pf.modifier != "owned") continue;
            // §28.1 per-instantiation erasure (#2047): an `owned` field whose
            // instantiated type is value-like or runtime-managed (e.g.
            // `Error(owned error: string)` at `Result<C, string>`) does NOT
            // participate in vtable disposal — disposing it would dispatch a
            // Dispose through a non-object payload. Only fields whose
            // instantiated type is an owning heap reference are disposed.
            const sema::TypeRef* ftInst = common::substitutedChoicePayloadType(
                pf, choiceDecl, choiceRef, ctx_);
            if (!common::ownedMarkerActiveOnInstantiatedType(ftInst, ctx_))
                continue;
            ownedFieldIdxs.push_back(static_cast<unsigned>(j));
        }
        if (ownedFieldIdxs.empty()) continue;  // nothing to dispose — switch
                                               // arm collapses to join.
        auto* armBB =
            llvm::BasicBlock::Create(llvm_, "ocd.arm", currentFn_);
        sw->addCase(
            llvm::ConstantInt::get(llvm::Type::getInt32Ty(llvm_), i), armBB);
        builder_.SetInsertPoint(armBB);
        // GEP into the case's payload slot (field 1+i) and load the
        // payload struct value, then extract each owned field and dispose.
        llvm::Value* casePayload = builder_.CreateExtractValue(
            choiceVal, {1 + (unsigned)i}, "ocd.case.payload");
        for (unsigned j : ownedFieldIdxs) {
            llvm::Value* ownedPtr = builder_.CreateExtractValue(
                casePayload, {j}, "ocd.owned.field");
            // Null-guard: a never-bound arm of the active-case match could
            // leave an `owned`-payload slot null (the binding took it out);
            // or a payload-defaulted construction. `emitDisposeAndReclaim`
            // is null-safe (free(null) no-op) but the dispatch on a null
            // vtable would crash — guard explicitly.
            llvm::Value* isNull = builder_.CreateICmpEQ(
                ownedPtr, llvm::ConstantPointerNull::get(ptrTy()));
            auto* dispBB = llvm::BasicBlock::Create(
                llvm_, "ocd.dispose", currentFn_);
            auto* contBB = llvm::BasicBlock::Create(
                llvm_, "ocd.cont", currentFn_);
            builder_.CreateCondBr(isNull, contBB, dispBB);
            builder_.SetInsertPoint(dispBB);
            emitDisposeAndReclaim(ownedPtr);
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
        }
        builder_.CreateBr(joinBB);
    }
    builder_.SetInsertPoint(joinBB);
}


void Emitter::emitValueLikeChoiceActiveCaseStrRelease(
        llvm::AllocaInst* slot, const sema::TypeSymbol* choiceTy,
        const sema::TypeRef* choiceRef) {
    // fix #2115 / §28.9 / §35.4: release the ACTIVE case's runtime-managed
    // (`string`) payload(s) of a VALUE-LIKE choice struct at scope end. The
    // choice struct is { i32 tag, case0_payload, case1_payload, … } (see
    // choiceStruct); a string payload field is a `ptr` slot holding a +1
    // `bz_string*` stored by the §35.3 construction path with no retain/release
    // tracking. This emit reads the runtime TAG (field 0), switches on it, and
    // for the active case releases each string-typed payload field via
    // emitStringRelease — the value-like analog of
    // emitOwningChoiceActiveCaseDisposal (which disposes `owned` heap payloads
    // via the vtable). bz_string_release is null-safe (a never-bound string?
    // slot) and a no-op on a sentinel/literal, so an arm whose string payload
    // was a literal releases nothing. Payload-free / non-string-payload cases
    // release nothing, so their switch arm collapses to the join.
    //
    // SOUNDNESS: the registration (armValueLikeChoiceStrRelease) guarantees the
    // local is non-escaping (StringEscapeWalker) AND the initializer is a choice
    // construction every string payload arg of which is a +1 producer or a
    // literal — so the slot provably owns exactly +1 on the active case's
    // strings, and no copy/alias/release-site exists to double-free. Fires on a
    // SINGLE exit edge (the slot goes out of scope), so like the owning-choice
    // active-case disposal the release is exactly-once per exit. A reassigned
    // local is excluded at registration (no release-old-on-reassign machinery —
    // reassignment is a conservative escape), so the active case here is always
    // the constructed case and its string slots are the +1 the walk balances.
    // // per §28.9 (string refcount discipline), §35.4 v0.226.
    if (!slot || !choiceTy) return;
    // §14.1 / §28.1 (#2047): a generic value-like choice (`Result<int, string>`)
    // is released from a non-generic body, so install the instantiation's
    // substitution → the specialized payload struct (a tag/payload type mismatch
    // would otherwise fail an ICmp assertion). Same guard the owning-choice emit
    // and choiceStruct consult.
    RecordSubstGuard choiceSubst(this, choiceRef);
    const auto* choiceDecl =
        dynamic_cast<const ast::TypeDecl*>(choiceTy->decl);
    auto cases = choiceCasesOf(choiceTy);
    if (cases.empty()) return;
    llvm::StructType* st = choiceStruct(choiceTy);
    if (!st) return;
    // Only emit the switch if at least one case has a string payload field;
    // otherwise every arm collapses to the join and there is nothing to release.
    auto caseHasStringPayload = [&](const sema::ChoiceCaseSymbol* cc) -> bool {
        if (!cc || !cc->caseDecl) return false;
        for (std::size_t j = 0; j < cc->caseDecl->payload.size(); ++j) {
            const ast::Param& pf = cc->caseDecl->payload[j];
            if (!pf.type) continue;
            const sema::TypeRef* ftInst = common::substitutedChoicePayloadType(
                pf, choiceDecl, choiceRef, ctx_);
            if (ftInst &&
                ftInst->kind == sema::TypeRef::Kind::Primitive &&
                ftInst->primitive == sema::PrimitiveType::String)
                return true;
        }
        return false;
    };
    bool any = false;
    for (auto* cc : cases)
        if (caseHasStringPayload(cc)) { any = true; break; }
    if (!any) return;
    llvm::Value* choiceVal = builder_.CreateLoad(st, slot, "vlchoice.val");
    llvm::Value* tag = builder_.CreateExtractValue(choiceVal, {0}, "vlchoice.tag");
    auto* joinBB =
        llvm::BasicBlock::Create(llvm_, "vlcsr.join", currentFn_);
    auto* switchBB =
        llvm::BasicBlock::Create(llvm_, "vlcsr.switch", currentFn_);
    builder_.CreateBr(switchBB);
    builder_.SetInsertPoint(switchBB);
    auto* sw = builder_.CreateSwitch(
        tag, joinBB, static_cast<unsigned>(cases.size()));
    // Walk every case in declaration order; one switch arm per case that has at
    // least one string payload field. The string payload slot is released in
    // payload declaration order (§35.4 "in payload declaration order" — the same
    // ordering the owning-choice active-case disposal uses).
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const sema::ChoiceCaseSymbol* cc = cases[i];
        if (!cc || !cc->caseDecl) continue;
        // Collect indices of this case's STRING payload fields (instantiated
        // type). Non-string fields are ignored — only runtime-managed string
        // slots carry the +1 the construction mints.
        std::vector<unsigned> strFieldIdxs;
        for (std::size_t j = 0; j < cc->caseDecl->payload.size(); ++j) {
            const ast::Param& pf = cc->caseDecl->payload[j];
            if (!pf.type) continue;
            const sema::TypeRef* ftInst = common::substitutedChoicePayloadType(
                pf, choiceDecl, choiceRef, ctx_);
            if (ftInst &&
                ftInst->kind == sema::TypeRef::Kind::Primitive &&
                ftInst->primitive == sema::PrimitiveType::String) {
                strFieldIdxs.push_back(static_cast<unsigned>(j));
            }
        }
        if (strFieldIdxs.empty()) continue;  // nothing to release — arm
                                             // collapses to join.
        auto* armBB =
            llvm::BasicBlock::Create(llvm_, "vlcsr.arm", currentFn_);
        sw->addCase(llvm::ConstantInt::get(llvm::Type::getInt32Ty(llvm_), i),
                    armBB);
        builder_.SetInsertPoint(armBB);
        // GEP into the case's payload slot (field 1+i) and extract each string
        // field. The payload slot is field 1+i of the choice struct; field j of
        // that payload is the string pointer.
        llvm::Value* casePayload = builder_.CreateExtractValue(
            choiceVal, {1 + (unsigned)i}, "vlcsr.case.payload");
        for (unsigned j : strFieldIdxs) {
            llvm::Value* strPtr = builder_.CreateExtractValue(
                casePayload, {j}, "vlcsr.str.field");
            // bz_string_release is null-safe (a never-bound string? slot) and a
            // no-op on a sentinel/literal payload — release unconditionally.
            emitStringRelease(strPtr);
        }
        builder_.CreateBr(joinBB);
    }
    builder_.SetInsertPoint(joinBB);
}


// fix #2425 / §9.8 / §28.9 / §35.4 v0.235: does value-like choice `ts` own,
// directly or transitively (through a non-nullable nested-record or
// nested-choice payload field), at least one `string`? See the declaration
// in LlvmBackend_impl.hpp for the full termination argument (§35.1's choice
// self-containment check makes the record+choice value-type graph a finite
// DAG). GENERIC choices are excluded (returns false unconditionally): a
// generic choice's payload struct layout is per-instantiation
// (choiceStruct's #2047 specialization), and neither this classifier nor
// emitChoiceStringFieldReleases_impl below installs a RecordSubstGuard for
// a NESTED generic choice/record reached mid-recursion — the same narrow,
// documented scope #2405's record recursion already has (it does not
// thread generic substitution through nested levels either). A generic
// value-like choice component/payload is therefore a residual gap, not a
// new one this fix introduces — filed if found in a live shape (none of
// this issue's own repros are generic).
bool Emitter::choiceHasStringFields(const sema::TypeSymbol* ts) {
    if (!ts || ts->kind != ast::TypeDeclKind::Choice) return false;
    if (!ts->genericParams.empty()) return false;
    for (const sema::ChoiceCaseSymbol* cc : choiceCasesOf(ts)) {
        if (!cc || !cc->caseDecl) continue;
        for (const ast::Param& pf : cc->caseDecl->payload) {
            if (!pf.type) continue;
            const sema::TypeRef* ft = ctx_.resolvedType(*pf.type);
            if (!ft) continue;
            if (ft->kind == sema::TypeRef::Kind::Primitive &&
                ft->primitive == sema::PrimitiveType::String)
                return true;
            if (ft->nullable || ft->kind != sema::TypeRef::Kind::Named ||
                !ft->declaration)
                continue;
            if (ft->declaration->kind == ast::TypeDeclKind::Record &&
                recordHasStringFields(ft->declaration))
                return true;
            if (ft->declaration->kind == ast::TypeDeclKind::Choice &&
                choiceHasStringFields(ft->declaration))
                return true;
        }
    }
    return false;
}


// fix #2425 / §9.8 / §28.3 / §28.9 / §35.4 v0.235: release the ACTIVE case's
// `string`-owning payload(s) of value-like choice `choiceTy` at struct
// ADDRESS `choiceAddr`, recursing into a non-nullable nested-record or
// nested-choice payload field to ARBITRARY depth. This is the choice-side
// counterpart to emitRecordStringFieldReleases_impl's #2405 arbitrary-depth
// recursion, and the address-based (GEP, not load+extractvalue) analog of
// #2115's emitValueLikeChoiceActiveCaseStrRelease above — that function is
// VALUE-based (loads the whole struct once, ExtractValue per field) and
// stops at ONE level (direct string payload fields only); this one is
// ADDRESS-based so a nested record/choice payload's OWN storage can be
// reached by a further GEP off the same base, exactly as
// emitRecordStringFieldReleases_impl reaches a nested record. The two
// functions are NOT merged: this one is used only by the #2398/#2425
// deserialize disposal-scope registration (LlvmBackend_Serialization.cpp),
// which already holds an ADDRESS (an alloca'd component/payload slot); the
// #2115 mechanism is used by the scope-end LOCAL release
// (LlvmBackend_Setup.cpp), which arms off a VALUE-level escape analysis
// unrelated to this one. See choiceHasStringFields just above for the
// termination argument and the (documented, narrow) generic exclusion this
// function shares via that gate.
//
// A NULLABLE nested-record/nested-choice payload field is excluded, matching
// emitRecordStringFieldReleases_impl's own exclusion: it lowers to a
// `{i8, T}` wrapper struct, not the bare layout this walker assumes.
void Emitter::emitChoiceStringFieldReleases_impl(
        llvm::Value* choiceAddr, const sema::TypeSymbol* choiceTy) {
    if (!choiceAddr || !choiceTy || choiceTy->kind != ast::TypeDeclKind::Choice)
        return;
    if (!choiceHasStringFields(choiceTy)) return;  // also excludes generics
    auto cases = choiceCasesOf(choiceTy);
    if (cases.empty()) return;
    llvm::StructType* st = choiceStruct(choiceTy);
    if (!st) return;
    llvm::Value* tagAddr =
        builder_.CreateStructGEP(st, choiceAddr, 0, "chsfr.tag.addr");
    llvm::Value* tag = builder_.CreateLoad(i32(), tagAddr, "chsfr.tag");
    auto* joinBB = llvm::BasicBlock::Create(llvm_, "chsfr.join", currentFn_);
    auto* switchBB = llvm::BasicBlock::Create(llvm_, "chsfr.switch", currentFn_);
    builder_.CreateBr(switchBB);
    builder_.SetInsertPoint(switchBB);
    auto* sw = builder_.CreateSwitch(
        tag, joinBB, static_cast<unsigned>(cases.size()));
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const sema::ChoiceCaseSymbol* cc = cases[i];
        if (!cc || !cc->caseDecl) continue;
        // Does THIS case own a string, directly or transitively? Skip the
        // arm entirely (collapses to the join) if not — mirrors #2115's own
        // per-case caseHasStringPayload guard.
        bool caseOwnsString = false;
        for (const ast::Param& pf : cc->caseDecl->payload) {
            if (!pf.type) continue;
            const sema::TypeRef* ft = ctx_.resolvedType(*pf.type);
            if (!ft) continue;
            if (ft->kind == sema::TypeRef::Kind::Primitive &&
                ft->primitive == sema::PrimitiveType::String) {
                caseOwnsString = true;
                break;
            }
            if (ft->nullable || ft->kind != sema::TypeRef::Kind::Named ||
                !ft->declaration)
                continue;
            if ((ft->declaration->kind == ast::TypeDeclKind::Record &&
                 recordHasStringFields(ft->declaration)) ||
                (ft->declaration->kind == ast::TypeDeclKind::Choice &&
                 choiceHasStringFields(ft->declaration))) {
                caseOwnsString = true;
                break;
            }
        }
        if (!caseOwnsString) continue;
        auto* armBB = llvm::BasicBlock::Create(llvm_, "chsfr.arm", currentFn_);
        sw->addCase(llvm::ConstantInt::get(llvm::Type::getInt32Ty(llvm_), i),
                    armBB);
        builder_.SetInsertPoint(armBB);
        // GEP into the case's payload slot (field 1+i) — embedded inline in
        // the SAME outer struct, exactly like a record's nested-record
        // member (§35.1: a choice's case payloads are value-typed storage).
        llvm::Value* payloadAddr = builder_.CreateStructGEP(
            st, choiceAddr, 1 + (unsigned)i, "chsfr.case.addr");
        llvm::Type* payloadTy = st->getElementType(1 + (unsigned)i);
        auto* payloadStruct = llvm::dyn_cast<llvm::StructType>(payloadTy);
        for (std::size_t j = 0; j < cc->caseDecl->payload.size(); ++j) {
            const ast::Param& pf = cc->caseDecl->payload[j];
            if (!pf.type) continue;
            const sema::TypeRef* ft = ctx_.resolvedType(*pf.type);
            if (!ft || !payloadStruct || j >= payloadStruct->getNumElements())
                continue;
            if (ft->kind == sema::TypeRef::Kind::Primitive &&
                ft->primitive == sema::PrimitiveType::String) {
                llvm::Value* fAddr = builder_.CreateStructGEP(
                    payloadStruct, payloadAddr, (unsigned)j, "chsfr.str.addr");
                llvm::Value* fVal =
                    builder_.CreateLoad(ptrTy(), fAddr, "chsfr.str.val");
                emitStringRelease(fVal);
                builder_.CreateStore(
                    llvm::ConstantPointerNull::get(ptrTy()), fAddr);
                continue;
            }
            if (ft->nullable || ft->kind != sema::TypeRef::Kind::Named ||
                !ft->declaration)
                continue;
            if (ft->declaration->kind == ast::TypeDeclKind::Record &&
                recordHasStringFields(ft->declaration)) {
                llvm::Value* nestedAddr = builder_.CreateStructGEP(
                    payloadStruct, payloadAddr, (unsigned)j,
                    "chsfr.nested.rec.addr");
                emitRecordStringFieldReleases_impl(nestedAddr, ft->declaration);
                continue;
            }
            if (ft->declaration->kind == ast::TypeDeclKind::Choice &&
                choiceHasStringFields(ft->declaration)) {
                llvm::Value* nestedAddr = builder_.CreateStructGEP(
                    payloadStruct, payloadAddr, (unsigned)j,
                    "chsfr.nested.ch.addr");
                emitChoiceStringFieldReleases_impl(nestedAddr, ft->declaration);
            }
        }
        builder_.CreateBr(joinBB);
    }
    builder_.SetInsertPoint(joinBB);
}


// fix #2326 / §9.8 v0.228: does the (statically-resolved) element type `tr`
// of a non-nullable inline array carry a string — directly (a `string`
// element), or transitively (through a non-nullable nested record / choice /
// inline-array element)? Mirrors the per-field arm of recordHasStringFields
// (LlvmBackend_impl.hpp) in shape: walk the element type one layer, and
// recurse into the named record / choice / inline-array cases. The
// element-type recursion is finite for the same reason as the record /
// choice walks: sema rejects a self-containing value-type graph at the
// declaration site (recordValueContainsSelf / checkChoiceSelfContainment
// in TypeResolution_Checks.cpp), so the reachable type graph is a finite
// DAG. A nullable element type is treated as string-free: a `string?` field
// in a record lowers to a {i8, ptr} wrapper that the existing nullable
// exclusion already handles, and the inline-array shape carries no parallel
// obligation in the spec.
//
// Generic named element types are excluded by their own gate (declaration
// null at this layer) — the same narrow, documented scope
// choiceHasStringFields above has, mirroring #2405's record recursion. A
// generic value-type element is therefore a residual gap, not a new one
// this fix introduces; the live repros of this issue are all concrete.
bool Emitter::inlineArrayElementCarriesString(const sema::TypeRef* tr) {
    if (!tr || tr->nullable) return false;
    using K = sema::TypeRef::Kind;
    if (tr->kind == K::Primitive)
        return tr->primitive == sema::PrimitiveType::String;
    if (tr->kind == K::InlineArray)
        return tr->arrayElement &&
               inlineArrayElementCarriesString(tr->arrayElement);
    if (tr->kind == K::Named && tr->declaration) {
        if (tr->declaration->kind == ast::TypeDeclKind::Record)
            return recordHasStringFields(tr->declaration);
        if (tr->declaration->kind == ast::TypeDeclKind::Choice)
            return choiceHasStringFields(tr->declaration);
        return false;  // enum: integer representation, no string to release
    }
    return false;
}


// fix #2326 / §9.8 v0.228: release every `string` storage reachable from
// inline-array element type at array ADDRESS `arrAddr` for the array
// type `arrayTy` (the OUTER inline-array TypeRef). Parallels
// emitInlineArrayEquality (LlvmBackend_Layout.cpp, §8.3 / §15.1.2): N is
// compile-time-constant (`arraySizeValue`, folded by sema per §15.1.2),
// so the walk is unrolled. Per-element dispatch mirrors
// emitRecordStringFieldReleases_impl's nested-field dispatch: a `string`
// element is bz_string_release'd and the slot nulled (matching the field
// nulling in the record walker); a non-nullable record element recurses
// via emitRecordStringFieldReleases_impl; a non-nullable choice element
// recurses via emitChoiceStringFieldReleases_impl; a non-nullable nested
// inline-array element recurses into itself. The element-type recursion
// terminates for the same reason as the predicate's: a finite DAG over
// the value-type graph.
//
// A NULLABLE element is excluded by the same reasoning the choice /
// record walkers use: it lowers to a {i8, T} wrapper, not the bare layout
// this walker assumes. A nullable element on a non-nullable array (e.g.
// `[N]string?`) is therefore a separate concern: the wire path stores an
// `i8` discriminator alongside each element; the inline-array
// serialization lane currently rejects nullable element types
// (isSerializationEligibleType's own gate, see TypeResolution_Checks.cpp),
// so a nullable-element inline array is not a live shape here.
//
// Takes the OUTER array TypeRef (not just the element type) so the
// per-element loop can derive N from `arraySizeValue` directly. Modern
// LLVM opaque pointers would otherwise force a `getElementType` on the
// pointee (which may be a primitive ptr, not a PointerType).
//
// Used by:
//   * emitRecordStringFieldReleases_impl (LlvmBackend_impl.hpp), to
//     recurse into a non-nullable inline-array field of a record;
//   * the #2326 deserialize-disposal-scope registration
//     (LlvmBackend_Serialization.cpp), to release the strings on a
//     non-nullable inline-array component / payload slot against a
//     LATER sibling's read throwing — its own entry, parallel to the
//     recordStringFields / choiceStringFields entries.
void Emitter::emitInlineArrayStringReleases_impl(
        llvm::Value* arrAddr, const sema::TypeRef* arrayTy) {
    if (!arrAddr || !arrayTy) return;
    // The element type is the array's `arrayElement`; N is `arraySizeValue`
    // (§15.1.2, folded by sema). §15.1 forbids an inline array of inline
    // arrays in practice, so a NESTED `[M][N]T` is never live here — the
    // recursion below handles only the defensive "what if §15.1 changes"
    // case, and `arrayElement` will be a string-bearing non-Array type in
    // every actual live shape.
    const sema::TypeRef* elemTy = arrayTy->arrayElement;
    int n_signed = arrayTy->arraySizeValue;
    if (!elemTy || n_signed < 0) return;
    if (elemTy->nullable) return;
    // Guard with the same predicate: if the element type carries no
    // string, there is nothing to release (the caller only ever invokes
    // this when the gate was true, but defense-in-depth matches the
    // emitChoiceStringFieldReleases_impl pattern).
    if (!inlineArrayElementCarriesString(elemTy)) return;
    unsigned n = static_cast<unsigned>(n_signed);

    llvm::Type* elemLlvmTy = lowerTypeRef(elemTy);
    if (!elemLlvmTy) return;
    auto* arrLlvmTy = llvm::ArrayType::get(elemLlvmTy, n);
    for (unsigned i = 0; i < n; ++i) {
        llvm::Value* slotAddr = builder_.CreateConstGEP2_64(
            arrLlvmTy, arrAddr, 0, i, "inlarr.elem.addr");
        if (elemTy->kind == sema::TypeRef::Kind::Primitive &&
            elemTy->primitive == sema::PrimitiveType::String) {
            llvm::Value* v =
                builder_.CreateLoad(ptrTy(), slotAddr, "inlarr.str.val");
            emitStringRelease(v);
            builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()),
                                 slotAddr);
            continue;
        }
        if (elemTy->kind == sema::TypeRef::Kind::InlineArray) {
            // Nested inline array: recurse into the sub-element type
            // against the sub-array's address. §15.1 forbids this in
            // practice (an inline array may not be the element of
            // another inline array) — left in place defensively in case
            // §15.1 ever changes; the recursion is finite.
            emitInlineArrayStringReleases_impl(slotAddr, elemTy);
            continue;
        }
        if (elemTy->nullable || elemTy->kind != sema::TypeRef::Kind::Named ||
            !elemTy->declaration)
            continue;
        if (elemTy->declaration->kind == ast::TypeDeclKind::Record &&
            recordHasStringFields(elemTy->declaration)) {
            emitRecordStringFieldReleases_impl(slotAddr, elemTy->declaration);
            continue;
        }
        if (elemTy->declaration->kind == ast::TypeDeclKind::Choice &&
            choiceHasStringFields(elemTy->declaration)) {
            emitChoiceStringFieldReleases_impl(slotAddr, elemTy->declaration);
        }
    }
}


void Emitter::disposeFreshNewReceiverTemp(const ast::Expr* recvExpr,
                                          llvm::Value* recvPtr,
                                          const sema::TypeRef* resultTy,
                                          bool calleeReturnsOwned) {
    // #1039 / #1053 / §28.3: a fresh owned-class producer used ONLY as the
    // receiver of a member access or method call — never bound to a variable —
    // is owned by nothing and leaks. Dispose it after the value result has been
    // extracted, when the receiver is a fresh `new`/`owned`-returning producer
    // (#1039/#1053-case-2) and the result cannot alias into it.
    if (!recvExpr || !recvPtr || !resultTy) return;
    if (!isFreshOwnedClassProducer(recvExpr)) return;
    // The extracted result must be a NON-REFERENCE value — provably free of any
    // reference or borrow into the receiver: a numeric/bool/char/decimal
    // primitive, an enum, or unit. EXCLUDED, because each may alias into the
    // receiver: `string` and `object` (reference-typed primitives — a field /
    // method result borrows the receiver's refcounted string or sub-object); a
    // struct (could embed a borrow of a receiver sub-object); any class /
    // interface result (a reference). Excluding these frees the temporary ONLY
    // when nothing aliases it. (Bezel's borrow checker guarantees an instance
    // method cannot escape `this` past the call when its result is such a
    // value, so post-call disposal is sound — §15.3.)
    bool valueResult = false;
    if (resultTy->kind == sema::TypeRef::Kind::Unit) {
        valueResult = true;
    } else if (resultTy->kind == sema::TypeRef::Kind::Primitive &&
               resultTy->primitive != sema::PrimitiveType::None &&
               resultTy->primitive != sema::PrimitiveType::String &&
               resultTy->primitive != sema::PrimitiveType::Object) {
        valueResult = true;
    } else if (resultTy->kind == sema::TypeRef::Kind::Named &&
               resultTy->declaration) {
        const auto* td =
            dynamic_cast<const ast::TypeDecl*>(resultTy->declaration->decl);
        if (td && td->kind == ast::TypeDeclKind::Enum) valueResult = true;
        // fix #1859 / §28.3: a CLASS/INTERFACE result under an ANNOUNCED
        // `owned` return type is a FRESH transfer, not a borrow of the
        // receiver — the declared return type IS the announcement (K1.5 +
        // K3.13, "Fresh productions are owned: … owned-returning calls").
        // The ordinary (unmarked) instance-method-return row is the one that
        // aliases the receiver ("Borrowed — instance method: source is the
        // receiver") and stays excluded (calleeReturnsOwned is false there).
        // An owned return could, in principle, construct a fresh object
        // whose OWN field stores a borrow of the receiver (the admitted
        // `_owner`-back-reference field-store shape, §28.6) — that is the
        // SAME residual-trust hazard the language already accepts at every
        // other field-store site (§28.6's "honest limit of the static
        // rule", backstopped by checked-disposal mode's DisposedAccess-
        // Exception), not a new one this fix introduces: the receiver was
        // ALREADY due for disposal at the end of the full expression per
        // §28.3's unbound-temporary rule (its only use was as this call's
        // receiver — a borrowing use), independent of what the callee's
        // body happens to do internally.
        else if (td && (td->kind == ast::TypeDeclKind::Class ||
                        td->kind == ast::TypeDeclKind::Interface) &&
                 calleeReturnsOwned) {
            valueResult = true;
        }
    } else if (resultTy->kind == sema::TypeRef::Kind::Named &&
               !resultTy->declaration && calleeReturnsOwned) {
        // fix #2205 / §28.3: an owning-collection / owning-wrapper result
        // (decl-null builtin, e.g. OwningArray<T>, OwningDictionary<K,V>,
        // OwningList<T>, Shared<T>, …) under an ANNOUNCED `owned` return is
        // likewise a FRESH transfer, not a borrow of the receiver — same
        // K1.5/K3.13 reasoning as #1859's class/interface case, just
        // expressed through the name-head instead of an AST TypeDecl (the
        // owning types are decl-null in the type system). The ordinary
        // (unmarked) instance-method-return row of an owning return is
        // itself an owned producer per §28.3's producer table, and the
        // receiver was ALREADY due for disposal at the end of the full
        // expression per §28.3's unbound-temporary rule.
        std::string_view head = resultTy->headName();
        if (common::isOwningCollectionHead(head) ||
            common::isOwningTypeHead(head)) {
            valueResult = true;
        }
    }
    if (!valueResult) return;
    // Dispose (idempotent vtable Dispose + reclaim exactly once). A fresh `new`
    // is non-null, but null-guard for safety/uniformity (folded away for a
    // provably-non-null pointer).
    llvm::Value* isNull = builder_.CreateICmpEQ(
        recvPtr, llvm::ConstantPointerNull::get(ptrTy()));
    auto* dispBB =
        llvm::BasicBlock::Create(llvm_, "freshrecv.dispose", currentFn_);
    auto* contBB =
        llvm::BasicBlock::Create(llvm_, "freshrecv.cont", currentFn_);
    builder_.CreateCondBr(isNull, contBB, dispBB);
    builder_.SetInsertPoint(dispBB);
    emitDisposeAndReclaim(recvPtr);
    builder_.CreateBr(contBB);
    builder_.SetInsertPoint(contBB);
}


llvm::Function* Emitter::elementDisposerThunk() {
    // #362 / stdlib §4.5: a standalone `void(void*)` thunk that runs an owning
    // collection element's destructor via the element's own vtable (slot 2,
    // CG35-itable), skipping a null element. Built once with its own IRBuilder
    // so the active `builder_` insert point is untouched.
    if (elementDisposerThunk_) return elementDisposerThunk_;
    auto* fnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_),
                                         {ptrTy()}, /*isVarArg=*/false);
    auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage,
                                      "bz$elemDispose", module_.get());
    llvm::Argument* obj = &*fn->args().begin();
    auto* entry = llvm::BasicBlock::Create(llvm_, "entry", fn);
    auto* dispBB = llvm::BasicBlock::Create(llvm_, "elem.dispose", fn);
    auto* retBB  = llvm::BasicBlock::Create(llvm_, "elem.ret", fn);
    llvm::IRBuilder<> b(entry);
    // A null/already-moved slot is safe to skip (no double-dispose / UAF).
    llvm::Value* isNull = b.CreateICmpEQ(
        obj, llvm::ConstantPointerNull::get(ptrTy()));
    b.CreateCondBr(isNull, retBB, dispBB);
    b.SetInsertPoint(dispBB);
    // vtable layout (CG35-itable): [0]=td$, [1]=itmap$, [2]=Dispose.
    llvm::Value* vptr = b.CreateLoad(ptrTy(), obj);
    llvm::Value* slot = b.CreateGEP(ptrTy(), vptr, {b.getInt32(2)});
    llvm::Value* disposeFn = b.CreateLoad(ptrTy(), slot);
    auto* dispTy = llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_),
                                           {ptrTy()}, /*isVarArg=*/false);
    b.CreateCall(dispTy, disposeFn, {obj});
    // #380 / §28: an owning collection's element is a heap class instance; after
    // its (most-derived) Dispose runs the destructor + base chain, reclaim its
    // block. This thunk is the element-dispose path that does NOT route through
    // emitDynamicDispose (it is invoked per live element by bz_array_free), so
    // the free must be emitted here too. obj is non-null in this block.
    //
    // §28.2 v0.134 / issue #601 / #2514: mirror emitDisposeAndReclaim's
    // checked-disposal quarantine — in checked mode the block is NOT freed
    // after its Dispose runs, so the per-instance disposed flag (set by the
    // vtable Dispose call above) stays readable for a later member-entry /
    // field-access guard. This thunk is ALSO the per-T result_disposer attached
    // to a Pattern A `Task<T:class>` at production
    // (asyncPatternAResultDisposer_ → patternAResultDisposerFor → this thunk),
    // so without the gate a stale borrow of a disposed task's result — the
    // §28.7/§30.5/§30.10 drop-on-overwrite shape — reads FREED heap before the
    // flag guard fires (a heap-use-after-free under ASan, #2173/#2514). Gating
    // the free keeps the quarantined block's flag valid, making the stale read
    // deterministically raise DisposedAccessException (§31.4) instead. The
    // owning-collection element path is made sound the same way (each element
    // is quarantined alongside the already-quarantined header,
    // bz_array_mark_disposed). `checkedDisposal_` is fixed for the whole
    // module compile, so baking the branch into the cached thunk is correct.
    if (!checkedDisposal_)
        b.CreateCall(freeFn(), {obj});
    b.CreateBr(retBB);
    b.SetInsertPoint(retBB);
    b.CreateRetVoid();
    elementDisposerThunk_ = fn;
    return fn;
}


// §28.13 / stdlib §2.16 (#603): void(void*) thunk that calls
// bz_shared_release(ptr), used as the per-element disposal callback for an
// OwningArray<Shared<T>> (or similar owning collection of shared handles).
// Mirrors elementDisposerThunk but calls the runtime release rather than
// dispatching through a class vtable (Shared<T> has no vtable).
llvm::Function* Emitter::sharedReleaseThunk() {
    if (sharedReleaseThunk_) return sharedReleaseThunk_;
    auto* fnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_),
                                         {ptrTy()}, /*isVarArg=*/false);
    auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage,
                                      "bz$sharedRelease", module_.get());
    llvm::Argument* h = &*fn->args().begin();
    auto* entry = llvm::BasicBlock::Create(llvm_, "entry", fn);
    auto* relBB  = llvm::BasicBlock::Create(llvm_, "h.rel",  fn);
    auto* retBB  = llvm::BasicBlock::Create(llvm_, "h.ret",  fn);
    llvm::IRBuilder<> b(entry);
    b.CreateCondBr(
        b.CreateICmpEQ(h, llvm::ConstantPointerNull::get(ptrTy())),
        retBB, relBB);
    b.SetInsertPoint(relBB);
    auto* relFn = declareC("bz_shared_release",
                           llvm::Type::getVoidTy(llvm_), {ptrTy()});
    b.CreateCall(relFn, {h});
    b.CreateBr(retBB);
    b.SetInsertPoint(retBB);
    b.CreateRetVoid();
    sharedReleaseThunk_ = fn;
    return fn;
}

// §28.13 / stdlib §2.16 (#603): void(void*) thunk that calls
// bz_weak_release(ptr), used as the per-element disposal callback for an
// OwningArray<Weak<T>>.
llvm::Function* Emitter::weakReleaseThunk() {
    if (weakReleaseThunk_) return weakReleaseThunk_;
    auto* fnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_),
                                         {ptrTy()}, /*isVarArg=*/false);
    auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage,
                                      "bz$weakRelease", module_.get());
    llvm::Argument* h = &*fn->args().begin();
    auto* entry = llvm::BasicBlock::Create(llvm_, "entry", fn);
    auto* relBB  = llvm::BasicBlock::Create(llvm_, "h.rel",  fn);
    auto* retBB  = llvm::BasicBlock::Create(llvm_, "h.ret",  fn);
    llvm::IRBuilder<> b(entry);
    b.CreateCondBr(
        b.CreateICmpEQ(h, llvm::ConstantPointerNull::get(ptrTy())),
        retBB, relBB);
    b.SetInsertPoint(relBB);
    auto* relFn = declareC("bz_weak_release",
                           llvm::Type::getVoidTy(llvm_), {ptrTy()});
    b.CreateCall(relFn, {h});
    b.CreateBr(retBB);
    b.SetInsertPoint(retBB);
    b.CreateRetVoid();
    weakReleaseThunk_ = fn;
    return fn;
}

// stdlib §5 (#651): void(void*) thunk that calls bz_grouping_free(ptr),
// used as the per-element disposal callback for an Array<IGrouping<TKey,T>>.
// Null-checks the pointer first (same pattern as sharedReleaseThunk).
llvm::Function* Emitter::groupingFreeThunk() {
    if (groupingFreeThunk_) return groupingFreeThunk_;
    auto* fnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_),
                                         {ptrTy()}, /*isVarArg=*/false);
    auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage,
                                      "bz$groupingFree", module_.get());
    llvm::Argument* h = &*fn->args().begin();
    auto* entry = llvm::BasicBlock::Create(llvm_, "entry", fn);
    auto* freeBB = llvm::BasicBlock::Create(llvm_, "g.free", fn);
    auto* retBB  = llvm::BasicBlock::Create(llvm_, "g.ret",  fn);
    llvm::IRBuilder<> b(entry);
    b.CreateCondBr(
        b.CreateICmpEQ(h, llvm::ConstantPointerNull::get(ptrTy())),
        retBB, freeBB);
    b.SetInsertPoint(freeBB);
    auto* freeFn = declareC("bz_grouping_free",
                            llvm::Type::getVoidTy(llvm_), {ptrTy()});
    b.CreateCall(freeFn, {h});
    b.CreateBr(retBB);
    b.SetInsertPoint(retBB);
    b.CreateRetVoid();
    groupingFreeThunk_ = fn;
    return fn;
}

// §2.11 (#838): the per-element disposer thunk for a Subscription element of an
// owning collection (OwningArray<Subscription>, etc.). A Subscription is a
// runtime-managed opaque handle (bz_subscription_t*) disposed by
// bz_subscription_dispose (which unsubscribes), not via a vtable — so an owning
// collection of Subscriptions needs this thunk to unsubscribe each element at
// scope-end/clear/remove. Null-safe + idempotent (mirrors the `using var`
// Subscription auto-dispose). Same shape as groupingFreeThunk.
llvm::Function* Emitter::subscriptionDisposeThunk() {
    if (subscriptionDisposeThunk_) return subscriptionDisposeThunk_;
    auto* fnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_),
                                         {ptrTy()}, /*isVarArg=*/false);
    auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage,
                                      "bz$subscriptionDispose", module_.get());
    llvm::Argument* h = &*fn->args().begin();
    auto* entry = llvm::BasicBlock::Create(llvm_, "entry", fn);
    auto* dispBB = llvm::BasicBlock::Create(llvm_, "s.disp", fn);
    auto* retBB  = llvm::BasicBlock::Create(llvm_, "s.ret",  fn);
    llvm::IRBuilder<> b(entry);
    b.CreateCondBr(
        b.CreateICmpEQ(h, llvm::ConstantPointerNull::get(ptrTy())),
        retBB, dispBB);
    b.SetInsertPoint(dispBB);
    auto* dispFn = declareC("bz_subscription_dispose",
                            llvm::Type::getVoidTy(llvm_), {ptrTy()});
    b.CreateCall(dispFn, {h});
    b.CreateBr(retBB);
    b.SetInsertPoint(retBB);
    b.CreateRetVoid();
    subscriptionDisposeThunk_ = fn;
    return fn;
}

// §28.11 (#868): void(void*) thunk that frees a nested builtin owning collection
// element (e.g. the OwningArray<R> inside an OwningArray<OwningArray<R>>) by calling
// the right runtime free fn with the recursive inner-element disposer.
// Mirrors sharedReleaseThunk / groupingFreeThunk in shape: own IRBuilder, null
// check, call free fn, return void. Cached per inner element-collection TypeRef*.
//
// Free fn selection (mirrors emitOwnedFieldDisposals / emitDisposals):
//   OwningArray / OwningStack / OwningQueue  → bz_array_free(ptr, innerDisposer)
//   OwningLinkedList                       → bz_llist_free(ptr, innerDisposer)
//   OwningDictionary                       → bz_dict_free(ptr, innerValueDisposer)
//     (OwningDictionary<K,V> owns its V elements; genericArgs[1] is V)
//   OwningSet / OwningBuffer                → bz_array_free(ptr, elementDisposer)
//     (#888: an OwningSet<class>'s elements are disposed by bz_array_free with the
//     element disposer, the same call the top-level/field OwningSet disposal uses.)
//
// The recursion terminates: each level peels one collection layer; eventually the
// inner element is a class R → elementDisposerThunk, or a plain value → null.
llvm::Function* Emitter::nestedCollectionDisposerThunk(const sema::TypeRef* elemCollTy) {
    // Cache hit.
    auto it = nestedCollectionDisposerThunks_.find(elemCollTy);
    if (it != nestedCollectionDisposerThunks_.end()) return it->second;

    // Determine the inner element type and free fn name from the collection's
    // kind (using the same name-detection as isCollectionTypeName /
    // isLinkedListTypeName / isDictTypeName, per §28.11 #868).
    std::string_view collName;
    if (elemCollTy && elemCollTy->kind == sema::TypeRef::Kind::Named &&
        !elemCollTy->declaration &&
        elemCollTy->astNode && elemCollTy->astNode->name &&
        !elemCollTy->astNode->name->parts.empty())
        collName = elemCollTy->headName();

    bool isLL   = (collName == "OwningLinkedList");
    bool isDict = (collName == "OwningDictionary");

    const char* freeName = isLL ? "bz_llist_free"
                         : isDict ? "bz_dict_free"
                         : "bz_array_free";

    // Resolve the inner element type (the thing the inner collection owns):
    //   OwningArray/Stack/Queue/LinkedList/Buffer/Set → genericArgs[0]
    //   OwningDictionary                              → genericArgs[1] (the value)
    // §28.11 (#888): OwningSet's class elements are freed via bz_array_free with
    // the element disposer — the SAME call the top-level/field OwningSet disposal
    // uses (it is not a dict-type-name, so the field path at emitOwnedField-
    // Disposals selects bz_array_free + the element disposer). Earlier this thunk
    // passed a null disposer for a nested OwningSet, so an OwningSet<class> inside
    // another owning collection leaked its elements; routing genericArgs[0]
    // through elementPolicyFor disposes them, mirroring the top-level path.
    const sema::TypeRef* innerElem = nullptr;
    if (elemCollTy && !isDict) {
        innerElem = (!elemCollTy->genericArgs.empty()) ? elemCollTy->genericArgs[0]
                                                       : nullptr;
    } else if (isDict) {
        innerElem = (elemCollTy->genericArgs.size() > 1) ? elemCollTy->genericArgs[1]
                                                         : nullptr;
    }

    // Recursively get the inner element's disposer (elementPolicyFor does its
    // own caching / base cases, including further nested-collection detection).
    llvm::Function* innerDisposer =
        elementPolicyFor(innerElem, /*owningFamily=*/true).disposerThunk;

    // Build the thunk: void(void* innerCollPtr) { if(!ptr) return;
    //                                              freeFn(ptr, innerDisposer); }
    // Uses its own IRBuilder so `builder_` insert point is untouched.
    auto* fnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_),
                                         {ptrTy()}, /*isVarArg=*/false);
    // Name the thunk after the collection kind for debuggability.
    std::string thunkName = std::string("bz$nestedFree$") + std::string(collName);
    // Make unique if same collection name appears multiple times (shouldn't, but
    // guard against symbol clash if two OwningArray<R1> and OwningArray<R2> both
    // need a thunk — give each its own symbol via the cache key pointer).
    if (module_->getFunction(thunkName)) {
        thunkName += "$" + std::to_string(
            reinterpret_cast<std::uintptr_t>(elemCollTy));
    }
    auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage,
                                      thunkName, module_.get());
    llvm::Argument* ptr = &*fn->args().begin();
    auto* entry  = llvm::BasicBlock::Create(llvm_, "entry",    fn);
    auto* freeBB = llvm::BasicBlock::Create(llvm_, "coll.free", fn);
    auto* retBB  = llvm::BasicBlock::Create(llvm_, "coll.ret",  fn);
    llvm::IRBuilder<> b(entry);
    // Null-guard: bz_array_free/bz_llist_free/bz_dict_free are null-safe but
    // check here anyway (mirrors sharedReleaseThunk pattern).
    b.CreateCondBr(
        b.CreateICmpEQ(ptr, llvm::ConstantPointerNull::get(ptrTy())),
        retBB, freeBB);
    b.SetInsertPoint(freeBB);
    llvm::Value* innerDispArg = innerDisposer
        ? llvm::cast<llvm::Value>(innerDisposer)
        : llvm::ConstantPointerNull::get(ptrTy());
    auto* freeFn = declareC(freeName,
                            llvm::Type::getVoidTy(llvm_),
                            {ptrTy(), ptrTy()});
    b.CreateCall(freeFn, {ptr, innerDispArg});
    b.CreateBr(retBB);
    b.SetInsertPoint(retBB);
    b.CreateRetVoid();

    nestedCollectionDisposerThunks_[elemCollTy] = fn;
    return fn;
}


// #2279 (§30.5 Pattern A, v0.236): build the per-result-collection-type
// `void(void*)` thunk that frees a Pattern A task's RESULT when that result
// is itself an intrinsic collection (Array<T> / Set<T> / Dictionary<K,V> /
// LinkedList<T> / Stack<T> / Queue<T> / OwningBuffer<T> / OwningArray<T> /
// Owning* collections). §30.5 producer-side rules: a `Task<T:class>` for
// class T always owns its result directly, and "disposing the task disposes
// the result" — for a class instance that means calling its destructor via
// the vtable; for an intrinsic collection that means freeing the
// collection's backing storage (the bz_array_t*/bz_dict_t*/bz_llist_t*/*
// bz_buffer_t* header+data) with the right per-element disposer. The
// existing elementPolicyFor-based path (`patternAResultDisposerFor`'s
// ClassOwned branch) returns null when T is e.g. Array<int> (int is a Value
// category, no disposer thunk) — the result then leaked (LSan: 104 bytes
// from bz_array_alloc per #2279's repro). This helper fills that gap.
//
// Free fn selection (mirrors the `using var` collection disposal path and
// `nestedCollectionDisposerThunk` above, so the result-disposal contract
// stays consistent regardless of whether the consumer is the task's own
// teardown or a `using var` of the task):
//   Array / Stack / Queue / Set / SortedSet / OwningArray / OwningStack
//     / OwningQueue / OwningSet              → bz_array_free(ptr, elemDisposer)
//   Dictionary / OwningDictionary / SortedDictionary
//                                             → bz_dict_free(ptr, valueDisposer)
//   LinkedList / OwningLinkedList             → bz_llist_free(ptr, elemDisposer)
//   OwningBuffer                             → bz_buffer_free(ptr)
//
// Per-element disposer is the per-element-policy disposer for the SAME
// family (owning vs borrowing) as the result collection — so a borrowing
// Array<MyClass> passes null (the array only borrows its class elements per
// §28.11; elements are owned elsewhere and never disposed by the array),
// an OwningArray<MyClass> passes the element-disposer thunk, an
// Array<string> passes stringElementReleaseThunk (the +1 retained at insert
// is released at free), an OwningDictionary<MyClass, MyClass> passes the
// value-side disposer (genericArgs[1]). Returns null when `resultCollTy` is
// not an intrinsic collection — the caller falls back to the existing
// class-result path which is correct for plain classes.
llvm::Function* Emitter::patternAResultCollectionDisposer(
    const sema::TypeRef* resultCollTy) {
    // Cache hit.
    auto it = patternAResultCollectionDisposers_.find(resultCollTy);
    if (it != patternAResultCollectionDisposers_.end()) return it->second;

    // Reject non-intrinsic-collection types — the existing class-result path
    // is correct for plain classes. An intrinsic collection has no
    // declaration (it's a compiler-known builtin, like the Shared/Weak/
    // IGrouping/Subscription builtins) with a name-typed astNode. Mirror
    // the builtin-collTy detection already used by nestedCollectionDisposer-
    // Thunk / collectionTypeOfLocal / linkedListTypeOfLocal — same keying,
    // so a user class literally named `Array` keeps the existing path.
    std::string_view collName;
    if (!resultCollTy || resultCollTy->kind != sema::TypeRef::Kind::Named ||
        resultCollTy->declaration ||
        !resultCollTy->astNode || !resultCollTy->astNode->name ||
        resultCollTy->astNode->name->parts.empty()) {
        return nullptr;
    }
    collName = resultCollTy->headName();

    // Map the collection kind to (free-fn, family-for-element-policy). The
    // runtime free fns accept a null per-element disposer for value-typed
    // elements (bz_array_free skips the dispose loop) and for borrowing
    // elements (the array only borrows its elements per §28.11). For a
    // borrowing collection of strings, stringElementReleaseThunk is the
    // right disposer (the +1 retained at insert must be released at free).
    bool isLL          = (collName == "OwningLinkedList" || collName == "LinkedList");
    bool isDict        = (collName == "Dictionary" || collName == "OwningDictionary" ||
                          collName == "SortedDictionary");
    bool isBuffer      = (collName == "OwningBuffer");
    bool isOwningColl  = isLL || isDict || isBuffer
                          ? false  // isOwning derived per-coll below
                          : common::isOwningPrefixedName(collName) ||
                            collName == "Array" || collName == "Stack" ||
                            collName == "Queue" || collName == "Set" ||
                            collName == "SortedSet";
    // For LinkedList / Dictionary, the owning property is the head's
    // "Owning" prefix (per stdlib §4.6/§4.7/§4.8/§4.9 — OwningLinkedList /
    // OwningDictionary own their elements; the borrowing forms don't).
    if (collName == "LinkedList" || collName == "Dictionary" ||
        collName == "SortedDictionary") {
        isOwningColl = common::isOwningPrefixedName(collName);
    }
    if (!isLL && !isDict && !isBuffer && !isOwningColl) {
        // Not a recognized intrinsic collection. Fall through to the
        // class-result path; the caller returns null in that case.
        return nullptr;
    }
    // OwningBuffer is NOT value-owning by element kind — its `T` is value-
    // typed by construction (it is the manual-storage primitive, §4.14) and
    // bz_buffer_free does not take an element disposer. The owning flag is
    // irrelevant for buffer selection; treat its element disposer as null.
    if (isBuffer) {
        isOwningColl = false;  // no element disposer on bz_buffer_free
    }

    const char* freeName = isLL ? "bz_llist_free"
                         : isDict ? "bz_dict_free"
                         : isBuffer ? "bz_buffer_free"
                         : "bz_array_free";
    // The free fn signature: bz_array_free/bz_llist_free/bz_dict_free take
    // (ptr, dispose_elem), bz_buffer_free takes (ptr) only.
    bool freeFnTakesElem = !isBuffer;

    // Per-element (or per-value) disposer:
    //   Array/Stack/Queue/Set/OwningArray/OwningStack/OwningQueue/OwningSet/
    //   LinkedList/OwningLinkedList: elementType = genericArgs[0], isOwning
    //     = isOwningColl
    //   Dictionary/OwningDictionary/SortedDictionary: elementType is the
    //     KEY (genericArgs[0]) for isOwning purposes, BUT the value
    //     (genericArgs[1]) is the one that carries the disposal obligation
    //     (§28.11 "owning dictionary owns its values, keys are value-like").
    //     For the runtime free call we pass the VALUE disposer; the value
    //     element policy (owningDictionaryElementDisposer etc.) already uses
    //     genericArgs[1].
    //   OwningBuffer: no per-element disposer.
    const sema::TypeRef* elemForDisposer = nullptr;
    if (isBuffer) {
        elemForDisposer = nullptr;
    } else if (isDict) {
        elemForDisposer = (resultCollTy->genericArgs.size() > 1)
                              ? resultCollTy->genericArgs[1] : nullptr;
    } else {
        elemForDisposer = (!resultCollTy->genericArgs.empty())
                              ? resultCollTy->genericArgs[0] : nullptr;
    }
    // Resolve the disposer thunk through elementPolicyFor — it returns null
    // for value elements (no obligation), the class element-disposer thunk
    // for class elements, and a string-typed variant via the same call. For
    // the array-of-string case specifically, elementPolicyFor returns
    // Category::RuntimeManaged with disposerThunk=null but retainRelease=true;
    // we explicitly use stringElementReleaseThunk below because the runtime
    // call needs a real function pointer for the per-element release.
    llvm::Value* elemDisposer = nullptr;
    if (elemForDisposer) {
        ElementPolicy elemPol = elementPolicyFor(elemForDisposer, isOwningColl);
        if (elemPol.disposerThunk) {
            elemDisposer = elemPol.disposerThunk;
        } else if (elemPol.cat == ElementPolicy::Category::RuntimeManaged &&
                   elemForDisposer->kind == sema::TypeRef::Kind::Primitive &&
                   elemForDisposer->primitive == sema::PrimitiveType::String) {
            // Borrowing Array<string> / Dictionary<string, int> / etc. —
            // release each string element. Mirrors the
            // collectionElemDisposer wiring in LlvmBackend_Setup.cpp
            // (which sets stringElementReleaseThunk for string-element
            // collections).
            elemDisposer = stringElementReleaseThunk();
        }
    }

    // Build the thunk: void(void* resultCollPtr) {
    //                       if (!ptr) return;
    //                       freeFn(ptr, elemDisposer); }
    // For bz_buffer_free (no element-disposer arg), the call drops it.
    auto* fnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_),
                                         {ptrTy()}, /*isVarArg=*/false);
    std::string thunkName = std::string("bz$patAResultCollFree$") +
                            std::string(collName);
    if (module_->getFunction(thunkName)) {
        thunkName += "$" + std::to_string(
            reinterpret_cast<std::uintptr_t>(resultCollTy));
    }
    auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage,
                                      thunkName, module_.get());
    llvm::Argument* ptr = &*fn->args().begin();
    auto* entry  = llvm::BasicBlock::Create(llvm_, "entry",    fn);
    auto* freeBB = llvm::BasicBlock::Create(llvm_, "coll.free", fn);
    auto* retBB  = llvm::BasicBlock::Create(llvm_, "coll.ret",  fn);
    llvm::IRBuilder<> b(entry);
    // Null-guard: every free fn above is null-safe but the guard matches the
    // nestedCollectionDisposerThunk convention.
    b.CreateCondBr(
        b.CreateICmpEQ(ptr, llvm::ConstantPointerNull::get(ptrTy())),
        retBB, freeBB);
    b.SetInsertPoint(freeBB);
    if (freeFnTakesElem) {
        llvm::Value* dispArg = elemDisposer
            ? elemDisposer
            : llvm::ConstantPointerNull::get(ptrTy());
        auto* freeFn = declareC(freeName,
                                llvm::Type::getVoidTy(llvm_),
                                {ptrTy(), ptrTy()});
        b.CreateCall(freeFn, {ptr, dispArg});
    } else {
        // bz_buffer_free: single-arg free.
        auto* freeFn = declareC(freeName,
                                llvm::Type::getVoidTy(llvm_),
                                {ptrTy()});
        b.CreateCall(freeFn, {ptr});
    }
    b.CreateBr(retBB);
    b.SetInsertPoint(retBB);
    b.CreateRetVoid();

    patternAResultCollectionDisposers_[resultCollTy] = fn;
    return fn;
}


bool Emitter::collectionElemIsClass(const sema::TypeRef* collTy, unsigned idx) {
    // fix #619 / §28.11 v0.135 / §14.4: the §28.11 table's owned/borrow column
    // applies to "class / interface type" — both class AND interface elements are
    // class-like from the collection's perspective (vtable-dispatch, pointer slot).
    // Widening to Class|Interface fixes the Class-only check that caused
    // OwningDictionary<K,IFoo> indexer-set and move-out to skip disposal for
    // interface-valued slots.
    if (!collTy || collTy->genericArgs.size() <= idx) return false;
    const sema::TypeRef* el = collTy->genericArgs[idx];
    if (!el || el->kind != sema::TypeRef::Kind::Named || !el->declaration)
        return false;
    return el->declaration->kind == ast::TypeDeclKind::Class ||
           el->declaration->kind == ast::TypeDeclKind::Interface;
}


// #602 batch 3: owningCollectionElemDisposer deleted — all call sites now use
// elementPolicyFor(el, owningFamily).disposerThunk directly.
// #602 batch 3: owningDictValueDisposer deleted — all call sites now use
// elementPolicyFor(val, dictIsOwning).disposerThunk directly.
// #602 batch 3: isStringElementSet deleted — all call sites now use
// elementPolicyFor(el, false) + string primitive check directly.
// #602 batch 3: isStringValueDict deleted — all call sites now use
// elementPolicyFor(val, false) + string primitive check directly.
// #602 batch 3: stringElementReleaseDisposer deleted — all disposal call sites
// now use elementPolicyFor directly.
// #602 batch 4: isStringElementArray deleted — its only use was inside
// emitArrayRetainStringsIfStringArray; that helper now calls elementPolicyFor
// directly (the logic is inlined in LlvmBackend_impl.hpp).

llvm::Function* Emitter::stringElementReleaseThunk() {
    // #429 / §4.6 / §28.9: a standalone `void(void*)` thunk that releases one
    // co-owned string element (the slot holds a bz_string*). bz_string_release is
    // null-safe and a no-op on a sentinel/literal, so a null/never-stored slot is
    // safe to pass. Built once with its own IRBuilder so the active `builder_`
    // insert point is untouched (mirrors elementDisposerThunk).
    if (stringElementReleaseThunk_) return stringElementReleaseThunk_;
    auto* fnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_),
                                         {ptrTy()}, /*isVarArg=*/false);
    auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage,
                                      "bz$strElemRelease", module_.get());
    llvm::Argument* s = &*fn->args().begin();
    auto* entry = llvm::BasicBlock::Create(llvm_, "entry", fn);
    llvm::IRBuilder<> b(entry);
    auto* relFn = declareC("bz_string_release",
                           llvm::Type::getVoidTy(llvm_), {ptrTy()});
    b.CreateCall(relFn, {s});   // null-safe / sentinel-safe in the runtime
    b.CreateRetVoid();
    stringElementReleaseThunk_ = fn;
    return fn;
}



llvm::Function* Emitter::valueBoxFreeThunk() {
    // #401 / stdlib §4.8 + language spec §3.2 / §9: a standalone `void(void*)`
    // thunk that just `free`s the pointer (null-safe), with NO destructor and no
    // vtable dispatch. A record value in a borrowing Dictionary<K, record> is
    // heap-BOXED (see the encode in LlvmBackend_{Calls,Assign}.cpp); a record is
    // a value type (§3.2 "value types ... do not [carry a disposal obligation]";
    // §9 "A record is a value type"), so the box has nothing to dispose inside it
    // — it is just storage to reclaim. Built once with its own IRBuilder so the
    // active `builder_` insert point is untouched (mirrors elementDisposerThunk).
    if (valueBoxFreeThunk_) return valueBoxFreeThunk_;
    auto* fnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_),
                                         {ptrTy()}, /*isVarArg=*/false);
    auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage,
                                      "bz$valBoxFree", module_.get());
    llvm::Argument* box = &*fn->args().begin();
    auto* entry = llvm::BasicBlock::Create(llvm_, "entry", fn);
    auto* freeBB = llvm::BasicBlock::Create(llvm_, "box.free", fn);
    auto* retBB  = llvm::BasicBlock::Create(llvm_, "box.ret", fn);
    llvm::IRBuilder<> b(entry);
    // A null box (e.g. a moved-from / never-stored slot) is safe to skip.
    llvm::Value* isNull = b.CreateICmpEQ(
        box, llvm::ConstantPointerNull::get(ptrTy()));
    b.CreateCondBr(isNull, retBB, freeBB);
    b.SetInsertPoint(freeBB);
    b.CreateCall(freeFn(), {box});
    b.CreateBr(retBB);
    b.SetInsertPoint(retBB);
    b.CreateRetVoid();
    valueBoxFreeThunk_ = fn;
    return fn;
}


llvm::Function* Emitter::dictValueBoxFreer(const sema::TypeRef* dictTy) {
    // #401 / stdlib §4.8: only a BORROWING Dictionary<K,V> whose value type V is
    // a RECORD or a NULLABLE VALUE TYPE stores a heap box per value (the encode in
    // LlvmBackend_{Calls,Assign}.cpp boxes exactly `(valRec || valNullableVal) &&
    // isStructTy()`); its disposal must reclaim each surviving box. An
    // OwningDictionary<K, class> routes through owningDictValueDisposer instead
    // (#394: dispose+free a class object), and a borrowing Dictionary with a
    // scalar/string value stores no box (the value is inttoptr'd or is the pointer
    // itself) — both return null here, so the value box is never freed twice.
    // The record check mirrors the encode's broad test (any record declaration,
    // equality-bearing or not), NOT the narrower recordTypeOfRef, so it matches
    // every boxed value exactly.
    // issue #539 / §7: nullable value types ({i8,T} tagged structs) are also
    // heap-boxed by encodeValue; their boxes must be freed at Clear/scope-end too.
    if (!dictTy || !dictTy->astNode || !dictTy->astNode->name ||
        dictTy->astNode->name->parts.empty())
        return nullptr;
    if (dictTy->headName() != "Dictionary") return nullptr;
    if (dictTy->genericArgs.size() <= 1) return nullptr;
    const sema::TypeRef* valTy = dictTy->genericArgs[1];
    bool valIsRecord =
        valTy && valTy->kind == sema::TypeRef::Kind::Named &&
        valTy->declaration &&
        valTy->declaration->kind == ast::TypeDeclKind::Record;
    bool valIsNullableVal = valTy && isNullableValueTy(valTy);
    if (!valIsRecord && !valIsNullableVal) return nullptr;
    return valueBoxFreeThunk();
}


// #816 / §28.5: build (once per instantiation, cached) the specialized CORE
// disposer for a class `ts` reached as the BASE of a specialized derived class,
// with the current genericSubst_ active. Mirrors the core half of
// emitSpecializedDisposer (which builds a public run-once wrapper + core for the
// most-derived class) but returns just the core, since a base chain calls the
// base CORE (unguarded — the run-once flag is checked once at the most-derived
// public Dispose). Saves/restores the full emit state + insert point because it
// is invoked mid-emission of the derived class's disposer (from
// emitBaseDisposeChain).
llvm::Function* Emitter::specializedDisposerCoreFor(const sema::TypeSymbol* ts) {
    std::vector<const sema::Symbol*> storage;
    collectInstanceStorage(ts, storage);
    std::vector<llvm::Type*> specFieldTys;
    specFieldTys.push_back(ptrTy());
    specFieldTys.push_back(disposedFlagTy());
    for (const sema::Symbol* m : storage)
        specFieldTys.push_back(storageType(m));
    auto& specMap = specializedDisposerCores_[ts];
    auto sit = specMap.find(specFieldTys);
    if (sit != specMap.end()) return sit->second;

    std::string suffix = "$baseSpec" + std::to_string(specMap.size());
    auto* fnType = llvm::FunctionType::get(
        llvm::Type::getVoidTy(llvm_), {ptrTy()}, /*isVarArg=*/false);
    auto* coreFn = llvm::Function::Create(
        fnType, llvm::Function::InternalLinkage,
        "bz$" + std::string(ts->name) + "$DisposeCore" + suffix, module_.get());
    specMap[specFieldTys] = coreFn;  // register before emit (self/diamond safety)

    // Save the caller's (derived-disposer) emit state + insert point.
    llvm::Function* savedFn = currentFn_;
    llvm::Value* savedThis = currentThis_;
    auto savedLocals = locals_;
    auto savedParamSlots = paramSlots_;
    auto savedRefParams = refParams_;
    auto savedDisposeStack = disposeStack_;
    auto savedLoopStack = loopStack_;
    auto savedFinallyStack = finallyStack_;
    llvm::BasicBlock* savedPropagate = propagateBB_;
    llvm::BasicBlock* savedUnwind = unwindTarget_;
    llvm::Value* savedCaughtExc = currentCaughtExc_;
    std::string_view savedCaughtName = currentCaughtName_;
    std::size_t savedScopeDepth = unwindScopeDepth_;
    llvm::IRBuilder<>::InsertPoint savedIP = builder_.saveIP();

    emitDisposerCore(ts, coreFn);  // genericSubst_ stays active

    currentFn_ = savedFn;
    currentThis_ = savedThis;
    locals_ = std::move(savedLocals);
    paramSlots_ = std::move(savedParamSlots);
    refParams_ = std::move(savedRefParams);
    disposeStack_ = std::move(savedDisposeStack);
    loopStack_ = std::move(savedLoopStack);
    finallyStack_ = std::move(savedFinallyStack);
    propagateBB_ = savedPropagate;
    unwindTarget_ = savedUnwind;
    currentCaughtExc_ = savedCaughtExc;
    currentCaughtName_ = savedCaughtName;
    unwindScopeDepth_ = savedScopeDepth;
    builder_.restoreIP(savedIP);
    return coreFn;
}


void Emitter::emitBaseDisposeChain(const sema::TypeSymbol* ts) {
    // §28.4 step 3: chain to the base class's CORE disposer (unguarded), NOT its
    // public Dispose. The per-object run-once flag (§28.2 / §28.6) is checked
    // and set exactly once at the most-derived public Dispose entry; calling the
    // base's public Dispose here would re-check that same flag (same object) and
    // see it already set, skipping the base destructor + base owned fields. The
    // core runs the base's destructor + owned-field disposal + its own base
    // chain unconditionally, which is correct because the whole chain executes
    // within the single guarded most-derived disposal.
    for (std::size_t bi = 0; bi < ts->baseTypes.size(); ++bi) {
        const sema::TypeSymbol* base = ts->baseTypes[bi];
        if (!base || base->kind != ast::TypeDeclKind::Class) continue;
        // §14 / §28.4 / §11.7: the shared, type-erased base core disposer
        // (disposerCoreFns_[base]) is emitted ONCE against the base template's
        // erased layout (each of its type parameters lowered as the i32
        // placeholder). That template disposer is WRONG for a concrete
        // instantiation of the base whenever either:
        //   (a) #816: the base (transitively) has an owned type-PARAMETER field
        //       (`owned _f: T`) — the erased disposer cannot classify the erased T
        //       as an owned class, so it SKIPS disposing that field entirely
        //       (→ leak), OR
        //   (b) #2019: the base's instantiated field layout DIFFERS from the erased
        //       template layout — e.g. a width-varying value field (`bval: T`, i64
        //       for T=long) that precedes an owned CONCRETE field (`owned bf: Cell`)
        //       shifts that owned field's offset, so the template disposer frees/
        //       reads the WRONG offset (→ SEGV on a plain `D<long> : B<T>`). Note
        //       (a) is NOT subsumed by (b): a `T`=int owned type-param field leaves
        //       the layout identical to the i32 template yet is still skipped.
        // In either case, bind the base's concrete type arguments — from
        // ts->baseTypeRefs[bi], resolved under any active OUTER substitution — and
        // call a SPECIALIZED base core emitted with that substitution active, so
        // every owned base field is disposed exactly once at its CORRECT offset.
        // The baseTypeRefs derivation covers BOTH `class Child<T> : Base<T>` (T
        // already bound to e.g. Cell in the active subst) and the non-generic
        // `class ConcreteChild : Base<Cell>` (Cell is concrete in baseTypeRefs) —
        // the same per-base-arg map classStruct builds for inherited fields.
        const ast::TypeDecl* btd =
            base->decl ? dynamic_cast<const ast::TypeDecl*>(base->decl) : nullptr;
        if (btd && !btd->genericParams.empty() && bi < ts->baseTypeRefs.size() &&
            ts->baseTypeRefs[bi] &&
            btd->genericParams.size() == ts->baseTypeRefs[bi]->genericArgs.size()) {
            const sema::TypeRef* baseRef = ts->baseTypeRefs[bi];
            std::unordered_map<std::string_view, llvm::Type*> newSubst;
            std::unordered_map<std::string_view, const sema::TypeRef*> newSema;
            for (std::size_t gi = 0; gi < btd->genericParams.size(); ++gi) {
                // Resolve each base arg under the OUTER subst BEFORE installing
                // the new one (so `Base<T>` with the outer T=Cell maps to Cell).
                newSubst[btd->genericParams[gi].name] =
                    lowerTypeRef(baseRef->genericArgs[gi]);
                newSema[btd->genericParams[gi].name] =
                    resolveTypeParamRef(baseRef->genericArgs[gi]);
            }
            // #2019: compute the base's instance-field storage-type sequence under
            // BOTH the erased template layout (empty subst → i32 placeholders, what
            // disposerCoreFns_[base] was emitted with) and the concrete base subst.
            // If the two sequences differ, the template disposer's field offsets
            // are wrong for this instantiation and we MUST use a specialized core.
            // (Comparing the storageType sequence is exact: identical field types ⇒
            // identical offsets; llvm::Type* are context-unique so `==` is
            // structural.) collectInstanceStorage/storageType only read the subst —
            // no fieldIndex_ / classStruct side effects here.
            auto baseFieldLayout = [&]() {
                std::vector<const sema::Symbol*> storage;
                collectInstanceStorage(base, storage);
                std::vector<llvm::Type*> tys;
                tys.reserve(storage.size());
                for (const sema::Symbol* m : storage) tys.push_back(storageType(m));
                return tys;
            };
            auto savedSubst = genericSubst_;
            auto savedSema = genericSubstSemaTy_;
            genericSubst_.clear();
            genericSubstSemaTy_.clear();
            std::vector<llvm::Type*> tmplLayout = baseFieldLayout();
            genericSubst_ = std::move(newSubst);
            genericSubstSemaTy_ = std::move(newSema);
            std::vector<llvm::Type*> specLayout = baseFieldLayout();
            bool needSpec =
                classHasOwnedTypeParamField(base) || (tmplLayout != specLayout);
            llvm::Function* specCore =
                needSpec ? specializedDisposerCoreFor(base) : nullptr;
            genericSubst_ = std::move(savedSubst);
            genericSubstSemaTy_ = std::move(savedSema);
            if (specCore) {
                builder_.CreateCall(specCore, {currentThis_});
                return;
            }
        }
        auto it = disposerCoreFns_.find(base);
        if (it != disposerCoreFns_.end()) builder_.CreateCall(it->second,
                                                          {currentThis_});
        return;  // single base class
    }
}


void Emitter::disposeDisplacedOwnedClassLocal(const ast::Expr* target,
                                              llvm::Value* addr,
                                              llvm::Type* elemTy,
                                              llvm::Value* newVal) {
    // §28.7 (#913/#1054): reassigning an owned class LOCAL must dispose the
    // displaced value before overwriting (the class analog of the string/
    // closure/box/owned-collection field-store helpers). Sound owned-detection:
    // the target must be a plain local variable that `unwindOwnedLocalClass`
    // classifies as a definitely-owned plain class/interface local (`new C()` /
    // `owned`-returning initializer). That EXCLUDES a borrow-initialized local,
    // a collection/task/subscription local, and any param (borrow OR owned — a
    // ParameterSymbol is not a LocalDecl), so a borrowed value owned elsewhere
    // is never disposed here (no double-free). The scope-end / explicit-dispose
    // path re-loads the slot and disposes the NEW value; here we dispose the OLD
    // one — exactly once each.
    if (!addr) return;
    auto* id = dynamic_cast<const ast::IdentifierExpr*>(target);
    if (!id) return;  // field / index / static target — not a plain local
    auto* loc = dynamic_cast<sema::LocalSymbol*>(ctx_.exprSymbol(*id));
    if (!loc) return;  // a parameter (borrow or owned) — excluded
    auto* ld = dynamic_cast<const ast::LocalDecl*>(loc->decl);
    if (!ld || !unwindOwnedLocalClass(ld)) return;  // not an owned plain class local
    llvm::Value* old = builder_.CreateLoad(elemTy, addr, "reassign.old");
    if (!old->getType()->isPointerTy()) return;
    // Null-guard (a moved-out slot holds null) AND identity-guard (a self-
    // assignment `c := c` would otherwise dispose the value being stored → UAF).
    llvm::Value* nullp = llvm::ConstantPointerNull::get(ptrTy());
    llvm::Value* live = builder_.CreateICmpNE(old, nullp, "reassign.live");
    llvm::Value* doDispose = live;
    if (newVal && newVal->getType()->isPointerTy())
        doDispose = builder_.CreateAnd(
            live, builder_.CreateICmpNE(old, newVal, "reassign.diff"));
    auto* dispBB = llvm::BasicBlock::Create(llvm_, "reassign.dispose", currentFn_);
    auto* contBB = llvm::BasicBlock::Create(llvm_, "reassign.cont", currentFn_);
    builder_.CreateCondBr(doDispose, dispBB, contBB);
    builder_.SetInsertPoint(dispBB);
    emitDisposeAndReclaim(old);
    builder_.CreateBr(contBB);
    builder_.SetInsertPoint(contBB);
}


void Emitter::emitDisposals(const std::vector<Disposal>& scope, bool isUnwind) {
    for (auto it = scope.rbegin(); it != scope.rend(); ++it) {
        // #422 / §4.6 / §28.9: a provably-non-escaping `var` string local.
        // Release its single (producer) reference at scope end — on BOTH the
        // normal and the unwind exit (the reference dies either way; a given
        // exit goes one way only, so this runs exactly once per scope exit). The
        // local was proven non-escaping, so nothing else holds the pointer:
        // bz_string_release frees the data+header exactly once (no UAF, no
        // double-free). bz_string_release is null-safe and a no-op on a
        // sentinel/literal — but an eligible local always holds a heap +1 value.
        if (it->stringRelease) {
            llvm::Value* s = builder_.CreateLoad(ptrTy(), it->slot, "str.rel");
            emitStringRelease(s);
            continue;
        }
        // #422 / §27.5 / §28.9: a provably-non-escaping `var` closure
        // (function-typed) local. Release its single (producer) reference at
        // scope end — on BOTH the normal and unwind exit (the reference dies
        // either way; a given exit goes one way only, so this runs exactly once
        // per scope exit). The local was proven non-escaping, so nothing else
        // holds the cell pointer: bz_closure_release frees the cell + its env
        // block exactly once (no UAF, no double-free of the cell). Captured
        // values are borrows (§28.9), so the env's CONTENTS are not freed — no
        // double-dispose of a capture. bz_closure_release is null-safe.
        if (it->closureRelease) {
            llvm::Value* c = builder_.CreateLoad(ptrTy(), it->slot, "cl.rel");
            emitClosureRelease(c);
            continue;
        }
        // #1930 / stdlib §5 / §28.9: a provably-non-escaping `var g :=
        // <src>.GroupBy(sel).First()/…` local — the returned IGrouping of a bare-
        // GroupBy element-returning terminal. Its SIBLING groups + the outer array
        // were already reclaimed by the terminal (bz_groupby_free_groups_except),
        // keeping only this returned grouping; free it here (element array + key
        // buffer + group object) at scope end — on BOTH the normal and unwind exit
        // (the reference dies either way; a given exit goes one way only, so exactly
        // once per exit). The local was proven non-escaping, so nothing else holds
        // the pointer: bz_grouping_free frees the grouping exactly once (no UAF, no
        // double-free). bz_grouping_free is null-safe (an *OrNull empty result is
        // null → a no-op).
        if (it->grouping) {
            llvm::Value* g = builder_.CreateLoad(ptrTy(), it->slot, "grp.free");
            auto* gfree = declareC("bz_grouping_free",
                                   llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(gfree, {g});
            continue;
        }
        // #422 / §8.6 / §28.9: a provably-non-escaping `var` Object/interface
        // local holding a BOXED VALUE. Release its single (producer) reference at
        // scope end — on BOTH the normal and unwind exit (the reference dies
        // either way; a given exit goes one way only, so this runs exactly once
        // per scope exit). The local was proven non-escaping, so nothing else
        // holds the box pointer: bz_box_release frees the box wrapper exactly
        // once (no UAF, no double-free). The boxed value is a value-type leaf, so
        // only the wrapper is freed. This is NEVER a class instance (a class
        // coerced to Object is not a box producer, so it is never registered
        // here) — the class-instance ownership/disposal path is untouched.
        // bz_box_release is null-safe (a null T? boxes to a null Object).
        if (it->boxRelease) {
            llvm::Value* b = builder_.CreateLoad(ptrTy(), it->slot, "box.rel");
            emitBoxRelease(b);
            continue;
        }
        // fix #1747 / §28.9 / §9.7 / §28.3: a plain `var` RECORD local bound
        // from a record-returning CALL (`var p := Mk()`), proven non-escaping
        // by the conservative escape walk (StringEscapeWalker with
        // recordFieldReleaseMode — see LlvmBackend_Setup.cpp). fix #2448 /
        // §9.7 (v0.235): no callee-side retain mints this +1 — #2453 removed
        // the never-wired `emitRecordStringFieldRetains`/`currentReturnIsRecord_`
        // this comment used to credit with that mint (grep-confirmed dead:
        // neither was ever called/set). The actual soundness mechanism is
        // `recordCallResultReleaseEligible_` only admitting a local whose
        // initializer is PROVABLY a fresh, alias-free construction
        // (isFreshRecordConstructionOfExactType /
        // isProvablyFreshRecordFactoryMethod, LlvmBackend_Setup.cpp): each
        // string field's value already carries whatever +1/borrow state it
        // had AT CONSTRUCTION (a by-value return is a bit-copy that touches
        // no refcount), and that eligibility proof is what establishes this
        // local as the value's sole owner. At scope end — on
        // BOTH the normal and unwind exit (the fields die either way; a
        // given exit goes one way only, so each is released exactly once)
        // — every string field is bz_string_release'd and the slot nulled.
        // The local was proven non-escaping, so no alias survives the scope
        // (no UAF) and each field owns exactly one +1 (no double-free).
        // bz_string_release is null-/sentinel-safe so a literal field is a
        // no-op. `slot` is the record's struct alloca; `cls` is the record
        // TypeSymbol used to GEP each string storage member.
        if (it->recordStringFields) {
            emitRecordStringFieldReleases(it->slot, it->cls);
            continue;
        }
        // fix #2425 / §9.8 / §28.3/§28.9 / §35.4: the choice-side counterpart
        // just above — an already-populated deserialize component/payload
        // slot whose type is a value-like choice owning a string (directly
        // or through a nested record/choice payload), guarded against a
        // LATER sibling's read throwing. Runs on BOTH the normal and unwind
        // exit for the same reason `recordStringFields` does (the #2398/
        // #2425 registration is popped WITHOUT disposal once every required
        // component is confirmed present, so this only ever fires on the
        // throw path this scope exists to guard).
        if (it->choiceStringFields) {
            emitChoiceStringFieldReleases(it->slot, it->cls);
            continue;
        }
        // fix #2326 / §9.8 v0.228: the array-side mirror of
        // `choiceStringFields` just above — a non-nullable INLINE-ARRAY
        // deserialize component/payload slot whose element type carries
        // a string. The disposer's element TypeRef drives the per-element
        // dispatch (string → release; record → recurse; choice →
        // choice walker; nested array → recurse). Runs on BOTH the
        // normal and unwind exits, gated on the same `stringReleaseEligible_`
        // class as `stringRelease` / `recordStringFields` /
        // `choiceStringFields` — the registration is popped WITHOUT
        // disposal once every required component is confirmed present
        // (#2398 / #2425 disposal-scope semantics), so this only ever
        // fires on the throw path the scope exists to guard.
        if (it->inlineArrayStringFields) {
            emitInlineArrayStringReleases(it->slot, it->inlineArrayElemType);
            continue;
        }
        // #429 / §27.5: a boxed capture cell (shared heap allocation from
        // boxLocalSlot) whose owning lambda is proven non-escaping.  `slot`
        // is the ptr-alloca (newSlot) holding the cell pointer.  At scope end
        // we load the cell pointer and call free() on it.
        // ORDER: this entry is registered for the captured-local slot, which
        // was declared BEFORE the lambda local.  In reverse-decl order the
        // lambda local's closureRelease fires FIRST (LIFO), running the envdtor
        // which reads all values through the cell.  Only then does this entry
        // fire and free the cell itself — no UAF.
        // NULL SAFETY: the guarded boxing path null-initialises the cell-holder
        // in the function entry block; free(null) is a no-op (POSIX).
        // BOTH normal and unwind paths: the cell has no live external references
        // at this point (all lambdas referencing it are non-escaping and have
        // been released), so freeing on either path is safe.
        //
        // fix #2334: EXCEPT when skipCellFreeOnUnwind is set — the TaskScope.
        // Spawn special case's "non-escaping" proof depends on the bounding
        // `using var`'s disposal being quiescent, which holds only on the
        // properly-awaited NORMAL edge (see Disposal::skipCellFreeOnUnwind's
        // declaration for the full argument and the #1422 non-blocking-
        // zombie-drain citation). Skip the free on the unwind edge for such
        // an entry — the cell leaks there instead of a use-after-free.
        if (it->boxedCellFree) {
            if (isUnwind && it->skipCellFreeOnUnwind) continue;
            llvm::Value* cell =
                builder_.CreateLoad(ptrTy(), it->slot, "cell.ptr");
            builder_.CreateCall(freeFn(), {cell});
            // A guarded capture may be re-entered (for example, a case arm in
            // a loop). Null the holder after reclaim so the next entry allocates
            // a fresh cell instead of reusing freed storage.
            builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()),
                                 it->slot);
            continue;
        }
        // #1017: a DEFERRED boxed-cell free is handled in the SECOND pass below
        // (after every main-loop disposal in this scope), not here — the owner
        // disposal reads the object through the cell and must run first.
        if (it->boxedCellFreeDeferred) continue;
        // #383 / §31.1: the caught exception object. On a normal handled exit
        // it is freed (destructor + base chain + block, via its vtable); on the
        // unwind path it is SKIPPED — a re-raise/throw leaving the arm keeps it
        // in flight for the enclosing handler. It is non-null here (it was the
        // in-flight exception that matched this arm).
        if (it->caughtException) {
            if (isUnwind) continue;
            llvm::Value* obj = builder_.CreateLoad(ptrTy(), it->slot);
            // §30.4 / §30.9 (#854): a task-fault exception re-raised by `await t`
            // of a BOUND task is BORROWED from the task (codegen marked it via
            // bz_mark_exc_borrowed) — the awaiter does NOT own it, so a SECOND
            // `await t` of the same faulted task must still find a LIVE error_obj.
            // Skip the reclaim + unmark for a borrowed object; a normal (owned)
            // caught exception is reclaimed as before (§28.2/#380: idempotent
            // Dispose + base chain + block reclaim). Branch at runtime on the
            // per-object borrowed-set membership.
            llvm::Function* fn = builder_.GetInsertBlock()->getParent();
            auto* isBorrowedFn = declareC("bz_is_exc_borrowed",
                                          llvm::Type::getInt8Ty(llvm_), {ptrTy()});
            llvm::Value* bv =
                builder_.CreateCall(isBorrowedFn, {obj}, "exc.borrowed");
            llvm::Value* isB = builder_.CreateICmpNE(
                bv, llvm::ConstantInt::get(llvm::Type::getInt8Ty(llvm_), 0));
            auto* borrowBB = llvm::BasicBlock::Create(llvm_, "exc.borrow", fn);
            auto* ownedBB  = llvm::BasicBlock::Create(llvm_, "exc.owned", fn);
            auto* contBB   = llvm::BasicBlock::Create(llvm_, "exc.dispcont", fn);
            auto* drainBB = llvm::BasicBlock::Create(llvm_, "exc.eo.drain", fn);
            builder_.CreateCondBr(isB, borrowBB, ownedBB);
            // Borrowed: the fault is owned by the task, not this awaiter — the
            // awaiter must NOT reclaim it (a SECOND `await t` re-raises the same
            // live instance, §30.4). Unmark it; handling has now completed.
            // §30.9 in-flight-deferral (v0.179) terminal act: if the owning task
            // was disposed WHILE this fault was in flight (a faulted task bound
            // inside this `try`, unwound by its own fault — #1288 / #1197), its
            // scope-end recorded the exception for deferred disposal instead of
            // freeing it mid-dispatch. Perform that deferred dispose now, as the
            // terminal act of handling; otherwise leave the live instance to its
            // still-owning task's own scope-end.
            builder_.SetInsertPoint(borrowBB);
            auto* unmarkFn = declareC("bz_unmark_exc_borrowed",
                                      llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(unmarkFn, {obj});
            auto* isDeferredFn = declareC("bz_is_exc_deferred",
                                          llvm::Type::getInt8Ty(llvm_), {ptrTy()});
            llvm::Value* dv = builder_.CreateCall(isDeferredFn, {obj},
                                                  "exc.deferred");
            llvm::Value* isD = builder_.CreateICmpNE(
                dv, llvm::ConstantInt::get(llvm::Type::getInt8Ty(llvm_), 0));
            builder_.CreateCondBr(isD, drainBB, contBB);
            builder_.SetInsertPoint(drainBB);
            auto* clearFn = declareC("bz_clear_exc_deferred",
                                     llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(clearFn, {obj});
            emitDisposeAndReclaim(obj);
            builder_.CreateBr(contBB);
            // Owned: normal reclaim.
            builder_.SetInsertPoint(ownedBB);
            emitDisposeAndReclaim(obj);
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        // §15.3 / §15.1.1 (#747): a move-only owning inline-array local. Dispose
        // each non-null slot of the [N x ptr] alloca via the slot object's own
        // vtable (dynamic dispatch), then null the slot. Runs on BOTH normal and
        // unwind exit — the array as a whole carries no class Dispose (it is a
        // value type, not IDisposable), so unlike a plain owned class local
        // (unwindOnly below) it auto-disposes on normal exit too (§15.3 line 2021:
        // "a move-only local's owned slots are auto-disposed at scope end").
        // Only registered for locals proven never transferred out, so this is the
        // single disposal of each slot's object (no double-free).
        if (it->inlineOwnedArray) {
            auto* arrTy = llvm::ArrayType::get(ptrTy(), it->inlineSlotCount);
            for (unsigned i = 0; i < it->inlineSlotCount; ++i) {
                llvm::Value* slotAddr = builder_.CreateInBoundsGEP(
                    arrTy, it->slot,
                    {builder_.getInt32(0), builder_.getInt32(i)}, "ioa.slot");
                llvm::Value* obj = builder_.CreateLoad(ptrTy(), slotAddr, "ioa.val");
                llvm::Value* isNull = builder_.CreateICmpEQ(
                    obj, llvm::ConstantPointerNull::get(ptrTy()));
                auto* dispBB = llvm::BasicBlock::Create(llvm_, "ioa.dispose",
                                                        currentFn_);
                auto* contBB = llvm::BasicBlock::Create(llvm_, "ioa.cont",
                                                        currentFn_);
                builder_.CreateCondBr(isNull, contBB, dispBB);
                builder_.SetInsertPoint(dispBB);
                emitDisposeAndReclaim(obj);
                builder_.CreateStore(
                    llvm::ConstantPointerNull::get(ptrTy()), slotAddr);
                builder_.CreateBr(contBB);
                builder_.SetInsertPoint(contBB);
            }
            continue;
        }
        // §15.3 / §27.5 (#782): a CAPTURED move-only owning inline-array — its
        // [N x ptr] value lives in the boxed cell (boxLocalSlot copied it there).
        // `slot` is the cell-HOLDER ptr-alloca; load the cell pointer, then dispose
        // each non-null element by GEPing the [N x ptr] over the CELL (heap), and
        // null the slot. The cell itself is freed separately by the boxedCellFree
        // that fires AFTER this. Non-escaping-gated at registration so the cell is
        // alive here and no escaping closure still references the elements.
        if (it->inlineOwnedArrayCell) {
            llvm::Value* cell =
                builder_.CreateLoad(ptrTy(), it->slot, "ioac.cell");
            auto* cellNull = builder_.CreateICmpEQ(
                cell, llvm::ConstantPointerNull::get(ptrTy()));
            auto* liveBB = llvm::BasicBlock::Create(llvm_, "ioac.live", currentFn_);
            auto* afterBB = llvm::BasicBlock::Create(llvm_, "ioac.after", currentFn_);
            builder_.CreateCondBr(cellNull, afterBB, liveBB);
            builder_.SetInsertPoint(liveBB);
            auto* arrTy = llvm::ArrayType::get(ptrTy(), it->inlineSlotCount);
            for (unsigned i = 0; i < it->inlineSlotCount; ++i) {
                llvm::Value* slotAddr = builder_.CreateInBoundsGEP(
                    arrTy, cell,
                    {builder_.getInt32(0), builder_.getInt32(i)}, "ioac.slot");
                llvm::Value* obj =
                    builder_.CreateLoad(ptrTy(), slotAddr, "ioac.val");
                llvm::Value* isNull = builder_.CreateICmpEQ(
                    obj, llvm::ConstantPointerNull::get(ptrTy()));
                auto* dispBB = llvm::BasicBlock::Create(llvm_, "ioac.dispose",
                                                        currentFn_);
                auto* contBB = llvm::BasicBlock::Create(llvm_, "ioac.cont",
                                                        currentFn_);
                builder_.CreateCondBr(isNull, contBB, dispBB);
                builder_.SetInsertPoint(dispBB);
                emitDisposeAndReclaim(obj);
                builder_.CreateStore(
                    llvm::ConstantPointerNull::get(ptrTy()), slotAddr);
                builder_.CreateBr(contBB);
                builder_.SetInsertPoint(contBB);
            }
            builder_.CreateBr(afterBB);
            builder_.SetInsertPoint(afterBB);
            continue;
        }
        // §28.3 (line 4910): a plain `var` owned local / `owned` parameter. It is
        // auto-disposed ONLY when an exception unwinds out of its scope; on a
        // normal exit it must have reached a discharge point (sema-enforced), so
        // disposing it here would double-dispose a moved-out / returned value.
        // On the unwind path: dispatch its idempotent Dispose and reclaim the
        // block once, null-guarded (an uninitialized/conditional owned local's
        // slot may be null — registration is restricted to definitely-assigned,
        // never-moved/disposed owned locals/params, but the null-check is kept
        // for the nullable-owned and defensive cases). The dispatch is dynamic
        // (runtime class), matching every other class-instance dispose site.
        // #2000 / §28.13 / §28.4 / §28.6: an `unwindOnly` entry whose type is a
        // compiler-known builtin owned handle (ChannelSender<T> / Channel<T> /
        // Shared<T> / Weak<T>) — released via a TYPE-SPECIFIC runtime call
        // (bz_channel_sender_release / bz_channel_dispose / bz_shared_release /
        // bz_weak_release) rather than the generic vtable slot 2, which does
        // not touch the handle refcount and cannot close the channel. After the
        // release the slot is NULLed — same dispose-exactly-once pattern as the
        // class path below — so a sibling / outer disposal registered against
        // the same slot reads null on its own (later, in reverse-declaration-
        // order iteration) unwind path and skips. The unbox is load → null-
        // check → release → store null, identical shape to the class path.
        if (it->unwindOnly) {
            if (!isUnwind) continue;
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* obj = builder_.CreateLoad(ptrTy(), addr);
            llvm::Value* isNull = builder_.CreateICmpEQ(
                obj, llvm::ConstantPointerNull::get(ptrTy()));
            auto* dispBB = llvm::BasicBlock::Create(llvm_, "unwind.dispose",
                                                    currentFn_);
            auto* contBB = llvm::BasicBlock::Create(llvm_, "unwind.cont",
                                                    currentFn_);
            builder_.CreateCondBr(isNull, contBB, dispBB);
            builder_.SetInsertPoint(dispBB);
            if (it->sharedHandle) {
                auto* fn = declareC("bz_shared_release",
                                    llvm::Type::getVoidTy(llvm_), {ptrTy()});
                builder_.CreateCall(fn, {obj});
            } else if (it->channel) {
                auto* fn = declareC("bz_channel_dispose",
                                    llvm::Type::getVoidTy(llvm_), {ptrTy()});
                builder_.CreateCall(fn, {obj});
            } else if (it->channelSender) {
                auto* fn = declareC("bz_channel_sender_release",
                                    llvm::Type::getVoidTy(llvm_), {ptrTy()});
                builder_.CreateCall(fn, {obj});
            } else if (it->weakHandle) {
                auto* fn = declareC("bz_weak_release",
                                    llvm::Type::getVoidTy(llvm_), {ptrTy()});
                builder_.CreateCall(fn, {obj});
            } else {
                emitDisposeAndReclaim(obj);
            }
            builder_.CreateStore(
                llvm::ConstantPointerNull::get(ptrTy()), addr);
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        // per §2.11: a `using var` Subscription auto-disposes by removing its
        // registration through the events runtime — it has no vtable. (Dispose
        // is idempotent, so a null/already-removed handle is safe.)
        if (it->subscription) {
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* sub = builder_.CreateLoad(ptrTy(), addr);
            auto* fn = declareC("bz_subscription_dispose",
                                llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(fn, {sub});
            continue;
        }
        // #362 / stdlib §4.5: a `using var` heap collection. Free the backing
        // via bz_array_free; for an owning collection of class elements the
        // element-disposer thunk cascades disposal into each (non-null) element
        // exactly once (the thunk null-checks; the array iterates live slots).
        if (it->collection) {
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* arr = builder_.CreateLoad(ptrTy(), addr);
            llvm::Value* disp = it->collectionElemDisposer
                ? llvm::cast<llvm::Value>(it->collectionElemDisposer)
                : llvm::ConstantPointerNull::get(ptrTy());
            auto* fn = declareC("bz_array_free",
                                llvm::Type::getVoidTy(llvm_),
                                {ptrTy(), ptrTy()});
            builder_.CreateCall(fn, {arr, disp});
            continue;
        }
        // #394 / stdlib §4.9: a `using var` hash-backed dictionary. Free the
        // backing table (and its malloc'd key bytes) via bz_dict_free; for an
        // OwningDictionary with a class VALUE type the value-disposer thunk
        // cascades disposal into each surviving (non-null) value exactly once
        // (bz_dict_free iterates the live entries; the thunk null-checks). A
        // value DISPLACED earlier by the indexer setter (#388) was already
        // removed from the table and disposed there, so it is not re-disposed
        // here — no double-free. bz_dict_free is null-safe.
        // #429 / §28.9 / §4.10: a Set<string> stores its string elements as
        // KEYS (not values) in the generic-key table. Before bz_dict_free frees
        // each key copy, emit bz_set_string_keys_release to call
        // bz_string_release on every live key's string pointer. This discharges
        // the set's co-owned +1 per element (retained at Add). bz_dict_free is
        // then called with a null value disposer (sentinels, nothing to dispose).
        if (it->dict) {
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* tbl = builder_.CreateLoad(ptrTy(), addr);
            if (it->dictStringKeys) {
                // Set<string>: release each string element's +1 before freeing.
                auto* relFn = declareC("bz_set_string_keys_release",
                                       llvm::Type::getVoidTy(llvm_), {ptrTy()});
                builder_.CreateCall(relFn, {tbl});
            }
            llvm::Value* disp = it->dictValueDisposer
                ? llvm::cast<llvm::Value>(it->dictValueDisposer)
                : llvm::ConstantPointerNull::get(ptrTy());
            auto* fn = declareC("bz_dict_free",
                                llvm::Type::getVoidTy(llvm_),
                                {ptrTy(), ptrTy()});
            builder_.CreateCall(fn, {tbl, disp});
            continue;
        }
        // per §4.6/§4.7: a `using var` LinkedList<T> / OwningLinkedList<T>.
        // Free the node chain via bz_llist_free; for an OwningLinkedList<T> of
        // class/interface elements the element-disposer thunk cascades disposal
        // into each (non-null) element exactly once (same policy as OwningArray —
        // but on the node value pointer, not an array slot).
        // #602 batch 3/4: owningCollectionElemDisposer deleted; d.collectionElemDisposer
        // was set from elementPolicyFor at registerLocal time (covers class, interface,
        // Shared<T>, Weak<T> elements per §28.11 v0.135).
        if (it->linkedList) {
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* list = builder_.CreateLoad(ptrTy(), addr);
            llvm::Value* disp = it->collectionElemDisposer
                ? llvm::cast<llvm::Value>(it->collectionElemDisposer)
                : llvm::ConstantPointerNull::get(ptrTy());
            auto* fn = declareC("bz_llist_free",
                                llvm::Type::getVoidTy(llvm_),
                                {ptrTy(), ptrTy()});
            builder_.CreateCall(fn, {list, disp});
            continue;
        }
        // §2.15 / issue #550: a `using var` BinarySerializer — free the
        // serializer buffer + state via bz_binser_free.
        if (it->binarySerializer) {
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* h = builder_.CreateLoad(ptrTy(), addr);
            llvm::Value* isNull = builder_.CreateICmpEQ(
                h, llvm::ConstantPointerNull::get(ptrTy()));
            auto* frBB   = llvm::BasicBlock::Create(llvm_, "binser.free",  currentFn_);
            auto* contBB = llvm::BasicBlock::Create(llvm_, "binser.cont",  currentFn_);
            builder_.CreateCondBr(isNull, contBB, frBB);
            builder_.SetInsertPoint(frBB);
            auto* fn = declareC("bz_binser_free",
                                llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(fn, {h});
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        // §2.15 / issue #550: a `using var` BinaryDeserializer — free the
        // deserializer cursor state via bz_binder_free.
        if (it->binaryDeserializer) {
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* h = builder_.CreateLoad(ptrTy(), addr);
            llvm::Value* isNull = builder_.CreateICmpEQ(
                h, llvm::ConstantPointerNull::get(ptrTy()));
            auto* frBB   = llvm::BasicBlock::Create(llvm_, "binder.free",  currentFn_);
            auto* contBB = llvm::BasicBlock::Create(llvm_, "binder.cont",  currentFn_);
            builder_.CreateCondBr(isNull, contBB, frBB);
            builder_.SetInsertPoint(frBB);
            auto* fn = declareC("bz_binder_free",
                                llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(fn, {h});
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        // §2.15 / issue #550: a `using var` JsonSerializer — free via
        // bz_binser_free (same as BinarySerializer; kind field differentiates).
        if (it->jsonSerializer) {
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* h = builder_.CreateLoad(ptrTy(), addr);
            llvm::Value* isNull = builder_.CreateICmpEQ(
                h, llvm::ConstantPointerNull::get(ptrTy()));
            auto* frBB   = llvm::BasicBlock::Create(llvm_, "jser.free",  currentFn_);
            auto* contBB = llvm::BasicBlock::Create(llvm_, "jser.cont",  currentFn_);
            builder_.CreateCondBr(isNull, contBB, frBB);
            builder_.SetInsertPoint(frBB);
            auto* fn = declareC("bz_binser_free",
                                llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(fn, {h});
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        // §2.15 / issue #550: a `using var` JsonDeserializer — free via
        // bz_binder_free (same as BinaryDeserializer; kind field differentiates).
        if (it->jsonDeserializer) {
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* h = builder_.CreateLoad(ptrTy(), addr);
            llvm::Value* isNull = builder_.CreateICmpEQ(
                h, llvm::ConstantPointerNull::get(ptrTy()));
            auto* frBB   = llvm::BasicBlock::Create(llvm_, "jder.free",  currentFn_);
            auto* contBB = llvm::BasicBlock::Create(llvm_, "jder.cont",  currentFn_);
            builder_.CreateCondBr(isNull, contBB, frBB);
            builder_.SetInsertPoint(frBB);
            auto* fn = declareC("bz_binder_free",
                                llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(fn, {h});
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        // §8.1 / issue #646: a `using var` Stopwatch. Free the timing struct
        // via bz_stopwatch_free. Null-guarded for consistency with other
        // builtin disposal sites.
        if (it->stopwatch) {
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* sw = builder_.CreateLoad(ptrTy(), addr);
            llvm::Value* isNull = builder_.CreateICmpEQ(
                sw, llvm::ConstantPointerNull::get(ptrTy()));
            auto* frBB   = llvm::BasicBlock::Create(llvm_, "sw.free",  currentFn_);
            auto* contBB = llvm::BasicBlock::Create(llvm_, "sw.cont",  currentFn_);
            builder_.CreateCondBr(isNull, contBB, frBB);
            builder_.SetInsertPoint(frBB);
            auto* fn = declareC("bz_stopwatch_free",
                                llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(fn, {sw});
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        // stdlib §7.2 / issue #645: a `using var` FileStream. Dispose via
        // bz_filestream_dispose — flushes buffered writes, closes the OS handle
        // and frees the handle object. Null-guarded like the sites above: an
        // explicit `fs.Dispose()` earlier in the scope nulls the slot, and a
        // failed open stores null, so the guard is what makes both cases a
        // no-op rather than a second dispose.
        if (it->fileStream) {
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* fs = builder_.CreateLoad(ptrTy(), addr);
            llvm::Value* isNull = builder_.CreateICmpEQ(
                fs, llvm::ConstantPointerNull::get(ptrTy()));
            auto* frBB   = llvm::BasicBlock::Create(llvm_, "fs.dispose", currentFn_);
            auto* contBB = llvm::BasicBlock::Create(llvm_, "fs.cont",    currentFn_);
            builder_.CreateCondBr(isNull, contBB, frBB);
            builder_.SetInsertPoint(frBB);
            auto* fn = declareC("bz_filestream_dispose",
                                llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(fn, {fs});
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        // §6.1 / §30.8: a `using var` CancellationTokenSource. Dispose via
        // bz_cts_dispose (idempotent: signals all tokens, then frees the source).
        // null-guarded: bz_cts_dispose handles null internally (its disposed flag
        // prevents double-free), but we guard anyway for consistency with other
        // builtin disposal sites.
        if (it->cancellationTokenSource) {
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* cts = builder_.CreateLoad(ptrTy(), addr);
            auto* fn = declareC("bz_cts_dispose",
                                llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(fn, {cts});
            continue;
        }
        // §28.13 / stdlib §2.16 (#603): a `using var` Shared<T> handle.
        // Null-guarded: bz_shared_release handles null gracefully, but we load
        // and call only when non-null for consistency with other disposal sites.
        if (it->sharedHandle) {
            // #1957: an `owned` Shared<T> PARAMETER discharged on the normal path
            // releases here only on unwind (the discharge site owns the normal
            // exit). A `using var` / auto-dropped owner leaves the flag false and
            // releases on both exits.
            if (it->releaseOnUnwindOnly && !isUnwind) continue;
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* h = builder_.CreateLoad(ptrTy(), addr);
            llvm::Value* isNull = builder_.CreateICmpEQ(
                h, llvm::ConstantPointerNull::get(ptrTy()));
            auto* relBB  = llvm::BasicBlock::Create(llvm_, "shared.rel",  currentFn_);
            auto* contBB = llvm::BasicBlock::Create(llvm_, "shared.cont", currentFn_);
            builder_.CreateCondBr(isNull, contBB, relBB);
            builder_.SetInsertPoint(relBB);
            auto* fn = declareC("bz_shared_release",
                                llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(fn, {h});
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        // §28.13 / stdlib §2.16 (#603): a `using var` Weak<T> handle.
        if (it->weakHandle) {
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* h = builder_.CreateLoad(ptrTy(), addr);
            llvm::Value* isNull = builder_.CreateICmpEQ(
                h, llvm::ConstantPointerNull::get(ptrTy()));
            auto* relBB  = llvm::BasicBlock::Create(llvm_, "weak.rel",  currentFn_);
            auto* contBB = llvm::BasicBlock::Create(llvm_, "weak.cont", currentFn_);
            builder_.CreateCondBr(isNull, contBB, relBB);
            builder_.SetInsertPoint(relBB);
            auto* fn = declareC("bz_weak_release",
                                llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(fn, {h});
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        // stdlib §6.7 / §30.5: a `using var` Task / Task<T>. At scope end, the
        // task's Dispose() runs "but does not block waiting for completion" (§30.5).
        // We implement this via bz_task_fire_and_forget, which registers a
        // continuation that calls bz_task_free when the task eventually completes.
        // This is safe for both already-complete tasks (the continuation fires
        // immediately via bz_task_then's fast path) and still-running tasks
        // (the task struct remains live until the continuation fires). The
        // alternative of calling bz_task_free directly at scope end would be
        // unsafe if the task is still running on the thread pool (UAF). Null-
        // guarded: bz_task_fire_and_forget tolerates a null task gracefully (no
        // continuation is registered for null). Double-free safety: lowerAwait only
        // frees tasks whose operand is an anonymous CallExpr (`await Producer()`),
        // never a named local (`await t`), so a using-var-bound task is fired-
        // and-forgotten exactly once here at scope end.
        if (it->task) {
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* t = builder_.CreateLoad(ptrTy(), addr);
            llvm::Value* isNull = builder_.CreateICmpEQ(
                t, llvm::ConstantPointerNull::get(ptrTy()));
            auto* fafBB = llvm::BasicBlock::Create(llvm_, "task.faf", currentFn_);
            auto* contBB = llvm::BasicBlock::Create(llvm_, "task.cont", currentFn_);
            builder_.CreateCondBr(isNull, contBB, fafBB);
            builder_.SetInsertPoint(fafBB);
            // §30.9 fault ownership (v0.179): a faulted task OWNS its fault
            // exception and disposes it exactly once at task disposal — the task
            // LOCAL's scope-end here (bz_task_free / fire-and-forget frees the task
            // struct + its buffers but NOT the Bezel exception object; only Bezel
            // codegen can run its destructor). Three paths:
            //   • BORROWED aggregate (§30.10 / #1529): a WhenAll/WhenAll<T>
            //     aggregate never owns its error_obj (only borrows a reference
            //     from the input task that does) — skip unconditionally; see
            //     bz_task_fault_borrowed below.
            //   • NOT in flight (not borrowed): the fault was already fully handled
            //     (or never awaited) — dispose the exception now.
            //   • IN FLIGHT (borrowed by an awaiter mid-dispatch): a faulted task
            //     bound INSIDE a `try` is unwound by its OWN re-raised fault while
            //     the exception is still routing to the enclosing catch (#1288 /
            //     #1197). Disposing it here would free an object the catch arm's
            //     type-test / binding still reads (UAF / double-free). Instead
            //     record it for deferred disposal (bz_defer_exc_dispose); the catch
            //     arm's normal exit performs the dispose as its terminal act (the
            //     borrowed branch above drains bz_is_exc_deferred). Refinement (a).
            // (A still-running async task is not yet errored here → skipped; its
            // unobserved-fault error_obj is a separate residual, #1021.) A
            // non-faulted task has a null error_obj → skipped. bz_task_free below
            // ignores error_obj, so the now-dangling task->error_obj is never read.
            {
                auto* isErrFn = declareC("bz_task_is_error", i1(), {ptrTy()});
                llvm::Value* err = builder_.CreateCall(isErrFn, {t}, "task.err");
                auto* edBB = llvm::BasicBlock::Create(llvm_, "task.errdisp",
                                                      currentFn_);
                auto* eaBB = llvm::BasicBlock::Create(llvm_, "task.errcont",
                                                      currentFn_);
                builder_.CreateCondBr(err, edBB, eaBB);
                builder_.SetInsertPoint(edBB);
                // §30.10 / #1529: a WhenAll/WhenAll<T> AGGREGATE task whose
                // error_obj was propagated from an input task's OWNED fault is
                // only BORROWING that reference (spec §30.10: "an aggregate
                // task owns no fault of its own"). Its own scope-end must never
                // reclaim it — the input task remains the sole owner and
                // disposes it at ITS OWN, independent scope end. Skip entirely,
                // unconditionally: without this check, a bound aggregate whose
                // fault was already unmarked-not-deferred by a catch arm (the
                // "owned" branch below) would free the exception out from under
                // the still-live input task, which then double-frees it at its
                // own disposal (the crash this fix closes).
                auto* fbFn = declareC("bz_task_fault_borrowed",
                                      llvm::Type::getInt8Ty(llvm_), {ptrTy()});
                llvm::Value* fb = builder_.CreateCall(fbFn, {t}, "task.faultborrowed");
                llvm::Value* isFB = builder_.CreateICmpNE(
                    fb, llvm::ConstantInt::get(llvm::Type::getInt8Ty(llvm_), 0));
                auto* borrowedAggBB = llvm::BasicBlock::Create(
                    llvm_, "task.eo.aggborrowed", currentFn_);
                auto* checkBB = llvm::BasicBlock::Create(llvm_, "task.eo.check",
                                                         currentFn_);
                builder_.CreateCondBr(isFB, borrowedAggBB, checkBB);
                builder_.SetInsertPoint(borrowedAggBB);
                builder_.CreateBr(eaBB);
                builder_.SetInsertPoint(checkBB);
                auto* eoFn = declareC("bz_task_error_obj", ptrTy(), {ptrTy()});
                llvm::Value* eo = builder_.CreateCall(eoFn, {t}, "task.errobj");
                llvm::Value* eoNull = builder_.CreateICmpEQ(
                    eo, llvm::ConstantPointerNull::get(ptrTy()));
                auto* doBB = llvm::BasicBlock::Create(llvm_, "task.eodisp",
                                                      currentFn_);
                builder_.CreateCondBr(eoNull, eaBB, doBB);
                builder_.SetInsertPoint(doBB);
                // In flight (borrowed) → defer; else dispose now.
                auto* isBorrowedFn = declareC("bz_is_exc_borrowed",
                                              llvm::Type::getInt8Ty(llvm_), {ptrTy()});
                llvm::Value* bv = builder_.CreateCall(isBorrowedFn, {eo},
                                                      "task.eo.borrowed");
                llvm::Value* isB = builder_.CreateICmpNE(
                    bv, llvm::ConstantInt::get(llvm::Type::getInt8Ty(llvm_), 0));
                auto* deferBB = llvm::BasicBlock::Create(llvm_, "task.eo.defer",
                                                         currentFn_);
                auto* dispBB  = llvm::BasicBlock::Create(llvm_, "task.eo.dispnow",
                                                         currentFn_);
                builder_.CreateCondBr(isB, deferBB, dispBB);
                builder_.SetInsertPoint(deferBB);
                auto* deferFn = declareC("bz_defer_exc_dispose",
                                         llvm::Type::getVoidTy(llvm_), {ptrTy()});
                builder_.CreateCall(deferFn, {eo});
                builder_.CreateBr(eaBB);
                builder_.SetInsertPoint(dispBB);
                emitDisposeAndReclaim(eo);
                builder_.CreateBr(eaBB);
                builder_.SetInsertPoint(eaBB);
            }
            // §30.5 / #1258, #1503: attach the per-T disposer for the
            // produced-but-unconsumed owned class result of an OwningTask
            // <T:class> BEFORE the task is reclaimed below. bz_task_free
            // (called either synchronously by the fast path just below, when
            // the task is already done, or later by bz_task_fire_and_forget's
            // completion continuation when it is not) is what actually runs
            // the disposer, guarded so it never double-frees a result an
            // awaiter already took (a consuming await/`.Result` nulls the
            // result slot first via bz_task_take_result) and never fires on a
            // faulted task (no result was produced).
            //
            // #1503: this used to call bz_task_dispose_owned_result directly
            // here, which only ever disposed a result already sitting in the
            // task's buffer AT THIS EXACT SYNCHRONOUS POINT — a task still
            // running at this scope-end drop (task->result not yet set) saw a
            // no-op, and the per-T disposer was never retained anywhere, so
            // its eventual result leaked once bz_task_fire_and_forget's
            // continuation later called plain bz_task_free. Persisting the
            // disposer ON THE TASK via bz_task_set_result_disposer instead
            // means it survives to whichever bz_task_free call actually
            // reclaims the task, matching §30.5's "the Dispose() call
            // attaches a continuation that disposes the produced result
            // whenever it appears" for a not-yet-settled task.
            // §30.15 (v0.179, #1524): a Channel<T>.Receive() task overrides the
            // ordinary "attach a disposer, then fire-and-forget" sequence below.
            // The synchronous already-faulted-at-scope-end exception disposal
            // above (the `if (err) {...}` block) still applies unchanged — it is
            // about the fault OBJECT, orthogonal to the channel ITEM the give-
            // back protocol is about. What must NOT happen for a Receive() task
            // is (a) attaching a result_disposer that would eventually DESTROY a
            // produced item (bz_task_free's default for an unconsumed owned
            // result), or (b) plain fire-and-forget's continuation silently
            // dropping a not-yet-produced item once it arrives. Both are exactly
            // what spec §30.15 calls "the one place §30.5's dispose-continuation
            // semantics do not apply to a pending result": instead
            // bz_channel_receive_abandon deregisters a still-parked waiter, or
            // gives a produced item back to the channel's head, so "no message
            // is consumed or lost."
            if (it->channelReceiveTask) {
                auto* fn = declareC("bz_channel_receive_abandon",
                                    llvm::Type::getVoidTy(llvm_), {ptrTy()});
                builder_.CreateCall(fn, {t});
                builder_.CreateBr(contBB);
                builder_.SetInsertPoint(contBB);
                continue;
            }
            if (it->taskOwnedResultDisposer) {
                auto* setDispFn = declareC("bz_task_set_result_disposer",
                                           llvm::Type::getVoidTy(llvm_),
                                           {ptrTy(), ptrTy()});
                builder_.CreateCall(setDispFn, {t, it->taskOwnedResultDisposer});
            }
            // §30.10 (v0.179 / #1520): in checked disposal mode, route this
            // abandonment through bz_task_fire_and_forget_checked instead of
            // the plain entry point. If the task is STILL RUNNING at this
            // scope-end drop (the genuinely-abandoned-continuation case the
            // scope-end error_obj handling above deliberately skips, per the
            // #1021 comment), its eventual continuation may resume past this
            // point and touch state this scope's OTHER disposals just tore
            // down — the §30.1 source-outlives-settlement violation this
            // whole backstop exists for. Spec §30.10: "a DisposedAccessException
            // raised in an abandoned continuation is first reported through
            // the same implementation-defined diagnostic channel as
            // FireAndForget's logging, then swallowed." An unchecked build
            // never emits the disposed-flag guard that raises
            // DisposedAccessException in the first place, so the plain
            // (unchanged) entry point is correct and zero-cost there.
            if (checkedDisposal_) {
                llvm::Value* daVt = llvm::ConstantPointerNull::get(ptrTy());
                { auto vit = builtinVtable_.find("DisposedAccessException");
                  if (vit != builtinVtable_.end()) daVt = vit->second; }
                auto* fn = declareC("bz_task_fire_and_forget_checked",
                                    llvm::Type::getVoidTy(llvm_),
                                    {ptrTy(), ptrTy()});
                builder_.CreateCall(fn, {t, daVt});
            } else {
                auto* fn = declareC("bz_task_fire_and_forget",
                                    llvm::Type::getVoidTy(llvm_), {ptrTy()});
                builder_.CreateCall(fn, {t});
            }
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        // §6.6 / issue #974: a `using var` TaskCompletionSource<T> /
        // OwningTaskCompletionSource<T> / TaskCompletionSource.
        // Disposal sequence (all null-guarded; runs on both normal and unwind):
        //   1. bz_tcs_fault_if_pending(tcs, ioe_vtable): if the inner task has not
        //      been completed yet, fault it with InvalidOperationException so that
        //      any awaiter wakes up with an error rather than hanging indefinitely.
        //   2. Error-obj cleanup on the inner task (mirrors the d.task arm above):
        //      if the task is faulted, dispose the error object exactly once.
        //   3. bz_task_fire_and_forget(inner_task): release the task struct when it
        //      has finished running (immediate if already done, deferred if running).
        //   4. bz_tcs_free(tcs): free the bz_tcs_t wrapper itself.
        if (it->tcs) {
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* tcs = builder_.CreateLoad(ptrTy(), addr, "tcs.load");
            llvm::Value* isNull = builder_.CreateICmpEQ(
                tcs, llvm::ConstantPointerNull::get(ptrTy()));
            auto* dispBB  = llvm::BasicBlock::Create(llvm_, "tcs.dispose",  currentFn_);
            auto* contBB  = llvm::BasicBlock::Create(llvm_, "tcs.cont",     currentFn_);
            builder_.CreateCondBr(isNull, contBB, dispBB);
            builder_.SetInsertPoint(dispBB);

            // Step 1: fault the inner task if not yet completed.
            llvm::Value* ioeVt = llvm::ConstantPointerNull::get(ptrTy());
            {   auto vit = builtinVtable_.find("InvalidOperationException");
                if (vit != builtinVtable_.end()) ioeVt = vit->second;
            }
            auto* faultFn = declareC("bz_tcs_fault_if_pending",
                                      llvm::Type::getVoidTy(llvm_), {ptrTy(), ptrTy()});
            builder_.CreateCall(faultFn, {tcs, ioeVt});

            // Step 2: get the inner task pointer and clean up its error object.
            auto* innerFn = declareC("bz_tcs_task", ptrTy(), {ptrTy()});
            llvm::Value* t = builder_.CreateCall(innerFn, {tcs}, "tcs.inner");
            {
                auto* isErrFn = declareC("bz_task_is_error", i1(), {ptrTy()});
                llvm::Value* err = builder_.CreateCall(isErrFn, {t}, "tcs.task.err");
                auto* edBB = llvm::BasicBlock::Create(llvm_, "tcs.task.errdisp",
                                                       currentFn_);
                auto* eaBB = llvm::BasicBlock::Create(llvm_, "tcs.task.errcont",
                                                       currentFn_);
                builder_.CreateCondBr(err, edBB, eaBB);
                builder_.SetInsertPoint(edBB);
                auto* eoFn = declareC("bz_task_error_obj", ptrTy(), {ptrTy()});
                llvm::Value* eo = builder_.CreateCall(eoFn, {t}, "tcs.task.errobj");
                llvm::Value* eoNull = builder_.CreateICmpEQ(
                    eo, llvm::ConstantPointerNull::get(ptrTy()));
                auto* doBB = llvm::BasicBlock::Create(llvm_, "tcs.task.eodisp",
                                                       currentFn_);
                builder_.CreateCondBr(eoNull, eaBB, doBB);
                builder_.SetInsertPoint(doBB);
                // §30.9 in-flight-deferral (v0.179): if the fault is still in
                // flight (borrowed by an awaiter mid-dispatch — a TCS-backed task
                // bound inside a `try`, unwound by its own re-raised fault),
                // defer its disposal to the catch arm's terminal act rather than
                // freeing the object mid-dispatch (UAF). Otherwise dispose now.
                auto* isBorrowedFn = declareC("bz_is_exc_borrowed",
                                              llvm::Type::getInt8Ty(llvm_), {ptrTy()});
                llvm::Value* bv = builder_.CreateCall(isBorrowedFn, {eo},
                                                      "tcs.eo.borrowed");
                llvm::Value* isB = builder_.CreateICmpNE(
                    bv, llvm::ConstantInt::get(llvm::Type::getInt8Ty(llvm_), 0));
                auto* deferBB = llvm::BasicBlock::Create(llvm_, "tcs.eo.defer",
                                                         currentFn_);
                auto* dispBB  = llvm::BasicBlock::Create(llvm_, "tcs.eo.dispnow",
                                                         currentFn_);
                builder_.CreateCondBr(isB, deferBB, dispBB);
                builder_.SetInsertPoint(deferBB);
                auto* deferFn = declareC("bz_defer_exc_dispose",
                                         llvm::Type::getVoidTy(llvm_), {ptrTy()});
                builder_.CreateCall(deferFn, {eo});
                builder_.CreateBr(eaBB);
                builder_.SetInsertPoint(dispBB);
                emitDisposeAndReclaim(eo);
                builder_.CreateBr(eaBB);
                builder_.SetInsertPoint(eaBB);
            }

            // Step 2.5: §28.3 / #1250: for OwningTaskCompletionSource<T:class>, dispose
            // the inner task's owned result if set but never consumed via .Result/await.
            // bz_tcs_dispose_owned_result is safe on all paths:
            //   • owned_result_disposer == null → plain TCS or value-type; no-op.
            //   • task->is_error → TrySetException or fault_if_pending fired; no result.
            //   • task->result == null → bz_task_take_result already consumed it; no-op.
            // Must run BEFORE fire-and-forget while inner_task is still alive.
            {
                auto* dispOwnedFn = declareC("bz_tcs_dispose_owned_result",
                                              llvm::Type::getVoidTy(llvm_), {ptrTy()});
                builder_.CreateCall(dispOwnedFn, {tcs});
            }

            // Step 3: fire-and-forget the inner task.
            auto* fafFn = declareC("bz_task_fire_and_forget",
                                    llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(fafFn, {t});

            // Step 4: free the TCS struct itself.
            auto* freeFn2 = declareC("bz_tcs_free",
                                      llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(freeFn2, {tcs});

            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        // §6.5 / §24.3 (#2026): a `using var` EventSource<T>/AsyncEventSource<T>
        // — a bz_event_source_t* with no vtable. Free the subscriber list + struct
        // via bz_event_source_free (null-safe: `if (!s) return;`, so a ctor that
        // threw before the local was assigned, or a moved-out slot, is safe). The
        // freed source does NOT cascade into the borrowed handler closures (those
        // are owned by their Subscriptions, disposed separately). Null-guard for
        // consistency with other builtin disposal sites; null the slot after so a
        // re-entrant / unwind pass is a no-op (no double-free).
        if (it->eventSource) {
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* src = builder_.CreateLoad(ptrTy(), addr, "esrc.load");
            llvm::Value* isNull = builder_.CreateICmpEQ(
                src, llvm::ConstantPointerNull::get(ptrTy()));
            auto* frBB   = llvm::BasicBlock::Create(llvm_, "esrc.free", currentFn_);
            auto* contBB = llvm::BasicBlock::Create(llvm_, "esrc.cont", currentFn_);
            builder_.CreateCondBr(isNull, contBB, frBB);
            builder_.SetInsertPoint(frBB);
            auto* fn = declareC("bz_event_source_free",
                                llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(fn, {src});
            builder_.CreateStore(
                llvm::ConstantPointerNull::get(ptrTy()), addr);
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        // §6.2 / issue #647: a `using var` Mutex. Dispose via bz_mutex_dispose
        // (marks disposed, drops the Mutex object's core ref; destroys the
        // underlying pthread_mutex when the last Lock is also released).
        // Null-guarded: bz_mutex_dispose is null-safe, but we guard for
        // consistency with other builtin disposal sites.
        if (it->mutex) {
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* mx = builder_.CreateLoad(ptrTy(), addr, "mx.load");
            llvm::Value* isNull = builder_.CreateICmpEQ(
                mx, llvm::ConstantPointerNull::get(ptrTy()));
            auto* dispBB  = llvm::BasicBlock::Create(llvm_, "mx.dispose",  currentFn_);
            auto* contBB  = llvm::BasicBlock::Create(llvm_, "mx.cont",     currentFn_);
            builder_.CreateCondBr(isNull, contBB, dispBB);
            builder_.SetInsertPoint(dispBB);
            auto* fn = declareC("bz_mutex_dispose",
                                llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(fn, {mx});
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        // §6.2 / issue #647: a `using var` Lock. Release via bz_lock_release
        // (idempotent per §28.2: bz_lock_release checks the released flag).
        // Null-guarded: bz_lock_release handles null (null Lock? on timeout path).
        if (it->lock) {
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* lk = builder_.CreateLoad(ptrTy(), addr, "lk.load");
            llvm::Value* isNull = builder_.CreateICmpEQ(
                lk, llvm::ConstantPointerNull::get(ptrTy()));
            auto* relBB  = llvm::BasicBlock::Create(llvm_, "lk.release",  currentFn_);
            auto* contBB = llvm::BasicBlock::Create(llvm_, "lk.cont",     currentFn_);
            builder_.CreateCondBr(isNull, contBB, relBB);
            builder_.SetInsertPoint(relBB);
            auto* fn = declareC("bz_lock_release",
                                llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(fn, {lk});
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        // §6.3 / issue #647: a `using var` Semaphore. Dispose via bz_semaphore_dispose
        // (marks disposed, drops the Semaphore object's core ref; destroys the
        // underlying sem_t when the last SemaphoreToken is also released).
        if (it->semaphore) {
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* sem = builder_.CreateLoad(ptrTy(), addr, "sem.load");
            llvm::Value* isNull = builder_.CreateICmpEQ(
                sem, llvm::ConstantPointerNull::get(ptrTy()));
            auto* dispBB  = llvm::BasicBlock::Create(llvm_, "sem.dispose",  currentFn_);
            auto* contBB  = llvm::BasicBlock::Create(llvm_, "sem.cont",     currentFn_);
            builder_.CreateCondBr(isNull, contBB, dispBB);
            builder_.SetInsertPoint(dispBB);
            auto* fn = declareC("bz_semaphore_dispose",
                                llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(fn, {sem});
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        // §6.3 / issue #647: a `using var` SemaphoreToken. Release via
        // bz_semaphore_token_release (idempotent per §28.2: released flag
        // prevents double-release).  Null-safe (handles null SemaphoreToken?
        // on the timeout path).
        if (it->semaphoreToken) {
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* tok = builder_.CreateLoad(ptrTy(), addr, "stok.load");
            llvm::Value* isNull = builder_.CreateICmpEQ(
                tok, llvm::ConstantPointerNull::get(ptrTy()));
            auto* relBB  = llvm::BasicBlock::Create(llvm_, "stok.release",  currentFn_);
            auto* contBB = llvm::BasicBlock::Create(llvm_, "stok.cont",     currentFn_);
            builder_.CreateCondBr(isNull, contBB, relBB);
            builder_.SetInsertPoint(relBB);
            auto* fn = declareC("bz_semaphore_token_release",
                                llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(fn, {tok});
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        // issue #545: a `using var` Buffer<T>. Free via bz_buffer_free (null-safe
        // plain free — value elements carry no disposal obligation, so no per-element
        // callback is needed). Null-guarded for consistency with other builtin disposal.
        if (it->buffer) {
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* buf = builder_.CreateLoad(ptrTy(), addr, "buf.load");
            llvm::Value* isNull = builder_.CreateICmpEQ(
                buf, llvm::ConstantPointerNull::get(ptrTy()));
            auto* frBB   = llvm::BasicBlock::Create(llvm_, "buf.free",  currentFn_);
            auto* contBB = llvm::BasicBlock::Create(llvm_, "buf.cont",  currentFn_);
            builder_.CreateCondBr(isNull, contBB, frBB);
            builder_.SetInsertPoint(frBB);
            auto* fn = declareC("bz_buffer_free",
                                llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(fn, {buf});
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        // stdlib §10.1/§10.2/§10.3 / §32.6 / fix #977 / #985: a `using var`
        // CString, WString, or RawBytes. All are produced by static From /
        // FromBytes factories. CString/WString are bare malloc'd buffers
        // (bz_string_to_utf8z / bz_string_to_utf16z) released by a single
        // null-guarded free(). RawBytes (fix #985) is a TWO-LEVEL allocation —
        // a { ptr, len } struct whose `ptr` owns a separate byte buffer — so it
        // is released via bz_rawbytes_free, which frees the buffer THEN the
        // struct, exactly once. The disposal contract (stdlib §10.x destructor)
        // says "releases the underlying buffer when this instance allocated it".
        if (it->ffiBuffer) {
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* fbuf = builder_.CreateLoad(ptrTy(), addr, "ffibuf.load");
            llvm::Value* isNull = builder_.CreateICmpEQ(
                fbuf, llvm::ConstantPointerNull::get(ptrTy()));
            auto* frBB   = llvm::BasicBlock::Create(llvm_, "ffibuf.free",  currentFn_);
            auto* contBB = llvm::BasicBlock::Create(llvm_, "ffibuf.cont",  currentFn_);
            builder_.CreateCondBr(isNull, contBB, frBB);
            builder_.SetInsertPoint(frBB);
            if (it->rawBytes) {
                // fix #985: free the owned buffer AND the { ptr, len } struct.
                auto* fn = declareC("bz_rawbytes_free",
                                    llvm::Type::getVoidTy(llvm_), {ptrTy()});
                builder_.CreateCall(fn, {fbuf});
            } else {
                builder_.CreateCall(freeFn(), {fbuf});
            }
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        // §30.15 (#600 phase c): a `using var` Channel<T>. Dispose via
        // bz_channel_dispose (closes, faults pending receivers with
        // ChannelClosedException, disposes undelivered items in reverse order
        // per §28.11, frees the struct). The ChannelClosedException vtable is
        // retrieved from builtinVtable_ (set up by setupBuiltinExceptions).
        if (it->channel) {
            // #1957: an `owned` Channel<T> PARAMETER discharged on the normal
            // path disposes here only on unwind; a `using var` / auto-dropped
            // owner leaves the flag false and disposes on both exits.
            if (it->releaseOnUnwindOnly && !isUnwind) continue;
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* ch = builder_.CreateLoad(ptrTy(), addr);
            llvm::Value* isNull = builder_.CreateICmpEQ(
                ch, llvm::ConstantPointerNull::get(ptrTy()));
            auto* dispBB  = llvm::BasicBlock::Create(llvm_, "ch.dispose",  currentFn_);
            auto* contBB  = llvm::BasicBlock::Create(llvm_, "ch.cont",     currentFn_);
            builder_.CreateCondBr(isNull, contBB, dispBB);
            builder_.SetInsertPoint(dispBB);
            // Get ChannelClosedException vtable (may be null before setup).
            llvm::Value* cceVt = llvm::ConstantPointerNull::get(ptrTy());
            {   auto vit = builtinVtable_.find("ChannelClosedException");
                if (vit != builtinVtable_.end()) cceVt = vit->second;
            }
            auto* fn = declareC("bz_channel_dispose",
                                llvm::Type::getVoidTy(llvm_),
                                {ptrTy(), ptrTy()});
            builder_.CreateCall(fn, {ch, cceVt});
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        // #628 / stdlib §4.9 v0.135: a `using var` DrainedEntry<K,V> — the
        // loop variable from `for var entry in d.Drain() do … end`. The entry
        // struct is {K key; V* value} allocated on the stack. At scope end:
        //   1. Load V* from field[1] and dispose+reclaim the owned class value.
        //   2. If K is string: load the bz_string_t* from field[0] and release
        //      it (bz_string_release) — the drain runtime transferred ownership
        //      of the heap-allocated key string to the loop entry struct.
        // The slot is a struct AllocaInst, not a pointer-to-heap-object; no
        // boxedStorage detour — load fields directly via GEP.
        if (it->drainedEntry) {
            // drainedEntryKeyType / drainedEntryValType carry the LLVM types for
            // fields 0 and 1 of the entry struct (same layout as the alloca in
            // lowerDictDrainFor: { keyLlvm, valLlvm }). They may be null if
            // lowerTypeRef failed at registration time; skip the whole disposal
            // in that case (should not happen in well-formed code).
            llvm::Type* kTy = it->drainedEntryKeyType;
            llvm::Type* vTy = it->drainedEntryValType;
            if (kTy && vTy) {
                llvm::StructType* entStTy =
                    llvm::StructType::get(llvm_, {kTy, vTy});
                // Step 1: dispose the owned V class value (field[1]).
                // Guard: V must be a pointer type (class). OwningDictionary<K,V>
                // requires V: class per §4.9, so in well-formed code this always
                // holds; the guard prevents codegen crashes on ill-formed programs.
                if (vTy->isPointerTy()) {
                    llvm::Value* valFldPtr =
                        builder_.CreateStructGEP(entStTy, it->slot, 1, "de.val.ptr");
                    llvm::Value* valPtr =
                        builder_.CreateLoad(ptrTy(), valFldPtr, "de.val");
                    llvm::Value* isNullVal = builder_.CreateICmpEQ(
                        valPtr, llvm::ConstantPointerNull::get(ptrTy()));
                    auto* dispBB = llvm::BasicBlock::Create(
                        llvm_, "de.dispose", currentFn_);
                    auto* afterValBB = llvm::BasicBlock::Create(
                        llvm_, "de.after_val", currentFn_);
                    builder_.CreateCondBr(isNullVal, afterValBB, dispBB);
                    builder_.SetInsertPoint(dispBB);
                    // Dispose via dynamic dispatch (emitDisposeAndReclaim handles
                    // the vtable call + bz_free; dynamic dispatch is correct for
                    // both user and builtin V class types).
                    emitDisposeAndReclaim(valPtr);
                    builder_.CreateStore(
                        llvm::ConstantPointerNull::get(ptrTy()), valFldPtr);
                    builder_.CreateBr(afterValBB);
                    builder_.SetInsertPoint(afterValBB);
                }
                // Step 2: if K is string, release the owned key string.
                if (it->drainedEntryKeyIsString) {
                    llvm::Value* keyFldPtr =
                        builder_.CreateStructGEP(entStTy, it->slot, 0, "de.key.ptr");
                    llvm::Value* keyPtr =
                        builder_.CreateLoad(ptrTy(), keyFldPtr, "de.key");
                    emitStringRelease(keyPtr);
                }
            }
            continue;
        }
        // §30.15 (#600 phase c): a `using var` ChannelSender<T>. Release via
        // bz_channel_sender_release (decrements sender count; may close the
        // channel if this was the last sender).
        if (it->channelSender) {
            // #1957: an `owned` ChannelSender<T> PARAMETER discharged on the
            // normal path releases here only on unwind (the discharge site — a
            // move/return/.Dispose() — owns the normal exit); a `using var` or an
            // AUTO-DROPPED owned sender (§28.3, the never-discharged owner) leaves
            // the flag false and releases on both exits. As the last sender this
            // release closes the channel (§30.15).
            if (it->releaseOnUnwindOnly && !isUnwind) continue;
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* tx = builder_.CreateLoad(ptrTy(), addr);
            llvm::Value* isNull = builder_.CreateICmpEQ(
                tx, llvm::ConstantPointerNull::get(ptrTy()));
            auto* relBB  = llvm::BasicBlock::Create(llvm_, "tx.release",  currentFn_);
            auto* contBB = llvm::BasicBlock::Create(llvm_, "tx.cont",     currentFn_);
            builder_.CreateCondBr(isNull, contBB, relBB);
            builder_.SetInsertPoint(relBB);
            llvm::Value* cceVt = llvm::ConstantPointerNull::get(ptrTy());
            {   auto vit = builtinVtable_.find("ChannelClosedException");
                if (vit != builtinVtable_.end()) cceVt = vit->second;
            }
            auto* fn = declareC("bz_channel_sender_release",
                                llvm::Type::getVoidTy(llvm_),
                                {ptrTy(), ptrTy()});
            builder_.CreateCall(fn, {tx, cceVt});
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        // stdlib §6.10 (v0.175, #808): an `await using var` TaskScope. Normal
        // exit: bz_task_scope_dispose_async + await (cancel, drain, swallow;
        // IOE iff Join was never awaited). Unwind / non-async: the
        // non-blocking bz_task_scope_dispose_unwind zombie drain. The runtime
        // frees the scope struct on both paths.
        if (it->taskScope) {
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* sc = builder_.CreateLoad(ptrTy(), addr, "tscope.load");
            llvm::Value* isNull = builder_.CreateICmpEQ(
                sc, llvm::ConstantPointerNull::get(ptrTy()));
            auto* dispBB = llvm::BasicBlock::Create(llvm_, "tscope.dispose",
                                                    currentFn_);
            auto* contBB = llvm::BasicBlock::Create(llvm_, "tscope.cont",
                                                    currentFn_);
            builder_.CreateCondBr(isNull, contBB, dispBB);
            builder_.SetInsertPoint(dispBB);
            // Null the slot BEFORE the (possibly awaited) dispose: the
            // normal-path dispose task may FAULT (missing-Join IOE, §6.10),
            // whose re-raise unwinds into the isUnwind disposal pass — which
            // must find the slot already null (the runtime freed the scope
            // when the dispose task completed; a second dispose through the
            // stale pointer is a UAF).
            builder_.CreateStore(
                llvm::ConstantPointerNull::get(ptrTy()), addr);
            emitTaskScopeDispose(sc, isUnwind);
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        // §28.12 / §30 (`await using var`): async disposal path. On the NORMAL
        // exit (not isUnwind): call DisposeAsync() directly, await the returned
        // Task (a proper suspend point), then free the block (physical reclamation).
        // On the UNWIND path (exception in flight): synchronous Dispose() fallback —
        // the resource is reclaimed on every exit path.
        if (it->asyncDispose) {
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* obj = builder_.CreateLoad(ptrTy(), addr);
            llvm::Value* isNull = builder_.CreateICmpEQ(
                obj, llvm::ConstantPointerNull::get(ptrTy()));
            auto* dispBB = llvm::BasicBlock::Create(llvm_, "awaitusing.dispose",
                                                    currentFn_);
            auto* contBB = llvm::BasicBlock::Create(llvm_, "awaitusing.cont",
                                                    currentFn_);
            builder_.CreateCondBr(isNull, contBB, dispBB);
            builder_.SetInsertPoint(dispBB);
            // Pass the direct function pointer; null means sync fallback.
            // On the unwind path, always use sync fallback (pass null).
            emitAsyncDisposeAwaitAndFree(
                obj, isUnwind ? nullptr : it->disposeAsyncFn);
            builder_.CreateStore(
                llvm::ConstantPointerNull::get(ptrTy()), addr);
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        // §29.7 (#623): an iterator enumerator whose Dispose is at vtable slot 1
        // (iterator format: [0]=td, [1]=Dispose, [2]=MoveNext, [3]=Current).
        // Call Dispose(self) via slot 1 (which runs pending finally blocks inside
        // the state machine and sets state=-1) then free the heap frame.
        if (it->iterEnumDispose) {
            llvm::Value* obj = builder_.CreateLoad(ptrTy(), it->slot);
            llvm::Value* isNull = builder_.CreateICmpEQ(
                obj, llvm::ConstantPointerNull::get(ptrTy()));
            auto* dispBB = llvm::BasicBlock::Create(llvm_, "iter.dispose",
                                                    currentFn_);
            auto* contBB = llvm::BasicBlock::Create(llvm_, "iter.cont",
                                                    currentFn_);
            builder_.CreateCondBr(isNull, contBB, dispBB);
            builder_.SetInsertPoint(dispBB);
            // Dispatch vtable slot 1 (Dispose).
            llvm::Value* vt = builder_.CreateLoad(ptrTy(), obj);
            llvm::Value* dispSlot =
                builder_.CreateGEP(ptrTy(), vt, {builder_.getInt32(1)});
            llvm::Value* dispFnPtr = builder_.CreateLoad(ptrTy(), dispSlot);
            auto* dispTy = llvm::FunctionType::get(
                llvm::Type::getVoidTy(llvm_), {ptrTy()}, false);
            builder_.CreateCall(dispTy, dispFnPtr, {obj});
            // Dispose now freed the frame — null the slot.
            builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()),
                                 it->slot);
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        // #383 / §28: a `using var` owning a class/interface object whose
        // concrete class is not statically known here (interface-typed slot,
        // call returning `owned T`, or `move x.f`). Dispose dynamically through
        // its own vtable, null-guarded — an interface/nullable slot may hold
        // null. The dynamic dispatch runs the most-derived destructor + base
        // chain (idempotent, §28.2) and reclaims the block once (#380); the slot
        // is nulled afterward so any further disposal through it is a no-op
        // (§28.2; the #396 slot-null precedent).
        // §35.4 (v0.200, #2045 stage 3): an OWNING-classified choice local.
        // The active-case disposal emits a switch on the runtime tag and
        // disposes the active case's owned payloads. Fires on BOTH the
        // normal and unwind exit — like an `inlineOwnedArray` disposal, the
        // slot value is whatever the active case holds at scope end, and
        // disposing it is the SOLE path for those payloads.
        if (it->owningChoice) {
            emitOwningChoiceActiveCaseDisposal(it->slot, it->owningChoiceTy,
                                               it->owningChoiceRef);
            continue;
        }
        // fix #2115 / §28.9 / §35.4: a VALUE-LIKE choice local whose
        // construction mints a +1 string payload. The active-case string
        // release emits a switch on the runtime tag and calls
        // bz_string_release on the active case's string payload field(s). Fires
        // on BOTH the normal and unwind exit (the value dies either way; a
        // given exit goes one way only → exactly once per exit). Registered
        // ONLY for a non-escaping local (the escape walk's verdict), so the
        // slot is the sole owner of the +1 and no alias can double-free.
        if (it->valueLikeChoiceStrRelease) {
            emitValueLikeChoiceActiveCaseStrRelease(
                it->slot, it->valueLikeChoiceStrReleaseTy,
                it->valueLikeChoiceStrReleaseRef);
            continue;
        }
        if (it->dynamicDispose) {
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* obj = builder_.CreateLoad(ptrTy(), addr);
            llvm::Value* isNull = builder_.CreateICmpEQ(
                obj, llvm::ConstantPointerNull::get(ptrTy()));
            auto* dispBB = llvm::BasicBlock::Create(llvm_, "using.dispose",
                                                    currentFn_);
            auto* contBB = llvm::BasicBlock::Create(llvm_, "using.cont",
                                                    currentFn_);
            builder_.CreateCondBr(isNull, contBB, dispBB);
            builder_.SetInsertPoint(dispBB);
            emitDisposeAndReclaim(obj);
            builder_.CreateStore(
                llvm::ConstantPointerNull::get(ptrTy()), addr);
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        if (disposerFns_.find(it->cls) == disposerFns_.end()) continue;
        // CG18 / §28.2: dispatch Dispose dynamically through the object's vtable
        // (idempotent) and reclaim the block once, so a `using var` bound to a
        // subclass instance runs the correct runtime Dispose. This scope-end
        // disposal runs exactly once per scope (normal and unwind exits are
        // mutually exclusive; a break/continue terminates the block first).
        // §27.5: a captured ("boxed") `using var` local lives in a shared
        // heap cell; load the object through the cell, not the cell pointer.
        // #415 / §28.2 / §28.5 (line 5066): an earlier explicit `x.Dispose()`
        // now RECLAIMS the block and NULLS this slot (the explicit-dispose
        // lowering in LlvmBackend_Calls.cpp), so this scope-end site must skip a
        // null slot — reloading null and calling emitDisposeAndReclaim(null)
        // would dispatch through a null vtable. Null-guard (mirroring the
        // dynamicDispose path) and null the slot after reclaim so the object is
        // freed EXACTLY ONCE across the explicit + scope-end paths (no double-
        // free): whichever site disposes first reclaims and nulls; the other
        // finds null and skips.
        llvm::Type* ety = nullptr;
        llvm::Value* addr = boxedStorage(it->slot, ety);
        llvm::Value* obj = builder_.CreateLoad(ptrTy(), addr);
        llvm::Value* isNull = builder_.CreateICmpEQ(
            obj, llvm::ConstantPointerNull::get(ptrTy()));
        auto* dispBB = llvm::BasicBlock::Create(llvm_, "using.cls.dispose",
                                                currentFn_);
        auto* contBB = llvm::BasicBlock::Create(llvm_, "using.cls.cont",
                                                currentFn_);
        builder_.CreateCondBr(isNull, contBB, dispBB);
        builder_.SetInsertPoint(dispBB);
        emitDisposeAndReclaim(obj);
        builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), addr);
        builder_.CreateBr(contBB);
        builder_.SetInsertPoint(contBB);
    }
    // #1017 SECOND PASS: free any boxed capture cells whose plain boxedCellFree
    // was suppressed (boxedCellFreeDeferred). This runs AFTER every main-loop
    // disposal in this scope — including the owner's reads-through-cell disposal
    // (using var / collection / dict / subscription / ... — it loads the object
    // THROUGH the cell) and every capturing closure's release — so the cell has
    // no live reader or env reference left. Freeing here (instead of the
    // suppressed boxedCellFree, which LIFO order would have fired BEFORE the owner
    // disposal → UAF) closes the 8-byte capture-cell leak. free(null) is a no-op
    // (guarded boxing may leave the cell-holder null). Exactly-once: a deferred
    // entry is registered ONLY where boxedCellFree was suppressed (mutually
    // exclusive with boxedCellFree) and deduped per slot, so the cell is freed
    // exactly once across both the normal and the unwind exit (a given exit goes
    // one way only). Fires on both paths, symmetric with boxedCellFree —
    // EXCEPT skipCellFreeOnUnwind entries (fix #2334; see boxedCellFree's own
    // handling above and Disposal::skipCellFreeOnUnwind's declaration).
    for (const auto& d : scope) {
        if (!d.boxedCellFreeDeferred) continue;
        if (isUnwind && d.skipCellFreeOnUnwind) continue;
        llvm::Value* cell =
            builder_.CreateLoad(ptrTy(), d.slot, "cell.deferred.ptr");
        builder_.CreateCall(freeFn(), {cell});
        // Null the guarded holder so a later re-entry allocates a fresh cell.
        builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), d.slot);
    }
}


void Emitter::emitAllDisposals() {
    for (auto it = disposeStack_.rbegin(); it != disposeStack_.rend(); ++it)
        emitDisposals(*it);
}


void Emitter::emitLoopDisposals(std::size_t loopScopeDepth) {
    for (std::size_t i = disposeStack_.size(); i-- > loopScopeDepth;)
        emitDisposals(disposeStack_[i]);
}


void Emitter::emitDisposalsInRange(std::size_t lo, std::size_t hi) {
    // fix #1567 / §29.7/§28.10 (v0.182): the normal-exit twin of
    // emitUnwindDisposalsInRange — dispose scopes [lo, hi) innermost-first, but
    // on the NON-unwind (isUnwind=false) path so caughtException entries are not
    // skipped. A `return` routing through a `finally` uses this to dispose only
    // the try-body scopes before the finally and, in the finally's normal-exit
    // dispatch, the enclosing scopes after it — reverse establishment order.
    if (hi > disposeStack_.size()) hi = disposeStack_.size();
    for (std::size_t i = hi; i-- > lo;)
        emitDisposals(disposeStack_[i]);
}


}  // namespace bezel::codegen
