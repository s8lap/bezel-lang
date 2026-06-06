#include "codegen/LlvmBackend_impl.hpp"

namespace bezel::codegen {

// ===========================================================================
// §30 async/await state-machine codegen (Phase 2b).
//
// Layers the task-driver shape on top of the kind-agnostic suspend/resume
// scaffolding extracted in Phase 2a (LlvmBackend_Iterators.cpp). The async
// path mirrors the iterator (`Yield`) path with smKind_ == Await:
//
//   - The original method (declared `async`, returning Task / Task<T> /
//     OwnedTask<T>) becomes the ENTRY function. It allocates the outer Task,
//     calloc's the heap frame, spills the captures (`this` + params) and the
//     outer-task pointer into the frame, then invokes the Resume function once.
//     Because the runtime's bz_task_then has an already-done fast path and the
//     synchronous prefix runs inline (§30.1: "begins execution synchronously …
//     until the first await of an incomplete task"), a body whose awaited tasks
//     are all already-complete runs to completion within this single inline
//     drive. The entry returns the outer Task pointer.
//
//   - A Resume(void* completedTask, void* frame) continuation matches the
//     bz_task_then ABI. It restores the captures from the frame and runs the
//     resume dispatch (LlvmBackend_Iterators.cpp emitResumeDispatch). Each
//     `await` suspend point (LlvmBackend_Exprs.cpp lowerAwait) spills the
//     suspend-locals, registers Resume on the awaited task, and returns; the
//     resume re-enters at the next state block and reads the awaited result.
//     `return e;` completes the outer task (LlvmBackend_Stmts.cpp ReturnStmt,
//     gated on smKind_ == Await).
//
// #438: `await` inside a loop body (while / range-for / nested) is supported.
// Loop-carried and pre-/post-loop locals are frame-backed allocas (the iterator
// path's spill/restore machinery, reused), and fixupSuspendDominance handles the
// remaining anonymous cross-suspend temporaries (e.g. the left operand of
// `total + await f()`).
//
// #439-followup: those anonymous cross-suspend temporaries now live in the HEAP
// FRAME, not a function-local stack slot, so they survive a REAL cross-thread
// suspend. fixupSuspendDominance demotes the offending values and then
// promoteDemotedSlotsToFrame moves each into a side spill area pointed to from
// the frame (asyncSpillPtrField_) — reached through the single `state` pointer
// bz_task_then carries to the worker — and freed on the terminal paths
// (emitFreeSpillArea). This makes `var x := total + await f()` (and the
// loop-carried / multi-await variants) correct across a thread hop, where a
// stack slot held garbage on the worker's fresh stack.
//
// Deferred to later increments (not emitted here): await inside try/catch/
// finally (resuming into the right handler scope — rejected with a §30.9
// diagnostic), §30.9 faulting re-raise beyond the basic check, async iterators
// (IAsyncEnumerable / `await for`), Task.FireAndForget, and `await using`.
// ===========================================================================

bool Emitter::exprContainsAwait(const ast::Expr* e) {
    if (!e) return false;
    if (auto* un = dynamic_cast<const ast::UnaryExpr*>(e)) {
        if (un->op == "await") return true;
        return exprContainsAwait(un->operand.get());
    }
    if (auto* bin = dynamic_cast<const ast::BinaryExpr*>(e))
        return exprContainsAwait(bin->left.get()) ||
               exprContainsAwait(bin->right.get());
    if (auto* ce = dynamic_cast<const ast::CallExpr*>(e)) {
        if (exprContainsAwait(ce->callee.get())) return true;
        for (const auto& a : ce->args)
            if (exprContainsAwait(a.value.get())) return true;
        return false;
    }
    if (auto* me = dynamic_cast<const ast::MemberAccessExpr*>(e))
        return exprContainsAwait(me->target.get());
    if (auto* ix = dynamic_cast<const ast::IndexExpr*>(e)) {
        if (exprContainsAwait(ix->target.get())) return true;
        for (const auto& a : ix->indices)
            if (exprContainsAwait(a.value.get())) return true;
        return false;
    }
    if (auto* cd = dynamic_cast<const ast::ConditionalExpr*>(e))
        return exprContainsAwait(cd->condition.get()) ||
               exprContainsAwait(cd->thenValue.get()) ||
               exprContainsAwait(cd->elseValue.get());
    return false;
}


bool Emitter::containsAwait(const std::vector<std::unique_ptr<ast::Stmt>>& stmts) {
    for (const auto& s : stmts) {
        if (!s) continue;
        if (auto* ld = dynamic_cast<const ast::LocalDecl*>(s.get())) {
            if (exprContainsAwait(ld->initializer.get())) return true;
        } else if (auto* lc = dynamic_cast<const ast::LocalConstDecl*>(s.get())) {
            if (exprContainsAwait(lc->value.get())) return true;
        } else if (auto* as = dynamic_cast<const ast::AssignmentStmt*>(s.get())) {
            if (exprContainsAwait(as->value.get())) return true;
        } else if (auto* es = dynamic_cast<const ast::ExpressionStmt*>(s.get())) {
            if (exprContainsAwait(es->expr.get())) return true;
        } else if (auto* rs = dynamic_cast<const ast::ReturnStmt*>(s.get())) {
            if (exprContainsAwait(rs->value.get())) return true;
        } else if (auto* fs = dynamic_cast<const ast::ForStmt*>(s.get())) {
            if (containsAwait(fs->body)) return true;
        } else if (auto* ws = dynamic_cast<const ast::WhileStmt*>(s.get())) {
            if (exprContainsAwait(ws->condition.get())) return true;
            if (containsAwait(ws->body)) return true;
        } else if (auto* ifs = dynamic_cast<const ast::IfStmt*>(s.get())) {
            if (exprContainsAwait(ifs->ifBranch.condition.get())) return true;
            if (containsAwait(ifs->ifBranch.body)) return true;
            for (const auto& el : ifs->elifBranches) {
                if (exprContainsAwait(el.condition.get())) return true;
                if (containsAwait(el.body)) return true;
            }
            if (ifs->hasElse && containsAwait(ifs->elseBody)) return true;
        } else if (auto* db = dynamic_cast<const ast::DoEndBlock*>(s.get())) {
            if (containsAwait(db->body)) return true;
        }
    }
    return false;
}


// §30.2: count the await suspend points in a body (drives the resume switch's
// state-block count, exactly like collectYields for the iterator path).
static unsigned countAwaits(const std::vector<std::unique_ptr<ast::Stmt>>& stmts);

static unsigned countAwaitsInExpr(const ast::Expr* e) {
    if (!e) return 0;
    if (auto* un = dynamic_cast<const ast::UnaryExpr*>(e)) {
        unsigned n = countAwaitsInExpr(un->operand.get());
        if (un->op == "await") n += 1;
        return n;
    }
    if (auto* bin = dynamic_cast<const ast::BinaryExpr*>(e))
        return countAwaitsInExpr(bin->left.get()) +
               countAwaitsInExpr(bin->right.get());
    if (auto* ce = dynamic_cast<const ast::CallExpr*>(e)) {
        unsigned n = countAwaitsInExpr(ce->callee.get());
        for (const auto& a : ce->args) n += countAwaitsInExpr(a.value.get());
        return n;
    }
    if (auto* me = dynamic_cast<const ast::MemberAccessExpr*>(e))
        return countAwaitsInExpr(me->target.get());
    if (auto* ix = dynamic_cast<const ast::IndexExpr*>(e)) {
        unsigned n = countAwaitsInExpr(ix->target.get());
        for (const auto& a : ix->indices) n += countAwaitsInExpr(a.value.get());
        return n;
    }
    if (auto* cd = dynamic_cast<const ast::ConditionalExpr*>(e))
        return countAwaitsInExpr(cd->condition.get()) +
               countAwaitsInExpr(cd->thenValue.get()) +
               countAwaitsInExpr(cd->elseValue.get());
    return 0;
}

static unsigned countAwaits(const std::vector<std::unique_ptr<ast::Stmt>>& stmts) {
    unsigned n = 0;
    for (const auto& s : stmts) {
        if (!s) continue;
        if (auto* ld = dynamic_cast<const ast::LocalDecl*>(s.get()))
            n += countAwaitsInExpr(ld->initializer.get());
        else if (auto* lc = dynamic_cast<const ast::LocalConstDecl*>(s.get()))
            n += countAwaitsInExpr(lc->value.get());
        else if (auto* as = dynamic_cast<const ast::AssignmentStmt*>(s.get()))
            n += countAwaitsInExpr(as->value.get());
        else if (auto* es = dynamic_cast<const ast::ExpressionStmt*>(s.get()))
            n += countAwaitsInExpr(es->expr.get());
        else if (auto* rs = dynamic_cast<const ast::ReturnStmt*>(s.get()))
            n += countAwaitsInExpr(rs->value.get());
        else if (auto* fs = dynamic_cast<const ast::ForStmt*>(s.get()))
            n += countAwaits(fs->body);
        else if (auto* ws = dynamic_cast<const ast::WhileStmt*>(s.get())) {
            n += countAwaitsInExpr(ws->condition.get());
            n += countAwaits(ws->body);
        } else if (auto* ifs = dynamic_cast<const ast::IfStmt*>(s.get())) {
            n += countAwaitsInExpr(ifs->ifBranch.condition.get());
            n += countAwaits(ifs->ifBranch.body);
            for (const auto& el : ifs->elifBranches) {
                n += countAwaitsInExpr(el.condition.get());
                n += countAwaits(el.body);
            }
            if (ifs->hasElse) n += countAwaits(ifs->elseBody);
        } else if (auto* db = dynamic_cast<const ast::DoEndBlock*>(s.get())) {
            n += countAwaits(db->body);
        }
    }
    return n;
}


// §30.9 / §31.2 (#438): detect an `await` in a STILL-DEFERRED try position — a
// catch arm (guard or body) or a `finally` body. An `await` in the try BODY is
// now supported: lowerTry lowers the body (and hence the await's resume block)
// inside the try's protected region, so the resume keeps the catch dispatch /
// finally as its unwind target — a faulted await (re-raised with the original
// exception object, §30.9) routes to the enclosing catch, and the finally runs
// on every completion path. What remains hard is resuming INTO a catch handler
// (re-establishing the caught-exception disposal scope + currentCaughtExc_
// across a suspend) or into a finally (the exit-mode re-dispatch across a
// suspend); those are rejected with a diagnostic + safe stub. We recurse into
// every compound statement — including a try BODY — so a deferred position
// nested inside a loop / if / do-block / inner try is still caught.
bool Emitter::awaitInDeferredTryPosition(
    const std::vector<std::unique_ptr<ast::Stmt>>& stmts) {
    for (const auto& s : stmts) {
        if (!s) continue;
        if (auto* ts = dynamic_cast<const ast::TryStmt*>(s.get())) {
            // A catch guard / arm body or a finally with an await is deferred.
            // A catch arm body is a single Stmt (often a DoEndBlock);
            // containsAwait operates on a statement list, so inspect the single
            // statement directly via the common shapes.
            for (const auto& arm : ts->catches) {
                if (arm.guard && exprContainsAwait(arm.guard.get()))
                    return true;
                const ast::Stmt* ab = arm.body.get();
                if (ab) {
                    if (auto* db = dynamic_cast<const ast::DoEndBlock*>(ab)) {
                        if (containsAwait(db->body)) return true;
                    } else if (auto* es =
                                   dynamic_cast<const ast::ExpressionStmt*>(ab)) {
                        if (exprContainsAwait(es->expr.get())) return true;
                    } else if (auto* asn =
                                   dynamic_cast<const ast::AssignmentStmt*>(ab)) {
                        if (exprContainsAwait(asn->value.get())) return true;
                    } else if (auto* ld =
                                   dynamic_cast<const ast::LocalDecl*>(ab)) {
                        if (exprContainsAwait(ld->initializer.get())) return true;
                    } else if (auto* rs =
                                   dynamic_cast<const ast::ReturnStmt*>(ab)) {
                        if (exprContainsAwait(rs->value.get())) return true;
                    }
                }
            }
            if (containsAwait(ts->finallyBody)) return true;
            // The try BODY is supported, but it may itself contain a NESTED try
            // whose catch / finally awaits — recurse into the body too.
            if (awaitInDeferredTryPosition(ts->body)) return true;
            continue;
        }
        // Recurse into other compound statements so a deferred position nested
        // inside a loop, if, or do-block is still caught.
        if (auto* fs = dynamic_cast<const ast::ForStmt*>(s.get())) {
            if (awaitInDeferredTryPosition(fs->body)) return true;
        } else if (auto* ws = dynamic_cast<const ast::WhileStmt*>(s.get())) {
            if (awaitInDeferredTryPosition(ws->body)) return true;
        } else if (auto* ifs = dynamic_cast<const ast::IfStmt*>(s.get())) {
            if (awaitInDeferredTryPosition(ifs->ifBranch.body)) return true;
            for (const auto& el : ifs->elifBranches)
                if (awaitInDeferredTryPosition(el.body)) return true;
            if (ifs->hasElse && awaitInDeferredTryPosition(ifs->elseBody))
                return true;
        } else if (auto* db = dynamic_cast<const ast::DoEndBlock*>(s.get())) {
            if (awaitInDeferredTryPosition(db->body)) return true;
        }
    }
    return false;
}


void Emitter::emitAsyncMethod(sema::MethodSymbol* ms, llvm::Function* fn,
                              const CallableInfo& ci) {
    if (!ci.stmts) return;
    smKind_ = SuspendKind::Await;

    // §30.9 / §31.2 (#438): `await` in a try BODY is supported (lowerTry lowers
    // the await's resume block inside the protected region, so a faulted await
    // re-raises into the enclosing catch and the finally runs on every path).
    // `await` in a catch arm or a `finally` is still deferred — resuming INTO a
    // catch handler / finally across a suspend needs the caught-exception
    // disposal scope / exit-mode re-dispatch re-established at resume, which the
    // flat resume model can't yet do. Reject those positions with a clear
    // diagnostic + safe stub so no malformed IR escapes.
    if (awaitInDeferredTryPosition(*ci.stmts)) {
        ctx_.diagnostics().report(
            Severity::Error,
            ms->decl ? ms->decl->span.begin : SourceLocation{},
            "not yet implemented: `await` inside a catch arm or a `finally` — "
            "resuming into the handler scope across a suspend is a deferred "
            "increment (`await` in a try BODY is supported) (§30.9)");
        // Emit a minimal well-formed body so no malformed IR escapes: alloc the
        // task, complete it with a zero/default result, and return it (or the
        // exit code for async Main). The diagnostic above fails the compile.
        emitAsyncStub(ms, fn, ci);
        return;
    }

    // Determine the result element type T from the return type's generic arg.
    //   Task           → no result value (asyncResultTy_ = null)
    //   Task<T>        → T
    //   OwnedTask<T>   → T (Pattern B; ownership transfer is the awaiter's job)
    llvm::Type* resultTy = nullptr;
    if (ci.returnType) {
        auto* retRef = ctx_.resolvedType(*ci.returnType);
        if (retRef && !retRef->genericArgs.empty() && retRef->genericArgs[0])
            resultTy = lowerTypeRef(retRef->genericArgs[0]);
    }
    asyncResultTy_ = resultTy;

    // Build the kind-agnostic frame with three trailing async fields:
    //   [.. ] outerTask (ptr), [.. ] pendingTask (ptr), [.. ] spillArea (ptr).
    // Layout: { ptr vtable, i32 state, captures..., locals...,
    //           ptr outer, ptr pending, ptr spill }
    // §30.2 / §30.13 (#439-followup): `spill` points at a SEPARATELY-calloc'd
    // heap region holding the anonymous cross-suspend temporaries that
    // fixupSuspendDominance would otherwise leave in function-local stack slots
    // (e.g. the pre-suspend operand of `total + await f()`). Those slots are
    // garbage after a real cross-thread resume (the worker has a fresh stack), so
    // such temporaries must round-trip the HEAP frame like named locals do. The
    // spill area is appended as a side region (rather than grown into this struct,
    // whose body is already finalized) and pointed-to from a frame field so the
    // single `state` pointer that bz_task_then carries across the thread boundary
    // reaches it. It is lazily calloc'd on the first resume entry (see
    // fixupSuspendDominance) and stays null when the body has no such temporary.
    StateMachineFrame frame = buildStateMachineFrame(
        ms, *ci.stmts, {ptrTy(), ptrTy(), ptrTy()},
        std::string("async.") + std::string(ms->name));
    auto* stateStructTy = frame.structTy;
    const unsigned STATE_FIELD = frame.stateField;
    const unsigned OUTER_TASK_FIELD = frame.currentValField;        // first trailing
    const unsigned PENDING_TASK_FIELD = frame.currentValField + 1;  // second trailing
    const unsigned SPILL_PTR_FIELD = frame.currentValField + 2;     // third trailing
    asyncOuterTaskField_ = OUTER_TASK_FIELD;
    asyncPendingTaskField_ = PENDING_TASK_FIELD;
    asyncSpillPtrField_ = SPILL_PTR_FIELD;

    bool isInstance = !isStaticMethod(ms);
    std::string prefix = std::string("bz$async$") + std::string(ms->name) + "$";

    // --- Resume(ptr completedTask, ptr frame) -> void ---
    // The bz_task_then continuation ABI is void(void* task, void* state); the
    // frame is the state.
    auto* resumeTy = llvm::FunctionType::get(
        llvm::Type::getVoidTy(llvm_), {ptrTy(), ptrTy()}, false);
    auto* resumeFn = llvm::Function::Create(
        resumeTy, llvm::Function::InternalLinkage, prefix + "Resume",
        module_.get());
    asyncResumeFn_ = resumeFn;

    // --- ENTRY (= the original method body): alloc task + frame, drive once ---
    {
        currentFn_ = fn;
        currentThis_ = nullptr;
        locals_.clear(); paramSlots_.clear(); refParams_.clear();
        disposeStack_.clear(); loopStack_.clear(); finallyStack_.clear();
        propagateBB_ = nullptr; unwindTarget_ = nullptr;
        currentCaughtExc_ = nullptr; unwindScopeDepth_ = 0;
        auto* entry = llvm::BasicBlock::Create(llvm_, "entry", fn);
        builder_.SetInsertPoint(entry);
        bindParameters(fn, isInstance, &ms->params);

        // outerTask = bz_task_alloc()
        auto* allocFn = declareC("bz_task_alloc", ptrTy(), {});
        llvm::Value* outerTask = builder_.CreateCall(allocFn, {}, "async.task");

        // frame = calloc(1, sizeof(Frame))
        uint64_t stSz = module_->getDataLayout().getTypeAllocSize(stateStructTy);
        auto* callocFn2 = declareC("calloc", ptrTy(), {i64(), i64()});
        llvm::Value* st = builder_.CreateCall(
            callocFn2, {builder_.getInt64(1), builder_.getInt64(stSz)},
            "async.frame");

        // state = 0
        llvm::Value* stateField =
            builder_.CreateStructGEP(stateStructTy, st, STATE_FIELD);
        builder_.CreateStore(builder_.getInt32(0), stateField);
        // outerTask field
        llvm::Value* otField =
            builder_.CreateStructGEP(stateStructTy, st, OUTER_TASK_FIELD);
        builder_.CreateStore(outerTask, otField);
        // captures (this + params)
        emitFrameCaptureStores(frame, st);

        // §30.13 (#439): start the runtime thread pool before driving an async
        // Main. The pool's worker threads run the continuations of genuinely-
        // incomplete tasks (Task.Delay, and any task whose producer suspends),
        // resuming the state machine across the thread boundary. Starting it in
        // the Main prologue (a single program-wide root, §22.7 / §33.2) means the
        // pool is up before any await that might suspend. A purely synchronous /
        // already-done body never touches the pool (each await hits the fast path
        // and runs inline), so the threads simply idle until shutdown — but
        // starting them is cheap and keeps the cross-thread path correct. For a
        // non-Main async method the pool is initialized lazily by the runtime
        // (bz_task_delay / bz_task_schedule) on first use. bz_scheduler_init is
        // idempotent (a no-op if already started), so a lazy init that already ran
        // is harmless.
        if (isMain(ms)) {
            auto* schedInitFn = declareC("bz_scheduler_init",
                                         llvm::Type::getVoidTy(llvm_), {i32()});
            // A small fixed pool (4 workers) — enough to run nested/concurrent
            // continuations without unbounded thread growth (§30.13).
            builder_.CreateCall(schedInitFn, {builder_.getInt32(4)});
        }

        // Drive the state machine once synchronously (§30.1 synchronous prefix).
        builder_.CreateCall(resumeFn, {llvm::ConstantPointerNull::get(ptrTy()), st});

        if (isMain(ms)) {
            // §33.2: async Main. The C entry `main` returns i32 — block for the
            // task's completion (bz_task_await_sync; already-done returns
            // immediately) and use its result as the process exit code. A
            // `Task<int>` yields the int; a bare `Task` yields 0 (§33.3 normal
            // completion).
            auto* awaitFn = declareC("bz_task_await_sync", ptrTy(), {ptrTy()});
            llvm::Value* resPtr =
                builder_.CreateCall(awaitFn, {outerTask}, "main.result.ptr");
            // §33.3 / §30.9: a faulted Main task is an uncaught exception at the
            // process boundary — surface it (stderr + non-zero exit) rather than
            // dereferencing the null result buffer.
            auto* isErrFn = declareC("bz_task_is_error", i1(), {ptrTy()});
            llvm::Value* mainErr =
                builder_.CreateCall(isErrFn, {outerTask}, "main.faulted");
            auto* errBB = llvm::BasicBlock::Create(llvm_, "main.fault", fn);
            auto* okBB = llvm::BasicBlock::Create(llvm_, "main.ok", fn);
            builder_.CreateCondBr(mainErr, errBB, okBB);
            builder_.SetInsertPoint(errBB);
            auto* unhandledFn = declareC("bz_unhandled_exception",
                                         llvm::Type::getVoidTy(llvm_), {});
            unhandledFn->setDoesNotReturn();
            builder_.CreateCall(unhandledFn, {});
            builder_.CreateUnreachable();
            builder_.SetInsertPoint(okBB);
            llvm::Value* res = nullptr;
            if (resultTy && resultTy->isIntegerTy()) {
                res = builder_.CreateLoad(resultTy, resPtr, "main.result");
                if (resultTy != i32()) res = builder_.CreateTruncOrBitCast(res, i32());
            }
            // §30.13 (#439): shut the thread pool down cleanly before Main
            // returns so the process exits without hung worker threads (and
            // leak-clean under LSan: the pool's threads + queue are joined and
            // freed). This runs AFTER bz_task_await_sync(outerTask) above, so the
            // outer task is already complete — every continuation has either run
            // or is the one that completed it; shutdown drains the queue and joins
            // each worker, so no continuation is still in flight when we free the
            // task below. bz_scheduler_shutdown is a no-op if the pool was never
            // started (a purely synchronous body that lazily-init'd nothing).
            auto* schedShutdownFn = declareC("bz_scheduler_shutdown",
                                             llvm::Type::getVoidTy(llvm_), {});
            builder_.CreateCall(schedShutdownFn, {});
            // §30.5 (minimal): reclaim the outer task (its result buffer) on the
            // normal completion path so the process is leak-clean. The frame was
            // already freed by the completing resume (emitAsyncComplete). Safe
            // here: the outer task is complete and the pool is shut down, so no
            // continuation is still in flight referencing the task.
            auto* freeTaskFn = declareC("bz_task_free", llvm::Type::getVoidTy(llvm_),
                                        {ptrTy()});
            builder_.CreateCall(freeTaskFn, {outerTask});
            builder_.CreateRet(res ? res : builder_.getInt32(0));
        } else {
            // Return the outer task pointer (the method's Task / Task<T> result).
            builder_.CreateRet(outerTask);
        }
    }

    // --- Resume: the state machine body ---
    {
        currentFn_ = resumeFn;
        currentThis_ = nullptr;
        locals_.clear(); paramSlots_.clear(); refParams_.clear();
        disposeStack_.clear(); loopStack_.clear(); finallyStack_.clear();
        propagateBB_ = nullptr; unwindTarget_ = nullptr;
        currentCaughtExc_ = nullptr; unwindScopeDepth_ = 0;
        auto* entry = llvm::BasicBlock::Create(llvm_, "entry", resumeFn);
        builder_.SetInsertPoint(entry);

        // arg0 = completedTask (the task that fired this resume; unused on the
        //        initial drive and read via the frame's pendingTask slot at the
        //        await site instead), arg1 = frame.
        llvm::Value* self = resumeFn->getArg(1);

        // Restore captured params (this + params) from the frame.
        emitFrameCaptureRestores(frame, self);

        // State field GEP — read by the resume dispatch, written at each await.
        llvm::Value* stateGEP =
            builder_.CreateStructGEP(stateStructTy, self, STATE_FIELD);

        unsigned numAwaits = countAwaits(*ci.stmts);

        // Emit the resume switch + state blocks. The async "done" / fall-off
        // return completes the outer task with no value (§30.1: falling off a
        // Task-returning async method == `return;`) and returns from resume.
        std::vector<llvm::BasicBlock*> stateBBs = emitResumeDispatch(
            frame, self, stateGEP, numAwaits,
            [&]() {
                emitAsyncComplete(/*value=*/nullptr);
                builder_.CreateRetVoid();
            });

        // §30.9 (basic): the async fault boundary. An uncaught throw in the
        // async body — a synchronous `throw`, or the await-faulted re-raise —
        // branches here (it is the resume function's default unwindTarget_). It
        // completes the OUTER task with an error so the awaiter observes the
        // fault, then returns from resume. A try/catch in the body installs its
        // own nearer unwindTarget_ via lowerTry, so handled throws never reach
        // here; only genuinely-uncaught faults do.
        //
        // §30.9: the in-flight exception object lives in the bz$exc global at this
        // point (a synchronous `throw` stored it there before branching to the
        // unwind target; a faulted-await re-raise — lowerAwait below — likewise
        // restores the awaited task's exception object into bz$exc before
        // branching here when no nearer handler exists). Carry that ORIGINAL
        // object through the outer task (bz_task_complete_error_obj) so the next
        // awaiter — including one inside its own try/catch — re-raises the real
        // exception type, not a reconstructed message. If bz$exc happens to be
        // null (a non-object fault), the awaiter's await.fault path falls back to
        // completing with no object (the unhandled boundary still exits non-zero).
        auto* faultBB = llvm::BasicBlock::Create(llvm_, "async.fault", resumeFn);
        unwindTarget_ = faultBB;
        {
            llvm::IRBuilder<>::InsertPointGuard guard(builder_);
            builder_.SetInsertPoint(faultBB);
            llvm::Value* otField = builder_.CreateStructGEP(
                stateStructTy, self, OUTER_TASK_FIELD);
            llvm::Value* outerTask = builder_.CreateLoad(ptrTy(), otField);
            llvm::Value* excObj =
                builder_.CreateLoad(ptrTy(), excGlobal(), "async.fault.exc");
            auto* completeErrObjFn = declareC("bz_task_complete_error_obj",
                                              llvm::Type::getVoidTy(llvm_),
                                              {ptrTy(), ptrTy()});
            builder_.CreateCall(completeErrObjFn, {outerTask, excObj});
            // The exception object's ownership transfers to the outer task's
            // awaiter (or the unhandled boundary); clear bz$exc so it is not
            // double-handled if this resume activation continues elsewhere.
            builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()),
                                 excGlobal());
            // §30.5: the frame is dead on this terminal (fault) path — free it.
            // §30.13 (#439-followup): free the cross-suspend spill area first (if
            // any) — it is a side allocation pointed to from the frame.
            emitFreeSpillArea(self);
            auto* freeFn2 = declareC("free", llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(freeFn2, {self});
            builder_.CreateRetVoid();
        }

        // Emit Resume body starting from state 0. The await suspend points and
        // ReturnStmt handlers (gated on smKind_ == Await) interrupt it to emit
        // the suspend transitions / task completion.
        builder_.SetInsertPoint(stateBBs[0]);
        disposeStack_.emplace_back();
        bool term = lowerBody(*ci.stmts);
        disposeStack_.pop_back();
        // If the body fell through without a terminator, complete with no value
        // and return (§30.1 fall-off == return;).
        if (!term && !builder_.GetInsertBlock()->getTerminator()) {
            emitAsyncComplete(/*value=*/nullptr);
            builder_.CreateRetVoid();
        }

        // §30.2 (#438): repair SSA dominance broken by awaits embedded in larger
        // expressions (e.g. the loop-carried `total := total + await f(i)`): a
        // temporary computed before the suspend but used in the resume block,
        // which the dispatch switch / loop back-edge re-enters. Named locals
        // already round-trip through the frame; this demotes the remaining
        // anonymous temporaries to stack slots so the resume function verifies.
        fixupSuspendDominance(resumeFn);

        // Clear state-machine state.
        smStateStruct_ = nullptr;
        smSelf_ = nullptr;
        smLocalFields_.clear();
        smAllocaToField_.clear();
        asyncResumeFn_ = nullptr;
        asyncResultTy_ = nullptr;
    }
}


