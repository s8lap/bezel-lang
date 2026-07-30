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

// fix #2414 (§22.10/§23.4/§2.8.2 clause 1/§2.8.3 rule 4, v0.235): an
// indexer's k-th index-parameter TYPE / total ARITY, resolved from either
// its LOCAL ast::PropertyDecl (`ps->decl`) or, for an IMPORTED indexer (no
// ast::PropertyDecl to read — ModuleLoader.cpp's Indexer case reconstructs
// `indexerParams` as ParameterSymbols carrying `resolvedDeclType` instead),
// the reconstructed ParameterSymbol directly. Every consumer below
// (findIndexer, selectIndexer, and lowerIndexerGet/-Set's signature-building)
// goes through these two functions instead of reading `pd` directly, so a
// LOCAL and an IMPORTED indexer are selected/typed identically — before this
// fix, every one of these sites unconditionally required `pd`, silently
// excluding every imported indexer candidate from overload resolution
// (confirmed: `selectIndexer` returned null for any imported class's
// indexer, well before codegen even reached the getter/setter dispatch).
// Mirrors the identically-shaped fallback added to
// ExprTypecheck_Exprs.cpp's sema-side overload matcher.
const sema::TypeRef* indexerParamType(sema::SemaContext& ctx,
                                      const sema::PropertySymbol* ps,
                                      std::size_t k) {
    if (auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl)) {
        return (k < pd->indexerParams.size() && pd->indexerParams[k].type)
                   ? ctx.resolvedType(*pd->indexerParams[k].type)
                   : nullptr;
    }
    if (k < ps->indexerParams.size()) {
        if (auto* p =
                dynamic_cast<sema::ParameterSymbol*>(ps->indexerParams[k]))
            return p->resolvedDeclType;
    }
    return nullptr;
}

std::size_t indexerParamCount(const sema::PropertySymbol* ps) {
    if (auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl))
        return pd->indexerParams.size();
    return ps->indexerParams.size();
}
}  // namespace

// fix #2437 (§11.2/§23.4, v0.235): find `ts`'s indexer whose FULL parameter
// list — arity AND every parameter type (§23.4: "Indexers may be overloaded
// by parameter type list") — matches `ifaceIdx`'s. The prior match compared
// ONLY the first index parameter's type, so an interface obligation
// `[i:int,j:int]` matched a class's narrower `[i:int]` overload whenever the
// two shared a first parameter type — confirmed: installed the WRONG getter
// into the itable slot (the issue's repro returned 77, the `[i:int]`
// getter's constant, instead of 17 from `[i:int,j:int]`'s `5+i+j`), silently,
// no diagnostic.
//
// There is also no more "fall back to the first indexer found when nothing
// matches exactly": that fallback was itself an UNBOUNDED version of the
// same arity-blind defect (it would accept ANY indexer of ANY shape, once no
// exact match existed, as if it implemented the obligation) — installing a
// getter/setter whose actual parameter types differ from what an interface
// caller passes is itable-level type confusion, strictly worse than a null
// slot (which #2438's sema-side mirror already turns into a compile-time
// reject rather than letting a mismatched candidate reach codegen at all).
//
// Returns null when no member of `ts` matches by full parameter list.
// `checkAbstractImplementations`'s ifaceIndexerObligations loop (sema,
// TypeResolution_Checks.cpp) deliberately mirrors this exact selection — see
// that function's comment for why the two must move together — so a null
// return here always corresponds to a program sema has already rejected,
// never a silently-wrong itable install.
//
// Scoped to `ts->members` only (no base-class walk): a class satisfying the
// obligation only through an INHERITED indexer is a separate, independent
// defect (`findIndexer` never calls `baseClassOf`, unlike its named-property
// sibling `classPropertyByName`) — filed as #2442, not attempted here.
const sema::PropertySymbol* Emitter::findIndexer(
    const sema::TypeSymbol* ts, const sema::PropertySymbol* ifaceIdx) const {
    if (!ts || !ifaceIdx) return nullptr;
    const std::size_t nparams = indexerParamCount(ifaceIdx);
    for (const sema::Symbol* m : ts->members) {
        auto* ps = dynamic_cast<const sema::PropertySymbol*>(m);
        if (!ps || !ps->isIndexer) continue;
        if (indexerParamCount(ps) != nparams) continue;
        bool all = true;
        for (std::size_t k = 0; k < nparams; ++k) {
            if (!indexerArgMatches(indexerParamType(ctx_, ps, k),
                                   indexerParamType(ctx_, ifaceIdx, k))) {
                all = false;
                break;
            }
        }
        if (all) return ps;  // §11.2/§23.4: full parameter-list match
    }
    return nullptr;
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
            if (indexerParamCount(ps) != nargs) continue;
            if (!arityOnly) arityOnly = ps;
            bool all = true;
            for (std::size_t k = 0; k < nargs; ++k) {
                const sema::TypeRef* pty = indexerParamType(ctx_, ps, k);
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
        const sema::TypeRef* arrEl2 =
            (!recvTy->genericArgs.empty()) ? recvTy->genericArgs[0] : nullptr;
        bool arrIsOwning2 =
            recvTy->astNode && recvTy->astNode->name &&
            !recvTy->astNode->name->parts.empty() &&
            common::isOwningPrefixedName(recvTy->headName());
        {
            llvm::Function* elemDisp =
                elementPolicyFor(arrEl2, arrIsOwning2).disposerThunk;
            if (!elemDisp && listIfaceOwning) elemDisp = elementDisposerThunk();
            if (elemDisp) {
                auto* fn = declareC("bz_array_set_disposing",
                    llvm::Type::getVoidTy(llvm_),
                    {ptrTy(), i64(), ptrTy(), i64(), ptrTy()});
                builder_.CreateCall(fn, {recv, idx, slot, builder_.getInt64(esz),
                                         elemDisp});
                // fix #1970 / §28.4 / §28.6: OwningArray<T>[i] := owned v (and
                // any IOwningList<T>[i] := owned v via the §4.2 path) has just
                // transferred ownership of `v` into the array slot. Null the
                // caller's slot for a bare owned local/param RHS so the caller's
                // null-guarded unwindOnly disposal — which
                // UnwindIneligibleWalker now leaves eligible for this
                // AssignmentStmt shape (see indexerSetEligibleSource in
                // LlvmBackend_Setup.cpp's UnwindIneligibleWalker) — is
                // exactly-once: disposes a value still live at a throw that
                // PRECEDES the indexer-set (fixes the #1970 leak); skips the
                // already-stored value at the post-set throw edge (no double-
                // free with the array's slot-owner element disposal on the
                // displaced-prior branch). Mirrors the OwningArray.Add / Push /
                // Enqueue / Insert nulling paths.
                nullTransferredOwnedLocalSlot(valueExpr);
                return true;
            }
        }
        auto* fn = declareC("bz_array_set", llvm::Type::getVoidTy(llvm_),
                            {ptrTy(), i64(), ptrTy(), i64()});
        builder_.CreateCall(fn, {recv, idx, slot, builder_.getInt64(esz)});
        // fix #2204 / §28.3 Unbound owned temporaries / §28.11 "Storing a
        // borrow": the SAME fresh-producer argument leak the #1881 Add/Push/
        // Enqueue fix disposes exists here for the borrowing (no-disposer)
        // branch of the Array<T> indexer-set `arr[i] := rhs` — an owned
        // TEMPORARY (`new R(1)`) is a non-owned RHS to a borrow-taking set,
        // transfers no ownership (§28.3's "non-owned argument"), and under the
        // pre-fix codegen was never disposed: dtor never ran, LSan reported 16B
        // leaked. Per §28.3 v0.226 the assignment-statement end IS the full-
        // expression boundary for this RHS (the indexer-set is a statement-
        // shaped store, not a chain-off expression — nothing downstream can
        // reuse the value). Dispose a fresh-producer RHS (isFreshOwnedClass-
        // Producer — the same discriminant the #1039/#1053 receiver-temp and
        // #1881 Add/Push/Enqueue/Insert fixes use) right after the
        // bz_array_set call, gated on the receiving collection being BORROWING
        // (the owning branch fell through above into the disposing setter +
        // nullTransferredOwnedLocalSlot path, which already handles the
        // transfer; mutually exclusive). This intentionally creates the same
        // "borrow-lifetime hazard that checked disposal mode backstops" the
        // spec names: the copy just stored in `recv` dangles from this point
        // on, backstopped by DisposedAccessException in checked-disposal mode,
        // undefined behavior (by design) unchecked — the same accepted
        // residual as every other borrowing-collection store (§28.11).
        if (!arrIsOwning2 && isFreshOwnedClassProducer(valueExpr))
            emitDisposeAndReclaim(val);
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
            // fix #1970 / §28.4 / §28.6: OwningDictionary<K,V>[k] := owned v
            // has just transferred ownership of `v` (the disposed prior value
            // for an existing key is handled by the runtime's disposing setter;
            // for a fresh key the new value is stored). Null the caller's slot
            // for a bare owned local/param RHS so the caller's null-guarded
            // unwindOnly disposal — which UnwindIneligibleWalker now leaves
            // eligible for this AssignmentStmt shape (indexerSetEligibleSource
            // for OwningDictionary) — is exactly-once: disposes a value still
            // live at a throw that PRECEDES the indexer-set (fixes the #1970
            // leak); skips the just-stored value at the post-set throw edge
            // (no double-free with the dict's table-disposal of the displaced
            // prior element). Mirrors the OwningDictionary.Add / Insert /
            // OwningArray.Add nulling paths.
            if (dictIsOwning2)
                nullTransferredOwnedLocalSlot(valueExpr);
        } else {
            auto* fn = declareC("bz_dict_set_gen", llvm::Type::getVoidTy(llvm_),
                                {ptrTy(), ptrTy(), ptrTy()});
            builder_.CreateCall(fn, {recv, keyPtr, vPtr});
            // fix #2204 / §28.3 Unbound owned temporaries / §28.11 "Storing a
            // borrow": the SAME fresh-producer argument leak the #1881 Add/Push/
            // Enqueue fix disposes exists here for a BORROWING Dictionary<K,V>'s
            // indexer-set `d[k] := rhs` (gen-key, no disposer) — an owned
            // TEMPORARY (`new R(1)`) is a non-owned RHS to a borrow-taking set,
            // transfers no ownership (§28.3's "non-owned argument"), and under
            // the pre-fix codegen was never disposed: dtor never ran, LSan
            // reported 16B leaked. Per §28.3 v0.226 the assignment-statement end
            // IS the full-expression boundary for this RHS (the indexer-set is
            // a statement-shaped store, not a chain-off expression). Dispose a
            // fresh-producer RHS (isFreshOwnedClassProducer — the same
            // discriminant the #1039/#1053 receiver-temp and #1881 Add/Push/
            // Enqueue/Insert fixes use) right after the bz_dict_set_gen call,
            // gated on the receiving collection being BORROWING (`!dictIsOwning2`;
            // an OwningDictionary takes ownership via the disposing setter +
            // nullTransferredOwnedLocalSlot path above — mutually exclusive).
            // The fresh-producer gate IS the class-type check: isFreshOwnedClass-
            // Producer only returns true for class-typed fresh producers, so a
            // record (the dict's value is heap-boxed — `vPtr` is a separately-
            // owned box, not the source `val`) or a string (the dict retains
            // a +1, transferring the borrow into a co-owned +1) is NEVER
            // matched — no spurious double-free of the boxed record or the
            // transferred string +1. This intentionally creates the same
            // "borrow-lifetime hazard that checked disposal mode backstops"
            // the spec names: the pointer just stored in the dict dangles
            // from this point on, backstopped by DisposedAccessException in
            // checked-disposal mode, undefined behavior (by design) unchecked
            // — the same accepted residual as every other borrowing-collection
            // store (§28.11).
            if (!dictIsOwning2 && isFreshOwnedClassProducer(valueExpr))
                emitDisposeAndReclaim(val);
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
        // fix #1970 / §28.4 / §28.6: see the gen-key branch above — for an
        // OwningDictionary<K,V> with string key the value has just been
        // transferred; null the caller's slot for a bare owned local/param RHS.
        if (dictIsOwning2)
            nullTransferredOwnedLocalSlot(valueExpr);
    } else {
        auto* fn = declareC("bz_dict_set_str", llvm::Type::getVoidTy(llvm_),
                            {ptrTy(), ptrTy(), i64(), ptrTy()});
        builder_.CreateCall(fn, {recv, kd, kl, vPtr});
        // fix #2204 / §28.3 Unbound owned temporaries / §28.11 "Storing a
        // borrow": the borrowing Dictionary<K,V> string-key branch of the
        // indexer-set `d[k] := rhs` — identical to the gen-key branch above
        // except the key is a string. The value side is the same: a fresh-
        // producer class RHS is a non-owned argument to a borrow-taking set,
        // transfers no ownership, and under the pre-fix codegen was never
        // disposed. Dispose it right after the bz_dict_set_str call, gated on
        // the receiving collection being BORROWING (the owning branch fell
        // through above into the disposing setter + nullTransferredOwnedLocal-
        // Slot path, which already handles the transfer; mutually exclusive).
        // Same isFreshOwnedClassProducer gate as the gen-key branch — the
        // class-only gate excludes records (heap-boxed, vPtr is a separately
        // owned box) and strings (the dict retains a +1, transferring the
        // borrow into a co-owned +1) so no spurious double-free.
        if (!dictIsOwning2 && isFreshOwnedClassProducer(valueExpr))
            emitDisposeAndReclaim(val);
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
    // fix #2414 (§22.10/§23.4/§2.8.2 clause 1/§2.8.3 rule 4, v0.235): an
    // IMPORTED indexer has no ast::PropertyDecl (mirrors Property's own
    // #503 limitation — ModuleLoader.cpp's Indexer case reconstructs
    // `indexerParams` from the side tables, not an AST node). The
    // itable/interface and vtable/virtual dispatch paths below both still
    // read `pd->type`/`pd->indexerParams` for signature info, and the
    // final inline-body path genuinely needs the body AST — none of those
    // exist for an imported symbol, so they are left requiring `pd` (the
    // itable/vtable-dispatched imported-indexer shape is explicitly out of
    // scope for this fix; see xmod_indexer_import_2414_test.cpp's T4
    // accept-control test and file-header "SCOPE" note). The remaining,
    // and most common, case — a NON-VIRTUAL indexer on a CLASS-typed
    // receiver (the issue's own confirmed repro) — dispatches through the
    // externally-declared `bz$<Class>$idxget$<arity>` function
    // declareImportedIndexerGetters installs, using the reconstructed
    // side-table types (the callee's own LLVM signature) instead of `pd`.
    if (!pd) {
        if (recvDecl->kind != ast::TypeDeclKind::Class) return nullptr;
        auto git = importedIndexerGetterFns_.find(ps);
        if (git == importedIndexerGetterFns_.end()) return nullptr;
        llvm::Function* fn = git->second;
        llvm::Value* obj = lowerExpr(*ix.target);
        if (!obj || !obj->getType()->isPointerTy()) return nullptr;
        auto* fnTy = fn->getFunctionType();
        std::vector<llvm::Value*> callArgs;
        callArgs.push_back(obj);
        for (std::size_t k = 0; k < ps->indexerParams.size() &&
             k < ix.indices.size() &&
             k + 1 < fnTy->getNumParams(); ++k) {
            if (!ix.indices[k].value) return nullptr;
            llvm::Value* argVal = lowerExpr(*ix.indices[k].value);
            if (!argVal) return nullptr;
            callArgs.push_back(
                coerceNumericToType(argVal, fnTy->getParamType(k + 1), false));
        }
        llvm::Value* r = builder_.CreateCall(fnTy, fn, callArgs);
        emitPendingCheck();
        return r;
    }
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
    // fix #2414: the SETTER twin of lowerIndexerGet's decl-null fallback
    // immediately above — see its comment for the full rationale. Dispatches
    // through the externally-declared `bz$<Class>$idxset$<arity>` function
    // declareImportedIndexerSetters installs (absent for a getter-only
    // imported indexer, in which case this correctly falls through and
    // returns false — the same "no setter to lower" outcome a local
    // getter-only indexer's `!setterExpr && !setterStmts` check produces
    // further down).
    if (!pd) {
        if (recvDecl->kind != ast::TypeDeclKind::Class) return false;
        auto sit = importedIndexerSetterFns_.find(ps);
        if (sit == importedIndexerSetterFns_.end()) return false;
        llvm::Function* fn = sit->second;
        llvm::Value* obj = lowerExpr(*ix.target);
        if (!obj || !obj->getType()->isPointerTy()) return false;
        auto* fnTy = fn->getFunctionType();
        std::vector<llvm::Value*> callArgs;
        callArgs.push_back(obj);
        for (std::size_t k = 0; k < ps->indexerParams.size() &&
             k < ix.indices.size() &&
             k + 1 < fnTy->getNumParams(); ++k) {
            if (!ix.indices[k].value) return false;
            llvm::Value* argVal = lowerExpr(*ix.indices[k].value);
            if (!argVal) return false;
            callArgs.push_back(
                coerceNumericToType(argVal, fnTy->getParamType(k + 1), false));
        }
        if (fnTy->getNumParams() == 0) return false;
        llvm::Type* valTy = fnTy->getParamType(fnTy->getNumParams() - 1);
        callArgs.push_back(coerceNumericToType(val, valTy, false));
        builder_.CreateCall(fnTy, fn, callArgs);
        emitPendingCheck();
        return true;
    }
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
        isUnsignedTargetExpr(*as.target),
        isCharTargetExpr(*as.target),      // §6.6/§5.2 #1961
        isUnsignedTargetExpr(*as.value));  // §6.2 #2140 rUns
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
        // §6.2 / §5.1 (#2161): an integer RHS narrower than the dict value
        // type widens (ZExt unsigned, SExt signed — the RHS's own signedness,
        // per §5.1's table) before emitCompoundOp; a wider RHS flows through
        // UNCHANGED so emitCompoundOp's #2140/#2147 wide branch runs the op
        // at the §6.2-promoted width and applies the §6.6/§5.2 CHECKED
        // narrow-integer write-back. The pre-fix unconditional SExt was an
        // invalid narrowing cast on a wider variable RHS (IR-verify abort:
        // "Type too small for SExt  %14 = sext i64 %13 to i32") and silently
        // truncated a wider constant RHS to a wrong in-range value. Mirrors
        // coerceNumericToType's `srcBits < targetBits` widening rule; the
        // array-indexer branch has no pre-widen and lets emitCompoundOp run
        // the math end-to-end.
        rhs = coerceNumericToType(rhs, valLlvm,
                                  isUnsignedTargetExpr(*as.value));
        llvm::Value* newVal = emitCompoundOp(
            op, cur, rhs, valLlvm->isFloatingPointTy(),
            isUnsignedTargetExpr(*as.target),
            isCharTargetExpr(*as.target),      // §6.6/§5.2 #1961
            isUnsignedTargetExpr(*as.value));  // §6.2 #2140 rUns
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
            isUnsignedTargetExpr(*as.target),
            isCharTargetExpr(*as.target),      // §6.6/§5.2 #1961
            isUnsignedTargetExpr(*as.value));  // §6.2 #2140 rUns
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
            binOp, cur, one, fp, isUnsignedTargetExpr(*inc.target),
            isCharTargetExpr(*inc.target));  // §6.6/§5.2 #1961
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
        binOp, cur, one, fp, isUnsignedTargetExpr(*inc.target),
        isCharTargetExpr(*inc.target));  // §6.6/§5.2 #1961
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
    // #2274 / §31.2: the getter is its OWN function — the caller's active
    // try/finally frames do not apply inside it, so finallyStack_ is cleared
    // below. It must therefore be SAVED and restored like loopStack_: without
    // the save, emitting a block getter from inside a `try … finally` destroyed
    // the enclosing frame, and lowerTry's own `finallyStack_.back()`/`pop_back()`
    // after the body then ran on an empty stack (assertion/UB), while a
    // `return`/`break` leaving that try emitted NO finally at all — contradicting
    // §31.2: "`finally` runs whether the protected block completed normally, was
    // exited by an exception (caught or uncaught), or was exited by `return` —
    // or by a `break` or `continue` targeting an enclosing loop."
    // finallyBodyStack_ (§29.7 #623) is documented as "always the same depth as
    // finallyStack_", so it is cleared and restored in lockstep.
    auto  savedFinally   = finallyStack_;
    auto  savedFinBodies = finallyBodyStack_;
    auto* savedUnwind    = unwindTarget_;
    auto* savedProp      = propagateBB_;
    auto* savedCaught    = currentCaughtExc_;
    auto  savedUD        = unwindScopeDepth_;
    auto  savedDiv       = diverged_;
    auto  savedGSubst    = genericSubst_;
    // #2170 / §30.5: a block-getter accessed from an async body must not
    // inherit the caller's state-machine mode — the getter's `return` is
    // ordinary (its `ret i32` terminates the getter), and the async return
    // path's gate `smKind_ == Await && smStateStruct_` (LlvmBackend_Stmts.cpp
    // ~line 1510) would otherwise emit bz_task_complete / spill-free /
    // frame-free into the getter's body. Save+null+restore.
    auto  savedSmKind    = smKind_;
    auto* savedSmStruct  = smStateStruct_;

    // --- Switch into the getter function ---
    currentFn_          = fn;
    builder_.SetInsertPoint(entryBB);
    currentThis_        = &*fn->args().begin();
    locals_.clear();
    paramSlots_.clear();
    refParams_.clear();
    disposeStack_.clear();
    loopStack_.clear(); finallyStack_.clear(); finallyBodyStack_.clear();
    unwindTarget_       = nullptr;
    propagateBB_        = nullptr;
    currentCaughtExc_   = nullptr;
    unwindScopeDepth_   = 0;
    diverged_           = false;
    smKind_             = SuspendKind::Yield;
    smStateStruct_      = nullptr;

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
    finallyStack_       = std::move(savedFinally);      // #2274 / §31.2
    finallyBodyStack_   = std::move(savedFinBodies);    // #2274 / §29.7
    unwindTarget_       = savedUnwind;
    propagateBB_        = savedProp;
    currentCaughtExc_   = savedCaught;
    unwindScopeDepth_   = savedUD;
    diverged_           = savedDiv;
    genericSubst_       = std::move(savedGSubst);
    smKind_             = savedSmKind;
    smStateStruct_      = savedSmStruct;
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
    if (!pd) {
        // fix #2387 (§2.8.2/§2.8.3 rule 4, §11.2, §23; v0.235): an IMPORTED
        // PropertySymbol carries no reconstructed ast::PropertyDecl —
        // ModuleLoader never attaches one to a body-less imported
        // declaration (the getter/setter BODY is not reconstructed there;
        // only the resolved type rides the importedMemberType side table,
        // ModuleLoader.cpp ~445). This is true whether `ps` was resolved
        // directly OR reached by walking an imported base class's chain
        // (classPropertyByName, called from the itable-building loop in
        // LlvmBackend_Setup.cpp when a LOCAL class satisfies an interface
        // obligation via a property it INHERITS from an imported base — the
        // vertical codegen partner of #2380's sema fix, which correctly
        // accepts that program). Pre-fix this unconditionally returned
        // nullptr, so the itable-building loop
        // (LlvmBackend_Setup.cpp declareVtables Pass D) left the property's
        // itable slot as ConstantPointerNull — a call through the interface
        // reference (`b.Name`) then calls address 0x0 (confirmed SIGSEGV).
        //
        // Fall back to the external accessor stub already declared for
        // ordinary cross-module property access — declareImportedPropertyGetters
        // (LlvmBackend_Setup.cpp), which the non-itable property-read path
        // already consults the same way (LlvmBackend_Exprs.cpp ~4283-4300,
        // issue #503). Its ABI (`T get(ptr this)`) is exactly the itable
        // slot's expected signature, so the itable can install it directly.
        //
        // fix #2400 (§2.8.2/§2.8.3 rule 4, §11.2, §23; v0.235): the SETTER
        // twin of the getter fallback just above. declareImportedPropertyGetters
        // used to be the ONLY stub of this shape — a settable property
        // inherited from an imported base class crashed identically on its
        // SETTER slot (confirmed by probe — a write-only repro segfaulted the
        // same way, faulting at address 0x0). declareImportedPropertySetters
        // (LlvmBackend_Setup.cpp) now declares the symmetric external stub
        // (`void set(ptr this, T value)`, matching the itable slot's expected
        // signature exactly) and populates importedPropSetterFns_; fall back
        // to it the same way the getter branch does.
        auto& importedFns = isSet ? importedPropSetterFns_ : importedPropGetterFns_;
        if (auto git = importedFns.find(ps); git != importedFns.end()) {
            propAccessorFns_[key] = git->second;
            return git->second;
        }
        return nullptr;
    }

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
    // #2170 / §30.5: an accessor (block getter/setter) built during vtable
    // emission may also be reached from an async body; the saved/restored
    // smKind_/smStateStruct_ here ensures the accessor's `return` does not
    // accidentally trip the async-complete path on its parent.
    auto  savedSmKind    = smKind_;
    auto* savedSmStruct  = smStateStruct_;

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
    smKind_             = SuspendKind::Yield;
    smStateStruct_      = nullptr;

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
    smKind_             = savedSmKind;
    smStateStruct_      = savedSmStruct;
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
    // #2274 / §31.2 / §29.7: same save/restore hole as ensurePropertyBlockGetter
    // — a block-bodied INDEXER getter read from inside a `try … finally` cleared
    // the caller's finally frames, so lowerTry's post-body `finallyStack_.back()`
    // ran on an empty stack and any `return`/`break` leaving that try emitted no
    // finally. See the note in ensurePropertyBlockGetter for the spec sentence.
    auto  savedFinally   = finallyStack_;
    auto  savedFinBodies = finallyBodyStack_;
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
    loopStack_.clear(); finallyStack_.clear(); finallyBodyStack_.clear();
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
    finallyStack_       = std::move(savedFinally);      // #2274 / §31.2
    finallyBodyStack_   = std::move(savedFinBodies);    // #2274 / §29.7
    unwindTarget_       = savedUnwind;
    propagateBB_        = savedProp;
    currentCaughtExc_   = savedCaught;
    unwindScopeDepth_   = savedUD;
    diverged_           = savedDiv;
    genericSubst_       = std::move(savedGSubst);
    if (savedBB) builder_.SetInsertPoint(savedBB, savedBBOffset);

    return fn;
}


