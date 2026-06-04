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
        // §8.1: Object is the universal reference root — every reference type
        // reaches it via its heap representation, so an Object-typed value is a
        // heap pointer (like string above), not a scalar. Lowering it to a
        // pointer is what lets a type test (§12.7/§17.4) on an Object/Object?
        // operand load its vtable and compare descriptors; without this the
        // operand was an i32 and the type test's ICmp against a null pointer
        // aborted the compiler (#399).
        case P::Object:                return ptrTy();
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
        // §27.5: a captured ("boxed") local's address is its shared cell.
        return boxedStorage(slot, elemTy);
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


// per §10 / issue #364: a NULLABLE ENUM (a nullable Named TypeRef whose
// declaration is an enum) is a value type that lacked a null representation —
// a non-nullable enum lowers to its underlying integer (i32), so `enum?` stayed
// i32 and `null` collided with the value-0 member. It now lowers, like the
// value primitives, to the tagged {i8 hasValue; i32 value} struct, so null is
// representable and distinct from every enum value.
bool Emitter::isNullableEnumTy(const sema::TypeRef* rt) {
    return rt && rt->nullable &&
           rt->kind == sema::TypeRef::Kind::Named && rt->declaration &&
           rt->declaration->kind == ast::TypeDeclKind::Enum;
}


// per §8.3/§9.1: a nullable reference type lowers to a plain `ptr` whose null
// is a real null pointer (no hasValue tag). The synthesized value-semantics
// members must null-guard a field of such a type before dereferencing it.
bool Emitter::isNullableRefTy(const sema::TypeRef* rt) {
    if (!rt || !rt->nullable) return false;
    using K = sema::TypeRef::Kind;
    using P = sema::PrimitiveType;
    // string? and Object? are reference primitives that lower to ptr.
    if (rt->kind == K::Primitive)
        return rt->primitive == P::String || rt->primitive == P::Object;
    // Nullable class/interface field (a named class or interface type).
    if (rt->kind == K::Named && rt->declaration)
        return rt->declaration->kind == ast::TypeDeclKind::Class ||
               rt->declaration->kind == ast::TypeDeclKind::Interface;
    return false;
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
    // CG-nullable: nullable value type (prim or enum) → {i8 hasValue; T value}.
    // For an enum the value slot is its underlying integer (i32 — CG44); per
    // issue #364 this gives `enum?` a null distinct from the value-0 member.
    if (isNullableValueTy(rt)) return nullableValueStructTy(rt);
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
            // §32.5 / §32.7 / stdlib §10: FFI-boundary types. IntPtr and
            // UIntPtr are pointer-sized integers — on the host target that is
            // 64-bit, so they lower to i64. CString/WString/RawBytes are
            // opaque pointers at the C boundary (char*, wchar*, void*).
            if (!rt->declaration && rt->astNode && rt->astNode->name &&
                !rt->astNode->name->parts.empty()) {
                auto fn = rt->astNode->name->parts[0];
                if (fn == "IntPtr" || fn == "UIntPtr")
                    return i64();  // per §32.7: pointer-sized integer (i64 host)
                if (fn == "CString" || fn == "WString" || fn == "RawBytes")
                    return ptrTy();  // per §32.5: opaque C-boundary pointer
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
                    n == "Task" || n == "OwnedTask" ||
                    // per §2.11 / §24.4: a Subscription is a runtime-managed
                    // opaque handle (bz_subscription_t*), returned by
                    // `<event>.Subscribe(...)` / CancellationToken.Register and
                    // disposed via bz_subscription_dispose. Without this it fell
                    // through to the i32 default, truncating the pointer when a
                    // Subscription was a method return / parameter / field — so
                    // its later `.Dispose()` saw a non-pointer and was skipped.
                    n == "Subscription")
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
    // per §24.1: an event's backing EventSource<T> field is a runtime object
    // referenced by an opaque pointer.
    if (dynamic_cast<const sema::EventSymbol*>(m))
        return ptrTy();
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
        specFieldTys.push_back(ptrTy());            // [0] vtable pointer
        specFieldTys.push_back(disposedFlagTy());   // [1] disposed run-once flag
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
        // Register field indices for this specialization (data fields start at
        // index 2, after the vtable and the disposed flag).
        unsigned sidx = 2;
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
    // CG17: slot 0 is the hidden vtable pointer.
    fieldTys.push_back(ptrTy());
    // §28.2 / §28.6: slot 1 is the per-object i8 "disposed" run-once flag,
    // calloc-zeroed to 0 = not-disposed. It sits BEFORE all data fields so it is
    // at a FIXED offset (right after the vtable pointer) for every class — the
    // same in a base, every derived class (prefix layout preserved: inherited
    // data fields keep their index 2..), and every generic specialization
    // (independent of field type sizes). The shared, type-erased generated
    // Dispose (one per class template, used by all instantiations) therefore
    // reads/writes the flag at the correct offset regardless of substitution.
    fieldTys.push_back(disposedFlagTy());
    unsigned idx = 2;
    for (const sema::Symbol* m : storage) {
        fieldTys.push_back(storageType(m));
        fieldIndex_[m] = idx++;
    }
    st->setBody(fieldTys);
    return st;
}


