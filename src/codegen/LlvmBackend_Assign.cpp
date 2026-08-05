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

// fix #2832 (§12.3, v0.240): is this indexer access `base[...]`? §12.3 —
// "Inside a derived member, `base.Name(...)` calls the base class's
// implementation, bypassing virtual dispatch" — so a `base`-rooted access is
// NON-VIRTUAL, and the two #1080 vtable-dispatch branches below must decline
// it (the base indexer shares its vtable slot with the derived override, so
// dispatching would run the override — the defect). Mirrors the `base`
// bail-outs in lowerVirtualPropertyGet / lowerVirtualPropertySet.
bool indexTargetIsBase(const ast::IndexExpr& ix) {
    auto* id = dynamic_cast<const ast::IdentifierExpr*>(ix.target.get());
    return id && id->name == "base";
}

// fix #2528 (§23.4 / §11.2 / §14.1, v0.236): an interface indexer's element
// type is declared in the interface generic parameter list, but an itable call
// is lowered from the concrete receiver type (`IFlag<bool>`). Do the positional
// substitution before lowering the signature; lowerType(T) otherwise returns
// the non-null i32 generic-erasure placeholder and locks the call to the wrong
// ABI. This covers both local AST declarations and imported side-table types.
const sema::TypeRef* interfaceIndexerElementType(
    sema::SemaContext& ctx, const ast::IndexExpr& ix,
    const sema::TypeSymbol* iface, const sema::PropertySymbol* ps) {
    if (!iface || !ps || !ix.target) return nullptr;
    const sema::TypeRef* elem = nullptr;
    if (auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl)) {
        if (pd->type) elem = ctx.resolvedType(*pd->type);
    } else {
        elem = ctx.importedMemberType(ps);
    }
    if (!elem || elem->kind != sema::TypeRef::Kind::Named ||
        elem->declaration || iface->genericParams.empty())
        return elem;
    const sema::TypeRef* recvTy = ctx.exprType(*ix.target);
    if (!recvTy || recvTy->genericArgs.empty()) return elem;
    const std::string_view elemName = elem->headName();
    for (std::size_t i = 0; i < iface->genericParams.size(); ++i) {
        auto* gp = dynamic_cast<const sema::GenericParamSymbol*>(
            iface->genericParams[i]);
        if (gp && gp->name == elemName && i < recvTy->genericArgs.size())
            return recvTy->genericArgs[i];
    }
    return elem;
}
}  // namespace