llvm::Value* Emitter::lowerAwait(const ast::UnaryExpr& un) {
    // Defensive: `await` outside an async state machine is rejected by sema; if
    // we somehow reach here without the async frame live, produce nothing.
    if (smKind_ != SuspendKind::Await || !smStateStruct_ || !smSelf_ ||
        !asyncResumeFn_)
        return nullptr;
    if (!un.operand) return nullptr;

    // Lower the operand to the awaited task pointer.
    llvm::Value* task = lowerExpr(*un.operand);
    if (!task) return nullptr;

    // The await result type (§30.3 / §30.5): T for Task<T>/OwnedTask<T>, or
    // unit for `await Task`. sema set this on the UnaryExpr.
    llvm::Type* resultTy = nullptr;
    if (auto* rt = ctx_.exprType(un)) {
        if (rt->kind != sema::TypeRef::Kind::Unit)
            resultTy = lowerTypeRef(rt);
    }

    // Store the awaited task into the frame's pending-task slot so the resumed
    // state can read its result (the local `task` value does not survive a
    // suspend).
    llvm::Value* pendField =
        builder_.CreateStructGEP(smStateStruct_, smSelf_, asyncPendingTaskField_);
    builder_.CreateStore(task, pendField);

    // Spill all suspend-locals into the frame BEFORE the fast/slow branch, so
    // the frame is the canonical store across the await boundary on BOTH paths.
    // The resume block (the convergence point) restores from the frame
    // unconditionally; if the fast path skipped this spill, that restore would
    // clobber the live alloca with the frame's stale (zero) value. (This is the
    // analogue of the yield handler's pre-yield spill, but here it must precede
    // the already-done fast path too.)
    for (auto& [allocaSlot, fieldIdx] : smAllocaToField_) {
        llvm::Value* fptr =
            builder_.CreateStructGEP(smStateStruct_, smSelf_, fieldIdx);
        llvm::Value* curVal =
            builder_.CreateLoad(allocaSlot->getAllocatedType(), allocaSlot);
        builder_.CreateStore(curVal, fptr);
    }

    // Allocate the next state block — both the already-done fast path and the
    // resumed continuation converge here to read the result (§30.2).
    unsigned nextState = ++smNextSuspendState_;
    llvm::BasicBlock* resumeBB = (nextState < smStateBBs_.size())
                                     ? smStateBBs_[nextState]
                                     : llvm::BasicBlock::Create(
                                           llvm_, "await.resume", currentFn_);

    // Fast path test: bz_task_is_done(task)?
    auto* isDoneFn = declareC("bz_task_is_done", i1(), {ptrTy()});
    llvm::Value* done = builder_.CreateCall(isDoneFn, {task}, "await.done");

    auto* suspendBB = llvm::BasicBlock::Create(llvm_, "await.suspend", currentFn_);
    builder_.CreateCondBr(done, resumeBB, suspendBB);

    // --- Suspend path (§30.2: the task is running) ---
    builder_.SetInsertPoint(suspendBB);
    // Record the resume state index, register the continuation, and return.
    builder_.CreateStore(builder_.getInt32((int)nextState), smStateGEP_);
    auto* thenFn = declareC("bz_task_then", llvm::Type::getVoidTy(llvm_),
                            {ptrTy(), ptrTy(), ptrTy()});
    builder_.CreateCall(thenFn, {task, asyncResumeFn_, smSelf_});
    builder_.CreateRetVoid();

    // --- Resume / fast-path convergence: read the result (§30.2) ---
    builder_.SetInsertPoint(resumeBB);
    // Restore all suspend-locals from the frame (they were spilled above; on the
    // fast path they are simply re-loaded with the same values).
    for (auto& [allocaSlot, fieldIdx] : smAllocaToField_) {
        llvm::Value* fptr =
            builder_.CreateStructGEP(smStateStruct_, smSelf_, fieldIdx);
        llvm::Value* savedVal =
            builder_.CreateLoad(allocaSlot->getAllocatedType(), fptr);
        builder_.CreateStore(savedVal, allocaSlot);
    }
    // Reload the awaited task from the pending slot.
    llvm::Value* awaitedTask =
        builder_.CreateLoad(ptrTy(), builder_.CreateStructGEP(
                                         smStateStruct_, smSelf_,
                                         asyncPendingTaskField_));

    // §30.9: awaiting a faulted task RE-RAISES the original exception "with the
    // original stack trace preserved" (spec §30.9). The state machine routes this
    // re-raise through the SAME unwind machinery a synchronous `throw` uses:
    // restore the awaited task's exception object into the bz$exc global, run the
    // intervening unwind disposals, and branch to the current unwindTarget_.
    //   - Inside a try BODY, unwindTarget_ is the try's catch.dispatch, so the
    //     fault is CAUGHT by the enclosing catch (and the finally runs) — §31.2.
    //   - Outside any try, unwindTarget_ is the resume function's async.fault
    //     boundary, which completes THIS outer task with the same exception
    //     object (bz_task_complete_error_obj) so the next awaiter re-raises it in
    //     turn; at the top (async Main) the process exits non-zero (§33.3).
    // Carrying the exception OBJECT (not just a message) lets a type-specific
    // catch arm match it (emitIsA needs the real object). Emitted unconditionally
    // so `await Task` (no value) re-raises a fault too.
    {
        auto* isErrFn = declareC("bz_task_is_error", i1(), {ptrTy()});
        llvm::Value* faulted =
            builder_.CreateCall(isErrFn, {awaitedTask}, "await.faulted");
        auto* faultBB = llvm::BasicBlock::Create(llvm_, "await.fault", currentFn_);
        auto* okBB = llvm::BasicBlock::Create(llvm_, "await.ok", currentFn_);
        builder_.CreateCondBr(faulted, faultBB, okBB);

        builder_.SetInsertPoint(faultBB);
        // Restore the awaited task's exception object into the in-flight slot.
        // The object is a Bezel heap exception independent of the task's own
        // buffers, so freeing the task below does not free it.
        auto* errObjFn = declareC("bz_task_error_obj", ptrTy(), {ptrTy()});
        llvm::Value* excObj =
            builder_.CreateCall(errObjFn, {awaitedTask}, "await.errobj");
        builder_.CreateStore(excObj, excGlobal());
        // §30.5: reclaim the awaited task here when it is an anonymous producer
        // result (`await Producer()`) — a single-use task with no other owner.
        // bz_task_free frees the task struct + its result/message buffers but NOT
        // the exception object, which now lives on in bz$exc for the handler.
        if (dynamic_cast<const ast::CallExpr*>(un.operand.get())) {
            auto* freeTaskFn = declareC("bz_task_free",
                                        llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(freeTaskFn, {awaitedTask});
        }
        // §28.3 / CG21: dispose the using-vars unwound between here and the
        // handler (a caught-exception entry is skipped — it stays in flight), the
        // same as emitThrowValue's synchronous-throw path, then branch to the
        // unwind target (catch.dispatch inside a try, or async.fault otherwise).
        emitUnwindDisposals();
        builder_.CreateBr(unwindBB());

        builder_.SetInsertPoint(okBB);
    }

    // Read the result. bz_task_await_sync returns a pointer to the result buffer
    // (already-complete → returns immediately). For `await Task` (no value) the
    // result is unit; return a null placeholder.
    auto* awaitFn = declareC("bz_task_await_sync", ptrTy(), {ptrTy()});
    llvm::Value* result = nullptr;
    if (!resultTy) {
        // Drain to ensure completion ordering even with no value.
        builder_.CreateCall(awaitFn, {awaitedTask});
        result = llvm::ConstantPointerNull::get(ptrTy());
    } else {
        llvm::Value* resPtr =
            builder_.CreateCall(awaitFn, {awaitedTask}, "await.result.ptr");
        result = builder_.CreateLoad(resultTy, resPtr, "await.result");
        // The result is loaded BY VALUE into the await site before the task is
        // freed below, so the value (and the task's freed result buffer) do not
        // alias afterwards.
    }
    // §30.5 (minimal): reclaim the awaited task when its operand is an anonymous
    // producer result (`await Producer()`) — a single-use task with no other
    // owner. A task bound to a local (`await t`, `using var t := ...`) is left to
    // its binder's disposal (the using-var / §30.5 disposal path), so we do NOT
    // free it here (that would double-free). This keeps the common
    // `await SomeAsyncCall()` chain leak-clean without risking the bound-task
    // case. (Multi-await Pattern A on a class-typed Task<T> is deferred; the
    // value-typed minimal cut's anonymous producer results are single-consumer.)
    if (dynamic_cast<const ast::CallExpr*>(un.operand.get())) {
        auto* freeTaskFn = declareC("bz_task_free", llvm::Type::getVoidTy(llvm_),
                                    {ptrTy()});
        builder_.CreateCall(freeTaskFn, {awaitedTask});
    }
    return result;
}


// §30.2 / §30.13 (#438 + #439-followup): repair SSA dominance in a resume
// function, moving the offending cross-suspend temporaries into the HEAP FRAME
// so they survive a REAL cross-thread suspend.
//
// After the body is lowered, every value computed before a suspend and used in a
// resume block (the dispatch switch / loop back-edge re-enters those blocks)
// must not assume its def dominates the use ("instruction does not dominate all
// uses"). Named locals already round-trip through the frame (smAllocaToField_);
// the remaining offenders are anonymous temporaries — e.g. the left operand of
// `total + await f()` loaded before the await, used after.
//
// We demote every register whose def fails to dominate a use (DemoteRegToStack),
// and every PHI with a mismatched incoming edge (DemotePHIToStack), iterating to
// a fixpoint (a demote can expose a new offender when one demoted value fed
// another). Each demotion creates a function-local stack alloca. A plain stack
// slot is WRONG across a real thread hop: the resume runs on a worker thread
// with a fresh stack, so the slot holding the pre-suspend value is garbage. We
// therefore PROMOTE every demoted slot into the heap state-machine frame: a
// SEPARATELY-calloc'd spill area, pointed to from a frame field
// (asyncSpillPtrField_), reached through the single `state` pointer that
// bz_task_then carries across the thread boundary — exactly how named locals
// (smAllocaToField_) already survive. The demoted alloca is replaced by a GEP
// into that heap spill area (computed in the resume entry, so it dominates every
// use); the store-before-suspend and load-after-resume that DemoteRegToStack
// emitted then read/write the persistent heap slot. This is valid in BOTH the
// synchronous / already-done cut (the await fast path falls through inline) and a
// real cross-thread suspend (the frame is shared across the activation), the
// same property named locals already rely on.
void Emitter::fixupSuspendDominance(llvm::Function* fn) {
    if (!fn || fn->isDeclaration()) return;

    // Iterate to a fixpoint: each pass recomputes the dominator tree, finds the
    // first instruction with a non-dominated use, and demotes it. Collect the
    // alloca each demotion creates so we can promote them into the heap frame.
    // Bounded by the instruction count (each instruction is demoted at most once).
    std::vector<llvm::AllocaInst*> demoted;
    bool changed = true;
    unsigned guard = 0;
    const unsigned cap = 100000;  // safety bound; never expected to be hit
    while (changed && guard++ < cap) {
        changed = false;
        llvm::DominatorTree dt(*fn);

        for (llvm::BasicBlock& bb : *fn) {
            for (llvm::Instruction& inst : bb) {
                // An alloca defines a pointer that is trivially available; skip.
                if (llvm::isa<llvm::AllocaInst>(&inst)) continue;

                bool offends = false;
                for (llvm::Use& use : inst.uses()) {
                    auto* user = llvm::dyn_cast<llvm::Instruction>(use.getUser());
                    if (!user) continue;
                    // A PHI use is checked against the corresponding incoming
                    // block, not the PHI's own block (LLVM dominance rule).
                    if (auto* phi = llvm::dyn_cast<llvm::PHINode>(user)) {
                        llvm::BasicBlock* incoming =
                            phi->getIncomingBlock(use);
                        if (!dt.dominates(inst.getParent(), incoming)) {
                            offends = true;
                            break;
                        }
                    } else if (!dt.dominates(&inst, user)) {
                        offends = true;
                        break;
                    }
                }
                if (!offends) continue;

                llvm::AllocaInst* slot = nullptr;
                if (auto* phi = llvm::dyn_cast<llvm::PHINode>(&inst))
                    slot = llvm::DemotePHIToStack(phi);
                else
                    slot = llvm::DemoteRegToStack(inst);
                if (slot) demoted.push_back(slot);
                changed = true;
                break;  // dominator tree is now stale; recompute next pass
            }
            if (changed) break;
        }
    }

    // No cross-suspend temporary → nothing to move into the frame; the spill-ptr
    // frame field stays null (harmless).
    if (demoted.empty()) return;

    // Promote the demoted stack slots into a heap spill area reached through the
    // frame, so they survive a cross-thread resume (§30.13 / #439-followup).
    promoteDemotedSlotsToFrame(fn, demoted);
}


// §30.2 / §30.13 (#439-followup): move the demoted cross-suspend stack slots
// into a heap spill area pointed to from the frame, so a real cross-thread
// resume reads the persisted values rather than garbage on the worker's fresh
// stack. `demoted` are the allocas fixupSuspendDominance created (one per
// cross-suspend temporary). We:
//   1. Lay out a spill struct { temp0, temp1, ... } from the demoted slots.
//   2. In the resume entry block, before the dispatch switch, lazily calloc the
//      spill area on the FIRST entry (when the frame's spill-ptr field is null)
//      and store its pointer back into the frame; on a later resume the existing
//      pointer is reused. The setup runs on every entry, so the per-field GEPs
//      it computes dominate all blocks reachable from the switch.
//   3. Replace each demoted alloca with its GEP into the heap spill area
//      (replaceAllUsesWith), so the store-before-suspend / load-after-resume
//      DemoteRegToStack emitted now target the persistent heap slot.
void Emitter::promoteDemotedSlotsToFrame(
    llvm::Function* fn, const std::vector<llvm::AllocaInst*>& demoted) {
    if (demoted.empty() || !smStateStruct_) return;

    // (1) Spill struct layout: one field per demoted slot, in stable order.
    std::vector<llvm::Type*> spillFields;
    spillFields.reserve(demoted.size());
    for (llvm::AllocaInst* a : demoted)
        spillFields.push_back(a->getAllocatedType());
    auto* spillStructTy = llvm::StructType::create(
        llvm_, spillFields, fn->getName().str() + ".spill");

    // (2) Resume entry setup, inserted before the dispatch switch (the entry
    // block's terminator). The frame pointer is the resume function's 2nd arg.
    llvm::Value* self = fn->getArg(1);
    llvm::BasicBlock& entryBB = fn->getEntryBlock();
    llvm::Instruction* term = entryBB.getTerminator();
    // The resume entry always ends in the dispatch switch (emitResumeDispatch);
    // bail defensively if that invariant is ever broken rather than crash.
    if (!term) return;

    llvm::IRBuilder<> b(term);
    // Load the current spill-area pointer from the frame.
    llvm::Value* spillField =
        b.CreateStructGEP(smStateStruct_, self, asyncSpillPtrField_);
    llvm::Value* curPtr = b.CreateLoad(ptrTy(), spillField, "spill.cur");
    llvm::Value* isNull = b.CreateICmpEQ(
        curPtr, llvm::ConstantPointerNull::get(ptrTy()), "spill.isnull");

    // Split off the dispatch (the terminator) into a continuation block, then
    // replace entry's fall-through with a null-check that callocs on first entry.
    llvm::BasicBlock* contBB =
        entryBB.splitBasicBlock(term, entryBB.getName() + ".spillcont");
    llvm::BasicBlock* allocBB =
        llvm::BasicBlock::Create(llvm_, entryBB.getName() + ".spillalloc", fn,
                                 contBB);
    // splitBasicBlock left an unconditional br entry->cont; rewrite it to the
    // null-check branch (the load/icmp above precede this terminator).
    entryBB.getTerminator()->eraseFromParent();
    {
        llvm::IRBuilder<> eb(&entryBB);
        eb.CreateCondBr(isNull, allocBB, contBB);
    }
    // allocBB: calloc(1, sizeof(spillStruct)), store into the frame field, br cont.
    llvm::Value* freshPtr = nullptr;
    {
        llvm::IRBuilder<> ab(allocBB);
        uint64_t sz = module_->getDataLayout().getTypeAllocSize(spillStructTy);
        auto* callocFn = declareC("calloc", ptrTy(), {i64(), i64()});
        freshPtr = ab.CreateCall(
            callocFn, {ab.getInt64(1), ab.getInt64((int64_t)sz)}, "spill.new");
        ab.CreateStore(freshPtr, spillField);
        ab.CreateBr(contBB);
    }
    // contBB head: PHI the spill pointer (existing vs. fresh), then GEP each
    // field. These dominate the dispatch switch and hence every state block.
    llvm::IRBuilder<> cb(&*contBB->getFirstInsertionPt());
    auto* spillPtr = cb.CreatePHI(ptrTy(), 2, "spill.ptr");
    spillPtr->addIncoming(curPtr, &entryBB);
    spillPtr->addIncoming(freshPtr, allocBB);

    // (3) Re-point each demoted alloca at its heap-spill GEP.
    for (unsigned i = 0; i < demoted.size(); ++i) {
        llvm::AllocaInst* a = demoted[i];
        llvm::Value* gep =
            cb.CreateStructGEP(spillStructTy, spillPtr, i,
                               a->getName().str() + ".spill");
        a->replaceAllUsesWith(gep);
        a->eraseFromParent();
    }
}


// §30.2 deferred tail: emit a well-formed but inert entry for an async method
// whose body codegen is deferred (await-in-try). The compile already failed via
// the diagnostic; this only keeps the emitted IR valid. The body is NOT lowered.
void Emitter::emitAsyncStub(sema::MethodSymbol* ms, llvm::Function* fn,
                            const CallableInfo& ci) {
    llvm::Type* resultTy = nullptr;
    if (ci.returnType) {
        auto* retRef = ctx_.resolvedType(*ci.returnType);
        if (retRef && !retRef->genericArgs.empty() && retRef->genericArgs[0])
            resultTy = lowerTypeRef(retRef->genericArgs[0]);
    }
    currentFn_ = fn;
    currentThis_ = nullptr;
    locals_.clear(); paramSlots_.clear(); refParams_.clear();
    disposeStack_.clear(); loopStack_.clear(); finallyStack_.clear();
    propagateBB_ = nullptr; unwindTarget_ = nullptr;
    currentCaughtExc_ = nullptr; unwindScopeDepth_ = 0;
    auto* entry = llvm::BasicBlock::Create(llvm_, "entry", fn);
    builder_.SetInsertPoint(entry);

    auto* allocFn = declareC("bz_task_alloc", ptrTy(), {});
    llvm::Value* task = builder_.CreateCall(allocFn, {}, "async.stub.task");
    auto* completeFn = declareC("bz_task_complete", llvm::Type::getVoidTy(llvm_),
                                {ptrTy(), ptrTy(), i64()});
    if (resultTy) {
        uint64_t sz = module_->getDataLayout().getTypeAllocSize(resultTy);
        llvm::AllocaInst* slot = entryAlloca(resultTy);
        builder_.CreateStore(llvm::Constant::getNullValue(resultTy), slot);
        builder_.CreateCall(completeFn, {task, slot, builder_.getInt64((int64_t)sz)});
    } else {
        builder_.CreateCall(completeFn,
                            {task, llvm::ConstantPointerNull::get(ptrTy()),
                             builder_.getInt64(0)});
    }
    if (isMain(ms))
        builder_.CreateRet(builder_.getInt32(0));
    else
        builder_.CreateRet(task);
}


// §30.1 / §30.9: complete the outer task. `value` is the lowered return value
// (null for a `Task` with no value, or a fall-off completion). The result is
// copied by the runtime (bz_task_complete malloc+memcpy's `result_size` bytes),
// so a stack slot is a fine source.
void Emitter::emitAsyncComplete(llvm::Value* value) {
    llvm::Value* self = smSelf_;
    if (!self || !smStateStruct_) return;
    llvm::Value* otField =
        builder_.CreateStructGEP(smStateStruct_, self, asyncOuterTaskField_);
    llvm::Value* outerTask = builder_.CreateLoad(ptrTy(), otField);

    auto* completeFn = declareC(
        "bz_task_complete", llvm::Type::getVoidTy(llvm_),
        {ptrTy(), ptrTy(), i64()});

    if (value && asyncResultTy_) {
        // Spill the value into a stack slot and hand its address + size to the
        // runtime (§30.1: `return expression;` completes the task with value).
        uint64_t sz = module_->getDataLayout().getTypeAllocSize(asyncResultTy_);
        llvm::AllocaInst* slot = entryAlloca(asyncResultTy_);
        builder_.CreateStore(value, slot);
        builder_.CreateCall(completeFn,
                            {outerTask, slot, builder_.getInt64((int64_t)sz)});
    } else {
        // `Task` (no value), or fall-off / `return;`: complete with null/0.
        builder_.CreateCall(completeFn,
                            {outerTask, llvm::ConstantPointerNull::get(ptrTy()),
                             builder_.getInt64(0)});
    }
    // §30.5 (minimal): the frame is dead once the task is completed on a terminal
    // path. In the synchronous / already-done cut the resume runs inline to
    // completion with no pending suspension, so freeing the frame here is safe and
    // keeps every async method (not just Main) leak-clean. (A real suspended
    // resume must keep the frame alive until the LAST resume; that terminal-only
    // free still holds — this runs only on the completing resume. Owned suspend-
    // locals would be disposed before this; the value-typed minimal cut has none.)
    auto* freeFn2 = declareC("free", llvm::Type::getVoidTy(llvm_), {ptrTy()});
    // §30.13 (#439-followup): free the cross-suspend spill area (if any) before
    // the frame. It is a side allocation pointed to from the frame's spill-ptr
    // field; the field is null when the body had no cross-suspend temporary, and
    // free(null) is a no-op, so this is unconditional and safe in every shape.
    emitFreeSpillArea(self);
    builder_.CreateCall(freeFn2, {self});
}


// §30.13 (#439-followup): free the heap spill area pointed to from the frame's
// spill-ptr field, if present. Emitted on each terminal path that frees the
// frame (emitAsyncComplete + the async.fault boundary). The field is null when
// fixupSuspendDominance allocated no spill area, and free(null) is a no-op, so
// this is safe regardless of body shape. Must run BEFORE free(self) (it reads
// the field through the frame pointer).
void Emitter::emitFreeSpillArea(llvm::Value* self) {
    if (!self || !smStateStruct_) return;
    llvm::Value* spillField =
        builder_.CreateStructGEP(smStateStruct_, self, asyncSpillPtrField_);
    llvm::Value* spillPtr = builder_.CreateLoad(ptrTy(), spillField, "spill.free");
    auto* freeFn = declareC("free", llvm::Type::getVoidTy(llvm_), {ptrTy()});
    builder_.CreateCall(freeFn, {spillPtr});
}


}  // namespace bezel::codegen