// §28.2 / §28.6: the LLVM type of the per-object "disposed" run-once flag — a
// single byte right after the vtable pointer in every class struct. calloc
// zeroes it (0 = not yet disposed); the generated Dispose sets it to 1 on first
// disposal.
llvm::Type* Emitter::disposedFlagTy() { return llvm::Type::getInt8Ty(llvm_); }


// §28.2 / §28.6: the struct index of every class's disposed flag — fixed at 1,
// right after the vtable pointer (index 0) and before all data fields. Uniform
// across the hierarchy and across generic specializations, so the shared
// generated Dispose accesses it correctly on any instance.
unsigned Emitter::disposedFlagIndex(const sema::TypeSymbol* /*t*/) {
    return 1u;
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


// §27.5: route a local/param access through its shared heap cell when the
// variable has been boxed for capture; otherwise use the alloca directly.
llvm::Value* Emitter::boxedStorage(llvm::AllocaInst* slot, llvm::Type*& elemTy) {
    auto it = cellVars_.find(slot);
    if (it != cellVars_.end()) {
        elemTy = it->second;
        // The alloca holds a pointer to the heap cell; the value lives there.
        return builder_.CreateLoad(ptrTy(), slot, "cell");
    }
    elemTy = slot->getAllocatedType();
    return slot;
}


// §27.5: promote a local/param slot to a shared heap cell so that the
// enclosing function and any capturing closure access the same storage.
// Idempotent: a slot already boxed is reused (multiple lambdas capturing the
// same variable share one cell). Returns the cell pointer and, through
// `newSlot`, the `ptr`-typed alloca that now holds it.
llvm::Value* Emitter::boxLocalSlot(llvm::AllocaInst* slot,
                                   llvm::AllocaInst*& newSlot) {
    auto it = cellVars_.find(slot);
    if (it != cellVars_.end()) {
        // Already boxed: the slot is already a ptr-alloca holding the cell.
        newSlot = slot;
        return builder_.CreateLoad(ptrTy(), slot, "cell");
    }
    llvm::Type* valTy = slot->getAllocatedType();
    // Allocate a one-value heap cell sized to the value type.
    llvm::Value* cellSize = builder_.CreatePtrToInt(
        builder_.CreateGEP(valTy, llvm::ConstantPointerNull::get(ptrTy()),
                           {builder_.getInt32(1)}),
        i64());
    llvm::Value* cell = builder_.CreateCall(callocFn(),
                                            {builder_.getInt64(1), cellSize});
    // Copy the current value out of the original slot into the cell.
    llvm::Value* cur = builder_.CreateLoad(valTy, slot, "preboxed");
    builder_.CreateStore(cur, cell);
    // A value slot (e.g. `alloca i32`) cannot hold a pointer, so use a fresh
    // `ptr`-typed alloca as the cell holder. The caller rebinds the variable
    // to this slot; cellVars_ marks it boxed.
    newSlot = entryAlloca(ptrTy());
    builder_.CreateStore(cell, newSlot);
    cellVars_[newSlot] = valTy;
    return cell;
}


// §27.5: rebind a boxed `using var`'s pending disposal to its new slot.
void Emitter::rebindDisposalSlot(llvm::AllocaInst* oldSlot,
                                 llvm::AllocaInst* newSlot) {
    if (oldSlot == newSlot) return;
    for (auto& scope : disposeStack_)
        for (auto& d : scope)
            if (d.slot == oldSlot) d.slot = newSlot;
}


namespace {
// §27.5: recursive capture-set walker. Visits every sub-expression and
// sub-statement of a node, recording the symbol of any identifier reference.
// Nested lambdas are descended into so their captures of an outer scope are
// counted too. The walk over-approximates harmlessly (e.g. it records a
// lambda's own parameters and inner locals as well); the caller intersects
// the result against the enclosing function's known locals/params.
struct CaptureWalker {
    const sema::SemaContext& ctx;
    std::unordered_set<const sema::Symbol*>& out;

    void expr(const ast::Expr* e) {
        if (!e) return;
        if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(e)) {
            if (sema::Symbol* s = ctx.exprSymbol(*id)) out.insert(s);
            return;
        }
        if (auto* m = dynamic_cast<const ast::MemberAccessExpr*>(e)) {
            expr(m->target.get()); return; }
        if (auto* m = dynamic_cast<const ast::NullCondMemberExpr*>(e)) {
            expr(m->target.get()); return; }
        if (auto* c = dynamic_cast<const ast::CallExpr*>(e)) {
            expr(c->callee.get());
            for (auto& a : c->args) expr(a.value.get());
            return; }
        if (auto* c = dynamic_cast<const ast::NullCondCallExpr*>(e)) {
            expr(c->callee.get());
            for (auto& a : c->args) expr(a.value.get());
            return; }
        if (auto* ix = dynamic_cast<const ast::IndexExpr*>(e)) {
            expr(ix->target.get());
            for (auto& a : ix->indices) expr(a.value.get());
            return; }
        if (auto* b = dynamic_cast<const ast::BinaryExpr*>(e)) {
            expr(b->left.get()); expr(b->right.get()); return; }
        if (auto* u = dynamic_cast<const ast::UnaryExpr*>(e)) {
            expr(u->operand.get()); return; }
        if (auto* p = dynamic_cast<const ast::PostfixBangExpr*>(e)) {
            expr(p->operand.get()); return; }
        if (auto* t = dynamic_cast<const ast::TypeTestExpr*>(e)) {
            expr(t->operand.get()); return; }
        if (auto* c = dynamic_cast<const ast::ConditionalExpr*>(e)) {
            expr(c->condition.get()); expr(c->thenValue.get());
            expr(c->elseValue.get()); return; }
        if (auto* t = dynamic_cast<const ast::ThrowExpr*>(e)) {
            expr(t->operand.get()); return; }
        if (auto* r = dynamic_cast<const ast::RangeExpr*>(e)) {
            expr(r->lo.get()); expr(r->hi.get()); expr(r->step.get());
            return; }
        if (auto* n = dynamic_cast<const ast::NewExpr*>(e)) {
            for (auto& a : n->args) expr(a.value.get());
            return; }
        if (auto* w = dynamic_cast<const ast::WithExpr*>(e)) {
            expr(w->source.get());
            for (auto& a : w->assigns) expr(a.value.get());
            return; }
        if (auto* cl = dynamic_cast<const ast::CollectionLiteralExpr*>(e)) {
            for (auto& el : cl->elements) expr(el.get());
            for (auto& k : cl->keys) expr(k.get());
            return; }
        if (auto* is = dynamic_cast<const ast::InterpolatedStringExpr*>(e)) {
            for (auto& part : is->parts) expr(part.expr.get());
            return; }
        if (auto* lam = dynamic_cast<const ast::LambdaExpr*>(e)) {
            // Descend into nested lambdas: their references reach the
            // grandparent scope too.
            if (lam->exprBody) expr(lam->exprBody.get());
            for (auto& s : lam->stmts) stmt(s.get());
            return; }
        if (auto* ce = dynamic_cast<const ast::CaseExpr*>(e)) {
            expr(ce->scrutinee.get());
            for (auto& arm : ce->arms) {
                expr(arm.guard.get());
                expr(arm.exprBody.get());
                for (auto& s : arm.stmts) stmt(s.get());
            }
            return; }
        // Literals and other leaf expressions: nothing to do.
    }

    void stmt(const ast::Stmt* s) {
        if (!s) return;
        if (auto* ld = dynamic_cast<const ast::LocalDecl*>(s)) {
            expr(ld->initializer.get()); return; }
        if (auto* lc = dynamic_cast<const ast::LocalConstDecl*>(s)) {
            expr(lc->value.get()); return; }
        if (auto* r = dynamic_cast<const ast::ReturnStmt*>(s)) {
            expr(r->value.get()); return; }
        if (auto* es = dynamic_cast<const ast::ExpressionStmt*>(s)) {
            expr(es->expr.get()); return; }
        if (auto* ts = dynamic_cast<const ast::ThrowStmt*>(s)) {
            expr(ts->value.get()); return; }
        if (auto* as = dynamic_cast<const ast::AssignmentStmt*>(s)) {
            expr(as->target.get()); expr(as->value.get()); return; }
        if (auto* inc = dynamic_cast<const ast::IncrementStmt*>(s)) {
            expr(inc->target.get()); return; }
        if (auto* ds = dynamic_cast<const ast::DiscardStmt*>(s)) {
            expr(ds->value.get()); return; }
        if (auto* ys = dynamic_cast<const ast::YieldStmt*>(s)) {
            expr(ys->value.get()); return; }
        if (auto* is = dynamic_cast<const ast::IfStmt*>(s)) {
            expr(is->ifBranch.condition.get());
            for (auto& b : is->ifBranch.body) stmt(b.get());
            for (auto& br : is->elifBranches) {
                expr(br.condition.get());
                for (auto& b : br.body) stmt(b.get());
            }
            for (auto& b : is->elseBody) stmt(b.get());
            return; }
        if (auto* ws = dynamic_cast<const ast::WhileStmt*>(s)) {
            expr(ws->condition.get());
            for (auto& b : ws->body) stmt(b.get());
            return; }
        if (auto* fs = dynamic_cast<const ast::ForStmt*>(s)) {
            expr(fs->iterable.get()); expr(fs->withExpr.get());
            for (auto& b : fs->body) stmt(b.get());
            return; }
        if (auto* d = dynamic_cast<const ast::DoEndBlock*>(s)) {
            for (auto& b : d->body) stmt(b.get());
            return; }
        if (auto* ts = dynamic_cast<const ast::TryStmt*>(s)) {
            for (auto& b : ts->body) stmt(b.get());
            for (auto& c : ts->catches) {
                expr(c.guard.get());
                stmt(c.body.get());
            }
            for (auto& b : ts->finallyBody) stmt(b.get());
            return; }
        if (auto* cs = dynamic_cast<const ast::CaseStmt*>(s)) {
            expr(cs->scrutinee.get());
            for (auto& arm : cs->arms) {
                expr(arm.guard.get());
                expr(arm.exprBody.get());
                for (auto& a : arm.stmts) stmt(a.get());
            }
            return; }
        // BreakStmt / ContinueStmt: no sub-expressions.
    }
};
}  // namespace


