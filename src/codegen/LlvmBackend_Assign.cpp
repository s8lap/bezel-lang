#include "codegen/LlvmBackend_impl.hpp"

namespace bezel::codegen {

namespace {
// §23.4 (#774): exact structural match between an indexer's first parameter
// type and the index argument's type — by primitive for value/string types and
// by declaration for named types. Used to pick the right overload; the
// checkTypeOverloads distinctness check (sema) guarantees at most one indexer
// matches any concrete argument type exactly.
bool indexerArgMatches(const sema::TypeRef* p, const sema::TypeRef* a) {
    if (!p || !a || p->kind != a->kind) return false;
    if (p->kind == sema::TypeRef::Kind::Primitive)
        return p->primitive == a->primitive;
    if (p->kind == sema::TypeRef::Kind::Named)
        return p->declaration == a->declaration;
    return false;
}
}  // namespace

const sema::PropertySymbol* Emitter::findIndexer(
    const sema::TypeSymbol* ts, const sema::TypeRef* argTy) const {
    const sema::PropertySymbol* first = nullptr;
    for (const sema::Symbol* m : ts->members) {
        auto* ps = dynamic_cast<const sema::PropertySymbol*>(m);
        if (!ps || !ps->isIndexer) continue;
        if (!first) first = ps;
        if (!argTy) continue;  // no argument type → first indexer (legacy)
        auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl);
        if (!pd || pd->indexerParams.empty() || !pd->indexerParams[0].type)
            continue;
        if (indexerArgMatches(ctx_.resolvedType(*pd->indexerParams[0].type),
                              argTy))
            return ps;  // §23.4: the overload whose param type matches the arg
    }
    return first;  // single indexer, or an argument matching no overload
                   // exactly (e.g. a widening index) → the first indexer
}

// §23.4 (#1172): select the indexer OVERLOAD for `ix` by its FULL parameter
// list (arity + every parameter type), not just the first parameter as
// findIndexer does (#774). The prior first-parameter-only match selected the
// wrong overload when overloads shared a first parameter type (`[r,c]` vs
// `[r,s]` → IR_FAIL when the wrong getter bound a string to an int param), and
// picked a 1-parameter overload for a 2-argument call (dropping the extra index
// → WRONG_VALUE). Candidates are gathered over the receiver type and its bases.
// Selection mirrors the sema selector in ExprTypecheck_Exprs.cpp so codegen
// lowers exactly the overload sema typed: an exact full-list match wins; failing
// that the sole/first arity-matching candidate is used (its arguments coerced at
// the call site, preserving the single-indexer widening path). Null when no
// candidate's arity matches (sema has already reported §23.4).
const sema::PropertySymbol* Emitter::selectIndexer(
    const sema::TypeSymbol* ts, const ast::IndexExpr& ix) const {
    if (!ts) return nullptr;
    const std::size_t nargs = ix.indices.size();
    std::vector<const sema::TypeRef*> argTys;
    argTys.reserve(nargs);
    for (const auto& idx : ix.indices)
        argTys.push_back(idx.value ? ctx_.exprType(*idx.value) : nullptr);
    const sema::PropertySymbol* arityOnly = nullptr;
    std::vector<const sema::TypeSymbol*> frontier{ts};
    for (std::size_t fi = 0; fi < frontier.size(); ++fi) {
        const sema::TypeSymbol* t = frontier[fi];
        for (const sema::Symbol* m : t->members) {
            auto* ps = dynamic_cast<const sema::PropertySymbol*>(m);
            if (!ps || !ps->isIndexer) continue;
            auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl);
            if (!pd || pd->indexerParams.size() != nargs) continue;
            if (!arityOnly) arityOnly = ps;
            bool all = true;
            for (std::size_t k = 0; k < nargs; ++k) {
                const sema::TypeRef* pty =
                    pd->indexerParams[k].type
                        ? ctx_.resolvedType(*pd->indexerParams[k].type)
                        : nullptr;
                if (!indexerArgMatches(pty, argTys[k])) { all = false; break; }
            }
            if (all) return ps;  // exact full-parameter-list match
        }
        for (const sema::TypeSymbol* b : t->baseTypes)
            if (b) frontier.push_back(b);
    }
    return arityOnly;  // sole/first arity match (implicit conv), or null
}


// fix #682/#1079 / stdlib §4.4 / spec §16.3 (v0.182, #1579): Range-slice an
// Array<T> — extracted from lowerCollectionIndexerGet's `arr[r]` handling so
// the named `Array<T>.Slice(r)` method (§16.3: "`arr[r]` as the
// compiler-recognized equivalent") can share it exactly. `idxExpr` is a
// RangeExpr or a Range-typed variable; both lower to a %bz.range struct
// { i64 start, i64 end, i64 step, i8 flags }. Returns null (no behaviour,
// no side effects) when `idxExpr` is not Range-typed, so callers fall
// through to their own scalar-index path.
llvm::Value* Emitter::lowerArrayRangeSlice(llvm::Value* recv,
                                           const sema::TypeRef* recvTy,
                                           const ast::Expr& idxExpr) {
    auto* idxTy = ctx_.exprType(idxExpr);
    bool isRangeIdx = isRangeType(idxTy);
    if (!isRangeIdx) {
        if (dynamic_cast<const ast::RangeExpr*>(&idxExpr))
            isRangeIdx = true;  // any RangeExpr used as an index
    }
    if (!isRangeIdx) return nullptr;

    int64_t esz = collectionElemSize(recvTy);
    // Get array length for open-ended endpoints and bounds-checking.
    auto* lenFnS = declareC("bz_array_len", i64(), {ptrTy()});
    llvm::Value* arrLen = builder_.CreateCall(lenFnS, {recv}, "sl.arrlen");
    llvm::Value* lo = nullptr, *hi = nullptr;
    bool inclusive = true;
    if (auto* re = dynamic_cast<const ast::RangeExpr*>(&idxExpr)) {
        if (re->lo) {
            lo = lowerExpr(*re->lo);
            if (lo && !lo->getType()->isIntegerTy(64))
                lo = builder_.CreateSExt(lo, i64(), "sl.lo");
        }
        if (re->hi) {
            hi = lowerExpr(*re->hi);
            if (hi && !hi->getType()->isIntegerTy(64))
                hi = builder_.CreateSExt(hi, i64(), "sl.hi");
        }
        inclusive = re->inclusive;
    } else {
        // Range-typed variable: load struct and extract lo/hi/flags.
        llvm::Value* rv = lowerExpr(idxExpr);
        if (!rv) return nullptr;
        lo  = builder_.CreateExtractValue(rv, {0u}, "sl.lo");
        hi  = builder_.CreateExtractValue(rv, {1u}, "sl.hi");
        llvm::Value* flg  = builder_.CreateExtractValue(rv, {3u}, "sl.flg");
        llvm::Value* incl = builder_.CreateTrunc(flg, i1(), "sl.incl");
        // Convert inclusive hi to exclusive hi dynamically.
        llvm::Value* hiPlus = builder_.CreateAdd(hi, builder_.getInt64(1));
        hi = builder_.CreateSelect(incl, hiPlus, hi, "sl.endexcl");
        inclusive = false;  // already exclusive
    }
    // Resolve open-ended endpoints.
    if (!lo) lo = builder_.getInt64(0);
    if (!hi) {
        hi = arrLen;
        inclusive = false;  // open end is past-the-end (exclusive)
    }
    // Convert inclusive end to exclusive.
    llvm::Value* endExcl = inclusive
        ? builder_.CreateAdd(hi, builder_.getInt64(1), "sl.endexcl")
        : hi;
    // §15.6: bounds-check — start must be in [0, Count], end in [0, Count],
    // start <= end.  Raise IndexOutOfRangeException on violation.
    llvm::Value* loNeg = builder_.CreateICmpSLT(lo, builder_.getInt64(0), "sl.loneg");
    llvm::Value* endOOB = builder_.CreateICmpSGT(endExcl, arrLen, "sl.eoob");
    llvm::Value* loGtEnd = builder_.CreateICmpSGT(lo, endExcl, "sl.loge");
    llvm::Value* oor = builder_.CreateOr(
        builder_.CreateOr(loNeg, endOOB), loGtEnd, "sl.oor");
    emitGuardThrow(oor, "IndexOutOfRangeException");
    auto* slFn = declareC("bz_array_slice", ptrTy(),
                          {ptrTy(), i64(), i64(), i64()});
    return builder_.CreateCall(
        slFn, {recv, lo, endExcl, builder_.getInt64(esz)}, "sl.arr");
}

