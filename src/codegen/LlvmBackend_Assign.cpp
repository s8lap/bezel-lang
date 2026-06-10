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

    // §4.15 (#549): ArraySpan<T>[i] / ReadOnlyArraySpan<T>[i]
    // → Source[Start + i] with bounds-check against Length (not array Count).
    if (isArraySpanTypeName(recvTy)) {
        llvm::Value* spanVal = lowerExpr(*ix.target);
        if (!spanVal) return nullptr;
        llvm::Value* idx = lowerExpr(*ix.indices[0].value);
        if (!idx) return nullptr;
        // Unpack span fields.
        auto* spanSt = arraySpanStructType();
        auto* slot = entryAlloca(spanSt);
        builder_.CreateStore(spanVal, slot);
        llvm::Value* src  = builder_.CreateLoad(ptrTy(),
            builder_.CreateStructGEP(spanSt, slot, 0), "sp.src");
        llvm::Value* base = builder_.CreateLoad(i32(),
            builder_.CreateStructGEP(spanSt, slot, 1), "sp.st");
        llvm::Value* slen = builder_.CreateLoad(i32(),
            builder_.CreateStructGEP(spanSt, slot, 2), "sp.len");
        // Bounds-check i against Length (§4.15: IndexOutOfRangeException).
        if (!idx->getType()->isIntegerTy(32))
            idx = builder_.CreateTrunc(idx, i32());
        llvm::Value* idxNeg = builder_.CreateICmpSLT(idx, builder_.getInt32(0));
        llvm::Value* idxOOB = builder_.CreateICmpSGE(idx, slen);
        emitGuardThrow(builder_.CreateOr(idxNeg, idxOOB),
                       "IndexOutOfRangeException");
        // Absolute index = Start + i.
        llvm::Value* absIdx = builder_.CreateAdd(
            builder_.CreateSExt(base, i64()),
            builder_.CreateSExt(idx,  i64()), "sp.absi");
        int64_t esz = collectionElemSize(recvTy);
        llvm::Type* retTy = i32();
        if (!recvTy->genericArgs.empty() && recvTy->genericArgs[0])
            if (auto* et = lowerTypeRef(recvTy->genericArgs[0])) retTy = et;
        llvm::AllocaInst* out = entryAlloca(retTy);
        auto* getFn = declareC("bz_array_get", llvm::Type::getVoidTy(llvm_),
                               {ptrTy(), i64(), ptrTy(), i64()});
        builder_.CreateCall(getFn, {src, absIdx, out, builder_.getInt64(esz)});
        return builder_.CreateLoad(retTy, out, "sp.elem");
    }

    // issue #401 / §4.1-§4.3: a list-INTERFACE receiver (IReadOnlyList<T> /
    // IList<T> / IOwnedList<T>) is array-backed; its runtime pointer is the same
    // bz_array_t* as the concrete Array<T>/OwnedArray<T>, and its element type is
    // genericArgs[0] (same as the array path), so it takes the `isArr` get path.
    bool isArr  = isCollectionTypeName(recvTy) || isListInterfaceName(recvTy);
    bool isDict = isDictTypeName(recvTy);
    if (!isArr && !isDict) return nullptr;
    llvm::Value* recv = lowerExpr(*ix.target);
    if (!recv) return nullptr;

    // --- Array<T> / OwnedArray<T> indexer get (§4.4 / §4.5; #401 §4.1-§4.3) --
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
    // issue #539 / §4.8 / §7: nullable value types (int?, bool?, enum?,
    // nullable record) lower to a tagged {i8 hasValue; T} struct and are
    // heap-boxed by encodeValue (the same path as non-nullable records).
    // Decode them the same way: load the struct bytes back from the box.
    bool valNullableVal = valTy && isNullableValueTy(valTy);
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
    // cases are: a record or nullable-value-type tagged struct — load the boxed
    // struct out of the box (issue #393 / #539); scalars — decoded from the
    // inttoptr'd slot; object/string pointers — the void* IS the pointer.
    llvm::Value* valPtr = builder_.CreateLoad(ptrTy(), tmpSlot);
    if ((valRec || valNullableVal) && valLlvm->isStructTy())
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
// `.Add(k, v)` lowering. `val` is the already-lowered RHS; `valueExpr` is the
// RHS AST (when available) used to decide transfer-vs-retain for a #429
// borrowing Array<string> overwrite.
bool Emitter::lowerCollectionIndexerSet(const ast::IndexExpr& ix,
                                        llvm::Value* val,
                                        const ast::Expr* valueExpr) {
    if (!ix.target || ix.indices.empty() || !ix.indices[0].value || !val)
        return false;
    auto* recvTy = ctx_.exprType(*ix.target);

    // §4.15 (#549): ArraySpan<T>[i] := value — borrowing-set; no disposal.
    // ReadOnlyArraySpan<T> has no setter; codegen should never reach here for it,
    // but guard defensively.
    if (isArraySpanTypeName(recvTy)) {
        std::string_view rcn = (recvTy->astNode && recvTy->astNode->name &&
                                !recvTy->astNode->name->parts.empty())
            ? std::string_view(recvTy->astNode->name->parts[0]) : "";
        if (rcn == "ReadOnlyArraySpan") return false;  // read-only: no setter
        llvm::Value* spanVal = lowerExpr(*ix.target);
        if (!spanVal) return false;
        llvm::Value* idx = lowerExpr(*ix.indices[0].value);
        if (!idx) return false;
        auto* spanSt = arraySpanStructType();
        auto* slot = entryAlloca(spanSt);
        builder_.CreateStore(spanVal, slot);
        llvm::Value* src  = builder_.CreateLoad(ptrTy(),
            builder_.CreateStructGEP(spanSt, slot, 0), "sp.src");
        llvm::Value* base = builder_.CreateLoad(i32(),
            builder_.CreateStructGEP(spanSt, slot, 1), "sp.st");
        llvm::Value* slen = builder_.CreateLoad(i32(),
            builder_.CreateStructGEP(spanSt, slot, 2), "sp.len");
        if (!idx->getType()->isIntegerTy(32))
            idx = builder_.CreateTrunc(idx, i32());
        llvm::Value* idxNeg = builder_.CreateICmpSLT(idx, builder_.getInt32(0));
        llvm::Value* idxOOB = builder_.CreateICmpSGE(idx, slen);
        emitGuardThrow(builder_.CreateOr(idxNeg, idxOOB),
                       "IndexOutOfRangeException");
        llvm::Value* absIdx = builder_.CreateAdd(
            builder_.CreateSExt(base, i64()),
            builder_.CreateSExt(idx,  i64()), "sp.absi");
        int64_t esz = collectionElemSize(recvTy);
        // Borrowing set — no disposal of the displaced slot (§4.15 / §4.4).
        llvm::AllocaInst* valSlot = entryAlloca(val->getType());
        builder_.CreateStore(val, valSlot);
        auto* setFn = declareC("bz_array_set", llvm::Type::getVoidTy(llvm_),
                               {ptrTy(), i64(), ptrTy(), i64()});
        builder_.CreateCall(setFn, {src, absIdx, valSlot, builder_.getInt64(esz)});
        return true;
    }

    // issue #401 / §4.1-§4.3: a list-INTERFACE receiver is array-backed (same
    // bz_array_t* as the concrete array), so it takes the `isArr` set path. The
    // OWNING interface IOwnedList<T> (§4.2) carries the disposing-setter
    // semantics; IList<T> (§4.1, borrowing) and IReadOnlyList<T> (get-only) do
    // not. IReadOnlyList<T> declares no setter, so it never reaches this set.
    bool listIfaceOwning = false;
    bool isListIface = isListInterfaceName(recvTy, &listIfaceOwning);
    bool isArr  = isCollectionTypeName(recvTy) || isListIface;
    bool isDict = isDictTypeName(recvTy);
    if (!isArr && !isDict) return false;
    llvm::Value* recv = lowerExpr(*ix.target);
    if (!recv) return false;

    // --- Array<T> / OwnedArray<T> indexer set (§4.4 / §4.5; #401 §4.1-§4.3) --
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
        // fix #455 / §4.5: when the element type is a nullable value type
        // (int?, bool?, enum?, record?) the lowered val may be the inner
        // primitive rather than the full tagged {i8 hasValue; T} struct.
        // Lift it to the tagged struct before allocating the slot so the
        // alloca matches the esz that collectionElemSize computed.
        // A null pointer val (from an untyped `null` literal) maps to the
        // tagged null struct; an inner scalar maps to {hasValue=1, val}.
        {
            const sema::TypeRef* elemTyRef =
                (recvTy && !recvTy->genericArgs.empty())
                ? recvTy->genericArgs[0] : nullptr;
            if (elemTyRef && isNullableValueTy(elemTyRef)) {
                llvm::StructType* expected = nullableValueStructTy(elemTyRef);
                if (expected && val->getType() != expected) {
                    if (val->getType()->isPointerTy())
                        val = nullableValueNull(elemTyRef);  // null → tagged null
                    else
                        val = wrapNullableValue(elemTyRef, val);  // scalar → {1, val}
                }
            }
        }
        // #429 / §4.6 / §28.9: a borrowing Array<string> overwrite via the
        // indexer `arr[i] := v` releases the displaced old element and takes a +1
        // on the new one (transfer a +1 producer, retain anything else). The
        // disposing setter releases the OLD value AFTER reading it, so emitting
        // the new-retain BEFORE the call keeps a self-set `a[i] := a[i]` safe (the
        // rc dips +1 then back, never transiently 0 — no UAF / double-free). This
        // is checked BEFORE the owning-class disposer below; a string element is
        // never a class, so the two paths are mutually exclusive.
        if (llvm::Function* strDisp = stringElementReleaseDisposer(recvTy)) {
            if (!valueExpr || !isPlusOneStringProducer(valueExpr))
                emitStringRetain(val);
            llvm::AllocaInst* slot = entryAlloca(val->getType());
            builder_.CreateStore(val, slot);
            auto* fn = declareC("bz_array_set_disposing",
                llvm::Type::getVoidTy(llvm_),
                {ptrTy(), i64(), ptrTy(), i64(), ptrTy()});
            builder_.CreateCall(fn, {recv, idx, slot, builder_.getInt64(esz),
                                     strDisp});
            return true;
        }
        llvm::AllocaInst* slot = entryAlloca(val->getType());
        builder_.CreateStore(val, slot);
        // §4.5 / §4.2: an OWNING array's set(owned) — whether reached through the
        // concrete OwnedArray<T> or through an IOwnedList<T> view (#401) —
        // disposes the displaced class element before storing the new owner; a
        // borrowing Array<T> / IList<T> set just overwrites (no slot ownership).
        // owningCollectionElemDisposer keys on the concrete "Owned…" name, so it
        // misses IOwnedList<T>; supply the shared class-element disposer for the
        // owning-interface case (T: class is guaranteed by §4.2's constraint).
        llvm::Function* elemDisp = owningCollectionElemDisposer(recvTy);
        if (!elemDisp && listIfaceOwning) elemDisp = elementDisposerThunk();
        if (elemDisp) {
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
    // issue #539 / §4.8 / §7: nullable value types lower to a tagged {i8,T}
    // struct and must be heap-boxed the same way non-nullable records are.
    bool valNullableVal = valTy && isNullableValueTy(valTy);
    // Encode the value as the stored void* (mirror the dict Add path in
    // LlvmBackend_Calls.cpp). The cases are: a record or nullable-value-type
    // tagged struct — heap-boxed (malloc + store of its bytes), the box pointer
    // stored, decoded back by the indexer get (issue #393 / #539 / §4.8);
    // object/string pointers — stored directly; scalars — widened to i64 and
    // inttoptr'd.
    // issue #544 (mirrors LlvmBackend_Calls.cpp encodeValue fix): when the
    // value type is nullable (int?, enum?, nullable record) but the lowered
    // value is NOT yet a struct (scalar literal `d["k"] := 7` lowers as i32;
    // null literal lowers as a null pointer), promote it to the tagged
    // {i8,T} struct BEFORE the struct-boxing branch below, so the box always
    // holds the full tagged struct and the decode path (lowerCollectionIndexerGet
    // / TryGetValue) can safely load {i8,T} from the box.
    auto encodeValue = [&](llvm::Value* v) -> llvm::Value* {
        if (valNullableVal && !v->getType()->isStructTy()) {
            if (v->getType()->isPointerTy() &&
                llvm::isa<llvm::ConstantPointerNull>(v)) {
                v = nullableValueNull(valTy);    // null literal → {i8=0, T=0}
            } else if (!v->getType()->isPointerTy()) {
                v = wrapNullableValue(valTy, v); // scalar T → {i8=1, T=v}
            }
        }
        // fix #570 / §4.8 / §7 / §9: nullable RECORD value (Item?) — when the
        // caller passes the inner record struct directly (e.g. `d["k"] := new Item(42)`
        // implicitly coerced to Item?), the value is an Item struct (not the
        // tagged {i8,Item} struct).  The promotion above skips it (isStructTy() is
        // true).  Wrap into {i8=1, Item} before boxing so decode always finds the
        // full tagged struct.  Detect by comparing against the outer tagged struct.
        if (valNullableVal && v->getType()->isStructTy() &&
            v->getType() != nullableValueStructTy(valTy))
            v = wrapNullableValue(valTy, v);  // inner record → {i8=1, record}
        if ((valRec || valNullableVal) && v->getType()->isStructTy()) {
            // Box the struct (record or nullable-value tagged struct).
            uint64_t vsz =
                module_->getDataLayout().getTypeAllocSize(v->getType());
            auto* mallocFn = declareC("malloc", ptrTy(), {i64()});
            llvm::Value* box =
                builder_.CreateCall(mallocFn, {builder_.getInt64((int64_t)vsz)});
            builder_.CreateStore(v, box);   // copy the struct bytes into the box
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
    // #401 / §4.8: a borrowing Dictionary<K, record> heap-boxes its values, so
    // OVERWRITING an existing key would otherwise leak the displaced box. Free
    // the displaced box here (plain free — a record is a value type, §3.2 / §9)
    // via the disposing setter, so it is gone from the table and not freed again
    // at scope end (no double-free; the scope-end #401 path frees only the
    // surviving box). dictValueBoxFreer is non-null only for that case, and is
    // mutually exclusive with the owning-class disposer above.
    // #429 / §28.9: a borrowing Dictionary<K, string> co-owns a +1 per stored
    // value. Overwriting an existing key must release the OLD value's +1 (via the
    // disposing setter) and take a +1 on the NEW one (retain-before-release so a
    // self-overwrite `d[k] := d[k]` never dips rc to 0 — no UAF/double-free).
    llvm::Function* valDisp =
        owningDict ? elementDisposerThunk() : dictValueBoxFreer(recvTy);
    bool isStrVal = isStringValueDict(recvTy);
    if (!valDisp && isStrVal)
        valDisp = stringElementReleaseThunk();
    // #429: retain the new string value BEFORE encoding (val is still the raw
    // bz_string* here; encodeValue returns it as-is for pointer values). Emit
    // the retain BEFORE the store so a self-overwrite `d[k] := d[k]` always
    // sees rc >= 2 when the old value is released. A +1 producer is transferred.
    if (isStrVal && (!valueExpr || !isPlusOneStringProducer(valueExpr)))
        emitStringRetain(val);
    llvm::Value* vPtr = encodeValue(val);
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
    // Find a getter accessor body. The getter is either an expression body
    // (`property [i]: T => expr;` or `get => expr;`) or a BLOCK body
    // (`get => do … end;`, §23.4). A block body may contain a `return`, so it
    // must be lowered as a standalone function (§22.9) rather than inlined into
    // the caller — see ensureIndexerBlockGetter.
    const ast::Expr* getterExpr = nullptr;
    const ast::PropertyAccessor* getterBlock = nullptr;
    if (pd->bodyKind == ast::PropertyBodyKind::Expression && pd->exprBody)
        getterExpr = pd->exprBody.get();
    for (const auto& a : pd->accessors) {
        if (!a.isSet) {
            if (a.bodyKind == ast::PropertyAccessorBodyKind::Block)
                getterBlock = &a;
            else if (a.exprBody)
                getterExpr = a.exprBody.get();
            break;
        }
    }
    if (!getterExpr && !getterBlock) return nullptr;

    // --- Block-form getter: emit/call a standalone getter function. ---
    // §23.4 / §22.9 / §9.4: the body's `return` must terminate only the getter,
    // and the (possibly record/aggregate) result flows back by value through the
    // function's declared return type — exactly as a record-returning method
    // does. Inlining the block would (a) discard the value and (b) ret out of
    // the enclosing method with the wrong type.
    if (getterBlock) {
        llvm::Value* obj = lowerExpr(*ix.target);
        if (!obj) return nullptr;
        // §9.2: a record receiver is a by-value struct; spill it and pass the
        // address (the getter takes `this` as a pointer), mirroring the
        // block-form property-getter call path.
        if (!obj->getType()->isPointerTy()) {
            llvm::AllocaInst* tmp = entryAlloca(obj->getType());
            builder_.CreateStore(obj, tmp);
            obj = tmp;
        }
        std::vector<llvm::Value*> callArgs;
        callArgs.push_back(obj);
        for (std::size_t k = 0; k < ps->indexerParams.size() &&
             k < ix.indices.size(); ++k) {
            if (!ix.indices[k].value) return nullptr;
            llvm::Value* argVal = lowerExpr(*ix.indices[k].value);
            if (!argVal) return nullptr;
            callArgs.push_back(argVal);
        }
        llvm::Function* fn = ensureIndexerBlockGetter(pd, getterBlock, ps);
        llvm::Value* res =
            builder_.CreateCall(fn->getFunctionType(), fn, callArgs);
        // §31.1/§31.2: a throw inside the standalone getter leaves the pending
        // exception set and returns; route it to the enclosing unwind target,
        // exactly as an ordinary method call does.
        emitPendingCheck();
        return res;
    }

    // --- Expression-bodied getter: inline the body against `this`+params. ---
    // Bind `this` to the target object and indexer params to the args.
    llvm::Value* obj = lowerExpr(*ix.target);
    if (!obj) return nullptr;
    llvm::Value* savedThis = currentThis_;
    auto savedLocals = locals_;
    auto savedParams = paramSlots_;
    // per §14.1 (generics are monomorphized): a generic indexer
    // `property [i: int]: T` on `Box<Pair>` returns a `Pair`, not the erased
    // `T`. Install the receiver's substitution so a `T`-typed field read inside
    // the getter body (`=> _v`) loads the concrete record (§9: a record is a
    // value type), not its i32 erasure (which would truncate the multi-field
    // record to one word).
    auto savedSubst = setupReceiverGenericSubst(ix.target.get(), ps->parent);
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
    llvm::Value* result = lowerExpr(*getterExpr);
    currentThis_ = savedThis;
    locals_ = std::move(savedLocals);
    paramSlots_ = std::move(savedParams);
    genericSubst_ = std::move(savedSubst);
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


// §20 / §23.4: compound assignment `a[i] OP= v` through a user-defined indexer
// property (a `property [i: T]: U` with get/set accessors). Per §23.4, this
// desugars to `a[i] := a[i] OP v`: read via the getter, compute the new value,
// then write via the setter. The receiver and each index expression are evaluated
// ONCE and reused for both calls, avoiding double side-effects.
//
// This mirrors lowerPropertyCompound (§22.9 non-indexer properties) but for the
// indexer case: lowerPropertyCompound short-circuits early on `ps->isIndexer`,
// and the generic lvalueAddr fallback returns null for user-indexer targets (it
// only handles the built-in inline-array case via llvm::ArrayType). Without this
// function the compound assign was silently a no-op (returns old value, no store).
bool Emitter::lowerIndexerCompound(const ast::AssignmentStmt& as) {
    if (!as.target || !as.value) return false;
    auto* ix = dynamic_cast<const ast::IndexExpr*>(as.target.get());
    if (!ix || !ix->target || ix->indices.empty() || !ix->indices[0].value)
        return false;
    // Check that this is a user-defined indexer (class/record with a property
    // [i]: T declaration). Built-in arrays fall through to lvalueAddr instead.
    auto* targetTy = ctx_.exprType(*ix->target);
    if (!targetTy || targetTy->kind != sema::TypeRef::Kind::Named ||
        !targetTy->declaration)
        return false;
    auto* ps = findIndexer(targetTy->declaration);
    if (!ps) return false;
    auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl);
    if (!pd) return false;

    // --- Locate getter body. ---
    const ast::Expr* getterExpr = nullptr;
    const ast::PropertyAccessor* getterBlock = nullptr;
    if (pd->bodyKind == ast::PropertyBodyKind::Expression && pd->exprBody)
        getterExpr = pd->exprBody.get();
    for (const auto& a : pd->accessors) {
        if (!a.isSet) {
            if (a.bodyKind == ast::PropertyAccessorBodyKind::Block)
                getterBlock = &a;
            else if (a.exprBody)
                getterExpr = a.exprBody.get();
            break;
        }
    }
    if (!getterExpr && !getterBlock) return false;

    // --- Locate setter body. ---
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

    // --- Evaluate receiver and all index params ONCE. ---
    // Both the getter and setter share the resulting slots so neither expression
    // is re-evaluated. This avoids observable double side-effects on the receiver
    // and/or index sub-expressions (per §23.4's single-evaluation contract).
    llvm::Value* obj = lowerExpr(*ix->target);
    if (!obj) return false;

    // Spill each index param into a slot; the getter and setter bodies will
    // read back from paramSlots_ which we rebuild identically for each call.
    std::vector<llvm::AllocaInst*> idxSlots;
    idxSlots.reserve(ps->indexerParams.size());
    for (std::size_t k = 0; k < ps->indexerParams.size() &&
         k < ix->indices.size(); ++k) {
        if (!ix->indices[k].value) { idxSlots.push_back(nullptr); continue; }
        llvm::Value* argVal = lowerExpr(*ix->indices[k].value);
        if (!argVal) { idxSlots.push_back(nullptr); continue; }
        llvm::AllocaInst* slot = entryAlloca(argVal->getType());
        builder_.CreateStore(argVal, slot);
        idxSlots.push_back(slot);
    }

    // --- Call getter (inline expr-body or standalone block-body function). ---
    llvm::Value* cur = nullptr;
    if (getterBlock) {
        // Block-form getter: call the standalone function, sharing the already-
        // lowered obj (as a pointer) and pre-computed index values.
        llvm::Value* objPtr = obj;
        if (!objPtr->getType()->isPointerTy()) {
            llvm::AllocaInst* tmp = entryAlloca(obj->getType());
            builder_.CreateStore(obj, tmp);
            objPtr = tmp;
        }
        std::vector<llvm::Value*> callArgs;
        callArgs.push_back(objPtr);
        for (llvm::AllocaInst* slot : idxSlots) {
            if (!slot) return false;
            llvm::Value* v = builder_.CreateLoad(slot->getAllocatedType(), slot);
            callArgs.push_back(v);
        }
        llvm::Function* fn = ensureIndexerBlockGetter(pd, getterBlock, ps);
        cur = builder_.CreateCall(fn->getFunctionType(), fn, callArgs);
        emitPendingCheck();
    } else {
        // Expression-body getter: inline against obj and pre-computed params.
        llvm::Value* savedThis = currentThis_;
        auto savedLocals = locals_;
        auto savedParams = paramSlots_;
        auto savedSubst = setupReceiverGenericSubst(ix->target.get(), ps->parent);
        currentThis_ = obj;
        locals_.clear();
        paramSlots_.clear();
        for (std::size_t k = 0; k < ps->indexerParams.size() &&
             k < idxSlots.size(); ++k) {
            if (!idxSlots[k]) continue;
            paramSlots_[ps->indexerParams[k]] = idxSlots[k];
        }
        cur = lowerExpr(*getterExpr);
        currentThis_ = savedThis;
        locals_ = std::move(savedLocals);
        paramSlots_ = std::move(savedParams);
        genericSubst_ = std::move(savedSubst);
    }
    if (!cur) return false;

    // --- Apply the compound operator. ---
    llvm::Value* rhs = lowerExpr(*as.value);
    if (!rhs) return false;
    std::string op(as.op);
    if (!op.empty() && op.back() == '=') op.pop_back();
    llvm::Value* newVal = emitCompoundOp(
        op, cur, rhs, cur->getType()->isFloatingPointTy(),
        isUnsignedTargetExpr(*as.target));
    if (!newVal) return false;

    // --- Call setter with the new value and the pre-computed receiver/params. ---
    llvm::Value* savedThis = currentThis_;
    auto savedLocals = locals_;
    auto savedParams = paramSlots_;
    currentThis_ = obj;
    locals_.clear();
    paramSlots_.clear();
    // Bind each index param from its pre-computed slot.
    for (std::size_t k = 0; k < ps->indexerParams.size() &&
         k < idxSlots.size(); ++k) {
        if (!idxSlots[k]) continue;
        paramSlots_[ps->indexerParams[k]] = idxSlots[k];
    }
    // Bind `value` (anchored on the PropertyDecl, per §22.10 / lowerIndexerSet).
    {
        llvm::AllocaInst* vslot = entryAlloca(newVal->getType());
        builder_.CreateStore(newVal, vslot);
        locals_[pd] = vslot;
    }
    if (setterExpr) lowerExpr(*setterExpr);
    else if (setterStmts) lowerScopedBlock(*setterStmts);
    currentThis_ = savedThis;
    locals_ = std::move(savedLocals);
    paramSlots_ = std::move(savedParams);
    return true;
}


// fix #508 / stdlib §4.4 / §4.5 / §17.15: compound assignment `arr[i] OP= v`
// on a HEAP-collection Array<T> / OwnedArray<T> indexer. The plain-assignment
// path (`arr[i] := v`) was already correct via lowerCollectionIndexerSet, and
// the explicit read-modify-write (`arr[i] := arr[i] OP v`) worked because the
// get and the set each drove their own receiver+index evaluation. The COMPOUND
// path (`arr[i] OP= v`) was broken because lowerStmt's else branch tried:
//   (1) lowerPropertyCompound  → returns false (no user-property on Array<T>)
//   (2) lowerIndexerCompound   → returns false (no user-indexer on Array<T>)
//   (3) lvalueAddr             → returns null (no LLVM ArrayType for bz_array_t*)
// After null from lvalueAddr, the branch returned without storing, silently
// losing the new value.
//
// Fix: evaluate receiver and index ONCE, call bz_array_get, apply OP, then call
// bz_array_set / bz_array_set_disposing — mirrors lowerCollectionIndexerGet for
// the read and lowerCollectionIndexerSet for the write, sharing a single recv+idx
// evaluation so the single-evaluation contract (§17.15) is satisfied.
//
// Dictionary compound-assign is NOT handled here: scalar-encoded dictionary
// values require round-tripping through the inttoptr encode/decode, which adds
// complexity with little payoff (dict compound-assign is unusual and can be
// written explicitly as `d[k] := d[k] OP v`). That gap is noted and left to a
// follow-up if needed.
bool Emitter::lowerCollectionIndexerCompound(const ast::AssignmentStmt& as) {
    if (!as.target || !as.value) return false;
    auto* ix = dynamic_cast<const ast::IndexExpr*>(as.target.get());
    if (!ix || !ix->target || ix->indices.empty() || !ix->indices[0].value)
        return false;
    auto* recvTy = ctx_.exprType(*ix->target);
    // Only handle Array<T> / OwnedArray<T> and list-interface receivers
    // (IReadOnlyList<T> / IList<T> / IOwnedList<T> are array-backed, §4.1-§4.3).
    // Dictionary compound-assign is left to the explicit form (see note above).
    bool isArr  = isCollectionTypeName(recvTy) || isListInterfaceName(recvTy);
    if (!isArr) return false;

    // --- Evaluate receiver and index ONCE (§17.15 single-evaluation). ---
    llvm::Value* recv = lowerExpr(*ix->target);
    if (!recv) return false;
    llvm::Value* idx = lowerExpr(*ix->indices[0].value);
    if (!idx) return false;
    idx = idx->getType()->isIntegerTy(64) ? idx
        : builder_.CreateSExt(idx, i64());

    // --- Bounds-check (§31.2: out-of-range raises IndexOutOfRangeException). ---
    auto* lenFn = declareC("bz_array_len", i64(), {ptrTy()});
    llvm::Value* len = builder_.CreateCall(lenFn, {recv});
    llvm::Value* oor = builder_.CreateICmpUGE(idx, len, "cic.oor");
    emitGuardThrow(oor, "IndexOutOfRangeException");

    // --- Determine element LLVM type and element-size (mirroring the get). ---
    int64_t esz = collectionElemSize(recvTy);
    llvm::Type* elemTy = i32();
    if (recvTy && !recvTy->genericArgs.empty() && recvTy->genericArgs[0])
        if (auto* et = lowerTypeRef(recvTy->genericArgs[0])) elemTy = et;

    // --- Read current value (bz_array_get). ---
    llvm::AllocaInst* getSlot = entryAlloca(elemTy);
    auto* getFn = declareC("bz_array_get", llvm::Type::getVoidTy(llvm_),
                           {ptrTy(), i64(), ptrTy(), i64()});
    builder_.CreateCall(getFn, {recv, idx, getSlot, builder_.getInt64(esz)});
    llvm::Value* cur = builder_.CreateLoad(elemTy, getSlot, "cic.cur");

    // --- Evaluate RHS and apply the compound operator. ---
    llvm::Value* rhs = lowerExpr(*as.value);
    if (!rhs) return false;
    std::string op(as.op);
    if (!op.empty() && op.back() == '=') op.pop_back();
    llvm::Value* newVal = emitCompoundOp(
        op, cur, rhs, elemTy->isFloatingPointTy(),
        isUnsignedTargetExpr(*as.target));
    if (!newVal) return false;

    // --- Lift nullable-value-typed result (mirrors lowerCollectionIndexerSet
    //     fix #455 / §4.5). ---
    {
        const sema::TypeRef* elemTyRef =
            (recvTy && !recvTy->genericArgs.empty())
            ? recvTy->genericArgs[0] : nullptr;
        if (elemTyRef && isNullableValueTy(elemTyRef)) {
            llvm::StructType* expected = nullableValueStructTy(elemTyRef);
            if (expected && newVal->getType() != expected) {
                if (newVal->getType()->isPointerTy())
                    newVal = nullableValueNull(elemTyRef);
                else
                    newVal = wrapNullableValue(elemTyRef, newVal);
            }
        }
    }

    // --- Write back (bz_array_set / bz_array_set_disposing for OwnedArray). ---
    llvm::AllocaInst* setSlot = entryAlloca(newVal->getType());
    builder_.CreateStore(newVal, setSlot);

    bool listIfaceOwning = false;
    if (isListInterfaceName(recvTy, &listIfaceOwning)) { /* already set */ }
    llvm::Function* elemDisp = owningCollectionElemDisposer(recvTy);
    if (!elemDisp && listIfaceOwning) elemDisp = elementDisposerThunk();
    if (elemDisp) {
        auto* setFn = declareC("bz_array_set_disposing",
                               llvm::Type::getVoidTy(llvm_),
                               {ptrTy(), i64(), ptrTy(), i64(), ptrTy()});
        builder_.CreateCall(setFn,
                            {recv, idx, setSlot, builder_.getInt64(esz), elemDisp});
    } else {
        auto* setFn = declareC("bz_array_set", llvm::Type::getVoidTy(llvm_),
                               {ptrTy(), i64(), ptrTy(), i64()});
        builder_.CreateCall(setFn, {recv, idx, setSlot, builder_.getInt64(esz)});
    }
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


// #481 / §12.1 / §12.5 / §23: emit (once, cached) a uniform-signature accessor
// FUNCTION for a virtual property, suitable for a vtable/itable slot. The body
// is the property's concrete accessor implementation on `cls` (arrow, block, or
// auto-property backing-field access). Getter: `T get(ptr this)`. Setter:
// `void set(ptr this, T value)`. An ABSTRACT accessor (no body) returns nullptr
// so its slot stays null on the abstract base and is filled by the override's
// function. Mirrors the body-emitting structure of ensurePropertyBlockGetter:
// it saves/restores the whole emitter state and may run during declareVtables
// (before ordinary method bodies are emitted).
llvm::Function* Emitter::ensurePropertyAccessorFn(const sema::TypeSymbol* cls,
                                                  const sema::PropertySymbol* ps,
                                                  bool isSet) {
    auto key = std::make_pair(ps, isSet);
    if (auto it = propAccessorFns_.find(key); it != propAccessorFns_.end())
        return it->second;

    auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl);
    if (!pd) return nullptr;

    llvm::Type* valTy = lowerType(pd->type.get());
    if (!valTy) valTy = i32();
    // #510 / §7 / §12.1 / §13: resolve the property's sema TypeRef so we can
    // detect nullable value types (int?, bool?, enum?) and wrap the accessor
    // body's return value into the tagged { i8, T } struct when lowerExpr
    // returns the raw inner value (Bug3 of #510). Null when pd->type is absent.
    const sema::TypeRef* retTyRef = pd->type ? ctx_.resolvedType(*pd->type)
                                              : nullptr;

    // Determine the concrete accessor body. Returns null fn (no slot fill) for
    // an abstract accessor; the override supplies the real one.
    const ast::Expr* getterExpr = nullptr;   // arrow getter
    const ast::PropertyAccessor* blockGet = nullptr;
    const ast::PropertyAccessor* setAcc = nullptr;
    bool isAuto = (pd->bodyKind == ast::PropertyBodyKind::Auto);
    if (!isSet) {
        if (pd->bodyKind == ast::PropertyBodyKind::Expression)
            getterExpr = pd->exprBody.get();
        else if (pd->bodyKind == ast::PropertyBodyKind::Block) {
            for (const auto& a : pd->accessors) {
                if (a.isSet) continue;
                if (a.bodyKind == ast::PropertyAccessorBodyKind::Block)
                    blockGet = &a;
                else if (a.bodyKind == ast::PropertyAccessorBodyKind::Expression)
                    getterExpr = a.exprBody.get();
                break;
            }
        }
        // Abstract (no get body) and not auto → no implementation here.
        if (!isAuto && !getterExpr && !blockGet &&
            pd->bodyKind != ast::PropertyBodyKind::Block)
            return nullptr;
        if (pd->bodyKind == ast::PropertyBodyKind::Block && !getterExpr &&
            !blockGet)
            return nullptr;  // abstract/interface block with no get body
    } else {
        if (pd->bodyKind == ast::PropertyBodyKind::Block) {
            for (const auto& a : pd->accessors) {
                if (a.isSet &&
                    a.bodyKind != ast::PropertyAccessorBodyKind::Abstract) {
                    setAcc = &a;
                    break;
                }
            }
            if (!setAcc) return nullptr;  // abstract/interface setter, no body
        } else if (!isAuto) {
            return nullptr;  // expression-bodied property has no setter
        }
    }

    // Signature: getter (this)->T, setter (this, value:T)->void.
    std::vector<llvm::Type*> paramTys{ptrTy()};
    if (isSet) paramTys.push_back(valTy);
    auto* fnTy = llvm::FunctionType::get(
        isSet ? llvm::Type::getVoidTy(llvm_) : valTy, paramTys, false);
    std::string mangled = "bz$" + std::string(cls->name) + "$" +
                          std::string(ps->name) +
                          (isSet ? "$propset" : "$propget");
    auto* fn = llvm::Function::Create(
        fnTy, llvm::GlobalValue::InternalLinkage, mangled, *module_);
    propAccessorFns_[key] = fn;

    auto* entryBB = llvm::BasicBlock::Create(llvm_, "entry", fn);

    // --- Save entire emitter state (mirrors ensurePropertyBlockGetter). ---
    auto* savedFn        = currentFn_;
    auto  savedBB        = builder_.GetInsertBlock();
    auto  savedBBOffset  = savedBB ? builder_.GetInsertPoint()
                                   : decltype(savedBB->begin()){};
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

    currentFn_          = fn;
    builder_.SetInsertPoint(entryBB);
    auto argIt = fn->args().begin();
    currentThis_        = &*argIt;
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

    // §28.2 v0.134 / issue #601: checked disposal mode guard for property
    // accessors. Class properties only (enum properties are NOT disposable,
    // and records have no disposed flag). Per §28.2: "a property or indexer
    // accessor invocation" on a disposed instance raises DisposedAccessException.
    if (checkedDisposal_ && cls && cls->kind == ast::TypeDeclKind::Class) {
        llvm::StructType* st = classStruct(cls);
        unsigned fi = disposedFlagIndex(cls);
        llvm::Value* flagAddr =
            builder_.CreateStructGEP(st, currentThis_, fi, "cd.flag");
        llvm::Value* disposed = builder_.CreateICmpNE(
            builder_.CreateLoad(disposedFlagTy(), flagAddr),
            llvm::ConstantInt::get(disposedFlagTy(), 0), "cd.disposed");
        emitGuardThrow(disposed, "DisposedAccessException");
    }

    // #574 / §10.4 / §19.2 (CG30): if this property belongs to an enum, set
    // currentEnumMethod_ so that enumTypeOf("this") resolves correctly inside
    // a `case this` body — sema does not attach an exprType to `this` inside
    // enum members, so the same workaround used by enum instance methods is
    // needed here too.
    const sema::TypeSymbol* savedEnumMethod2 = currentEnumMethod_;
    if (cls && cls->kind == ast::TypeDeclKind::Enum)
        currentEnumMethod_ = cls;

    if (!isSet) {
        // --- Getter body. ---
        if (isAuto) {
            // §23.1: auto-property backing field read.
            llvm::StructType* st = classStruct(cls);
            auto idxIt = fieldIndex_.find(ps);
            if (idxIt != fieldIndex_.end()) {
                llvm::Value* slot = builder_.CreateStructGEP(
                    st, currentThis_, idxIt->second);
                builder_.CreateRet(builder_.CreateLoad(valTy, slot));
            } else {
                builder_.CreateRet(llvm::Constant::getNullValue(valTy));
            }
        } else if (getterExpr) {
            llvm::Value* v = lowerExpr(*getterExpr);
            if (!builder_.GetInsertBlock()->getTerminator()) {
                // #510 / §7 / §12.1 / §13 (Bug3): when the property return
                // type is a nullable value type ({ i8, T } struct) but
                // lowerExpr returned the raw inner value (e.g. i1 for `true`,
                // i32 for an integer literal, or ptr null for the `null`
                // literal), wrap it into the struct before returning. Without
                // this the ret instruction type-mismatches the function's
                // declared return type and the IR verifier rejects the module.
                //
                // The mismatch arises because lowerExpr evaluates the body
                // expression in its "natural" representation (not nullable),
                // while the function's return type is the nullable struct
                // derived from the property's declared type annotation. The
                // wrap mirrors the same coercion that field initializers and
                // argument-passing do for nullable value fields (§7).
                if (v && v->getType() != valTy &&
                    retTyRef && isNullableValueTy(retTyRef)) {
                    if (llvm::isa<llvm::ConstantPointerNull>(v))
                        // `null` literal from lowerExpr → tagged-null struct.
                        v = nullableValueNull(retTyRef);
                    else
                        // Plain T value → { i8 1, T v } (has-value = true).
                        v = wrapNullableValue(retTyRef, v);
                }
                builder_.CreateRet(v ? v
                                     : llvm::Constant::getNullValue(valTy));
            }
        } else if (blockGet) {
            lowerScopedBlock(blockGet->stmts);
            if (!builder_.GetInsertBlock()->getTerminator())
                emitImplicitReturn(fn);
        }
    } else {
        // --- Setter body: bind the implicit `value` param (anchored on pd). ---
        llvm::Argument& valArg = *std::next(fn->args().begin());
        llvm::AllocaInst* vslot = entryAlloca(valArg.getType());
        builder_.CreateStore(&valArg, vslot);
        locals_[pd] = vslot;
        if (isAuto) {
            // §23.1: auto-property backing field write.
            llvm::StructType* st = classStruct(cls);
            auto idxIt = fieldIndex_.find(ps);
            if (idxIt != fieldIndex_.end()) {
                llvm::Value* slot = builder_.CreateStructGEP(
                    st, currentThis_, idxIt->second);
                builder_.CreateStore(&valArg, slot);
            }
        } else if (setAcc) {
            if (setAcc->bodyKind == ast::PropertyAccessorBodyKind::Expression &&
                setAcc->exprBody)
                lowerExpr(*setAcc->exprBody);
            else
                lowerScopedBlock(setAcc->stmts);
        }
        if (!builder_.GetInsertBlock()->getTerminator())
            builder_.CreateRetVoid();
    }

    // --- Restore state. ---
    currentEnumMethod_   = savedEnumMethod2;
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


// §23.4 / §22.9: emit a standalone function for a BLOCK-form indexer getter
// (`property [i: …]: T get => do…end;`). Mirrors ensurePropertyBlockGetter but
// the signature carries the index parameters after `this`, and the body binds
// each index param to a spilled stack slot so the getter body's identifier
// reads resolve them. Emitting the getter as its own function (rather than
// inlining the block into the caller) is required for any block getter — the
// `return` inside the body must terminate only the getter, not the enclosing
// method — and it lets a record (aggregate) result flow back by value through
// the function's declared return type (§9.4), the same path the block-form
// property getter and a record-returning method use.
llvm::Function* Emitter::ensureIndexerBlockGetter(
    const ast::PropertyDecl* pd, const ast::PropertyAccessor* acc,
    const sema::PropertySymbol* ps) {
    auto it = propBlockGetterFns_.find(pd);
    if (it != propBlockGetterFns_.end()) return it->second;

    llvm::Type* retTy = lowerType(pd->type.get());
    if (!retTy) retTy = i32();

    // Signature: (this, idx0, idx1, …). The index params take their declared
    // (lowered) types; an absent/unlowerable type falls back to i32.
    std::vector<llvm::Type*> paramTys;
    paramTys.push_back(ptrTy());                     // `this`
    for (const auto& p : pd->indexerParams) {
        llvm::Type* pt = p.type ? lowerType(p.type.get()) : nullptr;
        paramTys.push_back(pt ? pt : i32());
    }

    auto* fnTy = llvm::FunctionType::get(retTy, paramTys, false);
    auto* fn = llvm::Function::Create(
        fnTy, llvm::GlobalValue::InternalLinkage, "bz$indexer$get", *module_);
    propBlockGetterFns_[pd] = fn;

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

    // arg 0 is `this`; args 1.. are the index params. Spill each index param to
    // a stack slot so localSlot() resolves the body's identifier reads.
    auto argIt = fn->args().begin();
    currentThis_ = &*argIt;
    ++argIt;
    for (std::size_t k = 0; k < ps->indexerParams.size() &&
         argIt != fn->args().end(); ++k, ++argIt) {
        llvm::Argument& a = *argIt;
        llvm::AllocaInst* slot = entryAlloca(a.getType());
        builder_.CreateStore(&a, slot);
        paramSlots_[ps->indexerParams[k]] = slot;
    }

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
    // per §23.3 / §13 / #476: a STATIC property has no receiver (`this`).
    // Mirrors the static-getter path in LlvmBackend_Exprs.cpp (the
    // "isStaticProp" branch). For static properties, skip receiver resolution
    // entirely — both bare `X := v` and qualified `C.X := v` just bind
    // `value` and execute the setter body in the current static context.
    bool isStaticProp = std::any_of(ps->modifiers.begin(), ps->modifiers.end(),
                                    [](const auto& m){ return m == "static"; });
    if (isStaticProp) {
        llvm::AllocaInst* slot = entryAlloca(v->getType());
        builder_.CreateStore(v, slot);
        locals_[pd] = slot;
        if (setter->bodyKind == ast::PropertyAccessorBodyKind::Expression &&
            setter->exprBody)
            lowerExpr(*setter->exprBody);
        else
            lowerScopedBlock(setter->stmts);
        return true;
    }
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


// §29.2 (#437): generate a type-specific, vtable-backed array enumerator.
// Layout:  { ptr vtable, ptr arr, i64 idx, i64 esz }
// Vtable:  [0]=td(self), [1]=Dispose, [2]=MoveNext→i1, [3]=Current→T
// The Current function returns the element VALUE directly (not a void*
// pointer), matching the iterator state machine's vtable protocol so that
// lowerIEnumerableBuiltinFor's vtable dispatch can be used uniformly for
// both Array<T> and iterator-method IEnumerable<T> sources.
llvm::Value* Emitter::wrapArrayAsIEnumerable(llvm::Value* arr,
                                             const sema::TypeRef* iEnumTy) {
    // Determine element type and size.
    // per §9 (bug D): a record element type lowers to an LLVM StructType whose
    // allocation size may exceed 8 bytes; use DataLayout for structs so the
    // esz stored in the enumerator frame matches the stride used by
    // bz_array_ptr_at (same fix as collectionElemSize in Calls.cpp).
    llvm::Type* elemTy = i32();
    int64_t esz = 8;
    if (iEnumTy && !iEnumTy->genericArgs.empty() && iEnumTy->genericArgs[0]) {
        elemTy = lowerTypeRef(iEnumTy->genericArgs[0]);
        if (elemTy->isIntegerTy(32) || elemTy->isFloatTy()) esz = 4;
        else if (elemTy->isIntegerTy(1) || elemTy->isIntegerTy(8)) esz = 1;
        else if (elemTy->isIntegerTy(16)) esz = 2;
        else if (elemTy->isStructTy())
            esz = (int64_t)module_->getDataLayout().getTypeAllocSize(elemTy);
        else esz = 8;
    }

    // Enumerator struct: { ptr vtable, ptr arr, i64 idx, i64 esz }
    // We use a unique struct name keyed on the element type to allow caching.
    std::string elemName = elemTy->isIntegerTy() ?
        ("i" + std::to_string(elemTy->getIntegerBitWidth())) :
        (elemTy->isPointerTy() ? "ptr" : "f");
    std::string stName = "arrenum." + elemName;
    auto* stTy = llvm::StructType::getTypeByName(llvm_, stName);
    if (!stTy) {
        stTy = llvm::StructType::create(llvm_,
            {ptrTy(), ptrTy(), i64(), i64()}, stName);
    }

    // Generate (or reuse) the three method functions.
    std::string prefix = "bz$arrenum$" + elemName + "$";
    auto getOrCreate = [&](const std::string& name, llvm::FunctionType* ft,
                           std::function<void(llvm::Function*)> fill)
        -> llvm::Function* {
        if (auto* f = module_->getFunction(name)) return f;
        auto* f = llvm::Function::Create(ft, llvm::Function::InternalLinkage,
                                         name, module_.get());
        fill(f);
        return f;
    };

    // Dispose: free(self)
    auto* dispFn = getOrCreate(prefix + "Dispose",
        llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_), {ptrTy()}, false),
        [&](llvm::Function* f) {
            auto* bb = llvm::BasicBlock::Create(llvm_, "entry", f);
            llvm::IRBuilder<> b(bb);
            auto* freeFn2 = module_->getOrInsertFunction(
                "free", llvm::FunctionType::get(
                    llvm::Type::getVoidTy(llvm_), {ptrTy()}, false)).getCallee();
            b.CreateCall(llvm::FunctionType::get(
                llvm::Type::getVoidTy(llvm_), {ptrTy()}, false),
                freeFn2, {f->arg_begin()});
            b.CreateRetVoid();
        });

    // MoveNext: e->idx++; return idx < bz_array_len(arr)
    auto* mnFn = getOrCreate(prefix + "MoveNext",
        llvm::FunctionType::get(i1(), {ptrTy()}, false),
        [&](llvm::Function* f) {
            auto* bb = llvm::BasicBlock::Create(llvm_, "entry", f);
            llvm::IRBuilder<> b(bb);
            llvm::Value* self = f->arg_begin();
            llvm::Value* idxF = b.CreateStructGEP(stTy, self, 2);
            llvm::Value* idx = b.CreateLoad(i64(), idxF);
            llvm::Value* next = b.CreateAdd(idx, llvm::ConstantInt::get(i64(), 1));
            b.CreateStore(next, idxF);
            llvm::Value* arrF = b.CreateStructGEP(stTy, self, 1);
            llvm::Value* arrPtr = b.CreateLoad(ptrTy(), arrF);
            auto* lenFT = llvm::FunctionType::get(i64(), {ptrTy()}, false);
            auto* lenFn2 = module_->getOrInsertFunction("bz_array_len", lenFT).getCallee();
            llvm::Value* len = b.CreateCall(lenFT, lenFn2, {arrPtr});
            b.CreateRet(b.CreateICmpSLT(next, len));
        });

    // Current: load element at idx and return by value
    auto* curFn = getOrCreate(prefix + "Current",
        llvm::FunctionType::get(elemTy, {ptrTy()}, false),
        [&](llvm::Function* f) {
            auto* bb = llvm::BasicBlock::Create(llvm_, "entry", f);
            llvm::IRBuilder<> b(bb);
            llvm::Value* self = f->arg_begin();
            llvm::Value* arrF = b.CreateStructGEP(stTy, self, 1);
            llvm::Value* arrPtr = b.CreateLoad(ptrTy(), arrF);
            llvm::Value* idxF = b.CreateStructGEP(stTy, self, 2);
            llvm::Value* idx = b.CreateLoad(i64(), idxF);
            auto* ptrAtFT = llvm::FunctionType::get(ptrTy(), {ptrTy(), i64(), i64()}, false);
            auto* ptrAtFn = module_->getOrInsertFunction("bz_array_ptr_at", ptrAtFT).getCallee();
            llvm::Value* ep = b.CreateCall(ptrAtFT, ptrAtFn,
                {arrPtr, idx, llvm::ConstantInt::get(i64(), esz)});
            b.CreateRet(b.CreateLoad(elemTy, ep));
        });

    // Build the vtable (4-pointer constant).
    std::string vtName = prefix + "vtable";
    auto* vtGV = module_->getGlobalVariable(vtName);
    if (!vtGV) {
        auto* vtTy = llvm::ArrayType::get(ptrTy(), 4);
        std::vector<llvm::Constant*> elems = {
            llvm::ConstantPointerNull::get(ptrTy()), // slot 0: self-ptr (filled below)
            dispFn,
            mnFn,
            curFn
        };
        vtGV = new llvm::GlobalVariable(*module_, vtTy, /*isConst=*/true,
            llvm::GlobalValue::InternalLinkage,
            llvm::ConstantArray::get(vtTy, elems), vtName);
        // Slot 0: the vtable's own address (self-referential td sentinel).
        elems[0] = llvm::ConstantExpr::getPointerCast(vtGV, ptrTy());
        vtGV->setInitializer(llvm::ConstantArray::get(vtTy, elems));
    }

    // Allocate and fill the enumerator struct.
    uint64_t stSz = module_->getDataLayout().getTypeAllocSize(stTy);
    auto* callocFn2 = declareC("calloc", ptrTy(), {i64(), i64()});
    llvm::Value* en = builder_.CreateCall(callocFn2,
        {builder_.getInt64(1), builder_.getInt64(stSz)}, "arrenum");
    // [0] vtable
    builder_.CreateStore(vtGV,
        builder_.CreateStructGEP(stTy, en, 0));
    // [1] arr
    builder_.CreateStore(arr,
        builder_.CreateStructGEP(stTy, en, 1));
    // [2] idx = -1 (calloc zeroed it, but -1 means "before first")
    builder_.CreateStore(builder_.getInt64(-1),
        builder_.CreateStructGEP(stTy, en, 2));
    // [3] esz
    builder_.CreateStore(builder_.getInt64(esz),
        builder_.CreateStructGEP(stTy, en, 3));
    return en;
}


// §29.2 (#552): generate (or reuse) a standalone thunk function for an
// expression-form property getter. The thunk signature is `retTy(ptr self)`,
// matching the block-getter protocol of ensurePropertyBlockGetter.  We save
// and restore the full emitter context exactly as ensurePropertyBlockGetter
// does; lowerExpr then runs inside the thunk's basic block with the thunk
// parameter as currentThis_.
llvm::Function* Emitter::ensureExprGetterThunk(const sema::PropertySymbol* ps,
                                               const ast::Expr* getterExpr,
                                               llvm::Type* retTy) {
    auto it = exprGetterThunks_.find(ps);
    if (it != exprGetterThunks_.end()) return it->second;

    if (!retTy) retTy = i32();
    auto* fnTy = llvm::FunctionType::get(retTy, {ptrTy()}, false);
    auto* fn = llvm::Function::Create(
        fnTy, llvm::GlobalValue::InternalLinkage,
        "bz$exprget$" + std::string(ps->name), *module_);
    exprGetterThunks_[ps] = fn;

    auto* entryBB = llvm::BasicBlock::Create(llvm_, "entry", fn);

    // Save entire emitter state (mirrors ensurePropertyBlockGetter).
    auto* savedFn       = currentFn_;
    auto  savedBB       = builder_.GetInsertBlock();
    auto  savedBBOffset = savedBB ? builder_.GetInsertPoint()
                                  : decltype(savedBB->begin()){};
    auto* savedThis     = currentThis_;
    auto  savedLocals   = locals_;
    auto  savedParams   = paramSlots_;
    auto  savedRef      = refParams_;
    auto  savedDispose  = disposeStack_;
    auto  savedLoop     = loopStack_;
    auto* savedUnwind   = unwindTarget_;
    auto* savedProp     = propagateBB_;
    auto* savedCaught   = currentCaughtExc_;
    auto  savedUD       = unwindScopeDepth_;
    auto  savedDiv      = diverged_;
    auto  savedGSubst   = genericSubst_;

    currentFn_         = fn;
    builder_.SetInsertPoint(entryBB);
    currentThis_       = &*fn->args().begin();
    locals_.clear(); paramSlots_.clear(); refParams_.clear();
    disposeStack_.clear(); loopStack_.clear(); finallyStack_.clear();
    unwindTarget_      = nullptr; propagateBB_ = nullptr;
    currentCaughtExc_  = nullptr; unwindScopeDepth_ = 0;
    diverged_          = false; genericSubst_.clear();

    llvm::Value* val = lowerExpr(*getterExpr);
    if (!val) val = llvm::Constant::getNullValue(retTy);
    if (val->getType() != retTy && retTy->isIntegerTy() &&
        val->getType()->isIntegerTy())
        val = builder_.CreateTrunc(val, retTy);
    builder_.CreateRet(val);

    // Restore state.
    currentFn_         = savedFn;
    if (savedBB) builder_.SetInsertPoint(savedBB, savedBBOffset);
    currentThis_       = savedThis;
    locals_            = std::move(savedLocals);
    paramSlots_        = std::move(savedParams);
    refParams_         = std::move(savedRef);
    disposeStack_      = std::move(savedDispose);
    loopStack_         = std::move(savedLoop);
    unwindTarget_      = savedUnwind;
    propagateBB_       = savedProp;
    currentCaughtExc_  = savedCaught;
    unwindScopeDepth_  = savedUD;
    diverged_          = savedDiv;
    genericSubst_      = std::move(savedGSubst);

    return fn;
}


// §29.2 (#552): wrap a user class instance (implementing IEnumerable<T>) in a
// type-specific, vtable-backed enumerator whose MoveNext/Current/Dispose
// delegate to the enumerator class returned by GetEnumerator().  Layout:
//   struct { ptr vtable, ptr enumerator }
//   vtable: [0]=td(self), [1]=Dispose, [2]=MoveNext→i1, [3]=Current→T
// Eagerly calls GetEnumerator() on the source object at the wrapping site.
// Returns src unchanged if GetEnumerator() or the enumerator class cannot be
// resolved; the caller falls back to the raw pointer (which will misfire at
// runtime, but that's no worse than before this fix).
llvm::Value* Emitter::wrapUserClassAsIEnumerable(llvm::Value* src,
                                                  const sema::TypeSymbol* srcTs,
                                                  const sema::TypeRef* iEnumTy) {
    // 1. Find GetEnumerator on the source class.
    sema::MethodSymbol* getEnumSym = findMethodByName(srcTs, "GetEnumerator");
    if (!getEnumSym) return src;

    // 2. Find the concrete enumerator TypeSymbol (same logic as lowerIEnumerableFor).
    auto* getEnumDecl = dynamic_cast<const ast::MethodDecl*>(getEnumSym->decl);
    if (!getEnumDecl || !getEnumDecl->returnType) return src;
    auto* enumTyRef = ctx_.resolvedType(*getEnumDecl->returnType);
    if (!enumTyRef) return src;
    auto* enumTs = (enumTyRef->kind == sema::TypeRef::Kind::Named)
                   ? enumTyRef->declaration : nullptr;
    if (!enumTs) {
        // GetEnumerator() declares return type as builtin IEnumerator<T>.
        // Scan all compiled TypeSymbols for a class with MoveNext + Current.
        for (const auto& sym : ctx_.symbols()) {
            auto* cand = dynamic_cast<const sema::TypeSymbol*>(sym.get());
            if (!cand || cand->kind != ast::TypeDeclKind::Class) continue;
            if (cand == srcTs) continue;
            bool hasMN = false, hasCur = false;
            for (auto* m : cand->members) {
                auto* ms = dynamic_cast<sema::MethodSymbol*>(m);
                if (!ms || !methodFns_.count(ms)) continue;
                if (ms->name == "MoveNext")  hasMN  = true;
                if (ms->name == "Current")   hasCur = true;
            }
            if (!hasCur) {
                for (auto* m : cand->members) {
                    auto* ps = dynamic_cast<sema::PropertySymbol*>(m);
                    if (ps && ps->name == "Current") { hasCur = true; break; }
                }
            }
            if (hasMN && hasCur) { enumTs = const_cast<sema::TypeSymbol*>(cand); break; }
        }
    }
    if (!enumTs) return src;

    // 3. Find MoveNext, Current, and Dispose on the enumerator class.
    sema::MethodSymbol* moveNextSym = findMethodByName(enumTs, "MoveNext");
    if (!moveNextSym) return src;
    auto mnIt = methodFns_.find(moveNextSym);
    if (mnIt == methodFns_.end()) return src;
    llvm::Function* moveNextFn = mnIt->second;

    sema::MethodSymbol* currentMethodSym = findMethodByName(enumTs, "Current");
    sema::PropertySymbol* currentPropSym = nullptr;
    if (!currentMethodSym)
        currentPropSym = classPropertyByName(enumTs, "Current");

    // 4. Resolve element type.
    llvm::Type* elemTy = ptrTy();
    if (iEnumTy && !iEnumTy->genericArgs.empty() && iEnumTy->genericArgs[0])
        elemTy = lowerTypeRef(iEnumTy->genericArgs[0]);

    // 5. Resolve the Current function pointer.
    llvm::Function* currentFn = nullptr;
    if (currentMethodSym) {
        auto curIt = methodFns_.find(currentMethodSym);
        if (curIt != methodFns_.end()) currentFn = curIt->second;
    } else if (currentPropSym) {
        auto* pd = dynamic_cast<const ast::PropertyDecl*>(currentPropSym->decl);
        if (pd) {
            if (const ast::PropertyAccessor* acc = blockGetterOf(currentPropSym)) {
                currentFn = ensurePropertyBlockGetter(pd, acc);
            } else if (const ast::Expr* ge = computedGetterOf(currentPropSym)) {
                llvm::Type* propRetTy = pd->type ? lowerType(pd->type.get()) : elemTy;
                if (!propRetTy) propRetTy = elemTy;
                currentFn = ensureExprGetterThunk(currentPropSym, ge, propRetTy);
            }
        }
    }
    if (!currentFn) return src;

    // 6. Resolve the Dispose function pointer (optional; use no-op if absent).
    llvm::Function* enumDisposeFn = nullptr;
    {
        auto dIt = disposerFns_.find(enumTs);
        if (dIt != disposerFns_.end()) enumDisposeFn = dIt->second;
    }

    // 7. Build type-specific wrapper functions keyed on the enumerator class name.
    std::string key = std::string(enumTs->name);
    std::string stName = "userenumenum." + key;
    auto* stTy = llvm::StructType::getTypeByName(llvm_, stName);
    if (!stTy)
        stTy = llvm::StructType::create(llvm_, {ptrTy(), ptrTy()}, stName);

    std::string prefix = "bz$userenumenum$" + key + "$";
    auto getOrCreate = [&](const std::string& name, llvm::FunctionType* ft,
                           std::function<void(llvm::Function*)> fill)
        -> llvm::Function* {
        if (auto* f = module_->getFunction(name)) return f;
        auto* f = llvm::Function::Create(ft, llvm::Function::InternalLinkage,
                                         name, module_.get());
        fill(f);
        return f;
    };

    // Dispose: call enum.Dispose() then free(self).
    llvm::Function* wrapDisposeFn = getOrCreate(prefix + "Dispose",
        llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_), {ptrTy()}, false),
        [&](llvm::Function* f) {
            auto* bb = llvm::BasicBlock::Create(llvm_, "entry", f);
            llvm::IRBuilder<> b(bb);
            llvm::Value* self = f->arg_begin();
            llvm::Value* enumF = b.CreateStructGEP(stTy, self, 1);
            llvm::Value* enumPtr = b.CreateLoad(ptrTy(), enumF);
            if (enumDisposeFn) {
                b.CreateCall(enumDisposeFn->getFunctionType(), enumDisposeFn, {enumPtr});
            }
            auto* freeFnCallee = module_->getOrInsertFunction(
                "free", llvm::FunctionType::get(
                    llvm::Type::getVoidTy(llvm_), {ptrTy()}, false)).getCallee();
            b.CreateCall(llvm::FunctionType::get(
                llvm::Type::getVoidTy(llvm_), {ptrTy()}, false),
                freeFnCallee, {self});
            b.CreateRetVoid();
        });

    // MoveNext: call enum.MoveNext() and return bool.
    llvm::Function* wrapMoveNextFn = getOrCreate(prefix + "MoveNext",
        llvm::FunctionType::get(i1(), {ptrTy()}, false),
        [&](llvm::Function* f) {
            auto* bb = llvm::BasicBlock::Create(llvm_, "entry", f);
            llvm::IRBuilder<> b(bb);
            llvm::Value* self = f->arg_begin();
            llvm::Value* enumF = b.CreateStructGEP(stTy, self, 1);
            llvm::Value* enumPtr = b.CreateLoad(ptrTy(), enumF);
            llvm::Value* r = b.CreateCall(moveNextFn->getFunctionType(),
                                          moveNextFn, {enumPtr});
            // Ensure i1 return regardless of MoveNext's declared LLVM type.
            if (r->getType() != b.getInt1Ty())
                r = b.CreateTrunc(r, b.getInt1Ty());
            b.CreateRet(r);
        });

    // Current: call enum.Current getter and return T.
    llvm::Function* wrapCurrentFn = getOrCreate(prefix + "Current",
        llvm::FunctionType::get(elemTy, {ptrTy()}, false),
        [&](llvm::Function* f) {
            auto* bb = llvm::BasicBlock::Create(llvm_, "entry", f);
            llvm::IRBuilder<> b(bb);
            llvm::Value* self = f->arg_begin();
            llvm::Value* enumF = b.CreateStructGEP(stTy, self, 1);
            llvm::Value* enumPtr = b.CreateLoad(ptrTy(), enumF);
            llvm::Value* r = b.CreateCall(currentFn->getFunctionType(),
                                          currentFn, {enumPtr});
            // Coerce to elemTy if needed (e.g. i32 → i1 for bool).
            if (r->getType() != elemTy && elemTy->isIntegerTy() &&
                r->getType()->isIntegerTy())
                r = b.CreateTrunc(r, elemTy);
            b.CreateRet(r);
        });

    // 8. Build vtable global.
    std::string vtName = prefix + "vtable";
    auto* vtGV = module_->getGlobalVariable(vtName);
    if (!vtGV) {
        auto* vtTy = llvm::ArrayType::get(ptrTy(), 4);
        std::vector<llvm::Constant*> elems = {
            llvm::ConstantPointerNull::get(ptrTy()),  // slot 0: filled below
            wrapDisposeFn,
            wrapMoveNextFn,
            wrapCurrentFn
        };
        vtGV = new llvm::GlobalVariable(*module_, vtTy, /*isConst=*/true,
            llvm::GlobalValue::InternalLinkage,
            llvm::ConstantArray::get(vtTy, elems), vtName);
        elems[0] = llvm::ConstantExpr::getPointerCast(vtGV, ptrTy());
        vtGV->setInitializer(llvm::ConstantArray::get(vtTy, elems));
    }

    // 9. Eagerly call GetEnumerator() on the source to get the enumerator ptr.
    std::vector<llvm::Value*> geArgs = {src};
    llvm::Value* enumPtr = emitMethodCall(getEnumSym, geArgs, /*baseQualified=*/false);
    if (!enumPtr) return src;

    // 10. Allocate wrapper struct and fill fields.
    uint64_t stSz = module_->getDataLayout().getTypeAllocSize(stTy);
    auto* callocFn2 = declareC("calloc", ptrTy(), {i64(), i64()});
    llvm::Value* wrap = builder_.CreateCall(callocFn2,
        {builder_.getInt64(1), builder_.getInt64(stSz)}, "userenumenum");
    builder_.CreateStore(vtGV, builder_.CreateStructGEP(stTy, wrap, 0));
    builder_.CreateStore(enumPtr, builder_.CreateStructGEP(stTy, wrap, 1));
    return wrap;
}


bool Emitter::buildOrderedArgs(const std::vector<ast::Param>& params,
                      const std::vector<ast::CallArgument>& args,
                      std::vector<llvm::Value*>& out,
                      std::vector<const ast::Expr*>* outExprs) {
    std::vector<llvm::Value*> ordered(params.size(), nullptr);
    // issue #421 / #410: parallel source expression per parameter position, so
    // the caller can recover each argument's signedness for numeric widening.
    std::vector<const ast::Expr*> orderedExprs(params.size(), nullptr);
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
            // §29.2 (#437): when an Array<T> is passed to a formal parameter
            // typed as `IEnumerable<T>` (the pure iteration-only interface),
            // wrap the raw bz_array_t* in a type-specific vtable-backed
            // enumerator whose MoveNext/Current have the SAME protocol as an
            // iterator state machine (Current returns elemTy directly, not a
            // void* pointer).  Only `IEnumerable<T>` is wrapped — the richer
            // interfaces (IReadOnlyList, ICollection, etc.) also support
            // indexer access, which the wrapped enumerator does NOT provide,
            // so wrapping them would break callee indexer operations.
            // Without wrapping, a callee iterating an IEnumerable<T> parameter
            // with vtable dispatch would interpret the bz_array_t layout as a
            // vtable-backed object → crash.
            if (formalTy && formalTy->kind == sema::TypeRef::Kind::Named &&
                !formalTy->declaration &&
                formalTy->astNode && formalTy->astNode->name &&
                !formalTy->astNode->name->parts.empty() &&
                formalTy->astNode->name->parts[0] == "IEnumerable") {
                // Check if the ACTUAL argument type is Array<T>.
                sema::TypeRef* actualTy = ctx_.exprType(*a.value);
                bool actualIsArray = actualTy &&
                    actualTy->kind == sema::TypeRef::Kind::Named &&
                    actualTy->astNode && actualTy->astNode->name &&
                    !actualTy->astNode->name->parts.empty() &&
                    actualTy->astNode->name->parts[0] == "Array";
                if (actualIsArray) {
                    v = wrapArrayAsIEnumerable(v, formalTy);
                }
                // §29.2 (#552): when a user class (implementing IEnumerable<T>)
                // is passed to an IEnumerable<T> parameter, wrap it in a thin
                // sm-compatible enumerator that eagerly calls GetEnumerator() and
                // delegates MoveNext/Current/Dispose to the concrete enumerator
                // class. Without this the callee's vtable-dispatch path would
                // call vtable[2] (Dispose on the class) instead of MoveNext.
                else if (actualTy &&
                         actualTy->kind == sema::TypeRef::Kind::Named &&
                         actualTy->declaration) {
                    v = wrapUserClassAsIEnumerable(v, actualTy->declaration, formalTy);
                }
            }
        }
        ordered[idx] = v;
        orderedExprs[idx] = a.value.get();
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
            // A defaulted parameter has no call-site argument expression;
            // record the default's own expression so its signedness is honored.
            orderedExprs[j] = dv;
        }
        out.push_back(ordered[j]);
        if (outExprs) outExprs->push_back(orderedExprs[j]);
    }
    return true;
}


}  // namespace bezel::codegen