void Emitter::collectCapturedSyms(
        const ast::LambdaExpr& lam,
        std::unordered_set<const sema::Symbol*>& out) {
    CaptureWalker w{ctx_, out};
    if (lam.exprBody) w.expr(lam.exprBody.get());
    for (auto& s : lam.stmts) w.stmt(s.get());
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


// §8.3 / §9.1: compare one field value of declared Bezel type `fieldTy`.
// Strings compare by content (§4.6) — the same content equality the `=`
// operator gives strings, and consistent with the content-based hashing in
// emitValueHashCode; two records with equal string content are equal even when
// the strings are distinct heap objects. Nested records recurse structurally
// (§9.1 transitivity). Primitives and enums compare by value; class references
// compare by identity (reference equality, §8.4). When `fieldTy` is null the
// comparison falls back to the LLVM value shape (string ptr → content, struct →
// recurse, float → FCmp, integer/ptr → ICmp).
llvm::Value* Emitter::emitValueEquality(llvm::Value* l, llvm::Value* r,
                                        const sema::TypeRef* fieldTy) {
    using K = sema::TypeRef::Kind;
    // per §8.3/§9.1: a NULLABLE REFERENCE field (string?, Object?, nullable
    // class/interface) lowers to a plain `ptr` whose null is a real null
    // pointer. Equality is: both-null → equal; exactly-one-null → not-equal;
    // neither-null → the type's normal comparison (content for string,
    // identity for refs). Guard the one-null case BEFORE any loadStringLen /
    // content compare so a null field can't be dereferenced (bug A).
    if (isNullableRefTy(fieldTy) && l->getType()->isPointerTy() &&
        r->getType()->isPointerTy()) {
        auto* nullP = llvm::ConstantPointerNull::get(ptrTy());
        llvm::Value* lNull = builder_.CreateICmpEQ(l, nullP, "eq.lnull");
        llvm::Value* rNull = builder_.CreateICmpEQ(r, nullP, "eq.rnull");
        llvm::Value* bothNull = builder_.CreateAnd(lNull, rNull, "eq.bothnull");
        llvm::Value* eitherNull = builder_.CreateOr(lNull, rNull, "eq.eithernull");
        auto* cmpBB  = llvm::BasicBlock::Create(llvm_, "eq.cmp", currentFn_);
        auto* endBB  = llvm::BasicBlock::Create(llvm_, "eq.end", currentFn_);
        auto* entryBB = builder_.GetInsertBlock();
        // If either is null, skip the content compare; result = bothNull.
        builder_.CreateCondBr(eitherNull, endBB, cmpBB);
        builder_.SetInsertPoint(cmpBB);
        bool isStr = fieldTy->kind == K::Primitive &&
                     fieldTy->primitive == sema::PrimitiveType::String;
        // Both non-null: string → content equality; ref → identity.
        llvm::Value* cmp = isStr ? emitStringEq(l, r)
                                 : builder_.CreateICmpEQ(l, r, "eq.refid");
        auto* cmpEnd = builder_.GetInsertBlock();
        builder_.CreateBr(endBB);
        builder_.SetInsertPoint(endBB);
        auto* phi = builder_.CreatePHI(i1(), 2, "eq.nullable");
        phi->addIncoming(bothNull, entryBB);
        phi->addIncoming(cmp, cmpEnd);
        return phi;
    }
    // per §8.3/§9.1 / issue #364: a NULLABLE VALUE field (int?, bool?, double?,
    // … the value primitives isNullablePrimTy recognizes, AND a nullable enum —
    // isNullableValueTy) lowers to a tagged struct {i8 hasValue; T value}
    // (nullableValueStructTy). The null-safe `=` of §8.4 is: both-null → equal;
    // exactly-one-null → not-equal; neither-null → the inner T's `=`. Drive it
    // off the hasValue tags so the `value` slot is only compared when both are
    // present (an absent value's slot is zero-filled but meaningless).
    if (isNullableValueTy(fieldTy) && l->getType()->isStructTy() &&
        r->getType()->isStructTy()) {
        llvm::Value* lHas = nullablePrimHasValue(l);   // i1
        llvm::Value* rHas = nullablePrimHasValue(r);
        llvm::Value* tagsEq = builder_.CreateICmpEQ(lHas, rHas, "eq.nvtag");
        llvm::Value* bothHave = builder_.CreateAnd(lHas, rHas, "eq.nvboth");
        // Inner T equality of the value slots — primitives/enums compare by
        // value (the fall-through ICmp/FCmp below, reached via a nullable-off
        // recursion so floats use FCmp; an enum's i32 value uses ICmp).
        sema::TypeRef innerTy = *fieldTy;
        innerTy.nullable = false;
        llvm::Value* valEq = emitValueEquality(
            nullablePrimValue(l), nullablePrimValue(r), &innerTy);
        // equal  iff  tags equal AND (both-null OR inner values equal).
        // both-null is (tags equal AND !bothHave); neither-null path needs
        // valEq. Combine: tagsEq AND (NOT bothHave OR valEq).
        llvm::Value* notBoth = builder_.CreateNot(bothHave, "eq.nvnotboth");
        llvm::Value* inner = builder_.CreateOr(notBoth, valEq, "eq.nvinner");
        return builder_.CreateAnd(tagsEq, inner, "eq.nullableval");
    }
    // String field: content equality (§4.6), not pointer identity. per §8.3
    bool isString = fieldTy && fieldTy->kind == K::Primitive &&
                    fieldTy->primitive == sema::PrimitiveType::String;
    if (isString && l->getType()->isPointerTy() && r->getType()->isPointerTy())
        return emitStringEq(l, r);
    // Nested record (§9.1 transitivity): recurse with its own type so that
    // record's string fields are likewise compared by content.
    if (fieldTy && fieldTy->kind == K::Named && fieldTy->declaration &&
        fieldTy->declaration->kind == ast::TypeDeclKind::Record &&
        l->getType()->isStructTy()) {
        if (auto* nested = llvm::dyn_cast<llvm::StructType>(l->getType()))
            return emitStructEquality(l, r, nested, fieldTy->declaration);
    }
    // Fallback by LLVM shape (covers null fieldTy and primitives/enums/refs).
    // Nested structs recurse; floats use FCmp; everything else (integers,
    // and ptr fields when the declared type is unknown) compares by ICmp —
    // identical to the pre-fix behavior. A string field is only reliably
    // identified by its declared type, so when `fieldTy` is absent we do NOT
    // assume ptr-means-string (that would wrongly content-compare class
    // references). Both real call sites pass the record type, so this fallback
    // is purely defensive.
    if (auto* nested = llvm::dyn_cast<llvm::StructType>(l->getType()))
        return emitStructEquality(l, r, nested);
    if (l->getType()->isFloatingPointTy())
        return builder_.CreateFCmpOEQ(l, r);
    return builder_.CreateICmpEQ(l, r);
}

llvm::Value* Emitter::emitStructEquality(llvm::Value* l, llvm::Value* r,
                                llvm::StructType* st,
                                const sema::TypeSymbol* rec) {
    llvm::Value* acc = builder_.getInt1(true);
    // §9.1/§9.2: when the record type is known, drive comparison off each
    // storage member's declared Bezel type (the same order recordStruct lays
    // out, and the same collectRecordStorage / recordMemberType machinery the
    // synthesized GetHashCode uses) so string fields compare by content.
    if (rec) {
        std::vector<const sema::Symbol*> storage;
        collectRecordStorage(rec, storage);
        unsigned i = 0;
        for (const sema::Symbol* m : storage) {
            if (i >= st->getNumElements()) break;
            llvm::Value* li = builder_.CreateExtractValue(l, {i});
            llvm::Value* ri = builder_.CreateExtractValue(r, {i});
            acc = builder_.CreateAnd(
                acc, emitValueEquality(li, ri, recordMemberType(m)));
            ++i;
        }
        return acc;
    }
    // No record type available: compare by LLVM field shape (per-field
    // emitValueEquality with a null declared type — recurses into nested
    // structs, content-compares ptr fields). per §8.3
    for (unsigned i = 0; i < st->getNumElements(); ++i) {
        llvm::Value* li = builder_.CreateExtractValue(l, {i});
        llvm::Value* ri = builder_.CreateExtractValue(r, {i});
        acc = builder_.CreateAnd(acc, emitValueEquality(li, ri, nullptr));
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
            // §27.5: a captured record local lives in its shared cell.
            llvm::Type* ety = nullptr;
            base = boxedStorage(slot, ety);
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
