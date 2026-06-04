#include "codegen/LlvmBackend_impl.hpp"

namespace bezel::codegen {

const sema::PropertySymbol* Emitter::findIndexer(
    const sema::TypeSymbol* ts) const {
    for (const sema::Symbol* m : ts->members) {
        if (auto* ps = dynamic_cast<const sema::PropertySymbol*>(m))
            if (ps->isIndexer) return ps;
    }
    return nullptr;
}


// issue #388 / stdlib §4.4-§4.9: lower a HEAP-collection indexer read `a[i]` /
// `dict[key]` against the runtime backing. Mirrors the `.Get(i)` (§4.5) and
// `.TryGetValue` (§4.8) lowering in LlvmBackend_Calls.cpp.
llvm::Value* Emitter::lowerCollectionIndexerGet(const ast::IndexExpr& ix) {
    if (!ix.target || ix.indices.empty() || !ix.indices[0].value)
        return nullptr;
    auto* recvTy = ctx_.exprType(*ix.target);
    bool isArr  = isCollectionTypeName(recvTy);
    bool isDict = isDictTypeName(recvTy);
    if (!isArr && !isDict) return nullptr;
    llvm::Value* recv = lowerExpr(*ix.target);
    if (!recv) return nullptr;

    // --- Array<T> / OwnedArray<T> indexer get (§4.4 / §4.5) ----------------
    // `a[i]` reads the slot at i (the getter borrows for class T, copies for
    // value T). The element type is genericArgs[0]; bounds-check against Count.
    if (isArr) {
        llvm::Value* idx = lowerExpr(*ix.indices[0].value);
        if (!idx) return nullptr;
        idx = idx->getType()->isIntegerTy(64) ? idx
            : builder_.CreateSExt(idx, i64());
        // §31.2: out-of-range raises IndexOutOfRangeException.
        auto* lenFn = declareC("bz_array_len", i64(), {ptrTy()});
        llvm::Value* len = builder_.CreateCall(lenFn, {recv});
        llvm::Value* oor = builder_.CreateICmpUGE(idx, len, "ix.oor");
        emitGuardThrow(oor, "IndexOutOfRangeException");
        int64_t esz = collectionElemSize(recvTy);
        llvm::Type* retTy = i32();
        if (recvTy && !recvTy->genericArgs.empty() && recvTy->genericArgs[0])
            if (auto* et = lowerTypeRef(recvTy->genericArgs[0])) retTy = et;
        llvm::AllocaInst* out = entryAlloca(retTy);
        auto* getFn = declareC("bz_array_get", llvm::Type::getVoidTy(llvm_),
                               {ptrTy(), i64(), ptrTy(), i64()});
        builder_.CreateCall(getFn, {recv, idx, out, builder_.getInt64(esz)});
        return builder_.CreateLoad(retTy, out, "ix.elem");
    }

    // --- Dictionary<K,V> / OwnedDictionary<K,V> indexer get (§4.8 / §4.9) --
    // `dict[key]` returns the stored value; an absent key raises
    // KeyNotFoundException (§4.8). Mirrors the TryGetValue decode path.
    const sema::TypeRef* keyTy =
        genericBackingType(recvTy, 0, /*isSet=*/false, nullptr);
    bool keyIsRecord = keyTy && recordTypeOfRef(keyTy) != nullptr;
    const sema::TypeRef* valTy =
        (recvTy && recvTy->genericArgs.size() > 1) ? recvTy->genericArgs[1]
                                                   : nullptr;
    const sema::TypeSymbol* valRec =
        (valTy && valTy->kind == sema::TypeRef::Kind::Named &&
         valTy->declaration &&
         valTy->declaration->kind == ast::TypeDeclKind::Record)
            ? valTy->declaration : nullptr;
    // issue #393 / §4.8: a record (aggregate) value is heap-boxed in the dict
    // (the runtime stores a malloc'd box of the record bytes — see the `.Set` /
    // `.TryGetValue` encode/decode in LlvmBackend_Calls.cpp). It is too wide for
    // the scalar inttoptr value-slot encoding, so the indexer get decodes it the
    // same way TryGetValue does: load the box pointer, then load the record
    // struct out of the box (the decode below, where `valLlvm` is the record's
    // LLVM struct type). The throw-on-absent guard guarantees the box is
    // non-null, so no extra null check is needed.
    llvm::Type* valLlvm = valTy ? lowerTypeRef(valTy) : ptrTy();
    if (!valLlvm) valLlvm = ptrTy();

    llvm::Value* k = lowerExpr(*ix.indices[0].value);
    if (!k) return nullptr;
    auto* tmpSlot = entryAlloca(ptrTy());
    llvm::Value* found;
    if (keyTy) {
        // spill the key value to a slot (record keys arrive as an address).
        llvm::Value* keyPtr = k;
        if (!(keyIsRecord && k->getType()->isPointerTy())) {
            llvm::AllocaInst* slot = entryAlloca(k->getType());
            builder_.CreateStore(k, slot);
            keyPtr = slot;
        }
        auto* fn = declareC("bz_dict_try_get_gen", i1(),
                            {ptrTy(), ptrTy(), ptrTy()});
        found = builder_.CreateCall(fn, {recv, keyPtr, tmpSlot});
    } else {
        llvm::Value* kd = loadStringData(k);
        llvm::Value* kl = loadStringLen(k);
        auto* fn = declareC("bz_dict_try_get_str", i1(),
                            {ptrTy(), ptrTy(), i64(), ptrTy()});
        found = builder_.CreateCall(fn, {recv, kd, kl, tmpSlot});
    }
    // §4.8: absent key → KeyNotFoundException.
    emitGuardThrow(builder_.CreateNot(found), "KeyNotFoundException");
    // Decode the stored void* into the value type (mirror TryGetValue). The
    // cases are: a record (aggregate) value — load the boxed record struct out
    // of the box (issue #393); scalars — decoded from the inttoptr'd slot;
    // object/string pointers — the void* IS the pointer.
    llvm::Value* valPtr = builder_.CreateLoad(ptrTy(), tmpSlot);
    if (valRec && valLlvm->isStructTy())
        return builder_.CreateLoad(valLlvm, valPtr, "ix.rec");
    if (!valLlvm->isPointerTy()) {
        llvm::Value* asI64 = builder_.CreatePtrToInt(valPtr, i64());
        if (valLlvm->isDoubleTy())
            return builder_.CreateBitCast(asI64, valLlvm);
        if (valLlvm->isFloatTy())
            return builder_.CreateBitCast(builder_.CreateTrunc(asI64, i32()),
                                          valLlvm);
        if (valLlvm->isIntegerTy(64)) return asI64;
        return builder_.CreateTrunc(asI64, valLlvm);
    }
    return valPtr;  // pointer value (string / object)
}


// issue #409 / language spec §28.8.1 / §28.11: lower `move coll[i]` (the
// slot-take form) on an OWNING heap collection (OwnedArray / OwnedDictionary).
// The element is read out as an owned value AND the slot is left in moved-from
// state — for these runtime-backed collections a moved-from slot is a NULL slot.
// The collection's element-disposer (bz_array_free / bz_dict_free) already skips
// null slots, so nulling here prevents the collection's later Dispose from
// re-freeing the element that the new owner now owns (the #409 double-free).
//
// Mirrors lowerCollectionIndexerGet for the read, then issues a NON-disposing
// slot store of null (the value was moved out, not displaced — there is nothing
// to dispose at the slot, per §28.11's "indexer setter on a moved-from slot").
// Returns nullptr if `ix` is not an owning-collection slot, so callers fall
// through to the ordinary borrowing-read lowering (a borrow does not null the
// slot — §28.11). The index/key expression is evaluated exactly once.
llvm::Value* Emitter::lowerMoveFromCollectionSlot(const ast::IndexExpr& ix) {
    if (!ix.target || ix.indices.empty() || !ix.indices[0].value)
        return nullptr;
    auto* recvTy = ctx_.exprType(*ix.target);
    bool isArr  = isCollectionTypeName(recvTy);
    bool isDict = isDictTypeName(recvTy);
    if (!isArr && !isDict) return nullptr;

    // --- OwnedArray<T> slot-move (§28.11; admitted only on owning arrays) ----
    if (isArr) {
        // Only an OWNING array's slot carries ownership to transfer; a borrowing
        // Array<T> slot is a borrow and `move arr[i]` is rejected by sema. Guard
        // on the disposer so a non-owning array (defensively) never nulls a slot.
        if (!owningCollectionElemDisposer(recvTy)) return nullptr;
        llvm::Value* recv = lowerExpr(*ix.target);
        if (!recv) return nullptr;
        llvm::Value* idx = lowerExpr(*ix.indices[0].value);   // evaluate once
        if (!idx) return nullptr;
        idx = idx->getType()->isIntegerTy(64) ? idx
            : builder_.CreateSExt(idx, i64());
        // §31.2 / §28.11: out-of-range raises before any slot is moved-from.
        auto* lenFn = declareC("bz_array_len", i64(), {ptrTy()});
        llvm::Value* len = builder_.CreateCall(lenFn, {recv});
        llvm::Value* oor = builder_.CreateICmpUGE(idx, len, "mv.oor");
        emitGuardThrow(oor, "IndexOutOfRangeException");
        int64_t esz = collectionElemSize(recvTy);
        llvm::Type* retTy = ptrTy();
        if (recvTy && !recvTy->genericArgs.empty() && recvTy->genericArgs[0])
            if (auto* et = lowerTypeRef(recvTy->genericArgs[0])) retTy = et;
        // Read the owned element out of the slot.
        llvm::AllocaInst* out = entryAlloca(retTy);
        auto* getFn = declareC("bz_array_get", llvm::Type::getVoidTy(llvm_),
                               {ptrTy(), i64(), ptrTy(), i64()});
        builder_.CreateCall(getFn, {recv, idx, out, builder_.getInt64(esz)});
        llvm::Value* moved = builder_.CreateLoad(retTy, out, "mv.elem");
        // Leave the slot moved-from (null). NON-disposing set: the element was
        // taken out, not displaced — there is no previous value to dispose
        // (§28.11 "indexer setter on a moved-from slot").
        llvm::AllocaInst* nullSlot = entryAlloca(retTy);
        builder_.CreateStore(llvm::Constant::getNullValue(retTy), nullSlot);
        auto* setFn = declareC("bz_array_set", llvm::Type::getVoidTy(llvm_),
                               {ptrTy(), i64(), ptrTy(), i64()});
        builder_.CreateCall(setFn,
                            {recv, idx, nullSlot, builder_.getInt64(esz)});
        return moved;
    }

    // --- OwnedDictionary<K,V> slot-move (§28.11 / §4.9) ---------------------
    // Only an owning dictionary's value carries ownership; a borrowing
    // Dictionary<K,V> value is a borrow and `move dict[key]` is rejected by
    // sema. The owned thing is the VALUE (genericArgs[1]).
    bool owningDict =
        recvTy && recvTy->astNode && recvTy->astNode->name &&
        !recvTy->astNode->name->parts.empty() &&
        recvTy->astNode->name->parts[0] == "OwnedDictionary" &&
        collectionElemIsClass(recvTy, 1);
    if (!owningDict) return nullptr;
    llvm::Value* recv = lowerExpr(*ix.target);
    if (!recv) return nullptr;
    const sema::TypeRef* keyTy =
        genericBackingType(recvTy, 0, /*isSet=*/false, nullptr);
    bool keyIsRecord = keyTy && recordTypeOfRef(keyTy) != nullptr;
    // An owning dictionary owns class-typed VALUES, so the stored void* is the
    // owned object pointer; no scalar/record decode is needed (collectionElemIsClass
    // on the value already gated this path).

    llvm::Value* k = lowerExpr(*ix.indices[0].value);     // evaluate once
    if (!k) return nullptr;
    auto* tmpSlot = entryAlloca(ptrTy());
    llvm::Value* found;
    llvm::Value* keyPtr = nullptr;     // generic-key path needs it again to null
    llvm::Value* kd = nullptr;         // string-key path needs it again to null
    llvm::Value* kl = nullptr;
    if (keyTy) {
        keyPtr = k;
        if (!(keyIsRecord && k->getType()->isPointerTy())) {
            llvm::AllocaInst* slot = entryAlloca(k->getType());
            builder_.CreateStore(k, slot);
            keyPtr = slot;
        }
        auto* fn = declareC("bz_dict_try_get_gen", i1(),
                            {ptrTy(), ptrTy(), ptrTy()});
        found = builder_.CreateCall(fn, {recv, keyPtr, tmpSlot});
    } else {
        kd = loadStringData(k);
        kl = loadStringLen(k);
        auto* fn = declareC("bz_dict_try_get_str", i1(),
                            {ptrTy(), ptrTy(), i64(), ptrTy()});
        found = builder_.CreateCall(fn, {recv, kd, kl, tmpSlot});
    }
    // §4.8/§4.9: a `move` of an absent key raises, same as the indexer get.
    emitGuardThrow(builder_.CreateNot(found), "KeyNotFoundException");
    // The stored void* is the owned value pointer (a class object).
    llvm::Value* moved = builder_.CreateLoad(ptrTy(), tmpSlot, "mv.val");
    // Leave the slot moved-from: store null under the existing key via the
    // NON-disposing setter (the value was taken out, not displaced — §28.11).
    // bz_dict_free skips null values, so the dict's Dispose won't re-free it.
    llvm::Value* nullV = llvm::ConstantPointerNull::get(ptrTy());
    if (keyTy) {
        auto* fn = declareC("bz_dict_set_gen", llvm::Type::getVoidTy(llvm_),
                            {ptrTy(), ptrTy(), ptrTy()});
        builder_.CreateCall(fn, {recv, keyPtr, nullV});
    } else {
        auto* fn = declareC("bz_dict_set_str", llvm::Type::getVoidTy(llvm_),
                            {ptrTy(), ptrTy(), i64(), ptrTy()});
        builder_.CreateCall(fn, {recv, kd, kl, nullV});
    }
    return moved;   // owned class object pointer, taken out of the slot
}


// issue #388 / stdlib §4.4-§4.9: lower a HEAP-collection indexer setter
// `a[i] := v` / `dict[key] := v`. For an OWNING collection (OwnedArray /
// OwnedDictionary) the displaced slot/value is disposed exactly once before the
// new owner is stored (§4.5 / §4.9). Mirrors `.Set(i, v)` (§4.5) and the dict
// `.Add(k, v)` lowering. `val` is the already-lowered RHS.
bool Emitter::lowerCollectionIndexerSet(const ast::IndexExpr& ix,
                                        llvm::Value* val) {
    if (!ix.target || ix.indices.empty() || !ix.indices[0].value || !val)
        return false;
    auto* recvTy = ctx_.exprType(*ix.target);
    bool isArr  = isCollectionTypeName(recvTy);
    bool isDict = isDictTypeName(recvTy);
    if (!isArr && !isDict) return false;
    llvm::Value* recv = lowerExpr(*ix.target);
    if (!recv) return false;

    // --- Array<T> / OwnedArray<T> indexer set (§4.4 / §4.5) ----------------
    if (isArr) {
        llvm::Value* idx = lowerExpr(*ix.indices[0].value);
        if (!idx) return false;
        idx = idx->getType()->isIntegerTy(64) ? idx
            : builder_.CreateSExt(idx, i64());
        auto* lenFn = declareC("bz_array_len", i64(), {ptrTy()});
        llvm::Value* len = builder_.CreateCall(lenFn, {recv});
        llvm::Value* oor = builder_.CreateICmpUGE(idx, len, "ix.oor");
        emitGuardThrow(oor, "IndexOutOfRangeException");
        int64_t esz = collectionElemSize(recvTy);
        llvm::AllocaInst* slot = entryAlloca(val->getType());
        builder_.CreateStore(val, slot);
        // §4.5: an OWNING array's set(owned) disposes the displaced element
        // (a class object reference) before storing the new owner; a borrowing
        // Array<T> set just overwrites (§4.4 — no slot ownership).
        if (llvm::Function* elemDisp = owningCollectionElemDisposer(recvTy)) {
            auto* fn = declareC("bz_array_set_disposing",
                llvm::Type::getVoidTy(llvm_),
                {ptrTy(), i64(), ptrTy(), i64(), ptrTy()});
            builder_.CreateCall(fn, {recv, idx, slot, builder_.getInt64(esz),
                                     elemDisp});
            return true;
        }
        auto* fn = declareC("bz_array_set", llvm::Type::getVoidTy(llvm_),
                            {ptrTy(), i64(), ptrTy(), i64()});
        builder_.CreateCall(fn, {recv, idx, slot, builder_.getInt64(esz)});
        return true;
    }

    // --- Dictionary<K,V> / OwnedDictionary<K,V> indexer set (§4.8 / §4.9) --
    // `dict[key] := v` is insert-or-replace. For an OWNING dictionary the
    // previous value under an existing key is disposed before the new owner is
    // stored (§4.9); a borrowing Dictionary<K,V> just overwrites (§4.8).
    const sema::TypeRef* keyTy =
        genericBackingType(recvTy, 0, /*isSet=*/false, nullptr);
    bool keyIsRecord = keyTy && recordTypeOfRef(keyTy) != nullptr;
    const sema::TypeRef* valTy =
        (recvTy && recvTy->genericArgs.size() > 1) ? recvTy->genericArgs[1]
                                                   : nullptr;
    const sema::TypeSymbol* valRec =
        (valTy && valTy->kind == sema::TypeRef::Kind::Named &&
         valTy->declaration &&
         valTy->declaration->kind == ast::TypeDeclKind::Record)
            ? valTy->declaration : nullptr;
    // Encode the value as the stored void* (mirror the dict Add path in
    // LlvmBackend_Calls.cpp). The cases are: a record (aggregate) value —
    // heap-boxed (malloc + store of its bytes), the box pointer stored, decoded
    // back by the indexer get (issue #393 / §4.8); object/string pointers —
    // stored directly; scalars — widened to i64 and inttoptr'd.
    auto encodeValue = [&](llvm::Value* v) -> llvm::Value* {
        if (valRec && v->getType()->isStructTy()) {
            llvm::StructType* vst = recordStruct(valRec);
            uint64_t vsz = module_->getDataLayout().getTypeAllocSize(vst);
            auto* mallocFn = declareC("malloc", ptrTy(), {i64()});
            llvm::Value* box =
                builder_.CreateCall(mallocFn, {builder_.getInt64((int64_t)vsz)});
            builder_.CreateStore(v, box);   // copy the record bytes into the box
            return box;
        }
        if (v->getType()->isPointerTy()) return v;
        llvm::Value* asI64 = v;
        if (v->getType()->isFloatingPointTy()) {
            if (v->getType()->isDoubleTy())
                asI64 = builder_.CreateBitCast(v, i64());
            else
                asI64 = builder_.CreateZExt(
                    builder_.CreateBitCast(v, i32()), i64());
        } else if (!v->getType()->isIntegerTy(64)) {
            asI64 = builder_.CreateZExt(v, i64());
        }
        return builder_.CreateIntToPtr(asI64, ptrTy());
    };

    llvm::Value* k = lowerExpr(*ix.indices[0].value);
    if (!k) return false;
    llvm::Value* vPtr = encodeValue(val);
    // §4.9: an OWNING dictionary (OwnedDictionary<K,V> where V: class) disposes
    // the displaced value before storing the new owner. Unlike owning ARRAYS
    // (whose owned element is genericArgs[0]), an owning dictionary's owned thing
    // is its VALUE = genericArgs[1]; owningCollectionElemDisposer keys on idx 0,
    // so compute the value-disposer here. The shared elementDisposerThunk
    // disposes+frees any class object via its own vtable, so it applies as-is.
    bool owningDict =
        recvTy && recvTy->astNode && recvTy->astNode->name &&
        !recvTy->astNode->name->parts.empty() &&
        recvTy->astNode->name->parts[0] == "OwnedDictionary" &&
        collectionElemIsClass(recvTy, 1);
    llvm::Function* valDisp = owningDict ? elementDisposerThunk() : nullptr;
    if (keyTy) {
        llvm::Value* keyPtr = k;
        if (!(keyIsRecord && k->getType()->isPointerTy())) {
            llvm::AllocaInst* slot = entryAlloca(k->getType());
            builder_.CreateStore(k, slot);
            keyPtr = slot;
        }
        if (valDisp) {
            auto* fn = declareC("bz_dict_set_gen_disposing",
                llvm::Type::getVoidTy(llvm_), {ptrTy(), ptrTy(), ptrTy(),
                                               ptrTy()});
            builder_.CreateCall(fn, {recv, keyPtr, vPtr, valDisp});
        } else {
            auto* fn = declareC("bz_dict_set_gen", llvm::Type::getVoidTy(llvm_),
                                {ptrTy(), ptrTy(), ptrTy()});
            builder_.CreateCall(fn, {recv, keyPtr, vPtr});
        }
        return true;
    }
    llvm::Value* kd = loadStringData(k);
    llvm::Value* kl = loadStringLen(k);
    if (valDisp) {
        auto* fn = declareC("bz_dict_set_str_disposing",
            llvm::Type::getVoidTy(llvm_),
            {ptrTy(), ptrTy(), i64(), ptrTy(), ptrTy()});
        builder_.CreateCall(fn, {recv, kd, kl, vPtr, valDisp});
    } else {
        auto* fn = declareC("bz_dict_set_str", llvm::Type::getVoidTy(llvm_),
                            {ptrTy(), ptrTy(), i64(), ptrTy()});
        builder_.CreateCall(fn, {recv, kd, kl, vPtr});
    }
    return true;
}


llvm::Value* Emitter::lowerIndexerGet(const ast::IndexExpr& ix) {
    if (!ix.target || ix.indices.empty() || !ix.indices[0].value)
        return nullptr;
    auto* targetTy = ctx_.exprType(*ix.target);
    if (!targetTy || targetTy->kind != sema::TypeRef::Kind::Named ||
        !targetTy->declaration)
        return nullptr;
    auto* ps = findIndexer(targetTy->declaration);
    if (!ps) return nullptr;
    auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl);
    if (!pd) return nullptr;
    // Find a getter accessor body.
    const ast::Expr* getterExpr = nullptr;
    const std::vector<std::unique_ptr<ast::Stmt>>* getterStmts = nullptr;
    if (pd->bodyKind == ast::PropertyBodyKind::Expression && pd->exprBody)
        getterExpr = pd->exprBody.get();
    for (const auto& a : pd->accessors) {
        if (!a.isSet) {
            if (a.exprBody) getterExpr = a.exprBody.get();
            else if (!a.stmts.empty()) getterStmts = &a.stmts;
            break;
        }
    }
    if (!getterExpr && !getterStmts) return nullptr;
    // Bind `this` to the target object and indexer params to the args.
    llvm::Value* obj = lowerExpr(*ix.target);
    if (!obj) return nullptr;
    llvm::Value* savedThis = currentThis_;
    auto savedLocals = locals_;
    auto savedParams = paramSlots_;
    currentThis_ = obj;
    locals_.clear();
    paramSlots_.clear();
    // Bind each indexer parameter to its argument.
    for (std::size_t k = 0; k < ps->indexerParams.size() &&
         k < ix.indices.size(); ++k) {
        if (!ix.indices[k].value) continue;
        llvm::Value* argVal = lowerExpr(*ix.indices[k].value);
        if (!argVal) continue;
        auto* slot = entryAlloca(argVal->getType());
        builder_.CreateStore(argVal, slot);
        paramSlots_[ps->indexerParams[k]] = slot;
    }
    llvm::Value* result = nullptr;
    if (getterExpr) result = lowerExpr(*getterExpr);
    else { lowerScopedBlock(*getterStmts); }
    currentThis_ = savedThis;
    locals_ = std::move(savedLocals);
    paramSlots_ = std::move(savedParams);
    return result;
}


