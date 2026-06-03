#include "codegen/LlvmBackend_impl.hpp"

namespace bezel::codegen {

llvm::Type* Emitter::lowerPrimitive(sema::PrimitiveType p) {
    using P = sema::PrimitiveType;
    switch (p) {
        case P::Bool:   return i1();
        case P::Float:  return llvm::Type::getFloatTy(llvm_);
        case P::Double: return llvm::Type::getDoubleTy(llvm_);
        // CG25: integer widths (§5). Char is a 32-bit code point.
        case P::Byte: case P::SByte:   return llvm::Type::getInt8Ty(llvm_);
        case P::Short: case P::UShort: return llvm::Type::getInt16Ty(llvm_);
        case P::Long: case P::ULong:   return i64();
        case P::Int: case P::UInt: case P::Char:
                                       return i32();
        // CG27a (§4.6): string is a reference type — pointer to bz_string.
        case P::String:                return ptrTy();
        default:                       return i32();
    }
}


bool Emitter::isUnsignedPrim(sema::PrimitiveType p) {
    using P = sema::PrimitiveType;
    return p == P::Byte || p == P::UShort || p == P::UInt ||
           p == P::ULong || p == P::Char;
}


bool Emitter::isUnsignedBinOperand(const ast::BinaryExpr& bin) {
    auto unsignedExpr = [&](const ast::Expr* e) {
        if (!e) return false;
        auto* rt = ctx_.exprType(*e);
        return rt && rt->kind == sema::TypeRef::Kind::Primitive &&
               isUnsignedPrim(rt->primitive);
    };
    return unsignedExpr(bin.left.get()) || unsignedExpr(bin.right.get());
}


bool Emitter::isUnsignedTargetExpr(const ast::Expr& e) {
    auto* rt = ctx_.exprType(e);
    return rt && rt->kind == sema::TypeRef::Kind::Primitive &&
           isUnsignedPrim(rt->primitive);
}


llvm::Value* Emitter::lvalueAddr(const ast::Expr& target, llvm::Type*& elemTy) {
    // CG50: a ref/out parameter — its address is the incoming pointer.
    if (llvm::Value* a = refParamAddr(target, elemTy)) return a;
    if (llvm::AllocaInst* slot = localSlot(target)) {
        elemTy = slot->getAllocatedType();
        return slot;
    }
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&target)) {
        if (llvm::Value* a = fieldAddr(*ma, elemTy)) return a;
    }
    if (llvm::GlobalVariable* gv = staticFieldGlobal(target)) {
        elemTy = gv->getValueType();
        return gv;
    }
    // §15.1: inline array element lvalue `a[i]`.
    if (auto* ix = dynamic_cast<const ast::IndexExpr*>(&target)) {
        if (ix->target && !ix->indices.empty() && ix->indices[0].value) {
            llvm::Type* arrSlotTy = nullptr;
            llvm::Value* arrAddr = lvalueAddr(*ix->target, arrSlotTy);
            if (arrAddr && arrSlotTy) {
                if (auto* arrTy = llvm::dyn_cast<llvm::ArrayType>(arrSlotTy)) {
                    llvm::Value* idx = lowerExpr(*ix->indices[0].value);
                    if (idx) {
                        // Bounds check (§15.1): IndexOutOfRangeException.
                        uint64_t sz = arrTy->getNumElements();
                        llvm::Value* idx64 = idx->getType()->isIntegerTy(64)
                            ? idx : builder_.CreateSExt(idx, i64());
                        llvm::Value* oor = builder_.CreateICmpUGE(
                            idx64, builder_.getInt64(sz), "arr.wr.oor");
                        emitGuardThrow(oor, "IndexOutOfRangeException");
                        elemTy = arrTy->getElementType();
                        return builder_.CreateGEP(
                            arrTy, arrAddr, {builder_.getInt64(0), idx});
                    }
                }
            }
        }
    }
    return implicitFieldAddr(target, elemTy);
}