// #730/#2437 / §13 / §11.2 / §23.4: the class `cls`'s concrete indexer
// implementing the interface indexer `ifaceIdx`. Indexers are anonymous (all
// named ".this"), so classPropertyByName can't be used; match by FULL
// parameter list (arity + every parameter type) via findIndexer — see its
// own comment for the arity-blind defect (#2437) this fixes.
const sema::PropertySymbol* Emitter::classIndexerImplementing(
    const sema::TypeSymbol* cls, const sema::PropertySymbol* ifaceIdx) {
    return findIndexer(cls, ifaceIdx);
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


// fix #2400 (§2.8.2/§2.8.3 rule 4, §11.2, §23; v0.235): a DIRECT (non-itable,
// non-virtual) cross-module property SET — `importedObj.Prop := v` where Prop
// is a plain, non-virtual, non-interface property declared (or inherited) on
// an imported class, accessed through the imported class's own static type
// rather than through an interface reference (which dispatches through
// lowerVirtualPropertySet's itable/vtable slot lookup instead — already fixed
// by ensurePropertyAccessorFn's importedPropSetterFns_ fallback above).
// computedSetterOf returns nullptr for this shape (the imported PropertySymbol
// has no ast::PropertyDecl), so lowerPropertySet/lowerPropertyCompound bail out
// before ever reaching lowerPropertySetValue.
//
// Before this fix there was NO fallback at all for this shape: the assignment
// statement fell through every check in the AssignmentStmt lowering
// (LlvmBackend_Stmts.cpp) to the generic lvalueAddr fallback, which returns
// null for a PropertySymbol target (it is not a FieldSymbol, so fieldAddr does
// not resolve it) — the write was silently DROPPED, no crash. Confirmed by
// probe: `d.Name := 55; return d.Name - 55;` (where Name's backing field
// starts at 1) returned -54 (WEXITSTATUS-masked to 202) — the READ correctly
// returned the untouched original value 1 via the existing
// importedPropGetterFns_ getter fallback (LlvmBackend_Exprs.cpp ~4283-4300,
// issue #503), proving the WRITE never happened at all rather than writing to
// the wrong place.
//
// Mirrors that getter fallback's shape, calling the external setter stub
// (`bz$<Class>$<Prop>$propset`) declared by declareImportedPropertySetters
// (LlvmBackend_Setup.cpp) instead. The value is coerced to the stub's declared
// parameter type first, mirroring lowerVirtualPropertySet's identical
// coercion — both cross a fixed-ABI function-pointer boundary where an
// implicit numeric widening admitted by sema (e.g. an `int` literal assigned
// to a `long`-typed property) must be made explicit in the IR.
bool Emitter::lowerImportedPropertySetValue(const ast::Expr& target,
                                            llvm::Value* v) {
    if (!v) return false;
    auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&target);
    if (!ma || !ma->target) return false;
    auto* ps = dynamic_cast<sema::PropertySymbol*>(ctx_.exprSymbol(*ma));
    if (!ps || ps->isIndexer) return false;
    auto sit = importedPropSetterFns_.find(ps);
    if (sit == importedPropSetterFns_.end()) return false;
    llvm::Value* recv = lowerExpr(*ma->target);
    if (!recv || !recv->getType()->isPointerTy()) return false;
    llvm::Function* fn = sit->second;
    llvm::Type* valTy = fn->getFunctionType()->getParamType(1);
    v = coerceNumericToType(v, valTy, isUnsignedTargetExpr(target));
    builder_.CreateCall(fn->getFunctionType(), fn, {recv, v});
    emitPendingCheck();
    return true;
}