bool Emitter::lowerIndexerSet(const ast::IndexExpr& ix, llvm::Value* val) {
    if (!ix.target || ix.indices.empty() || !ix.indices[0].value)
        return false;
    auto* targetTy = ctx_.exprType(*ix.target);
    if (!targetTy || targetTy->kind != sema::TypeRef::Kind::Named ||
        !targetTy->declaration)
        return false;
    auto* ps = findIndexer(targetTy->declaration);
    if (!ps) return false;
    auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl);
    if (!pd) return false;
    // Find a setter accessor body.
    const ast::Expr* setterExpr = nullptr;
    const std::vector<std::unique_ptr<ast::Stmt>>* setterStmts = nullptr;
    for (const auto& a : pd->accessors) {
        if (a.isSet) {
            if (a.exprBody) setterExpr = a.exprBody.get();
            else if (!a.stmts.empty()) setterStmts = &a.stmts;
            break;
        }
    }
    if (!setterExpr && !setterStmts) return false;
    // Bind `this`, indexer params, and implicit `value` param.
    llvm::Value* obj = lowerExpr(*ix.target);
    if (!obj) return false;
    llvm::Value* savedThis = currentThis_;
    auto savedLocals = locals_;
    auto savedParams = paramSlots_;
    currentThis_ = obj;
    locals_.clear();
    paramSlots_.clear();
    // Indexer params.
    for (std::size_t k = 0; k < ps->indexerParams.size() &&
         k < ix.indices.size(); ++k) {
        if (!ix.indices[k].value) continue;
        llvm::Value* argVal = lowerExpr(*ix.indices[k].value);
        if (!argVal) continue;
        auto* slot = entryAlloca(argVal->getType());
        builder_.CreateStore(argVal, slot);
        paramSlots_[ps->indexerParams[k]] = slot;
    }
    // `value` param: look for the LocalSymbol anchored on pd.
    // The codegen's `lowerPropertySet` uses locals_[pd] for `value`.
    if (val) {
        auto* slot = entryAlloca(val->getType());
        builder_.CreateStore(val, slot);
        locals_[pd] = slot;
    }
    if (setterExpr) lowerExpr(*setterExpr);
    else if (setterStmts) lowerScopedBlock(*setterStmts);
    currentThis_ = savedThis;
    locals_ = std::move(savedLocals);
    paramSlots_ = std::move(savedParams);
    return true;
}


