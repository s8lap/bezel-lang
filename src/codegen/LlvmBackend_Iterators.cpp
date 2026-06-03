#include "codegen/LlvmBackend_impl.hpp"

namespace bezel::codegen {

void Emitter::collectIterLocals(
    const std::vector<std::unique_ptr<ast::Stmt>>& body,
    std::vector<std::pair<const ast::Node*, llvm::Type*>>& out) {
    for (const auto& s : body) {
        if (!s) continue;
        if (auto* ld = dynamic_cast<const ast::LocalDecl*>(s.get())) {
            llvm::Type* ty = i32();
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
            collectIterLocals(fs->body, out);
        } else if (auto* ws = dynamic_cast<const ast::WhileStmt*>(s.get())) {
            collectIterLocals(ws->body, out);
        } else if (auto* ifs = dynamic_cast<const ast::IfStmt*>(s.get())) {
            collectIterLocals(ifs->ifBranch.body, out);
            for (const auto& el : ifs->elifBranches)
                collectIterLocals(el.body, out);
            if (ifs->hasElse) collectIterLocals(ifs->elseBody, out);
        } else if (auto* db = dynamic_cast<const ast::DoEndBlock*>(s.get())) {
            collectIterLocals(db->body, out);
        }
    }
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
    }
    return false;
}


void Emitter::collectYields(const std::vector<std::unique_ptr<ast::Stmt>>& stmts,
                           std::vector<const ast::YieldStmt*>& out) {
    for (const auto& s : stmts) {
        if (!s) continue;
        if (auto* ys = dynamic_cast<const ast::YieldStmt*>(s.get()))
            { out.push_back(ys); continue; }
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
        }
    }
}


