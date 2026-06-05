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
// Deferred to later increments (not emitted here): await inside try/catch/
// finally (resuming into the right handler scope), §30.9 faulting re-raise
// beyond the basic check, atomic refcounts (§30 2c), async iterators
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


// §30.2 (deferred tail): detect an `await` that appears INSIDE a loop body
// (for / while). The flat state-block + dispatch-switch resume model produces
// malformed IR (a value computed in a resume block does not dominate its uses
// on the loop back-edge) for await-in-loop; that case needs CFG reconstruction
// and is deferred to a later increment. We detect it up front and emit a safe
// stub instead of malformed IR. `inLoop` becomes true once we recurse into a
// loop body.
bool Emitter::awaitInLoop(const std::vector<std::unique_ptr<ast::Stmt>>& stmts,
                          bool inLoop) {
    for (const auto& s : stmts) {
        if (!s) continue;
        if (auto* fs = dynamic_cast<const ast::ForStmt*>(s.get())) {
            if (awaitInLoop(fs->body, /*inLoop=*/true)) return true;
        } else if (auto* ws = dynamic_cast<const ast::WhileStmt*>(s.get())) {
            // An await in a while condition is itself a loop-carried await.
            if (exprContainsAwait(ws->condition.get())) return true;
            if (awaitInLoop(ws->body, /*inLoop=*/true)) return true;
        } else if (auto* ifs = dynamic_cast<const ast::IfStmt*>(s.get())) {
            if (inLoop) {
                if (exprContainsAwait(ifs->ifBranch.condition.get()))
                    return true;
                for (const auto& el : ifs->elifBranches)
                    if (exprContainsAwait(el.condition.get())) return true;
            }
            if (awaitInLoop(ifs->ifBranch.body, inLoop)) return true;
            for (const auto& el : ifs->elifBranches)
                if (awaitInLoop(el.body, inLoop)) return true;
            if (ifs->hasElse && awaitInLoop(ifs->elseBody, inLoop)) return true;
        } else if (auto* db = dynamic_cast<const ast::DoEndBlock*>(s.get())) {
            if (awaitInLoop(db->body, inLoop)) return true;
        } else if (inLoop) {
            // Inside a loop body, any await-bearing statement is the deferred case.
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
            }
        }
    }
    return false;
}