llvm::Value* Emitter::emitCompoundOp(const std::string& op, llvm::Value* l,
                            llvm::Value* r, bool fp, bool uns) {
    if (fp) {
        if (op == "+") return builder_.CreateFAdd(l, r);
        if (op == "-") return builder_.CreateFSub(l, r);
        if (op == "*") return builder_.CreateFMul(l, r);
        if (op == "/") return builder_.CreateFDiv(l, r);
        return nullptr;
    }
    if (op == "+")
        return emitCheckedArith(uns ? llvm::Intrinsic::uadd_with_overflow
                                    : llvm::Intrinsic::sadd_with_overflow,
                                l, r);
    if (op == "-")
        return emitCheckedArith(uns ? llvm::Intrinsic::usub_with_overflow
                                    : llvm::Intrinsic::ssub_with_overflow,
                                l, r);
    if (op == "*")
        return emitCheckedArith(uns ? llvm::Intrinsic::umul_with_overflow
                                    : llvm::Intrinsic::smul_with_overflow,
                                l, r);
    if (op == "/" || op == "mod") {
        emitGuardThrow(builder_.CreateICmpEQ(r, zeroLike(r)),
                       "DivideByZeroException");
        if (uns)
            return op == "/" ? builder_.CreateUDiv(l, r)
                             : builder_.CreateURem(l, r);
        llvm::Value* negOne = llvm::ConstantInt::getSigned(r->getType(), -1);
        emitGuardThrow(
            builder_.CreateAnd(builder_.CreateICmpEQ(l, minIntLike(r)),
                               builder_.CreateICmpEQ(r, negOne)),
            "OverflowException");
        return op == "/" ? builder_.CreateSDiv(l, r)
                         : builder_.CreateSRem(l, r);
    }
    if (op == "&") return builder_.CreateAnd(l, r);
    if (op == "|") return builder_.CreateOr(l, r);
    if (op == "^") return builder_.CreateXor(l, r);
    if (op == "<<") return builder_.CreateShl(l, r);
    if (op == ">>")
        return uns ? builder_.CreateLShr(l, r) : builder_.CreateAShr(l, r);
    return nullptr;
}


llvm::Type* Emitter::lowerType(const ast::Type* t) {
    if (!t) return llvm::Type::getVoidTy(llvm_);
    return lowerTypeRef(ctx_.resolvedType(*t));
}


bool Emitter::isNullablePrimTy(const sema::TypeRef* rt) {
    if (!rt || !rt->nullable ||
        rt->kind != sema::TypeRef::Kind::Primitive) return false;
    using P = sema::PrimitiveType;
    switch (rt->primitive) {
        case P::Bool:
        case P::Byte: case P::SByte:
        case P::Short: case P::UShort:
        case P::Int: case P::UInt:
        case P::Long: case P::ULong:
        case P::Float: case P::Double:
            return true;
        default: return false;  // String, Char, Object, Decimal, None
    }
}


llvm::StructType* Emitter::nullablePrimStructTy(sema::PrimitiveType pt) {
    return llvm::StructType::get(llvm_, {nullablePrimHasValTy(), lowerPrimitive(pt)});
}


llvm::Value* Emitter::wrapNullablePrim(sema::PrimitiveType pt, llvm::Value* val) {
    auto* st = nullablePrimStructTy(pt);
    auto* hasValTy = nullablePrimHasValTy();
    llvm::Value* result =
        builder_.CreateInsertValue(llvm::UndefValue::get(st),
            llvm::ConstantInt::get(hasValTy, 1), {0u});
    return builder_.CreateInsertValue(result, val, {1u});
}