const ast::Expr* Emitter::computedGetterOf(sema::Symbol* sym) {
    auto* ps = dynamic_cast<sema::PropertySymbol*>(sym);
    if (!ps || ps->isIndexer) return nullptr;
    auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl);
    if (!pd || pd->isIndexer) return nullptr;
    // Property-level expression body: `property P: T => expr;`.
    if (pd->bodyKind == ast::PropertyBodyKind::Expression)
        return pd->exprBody.get();
    // Accessor-block getter with an expression body: `{ get => expr; }`.
    if (pd->bodyKind == ast::PropertyBodyKind::Block) {
        for (const auto& acc : pd->accessors) {
            if (!acc.isSet &&
                acc.bodyKind == ast::PropertyAccessorBodyKind::Expression)
                return acc.exprBody.get();
        }
    }
    return nullptr;
}


const ast::PropertyAccessor* Emitter::blockGetterOf(sema::Symbol* sym) {
    auto* ps = dynamic_cast<sema::PropertySymbol*>(sym);
    if (!ps || ps->isIndexer) return nullptr;
    auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl);
    if (!pd || pd->isIndexer) return nullptr;
    if (pd->bodyKind == ast::PropertyBodyKind::Block) {
        for (const auto& acc : pd->accessors) {
            if (!acc.isSet &&
                acc.bodyKind == ast::PropertyAccessorBodyKind::Block)
                return &acc;
        }
    }
    return nullptr;
}