// issue #388 / stdlib §4.4-§4.9: lower a HEAP-collection indexer read `a[i]` /
// `dict[key]` against the runtime backing. Mirrors the `.Get(i)` (§4.5) and
// `.TryGetValue` (§4.8) lowering in LlvmBackend_Calls.cpp.
llvm::Value* Emitter::lowerCollectionIndexerGet(const ast::IndexExpr& ix) {
    if (!ix.target || ix.indices.empty() || !ix.indices[0].value)
        return nullptr;
    auto* recvTy = ctx_.exprType(*ix.target);

    // §3.3 (#883): ReadOnlyStringSpan[i] → the (Start + i)-th CODE POINT of the
    // source string (a char), bounds-checked against the span's Length (not the
    // string's). The source (field 0) is a string pointer, so the element is read
    // via loadCodePoint — NOT bz_array_get (the array-span path below). This must
    // precede the isSpanLikeTypeName branch, which would mis-treat the string
    // source as an array.
    if (recvTy && recvTy->kind == sema::TypeRef::Kind::Named &&
        !recvTy->declaration && recvTy->astNode && recvTy->astNode->name &&
        !recvTy->astNode->name->parts.empty() &&
        recvTy->headName() == "ReadOnlyStringSpan") {
        llvm::Value* spanVal = lowerExpr(*ix.target);
        if (!spanVal) return nullptr;
        llvm::Value* idx = lowerExpr(*ix.indices[0].value);
        if (!idx) return nullptr;
        auto* spanSt = arraySpanStructType();
        auto* slot = entryAlloca(spanSt);
        builder_.CreateStore(spanVal, slot);
        llvm::Value* src  = builder_.CreateLoad(ptrTy(),
            builder_.CreateStructGEP(spanSt, slot, 0), "ss.src");
        llvm::Value* base = builder_.CreateLoad(i32(),
            builder_.CreateStructGEP(spanSt, slot, 1), "ss.st");
        llvm::Value* slen = builder_.CreateLoad(i32(),
            builder_.CreateStructGEP(spanSt, slot, 2), "ss.len");
        if (!idx->getType()->isIntegerTy(32))
            idx = builder_.CreateTrunc(idx, i32());
        llvm::Value* idxNeg = builder_.CreateICmpSLT(idx, builder_.getInt32(0));
        llvm::Value* idxOOB = builder_.CreateICmpSGE(idx, slen);
        emitGuardThrow(builder_.CreateOr(idxNeg, idxOOB),
                       "IndexOutOfRangeException");
        llvm::Value* absIdx = builder_.CreateAdd(
            builder_.CreateSExt(base, i64()),
            builder_.CreateSExt(idx,  i64()), "ss.absi");
        return loadCodePoint(src, absIdx);   // i32 code point (char)
    }

    // §4.15 (#549): ArraySpan<T>[i] / ReadOnlyArraySpan<T>[i]
    // → Source[Start + i] with bounds-check against Length (not array Count).
    if (isSpanLikeTypeName(recvTy)) {
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
        // §4.15 (#1158): also bounds-check the absolute index against the source
        // array's LIVE Count. The span's Length is fixed at construction, but the
        // backing Array<T> can shrink (RemoveAt/Remove/Clear) below Start+Length;
        // the spec getter is `return Source[Start + i]`, which routes through the
        // array's own Count-checked indexer (§4.4 / §31.2). bz_array_get does NOT
        // bounds-check, so without this a shrunk array yields stale backing data
        // instead of throwing. Mirrors the `arr[i]` Count check below (unsigned
        // compare also rejects a negative absolute index).
        auto* spLenFn = declareC("bz_array_len", i64(), {ptrTy()});
        llvm::Value* arrCount = builder_.CreateCall(spLenFn, {src}, "sp.cnt");
        emitGuardThrow(builder_.CreateICmpUGE(absIdx, arrCount, "sp.cntoob"),
                       "IndexOutOfRangeException");
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

    // issue #545: Buffer<T>[i] get — §16 bounds-checked read via bz_buffer_get_ptr.
    // Layout: bz_buffer_hdr_t { i64 len, i64 elem_size } + inline payload.
    // Bounds check: raise IndexOutOfRangeException if idx < 0 OR idx >= len.
    // Load: call bz_buffer_get_ptr(buf, idx64) → void* element pointer; load T.
    // Returns the element value (copy — T is a value type per restriction).
    {
        bool isBuf = recvTy && recvTy->kind == sema::TypeRef::Kind::Named &&
                     !recvTy->declaration && recvTy->astNode && recvTy->astNode->name &&
                     !recvTy->astNode->name->parts.empty() &&
                     recvTy->headName() == "Buffer";
        if (isBuf) {
            llvm::Value* buf = lowerExpr(*ix.target);
            if (!buf) return nullptr;
            llvm::Value* idx = lowerExpr(*ix.indices[0].value);
            if (!idx) return nullptr;
            // Widen idx to i64 for comparison and bz_buffer_get_ptr.
            llvm::Value* idx64 = idx->getType()->isIntegerTy(64)
                ? idx : builder_.CreateSExt(idx, i64(), "buf.idx64");
            // Load the header len field.
            // bz_buffer_hdr_t: first field is int64 len at byte offset 0.
            // We load directly from the buffer pointer (opaque ptr) as i64
            // to avoid struct-type aliasing issues with CreateStructGEP.
            llvm::Value* len64 = builder_.CreateLoad(i64(), buf, "buf.len");
            // §16 / issue #545: bounds check — raise IndexOutOfRangeException
            // if (uint64_t)idx >= (uint64_t)len (covers negative idx too).
            llvm::Value* oor = builder_.CreateICmpUGE(idx64, len64, "buf.oor");
            emitGuardThrow(oor, "IndexOutOfRangeException");
            // Call bz_buffer_get_ptr to get the element pointer.
            auto* ptrFn = declareC("bz_buffer_get_ptr", ptrTy(), {ptrTy(), i64()});
            llvm::Value* elemPtr =
                builder_.CreateCall(ptrFn, {buf, idx64}, "buf.eptr");
            // Determine the LLVM type for T so we can load the element.
            llvm::Type* elemTy = i32();  // default: int
            if (!recvTy->genericArgs.empty() && recvTy->genericArgs[0]) {
                if (auto* et = lowerTypeRef(recvTy->genericArgs[0]))
                    elemTy = et;
            }
            llvm::Value* elem = builder_.CreateLoad(elemTy, elemPtr, "buf.elem");
            // For narrow INTEGER types (i8/i16), sign-extend to i32 so arithmetic
            // operations on the result work correctly (Bezel ints are i32).
            // bool (i1) is left as i1 — used directly in `if` conditions; it never
            // participates in integer arithmetic (sema prevents bool+bool etc.).
            if (elemTy->isIntegerTy(8) || elemTy->isIntegerTy(16))
                return builder_.CreateSExt(elem, i32(), "buf.elem.i32");
            return elem;
        }
    }

    // issue #401 / §4.1-§4.3: a list-INTERFACE receiver (IReadOnlyList<T> /
    // IList<T> / IOwningList<T>) is array-backed; its runtime pointer is the same
    // bz_array_t* as the concrete Array<T>/OwningArray<T>, and its element type is
    // genericArgs[0] (same as the array path), so it takes the `isArr` get path.
    bool isArr  = isCollectionTypeName(recvTy) || isListInterfaceName(recvTy);
    bool isDict = isDictTypeName(recvTy);
    if (!isArr && !isDict) return nullptr;
    llvm::Value* recv = lowerExpr(*ix.target);
    if (!recv) return nullptr;
    // §28.2 v0.135 / issue #621: checked disposal mode guard for the built-in
    // array-backed collection indexer getter. bz_array_mark_disposed sets a
    // `disposed` byte on the bz_array_t header and quarantines the struct
    // (does not free); bz_array_is_disposed reads that byte. Guard arrays only
    // (isArr); dict and LinkedList have separate disposal paths not yet guarded.
    if (checkedDisposal_ && isArr) {
        llvm::Type* i8Ty = llvm::Type::getInt8Ty(llvm_);
        auto* isFn = declareC("bz_array_is_disposed", i8Ty, {ptrTy()});
        llvm::Value* flag = builder_.CreateCall(isFn, {recv}, "cd.arrdis");
        llvm::Value* disposed = builder_.CreateICmpNE(
            flag, llvm::ConstantInt::get(i8Ty, 0), "cd.disposed");
        emitGuardThrow(disposed, "DisposedAccessException");
    }

    // --- Array<T> / OwningArray<T> indexer get (§4.4 / §4.5; #401 §4.1-§4.3) --
    // `a[i]` reads the slot at i (the getter borrows for class T, copies for
    // value T). The element type is genericArgs[0]; bounds-check against Count.
    if (isArr) {
        // fix #682 / §15.6 / §16.3 (v0.182, #1579): `arr[r]` where r is a
        // Range<int> → slice (new Array<T>). Shared with the named
        // `Array<T>.Slice(r)` method (stdlib §4.4) via lowerArrayRangeSlice.
        if (auto* sliced =
                lowerArrayRangeSlice(recv, recvTy, *ix.indices[0].value))
            return sliced;
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

    // --- Dictionary<K,V> / OwningDictionary<K,V> indexer get (§4.8 / §4.9) --
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
    // cases are: a record, nullable-value-type, or primitive struct (decimal) —
    // load the boxed struct out of the box (issue #393 / #539 / fix #685);
    // scalars — decoded from the inttoptr'd slot; object/string pointers — the
    // void* IS the pointer.
    llvm::Value* valPtr = builder_.CreateLoad(ptrTy(), tmpSlot);
    // fix #685: any struct-valued type (record, nullable, or e.g. decimal) stored
    // as a heap-boxed void* — load the struct bytes back from the box.
    if (valLlvm->isStructTy())
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
// slot-take form) on an OWNING heap collection (OwningArray / OwningDictionary).
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
    // §15.3 / §28.11 (#747): `move a[i]` on a move-only OWNING inline array
    // ([N owned]T? / [N owned]T). Read the owned element out of the slot and
    // leave the slot NULL (moved-from), so the array's scope-end slot disposal
    // (Disposal.inlineOwnedArray) skips it — the new owner now owns the element.
    // Without this null the dangling slot pointer would be double-freed at scope
    // end (the inline array stays disposal-eligible — a slot-move is a borrow of
    // the array as a whole). lvalueAddr computes the bounds-checked slot address,
    // evaluating the index exactly once.
    if (recvTy && recvTy->kind == sema::TypeRef::Kind::InlineArray &&
        recvTy->arrayOwned) {
        llvm::Type* slotElemTy = nullptr;
        llvm::Value* slotAddr = lvalueAddr(ix, slotElemTy);
        if (!slotAddr || !slotElemTy) return nullptr;
        llvm::Value* moved = builder_.CreateLoad(slotElemTy, slotAddr, "ioa.mv");
        builder_.CreateStore(
            llvm::Constant::getNullValue(slotElemTy), slotAddr);
        return moved;
    }
    // §4.14 / §28.11 (#907): `move sp[i]` on an OWNING `Span<classT>` (the span
    // returned by `OwningBuffer<classT>.AsSpan()`). The span's slots own their
    // elements, so a move transfers ownership OUT of the underlying buffer slot
    // at absolute index Start+i and leaves it NULL (moved-from) — the buffer's
    // scope-end disposal then skips it (no double-free). The borrowing spans
    // (ArraySpan/ReadOnly*) reject `move` in sema (#871); a value-typed Span has
    // nothing to own, so it falls through to a plain read.
    if (isOwningSpanTypeName(recvTy)) {
        const sema::TypeRef* spEl =
            (!recvTy->genericArgs.empty()) ? recvTy->genericArgs[0] : nullptr;
        if (!elementPolicyFor(spEl, /*owningFamily=*/true).disposerThunk)
            return nullptr;  // value-typed Span: plain read, not an owning move
        llvm::Value* spanVal = lowerExpr(*ix.target);
        if (!spanVal) return nullptr;
        auto* spanSt = arraySpanStructType();
        auto* slot = entryAlloca(spanSt);
        builder_.CreateStore(spanVal, slot);
        llvm::Value* src  = builder_.CreateLoad(ptrTy(),
            builder_.CreateStructGEP(spanSt, slot, 0), "spm.src");
        llvm::Value* base = builder_.CreateLoad(i32(),
            builder_.CreateStructGEP(spanSt, slot, 1), "spm.st");
        llvm::Value* slen = builder_.CreateLoad(i32(),
            builder_.CreateStructGEP(spanSt, slot, 2), "spm.len");
        llvm::Value* idx = lowerExpr(*ix.indices[0].value);   // evaluate once
        if (!idx) return nullptr;
        if (!idx->getType()->isIntegerTy(32))
            idx = builder_.CreateTrunc(idx, i32());
        llvm::Value* idxNeg = builder_.CreateICmpSLT(idx, builder_.getInt32(0));
        llvm::Value* idxOOB = builder_.CreateICmpSGE(idx, slen);
        emitGuardThrow(builder_.CreateOr(idxNeg, idxOOB),
                       "IndexOutOfRangeException");
        llvm::Value* absIdx = builder_.CreateAdd(
            builder_.CreateSExt(base, i64()),
            builder_.CreateSExt(idx,  i64()), "spm.absi");
        int64_t esz = collectionElemSize(recvTy);
        llvm::Type* retTy = ptrTy();
        if (recvTy && !recvTy->genericArgs.empty() && recvTy->genericArgs[0])
            if (auto* et = lowerTypeRef(recvTy->genericArgs[0])) retTy = et;
        // Read the owned element out of the underlying buffer slot.
        llvm::AllocaInst* out = entryAlloca(retTy);
        auto* getFn = declareC("bz_array_get", llvm::Type::getVoidTy(llvm_),
                               {ptrTy(), i64(), ptrTy(), i64()});
        builder_.CreateCall(getFn, {src, absIdx, out, builder_.getInt64(esz)});
        llvm::Value* moved = builder_.CreateLoad(retTy, out, "spm.elem");
        // Leave the slot moved-from (null) via the NON-disposing setter.
        llvm::AllocaInst* nullSlot = entryAlloca(retTy);
        builder_.CreateStore(llvm::Constant::getNullValue(retTy), nullSlot);
        auto* setFn = declareC("bz_array_set", llvm::Type::getVoidTy(llvm_),
                               {ptrTy(), i64(), ptrTy(), i64()});
        builder_.CreateCall(setFn,
                            {src, absIdx, nullSlot, builder_.getInt64(esz)});
        return moved;
    }

    bool isArr  = isCollectionTypeName(recvTy);
    bool isDict = isDictTypeName(recvTy);
    if (!isArr && !isDict) return nullptr;

    // --- OwningArray<T> slot-move (§28.11; admitted only on owning arrays) ----
    if (isArr) {
        // Only an OWNING array's slot carries ownership to transfer; a borrowing
        // Array<T> slot is a borrow and `move arr[i]` is rejected by sema. Guard
        // on the disposer so a non-owning array (defensively) never nulls a slot.
        // #602 / §28.11 v0.135: derive ownership from elementPolicyFor directly.
        {
            const sema::TypeRef* arrEl =
                (!recvTy->genericArgs.empty()) ? recvTy->genericArgs[0] : nullptr;
            bool arrIsOwning =
                recvTy->astNode && recvTy->astNode->name &&
                !recvTy->astNode->name->parts.empty() &&
                common::isOwningPrefixedName(recvTy->headName());
            if (!elementPolicyFor(arrEl, arrIsOwning).disposerThunk) return nullptr;
        }
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

    // --- OwningDictionary<K,V> slot-move (§28.11 / §4.9) ---------------------
    // Only an owning dictionary's value carries ownership; a borrowing
    // Dictionary<K,V> value is a borrow and `move dict[key]` is rejected by
    // sema. The owned thing is the VALUE (genericArgs[1]).
    // #602 batch 4 / §28.11 v0.135: gate on elementPolicyFor (ClassOwned) rather
    // than collectionElemIsClass, so Shared<T>/Weak<T> values are included.
    // collectionElemIsClass checked Class|Interface only; elementPolicyFor covers
    // the full ClassOwned row (class, interface, Shared, Weak).
    const sema::TypeRef* dictMoveValTy =
        (recvTy && recvTy->genericArgs.size() > 1) ? recvTy->genericArgs[1]
                                                    : nullptr;
    bool owningDict =
        recvTy && recvTy->astNode && recvTy->astNode->name &&
        !recvTy->astNode->name->parts.empty() &&
        recvTy->headName() == "OwningDictionary" &&
        elementPolicyFor(dictMoveValTy, /*owningFamily=*/true).cat ==
            ElementPolicy::Category::ClassOwned;
    if (!owningDict) return nullptr;
    llvm::Value* recv = lowerExpr(*ix.target);
    if (!recv) return nullptr;
    const sema::TypeRef* keyTy =
        genericBackingType(recvTy, 0, /*isSet=*/false, nullptr);
    bool keyIsRecord = keyTy && recordTypeOfRef(keyTy) != nullptr;
    // An owning dictionary owns class-typed VALUES (per §28.11 ClassOwned row),
    // so the stored void* is the owned object pointer; no scalar/record decode
    // is needed — the ClassOwned gate above ensures a pointer-sized slot.

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
// `a[i] := v` / `dict[key] := v`. For an OWNING collection (OwningArray /
// OwningDictionary) the displaced slot/value is disposed exactly once before the
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
    // §4.14 (#779): the OWNING Span<T>[i] := value over an OwningBuffer DISPOSES
    // the displaced class-typed element (the slot owns it), mirroring the direct
    // OwningBuffer<T>[i] disposing-set. ReadOnly spans have no setter.
    if (isSpanLikeTypeName(recvTy)) {
        std::string_view rcn = (recvTy->astNode && recvTy->astNode->name &&
                                !recvTy->astNode->name->parts.empty())
            ? std::string_view(recvTy->headName()) : "";
        if (rcn == "ReadOnlyArraySpan" || rcn == "ReadOnlySpan")
            return false;  // read-only: no setter
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
        llvm::AllocaInst* valSlot = entryAlloca(val->getType());
        builder_.CreateStore(val, valSlot);
        // §4.14 (#779): for an owning Span<classT> the displaced owner must be
        // disposed exactly once before the new owner is stored (the slot owns the
        // element). For a borrowing ArraySpan (or a value-typed Span) the slot is
        // a borrow / a value — no disposal (§4.15 / §4.4).
        const sema::TypeRef* spEl =
            (!recvTy->genericArgs.empty()) ? recvTy->genericArgs[0] : nullptr;
        llvm::Function* elemDisp = isOwningSpanTypeName(recvTy)
            ? elementPolicyFor(spEl, /*owningFamily=*/true).disposerThunk
            : nullptr;
        if (elemDisp) {
            auto* setFn = declareC("bz_array_set_disposing",
                                   llvm::Type::getVoidTy(llvm_),
                                   {ptrTy(), i64(), ptrTy(), i64(), ptrTy()});
            builder_.CreateCall(setFn,
                {src, absIdx, valSlot, builder_.getInt64(esz), elemDisp});
        } else {
            auto* setFn = declareC("bz_array_set", llvm::Type::getVoidTy(llvm_),
                                   {ptrTy(), i64(), ptrTy(), i64()});
            builder_.CreateCall(setFn,
                {src, absIdx, valSlot, builder_.getInt64(esz)});
        }
        return true;
    }

    // issue #545: Buffer<T>[i] := val — §16 bounds-checked write via bz_buffer_get_ptr.
    // Bounds check, get element pointer, store val.  Value elements: no disposal of
    // the displaced slot (no ownership obligation for primitives).
    {
        bool isBuf = recvTy && recvTy->kind == sema::TypeRef::Kind::Named &&
                     !recvTy->declaration && recvTy->astNode && recvTy->astNode->name &&
                     !recvTy->astNode->name->parts.empty() &&
                     recvTy->headName() == "Buffer";
        if (isBuf) {
            llvm::Value* buf = lowerExpr(*ix.target);
            if (!buf) return false;
            llvm::Value* idx = lowerExpr(*ix.indices[0].value);
            if (!idx) return false;
            llvm::Value* idx64 = idx->getType()->isIntegerTy(64)
                ? idx : builder_.CreateSExt(idx, i64(), "buf.sidx64");
            // Load header len for bounds check.
            // bz_buffer_hdr_t: int64 len at byte offset 0 (opaque ptr direct load).
            llvm::Value* len64 = builder_.CreateLoad(i64(), buf, "buf.slen");
            // §16 / issue #545: bounds check.
            llvm::Value* oor = builder_.CreateICmpUGE(idx64, len64, "buf.soor");
            emitGuardThrow(oor, "IndexOutOfRangeException");
            // Get element pointer.
            auto* ptrFn = declareC("bz_buffer_get_ptr", ptrTy(), {ptrTy(), i64()});
            llvm::Value* elemPtr =
                builder_.CreateCall(ptrFn, {buf, idx64}, "buf.septr");
            // Determine the slot element type from T (genericArgs[0]) so we
            // can truncate a wider-than-slot value (e.g. i32 literal into a
            // byte slot).  If val's type already matches or is narrower, store
            // directly; otherwise truncate to the slot width.
            llvm::Type* slotTy = val->getType();
            if (!recvTy->genericArgs.empty() && recvTy->genericArgs[0]) {
                if (auto* et = lowerTypeRef(recvTy->genericArgs[0]))
                    slotTy = et;
            }
            llvm::Value* storeVal = val;
            if (storeVal->getType() != slotTy) {
                if (slotTy->isIntegerTy() && storeVal->getType()->isIntegerTy()) {
                    unsigned slotBits  = slotTy->getIntegerBitWidth();
                    unsigned valBits   = storeVal->getType()->getIntegerBitWidth();
                    if (valBits > slotBits)
                        storeVal = builder_.CreateTrunc(storeVal, slotTy, "buf.str.trunc");
                    else if (valBits < slotBits)
                        storeVal = builder_.CreateSExt(storeVal, slotTy, "buf.str.ext");
                }
            }
            builder_.CreateStore(storeVal, elemPtr);
            return true;
        }
    }

    // issue #401 / §4.1-§4.3: a list-INTERFACE receiver is array-backed (same
    // bz_array_t* as the concrete array), so it takes the `isArr` set path. The
    // OWNING interface IOwningList<T> (§4.2) carries the disposing-setter
    // semantics; IList<T> (§4.1, borrowing) and IReadOnlyList<T> (get-only) do
    // not. IReadOnlyList<T> declares no setter, so it never reaches this set.
    bool listIfaceOwning = false;
    bool isListIface = isListInterfaceName(recvTy, &listIfaceOwning);
    bool isArr  = isCollectionTypeName(recvTy) || isListIface;
    bool isDict = isDictTypeName(recvTy);
    if (!isArr && !isDict) return false;
    llvm::Value* recv = lowerExpr(*ix.target);
    if (!recv) return false;
    // §28.2 v0.135 / issue #621: checked disposal mode guard for the built-in
    // array-backed collection indexer setter. Mirrors the getter guard above.
    if (checkedDisposal_ && isArr) {
        llvm::Type* i8Ty = llvm::Type::getInt8Ty(llvm_);
        auto* isFn = declareC("bz_array_is_disposed", i8Ty, {ptrTy()});
        llvm::Value* flag = builder_.CreateCall(isFn, {recv}, "cd.arrdis");
        llvm::Value* disposed = builder_.CreateICmpNE(
            flag, llvm::ConstantInt::get(i8Ty, 0), "cd.disposed");
        emitGuardThrow(disposed, "DisposedAccessException");
    }

    // --- Array<T> / OwningArray<T> indexer set (§4.4 / §4.5; #401 §4.1-§4.3) --
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
        // #602 / §28.11 v0.135: derive element policy from elementPolicyFor.
        {
            const sema::TypeRef* arrEl =
                (!recvTy->genericArgs.empty()) ? recvTy->genericArgs[0] : nullptr;
            bool arrIsOwning =
                recvTy->astNode && recvTy->astNode->name &&
                !recvTy->astNode->name->parts.empty() &&
                common::isOwningPrefixedName(recvTy->headName());
            ElementPolicy arrPol = elementPolicyFor(arrEl, arrIsOwning);
            bool isStrElemArr =
                !arrIsOwning &&
                arrPol.cat == ElementPolicy::Category::RuntimeManaged &&
                arrEl && !arrEl->nullable &&
                arrEl->kind == sema::TypeRef::Kind::Primitive &&
                arrEl->primitive == sema::PrimitiveType::String &&
                recvTy->astNode && recvTy->astNode->name &&
                !recvTy->astNode->name->parts.empty() &&
                [](std::string_view n){return n=="Array"||n=="Stack"||n=="Queue";}(
                    recvTy->headName());
            if (isStrElemArr) {
                if (!valueExpr || !isPlusOneStringProducer(valueExpr))
                    emitStringRetain(val);
                llvm::AllocaInst* slot = entryAlloca(val->getType());
                builder_.CreateStore(val, slot);
                llvm::Function* strDisp = stringElementReleaseThunk();
                auto* fn = declareC("bz_array_set_disposing",
                    llvm::Type::getVoidTy(llvm_),
                    {ptrTy(), i64(), ptrTy(), i64(), ptrTy()});
                builder_.CreateCall(fn, {recv, idx, slot, builder_.getInt64(esz),
                                         strDisp});
                return true;
            }
        }
        llvm::AllocaInst* slot = entryAlloca(val->getType());
        builder_.CreateStore(val, slot);
        // §4.5 / §4.2: an OWNING array's set(owned) — whether reached through the
        // concrete OwningArray<T> or through an IOwningList<T> view (#401) —
        // disposes the displaced class element before storing the new owner; a
        // borrowing Array<T> / IList<T> set just overwrites (no slot ownership).
        // #602 / §28.11 v0.135: element disposer from elementPolicyFor (covers
        // class, interface, Shared<T>, and Weak<T> elements; IOwningList<T> case
        // falls back to elementDisposerThunk() since the interface receiver's
        // concrete element type is not in its TypeRef — T: class is guaranteed
        // by §4.2's constraint, so the shared vtable thunk is correct).
        {
            const sema::TypeRef* arrEl =
                (!recvTy->genericArgs.empty()) ? recvTy->genericArgs[0] : nullptr;
            bool arrIsOwning2 =
                recvTy->astNode && recvTy->astNode->name &&
                !recvTy->astNode->name->parts.empty() &&
                common::isOwningPrefixedName(recvTy->headName());
            llvm::Function* elemDisp =
                elementPolicyFor(arrEl, arrIsOwning2).disposerThunk;
            if (!elemDisp && listIfaceOwning) elemDisp = elementDisposerThunk();
            if (elemDisp) {
                auto* fn = declareC("bz_array_set_disposing",
                    llvm::Type::getVoidTy(llvm_),
                    {ptrTy(), i64(), ptrTy(), i64(), ptrTy()});
                builder_.CreateCall(fn, {recv, idx, slot, builder_.getInt64(esz),
                                         elemDisp});
                return true;
            }
        }
        auto* fn = declareC("bz_array_set", llvm::Type::getVoidTy(llvm_),
                            {ptrTy(), i64(), ptrTy(), i64()});
        builder_.CreateCall(fn, {recv, idx, slot, builder_.getInt64(esz)});
        return true;
    }

    // --- Dictionary<K,V> / OwningDictionary<K,V> indexer set (§4.8 / §4.9) --
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
    // §4.9: an OWNING dictionary (OwningDictionary<K,V> where V: class/interface)
    // disposes the displaced value before storing the new owner. Unlike owning
    // ARRAYS (whose owned element is genericArgs[0]), an owning dictionary's
    // owned thing is its VALUE = genericArgs[1].
    // #602 / §28.11 v0.135: derive value disposer for the dict indexer-set
    // overwrite path directly from elementPolicyFor (covers class AND interface
    // value types, as well as Shared<T>/Weak<T> handles — batch 3 wired this
    // path; batch 4 removes the last stale reference to deleted helpers).
    bool dictIsOwning2 =
        recvTy && recvTy->astNode && recvTy->astNode->name &&
        !recvTy->astNode->name->parts.empty() &&
        recvTy->headName() == "OwningDictionary";
    const sema::TypeRef* dictValTy2 =
        (recvTy && recvTy->genericArgs.size() > 1) ? recvTy->genericArgs[1]
                                                    : nullptr;
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
        elementPolicyFor(dictValTy2, dictIsOwning2).disposerThunk;
    if (!valDisp) valDisp = dictValueBoxFreer(recvTy);
    // isStrVal: borrowing Dictionary<K, string> — detect via elementPolicyFor.
    bool isStrVal =
        !dictIsOwning2 && dictValTy2 && !dictValTy2->nullable &&
        dictValTy2->kind == sema::TypeRef::Kind::Primitive &&
        dictValTy2->primitive == sema::PrimitiveType::String &&
        elementPolicyFor(dictValTy2, /*owning=*/false).cat ==
            ElementPolicy::Category::RuntimeManaged;
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


const sema::TypeSymbol* Emitter::indexReceiverDecl(const ast::Expr* target) {
    if (!target) return nullptr;
    if (auto* tt = ctx_.exprType(*target)) {
        if (tt->kind == sema::TypeRef::Kind::Named && tt->declaration)
            return tt->declaration;
        // #794 / §14.2: a bare generic-parameter receiver (`x: T`, no
        // declaration) constrained `where T: IIndexable`. Resolve T through the
        // active monomorphization to its concrete class/record declaration so the
        // indexer access devirtualizes to the substituted type's own indexer —
        // exactly mirroring the method-call constraint devirt in
        // LlvmBackend_Calls.cpp (~9095). In an unspecialized template body there
        // is no substitution, so this falls through and the access stays gated
        // (returns 0) rather than miscompiling.
        if (tt->kind == sema::TypeRef::Kind::Named && !tt->declaration &&
            tt->astNode && tt->astNode->name &&
            tt->astNode->name->parts.size() == 1) {
            auto sit = genericSubstSemaTy_.find(tt->headName());
            if (sit != genericSubstSemaTy_.end() && sit->second &&
                sit->second->kind == sema::TypeRef::Kind::Named &&
                sit->second->declaration)
                return sit->second->declaration;
        }
    }
    // #747 R42: `this[i]` inside an instance method — `this` carries no sema
    // exprType, so recover the owner TypeSymbol from the current method (mirrors
    // lowerWith's reverse-lookup of currentFn_ in methodFns_).
    if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(target)) {
        if ((id->name == "this" || id->name == "base") && currentFn_) {
            for (const auto& [ms, fn] : methodFns_) {
                if (fn != currentFn_) continue;
                if (ms->parent) return ms->parent;
                // #1237 / §26.5: extension methods have no parent TypeSymbol.
                // The extension's targetType is the receiver's declared type,
                // so `this[i]` inside an extension on an interface dispatches
                // through the itable just like a direct interface-variable
                // indexer read.
                if (isExtensionMethod(ms) && ms->extensionDecl->targetType) {
                    const sema::TypeRef* t =
                        ctx_.resolvedType(*ms->extensionDecl->targetType);
                    if (t && t->kind == sema::TypeRef::Kind::Named &&
                        t->declaration)
                        return t->declaration;
                }
            }
            if (currentSpecMethodSymbol_ && currentSpecMethodSymbol_->parent)
                return currentSpecMethodSymbol_->parent;
            // #1237: same for a specialised extension method body.
            if (currentSpecMethodSymbol_ &&
                isExtensionMethod(currentSpecMethodSymbol_) &&
                currentSpecMethodSymbol_->extensionDecl->targetType) {
                const sema::TypeRef* t = ctx_.resolvedType(
                    *currentSpecMethodSymbol_->extensionDecl->targetType);
                if (t && t->kind == sema::TypeRef::Kind::Named && t->declaration)
                    return t->declaration;
            }
        }
    }
    return nullptr;
}

llvm::Value* Emitter::lowerIndexerGet(const ast::IndexExpr& ix) {
    if (!ix.target || ix.indices.empty() || !ix.indices[0].value)
        return nullptr;
    const sema::TypeSymbol* recvDecl = indexReceiverDecl(ix.target.get());
    if (!recvDecl) return nullptr;
    auto* ps = selectIndexer(recvDecl, ix);  // per §23.4 (#1172)
    if (!ps) return nullptr;
    auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl);
    if (!pd) return nullptr;
    // #730 / §13 / §23.4: an indexer accessed through an INTERFACE-typed receiver
    // (`(arr: IFoo)[i]`) dispatches dynamically through the itable — the static
    // body resolution below would find the (bodiless) interface indexer and bail
    // to 0. Load the implementing class's getter from itab[get-index] and call it
    // as `T get(ptr this, idx...)`.
    if (recvDecl->kind == ast::TypeDeclKind::Interface) {
        auto ifIt = ifacePropertyGetIndex_.find(ps);
        auto descIt = descArray_.find(recvDecl);
        if (ifIt == ifacePropertyGetIndex_.end() || descIt == descArray_.end())
            return nullptr;
        llvm::Type* retTy = lowerType(pd->type.get());
        if (!retTy) {
            // Generic T return (e.g. IReadOnlySpan<T> indexer): recover the
            // concrete element type from the receiver's first generic arg. // per §4.16
            const sema::TypeRef* recvTy = ctx_.exprType(*ix.target);
            if (recvTy && !recvTy->genericArgs.empty())
                retTy = lowerTypeRef(recvTy->genericArgs[0]);
            if (!retTy) retTy = i32();
        }
        std::vector<llvm::Type*> paramTys;
        paramTys.push_back(ptrTy());
        for (const auto& p : pd->indexerParams) {
            llvm::Type* pt = p.type ? lowerType(p.type.get()) : nullptr;
            paramTys.push_back(pt ? pt : i32());
        }
        auto* gFnTy = llvm::FunctionType::get(retTy, paramTys, false);
        llvm::Value* obj = lowerExpr(*ix.target);
        if (!obj || !obj->getType()->isPointerTy()) return nullptr;
        llvm::Value* vptr = builder_.CreateLoad(ptrTy(), obj);
        auto* lookupFn =
            declareC("bz_itable_lookup", ptrTy(), {ptrTy(), ptrTy()});
        llvm::Value* itmapPtr = builder_.CreateLoad(
            ptrTy(), builder_.CreateGEP(ptrTy(), vptr, {builder_.getInt32(1)}));
        llvm::Value* itab = builder_.CreateCall(
            llvm::FunctionType::get(ptrTy(), {ptrTy(), ptrTy()}, false),
            lookupFn, {itmapPtr, descIt->second}, "itab");
        llvm::Value* fnPtr = builder_.CreateLoad(
            ptrTy(),
            builder_.CreateGEP(ptrTy(), itab,
                               {builder_.getInt32(ifIt->second)}),
            "idxget");
        std::vector<llvm::Value*> callArgs;
        callArgs.push_back(obj);
        for (std::size_t k = 0; k < pd->indexerParams.size() &&
             k < ix.indices.size(); ++k) {
            if (!ix.indices[k].value) return nullptr;
            llvm::Value* argVal = lowerExpr(*ix.indices[k].value);
            if (!argVal) return nullptr;
            callArgs.push_back(coerceNumericToType(argVal, paramTys[k + 1],
                                                   false));
        }
        llvm::Value* r = builder_.CreateCall(gFnTy, fnPtr, callArgs);
        emitPendingCheck();
        return r;
    }
    // #1080 / §23.4: a virtual/override/abstract indexer on a CLASS receiver
    // dispatches dynamically through the vtable slot (the static getter-body
    // resolution below binds the COMPILE-TIME receiver type's getter, so a
    // `base[i]` or `this[i]` read would run the base/abstract body). Load the
    // getter fn-ptr from vt[propGetSlot_] and call it as `T get(ptr this,
    // idx...)`. Mirrors the #481 property-vtable path; the slot was assigned by
    // buildVtable and filled with ensureIndexerGetterFn.
    if (recvDecl->kind == ast::TypeDeclKind::Class) {
        auto gsIt = propGetSlot_.find(ps);
        if (gsIt != propGetSlot_.end()) {
            llvm::Type* retTy = lowerType(pd->type.get());
            if (!retTy) retTy = i32();
            std::vector<llvm::Type*> paramTys;
            paramTys.push_back(ptrTy());
            for (const auto& p : pd->indexerParams) {
                llvm::Type* pt = p.type ? lowerType(p.type.get()) : nullptr;
                paramTys.push_back(pt ? pt : i32());
            }
            auto* gFnTy = llvm::FunctionType::get(retTy, paramTys, false);
            llvm::Value* obj = lowerExpr(*ix.target);
            if (obj && obj->getType()->isPointerTy()) {
                llvm::Value* vptr = builder_.CreateLoad(ptrTy(), obj);
                llvm::Value* fnPtr = builder_.CreateLoad(
                    ptrTy(),
                    builder_.CreateGEP(ptrTy(), vptr,
                                       {builder_.getInt32(gsIt->second)}),
                    "idxget");
                std::vector<llvm::Value*> callArgs;
                callArgs.push_back(obj);
                for (std::size_t k = 0; k < pd->indexerParams.size() &&
                     k < ix.indices.size(); ++k) {
                    if (!ix.indices[k].value) return nullptr;
                    llvm::Value* argVal = lowerExpr(*ix.indices[k].value);
                    if (!argVal) return nullptr;
                    callArgs.push_back(
                        coerceNumericToType(argVal, paramTys[k + 1], false));
                }
                llvm::Value* r = builder_.CreateCall(gFnTy, fnPtr, callArgs);
                emitPendingCheck();
                return r;
            }
        }
    }
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
    // #794: lower the index ARGUMENTS in the CALLER's scope — BEFORE clearing
    // locals_/paramSlots_ for the inline getter body. An index expression may
    // reference the enclosing method's locals or parameters (e.g. `x[i]` where
    // `i` is an outer parameter). Lowering it after the clear (as the prior code
    // did) resolved the outer name to 0. This surfaced for an indexer reached
    // through a generic constraint (`At<T>(x: T, i: int) where T: IIndexable`
    // → `x[i]`, where the index `i` is the method parameter), but it was a latent
    // hazard for any expression-bodied user indexer whose index references a
    // caller local/param.
    std::vector<llvm::Value*> argVals;
    for (std::size_t k = 0; k < ps->indexerParams.size() &&
         k < ix.indices.size(); ++k) {
        if (!ix.indices[k].value) { argVals.push_back(nullptr); continue; }
        llvm::Value* av = lowerExpr(*ix.indices[k].value);
        if (!av) return nullptr;
        argVals.push_back(av);
    }
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
    // §9.2 / #794: a RECORD receiver is a by-value struct; the getter body reads
    // its fields through `this` as a pointer, so spill the value to a stack slot
    // and bind the address. (A class receiver is already a pointer — skip.) This
    // matters for an indexer reached through a generic constraint where T = a
    // record: `lowerExpr(x)` yields the record value, and a field GEP on a
    // non-pointer would emit malformed IR. Mirrors the block-form getter spill.
    if (!obj->getType()->isPointerTy()) {
        llvm::AllocaInst* tmp = entryAlloca(obj->getType());
        builder_.CreateStore(obj, tmp);
        obj = tmp;
    }
    currentThis_ = obj;
    locals_.clear();
    paramSlots_.clear();
    // Bind each indexer parameter to its pre-lowered argument value.
    for (std::size_t k = 0; k < ps->indexerParams.size() && k < argVals.size();
         ++k) {
        if (!argVals[k]) continue;
        auto* slot = entryAlloca(argVals[k]->getType());
        builder_.CreateStore(argVals[k], slot);
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
    const sema::TypeSymbol* recvDecl = indexReceiverDecl(ix.target.get());
    if (!recvDecl) return false;
    auto* ps = selectIndexer(recvDecl, ix);  // per §23.4 (#1172)
    if (!ps) return false;
    auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl);
    if (!pd) return false;
    // #730 / §13 / §23.4: `(arr: IFoo)[i] := v` through an INTERFACE-typed
    // receiver dispatches the setter dynamically through the itable — load the
    // implementing class's setter from itab[set-index] and call it as
    // `void set(ptr this, idx..., value)`.
    if (recvDecl->kind == ast::TypeDeclKind::Interface) {
        auto ifIt = ifacePropertySetIndex_.find(ps);
        auto descIt = descArray_.find(recvDecl);
        if (ifIt == ifacePropertySetIndex_.end() || descIt == descArray_.end())
            return false;
        llvm::Type* valTy = lowerType(pd->type.get());
        if (!valTy) valTy = i32();
        std::vector<llvm::Type*> paramTys;
        paramTys.push_back(ptrTy());
        for (const auto& p : pd->indexerParams) {
            llvm::Type* pt = p.type ? lowerType(p.type.get()) : nullptr;
            paramTys.push_back(pt ? pt : i32());
        }
        paramTys.push_back(valTy);
        auto* sFnTy = llvm::FunctionType::get(
            llvm::Type::getVoidTy(llvm_), paramTys, false);
        llvm::Value* obj = lowerExpr(*ix.target);
        if (!obj || !obj->getType()->isPointerTy()) return false;
        llvm::Value* vptr = builder_.CreateLoad(ptrTy(), obj);
        auto* lookupFn =
            declareC("bz_itable_lookup", ptrTy(), {ptrTy(), ptrTy()});
        llvm::Value* itmapPtr = builder_.CreateLoad(
            ptrTy(), builder_.CreateGEP(ptrTy(), vptr, {builder_.getInt32(1)}));
        llvm::Value* itab = builder_.CreateCall(
            llvm::FunctionType::get(ptrTy(), {ptrTy(), ptrTy()}, false),
            lookupFn, {itmapPtr, descIt->second}, "itab");
        llvm::Value* fnPtr = builder_.CreateLoad(
            ptrTy(),
            builder_.CreateGEP(ptrTy(), itab,
                               {builder_.getInt32(ifIt->second)}),
            "idxset");
        std::vector<llvm::Value*> callArgs;
        callArgs.push_back(obj);
        for (std::size_t k = 0; k < pd->indexerParams.size() &&
             k < ix.indices.size(); ++k) {
            if (!ix.indices[k].value) return false;
            llvm::Value* argVal = lowerExpr(*ix.indices[k].value);
            if (!argVal) return false;
            callArgs.push_back(coerceNumericToType(argVal, paramTys[k + 1],
                                                   false));
        }
        callArgs.push_back(coerceNumericToType(val, valTy, false));
        builder_.CreateCall(sFnTy, fnPtr, callArgs);
        emitPendingCheck();
        return true;
    }
    // #1080 / §23.4: a virtual/override/abstract indexer SETTER on a CLASS
    // receiver dispatches dynamically through the vtable slot (symmetric with
    // the getter path above). Load the setter fn-ptr from vt[propSetSlot_] and
    // call it as `void set(ptr this, idx..., value)`.
    if (recvDecl->kind == ast::TypeDeclKind::Class) {
        auto ssIt = propSetSlot_.find(ps);
        if (ssIt != propSetSlot_.end()) {
            llvm::Type* valTy = lowerType(pd->type.get());
            if (!valTy) valTy = i32();
            std::vector<llvm::Type*> paramTys;
            paramTys.push_back(ptrTy());
            for (const auto& p : pd->indexerParams) {
                llvm::Type* pt = p.type ? lowerType(p.type.get()) : nullptr;
                paramTys.push_back(pt ? pt : i32());
            }
            paramTys.push_back(valTy);
            auto* sFnTy = llvm::FunctionType::get(
                llvm::Type::getVoidTy(llvm_), paramTys, false);
            llvm::Value* obj = lowerExpr(*ix.target);
            if (obj && obj->getType()->isPointerTy()) {
                llvm::Value* vptr = builder_.CreateLoad(ptrTy(), obj);
                llvm::Value* fnPtr = builder_.CreateLoad(
                    ptrTy(),
                    builder_.CreateGEP(ptrTy(), vptr,
                                       {builder_.getInt32(ssIt->second)}),
                    "idxset");
                std::vector<llvm::Value*> callArgs;
                callArgs.push_back(obj);
                for (std::size_t k = 0; k < pd->indexerParams.size() &&
                     k < ix.indices.size(); ++k) {
                    if (!ix.indices[k].value) return false;
                    llvm::Value* argVal = lowerExpr(*ix.indices[k].value);
                    if (!argVal) return false;
                    callArgs.push_back(
                        coerceNumericToType(argVal, paramTys[k + 1], false));
                }
                callArgs.push_back(coerceNumericToType(val, valTy, false));
                builder_.CreateCall(sFnTy, fnPtr, callArgs);
                emitPendingCheck();
                return true;
            }
        }
    }
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
    // #794: lower index ARGUMENTS in the CALLER's scope, BEFORE clearing
    // locals_/paramSlots_ (they may reference the enclosing method's
    // locals/params — the same hazard fixed in lowerIndexerGet).
    std::vector<llvm::Value*> argVals;
    for (std::size_t k = 0; k < ps->indexerParams.size() &&
         k < ix.indices.size(); ++k) {
        if (!ix.indices[k].value) { argVals.push_back(nullptr); continue; }
        llvm::Value* av = lowerExpr(*ix.indices[k].value);
        if (!av) return false;
        argVals.push_back(av);
    }
    llvm::Value* savedThis = currentThis_;
    auto savedLocals = locals_;
    auto savedParams = paramSlots_;
    // §14.1 / #794: install the receiver's generic substitution (mirrors the
    // getter path) so a `T`-typed field read/write inside the setter body uses
    // the concrete layout, not the i32 erasure.
    auto savedSubst = setupReceiverGenericSubst(ix.target.get(), ps->parent);
    // §9.2 / #794: spill a by-value RECORD receiver to a pointer for `this`.
    if (!obj->getType()->isPointerTy()) {
        llvm::AllocaInst* tmp = entryAlloca(obj->getType());
        builder_.CreateStore(obj, tmp);
        obj = tmp;
    }
    currentThis_ = obj;
    locals_.clear();
    paramSlots_.clear();
    // Indexer params (pre-lowered above).
    for (std::size_t k = 0; k < ps->indexerParams.size() && k < argVals.size();
         ++k) {
        if (!argVals[k]) continue;
        auto* slot = entryAlloca(argVals[k]->getType());
        builder_.CreateStore(argVals[k], slot);
        paramSlots_[ps->indexerParams[k]] = slot;
    }
    // `value` param: look for the LocalSymbol anchored on pd.
    // The codegen's `lowerPropertySet` uses locals_[pd] for `value`.
    if (val) {
        auto* slot = entryAlloca(val->getType());
        builder_.CreateStore(val, slot);
        locals_[pd] = slot;
    }
    if (setterExpr) {
        lowerExpr(*setterExpr);
    } else if (setterStmts) {
        // §23.3 / §23.5 (#1236): mirror lowerPropertySetValue (§22.9 / #739) —
        // set up an accessor-end block so `return;` inside the block body ends
        // only this setter, not the enclosing method. Without this, `return;`
        // falls through to the enclosing method's return path, emitting a
        // second terminator into the same BB (Facet A: IR verification failure)
        // or disposing the enclosing method's locals early (Facet B: the write
        // is committed but the object is freed before the subsequent read).
        llvm::BasicBlock* savedAccEnd = inlinedAccessorEnd_;
        auto* accEndBB = llvm::BasicBlock::Create(llvm_, "idxset.end", currentFn_);
        inlinedAccessorEnd_ = accEndBB;
        bool term = lowerScopedBlock(*setterStmts);
        inlinedAccessorEnd_ = savedAccEnd;
        if (!term) builder_.CreateBr(accEndBB);
        builder_.SetInsertPoint(accEndBB);
    }
    currentThis_ = savedThis;
    locals_ = std::move(savedLocals);
    paramSlots_ = std::move(savedParams);
    genericSubst_ = std::move(savedSubst);
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
    const sema::TypeSymbol* recvDecl = indexReceiverDecl(ix->target.get());
    if (!recvDecl) return false;
    auto* ps = selectIndexer(recvDecl, *ix);  // per §23.4 (#1172)
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
    if (!rhs) {
        // #1386: ICE — lowerExpr returned null for the RHS of a compound
        // indexer assignment; the expression kind is not yet handled.
        ctx_.diagnostics().report(
            Severity::Error, as.value->span.begin,
            "internal: lowerExpr returned null for compound indexer "
            "assignment RHS (block-getter path) — #1386");
        return false;
    }
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
// on a HEAP-collection Array<T> / OwningArray<T> indexer. The plain-assignment
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
// fix #613 / §25.1: compound-increment / compound-decrement on a
// Dictionary<K,V> receiver was previously not handled (the earlier note said
// "dict compound-assign is unusual"; that was for OP= in general, but for
// `d[k] OP= v` we now cover it here uniformly alongside the array path).
//
// The encoder/decoder round-trip is the same as for the plain-set / plain-get
// paths already implemented in lowerCollectionIndexerSet / lowerCollectionIndexerGet.
// We reuse those helpers (read via lowerCollectionIndexerGet, encode new value
// via lowerCollectionIndexerSet with a synthetic IndexExpr) to keep the fix
// minimal.  lowerCollectionIndexerGet already handles both string and generic
// (non-string) keys and the full inttoptr / box decode.  lowerCollectionIndexerSet
// handles the matching encode + bz_dict_set_str / bz_dict_set_gen write-back.
bool Emitter::lowerCollectionIndexerCompound(const ast::AssignmentStmt& as) {
    if (!as.target || !as.value) return false;
    auto* ix = dynamic_cast<const ast::IndexExpr*>(as.target.get());
    if (!ix || !ix->target || ix->indices.empty() || !ix->indices[0].value)
        return false;
    auto* recvTy = ctx_.exprType(*ix->target);
    // Handle Array<T>/OwningArray<T> and list-interface receivers (§4.1-§4.3)
    // as before, and also Dictionary<K,V>/OwningDictionary<K,V> (fix #613 / §4.8).
    bool isArr  = isCollectionTypeName(recvTy) || isListInterfaceName(recvTy);
    bool isDict = isDictTypeName(recvTy);
    if (!isArr && !isDict) return false;

    // --- Dictionary<K,V> OP= path (fix #613 / §4.8 / §25.1) ---
    // Read via lowerCollectionIndexerGet (handles key lookup, inttoptr decode,
    // record-box decode), apply OP, write via lowerCollectionIndexerSet (handles
    // key lookup, inttoptr encode, record-box encode).  Receiver and key are
    // evaluated once by the helpers (each call re-evaluates via lowerExpr; to
    // preserve single-evaluation we spill the key to an alloca and pass a fake
    // IndexExpr — but the helpers each call lowerExpr internally, so the
    // simplest sound approach is to fall through to the array path for isArr,
    // and for isDict call the helpers which re-evaluate via lowerExpr.  For
    // typical key expressions (literals / local reads) this is a no-op; for
    // side-effecting keys it would double-evaluate but sema already disallows
    // side-effecting dict key expressions in read-modify-write positions.
    // The key is re-evaluated twice but the RECEIVER is re-evaluated once
    // because both helpers look it up from ix->target via lowerExpr which for
    // a local-variable receiver just loads the pointer — effectively free.
    if (isDict) {
        llvm::Value* cur = lowerCollectionIndexerGet(*ix);
        if (!cur) return false;
        llvm::Value* rhs = lowerExpr(*as.value);
        if (!rhs) {
            // #1386: ICE — lowerExpr returned null for the RHS of a
            // compound dict-indexer assignment; expression kind unhandled.
            ctx_.diagnostics().report(
                Severity::Error, as.value->span.begin,
                "internal: lowerExpr returned null for compound dict-indexer "
                "assignment RHS — #1386");
            return false;
        }
        std::string op(as.op);
        if (!op.empty() && op.back() == '=') op.pop_back();
        // fix #626 / §25.1: if the VALUE element type has a user-defined
        // `static operator <op>` (two params), dispatch to it and write back —
        // mirrors the #610 overload check in lowerStmt.  emitCompoundOp cannot
        // handle struct operands (LLVM rejects sadd.with.overflow on struct
        // types), so the overload must be checked BEFORE emitCompoundOp.
        {
            const sema::TypeRef* valTy =
                (recvTy && recvTy->genericArgs.size() > 1)
                    ? recvTy->genericArgs[1] : nullptr;
            sema::MethodSymbol* opMethod = nullptr;
            if (valTy && valTy->kind == sema::TypeRef::Kind::Named &&
                valTy->declaration) {
                for (sema::Symbol* m : valTy->declaration->members) {
                    auto* ms = dynamic_cast<sema::MethodSymbol*>(m);
                    if (ms && ms->isOperator && ms->operatorToken == op &&
                        ms->params.size() == 2) {
                        opMethod = ms;
                        break;
                    }
                }
            }
            if (opMethod) {
                // §25.1: desugar to get → overload call → set.
                std::vector<llvm::Value*> args{cur, rhs};
                llvm::Value* res =
                    emitMethodCall(opMethod, args, /*baseQualified=*/false);
                if (!res) return false;
                return lowerCollectionIndexerSet(*ix, res, /*valueExpr=*/nullptr);
            }
        }
        // Determine the value LLVM type to compute newVal in the right type.
        const sema::TypeRef* valTy =
            (recvTy && recvTy->genericArgs.size() > 1) ? recvTy->genericArgs[1]
                                                        : nullptr;
        llvm::Type* valLlvm = valTy ? lowerTypeRef(valTy) : cur->getType();
        if (!valLlvm) valLlvm = cur->getType();
        // Widen RHS to match (mirrors lowerCollectionIndexerCompound array path).
        if (rhs->getType() != valLlvm && rhs->getType()->isIntegerTy() &&
            valLlvm->isIntegerTy())
            rhs = builder_.CreateSExt(rhs, valLlvm);
        llvm::Value* newVal = emitCompoundOp(
            op, cur, rhs, valLlvm->isFloatingPointTy(),
            isUnsignedTargetExpr(*as.target));
        if (!newVal) return false;
        // Write back via lowerCollectionIndexerSet (encodes and dispatches to
        // bz_dict_set_str / bz_dict_set_gen, handling disposing variants).
        return lowerCollectionIndexerSet(*ix, newVal, /*valueExpr=*/nullptr);
    }

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
    if (!rhs) {
        // #1386: ICE — lowerExpr returned null for the RHS of a compound
        // array-indexer assignment; the expression kind is not yet handled.
        ctx_.diagnostics().report(
            Severity::Error, as.value->span.begin,
            "internal: lowerExpr returned null for compound array-indexer "
            "assignment RHS — #1386");
        return false;
    }
    std::string op(as.op);
    if (!op.empty() && op.back() == '=') op.pop_back();

    // fix #626 / §25.1: if the element type has a user-defined
    // `static operator <op>` (two params), dispatch to it and write back —
    // mirrors the #610 overload check in lowerStmt and the dict branch above.
    // emitCompoundOp cannot handle struct operands (LLVM rejects
    // sadd.with.overflow on struct types), so this must be checked FIRST.
    llvm::Value* newVal = nullptr;
    {
        const sema::TypeRef* elemTyRef =
            (recvTy && !recvTy->genericArgs.empty())
                ? recvTy->genericArgs[0] : nullptr;
        sema::MethodSymbol* opMethod = nullptr;
        if (elemTyRef && elemTyRef->kind == sema::TypeRef::Kind::Named &&
            elemTyRef->declaration) {
            for (sema::Symbol* m : elemTyRef->declaration->members) {
                auto* ms = dynamic_cast<sema::MethodSymbol*>(m);
                if (ms && ms->isOperator && ms->operatorToken == op &&
                    ms->params.size() == 2) {
                    opMethod = ms;
                    break;
                }
            }
        }
        if (opMethod) {
            // §25.1: desugar to get → overload call → set. The write-back MUST
            // reuse the already-evaluated recv/idx (the shared block below)
            // rather than re-lowering `*ix` via lowerCollectionIndexerSet —
            // that helper re-evaluates BOTH ix->target and ix->indices[0].value,
            // double-evaluating a side-effecting index subexpression
            // (`arr[Idx()] += …`) and violating §17.6 "evaluated exactly once"
            // (#1299): the get read one slot and the set wrote another.
            std::vector<llvm::Value*> args{cur, rhs};
            newVal = emitMethodCall(opMethod, args, /*baseQualified=*/false);
            if (!newVal) return false;
        }
    }

    // Non-overload path: compute the new value with the built-in operator.
    if (!newVal) {
        newVal = emitCompoundOp(
            op, cur, rhs, elemTy->isFloatingPointTy(),
            isUnsignedTargetExpr(*as.target));
        if (!newVal) return false;
    }

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

    // --- Write back (bz_array_set / bz_array_set_disposing for OwningArray). ---
    llvm::AllocaInst* setSlot = entryAlloca(newVal->getType());
    builder_.CreateStore(newVal, setSlot);

    bool listIfaceOwning = false;
    if (isListInterfaceName(recvTy, &listIfaceOwning)) { /* already set */ }
    // #602 / §28.11 v0.135: derive element disposer from elementPolicyFor.
    {
        const sema::TypeRef* ixEl =
            (!recvTy->genericArgs.empty()) ? recvTy->genericArgs[0] : nullptr;
        bool ixIsOwning =
            recvTy->astNode && recvTy->astNode->name &&
            !recvTy->astNode->name->parts.empty() &&
            common::isOwningPrefixedName(recvTy->headName());
        llvm::Function* elemDisp =
            elementPolicyFor(ixEl, ixIsOwning).disposerThunk;
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
    }
    return true;
}


// fix #613 / §25.1 / §4.4 / §4.8: `arr[i]++` / `arr[i]--` and
// `dict[k]++` / `dict[k]--` on BUILT-IN heap collections (Array<T>,
// Dictionary<K,V> and their owning variants).
//
// Root cause: the IncrementStmt handler in lowerStmt calls lvalueAddr, which
// returns null for bz_array_t* / bz_dict_t* receivers (no LLVM ArrayType).
// The #587 fallback then tried (a) block-setter property and (b) user-defined
// indexer — neither matches a built-in collection since they have no
// PropertyDecl.  Both branches fell through without writing back.
//
// Fix: read via lowerCollectionIndexerGet (handles array bz_array_get with
// bounds-check and dict bz_dict_try_get_* with inttoptr/box decode), compute
// ±1, write back via lowerCollectionIndexerSet (handles bz_array_set and the
// dict bz_dict_set_* encode + owning-dispose variants).  Receiver and key are
// evaluated internally by each helper; for typical element types (int, bool,
// enum) the round-trip is the same as a plain `arr[i] := arr[i] + 1`.
//
// Returns true if `inc->target` is a built-in heap-collection IndexExpr and
// the increment/decrement was emitted; false if the caller should try other
// paths (user-defined indexer, computed property, plain alloca).
bool Emitter::lowerCollectionIndexerIncrDecr(
    const ast::IncrementStmt& inc) {
    if (!inc.target) return false;
    auto* ix = dynamic_cast<const ast::IndexExpr*>(inc.target.get());
    if (!ix || !ix->target || ix->indices.empty() || !ix->indices[0].value)
        return false;
    auto* recvTy = ctx_.exprType(*ix->target);
    bool isArr  = isCollectionTypeName(recvTy) || isListInterfaceName(recvTy);
    bool isDict = isDictTypeName(recvTy);
    if (!isArr && !isDict) return false;

    const std::string binOp = (inc.op == "++") ? "+" : "-";

    // --- Array<T> path: single-evaluation get → ±1 → set (§17.6, #1299). ---
    // Evaluate the receiver AND the index ONCE and reuse them for both the read
    // and the write-back — going through lowerCollectionIndexerGet then
    // lowerCollectionIndexerSet(*ix, …) re-lowers ix->target and
    // ix->indices[0].value twice, double-evaluating a side-effecting index
    // (`arr[Idx()]++`): the get read one slot and the set incremented another.
    // (The dict path below still uses the helpers — sema disallows a
    // side-effecting dict key in a read-modify-write position, matching the
    // compound-assign path.)
    if (isArr) {
        llvm::Value* recv = lowerExpr(*ix->target);
        if (!recv) return false;
        llvm::Value* idx = lowerExpr(*ix->indices[0].value);
        if (!idx) return false;
        idx = idx->getType()->isIntegerTy(64) ? idx
            : builder_.CreateSExt(idx, i64());

        auto* lenFn = declareC("bz_array_len", i64(), {ptrTy()});
        llvm::Value* len = builder_.CreateCall(lenFn, {recv});
        emitGuardThrow(builder_.CreateICmpUGE(idx, len, "cid.oor"),
                       "IndexOutOfRangeException");

        int64_t esz = collectionElemSize(recvTy);
        llvm::Type* elemTy = i32();
        if (recvTy && !recvTy->genericArgs.empty() && recvTy->genericArgs[0])
            if (auto* et = lowerTypeRef(recvTy->genericArgs[0])) elemTy = et;

        llvm::AllocaInst* getSlot = entryAlloca(elemTy);
        auto* getFn = declareC("bz_array_get", llvm::Type::getVoidTy(llvm_),
                               {ptrTy(), i64(), ptrTy(), i64()});
        builder_.CreateCall(getFn, {recv, idx, getSlot, builder_.getInt64(esz)});
        llvm::Value* cur = builder_.CreateLoad(elemTy, getSlot, "cid.cur");

        bool fp = cur->getType()->isFloatingPointTy();
        llvm::Value* one = fp
            ? llvm::cast<llvm::Value>(llvm::ConstantFP::get(cur->getType(), 1.0))
            : llvm::cast<llvm::Value>(llvm::ConstantInt::get(cur->getType(), 1));
        llvm::Value* newVal = emitCompoundOp(
            binOp, cur, one, fp, isUnsignedTargetExpr(*inc.target));
        if (!newVal) return false;

        llvm::AllocaInst* setSlot = entryAlloca(newVal->getType());
        builder_.CreateStore(newVal, setSlot);
        bool listIfaceOwning = false;
        isListInterfaceName(recvTy, &listIfaceOwning);
        const sema::TypeRef* ixEl =
            (!recvTy->genericArgs.empty()) ? recvTy->genericArgs[0] : nullptr;
        bool ixIsOwning =
            recvTy->astNode && recvTy->astNode->name &&
            !recvTy->astNode->name->parts.empty() &&
            common::isOwningPrefixedName(recvTy->headName());
        llvm::Function* elemDisp = elementPolicyFor(ixEl, ixIsOwning).disposerThunk;
        if (!elemDisp && listIfaceOwning) elemDisp = elementDisposerThunk();
        if (elemDisp) {
            auto* setFn = declareC("bz_array_set_disposing",
                                   llvm::Type::getVoidTy(llvm_),
                                   {ptrTy(), i64(), ptrTy(), i64(), ptrTy()});
            builder_.CreateCall(setFn, {recv, idx, setSlot,
                                        builder_.getInt64(esz), elemDisp});
        } else {
            auto* setFn = declareC("bz_array_set", llvm::Type::getVoidTy(llvm_),
                                   {ptrTy(), i64(), ptrTy(), i64()});
            builder_.CreateCall(setFn, {recv, idx, setSlot,
                                        builder_.getInt64(esz)});
        }
        return true;
    }

    // --- Dictionary<K,V> path: read, ±1, write via the helpers. ---
    llvm::Value* cur = lowerCollectionIndexerGet(*ix);
    if (!cur) return false;
    bool fp = cur->getType()->isFloatingPointTy();
    llvm::Value* one = fp
        ? llvm::cast<llvm::Value>(llvm::ConstantFP::get(cur->getType(), 1.0))
        : llvm::cast<llvm::Value>(llvm::ConstantInt::get(cur->getType(), 1));
    llvm::Value* newVal = emitCompoundOp(
        binOp, cur, one, fp, isUnsignedTargetExpr(*inc.target));
    if (!newVal) return false;
    return lowerCollectionIndexerSet(*ix, newVal, /*valueExpr=*/nullptr);
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


// #730 / §13 / §23.4: the class `cls`'s concrete indexer implementing the
// interface indexer `ifaceIdx`. Indexers are anonymous (all named ".this"), so
// classPropertyByName can't be used; match by the first index-parameter type
// via findIndexer (which falls back to the sole indexer when there is one).
const sema::PropertySymbol* Emitter::classIndexerImplementing(
    const sema::TypeSymbol* cls, const sema::PropertySymbol* ifaceIdx) {
    if (!cls || !ifaceIdx) return nullptr;
    const sema::TypeRef* firstParamTy = nullptr;
    if (auto* ipd = dynamic_cast<const ast::PropertyDecl*>(ifaceIdx->decl))
        if (!ipd->indexerParams.empty() && ipd->indexerParams[0].type)
            firstParamTy = ctx_.resolvedType(*ipd->indexerParams[0].type);
    return findIndexer(cls, firstParamTy);
}


// #730 / §23.4 / §13: emit a standalone `T get(ptr this, idx...)` for a class's
// concrete indexer getter, used to fill the itable slot for interface-typed
// indexer dispatch. Block-bodied getters reuse ensureIndexerBlockGetter (same
// signature); expression-bodied getters are emitted here. Cached per indexer
// PropertySymbol.
llvm::Function* Emitter::ensureIndexerGetterFn(const sema::TypeSymbol* cls,
                                               const sema::PropertySymbol* ps) {
    (void)cls;
    auto cached = indexerGetterFns_.find(ps);
    if (cached != indexerGetterFns_.end()) return cached->second;
    auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl);
    if (!pd) return nullptr;

    // Select the getter body form (mirrors lowerIndexerGet).
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
    // Block-bodied: reuse the existing standalone-function emitter (identical
    // `T get(ptr this, idx...)` signature) — no duplicate emission.
    if (getterBlock) {
        llvm::Function* fn = ensureIndexerBlockGetter(pd, getterBlock, ps);
        indexerGetterFns_[ps] = fn;
        return fn;
    }
    if (!getterExpr) return nullptr;

    llvm::Type* retTy = lowerType(pd->type.get());
    if (!retTy) retTy = i32();
    std::vector<llvm::Type*> paramTys;
    paramTys.push_back(ptrTy());                     // `this`
    for (const auto& p : pd->indexerParams) {
        llvm::Type* pt = p.type ? lowerType(p.type.get()) : nullptr;
        paramTys.push_back(pt ? pt : i32());
    }
    auto* fnTy = llvm::FunctionType::get(retTy, paramTys, false);
    auto* fn = llvm::Function::Create(
        fnTy, llvm::GlobalValue::InternalLinkage, "bz$indexer$get", *module_);
    indexerGetterFns_[ps] = fn;

    auto* entryBB = llvm::BasicBlock::Create(llvm_, "entry", fn);
    // Save + reset the full per-function emitter state (mirrors
    // ensureIndexerBlockGetter): the body may emit checks/throws that branch to
    // the unwind/propagate blocks, which must be this function's, not the
    // caller's (else IR references a block in another function).
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
    auto  savedFinally  = finallyStack_;
    auto* savedUnwind   = unwindTarget_;
    auto* savedProp     = propagateBB_;
    auto* savedCaught   = currentCaughtExc_;
    auto  savedUD       = unwindScopeDepth_;
    auto  savedDiv      = diverged_;
    auto  savedGSubst   = genericSubst_;

    currentFn_ = fn;
    builder_.SetInsertPoint(entryBB);
    locals_.clear();
    paramSlots_.clear();
    refParams_.clear();
    disposeStack_.clear();
    loopStack_.clear();
    finallyStack_.clear();
    unwindTarget_     = nullptr;
    propagateBB_      = nullptr;
    currentCaughtExc_ = nullptr;
    unwindScopeDepth_ = 0;
    diverged_         = false;

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
    llvm::Value* result = lowerExpr(*getterExpr);
    if (!result) result = llvm::Constant::getNullValue(retTy);
    result = coerceNumericToType(result, retTy, false);
    if (!builder_.GetInsertBlock()->getTerminator())
        builder_.CreateRet(result);

    currentFn_        = savedFn;
    currentThis_      = savedThis;
    locals_           = std::move(savedLocals);
    paramSlots_       = std::move(savedParams);
    refParams_        = std::move(savedRef);
    disposeStack_     = std::move(savedDispose);
    loopStack_        = std::move(savedLoop);
    finallyStack_     = std::move(savedFinally);
    unwindTarget_     = savedUnwind;
    propagateBB_      = savedProp;
    currentCaughtExc_ = savedCaught;
    unwindScopeDepth_ = savedUD;
    diverged_         = savedDiv;
    genericSubst_     = std::move(savedGSubst);
    if (savedBB) builder_.SetInsertPoint(savedBB, savedBBOffset);
    return fn;
}


// #730 / §23.4 / §13: emit a standalone `void set(ptr this, idx..., value)` for
// a class's concrete indexer setter, used to fill the itable slot for
// interface-typed indexer assignment. Handles expression- and block-bodied
// setters. Cached per indexer PropertySymbol. Returns null if the indexer has
// no setter.
llvm::Function* Emitter::ensureIndexerSetterFn(const sema::TypeSymbol* cls,
                                               const sema::PropertySymbol* ps) {
    (void)cls;
    auto cached = indexerSetterFns_.find(ps);
    if (cached != indexerSetterFns_.end()) return cached->second;
    auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl);
    if (!pd) return nullptr;

    const ast::Expr* setterExpr = nullptr;
    const std::vector<std::unique_ptr<ast::Stmt>>* setterStmts = nullptr;
    for (const auto& a : pd->accessors) {
        if (a.isSet) {
            if (a.exprBody) setterExpr = a.exprBody.get();
            else if (!a.stmts.empty()) setterStmts = &a.stmts;
            break;
        }
    }
    if (!setterExpr && !setterStmts) return nullptr;

    llvm::Type* valTy = lowerType(pd->type.get());
    if (!valTy) valTy = i32();
    std::vector<llvm::Type*> paramTys;
    paramTys.push_back(ptrTy());                     // `this`
    for (const auto& p : pd->indexerParams) {
        llvm::Type* pt = p.type ? lowerType(p.type.get()) : nullptr;
        paramTys.push_back(pt ? pt : i32());
    }
    paramTys.push_back(valTy);                        // implicit `value`
    auto* fnTy = llvm::FunctionType::get(
        llvm::Type::getVoidTy(llvm_), paramTys, false);
    auto* fn = llvm::Function::Create(
        fnTy, llvm::GlobalValue::InternalLinkage, "bz$indexer$set", *module_);
    indexerSetterFns_[ps] = fn;

    auto* entryBB = llvm::BasicBlock::Create(llvm_, "entry", fn);
    // Save + reset the full per-function emitter state (see ensureIndexerGetterFn).
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
    auto  savedFinally  = finallyStack_;
    auto* savedUnwind   = unwindTarget_;
    auto* savedProp     = propagateBB_;
    auto* savedCaught   = currentCaughtExc_;
    auto  savedUD       = unwindScopeDepth_;
    auto  savedDiv      = diverged_;
    auto  savedGSubst   = genericSubst_;

    currentFn_ = fn;
    builder_.SetInsertPoint(entryBB);
    locals_.clear();
    paramSlots_.clear();
    refParams_.clear();
    disposeStack_.clear();
    loopStack_.clear();
    finallyStack_.clear();
    unwindTarget_     = nullptr;
    propagateBB_      = nullptr;
    currentCaughtExc_ = nullptr;
    unwindScopeDepth_ = 0;
    diverged_         = false;

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
    // The implicit `value` param is the last argument; the setter body reads it
    // via locals_[pd] (the same anchor lowerIndexerSet uses).
    if (argIt != fn->args().end()) {
        llvm::Argument& a = *argIt;
        llvm::AllocaInst* slot = entryAlloca(a.getType());
        builder_.CreateStore(&a, slot);
        locals_[pd] = slot;
    }
    if (setterExpr) lowerExpr(*setterExpr);
    else if (setterStmts) lowerScopedBlock(*setterStmts);
    if (!builder_.GetInsertBlock()->getTerminator())
        builder_.CreateRetVoid();

    currentFn_        = savedFn;
    currentThis_      = savedThis;
    locals_           = std::move(savedLocals);
    paramSlots_       = std::move(savedParams);
    refParams_        = std::move(savedRef);
    disposeStack_     = std::move(savedDispose);
    loopStack_        = std::move(savedLoop);
    finallyStack_     = std::move(savedFinally);
    unwindTarget_     = savedUnwind;
    propagateBB_      = savedProp;
    currentCaughtExc_ = savedCaught;
    unwindScopeDepth_ = savedUD;
    diverged_         = savedDiv;
    genericSubst_     = std::move(savedGSubst);
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
        // §22.9 (#739): inline the setter body with an accessor-end target so a
        // `return;` ends the setter rather than the enclosing method.
        llvm::BasicBlock* savedAccEnd = inlinedAccessorEnd_;
        auto* accEndBB = llvm::BasicBlock::Create(llvm_, "setter.end", currentFn_);
        inlinedAccessorEnd_ = accEndBB;
        bool term = false;
        if (setter->bodyKind == ast::PropertyAccessorBodyKind::Expression &&
            setter->exprBody)
            lowerExpr(*setter->exprBody);
        else
            term = lowerScopedBlock(setter->stmts);
        inlinedAccessorEnd_ = savedAccEnd;
        if (!term) builder_.CreateBr(accEndBB);
        builder_.SetInsertPoint(accEndBB);
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
    // §22.9 (#739): accessor-end target — a `return;` ends the setter, not the
    // enclosing method (the setter body is inlined here).
    llvm::BasicBlock* savedAccEnd = inlinedAccessorEnd_;
    auto* accEndBB = llvm::BasicBlock::Create(llvm_, "setter.end", currentFn_);
    inlinedAccessorEnd_ = accEndBB;
    bool term = false;
    if (setter->bodyKind == ast::PropertyAccessorBodyKind::Expression &&
        setter->exprBody)
        lowerExpr(*setter->exprBody);
    else
        term = lowerScopedBlock(setter->stmts);
    inlinedAccessorEnd_ = savedAccEnd;
    if (!term) builder_.CreateBr(accEndBB);
    builder_.SetInsertPoint(accEndBB);
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


// §29.2 / §4.3 (#1310): shared array/user-class → IEnumerable<T> wrapping. When
// a value is coerced to the pure declaration-less `IEnumerable<T>` (a call
// argument, a local-decl init, or — the #1310 gap — a method RETURN), the value
// must become a real vtable-backed enumerator, because the IEnumerable dispatch
// paths (for-in via lowerIEnumerableBuiltinFor, and the scope-end iterEnumDispose
// via vtable slot 1) read the value's first word as a vtable pointer. A raw
// bz_array_t* / user-class instance has no such vtable → SIGSEGV on iteration or
// disposal. The wrapper SHARES the source (borrowed pointer, no ownership
// transfer), so an owning collection still disposes its elements once at its own
// binder's scope end (no double-free). Mirrors the buildOrderedArgs argument
// path (#1067/#1068/#552).
llvm::Value* Emitter::wrapAsIEnumerableForTarget(
        llvm::Value* v, const sema::TypeRef* targetTy,
        const ast::Expr* srcExpr) {
    if (!v || !targetTy || !srcExpr) return v;
    if (!(targetTy->kind == sema::TypeRef::Kind::Named &&
          !targetTy->declaration && targetTy->astNode &&
          targetTy->astNode->name && !targetTy->astNode->name->parts.empty() &&
          targetTy->headName() == "IEnumerable"))
        return v;
    sema::TypeRef* actualTy = ctx_.exprType(*srcExpr);
    std::string_view an;
    if (actualTy && actualTy->kind == sema::TypeRef::Kind::Named &&
        actualTy->astNode && actualTy->astNode->name &&
        !actualTy->astNode->name->parts.empty())
        an = actualTy->headName();
    bool actualIsArray =
        (an == "Array" || an == "OwningArray" || an == "OwningBuffer" ||
         an == "Stack" || an == "OwningStack" ||
         an == "Queue" || an == "OwningQueue");
    if (actualIsArray)
        return wrapArrayAsIEnumerable(v, targetTy);
    if (actualTy && isLinkedListTypeName(actualTy))
        return wrapLinkedListAsIEnumerable(v, targetTy);
    // A user class implementing IEnumerable<T> (declaration-carrying). A source
    // whose type is ALREADY a declaration-less IEnumerable (an alias / another
    // wrapped value) has no declaration and passes through unchanged.
    if (actualTy && actualTy->kind == sema::TypeRef::Kind::Named &&
        actualTy->declaration)
        return wrapUserClassAsIEnumerable(v, actualTy->declaration, targetTy);
    return v;
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

    // §29.11 (#943): GetEnumerator(self) → a FRESH independent enumerator over the
    // same array, positioned before the first element (idx = -1). The borrowed
    // `arr` pointer is shared (not owned by the enumerator struct), so cloning is a
    // plain struct copy with the cursor reset — a second `for` loop over the same
    // IEnumerable<T> value replays from the start (§29.11) instead of reusing the
    // exhausted cursor. Both source and clone Dispose are a single free() of their
    // own struct; the array itself is unaffected.
    auto* geFn = getOrCreate(prefix + "GetEnumerator",
        llvm::FunctionType::get(ptrTy(), {ptrTy()}, false),
        [&](llvm::Function* f) {
            auto* bb = llvm::BasicBlock::Create(llvm_, "entry", f);
            llvm::IRBuilder<> b(bb);
            llvm::Value* self = f->arg_begin();
            uint64_t cloneSz = module_->getDataLayout().getTypeAllocSize(stTy);
            auto* callocFT = llvm::FunctionType::get(ptrTy(), {i64(), i64()}, false);
            auto* callocC = module_->getOrInsertFunction("calloc", callocFT).getCallee();
            llvm::Value* clone = b.CreateCall(callocFT, callocC,
                {llvm::ConstantInt::get(i64(), 1),
                 llvm::ConstantInt::get(i64(), (int64_t)cloneSz)});
            // Copy vtable (slot 0, ptr), arr (slot 1, ptr), esz (slot 3, i64);
            // reset idx (slot 2) to -1 (before-first).
            llvm::Value* vt = b.CreateLoad(ptrTy(),
                b.CreateStructGEP(stTy, self, 0));
            b.CreateStore(vt, b.CreateStructGEP(stTy, clone, 0));
            llvm::Value* arrCp = b.CreateLoad(ptrTy(),
                b.CreateStructGEP(stTy, self, 1));
            b.CreateStore(arrCp, b.CreateStructGEP(stTy, clone, 1));
            llvm::Value* eszCp = b.CreateLoad(i64(),
                b.CreateStructGEP(stTy, self, 3));
            b.CreateStore(eszCp, b.CreateStructGEP(stTy, clone, 3));
            b.CreateStore(llvm::ConstantInt::get(i64(), -1),
                b.CreateStructGEP(stTy, clone, 2));
            b.CreateRet(clone);
        });

    // Build the vtable (§29.11 #943: 5 slots — slot 4 = GetEnumerator/clone).
    std::string vtName = prefix + "vtable";
    auto* vtGV = module_->getGlobalVariable(vtName);
    if (!vtGV) {
        auto* vtTy = llvm::ArrayType::get(ptrTy(), 5);
        std::vector<llvm::Constant*> elems = {
            llvm::ConstantPointerNull::get(ptrTy()), // slot 0: self-ptr (filled below)
            dispFn,
            mnFn,
            curFn,
            geFn
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


// #1069: node-walking IEnumerable wrapper for (Owned)LinkedList<T>. Mirrors
// wrapArrayAsIEnumerable but the cursor is a bz_llist node pointer rather than an
// index. Struct { ptr vtable, ptr llist, ptr current_node, ptr next_node }:
//   - next_node    : the node MoveNext will return next (init = first node).
//   - current_node : the node Current reads (set by MoveNext).
// Exhaustion is next_node == null; no started-flag is needed. The wrapper SHARES
// the list (no ownership), so an owning list still disposes its elements once at
// its binder's scope end; Dispose just frees the enumerator struct.
llvm::Value* Emitter::wrapLinkedListAsIEnumerable(llvm::Value* list,
                                                  const sema::TypeRef* iEnumTy) {
    llvm::Type* elemTy = i32();
    if (iEnumTy && !iEnumTy->genericArgs.empty() && iEnumTy->genericArgs[0])
        elemTy = lowerTypeRef(iEnumTy->genericArgs[0]);

    std::string elemName = elemTy->isIntegerTy()
        ? ("i" + std::to_string(elemTy->getIntegerBitWidth()))
        : (elemTy->isPointerTy() ? "ptr" : "f");
    std::string stName = "llenum." + elemName;
    auto* stTy = llvm::StructType::getTypeByName(llvm_, stName);
    if (!stTy)
        stTy = llvm::StructType::create(
            llvm_, {ptrTy(), ptrTy(), ptrTy(), ptrTy()}, stName);

    std::string prefix = "bz$llenum$" + elemName + "$";
    auto getOrCreate = [&](const std::string& name, llvm::FunctionType* ft,
                           std::function<void(llvm::Function*)> fill)
        -> llvm::Function* {
        if (auto* f = module_->getFunction(name)) return f;
        auto* f = llvm::Function::Create(ft, llvm::Function::InternalLinkage,
                                         name, module_.get());
        fill(f);
        return f;
    };
    auto* voidTy = llvm::Type::getVoidTy(llvm_);
    auto* firstFT = llvm::FunctionType::get(ptrTy(), {ptrTy()}, false);
    auto* nextFT = llvm::FunctionType::get(ptrTy(), {ptrTy()}, false);
    auto* valFT = llvm::FunctionType::get(ptrTy(), {ptrTy()}, false);

    // Dispose: free(self).
    auto* dispFn = getOrCreate(prefix + "Dispose",
        llvm::FunctionType::get(voidTy, {ptrTy()}, false),
        [&](llvm::Function* f) {
            auto* bb = llvm::BasicBlock::Create(llvm_, "entry", f);
            llvm::IRBuilder<> b(bb);
            auto* freeFn = module_->getOrInsertFunction(
                "free", llvm::FunctionType::get(voidTy, {ptrTy()}, false))
                .getCallee();
            b.CreateCall(llvm::FunctionType::get(voidTy, {ptrTy()}, false),
                         freeFn, {f->arg_begin()});
            b.CreateRetVoid();
        });

    // MoveNext: cur = next; if cur == null return false; current = cur;
    //           next = node_next(cur); return true.
    auto* mnFn = getOrCreate(prefix + "MoveNext",
        llvm::FunctionType::get(i1(), {ptrTy()}, false),
        [&](llvm::Function* f) {
            auto* entry = llvm::BasicBlock::Create(llvm_, "entry", f);
            auto* doneBB = llvm::BasicBlock::Create(llvm_, "ll.none", f);
            auto* advBB = llvm::BasicBlock::Create(llvm_, "ll.adv", f);
            llvm::IRBuilder<> b(entry);
            llvm::Value* self = f->arg_begin();
            llvm::Value* nextF = b.CreateStructGEP(stTy, self, 3);
            llvm::Value* cur = b.CreateLoad(ptrTy(), nextF);
            llvm::Value* isNull = b.CreateICmpEQ(
                cur, llvm::ConstantPointerNull::get(ptrTy()));
            b.CreateCondBr(isNull, doneBB, advBB);
            b.SetInsertPoint(doneBB);
            b.CreateRet(llvm::ConstantInt::getFalse(llvm_));
            b.SetInsertPoint(advBB);
            b.CreateStore(cur, b.CreateStructGEP(stTy, self, 2));  // current = cur
            auto* nextFn = module_->getOrInsertFunction("bz_llist_node_next",
                                                        nextFT).getCallee();
            llvm::Value* nn = b.CreateCall(nextFT, nextFn, {cur});
            b.CreateStore(nn, nextF);                              // next = next(cur)
            b.CreateRet(llvm::ConstantInt::getTrue(llvm_));
        });

    // Current: load value of current_node and return by value.
    auto* curFn = getOrCreate(prefix + "Current",
        llvm::FunctionType::get(elemTy, {ptrTy()}, false),
        [&](llvm::Function* f) {
            auto* bb = llvm::BasicBlock::Create(llvm_, "entry", f);
            llvm::IRBuilder<> b(bb);
            llvm::Value* self = f->arg_begin();
            llvm::Value* cur = b.CreateLoad(
                ptrTy(), b.CreateStructGEP(stTy, self, 2));
            auto* valFn = module_->getOrInsertFunction("bz_llist_node_value_ptr",
                                                       valFT).getCallee();
            llvm::Value* vp = b.CreateCall(valFT, valFn, {cur});
            b.CreateRet(b.CreateLoad(elemTy, vp));
        });

    // GetEnumerator: a FRESH enumerator over the same list, positioned before the
    // first element (current = null, next = first node) — replays from the head.
    auto* geFn = getOrCreate(prefix + "GetEnumerator",
        llvm::FunctionType::get(ptrTy(), {ptrTy()}, false),
        [&](llvm::Function* f) {
            auto* bb = llvm::BasicBlock::Create(llvm_, "entry", f);
            llvm::IRBuilder<> b(bb);
            llvm::Value* self = f->arg_begin();
            uint64_t cloneSz = module_->getDataLayout().getTypeAllocSize(stTy);
            auto* callocFT = llvm::FunctionType::get(ptrTy(), {i64(), i64()}, false);
            auto* callocC = module_->getOrInsertFunction("calloc", callocFT)
                                .getCallee();
            llvm::Value* clone = b.CreateCall(callocFT, callocC,
                {llvm::ConstantInt::get(i64(), 1),
                 llvm::ConstantInt::get(i64(), (int64_t)cloneSz)});
            llvm::Value* vt = b.CreateLoad(ptrTy(),
                b.CreateStructGEP(stTy, self, 0));
            b.CreateStore(vt, b.CreateStructGEP(stTy, clone, 0));
            llvm::Value* lst = b.CreateLoad(ptrTy(),
                b.CreateStructGEP(stTy, self, 1));
            b.CreateStore(lst, b.CreateStructGEP(stTy, clone, 1));
            auto* firstFn = module_->getOrInsertFunction("bz_llist_first_node",
                                                         firstFT).getCallee();
            llvm::Value* first = b.CreateCall(firstFT, firstFn, {lst});
            b.CreateStore(first, b.CreateStructGEP(stTy, clone, 3));  // next
            b.CreateStore(llvm::ConstantPointerNull::get(ptrTy()),
                          b.CreateStructGEP(stTy, clone, 2));         // current
            b.CreateRet(clone);
        });

    // vtable: [0]=self-ptr sentinel, [1]=Dispose, [2]=MoveNext, [3]=Current,
    // [4]=GetEnumerator (same 5-slot shape as the array wrapper, §29.11 #943).
    std::string vtName = prefix + "vtable";
    auto* vtGV = module_->getGlobalVariable(vtName);
    if (!vtGV) {
        auto* vtTy = llvm::ArrayType::get(ptrTy(), 5);
        std::vector<llvm::Constant*> elems = {
            llvm::ConstantPointerNull::get(ptrTy()), dispFn, mnFn, curFn, geFn};
        vtGV = new llvm::GlobalVariable(*module_, vtTy, /*isConst=*/true,
            llvm::GlobalValue::InternalLinkage,
            llvm::ConstantArray::get(vtTy, elems), vtName);
        elems[0] = llvm::ConstantExpr::getPointerCast(vtGV, ptrTy());
        vtGV->setInitializer(llvm::ConstantArray::get(vtTy, elems));
    }

    // Allocate and fill the enumerator: { vtable, list, current=null, next=first }.
    uint64_t stSz = module_->getDataLayout().getTypeAllocSize(stTy);
    auto* callocFn2 = declareC("calloc", ptrTy(), {i64(), i64()});
    llvm::Value* en = builder_.CreateCall(callocFn2,
        {builder_.getInt64(1), builder_.getInt64((int64_t)stSz)}, "llenum");
    builder_.CreateStore(vtGV, builder_.CreateStructGEP(stTy, en, 0));
    builder_.CreateStore(list, builder_.CreateStructGEP(stTy, en, 1));
    builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()),
                         builder_.CreateStructGEP(stTy, en, 2));  // current
    auto* firstFn = declareC("bz_llist_first_node", ptrTy(), {ptrTy()});
    llvm::Value* first = builder_.CreateCall(firstFn, {list}, "ll.first");
    builder_.CreateStore(first, builder_.CreateStructGEP(stTy, en, 3));  // next
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
        // §29.11 (#943): { ptr vtable, ptr enumerator, ptr source } — the source
        // object is retained so GetEnumerator (slot 4) can re-run the source's own
        // GetEnumerator() to hand out an independent enumerator on re-iteration.
        stTy = llvm::StructType::create(llvm_, {ptrTy(), ptrTy(), ptrTy()}, stName);

    // Resolve the source class's GetEnumerator LLVM function so the clone can call
    // it directly with the retained source as `this`. If unavailable, the clone
    // falls back to returning a fresh wrapper around the SAME inner enumerator —
    // which is still a distinct struct (so for-in's enumerator disposal stays
    // independent of the source's own scope-end disposal), preserving soundness
    // even when true re-iteration cannot be realized.
    llvm::Function* srcGetEnumFn = nullptr;
    {
        auto geIt = methodFns_.find(getEnumSym);
        if (geIt != methodFns_.end()) srcGetEnumFn = geIt->second;
    }

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

    // §29.11 (#943): GetEnumerator(self) → a FRESH independent wrapper enumerator.
    // Re-run the source class's GetEnumerator() on the retained source object to
    // obtain a brand-new inner enumerator positioned before the first element, and
    // wrap it in a fresh struct. A second `for` loop over the same IEnumerable<T>
    // value therefore replays from the start. The source is disposed once at its
    // own scope end; each loop disposes only its own wrapper+inner enumerator.
    llvm::Function* wrapGetEnumFn = getOrCreate(prefix + "GetEnumerator",
        llvm::FunctionType::get(ptrTy(), {ptrTy()}, false),
        [&](llvm::Function* f) {
            auto* bb = llvm::BasicBlock::Create(llvm_, "entry", f);
            llvm::IRBuilder<> b(bb);
            llvm::Value* self = f->arg_begin();
            uint64_t cloneSz = module_->getDataLayout().getTypeAllocSize(stTy);
            auto* callocFT = llvm::FunctionType::get(ptrTy(), {i64(), i64()}, false);
            auto* callocC = module_->getOrInsertFunction("calloc", callocFT).getCallee();
            llvm::Value* clone = b.CreateCall(callocFT, callocC,
                {llvm::ConstantInt::get(i64(), 1),
                 llvm::ConstantInt::get(i64(), (int64_t)cloneSz)});
            // Copy vtable (slot 0) and source object (slot 2).
            llvm::Value* vt = b.CreateLoad(ptrTy(), b.CreateStructGEP(stTy, self, 0));
            b.CreateStore(vt, b.CreateStructGEP(stTy, clone, 0));
            llvm::Value* srcObj = b.CreateLoad(ptrTy(), b.CreateStructGEP(stTy, self, 2));
            b.CreateStore(srcObj, b.CreateStructGEP(stTy, clone, 2));
            // Fresh inner enumerator: call the source's GetEnumerator(srcObj).
            llvm::Value* innerEnum;
            if (srcGetEnumFn)
                innerEnum = b.CreateCall(srcGetEnumFn->getFunctionType(),
                                         srcGetEnumFn, {srcObj});
            else  // fallback: reuse the existing inner enumerator (still distinct
                  // wrapper struct → disposal stays independent of the source).
                innerEnum = b.CreateLoad(ptrTy(), b.CreateStructGEP(stTy, self, 1));
            b.CreateStore(innerEnum, b.CreateStructGEP(stTy, clone, 1));
            b.CreateRet(clone);
        });

    // 8. Build vtable global (§29.11 #943: 5 slots — slot 4 = GetEnumerator).
    std::string vtName = prefix + "vtable";
    auto* vtGV = module_->getGlobalVariable(vtName);
    if (!vtGV) {
        auto* vtTy = llvm::ArrayType::get(ptrTy(), 5);
        std::vector<llvm::Constant*> elems = {
            llvm::ConstantPointerNull::get(ptrTy()),  // slot 0: filled below
            wrapDisposeFn,
            wrapMoveNextFn,
            wrapCurrentFn,
            wrapGetEnumFn
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

    // 10. Allocate wrapper struct and fill fields (vtable, enumerator, source).
    uint64_t stSz = module_->getDataLayout().getTypeAllocSize(stTy);
    auto* callocFn2 = declareC("calloc", ptrTy(), {i64(), i64()});
    llvm::Value* wrap = builder_.CreateCall(callocFn2,
        {builder_.getInt64(1), builder_.getInt64(stSz)}, "userenumenum");
    builder_.CreateStore(vtGV, builder_.CreateStructGEP(stTy, wrap, 0));
    builder_.CreateStore(enumPtr, builder_.CreateStructGEP(stTy, wrap, 1));
    builder_.CreateStore(src, builder_.CreateStructGEP(stTy, wrap, 2));
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
    // §28.3 (#993): an OWNED class temporary produced by a `new` argument (e.g.
    // `new Cell()` in `C.Use(new Cell(), C.Throw())`) is evaluated EAGERLY, in
    // source order, before the call runs. If a LATER argument's evaluation throws
    // (propagates), control leaves the argument list WITHOUT invoking the callee,
    // so any already-evaluated owned temporary in an earlier slot is orphaned and
    // — having no owning local/field/slot and no disposal registration — leaks.
    // The spec's exception-path discharge (§28.3 line 4968; the same principle the
    // object-initializer and collection-literal rules cite at lines 3829/2166)
    // requires that already-evaluated owned temporary to be auto-disposed as the
    // exception unwinds. We register each such temporary as an `unwindOnly`
    // disposal in the CURRENT scope (the disposeStack_ scope a later argument's
    // emitPendingCheck unwinds through), then DISCHARGE exactly those entries —
    // tracked by their spill slots in `argTempSlots` — once every argument is
    // evaluated and the call is about to run. The `unwindOnly` flag means these
    // entries fire ONLY on the exception edge (never on normal exit), and the
    // discharge removes them before the call, so the callee's normal-path ownership
    // of the transferred temporary is never double-disposed (dispose-exactly-once,
    // §28.3). The discharge erases ONLY these tracked entries — other disposals
    // pushed onto the same scope during argument evaluation (notably the borrow
    // collection-literal-arg temp at the top of this loop, which must SURVIVE the
    // call to be disposed at the enclosing scope's end) are left intact.
    std::vector<llvm::AllocaInst*> argTempSlots;
    for (const auto& a : args) {
        std::size_t idx = params.size();
        if (a.name.empty()) {
            idx = pos++;
        } else {
            for (std::size_t j = 0; j < params.size(); ++j)
                if (params[j].name == a.name) { idx = j; break; }
        }
        if (idx >= params.size() || ordered[idx]) return false;
        // §22.4 (#1301): an `out _` / `ref _` discard has no destination lvalue —
        // the parser leaves a.value null with a.isDiscard set. Allocate a
        // throwaway scratch slot of the parameter's type and pass its address so
        // the callee has a valid by-ref destination to write; the written value
        // is never read back ("the receiving slot is internal to the compiler
        // and is not observable", §22.4). This MUST precede the `!a.value` guard
        // below — otherwise that guard aborted the whole call, which then lowered
        // to null and the method's RETURN value was silently dropped (rc 0).
        if (a.isDiscard) {
            const sema::TypeRef* pt = params[idx].type
                ? ctx_.resolvedType(*params[idx].type) : nullptr;
            llvm::Type* slotTy = pt ? lowerTypeRef(pt) : nullptr;
            if (!slotTy) slotTy = i32();
            ordered[idx] = entryAlloca(slotTy);
            orderedExprs[idx] = nullptr;
            continue;
        }
        if (!a.value) return false;
        // CG50: a ref/out parameter receives the address of its argument
        // lvalue; an ordinary parameter receives the argument's value.
        // §22.4: `out var x` — ensureOutVarSlots already allocated the
        // slot; lvalueAddr will find it via locals_ lookup.
        llvm::Value* v;
        if (isByRefMod(params[idx].modifier)) {
            llvm::Type* et = nullptr;
            v = lvalueAddr(*a.value, et);
        } else if (auto* cl = dynamic_cast<const ast::CollectionLiteralExpr*>(
                       a.value.get())) {
            // #999 / §15.4: a collection literal passed inline as a call argument.
            // §15.4 admits a collection literal "via the parameter type of a
            // method call", so this is normative syntax that must CONSTRUCT the
            // collection — a CollectionLiteralExpr is not a general expression
            // value (lowerExpr returns null for it), and the call-argument path
            // had no dedicated materialization, so the literal previously lowered
            // to null/garbage (an empty collection for ordinary methods; a
            // dereferenced garbage pointer → crash for the byte-array FFI
            // factories). Materialize it into an owned temporary of the
            // parameter's target type via the SAME construction machinery the
            // local-decl / field-init paths use, then dispose the temp per the
            // parameter's mode (§28.3): an `owned` parameter takes ownership
            // (no caller disposal); a borrow parameter borrows it for the call
            // duration, so the temp is an owned local disposed exactly once at
            // scope exit (on the normal AND unwind paths via disposeStack_).
            const sema::TypeRef* pt =
                params[idx].type ? ctx_.resolvedType(*params[idx].type) : nullptr;
            // fix #1382 / §15.4: an INLINE-ARRAY parameter `[N]T` receiving a
            // collection literal `[e1, …, eN]`. materializeCollectionLiteralForArg
            // only handles heap collections; for a value-type inline array it
            // returns nullptr and the old code bailed out, leaving the body
            // record's local variable slot uninitialised (garbage read-back).
            // Mirror the positional-record constructor path: materialise
            // element-by-element into a stack alloca and load the aggregate.
            // Inline arrays are value types — no disposal registration needed.
            if (pt && pt->kind == sema::TypeRef::Kind::InlineArray && !cl->isDict) {
                llvm::Type* arrLlvmTy = lowerTypeRef(pt);
                if (auto* aty = llvm::dyn_cast<llvm::ArrayType>(arrLlvmTy)) {
                    llvm::AllocaInst* aslot = entryAlloca(aty);
                    builder_.CreateStore(
                        llvm::ConstantAggregateZero::get(aty), aslot);
                    llvm::Type* elTy = aty->getElementType();
                    for (std::size_t k = 0;
                         k < cl->elements.size() &&
                         k < aty->getNumElements(); ++k) {
                        if (!cl->elements[k]) continue;
                        llvm::Value* ev = lowerExpr(*cl->elements[k]);
                        if (!ev) {
                            // #1386: ICE — lowerExpr returned null for a
                            // non-null inline-array element in buildOrderedArgs;
                            // the expression kind is not yet handled by codegen.
                            ctx_.diagnostics().report(
                                Severity::Error,
                                cl->elements[k]->span.begin,
                                "internal: lowerExpr returned null for "
                                "inline-array element in call argument — #1386");
                            continue;
                        }
                        if (ev->getType() != elTy &&
                            ev->getType()->isIntegerTy() &&
                            elTy->isIntegerTy()) {
                            unsigned eb = ev->getType()->getIntegerBitWidth();
                            unsigned db = elTy->getIntegerBitWidth();
                            ev = db > eb ? builder_.CreateSExt(ev, elTy)
                                         : builder_.CreateTrunc(ev, elTy);
                        }
                        builder_.CreateStore(
                            coerceToNullableSlot(elTy, ev),
                            builder_.CreateGEP(
                                aty, aslot,
                                {builder_.getInt32(0),
                                 builder_.getInt32(static_cast<unsigned>(k))}));
                    }
                    v = builder_.CreateLoad(aty, aslot);
                }
            } else {
                llvm::Value* coll =
                    pt ? materializeCollectionLiteralForArg(cl, pt) : nullptr;
                if (!coll) return false;  // unrecognised target — decline cleanly
                // Spill the collection pointer to a slot (the disposal entry frees
                // through the alloca, mirroring a `using var` local's slot).
                llvm::AllocaInst* slot = entryAlloca(coll->getType());
                builder_.CreateStore(coll, slot);
                v = coll;
                bool ownedParam = (params[idx].modifier == "owned");
                if (!ownedParam && !disposeStack_.empty())
                    disposeStack_.back().push_back(
                        buildCollectionTempDisposal(slot, pt));
            }
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
        if (!v) {
            // #1386: ICE — lowerExpr returned null for a call argument value;
            // the expression kind is not yet handled by codegen.
            ctx_.diagnostics().report(
                Severity::Error, a.value->span.begin,
                "internal: lowerExpr returned null for call argument in "
                "buildOrderedArgs — #1386");
            return false;
        }
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
            } else if (formalTy) {
                // per §7.4 / §12.7 (#1191): a value-type nullable local that sema
                // proved non-null by null-flow narrowing is accepted as a
                // NON-nullable argument (checkAssignable now treats the narrowed
                // `from` as non-nullable in argument position too). Sema kept its
                // declared {flag,value} layout, so unwrap the tagged-optional to
                // the parameter's bare value here — mirroring the unwrap local-init
                // / return / assignment already apply. A no-op when `v` is not that
                // struct (reference args and already-scalar values pass through).
                v = unwrapNarrowedNullable(v, lowerTypeRef(formalTy));
            }
            // §8.6: a value-typed argument bound to an Object/interface
            // parameter is boxed, so a later `is`/`as` or virtual dispatch on
            // that parameter (which lowers to a descriptor walk through the
            // object's first word) sees a real box rather than the raw value
            // bytes. boxForTarget is a no-op for reference arguments (#399).
            v = boxForTarget(v, formalTy, a.value.get());
            // §29.2 (#437/#1067/#1068/#552): when an Array<T> / (Owned)LinkedList
            // / user class is passed to a formal `IEnumerable<T>` parameter, wrap
            // it in the vtable-backed enumerator so the callee's vtable dispatch
            // does not misread a raw bz_array_t* / class instance (SIGSEGV). The
            // wrapper SHARES the source (borrowed), so an owning collection still
            // disposes its elements once at its own binder's scope end. Only the
            // pure `IEnumerable<T>` is wrapped — richer interfaces keep the
            // array-backed protocol. Shared with the local-decl init and the
            // method-RETURN coercion (#1310).
            v = wrapAsIEnumerableForTarget(v, formalTy, a.value.get());
        }
        ordered[idx] = v;
        orderedExprs[idx] = a.value.get();
        // §28.3 (#993): if THIS argument hands an owned class temporary to the
        // call, register a transient unwind-only disposal so a LATER argument that
        // throws disposes this orphaned temporary. Two forms produce such a
        // temporary whose ONLY remaining owner is the in-flight call:
        //   (a) `new T(...)` — a sourceless owned heap instance (§28.8); nothing
        //       else owns or disposes it.
        //   (b) `move x` / `move x.f` — the source local/field is moved-from, so
        //       its own scope-end / owned-field disposal is suppressed (the move
        //       lowering nulls a using-var / owned-field slot, and the
        //       UnwindIneligibleWalker marks a moved-from plain `var`/param
        //       unwind-ineligible). The moved-out value then lives only in this
        //       argument slot with no disposal registration.
        // In both cases this is the SOLE site that disposes the value on the unwind
        // edge (no double-free); on the normal path the entry is discharged below
        // before the call consumes the value. Restricted to a CLASS static type (a
        // heap instance with a disposal obligation) and a non-null pointer value —
        // a record/value-type, a move-only inline array, or a null literal is not a
        // heap-owned class instance and is excluded.
        const ast::Expr* av = a.value.get();
        bool isOwnedTempArg = false;
        if (dynamic_cast<const ast::NewExpr*>(av)) {
            isOwnedTempArg = true;
        } else if (auto* un = dynamic_cast<const ast::UnaryExpr*>(av)) {
            // `move x` / `move x.f` of an owned reference (not a value-type move).
            if (un->op == "move") isOwnedTempArg = true;
        } else if (auto* be = dynamic_cast<const ast::BinaryExpr*>(av)) {
            // #1019: an owned-returning operator result (`a + b` with
            // `operator +(...): owned Cell`) is a fresh owned class temporary too.
            if (auto* sym = ctx_.exprSymbol(*be))
                if (auto* ms = dynamic_cast<sema::MethodSymbol*>(sym))
                    if (auto* od = dynamic_cast<const ast::OperatorDecl*>(ms->decl))
                        if (od->returnOwned) isOwnedTempArg = true;
        } else if (auto* ce = dynamic_cast<const ast::CallExpr*>(av)) {
            // #1019: a call to a user method declared `owned`-return produces a
            // fresh owned class temporary (`f(Make())` with `Make(): owned Cell`).
            if (ce->callee)
                if (auto* sym = ctx_.exprSymbol(*ce->callee))
                    if (auto* ms = dynamic_cast<sema::MethodSymbol*>(sym))
                        if (auto* md = dynamic_cast<const ast::MethodDecl*>(ms->decl))
                            if (md->returnOwned) isOwnedTempArg = true;
        }
        if (isOwnedTempArg && !disposeStack_.empty() &&
            v->getType()->isPointerTy() &&
            !llvm::isa<llvm::ConstantPointerNull>(v)) {
            const sema::TypeRef* at = ctx_.exprType(*av);
            bool isClassInstance = at &&
                at->kind == sema::TypeRef::Kind::Named && at->declaration &&
                at->declaration->kind == ast::TypeDeclKind::Class;
            if (isClassInstance) {
                llvm::AllocaInst* tslot = entryAlloca(v->getType());
                builder_.CreateStore(v, tslot);
                Disposal d{tslot, nullptr};
                if (params[idx].modifier == "owned") {
                    // #993: an `owned` parameter takes ownership on the normal
                    // path, so this entry is unwind-ONLY (fires only if a LATER
                    // argument throws) and is DISCHARGED below before the call —
                    // the callee then disposes it (dispose-exactly-once).
                    d.unwindOnly = true;
                    d.dynamicDispose = false;
                    disposeStack_.back().push_back(d);
                    argTempSlots.push_back(tslot);
                } else {
                    // #1019: a BORROW parameter does NOT take ownership — the fresh
                    // owned temporary is borrowed for the call duration and must be
                    // disposed at scope end on BOTH the normal and unwind edges. It
                    // is NOT added to argTempSlots, so the pre-call discharge leaves
                    // it intact; dynamicDispose dispatches Dispose+reclaim through
                    // the object's own vtable, null-guarded. Only a FRESH producer
                    // (new / move / owned operator / owned call) reaches here — a
                    // bound-local / field / param borrow argument is not one of
                    // these shapes, so it is never disposed (no double-free / UAF).
                    d.dynamicDispose = true;
                    disposeStack_.back().push_back(d);
                }
            }
        }
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
            // fix #1163 / §22.5 / §17.11: a `default(T)` default-value
            // expression is reached through the PARAMETER declaration, whose
            // inner type node (the `T` of `default(T)`) the type-resolution pass
            // does NOT walk — it resolves only the parameter's own `: Type`
            // annotation (TypeResolution_Names.cpp resolves `p.type`, not
            // `p.defaultValue`). So lowering the DefaultExpr the ordinary way
            // calls `lowerType(de->type)` on an UNRESOLVED TypeRef, which falls
            // back to the i32 placeholder; a reference / nullable-ref
            // `default(R?)` then becomes a bare `i32 0` passed to a `ptr`
            // parameter ("Call parameter type does not match function
            // signature"). Lower the omitted-argument `default(T)` at the
            // PARAMETER's OWN lowered type, which IS resolved (resolveType on
            // `p.type` above): `ptr null` for a reference / nullable-ref, the
            // tagged {i8,T} zero for a nullable value, and 0 / 0.0 / false for a
            // value type — exactly the zero value `default(T)` denotes, already
            // in the parameter's representation.
            if (dynamic_cast<const ast::DefaultExpr*>(dv)) {
                ordered[j] = llvm::Constant::getNullValue(
                    lowerType(params[j].type.get()));
            } else {
                ordered[j] = lowerExpr(*dv);
                // fix #1235 / §22.5 / §22.7: a bare `null` literal default
                // for a nullable VALUE param (int?, enum?, nullable record)
                // lowers as ptr null but the param's LLVM type is {i8,T}.
                // Apply the same coercion the explicit-arg path uses
                // (CG-nullable, see L3523): ptr null → nullableValueNull.
                if (ordered[j] && params[j].type) {
                    sema::TypeRef* formalTy =
                        ctx_.resolvedType(*params[j].type);
                    if (isNullableValueTy(formalTy)) {
                        auto* expectedSt = nullableValueStructTy(formalTy);
                        if (ordered[j]->getType() != expectedSt) {
                            if (ordered[j]->getType()->isPointerTy() &&
                                llvm::isa<llvm::ConstantPointerNull>(
                                    ordered[j])) {
                                ordered[j] = nullableValueNull(formalTy);
                            } else if (ordered[j]->getType() ==
                                       expectedSt->getElementType(1)) {
                                ordered[j] =
                                    wrapNullableValue(formalTy, ordered[j]);
                            }
                        }
                    }
                }
            }
            paramSlots_ = std::move(savedParams);
            if (!ordered[j]) return false;
            // A defaulted parameter has no call-site argument expression;
            // record the default's own expression so its signedness is honored.
            orderedExprs[j] = dv;
        }
        out.push_back(ordered[j]);
        if (outExprs) outExprs->push_back(orderedExprs[j]);
    }
    // §28.3 (#993): all arguments (and any default-value expressions) evaluated
    // successfully — the call is about to run and will take ownership of any
    // transferred owned temporaries on the normal path. Discharge the transient
    // unwind-only disposals registered for owned `new`/`move` argument temporaries
    // by erasing exactly those entries (matched by their spill slots), so the
    // normal path never double-disposes them. They were `unwindOnly`, so they never
    // fire on a normal exit; a later argument (or default) that threw already
    // unwound through them. Other disposals pushed during argument evaluation (e.g.
    // a borrow collection-literal-arg temp) are NOT removed — they must survive the
    // call and dispose at the enclosing scope's end.
    if (!argTempSlots.empty() && !disposeStack_.empty()) {
        auto& scope = disposeStack_.back();
        scope.erase(
            std::remove_if(scope.begin(), scope.end(),
                           [&](const Disposal& d) {
                               return d.unwindOnly &&
                                      std::find(argTempSlots.begin(),
                                                argTempSlots.end(),
                                                d.slot) != argTempSlots.end();
                           }),
            scope.end());
    }
    return true;
}


}  // namespace bezel::codegen