llvm::Type* Emitter::lowerTypeRef(const sema::TypeRef* rt) {
    if (!rt) return i32();
    // CG-nullable: nullable primitive → {i1 hasValue; primType value}
    if (isNullablePrimTy(rt)) return nullablePrimStructTy(rt->primitive);
    switch (rt->kind) {
        case sema::TypeRef::Kind::Unit:
            return llvm::Type::getVoidTy(llvm_);
        case sema::TypeRef::Kind::Primitive:
            return lowerPrimitive(rt->primitive);
        case sema::TypeRef::Kind::Named:
            // CG-generic: a Named type with null declaration is a generic
            // type parameter (e.g. T in Box<T>). If there's an active
            // substitution, resolve via the param's name in the ast::Type.
            if (!rt->declaration && !genericSubst_.empty()) {
                if (rt->astNode && rt->astNode->name &&
                    rt->astNode->name->parts.size() == 1) {
                    auto sit = genericSubst_.find(
                        rt->astNode->name->parts[0]);
                    if (sit != genericSubst_.end())
                        return sit->second;
                }
                return i32();  // unresolved generic param → i32 fallback
            }
            // §3 stdlib: collection and IO built-in classes (no user TypeDecl).
            // All are heap-allocated opaque pointers to runtime structs.
            if (!rt->declaration && rt->astNode && rt->astNode->name &&
                !rt->astNode->name->parts.empty()) {
                auto n = rt->astNode->name->parts[0];
                if (n == "Array" || n == "OwnedArray" ||
                    n == "Dictionary" || n == "OwnedDictionary" ||
                    n == "Stack" || n == "OwnedStack" ||
                    n == "Queue" || n == "OwnedQueue" ||
                    n == "Set" || n == "OwnedSet" ||
                    n == "LinkedList" || n == "OwnedLinkedList" ||
                    n == "StringBuilder" || n == "Random" ||
                    n == "DateTime" || n == "TimeSpan" ||
                    // §29: iteration interfaces are heap-allocated objects.
                    n == "IEnumerable" || n == "IEnumerator" ||
                    n == "OwnedEnumerable" || n == "OwnedEnumerator" ||
                    n == "IAsyncEnumerable" || n == "IAsyncEnumerator" ||
                    n == "OwnedAsyncEnumerable" || n == "OwnedAsyncEnumerator" ||
                    n == "IReadOnlyCollection" || n == "IReadOnlyList" ||
                    n == "ICollection" || n == "IList" ||
                    n == "IOwnedCollection" || n == "IOwnedList" ||
                    // §30: Task types are also heap-allocated.
                    n == "Task" || n == "OwnedTask")
                    return ptrTy();
            }
            if (rt->declaration &&
                (rt->declaration->kind == ast::TypeDeclKind::Class ||
                 rt->declaration->kind == ast::TypeDeclKind::Interface)) {
                return ptrTy();  // reference types are heap pointers
            }
            // §9.4: a record is a value type, lowered to a first-class
            // LLVM struct value.
            if (rt->declaration &&
                rt->declaration->kind == ast::TypeDeclKind::Record) {
                return recordStruct(rt->declaration);
            }
            return i32();  // other value-kind named (enum) — int
        case sema::TypeRef::Kind::InlineArray: {
            // §15.1: [N]T is an LLVM fixed-size array [N x elemType].
            // N is stored as an IntLiteralExpr; element type recurses.
            if (!rt->arrayElement || !rt->arraySize) return i32();
            llvm::Type* elem = lowerTypeRef(rt->arrayElement);
            auto* lit =
                dynamic_cast<const ast::IntLiteralExpr*>(rt->arraySize);
            if (!lit) return i32();
            uint64_t n = std::stoull(std::string(lit->text));
            return llvm::ArrayType::get(elem, n);
        }
        case sema::TypeRef::Kind::Function:
            // CG60: a function type `(T1, …) -> R` — stored as an opaque
            // pointer to the function body (a plain function pointer in
            // the non-capturing / single-representation case).
            return ptrTy();
        default:
            return i32();
    }
}