// #2204/§28.3 (found by this fix's own self-pass, not the original issue):
// the shape check MUST run — and fail — BEFORE `value` is lowered. An earlier
// version of this function lowered `value` unconditionally, exactly like
// lowerPropertySet's own `llvm::Value* v = lowerExpr(value); return v ? ... :
// false;` shape — but lowerPropertySet's FIRST line is `if
// (!computedSetterOf(target)) return false;`, a pure AST/symbol shape check
// with no side effects, so its own value-lowering is unreachable for a
// non-matching target. This function has no such pre-check, so lowerExpr(value)
// ran for EVERY assignment target this fallback was tried against — every
// indexer set (`arr[i] := v`, an ast::IndexExpr, never a MemberAccessExpr) and
// every LOCAL (non-imported) auto-property set (computedSetterOf itself
// excludes auto-properties, so lowerPropertySet also fails for those,
// reaching this fallback) — discarding the resulting value and evaluating the
// SAME expression a SECOND time in the real indexer/field-store path that
// runs afterward. For an RHS that creates a fresh owned temporary (`new T()`,
// an owned-returning call), this DOUBLE-EVALUATED and hence DOUBLE-CREATED a
// temporary — confirmed by this fix's own broader self-pass sweep, which a
// narrower one would have missed: borrowing_collection_indexer_set_temp_2204,
// collection_indexer_388, owned_array_set_dispose_919, owning_span_779,
// interface_indexer_401, and codegen_escaping_string_429's auto-property case
// all failed under the double-evaluation before this fix. Mirrors
// lowerPropertySet's discipline: check the target's SHAPE (MemberAccessExpr →
// PropertySymbol → present in importedPropSetterFns_) first, with no side
// effects, and only call lowerExpr(value) once that succeeds.
bool Emitter::lowerImportedPropertySet(const ast::Expr& target,
                                       const ast::Expr& value) {
    auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&target);
    if (!ma || !ma->target) return false;
    auto* ps = dynamic_cast<sema::PropertySymbol*>(ctx_.exprSymbol(*ma));
    if (!ps || ps->isIndexer) return false;
    if (!importedPropSetterFns_.count(ps)) return false;
    llvm::Value* v = lowerExpr(value);
    return v ? lowerImportedPropertySetValue(target, v) : false;
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
        isUnsignedTargetExpr(*as.target),
        isCharTargetExpr(*as.target),      // §6.6/§5.2 #1961
        isUnsignedTargetExpr(*as.value));  // §6.2 #2140 rUns
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
        const ast::Expr* srcExpr, bool ownLazyPipeline,
        bool* outOwningWrap) {
    if (outOwningWrap) *outOwningWrap = false;
    if (!v || !targetTy || !srcExpr) return v;
    if (!(targetTy->kind == sema::TypeRef::Kind::Named &&
          !targetTy->declaration && targetTy->astNode &&
          targetTy->astNode->name && !targetTy->astNode->name->parts.empty() &&
          targetTy->headName() == "IEnumerable"))
        return v;
    sema::TypeRef* actualTy = ctx_.exprType(*srcExpr);
    // #1916 / stdlib §5 / §29.11: a lazy LINQ pipeline (Select/Where/… → a
    // bz_lazy_t* sentinel) widened to a pure `IEnumerable<T>` in a RETURN
    // position. The lazy vtable has only 4 slots (td/Dispose/MoveNext/Current)
    // and its Current returns a POINTER; the caller's for-in over the returned
    // IEnumerable<T> drives the §29.11 iterator vtable — it reads slot 4
    // (GetEnumerator, OUT OF BOUNDS for the 4-slot lazy vtable → a garbage
    // function-pointer call → SIGSEGV) and expects Current BY VALUE. Drain the
    // pipeline once into a fresh owned Array<T> and wrap it in an OWNING
    // vtable-backed enumerator (mirrors #1911's local-decl fix): the caller owns
    // the returned IEnumerable and disposes it exactly once — a for-in over a
    // CallExpr temporary takes the loop-owned (!sourceIsNamedLocal) slot-1
    // Dispose path, freeing the drained array leak-free. Gated on
    // ownLazyPipeline: only RETURN sites (the caller owns the result) pass true;
    // the ARGUMENT position (a BORROWED callee param) needs a call-site
    // temporary-disposal contract and is deferred (#1916).
    if (ownLazyPipeline && isLazyEnumerableType(actualTy)) {
        if (outOwningWrap) *outOwningWrap = true;
        return drainLazyPipelineToOwnedIEnumerable(v, targetTy);
    }
    std::string_view an;
    if (actualTy && actualTy->kind == sema::TypeRef::Kind::Named &&
        actualTy->astNode && actualTy->astNode->name &&
        !actualTy->astNode->name->parts.empty())
        an = actualTy->headName();
    bool actualIsArray =
        (an == "Array" || an == "OwningArray" || an == "OwningBuffer" ||
         an == "Stack" || an == "OwningStack" ||
         an == "Queue" || an == "OwningQueue");
    // fix #1069 / stdlib §4.10 / §29.2: a `Set<T>`/`OwningSet<T>` widened to a
    // pure `IEnumerable<T>` used to pass through UNWRAPPED — the callee's for-in
    // then read the raw backing pointer's first word as a vtable (SIGSEGV; the
    // issue reported the older "yields 0"). Two backings, two treatments:
    //
    //   * CLASS/interface element — genericBackingType declines every class, so
    //     the set is bz_array_t-backed (`new Set<C>()` falls through lowerNew to
    //     bz_array_alloc) and that backing IS the dense element array. The plain
    //     array enumerator therefore walks exactly the set's logical elements;
    //     take the BORROWING wrap below, which shares the source and adds no
    //     ownership (the set's own binder still disposes its elements once).
    //     `OwningSet<T>` requires `where T: class`, so this is the only backing a
    //     valid OwningSet ever has.
    //   * value/record/string element — a generic-key HASH table whose slots are
    //     not a dense element array, which is exactly why #1068's dense-array fix
    //     excluded Set. Snapshot it instead (below).
    //
    // `SortedSet<T>` is deliberately NOT included: the snapshot below is in hash
    // order, and yielding a sorted set's elements unordered would silently break
    // §4.18's ordered-iteration contract — a worse failure than the current one.
    bool actualIsClassSet =
        !actualIsArray && actualTy && (an == "Set" || an == "OwningSet") &&
        genericBackingType(actualTy, 0, /*isSet=*/true, nullptr) == nullptr;
    if (actualIsArray || actualIsClassSet) {
        // §29 / §12.9 (#1850): a nested covariant view whose elements are
        // themselves array-backed collections viewed as pure IEnumerable<T>
        // needs the per-element re-wrapping nested enumerator, not the plain one.
        if (nestedIEnumViewNeeded(actualTy, targetTy))
            return wrapNestedArrayAsIEnumerable(v, actualTy, targetTy);
        return wrapArrayAsIEnumerable(v, targetTy);
    }
    // fix #1069 / stdlib §4.10 / §5: DICT-backed `Set<T>` — snapshot its element
    // keys into a fresh array and hand out an OWNING enumerator over the
    // snapshot. Reuses the tested §389 Set lazy source (bz_lazy_from_set, which
    // snapshots the key table) and the #1916 drain (bz_lazy_to_array, which
    // materialises the array AND disposes the lazy source), so the only new
    // object is the snapshot the returned enumerator owns. The SOURCE set is
    // untouched — no element is retained, released, or disposed here, so an
    // owning set still disposes each element exactly once at its own binder's
    // scope end. `outOwningWrap` tells the caller it must dispose the enumerator.
    if (actualTy && (an == "Set" || an == "OwningSet")) {
        uint64_t ksz = 0;
        if (genericBackingType(actualTy, 0, /*isSet=*/true, &ksz)) {
            auto* setFn = declareC("bz_lazy_from_set", ptrTy(), {ptrTy(), i64()});
            llvm::Value* lazy = builder_.CreateCall(
                setFn, {v, builder_.getInt64((int64_t)ksz)}, "setenum.lazy");
            if (outOwningWrap) *outOwningWrap = true;
            return drainLazyPipelineToOwnedIEnumerable(lazy, targetTy);
        }
    }
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
    // Determine the element type; ensureArrayEnumMachinery derives the stride and
    // the cached vtable/struct from it, then we allocate + fill one instance.
    llvm::Type* elemTy = i32();
    if (iEnumTy && !iEnumTy->genericArgs.empty() && iEnumTy->genericArgs[0])
        elemTy = lowerTypeRef(iEnumTy->genericArgs[0]);
    ArrEnumMachinery m = ensureArrayEnumMachinery(elemTy);
    llvm::StructType* stTy = m.stTy;
    // Allocate and fill the enumerator struct { vtable, arr, idx=-1, esz }.
    uint64_t stSz = module_->getDataLayout().getTypeAllocSize(stTy);
    auto* callocFn2 = declareC("calloc", ptrTy(), {i64(), i64()});
    llvm::Value* en = builder_.CreateCall(callocFn2,
        {builder_.getInt64(1), builder_.getInt64(stSz)}, "arrenum");
    builder_.CreateStore(m.vtGV, builder_.CreateStructGEP(stTy, en, 0));
    builder_.CreateStore(arr, builder_.CreateStructGEP(stTy, en, 1));
    builder_.CreateStore(builder_.getInt64(-1),
        builder_.CreateStructGEP(stTy, en, 2));
    builder_.CreateStore(builder_.getInt64(m.esz),
        builder_.CreateStructGEP(stTy, en, 3));
    return en;
}

// #1911 / stdlib §5 / §29.11: wrap a FRESH temporary array (no other owner) as a
// vtable-backed IEnumerable<T> enumerator that OWNS the array. Reuses the plain
// array-enum struct/MoveNext/Current machinery (identical layout + iteration),
// but installs a distinct vtable whose:
//   * Dispose (slot 1) frees the backing array (null element disposer — the
//     snapshot borrows its elements, owning none) AND the enumerator struct; and
//   * GetEnumerator (slot 4) stamps each clone with the NON-owning vtable, so a
//     per-loop enumerator (the for-in clone) borrows the shared array and frees
//     only its own struct. The owning SOURCE (this value, held by the local) is
//     the sole array owner and frees it exactly once at scope end
//     (registerIterableSourceDisposal / registerUsingDisposal → slot-1 Dispose).
// This is the lazy-pipeline analogue of the borrow/own split the plain wrapper
// relies on when the Array<T> has a separate owning local.
llvm::Value* Emitter::wrapOwnedArrayAsIEnumerable(llvm::Value* arr,
                                                  const sema::TypeRef* iEnumTy) {
    llvm::Type* elemTy = i32();
    if (iEnumTy && !iEnumTy->genericArgs.empty() && iEnumTy->genericArgs[0])
        elemTy = lowerTypeRef(iEnumTy->genericArgs[0]);
    // Ensure the plain (non-owning) machinery exists: shared struct type, the
    // non-owning vtable (for clones), the shared MoveNext/Current, and esz.
    ArrEnumMachinery m = ensureArrayEnumMachinery(elemTy);
    llvm::StructType* stTy = m.stTy;
    // Element-name key (same scheme as ensureArrayEnumMachinery) so the owning
    // Dispose / GetEnumerator / vtable are created once per element type.
    std::string elemName = elemTy->isIntegerTy() ?
        ("i" + std::to_string(elemTy->getIntegerBitWidth())) :
        (elemTy->isPointerTy() ? "ptr" : "f");
    std::string plainPrefix = "bz$arrenum$" + elemName + "$";
    std::string prefix = "bz$arrenumown$" + elemName + "$";

    // Owning Dispose: bz_array_free(self->arr, null); free(self).
    auto* dispFn = module_->getFunction(prefix + "Dispose");
    if (!dispFn) {
        dispFn = llvm::Function::Create(
            llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_), {ptrTy()}, false),
            llvm::Function::InternalLinkage, prefix + "Dispose", module_.get());
        auto* bb = llvm::BasicBlock::Create(llvm_, "entry", dispFn);
        llvm::IRBuilder<> b(bb);
        llvm::Value* self = dispFn->arg_begin();
        llvm::Value* arrPtr =
            b.CreateLoad(ptrTy(), b.CreateStructGEP(stTy, self, 1));
        auto* freeArrFn = module_->getOrInsertFunction(
            "bz_array_free", llvm::FunctionType::get(
                llvm::Type::getVoidTy(llvm_), {ptrTy(), ptrTy()}, false))
            .getCallee();
        b.CreateCall(
            llvm::FunctionType::get(
                llvm::Type::getVoidTy(llvm_), {ptrTy(), ptrTy()}, false),
            freeArrFn, {arrPtr, llvm::ConstantPointerNull::get(ptrTy())});
        auto* freeFn = module_->getOrInsertFunction(
            "free", llvm::FunctionType::get(
                llvm::Type::getVoidTy(llvm_), {ptrTy()}, false)).getCallee();
        b.CreateCall(llvm::FunctionType::get(
            llvm::Type::getVoidTy(llvm_), {ptrTy()}, false), freeFn, {self});
        b.CreateRetVoid();
    }

    // Owning GetEnumerator: a fresh NON-owning clone over the same borrowed array,
    // positioned before the first element (idx = -1). Stamping the clone with the
    // NON-owning vtable (m.vtGV) — not this owning one — is what keeps a per-loop
    // enumerator from freeing the shared array (only the source owns it).
    auto* geFn = module_->getFunction(prefix + "GetEnumerator");
    if (!geFn) {
        geFn = llvm::Function::Create(
            llvm::FunctionType::get(ptrTy(), {ptrTy()}, false),
            llvm::Function::InternalLinkage, prefix + "GetEnumerator",
            module_.get());
        auto* bb = llvm::BasicBlock::Create(llvm_, "entry", geFn);
        llvm::IRBuilder<> b(bb);
        llvm::Value* self = geFn->arg_begin();
        uint64_t cloneSz = module_->getDataLayout().getTypeAllocSize(stTy);
        auto* callocFT = llvm::FunctionType::get(ptrTy(), {i64(), i64()}, false);
        auto* callocC = module_->getOrInsertFunction("calloc", callocFT).getCallee();
        llvm::Value* clone = b.CreateCall(callocFT, callocC,
            {llvm::ConstantInt::get(i64(), 1),
             llvm::ConstantInt::get(i64(), (int64_t)cloneSz)});
        // Clone gets the NON-owning vtable (borrows the array).
        b.CreateStore(m.vtGV, b.CreateStructGEP(stTy, clone, 0));
        llvm::Value* arrCp = b.CreateLoad(ptrTy(),
            b.CreateStructGEP(stTy, self, 1));
        b.CreateStore(arrCp, b.CreateStructGEP(stTy, clone, 1));
        llvm::Value* eszCp = b.CreateLoad(i64(),
            b.CreateStructGEP(stTy, self, 3));
        b.CreateStore(eszCp, b.CreateStructGEP(stTy, clone, 3));
        b.CreateStore(llvm::ConstantInt::get(i64(), -1),
            b.CreateStructGEP(stTy, clone, 2));
        b.CreateRet(clone);
    }

    // Owning vtable: [0]=self, [1]=owning Dispose, [2]=MoveNext, [3]=Current,
    // [4]=owning GetEnumerator. Slots 2/3 reuse the plain machinery's functions.
    auto* mnFn = module_->getFunction(plainPrefix + "MoveNext");
    auto* curFn = module_->getFunction(plainPrefix + "Current");
    std::string vtName = prefix + "vtable";
    auto* vtGV = module_->getGlobalVariable(vtName);
    if (!vtGV) {
        auto* vtTy = llvm::ArrayType::get(ptrTy(), 5);
        std::vector<llvm::Constant*> elems = {
            llvm::ConstantPointerNull::get(ptrTy()),  // slot 0: self-ptr (below)
            dispFn, mnFn, curFn, geFn
        };
        vtGV = new llvm::GlobalVariable(*module_, vtTy, /*isConst=*/true,
            llvm::GlobalValue::InternalLinkage,
            llvm::ConstantArray::get(vtTy, elems), vtName);
        elems[0] = llvm::ConstantExpr::getPointerCast(vtGV, ptrTy());
        vtGV->setInitializer(llvm::ConstantArray::get(vtTy, elems));
    }

    // Allocate + fill the OWNING enumerator instance { owningVt, arr, idx=-1, esz }.
    uint64_t stSz = module_->getDataLayout().getTypeAllocSize(stTy);
    auto* callocFn2 = declareC("calloc", ptrTy(), {i64(), i64()});
    llvm::Value* en = builder_.CreateCall(callocFn2,
        {builder_.getInt64(1), builder_.getInt64(stSz)}, "arrenumown");
    builder_.CreateStore(vtGV, builder_.CreateStructGEP(stTy, en, 0));
    builder_.CreateStore(arr, builder_.CreateStructGEP(stTy, en, 1));
    builder_.CreateStore(builder_.getInt64(-1),
        builder_.CreateStructGEP(stTy, en, 2));
    builder_.CreateStore(builder_.getInt64(m.esz),
        builder_.CreateStructGEP(stTy, en, 3));
    return en;
}

