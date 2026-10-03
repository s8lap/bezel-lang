#include "codegen/LlvmBackend_impl.hpp"
#include "llvm/ADT/STLFunctionalExtras.h"  // llvm::function_ref (fix #4901)
#include "common/OwnershipFacts.hpp"  // fix #3189: calleeReturnsOwned — the M2 owned-return channel

namespace bezel::codegen {

// fix #4901: defined in LlvmBackend_Calls2.cpp beside the `.Subscribe`/`.Add`
// arms it also gates; shared with tryLowerEventCompound's `+=` route below.
bool sharedEventHandlerAdmissible(
        sema::SemaContext& ctx, const ast::Expr* handler,
        const ast::Expr* receiver, const sema::TypeRef* payload,
        llvm::function_ref<bool(std::string_view)> isBuiltinName,
        bool honourSelectedOverload);

namespace {
// §23.4 (#774): exact structural match between an indexer's first parameter
// type and the index argument's type — by primitive for value/string types and
// by declaration for named types. Used to pick the right overload; the
// checkTypeOverloads distinctness check (sema) guarantees at most one indexer
// matches any concrete argument type exactly. fix #2834 (§23.4 / §7.1,
// v0.240): compare `nullable` too — an `int?` argument and an `int`
// parameter are NOT the same type under §7.1, so the structural match must
// reject that mismatch, otherwise an exact full-list match reports a
// `Nullable` parameter and a `NonNull` parameter as the same shape and the
// resolver picks the wrong one (mirrors the identically-shaped fix in
// ExprTypecheck_Exprs.cpp's sema-side `allParamsMatch`).
bool indexerArgMatches(const sema::TypeRef* p, const sema::TypeRef* a) {
    if (!p || !a || p->kind != a->kind) return false;
    if (p->nullable != a->nullable) return false;
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
        // fix #3103 (§15.6, v0.255, range-step on slice): `arr[a..b step z]`
        // is a SILENT-DROP defect — the step expression was never read in this
        // lowering (only lo/hi/inclusive were), so the side effect of `z` was
        // skipped and the runtime zero-step fault of §16.4 never fired.  The
        // spec's slice form (§15.6) is stepless by definition: the slicing
        // operation is over a contiguous range. A `step` in a slice index
        // therefore has no meaning, mirroring §16.4's parallel restriction
        // "step is not legal in case patterns" (sema/ExprTypecheck.cpp —
        // those patterns test membership, and a stride has no meaning). The
        // constant-zero case is already rejected at sema by §16.4, so the
        // diagnostic here covers the rest: computed steps, including
        // side-effecting ones that the old lowering skipped entirely.
        if (re->step) {
            ctx_.diagnostics().report(
                Severity::Error, re->step->span.begin,
                "`step` is not legal in a slice index — `arr[a..b]` is a "
                "contiguous slice (§15.6) and has no stride; remove the "
                "`step z` clause (use a `for`-loop over the stepped range "
                "to walk a strided sub-range)");
        }
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
    // source string (a char), bounds-checked against the span's Length AND
    // against the SOURCE STRING's length (see below). The source (field 0) is a
    // string pointer, so the element is read via loadCodePoint — NOT
    // bz_array_get (the array-span path below). This must precede the
    // isSpanLikeTypeName branch, which would mis-treat the string source as an
    // array.
    if (recvTy && recvTy->kind == sema::TypeRef::Kind::Named &&
        !recvTy->declaration && recvTy->astNode && recvTy->astNode->name &&
        !recvTy->astNode->name->parts.empty() &&
        recvTy->headName() == "ReadOnlyStringSpan") {
        llvm::Value* spanVal = lowerExpr(*ix.target);
        if (!spanVal) return nullptr;
        llvm::Value* idx = lowerExpr(*ix.indices[0].value);
        if (!idx) return nullptr;
        // fix #2799 / §7.4: unwrap a §7.4-narrowed nullable index before the
        // trunc (same soundness story as the Array<string> indexer fix above;
        // checkIndexArgNullability already rejects the un-narrowed form).
        idx = unwrapNarrowedNullableOperand(idx, ix.indices[0].value.get());
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
        // fix #3098 (follow-up) / stdlib §3.3 + §4.14 (v0.195): bounds-check the
        // ABSOLUTE index against the SOURCE STRING, not only against the span's
        // own `Length` above.
        //
        // stdlib §4.14 "Construction patterns" is explicit that the direct span
        // constructors "do **not** range-check `(Start, Length)` against the
        // source (v0.195): a mis-constructed span faults at each out-of-range
        // *access* instead — the span-level check passes and **the source's own
        // bounds check throws `IndexOutOfRangeException`**". That composition is
        // what makes the no-eager-check rule memory-SAFE — and for this type it
        // did not exist: the span-level check above compares against `Length`
        // and then calls `loadCodePoint` DIRECTLY, and `loadCodePoint`
        // (LlvmBackend_String.cpp) performs no check of its own — its header
        // states its precondition outright, "idx < len is guaranteed by every
        // caller". The plain string indexer supplies exactly this guard
        // (LlvmBackend_Exprs.cpp, `ICmpUGE(idx64, loadStringLen(...))`, R33);
        // the span path bypassed it, so `new ReadOnlyStringSpan("abc", 0, 100)`
        // then `s[50]` read ~50 code points past the buffer. Adding the arm that
        // makes the direct constructor construct (#3098) is what made such a
        // span reachable, so the guard §4.14 promises is owed here.
        //
        // UNSIGNED compare, deliberately: it rejects a negative `absIdx` in the
        // same instruction. The span-level check above rejects a negative `idx`
        // but says nothing about `Start`, and a negative `Start` is the other
        // half of a mis-constructed span (`new ReadOnlyStringSpan("abc", -5, 10)`);
        // sign-extended, it is a huge unsigned value and fails this compare.
        // Mirrors the string indexer's own unsigned test. // per stdlib §3.3, §4.14
        emitGuardThrow(
            builder_.CreateICmpUGE(absIdx, loadStringLen(src), "ss.src.oor"),
            "IndexOutOfRangeException");
        return loadCodePoint(src, absIdx);   // i32 code point (char)
    }

    // §4.15 (#549): ArraySpan<T>[i] / ReadOnlyArraySpan<T>[i]
    // → Source[Start + i] with bounds-check against Length (not array Count).
    if (isSpanLikeTypeName(recvTy)) {
        llvm::Value* spanVal = lowerExpr(*ix.target);
        if (!spanVal) return nullptr;
        llvm::Value* idx = lowerExpr(*ix.indices[0].value);
        if (!idx) return nullptr;
        // fix #2799 / §7.4: unwrap a §7.4-narrowed nullable index before the
        // trunc (same soundness story as the ReadOnlyStringSpan fix above;
        // checkIndexArgNullability already rejects the un-narrowed form).
        idx = unwrapNarrowedNullableOperand(idx, ix.indices[0].value.get());
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
            // fix #2799 / §7.4: unwrap a §7.4-narrowed nullable index before
            // the sext (same soundness story as the Array<int> indexer fix
            // above; checkIndexArgNullability already rejects the un-narrowed
            // form). The Buffer<T> family is conjectured (issue §B) but the
            // unwrap is unconditional — the gate covers it.
            idx = unwrapNarrowedNullableOperand(idx, ix.indices[0].value.get());
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
        // fix #2799 / §7.4: a §7.4-flow-narrowed `int?` index arrives here
        // still wearing its `{i8 hasVal; i32 payload}` box (sema left the
        // expression's declared type as `int?` and recorded the narrowing in
        // checkAssignable — see #2614's docs). Unwrap the payload before the
        // sext; checkIndexArgNullability already rejects the un-narrowed form,
        // so this is sound. Twin of the string indexer's fix (#2799) and of
        // #2614's getIntArg helper.
        idx = unwrapNarrowedNullableOperand(idx, ix.indices[0].value.get());
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
    // fix #3976 (stdlib §4.17, v0.295): fix #3947 rejected a no-CompareTo
    // eligible key at `.ContainsKey`/`.Add`/etc (LlvmBackend_Calls.cpp's
    // isDict block) but this indexer GET is a SEPARATE call site that also
    // consults `genericBackingType` and was left silently admitting the
    // identical shape — `sd[k]` read via structural (§9.1/§9.2) identity
    // where §4.17 requires `CompareTo`-equality. Reuse the ONE authority
    // fix #3947 established rather than a third inline copy of the same
    // predicate.
    if (reportSortedElementNeedsCompareTo(recvTy, keyTy, ix.span.begin,
                                          "the `[key]` indexer")) {
        llvm::Type* resTy = i32();
        if (sema::TypeRef* rt = ctx_.exprType(ix))
            if (llvm::Type* lt = lowerTypeRef(rt)) resTy = lt;
        return llvm::PoisonValue::get(resTy);
    }
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
    // fix #4640 (§8.6 / stdlib §4.8, v0.307): §8.6-box a bare-value key against
    // an Object/interface-typed Dictionary slot. The runtime reads `key_size`
    // bytes from the key pointer (bz_dict_try_get_gen -> dict_find_gen ->
    // hash_fn/eq_fn, both `key_size`-byte memcpys); with a bare `int` key the
    // codegen lowered the value as i32, spilled it to a 4-byte stack slot, and
    // the runtime then read 12 B of stack redzone past it — MEASURED under
    // ASan as a stack-buffer-overflow in bz$valkey$hash at
    // bz_dict_try_get_gen. boxForTarget (LlvmBackend_Layout.cpp) mints a fresh
    // §8.6 box (24 B / 1 alloc for an int payload) and returns the box
    // pointer; the runtime then reads a 16-byte box (vtable, rc, payload)
    // align. boxForTarget is a no-op for a reference source (the pre-boxed
    // `var o: Object := 1;` spelling) and for a non-Object/interface key
    // (Dictionary<int, V> etc.), so every already-correct shape stays
    // byte-identical — control rows in
    // dict_object_bare_value_key_indexer_set_4640_test.cpp prove that.
    if (keyTy) {
        k = boxForTarget(k, keyTy, ix.indices[0].value.get());
    }
    auto* tmpSlot = entryAlloca(ptrTy());
    llvm::Value* found;
    if (keyTy) {
        // spill the key value to a slot (record keys arrive as an address).
        // fix #4640: a freshly-boxed key is already a pointer; the spill arm
        // is skipped for it (k->getType()->isPointerTy()) — the box pointer IS
        // the key slot's content.
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
        llvm::Value* kw = loadStringWidth(k);   // fix #4499 (§4.6/§8.3, v0.306)
        auto* fn = declareC("bz_dict_try_get_str", i1(),
                            {ptrTy(), ptrTy(), i64(), i8(), ptrTy()});
        found = builder_.CreateCall(fn, {recv, kd, kl, kw, tmpSlot});
        // fix #3703 / §28.9: a Dictionary<K,V> string KEY is copied into the
        // table BY VALUE (bz_dict_try_get_str reads kd/kl and never retains
        // the bz_string* itself — unlike the VALUE slot, which stores the
        // pointer and is released by the #429 mechanism). A fresh +1 KEY
        // producer ($"...", string concat, a case/if arm-meet) used only to
        // derive this call's kd/kl is never referenced again, so nothing else
        // will ever discharge its +1. Release it now: after the byte read (kd
        // is an alias into k's own buffer, so releasing any earlier would be a
        // use-after-free), before the guard-throw below so BOTH outcomes
        // (found / KeyNotFoundException) are covered.
        if (isPlusOneStringProducer(ix.indices[0].value.get()))
            emitStringRelease(k);
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
        // fix #2799 / §7.4: unwrap a §7.4-narrowed nullable index before the
        // trunc (same soundness story as the ArraySpan<T> indexer fix above;
        // checkIndexArgNullability already rejects the un-narrowed form).
        idx = unwrapNarrowedNullableOperand(idx, ix.indices[0].value.get());
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
        // fix #2799 / §7.4: unwrap a §7.4-narrowed nullable index before the
        // sext (same soundness story as the Array<T> indexer fix above;
        // checkIndexArgNullability already rejects the un-narrowed form).
        idx = unwrapNarrowedNullableOperand(idx, ix.indices[0].value.get());
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
    // fix #4640 (§8.6 / stdlib §4.8, v0.307): §8.6-box a bare-value key against
    // an Object/interface-typed Dictionary slot. Same defect as
    // lowerCollectionIndexerGet: bz_dict_try_get_gen reads `key_size` bytes
    // from the key pointer, so a bare-int key on a 4-byte stack slot is a
    // stack-buffer-overflow in bz$valkey$hash. See that site for the full
    // rationale; boxForTarget is a no-op for reference / non-Object keys.
    if (keyTy) {
        k = boxForTarget(k, keyTy, ix.indices[0].value.get());
    }
    auto* tmpSlot = entryAlloca(ptrTy());
    llvm::Value* found;
    llvm::Value* keyPtr = nullptr;     // generic-key path needs it again to null
    llvm::Value* kd = nullptr;         // string-key path needs it again to null
    llvm::Value* kl = nullptr;
    llvm::Value* kw = nullptr;         // fix #4499: storage width rides along
    if (keyTy) {
        keyPtr = k;
        // fix #4640: a freshly-boxed key is already a pointer; the spill arm
        // is skipped for it (k->getType()->isPointerTy()).
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
        kw = loadStringWidth(k);        // fix #4499
        auto* fn = declareC("bz_dict_try_get_str", i1(),
                            {ptrTy(), ptrTy(), i64(), i8(), ptrTy()});
        found = builder_.CreateCall(fn, {recv, kd, kl, kw, tmpSlot});
    }
    // §4.8/§4.9: a `move` of an absent key raises, same as the indexer get.
    // fix #3703 / §28.9: unlike lowerCollectionIndexerGet (a SINGLE read of
    // kd/kl, so a release placed before this guard covers both outcomes),
    // this function reads kd/kl a SECOND time below (the null-store), only on
    // the found path — so a release placed before the guard would be a
    // use-after-free of that second read. On the NOT-found (throw) edge, k's
    // one-and-only read here is already complete, so it is safe — and
    // necessary — to release a fresh +1 KEY producer on THAT edge instead
    // (else it leaks: the found-path release below never runs when we throw).
    // Manual branch instead of emitGuardThrow (mirrors the `.Add` owning-value
    // dispose-then-throw arm elsewhere in this file) ONLY when a release is
    // actually needed; every other case (non-string key, or a borrowed key)
    // keeps the plain emitGuardThrow, unchanged.
    if (!keyTy && isPlusOneStringProducer(ix.indices[0].value.get())) {
        llvm::Value* notFound = builder_.CreateNot(found);
        auto* errBB = llvm::BasicBlock::Create(llvm_, "mv.notfound", currentFn_);
        auto* foundBB = llvm::BasicBlock::Create(llvm_, "mv.found", currentFn_);
        builder_.CreateCondBr(notFound, errBB, foundBB);
        builder_.SetInsertPoint(errBB);
        emitStringRelease(k);   // the ONLY execution of this call's key release
        emitThrowBuiltin("KeyNotFoundException");
        builder_.SetInsertPoint(foundBB);
    } else {
        emitGuardThrow(builder_.CreateNot(found), "KeyNotFoundException");
    }
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
                            {ptrTy(), ptrTy(), i64(), i8(), ptrTy()});
        builder_.CreateCall(fn, {recv, kd, kl, kw, nullV});
        // fix #3703 / §28.9: kd/kl alias k's own buffer and are read TWICE in
        // this function (the try_get probe above and this null-store); only
        // now, after the last read, is it safe to discharge a fresh +1 KEY
        // producer without a use-after-free. See lowerCollectionIndexerGet.
        if (isPlusOneStringProducer(ix.indices[0].value.get()))
            emitStringRelease(k);
    }
    return moved;   // owned class object pointer, taken out of the slot
}


// fix #5084 (stdlib §4.8 / §5.1 "Nullable conversions", v0.307): shared
// promotion half of the dictionary-VALUE encode — see the declaration in
// LlvmBackend_impl.hpp for the full rationale. Lifts `v` to the nullable-
// value tagged `{i8,T}` struct when `valTy` is a nullable value type and `v`
// is not already that exact struct; returns `v` unchanged otherwise. This is
// verbatim the promotion block `lowerCollectionIndexerSet`'s `encodeValue`
// used to run inline (issue #544 / #5078), factored out so
// `materializeHeapDictLiteral`'s `encodeVal` (LlvmBackend_Setup.cpp) can call
// the SAME logic for a dictionary LITERAL's `["k": v]` entries instead of
// passing `v` straight through — the gap that let a literal `null` reach the
// runtime as a bare `ptr null` (Bug A) and let a literal non-null scalar
// (`["k": 99]` against `Dictionary<K, long?>`) reach the runtime as a raw,
// untagged inttoptr'd value that later crashed a read-back AND the disposing
// overwrite's `free()` of it (Bug B — MEASURED: both bugs share this one
// root cause; #5084's own "Bug B" repro's `d["k"] := null;` overwrite
// SIGABRTs freeing the bogus untagged `99` stored by the literal, not
// anything wrong with `bz_dict_set_str_disposing` itself).
llvm::Value* Emitter::liftDictValueToNullableStruct(
        llvm::Value* v, const sema::TypeRef* valTy,
        const ast::Expr* valueExpr) {
    if (!valTy || !isNullableValueTy(valTy)) return v;
    if (!v->getType()->isStructTy()) {
        if (v->getType()->isPointerTy() &&
            llvm::isa<llvm::ConstantPointerNull>(v)) {
            return nullableValueNull(valTy);   // null literal → {i8=0, T=0}
        }
        if (!v->getType()->isPointerTy()) {
            // fix #5005 (§5.1 `S → T → T?`, v0.307): widen a narrower numeric
            // `v` to the nullable's payload width BEFORE `wrapNullableValue`,
            // which otherwise hands an undersized value to its
            // `reinterpretToType` alloca round-trip.
            llvm::StructType* nst = nullableValueStructTy(valTy);
            llvm::Type* payloadTy = nst ? nst->getElementType(1) : nullptr;
            if (payloadTy &&
                (v->getType()->isIntegerTy() ||
                 v->getType()->isFloatingPointTy()) &&
                v->getType() != payloadTy)
                v = coerceNumericToType(
                    v, payloadTy, valueExpr && isUnsignedTargetExpr(*valueExpr));
            return wrapNullableValue(valTy, v);  // scalar T → {i8=1, T=v}
        }
        return v;
    }
    // fix #570 / §4.8 / §7 / §9: nullable RECORD value (Item?) — the caller
    // passed the inner record struct directly (e.g. `d["k"] := new Item(42)`
    // implicitly coerced to Item?): wrap into {i8=1, Item} before boxing so
    // decode always finds the full tagged struct. Detect by comparing against
    // the outer tagged struct.
    if (v->getType() != nullableValueStructTy(valTy)) {
        // fix #5078 (§5.1 "Nullable conversions", v0.307): `v` may instead
        // ALREADY be the tagged `{i8, S}` struct of a DIFFERENT nullable-
        // VALUE type — try the shared unwrap/convert/rewrap lift first (a
        // no-op unless `v` is itself the tagged shape) and fall back to the
        // inner-record wrap only when it declines.
        llvm::StructType* wantSt = nullableValueStructTy(valTy);
        llvm::Value* lifted = liftNullableValueToSlot(
            v, wantSt, valueExpr && isUnsignedTargetExpr(*valueExpr));
        return (lifted->getType() == wantSt)
                   ? lifted
                   : wrapNullableValue(valTy, v);  // inner record → {i8=1, record}
    }
    return v;
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
        // fix #2799 / §7.4: unwrap a §7.4-narrowed nullable index before the
        // trunc (same soundness story as the Span<…> indexer-get fix above;
        // checkIndexArgNullability already rejects the un-narrowed form).
        idx = unwrapNarrowedNullableOperand(idx, ix.indices[0].value.get());
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
            // fix #2799 / §7.4: unwrap a §7.4-narrowed nullable index before
            // the sext (same soundness story as the Buffer<T> indexer-get
            // fix above; checkIndexArgNullability already rejects the
            // un-narrowed form).
            idx = unwrapNarrowedNullableOperand(idx, ix.indices[0].value.get());
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
        // fix #2799 / §7.4: unwrap a §7.4-narrowed nullable index before the
        // sext (same soundness story as the Array<T> indexer-get fix above;
        // checkIndexArgNullability already rejects the un-narrowed form).
        idx = unwrapNarrowedNullableOperand(idx, ix.indices[0].value.get());
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
                    else {
                        // fix #5078 (§5.1 "Nullable conversions", v0.307):
                        // `val` may ALREADY be the tagged `{i8, S}` struct of
                        // a DIFFERENT nullable-value type — a nullable-VALUE
                        // RHS (`arr[0] := ki;` with `ki: int?`,
                        // `arr: Array<double?>`) lowers to `{i8, i32}` before
                        // it ever reaches here. The #5017 lift for this exact
                        // shape lives in lowerStmt's shared `elemTy`-based
                        // preamble (LlvmBackend_Stmts.cpp), but this
                        // indexer-setter arm is a SEPARATE `IndexExpr` branch
                        // that `return`s before reaching it, so a tagged-
                        // struct `val` reached the scalar-only #5005 widen
                        // below unconverted (not `isIntegerTy()` /
                        // `isFloatingPointTy()`) and then `wrapNullableValue`
                        // — whose `reinterpretToType` round-trip does not
                        // reshape a MISMATCHED struct — handed its
                        // `insertvalue` a `{i8, i32}` where a `double` payload
                        // was expected. MEASURED: "Invalid InsertValueInst
                        // operands!" / IR verification failed on
                        // `arr[0] := ki;` (`Array<double?>`, `ki: int?`), and
                        // identically for `??=`, a null source, and an
                        // unsigned (`uint?`) source. `liftNullableValueToSlot`
                        // is a no-op (returns `val` unchanged) unless `val` is
                        // ITSELF that tagged shape, so trying it first is safe
                        // before the bare-scalar #5005 path below, which still
                        // handles the non-tagged (`int`/`double`) RHS case.
                        llvm::Value* lifted = liftNullableValueToSlot(
                            val, expected,
                            valueExpr && isUnsignedTargetExpr(*valueExpr));
                        if (lifted->getType() == expected) {
                            val = lifted;
                        } else {
                        // fix #5005 (§5.1 `S → T → T?`, v0.307): a NARROWER
                        // numeric `val` (e.g. an `int` at an `Array<long?>`
                        // element, `a[i] := k;`) reached `wrapNullableValue`
                        // at its source width — `wrapNullableValue`'s
                        // `reinterpretToType` round-trips through an alloca
                        // sized to the DESTINATION (payload) type, so an
                        // undersized `i32` store there left the high bytes
                        // uninitialized and the subsequent `insertvalue`
                        // rejected the width mismatch outright: MEASURED
                        // "Invalid InsertValueInst operands!" / IR
                        // verification failure on `Array<long?>[i] := k;`
                        // with `k: int`. Widen against the payload width
                        // FIRST — the same `coerceNumericToType` widen the
                        // declaration-initializer path (fix #839) and the
                        // local/field reassignment path (fix #5005, above in
                        // LlvmBackend_Stmts.cpp) both perform — so
                        // `wrapNullableValue` always receives a value already
                        // at the payload's width.
                        llvm::Type* payloadTy = expected->getElementType(1);
                        if ((val->getType()->isIntegerTy() ||
                             val->getType()->isFloatingPointTy()) &&
                            val->getType() != payloadTy)
                            val = coerceNumericToType(
                                val, payloadTy,
                                valueExpr && isUnsignedTargetExpr(*valueExpr));
                        val = wrapNullableValue(elemTyRef, val);  // scalar → {1, val}
                        }
                    }
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
    // fix #3976 (stdlib §4.17, v0.295): the SET twin of the indexer-get check
    // above (`lowerCollectionIndexerGet`) — `sd[k] := v` is a SEPARATE call
    // site that also consults `genericBackingType` and silently admitted a
    // no-CompareTo eligible key via structural identity. Reject before any
    // codegen for this operation, matching #3947's own member-family
    // rejection; the caller's `val` was already lowered and is simply left
    // unused (the compile fails on this Error regardless).
    if (reportSortedElementNeedsCompareTo(recvTy, keyTy, ix.span.begin,
                                          "the `[key]` indexer"))
        return true;
    bool keyIsRecord = keyTy && recordTypeOfRef(keyTy) != nullptr;
    const sema::TypeRef* valTy =
        (recvTy && recvTy->genericArgs.size() > 1) ? recvTy->genericArgs[1]
                                                   : nullptr;
    // fix #3780: the boxing decision below no longer needs its own
    // record-vs-not classification (`valRec`) — it now reads
    // `dictValueIsBoxedStruct` (LlvmBackend_impl.hpp), which derives the
    // answer from the lowered LLVM type itself, so no local record symbol is
    // required here.
    // issue #539 / §4.8 / §7: nullable value types lower to a tagged {i8,T}
    // struct and must be heap-boxed the same way non-nullable records are.
    bool valNullableVal = valTy && isNullableValueTy(valTy);
    // fix #3974 / §9.7 / §28.9 / stdlib §4.8 (v0.295 follow-up): distinguish
    // borrow-source from +1-transfer-source at the insertion site so the
    // retain walker ONLY fires when the box's fields are not already +1
    // owners. The unconditional retain of the prior cycle 5 attempt
    // (commit 4356b91's gate was just `recordHasStringFields`) over-retained
    // the explicit-ctor + +1-arg case — `new Box($"...")` where the ctor's
    // `this.S := s` already routes through `lowerStringFieldStore`'s
    // retain-new / release-old and transfers the +1 to the field — and
    // broke #3798 (4 tests) + #3802 (4 tests). The fix is a per-source
    // gate: fire retain when the source's string field is provably a
    // BORROW (an IdentifierExpr / MemberAccessExpr arg, a fresh non-+1
    // call), skip it when the construction is a +1 transfer.
    const sema::TypeSymbol* valRec3974 =
        (valTy && valTy->kind == sema::TypeRef::Kind::Named &&
         valTy->declaration &&
         valTy->declaration->kind == ast::TypeDeclKind::Record)
            ? valTy->declaration : nullptr;
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
    // fix #5084 (stdlib §4.8 / §5.1, v0.307): this promotion now lives in the
    // shared `liftDictValueToNullableStruct` (declared in LlvmBackend_impl.hpp,
    // defined just above this function) so `materializeHeapDictLiteral`'s own
    // value encode (LlvmBackend_Setup.cpp) can reuse it verbatim instead of
    // re-deriving it — see that function's header comment for the bug this
    // sharing fixes.
    auto encodeValue = [&](llvm::Value* v) -> llvm::Value* {
        v = liftDictValueToNullableStruct(v, valTy, valueExpr);
        // fix #3780 (§4.8 / §28.9, v0.286): route the boxing decision through
        // the #3770/#3768 shared predicate instead of this arm's own
        // `(valRec || valNullableVal) && isStructTy()` copy — see
        // dictValueIsBoxedStruct's declaration in LlvmBackend_impl.hpp for why
        // `isStructTy()` alone is a strict widening (a struct-lowering V that
        // is neither a record nor a nullable value type, i.e. a `choice`, was
        // excluded here and fell through to the scalar-widening tail below,
        // which crashed the IR verifier trying to ZExt a struct value).
        if (dictValueIsBoxedStruct(v->getType())) {
            // Box the struct (record, nullable-value tagged struct, or choice).
            uint64_t vsz =
                module_->getDataLayout().getTypeAllocSize(v->getType());
            auto* mallocFn = declareC("malloc", ptrTy(), {i64()});
            llvm::Value* box =
                builder_.CreateCall(mallocFn, {builder_.getInt64((int64_t)vsz)});
            builder_.CreateStore(v, box);   // copy the struct bytes into the box
            // fix #3974 (§9.7 / §28.9 / stdlib §4.8, v0.295 follow-up): when
            // the source's string field is a BORROW (an IdentifierExpr /
            // MemberAccessExpr, a non-+1 call, etc.), make the box a genuine
            // +1 owner by retaining each reachable string field — symmetric
            // with `boxedRecordValueFreer` / `boxedChoiceValueFreer`'s
            // assumption (the freer drains those +1s at scope-end / Clear /
            // indexer-overwrite / Remove). Without this retain the box's
            // string-field copy is a BORROW of the source and the freer's
            // release frees a +1 the source still owns — heap-use-after-free
            // (issue repro: `new Box(s, 1)` where `s` is a plain local).
            //
            // Gating (`boxValueNeedsStringRetain`, fix #3974 v0.294, refined
            // v0.295): the predicate returns false (don't retain) when the
            // construction is provably a +1 transfer. For positional-field
            // records, `isFreshRecordConstructionOfExactType` recognises it
            // directly. For explicit-ctor records, `boxValueNeedsStringRetain`
            // is keyed on the NewExpr's args themselves: a literal / +1
            // rvalue arg to the string field's ctor parameter is treated as
            // +1 transfer (the ctor's `this.S := s` routes through
            // `lowerStringFieldStore`'s retain-on-non-producer path, giving
            // the field its own +1 — no box-level retain needed); an
            // IdentifierExpr / MemberAccessExpr arg is a borrow (the field
            // shares the source's +1), so the box needs its own retain.
            //
            // Nullable V (`Box?`, `Choice?`) is supported: GEP element 1 of
            // the {i8, T} wrapper, walk T's string fields unchanged. Same
            // adjustment the freer's release twin does.
            if (valTy) {
                llvm::Value* recAddr = box;
                if (valNullableVal) {
                    llvm::StructType* wrapTy = nullableValueStructTy(valTy);
                    recAddr = builder_.CreateStructGEP(
                        wrapTy, box, 1, "dvbxre.nrec.payload");
                }
                if (valRec3974 && recordHasStringFields(valTy) &&
                    boxValueNeedsStringRetain(valueExpr, valTy)) {
                    emitRecordStringFieldRetains_impl(recAddr, valTy);
                } else if (!valNullableVal && valTy->kind ==
                               sema::TypeRef::Kind::Named &&
                           valTy->declaration &&
                           valTy->declaration->kind ==
                               ast::TypeDeclKind::Choice &&
                           choiceHasStringFields(valTy->declaration) &&
                           boxValueNeedsStringRetain(valueExpr, valTy)) {
                    emitChoiceStringFieldRetains_impl(
                        recAddr, valTy->declaration);
                }
            }
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
    // fix #4640 (§8.6 / stdlib §4.8, v0.307): §8.6-box a bare-value key against
    // an Object/interface-typed Dictionary slot. The runtime reads `key_size`
    // bytes from the key pointer (bz_dict_set_gen -> memcpy(kc, key,
    // d->key_size)); with a bare `int` key the codegen lowered the value as
    // i32, spilled it to a 4-byte stack slot, and the runtime then read 12 B
    // of stack redzone past it — MEASURED under ASan as a stack-buffer-overflow
    // in bz$valkey$hash at bz_dict_set_gen:925. boxForTarget
    // (LlvmBackend_Layout.cpp) mints a fresh §8.6 box (24 B / 1 alloc for an
    // int payload) and returns the box pointer; the runtime then reads a
    // 16-byte box (vtable, rc, payload) align. boxForTarget is a no-op for a
    // reference source (the pre-boxed `var o: Object := 1;` spelling) and for
    // a non-Object/interface key (Dictionary<int, V> etc.), so every
    // already-correct shape stays byte-identical — control rows in
    // dict_object_bare_value_key_indexer_set_4640_test.cpp prove that.
    //
    // ⚠ MEASURED LEAK, deliberately not discharged. The runtime's
    // bz_dict_set_gen memcpys the 16 box bytes into a freshly malloc'd table
    // entry (kc) and frees its own copy; the original box allocation is then
    // unreferenced — 24 B / 1 alloc per call. Per the same trade #4619
    // declined for the Array<Object> membership-query residual (24 B per
    // call, same `Box<` spelling carries the same residual, runtime
    // box-vtable guard is the way out): recorded here as a decline-with-
    // reasons rather than taken silently.
    if (keyTy) {
        k = boxForTarget(k, keyTy, ix.indices[0].value.get());
    }
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
        // fix #4640: a freshly-boxed key is already a pointer (boxForTarget
        // returned the box pointer); the spill arm is skipped for it
        // (k->getType()->isPointerTy()) — the box pointer IS the key slot's
        // content. Records are still pointers and still skip the spill arm.
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
    llvm::Value* kw = loadStringWidth(k);   // fix #4499 (§4.6/§8.3, v0.306)
    if (valDisp) {
        auto* fn = declareC("bz_dict_set_str_disposing",
            llvm::Type::getVoidTy(llvm_),
            {ptrTy(), ptrTy(), i64(), i8(), ptrTy(), ptrTy()});
        builder_.CreateCall(fn, {recv, kd, kl, kw, vPtr, valDisp});
        // fix #1970 / §28.4 / §28.6: see the gen-key branch above — for an
        // OwningDictionary<K,V> with string key the value has just been
        // transferred; null the caller's slot for a bare owned local/param RHS.
        if (dictIsOwning2)
            nullTransferredOwnedLocalSlot(valueExpr);
    } else {
        auto* fn = declareC("bz_dict_set_str", llvm::Type::getVoidTy(llvm_),
                            {ptrTy(), ptrTy(), i64(), i8(), ptrTy()});
        builder_.CreateCall(fn, {recv, kd, kl, kw, vPtr});
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
    // fix #3703 / §28.9: a Dictionary<K,V> string KEY is copied into the
    // table BY VALUE (bz_dict_set_str/_disposing read kd/kl and strndup the
    // bytes — they never retain the bz_string* itself, unlike the VALUE
    // slot). kd/kl are computed once, immediately above, with a single use
    // in either sub-branch, so releasing a fresh +1 KEY producer here (after
    // both branches have already consumed kd/kl) is safe and covers both.
    if (isPlusOneStringProducer(ix.indices[0].value.get()))
        emitStringRelease(k);
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
    // fix #3231 (§12.3 / §23.4 / §2.8.2, v0.252): the widened
    // emitExportedIndexerGetters / -Setters now define
    // `bz$<Class>$idxget$<arity>` / `$idxset$<arity>` for VIRTUAL
    // indexers too, and declareImportedIndexerGetters / -Setters
    // populate `importedIndexerGetterDirectFns_` /
    // `importedIndexerSetterDirectFns_` with the OWNER's direct entry.
    // The base-rooted call site picks that up before this function is
    // reached (or, for callers that consult this function as a gate,
    // finds the map non-empty and lowers cleanly). Only when the
    // OWNER-class export was erased (an ABSTRACT virtual member whose
    // wrapper had no body to wrap) does this function still reject —
    // and that is the §12.5-correct behaviour.
    if (!importedIndexerGetterDirectFns_.count(ps) &&
        !importedIndexerSetterDirectFns_.count(ps)) {
        ctx_.diagnostics().report(
            Severity::Error, ix.span.begin,
            "cannot lower `base[...]` against the imported virtual indexer "
            "on `" +
                std::string(ps->parent ? ps->parent->name : "") +
                "` — §12.3 requires the base class's own implementation, "
                "but the cross-module indexer ABI exposes no direct entry "
                "for an ABSTRACT virtual member (§12.3/§23.4/§2.8.2) — "
                "#2837");
        return true;
    }
    return false;
}

// fix #2837 (§12.3 / §23.4 / §2.8.2 / §26.5, v0.282): the closing pin for
// the cross-module `base[i]` reject. #2832's `rejectImportedVirtualBaseIndexer`
// above is the diagnostic side — it fires ONLY when the OWNER-direct entry
// map is empty (the ABSTRACT virtual shape, which §12.5-correctly has no
// OWNER body to call from `base[i]` either way). The remaining half is the
// call-side route in `lowerIndexerGet` / `lowerIndexerSet` /
// `lowerIndexerCompoundCore` (this file, lines ~1737 / ~2454 / ~3168
// respectively) that dispatches `base[i]` through the OWNER's direct entry
// — `importedIndexerGetterDirectFns_[ps]` and
// `importedIndexerSetterDirectFns_[ps]` — when the OWNER body exists.
//
// The structural ABI addition lives in `LlvmBackend_Setup.cpp`:
//   * `emitExportedIndexerGetters` / `-Setters` were widened (#3231) to
//     emit ExternalLinkage `bz$<Class>$idxget$<arity>` / `$idxset$<arity>`
//     wrappers for VIRTUAL, non-override, non-abstract indexers too — the
//     OWNER-class body, the same direct-entry ABI the non-virtual case
//     uses; and
//   * `declareImportedIndexerGetters` / `-Setters` declare the matching
//     external in four new direct-entry maps. `indexTargetIsBase(ix)` is
//     the §12.3 predicate that picks this route OVER the vtable-dispatch
//     wrapper (`importedIndexerGetterFns_[ps]`, #2545).
//
// The three call-site guards (lowerIndexerGet / -Set / -CompoundCore) are
// the SAME shape: `indexTargetIsBase(ix) && isVirtualIndexer(ps) &&
// <direct-entry-map>.count(ps)`. The reject below is the
// last-resort fallback that fires when no OWNER-direct entry exists —
// only ever the ABSTRACT virtual case.
//
// fix #3199 (§12.3 / §23 / §2.8.2, v0.254) — the named-property twin of
// `rejectImportedVirtualBaseIndexer` (#2832) immediately above. See the header
// for the contract and the ABI mechanism.
//
// `base.P` on an IMPORTED VIRTUAL base property:
//   * an IMPORTED property has no ast::PropertyDecl (#2414: ModuleLoader
//     reconstructs no AST), so every accessor call must go through
//     `importedPropGetterFns_` / `-SetterFns_`; and
//   * for a VIRTUAL imported property what sits in those maps is nothing:
//     `emitExportedPropertyGetters`/`-Setters` (LlvmBackend_Setup.cpp) skip
//     a virtual property ("Skip virtual/override/abstract/static — they have
//     other dispatch paths"), so the exporter emits no
//     `bz$<Class>$<Prop>$propget` symbol for it at all
//     (confirmed by `nm` on a lib object: lowercase `t`, InternalLinkage).
//     `declareImportedPropertyGetters`/`-Setters` correspondingly skip it
//     (`if (propGetSlot_.count(ps)) continue;`).
//
// Calling the vtable-slot accessor for `base.P` would run the OVERRIDE
// (the imported virtual accessor dispatches through the vtable, and the
// derived override shares that slot — the same shape #2832 rejects for the
// indexer form). MEASURED cross-module on HEAD: `base.P` returned -1 (the
// #2491 zero/null degradation) where 8 is correct — no compile error, only
// an `internal:` diagnostic that does not fail the build; the WRITE
// half (`base.P := 7`) silently dropped, leaving `_v` untouched
// (measured exit 3 where 7 is correct).
//
// Report instead of emitting it: a clean compile error beats a silent wrong
// value. The ABI addition that would make it work is the property analog of
// #2837's fix direction — out of scope for this interim reject, which matches
// the indexer twin's own interim (#2832) exactly.
//
// The imported NON-virtual case is untouched and correct (measured through
// `base.P`, `this.P`, and a derived override of a non-virtual imported base
// property): its entry point IS the direct `bz$…$propget` stub that
// `emitExportedPropertyGetters` defines and `declareImportedPropertyGetters`
// declares.
bool Emitter::rejectImportedVirtualBaseProperty(
    const ast::MemberAccessExpr& ma, const sema::PropertySymbol* ps) {
    if (!ps) return false;
    if (dynamic_cast<const ast::PropertyDecl*>(ps->decl)) return false;
    auto* id = dynamic_cast<const ast::IdentifierExpr*>(ma.target.get());
    if (!id || id->name != "base") return false;
    if (!isVirtualProperty(ps)) return false;
    // fix #3231 (§12.3 / §23 / §2.8.2, v0.252): the widened
    // emitExportedPropertyGetters / -Setters now define
    // `bz$<Class>$<Prop>$propget` / `$propset` for VIRTUAL members too, and
    // declareImportedPropertyGetters / -Setters populate the
    // `importedPropGetterDirectFns_` / `importedPropSetterDirectFns_` maps
    // with the OWNER's direct entry. The base-rooted call site picks that
    // up before this function is reached (or, for callers that consult
    // this function as a gate, finds the map non-empty and lowers cleanly).
    // Only when the OWNER-class export was erased (an ABSTRACT virtual
    // member whose wrapper had no body to wrap) does this function still
    // reject — and that is the §12.5-correct behaviour: an abstract base
    // accessor has no implementation to call from `base.P` either way.
    if (!importedPropGetterDirectFns_.count(ps) &&
        !importedPropSetterDirectFns_.count(ps)) {
        ctx_.diagnostics().report(
            Severity::Error, ma.span.begin,
            "cannot lower `base." + std::string(ps->name) +
                "` against the imported virtual property on `" +
                std::string(ps->parent ? ps->parent->name : "") +
                "` — §12.3 requires the base class's own implementation, "
                "but the cross-module property ABI exposes no direct entry "
                "for an ABSTRACT virtual member "
                "(§12.3/§23/§2.8.2) — #3199");
        return true;
    }
    return false;
}

llvm::Value* Emitter::lowerIndexerGet(const ast::IndexExpr& ix) {
    if (!ix.target || ix.indices.empty() || !ix.indices[0].value)
        return nullptr;
    const sema::TypeSymbol* recvDecl = indexAccessReceiverDecl(ix);
    if (!recvDecl) return nullptr;
    auto* ps = selectIndexer(recvDecl, ix);  // per §23.4 (#1172)
    if (!ps) return nullptr;
    // fix #3231 (§12.3 / §23.4 / §2.8.2, v0.252): `base[i]` on an IMPORTED
    // VIRTUAL base indexer now ROUTES through the OWNER's direct entry —
    // `importedIndexerGetterDirectFns_[ps]`, populated by
    // declareImportedIndexerGetters with the symbol the widened
    // emitExportedIndexerGetters defines for VIRTUAL indexers. The check
    // runs BEFORE `rejectImportedVirtualBaseIndexer` (which now only fires
    // for the ABSTRACT virtual shape — §12.5-correct, since no OWNER
    // body exists to call from `base[i]` either way).
    if (indexTargetIsBase(ix) && isVirtualIndexer(ps) &&
        importedIndexerGetterDirectFns_.count(ps)) {
        llvm::Function* fn = importedIndexerGetterDirectFns_.find(ps)->second;
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
        // fix #2816 / §28.3 "Unbound owned temporaries" (v0.240): establish the
        // temporary's disposal AT ITS PRODUCTION — before the INDEX ARGUMENTS
        // are lowered and before the accessor runs — so a throw from either
        // unwinds through it. Discharged by the reclaim below.
        llvm::AllocaInst* recvUnwindSlot = registerFreshNewReceiverTempUnwind(
            ix.target.get(), obj, ctx_.exprType(ix),
            /*calleeReturnsOwned=*/false);
        if (!obj->getType()->isPointerTy()) {
            if (!obj->getType()->isAggregateType()) return nullptr;
            llvm::AllocaInst* tmp = entryAlloca(obj->getType());
            builder_.CreateStore(obj, tmp);
            obj = tmp;
        }
        // fix #3353 (§14.1 / §22.7 / §23.4, v0.240): for a DERIVED
        // receiver (`g: GD: GBase<Pt>`) accessing an inherited indexer on
        // a GENERIC base (`property [i]: T => _v` on `GBase<T>`), the
        // LIB's `bz$<GBase>$idxget$<arity>` stub reads only the first 4
        // bytes of the substituted `_v: T` slot (the stub's body was
        // lowered against `classStruct(GBase)`'s T→i32 placeholder).
        // Detect this case and substitute a per-(ps, recvTy)
        // InternalLinkage wrapper that GEPs the inherited slot under the
        // importer's own T→Pt substitution. Pre-condition: the body must
        // be a single field read on the inherited type-parameter slot —
        // the helper returns nullptr otherwise and we fall through to
        // the original stub (which still produces wrong values for any
        // non-trivial body shape; that limitation is acknowledged in
        // #3353). See emitImportedIndexerGetterInheritedWrapper's header.
        llvm::Function* inhWrapper = nullptr;
        if (sema::TypeRef* recvTy = ctx_.exprType(*ix.target)) {
            inhWrapper = emitImportedIndexerGetterInheritedWrapper(ps, recvTy);
        }
        if (inhWrapper) fn = inhWrapper;
        // CRITICAL: fetch fnTy AFTER the wrapper swap. The wrapper has a
        // DIFFERENT return type (the substituted T, e.g. %record.Pt, not
        // the stub's i32 placeholder), so the call site must read fnTy
        // off the actual `fn` we are calling — otherwise CreateCall
        // interprets the call as returning i32 and the substituted value
        // (an 8-byte Pt struct) is read as only its low 4 bytes.
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
                                        /*calleeReturnsOwned=*/false,
                                        recvUnwindSlot);
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
        // fix #2816 / §28.3 (v0.240): establish the temporary's disposal at its
        // production — before the index arguments and the dispatched accessor.
        llvm::AllocaInst* recvUnwindSlot = registerFreshNewReceiverTempUnwind(
            ix.target.get(), obj, ctx_.exprType(ix),
            /*calleeReturnsOwned=*/false);
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
                                        /*calleeReturnsOwned=*/false,
                                        recvUnwindSlot);
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
                // fix #2816 / §28.3 (v0.240): establish the temporary's disposal
                // at its production — before the index arguments and the
                // vtable-dispatched accessor.
                llvm::AllocaInst* recvUnwindSlot =
                    registerFreshNewReceiverTempUnwind(
                        ix.target.get(), obj, ctx_.exprType(ix),
                        /*calleeReturnsOwned=*/false);
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
                                                /*calleeReturnsOwned=*/false,
                                                recvUnwindSlot);
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
    // §23.4 / §21.7: the body's `return` must terminate only the getter,
    // and the (possibly record/aggregate) result flows back by value through the
    // function's declared return type — exactly as a record-returning method
    // does. Inlining the block would (a) discard the value and (b) ret out of
    // the enclosing method with the wrong type.
    if (getterBlock) {
        llvm::Value* obj = lowerExpr(*ix.target);
        if (!obj) return nullptr;
        // fix #2816 / §28.3 (v0.240): establish the temporary's disposal at its
        // production — before the index arguments and the standalone getter.
        llvm::AllocaInst* recvUnwindSlot = registerFreshNewReceiverTempUnwind(
            ix.target.get(), obj, ctx_.exprType(ix),
            /*calleeReturnsOwned=*/false);
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
        // fix #4973 (§14.1 / §23.4 / §2.8.3 rule 4, v0.307): the BLOCK-FORM
        // INDEXER GETTER twin of the property-block-getter fix on
        // 4973-getter-draft (commit 9dcc51d7) and the setter fix on the same
        // branch (1c6a95c5). Mirror the expression-bodied indexer pattern at
        // LlvmBackend_Assign.cpp:4284 (`setupReceiverGenericSubst`). The
        // block-bodied indexer call was MISSED for the same reason as the
        // block-form property getter: the body lowers as a STANDALONE function
        // — `ensureIndexerBlockGetter` builds the signature from
        // `lowerType(pd->type.get())` which falls back to `i32()` when no subst
        // is active, so an inherited indexer on a generic base reached through
        // a non-generic derived receiver (`e[i]` where `e: E`, `E: B<T>`,
        // `[i]: T` declared on `B<T>`) reads only the first 4 bytes of the
        // substituted `_v: T` field (where T=string is 8 bytes). Install
        // `computeAncestorGenericSubst` BEFORE the call; save+restore keeps
        // the subst from leaking past this read into the enclosing expression.
        // Same gate as the getter fix: explicit receiver (IndexExpr target is
        // always explicit for `[i]`), Named type, declaration != ps->parent,
        // declaring class has genericParams.
        const sema::TypeRef* idxRecvTy4973 = ctx_.exprType(*ix.target);
        std::unordered_map<std::string_view, llvm::Type*> savedIdxSubst;
        std::unordered_map<std::string_view, const sema::TypeRef*> savedIdxSema;
        bool idxSubstInstalled = false;
        if (idxRecvTy4973 &&
            idxRecvTy4973->kind == sema::TypeRef::Kind::Named &&
            idxRecvTy4973->declaration &&
            idxRecvTy4973->declaration != ps->parent &&
            !ps->parent->genericParams.empty()) {
            std::unordered_map<std::string_view, llvm::Type*> ancSubst;
            std::unordered_map<std::string_view, const sema::TypeRef*> ancSema;
            if (computeAncestorGenericSubst(idxRecvTy4973, ps->parent,
                                            ancSubst, ancSema)) {
                savedIdxSubst = std::move(genericSubst_);
                savedIdxSema = std::move(genericSubstSemaTy_);
                genericSubst_ = std::move(ancSubst);
                genericSubstSemaTy_ = std::move(ancSema);
                idxSubstInstalled = true;
            }
        }
        llvm::Function* fn = ensureIndexerBlockGetter(pd, getterBlock, ps);
        llvm::Value* res =
            builder_.CreateCall(fn->getFunctionType(), fn, callArgs);
        // fix #4973 restore: drop the ancestor subst installed above before the
        // indexer block-getter build/call.
        if (idxSubstInstalled) {
            genericSubst_ = std::move(savedIdxSubst);
            genericSubstSemaTy_ = std::move(savedIdxSema);
        }
        // §31.1/§31.2: a throw inside the standalone getter leaves the pending
        // exception set and returns; route it to the enclosing unwind target,
        // exactly as an ordinary method call does.
        emitPendingCheck();
        // fix #2717: after emitPendingCheck, so the release sits on the normal
        // path only — on a throw edge the temporary leaks, which is #429's own
        // (`!diverged_`) discipline and the safe direction.
        releaseIndexArgStringProducers(ix, ps, recvDecl, idxArgVals,
                                       /*accessorResultEscapes=*/true);
        // fix #4078 (§28.14 / §27.5 / §28.9, v0.294): the closure-arg twin of
        // the string release above. The standalone getter body never stored the
        // callback (§23.5 forbids ref/out/owned on indexer params; the body is
        // verified by MethodOwnership::callBoundedScan), so the producer's +1
        // is the caller's to drop on the normal-return edge. See the
        // expression-bodied indexer-getter twin's call site below for the same
        // pattern. `diverged_` is already false here — the release sits past
        // emitPendingCheck on the post-call straight-line.
        releaseIndexArgClosureProducers(ix, ps, idxArgVals);
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
                                        /*calleeReturnsOwned=*/false,
                                        recvUnwindSlot);
        return res;
    }

    // --- Expression-bodied getter: inline the body against `this`+params. ---
    // Bind `this` to the target object and indexer params to the args.
    llvm::Value* obj = lowerExpr(*ix.target);
    if (!obj) return nullptr;
    // fix #2816 / §28.3 (v0.240): establish the temporary's disposal at its
    // production — before the index arguments (lowered just below, in the
    // CALLER's scope) and before the inlined getter body, either of which may
    // throw. Discharged by the reclaim at the end of this function.
    llvm::AllocaInst* recvUnwindSlot = registerFreshNewReceiverTempUnwind(
        ix.target.get(), obj, ctx_.exprType(ix), /*calleeReturnsOwned=*/false);
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
    // per §11.8's strategy latitude (this implementation monomorphizes; proposal 0103 records the choice): a generic indexer
    // `property [i: int]: T` on `Box<Pair>` returns a `Pair`, not the erased
    // `T`. Install the receiver's substitution so a `T`-typed field read inside
    // the getter body (`=> _v`) loads the concrete record (§9: a record is a
    // value type), not its i32 erasure (which would truncate the multi-field
    // record to one word).
    auto savedSubstPair = setupReceiverGenericSubst(ix.target.get(), ps->parent);
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
    // fix #3356 / §29.2 / §23.4 (v0.278): the INDEXER spelling of the
    // EXPRESSION-BODIED PROPERTY GETTER's IEnumerable-family wrap. The
    // indexer inline body (`property [i: int]: IEnumerable<T> => coll;`)
    // has no RETURN edge the BLOCK-BODIED §29.2 wrap (#2703's
    // `wrapAsIEnumerableForTarget` at `lowerReturn`) can fire on, so
    // the wrap must live at the read site — exactly where the property
    // GETTER inline path (`wrapPropertyArrayAsIEnumerable`,
    // LlvmBackend_Exprs.cpp ~5596) already calls it. Pre-fix the
    // indexer read returned the raw backing pointer of the source
    // collection — a `bz_llist_t*` / `bz_dict_t*` / user-class
    // descriptor whose first word is the backing's count field, NOT a
    // vtable pointer — and the caller's `for x in obj[i]` drove §29.2
    // vtable dispatch on the raw descriptor, jumped through vtable
    // slot 4 (GetEnumerator, NULL) and SIGSEGV'd (`pc 0x0`, exit 139).
    // The PROPERTY GETTER inline path (#2777/#2815/#3355) fixed the
    // same defect for the non-indexer accessor by adding this exact
    // call there; the indexer shape (takes an `[i]` parameter list,
    // inlines its body against `this`+indexer-params rather than just
    // `this`) is structurally distinct — the wrap was never mirrored.
    // `wrapPropertyArrayAsIEnumerable` is the existing helper (its
    // `headName() == "IEnumerable"` / `"IOwningEnumerable"` gate and
    // its dispatch to `wrapAsIEnumerableForTarget` for the non-Array
    // family — LinkedList, Set, user-class IEnumerable — are exactly
    // what the indexer body needs); composing it AFTER `boxForTarget`
    // (a value-typed source is boxed before the IEnumerable wrap, so
    // a `property [i: int]: IEnumerable<Object>` shape still wraps
    // correctly) and AFTER `wrapNullableGetterResult` (the two are
    // disjoint: a nullable value type never reaches a pure
    // IEnumerable target) keeps the order the property inline path
    // established. Done BEFORE `releaseIndexArgStringProducers`
    // because that helper concerns the INDEX ARGUMENTS, never the
    // result. Done BEFORE the receiver's generic-substitution restore
    // at line ~2244 so a `T`-typed body under monomorphisation wraps
    // against the substituted `T` rather than the erased placeholder
    // (#1279); `wrapPropertyArrayAsIEnumerable` is type-blind on its
    // third argument (it only reads the declared `declTy`'s
    // headName), so the substitution does not reach the helper, but
    // the position parity with the property inline path is what makes
    // a future spec change to that helper apply uniformly.
    result = wrapPropertyArrayAsIEnumerable(result, ctx_.exprType(ix),
                                            getterExpr, &ix);
    // fix #2717: release a `+1` string producer index temporary now the getter
    // body has read it. The helper's element-type gate is what makes this safe
    // on the GETTER side: an inlined `get => k;` lowers to a bare load of the
    // index (no minting return), so a `string`-typed indexer is excluded and
    // `result` can never alias what this releases.
    releaseIndexArgStringProducers(ix, ps, recvDecl, argVals,
                                   /*accessorResultEscapes=*/true);
    // fix #4078 (§28.14 / §27.5 / §28.9, v0.294): the closure-arg twin of the
    // string release above. A lambda literal or method reference passed as an
    // INDEX argument is a +1 closure producer; §23.5 forbids `ref`/`out`/`owned`
    // on indexer parameters, so the parameter is empty-modifier and therefore
    // call-bounded (the getter body provably holds no reference to the cell
    // once the call returns). Drop the producer's +1 on the normal-return edge
    // here — guarded on `!diverged_` (the inline body threw) so the release is
    // not emitted into unreachable IR. The unwind edge's leak is the safe
    // direction; an exact mirror of the §28.14 release already established in
    // the method-call path (LlvmBackend_Calls2.cpp ~11000) and in the
    // function-value call path (#4078's own other half).
    if (!diverged_) releaseIndexArgClosureProducers(ix, ps, argVals);
    currentThis_ = savedThis;
    locals_ = std::move(savedLocals);
    paramSlots_ = std::move(savedParams);
    genericSubst_ = std::move(savedSubstPair.subst);
    genericSubstSemaTy_ = std::move(savedSubstPair.semaTy);
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
                                    /*calleeReturnsOwned=*/false,
                                    recvUnwindSlot);
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

// fix #4078 (§28.14 / §27.5 / §28.9, v0.294): the closure-arg twin of
// `releaseIndexArgStringProducers` (immediately above). Drops the producer's
// +1 on every +1 closure-producer index argument after the indexer getter has
// consumed it.
//
// WHY A RELEASE IS OWED. The same §28.14 / §23.5 / #429 logic the string
// release cites: §23.5 forbids `ref`/`out`/`owned` on indexer parameters, so
// an index argument is always an unmodified by-value borrow — the getter body
// never consumes it — and a producer temporary (`box[lambda() => ...]`) is
// bound to no local, so no scope-end release covers it. The §28.14
// call-boundedness guarantee runs on the getter body: `MethodOwnership::callBoundedScan`
// (or its function-value equivalent) rejects every sink, so the producer's +1
// is provably the sole remaining reference once the getter returns.
//
// WHY IT CANNOT OVER-RELEASE. The predicate is `isPlusOneClosureProducer`, the
// same predicate #429 / #1073 / #3498 use. It admits ONLY a LambdaExpr literal
// or a bare/qualified method reference (the codegen-internal +1 producers); a
// CALLER-OWNED LOCAL passed as an index (`var g := lambda() => …; t[g];`) is
// NOT matched — its +1 belongs to the local's own scope, and an extra release
// here would be a double-free (the local's scope-end release runs and frees
// again). Mirrors `releaseIndexArgStringProducers`'s identifier-exclusion
// exactly.
//
// GATED ON `!diverged_` (already enforced by every call site that names this
// helper): on a path the getter body diverged from (a `throw`), the release
// would be in unreachable IR. The unwind edge's leak is the safe direction —
// an exact mirror of #429's `!diverged_` discipline at line 7834.
//
// RELEASE TARGET. `emitClosureRelease` (defined in LlvmBackend_Disposal.cpp)
// is null-safe and decrements the rc atomically; a spurious double release on
// a transferred closure would be caught as a use-after-free by the bz_closure_
// release atomic sub_fetch returning > 0.
void Emitter::releaseIndexArgClosureProducers(
        const ast::IndexExpr& ix, const sema::PropertySymbol* ps,
        const std::vector<llvm::Value*>& argVals) {
    if (!ps) return;
    for (std::size_t k = 0; k < ps->indexerParams.size() &&
         k < ix.indices.size() && k < argVals.size(); ++k) {
        const ast::Expr* ie = ix.indices[k].value.get();
        if (!ie || !argVals[k] || !argVals[k]->getType()->isPointerTy())
            continue;
        // §22.3: the same PINNED index exclusion the string release applies
        // — a `ref`/`out` copy-in/copy-out bracket owns the index's lifetime
        // across both getter and setter halves, and releasing here would
        // double-free it. The indexer parameter list itself forbids `ref`/`out`
        // per §23.5, so exprValueOverride_ is the only way this arm fires.
        if (exprValueOverride_.count(ie)) continue;
        if (!isPlusOneClosureProducer(ie)) continue;
        emitClosureRelease(argVals[k]);  // null-safe; atomic rc dec
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
    // fix #3231 (§12.3 / §23.4 / §2.8.2, v0.252): the `base[i] := v`
    // half of the widening. §12.3 bypasses virtual dispatch and writes
    // the OWNER-class slot — the direct entry in
    // `importedIndexerSetterDirectFns_[ps]` (populated by
    // declareImportedIndexerSetters, defined by the widened
    // emitExportedIndexerSetters) is the right call. Runs BEFORE
    // `rejectImportedVirtualBaseIndexer`, which now only fires for the
    // ABSTRACT virtual shape.
    if (indexTargetIsBase(ix) && isVirtualIndexer(ps) &&
        importedIndexerSetterDirectFns_.count(ps)) {
        llvm::Function* fn = importedIndexerSetterDirectFns_.find(ps)->second;
        llvm::Value* obj = lowerExpr(*ix.target);
        if (!obj) return false;
        if (!obj->getType()->isPointerTy()) {
            if (!obj->getType()->isAggregateType()) return false;
            llvm::AllocaInst* tmp = entryAlloca(obj->getType());
            builder_.CreateStore(obj, tmp);
            obj = tmp;
        }
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
        if (val) {
            llvm::Type* valTy = fnTy->getParamType(fnTy->getNumParams() - 1);
            val = coerceInitializerToSlot(val, valTy,
                                          isUnsignedTargetExpr(ix));
            val = coerceToNullableSlot(valTy, val);
            val = boxForTarget(val, ctx_.exprType(ix), valueExpr);
            callArgs.push_back(val);
        }
        builder_.CreateCall(fnTy, fn, callArgs);
        emitPendingCheck();
        return true;
    }
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
        // fix #3353 setter twin (§14.1 / §22.7 / §23.4, v0.240): mirror the
        // getter branch — substitute a per-(ps, recvTy) InternalLinkage
        // wrapper when the receiver is a derived class with a substitution
        // for the inherited generic base's T parameter. Same pre-condition
        // and same fallback semantics as the getter: the wrapper is emitted
        // only when the setter's `value` parameter is the inherited
        // type-parameter T and the body is a trivial single-field write.
        llvm::Function* inhWrapper = nullptr;
        if (sema::TypeRef* recvTy = ctx_.exprType(*ix.target)) {
            inhWrapper = emitImportedIndexerSetterInheritedWrapper(ps, recvTy);
        }
        if (inhWrapper) fn = inhWrapper;
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
        // fix #2859 (§7.2/§5.1/§2.8.2/§23.4, v0.240): the IMPORTED-indexer
        // setter twin of `lowerVirtualPropertySet`'s #2927 fix. The setter
        // slot is §7.2's tagged `{i8 hasValue; T}` struct for a nullable VALUE
        // element, but the RHS arrives here as the bare payload `T`. The bare
        // `coerceNumericToType` has no case for that struct, and a nullable
        // value assignment `r[i] := 4` would emit a malformed IR call and fail
        // verification. Mirror the property setters' #2927/#2937 pair: first
        // slot-width adjustment (#2903, §5.1 in both directions, with the
        // nullable slot's PAYLOAD as the width to match), then §7.2's
        // present-wrap (which also normalizes a `null` literal to the all-zero
        // form). Both helpers are no-ops when `val` already IS the slot type,
        // so the callers that arrive with a tagged value (the §22.3
        // `ref`/`out` write-back, #2862's compound path) are unaffected.
        val = coerceInitializerToSlot(val, valTy, /*srcUnsigned=*/false);
        val = coerceToNullableSlot(valTy, val);
        callArgs.push_back(val);
        builder_.CreateCall(fnTy, fn, callArgs);
        emitPendingCheck();
        releaseIndexArgStringProducers(ix, ps, recvDecl, idxArgVals,
                                       /*accessorResultEscapes=*/false);
        // fix #4078 (§28.14 / §27.5 / §28.9, v0.294): the SETTER twin of the
        // GETTER-side `releaseIndexArgClosureProducers` call wired in by
        // #4078's first half. §23.5 forbids `ref`/`out`/`owned` on indexer
        // parameters so the imported setter receives a plain by-value borrow
        // and never consumes the callback; once it returns the producer's +1
        // is the caller's to drop on the normal-return edge (the setter body
        // is verified call-bounded by `MethodOwnership::callBoundedScan`).
        // Gated on `!diverged_` (mirror #429's `!diverged_` discipline at line
        // 7834) so the release is not emitted into unreachable IR after a
        // setter that threw. The unwind edge's caller is responsible for the
        // throw path — leaks on the unwind edge are the safe direction.
        if (!diverged_) releaseIndexArgClosureProducers(ix, ps, idxArgVals);
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
        // fix #2859 (§7.2/§5.1/§2.8.2/§23.4, v0.240): the INTERFACE-indexer
        // setter twin of #2927. valTy is the interface-indexer's element type,
        // which is §7.2's tagged `{i8 hasValue; T}` struct for a nullable value
        // element. The bare RHS arrives as the bare payload `T`; same pair
        // (#2903 + #813) — see the imported-setter twin's rationale directly
        // above for the full citation chain.
        val = coerceInitializerToSlot(val, valTy, /*srcUnsigned=*/false);
        val = coerceToNullableSlot(valTy, val);
        callArgs.push_back(val);
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
                // fix #2859 (§7.2/§5.1/§2.8.2/§23.4, v0.240): the CLASS-
                // VTABLE-indexer setter twin of #2927. valTy reads
                // `pd->type.get()` straight, so a nullable VALUE element
                // (property [k]: int?) reaches the call as the tagged struct
                // and `r[i] := 4` lands with bare-i32 where struct is required
                // — same IR verification abort as the property case. Same
                // pair (#2903 + #813) — see the imported-setter twin's
                // rationale directly above for the full citation chain.
                val = coerceInitializerToSlot(val, valTy, /*srcUnsigned=*/false);
                val = coerceToNullableSlot(valTy, val);
                callArgs.push_back(val);
                builder_.CreateCall(sFnTy, fnPtr, callArgs);
                emitPendingCheck();
                releaseIndexArgStringProducers(ix, ps, recvDecl, idxArgVals,
                                               /*accessorResultEscapes=*/false);
                // fix #4078 (§28.14 / §27.5 / §28.9, v0.294): the SETTER twin
                // of the GETTER-side closure release wired in by #4078's first
                // half. The class-vtable setter path here dispatches through
                // `vt[propSetSlot_]`, which is always a class's OWN slot (a
                // vtable slot is never record-backed — §9), so the setter
                // receives a plain by-value borrow of the closure (§23.5
                // forbids `ref`/`out`/`owned` on indexer parameters) and the
                // producer's +1 is the caller's to drop on the normal-return
                // edge. Gated on `!diverged_` (mirror #429's `!diverged_`
                // discipline at line 7834) so the release is not emitted into
                // unreachable IR after a setter that threw. Leaks on the
                // unwind edge are the safe direction.
                if (!diverged_) releaseIndexArgClosureProducers(ix, ps, idxArgVals);
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
    auto savedSubstPair = setupReceiverGenericSubst(ix.target.get(), ps->parent);
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
        // §23.3 / §23.5 (#1236): mirror lowerPropertySetValue (§21.7 / #739) —
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
    // fix #4078 (§28.14 / §27.5 / §28.9, v0.294): the SETTER twin of the
    // GETTER-side `releaseIndexArgClosureProducers` call wired in by #4078's
    // first half. A lambda literal or method reference passed as an INDEX
    // argument is a +1 closure producer; §23.5 forbids `ref`/`out`/`owned`
    // on indexer parameters, so the parameter is empty-modifier and therefore
    // call-bounded (the setter body provably holds no reference to the cell
    // once the call returns). Drop the producer's +1 on the normal-return
    // edge here — guarded on `!diverged_` (the inline body threw) so the
    // release is not emitted into unreachable IR. The unwind edge's leak is
    // the safe direction; an exact mirror of the §28.14 release already
    // established in the method-call path (LlvmBackend_Calls2.cpp ~11000),
    // in the function-value call path (#4078's own other half), and in the
    // GETTER twin's expression-bodied and block-form paths above.
    if (!diverged_) releaseIndexArgClosureProducers(ix, ps, argVals);
    currentThis_ = savedThis;
    locals_ = std::move(savedLocals);
    paramSlots_ = std::move(savedParams);
    genericSubst_ = std::move(savedSubstPair.subst);
    genericSubstSemaTy_ = std::move(savedSubstPair.semaTy);
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
    std::string op(as.op);
    if (!op.empty() && op.back() == '=') op.pop_back();
    return lowerIndexerCompoundCore(*as.target, op, as.value.get());
}


// fix #2848 (§17.6/§6.6/§12.1/§23.4, v0.240): `a[i]++` / `a[i]--` — §17.6's
// indexer desugaring, which that section states verbatim "is the indexer
// instance of the general compound-assignment rule of §6.6". Routed through
// the SAME body as `a[i] += 1` rather than a second hand-written copy of it;
// see the declaration in LlvmBackend_impl.hpp for what the copy had drifted
// out of sync with. §17.6, verbatim: "For built-in numeric types, `x++` is
// equivalent to `x := x + 1` (and likewise `x--` to `x := x - 1`)".
bool Emitter::lowerIndexerIncrDecr(const ast::IncrementStmt& inc) {
    if (!inc.target) return false;
    return lowerIndexerCompoundCore(*inc.target,
                                    inc.op == "++" ? "+" : "-",
                                    /*valueExpr=*/nullptr);
}


bool Emitter::lowerIndexerCompoundCore(const ast::Expr& targetExpr,
                                       const std::string& binOp,
                                       const ast::Expr* valueExpr) {
    auto* ix = dynamic_cast<const ast::IndexExpr*>(&targetExpr);
    if (!ix || !ix->target || ix->indices.empty() || !ix->indices[0].value)
        return false;
    // fix #2848: §17.6's right operand for the `++`/`--` spelling — the
    // implicit literal `1`, synthesized at the GETTER's result type (`cur`)
    // rather than lowered from an AST node, because the desugaring has none.
    // fix #2888: from the ONE definition of that rule (emitImplicitOne), which
    // also covers a `decimal` element (a struct-typed primitive) and a NULLABLE
    // element (`b[0]++` where the indexer yields `long?` — this copy returned
    // null for it, and the whole lowering then declined with an internal
    // "#1386" error on a legal program). A type with no `1` still yields null,
    // which declines rather than emitting a malformed constant.
    auto lowerRhs = [&](llvm::Value* cur) -> llvm::Value* {
        if (valueExpr) return lowerExpr(*valueExpr);
        if (!cur) return nullptr;
        return emitImplicitOne(cur->getType());
    };
    // §6.2/#2140's `rUns` (is the RIGHT operand unsigned?) — §17.6's implicit
    // `1` is a signed literal, so `false` for the `++`/`--` spelling, which is
    // exactly what the pre-fix mirror passed (it omitted the argument).
    const bool rhsUnsigned = valueExpr && isUnsignedTargetExpr(*valueExpr);
    // fix #2904 (§25.1/§25.2/§6.6, v0.240): the operand SEMA types the §25
    // overload lookup selects from. Every one of this function's three
    // compound-value sites (imported indexer, the two dynamic-dispatch
    // branches, the local block-getter path) called `emitCompoundOp` directly,
    // so a RECORD-typed indexer element with a user `operator <op>` aborted the
    // compile — measured on c7404bf, `b[0] += one` ICEd with "Intrinsic has
    // incorrect return type! ptr @llvm.sadd.with.overflow.s_record.Pairs".
    // The `++`/`--` spelling has no RHS node and so finds no overload, which is
    // correct: §17.6's identification of `x++` with `x := x + 1` is scoped
    // "For built-in numeric types".
    const sema::TypeRef* opLeftTy = ctx_.exprType(targetExpr);
    const sema::TypeRef* opRightTy =
        valueExpr ? ctx_.exprType(*valueExpr) : nullptr;
    // The ICE report below needs a source position; the `++`/`--` spelling has
    // no RHS node, so it points at the target.
    const auto rhsSpanBegin =
        valueExpr ? valueExpr->span.begin : targetExpr.span.begin;
    // Check that this is a user-defined indexer (class/record with a property
    // [i]: T declaration). Built-in arrays fall through to lvalueAddr instead.
    const sema::TypeSymbol* recvDecl = indexAccessReceiverDecl(*ix);
    if (!recvDecl) return false;
    auto* ps = selectIndexer(recvDecl, *ix);  // per §23.4 (#1172)
    if (!ps) return false;
    // fix #3231 (§12.3 / §23.4 / §2.8.2, v0.252): the COMPOUND half of
    // the indexer widening. `base[i] OP= v` (§12.3 bypasses virtual
    // dispatch) reads AND writes through the OWNER's body directly, so
    // dispatch through `importedIndexerGetterDirectFns_[ps]` for the read
    // and `importedIndexerSetterDirectFns_[ps]` for the write — both
    // populated by declareImportedIndexerGetters / -Setters, both
    // pointing at the symbol the widened emitters define. Runs BEFORE
    // `rejectImportedVirtualBaseIndexer`, which now only fires for the
    // ABSTRACT virtual shape.
    if (indexTargetIsBase(*ix) && isVirtualIndexer(ps) &&
        importedIndexerGetterDirectFns_.count(ps) &&
        importedIndexerSetterDirectFns_.count(ps)) {
        llvm::Function* getFn =
            importedIndexerGetterDirectFns_.find(ps)->second;
        llvm::Function* setFn =
            importedIndexerSetterDirectFns_.find(ps)->second;
        llvm::Value* obj = lowerExpr(*ix->target);
        if (!obj) return false;
        if (!obj->getType()->isPointerTy()) {
            if (!obj->getType()->isAggregateType()) return false;
            llvm::AllocaInst* tmp = entryAlloca(obj->getType());
            builder_.CreateStore(obj, tmp);
            obj = tmp;
        }
        auto* getFnTy = getFn->getFunctionType();
        std::vector<llvm::AllocaInst*> idxSlots;
        std::vector<llvm::Value*> idxArgVals;
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
        llvm::Value* rhs = lowerRhs(cur);
        if (!rhs) return false;
        llvm::Value* newVal = emitCompoundOpTyped(
            binOp, cur, rhs, opLeftTy, opRightTy,
            cur->getType()->isFloatingPointTy(),
            isUnsignedTargetExpr(targetExpr),
            isCharTargetExpr(targetExpr), rhsUnsigned);
        if (!newVal) return false;
        auto* setFnTy = setFn->getFunctionType();
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
        newVal = coerceInitializerToSlot(newVal, setFnTy->getParamType(
            setFnTy->getNumParams() - 1), isUnsignedTargetExpr(targetExpr));
        newVal = coerceToNullableSlot(
            setFnTy->getParamType(setFnTy->getNumParams() - 1), newVal);
        newVal = boxForTarget(newVal, ctx_.exprType(targetExpr), valueExpr);
        setArgs.push_back(newVal);
        builder_.CreateCall(setFnTy, setFn, setArgs);
        emitPendingCheck();
        return true;
    }
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
        llvm::Value* rhs = lowerRhs(cur);
        if (!rhs) {
            // #1386/#2509: ICE — lowerExpr returned null for the RHS.
            ctx_.diagnostics().report(
                Severity::Error, rhsSpanBegin,
                "internal: lowerExpr returned null for compound indexer "
                "assignment RHS (imported-indexer path) — #2509");
            return false;
        }
        llvm::Value* newVal = emitCompoundOpTyped(
            binOp, cur, rhs, opLeftTy, opRightTy,   // §25.1/§25.2 #2904
            cur->getType()->isFloatingPointTy(),
            isUnsignedTargetExpr(targetExpr),
            isCharTargetExpr(targetExpr),      // §6.6/§5.2 #1961
            rhsUnsigned);                      // §6.2 #2140 rUns
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
        // fix #2859 (§7.2/§5.1/§2.8.2/§23.4/§6.6, v0.240): the IMPORTED-
        // INDEXER compound-assign twin of #2927. The setter's `value`
        // parameter is the SAME tagged struct the plain `:=` setter expects,
        // but `newVal` is the §7.11/emitCompoundOp result, which is the bare
        // payload for a nullable value element (the lifting arm wraps the
        // RHS, not the LHS-to-set slot). Same pair — see the plain setter
        // twin's rationale directly above for the citation chain.
        newVal = coerceInitializerToSlot(newVal, valTy, /*srcUnsigned=*/false);
        newVal = coerceToNullableSlot(valTy, newVal);
        setArgs.push_back(newVal);
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
        llvm::Value* rhs = lowerRhs(cur);
        if (!rhs) {
            // #1386 precedent: ICE rather than a silently dropped store.
            ctx_.diagnostics().report(
                Severity::Error, rhsSpanBegin,
                "internal: lowerExpr returned null for compound indexer "
                "assignment RHS (dynamic-dispatch path) — #2836");
            return nullptr;
        }
        rhsOut = rhs;
        return emitCompoundOpTyped(
            binOp, cur, rhs, opLeftTy, opRightTy,   // §25.1/§25.2 #2904
            cur->getType()->isFloatingPointTy(),
            isUnsignedTargetExpr(targetExpr),
            isCharTargetExpr(targetExpr),      // §6.6/§5.2 #1961
            rhsUnsigned);                      // §6.2 #2140 rUns
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
        // fix #2848: `valueExpr` is null for `++`/`--`, whose right operand is
        // §17.6's implicit literal `1` — never a producer, so nothing to
        // release (and no AST node to test).
        if (rhs && rhs->getType()->isPointerTy() && valueExpr &&
            isPlusOneStringProducer(valueExpr))
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
        // fix #2859 (§7.2/§5.1/§2.8.2/§23.4/§6.6, v0.240): the INTERFACE-
        // INDEXER compound-assign twin of #2927. elemTy is the interface
        // indexer's element type — a §7.2 tagged struct for a nullable value
        // element, with the bare payload as `newVal`'s type. Same pair as the
        // plain-setter twin — see that block's comment for the chain. The
        // RECORDED SCOPE LIMIT's UAF release reasoning still applies
        // unchanged: the wrap is slot-shape only and affects only the LAST
        // call argument.
        newVal = coerceInitializerToSlot(newVal, elemTy, /*srcUnsigned=*/false);
        newVal = coerceToNullableSlot(elemTy, newVal);
        setArgs.push_back(newVal);
        builder_.CreateCall(sFnTy, setPtr, setArgs);
        emitPendingCheck();
        // fix #2847 (§23.4 / §6.6 / §9.7, v0.240): the INTERFACE-itable
        // compound-assign twin of the class-vtable branch's
        // `releaseCompoundProducers(newVal, rhs, /*classBacked=*/true)` below
        // (this file, line ~3623 — #2567's own structure for the same
        // desugaring's +1s on a class-backed path). The runtime implementor
        // behind an interface may be RECORD-backed, whose setter PLAIN-STORES
        // (§9/§9.7 — a record's reference fields are borrows stored plainly,
        // no disposer), so releasing newVal on the record path would drop the
        // field's only reference (UAF). Distinguish class- vs record-backed at
        // runtime through lowerIndexerSet's #2506/#2535 owner-kind flag —
        // itab[ifaceItabSlotCount_[ps->parent]] (NULL = record plain-store,
        // non-NULL = class retaining setter), which that function already reads
        // here (its own `idxset.class` / `idxset.record` block, ~line 2700).
        // Mirror that block one-for-one: on classBB release newVal's doubled
        // +1 (the emitCompoundOp/emitStringConcat result, rc=1 per #422; the
        // setter's retain bumped it to 2) and, when the compound RHS is itself
        // a +1 string producer AST (a BinaryExpr / interpolation / call —
        // emitStringConcat memcpy's rhs's bytes and leaves rhs's +1
        // untouched, so the rhs's strand has to be dropped separately), drop
        // rhs's +1 too; on recordBB emit no release — the record path's leak
        // is the safe direction this issue records rather than papers over,
        // exactly as #2548's own "Only sound on a CLASS-backed store" note
        // scopes `releaseCompoundProducers`. The flag is absent for an
        // interface whose itable has no owner-kind word; fall through (no
        // release) in that case — the pre-fix behaviour, also the safe
        // direction. Mirrors `lowerIndexerSet`'s interface branch's classBB
        // `emitProducerRelease` (this file, line ~2736) — `lowerIndexerSet`
        // already discharges the same `val` doubling on the class side; this
        // site is the COMPOUND-TWIN release that site is the plain-set twin
        // of. The new `releaseIndexArgStringProducers` early-returns for a
        // non-Class recvDecl (`Interface`, here), so the index-arg producers
        // (the `idxArgVals`) are not touched here — the pre-fix posture, also
        // unchanged by this fix.
        auto cntIt = ifaceItabSlotCount_.find(ps->parent);
        if (cntIt != ifaceItabSlotCount_.end() && !diverged_) {
            // The owner-kind flag lives at itab[ifaceItabSlotCount_[iface]]
            // (the LAST word in the itable, appended by #2506). Read it HERE
            // from the `itab` we already have in scope (the same value
            // computed for the fn-ptr load above; do not reload it
            // post-call — the receiver has been mutated by the setter and
            // reload could race).
            llvm::Value* flagAddr = builder_.CreateGEP(
                ptrTy(), itab,
                {builder_.getInt32(cntIt->second)}, "idxget.flagaddr");
            llvm::Value* flag = builder_.CreateLoad(
                ptrTy(), flagAddr, "idxget.flagval");
            llvm::Value* isClassBacked = builder_.CreateICmpNE(
                flag, llvm::ConstantPointerNull::get(ptrTy()),
                "idxget.isclass");
            llvm::BasicBlock* curBB = builder_.GetInsertBlock();
            llvm::Function* fn = curBB->getParent();
            llvm::BasicBlock* classBB =
                llvm::BasicBlock::Create(llvm_, "idxset.class", fn);
            llvm::BasicBlock* recordBB =
                llvm::BasicBlock::Create(llvm_, "idxset.record", fn);
            llvm::BasicBlock* doneBB =
                llvm::BasicBlock::Create(llvm_, "idxset.done", fn);
            builder_.CreateCondBr(isClassBacked, classBB, recordBB);
            // CLASS-backed branch: setter retained → release the desugaring's
            // two +1s. Mirrors the class-vtable compound branch's
            // `releaseCompoundProducers(newVal, rhs, /*classBacked=*/true)`
            // one-for-one (this file, line ~3623).
            builder_.SetInsertPoint(classBB);
            if (newVal && newVal->getType()->isPointerTy())
                emitStringRelease(newVal);   // emitStringConcat's rc=1 (#422)
            // RHS is a +1 producer only when the AST is a BinaryExpr /
            // interpolation / call (isPlusOneStringProducer); a literal RHS
            // is a sentinel (no +1), a named-local RHS is a borrow (no +1
            // owned by the compound). Gate on the AST shape, not the LLVM
            // pointer type, mirroring the inline-body release at line ~3874.
            if (rhs && rhs->getType()->isPointerTy() && valueExpr &&
                isPlusOneStringProducer(valueExpr))
                emitStringRelease(rhs);
            builder_.CreateBr(doneBB);
            // RECORD-backed branch: setter plain-stored → field aliases
            // newVal's heap (the field IS newVal's only reference). Emit
            // NO release — dropping newVal here would drop the field's
            // only reference (UAF, exactly the bug the flag is here to
            // prevent). The bounded leak this leaves behind on the record
            // path is the issue's recorded scope, not an oversight.
            builder_.SetInsertPoint(recordBB);
            builder_.CreateBr(doneBB);
            builder_.SetInsertPoint(doneBB);
        }
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
                // fix #2859 (§7.2/§5.1/§2.8.2/§23.4/§6.6, v0.240): the
                // CLASS-VTABLE-indexer compound twin of #2927. elemTy reads
                // `pd->type.get()` straight, so a nullable VALUE element (a
                // tagged struct) hits the same IR-verification abort as the
                // plain setter. Same helper pair — see that fix's block
                // comment for the citation chain. The setter retains (the
                // class-gated path), so the §7.11 lifting arm's wrapped
                // result is what we hand to the call.
                newVal = coerceInitializerToSlot(newVal, elemTy, /*srcUnsigned=*/false);
                newVal = coerceToNullableSlot(elemTy, newVal);
                setArgs.push_back(newVal);
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
        // fix #4973 (§14.1 / §23.4 / §2.8.3 rule 4, v0.307): the COMPOUND
        // INDEXER GETTER twin of the property-block-getter fix on
        // 4973-getter-draft (9dcc51d7) and the setter fix on the same branch
        // (1c6a95c5). The expression-bodied twin at line 4284 already uses
        // `setupReceiverGenericSubst` (the #585/#902/#3347 mechanism); this
        // block-bodied twin was MISSED for the same reason as the setter and
        // the read indexer: the body lowers as a STANDALONE function whose
        // signature is built from `lowerType(pd->type.get())` (falls back to
        // `i32()` without an active subst), so an inherited indexer on a
        // generic base reached through a non-generic derived receiver reads
        // only the first 4 bytes of the substituted `_v: T` field. Install
        // `computeAncestorGenericSubst` BEFORE the build; save+restore keeps
        // the subst from leaking past this read into the compound-op dispatch.
        const sema::TypeRef* cidxRecvTy4973 = ctx_.exprType(*ix->target);
        std::unordered_map<std::string_view, llvm::Type*> savedCidxSubst;
        std::unordered_map<std::string_view, const sema::TypeRef*> savedCidxSema;
        bool cidxSubstInstalled = false;
        if (cidxRecvTy4973 &&
            cidxRecvTy4973->kind == sema::TypeRef::Kind::Named &&
            cidxRecvTy4973->declaration &&
            cidxRecvTy4973->declaration != ps->parent &&
            !ps->parent->genericParams.empty()) {
            std::unordered_map<std::string_view, llvm::Type*> ancSubst;
            std::unordered_map<std::string_view, const sema::TypeRef*> ancSema;
            if (computeAncestorGenericSubst(cidxRecvTy4973, ps->parent,
                                            ancSubst, ancSema)) {
                savedCidxSubst = std::move(genericSubst_);
                savedCidxSema = std::move(genericSubstSemaTy_);
                genericSubst_ = std::move(ancSubst);
                genericSubstSemaTy_ = std::move(ancSema);
                cidxSubstInstalled = true;
            }
        }
        llvm::Function* fn = ensureIndexerBlockGetter(pd, getterBlock, ps);
        cur = builder_.CreateCall(fn->getFunctionType(), fn, callArgs);
        // fix #4973 restore: drop the ancestor subst installed above before
        // the indexer block-getter build/call.
        if (cidxSubstInstalled) {
            genericSubst_ = std::move(savedCidxSubst);
            genericSubstSemaTy_ = std::move(savedCidxSema);
        }
        emitPendingCheck();
    } else {
        // Expression-body getter: inline against obj and pre-computed params.
        llvm::Value* savedThis = currentThis_;
        auto savedLocals = locals_;
        auto savedParams = paramSlots_;
        auto savedSubstPair = setupReceiverGenericSubst(ix->target.get(), ps->parent);
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
        genericSubst_ = std::move(savedSubstPair.subst);
        genericSubstSemaTy_ = std::move(savedSubstPair.semaTy);
    }
    if (!cur) return false;

    // --- Apply the compound operator. ---
    llvm::Value* rhs = lowerRhs(cur);
    if (!rhs) {
        // #1386: ICE — lowerExpr returned null for the RHS of a compound
        // indexer assignment; the expression kind is not yet handled.
        ctx_.diagnostics().report(
            Severity::Error, rhsSpanBegin,
            "internal: lowerExpr returned null for compound indexer "
            "assignment RHS (block-getter path) — #1386");
        return false;
    }
    llvm::Value* newVal = emitCompoundOpTyped(
        binOp, cur, rhs, opLeftTy, opRightTy,   // §25.1/§25.2 #2904
        cur->getType()->isFloatingPointTy(),
        isUnsignedTargetExpr(targetExpr),
        isCharTargetExpr(targetExpr),      // §6.6/§5.2 #1961
        rhsUnsigned);                      // §6.2 #2140 rUns
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
        // For string `+=`, rhs is `lowerExpr(*valueExpr)` — a +1 string
        // when the AST is a producer (e.g. BinaryExpr("+", ...)) but a
        // BORROW when the AST is a named local (e.g. `s += t` where `t` is
        // a tracked string local). Check the AST shape, not the LLVM
        // pointer type: only release when the rhs is itself a +1 producer.
        // fix #2848: null `valueExpr` is the `++`/`--` spelling, whose right
        // operand is §17.6's implicit literal `1` — never a producer.
        if (rhs && rhs->getType()->isPointerTy() && valueExpr &&
            isPlusOneStringProducer(valueExpr))
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
        // fix #3924 (§6.6/§7.11/§17.6, v0.288): `d[k] += m` for a §7.4-narrowed
        // nullable local/parameter `m` — sema now admits it (#3830), but `rhs`
        // above is still lowered from `m`'s declared `{i8, T}` storage. Unwrap
        // BEFORE the overload lookup / `coerceNumericToType` below, mirroring
        // the local/field/static call site's #3924 fix in LlvmBackend_Stmts.cpp
        // (this dict path calls bare `emitCompoundOp`, not the already-fixed
        // `emitCompoundOpTyped`, so it needs its own copy of the unwrap). A
        // no-op when `as.value`'s declared type is not a nullable primitive —
        // in particular for a Named-type RHS the overload lookup just below
        // resolves on its own terms.
        rhs = unwrapNarrowedNullableOperand(rhs, as.value.get());
        // fix #626 / §25.1: if the VALUE element type has a user-defined
        // `static operator <op>` (two params), dispatch to it and write back —
        // mirrors the #610 overload check in lowerStmt.  emitCompoundOp cannot
        // handle struct operands (LLVM rejects sadd.with.overflow on struct
        // types), so the overload must be checked BEFORE emitCompoundOp.
        //
        // fix #2904 (§25.2, v0.240): the LOOKUP is now the one shared
        // definition (findCompoundOperatorOverload), which also gives this site
        // §25.2's in-order parameter match — "parameters match operands in
        // order" — that this hand-written copy never had. The LEFT type stays
        // the CONTAINER's declared value type, not `exprType` of the index
        // expression: that is this branch's own authority for what a `d[k]`
        // element is. The WRITE-BACK stays local (it must reuse the already-
        // evaluated receiver/index, see the Array branch's §17.6 note).
        {
            const sema::TypeRef* valTy =
                (recvTy && recvTy->genericArgs.size() > 1)
                    ? recvTy->genericArgs[1] : nullptr;
            sema::MethodSymbol* opMethod = findCompoundOperatorOverload(
                op, valTy, ctx_.exprType(*as.value), ctx_.stmtOperatorSymbol(as));
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
    // fix #2799 / §7.4: unwrap a §7.4-narrowed nullable index before the
    // sext (same soundness story as the Array<T> indexer-set fix above;
    // checkIndexArgNullability already rejects the un-narrowed form).
    idx = unwrapNarrowedNullableOperand(idx, ix->indices[0].value.get());
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
    // fix #3924 (§6.6/§7.11/§17.6, v0.288): `arr[i] += m` for a §7.4-narrowed
    // nullable local/parameter `m` — see the dict branch's identical #3924
    // comment above. This Array<T> branch calls bare `emitCompoundOp` too
    // (measured: `a[0] += m` for a narrowed `int?` local IR-verifier-aborted
    // identically to the local/field/static shape, "Call parameter type does
    // not match function signature!"), so it needs the same unwrap.
    rhs = unwrapNarrowedNullableOperand(rhs, as.value.get());
    std::string op(as.op);
    if (!op.empty() && op.back() == '=') op.pop_back();

    // fix #626 / §25.1: if the element type has a user-defined
    // `static operator <op>` (two params), dispatch to it and write back —
    // mirrors the #610 overload check in lowerStmt and the dict branch above.
    // emitCompoundOp cannot handle struct operands (LLVM rejects
    // sadd.with.overflow on struct types), so this must be checked FIRST.
    //
    // fix #2904 (§25.2, v0.240): the LOOKUP is the one shared definition
    // (findCompoundOperatorOverload), which also gives this site §25.2's
    // in-order parameter match. LEFT type = the CONTAINER's element type (this
    // branch's own authority for what `arr[i]` is); the write-back below stays
    // local for the §17.6 single-evaluation reason stated there.
    llvm::Value* newVal = nullptr;
    {
        const sema::TypeRef* elemTyRef =
            (recvTy && !recvTy->genericArgs.empty())
                ? recvTy->genericArgs[0] : nullptr;
        sema::MethodSymbol* opMethod = findCompoundOperatorOverload(
            op, elemTyRef, ctx_.exprType(*as.value),
            ctx_.stmtOperatorSymbol(as));
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
        // fix #2799 / §7.4: unwrap a §7.4-narrowed nullable index before the
        // sext (same soundness story as the Array<T> compound-assign fix
        // above; checkIndexArgNullability already rejects the un-narrowed
        // form).
        idx = unwrapNarrowedNullableOperand(idx, ix->indices[0].value.get());
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

        // §17.6's implicit `1` from the ONE definition of that rule (#2888):
        // an `Array<int?>` / `Array<decimal>` element lowers to a STRUCT, and
        // `ConstantInt::get` on a struct type is not a constant of that type.
        bool fp = cur->getType()->isFloatingPointTy();
        llvm::Value* one = emitImplicitOne(cur->getType());
        llvm::Value* newVal = one ? emitCompoundOp(
            binOp, cur, one, fp, isUnsignedTargetExpr(*inc.target),
            isCharTargetExpr(*inc.target))  // §6.6/§5.2 #1961
            : nullptr;
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
    // §17.6's implicit `1` from the ONE definition of that rule (#2888) — see
    // the Array<T> branch above for why `ConstantInt::get` was wrong here.
    bool fp = cur->getType()->isFloatingPointTy();
    llvm::Value* one = emitImplicitOne(cur->getType());
    llvm::Value* newVal = one ? emitCompoundOp(
        binOp, cur, one, fp, isUnsignedTargetExpr(*inc.target),
        isCharTargetExpr(*inc.target))  // §6.6/§5.2 #1961
        : nullptr;
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
//   * currentChoiceMethod_ — fix #4143 (§35.2/§35.5, v0.295), found by the R17
//     review of fix #4113 (which ADDED this member without adding it here).
//     `choiceTypeOf` (LlvmBackend_Setup.cpp) falls back to it to resolve a
//     bare `this`/`base` at a choice property's three accessor call sites
//     (LlvmBackend_Exprs.cpp inline getter read, `ensurePropertyAccessorFn`,
//     `makePropertyThunk`). Unlike currentEnumMethod_ below, this one IS
//     cleared here — see the member's own declaration comment
//     (LlvmBackend_impl.hpp) and the NestedBodyScope struct's comment on
//     `choiceMethod_` for the destructive-vs-soft-hint distinction that makes
//     the two asymmetric. Repro: a class property (`Animal.Sound`, a BLOCK-
//     form getter reached via `ensurePropertyBlockGetter`, one of this
//     guard's six emitters) whose body is `case this of Dog => ..., Cat =>
//     ...` printed the wildcard arm instead of the correct one when first
//     touched from inside a `choice` property's getter window — the leaked
//     `currentChoiceMethod_` (the CALLER's choice type) got folded into
//     `Animal`'s own CaseExpr scrutinee TypeRef at LlvmBackend_Exprs2.cpp's
//     "scrutTy not already a Choice" guard, silently reclassifying a class
//     receiver as the wrong choice type. A crash would have been loud; this
//     was a silent wrong answer.
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
//     to it to resolve a bare `this` in an enum member. DECLINED alongside
//     the #4143 choice fix above rather than bundled with it (fix #4143
//     revisited this entry and re-verified rather than trusting it as
//     written): the original "resetting would break a block-bodied ENUM
//     property, which works today" claim is now STALE — spec §10.5 forbids
//     an enum from declaring ANY block-form or storage-backed property or
//     indexer at all (confirmed mechanically: `enum E ... property P: int
//     get => 7; end P; end E;` is REJECTED, "enum `E` may not declare a
//     block-form property (§10.5)", also enforced by fix #2499), so none of
//     this guard's six memoized builders can ever be reached with an
//     enum-typed OWNING type for its own accessor — the scenario the old
//     comment worried about is unreachable. But UNLIKE choice's leak, an
//     enum's leaked value does not destructively overwrite the CaseExpr
//     scrutinee TypeRef (there is no enum-side analogue of choice's "scrutTy
//     not already a Choice" overwrite guard in LlvmBackend_Exprs2.cpp) — it
//     only feeds `enumTy`, a separate parameter `patternMatches` consults to
//     resolve a BARE identifier pattern as an enum-constant NAME
//     (`enumConstantByName`). Whether a leaked cross-type `enumTy` can still
//     cause a wrong match for some OTHER (class/record) body whose `case
//     this` pattern names happen to collide with the leaked enum's constant
//     names is a real but narrower and unmeasured question, out of scope for
//     an unblocking fix — filed as a follow-up (#4143's issue) rather than
//     guessed at and clobbered here. Left inherited; #2702 item 3 stays open
//     for the set-from-owner fix that would make clearing it safe in general.
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
      choiceMethod_(e.currentChoiceMethod_)          // #4143
// fix #3551 (§29.3 / §30.1 / §30.7, v0.286): the suspend/resume frame members
// (save+clear at ctor, restore at dtor) are generated from
// BEZEL_SM_FRAME_MEMBERS so a member added to the macro is handled in all
// three places by construction — the per-member hand-maintained list this
// used to be is gone, and the half-mand asymmetry #3505 catalogues is
// therefore structurally impossible here.
#define BEZEL_SM_FRAME_NBSCOPE_INIT(TYPE, MEMBER, NEUTRAL) , MEMBER(e.MEMBER)
      BEZEL_SM_FRAME_MEMBERS(BEZEL_SM_FRAME_NBSCOPE_INIT)
#undef BEZEL_SM_FRAME_NBSCOPE_INIT
      ,
      emittingDisposerBody_(e.emittingDisposerBody_),
      disposerEntryExc_(e.disposerEntryExc_),
      currentCaughtName_(e.currentCaughtName_),
      caughtExcArmTarget_(e.caughtExcArmTarget_),
      expectedFunctionType_(e.expectedFunctionType_),
      lastCallGenericSubst_(e.lastCallGenericSubst_),
      inlinedAccessorEnd_(e.inlinedAccessorEnd_),
      asyncChainedReturn_(e.asyncChainedReturn_),
      asyncBorrowedReturn_(e.asyncBorrowedReturn_),
      pendingOwnedParams_(e.pendingOwnedParams_),
      pendingArgsDisposal_(e.pendingArgsDisposal_),
      pendingChainOutDisposals_(e.pendingChainOutDisposals_),  // fix #4975
      genericSubstSemaTy_(e.genericSubstSemaTy_) {   // COPY: inherited, not cleared
    // Clear the non-frame emitter state a nested body must not INHERIT.
    e.currentReturnTypeNode_     = nullptr;
    e.currentLambdaReturnSemaTy_ = nullptr;
    e.accessorReturnTypeNode_    = nullptr;              // #2703
    e.accessorReturnTypeFn_      = nullptr;              // #2703
    e.currentChoiceMethod_       = nullptr;              // #4143
    e.emittingDisposerBody_      = false;
    e.disposerEntryExc_          = nullptr;
    e.currentCaughtName_         = {};
    e.caughtExcArmTarget_        = nullptr;
    e.expectedFunctionType_      = nullptr;
    e.lastCallGenericSubst_.clear();
    e.inlinedAccessorEnd_        = nullptr;
    e.asyncChainedReturn_        = false;
    e.asyncBorrowedReturn_       = false;
    e.pendingOwnedParams_.clear();
    e.pendingArgsDisposal_.reset();
    e.pendingChainOutDisposals_.clear();  // fix #4975
    // Clear the saved+restored frame to its neutral values. Generated from
    // BEZEL_SM_FRAME_MEMBERS so a member added to the macro is cleared in
    // lockstep with the ctor-init and the dtor-restore above. SuspendKind
    // has no neutral value (its three values are all valid `kind`s), and
    // `Yield` is what `smStateGEPHere`'s second gate at Stmts.cpp ~3620
    // admits (`smKind_ == Yield || smKind_ == AsyncYield`); see #2702 item
    // on the smStateGEP_ reset.
#define BEZEL_SM_FRAME_NBSCOPE_CLEAR(TYPE, MEMBER, NEUTRAL) e.MEMBER = NEUTRAL;
    BEZEL_SM_FRAME_MEMBERS(BEZEL_SM_FRAME_NBSCOPE_CLEAR)
#undef BEZEL_SM_FRAME_NBSCOPE_CLEAR
    // genericSubstSemaTy_ is intentionally left as-is (see the note above):
    // saved so a nested body's writes do not desynchronise it from
    // genericSubst_, but NOT cleared, because the body must resolve its type
    // parameters under the same substitution its signature was built with.
}

Emitter::NestedBodyScope::~NestedBodyScope() {
    // Restore the saved frame. Generated from BEZEL_SM_FRAME_MEMBERS so a
    // member added to the macro is restored here by construction — and
    // `iterIsOwningIter_` (#2621) is one of those members, so the
    // uninitialised struct-field-write that the pre-#3551 hand list missed
    // cannot happen on a folded scope.
#define BEZEL_SM_FRAME_NBSCOPE_RESTORE(TYPE, MEMBER, NEUTRAL) \
    e_.MEMBER = std::move(MEMBER);
    BEZEL_SM_FRAME_MEMBERS(BEZEL_SM_FRAME_NBSCOPE_RESTORE)
#undef BEZEL_SM_FRAME_NBSCOPE_RESTORE
    e_.currentReturnTypeNode_     = retNode_;
    e_.currentLambdaReturnSemaTy_ = lamRet_;
    e_.accessorReturnTypeNode_    = accessorRetNode_;   // #2703
    e_.accessorReturnTypeFn_      = accessorRetFn_;     // #2703
    e_.currentChoiceMethod_       = choiceMethod_;      // #4143
    e_.emittingDisposerBody_      = emittingDisposerBody_;
    e_.disposerEntryExc_          = disposerEntryExc_;
    e_.currentCaughtName_         = currentCaughtName_;
    e_.caughtExcArmTarget_        = caughtExcArmTarget_;
    e_.expectedFunctionType_      = expectedFunctionType_;
    e_.lastCallGenericSubst_      = std::move(lastCallGenericSubst_);
    e_.inlinedAccessorEnd_        = inlinedAccessorEnd_;
    e_.asyncChainedReturn_        = asyncChainedReturn_;
    e_.asyncBorrowedReturn_       = asyncBorrowedReturn_;
    e_.pendingOwnedParams_        = std::move(pendingOwnedParams_);
    e_.pendingArgsDisposal_       = std::move(pendingArgsDisposal_);
    e_.pendingChainOutDisposals_  = std::move(pendingChainOutDisposals_);  // fix #4975
    e_.genericSubstSemaTy_        = std::move(genericSubstSemaTy_);
}


llvm::Function* Emitter::ensurePropertyBlockGetter(const ast::PropertyDecl* pd,
                                          const ast::PropertyAccessor* acc) {
    // fix #2928: the memo key carries the accessor specialization tag; tag 0 is
    // the shared/erased template every ordinary caller wants.
    auto bgKey = std::make_pair(pd, accessorSpecTagNow());
    auto it = propBlockGetterFns_.find(bgKey);
    if (it != propBlockGetterFns_.end()) return it->second;

    llvm::Type* retTy = lowerType(pd->type.get());
    if (!retTy) retTy = i32();

    auto* fnTy = llvm::FunctionType::get(retTy, {ptrTy()}, false);
    auto* fn = llvm::Function::Create(
        fnTy, llvm::GlobalValue::InternalLinkage,
        "bz$prop$" + std::string(pd->name) + "$get", *module_);
    propBlockGetterFns_[bgKey] = fn;
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
    // fix #2928: the memo key carries the accessor specialization tag; tag 0 is
    // the shared/erased template every ordinary caller wants.
    auto key = std::make_tuple(ps, isSet, accessorSpecTagNow());
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
        // fix #3190 (§12.5/§23/§2.8.2 clause 1, v0.254): the map above is
        // populated by declareImportedPropertyGetters/-Setters, which SKIP any
        // property carrying a vtable slot (`if (propGetSlot_.count(ps))
        // continue;`) — i.e. every VIRTUAL one. So a virtual property
        // INHERITED and not overridden from an imported base found nothing
        // here, Pass D left its vtable/itable slot ConstantPointerNull, and
        // `d.P` / `b.P` / `i.P` all called 0x0 (measured: SIGSEGV where the
        // single-module twin answers 8). Forward through the exporter's own
        // `vt$<Owner>` slot instead; see importedVirtualAccessorSlotFn for why
        // that is the right function and why reusing the imported-ABI map
        // would recurse.
        if (llvm::Function* fwd = importedVirtualAccessorSlotFn(ps, isSet)) {
            propAccessorFns_[key] = fwd;
            return fwd;
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
    // fix #4036 (§12.5, v0.295): an ABSTRACT-SHORTHAND named property
    // (`abstract property Name: T;`) shares `PropertyBodyKind::Auto` with a
    // real auto-property, but #4020's `isAutoPropertyStorageSymbol` already
    // excludes it from `storage` (no backing field exists for it — #4018).
    // Without this early decline, the `isAuto` getter/setter branches below
    // fall into the `fieldIndex_.find(ps)` miss and its
    // `Constant::getNullValue` fallback, SYNTHESIZING a live function that
    // unconditionally returns null/zero — installed as a real vtable/itable
    // slot — instead of leaving the slot null the way an abstract METHOD or
    // an abstract INDEXER already do (methods: `MethodBodyKind::Abstract`
    // has no body to emit; indexers: `ensureIndexerGetterFn`/
    // `ensureIndexerSetterFn` have no "Auto" branch at all — the shape is
    // structurally unreachable there, not declined by an explicit check).
    // Decline here too, so the failure mode of a sema gap that ever lets an
    // uninstantiable abstract class's own vtable slot get READ is a loud
    // null-pointer fault, not a silently-returned wrong value.
    //
    // NOT (yet) independently observed as reachable: #4020 requires every
    // concrete class to override an inherited abstract property, so the
    // owning (abstract) class's OWN vtable/itable slot — the one this
    // synthesized stub fills — is never installed as the RUNTIME vtable of
    // any live object (an abstract class cannot be `new`'d). Separately
    // probed here: even the classic "virtual dispatch during base
    // construction" footgun (a non-virtual helper, called from the abstract
    // base's own constructor, reading `this.Current` before the derived
    // override's backing field is assigned) still resolves through the
    // DERIVED class's vtable slot, not the base's own — Bezel installs the
    // vtable pointer once at allocation to the leaf class; there is no
    // per-subobject vtable phase during base construction the way C++ has.
    // So this is defense-in-depth (the #3641/row-12 precedent), not a fix
    // for an observed bug.
    if (isAuto && sema::isAbstractPropertyShorthand(*pd)) return nullptr;
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

    // fix #2885 (§22.12/§22.13/§23, v0.240): a STATIC property's accessor has
    // NO receiver — §22.12: "a static body has no `this`". Every pre-#2885
    // caller of this function filtered statics out before reaching here (the
    // four emitExported* emitters skip them; a static property is never virtual
    // and never an interface member, §11.8, so no vtable/itable caller exists),
    // so this branch is reachable only from emitExportedStaticPropertyAccessors
    // — the exporter half of the cross-module static ABI. The differences are
    // exactly the three a missing `this` implies: the parameter list, the
    // `value` argument position, and the §28.2 checked-disposal guard (which
    // GEPs the receiver's disposed flag and would dereference nothing at all).
    const bool isStaticProp = isStaticProperty(ps);
    // A static AUTO-property's storage is a module-level global (§23.1 #743,
    // declareStatics), NOT an instance slot — the isAuto branches below would
    // GEP `currentThis_` into the instance struct. The static arms added by
    // #2885 read/write that global instead; with no global there is no storage
    // to accessor-ise, so decline BEFORE the function is created rather than
    // emitting a body that returns a silent zero.
    if (isStaticProp && isAuto && !staticGlobals_.count(ps)) return nullptr;

    // Signature: getter (this)->T, setter (this, value:T)->void; static drops
    // the `this` parameter from both.
    std::vector<llvm::Type*> paramTys;
    if (!isStaticProp) paramTys.push_back(ptrTy());
    if (isSet) paramTys.push_back(valTy);
    auto* fnTy = llvm::FunctionType::get(
        isSet ? llvm::Type::getVoidTy(llvm_) : valTy, paramTys, false);
    std::string mangled = "bz$" + std::string(cls->name) + "$" +
                          std::string(ps->name) +
                          (isSet ? "$propset" : "$propget");
    // fix #2928: name a per-instantiation accessor for what it is. Without this
    // LLVM merely uniquifies the InternalLinkage name to `…$propget.1`, and this
    // family's whole evidence trail is "which function name is in the stack
    // frame" (the issue's own measurement was `SEGV … in bz$D2$Score$propget`).
    if (unsigned specTag = std::get<2>(key))
        mangled += "$spec" + std::to_string(specTag);
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
    // and it runs BEFORE `setupBoxDescriptors()` (moved directly after
    // `declareVtables()` by #2864), `emitExportedPropertyGetters()` and
    // `emitExportedPropertySetters()`. So "every call site precedes the body
    // loop" does NOT establish that no user code has been lowered yet.
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
    // fix #2885: no `this` argument exists on a static accessor (§22.12); the
    // body never reads one (sema rejects `this` in a static body), and leaving
    // it null keeps any future reader from silently binding argument 0 — the
    // `value` parameter — as a receiver.
    currentThis_        = isStaticProp ? nullptr : &*argIt;
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
    // fix #2885: a STATIC accessor has no instance to test (§28.2's guard is
    // about "a property or indexer accessor invocation" on a disposed
    // INSTANCE); with no `this` the GEP below would have no base at all.
    if (checkedDisposal_ && !isStaticProp && cls &&
        cls->kind == ast::TypeDeclKind::Class) {
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

    // fix #4113 (§35.2/§35.5, v0.295): the choice twin of the enum
    // workaround above — a choice property's BLOCK-bodied getter/setter (or
    // an interface/box-dispatch accessor built here) whose body is
    // `case this of …` needs the identical choiceTypeOf() recovery: this
    // accessor function is never registered in methodFns_ (it is not a
    // MethodSymbol), so choiceTypeOf's existing "this" recovery would
    // otherwise find nothing.
    //
    // fix #4143 (§35.2/§35.5, v0.295): SET-FROM-OWNER, not conditionally-
    // set-else-inherit, for consistency with the other two property-
    // accessor sites (LlvmBackend_Exprs.cpp, LlvmBackend_Layout.cpp) fixed
    // alongside this one. `NestedBodyScope` (constructed above in this
    // function) already clears `currentChoiceMethod_` to null before this
    // point runs, so the `else` branch is defensive rather than
    // load-bearing here today — but this function's OWN identity should
    // not depend on a caller of a different guard having gotten its clear
    // list right; see that guard's `choiceMethod_` member for the
    // destructive-overwrite mechanism this protects against.
    const sema::TypeSymbol* savedChoiceMethod2 = currentChoiceMethod_;
    currentChoiceMethod_ =
        (cls && cls->kind == ast::TypeDeclKind::Choice) ? cls : nullptr;

    if (!isSet) {
        // --- Getter body. ---
        if (isAuto && isStaticProp) {
            // fix #2885 / §23.1 (#743): a STATIC auto-property's storage is the
            // module-level global declareStatics allocated for it — the same
            // one staticFieldGlobal routes the same-module implicit get/set to.
            builder_.CreateRet(
                builder_.CreateLoad(valTy, staticGlobals_.find(ps)->second));
        } else if (isAuto) {
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
                // ⚠ LIVE ON THE EXPORTED FACE SINCE #2864, STILL INERT ON THE
                // VTABLE FACE. `boxValue` resolves the box vtable through
                // `primBoxVtable_`/`boxVtable_`, which `setupBoxDescriptors()`
                // fills. #2864 moved that pass to run immediately after
                // `declareVtables()` and therefore BEFORE the four
                // `emitExported{Property,Indexer}{Getters,Setters}` emitters, so
                // every accessor body those request — the NON-virtual public
                // property/indexer, #2864's own repro — now boxes properly.
                // The call sites INSIDE `declareVtables()` (Pass D, the
                // vtable/itable slots) still run while `boxVtableFor` returns
                // null, so `public virtual property O: Object => 0;` still fails
                // to build. That residual is #2759, and it is NOT fixable by
                // simply moving `setupBoxDescriptors` one step earlier: doing so
                // regresses a working record program to a null-vptr SIGSEGV —
                // the measurement is recorded at the call site
                // (LlvmBackend_impl.hpp, `setupBoxDescriptors()`). A build
                // failure is the SAFE residual: unlike the inlined read this
                // shape never silently produced a wrong value.
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
                // fix #2944 (§12.1 / §14.1 / §23, v0.240, NARROW): an
                // EXPRESSION-bodied property GETTER built as the shared/erased
                // TEMPLATE (tag 0 — `declareVtables` Pass D's first emission,
                // before any per-instantiation tag is installed) lowers its
                // body without an active substitution, so the lowered value of
                // an expression that REACHES a `T`-typed field through a
                // GENERIC BASE (`D2<U> : GBase<string>` reads `v2: T` declared
                // in `GBase`) is the i32 ERASED PLACEHOLDER. The function's
                // declared RETURN TYPE was built from
                // `lowerType(pd->type)` — which respects the OVERRIDE's
                // concrete annotation (e.g. `Item: string` -> ptr) — so the
                // body's `ret i32` inside a `ptr`-returning function fails
                // IR verification: "Function return type does not match
                // operand type of return inst!". The METHOD twin
                // (`emitReturn`, LlvmBackend_Setup.cpp ~10818) handles the
                // BLOCK-bodied analogue by substituting a type-correct
                // `ret null` whenever `emittingGenericTemplateBody_` is
                // true; this site bypasses emitReturn and was missing the
                // guard, so the same shape on the EXPRESSION-GETTER face
                // was a hard abort.
                //
                // NARROW PREDICATE — !cls->genericParams.empty(). #2759's
                // residual (`public virtual property O: Object => 0;` on a
                // non-generic `Base`) is the EXACT same mismatch on a
                // non-generic class: body returns i32, function returns
                // ptr. The previous lane's fix (commit c5974ef) armed the
                // coercion for every tag-0 emission, which made #2759's
                // pinned-as-broken test build successfully and was reverted
                // (commit 671726f). Restricting the coercion to GENERIC
                // classes leaves the #2759 path untouched (its build failure
                // is the deliberate safe direction, per the test's own
                // comment) and only arms the substitution-erased-template
                // case where the body references a generic-base field with
                // no active `genericSubst_`. A generic class with a
                // non-type-param property (`Item: int get => a`) has
                // matching body and return types, so the coercion no-ops
                // even when the predicate fires.
                if (v && v->getType() != valTy && accessorSpecTagNow() == 0 &&
                    !cls->genericParams.empty())
                    v = llvm::Constant::getNullValue(valTy);
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
            // fix #2944 (§12.1 / §14.1 / §23, v0.240, NARROW): a BLOCK-bodied
            // property GETTER built as the shared/erased TEMPLATE (tag 0)
            // lowers its body's `return v;` through `emitReturn` with no
            // active substitution, so the value carries the i32 ERASED
            // PLACEHOLDER for a `T`-typed field reached through a generic
            // base, while the function's declared RETURN TYPE comes from
            // the OVERRIDE's concrete annotation (`Item: string` -> ptr).
            // `emitReturn`'s `emittingGenericTemplateBody_` guard
            // (LlvmBackend_Setup.cpp ~10818) would coerce the mismatched
            // return to a type-correct `ret null`, but the flag is set
            // only by `emitFunctionBody` for METHOD templates — this
            // getter's body is emitted INSIDE `ensurePropertyAccessorFn`,
            // never via `emitFunctionBody`.
            //
            // NARROW PREDICATE — !cls->genericParams.empty(). Same shape
            // as the expression-getter arm above: #2759's
            // `public virtual property O: Object get => do return _n;
            // end;` (block form) on a non-generic `Holder` would also
            // mismatch (body i32, function ptr) and was deliberately
            // pinned as a build failure. Restricting the arm to GENERIC
            // classes preserves the #2759 residual — and since the #2759
            // test is only the expression-bodied variant, the block-form
            // arm here is the more conservative choice: it never fires
            // for non-generic classes regardless of body shape.
            bool savedEmittingGenericTemplateBody = emittingGenericTemplateBody_;
            if (accessorSpecTagNow() == 0 && !cls->genericParams.empty())
                emittingGenericTemplateBody_ = true;
            lowerScopedBlock(blockGet->stmts);
            if (!builder_.GetInsertBlock()->getTerminator())
                emitImplicitReturn(fn);
            emittingGenericTemplateBody_ = savedEmittingGenericTemplateBody;
        }
    } else {
        // --- Setter body: bind the implicit `value` param (anchored on pd). ---
        // fix #2885: `value` is argument 0 on a static accessor (no `this`).
        llvm::Argument& valArg = isStaticProp
                                     ? *fn->args().begin()
                                     : *std::next(fn->args().begin());
        llvm::AllocaInst* vslot = entryAlloca(valArg.getType());
        builder_.CreateStore(&valArg, vslot);
        locals_[pd] = vslot;
        if (isAuto && isStaticProp) {
            // fix #2885 / §23.1 (#743): the STATIC auto-property's global.
            // A PLAIN store, deliberately, with none of the instance branch's
            // ARC below: §22.12 rule 2's store semantics for an admitted
            // mutable static are exactly that — "assigning a fresh owned value
            // into an admitted mutable static discharges the producer" and
            // "overwriting an admitted mutable static never disposes the
            // displaced referent" — and the admitted set is primitives/enums
            // plus the `Task?` cache, never `string` (rule 2 rejects a mutable
            // `string` static outright). This matches the same-module lowering
            // through staticFieldGlobal byte for byte.
            builder_.CreateStore(&valArg, staticGlobals_.find(ps)->second);
        } else if (isAuto) {
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
                // fix #4965 (§4.6 / §28.9 / §14.1, v0.307): a bare-`T`
                // auto-property at `T = string` (this accessor is emitted per
                // instantiation with its substitution active). Retain the new
                // value so the backing field owns the +1 the specialized
                // disposer releases; keep the old value (leak) rather than
                // release it — a caller in a generic body may hold it in a
                // `T` local that owns no +1 (see isTypeParamBoundToString).
                bool tpStrField = classField && !strField &&
                                  isTypeParamStringStorageMember(ps) &&
                                  valArg.getType()->isPointerTy();
                bool closureField = classField && !strField && !tpStrField &&
                                    isClosureStorageMember(ps) &&
                                    valArg.getType()->isPointerTy();
                // fix #2500 (§23.3/§28.9): box-typed (Object/interface) CLASS
                // auto-property backing field — the box twin of strField/
                // closureField above. isBoxStorageMember (class-gated, now
                // populated before this body is built — see the comment above)
                // confirms the field ONLY ever holds boxes, so bz_box_retain/
                // release are type-correct here (never called on a class-ref
                // pointer, which lacks a box header).
                bool boxField = classField && !strField && !tpStrField &&
                                !closureField && isBoxStorageMember(ps) &&
                                valArg.getType()->isPointerTy();
                if (strField) {
                    llvm::Value* oldVal = builder_.CreateLoad(
                        ptrTy(), slot, "propset.str.old");
                    emitStringRetain(&valArg);
                    builder_.CreateStore(&valArg, slot);
                    emitStringRelease(oldVal);
                } else if (tpStrField) {
                    emitStringRetain(&valArg);
                    builder_.CreateStore(&valArg, slot);
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
    currentChoiceMethod_ = savedChoiceMethod2;
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
    // fix #2928: the memo key carries the accessor specialization tag; tag 0 is
    // the shared/erased template every ordinary caller wants.
    auto bgKey = std::make_pair(pd, accessorSpecTagNow());
    auto it = propBlockGetterFns_.find(bgKey);
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
    propBlockGetterFns_[bgKey] = fn;

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


// fix #3190 (§12.1 / §12.5 / §23 "Properties" / §23.4 "Indexers" /
// §2.8.2 clause 1 / §2.8.3 rule 4, v0.254): the vtable/itable-slot function
// for a VIRTUAL property or indexer accessor whose most-derived
// implementation is declared on an IMPORTED base class.
//
// THE SHAPE, and why every existing fallback misses it. `buildVtable`'s
// Pass D (LlvmBackend_Setup.cpp) resolves a slot's accessor with
// `classPropertyByName` / `classIndexerMatching`, both of which walk
// `baseClassOf` ACROSS the module boundary, so `impl` can be an imported
// `PropertySymbol` — which never carries an `ast::PropertyDecl`. The three
// builders then had nothing to return:
//   * `ensureIndexerGetterFn`/`-SetterFn` returned nullptr outright;
//   * `ensurePropertyAccessorFn` fell back to `importedPropGetterFns_`/
//     `importedPropSetterFns_` (#2387/#2400) — but
//     `declareImportedProperty{Getters,Setters}` skip any property that HAS a
//     vtable slot (`if (propGetSlot_.count(ps)) continue;`), i.e. precisely
//     the virtual ones, so those maps are EMPTY for this shape.
// The slot kept its `ConstantPointerNull` initialiser and every dispatch
// called address 0x0. MEASURED on this fix's parent, five spellings, all
// SIGSEGV/139 against a character-identical single-module twin that answers
// correctly: `d.P` on the concrete derived type (8), `b.P` through a
// base-typed reference (8), `b.P := 30; b.P` (35), `i.P` through a LOCAL
// interface the derived class implements (8), and the indexer twin `b[7]`
// (107). A virtual METHOD inherited the same way was already CORRECT (42) —
// a method has an ExternalLinkage `bz$<Class>$<M>` symbol, which is exactly
// the asymmetry: `ensurePropertyAccessorFn` names the real accessor body
// `bz$<Class>$<Prop>$propget` with INTERNAL linkage, and
// `emitExportedProperty/Indexer{Getters,Setters}` all skip virtual members,
// so the dependency object exports NO symbol for it (`nm`-confirmed).
//
// WHAT IT INSTALLS. The dependency's `vt$<Owner>` global IS ExternalLinkage
// and is ALREADY declared in the importer (LlvmBackend_Setup.cpp, for the
// §8.6 string discriminator). Since the local class does not override, the
// function its slot needs is byte-identically the one the exporter put in
// `vt$<Owner>[slot]`. So this emits a small internal forwarding thunk with
// the accessor's own ABI that loads that slot and calls it. No new metadata,
// no new exported symbol, and no new assumption: the slot INDEX is
// `propGetSlot_`/`propSetSlot_`, the same index #2545's wrapper already GEPs
// and the same one Pass D already uses.
//
// ⚠ IT MUST NOT REUSE `importedIndexer{Getter,Setter}Fns_`. For a VIRTUAL
// indexer that map holds #2545's dispatch wrapper, which loads the vptr OFF
// THE RECEIVER — installing it in the receiver's own vtable slot is unbounded
// recursion, not a fix. That is why this resolves through `vt$<Owner>` (a
// fixed global) rather than through the object.
//
// Narrower than #2837, deliberately: that issue needs a NON-virtual entry
// point so `base[i]` can bypass an override, which this thunk cannot give it
// (it forwards to whatever the base's own slot holds, which is the correct
// answer here and the wrong one there).
llvm::Function* Emitter::importedVirtualAccessorSlotFn(
        const sema::PropertySymbol* ps, bool isSet) {
    if (!ps) return nullptr;
    // A LOCAL accessor has a body to emit; only a decl-less (imported) one
    // reaches here. `ps->decl` is checked directly rather than via a
    // PropertyDecl cast so a symbol carrying some OTHER decl node also
    // declines (it is not a shape this thunk can describe).
    if (ps->decl) return nullptr;
    const sema::TypeSymbol* owner = ps->parent;
    if (!owner || owner->kind != ast::TypeDeclKind::Class) return nullptr;
    // `vt$<Owner>` is ExternalLinkage only under declareVtables' own export
    // gate (public + non-generic — a generic's vtable is per-instantiation and
    // internal in the exporter). Outside it there is no symbol to name, so
    // decline and keep today's null slot rather than mint an unresolvable
    // relocation in an initialized global.
    if (owner->visibility != ast::Visibility::Public) return nullptr;
    if (!owner->genericParams.empty()) return nullptr;
    if (isStaticProperty(ps)) return nullptr;  // no `this`, no vtable slot
    // An `abstract` accessor has NO body in the exporter either, so
    // `vt$<Owner>`'s own slot is null: forwarding through it would relocate
    // the identical crash rather than fix it. §12.5 says the override fills
    // the slot, and an override is LOCAL (it has an `ast::PropertyDecl`), so
    // this branch is never the one that should answer for an abstract member.
    for (auto mod : ps->modifiers)
        if (mod == "abstract") return nullptr;
    const auto& slotMap = isSet ? propSetSlot_ : propGetSlot_;
    auto slotIt = slotMap.find(ps);
    // No slot ⇒ NOT virtual ⇒ the #2387/#2400/#2414 direct-call ABI owns this
    // member and the maps above already answer for it. Declining here is what
    // keeps the two routes from installing different functions for one shape.
    if (slotIt == slotMap.end()) return nullptr;
    const unsigned slot = slotIt->second;

    // The accessor ABI, from the reconstructed side tables — identical to the
    // one declareImportedIndexerGetters builds (§23: get/set share the
    // declared type; the index parameters ride between `this` and `value`).
    // `indexerParams` is empty for a named property, so one loop covers both.
    sema::TypeRef* mt = ctx_.importedMemberType(ps);
    llvm::Type* valTy = mt ? lowerTypeRef(mt) : nullptr;
    if (!valTy) return nullptr;
    std::vector<llvm::Type*> paramTys;
    paramTys.push_back(ptrTy());  // `this`
    for (sema::Symbol* p : ps->indexerParams) {
        auto* pps = dynamic_cast<sema::ParameterSymbol*>(p);
        sema::TypeRef* pt = pps ? pps->resolvedDeclType : nullptr;
        llvm::Type* plt = pt ? lowerTypeRef(pt) : nullptr;
        if (!plt) return nullptr;
        paramTys.push_back(plt);
    }
    if (isSet) paramTys.push_back(valTy);
    auto* fnTy = llvm::FunctionType::get(
        isSet ? llvm::Type::getVoidTy(llvm_) : valTy, paramTys, false);

    // The owner + slot pair names this thunk uniquely (a getter and a setter
    // never share a slot), so the module itself is the memo — no twelfth side
    // table.
    std::string name =
        "bz$" + mangledTypeName(owner) + "$vtfwd$" + std::to_string(slot);
    if (auto* existing = module_->getFunction(name)) return existing;

    std::string vtName = "vt$" + mangledTypeName(owner);
    llvm::GlobalVariable* vt = module_->getNamedGlobal(vtName);
    if (!vt) {
        // Same zero-length-array declaration shape declareVtables uses for an
        // imported class's vtable: the element count is the exporter's
        // virtual-slot count, unknown here and irrelevant — the global is only
        // ever GEP'd as an array of `ptr`.
        vt = new llvm::GlobalVariable(
            *module_, llvm::ArrayType::get(ptrTy(), 0), /*isConstant=*/true,
            llvm::GlobalValue::ExternalLinkage, /*init=*/nullptr, vtName);
    }

    auto* fn = llvm::Function::Create(
        fnTy, llvm::GlobalValue::InternalLinkage, name, module_.get());
    auto* savedFn = currentFn_;
    auto savedBB = builder_.GetInsertBlock();
    auto savedBBOffset =
        savedBB ? builder_.GetInsertPoint() : decltype(savedBB->begin()){};
    currentFn_ = fn;
    auto* entry = llvm::BasicBlock::Create(llvm_, "entry", fn);
    builder_.SetInsertPoint(entry);
    llvm::Value* slotPtr =
        builder_.CreateGEP(ptrTy(), vt, {builder_.getInt32(slot)});
    llvm::Value* target = builder_.CreateLoad(ptrTy(), slotPtr, "vtfwd");
    std::vector<llvm::Value*> callArgs;
    for (auto& a : fn->args()) callArgs.push_back(&a);
    if (isSet) {
        builder_.CreateCall(fnTy, target, callArgs);
        builder_.CreateRetVoid();
    } else {
        builder_.CreateRet(builder_.CreateCall(fnTy, target, callArgs));
    }
    currentFn_ = savedFn;
    if (savedBB) builder_.SetInsertPoint(savedBB, savedBBOffset);
    return fn;
}


// #730 / §23.4 / §13: emit a standalone `T get(ptr this, idx...)` for a class's
// concrete indexer getter, used to fill the itable slot for interface-typed
// indexer dispatch. Block-bodied getters reuse ensureIndexerBlockGetter (same
// signature); expression-bodied getters are emitted here. Cached per indexer
// PropertySymbol.
llvm::Function* Emitter::ensureIndexerGetterFn(const sema::TypeSymbol* cls,
                                               const sema::PropertySymbol* ps) {
    (void)cls;
    // fix #2928: the memo key carries the accessor specialization tag; tag 0 is
    // the shared/erased template every ordinary caller wants.
    auto idxKey = std::make_pair(ps, accessorSpecTagNow());
    auto cached = indexerGetterFns_.find(idxKey);
    if (cached != indexerGetterFns_.end()) return cached->second;
    auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl);
    if (!pd) {
        // fix #3190 (§23.4/§12.1/§2.8.2 clause 1, v0.254): the INDEXER twin of
        // the ensurePropertyAccessorFn fallback — an imported indexer has no
        // AST body to build, and for a VIRTUAL one there is no exported
        // direct-call stub either (emitExportedIndexerGetters skips
        // isVirtualIndexer). Pass D therefore left the vtable slot
        // ConstantPointerNull and `b[7]` called 0x0 (measured: SIGSEGV where
        // the single-module twin answers 110). Deliberately NOT
        // `importedIndexerGetterFns_`: for a virtual indexer that holds
        // #2545's receiver-vptr wrapper, which in its own slot recurses.
        llvm::Function* fwd =
            importedVirtualAccessorSlotFn(ps, /*isSet=*/false);
        if (fwd) indexerGetterFns_[idxKey] = fwd;
        return fwd;
    }

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
        indexerGetterFns_[idxKey] = fn;
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
        fnTy, llvm::GlobalValue::InternalLinkage,
        // fix #2928: `$specN` names a per-instantiation indexer accessor (see
        // the property twin in ensurePropertyAccessorFn).
        idxKey.second ? "bz$indexer$get$spec" + std::to_string(idxKey.second)
                      : std::string("bz$indexer$get"),
        *module_);
    indexerGetterFns_[idxKey] = fn;

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
    // fix #2748 (§23.4 / §23.2, v0.240): the vtable/itable slot for an
    // expression-bodied INDEXER GETTER sits on the same memoised-standalone-
    // function face as `ensurePropertyBlockGetter` / `ensureIndexerBlockGetter`
    // / `ensurePropertyAccessorFn`, but unlike those three the module-level
    // path here was never wrapped in `NestedBodyScope`. The four already-
    // guarded emitters share `NestedBodyScope`'s reset of the per-body
    // channels (#2170, #2694, #2701, #2702, #2703, #2731 item 1, #2731
    // item 3, #3125) — every member the guard owns, and which the caller's
    // emission may have left set, is reset on entry and restored on exit. The
    // issue verified that every reachable entry to THIS emitter is module-
    // level (it is called only from `declareVtables`, `setupBoxDescriptors`
    // and `emitExportedIndexerGetters`, all above the method-body loop), so
    // every member the guard resets is ALREADY neutral at every reachable
    // entry — adopting the guard is pure hardening, identical in shape to
    // the three already-guarded emitters. `genericSubst_` is one of the six
    // deliberately-INHERITED members (#2702); the explicit save/restore
    // below stays.
    NestedBodyScope nestedScope(*this);

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
    // property arm in ensurePropertyAccessorFn above. LIVE when this emitter is
    // reached from `emitExportedIndexerGetters` (which #2864 moved after
    // `setupBoxDescriptors`); still subject to the #2759 inertness when it is
    // reached from declareVtables Pass D. See the note there.
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
    // fix #2928: the memo key carries the accessor specialization tag; tag 0 is
    // the shared/erased template every ordinary caller wants.
    auto idxKey = std::make_pair(ps, accessorSpecTagNow());
    auto cached = indexerSetterFns_.find(idxKey);
    if (cached != indexerSetterFns_.end()) return cached->second;
    auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl);
    if (!pd) {
        // fix #3190: the SETTER twin of the getter fallback just above — same
        // shape, same reason, same measurement (`b[5] := 42` through an
        // inherited imported VIRTUAL indexer SIGSEGV'd). The `propSetSlot_`
        // lookup inside the helper is itself the "does a setter exist" gate:
        // buildVtable only assigns a set slot when `propertyHasSetter(ps)`,
        // which for an imported symbol already reads
        // ctx_.importedPropertyHasSetter (#2400/#2406).
        llvm::Function* fwd =
            importedVirtualAccessorSlotFn(ps, /*isSet=*/true);
        if (fwd) indexerSetterFns_[idxKey] = fwd;
        return fwd;
    }

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
        fnTy, llvm::GlobalValue::InternalLinkage,
        // fix #2928: see the getter twin just above.
        idxKey.second ? "bz$indexer$set$spec" + std::to_string(idxKey.second)
                      : std::string("bz$indexer$set"),
        *module_);
    indexerSetterFns_[idxKey] = fn;

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
    // fix #2748 (§23.4 / §23.2, v0.240): the SETTER twin of the indexer getter
    // just above — the vtable/itable slot for an expression-bodied INDEXER
    // SETTER was also unwrapped. The setter returns void and may legitimately
    // contain a bare `return;` that runs through `LlvmBackend_Stmts.cpp` ~1795,
    // so the inherited-state hazard is not just an `inlinedAccessorEnd_`
    // cross-function `br` waiting to happen on a state-machine emitter — it is
    // the same module-level pattern, and the same hardening applies. See the
    // sibling comment at `ensureIndexerGetterFn` for the per-channel
    // inventory. `genericSubst_` stays in the explicit save/restore below;
    // `NestedBodyScope` leaves it inherited (#2702).
    NestedBodyScope nestedScope(*this);

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
//
// #3204 (§11.2 / §23.4 / §2.8.2 clause 1 / §2.8.3 rule 4, v0.254): the
// IMPORTED branch now consults `ensureIndexerGetterFn`/`-SetterFn` FIRST,
// which already covers an imported virtual / override candidate by
// returning the vtable-forward thunk #3190 added
// (`importedVirtualAccessorSlotFn`, the function that loads the
// `vt$<Owner>` slot and tail-calls it). The original `importedIndexer
// GetterFns_`/`-SetterFns_` map lookup is kept as the non-virtual
// fallback — those maps are populated ONLY by #2414's direct-call ABI
// declarators, which skip virtual members. The two are mutually
// exclusive at the codegen layer (the same predicate in
// `importedVirtualAccessorSlotFn` and in `declareImportedIndexerGetters`
// is the same "is there a vtable slot?" check), so this function
// resolves the correct ABI for each shape and rejects only the
// ABSTRACT shape (the forwarder's own `abstract` guard declines — its
// slot is null — and the direct-call declarator's skip list excludes
// it too). A miss on both branches (defensive) still falls through to
// null, matching #2438's existing "missing implementation" contract.
llvm::Function* Emitter::indexerItableAccessor(const sema::TypeSymbol* ts,
                                               const sema::PropertySymbol* iimpl,
                                               bool isSet) {
    if (!iimpl) return nullptr;
    if (dynamic_cast<const ast::PropertyDecl*>(iimpl->decl))
        return isSet ? ensureIndexerSetterFn(ts, iimpl)
                     : ensureIndexerGetterFn(ts, iimpl);
    // #3204: try the vtable-forward thunk route first (handles virtual /
    // override imported candidates — see #3190). Fall through to the
    // direct-call ABI lookup for non-virtual imported candidates (the
    // #2449 path). Both routes return null for an abstract candidate,
    // which is the sema-correct "does not implement" outcome.
    if (llvm::Function* fwd = isSet ? ensureIndexerSetterFn(ts, iimpl)
                                      : ensureIndexerGetterFn(ts, iimpl)) {
        return fwd;
    }
    auto& map = isSet ? importedIndexerSetterFns_ : importedIndexerGetterFns_;
    auto it = map.find(iimpl);
    return it != map.end() ? it->second : nullptr;
}


const ast::PropertyAccessor* Emitter::computedSetterOf(const ast::Expr& target) {
    auto* ps = dynamic_cast<sema::PropertySymbol*>(ctx_.exprSymbol(target));
    // #3152: the AST-ONLY predicate is the right one HERE — this function
    // returns an `ast::PropertyAccessor*` out of `pd->accessors`, so a symbol
    // with no `ast::PropertyDecl` has nothing to return and the next line says
    // so. Asking the storage question instead would change which of two
    // `nullptr` returns an imported property takes, and nothing else.
    if (!ps || ps->isIndexer || sema::hasAstAutoPropertyDecl(ps))
        return nullptr;
    auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl);
    if (!pd || pd->bodyKind != ast::PropertyBodyKind::Block) return nullptr;
    for (const auto& acc : pd->accessors) {
        if (acc.isSet &&
            acc.bodyKind != ast::PropertyAccessorBodyKind::Abstract)
            return &acc;
    }
    return nullptr;
}


bool Emitter::propSetterMayTakeOwned(const sema::PropertySymbol* ps) const {
    // fix #5209 (§23.3, v0.313): the symbol-side bit is set for a local
    // property from its accessor list and for an imported one from the
    // `MetaMember::setterTakesOwned` wire scalar. A null symbol is the
    // conservative "may take owned" (no caller-side dispose).
    return !ps || ps->setterTakesOwned;
}

bool Emitter::lowerPropertySet(const ast::Expr& target, const ast::Expr& value) {
    if (!computedSetterOf(target)) return false;
    llvm::Value* v = nullptr;
    // fix #4345 property-setter twin (§5.1/§11.2/§15.4, v0.298): the
    // indexer-setter codegen had no case for a CollectionLiteralExpr RHS —
    // `lowerExpr` returns null for one (§15.4), the assignment silently
    // degraded via the pre-existing #4045 backstop, and the write never
    // happened. The sema side (#4316's gate that admits the conforming
    // program and rejects the non-conforming twin) applies here too.
    // Mirror the indexer-setter / local-decl / return-stmt / field-init
    // pattern exactly: a literal RHS targeting an INLINE-array property
    // builds the [N x elem] aggregate by-value; a literal targeting a
    // HEAP collection goes through materializeHeapCollectionLiteral /
    // materializeHeapDictLiteral. Target type is read from
    // ctx_.exprType(target) (the property's declared type).
    if (auto* cl = dynamic_cast<const ast::CollectionLiteralExpr*>(&value)) {
        const sema::TypeRef* propTy = ctx_.exprType(target);
        if (propTy && propTy->kind == sema::TypeRef::Kind::InlineArray &&
            !cl->isDict) {
            if (auto* aty = llvm::dyn_cast<llvm::ArrayType>(
                    lowerTypeRef(propTy))) {
                llvm::Type* elemTy = aty->getElementType();
                llvm::AllocaInst* tmp = entryAlloca(aty);
                for (std::size_t i = 0; i < cl->elements.size(); ++i) {
                    if (!cl->elements[i]) continue;
                    llvm::Value* ev = lowerExpr(*cl->elements[i]);
                    if (!ev) {
                        ev = lowerCollectionLiteralElementOrUnitNull(
                            *cl->elements[i], elemTy);
                        if (!ev) {
                            ctx_.diagnostics().report(
                                Severity::Error,
                                cl->elements[i]->span.begin,
                                "internal: lowerExpr returned null for "
                                "property-setter inline-array collection-literal "
                                "element — #1386");
                            continue;
                        }
                    }
                    if (ev->getType() != elemTy &&
                        ev->getType()->isIntegerTy() &&
                        elemTy->isIntegerTy()) {
                        unsigned eb = ev->getType()->getIntegerBitWidth();
                        unsigned db = elemTy->getIntegerBitWidth();
                        ev = db > eb ? builder_.CreateSExt(ev, elemTy)
                                     : builder_.CreateTrunc(ev, elemTy);
                    }
                    llvm::Value* gep = builder_.CreateGEP(
                        aty, tmp,
                        {builder_.getInt32(0),
                         builder_.getInt32((unsigned)i)});
                    builder_.CreateStore(ev, gep);
                }
                v = builder_.CreateLoad(aty, tmp, "pset.inlarr");
            }
        } else if (propTy && !cl->isDict) {
            v = materializeHeapCollectionLiteral(cl, propTy);
        } else if (propTy && !cl->keys.empty()) {
            v = materializeHeapDictLiteral(cl, propTy);
        }
    }
    if (!v) v = lowerExpr(value);
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


// fix #4614(b) (§22.16, v0.306): §22.16's instrumentation set names
//
//   "- **Entry and normal exit** of every `public` **setter** — block-form,
//      auto-property, indexer, `set(owned)` alike."
//
// and NO accessor path emitted it. `grep -n emitInvariantCheck src/codegen/`
// found six call sites — the public-method entry (`emitFunctionBody`), the
// public-method exit (`armEnsuresEpilogue` -> `finalizeEnsuresEpilogue`) and
// four construction-exit sites — so the whole setter row of the set was
// absent. Measured on this HEAD before this fix: a `public property Q: int`
// whose block-form setter drives the instance through a state the class
// invariant rejects ran to completion and returned 42, where §22.16 requires
// ContractViolationException (99). That is a SILENT UNDER-CHECK, the
// direction §22.16's observable-boundary property exists to prevent.
//
// This predicate answers "does a store written at `target`, through `ps`'s
// setter, sit on one of those boundaries?" and returns the class whose
// conjunction to evaluate, or null.
//
// THE `this`-REENTRANCY EXEMPTION IS PART OF THE ANSWER, not a separate
// concern: §22.16's "A call whose receiver is `this` — explicit or implicit
// — from inside any instance member of the class or a subclass enters the
// member's unchecked face" governs setters exactly as it governs methods,
// and instrumenting without it would turn a silent under-check into a loud
// OVER-check — a public method that legitimately drives its own instance
// through a broken intermediate state via its own setter would fault. The
// test is on the SYNTAX, character for character the one `emitMethodCall`
// applies (#4597): `this.P := v`, `base.P := v`, a bare `P := v` inside an
// instance member, and the indexer spellings of each.
//
// fix #4651 (§22.16 / §2.8.2, v0.307): the predicate is now two functions.
// `setterInstrumentationClass` (immediately below) is its RECEIVER-INDEPENDENT
// half — everything decided from the DECLARATION alone: mode, class kind,
// `static`, §13.6 accessor visibility, and a non-empty conjunction. It is
// split out because the exporter-side §22.16 checked face
// (`emitExportedPropertySetters`) asks exactly that question and has no
// `ast::Expr` target to ask the other one with — and needs none, since every
// caller of that face is an importer, never a `this` receiver inside the
// declaring class. `instrumentedSetterClass` keeps the `this`-reentrancy
// exemption and is unchanged in behaviour.
const sema::TypeSymbol* Emitter::setterInstrumentationClass(
    const sema::PropertySymbol* ps, const ast::PropertyDecl* pd) const {
    // §22.16 / #4386: nothing is instrumented in release mode.
    if (!checkedContracts_ || !ps) return nullptr;
    // §22.16 "Never instrumented: non-public methods and setters, static
    // members, destructors, and `Dispose`". Records and enums have no class
    // invariant (the member is a class member, §22.16).
    if (!ps->parent || ps->parent->kind != ast::TypeDeclKind::Class)
        return nullptr;
    if (isStaticProperty(ps)) return nullptr;
    if (ps->visibility != ast::Visibility::Public) return nullptr;
    // §13.6 accessor visibility: `public property Id: int private set;` is a
    // public property with a NON-public setter, and §22.16's row is about the
    // setter.
    //
    // `pd` is the declaration the caller has already resolved. For
    // `lowerPropertySetValue` it is on that path only because
    // `computedSetterOf` returned a `pd->accessors` element, which requires
    // `ps->decl` to be a block-form `ast::PropertyDecl`; for
    // `emitExportedPropertySetters` it is that loop's own
    // `dynamic_cast<…>(ps->decl)`, which it already requires to be non-null.
    // Taking it as a parameter rather than re-deriving it with a second
    // `dynamic_cast` keeps this predicate off the imported-symbol
    // classification seam (umbrella #3152): there is no null-`decl` arm here
    // to answer "no visibility refinement" for an imported property, because
    // an imported property reaches neither caller. Null-tolerant so a future
    // caller that has no declaration degrades to "no accessor-level
    // refinement" explicitly rather than by a silent cast failure.
    if (pd) {
        if (pd->hasAutoAccessorVis && pd->autoAccessorIsSet &&
            pd->autoAccessorVis != ast::Visibility::Public)
            return nullptr;
        for (const auto& a : pd->accessors)
            if (a.isSet && a.visibility != ast::Visibility::None &&
                a.visibility != ast::Visibility::Public)
                return nullptr;
    }
    if (!hasClassInvariant(ps->parent)) return nullptr;
    return ps->parent;
}


const sema::TypeSymbol* Emitter::instrumentedSetterClass(
    const ast::Expr& target, const sema::PropertySymbol* ps,
    const ast::PropertyDecl* pd) const {
    if (!setterInstrumentationClass(ps, pd)) return nullptr;
    // The §22.16 `this`-reentrancy exemption, decided from the receiver's own
    // SYNTAX (see the header). `currentThis_` guards the implicit spelling so
    // a bare name lowered outside any instance member is never mistaken for
    // one.
    const ast::Expr* recvExpr = nullptr;
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&target))
        recvExpr = ma->target.get();
    else if (auto* ix = dynamic_cast<const ast::IndexExpr*>(&target))
        recvExpr = ix->target.get();
    else if (dynamic_cast<const ast::IdentifierExpr*>(&target))
        return currentThis_ ? nullptr : ps->parent;  // implicit `this`
    if (recvExpr)
        if (auto* tid = dynamic_cast<const ast::IdentifierExpr*>(recvExpr))
            if ((tid->name == "this" || tid->name == "base") && currentThis_)
                return nullptr;
    return ps->parent;
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
    // fix #5119 (§28.3 *Unbound owned temporaries* + §23.3 *Explicit setter
    // parameter modifier*, v0.313): a setter's implicit `value` parameter "has
    // no modifier by default and takes a borrow", so a fresh owned class
    // producer on the right of `obj.P := new C(...)` is an unbound owned
    // temporary serving a non-`owned` argument — disposed at the end of the
    // full expression (the assignment statement), exactly as the method-call
    // twin `obj.Put(new C(...))` already is. Before this fix the inlined-
    // setter path disposed nothing, leaking one object per assignment (for a
    // concrete `Item` property and a bare-`T` one at `T = Item` alike). A
    // `set(owned)` setter takes the value over and is excluded. A borrowing
    // field the setter body stored the value into dangles from here on: the
    // §28.3 borrowed-field store rule's recorded trust residual (K4.3/K4.1),
    // the same posture as the borrowing-collection store (#1881).
    auto emitUnboundOwnedTempDispose = [&] {
        if (setter->setterParamMod != "owned" && valueExpr &&
            isFreshOwnedClassProducer(valueExpr))
            emitDisposeAndReclaim(v);
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
        // §21.7 (#739): inline the setter body with an accessor-end target so a
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
        emitUnboundOwnedTempDispose();
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
    // fix #4614(b) (§22.16, v0.306): the setter ENTRY boundary. Emitted
    // before the inlined setter body so the instance is observed in the
    // state the caller handed over, exactly as the public-method entry
    // check is emitted before the method body (`emitFunctionBody`).
    const sema::TypeSymbol* invCls4614 = instrumentedSetterClass(target, ps, pd);
    if (invCls4614) emitInvariantCheck(recv, invCls4614);
    // fix #4973 / fix #4983 (§14.1 / §23 / §2.8.3 rule 4, v0.307): the SETTER
    // twin of the block-form property GETTER fix on branch 4973-getter-draft
    // (commit 9dcc51d7). Mirror the expression-bodied getter path at
    // LlvmBackend_Exprs.cpp:5910-5994 (which has both the SAME-CLASS branch
    // and the INHERITED branch). An inlined setter body that touches a
    // `T`-typed field needs the substitution installed BEFORE the inline
    // body, otherwise the body lowers against the erased T->i32 placeholder
    // and writes `_v` at the wrong offset.
    //
    // Two receiver shapes are broken without this fix:
    //
    //   (1) INHERITED (#4973, commit c0076598): the receiver is a non-generic
    //       derived class whose base is generic (`e: E`, `E: B<T>`, `Val: T`
    //       declared on `B<T>`). Walk the base chain via
    //       `computeAncestorGenericSubst` to install the ancestor subst
    //       `{T → string}`. Without the subst, `_v := value` writes the
    //       8-byte `value` into a 4-byte slot (opaque-ptr store), corrupting
    //       the upper 4 bytes of `_v`.
    //
    //   (2) SAME-CLASS (#4983, this commit): the receiver declaration EQUALS
    //       the property's declaring class (`b: B<string>`, `Val: T`
    //       declared on `B<T>`). The #4973 gate's `declaration != ps->parent`
    //       arm correctly skipped this shape — `computeAncestorGenericSubst`
    //       refuses on `cur == ancestor` (LlvmBackend_Calls2.cpp:13110) because
    //       there is no "ancestor walk" needed — but the substitution is
    //       still REQUIRED for the inline body to see the substituted
    //       layout, not the i32-erased template layout. Without the subst,
    //       `_v := value` writes the 8-byte `value` into the i32-shaped
    //       offset 12 instead of the specialized offset 16 (the bug prints
    //       `x\n\n` instead of `x\ny\n`). The substitution is read DIRECTLY
    //       off the receiver's own genericArgs (`B<string>` carries
    //       `genericArgs = [string]`), mirrored on
    //       LlvmBackend_Exprs.cpp:5922-5960 (the same-class GETTER inline
    //       path that already works correctly for `b.Val` reads) and on
    //       LlvmBackend_Layout.cpp:6285-6310 (the same-class GETTER thunk
    //       path).
    //
    // Both branches share the same save+restore discipline to keep the
    // installed subst from leaking past this write into the enclosing
    // statement. The implicit-`this` case (`ma->target` is null) is
    // correctly skipped — no receiver expression means no static type to
    // read genericArgs off of.
    const sema::TypeRef* setRecvTy4973 = nullptr;
    if (auto* ma4973 = dynamic_cast<const ast::MemberAccessExpr*>(&target))
        if (ma4973->target) setRecvTy4973 = ctx_.exprType(*ma4973->target);
    std::unordered_map<std::string_view, llvm::Type*> savedGSubstSet;
    std::unordered_map<std::string_view, const sema::TypeRef*> savedGSemaSet;
    bool substInstalledSet = false;
    if (setRecvTy4973 && setRecvTy4973->kind == sema::TypeRef::Kind::Named &&
        setRecvTy4973->declaration &&
        !ps->parent->genericParams.empty()) {
        if (setRecvTy4973->declaration != ps->parent) {
            // INHERITED (#4973): walk the base chain via
            // `computeAncestorGenericSubst`.
            std::unordered_map<std::string_view, llvm::Type*> ancSubst;
            std::unordered_map<std::string_view, const sema::TypeRef*> ancSema;
            if (computeAncestorGenericSubst(setRecvTy4973, ps->parent,
                                            ancSubst, ancSema)) {
                savedGSubstSet = std::move(genericSubst_);
                savedGSemaSet = std::move(genericSubstSemaTy_);
                genericSubst_ = std::move(ancSubst);
                genericSubstSemaTy_ = std::move(ancSema);
                substInstalledSet = true;
            }
        } else if (!setRecvTy4973->genericArgs.empty()) {
            // SAME-CLASS (#4983): read the subst off the receiver's own
            // genericArgs directly — mirrors Exprs.cpp:5922-5960.
            const std::vector<std::string_view> gpNames =
                sema::genericParamNames(ps->parent);
            if (gpNames.size() == setRecvTy4973->genericArgs.size()) {
                std::unordered_map<std::string_view, llvm::Type*> newSubst;
                std::unordered_map<std::string_view,
                                   const sema::TypeRef*> newSema;
                bool argsOk = true;
                for (std::size_t gi = 0; gi < gpNames.size(); ++gi) {
                    if (auto* lt = lowerTypeRef(
                            setRecvTy4973->genericArgs[gi])) {
                        newSubst[gpNames[gi]] = lt;
                    } else {
                        argsOk = false; break;
                    }
                    newSema[gpNames[gi]] =
                        resolveTypeParamRef(
                            setRecvTy4973->genericArgs[gi]);
                }
                if (argsOk) {
                    // fix #768 (§14.1): a generic nullable field read with
                    // the wrong type can produce wrong values / mismatched
                    // ICmp, so build the SEMA-side subst too, matching the
                    // getter's #768 mechanism.
                    savedGSubstSet = std::move(genericSubst_);
                    savedGSemaSet = std::move(genericSubstSemaTy_);
                    genericSubst_ = std::move(newSubst);
                    genericSubstSemaTy_ = std::move(newSema);
                    substInstalledSet = true;
                }
            }
        }
    }
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
    // §21.7 (#739): accessor-end target — a `return;` ends the setter, not the
    // enclosing method (the setter body is inlined here).
    llvm::BasicBlock* savedAccEnd = inlinedAccessorEnd_;
    auto* accEndBB = llvm::BasicBlock::Create(llvm_, "setter.end", currentFn_);
    inlinedAccessorEnd_ = accEndBB;
    bool term = false;
    // fix #5018 (§28.9 / §14.1 / §2.8.3 rule 4, v0.307): arm the narrow
    // release-safety window (see the flag's declaration comment in
    // LlvmBackend_impl.hpp) for exactly this inlined setter body's own
    // statements — a bare-`T` field store reached from here (`_v := value`)
    // may now release the displaced value instead of leaking it (#4965's
    // default everywhere else). Only armed when a subst was actually
    // installed above (a `T`-bound field access with no active subst cannot
    // resolve to `string` regardless of this flag). Save/restore so a call
    // made FROM this body into another function (which does not inherit
    // this narrow guarantee) cannot see the flag left on.
    bool savedInlinedSetterBareTFieldReleaseSafe =
        inlinedSetterBareTFieldReleaseSafe_;
    if (substInstalledSet) inlinedSetterBareTFieldReleaseSafe_ = true;
    if (setter->bodyKind == ast::PropertyAccessorBodyKind::Expression &&
        setter->exprBody)
        lowerExpr(*setter->exprBody);
    else
        term = lowerScopedBlock(setter->stmts);
    inlinedSetterBareTFieldReleaseSafe_ =
        savedInlinedSetterBareTFieldReleaseSafe;
    inlinedAccessorEnd_ = savedAccEnd;
    if (!term) builder_.CreateBr(accEndBB);
    builder_.SetInsertPoint(accEndBB);
    currentThis_ = savedThis;
    // fix #4614(b) (§22.16, v0.306): the setter NORMAL-EXIT boundary.
    // `accEndBB` is the join every non-throwing path through the inlined
    // body reaches (§21.7's accessor-end target, #739), which is exactly
    // §22.16's "normal exit" — "throw exits check nothing" is why an
    // unwinding path must not reach it.
    if (invCls4614 && !builder_.GetInsertBlock()->getTerminator())
        emitInvariantCheck(recv, invCls4614);
    // fix #2568 (§23.3/§28.9/§4.6, v0.236): the INSTANCE-PATH twin of the
    // static-path emitProducerRelease above. Same gate (classOwned) for the
    // same record-UAF reason. See the long comment above the static branch
    // for the full rationale and LSan reproduction.
    if (classOwned) emitProducerRelease();
    emitUnboundOwnedTempDispose();
    // fix #4973 / fix #4983 restore: drop the subst installed at the top
    // of this instance branch before the inline body emission.
    if (substInstalledSet) {
        genericSubst_ = std::move(savedGSubstSet);
        genericSubstSemaTy_ = std::move(savedGSemaSet);
    }
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
    // fix #2885 (§22.12/§22.13/§23.3/§2.8.2 clause 1, v0.240): a STATIC imported
    // property writes through its own no-`this` stub. Pre-fix this function
    // reached the receiver resolution below with `ma->target` naming a TYPE, got
    // nothing pointer- or aggregate-shaped out of it, and returned false — the
    // statement then fell through to the generic lvalueAddr fallback, which
    // cannot resolve a PropertySymbol target at all, and the write was SILENTLY
    // DROPPED (measured 0 where 8 is correct). The receiver resolution is
    // skipped entirely, exactly as the LOCAL static path does
    // (lowerPropertySetValue's `isStaticProp` branch).
    auto stIt = importedStaticPropSetterFns_.find(ps);
    const bool isStaticProp = stIt != importedStaticPropSetterFns_.end();
    if (!isStaticProp && sit == importedPropSetterFns_.end()) return false;
    bool isEnumReceiver = ps->parent &&
                          ps->parent->kind == ast::TypeDeclKind::Enum;
    llvm::Value* recv = nullptr;
    if (!isStaticProp) {
        llvm::Type* recvElemTy = nullptr;
        if (llvm::Value* addr = lvalueAddr(*ma->target, recvElemTy)) {
            if (recvElemTy && recvElemTy->isPointerTy())
                recv = builder_.CreateLoad(recvElemTy, addr);
            else if (recvElemTy && recvElemTy->isAggregateType() &&
                     !isEnumReceiver)
                recv = addr;  // the address of the receiver's real storage
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
    llvm::Function* fn = isStaticProp ? stIt->second : sit->second;
    llvm::Type* valTy = fn->getFunctionType()->getParamType(isStaticProp ? 0 : 1);
    // fix #2937 (§7.2/§5.1/§2.8.2 clause 1/§23.3, v0.240): the CROSS-MODULE
    // twin of #2927. This site was a bare `coerceNumericToType`, which covers
    // §5.1's numeric widening but has no case for §7.2's tagged
    // `{i8 hasValue; T}` slot, so a NULLABLE-VALUE imported property took the
    // bare payload and the compile ABORTED. MEASURED against a two-module
    // build on the #2927 fix: `p.PP := 1` where `public property PP: int?`
    // lives on an imported class gave "Call parameter type does not match
    // function signature! i32 1 { i8, i32 } call void
    // @"bz$XLib.XPlain$PP$propset"(ptr, i32 1)". Found by probing #2927's
    // sibling helpers across an artifact boundary — the in-module spellings
    // (`lowerPropertySetValue`) and the virtual/interface one
    // (`lowerVirtualPropertySet`, #2927) were both already correct, and this
    // ABI-stub path is reachable ONLY cross-module, which is why no
    // single-module test could see it.
    //
    // Same two helpers, in the same order, for the same reason as #2927:
    // #2903's one definition of an initializer's slot width (which takes the
    // nullable slot's PAYLOAD as the width to match, in both of §5.1's
    // directions), then §7.2's present-wrap — which also normalizes a `null`
    // literal to the all-zero form. Both are no-ops when `v` is already the
    // slot type, so the callers that arrive with a tagged value (the §22.3
    // `ref`/`out` write-back, #2862's compound path) are unaffected.
    v = coerceInitializerToSlot(v, valTy, isUnsignedTargetExpr(target));
    v = coerceToNullableSlot(valTy, v);
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
    if (isStaticProp) {
        // §22.13: the write is a static-member access, so the declaring type's
        // lazy initialization must have run first — the same guard a
        // cross-module static FIELD write already emits (emitEnsureStaticInit's
        // imported branch calls the exporter's `bz$<T>$ensureinit`).
        emitEnsureStaticInit(ps->parent);
        builder_.CreateCall(fn->getFunctionType(), fn, {v});
    } else {
        builder_.CreateCall(fn->getFunctionType(), fn, {recv, v});
    }
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
    // fix #5209 (§23.3 + §28.3, v0.313): mirror #5119 — a fresh owned class
    // temporary served to a plain (non-`set(owned)`) imported setter is an
    // unbound owned temporary, disposed at the end of the full expression.
    if (valueExpr && !propSetterMayTakeOwned(ps) &&
        isFreshOwnedClassProducer(valueExpr))
        emitDisposeAndReclaim(v);
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
    // fix #3231 (§12.3 / §23 / §2.8.2, v0.252): the `base.P := v` half of
    // the widening. §12.3 bypasses virtual dispatch and writes the
    // OWNER-class slot — the direct entry in
    // `importedPropSetterDirectFns_[ps]` (populated by
    // declareImportedPropertySetters, defined by the widened
    // emitExportedPropertySetters) is the right call. The dispatch runs
    // BEFORE the rejection function (so the #2204 "no double-evaluation"
    // discipline holds — a side-effect-free symbol-shape check decides
    // the target up front, the `value` is lowered only once a successful
    // call target is found).
    if (auto* baseId = dynamic_cast<const ast::IdentifierExpr*>(
            ma->target.get());
        baseId && baseId->name == "base" &&
        importedPropSetterDirectFns_.count(ps)) {
        llvm::Value* v = lowerExpr(value);
        if (!v) return false;
        llvm::Function* fn = importedPropSetterDirectFns_.find(ps)->second;
        // The receiver for `base.` IS the enclosing method's
        // `currentThis_` (§12.3); the same pointer every other base-class
        // accessor call uses in this module.
        llvm::Value* recv = currentThis_;
        if (!recv) return false;
        llvm::Type* valTy = fn->getFunctionType()->getParamType(1);
        v = coerceInitializerToSlot(v, valTy, isUnsignedTargetExpr(target));
        v = coerceToNullableSlot(valTy, v);
        v = boxForTarget(v, ctx_.exprType(target), &value);
        builder_.CreateCall(fn->getFunctionType(), fn, {recv, v});
        emitPendingCheck();
        return true;
    }
    // fix #3199 (§12.3 / §23 / §2.8.2, v0.254): reject `base.P := v` on an
    // IMPORTED VIRTUAL base property with a clean §12.3 error, the write twin
    // of the indexer reject (#2832) and the read path added at
    // `LlvmBackend_Exprs.cpp` ~5524. Pre-fix this function silently dropped
    // the write (the imported virtual property has no entry in
    // `importedPropSetterFns_`, so the `count` check below declined it without
    // any diagnostic and the caller's lvalueAddr fallback couldn't resolve a
    // PropertySymbol — measured `base.P := 7` left `_v` at 3 where 7 is
    // correct). Checked here, BEFORE the `value` lowering (the #2204
    // discipline), so a rejected target never double-evaluates it.
    // fix #3231: after the direct-entry check above, this rejection now
    // only fires when no direct entry exists (the ABSTRACT virtual shape).
    if (rejectImportedVirtualBaseProperty(*ma, ps)) return false;
    // fix #2885: the STATIC map is the second admissible shape — same #2204
    // discipline (a pure symbol-shape check with no side effects BEFORE
    // `value` is lowered, so a non-matching target never double-evaluates it).
    if (!importedPropSetterFns_.count(ps) &&
        !importedStaticPropSetterFns_.count(ps)) return false;
    llvm::Value* v = nullptr;
    // fix #4345 imported-property-setter twin (§5.1/§11.2/§15.4, v0.298):
    // mirrors the lowerPropertySet fix above. An imported property setter's
    // RHS that is a CollectionLiteralExpr would silently degrade via the
    // #4045 backstop — lowerExpr returns null for one (§15.4) and the
    // write never happened. Use ctx_.exprType(target) for the property's
    // declared type, exactly as lowerPropertySet does.
    if (auto* cl = dynamic_cast<const ast::CollectionLiteralExpr*>(&value)) {
        const sema::TypeRef* propTy = ctx_.exprType(target);
        if (propTy && propTy->kind == sema::TypeRef::Kind::InlineArray &&
            !cl->isDict) {
            if (auto* aty = llvm::dyn_cast<llvm::ArrayType>(
                    lowerTypeRef(propTy))) {
                llvm::Type* elemTy = aty->getElementType();
                llvm::AllocaInst* tmp = entryAlloca(aty);
                for (std::size_t i = 0; i < cl->elements.size(); ++i) {
                    if (!cl->elements[i]) continue;
                    llvm::Value* ev = lowerExpr(*cl->elements[i]);
                    if (!ev) {
                        ev = lowerCollectionLiteralElementOrUnitNull(
                            *cl->elements[i], elemTy);
                        if (!ev) {
                            ctx_.diagnostics().report(
                                Severity::Error,
                                cl->elements[i]->span.begin,
                                "internal: lowerExpr returned null for "
                                "imported-property-setter inline-array "
                                "collection-literal element — #1386");
                            continue;
                        }
                    }
                    if (ev->getType() != elemTy &&
                        ev->getType()->isIntegerTy() &&
                        elemTy->isIntegerTy()) {
                        unsigned eb = ev->getType()->getIntegerBitWidth();
                        unsigned db = elemTy->getIntegerBitWidth();
                        ev = db > eb ? builder_.CreateSExt(ev, elemTy)
                                     : builder_.CreateTrunc(ev, elemTy);
                    }
                    llvm::Value* gep = builder_.CreateGEP(
                        aty, tmp,
                        {builder_.getInt32(0),
                         builder_.getInt32((unsigned)i)});
                    builder_.CreateStore(ev, gep);
                }
                v = builder_.CreateLoad(aty, tmp, "ipset.inlarr");
            }
        } else if (propTy && !cl->isDict) {
            v = materializeHeapCollectionLiteral(cl, propTy);
        } else if (propTy && !cl->keys.empty()) {
            v = materializeHeapDictLiteral(cl, propTy);
        }
    }
    if (!v) v = lowerExpr(value);
    // fix #2490: thread `value` through as valueExpr so
    // lowerImportedPropertySetValue can release a +1 producer passed
    // directly (see its own comment for the full rationale).
    return v ? lowerImportedPropertySetValue(target, v, &value) : false;
}


// fix #2862 (§6.6/§17.6/§2.8.2 clause 2/§12.1/§23.3, v0.240): the READ-MODIFY-
// WRITE twin of lowerImportedPropertySet (#2400). See the declaration in
// LlvmBackend_impl.hpp for the defect and the measurement; the two thin
// wrappers below share ONE body, per §17.6's own identification of `x++` with
// the §6.6 compound rule (the #2848 lesson, applied here so the next fix to
// this lowering cannot land in one spelling and miss the other).
bool Emitter::lowerImportedPropertyCompound(const ast::AssignmentStmt& as) {
    if (!as.target || !as.value) return false;
    std::string op(as.op);
    if (!op.empty() && op.back() == '=') op.pop_back();
    return lowerImportedPropertyCompoundCore(*as.target, op, as.value.get());
}


bool Emitter::lowerImportedPropertyIncrDecr(const ast::IncrementStmt& inc) {
    if (!inc.target) return false;
    // §17.6, verbatim: "For built-in numeric types, `x++` is equivalent to
    // `x := x + 1` (and likewise `x--` to `x := x - 1`)".
    return lowerImportedPropertyCompoundCore(*inc.target,
                                             inc.op == "++" ? "+" : "-",
                                             /*valueExpr=*/nullptr);
}


bool Emitter::lowerImportedPropertyCompoundCore(const ast::Expr& targetExpr,
                                                const std::string& binOp,
                                                const ast::Expr* valueExpr) {
    auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&targetExpr);
    if (!ma || !ma->target) return false;
    auto* ps = dynamic_cast<sema::PropertySymbol*>(ctx_.exprSymbol(*ma));
    if (!ps || ps->isIndexer) return false;
    // A LOCAL property carries an ast::PropertyDecl and is handled by
    // lowerPropertyCompound (which runs FIRST at both call sites); only the
    // imported shape — §2.8.2's metadata-reconstructed symbol, `decl == null`
    // — reaches here. Explicit rather than implied, so this can never
    // shadow the local path if the caller order ever changes.
    if (dynamic_cast<const ast::PropertyDecl*>(ps->decl)) return false;
    // fix #3231 (§12.3 / §23 / §2.8.2, v0.252): the COMPOUND half of the
    // property widening. `base.P OP= v` (§12.3 bypasses virtual dispatch)
    // reads AND writes through the OWNER's body directly. Dispatch through
    // `importedPropGetterDirectFns_[ps]` for the read and
    // `importedPropSetterDirectFns_[ps]` for the write — both populated by
    // declareImportedPropertyGetters / -Setters, both pointing at the
    // symbol the widened emitters define. Runs BEFORE
    // `rejectImportedVirtualBaseProperty`, which now only fires for the
    // ABSTRACT virtual shape.
    if (auto* baseId = dynamic_cast<const ast::IdentifierExpr*>(
            ma->target.get());
        baseId && baseId->name == "base" &&
        importedPropGetterDirectFns_.count(ps) &&
        importedPropSetterDirectFns_.count(ps)) {
        llvm::Function* getFn =
            importedPropGetterDirectFns_.find(ps)->second;
        llvm::Function* setFn =
            importedPropSetterDirectFns_.find(ps)->second;
        llvm::Value* recv = currentThis_;
        if (!recv) return false;
        // Read the OWNER's slot for the current value.
        auto* getFnTy = getFn->getFunctionType();
        llvm::Value* cur = builder_.CreateCall(getFnTy, getFn, {recv});
        emitPendingCheck();
        if (!cur) return false;
        // Apply the §6.6 compound operator.
        llvm::Value* rhs = valueExpr ? lowerExpr(*valueExpr) : nullptr;
        if (!rhs) {
            if (valueExpr) return false;
            rhs = emitImplicitOne(cur->getType());
            if (!rhs) return false;
        }
        const sema::TypeRef* opLeftTy = ctx_.exprType(*ma);
        const sema::TypeRef* opRightTy =
            valueExpr ? ctx_.exprType(*valueExpr) : nullptr;
        const bool rhsUnsigned =
            valueExpr && isUnsignedTargetExpr(*valueExpr);
        llvm::Value* newVal = emitCompoundOpTyped(
            binOp, cur, rhs, opLeftTy, opRightTy,
            cur->getType()->isFloatingPointTy(),
            isUnsignedTargetExpr(*ma), isCharTargetExpr(*ma), rhsUnsigned);
        if (!newVal) return false;
        // Write the OWNER's slot with the new value.
        auto* setFnTy = setFn->getFunctionType();
        llvm::Type* valTy = setFnTy->getParamType(1);
        newVal = coerceInitializerToSlot(newVal, valTy,
                                         isUnsignedTargetExpr(*ma));
        newVal = coerceToNullableSlot(valTy, newVal);
        newVal = boxForTarget(newVal, ctx_.exprType(*ma), valueExpr);
        builder_.CreateCall(setFnTy, setFn, {recv, newVal});
        emitPendingCheck();
        return true;
    }
    // fix #3199 (§12.3 / §23 / §2.8.2, v0.254): reject `base.P += v` /
    // `base.P++` on an IMPORTED VIRTUAL base property with a clean §12.3
    // error, the compound/incr twin of the plain-`:=` reject just above and
    // the read path's twin in `LlvmBackend_Exprs.cpp`. Pre-fix this branch
    // silently dropped the compound / increment too: the vtable-slot gate
    // below declined without a diagnostic, and the callers' generic
    // lvalueAddr fallback cannot resolve a PropertySymbol. Mirrors the
    // indexer twin (#2832), whose `rejectImportedVirtualBaseIndexer` is
    // called from `lowerIndexerCompound` (LlvmBackend_Assign.cpp ~2867).
    // MUST come BEFORE the vtable-slot decline below: that decline handles
    // `this.P OP= v` (virtual dispatch is correct there), so an ordering
    // that put the §12.3 reject AFTER it would silently drop the increment
    // spell — measured `base.P++` compiled cleanly on HEAD with only an
    // `internal:` warning and runtime `_v` left at 3 where 4 is correct.
    // fix #3231: after the direct-entry check above, this rejection now
    // only fires when no direct entry exists (the ABSTRACT virtual shape).
    if (rejectImportedVirtualBaseProperty(*ma, ps)) return false;
    // An imported VIRTUAL / INTERFACE property dispatches through its vtable
    // or itable slot (#2858 for the increment spelling, #481 for the compound
    // one) and must not be bound statically here. Both callers try those
    // branches first; decline explicitly for the same reason as above.
    if (propSetSlot_.count(ps) || ifacePropertySetIndex_.count(ps) ||
        propGetSlot_.count(ps) || ifacePropertyGetIndex_.count(ps))
        return false;
    auto git = importedPropGetterFns_.find(ps);
    auto sit = importedPropSetterFns_.find(ps);
    // fix #2885 (§22.12/§22.13/§6.6/§17.6, v0.240): the STATIC shape rides its
    // own no-`this` stub pair. Pre-fix this function declined for a static
    // property (the `mod == "static"` gate below), so `HS.SP += 1` / `HS.SP++`
    // fell through to the same dropped-write fallback the plain `:=` did
    // (measured 0 where 110 / 212 is correct). Both stubs must be present:
    // §6.6 reads AND writes, so a property missing either half is not this
    // lowering's shape.
    auto sgIt = importedStaticPropGetterFns_.find(ps);
    auto ssIt = importedStaticPropSetterFns_.find(ps);
    const bool isStaticProp = sgIt != importedStaticPropGetterFns_.end() &&
                              ssIt != importedStaticPropSetterFns_.end();
    if (!isStaticProp && (git == importedPropGetterFns_.end() ||
                          sit == importedPropSetterFns_.end())) return false;
    llvm::Function* getFn = isStaticProp ? sgIt->second : git->second;
    llvm::Function* setFn = isStaticProp ? ssIt->second : sit->second;
    auto* getFnTy = getFn->getFunctionType();
    auto* setFnTy = setFn->getFunctionType();
    const unsigned valueArgIdx = isStaticProp ? 0u : 1u;
    if (getFnTy->getNumParams() != (isStaticProp ? 0u : 1u) ||
        setFnTy->getNumParams() != valueArgIdx + 1) return false;
    // §23: "get/set share one declared type" — the getter's result and the
    // setter's `value` parameter are both `lowerType(P's declared type)`. A
    // disagreement means this is not the shape this lowering understands;
    // decline rather than emit a call the IR verifier would reject. Checked
    // BEFORE anything is emitted, so declining leaves no half-lowered
    // statement behind (the discipline #2204 established for
    // lowerImportedPropertySet).
    if (getFnTy->getReturnType() != setFnTy->getParamType(valueArgIdx))
        return false;
    // MIRROR THE EXPORTER'S OWN GATE, so this can never emit a call to a stub
    // nothing defines. `declareImportedPropertySetters` declares the external
    // reference for nearly every imported PropertySymbol, but
    // `emitExportedPropertySetters` (LlvmBackend_Setup.cpp) DEFINES it only for
    // a public, non-generic Class-or-Record type's public, non-static,
    // non-virtual property. Calling outside that set turns a program that
    // compiles and links today (with the silently-dropped write this issue is
    // about) into a hard `undefined reference` LINK ERROR — strictly worse, and
    // exactly the over-broad first draft #2474's own review caught on the
    // sibling getter path. Declining leaves those shapes precisely as they are
    // today; they are separate gaps, FILED rather than silently absorbed:
    //   * generic parent — #2884: the PLAIN `:=` on an imported generic
    //     class's property is ALREADY a hard link error today (measured:
    //     `undefined reference to bz$HLib.HG$P$propset`), because
    //     declareImportedPropertySetters has no generic gate while the
    //     exporter does. Resolving it needs the per-instantiation stub naming
    //     of §2.8.3 rule 4 — not this fix's to invent.
    //   * static — CLOSED by #2885, which gave the static case its own
    //     no-`this` accessor ABI (`$spropget`/`$spropset`) plus §22.13's lazy
    //     init at the access site; the `isStaticProp` branch above now takes
    //     that path instead of declining. The gate below still declines for a
    //     static property whose stubs are absent (a static AUTO-property,
    //     which is static STORAGE, not an accessor).
    //   * Enum: also lowers its receiver to a bare scalar `i32`, never a
    //     pointer or aggregate (#2474/#2489 carry the same guard).
    // An imported AUTO-property is direct instance STORAGE in the importing
    // module's layout (#2478 widened `storageMemberOf` to
    // `isAutoPropertyStorageSymbol` precisely so `lvalueAddr`/`fieldAddr` resolve its
    // slot), so the established path already reads and writes it correctly —
    // MEASURED: `b.A++` / `b.A += 4` on an imported auto-property give the
    // right answer both before and after this fix. Declining keeps that path,
    // rather than newly re-routing a working shape through the accessor stubs
    // and its different ARC discipline (§23.1/§28.9). This is the "do not widen
    // past the measured defect" line: the dropped write is the BLOCK-form
    // property, whose accessors are the only way in.
    if (sema::isAutoPropertyStorageSymbol(ps, ctx_)) return false;
    if (!ps->parent) return false;
    if (ps->parent->kind != ast::TypeDeclKind::Class &&
        ps->parent->kind != ast::TypeDeclKind::Record) return false;
    if (!ps->parent->genericParams.empty()) return false;
    if (ps->parent->visibility != ast::Visibility::Public) return false;
    if (ps->visibility != ast::Visibility::Public) return false;
    // fix #2885: a static property with no stub pair (a static AUTO-property)
    // still declines here; one WITH the pair took the static branch above.
    if (!isStaticProp && isStaticProperty(ps)) return false;

    // --- §6.6: resolve the RECEIVER's storage ONCE, share it with both calls.
    // Mirrors lowerImportedPropertySetValue's #2489 receiver resolution
    // verbatim: prefer lvalueAddr (a record receiver is by-value per §9.2, so
    // lowerExpr would load a COPY and the setter would mutate a throwaway
    // slot); fall back to lowerExpr + spill only for a receiver that is not an
    // addressable place. A STATIC property has no receiver at all (§22.12) —
    // `ma->target` names the TYPE — so the whole resolution is skipped.
    llvm::Value* obj = nullptr;
    if (!isStaticProp) {
        llvm::Type* objElemTy = nullptr;
        if (llvm::Value* addr = lvalueAddr(*ma->target, objElemTy)) {
            if (objElemTy && objElemTy->isPointerTy())
                obj = builder_.CreateLoad(objElemTy, addr);
            else if (objElemTy && objElemTy->isAggregateType())
                obj = addr;  // the address of the receiver's real storage
        }
        if (!obj) {
            llvm::Value* ov = lowerExpr(*ma->target);
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
    } else {
        // §22.13: the read-modify-write is a static-member access; run the
        // declaring type's lazy initialization once, before the getter.
        emitEnsureStaticInit(ps->parent);
    }

    // --- §6.6's order: the LHS chain (above), then the getter. ---
    llvm::Value* cur = isStaticProp
                           ? builder_.CreateCall(getFnTy, getFn, {})
                           : builder_.CreateCall(getFnTy, getFn, {obj});
    emitPendingCheck();   // §31.1/§31.2: a throw out of the imported getter
    if (!cur) return false;

    // --- then `e` (or §17.6's implicit `1`), then the operation. ---
    llvm::Value* rhs = nullptr;
    if (valueExpr) {
        rhs = lowerExpr(*valueExpr);
    } else {
        // §17.6's right operand for the `++`/`--` spelling, synthesized at the
        // getter's result type because the desugaring has no AST node for it —
        // from the ONE definition of that rule (#2888), which covers the
        // struct-typed `decimal` and the tagged nullable-value shapes that
        // `ConstantInt::get` cannot express.
        rhs = emitImplicitOne(cur->getType());
    }
    if (!rhs) {
        // #1386/#2509 precedent: an ICE beats a silently dropped store.
        ctx_.diagnostics().report(
            Severity::Error,
            valueExpr ? valueExpr->span.begin : targetExpr.span.begin,
            "internal: could not lower the right operand of a compound "
            "assignment to an imported property — #2862");
        return false;
    }
    // §5.1/§6.4 (#2883): the caller-side `decimal` check #2862 added here — a
    // ternary routing a `%bz.decimal` `cur` to emitDecimalArith — is GONE.
    // emitCompoundOp now owns the decimal arm itself (LlvmBackend_Layout.cpp),
    // so this call site holds one fewer copy of the same knowledge; #2862's
    // note said the check "should be deleted when that lands".
    // fix #2904 (§25.1/§25.2/§6.6, v0.240): via emitCompoundOpTyped, so a
    // RECORD- or class-typed IMPORTED property with a user `operator <op>`
    // takes §25's desugaring instead of feeding the struct to
    // `llvm.sadd.with.overflow`. (NOT separately probed across a two-module
    // build: this is the same missing arm the block-form, interface and
    // indexer spellings were MEASURED to have, reached through the same one
    // function.)
    llvm::Value* res = emitCompoundOpTyped(
        binOp, cur, rhs, ctx_.exprType(targetExpr),
        valueExpr ? ctx_.exprType(*valueExpr) : nullptr,
        cur->getType()->isFloatingPointTy(),
        isUnsignedTargetExpr(targetExpr),
        isCharTargetExpr(targetExpr),                     // §6.6/§5.2 #1961
        valueExpr && isUnsignedTargetExpr(*valueExpr));   // §6.2 #2140
    if (!res) return false;

    // --- then the store. ---
    llvm::Type* valTy = setFnTy->getParamType(valueArgIdx);
    llvm::Value* sv = coerceNumericToType(res, valTy,
                                          isUnsignedTargetExpr(targetExpr));
    if (sv->getType() != valTy) return false;   // see the agreement note above
    if (isStaticProp) builder_.CreateCall(setFnTy, setFn, {sv});
    else              builder_.CreateCall(setFnTy, setFn, {obj, sv});
    emitPendingCheck();   // §31.1/§31.2: a throw out of the imported setter

    // fix #2576's structure, mirrored (§23.3/§28.9/§4.6). For a string-typed
    // property, emitCompoundOp dispatches to emitStringConcat, which ALWAYS
    // mints a fresh rc=1 `res` (memcpy of both operands' bytes, #422), and the
    // imported setter stub forwards to ensurePropertyAccessorFn's body, which
    // RETAINS its incoming `value` for a CLASS-owned property (#2490). So
    // `res`'s +1 is doubled and must be discharged here — unconditionally on
    // (classOwned && pointer), NOT gated on the RHS being a producer, because
    // emitStringConcat mints +1 for a BORROW rhs too (that is exactly the half
    // #2568 left open and #2567/#2576 closed). `rhs`'s own +1 is a SECOND
    // orphan when the RHS AST is itself a producer — emitStringConcat copies
    // its bytes and leaves its refcount untouched — and is released only then
    // (a named local is a BORROW; releasing it would drop its owner's only
    // reference). CLASS-only gate (§9/§9.7): a RECORD's setter plain-stores,
    // so `res`'s +1 IS the field's reference and releasing it would dangle the
    // field — the live UAF shape #2490/#2506/#2548/#2576 all record. `cur` is
    // a BORROW (the getter returns a field load with no retain) and is
    // deliberately never released.
    bool classOwned = ps->parent && ps->parent->kind == ast::TypeDeclKind::Class;
    if (classOwned && !diverged_) {
        if (res->getType()->isPointerTy())
            emitStringRelease(res);
        if (rhs->getType()->isPointerTy() && valueExpr &&
            isPlusOneStringProducer(valueExpr))
            emitStringRelease(rhs);
    }
    return true;
}


bool Emitter::lowerPropertyCompound(const ast::AssignmentStmt& as) {
    if (!computedSetterOf(*as.target)) return false;
    llvm::Value* cur = lowerExpr(*as.target);   // getter read (CG33c)
    llvm::Value* rhs = lowerExpr(*as.value);
    if (!cur || !rhs) return false;
    std::string op(as.op);
    if (!op.empty() && op.back() == '=') op.pop_back();
    // fix #2904 (§25.1/§25.2/§6.6, v0.240): via emitCompoundOpTyped, so a
    // RECORD- or class-typed property whose type declares `operator <op>` takes
    // §25's read-modify-write desugaring. Before this, `b.PP += one` on a
    // `Pair`-typed block-form property handed the record struct to
    // `llvm.sadd.with.overflow` and ABORTED the compile — while the LOCAL
    // spelling (#610) and the heap-collection spelling (#626) each did the
    // lookup from their own copy of it.
    llvm::Value* res = emitCompoundOpTyped(
        op, cur, rhs, ctx_.exprType(*as.target), ctx_.exprType(*as.value),
        cur->getType()->isFloatingPointTy(),
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


// fix #2905 / §7.7 / §6.6 / §23.3 / §23.4: see the declaration comment in
// LlvmBackend_impl.hpp for the defect, the measurement and the contract.
bool Emitter::lowerAccessorNullCoalescingAssign(const ast::AssignmentStmt& as) {
    if (as.op != "??=" || !as.target || !as.value) return false;
    auto* ps = dynamic_cast<sema::PropertySymbol*>(ctx_.exprSymbol(*as.target));
    auto* ix = dynamic_cast<const ast::IndexExpr*>(as.target.get());
    // The POSITIVE gate: §6.6's "when the final segment is accessor-mediated (a
    // property or an indexer)". Everything else — a local, a parameter, a
    // field, a static, an auto-property — has an address and keeps the caller's
    // existing (correct, well-tested) `lvalueAddr` path.
    if (!ps && !ix) return false;

    // fix #2929 (§6.6/§7.7/§23.4, v0.240): EVALUATE THE LHS CHAIN ONCE.
    //
    // §6.6, verbatim, on the chain (the clause `??=`'s own paragraph points
    // back to with "The LHS chain is evaluated once as above"): "every
    // constituent of the left-hand side's lvalue chain (grammar A.6 `LValue`)
    // is evaluated exactly once, left to right: the base identifier or `this`,
    // each mid-chain member hop (a property hop's getter is invoked once as
    // part of locating the target), and every index expression in the chain".
    //
    // This function reads through `lowerExpr(*as.target)` and writes through a
    // setter helper that re-lowers the target's own subexpressions from the
    // AST, so every such constituent ran TWICE. MEASURED on 10bd6d3 with a
    // counter-returning index — `b[Ctr.NextIdx()] ??= Ctr.Val()` gave `idx=2`
    // where §6.6 requires 1 (getter 1, setter 1, RHS 1 were already right).
    //
    // The mechanism is the one §22.3's accessor `ref`/`out` bracket already
    // uses for the identical requirement (`emitAccessorRefArgCopyIn`, this
    // file): lower each constituent ONCE and pin it in `exprValueOverride_`,
    // which `lowerExpr` consults first (LlvmBackend_Exprs.cpp), so the getter
    // read below AND the setter's internal re-lowering both reuse the pinned
    // value instead of re-running the expression. The pins are dropped at
    // every exit. An expression already pinned by an enclosing bracket is left
    // alone (and not erased) so the two cannot fight over one node.
    std::vector<const ast::Expr*> pins;
    auto pin = [&](const ast::Expr* e) -> bool {
        if (!e || exprValueOverride_.count(e)) return true;
        llvm::Value* v = lowerExpr(*e);
        if (!v) return false;
        exprValueOverride_[e] = v;
        pins.push_back(e);
        return true;
    };
    auto unpinAll = [&] {
        for (const ast::Expr* e : pins) exprValueOverride_.erase(e);
        pins.clear();
    };
    if (ix) {
        // §6.6's "the base identifier or `this`, each mid-chain member hop …
        // and every index expression in the chain", left to right. A.6 keeps
        // the base itself an `Identifier`/`this`, but a MID-CHAIN hop may be a
        // property (`a.Inner[i] ??= v`), whose getter §6.6 says is "invoked
        // once as part of locating the target" — so the receiver is pinned
        // too, not just the indices.
        if (!pin(ix->target.get())) { unpinAll(); return false; }
        for (const auto& iv : ix->indices)
            if (!pin(iv.value.get())) { unpinAll(); return false; }
    } else if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(
                   as.target.get())) {
        // The same mid-chain hop for the PROPERTY spelling (`a.Inner.P ??=
        // v`): the setter helper re-lowers `ma->target`, re-invoking the
        // `Inner` getter. #2905 recorded this as "nothing observable re-runs"
        // on the grounds that A.6 forbids a method-call base — but a property
        // hop is not a method call and §6.6 names it explicitly.
        if (!pin(ma->target.get())) { unpinAll(); return false; }
    }

    // §6.6: "The LHS chain is evaluated once as above, the getter once".
    llvm::Value* cur = lowerExpr(*as.target);
    if (!cur) { unpinAll(); return false; }

    // The null test. §7.2: a nullable VALUE type is the tagged
    // `{i8 hasValue; T}` struct; a nullable REFERENCE (`string?`, a class) is a
    // bare pointer. A target of neither shape cannot be null, so §7.7's
    // assignment can never occur and there is nothing to emit — decline rather
    // than branch on a fabricated condition.
    llvm::Value* isNull = nullptr;
    if (cur->getType()->isPointerTy())
        isNull = builder_.CreateICmpEQ(
            cur, llvm::ConstantPointerNull::get(ptrTy()), "coaleq.isnull");
    else if (auto* st = llvm::dyn_cast<llvm::StructType>(cur->getType()))
        if (st->isLiteral() && st->getNumElements() == 2 &&
            st->getElementType(0) == nullablePrimHasValTy())
            isNull = builder_.CreateNot(nullablePrimHasValue(cur));
    if (!isNull) { unpinAll(); return false; }

    auto* assignBB =
        llvm::BasicBlock::Create(llvm_, "coaleq.acc.do", currentFn_);
    auto* endBB =
        llvm::BasicBlock::Create(llvm_, "coaleq.acc.end", currentFn_);
    builder_.CreateCondBr(isNull, assignBB, endBB);
    builder_.SetInsertPoint(assignBB);

    // §6.6: "`e` is evaluated — and the setter invoked — only when the
    // assignment actually occurs." Both live inside this branch, and nothing
    // above this point lowered `e`.
    bool stored = false;
    llvm::Value* rhs = lowerExpr(*as.value);
    if (rhs) {
        // §5.1 to the target's slot, then §7.2's present-wrap of a bare payload
        // (and the `null` literal's normalization to the all-zero form) — the
        // same two steps, from the same two helpers, that every other store
        // into a nullable slot uses.
        rhs = coerceInitializerToSlot(rhs, cur->getType(),
                                      isUnsignedTargetExpr(*as.value));
        rhs = coerceToNullableSlot(cur->getType(), rhs);
        // The store, dispatched in the SAME order the plain `:=` arm of
        // lowerStmt uses: the virtual/interface vtable path first (so an
        // override is not bypassed), then the local block-form setter, then the
        // imported-property ABI stub, then the user indexer, then the built-in
        // heap collection.
        if (ps && !ps->isIndexer &&
            (propSetSlot_.count(ps) || ifacePropertySetIndex_.count(ps)))
            stored = lowerVirtualPropertySet(*as.target, rhs, as.value.get());
        if (!stored)
            stored = lowerPropertySetValue(*as.target, rhs, as.value.get());
        if (!stored)
            stored =
                lowerImportedPropertySetValue(*as.target, rhs, as.value.get());
        if (!stored && ix) stored = lowerIndexerSet(*ix, rhs, as.value.get());
        if (!stored && ix)
            stored = lowerCollectionIndexerSet(*ix, rhs, as.value.get());
    }
    if (!builder_.GetInsertBlock()->getTerminator())
        builder_.CreateBr(endBB);
    builder_.SetInsertPoint(endBB);
    // fix #2929: the getter (unconditional) and the setter (assign branch) are
    // both behind us, so the pins have no further readers — drop them at the
    // JOIN, where any later lowering of the same nodes is a genuinely new
    // evaluation. Kept until here rather than released early because the
    // setter helpers re-lower from the AST inside `assignBB`.
    unpinAll();
    if (!stored && !ctx_.diagnostics().hasErrors())
        // #1386/#2509 precedent: an ICE beats a silently dropped store. The
        // whole point of #2905 is that this shape produced NO diagnostic.
        ctx_.diagnostics().report(
            Severity::Error, as.target->span.begin,
            "internal: could not lower the setter of a null-coalescing "
            "assignment to an accessor-mediated target — #2905");
    return true;
}


// per §24 / §6.5: `event += handler` and `event -= handler` are a fixed,
// language-defined desugaring to `event.Add(handler)` and
// `event.Remove(handler)`. They are NOT the compound-assignment statements of
// §17 (they neither read nor assign the event). The form is available wherever
// the source is in scope, i.e. to the declaring class for an owned field, and
// for either of two shapes the codegen must recognize:
//
//   1. The legacy member-kind form (`public event Clicked: Ev;` — retired in
//      v1) — `eventSymbolOf(*as.target)` returns a non-null `EventSymbol`. The
//      source pointer comes from `eventSourcePtr` (which loads the EventSymbol
//      backing source via the field slot / itable accessor / static global).
//   2. The v1 §6.5 pattern (an `owned [readonly] EventSource<T>` /
//      `AsyncEventSource<T>` / `SharedEventSource<T>` field, with or without a
//      separate `IEvent<T>` property face) — the target expression carries a
//      regular VariableSymbol/MemberSymbol with static type one of the three
//      event-source classes. There is no EventSymbol; the runtime rep IS the
//      field's loaded value (a bz_event_source_t*, the same `lowerExpr`
//      produces for any other field/parameter of the same type).
//
// Returns true when handled.
bool Emitter::tryLowerEventCompound(const ast::AssignmentStmt& as) {
    if (!as.target || !as.value) return false;
    if (as.op != "+=" && as.op != "-=") return false;
    const sema::TypeRef* payloadTy = nullptr;
    llvm::Value* src = nullptr;
    if (const sema::EventSymbol* es = eventSymbolOf(*as.target)) {
        // Legacy member-kind event (§24, retired in v1).
        src = eventSourcePtr(*as.target);
        if (!src) return false;
        payloadTy = eventPayloadType(es);
    } else {
        // v1 §6.5 pattern: target's STATIC type identifies the field as an
        // event source. The runtime value is the loaded field — load via
        // lowerExpr exactly as any other field/parameter of the type would
        // be read; the runtime rep (bz_event_source_t*) is the same one
        // `eventSourcePtr` would produce for the legacy form, and the same
        // one `dispatchEventMethods` consumes for a method-call spelling
        // (`_e.Add(h)`). genericArgs[0] carries the payload type, matching
        // the sema branch at #4792 (the parameter-inference fix whose
        // payload source this codegen reads from — ExprTypecheck.cpp).
        const sema::TypeRef* tgtTy = ctx_.exprType(*as.target);
        if (!isEventSourceTypeName(tgtTy) && !isSharedEventSourceTypeName(tgtTy))
            return false;
        if (tgtTy && !tgtTy->genericArgs.empty()) payloadTy = tgtTy->genericArgs[0];
        // fix #4901 (stdlib §6.5 "Capture-freedom at every subscribe
        // position", spec §28.14, v0.307): `+=` on a SharedEventSource<T> is
        // the desugaring of `.Add(handler)` (spec §17.2), a subscribe position
        // of the shared pair. Sema (#4920) rejects a capturing handler here,
        // and its assignability check rejects a wrong arity, a mismatched
        // parameter type or an `owned` formal, but it judges an overload group
        // by the overload it happens to bind and admits a non-capturing
        // lambda. Codegen lowers only what it has measured sound,
        // so apply the SAME gate the `.Subscribe`/`.Add` call arms use
        // (sharedEventHandlerAdmissible, LlvmBackend_Calls2.cpp): a static,
        // non-overloaded method reference of exactly `(T) -> ()`. Unlike those
        // arms there is no #2457 backstop behind this statement, so reject
        // here. Lowering a capture-free lambda on this pair is #4972 (a
        // spurious reject today). `-=` (Remove, "call-bounded: compares only")
        // is not a subscribe position and is left as it was; its per-call
        // closure-record leak is #4970.
        if (as.op == "+=" && isSharedEventSourceTypeName(tgtTy) &&
            !sharedEventHandlerAdmissible(
                ctx_, as.value.get(), as.target.get(), payloadTy,
                [this](std::string_view n) { return isBuiltinTypeRefName(n); },
                // #4978 scope: only the `.Subscribe`/`.Add`/`.Remove` arms
                // honour a selector-chosen overload; `+=` on an overloaded
                // group stays the #4901 deliberate spurious reject.
                /*honourSelectedOverload=*/false)) {
            ctx_.diagnostics().report(
                Severity::Error, as.value->span.begin,
                "`+=` on a `SharedEventSource<T>` is lowered only for a "
                "static, non-overloaded method reference whose signature is "
                "exactly `(T) -> ()` for the source's payload type `T` "
                "(stdlib §6.5); this handler is not one — a lambda handler on "
                "this pair is not yet lowered (#4972), and an overloaded, "
                "`owned`-formal or mistyped method group is rejected");
            return true;
        }
        src = lowerExpr(*as.target);
        if (!src) return true;  // error path; avoid bad fallthrough
    }
    // Lower the handler to a closure, hinting the expected (payload)->() type
    // so an untyped lambda param resolves the payload type.
    const sema::TypeRef* saved = expectedFunctionType_;
    sema::TypeRef handlerTy;
    handlerTy.kind = sema::TypeRef::Kind::Function;
    if (payloadTy) {
        sema::TypeRef::FuncParam p;
        p.type = const_cast<sema::TypeRef*>(payloadTy);
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

// fix #3082 (§22.5 / §2.8.2, v0.244): the imported-CONSTRUCTOR twin of
// `argsCanFill`. Same rule, same return value on every input the sema
// binder (`bindArgsToParams` over `FormalsView::of(cs)`) accepts; differs
// only in the source of the parameter list (an `ast::Param` vector vs. a
// `ConstructorSymbol` whose params are `ParameterSymbol`s with `hasDefault`
// carried on the wire by #3178). Pure check — no IR emitted — consumed by
// `ctorForArgs`'s fallback to select the right ctor before lowering its
// arguments.
bool Emitter::argsCanFillSymbolic(
        const sema::ConstructorSymbol& cs,
        const std::vector<ast::CallArgument>& args) {
    const std::size_t n = sema::formalParamCount(cs);
    std::vector<bool> filled(n, false);
    std::size_t pos = 0;
    for (const auto& a : args) {
        std::size_t idx;
        if (a.name.empty()) {
            if (pos >= n) return false;
            idx = pos++;
        } else {
            idx = n;
            for (std::size_t j = 0; j < n; ++j)
                if (sema::formalParamName(cs, j) == a.name) { idx = j; break; }
            if (idx >= n) return false;
        }
        if (filled[idx]) return false;
        filled[idx] = true;
    }
    for (std::size_t j = 0; j < n; ++j)
        if (!filled[j] && !sema::formalParamHasDefault(cs, j)) return false;
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
    // fix #3355 / §29.2 / §23.2 (v0.278) — interim SIGSEGV-elimination fix:
// widen the gate to also accept `IOwningEnumerable` as the target head.
// Pre-fix this gate keyed on `headName() == "IEnumerable"` and declined
// every other name, so an `IOwningEnumerable<T>`-returning source returned
// the raw backing pointer (no vtable at offset 0 — the for-in at the
// call site read slot 4 of the raw backing as a function pointer →
// SIGSEGV). #2769's commentary at line 193 names this defect as a
// separate pre-existing issue: "the IOwningEnumerable arm of the wrap
// function itself is broken upstream of #2769's scope. Filed as a
// separate issue" — that issue is #3355.
//
// SCOPE AND LIMITATIONS. This fix eliminates the SIGSEGV but does NOT
// implement spec-correct IOwningEnumerable semantics. The downstream
// dispatch reuses the existing wrap helpers (`wrapOwnedArrayAsIEnumerable`
// at line 7689, `wrapOwnedLinkedListAsIEnumerable` at line 7739,
// `drainLazyPipelineToOwnedIEnumerable` at line 7717,
// `wrapUserClassAsIEnumerable` at line 7771), each of which installs
// the BORROWING `Current` (slot 3) — a function that returns the
// element pointer at the current index without transferring ownership
// out of the underlying collection. The IOwningEnumerable<T> spec
// (§29.1 / §29.11) requires slot 3 to be `TakeCurrent` — a function
// that transfers ownership of the pending element AND nulls the
// pending slot, so the collection's slot is empty after each yield
// (single-consumption drain per §29.11). With this commit, slot 3
// remains the borrowing Current; the for-in desugaring marks the loop
// variable as `owned` (sema enforces auto-disposal at iteration end,
// LlvmBackend_Stmts.cpp:7602-7607), and the array's element disposer
// is null for non-string class elements — so the test under
// `tests/codegen/iowningenumerable_getter_value_arm_3355_test.cpp`
// observes exact-once disposal even though the ownership is being
// discharged by the LOOP'S auto-dispose rather than the slot-3
// TakeCurrent. Re-iterating the same IOwningEnumerable reads the same
// items twice (the array's slots are not nulled), violating
// single-consumption.
//
// The class-element follow-up (#3364) is the right home for the
// TakeCurrent-primitive fix: a distinct slot-3 primitive that nulls
// the pending slot on transfer AND updates the wrap helpers to install
// it for IOwningEnumerable targets. The "value-element arm" framing
// in the lane brief does not apply — `IOwningEnumerable<T>` constrains
// `where T: class` (Bezel_Standard_Library.md §4.13, v0.278) and so
// has no value-element arm; the wrap helpers' slot-3 reuse of the
// borrowing Current is the SAME defect for both class and (hypothetical)
// value elements.
    // fix #3419 (§29.2 / stdlib §4.12–§4.13, v0.283): this gate is the MINT
    // side of the enumerable-wrap grid — it decides which declared heads take a
    // §29.2 wrapper frame at all, and every ownership-CLAIM site downstream is
    // only correct while it agrees. #3355 widened it by hand and #3392 was the
    // claim predicate that did not follow. Both now read one classifier.
    if (!(targetTy->kind == sema::TypeRef::Kind::Named &&
          !targetTy->declaration && targetTy->astNode &&
          targetTy->astNode->name && !targetTy->astNode->name->parts.empty() &&
          common::isSyncIterFormatEnumerableHead(targetTy->headName())))
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
    // fix #3419 (§29.2 / stdlib §4.4–§4.5 / §4.11 / §4.14, v0.283): the grid's
    // SOURCE axis. This function is the wrap AUTHORITY, and its own list was
    // still one of four verbatim copies of these seven names (the local-decl
    // mint, `nestedIEnumViewNeeded`, and — until #3405 deleted it — the
    // property face). One table now, in the registry, so adding a family
    // member is one edit rather than a search.
    bool actualIsArray = common::isArrayEnumeratorWrapSourceName(an);
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
                freeCollectionSourceNow(v, actualTy);
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
        // disposes the enumerator exactly once). A borrowed source keeps the
        // plain wrap (the source's owner retains the disposal obligation).
        //
        // fix #3450 (§28.2 / §28.9 / §28.11; stdlib §4.11, v0.283): the
        // Stack/OwningStack arm USED to read "has no OWNING-wrap variant in
        // this fix", build the BORROWING wrap and then call
        // `freeCollectionSourceNow` on the backing right here — handing the
        // caller an enumerator over memory this function had just freed.
        // Every subsequent MoveNext/Current read freed memory: measured on
        // 0c5ecd1, `order=0` (a SILENT zero-iteration loop) on the plain
        // build and an ASan SEGV in `bz$stackenum$i32$Current` on the
        // sanitized one — #2633's oracle split in one program. That claim
        // stopped being true at #2987, which built `wrapOwnedStackAsIEnumerator`
        // for exactly this shape (the local-decl site at
        // `LlvmBackend_Setup.cpp` has used it since v0.240). This is the
        // LinkedList twin's fix (#3354) applied to the stack: use the OWNING
        // sibling and drop the immediate free, so the backing is reclaimed by
        // the wrap's slot-1 Dispose at the CALLER's scope end — after
        // iteration, not before it.
        if (outWrapMade) *outWrapMade = true;
        if (srcOwnedByCallee) {
            if (an == "Stack" || an == "OwningStack")
                return wrapOwnedStackAsIEnumerator(v, targetTy);
            return wrapOwnedArrayAsIEnumerable(v, targetTy);
        }
        if (an == "Stack" || an == "OwningStack")
            return wrapStackAsIEnumerator(v, targetTy);
        return wrapArrayAsIEnumerable(v, targetTy);
    }
    // fix #1069 / stdlib §4.10 / §5: DICT-backed `Set<T>` — snapshot its element
    // keys into a fresh array and hand out an OWNING enumerator over the
    // snapshot. Reuses the tested #389 Set lazy source (bz_lazy_from_set, which
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
            // fix #3568: the null value disposer is EXPLICIT here, not a
            // default — a dict-backed Set's hash table has keys and no values,
            // so there is no value slot for a disposer to reach. (Its string
            // KEYS are a separate, pre-existing gap: they are released by
            // `bz_set_string_keys_release`, a scope-end pass this RETURN
            // coercion does not run — see freeDictSourceNow's #2710 note.)
            if (srcOwnedByCallee)
                freeDictSourceNow(v, /*valDisposer=*/nullptr);
            return drainLazyPipelineToOwnedIEnumerable(lazy, targetTy);
        }
    }
    // fix #3695 (stdlib §4.18 / stdlib §4.1 / §29.2 / §28.3, v0.286): SortedSet<T>'s arm of the
    // §29.2 wrap grid — MISSING ENTIRELY until this commit. A `SortedSet<T>`
    // coerced to `IEnumerable<T>` (an argument, a local-decl init, a method
    // RETURN) used to fall off the end of this function to the bare `return v`
    // and hand the caller the raw `bz_dict_t*`. §29.2's desugaring then read
    // that value's first word as a vtable and called slot 4 (GetEnumerator):
    // measured as a deterministic SIGSEGV (rc 139) with ASan reporting a
    // null-address call (`pc 0x0`), at BOTH faces this function serves — the
    // RETURN coercion and the ARGUMENT coercion — while the same SortedSet
    // reached through its concrete type (`.GetEnumerator()` direct, `for x in
    // s`) iterates cleanly through `wrapSortedSetAsIEnumerable`. The direct /
    // for-in surfaces both call `wrapSortedSetAsIEnumerable`
    // (`LlvmBackend_Calls2.cpp:14146`); this arm extends the §29.2 coercion to
    // route through the SAME helper, so the interface-upcast path is
    // structurally identical to the direct path rather than a separately
    // (and incorrectly) re-derived one.
    //
    // SortedSet is ALWAYS hash-backed (`where T: IComparable<T>` excludes
    // class elements, so the dense-array path is unreachable — same reason
    // the #2546 SortedSet.GetEnumerator arm uses `wrapSortedSetAsIEnumerable`
    // rather than the array wrap), so the OrderedSet/SortedDictionary sibling
    // arm here is the right model.
    //
    // OWNERSHIP. `wrapSortedSetAsIEnumerable` already returns an OWNING
    // enumerator over a fresh sorted snapshot (`sortedSetSnapshotToSortedArray`
    // → `wrapOwnedArrayAsIEnumerable`) — the wrapper's slot-1 Dispose frees
    // the snapshot array exactly once at the caller's scope end. The snapshot
    // holds +1 references to the source's keys (a string slot is the
    // SortedSet's own bz_string*, +1, exactly as `bz_lazy_from_set` arranges);
    // releasing the source via `bz_dict_free` decrements those refs, and the
    // snapshot's independent +1 references keep the keys alive for iteration.
    // So a callee-OWNED source (the RETURN site) frees its source RIGHT HERE
    // — mirror #2299's `Set`/`Dictionary` arms — and a BORROWED source (the
    // ARGUMENT / local-decl sites) lets the source's binder reclaim at its
    // own scope end. `freeDictSourceNow(v, /*valDisposer=*/nullptr)` is the
    // right free: SortedSet is a hash table of keys with no values, so the
    // value-disposer argument is null.
    if (actualTy && (actualTy->headName() == "SortedSet" ||
                     builtinTypeName(actualTy) == "SortedSet")) {
        // fix #4099 (stdlib §4.18, v0.306 follow-up): the widening-coercion
        // twin of #3976's indexer/for-in check. A `SortedSet<T>` coerced to
        // `IEnumerable<T>` here used to silently admit an element type that
        // is eligible ONLY via the auto-hashable (§9.2) or class/interface
        // (hasObjectHeaderEquality) branches — both of which establish
        // hashing/equality but not ordering. Reject before the wrap so the
        // compile fails consistently with `.Add`/`.Contains` (fix #3947) and
        // the indexer/for-in (fix #3976). Reuses the shared authority
        // `reportSortedElementNeedsCompareTo` (LlvmBackend_Calls3.cpp) rather
        // than a fourth inline copy of the predicate (LANE_RULES §15). The
        // poison-value return mirrors the indexer's own — well-formed no-op.
        if (reportSortedElementNeedsCompareTo(
                actualTy, actualTy->genericArgs[0], srcExpr->span.begin,
                "the `IEnumerable<T>` coercion")) {
            if (outWrapMade) *outWrapMade = false;
            return llvm::PoisonValue::get(ptrTy());
        }
        if (outWrapMade) *outWrapMade = true;
        // Snapshot the source FIRST, then free the source. The snapshot is
        // built FROM the live source dict (bz_dict_keys_gen reads the key
        // bytes), and the snapshot is independent of the source's allocation
        // once built — so reversing the order would UAF on the source's key
        // bytes. Mirror the local-decl site's snapshot-then-free ordering.
        llvm::Value* wrap = wrapSortedSetAsIEnumerable(v, targetTy);
        if (wrap && srcOwnedByCallee)
            freeDictSourceNow(v, /*valDisposer=*/nullptr);
        return wrap;
    }
    // fix #3519 (§29.2 / §28.9 / §28.3, v0.283; stdlib §4.8 / §4.17): the
    // Dictionary family's arm of the §29.2 wrap grid — MISSING ENTIRELY until
    // this commit. A `Dictionary<K,V>` coerced to
    // `IEnumerable<DictionaryEntry<K,V>>` fell off the end of this function to
    // the bare `return v` and handed the caller the raw `bz_dict_t*`. §29.2's
    // desugaring then reads that value's first word as a vtable and calls slot
    // 4 (`GetEnumerator`): measured as a deterministic SIGSEGV (rc 139) with
    // ASan reporting a wild jump ("PC is at a non-executable region"), at BOTH
    // faces this function serves — the RETURN coercion and the ARGUMENT
    // coercion — while iterating the same dictionary in place is clean. The
    // tell in the object file is that the failing program emits no
    // `bz$...enum...$vtable` pair at all, because no wrap was ever minted.
    //
    // This is #3419's duplicated-arm shape once more, and specifically the
    // half of it where an OWNERSHIP axis was widened without the WRAP axis:
    // `returnSourceIsCalleeOwnedCollection` has named `Dictionary` /
    // `OwningDictionary` / `SortedDictionary` since #2299 (it must, or the
    // source would be double-freed), so `srcOwnedByCallee` arrives here TRUE
    // for exactly the shape this function had no arm for.
    //
    // NO NEW PREDICATE AND NO NEW WRAP HELPER. `isDictTypeName` is the
    // emitter's existing face of `common::isDictionaryName` — the same
    // authority the indexer, `.Count` and for-in lowering already read — and
    // the wrap authority already exists and is already exercised by the
    // explicit `.GetEnumerator()` face (`LlvmBackend_Calls2.cpp` ~6766):
    // `wrapDictionaryAsIEnumerable` snapshots the live keys+values, decodes
    // one `{K, V}` entry struct per pair (rebuilding ref-counted string keys)
    // and returns an OWNING array enumerator over that snapshot — routing the
    // string-key shape through `wrapDictionaryEntryArrayAsIEnumerable` so the
    // rebuilt keys are released. `wrapSortedDictionaryAsIEnumerable` is its
    // ordered sibling; a `SortedDictionary` must take it, because the plain
    // snapshot is in hash order and stdlib §4.17's ascending-key iteration
    // would be silently broken by it — the same contract reason `SortedSet` is
    // excluded from the array arm above.
    //
    // OWNERSHIP. The snapshot is INDEPENDENT of the source dictionary (a fresh
    // `bz_array_t`, rebuilt keys, copied value words), so the wrapper's slot-1
    // Dispose reclaims exactly the snapshot and never the source — which is
    // why a callee-OWNED source must be freed here and now, exactly as the
    // dict-backed `Set` arm above does (#2299's `freeDictSourceNow`).
    // Scope-end disposal is NOT an option: it would race the caller's
    // iteration of the returned wrapper. §28.3 suppresses the returned local's
    // own reclaim, so this free is exactly-once. `bz_dict_free(d, null)`
    // releases the table without touching the values, which is what keeps the
    // snapshot's borrowed value words live for the caller's iteration.
    if (isDictTypeName(actualTy)) {
        // fix #3519, NARROWED on review: only the entry shapes this path
        // carries end to end. A non-primitive K or V falls through to the
        // historical `return v`, where it keeps failing LOUDLY (the SIGSEGV it
        // produced before #3519) instead of being turned into a wrong answer.
        //
        // fix #3568 (§29.2 / §28.9 / §4.8, v0.286): the narrowing existed
        // because this arm had no way to discharge a non-primitive value's
        // disposal obligation. It has one now — `dictSnapshotValueDisposers`
        // splits that obligation into the half the source free discharges and
        // the half the snapshot's slot-1 Dispose does — so the gate is handed
        // the SOURCE type and admits every shape those two disposers cover.
        //
        // fix #4099 (stdlib §4.17, v0.306 follow-up): the widening-coercion
        // twin of #3976's indexer/for-in check. A `SortedDictionary<K, V>`
        // coerced to `IEnumerable<DictionaryEntry<K, V>>` here used to
        // silently admit a key type eligible ONLY via the auto-hashable
        // (§9.2) or class/interface (hasObjectHeaderEquality) branches —
        // hashing/equality, not ordering. Reject before the wrap so the
        // compile fails consistently with `.Add`/`.Contains` (fix #3947) and
        // the indexer/for-in (fix #3976). The plain `Dictionary<K, V>` arm
        // is unaffected — its `where K: IEquatable<K>` constraint (§4.8)
        // admits any key with structural equality, which is the right
        // contract for an unordered map. The poison-value return mirrors
        // the indexer's own — well-formed no-op. This check fires BEFORE
        // `dictEntryWrapShapeSupported` so a SortedDictionary<Pt, int> widened
        // to `IEnumerable<DictionaryEntry<Pt, int>>` is rejected even when
        // the wrap helper declines (the helper's own decline is the
        // historical fail-loud path, not a silent admit — but it was
        // unreachable in the rejection lane without this check landing
        // first).
        if (builtinTypeName(actualTy) == "SortedDictionary" &&
            reportSortedElementNeedsCompareTo(
                actualTy, actualTy->genericArgs[0], srcExpr->span.begin,
                "the `IEnumerable<DictionaryEntry<K, V>>` coercion")) {
            if (outWrapMade) *outWrapMade = false;
            return llvm::PoisonValue::get(ptrTy());
        }
        if (!dictEntryWrapShapeSupported(targetTy, actualTy)) return v;
        // The two halves are derived TOGETHER, at the one site that decides
        // both, so a shape can never take one without the other. Both stay null
        // for a BORROWED source: the source dictionary outlives this coercion
        // and keeps owning its values, so the snapshot must neither free a box
        // nor dispose a value.
        llvm::Function* immediateValDisp = nullptr;
        llvm::Function* deferredValDisp  = nullptr;
        if (srcOwnedByCallee)
            dictSnapshotValueDisposers(actualTy, &immediateValDisp,
                                       &deferredValDisp);
        llvm::Value* wrap =
            (builtinTypeName(actualTy) == "SortedDictionary")
                ? wrapSortedDictionaryAsIEnumerable(v, targetTy)
                : wrapDictionaryAsIEnumerable(v, targetTy, deferredValDisp);
        // A malformed entry TypeRef (no `DictionaryEntry<K,V>` args) makes the
        // helpers decline; fall through to the historical pass-through rather
        // than claim a frame that was never minted (#3419: a claim wider than
        // the mint double-frees).
        if (wrap) {
            if (outWrapMade) *outWrapMade = true;
            // fix #3568: the source free discharges ONLY the storage the
            // snapshot copied out of the table (the record / nullable-value
            // box). Everything the snapshot ALIASED is discharged by
            // `deferredValDisp` at the wrapper's slot-1 Dispose instead —
            // passing it here would free the objects the caller is about to
            // iterate.
            if (srcOwnedByCallee) freeDictSourceNow(v, immediateValDisp);
            return wrap;
        }
        return v;
    }
    // fix #2298 / §28.9: LinkedList<T> widened to `IEnumerable<T>` here uses
    // the plain borrow wrap (slot-1 Dispose = `free(self)`, the wrapper frame
    // only) for the BORROWED source — the source's binder still disposes its
    // backing at its own scope end. fix #3354 / §28.2 / §28.9 / §28.11: a
    // callee-OWNED source (a return-coerced value whose ownership transfers
    // out of this scope) now uses the OWNING sibling wrapOwnedLinkedListAsIEnumerable
    // — its slot-1 Dispose frees the backing bz_llist AND the wrapper frame
    // at the CALLER's scope end, AFTER iteration. The previous shape (plain
    // borrow wrap + `freeLinkedListSourceNow` immediately after) freed the
    // backing right here, so the caller's `for x in e do ...` walked
    // `bz_llist_first_node` / `bz_llist_node_next` on already-freed memory —
    // confirmed UAF under ASan/LSan (see the #3354 regression test). The
    // OWNING-wrap's slot-4 GetEnumerator stamps fresh NON-owning clones with
    // the PLAIN vtable, so a per-loop enumerator frees only its own struct
    // (#2987's protocol). The companion #2710 owned-array path is the
    // structural template for this fix.
    if (actualTy && isLinkedListTypeName(actualTy)) {
        if (outWrapMade) *outWrapMade = true;
        if (srcOwnedByCallee) {
            return wrapOwnedLinkedListAsIEnumerable(v, targetTy);
        }
        return wrapLinkedListAsIEnumerable(v, targetTy);
    }
    // A user class implementing IEnumerable<T> (declaration-carrying). A source
    // whose type is ALREADY a declaration-less IEnumerable (an alias / another
    // wrapped value) has no declaration and passes through unchanged.
    //
    // fix #3277 (§28.2 / §28.6 / §28.10, v0.262): when the source is a producer
    // expression (e.g. `BoxEnum3267.FromSeed(seed)` — a CallExpr yielding a
    // fresh `owned BoxEnum3267`), the wrapping is the SOURCE's only owner:
    // the call is bound to no other name, so the wrapper's slot-1 Dispose
    // must free the source. Mirror the local-decl / GetEnumerator sibling
    // fixes (`!srcIsNamedOwner` at Setup.cpp:12607 and Calls2.cpp:5076) —
    // both compute the same `srcIsNamedOwner` predicate (a named local /
    // parameter / field / auto-property keeps the source alive for itself,
    // the wrapper never owns it). Default `ownsSrc=false` at this site left
    // every temporary producer at #3277's leak (single-module twin ASan
    // could detect, cross-module imported could not — #2633's asymmetry).
    if (actualTy && actualTy->kind == sema::TypeRef::Kind::Named &&
        actualTy->declaration) {
        bool srcIsNamedOwner = false;
        if (sema::Symbol* sym = ctx_.exprSymbol(*srcExpr)) {
            if (dynamic_cast<sema::LocalSymbol*>(sym) != nullptr ||
                dynamic_cast<sema::ParameterSymbol*>(sym) != nullptr ||
                dynamic_cast<sema::FieldSymbol*>(sym) != nullptr) {
                srcIsNamedOwner = true;
            } else if (auto* ps = dynamic_cast<sema::PropertySymbol*>(sym)) {
                srcIsNamedOwner =
                    sema::isAutoPropertyStorageSymbol(ps, ctx_);
            }
        }
        return wrapUserClassAsIEnumerable(v, actualTy->declaration, targetTy,
                                          /*ownsSrc=*/!srcIsNamedOwner);
    }
    return v;
}

// fix #2779 / §29.2 / §29.11 / §23.3 / §28.3 K3 / §28.9 (v0.240): a static
// widening of the §23.3 / §29.11 property-read ownership classification for
// the INTERFACE-dispatched shape — `s.E` where `s: ISeq` (an interface
// declared with a body-less `readonly property E: IEnumerable<T>;`). At an
// interface-typed receiver, `ctx_.exprSymbol` resolves to the INTERFACE's
// PropertySymbol (whose `ast::PropertyDecl` records `bodyKind == Auto` —
// the interface declaration has no accessor body), so the existing
// `sema::isAutoPropertyStorageSymbol` classification is statically forced to
// `true` and the for-in / using-var / plain-`var` paths correctly refuse to
// claim the wrapper frame as theirs. The runtime behaviour is the OPPOSITE:
// the wrap-creator (`wrapAsIEnumerableForTarget` on the BLOCK-bodied getter's
// RETURN edge, #2703; or `wrapPropertyArrayAsIEnumerable` on the
// EXPRESSION-BODIED read site, #2815 / #2777) fires UNCONDITIONALLY for every
// iterator-format-typed property read, for every implementer (Array/Stack/
// Queue/Set/LinkedList/user-class-IEnumerable<T>), and the wrapper frame is
// a fresh heap object the reader must reclaim. The interface dispatch hides
// the body-vs-storage distinction a concrete receiver would expose, so the
// static widening here (always classify as fresh-mint) is sound for the
// iterator-format declared type — and for that family alone.
//
// The opposite-direction pin #2174 keeps: a non-iterator-format interface
// property read (e.g. `property Name: string;` over `s: IShape`) stays a
// borrow, because the wrap-creator never fires for non-iterator-format
// declared types. The widening is therefore ITERATOR-FORMAT-SPECIFIC, not a
// general flip of all interface-dispatched reads — it preserves the #2174
// heap-use-after-free defence for the family that defence was designed for.
//
// Per the issue's "needs a runtime discriminator" framing, this is the
// narrower implementation that takes the "wrap unconditional at the
// interface face" candidate the issue enumerated: the wrap-creator is
// already unconditional (every implementer mints), so the call site can
// claim ownership statically. The double-free direction the issue warns
// about (an implementer whose `E` is an auto-property handing back stored
// state) does not arise: the interface declaration `readonly property E:
// IEnumerable<T>;` has no storage of its own, so no implementer can hand
// back a "stored" IEnumerable through the interface slot — every read
// dispatches through the implementer's accessor, and every implementer's
// accessor that returns a non-IEnumerable body type goes through the
// wrap-creator. (The interface declaration could DECLARE the property with
// an expression body / block body, in which case the wrap would happen at
// the interface's accessor, but the test in this lane uses the canonical
// body-less form that the issue's repro shape relies on.)
bool Emitter::isInterfaceDispatchedIteratorRead(
        const ast::Expr* iterable) const {
    if (!iterable) return false;
    auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(iterable);
    if (!ma) return false;
    auto* ps = dynamic_cast<sema::PropertySymbol*>(ctx_.exprSymbol(*ma));
    if (!ps || ps->isIndexer) return false;
    if (!ps->parent || ps->parent->kind != ast::TypeDeclKind::Interface)
        return false;
    // The property's declared return type is the MemberAccessExpr's sema type
    // (mirrors `wrapPropertyArrayAsIEnumerable`'s `ctx_.exprType(*ma)`
    // argument). For an interface-dispatched read the resolved symbol is the
    // interface's `PropertySymbol` (whose `ast::PropertyDecl` records
    // `bodyKind == Auto` because the interface declaration has no accessor
    // body), and the property's declared type is exactly the interface's
    // declared type for the slot — the four iterator-format heads
    // (#943/#1525/#1268/#2356) the wrap-creator keys on. A null check on
    // the exprType defends against untyped arguments in error-recovery
    // paths.
    const sema::TypeRef* propTy = ctx_.exprType(*ma);
    if (!propTy || propTy->kind != sema::TypeRef::Kind::Named ||
        propTy->declaration)
        return false;
    if (!propTy->astNode || !propTy->astNode->name ||
        propTy->astNode->name->parts.empty())
        return false;
    std::string_view n = propTy->headName();
    // fix #3419 (§29.2, v0.283): read the shared head classifier.
    return common::isIterFormatEnumerableHead(n);
}

// fix #2815 / §23.2 / §29.2 (v0.240) and #2777 (§29.2 / §23.2, v0.240):
// see the declaration in LlvmBackend_impl.hpp.
//
// #2815 — the shape we care about is `public property P: IEnumerable<int>
// get => _items;` (or its block-bodied / bare-identifier spellings) where
// the getter body's sema type is an Array-backed collection. The
// `actualIsArray` / `initIsArray` predicates in `wrapAsIEnumerableForTarget`
// and `lowerLocalDecl` look at the EXPRESSION's sema type, which for a
// property read IS the property's declared `IEnumerable<T>` (the very type
// the wrap helper is trying to reach), so the wrap is missed. Reach the
// getter body directly and wrap when its declared type is one of the
// array-backed collections.
//
// #2777 — the EXPRESSION-BODIED twin of #2703's BLOCK-BODIED §29.2 wrap.
// #2703 wrapped the BLOCK form on the RETURN edge via
// `wrapAsIEnumerableForTarget`, which handles Array/Set/LinkedList/user-
// class sources uniformly. #2815 extended the EXPRESSION-BODIED form to
// the Array family only — a Set/LinkedList/user-class-IEnumerable getter
// body (`=> _s;` where `_s: OwningSet<Item>`) fell through unwrapped and
// the caller's for-in drove vtable dispatch on the raw Set/LinkedList
// descriptor → SIGSEGV at slot 4 (GetEnumerator, NULL). Dispatch to
// `wrapAsIEnumerableForTarget` for the non-Array case (IEnumerable target
// only — IOwningEnumerable has no wrap variant for Set/LinkedList today
// and stays a no-op here; see the deferred follow-up note).
//
// BORROWING wrap (wrapArrayAsIEnumerable). The source has an existing owner:
//   * `owned _items: Array<int>` field → owning object disposes it (#1840's
//     same-source path, the issue's repro)
//   * `_items: Array<int>` (borrowing field) → same, just on a different
//     lifetime tier
//   * parameter / local → the parameter / local's scope-end disposes it
// A fresh `new Array<int>(...)` getter body is OUT-OF-SCOPE here — its
// symptom is a leak (no owner), not the crash #2815 pins; classifying it
// requires AST shape (a `NewExpr` at the property-read site, not at the
// field) and is left as a follow-up. Returns `v` unchanged when the declared
// type is not a pure `IEnumerable<T>` / `IOwningEnumerable<T>` or the
// getter's type is not an array-backed collection.
//
// ⚠ fix #3405 (v0.283) CORRECTS the two sentences above. (a) The named
// "BORROWING wrap (wrapArrayAsIEnumerable)" is no longer called from here —
// the array arm was deleted and every family dispatches through
// `wrapAsIEnumerableForTarget`; see the in-body comment for why. (b) The
// fresh-`new` getter is not a deferred leak but a non-case: sema rejects it
// ("a property or indexer getter may not return a freshly-produced owned
// value", §28.6 — measured on 1e3861a).
llvm::Value* Emitter::wrapPropertyArrayAsIEnumerable(
        llvm::Value* v, const sema::TypeRef* declTy,
        const ast::Expr* getter, const ast::Expr* readExpr) {
    // fix #3419 (§29.2 / §23.2 / §23.4, v0.283): every `return` below reports
    // this read's verdict through `report`, so the ownership-claim site
    // (`registerIterableSourceDisposal`) reads the mint rather than re-deriving
    // it from a name list. Insert on wrap, ERASE on decline — see the member's
    // comment in LlvmBackend_impl.hpp for why the erase is load-bearing.
    auto report = [&](llvm::Value* result) -> llvm::Value* {
        if (readExpr) {
            if (result && result != v)
                propReadIterWrappedExprs_.insert(readExpr);
            else
                propReadIterWrappedExprs_.erase(readExpr);
        }
        return result;
    };
    if (!v || !declTy || !getter) return report(v);
    if (declTy->kind != sema::TypeRef::Kind::Named || declTy->declaration)
        return report(v);
    if (!declTy->astNode || !declTy->astNode->name ||
        declTy->astNode->name->parts.empty())
        return report(v);
    std::string_view dn = declTy->headName();
    // fix #3419 (§29.2, v0.283): read the shared head classifier.
    if (!common::isSyncIterFormatEnumerableHead(dn)) return report(v);
    sema::TypeRef* getterTy = ctx_.exprType(*getter);
    if (!getterTy || getterTy->kind != sema::TypeRef::Kind::Named)
        return report(v);
    if (!getterTy->astNode || !getterTy->astNode->name ||
        getterTy->astNode->name->parts.empty())
        return report(v);
    // fix #3405 (§29.2 / §23.2; stdlib §4.11, v0.283): the array-family arm
    // that used to sit here — a second spelling of `wrapAsIEnumerableForTarget`'s
    // `actualIsArray` grid, routing straight to `wrapArrayAsIEnumerable` — is
    // DELETED rather than aligned. It was a strict SUBSET of the dispatch below
    // (the borrowed, non-nested, forward-order cell) and, being a copy, it did
    // not carry the three decisions the authority makes:
    //
    //   * the §4.11 stack divert (#2486/#2471) — measured on 1e3861a, a
    //     `public property Items: IEnumerable<Item> => _items;` over an
    //     `owned _items: OwningStack<Item>` of 1,2,3 yielded 1,2,3, while the
    //     argument coercion and the direct `for` over the same stack yielded
    //     3,2,1. stdlib §4.11 "Iteration order" (v0.283) requires the latter:
    //     "All four types iterate (via `IReadOnlyCollection<T>`) in take order
    //     — a stack top-to-bottom (pop order), a queue front-to-back (dequeue
    //     order); the owning forms iterate identically, yielding borrows."
    //     At the `IOwningEnumerable<Item>` head the same read drained the
    //     source front-to-back, which is #3405 as filed.
    //   * the §12.9 nested covariant view (#1850/#2487) — the same property at
    //     `IEnumerable<IEnumerable<int>>` over `Array<Array<int>>` SIGSEGV'd on
    //     1e3861a, because the plain wrap hands the inner `bz_array_t*` to a
    //     consumer that drives vtable dispatch on it.
    //   * the #2299 callee-owned arm. Unreachable HERE and measured so: a
    //     getter body that is a `new` is rejected by sema ("a property or
    //     indexer getter may not return a freshly-produced owned value", §28.6),
    //     the `move` and call-result shapes classify Borrowed, and an
    //     expression body has no locals — so `returnSourceIsCalleeOwnedCollection`
    //     is statically false at this face and the delete changes nothing there.
    //
    // This is the #3419 shape: the wrap-creator decides, and this site reads
    // its verdict instead of re-deriving it.
    //
    // fix #2777 (§29.2 / §23.2, v0.240): the EXPRESSION-BODIED sibling of
    // #2703's BLOCK-BODIED §29.2 wrap. The block form wraps on the RETURN
    // edge through `wrapAsIEnumerableForTarget` (handles Set, LinkedList,
    // user-class IEnumerable in addition to Array/Stack/Queue); the
    // expression-bodied form has no RETURN and must wrap at the read site,
    // so dispatch to the same helper the block form uses. Mirrors the
    // block form's RETURN-edge call verbatim: `ownLazyPipeline=true`
    // (caller owns the returned enumerator — same as a RETURN position)
    // and `srcOwnedByCallee = returnSourceIsCalleeOwnedCollection(getter)`
    // (only `new T(...)`, `move e`, or a producer-init LOCAL transfers
    // ownership into the wrapper — a plain field/parameter/local borrow
    // keeps the wrap BORROWING, exactly as the block form does).
    //
    // fix #3355 / §29.2 / §23.2 (v0.278) — interim SIGSEGV-elimination:
    // the same dispatch for the `IOwningEnumerable<T>` head. See the
    // gate comment at line 7561-7594 for the scope and limitations; the
    // wrap helpers' slot-3 Current-vs-TakeCurrent gap is the
    // class-element follow-up #3364.
    // fix #3419 (§29.2, v0.283): the head re-test that stood here was a THIRD
    // spelling of the gate in this same function — and a vacuous one: the early
    // return above already declined every other head and `dn` is not reassigned
    // between the two, so the condition was invariantly true and the trailing
    // `return v;` unreachable. A dead copy of a classification still drifts on
    // the next family member, and a reader cannot tell a dead one from a live
    // one; delete rather than align (#3419 point 2).
    return report(wrapAsIEnumerableForTarget(
        v, declTy, getter,
        /*ownLazyPipeline=*/true,
        /*outWrapMade=*/nullptr,
        /*srcOwnedByCallee=*/
        returnSourceIsCalleeOwnedCollection(getter)));
}

// fix #2299 / §28.9: emit an immediate bz_array_free(v, ?) for the
// source Array-family collection, right at the RETURN coercion (NOT at
// scope end — the return is about to hand the wrapping enumerator to the
// caller, and any scope-end disposal would fire before the caller
// finishes iterating the wrap and free the source out from under it).
// Caller MUST only invoke when srcOwnedByCallee was true (a parameter/
// field/already-IEnumerable source must stay borrowed — its owner keeps
// the disposal obligation). The wrapper's slot-1 Dispose runs on the
// caller's side and frees only the wrapper frame (the backing was
// already reclaimed here).
// fix #2710 (§28.2 / §28.9 / §28.11, v0.240): the element disposer is
// derived from the SOURCE type's element, not hardcoded to null. The
// borrowing-family `string` element is the RuntimeManaged row — its
// inserter (the literal arm's `litIsStrArr` and the four `.Add` arms in
// `LlvmBackend_Calls.cpp`) already retains the array's +1, so the
// RETURN-coercion free MUST release it too. A null disposer was leaking
// one +1 per string element (bounded leak, not unsoundness). The
// `!nullable` guard mirrors Site 3 — a nullable string element is never
// retained at construction, so releasing here would be a stray
// double-free direction. The predicate is the verbatim mirror of
// `arrIsStringElem` at LlvmBackend_Calls.cpp (the predicate the existing
// `.Remove*`/`.Clear` releases use), so the release is provably paired.
void Emitter::freeCollectionSourceNow(llvm::Value* v,
                                      const sema::TypeRef* srcTy) {
    if (!v) return;
    llvm::Function* elemDisp = nullptr;
    if (srcTy && !srcTy->genericArgs.empty()) {
        const sema::TypeRef* elTy = srcTy->genericArgs[0];
        ElementPolicy elPol = elementPolicyFor(elTy, /*owning=*/false);
        elemDisp = elPol.disposerThunk;
        bool arrIsStringElem =
            elPol.cat == ElementPolicy::Category::RuntimeManaged &&
            elTy && elTy->kind == sema::TypeRef::Kind::Primitive &&
            elTy->primitive == sema::PrimitiveType::String && !elTy->nullable;
        if (!elemDisp && arrIsStringElem)
            elemDisp = stringElementReleaseThunk();
    }
    llvm::Value* disp = elemDisp
        ? llvm::cast<llvm::Value>(elemDisp)
        : llvm::ConstantPointerNull::get(ptrTy());
    auto* fn = declareC("bz_array_free", llvm::Type::getVoidTy(llvm_),
                        {ptrTy(), ptrTy()});
    builder_.CreateCall(fn, {v, disp});
}

// fix #2299 / §28.9: emit an immediate bz_dict_free(v, null) for the
// source dict-backed Set after the snapshot.
//
// fix #2710 note (§28.2 / §28.9 / §28.11, v0.240): this helper is ONLY
// called for a dict-backed Set (the local-decl-init Set arm at
// LlvmBackend_Setup.cpp and the return-coercion Set arm in
// wrapAsIEnumerableForTarget). Set's string keys are released by
// `bz_set_string_keys_release` (a separate pre-free pass registered in
// registerUsingDisposal — the dictStringKeys flag, Disposal.cpp:4819),
// NOT by the value disposer argument — `freeDictSourceNow` therefore
// passes a null disposer (the Set has no values to release through the
// disposer slot). For Set<string> this means a borrow-parameter
// return-coercion has a separate, pre-existing leak: the source Set is
// freed here WITHOUT calling `bz_set_string_keys_release` (the
// scope-end path does, the return-coercion path does not). Filed as a
// follow-up — out of #2710's stated scope (Array AND LinkedList); the
// set-key release lives at the scope-end-disposal layer, not the
// RETURN-coercion one.
//
// fix #3568 (§29.2 / §28.9 / stdlib §4.8, v0.286): the one-argument spelling is
// now the KEYS-ONLY face and nothing else. It is reached from exactly the two
// dict-backed `Set` arms (LlvmBackend_Setup.cpp's local-decl mint and — before
// this commit — the RETURN coercion in this file): a Set's hash table has keys
// and no values, so there is no value slot a disposer could reach. A REAL
// dictionary's values are neither disposed nor released by this call, which is
// the whole of issue 3568; every dictionary caller uses the two-argument
// overload and states which obligation the free discharges.
void Emitter::freeDictSourceNow(llvm::Value* v) {
    freeDictSourceNow(v, /*valDisposer=*/nullptr);
}

void Emitter::freeDictSourceNow(llvm::Value* v, llvm::Function* valDisposer) {
    if (!v) return;
    auto* fn = declareC("bz_dict_free", llvm::Type::getVoidTy(llvm_),
                        {ptrTy(), ptrTy()});
    llvm::Value* disp = valDisposer
        ? llvm::cast<llvm::Value>(valDisposer)
        : llvm::cast<llvm::Value>(llvm::ConstantPointerNull::get(ptrTy()));
    builder_.CreateCall(fn, {v, disp});
}

// fix #3568 (§29.2 / §28.9 / §28.11 / stdlib §4.8, v0.286): see the declaration
// in LlvmBackend_impl.hpp for the rule. The three arms below are the SAME
// cascade, in the same order, that the scope-end dictionary disposal already
// runs at LlvmBackend_Setup.cpp (:15913 and :17266) and the indexer-overwrite
// path runs at :1297 of this file — `elementPolicyFor(V, owning)`, then
// `dictValueBoxFreer`, then the #429 borrowing-string release. NO NEW
// CLASSIFIER: this helper adds only the routing of each arm to the moment that
// is safe for it, so `*immediateOut ∪ *deferredOut` is exactly the single
// disposer a scope-end free would have selected for the same dictionary.
void Emitter::dictSnapshotValueDisposers(const sema::TypeRef* dictTy,
                                         llvm::Function** immediateOut,
                                         llvm::Function** deferredOut) {
    if (immediateOut) *immediateOut = nullptr;
    if (deferredOut)  *deferredOut  = nullptr;
    if (!dictTy || !dictTy->astNode || !dictTy->astNode->name ||
        dictTy->astNode->name->parts.empty())
        return;
    // The raw `headName()`, matching `dictValueBoxFreer`'s own spelling and
    // `dictEntryWrapShapeSupported`'s — see the ⚠ note on that gate for why the
    // three must read the head the SAME way rather than the cleverest way.
    std::string_view head = dictTy->headName();
    // `SortedDictionary` is deliberately absent: its snapshot is minted by
    // `wrapSortedDictionaryAsIEnumerable` (LlvmBackend_Calls2.cpp), which has
    // no snapshot-disposer parameter, so `dictEntryWrapShapeSupported` keeps it
    // on the narrow primitive-only shape and it never reaches here.
    if (head != "Dictionary" && head != "OwningDictionary") return;
    const sema::TypeRef* valTy =
        (dictTy->genericArgs.size() > 1) ? dictTy->genericArgs[1] : nullptr;
    if (!valTy) return;
    bool owning = (head == "OwningDictionary");

    // ARM 1 — DEFERRED. An OwningDictionary<K, class/interface> (and the
    // Shared/Weak/Task/nested-collection handles elementPolicyFor also owns)
    // stores the object POINTER in the slot, and the snapshot decode aliases
    // that same pointer (`valDecoded = slotV`). Disposing at the source free
    // would leave the caller iterating disposed objects — #3568's stated
    // use-after-free trap. The obligation moves to the snapshot's slot-1
    // Dispose, which runs at the caller's scope end, after iteration.
    //
    // ⚠ This arm is NOT reachable from today's `dictEntryWrapShapeSupported`,
    // which declines a class/interface V on issue 3563's read-side defect (see
    // that gate). It is here — first, and in the same order as the scope-end
    // cascade — deliberately: the ORDER is the load-bearing part. An
    // OwningDictionary's class value must be answered by `elementPolicyFor`
    // and never fall through to ARM 3, and the union of the two outputs must
    // stay equal to the single disposer a scope-end free selects. Measured on
    // this build with the gate temporarily widened: `OwningDictionary<int,
    // Item>` iterated 200x is LSan clean and disposes each Item exactly once.
    if (llvm::Function* owned = elementPolicyFor(valTy, owning).disposerThunk) {
        if (deferredOut) *deferredOut = owned;
        return;
    }
    // ARM 2 — IMMEDIATE. #401 / #539: a borrowing Dictionary<K, record> and a
    // Dictionary<K, V?> over a value type heap-BOX each value; the snapshot
    // decode `CreateLoad`s the struct OUT of that box, so the snapshot owns its
    // own bytes and the box is dead as soon as the snapshot exists. Freeing it
    // at the source free is both safe and the only place it can happen — the
    // snapshot never holds the box pointer to free later.
    if (llvm::Function* box = dictValueBoxFreer(dictTy)) {
        if (immediateOut) *immediateOut = box;
        return;
    }
    // ARM 3 — DEFERRED. #429 / §28.9: a borrowing Dictionary<K, string>
    // co-owns a +1 per stored value, and the decode aliases the same
    // `bz_string*`. Releasing at the source free can drop the last reference
    // while the snapshot still points at it; letting the snapshot INHERIT the
    // table's +1 and release it at slot-1 Dispose keeps the accounting exact
    // and the bytes alive for the whole iteration. The `!nullable` guard
    // mirrors the scope-end and freeLinkedListSourceNow spellings — a nullable
    // string value is never retained at construction, so there is no +1 to
    // discharge.
    if (!owning && !valTy->nullable &&
        valTy->kind == sema::TypeRef::Kind::Primitive &&
        valTy->primitive == sema::PrimitiveType::String &&
        elementPolicyFor(valTy, /*owning=*/false).cat ==
            ElementPolicy::Category::RuntimeManaged) {
        if (deferredOut) *deferredOut = stringElementReleaseThunk();
    }
}

// fix #2299 / §28.9: emit an immediate bz_llist_free(v, ?) for the
// source LinkedList after the wrap.
// fix #2710 (§28.2 / §28.9 / §28.11, v0.240): the element disposer is
// derived from the SOURCE type's element, not hardcoded to null. The
// borrowing-family `string` element is the RuntimeManaged row — its
// inserter (the four `.AddFirst`/`.AddLast`/`.AddBefore`/`.AddAfter`
// arms in LlvmBackend_Calls3.cpp, #2686) already retains the list's +1,
// so the RETURN-coercion free MUST release it too. A null disposer was
// leaking one +1 per string element (bounded leak, not unsoundness).
// The `!nullable` guard mirrors Site 3 — a nullable string element is
// never retained at construction. The predicate is the verbatim mirror
// of `llIsStrElem` in LlvmBackend_Calls3.cpp, so the release is
// provably paired.
void Emitter::freeLinkedListSourceNow(llvm::Value* v,
                                      const sema::TypeRef* srcTy) {
    if (!v) return;
    llvm::Function* elemDisp = nullptr;
    if (srcTy && !srcTy->genericArgs.empty()) {
        const sema::TypeRef* elTy = srcTy->genericArgs[0];
        ElementPolicy elPol = elementPolicyFor(elTy, /*owning=*/false);
        elemDisp = elPol.disposerThunk;
        bool llIsStringElem =
            elPol.cat == ElementPolicy::Category::RuntimeManaged &&
            elTy && elTy->kind == sema::TypeRef::Kind::Primitive &&
            elTy->primitive == sema::PrimitiveType::String && !elTy->nullable;
        if (!elemDisp && llIsStringElem)
            elemDisp = stringElementReleaseThunk();
    }
    llvm::Value* disp = elemDisp
        ? llvm::cast<llvm::Value>(elemDisp)
        : llvm::ConstantPointerNull::get(ptrTy());
    auto* fn = declareC("bz_llist_free", llvm::Type::getVoidTy(llvm_),
                        {ptrTy(), ptrTy()});
    builder_.CreateCall(fn, {v, disp});
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
    // fix #3385 (§29.1 / §29.2 / §29.11; stdlib §4.5 / §4.13, v0.282): at an
    // `IOwningEnumerable<T>` target the wrap must hand out the DRAINING
    // enumerator, not this borrowing one.
    //
    // §29.1's "`Current` semantics on borrowing and owning enumerators" makes
    // slot 3 a different operation for the two heads: `IEnumerator<T>.Current`
    // is non-destructive and lends, while `IOwningEnumerator<T>.TakeCurrent()`
    // "transfers ownership of the most-recently-yielded value to the caller,
    // leaving the enumerator's internal slot empty" — and §29.2 desugars a
    // source of static type `IOwningEnumerable<T>` through `TakeCurrent()`, so
    // the loop variable OWNS each element and discharges it. Installing the
    // borrowing `Current` under that head (#3355's interim SIGSEGV fix, which
    // widened this gate but reused the borrowing helpers) left the SOURCE
    // collection still holding every element it had already handed out: on a
    // field-owned source — §28.6's stored-view face, `public property Items:
    // IOwningEnumerable<Item> => _items;` over `owned _items: OwningArray<Item>`
    // — the loop discharged each element and the field's own `bz_array_free`
    // element-disposer pass then disposed the very same elements again
    // (heap-use-after-free, #3385). §28.6's own worked example states the
    // permission this relies on: a receiver-sourced borrow of an owning
    // collection field is one "the caller may read/iterate/mutate/DRAIN".
    //
    // The transfer is therefore: element ownership leaves the collection at
    // each yield (stdlib §4.5 "elements leave the collection as they are
    // yielded"; §4.13's drain-view contract) and lands on the loop variable,
    // which §28.3/§29.2 already require to be discharged. The CONTAINER is
    // untouched — this wrap borrows the array exactly as the borrowing one
    // does, and the field's holder still frees the (now empty) array.
    //
    // No second classification predicate is minted: `wrapArrayAsIOwningEnumerator`
    // (#2218) is the existing draining authority — MoveNext pops through the
    // same `bz_array_drain_next` the `for var x in coll.Drain()` loop uses,
    // TakeCurrent moves the pending element out and nulls the slot per §29.1,
    // and Dispose disposes an advanced-but-untaken pending. Only its slot 4 is
    // extended (above) to mint a fresh enumerator, which the §29.2 desugaring
    // needs and stdlib §4.13's view contract independently requires.
    //
    // Reachability: `IOwningEnumerable<T>` constrains `where T: class` (stdlib
    // §4.13), so the element is always a class pointer — the pointer-sized
    // element `bz_array_drain_next` requires — and only the owning collection
    // families implement the interface, so a BORROWING `Array<T>` source can
    // never arrive here (sema rejects the conversion, §5.1). The pointer guard
    // keeps the arm off the non-class element types this helper also serves.
    //
    // fix #3405 (stdlib §4.11, v0.283): the test itself moved into
    // `owningDrainWrapForHead` so the reverse-order sibling
    // `wrapStackAsIEnumerator` asks the SAME question rather than re-spelling
    // it (or, as it did, not asking it at all). Front-to-back drain here; the
    // stack leaf passes `ArrayLifo` and (fix #3461) the LinkedList leaf `List`.
    if (llvm::Value* drain =
            owningDrainWrapForHead(arr, iEnumTy, DrainBacking::ArrayForward))
        return drain;
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

// fix #3405 (§29.1 / §29.2; stdlib §4.11 / §4.13, v0.283): the ONE owning-head
// test both bz_array_t*-backed leaf wraps consult. See the declaration in
// LlvmBackend_impl.hpp for why it is a function rather than an inline `if`.
//
// #3385 established the rule — at an `IOwningEnumerable<T>` target the wrap
// must be the DRAINING enumerator (§29.2 desugars an owning-typed source
// through `TakeCurrent()`, so element ownership leaves the collection at each
// yield; a borrowing `Current` under that head leaves the source still holding
// elements the loop has already discharged, which is a double dispose on a
// field-owned source). It wrote the rule into `wrapArrayAsIEnumerable` alone,
// so the reverse-order sibling `wrapStackAsIEnumerator` — the leaf every
// coercion position routes a `Stack`/`OwningStack` source through — never
// asked the question and kept the borrowing wrap at the owning head.
//
// `backing` is the ONLY thing the leaves disagree about: which drain primitive
// walks the source. The array pair's distinction is the one stdlib §4.11
// "Iteration order" already draws: "All four types iterate (via
// `IReadOnlyCollection<T>`) in take order — a stack top-to-bottom (pop order),
// a queue front-to-back (dequeue order); the owning forms iterate identically,
// yielding borrows." `bz_array_drain_next_lifo` is the existing primitive for
// the stack side (the `for var x in stk.Drain()` loop already uses it,
// LlvmBackend_Stmts.cpp).
//
// fix #3461 (§29.1 / §29.2; stdlib §4.7 / §4.13, v0.283): `List` is the third
// leaf — `LinkedList<T>` / `OwningLinkedList<T>` are node-backed
// (`bz_llist_t*`), not `bz_array_t*`-backed, so `wrapLinkedListAsIEnumerable`
// is a leaf of this same grid that #3385 and #3405 never reached. It kept the
// borrowing `Current` at an `IOwningEnumerable<T>` target, which on a
// field-owned source is #3385's double dispose byte for byte (measured:
// `rem=3` after what should be an owning drain, then heap-use-after-free in
// `bz$elemDispose` <- `bz_llist_free` <- `C$DisposeCore`). The head test is
// NOT re-spelled for it; the leaf asks THIS function, exactly as the two array
// leaves do, and `bz_llist_drain_next` is the existing head-to-tail primitive
// `for var x in list.Drain()` already selects (LlvmBackend_Stmts.cpp).
//
// Returns null — not `arr` — when the head is not the owning one, so a caller
// can distinguish "not my case" from "already an enumerator" and fall through
// to its own borrowing machinery.
//
// Reachability guard (#3385's, unchanged): `IOwningEnumerable<T>` constrains
// `where T: class` (stdlib §4.13), so the element is always a class pointer —
// the pointer-sized element `bz_array_drain_next*` / `bz_llist_drain_next`
// requires. The pointer test keeps the arm off the non-class element types
// these helpers also serve.
llvm::Value* Emitter::owningDrainWrapForHead(llvm::Value* src,
                                             const sema::TypeRef* iEnumTy,
                                             DrainBacking backing) {
    if (!src) return nullptr;
    if (builtinTypeName(iEnumTy) != "IOwningEnumerable") return nullptr;
    llvm::Type* elemTy = i32();
    if (iEnumTy && !iEnumTy->genericArgs.empty() && iEnumTy->genericArgs[0])
        elemTy = lowerTypeRef(iEnumTy->genericArgs[0]);
    if (!elemTy->isPointerTy()) return nullptr;
    if (backing == DrainBacking::List)
        return wrapLinkedListAsIOwningEnumerator(src, iEnumTy);
    return wrapArrayAsIOwningEnumerator(
        src, iEnumTy, /*lifo=*/backing == DrainBacking::ArrayLifo);
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
// fix #3450 (§28.11; stdlib §4.11, v0.283): the element disposer an
// OWNING enumerator wrap installs in its slot-1 `bz_array_free`, and the cache
// key that keeps two element types from sharing one Dispose.
//
// THE PREDICATE. #2710 wrote it into `wrapOwnedArrayAsIEnumerable`: the
// disposer comes from the ENUMERATOR's element type (the `T` in
// `IEnumerable<T>`), not from the source collection — the wrap hands out
// borrows, so a class element is NOT disposed here (the ClassBorrow row's
// thunk is null) while a RuntimeManaged non-nullable `string` element IS
// released, because its insertion retained the array's +1. The `!nullable`
// guard is #2710's: a nullable string element is never retained at
// construction, so releasing it would be a double-free direction. The stack
// twin `wrapOwnedStackAsIEnumerator` hardcoded null instead — #3419's pattern,
// and the arm this fix makes a second caller of.
//
// THE CACHE KEY. Both wraps name their per-element-type functions from the
// LOWERED llvm type ("ptr", "i32", …), and `string`, a class, and `IGrouping`
// all lower to `ptr`. Without a key segment for the disposer, the FIRST
// element type minted at this face wins the shared `…$Dispose` and every other
// one inherits it: a module holding both a `Stack<string>` and a
// `Stack<SomeClass>` at this face would run the string RELEASE over class
// pointers, or skip the release entirely. That is the over-release direction,
// so the disposer's identity is part of the key. The segment is EMPTY when the
// disposer is null, so the overwhelmingly common shape keeps its existing
// symbol names.
llvm::Function* Emitter::ownedWrapElementDisposer(const sema::TypeRef* iEnumTy) {
    const sema::TypeRef* elTy = (iEnumTy && !iEnumTy->genericArgs.empty())
                                    ? iEnumTy->genericArgs[0] : nullptr;
    ElementPolicy pol = elementPolicyFor(elTy, /*owning=*/false);
    if (pol.disposerThunk) return pol.disposerThunk;
    bool isStringElem =
        pol.cat == ElementPolicy::Category::RuntimeManaged &&
        elTy && elTy->kind == sema::TypeRef::Kind::Primitive &&
        elTy->primitive == sema::PrimitiveType::String && !elTy->nullable;
    return isStringElem ? stringElementReleaseThunk() : nullptr;
}

std::string Emitter::ownedWrapDisposerKey(llvm::Function* elDisp) {
    return elDisp ? ("$" + elDisp->getName().str()) : std::string();
}

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
    // fix #3570 (§29.2 / §28.9 / §4.8, v0.286): the pre-fix scheme collapsed
    // every non-integer, non-pointer element type (notably structs) to the
    // single letter `"f"`, so two different struct element types in one module
    // — e.g. `DictionaryEntry<int,int>` (8 B) and `DictionaryEntry<string,int>`
    // (16 B) — shared one `bz$arrenum$f$Current` whose baked-in stride was the
    // first creator's. Suffix the alloc size so each distinct stride gets its
    // own machinery; integer/pointer kinds still keep their discriminating
    // bit-width / `"ptr"` segment.
    //
    // fix #3570 / §6b unification (v0.286): this site now feeds through the
    // shared `enumMachineryElementName` helper. The pre-unification spelling
    // lived inline here AND at `ensureArrayEnumMachinery` AND at
    // `wrapDictionaryEntryArrayAsIEnumerable`, AND (in #3661) at the stack
    // twin — five sites, one predicate. Keeping the helper as the single
    // spelling is what keeps a future sibling site from regressing in this
    // exact shape.
    std::string elemName = enumMachineryElementName(elemTy);
    std::string plainPrefix = "bz$arrenum$" + elemName + "$";
    // fix #3450 (v0.283): the disposer is part of the cache key — see
    // `ownedWrapElementDisposer` for why sharing one Dispose across two
    // element types that both lower to `ptr` is an over-release hazard.
    llvm::Function* elDisp = ownedWrapElementDisposer(iEnumTy);
    std::string prefix =
        "bz$arrenumown$" + elemName + ownedWrapDisposerKey(elDisp) + "$";

    // Owning Dispose: bz_array_free(self->arr, <element disposer>); free(self).
    // fix #2710 (§28.2 / §28.9 / §28.11, v0.240): the element disposer is
    // derived from the ENUMERATOR's element type (the T in IEnumerable<T>),
    // not hardcoded to null. The borrowing-family `string` element is the
    // RuntimeManaged row — its inserter (the literal arm's `litIsStrArr`
    // and the four `.Add` arms in `LlvmBackend_Calls.cpp`) already retains
    // the array's +1, so the OWNING-wrap's slot-1 Dispose (the caller's
    // scope-end free) MUST release it too. A null disposer was leaking
    // one +1 per string element (bounded leak, not unsoundness). The
    // `!nullable` guard mirrors Site 3 — a nullable string element is never
    // retained at construction, so releasing here would be a stray
    // double-free direction. The predicate is the verbatim mirror of
    // `arrIsStringElem` at LlvmBackend_Calls.cpp (the predicate the existing
    // `.Remove*`/`.Clear` releases use), so the release is provably paired.
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
        // Element-type-derived disposer, hoisted to the cache key above
        // (fix #3450): the same shape #2710 Site 2's
        // `freeCollectionSourceNow` carries — RuntimeManaged +
        // non-nullable string primitive → stringElementReleaseThunk.
        llvm::Value* elDispVal = elDisp
            ? llvm::cast<llvm::Value>(elDisp)
            : llvm::ConstantPointerNull::get(ptrTy());
        auto* freeArrFn = module_->getOrInsertFunction(
            "bz_array_free", llvm::FunctionType::get(
                llvm::Type::getVoidTy(llvm_), {ptrTy(), ptrTy()}, false))
            .getCallee();
        b.CreateCall(
            llvm::FunctionType::get(
                llvm::Type::getVoidTy(llvm_), {ptrTy(), ptrTy()}, false),
            freeArrFn, {arrPtr, elDispVal});
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
    // fix #3477: `getNamedGlobal`, NOT `getGlobalVariable(vtName)`. The
    // one-argument overload is `getGlobalVariable(Name, /*AllowInternal=*/false)`
    // (llvm/IR/Module.h) and returns null for a global with LOCAL linkage --
    // which every vtable below is. So this guard never fired: each wrap site
    // minted a fresh vtable and LLVM uniqued the name (`...$vtable.1`),
    // contradicting the comment above that the vtable is "created once per
    // element type". `getNamedGlobal` is `getGlobalVariable(Name, true)` and is
    // already this file's spelling for the same question (see the imported-
    // indexer thunk's `vt$<Type>` lookup, which notes that "the module itself
    // is the memo — no twelfth side table"). Sharing is sound because the
    // vtable's contents are ONLY the self-pointer and functions that the
    // `getFunction(name)` caches above ALREADY share under this same key, so
    // the vtable inherits exactly the key those functions have (#3450 is the
    // record of that key being sharpened, and this lookup rides it).
    auto* vtGV = module_->getNamedGlobal(vtName);
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
// fix #3461 (v0.283): the element-type resolution the owning-drain wraps
// share. Extracted from `wrapArrayAsIOwningEnumerator` when the LinkedList
// sibling needed the identical rule — a second copy of it would be a fifth
// spelling of the same grid decision (#3419 point 1), and the rule is subtle
// enough to drift (the unresolved-type-parameter arm below is #990's, and a
// copy that forgot it would key an `i32` machinery for a class element).
//
// `IOwningEnumerable<T>` constrains `T: class` (stdlib §4.13), so the element
// is always a class pointer. Resolve through the active per-instantiation
// substitution in case `iOwnEnumTy` is a generic template; the bare
// type-parameter case lowers as a class pointer (§29.2 #990 mirror).
llvm::Type* Emitter::owningDrainElemType(const sema::TypeRef* iOwnEnumTy) {
    const sema::TypeRef* elemTr = (iOwnEnumTy && !iOwnEnumTy->genericArgs.empty())
                                      ? iOwnEnumTy->genericArgs[0]
                                      : nullptr;
    const sema::TypeRef* resolvedElem = resolveTypeParamRef(elemTr);
    if (resolvedElem && resolvedElem != elemTr)
        return lowerTypeRef(resolvedElem);
    if (elemTr && elemTr->kind == sema::TypeRef::Kind::Named &&
        elemTr->declaration == nullptr)
        return ptrTy();   // unresolved type param — class per §4.13
    if (elemTr)
        return lowerTypeRef(elemTr);
    return ptrTy();
}

// fix #4750 / #4772 (§29.1 / §31.4, v0.307): see the declaration. Mirrors the
// iterator-method TakeCurrent (LlvmBackend_Iterators.cpp, #1349): the uniform
// kExcObjSize exception block, the InvalidOperationException vtable, the
// `bz_exc_addr` signal, and a null return the dispatching caller's pending-
// exception check turns into the raise.
void Emitter::emitOwningDrainTakeCurrentBody(llvm::Function* tcFn,
                                             llvm::StructType* stTy,
                                             unsigned pendIdx) {
    auto* entryBB = llvm::BasicBlock::Create(llvm_, "entry", tcFn);
    auto* okBB = llvm::BasicBlock::Create(llvm_, "tc.ok", tcFn);
    auto* ioeBB = llvm::BasicBlock::Create(llvm_, "tc.ioe", tcFn);
    llvm::IRBuilder<> b(entryBB);
    llvm::Value* self = &*tcFn->arg_begin();
    llvm::Value* pendF = b.CreateStructGEP(stTy, self, pendIdx);
    llvm::Value* pend = b.CreateLoad(ptrTy(), pendF, "odtc.pend");
    b.CreateCondBr(
        b.CreateICmpEQ(pend, llvm::ConstantPointerNull::get(ptrTy()),
                       "odtc.pend.null"),
        ioeBB, okBB);

    b.SetInsertPoint(ioeBB);
    auto* callocTy = llvm::FunctionType::get(ptrTy(), {i64(), i64()}, false);
    auto* callocC = module_->getOrInsertFunction("calloc", callocTy).getCallee();
    llvm::Value* excObj = b.CreateCall(
        callocTy, callocC,
        {llvm::ConstantInt::get(i64(), 1),
         llvm::ConstantInt::get(i64(), kExcObjSize)},
        "odtc.excobj");
    auto vit = builtinVtable_.find("InvalidOperationException");
    if (vit != builtinVtable_.end()) b.CreateStore(vit->second, excObj);
    auto* excAddrTy = llvm::FunctionType::get(ptrTy(), {}, false);
    auto* excAddrC =
        module_->getOrInsertFunction("bz_exc_addr", excAddrTy).getCallee();
    b.CreateStore(excObj, b.CreateCall(excAddrTy, excAddrC, {}, "odtc.exc"));
    b.CreateRet(llvm::ConstantPointerNull::get(ptrTy()));

    // Transfer: null the slot BEFORE returning so a later TakeCurrent /
    // MoveNext / Dispose sees an empty pending (§29.1 single consumption).
    b.SetInsertPoint(okBB);
    b.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), pendF);
    b.CreateRet(pend);
}

llvm::Value* Emitter::wrapArrayAsIOwningEnumerator(
        llvm::Value* arr, const sema::TypeRef* iOwnEnumTy, bool lifo) {
    llvm::Type* elemLlvm = owningDrainElemType(iOwnEnumTy);

    // Element-name key (same scheme as ensureArrayEnumMachinery) so the
    // Dispose / MoveNext / TakeCurrent / vtable are created once per element
    // type. The wrapper is a per-element-type singleton — fresh calls allocate
    // a new instance but reuse the same code and vtable.
    std::string elemName = elemLlvm->isIntegerTy()
        ? ("i" + std::to_string(elemLlvm->getIntegerBitWidth()))
        : (elemLlvm->isPointerTy() ? "ptr" : "f");
    // fix #3405 (stdlib §4.11, v0.283): `lifo` keys the cache as well as the
    // drain primitive. The two variants share the struct LAYOUT but must never
    // share a vtable — a front-to-back MoveNext installed on a stack view
    // would silently yield §4.11's wrong order, which is the defect this fix
    // closes.
    std::string prefix =
        std::string("bz$ownarrenum$") + (lifo ? "lifo$" : "") + elemName + "$";

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
        // fix #3405 (stdlib §4.11 "Iteration order", v0.283): a stack view
        // pops the TOP first, which on the bz_array_t* backing is the LAST
        // slot — `bz_array_drain_next_lifo`, the same primitive the
        // `for var x in stk.Drain()` desugaring already selects
        // (LlvmBackend_Stmts.cpp). Every other array-backed family keeps the
        // front-to-back `bz_array_drain_next`.
        auto* drainFn = module_->getOrInsertFunction(
            lifo ? "bz_array_drain_next_lifo" : "bz_array_drain_next",
            drainFT).getCallee();
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

    // --- TakeCurrent(self) -> ptr — null pending raises IOE; else transfer. ---
    // fix #4750 / #4772 (§29.1, v0.307): this used to hand back a null
    // pending as-is ("null in, null out"), deferring to the caller's
    // `if en.MoveNext()` guard. §29.1 makes the raise normative for every
    // `IOwningEnumerator<T>.TakeCurrent()`: "a second take without an
    // intervening advance, or a take before the first advance / after
    // exhaustion, raises InvalidOperationException". The §29.2 for-in
    // desugaring takes exactly once per successful MoveNext, so the raise
    // arm is unreachable there; only the manual protocol observes it.
    auto* tcFn = module_->getFunction(prefix + "TakeCurrent");
    if (!tcFn) {
        tcFn = llvm::Function::Create(
            llvm::FunctionType::get(ptrTy(), {ptrTy()}, false),
            llvm::Function::InternalLinkage, prefix + "TakeCurrent",
            module_.get());
        emitOwningDrainTakeCurrentBody(tcFn, stTy, /*pendIdx=*/2);
    }

    // --- Slot 4 (GetOwningEnumerator): a FRESH enumerator over the same
    // borrowed array, positioned before the collection's current remainder. ---
    auto* geFn = module_->getFunction(prefix + "GetOwningEnumerator");
    if (!geFn) {
        geFn = llvm::Function::Create(
            llvm::FunctionType::get(ptrTy(), {ptrTy()}, false),
            llvm::Function::InternalLinkage, prefix + "GetOwningEnumerator",
            module_.get());
        auto* bb = llvm::BasicBlock::Create(llvm_, "entry", geFn);
        llvm::IRBuilder<> b(bb);
        // fix #3385 (§29.1 / §29.11; stdlib §4.13, v0.282): the previous body
        // was `return self;`, documented as a "safe no-op default" for a slot
        // an IOwningEnumerator<T> receiver never dispatches. The same
        // machinery now also backs the IOwningEnumerable<T> VIEW (the owning
        // arm of wrapArrayAsIEnumerable below), where §29.2's desugaring DOES
        // call slot 4 for the loop enumerator and then disposes BOTH the
        // source view and that enumerator — returning `self` would free one
        // struct twice. A fresh clone is also exactly what stdlib §4.13
        // requires of a drain view: "each GetOwningEnumerator() call on a live
        // view yields a fresh enumerator over the collection's *current*
        // remainder ... a re-enumeration continues draining what remains,
        // never replaying what already left" — which falls out of sharing the
        // borrowed `arr` and starting with an empty `pending`. The clone owns
        // neither the array nor a pending element, so its Dispose is the same
        // free-the-struct path as the source's.
        llvm::Value* self = &*geFn->arg_begin();
        uint64_t cloneSz = module_->getDataLayout().getTypeAllocSize(stTy);
        auto* callocFT = llvm::FunctionType::get(ptrTy(), {i64(), i64()}, false);
        auto* callocC =
            module_->getOrInsertFunction("calloc", callocFT).getCallee();
        llvm::Value* clone = b.CreateCall(
            callocFT, callocC,
            {llvm::ConstantInt::get(i64(), 1),
             llvm::ConstantInt::get(i64(), (int64_t)cloneSz)},
            "oaege.clone");
        llvm::Value* vt =
            b.CreateLoad(ptrTy(), b.CreateStructGEP(stTy, self, 0));
        b.CreateStore(vt, b.CreateStructGEP(stTy, clone, 0));
        llvm::Value* arrCp =
            b.CreateLoad(ptrTy(), b.CreateStructGEP(stTy, self, 1));
        b.CreateStore(arrCp, b.CreateStructGEP(stTy, clone, 1));
        b.CreateStore(llvm::ConstantPointerNull::get(ptrTy()),
                      b.CreateStructGEP(stTy, clone, 2));
        b.CreateRet(clone);
    }

    // Vtable: [0]=self, [1]=Dispose, [2]=MoveNext, [3]=TakeCurrent,
    // [4]=GetOwningEnumerator (fresh clone over the current remainder).
    std::string vtName = prefix + "vtable";
    // fix #3477: `getNamedGlobal` (AllowInternal), not the one-argument
    // `getGlobalVariable` -- see wrapOwnedArrayAsIEnumerable for why.
    auto* vtGV = module_->getNamedGlobal(vtName);
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

// fix #3461 (§29.1 / §29.2 / §29.11; stdlib §4.7 / §4.13, v0.283): the
// LinkedList sibling of `wrapArrayAsIOwningEnumerator` — the DRAINING
// enumerator an `IOwningEnumerable<T>` head requires over a node-backed
// `bz_llist_t*` source.
//
// WHY IT HAD TO EXIST. #3385 established the rule (a source of static type
// `IOwningEnumerable<T>` desugars through `TakeCurrent()` per §29.2, so
// element ownership LEAVES the collection at each yield; a borrowing `Current`
// under that head leaves the source still holding elements the loop has
// already discharged) and #3405 taught the second array leaf to ask it. The
// LinkedList family reaches its wrap by a third leaf,
// `wrapLinkedListAsIEnumerable`, whose only enumerator is the node-walking
// BORROWING one — there was no draining variant for it to divert to. Measured
// on 0c5ecd1 with `owned _l: OwningLinkedList<Item>` read through a §28.6
// stored view at an `IOwningEnumerable<Item>` head: `rem=3` after what should
// be an owning drain, then `heap-use-after-free` in `bz$elemDispose` <-
// `bz_llist_free` <- `C$DisposeCore` — #3385's failure byte for byte.
//
// STRUCTURE. Deliberately field-for-field the array wrap's: a 3-pointer frame
// `{ ptr vtable, ptr list, ptr pending }` that BORROWS the list and owns only
// the class pointer parked in `pending`. Only the drain primitive differs —
// `bz_llist_drain_next` pops the head node and unlinks it WITHOUT disposing
// (stdlib §4.7's `Drain()` primitive, head-to-tail per §28.11's iteration
// order), where the array wrap pops a slot. The container is untouched: the
// field's holder still frees the (now empty) list.
//
//   * MoveNext (slot 2) pops the next element into `pending`, disposing an
//     advanced-but-untaken previous first (§29.1's disposition for an
//     abandoned pending, the same rule the array wrap follows).
//   * TakeCurrent (slot 3) moves `pending` out and NULLS the slot — §29.1:
//     "transfers ownership of the most-recently-yielded value to the caller,
//     leaving the enumerator's internal slot empty".
//   * Dispose (slot 1) disposes a non-null `pending`, then frees the frame.
//     The LIST is NOT freed here — the source's owner disposes it.
//   * Slot 4 (GetOwningEnumerator) mints a FRESH frame over the same borrowed
//     list with an empty `pending`, which §29.2's desugaring requires (it
//     calls slot 4 and then disposes BOTH the view and the enumerator, so
//     returning `self` would free one frame twice) and which stdlib §4.13's
//     drain-view contract independently requires ("a re-enumeration continues
//     draining what remains, never replaying what already left" — which falls
//     out of sharing the list, since the drain has already unlinked the nodes
//     that left).
llvm::Value* Emitter::wrapLinkedListAsIOwningEnumerator(
        llvm::Value* list, const sema::TypeRef* iOwnEnumTy) {
    llvm::Type* elemLlvm = owningDrainElemType(iOwnEnumTy);

    // Element-name key (the scheme ensureLinkedListEnumMachinery uses) so the
    // Dispose / MoveNext / TakeCurrent / vtable are created once per element
    // type. The struct name is distinct from the borrowing wrap's
    // ("llenum.<elem>") because the LAYOUT differs — 3 fields with a `pending`
    // slot, not 4 with a node cursor pair.
    std::string elemName = elemLlvm->isIntegerTy()
        ? ("i" + std::to_string(elemLlvm->getIntegerBitWidth()))
        : (elemLlvm->isPointerTy() ? "ptr" : "f");
    std::string prefix = "bz$ownllenum$" + elemName + "$";
    std::string stName = "ownllenum." + elemName;
    auto* stTy = llvm::StructType::getTypeByName(llvm_, stName);
    if (!stTy)
        stTy = llvm::StructType::create(llvm_,
            {ptrTy(), ptrTy(), ptrTy()}, stName);
    auto* voidTy = llvm::Type::getVoidTy(llvm_);

    // --- Dispose(self) — dispose pending (if any), free the frame. ---
    auto* dispFn = module_->getFunction(prefix + "Dispose");
    if (!dispFn) {
        dispFn = llvm::Function::Create(
            llvm::FunctionType::get(voidTy, {ptrTy()}, false),
            llvm::Function::InternalLinkage, prefix + "Dispose", module_.get());
        auto* bb = llvm::BasicBlock::Create(llvm_, "entry", dispFn);
        // §28: emitDisposeAndReclaim inserts through the Emitter's `builder_` /
        // `currentFn_`. Swap them in for the helper body and restore after, so
        // the caller's own insert point is untouched (the save pattern
        // wrapArrayAsIOwningEnumerator uses, and for the same #2218 reason —
        // an unsaved builder emits the helper's instructions into the wrong
        // function).
        llvm::Function* savedCurrentFn = currentFn_;
        auto* savedBB = builder_.GetInsertBlock();
        llvm::BasicBlock::iterator savedIP{};
        if (savedBB) savedIP = builder_.GetInsertPoint();
        currentFn_ = dispFn;
        builder_.SetInsertPoint(bb);
        llvm::Value* self = dispFn->arg_begin();
        llvm::Value* pendF = builder_.CreateStructGEP(stTy, self, 2);
        llvm::Value* pend = builder_.CreateLoad(ptrTy(), pendF, "olld.pend");
        llvm::Value* isNull = builder_.CreateICmpEQ(
            pend, llvm::ConstantPointerNull::get(ptrTy()), "olld.pend.null");
        auto* dispBB = llvm::BasicBlock::Create(llvm_, "olld.disp", dispFn);
        auto* contBB = llvm::BasicBlock::Create(llvm_, "olld.cont", dispFn);
        builder_.CreateCondBr(isNull, contBB, dispBB);
        builder_.SetInsertPoint(dispBB);
        // Null the slot BEFORE disposing so a re-entrant Dispose (the class's
        // own slot-1 cascading back here) cannot re-dispose the same pointer.
        builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), pendF);
        emitDisposeAndReclaim(pend);
        builder_.CreateBr(contBB);
        builder_.SetInsertPoint(contBB);
        auto* freeFn = module_->getOrInsertFunction(
            "free", llvm::FunctionType::get(voidTy, {ptrTy()}, false))
            .getCallee();
        builder_.CreateCall(
            llvm::FunctionType::get(voidTy, {ptrTy()}, false), freeFn, {self});
        builder_.CreateRetVoid();
        currentFn_ = savedCurrentFn;
        if (savedBB) builder_.SetInsertPoint(savedBB, savedIP);
    }

    // --- MoveNext(self) -> i1 — pop the head node via bz_llist_drain_next. ---
    auto* mnFn = module_->getFunction(prefix + "MoveNext");
    if (!mnFn) {
        mnFn = llvm::Function::Create(
            llvm::FunctionType::get(i1(), {ptrTy()}, false),
            llvm::Function::InternalLinkage, prefix + "MoveNext", module_.get());
        auto* bb = llvm::BasicBlock::Create(llvm_, "entry", mnFn);
        llvm::Function* savedCurrentFn = currentFn_;
        auto* savedBB = builder_.GetInsertBlock();
        llvm::BasicBlock::iterator savedIP{};
        if (savedBB) savedIP = builder_.GetInsertPoint();
        currentFn_ = mnFn;
        builder_.SetInsertPoint(bb);
        llvm::Value* self = mnFn->arg_begin();
        // An advanced-but-untaken previous pending is disposed before the next
        // advance — the frame holds ONE pending slot, and §29.1's standard
        // disposition for an abandoned pending is "dispose".
        llvm::Value* pendF = builder_.CreateStructGEP(stTy, self, 2);
        llvm::Value* prev = builder_.CreateLoad(ptrTy(), pendF, "ollm.prev");
        llvm::Value* prevNull = builder_.CreateICmpEQ(
            prev, llvm::ConstantPointerNull::get(ptrTy()), "ollm.prev.null");
        auto* dispPrevBB = llvm::BasicBlock::Create(llvm_, "ollm.dispprev", mnFn);
        auto* afterDispBB = llvm::BasicBlock::Create(llvm_, "ollm.after", mnFn);
        builder_.CreateCondBr(prevNull, afterDispBB, dispPrevBB);
        builder_.SetInsertPoint(dispPrevBB);
        builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), pendF);
        emitDisposeAndReclaim(prev);
        builder_.CreateBr(afterDispBB);
        builder_.SetInsertPoint(afterDispBB);
        llvm::Value* listF = builder_.CreateStructGEP(stTy, self, 1);
        llvm::Value* listPtr = builder_.CreateLoad(ptrTy(), listF, "ollm.list");
        // #2264 / §28.3, the array wrap's hazard verbatim: bz_llist_drain_next
        // returns false WITHOUT writing *val_out on an empty list, so the
        // out-slot must be zero-initialised on EVERY invocation — otherwise
        // the final (false) MoveNext publishes the previous call's stale
        // pointer into `pending` and the scope-end Dispose double-frees an
        // already-discharged element. The alloca + store go in the entry
        // block, which runs on every call.
        auto* allocaBB = &mnFn->getEntryBlock();
        llvm::IRBuilder<> ab(allocaBB, allocaBB->begin());
        auto* outSlot = ab.CreateAlloca(ptrTy(), nullptr, "ollm.out");
        ab.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), outSlot,
                       "ollm.out.init");
        auto* drainFT = llvm::FunctionType::get(i1(), {ptrTy(), ptrTy()}, false);
        auto* drainFn = module_->getOrInsertFunction(
            "bz_llist_drain_next", drainFT).getCallee();
        llvm::Value* ok = builder_.CreateCall(
            drainFT, drainFn, {listPtr, outSlot}, "ollm.ok");
        llvm::Value* popped =
            builder_.CreateLoad(ptrTy(), outSlot, "ollm.popped");
        builder_.CreateStore(popped, builder_.CreateStructGEP(stTy, self, 2));
        builder_.CreateRet(ok);
        currentFn_ = savedCurrentFn;
        if (savedBB) builder_.SetInsertPoint(savedBB, savedIP);
    }

    // --- TakeCurrent(self) -> ptr — null pending raises IOE; else transfer. ---
    // fix #4750 / #4772 (§29.1, v0.307): the same body as the array leaf.
    auto* tcFn = module_->getFunction(prefix + "TakeCurrent");
    if (!tcFn) {
        tcFn = llvm::Function::Create(
            llvm::FunctionType::get(ptrTy(), {ptrTy()}, false),
            llvm::Function::InternalLinkage, prefix + "TakeCurrent",
            module_.get());
        emitOwningDrainTakeCurrentBody(tcFn, stTy, /*pendIdx=*/2);
    }

    // --- Slot 4 (GetOwningEnumerator): a FRESH frame over the same borrowed
    // list, with an empty pending — never `self` (§29.2 disposes both the view
    // and the loop enumerator; sharing one frame would free it twice). ---
    auto* geFn = module_->getFunction(prefix + "GetOwningEnumerator");
    if (!geFn) {
        geFn = llvm::Function::Create(
            llvm::FunctionType::get(ptrTy(), {ptrTy()}, false),
            llvm::Function::InternalLinkage, prefix + "GetOwningEnumerator",
            module_.get());
        auto* bb = llvm::BasicBlock::Create(llvm_, "entry", geFn);
        llvm::IRBuilder<> b(bb);
        llvm::Value* self = &*geFn->arg_begin();
        uint64_t cloneSz = module_->getDataLayout().getTypeAllocSize(stTy);
        auto* callocFT = llvm::FunctionType::get(ptrTy(), {i64(), i64()}, false);
        auto* callocC =
            module_->getOrInsertFunction("calloc", callocFT).getCallee();
        llvm::Value* clone = b.CreateCall(
            callocFT, callocC,
            {llvm::ConstantInt::get(i64(), 1),
             llvm::ConstantInt::get(i64(), (int64_t)cloneSz)},
            "ollge.clone");
        llvm::Value* vt =
            b.CreateLoad(ptrTy(), b.CreateStructGEP(stTy, self, 0));
        b.CreateStore(vt, b.CreateStructGEP(stTy, clone, 0));
        llvm::Value* listCp =
            b.CreateLoad(ptrTy(), b.CreateStructGEP(stTy, self, 1));
        b.CreateStore(listCp, b.CreateStructGEP(stTy, clone, 1));
        b.CreateStore(llvm::ConstantPointerNull::get(ptrTy()),
                      b.CreateStructGEP(stTy, clone, 2));
        b.CreateRet(clone);
    }

    // Vtable: [0]=self, [1]=Dispose, [2]=MoveNext, [3]=TakeCurrent,
    // [4]=GetOwningEnumerator (the same 5-slot §29.11 shape).
    std::string vtName = prefix + "vtable";
    // fix #3477: `getNamedGlobal` (AllowInternal), not the one-argument
    // `getGlobalVariable` -- see wrapOwnedArrayAsIEnumerable for why.
    auto* vtGV = module_->getNamedGlobal(vtName);
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

    // Allocate + fill the frame { vtable, list, pending=null }.
    uint64_t stSz = module_->getDataLayout().getTypeAllocSize(stTy);
    auto* callocFn2 = declareC("calloc", ptrTy(), {i64(), i64()});
    llvm::Value* en = builder_.CreateCall(callocFn2,
        {builder_.getInt64(1), builder_.getInt64((int64_t)stSz)}, "ownllenum");
    builder_.CreateStore(vtGV, builder_.CreateStructGEP(stTy, en, 0));
    builder_.CreateStore(list, builder_.CreateStructGEP(stTy, en, 1));
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
    // fix #3570 (§29.2 / §28.9 / §4.8, v0.286): the pre-fix scheme collapsed
    // every non-integer, non-pointer element type (notably structs) to the
    // single letter `"f"`, so two different struct element types in one module
    // — e.g. `DictionaryEntry<int,int>` (8 B) and `DictionaryEntry<string,int>`
    // (16 B) — shared one `bz$arrenum$f$Current` whose baked-in stride was the
    // first creator's. Suffix the alloc size so each distinct stride gets its
    // own machinery; integer/pointer kinds still keep their discriminating
    // bit-width / `"ptr"` segment.
    //
    // fix #3570 / §6b unification (v0.286): this site now feeds through the
    // shared `enumMachineryElementName` helper — see the array twin above.
    std::string elemName = enumMachineryElementName(elemTy);
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
    // fix #3477: `getNamedGlobal` (AllowInternal), not the one-argument
    // `getGlobalVariable` -- see wrapOwnedArrayAsIEnumerable for why.
    auto* vtGV = module_->getNamedGlobal(vtName);
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

// §29.2 / §28.9 / stdlib §4.8 — fix #3570 / #3661 (v0.286). See declaration in
// LlvmBackend_impl.hpp. The shared cache-key element-name segment for the
// four enumerable-machinery families. Integer / pointer kinds keep their
// discriminating `i<bitwidth>` / `"ptr"` segments; the non-int / non-ptr
// fallback broadens to `"f<allocSize>"` so distinct struct strides get
// distinct machinery.
std::string Emitter::enumMachineryElementName(llvm::Type* elemTy) const {
    if (elemTy->isIntegerTy())
        return "i" + std::to_string(elemTy->getIntegerBitWidth());
    if (elemTy->isPointerTy())
        return "ptr";
    return "f" + std::to_string(
        (int64_t)module_->getDataLayout().getTypeAllocSize(elemTy));
}

// §29.2 / §28.9 / stdlib §4.6 / §4.7 — fix #3663 (v0.286). See declaration
// in LlvmBackend_impl.hpp. The LinkedList-family (`llenum`) cache-key
// element-name segment. NOT the same alphabet as `enumMachineryElementName`
// above: `ensureLinkedListEnumMachinery`'s `Current` returns `elemTy` BY
// VALUE (`FunctionType::get(elemTy, {ptrTy()}, false)`, `CreateRet(CreateLoad
// (elemTy, vp))`), and MEASURING the same-alloc-size/different-LAYOUT
// question this issue raised (a `record IntPair(X: int, Y: int)` — 2 ints,
// x86-64 SysV INTEGER class — paired with a `record OneDouble(V: double)` —
// one double, SSE class — both 8 B) against an allocSize-only key shows it
// is NOT sufficient: the second record's for-in silently read the wrong
// return-register class through the shared, first-creator-typed `Current`
// (`sum=0` instead of `sum=300` — a WRONG VALUE, not a crash, since both
// registers are architecturally valid — LANE_RULES §6c "loud -> quiet is a
// rejected trade" would forbid SHIPPING this as a narrowing; it is instead
// the reason this key widens past the allocSize scheme). A struct/class
// element type in Bezel always lowers (`recordStruct`/`classStruct`,
// LlvmBackend_Layout.cpp) to a NAMED (identified) `llvm::StructType` unique
// per declaration and per generic instantiation (`recordStruct` folds
// generic arguments into a `$specN` name; LLVM's own struct-name uniquing —
// `StructType::create` appends a numeric suffix on a name clash — is the
// safety net if two distinct declarations ever produced the same raw name),
// so the struct's own LLVM type name already IS full type identity, at no
// extra bookkeeping cost.
std::string Emitter::linkedListEnumElementName(llvm::Type* elemTy) const {
    if (elemTy->isIntegerTy())
        return "i" + std::to_string(elemTy->getIntegerBitWidth());
    if (elemTy->isPointerTy())
        return "ptr";
    // A bare floating-point element (`LinkedList<double>`, no wrapping
    // record) is not an `llvm::StructType` — give it its own scalar-width
    // arm rather than letting it fall into the struct arm (false, wrong
    // shape) or the byte-size fallback below (which would collide it with
    // an equal-size struct, reintroducing exactly the hazard this helper
    // exists to close).
    if (elemTy->isFloatingPointTy())
        return "fp" + std::to_string(elemTy->getPrimitiveSizeInBits());
    if (elemTy->isStructTy()) {
        auto* st = llvm::cast<llvm::StructType>(elemTy);
        if (!st->isLiteral() && st->hasName())
            return "s$" + st->getName().str();
    }
    // Fallback for an anonymous/literal aggregate — not reachable through
    // any current Bezel value-type lowering (every record/class lowers to a
    // NAMED struct above), kept only so an unnamed aggregate degrades to
    // the #3570/#3661 allocSize scheme rather than asserting.
    return "f" + std::to_string(
        (int64_t)module_->getDataLayout().getTypeAllocSize(elemTy));
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

    // fix #3661 (§29.2 / §28.9 / §4.8, v0.286): the inline cache-key spelling
    // collapsed every struct element type to `"f"`, so two different struct
    // element types in one module — e.g. `Stack<Point>` (8 B) and
    // `Stack<BigPoint>` (16 B) — shared one `bz$stackenum$f$Current` whose
    // baked-in stride was the first creator's (and whose `Current` returned
    // the first creator's elemTy). Use the shared helper so each distinct
    // stride gets its own machinery; integer / pointer kinds still keep their
    // discriminating bit-width / `"ptr"` segment.
    std::string elemName = enumMachineryElementName(elemTy);
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
    // fix #3477: `getNamedGlobal` (AllowInternal), not the one-argument
    // `getGlobalVariable` -- see wrapOwnedArrayAsIEnumerable for why.
    auto* vtGV = module_->getNamedGlobal(vtName);
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
    // fix #3405 (§29.1 / §29.2; stdlib §4.11 / §4.13, v0.283): the owning-head
    // divert #3385 gave the FORWARD leaf, asked here through the one shared
    // authority. Reaching this leaf means the source is the stack family, so
    // the drain is `bz_array_drain_next_lifo` — §4.11's pop order, the same
    // order the borrowing machinery below produces. Without this the
    // `IOwningEnumerable<T>` head got a borrowing `Current` over a stack
    // source: the loop discharged each element per §29.2 while the source
    // still held it, which is exactly the double dispose #3385 closed for the
    // forward families.
    if (llvm::Value* drain =
            owningDrainWrapForHead(arr, iEnumTy, DrainBacking::ArrayLifo))
        return drain;
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

// fix #2987 (§28.3 / §28.9 / §29.2, v0.240): the OWNING sibling of
// wrapStackAsIEnumerator. Same 4-field struct layout ({vtable, arr, idx, esz})
// and same before-first cursor (idx = bz_array_len(arr) — runtime value, for
// §4.11 pop order), but a SEPARATE vtable whose slot-1 Dispose frees the
// backing bz_array AND the wrapper frame in one call. Mirror of
// wrapOwnedArrayAsIEnumerable for the reverse-order Stack machinery —
// ensureStackEnumMachinery itself is unchanged (it still builds the BORROWING
// vtable the non-`new` local-decl cases, the explicit-GetEnumerator path, and
// the argument/return coercions reuse); this is the lone OWNING variant the
// "unbound `new` Stack" local-decl case needs. Slot-1 Dispose walks the
// wrapper struct (same struct type as the plain wrap) to free the bz_array
// the wrapper OWNS — the snapshotted idx/esz are not part of the owning
// responsibility. Slot 4 stamps fresh NON-owning clones (GetEnumerator) so a
// per-loop enumerator disposes only its own struct while the wrapper owns the
// array exactly once at the local's scope end (§28.3 — leak-free, no
// double-free).
//
// fix #3450 (§28.2 / §28.9 / §28.11, v0.283): the slot-1 `bz_array_free`'s
// element disposer is DERIVED from the enumerator's element type, exactly as
// its array twin `wrapOwnedArrayAsIEnumerable` has derived it since #2710 —
// it is no longer hardcoded null. #2987's argument for the null ("the source
// is an unbound `new` by construction, so it is EMPTY") held only at the
// local-decl site it was written for; #3450 makes the RETURN coercion a
// second caller, where the source is a callee-owned local that has been
// PUSHED to before the return, so emptiness is false there. The two wraps
// answering the same question differently is #3419's pattern, and the answer
// that differs is a leak (a `Stack<string>` source retains one +1 per element
// at insertion — the `.Push` arms in `LlvmBackend_Calls.cpp` — which a null
// disposer never releases). The predicate is `wrapOwnedArrayAsIEnumerable`'s
// verbatim, so the two families now agree by construction rather than by
// coincidence.
llvm::Value* Emitter::wrapOwnedStackAsIEnumerator(llvm::Value* arr,
                                                  const sema::TypeRef* iEnumTy) {
    llvm::Type* elemTy = i32();
    if (iEnumTy && !iEnumTy->genericArgs.empty() && iEnumTy->genericArgs[0])
        elemTy = lowerTypeRef(iEnumTy->genericArgs[0]);
    // Reuse the plain struct type (same layout: vtable, arr, idx, esz) and the
    // plain MoveNext/Current functions (direction is purely an idx-encoding
    // detail; the wrapper struct is identical). esz is shared too — both
    // forward and reverse wrappers index with the same element stride.
    ArrEnumMachinery m = ensureStackEnumMachinery(elemTy);
    llvm::StructType* stTy = m.stTy;

    // Element-name key (same scheme as ensureStackEnumMachinery) so the
    // owning Dispose / GetEnumerator / vtable are created once per element
    // type and shared across every OWNING Stack wrap in the module.
    // fix #3661 (§29.2 / §28.9 / §4.8, v0.286): the inline cache-key spelling
    // collapsed every struct element type to `"f"` — two different struct
    // element types in one module (e.g. `Stack<Point>` 8 B and `Stack<BigPoint>`
    // 16 B) shared one `bz$stackenum$f$Current`. Use the shared helper so each
    // distinct stride gets its own machinery.
    std::string elemName = enumMachineryElementName(elemTy);
    std::string plainPrefix = "bz$stackenum$" + elemName + "$";
    // fix #3450 (v0.283): the element-type-derived disposer this wrap's
    // slot-1 `bz_array_free` installs — the array twin's #2710 predicate,
    // asked through the one shared helper rather than re-spelled — and the
    // cache-key segment that keeps two `ptr`-lowered element types from
    // sharing one Dispose.
    llvm::Function* elDisp = ownedWrapElementDisposer(iEnumTy);
    std::string prefix =
        "bz$stackenumown$" + elemName + ownedWrapDisposerKey(elDisp) + "$";

    // Owning Dispose: bz_array_free(self->arr, <element disposer>); free(self).
    auto* dispFn = module_->getFunction(prefix + "Dispose");
    if (!dispFn) {
        dispFn = llvm::Function::Create(
            llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_), {ptrTy()}, false),
            llvm::Function::InternalLinkage, prefix + "Dispose", module_.get());
        auto* bb = llvm::BasicBlock::Create(llvm_, "entry", dispFn);
        llvm::IRBuilder<> b(bb);
        llvm::Value* self = dispFn->arg_begin();
        llvm::Value* arrPtr = b.CreateLoad(ptrTy(),
            b.CreateStructGEP(stTy, self, 1));
        // fix #3450 (§28.11, v0.283): the element-type-derived disposer
        // (computed with the cache key above) — null for the ClassBorrow row
        // an `IEnumerable<T>` head produces, so a class element is NOT
        // disposed here; the loop's borrow does not own it.
        llvm::Value* elDispVal = elDisp
            ? llvm::cast<llvm::Value>(elDisp)
            : llvm::ConstantPointerNull::get(ptrTy());
        auto* freeArrFn = module_->getOrInsertFunction(
            "bz_array_free", llvm::FunctionType::get(
                llvm::Type::getVoidTy(llvm_), {ptrTy(), ptrTy()}, false))
            .getCallee();
        b.CreateCall(
            llvm::FunctionType::get(
                llvm::Type::getVoidTy(llvm_), {ptrTy(), ptrTy()}, false),
            freeArrFn, {arrPtr, elDispVal});
        auto* freeFn = module_->getOrInsertFunction(
            "free", llvm::FunctionType::get(
                llvm::Type::getVoidTy(llvm_), {ptrTy()}, false)).getCallee();
        b.CreateCall(llvm::FunctionType::get(
            llvm::Type::getVoidTy(llvm_), {ptrTy()}, false), freeFn, {self});
        b.CreateRetVoid();
    }

    // Owning GetEnumerator: a fresh NON-owning clone over the same shared
    // array (the plain BORROWING machinery's vtable — `m.vtGV` — is what
    // keeps a per-loop enumerator from freeing the shared array). Positioned
    // before the first element (idx = bz_array_len(arr), re-fetched fresh
    // in THIS clone — never copied from `self`'s idx, which may already be
    // partway through its own walk) so a second `for`/manual walk over the
    // same source replays the §4.11 top-to-bottom pop order from the top,
    // per §29.11. Mirrors the plain machinery's GetEnumerator field-for-field
    // except for the vtable stamp.
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
        // Clone gets the NON-owning vtable (borrows the array — the OWNING
        // wrapper above is the array's sole owner).
        b.CreateStore(m.vtGV, b.CreateStructGEP(stTy, clone, 0));
        llvm::Value* arrCp = b.CreateLoad(ptrTy(),
            b.CreateStructGEP(stTy, self, 1));
        b.CreateStore(arrCp, b.CreateStructGEP(stTy, clone, 1));
        llvm::Value* eszCp = b.CreateLoad(i64(),
            b.CreateStructGEP(stTy, self, 3));
        b.CreateStore(eszCp, b.CreateStructGEP(stTy, clone, 3));
        // before-first cursor: idx = bz_array_len(arr) — fresh fetch, not
        // copied from `self` (the §4.11 pop-order before-first is a RUNTIME
        // value, not a fixed sentinel).
        auto* lenFT = llvm::FunctionType::get(i64(), {ptrTy()}, false);
        auto* lenFn2 = module_->getOrInsertFunction("bz_array_len", lenFT)
            .getCallee();
        llvm::Value* len = b.CreateCall(lenFT, lenFn2, {arrCp});
        b.CreateStore(len, b.CreateStructGEP(stTy, clone, 2));
        b.CreateRet(clone);
    }

    // Owning vtable: [0]=self, [1]=owning Dispose, [2]=plain MoveNext,
    // [3]=plain Current, [4]=owning GetEnumerator. Slots 2/3 reuse the plain
    // machinery's functions (the direction-only change lives in the cursor
    // encoding, which they don't touch).
    auto* mnFn = module_->getFunction(plainPrefix + "MoveNext");
    auto* curFn = module_->getFunction(plainPrefix + "Current");
    std::string vtName = prefix + "vtable";
    // fix #3477: `getNamedGlobal` (AllowInternal), not the one-argument
    // `getGlobalVariable` -- see wrapOwnedArrayAsIEnumerable for why.
    auto* vtGV = module_->getNamedGlobal(vtName);
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

    // Allocate + fill the OWNING enumerator instance { owningVt, arr, idx=len, esz }.
    uint64_t stSz = module_->getDataLayout().getTypeAllocSize(stTy);
    auto* callocFn2 = declareC("calloc", ptrTy(), {i64(), i64()});
    llvm::Value* en = builder_.CreateCall(callocFn2,
        {builder_.getInt64(1), builder_.getInt64(stSz)}, "stkenumown");
    builder_.CreateStore(vtGV, builder_.CreateStructGEP(stTy, en, 0));
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
        // fix #3419 (§29.2, v0.283): the SOURCE axis of the same grid
        // `wrapAsIEnumerableForTarget`'s `actualIsArray` decides — this was a
        // verbatim second copy of those seven names, 70 lines from the first.
        return common::isArrayEnumeratorWrapSourceName(t->headName());
    };
    auto isPureIEnumView = [](const sema::TypeRef* t) -> bool {
        if (!t || t->kind != sema::TypeRef::Kind::Named || t->declaration)
            return false;
        // fix #3419 (§29.2, v0.283): read the shared head classifier.
        return common::isSyncIterFormatEnumerableHead(t->headName());
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
    // fix #3662 (§29.2 / §28.9 / §4.8, v0.286): the inline cache-key spelling
    // collapsed every struct element type to `"f"`, so two different struct
    // element types reaching the LEAF branch below in one module (e.g. an
    // `Array<Array<Point>>` and an `Array<Array<BigPoint>>` both coerced to a
    // doubly-nested `IEnumerable<IEnumerable<T>>`) shared one
    // `bz$viewfac$P$f` factory whose baked-in machinery (vtable/struct/esz —
    // via `ensureArrayEnumMachinery`/`ensureStackEnumMachinery`) was the
    // first creator's. NOTE: fixing #3570/#3661 alone does NOT transitively
    // fix this — this factory's OWN cache lookup
    // (`module_->getFunction(name)`) short-circuits BEFORE ever calling the
    // (correctly keyed) array/stack machinery helper on the second creator's
    // element type, so the second struct type silently reuses the first
    // creator's factory body wholesale. Use the shared helper so each
    // distinct struct stride gets its own factory; integer/pointer kinds
    // keep their discriminating bit-width/`"ptr"` segments. The NESTED
    // branch below reuses this same `elemName`, but in practice `elemTy` is
    // always a pointer there (`viewElem` is itself an `IEnumerable<T>`
    // interface reference, per `isPureIEnumView`), so the observable struct
    // collision is confined to the LEAF branch — see the regression test
    // for the measured repro and the exclusion-diff note in the commit body
    // for why the brief's "leaf is transitively fixed" premise did not hold.
    std::string elemName = enumMachineryElementName(elemTy);

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

    // fix #3662 (§29.2 / §28.9 / §4.8, v0.286): unlike `stName` above (safe to
    // share — the 5-field {vtable,arr,idx,esz,curInner} struct layout is
    // IDENTICAL regardless of the inner shape), this prefix drives the
    // actual Dispose/MoveNext/Current/GetEnumerator/vtable FUNCTION names,
    // and `Current`'s body (below) bakes a direct call to `innerFac` into
    // its IR. `elemName` is always `"ptr"` at this nesting level (`viewElem`
    // is itself an `IEnumerable<T>` interface reference, so `lowerTypeRef`
    // always returns a pointer type here — see `enumMachineryElementName`'s
    // pointer arm) REGARDLESS of what the inner element type actually is.
    // So two different inner shapes reaching this NESTED branch (e.g. an
    // `Array<Array<Point>>` and an `Array<Array<BigPoint>>` view in one
    // module) would share one `bz$arrenumnest$ptr$Current`, whose `innerFac`
    // call is baked in at FIRST creation (`getOrCreate` below returns the
    // existing function without re-running `fill()` — see LANE_RULES §12,
    // an anchor/cache hit is a silent miss, not a rebuild) — the SECOND
    // shape's `Current` silently calls the FIRST shape's `innerFac` on the
    // wrong-sized backing array. Key on `innerFac`'s own (correctly
    // per-shape-unique, after the leaf-branch fix above) name instead, the
    // same discriminator the factory's own `name` (above) already uses.
    std::string prefix = (backingIsStack ? "bz$stackarrenumnest$"
                                         : "bz$arrenumnest$")
                         + innerFac->getName().str() + "$";
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
    // fix #3477: `getNamedGlobal` (AllowInternal), not the one-argument
    // `getGlobalVariable` -- see wrapOwnedArrayAsIEnumerable for why.
    auto* vtGV = module_->getNamedGlobal(vtName);
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


// fix #2987 / #1069: shared machinery for the LinkedList/T> IEnumerable
// wrapper — the cached struct type, the plain (BORROWING) Dispose, the
// node-walking MoveNext/Current, the GetEnumerator that re-positions a fresh
// clone before the head, and the BORROWING vtable. Both wrapLinkedListAsIEnumerable
// (the named-source / shared-source borrowing wrap) and
// wrapOwnedLinkedListAsIEnumerable (the unbound-`new` OWNING wrap, #2987) share
// this single machinery — the OWNING wrap only adds a slot-1 OWNING Dispose
// and an OWNING vtable that points at it. Cached/keyed by element type via
// the "llenum.<elem>" struct name and "bz$llenum$<elem>$" function prefix,
// so one set per element type per module.
Emitter::LlEnumMachinery Emitter::ensureLinkedListEnumMachinery(llvm::Type* elemTy) {
    LlEnumMachinery m;
    // fix #3663 (§29.2 / §28.9 / §4.6 / §4.7, v0.286): the inline cache-key
    // spelling collapsed every struct element type to `"f"`, so two
    // different struct element types in one module — e.g. `LinkedList<Point>`
    // (8 B) and `LinkedList<BigPoint>` (16 B) — shared one
    // `bz$llenum$f$Current` whose baked-in element type was the first
    // creator's, the #3570/#3661 shape. MEASURED here, though: an
    // allocSize-only key (`enumMachineryElementName`, the #3570/#3661/#3662
    // pattern) is NOT sufficient at this site — `Current` returns `elemTy`
    // BY VALUE, and two struct types of the SAME alloc size but a DIFFERENT
    // LAYOUT (a `record IntPair(X: int, Y: int)` vs a `record OneDouble
    // (V: double)`, both 8 B — different x86-64 SysV ABI aggregate-return
    // classes) silently produced a WRONG VALUE sharing one `f8`-keyed
    // `Current` (`sum=0` instead of `sum=300` for the second type's loop),
    // not a crash. Use the STRONGER `linkedListEnumElementName` key instead
    // — see its declaration/definition for why the struct's own LLVM type
    // name (full per-declaration identity) closes this without a mangled
    // layout string. `enumMachineryElementName`'s contract for its existing
    // six call sites is UNCHANGED (LANE_RULES §R12: "do not widen the
    // helper's contract... without stating why") — this is a parallel
    // helper, not a widening of that one, and the same same-size/
    // different-layout gap likely exists at those six sites too (`Current`
    // there also returns `elemTy` by value) but is UNMEASURED and OUT OF
    // SCOPE for this lane; filed separately (see this fix's commit body).
    std::string elemName = linkedListEnumElementName(elemTy);
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
    // fix #3477: `getNamedGlobal` (AllowInternal), not the one-argument
    // `getGlobalVariable` -- see wrapOwnedArrayAsIEnumerable for why.
    auto* vtGV = module_->getNamedGlobal(vtName);
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

    m.stTy = stTy;
    m.vtGV = vtGV;
    return m;
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
    // fix #3461 (§29.1 / §29.2; stdlib §4.7 / §4.13, v0.283): the owning-head
    // divert #3385 gave the forward array leaf and #3405 gave the reverse one,
    // asked here through the SAME shared authority. This leaf is where every
    // coercion position routes a `LinkedList`/`OwningLinkedList` source, so
    // before this it handed out the node-walking BORROWING `Current` at an
    // `IOwningEnumerable<T>` target: the loop discharged each element per
    // §29.2 while the list still held it, and the source's holder then ran
    // `bz_llist_free` with the owning list's element disposer over the very
    // same nodes — heap-use-after-free, #3385's failure at the third family.
    // Reaching this leaf means the source is node-backed, so the drain is
    // `bz_llist_drain_next`.
    if (llvm::Value* drain =
            owningDrainWrapForHead(list, iEnumTy, DrainBacking::List))
        return drain;
    llvm::Type* elemTy = i32();
    if (iEnumTy && !iEnumTy->genericArgs.empty() && iEnumTy->genericArgs[0])
        elemTy = lowerTypeRef(iEnumTy->genericArgs[0]);
    LlEnumMachinery m = ensureLinkedListEnumMachinery(elemTy);
    llvm::StructType* stTy = m.stTy;
    llvm::GlobalVariable* vtGV = m.vtGV;

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

// fix #2987 (§28.3 / §28.9 / §29.2, v0.240): the OWNING sibling of
// wrapLinkedListAsIEnumerable. Same 4-field struct layout ({vtable, list,
// current, next}) and the same node-walking MoveNext/Current bodies (both
// reused from ensureLinkedListEnumMachinery's plain machinery, since the
// node-walking logic never touches element ownership), but a SEPARATE vtable
// whose slot-1 Dispose frees the backing bz_llist AND the wrapper frame in
// one call. Mirror of wrapOwnedArrayAsIEnumerable for the LinkedList
// machinery — the plain wrap's struct type and MoveNext/Current functions are
// unchanged (still keyed on element type); this is the lone OWNING variant
// the "unbound `new` LinkedList" local-decl case needs. Slot 4 stamps fresh
// NON-owning clones (GetEnumerator) so a per-loop enumerator disposes only
// its own struct while the wrapper owns the list exactly once at the local's
// scope end (§28.3 — leak-free, no double-free). Element-disposer-less
// bz_llist_free is correct at the local-decl site because the source is an
// unbound `new` by construction, so it is EMPTY (no constructor takes
// elements; collection constructors reserve a node pool, not contents —
// mirroring #2975 manifestation 3's empty-by-construction predicate).
llvm::Value* Emitter::wrapOwnedLinkedListAsIEnumerable(
        llvm::Value* list, const sema::TypeRef* iEnumTy) {
    llvm::Type* elemTy = i32();
    if (iEnumTy && !iEnumTy->genericArgs.empty() && iEnumTy->genericArgs[0])
        elemTy = lowerTypeRef(iEnumTy->genericArgs[0]);
    // Ensure the plain machinery exists (struct type, plain Dispose, plain
    // MoveNext, plain Current, plain GetEnumerator, plain vtable) — the OWNING
    // wrap reuses slots 2/3 of the OWNING vtable (MoveNext/Current, identical
    // bodies, never touch ownership) and stamps GetEnumerator clones with the
    // plain vtable (so per-loop enumerators free only their own struct).
    LlEnumMachinery m = ensureLinkedListEnumMachinery(elemTy);
    llvm::StructType* stTy = m.stTy;
    llvm::GlobalVariable* plainVtGV = m.vtGV;

    // fix #3663 (§29.2 / §28.9 / §4.6 / §4.7, v0.286): same collapse as
    // `ensureLinkedListEnumMachinery` above — MUST use the SAME
    // `linkedListEnumElementName` helper (not `enumMachineryElementName`),
    // so the OWNING wrap's `plainPrefix` agrees with the plain machinery's
    // key for the same element type (they must name the SAME
    // `bz$llenum$<elemName>$` family so the OWNING vtable's slots 2/3
    // correctly reuse the plain MoveNext/Current `m` just built — a
    // mismatched key here would silently rebuild a SECOND, disconnected
    // "plain" family that the OWNING vtable never points at).
    std::string elemName = linkedListEnumElementName(elemTy);
    std::string plainPrefix = "bz$llenum$" + elemName + "$";
    std::string prefix = "bz$llenumown$" + elemName + "$";
    auto* voidTy = llvm::Type::getVoidTy(llvm_);

    // --- Owning Dispose: bz_llist_free(self->list, null); free(self).
    auto* dispFn = module_->getFunction(prefix + "Dispose");
    if (!dispFn) {
        dispFn = llvm::Function::Create(
            llvm::FunctionType::get(voidTy, {ptrTy()}, false),
            llvm::Function::InternalLinkage, prefix + "Dispose",
            module_.get());
        auto* bb = llvm::BasicBlock::Create(llvm_, "entry", dispFn);
        llvm::IRBuilder<> b(bb);
        llvm::Value* self = dispFn->arg_begin();
        llvm::Value* listPtr = b.CreateLoad(ptrTy(),
            b.CreateStructGEP(stTy, self, 1));
        auto* freeLstFn = module_->getOrInsertFunction(
            "bz_llist_free",
            llvm::FunctionType::get(voidTy, {ptrTy(), ptrTy()}, false))
            .getCallee();
        b.CreateCall(
            llvm::FunctionType::get(voidTy, {ptrTy(), ptrTy()}, false),
            freeLstFn, {listPtr, llvm::ConstantPointerNull::get(ptrTy())});
        auto* freeFn = module_->getOrInsertFunction(
            "free", llvm::FunctionType::get(voidTy, {ptrTy()}, false))
            .getCallee();
        b.CreateCall(llvm::FunctionType::get(voidTy, {ptrTy()}, false),
                     freeFn, {self});
        b.CreateRetVoid();
    }

    // --- Owning GetEnumerator: fresh NON-owning clone over the same shared
    // list. Stamped with the PLAIN vtable (slot 1 Dispose = free(self) only,
    // never the shared backing list) so a per-loop enumerator disposes only
    // its own struct — exactly the contract the borrowing wrap already gives
    // every named-source LinkedList caller. Slots 2/3 are the same plain
    // MoveNext/Current bodies, so iteration order matches the borrowing
    // wrapper field-for-field (§29.11 re-iterability).
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
        auto* callocC = module_->getOrInsertFunction("calloc", callocFT)
                            .getCallee();
        llvm::Value* clone = b.CreateCall(callocFT, callocC,
            {llvm::ConstantInt::get(i64(), 1),
             llvm::ConstantInt::get(i64(), (int64_t)cloneSz)});
        // Clone gets the PLAIN (BORROWING) vtable — same plainVtGV the
        // borrowing wrap uses for named sources, so the per-loop enumerator
        // frees only its own struct.
        b.CreateStore(plainVtGV, b.CreateStructGEP(stTy, clone, 0));
        llvm::Value* lst = b.CreateLoad(ptrTy(),
            b.CreateStructGEP(stTy, self, 1));
        b.CreateStore(lst, b.CreateStructGEP(stTy, clone, 1));
        auto* firstFT = llvm::FunctionType::get(ptrTy(), {ptrTy()}, false);
        auto* firstFn = module_->getOrInsertFunction("bz_llist_first_node",
                                                     firstFT).getCallee();
        llvm::Value* first = b.CreateCall(firstFT, firstFn, {lst});
        b.CreateStore(first, b.CreateStructGEP(stTy, clone, 3));  // next
        b.CreateStore(llvm::ConstantPointerNull::get(ptrTy()),
                      b.CreateStructGEP(stTy, clone, 2));         // current
        b.CreateRet(clone);
    }

    // --- Owning vtable: [0]=self, [1]=owning Dispose, [2]=plain MoveNext,
    // [3]=plain Current, [4]=owning GetEnumerator. MoveNext/Current reuse
    // the plain machinery (the node-walking bodies never touch element
    // ownership; only slot 1's Dispose determines who frees the backing).
    auto* mnFn = module_->getFunction(plainPrefix + "MoveNext");
    auto* curFn = module_->getFunction(plainPrefix + "Current");
    std::string vtName = prefix + "vtable";
    // fix #3477: `getNamedGlobal` (AllowInternal), not the one-argument
    // `getGlobalVariable` -- see wrapOwnedArrayAsIEnumerable for why.
    auto* vtGV = module_->getNamedGlobal(vtName);
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

    // Allocate + fill the OWNING enumerator instance { owningVt, list, current=null, next=first }.
    uint64_t stSz = module_->getDataLayout().getTypeAllocSize(stTy);
    auto* callocFn2 = declareC("calloc", ptrTy(), {i64(), i64()});
    llvm::Value* en = builder_.CreateCall(callocFn2,
        {builder_.getInt64(1), builder_.getInt64((int64_t)stSz)}, "llenumown");
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
// fix #3519 (§29.2, v0.283; stdlib §4.8 / §4.17), WIDENED by fix #3568
// (v0.286): see the declaration in LlvmBackend_impl.hpp for why this gate
// exists. It drills the SAME path `wrapDictionaryAsIEnumerable` drills —
// iEnumTy -> DictionaryEntry<K,V> -> {K, V} — reading only classifications the
// front end already computed (`sema::TypeRef::Kind`, `->nullable`, the
// declaration's `ast::TypeDeclKind`), never a new one.
//
// v0.283 admitted a NON-NULLABLE PRIMITIVE K and V and nothing else, because
// the arm could not discharge a richer value's disposal obligation: records,
// classes and nullables leaked (issue 3568) and `x.Value.<member>` failed to
// lower on the wrapped face (issue 3563). #3568 supplies the missing halves —
// `dictSnapshotValueDisposers` splits the obligation into the source free and
// the snapshot's slot-1 Dispose — so the gate now admits every shape those two
// disposers cover, PROVIDED the caller can name the source dictionary type
// (see the `srcDictTy` reasoning in the body). Issue 3563 is unaffected and
// still open: `var p := x.Value;` then `p.<member>` is the working spelling.
//
// `Object` IS admitted: `PrimitiveType::Object` is a PrimitiveType ("§4 Object
// class — treated as a primitive lookup key", `sema/TypeRef.hpp:30`), so
// `Dictionary<int, Object>` passes this gate. That is deliberate and measured
// clean (`iters=1`, rc 0, LSan clean) — an `Object`-valued entry carries a
// borrowed reference the table does not own, so the null value disposer is
// sound for it exactly as it is for `string`.
bool Emitter::dictEntryWrapShapeSupported(const sema::TypeRef* iEnumTy,
                                          const sema::TypeRef* srcDictTy) {
    if (!iEnumTy || iEnumTy->genericArgs.empty() || !iEnumTy->genericArgs[0])
        return false;
    const sema::TypeRef* entryTy = iEnumTy->genericArgs[0];
    if (entryTy->genericArgs.size() < 2 || !entryTy->genericArgs[0] ||
        !entryTy->genericArgs[1])
        return false;
    // NULLABILITY IS A SEPARATE AXIS, NOT A `Kind`. `sema::TypeRef::Kind` is
    // {Error, Primitive, Named, Unit, InlineArray, Function} — there is no
    // `Nullable` member; `nullable` is a `bool` on the SAME TypeRef
    // (`sema/TypeRef.hpp:43`, and Kind::Primitive's own comment reads
    // "PrimitiveType + nullable"). So a `V?` over a primitive is still
    // `Kind::Primitive` and a kind test alone ADMITS it — measured, a
    // `Dictionary<int, int?>` iterated 200x leaked 3200 B in 400 allocations
    // under LSan, which is issue 3568's null-value-disposer leak reached
    // through a second door: `bz_dict_free(d, null)` never releases the boxed
    // nullable payloads. Every test below therefore spells `->nullable` out
    // rather than relying on the kind: fix #3568 ADMITS a nullable V (the
    // source free now passes `valueBoxFreeThunk`, which reclaims exactly those
    // payloads) and still DECLINES a nullable K.
    using K = sema::TypeRef::Kind;
    const sema::TypeRef* keyTy = entryTy->genericArgs[0];
    const sema::TypeRef* valTy = entryTy->genericArgs[1];

    // The v0.283 answer, kept verbatim: both K and V a NON-NULLABLE primitive.
    bool primK = keyTy->kind == K::Primitive && !keyTy->nullable;
    bool primV = valTy->kind == K::Primitive && !valTy->nullable;
    bool narrow = primK && primV;

    // fix #3568 (§29.2 / §28.9 / §4.8, v0.286). WITHOUT a source dictionary
    // type this stays the v0.283 answer. That is not conservatism for its own
    // sake: every shape the widening admits needs a value disposer, and a
    // disposer can only be derived from the SOURCE type (its head name decides
    // Dictionary vs OwningDictionary vs SortedDictionary; its genericArgs[1] is
    // the V the thunk is built from) — the entry's V alone cannot tell an
    // OwningDictionary's OWNED class value from a Dictionary's BORROWED one,
    // and those need opposite treatment. A caller that cannot name its source
    // therefore gets exactly the shapes that need no disposer at all.
    //
    // ⚠ The head is read with the RAW `headName()` spelling, guarded on the
    // astNode — deliberately NOT the alias-aware `builtinTypeName`, even though
    // the call site uses that one to pick the sorted mint. The invariant that
    // matters is "this gate admits exactly what `dictSnapshotValueDisposers`
    // can discharge", and that helper's ARM 2 bottoms out in
    // `dictValueBoxFreer`, which tests `headName() == "Dictionary"`. Reading
    // the head more cleverly HERE than the disposer does THERE would admit an
    // alias-resolved or imported `Dictionary<K, V?>` whose box then has nobody
    // to free it — a leak newly introduced by a widening, which is the exact
    // failure mode §6b exists to catch. So both read the same spelling, and an
    // aliased / imported dictionary TypeRef falls back to the v0.283 answer:
    // conservative, and byte-identical to today's behaviour for it.
    if (!srcDictTy || !srcDictTy->astNode || !srcDictTy->astNode->name ||
        srcDictTy->astNode->name->parts.empty())
        return narrow;
    std::string_view head = srcDictTy->headName();
    // `SortedDictionary` is minted by `wrapSortedDictionaryAsIEnumerable`
    // (LlvmBackend_Calls2.cpp), which takes no snapshot value disposer, so it
    // keeps the narrow answer. Widening it belongs with that helper.
    if (head != "Dictionary" && head != "OwningDictionary") return narrow;

    // A user-declared value type / reference type reached through a resolved
    // declaration. `declaration == nullptr` on a Named TypeRef is a
    // COMPILER-KNOWN builtin (Shared<T>, Weak<T>, Task, IGrouping, a nested
    // Owning* collection, Span/Guid/DateTime...) — each of those has its own
    // slot policy and none is exercised here, so they stay declined.
    auto namedDeclKind = [](const sema::TypeRef* t) -> const ast::TypeDeclKind* {
        if (!t || t->kind != K::Named || !t->declaration) return nullptr;
        return &t->declaration->kind;
    };

    // K axis. A non-nullable primitive (the v0.283 set) OR a non-nullable
    // ENUM: an enum key takes `bz_dict_keys_gen`'s generic-key table, which the
    // decode loads BY VALUE out of the malloc'd raw-key copy and then frees, so
    // it carries no ownership into the snapshot. MEASURED (200x, LSan):
    // `Dictionary<Color, int>` gives `reds=200 total=6000`, clean.
    //   Still declined on this axis: a NULLABLE key (a tagged struct as a hash
    // key is a shape this path has never carried) and a class / interface /
    // choice key. A RECORD key is declined here for a boring reason rather than
    // a codegen one — §8.3 already requires a hashable key, so the record forms
    // that reach codegen at all are a narrow subset this commit did not measure.
    if (!primK) {
        const ast::TypeDeclKind* kk = namedDeclKind(keyTy);
        if (keyTy->nullable || !kk || *kk != ast::TypeDeclKind::Enum)
            return false;
    }

    // V axis. Admitted:
    //   * Primitive, NULLABLE OR NOT. The non-nullable half is v0.283's set
    //     (scalars, `string`, `Object`); the nullable half is new — `int?` is
    //     the #539 tagged-struct box the decode `CreateLoad`s out by value, and
    //     `dictSnapshotValueDisposers` now frees that box at the source free.
    //     MEASURED (200x, LSan): `Dictionary<int, int?>` gives
    //     `keys=600 present=200`, clean, where the pre-#3568 kind-only gate
    //     leaked 3200 B in 400 allocations.
    //   * ENUM. Admitted NULLABLE OR NOT — the Named branch (`namedDeclKind`
    //     below) has no `->nullable` exclusion, unlike `primV` above (the
    //     v0.283 narrow test) which spells `!valTy->nullable` explicitly. The
    //     two shapes are NOT the same, though: a non-nullable `Color` is a
    //     bare integer in the slot with nothing to dispose, but a nullable
    //     `Color?` is heap-boxed on `.Add` exactly like `int?` —
    //     `isNullableValueTy` folds `isNullableEnumTy` in alongside the
    //     primitive/record nullable forms (LlvmBackend_Calls.cpp's
    //     `encodeValue`, issue #539: "nullable value types (int?, bool?,
    //     enum?, nullable record) lower to a tagged {i8 hasValue; T}
    //     struct"), so `Color?`'s box IS something to release. It needs no
    //     enum-specific handling here only because `dictValueBoxFreer`'s
    //     `isStructTy()` test (`dictValueIsBoxedStruct`) already catches it
    //     the same generic way it catches `int?` — not because there is
    //     nothing to free.
    //     MEASURED (200x, LSan): `Dictionary<int, Color>` gives
    //     `keys=600 total=800`, clean.
    //
    // ⚠ A RECORD, CLASS or INTERFACE value stays DECLINED, and the reason is no
    // longer this issue. #3568's disposal half is solved for them too — ARM 1
    // and ARM 2 of `dictSnapshotValueDisposers` cover the owned class pointer
    // and the record box, and an `OwningDictionary<int, Item>` measured LSan
    // clean at 200x on this build. What is NOT solved is issue 3563: on the
    // WRAPPED entry face `x.Value.<member>` fails to lower (the #2961 internal,
    // store dropped), so a valid program that reads a field of a record- or
    // class-typed value silently computes 0 — measured `n=400 total=0` where
    // the in-place control gives the right answer. Admitting these shapes would
    // convert a deterministic SIGSEGV into a QUIET WRONG ANSWER, which is the
    // trade this arm was narrowed to avoid in the first place. They come in
    // with 3563, not before it; the gate is one `TypeDeclKind` away.
    //   A CHOICE value was ALSO declined for a third, independent reason: it
    // lowers to a bare struct that is neither `valRec` nor `valNullableVal`,
    // so the decode's scalar arm would try to `trunc` an i64 into a struct
    // type — measured as the exact `Trunc only produces integer` IR-verifier
    // crash issue #3758 reports for `for var e in d` over
    // `Dictionary<int, Choice>`. fix #3758 closes THAT reason (decodeDictValueAt
    // now derives struct-vs-scalar from `valLlvm` itself, so it struct-loads
    // a choice correctly), but a choice V stays declined HERE regardless: its
    // disposal obligation through `dictSnapshotValueDisposers` and its
    // `x.Value.<member>` lowering on the wrapped entry face are both
    // unaudited for a choice payload (issue 3563 measured only record/class),
    // so admitting it here would risk the exact quiet-wrong-answer trade the
    // record/class exclusion above was written to avoid. Filed as followup
    // #3767 rather than lifted speculatively.
    if (valTy->kind == K::Primitive) return true;
    const ast::TypeDeclKind* vk = namedDeclKind(valTy);
    return vk && (*vk == ast::TypeDeclKind::Enum ||
                  *vk == ast::TypeDeclKind::Choice);
}

llvm::Value* Emitter::wrapDictionaryAsIEnumerable(
        llvm::Value* dict, const sema::TypeRef* iEnumTy,
        llvm::Function* snapshotValDisposer) {
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

    // fix #3758: `valRec` still selects a debug-name prefix below;
    // decodeDictValueAt's struct-vs-scalar decision no longer needs a
    // per-caller flag (it now reads valLlvm's own LLVM type).
    const sema::TypeSymbol* valRec =
        (valTy->kind == sema::TypeRef::Kind::Named && valTy->declaration &&
         valTy->declaration->kind == ast::TypeDeclKind::Record)
            ? valTy->declaration : nullptr;

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
    auto* widsOutSlot = entryAlloca(ptrTy());   // fix #4499: per-key §4.6 width
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
            {ptrTy(), ptrTy(), ptrTy(), ptrTy(), ptrTy()});
        builder_.CreateCall(kfn, {dict, keysOutSlot, lensOutSlot, widsOutSlot,
                                  cntSlot});
        auto* vfn = declareC("bz_dict_values_str",
            llvm::Type::getVoidTy(llvm_),
            {ptrTy(), ptrTy(), ptrTy()});
        builder_.CreateCall(vfn, {dict, valsOutSlot, entryAlloca(i64())});
    }

    llvm::Value* keysArr = builder_.CreateLoad(ptrTy(), keysOutSlot);
    llvm::Value* lensArr = isStringKey
                               ? builder_.CreateLoad(ptrTy(), lensOutSlot)
                               : nullptr;
    llvm::Value* widsArr = isStringKey            // fix #4499
                               ? builder_.CreateLoad(ptrTy(), widsOutSlot)
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

        // --- decode key (narrow #3717: shared with lowerDictEntryFor and
        // #3576's arm — see LlvmBackend_impl.hpp for the helpers) ---
        llvm::Value* keyDecoded;
        if (isStringKey) {
            auto [strObj, kp] =
                decodeDictStringKeyAt(keysArr, lensArr, widsArr, idxB);
            // Free the raw key bytes from bz_dict_keys_str.
            builder_.CreateCall(freeFn, {kp});
            keyDecoded = strObj;
        } else if (useGen) {
            auto [gk, keyPtr] = decodeDictGenericKeyAt(keyLlvm, keysArr, idxB,
                                                        "denum.kgen");
            // Free the malloc'd raw-bytes key copy.
            builder_.CreateCall(freeFn, {keyPtr});
            keyDecoded = gk;
        } else {
            keyDecoded = llvm::Constant::getNullValue(keyLlvm);
        }

        // --- decode value (mirrors lowerDictEntryFor; fix #3758: struct-vs-
        // scalar is now decided BY decodeDictValueAt from valLlvm itself) ---
        llvm::Value* valDecoded = decodeDictValueAt(
            valLlvm, valsArr, idxB, valRec ? "denum.vrec" : "denum.vnv");

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
    if (isStringKey) {
        builder_.CreateCall(freeFn, {lensArr});
        builder_.CreateCall(freeFn, {widsArr});   // fix #4499
    }
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
    //
    // fix #3568 (§29.2 / §28.9 / §28.11, v0.286): the same wrapper is now also
    // the home of the VALUE obligation the snapshot inherited from a
    // callee-owned source (`snapshotValDisposer`, non-null only for the aliased
    // shapes — an OwningDictionary's class value, a borrowing
    // Dictionary<K, string>'s co-owned +1). It has to live here rather than at
    // the source free for exactly the reason the #2522 key release does: the
    // entries are {K, V} STRUCTS, so `bz_array_free`'s `void(*)(void*)`
    // per-slot disposer cannot reach struct field 1, and disposing at the
    // source free would free the objects the caller is about to iterate.
    if (isStringKey || snapshotValDisposer) {
        return wrapDictionaryEntryArrayAsIEnumerable(
            arr, iEnumTy, entryStTy, esz, /*releaseKey=*/isStringKey,
            snapshotValDisposer);
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
// fix #3568 (§29.2 / §28.9 / §28.11, v0.286): the walk is now the general
// per-entry disposal hook. `releaseKey` (default true — the #2522 contract the
// SortedDictionary caller in LlvmBackend_Calls2.cpp still wants) says field 0
// holds a rebuilt ref-counted bz_string* to release; `valDisposer` (default
// null) is a void(void*) thunk called on field 1 of every entry, non-null only
// when the snapshot INHERITED the source table's value ownership. The same
// struct-field argument covers both: bz_array_free's per-slot disposer is a
// `void(*)(void*)` applied to the SLOT, which for a {K, V} struct array reaches
// neither field.
//
// A non-string-keyed dictionary reaches this path only when `valDisposer` is
// non-null (`releaseKey` false); with neither obligation
// wrapDictionaryAsIEnumerable keeps the plain wrapOwnedArrayAsIEnumerable (the
// keys are inline bytes with no heap ownership, so the plain null-disposer free
// is correct).
llvm::Value* Emitter::wrapDictionaryEntryArrayAsIEnumerable(
        llvm::Value* arr,
        const sema::TypeRef* iEnumTy,
        llvm::StructType* entryStTy,
        int64_t esz,
        bool releaseKey,
        llvm::Function* valDisposer) {
    // The plain array-enum machinery is keyed on the element TYPE — for
    // Dictionary<string,V> that is DictionaryEntry<string,V> (a struct),
    // so ensureArrayEnumMachinery(<entry struct type>) builds the same
    // {ptr vtable, ptr arr, i64 idx, i64 esz} wrapper layout used by
    // wrapOwnedArrayAsIEnumerable. MoveNext/Current from the plain
    // machinery are correct here (they only walk the array).
    ArrEnumMachinery m = ensureArrayEnumMachinery(entryStTy);
    llvm::StructType* stTy = m.stTy;

    // Element-name key matches ensureArrayEnumMachinery's per-element-type
    // scheme. Reusing the same prefix means MoveNext/Current created by the
    // plain machinery are looked up under the SAME name as
    // wrapOwnedArrayAsIEnumerable uses when it was invoked first (e.g. by
    // the foreach dictionary path), avoiding two parallel sets of identical
    // MoveNext/Current for the same entry type. fix #3570 (§29.2 / §28.9 /
    // §4.8, v0.286): include the entry struct's alloc size so two entry types
    // with different strides (DictionaryEntry<int,int> = 8 B vs
    // DictionaryEntry<string,int> = 16 B) get distinct plain MoveNext/Current
    // instances — the pre-fix `"f"` collapse shared one Current at the first
    // creator's stride.
    //
    // fix #3570 / §6b unification (v0.286): this site now feeds through the
    // shared `enumMachineryElementName` helper — see the array twin above.
    // `entryStTy` is a struct type (always); the helper's integer/pointer
    // branches never fire here, but using the same spelling keeps the cache
    // key consistent across the array family — a DictionaryEntry<int,int>
    // minted via the plain array machinery gets the same `f8` segment it
    // would get via the dict-entry wrap, so MoveNext/Current reuse just works.
    std::string elemName = enumMachineryElementName(entryStTy);
    std::string plainPrefix = "bz$arrenum$" + elemName + "$";
    // fix #3568 / #3450's cache-key discipline: the Dispose this builds is
    // SPECIALISED on both obligations, so both are part of the key of every
    // global keyed by `prefix` (the Dispose, the GetEnumerator clone stamp and
    // the owning vtable). Without the value-disposer segment two
    // DictionaryEntry<int, C1> / DictionaryEntry<int, C2> instantiations that
    // share an entry LAYOUT would also share the first creator's Dispose — the
    // exact collapse #3570 measured for MoveNext/Current at a shared stride.
    // The disposer's own symbol name is the key: `elementPolicyFor` hands back
    // one cached thunk per behaviour (bz$elemDispose, bz$strElemRelease,
    // bz$valBoxFree), so equal names mean identical Dispose bodies.
    std::string prefix = "bz$dictentryenumown$" + elemName + "$" +
                         (releaseKey ? "k1" : "k0");
    if (valDisposer) prefix += "$" + valDisposer->getName().str();
    prefix += "$";

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

            if (releaseKey) {
                // strPtr = entryPtr->Key (field 0 of the {K, V} struct,
                // K = bz_string*)
                llvm::Value* strPtr = b.CreateLoad(
                    ptrTy(),
                    b.CreateStructGEP(entryStTy, entryPtr, 0),
                    "de.disp.k");

                // bz_string_release(strPtr) — null-safe (a null slot would have
                // leaked only as a stale sentinel; release is null-safe per
                // §8.6).
                auto* relFn = module_->getOrInsertFunction(
                    "bz_string_release",
                    llvm::FunctionType::get(
                        llvm::Type::getVoidTy(llvm_), {ptrTy()},
                        false)).getCallee();
                b.CreateCall(
                    llvm::FunctionType::get(
                        llvm::Type::getVoidTy(llvm_), {ptrTy()}, false),
                    relFn, {strPtr});
            }

            // fix #3568 (§29.2 / §28.9 / §28.11, v0.286): valDisposer(field 1).
            // Field 1 of the entry is the VALUE word the decode aliased out of
            // the source table — an OwningDictionary's class pointer or a
            // borrowing Dictionary<K, string>'s co-owned bz_string*. The source
            // table was freed with a NULL value disposer at the RETURN
            // coercion precisely so this call is the ONLY one: exactly-once,
            // and after the caller has finished iterating. Every thunk
            // elementPolicyFor / stringElementReleaseThunk hands back is
            // null-safe, so a never-stored slot needs no extra guard here
            // (mirrors the key release above).
            //
            // INVARIANT: field 1 is a POINTER whenever `valDisposer` is
            // non-null. `dictSnapshotValueDisposers` hands one back only from
            // ARM 1 (elementPolicyFor's owned-slot rows — class/interface,
            // Shared/Weak, Task, IGrouping, Subscription, nested owning
            // collection: every one a `ptrTy()` slot) and ARM 3 (a
            // `bz_string*`). The `isPointerTy` test states that rather than
            // assuming it; it can only fail if a future arm hands back a
            // disposer for a by-value slot, and the right answer for such a
            // slot is a box free at the SOURCE (ARM 2), never a call on the
            // struct bytes.
            if (valDisposer && entryStTy->getElementType(1)->isPointerTy()) {
                llvm::Value* valPtr = b.CreateLoad(
                    ptrTy(),
                    b.CreateStructGEP(entryStTy, entryPtr, 1),
                    "de.disp.v");
                b.CreateCall(valDisposer, {valPtr});
            }

            b.CreateStore(
                b.CreateAdd(ci2, b.getInt64(1)), idxSlot);
            b.CreateBr(condBB);
        }

        b.SetInsertPoint(endBB);
        // bz_array_free(arr, null) — the entries' string keys (when
        // `releaseKey`) and their inherited value obligations (when
        // `valDisposer`) have been discharged above; the array itself holds no
        // further heap references at this point. With NEITHER obligation the
        // slots are int / record / borrowed-pointer copies by value — not
        // retained copies — and the source dictionary still owns its values.
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
    // fix #3477: `getNamedGlobal` (AllowInternal), not the one-argument
    // `getGlobalVariable` -- see wrapOwnedArrayAsIEnumerable for why.
    auto* vtGV = module_->getNamedGlobal(vtName);
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
    // fix #2748 (§23.2 / §29.2, v0.240): the THUNK for an EXPRESSION-bodied
    // getter, built on demand by `wrapUserClassAsIEnumerable` when an
    // `IEnumerable<T>`-implementing class's `Current` accessor is an `=>`
    // expression. This is the REACHABLE one of the three unguarded lazy
    // emitters (#2731 item 1): unlike the two indexer builders above, this
    // thunk is reachable from ordinary expression lowering via seven caller
    // chains enumerated in the issue (the most load-bearing are the
    // `emitFunctionBody` / `buildSpecializedMethodFn` /
    // `emitInlineGenericMethod` paths, which is also exactly the context in
    // which `genericSubst_` is non-empty). All three unguarded emitters
    // share `NestedBodyScope`'s reset of the per-body channels (#2170,
    // #2694, #2701, #2702, #2703, #2731 item 1, #2731 item 3, #3125), and
    // each leaves `genericSubst_` inherited as one of the six deliberate-
    // inherit members (#2702).
    //
    // The body must resolve its type parameters under the same substitution
    // its signature was built with — `retTy` is passed IN by the caller,
    // computed under the caller's substitution, which is exactly the
    // argument #2702 used to justify NOT clearing. The original code here
    // cleared `genericSubst_` (`diverged_ = false; genericSubst_.clear();`),
    // making this thunk the odd one out of the six lazy emitters and the
    // only one that would have left a desynced `genericSubst_`/`genericSubstSemaTy_`
    // pair under the new guard: `NestedBodyScope` saves the sema twin and
    // leaves it inherited, while the explicit clear still runs, so the two
    // would have pointed at different substitutions — the opposite direction
    // of the desync #2702 fixed. The clear is removed; `genericSubst_` is
    // now inherited like every other per-body emitter. `genericSubst_`
    // restore-on-exit stays.
    NestedBodyScope nestedScope(*this);

    currentFn_         = fn;
    builder_.SetInsertPoint(entryBB);
    currentThis_       = &*fn->args().begin();
    locals_.clear(); paramSlots_.clear(); refParams_.clear();
    disposeStack_.clear(); loopStack_.clear(); finallyStack_.clear();
    finallyBodyStack_.clear();
    unwindTarget_      = nullptr; propagateBB_ = nullptr;
    currentCaughtExc_  = nullptr; unwindScopeDepth_ = 0;
    diverged_          = false;

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
//
// fix #3197 / §29.1 / §29.2 / §29.11 / §2.8.2 (v0.254): the original code
// took `getEnumSym->decl` (an AST MethodDecl) and bailed to `return src`
// when the cast was null — which it always is for an IMPORTED MethodSymbol
// (`MemberSymbol::decl` is null for imports; the loader does not re-parse
// body text for non-generic classes, see the imported-symbol programme
// handoff §1). The result was a SILENT SIGSEGV (exit 139) when the
// §29.11 enumerator vtable was driven through the raw class instance. The
// fix: when `getEnumDecl` is null, take the return type from
// `ctx_.importedReturnType(getEnumSym)` — the loader records it; and let
// the structural `MoveNext` + `Current` scan walk `importedSymbols()` too.
// The `methodFns_.count(ms)` gate already answers correctly for an
// imported method (declareImportedFunctions populates it).
llvm::Value* Emitter::wrapUserClassAsIEnumerable(llvm::Value* src,
                                                  const sema::TypeSymbol* srcTs,
                                                  const sema::TypeRef* iEnumTy,
                                                  bool ownsSrc) {
    // 1. Find GetEnumerator on the source class.
    sema::MethodSymbol* getEnumSym = findMethodByName(srcTs, "GetEnumerator");
    if (!getEnumSym) return src;

    // 2. Find the concrete enumerator TypeSymbol (same logic as lowerIEnumerableFor).
    auto* getEnumDecl = dynamic_cast<const ast::MethodDecl*>(getEnumSym->decl);
    sema::TypeRef* enumTyRef = nullptr;
    if (getEnumDecl && getEnumDecl->returnType) {
        enumTyRef = ctx_.resolvedType(*getEnumDecl->returnType);
    } else {
        // Imported MethodSymbol — no AST decl. The loader recorded the
        // return type on the side table; reuse it.
        enumTyRef = ctx_.importedReturnType(getEnumSym);
    }
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
            //
            // fix #3197 / §29.1 / §29.11 (v0.254): `ctx_.symbols()` excludes
            // imported symbols by construction (`makeImportedSymbol` keeps
            // them out) — so a class declared in a separate module that is
            // imported here was NEVER considered as the enumerator class,
            // and `enumTs` stayed null. Then `if (!enumTs) return src;`
            // returned the raw, unwrapped instance — the §29.11 enumerator
            // vtable was driven through it, and the runtime SIGSEGV'd.
            // Walk `ctx_.importedSymbols()` in the same pass. The
            // `methodFns_.count(ms)` gate above already answers correctly
            // for an imported method: `declareImportedFunctions`
            // (LlvmBackend_Setup.cpp ~L2038) populates an external-linkage
            // `llvm::Function` for every imported MethodSymbol under
            // `methodFns_[ms]`, regardless of whether the body is locally
            // known.
            auto scanSymbols = [&](const sema::Symbol* symPtr) -> void {
                auto* cand = dynamic_cast<const sema::TypeSymbol*>(symPtr);
                if (!cand || cand->kind != ast::TypeDeclKind::Class) return;
                if (cand == srcTs) return;
                if (structurallyIsEnumerator(cand)) {
                    enumTs = const_cast<sema::TypeSymbol*>(cand);
                }
            };
            for (const auto& sym : ctx_.symbols()) scanSymbols(sym.get());
            if (!enumTs) {
                for (const sema::Symbol* sym : ctx_.importedSymbols())
                    scanSymbols(sym);
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
        // fix #3197 / §23 / §29.11 (v0.254): for an IMPORTED enumerator
        // class, `currentPropSym->decl` is null (PropertySymbol::decl is
        // never set for imports, see the imported-symbol programme handoff
        // §1). The cross-module ABI is the `$propget` stub declared by
        // `declareImportedPropertyGetters` (LlvmBackend_Setup.cpp ~L2633),
        // which populates `importedPropGetterFns_[ps]` — and it engages for
        // this shape: the `Current` here is non-virtual and class-parented,
        // so neither of `declareImportedPropertyGetters`' two skips
        // (virtual, interface-parented) applies.
        //
        // #3275 (§23 / §29.1+§29.2, v0.254): the loader's documented invariant
        // is that `PropertySymbol::decl` is null for every IMPORTED
        // PropertySymbol (ModuleLoader.cpp ~L891, the
        // `MetaMemberKind::Property` arm of `convertMember`, only sets
        // `parent`/`name`/`modifiers`/`scope`/`visibility`/side-table
        // entries; never `decl`). Verified empirically for every shape
        // the for-in driver / wrapper can land on:
        //   - method-form `Current(): T`         → `currentMethodSym`,
        //     never `currentPropSym`, no read.
        //   - non-virtual class-parented `Current` → `importedPropGetterFns_`
        //     populated, the `else` arm below reads `currentFn` from it
        //     (no thunk build, no `pd`).
        //   - virtual class-parented `Current`   → same path post-#3267
        //     (`declareImportedPropertyGetters` now populates the thunk
        //     for VIRTUAL imported properties — fix `251213d`).
        //   - interface-parented `Current`       → `importedPropGetterFns_`
        //     skip path; the `else` arm reads nothing.
        // In every shape `currentPropSym->decl` is null, so the `if (pd)`
        // arm below is unreachable on the for-in driver path. The refute
        // commit `1a74041`'s `!ctx_.isImported(enumTs)` band-aid at this
        // site and the parallel one at `lowerIEnumerableFor` was preserved
        // as defensive until #3275 investigated — it was reverted by the
        // structural fix #3267 (`251213d`).
        //
        // The `#ifndef NDEBUG` invariant check below pins the documented
        // invariant: any future loader change that re-introduces `decl`
        // on an imported PropertySymbol trips the assertion here in
        // debug builds. The compiled-program observable is unchanged.
#ifndef NDEBUG
        if (ctx_.isImported(enumTs) && currentPropSym->decl != nullptr) {
            llvm::errs() << "bezel codegen: imported-symbol invariant "
                            "violated (§23 / #3275): imported "
                         << enumTs->name << "."
                         << currentPropSym->name
                         << " carries a non-null decl. The cross-module "
                            "thunk path below would compile against the "
                            "WRONG class layout (importer-side offsets, "
                            "exporter-side runtime object). Failing "
                            "compilation early so the loader regression "
                            "is caught at first repro, not at runtime.\n";
            abort();
        }
#endif
        auto* pd = dynamic_cast<const ast::PropertyDecl*>(currentPropSym->decl);
        if (pd) {
            if (const ast::PropertyAccessor* acc = blockGetterOf(currentPropSym)) {
                currentFn = ensurePropertyBlockGetter(pd, acc);
            } else if (const ast::Expr* ge = computedGetterOf(currentPropSym)) {
                llvm::Type* propRetTy = pd->type ? lowerType(pd->type.get()) : elemTy;
                if (!propRetTy) propRetTy = elemTy;
                currentFn = ensureExprGetterThunk(currentPropSym, ge, propRetTy);
            }
        } else {
            auto curIt = importedPropGetterFns_.find(currentPropSym);
            if (curIt != importedPropGetterFns_.end()) currentFn = curIt->second;
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
    // fix #3477: `getNamedGlobal` (AllowInternal), not the one-argument
    // `getGlobalVariable` -- see wrapOwnedArrayAsIEnumerable for why.
    auto* vtGV = module_->getNamedGlobal(vtName);
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

        // fix #2746 (§22.3 / §28.9 / §23.4, v0.282): release every `+1` string
        // PRODUCER INDEX that `emitAccessorRefArgCopyIn` pinned for this
        // bracket. The pin bracket owns the index's lifetime across BOTH the
        // copy-in getter AND the write-back setter, so #2717's
        // `exprValueOverride_.count(ie)` suppression in
        // `releaseIndexArgStringProducers` is structurally correct for both
        // accessor sites — but leaves a leak: the suppression skips the
        // release for ALL pinned indices, including CLASS-receiver cases where
        // a release at the bracket's end is safe (the class field is the
        // VALUE, not the INDEX). #2746 closes that leak by relocating the
        // release into the bracket itself, where it fires EXACTLY ONCE — after
        // the call (write-back on the normal edge, no write-back on the
        // exception edge; on both edges the callee is done with the index
        // because it never received it — §23.5's "ref/out in indexer
        // parameter lists" restriction means the index is a lookup key the
        // callee never sees) and BEFORE the pins are erased.
        //
        // GATES. The same two gates `releaseIndexArgStringProducers` already
        // applies, mirrored verbatim so the two halves of the index-argument
        // release discipline share exactly one definition:
        //   * receiver-kind gate — RECORD (§9/§9.7) and INTERFACE receivers
        //     are deliberately excluded. A record's reference fields are
        //     borrows with no independent +1 (§9.7); an interface's runtime
        //     implementor may be record-backed. The accessor might store the
        //     index into a record field, and a release here would dangle that
        //     field. Same discriminant as `emitProducerRelease`'s
        //     `recvDecl->kind == Class` gate. The bracket owns the lifetime
        //     for the call, NOT for any callee-side aliasing — that hazard is
        //     the same one `releaseIndexArgStringProducers` declines.
        //   * `diverged_` — mirror #2717's own `!diverged_` guard; on a
        //     diverged path the callee never ran, but releasing into a
        //     terminated BB is malformed IR. (The exception edge that flips
        //     `emit=false` does NOT set `diverged_` — the divergence flag is
        //     for control flow like `throw`, the exception flag is for
        //     setter-call-site unwind — and the bracket release has to fire
        //     on the exception edge because the callee is done with the index
        //     either way.)
        // The element-type gate that `releaseIndexArgStringProducers` applies
        // does NOT apply here: by the time the bracket release fires, the
        // getter's result has been consumed (the callee wrote to the slot,
        // and the write-back setter has either run with the new slot value or
        // is no longer going to run). The "accessor hands the caller back the
        // index" hazard (#2717's round-3 fix-closed) is exclusively a
        // between-accessor concern; nothing in the bracket reads through the
        // index value after the slot is consumed.
        if (!diverged_ && r.target) {
            if (auto* ix = dynamic_cast<const ast::IndexExpr*>(r.target)) {
                const sema::TypeSymbol* recvDecl =
                    indexAccessReceiverDecl(*ix);
                if (recvDecl &&
                    recvDecl->kind == ast::TypeDeclKind::Class) {
                    // Build a set of pinned expressions so the
                    // "is this index one we pinned" check is O(1) per index
                    // rather than O(|pins|). `pins` is small (one receiver +
                    // one entry per index), so the optimization is incidental;
                    // the correctness benefit is the same: only pinned indices
                    // are released, exactly once.
                    std::unordered_set<const ast::Expr*> pinned;
                    pinned.reserve(r.pins.size());
                    for (const ast::Expr* e : r.pins) pinned.insert(e);
                    for (std::size_t k = 0; k < ix->indices.size(); ++k) {
                        const ast::Expr* ie = ix->indices[k].value.get();
                        if (!ie || !pinned.count(ie)) continue;
                        if (!isPlusOneStringProducer(ie)) continue;
                        auto it = exprValueOverride_.find(ie);
                        if (it == exprValueOverride_.end()) continue;
                        llvm::Value* v = it->second;
                        if (!v || !v->getType()->isPointerTy()) continue;
                        emitStringRelease(v);   // null/sentinel-safe, NOT
                                                 // idempotent — exactly one
                                                 // fire is the discipline
                    }
                }
            }
        }
        for (const ast::Expr* e : r.pins) exprValueOverride_.erase(e);
    }
    recs.clear();
}


// fix #2938 (§22.3 / §23.3 / §23.4, v0.240) — built-in out-target address
// resolver. The ~25 BUILTIN dispatch sites (`int.TryParse`,
// `Dictionary.TryGetValue`, `Convert.TryToInt32`, `DateTime.TryParse`, …) each
// hand-roll their out-target resolution with `lvalueAddr` alone:
//
//     llvm::Value* outAddr = lvalueAddr(*call.args[N].value, outTy);
//     if (!outAddr) return nullptr;
//
// `lvalueAddr` only resolves TRUE-ALIAS locations (local / field / static /
// inline-array element); a PROPERTY or INDEXER slot returns null, the call
// bails, and the §22.3-admitted shape silently compiles to a dropped write
// (#1302's user-method path was widened; #2462 the imported-method path;
// #2579 the ctor-chain path; #2319 the element-policy path; this arm was
// never widened). `resolveOutAddrWithBracket` is the unified fallback the
// builtin sites reuse: try `lvalueAddr` first, and on null fall back to the
// §22.3 copy-in/copy-out bracket via `emitAccessorRefArgCopyIn`, populating
// `sink` so the caller can flush the write-back after the call.
//
// On success `outTy` is set to the slot's effective TYPE — for the
// `lvalueAddr` path, the type the caller passed in (a true-alias location
// reports its own width); for the bracket path, the LOCATION's type
// (`lowerTypeRef(exprType(target))`), so the builtin's existing store
// helpers (`storeTryOutValue`, `storeTryParseOut`, `CreateStore`) work
// unchanged. The bracket's paramTy-driven widening refinement (#2266) is
// deliberately NOT mirrored here: an imported method's own AST param is
// never reconstructed (#1221's limitation), and §22.3's "exact referent
// type" rule means the target's own exprType already gives the correct
// slot type in the ordinary case (the imported-method twin at Calls2.cpp
// ~7927 follows the same `paramTy=nullptr` convention).
//
// Returns null only when neither path produced an address — i.e. a §22.3-
// rejected shape (read-only property, owned-state slot, static auto-property,
// `out owned` of an inadmissible type) that the caller is expected to
// surface as its own diagnostic. // per §22.3 / §23.3 / §23.4
llvm::Value* Emitter::resolveOutAddrWithBracket(
    const ast::Expr& target, llvm::Type*& outTy,
    std::vector<AccessorRefArg>& sink) {
    llvm::Value* v = lvalueAddr(target, outTy);
    if (v) return v;
    v = emitAccessorRefArgCopyIn(target, /*paramTy=*/nullptr,
                                 /*isOut=*/true, sink);
    if (v) {
        // The bracket sized its slot to the LOCATION's type (widened to
        // paramTy when larger). The store helpers use outTy to decide
        // between a bare store and a `{i8,T}` tagged-struct marshalling
        // (#1470); without this set, the bracket's slot would be mis-typed
        // and the builtin's store could miss the value half.
        if (const sema::TypeRef* et = ctx_.exprType(target))
            outTy = lowerTypeRef(et);
    }
    return v;
}


// fix #5289 (§17.13, §22.6, v0.313): see the declaration in
// LlvmBackend_impl.hpp. The slot is zero-initialised so a builtin that writes
// only on its success path leaves a defined value; it is never read back.
llvm::Value* Emitter::resolveBuiltinOutArg(
    const ast::CallArgument& a, llvm::Type* discardTy, llvm::Type*& outTy,
    std::vector<AccessorRefArg>& sink) {
    if (!a.value) {
        outTy = discardTy ? discardTy : i32();
        llvm::AllocaInst* scratch = entryAlloca(outTy);
        // Zero-init in the ENTRY block, right after the alloca (never at the
        // current insertion point: a site may already have emitted its branch
        // terminator before it resolves the out argument — TryRead*), so it
        // dominates every path and a never-written slot reads null/0.
        {
            llvm::IRBuilder<> eb(scratch->getParent(),
                                 scratch->getNextNode()
                                     ? scratch->getNextNode()->getIterator()
                                     : scratch->getParent()->end());
            eb.CreateStore(llvm::Constant::getNullValue(scratch->getAllocatedType()),
                           scratch);
        }
        return scratch;
    }
    return resolveOutAddrWithBracket(*a.value, outTy, sink);
}

llvm::Type* Emitter::tryOutNullableSlotTy(const sema::TypeRef* elem) {
    if (!elem) return ptrTy();
    sema::TypeRef nn = *elem;
    nn.nullable = true;
    llvm::Type* t = lowerTypeRef(&nn);
    return t ? t : ptrTy();
}

void Emitter::releaseDiscardedSharedOut(llvm::Value* scratch) {
    llvm::Value* h = builder_.CreateLoad(ptrTy(), scratch, "outdisc.sh");
    llvm::Value* isNull = builder_.CreateICmpEQ(
        h, llvm::ConstantPointerNull::get(ptrTy()));
    auto* relBB = llvm::BasicBlock::Create(llvm_, "outdisc.shrel", currentFn_);
    auto* contBB = llvm::BasicBlock::Create(llvm_, "outdisc.shcont", currentFn_);
    builder_.CreateCondBr(isNull, contBB, relBB);
    builder_.SetInsertPoint(relBB);
    builder_.CreateCall(declareC("bz_shared_release",
                                 llvm::Type::getVoidTy(llvm_), {ptrTy()}),
                        {h});
    builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), scratch);
    builder_.CreateBr(contBB);
    builder_.SetInsertPoint(contBB);
}

void Emitter::disposeDiscardedOwnedOut(llvm::Value* scratch,
                                       const sema::TypeRef* elem) {
    if (!scratch || !elem || elem->kind != sema::TypeRef::Kind::Named ||
        !elem->declaration)
        return;
    const ast::TypeDeclKind tk = elem->declaration->kind;
    if (tk != ast::TypeDeclKind::Class && tk != ast::TypeDeclKind::Interface)
        return;
    llvm::Value* obj = builder_.CreateLoad(ptrTy(), scratch, "outdisc.obj");
    llvm::Value* isNull = builder_.CreateICmpEQ(
        obj, llvm::ConstantPointerNull::get(ptrTy()));
    auto* dispBB =
        llvm::BasicBlock::Create(llvm_, "outdisc.dispose", currentFn_);
    auto* contBB = llvm::BasicBlock::Create(llvm_, "outdisc.cont", currentFn_);
    builder_.CreateCondBr(isNull, contBB, dispBB);
    builder_.SetInsertPoint(dispBB);
    emitDisposeAndReclaim(obj);
    builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), scratch);
    builder_.CreateBr(contBB);
    builder_.SetInsertPoint(contBB);
}

// fix #3777 (§22.5 "Optional parameters and named arguments" / §22.8 step 1 /
// §12.8, v0.286): the ONE materialization of an omitted §22.5 default for a
// LOCALLY declared formal list. Extracted verbatim from `buildOrderedArgs`,
// whose tail loop this used to be, because a §12.8 chain call
// (`Emitter::emitChainCall`) now has to fill exactly the same omitted tail and
// a second spelling of "what value does an omitted parameter take" is the
// defect class CLAUDE.md §6 names. `ordered` is sized to `params` with a null
// in every slot the call did not supply; on return every slot holds a value.
// Returns false — leaving the caller to abandon the call exactly as it did
// before — for a slot with no default, or a default that failed to lower.
// `orderedExprs`, when given, is sized to `params` and receives each filled
// slot's own default expression so the caller can recover its signedness.
bool Emitter::fillOmittedParamDefaults(
        const std::vector<ast::Param>& params,
        std::vector<llvm::Value*>& ordered,
        std::vector<const ast::Expr*>* orderedExprs) {
    // fix #4967 (§5.1 "Nullable conversions", v0.307): the `S? → T?` lifted
    // widening at a CALL ARGUMENT. §5.1: "The two rules compose through at
    // most one `?` on each side", so an `int?` argument binds a `long?`
    // formal (and `float?` a `double?` one). The argument is already the
    // tagged `{i8, S}` struct, so neither the exact-payload wrap nor the
    // #4922 widen-then-wrap applied, and it reached the call as `{i8, i32}`
    // against `{i8, i64}` — IR verification failure. Rebuild the struct with
    // the payload WIDENED and the hasValue tag kept, as lowerLocalDecl does
    // (#840). `coerceNumericToType` is §5.1's widening direction only, so no
    // narrowing pair is produced; nullptr for any shape not covered (a
    // non-`{i8, numeric}` source or formal, or a pair §5.1 does not widen).
    // A lambda, not a member, to keep LlvmBackend_impl.hpp (160 includers)
    // untouched; buildOrderedArgs below carries the same body — keep the two
    // in step.
    auto widenNullableArgPayload = [this](llvm::Value* v,
                                          llvm::StructType* expectedSt,
                                          const ast::Expr* srcExpr)
        -> llvm::Value* {
        auto* vSt = v ? llvm::dyn_cast<llvm::StructType>(v->getType())
                      : nullptr;
        auto numeric = [](llvm::Type* t) {
            return (t->isIntegerTy() && !t->isIntegerTy(1)) ||
                   t->isFloatingPointTy();
        };
        if (!vSt || !expectedSt || vSt == expectedSt ||
            vSt->getNumElements() != 2 ||
            vSt->getElementType(0) != nullablePrimHasValTy() ||
            expectedSt->getNumElements() != 2 ||
            expectedSt->getElementType(0) != nullablePrimHasValTy() ||
            !numeric(vSt->getElementType(1)) ||
            !numeric(expectedSt->getElementType(1)))
            return nullptr;
        llvm::Type* dstPay = expectedSt->getElementType(1);
        llvm::Value* conv = coerceNumericToType(
            builder_.CreateExtractValue(v, {1u}, "arg.lift.in"), dstPay,
            srcExpr && isUnsignedTargetExpr(*srcExpr));
        if (conv->getType() != dstPay) return nullptr;
        llvm::Value* res = builder_.CreateInsertValue(
            llvm::UndefValue::get(expectedSt),
            builder_.CreateExtractValue(v, {0u}, "arg.lift.has"), {0u});
        return builder_.CreateInsertValue(res, conv, {1u});
    };
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
                // fix #4665 (§17.11 + §4.7, v0.307): asked through
                // `unitStorageTy`, the ONE spelling of "the storage form of a
                // lowered type". A UNIT-typed parameter with an omitted
                // `default(())` argument lowered its own `: ()` annotation to
                // LLVM `void`, and `Constant::getNullValue(void)` is an
                // `llvm_unreachable` this release libLLVM compiles to `ud2` —
                // `bezelc` died of SIGILL with no diagnostic (measured rc=132
                // on 572f5fc5 for `Take(p: () := default(()))` called as
                // `Take()`). Unlike the value-position `DefaultExpr` arm, this
                // site owes a REAL operand: `lowerParamType` substitutes the
                // §4.7 placeholder byte in the callee's signature, so the
                // caller must pass one byte to keep the call's types matching
                // — the same `i8 0` the explicit-argument rescue in
                // `buildOrderedArgs` substitutes for a null `v`.
                ordered[j] = llvm::Constant::getNullValue(
                    unitStorageTy(lowerType(params[j].type.get())));
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
                            } else if (llvm::Value* lifted =
                                           widenNullableArgPayload(
                                               ordered[j], expectedSt, dv)) {
                                // fix #4967 (§5.1 `S? → T?`, v0.307): the
                                // omitted-default twin — `b: long? := a` with
                                // an earlier `a: int?` formal (§22.5).
                                ordered[j] = lifted;
                            } else if (ordered[j]->getType()->isIntegerTy() ||
                                       ordered[j]->getType()
                                           ->isFloatingPointTy()) {
                                // fix #4922 (§5.1 `S → T → T?`, v0.307): the
                                // omitted-default twin of the explicit-arg
                                // widen-then-wrap in buildOrderedArgs —
                                // `c: long? := 7` lowers its default at i32.
                                ordered[j] = coerceReturnToNullableValue(
                                    ordered[j], formalTy,
                                    isUnsignedTargetExpr(*dv),
                                    /*sourceIsNullableRef=*/false, dv);
                            }
                        }
                    }
                }
            }
            paramSlots_ = std::move(savedParams);
            if (!ordered[j]) return false;
            // A defaulted parameter has no call-site argument expression;
            // record the default's own expression so its signedness is honored.
            if (orderedExprs) (*orderedExprs)[j] = dv;
        }
    }
    return true;
}


bool Emitter::buildOrderedArgs(const std::vector<ast::Param>& params,
                      const std::vector<ast::CallArgument>& args,
                      std::vector<llvm::Value*>& out,
                      std::vector<const ast::Expr*>* outExprs,
                      bool nullTransferredOwned,
                      std::vector<AccessorRefArg>* accessorRefArgs) {
    // fix #4967 (§5.1 `S? → T?`, v0.307): the same lifted widening as the
    // identically-named lambda in fillOmittedParamDefaults above — see there.
    auto widenNullableArgPayload = [this](llvm::Value* v,
                                          llvm::StructType* expectedSt,
                                          const ast::Expr* srcExpr)
        -> llvm::Value* {
        auto* vSt = v ? llvm::dyn_cast<llvm::StructType>(v->getType())
                      : nullptr;
        auto numeric = [](llvm::Type* t) {
            return (t->isIntegerTy() && !t->isIntegerTy(1)) ||
                   t->isFloatingPointTy();
        };
        if (!vSt || !expectedSt || vSt == expectedSt ||
            vSt->getNumElements() != 2 ||
            vSt->getElementType(0) != nullablePrimHasValTy() ||
            expectedSt->getNumElements() != 2 ||
            expectedSt->getElementType(0) != nullablePrimHasValTy() ||
            !numeric(vSt->getElementType(1)) ||
            !numeric(expectedSt->getElementType(1)))
            return nullptr;
        llvm::Type* dstPay = expectedSt->getElementType(1);
        llvm::Value* conv = coerceNumericToType(
            builder_.CreateExtractValue(v, {1u}, "arg.lift.in"), dstPay,
            srcExpr && isUnsignedTargetExpr(*srcExpr));
        if (conv->getType() != dstPay) return nullptr;
        llvm::Value* res = builder_.CreateInsertValue(
            llvm::UndefValue::get(expectedSt),
            builder_.CreateExtractValue(v, {0u}, "arg.lift.has"), {0u});
        return builder_.CreateInsertValue(res, conv, {1u});
    };
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
                // fix #3142 (§2.8.3 rule 4, v0.252): the kind comes off the
                // TypeSymbol. Through `->decl` it was null for every imported
                // type, so an `out owned` DISCARD of an imported class was
                // neither nulled nor registered in discardOwnedOutSlots_ — the
                // slot held a produced value nobody disposed. LEAK, cross-module
                // only, no diagnostic.
                {
                    const ast::TypeDeclKind tk = pt->declaration->kind;
                    if (tk == ast::TypeDeclKind::Class ||
                        tk == ast::TypeDeclKind::Interface) {
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
        //
        // fix #4514 (§15.1 / §2.8.3 rule 3, v0.306): INITIALIZED. This was a
        // bare `llvm::Value* v;`, and not every arm below assigns it — the
        // §15.4 inline-array arm assigns only when `lowerTypeRef` of the
        // formal yields an `llvm::ArrayType`, and falls straight through when
        // it yields the i32 placeholder instead (an inline-array formal whose
        // extent did not fold — see `lowerTypeRef`'s `arraySizeValue < 0`
        // fallback). Every subsequent read of `v` in this loop then touched an
        // indeterminate value: MEASURED as `v = 0xffffffff` and a SIGSEGV at
        // `v->getType()` about 200 lines down, on the issue's repro (a
        // `const`-named extent `[MAX]int` on an IMPORTED GENERIC constructor's
        // parameter, whose importer-side reconstruction left the extent
        // unfolded). A null here routes that same shape into the `#1386`
        // internal-error diagnostic below — a clean compile-time failure with
        // a real location — instead of a crash. This is the general guard: any
        // future arm that declines without assigning now fails loudly rather
        // than reading garbage. // per §15.1
        llvm::Value* v = nullptr;
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
            //
            // The #4805 fix-forward places the clean §22.3 ref-mismatch
            // diagnostic block (below) AFTER this fallback so an indexer-slot
            // or property-slot `ref` argument still goes through copy-in /
            // copy-out rather than being rejected with the §22.3 message.
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
            // fix #4805 (§22.3, §9.3, v0.307): only AFTER the
            // `emitAccessorRefArgCopyIn` indexer/property fallback above has
            // had a chance to recover do we conclude this argument is a true
            // non-lvalue (e.g. `new Rec(6)` where `Rec` has
            // `constructor(ref x: int)`). `lvalueAddr` does NOT itself handle
            // indexer/property slots — only `emitAccessorRefArgCopyIn` does
            // (see #1302 above). The block fires only when BOTH `lvalueAddr`
            // returned null AND the accessor fallback did not assign `v`,
            // emitting the clean §22.3 diagnostic instead of falling through
            // to the generic #1386 ICE.
            if (!v) {
                // `currentCallee_` is a Member* (MethodDecl/ConstructorDecl)
                // for method calls but may be null or non-Member for
                // `new Record(...)` — resolve the name via `initialSymbol`
                // which is always a NamedSymbol for any callable.
                std::string pname = params[idx].name.empty()
                                        ? "(unnamed)"
                                        : std::string(params[idx].name);
                std::string calleeName = "(unknown)";
                if (currentCallee_) {
                    if (auto* md = dynamic_cast<const ast::MethodDecl*>(currentCallee_))
                        calleeName = std::string(md->name);
                    else if (auto* cd = dynamic_cast<const ast::ConstructorDecl*>(currentCallee_))
                        ;  // ConstructorDecl has no name field; use "(unknown)"
                    else if (const ast::IdentifierExpr* id =
                                 dynamic_cast<const ast::IdentifierExpr*>(currentCallee_))
                        calleeName = std::string(id->name);
                }
                std::string modifierStr(params[idx].modifier);
                ctx_.diagnostics().report(
                    Severity::Error, a.value->span.begin,
                    "call to `" + calleeName +
                        "` has a call-site `ref`/`out` mismatch: parameter `" +
                        pname + "` is `" + modifierStr +
                        "` — pass the argument with `ref` at the call site "
                        "(§22.3 — the caller writes `ref`/`out` at the call site, "
                        "and only for a `ref`/`out` parameter)");
                return false;
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
            // fix #4813 (§14.1 / §14.5 / §15.4, v0.307): the literal is built
            // at the FORMAL's type, and a generic callee's own type parameter
            // in it (`xs: Array<T>` of `C<T>`) belongs to the callee. The
            // active maps are the caller's, so lowering it here read an unbound
            // placeholder (i32 for any element type), or the caller's same-named
            // `T`, while the callee body is specialized at its real `T`:
            // `C<int>([11, 22])` inside `B<T>` at T = string built 8-byte
            // elements the body read as 4-byte ones (ASan stack-buffer-overflow
            // in bz_array_add; `11,0`). Resolve it through the callee's own
            // bindings, computed from the same call by lowerCallDispatch.
            if (pt && calleeFormalSubstParams_ == &params &&
                !calleeFormalSemaSubst_.empty())
                pt = substituteTypeRefLocal(pt, calleeFormalSemaSubst_);
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
                } else {
                    // fix #4514 (§15.1 / §2.8.3 rule 3, v0.306): sema says the
                    // formal IS an inline array, but `lowerTypeRef` answered
                    // something other than an `llvm::ArrayType` — which it does
                    // for exactly one shape, an extent that is neither an AST
                    // literal nor a folded `arraySizeValue >= 0`, where it
                    // returns the bare `i32` placeholder ("what keeps an
                    // unfoldable size failing loudly", LlvmBackend_Layout.cpp).
                    // Say so HERE rather than letting the null `v` fall through
                    // to the generic `#1386` "lowerExpr returned null" line: the
                    // argument expression is fine, the PARAMETER's type is what
                    // could not be lowered, and naming the extent is what points
                    // a reader at the real defect (cross-module, this is a
                    // §2.8.3 rule 3 reconstruction gap — see #4514's other
                    // half). Returning false declines the call cleanly.
                    ctx_.diagnostics().report(
                        Severity::Error, a.value->span.begin,
                        "the inline-array parameter `" +
                            std::string(params[idx].name) +
                            "` has an extent that is not a compile-time "
                            "constant at this call site, so the argument "
                            "cannot be laid out (§15.1); for an IMPORTED "
                            "declaration this means the dependency's metadata "
                            "did not carry the folded extent (§2.8.3 rule 3, "
                            "#4514)");
                    return false;
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
            // fix #3778 (§27.5 / §28.9, v0.286): a lambda LITERAL passed as
            // a CALL ARGUMENT is a TEMPORARY — its closure is released at end
            // of statement, before any later read of a captured local. Mark
            // the inner `lowerLambda` so its first-capture content-release
            // gate widens (`(declaredShallower || inArgumentLambda_)`) and the
            // release moves to the captured local's own scope, where it
            // actually lines up. We only set the flag for the lambda-literal
            // shape (not for `IdentifierExpr` arguments that name a bound
            // closure — those are BORROWS, the callee consumes them, and the
            // existing bookkeeping already balances them).
            const bool savedInArg = inArgumentLambda_;
            if (a.value &&
                dynamic_cast<const ast::LambdaExpr*>(a.value.get()))
                inArgumentLambda_ = true;
            v = lowerExpr(*a.value);
            inArgumentLambda_ = savedInArg;
            expectedFunctionType_ = savedExpected;
        }
        // fix #3296 / §17.12 (v0.266): a `throw` expression used as an
        // argument diverges (CG40 — `lowerExpr` set `diverged_ = true` and
        // branched the current BB to `unwindBB`). The loop body below
        // emits FFI-buffer reclaims (`CreateStore` — see the #2809 block),
        // nullable wrapping (`CreateInsertValue` / `nullableValueNull` /
        // `wrapNullableValue`), §8.6 boxing (`CreateAlloc` / `CreateStore`),
        // IEnumerable wrapping (`CreateStore` / `disposeStack_.push_back`),
        // `disposeStack_.push_back` for `unwindOnly` / `dynamicDispose`
        // registrations, `boxForTarget`, and `wrapAsIEnumerableForTarget` —
        // all of which would land in the now-terminated basic block and the
        // IR verifier would reject with "Basic Block ... does not have
        // terminator". Return false (call failure) on the divergent edge.
        // The post-loop `transferredOwnedArgSlots` CreateStore and the
        // `argTempSlots` discharge below are guarded by the same
        // `diverged_` check just before `return true` at the bottom of this
        // function. The caller (lowerCallDispatch at line 8310) returns
        // `nullptr` on this failure; the CallExpr caller in `lowerExpr`
        // (LlvmBackend_Exprs.cpp:1690) propagates `nullptr` upward, and the
        // throw has already branched to `unwindBB` so no further IR is
        // emitted on the dead BB by THIS call. Sibling fix at the
        // extern-call argument loop (LlvmBackend_Calls2.cpp:4046, also for
        // #3296) and at `tryLowerResultCombinatorCall`
        // (LlvmBackend_Calls.cpp:6818). Scoped narrowly to the divergent-
        // argument case; the happy path is untouched.
        if (diverged_) return false;
        // fix #3105 (§4.7 / §8.5 / §21.9, v0.255): broaden #3079's Console
        // guard to the broader call family. `lowerExpr` for a unit-returning
        // CallExpr (`ReturnsUnit()` where `ReturnsUnit(): ()`) returns a
        // NON-NULL but VOID-typed `llvm::Value*` — the `CallInst` IS a
        // `Value`, the instruction is just type-void. The pre-fix #2915
        // guard below was structured around the "lowerExpr returns null for
        // unit-typed args" premise (the comment two lines up still states
        // it), and that premise is false for a unit-RETURNING call: it
        // returns a void-typed value, not null. The result: the `if (!v)`
        // guard does NOT fire, the void value is pushed into `args`
        // directly, and the call is emitted with a void-typed argument
        // where the callee signature expects `i8` (the §4.7 unit placeholder
        // byte `lowerParamType` substitutes for `void`). The IR verifier
        // rejects the misaligned call with "Call parameter type does not
        // match function signature" — MEASURED on 50eb493 with the issue's
        // `TakeUnit(ReturnsUnit())` repro. The #3079 Console-only guard
        // caught the same shape for `Console.WriteLine(<unit>)`; the
        // broader call family did not.
        //
        // Detect the void-typed v BEFORE the existing rescue runs and
        // substitute the §4.7 unit placeholder byte when the formal
        // parameter is unit-typed — the same byte the existing null-v
        // rescue substitutes. The side effects of the original call (the
        // emitted `CreateCall`) are preserved: `lowerExpr` already emitted
        // them above; the substitution only replaces the `Value*` the
        // downstream code pushes into `args`. The nullable-unit branch
        // below mirrors the existing `unitValueForTarget` path for the
        // same formal shape.
        if (v && v->getType()->isVoidTy() && params[idx].type) {
            sema::TypeRef* formal0 = ctx_.resolvedType(*params[idx].type);
            // fix #3487: holder hoisted out of the inner `if` for the same
            // reason as the `!v` rescue below — see the long comment there.
            // This arm's escape is not reachable from any program found so
            // far (every unit-valued argument measured lowers to a NULL `v`,
            // taking the `!v` rescue, not a void-typed one), but the shape is
            // identical and the two arms are 40 lines apart; leaving one
            // spelled the defective way is how the next reader copies it.
            sema::TypeRef concrHolder;
            if (formal0) {
                if (formal0->kind == sema::TypeRef::Kind::Named &&
                    !formal0->declaration && formal0->nullable) {
                    if (nullableTypeParamConcrete(formal0, concrHolder))
                        formal0 = &concrHolder;
                } else {
                    formal0 = const_cast<sema::TypeRef*>(
                        resolveTypeParamRef(formal0));
                }
            }
            if (formal0 && formal0->kind == sema::TypeRef::Kind::Unit &&
                !formal0->nullable) {
                v = llvm::ConstantInt::get(unitPlaceholderTy(), 0);
            } else if (formal0 && isNullableUnitTy(formal0)) {
                v = nullableUnitPresent(formal0);
            }
        }
        // fix #2915 (§4.7 / §8.6 "Boxing of the unit value", v0.240): a
        // unit-typed argument has NO lowered value — `()` lowers to LLVM
        // `void`, so lowerExpr returns null for it — and at an `Object`/
        // interface parameter §4.7 requires a boxed `()` rather than nothing.
        // Routed through the SAME boxForTarget the local-decl, assignment and
        // return positions use, so all four agree by construction. A no-op for
        // every other null-`v` shape and for a non-Object parameter, so the
        // #1386 ICE below still fires for the cases it was written for.
        if (!v && params[idx].type) {
            sema::TypeRef* formal0 = ctx_.resolvedType(*params[idx].type);
            // fix #3137 / §4.7: a generic parameter `T` monomorphized at the
            // call site to `()` or `()?` (per §14.2's `where T: value` admitted-
            // substitutions row) arrives here as a bare `Named T` — `formal0`'s
            // `kind` is `Named`, NOT `Unit`. Resolve through the active
            // monomorphization (the #2901 mechanism `boxForTarget` also uses
            // on its target side, ~L3042) so the `Kind::Unit` /
            // `isNullableUnitTy` tests below see the concrete unit type and the
            // rescue fires. The pre-pass that installs the substitution for a
            // unit-substituted explicit type arg (see LlvmBackend_Calls2.cpp
            // ~L7934) is itself targeted to unit-shaped substitutions only —
            // it stays a no-op for any other type, so the existing
            // (#885/#886) `GetVal<int>(null, 42)` path that depends on the
            // `T?` formal staying bare until `applyMethodGenericSubst` runs
            // is untouched.
            // fix #3487: `concrHolder` is declared HERE, in the scope that
            // outlives every dereference of `formal0` below, not inside the
            // inner `if`. `nullableTypeParamConcrete` fills it by value and
            // `formal0` is made to point AT it, so a holder scoped to the
            // inner `if` is destroyed at that block's closing brace while
            // `formal0` — declared one scope out — keeps pointing at the dead
            // slot. `unitValueForTarget`/`isNullableUnitTy` then read it:
            // MEASURED as `stack-use-after-scope` by a `bezelc` built with
            // `-fsanitize=address -fsanitize-address-use-after-scope`, on
            // `C.Take<()>(C.Nothing())` where `Take<T>(x: T?) where T: value`
            // (§4.7 / §14.2, v0.283). Without ASan the read happened to land on
            // unclobbered stack and the program printed the right answers,
            // which is why it went unnoticed. `Emitter::unitValueForCtorParam`
            // (LlvmBackend_Layout.cpp) is the local precedent for keeping the
            // holder alive across the call.
            sema::TypeRef concrHolder;
            if (formal0) {
                if (formal0->kind == sema::TypeRef::Kind::Named &&
                    !formal0->declaration && formal0->nullable) {
                    // `T?` with an active `T → <concrete>` substitution:
                    // preserve nullability (resolveTypeParamRef strips it).
                    if (nullableTypeParamConcrete(formal0, concrHolder))
                        formal0 = &concrHolder;
                } else {
                    formal0 = const_cast<sema::TypeRef*>(
                        resolveTypeParamRef(formal0));
                }
            }
            // fix #3095 (§4.7, v0.252): the two OTHER unit-source targets a
            // call argument can have, both of which demand a real operand
            // because the callee's signature has a slot for them.
            //   * `()?` — the present optional `{i8 1, i8 0}`
            //     (unitValueForTarget; §4.7's "two values, `null` and `()`").
            //   * `()` itself — the inert placeholder byte. `lowerParamType`
            //     substitutes `unitPlaceholderTy()` for the `void` a unit
            //     parameter would otherwise lower to (void is not a valid
            //     parameter/alloca type — it SIGILL'd `bezelc` in
            //     bindParameters), so the caller owes one byte here to keep
            //     the call's arity and types matching the callee.
            llvm::Value* uv = unitValueForTarget(v, formal0, a.value.get());
            if (uv != v)
                v = uv;
            else if (formal0 && formal0->kind == sema::TypeRef::Kind::Unit &&
                     !formal0->nullable)
                v = llvm::ConstantInt::get(unitPlaceholderTy(), 0);
            else
                v = boxForTarget(v, formal0, a.value.get());
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
                    } else if (llvm::Value* lifted = widenNullableArgPayload(
                                   v, expectedSt, a.value.get())) {
                        // fix #4967 (§5.1 "Nullable conversions", v0.307):
                        // `S?` → `T?` — an `int?` argument at a `long?` formal
                        // arrives as `{i8, i32}`; see widenNullableArgPayload.
                        v = lifted;
                    } else if (v->getType()->isIntegerTy() ||
                               v->getType()->isFloatingPointTy()) {
                        // fix #4922 (§5.1 "Nullable conversions", v0.307):
                        // `S` converts to `T?` via `S → T → T?`, so an `int`
                        // argument at a `long?` formal must first widen to the
                        // payload and THEN wrap. Only the exact-payload case
                        // wrapped here, so the bare value escaped: the call
                        // site's per-arg `coerceExprValueToSlot` widened it to
                        // i64 but never wraps (`call @F(i64 5)` against
                        // `{i8, i64}` — IR verify failure), and a `new` took
                        // the inline-ctor path on the type mismatch, bound the
                        // formal in a scalar slot, and `a ?? -1` then ran
                        // `extractvalue` on an i64 — a bezelc SEGFAULT.
                        // `coerceReturnToNullableValue` is the existing
                        // widen-then-wrap for exactly this composition (#843,
                        // sema-gated narrowing per #2925), as #4904 uses at a
                        // ctor-chain argument; it leaves `v` unchanged for a
                        // pair §5.1 does not cover.
                        v = coerceReturnToNullableValue(
                            v, formalTy,
                            a.value ? isUnsignedTargetExpr(*a.value) : false,
                            /*sourceIsNullableRef=*/false, a.value.get());
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
                llvm::Value* carrier = v;
                v = unwrapNarrowedNullable(v, lowerTypeRef(formalTy));
                // fix #4899 (§14.1 / §7 / §7.10, v0.307): a BARE type-parameter
                // formal (`v: T`) lowers here to the i32 placeholder, because the
                // callee's substitution is not installed until after argument
                // lowering — so this unwrap also fires when `T := int?`, where
                // the carrier's tag is the value. Record it so the call site can
                // restore the carrier once `T` is known
                // (restoreBareTParamNullableCarriers).
                if (v != carrier && isBareTypeParamRef(formalTy))
                    bareTParamNarrowUnwraps_.push_back({idx, v, carrier});
            }
            // §8.6: a value-typed argument bound to an Object/interface
            // parameter is boxed, so a later `is`/`as` or virtual dispatch on
            // that parameter (which lowers to a descriptor walk through the
            // object's first word) sees a real box rather than the raw value
            // bytes. boxForTarget is a no-op for reference arguments (#399).
            //
            // fix #2952 (§8.6 / §28.9 / §28.3, v0.240): the freshly minted box
            // is a §28.3 unbound owned temporary whose lifetime ends at the
            // call return — bz_box_release it then, but ONLY when the callee's
            // body provably does not retain the parameter (§28.3 forbids a
            // borrowed parameter from being stored into a longer-lived
            // location, but the compiler's escape analysis conservatively
            // leaks rather than guessing). The save/clear on lastBoxMint_
            // isolates this argument from any later boxForTarget on a sibling
            // argument (or the per-boxable-type rescue on the SAME argument's
            // own tail — see #2915). The callee Decl flows through
            // currentCallee_ (set by the call site BEFORE buildOrderedArgs).
            llvm::Value* savedBoxMint = lastBoxMint_;
            lastBoxMint_ = nullptr;
            v = boxForTarget(v, formalTy, a.value.get());
            // fix #2952: if THIS argument just minted a fresh box (lastBoxMint_
            // is now non-null and IS `v`) AND the callee's parameter is
            // provably non-escaping (analyzed by isCalleeParamNonEscaping),
            // queue it for release at the post-call site. A null `v` after
            // boxForTarget means no box was minted (reference source or null
            // unit-source rescue), so nothing to release.
            if (v && lastBoxMint_ && v == lastBoxMint_ &&
                currentCallee_ != nullptr) {
                if (isCalleeParamNonEscaping(currentCallee_, idx))
                    pendingArgBoxReleases_.push_back(v);
            }
            lastBoxMint_ = savedBoxMint;
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
                // fix #3695 ARGUMENT-face, NARROWED by fix-forward v3 (this
                // commit) after d63b0fe's `disposeStack_.front()` was measured
                // to leak 3 allocs × N iterations under a while/for-loop body
                // (the borrowed `Dictionary<int, int?>` argument in
                // `dict_snapshot_value_disposer_3568.borrowed_…_is_not_double_
                // freed_28_3` — present=200 × 3 allocs/iter = 600 allocs
                // leaked, 36 616 B; ASan: 199 wraps × 32 B + 199 array-data
                // × 96 B + 199 array-header × 56 B). The forward fix is to
                // re-register on the CURRENT inner scope (`back()`) — the
                // scope #3519 narrow (commit 7b1cabc, lifted to r18-c7
                // alongside this) lifts non-`accSlot` per-part entries to
                // the parent before `pop_back` on the string-interpolation
                // part scope (`LlvmBackend_Exprs.cpp` ~L379–L401), so the
                // string-interp case this commit originally fixed is now
                // handled by #3519. `back()` is therefore the right scope
                // for every call site: the function-body scope when the
                // call is at the top of the function (fires at function
                // end, exactly once, same as #2298/#2299/#3354 do); the
                // while/for-loop body scope when the call is inside a loop
                // (fires at end of every iteration — the per-iteration
                // cleanup that 200 iterations of a borrowed Dictionary
                // argument require, and that `front()` was bypassing).
                // The same fix restored `sortedset_ienumerable_getenumerator_
                // 3695` row D (`Dictionary<int, int>` passed as
                // `IEnumerable<DictionaryEntry<int, int>>` argument inside
                // a `Console.WriteLine($"…{Sum(d)}…")`) to a green PASS,
                // because #3519's lift moves the wrap entry to the
                // enclosing statement's scope, which fires emitDisposals
                // on the normal exit path. (#29.2 / stdlib §4.18 / §4.1 /
                // §28.3, v0.298.)
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
            //
            // fix #3189 (family M2 of umbrella #3152): both-sources marker read.
            // `ms->isOperator` is the both-sources spelling of the old
            // `dynamic_cast<ast::OperatorDecl*>(ms->decl) != nullptr`, so the
            // gate stays exactly as narrow. The CALLEE here is always LOCAL
            // (this runs inside buildOrderedArgs, reached only with a
            // `md->params` / `cd->params` list), but the ARGUMENT may be an
            // imported producer — `f(Lib.Make())`, `f(a + b)` — and that is the
            // case both arms answered `false` for.
            if (auto* sym = ctx_.exprSymbol(*be))
                if (auto* ms = dynamic_cast<sema::MethodSymbol*>(sym))
                    if (ms->isOperator && sema::methodReturnsOwned(*ms, ctx_))
                        isOwnedTempArg = true;
        } else if (auto* ce = dynamic_cast<const ast::CallExpr*>(av)) {
            // #1019: a call to a user method declared `owned`-return produces a
            // fresh owned class temporary (`f(Make())` with `Make(): owned Cell`).
            if (common::calleeReturnsOwned(*ce, ctx_)) isOwnedTempArg = true;
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
    // fix #3777: the §22.5 omitted-default fill is now `fillOmittedParamDefaults`
    // — the same body, stated once, so that `emitChainCall` fills a chain
    // call's omitted tail by the identical rule rather than by a second copy.
    if (!fillOmittedParamDefaults(params, ordered, &orderedExprs)) return false;
    for (std::size_t j = 0; j < params.size(); ++j) {
        out.push_back(ordered[j]);
        if (outExprs) outExprs->push_back(orderedExprs[j]);
    }
    // fix #3296 / §17.12 (v0.266): a `throw` argument above set `diverged_`
    // and branched the current BB to `unwindBB`. The post-loop
    // `transferredOwnedArgSlots` CreateStore (the caller's transferred-owned
    // slot nulling) and the `argTempSlots` discharge `scope.erase` below
    // both operate on the dead BB and would either fail the IR verifier
    // (CreateStore into a terminated BB) or, for the scope.erase, run
    // bookkeeping on a scope the throw has already unwound through.
    // Returning false here propagates the same fail-with-error code path the
    // caller (`if (!buildOrderedArgs(...))`) already handles — the divergent
    // edge has no caller to discharge into anyway (the call never runs).
    if (diverged_) return false;
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


namespace {
// fix #2952 (§8.6 / §28.9 / §28.3, v0.240): walk a callee's METHOD/CTOR body to
// determine whether its parameter at a fixed index ESCAPES. The mirror image of
// BoxEscapeWalker (which tracks a LocalDecl in the CALLER's body) — here we
// track a ParameterSymbol in the CALLEE'S body, so the "use as call receiver"
// / "pass to a call" / "assign to a field" rules apply symmetrically:
//
//   * `v.GetHashCode()` / `v.Equals(x)` / `v.ToString()` — receiver read; not
//     an escape (callee borrows; Bezel ownership rules forbid retaining a
//     borrow into a longer-lived location, and `bz_box_retain` is only emitted
//     for lambda-capture / class-field-store).
//   * `v is T` / `v as T` (value target) — descriptor walk / payload copy;
//     not an escape.
//   * `v = null` / `v <> null` (with literal null) — read only; not escape.
//   * Anything else that hands `v` to another call, an assignment target, a
//     return, a lambda capture, an array index, a collection literal — ESCAPE.
//
// The tracked parameter's identity is the `ParameterSymbol::declType` AST
// pointer (`md->params[i].type.get()`), which is unique per parameter (each
// parameter carries its own type annotation). Imported callees have no
// ast::MethodDecl/ConstructorDecl in this module and so never reach the
// walker — they are conservatively leaked by the caller.
struct ParamEscapeWalker {
    const sema::SemaContext& ctx;
    const ast::Type* paramDeclType;
    bool escaped = false;

    bool isTarget(const ast::Expr* e) {
        auto* id = dynamic_cast<const ast::IdentifierExpr*>(e);
        if (!id) return false;
        auto* sym = ctx.exprSymbol(*id);
        if (!sym) return false;
        auto* ps = dynamic_cast<sema::ParameterSymbol*>(sym);
        return ps && ps->declType == paramDeclType;
    }

    void expr(const ast::Expr* e, bool safeRead) {
        if (!e || escaped) return;
        if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(e)) {
            if (isTarget(id) && !safeRead) escaped = true;
            return;
        }
        // `v is T` / `v as T` — read only.
        if (auto* t = dynamic_cast<const ast::TypeTestExpr*>(e)) {
            expr(t->operand.get(), /*safeRead=*/true);
            return;
        }
        // `v = null` / `v <> null` (literal null): read only.
        if (auto* b = dynamic_cast<const ast::BinaryExpr*>(e)) {
            if (b->op == "=" || b->op == "<>") {
                bool lNull = dynamic_cast<const ast::NullLiteralExpr*>(
                                 b->left.get()) != nullptr;
                bool rNull = dynamic_cast<const ast::NullLiteralExpr*>(
                                 b->right.get()) != nullptr;
                expr(b->left.get(), /*safeRead=*/rNull);
                expr(b->right.get(), /*safeRead=*/lNull);
                return;
            }
            expr(b->left.get(), /*safeRead=*/false);
            expr(b->right.get(), /*safeRead=*/false);
            return;
        }
        // Direct method call with `v` as receiver: receiver read; not escape.
        if (auto* c = dynamic_cast<const ast::CallExpr*>(e)) {
            if (auto* mem = dynamic_cast<const ast::MemberAccessExpr*>(
                    c->callee.get())) {
                if (isTarget(mem->target.get())) {
                    // Receiver is the tracked parameter — safe read.
                    for (auto& a : c->args) expr(a.value.get(), false);
                    return;
                }
                expr(mem->target.get(), false);
                for (auto& a : c->args) expr(a.value.get(), false);
                return;
            }
            if (auto* mem = dynamic_cast<const ast::NullCondMemberExpr*>(
                    c->callee.get())) {
                if (isTarget(mem->target.get())) {
                    for (auto& a : c->args) expr(a.value.get(), false);
                    return;
                }
                expr(mem->target.get(), false);
                for (auto& a : c->args) expr(a.value.get(), false);
                return;
            }
            // Generic call — any arg may escape through the callee.
            for (auto& a : c->args) expr(a.value.get(), false);
            return;
        }
        if (auto* c = dynamic_cast<const ast::NullCondCallExpr*>(e)) {
            if (auto* mem = dynamic_cast<const ast::MemberAccessExpr*>(
                    c->callee.get())) {
                if (isTarget(mem->target.get())) {
                    for (auto& a : c->args) expr(a.value.get(), false);
                    return;
                }
                expr(mem->target.get(), false);
                for (auto& a : c->args) expr(a.value.get(), false);
                return;
            }
            for (auto& a : c->args) expr(a.value.get(), false);
            return;
        }
        // `o!` forwards o onward — inherit the parent position.
        if (auto* p = dynamic_cast<const ast::PostfixBangExpr*>(e)) {
            expr(p->operand.get(), safeRead);
            return;
        }
        // Conditional: condition reads; branches forward.
        if (auto* c = dynamic_cast<const ast::ConditionalExpr*>(e)) {
            expr(c->condition.get(), true);
            expr(c->thenValue.get(), safeRead);
            expr(c->elseValue.get(), safeRead);
            return;
        }
        // Everything else conservatively escapes: lambda capture, collection
        // literal, with, new, throw, interpolation, unary, range.
        if (auto* lam = dynamic_cast<const ast::LambdaExpr*>(e)) {
            if (lam->exprBody) expr(lam->exprBody.get(), false);
            for (auto& s : lam->stmts) stmt(s.get());
            return;
        }
        if (auto* n = dynamic_cast<const ast::NewExpr*>(e)) {
            for (auto& a : n->args) expr(a.value.get(), false);
            return;
        }
        if (auto* w = dynamic_cast<const ast::WithExpr*>(e)) {
            expr(w->source.get(), false);
            for (auto& a : w->assigns) expr(a.value.get(), false);
            return;
        }
        if (auto* cl = dynamic_cast<const ast::CollectionLiteralExpr*>(e)) {
            for (auto& el : cl->elements) expr(el.get(), false);
            for (auto& k : cl->keys) expr(k.get(), false);
            return;
        }
        if (auto* ix = dynamic_cast<const ast::IndexExpr*>(e)) {
            expr(ix->target.get(), false);
            for (auto& ind : ix->indices) expr(ind.value.get(), false);
            return;
        }
        // Conservative: anything else treats the value as escaping.
        // (This covers UnaryExpr, ThrowExpr, InterpolatedStringExpr,
        // RangeExpr, CaseExpr — all of them can carry the value into a
        // longer-lived location or to a position we can't statically prove
        // safe.)
        // Recurse conservatively on a few key shapes.
        if (auto* u = dynamic_cast<const ast::UnaryExpr*>(e)) {
            expr(u->operand.get(), false);
            return;
        }
        if (auto* is = dynamic_cast<const ast::InterpolatedStringExpr*>(e)) {
            // Interpolation hole — render the value via ToString. Like the
            // direct method-call case, the callee borrows. Not escape.
            for (auto& part : is->parts) expr(part.expr.get(), true);
            return;
        }
        if (auto* ce = dynamic_cast<const ast::CaseExpr*>(e)) {
            expr(ce->scrutinee.get(), false);
            for (auto& arm : ce->arms) {
                expr(arm.guard.get(), false);
                expr(arm.exprBody.get(), false);
                for (auto& s : arm.stmts) stmt(s.get());
            }
            return;
        }
    }

    void stmt(const ast::Stmt* s) {
        if (!s || escaped) return;
        if (auto* ld = dynamic_cast<const ast::LocalDecl*>(s)) {
            // fix #3200 (§12.7 / §8.6, v0.254): mirror BoxEscapeWalker's
            // same-arm relaxation — `var b: [3]int := o;` inside
            // `if o is [3]int then` copies the box's PAYLOAD BYTES into the
            // new value-typed local (the box stays in `o`, the parameter this
            // walker tracks). ParamEscapeWalker is what arms the caller's
            // `pendingArgBoxReleases_` flush (§8.6 / §28.9 / §28.3): the
            // walker's `escaped` verdict here is the ONE switch that decides
            // whether the post-call `bz_box_release` runs. Without the
            // relaxation, a parameter narrowed to a value type then copied
            // into a value-typed local leaks 32 B / call (the per-iteration
            // box from `Run(src)`). The three preconditions match
            // `BoxEscapeWalker`'s arm: declared value type, bare-identifier
            // initializer, currently narrowed at this point. ParamEscapeWalker
            // does NOT see the parameter's declared type (its `isTarget`
            // keys on `paramDeclType`); the new local's initializer is
            // checked for the narrowing + the initializer's identifier's
            // declared type is fetched from sema directly.
            bool narrowedRead = false;
            if (ld->type && ld->initializer) {
                const sema::TypeRef* declTy = ctx.resolvedType(*ld->type);
                const sema::TypeRef* initTy =
                    ctx.exprType(*ld->initializer);
                auto isBoxingTarget = [](const sema::TypeRef* t) {
                    if (!t) return false;
                    using K = sema::TypeRef::Kind;
                    using P = sema::PrimitiveType;
                    if (t->kind == K::Primitive)
                        return t->primitive == P::Object;
                    if (t->kind == K::Named && t->declaration)
                        return t->declaration->kind ==
                               ast::TypeDeclKind::Interface;
                    return false;
                };
                if (declTy && initTy &&
                    !isBoxingTarget(declTy) &&
                    !isBoxingTarget(initTy)) {
                    if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(
                            ld->initializer.get())) {
                        if (isTarget(id) &&
                            id->name != "this" && id->name != "base") {
                            narrowedRead = true;
                        }
                    }
                }
            }
            expr(ld->initializer.get(), /*safeRead=*/narrowedRead);
            return;
        }
        if (auto* lc = dynamic_cast<const ast::LocalConstDecl*>(s)) {
            expr(lc->value.get(), false); return;
        }
        if (auto* r = dynamic_cast<const ast::ReturnStmt*>(s)) {
            expr(r->value.get(), false); return;
        }
        if (auto* es = dynamic_cast<const ast::ExpressionStmt*>(s)) {
            expr(es->expr.get(), false); return;
        }
        if (auto* ts = dynamic_cast<const ast::ThrowStmt*>(s)) {
            expr(ts->value.get(), false); return;
        }
        if (auto* as = dynamic_cast<const ast::AssignmentStmt*>(s)) {
            // Target reassigned to the tracked param — also a use; treat as
            // non-safe. We don't separately check the target here; only the
            // value is what can leak the box.
            expr(as->target.get(), false);
            expr(as->value.get(), false);
            return;
        }
        if (auto* is = dynamic_cast<const ast::IfStmt*>(s)) {
            expr(is->ifBranch.condition.get(), true);
            for (auto& b : is->ifBranch.body) stmt(b.get());
            for (auto& br : is->elifBranches) {
                expr(br.condition.get(), true);
                for (auto& b : br.body) stmt(b.get());
            }
            for (auto& b : is->elseBody) stmt(b.get());
            return;
        }
        if (auto* ws = dynamic_cast<const ast::WhileStmt*>(s)) {
            expr(ws->condition.get(), true);
            for (auto& b : ws->body) stmt(b.get());
            return;
        }
        if (auto* fs = dynamic_cast<const ast::ForStmt*>(s)) {
            expr(fs->iterable.get(), false);
            expr(fs->withExpr.get(), false);
            for (auto& b : fs->body) stmt(b.get());
            return;
        }
        if (auto* d = dynamic_cast<const ast::DoEndBlock*>(s)) {
            for (auto& b : d->body) stmt(b.get());
            return;
        }
        if (auto* ts = dynamic_cast<const ast::TryStmt*>(s)) {
            for (auto& b : ts->body) stmt(b.get());
            for (auto& c : ts->catches) {
                expr(c.guard.get(), true);
                stmt(c.body.get());
            }
            for (auto& b : ts->finallyBody) stmt(b.get());
            return;
        }
        if (auto* cs = dynamic_cast<const ast::CaseStmt*>(s)) {
            expr(cs->scrutinee.get(), false);
            for (auto& arm : cs->arms) {
                expr(arm.guard.get(), false);
                expr(arm.exprBody.get(), false);
                for (auto& s : arm.stmts) stmt(s.get());
            }
            return;
        }
    }
};
}  // namespace


// fix #2952 (§8.6 / §28.9 / §28.3, v0.240): is the callee's parameter at
// `paramIdx` provably NON-ESCAPING the callee body? Imported callees have no
// body in this module ⇒ false (leak — the safe direction per #2823/#2874).
// Locally-compiled MethodDecl / ConstructorDecl: walk the body with
// ParamEscapeWalker; the result is cached per callee Decl.
bool Emitter::isCalleeParamNonEscaping(const ast::Member* callee,
                                       size_t paramIdx) {
    if (!callee) return false;
    // Check the cache first.
    auto it = calleeParamNonEscape_.find(callee);
    if (it != calleeParamNonEscape_.end())
        return it->second.count(paramIdx) != 0;

    // Build the cache entry by walking every parameter (the walker is O(N) per
    // call, so amortising it across all Object/interface params of one callee
    // is cheaper than per-parameter queries). Parameters that aren't
    // Object/interface never box anyway and never reach the call-site release
    // check, but walking them is cheap and keeps the cache complete.
    std::unordered_set<size_t> nonEscape;
    ParamEscapeWalker w{ctx_, nullptr};
    auto walkParams = [&](const std::vector<ast::Param>& params,
                          const std::vector<std::unique_ptr<ast::Stmt>>* body,
                          const ast::Expr* exprBody) {
        for (size_t i = 0; i < params.size(); ++i) {
            if (!params[i].type) continue;
            const sema::TypeRef* pt = ctx_.resolvedType(*params[i].type);
            if (!pt || !isObjectOrInterfaceTarget(pt)) continue;
            ParamEscapeWalker w{ctx_, params[i].type.get()};
            if (body) {
                for (auto& s : *body) w.stmt(s.get());
            }
            if (exprBody) w.expr(exprBody, false);
            if (!w.escaped) nonEscape.insert(i);
        }
    };
    if (auto* md = dynamic_cast<const ast::MethodDecl*>(callee)) {
        if (md->bodyKind == ast::MethodBodyKind::Expression) {
            walkParams(md->params, nullptr, md->exprBody.get());
        } else if (md->bodyKind == ast::MethodBodyKind::Block) {
            walkParams(md->params, &md->stmts, nullptr);
        }
        // Abstract or empty body ⇒ no params escape (nothing to escape
        // through). Cache the empty set so subsequent queries are O(1).
    } else if (auto* cd = dynamic_cast<const ast::ConstructorDecl*>(callee)) {
        walkParams(cd->params, &cd->body, nullptr);
    } else {
        return false;
    }
    bool result = nonEscape.count(paramIdx) != 0;
    calleeParamNonEscape_[callee] = std::move(nonEscape);
    return result;
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