llvm::Type* Emitter::storageType(const sema::Symbol* m) {
    // §9.1: a record's positional field carries its type on the
    // originating Param, not a FieldDecl (its `decl` is the TypeDecl).
    if (auto* fs = dynamic_cast<const sema::FieldSymbol*>(m))
        if (fs->posParam)
            return lowerType(fs->posParam->type.get());
    if (auto* fd = dynamic_cast<const ast::FieldDecl*>(m->decl))
        return lowerType(fd->type.get());
    if (auto* pd = dynamic_cast<const ast::PropertyDecl*>(m->decl))
        return lowerType(pd->type.get());
    return i32();
}


llvm::StructType* Emitter::classStruct(const sema::TypeSymbol* t) {
    // CG29: a record has no vtable header; route to recordStruct so every
    // classStruct caller (field access, etc.) gets the right layout.
    if (t->kind == ast::TypeDeclKind::Record) return recordStruct(t);

    // CG-generic: if we have an active substitution, compute the concrete
    // field types first and look them up in the specialization cache.
    if (!genericSubst_.empty()) {
        // Peek at what field types we'd get with the substitution.
        std::vector<const sema::Symbol*> storage;
        collectInstanceStorage(t, storage);
        std::vector<llvm::Type*> specFieldTys;
        specFieldTys.push_back(ptrTy());
        for (const sema::Symbol* m : storage)
            specFieldTys.push_back(storageType(m));
        // Cache lookup: specializedStructs_[t][specFieldTys] → existing
        auto& specMap = specializedStructs_[t];
        auto sit = specMap.find(specFieldTys);
        if (sit != specMap.end()) return sit->second;
        // Create new specialized struct.
        std::string specName = std::string("class.") + std::string(t->name)
            + "$spec" + std::to_string(specMap.size());
        auto* specSt = llvm::StructType::create(llvm_, specName);
        specSt->setBody(specFieldTys);
        specMap[specFieldTys] = specSt;
        // Register field indices for this specialization.
        unsigned sidx = 1;
        for (const sema::Symbol* m : storage)
            fieldIndex_[m] = sidx++;
        return specSt;
    }

    auto it = structs_.find(t);
    if (it != structs_.end()) return it->second;
    auto* st = llvm::StructType::create(
        llvm_, std::string("class.") + std::string(t->name));
    structs_[t] = st;  // record before filling (supports self-reference)
    std::vector<const sema::Symbol*> storage;
    collectInstanceStorage(t, storage);
    std::vector<llvm::Type*> fieldTys;
    // CG17: slot 0 is the hidden vtable pointer; data fields follow.
    fieldTys.push_back(ptrTy());
    unsigned idx = 1;
    for (const sema::Symbol* m : storage) {
        fieldTys.push_back(storageType(m));
        fieldIndex_[m] = idx++;
    }
    st->setBody(fieldTys);
    return st;
}


llvm::StructType* Emitter::recordStruct(const sema::TypeSymbol* t) {
    auto it = structs_.find(t);
    if (it != structs_.end()) return it->second;
    auto* st = llvm::StructType::create(
        llvm_, std::string("record.") + std::string(t->name));
    structs_[t] = st;  // before filling, to support self-reference
    std::vector<llvm::Type*> fieldTys;
    unsigned idx = 0;
    // §9.1 / §9.2: storage = non-static FieldSymbols (positional + body
    // fields) and body auto-properties (PropertySymbol), in declaration order.
    std::vector<const sema::Symbol*> storage;
    collectRecordStorage(t, storage);
    for (const sema::Symbol* m : storage) {
        fieldTys.push_back(storageType(m));
        fieldIndex_[m] = idx++;
    }
    st->setBody(fieldTys);
    return st;
}