// #1916 / #1911 / stdlib §5 / §29.11: drain a lazy LINQ pipeline (bz_lazy_t*)
// once into a fresh owned Array<T> and wrap that array in an OWNING vtable-backed
// enumerator. bz_lazy_to_array walks the pipeline to completion, materialising a
// fresh bz_array_t* AND disposing the pipeline (releasing its inline-lambda
// closures), so the resulting enumerator is the SOLE owner of independent data —
// its slot-1 Dispose frees the array exactly once at the consumer's scope end.
// `iEnumTy` is the target `IEnumerable<T>`; genericArgs[0] gives the element
// stride (matching lowerLocalDecl's #1911 esz computation). Shared by
// lowerLocalDecl (local-decl variant) and wrapAsIEnumerableForTarget (RETURN
// variant).
llvm::Value* Emitter::drainLazyPipelineToOwnedIEnumerable(
        llvm::Value* lazyVal, const sema::TypeRef* iEnumTy) {
    llvm::Type* et =
        (!iEnumTy || iEnumTy->genericArgs.empty() || !iEnumTy->genericArgs[0])
            ? i32() : lowerTypeRef(iEnumTy->genericArgs[0]);
    int64_t esz = 8;
    if (et->isIntegerTy(32) || et->isFloatTy()) esz = 4;
    else if (et->isIntegerTy(1) || et->isIntegerTy(8)) esz = 1;
    else if (et->isIntegerTy(16)) esz = 2;
    else if (et->isStructTy())
        esz = (int64_t)module_->getDataLayout().getTypeAllocSize(et);
    auto* toArrFn = declareC("bz_lazy_to_array", ptrTy(), {ptrTy(), i64()});
    llvm::Value* arr = builder_.CreateCall(
        toArrFn, {lazyVal, builder_.getInt64(esz)}, "lazy.mat");
    return wrapOwnedArrayAsIEnumerable(arr, iEnumTy);
}

// #2218 / stdlib §4.5/§4.13 / spec §29.1/§29.2: wrap a bz_array_t*-backed owning
// collection — OwningArray<T> / OwningSet<T> / OwningQueue<T> — in a vtable-
// backed IOwningEnumerator<T> for the explicit `coll.Drain().GetOwningEnumerator()`
// (or `var e := coll.Drain(); e.GetOwningEnumerator()`) manual-protocol path.
// .Drain() on these types returns the collection itself at static type
// IOwningEnumerable<T> (stdlib §4.5 "no separate enumerable object"), so the
// receiver is a raw bz_array_t* with NO vtable at offset 0 (its first field is
// `data`, a heap-buffer pointer). Without this wrapper the codegen dispatched
// the receiver's slot 4 — a bz_array_t* has no slot 4, so the load returned
// garbage and the call SEGV'd (#2218). The wrapper:
//
//   * BORROWS the array — the source `coll` (still in scope) owns the array
//     and disposes any remaining elements on its own scope end (the §28.3
//     owned-local discharge rules apply to the original `a`).
//   * MoveNext (slot 2) pops the next element via bz_array_drain_next into
//     the wrapper's `pending` slot, returning i1 (true ⇔ a new element was
//     yielded). §29.1's "advanced-but-untaken" pending is held in the slot
//     until TakeCurrent / Dispose / a subsequent MoveNext.
//   * TakeCurrent (slot 3) moves the pending element out and nulls the slot,
//     per §29.1 ("after a successful MoveNext, TakeCurrent() transfers
//     ownership of the most-recently-yielded value to the caller, leaving the
//     enumerator's internal slot empty"). A double-take / before-first-advance
//     / after-exhaustion surfaces as a null `pending` load and is handled by
//     the existing iterator-frame IOE path (the lower-level path here is just
//     the pop-and-store; an IOE-raising wrapper is layered on top in
//     emitIteratorMethod-style when TakeCurrent comes from a yield-iterator
//     frame — this wrapper is the manual-protocol analogue, mirroring the
//     bz_array_drain_next-based drain loop of `for var x in coll.Drain()`).
//   * Dispose (slot 1) disposes the pending value if non-null (per §29.1 "a
//     Dispose() after an advance whose value was never taken disposes the
//     pending value"), then frees the wrapper. The ARRAY is NOT freed —
//     `coll`'s owner (the user's `using var a`) disposes it.
//   * Slot 4 (GetOwningEnumerator) returns `self` as a safe no-op — calling
//     GetOwningEnumerator on an IOwningEnumerator<T> is not a defined
//     operation, but a vtable slot must exist; the function returns the
//     receiver so any unexpected dispatch is at least non-crashing.
//
// Element name key matches ensureArrayEnumMachinery's per-element-type
// scheme, so all calls with the same element T share the same functions +
// vtable global.
llvm::Value* Emitter::wrapArrayAsIOwningEnumerator(
        llvm::Value* arr, const sema::TypeRef* iOwnEnumTy) {
    // Element type is always a class pointer (IOwningEnumerable<T> constrains
    // T: class per stdlib §4.13). Resolve through the active per-instantiation
    // substitution in case the iOwnEnumTy is a generic template; the bare
    // type-parameter case lowers as a class pointer (§29.2 #990 mirror).
    const sema::TypeRef* elemTr = (iOwnEnumTy && !iOwnEnumTy->genericArgs.empty())
                                      ? iOwnEnumTy->genericArgs[0]
                                      : nullptr;
    const sema::TypeRef* resolvedElem = resolveTypeParamRef(elemTr);
    llvm::Type* elemLlvm;
    if (resolvedElem && resolvedElem != elemTr)
        elemLlvm = lowerTypeRef(resolvedElem);
    else if (elemTr && elemTr->kind == sema::TypeRef::Kind::Named &&
             elemTr->declaration == nullptr)
        elemLlvm = ptrTy();   // unresolved type param — class per §4.13
    else if (elemTr)
        elemLlvm = lowerTypeRef(elemTr);
    else
        elemLlvm = ptrTy();

    // Element-name key (same scheme as ensureArrayEnumMachinery) so the
    // Dispose / MoveNext / TakeCurrent / vtable are created once per element
    // type. The wrapper is a per-element-type singleton — fresh calls allocate
    // a new instance but reuse the same code and vtable.
    std::string elemName = elemLlvm->isIntegerTy()
        ? ("i" + std::to_string(elemLlvm->getIntegerBitWidth()))
        : (elemLlvm->isPointerTy() ? "ptr" : "f");
    std::string prefix = "bz$ownarrenum$" + elemName + "$";

    // Wrapper struct: { ptr vtable, ptr arr, ptr pending }. Three pointer
    // fields (vtable + arr + pending); the wrapper holds a borrow of the
    // array (does NOT own it) and an owned class pointer in `pending` that
    // the Dispose path must release if non-null (§29.1 abandonment rule).
    std::string stName = "ownarrenum." + elemName;
    auto* stTy = llvm::StructType::getTypeByName(llvm_, stName);
    if (!stTy) {
        stTy = llvm::StructType::create(llvm_,
            {ptrTy(), ptrTy(), ptrTy()}, stName);
    }

    // The disposer for `pending` — for an owning collection, the element type
    // is a class and its destructor is what the runtime calls. We mirror the
    // bz_array_drain_next contract (the value left in pending is the popped
    // CLASS pointer), so we use emitDisposeAndReclaim on the loaded pointer.
    // That helper nulls the slot on the way out and recycles storage; the
    // pending slot is then nulled in the wrapper so a redundant Dispose is
    // a no-op (mirrors the iterator-frame's -2 sentinel + slot nulling).

    // --- Dispose(self) — dispose pending (if any), free wrapper. ---
    auto* dispFn = module_->getFunction(prefix + "Dispose");
    if (!dispFn) {
        dispFn = llvm::Function::Create(
            llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_), {ptrTy()}, false),
            llvm::Function::InternalLinkage, prefix + "Dispose", module_.get());
        auto* bb = llvm::BasicBlock::Create(llvm_, "entry", dispFn);
        // §28: emitDisposeAndReclaim uses the Emitter's `builder_` /
        // `currentFn_` to insert into. Swap them in for the duration of the
        // helper body, then restore so the caller of
        // wrapArrayAsIOwningEnumerator sees its own builder / currentFn
        // unchanged (#2218 IR-verification: the previous attempt left the
        // outer currentFn_ in place and ended up emitting helper-body
        // instructions into the wrong function). The save pattern mirrors
        // emitPosFieldGetter (LlvmBackend_impl.hpp ~1462) — save the insert
        // block/point rather than the whole IRBuilder (which is
        // non-copyable).
        llvm::Function* savedCurrentFn = currentFn_;
        auto* savedBB = builder_.GetInsertBlock();
        llvm::BasicBlock::iterator savedIP{};
        if (savedBB) savedIP = builder_.GetInsertPoint();
        currentFn_ = dispFn;
        builder_.SetInsertPoint(bb);
        llvm::Value* self = dispFn->arg_begin();
        // Load pending (ptr); if non-null, dispose + reclaim.
        llvm::Value* pendF = builder_.CreateStructGEP(stTy, self, 2);
        llvm::Value* pend = builder_.CreateLoad(ptrTy(), pendF, "oaed.pend");
        llvm::Value* isNull = builder_.CreateICmpEQ(
            pend, llvm::ConstantPointerNull::get(ptrTy()), "oaed.pend.null");
        auto* dispBB = llvm::BasicBlock::Create(llvm_, "oaed.disp", dispFn);
        auto* contBB = llvm::BasicBlock::Create(llvm_, "oaed.cont", dispFn);
        builder_.CreateCondBr(isNull, contBB, dispBB);
        builder_.SetInsertPoint(dispBB);
        // emitDisposeAndReclaim expects an instruction-point set on the
        // current builder. It loads the class vtable, dispatches the slot-1
        // Dispose, then frees the object. Set the slot to null first so a
        // re-entrant Dispose (slot-1 Dispose cascades back to pending's
        // release) does not re-dispose.
        builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), pendF);
        emitDisposeAndReclaim(pend);
        builder_.CreateBr(contBB);
        builder_.SetInsertPoint(contBB);
        // Free the wrapper struct.
        auto* freeFn = module_->getOrInsertFunction(
            "free", llvm::FunctionType::get(
                llvm::Type::getVoidTy(llvm_), {ptrTy()}, false)).getCallee();
        builder_.CreateCall(llvm::FunctionType::get(
            llvm::Type::getVoidTy(llvm_), {ptrTy()}, false), freeFn, {self});
        builder_.CreateRetVoid();
        currentFn_ = savedCurrentFn;
        if (savedBB) builder_.SetInsertPoint(savedBB, savedIP);
    }

    // --- MoveNext(self) -> i1 — pop next element via bz_array_drain_next. ---
    auto* mnFn = module_->getFunction(prefix + "MoveNext");
    if (!mnFn) {
        mnFn = llvm::Function::Create(
            llvm::FunctionType::get(i1(), {ptrTy()}, false),
            llvm::Function::InternalLinkage, prefix + "MoveNext", module_.get());
        auto* bb = llvm::BasicBlock::Create(llvm_, "entry", mnFn);
        // Swap builder_/currentFn_ for the helper body (see Dispose above).
        llvm::Function* savedCurrentFn = currentFn_;
        auto* savedBB = builder_.GetInsertBlock();
        llvm::BasicBlock::iterator savedIP{};
        if (savedBB) savedIP = builder_.GetInsertPoint();
        currentFn_ = mnFn;
        builder_.SetInsertPoint(bb);
        llvm::Value* self = mnFn->arg_begin();
        // If a previous advance's pending value is still set (the user broke
        // the protocol by calling MoveNext again without TakeCurrent), dispose
        // it before the next advance — the wrapper does not retain two
        // pendings (it has a single slot). Per §29.1 the standard disposition
        // for an undisposed pending is "dispose", matching the iterator-frame
        // MoveNext's "dispose previous yield" behaviour (#747 / Iterators.cpp
        // ~960).
        llvm::Value* pendF2 = builder_.CreateStructGEP(stTy, self, 2);
        llvm::Value* prev = builder_.CreateLoad(ptrTy(), pendF2, "oaem.prev");
        llvm::Value* prevNull = builder_.CreateICmpEQ(
            prev, llvm::ConstantPointerNull::get(ptrTy()), "oaem.prev.null");
        auto* dispPrevBB =
            llvm::BasicBlock::Create(llvm_, "oaem.dispprev", mnFn);
        auto* afterDispBB =
            llvm::BasicBlock::Create(llvm_, "oaem.after", mnFn);
        builder_.CreateCondBr(prevNull, afterDispBB, dispPrevBB);
        builder_.SetInsertPoint(dispPrevBB);
        builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), pendF2);
        emitDisposeAndReclaim(prev);
        builder_.CreateBr(afterDispBB);
        builder_.SetInsertPoint(afterDispBB);
        // bz_array_drain_next(arr, &pending) → i1 (true = yielded)
        llvm::Value* arrF = builder_.CreateStructGEP(stTy, self, 1);
        llvm::Value* arrPtr = builder_.CreateLoad(ptrTy(), arrF, "oaem.arr");
        // Use a fresh alloca in the function's entry block for the out-param
        // (drain writes the popped class pointer into it).
        auto* allocaBB = &mnFn->getEntryBlock();
        llvm::IRBuilder<> ab(allocaBB, allocaBB->begin());
        auto* outSlot = ab.CreateAlloca(ptrTy(), nullptr, "oaem.out");
        auto* drainFT = llvm::FunctionType::get(
            i1(), {ptrTy(), ptrTy()}, false);
        auto* drainFn = module_->getOrInsertFunction(
            "bz_array_drain_next", drainFT).getCallee();
        llvm::Value* ok = builder_.CreateCall(drainFT, drainFn, {arrPtr, outSlot},
                                       "oaem.ok");
        // Store the popped pointer into pending (a no-op if drain returned
        // false, since the slot was just nulled in that path).
        llvm::Value* popped = builder_.CreateLoad(ptrTy(), outSlot, "oaem.popped");
        builder_.CreateStore(popped, builder_.CreateStructGEP(stTy, self, 2));
        builder_.CreateRet(ok);
        currentFn_ = savedCurrentFn;
        if (savedBB) builder_.SetInsertPoint(savedBB, savedIP);
    }

    // --- TakeCurrent(self) -> ptr — move pending out, null slot, return. ---
    auto* tcFn = module_->getFunction(prefix + "TakeCurrent");
    if (!tcFn) {
        tcFn = llvm::Function::Create(
            llvm::FunctionType::get(ptrTy(), {ptrTy()}, false),
            llvm::Function::InternalLinkage, prefix + "TakeCurrent",
            module_.get());
        auto* bb = llvm::BasicBlock::Create(llvm_, "entry", tcFn);
        llvm::IRBuilder<> b(bb);
        llvm::Value* self = tcFn->arg_begin();
        llvm::Value* pendF = b.CreateStructGEP(stTy, self, 2);
        llvm::Value* pend = b.CreateLoad(ptrTy(), pendF, "oaetc.pend");
        // Null the slot before returning so a subsequent TakeCurrent / Dispose
        // / MoveNext sees an empty pending. (The IOE check on a null pending
        // is raised by the iterator-frame's TakeCurrent wrapper when the
        // enumerator is a yield-iterator frame; for THIS manual-protocol
        // wrapper we just hand back the pointer as-is — null in, null out —
        // and let the caller's `if en.MoveNext() then ...` guard the take.)
        b.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), pendF);
        b.CreateRet(pend);
    }

    // --- Slot 4 (GetOwningEnumerator): return self (no-op safe default). ---
    auto* geFn = module_->getFunction(prefix + "GetOwningEnumerator");
    if (!geFn) {
        geFn = llvm::Function::Create(
            llvm::FunctionType::get(ptrTy(), {ptrTy()}, false),
            llvm::Function::InternalLinkage, prefix + "GetOwningEnumerator",
            module_.get());
        auto* bb = llvm::BasicBlock::Create(llvm_, "entry", geFn);
        llvm::IRBuilder<> b(bb);
        b.CreateRet(&*geFn->arg_begin());
    }

    // Vtable: [0]=self, [1]=Dispose, [2]=MoveNext, [3]=TakeCurrent,
    // [4]=GetOwningEnumerator (no-op).
    std::string vtName = prefix + "vtable";
    auto* vtGV = module_->getGlobalVariable(vtName);
    if (!vtGV) {
        auto* vtTy = llvm::ArrayType::get(ptrTy(), 5);
        std::vector<llvm::Constant*> elems = {
            llvm::ConstantPointerNull::get(ptrTy()),  // slot 0: self-ptr (below)
            dispFn, mnFn, tcFn, geFn
        };
        vtGV = new llvm::GlobalVariable(*module_, vtTy, /*isConst=*/true,
            llvm::GlobalValue::InternalLinkage,
            llvm::ConstantArray::get(vtTy, elems), vtName);
        elems[0] = llvm::ConstantExpr::getPointerCast(vtGV, ptrTy());
        vtGV->setInitializer(llvm::ConstantArray::get(vtTy, elems));
    }

    // Allocate + fill the OWNING-enumerator instance { vtable, arr, pending=0 }.
    uint64_t stSz = module_->getDataLayout().getTypeAllocSize(stTy);
    auto* callocFn2 = declareC("calloc", ptrTy(), {i64(), i64()});
    llvm::Value* en = builder_.CreateCall(callocFn2,
        {builder_.getInt64(1), builder_.getInt64(stSz)}, "ownarrenum");
    builder_.CreateStore(vtGV, builder_.CreateStructGEP(stTy, en, 0));
    builder_.CreateStore(arr, builder_.CreateStructGEP(stTy, en, 1));
    builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()),
        builder_.CreateStructGEP(stTy, en, 2));
    return en;
}