// fix #2437/#2442 (§11.2/§23.4, v0.235): find the class `ts`'s (or an
// inherited base's) indexer whose FULL parameter list — arity AND every
// parameter type (§23.4: "Indexers may be overloaded by parameter type
// list") — matches `ifaceIdx`'s. Two defects that both live in this one
// function (and its only caller, classIndexerImplementing), fixed in two
// sequenced commits — #2437 first, #2442 (this one, the base-class walk)
// building on top of it once landed:
//
//   #2437 (arity-blind): the prior match compared ONLY the first index
//   parameter's type, so an interface obligation `[i:int,j:int]` matched a
//   class's narrower `[i:int]` overload whenever the two shared a first
//   parameter type — confirmed: installed the WRONG getter into the itable
//   slot (the issue's repro returned 77, the `[i:int]` getter's constant,
//   instead of 17 from `[i:int,j:int]`'s `5+i+j`), silently, no diagnostic.
//   There is also no more "fall back to the first indexer found when
//   nothing matches exactly": that fallback was an UNBOUNDED version of the
//   same defect (it would accept ANY indexer of ANY shape, once no exact
//   match existed, as if it implemented the obligation) — installing a
//   getter/setter whose actual parameter types differ from what an
//   interface caller passes is itable-level type confusion, strictly worse
//   than a null slot (which #2438's sema-side mirror already turns into a
//   compile-time reject rather than letting a mismatched candidate reach
//   codegen at all).
//
//   #2442 (no base walk): scoped to `ts->members` only, unlike its sibling
//   `classPropertyByName`, so a class satisfying the obligation ONLY
//   through an inherited indexer (declared on a base, never redeclared)
//   never found a candidate at all — SIGSEGV (confirmed by probe, exit
//   139): declareVtables Pass D installs `ConstantPointerNull` into BOTH
//   the getter and setter slots whenever this function returns null.
//
// A candidate reached ONLY through an IMPORTED base class needed a THIRD,
// later fix (#2449) to actually wire: `ensureIndexerGetterFn`/`-SetterFn`
// build a body from `ps->decl`, which is always null for an imported
// symbol (ModuleLoader reconstructs no AST — mirrors Property's #503
// limitation). #2449 admits such a candidate here only when it is
// wireable through #2414's non-virtual external-stub ABI
// (`declareImportedIndexerGetters`/`-Setters`, `bz$<Class>$idxget$<arity>`/
// `$idxset$<arity>`) — see `indexerCandidateWireable` below and
// `Emitter::indexerItableAccessor` (LlvmBackend_Setup.cpp), which resolves
// the actual function to install (local body vs. imported stub) once a
// candidate is selected. A VIRTUAL imported indexer is deliberately still
// excluded (dispatches through a vtable slot, not this ABI — devirtualizing
// it into a static itable-slot constant is a separate, harder problem, out
// of scope here and for #2414 alike).
//
// Returns null when no member, own or inherited, matches by full parameter
// list AND is wireable. `checkAbstractImplementations`'s
// ifaceIndexerObligations loop (sema, TypeResolution_Checks.cpp)
// deliberately mirrors this exact selection — see that function's comment
// for why the two must move together — so a null return here always
// corresponds to a program sema has already rejected, never a
// silently-wrong itable install.
const sema::PropertySymbol* Emitter::findIndexer(
    const sema::TypeSymbol* ts, const sema::PropertySymbol* ifaceIdx) const {
    if (!ts || !ifaceIdx) return nullptr;
    const std::size_t nparams = indexerParamCount(ifaceIdx);
    for (const sema::TypeSymbol* t = ts; t; t = baseClassOf(t)) {
        for (const sema::Symbol* m : t->members) {
            auto* ps = dynamic_cast<const sema::PropertySymbol*>(m);
            if (!ps || !ps->isIndexer) continue;
            if (!indexerCandidateWireable(ps)) continue;
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
// borrowing Array<string> overwrite. `valIsFreshPlusOneString` (fix #2596): set
// by the dict COMPOUND path whose `val` is an emitStringConcat result (always
// +1 per #422) — skips the #429 retain so the +1 transfers into the slot.
bool Emitter::lowerCollectionIndexerSet(const ast::IndexExpr& ix,
                                        llvm::Value* val,
                                        const ast::Expr* valueExpr,
                                        bool valIsFreshPlusOneString) {
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
        // #2598 / §4.1 / §4.4: an IList<string> receiver routes its indexer set
        // through the SAME bz_array_set ABI (issue #401 — IList<T> is array-backed,
        // same bz_array_t* as Array<T>), so the same disposing-setter path applies.
        // The predicate previously gated on a head-name allowlist of concrete
        // borrowing families (Array/Stack/Queue); IList<T> (the borrowing list
        // interface, §4.1) is also array-backed and borrowing, so it is admitted.
        // IReadOnlyList<T> is read-only (never reaches the setter); IOwningList<T>
        // is owning (handled by the separate elemDisp path below, which runs the
        // owning-family disposer against the class element — never a string).
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
                [](std::string_view n){
                    // #2598: IList<T> is array-backed + borrowing (issue #401),
                    // same bz_array_set ABI as Array<T>; admitted alongside the
                    // concrete borrowing families.
                    return n=="Array"||n=="Stack"||n=="Queue"||n=="IList";
                }(recvTy->headName());
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
    // fix #2596: the dict COMPOUND path passes valIsFreshPlusOneString=true
    // because its `val` is an emitStringConcat result (always rc=1 per #422) —
    // a retain here would double-count to +2 and leak one +1 per iteration.
    if (isStrVal && !valIsFreshPlusOneString &&
        (!valueExpr || !isPlusOneStringProducer(valueExpr)))
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

// fix #2832 / #2833 (§12.3 / §23.4, v0.240) — see the header for the contract.
const sema::TypeSymbol* Emitter::indexAccessReceiverDecl(
    const ast::IndexExpr& ix) {
    const ast::Expr* target = ix.target.get();
    if (!target) return nullptr;
    auto* id = dynamic_cast<const ast::IdentifierExpr*>(target);
    const bool viaBase = id && id->name == "base";
    const bool viaKeyword = id && (id->name == "this" || id->name == "base");

    if (const sema::TypeSymbol* recv = indexReceiverDecl(target)) {
        // fix #2832 (§12.3, v0.240): "Inside a derived member, `base.Name(...)`
        // calls the base class's implementation, bypassing virtual dispatch."
        // indexReceiverDecl's #747 R42 branch answers `base` with the DERIVED
        // type (it recovers "the class whose body this is"), so selectIndexer
        // was handed the derived type and picked its OVERRIDE — `base[i]` on a
        // `virtual` base indexer returned the override's value, silently, with
        // no diagnostic, on the one construct whose entire purpose is to bypass
        // the override. Take the §12.3 step to the class base here, mirroring
        // sema's own `base` receiver resolution (ExprTypecheck_Calls.cpp
        // `memberTargetType`, which walks `currentType_->baseTypes` for the
        // class base). Only the CLASS base is a candidate: an interface in
        // `baseTypes` declares no implementation for `base` to reach.
        //
        // The dispatch half is separate and lives at the two #1080 vtable
        // branches below (lowerIndexerGet / lowerIndexerSet), which this step
        // alone would NOT have fixed — the base's PropertySymbol shares its
        // vtable slot with the derived override, so a vtable-dispatched
        // `base[i]` still runs the override. Those branches now decline a
        // `base` receiver exactly as lowerVirtualPropertyGet / -Set already
        // decline `base.P` (§12.3), leaving the static getter/setter-body path
        // to bind the base's own accessor.
        if (!viaBase) return recv;
        for (const sema::TypeSymbol* b : recv->baseTypes)
            if (b && b->kind == ast::TypeDeclKind::Class) return b;
        // No class base: `base` is not legal here at all (§12.3 "`base` is
        // legal only inside instance members of a derived class"). Sema rejects
        // it; fall through rather than silently indexing the derived type.
    }

    // fix #2833 (§23.4 / §27.3, v0.240): the receiver could not be recovered
    // from the current llvm::Function. That happens for a LAMBDA body (whose
    // closure fn is not a value in methodFns_, and for which
    // currentSpecMethodSymbol_ is unset) and for an `async` body (a
    // state-machine frame, likewise not in methodFns_) — MEASURED, both, on
    // this tree: the read degraded to 0 and the write was silently discarded.
    //
    // Recover it from the indexer PropertySymbol sema attached to this
    // IndexExpr (#2530). This is CATEGORY-BLIND by construction — it asks the
    // type-checker what this very access resolved to rather than enumerating
    // codegen body kinds — so a lambda, a `do`-bodied lambda, a nested lambda,
    // an `async` body and any future body kind are all covered by the same
    // line, and the #2833 announcement below stops firing for them without
    // being weakened.
    //
    // It yields the indexer's DECLARING type, which for an INHERITED indexer is
    // a base of the receiver rather than the receiver itself. That is the same
    // PropertySymbol `selectIndexer` would have found by walking the receiver's
    // base chain (both walk `baseTypes` from the same root), and every use of
    // the returned symbol below is either keyed on the PropertySymbol itself
    // (the imported-stub and vtable-slot maps) or on `->kind`, which a class's
    // class base shares. Restricted to the `this`/`base` keyword: an ordinary
    // typed receiver already resolves through its exprType above, and this
    // fallback must not silently redirect one that legitimately failed.
    if (viaKeyword) {
        if (auto* ps = dynamic_cast<const sema::PropertySymbol*>(
                ctx_.exprSymbol(ix)))
            if (ps->isIndexer && ps->parent) return ps->parent;
    }

    // #2795 follow-up (§23.4, v0.240): ANNOUNCE THE DROP. Reaching here with a
    // `this`/`base`-rooted index means the receiver TypeSymbol could not be
    // recovered — and EVERY caller bails immediately on a null return, silently:
    //   * lowerIndexerGet returns null, so the READ degrades to a zero/null
    //     value through lowerLocalDecl's #2434 / emitReturn's #2491 fallbacks.
    //     The two fallbacks after it in lowerExpr cannot rescue a `this`
    //     receiver either — it has no builtin-collection exprType for
    //     lowerCollectionIndexerGet, and lvalueAddr yields the class struct
    //     type, not an llvm::ArrayType.
    //   * lowerIndexerSet returns false, so the WRITE is silently discarded.
    //   * lowerIndexerCompound returns false — documented at its own head as
    //     "silently a no-op (returns old value, no store)".
    //
    // WHY THIS EXISTS. #2795 gave `this[i]` a receiver type in sema, which is
    // correct, but it also removed the *incidental* #2491 note that used to
    // accompany the lambda case: that note fired on the CALLER's `return f(i);`
    // only because an untyped `this[n]` left the lambda's return type unknown.
    // With the type available the caller lowers cleanly and the wrong value
    // became FULLY SILENT — a net loss against this tree's standing rule that a
    // clean diagnostic beats a silent wrong value.
    //
    // GATED ON THE PROPERTY, NOT A SOURCE CATEGORY (the #2787 lesson), which is
    // why #2833's fix above could switch it off without weakening it: it tests
    // "the recovery failed", so making the recovery SUCCEED for the lambda and
    // `async` bodies retires it by construction for exactly those shapes and
    // keeps it armed for any receiver still genuinely unrecoverable (an
    // IndexExpr sema left without a resolved indexer PropertySymbol, which
    // today means a compile that already has errors — hence the guard below —
    // and any future body kind that breaks BOTH recoveries at once).
    //
    // Warning, suppressed once the compile already has errors — the #2491 /
    // #2775 precedent, so a cascade never buries the real diagnostic. That guard
    // also covers the shapes that reach here with no `currentFn_` at all: a
    // field initializer (already the §22.8 error) and a static member (already
    // the §12.3 error) stay single-diagnostic.
    if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(target)) {
        if ((id->name == "this" || id->name == "base") &&
            !ctx_.diagnostics().hasErrors()) {
            ctx_.diagnostics().report(
                Severity::Warning, id->span.begin,
                "internal: this `" + std::string(id->name) +
                "[i]` indexer access was DROPPED — the receiver could not be "
                "recovered, neither from the enclosing function nor from the "
                "resolved indexer; the read degrades to a zero/null value and "
                "a write is silently discarded (§23.4) — #2833");
        }
    }
    return nullptr;
}

// fix #2832 (§12.3 / §23.4 / §2.8.2, v0.240) — see the header for the contract.
bool Emitter::rejectImportedVirtualBaseIndexer(
    const ast::IndexExpr& ix, const sema::PropertySymbol* ps) {
    // Gated on the PROPERTY that actually blocks the lowering — "the only
    // entry point I have dispatches virtually" — not on a source category.
    //   * an IMPORTED indexer has no ast::PropertyDecl (#2414: ModuleLoader
    //     reconstructs no AST), so every accessor call must go through
    //     importedIndexerGetterFns_ / -SetterFns_; and
    //   * for a VIRTUAL imported indexer what sits there is #2545's
    //     vtable-DISPATCH wrapper, because emitExportedIndexerGetters /
    //     -Setters skip a virtual indexer ("Skip virtual/override/abstract/
    //     interface — other dispatch paths") and so the exporting module emits
    //     no `bz$<Class>$idxget$<arity>` symbol for it at all.
    // Calling that wrapper from `base[i]` would run the OVERRIDE — precisely
    // the #2832 miscompile, one level below where #2832's fix operates
    // (MEASURED cross-module: 201 where 101 is correct, before and after that
    // fix). Report instead of emitting it: a clean compile error beats a
    // silent wrong value. The ABI addition that would make it work is #2837.
    //
    // The imported NON-virtual case is untouched and correct (measured: 101
    // through `base[i]`, `this[i]`, and `this[i]` inside a lambda), because
    // its entry point IS the direct `bz$…$idxget$…` stub.
    if (!ps || dynamic_cast<const ast::PropertyDecl*>(ps->decl)) return false;
    if (!indexTargetIsBase(ix) || !isVirtualIndexer(ps)) return false;
    ctx_.diagnostics().report(
        Severity::Error, ix.span.begin,
        "cannot lower `base[...]` against the imported virtual indexer on `" +
            std::string(ps->parent ? ps->parent->name : "") +
            "` — §12.3 requires the base class's own implementation, but the "
            "cross-module indexer ABI exports a direct entry point only for a "
            "NON-virtual indexer, so the only imported entry available "
            "dispatches virtually and would run the override "
            "(§12.3/§23.4/§2.8.2) — #2837");
    return true;
}

llvm::Value* Emitter::lowerIndexerGet(const ast::IndexExpr& ix) {
    if (!ix.target || ix.indices.empty() || !ix.indices[0].value)
        return nullptr;
    const sema::TypeSymbol* recvDecl = indexAccessReceiverDecl(ix);
    if (!recvDecl) return nullptr;
    auto* ps = selectIndexer(recvDecl, ix);  // per §23.4 (#1172)
    if (!ps) return nullptr;
    if (rejectImportedVirtualBaseIndexer(ix, ps)) return nullptr;  // #2832
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
    //
    // fix #2489 (§9.2/§23.4, v0.236): widened from `Class`-only to
    // `Class || Record` — §23.4 permits a read-write indexer on a body-form
    // record with mutable properties, and emitExportedIndexerGetters (this
    // fix's own companion in LlvmBackend_Setup.cpp) now exports a stub for
    // one. Confirmed by probe: pre-fix, `r[3]` against an imported record
    // indexer returned 0 (this `recvDecl->kind != Class` check rejected the
    // record receiver and returned nullptr BEFORE the call was ever
    // considered — no crash, no link error, just the caller's zero-init
    // backstop). A record receiver also needs the by-value spill
    // (`obj->getType()->isPointerTy()` alone rejects it, issue #347/§9.2,
    // same as every other by-value-record-receiver site this fix touches)
    // — added below, guarded by `isAggregateType()` (redundant with the
    // `Class || Record` kind check above, since an Enum can never reach
    // here per §23.5's "Enums may not declare indexers", confirmed still
    // enforced by probe — but kept for defense-in-depth, matching every
    // sibling spill site's own belt-and-suspenders posture).
    // fix #2422 (§23.4/§11.2/§2.8.2 clause 1, v0.236): an IMPORTED INTERFACE-
    // typed receiver also has pd == null (the imported interface's indexer has
    // no AST, same as an imported class indexer above). The external-stub
    // dispatch below is specific to a Class/Record receiver; an Interface-typed
    // receiver dispatches through the itable (the #730 branch immediately
    // after), which now also handles pd == null by reading the signature types
    // off the reconstructed side tables (ctx_.importedMemberType /
    // indexerParamType), mirroring #2401's property-through-imported-interface
    // fix. Let it fall through by excluding Interface from this gate.
    if (!pd && recvDecl->kind != ast::TypeDeclKind::Interface) {
        if (recvDecl->kind != ast::TypeDeclKind::Class &&
            recvDecl->kind != ast::TypeDeclKind::Record) return nullptr;
        auto git = importedIndexerGetterFns_.find(ps);
        if (git == importedIndexerGetterFns_.end()) return nullptr;
        llvm::Function* fn = git->second;
        llvm::Value* obj = lowerExpr(*ix.target);
        if (!obj) return nullptr;
        if (!obj->getType()->isPointerTy()) {
            if (!obj->getType()->isAggregateType()) return nullptr;
            llvm::AllocaInst* tmp = entryAlloca(obj->getType());
            builder_.CreateStore(obj, tmp);
            obj = tmp;
        }
        auto* fnTy = fn->getFunctionType();
        std::vector<llvm::Value*> callArgs;
        std::vector<llvm::Value*> idxArgVals;   // fix #2717
        callArgs.push_back(obj);
        for (std::size_t k = 0; k < ps->indexerParams.size() &&
             k < ix.indices.size() &&
             k + 1 < fnTy->getNumParams(); ++k) {
            if (!ix.indices[k].value) return nullptr;
            llvm::Value* argVal = lowerExpr(*ix.indices[k].value);
            if (!argVal) return nullptr;
            callArgs.push_back(
                coerceNumericToType(argVal, fnTy->getParamType(k + 1), false));
            idxArgVals.push_back(argVal);
        }
        llvm::Value* r = builder_.CreateCall(fnTy, fn, callArgs);
        emitPendingCheck();
        releaseIndexArgStringProducers(ix, ps, recvDecl, idxArgVals,
                                       /*accessorResultEscapes=*/true);
        // fix #2814 (§28.3 "Unbound owned temporaries", v0.240): the INDEX half
        // of the same sentence the property half (#2751) covers — "used only by
        // borrowing operations within the expression … as a member or INDEX
        // access". `(new Src())[1]` leaked its receiver in every one of this
        // function's five getter paths (MEASURED: 320 B / 20 allocs, live-object
        // count 20 at exit). `disposeFreshNewReceiverTemp` already gates itself
        // to a provably non-reference result, so a `string`/class/interface
        // element still declines — the safe direction.
        //
        // `calleeReturnsOwned` is false BY CONSTRUCTION, not by choice: §A.3's
        // `IndexerDecl` grammar carries no `owned` marker (only
        // `StaticReadonlyDecl` does), so an indexer read can never announce a
        // fresh transfer and the #1859/#2205 widenings are unreachable here.
        // That is what keeps this clear of #2261's fresh-receiver UAF shape.
        if (!diverged_)
            disposeFreshNewReceiverTemp(ix.target.get(), obj, ctx_.exprType(ix),
                                        /*calleeReturnsOwned=*/false);
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
        // fix #2422 (§23.4/§11.2, v0.236): an imported interface indexer
        // carries its element type in ctx_.importedMemberType rather than an AST.
        // fix #2528 (§23.4/§11.2/§14.1, v0.236): resolve an interface generic
        // parameter against the receiver instantiation BEFORE LLVM lowering;
        // lowerTypeRef on the declared bare T yields a non-null i32 placeholder,
        // so a null-only fallback can never recover `IFlag<bool>`'s i1 ABI.
        const sema::TypeRef* retRef = interfaceIndexerElementType(
            ctx_, ix, recvDecl, ps);
        llvm::Type* retTy = lowerTypeRef(resolveTypeParamRef(retRef));
        if (!retTy) retTy = i32();
        const std::size_t nparams = indexerParamCount(ps);
        std::vector<llvm::Type*> paramTys;
        paramTys.push_back(ptrTy());
        for (std::size_t k = 0; k < nparams; ++k) {
            llvm::Type* pt = nullptr;
            if (pd) {
                if (k < pd->indexerParams.size() && pd->indexerParams[k].type)
                    pt = lowerType(pd->indexerParams[k].type.get());
            } else if (auto* tr = indexerParamType(ctx_, ps, k)) {
                pt = lowerTypeRef(tr);
            }
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
        for (std::size_t k = 0; k < nparams &&
             k < ix.indices.size(); ++k) {
            if (!ix.indices[k].value) return nullptr;
            llvm::Value* argVal = lowerExpr(*ix.indices[k].value);
            if (!argVal) return nullptr;
            callArgs.push_back(coerceNumericToType(argVal, paramTys[k + 1],
                                                   false));
        }
        llvm::Value* r = builder_.CreateCall(gFnTy, fnPtr, callArgs);
        emitPendingCheck();
        // fix #2814 (§28.3 "Unbound owned temporaries", v0.240): the INDEX half
        // of the same sentence the property half (#2751) covers — "used only by
        // borrowing operations within the expression … as a member or INDEX
        // access". `(new Src())[1]` leaked its receiver in every one of this
        // function's five getter paths (MEASURED: 320 B / 20 allocs, live-object
        // count 20 at exit). `disposeFreshNewReceiverTemp` already gates itself
        // to a provably non-reference result, so a `string`/class/interface
        // element still declines — the safe direction.
        //
        // `calleeReturnsOwned` is false BY CONSTRUCTION, not by choice: §A.3's
        // `IndexerDecl` grammar carries no `owned` marker (only
        // `StaticReadonlyDecl` does), so an indexer read can never announce a
        // fresh transfer and the #1859/#2205 widenings are unreachable here.
        // That is what keeps this clear of #2261's fresh-receiver UAF shape.
        if (!diverged_)
            disposeFreshNewReceiverTemp(ix.target.get(), obj, ctx_.exprType(ix),
                                        /*calleeReturnsOwned=*/false);
        return r;
    }
    // #1080 / §23.4: a virtual/override/abstract indexer on a CLASS receiver
    // dispatches dynamically through the vtable slot (the static getter-body
    // resolution below binds the COMPILE-TIME receiver type's getter, so a
    // `this[i]` read would run the base/abstract body). Load the getter fn-ptr
    // from vt[propGetSlot_] and call it as `T get(ptr this, idx...)`. Mirrors
    // the #481 property-vtable path; the slot was assigned by buildVtable and
    // filled with ensureIndexerGetterFn.
    //
    // fix #2832 (§12.3, v0.240): `base[i]` DECLINES this path. §12.3: "Inside a
    // derived member, `base.Name(...)` calls the base class's implementation,
    // bypassing virtual dispatch." Resolving the receiver to the class base
    // (indexAccessReceiverDecl) is necessary but NOT sufficient — the base
    // indexer's PropertySymbol shares its vtable slot with the derived
    // override, so a vtable-dispatched `base[i]` still ran the override
    // (MEASURED: 202 where 102 is correct). Declining here hands the access to
    // the static getter-body path below, which binds the BASE's own accessor —
    // exactly what lowerVirtualPropertyGet does for `base.P` ("bypass dispatch
    // — the static getter path handles it") and emitMethodCall's
    // `baseQualified` does for `base.M()`.
    if (recvDecl->kind == ast::TypeDeclKind::Class && !indexTargetIsBase(ix)) {
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
                std::vector<llvm::Value*> idxArgVals;   // fix #2717
                callArgs.push_back(obj);
                for (std::size_t k = 0; k < pd->indexerParams.size() &&
                     k < ix.indices.size(); ++k) {
                    if (!ix.indices[k].value) return nullptr;
                    llvm::Value* argVal = lowerExpr(*ix.indices[k].value);
                    if (!argVal) return nullptr;
                    callArgs.push_back(
                        coerceNumericToType(argVal, paramTys[k + 1], false));
                    idxArgVals.push_back(argVal);
                }
                llvm::Value* r = builder_.CreateCall(gFnTy, fnPtr, callArgs);
                emitPendingCheck();
                releaseIndexArgStringProducers(ix, ps, recvDecl, idxArgVals,
                                               /*accessorResultEscapes=*/true);
                // fix #2814 (§28.3 "Unbound owned temporaries", v0.240): the INDEX half
                // of the same sentence the property half (#2751) covers — "used only by
                // borrowing operations within the expression … as a member or INDEX
                // access". `(new Src())[1]` leaked its receiver in every one of this
                // function's five getter paths (MEASURED: 320 B / 20 allocs, live-object
                // count 20 at exit). `disposeFreshNewReceiverTemp` already gates itself
                // to a provably non-reference result, so a `string`/class/interface
                // element still declines — the safe direction.
                //
                // `calleeReturnsOwned` is false BY CONSTRUCTION, not by choice: §A.3's
                // `IndexerDecl` grammar carries no `owned` marker (only
                // `StaticReadonlyDecl` does), so an indexer read can never announce a
                // fresh transfer and the #1859/#2205 widenings are unreachable here.
                // That is what keeps this clear of #2261's fresh-receiver UAF shape.
                if (!diverged_)
                    disposeFreshNewReceiverTemp(ix.target.get(), obj, ctx_.exprType(ix),
                                                /*calleeReturnsOwned=*/false);
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
        std::vector<llvm::Value*> idxArgVals;   // fix #2717: parallel to indices
        callArgs.push_back(obj);
        for (std::size_t k = 0; k < ps->indexerParams.size() &&
             k < ix.indices.size(); ++k) {
            if (!ix.indices[k].value) return nullptr;
            llvm::Value* argVal = lowerExpr(*ix.indices[k].value);
            if (!argVal) return nullptr;
            callArgs.push_back(argVal);
            idxArgVals.push_back(argVal);
        }
        llvm::Function* fn = ensureIndexerBlockGetter(pd, getterBlock, ps);
        llvm::Value* res =
            builder_.CreateCall(fn->getFunctionType(), fn, callArgs);
        // §31.1/§31.2: a throw inside the standalone getter leaves the pending
        // exception set and returns; route it to the enclosing unwind target,
        // exactly as an ordinary method call does.
        emitPendingCheck();
        // fix #2717: after emitPendingCheck, so the release sits on the normal
        // path only — on a throw edge the temporary leaks, which is #429's own
        // (`!diverged_`) discipline and the safe direction.
        releaseIndexArgStringProducers(ix, ps, recvDecl, idxArgVals,
                                       /*accessorResultEscapes=*/true);
        // fix #2814 (§28.3 "Unbound owned temporaries", v0.240): the INDEX half
        // of the same sentence the property half (#2751) covers — "used only by
        // borrowing operations within the expression … as a member or INDEX
        // access". `(new Src())[1]` leaked its receiver in every one of this
        // function's five getter paths (MEASURED: 320 B / 20 allocs, live-object
        // count 20 at exit). `disposeFreshNewReceiverTemp` already gates itself
        // to a provably non-reference result, so a `string`/class/interface
        // element still declines — the safe direction.
        //
        // `calleeReturnsOwned` is false BY CONSTRUCTION, not by choice: §A.3's
        // `IndexerDecl` grammar carries no `owned` marker (only
        // `StaticReadonlyDecl` does), so an indexer read can never announce a
        // fresh transfer and the #1859/#2205 widenings are unreachable here.
        // That is what keeps this clear of #2261's fresh-receiver UAF shape.
        if (!diverged_)
            disposeFreshNewReceiverTemp(ix.target.get(), obj, ctx_.exprType(ix),
                                        /*calleeReturnsOwned=*/false);
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
    // fix #2761 (§8.6 "Boxing" / §23.4 "Indexers", v0.240): the INDEXER
    // spelling of the expression-getter box — `property [i: int]: Object =>
    // _n + i;` read as `h[0]` handed the caller a raw i32 in an `Object`, so
    // `h[0] is int` was silently FALSE. Same reasoning as the property inline
    // in LlvmBackend_Exprs.cpp; done while the receiver's generic substitution
    // (setupReceiverGenericSubst above) is still installed so a `T`-typed body
    // boxes as its concrete type (#1279), and BEFORE the #2717 index-producer
    // release, which concerns the index arguments and never the result.
    result = boxForTarget(result, ctx_.exprType(ix), getterExpr);
    // fix #2850 (§7.2 "Nullable value types" / §23.4 "Indexers", v0.240): the
    // INDEXER spelling of #1168's expression-getter nullable-VALUE coercion —
    // the twin of `wrapNullableGetterResult(ctx_.exprType(*ma), v)` on the
    // named-property inline path (LlvmBackend_Exprs.cpp). §7.2: "A value type
    // wrapped in `?` is an optional: it either holds a value or `null`", and
    // §23.5's restriction list names only move-only value types, so
    // `property [i: int]: int?` is an admitted declaration whose getter body
    // may legally yield `null` or a bare `int`. Without this the inlined body
    // handed the caller the RAW body value — a `ptr null` for `get => null;`,
    // a bare i32 for `get => i + 5;` — where the caller expects the tagged
    // `{i8 hasValue, i32}` optional, and the mismatch reached LLVM as an
    // assertion abort (a PHI-operand type assert for the `??` merge, a
    // checkGEPType assert for the bare-value shape) rather than any
    // diagnostic. Done AFTER boxForTarget (the two are disjoint: an `Object`
    // target is a reference type, so isNullableValueTy is false there) and
    // while the receiver's generic substitution is still installed.
    result = wrapNullableGetterResult(ctx_.exprType(ix), result);
    // fix #2717: release a `+1` string producer index temporary now the getter
    // body has read it. The helper's element-type gate is what makes this safe
    // on the GETTER side: an inlined `get => k;` lowers to a bare load of the
    // index (no minting return), so a `string`-typed indexer is excluded and
    // `result` can never alias what this releases.
    releaseIndexArgStringProducers(ix, ps, recvDecl, argVals,
                                   /*accessorResultEscapes=*/true);
    currentThis_ = savedThis;
    locals_ = std::move(savedLocals);
    paramSlots_ = std::move(savedParams);
    genericSubst_ = std::move(savedSubst);
    // fix #2814 (§28.3 "Unbound owned temporaries", v0.240): the INDEX half
    // of the same sentence the property half (#2751) covers — "used only by
    // borrowing operations within the expression … as a member or INDEX
    // access". `(new Src())[1]` leaked its receiver in every one of this
    // function's five getter paths (MEASURED: 320 B / 20 allocs, live-object
    // count 20 at exit). `disposeFreshNewReceiverTemp` already gates itself
    // to a provably non-reference result, so a `string`/class/interface
    // element still declines — the safe direction.
    //
    // `calleeReturnsOwned` is false BY CONSTRUCTION, not by choice: §A.3's
    // `IndexerDecl` grammar carries no `owned` marker (only
    // `StaticReadonlyDecl` does), so an indexer read can never announce a
    // fresh transfer and the #1859/#2205 widenings are unreachable here.
    // That is what keeps this clear of #2261's fresh-receiver UAF shape.
    if (!diverged_)
        disposeFreshNewReceiverTemp(ix.target.get(), obj, ctx_.exprType(ix),
                                    /*calleeReturnsOwned=*/false);
    return result;
}


// fix #2717 / §23.4 / §23.5 / §28.1 / §28.9 (v0.240): release a `+1` string
// PRODUCER passed directly as an INDEX ARGUMENT. See the declaration comment
// (LlvmBackend_impl.hpp) for the shape; this is the soundness argument.
//
// WHY A RELEASE IS OWED. §23.5 "Restrictions": "`ref` and `out` parameters are
// not allowed in indexer parameter lists"; §28.1: on a `string`, `owned` "is a
// compile error". An index argument is therefore ALWAYS an unmodified by-value
// borrow — the accessor never consumes it — and a producer temporary
// (`t[$"v-{i}"] := i`) is bound to no local, so no scope-end release covers it
// either. That is exactly #429's ARGUMENT-PASS category for an ordinary call
// (LlvmBackend_Calls2.cpp ~7834, which releases the identical shape after the
// call); the indexer call site simply never grew the same step.
//
// WHY IT CANNOT OVER-RELEASE. The gate is `isPlusOneStringProducer`, the same
// predicate #429 uses, and it is precisely the "a temporary this expression
// minted" test: it matches interpolation, string `+`, a non-nullable `??`, a
// string range slice, a primitive `.ToString()`, and an arm-meet `case`/`if`
// (whose lowering normalizes every arm to +1). It does NOT match an
// IdentifierExpr, so a CALLER-OWNED LOCAL passed as an index (`var s := …;
// t[s] := i;`) is never released here — that shape is the escape-walk half of
// #2717 (StringEscapeWalker's IndexExpr arm), where the local keeps its own +1
// and discharges it at scope end. The two halves are disjoint by construction:
// one releases only values no local owns, the other only arms the owner's own
// scope-end release. An over-correction in either direction shows up as a
// double-free, not silence.
//
// THE TWO EXCLUSIONS, both keeping the pre-existing leak (the safe direction):
//   * a RECORD receiver — §9/§9.7: a record's reference fields are borrows with
//     no independent +1 and no disposer, so an accessor that stores the index
//     into a field ALIASES this reference and releasing it would dangle the
//     field. Same discriminant as `emitProducerRelease`'s `recvDecl->kind ==
//     Class` gate below (#2548) and #2535/#2540's property/indexer-SET walker
//     gate. An INTERFACE receiver is excluded by the same test, because the
//     runtime implementor may be record-backed and — unlike #2540's *value*
//     relaxation — there is no paired call-site retain for the INDEX on the
//     record-backed itable branch.
//   * a `string`-typed indexer ELEMENT, and ONLY where the accessor's RESULT
//     can reach the caller (`accessorResultEscapes`, i.e. the four
//     lowerIndexerGet branches). An accessor may yield the index itself
//     (`get => k;`), and an INLINED expression-bodied getter lowers that to a
//     bare load, with none of the minting a compiled `return k;` would do
//     (#429/#694 return logic) — releasing then hands the caller a freed
//     pointer. On the pure SET path no getter runs at all, and on the
//     compound / `++` paths the getter's result is consumed by emitCompoundOp
//     BEFORE this release and never escapes the statement, so neither needs the
//     exclusion: a `[k: string]: string` setter's index is released normally.
//     (Because the gate now applies only in value position, `ctx_.exprType(ix)`
//     is always populated where it is consulted.)
//
// ⚠ THE §22.3 SUPPRESSION. `emitAccessorRefArgCopyIn` (this file) lowers a
// `ref`/`out` argument whose LOCATION is an indexer slot — `C.Bump(ref t[k])`,
// which §22.3 admits — by evaluating each index ONCE and PINNING it in
// `exprValueOverride_`, then driving lowerIndexerGet (the copy-in) and, after
// the call, lowerIndexerSet (the write-back). Both of those now call this
// helper, and the pin is still live across both, so without the check below the
// SAME producer would be released TWICE (emitStringRelease is null/sentinel-safe
// but NOT idempotent) and the write-back setter would read the freed index:
// a double free plus a use-after-free. Confirmed mechanically — the shape leaks
// 731 B / 40 allocations on 2a990de and SEGVs inside ASan's Deallocate with the
// releases unsuppressed. §22.3 owns that value's lifetime for the whole
// copy-in/copy-out bracket, so this helper must not touch it; the pre-existing
// leak stays on that path (the safe direction), tracked in #2746.
//
// NOTE the §23.5 rule ("`ref` and `out` parameters are not allowed in indexer
// parameter lists") does NOT cover this: it governs the indexer's OWN parameter
// list, a different construct from an argument whose LOCATION is an indexer
// slot. Conflating the two is what hid this case initially.
//
// RESIDUAL, recorded not hidden: a CLASS accessor that stores the index into a
// RECORD-typed field (a plain store per §9/§9.7) would still alias it. That is
// the identical residual #429's ARGUMENT-PASS release and #1430's escape-walk
// relaxation already carry for an ordinary method argument; it is not widened
// here, and closing it needs the callee-side record-field analysis neither has.
void Emitter::releaseIndexArgStringProducers(
        const ast::IndexExpr& ix, const sema::PropertySymbol* ps,
        const sema::TypeSymbol* recvDecl,
        const std::vector<llvm::Value*>& argVals,
        bool accessorResultEscapes) {
    if (!ps || !recvDecl) return;
    if (recvDecl->kind != ast::TypeDeclKind::Class) return;
    // Mirrors #429's own `!diverged_` guard (LlvmBackend_Calls2.cpp ~7834): on a
    // path the accessor body diverged from (`get => throw …`) the temporary
    // leaks rather than having a release emitted into unreachable — or
    // already-terminated — IR.
    if (diverged_) return;
    // The indexer's element type is the IndexExpr's own type. Consulted ONLY
    // where the accessor's result can reach the caller — see the header note.
    //
    // FAIL-CLOSED (round 3). This test used to be "is the element `string`?",
    // which was fail-OPEN: every element type that is pointer-shaped but not
    // spelled `string` fell THROUGH into the release while an inlined
    // `get => k;` handed the caller the very pointer being freed. Confirmed
    // reachable — `[k: string]: Object` with `get => k;` (`Object` accepts any
    // source, ExprTypecheck.cpp ~2964, and no conversion is emitted between the
    // body and this release) was an ASan `heap-use-after-free` in `main`, where
    // 2a990de merely leaked. Three more shapes had the same hole:
    // `IEquatable<string>` / `IComparable<string>` (accepted via
    // primitiveBoxesIntoBuiltinIface, which excludes only `Bool` — `Kind::Named`,
    // so the old test missed it); a bare generic `T` reached through a
    // `this` receiver (no substitution is installed for that receiver, so the
    // type stays an unresolved `Kind::Named` with a null declaration); and a
    // NULL `exprType` (an imported indexer with no side-table entry), which the
    // old `if (const TypeRef* …)` form let fall straight into the release.
    //
    // So: resolve a type parameter first, then release ONLY for an element that
    // provably cannot BE the index — a non-nullable scalar primitive, or unit.
    // `nullptr`, `string`, `Object`, ANY `Kind::Named` (class / interface /
    // record / enum / unresolved type parameter), `Function`, `InlineArray` and
    // `Error` all keep the pre-existing leak, which is the safe direction. This
    // matches the discipline the rest of this helper already follows (a
    // non-Class receiver, a pinned index, and a diverged path all decline).
    if (accessorResultEscapes &&
        !indexerElementCannotAliasIndex(resolveTypeParamRef(ctx_.exprType(ix))))
        return;
    for (std::size_t k = 0; k < ps->indexerParams.size() &&
         k < ix.indices.size() && k < argVals.size(); ++k) {
        const ast::Expr* ie = ix.indices[k].value.get();
        if (!ie || !argVals[k] || !argVals[k]->getType()->isPointerTy())
            continue;
        // §22.3 (see the header note): this index is PINNED by an accessor
        // `ref`/`out` copy-in/copy-out bracket, which owns its lifetime across
        // BOTH the copy-in getter and the write-back setter. Releasing here
        // would double-free it and dangle the write-back's read.
        if (exprValueOverride_.count(ie)) continue;
        if (!isPlusOneStringProducer(ie)) continue;
        emitStringRelease(argVals[k]);   // null/sentinel-safe, NOT idempotent
    }
}

bool Emitter::lowerIndexerSet(const ast::IndexExpr& ix, llvm::Value* val,
                              const ast::Expr* valueExpr) {
    if (!ix.target || ix.indices.empty() || !ix.indices[0].value)
        return false;
    const sema::TypeSymbol* recvDecl = indexAccessReceiverDecl(ix);
    if (!recvDecl) return false;
    auto* ps = selectIndexer(recvDecl, ix);  // per §23.4 (#1172)
    if (!ps) return false;
    if (rejectImportedVirtualBaseIndexer(ix, ps)) return false;  // #2832
    auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl);
    // fix #2414: the SETTER twin of lowerIndexerGet's decl-null fallback
    // immediately above — see its comment for the full rationale. Dispatches
    // through the externally-declared `bz$<Class>$idxset$<arity>` function
    // declareImportedIndexerSetters installs (absent for a getter-only
    // imported indexer, in which case this correctly falls through and
    // returns false — the same "no setter to lower" outcome a local
    // getter-only indexer's `!setterExpr && !setterStmts` check produces
    // further down).
    //
    // fix #2489 (§9.2/§23.4, v0.236): the SETTER twin of the widening in
    // lowerIndexerGet immediately above — same `Class`-only kind gate, same
    // missing by-value-record spill, same fix, same §23.4 authority. See
    // that function's comment for the full rationale (probe confirmed:
    // `r[5] := 9; return r[2];` against an imported record's read-write
    // indexer returned 0 pre-fix, both the SET and the independent GET
    // rejected by this same shape of gate).
    //
    // A SETTER additionally needs the receiver's OWN storage address, not a
    // copy — mirroring lowerImportedPropertySetValue's identical fix (this
    // file): `lowerExpr(*ix.target)` on a named record-typed local LOADS a
    // byte-for-byte copy, so spilling that copy into a fresh alloca (as a
    // GET-only path may safely do) mutates a throwaway slot, never the
    // caller's own variable — confirmed by probe (`r[5] := 9; return
    // r[2];` returned 2, not 6, i.e. the write silently failed to persist
    // even once the gate/spill above were in place). The pre-existing,
    // same-shaped LOCAL (`pd != nullptr`) inline-body branch a few dozen
    // lines below this one has this exact defect too — filed separately as
    // #2502, out of #2489's cross-module-ABI scope; this `!pd` branch is
    // new code written for #2489 and does not inherit it. Prefer
    // `lvalueAddr` (resolves a local/field to its own storage via
    // `localSlot`/`fieldAddr`) over `lowerExpr`; fall back to `lowerExpr` +
    // spill only for a non-addressable (temporary) receiver, where no
    // "original" exists to preserve.
    // fix #2422 (§23.4/§11.2/§2.8.2 clause 1, v0.236): the SETTER twin of
    // lowerIndexerGet's gate immediately above — an IMPORTED INTERFACE-typed
    // receiver also has pd == null, and must fall through to the itable-dispatch
    // branch below (which now handles pd == null) rather than be captured by
    // this Class/Record-only external-stub gate. See that gate's comment.
    if (!pd && recvDecl->kind != ast::TypeDeclKind::Interface) {
        if (recvDecl->kind != ast::TypeDeclKind::Class &&
            recvDecl->kind != ast::TypeDeclKind::Record) return false;
        auto sit = importedIndexerSetterFns_.find(ps);
        if (sit == importedIndexerSetterFns_.end()) return false;
        llvm::Function* fn = sit->second;
        llvm::Value* obj = nullptr;
        llvm::Type* objElemTy = nullptr;
        if (llvm::Value* addr = lvalueAddr(*ix.target, objElemTy)) {
            if (objElemTy && objElemTy->isPointerTy())
                obj = builder_.CreateLoad(objElemTy, addr);
            else if (objElemTy && objElemTy->isAggregateType())
                obj = addr;  // already the address of the receiver's real storage
        }
        if (!obj) {
            llvm::Value* ov = lowerExpr(*ix.target);
            if (!ov) return false;
            if (ov->getType()->isPointerTy()) {
                obj = ov;
            } else if (ov->getType()->isAggregateType()) {
                llvm::AllocaInst* tmp = entryAlloca(ov->getType());
                builder_.CreateStore(ov, tmp);
                obj = tmp;
            } else {
                return false;
            }
        }
        auto* fnTy = fn->getFunctionType();
        std::vector<llvm::Value*> callArgs;
        std::vector<llvm::Value*> idxArgVals;   // fix #2717
        callArgs.push_back(obj);
        for (std::size_t k = 0; k < ps->indexerParams.size() &&
             k < ix.indices.size() &&
             k + 1 < fnTy->getNumParams(); ++k) {
            if (!ix.indices[k].value) return false;
            llvm::Value* argVal = lowerExpr(*ix.indices[k].value);
            if (!argVal) return false;
            callArgs.push_back(
                coerceNumericToType(argVal, fnTy->getParamType(k + 1), false));
            idxArgVals.push_back(argVal);
        }
        if (fnTy->getNumParams() == 0) return false;
        llvm::Type* valTy = fnTy->getParamType(fnTy->getNumParams() - 1);
        callArgs.push_back(coerceNumericToType(val, valTy, false));
        builder_.CreateCall(fnTy, fn, callArgs);
        emitPendingCheck();
        releaseIndexArgStringProducers(ix, ps, recvDecl, idxArgVals,
                                       /*accessorResultEscapes=*/false);
        return true;
    }
    // fix #2548 (§23.4/§28.9/§4.6, v0.236): the INDEXER twin of
    // lowerVirtualPropertySet's #2490/#2500/#2506 emitProducerRelease
    // (LlvmBackend_Exprs.cpp ~5106-5128). The three setter-call paths below
    // (interface itable dispatch, class-vtable dispatch, inline setter body)
    // each invoke a class-owning setter (or class-side field store for the
    // inline path) that RETAINS its incoming `value` — but for a +1
    // PRODUCER RHS (`"abc" + i.ToString()`, a string interpolation, a method
    // call returning a +1 closure, or a box-return call to an
    // Object/interface property target), the caller transferred the fresh
    // reference across the call and the callee's retain doubled it; the
    // doubled reference would never be released. The pre-fix consequence:
    // every loop iteration leaks the producer's +1 — for `"abc" + i.ToString()`
    // that is ~37 bytes per iteration (32 byte bz_string header + ~5 byte
    // payload for the digit) — exactly the leak the issue observed (185
    // bytes per 10 iterations, the 50-byte "indirect" tail is the payload
    // string's own referenced byte-string header). Mirrors the property
    // setter's #2490 fix (the imported-stub sibling at lowerImportedPropertySetValue
    // ~4038) — see its comment for the full rationale on the class-retain /
    // record-plain-store split. As on the property side, the release is
    // unconditional ONLY on the class-backed paths; on the record-backed
    // itable path the setter plain-stores (a record's reference fields are
    // borrows with no independent +1 and no disposer — §9/§9.7) and
    // releasing here would drop the field's only reference (UAF). The
    // lambda below is the single emitProducerRelease used by all three
    // paths; each call site decides when to invoke it.
    auto emitProducerRelease = [&] {
        if (valueExpr && isPlusOneStringProducer(valueExpr))
            emitStringRelease(val);
        else if (valueExpr && (isPlusOneClosureProducer(valueExpr) ||
                               isUserMethodClosureReturnCall(valueExpr)))
            emitClosureRelease(val);
        else if (valueExpr && val && val->getType()->isPointerTy()) {
            // fix #2500 (§23.3/§28.9, v0.236): the box twin of the string/
            // closure release above — same isProducer predicate as
            // lowerBoxFieldStore / lowerImportedPropertySetValue. Gated on the
            // TARGET being Object/interface (and val a ptr): a value-typed
            // indexer's source may be boxable in isolation but is not boxed
            // here. The target is `ix` (the IndexExpr) for the indexer
            // shape; isObjectOrInterfaceTarget must accept IndexExpr (it's
            // the assignment-target shape used by lowerPropertySetValue too —
            // see #2500's call site in lowerVirtualPropertySet).
            const sema::TypeRef* tgtTy = ctx_.exprType(ix);
            if (tgtTy && isObjectOrInterfaceTarget(tgtTy)) {
                const sema::TypeRef* srcTy = ctx_.exprType(*valueExpr);
                if ((srcTy && (isBoxableValueType(srcTy) ||
                               isNullableValueTy(srcTy))) ||
                    isUserMethodBoxReturnCall(valueExpr))
                    emitBoxRelease(val);
            }
        }
    };
    // #730 / §13 / §23.4: `(arr: IFoo)[i] := v` through an INTERFACE-typed
    // receiver dispatches the setter dynamically through the itable — load the
    // implementing class's setter from itab[set-index] and call it as
    // `void set(ptr this, idx..., value)`.
    if (recvDecl->kind == ast::TypeDeclKind::Interface) {
        auto ifIt = ifacePropertySetIndex_.find(ps);
        auto descIt = descArray_.find(recvDecl);
        if (ifIt == ifacePropertySetIndex_.end() || descIt == descArray_.end())
            return false;
        // fix #2422 (§23.4/§11.2, v0.236): imported-interface signature
        // reconstruction; fix #2528: the setter's implicit value has the same
        // receiver-instantiated generic type as the getter return (per §14.1).
        const sema::TypeRef* valRef = interfaceIndexerElementType(
            ctx_, ix, recvDecl, ps);
        llvm::Type* valTy = lowerTypeRef(resolveTypeParamRef(valRef));
        if (!valTy) valTy = i32();
        const std::size_t nparams = indexerParamCount(ps);
        std::vector<llvm::Type*> paramTys;
        paramTys.push_back(ptrTy());
        for (std::size_t k = 0; k < nparams; ++k) {
            llvm::Type* pt = nullptr;
            if (pd) {
                if (k < pd->indexerParams.size() && pd->indexerParams[k].type)
                    pt = lowerType(pd->indexerParams[k].type.get());
            } else if (auto* tr = indexerParamType(ctx_, ps, k)) {
                pt = lowerTypeRef(tr);
            }
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
        for (std::size_t k = 0; k < nparams &&
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
        // fix #2540 (§23.4/§28.9/§11, v0.236): the INDEXER twin of
        // lowerVirtualPropertySet's #2535 paired call-site retain
        // (LlvmBackend_Exprs.cpp ~5178 — `lowerVirtualPropertySet`'s
        // `classBB`/`recordBB`/`doneBB` block). The walker above
        // (LlvmBackend_Setup.cpp, the new `classOrIfaceIdxSet` discriminant
        // in AssignmentStmt's `if (classOrIfaceIdxSet) { ... return; }`)
        // now treats `iiface[i] := s;` as a CALL-BOUNDED BORROW when `s`
        // is a tracked string NAMED LOCAL — arming the local's scope-end
        // release. That release needs a paired +1 on the heap it can drop
        // without danging any other reference — the setter provides it on
        // the CLASS-backed path (setter retains → field has +1, local
        // keeps its +1, scope-end release drops the local's, sound); on
        // the RECORD-backed path the setter keeps the PLAIN STORE (§9/
        // §9.7 — a boxed record's reference fields are borrows with no
        // disposer), so the field ALIASES the local's only reference and
        // the scope-end release would free the field's only ref → UAF.
        // Mint that extra +1 here, on the record path ONLY, so the scope-
        // end release has a balanced +1 to drop. Restricted to the NAMED
        // LOCAL case (an IdentifierExpr whose LocalSymbol's decl is in
        // stringReleaseEligible_ / stringBorrowRetainEligible_ — the SAME
        // sets the walker uses) so a producer-temporary RHS (whose only
        // release is the class-path emitProducerRelease on the property
        // side) doesn't get a phantom retain — that would leave a
        // stranded +1 on the heap. Mirrors #2535's gating: a runtime
        // owner-kind flag read from the itable's trailing word
        // (`ifaceItabSlotCount_[ps->parent]`, NULL = record-backed plain-
        // store, non-NULL = class-backed retaining setter) selects class-
        // backed (no extra retain) vs record-backed (mint the named-local
        // retain on the value before the scope-end release drops it).
        //
        // The itable flag travels inside the bytes the concrete type's
        // OWN module produced, so cross-module soundness carries over
        // (the importer reads it through `itab` at the same index; the
        // itable linkage is per-module InternalLinkage and not comparable
        // cross-module, but the flag itself is data the concrete-type
        // module baked — same shape #2506 / #2535 use for the property
        // case).
        //
        // On the class-vtable path (the `if (recvDecl->kind == Class)`
        // block below) and the local-inline path (the inlined setter
        // body dispatched via lowerScopedBlock further below) no such
        // retain is needed: the class's own setter
        // (ensureIndexerAccessorFn's class-gated retain) is unconditional
        // for the vtable-slot case (a vtable slot is always a class's
        // own slot, never a record's — §9: "A record cannot derive from
        // another record or from a class"), and the inline path is
        // gated on `ps->parent->kind == Class` by the property-SET gate
        // — the class-retain invariant holds soundly and the scope-end
        // release is balanced without help. Only the itable dispatch can
        // land on a record-backed runtime receiver, so the paired retain
        // lives ONLY on this itable path.
        auto isTrackedStringLocalForRelease = [&](const ast::Expr* e) -> bool {
            if (!e || currentFn_ != stringReleaseEligibleFn_) return false;
            auto* id = dynamic_cast<const ast::IdentifierExpr*>(e);
            if (!id) return false;
            auto* loc = dynamic_cast<sema::LocalSymbol*>(ctx_.exprSymbol(*id));
            if (!loc || !loc->decl) return false;
            const void* declNode = static_cast<const void*>(loc->decl);
            return stringReleaseEligible_.count(declNode) ||
                   stringBorrowRetainEligible_.count(declNode);
        };
        bool ifaceIdxNamedLocalEligible =
            valueExpr && val && val->getType()->isPointerTy() &&
            isTrackedStringLocalForRelease(valueExpr);
        auto cntIt = ifaceItabSlotCount_.find(ps->parent);
        if (cntIt != ifaceItabSlotCount_.end()) {
            // The owner-kind flag lives at itab[ifaceItabSlotCount_[iface]]
            // (the LAST word in the itable, appended by #2506). Read it
            // HERE from the `itab` we already have in scope (the same
            // value computed for the fn-ptr load above; do not reload it
            // post-call — the receiver has been mutated by the setter and
            // reload could race).
            llvm::Value* flagAddr = builder_.CreateGEP(
                ptrTy(), itab,
                {builder_.getInt32(cntIt->second)}, "idxset.flagaddr");
            llvm::Value* flag = builder_.CreateLoad(
                ptrTy(), flagAddr, "idxset.flagval");
            llvm::Value* isClassBacked = builder_.CreateICmpNE(
                flag, llvm::ConstantPointerNull::get(ptrTy()),
                "idxset.isclass");
            llvm::BasicBlock* curBB = builder_.GetInsertBlock();
            llvm::Function* fn = curBB->getParent();
            llvm::BasicBlock* classBB =
                llvm::BasicBlock::Create(llvm_, "idxset.class", fn);
            llvm::BasicBlock* recordBB =
                llvm::BasicBlock::Create(llvm_, "idxset.record", fn);
            llvm::BasicBlock* doneBB =
                llvm::BasicBlock::Create(llvm_, "idxset.done", fn);
            builder_.CreateCondBr(isClassBacked, classBB, recordBB);
            // CLASS-backed branch: setter retained → release the producer-RHS
            // +1 the setter's retain doubled (#2548, the indexer twin of
            // lowerVirtualPropertySet's emitProducerRelease on its classBB
            // — see emitProducerRelease's definition above). The named-local
            // close (the walker-armed scope-end release on the local) is
            // independent of this producer release — a producer RHS has no
            // local binding, only the value itself, and the doubled +1 has
            // no other owner; the classBB emitProducerRelease drops it
            // here. No extra retain needed on classBB: the class setter's
            // retain IS the field's +1, the local keeps its own +1, the
            // walker's scope-end release drops the local's +1, all +1's
            // accounted for.
            builder_.SetInsertPoint(classBB);
            emitProducerRelease();
            builder_.CreateBr(doneBB);
            // RECORD-backed branch: setter plain-stored → field aliases
            // the local's only reference. For a tracked-string NAMED LOCAL
            // RHS, mint the extra +1 here so the walker's scope-end
            // release on the local has a balanced +1 to drop.
            builder_.SetInsertPoint(recordBB);
            if (ifaceIdxNamedLocalEligible)
                emitStringRetain(val);
            builder_.CreateBr(doneBB);
            builder_.SetInsertPoint(doneBB);
        }
        return true;
    }
    // #1080 / §23.4: a virtual/override/abstract indexer SETTER on a CLASS
    // receiver dispatches dynamically through the vtable slot (symmetric with
    // the getter path above). Load the setter fn-ptr from vt[propSetSlot_] and
    // call it as `void set(ptr this, idx..., value)`.
    //
    // fix #2832 (§12.3, v0.240): `base[i] := v` declines it, exactly as the
    // getter twin above and as lowerVirtualPropertySet's own `base` bail-out
    // (`if (tid->name == "base") return false;  // §12.3`) — the write must
    // reach the BASE's setter body, not the override's vtable slot.
    if (recvDecl->kind == ast::TypeDeclKind::Class && !indexTargetIsBase(ix)) {
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
                std::vector<llvm::Value*> idxArgVals;   // fix #2717
                callArgs.push_back(obj);
                for (std::size_t k = 0; k < pd->indexerParams.size() &&
                     k < ix.indices.size(); ++k) {
                    if (!ix.indices[k].value) return false;
                    llvm::Value* argVal = lowerExpr(*ix.indices[k].value);
                    if (!argVal) return false;
                    callArgs.push_back(
                        coerceNumericToType(argVal, paramTys[k + 1], false));
                    idxArgVals.push_back(argVal);
                }
                callArgs.push_back(coerceNumericToType(val, valTy, false));
                builder_.CreateCall(sFnTy, fnPtr, callArgs);
                emitPendingCheck();
                releaseIndexArgStringProducers(ix, ps, recvDecl, idxArgVals,
                                               /*accessorResultEscapes=*/false);
                // fix #2548 (§23.4/§28.9/§4.6, v0.236): the CLASS-VTABLE
                // twin of the interface-dispatch classBB emitProducerRelease
                // above. The class-vtable path is unconditional-class-backed
                // (a vtable slot is always a class's own slot, never a
                // record's — §9: "A record cannot derive from another record
                // or from a class"), so the setter's class-gated retain
                // (ensureIndexerAccessorFn / ensureIndexerSetterFn's class
                // path) DID fire and the producer-RHS +1 was doubled. Drop
                // the doubled +1 here — see emitProducerRelease's definition
                // above for the predicate. Mirrors lowerVirtualPropertySet's
                // `!isIfaceProp` unconditional emitProducerRelease exactly
                // (the property-side analogue handles the same vtable path).
                emitProducerRelease();
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
    // fix #2502 (§9.2/§23.4, v0.236): resolve the receiver's OWN storage
    // address via `lvalueAddr` rather than loading a COPY via `lowerExpr` —
    // mirrors lowerImportedPropertySetValue's #2489 fix and this same
    // function's own `!pd` (imported) branch above. `lowerExpr(*ix.target)`
    // on a named record-typed local LOADS a byte-for-byte copy; the OLD code
    // then spilled that copy into a FRESH alloca (see the removed "§9.2 /
    // #794" spill a few lines below, which is now folded into this
    // resolution), so `this` inside the setter body pointed at a THROWAWAY
    // slot and the write never reached the caller's own variable. Confirmed
    // by probe (#2502's own repro): `r[5] := 9; return r[2];` returned 2 (the
    // untouched initial value), not 6 — no crash, no diagnostic, a silently
    // dropped write. Falls back to `lowerExpr` + spill only when the receiver
    // expression is NOT an addressable place (e.g. a fresh temporary —
    // `MakeRecord()[5] := 9`, where no "original" exists to preserve).
    llvm::Value* obj = nullptr;
    llvm::Type* objElemTy = nullptr;
    if (llvm::Value* addr = lvalueAddr(*ix.target, objElemTy)) {
        if (objElemTy && objElemTy->isPointerTy())
            obj = builder_.CreateLoad(objElemTy, addr);
        else if (objElemTy && objElemTy->isAggregateType())
            obj = addr;  // already the address of the receiver's real storage
    }
    if (!obj) {
        llvm::Value* ov = lowerExpr(*ix.target);
        if (!ov) return false;
        if (ov->getType()->isPointerTy()) {
            obj = ov;
        } else if (ov->getType()->isAggregateType()) {
            llvm::AllocaInst* tmp = entryAlloca(ov->getType());
            builder_.CreateStore(ov, tmp);
            obj = tmp;
        } else {
            return false;
        }
    }
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
    // fix #2502: `obj` is already pointer-typed at this point — resolved
    // above via `lvalueAddr` (the receiver's own storage) or, for a
    // non-addressable temporary, via `lowerExpr` + spill. The old bare
    // `!isPointerTy()` spill that used to live here is folded into that
    // resolution now (spilling here, post-resolution, would have re-copied
    // an already-correct address and defeated the fix).
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
    // fix #2712 / §28.1 / §28.9 / §23.4 (v0.240): the INLINED-SETTER twin of
    // #2696's `inlineCtorBody` retain, and the site #2712 was filed against.
    // This body is lowered into the ENCLOSING function — nothing here touches
    // `currentFn_` — so when that enclosing body is the analyzed one the
    // drop-on-overwrite `bz_string_release` that `k := k + "-x"` (or `value :=
    // …`) emits DOES fire, on a slot this frame gave no +1: it frees the
    // CALLER's string. Reproduced as `heap-use-after-free`, frames
    // `bz_string_release <- main`. Emitted after BOTH binds and before the body,
    // so it dominates every assignment. The helper's own co-gate
    // (`currentFn_ != stringReleaseEligibleFn_ -> return`) makes it a no-op when
    // this setter is itself inlined into a body lowered outside
    // emitFunctionBody, exactly as the release is a no-op there — no call-site
    // audit needed. Only the BLOCK-bodied setter can contain an assignment
    // statement (the parser stores even the bare `set => k := e;` form as
    // `stmts`, Parser_Decls.cpp); an `exprBody` setter needs nothing.
    if (setterStmts)
        retainAssignedStringParams(*setterStmts, ps->indexerParams, pd);
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
    // fix #2548 (§23.4/§28.9/§4.6, v0.236): the INLINE-SETTER twin of the
    // interface-dispatch classBB / class-vtable emitProducerRelease calls
    // above. The inline path is reached for two receiver shapes:
    //   * a CLASS receiver with a NON-VIRTUAL indexer (no propSetSlot_ entry
    //     in the vtable — #1080's own classification) — the inlined setter
    //     body stores `value` into the class's backing field, and the
    //     class-field store mints its own +1 (the same retain
    //     ensureIndexerSetterFn / ensurePropertyAccessorFn's class-gated
    //     retain does on the vtable path). A +1 producer RHS therefore
    //     leaves a doubled reference here too; release it.
    //   * a RECORD receiver — §9/§9.7: a record's reference fields are
    //     borrows with no independent +1 and no disposer, so the inline
    //     setter body does a PLAIN STORE. Releasing the producer here
    //     would drop the field's only reference (UAF). Skip on records.
    // Gated on `recvDecl->kind == Class` — same shape as
    // lowerImportedPropertySetValue's `classOwned` gate at this file ~4038
    // and lowerVirtualPropertySet's classBB (the latter via the runtime
    // itable flag for the interface dispatch). The inline path never
    // encounters an interface receiver (the interface branch above
    // returned true first), so no itable flag read is needed here.
    if (recvDecl->kind == ast::TypeDeclKind::Class)
        emitProducerRelease();
    // fix #2717: the INDEX-argument twin of the value-side emitProducerRelease
    // above. Emitted here — after the setter body, before the caller's
    // locals_/paramSlots_ are restored — so it dominates every use the body
    // makes of the index, and releases the ORIGINAL producer SSA value rather
    // than whatever #2712's retain/drop-on-overwrite pair left in the slot.
    releaseIndexArgStringProducers(ix, ps, recvDecl, argVals,
                                   /*accessorResultEscapes=*/false);
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
    const sema::TypeSymbol* recvDecl = indexAccessReceiverDecl(*ix);
    if (!recvDecl) return false;
    auto* ps = selectIndexer(recvDecl, *ix);  // per §23.4 (#1172)
    if (!ps) return false;
    if (rejectImportedVirtualBaseIndexer(*ix, ps)) return false;  // #2832
    auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl);
    // fix #2509 (§9.2/§23.4, v0.236): the IMPORTED-indexer compound-assign
    // twin of #2489's `!pd` branches in lowerIndexerGet/lowerIndexerSet
    // (this file) — and of this function's own LOCAL `pd != nullptr` path
    // below, which #2507 had just fixed for the receiver-resolution
    // copy-vs-address crash. An imported indexer has no ast::PropertyDecl
    // (reconstructed from side tables only, per #2414's own comment on
    // lowerIndexerGet); pre-fix this function bailed `if (!pd) return
    // false;` immediately, the caller then fell through
    // lowerCollectionIndexerCompound (false: not Array<T>/Dictionary<K,V>)
    // and the generic lvalueAddr fallback (null: no LLVM ArrayType for a
    // user indexer) — and the write was SILENTLY DROPPED, no crash, no
    // diagnostic (confirmed by probe against origin/main: `r[5] += 3;
    // return r[2];` against an imported RECORD indexer returned 2, the
    // pristine getter value `_v+2 = 0+2`, not the expected 5; identical
    // shape, identical exit code, against an imported CLASS indexer —
    // #2489's own Class||Record scope).
    //
    // Mirror #2489's `!pd` branches: dispatch directly through the
    // exported `bz$<Class>$idxget$<arity>` / `bz$<Class>$idxset$<arity>`
    // ABI stubs (importedIndexerGetterFns_ / importedIndexerSetterFns_),
    // evaluating the receiver and each index expression ONCE and sharing
    // them between the two calls (§23.4 single-evaluation contract — this
    // function's own header comment already documents it). Calling
    // lowerIndexerGet/lowerIndexerSet directly would re-evaluate those
    // sub-expressions, which the issue text explicitly rules out (a
    // #2510-style refactor, not a fix). Receiver storage resolution
    // mirrors both the #2507 local path below (lvalueAddr over lowerExpr:
    // a record receiver is by-value per §9.2, so lowerExpr loads a copy
    // and the setter would mutate a throwaway slot — #2489's own
    // confirmed defect for plain `r[i] := v`) and #2489's lowerIndexerSet
    // `!pd` branch.
    //
    // fix #2836 (§23.4/§11.2, v0.240): exclude an INTERFACE receiver from this
    // gate, exactly as #2422 excluded it from lowerIndexerGet's and
    // lowerIndexerSet's identical `!pd` gates. An IMPORTED interface's indexer
    // also has `pd == null`, and its accessors live in the itable, not in the
    // `bz$<Class>$idx{get,set}$<arity>` direct-call ABI — it must reach the
    // itable branch added just below rather than be rejected here.
    if (!pd && recvDecl->kind != ast::TypeDeclKind::Interface) {
        if (recvDecl->kind != ast::TypeDeclKind::Class &&
            recvDecl->kind != ast::TypeDeclKind::Record) return false;
        auto git = importedIndexerGetterFns_.find(ps);
        auto sit = importedIndexerSetterFns_.find(ps);
        if (git == importedIndexerGetterFns_.end() ||
            sit == importedIndexerSetterFns_.end()) return false;
        llvm::Function* getFn = git->second;
        llvm::Function* setFn = sit->second;

        // --- Resolve receiver storage ONCE. ---
        llvm::Value* obj = nullptr;
        llvm::Type* objElemTy = nullptr;
        if (llvm::Value* addr = lvalueAddr(*ix->target, objElemTy)) {
            if (objElemTy && objElemTy->isPointerTy())
                obj = builder_.CreateLoad(objElemTy, addr);
            else if (objElemTy && objElemTy->isAggregateType())
                obj = addr;  // address of the receiver's real storage
        }
        if (!obj) {
            llvm::Value* ov = lowerExpr(*ix->target);
            if (!ov) return false;
            if (ov->getType()->isPointerTy()) {
                obj = ov;
            } else if (ov->getType()->isAggregateType()) {
                llvm::AllocaInst* tmp = entryAlloca(ov->getType());
                builder_.CreateStore(ov, tmp);
                obj = tmp;
            } else {
                return false;
            }
        }

        // --- Spill each index param ONCE; shared by getter and setter. ---
        auto* getFnTy = getFn->getFunctionType();
        std::vector<llvm::AllocaInst*> idxSlots;
        std::vector<llvm::Value*> idxArgVals;   // fix #2717: original SSA values
        idxSlots.reserve(ps->indexerParams.size());
        for (std::size_t k = 0; k < ps->indexerParams.size() &&
             k < ix->indices.size() &&
             k + 1 < getFnTy->getNumParams(); ++k) {
            if (!ix->indices[k].value) return false;
            llvm::Value* argVal = lowerExpr(*ix->indices[k].value);
            if (!argVal) return false;
            llvm::AllocaInst* slot = entryAlloca(argVal->getType());
            builder_.CreateStore(argVal, slot);
            idxSlots.push_back(slot);
            idxArgVals.push_back(argVal);
        }

        // --- Call getter with the shared receiver and indices. ---
        std::vector<llvm::Value*> getArgs;
        getArgs.push_back(obj);
        for (std::size_t k = 0; k < idxSlots.size() &&
             k + 1 < getFnTy->getNumParams(); ++k) {
            if (!idxSlots[k]) return false;
            llvm::Value* v = builder_.CreateLoad(
                idxSlots[k]->getAllocatedType(), idxSlots[k]);
            getArgs.push_back(coerceNumericToType(
                v, getFnTy->getParamType(k + 1), false));
        }
        llvm::Value* cur = builder_.CreateCall(getFnTy, getFn, getArgs);
        emitPendingCheck();
        if (!cur) return false;

        // --- Apply the compound operator. ---
        llvm::Value* rhs = lowerExpr(*as.value);
        if (!rhs) {
            // #1386/#2509: ICE — lowerExpr returned null for the RHS.
            ctx_.diagnostics().report(
                Severity::Error, as.value->span.begin,
                "internal: lowerExpr returned null for compound indexer "
                "assignment RHS (imported-indexer path) — #2509");
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

        // --- Call setter with the new value + shared receiver/indices. ---
        auto* setFnTy = setFn->getFunctionType();
        if (setFnTy->getNumParams() == 0) return false;
        std::vector<llvm::Value*> setArgs;
        setArgs.push_back(obj);
        for (std::size_t k = 0; k < idxSlots.size() &&
             k + 1 < setFnTy->getNumParams(); ++k) {
            if (!idxSlots[k]) return false;
            llvm::Value* v = builder_.CreateLoad(
                idxSlots[k]->getAllocatedType(), idxSlots[k]);
            setArgs.push_back(coerceNumericToType(
                v, setFnTy->getParamType(k + 1), false));
        }
        llvm::Type* valTy = setFnTy->getParamType(setFnTy->getNumParams() - 1);
        setArgs.push_back(coerceNumericToType(newVal, valTy, false));
        builder_.CreateCall(setFnTy, setFn, setArgs);
        emitPendingCheck();
        // fix #2717: the index was evaluated ONCE and shared by the getter and
        // the setter, so its producer +1 is discharged once, HERE — after the
        // setter, the last consumer of the index on this path.
        releaseIndexArgStringProducers(*ix, ps, recvDecl, idxArgVals,
                                       /*accessorResultEscapes=*/false);
        return true;
    }

    // fix #2836 (§6.6 / §23.4 / §12.1, v0.240) — THE TWO DYNAMIC-DISPATCH
    // BRANCHES THIS FUNCTION NEVER HAD.
    //
    // §12.1, verbatim: "A method, property, or indexer is non-virtual unless
    // marked `virtual`. To override a virtual member in a derived class, mark
    // the matching member `override`." §23.4, verbatim: indexers "participate
    // in `virtual`/`override`/`sealed`/`abstract` like methods."
    //
    // `lowerIndexerGet` and `lowerIndexerSet` each carry an interface-itable
    // branch (#730) and a class-vtable branch (#1080) so a virtual indexer
    // dispatches on the RUNTIME type. `lowerIndexerCompound` carried NEITHER:
    // its local (`pd != nullptr`) path went straight to "find a getter accessor
    // body / find a setter accessor body" and inlined the COMPILE-TIME receiver
    // type's bodies. So `a[i] OP= v` was always statically bound, while the
    // plain `a[i]` read and the plain `a[i] := v` write right next to it
    // dispatched correctly — a SILENT wrong value on an idiomatic shape.
    // MEASURED on 9983ddb, `this[0] += 1` through a `virtual`/`override` pair:
    // 102 (the base's accessors) where 203 (the override's) is correct.
    //
    // An INTERFACE receiver was worse still: `pd` is the (bodiless) interface
    // PropertyDecl, so the accessor-body search below found nothing and the
    // whole statement was DROPPED — MEASURED 0, no diagnostic. So was an
    // `abstract` indexer declared on an abstract base and compounded through
    // `this[i]` in that base's own method (MEASURED 0): same bodiless shape.
    //
    // WHY NOT DELEGATE TO lowerIndexerGet + lowerIndexerSet (the #2510 shape,
    // correctly declined). §6.6, verbatim (the document's hard line wraps
    // joined with a single space): "the statement `x op= e` is equivalent to
    // `x := x op e` with the target evaluated **once**", and "every constituent
    // of the left-hand side's lvalue chain … is evaluated exactly once … and
    // every index expression in the chain". Driving the two
    // siblings would lower the receiver and each index TWICE. Both branches
    // below therefore evaluate the receiver and every index expression ONCE,
    // spill each index into a slot, and share those slots between the getter
    // call and the setter call — the same discipline the `!pd` branch above
    // already uses, and in §6.6's stated order (LHS constituents, getter, `e`,
    // the operation, the store).
    //
    // `base[i] OP= v` DECLINES the vtable branch, exactly as #2832 made
    // lowerIndexerGet's and lowerIndexerSet's decline it (§12.3: "Inside a
    // derived member, `base.Name(...)` calls the base class's implementation,
    // bypassing virtual dispatch"). The base indexer shares its vtable slot
    // with the derived override, so dispatching would run the override.
    //
    // Shared by both branches: §6.6's "then `e`, then the operation". Returns
    // the new value; `rhsOut` receives the lowered RHS so the caller can
    // discharge its +1 (see each branch's release block).
    auto emitCompoundValue = [&](llvm::Value* cur,
                                 llvm::Value*& rhsOut) -> llvm::Value* {
        rhsOut = nullptr;
        llvm::Value* rhs = lowerExpr(*as.value);
        if (!rhs) {
            // #1386 precedent: ICE rather than a silently dropped store.
            ctx_.diagnostics().report(
                Severity::Error, as.value->span.begin,
                "internal: lowerExpr returned null for compound indexer "
                "assignment RHS (dynamic-dispatch path) — #2836");
            return nullptr;
        }
        rhsOut = rhs;
        std::string op(as.op);
        if (!op.empty() && op.back() == '=') op.pop_back();
        return emitCompoundOp(
            op, cur, rhs, cur->getType()->isFloatingPointTy(),
            isUnsignedTargetExpr(*as.target),
            isCharTargetExpr(*as.target),      // §6.6/§5.2 #1961
            isUnsignedTargetExpr(*as.value));  // §6.2 #2140 rUns
    };

    // Shared by both branches: discharge the +1 references the desugaring
    // minted. `newVal` is `emitCompoundOp`'s result, and that function returns
    // a POINTER on exactly one path — `op == "+"` with two pointer operands,
    // which routes to `emitStringConcat` (a fresh rc=1 per #422); every other
    // path is numeric. So "pointer-typed" IS "a fresh string" here, which is
    // why the test is on the lowered type rather than a source-type category.
    // `rhs` is released only when the RHS AST is itself a +1 producer (a named
    // local is a BORROW — releasing it would drop the owner's only reference);
    // `emitCompoundOp` does not consume `rhs` (it concatenates into a fresh
    // buffer), so a producer's +1 would otherwise be stranded. This mirrors the
    // local inline path's own #2567 release block at the end of this function,
    // one for one. Only sound on a CLASS-backed store, where the setter retains
    // (§23.3/§9); the caller passes `classBacked=false` where the runtime
    // implementor may be a RECORD (whose reference fields are borrows stored
    // plainly, §9/§9.7 — releasing there would dangle the field).
    auto releaseCompoundProducers = [&](llvm::Value* newVal, llvm::Value* rhs,
                                        bool classBacked) {
        if (!classBacked || diverged_) return;
        if (newVal && newVal->getType()->isPointerTy())
            emitStringRelease(newVal);
        if (rhs && rhs->getType()->isPointerTy() && as.value.get() &&
            isPlusOneStringProducer(as.value.get()))
            emitStringRelease(rhs);
    };

    // --- #730 / §13 / §23.4 / §6.6: INTERFACE-typed receiver. -----------------
    // `(q: IFoo)[i] OP= v` dispatches BOTH halves through the itable — the
    // compound twin of lowerIndexerGet's and lowerIndexerSet's own #730
    // branches. `itab` is computed ONCE, before the getter call, and both
    // fn-ptr loads index it: re-deriving it after the setter would read a
    // receiver the setter has already mutated (lowerIndexerSet's #2506 note).
    // Handles `pd == null` (an IMPORTED interface indexer, #2422) by reading
    // the signature off the reconstructed side tables, as those branches do.
    if (recvDecl->kind == ast::TypeDeclKind::Interface) {
        auto gIt = ifacePropertyGetIndex_.find(ps);
        auto sIt = ifacePropertySetIndex_.find(ps);
        auto descIt = descArray_.find(recvDecl);
        if (gIt == ifacePropertyGetIndex_.end() ||
            sIt == ifacePropertySetIndex_.end() ||
            descIt == descArray_.end()) return false;
        // fix #2528's resolution: an interface generic parameter is resolved
        // against the receiver instantiation BEFORE LLVM lowering.
        const sema::TypeRef* elemRef =
            interfaceIndexerElementType(ctx_, *ix, recvDecl, ps);
        llvm::Type* elemTy = lowerTypeRef(resolveTypeParamRef(elemRef));
        if (!elemTy) elemTy = i32();
        const std::size_t nparams = indexerParamCount(ps);
        std::vector<llvm::Type*> paramTys;
        paramTys.push_back(ptrTy());
        for (std::size_t k = 0; k < nparams; ++k) {
            llvm::Type* pt = nullptr;
            if (pd) {
                if (k < pd->indexerParams.size() && pd->indexerParams[k].type)
                    pt = lowerType(pd->indexerParams[k].type.get());
            } else if (auto* tr = indexerParamType(ctx_, ps, k)) {
                pt = lowerTypeRef(tr);
            }
            paramTys.push_back(pt ? pt : i32());
        }
        auto* gFnTy = llvm::FunctionType::get(elemTy, paramTys, false);
        std::vector<llvm::Type*> setParamTys(paramTys);
        setParamTys.push_back(elemTy);
        auto* sFnTy = llvm::FunctionType::get(
            llvm::Type::getVoidTy(llvm_), setParamTys, false);

        // §6.6: the receiver, then every index, each exactly once.
        llvm::Value* obj = lowerExpr(*ix->target);
        if (!obj || !obj->getType()->isPointerTy()) return false;
        std::vector<llvm::Value*> idxArgVals;   // original SSA values (#2717)
        std::vector<llvm::AllocaInst*> idxSlots;
        for (std::size_t k = 0; k < nparams && k < ix->indices.size(); ++k) {
            if (!ix->indices[k].value) return false;
            llvm::Value* argVal = lowerExpr(*ix->indices[k].value);
            if (!argVal) return false;
            llvm::AllocaInst* slot = entryAlloca(argVal->getType());
            builder_.CreateStore(argVal, slot);
            idxSlots.push_back(slot);
            idxArgVals.push_back(argVal);
        }
        auto loadIndices = [&](std::vector<llvm::Value*>& into) {
            for (std::size_t k = 0; k < idxSlots.size(); ++k)
                into.push_back(coerceNumericToType(
                    builder_.CreateLoad(idxSlots[k]->getAllocatedType(),
                                        idxSlots[k]),
                    paramTys[k + 1], false));
        };

        llvm::Value* vptr = builder_.CreateLoad(ptrTy(), obj);
        auto* lookupFn =
            declareC("bz_itable_lookup", ptrTy(), {ptrTy(), ptrTy()});
        llvm::Value* itmapPtr = builder_.CreateLoad(
            ptrTy(), builder_.CreateGEP(ptrTy(), vptr, {builder_.getInt32(1)}));
        llvm::Value* itab = builder_.CreateCall(
            llvm::FunctionType::get(ptrTy(), {ptrTy(), ptrTy()}, false),
            lookupFn, {itmapPtr, descIt->second}, "itab");

        llvm::Value* getPtr = builder_.CreateLoad(
            ptrTy(),
            builder_.CreateGEP(ptrTy(), itab, {builder_.getInt32(gIt->second)}),
            "idxget");
        std::vector<llvm::Value*> getArgs;
        getArgs.push_back(obj);
        loadIndices(getArgs);
        llvm::Value* cur = builder_.CreateCall(gFnTy, getPtr, getArgs);
        emitPendingCheck();
        if (!cur) return false;

        llvm::Value* rhs = nullptr;
        llvm::Value* newVal = emitCompoundValue(cur, rhs);
        if (!newVal) return false;

        llvm::Value* setPtr = builder_.CreateLoad(
            ptrTy(),
            builder_.CreateGEP(ptrTy(), itab, {builder_.getInt32(sIt->second)}),
            "idxset");
        std::vector<llvm::Value*> setArgs;
        setArgs.push_back(obj);
        loadIndices(setArgs);
        setArgs.push_back(coerceNumericToType(newVal, elemTy, false));
        builder_.CreateCall(sFnTy, setPtr, setArgs);
        emitPendingCheck();
        // RECORDED SCOPE LIMIT, not an oversight: no producer release fires on
        // this branch. The runtime implementor behind an interface may be
        // RECORD-backed, whose setter PLAIN-STORES (§9/§9.7), and releasing
        // there would drop the field's only reference — a UAF. Distinguishing
        // the two needs lowerIndexerSet's #2506/#2535 runtime itable owner-kind
        // flag; until that is carried here a `string`-element compound through
        // an interface leaks its concatenation (MEASURED: 355 B / 20 allocs
        // over 5 iterations, no UAF — a bounded leak, the safe direction; filed
        // as #2847). `releaseIndexArgStringProducers` reaches
        // the same conclusion on its own (it early-returns for a non-Class
        // recvDecl), so it is not called here.
        return true;
    }

    // --- #1080 / §23.4 / §12.1 / §6.6: CLASS receiver, vtable dispatch. -------
    // The compound twin of lowerIndexerGet's and lowerIndexerSet's own #1080
    // branches: load the getter/setter fn-ptrs from vt[propGetSlot_] /
    // vt[propSetSlot_] and call them as `T get(ptr this, idx...)` /
    // `void set(ptr this, idx..., T)`. Slots were assigned by buildVtable and
    // filled by ensureIndexerGetterFn / -SetterFn.
    //
    // `!indexTargetIsBase` per §12.3 — see the block comment above. Both slots
    // are required: a getter-only virtual indexer has no setter to compound
    // through, and falling through to the static path (which would find no
    // setter body either) keeps that shape's existing outcome rather than
    // inventing a new one. MEASURED: that outcome is a SILENT no-op, identical
    // for the non-virtual spelling — a pre-existing SEMA gap (a write to a
    // getter-only indexer should be rejected, as it already is for a named
    // property), filed as #2851 and deliberately not papered over here.
    if (recvDecl->kind == ast::TypeDeclKind::Class && !indexTargetIsBase(*ix)) {
        auto gsIt = propGetSlot_.find(ps);
        auto ssIt = propSetSlot_.find(ps);
        if (gsIt != propGetSlot_.end() && ssIt != propSetSlot_.end()) {
            llvm::Type* elemTy = lowerType(pd->type.get());
            if (!elemTy) elemTy = i32();
            std::vector<llvm::Type*> paramTys;
            paramTys.push_back(ptrTy());
            for (const auto& p : pd->indexerParams) {
                llvm::Type* pt = p.type ? lowerType(p.type.get()) : nullptr;
                paramTys.push_back(pt ? pt : i32());
            }
            auto* gFnTy = llvm::FunctionType::get(elemTy, paramTys, false);
            std::vector<llvm::Type*> setParamTys(paramTys);
            setParamTys.push_back(elemTy);
            auto* sFnTy = llvm::FunctionType::get(
                llvm::Type::getVoidTy(llvm_), setParamTys, false);

            // §6.6: the receiver, then every index, each exactly once. A class
            // receiver is a pointer, so `lowerExpr` yields the object itself
            // (no #2507 lvalueAddr spill is needed or wanted — that exists for
            // the by-value RECORD receiver, and a record has no vtable: §9,
            // "A record cannot derive from another record or from a class").
            llvm::Value* obj = lowerExpr(*ix->target);
            if (obj && obj->getType()->isPointerTy()) {
                std::vector<llvm::Value*> idxArgVals;      // #2717
                std::vector<llvm::AllocaInst*> idxSlots;
                for (std::size_t k = 0; k < pd->indexerParams.size() &&
                     k < ix->indices.size(); ++k) {
                    if (!ix->indices[k].value) return false;
                    llvm::Value* argVal = lowerExpr(*ix->indices[k].value);
                    if (!argVal) return false;
                    llvm::AllocaInst* slot = entryAlloca(argVal->getType());
                    builder_.CreateStore(argVal, slot);
                    idxSlots.push_back(slot);
                    idxArgVals.push_back(argVal);
                }
                auto loadIndices = [&](std::vector<llvm::Value*>& into) {
                    for (std::size_t k = 0; k < idxSlots.size(); ++k)
                        into.push_back(coerceNumericToType(
                            builder_.CreateLoad(
                                idxSlots[k]->getAllocatedType(), idxSlots[k]),
                            paramTys[k + 1], false));
                };

                llvm::Value* vptr = builder_.CreateLoad(ptrTy(), obj);
                llvm::Value* getPtr = builder_.CreateLoad(
                    ptrTy(),
                    builder_.CreateGEP(ptrTy(), vptr,
                                       {builder_.getInt32(gsIt->second)}),
                    "idxget");
                std::vector<llvm::Value*> getArgs;
                getArgs.push_back(obj);
                loadIndices(getArgs);
                llvm::Value* cur = builder_.CreateCall(gFnTy, getPtr, getArgs);
                emitPendingCheck();
                if (!cur) return false;

                llvm::Value* rhs = nullptr;
                llvm::Value* newVal = emitCompoundValue(cur, rhs);
                if (!newVal) return false;

                llvm::Value* setPtr = builder_.CreateLoad(
                    ptrTy(),
                    builder_.CreateGEP(ptrTy(), vptr,
                                       {builder_.getInt32(ssIt->second)}),
                    "idxset");
                std::vector<llvm::Value*> setArgs;
                setArgs.push_back(obj);
                loadIndices(setArgs);
                setArgs.push_back(coerceNumericToType(newVal, elemTy, false));
                builder_.CreateCall(sFnTy, setPtr, setArgs);
                emitPendingCheck();
                // A vtable slot is always a CLASS's own slot (§9 again), so the
                // setter's class-gated retain fired and the desugaring's +1s are
                // doubled/stranded exactly as on the inline path — #2548's own
                // argument for the plain-set twin of this branch.
                releaseCompoundProducers(newVal, rhs, /*classBacked=*/true);
                // fix #2717: the index was lowered ONCE and shared by both
                // calls, so its producer +1 is discharged once, here — after
                // the setter, its last consumer.
                releaseIndexArgStringProducers(*ix, ps, recvDecl, idxArgVals,
                                               /*accessorResultEscapes=*/false);
                return true;
            }
        }
    }

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
    //
    // fix #2507 (§9.2/§23.4, v0.236): mirrors #2502's lowerIndexerSet fix (this
    // file) — resolve the receiver's OWN storage address via `lvalueAddr` rather
    // than loading a COPY via `lowerExpr`. The old `obj = lowerExpr(*ix->target)`
    // LOADS a byte-for-byte copy for a named record-typed local (records are
    // by-value, §9.2); that raw struct value was then bound directly as
    // `currentThis_` below with NO pointer/aggregate check at all for the
    // expression-getter and setter calls (the block-getter branch's own local
    // `objPtr` spill did check, but never wrote the spilled address back to
    // `obj`, so the setter call still got the unchecked raw copy). Confirmed by
    // probe (this issue's own repro): `r[5] += 3; return r[2];` failed IR
    // verification — "GEP base pointer is not a vector or a vector of pointers"
    // / "Store operand must be a pointer" — a compile-time abort on a
    // legitimate, spec-permitted shape (§23.4 read-write indexer on a record).
    // Falls back to `lowerExpr` + spill only when the receiver expression is
    // NOT an addressable place (a fresh temporary, where no "original" exists
    // to preserve, so a spilled copy is legal-but-meaningless, not unsound).
    llvm::Value* obj = nullptr;
    llvm::Type* objElemTy = nullptr;
    if (llvm::Value* addr = lvalueAddr(*ix->target, objElemTy)) {
        if (objElemTy && objElemTy->isPointerTy())
            obj = builder_.CreateLoad(objElemTy, addr);
        else if (objElemTy && objElemTy->isAggregateType())
            obj = addr;  // already the address of the receiver's real storage
    }
    if (!obj) {
        llvm::Value* ov = lowerExpr(*ix->target);
        if (!ov) return false;
        if (ov->getType()->isPointerTy()) {
            obj = ov;
        } else if (ov->getType()->isAggregateType()) {
            llvm::AllocaInst* tmp = entryAlloca(ov->getType());
            builder_.CreateStore(ov, tmp);
            obj = tmp;
        } else {
            return false;
        }
    }
    // `obj` is now already pointer-typed in every path above (a loaded pointer,
    // the receiver's own aggregate storage address, or a spilled temporary's
    // address) — the block-getter branch's own `objPtr` check just below is
    // therefore now defense-in-depth, not a required spill.

    // Spill each index param into a slot; the getter and setter bodies will
    // read back from paramSlots_ which we rebuild identically for each call.
    std::vector<llvm::AllocaInst*> idxSlots;
    std::vector<llvm::Value*> idxArgVals;   // fix #2717: original SSA values
    idxSlots.reserve(ps->indexerParams.size());
    for (std::size_t k = 0; k < ps->indexerParams.size() &&
         k < ix->indices.size(); ++k) {
        if (!ix->indices[k].value) {
            idxSlots.push_back(nullptr);
            idxArgVals.push_back(nullptr);
            continue;
        }
        llvm::Value* argVal = lowerExpr(*ix->indices[k].value);
        if (!argVal) {
            idxSlots.push_back(nullptr);
            idxArgVals.push_back(nullptr);
            continue;
        }
        llvm::AllocaInst* slot = entryAlloca(argVal->getType());
        builder_.CreateStore(argVal, slot);
        idxSlots.push_back(slot);
        idxArgVals.push_back(argVal);
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
    // fix #2712 / §28.1 / §28.9 / §23.4 (v0.240): the COMPOUND-ASSIGNMENT twin
    // of lowerIndexerSet's retain — this setter body is inlined into the
    // enclosing function the same way, and `t[s] += 1` on an indexer whose
    // block setter reassigns its own `string` index parameter reproduced the
    // same `heap-use-after-free`. See lowerIndexerSet for the full argument.
    if (setterStmts)
        retainAssignedStringParams(*setterStmts, ps->indexerParams, pd);
    if (setterExpr) lowerExpr(*setterExpr);
    else if (setterStmts) lowerScopedBlock(*setterStmts);
    // fix #2567 (§23.4/§28.9/§4.6, v0.236): the COMPOUND-ASSIGNMENT twin of
    // #2548's inline-body `emitProducerRelease()` (this file, line 2153-2154).
    // `lowerIndexerCompound` desugars `a[i] OP= rhs` to `a[i] := a[i] OP rhs`:
    //   * line 2452: `rhs = lowerExpr(*as.value)` — the rhs of the `+=`, e.g.
    //     the +1 string producer from `"abc" + i.ToString()`.
    //   * line 2464: `newVal = emitCompoundOp(op, cur, rhs, ...)` — a SECOND
    //     +1 producer (emitStringConcat allocates rc=1 per #422).
    //   * line 2488: `locals_[pd] = newVal` binds newVal as the setter's
    //     implicit `value` parameter.
    //   * line 2490-2491: setter body stores newVal to the class field.
    //
    // For a CLASS receiver, the setter (or inlined setter body) RETAINs its
    // incoming value (§23.3 / §9), doubling newVal's +1. The plain-assignment
    // path (`lowerIndexerSet`) closes this with a post-body `emitProducerRelease()`
    // gated on `recvDecl->kind == Class`; the compound path had no analogue, so
    // `h[i] += "abc" + i.ToString()` on a class indexer leaks 86 byte(s) per
    // iteration (LSan-confirmed: 430 byte(s) direct + indirect across 5
    // iterations of a 2-arg `+=` probe; the same shape leaks on a 1-arg
    // indexer too — the gap is arity-independent, the same as `lowerIndexerSet`'s
    // fix). The fix is the COMPOUND twin of that emitProducerRelease: after the
    // setter body, drop newVal's doubled +1.
    //
    // BUT — and this is the subtle part — `rhs` ALSO leaks here. `rhs` is
    // `lowerExpr(*as.value)`, a +1 string for `"abc" + i.ToString()`'s AST
    // shape. `emitCompoundOp` does NOT consume rhs (it memcpy's its bytes
    // into newVal and leaves rhs's +1 untouched). The plain-assignment path
    // closes this via the SAME emitProducerRelease, gated on the outer value
    // Expr's `isPlusOneStringProducer` predicate. We don't have the same AST
    // context here (the outer "as" is a compound AssignmentStmt, whose `as.value`
    // is the rhs AST — and that AST is itself a +1 producer for the canonical
    // `"abc" + i.ToString()` shape). For the string-`+` case, rhs is a +1
    // producer whose +1 was never discharged (the producer's whole life is
    // here — line 2452's `lowerExpr` returns it; line 2464's emitStringConcat
    // borrows it; the setter body stores newVal, NOT rhs). So rhs must be
    // released here too.
    //
    // For a record receiver the setter plain-stores (§9/§9.7), so releasing
    // would drop the field's only reference (UAF). Same gate as
    // `lowerIndexerSet`'s inline-body emitProducerRelease at line 2153.
    if (recvDecl->kind == ast::TypeDeclKind::Class) {
        if (newVal && newVal->getType()->isPointerTy())
            emitStringRelease(newVal);   // release the OP-result's +1
        // For string `+=`, rhs is `lowerExpr(*as.value)` — a +1 string
        // when the AST is a producer (e.g. BinaryExpr("+", ...)) but a
        // BORROW when the AST is a named local (e.g. `s += t` where `t` is
        // a tracked string local). Check the AST shape, not the LLVM
        // pointer type: only release when the rhs is itself a +1 producer.
        if (rhs && rhs->getType()->isPointerTy() && as.value.get() &&
            isPlusOneStringProducer(as.value.get()))
            emitStringRelease(rhs);      // release the rhs's +1 (producer)
    }
    // fix #2717: the INDEX-argument twin of the two value-side releases above.
    // The index was lowered ONCE (shared by the getter and the setter), so its
    // producer +1 is discharged once, after the setter — the last consumer.
    releaseIndexArgStringProducers(*ix, ps, recvDecl, idxArgVals,
                                   /*accessorResultEscapes=*/false);
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
        // fix #2596 (§23.4 / §28.9 / §4.6, v0.236): the DICT compound-assignment
        // leak on a borrowing Dictionary<K,string> (and SortedDictionary<K,string>).
        // The compound desugar `d[k] += rhs` mints TWO +1 string producers for a
        // string value: `rhs = lowerExpr(*as.value)` (a +1 for the canonical
        // BinaryExpr `"abc" + i.ToString()`) and `newVal = emitCompoundOp(...)`
        // which dispatches to emitStringConcat (a SECOND +1, per #422 — always
        // rc=1 regardless of whether rhs is itself a producer or a borrow). The
        // write-back below delegates to lowerCollectionIndexerSet, whose #429
        // borrowing-Dictionary<K,string> retain fires whenever valueExpr is null
        // OR not a +1 producer — and the compound path passed valueExpr=nullptr,
        // so the retain fired UNCONDITIONALLY, bumping newVal from +1 to +2.
        // The disposing setter then transferred only ONE +1 into the slot,
        // orphaning the retain's +1 each iteration (LSan on origin/main: 100
        // iterations of `d[0] += "abc" + i.ToString()` leaked 31485 byte(s)
        // across 400 allocations — two direct leak sites of 3200B each: the
        // spurious-retain'd newVal +1 and the rhs orphan +1; same shape as the
        // #2587 Array<string> compound leak and the #2567 user-indexer leak).
        //
        // The fix: signal to lowerCollectionIndexerSet that newVal is ALREADY a
        // +1 string producer (valIsFreshPlusOneString=true) so the #429 retain is
        // skipped and the +1 transfers into the slot via the disposing setter
        // (which also releases the displaced OLD value). Do NOT release newVal
        // afterward — its +1 is the slot's own reference (a COLLECTION indexer
        // TRANSFERs the +1, UNLIKE a CLASS property setter which RETAINs; see the
        // OWNERSHIP GATE in the #2587 isStrElemArr branch above). Then release rhs
        // when the AST is a +1 producer (emitStringConcat does NOT consume rhs —
        // it memcpy's its bytes and leaves rhs's +1 untouched). A BORROW rhs (a
        // named local) is NOT a producer; releasing it would UAF the local's only
        // ref. Gate on the AST shape, not the LLVM pointer type. Mirrors the
        // #2587 rhs-release (this file, the isStrElemArr branch ~L2785-2880) and
        // the plain-set path's conditional retain exactly.
        bool dictIsOwningC =
            recvTy && recvTy->astNode && recvTy->astNode->name &&
            !recvTy->astNode->name->parts.empty() &&
            recvTy->headName() == "OwningDictionary";
        bool isStrValDict =
            !dictIsOwningC && valTy && !valTy->nullable &&
            valTy->kind == sema::TypeRef::Kind::Primitive &&
            valTy->primitive == sema::PrimitiveType::String &&
            elementPolicyFor(valTy, /*owning=*/false).cat ==
                ElementPolicy::Category::RuntimeManaged;
        if (isStrValDict) {
            bool ok = lowerCollectionIndexerSet(*ix, newVal,
                /*valueExpr=*/nullptr, /*valIsFreshPlusOneString=*/true);
            // rhs is a +1 producer when the AST is a BinaryExpr/string-interp/
            // call (emitStringConcat does NOT consume rhs — it memcpy's its bytes
            // and leaves rhs's +1 untouched). A BORROW rhs (a named local) is NOT
            // a producer; releasing it would UAF the local's only ref at its
            // scope end. Gate on the AST shape, not the LLVM pointer type. Mirrors
            // the #2587 rhs-release (this file, isStrElemArr branch ~L2863).
            if (ok && rhs && rhs->getType()->isPointerTy() && as.value.get() &&
                isPlusOneStringProducer(as.value.get()))
                emitStringRelease(rhs);
            return ok;
        }
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

    // fix #2587 (§23.3/§28.9/§4.6, v0.236): the COLLECTION-INDEXER compound-
    // assignment leak on a borrowing Array<string> (and Stack<string> /
    // Queue<string>). The compound desugar `arr[i] OP= rhs` mints TWO +1
    // string producers for a string element: `rhs = lowerExpr(*as.value)` (a
    // +1 for the canonical BinaryExpr `"abc" + i.ToString()`) and `newVal =
    // emitCompoundOp(...)` which dispatches to emitStringConcat (a SECOND +1,
    // per #422). The generic write-back below derives its disposer from
    // elementPolicyFor, which returns disposerThunk=nullptr for a BORROWING
    // string element (RuntimeManaged row, retainRelease=true) — so it falls to
    // `bz_array_set` (a plain memcpy, no refcount management). That leaks
    // THREE ways: (1) rhs's +1 is an orphan (emitStringConcat memcpy's rhs's
    // bytes and leaves its +1 untouched), (2) the DISPLACED OLD slot value is
    // never released (bz_array_set has no disposer), and (3) each iteration's
    // newVal is byte-copied into the slot with no +1 bookkeeping. LSan on
    // origin/main: 100 iterations of `arr[0] += "abc" + i.ToString()` leak
    // 30961 byte(s) across 398 allocations (100 rhs headers + 99 displaced
    // old-value headers + their byte buffers).
    //
    // The plain-assignment path (lowerCollectionIndexerSet, this file line
    // ~957-987) closes this with a dedicated `isStrElemArr` branch: it emits
    // `bz_array_set_disposing` with stringElementReleaseThunk() (which
    // releases the displaced OLD slot value and memcpy's the new pointer in),
    // retaining first only when the incoming value is a BORROW. The compound
    // path had no analogue and fell through to the generic elementPolicyFor
    // store (bz_array_set, the bug). This block is the COMPOUND twin: mirror
    // that exact isStrElemArr branch.
    //
    // OWNERSHIP GATE — why newVal is NOT released here (UNLIKE #2567's class-
    // property compound fix): a CLASS property setter RETAINs its incoming
    // value (§23.3 / §9), doubling a +1 producer's refcount, so #2567
    // releases the OP-result's "extra" +1 after the setter body. A COLLECTION
    // indexer does NOT retain — it TRANSFERs the +1 into the slot (the
    // disposing setter releases the OLD value and memcpy's the new pointer
    // in, consuming exactly one +1). So newVal's +1 (from emitStringConcat)
    // is the slot's own reference; releasing it here would UAF the slot.
    // Only rhs's orphan +1 (when the AST is a producer) needs a caller-side
    // release — a borrow rhs's +1 belongs to its own slot and must NOT be
    // released (isPlusOneStringProducer gates this; a named-local rhs is a
    // borrow → no release → no UAF). Same ownership model as the plain-set
    // path's isStrElemArr branch (line ~975-987) and #429's dict-string fix.
    {
        const sema::TypeRef* sArrEl =
            (!recvTy->genericArgs.empty()) ? recvTy->genericArgs[0] : nullptr;
        bool sArrIsOwning =
            recvTy->astNode && recvTy->astNode->name &&
            !recvTy->astNode->name->parts.empty() &&
            common::isOwningPrefixedName(recvTy->headName());
        ElementPolicy sArrPol = elementPolicyFor(sArrEl, sArrIsOwning);
        bool isStrElemArr =
            !sArrIsOwning &&
            sArrPol.cat == ElementPolicy::Category::RuntimeManaged &&
            sArrEl && !sArrEl->nullable &&
            sArrEl->kind == sema::TypeRef::Kind::Primitive &&
            sArrEl->primitive == sema::PrimitiveType::String &&
            recvTy->astNode && recvTy->astNode->name &&
            !recvTy->astNode->name->parts.empty() &&
            [](std::string_view n){
                // #2598: IList<T> is array-backed + borrowing (issue #401),
                // same bz_array_set ABI as Array<T>; admitted alongside the
                // concrete borrowing families.
                return n=="Array"||n=="Stack"||n=="Queue"||n=="IList";
            }(recvTy->headName());
        if (isStrElemArr) {
            // newVal is +1 (emitStringConcat always mints rc=1). Transfer that
            // +1 into the slot via the disposing setter, which also releases
            // the displaced OLD value. Do NOT retain (newVal is already +1)
            // and do NOT release newVal afterward (its +1 is the slot's).
            llvm::AllocaInst* slot = entryAlloca(newVal->getType());
            builder_.CreateStore(newVal, slot);
            auto* setFn = declareC("bz_array_set_disposing",
                llvm::Type::getVoidTy(llvm_),
                {ptrTy(), i64(), ptrTy(), i64(), ptrTy()});
            builder_.CreateCall(setFn,
                {recv, idx, slot, builder_.getInt64(esz),
                 stringElementReleaseThunk()});
            // rhs is a +1 producer when the AST is a BinaryExpr/string-interp/
            // call (emitStringConcat does NOT consume rhs — it memcpy's its
            // bytes and leaves rhs's +1 untouched). A BORROW rhs (a named
            // local) is NOT a producer; releasing it would UAF the local's
            // only ref at its scope end. Gate on the AST shape, not the LLVM
            // pointer type. Mirrors lowerIndexerCompound line ~2539 (#2567).
            if (rhs && rhs->getType()->isPointerTy() && as.value.get() &&
                isPlusOneStringProducer(as.value.get()))
                emitStringRelease(rhs);
            return true;
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


// fix #2702 (§23.3 "Block-form properties" / §23.4 "Indexers" / §30.5 /
// §28.4, v0.240): save + neutralise the emitter state that a NESTED body
// emission must not inherit; the destructor restores it.
//
// WHY A GUARD AND NOT ANOTHER FIELD-BY-FIELD PATCH. The three adopters below
// (`ensurePropertyBlockGetter`, `ensureIndexerBlockGetter`,
// `ensurePropertyAccessorFn`) lower a user statement body into a SEPARATE,
// MEMOISED function from the middle of another function's emission. Any
// per-body field they fail to reset leaks the CALLER's context into the callee
// — and because the function is cached, whichever caller happened to be lowered
// first freezes that body for the whole module (the body loop iterates
// `std::unordered_map<sema::MethodSymbol*, llvm::Function*> methodFns_`,
// LlvmBackend_impl.hpp ~346/~565 — POINTER-HASH order, not declaration order).
// #2170, #2274/#2281 and #2694 each closed two fields of this set by hand at
// two or three sites; #2701 and #2702 are the next omissions. Putting the
// decision in one place is what stops the family from producing a new member
// every time a per-body field is added.
//
// ── RESET (the caller's value is provably wrong inside a separate function) ──
// Each entry below was CONFIRMED to be inherited in practice, by instrumenting
// every emitter's entry and compiling probe programs (not by code reading):
//   * currentReturnTypeNode_ / currentLambdaReturnSemaTy_ — #2694's pair. The
//     getter's `return` carries its OWN contract, never the reader's.
//   * smKind_ / smStateStruct_ / smStateGEP_ / smSelf_ / smDoneBB_ /
//     smStateBBs_ / smNextSuspendState_ / smLocalFields_ / smForInSrcFields_ /
//     smAllocaToField_ — the suspend/resume frame of the CALLER's state
//     machine. A getter is never a state machine. Confirmed inherited by
//     compiling a block-bodied getter read from an `async` body: every one of
//     these was live at the emitter's entry. `smStateGEP_` is the sharpest:
//     LlvmBackend_Stmts.cpp ~3699 stores through it on EVERY `try`'s unwind
//     edge gated on `smStateGEP_` ALONE, so nulling `smStateStruct_` (#2170)
//     does not protect it — that store lands in the caller's function and the
//     module fails IR verification ("Referring to an instruction in another
//     function!"). smKind_ is reset to `Yield` following #2170; `SuspendKind`
//     has no neutral value, and `Yield` is what OPENS the second gate at
//     ~3620 (`smStateGEP_ && (smKind_ == Yield || smKind_ == AsyncYield)`) —
//     which is exactly why smStateGEP_ must be nulled here rather than relying
//     on the kind. smDoneBB_ has no read reachable from lowering (verified);
//     it is reset with the rest of the frame because leaving one member of a
//     frame behind is how this family keeps recurring.
//   * iterCurrentValField_ / iterElemTy_ / iterElemSemaTy_ /
//     iterElemIsOwnedClass_ / iterCurrentValOwnedField_ — the yield-specific
//     half of that frame. `iterElemIsOwnedClass_` pairs with `smKind_` at
//     LlvmBackend_Stmts.cpp ~2829 (`smKind_ == Yield && iterElemIsOwnedClass_`),
//     and this guard FORCES `smKind_ == Yield`, so a stale `true` is a live
//     combination. These three were saved nowhere in the codebase before.
//   * asyncOuterTaskField_ / asyncPendingTaskField_ / asyncSpillPtrField_ —
//     frame field indices of the caller's async frame.
//   * emittingDisposerBody_ / disposerEntryExc_ — CONFIRMED inherited by
//     compiling a block getter whose first toucher is a `destructor` body.
//     `disposerEntryExc_` is a `CreateLoad` in the DESTRUCTOR's entry block
//     (LlvmBackend_Disposal.cpp ~126); `emitPendingCheck` (~1312) compares
//     against it after any call, so a getter body containing a call would
//     reference a value defined in another function.
//   * currentCaughtName_ / caughtExcArmTarget_ — the caller's catch-arm
//     binding. Benign today only because `currentCaughtExc_` is already nulled
//     by each emitter; resetting these makes that non-accidental rather than
//     a coincidence two guards apart.
//   * expectedFunctionType_ — describes an ARGUMENT POSITION in the caller;
//     `lowerLambda` reads it to infer a lambda's parameter/return types.
//   * lastCallGenericSubst_ — a transient `lowerExpr` → `lowerStmt` channel
//     (a call publishes, the next `var` decl consumes). A nested body's last
//     call would otherwise be consumed by the CALLER's next local declaration.
//   * asyncChainedReturn_ — fix #2731 item 1, the SHARPEST member of this set
//     and the only one whose omission was a live MEMORY-SAFETY defect rather
//     than an IR-verification failure. Same transient publish/consume shape as
//     lastCallGenericSubst_ above: LlvmBackend_Stmts.cpp ~1826 sets it when a
//     `return await <call>` is recognised as §30.5 Pattern A kind 2 (the
//     CHAINED SLOT), and it is read back TWICE — once by `lowerAwait`
//     (LlvmBackend_Async.cpp ~2932, `freeTaskAfter = isCall &&
//     !asyncChainedReturn_`) and once by the return statement itself
//     (Stmts.cpp ~1924, an UNCONDITIONAL read-and-clear with no
//     smKind_/smStateStruct_ gate). Both reads happen AFTER the return value's
//     operand has been lowered, so a nested body emitted during that lowering —
//     `return await Fetch(o.BlockProp)` — runs its OWN `return` through
//     Stmts.cpp ~1924 and clears the caller's flag before either legitimate
//     consumer sees it. Both then read `false`: `lowerAwait` emits
//     bz_task_free(source) at the await site (which runs the source task's
//     result_disposer on the awaited result), and the return skips the
//     bz_task_set_chained transfer and instead attaches a result disposer for
//     the SAME object to the outer task — a double dispose. §30.5 Pattern A
//     kind 2: "ownership of the source task transfers into the outer task's
//     state (a chained slot), and the outer destructor disposes the chained
//     source task, never the borrowed result directly … the source task is
//     disposed exactly once — by the outer task's destructor, transitively."
//     Reproduced as a heap-use-after-free under ASan and a SIGSEGV on the
//     plain oracle, through both ensurePropertyBlockGetter and
//     ensureIndexerBlockGetter (an expression-bodied getter is inlined, so it
//     does not reach this window).
//   * pendingOwnedParams_ / pendingArgsDisposal_ — fix #2731 item 3. Not a
//     per-call channel but a ROOT-SCOPE REGISTRATION channel: `bindParameters`
//     (LlvmBackend_Setup.cpp ~8579) fills pendingOwnedParams_ with the method's
//     `owned` parameters BEFORE any scope exists, and the FIRST root-scope
//     `lowerScopedBlock` (Setup.cpp ~21701/~21711, both gated on
//     `disposeStack_.size() == 1`) moves them out and registers each for §28.3
//     auto-drop. Every emitter here CLEARS `disposeStack_` before lowering, so
//     the nested body's own block IS a root scope and consumes the caller's
//     pending set. Setup.cpp ~21699 states the invariant this breaks in so many
//     words — "a lambda/thunk body that opens its own root scope inherits an
//     empty pendingOwnedParams_ unless its own bindParameters filled it".
//     The consumption is SILENT rather than a cross-function IR reference,
//     because `registerUnwindOwnedParam` (Setup.cpp ~11916) then bails on
//     `currentFn_ != unwindIneligibleFn_` — so the registration is dropped on
//     the floor and the caller's own `lowerScopedBlock` later finds the set
//     already empty. The `owned` parameter is then never disposed: a LEAK, and
//     one the compiler has already promised not to have ("owned parameter `r`
//     is implicitly disposed at the end of the method (§28.3 auto-drop)").
//     REACHED by a `requires` clause that reads a block-bodied property —
//     `emitRequiresPrologue` lowers that user expression (Setup.cpp ~4556)
//     between `bindParameters` and the body's own `lowerScopedBlock`
//     (Setup.cpp ~6348). That shape parses and typechecks; verified, and
//     verified to leak.
//   * inlinedAccessorEnd_ — a `BasicBlock*` in the CALLER's function; a bare
//     `return;` inside a nested body branches to it (LlvmBackend_Stmts.cpp
//     ~1777). Latent (sema rejects a valueless return in a value-returning
//     accessor) but a cross-function `br` if ever reached.
//
// ── DELIBERATELY INHERITED (resetting these would be a NEW bug) ──
// This list is the reason this guard is not simply "reset everything". Each
// was confirmed live at a nested emitter's entry too — but its value is
// CORRECT there:
//   * genericSubst_ — NOT cleared, and not even saved here (each emitter still
//     saves/restores it). The memoised getter's SIGNATURE is built by
//     `lowerType(pd->type.get())` BEFORE this guard runs, under the caller's
//     substitution; clearing the map would lower the BODY under a different
//     substitution than the signature it must satisfy. A generic class's block
//     getter would stop resolving `T` at all.
//   * genericSubstSemaTy_ — genericSubst_'s sema-level twin, read on the same
//     paths. Saved and restored here (it was saved by NONE of the emitters,
//     so a `new Foo<T>()` inside a getter body desynchronised the two halves)
//     but likewise NOT cleared, for the same reason.
//   * currentEnumMethod_ — `enumTypeOf` (LlvmBackend_Setup.cpp ~288) falls back
//     to it to resolve a bare `this` in an enum member. The correct value
//     inside a block getter is the getter's OWN owning type when that type is
//     an enum (which is precisely what `ensurePropertyAccessorFn` sets for
//     itself, #574) and null otherwise — NOT null unconditionally. The two
//     block-getter entry points do not receive the owning `TypeSymbol`, so
//     resetting it blind here would break a block-bodied ENUM property read
//     from inside an enum member, which works today. Left inherited on
//     purpose; #2702 item 3 stays open for the set-from-owner fix.
//   * currentSpecMethodSymbol_ / emittingGenericTemplateBody_ /
//     templateGenericParamNames_ — same shape as currentEnumMethod_: the right
//     value depends on whether the getter's OWN owner is an unspecialised
//     generic template, which is not knowable from these call sites. Confirmed
//     inherited (a block getter first touched from an unspecialised template
//     body enters with `emittingGenericTemplateBody_` true), so #2702 items 4
//     and 5 are real — they are left open rather than guessed at here.
Emitter::NestedBodyScope::NestedBodyScope(Emitter& e)
    : e_(e),
      retNode_(e.currentReturnTypeNode_),
      lamRet_(e.currentLambdaReturnSemaTy_),
      accessorRetNode_(e.accessorReturnTypeNode_),   // #2703
      accessorRetFn_(e.accessorReturnTypeFn_),       // #2703
      smKind_(e.smKind_),
      smStateStruct_(e.smStateStruct_),
      smSelf_(e.smSelf_),
      smStateGEP_(e.smStateGEP_),
      smStateBBs_(std::move(e.smStateBBs_)),
      smDoneBB_(e.smDoneBB_),
      smNextSuspendState_(e.smNextSuspendState_),
      smLocalFields_(std::move(e.smLocalFields_)),
      smForInSrcFields_(std::move(e.smForInSrcFields_)),
      smAllocaToField_(std::move(e.smAllocaToField_)),
      iterCurrentValField_(e.iterCurrentValField_),
      iterElemTy_(e.iterElemTy_),
      iterElemSemaTy_(e.iterElemSemaTy_),
      iterElemIsOwnedClass_(e.iterElemIsOwnedClass_),
      iterCurrentValOwnedField_(e.iterCurrentValOwnedField_),
      asyncOuterTaskField_(e.asyncOuterTaskField_),
      asyncPendingTaskField_(e.asyncPendingTaskField_),
      asyncSpillPtrField_(e.asyncSpillPtrField_),
      emittingDisposerBody_(e.emittingDisposerBody_),
      disposerEntryExc_(e.disposerEntryExc_),
      currentCaughtName_(e.currentCaughtName_),
      caughtExcArmTarget_(e.caughtExcArmTarget_),
      expectedFunctionType_(e.expectedFunctionType_),
      lastCallGenericSubst_(std::move(e.lastCallGenericSubst_)),
      inlinedAccessorEnd_(e.inlinedAccessorEnd_),
      asyncChainedReturn_(e.asyncChainedReturn_),
      pendingOwnedParams_(std::move(e.pendingOwnedParams_)),
      pendingArgsDisposal_(std::move(e.pendingArgsDisposal_)),
      genericSubstSemaTy_(e.genericSubstSemaTy_) {   // COPY: inherited, not cleared
    // The moved-from containers above are left in a valid-but-unspecified
    // state by `std::move`, so clear each explicitly.
    e.currentReturnTypeNode_     = nullptr;
    e.currentLambdaReturnSemaTy_ = nullptr;
    e.accessorReturnTypeNode_    = nullptr;              // #2703
    e.accessorReturnTypeFn_      = nullptr;              // #2703
    e.smKind_                    = SuspendKind::Yield;   // no neutral value
    e.smStateStruct_             = nullptr;
    e.smSelf_                    = nullptr;
    e.smStateGEP_                = nullptr;
    e.smStateBBs_.clear();
    e.smDoneBB_                  = nullptr;
    e.smNextSuspendState_        = 0;
    e.smLocalFields_.clear();
    e.smForInSrcFields_.clear();
    e.smAllocaToField_.clear();
    e.iterCurrentValField_       = 0;
    e.iterElemTy_                = nullptr;
    e.iterElemSemaTy_            = nullptr;
    e.iterElemIsOwnedClass_      = false;
    e.iterCurrentValOwnedField_  = 0;
    e.asyncOuterTaskField_       = 0;
    e.asyncPendingTaskField_     = 0;
    e.asyncSpillPtrField_        = 0;
    e.emittingDisposerBody_      = false;
    e.disposerEntryExc_          = nullptr;
    e.currentCaughtName_         = {};
    e.caughtExcArmTarget_        = nullptr;
    e.expectedFunctionType_      = nullptr;
    e.lastCallGenericSubst_.clear();
    e.inlinedAccessorEnd_        = nullptr;
    e.asyncChainedReturn_        = false;
    e.pendingOwnedParams_.clear();
    e.pendingArgsDisposal_.reset();
    // genericSubstSemaTy_ is intentionally left as-is (see the note above):
    // saved so a nested body's writes do not desynchronise it from
    // genericSubst_, but NOT cleared, because the body must resolve its type
    // parameters under the same substitution its signature was built with.
}

Emitter::NestedBodyScope::~NestedBodyScope() {
    e_.currentReturnTypeNode_     = retNode_;
    e_.currentLambdaReturnSemaTy_ = lamRet_;
    e_.accessorReturnTypeNode_    = accessorRetNode_;   // #2703
    e_.accessorReturnTypeFn_      = accessorRetFn_;     // #2703
    e_.smKind_                    = smKind_;
    e_.smStateStruct_             = smStateStruct_;
    e_.smSelf_                    = smSelf_;
    e_.smStateGEP_                = smStateGEP_;
    e_.smStateBBs_                = std::move(smStateBBs_);
    e_.smDoneBB_                  = smDoneBB_;
    e_.smNextSuspendState_        = smNextSuspendState_;
    e_.smLocalFields_             = std::move(smLocalFields_);
    e_.smForInSrcFields_          = std::move(smForInSrcFields_);
    e_.smAllocaToField_           = std::move(smAllocaToField_);
    e_.iterCurrentValField_       = iterCurrentValField_;
    e_.iterElemTy_                = iterElemTy_;
    e_.iterElemSemaTy_            = iterElemSemaTy_;
    e_.iterElemIsOwnedClass_      = iterElemIsOwnedClass_;
    e_.iterCurrentValOwnedField_  = iterCurrentValOwnedField_;
    e_.asyncOuterTaskField_       = asyncOuterTaskField_;
    e_.asyncPendingTaskField_     = asyncPendingTaskField_;
    e_.asyncSpillPtrField_        = asyncSpillPtrField_;
    e_.emittingDisposerBody_      = emittingDisposerBody_;
    e_.disposerEntryExc_          = disposerEntryExc_;
    e_.currentCaughtName_         = currentCaughtName_;
    e_.caughtExcArmTarget_        = caughtExcArmTarget_;
    e_.expectedFunctionType_      = expectedFunctionType_;
    e_.lastCallGenericSubst_      = std::move(lastCallGenericSubst_);
    e_.inlinedAccessorEnd_        = inlinedAccessorEnd_;
    e_.asyncChainedReturn_        = asyncChainedReturn_;
    e_.pendingOwnedParams_        = std::move(pendingOwnedParams_);
    e_.pendingArgsDisposal_       = std::move(pendingArgsDisposal_);
    e_.genericSubstSemaTy_        = std::move(genericSubstSemaTy_);
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
    // fix #2702 (§23.3/§30.5/§28.4, v0.240): everything a nested body emission
    // must not inherit — the #2170 state-machine pair (smKind_/smStateStruct_),
    // the #2694 return-contract pair, and the members those two fixes missed
    // (smStateGEP_ and the rest of the suspend frame, the destructor-body pair,
    // the catch-arm pair, and the narrow per-call channels) — is saved,
    // neutralised and restored by ONE guard. See its definition above for the
    // full list, the evidence for each member, and — importantly — the members
    // deliberately left INHERITED and why resetting them would be a new bug.
    // The guard's destructor runs last, after the explicit restores below, so
    // it always has the final say on the fields it owns.
    NestedBodyScope nestedScope(*this);
    // fix #2694 / §23.3 / §28.9 (v0.240): the SAME "a getter must not inherit
    // the caller's emission mode" reasoning as #2170 immediately above, for the
    // two RETURN-CONTRACT fields. This getter is MEMOIZED (propBlockGetterFns_)
    // and emitted LAZILY from expression lowering, and the body loop that
    // decides which call site lowers first iterates
    // `std::unordered_map<sema::MethodSymbol*, llvm::Function*> methodFns_`
    // (LlvmBackend_impl.hpp ~346/~565) — POINTER-HASH order, not declaration
    // order. So without this save+null the getter's `return <expr>` reached
    // emitReturn's `string` +1 mint (LlvmBackend_Stmts.cpp ~2113) carrying
    // whichever *caller's* return contract happened to hash first, and the
    // getter's ABI — baked once into the cached function — became a function of
    // hash-map iteration order. A `string`-typed block getter first touched
    // inside a `string`-returning METHOD (currentReturnTypeNode_) or, since
    // #2688, inside a block-bodied `string`-returning LAMBDA
    // (currentLambdaReturnSemaTy_) minted a +1 INSIDE the getter that neither
    // call site discharges: both the property-read path
    // (LlvmBackend_Exprs.cpp ~4342) and the indexer-read path (~1598 below)
    // hand the call result straight back as a BORROW, and #2304's producer
    // recognizer (LlvmBackend_Setup.cpp ~13703) deliberately does NOT classify
    // a statement-bodied block getter as a +1 producer. The getter's contract
    // is therefore +0, and NULL is the value that yields it — confirmed
    // mechanically: the identical program with two reader methods declared in
    // swapped order emitted the getter with and without `bz_string_retain`,
    // and only the retaining order leaked (LSan, 20 strings / 20 iterations).
    //
    // NULL rather than `pd->type.get()` (the getter's OWN return node) is
    // deliberate: pointing the field at the property's own type would make the
    // getter mint +1 for every `string` property read, which no call site
    // releases — a systematic leak, strictly worse than the order-dependence it
    // would replace. §8.6 boxing / §29.2 IEnumerable wrapping of a block
    // getter's returned value is the separate pre-existing defect that a
    // non-null node WOULD address; it fails IR verification today regardless of
    // this change (an `Object`-typed block getter returning an `int` emits
    // `ret i32` from a `ptr`-returning function whenever the first-touching
    // caller does not itself return Object), so this change only makes that
    // failure deterministic. Filed separately rather than fixed here.
    // (Both fields are now saved/nulled/restored by NestedBodyScope above,
    // together with the rest of the per-body set — fix #2702.)

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
    // fix #2703 (§23.3 / §8.6 / §29.2 / §15.1, v0.240): register the getter's
    // OWN declared type as the REPRESENTATION node for its `return`s. #2694
    // nulled `currentReturnTypeNode_` here to stop the +1 mint a getter's read
    // face must not perform (axiom A4, spec line 6351: "Accessor read faces are
    // lend-only … a getter may not return a fresh owned value"), and that is
    // still exactly right — this pair does not touch it. But the SAME field
    // also gates four purely-representational arms of `lowerReturn`, which have
    // nothing to do with ownership and are simply wrong when the node is
    // absent. All four were reproduced on the parent commit:
    //   * §8.6 boxing — `property O: Object get => do return _n; end` emitted
    //     `ret i32` from a `ptr`-returning function: "Function return type does
    //     not match operand type of return inst!" — a hard build failure.
    //   * §29.2 IEnumerable wrapping — `property E: IEnumerable<int>` whose
    //     body returns an `Array<int>` field handed the caller a raw
    //     `bz_array_t*`; the caller's for-in drove it through the §29.11
    //     enumerator vtable and SIGSEGV'd (confirmed, exit 139).
    //   * `coerceReturnToNullableValue` — `property L: long? … return 42;`
    //     failed IR verification ("ret i32 42" vs `{ i8, i64 }`). (A plain
    //     `int?` getter happens to survive: emitReturn has its own exact-width
    //     nullable-prim fallback. The widening/`T?`-monomorphisation/
    //     layout-identical-record shapes have no such backstop.)
    //   * collection-literal materialisation — `property A: [3]int … return
    //     [11,22,33];` left `v` null and returned a SILENT ZERO (exit 0 where
    //     11+22+33=66 was expected), with only the #2491 warning. The worst of
    //     the four: no crash, no diagnostic that fails the build.
    // `accessorReturnTypeFn_` pairs the node with THIS function so a nested
    // emission cannot inherit it; see the member's note in LlvmBackend_impl.hpp.
    accessorReturnTypeNode_ = pd->type.get();
    accessorReturnTypeFn_   = fn;

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
    // smKind_/smStateStruct_ (#2170), currentReturnTypeNode_/
    // currentLambdaReturnSemaTy_ (#2694) and the #2703 accessor-representation
    // pair are restored by ~NestedBodyScope.
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
    // #2281 / §31.2: an accessor is a separate function. Preserve the caller's
    // active try/finally frames across this nested emission; keep the parallel
    // finally-body stack depth-aligned per §29.7.
    auto  savedFinally   = finallyStack_;
    auto  savedFinBodies = finallyBodyStack_;
    auto* savedUnwind    = unwindTarget_;
    auto* savedProp      = propagateBB_;
    auto* savedCaught    = currentCaughtExc_;
    auto  savedUD        = unwindScopeDepth_;
    auto  savedDiv       = diverged_;
    auto  savedGSubst    = genericSubst_;
    // #2170 / §30.5: an accessor (block getter/setter) built during vtable
    // emission may also be reached from an async body; the saved/restored
    // smKind_/smStateStruct_ here ensures the accessor's `return` does not
    // accidentally trip the async-complete path on its parent. (Both fields,
    // and the rest of the per-body set, are now owned by NestedBodyScope.)
    //
    // #2708 (§23.3/§28.9, v0.240) — HARDENING, NOT A BUG FIX. #2708 reported
    // that this emitter still inherits the caller's RETURN CONTRACT
    // (`currentReturnTypeNode_`/`currentLambdaReturnSemaTy_`), the pair #2694
    // closed on the two direct-read emitters, and predicted the #2694 leak for
    // a `string` property read through an interface. That premise does NOT hold
    // today: unlike the two block getters, this emitter is never reached from
    // the middle of a method body.
    //
    // ⚠ THE PASS-ORDERING ARGUMENT IS NOT THE REASON, and must not be recorded
    // as one: `declareVtables()` (LlvmBackend_impl.hpp ~290) ITSELF lowers user
    // statement bodies — via the accessor calls at LlvmBackend_Setup.cpp
    // ~3699/~3706/~3826 and `ensureIndexer{Getter,Setter}Fn` at ~3824-3825 —
    // and it runs BEFORE `emitExportedPropertyGetters()` (~294),
    // `emitExportedPropertySetters()` (~298) and `setupBoxDescriptors()`
    // (~339). So "every call site precedes the body loop at ~346" does NOT
    // establish that no user code has been lowered yet.
    //
    // The real reason the two fields #2708 is about are clean at every entry is
    // that `currentReturnTypeNode_` and `currentLambdaReturnSemaTy_` are
    // BALANCED-WRITE at every writer — `emitFunctionBody`,
    // `buildSpecializedMethodFn` (Calls2.cpp ~8707 -> restored ~8957),
    // `genericInstSfinitFn` (Setup.cpp ~1104/~1110 -> ~1340), Setup.cpp
    // ~4670-4671 -> ~4676, and `lowerLambda` all save and restore. This emitter
    // pre-guard neither set nor saved them; it only READ them through the body
    // it lowered, so it could not leave them dirty either. Both start null at
    // the driver and every excursion returns them to null. Instrumenting the
    // entry on #2708's own interface-dispatch repro is consistent with that.
    //
    // (One wording caveat on that instrumentation: the entry state is not
    // "entirely default" from the SECOND sibling call onward — `:3911` restores
    // the builder insert point only under `if (savedBB)`, so on the first call
    // in a module nothing is restored. No live hazard: every consumer sets its
    // own insert point before emitting.)
    //
    // There is therefore no observable defect to regression-test, and none is
    // claimed.
    //
    // The guard is adopted anyway because it RESETS AT ENTRY rather than merely
    // restoring at exit, which is what makes the emitter robust independently of
    // both pass ordering and any future unbalanced writer: this emitter is
    // one moved call site away from the #2701/#2702 failure mode, and the whole
    // point of NestedBodyScope is that the next per-body field is handled in one
    // place. Today it is a no-op here.
    NestedBodyScope nestedScope(*this);

    currentFn_          = fn;
    builder_.SetInsertPoint(entryBB);
    auto argIt = fn->args().begin();
    currentThis_        = &*argIt;
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
                // fix #2761 (§8.6 "Boxing" / §23.3, v0.240): the VTABLE/ITABLE
                // face of an EXPRESSION-bodied getter, the sibling of #2703's
                // registration for the block face a few lines below. A
                // `public virtual property O: Object => 0;` emitted `ret i32 0`
                // from a `ptr`-returning slot function — "Function return type
                // does not match operand type of return inst!", a HARD build
                // failure on a shape §23.3 plainly admits (reproduced on
                // 0e554e3).
                // ⚠ MEASURED INERT TODAY, and shipped anyway: `boxValue`
                // resolves the box vtable through `primBoxVtable_`/`boxVtable_`,
                // which `setupBoxDescriptors()` fills — and that pass runs
                // AFTER `declareVtables()` (LlvmBackend_impl.hpp ~400), which is
                // what reaches this emitter. `boxVtableFor` therefore returns
                // null and `boxValue` returns `v` unchanged, so the virtual
                // spelling STILL fails to build. That is #2759 (the identical
                // gap #2703 hit on the block face), and it is not fixable by
                // reordering the two passes: `setupBoxDescriptors` is
                // documented at its call site as requiring the descArray_/
                // vtable globals `declareVtables` builds, so the dependency is
                // the wrong way round for a swap. Kept here so the arm is in
                // place — and so the fix is one pass-ordering change away —
                // rather than leaving the expression face the only one missing
                // it. A build failure is the SAFE residual: unlike the inlined
                // read this shape never silently produced a wrong value.
                v = boxForTarget(v, retTyRef, getterExpr);
                // #2491 (twin of #2434): the property getter body's return
                // expression failed to lower — this site bypasses emitReturn
                // (calls CreateRet directly), so the `v ? v : null` fallback
                // below is the getter's OWN silent zero, named explicitly in
                // #2491's evidence (#2474's investigation found a lazy
                // struct-build ordering bug compiled a getter to a literal
                // `ret 0` here with zero diagnostic). See the ReturnStmt site
                // in LlvmBackend_Stmts.cpp for the full rationale (Warning +
                // !hasErrors() guard). The zero/null ret still happens below.
                if (!v && !ctx_.diagnostics().hasErrors()) {
                    ctx_.diagnostics().report(
                        Severity::Warning, getterExpr->span.begin,
                        "internal: return-value expression failed to lower to a "
                        "value (degraded to a zero/null return value) — #2491");
                }
                builder_.CreateRet(v ? v
                                     : llvm::Constant::getNullValue(valTy));
            }
        } else if (blockGet) {
            // fix #2703 (§23.3 / §12 "Inheritance and Dispatch" vtable face,
            // v0.240): the VIRTUAL/interface face of the same block getter. This
            // emitter builds an independent function for the vtable/itable slot
            // and lowers the SAME user statement body, so it needs the same
            // representation node — otherwise a `property O: Object` block
            // getter read through an interface reference still emits `ret i32`
            // from a `ptr`-returning function even after the direct-read
            // emitters are fixed. Registered only on the GETTER face: a setter
            // returns void and has no return-representation contract.
            accessorReturnTypeNode_ = pd->type.get();
            accessorReturnTypeFn_   = fn;
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
                // fix #2490 (§23.3 "Block-form properties" / §28.9 "Runtime-
                // managed types", v0.236): pre-fix this was a bare,
                // unguarded pointer store — NO retain of the incoming value
                // and NO release of the value being displaced. §23.3 is
                // explicit that "the setter's implicit `value` parameter has
                // no modifier by default and takes a BORROW of the
                // property's declared type" (auto-properties have no syntax
                // slot for `set(owned)` — that spelling is admitted only on
                // block-form properties/indexers, §23.3 — so an auto-
                // property setter's `value` is ALWAYS a borrow, never an
                // announced transfer). A borrow means the CALLER keeps its
                // own reference and is not discharging it into this call;
                // if the callee (this setter) wants to keep the value alive
                // past the call — which storing it into a field that
                // outlives the call obviously does — it must mint its OWN
                // independent +1, exactly like an ordinary in-class field
                // store of a plain (non-modifier) parameter already does
                // (lowerStringFieldStore / lowerClosureFieldStore, #429 /
                // §4.6 / §27.5 / §28.9 — "field store of a `string` param
                // routes through lowerStringFieldStore's emitStringRetain").
                // Confirmed live consequence of the bare store: the field
                // and the caller's local silently aliased the SAME single
                // reference, so the owning object's disposal (which
                // correctly releases the field once, per
                // emitOwnedFieldDisposals) freed the string out from under
                // the caller's still-live local — SEGV in
                // bz_string_to_utf8z (xmod_auto_property_setter_arc_2490_
                // test.cpp T1, fail-red/pass-green demonstrated).
                //
                // Order mirrors lowerStringFieldStore/lowerClosureFieldStore
                // exactly: load-old -> retain-new -> store-new -> release-
                // old. Retaining before releasing keeps a self-assignment
                // (`h.Name := h.Name;`, which calls this SAME generated
                // function) transiently safe — the count dips +1 then back,
                // never 0 — instead of releasing first and risking a
                // display of the same object at rc 0.
                //
                // There is no source ast::Expr for the incoming value here
                // (it is an opaque function parameter, not a spelled-out
                // assignment RHS), so unlike lowerStringFieldStore's
                // isPlusOneStringProducer check, there is no way to prove
                // the value is a fresh, uniquely-owned +1 the callee could
                // transfer instead of retain — and §23.3 says it never is,
                // for a plain setter. Always retain; never transfer.
                //
                // CLASS ONLY, matching isStringFieldLvalue/
                // isClosureFieldLvalue's own gate: a RECORD's auto-property
                // backing field is a borrow with no independent +1 and no
                // disposer (records are legal with mutable auto-properties
                // per §9.2, but "the record does not own what \[reference
                // fields\] point to", §9; §9.7 — no disposer to balance a
                // retain against). Falls through to the pre-fix plain store
                // for a record (or any non-class `cls`), unaffected.
                //
                // Object/interface (box)-typed auto-properties ARE covered
                // here as of fix #2500 (§23.3/§28.9, v0.236): the box twin of
                // the string/closure retain/release just below. This was the
                // hazard #2500 was filed on — isBoxStorageMember's
                // classification prepass (collectBoxStorageFields,
                // LlvmBackend_Setup.cpp:18116) used to run AFTER declareVtables
                // ()/emitExportedPropertySetters() (this function's two
                // earliest, memoizing callers), so boxStorageFields_ read empty
                // at the point this body needed it. Fix #2500 REORDERED
                // collectBoxReturnMethods()/collectBoxStorageFields() to run
                // BEFORE declareVtables() (LlvmBackend_impl.hpp, after
                // computeEnumValues), so isBoxStorageMember now returns the
                // correct classification here. The retain is unconditional
                // (never a transfer) for the same §23.3 borrow reason as
                // string/closure; bz_box_retain/release are null-safe, so a
                // first-time set into a calloc-zeroed (null) backing field
                // releases null harmlessly. CLASS ONLY — a RECORD's
                // auto-property backing field is a borrow with no independent
                // +1 and no disposer (§9/§9.7); isBoxStorageMember is itself
                // class-gated, so boxField is false for a record regardless.
                bool classField = cls && cls->kind == ast::TypeDeclKind::Class;
                bool strField = classField && isStringStorageMember(ps) &&
                                valArg.getType()->isPointerTy();
                bool closureField = classField && !strField &&
                                    isClosureStorageMember(ps) &&
                                    valArg.getType()->isPointerTy();
                // fix #2500 (§23.3/§28.9): box-typed (Object/interface) CLASS
                // auto-property backing field — the box twin of strField/
                // closureField above. isBoxStorageMember (class-gated, now
                // populated before this body is built — see the comment above)
                // confirms the field ONLY ever holds boxes, so bz_box_retain/
                // release are type-correct here (never called on a class-ref
                // pointer, which lacks a box header).
                bool boxField = classField && !strField && !closureField &&
                                isBoxStorageMember(ps) &&
                                valArg.getType()->isPointerTy();
                if (strField) {
                    llvm::Value* oldVal = builder_.CreateLoad(
                        ptrTy(), slot, "propset.str.old");
                    emitStringRetain(&valArg);
                    builder_.CreateStore(&valArg, slot);
                    emitStringRelease(oldVal);
                } else if (closureField) {
                    llvm::Value* oldVal = builder_.CreateLoad(
                        ptrTy(), slot, "propset.clo.old");
                    emitClosureRetain(&valArg);
                    builder_.CreateStore(&valArg, slot);
                    emitClosureRelease(oldVal);
                } else if (boxField) {
                    // §23.3/§28.9: load-old -> retain-new -> store-new ->
                    // release-old, mirroring the string/closure order (retain
                    // before release keeps a self-assignment `h.V := h.V;`,
                    // which calls this same function, transiently safe).
                    llvm::Value* oldVal = builder_.CreateLoad(
                        ptrTy(), slot, "propset.box.old");
                    emitBoxRetain(&valArg);
                    builder_.CreateStore(&valArg, slot);
                    emitBoxRelease(oldVal);
                } else {
                    builder_.CreateStore(&valArg, slot);
                }
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
    finallyStack_       = std::move(savedFinally);      // #2281 / §31.2
    finallyBodyStack_   = std::move(savedFinBodies);    // #2281 / §29.7
    unwindTarget_       = savedUnwind;
    propagateBB_        = savedProp;
    currentCaughtExc_   = savedCaught;
    unwindScopeDepth_   = savedUD;
    diverged_           = savedDiv;
    genericSubst_       = std::move(savedGSubst);
    // smKind_/smStateStruct_ (#2170) are restored by ~NestedBodyScope, which
    // runs after this block. currentEnumMethod_ stays this function's own (it is
    // SET to `cls` for an enum above, #574) — the guard deliberately does not
    // touch it; see the guard's definition for why.
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
    // fix #2694 / §23.4 / §28.9 (v0.240): the same memoized-getter-inherits-the-
    // caller's-return-contract hole as ensurePropertyBlockGetter — see the long
    // note there for the reasoning, the +0 call-site contract (the read path at
    // ~line 1598 above hands this call's result straight back as a borrow), and
    // why NULL rather than the indexer's own return node is the right value.
    //
    // fix #2701 (§23.4 "Indexers" / §30.5, v0.240): this emitter is the INDEXER
    // spelling of ensurePropertyBlockGetter and lowers a user statement body the
    // same way, but #2170's `smKind_`/`smStateStruct_` save was never mirrored
    // here — so reading a block-bodied INDEXER from an `async` body reproduced
    // #2170's exact symptom (the getter's `return` took the async-return path,
    // emitting bz_task_complete / spill-free / frame-free into the getter, whose
    // own `ret i32` was then never emitted). Adopting NestedBodyScope closes
    // that together with #2702's residual set — which matters here in
    // particular, because #2170's reset value `smKind_ = Yield` is itself what
    // OPENS the `smStateGEP_ && (Yield || AsyncYield)` gate in lowerTry: fixing
    // the state-machine pair WITHOUT nulling `smStateGEP_` would have moved this
    // failure rather than removed it.
    NestedBodyScope nestedScope(*this);

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
    // fix #2703 (§23.4 "Indexers", v0.240): the INDEXER spelling of the
    // representation-node registration in ensurePropertyBlockGetter — see the
    // long note there for the four arms this re-enables, the evidence for each,
    // and why the +1-mint arms (#2694 / axiom A4) stay off. `pd->type` is the
    // same node this emitter already built the signature's `retTy` from, so the
    // body is now represented against exactly the type it must `ret`.
    accessorReturnTypeNode_ = pd->type.get();
    accessorReturnTypeFn_   = fn;

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
    // The #2694 return-contract pair and the #2170/#2701 state-machine fields
    // are restored by ~NestedBodyScope, which runs after this block.
    if (savedBB) builder_.SetInsertPoint(savedBB, savedBBOffset);

    return fn;
}


// #730/#2437/#2442 / §13 / §11.2 / §23.4: the class `cls`'s (or an inherited
// base's) concrete indexer implementing the interface indexer `ifaceIdx`.
// Indexers are anonymous (all named ".this"), so classPropertyByName can't
// be used; match by FULL parameter list (arity + every parameter type) via
// findIndexer, which also walks the base-class chain — see its own comment
// for the two defects (#2437 arity-blindness, #2442 no base walk) this
// fixes.
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
    // fix #2761 (§8.6 "Boxing" / §23.4 "Indexers", v0.240): the ITABLE face of
    // an expression-bodied INDEXER getter — the indexer spelling of the
    // property arm in ensurePropertyAccessorFn above. Subject to the SAME #2759
    // inertness on this face (this emitter is reached from declareVtables,
    // before setupBoxDescriptors); see the note there.
    if (pd->type)
        result = boxForTarget(result, ctx_.resolvedType(*pd->type), getterExpr);
    // fix #2850 (§7.2 "Nullable value types" / §23.4 "Indexers", v0.240): the
    // STANDALONE-FUNCTION face of the same missing coercion the inline path in
    // lowerIndexerGet carries — this emitter fills the vtable slot for a
    // `virtual` indexer and the itable slot for an interface-typed one, and its
    // signature already returns `lowerType(pd->type)` (the tagged
    // `{i8 hasValue, T}` optional). Returning the raw body value there is not a
    // PHI assert but MALFORMED IR (`ret ptr null` from a `{i8, i32}` function),
    // caught only by the module verifier. `coerceNumericToType` below cannot
    // close it: it is a no-op on a pointer→struct or scalar→struct pair.
    if (pd->type)
        result = wrapNullableGetterResult(ctx_.resolvedType(*pd->type), result);
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


// fix #2449 (§11.2/§23.4, v0.235): the itable-slot accessor function for
// concrete indexer `iimpl` (`classIndexerImplementing`'s result), covering
// BOTH the shapes that reach here now that #2442 walks the base-class
// chain: a LOCAL indexer (own or inherited from a LOCAL base) builds its
// body from the AST via ensureIndexerGetterFn/-SetterFn, exactly as before
// #2449; an IMPORTED indexer (inherited from an imported base class, no
// `ast::PropertyDecl` to build a body from) reuses the already-declared
// cross-module ABI stub #2414 installed in importedIndexerGetterFns_/
// -SetterFns_ (`declareImportedIndexerGetters`/`-Setters`, called earlier
// in `declareVtables`, before Pass D — the only caller of this function).
// `iimpl` reaching here is always `indexerCandidateWireable` per
// `findIndexer`'s own gate, so the imported-map lookup should always hit;
// a miss (defensive) falls through to null, installing a null itable slot
// — sound (matches #2438's existing "missing implementation" contract),
// not silently wrong.
llvm::Function* Emitter::indexerItableAccessor(const sema::TypeSymbol* ts,
                                               const sema::PropertySymbol* iimpl,
                                               bool isSet) {
    if (!iimpl) return nullptr;
    if (dynamic_cast<const ast::PropertyDecl*>(iimpl->decl))
        return isSet ? ensureIndexerSetterFn(ts, iimpl)
                     : ensureIndexerGetterFn(ts, iimpl);
    auto& map = isSet ? importedIndexerSetterFns_ : importedIndexerGetterFns_;
    auto it = map.find(iimpl);
    return it != map.end() ? it->second : nullptr;
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
    // fix #2568: thread `value` through as valueExpr so lowerPropertySetValue
    // can release a +1 producer passed directly to a CLASS-owned computed
    // setter (see the comment on the lambda above). The `ref`/`out` write-
    // back call site that uses lowerPropertySetValue with a pre-loaded `v`
    // (no source expression in scope) is the emitAccessorRefArgWriteBacks
    // call, which deliberately passes nullptr — leak-over-UAF is the safe
    // direction there, identical to #2540's nullptr-NAMED-LOCAL choice for
    // lowerIndexerSet.
    return v ? lowerPropertySetValue(target, v, &value) : false;
}


bool Emitter::lowerPropertySetValue(const ast::Expr& target, llvm::Value* v,
                                    const ast::Expr* valueExpr) {
    const ast::PropertyAccessor* setter = computedSetterOf(target);
    if (!setter || !v) return false;
    auto* ps = dynamic_cast<sema::PropertySymbol*>(ctx_.exprSymbol(target));
    auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl);
    // fix #2568 (§23.3/§28.9/§4.6, v0.236): the LOCAL non-virtual computed-
    // property setter path's missing producer release. The virtual/interface
    // analogue `lowerVirtualPropertySet` (LlvmBackend_Exprs.cpp ~5106-5128) has
    // emitted producer release since #2490/#2500/#2506; the indexer analogue
    // `lowerIndexerSet` got the same lambda in #2548/#2567. This function (the
    // block-setter / static-setter / compound-property / increment-property
    // path) was the lone holdout: a +1 PRODUCER RHS (`"abc" + i.ToString()`,
    // string interpolation, a closure-return method call, a box-return call to
    // an Object/interface property target) passed directly to a CLASS-OWNED
    // computed setter's inlined body is retained (the setter body stores it
    // into the backing field, which routes through `lowerStringFieldStore`'s
    // ARC — emitStringRetain/emitClosureRetain/emitBoxRetain), but no caller-
    // side release fired to balance the retain, so the producer's +1 leaked
    // out from under the caller's own reference. LSan-confirmed: 100
    // iterations of `s.Name := "abc" + i.ToString();` against a non-virtual
    // class computed property `set => _name := value;` leaked 3200 byte(s)
    // direct + 590 byte(s) indirect (100 bz_string headers + 100 payload
    // substrings), i.e. 32 + 5.9 ≈ 37 byte(s) per iteration, exactly the
    // per-iteration pattern the issue brief observed. Pre-fix, every loop
    // iteration leaked. Fix mirrors lowerIndexerSet's inline-body
    // emitProducerRelease (`recvDecl->kind == Class` gate, applied below
    // after the setter body) exactly: gated Class-only because a RECORD's
    // setter body keeps the PLAIN STORE (§9/§9.7 — a record's reference
    // fields are borrows with no independent +1 and no disposer), so the
    // callee never retained and releasing here would drop the field's only
    // reference (UAF, the live failure #2490/#2506/#2548 caught for the
    // sibling sites).
    //
    // The lambda below is the single emitProducerRelease used by both the
    // static and instance branches. Same predicates as
    // lowerVirtualPropertySet's / lowerIndexerSet's / lowerIndexerCompound's
    // / lowerImportedPropertySetValue's: isPlusOneStringProducer /
    // isPlusOneClosureProducer / isUserMethodClosureReturnCall /
    // isUserMethodBoxReturnCall, plus the §8.6 box target check.
    auto emitProducerRelease = [&] {
        if (valueExpr && isPlusOneStringProducer(valueExpr))
            emitStringRelease(v);
        else if (valueExpr && (isPlusOneClosureProducer(valueExpr) ||
                               isUserMethodClosureReturnCall(valueExpr)))
            emitClosureRelease(v);
        else if (valueExpr && v && v->getType()->isPointerTy()) {
            // fix #2500 (§23.3/§28.9, v0.236): the box twin of the string/
            // closure release above — same isProducer predicate as
            // lowerBoxFieldStore / lowerImportedPropertySetValue. Gated on the
            // TARGET being Object/interface (and v a ptr): a value-typed
            // property's source may be boxable in isolation but is not boxed
            // here.
            const sema::TypeRef* tgtTy = ctx_.exprType(target);
            if (tgtTy && isObjectOrInterfaceTarget(tgtTy)) {
                const sema::TypeRef* srcTy = ctx_.exprType(*valueExpr);
                if ((srcTy && (isBoxableValueType(srcTy) ||
                               isNullableValueTy(srcTy))) ||
                    isUserMethodBoxReturnCall(valueExpr))
                    emitBoxRelease(v);
            }
        }
    };
    // CLASS-OWNED PROPERTY gate (§9/§9.7): only release the producer on the
    // CLASS path. Records' setter bodies plain-store, so the callee never
    // retained and dropping the producer here would UAF the field's only ref.
    // The static branch below has `ps->parent` set to the declaring class (a
    // STATIC property's `parent` is its declaring type), so the same gate
    // covers it. ENUM properties are blocked by `computedSetterOf` for
    // non-block setters (#2499) and are not an active path here.
    bool classOwned = ps->parent &&
                      ps->parent->kind == ast::TypeDeclKind::Class;
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
        // fix #2712 / §23.3 / §28.9 (v0.240): the STATIC-property twin of the
        // inlined-indexer-setter retain — found by sweeping the shape rather
        // than listed on the issue, and reproduced mechanically (`value := "zz"`
        // inside `static property P: string`'s block setter freed the caller's
        // string: `heap-use-after-free`, frames `bz_string_release <- main`).
        // `ps->indexerParams` is empty for a named property, so this call is
        // about `value` alone. See lowerIndexerSet for the full argument.
        retainAssignedStringParams(setter->stmts, ps->indexerParams, pd);
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
        // fix #2568: release the producer's +1 the static setter's body
        // doubled via its field store. A static property's `ps->parent` is its
        // declaring type (a class for `static` properties on a class), so
        // classOwned already disambiguates class-vs-record for the static
        // path. Records cannot have static properties per §10.5 anyway, so
        // the gate is conservative-on-leak, correct-on-UAF.
        if (classOwned) emitProducerRelease();
        return true;
    }
    // fix #2498 (§9.2/§9.4/§23, v0.236): mirrors lowerImportedPropertySetValue's
    // #2489 fix a few dozen lines above — this LOCAL (non-imported) sibling had
    // the identical missing-spill defect for a RECORD receiver, plus the same
    // "spilled copy, not original storage" trap once a naive spill is added.
    // Before this fix, `recv = lowerExpr(*ma->target)` on a record-typed
    // receiver yielded a by-value struct; binding that struct value directly
    // as `currentThis_` made the inlined setter body's implicit field write
    // (`_n := value;`, resolved via lvalueAddr/implicitFieldAddr, which always
    // expects a pointer `this`) emit a StructGEP against a non-pointer base —
    // malformed IR, confirmed by probe against this exact repro (#2498's own):
    // `r.Name := 55; return r.x + r.Name;` failed IR verification with "GEP
    // base pointer is not a vector or a vector of pointers" / "Store operand
    // must be a pointer", not a clean compile. A bare `!isPointerTy()` spill
    // (the naive fix) would also be unsound here for the same reason #2489's
    // comment documents: #2499's pre-existing sema gap can let an ENUM
    // property reach a computed setter, and an enum receiver's scalar `i32`
    // must NOT be spilled/treated as `this` (it is used as a plain integer,
    // not a struct pointer) — guard on `isAggregateType() && !isEnumReceiver`,
    // never a bare `!isPointerTy()`. And spilling a LOADED COPY (rather than
    // resolving the receiver's own storage address via `lvalueAddr`) would
    // reproduce #2502's silent-no-op defect: the write would land in a
    // throwaway alloca instead of the caller's actual variable. Only the
    // `ma->target` (explicit-receiver, e.g. `r.Name := 55`) branch needs this
    // — the implicit-`this` default below is already a pointer (methods bind
    // `currentThis_` to a pointer argument regardless of record/class kind,
    // LlvmBackend_Setup.cpp ~L7576), so no spill/address resolution applies
    // there.
    llvm::Value* recv = currentThis_;
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&target)) {
        if (!ma->target) {
            recv = currentThis_;
        } else {
            bool isEnumReceiver = ps->parent &&
                                  ps->parent->kind == ast::TypeDeclKind::Enum;
            recv = nullptr;
            llvm::Type* recvElemTy = nullptr;
            if (llvm::Value* addr = lvalueAddr(*ma->target, recvElemTy)) {
                if (recvElemTy && recvElemTy->isPointerTy())
                    recv = builder_.CreateLoad(recvElemTy, addr);
                else if (recvElemTy && recvElemTy->isAggregateType() &&
                         !isEnumReceiver)
                    recv = addr;  // already the receiver's own storage address
            }
            if (!recv) {
                llvm::Value* rv = lowerExpr(*ma->target);
                if (!rv) return false;
                if (rv->getType()->isPointerTy()) {
                    recv = rv;
                } else if (rv->getType()->isAggregateType() && !isEnumReceiver) {
                    llvm::AllocaInst* tmp = entryAlloca(rv->getType());
                    builder_.CreateStore(rv, tmp);
                    recv = tmp;
                } else {
                    return false;
                }
            }
        }
    }
    if (!recv) return false;
    // Bind `value` — sema anchored its symbol's decl on the PropertyDecl.
    llvm::AllocaInst* slot = entryAlloca(v->getType());
    builder_.CreateStore(v, slot);
    locals_[pd] = slot;
    // fix #2712 / §23.3 / §28.9 (v0.240): the INSTANCE-property twin of the
    // static branch's retain immediately above — same inlined body, same
    // unpaired drop-on-overwrite release, reproduced the same way. See
    // lowerIndexerSet for the full argument.
    retainAssignedStringParams(setter->stmts, ps->indexerParams, pd);
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
    // fix #2568 (§23.3/§28.9/§4.6, v0.236): the INSTANCE-PATH twin of the
    // static-path emitProducerRelease above. Same gate (classOwned) for the
    // same record-UAF reason. See the long comment above the static branch
    // for the full rationale and LSan reproduction.
    if (classOwned) emitProducerRelease();
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
//
// fix #2489 (§9.2/§9.4/§23, v0.236): this function had NO by-value-receiver
// handling at all — `!recv->getType()->isPointerTy()` unconditionally
// rejected the call, exactly #2474's pre-fix property-GETTER shape
// (LlvmBackend_Exprs.cpp's "imported non-virtual property getter" site).
// A RECORD receiver lowers to a by-value struct (issue #347/§9.2), so a
// settable computed property on an imported record always fell through this
// guard — confirmed by probe: `r.Name := 55; return r.x + r.Name;` against
// an imported record returned 9 (the untouched original `_n = 1`, i.e.
// 9 + 1), not 64, proving the write silently dropped rather than crashing
// or link-failing (this guard, not emitExportedPropertySetters' own
// pre-#2489 Class-only gate, is what kept it link-safe pre-fix: the call
// was never even emitted). Spill the receiver exactly as #2474's layer 2
// does for the getter, guarded to `isAggregateType() && !isEnumReceiver` —
// NOT a bare `!isPointerTy()`. #2474's own review caught that the bare
// form is unsound here: declareImportedPropertySetters (no kind filter
// beyond virtual/interface + ctx_.importedPropertyHasSetter) can — because
// of a separate, pre-existing sema gap (#2499: an enum block-form settable
// property is wrongly accepted, in violation of §10.5) — declare a
// `$propset` reference for an ENUM property today; emitExportedPropertySetters
// deliberately stays `Class || Record`, never `Enum`, so an unconditional
// spill of an enum's scalar `i32` receiver into a pointer would fire a call
// against a symbol nothing defines, turning a (silently-wrong-but-legal)
// compiling-and-linking program into a hard `undefined reference` link
// error. Confirmed mechanically: #2499's own repro shape, promoted to a
// direct cross-module SET, links successfully with this guard in place.
//
// A second, independent defect surfaced while testing the spill above: a
// naive `lowerExpr(*ma->target)` + spill-to-a-fresh-alloca (the pattern
// every SIBLING by-value-receiver site in this codebase uses for READS,
// #2474/#2489's own indexer-getter fix included) is WRONG for a SETTER
// specifically. `lowerExpr` on a plain identifier referencing a
// record-typed local LOADS the record's current value — a byte-for-byte
// COPY, not the variable's own storage. Spilling that copy into a NEW
// alloca and writing through it (via the exported setter stub) mutates
// only the throwaway copy; the caller's own variable is never touched, so
// `r.Name := 55; return r.x + r.Name;` (T1's own repro) evaluated as `9 +
// 1` (the untouched original), not `9 + 55` — confirmed mechanically
// before this second fix landed. (The identical defect shape exists in the
// pre-existing, same-module `lowerIndexerSet` inline path — filed
// separately as #2502, out of #2489's cross-module-ABI scope; this
// function's OWN fix, being new code written for #2489, does not inherit
// it.) The fix: prefer `lvalueAddr` (which resolves a plain local/field to
// its OWN storage address via `localSlot`/`fieldAddr`) over `lowerExpr`
// for the receiver — for a pointer-shaped (class) slot, load the pointer
// value from that address (identical to what `lowerExpr` would have
// yielded, since a class reference is a value that may be freely copied);
// for an aggregate-shaped (record) slot, the address itself IS the correct
// `this` — no spill needed, because it already addresses the real
// storage. Only when the receiver expression is NOT an addressable place
// (e.g. a fresh temporary — `MakeRecord().Name := 55`, where no "original"
// exists to preserve) does this fall back to `lowerExpr` + spill, exactly
// as before.
bool Emitter::lowerImportedPropertySetValue(const ast::Expr& target,
                                            llvm::Value* v,
                                            const ast::Expr* valueExpr) {
    if (!v) return false;
    auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&target);
    if (!ma || !ma->target) return false;
    auto* ps = dynamic_cast<sema::PropertySymbol*>(ctx_.exprSymbol(*ma));
    if (!ps || ps->isIndexer) return false;
    auto sit = importedPropSetterFns_.find(ps);
    if (sit == importedPropSetterFns_.end()) return false;
    bool isEnumReceiver = ps->parent &&
                          ps->parent->kind == ast::TypeDeclKind::Enum;
    llvm::Value* recv = nullptr;
    llvm::Type* recvElemTy = nullptr;
    if (llvm::Value* addr = lvalueAddr(*ma->target, recvElemTy)) {
        if (recvElemTy && recvElemTy->isPointerTy())
            recv = builder_.CreateLoad(recvElemTy, addr);
        else if (recvElemTy && recvElemTy->isAggregateType() &&
                 !isEnumReceiver)
            recv = addr;  // already the address of the receiver's real storage
    }
    if (!recv) {
        llvm::Value* rv = lowerExpr(*ma->target);
        if (!rv) return false;
        if (rv->getType()->isPointerTy()) {
            recv = rv;
        } else if (rv->getType()->isAggregateType() && !isEnumReceiver) {
            llvm::AllocaInst* tmp = entryAlloca(rv->getType());
            builder_.CreateStore(rv, tmp);
            recv = tmp;
        } else {
            return false;
        }
    }
    llvm::Function* fn = sit->second;
    llvm::Type* valTy = fn->getFunctionType()->getParamType(1);
    v = coerceNumericToType(v, valTy, isUnsignedTargetExpr(target));
    // fix #2500 (§8.6/§23.3/§28.9, v0.236): for an Object/interface property
    // the setter's `value` parameter is `ptr` (a box pointer), but the RHS
    // arrived here as the bare lowered value (e.g. an i32 literal) — without
    // boxing, the call passes i32 where ptr is required and IR verification
    // rejects the module. This is the cross-module-setter twin of the §8.6
    // boxing the direct-assignment path does (LlvmBackend_Stmts.cpp, just
    // before the field-store helpers); boxForTarget is a no-op for a reference
    // source (a class instance assigned to Object stays a bare ptr) and for a
    // null valueExpr (the ref/out write-back caller passes no RHS expr), so
    // only a boxable VALUE source is wrapped.
    v = boxForTarget(v, ctx_.exprType(target), valueExpr);
    builder_.CreateCall(fn->getFunctionType(), fn, {recv, v});
    emitPendingCheck();
    // fix #2490 (§23.3/§4.6/§28.9, v0.236): the ARGUMENT-PASS release, the
    // property-setter-call twin of the ordinary method-call release in
    // LlvmBackend_Calls2.cpp (~7035, the "#429 ARGUMENT-PASS category"
    // comment there has the full rationale). The generated setter this call
    // just invoked RETAINS its incoming `value` (ensurePropertyAccessorFn's
    // #2490 fix — §23.3 makes every plain setter parameter a borrow, so the
    // callee never transfers, always mints its own +1) — but ONLY for a
    // CLASS-owned property (ensurePropertyAccessorFn's ARC is deliberately
    // gated to `cls->kind == Class`; a RECORD's auto-property backing field
    // is a borrow with no independent +1, §9/§9.7, and keeps the pre-fix
    // plain store). Gating this release the SAME way is required, not
    // optional: releasing a producer the record-owning callee never retained
    // would drop the field's ONLY reference immediately — a confirmed live
    // UAF found by this fix's own adversarial pass (`r.Name := "a" + "b";`
    // on a record-typed imported property crashed in bz_string_to_utf8z when
    // this release fired unconditionally). A +1 producer passed DIRECTLY as
    // a CLASS setter's value — `h.Name := "a" + "b";`, no local binding of
    // its own — holds an independent, now-doubled reference nothing else
    // will ever release: this call's own `v` and the callee's fresh retain.
    // A bare `v` (an identifier / field / any non-producer borrow) is
    // unaffected either way, since it is that binding's own responsibility
    // to release at its own scope end, same as every other #429
    // ARGUMENT-PASS site.
    bool classOwned = ps->parent && ps->parent->kind == ast::TypeDeclKind::Class;
    if (classOwned && valueExpr && isPlusOneStringProducer(valueExpr))
        emitStringRelease(v);
    else if (classOwned && valueExpr &&
             (isPlusOneClosureProducer(valueExpr) ||
              isUserMethodClosureReturnCall(valueExpr)))
        emitClosureRelease(v);
    else if (classOwned && valueExpr && v->getType()->isPointerTy()) {
        // fix #2500 (§23.3/§28.9, v0.236): the box twin of the string/closure
        // ARGUMENT-PASS release above. The class-owning setter this call just
        // invoked RETAINS its incoming `value` (ensurePropertyAccessorFn's
        // #2500 box branch), so a +1 box PRODUCER passed directly as the
        // setter argument now holds an independent, doubled reference nothing
        // else will release: this call's own `v` (a fresh box boxForTarget
        // just minted for a boxable VALUE source) or a box-return method
        // call's callee-produced box. Release it here, mirroring
        // lowerBoxFieldStore's producer-transfer counterpart exactly (same
        // isProducer predicate). The TARGET must be Object/interface (and v a
        // ptr) — a VALUE-typed property's source may be "boxable" in isolation
        // (an int literal) but is NOT boxed at this call site, so there is no
        // box to release and bz_box_release must not be called on the raw i32.
        // A borrow (a named local / field holding a box whose +1 belongs to
        // its own slot) is unaffected — its own scope-end release still
        // applies. Gated classOwned for the same reason as the string/closure
        // branches: a RECORD's auto-property setter keeps the pre-fix plain
        // store (#9/§9.7), so it never retained, and releasing here would
        // drop the field's only reference.
        const sema::TypeRef* tgtTy = ctx_.exprType(target);
        if (tgtTy && isObjectOrInterfaceTarget(tgtTy)) {
            const sema::TypeRef* srcTy = ctx_.exprType(*valueExpr);
            if ((srcTy && (isBoxableValueType(srcTy) ||
                           isNullableValueTy(srcTy))) ||
                isUserMethodBoxReturnCall(valueExpr))
                emitBoxRelease(v);
        }
    }
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
    // fix #2490: thread `value` through as valueExpr so
    // lowerImportedPropertySetValue can release a +1 producer passed
    // directly (see its own comment for the full rationale).
    return v ? lowerImportedPropertySetValue(target, v, &value) : false;
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
    // fix #2576 (§23.3/§28.9/§4.6, v0.236): the COMPOUND-PROPERTY twin of
    // lowerIndexerCompound's #2567 fix (this file, line ~2531-2542), mirrored
    // EXACTLY. `lowerPropertyCompound` desugars `s.Name OP= rhs` to
    // `s.Name := s.Name OP rhs`:
    //   * line 4349: `rhs = lowerExpr(*as.value)` — a +1 string when the AST
    //     is a producer (e.g. BinaryExpr `+` `"abc" + i.ToString()`), a BORROW
    //     when the AST is a named local (`s.Name += t`).
    //   * line 4353: `res = emitCompoundOp(op, cur, rhs, ...)` — for a string
    //     target emitCompoundOp dispatches to emitStringConcat (line ~204),
    //     which ALWAYS mints a fresh rc=1 string (per #422), memcpy'ing both
    //     operands' bytes into a distinct header. So `res` is +1 regardless of
    //     whether `rhs` is a producer or a borrow.
    //   * lowerPropertySetValue inlines the setter body, which stores `res`
    //     into the backing field via lowerStringFieldStore's ARC — retain(res)
    //     doubles res's +1 for a CLASS-owned property (§9/§23.3).
    //
    // TWO +1 producers can escape here, and #2568's valueExpr-based release
    // (threading `as.value` so emitProducerRelease fires inside
    // lowerPropertySetValue) closes only HALF the gap:
    //   (1) `res`'s +1 — #2568 releases it ONLY when `isPlusOneStringProducer
    //       (as.value)` is true, i.e. only for a PRODUCER rhs. For a BORROW rhs
    //       (`s.Name += local`), as.value is a named IdentifierExpr (not a
    //       producer), so #2568's release does NOT fire, yet res is still +1
    //       from emitStringConcat — so res's doubled +1 leaks (LSan-confirmed:
    //       100 iterations of `s.Name += local` leak 3200 byte(s) direct +
    //       15250 byte(s) indirect, the accumulated field string never freed
    //       because each displaced value's refcount was 2 not 1). This is the
    //       compound-path gap #2567 closed for indexers by releasing `newVal`
    //       UNCONDITIONALLY on (classOwned && pointer), NOT gated on the rhs
    //       predicate.
    //   (2) `rhs`'s +1 — a producer rhs (`"abc" + i.ToString()`) is a SECOND
    //       orphan +1: emitStringConcat memcpy's rhs's bytes into res and
    //       leaves rhs's own +1 untouched (LSan-confirmed: 37 byte(s)/iter on
    //       top of the res leak — 3200 direct + 590 indirect for the producer
    //       shape when the res release is also missing).
    //
    // The fix mirrors lowerIndexerCompound's #2567 structure EXACTLY: pass
    // `nullptr` as valueExpr so #2568's narrower producer-only release does
    // NOT fire inside lowerPropertySetValue (avoiding a double-release of res
    // for the producer-rhs case), then release BOTH producers here after the
    // setter body returns:
    //   * `res` — released when classOwned && pointer (the emitStringConcat
    //     +1; unconditional for any string compound, producer OR borrow rhs,
    //     because emitStringConcat always mints +1).
    //   * `rhs` — released when classOwned && pointer && isPlusOneString
    //     Producer(as.value) (only a producer rhs carries its own +1; a borrow
    //     rhs's +1 belongs to its own slot and must not be released).
    // CLASS-only gate (§9/§9.7): a record's setter body plain-stores `res`,
    // so res's +1 is the field's own reference — releasing would UAF the
    // field (the live failure #2490/#2506/#2548 caught for sibling sites).
    // The record-side compound leak is the §9/§9.7 auto-property gap tracked
    // by #2577; do NOT widen this gate to records. res and rhs are DISTINCT
    // allocations (emitStringConcat allocates a fresh header), so releasing
    // rhs cannot UAF res or the field.
    if (!res) return false;
    // nullptr valueExpr: see the long comment above — #2576 handles res's
    // release itself (mirroring #2567) rather than relying on #2568's
    // narrower producer-only valueExpr release inside lowerPropertySetValue.
    bool handled = lowerPropertySetValue(*as.target, res, nullptr);
    auto* ps = dynamic_cast<sema::PropertySymbol*>(ctx_.exprSymbol(*as.target));
    bool classOwned = ps && ps->parent &&
                      ps->parent->kind == ast::TypeDeclKind::Class;
    if (classOwned) {
        if (res && res->getType()->isPointerTy())
            emitStringRelease(res);  // release the OP-result's +1 (emitStringConcat)
        // For string `+=`, rhs is `lowerExpr(*as.value)` — a +1 string when
        // the AST is a producer (e.g. BinaryExpr("+", ...)) but a BORROW when
        // the AST is a named local. Check the AST shape, not the LLVM pointer
        // type: only release when rhs is itself a +1 producer. Mirrors
        // lowerIndexerCompound line ~2539.
        if (rhs && rhs->getType()->isPointerTy() && as.value.get() &&
            isPlusOneStringProducer(as.value.get()))
            emitStringRelease(rhs);  // release the rhs's orphan +1 (producer)
    }
    return handled;
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
        bool* outWrapMade, bool srcOwnedByCallee) {
    if (outWrapMade) *outWrapMade = false;
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
        if (outWrapMade) *outWrapMade = true;
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
        // needs the per-element re-wrapping nested enumerator, not the plain
        // one. #2487 (stdlib §4.11 "Iteration order" v0.195; language spec
        // §29.1, v0.236): the nested factory (ensureIEnumViewFactory /
        // wrapNestedArrayAsIEnumerable) is now Stack-aware in BOTH directions
        // — a Stack<T> OUTER container traverses pop order, and a Stack<T>
        // ELEMENT's leaf wrap reuses #2471's reverse machinery — so this
        // dispatch is correct as-is for the nested shape too.
        if (nestedIEnumViewNeeded(actualTy, targetTy)) {
            if (outWrapMade) *outWrapMade = true;
            // fix #2299 / §28.9: callee-owned source → use the OWNING
            // nested-array enumerator (its slot-1 Dispose frees BOTH the
            // wrapper frame and the backing array exactly once). For
            // borrowed source, keep the plain wrap.
            if (srcOwnedByCallee) {
                // The nested wrap currently doesn't have an owning variant;
                // free the source immediately and return the plain wrap. The
                // caller will dispose the wrapper frame (free(self)).
                llvm::Value* wrap =
                    wrapNestedArrayAsIEnumerable(v, actualTy, targetTy);
                freeCollectionSourceNow(v);
                return wrap;
            }
            return wrapNestedArrayAsIEnumerable(v, actualTy, targetTy);
        }
        // #2486 (stdlib §4.11 "Iteration order" v0.195; language spec §29.1,
        // agreeing with #2472's for-loop fix and #2471's explicit
        // .GetEnumerator() fix): a Stack<T>/OwningStack<T> widened to a pure
        // IEnumerable<T> at an argument, local-decl-init, or method-RETURN
        // coercion — all three positions share this one dispatch point —
        // must iterate top-to-bottom (pop order), not physical/forward
        // order. Reuse #2471's own reverse machinery (wrapStackAsIEnumerator
        // / ensureStackEnumMachinery, LlvmBackend_Assign.cpp) verbatim
        // rather than minting a third implementation, so this path's element
        // order is structurally the SAME code as the explicit-GetEnumerator
        // and for-loop paths, not just independently re-derived to agree.
        // fix #2298 / §28.9: report `outWrapMade = true` so the caller
        // (buildOrderedArgs) registers the scope-end iterEnumDispose that
        // frees the wrapper frame (slot-1 Dispose = `free(self)` for both
        // Array and Stack — only the frame, never the shared backing).
        // fix #2299 / §28.9: at the RETURN coercion, a callee-owned Array/
        // Queue source is wrapped in an OWNING enumerator (slot-1 Dispose
        // frees the wrapper frame AND the backing array — the caller
        // disposes the enumerator exactly once). A Stack/OwningStack source
        // has no OWNING-wrap variant in this fix; the borrowing wrap is
        // kept and the source is freed immediately after the wrap is built
        // (the wrap's slot-1 Dispose is still `free(self)` on the caller's
        // side, freeing only the wrapper frame — the backing was already
        // reclaimed). A borrowed source keeps the plain wrap (the source's
        // owner retains the disposal obligation).
        if (outWrapMade) *outWrapMade = true;
        if (srcOwnedByCallee) {
            if (an == "Stack" || an == "OwningStack") {
                llvm::Value* wrap = wrapStackAsIEnumerator(v, targetTy);
                freeCollectionSourceNow(v);
                return wrap;
            }
            return wrapOwnedArrayAsIEnumerable(v, targetTy);
        }
        if (an == "Stack" || an == "OwningStack")
            return wrapStackAsIEnumerator(v, targetTy);
        return wrapArrayAsIEnumerable(v, targetTy);
    }
    // fix #1069 / stdlib §4.10 / §5: DICT-backed `Set<T>` — snapshot its element
    // keys into a fresh array and hand out an OWNING enumerator over the
    // snapshot. Reuses the tested §389 Set lazy source (bz_lazy_from_set, which
    // snapshots the key table) and the #1916 drain (bz_lazy_to_array, which
    // materialises the array AND disposes the lazy source), so the only new
    // object is the snapshot the returned enumerator owns. fix #2299 / §28.9:
    // the SOURCE set is no longer needed once the snapshot is taken (the
    // snapshot is independent, owning its own key table) — the caller
    // returns the OWNING-enumerator snapshot, while the source set is freed
    // right here at the RETURN site. (Registering the free at scope end
    // would race the caller's use of the returned wrapper.) The source local's
    // normal reclaim is suppressed by §28.3, so this free is exactly-once.
    if (actualTy && (an == "Set" || an == "OwningSet")) {
        uint64_t ksz = 0;
        if (genericBackingType(actualTy, 0, /*isSet=*/true, &ksz)) {
            auto* setFn = declareC("bz_lazy_from_set", ptrTy(), {ptrTy(), i64()});
            llvm::Value* lazy = builder_.CreateCall(
                setFn, {v, builder_.getInt64((int64_t)ksz)}, "setenum.lazy");
            if (outWrapMade) *outWrapMade = true;
            // fix #2299 / §28.9: free the source set right after the snapshot.
            if (srcOwnedByCallee)
                freeDictSourceNow(v);
            return drainLazyPipelineToOwnedIEnumerable(lazy, targetTy);
        }
    }
    // fix #2298 / §28.9: LinkedList<T> widened to `IEnumerable<T>` here uses
    // the plain borrow wrap (slot-1 Dispose = `free(self)`, the wrapper frame
    // only). fix #2299 / §28.9: LinkedList has no OWNING-wrap variant in
    // this fix; for a callee-owned return, free the source immediately after
    // the wrap is built (the caller disposes the wrap frame only).
    if (actualTy && isLinkedListTypeName(actualTy)) {
        if (outWrapMade) *outWrapMade = true;
        if (srcOwnedByCallee) {
            llvm::Value* wrap = wrapLinkedListAsIEnumerable(v, targetTy);
            freeLinkedListSourceNow(v);
            return wrap;
        }
        return wrapLinkedListAsIEnumerable(v, targetTy);
    }
    // A user class implementing IEnumerable<T> (declaration-carrying). A source
    // whose type is ALREADY a declaration-less IEnumerable (an alias / another
    // wrapped value) has no declaration and passes through unchanged.
    if (actualTy && actualTy->kind == sema::TypeRef::Kind::Named &&
        actualTy->declaration)
        return wrapUserClassAsIEnumerable(v, actualTy->declaration, targetTy);
    return v;
}

// fix #2299 / §28.9: emit an immediate bz_array_free(v, null) for the
// source Array-family collection, right at the RETURN coercion (NOT at
// scope end — the return is about to hand the wrapping enumerator to the
// caller, and any scope-end disposal would fire before the caller
// finishes iterating the wrap and free the source out from under it).
// Caller MUST only invoke when srcOwnedByCallee was true (a parameter/
// field/already-IEnumerable source must stay borrowed — its owner keeps
// the disposal obligation). The wrapper's slot-1 Dispose runs on the
// caller's side and frees only the wrapper frame (the backing was
// already reclaimed here).
void Emitter::freeCollectionSourceNow(llvm::Value* v) {
    if (!v) return;
    auto* fn = declareC("bz_array_free", llvm::Type::getVoidTy(llvm_),
                        {ptrTy(), ptrTy()});
    builder_.CreateCall(fn, {v, llvm::ConstantPointerNull::get(ptrTy())});
}

// fix #2299 / §28.9: emit an immediate bz_dict_free(v, null) for the
// source dict-backed Set after the snapshot.
void Emitter::freeDictSourceNow(llvm::Value* v) {
    if (!v) return;
    auto* fn = declareC("bz_dict_free", llvm::Type::getVoidTy(llvm_),
                        {ptrTy(), ptrTy()});
    builder_.CreateCall(fn, {v, llvm::ConstantPointerNull::get(ptrTy())});
}

// fix #2299 / §28.9: emit an immediate bz_llist_free(v, null) for the
// source LinkedList after the wrap.
void Emitter::freeLinkedListSourceNow(llvm::Value* v) {
    if (!v) return;
    auto* fn = declareC("bz_llist_free", llvm::Type::getVoidTy(llvm_),
                        {ptrTy(), ptrTy()});
    builder_.CreateCall(fn, {v, llvm::ConstantPointerNull::get(ptrTy())});
}

// fix #2299 / §28.9: at the RETURN coercion, decide whether the source
// expression is a callee-OWNED producer whose ownership transfer on `return`
// must be honoured by the wrap site. Mirrors `registerIterableSourceDisposal`'s
// `ownsSource` gate (the local-decl path's #943/#1911/#1069 sibling, §29.11):
//   * a `new T(...)` for any reference type → owned (§28.3 K1).
//   * a `move e` → owned (§28.8.1 transfer mark).
//   * an IdentifierExpr naming a LOCAL whose initializer was a producer form
//     (NewExpr, CallExpr) → owned — the local's `+1` flows out via §28.3.
// Anything else (a parameter read, a field read, a method-call result whose
// declared return is not `owned`, a borrow-init local, an IndexExpr, an
// arbitrary shape) is a BORROW; we do not register the source-disposal here.
// Conservative: only NEW-of-ref and producer-init LOCAL classify as Owned
// (a wider net risks UAF on borrowed collections that share the slot with
// another owner).
bool Emitter::returnSourceIsCalleeOwnedCollection(const ast::Expr* src) const {
    if (!src) return false;
    // fix #2299: a fresh-owned producer. `new T(...)` of a CLASS (or interface)
    // is a +1 producer; the new T(...) shorthand (`new(...)`) is the same
    // NewExpr with `type` null but sema has recorded the resolved target as
    // the NewExpr's exprType, so type-class is checked via exprType here.
    // Array<T> is a borrowing collection (no ownable backing — stdlib §4.4),
    // but the wrapped RESULT is what we own (the NewExpr mints a fresh
    // bz_array_t); callee-ownership is recognized by the fact that the source
    // is a class-typed NewExpr (a class has +1 disposal obligation at the
    // allocation site). For non-class types (records, value builtins) we
    // fall through — they have no ownership to transfer (no disposal site).
    if (dynamic_cast<const ast::NewExpr*>(src)) {
        const sema::TypeRef* rt = ctx_.exprType(*src);
        if (rt && rt->kind == sema::TypeRef::Kind::Named) {
            // Class or interface declaration → owned. For decl-NULL (builtin
            // collection types — Array<T>, Set<T>, LinkedList<T>, etc.) we
            // recognize them by name. Both cases are "fresh +1 producers
            // where the wrapper must take ownership of the allocation".
            if (rt->declaration) {
                if (rt->declaration->kind == ast::TypeDeclKind::Class ||
                    rt->declaration->kind == ast::TypeDeclKind::Interface)
                    return true;
            } else if (rt->astNode && rt->astNode->name &&
                       !rt->astNode->name->parts.empty()) {
                std::string_view n = rt->headName();
                // Borrowing-family builtins (whose allocation is the source's
                // sole bz_array_t / bz_llist_t) and the dict-backed Set. These
                // are the cases the wrapAsIEnumerableForTarget branches handle
                // (Array/Stack/Queue wrap, LinkedList wrap, dict-backed Set
                // snapshot). Match them here so the OWNING/early-free logic
                // in the wrap function applies.
                if (n == "Array" || n == "OwningArray" || n == "OwningBuffer" ||
                    n == "Stack" || n == "OwningStack" ||
                    n == "Queue" || n == "OwningQueue" ||
                    n == "LinkedList" || n == "OwningLinkedList" ||
                    n == "Set" || n == "OwningSet" || n == "SortedSet" ||
                    n == "Dictionary" || n == "OwningDictionary" ||
                    n == "SortedDictionary")
                    return true;
            }
        }
        // Value-type NewExpr (record / built-in value type / non-collection
        // class) is Borrowed — no ownership to transfer; falls through.
        return false;
    }
    // `move e` — the §28.8.1 transfer mark moves ownership into the
    // destination (here: the return wrapper), so the source local is freed
    // exactly once at scope end (the UnwindIneligibleWalker suppresses the
    // moved-from local's normal reclaim). The wrap can reuse that disposal:
    // if `e` is a callee-owned local the underlying array/llist is the freed
    // target. Conservatively only return true when the moved operand is
    // itself a NEW or producer-init LOCAL (the cases that own something).
    if (auto* un = dynamic_cast<const ast::UnaryExpr*>(src)) {
        if (un->op == "move" && un->operand)
            return returnSourceIsCalleeOwnedCollection(un->operand.get());
        return false;
    }
    // An IdentifierExpr naming a local: the local is owned if and only if its
    // initializer was a producer form (NewExpr/CallExpr). The bare-identifier
    // alias case (`return b;` where `b` was `var b := a;`) is intentionally
    // classified Borrowed — `a` still owns and disposes the collection, and
    // registering a disposal here would double-free. The same shared predicate
    // registerIterableSourceDisposal uses (`isCall || isNew`) for the local-
    // decl `ownsSource` gate, so the two sites agree.
    if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(src)) {
        sema::Symbol* sym = ctx_.exprSymbol(*id);
        if (auto* ls = dynamic_cast<sema::LocalSymbol*>(sym)) {
            const ast::LocalDecl* ld =
                dynamic_cast<const ast::LocalDecl*>(ls->decl);
            if (!ld || !ld->initializer) return false;
            if (dynamic_cast<const ast::NewExpr*>(ld->initializer.get()))
                return true;
            if (dynamic_cast<const ast::CallExpr*>(ld->initializer.get()))
                return true;
            return false;
        }
        // A ParameterSymbol, FieldSymbol, PropertySymbol, or a synthetic
        // (sema-created) symbol is a BORROW by definition (§28.3 storage-
        // lends + §28.6 no transfer without an explicit move mark).
        return false;
    }
    // Method-call results, member-access, index, ternary, etc. — the
    // narrower forms above already cover the typical RETURN shapes; a deeper
    // ownership analysis (call-return-is-owned) is deliberately out of scope
    // for this fix — err on the BORROW side.
    return false;
}

// §29.2 (#437): generate a type-specific, vtable-backed array enumerator.

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
        // (drain writes the popped class pointer into it). Zero-init the slot
        // up front: #2264 / §28.3 — bz_array_drain_next returns false WITHOUT
        // writing *val_out when the array is empty (a performance choice on
        // the hot path), so the slot keeps the previous invocation's stale
        // pointer on the final (false) MoveNext. The unconditional
        // "load → store to pending" below would then publish that stale
        // pointer to the wrapper's pending field, and the using-var scope-end
        // Dispose (which calls emitDisposeAndReclaim on a non-null pending)
        // would double-free / UAF the already-disposed element. Zero-init in
        // the entry block runs on every MoveNext invocation, so the final
        // false-returning call observes a clean null slot and publishes null
        // to pending — Dispose becomes a no-op as §29.1 requires.
        auto* allocaBB = &mnFn->getEntryBlock();
        llvm::IRBuilder<> ab(allocaBB, allocaBB->begin());
        auto* outSlot = ab.CreateAlloca(ptrTy(), nullptr, "oaem.out");
        ab.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), outSlot,
                       "oaem.out.init");
        auto* drainFT = llvm::FunctionType::get(
            i1(), {ptrTy(), ptrTy()}, false);
        auto* drainFn = module_->getOrInsertFunction(
            "bz_array_drain_next", drainFT).getCallee();
        llvm::Value* ok = builder_.CreateCall(drainFT, drainFn, {arrPtr, outSlot},
                                       "oaem.ok");
        // Store the popped pointer into pending. With the entry-block zero-init
        // above, this is correct on both branches: drain-true → publish the
        // just-written popped pointer; drain-false → publish null (the init),
        // matching bz_array_drain_next's "false ⇒ no write" contract.
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

// #2471 (stdlib §4.11 "Iteration order" v0.195; language spec §29.1,
// agreeing with #2472's for-loop fix, LlvmBackend_Stmts.cpp): the
// REVERSE-order twin of ensureArrayEnumMachinery, for Stack<T>/
// OwningStack<T>'s explicit `.GetEnumerator()` call. Mirrors that
// function's structure field-for-field (see its own comments for the
// Dispose/Current rationale, unchanged here) with three differences, all
// driven by the direction reversal:
//   1. A distinct struct/function name prefix ("stackenum."/
//      "bz$stackenum$..." vs "arrenum."/"bz$arrenum$...") so this cache
//      never collides with or is mistaken for the forward machinery.
//   2. MoveNext counts DOWN (`idx--; return idx >= 0`) instead of up —
//      §4.11's "a stack top-to-bottom (pop order)" contract, matching the
//      SAME physical index Pop() itself reads (LlvmBackend_Calls.cpp
//      ~4566-4588, `m == "Pop"`: `len - 1`).
//   3. The "before-first" (top) sentinel is `bz_array_len(arr)` — a RUNTIME
//      value that must be (re-)fetched, not the forward machinery's fixed
//      `-1` constant — computed once here at wrap time and again inside the
//      GetEnumerator (clone) function below, so a clone always starts from
//      the collection's CURRENT length rather than one captured earlier.
Emitter::ArrEnumMachinery Emitter::ensureStackEnumMachinery(llvm::Type* elemTy) {
    int64_t esz = 8;
    if (elemTy->isIntegerTy(32) || elemTy->isFloatTy()) esz = 4;
    else if (elemTy->isIntegerTy(1) || elemTy->isIntegerTy(8)) esz = 1;
    else if (elemTy->isIntegerTy(16)) esz = 2;
    else if (elemTy->isStructTy())
        esz = (int64_t)module_->getDataLayout().getTypeAllocSize(elemTy);
    else esz = 8;

    std::string elemName = elemTy->isIntegerTy() ?
        ("i" + std::to_string(elemTy->getIntegerBitWidth())) :
        (elemTy->isPointerTy() ? "ptr" : "f");
    std::string stName = "stackenum." + elemName;
    auto* stTy = llvm::StructType::getTypeByName(llvm_, stName);
    if (!stTy) {
        stTy = llvm::StructType::create(llvm_,
            {ptrTy(), ptrTy(), i64(), i64()}, stName);
    }

    std::string prefix = "bz$stackenum$" + elemName + "$";
    auto getOrCreate = [&](const std::string& name, llvm::FunctionType* ft,
                           std::function<void(llvm::Function*)> fill)
        -> llvm::Function* {
        if (auto* f = module_->getFunction(name)) return f;
        auto* f = llvm::Function::Create(ft, llvm::Function::InternalLinkage,
                                         name, module_.get());
        fill(f);
        return f;
    };

    // Dispose: free(self) — identical to the forward machinery's.
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

    // MoveNext: e->idx--; return idx >= 0.
    auto* mnFn = getOrCreate(prefix + "MoveNext",
        llvm::FunctionType::get(i1(), {ptrTy()}, false),
        [&](llvm::Function* f) {
            auto* bb = llvm::BasicBlock::Create(llvm_, "entry", f);
            llvm::IRBuilder<> b(bb);
            llvm::Value* self = f->arg_begin();
            llvm::Value* idxF = b.CreateStructGEP(stTy, self, 2);
            llvm::Value* idx = b.CreateLoad(i64(), idxF);
            llvm::Value* next =
                b.CreateSub(idx, llvm::ConstantInt::get(i64(), 1));
            b.CreateStore(next, idxF);
            b.CreateRet(
                b.CreateICmpSGE(next, llvm::ConstantInt::get(i64(), 0)));
        });

    // Current: load element at idx and return by value — identical to the
    // forward machinery's (direction affects only which idx MoveNext leaves
    // behind; Current always just reads whatever idx currently holds).
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
            auto* ptrAtFT = llvm::FunctionType::get(
                ptrTy(), {ptrTy(), i64(), i64()}, false);
            auto* ptrAtFn =
                module_->getOrInsertFunction("bz_array_ptr_at", ptrAtFT)
                    .getCallee();
            llvm::Value* ep = b.CreateCall(ptrAtFT, ptrAtFn,
                {arrPtr, idx, llvm::ConstantInt::get(i64(), esz)});
            b.CreateRet(b.CreateLoad(elemTy, ep));
        });

    // GetEnumerator(self) → a FRESH independent enumerator over the same
    // array, positioned before the first (top) element — idx =
    // bz_array_len(arr), re-fetched fresh in THIS clone (not copied from
    // `self`'s idx field, which may already be partway through its own
    // walk) so a second `for`/manual walk over the same source replays the
    // CURRENT top-to-bottom order from the top, per §29.11.
    auto* geFn = getOrCreate(prefix + "GetEnumerator",
        llvm::FunctionType::get(ptrTy(), {ptrTy()}, false),
        [&](llvm::Function* f) {
            auto* bb = llvm::BasicBlock::Create(llvm_, "entry", f);
            llvm::IRBuilder<> b(bb);
            llvm::Value* self = f->arg_begin();
            uint64_t cloneSz = module_->getDataLayout().getTypeAllocSize(stTy);
            auto* callocFT =
                llvm::FunctionType::get(ptrTy(), {i64(), i64()}, false);
            auto* callocC =
                module_->getOrInsertFunction("calloc", callocFT).getCallee();
            llvm::Value* clone = b.CreateCall(callocFT, callocC,
                {llvm::ConstantInt::get(i64(), 1),
                 llvm::ConstantInt::get(i64(), (int64_t)cloneSz)});
            llvm::Value* vt =
                b.CreateLoad(ptrTy(), b.CreateStructGEP(stTy, self, 0));
            b.CreateStore(vt, b.CreateStructGEP(stTy, clone, 0));
            llvm::Value* arrCp =
                b.CreateLoad(ptrTy(), b.CreateStructGEP(stTy, self, 1));
            b.CreateStore(arrCp, b.CreateStructGEP(stTy, clone, 1));
            llvm::Value* eszCp =
                b.CreateLoad(i64(), b.CreateStructGEP(stTy, self, 3));
            b.CreateStore(eszCp, b.CreateStructGEP(stTy, clone, 3));
            auto* lenFT = llvm::FunctionType::get(i64(), {ptrTy()}, false);
            auto* lenFn2 =
                module_->getOrInsertFunction("bz_array_len", lenFT)
                    .getCallee();
            llvm::Value* len = b.CreateCall(lenFT, lenFn2, {arrCp});
            b.CreateStore(len, b.CreateStructGEP(stTy, clone, 2));
            b.CreateRet(clone);
        });

    // Build the vtable (same 5-slot §29.11 layout as the forward machinery).
    std::string vtName = prefix + "vtable";
    auto* vtGV = module_->getGlobalVariable(vtName);
    if (!vtGV) {
        auto* vtTy = llvm::ArrayType::get(ptrTy(), 5);
        std::vector<llvm::Constant*> elems = {
            llvm::ConstantPointerNull::get(ptrTy()),
            dispFn, mnFn, curFn, geFn
        };
        vtGV = new llvm::GlobalVariable(*module_, vtTy, /*isConst=*/true,
            llvm::GlobalValue::InternalLinkage,
            llvm::ConstantArray::get(vtTy, elems), vtName);
        elems[0] = llvm::ConstantExpr::getPointerCast(vtGV, ptrTy());
        vtGV->setInitializer(llvm::ConstantArray::get(vtTy, elems));
    }

    return {stTy, vtGV, esz};
}