llvm::Value* Emitter::lowerTypeTest(const ast::TypeTestExpr& te) {
    if (!te.operand || !te.testType) return nullptr;
    llvm::Value* obj = lowerExpr(*te.operand);
    if (!obj) return nullptr;
    sema::TypeRef* rt = ctx_.resolvedType(*te.testType);
    if (!rt || rt->kind != sema::TypeRef::Kind::Named || !rt->declaration)
        return nullptr;
    auto dit = descArray_.find(rt->declaration);
    if (dit == descArray_.end()) return nullptr;
    llvm::Value* isA = emitIsA(obj, dit->second);
    if (te.op == "is") return isA;
    if (te.op == "is not")
        return builder_.CreateXor(isA, builder_.getInt1(true));
    if (te.op == "as")
        return builder_.CreateSelect(
            isA, obj, llvm::ConstantPointerNull::get(ptrTy()));
    return nullptr;
}


llvm::Value* Emitter::emitIsA(llvm::Value* obj, llvm::GlobalVariable* targetDesc) {
    auto* nullBB = llvm::BasicBlock::Create(llvm_, "isa.null", currentFn_);
    auto* loadBB = llvm::BasicBlock::Create(llvm_, "isa.load", currentFn_);
    auto* bodyBB = llvm::BasicBlock::Create(llvm_, "isa.body", currentFn_);
    auto* matchBB = llvm::BasicBlock::Create(llvm_, "isa.match", currentFn_);
    auto* nextBB = llvm::BasicBlock::Create(llvm_, "isa.next", currentFn_);
    auto* endBB = llvm::BasicBlock::Create(llvm_, "isa.end", currentFn_);

    llvm::Value* objIsNull = builder_.CreateICmpEQ(
        obj, llvm::ConstantPointerNull::get(ptrTy()));
    builder_.CreateCondBr(objIsNull, nullBB, loadBB);

    builder_.SetInsertPoint(nullBB);
    builder_.CreateBr(endBB);

    builder_.SetInsertPoint(loadBB);
    llvm::Value* vptr = builder_.CreateLoad(ptrTy(), obj);
    llvm::Value* desc = builder_.CreateLoad(ptrTy(), vptr);  // vtable[0]
    builder_.CreateBr(bodyBB);

    builder_.SetInsertPoint(bodyBB);
    auto* idx = builder_.CreatePHI(i32(), 2);
    idx->addIncoming(builder_.getInt32(0), loadBB);
    llvm::Value* elem = builder_.CreateLoad(
        ptrTy(), builder_.CreateGEP(ptrTy(), desc, {idx}));
    llvm::Value* atNull = builder_.CreateICmpEQ(
        elem, llvm::ConstantPointerNull::get(ptrTy()));
    builder_.CreateCondBr(atNull, endBB, matchBB);

    builder_.SetInsertPoint(matchBB);
    llvm::Value* match = builder_.CreateICmpEQ(elem, targetDesc);
    builder_.CreateCondBr(match, endBB, nextBB);

    builder_.SetInsertPoint(nextBB);
    idx->addIncoming(builder_.CreateAdd(idx, builder_.getInt32(1)), nextBB);
    builder_.CreateBr(bodyBB);

    builder_.SetInsertPoint(endBB);
    auto* result = builder_.CreatePHI(i1(), 3);
    result->addIncoming(builder_.getInt1(false), nullBB);
    result->addIncoming(builder_.getInt1(false), bodyBB);    // hit null
    result->addIncoming(builder_.getInt1(true), matchBB);    // matched
    return result;
}


llvm::Value* Emitter::refParamAddr(const ast::Expr& e, llvm::Type*& elemTy) {
    if (!dynamic_cast<const ast::IdentifierExpr*>(&e)) return nullptr;
    sema::Symbol* sym = ctx_.exprSymbol(e);
    if (!sym) return nullptr;
    auto it = refParams_.find(sym);
    if (it == refParams_.end()) return nullptr;
    elemTy = it->second.second;
    return it->second.first;
}