// §29.2 (#437 / #1850): create-or-reuse the array-enumerator machinery (struct
// type, method functions, vtable) keyed on the lowered element type. Factored
// out of wrapArrayAsIEnumerable so the nested covariant-view factory
// (ensureIEnumViewFactory) can build fresh enumerator instances over the same
// cached functions/vtable.
Emitter::ArrEnumMachinery Emitter::ensureArrayEnumMachinery(llvm::Type* elemTy) {
    // per §9 (bug D): a record element type lowers to an LLVM StructType whose
    // allocation size may exceed 8 bytes; use DataLayout for structs so the esz
    // stored in the enumerator frame matches the stride used by bz_array_ptr_at
    // (same fix as collectionElemSize in Calls.cpp).
    int64_t esz = 8;
    if (elemTy->isIntegerTy(32) || elemTy->isFloatTy()) esz = 4;
    else if (elemTy->isIntegerTy(1) || elemTy->isIntegerTy(8)) esz = 1;
    else if (elemTy->isIntegerTy(16)) esz = 2;
    else if (elemTy->isStructTy())
        esz = (int64_t)module_->getDataLayout().getTypeAllocSize(elemTy);
    else esz = 8;

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

    return {stTy, vtGV, esz};
}

// §29 / §12.9 (#1850): a factory `ptr F(ptr backing)` wrapping a bz_array_t*
// `backing` (whose static type is `storedTy`) as the covariant view `viewTy`.
//
// When the element representation matches the view (a plain covariant view such
// as Array<Dog> → IEnumerable<Animal>, both stored/viewed as object ptrs), F
// allocates the ordinary array enumerator. When the ELEMENT itself is an
// array-backed collection viewed as a pure IEnumerable<T> (Array<Array<Dog>> →
// IEnumerable<IEnumerable<Dog>>), the stored element is a raw bz_array_t* but
// the consumer iterates it via vtable dispatch — so F allocates a NESTED wrapper
// whose Current lazily materialises a proper inner view per element (recursing
// through this same factory for deeper nesting). The nested wrapper owns the one
// live inner view: MoveNext disposes the previous element's inner view, and
// Dispose disposes the last one, before freeing the wrapper struct itself. The
// backing array is borrowed (shared), exactly like the plain wrapper.
// §29 / §12.9 (#1850): the nested-wrap decision — element is an array-backed
// collection (bz_array_t*, lowers to ptr) VIEWED as a pure IEnumerable<T>.
// Only the pure vtable-dispatched interfaces (IEnumerable / IOwningEnumerable)
// make the consumer's inner for-in vtable-dispatch
// (lowerIEnumerableBuiltinFor's checkPureIEnumerable / the field
// IOwningEnumerable case); richer interfaces keep the array-index protocol, so
// they must NOT be nest-wrapped (that would break their raw-array iteration).
bool Emitter::nestedIEnumViewNeeded(const sema::TypeRef* storedTy,
                                    const sema::TypeRef* viewTy) const {
    auto isArrayBacked = [](const sema::TypeRef* t) -> bool {
        if (!t || t->kind != sema::TypeRef::Kind::Named) return false;
        std::string_view h = t->headName();
        return h == "Array" || h == "OwningArray" || h == "OwningBuffer" ||
               h == "Stack" || h == "OwningStack" ||
               h == "Queue" || h == "OwningQueue";
    };
    auto isPureIEnumView = [](const sema::TypeRef* t) -> bool {
        if (!t || t->kind != sema::TypeRef::Kind::Named || t->declaration)
            return false;
        std::string_view h = t->headName();
        return h == "IEnumerable" || h == "IOwningEnumerable";
    };
    const sema::TypeRef* storedElem =
        (storedTy && !storedTy->genericArgs.empty()) ? storedTy->genericArgs[0]
                                                     : nullptr;
    const sema::TypeRef* viewElem =
        (viewTy && !viewTy->genericArgs.empty()) ? viewTy->genericArgs[0]
                                                 : nullptr;
    return isArrayBacked(storedElem) && isPureIEnumView(viewElem);
}

llvm::Function* Emitter::ensureIEnumViewFactory(const sema::TypeRef* storedTy,
                                                const sema::TypeRef* viewTy) {
    const sema::TypeRef* storedElem =
        (storedTy && !storedTy->genericArgs.empty()) ? storedTy->genericArgs[0]
                                                     : nullptr;
    const sema::TypeRef* viewElem =
        (viewTy && !viewTy->genericArgs.empty()) ? viewTy->genericArgs[0]
                                                 : nullptr;
    bool elemNeedsWrap = nestedIEnumViewNeeded(storedTy, viewTy);

    llvm::Type* elemTy = viewElem ? lowerTypeRef(viewElem) : i32();
    std::string elemName = elemTy->isIntegerTy()
        ? ("i" + std::to_string(elemTy->getIntegerBitWidth()))
        : (elemTy->isPointerTy() ? "ptr" : "f");

    if (!elemNeedsWrap) {
        // Leaf: a plain array enumerator over the (borrowed) backing array.
        std::string name = "bz$viewfac$P$" + elemName;
        if (auto* f = module_->getFunction(name)) return f;
        ArrEnumMachinery m = ensureArrayEnumMachinery(elemTy);
        auto* ft = llvm::FunctionType::get(ptrTy(), {ptrTy()}, false);
        auto* f = llvm::Function::Create(ft, llvm::Function::InternalLinkage,
                                         name, module_.get());
        auto* bb = llvm::BasicBlock::Create(llvm_, "entry", f);
        llvm::IRBuilder<> b(bb);
        llvm::Value* backing = f->arg_begin();
        uint64_t stSz = module_->getDataLayout().getTypeAllocSize(m.stTy);
        auto* callocFT = llvm::FunctionType::get(ptrTy(), {i64(), i64()}, false);
        auto* callocC = module_->getOrInsertFunction("calloc", callocFT)
                            .getCallee();
        llvm::Value* en = b.CreateCall(callocFT, callocC,
            {llvm::ConstantInt::get(i64(), 1),
             llvm::ConstantInt::get(i64(), (int64_t)stSz)});
        b.CreateStore(m.vtGV, b.CreateStructGEP(m.stTy, en, 0));
        b.CreateStore(backing, b.CreateStructGEP(m.stTy, en, 1));
        b.CreateStore(llvm::ConstantInt::get(i64(), -1),
                      b.CreateStructGEP(m.stTy, en, 2));
        b.CreateStore(llvm::ConstantInt::get(i64(), m.esz),
                      b.CreateStructGEP(m.stTy, en, 3));
        b.CreateRet(en);
        return f;
    }

    // Nested: each element (a bz_array_t*) is itself wrapped as `viewElem` via
    // the inner factory. Recurse to get it first (its name encodes the inner
    // shape, so this factory's cache key is unique per whole wrap shape).
    llvm::Function* innerFac = ensureIEnumViewFactory(storedElem, viewElem);
    std::string name = "bz$viewfac$N$" + innerFac->getName().str();
    if (auto* f = module_->getFunction(name)) return f;

    // Nested enumerator struct: { ptr vtable, ptr arr, i64 idx, i64 esz,
    //                             ptr curInner }.  esz here is 8 — the outer
    //  backing array's elements are collection pointers (bz_array_t*).
    std::string stName = "arrenumnest." + elemName;
    auto* stTy = llvm::StructType::getTypeByName(llvm_, stName);
    if (!stTy)
        stTy = llvm::StructType::create(
            llvm_, {ptrTy(), ptrTy(), i64(), i64(), ptrTy()}, stName);

    std::string prefix = "bz$arrenumnest$" + elemName + "$";
    auto* voidTy = llvm::Type::getVoidTy(llvm_);
    auto getOrCreate = [&](const std::string& fname, llvm::FunctionType* ft,
                           std::function<void(llvm::Function*)> fill)
        -> llvm::Function* {
        if (auto* g = module_->getFunction(fname)) return g;
        auto* g = llvm::Function::Create(ft, llvm::Function::InternalLinkage,
                                         fname, module_.get());
        fill(g);
        return g;
    };
    auto* freeFT = llvm::FunctionType::get(voidTy, {ptrTy()}, false);

    // Dispatch the current inner view's slot-1 Dispose (if any), then null it.
    // The inner view carries its own vtable at slot 0; slot 1 is Dispose (§29.11).
    auto disposeCurInner = [&](llvm::IRBuilder<>& b, llvm::Value* self) {
        auto* fn = b.GetInsertBlock()->getParent();
        auto* haveBB = llvm::BasicBlock::Create(llvm_, "ci.have", fn);
        auto* contBB = llvm::BasicBlock::Create(llvm_, "ci.cont", fn);
        llvm::Value* ciF = b.CreateStructGEP(stTy, self, 4);
        llvm::Value* ci = b.CreateLoad(ptrTy(), ciF);
        llvm::Value* isNull =
            b.CreateICmpEQ(ci, llvm::ConstantPointerNull::get(ptrTy()));
        b.CreateCondBr(isNull, contBB, haveBB);
        b.SetInsertPoint(haveBB);
        // vt = ci[0]; dispFn = vt[1] (Dispose slot); dispFn(ci). The vtable is a
        // dense array of ptr — index it as the rest of codegen does (getelementptr
        // ptr, vt, i32 1), not via a struct GEP.
        llvm::Value* vt = b.CreateLoad(ptrTy(), ci);
        llvm::Value* dispSlot = b.CreateGEP(ptrTy(), vt, {b.getInt32(1)});
        llvm::Value* dispFn = b.CreateLoad(ptrTy(), dispSlot);
        b.CreateCall(freeFT, dispFn, {ci});
        b.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), ciF);
        b.CreateBr(contBB);
        b.SetInsertPoint(contBB);
    };

    // Dispose(self): dispose the last live inner view, then free(self).
    auto* dispFn = getOrCreate(prefix + "Dispose", freeFT,
        [&](llvm::Function* f) {
            auto* bb = llvm::BasicBlock::Create(llvm_, "entry", f);
            llvm::IRBuilder<> b(bb);
            disposeCurInner(b, f->arg_begin());
            auto* freeC = module_->getOrInsertFunction("free", freeFT)
                              .getCallee();
            b.CreateCall(freeFT, freeC, {f->arg_begin()});
            b.CreateRetVoid();
        });

    // MoveNext(self): dispose the previous element's inner view, advance idx,
    // return idx < len(arr).
    auto* mnFn = getOrCreate(prefix + "MoveNext",
        llvm::FunctionType::get(i1(), {ptrTy()}, false),
        [&](llvm::Function* f) {
            auto* bb = llvm::BasicBlock::Create(llvm_, "entry", f);
            llvm::IRBuilder<> b(bb);
            llvm::Value* self = f->arg_begin();
            disposeCurInner(b, self);
            llvm::Value* idxF = b.CreateStructGEP(stTy, self, 2);
            llvm::Value* idx = b.CreateLoad(i64(), idxF);
            llvm::Value* next =
                b.CreateAdd(idx, llvm::ConstantInt::get(i64(), 1));
            b.CreateStore(next, idxF);
            llvm::Value* arrPtr =
                b.CreateLoad(ptrTy(), b.CreateStructGEP(stTy, self, 1));
            auto* lenFT = llvm::FunctionType::get(i64(), {ptrTy()}, false);
            auto* lenFn = module_->getOrInsertFunction("bz_array_len", lenFT)
                              .getCallee();
            llvm::Value* len = b.CreateCall(lenFT, lenFn, {arrPtr});
            b.CreateRet(b.CreateICmpSLT(next, len));
        });

    // Current(self): if the inner view for this element is already materialised,
    // return it; else load the element (a bz_array_t*), wrap it via innerFac,
    // cache it in curInner, and return it.
    auto* curFn = getOrCreate(prefix + "Current",
        llvm::FunctionType::get(ptrTy(), {ptrTy()}, false),
        [&](llvm::Function* f) {
            auto* entry = llvm::BasicBlock::Create(llvm_, "entry", f);
            auto* mkBB = llvm::BasicBlock::Create(llvm_, "ci.make", f);
            auto* retBB = llvm::BasicBlock::Create(llvm_, "ci.ret", f);
            llvm::IRBuilder<> b(entry);
            llvm::Value* self = f->arg_begin();
            llvm::Value* ciF = b.CreateStructGEP(stTy, self, 4);
            llvm::Value* ci = b.CreateLoad(ptrTy(), ciF);
            llvm::Value* isNull =
                b.CreateICmpEQ(ci, llvm::ConstantPointerNull::get(ptrTy()));
            b.CreateCondBr(isNull, mkBB, retBB);
            b.SetInsertPoint(mkBB);
            llvm::Value* arrPtr =
                b.CreateLoad(ptrTy(), b.CreateStructGEP(stTy, self, 1));
            llvm::Value* idx =
                b.CreateLoad(i64(), b.CreateStructGEP(stTy, self, 2));
            auto* ptrAtFT =
                llvm::FunctionType::get(ptrTy(), {ptrTy(), i64(), i64()}, false);
            auto* ptrAtFn =
                module_->getOrInsertFunction("bz_array_ptr_at", ptrAtFT)
                    .getCallee();
            llvm::Value* ep = b.CreateCall(ptrAtFT, ptrAtFn,
                {arrPtr, idx, llvm::ConstantInt::get(i64(), 8)});
            llvm::Value* innerArr = b.CreateLoad(ptrTy(), ep);
            auto* facFT = llvm::FunctionType::get(ptrTy(), {ptrTy()}, false);
            llvm::Value* inner = b.CreateCall(facFT, innerFac, {innerArr});
            b.CreateStore(inner, ciF);
            b.CreateBr(retBB);
            b.SetInsertPoint(retBB);
            llvm::PHINode* phi = b.CreatePHI(ptrTy(), 2);
            phi->addIncoming(ci, entry);
            phi->addIncoming(inner, mkBB);
            b.CreateRet(phi);
        });

    // GetEnumerator(self): a FRESH nested enumerator over the same backing array,
    // before the first element (idx = -1, curInner = null) — §29.11 replay.
    auto* geFn = getOrCreate(prefix + "GetEnumerator",
        llvm::FunctionType::get(ptrTy(), {ptrTy()}, false),
        [&](llvm::Function* f) {
            auto* bb = llvm::BasicBlock::Create(llvm_, "entry", f);
            llvm::IRBuilder<> b(bb);
            llvm::Value* self = f->arg_begin();
            uint64_t cloneSz = module_->getDataLayout().getTypeAllocSize(stTy);
            auto* callocFT =
                llvm::FunctionType::get(ptrTy(), {i64(), i64()}, false);
            auto* callocC = module_->getOrInsertFunction("calloc", callocFT)
                                .getCallee();
            llvm::Value* clone = b.CreateCall(callocFT, callocC,
                {llvm::ConstantInt::get(i64(), 1),
                 llvm::ConstantInt::get(i64(), (int64_t)cloneSz)});
            llvm::Value* vt =
                b.CreateLoad(ptrTy(), b.CreateStructGEP(stTy, self, 0));
            b.CreateStore(vt, b.CreateStructGEP(stTy, clone, 0));
            llvm::Value* arrCp =
                b.CreateLoad(ptrTy(), b.CreateStructGEP(stTy, self, 1));
            b.CreateStore(arrCp, b.CreateStructGEP(stTy, clone, 1));
            b.CreateStore(llvm::ConstantInt::get(i64(), -1),
                          b.CreateStructGEP(stTy, clone, 2));
            b.CreateStore(llvm::ConstantInt::get(i64(), 8),
                          b.CreateStructGEP(stTy, clone, 3));
            // curInner (slot 4) already null from calloc.
            b.CreateRet(clone);
        });

    // vtable: [0]=self sentinel, [1]=Dispose, [2]=MoveNext, [3]=Current,
    // [4]=GetEnumerator (the same 5-slot shape as every sync IEnumerable
    // producer, §29.11 #943).
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

    // The factory itself: allocate + fill a nested enumerator over `backing`.
    auto* ft = llvm::FunctionType::get(ptrTy(), {ptrTy()}, false);
    auto* f = llvm::Function::Create(ft, llvm::Function::InternalLinkage,
                                     name, module_.get());
    auto* bb = llvm::BasicBlock::Create(llvm_, "entry", f);
    llvm::IRBuilder<> b(bb);
    llvm::Value* backing = f->arg_begin();
    uint64_t stSz = module_->getDataLayout().getTypeAllocSize(stTy);
    auto* callocFT = llvm::FunctionType::get(ptrTy(), {i64(), i64()}, false);
    auto* callocC = module_->getOrInsertFunction("calloc", callocFT).getCallee();
    llvm::Value* en = b.CreateCall(callocFT, callocC,
        {llvm::ConstantInt::get(i64(), 1),
         llvm::ConstantInt::get(i64(), (int64_t)stSz)});
    b.CreateStore(vtGV, b.CreateStructGEP(stTy, en, 0));
    b.CreateStore(backing, b.CreateStructGEP(stTy, en, 1));
    b.CreateStore(llvm::ConstantInt::get(i64(), -1),
                  b.CreateStructGEP(stTy, en, 2));
    b.CreateStore(llvm::ConstantInt::get(i64(), 8),
                  b.CreateStructGEP(stTy, en, 3));
    // curInner (slot 4) already null from calloc.
    b.CreateRet(en);
    return f;
}