// #2471: build the initial (before-first / top) reverse enumerator struct
// over `arr`, mirroring wrapArrayAsIEnumerable's shape exactly (see its own
// comment) except the idx field is `bz_array_len(arr)` — a runtime value —
// rather than a fixed `-1`.
llvm::Value* Emitter::wrapStackAsIEnumerator(llvm::Value* arr,
                                             const sema::TypeRef* iEnumTy) {
    llvm::Type* elemTy = i32();
    if (iEnumTy && !iEnumTy->genericArgs.empty() && iEnumTy->genericArgs[0])
        elemTy = lowerTypeRef(iEnumTy->genericArgs[0]);
    ArrEnumMachinery m = ensureStackEnumMachinery(elemTy);
    llvm::StructType* stTy = m.stTy;
    uint64_t stSz = module_->getDataLayout().getTypeAllocSize(stTy);
    auto* callocFn2 = declareC("calloc", ptrTy(), {i64(), i64()});
    llvm::Value* en = builder_.CreateCall(callocFn2,
        {builder_.getInt64(1), builder_.getInt64(stSz)}, "stkenum");
    builder_.CreateStore(m.vtGV, builder_.CreateStructGEP(stTy, en, 0));
    builder_.CreateStore(arr, builder_.CreateStructGEP(stTy, en, 1));
    auto* lenFn = declareC("bz_array_len", i64(), {ptrTy()});
    llvm::Value* len = builder_.CreateCall(lenFn, {arr});
    builder_.CreateStore(len, builder_.CreateStructGEP(stTy, en, 2));
    builder_.CreateStore(builder_.getInt64(m.esz),
        builder_.CreateStructGEP(stTy, en, 3));
    return en;
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

    // fix #2487 (stdlib §4.11 "Iteration order" v0.195; language spec §29.1,
    // v0.236): the collection whose backing THIS recursion level enumerates
    // may itself be a Stack<T>/OwningStack<T>, in which case ITS OWN traversal
    // must be top-to-bottom (pop order), not physical/forward order —
    // independent of whether the element needs per-item re-wrapping. Two
    // independent defects, both captured here by reading storedTy's head name
    // at this level:
    //   * Defect B (leaf): a Stack<T> ELEMENT of an outer Array<Stack<T>>
    //     recurses in here as storedTy=Stack<T>, viewTy=IEnumerable<T>. The
    //     leaf branch hardcoded the forward ensureArrayEnumMachinery; it must
    //     instead reuse #2471's reverse ensureStackEnumMachinery and seed
    //     idx = bz_array_len(backing) (runtime before-top sentinel).
    //   * Defect A (nested): a Stack<T> OUTER container (e.g.
    //     Stack<Array<T>>) is handled at THIS level as storedTy=
    //     Stack<Array<T>>. The nested MoveNext/GetEnumerator hardcoded a
    //     forward cursor (idx = -1, idx++, idx < len); it must count DOWN
    //     (idx = len, idx--, idx >= 0), mirroring ensureStackEnumMachinery.
    auto isStackBacking = [](const sema::TypeRef* t) -> bool {
        if (!t || t->kind != sema::TypeRef::Kind::Named) return false;
        std::string_view h = t->headName();
        return h == "Stack" || h == "OwningStack";
    };
    bool backingIsStack = isStackBacking(storedTy);

    if (!elemNeedsWrap) {
        // Leaf: a plain enumerator over the (borrowed) backing array. #2487
        // Defect B: route a Stack/OwningStack leaf through #2471's reverse
        // machinery (ensureStackEnumMachinery) instead of the forward
        // ensureArrayEnumMachinery, and seed idx = bz_array_len (the runtime
        // before-top sentinel) instead of the fixed -1. The cache key (and
        // machinery) carry an "S" tag so a Stack<int> leaf and an Array<int>
        // leaf in the same module get DISTINCT factories/vtables.
        std::string name = (backingIsStack ? "bz$viewfac$SP$" : "bz$viewfac$P$")
                           + elemName;
        if (auto* f = module_->getFunction(name)) return f;
        ArrEnumMachinery m = backingIsStack
            ? ensureStackEnumMachinery(elemTy)
            : ensureArrayEnumMachinery(elemTy);
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
        if (backingIsStack) {
            // idx = bz_array_len(backing) — runtime before-top sentinel,
            // matching wrapStackAsIEnumerator (#2471).
            auto* lenFT = llvm::FunctionType::get(i64(), {ptrTy()}, false);
            auto* lenFn = module_->getOrInsertFunction("bz_array_len", lenFT)
                              .getCallee();
            llvm::Value* len = b.CreateCall(lenFT, lenFn, {backing});
            b.CreateStore(len, b.CreateStructGEP(m.stTy, en, 2));
        } else {
            b.CreateStore(llvm::ConstantInt::get(i64(), -1),
                          b.CreateStructGEP(m.stTy, en, 2));
        }
        b.CreateStore(llvm::ConstantInt::get(i64(), m.esz),
                      b.CreateStructGEP(m.stTy, en, 3));
        b.CreateRet(en);
        return f;
    }

    // Nested: each element (a bz_array_t*) is itself wrapped as `viewElem` via
    // the inner factory. Recurse to get it first (its name encodes the inner
    // shape, so this factory's cache key is unique per whole wrap shape).
    llvm::Function* innerFac = ensureIEnumViewFactory(storedElem, viewElem);
    // #2487 Defect A: encode the OUTER backing kind in this factory's name AND
    // the nested machinery's struct/function prefix. A Stack<Array<T>> (reverse
    // outer) and an Array<Array<T>> (forward outer) share the SAME innerFac
    // (both leaf over Array<T> → IEnumerable<T>), so without this tag they
    // would collide on one nested MoveNext/GetEnumerator — the only pieces
    // that differ by outer direction. The "S" tag keeps them distinct, exactly
    // as the leaf branch's tag keeps a Stack<int> leaf distinct from an
    // Array<int> leaf.
    std::string name = (backingIsStack ? "bz$viewfac$SN$" : "bz$viewfac$N$")
                       + innerFac->getName().str();
    if (auto* f = module_->getFunction(name)) return f;

    // Nested enumerator struct: { ptr vtable, ptr arr, i64 idx, i64 esz,
    //                             ptr curInner }.  esz here is 8 — the outer
    //  backing array's elements are collection pointers (bz_array_t*).
    std::string stName = (backingIsStack ? "stackarrenumnest." : "arrenumnest.")
                         + elemName;
    auto* stTy = llvm::StructType::getTypeByName(llvm_, stName);
    if (!stTy)
        stTy = llvm::StructType::create(
            llvm_, {ptrTy(), ptrTy(), i64(), i64(), ptrTy()}, stName);

    std::string prefix = (backingIsStack ? "bz$stackarrenumnest$"
                                         : "bz$arrenumnest$")
                         + elemName + "$";
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
    // return whether another element remains. #2487 Defect A: a Stack OUTER
    // counts DOWN (idx--; return idx >= 0) — §4.11 pop order — mirroring
    // ensureStackEnumMachinery's own MoveNext; an Array/Queue outer counts UP
    // (idx++; return idx < len), as before.
    auto* mnFn = getOrCreate(prefix + "MoveNext",
        llvm::FunctionType::get(i1(), {ptrTy()}, false),
        [&](llvm::Function* f) {
            auto* bb = llvm::BasicBlock::Create(llvm_, "entry", f);
            llvm::IRBuilder<> b(bb);
            llvm::Value* self = f->arg_begin();
            disposeCurInner(b, self);
            llvm::Value* idxF = b.CreateStructGEP(stTy, self, 2);
            llvm::Value* idx = b.CreateLoad(i64(), idxF);
            llvm::Value* next = backingIsStack
                ? b.CreateSub(idx, llvm::ConstantInt::get(i64(), 1))
                : b.CreateAdd(idx, llvm::ConstantInt::get(i64(), 1));
            b.CreateStore(next, idxF);
            if (backingIsStack) {
                b.CreateRet(b.CreateICmpSGE(
                    next, llvm::ConstantInt::get(i64(), 0)));
            } else {
                llvm::Value* arrPtr =
                    b.CreateLoad(ptrTy(), b.CreateStructGEP(stTy, self, 1));
                auto* lenFT = llvm::FunctionType::get(i64(), {ptrTy()}, false);
                auto* lenFn = module_->getOrInsertFunction("bz_array_len", lenFT)
                                  .getCallee();
                llvm::Value* len = b.CreateCall(lenFT, lenFn, {arrPtr});
                b.CreateRet(b.CreateICmpSLT(next, len));
            }
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
    // before the first element (curInner = null) — §29.11 replay. #2487 Defect A:
    // a Stack OUTER seeds idx = bz_array_len(arrCp), re-fetched fresh in THIS
    // clone (not copied from self's idx, which may be partway through its own
    // walk), matching ensureStackEnumMachinery's own GetEnumerator; an Array/
    // Queue outer seeds the fixed -1, as before.
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
            if (backingIsStack) {
                auto* lenFT = llvm::FunctionType::get(i64(), {ptrTy()}, false);
                auto* lenFn =
                    module_->getOrInsertFunction("bz_array_len", lenFT)
                        .getCallee();
                llvm::Value* len = b.CreateCall(lenFT, lenFn, {arrCp});
                b.CreateStore(len, b.CreateStructGEP(stTy, clone, 2));
            } else {
                b.CreateStore(llvm::ConstantInt::get(i64(), -1),
                              b.CreateStructGEP(stTy, clone, 2));
            }
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
    // #2487 Defect A: a Stack OUTER seeds idx = bz_array_len(backing) (runtime
    // before-top sentinel, matching wrapStackAsIEnumerator); an Array/Queue
    // outer seeds the fixed -1.
    if (backingIsStack) {
        auto* lenFT = llvm::FunctionType::get(i64(), {ptrTy()}, false);
        auto* lenFn = module_->getOrInsertFunction("bz_array_len", lenFT)
                          .getCallee();
        llvm::Value* len = b.CreateCall(lenFT, lenFn, {backing});
        b.CreateStore(len, b.CreateStructGEP(stTy, en, 2));
    } else {
        b.CreateStore(llvm::ConstantInt::get(i64(), -1),
                      b.CreateStructGEP(stTy, en, 2));
    }
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


// #2471 (stdlib §4.8/§4.12; language spec §29.1, v0.236): Dictionary<K,V>'s
// explicit `.GetEnumerator()` call. Dictionary is hash-backed (bz_dict_t*),
// not array-backed — its entries are not a dense element array — so (unlike
// Array/Stack/Queue) we MATERIALISE the entries into a fresh owned bz_array_t
// of {K, V} structs (the SAME layout lowerDictEntryFor and the Calls3 LINQ
// source use), then wrap that array via wrapOwnedArrayAsIEnumerable. The
// wrap is OWNING — the returned IEnumerable's slot-1 Dispose frees the
// materialised snapshot exactly once; the SOURCE dictionary is untouched
// (no entry is retained/released/disposed here, so an owning dictionary
// still disposes each value at its own binder's scope end). The decode loop
// mirrors lowerDictEntryFor / the Calls3 LINQ source field-for-field,
// including the string-key fast path (rebuilds a string object per key,
// frees bz_dict_keys_str's raw bytes), the generic-key table (loads from
// the malloc'd raw-bytes key copy and frees it), and every value encoding
// (record-box load, nullable-value load, scalar inttoptr/bitcast, class
// pointer pass-through). Iteration order is insertion order (the order
// bz_dict_keys_*/bz_dict_values_* return), matching §4.8's contract and
// lowerDictEntryFor's foreach behaviour.
llvm::Value* Emitter::wrapDictionaryAsIEnumerable(
        llvm::Value* dict, const sema::TypeRef* iEnumTy) {
    if (!dict || !iEnumTy) return nullptr;
    // iEnumTy = IEnumerator<DictionaryEntry<K,V>>; drill into the entry to
    // recover K and V.
    if (iEnumTy->genericArgs.empty() || !iEnumTy->genericArgs[0]) return nullptr;
    const sema::TypeRef* entryTy = iEnumTy->genericArgs[0];
    if (entryTy->genericArgs.size() < 2 || !entryTy->genericArgs[0] ||
        !entryTy->genericArgs[1])
        return nullptr;
    const sema::TypeRef* keyTy = entryTy->genericArgs[0];
    const sema::TypeRef* valTy = entryTy->genericArgs[1];

    // Record / nullable-val flags for value (mirrors lowerDictEntryFor).
    const sema::TypeSymbol* valRec =
        (valTy->kind == sema::TypeRef::Kind::Named && valTy->declaration &&
         valTy->declaration->kind == ast::TypeDeclKind::Record)
            ? valTy->declaration : nullptr;
    bool valNullableVal = isNullableValueTy(valTy);

    llvm::Type* keyLlvm = lowerTypeRef(keyTy);
    llvm::Type* valLlvm = lowerTypeRef(valTy);
    if (!keyLlvm || !valLlvm) return nullptr;
    llvm::StructType* entryStTy =
        llvm::StructType::get(llvm_, {keyLlvm, valLlvm});
    const llvm::DataLayout& dl = module_->getDataLayout();
    int64_t esz = (int64_t)dl.getTypeAllocSize(entryStTy);

    // String-key dicts use bz_dict_keys_str (fast path); generic-key dicts
    // (record/int/enum keys) use bz_dict_keys_gen. Mirrors lowerDictEntryFor /
    // the Calls3 LINQ source. Determine the backing by checking the key type:
    // a Primitive String key is the string fast path; every other valid dict
    // key (int/record/enum) is the generic-key table. (Cannot reuse
    // genericBackingType here without the Dictionary's own TypeRef, which the
    // iEnumTy does not carry — its genericArgs[0] is the entry, not the dict.)
    using K = sema::TypeRef::Kind;
    bool isStringKey = keyTy->kind == K::Primitive &&
                       keyTy->primitive == sema::PrimitiveType::String;
    bool useGen = !isStringKey;

    // --- snapshot keys + values ---
    auto* keysOutSlot = entryAlloca(ptrTy());
    auto* lensOutSlot = entryAlloca(ptrTy());
    auto* cntSlot     = entryAlloca(i64());
    auto* valsOutSlot = entryAlloca(ptrTy());

    if (useGen) {
        auto* kfn = declareC("bz_dict_keys_gen",
            llvm::Type::getVoidTy(llvm_), {ptrTy(), ptrTy(), ptrTy()});
        builder_.CreateCall(kfn, {dict, keysOutSlot, cntSlot});
        auto* vfn = declareC("bz_dict_values_gen",
            llvm::Type::getVoidTy(llvm_), {ptrTy(), ptrTy(), ptrTy()});
        builder_.CreateCall(vfn, {dict, valsOutSlot, entryAlloca(i64())});
    } else {
        auto* kfn = declareC("bz_dict_keys_str",
            llvm::Type::getVoidTy(llvm_),
            {ptrTy(), ptrTy(), ptrTy(), ptrTy()});
        builder_.CreateCall(kfn, {dict, keysOutSlot, lensOutSlot, cntSlot});
        auto* vfn = declareC("bz_dict_values_str",
            llvm::Type::getVoidTy(llvm_),
            {ptrTy(), ptrTy(), ptrTy()});
        builder_.CreateCall(vfn, {dict, valsOutSlot, entryAlloca(i64())});
    }

    llvm::Value* keysArr = builder_.CreateLoad(ptrTy(), keysOutSlot);
    llvm::Value* lensArr = isStringKey
                               ? builder_.CreateLoad(ptrTy(), lensOutSlot)
                               : nullptr;
    llvm::Value* valsArr = builder_.CreateLoad(ptrTy(), valsOutSlot);
    llvm::Value* cnt     = builder_.CreateLoad(i64(), cntSlot);

    // Allocate the result bz_array_t (8-slot initial capacity, like Calls3).
    auto* allocFn = declareC("bz_array_alloc", ptrTy(), {i64(), i64()});
    llvm::Value* arr = builder_.CreateCall(
        allocFn, {builder_.getInt64(esz), builder_.getInt64(8)}, "denum.arr");

    auto* addFn = declareC("bz_array_add", llvm::Type::getVoidTy(llvm_),
                           {ptrTy(), ptrTy(), i64()});
    auto* freeFn = declareC("free", llvm::Type::getVoidTy(llvm_), {ptrTy()});

    // Decode loop — build entry structs and append to arr.
    auto* idxSlot = entryAlloca(i64());
    builder_.CreateStore(builder_.getInt64(0), idxSlot);
    auto* entrySlot = entryAlloca(entryStTy);

    auto* condBB = llvm::BasicBlock::Create(llvm_, "denum.cond", currentFn_);
    auto* bodyBB = llvm::BasicBlock::Create(llvm_, "denum.body", currentFn_);
    auto* endBB  = llvm::BasicBlock::Create(llvm_, "denum.end",  currentFn_);
    builder_.CreateBr(condBB);

    builder_.SetInsertPoint(condBB);
    {
        llvm::Value* idx = builder_.CreateLoad(i64(), idxSlot);
        builder_.CreateCondBr(
            builder_.CreateICmpSLT(idx, cnt), bodyBB, endBB);
    }

    builder_.SetInsertPoint(bodyBB);
    {
        llvm::Value* idxB = builder_.CreateLoad(i64(), idxSlot);

        // --- decode key (mirrors lowerDictEntryFor) ---
        llvm::Value* keyDecoded;
        if (isStringKey) {
            auto* mallocFn = declareC("malloc", ptrTy(), {i64()});
            auto* memcpyFn = declareC("memcpy", ptrTy(),
                                      {ptrTy(), ptrTy(), i64()});
            auto* callocFn = declareC("calloc", ptrTy(), {i64(), i64()});
            auto* i8Ty = llvm::Type::getInt8Ty(llvm_);
            uint64_t strSz = dl.getTypeAllocSize(stringType());
            llvm::Value* kpPtr = builder_.CreateGEP(ptrTy(), keysArr, idxB);
            llvm::Value* kp    = builder_.CreateLoad(ptrTy(), kpPtr);
            llvm::Value* lpPtr = builder_.CreateGEP(i64(), lensArr, idxB);
            llvm::Value* kl    = builder_.CreateLoad(i64(), lpPtr);
            llvm::Value* dataSz  = builder_.CreateAdd(kl, builder_.getInt64(1));
            llvm::Value* dataCpy = builder_.CreateCall(mallocFn, {dataSz});
            builder_.CreateCall(memcpyFn, {dataCpy, kp, kl});
            llvm::Value* nulPtr = builder_.CreateGEP(i8Ty, dataCpy, kl);
            builder_.CreateStore(llvm::ConstantInt::get(i8Ty, 0), nulPtr);
            llvm::Value* strObj = builder_.CreateCall(
                callocFn,
                {builder_.getInt64(1), builder_.getInt64((int64_t)strSz)});
            builder_.CreateStore(dataCpy,
                builder_.CreateStructGEP(stringType(), strObj, 0));
            builder_.CreateStore(kl,
                builder_.CreateStructGEP(stringType(), strObj, 1));
            builder_.CreateStore(llvm::ConstantInt::get(i8Ty, 1),
                builder_.CreateStructGEP(stringType(), strObj, 2));
            initStringRc(strObj);
            // Free the raw key bytes from bz_dict_keys_str.
            builder_.CreateCall(freeFn, {kp});
            keyDecoded = strObj;
        } else if (useGen) {
            llvm::Value* kpPtr  = builder_.CreateGEP(ptrTy(), keysArr, idxB);
            llvm::Value* keyPtr = builder_.CreateLoad(ptrTy(), kpPtr);
            keyDecoded =
                builder_.CreateLoad(keyLlvm, keyPtr, "denum.kgen");
            // Free the malloc'd raw-bytes key copy.
            builder_.CreateCall(freeFn, {keyPtr});
        } else {
            keyDecoded = llvm::Constant::getNullValue(keyLlvm);
        }

        // --- decode value (mirrors lowerDictEntryFor) ---
        llvm::Value* vpPtr  = builder_.CreateGEP(ptrTy(), valsArr, idxB);
        llvm::Value* slotV  = builder_.CreateLoad(ptrTy(), vpPtr);
        llvm::Value* valDecoded;
        if (valRec) {
            valDecoded = builder_.CreateLoad(valLlvm, slotV, "denum.vrec");
        } else if (valNullableVal && valLlvm->isStructTy()) {
            valDecoded = builder_.CreateLoad(valLlvm, slotV, "denum.vnv");
        } else if (!valLlvm->isPointerTy()) {
            llvm::Value* asI64 = builder_.CreatePtrToInt(slotV, i64());
            if (valLlvm->isDoubleTy())
                valDecoded = builder_.CreateBitCast(asI64, valLlvm);
            else if (valLlvm->isFloatTy())
                valDecoded = builder_.CreateBitCast(
                    builder_.CreateTrunc(asI64, i32()), valLlvm);
            else if (valLlvm->isIntegerTy(64))
                valDecoded = asI64;
            else
                valDecoded = builder_.CreateTrunc(asI64, valLlvm);
        } else {
            valDecoded = slotV;
        }

        // Store key at field 0, value at field 1 and append.
        builder_.CreateStore(keyDecoded,
            builder_.CreateStructGEP(entryStTy, entrySlot, 0));
        builder_.CreateStore(valDecoded,
            builder_.CreateStructGEP(entryStTy, entrySlot, 1));
        builder_.CreateCall(addFn, {arr, entrySlot, builder_.getInt64(esz)});
        builder_.CreateStore(
            builder_.CreateAdd(idxB, builder_.getInt64(1)), idxSlot);
        builder_.CreateBr(condBB);
    }

    builder_.SetInsertPoint(endBB);
    builder_.CreateCall(freeFn, {keysArr});
    if (isStringKey) builder_.CreateCall(freeFn, {lensArr});
    builder_.CreateCall(freeFn, {valsArr});

    // Wrap the materialised entries as an OWNING vtable-backed IEnumerable
    // (slot-1 Dispose frees the snapshot; GetEnumerator clones over it). The
    // element type/stride come from iEnumTy's DictionaryEntry<K,V> arg, so
    // ensureArrayEnumMachinery builds the correct {K, V} struct layout.
    //
    // fix #2522 (v0.236, sibling of the Calls3 LINQ source residual): for
    // string-keyed dictionaries the rebuilt bz_string keys at struct offset 0
    // are ref-counted producers (initStringRc → rc=1) and need bz_string_release
    // when the snapshot is dropped. The plain owning wrapper's Dispose calls
    // bz_array_free(arr, null) — a null element disposer — and bz_array_free's
    // element-disposer signature is `void(*)(void*)` cast on each slot, which
    // only works for pointer-element arrays. The entries are {K, V} STRUCTS,
    // so the plain wrapper leaks every rebuilt key (one ~24-byte header + its
    // data buffer per entry, bounded — value-correct). wrapDictionaryEntryArrayAsIEnumerable
    // is a Dictionary-entry-specific owning wrapper whose Dispose walks the
    // array via bz_array_ptr_at and calls bz_string_release on each Key slot
    // at struct offset 0 before freeing the array.
    if (isStringKey) {
        return wrapDictionaryEntryArrayAsIEnumerable(arr, iEnumTy, entryStTy, esz);
    }
    return wrapOwnedArrayAsIEnumerable(arr, iEnumTy);
}


// #2522 (stdlib §4.8/§4.12; language spec §29.1, v0.236): OWNING wrapper for
// the materialised Dictionary<string,V> entry array. The plain
// wrapOwnedArrayAsIEnumerable releases the array via bz_array_free(arr, null)
// — a null element disposer, which is correct for pointer-element arrays
// only. The Dictionary<string,V> materialisation stores {bz_string*, V}
// STRUCTS inline (one malloc per entry, the bz_string at offset 0), so the
// plain wrapper never reaches the rebuilt string at struct offset 0 and
// every iteration leaks it (bounded, value-correct, but visible to LSan —
// ~24-byte header + the data buffer per entry).
//
// This wrapper mirrors wrapOwnedArrayAsIEnumerable's struct layout and
// vtable protocol (5-slot vtable: self/Dispose/MoveNext/Current/GetEnumerator)
// and reuses the plain array-enum machinery's MoveNext/Current (they
// operate on bz_array_t* length/element access and never touch element
// ownership). The only difference is the owning Dispose, which walks the
// array via bz_array_ptr_at(arr, i, esz) for i in [0, count), loads the
// bz_string pointer at struct offset 0 of each entry, and calls
// bz_string_release on it BEFORE bz_array_free(arr, null) frees the array.
// The wrapper is keyed by entry-layout-shape so multiple Dictionary<string,V>
// instantiations (with the same {K, V} struct shape) share the same Dispose
// and vtable globals.
//
// Per-element-type custom Dispose machinery, similar in spirit to how
// OwningArray<T> threads an element disposer; here the "array" holds
// fixed-layout structs so we walk them manually rather than via the
// per-slot void(*)(void*) hook.
//
// Non-string-keyed dictionaries (int/record/enum keys) DO NOT take this
// path — wrapDictionaryAsIEnumerable keeps the plain wrapOwnedArrayAsIEnumerable
// for them (the keys are inline bytes with no heap ownership, so the
// plain null-disposer free is correct).
llvm::Value* Emitter::wrapDictionaryEntryArrayAsIEnumerable(
        llvm::Value* arr,
        const sema::TypeRef* iEnumTy,
        llvm::StructType* entryStTy,
        int64_t esz) {
    // The plain array-enum machinery is keyed on the element TYPE — for
    // Dictionary<string,V> that is DictionaryEntry<string,V> (a struct),
    // so ensureArrayEnumMachinery(<entry struct type>) builds the same
    // {ptr vtable, ptr arr, i64 idx, i64 esz} wrapper layout used by
    // wrapOwnedArrayAsIEnumerable. MoveNext/Current from the plain
    // machinery are correct here (they only walk the array).
    ArrEnumMachinery m = ensureArrayEnumMachinery(entryStTy);
    llvm::StructType* stTy = m.stTy;

    // Element-name key matches ensureArrayEnumMachinery's per-element-type
    // scheme (it falls back to "f" for non-integer/non-pointer types —
    // including structs). Reusing the same prefix means MoveNext/Current
    // created by the plain machinery are looked up under the SAME name as
    // wrapOwnedArrayAsIEnumerable uses when it was invoked first (e.g. by
    // the foreach dictionary path), avoiding two parallel sets of identical
    // MoveNext/Current for the same entry type.
    std::string elemName = entryStTy->isIntegerTy()
        ? ("i" + std::to_string(entryStTy->getIntegerBitWidth()))
        : (entryStTy->isPointerTy() ? "ptr" : "f");
    std::string plainPrefix = "bz$arrenum$" + elemName + "$";
    std::string prefix = "bz$dictentryenumown$" + elemName + "$";

    // --- Owning Dispose ---
    //   for i in 0..count: bz_string_release(entry[i].Key)
    //   bz_array_free(arr, null); free(self)
    auto* dispFn = module_->getFunction(prefix + "Dispose");
    if (!dispFn) {
        dispFn = llvm::Function::Create(
            llvm::FunctionType::get(
                llvm::Type::getVoidTy(llvm_), {ptrTy()}, false),
            llvm::GlobalValue::InternalLinkage, prefix + "Dispose",
            module_.get());
        auto* bb = llvm::BasicBlock::Create(llvm_, "entry", dispFn);
        llvm::IRBuilder<> b(bb);
        llvm::Value* self = dispFn->arg_begin();
        llvm::Value* arrPtr = b.CreateLoad(
            ptrTy(), b.CreateStructGEP(stTy, self, 1));

        // count = bz_array_len(arr)
        auto* lenFn = module_->getOrInsertFunction(
            "bz_array_len",
            llvm::FunctionType::get(i64(), {ptrTy()}, false)).getCallee();
        llvm::Value* cnt = b.CreateCall(
            llvm::FunctionType::get(i64(), {ptrTy()}, false),
            lenFn, {arrPtr}, "de.disp.cnt");

        // Loop: for i = 0; i < count; i++.
        auto* idxSlot = b.CreateAlloca(i64(), nullptr, "de.disp.idx");
        b.CreateStore(b.getInt64(0), idxSlot);
        auto* condBB = llvm::BasicBlock::Create(llvm_, "de.disp.cond", dispFn);
        auto* bodyBB = llvm::BasicBlock::Create(llvm_, "de.disp.body", dispFn);
        auto* endBB  = llvm::BasicBlock::Create(llvm_, "de.disp.end",  dispFn);
        b.CreateBr(condBB);

        b.SetInsertPoint(condBB);
        llvm::Value* ci = b.CreateLoad(i64(), idxSlot, "de.disp.i");
        b.CreateCondBr(b.CreateICmpSLT(ci, cnt), bodyBB, endBB);

        b.SetInsertPoint(bodyBB);
        {
            // entryPtr = bz_array_ptr_at(arr, i, esz)
            auto* ptrAtFT = llvm::FunctionType::get(
                ptrTy(), {ptrTy(), i64(), i64()}, false);
            auto* ptrAtFn = module_->getOrInsertFunction(
                "bz_array_ptr_at", ptrAtFT).getCallee();
            llvm::Value* ci2 = b.CreateLoad(i64(), idxSlot, "de.disp.i2");
            llvm::Value* entryPtr = b.CreateCall(ptrAtFT, ptrAtFn,
                {arrPtr, ci2, b.getInt64(esz)}, "de.disp.ep");

            // strPtr = entryPtr->Key (field 0 of the {K, V} struct, K = bz_string*)
            llvm::Value* strPtr = b.CreateLoad(
                ptrTy(),
                b.CreateStructGEP(entryStTy, entryPtr, 0),
                "de.disp.k");

            // bz_string_release(strPtr) — null-safe (a null slot would have
            // leaked only as a stale sentinel; release is null-safe per §8.6).
            auto* relFn = module_->getOrInsertFunction(
                "bz_string_release",
                llvm::FunctionType::get(
                    llvm::Type::getVoidTy(llvm_), {ptrTy()}, false)).getCallee();
            b.CreateCall(
                llvm::FunctionType::get(
                    llvm::Type::getVoidTy(llvm_), {ptrTy()}, false),
                relFn, {strPtr});

            b.CreateStore(
                b.CreateAdd(ci2, b.getInt64(1)), idxSlot);
            b.CreateBr(condBB);
        }

        b.SetInsertPoint(endBB);
        // bz_array_free(arr, null) — the entries' string keys have been
        // released above; the array itself holds no further heap references
        // at this point (values are int / record / class pointer copies by
        // value into the slot — NOT retrained copies — so they need no
        // disposal here; the source dictionary still owns its values).
        auto* freeArrFn = module_->getOrInsertFunction(
            "bz_array_free", llvm::FunctionType::get(
                llvm::Type::getVoidTy(llvm_), {ptrTy(), ptrTy()}, false))
            .getCallee();
        b.CreateCall(
            llvm::FunctionType::get(
                llvm::Type::getVoidTy(llvm_), {ptrTy(), ptrTy()}, false),
            freeArrFn, {arrPtr, llvm::ConstantPointerNull::get(ptrTy())});

        // free(self)
        auto* freeFn = module_->getOrInsertFunction(
            "free", llvm::FunctionType::get(
                llvm::Type::getVoidTy(llvm_), {ptrTy()}, false)).getCallee();
        b.CreateCall(
            llvm::FunctionType::get(
                llvm::Type::getVoidTy(llvm_), {ptrTy()}, false),
            freeFn, {self});
        b.CreateRetVoid();
    }

    // --- Owning GetEnumerator ---
    // Same as wrapOwnedArrayAsIEnumerable: a fresh NON-owning clone over the
    // shared array (idx = -1, NON-owning vtable). The clone's Dispose frees
    // only its own struct; the source owns the array exactly once.
    auto* geFn = module_->getFunction(prefix + "GetEnumerator");
    if (!geFn) {
        geFn = llvm::Function::Create(
            llvm::FunctionType::get(ptrTy(), {ptrTy()}, false),
            llvm::GlobalValue::InternalLinkage, prefix + "GetEnumerator",
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

    // --- Owning vtable ---
    // [0]=self, [1]=owning Dispose, [2]=plain MoveNext, [3]=plain Current,
    // [4]=owning GetEnumerator. MoveNext/Current reuse the plain machinery
    // (the same shared bz$arrenum$struct$MoveNext/Current that wrapOwnedArrayAsIEnumerable
    // uses when the Dictionary entry type is a struct).
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
        {builder_.getInt64(1), builder_.getInt64(stSz)}, "dictentryenumown");
    builder_.CreateStore(vtGV, builder_.CreateStructGEP(stTy, en, 0));
    builder_.CreateStore(arr, builder_.CreateStructGEP(stTy, en, 1));
    builder_.CreateStore(builder_.getInt64(-1),
        builder_.CreateStructGEP(stTy, en, 2));
    builder_.CreateStore(builder_.getInt64(m.esz),
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
    // #2281 / §31.2: the expression-getter thunk is a separate function.
    // Preserve the caller's active try/finally frames across nested emission;
    // finallyBodyStack_ stays depth-aligned per §29.7.
    auto  savedFinally   = finallyStack_;
    auto  savedFinBodies = finallyBodyStack_;
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
    finallyBodyStack_.clear();
    unwindTarget_      = nullptr; propagateBB_ = nullptr;
    currentCaughtExc_  = nullptr; unwindScopeDepth_ = 0;
    diverged_          = false; genericSubst_.clear();

    llvm::Value* val = lowerExpr(*getterExpr);
    // fix #2761 (§8.6 "Boxing" / §23.2, v0.240): the THUNK face of an
    // expression-bodied getter (built for a §29.2 enumerator's `Current`) owes
    // the same box as the inlined read — its `ret` must carry the property's
    // declared representation, not the body's natural one.
    if (auto* tpd = dynamic_cast<const ast::PropertyDecl*>(ps->decl))
        if (tpd->type)
            val = boxForTarget(val, ctx_.resolvedType(*tpd->type), getterExpr);
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
    finallyStack_      = std::move(savedFinally);       // #2281 / §31.2
    finallyBodyStack_  = std::move(savedFinBodies);     // #2281 / §29.7
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
    const ast::Expr& target, llvm::Type* paramTy, bool isOut,
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
                // fix #2454 (§22.3/§2.8.2 clause 1/§2.8.3 rule 4, v0.235):
                // a THIRD fallback, lowerImportedPropertySetValue (#2400) —
                // for a PLAIN (non-virtual, non-interface) property
                // inherited or declared on an IMPORTED class, both of the
                // above fail (lowerVirtualPropertySet needs a vtable/itable
                // slot this shape has none of; lowerPropertySetValue needs
                // `pd`, an ast::PropertyDecl, which an imported
                // PropertySymbol never has), so a `ref`/`out` write-back to
                // such a property had no path at all — confirmed by probe:
                // a LOCAL method taking `ref v: int` called as
                // `Bump(ref imported.Prop)` hit this function's own
                // "internal: no setter" diagnostic even for a fully public,
                // unrestricted imported property (a §22.3-conformant
                // program the sema checks above correctly accept). Mirrors
                // #2400's own DIRECT (non-bracket) `:=` fix exactly, reusing
                // the SAME importedPropSetterFns_-backed function — no new
                // machinery.
                done = lowerVirtualPropertySet(*r.target, v) ||
                       lowerPropertySetValue(*r.target, v) ||
                       lowerImportedPropertySetValue(*r.target, v);
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
    // discharge block and the header comment. #2310 / §29.8 / §30.1 (#973a
    // residual): keep the source EXPRESSION alongside each slot so the same
    // "about to run" point can also null the FRAME capture slot of an `owned`
    // state-machine parameter — covering forms 3 (`M(owned p)`) and 6
    // (`new Ctor(owned p)`). // per §28.3, §29.8, §30.1
    std::vector<std::pair<llvm::AllocaInst*, const ast::Expr*>>
        transferredOwnedArgSlots;
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
                        transferredOwnedArgSlots.emplace_back(
                            s, a.value.get());
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
                (params[idx].modifier == "ref" || params[idx].modifier == "out")) {
                llvm::Type* paramTy = nullptr;
                if (params[idx].type)
                    if (const sema::TypeRef* pt =
                            ctx_.resolvedType(*params[idx].type))
                        paramTy = lowerTypeRef(pt);
                v = emitAccessorRefArgCopyIn(
                    *a.value, paramTy,
                    /*isOut=*/params[idx].modifier == "out", *accessorRefArgs);
            }
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
        // fix #2809 / §28.3 *Unbound owned temporaries* + §32.6 (v0.240): an FFI
        // buffer temporary minted directly in an ORDINARY-CALL argument position
        // — `len(CString.From("abc"))`, `new W(CString.From(s))`. #2728/#2733
        // wired this reclaim into the EXTERN argument arm
        // (LlvmBackend_Calls2.cpp), the bare-ExpressionStmt/DiscardStmt arms and
        // (#2734) `string.From`, but never into the general call path, so the
        // buffer simply leaked. §28.3: such a temporary "is disposed at the end
        // of the full expression in which it is produced, after every use of it
        // within that expression has completed" — which is exactly what
        // registerFfiExprTemp (establish at production, covering the unwind
        // edge) plus flushFfiExprTemps (discharge at the full-expression
        // boundary) implement, so the callee still borrows a LIVE buffer for the
        // whole call and the free runs once, after it.
        //
        // ⚠ Same predicate as the extern arm and for the same reason:
        // isFfiBufferFactoryCall admits ONLY a directly-written
        // CString.From/FromBytes / WString.From / RawBytes.From whose sema
        // result is the builtin head — bound to no name by construction, which
        // is §28.3's *unbound* case. A `using var`-bound argument is a plain
        // IdentifierExpr and never matches, so the §32.6 rule that the CALLER of
        // a bound CString keeps the disposal obligation is untouched.
        //
        // ⚠ An `owned` transfer is EXCLUDED: with `owned` the callee takes the
        // disposal obligation (§28.3 transfer), so reclaiming here would be a
        // double free. Skipping the registration leaves at worst a bounded leak
        // if the callee does not dispose it — the deliberate direction, and
        // today it DOES leak: the callee emits no reclaim for an `owned`
        // FFI-buffer parameter either. Filed as #2823, whose fix belongs on the
        // CALLEE side; these exclusions stay correct when it lands.
        if (a.value && !a.isDiscard && a.modifier != "owned" &&
            params[idx].modifier != "owned") {
            bool argIsRawBytes = false;
            if (isFfiBufferFactoryCall(*a.value, argIsRawBytes)) {
                // Null-initialized in the ENTRY block, not merely stored here:
                // the reclaim is reachable from the unwind edge of an argument
                // evaluated BEFORE this one and from a sibling branch that never
                // ran this store, neither of which this store dominates (#2733).
                auto* tmpSlot = entryAllocaNullPtr();
                builder_.CreateStore(v, tmpSlot);
                registerFfiExprTemp(tmpSlot, argIsRawBytes);
            }
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
            // with the identical disposal obligation. `wrapMade` is the wrap's
            // own report of "this call allocated a fresh wrapper the caller must
            // dispose"; it is true for the lazy-pipeline case (owning), the
            // dict-backed-set snapshot case (owning), and the Array/Stack/Queue/
            // LinkedList wrap cases (borrowing — slot-1 Dispose is `free(self)`,
            // the wrapper frame only, NEVER the shared backing). A CLASS-element
            // set takes the BORROWING array wrap and reports true here so the
            // wrapper frame is freed exactly once. fix #2298 / §28.9: before
            // this, only the OWNING cases registered the iterEnumDispose, so
            // every borrowing wrap leaked its 32-byte frame — silently unbounded
            // in loops. Now both shapes share the same scope-end disposal, and
            // the non-owning slot-1 Dispose (free(self) only, NEVER the shared
            // backing) preserves the source's own ownership.
            bool wrapMade = false;
            v = wrapAsIEnumerableForTarget(v, formalTy, a.value.get(),
                                           /*ownLazyPipeline=*/lazyArgWrapped,
                                           &wrapMade);
            if (wrapMade && v && v->getType()->isPointerTy() &&
                !disposeStack_.empty()) {
                llvm::AllocaInst* eslot = entryAlloca(v->getType());
                builder_.CreateStore(v, eslot);
                Disposal d{eslot, nullptr};
                d.iterEnumDispose = true;  // slot-1 Dispose: free wrapper frame
                                            // (owning cases also free drained
                                            // array; borrowing cases free only
                                            // the frame — never the shared
                                            // backing, exactly-once).
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
    for (auto& [slot, srcExpr] : transferredOwnedArgSlots) {
        if (!slot) continue;
        llvm::Type* ety = nullptr;
        llvm::Value* addr = boxedStorage(slot, ety);
        if (addr && ety && ety->isPointerTy())
            builder_.CreateStore(
                llvm::ConstantPointerNull::get(ptrTy()), addr);
        // #2310 / §29.8 / §30.1 (#973a residual): also null the FRAME capture
        // slot of an `owned` state-machine PARAMETER this call just consumed —
        // forms 3 (`M(owned p)`) and 6 (`new Ctor(owned p)`). Self-gating (no-op
        // outside a state-machine body, for non-parameter sources, for non-owned
        // parameters). per §29.8 / §30.1
        nullStateMachineOwnedParamCapture(srcExpr);
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
    // #2310 / §29.8 / §30.1 (#973a residual): when the source is an `owned`
    // state-machine PARAMETER, also null its FRAME capture slot — so the
    // frame's terminal-state disposal (null-guarded) skips the just-transferred
    // parameter on the discharging path. Covers every call site of this helper:
    // the `y := owned p` assignment marker (form 4) and the owning-collection
    // element transfer Add / Insert / Set (form 5). Self-gating — no-op outside
    // a state-machine body, for non-parameter sources, for non-owned parameters.
    // per §29.8 / §30.1
    nullStateMachineOwnedParamCapture(argValue);
}


}  // namespace bezel::codegen