llvm::Function* Emitter::ensurePropertyBlockGetter(const ast::PropertyDecl* pd,
                                          const ast::PropertyAccessor* acc) {
    auto it = propBlockGetterFns_.find(pd);
    if (it != propBlockGetterFns_.end()) return it->second;

    llvm::Type* retTy = lowerType(pd->type.get());
    if (!retTy) retTy = i32();

    auto* fnTy = llvm::FunctionType::get(retTy, {ptrTy()}, false);
    auto* fn = llvm::Function::Create(
        fnTy, llvm::GlobalValue::InternalLinkage,
        "bz$prop$" + std::string(pd->name) + "$get", *module_);
    propBlockGetterFns_[pd] = fn;
    // no capture attribute added (Captures API varies by LLVM version)

    auto* entryBB = llvm::BasicBlock::Create(llvm_, "entry", fn);

    // --- Save entire emitter state ---
    auto* savedFn        = currentFn_;
    auto  savedBB        = builder_.GetInsertBlock();
    auto  savedBBOffset  = savedBB ? builder_.GetInsertPoint() : decltype(savedBB->begin()){};
    auto* savedThis      = currentThis_;
    auto  savedLocals    = locals_;
    auto  savedParams    = paramSlots_;
    auto  savedRef       = refParams_;
    auto  savedDispose   = disposeStack_;
    auto  savedLoop      = loopStack_;
    auto* savedUnwind    = unwindTarget_;
    auto* savedProp      = propagateBB_;
    auto* savedCaught    = currentCaughtExc_;
    auto  savedUD        = unwindScopeDepth_;
    auto  savedDiv       = diverged_;
    auto  savedGSubst    = genericSubst_;

    // --- Switch into the getter function ---
    currentFn_          = fn;
    builder_.SetInsertPoint(entryBB);
    currentThis_        = &*fn->args().begin();
    locals_.clear();
    paramSlots_.clear();
    refParams_.clear();
    disposeStack_.clear();
    loopStack_.clear(); finallyStack_.clear();
    unwindTarget_       = nullptr;
    propagateBB_        = nullptr;
    currentCaughtExc_   = nullptr;
    unwindScopeDepth_   = 0;
    diverged_           = false;

    // --- Emit body ---
    lowerScopedBlock(acc->stmts);
    if (!builder_.GetInsertBlock()->getTerminator())
        emitImplicitReturn(fn);

    // --- Restore state ---
    currentFn_          = savedFn;
    currentThis_        = savedThis;
    locals_             = std::move(savedLocals);
    paramSlots_         = std::move(savedParams);
    refParams_          = std::move(savedRef);
    disposeStack_       = std::move(savedDispose);
    loopStack_          = std::move(savedLoop);
    unwindTarget_       = savedUnwind;
    propagateBB_        = savedProp;
    currentCaughtExc_   = savedCaught;
    unwindScopeDepth_   = savedUD;
    diverged_           = savedDiv;
    genericSubst_       = std::move(savedGSubst);
    if (savedBB) builder_.SetInsertPoint(savedBB, savedBBOffset);

    return fn;
}