// §29 / §12.9 (#1850): wrap a nested covariant IEnumerable view by calling the
// shape-specific factory built by ensureIEnumViewFactory.
llvm::Value* Emitter::wrapNestedArrayAsIEnumerable(llvm::Value* arr,
                                                   const sema::TypeRef* storedTy,
                                                   const sema::TypeRef* viewTy) {
    llvm::Function* fac = ensureIEnumViewFactory(storedTy, viewTy);
    return builder_.CreateCall(fac, {arr}, "nestview");
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
                                                  const sema::TypeRef* iEnumTy,
                                                  bool ownsSrc) {
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
        // GetEnumerator() declares return type as builtin IEnumerator<T> (the
        // ONLY spelling admissible for an interface implementation — §11.2/§12.1
        // "Member matching" requires the return type to be exactly equal to the
        // interface's under substitution, so a user's `GetEnumerator():
        // IEnumerator<T>` can never name a concrete class directly). Find the
        // concrete class the runtime object will actually be: a class exposing
        // MoveNext + Current.
        //
        // fix #2384 / §29.1/§29.11 (v0.235): check the SOURCE class itself
        // FIRST. A class may legally implement BOTH `IEnumerable<T>` and
        // `IEnumerator<T>` and return `this` from `GetEnumerator()` (the
        // single-object self-returning idiom) — precedent: #2356's
        // GetAsyncEnumerator fix treats a hand-written implementation that
        // returns `this` as legal ("a case the compiler cannot rule out"), and
        // §29.11's "each GetEnumerator() call produces an independent
        // enumerator" is satisfied at the WRAPPER level (a fresh calloc'd
        // adapter struct every call, `wrapGetEnumFn` below re-invokes the
        // source's own GetEnumerator() each time) — it does not forbid the
        // wrapped class from being its own enumerator. The OLD code
        // unconditionally excluded `cand == srcTs` from the scan below, so this
        // shape was NEVER found: `enumTs` stayed null and
        // `wrapUserClassAsIEnumerable` returned the raw, UNWRAPPED class
        // instance (`if (!enumTs) return src;`) — any iterator-vtable dispatch
        // on it then read past the object's small type-descriptor header
        // (confirmed ASan global-buffer-overflow, issue #2384's repro).
        auto structurallyIsEnumerator = [&](const sema::TypeSymbol* cand) {
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
            return hasMN && hasCur;
        };
        if (structurallyIsEnumerator(srcTs)) {
            enumTs = const_cast<sema::TypeSymbol*>(srcTs);
        } else {
            // Not self-implementing: scan all OTHER compiled TypeSymbols for a
            // class with MoveNext + Current (the two-class idiom, e.g. a
            // separate `IntRangeEnum` alongside `IntRange`).
            for (const auto& sym : ctx_.symbols()) {
                auto* cand = dynamic_cast<const sema::TypeSymbol*>(sym.get());
                if (!cand || cand->kind != ast::TypeDeclKind::Class) continue;
                if (cand == srcTs) continue;
                if (structurallyIsEnumerator(cand)) {
                    enumTs = const_cast<sema::TypeSymbol*>(cand);
                    break;
                }
            }
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
    // fix #2424: resolve the SOURCE class's own Dispose too (distinct from
    // enumDisposeFn whenever the two-class idiom applies — enumTs != srcTs).
    // Needed so a wrapper that owns a temporary source can dispatch srcTs's
    // own destructor/owned-field cascade (§28.2) before reclaiming its block,
    // not just the enumerator's.
    llvm::Function* srcDisposeFn = nullptr;
    {
        auto sdIt = disposerFns_.find(srcTs);
        if (sdIt != disposerFns_.end()) srcDisposeFn = sdIt->second;
    }

    // 7. Build type-specific wrapper functions keyed on the (source,
    // enumerator) class PAIR.
    //
    // fix #2424: widened from `enumTs->name` alone to `srcTs->name + "$" +
    // enumTs->name`. `srcDisposeFn` (above) is a SPECIFIC llvm::Function*
    // closed over directly into the generated `wrapDisposeFn`'s IR body
    // (mirroring how `enumDisposeFn` is already closed over) — safe only if
    // this cached wrapper is unique per source class. Keying on `enumTs`
    // alone is NOT enough: two unrelated source classes can legally return
    // the SAME enumerator class from GetEnumerator() (e.g. two IEnumerable<T>
    // classes both minting a shared `RangeEnum`), and `getOrCreate` reuses
    // whichever function was compiled FIRST by name — reusing an
    // `enumTs`-only-keyed wrapper across two different `srcTs` would bake in
    // the WRONG source's disposer. Widening the key gives each (source,
    // enumerator) pair its own generated functions, closing that hazard; the
    // common case (one source class per enumerator class) is unaffected since
    // the pair is still unique and the cache still hits on every repeat wrap.
    std::string key = std::string(srcTs->name) + "$" + std::string(enumTs->name);
    std::string stName = "userenumenum." + key;
    auto* stTy = llvm::StructType::getTypeByName(llvm_, stName);
    if (!stTy)
        // §29.11 (#943): { ptr vtable, ptr enumerator, ptr source, i1 ownsSrc }
        // — the source object is retained so GetEnumerator (slot 4) can
        // re-run the source's own GetEnumerator() to hand out an independent
        // enumerator on re-iteration. fix #2424: slot 3 (`ownsSrc`) is a
        // per-INSTANCE runtime flag — true only on the ORIGINAL wrapper
        // returned directly from THIS call when the caller determined `src`
        // is an unowned temporary; every clone `wrapGetEnumFn` mints always
        // stores `false` here regardless (see below) — only the ORIGINAL
        // binding's single scope-end disposal may ever free `src`, never a
        // re-iteration clone (which would multiply-free the shared source).
        stTy = llvm::StructType::create(
            llvm_, {ptrTy(), ptrTy(), ptrTy(), i1()}, stName);

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

    // Dispose: call enum.Dispose(), reclaim the inner enumerator block, then
    // free(self).
    //
    // fix #2389 (§28.2 Dispose contract / §28.3, §28.6 "every heap object is
    // disposed exactly once", v0.235): `enumDisposeFn` is `disposerFns_[enumTs]`
    // — the flag-guarded PUBLIC Dispose (destructor + owned-field disposal,
    // §28.2), which only DISPATCHES; per emitDynamicDispose's own contract
    // ("This does NOT free the object's heap block") it never reclaims the
    // block. Every other terminal disposal site pairs that dispatch with a
    // reclaim (emitDisposeAndReclaim, LlvmBackend_Disposal.cpp); this wrapper
    // was the one terminal site that dispatched Dispose on the retained inner
    // enumerator (`enumPtr`, slot 1) and then freed only the WRAPPER struct
    // (`self`) — every IntRangeEnum-shaped instance minted along the way (the
    // eager fetch this wrapper's own construction performs, plus one per
    // wrapGetEnumFn clone) leaked unconditionally (confirmed under LSan,
    // issue #2389's repro: 72 bytes / 3 allocations for the two-class idiom).
    //
    // This wrapper IS the sole owner of `enumPtr`'s block UNLESS `enumPtr`
    // ALIASES the retained source object at slot 2 — the single-object
    // self-returning idiom (#2384: a class implementing BOTH `IEnumerable<T>`
    // and `IEnumerator<T>` whose own `GetEnumerator()` returns `this`).
    // There `enumPtr == srcObj` by runtime identity (a compile-time-only
    // structural fact cannot distinguish this from a fresh-instance return,
    // mirroring the `enumPtr == srcPtr` RUNTIME guard `lowerIEnumerableFor`
    // already uses for the same alias, LlvmBackend_Stmts.cpp), and `srcObj`
    // is independently owned — reclaimed by ITS OWN scope-end/using-var
    // disposal — so freeing it again here would be a double-free. The
    // two-class idiom (the common case, e.g. IntRange/IntRangeEnum) never
    // aliases: `enumPtr` is always a distinct, solely-owned allocation this
    // wrapper is the sole owner of, so the runtime guard below is a no-op
    // there and the block is freed unconditionally, closing the leak.
    //
    // `self`'s own reclaim (just below) has always been unconditional,
    // ignoring `checkedDisposal_`'s debug-mode quarantine (§28.2 v0.134); the
    // inner enumerator's reclaim added here matches that existing, established
    // convention for this wrapper rather than introducing new asymmetric
    // gating not part of this fix's scope.
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
            auto* freeFnTy = llvm::FunctionType::get(
                llvm::Type::getVoidTy(llvm_), {ptrTy()}, false);
            // fix #2389: reclaim enumPtr's block unless it aliases the
            // retained source (slot 2) — see the block comment above.
            llvm::Value* srcF = b.CreateStructGEP(stTy, self, 2);
            llvm::Value* srcObj = b.CreateLoad(ptrTy(), srcF);
            llvm::Value* aliasesSrc =
                b.CreateICmpEQ(enumPtr, srcObj, "wrapdispose.enum_is_src");
            auto* freeEnumBB =
                llvm::BasicBlock::Create(llvm_, "wrapdispose.freeenum", f);
            auto* afterEnumBB =
                llvm::BasicBlock::Create(llvm_, "wrapdispose.afterenum", f);
            b.CreateCondBr(aliasesSrc, afterEnumBB, freeEnumBB);
            b.SetInsertPoint(freeEnumBB);
            b.CreateCall(freeFnTy, freeFnCallee, {enumPtr});
            b.CreateBr(afterEnumBB);
            b.SetInsertPoint(afterEnumBB);
            // fix #2424 (§28 preamble/§28.9, §29.1, v0.235): when THIS
            // specific instance owns `srcObj` (slot 3, `ownsSrc` — true only
            // for the original wrapper bound to an unnamed temporary source;
            // always false on a re-iteration clone, see wrapGetEnumFn below),
            // it is `srcObj`'s ONLY disposer anywhere in the program — no
            // named local/param/field/using-var owns it independently. Two
            // sub-cases:
            //   * aliasesSrc (self-returning idiom, `enumPtr == srcObj`):
            //     `enumDisposeFn(enumPtr)` above ALREADY dispatched `srcObj`'s
            //     own Dispose (enumTs == srcTs when aliased, so the two
            //     disposer functions are the same one) — only the physical
            //     reclaim (skipped above by the alias guard) remains.
            //   * !aliasesSrc (two-class idiom): `srcObj` is a genuinely
            //     distinct object whose Dispose was never dispatched by
            //     anything above — dispatch `srcDisposeFn` (srcTs's own
            //     §28.2 Dispose) here, then reclaim.
            llvm::Value* ownsF = b.CreateStructGEP(stTy, self, 3);
            llvm::Value* ownsSrcVal = b.CreateLoad(b.getInt1Ty(), ownsF);
            auto* ownSrcBB =
                llvm::BasicBlock::Create(llvm_, "wrapdispose.ownsrc", f);
            auto* afterSrcBB =
                llvm::BasicBlock::Create(llvm_, "wrapdispose.aftersrc", f);
            b.CreateCondBr(ownsSrcVal, ownSrcBB, afterSrcBB);
            b.SetInsertPoint(ownSrcBB);
            {
                auto* dispatchSrcBB =
                    llvm::BasicBlock::Create(llvm_, "wrapdispose.srcdispatch", f);
                auto* reclaimSrcBB =
                    llvm::BasicBlock::Create(llvm_, "wrapdispose.srcreclaim", f);
                b.CreateCondBr(aliasesSrc, reclaimSrcBB, dispatchSrcBB);
                b.SetInsertPoint(dispatchSrcBB);
                if (srcDisposeFn) {
                    b.CreateCall(srcDisposeFn->getFunctionType(), srcDisposeFn,
                                 {srcObj});
                }
                b.CreateBr(reclaimSrcBB);
                b.SetInsertPoint(reclaimSrcBB);
                b.CreateCall(freeFnTy, freeFnCallee, {srcObj});
                b.CreateBr(afterSrcBB);
            }
            b.SetInsertPoint(afterSrcBB);
            b.CreateCall(freeFnTy, freeFnCallee, {self});
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
    // value therefore replays from the start. The source is disposed once — either
    // at its own independent scope end (a NAMED source), or by the ORIGINAL
    // wrapper's own disposal (an owned temporary, fix #2424, `ownsSrc`) — and
    // EITHER WAY that is never this clone's job: each loop disposes only its own
    // wrapper+inner enumerator.
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
            // fix #2424: the clone NEVER owns `srcObj`, regardless of whether
            // `self` (the wrapper this clone was minted from) does. Only the
            // ORIGINAL binding's own scope-end disposal may free the shared
            // source — every clone disposing it too would multiply-free it
            // (confirmed as the exact hazard the multiple-live-enumerators
            // axis targets; see the adversarial-grid tests).
            b.CreateStore(b.getFalse(), b.CreateStructGEP(stTy, clone, 3));
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

    // 10. Allocate wrapper struct and fill fields (vtable, enumerator, source,
    // ownsSrc).
    uint64_t stSz = module_->getDataLayout().getTypeAllocSize(stTy);
    auto* callocFn2 = declareC("calloc", ptrTy(), {i64(), i64()});
    llvm::Value* wrap = builder_.CreateCall(callocFn2,
        {builder_.getInt64(1), builder_.getInt64(stSz)}, "userenumenum");
    builder_.CreateStore(vtGV, builder_.CreateStructGEP(stTy, wrap, 0));
    builder_.CreateStore(enumPtr, builder_.CreateStructGEP(stTy, wrap, 1));
    builder_.CreateStore(src, builder_.CreateStructGEP(stTy, wrap, 2));
    // fix #2424: only THIS original instance's `ownsSrc` reflects the
    // caller's named-vs-temporary determination — every clone this wrapper
    // later mints (wrapGetEnumFn) hardcodes `false` regardless (see above).
    builder_.CreateStore(ownsSrc ? builder_.getTrue() : builder_.getFalse(),
                          builder_.CreateStructGEP(stTy, wrap, 3));
    return wrap;
}


// fix #1302 / §22.3 (v0.228) "**`ref` local and field targets alias; accessor
// targets bracket.**" — the copy-IN half.
//
// §22.3's "`ref` and `out` argument locations" admits "an indexer slot, or a
// property with an accessible setter" as `ref`/`out` argument locations, and
// the paragraph that follows fixes their semantics: such a target "is instead
// **copy-in / copy-out** under §17.6's evaluate-once discipline: the receiver
// and every index expression are evaluated exactly once, in §22.5's call-site
// textual order; for `ref`, the getter is called once to produce the callee's
// slot value (for `out` no getter call occurs); on **normal return** the setter
// is called once with the slot's final value."
//
// Codegen had no such bracket. `lvalueAddr` resolves only locals, fields,
// statics and INLINE-array elements (a real address exists for each), so every
// accessor-mediated target — a heap-collection slot (`arr[0]`, `dict[k]`), a
// user-defined indexer, a block-form property — returned null and the whole
// call failed with buildOrderedArgs' "#1386" internal-error diagnostic. (Before
// the #1682 §22.3 category check landed, the same null instead let the call
// through with a garbage/temporary address and the callee's write was silently
// DROPPED — the WRONG_VALUE #1302 was filed for.)
//
// This helper emits the copy-in half and records what the copy-out half needs:
//   * the receiver and every index expression are lowered ONCE here and PINNED
//     in `exprValueOverride_`, so the getter below AND the setter in
//     emitAccessorRefArgWriteBacks reuse those values rather than re-evaluating
//     a side-effecting subexpression (`M(ref a[Next()])` must call `Next()`
//     once, not twice, and the get and the set must address the SAME slot —
//     the identical hazard #1299 fixed for `a[i] OP= v`);
//   * for `ref` the getter runs once (driving whichever existing read path the
//     location has — bz_array_get / bz_dict_try_get_* / a user indexer getter /
//     a property getter) and its value is stored into the callee's slot; for
//     `out` NO getter call occurs, and the slot is zero-initialized so the
//     callee's by-reference slot never starts as garbage;
//   * the slot's address is what the callee receives, so the callee's writes
//     land in the caller's frame and nothing aliases the collection.
//
// Returns the slot address, or null when `target` is not an accessor-mediated
// location this bracket can drive (caller falls through to #1386).
llvm::Value* Emitter::emitAccessorRefArgCopyIn(
    const ast::Expr& target, const ast::Param& param, bool isOut,
    std::vector<AccessorRefArg>& sink) {
    AccessorRefArg rec;
    rec.target = &target;

    // Pin one already-lowered subexpression value so a later re-lowering of the
    // same AST node (inside the getter here, or the setter after the call)
    // returns it instead of re-running the expression. // per §22.3 / §17.6
    auto pin = [&](const ast::Expr* e) -> bool {
        if (!e) return true;
        llvm::Value* v = lowerExpr(*e);
        if (!v) return false;
        exprValueOverride_[e] = v;
        rec.pins.push_back(e);
        return true;
    };
    auto unpinAll = [&]() {
        for (const ast::Expr* e : rec.pins) exprValueOverride_.erase(e);
    };

    // --- Classify the location and evaluate its subexpressions exactly once ---
    if (auto* ix = dynamic_cast<const ast::IndexExpr*>(&target)) {
        // An indexer slot: the receiver, then every index expression, in
        // call-site textual order (§22.3 / §22.5).
        if (!ix->target || ix->indices.empty()) return nullptr;
        if (!pin(ix->target.get())) { unpinAll(); return nullptr; }
        for (const auto& iv : ix->indices)
            if (!pin(iv.value.get())) { unpinAll(); return nullptr; }
    } else if (auto* ps = dynamic_cast<sema::PropertySymbol*>(
                   ctx_.exprSymbol(target))) {
        // "a property with an accessible setter" (§22.3). An indexer is a
        // PropertySymbol too but arrives as the IndexExpr above.
        if (ps->isIndexer) return nullptr;
        // A STATIC property is declined, deliberately and uniformly. §22.3
        // excludes "no **mutable static** admitted by §22.12 rule 2, whose
        // store semantics are defined for direct assignment only" — rule 2
        // names "a mutable … static field or auto-property", so a mutable
        // static AUTO-property is plainly excluded while a BLOCK-form static
        // property (whose setter body does the storing) is not clearly either
        // way. Rather than commit to a reading, keep the status quo — both
        // spellings, `ref S` and `ref Box.S`, already fail the same way — and
        // decline HERE so the two spellings cannot diverge: only the qualified
        // one has a receiver expression to pin, so without this the bare-name
        // form would have been silently admitted and the qualified one
        // rejected. The ruling request is filed. // per §22.3 / §22.12 rule 2
        for (const auto& m : ps->modifiers)
            if (m == "static") return nullptr;
        // An implicit-`this` access has no receiver expression to evaluate.
        if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&target))
            if (ma->target && !pin(ma->target.get())) { unpinAll(); return nullptr; }
    } else {
        return nullptr;  // not accessor-mediated — caller reports #1386
    }

    // --- The callee's slot. Its value type is the LOCATION's type (what the
    //     setter consumes); the alloca is widened if the callee's declared
    //     parameter type is larger, so a callee storing through a wider
    //     parameter cannot write past the slot (the #2266 slot-clobber
    //     shape). // per §22.3
    llvm::Type* valueTy = nullptr;
    if (const sema::TypeRef* et = ctx_.exprType(target)) valueTy = lowerTypeRef(et);
    llvm::Type* paramTy = nullptr;
    if (param.type)
        if (const sema::TypeRef* pt = ctx_.resolvedType(*param.type))
            paramTy = lowerTypeRef(pt);
    if (!valueTy) valueTy = paramTy;
    if (!valueTy) { unpinAll(); return nullptr; }
    llvm::Type* slotTy = valueTy;
    if (paramTy && paramTy != valueTy &&
        module_->getDataLayout().getTypeAllocSize(paramTy) >
            module_->getDataLayout().getTypeAllocSize(valueTy))
        slotTy = paramTy;

    llvm::AllocaInst* slot = entryAlloca(slotTy);
    builder_.CreateStore(llvm::Constant::getNullValue(slotTy), slot);

    // --- `ref`: one getter call, into the slot. `out`: no getter (§22.3). ---
    if (!isOut) {
        llvm::Value* cur = lowerExpr(target);
        if (!cur) { unpinAll(); return nullptr; }
        // The callee reads the slot at its PARAMETER's width, so an integer
        // read narrower than the slot is widened first; otherwise the value's
        // own type is stored (the alloca is at least that large by the sizing
        // above). A shape neither equal nor integer-coercible is not one this
        // bracket can pass safely — decline rather than emit a mistyped store.
        if (cur->getType() != slotTy) {
            if (cur->getType()->isIntegerTy() && slotTy->isIntegerTy())
                cur = coerceNumericToType(cur, slotTy,
                                          isUnsignedTargetExpr(target));
            else if (cur->getType() != valueTy) { unpinAll(); return nullptr; }
        }
        builder_.CreateStore(cur, slot);
    }

    rec.slot = slot;
    rec.valueTy = valueTy;
    sink.push_back(std::move(rec));
    return slot;
}