void Emitter::emitIteratorMethod(sema::MethodSymbol* ms, llvm::Function* fn,
                         const CallableInfo& ci) {
    if (!ci.stmts) return;
    // Determine element type T from the return type's generic arg.
    llvm::Type* elemTy = i32();  // default
    if (ci.returnType) {
        auto* retRef = ctx_.resolvedType(*ci.returnType);
        if (retRef && !retRef->genericArgs.empty() && retRef->genericArgs[0])
            elemTy = lowerTypeRef(retRef->genericArgs[0]);
    }

    // Collect all parameters to capture.
    std::vector<std::pair<sema::Symbol*, llvm::Type*>> captures;
    bool isInstance = !isStaticMethod(ms);
    if (isInstance)
        captures.push_back({nullptr, ptrTy()});  // 'this' capture
    for (auto* param : ms->params) {
        auto* ps = dynamic_cast<sema::ParameterSymbol*>(param);
        if (!ps) continue;
        llvm::Type* pty = ptrTy();  // default ptr for class params
        if (ps->declType) {
            if (auto* rt = ctx_.resolvedType(*ps->declType))
                pty = lowerTypeRef(rt);
        }
        captures.push_back({ps, pty});
    }

    // Pre-scan body to find all locals that need to live across yield points.
    std::vector<std::pair<const ast::Node*, llvm::Type*>> bodyLocals;
    if (ci.stmts) collectIterLocals(*ci.stmts, bodyLocals);

    // Build state struct type:
    // { ptr vtable, i32 state, captures..., locals..., T current_val }
    std::vector<llvm::Type*> fields;
    fields.push_back(ptrTy());   // vtable  → field 0
    fields.push_back(i32());     // state   → field 1
    for (auto& [sym, ty] : captures) fields.push_back(ty);  // params → fields 2..N
    // Body locals → fields N+1..M
    const unsigned LOCAL_START = (unsigned)(2 + captures.size());
    for (auto& [node, ty] : bodyLocals) fields.push_back(ty);
    fields.push_back(elemTy);    // current_val → last field
    auto* stateStructTy = llvm::StructType::create(llvm_, fields,
        std::string("iter.") + std::string(ms->name));

    const unsigned VTABLE_FIELD = 0;
    const unsigned STATE_FIELD = 1;
    const unsigned CAPTURE_START = 2;
    const unsigned CURRENT_VAL_FIELD = (unsigned)(2 + captures.size() + bodyLocals.size());

    // Build mapping from AST nodes to struct field indices.
    std::unordered_map<const ast::Node*, unsigned> localFields;
    for (unsigned li = 0; li < bodyLocals.size(); ++li)
        localFields[bodyLocals[li].first] = LOCAL_START + li;

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
    auto* disposeFn = llvm::Function::Create(
        llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_), {ptrTy()}, false),
        llvm::Function::InternalLinkage, prefix + "Dispose", module_.get());
    {
        auto* bb = llvm::BasicBlock::Create(llvm_, "entry", disposeFn);
        llvm::IRBuilder<> db(bb);
        db.CreateRetVoid();
    }

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
        // Store each capture
        unsigned ci_idx = CAPTURE_START;
        for (auto& [sym, ty] : captures) {
            llvm::Value* capField = builder_.CreateStructGEP(stateStructTy, st, ci_idx++);
            llvm::Value* val = nullptr;
            if (!sym) {
                // 'this' capture
                val = currentThis_ ? currentThis_ : fn->arg_begin();
            } else {
                auto it = paramSlots_.find(sym);
                if (it != paramSlots_.end())
                    val = builder_.CreateLoad(ty, it->second);
            }
            if (val) builder_.CreateStore(val, capField);
        }
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
        propagateBB_ = nullptr; unwindTarget_ = nullptr;
        currentCaughtExc_ = nullptr; unwindScopeDepth_ = 0;
        auto* entry = llvm::BasicBlock::Create(llvm_, "entry", moveNextFn);
        builder_.SetInsertPoint(entry);

        llvm::Value* self = &*moveNextFn->arg_begin();

        // Restore captured params from struct.
        unsigned ci_idx2 = CAPTURE_START;
        for (auto& [sym, ty] : captures) {
            llvm::Value* capField = builder_.CreateStructGEP(stateStructTy, self, ci_idx2++);
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

        // Read state field.
        llvm::Value* stateGEP = builder_.CreateStructGEP(stateStructTy, self, STATE_FIELD);
        llvm::Value* curState = builder_.CreateLoad(i32(), stateGEP);

        // Collect all yield stmts and assign state numbers.
        std::vector<const ast::YieldStmt*> yields;
        collectYields(*ci.stmts, yields);
        unsigned numYields = (unsigned)yields.size();

        // Create basic blocks for each state.
        // State 0: beginning, State 1..N: after each yield, State -1: done
        auto* doneBB = llvm::BasicBlock::Create(llvm_, "mn.done", currentFn_);
        std::vector<llvm::BasicBlock*> stateBBs;
        for (unsigned s = 0; s <= numYields; ++s) {
            stateBBs.push_back(
                llvm::BasicBlock::Create(llvm_, "mn.s" + std::to_string(s), currentFn_));
        }

        // Switch dispatch on state.
        auto* sw = builder_.CreateSwitch(curState, doneBB, numYields + 1);
        for (unsigned s = 0; s <= numYields; ++s)
            sw->addCase(builder_.getInt32((int)s), stateBBs[s]);

        // doneBB: exhausted → return false
        builder_.SetInsertPoint(doneBB);
        builder_.CreateRet(builder_.getFalse());

        // Set up the yield callback so lowerStmt knows what to do.
        // We use iterYields_ and iterStateGEP_ members.
        iterStateStruct_ = stateStructTy;
        iterSelf_ = self;
        iterStateGEP_ = stateGEP;
        iterStateBBs_ = stateBBs;
        iterDoneBB_ = doneBB;
        iterCurrentValField_ = CURRENT_VAL_FIELD;
        iterNextYieldState_ = 0;  // incremented as yields are processed
        iterElemTy_ = elemTy;
        iterLocalFields_ = localFields;

        // Emit MoveNext body starting from state 0.
        // The body is emitted linearly; YieldStmt handlers interrupt it to
        // emit the yield transition and jump to the next state block.
        builder_.SetInsertPoint(stateBBs[0]);
        disposeStack_.emplace_back();
        bool term = lowerBody(*ci.stmts);
        disposeStack_.pop_back();
        // If body didn't terminate, fall through to done.
        if (!term && !builder_.GetInsertBlock()->getTerminator()) {
            builder_.CreateStore(builder_.getInt32(-1), iterStateGEP_);
            builder_.CreateRet(builder_.getFalse());
        }

        // Clear iterator state.
        iterStateStruct_ = nullptr;
        iterSelf_ = nullptr;
        iterLocalFields_.clear();
        iterAllocaToField_.clear();
    }
}


}  // namespace bezel::codegen