const ast::PropertyAccessor* Emitter::computedSetterOf(const ast::Expr& target) {
    auto* ps = dynamic_cast<sema::PropertySymbol*>(ctx_.exprSymbol(target));
    if (!ps || ps->isIndexer || isAutoProperty(ps)) return nullptr;
    auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl);
    if (!pd || pd->bodyKind != ast::PropertyBodyKind::Block) return nullptr;
    for (const auto& acc : pd->accessors) {
        if (acc.isSet &&
            acc.bodyKind != ast::PropertyAccessorBodyKind::Abstract)
            return &acc;
    }
    return nullptr;
}


bool Emitter::lowerPropertySet(const ast::Expr& target, const ast::Expr& value) {
    if (!computedSetterOf(target)) return false;
    llvm::Value* v = lowerExpr(value);
    return v ? lowerPropertySetValue(target, v) : false;
}


bool Emitter::lowerPropertySetValue(const ast::Expr& target, llvm::Value* v) {
    const ast::PropertyAccessor* setter = computedSetterOf(target);
    if (!setter || !v) return false;
    auto* ps = dynamic_cast<sema::PropertySymbol*>(ctx_.exprSymbol(target));
    auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl);
    llvm::Value* recv = currentThis_;
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&target))
        recv = ma->target ? lowerExpr(*ma->target) : currentThis_;
    if (!recv) return false;
    // Bind `value` — sema anchored its symbol's decl on the PropertyDecl.
    llvm::AllocaInst* slot = entryAlloca(v->getType());
    builder_.CreateStore(v, slot);
    locals_[pd] = slot;
    llvm::Value* savedThis = currentThis_;
    currentThis_ = recv;
    if (setter->bodyKind == ast::PropertyAccessorBodyKind::Expression &&
        setter->exprBody)
        lowerExpr(*setter->exprBody);
    else
        lowerScopedBlock(setter->stmts);
    currentThis_ = savedThis;
    return true;
}