// fix #1302 / §22.3 — the copy-OUT half. "on **normal return** the setter is
// called once with the slot's final value. Write-backs for multiple
// accessor-mediated arguments run in argument textual order … and on an
// exception exit no write-back occurs" — hence `emit` (false on the exception
// edge) and the in-order walk of `recs`, which buildOrderedArgs appended in
// call-site textual order.
//
// The setter is driven through the SAME helpers the ordinary `loc := v`
// statement uses, so every location kind the language supports writes back by
// its own rules (an owning collection's disposing set, a virtual/interface
// property's dynamic dispatch, a user indexer's inlined accessor body). The
// receiver and index subexpressions stay pinned until here, so those helpers'
// internal `lowerExpr` calls reuse the copy-in's values and address exactly the
// slot the getter read (§22.3's evaluate-once).
void Emitter::emitAccessorRefArgWriteBacks(std::vector<AccessorRefArg>& recs,
                                           bool emit) {
    for (auto& r : recs) {
        if (emit && r.slot && r.valueTy && r.target) {
            llvm::Value* v = builder_.CreateLoad(r.valueTy, r.slot, "refacc.wb");
            bool done = false;
            if (auto* ix = dynamic_cast<const ast::IndexExpr*>(r.target)) {
                // Mirrors lowerStmt's `obj[i] := v` order: a user-declared
                // indexer first (it may be virtual/interface-dispatched), then
                // the built-in heap collections (§4.4-§4.9).
                done = lowerIndexerSet(*ix, v) ||
                       lowerCollectionIndexerSet(*ix, v, /*valueExpr=*/nullptr);
            } else {
                // Mirrors lowerStmt's `obj.P := v` order: a virtual/interface
                // property setter dispatches dynamically (§12.1/§12.5/§23);
                // otherwise the statically-known accessor body is inlined.
                done = lowerVirtualPropertySet(*r.target, v) ||
                       lowerPropertySetValue(*r.target, v);
            }
            if (!done)
                ctx_.diagnostics().report(
                    Severity::Error, r.target->span.begin,
                    "internal: no setter for the `ref`/`out` accessor "
                    "write-back — #1302");
        }
        for (const ast::Expr* e : r.pins) exprValueOverride_.erase(e);
    }
    recs.clear();
}


