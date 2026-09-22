#include "codegen/LlvmBackend_impl.hpp"

namespace bezel::codegen {
// ===========================================================================
// stdlib §5: deferred (lazy) LINQ operators.
//
// LINQ operators do no work until enumerated. Each operator lowers to a call
// into the runtime lazy-enumerator family (bz_lazy_*). Closures (predicates,
// selectors, key functions) are bridged to the type-erased runtime via small
// trampoline functions emitted here.
// ===========================================================================

// The byte size of an element of LLVM type `ty` as stored in a type-erased
// array slot (mirrors collectionElemSize's element-size choices).
// per §9 (bug D): pass the DataLayout so that a record StructType returns its
// actual allocation size instead of the hardcoded 8.
static int64_t lazyElemSize(llvm::Type* ty, const llvm::DataLayout& dl) {
    if (!ty) return 8;
    if (ty->isIntegerTy(32) || ty->isFloatTy()) return 4;
    if (ty->isIntegerTy(1) || ty->isIntegerTy(8)) return 1;
    if (ty->isIntegerTy(16)) return 2;
    if (ty->isStructTy()) return (int64_t)dl.getTypeAllocSize(ty);
    return 8;  // i64 / double / pointer (string, class)
}

bool Emitter::isLazyEnumerableType(const sema::TypeRef* rt) {
    // The lazy LINQ sentinel: a Named TypeRef with no declaration and no
    // astNode, carrying the element type as genericArgs[0]. Produced only by
    // the LINQ-operator typing in ExprTypecheck_Calls.cpp.
    //
    // §2.5 / §29.3 (issue #495): a TypeRef reconstructed from module metadata
    // for a builtin interface (IEnumerable<T>, IAsyncEnumerable<T>, etc.) has
    // the SAME shape — Named, no declaration, no astNode — but carries the
    // type name in importedName (set by ModuleLoader).  Exclude those from the
    // lazy-LINQ path so they fall through to lowerIEnumerableBuiltinFor /
    // lowerIAsyncEnumerableFor, which use the correct vtable protocol (Current
    // returns the element value directly, not a pointer to it).
    return rt && rt->kind == sema::TypeRef::Kind::Named && !rt->declaration &&
           !rt->astNode && !rt->genericArgs.empty() && rt->importedName.empty();
}

// issue #995 / stdlib §5 / §29: a VTABLE-BACKED `IEnumerable<T>` receiver — an
// interface-typed local/param/field (`var e: IEnumerable<int> := …`) or a
// yield-method result bound to such a type. Unlike the lazy-LINQ sentinel
// (isLazyEnumerableType: declaration-less, NO astNode, NO importedName), this
// type carries the interface name in an astNode (local annotation) or in
// importedName (cross-module). Its runtime value is the iterator-vtable object
// ([0]=td,[1]=Dispose,[2]=MoveNext,[3]=Current,[4]=GetEnumerator — §29.11),
// NOT a bz_lazy_t* or a bz_array_t*. LINQ terminals/operators and the lazy
// for-in were built only for the lazy sentinel, so such a receiver silently
// fell through to a wrong path and returned garbage (the #995 miscompile).
// Recognise it here so lowerToLazyEnumerator can materialize it into a concrete
// Array<T> (driving the vtable protocol once) and the rest of the lazy
// machinery composes uniformly. EXCLUDES the array/llist/dict/set/range/lazy
// cases (those carry a declaration or take a dedicated wrap path) and the
// richer list interfaces (IReadOnlyList/IList — array-backed, indexer-bearing,
// NOT vtable objects) and the polymorphic collection interfaces
// (IReadOnlyCollection/ICollection/IOwningCollection — diagnosed, #853).
bool Emitter::isVtableBackedIEnumerableType(const sema::TypeRef* rt) {
    if (!rt || rt->kind != sema::TypeRef::Kind::Named || rt->declaration)
        return false;
    if (rt->genericArgs.empty() || !rt->genericArgs[0]) return false;
    // Must carry a name source other than the lazy sentinel's empty shape.
    std::string_view n = builtinTypeName(rt);
    if (n.empty()) return false;
    // Only the pure iteration interfaces the for-in builtin path drives through
    // the iterator vtable (slot-1 Dispose, slot-4 GetEnumerator). These are
    // implemented uniformly by the yield-iterator / array-wrapper / user-class
    // -wrapper producers (§29.11), so the vtable read is always in-bounds.
    // fix #3419 (§29.2, v0.283): read the shared head classifier.
    return common::isSyncIterFormatEnumerableHead(n);
}

// stdlib §5 (#755): the ELEMENT TypeRef of an `IGrouping<TKey, T>` receiver —
// genericArgs[1]. `IGrouping<TKey, T> : IEnumerable<T>` (stdlib §5 line 3076),
// so the §5 LINQ surface resolves on it by inheritance; but unlike every other
// enumerable receiver the compiler recognises, its ELEMENT type is the SECOND
// generic argument (the first is the key). Every consumer that reads
// `recvTy->genericArgs[0]` for an element type would therefore read TKey.
//
// IGrouping is a compiler-known builtin (common/BuiltinRegistry.cpp) with no
// user declaration; its TypeRef reaches codegen in three shapes — a
// declaration-less astNode (inferred/annotated local), a builtin TypeSymbol
// whose `decl` is null, or an `importedName` set by ModuleLoader — matching
// the recognizers already used at the GroupBy sites in LlvmBackend_Calls.cpp
// and LlvmBackend_ElementPolicy.cpp.
const sema::TypeRef* Emitter::iGroupingElemTypeRef(const sema::TypeRef* rt) {
    if (!rt || rt->kind != sema::TypeRef::Kind::Named) return nullptr;
    std::string_view hn;
    if (rt->declaration) {
        if (rt->declaration->decl) return nullptr;  // a real user type
        hn = rt->declaration->name;
    } else if (rt->astNode && rt->astNode->name &&
               !rt->astNode->name->parts.empty()) {
        hn = rt->headName();
    } else if (!rt->importedName.empty()) {
        hn = rt->importedName;
    } else {
        return nullptr;
    }
    if (hn != "IGrouping") return nullptr;
    if (rt->genericArgs.size() < 2 || !rt->genericArgs[1]) return nullptr;
    return rt->genericArgs[1];
}

// stdlib §5 (#755) / §4.4: a synthesized `Array<T>` TypeRef over `elem`. An
// `IGrouping<TKey, T>`'s members live in a concrete `bz_array_t*` the grouping
// owns; restating the receiver TYPE as that element array lets the whole
// array-receiver lowering — every §5 terminal together with its spec-mandated
// faults (`Single()` → InvalidOperationException unless exactly one,
// `ElementAt(i)` → IndexOutOfRangeException, `First`/`Last` → empty fault) —
// apply verbatim, instead of a hand-rolled per-name duplicate that reproduced
// none of them (#2265). Construction mirrors builtinByteArrayTypeRef.
sema::TypeRef* Emitter::elementArrayTypeRef(const sema::TypeRef* elem) {
    static const ast::Type* kArrayAstNode = []() -> ast::Type* {
        auto* qn = new ast::QualifiedName;
        qn->parts.push_back("Array");
        auto* t = new ast::Type;
        t->kind = ast::TypeKind::Named;
        t->name.reset(qn);
        return t;
    }();
    auto* tr = ctx_.makeTypeRef();
    tr->kind = sema::TypeRef::Kind::Named;
    tr->declaration = nullptr;
    tr->astNode = kArrayAstNode;
    tr->genericArgs.push_back(const_cast<sema::TypeRef*>(elem));
    return tr;
}

// issue #343 / §8.3: trampoline matching the runtime's generic-dict hash fn
// pointer `uint64_t(const void* key)`. The runtime stores raw record bytes; we
// reinterpret the byte pointer as a record-struct pointer, load the value, and
// run the synthesized GetHashCode (emitRecordHashCode, §9.1/§9.2). The i32 hash
// is zero-extended to i64 (the runtime keeps the low 32 bits for bucketing).
llvm::Function* Emitter::buildRecordHashTrampoline(const sema::TypeSymbol* rec) {
    llvm::StructType* st = recordStruct(rec);
    auto* fty = llvm::FunctionType::get(i64(), {ptrTy()}, false);
    auto* fn = llvm::Function::Create(fty, llvm::Function::InternalLinkage,
                                      "bz$reckey$hash", module_.get());
    auto savedIP = builder_.saveIP();
    llvm::Function* savedFn = currentFn_;
    currentFn_ = fn;
    auto* entry = llvm::BasicBlock::Create(llvm_, "entry", fn);
    builder_.SetInsertPoint(entry);
    llvm::Value* rv = builder_.CreateLoad(st, fn->getArg(0), "key.rec");
    llvm::Value* h = emitRecordHashCode(rv, rec);          // i32
    builder_.CreateRet(builder_.CreateZExt(h, i64()));
    builder_.restoreIP(savedIP);
    currentFn_ = savedFn;
    return fn;
}

// issue #343 / §8.3: trampoline matching the runtime's generic-dict equality fn
// pointer `bool(const void* a, const void* b)`. Loads both record values and
// runs the synthesized structural equality (emitStructEquality, §9.1/§9.2),
// which compares string fields by content (#346) consistent with the hash.
llvm::Function* Emitter::buildRecordEqTrampoline(const sema::TypeSymbol* rec) {
    llvm::StructType* st = recordStruct(rec);
    auto* fty = llvm::FunctionType::get(i1(), {ptrTy(), ptrTy()}, false);
    auto* fn = llvm::Function::Create(fty, llvm::Function::InternalLinkage,
                                      "bz$reckey$eq", module_.get());
    auto savedIP = builder_.saveIP();
    llvm::Function* savedFn = currentFn_;
    currentFn_ = fn;
    auto* entry = llvm::BasicBlock::Create(llvm_, "entry", fn);
    builder_.SetInsertPoint(entry);
    llvm::Value* a = builder_.CreateLoad(st, fn->getArg(0), "eq.a");
    llvm::Value* b = builder_.CreateLoad(st, fn->getArg(1), "eq.b");
    builder_.CreateRet(emitStructEquality(a, b, st, rec));  // i1
    builder_.restoreIP(savedIP);
    currentFn_ = savedFn;
    return fn;
}

// §35.7 (v0.200, #2047): choice-key hash trampoline `uint64_t(const void*)`.
// The runtime stores the choice value's bytes; reinterpret the byte pointer as a
// pointer to the choice struct, load it, and run the §35.7 tag + active-payload
// hash (emitChoiceHashCode). Mirrors buildRecordHashTrampoline. The caller
// installs the key's instantiation subst so choiceStruct here is the specialized
// layout and emitChoiceHashCode folds each payload by its concrete type.
llvm::Function* Emitter::buildChoiceHashTrampoline(const sema::TypeSymbol* cho) {
    llvm::StructType* st = choiceStruct(cho);
    auto* fty = llvm::FunctionType::get(i64(), {ptrTy()}, false);
    auto* fn = llvm::Function::Create(fty, llvm::Function::InternalLinkage,
                                      "bz$chokey$hash", module_.get());
    auto savedIP = builder_.saveIP();
    llvm::Function* savedFn = currentFn_;
    currentFn_ = fn;
    auto* entry = llvm::BasicBlock::Create(llvm_, "entry", fn);
    builder_.SetInsertPoint(entry);
    llvm::Value* cv = builder_.CreateLoad(st, fn->getArg(0), "key.cho");
    llvm::Value* h = emitChoiceHashCode(cv, cho);          // i32
    builder_.CreateRet(builder_.CreateZExt(h, i64()));
    builder_.restoreIP(savedIP);
    currentFn_ = savedFn;
    return fn;
}

// §35.7 (v0.200, #2047): choice-key equality trampoline
// `bool(const void* a, const void* b)`. Loads both choice values and runs the
// §35.7 tag + active-payload equality (emitChoiceEquality — consistent with the
// hash, and comparing a string payload by content). Mirrors
// buildRecordEqTrampoline.
llvm::Function* Emitter::buildChoiceEqTrampoline(const sema::TypeSymbol* cho) {
    llvm::StructType* st = choiceStruct(cho);
    auto* fty = llvm::FunctionType::get(i1(), {ptrTy(), ptrTy()}, false);
    auto* fn = llvm::Function::Create(fty, llvm::Function::InternalLinkage,
                                      "bz$chokey$eq", module_.get());
    auto savedIP = builder_.saveIP();
    llvm::Function* savedFn = currentFn_;
    currentFn_ = fn;
    auto* entry = llvm::BasicBlock::Create(llvm_, "entry", fn);
    builder_.SetInsertPoint(entry);
    llvm::Value* a = builder_.CreateLoad(st, fn->getArg(0), "eq.a");
    llvm::Value* b = builder_.CreateLoad(st, fn->getArg(1), "eq.b");
    builder_.CreateRet(emitChoiceEquality(a, b, cho));  // i1
    builder_.restoreIP(savedIP);
    currentFn_ = savedFn;
    return fn;
}

// issues D3 / D8 / §4.8 / §4.10: decide whether a Dictionary key / Set element
// at generic-arg index `idx` needs the generic-key runtime backing, and if so
// return its resolved TypeRef and the per-key byte count.
//
//   Dictionary (isSet=false): a string key keeps the bz_dict_*_str fast path
//     (returns null). Every other hashable key — primitives, enums,
//     records (§4.8: "K must provide consistent Equals and GetHashCode") — uses
//     the generic path. The stored key bytes are the key's natural LLVM layout.
//   Set (isSet=true): the array backing cannot deduplicate (§4.10), so ALL
//     element types route through the generic hash table — including strings.
//
// `*byteSize` receives the stored key/element byte count (the LLVM alloc size
// of the key type: 8 for a string pointer, 4 for an int, sizeof(record), …).
// stdlib §4.10 (#351/#389): lower a dict-backed Set's set-algebra op. `other` is
// snapshotted into a malloc'd array of element-byte-blob pointers (the shape
// bz_dict_keys_gen produces), then folded into/against self by the runtime
// (bz_set_algebra_gen, which frees the snapshot). Two snapshot sources:
//   * a same-typed dict-backed Set `other` → bz_dict_keys_gen on its table (#370)
//   * a non-Set IEnumerable<T> `other` (e.g. an Array<T> source) → enumerate via
//     the lazy enumerator (#326) and collect each element via
//     bz_set_collect_enumerator.
// Either way the element type must match self's (same LLVM type + record-ness) so
// the snapshot bytes are layout-compatible with self's keys. Returns nullptr
// (caller defers) only on an element-type mismatch.
llvm::Value* Emitter::tryLowerSetAlgebra(std::string_view method,
                                         llvm::Value* selfTbl,
                                         const ast::Expr& otherExpr,
                                         const sema::TypeRef* selfElemRef,
                                         bool elemIsRecord) {
    if (!selfElemRef) return nullptr;
    auto* otherTy = ctx_.exprType(otherExpr);

    // Determine `other`'s element type and whether it is a same-typed dict-backed
    // Set (whose key table we can snapshot directly) or some other IEnumerable<T>.
    bool otherIsSet = otherTy && otherTy->astNode && otherTy->astNode->name &&
                      !otherTy->astNode->name->parts.empty() &&
                      !otherTy->declaration &&
                      (otherTy->headName() == "Set" ||
                       otherTy->headName() == "OwningSet") &&
                      genericBackingType(otherTy, 0, /*isSet=*/true, nullptr);

    const sema::TypeRef* otherElem =
        (otherTy && !otherTy->genericArgs.empty()) ? otherTy->genericArgs[0]
                                                    : nullptr;
    if (!otherElem) return nullptr;
    if (lowerTypeRef(otherElem) != lowerTypeRef(selfElemRef))
        return nullptr;  // element LLVM-type mismatch → defer
    if ((recordTypeOfRef(otherElem) != nullptr) != elemIsRecord)
        return nullptr;  // record-ness mismatch → defer

    // Build the snapshot (keys: void**, n: i64) of `other`'s elements.
    auto* keysOutSlot = entryAlloca(ptrTy());
    auto* cntSlot     = entryAlloca(i64());
    if (otherIsSet) {
        llvm::Value* otherTbl = lowerExpr(otherExpr);
        if (!otherTbl) return nullptr;
        auto* keysFn = declareC("bz_dict_keys_gen", llvm::Type::getVoidTy(llvm_),
                                {ptrTy(), ptrTy(), ptrTy()});
        builder_.CreateCall(keysFn, {otherTbl, keysOutSlot, cntSlot});
    } else {
        // Non-Set IEnumerable<T>: drain the lazy enumerator into the snapshot.
        // The enumerator's per-element byte stride (collectionElemSize) must
        // equal self's key_size, so the snapshot bytes are layout-compatible
        // with self's keys; they agree for primitives/enums/strings and for
        // records that fit the array stride. If they differ — a record wider
        // than the array's element stride, an independent Array<T>-storage
        // limitation — defer rather than read past the enumerator's buffer.
        uint64_t ksz = 0;
        genericBackingType(otherTy, 0, /*isSet=*/true, &ksz);  // self.key_size
        if ((int64_t)ksz != collectionElemSize(otherTy)) return nullptr;
        sema::TypeRef* enumElemRef = nullptr;
        llvm::Value* lazy = lowerToLazyEnumerator(otherExpr, enumElemRef);
        if (!lazy) return nullptr;
        auto* collectFn = declareC("bz_set_collect_enumerator",
            llvm::Type::getVoidTy(llvm_), {ptrTy(), i64(), ptrTy(), ptrTy()});
        builder_.CreateCall(collectFn,
            {lazy, builder_.getInt64((int64_t)ksz), keysOutSlot, cntSlot});
    }
    llvm::Value* keysArr = builder_.CreateLoad(ptrTy(), keysOutSlot);  // void**
    llvm::Value* cnt     = builder_.CreateLoad(i64(), cntSlot);

    // Map the method to the runtime op code and fold.
    int32_t op = method == "UnionWith"     ? 0
               : method == "ExceptWith"    ? 1
               : method == "IntersectWith" ? 2
               : method == "IsSubsetOf"    ? 3
               : /* IsSupersetOf */          4;
    bool isQuery = (op == 3 || op == 4);
    auto* foldFn = declareC("bz_set_algebra_gen", i32(),
                            {ptrTy(), ptrTy(), i64(), i32()});
    llvm::Value* r = builder_.CreateCall(foldFn,
        {selfTbl, keysArr, cnt, builder_.getInt32(op)}, "set.algebra");
    if (isQuery)
        return builder_.CreateTrunc(r, i1(), "set.algebra.r");
    return llvm::ConstantInt::get(i32(), 0);  // unit result for mutators
}

const sema::TypeRef* Emitter::genericBackingType(const sema::TypeRef* collTy,
                                                 unsigned idx, bool isSet,
                                                 uint64_t* byteSize) {
    if (!collTy || collTy->genericArgs.size() <= idx) return nullptr;
    const sema::TypeRef* ty = collTy->genericArgs[idx];
    if (!ty) return nullptr;
    // fix #567 / §14.1: inside a monomorphized generic body the key/element
    // genericArg is still the bare type parameter (`K` / `T`). Resolve it to the
    // concrete substituted type so eligibility + the hash/eq trampolines follow
    // the CONCRETE key/element's contract (§8.3), not the placeholder's string
    // fast path — which mismatches the caller's concrete dispatch (malformed IR /
    // segfault). No-op outside a generic body or for a concrete genericArg.
    ty = resolveTypeParamRef(ty);
    using K = sema::TypeRef::Kind;
    bool isString = ty->kind == K::Primitive &&
                    ty->primitive == sema::PrimitiveType::String && !ty->nullable;
    // Dictionary: string keys keep their dedicated fast path (bz_dict_*_str).
    if (!isSet && isString) return nullptr;
    // fix #567 / §14.1: a still-UNRESOLVED type parameter (a single-part Named
    // with no `declaration`, e.g. `K`/`T` with no active binding — the
    // UNSPECIALIZED template body emitted by emitFunctionBody) routes through the
    // generic-key hash path, NOT the string fast path. The template body is dead
    // (only monomorphized specializations are called), but it must still pass IR
    // verification; the placeholder lowers consistently to i32, so the i32-keyed
    // generic trampolines (store i32 / hash i32 / compare i32) produce valid IR,
    // whereas the string fast path would GEP the i32 key as a %bz.string pointer
    // (malformed IR). Every real call site sees the concrete substituted key.
    if (ty->kind == K::Named && !ty->declaration && ty->astNode &&
        ty->astNode->name && ty->astNode->name->parts.size() == 1) {
        if (byteSize) {
            llvm::Type* lt = lowerTypeRef(ty);  // i32 placeholder
            *byteSize = lt ? module_->getDataLayout().getTypeAllocSize(lt)
                           : (uint64_t)8;
        }
        return ty;
    }
    // Eligible generic-backed types: a record (existing path) or a non-nullable
    // primitive/enum scalar (D3) — plus string for sets (D8). Nullable keys and
    // other shapes are not wired here.
    bool eligible = false;
    if (recordTypeOfRef(ty)) eligible = true;
    else if (isString) eligible = true;  // set element only (dict returned above)
    else if (!ty->nullable && ty->kind == K::Primitive &&
             ty->primitive != sema::PrimitiveType::String &&
             ty->primitive != sema::PrimitiveType::Object)
        // fix #685 / §4.4 / §8.3: decimal is a 16-byte struct but has well-defined
        // value equality (bz_decimal_cmp) and hash (bz_decimal_hash — normalized so
        // 1.0m == 1.00m).  Allow decimal as a generic-backed key/set-element.
        eligible = true;  // int/long/bool/char/byte/short/uint/.../float/double/decimal
    else if (!ty->nullable && ty->kind == K::Named && ty->declaration &&
             ty->declaration->kind == ast::TypeDeclKind::Enum)
        eligible = true;  // enum key/element (lowers to i32; value-compared)
    else if (!ty->nullable && ty->kind == K::Named && ty->declaration &&
             ty->declaration->kind == ast::TypeDeclKind::Choice)
        // §35.7 (#2047): a value-like auto-hashable choice key/element
        // (sema rejects a non-value-like one). Stored as the choice struct's
        // bytes; hashed/compared by tag + active payload (build*ChoiceTrampoline).
        eligible = true;
    else if (!ty->nullable && hasObjectHeaderEquality(ty) &&
             (!isSet || (collTy && collTy->headName() == "SortedSet")))
        // fix #2773 (rows A3/A4) / §3.6 / §8.3 / stdlib §4.8: a DICTIONARY key
        // that carries the §8.3 object header — a class, an interface, or
        // `Object`. §3.6's hashable row admits "every type except (a) a
        // record that is neither auto-hashable nor declares user
        // equality, and (b) a choice that is not value-like
        // auto-hashable", and sema's isEqualityBearing admits a class
        // and an interface as a hash-container key verbatim ("class (reference
        // identity), interface, enum — all bear consistent Equals/
        // GetHashCode"), so the shape is legal and REACHABLE, not latent.
        //
        // Such a key has a perfectly good generic backing: the stored key bytes
        // are the reference itself (pointer-sized, held by BORROW — stdlib §4.8
        // "A class-typed key/element is held by borrow", and §4.9's
        // `Add(key: K, owned value: V)` marks only the VALUE owned, so there is
        // no key-disposal obligation to express), and its hash/equality are
        // §8.3's `Equals`/`GetHashCode` pair, which buildValueEqTrampoline /
        // buildValueHashTrampoline emit via the object vtable.
        //
        // Returning null here instead sent the key to the STRING fast path,
        // which GEPs the object as a `%bz.string`. Measured on
        // `new Dictionary<Box,int>().Add(b, 10)`: SEGV in
        // `bz_dict_contains_str` → `fnv1a`. The `GetEnumerator` sites
        // (LlvmBackend_Assign.cpp, LlvmBackend_Calls2.cpp) already classified
        // such a dictionary GENERIC-mode — they use a real TypeRef string test
        // — so BUILD mode and READ mode disagreed; converging on generic is
        // what makes the two agree.
        //
        // `isSet` is excluded deliberately for `Set<T>`/`OwningSet<T>`: a
        // class/interface SET element keeps its ARRAY-backed linear-scan
        // machinery (emitArrayBackedSetInsert, #1070), a separate and
        // working design.
        //
        // fix #2773 (B1) / stdlib §4.18 (v0.286): `SortedSet<T>` is the ONE
        // exception, named explicitly (`collTy->headName() == "SortedSet"`)
        // rather than lifting the `isSet` guard wholesale — `Set`/
        // `OwningSet` must NOT widen here (that would switch their whole
        // class-element representation from array- to dict-backed, a blast
        // radius no test in this fix measures). Unlike `Set`, `SortedSet`
        // has NO array-backed fallback at all — "SortedSet is hash-backed or
        // it is nothing" (`narrow #3428`'s own commit body) — so before this
        // line a class/interface `SortedSet<T>` could not be constructed at
        // all (#3428 made that a truthful `stdlib §4.18: ... not yet
        // implemented` rejection rather than the old #2974 degrade). What
        // changed since #3428: `buildObjectCompareToThunk` (`fix #3488`'s
        // lift of the #1195/#2768 `Array.Sort` thunks) now gives every
        // class/interface element with a dispatchable `CompareTo` a
        // `CompareTo`-through-the-vtable comparator, so #3428's objection —
        // "there is no such comparator in this compiler" — no longer holds
        // for an element that actually declares one. `lowerNew` narrows the
        // #3428 rejection to fire only when `buildObjectCompareToThunk`
        // returns null (§4.18's `where T: IComparable<T>` going unchecked for
        // a builtin generic is #3490's open question, unaffected by this fix).
        eligible = true;
    // fix #3740 (stdlib §4.18 / §14.2, v0.291): a SortedSet<T> RECORD element
    // that is NOT auto-hashable (`recordTypeOfRef` above already excluded it
    // — §3.6/§9.2: an explicit field declaration, not an auto-property, that
    // is not `equality skip`ped) but DOES declare a dispatchable
    // single-parameter `CompareTo` (§14.2's `IComparable<T>` shape) — the
    // value-type analogue of the class/interface exception immediately
    // above. Named explicitly to
    // `SortedSet` for the identical reason that one is: `Set<T>`/
    // `OwningSet<T>` must not widen (no test here measures that blast
    // radius), and unlike a plain hashable record (routed through
    // `recordTypeOfRef` and structural §9.1/§9.2 Equals/GetHashCode),
    // `SortedSet<T>`'s membership is `CompareTo`-equality, not structural
    // equality, per stdlib §4.18 ("Two elements are the same element exactly
    // when CompareTo returns 0" — the same rule §4.17 states for
    // `SortedDictionary`'s key). `recordHasDispatchableCompareTo` is a PURE
    // predicate (no IR side effects), mirroring `hasObjectHeaderEquality`
    // above: the side-effecting half (actually building the comparator) is
    // `buildRecordCompareToThunk`, called only by an actual consumer
    // (`buildSortedObjectKeyIdentity`, from `lowerNew`'s allocation path).
    // fix #3948 (stdlib §4.17, v0.294): `SortedDictionary<K, V>` KEY twin of
    // the `SortedSet<T>` ELEMENT exception immediately above — a record KEY
    // that is NOT auto-hashable (`recordTypeOfRef` excluded it above) but
    // DOES declare a dispatchable single-parameter `CompareTo` (§4.17's
    // `where K: IComparable<K>` shape). Before this line the `isSet` gate
    // kept this branch from firing for the KEY side (#3740 named `SortedSet`
    // explicitly for the same reason, never widening to the dictionary
    // family), so a `SortedDictionary<Cmp, V>` returned nullptr here and the
    // downstream diagnostic (LlvmBackend_Calls.cpp's `isDict` block, fix
    // #3865) reported `"not yet supported in codegen"` — the legal-key
    // shape the issue's own commit body records as a distinct diagnostic
    // from the false "does not implement IComparable" claim an actual
    // non-comparable record gets. Soundness is preserved by the existing
    // downstream: `lowerNew` (LlvmBackend_Exprs2.cpp) ALREADY calls
    // `buildSortedObjectKeyIdentity` for `colName == "SortedDictionary"`
    // (line 5557-5558), which itself ALREADY tries `buildRecordCompareToThunk`
    // for the `!recordTypeOfRef(keyTy)` shape (line 1083), so this branch's
    // eligibility widening and that comparator builder land together as
    // #3740's own fix insisted on — widening eligibility WITHOUT that
    // thunk would route through `buildValueHashTrampoline` /
    // `buildValueEqTrampoline`'s STRUCTURAL pair (silent-wrong-identity
    // regression; same shape #3740's commit body warned against for the
    // SortedSet ELEMENT case). Named explicitly to `SortedDictionary` for
    // the same reason `SortedSet` is: `Dictionary`/`OwningDictionary` must
    // NOT widen here (their stored identity is §8.3's `Equals`/`GetHashCode`
    // per §4.8/§4.9, not `CompareTo`-equality — §4.17's rule for
    // SortedDictionary is the strict exception), and the `recordTypeOfRef`
    // path immediately above already covers an AUTO-HASHABLE record (which
    // may or may not also declare CompareTo) without a behavior change.
    else if (!isSet && collTy && collTy->headName() == "SortedDictionary" &&
             !ty->nullable && recordHasDispatchableCompareTo(ty))
        eligible = true;
    else if (isSet && collTy && collTy->headName() == "SortedSet" &&
             !ty->nullable && recordHasDispatchableCompareTo(ty))
        eligible = true;
    if (!eligible) return nullptr;
    if (byteSize) {
        llvm::Type* lt = lowerTypeRef(ty);
        *byteSize = lt ? module_->getDataLayout().getTypeAllocSize(lt)
                       : (uint64_t)8;
    }
    return ty;
}

// issues D3 / D8 / §8.3: generic-dict hash trampoline `uint64_t(const void*)`
// over an arbitrary hashable value type. The runtime stores the key's
// natural bytes; the trampoline reinterprets the byte pointer as a pointer to
// the key's LLVM type, loads the value, and runs the same hash the language
// uses for the type — emitValueHashCode (§8.3): a string folds its code points
// (#346 content hash), a record folds its field hashes, a scalar derives an i32
// from its bits. The i32 is zero-extended (the runtime keeps the low 32 bits).
llvm::Function* Emitter::buildValueHashTrampoline(const sema::TypeRef* ty) {
    // A record key reuses the existing record trampoline (tested path).
    if (const sema::TypeSymbol* rec = recordTypeOfRef(ty)) {
        // issue #462 / §14.1: a GENERIC record key (`Dictionary<Box<string>, V>`)
        // needs the specialized struct layout + concrete per-field hashing inside
        // the trampoline. A no-op for a non-generic record key.
        RecordSubstGuard keySubst(this, ty);
        return buildRecordHashTrampoline(rec);
    }
    // §35.7 (#2047): a value-like CHOICE key hashes by tag + active payload;
    // install the instantiation subst so a generic choice key uses its
    // specialized layout + concrete per-payload hashing (a no-op if non-generic).
    if (ty->kind == sema::TypeRef::Kind::Named && ty->declaration &&
        ty->declaration->kind == ast::TypeDeclKind::Choice) {
        RecordSubstGuard keySubst(this, ty);
        return buildChoiceHashTrampoline(ty->declaration);
    }
    llvm::Type* lt = lowerTypeRef(ty);
    auto* fty = llvm::FunctionType::get(i64(), {ptrTy()}, false);
    auto* fn = llvm::Function::Create(fty, llvm::Function::InternalLinkage,
                                      "bz$valkey$hash", module_.get());
    // fix #2773 (rows A3/A4) / §8.3: a class/interface/`Object` key hashes by
    // its §8.3 pair — the user's `GetHashCode` override when declared, else the
    // ASSIGNED identity hash ("identity hashes are assigned, not derived …
    // never expose addresses"). lowerObjectGetHashCode is that dispatch, and is
    // the hash half of the same vtable pair buildValueEqTrampoline dispatches
    // equality through, so `a.Equals(b)` ⇒ equal hashes holds in the table.
    if (hasObjectHeaderEquality(ty)) {
        auto savedIP2 = builder_.saveIP();
        llvm::Function* savedFn2 = currentFn_;
        currentFn_ = fn;
        builder_.SetInsertPoint(llvm::BasicBlock::Create(llvm_, "entry", fn));
        llvm::Value* k = builder_.CreateLoad(ptrTy(), fn->getArg(0), "objkey");
        llvm::Value* oh = lowerObjectGetHashCode(k);
        builder_.CreateRet(oh ? builder_.CreateZExt(oh, i64())
                              : builder_.CreatePtrToInt(k, i64()));
        builder_.restoreIP(savedIP2);
        currentFn_ = savedFn2;
        return fn;
    }
    auto savedIP = builder_.saveIP();
    llvm::Function* savedFn = currentFn_;
    currentFn_ = fn;
    auto* entry = llvm::BasicBlock::Create(llvm_, "entry", fn);
    builder_.SetInsertPoint(entry);
    llvm::Value* v = builder_.CreateLoad(lt, fn->getArg(0), "key.val");
    // per §10.5 (v0.183): an enum key hashes via a user `GetHashCode(): int` if
    // declared, else by its underlying integer (§8.3). Non-enum keys:
    // emitValueHashCode hashes a string by content, a scalar by its bits.
    llvm::Value* h =
        (ty->kind == sema::TypeRef::Kind::Named && ty->declaration &&
         ty->declaration->kind == ast::TypeDeclKind::Enum)
            ? emitEnumHashCode(v, ty->declaration)
            : emitValueHashCode(v, ty);                    // i32
    builder_.CreateRet(builder_.CreateZExt(h, i64()));
    builder_.restoreIP(savedIP);
    currentFn_ = savedFn;
    return fn;
}

// issues D3 / D8 / §8.3: generic-dict equality trampoline
// `bool(const void* a, const void* b)` over an arbitrary hashable value
// type. Loads both keys from their byte pointers and compares them by the
// language's equality for the type: string content (emitStringEq, §4.6),
// structural record equality (emitStructEquality, §9.1/§9.2 — consistent with
// the hash), or an integer/float compare for primitives/enums.
llvm::Function* Emitter::buildValueEqTrampoline(const sema::TypeRef* ty) {
    if (const sema::TypeSymbol* rec = recordTypeOfRef(ty)) {
        // issue #462 / §14.1: a GENERIC record key needs the specialized struct
        // layout + concrete per-field equality inside the trampoline. A no-op for
        // a non-generic record key.
        RecordSubstGuard keySubst(this, ty);
        return buildRecordEqTrampoline(rec);
    }
    // §35.7 (#2047): a value-like CHOICE key compares by tag + active payload,
    // under the instantiation subst (consistent with buildChoiceHashTrampoline).
    if (ty->kind == sema::TypeRef::Kind::Named && ty->declaration &&
        ty->declaration->kind == ast::TypeDeclKind::Choice) {
        RecordSubstGuard keySubst(this, ty);
        return buildChoiceEqTrampoline(ty->declaration);
    }
    llvm::Type* lt = lowerTypeRef(ty);
    auto* fty = llvm::FunctionType::get(i1(), {ptrTy(), ptrTy()}, false);
    auto* fn = llvm::Function::Create(fty, llvm::Function::InternalLinkage,
                                      "bz$valkey$eq", module_.get());
    auto savedIP = builder_.saveIP();
    llvm::Function* savedFn = currentFn_;
    currentFn_ = fn;
    auto* entry = llvm::BasicBlock::Create(llvm_, "entry", fn);
    builder_.SetInsertPoint(entry);
    using K = sema::TypeRef::Kind;
    // fix #685 / §4.4 / §8.3: decimal trampoline — load the two structs directly
    // from the byte pointers and call bz_decimal_cmp (normalized equality).
    bool isDecimal = ty && ty->kind == K::Primitive &&
                     ty->primitive == sema::PrimitiveType::Decimal && !ty->nullable;
    if (isDecimal) {
        // The key bytes are the raw %bz.decimal struct bytes; ptr arg is the key ptr.
        auto* cmpFn = declareC("bz_decimal_cmp", i32(), {ptrTy(), ptrTy()});
        llvm::Value* cmp = builder_.CreateCall(cmpFn,
            {fn->getArg(0), fn->getArg(1)}, "dec.eq.cmp");
        llvm::Value* eq = builder_.CreateICmpEQ(cmp, llvm::ConstantInt::get(i32(), 0));
        builder_.CreateRet(eq);
        builder_.restoreIP(savedIP);
        currentFn_ = savedFn;
        return fn;
    }
    llvm::Value* a = builder_.CreateLoad(lt, fn->getArg(0), "eq.a");
    llvm::Value* b = builder_.CreateLoad(lt, fn->getArg(1), "eq.b");
    bool isString = ty->kind == K::Primitive &&
                    ty->primitive == sema::PrimitiveType::String;
    // fix #2773 (rows A3/A4) / §8.3 / §8.4: a class/interface/`Object` key
    // compares by its §8.3 pair — "For classes, `=` always invokes `Equals`.
    // … classes customize equality by overriding `Equals`" — so dispatch the
    // vtable slot rather than falling to the plain pointer ICmp below, which
    // would silently ignore an override the hash trampoline DOES honour and
    // break §8.3's hash/equality agreement inside the table.
    llvm::Value* eq =
        hasObjectHeaderEquality(ty) ? lowerObjectEquals(a, b) : nullptr;
    if (eq) {
        // (dispatched above)
    } else if (isString)
        eq = emitStringEq(a, b);                           // §4.6 content compare
    else if (lt->isFloatingPointTy()) {
        // fix #4586 / §4.3.1 (v0.306): a `double`/`float` DICT/SET KEY must
        // compare by the SAME reflexive `=` the language gives floats —
        // "NaN = NaN is true" — not raw IEEE `oeq`, which is false for
        // NaN-vs-NaN and would make `Set<double>.Contains`/
        // `Dictionary<double, V>.ContainsKey` answer false for a key the
        // scalar `=` operator (and §4.3.1's own rationale) say is present.
        // Mirrors the scalar `=` lowering (LlvmBackend_Exprs.cpp, `op == "="`)
        // and emitValueEquality's float arm (fix #1165, LlvmBackend_Layout.cpp):
        // oeq(a,b) OR (isNaN(a) AND isNaN(b)); ±0.0 stay equal because oeq is
        // already true for them. This trampoline previously used raw
        // `FCmpOEQ`, unlike the struct/array arm below which already routes
        // through emitValueEquality — so a `Guid` key got the reflexive rule
        // for free while a bare `double`/`float` key did not.
        llvm::Value* ieee_eq  = builder_.CreateFCmpOEQ(a, b, "feq");
        llvm::Value* a_is_nan = builder_.CreateFCmpUNO(a, a, "anan");
        llvm::Value* b_is_nan = builder_.CreateFCmpUNO(b, b, "bnan");
        llvm::Value* both_nan = builder_.CreateAnd(a_is_nan, b_is_nan, "abnan");
        eq = builder_.CreateOr(ieee_eq, both_nan, "feq_ref");
    } else if (lt->isStructTy() || lt->isArrayTy())
        // per §9.6 / stdlib §2.6 (#1464): a STRUCT- or ARRAY-shaped key — a
        // value-builtin like `Guid` ({i64,i64}), or an inline-array key —
        // compares STRUCTURALLY (field/bytewise), not via a raw ICmp on a
        // struct/array operand. `ICmpInst` accepts only int/ptr operands, so a
        // struct reaching `CreateICmpEQ` tripped `ICmpInst::AssertOK`. Route
        // through the canonical per-value equality helper (the same one the `=`
        // operator and record-field equality use), which recurses into the
        // struct's fields (guid → two i64 compares) — consistent with the hash
        // trampoline's structural fold.
        eq = emitValueEquality(a, b, ty);
    else if (ty->kind == K::Named && ty->declaration &&
             ty->declaration->kind == ast::TypeDeclKind::Enum)
        // per §10.5 (v0.183): an enum key compares via a user `Equals(E): bool`
        // if declared, else by underlying-integer equality (§8.3).
        eq = emitEnumEquals(a, b, ty->declaration);
    else
        eq = builder_.CreateICmpEQ(a, b);                  // int/bool/char/ptr
    builder_.CreateRet(eq);
    builder_.restoreIP(savedIP);
    currentFn_ = savedFn;
    return fn;
}

// fix #3488 / stdlib §4.17 / spec §14.2 (v0.283): the `int cmp(const void* a,
// const void* b)` thunk for a REFERENCE key/element, where `a`/`b` point at the
// stored key BYTES — for a class or interface key those bytes are the reference
// itself, so each argument is a `ptr*` and the thunk loads through it. The body
// dispatches the type's own `IComparable<T>.CompareTo`, which is what stdlib
// §4.17 names as the ordering authority: "Ordering is defined by `K`'s
// `IComparable<K>.CompareTo`".
//
// ⚠ THIS IS NOT A NEW CAPABILITY. #3428's commit body and #3488/#3489 both state
// that "there is no `CompareTo`-through-the-vtable comparator anywhere in the
// compiler". That was measured wrong: `Array<T>.Sort()` has built exactly this
// thunk since #1195 (`bz$classcmp$<C>`, class element) and #2768
// (`bz$ifacecmp$<I>`, interface element, via ensureIfaceCmpThunk), with the
// IDENTICAL `i32 (ptr, ptr)` slot-address ABI `buildSortedKeyComparator` needs.
// What was missing was the wiring, not the comparator. This function is the
// Sort() builder LIFTED OUT so that every caller shares one thunk per type —
// per the standing rule that a second spelling of an existing decision is the
// dominant defect generator here, the Sort() arm calls this rather than
// carrying its own copy, and so (since `narrow #3488`) does `Array<T>.Sorted()`,
// which had carried a THIRD, verbatim inline copy that #3488's own body
// asserted did not exist.
//
// Null for a reference type with no dispatchable `CompareTo`: `Object` (no
// declaration to search), and a class or interface that declares none. §4.17
// constrains `K: IComparable<K>`, but nothing checks that constraint for a
// builtin generic (#3490), so this arm is reachable and its callers must treat
// null as "not orderable" exactly as they already do for a record key.
llvm::Function* Emitter::buildObjectCompareToThunk(const sema::TypeRef* ty) {
    if (!ty || !hasObjectHeaderEquality(ty)) return nullptr;
    // `Object` is Kind::Primitive here — it has no declaration to search for a
    // `CompareTo`, and spec §8.1's declaration of it lists none (v0.283:
    // `GetType`/`Equals`/`GetHashCode`/`ToString`/`ReferenceEquals`, and
    // nothing else). An earlier revision of this line cited "stdlib §2.1" for
    // that fact; stdlib §2.1 is `Console`. The claim was right, the authority
    // was not.
    if (ty->kind != sema::TypeRef::Kind::Named || !ty->declaration) return nullptr;

    if (ty->declaration->kind == ast::TypeDeclKind::Interface) {
        // #2768's thunk, selected by the exact §14.2 rule-3 constraint
        // (selfComparableIfaceCompareTo) and dispatched through the §13 itable.
        if (sema::MethodSymbol* ifm = selfComparableIfaceCompareTo(ty))
            return ensureIfaceCmpThunk(ty->declaration, ifm);
        return nullptr;
    }

    sema::MethodSymbol* cmpMs = findMethodByName(ty->declaration, "CompareTo");
    if (!cmpMs || cmpMs->params.size() != 1) return nullptr;

    // fix #3512 (spec §2.8.3 rule 3, v0.283): the cache key is the
    // MODULE-QUALIFIED name, matching `ensureIfaceCmpThunk`'s interface arm
    // above (`bz$ifacecmp$` + mangledTypeName) and every other `bz$`/`vt$`/
    // `td$` symbol since #1488. It was #1195's UNQUALIFIED
    // `ty->declaration->name` until #3512: rule 3 states that "Two modules each
    // declaring a public type of the same name declare two unrelated types",
    // and two such types collapsed to ONE `bz$classcmp$Ord` here — so the
    // comparator built for whichever type reached this function FIRST was then
    // handed the other type's instances. Measured on 06551dd: two modules each
    // declaring `public class Ord: IComparable<Ord>` with OPPOSITE `CompareTo`
    // both sorted ascending (`Array.Sort`, `Array.Sorted` and
    // `SortedDictionary` iteration/`FirstKey`/`LastKey` alike), and with
    // DIFFERING FIELD LAYOUTS (a leading `string` against a leading `int`) the
    // shared thunk read one type's field at the other's offset and the program
    // SEGV'd (rc=139).
    //
    // The `SortedDictionary` half is why this is not merely an ordering bug:
    // `buildSortedObjectKeyIdentity` names its identity function after this one
    // (`bz$sortedobjkey$eq$` + this name), so the collision decided MAP
    // MEMBERSHIP too — the repro raised `ArgumentException` from an `Add` of a
    // key that was not present, §4.17's duplicate-key tier fired on two
    // distinct keys. Qualifying here fixes both, and the identity function
    // inherits the qualified name for free.
    //
    // Sharing is preserved exactly where it was intended: one thunk per TYPE,
    // so a program that both sorts an `Array<Ord>` and iterates a
    // `SortedDictionary<Ord, V>` still emits ONE.
    //
    // fix #3530 (§14.1 / §2.8.3 rules 3-4 / stdlib §4.4, v0.283): a CONSTRUCTED
    // generic class needs BOTH halves of rule 3's identity for a constructed
    // type — "a constructed generic type is identified by its generic's
    // identity plus the identities of its arguments" — and #3512/#3529 supplied
    // only the generic's half. `Array<Box<int>>` and `Array<Box<long>>` in one
    // program both mangled to `bz$classcmp$M.Box` and shared ONE thunk.
    // `gpNames.size() == genericArgs.size()` is the same concrete-instantiation
    // test RecordSubstGuard applies (LlvmBackend_Layout.cpp); a NON-generic
    // class fails it and keeps the pre-#3530 name byte-for-byte.
    const std::vector<std::string_view> gpNames =
        sema::genericParamNames(ty->declaration);
    const bool genericInst =
        !gpNames.empty() && gpNames.size() == ty->genericArgs.size();

    // Each argument is composed through the ACTIVE OUTER substitution FIRST,
    // via `substituteTypeRefLocal` — the SAME call the sibling generic-receiver
    // site makes (LlvmBackend_Calls2.cpp, the #2192/#3020 arm), deliberately
    // rather than a second spelling of it. Inside a specialization of an OUTER
    // generic, `Array<Box<T>>`'s argument is still the bare parameter `T`, a
    // TypeRef whose structural key ("UT<>;") is identical for EVERY binding, so
    // keying on the raw argument would rebuild one level down exactly the
    // collapse this fix removes — and in the REGRESSION direction, because
    // pre-#3530 every binding ran the placeholder template while post-#3530 the
    // first would mint a correct specialization that the rest silently inherit.
    //
    // ⚠ A SHALLOW resolution is not enough, which is why this is not
    // `resolveTypeParamRef`: that helper returns its argument unchanged the
    // moment `declaration` is non-null, so it answers the same TypeRef for
    // `Box<Pair<T>>` at every binding of `T` (no recursion into a CONSTRUCTED
    // argument's own arguments), and it drops the `?` that `appendDescKey`
    // deliberately encodes, collapsing `Box<T?>` onto `Box<T>`.
    // `substituteTypeRefLocal` does both — see its #2057 recursion arm and its
    // "Preserve `T?` nullability of the placeholder occurrence" copy — and
    // #2192/#3020 record that this exact collapse class ("collapsed every
    // `A<U>` onto ONE shared (erased) descriptor") already shipped once.
    //
    // An EMPTY outer environment (the ordinary top-level case) makes this the
    // identity, so a program with no enclosing generic is byte-identical.
    std::vector<const sema::TypeRef*> instArgs;
    if (genericInst) {
        instArgs.reserve(ty->genericArgs.size());
        for (const sema::TypeRef* a : ty->genericArgs)
            instArgs.push_back(
                substituteTypeRefLocal(a, genericSubstSemaTy_));
    }

    std::string thunkName =
        std::string("bz$classcmp$") + mangledTypeName(ty->declaration);
    for (const sema::TypeRef* a : instArgs)
        thunkName += "$" + descReadableMangle(a);

    // The cache is keyed on the STRUCTURAL identity (`descIdentityKey` — the
    // recursive encoding the per-monomorphization runtime descriptors and
    // buildSpecializedMethodFn's #1947 key already share), NOT on the emitted
    // name: `descReadableMangle` is explicitly cosmetic (it renders a type by
    // its SIMPLE name and lets LLVM uniquify a clash), so two distinct
    // instantiations can mangle alike — `Box<Outer1.Ord>` and `Box<Outer2.Ord>`,
    // the §11.6 shape #3529 fixed for the enclosing-type path — and a
    // name-keyed `getFunction` would then hand the second the first's thunk.
    // For a non-generic class `instArgs` is empty, so the key is that class's
    // declaration alone and the pre-#3530 sharing (one thunk per type across
    // Sort/Sorted/SortedDictionary) is preserved exactly.
    const std::string thunkKey = descIdentityKey(ty->declaration, instArgs);
    if (auto cit = classCmpThunks_.find(thunkKey); cit != classCmpThunks_.end())
        return cit->second;

    auto* thunkTy = llvm::FunctionType::get(i32(), {ptrTy(), ptrTy()}, false);
    auto* fn = llvm::Function::Create(thunkTy, llvm::Function::InternalLinkage,
                                      thunkName, module_.get());
    // Publish BEFORE the body below can build a specialization: that emits a
    // whole method body, which may itself sort an `Array<Self>` and re-enter
    // here for the same type. (The pre-#3530 body emitted nothing, so the
    // pre-#3530 `getFunction` check could sit after the Create.)
    classCmpThunks_[thunkKey] = fn;
    auto* bb = llvm::BasicBlock::Create(llvm_, "entry", fn);
    // A LOCAL IRBuilder, not `builder_`: the thunk is emitted in the middle of
    // another function's lowering, and touching `builder_`/`currentFn_` here
    // would drag `propagateBB_`/`unwindTarget_` with it (the hazard
    // rejectUnimplementedComparerCtor documents for the comparer trampolines).
    llvm::IRBuilder<> thunkB(bb);
    llvm::Value* aPtr = thunkB.CreateLoad(ptrTy(), &*fn->arg_begin());
    llvm::Value* bPtr = thunkB.CreateLoad(ptrTy(), &*std::next(fn->arg_begin()));
    auto* cmpToTy = llvm::FunctionType::get(i32(), {ptrTy(), ptrTy()}, false);
    llvm::Value* result = nullptr;
    auto vsIt = vtableSlot_.find(cmpMs);
    if (vsIt != vtableSlot_.end()) {
        // Virtual dispatch through the receiver's vtable (field 0), per §12.
        llvm::Value* vtable = thunkB.CreateLoad(ptrTy(), aPtr);
        llvm::Value* slotPtr =
            thunkB.CreateGEP(ptrTy(), vtable, {thunkB.getInt32(vsIt->second)});
        llvm::Value* fnPtr = thunkB.CreateLoad(ptrTy(), slotPtr);
        result = thunkB.CreateCall(cmpToTy, fnPtr, {aPtr, bPtr});
    } else {
        auto fnIt = methodFns_.find(cmpMs);
        // findMethodByName already demands `methodFns_.count(ms)`, so the miss
        // arm is unreachable; keep it well-formed rather than trusting that.
        if (fnIt == methodFns_.end()) {
            classCmpThunks_.erase(thunkKey);
            fn->eraseFromParent();
            return nullptr;
        }
        // fix #3530 (§14.1 / stdlib §4.4, v0.283): for a GENERIC class,
        // `methodFns_[cmpMs]` is the UNSPECIALIZED TEMPLATE — the body emitted
        // with every type parameter at its placeholder lowering (`T` -> i32),
        // which the emitter itself documents as "never the call target — every
        // call binds the concrete substitution and routes to a specialization"
        // (emittingGenericTemplateBody_, LlvmBackend_impl.hpp). Calling it here
        // read every field of a `Box<long>` at the `Box<int>` OFFSETS: measured
        // on e57a1eb, `class Box<T>` with `Lead: T; V: int` had the template
        // load `V` at +0x10 (Lead as i32) where the instantiation's own
        // `Box$CompareTo$spec` loads it at +0x18, so `CompareTo` compared the
        // low half of `Lead` instead of `V` and `Array<Box<long>>.Sort()`
        // returned the array in INSERTION order with nothing diagnosed —
        // a stdlib §4.4 "in place" natural-order sort silently not performed.
        // The VIRTUAL arm above was already correct (the per-instantiation
        // `vt$<C>$specN` carries the specialized method), which is why only a
        // non-virtual `CompareTo` was affected.
        // fix #3566 (second shape, stdlib §4.17 / §2.8.3 rule 4, v0.306): the
        // `genericInst` gate asks whether the ELEMENT type is a constructed
        // generic, which is the right question only while the `CompareTo`
        // being specialized is declared by that element type. A NON-generic
        // class over a generic base — `class DPlain: GBase<long>` — has no
        // generic params of its own, so `genericInst` is false and the
        // placeholder template was called: measured `first=3 last=2 count=3`
        // (insertion order) on the identical `SortedDictionary` repro. The
        // layout that matters belongs to `GBase<long>`, not to `DPlain`.
        // `specializedClassCompareToFn` resolves the base's parameters from
        // the base CLAUSE in that case, so the element type need not be
        // generic at all; `instArgs` and `gpNames` are both empty here, which
        // its own `instArgs.size() != gpNames.size()` guard admits, and the
        // thunk name and cache key (`descIdentityKey(decl, {})`) are the
        // pre-#3530 one-thunk-per-type form, unchanged.
        llvm::Function* target = nullptr;
        if (genericInst || (cmpMs->parent && ty->declaration &&
                            cmpMs->parent != ty->declaration))
            target = specializedClassCompareToFn(ty, cmpMs, gpNames, instArgs,
                                                 fnIt->second);
        if (!target) target = fnIt->second;
        result = thunkB.CreateCall(target->getFunctionType(), target,
                                   {aPtr, bPtr});
    }
    thunkB.CreateRet(result);
    return fn;
}


// fix #3530 (§14.1 / §2.8.3 rule 4 monomorphization, v0.283): the
// `CompareTo` body MONOMORPHIZED for one concrete instantiation of a generic
// class, for buildObjectCompareToThunk to call in place of the placeholder
// template. Null when the shape is not one this thunk's ABI can carry, in
// which case the caller keeps the pre-#3530 template call — a fallback that is
// no worse than the behaviour it replaces and cannot introduce a new mis-typed
// call.
//
// The substitution install is the same param<->arg binding RecordSubstGuard and
// the generic-class RECEIVER path (LlvmBackend_Layout.cpp's computed-accessor
// prolog, #3143) perform, and it consumes the caller's ALREADY-COMPOSED
// `instArgs` — the `substituteTypeRefLocal` results that also keyed the thunk's
// name and cache entry — so the name, the cache key and the emitted body cannot
// disagree about which instantiation this is. RecordSubstGuard itself is not
// reused: it deliberately admits only Record and Choice declarations, and
// widening its kind filter would change every one of its dozen existing call
// sites that can be handed a class TypeRef.
llvm::Function* Emitter::specializedClassCompareToFn(
        const sema::TypeRef* ty, sema::MethodSymbol* ms,
        const std::vector<std::string_view>& gpNames,
        const std::vector<const sema::TypeRef*>& instArgs,
        llvm::Function* templateFn) {
    // fix #3566 (stdlib §4.17 / spec §2.8.3 rule 4 / §14.1, v0.306) — AN
    // INHERITED `CompareTo` IS NOW SPECIALIZED, not declined.
    //
    // `findMethodByName` walks the base chain, so `ms` can belong to a BASE
    // class whose body reads the BASE's type-parameter names — while `gpNames`
    // are the DERIVED class's. For `class GBase<T>` / `class Derived<U>:
    // GBase<U>`, installing {"U" -> long} leaves `GBase`'s `T` unbound, so the
    // "specialization" lowered `GBase`'s `T`-typed fields at the i32
    // placeholder and came out INSTRUCTION-IDENTICAL to the template. #3530
    // therefore silently did nothing for that shape, and #3566 declined it
    // outright rather than emit a body that was still effectively a template.
    //
    // MEASURED consequence of the decline (on integration HEAD 00e10c0e, the
    // issue's own repro): `SortedDictionary<Derived<long>, int>` with keys
    // inserted 3, 1, 2 printed `first=3 last=2 count=3` — insertion order, i.e.
    // the comparator answered garbage for every pair. stdlib §4.17 makes
    // `K`'s `IComparable<K>.CompareTo` the ordering authority AND the key
    // identity ("two keys are the same key exactly when `CompareTo` returns
    // 0"), so a comparator reading the wrong field offsets decides both
    // ITERATION ORDER and MAP MEMBERSHIP. Silent wrong answer, rc=0.
    //
    // THE FIX is the one #3566 proposes, and it reuses the walk that already
    // exists rather than adding a second spelling of it (LANE_RULES §6):
    // `computeAncestorGenericSubst` walks `ty->declaration`'s base chain up to
    // `ms->parent`, composing each level's `baseTypeRefs[i]->genericArgs`
    // through the substitution built so far, and fills both maps keyed by the
    // ANCESTOR's parameter names. It is the same authority `fieldAddr`'s
    // inherited-field branch uses (#2840/#585), so the body is lowered against
    // exactly the offsets the field reads use. It is called BEFORE the new maps
    // are installed, so its own seed composes through the still-active outer
    // substitution (the transitive `Outer<X>` case, #2202).
    //
    // The thunk NAME and CACHE KEY keep using the DERIVED class's `instArgs`
    // — correct, because the thunk is per derived instantiation; only the
    // substitution the BODY is built under moves to the base's names.
    if (!ms->parent || !ty->declaration) return nullptr;
    const bool inheritedCompareTo = (ms->parent != ty->declaration);
    // The thunk loads two `ptr`s and calls `i32 (ptr, ptr)`. Require the
    // TEMPLATE to already have that signature before specializing: it is the
    // signature `CompareTo(other: Self): int` produces for every instantiation
    // (a class-typed parameter lowers to `ptr` in the template exactly as in
    // the specialization), while a `CompareTo(other: T): int` — legal, but not
    // the §14.2 `T: IComparable<T>` shape `Sort()` is admitted under — lowers
    // its parameter to the i32 placeholder here and to the concrete width
    // there. Checking the PARAMETER shapes on the template rather than on the
    // built specialization declines them without emitting a body at all.
    llvm::FunctionType* tft = templateFn ? templateFn->getFunctionType()
                                         : nullptr;
    if (!tft || tft->getNumParams() != 2 || tft->isVarArg() ||
        !tft->getReturnType()->isIntegerTy(32) ||
        !tft->getParamType(0)->isPointerTy() ||
        !tft->getParamType(1)->isPointerTy())
        return nullptr;
    if (isStaticMethod(ms)) return nullptr;  // `this` is the thunk's first arg

    CallableInfo ci;
    if (!callableInfo(ms, ci)) return nullptr;  // abstract / no AST body

    if (instArgs.size() != gpNames.size()) return nullptr;
    auto savedSubst = genericSubst_;
    auto savedSemaTy = genericSubstSemaTy_;
    std::unordered_map<std::string_view, llvm::Type*> newSubst;
    std::unordered_map<std::string_view, const sema::TypeRef*> newSema;
    if (inheritedCompareTo) {
        // fix #3566: the body reads the DECLARING BASE's parameter names.
        // `computeAncestorGenericSubst` re-derives them POSITIONALLY from each
        // base clause, so a renamed parameter (`Derived<U>: GBase<U>`, body
        // reading `T`) resolves exactly as an identically-spelled one does; a
        // shared spelling never reaches it by name. It returns false when the
        // ancestor is non-generic or the chain could not pin every parameter —
        // and in both cases declining is still the honest answer: the template
        // is then either already correct (nothing to substitute) or we cannot
        // prove what to substitute, which is #3530's own "no worse than the
        // behaviour it replaces" fallback.
        if (!computeAncestorGenericSubst(ty, ms->parent, newSubst, newSema))
            return nullptr;
    } else {
        // Both halves come from the caller's composed `instArgs`, in the
        // lockstep order the sibling site uses (`newSubst[p] =
        // lowerTypeRef(argSema); newSema[p] = argSema;`), so the LLVM and sema
        // maps cannot desync — the desync #3020 records as corrupting the
        // buildSpecializedMethodFn cache key.
        for (std::size_t gi = 0; gi < gpNames.size(); ++gi) {
            newSubst[gpNames[gi]] = lowerTypeRef(instArgs[gi]);
            newSema[gpNames[gi]] = instArgs[gi];
        }
    }
    genericSubst_ = std::move(newSubst);
    genericSubstSemaTy_ = std::move(newSema);
    // buildSpecializedMethodFn caches on (ms, subst, identity keys, paramTys),
    // so an instantiation whose `CompareTo` was already specialized for an
    // ordinary source-level call is REUSED rather than emitted twice; it also
    // saves and restores the whole builder/state-machine context itself, which
    // is what makes it safe to call from the middle of another body's lowering.
    llvm::Function* specFn =
        buildSpecializedMethodFn(ms, ci, {ptrTy(), ptrTy()});
    genericSubst_ = std::move(savedSubst);
    genericSubstSemaTy_ = std::move(savedSemaTy);
    if (!specFn) return nullptr;
    // The RETURN type is the one shape the template pre-check cannot settle: it
    // is re-derived from the declared return type UNDER the substitution, so a
    // `CompareTo(): T` bound to `long` is `i32` on the template and `i64` here.
    // Unlike the parameter checks above this one necessarily runs AFTER the body
    // is built, so declining leaves an unused internal function in the module
    // (harmless — nothing references it — but it is not the "no dead body" the
    // parameter pre-check gives).
    llvm::FunctionType* sft = specFn->getFunctionType();
    if (sft->getNumParams() != 2 || !sft->getReturnType()->isIntegerTy(32))
        return nullptr;
    return specFn;
}

// fix #3740 (stdlib §4.18 / §14.2, v0.291): is `ty` a RECORD that declares a
// dispatchable single-parameter `CompareTo`? A PURE predicate (no IR side
// effects — mirrors `hasObjectHeaderEquality`'s role for the class/interface
// exception in `genericBackingType`). Deliberately independent of
// `recordTypeOfRef`, which requires the record to be auto-hashable (§9.2): a
// non-auto-hashable record (e.g. one with an explicit field declaration, not
// an auto-property) can still be `IComparable<T>`-ordered — stdlib §4.18
// requires only `T: IComparable<T>`, not hashability — so this predicate must
// admit exactly the shape `recordTypeOfRef` excludes.
//
// review fix (#3740, silent-wrong-identity finding): a GENERIC record is
// EXCLUDED here too, matching `buildRecordCompareToThunk`'s own exclusion —
// the two must agree, because this predicate is `genericBackingType`'s
// ELIGIBILITY gate and the thunk is the only thing that can make that
// eligibility SOUND. Before this line, a non-auto-hashable generic record
// with `CompareTo` was eligibility-wise ADMITTED here while the thunk
// builder declined it: `genericBackingType` returned non-null, `lowerNew`
// built the dict table via `buildValueHashTrampoline`/`buildValueEqTrampoline`
// (the STRUCTURAL fallback, since `recordTypeOfRef` also declines a
// non-auto-hashable record), `buildSortedObjectKeyIdentity` then found
// `buildRecordCompareToThunk` null (generic) and left that structural pair
// UNCHANGED — so the SortedSet silently ordered/deduped by STRUCTURAL
// equality instead of `CompareTo`-equality. MEASURED: a
// `record BadG<T>: IComparable<BadG<T>>` with a class-typed `Item: R` field
// and a `Payload: T` field, `CompareTo` comparing only `Item.V` — two
// elements with equal `Item.V` but different `Payload` landed as TWO
// entries (`Count` == 2) where §4.18 ("adding an equal element leaves the
// set unmodified") requires ONE. That is a loud-crash-to-quiet-wrong-answer
// regression relative to the pre-#3740 behavior (a compiler abort), which
// this project treats as strictly worse than the crash it replaced. With
// this exclusion, `genericBackingType` declines a generic record exactly as
// it did before #3740, and the pre-existing loud paths (the #2974
// construction-time degrade, or — for a borrowed-parameter receiver with no
// construction — the residual malformed-IR crash #3865 already tracks for
// every non-eligible record shape) fire instead of a silent wrong answer.
// Monomorphizing a generic record's `CompareTo` (`buildRecordCompareThunk`
// learning to handle it, the alternative this fix declines) is a
// self-contained follow-up, not attempted here: no test in this lane
// exercises it, and the conservative, narrow fix is to keep the two
// predicates in agreement rather than teach the thunk a new capability.
bool Emitter::recordHasDispatchableCompareTo(const sema::TypeRef* ty) const {
    if (!ty || ty->kind != sema::TypeRef::Kind::Named || !ty->declaration ||
        ty->declaration->kind != ast::TypeDeclKind::Record)
        return false;
    if (!sema::genericParamNames(ty->declaration).empty()) return false;
    sema::MethodSymbol* cm = findMethodByName(ty->declaration, "CompareTo");
    return cm && cm->params.size() == 1;
}

// fix #3740 (stdlib §4.18 / §14.2, v0.291): the record analogue of
// `buildObjectCompareToThunk` above — a `CompareTo`-dispatch comparator thunk
// `int cmp(const void* a, const void* b)` for a RECORD element/key. Unlike a
// class/interface key (whose stored bytes ARE the reference, so the ABI is
// `ptr*` and the caller/callee spill through an extra indirection), a
// record's stored key/element bytes are its own INLINE struct value — the
// same layout `buildRecordHashTrampoline`/`buildRecordEqTrampoline` read — so
// `a`/`b` here are direct pointers to the record and no vtable dispatch
// applies: records are not polymorphic (CG29). `this` passes by pointer
// (§9.4/CG29, forwarded unchanged — `a` is already that address) and the
// second operand by value (loaded from `b`), the identical receiver/argument
// shape `Math.Min`'s pre-existing record-`CompareTo` dispatch builds inline
// (`comparableCompareTo`/`emitCompareToCmp`, LlvmBackend_Calls.cpp) — reused
// here as one shared thunk rather than re-derived per call site.
//
// A LOCAL IRBuilder, not `builder_` — mirrors `buildObjectCompareToThunk`:
// this thunk is built mid-lowering of another function, and touching
// `builder_`/`currentFn_` would drag `propagateBB_`/`unwindTarget_` with it
// (the same hazard `rejectUnimplementedComparerCtor` documents for the
// comparer trampolines). A DIRECT call to the compiled body, not
// `emitMethodCall`, for the identical reason.
//
// Null for a non-record, a record with no single-parameter `CompareTo`, or a
// GENERIC record — monomorphizing a generic record's `CompareTo` the way
// `specializedClassCompareToFn` does for a generic class is deliberately out
// of scope here (no test in this fix exercises it); the caller treats null as
// "not orderable", exactly as it already does for every other reference-type
// residual (#3490). The generic exclusion lives SOLELY in
// `recordHasDispatchableCompareTo` (the entry gate immediately below) —
// review fix (#3740): this function used to carry its OWN second copy of
// that same exclusion, and the eligibility predicate `genericBackingType`
// consults did NOT share it, so a generic record was ADMITTED at
// eligibility while DECLINED here — a silent-wrong-identity gap (see that
// predicate's comment). One spelling, not two, is what keeps them agreeing.
llvm::Function* Emitter::buildRecordCompareToThunk(const sema::TypeRef* ty) {
    if (!recordHasDispatchableCompareTo(ty)) return nullptr;
    const sema::TypeSymbol* rec = ty->declaration;
    sema::MethodSymbol* cmpMs = findMethodByName(rec, "CompareTo");
    auto fnIt = methodFns_.find(cmpMs);
    // `findMethodByName` already demands `methodFns_.count(ms)`; kept
    // well-formed rather than trusting that (mirrors buildObjectCompareToThunk).
    if (fnIt == methodFns_.end()) return nullptr;

    std::string thunkName = std::string("bz$reccmp$") + mangledTypeName(rec);
    if (auto* existing = module_->getFunction(thunkName)) return existing;

    llvm::StructType* st = recordStruct(rec);
    auto* fty = llvm::FunctionType::get(i32(), {ptrTy(), ptrTy()}, false);
    auto* fn = llvm::Function::Create(fty, llvm::Function::InternalLinkage,
                                      thunkName, module_.get());
    auto* bb = llvm::BasicBlock::Create(llvm_, "entry", fn);
    llvm::IRBuilder<> thunkB(bb);
    llvm::Value* thisPtr = &*fn->arg_begin();
    llvm::Value* bVal =
        thunkB.CreateLoad(st, &*std::next(fn->arg_begin()), "cmp.b");
    llvm::Value* result =
        thunkB.CreateCall(fnIt->second, {thisPtr, bVal}, "cmp.result");
    thunkB.CreateRet(result);
    return fn;
}

// fix #3488 / stdlib §4.17 (v0.283): the stored-identity pair for a REFERENCE
// key of a `SortedDictionary<K, V>`.
//
// §4.17 states the rule outright — "Two keys are the same key exactly when
// `CompareTo` returns `0`" — and names the ordering authority in the same
// paragraph: "Ordering is defined by `K`'s `IComparable<K>.CompareTo`". The
// generic-key table's ordinary pair for a class/interface key is #2773's §8.3
// `Equals`/`GetHashCode`, i.e. REFERENCE identity, so `Ord(5)` and a second
// `Ord(5)` were stored as two distinct keys: `TryAdd` of the second returned
// `true` and `Count` reached 2, where §4.17 makes them one key (and v0.282's
// `Add` must raise `ArgumentException`).
//
// The equality half is exactly `CompareTo(a, b) == 0`, dispatched through the
// same thunk that orders the collection, so membership and iteration order can
// never disagree.
//
// ⚠ THE HASH HALF IS DELIBERATELY CONSTANT, AND THAT IS A RECORDED TRADE, NOT
// AN OVERSIGHT. A hash table's contract is `eq(a, b) ⇒ hash(a) == hash(b)`.
// For a reference key there is NO derivable hash consistent with
// `CompareTo`-equality: two `CompareTo`-equal instances are distinct objects
// with distinct §8.3 identity hashes, and `CompareTo` exposes no key material
// to fold. So the only pair that satisfies the table's contract AND §4.17's
// identity rule is (constant, `CompareTo == 0`). That is CORRECT — every key
// lands in one probe cluster and `dict_find_gen`'s linear probe then decides
// by `eq_fn`, which is precisely §4.17's rule — but it makes §4.17's
// "Point operations … are **O(log n)**" clause O(n) for a reference key.
// Satisfying that clause needs the comparator-keyed ordered backing §4.17's own
// "owning sorted collections ride the self-hosted-collections work" note
// defers, and #3489 specifies;
// filed as a follow-up rather than attempted here. The alternative on offer was
// leaving the collection SILENTLY wrong (unsorted iteration, a SEGV on
// `FirstKey`, and duplicate keys) — a complexity clause is the cheaper thing to
// owe.
//
// Scalar, string, record, enum and choice keys never reach here: they keep
// their existing §8.3 pair, whose hash IS consistent with their value equality,
// and their O(1) probe.
bool Emitter::buildSortedObjectKeyIdentity(const sema::TypeRef* keyTy,
                                           llvm::Function*& hashOut,
                                           llvm::Function*& eqOut) {
    llvm::Function* cmp = buildObjectCompareToThunk(keyTy);
    // fix #3740 (stdlib §4.18, v0.291): a non-auto-hashable RECORD element
    // shares the identical "no hash consistent with CompareTo-equality"
    // problem a reference key has (this function's own header comment above)
    // — `buildObjectCompareToThunk` is null for EVERY record (it only ever
    // inspects a class/interface), so try the record analogue next.
    //
    // fix #3987 (stdlib §4.17/§4.18, v0.295): this used to gate the record
    // fallback on `!recordTypeOfRef(keyTy)` — "NARROW to exactly the shape
    // `genericBackingType`'s eligibility branch admits: an AUTO-HASHABLE
    // record that ALSO implements `CompareTo` keeps its EXISTING structural
    // §9.1/§9.2 identity unchanged". That restraint (matching #3740's own,
    // for a DIFFERENT — non-hashable — record shape) is itself the bug:
    // §4.17/§4.18 state the SOLE membership rule as `CompareTo`-equality,
    // unconditionally, with no carve-out for a record that also happens to
    // be hashable — hashability (§3.6/§9.2) is a distinct property. MEASURED
    // (issue 3987's own repro, `SortedSet<P>` with `P` auto-hashable +
    // `CompareTo`): `s.Add(new P(1,2)); s.Add(new P(1,3));` — CompareTo-equal
    // (`X` matches) but structurally distinct (`Y` differs) — landed as
    // `Count == 2` where §4.18 ("adding an equal element leaves the set
    // unmodified") requires `1`. `buildRecordCompareToThunk` already gates on
    // `recordHasDispatchableCompareTo` internally, so dropping the
    // `!recordTypeOfRef` guard here is a no-op for a record with no
    // `CompareTo` (cmp stays null, structural pair unchanged — the #3947
    // rejection at every member catches that shape) and ONLY changes the
    // auto-hashable-WITH-`CompareTo` shape to the CompareTo-based identity
    // §4.17/§4.18 actually specify.
    if (!cmp) cmp = buildRecordCompareToThunk(keyTy);
    // Null for `Object`, for a reference type that declares no `CompareTo`
    // — §4.17 constrains `K: IComparable<K>`, but nothing checks that for a
    // builtin generic (#3490), so such a key stays on today's §8.3 pair rather
    // than acquiring a new rejection this lane did not measure — and for a
    // record with no dispatchable `CompareTo` (same #3490-shaped residual).
    if (!cmp) return false;

    static const char* kHashName = "bz$sortedobjkey$hash";
    llvm::Function* hashFn = module_->getFunction(kHashName);
    if (!hashFn) {
        auto* hty = llvm::FunctionType::get(i64(), {ptrTy()}, false);
        hashFn = llvm::Function::Create(hty, llvm::Function::InternalLinkage,
                                        kHashName, module_.get());
        llvm::IRBuilder<> hb(llvm::BasicBlock::Create(llvm_, "entry", hashFn));
        hb.CreateRet(hb.getInt64(0));
    }

    std::string eqName = std::string("bz$sortedobjkey$eq$") + cmp->getName().str();
    llvm::Function* eqFn = module_->getFunction(eqName);
    if (!eqFn) {
        auto* ety = llvm::FunctionType::get(i1(), {ptrTy(), ptrTy()}, false);
        eqFn = llvm::Function::Create(ety, llvm::Function::InternalLinkage,
                                      eqName, module_.get());
        llvm::IRBuilder<> eb(llvm::BasicBlock::Create(llvm_, "entry", eqFn));
        // Both args are pointers to the stored key BYTES — for a reference key
        // that is a `ptr*`, which is the comparator thunk's own ABI, so they
        // forward unchanged.
        llvm::Value* c = eb.CreateCall(cmp->getFunctionType(), cmp,
                                       {&*eqFn->arg_begin(),
                                        &*std::next(eqFn->arg_begin())});
        eb.CreateRet(eb.CreateICmpEQ(c, llvm::ConstantInt::get(i32(), 0)));
    }

    hashOut = hashFn;
    eqOut   = eqFn;
    return true;
}

// fix #1426 / stdlib §4.17–§4.18: Build a comparison thunk for SortedDictionary
// FirstKey/LastKey, SortedSet First/Last, and sorted-snapshot iteration.
// Signature: int cmp(const void* a, const void* b)
//   a, b = direct pointers to key bytes (NOT void** of void*).
// Returns null for unsupported key types (record, decimal, unresolved param).
// Supported: integer types (i32, i64, i8, i16, i1), float/double.
// String-key SortedDictionary uses a dedicated bz_sort_kv_str/bz_dict_min_key_str
// path and does NOT use this comparator.
llvm::Function* Emitter::buildSortedKeyComparator(const sema::TypeRef* keyTy) {
    if (!keyTy) return nullptr;
    llvm::Type* kllvm = lowerTypeRef(keyTy);
    if (!kllvm) return nullptr;

    // Build a unique name per key LLVM type to cache it per module.
    std::string name = "bz$sdcmp$";
    if (kllvm->isIntegerTy(32))       name += "i32";
    else if (kllvm->isIntegerTy(64))  name += "i64";
    else if (kllvm->isIntegerTy(8))   name += "i8";
    else if (kllvm->isIntegerTy(16))  name += "i16";
    else if (kllvm->isIntegerTy(1))   name += "i1";
    else if (kllvm->isFloatTy())      name += "f32";
    else if (kllvm->isDoubleTy())     name += "f64";
    else if (hasObjectHeaderEquality(keyTy))
        // fix #3488 / stdlib §4.17: a class/interface key lowers to `ptr`, so it
        // used to take the `return nullptr` arm below — and EVERY consumer of
        // this comparator then silently degraded: `SortedDictionary` iteration
        // fell back to the unsorted insertion-order snapshot, and
        // `FirstKey`/`LastKey` failed to lower at all, which lowerLocalDecl
        // degraded (#2434) to a zero-initialised local whose first member read
        // SEGV'd. Ordering for such a key is the type's own `CompareTo`
        // (§4.17: "Ordering is defined by `K`'s `IComparable<K>.CompareTo`"),
        // which buildObjectCompareToThunk dispatches at the same ABI.
        return buildObjectCompareToThunk(keyTy);
    // fix #3740 (stdlib §4.18, v0.291): a non-auto-hashable RECORD key/element
    // with a dispatchable `CompareTo` — the value-type analogue of the
    // class/interface branch immediately above, admitting exactly the shape
    // `genericBackingType`'s own new SortedSet exception admits.
    //
    // fix #3987 (stdlib §4.17/§4.18, v0.295): this used to additionally gate
    // on `!recordTypeOfRef(keyTy)` — "an AUTO-HASHABLE record stays on
    // today's `nullptr`". That left `buildSortedObjectKeyIdentity`'s sibling
    // fix (above, same commit) half-applied: once dedup identity (`.Add`/
    // `.Contains`) uses `CompareTo`-equality for an auto-hashable record with
    // `CompareTo`, leaving ITS ORDERING comparator null reintroduces the
    // identical "accept by one member family, reject by another" defect
    // #3947 was filed to close — just in the opposite direction (`.Add`
    // correctly accepts and dedups; `.First`/`.Last`/`FirstKey`/`LastKey`/
    // sorted iteration falsely reject the SAME value with "does not
    // implement IComparable<T>", even though it does). MEASURED pre-fix: a
    // `SortedSet<AutoHashCmp>` (`AutoHashCmp` auto-hashable + `CompareTo`)
    // rejected `.First` with exactly that message. `recordHasDispatchableCompareTo`
    // already excludes a record with no `CompareTo` (returns false — falls to
    // the `else return nullptr` arm below unchanged), so dropping
    // `!recordTypeOfRef` here only changes the auto-hashable-WITH-`CompareTo`
    // shape, matching `buildSortedObjectKeyIdentity`'s identical widening so
    // hash/eq/ordering agree, per issue 3987's own suggested resolution (a).
    else if (recordHasDispatchableCompareTo(keyTy))
        return buildRecordCompareToThunk(keyTy);
    else return nullptr;  // unsupported (other records, decimal, etc.)

    if (auto* existing = module_->getFunction(name)) return existing;

    auto* fnTy = llvm::FunctionType::get(i32(), {ptrTy(), ptrTy()}, false);
    auto* fn   = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage,
                                        name, module_.get());
    auto savedIP = builder_.saveIP();
    llvm::Function* savedFn = currentFn_;
    currentFn_ = fn;
    auto* bb = llvm::BasicBlock::Create(llvm_, "entry", fn);
    builder_.SetInsertPoint(bb);
    llvm::Value* aPtr = fn->getArg(0);
    llvm::Value* bPtr = fn->getArg(1);
    llvm::Value* av   = builder_.CreateLoad(kllvm, aPtr, "ka");
    llvm::Value* bv   = builder_.CreateLoad(kllvm, bPtr, "kb");
    llvm::Value* isLess, *isGreater;
    if (kllvm->isFloatingPointTy()) {
        isLess    = builder_.CreateFCmpOLT(av, bv, "flt");
        isGreater = builder_.CreateFCmpOGT(av, bv, "fgt");
    } else {
        isLess    = builder_.CreateICmpSLT(av, bv, "ilt");
        isGreater = builder_.CreateICmpSGT(av, bv, "igt");
    }
    llvm::Value* res = builder_.CreateSelect(
        isLess,    builder_.getInt32(-1),
        builder_.CreateSelect(isGreater, builder_.getInt32(1), builder_.getInt32(0)));
    builder_.CreateRet(res);
    builder_.restoreIP(savedIP);
    currentFn_ = savedFn;
    return fn;
}

// fix #3976 (stdlib §4.17/§4.18, v0.295): the ONE authority for "is this
// SortedSet<T>/SortedDictionary<K,V> element/key, already found ELIGIBLE by
// genericBackingType, actually orderable?" — genericBackingType's own
// auto-hashable-RECORD (recordTypeOfRef) and class/interface
// (hasObjectHeaderEquality) branches admit an element/key regardless of
// ordering (they exist to serve Set<T>/Dictionary<K,V>, which need only
// hashing); a SortedSet/SortedDictionary merely inherits them, so `elemOrKeyTy`
// reaching a call site as non-null does not by itself mean it has a
// dispatchable `CompareTo`. Fix #3947 added exactly this
// `needsOrdering && !hasOrdering` check + diagnostic, but only at the CallExpr
// member-dispatch site (tryLowerCollectionInstanceCall, LlvmBackend_Calls.cpp)
// — TWICE, once for the SortedSet Set-family arm and once for the
// SortedDictionary isDict arm, with the diagnostic text written out inline
// both times. Issue #3976 is explicitly the set of OTHER call sites that
// consult genericBackingType and were left silently admitting the same
// no-CompareTo shape: the indexer (LlvmBackend_Assign.cpp) and for-in
// iteration (LlvmBackend_Stmts.cpp). Factored out here — rather than a third
// and fourth inline copy of the same predicate and message — per this
// project's own standing rule against duplicated classification predicates
// (CLAUDE.md rule "the dominant defect generator here"): those two new call
// sites feed this ONE authority instead of re-deriving the decision.
//
// Deliberately NOT retrofitted onto #3947's own two call sites in
// LlvmBackend_Calls.cpp — out of this fix's scope (LANE_RULES "no while we're
// at it expansions"); they keep their own inline copies, unchanged.
//
// `recvTy` is the SortedSet<T>/SortedDictionary<K,V> receiver type (read only
// for `headName()` — NOT re-validated; every caller already knows which
// family it is in from its own dispatch). `elemOrKeyTy` is the
// ALREADY-`genericBackingType`-resolved, non-null element/key TypeRef.
// `memberDesc` names the rejected surface for the diagnostic text (e.g. "the
// `[key]` indexer", "`for`-in iteration"). Returns true — and reports a
// located Error identical in wording to #3947's own — exactly when
// `elemOrKeyTy` is eligible ONLY via the hash-only branches and has no
// dispatchable `CompareTo`; the caller must then skip the operation (a
// well-formed no-op — a poison value for an expression, no codegen at all for
// a for-loop) instead of lowering it.
bool Emitter::reportSortedElementNeedsCompareTo(const sema::TypeRef* recvTy,
                                                const sema::TypeRef* elemOrKeyTy,
                                                SourceLocation loc,
                                                const char* memberDesc) {
    if (!recvTy || !elemOrKeyTy) return false;
    bool isSet = recvTy->headName() == "SortedSet";
    bool isDict = recvTy->headName() == "SortedDictionary";
    if (!isSet && !isDict) return false;
    bool isRecord = recordTypeOfRef(elemOrKeyTy) != nullptr;
    bool needsOrdering = isRecord || hasObjectHeaderEquality(elemOrKeyTy);
    bool hasOrdering = isRecord
        ? recordHasDispatchableCompareTo(elemOrKeyTy)
        : buildObjectCompareToThunk(elemOrKeyTy) != nullptr;
    if (!needsOrdering || hasOrdering) return false;

    const sema::TypeRef* raw = resolveTypeParamRef(elemOrKeyTy);
    std::string name = isSet ? "the element type" : "the key type";
    if (raw && raw->kind == sema::TypeRef::Kind::Named && raw->declaration)
        name = "`" + std::string(raw->declaration->name) + "`";
    std::string reason =
        isRecord
            ? "it declares no dispatchable single-parameter `CompareTo`, and "
              "being auto-hashable (stdlib \xC2\xA7""9.2) is not sufficient — "
              "hashable and orderable are different properties"
            : "it declares no dispatchable single-argument `CompareTo` — a "
              "class/interface's identity or structural equality does not "
              "establish an ordering";
    std::string collName = isSet ? "SortedSet<T>" : "SortedDictionary<K, V>";
    std::string sectionRef = isSet ? "\xC2\xA7""4.18" : "\xC2\xA7""4.17";
    std::string constraintName = isSet ? "IComparable<T>" : "IComparable<K>";
    ctx_.diagnostics().report(
        Severity::Error, loc,
        std::string(memberDesc) + " on `" + collName + "` (stdlib " +
            sectionRef + ") requires " + (isSet ? "the element" : "the key") +
            " type to implement `" + constraintName + "`; " + name +
            " does not (" + reason + ").");
    return true;
}

llvm::Function* Emitter::buildLinqTrampoline(int kind, llvm::Type* elemTy,
                                             llvm::Type* outTy,
                                             const sema::TypeSymbol* elemRec,
                                             const sema::TypeRef* elemRef) {
    // Build a fresh internal function (each LINQ-operator site gets its own;
    // they are tiny and the closure type is baked into the IR).
    llvm::FunctionType* fty = nullptr;
    if (kind == 0)        // predicate: i32(ptr closure, ptr elem)
        fty = llvm::FunctionType::get(i32(), {ptrTy(), ptrTy()}, false);
    else if (kind == 1)   // selector: void(ptr closure, ptr elem, ptr out)
        fty = llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_),
                                      {ptrTy(), ptrTy(), ptrTy()}, false);
    else if (kind == 2)   // key: i64(ptr closure, ptr elem)
        fty = llvm::FunctionType::get(i64(), {ptrTy(), ptrTy()}, false);
    else                  // equality: i32(ptr a, ptr b) — no closure
        fty = llvm::FunctionType::get(i32(), {ptrTy(), ptrTy()}, false);

    auto* fn = llvm::Function::Create(fty, llvm::Function::InternalLinkage,
                                      "bz$linq$tramp", module_.get());
    // Save/restore the builder's insert point AND currentFn_ — buildLinqTrampoline
    // is called mid-lowering of the enclosing function, and helpers like
    // emitStringEq create basic blocks in currentFn_, which must be the
    // trampoline while we emit its body.
    auto savedIP = builder_.saveIP();
    llvm::Function* savedFn = currentFn_;
    currentFn_ = fn;
    auto* entry = llvm::BasicBlock::Create(llvm_, "entry", fn);
    builder_.SetInsertPoint(entry);

    if (kind == 3) {
        // Element equality (Distinct): content-aware compare of two elements.
        llvm::Value* a = builder_.CreateLoad(elemTy, fn->getArg(0), "eq.a");
        llvm::Value* b = builder_.CreateLoad(elemTy, fn->getArg(1), "eq.b");
        llvm::Value* eq;
        if (elemRec) {
            // issue D6 / §9.1 / §9.2: a record element dedups by synthesized
            // structural equality (the same machinery Set<record> uses), NOT an
            // ICmp on the aggregate (which LLVM rejects). String fields compare
            // by content, nested records recurse — consistent with the hash.
            eq = emitStructEquality(a, b, llvm::cast<llvm::StructType>(elemTy),
                                    elemRec);
        } else if (elemTy->isPointerTy()) {
            // fix #2773 / §8.3 / §8.4 / §4.6: `isPointerTy()` does NOT mean
            // "string" — a class, an interface, `Object` and a boxed value all
            // lower to `ptr` too. Decide by the sema TypeRef, following the
            // buildValueEqTrampoline / emitValueEquality precedent ("when
            // fieldTy is absent we do NOT assume ptr-means-string"). Only a
            // TypeRef that PROVES `string` may take the `%bz.string` layout
            // path; before this fix a class element ran loadStringLen/
            // loadStringData over the object's own bytes — a heap-buffer-
            // overflow (ASan: READ past the object) and a silently wrong
            // dedup verdict.
            //
            // Every other pointer element goes through lowerObjectEquals,
            // which is category-blind by construction: it decides at RUN time
            // (null → reference identity, a genuine string → content equality,
            // otherwise vtable[kEqualsSlot] → the class's `Equals` override or
            // §8.3's reference-identity default). That is exactly the contract
            // stdlib §5 states for these operators — "`Distinct`, `Union`,
            // `Intersect`, `Except`, `Contains`, and `ToSet` require
            // hashable `T` … exactly as for `Set<T>`" — and `Set<C>`'s
            // own membership scan (emitArrayBackedSetInsert) already honours a
            // user `Equals` override and otherwise falls back to reference
            // identity. It is also the safe default for a null/unresolved
            // elemRef: a string still compares by content there.
            const sema::TypeRef* er =
                elemRef ? resolveTypeParamRef(elemRef) : nullptr;
            const bool provenString =
                er && er->kind == sema::TypeRef::Kind::Primitive &&
                er->primitive == sema::PrimitiveType::String;
            if (provenString) {
                eq = emitStringEq(a, b);       // string content equality (§4.6)
            } else {
                eq = lowerObjectEquals(a, b);  // §8.3/§8.4 object equality
                // Defensive: lowerObjectEquals only declines on a non-pointer
                // operand, which cannot happen here. Reference identity is the
                // §8.3 default and is always memory-safe.
                if (!eq) eq = builder_.CreateICmpEQ(a, b);
            }
        } else {
            eq = builder_.CreateICmpEQ(a, b);  // value equality
        }
        builder_.CreateRet(builder_.CreateZExt(eq, i32()));
        builder_.restoreIP(savedIP);
        currentFn_ = savedFn;
        return fn;
    }

    llvm::Value* closure = fn->getArg(0);
    llvm::Value* elemPtr = fn->getArg(1);
    // Load the element value from the type-erased pointer.
    llvm::Value* elem = builder_.CreateLoad(elemTy, elemPtr, "tramp.elem");
    // Closure ABI: { ptr fn, ptr env }; call as fn(env, elem).
    llvm::Value* fnPtr = builder_.CreateLoad(ptrTy(), closure, "cl.fn");
    llvm::Value* envSlot = builder_.CreateGEP(ptrTy(), closure, {builder_.getInt32(1)});
    llvm::Value* envPtr = builder_.CreateLoad(ptrTy(), envSlot, "cl.env");

    if (kind == 1) {
        // Selector: call returns outTy; store it through the out pointer.
        auto* clTy = llvm::FunctionType::get(outTy, {ptrTy(), elemTy}, false);
        llvm::Value* mapped = builder_.CreateCall(clTy, fnPtr, {envPtr, elem}, "cl.ret");
        builder_.CreateStore(mapped, fn->getArg(2));
        builder_.CreateRetVoid();
    } else if (kind == 0) {
        // Predicate: closure returns i1; widen to i32 for the C ABI.
        auto* clTy = llvm::FunctionType::get(i1(), {ptrTy(), elemTy}, false);
        llvm::Value* ok = builder_.CreateCall(clTy, fnPtr, {envPtr, elem}, "cl.ret");
        builder_.CreateRet(builder_.CreateZExt(ok, i32()));
    } else {
        // Key: closure returns keyRetTy — defaults to `int` when the caller
        // passes no outTy (the OrderBy/ThenBy sort-key path, which coerces its
        // key lambda to `int` via lowerLinqLambda before reaching here); widen/
        // convert to i64 for the runtime's key-bytes ABI (bz_lazy_to_dict /
        // bz_lazy_orderby / bz_lazy_thenby all read an int64_t key).
        //
        // #1692 / stdlib §5: ToDictionary's TKey is not restricted to `int` —
        // it can be any hashable type (e.g. `string`), so the caller
        // now passes the selector's REAL return type as outTy. Hardcoding `i32`
        // here unconditionally called the closure through the wrong function
        // signature for a non-i32 key (a `ptr`-returning string selector),
        // corrupting the stored key. A pointer-sized key (string/class) is
        // converted via ptrtoint; a 64-bit integer key passes through
        // unchanged; anything narrower is sign-extended, matching the previous
        // (int-only) behavior for that case.
        llvm::Type* keyRetTy = outTy ? outTy : i32();
        auto* clTy = llvm::FunctionType::get(keyRetTy, {ptrTy(), elemTy}, false);
        llvm::Value* k = builder_.CreateCall(clTy, fnPtr, {envPtr, elem}, "cl.ret");
        llvm::Value* k64;
        if (keyRetTy->isPointerTy())
            k64 = builder_.CreatePtrToInt(k, i64());
        else if (keyRetTy->isIntegerTy(64))
            k64 = k;
        else
            k64 = builder_.CreateSExt(k, i64());
        builder_.CreateRet(k64);
    }
    builder_.restoreIP(savedIP);
    currentFn_ = savedFn;
    return fn;
}

// stdlib §5 Projection: SelectMany flatten-selector trampoline. The runtime
// calls this `void*(closure, elem)` per upstream element; it invokes the user
// selector `(T) -> Array<U>` (returns a collection pointer) and wraps that
// collection as a lazy enumerator the runtime then drains. innerEsz is U's size.
llvm::Function* Emitter::buildSelectManyTrampoline(llvm::Type* elemTy,
                                                   int64_t innerEsz,
                                                   bool innerIsLazy,
                                                   bool rangeInner,
                                                   bool rangeInnerIsChar,
                                                   bool groupingInner) {
    auto* fty = llvm::FunctionType::get(ptrTy(), {ptrTy(), ptrTy()}, false);
    auto* fn = llvm::Function::Create(fty, llvm::Function::InternalLinkage,
                                      "bz$linq$flatsel", module_.get());
    auto savedIP = builder_.saveIP();
    llvm::Function* savedFn = currentFn_;
    currentFn_ = fn;
    auto* entry = llvm::BasicBlock::Create(llvm_, "entry", fn);
    builder_.SetInsertPoint(entry);
    llvm::Value* closure = fn->getArg(0);
    llvm::Value* elem = builder_.CreateLoad(elemTy, fn->getArg(1), "fs.elem");
    // Closure ABI { ptr fn, ptr env }; call fn(env, elem) -> ptr (the collection).
    llvm::Value* fnPtr = builder_.CreateLoad(ptrTy(), closure, "fs.fn");
    llvm::Value* envSlot = builder_.CreateGEP(ptrTy(), closure, {builder_.getInt32(1)});
    llvm::Value* envPtr = builder_.CreateLoad(ptrTy(), envSlot, "fs.env");
    llvm::Value* inner;
    if (rangeInner) {
        // issue #775 / §16.3: the selector returns a `Range<T>` VALUE — a 32-byte
        // { i64 start, i64 end, i64 step, i8 flags } struct, NOT a collection
        // pointer. Call it via the struct-return ABI (LLVM lowers the sret
        // automatically), extract the fields, and wrap them as a lazy range
        // enumerator the runtime drains. (Open-ended selector ranges yield empty
        // via bz_lazy_from_range — the per-element trampoline has no unwind
        // context to raise InvalidOperationException; the receiver path does.)
        auto* rty = rangeStructType();
        auto* clTy = llvm::FunctionType::get(rty, {ptrTy(), elemTy}, false);
        llvm::Value* rv = builder_.CreateCall(clTy, fnPtr, {envPtr, elem}, "fs.rng");
        llvm::Value* start = builder_.CreateExtractValue(rv, {0u}, "fs.rs");
        llvm::Value* end   = builder_.CreateExtractValue(rv, {1u}, "fs.re");
        llvm::Value* step  = builder_.CreateExtractValue(rv, {2u}, "fs.rp");
        llvm::Value* flags = builder_.CreateExtractValue(rv, {3u}, "fs.rf");
        // §16.3 (v0.183, #1586): see the mirrored isRangeRecv branch in
        // lowerToLazyEnumerator for the bit3 char-flag convention.
        if (rangeInnerIsChar)
            flags = builder_.CreateOr(flags, builder_.getInt8(0x8), "fs.rf.char");
        auto* frFn = declareC("bz_lazy_from_range", ptrTy(),
            {i64(), i64(), i64(), llvm::Type::getInt8Ty(llvm_), i64()});
        inner = builder_.CreateCall(frFn,
            {start, end, step, flags, builder_.getInt64(innerEsz)}, "fs.inner.rng");
        builder_.CreateRet(inner);
        builder_.restoreIP(savedIP);
        currentFn_ = savedFn;
        return fn;
    }
    auto* clTy = llvm::FunctionType::get(ptrTy(), {ptrTy(), elemTy}, false);
    llvm::Value* coll = builder_.CreateCall(clTy, fnPtr, {envPtr, elem}, "fs.coll");
    if (innerIsLazy) {
        // stdlib §5 (#758): the selector returns a LAZY enumerable directly — a
        // LINQ chain such as `(n) => arr.Where(...)`, whose result is already a
        // `bz_lazy_t*` enumerator. Hand it back unchanged for the runtime to
        // drain. Wrapping it via `bz_lazy_from_array` (the concrete-collection
        // path below) would mis-read the enumerator struct as an array header
        // and yield only a garbage prefix (the WRONG_VALUE behind the IR_FAIL).
        inner = coll;
    } else {
        // stdlib §5 (#755, finding [20]): an `IGrouping<TKey, U>` result is a
        // bz_grouping_t*, whose FIRST field is the element bz_array_t* — read it
        // out before wrapping. Wrapping the grouping itself made bz_lazy read
        // { void* elements; int64 key_esz; … } as an array header, so the yielded
        // count was the KEY's byte size (4 per group for an int key) rather than
        // the group's element count. The wrap stays borrowing: the grouping owns
        // this array and frees it in bz_grouping_free.
        if (groupingInner) {
            auto* elemsFn = declareC("bz_grouping_elements", ptrTy(), {ptrTy()});
            coll = builder_.CreateCall(elemsFn, {coll}, "fs.grp.elems");
        }
        // Wrap the returned Array<U> as a lazy enumerator (the runtime drains it).
        auto* wrapFn = declareC("bz_lazy_from_array", ptrTy(), {ptrTy(), i64()});
        inner = builder_.CreateCall(wrapFn,
            {coll, builder_.getInt64(innerEsz)}, "fs.inner");
    }
    builder_.CreateRet(inner);
    builder_.restoreIP(savedIP);
    currentFn_ = savedFn;
    return fn;
}

// stdlib §5 Set-like (Union/Intersect/Except): hash trampoline for the
// membership dict. Matches the runtime's `uint64_t(*)(const void*)` function
// pointer slot in bz_dict_t. Reuses buildValueHashTrampoline for non-null
// elemRef (record / scalar / string — all hashable types). Falls back
// to a simple identity hash on the raw element bytes for pointer-sized elements
// (class pointers) where content equality is reference equality.
llvm::Function* Emitter::buildLinqHashTrampoline(llvm::Type* elemTy,
                                                  const sema::TypeRef* elemRef,
                                                  const sema::TypeSymbol* elemRec) {
    // For a hashable value type (record, primitive, enum) reuse the
    // existing value-hash trampoline (tested path; string hashes by content).
    if (elemRef && (elemRec || (elemRef->kind == sema::TypeRef::Kind::Primitive &&
                                elemRef->primitive != sema::PrimitiveType::Object)))
        return buildValueHashTrampoline(elemRef);
    // Fallback: hash the element bytes by loading the pointer-width value and
    // returning its bit pattern as a uint64. For a class pointer `T*` this is
    // identity hash (pointer value), consistent with reference equality.
    auto* fty = llvm::FunctionType::get(i64(), {ptrTy()}, false);
    auto* fn = llvm::Function::Create(fty, llvm::Function::InternalLinkage,
                                      "bz$linq$hash", module_.get());
    auto savedIP = builder_.saveIP();
    llvm::Function* savedFn = currentFn_;
    currentFn_ = fn;
    auto* entry = llvm::BasicBlock::Create(llvm_, "entry", fn);
    builder_.SetInsertPoint(entry);
    llvm::Value* v = builder_.CreateLoad(elemTy, fn->getArg(0), "hash.v");
    llvm::Value* h = nullptr;
    if (elemTy->isPointerTy()) {
        // fix #2773 / §8.3: a pointer-shaped element that is not an
        // hashable VALUE type is a class/interface/`Object` reference.
        // Its §8.3 hash is the user's `GetHashCode` override when declared,
        // else the ASSIGNED identity hash (bz_obj_identity_hash — "identity
        // hashes are assigned, not derived … never expose addresses"), NOT the
        // raw pointer bits. lowerObjectGetHashCode dispatches exactly that
        // (null → 0, a run-time string → its content hash, otherwise
        // vtable[kGetHashCodeSlot]), which is the hash side of the SAME
        // vtable pair the equality trampoline now dispatches through
        // (lowerObjectEquals → vtable[kEqualsSlot]). Keeping the pair together
        // is what makes §8.3's `a.Equals(b) ⇒ a.GetHashCode() = b.GetHashCode()`
        // hold inside the membership dict: before this fix the hash was
        // pointer identity while the equality trampoline content-compared the
        // object's bytes as a `%bz.string`, so the two disagreed *and* the
        // comparison was memory-unsafe.
        if (llvm::Value* oh = lowerObjectGetHashCode(v))
            h = builder_.CreateZExt(oh, i64());
        else
            h = builder_.CreatePtrToInt(v, i64());  // defensive (unreachable)
    } else if (elemTy->isIntegerTy(64)) {
        h = v;
    } else if (elemTy->isIntegerTy(32)) {
        h = builder_.CreateZExt(v, i64());
    } else {
        h = builder_.CreateZExtOrBitCast(v, i64());
    }
    builder_.CreateRet(h);
    builder_.restoreIP(savedIP);
    currentFn_ = savedFn;
    return fn;
}

// stdlib §5 Combination: Zip combine trampoline `void(closure, a, b, out)`.
// Loads the left (aTy) and right (bTy) elements, calls the closure
// `(env, a, b) -> outTy`, and stores the projected result through `out`.
llvm::Function* Emitter::buildZipTrampoline(llvm::Type* aTy, llvm::Type* bTy,
                                            llvm::Type* outTy) {
    auto* fty = llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_),
                                        {ptrTy(), ptrTy(), ptrTy(), ptrTy()}, false);
    auto* fn = llvm::Function::Create(fty, llvm::Function::InternalLinkage,
                                      "bz$linq$zip", module_.get());
    auto savedIP = builder_.saveIP();
    llvm::Function* savedFn = currentFn_;
    currentFn_ = fn;
    auto* entry = llvm::BasicBlock::Create(llvm_, "entry", fn);
    builder_.SetInsertPoint(entry);
    llvm::Value* closure = fn->getArg(0);
    llvm::Value* a = builder_.CreateLoad(aTy, fn->getArg(1), "zip.a");
    llvm::Value* b = builder_.CreateLoad(bTy, fn->getArg(2), "zip.b");
    llvm::Value* fnPtr = builder_.CreateLoad(ptrTy(), closure, "zip.fn");
    llvm::Value* envSlot = builder_.CreateGEP(ptrTy(), closure, {builder_.getInt32(1)});
    llvm::Value* envPtr = builder_.CreateLoad(ptrTy(), envSlot, "zip.env");
    auto* clTy = llvm::FunctionType::get(outTy, {ptrTy(), aTy, bTy}, false);
    llvm::Value* r = builder_.CreateCall(clTy, fnPtr, {envPtr, a, b}, "zip.r");
    builder_.CreateStore(r, fn->getArg(3));
    builder_.CreateRetVoid();
    builder_.restoreIP(savedIP);
    currentFn_ = savedFn;
    return fn;
}

// stdlib §5 (#651/#752): is `e` a `GroupBy(keySelector)` call — a CallExpr whose
// callee is a MemberAccessExpr with member "GroupBy" and exactly one argument?
bool Emitter::isGroupByCall(const ast::Expr* e) {
    auto* call = dynamic_cast<const ast::CallExpr*>(e);
    if (!call || !call->callee || call->args.size() != 1 || !call->args[0].value)
        return false;
    auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(call->callee.get());
    return ma && ma->member == "GroupBy" && ma->target;
}

llvm::Value* Emitter::lowerGroupByMaterialize(const ast::CallExpr& srcCall) {
    // The caller guarantees isGroupByCall(&srcCall).
    auto* srcMa = dynamic_cast<const ast::MemberAccessExpr*>(srcCall.callee.get());
    if (!srcMa || !srcMa->target) return nullptr;
    // srcMa->target is the source collection/lazy.
    auto* srcRecvTy = ctx_.exprType(*srcMa->target);
    sema::TypeRef* valElemRef =
        (srcRecvTy && !srcRecvTy->genericArgs.empty())
        ? srcRecvTy->genericArgs[0] : nullptr;
    llvm::Type* valElemTy = valElemRef ? lowerTypeRef(valElemRef) : i32();
    int64_t val_esz = lazyElemSize(valElemTy, module_->getDataLayout());
    // Determine key element type from the key lambda's return.
    sema::TypeRef* keyElemRef = valElemRef; // default: identity
    auto* callTy = ctx_.exprType(srcCall);
    // callTy = IEnumerable<IGrouping<TKey,T>>; IGrouping genericArgs[0]=TKey.
    if (callTy && !callTy->genericArgs.empty() &&
        callTy->genericArgs[0] &&
        callTy->genericArgs[0]->astNode &&
        callTy->genericArgs[0]->astNode->name &&
        !callTy->genericArgs[0]->astNode->name->parts.empty() &&
        callTy->genericArgs[0]->headName() == "IGrouping" &&
        callTy->genericArgs[0]->genericArgs.size() >= 1) {
        keyElemRef = callTy->genericArgs[0]->genericArgs[0];
    }
    llvm::Type* keyElemTy = keyElemRef ? lowerTypeRef(keyElemRef) : i32();
    int64_t key_esz = lazyElemSize(keyElemTy, module_->getDataLayout());
    // Lower the source to a lazy enumerator.
    sema::TypeRef* dummyElem = nullptr;
    llvm::Value* srcLazy = lowerToLazyEnumerator(*srcMa->target, dummyElem);
    if (!srcLazy) return nullptr;
    // Lower the key lambda to a closure. Use selector trampoline (kind 1):
    // void(cl, elem, out) so the key is written as bytes into the output slot.
    // §27.6 (#1758): own+release the key closure when it is an inline +1 producer
    // — a lambda literal OR a primitive-intrinsic method reference.
    const bool relCl =
        srcCall.args[0].value &&
        (dynamic_cast<const ast::LambdaExpr*>(srcCall.args[0].value.get()) != nullptr ||
         isPrimIntrinsicMethodRef(srcCall.args[0].value.get()));
    llvm::Value* closure = nullptr;
    {
        auto* funcRef = ctx_.makeTypeRef();
        funcRef->kind = sema::TypeRef::Kind::Function;
        funcRef->funcReturn = keyElemRef;
        if (valElemRef) {
            sema::TypeRef::FuncParam fp;
            fp.type = valElemRef;
            funcRef->funcParams.push_back(std::move(fp));
        }
        const sema::TypeRef* saved = expectedFunctionType_;
        expectedFunctionType_ = funcRef;
        closure = lowerExpr(*srcCall.args[0].value);
        expectedFunctionType_ = saved;
    }
    if (!closure) return nullptr;
    llvm::Function* keyTramp = buildLinqTrampoline(1, valElemTy, keyElemTy);
    auto* gbFn = declareC("bz_groupby_to_array", ptrTy(),
        {ptrTy(), i64(), i64(), ptrTy(), ptrTy()});
    llvm::Value* groups = builder_.CreateCall(gbFn,
        {srcLazy, builder_.getInt64(val_esz), builder_.getInt64(key_esz),
         closure, keyTramp}, "linq.groupby");
    if (relCl && closure) emitClosureRelease(closure);
    return groups;
}

// issue #995 / stdlib §5 / §29.11: materialize a VTABLE-BACKED `IEnumerable<T>`
// receiver (interface-typed local/param, or a yield-method result) into a fresh
// owned `Array<T>` (a bz_array_t* of element bytes) by driving the iterator
// vtable protocol ONCE in IR. The vtable layout is
// [0]=td,[1]=Dispose,[2]=MoveNext,[3]=Current,[4]=GetEnumerator (§29.11).
//
// Protocol (mirrors lowerIEnumerableBuiltinFor's vtable branch):
//   1. GetEnumerator (slot 4) on the source → a FRESH, INDEPENDENT enumerator.
//   2. Loop: MoveNext (slot 2) → i1; while true, Current (slot 3) → element BY
//      VALUE in its natural LLVM type; append the element's bytes to the array.
//   3. Dispose (slot 1) the enumerator exactly once after the drain.
//
// Disposal is exactly-once and leak-free:
//   - The per-consumption ENUMERATOR (the GetEnumerator clone) is disposed here.
//   - The SOURCE IEnumERABLE is NOT disposed here when it is a named
//     local/param/field — that value is disposed at its own scope end
//     (registerIterableSourceDisposal / registerUsingDisposal, §29.11). It IS
//     disposed here when it is a freshly produced temporary (a CallExpr/NewExpr
//     result with no owning binding, e.g. `P.Seq().Count()`) — nobody else owns
//     it. This mirrors lowerIEnumerableBuiltinFor's `sourceIsNamedLocal` rule,
//     so there is no double-free and no leak.
//
// The resulting Array<T> is a borrowing snapshot of the enumerated bytes (it
// owns no elements). The caller wraps it via bz_lazy_from_owned_array, so the
// lazy pipeline's dispose (a terminal's bz_lazy_to_array drain, or a
// Where/Select/… for-in) frees the array exactly once with a null disposer
// (leak-free, no double-free). Returns the array pointer, or null if the
// receiver is not a vtable-backed IEnumerable.
llvm::Value* Emitter::materializeVtableIEnumerableToArray(
    const ast::Expr& recvExpr, sema::TypeRef*& elemRef) {
    auto* recvTy = ctx_.exprType(recvExpr);
    if (!isVtableBackedIEnumerableType(recvTy)) return nullptr;
    elemRef = recvTy->genericArgs[0];
    llvm::Type* elemTy = lowerTypeRef(elemRef);
    if (!elemTy) return nullptr;

    llvm::Value* srcObj = lowerExpr(recvExpr);
    if (!srcObj) return nullptr;

    // Element byte size for the array (mirrors lazyElemSize / collectionElemSize).
    const llvm::DataLayout& dl = module_->getDataLayout();
    int64_t esz = 8;
    if (elemTy->isIntegerTy(32) || elemTy->isFloatTy()) esz = 4;
    else if (elemTy->isIntegerTy(1) || elemTy->isIntegerTy(8)) esz = 1;
    else if (elemTy->isIntegerTy(16)) esz = 2;
    else if (elemTy->isStructTy()) esz = (int64_t)dl.getTypeAllocSize(elemTy);

    // 1. GetEnumerator (slot 4): a fresh, independent enumerator over the source.
    llvm::Value* srcVt = builder_.CreateLoad(ptrTy(), srcObj);
    llvm::Value* geSlotPtr =
        builder_.CreateGEP(ptrTy(), srcVt, {builder_.getInt32(4)});
    llvm::Value* geFnPtr = builder_.CreateLoad(ptrTy(), geSlotPtr);
    auto* geTy = llvm::FunctionType::get(ptrTy(), {ptrTy()}, false);
    llvm::Value* en = builder_.CreateCall(geTy, geFnPtr, {srcObj}, "mat.getenum");

    // Allocate the result array (esz-byte elements).
    auto* allocFn = declareC("bz_array_alloc", ptrTy(), {i64(), i64()});
    llvm::Value* arr = builder_.CreateCall(
        allocFn, {builder_.getInt64(esz), builder_.getInt64(8)}, "mat.arr");

    auto* addFn = declareC("bz_array_add", llvm::Type::getVoidTy(llvm_),
                           {ptrTy(), ptrTy(), i64()});
    auto* moveNextTy = llvm::FunctionType::get(i1(), {ptrTy()}, false);
    auto* currentTy = llvm::FunctionType::get(elemTy, {ptrTy()}, false);

    // 2. Drain loop: while MoveNext(en) { append Current(en) to arr }.
    auto* condBB = llvm::BasicBlock::Create(llvm_, "mat.cond", currentFn_);
    auto* bodyBB = llvm::BasicBlock::Create(llvm_, "mat.body", currentFn_);
    auto* endBB  = llvm::BasicBlock::Create(llvm_, "mat.end",  currentFn_);
    builder_.CreateBr(condBB);

    builder_.SetInsertPoint(condBB);
    llvm::Value* vt = builder_.CreateLoad(ptrTy(), en);
    llvm::Value* mnSlot = builder_.CreateGEP(ptrTy(), vt, {builder_.getInt32(2)});
    llvm::Value* mnFn = builder_.CreateLoad(ptrTy(), mnSlot);
    llvm::Value* moved = builder_.CreateCall(moveNextTy, mnFn, {en}, "mat.moved");
    // §29.2/§31 (#630): MoveNext runs the iterator body; an exception thrown
    // there sets bz$exc and returns false. Surface it (the standard post-call
    // gate) instead of silently ending the drain. The enumerator/source are not
    // yet registered on the disposeStack_ here, but emitPendingCheck unwinds to
    // the enclosing handler; the small temporary leak on a thrown exception is
    // acceptable (no double-free) and matches the lazy-terminal materialize path.
    emitPendingCheck();
    builder_.CreateCondBr(moved, bodyBB, endBB);

    builder_.SetInsertPoint(bodyBB);
    llvm::Value* vt2 = builder_.CreateLoad(ptrTy(), en);
    llvm::Value* curSlot = builder_.CreateGEP(ptrTy(), vt2, {builder_.getInt32(3)});
    llvm::Value* curFn = builder_.CreateLoad(ptrTy(), curSlot);
    llvm::Value* curVal = builder_.CreateCall(currentTy, curFn, {en}, "mat.cur");
    // fix #2738 / §28.3 / §29.2 (v0.282): for a non-nullable string element,
    // the iterator's `yield return <+1 producer>` mints a fresh +1 reference
    // that the §29.2 row 3 enumerator-doesn't-dispose assumption relied on the
    // consumer to release. The for-in case takes the iterator as the sole
    // borrower — the iterator's current_val slot retains the +1 across the
    // iteration and the yield-store prev-dispose (#2738's primary fix) + the
    // iterator's Dispose release the +1 at the right times. The
    // vtable-materialize drain copies curVal's pointer into arr via
    // bz_array_add (which is a byte-copy, no retain), so the iterator's +1 is
    // the ONLY holder until the iterator disposes — but the iterator's yield-
    // store prev-dispose FREES the previous +1 before the next yield's store
    // overwrites the slot, and the iterator's Dispose FREES the last +1.
    // Without a balancing retain HERE, the previous N-1 curVal pointers in arr
    // (and the last one, after Dispose) are dangling when arr is later wrapped
    // as a lazy source and drained into the outer ToArray's result array. The
    // bz_array_retain_strings at the outer ToArray intercept (Calls3.cpp line
    // ~1709) retains each string by 1 — but for THIS materialize path, the
    // outer retain is also the over-count (#2738's second orphan: mat_arr is
    // freed at bz_lazy_dispose before the result array's lifetime ends, so
    // mat_arr's retains here are orphaned unless the outer retain is skipped
    // for this path). The conditional outer-retain skip below (line ~1718)
    // matches this conditional retain so the two stay in lockstep. Gated on
    // the same non-nullable string primitive predicate as the iterElemSemaTy
    // checks elsewhere so nullable strings / class elements / value primitives
    // are unaffected (they don't mint a +1 refcount on yield).
    if (elemRef &&
        elemRef->kind == sema::TypeRef::Kind::Primitive &&
        elemRef->primitive == sema::PrimitiveType::String &&
        !elemRef->nullable && elemTy->isPointerTy()) {
        emitStringRetain(curVal);
    }
    // Spill the by-value element to a stack slot and append its bytes.
    auto* elemSlot = entryAlloca(elemTy);
    builder_.CreateStore(curVal, elemSlot);
    builder_.CreateCall(addFn, {arr, elemSlot, builder_.getInt64(esz)});
    builder_.CreateBr(condBB);

    builder_.SetInsertPoint(endBB);
    // 3. Dispose the enumerator exactly once (slot 1 — iterator format, not the
    //    class vtable's slot 2). Runs the state machine's pending finally blocks
    //    (state → -1) and frees the frame.
    {
        llvm::Value* dvt = builder_.CreateLoad(ptrTy(), en);
        llvm::Value* dSlot =
            builder_.CreateGEP(ptrTy(), dvt, {builder_.getInt32(1)});
        llvm::Value* dFn = builder_.CreateLoad(ptrTy(), dSlot);
        auto* dispTy =
            llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_), {ptrTy()}, false);
        builder_.CreateCall(dispTy, dFn, {en});
    }
    // Dispose the SOURCE too when it is a freshly produced temporary (no owning
    // binding) — a named local/param/field source is disposed at its own scope
    // end, so disposing here would double-free.
    bool sourceIsNamed = false;
    if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(&recvExpr)) {
        sema::Symbol* sym = ctx_.exprSymbol(*id);
        sourceIsNamed = dynamic_cast<sema::LocalSymbol*>(sym) != nullptr ||
                        dynamic_cast<sema::ParameterSymbol*>(sym) != nullptr;
    } else if (dynamic_cast<const ast::MemberAccessExpr*>(&recvExpr)) {
        // A field/property read (`this.seq`, `obj.seq`) is owned by the object,
        // not this expression — do not dispose it here.
        sourceIsNamed = true;
    }
    if (!sourceIsNamed) {
        llvm::Value* svt = builder_.CreateLoad(ptrTy(), srcObj);
        llvm::Value* sSlot =
            builder_.CreateGEP(ptrTy(), svt, {builder_.getInt32(1)});
        llvm::Value* sFn = builder_.CreateLoad(ptrTy(), sSlot);
        auto* dispTy =
            llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_), {ptrTy()}, false);
        builder_.CreateCall(dispTy, sFn, {srcObj});
    }
    return arr;
}

llvm::Value* Emitter::lowerToLazyEnumerator(const ast::Expr& recvExpr,
                                            sema::TypeRef*& elemRef,
                                            bool ownFreshGroupings,
                                            llvm::Value** outRecvArr) {
    if (outRecvArr) *outRecvArr = nullptr;
    auto* recvTy = ctx_.exprType(recvExpr);
    // stdlib §5 (#752): a bare `GroupBy(...)` is IEnumerable<IGrouping<TKey,T>>
    // (so isLazyEnumerableType below is true), but lowerExpr cannot lower it to
    // a usable enumerator — only the `.ToArray()` interception handled GroupBy,
    // so Count()/Any()/Sum()/for-in over a bare GroupBy enumerated garbage.
    // Materialize the groups into an owned Array<IGrouping> and wrap it as a
    // lazy ARRAY source: each element is a bz_grouping_t* pointer, so the lazy
    // element size is one pointer. Now every lazy consumer composes uniformly.
    if (isGroupByCall(&recvExpr)) {
        llvm::Value* groups =
            lowerGroupByMaterialize(*static_cast<const ast::CallExpr*>(&recvExpr));
        if (!groups) return nullptr;
        elemRef = (recvTy && !recvTy->genericArgs.empty())
                  ? recvTy->genericArgs[0] : nullptr;
        int64_t esz = lazyElemSize(ptrTy(), module_->getDataLayout());
        // #1914: the groups array + groupings are a fresh temporary no name owns.
        // A DRAIN-then-DISPOSE consumer that reads each grouping only during
        // iteration (the for-in loop, ownFreshGroupings=true) owns and disposes
        // them via bz_lazy_from_owned_grouping_array — fixing the leak (selector
        // closure is already released in lowerGroupByMaterialize; this reclaims
        // the array + every grouping's element array/key/object). All other
        // consumers keep the borrowing bz_lazy_from_array: ToArray re-owns the
        // groupings through its produced Array<IGrouping> local (owning here would
        // double-free), and a scalar terminal (First/Aggregate) may read a
        // grouping pointer from the post-dispose snapshot (owning here would UAF).
        const char* wrapName = ownFreshGroupings
            ? "bz_lazy_from_owned_grouping_array" : "bz_lazy_from_array";
        auto* wrapFn = declareC(wrapName, ptrTy(), {ptrTy(), i64()});
        return builder_.CreateCall(wrapFn, {groups, builder_.getInt64(esz)},
                                   "lazy.groupby");
    }
    // issue #995 / stdlib §5 / §29.11: a VTABLE-BACKED `IEnumerable<T>` receiver
    // (interface-typed local/param/field, or a yield-method result) carries the
    // iterator-vtable object, not a bz_array_t*/bz_lazy_t*. Drive its vtable
    // protocol once to materialize the elements into a fresh Array<T> (disposing
    // the per-consumption enumerator, and the source iff it is a temporary), then
    // wrap that array as a lazy ARRAY source so every lazy consumer composes
    // uniformly. Without this the receiver fell through to bz_lazy_from_array over
    // the raw vtable object pointer → garbage (the #995 silent miscompile).
    if (isVtableBackedIEnumerableType(recvTy)) {
        llvm::Value* matArr =
            materializeVtableIEnumerableToArray(recvExpr, elemRef);
        if (!matArr) return nullptr;
        llvm::Type* et = elemRef ? lowerTypeRef(elemRef) : i32();
        int64_t esz = lazyElemSize(et, module_->getDataLayout());
        // The materialized array is a fresh temporary with no other owner — wrap
        // it as an OWNING lazy ARRAY source so bz_lazy_dispose frees it (leak-free
        // for both the terminal drain and the Where/Select/… for-in consumers).
        auto* wrapFn =
            declareC("bz_lazy_from_owned_array", ptrTy(), {ptrTy(), i64()});
        return builder_.CreateCall(wrapFn, {matArr, builder_.getInt64(esz)},
                                   "lazy.ienum");
    }
    llvm::Value* recv = lowerExpr(recvExpr);
    if (!recv) return nullptr;
    // stdlib §5 (#755): an `IGrouping<TKey, T>` source. `IGrouping : IEnumerable<T>`
    // (stdlib §5 line 3076), so a grouping is a legal LINQ receiver and a legal
    // `for` source (spec §29.2) — but its runtime value is a `bz_grouping_t*`,
    // not a bz_array_t*/bz_lazy_t*, so every enumerable consumer previously read
    // the grouping struct as whatever shape it expected and saw an EMPTY (or
    // garbage) sequence. Its members are a concrete, fully-materialized
    // `bz_array_t*` reachable via bz_grouping_elements; wrap THAT as a lazy ARRAY
    // source. The wrap is BORROWING (bz_lazy_from_array): the grouping owns its
    // element array and frees it in bz_grouping_free, so an owning wrap would
    // double-free. Element type is genericArgs[1] (genericArgs[0] is TKey).
    if (const sema::TypeRef* grpElem = iGroupingElemTypeRef(recvTy)) {
        elemRef = const_cast<sema::TypeRef*>(grpElem);
        llvm::Type* et = lowerTypeRef(grpElem);
        if (!et) et = i32();
        int64_t esz = lazyElemSize(et, module_->getDataLayout());
        auto* elemsFn = declareC("bz_grouping_elements", ptrTy(), {ptrTy()});
        llvm::Value* arr =
            builder_.CreateCall(elemsFn, {recv}, "igrp.elems");
        // Deliberately NOT surfaced through outRecvArr: that out-param exists so
        // a consuming caller can reclaim an UNBOUND fresh-materializer array, and
        // the grouping's array has an owner (the grouping).
        auto* wrapFn = declareC("bz_lazy_from_array", ptrTy(), {ptrTy(), i64()});
        return builder_.CreateCall(wrapFn, {arr, builder_.getInt64(esz)},
                                   "lazy.igrp");
    }
    // fix #1220 (residual) / stdlib §4.8 / §5: Dictionary<K,V> LINQ source.
    // Materialize the dictionary's entries into a fresh owned bz_array_t of
    // {K,V} structs (same layout as lowerDictEntryFor), then wrap it as an
    // owning lazy array source. `recv` is the dict ptr (already lowered above).
    if (isDictTypeName(recvTy) && recvTy && recvTy->genericArgs.size() >= 2 &&
        recvTy->genericArgs[0] && recvTy->genericArgs[1]) {
        const sema::TypeRef* keyTy = recvTy->genericArgs[0];
        const sema::TypeRef* valTy = recvTy->genericArgs[1];

        // fix #3758 (§4.8 / §9.x, v0.286): `valRec` still selects a debug-name
        // prefix below. It and the OLD `valNullableVal && isStructTy()` arm
        // were merged into a single `isStructTy()` predicate a few lines down
        // — see that decode's comment: #3754 named this arm as one of the two
        // copies NOT routed through decodeDictValueAt, and this lane's brief
        // permits applying the SAME one-line predicate widening in place
        // without extracting a shared helper here (that unification is
        // #3754's job).
        const sema::TypeSymbol* valRec =
            (valTy->kind == sema::TypeRef::Kind::Named &&
             valTy->declaration &&
             valTy->declaration->kind == ast::TypeDeclKind::Record)
                ? valTy->declaration : nullptr;

        llvm::Type* keyLlvm = lowerTypeRef(keyTy);
        llvm::Type* valLlvm = lowerTypeRef(valTy);
        if (keyLlvm && valLlvm) {
            llvm::StructType* entryStTy =
                llvm::StructType::get(llvm_, {keyLlvm, valLlvm});
            const llvm::DataLayout& dl = module_->getDataLayout();
            int64_t esz = (int64_t)dl.getTypeAllocSize(entryStTy);

            bool useGen = genericBackingType(recvTy, 0, /*isSet=*/false, nullptr) != nullptr;
            using K = sema::TypeRef::Kind;
            bool isStringKey = keyTy->kind == K::Primitive &&
                               keyTy->primitive == sema::PrimitiveType::String;

            // Build element TypeRef for the lazy pipeline's element tracking.
            {
                auto* entryTy = ctx_.makeTypeRef();
                entryTy->kind = sema::TypeRef::Kind::Named;
                entryTy->declaration = nullptr;
                entryTy->astNode = nullptr;
                static const char kDictEntryMat[] = "DictionaryEntry";
                entryTy->importedName = kDictEntryMat;
                entryTy->genericArgs.push_back(
                    const_cast<sema::TypeRef*>(keyTy));
                entryTy->genericArgs.push_back(
                    const_cast<sema::TypeRef*>(valTy));
                elemRef = entryTy;
            }

            // --- snapshot keys + values ---
            auto* keysOutSlot = entryAlloca(ptrTy());
            auto* lensOutSlot = entryAlloca(ptrTy());
            auto* widsOutSlot = entryAlloca(ptrTy());   // fix #4499: §4.6 widths
            auto* cntSlot     = entryAlloca(i64());
            auto* valsOutSlot = entryAlloca(ptrTy());

            if (useGen) {
                auto* kfn = declareC("bz_dict_keys_gen",
                    llvm::Type::getVoidTy(llvm_), {ptrTy(), ptrTy(), ptrTy()});
                builder_.CreateCall(kfn, {recv, keysOutSlot, cntSlot});
                auto* vfn = declareC("bz_dict_values_gen",
                    llvm::Type::getVoidTy(llvm_), {ptrTy(), ptrTy(), ptrTy()});
                builder_.CreateCall(vfn, {recv, valsOutSlot, entryAlloca(i64())});
            } else {
                auto* kfn = declareC("bz_dict_keys_str",
                    llvm::Type::getVoidTy(llvm_),
                    {ptrTy(), ptrTy(), ptrTy(), ptrTy(), ptrTy()});
                builder_.CreateCall(kfn,
                    {recv, keysOutSlot, lensOutSlot, widsOutSlot, cntSlot});
                auto* vfn = declareC("bz_dict_values_str",
                    llvm::Type::getVoidTy(llvm_),
                    {ptrTy(), ptrTy(), ptrTy()});
                builder_.CreateCall(vfn, {recv, valsOutSlot, entryAlloca(i64())});
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

            // fix #3511 / stdlib §4.17 (v0.285): this materializer is reached for
            // EVERY bz_dict_t*-backed receiver, `SortedDictionary<K, V>` included
            // — `isDictTypeName` is the #1414 registry's three-name set. The
            // snapshot above is in STORAGE order, which is right for §4.8's
            // insertion-ordered `Dictionary` and WRONG for §4.17's "iteration
            // visits entries in ascending key order". Sort keys and values
            // together here, exactly as `wrapSortedDictionaryAsIEnumerable`
            // (LlvmBackend_Calls2.cpp) does for the for-in / GetEnumerator path,
            // so the §5 chain over a sorted map yields the same sequence the bare
            // `for var e in sd` does.
            //
            // Why it matters that this is a VALUE bug and not only a rejection:
            // sema alone (the #3511 clobber fix) makes `sd.Take(1)` COMPILE, and
            // without this arm it then silently yields whichever entry the hash
            // table happens to store first — measured key 3 for a map built
            // `Add(3,·); Add(1,·); Add(2,·)`, where §4.17 requires key 1. A loud
            // reject would have become a quiet wrong answer.
            if (builtinTypeName(recvTy) == "SortedDictionary") {
                // Branch on `useGen` — the SAME flag that chose the snapshot
                // above — AND require `lensArr`, which is non-null exactly when
                // `isStringKey`. The sort must consume the arrays in the shape
                // they were actually filled: `bz_dict_keys_str` is what populates
                // `lensOutSlot`, and `bz_sort_kv_str` dereferences it. The two
                // flags agree in every case reachable today, so the conjunction
                // is belt-and-braces rather than a live distinction — it is
                // deliberately STRICTER than the `wrapSortedDictionaryAsIEnumerable`
                // spelling it mirrors (LlvmBackend_Calls2.cpp), which derives
                // `useGen = !isStringKey` and passes `lensArr` unguarded.
                if (!useGen && lensArr) {
                    auto* sortFn = declareC(
                        "bz_sort_kv_str", llvm::Type::getVoidTy(llvm_),
                        {ptrTy(), ptrTy(), ptrTy(), ptrTy(), i64()});
                    // fix #4499: the width spine permutes with the others.
                    builder_.CreateCall(sortFn,
                        {keysArr, lensArr, widsArr, valsArr, cnt});
                } else if (useGen) {
                    // A key type `buildSortedKeyComparator` cannot rank returns
                    // null; leaving the snapshot unsorted then matches what the
                    // for-in path already does for that same key type (#1426),
                    // rather than inventing a second answer here.
                    if (llvm::Function* cmpFn = buildSortedKeyComparator(keyTy)) {
                        auto* sortFn = declareC(
                            "bz_sort_kv_gen", llvm::Type::getVoidTy(llvm_),
                            {ptrTy(), ptrTy(), i64(), ptrTy()});
                        builder_.CreateCall(sortFn,
                                            {keysArr, valsArr, cnt, cmpFn});
                    }
                }
            }

            // Allocate the result bz_array_t.
            auto* allocFn = declareC("bz_array_alloc", ptrTy(), {i64(), i64()});
            llvm::Value* arr = builder_.CreateCall(
                allocFn,
                {builder_.getInt64(esz), builder_.getInt64(8)},
                "dmat.arr");

            auto* addFn = declareC("bz_array_add", llvm::Type::getVoidTy(llvm_),
                                   {ptrTy(), ptrTy(), i64()});
            auto* freeFn = declareC("free", llvm::Type::getVoidTy(llvm_),
                                    {ptrTy()});

            // Decode loop — build entry structs and append to arr.
            auto* idxSlot = entryAlloca(i64());
            builder_.CreateStore(builder_.getInt64(0), idxSlot);
            auto* entrySlot = entryAlloca(entryStTy);

            auto* condBB = llvm::BasicBlock::Create(llvm_, "dmat.cond", currentFn_);
            auto* bodyBB = llvm::BasicBlock::Create(llvm_, "dmat.body", currentFn_);
            auto* endBB  = llvm::BasicBlock::Create(llvm_, "dmat.end",  currentFn_);
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

                // --- decode key (fix #3754 / narrow #3717 / §4.8 / §28.9:
                // routed through the shared decodeDictStringKeyAt /
                // decodeDictGenericKeyAt helpers, declared in
                // LlvmBackend_impl.hpp and defined beside lowerDictEntryFor
                // in LlvmBackend_Stmts.cpp. This materializer frees the raw
                // key pointer IMMEDIATELY below — unlike the for-in loop's
                // deferred incBB release, this loop has no increment block —
                // unchanged from before this fix. The third branch (neither
                // string nor generic-backed) mirrors lowerDictEntryFor's own
                // "should not happen" null fallback verbatim.) ---
                llvm::Value* keyDecoded;
                if (isStringKey) {
                    auto strPr =
                        decodeDictStringKeyAt(keysArr, lensArr, widsArr, idxB);
                    keyDecoded = strPr.first;
                    builder_.CreateCall(freeFn, {strPr.second});
                } else if (useGen) {
                    auto genPr = decodeDictGenericKeyAt(
                        keyLlvm, keysArr, idxB, "dmat.kgen");
                    keyDecoded = genPr.first;
                    builder_.CreateCall(freeFn, {genPr.second});
                } else {
                    keyDecoded = llvm::Constant::getNullValue(keyLlvm);
                }

                // --- decode value (fix #3754: routed through
                // decodeDictValueAt; fix #3758 already derives the
                // struct-vs-scalar decision from valLlvm itself inside that
                // helper, so this call is behaviour-identical to the inline
                // isStructTy() check it replaces) ---
                llvm::Value* valDecoded = decodeDictValueAt(
                    valLlvm, valsArr, idxB, valRec ? "dmat.vrec" : "dmat.vnv");

                // Store key at field 0, value at field 1 and append.
                builder_.CreateStore(
                    keyDecoded,
                    builder_.CreateStructGEP(entryStTy, entrySlot, 0));
                builder_.CreateStore(
                    valDecoded,
                    builder_.CreateStructGEP(entryStTy, entrySlot, 1));
                builder_.CreateCall(addFn,
                    {arr, entrySlot, builder_.getInt64(esz)});
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

            // #2544 (stdlib §4.8/§4.12; spec §29.1, v0.236): for string-keyed
            // dictionaries the rebuilt bz_string keys at struct offset 0 are
            // +1 ref-counted producers (initStringRc → rc=1) and need
            // bz_string_release when the lazy source is disposed. The plain
            // bz_lazy_from_owned_array calls bz_array_free(arr, null) — a null
            // element disposer that never reaches the string at struct offset 0
            // (it is for pointer-element arrays only). bz_lazy_from_owned_str_dict_array
            // is the sibling of bz_lazy_from_owned_grouping_array (#1914) and
            // sets arr_str_keys := 1 so bz_lazy_dispose walks the array and
            // releases each Key before freeing the array. Non-string-keyed
            // dictionaries (int/record/enum keys) keep the plain wrapper — their
            // keys are inline bytes with no heap ownership, so the null-disposer
            // free is correct.
            const char* ownName = isStringKey
                ? "bz_lazy_from_owned_str_dict_array"
                : "bz_lazy_from_owned_array";
            auto* ownFn = declareC(ownName, ptrTy(), {ptrTy(), i64()});
            return builder_.CreateCall(ownFn,
                {arr, builder_.getInt64(esz)}, "lazy.dict");
        }
    }
    // issue #775 / §16.3: a `Range<T>` is IEnumerable<T>. Wrap its value struct
    // { i64 start, i64 end, i64 step, i8 flags } as a lazy range source so the
    // receiver-position operators (Count/Where/Select/Sum/…) enumerate it. An
    // open-ended range (flags bit1/bit2) raises InvalidOperationException per
    // §16.3 (GetEnumerator on an open-ended range).
    // A Range-typed receiver is recognized by name: in receiver position its
    // TypeSymbol carries the builtin Range class decl (so isRangeType's
    // `!declaration->decl` guard is false), but the lowered value is still the
    // `%bz.range` { i64 start, i64 end, i64 step, i8 flags } struct.
    bool isRangeRecv = recvTy && recvTy->kind == sema::TypeRef::Kind::Named &&
                       recvTy->declaration &&
                       recvTy->declaration->name == "Range";
    if (isRangeRecv) {
        elemRef = (recvTy && !recvTy->genericArgs.empty())
                      ? recvTy->genericArgs[0] : nullptr;
        llvm::Type* et = elemRef ? lowerTypeRef(elemRef) : i32();
        int64_t esz = lazyElemSize(et, module_->getDataLayout());
        llvm::Value* start = builder_.CreateExtractValue(recv, {0u}, "rng.start");
        llvm::Value* end   = builder_.CreateExtractValue(recv, {1u}, "rng.end");
        llvm::Value* step  = builder_.CreateExtractValue(recv, {2u}, "rng.step");
        llvm::Value* flags = builder_.CreateExtractValue(recv, {3u}, "rng.flags");
        // §16.3 (v0.183, #1586): flag the enumerator as char-stepping via a
        // spare bit (bit3 — bits 0-2 are IsEndInclusive/startOpen/endOpen,
        // §16.3) so bz_lazy_movenext's BZ_LAZY_RANGE case skips the UTF-16
        // surrogate gap instead of stepping by plain arithmetic. This is the
        // GetEnumerator()/LINQ-receiver path; lowerRangeFor's counted-loop
        // `for` optimization mirrors it via emitCharRangeStep.
        bool isCharRange = elemRef &&
            elemRef->kind == sema::TypeRef::Kind::Primitive &&
            elemRef->primitive == sema::PrimitiveType::Char;
        if (isCharRange)
            flags = builder_.CreateOr(flags, builder_.getInt8(0x8), "rng.flg.char");
        llvm::Value* isOpen = builder_.CreateICmpNE(
            builder_.CreateAnd(flags, builder_.getInt8(6)),
            builder_.getInt8(0), "rng.open");
        emitGuardThrow(isOpen, "InvalidOperationException");
        auto* fn = declareC("bz_lazy_from_range", ptrTy(),
            {i64(), i64(), i64(), llvm::Type::getInt8Ty(llvm_), i64()});
        return builder_.CreateCall(fn,
            {start, end, step, flags, builder_.getInt64(esz)}, "lazy.range");
    }
    if (isLazyEnumerableType(recvTy)) {
        // Already a lazy enumerator (chained LINQ): use it directly.
        elemRef = recvTy->genericArgs[0];
        return recv;
    }
    // fix #4207 (stdlib §5 / §12.9): an EXPLICITLY-NAMED
    // `IOrderedEnumerable<T>` receiver — a named local/param/field with explicit
    // type annotation, or a cross-module value. `IOrderedEnumerable<T> : IEnumerable<T>`
    // (stdlib §5 line 3592) and the OrderBy/OrderByDescending result interface.
    // Its runtime representation is the SAME `bz_lazy_t*` as the lazy sentinel
    // above — it IS a lazy enumerator, just named. Use it directly; element type
    // is at genericArgs[0].  Not isLazyEnumerableType: that predicate requires
    // declaration-less (no user TypeDecl, no astNode, no importedName — the exact
    // shape of the anonymous lazy sentinel), while a named IOrderedEnumerable has
    // an astNode (from the local annotation) or importedName (cross-module).
    if (recvTy && recvTy->kind == sema::TypeRef::Kind::Named &&
        !recvTy->declaration &&
        builtinTypeName(recvTy) == "IOrderedEnumerable" &&
        !recvTy->genericArgs.empty() && recvTy->genericArgs[0]) {
        elemRef = recvTy->genericArgs[0];
        return recv;
    }
    // issue #389 / stdlib §5 / §4.10: a dict-backed Set<T> is IEnumerable<T>
    // (Set: ICollection<T>: IEnumerable<T>) but its backing pointer is a
    // bz_dict_t*, not a bz_array_t* — wrap it via the Set lazy source, which
    // snapshots the element keys. Only the generic dict-backed Sets (record /
    // primitive / enum / string elements) need this; a plain class-element
    // OwningSet is array-backed and takes the ARRAY path below.
    //
    // fix #2726 / stdlib §4.18 (v0.278): admit SortedSet alongside Set/OwningSet
    // here — its runtime IS the bz_dict_t* (the same `bz_dict_alloc_generic`
    // lowerNew mints for Set/OwningSet, with sorted-ordering presentation
    // layered on, #1426). Without SortedSet in this predicate, a
    // `sortedSet.ToArray()` — or `new LinkedList<T>(sortedSet)`
    // (Exprs2.cpp:3965) — fell through to `bz_lazy_from_array` over a
    // `bz_dict_t*`: a silent misread where `bz_array_t`/`bz_dict_t`
    // struct-prefix coincidence aliased the count slot but `bz_array_ptr_at`
    // read `bz_entry_t` header bytes as element slots. Narrow residue of the
    // broader Set-family thesis (the prior registry move regressed 30+ tests,
    // per-fix-review verdict); that fix widened ONLY this one predicate,
    // mirroring the SortedSet membership already in Calls.cpp:2238,
    // Calls2.cpp:6231, Exprs.cpp:4364, and Setup.cpp:15101 / 16034 / 23682.
    elemRef = (recvTy && !recvTy->genericArgs.empty()) ? recvTy->genericArgs[0]
                                                       : nullptr;
    if (recvTy && recvTy->astNode && recvTy->astNode->name &&
        !recvTy->astNode->name->parts.empty() &&
        (recvTy->headName() == "Set" ||
         recvTy->headName() == "OwningSet" ||
         recvTy->headName() == "SortedSet")) {
        uint64_t ksz = 0;
        if (genericBackingType(recvTy, 0, /*isSet=*/true, &ksz)) {
            // fix #3711 (stdlib §4.18 / §5, v0.286): a SortedSet<T> receiver
            // must NOT take `bz_lazy_from_set`. That helper snapshots the
            // element keys with `bz_dict_keys_gen` and enumerates them in the
            // hash table's bucket order — correct for `Set`/`OwningSet`, whose
            // §4.10 surface promises no order at all, but a direct violation of
            // stdlib §4.18's "iterates its elements in ASCENDING order" for
            // `SortedSet<T>`. #2726 admitted SortedSet to this arm to stop the
            // bz_dict_t*-as-bz_array_t* misread and gave it the Set helper,
            // which fixed the COUNT and the element bytes but left the ORDER
            // wrong: measured on `new LinkedList<int>(s)` and
            // `a.Concat(s).ToArray()` for a SortedSet inserted 30,10,20,40, the
            // §5 path yielded 30,10,20,40 while `for var x in s` over the very
            // same set yielded 10,20,30,40 — the two surfaces disagreeing, the
            // exact property #2471 / #2472 established they must not.
            //
            // Feed the ONE existing sorted-snapshot authority instead
            // (`sortedSetSnapshotToSortedArray`, the snapshot half of
            // `wrapSortedSetAsIEnumerable` — the helper the §29.2 for-in and
            // explicit-`GetEnumerator()` surfaces already use), and wrap its
            // result with `bz_lazy_from_owned_array`. That wrap is the exact
            // ownership shape the snapshot needs and the same one
            // `bz_lazy_from_set` provided: the lazy owns the fresh array and
            // frees it once at `bz_lazy_dispose` with a NULL element disposer,
            // borrowing every element (a string slot holds the SortedSet's own
            // +1, which the set releases at its own scope end). Only the
            // element ORDER changes; `Set`/`OwningSet` keep `bz_lazy_from_set`
            // verbatim, and the two guards admitting this arm are unchanged, so
            // no receiver that reached one lowering now reaches a different one.
            if (recvTy->headName() == "SortedSet") {
                int64_t ssz = 0;
                if (llvm::Value* sortedArr =
                        sortedSetSnapshotToSortedArray(recv, recvTy, &ssz)) {
                    auto* ownFn = declareC("bz_lazy_from_owned_array", ptrTy(),
                                           {ptrTy(), i64()});
                    return builder_.CreateCall(
                        ownFn, {sortedArr, builder_.getInt64(ssz)},
                        "lazy.sortedset");
                }
                // The snapshot helper only declines when the element type has
                // no LLVM lowering — the same shape that would make every arm
                // below malformed. Fall through to the Set helper rather than
                // returning null: an unordered sequence is strictly better than
                // the #2434 zero-value degrade, and this arm is not reachable
                // for any element `genericBackingType` has just admitted.
            }
            auto* setFn = declareC("bz_lazy_from_set", ptrTy(), {ptrTy(), i64()});
            return builder_.CreateCall(setFn,
                {recv, builder_.getInt64((int64_t)ksz)}, "lazy.set");
        }
    }
    // per §4.6/§4.7 + §5 fix #450: LinkedList<T>/OwningLinkedList<T> is
    // IEnumerable<T> but backed by bz_llist_t*, not bz_array_t*.  Wrap via
    // bz_lazy_from_llist so LINQ operators can enumerate it correctly.
    if (isLinkedListTypeName(recvTy)) {
        int64_t esz = collectionElemSize(recvTy);
        auto* fn = declareC("bz_lazy_from_llist", ptrTy(), {ptrTy(), i64()});
        return builder_.CreateCall(fn, {recv, builder_.getInt64(esz)}, "lazy.llist");
    }
    // fix #1220 / §5 / §9.6: string is IEnumerable<char>. Materialize it into
    // a fresh owned Array<char> (elem_size=4, one i32 code-point per slot) via
    // bz_str_to_char_array (already lowered to `recv` above), then wrap as an
    // OWNING lazy ARRAY source so the lazy pipeline (Where/Select/Count/…) owns
    // and frees it on drain. `recv` is the string pointer (already evaluated).
    if (recvTy && recvTy->kind == sema::TypeRef::Kind::Primitive &&
        recvTy->primitive == sema::PrimitiveType::String) {
        auto* toArrFn = declareC("bz_str_to_char_array", ptrTy(), {ptrTy()});
        llvm::Value* charArr =
            builder_.CreateCall(toArrFn, {recv}, "str.chararray");
        // Element type for char is i32 (code point); size = 4.
        elemRef = ctx_.primitiveTypeRef(sema::PrimitiveType::Char);
        auto* ownFn =
            declareC("bz_lazy_from_owned_array", ptrTy(), {ptrTy(), i64()});
        return builder_.CreateCall(ownFn,
            {charArr, builder_.getInt64(4)}, "lazy.str");
    }
    // fix #449: only Array<T>-family (isCollectionTypeName) collections are
    // bz_array_t*-backed.  Do NOT fall through for an unrecognised receiver type
    // (e.g. the i32(0) produced by in-place Array.Reverse()) — that would pass
    // garbage as a pointer to bz_lazy_from_array, causing SIGABRT.
    if (!isCollectionTypeName(recvTy)) return nullptr;
    // Concrete collection (Array<T> family): wrap it as a lazy ARRAY source.
    int64_t esz = collectionElemSize(recvTy);
    auto* wrapFn = declareC("bz_lazy_from_array", ptrTy(), {ptrTy(), i64()});
    // #2151 / §28.3: surface the borrowed array through the out-param so callers
    // that CONSUME the lazy (ToDictionary, etc., which route through
    // bz_lazy_dispose) can still reclaim an unbound fresh-materializer
    // receiver at the call site — the lazy wrap itself is a borrow, so
    // bz_lazy_dispose leaves the array alive for the caller.
    if (outRecvArr) *outRecvArr = recv;
    return builder_.CreateCall(wrapFn, {recv, builder_.getInt64(esz)}, "lazy.src");
}

llvm::Value* Emitter::tryLowerLazyLinqCall(const ast::CallExpr& call) {
    if (!call.callee) return nullptr;
    auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(call.callee.get());
    if (!ma || !ma->target) return nullptr;
    const auto& m = ma->member;

    // Only intercept on a collection, lazy-enumerable, or LinkedList receiver.
    auto* recvTy = ctx_.exprType(*ma->target);

    // fix #1476 / stdlib §4.10: OwningSet<T>.Take(query: T): owned T (and its
    // TryTake analogue) share a method name with — but are NOT — the LINQ
    // Take(count: int) operator every array-backed collection exposes below.
    // Both take exactly one positional arg, so without this guard
    // isCollectionTypeName(OwningSet) + the unconditional `m == "Take"` match
    // in `isOp` further down intercepted the call, materialized `s` into a
    // lazy enumerator, and treated the class-typed query pointer as an int
    // count — `builder_.CreateSExt(nV, i64(), "tk.n")` on a pointer produced
    // malformed IR ("SExt only operates on integer", #1476). Bail out here so
    // both methods fall through to tryLowerCollectionInstanceCall's dedicated
    // OwningSet Take/TryTake handler (LlvmBackend_Calls.cpp).
    if ((m == "Take" || m == "TryTake") &&
        builtinTypeName(recvTy) == "OwningSet")
        return nullptr;

    bool collRecv = isCollectionTypeName(recvTy);
    bool lazyRecv = isLazyEnumerableType(recvTy);
    // fix #454 / §5 / §4.6: LinkedList<T> implements IEnumerable<T> but is
    // bz_llist_t*-backed, not bz_array_t*-backed.  lowerToLazyEnumerator already
    // routes it via bz_lazy_from_llist, but the guard here used to exclude it
    // (neither isCollectionTypeName nor isLazyEnumerableType is true for a
    // LinkedList), so Where/Select/ToArray/etc. silently fell through and returned
    // 0 or crashed.  Include it so the lazy operators wrap it correctly.
    bool llRecv = isLinkedListTypeName(recvTy);
    // issue #775 / §16.3: a `Range<T>` is IEnumerable<T> but is neither a
    // bz_array_t*-backed collection nor a lazy enumerator — without this it fell
    // through and Count()/Where()/Sum()/… over a range returned 0. lowerToLazy-
    // Enumerator wraps it via bz_lazy_from_range, so admit it here too.
    bool rangeRecv = recvTy && recvTy->kind == sema::TypeRef::Kind::Named &&
                     recvTy->declaration &&
                     recvTy->declaration->name == "Range";
    // issue #995 / stdlib §5 / §29.11: a vtable-backed IEnumerable<T> receiver
    // (interface-typed local/param, or a yield-method result) — lowerToLazy-
    // Enumerator materializes it into an Array<T> via the iterator vtable, so the
    // LINQ operators (Where/Select/Take/…) can enumerate it. Without this they
    // silently fell through (the #995 Where/Select garbage/NYI).
    bool vtRecv = isVtableBackedIEnumerableType(recvTy);
    // fix #1220 / §5 / §9.6: string is IEnumerable<char> — lowerToLazyEnumerator
    // materializes it via bz_str_to_char_array so LINQ operators can enumerate it.
    bool strRecv = recvTy && recvTy->kind == sema::TypeRef::Kind::Primitive &&
                   recvTy->primitive == sema::PrimitiveType::String;
    // fix #1220 (residual) / stdlib §4.8 / §5: Dictionary<K,V> is
    // ICollection<DictionaryEntry<K,V>> — LINQ operators on a Dictionary
    // materialize its entries into a lazy array of {K,V} structs via
    // lowerToLazyEnumerator's Dictionary branch (see below).
    bool dictRecv = isDictTypeName(recvTy) &&
                    recvTy && recvTy->genericArgs.size() >= 2 &&
                    recvTy->genericArgs[0] && recvTy->genericArgs[1];
    // stdlib §5 (#755): `IGrouping<TKey, T> : IEnumerable<T>` — a grouping is a
    // legal receiver for the whole §5 operator surface. lowerToLazyEnumerator
    // wraps its (grouping-owned) element array as a borrowing lazy ARRAY source,
    // so every operator below composes uniformly. In particular `g.ToArray()`
    // now takes the ordinary materialize path and returns a FRESH copy, as
    // stdlib §5:3070 declares (`ToArray(): owned Array<T>`) — the #1296
    // hand-rolled lowering returned the grouping's OWN array, which the caller's
    // `using var` then freed alongside bz_grouping_free (a double free, #2265).
    // Note recvTy is used ONLY in this gate; everything below reads the element
    // type from lowerToLazyEnumerator's `elemRef` out-param, which is the
    // grouping's genericArgs[1].
    bool igrpRecv = iGroupingElemTypeRef(recvTy) != nullptr;
    // fix #4207 (stdlib §5): an explicitly-named `IOrderedEnumerable<T>`
    // receiver (a named local/param/field with explicit type annotation, or a
    // cross-module value). `IOrderedEnumerable<T> : IEnumerable<T>` (stdlib
    // §5 line 3592) — it is a LINQ source with the SAME `bz_lazy_t*` runtime
    // representation as the lazy sentinel, and the element type is at
    // genericArgs[0].  lowerToLazyEnumerator handles it in its own arm (it is
    // NOT isLazyEnumerableType — that predicate requires declaration-less, which
    // a named IOrderedEnumerable is not), and every LINQ operator/terminal
    // composes uniformly over it. Without this, a named IOrderedEnumerable
    // receiver silently fell through to the #2491 zero/null backstop at
    // runtime, producing wrong values instead of a compile-time diagnostic.
    bool orderedEnumRecv =
        recvTy && recvTy->kind == sema::TypeRef::Kind::Named &&
        !recvTy->declaration &&
        builtinTypeName(recvTy) == "IOrderedEnumerable";
    // fix #3763 (stdlib §5 / §5's `Reverse` naming note v0.195 / §4.6 / §4.7 /
    // §4.10 / §4.18, v0.287): the receiver heads on which stdlib §5's LAZY
    // `Reverse()` is the only declared meaning — the Set family and the
    // linked-list family, neither of which declares an instance `Reverse()`.
    // Without this term `isOp` below refused `Reverse` on them, the call fell
    // through to `tryLowerCollectionInstanceCall`'s IN-PLACE `bz_array_*`
    // reverse (LlvmBackend_Calls.cpp), and a class-element `Set<T>` was
    // silently MUTATED where §5 promises a fresh lazy sequence — measured
    // `{1,2,3}` → `{3,2,1}` on the receiver itself, exit 0, no diagnostic.
    // `collRecv`/`llRecv` are re-tested alongside the head so this term can
    // only ever NARROW the two families already admitted at the gate below,
    // never widen the receiver set. The membership rule itself lives in ONE
    // place — `common::isLazyReverseDirectReceiverHead`, which sema's typing
    // arm (ExprTypecheck_Calls.cpp's `typeLinqOperator`) reads too; see its
    // header for which heads are excluded and why. Both halves are required:
    // this admission alone was measured INERT by #3735 (with no `exprType` on
    // the call, nothing reaches here).
    bool revRecv = (collRecv || llRecv) &&
                   common::isLazyReverseDirectReceiverHead(
                       builtinTypeName(recvTy));
    if (!collRecv && !lazyRecv && !llRecv && !rangeRecv && !vtRecv && !strRecv &&
        !dictRecv && !igrpRecv && !orderedEnumRecv)
        return nullptr;

    // Element LLVM type for the receiver's element TypeRef.
    auto elemLlvmOf = [&](sema::TypeRef* er) -> llvm::Type* {
        if (er) if (auto* t = lowerTypeRef(er)) return t;
        return i32();
    };

    // ----- ToArray: materialize the (lazy) pipeline into an owned Array<T> ---
    if (m == "ToArray" && call.args.empty()) {
        // stdlib §5 (#651): GroupBy(...).ToArray() — intercept before the
        // generic lazy-materialization path and return the materialized groups
        // array directly (avoids a redundant array→lazy→array round-trip). The
        // GroupBy source is itself a call expression.
        if (auto* srcCall = dynamic_cast<const ast::CallExpr*>(ma->target.get())) {
            if (isGroupByCall(srcCall)) {
                if (llvm::Value* groups = lowerGroupByMaterialize(*srcCall))
                    return groups;
            }
        }

        sema::TypeRef* elemRef = nullptr;
        llvm::Value* lazy = lowerToLazyEnumerator(*ma->target, elemRef);
        if (!lazy) return nullptr;
        int64_t esz = lazyElemSize(elemLlvmOf(elemRef), module_->getDataLayout());
        auto* toArr = declareC("bz_lazy_to_array", ptrTy(), {ptrTy(), i64()});
        llvm::Value* arr =
            builder_.CreateCall(toArr, {lazy, builder_.getInt64(esz)}, "linq.toarr");
        // #602 / #429 / §4.6 / §28.9 / §28.11 v0.134: a materialized Array<string>
        // co-owns a +1 per element so it can be released symmetrically. Keyed on
        // the produced element type being in the RuntimeManaged row AND specifically
        // a non-nullable string primitive (same logic as emitArrayRetainStringsIfStringArray;
        // #602 batch 4: isStringElementArray was deleted, RuntimeManaged check is direct).
        //
        // fix #2738 / §28.3 / §29.2 (v0.282) — NARROW VTABLE-BACKED EXCLUSION:
        // when the receiver is a vtable-backed IEnumerable<string> (the
        // materializeVtableIEnumerableToArray path drains the enumerator's
        // curVal pointers into a fresh bz_array_t, then the lowerToLazyEnumerator
        // call above wraps that array as a lazy ARRAY source for this bz_lazy_to_array
        // drain), the drain loop's per-element emitStringRetain (Calls3.cpp ~line
        // 1107 — the materialize's balancing retain for the iterator's
        // yield-store prev-dispose + Dispose release) is ALREADY the sole
        // ownership transfer. Adding the outer bz_array_retain_strings here
        // would over-count by 1 per element: mat_arr is freed at bz_lazy_dispose
        // before the result array's lifetime ends (the materialize's own
        // retains are then orphaned, leaving rc=1 at result.Dispose → leak of
        // every yielded string). For the non-vtable-backed source paths (Array<T>,
        // chain ending in Array<T> / Lazy<T>) the source array HOLDS the +1
        // refs for its strings across this drain — its `using var`/local
        // binding releases them symmetrically with the result's retain — so
        // the outer retain is correct and necessary there. Skip iff the recv is
        // vtable-backed AND elemRef is a non-nullable string primitive. The two
        // gates match the materialize drain's retain above exactly.
        {
            bool skipOuterRetain = false;
            if (elemRef &&
                elemRef->kind == sema::TypeRef::Kind::Primitive &&
                elemRef->primitive == sema::PrimitiveType::String &&
                !elemRef->nullable) {
                if (auto* recvTy = ctx_.exprType(*ma->target)) {
                    if (isVtableBackedIEnumerableType(recvTy))
                        skipOuterRetain = true;
                }
            }
            const ElementPolicy toArrPol =
                elementPolicyFor(elemRef, /*owningFamily=*/false);
            if (!skipOuterRetain &&
                toArrPol.cat == ElementPolicy::Category::RuntimeManaged &&
                elemRef &&
                elemRef->kind == sema::TypeRef::Kind::Primitive &&
                elemRef->primitive == sema::PrimitiveType::String &&
                !elemRef->nullable) {
                auto* rf = declareC("bz_array_retain_strings",
                                    llvm::Type::getVoidTy(llvm_), {ptrTy()});
                builder_.CreateCall(rf, {arr});
            }
        }
        return arr;
    }

    // ----- ToSet: materialize the (lazy) pipeline into an owned Set<T> -------
    // stdlib §5: ToSet() → owned Set<T>. The Set is dict-backed (generic-key
    // path); element hash and equality trampolines are the same as Distinct uses.
    if (m == "ToSet" && call.args.empty()) {
        sema::TypeRef* elemRef = nullptr;
        llvm::Value* lazy = lowerToLazyEnumerator(*ma->target, elemRef);
        if (!lazy) return nullptr;
        llvm::Type* elemTy2 = elemLlvmOf(elemRef);
        int64_t esz = lazyElemSize(elemTy2, module_->getDataLayout());
        const sema::TypeSymbol* elemRec = recordTypeOfRef(elemRef);
        llvm::Function* hashTramp = buildLinqHashTrampoline(elemTy2, elemRef, elemRec);
        llvm::Function* eqTramp   = buildLinqTrampoline(3, elemTy2, nullptr, elemRec,
                                                        elemRef);  // #2773
        // fix #2803 / §4.10 / §28.9: a CLASS or INTERFACE element takes the
        // ARRAY-backed Set<T> path — the same shape `new Set<C>()` / `new
        // OwningSet<C>()` builds (a `bz_array_t` scanned with §8.3 equality
        // via `emitArrayBackedSetInsert`). The disposal site at
        // LlvmBackend_Disposal.cpp:3375 frees an `OwningSet<T>` as
        // `bz_array_free`; the dict-backed `bz_lazy_to_set` result disagreed
        // with that contract and was freed by `bz_array_free` (or UAF'd on a
        // subsequent `Equals`/`GetHashCode` read of the freed region —
        // #2773's trampolines are the load-bearing reader). The dict path is
        // preserved for every element type the §4.10/§4.8 generic-key dict
        // backing admits (record, primitive, enum, choice, string).
        // genericBackingType returns null for class/interface set elements
        // (the `isSet` exclusion at `genericBackingType`'s line 405).
        const sema::TypeRef* recvTy = ctx_.exprType(*ma->target);
        if (!genericBackingType(recvTy, 0, /*isSet=*/true, nullptr)) {
            auto* toSetArr = declareC("bz_lazy_to_set_arraybacked", ptrTy(),
                                      {ptrTy(), i64(), ptrTy()});
            return builder_.CreateCall(toSetArr,
                {lazy, builder_.getInt64(esz), eqTramp}, "linq.toset.arr");
        }
        auto* toSet = declareC("bz_lazy_to_set", ptrTy(),
                               {ptrTy(), i64(), ptrTy(), ptrTy()});
        return builder_.CreateCall(toSet,
            {lazy, builder_.getInt64(esz), hashTramp, eqTramp}, "linq.toset");
    }

    // ----- ToDictionary: materialize into an owned Dictionary<TKey, TValue> --
    // stdlib §5: ToDictionary<TKey>(key: (T)->TKey): owned Dictionary<TKey, T>,
    // and the two-selector ToDictionary<TKey,TValue>(key, value: (T)->TValue):
    // owned Dictionary<TKey, TValue> (#747 R43 — was silently ignoring the value
    // selector and storing the element). The value selector is applied per
    // element when supplied.
    if (m == "ToDictionary" && call.args.size() >= 1 && call.args[0].value) {
        // #2151 / §28.3 / stdlib §5: a chained `X.Split(",").ToDictionary(...)` (or
        // any fresh-array-materializer receiver) leaves the intermediate `Split`
        // `Array<string>` temporary unbound — the `bz_lazy_from_array` wrap in
        // lowerToLazyEnumerator is a BORROW (the lazy is consumed by
        // `bz_lazy_to_dict` via `bz_lazy_dispose`, but the array it pointed at is
        // not freed — `bz_lazy_dispose` only frees arrays it OWNS via
        // `bz_lazy_from_owned_array`). The using-Array-eligibility walk keys on
        // `currentFn_ == borrowingArrayFreeEligibleFn_` for NAMED-LOCAL
        // `var xs := Split(...)` receivers (working there via a scope-end
        // `bz_array_free`), but does NOT fire for an unbound temporary receiver
        // of this shape — the array is dropped, not bound to a local. Reclaim
        // the intermediate here, immediately after `bz_lazy_to_dict` consumes
        // the lazy. `freeUnboundOwnedBorrowingArrayTemp` is gated on
        // `isFreshOwnerlessArrayMaterializerExpr` / `isUnboundOwnedBorrowingArrayCallExpr`
        // so a named-local / `using var` / `move`-receiver is NOT freed again
        // (its owner disposes it — no double-free, no UAF).
        sema::TypeRef* elemRef = nullptr;
        llvm::Value* recvArr = nullptr;
        llvm::Value* lazy = lowerToLazyEnumerator(*ma->target, elemRef,
                                                  /*ownFreshGroupings=*/false,
                                                  &recvArr);
        if (!lazy) return nullptr;
        llvm::Type* elemTy2 = elemLlvmOf(elemRef);

        // Key type: the lambda (T)->TKey; TKey is its return type.
        sema::TypeRef* keyRefSema = nullptr;
        llvm::Type* keyTy = i32();
        if (auto* lamTy = ctx_.exprType(*call.args[0].value))
            if (lamTy->kind == sema::TypeRef::Kind::Function && lamTy->funcReturn) {
                keyRefSema = lamTy->funcReturn;
                if (auto* t = lowerTypeRef(keyRefSema)) keyTy = t;
            }
        int64_t keyEsz = lazyElemSize(keyTy, module_->getDataLayout());
        // #1692 / stdlib §5: the key trampoline (kind 2, below) carries the key
        // selector's result through a SINGLE int64-return closure call — correct
        // for an integer-lowered scalar (int/enum/char/bool) or a pointer-sized
        // (string/class) TKey (converted via ptrtoint), but not for an aggregate
        // TKey (record/decimal, an LLVM struct) or a floating-point TKey
        // (float/double have no integer bit-pattern conversion in the trampoline).
        // Reject those with a clean diagnostic rather than silently corrupting
        // the stored key (struct/decimal) or crashing codegen's IRBuilder on an
        // invalid sext of a non-integer type (float/double) — mirroring the
        // existing two-selector value-type restriction just below.
        if (keyTy->isStructTy() || keyTy->isFloatingPointTy()) {
            ctx_.diagnostics().report(
                Severity::Error, call.span.begin,
                "`ToDictionary`'s key selector currently supports only a "
                "scalar or reference-typed TKey (int/enum/char/bool/string/"
                "class) in codegen; a key selector returning a record, "
                "`decimal`, `float`, or `double` is not yet implemented "
                "(stdlib \xC2\xA7""5)");
            return llvm::PoisonValue::get(ptrTy());
        }

        // Value type/encoding: with a value selector (two-arg form) the value is
        // the selector's TValue return; otherwise it is the element T.
        bool hasValueSel = call.args.size() >= 2 && call.args[1].value;
        llvm::Type* valTy = elemTy2;
        if (hasValueSel) {
            if (auto* lamTy = ctx_.exprType(*call.args[1].value))
                if (lamTy->kind == sema::TypeRef::Kind::Function &&
                    lamTy->funcReturn)
                    if (auto* t = lowerTypeRef(lamTy->funcReturn)) valTy = t;
            // The value trampoline (kind 2) carries the selector result through
            // an i32→i64 closure call, so only an i32-lowered scalar TValue
            // (int/enum/char) round-trips correctly. Reject wider/struct value
            // types with a clean diagnostic rather than silently miscompiling.
            if (valTy != i32()) {
                ctx_.diagnostics().report(
                    Severity::Error, call.span.begin,
                    "two-selector `ToDictionary(key, value)` currently supports "
                    "only an `int`-typed value selector in codegen; a value "
                    "selector returning another type is not yet implemented "
                    "(stdlib \xC2\xA7""5, #747)");
                return llvm::PoisonValue::get(ptrTy());
            }
        }
        int64_t valEsz = lazyElemSize(valTy, module_->getDataLayout());

        // Key closure + trampoline (kind 2 = key: i64(closure, elem_ptr)).
        // Note: lowerLinqLambda is not in scope here (defined later in the isOp
        // section). For ToDictionary the user always annotates the lambda key
        // type explicitly, so lowerExpr suffices.
        //
        // #1692 / stdlib §5: pass the selector's REAL return type (`keyTy`) as
        // the trampoline's outTy so the internal closure call uses the correct
        // function signature. buildLinqTrampoline's kind-2 branch previously
        // hardcoded an `i32` return for every key selector regardless of TKey —
        // for a `(T) -> string` key selector (a `ptr` return) this called the
        // closure through the WRONG signature, reading only the low 32 bits of
        // the pointer off the ABI return register and storing that garbage as
        // the dict key, so every subsequent lookup raised KeyNotFoundException
        // even for a key that was just inserted.
        llvm::Value* keyClosure = lowerExpr(*call.args[0].value);
        if (!keyClosure) return nullptr;
        llvm::Function* keyTramp = buildLinqTrampoline(2, elemTy2, keyTy);

        // Value closure + trampoline (kind 2 over the element type: applies the
        // value selector to each element). Null when there is no value selector.
        // valTy is already gated to `i32` above, so passing it here is a no-op
        // relative to the trampoline's int32 default — kept for symmetry with
        // the key trampoline above.
        llvm::Value* valClosure =
            llvm::ConstantPointerNull::get(ptrTy());
        llvm::Value* valTramp = llvm::ConstantPointerNull::get(ptrTy());
        if (hasValueSel) {
            valClosure = lowerExpr(*call.args[1].value);
            if (!valClosure) return nullptr;
            valTramp = buildLinqTrampoline(2, elemTy2, valTy);
        }

        // #1692 / stdlib §5: a Dictionary<string,V>'s OWN ContainsKey/indexer/
        // Remove codegen unconditionally dispatches to the dedicated string
        // fast path (bz_dict_contains_str / bz_dict_try_get_str / ...), which
        // requires the underlying bz_dict_t to be in STRING-KEY mode — never
        // the generic-key mode bz_dict_alloc_generic always produces. Tell the
        // runtime to build a string-mode dict (bz_dict_alloc + bz_dict_set_str)
        // when TKey is `string`, so the result is consumable by the rest of
        // Dictionary<string,V>'s codegen, not just by construction.
        bool keyIsString = keyRefSema &&
            keyRefSema->kind == sema::TypeRef::Kind::Primitive &&
            keyRefSema->primitive == sema::PrimitiveType::String;

        // Key hash + equality trampolines for the dict backing — only needed
        // for the generic-key path (a string-mode dict hashes/compares by
        // content itself, like any other Dictionary<string,V>).
        const sema::TypeSymbol* keyRec = recordTypeOfRef(keyRefSema);
        llvm::Function* hashTramp = keyIsString
            ? nullptr : buildLinqHashTrampoline(keyTy, keyRefSema, keyRec);
        llvm::Function* eqTramp = keyIsString
            ? nullptr : buildLinqTrampoline(3, keyTy, nullptr, keyRec,
                                            keyRefSema);  // #2773
        llvm::Value* hashTrampV = hashTramp
            ? static_cast<llvm::Value*>(hashTramp)
            : llvm::ConstantPointerNull::get(ptrTy());
        llvm::Value* eqTrampV = eqTramp
            ? static_cast<llvm::Value*>(eqTramp)
            : llvm::ConstantPointerNull::get(ptrTy());

        // #738: the value encoding must match the codegen Dictionary<K,V> read
        // (encodeValue) — a SCALAR value (non-struct LLVM type) is stored inline
        // in the void* slot; a STRUCT value (record/decimal) is heap-boxed. A
        // value selector always yields a scalar TValue (gated to i32 above).
        bool valInline = hasValueSel || (valTy && !valTy->isStructTy());
        auto* toDict = declareC("bz_lazy_to_dict", ptrTy(),
            {ptrTy(), i64(), i64(), ptrTy(), ptrTy(), ptrTy(), ptrTy(), i64(),
             ptrTy(), ptrTy(), i64()});
        llvm::Value* result = builder_.CreateCall(toDict,
            {lazy, builder_.getInt64(valEsz), builder_.getInt64(keyEsz),
             keyTramp, keyClosure, hashTrampV, eqTrampV,
             builder_.getInt64(valInline ? 1 : 0), valTramp, valClosure,
             builder_.getInt64(keyIsString ? 1 : 0)},
            "linq.todict");
        // #2263 / stdlib §5 / §4.12 / #2151: when the value type is `string`
        // (single-selector `ToDictionary(key)` over an `Array<string>` /
        // `string.Split(...)` source — `hasValueSel` false, `valInline` true),
        // `bz_lazy_to_dict` stores the element pointer into the dict slot
        // WITHOUT retaining the string struct (the dict's per-value slot is a
        // raw void* that aliases the bz_string_struct* the source array
        // element holds). The freeUnboundOwnedBorrowingArrayTemp call below
        // would then bz_array_free the source with the string-release thunk,
        // dropping each substring's refcount to 0 — every dict value is now a
        // dangling pointer (use-after-free on the next deref). SKIP the source
        // reclaim entirely when the value type is string; the source array
        // struct itself leaks (a small bounded leak vs. a guaranteed UAF). The
        // int / float / struct-Value / record-Value cases are unaffected
        // (struct values are malloc-copied in the runtime; scalars are
        // refcount-free; only a `string` value aliases the source array).
        bool valIsString = !hasValueSel && elemRef &&
            elemRef->kind == sema::TypeRef::Kind::Primitive &&
            elemRef->primitive == sema::PrimitiveType::String &&
            !elemRef->nullable;
        // #2151 / §28.3: reclaim an unbound fresh-materializer receiver array
        // here (the lazy is already consumed/disposed by bz_lazy_to_dict). See
        // the matching comment at the ToDictionary entry above for rationale
        // and the no-double-free gate.
        if (recvArr && !valIsString) freeUnboundOwnedBorrowingArrayTemp(
                ma->target.get(), recvArr, ctx_.exprType(*ma->target));
        // Release the inline-lambda / primitive-intrinsic-method-ref key/value
        // closures if we own them (eager materialization — the closures are
        // fully consumed during bz_lazy_to_dict, same as eager LINQ terminals
        // like Count/Sum). §27.6 (#1758): a `Type.Member` intrinsic reference is
        // also a fresh +1 cell (isPrimIntrinsicMethodRef) we must release.
        if (call.args[0].value &&
            (dynamic_cast<const ast::LambdaExpr*>(call.args[0].value.get()) ||
             isPrimIntrinsicMethodRef(call.args[0].value.get())))
            emitClosureRelease(keyClosure);
        if (hasValueSel && call.args[1].value &&
            (dynamic_cast<const ast::LambdaExpr*>(call.args[1].value.get()) ||
             isPrimIntrinsicMethodRef(call.args[1].value.get())))
            emitClosureRelease(valClosure);
        return result;
    }

    // The deferred operators. Lower the source to a lazy enumerator first.
    // NOTE: Reverse is intercepted only where §5's LAZY operator is the
    // receiver's declared meaning — on a concrete `Array`/`OwningArray` (whose
    // §4.4/§4.5 in-place `Reverse()` wins by spec §26.5 rule 1) it falls
    // through to the in-place handler in tryLowerCollectionInstanceCall (§3),
    // and so, today, do Stack/Queue and the other array-backed heads
    // `revRecv` excludes (fix #3763; see that flag's definition).
    bool isOp = (m == "Where" || m == "Select" || m == "Take" || m == "Skip" ||
                 m == "Distinct" || m == "OrderBy" || m == "OrderByDescending" ||
                 m == "Concat" || m == "TakeWhile" || m == "SkipWhile" ||
                 m == "SelectMany" || m == "Zip" ||
                 // stdlib §5 Set-like lazy ops (this milestone):
                 m == "Union" || m == "Intersect" || m == "Except" ||
                 // stdlib §5 Ordering secondary key:
                 m == "ThenBy" || m == "ThenByDescending" ||
                 // stdlib §5 lazy Reverse — on a lazy receiver (not Array, which
                 // has an in-place Array.Reverse()). issue #775: a Range<T> also
                 // has no in-place Reverse, so `(1..n).Reverse()` is the lazy
                 // materialize-then-yield-backwards form — without rangeRecv it
                 // fell through to the Array in-place handler (Count/Sum = 0;
                 // ToArray SIGABRT on the bz.range struct read as an array).
                 // fix #1220: string is IEnumerable<char> — admit Reverse on it.
                 // fix #3559: `OfType<U>()` was removed from stdlib §5 at
                 // v0.284 and its always-true-predicate lowering deleted here.
                 // fix #3763: `revRecv` — the Set and linked-list families,
                 // which declare no instance `Reverse()`, so §5's lazy operator
                 // is the only meaning. See `revRecv`'s definition above.
                 (m == "Reverse" &&
                  (lazyRecv || rangeRecv || strRecv || revRecv)));
    if (!isOp) return nullptr;

    // stdlib §5 (#755) / #1914: `X.GroupBy(k).SelectMany(lambda(g) => g)` — the
    // one composed shape that provably FLATTENS the groups away: the pipeline's
    // elements are T values copied out of each grouping's element array, so no
    // grouping POINTER can survive into the result and the fresh groups
    // temporary (which nothing else owns — it leaked) can be owned by the source
    // lazy and reclaimed at pipeline dispose. Every other operator may surface a
    // grouping downstream (`.Where(…).First()` returns one), where owning here
    // would use-after-free — those keep the borrowing default.
    bool ownSrcGroupings = false;
    if (m == "SelectMany" && call.args.size() == 1 && call.args[0].value)
        if (auto* smLamTy = ctx_.exprType(*call.args[0].value))
            if (smLamTy->kind == sema::TypeRef::Kind::Function &&
                smLamTy->funcReturn)
                ownSrcGroupings =
                    iGroupingElemTypeRef(smLamTy->funcReturn) != nullptr;

    sema::TypeRef* elemRef = nullptr;
    llvm::Value* src = lowerToLazyEnumerator(*ma->target, elemRef,
                                             ownSrcGroupings);
    if (!src) return nullptr;
    llvm::Type* elemTy = elemLlvmOf(elemRef);

    // #422 / §28.9 / §28.3: a LAZY operator stores its closure in the enumerator
    // (e->closure) and invokes it LATER during iteration — so the closure must
    // OUTLIVE the call. When it was produced INLINE (a lambda literal), it is a
    // +1 transient the enumerator owns; mark the enumerator with
    // bz_lazy_own_closure so bz_lazy_dispose releases it at enumerator dispose
    // (NOT here — a call-site release would be a use-after-free). A closure
    // arriving via a variable/field is a BORROW and is left untouched. Mirrors
    // the GetOrAdd/eager factoryIsInlineLambda guard.
    auto argIsInlineLambda = [&](size_t i) -> bool {
        if (i >= call.args.size() || !call.args[i].value) return false;
        const ast::Expr* a = call.args[i].value.get();
        if (dynamic_cast<const ast::LambdaExpr*>(a)) return true;
        // §27.6 (v0.184, #1758): a bare primitive-intrinsic method reference
        // (`Select(int.Parse)`, `Where(char.IsDigit)`) is ALSO a fresh +1
        // closure cell (makePrimIntrinsicThunk) the enumerator must own and
        // release on dispose — the same inline-producer shape isPlusOne-
        // ClosureProducer recognizes for the bound-local path. Without this the
        // cell leaked (40 bytes; #1758 pre-gate review). Keyed off the shared
        // predicate so the owned set matches the produced set exactly.
        return isPrimIntrinsicMethodRef(a);
    };
    // Thread the enumerator pointer through bz_lazy_own_closure(e) iff the
    // operator's closure arg was an inline lambda; otherwise return it unchanged.
    auto ownClosureIf = [&](llvm::Value* e, bool inlineLambda) -> llvm::Value* {
        if (!inlineLambda || !e) return e;
        auto* fn = declareC("bz_lazy_own_closure", ptrTy(), {ptrTy()});
        return builder_.CreateCall(fn, {e}, "linq.owncl");
    };

    // Lower a LINQ lambda argument with the expected (T)->ret function type so
    // unannotated lambda params get the right LLVM type.
    auto lowerLinqLambda = [&](const ast::Expr* argExpr,
                               sema::TypeRef* retTypeRef) -> llvm::Value* {
        auto* funcRef = ctx_.makeTypeRef();
        funcRef->kind = sema::TypeRef::Kind::Function;
        funcRef->funcReturn = retTypeRef;
        if (elemRef) {
            sema::TypeRef::FuncParam fp;
            fp.type = elemRef;
            funcRef->funcParams.push_back(std::move(fp));
        }
        const sema::TypeRef* saved = expectedFunctionType_;
        expectedFunctionType_ = funcRef;
        llvm::Value* v = lowerExpr(*argExpr);
        expectedFunctionType_ = saved;
        return v;
    };

    if (m == "Where" && call.args.size() == 1 && call.args[0].value) {
        llvm::Value* closure = lowerLinqLambda(
            call.args[0].value.get(),
            ctx_.primitiveTypeRef(sema::PrimitiveType::Bool));
        if (!closure) return nullptr;
        llvm::Function* tramp = buildLinqTrampoline(0, elemTy, nullptr);
        auto* whereFn = declareC("bz_lazy_where", ptrTy(), {ptrTy(), ptrTy(), ptrTy()});
        llvm::Value* e = builder_.CreateCall(whereFn, {src, closure, tramp}, "linq.where");
        return ownClosureIf(e, argIsInlineLambda(0));  // #422: release on dispose
    }
    if (m == "Select" && call.args.size() == 1 && call.args[0].value) {
        sema::TypeRef* retSemaType = nullptr;
        llvm::Type* outTy = i32();
        if (auto* lamTy = ctx_.exprType(*call.args[0].value))
            if (lamTy->kind == sema::TypeRef::Kind::Function && lamTy->funcReturn) {
                retSemaType = lamTy->funcReturn;
                if (auto* t = lowerTypeRef(retSemaType)) outTy = t;
            }
        llvm::Value* closure = lowerLinqLambda(call.args[0].value.get(), retSemaType);
        if (!closure) return nullptr;
        llvm::Function* tramp = buildLinqTrampoline(1, elemTy, outTy);
        int64_t outEsz = lazyElemSize(outTy, module_->getDataLayout());
        auto* selFn = declareC("bz_lazy_select", ptrTy(),
                               {ptrTy(), i64(), ptrTy(), ptrTy()});
        llvm::Value* e = builder_.CreateCall(selFn,
            {src, builder_.getInt64(outEsz), closure, tramp}, "linq.select");
        return ownClosureIf(e, argIsInlineLambda(0));  // #422: release on dispose
    }
    if (m == "Take" && call.args.size() == 1 && call.args[0].value) {
        llvm::Value* nV = lowerExpr(*call.args[0].value);
        if (!nV) return nullptr;
        llvm::Value* n64 = nV->getType()->isIntegerTy(64) ? nV
            : builder_.CreateSExt(nV, i64(), "tk.n");
        auto* takeFn = declareC("bz_lazy_take", ptrTy(), {ptrTy(), i64()});
        return builder_.CreateCall(takeFn, {src, n64}, "linq.take");
    }
    if (m == "Skip" && call.args.size() == 1 && call.args[0].value) {
        llvm::Value* nV = lowerExpr(*call.args[0].value);
        if (!nV) return nullptr;
        llvm::Value* n64 = nV->getType()->isIntegerTy(64) ? nV
            : builder_.CreateSExt(nV, i64(), "sk.n");
        auto* skipFn = declareC("bz_lazy_skip", ptrTy(), {ptrTy(), i64()});
        return builder_.CreateCall(skipFn, {src, n64}, "linq.skip");
    }
    if (m == "Distinct" && call.args.empty()) {
        // issue D6 / §9.1 / §9.2: a record element sequence dedups by its
        // synthesized structural equality (mirroring Set<record>), not an ICmp
        // on the aggregate. recordTypeOfRef is non-null only for an
        // hashable record element type.
        const sema::TypeSymbol* elemRec = recordTypeOfRef(elemRef);
        llvm::Function* eqTramp = buildLinqTrampoline(3, elemTy, nullptr, elemRec,
                                                      elemRef);  // #2773
        auto* distFn = declareC("bz_lazy_distinct", ptrTy(), {ptrTy(), ptrTy()});
        return builder_.CreateCall(distFn, {src, eqTramp}, "linq.distinct");
    }
    if ((m == "OrderBy" || m == "OrderByDescending") &&
        call.args.size() == 1 && call.args[0].value) {
        int32_t desc = (m == "OrderByDescending") ? 1 : 0;
        llvm::Value* closure = lowerLinqLambda(
            call.args[0].value.get(),
            ctx_.primitiveTypeRef(sema::PrimitiveType::Int));
        if (!closure) return nullptr;
        llvm::Function* tramp = buildLinqTrampoline(2, elemTy, nullptr);
        auto* obFn = declareC("bz_lazy_orderby", ptrTy(),
                              {ptrTy(), ptrTy(), ptrTy(), i32()});
        llvm::Value* e = builder_.CreateCall(obFn,
            {src, closure, tramp, builder_.getInt32(desc)}, "linq.orderby");
        return ownClosureIf(e, argIsInlineLambda(0));  // #422: release on dispose
    }
    // stdlib §5 Combination: Concat(other) — yield all of self, then all of
    // `other`. Lower `other` (an Array<T> or lazy IEnumerable<T>) to its own lazy
    // enumerator and hand both to the runtime.
    if (m == "Concat" && call.args.size() == 1 && call.args[0].value) {
        sema::TypeRef* otherElem = nullptr;
        llvm::Value* down = lowerToLazyEnumerator(*call.args[0].value, otherElem);
        if (!down) return nullptr;
        auto* concatFn = declareC("bz_lazy_concat", ptrTy(), {ptrTy(), ptrTy()});
        return builder_.CreateCall(concatFn, {src, down}, "linq.concat");
    }
    // stdlib §5 Slicing: TakeWhile/SkipWhile(predicate) — gate by a `(T)->bool`
    // predicate (reusing the kind-0 predicate trampoline).
    if ((m == "TakeWhile" || m == "SkipWhile") &&
        call.args.size() == 1 && call.args[0].value) {
        llvm::Value* closure = lowerLinqLambda(
            call.args[0].value.get(),
            ctx_.primitiveTypeRef(sema::PrimitiveType::Bool));
        if (!closure) return nullptr;
        llvm::Function* tramp = buildLinqTrampoline(0, elemTy, nullptr);
        const char* rt = (m == "TakeWhile") ? "bz_lazy_takewhile"
                                            : "bz_lazy_skipwhile";
        auto* fn = declareC(rt, ptrTy(), {ptrTy(), ptrTy(), ptrTy()});
        llvm::Value* e = builder_.CreateCall(fn, {src, closure, tramp},
                                   m == "TakeWhile" ? "linq.takewhile"
                                                    : "linq.skipwhile");
        return ownClosureIf(e, argIsInlineLambda(0));  // #422: release on dispose
    }
    // stdlib §5 Projection: SelectMany(selector: (T)->IEnumerable<U>) — flatten.
    // The selector returns an inner collection; the trampoline wraps it as a lazy
    // enumerator. The inner element size U comes from the selector's return type
    // (a collection/lazy carrying U at genericArgs[0]).
    if (m == "SelectMany" && call.args.size() == 1 && call.args[0].value) {
        sema::TypeRef* retSemaType = nullptr;
        llvm::Type* innerElemTy = i32();
        if (auto* lamTy = ctx_.exprType(*call.args[0].value))
            if (lamTy->kind == sema::TypeRef::Kind::Function && lamTy->funcReturn) {
                retSemaType = lamTy->funcReturn;  // IEnumerable<U> / Array<U>
                if (!retSemaType->genericArgs.empty() && retSemaType->genericArgs[0])
                    if (auto* t = lowerTypeRef(retSemaType->genericArgs[0]))
                        innerElemTy = t;
            }
        llvm::Value* closure = lowerLinqLambda(call.args[0].value.get(), retSemaType);
        if (!closure) return nullptr;
        int64_t innerEsz = lazyElemSize(innerElemTy, module_->getDataLayout());
        // #758: when the selector returns a lazy LINQ chain (not a concrete
        // collection), its result is already a `bz_lazy_t*` enumerator — the
        // trampoline returns it directly instead of wrapping it as an array.
        bool innerIsLazy = isLazyEnumerableType(retSemaType);
        // issue #775 / §16.3: the selector returns a `Range<T>` value (a 32-byte
        // struct, not a collection pointer) — the trampoline must call it via the
        // struct-return ABI and wrap it via bz_lazy_from_range. Recognized by
        // name (its TypeSymbol carries the builtin Range class decl).
        bool rangeInner = retSemaType &&
                          retSemaType->kind == sema::TypeRef::Kind::Named &&
                          retSemaType->declaration &&
                          retSemaType->declaration->name == "Range";
        // §16.3 (v0.183, #1586): T = char for the selector's Range<T> result.
        bool rangeInnerIsChar = rangeInner && !retSemaType->genericArgs.empty() &&
            retSemaType->genericArgs[0] &&
            retSemaType->genericArgs[0]->kind == sema::TypeRef::Kind::Primitive &&
            retSemaType->genericArgs[0]->primitive == sema::PrimitiveType::Char;
        // stdlib §5 (#755, finding [20]): the selector returns an
        // `IGrouping<TKey, U>` (`GroupBy(k).SelectMany(lambda(g) => g)`). Two
        // corrections: the inner ELEMENT type is genericArgs[1], not [0] (the
        // block above read TKey), and the returned pointer is a bz_grouping_t*,
        // not a bz_array_t* — read the grouping's element array in the
        // trampoline. Without the second the grouping struct was wrapped AS an
        // array header whose count field lands on `key_esz`, so every group
        // yielded key_esz elements regardless of its size (4 per group for an
        // int key — the reported symptom).
        const sema::TypeRef* grpInnerElem = iGroupingElemTypeRef(retSemaType);
        bool groupingInner = grpInnerElem != nullptr;
        if (groupingInner) {
            if (auto* t = lowerTypeRef(grpInnerElem)) innerElemTy = t;
            innerEsz = lazyElemSize(innerElemTy, module_->getDataLayout());
        }
        llvm::Function* tramp = buildSelectManyTrampoline(
            elemTy, innerEsz, innerIsLazy, rangeInner, rangeInnerIsChar,
            groupingInner);
        auto* smFn = declareC("bz_lazy_selectmany", ptrTy(),
                              {ptrTy(), i64(), ptrTy(), ptrTy()});
        llvm::Value* e = builder_.CreateCall(smFn,
            {src, builder_.getInt64(innerEsz), closure, tramp}, "linq.selectmany");
        return ownClosureIf(e, argIsInlineLambda(0));  // #422: release on dispose
    }
    // stdlib §5 Combination: Zip<U,R>(other, combine: (T,U)->R) — pair positionally.
    if (m == "Zip" && call.args.size() == 2 &&
        call.args[0].value && call.args[1].value) {
        // Lower `other` (the second source) to its own lazy enumerator; capture
        // its element type for the combine's 2nd parameter.
        sema::TypeRef* otherElem = nullptr;
        llvm::Value* down = lowerToLazyEnumerator(*call.args[0].value, otherElem);
        if (!down) return nullptr;
        llvm::Type* bTy = elemLlvmOf(otherElem);
        // The combine lambda `(T, U) -> R`. Lower it with the expected function
        // type so unannotated params get the right element LLVM types.
        sema::TypeRef* retSemaType = nullptr;
        llvm::Type* outTy = i32();
        if (auto* lamTy = ctx_.exprType(*call.args[1].value))
            if (lamTy->kind == sema::TypeRef::Kind::Function && lamTy->funcReturn) {
                retSemaType = lamTy->funcReturn;
                if (auto* t = lowerTypeRef(retSemaType)) outTy = t;
            }
        auto* combRef = ctx_.makeTypeRef();
        combRef->kind = sema::TypeRef::Kind::Function;
        combRef->funcReturn = retSemaType;
        if (elemRef) { sema::TypeRef::FuncParam fp; fp.type = elemRef;
                       combRef->funcParams.push_back(std::move(fp)); }
        if (otherElem) { sema::TypeRef::FuncParam fp; fp.type = otherElem;
                         combRef->funcParams.push_back(std::move(fp)); }
        const sema::TypeRef* savedExp = expectedFunctionType_;
        expectedFunctionType_ = combRef;
        llvm::Value* closure = lowerExpr(*call.args[1].value);
        expectedFunctionType_ = savedExp;
        if (!closure) return nullptr;
        llvm::Function* tramp = buildZipTrampoline(elemTy, bTy, outTy);
        int64_t outEsz = lazyElemSize(outTy, module_->getDataLayout());
        auto* zipFn = declareC("bz_lazy_zip", ptrTy(),
                               {ptrTy(), ptrTy(), i64(), ptrTy(), ptrTy()});
        llvm::Value* e = builder_.CreateCall(zipFn,
            {src, down, builder_.getInt64(outEsz), closure, tramp}, "linq.zip");
        return ownClosureIf(e, argIsInlineLambda(1));  // #422: combine is args[1]
    }
    // stdlib §5 Set-like lazy: Union/Intersect/Except(other: IEnumerable<T>).
    // Each builds a membership dict from `other` at construction, then yields
    // unique elements (Union: self∪other; Intersect: self∩other; Except: self∖other).
    // Element dedup uses the generic-dict hash/equality trampolines (same as
    // Distinct, but also needs a hash function for the O(1) dict path).
    if ((m == "Union" || m == "Intersect" || m == "Except") &&
        call.args.size() == 1 && call.args[0].value) {
        sema::TypeRef* otherElem = nullptr;
        llvm::Value* down = lowerToLazyEnumerator(*call.args[0].value, otherElem);
        if (!down) return nullptr;
        // Build element hash + equality trampolines for the membership dict.
        const sema::TypeSymbol* elemRec = recordTypeOfRef(elemRef);
        llvm::Function* hashTramp = buildLinqHashTrampoline(elemTy, elemRef, elemRec);
        llvm::Function* eqTramp   = buildLinqTrampoline(3, elemTy, nullptr, elemRec,
                                                        elemRef);  // #2773
        const char* rtFn = (m == "Union")     ? "bz_lazy_union"
                         : (m == "Intersect") ? "bz_lazy_intersect"
                                              : "bz_lazy_except";
        auto* fn = declareC(rtFn, ptrTy(), {ptrTy(), ptrTy(), ptrTy(), ptrTy()});
        return builder_.CreateCall(fn, {src, down, hashTramp, eqTramp},
                                   "linq." + std::string(m == "Union" ? "union"
                                                        : m == "Intersect" ? "intersect"
                                                                           : "except"));
    }
    // stdlib §5 Ordering: ThenBy / ThenByDescending(key: (T)->TKey) secondary sort.
    // The receiver is an IOrderedEnumerable<T> (lazy sentinel carrying T at [0]).
    // The primary key is already baked into the upstream ORDERBY node; ThenBy
    // reaches into it via the runtime (bz_lazy_thenby reads up->keyfn/closure).
    if ((m == "ThenBy" || m == "ThenByDescending") &&
        call.args.size() == 1 && call.args[0].value) {
        int32_t desc2 = (m == "ThenByDescending") ? 1 : 0;
        llvm::Value* closure2 = lowerLinqLambda(
            call.args[0].value.get(),
            ctx_.primitiveTypeRef(sema::PrimitiveType::Int));
        if (!closure2) return nullptr;
        llvm::Function* tramp2 = buildLinqTrampoline(2, elemTy, nullptr);
        auto* tbFn = declareC("bz_lazy_thenby", ptrTy(),
                              {ptrTy(), ptrTy(), ptrTy(), i32()});
        llvm::Value* e = builder_.CreateCall(tbFn,
            {src, closure2, tramp2, builder_.getInt32(desc2)}, "linq.thenby");
        // Mark closure2 as owned by the THENBY enumerator if it was an inline
        // lambda — disposed via own_closure2 at bz_lazy_dispose.
        if (argIsInlineLambda(0) && e) {
            auto* fn = declareC("bz_lazy_own_closure2", ptrTy(), {ptrTy()});
            e = builder_.CreateCall(fn, {e}, "linq.owncl2");
        }
        return e;
    }
    // stdlib §5 Ordering: Reverse() on an IEnumerable<T> receiver — lazy
    // materialize + yield backwards. (The in-place Array Reverse is lowered
    // separately before tryLowerLazyLinqCall is reached.)
    if (m == "Reverse" && call.args.empty()) {
        auto* revFn = declareC("bz_lazy_reverse", ptrTy(), {ptrTy()});
        return builder_.CreateCall(revFn, {src}, "linq.reverse");
    }
    return nullptr;
}


// ---------------------------------------------------------------------------
// per §4.6/§4.7: LinkedList<T> / OwningLinkedList<T> node-based API codegen.
// The runtime is bz_llist_t (doubly-linked list of inline-value nodes).
// A LinkedListNode<T> Bezel value is a raw ptr to bz_llist_node_t; `.Value`
// loads the inline bytes; `.Next`/`.Previous` follow the pointer chain.
// ---------------------------------------------------------------------------
llvm::Value* Emitter::tryLowerLinkedListInstanceCall(const ast::CallExpr& call) {
    if (!call.callee) return nullptr;
    auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(call.callee.get());
    if (!ma || !ma->target) return nullptr;
    const auto& m = ma->member;

    auto* recvTy = ctx_.exprType(*ma->target);

    // -----------------------------------------------------------------------
    // LinkedListNode<T> property accessor lowerings (syntactically a call-expr
    // when the parser sees node.Next or node.Value — or a member-access expr;
    // handled here for safety).  In practice these are MemberAccessExprs, not
    // CallExprs, so this block likely won't fire — but guard anyway.
    // -----------------------------------------------------------------------
    // (The real work for node properties is in lowerExpr's MemberAccessExpr
    // handling below.  tryLowerLinkedListInstanceCall handles CALL forms.)

    // -----------------------------------------------------------------------
    // LinkedList<T> / OwningLinkedList<T> method lowerings.
    // -----------------------------------------------------------------------
    if (!isLinkedListTypeName(recvTy)) return nullptr;

    // Element size for this instantiation.
    int64_t esz = collectionElemSize(recvTy);
    bool isOwning = recvTy && recvTy->astNode && recvTy->astNode->name &&
                    !recvTy->astNode->name->parts.empty() &&
                    recvTy->headName() == "OwningLinkedList";

    llvm::Value* recv = lowerExpr(*ma->target);
    if (!recv) return nullptr;

    // Element LLVM type (for loading values from nodes).
    llvm::Type* elemLLTy = i32();
    if (recvTy && !recvTy->genericArgs.empty() && recvTy->genericArgs[0])
        elemLLTy = lowerTypeRef(recvTy->genericArgs[0]);

    // #602 / §28.11 v0.134: hoist one element-slot policy lookup for LinkedList
    // operations that need per-element equality (Find/Remove/Contains) or disposal
    // (OwningLinkedList RemoveFirst/RemoveLast/Clear/Remove).
    // LinkedList is always in the borrowing family (no Owned* string elements);
    // OwningLinkedList is in the owning family (isOwning flag above).
    const sema::TypeRef* llElemRef =
        (recvTy && !recvTy->genericArgs.empty()) ? recvTy->genericArgs[0] : nullptr;
    const ElementPolicy llElemPol = elementPolicyFor(llElemRef, isOwning);
    // llElemIsClass: class-or-interface element — per §28.11 v0.135 fix #619:
    // elementPolicyFor now returns ClassOwned/ClassBorrow for interface elements.
    bool llElemIsClass =
        llElemPol.cat == ElementPolicy::Category::ClassOwned ||
        llElemPol.cat == ElementPolicy::Category::ClassBorrow;

    // fix #2686 / #602 / §28.11 / §28.9: a BORROWING `LinkedList<string>` co-owns
    // a +1 per stored element — §28.11's element-slot table puts a `string` in a
    // borrowing collection in the runtime-managed row, and the disposal side
    // already acts on that (`buildCollectionTempDisposal` and
    // `patternAResultCollectionDisposer` both install stringElementReleaseThunk
    // for it). No insert path took that +1, so a temp `LinkedList<string>`
    // released a reference it never retained: ASan SEGV in bz_string_release
    // under bz$strElemRelease under bz_llist_free. The predicate is a verbatim
    // mirror of the Array/Stack/Queue element arm of
    // materializeHeapCollectionLiteral (`litIsStrArr`): OwningLinkedList is
    // excluded (its element parameter is `owned` — a transfer, and
    // elementPolicyFor puts an owning-family string in the Value row), `string?`
    // is excluded, and a fresh +1 producer TRANSFERS via isPlusOneStringProducer
    // instead of double-retaining.
    bool llIsStrElem =
        !isOwning && llElemRef && !llElemRef->nullable &&
        llElemRef->kind == sema::TypeRef::Kind::Primitive &&
        llElemRef->primitive == sema::PrimitiveType::String &&
        llElemPol.cat == ElementPolicy::Category::RuntimeManaged;
    // Take the list's +1 on the element about to be stored into a new node.
    // `argExpr` is the element argument (arg 0 for AddFirst/AddLast, arg 1 for
    // AddBefore/AddAfter, whose arg 0 is the anchor node).
    auto llRetainElem = [&](llvm::Value* v, const ast::Expr* argExpr) {
        if (llIsStrElem && !isPlusOneStringProducer(argExpr)) emitStringRetain(v);
    };
    // fix #2692 / §28.11 / §28.9: the RELEASE half of the same co-ownership, for
    // the removal operations. Every removal below hands the runtime a per-element
    // disposer that `bz_llist_remove_first/_last/_node` invokes EXACTLY ONCE on
    // the element of the node it unlinks, so releasing there consumes exactly the
    // +1 `llRetainElem` (or a transferred +1 producer) put in at insert. Before
    // #2686 no insert path retained and this had to stay null; now all SIX do
    // (`.AddFirst`/`.AddLast`/`.AddBefore`/`.AddAfter` above, the
    // `bz_llist_add_last` arm of materializeHeapCollectionLiteral, and — since
    // #2693 — the `new LinkedList<T>(source)` arm of lowerNew, which appends the
    // drained source with the same unconditional per-element retain), which are
    // the ONLY ways an element enters a bz_llist_t — the runtime exposes no other
    // insert and codegen has exactly two `bz_llist_alloc` sites (that literal arm
    // and `new LinkedList<T>(…)`, whose no-arg form allocates EMPTY and whose
    // source form populates through the retaining append above). So the release
    // is provably paired. This is a verbatim mirror of the Array/Stack/Queue arm
    // (`if (!elemDisp && arrIsStringElem) elemDisp = stringElementReleaseThunk();`
    // at .RemoveAt/.Remove/.Clear in LlvmBackend_Calls.cpp), which has released a
    // borrowing `Array<string>`'s elements on removal since #602.
    // `.TryPopFirst`/`.TryPopLast` deliberately do NOT use this: `bz_llist_pop_*`
    // MOVES the element out by value into the caller's out-param, transferring
    // the reference rather than dropping it.
    auto llElemDisposer = [&](llvm::Function* owningDisp) -> llvm::Value* {
        if (!owningDisp && llIsStrElem) owningDisp = stringElementReleaseThunk();
        return owningDisp ? llvm::cast<llvm::Value>(owningDisp)
                          : llvm::cast<llvm::Value>(
                                llvm::ConstantPointerNull::get(ptrTy()));
    };
    // fix #4579 (§4.6 / §4.7 / §7.2, v0.306): every `bz_llist_add_*` below hands
    // the runtime a POINTER to this slot plus `esz`, and `llist_node_alloc`
    // memcpy's exactly `esz` bytes out of it. `esz` is `collectionElemSize`, the
    // ALLOCATION size of the element's LLVM type — for a nullable VALUE element
    // (`LinkedList<int?>`) that is the tagged `{i8 hasValue, T value}` struct, 8
    // bytes for `int?`. But the arms lowered the argument with a bare
    // `lowerExpr`, which for `ll.AddLast(1)` yields the inner `i32` (the
    // IntLiteral arm lowers from `rt->primitive` and ignores `rt->nullable`), so
    // the slot was a 4-byte `alloca i32` and the memcpy read 4 bytes PAST it.
    //
    // MEASURED on 75ae6094 (gate `--full`, shard codegen/5): "AddressSanitizer:
    // stack-buffer-overflow … READ of size 8 … in memcpy / llist_node_alloc /
    // bz_llist_add_last", with the frame report naming the two slots the repro
    // creates — `[64, 68)` (the 4-byte `AddLast(1)` slot, overflowed) beside
    // `[32, 40)` (the correctly-sized 8-byte `AddLast(null)` slot, because the
    // NullLiteral arm DOES consult `isNullableValueTy`).
    //
    // It is a wrong ANSWER before it is a memory error: `{i8, T}` puts the tag at
    // offset 0, so the stored node's hasValue byte became the payload's LOW BYTE
    // and the payload came from the uninitialized 4 bytes past the slot. Any
    // value whose low byte is zero read back as `null` — `ll.AddLast(256)` then
    // `256 in ll` answered FALSE on the plain build (§17.5's membership test over
    // §8.4 equality), which is why the plain oracle passed the ASan-failing row.
    //
    // The array-backed family has had the rule since fix #455 (§4.5); this is the
    // same helper, now shared (`ensureNullableElementValue`). Spill through one
    // place so the "slot type == element type" invariant cannot be restated
    // wrongly in a fifth arm.
    auto llElemSlot = [&](llvm::Value* v) -> llvm::AllocaInst* {
        v = ensureNullableElementValue(llElemRef, v);
        llvm::AllocaInst* s = entryAlloca(v->getType());
        builder_.CreateStore(v, s);
        return s;
    };

    // --- AddFirst(item): LinkedListNode<T> ---
    if (m == "AddFirst" && call.args.size() == 1 && call.args[0].value) {
        llvm::Value* v = lowerExpr(*call.args[0].value);
        if (!v) return nullptr;
        llRetainElem(v, call.args[0].value.get());   // fix #2686
        llvm::AllocaInst* slot = llElemSlot(v);      // fix #4579
        auto* fn = declareC("bz_llist_add_first", ptrTy(),
                            {ptrTy(), ptrTy(), i64()});
        llvm::Value* r = builder_.CreateCall(fn,
            {recv, slot, builder_.getInt64(esz)}, "ll.addFirst");
        // fix #1970 / §28.4 / §28.6: for OwningLinkedList<T>.AddFirst the
        // element parameter is `owned`, so a bare owned local/param argument's
        // ownership just transferred into the list (the runtime mem-copies
        // from `slot` into the new node, then the node now owns `v`). Null the
        // caller's slot so the caller's null-guarded unwindOnly disposal —
        // which UnwindIneligibleWalker now leaves eligible for this callee shape
        // (calleeIsOwningCollectionTransfer) — is exactly-once: it skips the
        // just-moved value (no double-free with the list's element disposal on
        // Clear/Remove*) yet disposes a value still live at a throw that
        // PRECEDES the AddFirst. A BORROWING LinkedList<T>.AddFirst keeps the
        // caller as owner — no transfer, no nulling.
        if (isOwning) nullTransferredOwnedLocalSlot(call.args[0].value.get());
        return r;
    }
    // --- AddLast(item): LinkedListNode<T> ---
    if (m == "AddLast" && call.args.size() == 1 && call.args[0].value) {
        llvm::Value* v = lowerExpr(*call.args[0].value);
        if (!v) return nullptr;
        llRetainElem(v, call.args[0].value.get());   // fix #2686
        llvm::AllocaInst* slot = llElemSlot(v);      // fix #4579
        auto* fn = declareC("bz_llist_add_last", ptrTy(),
                            {ptrTy(), ptrTy(), i64()});
        llvm::Value* r = builder_.CreateCall(fn,
            {recv, slot, builder_.getInt64(esz)}, "ll.addLast");
        // fix #1970 / §28.4 / §28.6: see the AddFirst note above — the
        // OwningLinkedList AddLast path transfers ownership of the element;
        // null the caller's slot so the UnwindIneligibleWalker-eligible local
        // gets exactly-once disposal. Borrowing LinkedList<T>.AddLast stays
        // caller-owned and is NOT nulled.
        if (isOwning) nullTransferredOwnedLocalSlot(call.args[0].value.get());
        return r;
    }
    // --- AddBefore(node, item): LinkedListNode<T> ---
    if (m == "AddBefore" && call.args.size() == 2 &&
        call.args[0].value && call.args[1].value) {
        llvm::Value* node = lowerExpr(*call.args[0].value);
        llvm::Value* v    = lowerExpr(*call.args[1].value);
        if (!node || !v) return nullptr;
        llRetainElem(v, call.args[1].value.get());   // fix #2686
        llvm::AllocaInst* slot = llElemSlot(v);      // fix #4579
        auto* fn = declareC("bz_llist_add_before", ptrTy(),
                            {ptrTy(), ptrTy(), ptrTy(), i64()});
        llvm::Value* r = builder_.CreateCall(fn,
            {recv, node, slot, builder_.getInt64(esz)}, "ll.addBefore");
        // fix #1970 / §28.4 / §28.6: see the AddFirst note — for
        // OwningLinkedList.AddBefore the element (arg 1) is `owned` and has
        // just been transferred into the new list node; null the caller's
        // slot for a bare owned local/param. Borrowing LinkedList.AddBefore
        // takes the item by borrow — no null.
        if (isOwning) nullTransferredOwnedLocalSlot(call.args[1].value.get());
        return r;
    }
    // --- AddAfter(node, item): LinkedListNode<T> ---
    if (m == "AddAfter" && call.args.size() == 2 &&
        call.args[0].value && call.args[1].value) {
        llvm::Value* node = lowerExpr(*call.args[0].value);
        llvm::Value* v    = lowerExpr(*call.args[1].value);
        if (!node || !v) return nullptr;
        llRetainElem(v, call.args[1].value.get());   // fix #2686
        llvm::AllocaInst* slot = llElemSlot(v);      // fix #4579
        auto* fn = declareC("bz_llist_add_after", ptrTy(),
                            {ptrTy(), ptrTy(), ptrTy(), i64()});
        llvm::Value* r = builder_.CreateCall(fn,
            {recv, node, slot, builder_.getInt64(esz)}, "ll.addAfter");
        // fix #1970 / §28.4 / §28.6: see the AddFirst note — for
        // OwningLinkedList.AddAfter the element (arg 1) is `owned` and has
        // just been transferred into the new list node; null the caller's
        // slot for a bare owned local/param. Borrowing LinkedList.AddAfter
        // takes the item by borrow — no null.
        if (isOwning) nullTransferredOwnedLocalSlot(call.args[1].value.get());
        return r;
    }
    // --- PopFirst(): owned T / PopLast(): owned T (OwningLinkedList only) ---
    // fix #642 / stdlib §4.7: transfer ownership of the first/last element to the
    // caller; remove WITHOUT disposal.  Only valid on OwningLinkedList<T> (the
    // borrowing LinkedList<T> has no owned-return variants).
    if ((m == "PopFirst" || m == "PopLast") && call.args.empty()) {
        // Guard: raise InvalidOperationException when empty.
        auto* cntFn = declareC("bz_llist_count", i64(), {ptrTy()});
        llvm::Value* cnt = builder_.CreateCall(cntFn, {recv}, "llpop.cnt");
        llvm::Value* isEmpty = builder_.CreateICmpEQ(
            cnt, builder_.getInt64(0), "llpop.empty");
        emitGuardThrow(isEmpty, "InvalidOperationException");
        // Output slot for the copied element.
        llvm::AllocaInst* out = entryAlloca(elemLLTy);
        if (m == "PopFirst") {
            auto* fn = declareC("bz_llist_pop_first",
                llvm::Type::getVoidTy(llvm_), {ptrTy(), ptrTy(), i64()});
            builder_.CreateCall(fn, {recv, out, builder_.getInt64(esz)});
        } else {
            auto* fn = declareC("bz_llist_pop_last",
                llvm::Type::getVoidTy(llvm_), {ptrTy(), ptrTy(), i64()});
            builder_.CreateCall(fn, {recv, out, builder_.getInt64(esz)});
        }
        return builder_.CreateLoad(elemLLTy, out, "llpop.val");
    }
    // --- TryPopFirst(out owned item): bool / TryPopLast(out owned item): bool ---
    // fix #831 / stdlib §4.7: the TRY analogue of PopFirst / PopLast on
    // OwningLinkedList<T>. On a non-empty list write the first/last element to the
    // out param and remove it WITHOUT disposal (ownership transfers to the out
    // param), returning true; on empty return false and leave the out param
    // unchanged. Before this fix it fell through → malformed IR (bug16).
    if ((m == "TryPopFirst" || m == "TryPopLast") &&
        call.args.size() == 1 && call.args[0].value) {
        // fix #2938 / §22.3: see TryPop in Calls.cpp.
        llvm::Type* outTy = nullptr;
        std::vector<AccessorRefArg> outArgs;
        llvm::Value* outAddr = resolveOutAddrWithBracket(
            *call.args[0].value, outTy, outArgs);
        if (!outAddr) {
            emitAccessorRefArgWriteBacks(outArgs, /*emit=*/false);
            return nullptr;
        }
        auto* cntFn = declareC("bz_llist_count", i64(), {ptrTy()});
        llvm::Value* cnt = builder_.CreateCall(cntFn, {recv}, "lltrypop.cnt");
        llvm::Value* nonEmpty = builder_.CreateICmpNE(
            cnt, builder_.getInt64(0), "lltrypop.ne");
        auto* foundBB =
            llvm::BasicBlock::Create(llvm_, "lltrypop.found", currentFn_);
        auto* endBB =
            llvm::BasicBlock::Create(llvm_, "lltrypop.end", currentFn_);
        llvm::BasicBlock* preBB = builder_.GetInsertBlock();
        builder_.CreateCondBr(nonEmpty, foundBB, endBB);
        builder_.SetInsertPoint(foundBB);
        if (m == "TryPopFirst") {
            auto* fn = declareC("bz_llist_pop_first",
                llvm::Type::getVoidTy(llvm_), {ptrTy(), ptrTy(), i64()});
            builder_.CreateCall(fn, {recv, outAddr, builder_.getInt64(esz)});
        } else {
            auto* fn = declareC("bz_llist_pop_last",
                llvm::Type::getVoidTy(llvm_), {ptrTy(), ptrTy(), i64()});
            builder_.CreateCall(fn, {recv, outAddr, builder_.getInt64(esz)});
        }
        builder_.CreateBr(endBB);
        builder_.SetInsertPoint(endBB);
        // fix #2938 / §22.3: flush any bracket on the normal-return edge.
        emitAccessorRefArgWriteBacks(outArgs, /*emit=*/true);
        auto* phi = builder_.CreatePHI(i1(), 2, "lltrypop.r");
        phi->addIncoming(builder_.getTrue(), foundBB);
        phi->addIncoming(builder_.getFalse(), preBB);
        return phi;
    }
    // --- RemoveFirst() ---
    if (m == "RemoveFirst" && call.args.empty()) {
        // #602 / §28.11 v0.135 fix #619: use llElemPol.disposerThunk (hoisted above).
        // Interface elements now produce a non-null disposerThunk via ClassOwned.
        llvm::Value* disp =
            llElemDisposer(isOwning ? llElemPol.disposerThunk : nullptr);
        auto* fn = declareC("bz_llist_remove_first", llvm::Type::getVoidTy(llvm_),
                            {ptrTy(), ptrTy()});
        builder_.CreateCall(fn, {recv, disp});
        return llvm::ConstantInt::get(i32(), 0);
    }
    // --- RemoveLast() ---
    if (m == "RemoveLast" && call.args.empty()) {
        // #602 / §28.11 v0.134: use llElemPol.disposerThunk (hoisted above).
        llvm::Value* disp =
            llElemDisposer(isOwning ? llElemPol.disposerThunk : nullptr);
        auto* fn = declareC("bz_llist_remove_last", llvm::Type::getVoidTy(llvm_),
                            {ptrTy(), ptrTy()});
        builder_.CreateCall(fn, {recv, disp});
        return llvm::ConstantInt::get(i32(), 0);
    }
    // --- RemoveNode(node) ---
    if (m == "RemoveNode" && call.args.size() == 1 && call.args[0].value) {
        llvm::Value* node = lowerExpr(*call.args[0].value);
        if (!node) return nullptr;
        // #602 / §28.11 v0.134: use llElemPol.disposerThunk (hoisted above).
        llvm::Value* disp =
            llElemDisposer(isOwning ? llElemPol.disposerThunk : nullptr);
        auto* fn = declareC("bz_llist_remove_node", llvm::Type::getVoidTy(llvm_),
                            {ptrTy(), ptrTy(), ptrTy()});
        builder_.CreateCall(fn, {recv, node, disp});
        return llvm::ConstantInt::get(i32(), 0);
    }
    // --- Find(item): LinkedListNode<T>? ---
    // fix #566 / §4.6/§8.3/§9.1: the previous bz_llist_find path used memcmp on
    // raw element bytes, comparing bz_string* pointer values (not content) for
    // string elements and raw pointer bytes for records with string fields.
    // Mirror the #562 fix for LinkedList<T>.Contains: walk node-by-node applying
    // emitRemoveElementEquality for per-element comparison, then return the
    // matching node pointer (LinkedListNode<T>?) — null if no match found.
    // No disposal: Find only borrows elements (§4.6, LinkedList<T> is non-owning).
    if (m == "Find" && call.args.size() == 1 && call.args[0].value) {
        llvm::Value* v = lowerExpr(*call.args[0].value);
        if (!v) return nullptr;
        // #602 / §28.11 v0.134: use llElemRef and llElemIsClass from policy hoist.
        const sema::TypeRef* elemRef = llElemRef;
        // Spill the search target to a stable alloca slot.
        llvm::AllocaInst* tgtSlot = entryAlloca(v->getType());
        builder_.CreateStore(v, tgtSlot);
        // nodeSlot: current node pointer during the walk.
        llvm::AllocaInst* nodeSlot = entryAlloca(ptrTy());
        // foundNodeSlot: the matching node pointer, null if not found.
        llvm::AllocaInst* foundNodeSlot = entryAlloca(ptrTy());
        auto* nullP = llvm::ConstantPointerNull::get(ptrTy());
        builder_.CreateStore(nullP, foundNodeSlot);
        auto* firstFn = declareC("bz_llist_first_node", ptrTy(), {ptrTy()});
        auto* nextFn  = declareC("bz_llist_node_next",  ptrTy(), {ptrTy()});
        auto* valFn   = declareC("bz_llist_node_value_ptr", ptrTy(), {ptrTy()});
        llvm::Value* firstNode = builder_.CreateCall(firstFn, {recv}, "llf.first");
        builder_.CreateStore(firstNode, nodeSlot);
        auto* condBB  = llvm::BasicBlock::Create(llvm_, "llf.c",  currentFn_);
        auto* bodyBB  = llvm::BasicBlock::Create(llvm_, "llf.b",  currentFn_);
        auto* matchBB = llvm::BasicBlock::Create(llvm_, "llf.m",  currentFn_);
        auto* nextBB  = llvm::BasicBlock::Create(llvm_, "llf.n",  currentFn_);
        auto* endBB   = llvm::BasicBlock::Create(llvm_, "llf.e",  currentFn_);
        builder_.CreateBr(condBB);
        // Loop header: exit when node == null.
        builder_.SetInsertPoint(condBB);
        llvm::Value* curNode = builder_.CreateLoad(ptrTy(), nodeSlot, "llf.cur");
        builder_.CreateCondBr(
            builder_.CreateICmpNE(curNode, nullP, "llf.nnull"),
            bodyBB, endBB);
        // Loop body: load element, test structural equality.
        builder_.SetInsertPoint(bodyBB);
        llvm::Value* vp = builder_.CreateCall(valFn, {curNode}, "llf.vp");
        llvm::Value* el = builder_.CreateLoad(elemLLTy, vp, "llf.el");
        llvm::Value* tgt = builder_.CreateLoad(v->getType(), tgtSlot, "llf.tgt");
        llvm::Value* eq = emitRemoveElementEquality(el, tgt, elemRef, llElemIsClass);
        if (!eq) return nullptr;
        builder_.CreateCondBr(eq, matchBB, nextBB);
        // Match: save the node pointer and exit the loop.
        builder_.SetInsertPoint(matchBB);
        builder_.CreateStore(curNode, foundNodeSlot);
        builder_.CreateBr(endBB);
        // No match: advance to next node.
        builder_.SetInsertPoint(nextBB);
        llvm::Value* nxt = builder_.CreateCall(nextFn, {curNode}, "llf.next");
        builder_.CreateStore(nxt, nodeSlot);
        builder_.CreateBr(condBB);
        // Return the matching node pointer (null if not found) — LinkedListNode<T>?.
        builder_.SetInsertPoint(endBB);
        return builder_.CreateLoad(ptrTy(), foundNodeSlot, "llf.result");
    }
    // --- Count (no-arg call form, e.g. l.Count()) → int ---
    if (m == "Count" && call.args.empty()) {
        auto* fn = declareC("bz_llist_count", i64(), {ptrTy()});
        llvm::Value* cnt = builder_.CreateCall(fn, {recv}, "ll.cnt");
        return builder_.CreateTrunc(cnt, i32(), "ll.cnt.i32");
    }
    // --- Clear() --- disposes all elements (OwningLinkedList) and frees all
    // nodes, then re-allocates an empty list. Simplest: remove all from front.
    // We implement by calling bz_llist_free then re-alloc would break existing
    // pointer. Instead: walk and free each node via remove_first repeatedly.
    // Cleanest: add bz_llist_clear to runtime. For now, implement as a loop.
    if (m == "Clear" && call.args.empty()) {
        // #602 / §28.11 v0.135 fix #619: use llElemPol.disposerThunk (hoisted above).
        // Interface elements now produce a non-null disposerThunk via ClassOwned.
        llvm::Value* disp =
            llElemDisposer(isOwning ? llElemPol.disposerThunk : nullptr);
        // Loop: while (first_node != null) remove_first.
        auto* condBB = llvm::BasicBlock::Create(llvm_, "ll.clr.c", currentFn_);
        auto* bodyBB = llvm::BasicBlock::Create(llvm_, "ll.clr.b", currentFn_);
        auto* endBB  = llvm::BasicBlock::Create(llvm_, "ll.clr.e", currentFn_);
        builder_.CreateBr(condBB);
        builder_.SetInsertPoint(condBB);
        auto* firstFn = declareC("bz_llist_first_node", ptrTy(), {ptrTy()});
        llvm::Value* firstNode = builder_.CreateCall(firstFn, {recv}, "ll.first");
        llvm::Value* notNull = builder_.CreateICmpNE(
            firstNode, llvm::ConstantPointerNull::get(ptrTy()), "ll.notNull");
        builder_.CreateCondBr(notNull, bodyBB, endBB);
        builder_.SetInsertPoint(bodyBB);
        auto* remFn = declareC("bz_llist_remove_first",
                               llvm::Type::getVoidTy(llvm_), {ptrTy(), ptrTy()});
        builder_.CreateCall(remFn, {recv, disp});
        builder_.CreateBr(condBB);
        builder_.SetInsertPoint(endBB);
        return llvm::ConstantInt::get(i32(), 0);
    }
    // --- Remove(item): bool — per §4.6/§4.3 IOwningCollection ---
    // fix #565 / §4.6/§8.3/§9.1: the previous bz_llist_find path used memcmp on
    // raw element bytes, comparing bz_string* pointer values (not content) for
    // string elements and raw pointer bytes for records with string fields.
    // Mirror the #562 fix for LinkedList<T>.Contains: walk node-by-node applying
    // emitRemoveElementEquality for per-element comparison, then call
    // bz_llist_remove_node on the matching node (unlinks + optionally disposes).
    // Ownership semantics are preserved: OwningLinkedList passes its disposer;
    // non-owning LinkedList passes null (§4.6, borrows — no element disposal).
    if (m == "Remove" && call.args.size() == 1 && call.args[0].value) {
        llvm::Value* v = lowerExpr(*call.args[0].value);
        if (!v) return nullptr;
        // #602 / §28.11 v0.134: use llElemRef and llElemIsClass from policy hoist.
        const sema::TypeRef* elemRef = llElemRef;
        // Spill the search target to a stable alloca slot.
        llvm::AllocaInst* tgtSlot = entryAlloca(v->getType());
        builder_.CreateStore(v, tgtSlot);
        // nodeSlot: current node pointer during the walk.
        llvm::AllocaInst* nodeSlot = entryAlloca(ptrTy());
        // foundNodeSlot: the node pointer saved when a match is found.
        llvm::AllocaInst* foundNodeSlot = entryAlloca(ptrTy());
        auto* nullP = llvm::ConstantPointerNull::get(ptrTy());
        builder_.CreateStore(nullP, foundNodeSlot);
        auto* firstFn = declareC("bz_llist_first_node", ptrTy(), {ptrTy()});
        auto* nextFn  = declareC("bz_llist_node_next",  ptrTy(), {ptrTy()});
        auto* valFn   = declareC("bz_llist_node_value_ptr", ptrTy(), {ptrTy()});
        llvm::Value* firstNode = builder_.CreateCall(firstFn, {recv}, "llr.first");
        builder_.CreateStore(firstNode, nodeSlot);
        auto* condBB  = llvm::BasicBlock::Create(llvm_, "llr.c",  currentFn_);
        auto* bodyBB  = llvm::BasicBlock::Create(llvm_, "llr.b",  currentFn_);
        auto* matchBB = llvm::BasicBlock::Create(llvm_, "llr.m",  currentFn_);
        auto* nextBB  = llvm::BasicBlock::Create(llvm_, "llr.n",  currentFn_);
        auto* endBB   = llvm::BasicBlock::Create(llvm_, "llr.e",  currentFn_);
        builder_.CreateBr(condBB);
        // Loop header: exit when node == null OR a match was stored.
        builder_.SetInsertPoint(condBB);
        llvm::Value* curNode = builder_.CreateLoad(ptrTy(), nodeSlot, "llr.cur");
        builder_.CreateCondBr(
            builder_.CreateICmpNE(curNode, nullP, "llr.nnull"),
            bodyBB, endBB);
        // Loop body: load element, test equality.
        builder_.SetInsertPoint(bodyBB);
        llvm::Value* vp = builder_.CreateCall(valFn, {curNode}, "llr.vp");
        llvm::Value* el = builder_.CreateLoad(elemLLTy, vp, "llr.el");
        llvm::Value* tgt = builder_.CreateLoad(v->getType(), tgtSlot, "llr.tgt");
        llvm::Value* eq = emitRemoveElementEquality(el, tgt, elemRef, llElemIsClass);
        if (!eq) return nullptr;
        builder_.CreateCondBr(eq, matchBB, nextBB);
        // Match: save the node pointer and exit the loop.
        builder_.SetInsertPoint(matchBB);
        builder_.CreateStore(curNode, foundNodeSlot);
        builder_.CreateBr(endBB);
        // No match: advance to next node.
        builder_.SetInsertPoint(nextBB);
        llvm::Value* nxt = builder_.CreateCall(nextFn, {curNode}, "llr.next");
        builder_.CreateStore(nxt, nodeSlot);
        builder_.CreateBr(condBB);
        // After loop: check if a match was found (foundNodeSlot != null).
        builder_.SetInsertPoint(endBB);
        llvm::Value* foundNode = builder_.CreateLoad(ptrTy(), foundNodeSlot,
                                                     "llr.fnd");
        llvm::Value* found = builder_.CreateICmpNE(foundNode, nullP, "llr.found");
        // If found, call RemoveNode (which unlinks + optionally disposes).
        // OwningLinkedList disposes the element; LinkedList borrows, no disposal.
        // #602 / §28.11 v0.135 fix #619: use llElemPol.disposerThunk from policy
        // hoist.  Interface elements now produce a non-null disposerThunk.
        llvm::Value* disp =
            llElemDisposer(isOwning ? llElemPol.disposerThunk : nullptr);
        auto* remBB  = llvm::BasicBlock::Create(llvm_, "llr.rem", currentFn_);
        auto* contBB = llvm::BasicBlock::Create(llvm_, "llr.cont", currentFn_);
        builder_.CreateCondBr(found, remBB, contBB);
        builder_.SetInsertPoint(remBB);
        auto* remFn = declareC("bz_llist_remove_node",
                               llvm::Type::getVoidTy(llvm_),
                               {ptrTy(), ptrTy(), ptrTy()});
        builder_.CreateCall(remFn, {recv, foundNode, disp});
        builder_.CreateBr(contBB);
        builder_.SetInsertPoint(contBB);
        // Return i1 bool — the branch condition MUST be i1 (M3 fix).
        return found;
    }
    // --- Contains(item): bool — node-walk with structural equality ---
    // fix #562 / §4.6/§8.3/§9.1: the previous bz_llist_find path used memcmp on
    // raw element bytes, comparing bz_string* pointer values (not content) for
    // string elements and raw pointer bytes for records with string fields.
    // Mirror the #560 fix for `x in LinkedList<T>`: walk node-by-node applying
    // emitRemoveElementEquality so that string content equality (§4.6) and
    // record structural equality (§8.3/§9.1) are correctly observed.
    if (m == "Contains" && call.args.size() == 1 && call.args[0].value) {
        llvm::Value* v = lowerExpr(*call.args[0].value);
        if (!v) return nullptr;
        // #602 / §28.11 v0.134: use llElemRef and llElemIsClass from policy hoist.
        const sema::TypeRef* elemRef = llElemRef;
        // Spill the search target to a stable alloca slot (same pattern as the
        // `in` operator LinkedList path in LlvmBackend_Exprs.cpp #2764).
        llvm::AllocaInst* tgtSlot = entryAlloca(v->getType());
        builder_.CreateStore(v, tgtSlot);
        llvm::AllocaInst* nodeSlot = entryAlloca(ptrTy());
        auto* firstFn = declareC("bz_llist_first_node", ptrTy(), {ptrTy()});
        auto* nextFn  = declareC("bz_llist_node_next",  ptrTy(), {ptrTy()});
        auto* valFn   = declareC("bz_llist_node_value_ptr", ptrTy(), {ptrTy()});
        auto* nullP = llvm::ConstantPointerNull::get(ptrTy());
        llvm::Value* firstNode = builder_.CreateCall(firstFn, {recv}, "llc.first");
        builder_.CreateStore(firstNode, nodeSlot);
        auto* condBB  = llvm::BasicBlock::Create(llvm_, "llc.c",  currentFn_);
        auto* bodyBB  = llvm::BasicBlock::Create(llvm_, "llc.b",  currentFn_);
        auto* foundBB = llvm::BasicBlock::Create(llvm_, "llc.f",  currentFn_);
        auto* nextBB  = llvm::BasicBlock::Create(llvm_, "llc.n",  currentFn_);
        auto* endBB   = llvm::BasicBlock::Create(llvm_, "llc.e",  currentFn_);
        builder_.CreateBr(condBB);
        builder_.SetInsertPoint(condBB);
        llvm::Value* curNode = builder_.CreateLoad(ptrTy(), nodeSlot, "llc.cur");
        builder_.CreateCondBr(
            builder_.CreateICmpNE(curNode, nullP, "llc.nnull"),
            bodyBB, endBB);
        builder_.SetInsertPoint(bodyBB);
        llvm::Value* vp = builder_.CreateCall(valFn, {curNode}, "llc.vp");
        llvm::Value* el = builder_.CreateLoad(elemLLTy, vp, "llc.el");
        llvm::Value* tgt = builder_.CreateLoad(v->getType(), tgtSlot, "llc.tgt");
        llvm::Value* eq = emitRemoveElementEquality(el, tgt, elemRef, llElemIsClass);
        if (!eq) return nullptr;
        builder_.CreateCondBr(eq, foundBB, nextBB);
        builder_.SetInsertPoint(foundBB);
        builder_.CreateBr(endBB);
        builder_.SetInsertPoint(nextBB);
        llvm::Value* nxt = builder_.CreateCall(nextFn, {curNode}, "llc.next");
        builder_.CreateStore(nxt, nodeSlot);
        builder_.CreateBr(condBB);
        builder_.SetInsertPoint(endBB);
        auto* phi = builder_.CreatePHI(i1(), 2, "llc.result");
        phi->addIncoming(builder_.getTrue(),  foundBB);
        phi->addIncoming(builder_.getFalse(), condBB);
        return phi;
    }
    return nullptr;
}


// §4.15 (#549): lower Array<T>.AsSpan()/AsReadOnlySpan() constructor overloads
// and ArraySpan<T>/ReadOnlyArraySpan<T> instance method calls.
//
// LLVM representation: { ptr source, i32 start, i32 length }
//   source — the underlying bz_array_t* pointer (borrowed; never freed by the span)
//   start  — the offset into the array (first element of the view)
//   length — the number of elements in the view
//
// The span is a by-value record (no heap allocation).  Disposing the span does
// NOT free the source array.  Per §4.15: "The `Source` field carries a borrow
// of the array; per the borrow-bearing-record rule, the span's lifetime is
// bounded by the source's lifetime."
//
// Overloads:
//   AsSpan()                  → ArraySpan<T>(arr, 0, arr.Count)
//   AsSpan(start)             → ArraySpan<T>(arr, start, arr.Count - start)
//   AsSpan(start, length)     → ArraySpan<T>(arr, start, length)
//   AsReadOnlySpan()          → ReadOnlyArraySpan<T>(arr, 0, arr.Count)
//   … (same pattern for AsReadOnlySpan overloads)
//
// Instance methods implemented here:
//   Length        → load field 2 (i32), sext to i32 (already i32)
//   IsEmpty       → Length == 0
//   Slice(s,l)    → build new span(Source, Start+s, l)
//   AsReadOnly()  → build ReadOnlyArraySpan with same (Source, Start, Length)
//   IndexOf(v)    → linear scan from Start to Start+Length; return first match
//   Contains(v)   → IndexOf(v) >= 0
//   Fill(v)       → write v into every slot (ArraySpan only)
//   CopyFrom(src) → copy src slots into this span's slots (ArraySpan only)
//   CopyTo(dst)   → copy this span's slots into dst (both span types)
//   Reverse()     → reverse elements in place (ArraySpan only)
//   SequenceEqual → compare pairwise
//
// Deferred (not implemented in this slice; guarded by a clean diagnostic):
//   <none – all declared members are implemented or emit a clear error>
//
// The span indexer get (`span[i]`) and set (`span[i] := v`) are handled in
// lowerCollectionIndexerGet / lowerCollectionIndexerSet in LlvmBackend_Assign.cpp.
// The for-in loop (`for var x in span`) is handled in LlvmBackend_Stmts.cpp.
llvm::Value* Emitter::tryLowerArraySpanCall(const ast::CallExpr& call) {
    if (!call.callee) return nullptr;
    auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(call.callee.get());
    if (!ma || !ma->target) return nullptr;
    const auto& m = ma->member;

    auto* recvTy = ctx_.exprType(*ma->target);
    if (!recvTy || recvTy->kind != sema::TypeRef::Kind::Named ||
        recvTy->declaration)
        return nullptr;
    if (!recvTy->astNode || !recvTy->astNode->name ||
        recvTy->astNode->name->parts.empty())
        return nullptr;
    std::string_view rcn = recvTy->headName();

    // §3.3 (#883, #971 item 1): ReadOnlyStringSpan instance methods (call form).
    // The struct is {string-ptr, i32 start, i32 length}; Length/IsEmpty are field
    // reads and Slice reconstructs a sub-view. The indexer is handled in
    // lowerCollectionIndexerGet (loadCodePoint).
    //
    // The search/compare family lowers to the bz_strspan_* runtime helpers, which
    // take each operand as an explicit (source string, start, len) CODE-POINT
    // triple and compare code point by code point — so the operands' storage
    // widths (§4.6 adaptive 1/2/4 bytes) are irrelevant and may differ. That is
    // the whole reason a span over a string cannot reuse the buffer-backed span
    // machinery (§3.3 "Why a separate type from ReadOnlySpan<char>").
    //
    // §3.3 defines the `string` overloads as "equivalent to calling the
    // ReadOnlyStringSpan overload after value.AsSpan()", so a string operand is
    // passed as the triple (ptr, 0, its code-point length) — the same call, no
    // allocation, no separate runtime entry point.
    if (rcn == "ReadOnlyStringSpan") {
        llvm::Value* spanVal = lowerExpr(*ma->target);
        if (!spanVal) return nullptr;
        auto* spanSt = arraySpanStructType();
        auto* spanSlot = entryAlloca(spanSt);
        builder_.CreateStore(spanVal, spanSlot);
        auto fSrc = [&] { return builder_.CreateLoad(ptrTy(),
            builder_.CreateStructGEP(spanSt, spanSlot, 0), "ss.src"); };
        auto fBase = [&] { return builder_.CreateLoad(i32(),
            builder_.CreateStructGEP(spanSt, spanSlot, 1), "ss.st"); };
        auto fLen = [&] { return builder_.CreateLoad(i32(),
            builder_.CreateStructGEP(spanSt, spanSlot, 2), "ss.len"); };
        if (m == "Length" && call.args.empty()) return fLen();
        if (m == "IsEmpty" && call.args.empty())
            return builder_.CreateICmpEQ(fLen(), builder_.getInt32(0), "ss.empty");
        // §3.3: Slice(start, length), and — since v0.195 — Slice(start), which is
        // `Slice(start, Length - start)`. The one-arg form previously fell out of
        // this branch to `return nullptr`, leaving the result local undefined
        // (an accept-trap: it compiled and produced garbage at run time).
        if (m == "Slice" && (call.args.size() == 1 || call.args.size() == 2) &&
            call.args[0].value &&
            (call.args.size() == 1 || call.args[1].value)) {
            // #2614 / §7.4: a flow-narrowed `int?` start/length is admitted by
            // sema as the non-nullable `int` parameter but still lowers to its
            // {i8,i32} box; unwrap before the trunc below, which would otherwise
            // emit `trunc {i8,i32} to i32` and fail verifyModule.
            llvm::Value* st = unwrapNarrowedNullableOperand(
                lowerExpr(*call.args[0].value), call.args[0].value.get());
            if (!st) return nullptr;
            if (!st->getType()->isIntegerTy(32))
                st = builder_.CreateTrunc(st, i32());
            llvm::Value* b = fBase();
            llvm::Value* L = fLen();
            emitGuardThrow(builder_.CreateOr(
                builder_.CreateICmpSLT(st, builder_.getInt32(0)),
                builder_.CreateICmpSGT(st, L)), "IndexOutOfRangeException");
            llvm::Value* avail = builder_.CreateSub(L, st);
            llvm::Value* sl = avail;   // v0.195 one-arg form: to the end
            if (call.args.size() == 2) {
                sl = unwrapNarrowedNullableOperand(
                    lowerExpr(*call.args[1].value), call.args[1].value.get());
                if (!sl) return nullptr;
                if (!sl->getType()->isIntegerTy(32))
                    sl = builder_.CreateTrunc(sl, i32());
                emitGuardThrow(builder_.CreateOr(
                    builder_.CreateICmpSLT(sl, builder_.getInt32(0)),
                    builder_.CreateICmpSGT(sl, avail)), "IndexOutOfRangeException");
            }
            return buildArraySpan(fSrc(), builder_.CreateAdd(b, st), sl);
        }
        // §3.3 Materialization: ToString() → a fresh string holding the span's
        // code points. bz_str_substring already slices a source string by
        // code-point index at the narrowest storage width that fits the result,
        // which is exactly this operation; it mints at rc := 1, and
        // isPlusOneStringProducer records that so the consumer transfers rather
        // than retains. (§3.3 permits returning the source itself when the span
        // covers all of it; always materializing is the simpler correct choice.)
        if (m == "ToString" && call.args.empty()) {
            auto* fn = declareC("bz_str_substring", ptrTy(), {ptrTy(), i64(), i64()});
            return builder_.CreateCall(
                fn, {fSrc(), builder_.CreateSExt(fBase(), i64(), "ss.ts.st"),
                     builder_.CreateSExt(fLen(), i64(), "ss.ts.len")},
                "ss.tostr");
        }
        // The search/compare family. Sema has already rejected every operand
        // shape §3.3 does not declare, so exactly one of the three cases below
        // applies to a well-typed call.
        if ((m == "IndexOf" || m == "Contains" || m == "StartsWith" ||
             m == "EndsWith" || m == "SequenceEqual") &&
            call.args.size() == 1 && call.args[0].value) {
            const auto* at = ctx_.exprType(*call.args[0].value);
            bool argIsChar = at && at->kind == sema::TypeRef::Kind::Primitive &&
                             at->primitive == sema::PrimitiveType::Char;
            bool argIsString = at && at->kind == sema::TypeRef::Kind::Primitive &&
                               at->primitive == sema::PrimitiveType::String;
            llvm::Value* selfSrc = fSrc();
            llvm::Value* selfSt  = builder_.CreateSExt(fBase(), i64(), "ss.q.st");
            llvm::Value* selfLen = builder_.CreateSExt(fLen(), i64(), "ss.q.len");
            // §3.3: IndexOf(char) / Contains(char) — the code-point scan. These
            // are the ONLY two methods with a char overload.
            if (argIsChar) {
                llvm::Value* cp = lowerExpr(*call.args[0].value);
                if (!cp) return nullptr;
                if (!cp->getType()->isIntegerTy(32))
                    cp = builder_.CreateZExtOrTrunc(cp, i32());
                auto* fn = declareC("bz_strspan_indexof_cp", i32(),
                                    {ptrTy(), i64(), i64(), i32()});
                llvm::Value* idx = builder_.CreateCall(
                    fn, {selfSrc, selfSt, selfLen, cp}, "ss.iofc");
                if (m == "Contains")
                    return builder_.CreateICmpSGE(idx, builder_.getInt32(0),
                                                  "ss.hasc");
                return idx;
            }
            // The needle's (source, start, len) triple: a string operand is the
            // whole string (start 0), a span operand is its own sub-range.
            llvm::Value* nSrc = nullptr;
            llvm::Value* nSt  = nullptr;
            llvm::Value* nLen = nullptr;
            if (argIsString) {
                nSrc = lowerExpr(*call.args[0].value);
                if (!nSrc) return nullptr;
                nSt  = builder_.getInt64(0);
                // -1 = the BZ_STRSPAN_WHOLE sentinel: "the whole string",
                // resolved inside the runtime helper AFTER its null guard.
                // Reading the length here instead (loadStringLen) would
                // dereference the operand, and the builtin string-method surface
                // still admits an un-narrowed `string?` argument — the sibling
                // `"h".IndexOf(nullString)` compiles today and returns -1 rather
                // than faulting, so this family must not fault either.
                nLen = builder_.getInt64(-1);
            } else {
                llvm::Value* nv = lowerExpr(*call.args[0].value);
                if (!nv || !nv->getType()->isStructTy()) return nullptr;
                auto* nSlot = entryAlloca(spanSt);
                builder_.CreateStore(nv, nSlot);
                nSrc = builder_.CreateLoad(ptrTy(),
                    builder_.CreateStructGEP(spanSt, nSlot, 0), "ss.n.src");
                nSt = builder_.CreateSExt(builder_.CreateLoad(i32(),
                    builder_.CreateStructGEP(spanSt, nSlot, 1)), i64(), "ss.n.st");
                nLen = builder_.CreateSExt(builder_.CreateLoad(i32(),
                    builder_.CreateStructGEP(spanSt, nSlot, 2)), i64(), "ss.n.len");
            }
            auto callSpanFn = [&](const char* name) {
                auto* fn = declareC(name, i32(),
                                    {ptrTy(), i64(), i64(), ptrTy(), i64(), i64()});
                return builder_.CreateCall(
                    fn, {selfSrc, selfSt, selfLen, nSrc, nSt, nLen}, "ss.q");
            };
            // §3.3: Contains(value) is defined as IndexOf(value) >= 0.
            if (m == "IndexOf") return callSpanFn("bz_strspan_indexof");
            if (m == "Contains")
                return builder_.CreateICmpSGE(callSpanFn("bz_strspan_indexof"),
                                              builder_.getInt32(0), "ss.has");
            const char* fnName = m == "StartsWith" ? "bz_strspan_startswith"
                               : m == "EndsWith"   ? "bz_strspan_endswith"
                                                   : "bz_strspan_seqequal";
            return builder_.CreateTrunc(callSpanFn(fnName), i1(), "ss.qb");
        }
        return nullptr;
    }

    // Helper: get the element size for a span over Array<T>.
    // Uses recvTy->genericArgs[0] (the element type T of the span).
    auto spanElemSize = [&]() -> int64_t {
        if (recvTy->genericArgs.empty() || !recvTy->genericArgs[0]) return 8;
        return collectionElemSize(recvTy);
    };
    auto spanElemTy = [&]() -> llvm::Type* {
        if (recvTy->genericArgs.empty() || !recvTy->genericArgs[0]) return i32();
        llvm::Type* t = lowerTypeRef(recvTy->genericArgs[0]);
        return t ? t : i32();
    };

    // -----------------------------------------------------------------------
    // Array<T>.AsSpan() / AsReadOnlySpan() — construct a span from an array.
    // §4.14 / #636: OwningBuffer<T> uses the same bz_array_t* backing as
    // Array<T>, so OwningBuffer<T>.AsSpan() / AsReadOnlySpan() lower identically.
    // The resulting span struct is { ptr source, i32 start, i32 length } —
    // the same bz.arrayspan used for ArraySpan<T> / ReadOnlyArraySpan<T>.
    // Previously a duplicate path in tryLowerSerializationCall handled only
    // the byte-element case and lacked bounds checks; delegating here closes
    // that gap and gives a single, element-width–correct lowering for all T.
    // -----------------------------------------------------------------------
    if ((rcn == "Array" || rcn == "OwningBuffer") &&
        (m == "AsSpan" || m == "AsReadOnlySpan")) {
        // The span element type and size are those of the array (genericArgs[0]).
        llvm::Value* arr = lowerExpr(*ma->target);
        if (!arr) return nullptr;
        auto* lenFn = declareC("bz_array_len", i64(), {ptrTy()});
        llvm::Value* count64 = builder_.CreateCall(lenFn, {arr}, "arr.count");
        llvm::Value* count = builder_.CreateTrunc(count64, i32(), "arr.count32");
        // No-arg form: full array [0, Count)
        if (call.args.empty()) {
            return buildArraySpan(arr, builder_.getInt32(0), count);
        }
        // §4.14 (#746 R41-J): AsSpan(r: Range<int>) — Start = r.Start, Length =
        // the range's element count (End - Start + 1 inclusive, End - Start
        // half-open). Previously the Range fell into the (start) form below,
        // which read only its low bound and set Length = Count - Start, ignoring
        // the End entirely (and never bounds-checking it).
        if (call.args.size() == 1 && call.args[0].value &&
            isRangeType(ctx_.exprType(*call.args[0].value))) {
            llvm::Value* rv = lowerExpr(*call.args[0].value);
            if (!rv || !rv->getType()->isStructTy()) return nullptr;
            llvm::Value* lo = builder_.CreateTrunc(
                builder_.CreateExtractValue(rv, {0u}), i32(), "sp.r.lo");
            llvm::Value* hi = builder_.CreateTrunc(
                builder_.CreateExtractValue(rv, {1u}), i32(), "sp.r.hi");
            llvm::Value* incl = builder_.CreateTrunc(
                builder_.CreateExtractValue(rv, {3u}), i1(), "sp.r.incl");
            llvm::Value* span0 = builder_.CreateSub(hi, lo, "sp.r.span");
            llvm::Value* len = builder_.CreateSelect(
                incl, builder_.CreateAdd(span0, builder_.getInt32(1)), span0,
                "sp.r.len");
            // bounds: Start ∈ [0, Count], len ≥ 0, Start+len ∈ [0, Count].
            llvm::Value* startNeg =
                builder_.CreateICmpSLT(lo, builder_.getInt32(0));
            llvm::Value* lenNeg =
                builder_.CreateICmpSLT(len, builder_.getInt32(0));
            llvm::Value* endOOB =
                builder_.CreateICmpSGT(builder_.CreateAdd(lo, len), count);
            emitGuardThrow(
                builder_.CreateOr(builder_.CreateOr(startNeg, lenNeg), endOOB),
                "IndexOutOfRangeException");
            return buildArraySpan(arr, lo, len);
        }
        // (start) form: [start, Count)
        if (call.args.size() == 1 && call.args[0].value) {
            llvm::Value* start = lowerExpr(*call.args[0].value);
            if (!start) return nullptr;
            if (!start->getType()->isIntegerTy(32))
                start = builder_.CreateTrunc(start, i32());
            // bounds check: start must be in [0, Count]
            llvm::Value* startNeg = builder_.CreateICmpSLT(
                start, builder_.getInt32(0), "span.stneg");
            llvm::Value* startOOB = builder_.CreateICmpSGT(
                start, count, "span.stoob");
            emitGuardThrow(builder_.CreateOr(startNeg, startOOB),
                           "IndexOutOfRangeException");
            llvm::Value* len = builder_.CreateSub(count, start, "span.len");
            return buildArraySpan(arr, start, len);
        }
        // (start, length) form — also handles Range<int> (deferred; treat as 2-arg)
        if (call.args.size() == 2 && call.args[0].value && call.args[1].value) {
            llvm::Value* start = lowerExpr(*call.args[0].value);
            llvm::Value* len   = lowerExpr(*call.args[1].value);
            if (!start || !len) return nullptr;
            if (!start->getType()->isIntegerTy(32))
                start = builder_.CreateTrunc(start, i32());
            if (!len->getType()->isIntegerTy(32))
                len = builder_.CreateTrunc(len, i32());
            // bounds check: start ∈ [0, Count], len ∈ [0, Count-start]
            llvm::Value* startNeg = builder_.CreateICmpSLT(
                start, builder_.getInt32(0));
            llvm::Value* startOOB = builder_.CreateICmpSGT(start, count);
            emitGuardThrow(builder_.CreateOr(startNeg, startOOB),
                           "IndexOutOfRangeException");
            llvm::Value* avail = builder_.CreateSub(count, start);
            llvm::Value* lenNeg  = builder_.CreateICmpSLT(
                len, builder_.getInt32(0));
            llvm::Value* lenOOB  = builder_.CreateICmpSGT(len, avail);
            emitGuardThrow(builder_.CreateOr(lenNeg, lenOOB),
                           "IndexOutOfRangeException");
            return buildArraySpan(arr, start, len);
        }
        return nullptr;
    }

    // -----------------------------------------------------------------------
    // ArraySpan<T> / ReadOnlyArraySpan<T> (§4.15, borrowing) and
    // Span<T> / ReadOnlySpan<T> (§4.14, owning over OwningBuffer) instance method
    // calls. All four share the bz.arrayspan layout, so the read-only / value
    // surface (Length, IsEmpty, Slice, IndexOf, Contains, SequenceEqual,
    // AsReadOnly, CopyTo, CopyFrom, Fill, Reverse) lowers identically. The owning
    // Span<classT> additionally exposes Clear / MoveFrom / MoveTo (§4.14).
    // -----------------------------------------------------------------------
    if (rcn != "ArraySpan" && rcn != "ReadOnlyArraySpan" &&
        rcn != "Span" && rcn != "ReadOnlySpan") return nullptr;
    bool isWritable   = (rcn == "ArraySpan" || rcn == "Span");
    bool isOwningSpan = (rcn == "Span");

    llvm::Value* spanVal = lowerExpr(*ma->target);
    if (!spanVal) return nullptr;

    // Spill the span value to an alloca once so repeated GEPs are stable.
    auto* spanSt = arraySpanStructType();
    auto* spanSlot = entryAlloca(spanSt);
    builder_.CreateStore(spanVal, spanSlot);
    auto loadSource = [&]() {
        return builder_.CreateLoad(ptrTy(),
            builder_.CreateStructGEP(spanSt, spanSlot, 0), "sp.src");
    };
    auto loadStart = [&]() {
        return builder_.CreateLoad(i32(),
            builder_.CreateStructGEP(spanSt, spanSlot, 1), "sp.st");
    };
    auto loadLen = [&]() {
        return builder_.CreateLoad(i32(),
            builder_.CreateStructGEP(spanSt, spanSlot, 2), "sp.len");
    };

    // #602 / §28.11 v0.134: hoist one element-slot policy lookup for ArraySpan
    // operations that need per-element equality (IndexOf / Contains /
    // SequenceEqual). Spans are always in the borrowing family (they never own
    // elements — §4.15: "the span's lifetime is bounded by the source").
    const sema::TypeRef* spanElemRef =
        (recvTy && !recvTy->genericArgs.empty()) ? recvTy->genericArgs[0] : nullptr;
    const ElementPolicy spanElemPol =
        elementPolicyFor(spanElemRef, /*owningFamily=*/false);
    // spanElemIsClass: class-or-interface element — per §28.11 v0.135 fix #619.
    // elementPolicyFor returns ClassBorrow for interface elements (spans are always
    // borrowing — §4.15). collectionElemIsClass is also widened.
    bool spanElemIsClass =
        spanElemPol.cat == ElementPolicy::Category::ClassOwned ||
        spanElemPol.cat == ElementPolicy::Category::ClassBorrow;

    // -- ToString ---------------------------------------------------------------
    // fix #2661 (spec §8.5, v0.252): the §8.5 positional-record default, which
    // §8.5 states is generated for "**every** type of the applicable kind,
    // wherever declared — user modules and the standard library alike". Was an
    // ACCEPT-TRAP: sema typed the call `string` (the universal-ToString rule)
    // and nothing lowered it, so the enclosing statement was silently dropped
    // (#2311 measured `Console.WriteLine(sp.ToString())` printing nothing, exit
    // 0, no diagnostic). #2311's holding position was a deferral diagnostic;
    // this is the lowering that replaces it. Delegates to the ONE renderer the
    // §18.5 interpolation hole and the boxed §4.16 receiver also use, so the
    // three spellings of this default cannot drift.
    if (m == "ToString" && call.args.empty())
        return emitSpanToString(spanVal, rcn);

    // -- Length / IsEmpty -------------------------------------------------------
    if (m == "Length" && call.args.empty()) {
        return loadLen();  // i32
    }
    if (m == "IsEmpty" && call.args.empty()) {
        return builder_.CreateICmpEQ(loadLen(), builder_.getInt32(0), "sp.empty");
    }

    // -- Slice(start: int, length: int) / Slice(start: int) --------------------
    // stdlib §4.14/§4.15: both overloads, on all four buffer/array-backed spans.
    // The v0.195 one-arg form is defined as `Slice(start, Length - start)`.
    //
    // #2312: this branch required `args.size() == 2`, so the one-arg form fell
    // through to the function's `return nullptr` and its value was never
    // produced — the enclosing statement was silently DROPPED (verified on
    // e87d868: `Console.WriteLine(ros.Slice(2).Length…)` printed nothing, and
    // `return ros.Slice(2).Length` returned 0 instead of 3, with `bezelc`
    // exiting 0 either way). Modelled on the ReadOnlyStringSpan.Slice site
    // above, which #971/9036830 already fixed the same way.
    //
    // THE OPERAND PRECONDITION, and where it does and does NOT come from.
    // `checkBuiltinSpanCallArgs` (#2614) rejects a PRIMITIVE operand that is not
    // assignable to `int` (including an un-narrowed `int?`), and sema's span
    // `Slice` typing branch now rejects every arity other than 1 or 2. Neither
    // rejects a NON-primitive operand: `span.Slice(r: Range<int>)` is undeclared
    // in §4.14/§4.15 but is accepted today and relied on by the ownership suite
    // (consume_site_freeze_span_1726 / _reassign_1741), so rejecting it would be
    // a false reject outside this issue's scope — filed, not swept in.
    //
    // Therefore the trunc below may NOT assume an integer, and the explicit
    // `isIntegerTy()` tests are load-bearing rather than defensive: they return
    // null instead of emitting `trunc {i32,i32} to i32` and truncating a struct
    // into a plausible-looking start index — which is the same
    // silent-wrong-value class this issue is about, one level down. Do not drop
    // them on the assumption that sema has already filtered the operand.
    //
    // ⚠ RETURNING NULL IS NOT RELIABLY LOUD, and an earlier version of this
    // comment claimed it was. The #2434/#2491 `internal:` notes are emitted
    // ONLY from local-decl-shaped and return-shaped positions —
    // LlvmBackend_Setup.cpp:9408, LlvmBackend_Stmts.cpp:1909 and :6319,
    // LlvmBackend_Calls2.cpp:8891, LlvmBackend_Assign.cpp:3472 — and NONE of
    // them covers an expression-statement or a call-argument position. In those
    // positions a null simply drops the enclosing statement with no diagnostic
    // at all and `bezelc` still exits 0, which is exactly the #2312 symptom this
    // branch was written to fix (`Console.WriteLine(ros.Slice(2).Length…)`
    // printed nothing). So a null here is the LEAST-BAD available outcome, not a
    // loud one: it forecloses the miscompile but does not guarantee a
    // diagnostic. #2662 tracks giving the Range operand a real answer.
    if (m == "Slice" && (call.args.size() == 1 || call.args.size() == 2) &&
        call.args[0].value &&
        (call.args.size() == 1 || call.args[1].value)) {
        llvm::Value* src  = loadSource();
        llvm::Value* base = loadStart();
        llvm::Value* slen = loadLen();
        // #2614 / §7.4: unwrap a flow-narrowed `int?` start/length to its bare
        // payload before the truncs below — see the ReadOnlyStringSpan.Slice
        // site above; identical shape, identical defect.
        // Lower BOTH operands before any guard, preserving the pre-#2312
        // left-to-right argument-evaluation order of the two-arg form: a side
        // effect in `length` must still happen before an out-of-range `start`
        // throws, exactly as it did before this branch grew the one-arg case.
        llvm::Value* start = unwrapNarrowedNullableOperand(
            lowerExpr(*call.args[0].value), call.args[0].value.get());
        if (!start || !start->getType()->isIntegerTy()) return nullptr;
        llvm::Value* explicitLen = nullptr;
        if (call.args.size() == 2) {
            explicitLen = unwrapNarrowedNullableOperand(
                lowerExpr(*call.args[1].value), call.args[1].value.get());
            if (!explicitLen || !explicitLen->getType()->isIntegerTy())
                return nullptr;
            if (!explicitLen->getType()->isIntegerTy(32))
                explicitLen = builder_.CreateTrunc(explicitLen, i32());
        }
        if (!start->getType()->isIntegerTy(32))
            start = builder_.CreateTrunc(start, i32());
        // bounds check against this span's length
        llvm::Value* startNeg = builder_.CreateICmpSLT(start, builder_.getInt32(0));
        llvm::Value* startOOB = builder_.CreateICmpSGT(start, slen);
        emitGuardThrow(builder_.CreateOr(startNeg, startOOB),
                       "IndexOutOfRangeException");
        llvm::Value* avail = builder_.CreateSub(slen, start);
        // v0.195 one-arg form: to the end of the span. `avail` is exactly
        // `Length - start`, and the guard above has established
        // `0 <= start <= Length`, so it is in range by construction and needs
        // no second bounds check.
        llvm::Value* slen2 = avail;
        if (explicitLen) {
            slen2 = explicitLen;
            llvm::Value* slen2Neg = builder_.CreateICmpSLT(slen2, builder_.getInt32(0));
            llvm::Value* slen2OOB = builder_.CreateICmpSGT(slen2, avail);
            emitGuardThrow(builder_.CreateOr(slen2Neg, slen2OOB),
                           "IndexOutOfRangeException");
        }
        llvm::Value* newStart = builder_.CreateAdd(base, start, "sl.newst");
        return buildArraySpan(src, newStart, slen2);
    }

    // -- AsReadOnly(): ArraySpan<T> → ReadOnlyArraySpan<T> -------------------
    if (m == "AsReadOnly" && isWritable && call.args.empty()) {
        // Same bit pattern; the type distinction is static only (§4.15).
        return spanVal;  // struct value is identical for both span types
    }

    // -- IndexOf(value: T) → int -----------------------------------------------
    if (m == "IndexOf" && call.args.size() == 1 && call.args[0].value) {
        llvm::Value* v = lowerExpr(*call.args[0].value);
        if (!v) return nullptr;
        int64_t esz = spanElemSize();
        llvm::Type* ety = spanElemTy();
        llvm::Value* src  = loadSource();
        llvm::Value* base = loadStart();
        llvm::Value* slen = loadLen();
        // idx32 = 0; loop while idx32 < slen
        auto* ptrAtFn = declareC("bz_array_ptr_at", ptrTy(), {ptrTy(), i64(), i64()});
        auto* idxSlot = entryAlloca(i32());
        builder_.CreateStore(builder_.getInt32(0), idxSlot);
        auto* condBB  = llvm::BasicBlock::Create(llvm_, "iof.c", currentFn_);
        auto* bodyBB  = llvm::BasicBlock::Create(llvm_, "iof.b", currentFn_);
        auto* foundBB = llvm::BasicBlock::Create(llvm_, "iof.f", currentFn_);
        auto* nextBB  = llvm::BasicBlock::Create(llvm_, "iof.n", currentFn_);
        auto* endBB   = llvm::BasicBlock::Create(llvm_, "iof.e", currentFn_);
        builder_.CreateBr(condBB);
        builder_.SetInsertPoint(condBB);
        llvm::Value* idx32 = builder_.CreateLoad(i32(), idxSlot, "iof.i");
        builder_.CreateCondBr(
            builder_.CreateICmpSLT(idx32, slen), bodyBB, endBB);
        builder_.SetInsertPoint(bodyBB);
        llvm::Value* absIdx = builder_.CreateAdd(
            builder_.CreateSExt(base, i64()),
            builder_.CreateSExt(idx32, i64()), "iof.ai");
        llvm::Value* ep = builder_.CreateCall(ptrAtFn,
            {src, absIdx, builder_.getInt64(esz)}, "iof.ep");
        llvm::Value* el = builder_.CreateLoad(ety, ep, "iof.el");
        // #602 / §28.11 v0.134: spanElemRef and spanElemIsClass from policy hoist.
        llvm::Value* eq = emitRemoveElementEquality(el, v,
            spanElemRef, spanElemIsClass);
        if (!eq) return nullptr;
        builder_.CreateCondBr(eq, foundBB, nextBB);
        builder_.SetInsertPoint(nextBB);
        builder_.CreateStore(
            builder_.CreateAdd(idx32, builder_.getInt32(1)), idxSlot);
        builder_.CreateBr(condBB);
        builder_.SetInsertPoint(foundBB);
        builder_.CreateBr(endBB);
        builder_.SetInsertPoint(endBB);
        auto* phi = builder_.CreatePHI(i32(), 2, "iof.r");
        phi->addIncoming(idx32, foundBB);  // found: return current local index
        phi->addIncoming(builder_.getInt32(-1), condBB);  // not found: -1
        return phi;
    }

    // -- Contains(value: T) → bool ---------------------------------------------
    if (m == "Contains" && call.args.size() == 1 && call.args[0].value) {
        // Reuse IndexOf: Contains = IndexOf >= 0
        // Build a synthetic IndexOf call result by inlining the same logic.
        // (Sharing code with the IndexOf path above would require factoring; for
        // clarity we simply check IndexOf's result here via an inline call.)
        // Re-enter tryLowerArraySpanCall with a temporary CallExpr? — too complex.
        // Instead, just inline: emit IndexOf and compare.
        // (This means two code-size copies, but both are inlined; acceptable.)
        llvm::Value* v = lowerExpr(*call.args[0].value);
        if (!v) return nullptr;
        int64_t esz = spanElemSize();
        llvm::Type* ety = spanElemTy();
        llvm::Value* src  = loadSource();
        llvm::Value* base = loadStart();
        llvm::Value* slen = loadLen();
        auto* ptrAtFn = declareC("bz_array_ptr_at", ptrTy(), {ptrTy(), i64(), i64()});
        auto* idxSlot = entryAlloca(i32());
        builder_.CreateStore(builder_.getInt32(0), idxSlot);
        auto* condBB  = llvm::BasicBlock::Create(llvm_, "cnt.c", currentFn_);
        auto* bodyBB  = llvm::BasicBlock::Create(llvm_, "cnt.b", currentFn_);
        auto* foundBB = llvm::BasicBlock::Create(llvm_, "cnt.f", currentFn_);
        auto* nextBB  = llvm::BasicBlock::Create(llvm_, "cnt.n", currentFn_);
        auto* endBB   = llvm::BasicBlock::Create(llvm_, "cnt.e", currentFn_);
        builder_.CreateBr(condBB);
        builder_.SetInsertPoint(condBB);
        llvm::Value* idx32 = builder_.CreateLoad(i32(), idxSlot, "cnt.i");
        builder_.CreateCondBr(
            builder_.CreateICmpSLT(idx32, slen), bodyBB, endBB);
        builder_.SetInsertPoint(bodyBB);
        llvm::Value* absIdx = builder_.CreateAdd(
            builder_.CreateSExt(base, i64()),
            builder_.CreateSExt(idx32, i64()));
        llvm::Value* ep = builder_.CreateCall(ptrAtFn,
            {src, absIdx, builder_.getInt64(esz)});
        llvm::Value* el = builder_.CreateLoad(ety, ep);
        // #602 / §28.11 v0.134: spanElemRef and spanElemIsClass from policy hoist.
        llvm::Value* eq = emitRemoveElementEquality(el, v,
            spanElemRef, spanElemIsClass);
        if (!eq) return nullptr;
        builder_.CreateCondBr(eq, foundBB, nextBB);
        builder_.SetInsertPoint(nextBB);
        builder_.CreateStore(
            builder_.CreateAdd(idx32, builder_.getInt32(1)), idxSlot);
        builder_.CreateBr(condBB);
        builder_.SetInsertPoint(foundBB);
        builder_.CreateBr(endBB);
        builder_.SetInsertPoint(endBB);
        auto* phi = builder_.CreatePHI(i1(), 2, "cnt.r");
        phi->addIncoming(builder_.getTrue(),  foundBB);
        phi->addIncoming(builder_.getFalse(), condBB);
        return phi;
    }

    // -- Fill(value: T) — ArraySpan<T> only ------------------------------------
    if (m == "Fill" && isWritable &&
        call.args.size() == 1 && call.args[0].value) {
        llvm::Value* v = lowerExpr(*call.args[0].value);
        if (!v) return nullptr;
        int64_t esz = spanElemSize();
        llvm::Value* src  = loadSource();
        llvm::Value* base = loadStart();
        llvm::Value* slen = loadLen();
        auto* ptrAtFn = declareC("bz_array_ptr_at", ptrTy(), {ptrTy(), i64(), i64()});
        auto* idxSlot = entryAlloca(i32());
        builder_.CreateStore(builder_.getInt32(0), idxSlot);
        auto* condBB = llvm::BasicBlock::Create(llvm_, "fill.c", currentFn_);
        auto* bodyBB = llvm::BasicBlock::Create(llvm_, "fill.b", currentFn_);
        auto* endBB  = llvm::BasicBlock::Create(llvm_, "fill.e", currentFn_);
        builder_.CreateBr(condBB);
        builder_.SetInsertPoint(condBB);
        llvm::Value* idx32 = builder_.CreateLoad(i32(), idxSlot, "fill.i");
        builder_.CreateCondBr(
            builder_.CreateICmpSLT(idx32, slen), bodyBB, endBB);
        builder_.SetInsertPoint(bodyBB);
        llvm::Value* absIdx = builder_.CreateAdd(
            builder_.CreateSExt(base, i64()),
            builder_.CreateSExt(idx32, i64()));
        llvm::Value* ep = builder_.CreateCall(ptrAtFn,
            {src, absIdx, builder_.getInt64(esz)});
        builder_.CreateStore(v, ep);  // opaque ptr; store writes ety bytes
        builder_.CreateStore(
            builder_.CreateAdd(idx32, builder_.getInt32(1)), idxSlot);
        builder_.CreateBr(condBB);
        builder_.SetInsertPoint(endBB);
        return llvm::ConstantInt::get(i32(), 0);  // unit
    }

    // -- CopyTo(destination: ArraySpan<T>) — both span types ------------------
    // -- CopyFrom(source: ReadOnlyArraySpan<T>) — ArraySpan<T> only ----------
    if ((m == "CopyTo" || (m == "CopyFrom" && isWritable)) &&
        call.args.size() == 1 && call.args[0].value) {
        llvm::Value* otherVal = lowerExpr(*call.args[0].value);
        if (!otherVal) return nullptr;
        // Spill the other span to an alloca.
        auto* otherSlot = entryAlloca(spanSt);
        builder_.CreateStore(otherVal, otherSlot);
        llvm::Value* dstSrc  = nullptr, *dstBase = nullptr, *dstLen = nullptr;
        llvm::Value* srcSrc  = nullptr, *srcBase = nullptr, *srcLen = nullptr;
        if (m == "CopyTo") {
            // copy THIS → dst (other)
            srcSrc  = loadSource(); srcBase = loadStart(); srcLen = loadLen();
            dstSrc  = builder_.CreateLoad(ptrTy(),
                builder_.CreateStructGEP(spanSt, otherSlot, 0));
            dstBase = builder_.CreateLoad(i32(),
                builder_.CreateStructGEP(spanSt, otherSlot, 1));
            dstLen  = builder_.CreateLoad(i32(),
                builder_.CreateStructGEP(spanSt, otherSlot, 2));
        } else {
            // CopyFrom: copy other (src) → THIS (dst)
            dstSrc  = loadSource(); dstBase = loadStart(); dstLen = loadLen();
            srcSrc  = builder_.CreateLoad(ptrTy(),
                builder_.CreateStructGEP(spanSt, otherSlot, 0));
            srcBase = builder_.CreateLoad(i32(),
                builder_.CreateStructGEP(spanSt, otherSlot, 1));
            srcLen  = builder_.CreateLoad(i32(),
                builder_.CreateStructGEP(spanSt, otherSlot, 2));
        }
        // lengths must match (§4.15 "lengths must match, else ArgumentException")
        emitGuardThrow(builder_.CreateICmpNE(srcLen, dstLen),
                       "ArgumentException");
        int64_t esz = spanElemSize();
        llvm::Type* ety = spanElemTy();
        auto* ptrAtFn = declareC("bz_array_ptr_at", ptrTy(), {ptrTy(), i64(), i64()});
        auto* idxSlot = entryAlloca(i32());
        builder_.CreateStore(builder_.getInt32(0), idxSlot);
        auto* condBB = llvm::BasicBlock::Create(llvm_, "cpy.c", currentFn_);
        auto* bodyBB = llvm::BasicBlock::Create(llvm_, "cpy.b", currentFn_);
        auto* endBB  = llvm::BasicBlock::Create(llvm_, "cpy.e", currentFn_);
        builder_.CreateBr(condBB);
        builder_.SetInsertPoint(condBB);
        llvm::Value* idx32 = builder_.CreateLoad(i32(), idxSlot, "cpy.i");
        builder_.CreateCondBr(
            builder_.CreateICmpSLT(idx32, srcLen), bodyBB, endBB);
        builder_.SetInsertPoint(bodyBB);
        llvm::Value* si = builder_.CreateAdd(
            builder_.CreateSExt(srcBase, i64()),
            builder_.CreateSExt(idx32, i64()));
        llvm::Value* di = builder_.CreateAdd(
            builder_.CreateSExt(dstBase, i64()),
            builder_.CreateSExt(idx32, i64()));
        llvm::Value* sp = builder_.CreateCall(ptrAtFn,
            {srcSrc, si, builder_.getInt64(esz)});
        llvm::Value* dp = builder_.CreateCall(ptrAtFn,
            {dstSrc, di, builder_.getInt64(esz)});
        llvm::Value* val = builder_.CreateLoad(ety, sp);
        builder_.CreateStore(val, dp);  // opaque ptr
        builder_.CreateStore(
            builder_.CreateAdd(idx32, builder_.getInt32(1)), idxSlot);
        builder_.CreateBr(condBB);
        builder_.SetInsertPoint(endBB);
        return llvm::ConstantInt::get(i32(), 0);  // unit
    }

    // -- Reverse() — ArraySpan<T> only ----------------------------------------
    if (m == "Reverse" && isWritable && call.args.empty()) {
        int64_t esz = spanElemSize();
        llvm::Type* ety = spanElemTy();
        llvm::Value* src  = loadSource();
        llvm::Value* base = loadStart();
        llvm::Value* slen = loadLen();
        auto* ptrAtFn = declareC("bz_array_ptr_at", ptrTy(), {ptrTy(), i64(), i64()});
        auto* idxSlot = entryAlloca(i32());
        builder_.CreateStore(builder_.getInt32(0), idxSlot);
        auto* condBB = llvm::BasicBlock::Create(llvm_, "sprev.c", currentFn_);
        auto* bodyBB = llvm::BasicBlock::Create(llvm_, "sprev.b", currentFn_);
        auto* endBB  = llvm::BasicBlock::Create(llvm_, "sprev.e", currentFn_);
        builder_.CreateBr(condBB);
        builder_.SetInsertPoint(condBB);
        llvm::Value* idx32 = builder_.CreateLoad(i32(), idxSlot, "rv.i");
        llvm::Value* half  = builder_.CreateSDiv(slen, builder_.getInt32(2));
        builder_.CreateCondBr(
            builder_.CreateICmpSLT(idx32, half), bodyBB, endBB);
        builder_.SetInsertPoint(bodyBB);
        llvm::Value* j = builder_.CreateSub(
            builder_.CreateSub(slen, idx32), builder_.getInt32(1));
        llvm::Value* ai = builder_.CreateAdd(
            builder_.CreateSExt(base, i64()),
            builder_.CreateSExt(idx32, i64()));
        llvm::Value* aj = builder_.CreateAdd(
            builder_.CreateSExt(base, i64()),
            builder_.CreateSExt(j, i64()));
        llvm::Value* pa = builder_.CreateCall(ptrAtFn,
            {src, ai, builder_.getInt64(esz)});
        llvm::Value* pb = builder_.CreateCall(ptrAtFn,
            {src, aj, builder_.getInt64(esz)});
        llvm::Value* ta = builder_.CreateLoad(ety, pa);
        llvm::Value* tb = builder_.CreateLoad(ety, pb);
        builder_.CreateStore(tb, pa);  // opaque ptrs
        builder_.CreateStore(ta, pb);
        builder_.CreateStore(
            builder_.CreateAdd(idx32, builder_.getInt32(1)), idxSlot);
        builder_.CreateBr(condBB);
        builder_.SetInsertPoint(endBB);
        return llvm::ConstantInt::get(i32(), 0);  // unit
    }

    // -- SequenceEqual(other: ArraySpan<T> | ReadOnlyArraySpan<T>) → bool -----
    // #3849 DECISION: KEEP this arm as a single implementation serving both
    // the concrete cross-mode call (ArraySpan<T> receiver, ReadOnlyArraySpan<T>
    // argument, or vice versa) AND an argument already typed IReadOnlySpan<T>,
    // rather than adding a second, interface-routed emitter.
    //
    // v0.290 (proposal 0123 R3) retired the concrete cross-mode overloads
    // (`ArraySpan<T>.SequenceEqual(other: ReadOnlyArraySpan<T>)` and its
    // `ReadOnlyArraySpan<T>` mirror); stdlib §4.15 now states: "There is no
    // concrete cross-mode overload, and none is needed: the interface form
    // `SequenceEqual(other: IReadOnlySpan<T>)` admits a `ReadOnlyArraySpan<T>`
    // (and any other span family at element type `T`) directly, under the
    // call-bounded boxing of language spec §8.6". Stdlib §4.16 ("SequenceEqual
    // and the interface") spells out the naive translation of that rule: box
    // the argument into a heap `IReadOnlySpan<T>` object and read it back
    // through "one virtual-dispatched indexer read per element (the argument
    // side; the receiver's reads stay direct)".
    //
    // For a CONCRETE argument (ArraySpan<T>/ReadOnlyArraySpan<T>/Span<T>/
    // ReadOnlySpan<T> — the cross-mode case the issue was filed against) this
    // arm skips the box and the virtual dispatch entirely: `lowerExpr` already
    // produces the raw `{ptr source, i32 start, i32 length}` value every span
    // type shares (see the block comment at this function's top), so reading
    // it directly is a sound devirtualization stdlib §1 permits
    // ("Implementations are free to optimize") — provably the same bytes the
    // interface path's box would eventually hand back. This predates the
    // v0.290 retirement (the arm was never argument-kind-specific for a
    // CONCRETE argument, going back to the pre-split LlvmBackend_Calls.cpp),
    // so nothing needed to change here for that case.
    //
    // BUT an argument whose STATIC type is *already* `IReadOnlySpan<T>` (an
    // identity match against this method's own `(other: IReadOnlySpan<T>)`
    // overload — no further boxing conversion happens, it is already boxed)
    // is NOT the same value shape: `lowerExpr` returns the boxed heap pointer
    // `{vtable*, i64 rc, bz.arrayspan}` (see `ensureSpanBoxVtable`'s `loadSpan`
    // lambda, LlvmBackend_Setup.cpp — box+16 is the arrayspan slot), not the
    // bare struct. An earlier version of this comment claimed the direct read
    // was sound unconditionally; it was not measured against this case, and it
    // was wrong: storing that lone pointer into a `spanSt`-typed alloca fills
    // only the first field, leaving `start`/`length` as UNINITIALIZED stack
    // bytes — the fast-path length compare below then reads garbage, so the
    // call could return the wrong boolean (valgrind: "Conditional jump ...
    // depends on uninitialised value(s) ... created by a stack allocation").
    // Detect that case with `isIReadOnlySpanType` (the same predicate the
    // rest of the codegen uses to recognize a boxed span — no new "is this a
    // span" spelling) and unbox through the SAME box+16 offset `loadSpan`
    // uses, reusing its GEP shape rather than re-deriving the layout.
    // Regression tests (array_span_549_test.cpp): the `sequence_equal_*_
    // cross_mode_*_3849` rows pin the concrete cross-mode case; `sequence_
    // equal_boxed_ireadonlyspan_arg_3849` pins THIS case with an asserted
    // VALUE (was 0, must be 1) and was the row that failed without the unbox.
    // ⚠ A DIFFERENT, pre-existing bug shares this call shape: the argument's
    // box leaks (32 bytes) whether or not the unbox above is present — filed
    // separately as issue 3874 (not fixed here; the two rows above assert
    // VALUE only, not ASan-cleanliness, for exactly that reason).
    if (m == "SequenceEqual" && call.args.size() == 1 && call.args[0].value) {
        llvm::Value* otherVal = lowerExpr(*call.args[0].value);
        if (!otherVal) return nullptr;
        auto* otherSlot = entryAlloca(spanSt);
        // #3849: an already-boxed `IReadOnlySpan<T>` argument is a heap
        // pointer, not the raw arrayspan struct — unbox it first (see the
        // block comment above).
        if (isIReadOnlySpanType(ctx_.exprType(*call.args[0].value))) {
            llvm::Value* boxSpanPtr = builder_.CreateGEP(
                ptrTy(), otherVal, {builder_.getInt32(2)}, "se.box.span");
            otherVal = builder_.CreateLoad(spanSt, boxSpanPtr, "se.unboxed");
        }
        builder_.CreateStore(otherVal, otherSlot);
        llvm::Value* aLen = loadLen();
        llvm::Value* bLen = builder_.CreateLoad(i32(),
            builder_.CreateStructGEP(spanSt, otherSlot, 2), "se.blen");
        // Fast-path: different lengths → false.
        auto* fastFalse = llvm::BasicBlock::Create(llvm_, "se.ff", currentFn_);
        auto* loopEntry = llvm::BasicBlock::Create(llvm_, "se.le", currentFn_);
        auto* condBB    = llvm::BasicBlock::Create(llvm_, "se.c",  currentFn_);
        auto* bodyBB    = llvm::BasicBlock::Create(llvm_, "se.b",  currentFn_);
        auto* neqBB     = llvm::BasicBlock::Create(llvm_, "se.ne", currentFn_);
        auto* incBB     = llvm::BasicBlock::Create(llvm_, "se.i",  currentFn_);
        auto* endBB     = llvm::BasicBlock::Create(llvm_, "se.e",  currentFn_);
        builder_.CreateCondBr(
            builder_.CreateICmpNE(aLen, bLen), fastFalse, loopEntry);
        builder_.SetInsertPoint(fastFalse);
        builder_.CreateBr(endBB);
        builder_.SetInsertPoint(loopEntry);
        auto* idxSlot = entryAlloca(i32());
        builder_.CreateStore(builder_.getInt32(0), idxSlot);
        // Compute src/start pointers here (in loopEntry) BEFORE the branch to
        // condBB so that no instructions are inserted after a terminator.
        int64_t esz = spanElemSize();
        llvm::Type* ety = spanElemTy();
        llvm::Value* aSrc  = loadSource();
        llvm::Value* aBase = loadStart();
        llvm::Value* bSrc  = builder_.CreateLoad(ptrTy(),
            builder_.CreateStructGEP(spanSt, otherSlot, 0));
        llvm::Value* bBase = builder_.CreateLoad(i32(),
            builder_.CreateStructGEP(spanSt, otherSlot, 1));
        auto* ptrAtFn = declareC("bz_array_ptr_at", ptrTy(), {ptrTy(), i64(), i64()});
        builder_.CreateBr(condBB);
        builder_.SetInsertPoint(condBB);
        llvm::Value* idx32 = builder_.CreateLoad(i32(), idxSlot, "se.idx");
        builder_.CreateCondBr(
            builder_.CreateICmpSLT(idx32, aLen), bodyBB, endBB);
        builder_.SetInsertPoint(bodyBB);
        llvm::Value* ai = builder_.CreateAdd(
            builder_.CreateSExt(aBase, i64()),
            builder_.CreateSExt(idx32, i64()));
        llvm::Value* bi = builder_.CreateAdd(
            builder_.CreateSExt(bBase, i64()),
            builder_.CreateSExt(idx32, i64()));
        llvm::Value* ap = builder_.CreateCall(ptrAtFn,
            {aSrc, ai, builder_.getInt64(esz)});
        llvm::Value* bp = builder_.CreateCall(ptrAtFn,
            {bSrc, bi, builder_.getInt64(esz)});
        llvm::Value* aEl = builder_.CreateLoad(ety, ap);
        llvm::Value* bEl = builder_.CreateLoad(ety, bp);
        // #602 / §28.11 v0.134: spanElemRef and spanElemIsClass from policy hoist.
        llvm::Value* eq = emitRemoveElementEquality(aEl, bEl,
            spanElemRef, spanElemIsClass);
        if (!eq) return nullptr;
        builder_.CreateCondBr(eq, incBB, neqBB);
        builder_.SetInsertPoint(neqBB);
        builder_.CreateBr(endBB);
        builder_.SetInsertPoint(incBB);
        builder_.CreateStore(
            builder_.CreateAdd(idx32, builder_.getInt32(1)), idxSlot);
        builder_.CreateBr(condBB);
        builder_.SetInsertPoint(endBB);
        auto* phi = builder_.CreatePHI(i1(), 3, "se.r");
        phi->addIncoming(builder_.getFalse(), fastFalse);
        phi->addIncoming(builder_.getFalse(), neqBB);
        phi->addIncoming(builder_.getTrue(),  condBB);
        return phi;
    }

    // -- Clear() — owning Span<classT> only (§4.14) ---------------------------
    // Dispose every owned element and leave each slot NULL (moved-from), so the
    // underlying OwningBuffer's scope-end disposal skips them (no double-free).
    if (m == "Clear" && isOwningSpan && call.args.empty()) {
        llvm::Function* disp =
            elementPolicyFor(spanElemRef, /*owningFamily=*/true).disposerThunk;
        int64_t esz = spanElemSize();
        llvm::Type* ety = spanElemTy();
        llvm::Value* src  = loadSource();
        llvm::Value* base = loadStart();
        llvm::Value* slen = loadLen();
        auto* setDispFn = declareC("bz_array_set_disposing",
            llvm::Type::getVoidTy(llvm_), {ptrTy(), i64(), ptrTy(), i64(), ptrTy()});
        auto* setFn = declareC("bz_array_set",
            llvm::Type::getVoidTy(llvm_), {ptrTy(), i64(), ptrTy(), i64()});
        auto* nullSlot = entryAlloca(ety);
        builder_.CreateStore(llvm::Constant::getNullValue(ety), nullSlot);
        auto* idxSlot = entryAlloca(i32());
        builder_.CreateStore(builder_.getInt32(0), idxSlot);
        auto* condBB = llvm::BasicBlock::Create(llvm_, "spclr.c", currentFn_);
        auto* bodyBB = llvm::BasicBlock::Create(llvm_, "spclr.b", currentFn_);
        auto* endBB  = llvm::BasicBlock::Create(llvm_, "spclr.e", currentFn_);
        builder_.CreateBr(condBB);
        builder_.SetInsertPoint(condBB);
        llvm::Value* idx32 = builder_.CreateLoad(i32(), idxSlot, "clr.i");
        builder_.CreateCondBr(
            builder_.CreateICmpSLT(idx32, slen), bodyBB, endBB);
        builder_.SetInsertPoint(bodyBB);
        llvm::Value* ai = builder_.CreateAdd(
            builder_.CreateSExt(base, i64()),
            builder_.CreateSExt(idx32, i64()));
        // dispose-then-null (bz_array_set_disposing reads the old slot, disposes
        // it if non-null, then stores the new value — here NULL).
        if (disp)
            builder_.CreateCall(setDispFn,
                {src, ai, nullSlot, builder_.getInt64(esz), disp});
        else
            builder_.CreateCall(setFn, {src, ai, nullSlot, builder_.getInt64(esz)});
        builder_.CreateStore(
            builder_.CreateAdd(idx32, builder_.getInt32(1)), idxSlot);
        builder_.CreateBr(condBB);
        builder_.SetInsertPoint(endBB);
        return llvm::ConstantInt::get(i32(), 0);  // unit
    }

    // -- MoveFrom(source: Span<T>) / MoveTo(destination: Span<T>) -------------
    // owning Span<classT> only (§4.14): move ownership slot-by-slot. The dest's
    // previously-owned element is disposed before the moved owner is stored; the
    // source slot is left NULL (moved-from). Lengths must match.
    if ((m == "MoveFrom" || m == "MoveTo") && isOwningSpan &&
        call.args.size() == 1 && call.args[0].value) {
        llvm::Value* otherVal = lowerExpr(*call.args[0].value);
        if (!otherVal) return nullptr;
        auto* otherSlot = entryAlloca(spanSt);
        builder_.CreateStore(otherVal, otherSlot);
        llvm::Value *srcSrc, *srcBase, *srcLen, *dstSrc, *dstBase, *dstLen;
        if (m == "MoveTo") {                 // this(src) → other(dst)
            srcSrc = loadSource(); srcBase = loadStart(); srcLen = loadLen();
            dstSrc  = builder_.CreateLoad(ptrTy(),
                builder_.CreateStructGEP(spanSt, otherSlot, 0));
            dstBase = builder_.CreateLoad(i32(),
                builder_.CreateStructGEP(spanSt, otherSlot, 1));
            dstLen  = builder_.CreateLoad(i32(),
                builder_.CreateStructGEP(spanSt, otherSlot, 2));
        } else {                             // MoveFrom: other(src) → this(dst)
            dstSrc = loadSource(); dstBase = loadStart(); dstLen = loadLen();
            srcSrc  = builder_.CreateLoad(ptrTy(),
                builder_.CreateStructGEP(spanSt, otherSlot, 0));
            srcBase = builder_.CreateLoad(i32(),
                builder_.CreateStructGEP(spanSt, otherSlot, 1));
            srcLen  = builder_.CreateLoad(i32(),
                builder_.CreateStructGEP(spanSt, otherSlot, 2));
        }
        emitGuardThrow(builder_.CreateICmpNE(srcLen, dstLen), "ArgumentException");
        llvm::Function* disp =
            elementPolicyFor(spanElemRef, /*owningFamily=*/true).disposerThunk;
        int64_t esz = spanElemSize();
        llvm::Type* ety = spanElemTy();
        auto* ptrAtFn = declareC("bz_array_ptr_at", ptrTy(), {ptrTy(), i64(), i64()});
        auto* setDispFn = declareC("bz_array_set_disposing",
            llvm::Type::getVoidTy(llvm_), {ptrTy(), i64(), ptrTy(), i64(), ptrTy()});
        auto* setFn = declareC("bz_array_set",
            llvm::Type::getVoidTy(llvm_), {ptrTy(), i64(), ptrTy(), i64()});
        auto* nullSlot = entryAlloca(ety);
        builder_.CreateStore(llvm::Constant::getNullValue(ety), nullSlot);
        auto* valSlot = entryAlloca(ety);
        auto* idxSlot = entryAlloca(i32());
        builder_.CreateStore(builder_.getInt32(0), idxSlot);
        auto* condBB = llvm::BasicBlock::Create(llvm_, "spmv.c", currentFn_);
        auto* bodyBB = llvm::BasicBlock::Create(llvm_, "spmv.b", currentFn_);
        auto* endBB  = llvm::BasicBlock::Create(llvm_, "spmv.e", currentFn_);
        builder_.CreateBr(condBB);
        builder_.SetInsertPoint(condBB);
        llvm::Value* idx32 = builder_.CreateLoad(i32(), idxSlot, "mv.i");
        builder_.CreateCondBr(
            builder_.CreateICmpSLT(idx32, srcLen), bodyBB, endBB);
        builder_.SetInsertPoint(bodyBB);
        llvm::Value* si = builder_.CreateAdd(
            builder_.CreateSExt(srcBase, i64()),
            builder_.CreateSExt(idx32, i64()));
        llvm::Value* di = builder_.CreateAdd(
            builder_.CreateSExt(dstBase, i64()),
            builder_.CreateSExt(idx32, i64()));
        // read the moved element out of the source slot
        llvm::Value* sp = builder_.CreateCall(ptrAtFn,
            {srcSrc, si, builder_.getInt64(esz)});
        llvm::Value* v = builder_.CreateLoad(ety, sp, "mv.v");
        builder_.CreateStore(v, valSlot);
        // dispose displaced dest, then store the moved owner
        if (disp)
            builder_.CreateCall(setDispFn,
                {dstSrc, di, valSlot, builder_.getInt64(esz), disp});
        else
            builder_.CreateCall(setFn, {dstSrc, di, valSlot, builder_.getInt64(esz)});
        // leave the source slot moved-from (NULL)
        builder_.CreateCall(setFn, {srcSrc, si, nullSlot, builder_.getInt64(esz)});
        builder_.CreateStore(
            builder_.CreateAdd(idx32, builder_.getInt32(1)), idxSlot);
        builder_.CreateBr(condBB);
        builder_.SetInsertPoint(endBB);
        return llvm::ConstantInt::get(i32(), 0);  // unit
    }

    // §4.15 (#1358): byte-buffer accessors — Read*/TryRead*/Write*/TryWrite*
    // for all four span types (ArraySpan/ReadOnlyArraySpan/Span/ReadOnlySpan)
    // when the element type is `byte`.  Methods operate from the start of the
    // span (no offset parameter — callers use .Slice() to position; stdlib §4.15).
    //
    // Names: {Try}{Read|Write}{Int16|UInt16|Int32|UInt32|Int64|UInt64|
    //                          Float|Double}{LittleEndian|BigEndian}
    if (spanElemRef &&
        spanElemRef->kind == sema::TypeRef::Kind::Primitive &&
        spanElemRef->primitive == sema::PrimitiveType::Byte) {
        std::string_view mv(m);
        bool isTry  = mv.starts_with("Try");
        std::string_view op = isTry ? mv.substr(3) : mv;
        bool isRead  = op.starts_with("Read");
        bool isWrite = op.starts_with("Write");
        if ((isRead || isWrite) &&
            (op.ends_with("LittleEndian") || op.ends_with("BigEndian"))) {
            bool isBE    = op.ends_with("BigEndian");
            std::string_view inner = isWrite ? op.substr(5) : op.substr(4);
            // Map method name → LLVM integer type + byte width.
            int byteWidth = 0;
            llvm::Type* intTy   = nullptr;
            bool        isFloat = false;
            if      (inner.starts_with("Int16")  || inner.starts_with("UInt16"))
                { byteWidth = 2; intTy = llvm::Type::getInt16Ty(llvm_); }
            else if (inner.starts_with("Int32")  || inner.starts_with("UInt32"))
                { byteWidth = 4; intTy = i32(); }
            else if (inner.starts_with("Float"))
                { byteWidth = 4; intTy = i32();  isFloat = true; }
            else if (inner.starts_with("Int64")  || inner.starts_with("UInt64"))
                { byteWidth = 8; intTy = i64(); }
            else if (inner.starts_with("Double"))
                { byteWidth = 8; intTy = i64();  isFloat = true; }
            if (byteWidth > 0) {
                // isWritable was already set from rcn above.
                // Write*/TryWrite* require a writable span (sema enforces this;
                // we bail silently here if somehow wrong at codegen).
                if (isWrite && !isWritable) return nullptr;

                llvm::Value* src  = loadSource();
                llvm::Value* base = loadStart();
                llvm::Value* slen = loadLen();
                // Raw byte pointer: bz_array_ptr_at(src, i64(base), 1).
                auto* ptrAtFn = declareC(
                    "bz_array_ptr_at", ptrTy(), {ptrTy(), i64(), i64()});
                llvm::Value* base64 = builder_.CreateSExt(base, i64());
                llvm::Value* rawPtr = builder_.CreateCall(
                    ptrAtFn, {src, base64, builder_.getInt64(1)}, "bba.ptr");
                llvm::Value* slen64 = builder_.CreateSExt(slen, i64());
                llvm::Value* wid64  = builder_.getInt64(byteWidth);

                if (!isTry) {
                    // Non-try variants: throw IndexOutOfRangeException if too short.
                    emitGuardThrow(
                        builder_.CreateICmpSLT(slen64, wid64),
                        "IndexOutOfRangeException");
                    if (isRead) {
                        // Load integer value, bswap if BE, bitcast to float/double.
                        llvm::Value* raw = builder_.CreateLoad(intTy, rawPtr, "bba.raw");
                        if (isBE) {
                            auto* bswapFn = llvm::Intrinsic::getDeclaration(
                                module_.get(), llvm::Intrinsic::bswap, {intTy});
                            raw = builder_.CreateCall(bswapFn, {raw}, "bba.bswap");
                        }
                        if (isFloat) {
                            llvm::Type* fpTy = byteWidth == 4
                                ? llvm::Type::getFloatTy(llvm_)
                                : llvm::Type::getDoubleTy(llvm_);
                            return builder_.CreateBitCast(raw, fpTy, "bba.fp");
                        }
                        return raw;
                    } else {
                        // Write: for float/double, bitcast the argument to int first.
                        if (call.args.empty() || !call.args[0].value) return nullptr;
                        llvm::Value* val = lowerExpr(*call.args[0].value);
                        if (!val) return nullptr;
                        if (isFloat) {
                            val = builder_.CreateBitCast(val, intTy, "bba.fi");
                        } else {
                            // Ensure correct bit-width for integer types.
                            if (val->getType() != intTy) {
                                if (intTy->getIntegerBitWidth() <
                                    val->getType()->getIntegerBitWidth())
                                    val = builder_.CreateTrunc(val, intTy, "bba.trunc");
                                else
                                    val = builder_.CreateZExt(val, intTy, "bba.zext");
                            }
                        }
                        if (isBE) {
                            auto* bswapFn = llvm::Intrinsic::getDeclaration(
                                module_.get(), llvm::Intrinsic::bswap, {intTy});
                            val = builder_.CreateCall(bswapFn, {val}, "bba.bswap");
                        }
                        builder_.CreateStore(val, rawPtr);
                        return llvm::ConstantInt::get(i32(), 0);  // unit
                    }
                } else {
                    // Try variants: return false (no throw) if span too short.
                    auto* okBB   = llvm::BasicBlock::Create(llvm_, "bba.ok", currentFn_);
                    auto* failBB = llvm::BasicBlock::Create(llvm_, "bba.fail", currentFn_);
                    auto* endBB  = llvm::BasicBlock::Create(llvm_, "bba.end", currentFn_);
                    llvm::Value* hasRoom =
                        builder_.CreateICmpSGE(slen64, wid64, "bba.room");
                    builder_.CreateCondBr(hasRoom, okBB, failBB);
                    if (isRead) {
                        // TryRead*(out value: T): bool
                        // out parameter is call.args[0]
                        if (call.args.empty() || !call.args[0].value) {
                            builder_.SetInsertPoint(okBB);
                            builder_.CreateBr(endBB);
                            builder_.SetInsertPoint(failBB);
                            builder_.CreateBr(endBB);
                            builder_.SetInsertPoint(endBB);
                            return builder_.getFalse();
                        }
                        // fix #2938 / §22.3: see TryGetValue in Calls.cpp.
                        // The pre-fix branch was `if (outAddr)` at both store
                        // sites, so a null lvalueAddr silently dropped the
                        // write. With the bracket, the slot is always present
                        // — only the writeback gating changes — so both stores
                        // run unconditionally and the bracket flushes at the
                        // merge block.
                        llvm::Type* outTy = nullptr;
                        std::vector<AccessorRefArg> outArgs;
                        llvm::Value* outAddr = resolveOutAddrWithBracket(
                            *call.args[0].value, outTy, outArgs);
                        if (!outAddr) {
                            emitAccessorRefArgWriteBacks(outArgs,
                                                         /*emit=*/false);
                            return nullptr;
                        }
                        // okBB: load, bswap if BE, bitcast, store to out, jump end
                        builder_.SetInsertPoint(okBB);
                        llvm::Value* raw =
                            builder_.CreateLoad(intTy, rawPtr, "bba.tryraw");
                        if (isBE) {
                            auto* bswapFn = llvm::Intrinsic::getDeclaration(
                                module_.get(), llvm::Intrinsic::bswap, {intTy});
                            raw = builder_.CreateCall(bswapFn, {raw}, "bba.tbswap");
                        }
                        llvm::Value* result = raw;
                        if (isFloat) {
                            llvm::Type* fpTy = byteWidth == 4
                                ? llvm::Type::getFloatTy(llvm_)
                                : llvm::Type::getDoubleTy(llvm_);
                            result = builder_.CreateBitCast(raw, fpTy, "bba.tfp");
                        }
                        if (outAddr) builder_.CreateStore(result, outAddr);
                        builder_.CreateBr(endBB);
                        // failBB: store default (0/0.0) to out, jump end
                        builder_.SetInsertPoint(failBB);
                        if (outAddr) {
                            llvm::Type* storeTy = result->getType();
                            builder_.CreateStore(
                                llvm::Constant::getNullValue(storeTy), outAddr);
                        }
                        builder_.CreateBr(endBB);
                        builder_.SetInsertPoint(endBB);
                        // fix #2938 / §22.3: flush any bracket on the
                        // normal-return edge.
                        emitAccessorRefArgWriteBacks(outArgs, /*emit=*/true);
                        auto* phi = builder_.CreatePHI(i1(), 2, "bba.tryres");
                        phi->addIncoming(builder_.getTrue(),  okBB);
                        phi->addIncoming(builder_.getFalse(), failBB);
                        return phi;
                    } else {
                        // TryWrite*(value: T): bool
                        if (call.args.empty() || !call.args[0].value) {
                            builder_.SetInsertPoint(okBB);
                            builder_.CreateBr(endBB);
                            builder_.SetInsertPoint(failBB);
                            builder_.CreateBr(endBB);
                            builder_.SetInsertPoint(endBB);
                            return builder_.getFalse();
                        }
                        llvm::Value* val = lowerExpr(*call.args[0].value);
                        if (!val) {
                            builder_.SetInsertPoint(okBB);
                            builder_.CreateBr(endBB);
                            builder_.SetInsertPoint(failBB);
                            builder_.CreateBr(endBB);
                            builder_.SetInsertPoint(endBB);
                            return builder_.getFalse();
                        }
                        // okBB: bitcast/bswap, store, return true
                        builder_.SetInsertPoint(okBB);
                        llvm::Value* toStore = val;
                        if (isFloat) {
                            toStore = builder_.CreateBitCast(val, intTy, "bba.twfi");
                        } else if (toStore->getType() != intTy) {
                            if (intTy->getIntegerBitWidth() <
                                toStore->getType()->getIntegerBitWidth())
                                toStore = builder_.CreateTrunc(toStore, intTy);
                            else
                                toStore = builder_.CreateZExt(toStore, intTy);
                        }
                        if (isBE) {
                            auto* bswapFn = llvm::Intrinsic::getDeclaration(
                                module_.get(), llvm::Intrinsic::bswap, {intTy});
                            toStore = builder_.CreateCall(
                                bswapFn, {toStore}, "bba.twbswap");
                        }
                        builder_.CreateStore(toStore, rawPtr);
                        builder_.CreateBr(endBB);
                        // failBB: no-op, jump end
                        builder_.SetInsertPoint(failBB);
                        builder_.CreateBr(endBB);
                        builder_.SetInsertPoint(endBB);
                        auto* phi = builder_.CreatePHI(i1(), 2, "bba.twres");
                        phi->addIncoming(builder_.getTrue(),  okBB);
                        phi->addIncoming(builder_.getFalse(), failBB);
                        return phi;
                    }
                }
            }
        }
    }

    return nullptr;
}

// ---------------------------------------------------------------------------
// §30.14/§30.15 (#600 phase c): Channel<T> and Domain.Spawn codegen.
//
// Handles:
//   Channel<T>:
//     NewSender()               → bz_channel_new_sender(ch)        → ptr (ChannelSender<T>)
//     Send(owned item)          → bz_channel_send(ch, item)        → void
//     Receive()                 → bz_channel_receive(ch, cce_vtable) → Task/OwningTask<T>
//     TryReceive(out item)      → bz_channel_try_receive(ch, &slot)  → bool
//     Count                     → bz_channel_count(ch)             → int
//     IsClosed                  → bz_channel_is_closed(ch)         → bool
//     Dispose()                 → bz_channel_dispose(ch, cce_vtable)→ void
//   ChannelSender<T>:
//     Send(owned item)          → bz_channel_sender_send(tx, item) → void
//     Dispose()                 → bz_channel_sender_release(tx, cce_vtable) → void
//   Domain.Spawn(entry, arg?)   → bz_domain_spawn(fn, arg, dfe_vtable) → OwningTask<R>
// ---------------------------------------------------------------------------
llvm::Value* Emitter::tryLowerChannelAndDomainCall(const ast::CallExpr& call) {
    if (!call.callee) return nullptr;
    auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(call.callee.get());
    if (!ma || !ma->target) return nullptr;

    const std::string_view mname = ma->member;

    // Helper: get ChannelClosedException vtable constant (or null if not set up).
    auto getCCEVtable = [&]() -> llvm::Value* {
        auto vit = builtinVtable_.find("ChannelClosedException");
        if (vit == builtinVtable_.end())
            return llvm::ConstantPointerNull::get(ptrTy());
        return vit->second;
    };
    // v0.174 (#809): ChannelFullException vtable — raised by the sync Send on a
    // full bounded channel (via bz_exc, like the closed-channel path).
    auto getCFEVtable = [&]() -> llvm::Value* {
        auto vit = builtinVtable_.find("ChannelFullException");
        if (vit == builtinVtable_.end())
            return llvm::ConstantPointerNull::get(ptrTy());
        return vit->second;
    };
    auto getDFEVtable = [&]() -> llvm::Value* {
        auto vit = builtinVtable_.find("DomainFaultedException");
        if (vit == builtinVtable_.end())
            return llvm::ConstantPointerNull::get(ptrTy());
        return vit->second;
    };

    // -----------------------------------------------------------------------
    // Channel<T> instance methods (receiver type name == "Channel").
    // -----------------------------------------------------------------------
    {
        auto* recvTy = ctx_.exprType(*ma->target);
        bool isChannel = recvTy && recvTy->kind == sema::TypeRef::Kind::Named &&
                         builtinTypeName(recvTy) == "Channel";
        if (isChannel) {
            llvm::Value* ch = lowerExpr(*ma->target);
            if (!ch) return nullptr;

            // NewSender(): allocate a ChannelSender handle.
            if (mname == "NewSender" && call.args.empty()) {
                auto* fn = declareC("bz_channel_new_sender", ptrTy(), {ptrTy()});
                return builder_.CreateCall(fn, {ch}, "ch.newsender");
            }

            // Send(owned item): transfer ownership of item into the channel.
            // per fix #634 / §30.15: bz_channel_send now takes cce_vtable and
            // stores a ChannelClosedException in bz_exc when the channel is
            // closed; emitPendingCheck() unwinds on that path.
            if (mname == "Send" && call.args.size() == 1 && call.args[0].value) {
                llvm::Value* item = lowerExpr(*call.args[0].value);
                if (!item) return nullptr;
                // Value types (int/bool/etc.) need to be widened to ptr-size.
                // We store them as bit-cast void* (uint64 reinterpretation).
                // For ptr-typed items (class, string) just pass as-is.
                llvm::Value* itemArg = item;
                if (item->getType()->isIntegerTy() &&
                    !item->getType()->isIntegerTy(64)) {
                    // Zero-extend to i64 then inttoptr.
                    llvm::Value* ext = builder_.CreateZExt(item, i64(), "send.ext");
                    itemArg = builder_.CreateIntToPtr(ext, ptrTy(), "send.cast");
                } else if (item->getType()->isIntegerTy(64)) {
                    itemArg = builder_.CreateIntToPtr(item, ptrTy(), "send.cast");
                } else if (!item->getType()->isPointerTy()) {
                    // double/float: bitcast to i64 then inttoptr.
                    llvm::Value* bits = builder_.CreateBitCast(item, i64(), "send.bits");
                    itemArg = builder_.CreateIntToPtr(bits, ptrTy(), "send.cast");
                }
                auto* fn = declareC("bz_channel_send",
                                    llvm::Type::getVoidTy(llvm_),
                                    {ptrTy(), ptrTy(), ptrTy(), ptrTy()});
                builder_.CreateCall(fn, {ch, itemArg, getCCEVtable(),
                                         getCFEVtable()});
                emitPendingCheck();  // per fix #634 / v0.174 #809: bz_exc on closed or full
                return nullptr;  // void — handled; fallthrough is benign (no symbol match)
            }

            // Receive(): returns a Task<T> or OwningTask<T>.
            if (mname == "Receive" && call.args.empty()) {
                auto* fn = declareC("bz_channel_receive", ptrTy(),
                                    {ptrTy(), ptrTy()});
                return builder_.CreateCall(fn, {ch, getCCEVtable()}, "ch.recv");
            }

            // TryReceive(out owned item: T?) → bool.
            // The out-var slot was pre-created by ensureOutVarSlots.
            // fix #667 / §30.15 / stdlib §6.9: value-type payload decode.
            // The runtime stores value-typed items as inttoptr(zext(val,i64)).
            // The out-slot for T? is {i8 hasValue; T value} (nullable struct)
            // for primitive/enum T, or a pointer for class/string T.
            // We must decode rawItem correctly:
            //   - struct outEty (nullable value type): ptrtoint → trunc/bitcast
            //     to inner type, then build {i8=1, innerVal} and store.
            //   - pointer outEty (class/string): store rawItem directly.
            if (mname == "TryReceive" && call.args.size() == 1 &&
                call.args[0].value) {
                // The out argument's address. // fix #2938 / §22.3
                // Pre-fix the fallback was a temp slot — a null lvalueAddr
                // silently dropped the write. resolveOutAddrWithBracket falls
                // back to the §22.3 bracket, so the write lands in the
                // caller's property/indexer on normal return.
                llvm::Type* outEty = nullptr;
                std::vector<AccessorRefArg> outArgs;
                llvm::Value* outSlot = resolveOutAddrWithBracket(
                    *call.args[0].value, outEty, outArgs);
                if (!outSlot) {
                    // The previous fallback path was a scratch slot; with the
                    // bracket, no fallback is needed — the slot is always
                    // present once resolution succeeds.
                    emitAccessorRefArgWriteBacks(outArgs, /*emit=*/false);
                    return nullptr;
                }
                // Allocate a ptr-sized slot for the raw item pointer.
                llvm::AllocaInst* rawSlot = entryAlloca(ptrTy());
                builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()),
                                     rawSlot);
                auto* fn = declareC("bz_channel_try_receive", i32(),
                                    {ptrTy(), ptrTy()});
                llvm::Value* ok = builder_.CreateCall(fn, {ch, rawSlot},
                                                      "ch.tryrecv");
                // If ok: decode and copy rawItem into outSlot (the caller's out-var).
                auto* okBB   = llvm::BasicBlock::Create(llvm_, "tryrecv.ok",
                                                         currentFn_);
                auto* contBB = llvm::BasicBlock::Create(llvm_, "tryrecv.cont",
                                                         currentFn_);
                llvm::Value* isOk = builder_.CreateICmpNE(ok, builder_.getInt32(0));
                builder_.CreateCondBr(isOk, okBB, contBB);
                builder_.SetInsertPoint(okBB);
                llvm::Value* rawItem = builder_.CreateLoad(ptrTy(), rawSlot);
                // fix #667 / §30.15: decode the stored void* into the out-slot type.
                // For nullable value types (outEty is a struct) the item was encoded
                // as inttoptr(zext(T_value, i64)) by the Send lowering above; decode
                // by ptrtoint + trunc/bitcast to the inner field type, then wrap in
                // the nullable struct {i8=1, innerVal}.
                // For pointer types (class, string) the item IS the raw pointer; store
                // directly (the slot holds a nullable pointer: non-null = has value).
                if (outEty && outEty->isStructTy()) {
                    // Nullable value type path.
                    auto* stTy = llvm::cast<llvm::StructType>(outEty);
                    llvm::Type* innerTy = stTy->getNumElements() > 1
                                         ? stTy->getElementType(1)
                                         : i32();
                    llvm::Value* asI64 = builder_.CreatePtrToInt(
                        rawItem, i64(), "tr.i64");
                    llvm::Value* decoded;
                    if (innerTy->isDoubleTy()) {
                        decoded = builder_.CreateBitCast(asI64, innerTy, "tr.dbl");
                    } else if (innerTy->isFloatTy()) {
                        llvm::Value* t32 = builder_.CreateTrunc(asI64, i32(), "tr.f32b");
                        decoded = builder_.CreateBitCast(t32, innerTy, "tr.flt");
                    } else if (innerTy->isIntegerTy(64)) {
                        decoded = asI64;
                    } else if (innerTy->isIntegerTy()) {
                        decoded = builder_.CreateTrunc(asI64, innerTy, "tr.int");
                    } else {
                        // Fallback for unexpected inner types (e.g. record struct):
                        // store as i64 and let the struct initialiser zero the rest.
                        decoded = asI64;
                    }
                    // Build the nullable struct {i8=1, decoded}.
                    llvm::Value* nv = llvm::UndefValue::get(stTy);
                    nv = builder_.CreateInsertValue(
                        nv,
                        llvm::ConstantInt::get(nullablePrimHasValTy(), 1), {0u},
                        "tr.hv");
                    // Coerce decoded to the exact inner slot type (handles the rare
                    // case where the truncation produced a slightly wrong width).
                    if (decoded->getType() != stTy->getElementType(1))
                        decoded = builder_.CreateIntCast(
                            decoded, stTy->getElementType(1),
                            /*isSigned=*/false, "tr.coerce");
                    nv = builder_.CreateInsertValue(nv, decoded, {1u}, "tr.nv");
                    builder_.CreateStore(nv, outSlot);
                } else {
                    // Pointer type (class, string): rawItem is the object pointer.
                    builder_.CreateStore(rawItem, outSlot);
                }
                builder_.CreateBr(contBB);
                builder_.SetInsertPoint(contBB);
                // fix #2938 / §22.3: flush any bracket on the normal-return edge.
                emitAccessorRefArgWriteBacks(outArgs, /*emit=*/true);
                return builder_.CreateICmpNE(ok, builder_.getInt32(0), "ch.try.r");
            }

            // Count → int64.
            if (mname == "Count" && call.args.empty()) {
                auto* fn = declareC("bz_channel_count", i64(), {ptrTy()});
                llvm::Value* c = builder_.CreateCall(fn, {ch}, "ch.count");
                // Truncate to i32 (Bezel int).
                return builder_.CreateTrunc(c, i32(), "ch.count.i");
            }

            // IsClosed → bool.
            if (mname == "IsClosed" && call.args.empty()) {
                auto* fn = declareC("bz_channel_is_closed", i32(), {ptrTy()});
                llvm::Value* r = builder_.CreateCall(fn, {ch}, "ch.isclosed");
                return builder_.CreateICmpNE(r, builder_.getInt32(0));
            }

            // Dispose() — explicit dispose of the channel (frees backing struct).
            // Null the receiver slot after the call so any subsequent
            // using-var scope-exit reload sees null and skips (§28.2).
            if (mname == "Dispose" && call.args.empty()) {
                auto* fn = declareC("bz_channel_dispose",
                                    llvm::Type::getVoidTy(llvm_),
                                    {ptrTy(), ptrTy()});
                builder_.CreateCall(fn, {ch, getCCEVtable()});
                if (!checkedDisposal_) {
                    llvm::Type* slotTy = nullptr;
                    if (llvm::Value* slot = lvalueAddr(*ma->target, slotTy))
                        builder_.CreateStore(
                            llvm::ConstantPointerNull::get(ptrTy()), slot);
                }
                return nullptr;
            }

            // §30.15 / stdlib §6.9 v0.186 (#1724): ReceiveAll() — returns a
            // fresh IAsyncEnumerable<T>* / IOwningAsyncEnumerable<T>*
            // (vtable-backed enumerator). The drain enumerable mints a frame
            // per call (v0.258 §28.3 calls-mint) and is a separate object from
            // the channel — disposing it ends the enumeration without
            // affecting the channel. The owning variant (class T) uses
            // TakeCurrent at vtable slot 3; the borrowing variant (value/string T)
            // uses Current. Both share vtable slots 1 (Dispose) and 2
            // (MoveNextAsync). The IOE vtable is fetched at the call site
            // (single-consumer rule: a second ReceiveAll or a ReceiveAll begun
            // while a Receive task is parked raises IOE via bz_exc).
            if (mname == "ReceiveAll" && call.args.empty()) {
                llvm::Value* ioeVt = llvm::ConstantPointerNull::get(ptrTy());
                {   auto vit = builtinVtable_.find(
                        "InvalidOperationException");
                    if (vit != builtinVtable_.end()) ioeVt = vit->second;
                }
                // The owning flag is 1 for class-T channels, 0 for value-T
                // and string-T channels. Detect via the element type of the
                // receiver's genericArgs[0]: a Named-with-declaration type is a
                // class.
                int32_t is_owning = 0;
                if (recvTy && recvTy->kind == sema::TypeRef::Kind::Named &&
                    !recvTy->genericArgs.empty() && recvTy->genericArgs[0]) {
                    auto* elemTy = recvTy->genericArgs[0];
                    if (elemTy->kind == sema::TypeRef::Kind::Named &&
                        elemTy->declaration &&
                        elemTy->declaration->kind ==
                            ast::TypeDeclKind::Class)
                        is_owning = 1;
                }
                auto* fn = declareC("bz_channel_recv_all_init", ptrTy(),
                                    {ptrTy(), ptrTy(), i32()});
                llvm::Value* es = builder_.CreateCall(
                    fn, {ch, ioeVt, builder_.getInt32(is_owning)},
                    "ch.recvall");
                // Single-consumer violation surfaces as IOE via bz_exc on
                // the runtime side (see bz_channel_recv_all_init). Mirror the
                // TryReceive / Send arm and check bz_exc here so the exception
                // propagates into the caller's handler.
                emitPendingCheck();
                return es;
            }
        }
    }

    // -----------------------------------------------------------------------
    // ChannelSender<T> instance methods (receiver type name == "ChannelSender").
    // -----------------------------------------------------------------------
    {
        auto* recvTy = ctx_.exprType(*ma->target);
        bool isSender = recvTy && recvTy->kind == sema::TypeRef::Kind::Named &&
                        builtinTypeName(recvTy) == "ChannelSender";
        if (isSender) {
            llvm::Value* tx = lowerExpr(*ma->target);
            if (!tx) return nullptr;

            // Send(owned item): transfer ownership into the channel.
            // per fix #634 / §30.15: bz_channel_sender_send now takes cce_vtable
            // and signals ChannelClosedException via bz_exc when closed.
            // v0.174 (#809): cfe_vtable signals ChannelFullException via bz_exc on
            // a full bounded channel (the sync Send never blocks).
            if (mname == "Send" && call.args.size() == 1 && call.args[0].value) {
                llvm::Value* item = lowerExpr(*call.args[0].value);
                if (!item) return nullptr;
                llvm::Value* itemArg = item;
                if (item->getType()->isIntegerTy() &&
                    !item->getType()->isIntegerTy(64)) {
                    llvm::Value* ext = builder_.CreateZExt(item, i64());
                    itemArg = builder_.CreateIntToPtr(ext, ptrTy());
                } else if (item->getType()->isIntegerTy(64)) {
                    itemArg = builder_.CreateIntToPtr(item, ptrTy());
                } else if (!item->getType()->isPointerTy()) {
                    llvm::Value* bits = builder_.CreateBitCast(item, i64());
                    itemArg = builder_.CreateIntToPtr(bits, ptrTy());
                }
                auto* fn = declareC("bz_channel_sender_send",
                                    llvm::Type::getVoidTy(llvm_),
                                    {ptrTy(), ptrTy(), ptrTy(), ptrTy()});
                builder_.CreateCall(fn, {tx, itemArg, getCCEVtable(),
                                         getCFEVtable()});
                emitPendingCheck();  // fix #634 / v0.174 #809: bz_exc on closed or full
                return nullptr;
            }

            // v0.174 (#809) / §6.9: SendAsync(owned item): Task — awaits capacity.
            // Mirrors Send's item encoding but returns the awaitable Task the
            // runtime hands back (already-done when space is free / unbounded;
            // pending on a full bounded channel until a Receive frees a slot).
            // The close-while-waiting fault (ChannelClosedException) is delivered
            // THROUGH the task and surfaces at the caller's await site — so no
            // emitPendingCheck here (unlike the synchronous Send).
            if (mname == "SendAsync" && call.args.size() == 1 &&
                call.args[0].value) {
                llvm::Value* item = lowerExpr(*call.args[0].value);
                if (!item) return nullptr;
                llvm::Value* itemArg = item;
                if (item->getType()->isIntegerTy() &&
                    !item->getType()->isIntegerTy(64)) {
                    llvm::Value* ext = builder_.CreateZExt(item, i64());
                    itemArg = builder_.CreateIntToPtr(ext, ptrTy());
                } else if (item->getType()->isIntegerTy(64)) {
                    itemArg = builder_.CreateIntToPtr(item, ptrTy());
                } else if (!item->getType()->isPointerTy()) {
                    llvm::Value* bits = builder_.CreateBitCast(item, i64());
                    itemArg = builder_.CreateIntToPtr(bits, ptrTy());
                }
                auto* fn = declareC("bz_channel_sender_send_async", ptrTy(),
                                    {ptrTy(), ptrTy(), ptrTy()});
                return builder_.CreateCall(fn, {tx, itemArg, getCCEVtable()},
                                           "ch.sendasync");
            }

            // Dispose() — release this sender handle (may close the channel).
            // Null the receiver slot after the call so any subsequent
            // using-var scope-exit reload sees null and skips (§28.2).
            if (mname == "Dispose" && call.args.empty()) {
                auto* fn = declareC("bz_channel_sender_release",
                                    llvm::Type::getVoidTy(llvm_),
                                    {ptrTy(), ptrTy()});
                builder_.CreateCall(fn, {tx, getCCEVtable()});
                if (!checkedDisposal_ && ma) {
                    llvm::Type* slotTy = nullptr;
                    if (llvm::Value* slot = lvalueAddr(*ma->target, slotTy))
                        builder_.CreateStore(
                            llvm::ConstantPointerNull::get(ptrTy()), slot);
                }
                return nullptr;
            }
        }
    }

    // -----------------------------------------------------------------------
    // Domain.Spawn(entry, arg?) — static class "Domain".
    // The entry expression is a static method reference (a MemberAccessExpr
    // like `Worker.Compute` or `Producer.SendOne`). We:
    //   1. Resolve the MethodSymbol from the entry expression.
    //   2. Build a static adapter function: `void* adapter(void* arg)` that
    //      calls the actual method with the right ABI and returns the result
    //      as a void* (or null for void). The adapter is what bz_domain_spawn
    //      receives as its entry_fn.
    //   3. Lower the optional argument and pass it to bz_domain_spawn.
    // -----------------------------------------------------------------------
    {
        auto* targetId = dynamic_cast<const ast::IdentifierExpr*>(ma->target.get());
        if (targetId && targetId->name == "Domain" && mname == "Spawn" &&
            !call.args.empty()) {
            const ast::Expr* entryExpr = call.args[0].value.get();
            if (!entryExpr) return nullptr;

            // #3181 / §30.14 / bz_domain_spawn arg_ptr comment: a struct-
            // typed spawn value crosses the boundary as a heap copy of the
            // value (the address is passed as the spawn's void* arg); the
            // receiver loads it and frees the copy. Pointer-typed (class)
            // values pass through (ownership transfer); integer-typed values
            // pack their zero-extended bits into the slot; **floating-point
            // values** (`f32` / `f64`) — also sendable per §30.14 — use the
            // SAME heap-copy path the struct branch uses (one unified "any
            // value-type payload" lowering; bits-in-slot has no representation
            // for `float` and is meaningless for structs > 8 bytes). The pre-
            // fix `bitcast → i64` fallback hit the verifier for both structs
            // not exactly 8 bytes AND for f32/f64 (followed by a `zext`
            // extension on a non-integer, both illegal) — the float/double
            // case is a current-day verifier abort, not a future-type hazard.
            // sharpen #3181: unsupported LLVM types (anything that is not
            // pointer / integer / float / struct) now produce an explicit
            // codegen diagnostic instead of an opaque verifier abort.
            // fix #3239: `expectedTy` is the entry's actual parameter type.
            // For a float / double literal the static type (default f64) does
            // not match a f32 entry param — the pre-fix sender used
            // `val->getType()` for both the malloc size AND the heap-store
            // type, so an f64 literal → 8-byte slot stored as f64, while the
            // receiver read 4 bytes as f32 and got the lower 4 bytes of the
            // i64 (the low half of 3.25's double representation is all-zero).
            // Coerce first so the heap copy is the entry param's size and
            // representation; receiver-side castToParamTy already loads the
            // expected type, so the round-trip aligns.
            auto spawnValueToVoidPtr = [&](llvm::Value* val,
                                           llvm::Type*  expectedTy)
                                       -> llvm::Value* {
                if (val->getType()->isPointerTy()) return val;
                if (val->getType()->isIntegerTy(64))
                    return builder_.CreateIntToPtr(val, ptrTy(),
                                                   "spawn.arg.cast");
                if (val->getType()->isIntegerTy())
                    return builder_.CreateIntToPtr(
                        builder_.CreateZExt(val, i64()), ptrTy(),
                        "spawn.arg.cast");
                if (val->getType()->isFloatTy() ||
                    val->getType()->isDoubleTy() ||
                    val->getType()->isStructTy()) {
                    // Coerce f64 → f32 (or f32 → f64) when the literal's
                    // static type doesn't match the entry param.
                    if (expectedTy &&
                        (expectedTy->isFloatTy() ||
                         expectedTy->isDoubleTy())) {
                        if (val->getType()->isDoubleTy() &&
                            expectedTy->isFloatTy())
                            val = builder_.CreateFPTrunc(val, expectedTy,
                                "spawn.arg.coerce.f64tof32");
                        else if (val->getType()->isFloatTy() &&
                                 expectedTy->isDoubleTy())
                            val = builder_.CreateFPExt(val, expectedTy,
                                "spawn.arg.coerce.f32tof64");
                    }
                    // #3181 (struct) / sharpen #3181 (f32 / f64): heap-copy
                    // the value and pass the address. §30.14 says every
                    // sendable value type crosses as a copy; the runtime
                    // contract (bz_domain_spawn's arg_ptr comment) names the
                    // copy's ADDRESS as the spawn arg.
                    auto* mallocFn = declareC("malloc", ptrTy(), {i64()});
                    int64_t sz = static_cast<int64_t>(
                        module_->getDataLayout().getTypeAllocSize(
                            val->getType()));
                    llvm::Value* slot = builder_.CreateCall(
                        mallocFn, {builder_.getInt64(sz)},
                        "spawn.arg.slot");
                    builder_.CreateStore(val,
                        builder_.CreateBitCast(slot,
                            val->getType()->getPointerTo()));
                    return slot;
                }
                // sharpen #3181: the pre-fix `bitcast → i64` fallback
                // produced an opaque IR-verifier abort whenever a non-
                // pointer / non-integer type slipped past sema. Replace
                // with a loud codegen diagnostic that names the type — a
                // reviewer-facing failure rather than a silent wrong-IR
                // pass-through (and §30.14's sendability check at the call
                // site ought to have rejected anything that reaches here).
                if (call.args[1].value) {
                    ctx_.diagnostics().report(
                        Severity::Error, call.args[1].value->span.begin,
                        "internal: unsupported spawn-arg LLVM type in "
                        "Domain.Spawn marshalling — f32 / f64 / struct are "
                        "handled via heap copy, integer / pointer via bits "
                        "and passthrough, anything else is a codegen gap "
                        "(\xC2\xA7""30.14 / #3181)");
                }
                return builder_.CreateIntToPtr(
                    builder_.getInt64(0), ptrTy(),
                    "spawn.arg.cast.unreachable");
            };

            // Resolve the entry method symbol.
            sema::MethodSymbol* entryMs =
                dynamic_cast<sema::MethodSymbol*>(ctx_.exprSymbol(*entryExpr));

            // §30.14 / fix #1228, #1490: non-capturing lambda as Domain.Spawn
            // entry, with or without a value-typed spawn argument. The spec
            // admits `Domain.Spawn<A, R>(entry: (owned A) -> owned R, owned
            // arg: A)` and its by-value-`A` sibling (stdlib §6.8) — a single-
            // param non-capturing lambda entry, exactly like the zero-param
            // `() -> R` form fix #1228 already handled. A non-capturing lambda
            // lowers to a closure cell { fn_ptr, env_ptr=null, rc=1,
            // envdtor=null }. We build a spawn adapter that loads fn+env from
            // the cell, calls fn(env, arg...), releases the closure, and
            // marshals the result. per §30.14 / §27.5.
            if (!entryMs) {
                if (auto* lamExpr =
                        dynamic_cast<const ast::LambdaExpr*>(entryExpr)) {
                    // nExtra is 0 (`Domain.Spawn(() => …)`) or 1 (`Domain.Spawn(
                    // (a: A) => …, arg)`) per stdlib §6.8's Spawn overload set
                    // (at most one `arg: A`; multiple values are aggregated by
                    // the caller into one sendable type before this call).
                    std::size_t nExtra =
                        call.args.size() > 1 ? call.args.size() - 1 : 0;

                    // 1. Lower the lambda → closure cell (inline transient).
                    llvm::Value* closureCell = lowerLambda(*lamExpr);
                    if (!closureCell) return nullptr;

                    // 2. Compute the lambda fn's LLVM return type, mirroring
                    //    lowerLambda's retTy logic so the indirect call type-
                    //    checks in IR. fn signature: (ptr env_ptr, params...)
                    //    -> lamRetTy.
                    llvm::Type* lamRetTy = i32();  // default (unresolved)
                    if (lamExpr->isAsync) {
                        // async lambda: state machine wraps into Task → ptr.
                        lamRetTy = ptrTy();
                    } else if (lamExpr->exprBody) {
                        if (sema::TypeRef* rt =
                                ctx_.exprType(*lamExpr->exprBody))
                            lamRetTy = lowerTypeRef(rt);
                        else if (isLambdaExprBodyVoidBuiltin(*lamExpr))
                            lamRetTy = llvm::Type::getVoidTy(llvm_);
                    } else {
                        // Block-body: use the sema-inferred funcReturn.
                        if (sema::TypeRef* lamTy =
                                ctx_.exprType(*lamExpr)) {
                            if (lamTy->kind ==
                                    sema::TypeRef::Kind::Function &&
                                    lamTy->funcReturn) {
                                llvm::Type* inf =
                                    lowerTypeRef(lamTy->funcReturn);
                                if (inf != i32()) lamRetTy = inf;
                            }
                        }
                    }

                    // 2b. fix #1490: compute the lambda's user-parameter LLVM
                    // types (mirroring lowerLambda's explicit-annotation
                    // path) so the indirect call below is built with the
                    // exact signature lowerLambda gave the closure's fn —
                    // without this the adapter always called fn_ptr(env_ptr)
                    // with a single-arg function type, which is a type
                    // mismatch (undefined behavior) against the two-plus-arg
                    // function lowerLambda actually emitted for `(n: int) =>
                    // …`, corrupting the stack the pthread trampoline reads.
                    const std::vector<sema::Symbol*>* lamParamSyms =
                        ctx_.lambdaParams(lamExpr);
                    std::vector<llvm::Type*> lamParamTys;
                    if (lamParamSyms) {
                        for (sema::Symbol* ps : *lamParamSyms) {
                            auto* par =
                                dynamic_cast<sema::ParameterSymbol*>(ps);
                            llvm::Type* pty = i32();
                            if (par && par->declType) {
                                if (sema::TypeRef* rt =
                                        ctx_.resolvedType(*par->declType))
                                    pty = lowerTypeRef(rt);
                            } else if (par && par->resolvedDeclType) {
                                pty = lowerTypeRef(par->resolvedDeclType);
                            }
                            lamParamTys.push_back(pty);
                        }
                    }
                    // Lambda fn LLVM type: (ptr env_ptr, params...) -> lamRetTy.
                    std::vector<llvm::Type*> lamFnParamTys;
                    lamFnParamTys.push_back(ptrTy());
                    for (llvm::Type* t : lamParamTys) lamFnParamTys.push_back(t);
                    auto* lamFnTy = llvm::FunctionType::get(
                        lamRetTy, lamFnParamTys, false);

                    // 3. Build spawn adapter: void* adapter(void* arg).
                    //    nExtra==0: arg IS the closure cell (fix #1228 shape).
                    //    nExtra>=1: arg is a malloc'd void*[1+nExtra] array —
                    //    slot 0 the closure cell, slots 1.. the extra Spawn
                    //    args in call order (the cell itself is not sendable
                    //    data, so it cannot ride in the single-arg fast path
                    //    the static-method adapter below uses for one arg).
                    //    The adapter loads fn+env from the cell, calls
                    //    fn(env, arg...), releases the closure, and marshals
                    //    the result. per §30.14 / fix #1490/#1491.
                    static std::atomic<unsigned> spawnLamCtr{0};
                    std::string aName = "spawn.lam$" +
                        std::to_string(spawnLamCtr.fetch_add(1));
                    auto* aTy = llvm::FunctionType::get(
                        ptrTy(), {ptrTy()}, false);
                    auto* adapter = llvm::Function::Create(
                        aTy, llvm::Function::InternalLinkage,
                        aName, module_.get());

                    {
                        llvm::Function*   svFn   = currentFn_;
                        llvm::Value*      svThis = currentThis_;
                        llvm::BasicBlock* svBB   = builder_.GetInsertBlock();

                        currentFn_   = adapter;
                        currentThis_ = nullptr;
                        auto* aBB = llvm::BasicBlock::Create(
                            llvm_, "entry", adapter);
                        builder_.SetInsertPoint(aBB);

                        llvm::Value* argParam = adapter->getArg(0);

                        // Cast a packed void* slot value to the lambda
                        // param's expected type — mirrors the static-method
                        // adapter's castToParamTy below (that lambda is
                        // scoped to the entryMs-found branch, so it is not
                        // reachable here).
                        // fix #3237: mirror the static-method adapter's `castToParamTy` shape
                        // so a struct (or float / double) extra arg that the
                        // sender heap-copied is loaded back as the user
                        // param's actual type — not bitcast to i64. The
                        // pre-fix fall-through was `CreatePtrToInt(voidPtr,
                        // i64())`, which returned an i64 where the lambda's
                        // actual param type is `%choice.CInt { i32, {i8},
                        // {i32} }` (or f32 / f64); the indirect call to
                        // the lambda then mismatched the function-type's
                        // param against the i64 operand and the verifier
                        // aborted with `i32Call parameter type does not
                        // match function signature!`.
                        auto castSlotToParamTy =
                            [&](llvm::Value* voidPtr,
                                llvm::Type* paramTy) -> llvm::Value* {
                            if (paramTy->isPointerTy()) return voidPtr;
                            if (paramTy->isIntegerTy(64))
                                return builder_.CreatePtrToInt(voidPtr, i64());
                            if (paramTy->isIntegerTy(32)) {
                                llvm::Value* w =
                                    builder_.CreatePtrToInt(voidPtr, i64());
                                return builder_.CreateTrunc(w, i32());
                            }
                            if (paramTy->isFloatTy() ||
                                paramTy->isDoubleTy() ||
                                paramTy->isStructTy()) {
                                return builder_.CreateLoad(paramTy,
                                    builder_.CreateBitCast(voidPtr,
                                        paramTy->getPointerTo()));
                            }
                            return builder_.CreatePtrToInt(voidPtr, i64());
                        };

                        llvm::Value* cellArg;
                        std::vector<llvm::Value*> extraSlotVals;
                        if (nExtra == 0) {
                            cellArg = argParam;
                        } else {
                            llvm::Value* cellSlotPtr =
                                builder_.CreateInBoundsGEP(
                                    ptrTy(), argParam, builder_.getInt64(0),
                                    "spawn.lam.cellslot");
                            cellArg = builder_.CreateLoad(
                                ptrTy(), cellSlotPtr, "spawn.lam.cell");
                            for (std::size_t i = 0; i < nExtra; ++i) {
                                llvm::Value* slotPtr =
                                    builder_.CreateInBoundsGEP(
                                        ptrTy(), argParam,
                                        builder_.getInt64(
                                            static_cast<uint64_t>(i + 1)),
                                        "spawn.lam.argslot" +
                                            std::to_string(i));
                                extraSlotVals.push_back(builder_.CreateLoad(
                                    ptrTy(), slotPtr,
                                    "spawn.lam.arg" + std::to_string(i)));
                            }
                            // Free the packed array now that everything is
                            // loaded out of it (mirrors the static-method
                            // adapter's multi-arg unpack-then-free below).
                            auto* freeFn = declareC(
                                "free", llvm::Type::getVoidTy(llvm_),
                                {ptrTy()});
                            builder_.CreateCall(freeFn, {argParam});
                        }

                        // Load fn_ptr from closure cell slot 0.
                        llvm::Value* fnPtr = builder_.CreateLoad(
                            ptrTy(), cellArg, "spawn.lam.fn");
                        // Load env_ptr from closure cell slot 1 (offset 8).
                        llvm::Value* envSlot = builder_.CreateGEP(
                            ptrTy(), cellArg,
                            {builder_.getInt32(1)},
                            "spawn.lam.env.slot");
                        llvm::Value* envPtr = builder_.CreateLoad(
                            ptrTy(), envSlot, "spawn.lam.env");

                        // Invoke lambda: fn_ptr(env_ptr, arg0, arg1, ...).
                        std::vector<llvm::Value*> callArgs;
                        callArgs.push_back(envPtr);
                        for (std::size_t i = 0; i < extraSlotVals.size();
                             ++i) {
                            llvm::Type* pty = i < lamParamTys.size()
                                ? lamParamTys[i] : ptrTy();
                            callArgs.push_back(
                                castSlotToParamTy(extraSlotVals[i], pty));
                        }
                        llvm::Value* lamRet =
                            builder_.CreateCall(lamFnTy, fnPtr, callArgs);

                        // fix #3237: free each per-slot heap-copy the sender
                        // made for a struct / float / double extra arg AFTER
                        // the entry has consumed it. The static-method
                        // adapter already tracks these individually; the
                        // lambda adapter has to do the same now that the
                        // sender's `spawnValueToVoidPtr` heap-copies structs
                        // and floats instead of bitcasting them into i64 —
                        // without this every struct/float extra arg leaks
                        // (measured under LSan: one heap copy per spawn per
                        // arg).
                        auto* freeArgsFn = declareC(
                            "free", llvm::Type::getVoidTy(llvm_), {ptrTy()});
                        for (std::size_t i = 0; i < extraSlotVals.size();
                             ++i) {
                            llvm::Type* pty = i < lamParamTys.size()
                                ? lamParamTys[i] : nullptr;
                            if (pty && (pty->isStructTy() ||
                                        pty->isFloatTy() ||
                                        pty->isDoubleTy())) {
                                builder_.CreateCall(freeArgsFn,
                                    {extraSlotVals[i]});
                            }
                        }

                        // Release closure cell in the spawned domain.
                        // Non-capturing lambda: rc=1→0, env=null, envdtor=null
                        // → free(cell). Caller transfers ownership on spawn;
                        // no caller-side release → no double-free. §28.9/§27.5.
                        auto* relFn = declareC(
                            "bz_closure_release",
                            llvm::Type::getVoidTy(llvm_), {ptrTy()});
                        builder_.CreateCall(relFn, {cellArg});

                        // fix #1491: marshal the lambda's result into the
                        // adapter's void* return, mirroring the static-method
                        // adapter below. Per §30.14 / stdlib §6.8 a value entry
                        // (`() -> R`) joins as a Task<R> whose result the runtime
                        // reads back from these bytes; a class entry
                        // (`() -> owned R`) crosses as the ptr; a unit entry
                        // (`() -> ()`) crosses as null. Before this the adapter
                        // always returned null, so `() => 42` joined to 0.
                        // Async entries keep the null return here — the runtime
                        // is meant to await the returned Task in-domain rather
                        // than treat it as the result (#1490/#1492/#1493).
                        if (lamExpr->isAsync ||
                            lamRetTy->isVoidTy() ||
                            lamRet->getType()->isVoidTy()) {
                            builder_.CreateRet(
                                llvm::ConstantPointerNull::get(ptrTy()));
                        } else if (lamRet->getType()->isPointerTy()) {
                            builder_.CreateRet(lamRet);
                        } else if (lamRet->getType()->isIntegerTy()) {
                            llvm::Value* wide = lamRet;
                            if (!wide->getType()->isIntegerTy(64))
                                wide = builder_.CreateZExt(wide, i64());
                            builder_.CreateRet(
                                builder_.CreateIntToPtr(wide, ptrTy()));
                        } else {
                            // Floating-point / other value result: reinterpret
                            // the bits as i64, then inttoptr (the join reads the
                            // low bytes back out under the result type).
                            llvm::Value* bits =
                                builder_.CreateBitCast(lamRet, i64());
                            builder_.CreateRet(
                                builder_.CreateIntToPtr(bits, ptrTy()));
                        }

                        currentFn_   = svFn;
                        currentThis_ = svThis;
                        if (svBB) builder_.SetInsertPoint(svBB);
                    }

                    // 4. Lower the Spawn argument(s) and pass to
                    //    bz_domain_spawn. nExtra==0: the closure cell is the
                    //    arg directly (fix #1228 shape). nExtra>=1: pack the
                    //    closure cell + extra args into a malloc'd
                    //    void*[1+nExtra] array; the adapter frees it. Either
                    //    way ownership of the cell (and any class-typed extra
                    //    arg — moved per §30.14) transfers to the spawned
                    //    domain's adapter; no caller-side release.
                    llvm::Value* spawnArg = closureCell;
                    if (nExtra >= 1) {
                        auto* mallocFn = declareC("malloc", ptrTy(), {i64()});
                        llvm::Value* arrSize = builder_.getInt64(
                            static_cast<uint64_t>(1 + nExtra) * 8u);
                        llvm::Value* arr = builder_.CreateCall(
                            mallocFn, {arrSize}, "spawn.lam.argpack");
                        llvm::Value* cellSlotPtr = builder_.CreateInBoundsGEP(
                            ptrTy(), arr, builder_.getInt64(0),
                            "spawn.lam.cellslot.store");
                        builder_.CreateStore(closureCell, cellSlotPtr);
                        for (std::size_t i = 0; i < nExtra; ++i) {
                            const auto& a = call.args[i + 1];
                            if (!a.value) continue;
                            llvm::Value* val = lowerExpr(*a.value);
                            if (!val) {
                                // #1386: ICE — lowerExpr returned null for a
                                // Domain.Spawn argument; the expression kind
                                // is not yet handled by codegen.
                                ctx_.diagnostics().report(
                                    Severity::Error, a.value->span.begin,
                                    "internal: lowerExpr returned null for "
                                    "Domain.Spawn argument — #1386");
                                continue;
                            }
                            // fix #3237: use the same `spawnValueToVoidPtr`
                            // unified marshalling the static-method adapter
                            // uses, so a struct (or float / double) extra arg
                            // rides on the heap-copy path instead of the
                            // pre-fix `bitcast → i64` fallback. The fallback
                            // is invalid IR for any struct not exactly 8
                            // bytes — a choice like `%choice.CInt { i32, {i8},
                            // {i32} }` is 12 bytes, so `bitcast … to i64` is
                            // `Invalid bitcast`; the inttoptr cascade then
                            // produces `Referring to an argument in another
                            // function!` and `Function return type does not
                            // match operand type of return inst!` errors when
                            // the receiver (whose castSlotToParamTy also
                            // needs the struct case — fixed below) tries to
                            // round-trip the value. The expected type is the
                            // lambda's user-param type so an f64 literal is
                            // coerced to an f32 entry param just like in the
                            // static-method fix #3239.
                            llvm::Type* expectedTy = i < lamParamTys.size()
                                ? lamParamTys[i] : nullptr;
                            llvm::Value* asPtr =
                                spawnValueToVoidPtr(val, expectedTy);
                            llvm::Value* slotPtr = builder_.CreateInBoundsGEP(
                                ptrTy(), arr,
                                builder_.getInt64(
                                    static_cast<uint64_t>(i + 1)),
                                "spawn.lam.argslot.store" +
                                    std::to_string(i));
                            builder_.CreateStore(asPtr, slotPtr);
                        }
                        spawnArg = arr;
                    }

                    // 5. bz_domain_spawn(adapter, spawnArg, dfe_vtable).
                    auto* spawnFn = declareC("bz_domain_spawn", ptrTy(),
                                             {ptrTy(), ptrTy(), ptrTy()});
                    return builder_.CreateCall(
                        spawnFn,
                        {adapter, spawnArg, getDFEVtable()},
                        "domain.spawn");
                }
                return nullptr;
            }

            // Find the actual LLVM function for the method.
            auto mfIt = methodFns_.find(entryMs);
            if (mfIt == methodFns_.end()) return nullptr;
            llvm::Function* origFn = mfIt->second;

            // Compute the number of extra arguments (args beyond pos 0).
            // Each extra arg is owned-transferred into the spawned domain.
            // §30.14 / #693: all args at pos >= 1 are owned-consumed.
            std::size_t nExtra = call.args.size() > 1 ? call.args.size() - 1 : 0;
            std::size_t nOrigParams = origFn->arg_size();

            // Build the adapter: `void* adapter(void* arg)`.
            // The adapter's body:
            //   - N==0 params: call origFn(), ignore arg.
            //   - N==1 param:  call origFn(arg) — arg IS the single param value.
            //   - N>=2 params: arg is a malloc'd void*[N] array; unpack each slot,
            //     free the array, then call origFn(p0, p1, …).
            // Return value: origFn's result cast to void* (or null for void).
            // per §30.14 / #693
            static std::atomic<unsigned> spawnAdaptCtr{0};
            std::string adaptName = "spawn.adapter$" +
                std::to_string(spawnAdaptCtr.fetch_add(1));
            auto* adaptFnTy = llvm::FunctionType::get(ptrTy(), {ptrTy()}, false);
            auto* adapter = llvm::Function::Create(
                adaptFnTy, llvm::Function::InternalLinkage,
                adaptName, module_.get());

            // Emit the adapter body.
            {
                llvm::Function* savedFn = currentFn_;
                llvm::Value*    savedThis = currentThis_;
                llvm::BasicBlock* savedBB = builder_.GetInsertBlock();

                currentFn_   = adapter;
                currentThis_ = nullptr;
                auto* entryBB = llvm::BasicBlock::Create(llvm_, "entry", adapter);
                builder_.SetInsertPoint(entryBB);

                llvm::Value* argParam = adapter->getArg(0);  // void* arg

                // Helper: cast a void* to the expected param type.
                // For struct-typed params (#3181: a value-like sendable
                // choice or sendable record) and for f32 / f64 sendable
                // primitives (sharpen #3181), the void* is the ADDRESS of a
                // heap copy the caller malloc'd; load the value out of it.
                // The caller records the same slot in heapCopiesToFree so
                // we free it after the entry call returns. The pre-fix
                // ptrtoint fallback is invalid for any non-trivial struct
                // and silently wrong for any 8-byte struct; for f32 / f64
                // the pre-fix code hit the verifier with `ptrtoint` of a
                // pointer-as-i64 fed into a float call.
                auto castToParamTy = [&](llvm::Value* voidPtr,
                                         llvm::Type*  paramTy) -> llvm::Value* {
                    if (paramTy->isPointerTy()) return voidPtr;
                    if (paramTy->isIntegerTy(64))
                        return builder_.CreatePtrToInt(voidPtr, i64());
                    if (paramTy->isIntegerTy(32)) {
                        llvm::Value* w = builder_.CreatePtrToInt(voidPtr, i64());
                        return builder_.CreateTrunc(w, i32());
                    }
                    if (paramTy->isFloatTy() ||
                        paramTy->isDoubleTy() ||
                        paramTy->isStructTy()) {
                        // #3181 (struct) / sharpen #3181 (f32 / f64):
                        // heap-copy of the value — load it.
                        return builder_.CreateLoad(paramTy,
                            builder_.CreateBitCast(voidPtr,
                                paramTy->getPointerTo()));
                    }
                    return builder_.CreatePtrToInt(voidPtr, i64());
                };

                // Track void* slots that the sender malloc'd as heap copies
                // — we free each one after the entry call returns.
                // Pointer / int args do not allocate (the slot itself
                // carries the value or pointer bits); struct args DO, and
                // f32 / f64 sendable primitives DO (sharpen #3181 — they
                // ride on the same heap-copy path as structs). The
                // multi-arg packed array's slots are freed together with
                // the array below; we only need to track each heap copy
                // individually here.
                std::vector<llvm::Value*> heapCopiesToFree;
                auto* freeFn = declareC("free",
                                        llvm::Type::getVoidTy(llvm_),
                                        {ptrTy()});

                std::vector<llvm::Value*> callArgs;
                if (nOrigParams == 1) {
                    // Single-argument entry: arg IS the param value (single void*).
                    llvm::Type* pTy = origFn->getArg(0)->getType();
                    callArgs.push_back(castToParamTy(argParam, pTy));
                    if (pTy->isStructTy() || pTy->isFloatTy() ||
                        pTy->isDoubleTy())
                        heapCopiesToFree.push_back(argParam);
                } else if (nOrigParams >= 2) {
                    // Multi-argument entry: arg is a void*[N] malloc'd array.
                    // Unpack each slot from the array, then free the array
                    // AND any per-slot heap-copies the sender made.
                    for (std::size_t i = 0; i < nOrigParams; ++i) {
                        // GEP into the void*[] at index i.
                        llvm::Value* slotPtr = builder_.CreateInBoundsGEP(
                            ptrTy(), argParam,
                            builder_.getInt64(static_cast<uint64_t>(i)),
                            "spawn.slot" + std::to_string(i));
                        llvm::Value* slotVal = builder_.CreateLoad(
                            ptrTy(), slotPtr, "spawn.val" + std::to_string(i));
                        llvm::Type* pTy = origFn->getArg(i)->getType();
                        callArgs.push_back(castToParamTy(slotVal, pTy));
                        if (pTy->isStructTy() || pTy->isFloatTy() ||
                            pTy->isDoubleTy())
                            heapCopiesToFree.push_back(slotVal);
                    }
                    // Free the packed-arg array after unpacking.
                    builder_.CreateCall(freeFn, {argParam});
                }
                // else: nOrigParams == 0 — no-arg entry, no call args.

                llvm::Value* ret = builder_.CreateCall(origFn, callArgs);
                // #3181 / sharpen #3181: free every heap-copy the sender
                // made for a struct / f32 / f64 arg AFTER the entry has
                // consumed it (the entry runs synchronously here, so the
                // copy is no longer needed).
                for (llvm::Value* p : heapCopiesToFree)
                    builder_.CreateCall(freeFn, {p});
                if (origFn->getReturnType()->isVoidTy()) {
                    builder_.CreateRet(llvm::ConstantPointerNull::get(ptrTy()));
                } else if (origFn->getReturnType()->isPointerTy()) {
                    builder_.CreateRet(ret);
                } else {
                    // Value result: inttoptr. fix #3239: the pre-fix code
                    // did `ZExt(ret, i64)` unconditionally for any non-
                    // integer type, which is a verifier abort for a float /
                    // double entry (`ZExt only operates on integer`); the
                    // IR builder accepts the call silently under -DNDEBUG
                    // and the verifier catches it downstream. For a float
                    // entry the bitcast → int → zext → i64 path puts the
                    // float bits in the low 4 bytes of an i64 (the same
                    // shape the lambda adapter's else branch uses); the
                    // await site's `CreateLoad(resTy, resPtr)` reads
                    // `resultTy` bytes from offset 0, so a Task<float> sees
                    // the original f32 bits. For double: bitcast f64 → i64
                    // directly (both are 8 bytes; no zext needed). For an
                    // integer result the existing ZExt → i64 path keeps
                    // working.
                    llvm::Value* wide = ret;
                    if (wide->getType()->isFloatTy()) {
                        // f32 → i32 (bitcast) → i64 (zext).
                        wide = builder_.CreateBitCast(wide, i32(),
                            "spawn.ret.f32toi32");
                        wide = builder_.CreateZExt(wide, i64(),
                            "spawn.ret.zext");
                    } else if (wide->getType()->isDoubleTy()) {
                        // f64 → i64 (bitcast; same size).
                        wide = builder_.CreateBitCast(wide, i64(),
                            "spawn.ret.f64toi64");
                    } else if (!wide->getType()->isIntegerTy(64)) {
                        wide = builder_.CreateZExt(wide, i64(),
                            "spawn.ret.zext");
                    }
                    builder_.CreateRet(
                        builder_.CreateIntToPtr(wide, ptrTy()));
                }

                // Restore emitter state.
                currentFn_   = savedFn;
                currentThis_ = savedThis;
                if (savedBB) builder_.SetInsertPoint(savedBB);
            }

            // Lower the argument(s) to pass to bz_domain_spawn.
            // N==0: null.
            // N==1: the single extra arg as void*.
            // N>=2: malloc a void*[N] array, store each arg, pass the array.
            // per §30.14 / #693
            llvm::Value* argVal = llvm::ConstantPointerNull::get(ptrTy());
            if (nExtra == 1 && call.args[1].value) {
                // Single extra arg — heap-copy path for struct / float / double
                // (fix #3181 + sharpen #3181: `spawnValueToVoidPtr` is the
                // unified value-type payload lowering; the pre-fix
                // `bitcast → i64` fallback hit the verifier for any struct not
                // exactly 8 bytes, and the sharpen added f32 / f64 + an
                // explicit diagnostic for unsupported types). The fallback
                // below (now `--begin unreachable--`) is the same diagnostic-
                // gated path the lambda uses upstream.
                llvm::Value* argExprV = lowerExpr(*call.args[1].value);
                if (argExprV) {
                    // fix #3239: pass the entry's first param type so a
                    // double-typed literal is truncated to a float param (the
                    // pre-fix spawn used the literal's static type for both
                    // the heap-copy size and the heap-store type, so the
                    // receiver's 4-byte f32 read got the lower half of an
                    // 8-byte f64 heap copy and observed 0.0).
                    llvm::Type* expectedTy = nOrigParams >= 1
                        ? origFn->getArg(0)->getType() : nullptr;
                    argVal = spawnValueToVoidPtr(argExprV, expectedTy);
                } else {
                    // #1386: ICE — lowerExpr returned null for a single
                    // Domain.Spawn argument; arg silently passed as null.
                    ctx_.diagnostics().report(
                        Severity::Error, call.args[1].value->span.begin,
                        "internal: lowerExpr returned null for single-arg "
                        "Domain.Spawn argument — #1386");
                }
            } else if (nExtra >= 2) {
                // Multi-arg: malloc a void*[nExtra] array and store each arg.
                // The adapter will free the array after unpacking.
                auto* mallocFn = declareC("malloc", ptrTy(), {i64()});
                llvm::Value* arrSize = builder_.getInt64(
                    static_cast<uint64_t>(nExtra) * 8u);  // nExtra * sizeof(void*)
                llvm::Value* arr = builder_.CreateCall(mallocFn, {arrSize},
                                                       "spawn.argpack");
                for (std::size_t i = 0; i < nExtra; ++i) {
                    const auto& a = call.args[i + 1];
                    if (!a.value) continue;
                    llvm::Value* val = lowerExpr(*a.value);
                    if (!val) {
                        // #1386: ICE — lowerExpr returned null for a
                        // Domain.Spawn extra argument; the expression kind
                        // is not yet handled by codegen.
                        ctx_.diagnostics().report(
                            Severity::Error, a.value->span.begin,
                            "internal: lowerExpr returned null for "
                            "Domain.Spawn argument — #1386");
                        continue;
                    }
                    // Cast to void* if needed.
                    llvm::Value* asPtr;
                    if (val->getType()->isPointerTy()) {
                        asPtr = val;
                    } else if (val->getType()->isIntegerTy(64)) {
                        asPtr = builder_.CreateIntToPtr(val, ptrTy());
                    } else if (val->getType()->isIntegerTy()) {
                        asPtr = builder_.CreateIntToPtr(
                            builder_.CreateZExt(val, i64()), ptrTy());
                    } else {
                        asPtr = builder_.CreateIntToPtr(
                            builder_.CreateBitCast(val, i64()), ptrTy());
                    }
                    llvm::Value* slotPtr = builder_.CreateInBoundsGEP(
                        ptrTy(), arr,
                        builder_.getInt64(static_cast<uint64_t>(i)),
                        "spawn.slot" + std::to_string(i));
                    builder_.CreateStore(asPtr, slotPtr);
                }
                argVal = arr;
            }

            auto* spawnFn = declareC("bz_domain_spawn", ptrTy(),
                                     {ptrTy(), ptrTy(), ptrTy()});
            return builder_.CreateCall(spawnFn,
                {adapter, argVal, getDFEVtable()},
                "domain.spawn");
        }
    }

    return nullptr;
}

// ---------------------------------------------------------------------------
// §30.12 (v0.179 / #1522): would-park guard shared by Mutex.Acquire()/
// TryAcquire(timeout) and Semaphore.Acquire()/TryAcquire(timeout).
//
// Both types' blocking forms are rejected LEXICALLY inside `async` methods/
// lambdas by sema (ExprTypecheck_Calls.cpp, next to the Task.Wait()/.Result
// reject of §30.3/#1362). This is the runtime layer for the case static
// rejection can't see: a non-`async` helper invoked from a domain
// continuation. Mirrors the Task guard's shape (LlvmBackend_Exprs.cpp
// `.Result`, LlvmBackend_Calls2.cpp `t.Wait()`) but a Mutex/Semaphore core has
// no side-effect-free "is available" peek the way `bz_task_is_done` is for a
// task, so availability is tested by actually attempting a non-blocking
// acquire (`tryAcquireFnName(handle, 0)`) — the probe result IS the
// acquisition on success, avoiding a separate blocking call and a TOCTOU race
// between "check" and "acquire".
//
// - Outside a domain (startup thread / FFI, stdlib §6.2/§6.3 "outside any
//   domain"): behavior is unchanged — `timeoutMs == nullptr` calls the plain
//   blocking Acquire(); a non-null `timeoutMs` calls TryAcquire with the
//   REQUESTED timeout (a timeout -> null is a legitimate outcome, not a
//   guard fire).
// - Inside a domain continuation: probe with timeout=0 regardless of the
//   caller's requested timeout — even a bounded wait still parks a pool
//   worker (§30.12: "never parks a thread inside a domain continuation").
//   An immediately-available acquisition succeeds exactly as it would
//   outside a domain (spec: "proceeds as before"); an unavailable one raises
//   InvalidOperationException instead of blocking.
// ---------------------------------------------------------------------------
llvm::Value* Emitter::emitGuardedBlockingAcquire(
        llvm::Value* handle, const char* tryAcquireFnName,
        const char* acquireFnName, llvm::Value* timeoutMs) {
    auto* tryFn = declareC(tryAcquireFnName, ptrTy(), {ptrTy(), i64()});
    auto* curDomFn = declareC("bz_current_domain_ptr", ptrTy(), {});
    llvm::Value* dom = builder_.CreateCall(curDomFn, {}, "acq.dom");
    llvm::Value* inDomain = builder_.CreateICmpNE(
        dom, llvm::Constant::getNullValue(ptrTy()), "acq.indomain");

    auto* curFn = currentFn_;
    auto* domBB = llvm::BasicBlock::Create(llvm_, "acq.dom.probe", curFn);
    auto* outBB = llvm::BasicBlock::Create(llvm_, "acq.outdom", curFn);
    auto* joinBB = llvm::BasicBlock::Create(llvm_, "acq.join", curFn);
    builder_.CreateCondBr(inDomain, domBB, outBB);

    // Inside a domain: non-blocking probe (timeout=0). Unavailable -> would
    // park a worker -> InvalidOperationException (§30.12) instead of blocking.
    builder_.SetInsertPoint(domBB);
    llvm::Value* probeResult = builder_.CreateCall(
        tryFn, {handle, llvm::ConstantInt::get(i64(), 0)}, "acq.probe");
    llvm::Value* probeFailed = builder_.CreateICmpEQ(
        probeResult, llvm::Constant::getNullValue(ptrTy()), "acq.probe.fail");
    emitGuardThrow(probeFailed, "InvalidOperationException");
    llvm::BasicBlock* domEndBB = builder_.GetInsertBlock();
    builder_.CreateBr(joinBB);

    // Outside a domain: unchanged behavior — full blocking Acquire(), or
    // TryAcquire's real requested timeout.
    builder_.SetInsertPoint(outBB);
    llvm::Value* outResult;
    if (timeoutMs) {
        outResult = builder_.CreateCall(tryFn, {handle, timeoutMs}, "acq.tryacq");
    } else {
        auto* blockFn = declareC(acquireFnName, ptrTy(), {ptrTy()});
        outResult = builder_.CreateCall(blockFn, {handle}, "acq.block");
    }
    builder_.CreateBr(joinBB);

    builder_.SetInsertPoint(joinBB);
    auto* phi = builder_.CreatePHI(ptrTy(), 2, "acq.result");
    phi->addIncoming(probeResult, domEndBB);
    phi->addIncoming(outResult, outBB);
    return phi;
}

// ---------------------------------------------------------------------------
// §6.2 / issue #647: Mutex instance methods.
//
//   Acquire()                 → bz_mutex_acquire(mx)             → ptr (owned Lock)
//   TryAcquire(timeout: ts)   → bz_mutex_try_acquire(mx, ms)     → ptr? (owned Lock?)
//   AcquireAsync(token?)      → bz_mutex_acquire_async(mx,t,oce) → ptr (OwningTask<Lock>)
//   TryAcquireAsync(ts,tok?)  → bz_mutex_try_acquire_async(...)  → ptr (OwningTask<Lock?>)
//   Dispose()                 → handled in the explicit-Dispose block above.
//
// TryAcquire lowers the TimeSpan argument to milliseconds (i64) using
// bz_timespan_total_milliseconds, truncated to int64_t. A null return from the
// runtime (timeout) is returned as a null ptr to the Bezel nullable owned Lock?.
// Acquire() and TryAcquire() are additionally guarded by
// emitGuardedBlockingAcquire (§30.12 / #1522, above).
// ---------------------------------------------------------------------------
llvm::Value* Emitter::tryLowerMutexLockCall(const ast::CallExpr& call) {
    if (!call.callee) return nullptr;
    auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(call.callee.get());
    if (!ma || !ma->target) return nullptr;

    const std::string_view mname = ma->member;
    const sema::TypeRef* recvTy = ctx_.exprType(*ma->target);
    if (!recvTy || builtinTypeName(recvTy) != "Mutex") return nullptr;

    llvm::Value* mx = lowerExpr(*ma->target);
    if (!mx) return nullptr;

    if (mname == "Acquire" && call.args.empty()) {
        // Acquire(): blocks until the mutex is available, returns owned Lock.
        // per §6.2: the non-null return makes the guard idiom straightforward.
        // §30.12 (v0.179 / #1522): guarded by emitGuardedBlockingAcquire — a
        // non-async helper reached from a domain continuation raises
        // InvalidOperationException instead of parking when the mutex is not
        // immediately available; outside a domain the blocking call is
        // unchanged (stdlib §6.2 "Blocking discipline").
        return emitGuardedBlockingAcquire(
            mx, "bz_mutex_try_acquire", "bz_mutex_acquire", nullptr);
    }

    if (mname == "TryAcquire" && call.args.size() == 1 && call.args[0].value) {
        // TryAcquire(timeout: TimeSpan): owned Lock?
        // Convert the TimeSpan (i64 tick value) to milliseconds (i64) and
        // call bz_mutex_try_acquire(mutex, timeout_ms). Returns null on timeout.
        llvm::Value* tsVal = lowerExpr(*call.args[0].value);
        if (!tsVal) return nullptr;
        // TimeSpan is stored as an i64 tick value (100-ns units).
        // Convert to milliseconds via bz_timespan_total_milliseconds (returns double).
        // Then truncate to i64 for the runtime's timeout_ms parameter.
        if (!tsVal->getType()->isIntegerTy(64)) {
            // Coerce to i64 if needed (e.g. i32 zero-extension).
            tsVal = builder_.CreateSExtOrTrunc(tsVal, i64(), "ts.i64");
        }
        auto* msFn = declareC("bz_timespan_total_milliseconds",
                               llvm::Type::getDoubleTy(llvm_), {i64()});
        llvm::Value* msDouble = builder_.CreateCall(msFn, {tsVal}, "ts.ms");
        // Truncate double to i64 (floor towards zero per standard C conv).
        llvm::Value* msI64 = builder_.CreateFPToSI(
            msDouble, i64(), "ts.ms.i64");
        // §30.12 (v0.179 / #1522): guarded — inside a domain continuation, a
        // busy mutex is probed non-blocking (timeout=0) instead of waiting out
        // `msI64`, since a bounded park still pins a worker; unavailable ->
        // InvalidOperationException. Outside a domain, `msI64` is honored as
        // before (a legitimate timeout -> null is unaffected).
        return emitGuardedBlockingAcquire(
            mx, "bz_mutex_try_acquire", "bz_mutex_acquire", msI64);
    }

    // §6.2 / #647 / v0.139: AcquireAsync / TryAcquireAsync.
    //
    // AcquireAsync(token := CancellationToken.None): OwningTask<Lock>
    //   → bz_mutex_acquire_async(mx, token, oce_vtable)
    // TryAcquireAsync(timeout: TimeSpan,
    //                  token := CancellationToken.None): OwningTask<Lock?>
    //   → bz_mutex_try_acquire_async(mx, timeout_ms, token, oce_vtable)
    //
    // token is the last argument (optional with default CancellationToken.None).
    // If the argument is absent or the runtime default applies, emit a null ptr
    // (CancellationToken.None = null per §6.1 runtime convention).
    //
    // oce_vtable: retrieve from builtinVtable_ for OperationCanceledException;
    // fall back to null (disables OCE object construction, fault still fires).

    // Helper: retrieve the OCE vtable value (same as in tryLowerStaticCall /
    // Task.Delay cancelable path).
    auto getOceVtable = [&]() -> llvm::Value* {
        auto vit = builtinVtable_.find("OperationCanceledException");
        if (vit != builtinVtable_.end())
            return builder_.CreateBitCast(vit->second, ptrTy());
        return llvm::ConstantPointerNull::get(ptrTy());
    };

    // Helper: extract the token argument (last arg, optional).
    // If not provided or evaluates to a null-typed expr, return null ptr
    // (CancellationToken.None).
    auto getTokenArg = [&](size_t argIdx) -> llvm::Value* {
        if (argIdx < call.args.size() && call.args[argIdx].value) {
            llvm::Value* tok = lowerExpr(*call.args[argIdx].value);
            if (tok) return tok;
        }
        // Default: CancellationToken.None = null ptr.
        return llvm::ConstantPointerNull::get(ptrTy());
    };

    if (mname == "AcquireAsync") {
        // AcquireAsync(token := CancellationToken.None)
        // token is args[0] if provided; otherwise null.
        llvm::Value* token = getTokenArg(0);
        llvm::Value* oceVtable = getOceVtable();
        auto* fn = declareC("bz_mutex_acquire_async",
                             ptrTy(), {ptrTy(), ptrTy(), ptrTy()});
        return builder_.CreateCall(fn, {mx, token, oceVtable}, "mx.acq.async");
    }

    if (mname == "TryAcquireAsync" && !call.args.empty() && call.args[0].value) {
        // TryAcquireAsync(timeout: TimeSpan,
        //                  token := CancellationToken.None)
        // timeout is args[0]; token is args[1] if present.
        llvm::Value* tsVal = lowerExpr(*call.args[0].value);
        if (!tsVal) return nullptr;
        // Convert TimeSpan (i64 ticks, 100-ns units) to milliseconds.
        if (!tsVal->getType()->isIntegerTy(64))
            tsVal = builder_.CreateSExtOrTrunc(tsVal, i64(), "ts.i64");
        auto* msFn = declareC("bz_timespan_total_milliseconds",
                               llvm::Type::getDoubleTy(llvm_), {i64()});
        llvm::Value* msDouble = builder_.CreateCall(msFn, {tsVal}, "ts.ms");
        llvm::Value* msI64 = builder_.CreateFPToSI(msDouble, i64(), "ts.ms.i64");
        llvm::Value* token = getTokenArg(1);
        llvm::Value* oceVtable = getOceVtable();
        auto* fn = declareC("bz_mutex_try_acquire_async",
                             ptrTy(), {ptrTy(), i64(), ptrTy(), ptrTy()});
        return builder_.CreateCall(fn,
            {mx, msI64, token, oceVtable}, "mx.tryacq.async");
    }

    return nullptr;
}

// ---------------------------------------------------------------------------
// §6.3 / issue #647: Semaphore instance methods.
//
//   Acquire()                 → bz_semaphore_acquire(sem)             → ptr (SemaphoreToken)
//   TryAcquire(timeout: ts)   → bz_semaphore_try_acquire(sem, ms)     → ptr? (SemaphoreToken?)
//   AcquireAsync(token?)      → bz_semaphore_acquire_async(sem,t,oce) → ptr (OwningTask<SemaphoreToken>)
//   TryAcquireAsync(ts,tok?)  → bz_semaphore_try_acquire_async(...)   → ptr (OwningTask<SemaphoreToken?>)
//   Dispose()                 → handled in the explicit-Dispose block above.
//
// Mirrors tryLowerMutexLockCall exactly, substituting Semaphore/SemaphoreToken
// for Mutex/Lock and the bz_semaphore_* runtime functions.
// ---------------------------------------------------------------------------
llvm::Value* Emitter::tryLowerSemaphoreTokenCall(const ast::CallExpr& call) {
    if (!call.callee) return nullptr;
    auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(call.callee.get());
    if (!ma || !ma->target) return nullptr;

    const std::string_view mname = ma->member;
    const sema::TypeRef* recvTy = ctx_.exprType(*ma->target);
    if (!recvTy || builtinTypeName(recvTy) != "Semaphore") return nullptr;

    llvm::Value* sem = lowerExpr(*ma->target);
    if (!sem) return nullptr;

    if (mname == "Acquire" && call.args.empty()) {
        // Acquire(): blocks until a permit is available, returns owned SemaphoreToken.
        // §30.12 (v0.179 / #1522): guarded — see tryLowerMutexLockCall's
        // Acquire() case; identical discipline for Semaphore (stdlib §6.3).
        return emitGuardedBlockingAcquire(
            sem, "bz_semaphore_try_acquire", "bz_semaphore_acquire", nullptr);
    }

    if (mname == "TryAcquire" && call.args.size() == 1 && call.args[0].value) {
        // TryAcquire(timeout: TimeSpan): owned SemaphoreToken?
        // Convert TimeSpan to milliseconds (i64) and call bz_semaphore_try_acquire.
        llvm::Value* tsVal = lowerExpr(*call.args[0].value);
        if (!tsVal) return nullptr;
        if (!tsVal->getType()->isIntegerTy(64))
            tsVal = builder_.CreateSExtOrTrunc(tsVal, i64(), "ts.i64");
        auto* msFn = declareC("bz_timespan_total_milliseconds",
                               llvm::Type::getDoubleTy(llvm_), {i64()});
        llvm::Value* msDouble = builder_.CreateCall(msFn, {tsVal}, "ts.ms");
        llvm::Value* msI64 = builder_.CreateFPToSI(msDouble, i64(), "ts.ms.i64");
        // §30.12 (v0.179 / #1522): guarded — see tryLowerMutexLockCall's
        // TryAcquire() case.
        return emitGuardedBlockingAcquire(
            sem, "bz_semaphore_try_acquire", "bz_semaphore_acquire", msI64);
    }

    // §6.3 / #647 / v0.140: AcquireAsync / TryAcquireAsync.
    //   AcquireAsync(token?)   → bz_semaphore_acquire_async(sem, token, oce_vtable)
    //   TryAcquireAsync(ts, token?) → bz_semaphore_try_acquire_async(...)
    auto getOceVtable = [&]() -> llvm::Value* {
        auto vit = builtinVtable_.find("OperationCanceledException");
        if (vit != builtinVtable_.end())
            return builder_.CreateBitCast(vit->second, ptrTy());
        return llvm::ConstantPointerNull::get(ptrTy());
    };
    auto getTokenArg = [&](size_t argIdx) -> llvm::Value* {
        if (argIdx < call.args.size() && call.args[argIdx].value) {
            llvm::Value* tok = lowerExpr(*call.args[argIdx].value);
            if (tok) return tok;
        }
        return llvm::ConstantPointerNull::get(ptrTy());
    };

    if (mname == "AcquireAsync") {
        llvm::Value* token = getTokenArg(0);
        llvm::Value* oceVtable = getOceVtable();
        auto* fn = declareC("bz_semaphore_acquire_async",
                             ptrTy(), {ptrTy(), ptrTy(), ptrTy()});
        return builder_.CreateCall(fn, {sem, token, oceVtable}, "sem.acq.async");
    }

    if (mname == "TryAcquireAsync" && !call.args.empty() && call.args[0].value) {
        llvm::Value* tsVal = lowerExpr(*call.args[0].value);
        if (!tsVal) return nullptr;
        if (!tsVal->getType()->isIntegerTy(64))
            tsVal = builder_.CreateSExtOrTrunc(tsVal, i64(), "ts.i64");
        auto* msFn = declareC("bz_timespan_total_milliseconds",
                               llvm::Type::getDoubleTy(llvm_), {i64()});
        llvm::Value* msDouble = builder_.CreateCall(msFn, {tsVal}, "ts.ms");
        llvm::Value* msI64 = builder_.CreateFPToSI(msDouble, i64(), "ts.ms.i64");
        llvm::Value* token = getTokenArg(1);
        llvm::Value* oceVtable = getOceVtable();
        auto* fn = declareC("bz_semaphore_try_acquire_async",
                             ptrTy(), {ptrTy(), i64(), ptrTy(), ptrTy()});
        return builder_.CreateCall(fn,
            {sem, msI64, token, oceVtable}, "sem.tryacq.async");
    }

    return nullptr;
}


// §6.4 / issue #647: Atomic<T> / AtomicInteger<T> instance methods.
//
// Handles:
//   Exchange(value: T): T      → bz_atomic_exchange(atom, val, cell_size)
//   CompareExchange(expected: T, replacement: T): T
//                              → bz_atomic_compare_exchange(atom, exp, rep, cell_size)
//   Increment(): T             → bz_atomic_add(atom,  1, signed_, cell_size, ove)
//   Decrement(): T             → bz_atomic_add(atom, -1, signed_, cell_size, ove)
//   Add(delta: T): T           → bz_atomic_add(atom, delta, signed_, cell_size, ove)
//
// The Value property setter `atom.Value := v` is handled in LlvmBackend_Stmts.cpp
// (tryLowerAtomicValueSet) so it intercepts before the plain lvalue path.
// The Value getter is handled in LlvmBackend_Exprs.cpp (cn == "Atomic" branch).
llvm::Value* Emitter::tryLowerAtomicCall(const ast::CallExpr& call) {
    // Callee must be a MemberAccessExpr whose target type is Atomic<T> or
    // AtomicInteger<T>.
    auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(call.callee.get());
    if (!ma || !ma->target) return nullptr;
    auto* recvTy = ctx_.exprType(*ma->target);
    std::string_view rbn = builtinTypeName(recvTy);
    if (rbn != "Atomic" && rbn != "AtomicInteger") return nullptr;

    const std::string mname(ma->member);
    llvm::Value* atom = lowerExpr(*ma->target);
    if (!atom) return nullptr;

    // Helper: compute cell_size from T (genericArgs[0]).
    auto getCellSize = [&]() -> int {
        if (!recvTy->genericArgs.empty() && recvTy->genericArgs[0]) {
            const sema::TypeRef* tArg = recvTy->genericArgs[0];
            if (tArg->kind == sema::TypeRef::Kind::Primitive) {
                using P = sema::PrimitiveType;
                switch (tArg->primitive) {
                    case P::Bool: case P::Byte: case P::SByte: return 1;
                    case P::Short: case P::UShort:             return 2;
                    case P::Long: case P::ULong:               return 8;
                    default: return 4;
                }
            }
            return 8;  // IntPtr/UIntPtr
        }
        return 4;
    };

    // Helper: is T a signed type (for overflow detection in bz_atomic_add).
    auto isSigned = [&]() -> bool {
        if (!recvTy->genericArgs.empty() && recvTy->genericArgs[0]) {
            const sema::TypeRef* tArg = recvTy->genericArgs[0];
            if (tArg->kind == sema::TypeRef::Kind::Primitive) {
                using P = sema::PrimitiveType;
                auto p = tArg->primitive;
                return (p == P::SByte || p == P::Short ||
                        p == P::Int   || p == P::Long);
            }
        }
        return true;  // default: int (signed)
    };

    // Helper: get T's LLVM type for truncation.
    auto getTTy = [&]() -> llvm::Type* {
        if (!recvTy->genericArgs.empty() && recvTy->genericArgs[0])
            if (auto* ty = lowerTypeRef(recvTy->genericArgs[0])) return ty;
        return i32();
    };

    // Helper: truncate an i64 result to T's width.
    auto truncResult = [&](llvm::Value* v64, const char* name) -> llvm::Value* {
        llvm::Type* tTy = getTTy();
        if (tTy->isIntegerTy() && !tTy->isIntegerTy(64))
            return builder_.CreateTrunc(v64, tTy, name);
        return v64;
    };

    int cs = getCellSize();

    if (mname == "Exchange" && !call.args.empty() && call.args[0].value) {
        // Exchange(value: T): T — seq-cst swap, returns old value.
        llvm::Value* rawVal = lowerExpr(*call.args[0].value);
        if (!rawVal) return nullptr;
        // #2605 / §7.4: a flow-narrowed `T?` operand (proved non-null by a
        // preceding null-check) lowers to its tagged-optional {i8,T} struct;
        // unwrap to the bare T payload before widening — the same unwrap the
        // user-method binding path performs (unwrapNarrowedNullable, mirrors
        // the narrowed value-type receiver at LlvmBackend_Calls2.cpp ~6623).
        // An un-narrowed `T?` is rejected by sema (#2602), so codegen only
        // sees a narrowed or plain-T operand here; the helper is a no-op for
        // a plain primitive.
        rawVal = unwrapNarrowedNullable(rawVal, getTTy());
        // Widen arg to i64.
        llvm::Value* v64 = rawVal;
        if (rawVal->getType()->isIntegerTy() && !rawVal->getType()->isIntegerTy(64))
            v64 = builder_.CreateZExt(rawVal, i64(), "exch.v64");
        auto* fn = declareC("bz_atomic_exchange", i64(), {ptrTy(), i64(), i32()});
        llvm::Value* old64 = builder_.CreateCall(
            fn, {atom, v64, builder_.getInt32(cs)}, "atom.exch");
        return truncResult(old64, "atom.exch.v");
    }

    if (mname == "CompareExchange" &&
        call.args.size() >= 2 && call.args[0].value && call.args[1].value) {
        // CompareExchange(expected: T, replacement: T): T — strong CAS.
        llvm::Value* expVal = lowerExpr(*call.args[0].value);
        llvm::Value* repVal = lowerExpr(*call.args[1].value);
        if (!expVal || !repVal) return nullptr;
        // #2605 / §7.4: unwrap a flow-narrowed `T?` (expected/replacement)
        // to its bare payload before widening — see Exchange above.
        expVal = unwrapNarrowedNullable(expVal, getTTy());
        repVal = unwrapNarrowedNullable(repVal, getTTy());
        auto widen = [&](llvm::Value* v) -> llvm::Value* {
            if (v->getType()->isIntegerTy() && !v->getType()->isIntegerTy(64))
                return builder_.CreateZExt(v, i64());
            return v;
        };
        auto* fn = declareC("bz_atomic_compare_exchange",
                             i64(), {ptrTy(), i64(), i64(), i32()});
        llvm::Value* obs64 = builder_.CreateCall(
            fn, {atom, widen(expVal), widen(repVal), builder_.getInt32(cs)},
            "atom.cas");
        return truncResult(obs64, "atom.cas.v");
    }

    // AtomicInteger<T> arithmetic (§6.4): Increment, Decrement, Add.
    // bz_atomic_add(atom, delta, signed_, cell_size, ove_vtable) → post-add as i64.
    if (rbn == "AtomicInteger") {
        auto getOveVtable = [&]() -> llvm::Value* {
            auto vit = builtinVtable_.find("OverflowException");
            if (vit != builtinVtable_.end())
                return builder_.CreateBitCast(vit->second, ptrTy());
            return llvm::ConstantPointerNull::get(ptrTy());
        };
        auto* addFn = declareC("bz_atomic_add",
                                i64(), {ptrTy(), i64(), i32(), i32(), ptrTy()});
        int sg = isSigned() ? 1 : 0;

        if (mname == "Increment" && call.args.empty()) {
            llvm::Value* res = builder_.CreateCall(
                addFn,
                {atom, builder_.getInt64(1), builder_.getInt32(sg),
                 builder_.getInt32(cs), getOveVtable()},
                "atom.incr");
            emitPendingCheck();  // OverflowException from bz_atomic_add
            return truncResult(res, "atom.incr.v");
        }
        if (mname == "Decrement" && call.args.empty()) {
            llvm::Value* res = builder_.CreateCall(
                addFn,
                {atom, builder_.getInt64(-1LL), builder_.getInt32(sg),
                 builder_.getInt32(cs), getOveVtable()},
                "atom.decr");
            emitPendingCheck();
            return truncResult(res, "atom.decr.v");
        }
        if (mname == "Add" && !call.args.empty() && call.args[0].value) {
            llvm::Value* deltaRaw = lowerExpr(*call.args[0].value);
            if (!deltaRaw) return nullptr;
            // #2605 / §7.4: unwrap a flow-narrowed `T?` delta to its bare
            // payload before widening — see Exchange above.
            deltaRaw = unwrapNarrowedNullable(deltaRaw, getTTy());
            // Sign-extend to i64 for signed T; zero-extend for unsigned T.
            llvm::Value* delta64 = deltaRaw;
            if (deltaRaw->getType()->isIntegerTy() &&
                !deltaRaw->getType()->isIntegerTy(64)) {
                delta64 = sg ? builder_.CreateSExt(deltaRaw, i64(), "add.d64")
                             : builder_.CreateZExt(deltaRaw, i64(), "add.d64");
            }
            llvm::Value* res = builder_.CreateCall(
                addFn,
                {atom, delta64, builder_.getInt32(sg),
                 builder_.getInt32(cs), getOveVtable()},
                "atom.add");
            emitPendingCheck();
            return truncResult(res, "atom.add.v");
        }
    }

    return nullptr;
}


// §6.6 / issue #974: TaskCompletionSource<T> / OwningTaskCompletionSource<T> /
// TaskCompletionSource (non-generic) instance method lowering.
//
// Handled methods:
//   TrySetResult(result: T): bool  → bz_tcs_try_set_result(tcs, &val, sizeof(T)) → i1
//   SetResult(result: T)           → same + emitGuardThrow(IOE) if already done
//   TrySetException(exc): bool     → bz_tcs_try_set_exception(tcs, exc_obj)      → i1
//   SetException(exc)              → same + emitGuardThrow(IOE) if already done
//   TrySetCancelled(): bool        → bz_tcs_try_set_cancelled(tcs, oce_vtable)   → i1
//   SetCancelled()                 → same + emitGuardThrow(IOE) if already done
//   Dispose()                      → no-op here; real work done by emitDisposals
//
// Detection: receiver builtin type name "TaskCompletionSource" or
// "OwningTaskCompletionSource" (both map to a bz_tcs_t* opaque pointer).
llvm::Value* Emitter::tryLowerTcsCall(const ast::CallExpr& call) {
    if (!call.callee) return nullptr;
    auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(call.callee.get());
    if (!ma || !ma->target) return nullptr;

    const std::string_view mname = ma->member;
    const sema::TypeRef* recvTy = ctx_.exprType(*ma->target);
    std::string_view rcn = builtinTypeName(recvTy);
    if (rcn != "TaskCompletionSource" && rcn != "OwningTaskCompletionSource")
        return nullptr;

    llvm::Value* tcs = lowerExpr(*ma->target);
    if (!tcs) return nullptr;

    // Helper: get InvalidOperationException vtable for Set* guard throws.
    auto getIoeVtable = [&]() -> llvm::Value* {
        auto vit = builtinVtable_.find("InvalidOperationException");
        if (vit != builtinVtable_.end())
            return builder_.CreateBitCast(vit->second, ptrTy());
        return llvm::ConstantPointerNull::get(ptrTy());
    };
    (void)getIoeVtable;  // used only in SetCancelled via emitGuardThrow

    // Helper: get OperationCanceledException vtable for TrySetCancelled.
    auto getOceVtable = [&]() -> llvm::Value* {
        auto vit = builtinVtable_.find("OperationCanceledException");
        if (vit != builtinVtable_.end())
            return builder_.CreateBitCast(vit->second, ptrTy());
        return llvm::ConstantPointerNull::get(ptrTy());
    };

    // TrySetResult / SetResult — pass the value through a stack slot.
    // Pattern mirrors Task.FromResult (LlvmBackend_Calls.cpp §6.7 arm above).
    // For OwningTaskCompletionSource<T>, T is a class pointer — same layout as
    // any other pointer-width value; ownership transfer is the caller's concern.
    if ((mname == "TrySetResult" || mname == "SetResult") &&
        !call.args.empty() && call.args[0].value) {
        llvm::Value* val = lowerExpr(*call.args[0].value);
        if (!val) return nullptr;
        llvm::Type* valTy = val->getType();
        llvm::AllocaInst* slot = builder_.CreateAlloca(valTy, nullptr,
                                                        "tcs.setres.slot");
        builder_.CreateStore(val, slot);
        int64_t sz = static_cast<int64_t>(
            module_->getDataLayout().getTypeAllocSize(valTy));
        auto* tryFn = declareC("bz_tcs_try_set_result", i32(),
                                {ptrTy(), ptrTy(), i64()});
        llvm::Value* ok32 = builder_.CreateCall(tryFn,
            {tcs, builder_.CreateBitCast(slot, ptrTy()), builder_.getInt64(sz)},
            "tcs.trysetres");
        llvm::Value* ok1 = builder_.CreateICmpNE(
            ok32, llvm::ConstantInt::get(i32(), 0), "tcs.trysetres.b");
        if (mname == "SetResult") {
            // SetResult: throw InvalidOperationException if already completed.
            llvm::Value* fail = builder_.CreateNot(ok1, "tcs.setres.fail");
            emitGuardThrow(fail, "InvalidOperationException");
            return nullptr;  // unit return
        }
        return ok1;
    }

    // TrySetException / SetException — pass the exception object directly.
    if ((mname == "TrySetException" || mname == "SetException") &&
        !call.args.empty() && call.args[0].value) {
        llvm::Value* exc = lowerExpr(*call.args[0].value);
        if (!exc) return nullptr;
        auto* tryFn = declareC("bz_tcs_try_set_exception", i32(),
                                {ptrTy(), ptrTy()});
        llvm::Value* ok32 = builder_.CreateCall(tryFn, {tcs, exc},
                                                 "tcs.trysetexc");
        llvm::Value* ok1 = builder_.CreateICmpNE(
            ok32, llvm::ConstantInt::get(i32(), 0), "tcs.trysetexc.b");
        if (mname == "SetException") {
            llvm::Value* fail = builder_.CreateNot(ok1, "tcs.setexc.fail");
            emitGuardThrow(fail, "InvalidOperationException");
            return nullptr;
        }
        return ok1;
    }

    // TrySetCancelled / SetCancelled — build OCE from vtable in the runtime.
    if (mname == "TrySetCancelled" || mname == "SetCancelled") {
        auto* tryFn = declareC("bz_tcs_try_set_cancelled", i32(),
                                {ptrTy(), ptrTy()});
        llvm::Value* ok32 = builder_.CreateCall(tryFn, {tcs, getOceVtable()},
                                                 "tcs.trysetcxl");
        llvm::Value* ok1 = builder_.CreateICmpNE(
            ok32, llvm::ConstantInt::get(i32(), 0), "tcs.trysetcxl.b");
        if (mname == "SetCancelled") {
            llvm::Value* fail = builder_.CreateNot(ok1, "tcs.setcxl.fail");
            emitGuardThrow(fail, "InvalidOperationException");
            return nullptr;
        }
        return ok1;
    }

    // Dispose() — real work is done by the using-var emitDisposals path.
    // An explicit tcs.Dispose() call in user code is a no-op here; the
    // disposal contract (bz_tcs_fault_if_pending + inner task fire-and-forget +
    // bz_tcs_free) is enforced by the Disposal.tcs emitDisposals arm.
    if (mname == "Dispose") {
        return nullptr;
    }

    return nullptr;
}


// §15.1.2 / §17.5 / #1342: lower an inline-array `[N]T` built-in intrinsic:
//   ToArray()    — copies all N slots into a fresh bz_array_t* (owned Array<T>).
//                  String elements are +1 retained (bz_array_retain_strings).
//   Contains(v)  — linear scan over the N slots; returns i1.  Element equality
//                  uses emitRemoveElementEquality (§9.2: value-eq for values,
//                  content-eq for strings, identity/Equals for classes, structural
//                  for records).
//   ToString()   — (§8.5 v0.182, #1579) the compiler-generated default:
//                  "[N]T"-style, the type's name — a compile-time constant.
//
// The receiver is an InlineArray-typed expression that lowers to an LLVM
// `[N x elemTy]` value (usually an lvalue, but we spill to a temp alloca when
// it is a computed expression).  The same pattern is used by lowerArrayFor.
llvm::Value* Emitter::tryLowerInlineArrayCall(const ast::CallExpr& call) {
    if (!call.callee) return nullptr;
    auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(call.callee.get());
    if (!ma || !ma->target) return nullptr;

    const std::string_view m = ma->member;
    // #3091 (§15.1.2 v0.249): an inline-array `a.Equals(b)` and
    // `a.GetHashCode()` must lower through §15.1.2's structural equality and
    // hash-combining rules, exactly as the `=` operator already does at
    // LlvmBackend_Exprs.cpp ~L8520. Before this fix the methods fell through to
    // a generic dispatch that returned a constant 0 / false (no inline-array
    // member existed), so two equal inline arrays compared unequal and the
    // hash was unconditionally 0 — the structural-equality / hashable
    // contract of §3.6 (auto-hashable), §8.3 and §15.1.2 was silently broken.
    if (m != "ToArray" && m != "Contains" && m != "ToString" &&
        m != "Equals" && m != "GetHashCode") return nullptr;

    const sema::TypeRef* recvTy = ctx_.exprType(*ma->target);
    if (!recvTy || recvTy->kind != sema::TypeRef::Kind::InlineArray) return nullptr;

    sema::TypeRef* elemRef = recvTy->arrayElement;
    int64_t N = recvTy->arraySizeValue;

    // §8.5 (v0.182, #1579, v0.240 move-only form): the compiler-generated
    // default `ToString` for an inline array is the type's name, "[3]int"-
    // style — deliberately NOT an element listing (elements may be class-typed
    // borrows whose formatting is not the container's business). The "all
    // forms" wording in §8.5 is explicit, and §15.1.1/§15.3 make `owned` part
    // of the type — so an owning `[2 owned]R` is a DIFFERENT type from `[2]R`
    // and must render DIFFERENTLY: `[2 owned]R`, not `[2]R`. N and the element
    // type are both static, so the whole result is a compile-time string
    // constant; the receiver's runtime VALUE is never read (its EXPRESSION is
    // still evaluated — see fix #3445 in the arm itself). Handled here (rather
    // than the general array-address path below) because the ANSWER needs
    // neither `arrAddr` nor `elemTy`/`esz`.
    //
    // #2959 (§8.5 v0.240): the direct-call arm used to build the name by hand
    // (`"[" + N + "]" + <element name>`) and so silently dropped the `owned`
    // marker, while the interpolation arm and the box thunk both already routed
    // through `inlineArrayTypeSpelling` (which keeps it). One §8.5 answer, three
    // renderers, disagreed on one cell. Delegate here too — same spelling,
    // single authority.
    if (m == "ToString" && call.args.empty()) {
        // fix #3445 (§7.5 / §22.3 / §22.5 / §8.2, v0.283): "the receiver's
        // runtime VALUE is never read" is NOT "the receiver EXPRESSION needs no
        // evaluating". The comment above conflated the two and this arm acted on
        // the conflation: it answered §8.5 from the static type and returned
        // WITHOUT ever handing `ma->target` to `lowerExpr`, so
        // `C.MakeArray().ToString()` deleted the call from the program — the
        // `CreateCall` was never emitted, `"[3]int"` was still exactly right, and
        // the whole thing was silent (measured on 0c5ecd1: the issue's repro
        // printed `A`, `[3]int`, `B` and never the receiver's own
        // `Console.WriteLine("side-arr")`, exit 0, no diagnostic).
        //
        // This is #3431's defect one type over. Those two are the WHOLE blast
        // radius: measured with a side-effecting producer, only the two §8.5
        // arms whose answer is a compile-time constant (`()` and `[N]T`) skipped
        // it; a record, an `int` and an `owned Array<int>` receiver all ran
        // their producer, because every arm that has to READ the receiver lowers
        // it as a side effect of needing it. Within THIS function `ToArray`,
        // `Contains`, `Equals` and `GetHashCode` all fall through to the
        // `arrAddr` computation below and so were never affected.
        //
        // A receiver is an evaluated operand, and none of the rules that say so
        // carves out a receiver whose §8.5 answer is static:
        //   * §7.5 "Evaluation": "`?.` short-circuits. The receiver is evaluated
        //     first; if it is null the access does not occur and **nothing to
        //     its right in the chain is evaluated**" — a rule that a NULL receiver
        //     SUPPRESSES the rest presupposes a non-null one's expression RUNS,
        //     and `.` has no short-circuit at all.
        //   * §22.3, the `ref`/`out` argument-locations passage: "the receiver
        //     and every index expression are evaluated exactly once, in §22.5's
        //     call-site textual order". Exactly once — not zero times.
        //   * §22.5 "Evaluation order": "The order is observable through
        //     side-effecting arguments and non-constant defaults … and is
        //     normative …" (the sentence continues "; it matches C#'s
        //     treatment of supplied arguments" — truncated, not ended).
        //   * §8.2 is the closest thing to a licence for answering a universal
        //     from the static type, and is explicit about what it removes: "the
        //     call is answered from the static type with no observable boxing".
        //     It removes the BOX, not the receiver.
        //
        // So: lower the receiver for its effects, EXACTLY ONCE, and then answer
        // §8.5's constant. `lowerExpr` (rather than the `lvalueAddr`-first
        // sequence the `arrAddr` block below uses) is deliberate: that sequence
        // can emit IR and THEN decline — the §12.7 arm and the `a[i]` arm both
        // do, see lvalueAddr's own ⚠ note — so falling back from it would risk
        // evaluating a side-effecting sub-expression TWICE, which for this arm
        // would be a worse defect than the one being fixed. One `lowerExpr` is
        // one evaluation. Its result is discarded rather than tested because a
        // null here must not resurrect the drop: §8.5's answer does not depend
        // on the value, so a receiver this backend cannot value-lower still gets
        // its correct rendering.
        (void)lowerExpr(*ma->target);  // per §22.3 / §7.5 / §22.5
        // lowerStringLiteral expects Bezel string-literal SOURCE TEXT
        // (including the surrounding quotes); a type name is always a plain
        // identifier sequence (plus "[", "]", "?"), none of which need
        // escaping for a non-verbatim literal.
        return lowerStringLiteral(
            "\"" + inlineArrayTypeSpelling(recvTy) + "\"", /*verbatim=*/false);
    }

    llvm::Type* elemTy = elemRef ? lowerTypeRef(elemRef) : i64();
    if (!elemTy) elemTy = i64();

    // Compute element byte size (mirrors collectionElemSize logic).
    auto elemSzOf = [&](llvm::Type* et) -> int64_t {
        if (!et) return 8;
        if (et->isIntegerTy(32) || et->isFloatTy()) return 4;
        if (et->isIntegerTy(1) || et->isIntegerTy(8)) return 1;
        if (et->isIntegerTy(16)) return 2;
        if (et->isStructTy())
            return (int64_t)module_->getDataLayout().getTypeAllocSize(et);
        return 8;  // ptr, i64, double
    };
    int64_t esz = elemSzOf(elemTy);

    // Get the receiver array address: try lvalue first, spill if needed.
    // Same pattern as lowerArrayFor (§15.1.2 iteration, LlvmBackend_Stmts.cpp).
    llvm::Type* arrSlotTy = nullptr;
    llvm::Value* arrAddr = lvalueAddr(*ma->target, arrSlotTy);
    if (!arrAddr || !arrSlotTy) {
        llvm::Value* v = lowerExpr(*ma->target);
        if (!v) return nullptr;
        auto* at = llvm::dyn_cast<llvm::ArrayType>(v->getType());
        if (!at) return nullptr;
        llvm::AllocaInst* tmp = entryAlloca(at);
        builder_.CreateStore(v, tmp);
        arrAddr = tmp;
        arrSlotTy = at;
    }
    auto* arrTy = llvm::dyn_cast<llvm::ArrayType>(arrSlotTy);
    if (!arrTy) return nullptr;

    // -----------------------------------------------------------------------
    // ToArray(): allocate a bz_array_t, copy N elements, retain strings.
    // -----------------------------------------------------------------------
    if (m == "ToArray" && call.args.empty()) {
        auto* allocFn = declareC("bz_array_alloc", ptrTy(), {i64(), i64()});
        llvm::Value* result = builder_.CreateCall(
            allocFn,
            {builder_.getInt64(esz), builder_.getInt64(N > 0 ? N : 0)},
            "ia.toarr");

        if (N > 0) {
            auto* addFn = declareC("bz_array_add",
                                   llvm::Type::getVoidTy(llvm_),
                                   {ptrTy(), ptrTy(), i64()});
            // Emit a counted loop: for i in 0..N-1: arr.add(&src[i], esz).
            auto* idxSlot = entryAlloca(i32());
            builder_.CreateStore(builder_.getInt32(0), idxSlot);
            auto* condBB = llvm::BasicBlock::Create(llvm_, "iat.cond", currentFn_);
            auto* bodyBB = llvm::BasicBlock::Create(llvm_, "iat.body", currentFn_);
            auto* endBB  = llvm::BasicBlock::Create(llvm_, "iat.end",  currentFn_);
            builder_.CreateBr(condBB);

            builder_.SetInsertPoint(condBB);
            llvm::Value* idx = builder_.CreateLoad(i32(), idxSlot);
            builder_.CreateCondBr(
                builder_.CreateICmpSLT(idx,
                    builder_.getInt32(static_cast<uint32_t>(N))),
                bodyBB, endBB);

            builder_.SetInsertPoint(bodyBB);
            llvm::Value* cur = builder_.CreateLoad(i32(), idxSlot);
            llvm::Value* ep = builder_.CreateGEP(
                arrTy, arrAddr, {builder_.getInt64(0), cur});
            builder_.CreateCall(addFn, {result, ep, builder_.getInt64(esz)});
            builder_.CreateStore(
                builder_.CreateAdd(cur, builder_.getInt32(1)), idxSlot);
            builder_.CreateBr(condBB);

            builder_.SetInsertPoint(endBB);

            // §15.1.2 / #1342: string elements — after filling, retain each
            // element +1 so the Array<string> co-owns a refcount per slot.
            // Mirrors the LINQ ToArray string path (tryLowerLazyLinqCall).
            if (elemRef &&
                elemRef->kind == sema::TypeRef::Kind::Primitive &&
                elemRef->primitive == sema::PrimitiveType::String &&
                !elemRef->nullable) {
                auto* rf = declareC("bz_array_retain_strings",
                                    llvm::Type::getVoidTy(llvm_), {ptrTy()});
                builder_.CreateCall(rf, {result});
            }
        }
        return result;
    }

    // -----------------------------------------------------------------------
    // Contains(v): linear scan, element-type equality per §9.2.
    // -----------------------------------------------------------------------
    if (m == "Contains" && call.args.size() == 1 && call.args[0].value) {
        llvm::Value* target = lowerExpr(*call.args[0].value);
        if (!target) return nullptr;

        // Empty array always returns false.
        if (N == 0) return builder_.getFalse();

        bool elemIsClass = elemRef &&
            elemRef->kind == sema::TypeRef::Kind::Named &&
            elemRef->declaration &&
            (elemRef->declaration->kind == ast::TypeDeclKind::Class ||
             elemRef->declaration->kind == ast::TypeDeclKind::Interface);

        auto* idxSlot = entryAlloca(i32());
        builder_.CreateStore(builder_.getInt32(0), idxSlot);
        auto* condBB  = llvm::BasicBlock::Create(llvm_, "iac.cond",  currentFn_);
        auto* bodyBB  = llvm::BasicBlock::Create(llvm_, "iac.body",  currentFn_);
        auto* foundBB = llvm::BasicBlock::Create(llvm_, "iac.found", currentFn_);
        auto* nextBB  = llvm::BasicBlock::Create(llvm_, "iac.next",  currentFn_);
        auto* endBB   = llvm::BasicBlock::Create(llvm_, "iac.end",   currentFn_);
        builder_.CreateBr(condBB);

        builder_.SetInsertPoint(condBB);
        llvm::Value* idx = builder_.CreateLoad(i32(), idxSlot);
        builder_.CreateCondBr(
            builder_.CreateICmpSLT(idx,
                builder_.getInt32(static_cast<uint32_t>(N))),
            bodyBB, endBB);

        builder_.SetInsertPoint(bodyBB);
        llvm::Value* cur = builder_.CreateLoad(i32(), idxSlot);
        llvm::Value* ep = builder_.CreateGEP(
            arrTy, arrAddr, {builder_.getInt64(0), cur});
        llvm::Value* el = builder_.CreateLoad(elemTy, ep);
        llvm::Value* eq = emitRemoveElementEquality(el, target, elemRef, elemIsClass);
        if (!eq) return nullptr;
        builder_.CreateCondBr(eq, foundBB, nextBB);

        builder_.SetInsertPoint(foundBB);
        builder_.CreateBr(endBB);

        builder_.SetInsertPoint(nextBB);
        llvm::Value* inc = builder_.CreateAdd(cur, builder_.getInt32(1));
        builder_.CreateStore(inc, idxSlot);
        builder_.CreateBr(condBB);

        builder_.SetInsertPoint(endBB);
        auto* phi = builder_.CreatePHI(i1(), 2, "iac.result");
        phi->addIncoming(builder_.getTrue(),  foundBB);
        phi->addIncoming(builder_.getFalse(), condBB);
        return phi;
    }

    // -----------------------------------------------------------------------
    // Equals(other): structural equality on the whole array (§15.1.2 v0.249,
    // #3091). The argument must be the same inline-array type; sema enforces
    // the constraint, here we assume it. Two arrays are equal iff every
    // element-pair compares equal under the element type's equality (§8.3,
    // §9.2). Reuses emitInlineArrayEquality — the same helper the `=` operator
    // dispatches to (LlvmBackend_Exprs.cpp ~L8520) — so the two paths cannot
    // diverge on a corner case (e.g. a nested inline array, a string slot).
    // Both sides need to be the loaded ArrayType VALUE (CreateExtractValue,
    // which emitInlineArrayEquality uses to read slots, requires the SSA value,
    // not a pointer).
    // -----------------------------------------------------------------------
    if (m == "Equals" && call.args.size() == 1 && call.args[0].value) {
        // Receiver: already lowered above as arrAddr (lvalue) or spilled to
        // an alloca (rvalue) — same path as ToArray/Contains above. Load the
        // value from the address.
        llvm::Value* lhsVal = builder_.CreateLoad(arrTy, arrAddr, "iae.l");

        // Argument: same lowering shape — lvalue first, spill to alloca if
        // the call site is a computed expression (e.g. method return).
        llvm::Value* otherRaw = lowerExpr(*call.args[0].value);
        if (!otherRaw) return nullptr;
        llvm::Value* rhsVal = nullptr;
        if (otherRaw->getType()->isArrayTy() &&
            otherRaw->getType() == arrTy) {
            rhsVal = otherRaw;  // already an ArrayType value
        } else if (otherRaw->getType()->isPointerTy()) {
            // Pointer-shaped (lvalue / spilled temporary): load.
            rhsVal = builder_.CreateLoad(arrTy, otherRaw, "iae.r");
        } else {
            return nullptr;
        }
        // §15.1.2: equality by element, in slot order, no special cases.
        // Returns i1 (true = equal). The pre-fix path returned a constant
        // false (no member recognised at all), which is what made two
        // equal inline arrays compare unequal.
        return emitInlineArrayEquality(lhsVal, rhsVal, arrTy, elemRef);
    }

    // -----------------------------------------------------------------------
    // GetHashCode(): combine slot hashes with the §9.1 record-style
    // combiner (h = h*31 + slotHash), seeded with 17 — the same recipe
    // emitValueHashCode uses for a field-typed inline array
    // (LlvmBackend_String.cpp ~L3925), so a record containing an inline
    // array field hashes identically with the field extracted and hashed
    // in isolation. The pre-fix path returned a constant 0.
    // -----------------------------------------------------------------------
    if (m == "GetHashCode" && call.args.empty()) {
        // Empty inline arrays are not a valid type (§15.1 — `[0]T` is a
        // compile error), so N >= 1 always. Seed = 17 (non-zero per §8.3
        // hashable contract), then h = h*31 + elemHash for each slot, the
        // same recipe emitValueHashCode uses for a field-typed inline
        // array (LlvmBackend_String.cpp ~L3925) and the record-hash combiner
        // (§9.1/§9.2) — so a record containing an inline-array field
        // hashes identically with the field extracted and hashed in
        // isolation, and the `a.Equals(b)` ↔ `a.GetHashCode() = b.GetHashCode()`
        // invariant holds (equal arrays hash equal).
        llvm::Value* lhsVal = builder_.CreateLoad(arrTy, arrAddr, "iah.l");
        llvm::Value* h = builder_.getInt32(17);
        for (unsigned i = 0; i < arrTy->getNumElements(); ++i) {
            llvm::Value* ei = builder_.CreateExtractValue(lhsVal, {i}, "iah.e");
            llvm::Value* eh = emitValueHashCode(ei, elemRef);
            h = builder_.CreateAdd(
                builder_.CreateMul(h, builder_.getInt32(31)), eh, "iah.fold");
        }
        return h;
    }

    return nullptr;
}


}  // namespace bezel::codegen
