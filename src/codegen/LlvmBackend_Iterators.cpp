#include "codegen/LlvmBackend_impl.hpp"

namespace bezel::codegen {

// ===========================================================================
// Suspend/resume state-machine scaffolding (§29 iterators / §30 async).
//
// Phase 2a extracted the kind-agnostic machinery — frame layout, the spill set
// (locals live across a suspend point), capture spill/restore, and the resume
// switch — out of the yield-specific iterator emitter so the async (`await`)
// path can reuse it (§30). The iterator (`Yield`) path below layers the
// yield-specific driver (IEnumerator<T> MoveNext/Current/Dispose + vtable) on
// top of those helpers. No behavior changes for iterators.
// ===========================================================================

void Emitter::collectSuspendLocals(
    const std::vector<std::unique_ptr<ast::Stmt>>& body,
    std::vector<std::pair<const ast::Node*, llvm::Type*>>& out) {
    for (const auto& s : body) {
        if (!s) continue;
        if (auto* ld = dynamic_cast<const ast::LocalDecl*>(s.get())) {
            // #461 (§30 async frames): the frame SLOT type for a suspend-live
            // local must be at least as wide as its alloca, or the per-suspend
            // spill (which stores `alloca->getAllocatedType()` bytes into the
            // slot — see emitAwaitSuspend) overflows into the adjacent frame
            // field and corrupts it (→ SIGSEGV on resume). When neither an
            // annotation nor an initializer exprType is available, default to a
            // POINTER-WIDTH slot (not i32): ptr is large enough to hold any
            // scalar alloca (i32/i64/bool/ptr) without overflow, so a local with
            // an unresolved type can never clobber its neighbours. With a known
            // type the exact lowered type is used (so an `int?`/record/value
            // struct still gets its precise slot and round-trips correctly).
            llvm::Type* ty = ptrTy();
            if (ld->type) {
                if (auto* rt = ctx_.resolvedType(*ld->type)) ty = lowerTypeRef(rt);
            } else if (ld->initializer) {
                if (auto* rt = ctx_.exprType(*ld->initializer)) ty = lowerTypeRef(rt);
            }
            out.push_back({ld, ty});
        } else if (auto* fs = dynamic_cast<const ast::ForStmt*>(s.get())) {
            // The loop variable is anchored on the ForStmt node.
            if (!fs->varName.empty()) {
                llvm::Type* ty = i32();  // range/int loop var default
                if (fs->varType) {
                    if (auto* rt = ctx_.resolvedType(*fs->varType)) ty = lowerTypeRef(rt);
                }
                out.push_back({fs, ty});
            }
            collectSuspendLocals(fs->body, out);
        } else if (auto* ws = dynamic_cast<const ast::WhileStmt*>(s.get())) {
            collectSuspendLocals(ws->body, out);
        } else if (auto* ifs = dynamic_cast<const ast::IfStmt*>(s.get())) {
            collectSuspendLocals(ifs->ifBranch.body, out);
            for (const auto& el : ifs->elifBranches)
                collectSuspendLocals(el.body, out);
            if (ifs->hasElse) collectSuspendLocals(ifs->elseBody, out);
        } else if (auto* db = dynamic_cast<const ast::DoEndBlock*>(s.get())) {
            collectSuspendLocals(db->body, out);
        } else if (auto* ts = dynamic_cast<const ast::TryStmt*>(s.get())) {
            // §30.9 / §31.2 (#438): a named local declared in a try BODY, a catch
            // ARM body, or a FINALLY that lives across an in-region await must be
            // frame-backed too (the resume re-enters the region across the
            // suspend). collectSuspendLocals previously skipped try regions because
            // await-in-catch/finally was deferred; now that those are supported,
            // recurse so e.g. `catch E => do var a := await f(); var b := await
            // g(); … end` round-trips `a` through the frame across the cross-thread
            // hop. The catch-arm exception BINDING is frame-backed separately (it
            // is not a LocalDecl — lowerTry registers its slot in
            // asyncHandlerSpillSlots_).
            collectSuspendLocals(ts->body, out);
            for (const auto& arm : ts->catches) {
                const ast::Stmt* ab = arm.body.get();
                if (auto* adb = dynamic_cast<const ast::DoEndBlock*>(ab))
                    collectSuspendLocals(adb->body, out);
                else if (auto* ald = dynamic_cast<const ast::LocalDecl*>(ab)) {
                    // A one-liner `catch E => var a := await f();` arm body.
                    llvm::Type* ty = i32();
                    if (ald->type) {
                        if (auto* rt = ctx_.resolvedType(*ald->type))
                            ty = lowerTypeRef(rt);
                    } else if (ald->initializer) {
                        if (auto* rt = ctx_.exprType(*ald->initializer))
                            ty = lowerTypeRef(rt);
                    }
                    out.push_back({ald, ty});
                }
            }
            if (ts->hasFinally) collectSuspendLocals(ts->finallyBody, out);
        }
    }
}


Emitter::StateMachineFrame Emitter::buildStateMachineFrame(
    sema::MethodSymbol* ms,
    const std::vector<std::unique_ptr<ast::Stmt>>& body,
    llvm::ArrayRef<llvm::Type*> extraTrailing,
    const std::string& structName) {
    StateMachineFrame frame;

    // Collect all parameters to capture: `this` (instance methods) then params.
    bool isInstance = !isStaticMethod(ms);
    if (isInstance)
        frame.captures.push_back({nullptr, ptrTy()});  // 'this' capture
    for (auto* param : ms->params) {
        auto* ps = dynamic_cast<sema::ParameterSymbol*>(param);
        if (!ps) continue;
        llvm::Type* pty = ptrTy();  // default ptr for class params
        if (ps->declType) {
            if (auto* rt = ctx_.resolvedType(*ps->declType))
                pty = lowerTypeRef(rt);
        }
        frame.captures.push_back({ps, pty});
    }

    // Pre-scan body to find all locals that need to live across a suspend point.
    collectSuspendLocals(body, frame.bodyLocals);

    // Build state struct type:
    //   { ptr vtable, i32 state, captures..., locals..., extraTrailing... }
    std::vector<llvm::Type*> fields;
    fields.push_back(ptrTy());   // vtable  → field 0
    fields.push_back(i32());     // state   → field 1
    for (auto& [sym, ty] : frame.captures) fields.push_back(ty);  // captures
    frame.localStart = (unsigned)(2 + frame.captures.size());
    for (auto& [node, ty] : frame.bodyLocals) fields.push_back(ty);  // locals
    // Kind-specific trailing fields (e.g. `Yield`'s `current_val`). Record the
    // index of the first one as `currentValField` for the yield driver.
    frame.currentValField =
        (unsigned)(frame.localStart + frame.bodyLocals.size());
    for (llvm::Type* ty : extraTrailing) fields.push_back(ty);

    frame.structTy = llvm::StructType::create(llvm_, fields, structName);
    frame.vtableField = 0;
    frame.stateField = 1;
    frame.captureStart = 2;

    // Build mapping from AST nodes to struct field indices.
    for (unsigned li = 0; li < frame.bodyLocals.size(); ++li)
        frame.localFields[frame.bodyLocals[li].first] = frame.localStart + li;

    return frame;
}


void Emitter::emitFrameCaptureStores(const StateMachineFrame& frame,
                                     llvm::Value* st) {
    unsigned idx = frame.captureStart;
    for (auto& [sym, ty] : frame.captures) {
        llvm::Value* capField =
            builder_.CreateStructGEP(frame.structTy, st, idx++);
        llvm::Value* val = nullptr;
        if (!sym) {
            // 'this' capture
            val = currentThis_ ? currentThis_ : currentFn_->arg_begin();
        } else {
            auto it = paramSlots_.find(sym);
            if (it != paramSlots_.end())
                val = builder_.CreateLoad(ty, it->second);
        }
        if (val) builder_.CreateStore(val, capField);
    }
}


void Emitter::emitFrameCaptureRestores(const StateMachineFrame& frame,
                                       llvm::Value* self) {
    unsigned idx = frame.captureStart;
    for (auto& [sym, ty] : frame.captures) {
        llvm::Value* capField =
            builder_.CreateStructGEP(frame.structTy, self, idx++);
        llvm::Value* val = builder_.CreateLoad(ty, capField);
        if (!sym) {
            // 'this' — set currentThis_
            currentThis_ = val;
        } else {
            // param — create a slot and store it
            auto* slot = entryAlloca(ty);
            builder_.CreateStore(val, slot);
            paramSlots_[sym] = slot;
        }
    }
}


std::vector<llvm::BasicBlock*> Emitter::emitResumeDispatch(
    const StateMachineFrame& frame, llvm::Value* self,
    llvm::Value* stateGEP, unsigned numSuspends,
    const std::function<void()>& emitDone) {
    // State 0: beginning. States 1..N: after each suspend point. The switch's
    // default + state -1 land in `doneBB`, which emits the kind's exhausted-
    // return.
    auto* doneBB = llvm::BasicBlock::Create(llvm_, "mn.done", currentFn_);
    std::vector<llvm::BasicBlock*> stateBBs;
    for (unsigned s = 0; s <= numSuspends; ++s)
        stateBBs.push_back(
            llvm::BasicBlock::Create(llvm_, "mn.s" + std::to_string(s),
                                     currentFn_));

    llvm::Value* curState = builder_.CreateLoad(i32(), stateGEP);
    auto* sw = builder_.CreateSwitch(curState, doneBB, numSuspends + 1);
    for (unsigned s = 0; s <= numSuspends; ++s)
        sw->addCase(builder_.getInt32((int)s), stateBBs[s]);

    // doneBB: exhausted → kind-specific return.
    builder_.SetInsertPoint(doneBB);
    emitDone();

    // Publish the live state so the per-suspend-point spill/restore (lowerStmt)
    // can run.
    smStateStruct_ = frame.structTy;
    smSelf_ = self;
    smStateGEP_ = stateGEP;
    smStateBBs_ = stateBBs;
    smDoneBB_ = doneBB;
    smNextSuspendState_ = 0;
    smLocalFields_ = frame.localFields;

    return stateBBs;
}


bool Emitter::containsYield(const std::vector<std::unique_ptr<ast::Stmt>>& stmts) {
    for (const auto& s : stmts) {
        if (!s) continue;
        if (dynamic_cast<const ast::YieldStmt*>(s.get())) return true;
        // Recurse into compound statements.
        if (auto* fs = dynamic_cast<const ast::ForStmt*>(s.get()))
            if (containsYield(fs->body)) return true;
        if (auto* ws = dynamic_cast<const ast::WhileStmt*>(s.get()))
            if (containsYield(ws->body)) return true;
        if (auto* ifs = dynamic_cast<const ast::IfStmt*>(s.get())) {
            if (containsYield(ifs->ifBranch.body)) return true;
            for (const auto& el : ifs->elifBranches)
                if (containsYield(el.body)) return true;
            if (ifs->hasElse && containsYield(ifs->elseBody)) return true;
        }
        if (auto* db = dynamic_cast<const ast::DoEndBlock*>(s.get()))
            if (containsYield(db->body)) return true;
        // §30.7 (#440): recurse into a try / catch / finally so a `yield` inside a
        // protected region still classifies the method as an iterator. The async
        // iterator path then rejects that shape (suspendInTryPosition); a sync
        // iterator's yield-in-try is already rejected earlier by sema flow checks.
        if (auto* ts = dynamic_cast<const ast::TryStmt*>(s.get())) {
            if (containsYield(ts->body)) return true;
            for (const auto& arm : ts->catches) {
                const ast::Stmt* ab = arm.body.get();
                if (auto* db = dynamic_cast<const ast::DoEndBlock*>(ab)) {
                    if (containsYield(db->body)) return true;
                } else if (dynamic_cast<const ast::YieldStmt*>(ab)) {
                    return true;
                }
            }
            if (containsYield(ts->finallyBody)) return true;
        }
    }
    return false;
}


void Emitter::collectYields(const std::vector<std::unique_ptr<ast::Stmt>>& stmts,
                           std::vector<const ast::YieldStmt*>& out) {
    for (const auto& s : stmts) {
        if (!s) continue;
        if (auto* ys = dynamic_cast<const ast::YieldStmt*>(s.get())) {
            // §29.3: only `yield return` consumes a resume-state slot; `yield break`
            // is a terminal that unconditionally returns false from MoveNext without
            // advancing the state counter. Counting it here would create a dangling
            // state block (mn.sN) with no predecessor and no terminator, which the
            // IR verifier rejects. (#437 fix)
            if (!ys->isBreak) out.push_back(ys);
            continue;
        }
        if (auto* fs = dynamic_cast<const ast::ForStmt*>(s.get()))
            collectYields(fs->body, out);
        else if (auto* ws = dynamic_cast<const ast::WhileStmt*>(s.get()))
            collectYields(ws->body, out);
        else if (auto* ifs = dynamic_cast<const ast::IfStmt*>(s.get())) {
            collectYields(ifs->ifBranch.body, out);
            for (const auto& el : ifs->elifBranches)
                collectYields(el.body, out);
            if (ifs->hasElse) collectYields(ifs->elseBody, out);
        } else if (auto* db = dynamic_cast<const ast::DoEndBlock*>(s.get())) {
            collectYields(db->body, out);
        } else if (auto* ts = dynamic_cast<const ast::TryStmt*>(s.get())) {
            // §29.6 / §30.7 (#554): `yield return` inside a try body with ONLY a
            // finally is now supported in async iterators. Collect yields from the
            // try body so the resume-switch has the correct number of state blocks.
            // Do NOT collect from catch arm bodies or the finally: §29.6 forbids
            // yield there, and suspendInTryPosition rejects such programs before
            // the resume switch is built.
            collectYields(ts->body, out);
        }
    }
}


void Emitter::emitIteratorMethod(sema::MethodSymbol* ms, llvm::Function* fn,
                         const CallableInfo& ci) {
    if (!ci.stmts) return;
    smKind_ = SuspendKind::Yield;

    // Determine element type T from the return type's generic arg.
    llvm::Type* elemTy = i32();  // default
    if (ci.returnType) {
        auto* retRef = ctx_.resolvedType(*ci.returnType);
        if (retRef && !retRef->genericArgs.empty() && retRef->genericArgs[0])
            elemTy = lowerTypeRef(retRef->genericArgs[0]);
    }

    // Build the kind-agnostic frame; the yield driver's one extra trailing
    // field is the `current_val` of element type T.
    // Layout: { ptr vtable, i32 state, captures..., locals..., T current_val }
    StateMachineFrame frame = buildStateMachineFrame(
        ms, *ci.stmts, {elemTy},
        std::string("iter.") + std::string(ms->name));
    auto* stateStructTy = frame.structTy;
    const unsigned VTABLE_FIELD = frame.vtableField;
    const unsigned STATE_FIELD = frame.stateField;
    const unsigned CURRENT_VAL_FIELD = frame.currentValField;

    bool isInstance = !isStaticMethod(ms);
    std::string prefix = std::string("bz$iter$") + std::string(ms->name) + "$";

    // --- MoveNext(ptr self) -> i1 ---
    auto* moveNextTy = llvm::FunctionType::get(i1(), {ptrTy()}, false);
    auto* moveNextFn = llvm::Function::Create(
        moveNextTy, llvm::Function::InternalLinkage,
        prefix + "MoveNext", module_.get());

    // --- get$Current(ptr self) -> T ---
    auto* currentTy = llvm::FunctionType::get(elemTy, {ptrTy()}, false);
    auto* currentFn = llvm::Function::Create(
        currentTy, llvm::Function::InternalLinkage,
        prefix + "Current", module_.get());

    // --- Dispose(ptr self) -> void ---
    // §29.7 (#623): the real Dispose is emitted AFTER MoveNext so we know
    // which yield states have active finally chains (smIterFinallyAtState_).
    // Declare the function now so the vtable can reference it; fill the body
    // below after MoveNext emission.
    auto* disposeFn = llvm::Function::Create(
        llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_), {ptrTy()}, false),
        llvm::Function::InternalLinkage, prefix + "Dispose", module_.get());

    // --- Vtable global for this iterator's IEnumerator<T> impl ---
    // Layout: [ptr vtable (self-ptr for td), ptr Dispose, ptr MoveNext, ptr get$Current]
    // We reuse the td format with a synthetic descriptor.
    // For simplicity, just make a constant array with 4 pointers.
    const unsigned VT_SIZE = 4;
    std::vector<llvm::Constant*> vtElems(VT_SIZE, llvm::ConstantPointerNull::get(ptrTy()));
    // Slot 0: type descriptor (point to itself as a sentinel)
    auto* vtGVType = llvm::ArrayType::get(ptrTy(), VT_SIZE);
    auto* vtGV = new llvm::GlobalVariable(*module_, vtGVType, true,
        llvm::GlobalValue::InternalLinkage,
        llvm::ConstantArray::get(vtGVType, vtElems),
        prefix + "vtable");
    // Fill: [0]=self-ptr, [1]=Dispose, [2]=MoveNext, [3]=Current
    vtElems[0] = llvm::ConstantExpr::getPointerCast(vtGV, ptrTy());
    vtElems[1] = disposeFn;
    vtElems[2] = moveNextFn;
    vtElems[3] = currentFn;
    vtGV->setInitializer(llvm::ConstantArray::get(vtGVType, vtElems));

    // --- GetEnumerator (= the original method body): alloc + fill struct ---
    // fn is the already-declared function for this method.
    // We reuse its parameter list but change its body.
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

        // Allocate state struct
        uint64_t stSz = module_->getDataLayout().getTypeAllocSize(stateStructTy);
        auto* callocFn2 = declareC("calloc", ptrTy(), {i64(), i64()});
        llvm::Value* st = builder_.CreateCall(callocFn2,
            {builder_.getInt64(1), builder_.getInt64(stSz)}, "iter.state");
        // Store vtable at field 0
        llvm::Value* vtField = builder_.CreateStructGEP(stateStructTy, st, VTABLE_FIELD);
        builder_.CreateStore(vtGV, vtField);
        // Store state = 0
        llvm::Value* stateField = builder_.CreateStructGEP(stateStructTy, st, STATE_FIELD);
        builder_.CreateStore(builder_.getInt32(0), stateField);
        // Store each capture (this + params) into the frame.
        emitFrameCaptureStores(frame, st);
        // Return the state struct (as ptr)
        builder_.CreateRet(st);
    }

    // --- Current: load current_val from struct ---
    {
        auto* entry = llvm::BasicBlock::Create(llvm_, "entry", currentFn);
        llvm::IRBuilder<> cb(entry);
        llvm::Value* self = &*currentFn->arg_begin();
        llvm::Value* cvField = cb.CreateStructGEP(stateStructTy, self, CURRENT_VAL_FIELD);
        cb.CreateRet(cb.CreateLoad(elemTy, cvField));
    }

    // --- MoveNext: the state machine body ---
    // We emit MoveNext by re-running the method body in a special "iterator" mode.
    // This re-uses lowerScopedBlock but with yield points turned into
    // state transitions.
    {
        currentFn_ = moveNextFn;
        currentThis_ = nullptr;
        locals_.clear(); paramSlots_.clear(); refParams_.clear();
        disposeStack_.clear(); loopStack_.clear(); finallyStack_.clear();
        // §29.7 (#623): clear the finally-chain tracking for this iterator.
        finallyBodyStack_.clear();
        smIterFinallyAtState_.clear();
        propagateBB_ = nullptr; unwindTarget_ = nullptr;
        currentCaughtExc_ = nullptr; unwindScopeDepth_ = 0;
        auto* entry = llvm::BasicBlock::Create(llvm_, "entry", moveNextFn);
        builder_.SetInsertPoint(entry);

        llvm::Value* self = &*moveNextFn->arg_begin();

        // Restore captured params (this + params) from the frame.
        emitFrameCaptureRestores(frame, self);

        // State field GEP — read by the resume dispatch, written at each yield.
        llvm::Value* stateGEP =
            builder_.CreateStructGEP(stateStructTy, self, STATE_FIELD);

        // Collect all yield stmts and assign state numbers.
        std::vector<const ast::YieldStmt*> yields;
        collectYields(*ci.stmts, yields);
        unsigned numYields = (unsigned)yields.size();

        // Emit the resume switch + state blocks. `Yield`'s exhausted-return is
        // `ret false` (MoveNext: "no more elements").
        std::vector<llvm::BasicBlock*> stateBBs = emitResumeDispatch(
            frame, self, stateGEP, numYields,
            [&]() { builder_.CreateRet(builder_.getFalse()); });

        // Yield-specific live state for the YieldStmt handler in lowerStmt.
        iterCurrentValField_ = CURRENT_VAL_FIELD;
        iterElemTy_ = elemTy;

        // Emit MoveNext body starting from state 0.
        // The body is emitted linearly; YieldStmt handlers interrupt it to
        // emit the yield transition and jump to the next state block.
        builder_.SetInsertPoint(stateBBs[0]);
        disposeStack_.emplace_back();
        bool term = lowerBody(*ci.stmts);
        disposeStack_.pop_back();
        // If body didn't terminate, fall through to done.
        if (!term && !builder_.GetInsertBlock()->getTerminator()) {
            builder_.CreateStore(builder_.getInt32(-1), smStateGEP_);
            builder_.CreateRet(builder_.getFalse());
        }

        // Clear state-machine state.
        smStateStruct_ = nullptr;
        smSelf_ = nullptr;
        smLocalFields_.clear();
        smAllocaToField_.clear();
    }

    // --- Dispose(ptr self) -> void (real body, §29.7 #623) ---
    // Emitted after MoveNext so smIterFinallyAtState_ is populated.
    // If the iterator was suspended inside a try/finally, run the finally
    // chains (innermost first) before freeing the frame.  A second Dispose
    // (state == -1) is a no-op per §29.7.
    {
        currentFn_ = disposeFn;
        currentThis_ = nullptr;
        locals_.clear(); paramSlots_.clear(); refParams_.clear();
        disposeStack_.clear(); loopStack_.clear(); finallyStack_.clear();
        finallyBodyStack_.clear();
        propagateBB_ = nullptr; unwindTarget_ = nullptr;
        currentCaughtExc_ = nullptr; unwindScopeDepth_ = 0;

        auto* entry = llvm::BasicBlock::Create(llvm_, "entry", disposeFn);
        builder_.SetInsertPoint(entry);

        llvm::Value* self = &*disposeFn->arg_begin();

        // Restore captures so finally bodies can access `this` / params.
        emitFrameCaptureRestores(frame, self);

        // Restore suspend-locals from the frame so finally bodies that
        // reference iterator-body locals round-trip correctly.
        for (auto& [node, fieldIdx] : frame.localFields) {
            llvm::Type* lty =
                frame.bodyLocals[fieldIdx - frame.localStart].second;
            llvm::Value* fptr = builder_.CreateStructGEP(
                stateStructTy, self, fieldIdx);
            llvm::Value* val = builder_.CreateLoad(lty, fptr);
            auto* slot = entryAlloca(lty);
            builder_.CreateStore(val, slot);
            locals_[node] = slot;
        }

        auto* freeFn = declareC("free", llvm::Type::getVoidTy(llvm_), {ptrTy()});

        // If no yield was ever inside a try/finally, emit a simple free + return.
        if (smIterFinallyAtState_.empty()) {
            // Mark done so a second Dispose is a no-op (not strictly necessary
            // when there are no finally chains, but keeps the contract clean).
            llvm::Value* stateGEP2 =
                builder_.CreateStructGEP(stateStructTy, self, STATE_FIELD);
            builder_.CreateStore(builder_.getInt32(-1), stateGEP2);
            builder_.CreateCall(freeFn, {self});
            builder_.CreateRetVoid();
        } else {
            // Build a switch on the current state.  State == -1 or 0: already
            // done or never suspended inside a try — just free.  State N: run
            // the finally chain recorded for that state, then free.
            auto* doneBB = llvm::BasicBlock::Create(llvm_, "dsp.done", disposeFn);
            llvm::Value* stateGEP2 =
                builder_.CreateStructGEP(stateStructTy, self, STATE_FIELD);
            llvm::Value* curState = builder_.CreateLoad(i32(), stateGEP2);
            // Mark done immediately so re-entrant Dispose is a no-op.
            builder_.CreateStore(builder_.getInt32(-1), stateGEP2);
            auto* sw = builder_.CreateSwitch(curState, doneBB,
                (unsigned)smIterFinallyAtState_.size());

            for (auto& [stateIdx, chain] : smIterFinallyAtState_) {
                auto* chainBB = llvm::BasicBlock::Create(
                    llvm_, "dsp.s" + std::to_string(stateIdx), disposeFn);
                sw->addCase(builder_.getInt32((int)stateIdx), chainBB);
                builder_.SetInsertPoint(chainBB);
                // Run finally bodies innermost-first (chain is outermost-first,
                // so iterate in reverse per §29.7 / §28.11).
                for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
                    disposeStack_.emplace_back();
                    lowerBody(**it);
                    disposeStack_.pop_back();
                }
                builder_.CreateBr(doneBB);
            }

            builder_.SetInsertPoint(doneBB);
            builder_.CreateCall(freeFn, {self});
            builder_.CreateRetVoid();
        }

        // Clear the per-iterator finally tracking.
        smIterFinallyAtState_.clear();
        finallyBodyStack_.clear();
    }
}


}  // namespace bezel::codegen