void Emitter::emitAsyncMethod(sema::MethodSymbol* ms, llvm::Function* fn,
                              const CallableInfo& ci) {
    if (!ci.stmts) return;
    smKind_ = SuspendKind::Await;

    // §30.2 deferred tail: `await` inside a loop body needs CFG reconstruction
    // (the flat resume model breaks SSA dominance across the loop back-edge).
    // Detect it and emit a safe, well-formed stub rather than malformed IR, with
    // a clear diagnostic so the limitation is visible (the IR verifier must stay
    // clean for all emitted async IR). The synchronous prefix + straight-line /
    // branch awaits are supported; await-in-loop is the next increment.
    if (awaitInLoop(*ci.stmts, /*inLoop=*/false)) {
        ctx_.diagnostics().report(
            Severity::Error,
            ms->decl ? ms->decl->span.begin : SourceLocation{},
            "not yet implemented: `await` inside a loop body (for/while) — the "
            "async state machine for await-in-loop is a deferred increment "
            "(§30.2)");
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

    // Build the kind-agnostic frame with two trailing async fields:
    //   [.. ] outerTask (ptr), [.. ] pendingTask (ptr).
    // Layout: { ptr vtable, i32 state, captures..., locals..., ptr outer, ptr pending }
    StateMachineFrame frame = buildStateMachineFrame(
        ms, *ci.stmts, {ptrTy(), ptrTy()},
        std::string("async.") + std::string(ms->name));
    auto* stateStructTy = frame.structTy;
    const unsigned STATE_FIELD = frame.stateField;
    const unsigned OUTER_TASK_FIELD = frame.currentValField;      // first trailing
    const unsigned PENDING_TASK_FIELD = frame.currentValField + 1;  // second trailing
    asyncOuterTaskField_ = OUTER_TASK_FIELD;
    asyncPendingTaskField_ = PENDING_TASK_FIELD;

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

        // §30.13: the runtime scheduler is NOT eagerly started here. For the
        // synchronous / already-done cut every await hits the fast path and runs
        // inline (bz_task_then and bz_task_complete both run continuations
        // inline when no scheduler exists), so no thread pool is needed and none
        // is spawned — keeping the process leak-free under LSan. A real
        // cross-thread suspend (bz_task_schedule on a producer that has not yet
        // completed) lazily initializes the pool in the runtime; that path is the
        // deferred concurrency increment.

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
            // §30.5 (minimal): reclaim the outer task (its result buffer) on the
            // normal completion path so the process is leak-clean. The frame was
            // already freed by the completing resume (emitAsyncComplete). Safe
            // here: the synchronous cut completes inline, so no continuation is
            // still in flight referencing the task.
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
        // here; only genuinely-uncaught faults do. (Resuming into a handler
        // scope across an await — await-in-try — is deferred; the minimal cut's
        // tests do not await inside try.) The original exception's TYPE is not
        // reconstructed across the task boundary yet — a generic message is used.
        auto* faultBB = llvm::BasicBlock::Create(llvm_, "async.fault", resumeFn);
        unwindTarget_ = faultBB;
        {
            llvm::IRBuilder<>::InsertPointGuard guard(builder_);
            builder_.SetInsertPoint(faultBB);
            llvm::Value* otField = builder_.CreateStructGEP(
                stateStructTy, self, OUTER_TASK_FIELD);
            llvm::Value* outerTask = builder_.CreateLoad(ptrTy(), otField);
            auto* completeErrFn = declareC("bz_task_complete_error",
                                           llvm::Type::getVoidTy(llvm_),
                                           {ptrTy(), ptrTy()});
            llvm::Value* msg = lowerStringLiteral("\"async method faulted\"", false);
            // bz_task_complete_error takes a C string (const char*); pass the
            // string object's data pointer.
            llvm::Value* msgData = loadStringData(msg);
            builder_.CreateCall(completeErrFn, {outerTask, msgData});
            // §30.5: the frame is dead on this terminal (fault) path — free it.
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

    // §30.9 (basic): a faulted awaited task propagates the fault. The minimal
    // cut forwards the fault UP THE AWAIT CHAIN: it completes THIS method's outer
    // task with the awaited task's error message (bz_task_complete_error) and
    // returns from resume. Whoever awaits this method's task then observes the
    // fault and propagates further; at the top (async Main) the entry's
    // bz_task_await_sync returns null and the process exits non-zero. This does
    // NOT yet resume into an enclosing try/catch handler scope — await-in-try and
    // re-raising the ORIGINAL exception object are deferred (§30.9). Emitted
    // unconditionally so `await Task` (no value) also propagates a fault.
    {
        auto* isErrFn = declareC("bz_task_is_error", i1(), {ptrTy()});
        llvm::Value* faulted =
            builder_.CreateCall(isErrFn, {awaitedTask}, "await.faulted");
        auto* faultBB = llvm::BasicBlock::Create(llvm_, "await.fault", currentFn_);
        auto* okBB = llvm::BasicBlock::Create(llvm_, "await.ok", currentFn_);
        builder_.CreateCondBr(faulted, faultBB, okBB);

        builder_.SetInsertPoint(faultBB);
        // Forward the awaited task's error message into the outer task.
        auto* errMsgFn = declareC("bz_task_error", ptrTy(), {ptrTy()});
        llvm::Value* msg = builder_.CreateCall(errMsgFn, {awaitedTask}, "await.errmsg");
        llvm::Value* otField = builder_.CreateStructGEP(
            smStateStruct_, smSelf_, asyncOuterTaskField_);
        llvm::Value* outerTask = builder_.CreateLoad(ptrTy(), otField);
        auto* completeErrFn = declareC("bz_task_complete_error",
                                       llvm::Type::getVoidTy(llvm_),
                                       {ptrTy(), ptrTy()});
        builder_.CreateCall(completeErrFn, {outerTask, msg});
        // §30.5: this fault path is terminal — reclaim the awaited task (if it
        // was an anonymous producer result) and the frame before returning.
        auto* freeFn2 = declareC("free", llvm::Type::getVoidTy(llvm_), {ptrTy()});
        if (dynamic_cast<const ast::CallExpr*>(un.operand.get())) {
            auto* freeTaskFn = declareC("bz_task_free",
                                        llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(freeTaskFn, {awaitedTask});
        }
        builder_.CreateCall(freeFn2, {smSelf_});
        builder_.CreateRetVoid();

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


// §30.2 deferred tail: emit a well-formed but inert entry for an async method
// whose body codegen is deferred (await-in-loop). The compile already failed via
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
    builder_.CreateCall(freeFn2, {self});
}


}  // namespace bezel::codegen