bool Emitter::lowerPropertyCompound(const ast::AssignmentStmt& as) {
    if (!computedSetterOf(*as.target)) return false;
    llvm::Value* cur = lowerExpr(*as.target);   // getter read (CG33c)
    llvm::Value* rhs = lowerExpr(*as.value);
    if (!cur || !rhs) return false;
    std::string op(as.op);
    if (!op.empty() && op.back() == '=') op.pop_back();
    llvm::Value* res = emitCompoundOp(
        op, cur, rhs, cur->getType()->isFloatingPointTy(),
        isUnsignedTargetExpr(*as.target));
    return res ? lowerPropertySetValue(*as.target, res) : false;
}


// per §24.3 / §24.4: inside the declaring class, `event += handler` and
// `event -= handler` are a fixed, language-defined desugaring to
// `event.Add(handler)` and `event.Remove(handler)`. They are NOT the
// compound-assignment statements of §17 (they neither read nor assign the
// event), and `event` here has static type EventSource<T>, available only
// inside the declaring class. Returns true when handled.
bool Emitter::tryLowerEventCompound(const ast::AssignmentStmt& as) {
    if (!as.target || !as.value) return false;
    if (as.op != "+=" && as.op != "-=") return false;
    const sema::EventSymbol* es = eventSymbolOf(*as.target);
    if (!es) return false;
    llvm::Value* src = eventSourcePtr(*as.target);
    if (!src) return false;
    // Lower the handler to a closure, hinting the expected (payload)->() type
    // so an untyped lambda param resolves the payload type.
    const sema::TypeRef* saved = expectedFunctionType_;
    sema::TypeRef handlerTy;
    handlerTy.kind = sema::TypeRef::Kind::Function;
    if (const sema::TypeRef* pt = eventPayloadType(es)) {
        sema::TypeRef::FuncParam p;
        p.type = const_cast<sema::TypeRef*>(pt);
        handlerTy.funcParams.push_back(p);
    }
    expectedFunctionType_ = &handlerTy;
    llvm::Value* closure = lowerExpr(*as.value);
    expectedFunctionType_ = saved;
    if (!closure) return true;  // handled (error path); avoid bad fallthrough
    if (as.op == "+=") {
        auto* addFn = declareC("bz_event_add", i64(), {ptrTy(), ptrTy()});
        builder_.CreateCall(addFn, {src, closure});
    } else {
        auto* remFn = declareC("bz_event_remove_closure",
                               llvm::Type::getVoidTy(llvm_), {ptrTy(), ptrTy()});
        builder_.CreateCall(remFn, {src, closure});
    }
    return true;
}