bool Emitter::buildOrderedArgs(const std::vector<ast::Param>& params,
                      const std::vector<ast::CallArgument>& args,
                      std::vector<llvm::Value*>& out,
                      std::vector<const ast::Expr*>* outExprs,
                      bool nullTransferredOwned,
                      std::vector<AccessorRefArg>* accessorRefArgs) {
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
    // #1918 / §28.3: slots of bare owned local/param arguments transferred to an
    // `owned` parameter — nulled below at the "call about to run" point when
    // `nullTransferredOwned` is set (the ordinary user-method call path). See the
    // discharge block and the header comment.
    std::vector<llvm::AllocaInst*> transferredOwnedArgSlots;
    for (const auto& a : args) {
        std::size_t idx = params.size();
        if (a.name.empty()) {
            idx = pos++;
        } else {
            for (std::size_t j = 0; j < params.size(); ++j)
                if (params[j].name == a.name) { idx = j; break; }
        }
        if (idx >= params.size() || ordered[idx]) return false;
        // #1918 / §28.3: record the caller's slot for a bare owned LOCAL / owned
        // PARAMETER passed with an ownership TRANSFER (explicit `owned` argument
        // modifier, or a mapped `owned` callee parameter). After the whole
        // argument list is evaluated this slot is nulled (below), so the caller's
        // null-guarded unwindOnly disposal (registerUnwindOwnedLocal /
        // registerUnwindOwnedParam) — which UnwindIneligibleWalker now leaves
        // eligible for exactly this shape — skips the just-transferred value on
        // the transfer / post-transfer throw edge (no double-free with the
        // callee's own owned-param disposal) while still disposing a SIBLING-
        // branch value that is live at a throw (no leak). Restricted to a plain
        // `var` owned local or an `owned` parameter of pointer (class/interface)
        // representation — exactly what those registrations cover; a `using var`,
        // collection, or value-typed binding is untouched. // per §28.3
        if (nullTransferredOwned && a.value && !a.isDiscard &&
            (a.modifier == "owned" || params[idx].modifier == "owned")) {
            if (auto* id =
                    dynamic_cast<const ast::IdentifierExpr*>(a.value.get())) {
                sema::Symbol* asym = ctx_.exprSymbol(*id);
                bool eligible = false;
                if (auto* loc = dynamic_cast<sema::LocalSymbol*>(asym))
                    eligible = loc->decl && !loc->isUsing;
                else if (auto* ps = dynamic_cast<sema::ParameterSymbol*>(asym))
                    eligible = (ps->modifier == "owned");
                if (eligible)
                    if (llvm::AllocaInst* s = localSlot(*id)) {
                        transferredOwnedArgSlots.push_back(s);
                        // #1990 / #1992 / §28.3: a bare owned local/param whose
                        // ownership transfers into an `owned` argument has its
                        // auto-dispose registered by registerUnwindOwnedLocal /
                        // registerUnwindOwnedParam in the scope where it was
                        // DECLARED (the whole-method / enclosing block). #1918
                        // relies on that registration firing on a later-arg throw.
                        // But when the transfer site sits inside a `try` whose
                        // `catch` is in that SAME enclosing scope, a throw from a
                        // LATER argument unwinds only to the in-method handler —
                        // it does NOT exit the declaring scope — so that
                        // registration never fires, and (because sema treats the
                        // local as moved past the `owned` transfer) neither does
                        // its normal-exit discharge: the still-owned value leaks on
                        // the caught path (#1990: one owned arg; #1992: all but the
                        // last of several). Mirror the #993 owned-TEMP handling:
                        // register a TRANSIENT unwind-only disposal for this local
                        // in the CURRENT scope (disposeStack_.back(), the scope the
                        // later argument's emitPendingCheck unwinds through on BOTH
                        // the caught and propagate edges), on the local's OWN slot.
                        // On the caught arg-throw it disposes the value exactly
                        // once; on a propagate throw it disposes + NULLS the slot,
                        // so the outer declaring-scope disposal (null-guarded) then
                        // skips — no double-free. On the normal (transfer-completes)
                        // path it is unwind-only (never fires on normal exit) and
                        // the slot is nulled below, so a subsequent unwind through
                        // this scope reloads null and skips too. Not discharged
                        // here (unlike the #993 temp): the entry must be left in
                        // place null-guarded so a SIBLING branch that did NOT
                        // transfer keeps a live disposal (mirrors why #1918 nulls
                        // at runtime instead of erasing the registration).
                        // Restricted to a vtable-dispatchable user class/interface
                        // instance OR a compiler-known builtin owned handle
                        // (ChannelSender<T> / Channel<T> / Shared<T> / Weak<T>,
                        // recognised by head name — same detector as the using-var
                        // path and #1957's registerUnwindOwnedParam branch) —
                        // exactly the set `unwindOnly` dispose covers (class via
                        // vtable slot 2, builtin via the type-specific runtime
                        // release in the extended unwindOnly block below). A bare
                        // owned LOCAL of one of these builtin handle types has no
                        // registerUnwindOwnedLocal registration (unwindOwnedLocalClass
                        // returns null — no user TypeSymbol) and no registerUnwindOwnedParam
                        // registration (it is a local, not a param), so without
                        // THIS transient the local is never released on the caught
                        // arg-throw — the #2000 leak. An erased generic is
                        // left to its declaring-scope registration (no class symbol
                        // and no builtin name). // per §28.3 / §28.4 / §28.6 / §28.13
                        if (!disposeStack_.empty()) {
                            const sema::TypeRef* at = ctx_.exprType(*id);
                            bool classOrInterface =
                                at &&
                                at->kind == sema::TypeRef::Kind::Named &&
                                at->declaration &&
                                (at->declaration->kind ==
                                     ast::TypeDeclKind::Class ||
                                 at->declaration->kind ==
                                     ast::TypeDeclKind::Interface);
                            // Mirror the builtinHead detector in
                            // registerUnwindOwnedParam (#1957). A builtin named
                            // type carries an astNode + head name but NO user
                            // TypeSymbol (at->declaration is null) — that is the
                            // shape ChannelSender/Channel/Shared/Weak resolve to.
                            std::string_view bhead;
                            if (at && at->kind == sema::TypeRef::Kind::Named &&
                                !at->declaration &&
                                at->astNode && at->astNode->name &&
                                !at->astNode->name->parts.empty()) {
                                bhead = at->headName();
                            }
                            bool builtinShared    = bhead == "Shared";
                            bool builtinChannel   = bhead == "Channel";
                            bool builtinSender    = bhead == "ChannelSender";
                            bool builtinWeak      = bhead == "Weak";
                            bool builtin =
                                builtinShared || builtinChannel ||
                                builtinSender  || builtinWeak;
                            if (classOrInterface || builtin) {
                                Disposal d{s, nullptr};
                                d.unwindOnly = true;
                                if (builtin) {
                                    d.sharedHandle  = builtinShared;
                                    d.channel       = builtinChannel;
                                    d.channelSender = builtinSender;
                                    d.weakHandle    = builtinWeak;
                                }
                                disposeStack_.back().push_back(d);
                            }
                        }
                    }
            }
        }
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
            llvm::AllocaInst* scratch = entryAlloca(slotTy);
            // #1467 / §22.4 / §28.3: `out _` against an `out owned` CLASS or
            // INTERFACE parameter. The callee writes a fresh OWNED heap object
            // into this scratch slot; the discard binds it to nothing, so §28.3
            // makes it an unbound owned temporary that must be disposed at the
            // end of the full statement — otherwise the object is neither
            // disposed (its destructor never runs) nor reclaimed (leak, #1467).
            // Null-initialize the slot and record it so lowerCall disposes it
            // (dynamic vtable Dispose + reclaim, null-guarded, exactly once)
            // right after the call. Definite assignment (§22.4) guarantees the
            // callee writes null-or-value into the out param before a normal
            // return, so the null-guard reclaims a produced value and skips the
            // null a false-returning `Try*` leaves. Gated to a user class /
            // interface (dynamic disposal dispatches through vtable slot 2): a
            // value-typed or builtin-collection/handle `out owned` is left
            // untouched here. A REAL `out owned var x` binding is not a discard
            // (a.value is set → ensureOutVarSlots), so its ownership transfer is
            // never touched by this path (no double-dispose). per §28.3
            if (params[idx].modifier == "out owned" &&
                slotTy == ptrTy() && pt &&
                pt->kind == sema::TypeRef::Kind::Named && pt->declaration) {
                if (auto* td = dynamic_cast<const ast::TypeDecl*>(
                        pt->declaration->decl)) {
                    if (td->kind == ast::TypeDeclKind::Class ||
                        td->kind == ast::TypeDeclKind::Interface) {
                        builder_.CreateStore(
                            llvm::ConstantPointerNull::get(ptrTy()), scratch);
                        discardOwnedOutSlots_.push_back(scratch);
                        // #1833 (a) / §22.4 / §28.3 / §17.13: the normal-path
                        // disposal above (`discardOwnedOutSlots_` loop in
                        // lowerCall) fires only when the callee RETURNS. If the
                        // callee WRITES the scratch (assigns a fresh owned heap
                        // value into `r`) and then THROWS, the scratch holds an
                        // owned value nobody will dispose — the LSan Direct
                        // leak (bz$W$TryMakeThrow). Per §28.3 the value's
                        // ownership already transferred to the caller the moment
                        // the callee assigned it; the caller's ordinary
                        // exception-path discharge must handle it on unwind.
                        // Register an `unwindOnly dynamicDispose` entry against
                        // the same scratch in `disposeStack_.back()`: it is
                        // null-guarded (a callee that did not write keeps the
                        // slot null and the disposal no-ops; a false-returning
                        // `Try*` callee that wrote null also skips), so a
                        // throw-after-assign path disposes the value EXACTLY
                        // ONCE on the unwind edge. The `unwindOnly` flag
                        // guarantees the normal-return path is unaffected
                        // (the discardOwnedOutSlots_ loop handles that).
                        // Mirrors the `out owned var x` case (handled earlier
                        // in ensureOutVarSlots): the inline recipient already
                        // registers an `unwindOnly` disposal, so the
                        // throw-after-assign repro there is sound — only the
                        // discard case was uncovered. The discard and the
                        // real-binding share the same scratch alloca, so a
                        // nested `out _, out owned var x` keeps the unwind
                        // coverage for both. // per §22.4, §28.3, §28.10,
                        // #1833 (a), #1467
                        if (!disposeStack_.empty()) {
                            Disposal d{scratch, nullptr};
                            d.unwindOnly = true;
                            d.dynamicDispose = true;
                            disposeStack_.back().push_back(d);
                        }
                    }
                }
            }
            ordered[idx] = scratch;
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
            // fix #1302 / §22.3: `lvalueAddr` covers exactly the categories
            // that ALIAS — "a `ref` argument naming a mutable local or writable
            // field is a true alias" (plus §15.1's inline-array element, which
            // is a slot inside such a location). The remaining admitted §22.3
            // categories — "an indexer slot, or a property with an accessible
            // setter" — have no address to alias; §22.3 gives them copy-in /
            // copy-out instead, which this bracket emits. The write-back is
            // flushed by the caller after the call returns normally.
            //
            // Restricted to `ref` and PLAIN `out`: §22.3 closes the list for
            // the owned variant — "for `out owned`, the accepted forms remain
            // exactly the inline `out using var x` / `out owned var x` of
            // §22.4" — so an accessor-mediated `out owned` argument is NOT an
            // admitted location and must not be bracketed into one (the
            // callee's fresh owned value would be stored into a borrowing slot
            // with nobody to discharge it, §28.6). It keeps today's
            // compile-time rejection through the #1386 fallback below; the
            // missing dedicated §22.3 diagnostic is filed separately.
            if (!v && accessorRefArgs &&
                (params[idx].modifier == "ref" || params[idx].modifier == "out"))
                v = emitAccessorRefArgCopyIn(
                    *a.value, params[idx],
                    /*isOut=*/params[idx].modifier == "out", *accessorRefArgs);
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
            //
            // #1923 / stdlib §5 / §29.11 / §28.3: a lazy LINQ pipeline
            // (Select/Where/… → a bz_lazy_t* sentinel, isLazyEnumerableType)
            // passed as a pure `IEnumerable<T>` ARGUMENT — the sibling of #1911
            // (local) and the #1916 RETURN half. The lazy vtable has only 4 slots
            // (td/Dispose/MoveNext/Current, Current-by-pointer); the callee's
            // for-in over the `IEnumerable<T>` param drives the §29.11 vtable and
            // reads slot 4 (GetEnumerator) out of bounds → a garbage
            // function-pointer call → SIGSEGV. So drain the pipeline once into a
            // fresh owned Array<T> and wrap it in the OWNING vtable-backed
            // enumerator (wrapAsIEnumerableForTarget with ownLazyPipeline=true).
            //
            // DISPOSAL CONTRACT (why this differs from the RETURN half, #1916): a
            // callee `IEnumerable<T>` parameter is BORROWED — the callee's for-in
            // takes the no-dispose sourceIsNamedLocal path and never disposes it,
            // and borrow-checking (§28.3) forbids the callee from retaining it
            // past the call. So the owning enumerator is a CALLER temporary the
            // CALLER must dispose exactly once, after the call, on BOTH the
            // normal-return and the exception-unwind edges — else the drained
            // array leaks. Register it as a scope-end iterEnumDispose (the same
            // slot-1 Dispose the local-decl #1911 source uses via
            // registerIterableSourceDisposal): its slot-1 Dispose frees the
            // drained array AND the enumerator frame, null-guarded, once. It is
            // NOT unwindOnly and NOT in argTempSlots, so the pre-call discharge
            // leaves it intact and it fires at scope end on the normal edge; a
            // LATER argument (or the callee) that throws unwinds through this
            // scope and fires it on the exception edge. Either way: exactly once,
            // no double-free (the callee never disposes a borrowed arg), no leak.
            // Mirrors the #1019 borrow-parameter `new T(...)` arg-temporary
            // disposal already registered below.
            bool lazyArgWrapped =
                formalTy &&
                formalTy->kind == sema::TypeRef::Kind::Named &&
                !formalTy->declaration && formalTy->astNode &&
                formalTy->astNode->name &&
                !formalTy->astNode->name->parts.empty() &&
                formalTy->headName() == "IEnumerable" &&
                isLazyEnumerableType(ctx_.exprType(*a.value));
            //
            // fix #1069 / stdlib §4.10: the SAME contract now also covers a
            // DICT-backed `Set<T>` argument — its wrap snapshots the key table
            // into a fresh array behind an OWNING enumerator, a caller temporary
            // with the identical disposal obligation. `owningArgWrap` is the
            // wrap's own report of "this result is owned"; it is true for exactly
            // the lazy-pipeline case (so the pre-existing behaviour is unchanged)
            // and the new set-snapshot case. A CLASS-element set takes the
            // BORROWING array wrap and reports false — nothing extra to dispose.
            bool owningArgWrap = false;
            v = wrapAsIEnumerableForTarget(v, formalTy, a.value.get(),
                                           /*ownLazyPipeline=*/lazyArgWrapped,
                                           &owningArgWrap);
            if (owningArgWrap && v && v->getType()->isPointerTy() &&
                !disposeStack_.empty()) {
                llvm::AllocaInst* eslot = entryAlloca(v->getType());
                builder_.CreateStore(v, eslot);
                Disposal d{eslot, nullptr};
                d.iterEnumDispose = true;  // slot-1 Dispose: free drained array + frame
                disposeStack_.back().push_back(d);
            }
            // §9 / §9.1 (#1759): passing `this` of a record method by value —
            // `this` lowered to the receiver POINTER (CG29 / §9.2), but a
            // by-value record parameter expects the record STRUCT. Load the
            // struct so the argument is a value-semantics copy, not the 8-byte
            // pointer (which fails IR verification: `call … (ptr)` vs a
            // `%record.T` parameter). A no-op for every argument that is not the
            // receiver pointer, or a parameter not typed as that record's struct.
            if (formalTy)
                v = coerceThisRecordValue(v, lowerTypeRef(formalTy));
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
    // #1918 / §28.3: now that every argument is evaluated (so a THROW while
    // evaluating a LATER argument has already unwound with the owned local still
    // LIVE — its disposal correctly fired), and the call is ABOUT TO RUN, null the
    // caller's slot for each bare owned local/param transferred to an `owned`
    // parameter. This store dominates the call's unwind edge, so if the CALLEE
    // throws (it now owns the value) the caller's null-guarded unwindOnly disposal
    // reads null and skips — dispose-exactly-once with the callee's owned-param
    // disposal. On a sibling branch where the transfer never runs, the slot stays
    // live and the unwind disposal reclaims it (the #1918 leak). Mirrors how an
    // explicit `x.Dispose()` / `_ := x` discharge nulls the slot for the same
    // null-guarded machinery. // per §28.3
    for (llvm::AllocaInst* slot : transferredOwnedArgSlots) {
        if (!slot) continue;
        llvm::Type* ety = nullptr;
        llvm::Value* addr = boxedStorage(slot, ety);
        if (addr && ety && ety->isPointerTy())
            builder_.CreateStore(
                llvm::ConstantPointerNull::get(ptrTy()), addr);
    }
    return true;
}


// #1955 / §28.3: null the caller's slot for a bare owned LOCAL / owned PARAMETER
// argument whose ownership just transferred at an owning-collection element-
// transfer lowering site (LlvmBackend_Calls.cpp Add/Push/Enqueue + Insert),
// mirroring the `transferredOwnedArgSlots` nulling buildOrderedArgs does for the
// user-method / constructor call paths. Restricted to a plain `var` owned local
// or an `owned` parameter of pointer representation — exactly the slots the
// unwindOnly disposal (registerUnwindOwnedLocal / registerUnwindOwnedParam)
// covers and that UnwindIneligibleWalker leaves eligible for this callee shape;
// a `using var`, collection, or value-typed binding is untouched (no slot
// nulled). // per §28.3
void Emitter::nullTransferredOwnedLocalSlot(const ast::Expr* argValue) {
    auto* id = dynamic_cast<const ast::IdentifierExpr*>(argValue);
    if (!id) return;
    sema::Symbol* asym = ctx_.exprSymbol(*id);
    bool eligible = false;
    if (auto* loc = dynamic_cast<sema::LocalSymbol*>(asym))
        eligible = loc->decl && !loc->isUsing;
    else if (auto* ps = dynamic_cast<sema::ParameterSymbol*>(asym))
        eligible = (ps->modifier == "owned");
    if (!eligible) return;
    llvm::AllocaInst* slot = localSlot(*id);
    if (!slot) return;
    llvm::Type* ety = nullptr;
    llvm::Value* addr = boxedStorage(slot, ety);
    if (addr && ety && ety->isPointerTy())
        builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), addr);
}


}  // namespace bezel::codegen