llvm::AllocaInst* Emitter::localSlot(const ast::Expr& e) {
    if (!dynamic_cast<const ast::IdentifierExpr*>(&e)) return nullptr;
    sema::Symbol* sym = ctx_.exprSymbol(e);
    if (!sym) return nullptr;
    if (dynamic_cast<sema::ParameterSymbol*>(sym)) {
        auto it = paramSlots_.find(sym);
        return it == paramSlots_.end() ? nullptr : it->second;
    }
    if (auto* loc = dynamic_cast<sema::LocalSymbol*>(sym)) {
        if (!loc->decl) return nullptr;
        auto it = locals_.find(loc->decl);
        return it == locals_.end() ? nullptr : it->second;
    }
    return nullptr;
}


llvm::Value* Emitter::makeMethodThunk(sema::MethodSymbol* ms,
                             llvm::Value* receiverVal) {
    auto it = methodFns_.find(ms);
    if (it == methodFns_.end()) return nullptr;
    llvm::Function* origFn = it->second;

    // Build the thunk's parameter types: (ptr env, user_params...).
    std::vector<llvm::Type*> thunkParamTys;
    thunkParamTys.push_back(ptrTy());  // env_ptr
    // Skip the leading `this` arg for instance methods
    // (it comes from env instead).
    bool instMethod = !isStaticMethod(ms);
    for (std::size_t i = (instMethod ? 1 : 0);
         i < origFn->arg_size(); ++i)
        thunkParamTys.push_back(origFn->getArg(i)->getType());
    llvm::Type* retTy = origFn->getReturnType();
    auto* thunkFnTy = llvm::FunctionType::get(retTy, thunkParamTys, false);

    // Create thunk function.
    static std::atomic<unsigned> thunkCtr{0};
    std::string thunkName = "thunk$" + std::to_string(thunkCtr.fetch_add(1));
    auto* thunk = llvm::Function::Create(
        thunkFnTy, llvm::Function::InternalLinkage, thunkName, module_.get());

    // Save emitter state.
    llvm::Function* savedFn = currentFn_;
    llvm::Value* savedThis = currentThis_;
    llvm::BasicBlock* savedBB = builder_.GetInsertBlock();
    auto savedLocals = locals_;
    auto savedParams = paramSlots_;
    auto savedRefParams = refParams_;
    auto savedDisposeStack = disposeStack_;
    auto savedLoopStack = loopStack_;
    llvm::BasicBlock* savedUnwind = unwindTarget_;
    llvm::BasicBlock* savedProp = propagateBB_;
    llvm::Value* savedCaught = currentCaughtExc_;
    std::size_t savedUnwindDepth = unwindScopeDepth_;
    bool savedDiverged = diverged_;

    currentFn_ = thunk;
    currentThis_ = nullptr;
    locals_.clear(); paramSlots_.clear(); refParams_.clear();
    disposeStack_.clear(); loopStack_.clear(); finallyStack_.clear();
    unwindTarget_ = nullptr; propagateBB_ = nullptr;
    currentCaughtExc_ = nullptr; unwindScopeDepth_ = 0; diverged_ = false;
    builder_.SetInsertPoint(
        llvm::BasicBlock::Create(llvm_, "entry", thunk));

    // Build call args to the original function.
    std::vector<llvm::Value*> callArgs;
    auto argIt = thunk->args().begin();
    llvm::Value* envPtrArg = &*argIt++;  // env_ptr (arg 0)
    if (instMethod) {
        // Load receiver from env: env_ptr → load ptr.
        callArgs.push_back(builder_.CreateLoad(ptrTy(), envPtrArg));
    }
    for (; argIt != thunk->args().end(); ++argIt)
        callArgs.push_back(&*argIt);

    llvm::Value* result = builder_.CreateCall(origFn, callArgs);
    if (retTy->isVoidTy()) builder_.CreateRetVoid();
    else builder_.CreateRet(result);

    // Restore emitter state.
    currentFn_ = savedFn;
    currentThis_ = savedThis;
    locals_ = std::move(savedLocals); paramSlots_ = std::move(savedParams);
    refParams_ = std::move(savedRefParams);
    disposeStack_ = std::move(savedDisposeStack);
    loopStack_ = std::move(savedLoopStack);
    unwindTarget_ = savedUnwind; propagateBB_ = savedProp;
    currentCaughtExc_ = savedCaught; unwindScopeDepth_ = savedUnwindDepth;
    diverged_ = savedDiverged;
    if (savedBB) builder_.SetInsertPoint(savedBB);

    // Build closure struct { thunk_ptr, env_ptr }.
    llvm::Value* closureObj = builder_.CreateCall(
        callocFn(), {builder_.getInt64(1), builder_.getInt64(16)});
    builder_.CreateStore(thunk, closureObj);
    // For instance methods: store a copy of the receiver in env.
    llvm::Value* envObj = llvm::ConstantPointerNull::get(ptrTy());
    if (instMethod && receiverVal) {
        envObj = builder_.CreateCall(
            callocFn(), {builder_.getInt64(1), builder_.getInt64(8)});
        builder_.CreateStore(receiverVal, envObj);
    }
    builder_.CreateStore(
        envObj,
        builder_.CreateGEP(ptrTy(), closureObj, {builder_.getInt32(1)}));
    return closureObj;
}