bool Emitter::argsCanFill(const std::vector<ast::Param>& params,
                        const std::vector<ast::CallArgument>& args) {
    std::vector<bool> filled(params.size(), false);
    std::size_t pos = 0;
    for (const auto& a : args) {
        std::size_t idx;
        if (a.name.empty()) {
            if (pos >= params.size()) return false;
            idx = pos++;
        } else {
            idx = params.size();
            for (std::size_t j = 0; j < params.size(); ++j)
                if (params[j].name == a.name) { idx = j; break; }
            if (idx >= params.size()) return false;
        }
        if (filled[idx]) return false;
        filled[idx] = true;
    }
    for (std::size_t j = 0; j < params.size(); ++j)
        if (!filled[j] && !params[j].defaultValue) return false;
    return true;
}


bool Emitter::buildOrderedArgs(const std::vector<ast::Param>& params,
                      const std::vector<ast::CallArgument>& args,
                      std::vector<llvm::Value*>& out) {
    std::vector<llvm::Value*> ordered(params.size(), nullptr);
    std::size_t pos = 0;
    for (const auto& a : args) {
        if (!a.value) return false;
        std::size_t idx = params.size();
        if (a.name.empty()) {
            idx = pos++;
        } else {
            for (std::size_t j = 0; j < params.size(); ++j)
                if (params[j].name == a.name) { idx = j; break; }
        }
        if (idx >= params.size() || ordered[idx]) return false;
        // CG50: a ref/out parameter receives the address of its argument
        // lvalue; an ordinary parameter receives the argument's value.
        // §22.4: `out var x` — ensureOutVarSlots already allocated the
        // slot; lvalueAddr will find it via locals_ lookup.
        llvm::Value* v;
        if (isByRefMod(params[idx].modifier)) {
            llvm::Type* et = nullptr;
            v = lvalueAddr(*a.value, et);
        } else {
            // When the argument is a lambda and the formal parameter has a
            // function type, inform lowerLambda of the expected param types
            // so unannotated lambda params get the right LLVM type.
            const sema::TypeRef* savedExpected = expectedFunctionType_;
            if (dynamic_cast<const ast::LambdaExpr*>(a.value.get()) &&
                params[idx].type) {
                sema::TypeRef* pt = ctx_.resolvedType(*params[idx].type);
                if (pt && pt->kind == sema::TypeRef::Kind::Function)
                    expectedFunctionType_ = pt;
            }
            v = lowerExpr(*a.value);
            expectedFunctionType_ = savedExpected;
        }
        if (!v) return false;
        // CG-nullable: if the formal parameter is a nullable value type (a
        // primitive, or a nullable enum — issue #364) but the argument value is
        // a plain value, wrap it.
        if (params[idx].type) {
            sema::TypeRef* formalTy = ctx_.resolvedType(*params[idx].type);
            if (isNullableValueTy(formalTy)) {
                auto* expectedSt = nullableValueStructTy(formalTy);
                if (v->getType() != expectedSt) {
                    if (v->getType()->isPointerTy() &&
                        llvm::isa<llvm::ConstantPointerNull>(v)) {
                        v = nullableValueNull(formalTy);
                    } else if (v->getType() == expectedSt->getElementType(1)) {
                        v = wrapNullableValue(formalTy, v);
                    }
                }
            }
            // §8.6: a value-typed argument bound to an Object/interface
            // parameter is boxed, so a later `is`/`as` or virtual dispatch on
            // that parameter (which lowers to a descriptor walk through the
            // object's first word) sees a real box rather than the raw value
            // bytes. boxForTarget is a no-op for reference arguments (#399).
            v = boxForTarget(v, formalTy, a.value.get());
        }
        ordered[idx] = v;
    }
    // §22.5: a default-value expression may reference an EARLIER parameter
    // (e.g. `M(a, b := a + 1)`). At the call site the earlier parameters are
    // already lowered into `ordered[k]` (k < j). To make a reference to such a
    // parameter resolve to this call's argument value, bind each earlier
    // ParameterSymbol to a slot holding its computed value via paramSlots_
    // before lowering the default — exactly the mechanism a parameter access
    // uses inside a method body (localSlot → paramSlots_). A reference to a
    // LATER parameter is rejected by sema (not yet bound here), and a
    // literal/constant default ignores these bindings, so both keep working.
    //
    // Map each parameter position to its ParameterSymbol. The collection pass
    // records sym->declType = params[k].type.get(), so the AST type pointer is
    // the stable key (declared method/ctor parameters always carry a type).
    std::vector<const sema::ParameterSymbol*> paramSyms;
    bool anyDefaultNeeded = false;
    for (std::size_t j = 0; j < params.size(); ++j)
        if (!ordered[j] && params[j].defaultValue) { anyDefaultNeeded = true; break; }
    if (anyDefaultNeeded) {
        paramSyms.assign(params.size(), nullptr);
        for (std::size_t k = 0; k < params.size(); ++k) {
            const ast::Type* pt = params[k].type.get();
            if (!pt) continue;
            for (const auto& s : ctx_.symbols()) {
                auto* psym = dynamic_cast<const sema::ParameterSymbol*>(s.get());
                if (psym && psym->declType == pt) { paramSyms[k] = psym; break; }
            }
        }
    }
    for (std::size_t j = 0; j < params.size(); ++j) {
        if (!ordered[j]) {
            const ast::Expr* dv = params[j].defaultValue.get();
            if (!dv) return false;
            // Temporarily bind each already-computed earlier parameter so the
            // default expression's references resolve to this call's values.
            auto savedParams = paramSlots_;
            for (std::size_t k = 0; k < j; ++k) {
                if (!ordered[k] || !paramSyms[k]) continue;
                auto* slot = entryAlloca(ordered[k]->getType());
                builder_.CreateStore(ordered[k], slot);
                paramSlots_[paramSyms[k]] = slot;
            }
            ordered[j] = lowerExpr(*dv);
            paramSlots_ = std::move(savedParams);
            if (!ordered[j]) return false;
        }
        out.push_back(ordered[j]);
    }
    return true;
}


}  // namespace bezel::codegen