sema::MemberSymbol* Emitter::storageMemberOf(sema::Symbol* sym) {
    if (auto* fs = dynamic_cast<sema::FieldSymbol*>(sym)) return fs;
    if (isAutoProperty(sym))
        return dynamic_cast<sema::PropertySymbol*>(sym);
    return nullptr;
}


llvm::Value* Emitter::implicitFieldAddr(const ast::Expr& e, llvm::Type*& elemTy) {
    if (!dynamic_cast<const ast::IdentifierExpr*>(&e)) return nullptr;
    auto* ms = storageMemberOf(ctx_.exprSymbol(e));
    if (!ms || !ms->parent || !currentThis_) return nullptr;
    auto idxIt = fieldIndex_.find(ms);
    if (idxIt == fieldIndex_.end()) return nullptr;  // static field
    llvm::StructType* st = classStruct(ms->parent);
    elemTy = st->getElementType(idxIt->second);
    return builder_.CreateStructGEP(st, currentThis_, idxIt->second);
}


llvm::Value* Emitter::emitStructEquality(llvm::Value* l, llvm::Value* r,
                                llvm::StructType* st) {
    llvm::Value* acc = builder_.getInt1(true);
    for (unsigned i = 0; i < st->getNumElements(); ++i) {
        llvm::Value* li = builder_.CreateExtractValue(l, {i});
        llvm::Value* ri = builder_.CreateExtractValue(r, {i});
        llvm::Type* ft = st->getElementType(i);
        llvm::Value* fieldEq;
        if (auto* nested = llvm::dyn_cast<llvm::StructType>(ft)) {
            fieldEq = emitStructEquality(li, ri, nested);
        } else if (ft->isFloatingPointTy()) {
            fieldEq = builder_.CreateFCmpOEQ(li, ri);
        } else {
            fieldEq = builder_.CreateICmpEQ(li, ri);
        }
        acc = builder_.CreateAnd(acc, fieldEq);
    }
    return acc;
}


llvm::Value* Emitter::fieldAddr(const ast::MemberAccessExpr& ma,
                       llvm::Type*& elemTy) {
    auto* ms = storageMemberOf(ctx_.exprSymbol(ma));
    if (!ms || !ms->parent || !ma.target) return nullptr;
    auto idxIt = fieldIndex_.find(ms);
    // CG-generic: for generic class field access (e.g. bd.value where
    // bd: Box<double>), set up the substitution from the target's exprType
    // so classStruct uses the correct concrete field types.
    auto savedSubst = genericSubst_;
    if (sema::TypeRef* targetTy = ctx_.exprType(*ma.target)) {
        if (targetTy->kind == sema::TypeRef::Kind::Named &&
            targetTy->declaration == ms->parent &&
            !targetTy->genericArgs.empty()) {
            auto* td = dynamic_cast<const ast::TypeDecl*>(
                ms->parent->decl);
            if (td && td->genericParams.size() ==
                      targetTy->genericArgs.size()) {
                std::unordered_map<std::string_view, llvm::Type*> newSubst;
                for (std::size_t gi = 0;
                     gi < td->genericParams.size(); ++gi) {
                    newSubst[td->genericParams[gi].name] =
                        lowerTypeRef(targetTy->genericArgs[gi]);
                }
                // If any resolved type is i32 (unresolved generic fallback),
                // try looking up the local's stored concrete substitution.
                bool hasUnresolved = false;
                for (auto& [n, ty] : newSubst)
                    if (ty == i32()) { hasUnresolved = true; break; }
                if (hasUnresolved) {
                    // Chase through PostfixBangExpr/MemberAccessExpr to
                    // find the root local variable (e.g. root._left!._val
                    // peels !._left to reach `root`).
                    const ast::Expr* cur = ma.target.get();
                    const ast::Node* decl = nullptr;
                    while (cur && !decl) {
                        sema::Symbol* sym = ctx_.exprSymbol(*cur);
                        if (auto* loc = dynamic_cast<sema::LocalSymbol*>(sym))
                            decl = loc->decl;
                        if (!decl) {
                            if (auto* bg = dynamic_cast<const ast::PostfixBangExpr*>(cur))
                                cur = bg->operand.get();
                            else if (auto* sub = dynamic_cast<const ast::MemberAccessExpr*>(cur))
                                cur = sub->target.get();
                            else
                                break;
                        }
                    }
                    if (decl) {
                        auto lit = localGenericSubst_.find(decl);
                        if (lit != localGenericSubst_.end())
                            newSubst = lit->second;
                    }
                }
                genericSubst_ = std::move(newSubst);
            }
        }
    }
    if (idxIt == fieldIndex_.end()) {
        // Re-run classStruct to populate fieldIndex_ under the substitution.
        classStruct(ms->parent);
        idxIt = fieldIndex_.find(ms);
    }
    llvm::StructType* st = classStruct(ms->parent);
    genericSubst_ = std::move(savedSubst);
    // CG29: a record is a first-class struct value, not a pointer. To take
    // the address of one of its fields, GEP the record's storage:
    //  - an addressable target (a record local/parameter) → GEP its slot
    //    directly, the natural value-type access;
    //  - an already-pointer target (a `this` pointer inside a record
    //    method) → GEP it as-is;
    //  - a temporary record value (e.g. a freshly returned record) → spill
    //    it to a stack slot and GEP that.
    if (ms->parent->kind == ast::TypeDeclKind::Record) {
        // CG29: prefer lvalue address so writes go to the original
        // memory (e.g. grid[2].Cells[x] := v must update grid[2],
        // not a stack copy). Fall back to lowerExpr for temporaries
        // (e.g. a freshly returned record value).
        llvm::Value* base = nullptr;
        llvm::Type* baseTy = nullptr;
        if (llvm::AllocaInst* slot = localSlot(*ma.target)) {
            base = slot;
        } else {
            base = lvalueAddr(*ma.target, baseTy);
            if (!base) {
                llvm::Value* obj = lowerExpr(*ma.target);
                if (!obj) return nullptr;
                if (obj->getType()->isPointerTy()) {
                    base = obj;
                } else {
                    llvm::Value* tmp = entryAlloca(st);
                    builder_.CreateStore(obj, tmp);
                    base = tmp;
                }
            }
        }
        elemTy = st->getElementType(idxIt->second);
        return builder_.CreateStructGEP(st, base, idxIt->second);
    }
    llvm::Value* obj = lowerExpr(*ma.target);
    if (!obj) return nullptr;
    elemTy = st->getElementType(idxIt->second);
    return builder_.CreateStructGEP(st, obj, idxIt->second);
}


}  // namespace bezel::codegen
