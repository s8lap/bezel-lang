#include "codegen/LlvmBackend_impl.hpp"

namespace bezel::codegen {

llvm::Value* Emitter::emitConsoleWrite(llvm::Value* strPtr, bool newline,
                               int fd) {
    // Declare POSIX write(2): ssize_t write(int, const void*, size_t)
    auto* writeFn = module_->getFunction("write");
    if (!writeFn) {
        auto* ft = llvm::FunctionType::get(i64(), {i32(), ptrTy(), i64()},
                                           false);
        writeFn = llvm::Function::Create(
            ft, llvm::Function::ExternalLinkage, "write", module_.get());
    }
    llvm::Value* data = loadStringData(strPtr);
    llvm::Value* len  = loadStringLen(strPtr);
    builder_.CreateCall(writeFn,
                        {builder_.getInt32(fd), data, len});
    if (newline) {
        // Write a newline byte.
        llvm::Constant* nlData = llvm::ConstantDataArray::getString(
            llvm_, "\n", /*AddNull=*/false);
        auto* nlGV = new llvm::GlobalVariable(
            *module_, nlData->getType(), true,
            llvm::GlobalValue::PrivateLinkage, nlData, ".bz.nl");
        builder_.CreateCall(writeFn,
                            {builder_.getInt32(fd), nlGV,
                             builder_.getInt64(1)});
    }
    return nullptr;  // unit (void)
}


llvm::Value* Emitter::emitConsoleReadLine() {
    auto* i8Ty = llvm::Type::getInt8Ty(llvm_);
    const unsigned BUFSZ = 4096;
    llvm::Value* buf = entryAlloca(llvm::ArrayType::get(i8Ty, BUFSZ));
    llvm::Value* bufPtr = builder_.CreateGEP(
        llvm::ArrayType::get(i8Ty, BUFSZ), buf,
        {builder_.getInt64(0), builder_.getInt64(0)});
    // fgets(bufPtr, BUFSZ, stdin)
    auto* fgetsFn = module_->getFunction("fgets");
    if (!fgetsFn) {
        auto* ft = llvm::FunctionType::get(ptrTy(), {ptrTy(), i32(), ptrTy()}, false);
        fgetsFn = llvm::Function::Create(
            ft, llvm::Function::ExternalLinkage, "fgets", module_.get());
    }
    // stdin global (FILE*)
    auto* stdinGV = module_->getGlobalVariable("stdin");
    if (!stdinGV) {
        stdinGV = new llvm::GlobalVariable(
            *module_, ptrTy(), false,
            llvm::GlobalValue::ExternalLinkage, nullptr, "stdin");
    }
    llvm::Value* stdinVal = builder_.CreateLoad(ptrTy(), stdinGV, "stdin");
    llvm::Value* fgRet = builder_.CreateCall(
        fgetsFn, {bufPtr, builder_.getInt32(BUFSZ), stdinVal}, "fgets.ret");
    auto* nullPtr = llvm::ConstantPointerNull::get(ptrTy());
    llvm::Value* isEOF = builder_.CreateICmpEQ(fgRet, nullPtr, "rl.eof");
    auto* eofBB = llvm::BasicBlock::Create(llvm_, "rl.eof",  currentFn_);
    auto* okBB  = llvm::BasicBlock::Create(llvm_, "rl.ok",   currentFn_);
    auto* endBB = llvm::BasicBlock::Create(llvm_, "rl.end",  currentFn_);
    builder_.CreateCondBr(isEOF, eofBB, okBB);
    builder_.SetInsertPoint(eofBB);
    builder_.CreateBr(endBB);
    // OK: compute length, strip trailing '\n'.
    builder_.SetInsertPoint(okBB);
    auto* strlenFn = module_->getFunction("strlen");
    if (!strlenFn) {
        auto* ft2 = llvm::FunctionType::get(i64(), {ptrTy()}, false);
        strlenFn = llvm::Function::Create(
            ft2, llvm::Function::ExternalLinkage, "strlen", module_.get());
    }
    llvm::Value* rawLen = builder_.CreateCall(strlenFn, {bufPtr}, "rawlen");
    llvm::Value* hasBytes = builder_.CreateICmpSGT(rawLen, builder_.getInt64(0));
    auto* stripBB = llvm::BasicBlock::Create(llvm_, "rl.strip", currentFn_);
    auto* afterBB = llvm::BasicBlock::Create(llvm_, "rl.after", currentFn_);
    builder_.CreateCondBr(hasBytes, stripBB, afterBB);
    builder_.SetInsertPoint(stripBB);
    llvm::Value* lastIdx = builder_.CreateSub(rawLen, builder_.getInt64(1));
    llvm::Value* lastPtr = builder_.CreateGEP(i8Ty, bufPtr, lastIdx);
    llvm::Value* lastByte = builder_.CreateLoad(i8Ty, lastPtr);
    llvm::Value* isNL = builder_.CreateICmpEQ(lastByte, llvm::ConstantInt::get(i8Ty, '\n'));
    auto* nlBB = llvm::BasicBlock::Create(llvm_, "rl.nl", currentFn_);
    builder_.CreateCondBr(isNL, nlBB, afterBB);
    builder_.SetInsertPoint(nlBB);
    builder_.CreateStore(llvm::ConstantInt::get(i8Ty, 0), lastPtr);
    builder_.CreateBr(afterBB);
    builder_.SetInsertPoint(afterBB);
    // Final length after stripping.
    llvm::Value* finalLen = builder_.CreateCall(strlenFn, {bufPtr}, "finalLen");
    // Heap-copy the data.
    auto* mallocFn = declareC("malloc", ptrTy(), {i64()});
    auto* memcpyFn = declareC("memcpy", ptrTy(), {ptrTy(), ptrTy(), i64()});
    llvm::Value* dataSz = builder_.CreateAdd(finalLen, builder_.getInt64(1));
    llvm::Value* dataCopy = builder_.CreateCall(mallocFn, {dataSz}, "rl.data");
    builder_.CreateCall(memcpyFn, {dataCopy, bufPtr, finalLen});
    llvm::Value* endData = builder_.CreateGEP(i8Ty, dataCopy, finalLen);
    builder_.CreateStore(llvm::ConstantInt::get(i8Ty, 0), endData);
    uint64_t strBytes = module_->getDataLayout().getTypeAllocSize(stringType());
    llvm::Value* strObj = builder_.CreateCall(mallocFn, {builder_.getInt64(strBytes)}, "rl.obj");
    builder_.CreateStore(dataCopy, builder_.CreateStructGEP(stringType(), strObj, 0));
    builder_.CreateStore(finalLen, builder_.CreateStructGEP(stringType(), strObj, 1));
    builder_.CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(llvm_), 1),
        builder_.CreateStructGEP(stringType(), strObj, 2));
        initStringRc(strObj);  // #422: producer's reference, rc := 1
    auto* okEnd = builder_.GetInsertBlock();
    builder_.CreateBr(endBB);
    // Merge.
    builder_.SetInsertPoint(endBB);
    auto* phi = builder_.CreatePHI(ptrTy(), 2, "rl.result");
    phi->addIncoming(nullPtr, eofBB);
    phi->addIncoming(strObj, okEnd);
    return phi;
}


bool Emitter::tryLowerConsoleCall(const ast::CallExpr& call) {
    if (!call.callee) return false;
    auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(call.callee.get());
    if (!ma || !ma->target) return false;

    // Detect Console.Error.Write/WriteLine — stderr output (fd=2).
    int stdfd = 1;  // default: stdout
    auto* id = dynamic_cast<const ast::IdentifierExpr*>(ma->target.get());
    if (!id) {
        // Try Console.Error.Write/WriteLine: target is MemberAccess "Console.Error"
        auto* innerMA = dynamic_cast<const ast::MemberAccessExpr*>(ma->target.get());
        if (!innerMA || !innerMA->target) return false;
        auto* innerID = dynamic_cast<const ast::IdentifierExpr*>(innerMA->target.get());
        if (!innerID || innerID->name != "Console") return false;
        if (innerMA->member == "Error" || innerMA->member == "Out") {
            stdfd = (innerMA->member == "Error") ? 2 : 1;
            id = nullptr;  // mark as Error/Out path
        } else return false;
    } else if (id->name != "Console") return false;

    bool isWriteLine = (ma->member == "WriteLine");
    bool isWrite = (ma->member == "Write");
    if (!isWrite && !isWriteLine) return false;

    // Console.WriteLine() with no args → just print a newline.
    if (call.args.empty() && isWriteLine) {
        auto* writeFn = module_->getFunction("write");
        if (!writeFn) {
            auto* ft = llvm::FunctionType::get(i64(), {i32(), ptrTy(), i64()}, false);
            writeFn = llvm::Function::Create(
                ft, llvm::Function::ExternalLinkage, "write", module_.get());
        }
        llvm::Constant* nlData = llvm::ConstantDataArray::getString(
            llvm_, "\n", false);
        unsigned id2 = stringLiteralIdx_++;
        auto* nlGV = new llvm::GlobalVariable(
            *module_, nlData->getType(), true,
            llvm::GlobalValue::PrivateLinkage, nlData,
            ".bz.console.nl." + std::to_string(id2));
        builder_.CreateCall(writeFn, {builder_.getInt32(stdfd), nlGV, builder_.getInt64(1)});
        return true;
    }
    if (call.args.size() != 1 || !call.args[0].value) return false;

    // Evaluate the argument. If it's a string, use directly; if it's a
    // primitive, convert via emitPrimitiveToString first.
    llvm::Value* arg = lowerExpr(*call.args[0].value);
    if (!arg) return false;
    auto* argTy = ctx_.exprType(*call.args[0].value);
    llvm::Value* strPtr = arg;
    // issue #421: a NUMERIC/char primitive conversion (emitPrimitiveToString's
    // snprintf path) heap-allocates BOTH the bz_string object and its data
    // buffer solely to feed this write; nothing else references it, so it must
    // be freed after the write or it leaks (~26 bytes/call for `WriteLine(7)`).
    // The bool path returns a constant-global string (not heap) and the named-
    // type ToString()/placeholder paths may return a retained/constant string,
    // so only the numeric/char conversion sets this.
    bool freeTransient = false;
    auto isFreeablePrim = [](sema::PrimitiveType p) {
        return p != sema::PrimitiveType::String &&
               p != sema::PrimitiveType::Bool;  // bool → constant globals
    };
    if (argTy) {
        if (argTy->kind == sema::TypeRef::Kind::Primitive &&
            argTy->primitive != sema::PrimitiveType::String) {
            strPtr = emitPrimitiveToString(arg, argTy->primitive);
            freeTransient = isFreeablePrim(argTy->primitive);
        } else if (argTy->kind == sema::TypeRef::Kind::Named &&
                   argTy->declaration) {
            // Named type (class/record) — call ToString() via virtual
            // dispatch if the type has a ToString method, else fall back
            // to a type-name placeholder string.
            auto* ts = argTy->declaration;
            // Find a 'ToString()' method walking the inheritance chain.
            // ToString() may be declared on a base class (e.g. Shape)
            // and inherited by the concrete type (Circle).
            sema::MethodSymbol* toStr = nullptr;
            for (auto* cur = ts; cur && !toStr;) {
                for (auto* sym : cur->members) {
                    auto* ms = dynamic_cast<sema::MethodSymbol*>(sym);
                    if (ms && ms->name == "ToString" && ms->params.empty()) {
                        toStr = ms;
                        break;
                    }
                }
                // Walk to first base class.
                sema::TypeSymbol* next = nullptr;
                for (auto* b : cur->baseTypes)
                    if (b && b->kind == ast::TypeDeclKind::Class) {
                        next = b; break;
                    }
                cur = next;
            }
            if (toStr) {
                // Emit the method call (virtual if overriding, static otherwise).
                std::vector<llvm::Value*> args{arg};
                strPtr = emitMethodCall(toStr, args, false);
            } else {
                // No ToString override — use type name as placeholder string.
                strPtr = lowerStringLiteral(
                    std::string("\"") + std::string(ts->name) + "\"",
                    false);
            }
        }
    }
    // issue #420: fall back on the lowered VALUE's LLVM type when the sema
    // argument type did not drive a string conversion above. Some spec-valid
    // primitive-typed argument expressions are not annotated with a Primitive
    // sema TypeRef — notably the built-in heap-collection `Count`/`Length`
    // property read (stdlib §4.4), which sema leaves untyped but codegen
    // lowers to a runtime bz_array_len/bz_dict_count `int` (`a.Count` in
    // `Console.WriteLine(a.Count)`). Without this, `strPtr` stayed the raw i32
    // and emitConsoleWrite GEP'd it as a `bz_string*`, producing IR the X86
    // backend cannot select ("Cannot emit physreg copy instruction"). Mirror
    // the string-interpolation fallback (emitWithFormatSpec): convert an
    // integer/float value to its decimal string. A pointer-typed value is
    // already a string object and is left as-is.
    if (strPtr == arg && arg->getType() && !arg->getType()->isPointerTy()) {
        if (arg->getType()->isDoubleTy() || arg->getType()->isFloatTy()) {
            strPtr = emitPrimitiveToString(arg, sema::PrimitiveType::Double);
            freeTransient = true;  // numeric snprintf path heap-allocates
        } else if (arg->getType()->isIntegerTy(1)) {
            strPtr = emitPrimitiveToString(arg, sema::PrimitiveType::Bool);
            // bool → constant globals, nothing to free.
        } else if (arg->getType()->isIntegerTy()) {
            // Widen to i64 so the %lld snprintf path receives the full value
            // (matches the interpolation path's sign-extension to i64).
            llvm::Value* iv = arg->getType()->isIntegerTy(64)
                ? arg
                : builder_.CreateSExt(arg, i64());
            strPtr = emitPrimitiveToString(iv, sema::PrimitiveType::Long);
            freeTransient = true;  // numeric snprintf path heap-allocates
        }
    }
    if (!strPtr) return false;
    emitConsoleWrite(strPtr, isWriteLine, stdfd);
    // issue #421: free the write-only transient (its data buffer first, then
    // the bz_string object). Done AFTER the write, which only reads the string.
    if (freeTransient) {
        builder_.CreateCall(freeFn(), {loadStringData(strPtr)});
        builder_.CreateCall(freeFn(), {strPtr});
    }
    return true;
}


int64_t Emitter::collectionElemSize(const sema::TypeRef* collTy) {
    if (!collTy || collTy->genericArgs.empty() || !collTy->genericArgs[0])
        return 8;
    llvm::Type* et = lowerTypeRef(collTy->genericArgs[0]);
    if (!et) return 8;
    if (et->isIntegerTy(32) || et->isFloatTy()) return 4;
    if (et->isIntegerTy(1) || et->isIntegerTy(8)) return 1;
    if (et->isIntegerTy(16)) return 2;
    return 8;  // ptr, i64, double
}


bool Emitter::isCollectionTypeName(const sema::TypeRef* rt) {
    if (!rt || rt->kind != sema::TypeRef::Kind::Named || rt->declaration)
        return false;
    if (!rt->astNode || !rt->astNode->name || rt->astNode->name->parts.empty())
        return false;
    auto n = rt->astNode->name->parts[0];
    return n == "Array" || n == "OwnedArray" ||
           n == "Stack" || n == "OwnedStack" ||
           n == "Queue" || n == "OwnedQueue" ||
           n == "Set" || n == "OwnedSet" ||
           n == "LinkedList" || n == "OwnedLinkedList";
}


bool Emitter::isDictTypeName(const sema::TypeRef* rt) {
    if (!rt || rt->kind != sema::TypeRef::Kind::Named || rt->declaration)
        return false;
    if (!rt->astNode || !rt->astNode->name || rt->astNode->name->parts.empty())
        return false;
    auto n = rt->astNode->name->parts[0];
    return n == "Dictionary" || n == "OwnedDictionary";
}


// issue #401 / stdlib §4.1 / §4.2 / §4.3: the int-indexed list interfaces.
// IReadOnlyList<T> (get-only, §4.1) is the shared read-only view of BOTH the
// borrowing array (Array<T>: IList<T>, §4.4) and the owning array (OwnedArray<T>:
// IOwnedList<T>, §4.5). All three are array-backed at runtime, so an interface-
// typed receiver carries the same bz_array_t* as the concrete collection and the
// indexer get/set lower through the identical bz_array_* runtime. IOwnedList<T>
// is the only owning-set (disposing) one; IReadOnlyList<T> / IList<T> are
// borrowing — IReadOnlyList<T> is read-only so its setter branch never runs.
bool Emitter::isListInterfaceName(const sema::TypeRef* rt, bool* outOwning) {
    if (outOwning) *outOwning = false;
    if (!rt || rt->kind != sema::TypeRef::Kind::Named || rt->declaration)
        return false;
    if (!rt->astNode || !rt->astNode->name || rt->astNode->name->parts.empty())
        return false;
    auto n = rt->astNode->name->parts[0];
    if (n == "IOwnedList") { if (outOwning) *outOwning = true; return true; }
    return n == "IReadOnlyList" || n == "IList";
}


// issue #343 / §4.8: a Dictionary<K,V> key (idx=0) or Set<T> element (idx=0)
// is a "record key" when the generic arg resolves to a record TypeSymbol that
// actually carries synthesized GetHashCode/Equals (positional records, or
// auto-equality-bearing body records — §9.1 / §9.2). For such keys the generic
// runtime variant is used; string and primitive keys keep their fast paths.
// issue #343 / §9.1 / §9.2: if `ref` resolves to a record type that actually
// carries synthesized GetHashCode/Equals (a positional record, or an
// auto-equality-bearing body record), return its TypeSymbol; otherwise null.
const sema::TypeSymbol* Emitter::recordTypeOfRef(const sema::TypeRef* ref) {
    if (!ref || ref->kind != sema::TypeRef::Kind::Named || !ref->declaration)
        return nullptr;
    const sema::TypeSymbol* ts = ref->declaration;
    if (ts->kind != ast::TypeDeclKind::Record) return nullptr;
    bool positional = false;
    if (auto* td = dynamic_cast<const ast::TypeDecl*>(ts->decl))
        positional = td->hasPositionalFields;
    if (positional || sema::isAutoEqualityBearingBodyRecord(ts))
        return ts;
    return nullptr;  // an ineligible body record has no auto equality (§9.2)
}

const sema::TypeSymbol* Emitter::recordKeyType(const sema::TypeRef* collTy,
                                               unsigned idx) {
    if (!collTy || collTy->genericArgs.size() <= idx) return nullptr;
    return recordTypeOfRef(collTy->genericArgs[idx]);
}


// issue #400 / §25.2 / §8.4: a class customizes equality by overriding `Equals`
// (it cannot overload `=`/`<>` — §25.2). Walk the class and its base chain for a
// user-declared single-parameter `Equals` method. `Object.Equals` is a compiler
// intrinsic and has no MethodSymbol in any user `members` list, so a class that
// does not override `Equals` yields null here — and Remove then keeps the §8.3
// reference-identity default. This parallels the ToString-override lookup the
// string-interpolation lowering performs.
sema::MethodSymbol* Emitter::classEqualsOverride(const sema::TypeSymbol* cls) {
    if (!cls || cls->kind != ast::TypeDeclKind::Class) return nullptr;
    for (auto* cur = cls; cur;) {
        for (auto* sym : cur->members) {
            auto* ms = dynamic_cast<sema::MethodSymbol*>(sym);
            if (ms && !ms->isOperator && ms->name == "Equals" &&
                ms->params.size() == 1 && !isStaticMethod(ms))
                return ms;
        }
        const sema::TypeSymbol* nxt = nullptr;
        for (auto* b : cur->baseTypes)
            if (b && b->kind == ast::TypeDeclKind::Class) { nxt = b; break; }
        cur = nxt;
    }
    return nullptr;
}


// issue #400 / §25.2: the per-element comparison `Array<T>`/`OwnedArray<T>`'s
// `.Remove(item)` uses to match `el` (a stored element, borrowed) against
// `target` (the query, a pure borrow — neither operand is consumed). The spec
// requires collections to use the element type's `Equals` (§25.2: "which use
// `Equals` rather than `=`"), defaulting per §8.3:
//   - class WITH a user `Equals` override → virtual `el.Equals(target)` (§8.4),
//     guarded so a null slot or null query falls back to reference identity
//     (matching §8.4's null-safe `=`: null = null is true; null = nonNull false);
//   - class WITHOUT an override → reference identity (§8.3 default);
//   - record → structural field-by-field equality (§8.3, emitStructEquality —
//     consistent with the record's auto `Equals`/`GetHashCode`);
//   - string → content equality (§4.6);
//   - primitive/enum (value type) → `==`.
llvm::Value* Emitter::emitRemoveElementEquality(llvm::Value* el,
                                                llvm::Value* target,
                                                const sema::TypeRef* elemTy,
                                                bool elemIsClass) {
    // Record element: structural value equality (§8.3). The Remove loop loads
    // `el` from the slot and lowers `target` as the record's struct value; an
    // ICmp on a struct is invalid, so this path is mandatory (not an extension).
    if (const sema::TypeSymbol* rec = recordTypeOfRef(elemTy)) {
        llvm::StructType* st = recordStruct(rec);
        llvm::Value* l = el, *r = target;
        if (l->getType()->isPointerTy()) l = builder_.CreateLoad(st, l);
        if (r->getType()->isPointerTy()) r = builder_.CreateLoad(st, r);
        if (l->getType()->isStructTy() && r->getType()->isStructTy())
            return emitStructEquality(l, r, st, rec);  // §8.3 structural
        // Shape mismatch (defensive): no match rather than an invalid compare.
        return builder_.getFalse();
    }

    // Class element: dispatch through a user `Equals` override when present.
    if (elemIsClass && el->getType()->isPointerTy() &&
        target->getType()->isPointerTy()) {
        const sema::TypeSymbol* cls =
            (elemTy && elemTy->kind == sema::TypeRef::Kind::Named)
                ? elemTy->declaration : nullptr;
        if (sema::MethodSymbol* eqm = classEqualsOverride(cls)) {
            // §8.4: `el.Equals(target)`. Guard against a null element slot or a
            // null query: dispatching through a null `el` would load a null
            // vtable. A null operand uses reference identity (= the null-safe
            // `=` of §8.4: null = null → true, null = nonNull → false), which a
            // plain pointer ICmp computes exactly.
            auto* nullP = llvm::ConstantPointerNull::get(ptrTy());
            llvm::Value* eitherNull = builder_.CreateOr(
                builder_.CreateICmpEQ(el, nullP),
                builder_.CreateICmpEQ(target, nullP));
            auto* idBB   = llvm::BasicBlock::Create(llvm_, "rmi.eq.id", currentFn_);
            auto* callBB = llvm::BasicBlock::Create(llvm_, "rmi.eq.call", currentFn_);
            auto* joinBB = llvm::BasicBlock::Create(llvm_, "rmi.eq.join", currentFn_);
            builder_.CreateCondBr(eitherNull, idBB, callBB);
            // null path: reference identity.
            builder_.SetInsertPoint(idBB);
            llvm::Value* idEq = builder_.CreateICmpEQ(el, target);
            llvm::BasicBlock* idEnd = builder_.GetInsertBlock();
            builder_.CreateBr(joinBB);
            // non-null path: virtual Equals(Object?) call. `target` is already a
            // class/Object pointer; `el` is the receiver (args[0]).
            builder_.SetInsertPoint(callBB);
            std::vector<llvm::Value*> eqArgs{el, target};
            llvm::Value* callEq =
                emitMethodCall(eqm, eqArgs, /*baseQualified=*/false);
            if (!callEq) return nullptr;
            if (callEq->getType()->isIntegerTy() &&
                !callEq->getType()->isIntegerTy(1))
                callEq = builder_.CreateICmpNE(
                    callEq, llvm::ConstantInt::get(callEq->getType(), 0));
            llvm::BasicBlock* callEnd = builder_.GetInsertBlock();
            builder_.CreateBr(joinBB);
            builder_.SetInsertPoint(joinBB);
            llvm::PHINode* phi = builder_.CreatePHI(i1(), 2, "rmi.eq");
            phi->addIncoming(idEq, idEnd);
            phi->addIncoming(callEq, callEnd);
            return phi;
        }
        // §8.3: no override → reference identity (the class default).
        return builder_.CreateICmpEQ(el, target);
    }

    // String element: content equality (§4.6).
    if (el->getType()->isPointerTy() && target->getType()->isPointerTy())
        return emitStringEq(el, target);

    // Value type (primitive/enum): `==`. Widen mismatched integer widths.
    llvm::Value* elN = el, *tN = target;
    if (el->getType() != target->getType() &&
        el->getType()->isIntegerTy() && target->getType()->isIntegerTy()) {
        unsigned ew = el->getType()->getIntegerBitWidth();
        unsigned tw = target->getType()->getIntegerBitWidth();
        llvm::Type* wide = ew >= tw ? el->getType() : target->getType();
        elN = builder_.CreateSExt(el, wide);
        tN  = builder_.CreateSExt(target, wide);
    }
    if (elN->getType()->isFloatingPointTy())
        return builder_.CreateFCmpOEQ(elN, tN);
    return builder_.CreateICmpEQ(elN, tN);
}


llvm::Value* Emitter::tryLowerCollectionInstanceCall(const ast::CallExpr& call) {
    if (!call.callee) return nullptr;
    auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(call.callee.get());
    if (!ma || !ma->target) return nullptr;
    const auto& m = ma->member;

    auto* recvTy = ctx_.exprType(*ma->target);
    bool isArr  = isCollectionTypeName(recvTy);
    bool isDict = isDictTypeName(recvTy);
    // Check for StringBuilder
    bool isStrBld = (recvTy && recvTy->astNode && recvTy->astNode->name &&
                     !recvTy->astNode->name->parts.empty() &&
                     recvTy->astNode->name->parts[0] == "StringBuilder");

    // FINDING [5] / stdlib §5: an EAGER terminal LINQ operator (Count / Sum /
    // Aggregate / Any / All / First / …) chained DIRECTLY onto a LAZY pipeline
    // (Select/Where/Concat/TakeWhile/SkipWhile/Zip/SelectMany — each returns
    // IEnumerable<T>). The terminal lowerings below read their source with
    // bz_array_len/bz_array_ptr_at, which assume a CONCRETE bz_array_t*; a lazy
    // enumerator (bz_lazy_t*) reports length 0, so the fold ran zero iterations
    // and returned the empty-sequence result (0/false/garbage). Fix: when the
    // receiver is a lazy IEnumerable<T> and the method is one of the
    // array-consuming terminals, MATERIALIZE the pipeline into a concrete
    // Array<T> (bz_lazy_to_array — the same drain ToArray uses, which disposes
    // the enumerator and releases its inline-lambda closures), then run the
    // existing array-based terminal over the materialized array. The temp array
    // (a borrowing snapshot of the enumerated bytes — it owns no elements) is
    // freed at the terminal's return with bz_array_free(arr, /*disposer=*/null).
    bool lazyRecv = isLazyEnumerableType(recvTy);
    bool isArrTerminal =
        m == "Count" || m == "Sum" || m == "Aggregate" || m == "Average" ||
        m == "Min" || m == "Max" || m == "First" || m == "Single" ||
        m == "Last" || m == "ElementAt" || m == "Any" || m == "All" ||
        m == "ForEach" || m == "Contains" || m == "IndexOf";
    bool lazyTerminal = lazyRecv && isArrTerminal;

    if (!isArr && !isDict && !isStrBld && !lazyTerminal) return nullptr;

    // The materialized-array temp to free before the terminal returns its
    // result (null unless we materialized a lazy receiver above).
    llvm::Value* lazyTempArr = nullptr;
    llvm::Value* recv = nullptr;
    if (lazyTerminal) {
        sema::TypeRef* elemRef = nullptr;
        llvm::Value* lazy = lowerToLazyEnumerator(*ma->target, elemRef);
        if (!lazy) return nullptr;
        // The materialized element byte size mirrors collectionElemSize: both
        // read the element TypeRef (genericArgs[0]). The lazy sentinel carries
        // it too, so collectionElemSize(recvTy) yields the same value the lazy
        // operators used when they were built (matching bz_lazy_to_array's esz).
        int64_t esz = collectionElemSize(recvTy);
        auto* toArr = declareC("bz_lazy_to_array", ptrTy(), {ptrTy(), i64()});
        recv = builder_.CreateCall(toArr, {lazy, builder_.getInt64(esz)},
                                   "linq.term.mat");
        lazyTempArr = recv;
        isArr = true;  // route through the concrete-array terminal lowerings
    } else {
        recv = lowerExpr(*ma->target);
    }
    if (!recv) return nullptr;

    // Free the materialized lazy snapshot (no element disposal — it borrows).
    // A no-op when the receiver was already a concrete collection. Emitted at
    // each terminal's return site so the result value is computed (loaded) from
    // the array BEFORE the backing buffer is released.
    auto freeLazyTemp = [&]() {
        if (!lazyTempArr) return;
        auto* fn = declareC("bz_array_free", llvm::Type::getVoidTy(llvm_),
                            {ptrTy(), ptrTy()});
        builder_.CreateCall(fn,
            {lazyTempArr, llvm::ConstantPointerNull::get(ptrTy())});
    };

    // -----------------------------------------------------------------------
    // Array / Stack / Queue / Set / LinkedList methods
    // -----------------------------------------------------------------------
    if (isArr) {
        int64_t esz = collectionElemSize(recvTy);

        // issues #343 / D8 / §4.10: a Set<T>/OwnedSet<T> is backed by the
        // generic-key hash table (allocated in the `new` path). Route its core
        // membership operations to the generic dict entry points so set
        // semantics follow the element's hash/equality (§8.3). This covers
        // records (#343), primitive/enum elements, AND string elements (D8) —
        // none of which the array backing could deduplicate. Element value is a
        // non-null sentinel; only membership matters. Other Set operations
        // (LINQ, set-algebra) are not wired in this slice and fall through.
        bool isSetName = recvTy && recvTy->astNode && recvTy->astNode->name &&
                         !recvTy->astNode->name->parts.empty() &&
                         (recvTy->astNode->name->parts[0] == "Set" ||
                          recvTy->astNode->name->parts[0] == "OwnedSet");
        if (isSetName) {
            if (const sema::TypeRef* elemTy =
                    genericBackingType(recvTy, 0, /*isSet=*/true, nullptr)) {
                // Spill the element to a stack slot of its natural LLVM type and
                // return the byte pointer the generic runtime expects. A record
                // value reaching us as a pointer is already an address; any
                // scalar/string value (including a string POINTER, whose bytes
                // are the key) is stored into a fresh slot of its own type.
                bool elemIsRecord = recordTypeOfRef(elemTy) != nullptr;
                auto elemPtr = [&](llvm::Value* v) -> llvm::Value* {
                    if (elemIsRecord && v->getType()->isPointerTy())
                        return v;  // record already spilled to an address
                    llvm::AllocaInst* slot = entryAlloca(v->getType());
                    builder_.CreateStore(v, slot);
                    return slot;
                };
                // sentinel non-null value stored for present elements
                llvm::Value* sentinel = builder_.CreateIntToPtr(
                    builder_.getInt64(1), ptrTy());
                // .Add(item) — insert (idempotent by content; §4.10 dedup).
                if (m == "Add" && call.args.size() == 1 && call.args[0].value) {
                    llvm::Value* item = lowerExpr(*call.args[0].value);
                    if (!item) return nullptr;
                    auto* fn = declareC("bz_dict_set_gen",
                        llvm::Type::getVoidTy(llvm_), {ptrTy(), ptrTy(), ptrTy()});
                    builder_.CreateCall(fn, {recv, elemPtr(item), sentinel});
                    return llvm::ConstantInt::get(i32(), 0);
                }
                // .Contains(item): bool
                if (m == "Contains" && call.args.size() == 1 && call.args[0].value) {
                    llvm::Value* item = lowerExpr(*call.args[0].value);
                    if (!item) return nullptr;
                    auto* fn = declareC("bz_dict_contains_gen", i1(),
                                        {ptrTy(), ptrTy()});
                    return builder_.CreateCall(fn, {recv, elemPtr(item)}, "set.has");
                }
                // .Remove(item): bool
                if (m == "Remove" && call.args.size() == 1 && call.args[0].value) {
                    llvm::Value* item = lowerExpr(*call.args[0].value);
                    if (!item) return nullptr;
                    auto* fn = declareC("bz_dict_remove_gen", i1(),
                                        {ptrTy(), ptrTy()});
                    return builder_.CreateCall(fn, {recv, elemPtr(item)}, "set.rm");
                }
                // .Count: int
                if (m == "Count" && call.args.empty()) {
                    auto* fn = declareC("bz_dict_count", i64(), {ptrTy()});
                    return builder_.CreateTrunc(
                        builder_.CreateCall(fn, {recv}), i32(), "set.cnt");
                }
                // .IsEmpty(): bool
                if (m == "IsEmpty" && call.args.empty()) {
                    auto* fn = declareC("bz_dict_count", i64(), {ptrTy()});
                    return builder_.CreateICmpEQ(
                        builder_.CreateCall(fn, {recv}),
                        builder_.getInt64(0), "set.empty");
                }
                // per §4.10 (#351): .Clear() empties the backing table. The
                // table stays allocated for reuse; Count is 0 afterward. A
                // dict-backed Set's value slots are non-owned sentinels, so no
                // value disposal is needed (null dispose_val).
                if (m == "Clear" && call.args.empty()) {
                    auto* fn = declareC("bz_dict_clear",
                        llvm::Type::getVoidTy(llvm_), {ptrTy(), ptrTy()});
                    builder_.CreateCall(fn,
                        {recv, llvm::ConstantPointerNull::get(ptrTy())});
                    return llvm::ConstantInt::get(i32(), 0);  // unit result
                }
                // per §4.10 (#351/#389): set-algebra ops over a dict-backed Set.
                // The helper snapshots `other`'s elements — either via the same-
                // typed Set's key table (bz_dict_keys_gen, #370) or, for a non-Set
                // IEnumerable<T> `other` (e.g. an Array<T> source), via the lazy
                // enumerator (#326) — then folds the snapshot into/against self in
                // the runtime (bz_set_algebra_gen). IntersectWith (#389) is now
                // included: it snapshots self's own keys before removing, so there
                // is no mutate-while-iterate hazard.
                if ((m == "UnionWith" || m == "ExceptWith" ||
                     m == "IntersectWith" ||
                     m == "IsSubsetOf" || m == "IsSupersetOf") &&
                    call.args.size() == 1 && call.args[0].value) {
                    // `elemTy` here is the self element's TypeRef (the binding
                    // from genericBackingType above); the helper lowers it.
                    if (llvm::Value* res = tryLowerSetAlgebra(
                            m, recv, *call.args[0].value, elemTy, elemIsRecord))
                        return res;
                    // other element type mismatched self → defer (fall out).
                }
                return nullptr;  // remaining ops scoped out for dict-backed sets
            }
        }

        // per §5 LINQ: closure struct { ptr fn, ptr env }; call as fn(env, elem).
        auto callClosure = [&](llvm::Value* closure,
                               llvm::Value* elem,
                               llvm::Type* retTy) -> llvm::Value* {
            llvm::Value* fnPtr = builder_.CreateLoad(ptrTy(), closure, "cl.fn");
            llvm::Value* envSlot = builder_.CreateGEP(ptrTy(), closure,
                                                      {builder_.getInt32(1)});
            llvm::Value* envPtr = builder_.CreateLoad(ptrTy(), envSlot, "cl.env");
            auto* fnTy = llvm::FunctionType::get(retTy, {ptrTy(), elem->getType()},
                                                 false);
            return builder_.CreateCall(fnTy, fnPtr, {envPtr, elem}, "cl.ret");
        };

        // Get element LLVM type from generic arg (default i32).
        auto getElemTy = [&]() -> llvm::Type* {
            if (recvTy && !recvTy->genericArgs.empty() && recvTy->genericArgs[0])
                if (auto* et = lowerTypeRef(recvTy->genericArgs[0])) return et;
            return i32();
        };

        // Lower the lambda argument with expectedFunctionType_ set so unannotated
        // lambda params get the right LLVM type (e.g. string elem → ptr, not i32).
        // retTypeRef: the TypeRef for the lambda's return type (bool/unit/etc).
        auto lowerLinqLambda = [&](const ast::Expr* argExpr,
                                   sema::TypeRef* retTypeRef) -> llvm::Value* {
            sema::TypeRef* elemTypeRef = (recvTy && !recvTy->genericArgs.empty())
                ? recvTy->genericArgs[0] : nullptr;
            // Build a synthetic Function TypeRef: (elemType) -> retType.
            auto* funcRef = ctx_.makeTypeRef();
            funcRef->kind = sema::TypeRef::Kind::Function;
            funcRef->funcReturn = retTypeRef;
            if (elemTypeRef) {
                sema::TypeRef::FuncParam fp;
                fp.type = elemTypeRef;
                funcRef->funcParams.push_back(std::move(fp));
            }
            const sema::TypeRef* savedExpected = expectedFunctionType_;
            expectedFunctionType_ = funcRef;
            llvm::Value* v = lowerExpr(*argExpr);
            expectedFunctionType_ = savedExpected;
            return v;
        };

        // #422 / §28.9 / §28.3: an EAGER LINQ predicate/selector closure produced
        // INLINE (a lambda literal) is a +1 transient we own; the eager op fully
        // consumes it in a loop that completes before the call returns, so we
        // release it at the loop-end. A closure arriving via a variable/field is a
        // BORROW (not ours to free). Only release the inline-lambda form — exactly
        // the GetOrAdd factoryIsInlineLambda guard. (Null-safe; LAZY ops differ —
        // they store the closure and release at enumerator dispose, see below.)
        auto argIsInlineLambda = [&](size_t i) -> bool {
            return i < call.args.size() && call.args[i].value &&
                   dynamic_cast<const ast::LambdaExpr*>(call.args[i].value.get())
                       != nullptr;
        };

        // per §5 LINQ: .Any(pred: (T)->bool): bool
        if (m == "Any" && call.args.size() == 1 && call.args[0].value) {
            const bool relCl = argIsInlineLambda(0);
            llvm::Value* closure = lowerLinqLambda(call.args[0].value.get(),
                ctx_.primitiveTypeRef(sema::PrimitiveType::Bool));
            if (!closure) return nullptr;
            llvm::Type* elemTy = getElemTy();
            auto* lenFn = declareC("bz_array_len", i64(), {ptrTy()});
            llvm::Value* len = builder_.CreateCall(lenFn, {recv});
            auto* idxSlot = entryAlloca(i64());
            builder_.CreateStore(builder_.getInt64(0), idxSlot);
            auto* condBB  = llvm::BasicBlock::Create(llvm_, "any.c", currentFn_);
            auto* bodyBB  = llvm::BasicBlock::Create(llvm_, "any.b", currentFn_);
            auto* nextBB  = llvm::BasicBlock::Create(llvm_, "any.n", currentFn_);
            auto* endBB   = llvm::BasicBlock::Create(llvm_, "any.e", currentFn_);
            builder_.CreateBr(condBB);
            builder_.SetInsertPoint(condBB);
            llvm::Value* idx = builder_.CreateLoad(i64(), idxSlot);
            builder_.CreateCondBr(builder_.CreateICmpSLT(idx, len), bodyBB, endBB);
            builder_.SetInsertPoint(bodyBB);
            auto* ptrFn = declareC("bz_array_ptr_at", ptrTy(), {ptrTy(), i64(), i64()});
            llvm::Value* ep = builder_.CreateCall(ptrFn, {recv, idx, builder_.getInt64(esz)});
            llvm::Value* el = builder_.CreateLoad(elemTy, ep);
            llvm::Value* ok = callClosure(closure, el, i1());
            builder_.CreateCondBr(ok, endBB, nextBB);
            builder_.SetInsertPoint(nextBB);
            builder_.CreateStore(builder_.CreateAdd(idx, builder_.getInt64(1)), idxSlot);
            builder_.CreateBr(condBB);
            builder_.SetInsertPoint(endBB);
            auto* phi = builder_.CreatePHI(i1(), 2, "any.r");
            phi->addIncoming(builder_.getFalse(), condBB);
            phi->addIncoming(builder_.getTrue(), bodyBB);
            if (relCl) emitClosureRelease(closure);  // #422: inline-lambda transient
            freeLazyTemp();  // FINDING [5]: release materialized lazy snapshot
            return phi;
        }

        // per §5 LINQ: .Any(): bool — true iff the source is non-empty.
        if (m == "Any" && call.args.empty()) {
            auto* lenFn = declareC("bz_array_len", i64(), {ptrTy()});
            llvm::Value* len = builder_.CreateCall(lenFn, {recv});
            llvm::Value* nonEmpty =
                builder_.CreateICmpSGT(len, builder_.getInt64(0), "any0.r");
            freeLazyTemp();  // FINDING [5]: release materialized lazy snapshot
            return nonEmpty;
        }

        // per §5 LINQ: .All(pred: (T)->bool): bool
        if (m == "All" && call.args.size() == 1 && call.args[0].value) {
            const bool relCl = argIsInlineLambda(0);
            llvm::Value* closure = lowerLinqLambda(call.args[0].value.get(),
                ctx_.primitiveTypeRef(sema::PrimitiveType::Bool));
            if (!closure) return nullptr;
            llvm::Type* elemTy = getElemTy();
            auto* lenFn = declareC("bz_array_len", i64(), {ptrTy()});
            llvm::Value* len = builder_.CreateCall(lenFn, {recv});
            auto* idxSlot = entryAlloca(i64());
            builder_.CreateStore(builder_.getInt64(0), idxSlot);
            auto* condBB  = llvm::BasicBlock::Create(llvm_, "all.c", currentFn_);
            auto* bodyBB  = llvm::BasicBlock::Create(llvm_, "all.b", currentFn_);
            auto* nextBB  = llvm::BasicBlock::Create(llvm_, "all.n", currentFn_);
            auto* endBB   = llvm::BasicBlock::Create(llvm_, "all.e", currentFn_);
            builder_.CreateBr(condBB);
            builder_.SetInsertPoint(condBB);
            llvm::Value* idx = builder_.CreateLoad(i64(), idxSlot);
            builder_.CreateCondBr(builder_.CreateICmpSLT(idx, len), bodyBB, endBB);
            builder_.SetInsertPoint(bodyBB);
            auto* ptrFn = declareC("bz_array_ptr_at", ptrTy(), {ptrTy(), i64(), i64()});
            llvm::Value* ep = builder_.CreateCall(ptrFn, {recv, idx, builder_.getInt64(esz)});
            llvm::Value* el = builder_.CreateLoad(elemTy, ep);
            llvm::Value* ok = callClosure(closure, el, i1());
            builder_.CreateCondBr(ok, nextBB, endBB);
            builder_.SetInsertPoint(nextBB);
            builder_.CreateStore(builder_.CreateAdd(idx, builder_.getInt64(1)), idxSlot);
            builder_.CreateBr(condBB);
            builder_.SetInsertPoint(endBB);
            auto* phi = builder_.CreatePHI(i1(), 2, "all.r");
            phi->addIncoming(builder_.getTrue(), condBB);
            phi->addIncoming(builder_.getFalse(), bodyBB);
            if (relCl) emitClosureRelease(closure);  // #422: inline-lambda transient
            freeLazyTemp();  // FINDING [5]: release materialized lazy snapshot
            return phi;
        }

        // per §5 LINQ: .ForEach(fn: (T)->())
        if (m == "ForEach" && call.args.size() == 1 && call.args[0].value) {
            const bool relCl = argIsInlineLambda(0);
            auto* unitRef = ctx_.makeTypeRef();
            unitRef->kind = sema::TypeRef::Kind::Unit;
            llvm::Value* closure = lowerLinqLambda(call.args[0].value.get(), unitRef);
            if (!closure) return nullptr;
            llvm::Type* elemTy = getElemTy();
            auto* lenFn = declareC("bz_array_len", i64(), {ptrTy()});
            llvm::Value* len = builder_.CreateCall(lenFn, {recv});
            auto* idxSlot = entryAlloca(i64());
            builder_.CreateStore(builder_.getInt64(0), idxSlot);
            auto* condBB = llvm::BasicBlock::Create(llvm_, "fe.c", currentFn_);
            auto* bodyBB = llvm::BasicBlock::Create(llvm_, "fe.b", currentFn_);
            auto* endBB  = llvm::BasicBlock::Create(llvm_, "fe.e", currentFn_);
            builder_.CreateBr(condBB);
            builder_.SetInsertPoint(condBB);
            llvm::Value* idx = builder_.CreateLoad(i64(), idxSlot);
            builder_.CreateCondBr(builder_.CreateICmpSLT(idx, len), bodyBB, endBB);
            builder_.SetInsertPoint(bodyBB);
            auto* ptrFn = declareC("bz_array_ptr_at", ptrTy(), {ptrTy(), i64(), i64()});
            llvm::Value* ep = builder_.CreateCall(ptrFn, {recv, idx, builder_.getInt64(esz)});
            llvm::Value* el = builder_.CreateLoad(elemTy, ep);
            callClosure(closure, el, llvm::Type::getVoidTy(llvm_));
            builder_.CreateStore(builder_.CreateAdd(idx, builder_.getInt64(1)), idxSlot);
            builder_.CreateBr(condBB);
            builder_.SetInsertPoint(endBB);
            if (relCl) emitClosureRelease(closure);  // #422: inline-lambda transient
            freeLazyTemp();  // FINDING [5]: release materialized lazy snapshot
            return llvm::ConstantInt::get(i32(), 0);
        }

        // per §5 LINQ: .Count(pred: (T)->bool): int
        if (m == "Count" && call.args.size() == 1 && call.args[0].value) {
            const bool relCl = argIsInlineLambda(0);
            llvm::Value* closure = lowerLinqLambda(call.args[0].value.get(),
                ctx_.primitiveTypeRef(sema::PrimitiveType::Bool));
            if (!closure) return nullptr;
            llvm::Type* elemTy = getElemTy();
            auto* lenFn = declareC("bz_array_len", i64(), {ptrTy()});
            llvm::Value* len = builder_.CreateCall(lenFn, {recv});
            auto* idxSlot = entryAlloca(i64());
            auto* cntSlot = entryAlloca(i32());
            builder_.CreateStore(builder_.getInt64(0), idxSlot);
            builder_.CreateStore(builder_.getInt32(0), cntSlot);
            auto* condBB = llvm::BasicBlock::Create(llvm_, "cnt.c", currentFn_);
            auto* bodyBB = llvm::BasicBlock::Create(llvm_, "cnt.b", currentFn_);
            auto* incrBB = llvm::BasicBlock::Create(llvm_, "cnt.i", currentFn_);
            auto* nextBB = llvm::BasicBlock::Create(llvm_, "cnt.n", currentFn_);
            auto* endBB  = llvm::BasicBlock::Create(llvm_, "cnt.e", currentFn_);
            builder_.CreateBr(condBB);
            builder_.SetInsertPoint(condBB);
            llvm::Value* idx = builder_.CreateLoad(i64(), idxSlot);
            builder_.CreateCondBr(builder_.CreateICmpSLT(idx, len), bodyBB, endBB);
            builder_.SetInsertPoint(bodyBB);
            auto* ptrFn = declareC("bz_array_ptr_at", ptrTy(), {ptrTy(), i64(), i64()});
            llvm::Value* ep = builder_.CreateCall(ptrFn, {recv, idx, builder_.getInt64(esz)});
            llvm::Value* el = builder_.CreateLoad(elemTy, ep);
            llvm::Value* ok = callClosure(closure, el, i1());
            builder_.CreateCondBr(ok, incrBB, nextBB);
            builder_.SetInsertPoint(incrBB);
            llvm::Value* c = builder_.CreateLoad(i32(), cntSlot);
            builder_.CreateStore(builder_.CreateAdd(c, builder_.getInt32(1)), cntSlot);
            builder_.CreateBr(nextBB);
            builder_.SetInsertPoint(nextBB);
            builder_.CreateStore(builder_.CreateAdd(idx, builder_.getInt64(1)), idxSlot);
            builder_.CreateBr(condBB);
            builder_.SetInsertPoint(endBB);
            if (relCl) emitClosureRelease(closure);  // #422: inline-lambda transient
            freeLazyTemp();  // FINDING [5]: release materialized lazy snapshot
            return builder_.CreateLoad(i32(), cntSlot, "cnt.r");
        }

        // per §5 LINQ: .Select / .Where are LAZY — see tryLowerLazyLinqCall,
        // dispatched before this collection path.

        // .Length: int / .Count: int
        if ((m == "Length" || m == "Count") && call.args.empty()) {
            auto* fn = declareC("bz_array_len", i64(), {ptrTy()});
            llvm::Value* len64 = builder_.CreateCall(fn, {recv});
            llvm::Value* len32 = builder_.CreateTrunc(len64, i32(), "arr.len");
            freeLazyTemp();  // FINDING [5]: release materialized lazy snapshot
            return len32;
        }
        // per §5 LINQ: .Sum(fn: (T)->int): int  /  .Sum(): int (for Array<int>)
        if (m == "Sum") {
            const bool relCl = argIsInlineLambda(0);
            llvm::Type* elemTy = getElemTy();
            sema::TypeRef* retTyRef = ctx_.primitiveTypeRef(sema::PrimitiveType::Int);
            llvm::Value* closure = call.args.empty() ? nullptr
                : lowerLinqLambda(call.args[0].value.get(), retTyRef);
            if (!call.args.empty() && !closure) return nullptr;
            auto* lenFn = declareC("bz_array_len", i64(), {ptrTy()});
            llvm::Value* len = builder_.CreateCall(lenFn, {recv});
            auto* idxSlot = entryAlloca(i64());
            auto* accSlot = entryAlloca(i32());
            builder_.CreateStore(builder_.getInt64(0), idxSlot);
            builder_.CreateStore(builder_.getInt32(0), accSlot);
            auto* condBB = llvm::BasicBlock::Create(llvm_, "sum.c", currentFn_);
            auto* bodyBB = llvm::BasicBlock::Create(llvm_, "sum.b", currentFn_);
            auto* endBB  = llvm::BasicBlock::Create(llvm_, "sum.e", currentFn_);
            builder_.CreateBr(condBB);
            builder_.SetInsertPoint(condBB);
            llvm::Value* idx = builder_.CreateLoad(i64(), idxSlot);
            builder_.CreateCondBr(builder_.CreateICmpSLT(idx, len), bodyBB, endBB);
            builder_.SetInsertPoint(bodyBB);
            auto* ptrFn = declareC("bz_array_ptr_at", ptrTy(), {ptrTy(), i64(), i64()});
            llvm::Value* ep = builder_.CreateCall(ptrFn, {recv, idx, builder_.getInt64(esz)});
            llvm::Value* el = builder_.CreateLoad(elemTy, ep);
            llvm::Value* val = closure ? callClosure(closure, el, i32()) : el;
            // Widen to i32 if needed
            if (val->getType() != i32()) val = builder_.CreateTrunc(val, i32());
            llvm::Value* acc = builder_.CreateLoad(i32(), accSlot);
            builder_.CreateStore(builder_.CreateAdd(acc, val), accSlot);
            builder_.CreateStore(builder_.CreateAdd(idx, builder_.getInt64(1)), idxSlot);
            builder_.CreateBr(condBB);
            builder_.SetInsertPoint(endBB);
            if (relCl && closure) emitClosureRelease(closure);  // #422: inline-lambda
            freeLazyTemp();  // FINDING [5]: release materialized lazy snapshot
            return builder_.CreateLoad(i32(), accSlot, "sum.r");
        }
        // per §5 LINQ: .Aggregate<TAccum>(seed: TAccum, fold: (TAccum, T) -> TAccum)
        // — eager left fold. The accumulator starts at `seed` and folds each
        // element via the binary `fold` lambda; the result is TAccum.
        if (m == "Aggregate" && call.args.size() == 2 &&
            call.args[0].value && call.args[1].value) {
            llvm::Type* elemTy = getElemTy();
            // Seed value establishes the accumulator's LLVM type (TAccum).
            llvm::Value* seed = lowerExpr(*call.args[0].value);
            if (!seed) return nullptr;
            llvm::Type* accTy = seed->getType();
            // Lower the fold lambda `(TAccum, T) -> TAccum`. Its params are
            // annotated in idiomatic use; lower it as a plain expression.
            const bool relCl = argIsInlineLambda(1);
            llvm::Value* foldCl = lowerExpr(*call.args[1].value);
            if (!foldCl) return nullptr;
            auto* accSlot = entryAlloca(accTy);
            builder_.CreateStore(seed, accSlot);
            auto* idxSlot = entryAlloca(i64());
            builder_.CreateStore(builder_.getInt64(0), idxSlot);
            auto* lenFn = declareC("bz_array_len", i64(), {ptrTy()});
            llvm::Value* len = builder_.CreateCall(lenFn, {recv});
            auto* condBB = llvm::BasicBlock::Create(llvm_, "agg.c", currentFn_);
            auto* bodyBB = llvm::BasicBlock::Create(llvm_, "agg.b", currentFn_);
            auto* endBB  = llvm::BasicBlock::Create(llvm_, "agg.e", currentFn_);
            builder_.CreateBr(condBB);
            builder_.SetInsertPoint(condBB);
            llvm::Value* idx = builder_.CreateLoad(i64(), idxSlot);
            builder_.CreateCondBr(builder_.CreateICmpSLT(idx, len), bodyBB, endBB);
            builder_.SetInsertPoint(bodyBB);
            auto* ptrFn = declareC("bz_array_ptr_at", ptrTy(), {ptrTy(), i64(), i64()});
            llvm::Value* ep = builder_.CreateCall(ptrFn, {recv, idx, builder_.getInt64(esz)});
            llvm::Value* el = builder_.CreateLoad(elemTy, ep);
            llvm::Value* acc = builder_.CreateLoad(accTy, accSlot);
            // Binary closure call: fn(env, acc, elem) -> TAccum.
            llvm::Value* fnPtr = builder_.CreateLoad(ptrTy(), foldCl, "agg.fn");
            llvm::Value* envSlot = builder_.CreateGEP(ptrTy(), foldCl, {builder_.getInt32(1)});
            llvm::Value* envPtr = builder_.CreateLoad(ptrTy(), envSlot, "agg.env");
            auto* foldTy = llvm::FunctionType::get(accTy, {ptrTy(), accTy, elemTy}, false);
            llvm::Value* next = builder_.CreateCall(foldTy, fnPtr, {envPtr, acc, el}, "agg.next");
            builder_.CreateStore(next, accSlot);
            builder_.CreateStore(builder_.CreateAdd(idx, builder_.getInt64(1)), idxSlot);
            builder_.CreateBr(condBB);
            builder_.SetInsertPoint(endBB);
            if (relCl) emitClosureRelease(foldCl);  // #422: inline-lambda transient
            freeLazyTemp();  // FINDING [5]: release materialized lazy snapshot
            return builder_.CreateLoad(accTy, accSlot, "agg.r");
        }
        // per §5/§5.1 LINQ: .Average(fn: (T)->numeric): double  /  .Average(): double
        // — eager. Accumulate each element (or selector result) as a double and
        // divide by the element count. The §5 selector overloads (int/long/double)
        // and the §5.1 per-type no-selector forms all return double.
        if (m == "Average") {
            const bool relCl = argIsInlineLambda(0);
            llvm::Type* elemTy = getElemTy();
            // The selector's actual LLVM return type (int/long/float/double per
            // the §5 overloads); the closure must be called with its true return
            // type, then coerced to double for accumulation.
            llvm::Type* selRetTy = elemTy;
            sema::TypeRef* selRetRef = nullptr;
            if (!call.args.empty() && call.args[0].value)
                if (auto* lamTy = ctx_.exprType(*call.args[0].value))
                    if (lamTy->kind == sema::TypeRef::Kind::Function && lamTy->funcReturn) {
                        selRetRef = lamTy->funcReturn;
                        if (auto* t = lowerTypeRef(selRetRef)) selRetTy = t;
                    }
            llvm::Value* closure = call.args.empty() ? nullptr
                : lowerLinqLambda(call.args[0].value.get(), selRetRef);
            if (!call.args.empty() && !closure) return nullptr;
            llvm::Type* dblTy = llvm::Type::getDoubleTy(llvm_);
            // Coerce an int/float numeric value to double.
            auto toDouble = [&](llvm::Value* v) -> llvm::Value* {
                if (v->getType()->isDoubleTy()) return v;
                if (v->getType()->isFloatingPointTy())
                    return builder_.CreateFPExt(v, dblTy);
                return builder_.CreateSIToFP(v, dblTy);
            };
            auto* lenFn = declareC("bz_array_len", i64(), {ptrTy()});
            llvm::Value* len = builder_.CreateCall(lenFn, {recv});
            auto* idxSlot = entryAlloca(i64());
            auto* accSlot = entryAlloca(dblTy);
            builder_.CreateStore(builder_.getInt64(0), idxSlot);
            builder_.CreateStore(llvm::ConstantFP::get(dblTy, 0.0), accSlot);
            auto* condBB = llvm::BasicBlock::Create(llvm_, "avg.c", currentFn_);
            auto* bodyBB = llvm::BasicBlock::Create(llvm_, "avg.b", currentFn_);
            auto* endBB  = llvm::BasicBlock::Create(llvm_, "avg.e", currentFn_);
            builder_.CreateBr(condBB);
            builder_.SetInsertPoint(condBB);
            llvm::Value* idx = builder_.CreateLoad(i64(), idxSlot);
            builder_.CreateCondBr(builder_.CreateICmpSLT(idx, len), bodyBB, endBB);
            builder_.SetInsertPoint(bodyBB);
            auto* ptrFn = declareC("bz_array_ptr_at", ptrTy(), {ptrTy(), i64(), i64()});
            llvm::Value* ep = builder_.CreateCall(ptrFn, {recv, idx, builder_.getInt64(esz)});
            llvm::Value* el = builder_.CreateLoad(elemTy, ep);
            llvm::Value* val = closure
                ? toDouble(callClosure(closure, el, selRetTy))
                : toDouble(el);
            llvm::Value* acc = builder_.CreateLoad(dblTy, accSlot);
            builder_.CreateStore(builder_.CreateFAdd(acc, val), accSlot);
            builder_.CreateStore(builder_.CreateAdd(idx, builder_.getInt64(1)), idxSlot);
            builder_.CreateBr(condBB);
            builder_.SetInsertPoint(endBB);
            llvm::Value* total = builder_.CreateLoad(dblTy, accSlot, "avg.sum");
            llvm::Value* cnt = builder_.CreateSIToFP(len, dblTy, "avg.cnt");
            if (relCl && closure) emitClosureRelease(closure);  // #422: inline-lambda
            freeLazyTemp();  // FINDING [5]: release materialized lazy snapshot
            return builder_.CreateFDiv(total, cnt, "avg.r");
        }
        // per §5 LINQ: .Min/.Max (fn: (T)->int): int  or no-arg for Array<int>
        if ((m == "Min" || m == "Max") &&
            (call.args.empty() || call.args.size() == 1)) {
            bool isMax = (m == "Max");
            const bool relCl = argIsInlineLambda(0);
            llvm::Type* elemTy = getElemTy();
            sema::TypeRef* retTyRef = ctx_.primitiveTypeRef(sema::PrimitiveType::Int);
            llvm::Value* closure = call.args.empty() ? nullptr
                : lowerLinqLambda(call.args[0].value.get(), retTyRef);
            if (!call.args.empty() && !closure) return nullptr;
            auto* lenFn = declareC("bz_array_len", i64(), {ptrTy()});
            llvm::Value* len = builder_.CreateCall(lenFn, {recv});
            auto* idxSlot  = entryAlloca(i64());
            auto* accSlot  = entryAlloca(i32());
            auto* initSlot = entryAlloca(i1());
            builder_.CreateStore(builder_.getInt64(0), idxSlot);
            builder_.CreateStore(builder_.getInt32(0), accSlot);
            builder_.CreateStore(builder_.getFalse(), initSlot);
            auto* condBB = llvm::BasicBlock::Create(llvm_, "mm.c",  currentFn_);
            auto* bodyBB = llvm::BasicBlock::Create(llvm_, "mm.b",  currentFn_);
            auto* updBB  = llvm::BasicBlock::Create(llvm_, "mm.u",  currentFn_);
            auto* nextBB = llvm::BasicBlock::Create(llvm_, "mm.n",  currentFn_);
            auto* endBB  = llvm::BasicBlock::Create(llvm_, "mm.e",  currentFn_);
            builder_.CreateBr(condBB);
            builder_.SetInsertPoint(condBB);
            llvm::Value* idx = builder_.CreateLoad(i64(), idxSlot);
            builder_.CreateCondBr(builder_.CreateICmpSLT(idx, len), bodyBB, endBB);
            builder_.SetInsertPoint(bodyBB);
            auto* ptrFn = declareC("bz_array_ptr_at", ptrTy(), {ptrTy(), i64(), i64()});
            llvm::Value* ep = builder_.CreateCall(ptrFn, {recv, idx, builder_.getInt64(esz)});
            llvm::Value* el = builder_.CreateLoad(elemTy, ep);
            llvm::Value* val = closure ? callClosure(closure, el, i32()) : el;
            if (val->getType() != i32()) val = builder_.CreateTrunc(val, i32());
            llvm::Value* inited = builder_.CreateLoad(i1(), initSlot);
            llvm::Value* acc = builder_.CreateLoad(i32(), accSlot);
            llvm::Value* cmp = isMax ? builder_.CreateICmpSGT(val, acc)
                                     : builder_.CreateICmpSLT(val, acc);
            builder_.CreateCondBr(builder_.CreateOr(builder_.CreateNot(inited), cmp),
                                  updBB, nextBB);
            builder_.SetInsertPoint(updBB);
            builder_.CreateStore(val, accSlot);
            builder_.CreateStore(builder_.getTrue(), initSlot);
            builder_.CreateBr(nextBB);
            builder_.SetInsertPoint(nextBB);
            builder_.CreateStore(builder_.CreateAdd(idx, builder_.getInt64(1)), idxSlot);
            builder_.CreateBr(condBB);
            builder_.SetInsertPoint(endBB);
            if (relCl && closure) emitClosureRelease(closure);  // #422: inline-lambda
            freeLazyTemp();  // FINDING [5]: release materialized lazy snapshot
            return builder_.CreateLoad(i32(), accSlot, "mm.r");
        }
        // per §5 LINQ: .First(pred: (T)->bool): T  (throws if empty/not found)
        if (m == "First" && call.args.size() == 1 && call.args[0].value) {
            const bool relCl = argIsInlineLambda(0);
            llvm::Value* closure = lowerLinqLambda(call.args[0].value.get(),
                ctx_.primitiveTypeRef(sema::PrimitiveType::Bool));
            if (!closure) return nullptr;
            llvm::Type* elemTy = getElemTy();
            auto* lenFn = declareC("bz_array_len", i64(), {ptrTy()});
            llvm::Value* len = builder_.CreateCall(lenFn, {recv});
            auto* resultSlot = entryAlloca(elemTy);
            auto* idxSlot = entryAlloca(i64());
            builder_.CreateStore(builder_.getInt64(0), idxSlot);
            auto* condBB  = llvm::BasicBlock::Create(llvm_, "fst.c", currentFn_);
            auto* bodyBB  = llvm::BasicBlock::Create(llvm_, "fst.b", currentFn_);
            auto* foundBB = llvm::BasicBlock::Create(llvm_, "fst.f", currentFn_);
            auto* nextBB  = llvm::BasicBlock::Create(llvm_, "fst.n", currentFn_);
            auto* endBB   = llvm::BasicBlock::Create(llvm_, "fst.e", currentFn_);
            builder_.CreateBr(condBB);
            builder_.SetInsertPoint(condBB);
            llvm::Value* idx = builder_.CreateLoad(i64(), idxSlot);
            builder_.CreateCondBr(builder_.CreateICmpSLT(idx, len), bodyBB, endBB);
            builder_.SetInsertPoint(bodyBB);
            auto* ptrFn = declareC("bz_array_ptr_at", ptrTy(), {ptrTy(), i64(), i64()});
            llvm::Value* ep = builder_.CreateCall(ptrFn, {recv, idx, builder_.getInt64(esz)});
            llvm::Value* el = builder_.CreateLoad(elemTy, ep);
            llvm::Value* ok = callClosure(closure, el, i1());
            builder_.CreateCondBr(ok, foundBB, nextBB);
            builder_.SetInsertPoint(foundBB);
            builder_.CreateStore(el, resultSlot);
            builder_.CreateBr(endBB);
            builder_.SetInsertPoint(nextBB);
            builder_.CreateStore(builder_.CreateAdd(idx, builder_.getInt64(1)), idxSlot);
            builder_.CreateBr(condBB);
            builder_.SetInsertPoint(endBB);
            if (relCl) emitClosureRelease(closure);  // #422: inline-lambda transient
            llvm::Value* fstv = builder_.CreateLoad(elemTy, resultSlot, "fst.r");
            freeLazyTemp();  // FINDING [5]: release materialized lazy snapshot
            return fstv;
        }
        // per §5 LINQ element access (no-predicate forms): First()/Single()
        // return the first element; Last() returns the last; ElementAt(i)
        // returns the element at index i (stdlib §5 — each `: T`). These read
        // an element from the eager Array source via bz_array_get. (The
        // empty/not-exactly-one throwing semantics noted in stdlib §5 are not
        // modeled in the JIT happy path, consistent with First(pred) above.)
        if ((m == "First" || m == "Single") && call.args.empty()) {
            llvm::Type* retTy = getElemTy();
            llvm::AllocaInst* out = entryAlloca(retTy);
            auto* getFn = declareC("bz_array_get", llvm::Type::getVoidTy(llvm_),
                                   {ptrTy(), i64(), ptrTy(), i64()});
            builder_.CreateCall(getFn,
                {recv, builder_.getInt64(0), out, builder_.getInt64(esz)});
            llvm::Value* fstv = builder_.CreateLoad(retTy, out, "fst.r");
            freeLazyTemp();  // FINDING [5]: release materialized lazy snapshot
            return fstv;
        }
        if (m == "Last" && call.args.empty()) {
            llvm::Type* retTy = getElemTy();
            auto* lenFn = declareC("bz_array_len", i64(), {ptrTy()});
            llvm::Value* len = builder_.CreateCall(lenFn, {recv});
            llvm::Value* idx = builder_.CreateSub(len, builder_.getInt64(1));
            llvm::AllocaInst* out = entryAlloca(retTy);
            auto* getFn = declareC("bz_array_get", llvm::Type::getVoidTy(llvm_),
                                   {ptrTy(), i64(), ptrTy(), i64()});
            builder_.CreateCall(getFn, {recv, idx, out, builder_.getInt64(esz)});
            llvm::Value* lstv = builder_.CreateLoad(retTy, out, "lst.r");
            freeLazyTemp();  // FINDING [5]: release materialized lazy snapshot
            return lstv;
        }
        if (m == "ElementAt" && call.args.size() == 1 && call.args[0].value) {
            llvm::Value* idx = lowerExpr(*call.args[0].value);
            if (!idx) return nullptr;
            idx = builder_.CreateSExt(idx, i64());
            llvm::Type* retTy = getElemTy();
            llvm::AllocaInst* out = entryAlloca(retTy);
            auto* getFn = declareC("bz_array_get", llvm::Type::getVoidTy(llvm_),
                                   {ptrTy(), i64(), ptrTy(), i64()});
            builder_.CreateCall(getFn, {recv, idx, out, builder_.getInt64(esz)});
            llvm::Value* eav = builder_.CreateLoad(retTy, out, "elemat.r");
            freeLazyTemp();  // FINDING [5]: release materialized lazy snapshot
            return eav;
        }
        // per §5 LINQ: .Skip / .Take are LAZY — see tryLowerLazyLinqCall.

        // per §3: .Sort() — in-place ascending sort for int arrays.
        // Uses bz_array_sort with a built-in i32 comparator thunk.
        if (m == "Sort" && call.args.empty()) {
            // Choose comparator based on element type: int → bz$intcmp, string → bz$strcmp.
            llvm::Type* elemTy2 = getElemTy();
            bool isStringElem = elemTy2 == ptrTy();
            llvm::Function* cmpFn = nullptr;
            if (isStringElem) {
                // String comparator: load two ptr elements, call strcmp on their data.
                auto* cmpName2 = "bz$strcmp";
                cmpFn = module_->getFunction(cmpName2);
                if (!cmpFn) {
                    auto* cmpTy2 = llvm::FunctionType::get(i32(), {ptrTy(), ptrTy()}, false);
                    cmpFn = llvm::Function::Create(cmpTy2, llvm::Function::InternalLinkage,
                                                    cmpName2, module_.get());
                    auto* bb2 = llvm::BasicBlock::Create(llvm_, "entry", cmpFn);
                    llvm::IRBuilder<> cmpB2(bb2);
                    // Each argument is a ptr* (pointer to bz.string*).
                    // Load the bz.string* from each slot, then load data pointer.
                    llvm::Value* aSlot = &*cmpFn->arg_begin();
                    llvm::Value* bSlot = &*std::next(cmpFn->arg_begin());
                    llvm::Value* aStr = cmpB2.CreateLoad(ptrTy(), aSlot);
                    llvm::Value* bStr = cmpB2.CreateLoad(ptrTy(), bSlot);
                    // Load data pointer (field 0) from bz.string struct
                    llvm::Value* aData = cmpB2.CreateLoad(ptrTy(),
                        cmpB2.CreateStructGEP(stringType(), aStr, 0));
                    llvm::Value* bData = cmpB2.CreateLoad(ptrTy(),
                        cmpB2.CreateStructGEP(stringType(), bStr, 0));
                    // Call strcmp
                    auto* strcmpFn2 = module_->getFunction("strcmp");
                    if (!strcmpFn2) {
                        auto* ft = llvm::FunctionType::get(i32(), {ptrTy(), ptrTy()}, false);
                        strcmpFn2 = llvm::Function::Create(ft, llvm::Function::ExternalLinkage,
                                                           "strcmp", module_.get());
                    }
                    cmpB2.CreateRet(cmpB2.CreateCall(strcmpFn2, {aData, bData}));
                }
            } else {
                // Integer comparator
                auto* cmpName = "bz$intcmp";
                cmpFn = module_->getFunction(cmpName);
                if (!cmpFn) {
                    auto* cmpTy = llvm::FunctionType::get(i32(), {ptrTy(), ptrTy()}, false);
                    cmpFn = llvm::Function::Create(cmpTy, llvm::Function::InternalLinkage,
                                                   cmpName, module_.get());
                    auto* bb = llvm::BasicBlock::Create(llvm_, "entry", cmpFn);
                    llvm::IRBuilder<> cmpB(bb);
                    llvm::Value* a = &*cmpFn->arg_begin();
                    llvm::Value* b = &*std::next(cmpFn->arg_begin());
                    llvm::Value* av = cmpB.CreateLoad(i32(), a);
                    llvm::Value* bv = cmpB.CreateLoad(i32(), b);
                    cmpB.CreateRet(cmpB.CreateSub(av, bv));
                }
            }
            auto* sortFn = declareC("bz_array_sort", llvm::Type::getVoidTy(llvm_),
                                    {ptrTy(), i64(), ptrTy()});
            builder_.CreateCall(sortFn, {recv, builder_.getInt64(esz), cmpFn});
            return llvm::ConstantInt::get(i32(), 0);
        }
        // per §3: .Reverse() — in-place reverse.
        if (m == "Reverse" && call.args.empty()) {
            llvm::Type* elemTy = getElemTy();
            auto* lenFn = declareC("bz_array_len", i64(), {ptrTy()});
            llvm::Value* len = builder_.CreateCall(lenFn, {recv});
            auto* idxSlot = entryAlloca(i64());
            builder_.CreateStore(builder_.getInt64(0), idxSlot);
            auto* condBB = llvm::BasicBlock::Create(llvm_, "rev.c", currentFn_);
            auto* bodyBB = llvm::BasicBlock::Create(llvm_, "rev.b", currentFn_);
            auto* endBB  = llvm::BasicBlock::Create(llvm_, "rev.e", currentFn_);
            builder_.CreateBr(condBB);
            builder_.SetInsertPoint(condBB);
            llvm::Value* i = builder_.CreateLoad(i64(), idxSlot);
            llvm::Value* half = builder_.CreateSDiv(len, builder_.getInt64(2));
            builder_.CreateCondBr(builder_.CreateICmpSLT(i, half), bodyBB, endBB);
            builder_.SetInsertPoint(bodyBB);
            auto* ptrFn = declareC("bz_array_ptr_at", ptrTy(), {ptrTy(), i64(), i64()});
            llvm::Value* j = builder_.CreateSub(builder_.CreateSub(len, i), builder_.getInt64(1));
            llvm::Value* pa = builder_.CreateCall(ptrFn, {recv, i, builder_.getInt64(esz)});
            llvm::Value* pb = builder_.CreateCall(ptrFn, {recv, j, builder_.getInt64(esz)});
            // Swap: temp = *pa; *pa = *pb; *pb = temp
            llvm::Value* ta = builder_.CreateLoad(elemTy, pa);
            llvm::Value* tb = builder_.CreateLoad(elemTy, pb);
            builder_.CreateStore(tb, pa);
            builder_.CreateStore(ta, pb);
            builder_.CreateStore(builder_.CreateAdd(i, builder_.getInt64(1)), idxSlot);
            builder_.CreateBr(condBB);
            builder_.SetInsertPoint(endBB);
            return llvm::ConstantInt::get(i32(), 0);
        }
        // per §5 LINQ: .OrderBy / .OrderByDescending are LAZY — see
        // tryLowerLazyLinqCall (the sorted snapshot is built on first
        // enumeration, after which iteration is deferred per element).

        // per §3: .IndexOf(item): int — index of first match, -1 if not found
        if (m == "IndexOf" && call.args.size() == 1 && call.args[0].value) {
            llvm::Value* target = lowerExpr(*call.args[0].value);
            if (!target) return nullptr;
            llvm::Type* elemTy = getElemTy();
            auto* lenFn = declareC("bz_array_len", i64(), {ptrTy()});
            llvm::Value* len = builder_.CreateCall(lenFn, {recv});
            auto* idxSlot = entryAlloca(i64());
            builder_.CreateStore(builder_.getInt64(0), idxSlot);
            auto* condBB  = llvm::BasicBlock::Create(llvm_, "iof.c", currentFn_);
            auto* bodyBB  = llvm::BasicBlock::Create(llvm_, "iof.b", currentFn_);
            auto* foundBB = llvm::BasicBlock::Create(llvm_, "iof.f", currentFn_);
            auto* nextBB  = llvm::BasicBlock::Create(llvm_, "iof.n", currentFn_);
            auto* endBB   = llvm::BasicBlock::Create(llvm_, "iof.e", currentFn_);
            builder_.CreateBr(condBB);
            builder_.SetInsertPoint(condBB);
            llvm::Value* idx = builder_.CreateLoad(i64(), idxSlot);
            builder_.CreateCondBr(builder_.CreateICmpSLT(idx, len), bodyBB, endBB);
            builder_.SetInsertPoint(bodyBB);
            auto* ptrFn = declareC("bz_array_ptr_at", ptrTy(), {ptrTy(), i64(), i64()});
            llvm::Value* ep = builder_.CreateCall(ptrFn, {recv, idx, builder_.getInt64(esz)});
            llvm::Value* el = builder_.CreateLoad(elemTy, ep);
            llvm::Value* eq;
            if (el->getType()->isPointerTy() && target->getType()->isPointerTy())
                eq = emitStringEq(el, target);
            else {
                llvm::Value* elN = el, *tN = target;
                if (el->getType() != target->getType() &&
                    el->getType()->isIntegerTy() && target->getType()->isIntegerTy()) {
                    unsigned ew = el->getType()->getIntegerBitWidth();
                    unsigned tw = target->getType()->getIntegerBitWidth();
                    llvm::Type* wide = ew >= tw ? el->getType() : target->getType();
                    elN = builder_.CreateSExt(el, wide);
                    tN = builder_.CreateSExt(target, wide);
                }
                eq = builder_.CreateICmpEQ(elN, tN);
            }
            builder_.CreateCondBr(eq, foundBB, nextBB);
            builder_.SetInsertPoint(foundBB);
            llvm::Value* result64 = idx;
            builder_.CreateBr(endBB);
            builder_.SetInsertPoint(nextBB);
            builder_.CreateStore(builder_.CreateAdd(idx, builder_.getInt64(1)), idxSlot);
            builder_.CreateBr(condBB);
            builder_.SetInsertPoint(endBB);
            auto* phiIdx = builder_.CreatePHI(i64(), 2, "iof.r");
            phiIdx->addIncoming(result64, foundBB);
            phiIdx->addIncoming(builder_.getInt64(-1), condBB);
            llvm::Value* iofv = builder_.CreateTrunc(phiIdx, i32(), "iof.i32");
            freeLazyTemp();  // FINDING [5]: release materialized lazy snapshot
            return iofv;
        }
        // per §5 LINQ: .Distinct() is LAZY — see tryLowerLazyLinqCall.

        // .Add(v) / .Push(v) / .Enqueue(v)
        if ((m == "Add" || m == "Push" || m == "Enqueue") &&
            call.args.size() == 1 && call.args[0].value) {
            llvm::Value* v = lowerExpr(*call.args[0].value);
            if (!v) return nullptr;
            // Store elem to stack alloca, pass pointer to runtime
            llvm::AllocaInst* slot = entryAlloca(v->getType());
            builder_.CreateStore(v, slot);
            auto* fn = declareC("bz_array_add", llvm::Type::getVoidTy(llvm_),
                                {ptrTy(), ptrTy(), i64()});
            builder_.CreateCall(fn, {recv, slot, builder_.getInt64(esz)});
            return llvm::ConstantInt::get(i32(), 0);  // unit/void result
        }
        // per §4.5: .Insert(index, item) — insert at index, shifting up. For an
        // OwnedArray<T> the item is `owned` (already moved in by the caller);
        // there is no element to dispose (a fresh slot is created, not a
        // replacement), so insertion is the same op for owning and borrowing
        // arrays — the runtime just shifts and stores the element bytes.
        if (m == "Insert" && call.args.size() == 2 &&
            call.args[0].value && call.args[1].value) {
            llvm::Value* idx = lowerExpr(*call.args[0].value);
            llvm::Value* v   = lowerExpr(*call.args[1].value);
            if (!idx || !v) return nullptr;
            idx = builder_.CreateSExt(idx, i64());
            llvm::AllocaInst* slot = entryAlloca(v->getType());
            builder_.CreateStore(v, slot);
            auto* fn = declareC("bz_array_insert_at", llvm::Type::getVoidTy(llvm_),
                                {ptrTy(), i64(), ptrTy(), i64()});
            builder_.CreateCall(fn, {recv, idx, slot, builder_.getInt64(esz)});
            return llvm::ConstantInt::get(i32(), 0);  // unit/void result
        }
        // .Get(i): T / .Peek(): T (top/front element)
        if ((m == "Get" || m == "Peek") &&
            (call.args.size() == 1 || call.args.empty())) {
            llvm::Value* idx = call.args.empty()
                ? nullptr : lowerExpr(*call.args[0].value);
            if (!call.args.empty() && !idx) return nullptr;
            if (call.args.empty()) {
                // Peek: last element (Stack) or first (Queue)
                auto* lenFn = declareC("bz_array_len", i64(), {ptrTy()});
                llvm::Value* len = builder_.CreateCall(lenFn, {recv});
                idx = builder_.CreateSub(len, builder_.getInt64(1));
            } else {
                idx = builder_.CreateSExt(idx, i64());
            }
            // Determine return type from generic arg
            llvm::Type* retTy = i32();
            if (recvTy && !recvTy->genericArgs.empty() && recvTy->genericArgs[0])
                retTy = lowerTypeRef(recvTy->genericArgs[0]);
            llvm::AllocaInst* out = entryAlloca(retTy);
            auto* fn = declareC("bz_array_get", llvm::Type::getVoidTy(llvm_),
                                {ptrTy(), i64(), ptrTy(), i64()});
            builder_.CreateCall(fn, {recv, idx, out, builder_.getInt64(esz)});
            return builder_.CreateLoad(retTy, out, "arr.elem");
        }
        // .Set(i, v)
        if (m == "Set" && call.args.size() == 2 &&
            call.args[0].value && call.args[1].value) {
            llvm::Value* idx = lowerExpr(*call.args[0].value);
            llvm::Value* v   = lowerExpr(*call.args[1].value);
            if (!idx || !v) return nullptr;
            idx = builder_.CreateSExt(idx, i64());
            llvm::AllocaInst* slot = entryAlloca(v->getType());
            builder_.CreateStore(v, slot);
            auto* fn = declareC("bz_array_set", llvm::Type::getVoidTy(llvm_),
                                {ptrTy(), i64(), ptrTy(), i64()});
            builder_.CreateCall(fn, {recv, idx, slot, builder_.getInt64(esz)});
            return llvm::ConstantInt::get(i32(), 0);
        }
        // .RemoveAt(i) / .Pop(): T / .Dequeue(): T
        if (m == "RemoveAt" && call.args.size() == 1 && call.args[0].value) {
            llvm::Value* idx = lowerExpr(*call.args[0].value);
            if (!idx) return nullptr;
            idx = builder_.CreateSExt(idx, i64());
            // #362 / stdlib §4.5: OwnedArray<T>.RemoveAt disposes the removed
            // element. Use the disposing variant for an owning class-element
            // array; a borrowing array just removes the slot.
            if (llvm::Function* elemDisp = owningCollectionElemDisposer(recvTy)) {
                auto* fn = declareC("bz_array_remove_at_disposing",
                    llvm::Type::getVoidTy(llvm_),
                    {ptrTy(), i64(), i64(), ptrTy()});
                builder_.CreateCall(fn, {recv, idx, builder_.getInt64(esz),
                                         elemDisp});
                return llvm::ConstantInt::get(i32(), 0);
            }
            auto* fn = declareC("bz_array_remove_at", llvm::Type::getVoidTy(llvm_),
                                {ptrTy(), i64(), i64()});
            builder_.CreateCall(fn, {recv, idx, builder_.getInt64(esz)});
            return llvm::ConstantInt::get(i32(), 0);
        }
        // per §4.4/§4.5: .Remove(item): bool — find the first matching element
        // and remove it; returns whether one was found. For an OwnedArray<T> the
        // matched (class) element is DISPOSED (and freed) via the element
        // disposer, mirroring RemoveAt; for a borrowing Array<T> the slot is
        // just removed. Match semantics (issue #400 / §25.2 — "collections use
        // Equals rather than ="): dispatched per element type by
        // emitRemoveElementEquality — a class with a user `Equals` override calls
        // it virtually (§8.4), a class without one uses reference identity (§8.3),
        // a record uses structural equality (§8.3), a string content (§4.6), a
        // value type `==`. The query `target` is a PURE BORROW — it is only
        // compared, never consumed or disposed by Remove (§4.5).
        if (m == "Remove" && call.args.size() == 1 && call.args[0].value) {
            llvm::Value* target = lowerExpr(*call.args[0].value);
            if (!target) return nullptr;
            llvm::Type* elemTy = getElemTy();
            bool elemIsClass = collectionElemIsClass(recvTy, 0);
            const sema::TypeRef* elemRef =
                (recvTy && !recvTy->genericArgs.empty()) ? recvTy->genericArgs[0]
                                                         : nullptr;
            auto* lenFn = declareC("bz_array_len", i64(), {ptrTy()});
            llvm::Value* len = builder_.CreateCall(lenFn, {recv});
            auto* idxSlot = entryAlloca(i64());
            builder_.CreateStore(builder_.getInt64(0), idxSlot);
            auto* condBB  = llvm::BasicBlock::Create(llvm_, "rmi.c", currentFn_);
            auto* bodyBB  = llvm::BasicBlock::Create(llvm_, "rmi.b", currentFn_);
            auto* foundBB = llvm::BasicBlock::Create(llvm_, "rmi.f", currentFn_);
            auto* nextBB  = llvm::BasicBlock::Create(llvm_, "rmi.n", currentFn_);
            auto* endBB   = llvm::BasicBlock::Create(llvm_, "rmi.e", currentFn_);
            builder_.CreateBr(condBB);
            builder_.SetInsertPoint(condBB);
            llvm::Value* idx = builder_.CreateLoad(i64(), idxSlot);
            builder_.CreateCondBr(builder_.CreateICmpSLT(idx, len), bodyBB, endBB);
            builder_.SetInsertPoint(bodyBB);
            auto* ptrFn = declareC("bz_array_ptr_at", ptrTy(), {ptrTy(), i64(), i64()});
            llvm::Value* ep = builder_.CreateCall(ptrFn, {recv, idx, builder_.getInt64(esz)});
            llvm::Value* el = builder_.CreateLoad(elemTy, ep);
            llvm::Value* eq =
                emitRemoveElementEquality(el, target, elemRef, elemIsClass);
            if (!eq) return nullptr;
            builder_.CreateCondBr(eq, foundBB, nextBB);
            builder_.SetInsertPoint(foundBB);
            // Remove (and, for an owning array, dispose) the matched slot.
            if (llvm::Function* elemDisp = owningCollectionElemDisposer(recvTy)) {
                auto* fn = declareC("bz_array_remove_at_disposing",
                    llvm::Type::getVoidTy(llvm_),
                    {ptrTy(), i64(), i64(), ptrTy()});
                builder_.CreateCall(fn, {recv, idx, builder_.getInt64(esz),
                                         elemDisp});
            } else {
                auto* fn = declareC("bz_array_remove_at",
                    llvm::Type::getVoidTy(llvm_), {ptrTy(), i64(), i64()});
                builder_.CreateCall(fn, {recv, idx, builder_.getInt64(esz)});
            }
            builder_.CreateBr(endBB);
            builder_.SetInsertPoint(nextBB);
            builder_.CreateStore(builder_.CreateAdd(idx, builder_.getInt64(1)), idxSlot);
            builder_.CreateBr(condBB);
            builder_.SetInsertPoint(endBB);
            auto* phi = builder_.CreatePHI(i1(), 2, "rmi.r");
            phi->addIncoming(builder_.getTrue(), foundBB);
            phi->addIncoming(builder_.getFalse(), condBB);
            return phi;
        }
        if ((m == "Pop" || m == "Dequeue") && call.args.empty()) {
            // Get last (Pop) or first (Dequeue) element, then remove it
            auto* lenFn = declareC("bz_array_len", i64(), {ptrTy()});
            llvm::Value* len = builder_.CreateCall(lenFn, {recv});
            llvm::Value* idx = m == "Pop"
                ? builder_.CreateSub(len, builder_.getInt64(1))
                : builder_.getInt64(0);
            llvm::Type* retTy = i32();
            if (recvTy && !recvTy->genericArgs.empty() && recvTy->genericArgs[0])
                retTy = lowerTypeRef(recvTy->genericArgs[0]);
            llvm::AllocaInst* out = entryAlloca(retTy);
            auto* getFn = declareC("bz_array_get", llvm::Type::getVoidTy(llvm_),
                                   {ptrTy(), i64(), ptrTy(), i64()});
            builder_.CreateCall(getFn, {recv, idx, out, builder_.getInt64(esz)});
            auto* remFn = declareC("bz_array_remove_at", llvm::Type::getVoidTy(llvm_),
                                   {ptrTy(), i64(), i64()});
            builder_.CreateCall(remFn, {recv, idx, builder_.getInt64(esz)});
            return builder_.CreateLoad(retTy, out, "arr.pop");
        }
        // .Clear()
        if (m == "Clear" && call.args.empty()) {
            // #362 / stdlib §4.5: OwnedArray<T>.Clear disposes every element.
            // Use the disposing variant for an owning class-element array; a
            // borrowing array just resets the count.
            if (llvm::Function* elemDisp = owningCollectionElemDisposer(recvTy)) {
                auto* fn = declareC("bz_array_clear_disposing",
                    llvm::Type::getVoidTy(llvm_), {ptrTy(), ptrTy()});
                builder_.CreateCall(fn, {recv, elemDisp});
                return llvm::ConstantInt::get(i32(), 0);
            }
            auto* fn = declareC("bz_array_clear", llvm::Type::getVoidTy(llvm_),
                                {ptrTy()});
            builder_.CreateCall(fn, {recv});
            return llvm::ConstantInt::get(i32(), 0);
        }
        // .DisposeElements() — stdlib §4.5: dispose each class-typed element,
        // then clear. For value-only element types it is equivalent to Clear().
        // Unlike Add/Clear on an OWNING array, this is the explicit en-masse
        // disposal a BORROWING Array<T> exposes; the disposer thunk is keyed on
        // the element being a class, not on the collection being owning.
        if (m == "DisposeElements" && call.args.empty()) {
            if (collectionElemIsClass(recvTy, 0)) {
                auto* fn = declareC("bz_array_clear_disposing",
                    llvm::Type::getVoidTy(llvm_), {ptrTy(), ptrTy()});
                builder_.CreateCall(fn, {recv, elementDisposerThunk()});
                return llvm::ConstantInt::get(i32(), 0);
            }
            auto* fn = declareC("bz_array_clear", llvm::Type::getVoidTy(llvm_),
                                {ptrTy()});
            builder_.CreateCall(fn, {recv});
            return llvm::ConstantInt::get(i32(), 0);
        }
        // .IsEmpty(): bool
        if (m == "IsEmpty" && call.args.empty()) {
            auto* lenFn = declareC("bz_array_len", i64(), {ptrTy()});
            llvm::Value* len = builder_.CreateCall(lenFn, {recv});
            return builder_.CreateICmpEQ(len, builder_.getInt64(0), "arr.empty");
        }
        // per §3: .AddRange(other: Array<T>) — append all elements from another array
        if (m == "AddRange" && call.args.size() == 1 && call.args[0].value) {
            llvm::Value* other = lowerExpr(*call.args[0].value);
            if (!other) return nullptr;
            llvm::Type* elemTy = getElemTy();
            auto* lenFn = declareC("bz_array_len", i64(), {ptrTy()});
            llvm::Value* otherLen = builder_.CreateCall(lenFn, {other});
            auto* idxSlot = entryAlloca(i64());
            builder_.CreateStore(builder_.getInt64(0), idxSlot);
            auto* condBB = llvm::BasicBlock::Create(llvm_, "ar.c", currentFn_);
            auto* bodyBB = llvm::BasicBlock::Create(llvm_, "ar.b", currentFn_);
            auto* endBB  = llvm::BasicBlock::Create(llvm_, "ar.e", currentFn_);
            builder_.CreateBr(condBB);
            builder_.SetInsertPoint(condBB);
            llvm::Value* idx = builder_.CreateLoad(i64(), idxSlot);
            builder_.CreateCondBr(builder_.CreateICmpSLT(idx, otherLen), bodyBB, endBB);
            builder_.SetInsertPoint(bodyBB);
            auto* ptrFn = declareC("bz_array_ptr_at", ptrTy(), {ptrTy(), i64(), i64()});
            llvm::Value* ep = builder_.CreateCall(ptrFn, {other, idx, builder_.getInt64(esz)});
            auto* addFn = declareC("bz_array_add", llvm::Type::getVoidTy(llvm_),
                                   {ptrTy(), ptrTy(), i64()});
            builder_.CreateCall(addFn, {recv, ep, builder_.getInt64(esz)});
            builder_.CreateStore(builder_.CreateAdd(idx, builder_.getInt64(1)), idxSlot);
            builder_.CreateBr(condBB);
            builder_.SetInsertPoint(endBB);
            return llvm::ConstantInt::get(i32(), 0);
        }
        // per §3: .Contains(item: T): bool (for int/value types only)
        if (m == "Contains" && call.args.size() == 1 && call.args[0].value) {
            llvm::Value* target = lowerExpr(*call.args[0].value);
            if (!target) return nullptr;
            llvm::Type* elemTy = getElemTy();
            auto* lenFn = declareC("bz_array_len", i64(), {ptrTy()});
            llvm::Value* len = builder_.CreateCall(lenFn, {recv});
            auto* idxSlot = entryAlloca(i64());
            builder_.CreateStore(builder_.getInt64(0), idxSlot);
            auto* condBB = llvm::BasicBlock::Create(llvm_, "con.c", currentFn_);
            auto* bodyBB = llvm::BasicBlock::Create(llvm_, "con.b", currentFn_);
            auto* foundBB = llvm::BasicBlock::Create(llvm_, "con.f", currentFn_);
            auto* nextBB = llvm::BasicBlock::Create(llvm_, "con.n", currentFn_);
            auto* endBB  = llvm::BasicBlock::Create(llvm_, "con.e", currentFn_);
            builder_.CreateBr(condBB);
            builder_.SetInsertPoint(condBB);
            llvm::Value* idx = builder_.CreateLoad(i64(), idxSlot);
            builder_.CreateCondBr(builder_.CreateICmpSLT(idx, len), bodyBB, endBB);
            builder_.SetInsertPoint(bodyBB);
            auto* ptrFn = declareC("bz_array_ptr_at", ptrTy(), {ptrTy(), i64(), i64()});
            llvm::Value* ep = builder_.CreateCall(ptrFn, {recv, idx, builder_.getInt64(esz)});
            llvm::Value* el = builder_.CreateLoad(elemTy, ep);
            // Compare: for integers use icmp eq; for pointers use string eq
            llvm::Value* eq;
            if (el->getType()->isPointerTy() && target->getType()->isPointerTy()) {
                eq = emitStringEq(el, target);  // string comparison
            } else {
                llvm::Value* elNorm = el, *tNorm = target;
                if (el->getType() != target->getType()) {
                    if (el->getType()->isIntegerTy() && target->getType()->isIntegerTy()) {
                        unsigned ew = el->getType()->getIntegerBitWidth();
                        unsigned tw = target->getType()->getIntegerBitWidth();
                        llvm::Type* wide = ew >= tw ? el->getType() : target->getType();
                        elNorm = builder_.CreateSExt(el, wide);
                        tNorm = builder_.CreateSExt(target, wide);
                    }
                }
                eq = builder_.CreateICmpEQ(elNorm, tNorm);
            }
            builder_.CreateCondBr(eq, foundBB, nextBB);
            builder_.SetInsertPoint(foundBB);
            builder_.CreateBr(endBB);
            builder_.SetInsertPoint(nextBB);
            builder_.CreateStore(builder_.CreateAdd(idx, builder_.getInt64(1)), idxSlot);
            builder_.CreateBr(condBB);
            builder_.SetInsertPoint(endBB);
            auto* phiCon = builder_.CreatePHI(i1(), 2, "con.r");
            phiCon->addIncoming(builder_.getTrue(), foundBB);
            phiCon->addIncoming(builder_.getFalse(), condBB);
            freeLazyTemp();  // FINDING [5]: release materialized lazy snapshot
            return phiCon;
        }

        // FINDING [5]: if the receiver was a materialized lazy snapshot but no
        // terminal handler above claimed the call (an arg shape we don't model),
        // release the temp before deferring so it does not leak. A concrete-array
        // receiver (lazyTempArr == null) falls through unchanged.
        if (lazyTempArr) { freeLazyTemp(); return nullptr; }
    }

    // -----------------------------------------------------------------------
    // Dictionary<string, V> methods (string-key variant)
    // -----------------------------------------------------------------------
    if (isDict) {
        // issues #343 / D3 / §4.8: a non-string key routes through the generic-
        // key runtime (bz_dict_*_gen), passing a pointer to the lowered key
        // bytes; the STRING key keeps the bz_dict_*_str fast path. `keyTy` is
        // non-null for every generic-backed key (records and primitives/enums),
        // null only for the string key.
        const sema::TypeRef* keyTy =
            genericBackingType(recvTy, 0, /*isSet=*/false, nullptr);
        bool keyIsRecord = keyTy && recordTypeOfRef(keyTy) != nullptr;
        // issue D4 / §4.8: V is unconstrained for the borrowing dictionary, so
        // it can be a record (aggregate). A record value is too wide for the
        // inttoptr value-slot encoding; it is heap-boxed and the box pointer is
        // stored, then loaded back on retrieval.
        const sema::TypeRef* valTy =
            (recvTy && recvTy->genericArgs.size() > 1) ? recvTy->genericArgs[1]
                                                       : nullptr;
        // Any record value (equality-bearing or not) is an aggregate and must be
        // boxed — not just the equality-bearing ones recordTypeOfRef accepts.
        const sema::TypeSymbol* valRec =
            (valTy && valTy->kind == sema::TypeRef::Kind::Named &&
             valTy->declaration &&
             valTy->declaration->kind == ast::TypeDeclKind::Record)
                ? valTy->declaration : nullptr;

        // Helper: extract string data/len from bz.string ptr
        auto strDataLen = [&](llvm::Value* s) -> std::pair<llvm::Value*, llvm::Value*> {
            llvm::Value* data = loadStringData(s);
            llvm::Value* len  = loadStringLen(s);
            return {data, len};
        };
        // issues #343 / D3: spill a generic key value to a stack slot and return
        // the byte pointer the generic runtime entry points expect. A record key
        // reaching us as a pointer is already an address; any other value
        // (scalar/enum) is stored into a slot of its natural LLVM type.
        auto genKeyPtr = [&](llvm::Value* k) -> llvm::Value* {
            if (keyIsRecord && k->getType()->isPointerTy())
                return k;  // record already spilled to an address
            llvm::AllocaInst* slot = entryAlloca(k->getType());
            builder_.CreateStore(k, slot);
            return slot;
        };
        // Encode a value as the stored void*. Pointer values store directly;
        // integer/float values widen to i64 and inttoptr so each stored value is
        // self-contained (no shared-alloca aliasing across loop iterations);
        // issue D4: a record value is heap-boxed (malloc + memcpy of its bytes)
        // and the box pointer is stored.
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

        // .Count: int
        if (m == "Count" && call.args.empty()) {
            auto* fn = declareC("bz_dict_count", i64(), {ptrTy()});
            llvm::Value* c = builder_.CreateCall(fn, {recv});
            return builder_.CreateTrunc(c, i32());
        }
        // §4.8 / §4.9: .Clear() — empty the table in place (it stays allocated;
        // Count is 0 afterward). The value-disposal obligation mirrors the
        // scope-end Dispose exactly (bz_dict_free in LlvmBackend_Setup.cpp):
        //   * OwnedDictionary<K, class>  → dispose+free every surviving value via
        //     its own vtable (owningDictValueDisposer): "Clear(); // disposes
        //     every value" (§4.9). Each value is disposed EXACTLY ONCE — a value
        //     displaced earlier by the indexer setter (#388) was already removed
        //     and disposed, so it is not in the table here (no double-free).
        //   * borrowing Dictionary<K, record> → free each value's heap box
        //     (dictValueBoxFreer; the box is value-storage, nothing to dispose
        //     inside — §4.8 Dictionary borrows, it does not own values).
        //   * everything else (scalar/string value, borrowing class value) → no
        //     value disposal (a borrowing Dictionary owns nothing; §4.8). The
        //     runtime always frees each key's byte copy regardless.
        // bz_dict_clear handles BOTH the string-key and generic-key tables (it
        // frees e.key — a strndup'd copy in string mode, a malloc'd byte copy in
        // generic mode — for every live entry).
        if (m == "Clear" && call.args.empty()) {
            llvm::Function* disp = owningDictValueDisposer(recvTy);
            if (!disp) disp = dictValueBoxFreer(recvTy);
            llvm::Value* dispV = disp
                ? llvm::cast<llvm::Value>(disp)
                : llvm::ConstantPointerNull::get(ptrTy());
            auto* fn = declareC("bz_dict_clear", llvm::Type::getVoidTy(llvm_),
                                {ptrTy(), ptrTy()});
            builder_.CreateCall(fn, {recv, dispV});
            return llvm::ConstantInt::get(i32(), 0);  // unit result
        }
        // .ContainsKey(k) / .Contains(k) (alias, §4.8): bool
        if ((m == "ContainsKey" || m == "Contains") &&
            call.args.size() == 1 && call.args[0].value) {
            llvm::Value* k = lowerExpr(*call.args[0].value);
            if (!k) return nullptr;
            if (keyTy) {
                auto* fn = declareC("bz_dict_contains_gen", i1(), {ptrTy(), ptrTy()});
                return builder_.CreateCall(fn, {recv, genKeyPtr(k)}, "dict.contains");
            }
            auto [kd, kl] = strDataLen(k);
            auto* fn = declareC("bz_dict_contains_str", i1(), {ptrTy(), ptrTy(), i64()});
            return builder_.CreateCall(fn, {recv, kd, kl}, "dict.contains");
        }
        // .Add(k, v) / .Set(k, v)
        if ((m == "Add" || m == "Set") && call.args.size() == 2 &&
            call.args[0].value && call.args[1].value) {
            llvm::Value* k = lowerExpr(*call.args[0].value);
            llvm::Value* v = lowerExpr(*call.args[1].value);
            if (!k || !v) return nullptr;
            llvm::Value* vPtr = encodeValue(v);
            if (keyTy) {
                auto* fn = declareC("bz_dict_set_gen", llvm::Type::getVoidTy(llvm_),
                                    {ptrTy(), ptrTy(), ptrTy()});
                builder_.CreateCall(fn, {recv, genKeyPtr(k), vPtr});
                return llvm::ConstantInt::get(i32(), 0);
            }
            auto [kd, kl] = strDataLen(k);
            auto* fn = declareC("bz_dict_set_str", llvm::Type::getVoidTy(llvm_),
                                {ptrTy(), ptrTy(), i64(), ptrTy()});
            builder_.CreateCall(fn, {recv, kd, kl, vPtr});
            return llvm::ConstantInt::get(i32(), 0);
        }
        // .Remove(k): bool
        if (m == "Remove" && call.args.size() == 1 && call.args[0].value) {
            llvm::Value* k = lowerExpr(*call.args[0].value);
            if (!k) return nullptr;
            if (keyTy) {
                auto* fn = declareC("bz_dict_remove_gen", i1(), {ptrTy(), ptrTy()});
                return builder_.CreateCall(fn, {recv, genKeyPtr(k)}, "dict.remove");
            }
            auto [kd, kl] = strDataLen(k);
            auto* fn = declareC("bz_dict_remove_str", i1(), {ptrTy(), ptrTy(), i64()});
            return builder_.CreateCall(fn, {recv, kd, kl}, "dict.remove");
        }
        // .TryGetValue(k, out v): bool
        // (Note: the out-var slot is pre-created by ensureOutVarSlots)
        if (m == "TryGetValue" && call.args.size() == 2 &&
            call.args[0].value && call.args[1].value) {
            llvm::Value* k = lowerExpr(*call.args[0].value);
            if (!k) return nullptr;
            // Get the address of the out parameter
            llvm::Type* outTy = nullptr;
            llvm::Value* outAddr = lvalueAddr(*call.args[1].value, outTy);
            if (!outAddr) return nullptr;
            // bz_dict_try_get_* writes the stored void* into *tmpSlot.
            // For pointer values: the void* IS the string/object pointer.
            // For integer/float values: the void* encodes the value via
            // inttoptr (mirroring the Add encoding above). Decode via ptrtoint.
            // issue D4: for a record value the void* is the heap box; copy the
            // record bytes from the box into the out slot.
            auto* tmpSlot = entryAlloca(ptrTy());
            llvm::Value* found;
            if (keyTy) {
                auto* fn = declareC("bz_dict_try_get_gen", i1(),
                                    {ptrTy(), ptrTy(), ptrTy()});
                found = builder_.CreateCall(fn, {recv, genKeyPtr(k), tmpSlot});
            } else {
                auto [kd, kl] = strDataLen(k);
                auto* fn = declareC("bz_dict_try_get_str", i1(),
                                    {ptrTy(), ptrTy(), i64(), ptrTy()});
                found = builder_.CreateCall(fn, {recv, kd, kl, tmpSlot});
            }
            llvm::Value* valPtr = builder_.CreateLoad(ptrTy(), tmpSlot);
            if (valRec && outTy && outTy->isStructTy()) {
                // Load the boxed record and store it into the out slot. Guard the
                // box pointer (it is null on a not-found lookup) so we never
                // dereference null: only copy on the found path.
                auto* copyBB = llvm::BasicBlock::Create(llvm_, "tg.copy", currentFn_);
                auto* endBB  = llvm::BasicBlock::Create(llvm_, "tg.end", currentFn_);
                llvm::Value* nonNull = builder_.CreateICmpNE(
                    valPtr, llvm::ConstantPointerNull::get(ptrTy()), "tg.nn");
                builder_.CreateCondBr(nonNull, copyBB, endBB);
                builder_.SetInsertPoint(copyBB);
                llvm::Value* recVal = builder_.CreateLoad(outTy, valPtr, "tg.rec");
                builder_.CreateStore(recVal, outAddr);
                builder_.CreateBr(endBB);
                builder_.SetInsertPoint(endBB);
            } else if (outTy && !outTy->isPointerTy()) {
                // Decode: ptrtoint → truncate/bitcast to target type
                llvm::Value* asI64 = builder_.CreatePtrToInt(valPtr, i64());
                llvm::Value* decoded = asI64;
                if (outTy->isFloatingPointTy()) {
                    if (outTy->isDoubleTy())
                        decoded = builder_.CreateBitCast(asI64, outTy);
                    else {
                        llvm::Value* trunc = builder_.CreateTrunc(asI64, i32());
                        decoded = builder_.CreateBitCast(trunc, outTy);
                    }
                } else if (!outTy->isIntegerTy(64)) {
                    decoded = builder_.CreateTrunc(asI64, outTy);
                }
                builder_.CreateStore(decoded, outAddr);
            } else {
                builder_.CreateStore(valPtr, outAddr);
            }
            return found;
        }
        // .GetOrAdd(k, factory): V  (stdlib §4.8 borrowing / §4.9 owning)
        //   Dictionary<K,V>:       factory is (K) -> V
        //   OwnedDictionary<K,V>:  factory is (K) -> owned V, result is a BORROW
        //
        // Semantics (§4.8 / §4.9): if `k` is present, return the existing value
        // and DO NOT invoke the factory; otherwise invoke factory(k) to produce a
        // V, store it under `k` (the same insert path Add/Set uses — record values
        // heap-boxed, class pointers stored directly), and return that value.
        //
        // Ownership (the cardinal risk, §28.3 / §28.8):
        //   * OwnedDictionary MISS: the factory yields an `owned V` (+1 class
        //     pointer); we STORE it (the dict takes ownership, exactly as
        //     `Add(owned)` / the indexer set does — encodeValue stores the pointer
        //     directly, bz_dict_set_* records it) and RETURN A BORROW = the stored
        //     slot read back (NOT a transfer; the dict still owns it and disposes
        //     it once at scope-end via its #394 disposal cascade). producerState
        //     in the ownership checker classifies the GetOrAdd result as Borrowed
        //     (no resolved `owned` return), so the caller gets no disposal
        //     obligation — no double-free.
        //   * OwnedDictionary HIT: the factory is NOT invoked (the call is in the
        //     miss branch only), so no owned V is produced — nothing leaks. The
        //     existing slot's value is returned as a borrow.
        //   * Borrowing Dictionary: V is a value/record/string; the factory yields
        //     a V which we store on the same insert path Add/Set uses; the return
        //     is the slot value read back (a copy for records/scalars, the pointer
        //     for strings/objects). No owned semantics either way.
        //
        // Lowering = TryGet, then conditionally (invoke factory + store) on the
        // miss path, then decode the slot value on BOTH paths and PHI-merge. The
        // factory closure is lowered ONCE up front (constructing the {fn,env}
        // closure value has no side effects — the factory BODY only runs when the
        // closure is called, which happens in the miss branch only).
        if (m == "GetOrAdd" && call.args.size() == 2 &&
            call.args[0].value && call.args[1].value) {
            // Result LLVM type = V (the value type, genericArgs[1]).
            llvm::Type* valLlvm = valTy ? lowerTypeRef(valTy) : ptrTy();
            if (!valLlvm) valLlvm = ptrTy();

            // Decode a stored void* slot value into the V LLVM type — identical to
            // the TryGetValue / indexer-get decode (record → load the boxed struct;
            // scalar → ptrtoint then truncate/bitcast; pointer → the void* IS the
            // string/object pointer). `slotV` is the raw stored void*.
            auto decodeSlot = [&](llvm::Value* slotV) -> llvm::Value* {
                if (valRec && valLlvm->isStructTy())
                    return builder_.CreateLoad(valLlvm, slotV, "goa.rec");
                if (!valLlvm->isPointerTy()) {
                    llvm::Value* asI64 = builder_.CreatePtrToInt(slotV, i64());
                    if (valLlvm->isDoubleTy())
                        return builder_.CreateBitCast(asI64, valLlvm);
                    if (valLlvm->isFloatTy())
                        return builder_.CreateBitCast(
                            builder_.CreateTrunc(asI64, i32()), valLlvm);
                    if (valLlvm->isIntegerTy(64)) return asI64;
                    return builder_.CreateTrunc(asI64, valLlvm);
                }
                return slotV;  // pointer value (string / object)
            };

            // Lower the key once, and spill it for the generic-key path.
            llvm::Value* k = lowerExpr(*call.args[0].value);
            if (!k) return nullptr;
            llvm::Value* genKey = keyTy ? genKeyPtr(k) : nullptr;
            llvm::Value* strKd = nullptr, *strKl = nullptr;
            if (!keyTy) { auto p = strDataLen(k); strKd = p.first; strKl = p.second; }

            // Lower the factory closure (K) -> V. Build a synthetic Function
            // TypeRef so an unannotated lambda param/return picks up the right
            // LLVM type (mirrors the LINQ lowerLinqLambda helper). The closure is
            // a { ptr fn, ptr env } struct value; calling it is fn(env, key).
            auto* funcRef = ctx_.makeTypeRef();
            funcRef->kind = sema::TypeRef::Kind::Function;
            funcRef->funcReturn = valTy ? const_cast<sema::TypeRef*>(valTy)
                                        : nullptr;
            if (recvTy && !recvTy->genericArgs.empty() &&
                recvTy->genericArgs[0]) {
                sema::TypeRef::FuncParam fp;
                fp.type = recvTy->genericArgs[0];   // K — the factory's parameter
                funcRef->funcParams.push_back(std::move(fp));
            }
            const sema::TypeRef* savedExpected = expectedFunctionType_;
            expectedFunctionType_ = funcRef;
            llvm::Value* closure = lowerExpr(*call.args[1].value);
            expectedFunctionType_ = savedExpected;
            if (!closure) return nullptr;
            // #422: a closure produced INLINE here (a lambda literal) is a fresh
            // transient with rc=1 that we own and must release after use; a closure
            // arriving via a variable/field is borrowed (not ours to free). Only
            // release the inline-lambda form.
            const bool factoryIsInlineLambda =
                dynamic_cast<const ast::LambdaExpr*>(call.args[1].value.get())
                    != nullptr;

            // First TryGet: is the key already present?
            auto* tmpSlot = entryAlloca(ptrTy());
            llvm::Value* found;
            if (keyTy) {
                auto* fn = declareC("bz_dict_try_get_gen", i1(),
                                    {ptrTy(), ptrTy(), ptrTy()});
                found = builder_.CreateCall(fn, {recv, genKey, tmpSlot});
            } else {
                auto* fn = declareC("bz_dict_try_get_str", i1(),
                                    {ptrTy(), ptrTy(), i64(), ptrTy()});
                found = builder_.CreateCall(fn, {recv, strKd, strKl, tmpSlot});
            }

            auto* missBB = llvm::BasicBlock::Create(llvm_, "goa.miss", currentFn_);
            auto* hitBB  = llvm::BasicBlock::Create(llvm_, "goa.hit",  currentFn_);
            auto* endBB  = llvm::BasicBlock::Create(llvm_, "goa.end",  currentFn_);
            builder_.CreateCondBr(found, hitBB, missBB);

            // HIT: decode the already-stored slot value. The factory is NOT called.
            builder_.SetInsertPoint(hitBB);
            llvm::Value* hitSlot = builder_.CreateLoad(ptrTy(), tmpSlot, "goa.hslot");
            llvm::Value* hitVal = decodeSlot(hitSlot);
            llvm::BasicBlock* hitEnd = builder_.GetInsertBlock();
            builder_.CreateBr(endBB);

            // MISS: invoke factory(k) → V, encode + store it (the dict takes
            // ownership for the owning case, exactly as Add(owned)/indexer-set do),
            // then re-read the stored slot and decode → the borrow-return value.
            builder_.SetInsertPoint(missBB);
            llvm::Value* fnPtr = builder_.CreateLoad(ptrTy(), closure, "goa.fn");
            llvm::Value* envSlot = builder_.CreateGEP(ptrTy(), closure,
                                                      {builder_.getInt32(1)});
            llvm::Value* envPtr = builder_.CreateLoad(ptrTy(), envSlot, "goa.env");
            auto* facTy = llvm::FunctionType::get(valLlvm, {ptrTy(), k->getType()},
                                                  false);
            llvm::Value* produced =
                builder_.CreateCall(facTy, fnPtr, {envPtr, k}, "goa.fac");
            llvm::Value* encoded = encodeValue(produced);
            if (keyTy) {
                auto* fn = declareC("bz_dict_set_gen", llvm::Type::getVoidTy(llvm_),
                                    {ptrTy(), ptrTy(), ptrTy()});
                builder_.CreateCall(fn, {recv, genKey, encoded});
            } else {
                auto* fn = declareC("bz_dict_set_str", llvm::Type::getVoidTy(llvm_),
                                    {ptrTy(), ptrTy(), i64(), ptrTy()});
                builder_.CreateCall(fn, {recv, strKd, strKl, encoded});
            }
            // Re-read the now-stored slot so the returned value is the BORROW of the
            // stored slot (mirrors the indexer-get-borrow; for a record value this
            // reads the freshly-stored box, for a class value the stored pointer).
            auto* tmpSlot2 = entryAlloca(ptrTy());
            if (keyTy) {
                auto* fn = declareC("bz_dict_try_get_gen", i1(),
                                    {ptrTy(), ptrTy(), ptrTy()});
                builder_.CreateCall(fn, {recv, genKey, tmpSlot2});
            } else {
                auto* fn = declareC("bz_dict_try_get_str", i1(),
                                    {ptrTy(), ptrTy(), i64(), ptrTy()});
                builder_.CreateCall(fn, {recv, strKd, strKl, tmpSlot2});
            }
            llvm::Value* missSlot = builder_.CreateLoad(ptrTy(), tmpSlot2, "goa.mslot");
            llvm::Value* missVal = decodeSlot(missSlot);
            llvm::BasicBlock* missEnd = builder_.GetInsertBlock();
            builder_.CreateBr(endBB);

            // Merge the two paths.
            builder_.SetInsertPoint(endBB);
            llvm::PHINode* phi = builder_.CreatePHI(valLlvm, 2, "goa.r");
            phi->addIncoming(hitVal, hitEnd);
            phi->addIncoming(missVal, missEnd);
            // #422: the factory closure, when produced INLINE (a lambda literal),
            // is a transient temporary we produced with rc=1 (the producer
            // reference); both paths are done using it here, so release it (frees
            // the cell + its env; captures are borrows, never freed — §28.9). On
            // the HIT path the closure was never called, but it was still
            // allocated and must be reclaimed. Null-safe.
            if (factoryIsInlineLambda) emitClosureRelease(closure);
            return phi;
        }
    }

    // ---------------------------------------------------------------------------
    // StringBuilder — bz_array_t of string pointers (parts list).
    // Append adds a string part; ToString concatenates all parts.
    // ---------------------------------------------------------------------------
    if (isStrBld) {
        constexpr int64_t SB_ESZ = 8;  // pointer-sized elements (string ptrs)

        // Helper: coerce value to string
        auto toStr = [&](llvm::Value* v, const ast::Expr* expr) -> llvm::Value* {
            if (!v) return nullptr;
            auto* rt = expr ? ctx_.exprType(*expr) : nullptr;
            if (rt && rt->kind == sema::TypeRef::Kind::Primitive &&
                rt->primitive != sema::PrimitiveType::String)
                return emitPrimitiveToString(v, rt->primitive);
            if (v->getType()->isPointerTy()) return v;
            // Fallback: format as integer
            llvm::Value* iv = v->getType()->isIntegerTy(64)
                ? v : builder_.CreateSExt(v, i64());
            return emitPrimitiveToString(iv, sema::PrimitiveType::Long);
        };

        if ((m == "Append" || m == "AppendLine") && !call.args.empty()) {
            llvm::Value* s = nullptr;
            if (call.args[0].value) {
                s = toStr(lowerExpr(*call.args[0].value), call.args[0].value.get());
            }
            if (s) {
                llvm::AllocaInst* slot = entryAlloca(ptrTy());
                builder_.CreateStore(s, slot);
                auto* fn = declareC("bz_array_add", llvm::Type::getVoidTy(llvm_),
                                    {ptrTy(), ptrTy(), i64()});
                builder_.CreateCall(fn, {recv, slot, builder_.getInt64(SB_ESZ)});
            }
            if (m == "AppendLine") {
                llvm::Value* nl = lowerStringLiteral("\"\n\"", false);
                llvm::AllocaInst* nlSlot = entryAlloca(ptrTy());
                builder_.CreateStore(nl, nlSlot);
                auto* fn = declareC("bz_array_add", llvm::Type::getVoidTy(llvm_),
                                    {ptrTy(), ptrTy(), i64()});
                builder_.CreateCall(fn, {recv, nlSlot, builder_.getInt64(SB_ESZ)});
            }
            return recv;  // allow chaining
        }
        if (m == "AppendLine" && call.args.empty()) {
            llvm::Value* nl = lowerStringLiteral("\"\n\"", false);
            llvm::AllocaInst* nlSlot = entryAlloca(ptrTy());
            builder_.CreateStore(nl, nlSlot);
            auto* fn = declareC("bz_array_add", llvm::Type::getVoidTy(llvm_),
                                {ptrTy(), ptrTy(), i64()});
            builder_.CreateCall(fn, {recv, nlSlot, builder_.getInt64(SB_ESZ)});
            return recv;
        }
        if (m == "Clear" && call.args.empty()) {
            auto* fn = declareC("bz_array_clear", llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(fn, {recv});
            return recv;
        }
        // §3.1: Insert(index: int, value: string): StringBuilder — flatten,
        // insert, reset to one part (runtime helper). Returns this for chaining.
        if (m == "Insert" && call.args.size() == 2 &&
            call.args[0].value && call.args[1].value) {
            llvm::Value* idx = lowerExpr(*call.args[0].value);
            llvm::Value* val = lowerExpr(*call.args[1].value);
            if (!idx || !val) return nullptr;
            llvm::Value* idx64 = idx->getType()->isIntegerTy(64)
                ? idx : builder_.CreateSExt(idx, i64());
            auto* fn = declareC("bz_sb_insert", llvm::Type::getVoidTy(llvm_),
                                {ptrTy(), i64(), ptrTy()});
            builder_.CreateCall(fn, {recv, idx64, val});
            return recv;
        }
        // §3.1: Remove(startIndex: int, length: int): StringBuilder.
        if (m == "Remove" && call.args.size() == 2 &&
            call.args[0].value && call.args[1].value) {
            llvm::Value* start = lowerExpr(*call.args[0].value);
            llvm::Value* len   = lowerExpr(*call.args[1].value);
            if (!start || !len) return nullptr;
            llvm::Value* start64 = start->getType()->isIntegerTy(64)
                ? start : builder_.CreateSExt(start, i64());
            llvm::Value* len64 = len->getType()->isIntegerTy(64)
                ? len : builder_.CreateSExt(len, i64());
            auto* fn = declareC("bz_sb_remove", llvm::Type::getVoidTy(llvm_),
                                {ptrTy(), i64(), i64()});
            builder_.CreateCall(fn, {recv, start64, len64});
            return recv;
        }
        // §3.1: Replace(oldValue: string, newValue: string): StringBuilder.
        if (m == "Replace" && call.args.size() == 2 &&
            call.args[0].value && call.args[1].value) {
            llvm::Value* oldV = lowerExpr(*call.args[0].value);
            llvm::Value* newV = lowerExpr(*call.args[1].value);
            if (!oldV || !newV) return nullptr;
            auto* fn = declareC("bz_sb_replace", llvm::Type::getVoidTy(llvm_),
                                {ptrTy(), ptrTy(), ptrTy()});
            builder_.CreateCall(fn, {recv, oldV, newV});
            return recv;
        }
        if (m == "Length" || m == "Count") {
            // Sum of lengths of all string parts
            auto* lenFn = declareC("bz_array_len", i64(), {ptrTy()});
            auto* ptrAtFn = declareC("bz_array_ptr_at", ptrTy(), {ptrTy(), i64(), i64()});
            llvm::Value* nParts = builder_.CreateCall(lenFn, {recv});
            auto* sumSlot = entryAlloca(i64());
            builder_.CreateStore(builder_.getInt64(0), sumSlot);
            auto* idxSlot = entryAlloca(i64());
            builder_.CreateStore(builder_.getInt64(0), idxSlot);
            auto* condBB = llvm::BasicBlock::Create(llvm_, "sb.len.c", currentFn_);
            auto* bodyBB = llvm::BasicBlock::Create(llvm_, "sb.len.b", currentFn_);
            auto* endBB  = llvm::BasicBlock::Create(llvm_, "sb.len.e", currentFn_);
            builder_.CreateBr(condBB);
            builder_.SetInsertPoint(condBB);
            llvm::Value* idx = builder_.CreateLoad(i64(), idxSlot);
            builder_.CreateCondBr(builder_.CreateICmpSLT(idx, nParts), bodyBB, endBB);
            builder_.SetInsertPoint(bodyBB);
            llvm::Value* idx2 = builder_.CreateLoad(i64(), idxSlot);
            llvm::Value* ep = builder_.CreateCall(ptrAtFn, {recv, idx2, builder_.getInt64(SB_ESZ)});
            llvm::Value* part = builder_.CreateLoad(ptrTy(), ep);
            llvm::Value* plen = loadStringLen(part);
            llvm::Value* curSum = builder_.CreateLoad(i64(), sumSlot);
            builder_.CreateStore(builder_.CreateAdd(curSum, plen), sumSlot);
            builder_.CreateStore(builder_.CreateAdd(idx2, builder_.getInt64(1)), idxSlot);
            builder_.CreateBr(condBB);
            builder_.SetInsertPoint(endBB);
            llvm::Value* total = builder_.CreateLoad(i64(), sumSlot);
            return builder_.CreateTrunc(total, i32());
        }
        if (m == "ToString" && call.args.empty()) {
            // Use the runtime helper to concatenate all string parts.
            auto* outDataSlot = entryAlloca(ptrTy());
            auto* outLenSlot  = entryAlloca(i64());
            auto* fn = declareC("bz_strbuilder_to_string",
                                llvm::Type::getVoidTy(llvm_),
                                {ptrTy(), ptrTy(), ptrTy()});
            builder_.CreateCall(fn, {recv, outDataSlot, outLenSlot});
            llvm::Value* data = builder_.CreateLoad(ptrTy(), outDataSlot);
            llvm::Value* len  = builder_.CreateLoad(i64(), outLenSlot);
            // Build a bz.string object from the malloc'd buffer.
            // #422: allocate the full header size (now 32 bytes incl. rc), not
            // a hardcoded 24 — writing rc into field 3 of a 24-byte block would
            // overflow. getTypeAllocSize is the single source of truth.
            uint64_t sobjSz = module_->getDataLayout().getTypeAllocSize(stringType());
            auto* sobj = builder_.CreateCall(
                callocFn(), {builder_.getInt64(1), builder_.getInt64((int64_t)sobjSz)});
            builder_.CreateStore(data,
                builder_.CreateStructGEP(stringType(), sobj, 0));
            builder_.CreateStore(len,
                builder_.CreateStructGEP(stringType(), sobj, 1));
            builder_.CreateStore(builder_.getInt8(1),
                builder_.CreateStructGEP(stringType(), sobj, 2));
            initStringRc(sobj);  // #422: producer's reference, rc := 1
            return sobj;
        }
    }

    return nullptr;  // not a collection call
}


llvm::Value* Emitter::tryLowerBuiltinClassCall(const ast::CallExpr& call) {
    if (!call.callee) return nullptr;
    auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(call.callee.get());
    if (!ma || !ma->target) return nullptr;
    auto* id = dynamic_cast<const ast::IdentifierExpr*>(ma->target.get());
    if (!id) return nullptr;
    const auto cls = id->name;
    const auto& m = ma->member;

    // §2.3 Environment class methods.
    if (cls == "Environment") {
        // Environment.Exit(code: int) — terminate the process.
        if (m == "Exit" && call.args.size() == 1 && call.args[0].value) {
            llvm::Value* code = lowerExpr(*call.args[0].value);
            if (!code) return nullptr;
            // Declare exit(3): void exit(int)
            auto* exitFn = module_->getFunction("exit");
            if (!exitFn) {
                auto* ft = llvm::FunctionType::get(
                    llvm::Type::getVoidTy(llvm_), {i32()}, false);
                exitFn = llvm::Function::Create(
                    ft, llvm::Function::ExternalLinkage, "exit", module_.get());
                exitFn->addFnAttr(llvm::Attribute::NoReturn);
            }
            // Truncate to i32 if needed.
            if (!code->getType()->isIntegerTy(32))
                code = builder_.CreateTrunc(code, i32());
            builder_.CreateCall(exitFn, {code});
            // Create an unreachable block after the noreturn call.
            builder_.CreateUnreachable();
            auto* deadBB = llvm::BasicBlock::Create(llvm_, "exit.dead", currentFn_);
            builder_.SetInsertPoint(deadBB);
            return llvm::ConstantInt::get(i32(), 0);  // nominal value, never reached
        }
        // Environment.GetEnvironmentVariable(name): string? — getenv
        if (m == "GetEnvironmentVariable" && call.args.size() == 1 &&
            call.args[0].value) {
            llvm::Value* nameStr = lowerExpr(*call.args[0].value);
            if (!nameStr) return nullptr;
            llvm::Value* nameData = loadStringData(nameStr);
            auto* getenvFn = module_->getFunction("getenv");
            if (!getenvFn) {
                auto* ft = llvm::FunctionType::get(ptrTy(), {ptrTy()}, false);
                getenvFn = llvm::Function::Create(
                    ft, llvm::Function::ExternalLinkage, "getenv", module_.get());
            }
            llvm::Value* rawPtr = builder_.CreateCall(getenvFn, {nameData}, "getenv.ret");
            // null → return null (string?). Non-null: wrap in bz_string.
            auto* nullPtr = llvm::ConstantPointerNull::get(ptrTy());
            llvm::Value* isNull = builder_.CreateICmpEQ(rawPtr, nullPtr);
            auto* nullBB = llvm::BasicBlock::Create(llvm_, "env.null", currentFn_);
            auto* okBB   = llvm::BasicBlock::Create(llvm_, "env.ok",   currentFn_);
            auto* endBB  = llvm::BasicBlock::Create(llvm_, "env.end",  currentFn_);
            auto* entBB  = builder_.GetInsertBlock();
            builder_.CreateCondBr(isNull, endBB, okBB);
            builder_.SetInsertPoint(okBB);
            // strlen of the result.
            auto* strlenFn = module_->getFunction("strlen");
            if (!strlenFn) {
                auto* ft2 = llvm::FunctionType::get(i64(), {ptrTy()}, false);
                strlenFn = llvm::Function::Create(ft2, llvm::Function::ExternalLinkage, "strlen", module_.get());
            }
            llvm::Value* rawLen = builder_.CreateCall(strlenFn, {rawPtr});
            auto* mallocFn = declareC("malloc", ptrTy(), {i64()});
            auto* memcpyFn = declareC("memcpy", ptrTy(), {ptrTy(), ptrTy(), i64()});
            auto* i8Ty = llvm::Type::getInt8Ty(llvm_);
            llvm::Value* dataSz = builder_.CreateAdd(rawLen, builder_.getInt64(1));
            llvm::Value* dataCopy = builder_.CreateCall(mallocFn, {dataSz});
            builder_.CreateCall(memcpyFn, {dataCopy, rawPtr, rawLen});
            llvm::Value* end = builder_.CreateGEP(i8Ty, dataCopy, rawLen);
            builder_.CreateStore(llvm::ConstantInt::get(i8Ty, 0), end);
            uint64_t strBytes = module_->getDataLayout().getTypeAllocSize(stringType());
            llvm::Value* strObj = builder_.CreateCall(mallocFn, {builder_.getInt64(strBytes)});
            builder_.CreateStore(dataCopy, builder_.CreateStructGEP(stringType(), strObj, 0));
            builder_.CreateStore(rawLen, builder_.CreateStructGEP(stringType(), strObj, 1));
            builder_.CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(llvm_), 1),
                builder_.CreateStructGEP(stringType(), strObj, 2));
                initStringRc(strObj);  // #422: producer's reference, rc := 1
            auto* okEnd = builder_.GetInsertBlock();
            builder_.CreateBr(endBB);
            builder_.SetInsertPoint(endBB);
            auto* phi = builder_.CreatePHI(ptrTy(), 2, "env.result");
            phi->addIncoming(nullPtr, entBB);
            phi->addIncoming(strObj, okEnd);
            return phi;
        }
    }

    if (cls == "Convert") {
        if (call.args.size() == 1 && call.args[0].value) {
            // Convert.ToInt32(s: string): int — atoi / strtol
            if (m == "ToInt32") {
                llvm::Value* arg = lowerExpr(*call.args[0].value);
                if (!arg) return nullptr;
                auto* argTy = ctx_.exprType(*call.args[0].value);
                // If it's already an int, return as-is.
                if (argTy && argTy->kind == sema::TypeRef::Kind::Primitive &&
                    argTy->primitive == sema::PrimitiveType::Int)
                    return arg;
                // For string: call atoi(data_ptr)
                llvm::Value* strPtr = arg;
                llvm::Value* data = loadStringData(strPtr);
                auto* atoiFn = module_->getFunction("atoi");
                if (!atoiFn) {
                    auto* ft = llvm::FunctionType::get(i32(), {ptrTy()}, false);
                    atoiFn = llvm::Function::Create(
                        ft, llvm::Function::ExternalLinkage, "atoi", module_.get());
                }
                return builder_.CreateCall(atoiFn, {data}, "atoi.ret");
            }
            // Convert.ToInt64(s: string): long — atoll
            if (m == "ToInt64") {
                llvm::Value* arg = lowerExpr(*call.args[0].value);
                if (!arg) return nullptr;
                llvm::Value* data = loadStringData(arg);
                auto* atolFn = module_->getFunction("atoll");
                if (!atolFn) {
                    auto* ft = llvm::FunctionType::get(i64(), {ptrTy()}, false);
                    atolFn = llvm::Function::Create(
                        ft, llvm::Function::ExternalLinkage, "atoll", module_.get());
                }
                return builder_.CreateCall(atolFn, {data}, "atoll.ret");
            }
            // Convert.ToDouble(s: string): double — atof
            if (m == "ToDouble") {
                llvm::Value* arg = lowerExpr(*call.args[0].value);
                if (!arg) return nullptr;
                llvm::Value* data = loadStringData(arg);
                auto* atofFn = module_->getFunction("atof");
                if (!atofFn) {
                    auto* ft = llvm::FunctionType::get(
                        llvm::Type::getDoubleTy(llvm_), {ptrTy()}, false);
                    atofFn = llvm::Function::Create(
                        ft, llvm::Function::ExternalLinkage, "atof", module_.get());
                }
                return builder_.CreateCall(atofFn, {data}, "atof.ret");
            }
            // Convert.ToBoolean(s: string): bool — "true"/"1" → true
            if (m == "ToBoolean") {
                llvm::Value* arg = lowerExpr(*call.args[0].value);
                if (!arg) return nullptr;
                llvm::Value* data = loadStringData(arg);
                auto* strcmpFn = module_->getFunction("strcasecmp");
                if (!strcmpFn) {
                    auto* ft = llvm::FunctionType::get(i32(), {ptrTy(), ptrTy()}, false);
                    strcmpFn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "strcasecmp", module_.get());
                }
                llvm::Constant* trueStr = llvm::ConstantDataArray::getString(llvm_, "true", true);
                unsigned tid = stringLiteralIdx_++;
                auto* trueGV = new llvm::GlobalVariable(*module_, trueStr->getType(), true,
                    llvm::GlobalValue::PrivateLinkage, trueStr, ".bz.bool.true." + std::to_string(tid));
                llvm::Value* cmpTrue = builder_.CreateICmpEQ(
                    builder_.CreateCall(strcmpFn, {data, trueGV}), builder_.getInt32(0));
                llvm::Constant* oneStr = llvm::ConstantDataArray::getString(llvm_, "1", true);
                auto* oneGV = new llvm::GlobalVariable(*module_, oneStr->getType(), true,
                    llvm::GlobalValue::PrivateLinkage, oneStr, ".bz.bool.one." + std::to_string(tid));
                llvm::Value* cmpOne = builder_.CreateICmpEQ(
                    builder_.CreateCall(strcmpFn, {data, oneGV}), builder_.getInt32(0));
                return builder_.CreateOr(cmpTrue, cmpOne, "tobool");
            }
            // Convert.ToString(n: int): string — alias for n.ToString()
            if (m == "ToString") {
                llvm::Value* arg = lowerExpr(*call.args[0].value);
                if (!arg) return nullptr;
                auto* argTy = ctx_.exprType(*call.args[0].value);
                if (argTy && argTy->kind == sema::TypeRef::Kind::Primitive)
                    return emitPrimitiveToString(arg, argTy->primitive);
            }
        }
        // §2.3: Convert.TryToInt32(value: string, out result: int): bool.
        // Returns true and sets result when `value` is a fully-valid integer;
        // false and leaves result at its default (0) otherwise. Mirrors the
        // int.TryParse strtol-with-endptr validation (§5.2).
        if (m == "TryToInt32" && call.args.size() == 2 &&
            call.args[0].value && call.args[1].value) {
            llvm::Value* strVal = lowerExpr(*call.args[0].value);
            if (!strVal) return nullptr;
            llvm::Value* strData = loadStringData(strVal);
            // Address of the out parameter (slot pre-created by ensureOutVarSlots).
            llvm::Type* et = nullptr;
            llvm::Value* outAddr = lvalueAddr(*call.args[1].value, et);
            if (!outAddr) return nullptr;
            auto* strtolFn = declareC("strtol", i64(), {ptrTy(), ptrTy(), i32()});
            auto* endSlot = entryAlloca(ptrTy());
            llvm::Value* val64 = builder_.CreateCall(
                strtolFn, {strData, endSlot, builder_.getInt32(10)}, "cvt.val");
            llvm::Value* endPtr = builder_.CreateLoad(ptrTy(), endSlot);
            // ok = (endPtr advanced past start) && (*endPtr == '\0')
            llvm::Value* advanced = builder_.CreateICmpNE(endPtr, strData, "cvt.adv");
            auto* i8t = llvm::Type::getInt8Ty(llvm_);
            llvm::Value* endCh = builder_.CreateLoad(i8t, endPtr, "cvt.end");
            llvm::Value* atNul = builder_.CreateICmpEQ(
                endCh, llvm::ConstantInt::get(i8t, 0), "cvt.nul");
            llvm::Value* ok = builder_.CreateAnd(advanced, atNul, "cvt.ok");
            // Store the (truncated to i32) parsed value; on failure strtol
            // returns 0, matching the "default value" contract.
            builder_.CreateStore(builder_.CreateTrunc(val64, i32()), outAddr);
            return ok;
        }
        // Convert.ToString(n, radix: int): string — radix-based conversion.
        if (m == "ToString" && call.args.size() == 2 &&
            call.args[0].value && call.args[1].value) {
            llvm::Value* arg = lowerExpr(*call.args[0].value);
            llvm::Value* radix = lowerExpr(*call.args[1].value);
            if (!arg || !radix) return nullptr;
            // Widen to i32.
            if (!arg->getType()->isIntegerTy(32))
                arg = builder_.CreateSExt(arg, i32());
            // Select format based on radix (constant check).
            const char* fmt = "%d";
            if (auto* cr = llvm::dyn_cast<llvm::ConstantInt>(radix)) {
                uint64_t r = cr->getZExtValue();
                if (r == 16) fmt = "%x";
                else if (r == 8) fmt = "%o";
                else if (r == 2) fmt = nullptr;  // handle below
            }
            if (fmt) {
                // Use snprintf with the chosen format.
                llvm::Constant* fmtData = llvm::ConstantDataArray::getString(llvm_, fmt, true);
                unsigned fid = stringLiteralIdx_++;
                auto* fmtGV = new llvm::GlobalVariable(*module_, fmtData->getType(), true,
                    llvm::GlobalValue::PrivateLinkage, fmtData, ".bz.rfmt." + std::to_string(fid));
                fmtGV->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
                auto* snpFn = module_->getFunction("snprintf");
                if (!snpFn) {
                    auto* ft = llvm::FunctionType::get(i32(), {ptrTy(), i64(), ptrTy()}, true);
                    snpFn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "snprintf", module_.get());
                }
                auto* i8Ty = llvm::Type::getInt8Ty(llvm_);
                llvm::Value* buf = entryAlloca(llvm::ArrayType::get(i8Ty, 64));
                llvm::Value* bufPtr = builder_.CreateGEP(llvm::ArrayType::get(i8Ty, 64), buf, {builder_.getInt64(0), builder_.getInt64(0)});
                llvm::Value* written = builder_.CreateCall(snpFn, {bufPtr, builder_.getInt64(64), fmtGV, arg}, "snp.ret");
                auto* mallocFn = declareC("malloc", ptrTy(), {i64()});
                auto* memcpyFn = declareC("memcpy", ptrTy(), {ptrTy(), ptrTy(), i64()});
                llvm::Value* len64 = builder_.CreateSExt(written, i64());
                llvm::Value* dataSz = builder_.CreateAdd(len64, builder_.getInt64(1));
                llvm::Value* dataCopy = builder_.CreateCall(mallocFn, {dataSz}, "rd.data");
                builder_.CreateCall(memcpyFn, {dataCopy, bufPtr, len64});
                llvm::Value* nulPtr = builder_.CreateGEP(i8Ty, dataCopy, len64);
                builder_.CreateStore(llvm::ConstantInt::get(i8Ty, 0), nulPtr);
                uint64_t strBytes = module_->getDataLayout().getTypeAllocSize(stringType());
                llvm::Value* strObj = builder_.CreateCall(mallocFn, {builder_.getInt64(strBytes)}, "rd.obj");
                builder_.CreateStore(dataCopy, builder_.CreateStructGEP(stringType(), strObj, 0));
                builder_.CreateStore(len64, builder_.CreateStructGEP(stringType(), strObj, 1));
                builder_.CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(llvm_), 1),
                    builder_.CreateStructGEP(stringType(), strObj, 2));
                    initStringRc(strObj);  // #422: producer's reference, rc := 1
                return strObj;
            }
            // Binary (radix=2): emit a runtime bit-scan loop.
            // Builds digits MSB-first into a 33-byte stack buffer.
            auto* i8Ty = llvm::Type::getInt8Ty(llvm_);
            auto* buf2 = entryAlloca(llvm::ArrayType::get(i8Ty, 33));
            llvm::Value* b2Ptr = builder_.CreateGEP(
                llvm::ArrayType::get(i8Ty, 33), buf2,
                {builder_.getInt64(0), builder_.getInt64(0)});
            // For n=0: just write "0"
            llvm::Value* isZero = builder_.CreateICmpEQ(arg, builder_.getInt32(0));
            auto* zeroBB = llvm::BasicBlock::Create(llvm_, "bn.zero", currentFn_);
            auto* nonBB  = llvm::BasicBlock::Create(llvm_, "bn.nonz", currentFn_);
            auto* binBB  = llvm::BasicBlock::Create(llvm_, "bn.end",  currentFn_);
            auto* entBN  = builder_.GetInsertBlock();
            builder_.CreateCondBr(isZero, zeroBB, nonBB);
            // Zero: write "0\0"
            builder_.SetInsertPoint(zeroBB);
            builder_.CreateStore(llvm::ConstantInt::get(i8Ty, '0'), b2Ptr);
            llvm::Value* z1 = builder_.CreateGEP(i8Ty, b2Ptr, builder_.getInt64(1));
            builder_.CreateStore(llvm::ConstantInt::get(i8Ty, 0), z1);
            llvm::Value* zlen = builder_.getInt64(1);
            auto* zeroBBEnd = builder_.GetInsertBlock();
            builder_.CreateBr(binBB);
            // Non-zero: find highest set bit, then emit each bit MSB-first.
            builder_.SetInsertPoint(nonBB);
            auto* posS = entryAlloca(i32());
            auto* lenS = entryAlloca(i64());
            builder_.CreateStore(builder_.getInt32(0), lenS);
            // Find bit width: highest set bit position.
            // Use while loop: find highest bit.
            auto* tmp = entryAlloca(i32());
            builder_.CreateStore(arg, tmp);
            auto* bitPos = entryAlloca(i32());
            builder_.CreateStore(builder_.getInt32(31), bitPos);
            // Scan from MSB to find the first 1 bit.
            auto* fhBB = llvm::BasicBlock::Create(llvm_, "bn.fh", currentFn_);
            auto* fhBodyBB = llvm::BasicBlock::Create(llvm_, "bn.fhb", currentFn_);
            auto* fhEndBB  = llvm::BasicBlock::Create(llvm_, "bn.fhe", currentFn_);
            builder_.CreateBr(fhBB);
            builder_.SetInsertPoint(fhBB);
            llvm::Value* bp = builder_.CreateLoad(i32(), bitPos);
            llvm::Value* bv = builder_.CreateLoad(i32(), tmp);
            llvm::Value* bit = builder_.CreateLShr(bv, bp);
            llvm::Value* isOne = builder_.CreateICmpEQ(
                builder_.CreateAnd(bit, builder_.getInt32(1)), builder_.getInt32(1));
            llvm::Value* bpGe0 = builder_.CreateICmpSGT(bp, builder_.getInt32(0));
            builder_.CreateCondBr(builder_.CreateAnd(bpGe0, builder_.CreateNot(isOne)),
                fhBodyBB, fhEndBB);
            builder_.SetInsertPoint(fhBodyBB);
            builder_.CreateStore(builder_.CreateSub(bp, builder_.getInt32(1)), bitPos);
            builder_.CreateBr(fhBB);
            builder_.SetInsertPoint(fhEndBB);
            // Now emit bits from bitPos down to 0.
            auto* emBP = entryAlloca(i32());
            auto* emLen = entryAlloca(i64());
            llvm::Value* startBP = builder_.CreateLoad(i32(), bitPos);
            builder_.CreateStore(startBP, emBP);
            builder_.CreateStore(builder_.getInt64(0), emLen);
            auto* emCBB = llvm::BasicBlock::Create(llvm_, "bn.ec", currentFn_);
            auto* emBBB = llvm::BasicBlock::Create(llvm_, "bn.eb", currentFn_);
            auto* emEBB = llvm::BasicBlock::Create(llvm_, "bn.ee", currentFn_);
            builder_.CreateBr(emCBB);
            builder_.SetInsertPoint(emCBB);
            llvm::Value* ep = builder_.CreateLoad(i32(), emBP);
            builder_.CreateCondBr(builder_.CreateICmpSGE(ep, builder_.getInt32(0)), emBBB, emEBB);
            builder_.SetInsertPoint(emBBB);
            llvm::Value* epB = builder_.CreateLoad(i32(), emBP);
            llvm::Value* lenV = builder_.CreateLoad(i64(), emLen);
            llvm::Value* bArg = builder_.CreateLoad(i32(), tmp);
            llvm::Value* d = builder_.CreateAnd(builder_.CreateLShr(bArg, epB), builder_.getInt32(1));
            llvm::Value* ch = builder_.CreateAdd(builder_.CreateTrunc(d, i8Ty),
                llvm::ConstantInt::get(i8Ty, '0'));
            llvm::Value* dp = builder_.CreateGEP(i8Ty, b2Ptr, lenV);
            builder_.CreateStore(ch, dp);
            builder_.CreateStore(builder_.CreateAdd(lenV, builder_.getInt64(1)), emLen);
            builder_.CreateStore(builder_.CreateSub(epB, builder_.getInt32(1)), emBP);
            builder_.CreateBr(emCBB);
            builder_.SetInsertPoint(emEBB);
            llvm::Value* nlen = builder_.CreateLoad(i64(), emLen);
            // Null-terminate.
            llvm::Value* np2 = builder_.CreateGEP(i8Ty, b2Ptr, nlen);
            builder_.CreateStore(llvm::ConstantInt::get(i8Ty, 0), np2);
            auto* nonBBEnd = builder_.GetInsertBlock();
            builder_.CreateBr(binBB);
            // Merge and create bz_string.
            builder_.SetInsertPoint(binBB);
            auto* phiLen = builder_.CreatePHI(i64(), 2, "bn.len");
            phiLen->addIncoming(zlen, zeroBBEnd);
            phiLen->addIncoming(nlen, nonBBEnd);
            auto* mallocFn = declareC("malloc", ptrTy(), {i64()});
            auto* memcpyFn = declareC("memcpy", ptrTy(), {ptrTy(), ptrTy(), i64()});
            llvm::Value* dataSz2 = builder_.CreateAdd(phiLen, builder_.getInt64(1));
            llvm::Value* dataCopy2 = builder_.CreateCall(mallocFn, {dataSz2}, "bn.data");
            builder_.CreateCall(memcpyFn, {dataCopy2, b2Ptr, phiLen});
            llvm::Value* nulPtr2 = builder_.CreateGEP(i8Ty, dataCopy2, phiLen);
            builder_.CreateStore(llvm::ConstantInt::get(i8Ty, 0), nulPtr2);
            uint64_t strBytes = module_->getDataLayout().getTypeAllocSize(stringType());
            llvm::Value* strObj2 = builder_.CreateCall(mallocFn, {builder_.getInt64(strBytes)}, "bn.obj");
            builder_.CreateStore(dataCopy2, builder_.CreateStructGEP(stringType(), strObj2, 0));
            builder_.CreateStore(phiLen, builder_.CreateStructGEP(stringType(), strObj2, 1));
            builder_.CreateStore(llvm::ConstantInt::get(i8Ty, 1),
                builder_.CreateStructGEP(stringType(), strObj2, 2));
                initStringRc(strObj2);  // #422: producer's reference, rc := 1
            return strObj2;
        }
    }

    // §9.6: string.Join(separator, items: Array<string>): string
    if ((cls == "string" || cls == "String") && m == "Join" &&
        call.args.size() == 2 && call.args[0].value && call.args[1].value) {
        llvm::Value* sep = lowerExpr(*call.args[0].value);
        llvm::Value* arr = lowerExpr(*call.args[1].value);
        if (!sep || !arr) return nullptr;
        // Emit inline join: iterate parts, concat sep+part (skip sep before first)
        auto* lenFn = declareC("bz_array_len", i64(), {ptrTy()});
        auto* ptrFn = declareC("bz_array_ptr_at", ptrTy(), {ptrTy(), i64(), i64()});
        llvm::Value* len = builder_.CreateCall(lenFn, {arr});
        // Compute total output length
        auto* totSlot = entryAlloca(i64());
        auto* idxSlot = entryAlloca(i64());
        builder_.CreateStore(builder_.getInt64(0), totSlot);
        builder_.CreateStore(builder_.getInt64(0), idxSlot);
        auto* condBB = llvm::BasicBlock::Create(llvm_, "jn.lc", currentFn_);
        auto* bodyBB = llvm::BasicBlock::Create(llvm_, "jn.lb", currentFn_);
        auto* doneL  = llvm::BasicBlock::Create(llvm_, "jn.ld", currentFn_);
        builder_.CreateBr(condBB);
        builder_.SetInsertPoint(condBB);
        llvm::Value* idx = builder_.CreateLoad(i64(), idxSlot);
        builder_.CreateCondBr(builder_.CreateICmpSLT(idx, len), bodyBB, doneL);
        builder_.SetInsertPoint(bodyBB);
        llvm::Value* ep = builder_.CreateCall(ptrFn, {arr, idx, builder_.getInt64(8)});
        llvm::Value* elem = builder_.CreateLoad(ptrTy(), ep);
        llvm::Value* elen = loadStringLen(elem);
        llvm::Value* tot = builder_.CreateLoad(i64(), totSlot);
        // Add sep length for all except first element
        llvm::Value* notFirst = builder_.CreateICmpSGT(idx, builder_.getInt64(0));
        llvm::Value* sepLen = loadStringLen(sep);
        llvm::Value* addSep = builder_.CreateSelect(notFirst, sepLen, builder_.getInt64(0));
        builder_.CreateStore(builder_.CreateAdd(tot, builder_.CreateAdd(elen, addSep)), totSlot);
        builder_.CreateStore(builder_.CreateAdd(idx, builder_.getInt64(1)), idxSlot);
        builder_.CreateBr(condBB);
        builder_.SetInsertPoint(doneL);
        // Allocate output buffer and fill
        llvm::Value* total = builder_.CreateLoad(i64(), totSlot);
        auto* mallocFn = declareC("malloc", ptrTy(), {i64()});
        auto* memcpyFn = declareC("memcpy", ptrTy(), {ptrTy(), ptrTy(), i64()});
        llvm::Value* outBuf = builder_.CreateCall(mallocFn, {
            builder_.CreateAdd(total, builder_.getInt64(1))});
        auto* offSlot = entryAlloca(i64());
        auto* idx2Slot = entryAlloca(i64());
        builder_.CreateStore(builder_.getInt64(0), offSlot);
        builder_.CreateStore(builder_.getInt64(0), idx2Slot);
        auto* condBB2 = llvm::BasicBlock::Create(llvm_, "jn.wc", currentFn_);
        auto* bodyBB2 = llvm::BasicBlock::Create(llvm_, "jn.wb", currentFn_);
        auto* doneL2  = llvm::BasicBlock::Create(llvm_, "jn.wd", currentFn_);
        builder_.CreateBr(condBB2);
        builder_.SetInsertPoint(condBB2);
        llvm::Value* idx2 = builder_.CreateLoad(i64(), idx2Slot);
        builder_.CreateCondBr(builder_.CreateICmpSLT(idx2, len), bodyBB2, doneL2);
        builder_.SetInsertPoint(bodyBB2);
        llvm::Value* off = builder_.CreateLoad(i64(), offSlot);
        // Copy separator if not first
        auto* i8Ty = llvm::Type::getInt8Ty(llvm_);
        llvm::Value* notFirst2 = builder_.CreateICmpSGT(idx2, builder_.getInt64(0));
        auto* sepCopyBB = llvm::BasicBlock::Create(llvm_, "jn.sc", currentFn_);
        auto* partBB = llvm::BasicBlock::Create(llvm_, "jn.pc", currentFn_);
        builder_.CreateCondBr(notFirst2, sepCopyBB, partBB);
        builder_.SetInsertPoint(sepCopyBB);
        llvm::Value* sepData = loadStringData(sep);
        llvm::Value* sepLen2 = loadStringLen(sep);
        llvm::Value* dstSep = builder_.CreateGEP(i8Ty, outBuf, off);
        builder_.CreateCall(memcpyFn, {dstSep, sepData, sepLen2});
        llvm::Value* off2 = builder_.CreateAdd(off, sepLen2);
        builder_.CreateBr(partBB);
        builder_.SetInsertPoint(partBB);
        auto* phi = builder_.CreatePHI(i64(), 2);
        phi->addIncoming(off, bodyBB2);   // no-sep path (false branch from bodyBB2)
        phi->addIncoming(off2, sepCopyBB); // sep-copied path
        // Actually need the right predecessor — rebuild using simpler pattern
        // Copy element
        llvm::Value* ep2 = builder_.CreateCall(ptrFn, {arr, idx2, builder_.getInt64(8)});
        llvm::Value* elem2 = builder_.CreateLoad(ptrTy(), ep2);
        llvm::Value* edata = loadStringData(elem2);
        llvm::Value* elen2 = loadStringLen(elem2);
        llvm::Value* dstElem = builder_.CreateGEP(i8Ty, outBuf, phi);
        builder_.CreateCall(memcpyFn, {dstElem, edata, elen2});
        llvm::Value* newOff = builder_.CreateAdd(phi, elen2);
        builder_.CreateStore(newOff, offSlot);
        builder_.CreateStore(builder_.CreateAdd(idx2, builder_.getInt64(1)), idx2Slot);
        builder_.CreateBr(condBB2);
        builder_.SetInsertPoint(doneL2);
        // NUL terminate
        llvm::Value* finalOff = builder_.CreateLoad(i64(), offSlot);
        llvm::Value* nulPtr = builder_.CreateGEP(i8Ty, outBuf, finalOff);
        builder_.CreateStore(llvm::ConstantInt::get(i8Ty, 0), nulPtr);
        // Wrap in bz.string
        auto* callocFn2 = declareC("calloc", ptrTy(), {i64(), i64()});
        uint64_t strSz = module_->getDataLayout().getTypeAllocSize(stringType());
        llvm::Value* strObj = builder_.CreateCall(callocFn2,
            {builder_.getInt64(1), builder_.getInt64(strSz)});
        builder_.CreateStore(outBuf, builder_.CreateStructGEP(stringType(), strObj, 0));
        builder_.CreateStore(total, builder_.CreateStructGEP(stringType(), strObj, 1));
        builder_.CreateStore(llvm::ConstantInt::get(i8Ty, 1),
            builder_.CreateStructGEP(stringType(), strObj, 2));
            initStringRc(strObj);  // #422: producer's reference, rc := 1
        return strObj;
    }

    if (cls == "Math") {
        if (call.args.size() == 1 && call.args[0].value) {
            llvm::Value* arg = lowerExpr(*call.args[0].value);
            if (!arg) return nullptr;
            if (m == "Abs") {
                if (arg->getType()->isFloatingPointTy()) {
                    // Abs for float/double — use fabs intrinsic.
                    auto* fabsFn = llvm::Intrinsic::getOrInsertDeclaration(
                        module_.get(), llvm::Intrinsic::fabs,
                        {arg->getType()});
                    return builder_.CreateCall(fabsFn, {arg}, "abs.result");
                }
                // Abs for int — select(n<0, -n, n)
                llvm::Value* neg = builder_.CreateNeg(arg, "abs.neg");
                llvm::Value* isNeg = builder_.CreateICmpSLT(
                    arg, llvm::ConstantInt::get(arg->getType(), 0));
                return builder_.CreateSelect(isNeg, neg, arg, "abs.result");
            }
            if (m == "Sign") {
                // Sign: -1 if n<0, 0 if n==0, 1 if n>0.
                auto* zero = llvm::ConstantInt::get(arg->getType(), 0);
                llvm::Value* isNeg = builder_.CreateICmpSLT(arg, zero, "sgn.neg");
                llvm::Value* isPos = builder_.CreateICmpSGT(arg, zero, "sgn.pos");
                llvm::Value* one = llvm::ConstantInt::get(arg->getType(), 1);
                llvm::Value* negOne = llvm::ConstantInt::getSigned(arg->getType(), -1);
                llvm::Value* s1 = builder_.CreateSelect(isPos, one, zero, "sgn.1");
                return builder_.CreateSelect(isNeg, negOne, s1, "sgn.result");
            }
            // Math.IsNaN(x): bool — NaN is not equal to itself.
            if (m == "IsNaN") {
                auto* dbl = llvm::Type::getDoubleTy(llvm_);
                if (!arg->getType()->isDoubleTy())
                    arg = builder_.CreateSIToFP(arg, dbl);
                return builder_.CreateFCmpUNO(arg, arg, "isnan");
            }
            // Math.IsInfinity(x): bool — compare with +/-infinity.
            if (m == "IsInfinity") {
                auto* dbl = llvm::Type::getDoubleTy(llvm_);
                if (!arg->getType()->isDoubleTy())
                    arg = builder_.CreateSIToFP(arg, dbl);
                auto* posInf = llvm::ConstantFP::getInfinity(dbl, false);
                auto* negInf = llvm::ConstantFP::getInfinity(dbl, true);
                llvm::Value* isPosInf = builder_.CreateFCmpOEQ(arg, posInf);
                llvm::Value* isNegInf = builder_.CreateFCmpOEQ(arg, negInf);
                return builder_.CreateOr(isPosInf, isNegInf, "isinf");
            }
            // Math.IsFinite(x): bool — not NaN and not infinity.
            if (m == "IsFinite") {
                auto* dbl = llvm::Type::getDoubleTy(llvm_);
                if (!arg->getType()->isDoubleTy())
                    arg = builder_.CreateSIToFP(arg, dbl);
                llvm::Value* isNaN = builder_.CreateFCmpUNO(arg, arg);
                auto* posInf = llvm::ConstantFP::getInfinity(dbl, false);
                auto* negInf = llvm::ConstantFP::getInfinity(dbl, true);
                llvm::Value* isInf = builder_.CreateOr(
                    builder_.CreateFCmpOEQ(arg, posInf),
                    builder_.CreateFCmpOEQ(arg, negInf));
                return builder_.CreateNot(builder_.CreateOr(isNaN, isInf), "isfinite");
            }
            if (m == "Cbrt") {
                // Math.Cbrt(x): double — cube root via libc cbrt().
                auto* dbl = llvm::Type::getDoubleTy(llvm_);
                if (!arg->getType()->isDoubleTy())
                    arg = builder_.CreateSIToFP(arg, dbl);
                auto* cbrtFn = module_->getFunction("cbrt");
                if (!cbrtFn) {
                    auto* ft = llvm::FunctionType::get(dbl, {dbl}, false);
                    cbrtFn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "cbrt", module_.get());
                }
                return builder_.CreateCall(cbrtFn, {arg}, "cbrt.result");
            }
            if (m == "Sqrt" || m == "Floor" || m == "Ceiling" || m == "Round" || m == "Truncate") {
                // Use llvm intrinsics for common math ops. §2.2: one-argument
                // Round is BANKER'S ROUNDING (round half to even) — that is
                // llvm.roundeven (IEEE 754 roundToNearestTiesToEven), NOT
                // llvm.round (which rounds halves away from zero).
                llvm::Intrinsic::ID iid =
                    m == "Sqrt"     ? llvm::Intrinsic::sqrt :
                    m == "Floor"    ? llvm::Intrinsic::floor :
                    m == "Ceiling"  ? llvm::Intrinsic::ceil :
                    m == "Truncate" ? llvm::Intrinsic::trunc :
                                      llvm::Intrinsic::roundeven;
                auto* dbl = llvm::Type::getDoubleTy(llvm_);
                if (!arg->getType()->isDoubleTy())
                    arg = builder_.CreateSIToFP(arg, dbl);
                auto* fn = llvm::Intrinsic::getDeclaration(module_.get(), iid, {dbl});
                return builder_.CreateCall(fn, {arg}, std::string(m) + ".result");
            }
        }
        if (call.args.size() == 2 && call.args[0].value && call.args[1].value) {
            llvm::Value* a = lowerExpr(*call.args[0].value);
            llvm::Value* b = lowerExpr(*call.args[1].value);
            if (!a || !b) return nullptr;
            if (m == "Min") {
                // Handle int or double operands.
                if (a->getType()->isIntegerTy()) {
                    llvm::Value* cmp = builder_.CreateICmpSLT(a, b);
                    return builder_.CreateSelect(cmp, a, b, "min.result");
                } else {
                    // float/double: use llvm.minnum intrinsic
                    auto* dbl = llvm::Type::getDoubleTy(llvm_);
                    if (!a->getType()->isDoubleTy()) a = builder_.CreateSIToFP(a, dbl);
                    if (!b->getType()->isDoubleTy()) b = builder_.CreateSIToFP(b, dbl);
                    auto* fn = llvm::Intrinsic::getDeclaration(module_.get(), llvm::Intrinsic::minnum, {dbl});
                    return builder_.CreateCall(fn, {a, b}, "fmin.result");
                }
            }
            if (m == "Max") {
                if (a->getType()->isIntegerTy()) {
                    llvm::Value* cmp = builder_.CreateICmpSGT(a, b);
                    return builder_.CreateSelect(cmp, a, b, "max.result");
                } else {
                    auto* dbl = llvm::Type::getDoubleTy(llvm_);
                    if (!a->getType()->isDoubleTy()) a = builder_.CreateSIToFP(a, dbl);
                    if (!b->getType()->isDoubleTy()) b = builder_.CreateSIToFP(b, dbl);
                    auto* fn = llvm::Intrinsic::getDeclaration(module_.get(), llvm::Intrinsic::maxnum, {dbl});
                    return builder_.CreateCall(fn, {a, b}, "fmax.result");
                }
            }
            // Math.Pow(b, e): double — libc pow()
            if (m == "Pow") {
                auto* dbl = llvm::Type::getDoubleTy(llvm_);
                if (!a->getType()->isDoubleTy()) a = builder_.CreateSIToFP(a, dbl);
                if (!b->getType()->isDoubleTy()) b = builder_.CreateSIToFP(b, dbl);
                auto* powFn = module_->getFunction("pow");
                if (!powFn) {
                    auto* ft = llvm::FunctionType::get(dbl, {dbl, dbl}, false);
                    powFn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "pow", module_.get());
                }
                return builder_.CreateCall(powFn, {a, b}, "pow.result");
            }
            // §2.2: Math.Round(x, digits): double — banker's rounding to `digits`
            // fractional digits. Compute roundeven(x * 10^digits) / 10^digits,
            // where 10^digits is libc pow(10, digits). roundeven gives the same
            // round-half-to-even semantics as the one-argument form.
            if (m == "Round") {
                auto* dbl = llvm::Type::getDoubleTy(llvm_);
                if (!a->getType()->isDoubleTy()) a = builder_.CreateSIToFP(a, dbl);
                // `digits` is an int; widen to double for pow.
                llvm::Value* digD = b;
                if (!digD->getType()->isDoubleTy())
                    digD = builder_.CreateSIToFP(digD, dbl);
                auto* powFn = module_->getFunction("pow");
                if (!powFn) {
                    auto* ft = llvm::FunctionType::get(dbl, {dbl, dbl}, false);
                    powFn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "pow", module_.get());
                }
                llvm::Value* scale = builder_.CreateCall(
                    powFn, {llvm::ConstantFP::get(dbl, 10.0), digD}, "round.scale");
                llvm::Value* scaled = builder_.CreateFMul(a, scale, "round.scaled");
                auto* reFn = llvm::Intrinsic::getDeclaration(
                    module_.get(), llvm::Intrinsic::roundeven, {dbl});
                llvm::Value* rounded = builder_.CreateCall(reFn, {scaled}, "round.re");
                return builder_.CreateFDiv(rounded, scale, "round.result");
            }
        }
        // §2.2: Math.Pi, Math.E, Math.Tau — named constants. (`PI` is the legacy
        // spelling kept for the existing intrinsic test; the spec name is `Pi`.)
        if (call.args.empty()) {
            auto* dbl = llvm::Type::getDoubleTy(llvm_);
            if (m == "Pi" || m == "PI") return llvm::ConstantFP::get(dbl, 3.14159265358979323846);
            if (m == "E")  return llvm::ConstantFP::get(dbl, 2.71828182845904523536);
            if (m == "Tau") return llvm::ConstantFP::get(dbl, 6.28318530717958647692);
        }
        // Math.Log(x), Math.Log2(x), Math.Log10(x)
        // Math.Clamp(value, min, max): clamp value to [min, max].
        if (m == "Clamp" && call.args.size() == 3 &&
            call.args[0].value && call.args[1].value && call.args[2].value) {
            llvm::Value* val = lowerExpr(*call.args[0].value);
            llvm::Value* lo  = lowerExpr(*call.args[1].value);
            llvm::Value* hi  = lowerExpr(*call.args[2].value);
            if (!val || !lo || !hi) return nullptr;
            if (val->getType()->isIntegerTy()) {
                llvm::Value* clamped = builder_.CreateSelect(
                    builder_.CreateICmpSLT(val, lo), lo, val);
                return builder_.CreateSelect(
                    builder_.CreateICmpSGT(clamped, hi), hi, clamped, "clamp.result");
            } else {
                auto* dbl = llvm::Type::getDoubleTy(llvm_);
                if (!val->getType()->isDoubleTy()) val = builder_.CreateSIToFP(val, dbl);
                if (!lo->getType()->isDoubleTy())  lo  = builder_.CreateSIToFP(lo, dbl);
                if (!hi->getType()->isDoubleTy())  hi  = builder_.CreateSIToFP(hi, dbl);
                llvm::Value* clamped = builder_.CreateSelect(
                    builder_.CreateFCmpOLT(val, lo), lo, val);
                return builder_.CreateSelect(
                    builder_.CreateFCmpOGT(clamped, hi), hi, clamped, "clamp.f.result");
            }
        }
        if (call.args.size() == 1 && call.args[0].value && (m == "Log" || m == "Log2" || m == "Log10")) {
            llvm::Value* arg = lowerExpr(*call.args[0].value);
            if (!arg) return nullptr;
            auto* dbl = llvm::Type::getDoubleTy(llvm_);
            if (!arg->getType()->isDoubleTy()) arg = builder_.CreateSIToFP(arg, dbl);
            const char* cname = m == "Log2" ? "log2" : m == "Log10" ? "log10" : "log";
            auto* logFn = module_->getFunction(cname);
            if (!logFn) {
                auto* ft = llvm::FunctionType::get(dbl, {dbl}, false);
                logFn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, cname, module_.get());
            }
            return builder_.CreateCall(logFn, {arg}, std::string(m) + ".result");
        }
        // §6.1/§6.3: Trig functions — 1-arg via libc.
        if (call.args.size() == 1 && call.args[0].value) {
            llvm::Value* arg = lowerExpr(*call.args[0].value);
            if (!arg) return nullptr;
            auto* dbl = llvm::Type::getDoubleTy(llvm_);
            if (!arg->getType()->isDoubleTy()) arg = builder_.CreateSIToFP(arg, dbl);
            const char* fn1 = m == "Sin" ? "sin" : m == "Cos" ? "cos" :
                m == "Tan" ? "tan" : m == "Asin" ? "asin" : m == "Acos" ? "acos" :
                m == "Atan" ? "atan" : m == "Exp" ? "exp" :
                m == "Sinh" ? "sinh" : m == "Cosh" ? "cosh" : m == "Tanh" ? "tanh" : nullptr;
            if (fn1) {
                auto* fn = module_->getFunction(fn1);
                if (!fn) {
                    auto* ft = llvm::FunctionType::get(dbl, {dbl}, false);
                    fn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, fn1, module_.get());
                }
                return builder_.CreateCall(fn, {arg}, std::string(m) + ".result");
            }
        }
        // §6.1: Math.Fmod, IEEERemainder, Atan2, Hypot — 2-arg via libc.
        if (call.args.size() == 2 && call.args[0].value && call.args[1].value &&
            (m == "Fmod" || m == "IEEERemainder" || m == "Atan2" || m == "Hypot")) {
            llvm::Value* a = lowerExpr(*call.args[0].value);
            llvm::Value* b = lowerExpr(*call.args[1].value);
            if (!a || !b) return nullptr;
            auto* dbl = llvm::Type::getDoubleTy(llvm_);
            if (!a->getType()->isDoubleTy()) a = builder_.CreateSIToFP(a, dbl);
            if (!b->getType()->isDoubleTy()) b = builder_.CreateSIToFP(b, dbl);
            const char* fn2 = m == "Fmod" ? "fmod" : m == "IEEERemainder" ? "remainder" :
                               m == "Atan2" ? "atan2" : "hypot";
            auto* fn = module_->getFunction(fn2);
            if (!fn) {
                auto* ft = llvm::FunctionType::get(dbl, {dbl, dbl}, false);
                fn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, fn2, module_.get());
            }
            return builder_.CreateCall(fn, {a, b}, std::string(m) + ".result");
        }
    }

    // §2.x: Random class — pseudorandom numbers via libc rand()/srand().
    if (cls == "Random") {
        auto* randFn = module_->getFunction("rand");
        if (!randFn) {
            auto* ft = llvm::FunctionType::get(i32(), {}, false);
            randFn = llvm::Function::Create(
                ft, llvm::Function::ExternalLinkage, "rand", module_.get());
        }
        // Helper: make non-negative.
        auto absRand = [&](llvm::Value* v) -> llvm::Value* {
            llvm::Value* isNeg = builder_.CreateICmpSLT(
                v, llvm::ConstantInt::get(i32(), 0));
            return builder_.CreateSelect(isNeg, builder_.CreateNeg(v), v);
        };
        if (m == "Next" && call.args.empty())
            return builder_.CreateCall(randFn, {}, "rand.ret");
        if (m == "Next" && call.args.size() == 1 && call.args[0].value) {
            llvm::Value* maxV = lowerExpr(*call.args[0].value);
            if (!maxV) return nullptr;
            return builder_.CreateSRem(
                absRand(builder_.CreateCall(randFn, {}, "rand.r")),
                maxV, "rand.mod");
        }
        if (m == "Next" && call.args.size() == 2 &&
            call.args[0].value && call.args[1].value) {
            llvm::Value* minV = lowerExpr(*call.args[0].value);
            llvm::Value* maxV = lowerExpr(*call.args[1].value);
            if (!minV || !maxV) return nullptr;
            llvm::Value* range = builder_.CreateSub(maxV, minV);
            return builder_.CreateAdd(
                builder_.CreateSRem(
                    absRand(builder_.CreateCall(randFn, {}, "rand.r")),
                    range),
                minV, "rand.result");
        }
        if (m == "NextDouble" && call.args.empty()) {
            auto* dbl = llvm::Type::getDoubleTy(llvm_);
            llvm::Value* r = absRand(builder_.CreateCall(randFn, {}, "rand.r"));
            return builder_.CreateFDiv(
                builder_.CreateSIToFP(r, dbl),
                llvm::ConstantFP::get(dbl, 2147483648.0), "rand.dbl");
        }
        if (m == "Seed" && call.args.size() == 1 && call.args[0].value) {
            auto* srandFn = module_->getFunction("srand");
            if (!srandFn) {
                auto* ft = llvm::FunctionType::get(
                    llvm::Type::getVoidTy(llvm_), {i32()}, false);
                srandFn = llvm::Function::Create(
                    ft, llvm::Function::ExternalLinkage, "srand", module_.get());
            }
            llvm::Value* seed = lowerExpr(*call.args[0].value);
            if (!seed) return nullptr;
            if (!seed->getType()->isIntegerTy(32))
                seed = builder_.CreateTrunc(seed, i32());
            builder_.CreateCall(srandFn, {seed});
            return nullptr;  // unit
        }
    }

    // ---------------------------------------------------------------------------
    // §2.5 DateTime — static members and methods via bz_datetime_* runtime.
    // ---------------------------------------------------------------------------
    if (cls == "DateTime") {
        if (m == "Now" && call.args.empty()) {
            auto* fn = declareC("bz_datetime_now_ticks", i64(), {});
            return builder_.CreateCall(fn, {}, "dt.now");
        }
        if (m == "UtcNow" && call.args.empty()) {
            auto* fn = declareC("bz_datetime_utcnow_ticks", i64(), {});
            return builder_.CreateCall(fn, {}, "dt.utcnow");
        }
        // DateTime.FromTicks(ticks: long): long (identity)
        if (m == "FromTicks" && call.args.size() == 1 && call.args[0].value) {
            return lowerExpr(*call.args[0].value);
        }
    }
    if (cls == "TimeSpan") {
        // TimeSpan.FromSeconds(n): long  (n * 10_000_000L)
        if (m == "FromSeconds" && call.args.size() == 1 && call.args[0].value) {
            llvm::Value* v = lowerExpr(*call.args[0].value);
            if (!v) return nullptr;
            if (!v->getType()->isIntegerTy(64)) v = builder_.CreateSExt(v, i64());
            return builder_.CreateMul(v, builder_.getInt64(10000000LL), "ts.sec");
        }
        if (m == "FromMilliseconds" && call.args.size() == 1 && call.args[0].value) {
            llvm::Value* v = lowerExpr(*call.args[0].value);
            if (!v) return nullptr;
            if (!v->getType()->isIntegerTy(64)) v = builder_.CreateSExt(v, i64());
            return builder_.CreateMul(v, builder_.getInt64(10000LL), "ts.ms");
        }
    }

    // ---------------------------------------------------------------------------
    // §7 System.IO — File, Directory, Path static methods via bz_* runtime.
    // ---------------------------------------------------------------------------
    auto loadBzString = [&](llvm::Value* s) -> std::pair<llvm::Value*, llvm::Value*> {
        return {loadStringData(s), loadStringLen(s)};
    };
    auto strToPath = [&](llvm::Value* s) -> llvm::Value* {
        // Convert bz.string to C NUL-terminated string via snprintf
        llvm::Value* data = loadStringData(s);
        llvm::Value* len64 = loadStringLen(s);
        // Create a temp buffer of len+1 bytes
        llvm::Value* bufLen = builder_.CreateAdd(len64, builder_.getInt64(1));
        auto* fn = declareC("malloc", ptrTy(), {i64()});
        llvm::Value* buf = builder_.CreateCall(fn, {bufLen});
        auto* mcpFn = declareC("memcpy", ptrTy(), {ptrTy(), ptrTy(), i64()});
        builder_.CreateCall(mcpFn, {buf, data, len64});
        // NUL-terminate
        llvm::Value* nul = builder_.CreateGEP(
            llvm::Type::getInt8Ty(llvm_), buf, len64);
        builder_.CreateStore(builder_.getInt8(0), nul);
        return buf;
    };
    auto ptrToStr = [&](llvm::Value* ptr, llvm::Value* lenVal) -> llvm::Value* {
        // Build a bz.string from a malloc'd C string (ptr, len, width=1).
        // #422: allocate the full header size (now 32 bytes incl. rc).
        uint64_t sobjSz = module_->getDataLayout().getTypeAllocSize(stringType());
        auto* sobj = builder_.CreateCall(
            callocFn(), {builder_.getInt64(1), builder_.getInt64((int64_t)sobjSz)});
        builder_.CreateStore(ptr,
            builder_.CreateStructGEP(stringType(), sobj, 0));
        builder_.CreateStore(lenVal,
            builder_.CreateStructGEP(stringType(), sobj, 1));
        builder_.CreateStore(builder_.getInt8(1),
            builder_.CreateStructGEP(stringType(), sobj, 2));
        initStringRc(sobj);  // #422: producer's reference, rc := 1
        return sobj;
    };

    if (cls == "File") {
        if (m == "Exists" && call.args.size() == 1 && call.args[0].value) {
            llvm::Value* s = lowerExpr(*call.args[0].value);
            if (!s) return nullptr;
            llvm::Value* path = strToPath(s);
            auto* fn = declareC("bz_file_exists", i1(), {ptrTy()});
            return builder_.CreateCall(fn, {path});
        }
        if (m == "Delete" && call.args.size() == 1 && call.args[0].value) {
            llvm::Value* s = lowerExpr(*call.args[0].value);
            if (!s) return nullptr;
            auto* fn = declareC("bz_file_delete", i1(), {ptrTy()});
            return builder_.CreateCall(fn, {strToPath(s)});
        }
        if (m == "ReadAllText" && call.args.size() == 1 && call.args[0].value) {
            llvm::Value* s = lowerExpr(*call.args[0].value);
            if (!s) return nullptr;
            auto* outSlot = entryAlloca(ptrTy());
            auto* lenSlot = entryAlloca(i64());
            auto* fn = declareC("bz_file_read_all_text", i32(),
                                {ptrTy(), ptrTy(), ptrTy()});
            builder_.CreateCall(fn, {strToPath(s), outSlot, lenSlot});
            llvm::Value* ptr = builder_.CreateLoad(ptrTy(), outSlot);
            llvm::Value* len = builder_.CreateLoad(i64(), lenSlot);
            return ptrToStr(ptr, len);
        }
        if (m == "ReadAllLines" && call.args.size() == 1 && call.args[0].value) {
            llvm::Value* s = lowerExpr(*call.args[0].value);
            if (!s) return nullptr;
            auto* fn = declareC("bz_file_read_all_lines", ptrTy(), {ptrTy()});
            return builder_.CreateCall(fn, {strToPath(s)}, "lines.arr");
        }
        if (m == "WriteAllText" && call.args.size() == 2 &&
            call.args[0].value && call.args[1].value) {
            llvm::Value* path = lowerExpr(*call.args[0].value);
            llvm::Value* text = lowerExpr(*call.args[1].value);
            if (!path || !text) return nullptr;
            auto [td, tl] = loadBzString(text);
            auto* fn = declareC("bz_file_write_all_text", i32(),
                                {ptrTy(), ptrTy(), i64()});
            return builder_.CreateCall(fn, {strToPath(path), td, tl});
        }
        if (m == "AppendAllText" && call.args.size() == 2 &&
            call.args[0].value && call.args[1].value) {
            llvm::Value* path = lowerExpr(*call.args[0].value);
            llvm::Value* text = lowerExpr(*call.args[1].value);
            if (!path || !text) return nullptr;
            auto [td, tl] = loadBzString(text);
            auto* fn = declareC("bz_file_append_all_text", i32(),
                                {ptrTy(), ptrTy(), i64()});
            return builder_.CreateCall(fn, {strToPath(path), td, tl});
        }
    }
    if (cls == "Directory") {
        if (m == "Exists" && call.args.size() == 1 && call.args[0].value) {
            llvm::Value* s = lowerExpr(*call.args[0].value);
            if (!s) return nullptr;
            auto* fn = declareC("bz_dir_exists", i1(), {ptrTy()});
            return builder_.CreateCall(fn, {strToPath(s)});
        }
        if (m == "CreateDirectory" && call.args.size() == 1 && call.args[0].value) {
            llvm::Value* s = lowerExpr(*call.args[0].value);
            if (!s) return nullptr;
            auto* fn = declareC("bz_dir_create", i32(), {ptrTy()});
            return builder_.CreateCall(fn, {strToPath(s)});
        }
        if (m == "Delete" && call.args.size() >= 1 && call.args[0].value) {
            llvm::Value* s = lowerExpr(*call.args[0].value);
            if (!s) return nullptr;
            // Second arg: recursive (bool), default false
            llvm::Value* recursive = builder_.getFalse();
            if (call.args.size() >= 2 && call.args[1].value) {
                llvm::Value* r = lowerExpr(*call.args[1].value);
                if (r) recursive = r;
            }
            auto* fn = declareC("bz_dir_delete", i32(), {ptrTy(), i1()});
            return builder_.CreateCall(fn, {strToPath(s), recursive});
        }
    }
    // §7: Directory.GetFiles / GetDirectories — return Array<string>
    // bz_dir_get_files returns char** with count via int64_t*.
    // We convert to a bz_array_t of bz.string objects.
    if (cls == "Directory") {
        auto makeFilesList = [&](const char* fnName) -> llvm::Value* {
            if (call.args.size() != 1 || !call.args[0].value) return nullptr;
            llvm::Value* s = lowerExpr(*call.args[0].value);
            if (!s) return nullptr;
            auto* cntSlotG = entryAlloca(i64());
            auto* getFilesFn = declareC(fnName, ptrTy(), {ptrTy(), ptrTy()});
            // Returns char** (array of NUL-terminated paths)
            llvm::Value* rawArr = builder_.CreateCall(getFilesFn,
                {strToPath(s), cntSlotG});
            llvm::Value* cnt = builder_.CreateLoad(i64(), cntSlotG);
            auto* allocFn3 = declareC("bz_array_alloc", ptrTy(), {i64(), i64()});
            llvm::Value* result = builder_.CreateCall(allocFn3,
                {builder_.getInt64(8), cnt});
            auto* addFn3 = declareC("bz_array_add", llvm::Type::getVoidTy(llvm_),
                                    {ptrTy(), ptrTy(), i64()});
            auto* strlenFn3 = declareC("strlen", i64(), {ptrTy()});
            auto* mallocFn3 = declareC("malloc", ptrTy(), {i64()});
            auto* memcpyFn3 = declareC("memcpy", ptrTy(), {ptrTy(), ptrTy(), i64()});
            auto* callocFn4 = declareC("calloc", ptrTy(), {i64(), i64()});
            auto* i8Ty3 = llvm::Type::getInt8Ty(llvm_);
            uint64_t strSz3 = module_->getDataLayout().getTypeAllocSize(stringType());
            auto* idxSlotG = entryAlloca(i64());
            builder_.CreateStore(builder_.getInt64(0), idxSlotG);
            auto* cG = llvm::BasicBlock::Create(llvm_, "gf.c", currentFn_);
            auto* bG = llvm::BasicBlock::Create(llvm_, "gf.b", currentFn_);
            auto* eG = llvm::BasicBlock::Create(llvm_, "gf.e", currentFn_);
            builder_.CreateBr(cG);
            builder_.SetInsertPoint(cG);
            llvm::Value* iG = builder_.CreateLoad(i64(), idxSlotG);
            builder_.CreateCondBr(builder_.CreateICmpSLT(iG, cnt), bG, eG);
            builder_.SetInsertPoint(bG);
            llvm::Value* pG = builder_.CreateGEP(ptrTy(), rawArr, iG);
            llvm::Value* cstrG = builder_.CreateLoad(ptrTy(), pG);
            llvm::Value* lenG = builder_.CreateCall(strlenFn3, {cstrG});
            llvm::Value* dataSzG = builder_.CreateAdd(lenG, builder_.getInt64(1));
            llvm::Value* dataCpG = builder_.CreateCall(mallocFn3, {dataSzG});
            builder_.CreateCall(memcpyFn3, {dataCpG, cstrG, lenG});
            llvm::Value* nulPtrG = builder_.CreateGEP(i8Ty3, dataCpG, lenG);
            builder_.CreateStore(llvm::ConstantInt::get(i8Ty3, 0), nulPtrG);
            llvm::Value* strObjG = builder_.CreateCall(callocFn4,
                {builder_.getInt64(1), builder_.getInt64(strSz3)});
            builder_.CreateStore(dataCpG, builder_.CreateStructGEP(stringType(), strObjG, 0));
            builder_.CreateStore(lenG, builder_.CreateStructGEP(stringType(), strObjG, 1));
            builder_.CreateStore(llvm::ConstantInt::get(i8Ty3, 1),
                builder_.CreateStructGEP(stringType(), strObjG, 2));
                initStringRc(strObjG);  // #422: producer's reference, rc := 1
            auto* slotG = entryAlloca(ptrTy());
            builder_.CreateStore(strObjG, slotG);
            builder_.CreateCall(addFn3, {result, slotG, builder_.getInt64(8)});
            builder_.CreateStore(builder_.CreateAdd(iG, builder_.getInt64(1)), idxSlotG);
            builder_.CreateBr(cG);
            builder_.SetInsertPoint(eG);
            return result;
        };
        if (m == "GetFiles") return makeFilesList("bz_dir_get_files");
        if (m == "GetDirectories") return makeFilesList("bz_dir_get_dirs");
    }
    if (cls == "Path") {
        auto make1arg = [&](const char* fnName) -> llvm::Value* {
            if (call.args.size() != 1 || !call.args[0].value) return nullptr;
            llvm::Value* s = lowerExpr(*call.args[0].value);
            if (!s) return nullptr;
            llvm::Value* p = strToPath(s);
            auto* fn = declareC(fnName, ptrTy(), {ptrTy()});
            llvm::Value* result = builder_.CreateCall(fn, {p});
            // result is a malloc'd C string; compute its length via strlen
            auto* strl = declareC("strlen", i64(), {ptrTy()});
            llvm::Value* len = builder_.CreateCall(strl, {result});
            return ptrToStr(result, len);
        };
        if (m == "GetFileName") return make1arg("bz_path_filename");
        if (m == "GetExtension") return make1arg("bz_path_extension");
        if (m == "GetFileNameWithoutExtension") return make1arg("bz_path_stem");
        if (m == "GetDirectoryName") return make1arg("bz_path_dirname");
        if (m == "GetFullPath") return make1arg("bz_path_absolute");
        if (m == "Combine" && call.args.size() == 2 &&
            call.args[0].value && call.args[1].value) {
            llvm::Value* a = lowerExpr(*call.args[0].value);
            llvm::Value* b = lowerExpr(*call.args[1].value);
            if (!a || !b) return nullptr;
            auto* fn = declareC("bz_path_combine", ptrTy(), {ptrTy(), ptrTy()});
            llvm::Value* result = builder_.CreateCall(fn, {strToPath(a), strToPath(b)});
            auto* strl = declareC("strlen", i64(), {ptrTy()});
            llvm::Value* len = builder_.CreateCall(strl, {result});
            return ptrToStr(result, len);
        }
    }

    // ---------------------------------------------------------------------------
    // §2.6 Random — instance methods via bz_random_* runtime.
    // (Static Random.Next etc. are handled via the bare class above.)
    // ---------------------------------------------------------------------------
    // Random instance methods: `rng.Next(...)` where rng is a variable.
    // The `id` check above resolved `cls = "rng"` (the variable name), not
    // a class name. Instead check the receiver's sema type.
    {  // always enter this block (was wrongly guarded by `!id`)
        if (auto* ma2 = dynamic_cast<const ast::MemberAccessExpr*>(call.callee.get())) {
            if (ma2->target) {
                auto* recvTy = ctx_.exprType(*ma2->target);
                if (recvTy && recvTy->kind == sema::TypeRef::Kind::Named &&
                    !recvTy->declaration && recvTy->astNode &&
                    recvTy->astNode->name && !recvTy->astNode->name->parts.empty() &&
                    recvTy->astNode->name->parts[0] == "Random") {
                    llvm::Value* rng = lowerExpr(*ma2->target);
                    if (!rng) return nullptr;
                    const auto& mname = ma2->member;
                    if (mname == "Next" && call.args.empty()) {
                        auto* fn = declareC("bz_random_next_int", i32(),
                                            {ptrTy(), i32(), i32()});
                        return builder_.CreateCall(fn,
                            {rng, builder_.getInt32(0),
                             builder_.getInt32(2147483647)});
                    }
                    if (mname == "Next" && call.args.size() == 1 && call.args[0].value) {
                        llvm::Value* hi = lowerExpr(*call.args[0].value);
                        auto* fn = declareC("bz_random_next_int", i32(),
                                            {ptrTy(), i32(), i32()});
                        return builder_.CreateCall(fn,
                            {rng, builder_.getInt32(0), hi});
                    }
                    if (mname == "Next" && call.args.size() == 2 &&
                        call.args[0].value && call.args[1].value) {
                        llvm::Value* lo = lowerExpr(*call.args[0].value);
                        llvm::Value* hi = lowerExpr(*call.args[1].value);
                        auto* fn = declareC("bz_random_next_int", i32(),
                                            {ptrTy(), i32(), i32()});
                        return builder_.CreateCall(fn, {rng, lo, hi});
                    }
                    if (mname == "NextDouble" && call.args.empty()) {
                        auto* fn = declareC("bz_random_next_double",
                                            llvm::Type::getDoubleTy(llvm_), {ptrTy()});
                        return builder_.CreateCall(fn, {rng});
                    }
                }
            }
        }
    }

    return nullptr;
}


// stdlib §3.2: Encoding.Utf8.GetBytes(s) / Encoding.Utf8.GetString(bytes[,i,c]).
// The call's callee is `(Encoding.Utf8).GetBytes` — a member access whose TARGET
// is itself the member access `Encoding.Utf8`. We match that two-level shape:
//   ma          = (Encoding.Utf8).GetBytes        (the callee)
//   ma->target  = Encoding.Utf8                    (a MemberAccessExpr)
//   inner       = Encoding.Utf8 — inner->target is the `Encoding` identifier,
//                 inner->member is the singleton name (Utf8/Utf16/Utf32/Ascii).
// Only Utf8 is implemented (see bz_encoding_*); the others are rejected in sema,
// so we never reach codegen for them — defensively return null if seen here.
llvm::Value* Emitter::tryLowerEncodingCall(const ast::CallExpr& call) {
    if (!call.callee) return nullptr;
    auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(call.callee.get());
    if (!ma || !ma->target) return nullptr;
    auto* inner = dynamic_cast<const ast::MemberAccessExpr*>(ma->target.get());
    if (!inner || !inner->target) return nullptr;
    auto* clsId = dynamic_cast<const ast::IdentifierExpr*>(inner->target.get());
    if (!clsId || clsId->name != "Encoding") return nullptr;
    // Only the Utf8 singleton is implemented (§3.2). Utf16/Utf32/Ascii are
    // deferred; sema rejects them, so this guard is a defensive backstop.
    if (inner->member != "Utf8") return nullptr;

    const auto& m = ma->member;

    // GetBytes(s): owned Array<byte> — UTF-8 encode the string into a fresh
    // bz_array_t* of bytes (elem_size 1). The result is owned by the caller.
    if (m == "GetBytes" && call.args.size() == 1 && call.args[0].value) {
        llvm::Value* strPtr = lowerExpr(*call.args[0].value);
        if (!strPtr) return nullptr;
        auto* fn = declareC("bz_encoding_utf8_get_bytes", ptrTy(), {ptrTy()});
        return builder_.CreateCall(fn, {strPtr}, "enc.bytes");
    }

    // GetString(bytes): string — decode the whole array (index 0, count Length).
    // GetString(bytes, index, count): string — decode the requested sub-range.
    if (m == "GetString" &&
        (call.args.size() == 1 || call.args.size() == 3) &&
        call.args[0].value) {
        llvm::Value* arr = lowerExpr(*call.args[0].value);
        if (!arr) return nullptr;
        llvm::Value* index;
        llvm::Value* count;
        if (call.args.size() == 1) {
            // index = 0, count = bz_array_len(arr).
            index = builder_.getInt64(0);
            auto* lenFn = declareC("bz_array_len", i64(), {ptrTy()});
            count = builder_.CreateCall(lenFn, {arr}, "enc.arrlen");
        } else {
            if (!call.args[1].value || !call.args[2].value) return nullptr;
            llvm::Value* iv = lowerExpr(*call.args[1].value);
            llvm::Value* cv = lowerExpr(*call.args[2].value);
            if (!iv || !cv) return nullptr;
            // §3.2 index/count are `int` (i32); widen to the runtime's i64.
            index = iv->getType()->isIntegerTy(64)
                        ? iv : builder_.CreateSExt(iv, i64(), "enc.idx");
            count = cv->getType()->isIntegerTy(64)
                        ? cv : builder_.CreateSExt(cv, i64(), "enc.cnt");
        }
        auto* fn = declareC("bz_encoding_utf8_get_string", ptrTy(),
                            {ptrTy(), i64(), i64()});
        return builder_.CreateCall(fn, {arr, index, count}, "enc.str");
    }

    return nullptr;
}


llvm::Value* Emitter::tryLowerPrimitiveStaticCall(const ast::CallExpr& call) {
    if (!call.callee) return nullptr;
    auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(call.callee.get());
    if (!ma || !ma->target) return nullptr;
    auto* id = dynamic_cast<const ast::IdentifierExpr*>(ma->target.get());
    if (!id) return nullptr;
    sema::PrimitiveType pt = sema::primitiveFromName(id->name);
    if (pt == sema::PrimitiveType::None) return nullptr;
    const auto& m = ma->member;
    using P = sema::PrimitiveType;

    // §5: char static classification methods — char.IsDigit(c), etc.
    if (pt == P::Char && call.args.size() == 1 && call.args[0].value) {
        llvm::Value* arg = lowerExpr(*call.args[0].value);
        if (!arg) return nullptr;
        // Widen to i32 for libc functions if needed.
        if (!arg->getType()->isIntegerTy(32))
            arg = builder_.CreateSExt(arg, i32());
        // Map Bezel method names to libc functions.
        const char* fnName = nullptr;
        if (m == "IsDigit") fnName = "isdigit";
        else if (m == "IsLetter") fnName = "isalpha";
        else if (m == "IsLetterOrDigit") fnName = "isalnum";
        else if (m == "IsWhiteSpace") fnName = "isspace";
        else if (m == "IsUpper") fnName = "isupper";
        else if (m == "IsLower") fnName = "islower";
        else if (m == "IsPunctuation") fnName = "ispunct";
        if (fnName) {
            auto* fn = module_->getFunction(fnName);
            if (!fn) {
                auto* ft = llvm::FunctionType::get(i32(), {i32()}, false);
                fn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, fnName, module_.get());
            }
            llvm::Value* res = builder_.CreateCall(fn, {arg}, std::string(fnName) + ".res");
            return builder_.CreateICmpNE(res, builder_.getInt32(0), std::string(fnName) + ".bool");
        }
        if (m == "ToUpper") {
            auto* fn = module_->getFunction("toupper");
            if (!fn) {
                auto* ft = llvm::FunctionType::get(i32(), {i32()}, false);
                fn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "toupper", module_.get());
            }
            return builder_.CreateCall(fn, {arg}, "toupper.result");
        }
        if (m == "ToLower") {
            auto* fn = module_->getFunction("tolower");
            if (!fn) {
                auto* ft = llvm::FunctionType::get(i32(), {i32()}, false);
                fn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "tolower", module_.get());
            }
            return builder_.CreateCall(fn, {arg}, "tolower.result");
        }
        // char.ToString(c): string — convert char code point to string.
        // Uses the same path as c.ToString() for a char value.
        if (m == "ToString") {
            return emitPrimitiveToString(arg, sema::PrimitiveType::Char);
        }
    }

    // string.From(char): string — one-character string from a char code point.
    // string.From(c) is equivalent to char.ToString(c).
    if (pt == P::String && m == "From" &&
        call.args.size() == 1 && call.args[0].value) {
        llvm::Value* arg = lowerExpr(*call.args[0].value);
        if (!arg) return nullptr;
        // Normalize to i32 if needed.
        if (!arg->getType()->isIntegerTy(32))
            arg = builder_.CreateSExt(arg, i32());
        return emitPrimitiveToString(arg, P::Char);
    }

    // string.IsNullOrEmpty(s): bool — true if s is null or length 0.
    if (pt == P::String && m == "IsNullOrEmpty" &&
        call.args.size() == 1 && call.args[0].value) {
        llvm::Value* arg = lowerExpr(*call.args[0].value);
        if (!arg) return nullptr;
        // null → true
        llvm::Value* isNull = builder_.CreateICmpEQ(
            arg, llvm::ConstantPointerNull::get(ptrTy()), "ine.null");
        auto* nullBB = llvm::BasicBlock::Create(llvm_, "ine.null", currentFn_);
        auto* checkBB= llvm::BasicBlock::Create(llvm_, "ine.check", currentFn_);
        auto* endBB  = llvm::BasicBlock::Create(llvm_, "ine.end",   currentFn_);
        auto* entBB  = builder_.GetInsertBlock();
        builder_.CreateCondBr(isNull, endBB, checkBB);
        builder_.SetInsertPoint(checkBB);
        llvm::Value* len = loadStringLen(arg);
        llvm::Value* empty = builder_.CreateICmpEQ(len, builder_.getInt64(0), "ine.empty");
        auto* chkEnd = builder_.GetInsertBlock();
        builder_.CreateBr(endBB);
        builder_.SetInsertPoint(endBB);
        auto* phi = builder_.CreatePHI(i1(), 2, "ine.result");
        phi->addIncoming(builder_.getTrue(), entBB);
        phi->addIncoming(empty, chkEnd);
        return phi;
    }
    // string.IsNullOrWhiteSpace(s): bool — null, empty, or all whitespace.
    if (pt == P::String && m == "IsNullOrWhiteSpace" &&
        call.args.size() == 1 && call.args[0].value) {
        llvm::Value* arg = lowerExpr(*call.args[0].value);
        if (!arg) return nullptr;
        auto* isspaceFn = module_->getFunction("isspace");
        if (!isspaceFn) {
            auto* ft = llvm::FunctionType::get(i32(), {i32()}, false);
            isspaceFn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "isspace", module_.get());
        }
        auto* i8Ty = llvm::Type::getInt8Ty(llvm_);
        llvm::Value* isNull = builder_.CreateICmpEQ(
            arg, llvm::ConstantPointerNull::get(ptrTy()), "inws.null");
        auto* entBB = builder_.GetInsertBlock();
        auto* chkBB = llvm::BasicBlock::Create(llvm_, "inws.chk", currentFn_);
        auto* loopBB = llvm::BasicBlock::Create(llvm_, "inws.loop", currentFn_);
        auto* bodyBB = llvm::BasicBlock::Create(llvm_, "inws.body", currentFn_);
        auto* nonwsBB = llvm::BasicBlock::Create(llvm_, "inws.nonws", currentFn_);
        auto* endBB  = llvm::BasicBlock::Create(llvm_, "inws.end",  currentFn_);
        builder_.CreateCondBr(isNull, endBB, chkBB);
        builder_.SetInsertPoint(chkBB);
        llvm::Value* len = loadStringLen(arg);
        llvm::Value* data = loadStringData(arg);
        auto* idxS = entryAlloca(i64());
        builder_.CreateStore(builder_.getInt64(0), idxS);
        builder_.CreateBr(loopBB);
        builder_.SetInsertPoint(loopBB);
        llvm::Value* idx = builder_.CreateLoad(i64(), idxS);
        builder_.CreateCondBr(builder_.CreateICmpSLT(idx, len), bodyBB, endBB);
        builder_.SetInsertPoint(bodyBB);
        // For now: only check width=1 chars (ASCII whitespace check).
        llvm::Value* idxB = builder_.CreateLoad(i64(), idxS);
        llvm::Value* cp = loadCodePoint(arg, idxB);
        llvm::Value* isSp = builder_.CreateCall(isspaceFn, {cp});
        llvm::Value* isSpBool = builder_.CreateICmpNE(isSp, builder_.getInt32(0));
        auto* incBB = llvm::BasicBlock::Create(llvm_, "inws.inc", currentFn_);
        builder_.CreateCondBr(isSpBool, incBB, nonwsBB);
        builder_.SetInsertPoint(incBB);
        builder_.CreateStore(builder_.CreateAdd(idxB, builder_.getInt64(1)), idxS);
        builder_.CreateBr(loopBB);
        builder_.SetInsertPoint(nonwsBB);
        builder_.CreateBr(endBB);
        auto* nonwsEnd = builder_.GetInsertBlock();
        builder_.SetInsertPoint(endBB);
        auto* phi = builder_.CreatePHI(i1(), 3, "inws.result");
        phi->addIncoming(builder_.getTrue(), entBB);   // null → true
        phi->addIncoming(builder_.getTrue(), loopBB);  // all whitespace → true
        phi->addIncoming(builder_.getFalse(), nonwsEnd);// found non-WS → false
        return phi;
    }
    // string.Empty → empty string literal.
    if (pt == P::String && m == "Empty" && call.args.empty()) {
        return lowerStringLiteral("\"\"", false);
    }
    // §9.6: string.Compare(a, b): int — ordinal comparison; returns
    // negative/0/positive like memcmp.
    if (pt == P::String && m == "Compare" &&
        call.args.size() == 2 && call.args[0].value && call.args[1].value) {
        llvm::Value* a = lowerExpr(*call.args[0].value);
        llvm::Value* b = lowerExpr(*call.args[1].value);
        if (!a || !b) return nullptr;
        return emitStringOrdCmp(a, b);
    }
    // §9.6: string.Compare(a, b, ignoreCase: bool): int — ordinal comparison,
    // optionally ASCII case-folded. Routed through the runtime helper so the
    // case-folding logic lives in one testable place.
    if (pt == P::String && m == "Compare" && call.args.size() == 3 &&
        call.args[0].value && call.args[1].value && call.args[2].value) {
        llvm::Value* a = lowerExpr(*call.args[0].value);
        llvm::Value* b = lowerExpr(*call.args[1].value);
        llvm::Value* ic = lowerExpr(*call.args[2].value);
        if (!a || !b || !ic) return nullptr;
        llvm::Value* ic32 = ic->getType()->isIntegerTy(32)
            ? ic : builder_.CreateZExt(ic, i32());
        auto* fn = declareC("bz_string_compare", i32(),
                            {ptrTy(), i64(), ptrTy(), i64(), i32()});
        return builder_.CreateCall(
            fn, {loadStringData(a), loadStringLen(a),
                 loadStringData(b), loadStringLen(b), ic32}, "str.cmp.ci");
    }
    // §9.6 (static): string.Repeat(c: char, count: int): string — a `count`-long
    // string of the single code point `c`. Width-1 byte mode (ASCII fast path).
    if (pt == P::String && m == "Repeat" && call.args.size() == 2 &&
        call.args[0].value && call.args[1].value) {
        llvm::Value* ch  = lowerExpr(*call.args[0].value);
        llvm::Value* cnt = lowerExpr(*call.args[1].value);
        if (!ch || !cnt) return nullptr;
        auto* i8Ty = llvm::Type::getInt8Ty(llvm_);
        llvm::Value* count64 = cnt->getType()->isIntegerTy(64)
            ? cnt : builder_.CreateSExt(cnt, i64());
        // Clamp negative counts to 0 (an empty string).
        llvm::Value* neg = builder_.CreateICmpSLT(count64, builder_.getInt64(0));
        count64 = builder_.CreateSelect(neg, builder_.getInt64(0), count64);
        llvm::Value* chI8 = builder_.CreateTrunc(
            ch->getType()->isIntegerTy(32) ? ch
                : builder_.CreateSExt(ch, i32()), i32());  // keep i32 for memset
        auto* mallocFn = declareC("malloc", ptrTy(), {i64()});
        auto* memsetFn = declareC("memset", ptrTy(), {ptrTy(), i32(), i64()});
        llvm::Value* dataSz = builder_.CreateAdd(count64, builder_.getInt64(1));
        llvm::Value* newData = builder_.CreateCall(mallocFn, {dataSz}, "rep.data");
        builder_.CreateCall(memsetFn, {newData, chI8, count64});
        builder_.CreateStore(llvm::ConstantInt::get(i8Ty, 0),
            builder_.CreateGEP(i8Ty, newData, count64));  // NUL terminator
        uint64_t strBytes = module_->getDataLayout().getTypeAllocSize(stringType());
        llvm::Value* strObj = builder_.CreateCall(
            mallocFn, {builder_.getInt64(strBytes)}, "rep.obj");
        builder_.CreateStore(newData,
            builder_.CreateStructGEP(stringType(), strObj, 0));
        builder_.CreateStore(count64,
            builder_.CreateStructGEP(stringType(), strObj, 1));
        builder_.CreateStore(llvm::ConstantInt::get(i8Ty, 1),
            builder_.CreateStructGEP(stringType(), strObj, 2));
        initStringRc(strObj);  // #422: producer's reference, rc := 1
        return strObj;
    }

    // double.TryParse / float.TryParse(s, out result): bool — stdlib §5.2
    if (m == "TryParse" && call.args.size() == 2 &&
        call.args[0].value && call.args[1].value &&
        (pt == P::Double || pt == P::Float)) {
        llvm::Value* strVal = lowerExpr(*call.args[0].value);
        if (!strVal) return nullptr;
        llvm::Value* strData = loadStringData(strVal);
        llvm::Type* et = nullptr;
        llvm::Value* outAddr = lvalueAddr(*call.args[1].value, et);
        if (!outAddr) return nullptr;
        // strtod(data, &endptr)
        auto* strtodFn = declareC("strtod",
            llvm::Type::getDoubleTy(llvm_), {ptrTy(), ptrTy()});
        auto* endSlot = entryAlloca(ptrTy());
        llvm::Value* valD = builder_.CreateCall(
            strtodFn, {strData, endSlot}, "tryp.d");
        llvm::Value* endPtr = builder_.CreateLoad(ptrTy(), endSlot);
        llvm::Value* advanced = builder_.CreateICmpNE(endPtr, strData, "tryp.adv");
        auto* i8Ty3 = llvm::Type::getInt8Ty(llvm_);
        llvm::Value* endCh = builder_.CreateLoad(i8Ty3, endPtr, "tryp.end");
        llvm::Value* atNul = builder_.CreateICmpEQ(
            endCh, llvm::ConstantInt::get(i8Ty3, 0), "tryp.nul");
        llvm::Value* ok = builder_.CreateAnd(advanced, atNul, "tryp.ok");
        if (pt == P::Float) {
            builder_.CreateStore(
                builder_.CreateFPTrunc(valD, llvm::Type::getFloatTy(llvm_)), outAddr);
        } else {
            builder_.CreateStore(valD, outAddr);
        }
        return ok;
    }

    // int.TryParse(s, out result): bool — stdlib §5.2
    // Returns true and sets result if s is a valid integer; false and sets
    // result to 0 otherwise.
    if (m == "TryParse" && call.args.size() == 2 &&
        call.args[0].value && call.args[1].value &&
        (pt == P::Int || pt == P::Long)) {
        llvm::Value* strVal = lowerExpr(*call.args[0].value);
        if (!strVal) return nullptr;
        llvm::Value* strData = loadStringData(strVal);
        // Get address of the out parameter.
        llvm::Type* et = nullptr;
        llvm::Value* outAddr = lvalueAddr(*call.args[1].value, et);
        if (!outAddr) return nullptr;
        // strtol(data, &endptr, 10) for validation.
        auto* strtolFn = declareC("strtol", i64(), {ptrTy(), ptrTy(), i32()});
        auto* endSlot = entryAlloca(ptrTy());
        llvm::Value* val64 = builder_.CreateCall(
            strtolFn, {strData, endSlot, builder_.getInt32(10)}, "tryp.val");
        llvm::Value* endPtr = builder_.CreateLoad(ptrTy(), endSlot);
        // ok = (endPtr != strData) && (*endPtr == '\0')
        llvm::Value* advanced = builder_.CreateICmpNE(endPtr, strData, "tryp.adv");
        auto* i8Ty2 = llvm::Type::getInt8Ty(llvm_);
        llvm::Value* endCh = builder_.CreateLoad(i8Ty2, endPtr, "tryp.end");
        llvm::Value* atNul = builder_.CreateICmpEQ(
            endCh, llvm::ConstantInt::get(i8Ty2, 0), "tryp.nul");
        llvm::Value* ok = builder_.CreateAnd(advanced, atNul, "tryp.ok");
        // Store result.
        if (pt == P::Long) {
            builder_.CreateStore(val64, outAddr);
        } else {
            builder_.CreateStore(builder_.CreateTrunc(val64, i32()), outAddr);
        }
        return ok;
    }

    // int.Parse(s) / long.Parse(s) / double.Parse(s) etc.
    if (m == "Parse" && call.args.size() == 1 && call.args[0].value) {
        llvm::Value* arg = lowerExpr(*call.args[0].value);
        if (!arg) return nullptr;
        // If the argument is a string, use atoi/atoll/atof.
        auto* argTy = ctx_.exprType(*call.args[0].value);
        if (argTy && argTy->kind == sema::TypeRef::Kind::Primitive &&
            argTy->primitive == P::String) {
            llvm::Value* data = loadStringData(arg);
            if (pt == P::Int || pt == P::UInt || pt == P::Short ||
                pt == P::UShort || pt == P::Byte || pt == P::SByte) {
                auto* atoiFn = module_->getFunction("atoi");
                if (!atoiFn) {
                    auto* ft = llvm::FunctionType::get(i32(), {ptrTy()}, false);
                    atoiFn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "atoi", module_.get());
                }
                llvm::Value* r = builder_.CreateCall(atoiFn, {data});
                // Narrow if needed.
                if (pt != P::Int && pt != P::UInt) r = builder_.CreateTrunc(r, lowerPrimitive(pt));
                return r;
            }
            if (pt == P::Long || pt == P::ULong) {
                auto* atollFn = module_->getFunction("atoll");
                if (!atollFn) {
                    auto* ft = llvm::FunctionType::get(i64(), {ptrTy()}, false);
                    atollFn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "atoll", module_.get());
                }
                return builder_.CreateCall(atollFn, {data});
            }
            if (pt == P::Double || pt == P::Float) {
                auto* atofFn = module_->getFunction("atof");
                if (!atofFn) {
                    auto* ft = llvm::FunctionType::get(llvm::Type::getDoubleTy(llvm_), {ptrTy()}, false);
                    atofFn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "atof", module_.get());
                }
                llvm::Value* d = builder_.CreateCall(atofFn, {data});
                if (pt == P::Float) d = builder_.CreateFPTrunc(d, llvm::Type::getFloatTy(llvm_));
                return d;
            }
        }
        // If already the right type, just return it.
        return arg;
    }
    return nullptr;
}


void Emitter::ensureOutVarSlots(const ast::CallExpr& call) {
    // Try to resolve the called method so we know its parameter types.
    // For primitive static calls (int.TryParse, double.TryParse, etc.)
    // we can read the callee primitive type from the member access.
    sema::PrimitiveType callPrimTy = sema::PrimitiveType::None;
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(call.callee.get())) {
        if (ma->target) {
            if (auto* rt = ctx_.exprType(*ma->target)) {
                if (rt->kind == sema::TypeRef::Kind::Primitive)
                    callPrimTy = rt->primitive;
            }
            // Also check type name for static access (e.g. IdentifierExpr("double"))
            if (callPrimTy == sema::PrimitiveType::None) {
                if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(ma->target.get())) {
                    callPrimTy = sema::primitiveFromName(id->name);
                }
            }
        }
    }
    // Look up regular method params via callee symbol.
    sema::MethodSymbol* ms = nullptr;
    if (auto* calleeSym = ctx_.exprSymbol(*call.callee))
        ms = dynamic_cast<sema::MethodSymbol*>(calleeSym);

    std::size_t pos = 0;
    for (const auto& a : call.args) {
        if (a.modifier != "out var" || !a.value) { pos++; continue; }
        auto* id = dynamic_cast<const ast::IdentifierExpr*>(a.value.get());
        if (!id) { pos++; continue; }
        // Skip if slot already created.
        sema::Symbol* sym = ctx_.exprSymbol(*id);
        auto* loc = dynamic_cast<sema::LocalSymbol*>(sym);
        if (loc && loc->decl && locals_.count(loc->decl)) { pos++; continue; }
        if (locals_.count(id)) { pos++; continue; }
        // Determine slot type:
        // 1. From the sema-resolved method parameter at position `pos`.
        // 2. From the callee primitive type (for double/float TryParse).
        // 3. Default to i32.
        llvm::Type* slotTy = i32();
        if (ms) {
            auto* md = dynamic_cast<const ast::MethodDecl*>(ms->decl);
            if (md && pos < md->params.size() && md->params[pos].type) {
                sema::TypeRef* pt = ctx_.resolvedType(*md->params[pos].type);
                if (pt) slotTy = lowerTypeRef(pt);
            }
        } else if (callPrimTy != sema::PrimitiveType::None) {
            using P = sema::PrimitiveType;
            // For TryParse, the out result is the same primitive type.
            if (callPrimTy == P::Double) slotTy = llvm::Type::getDoubleTy(llvm_);
            else if (callPrimTy == P::Float) slotTy = llvm::Type::getFloatTy(llvm_);
            else if (callPrimTy == P::Long || callPrimTy == P::ULong) slotTy = i64();
            else slotTy = i32();
        } else if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(call.callee.get())) {
            // §3: collection instance calls (e.g. dict.TryGetValue).
            // Infer out-param type from the collection's generic args.
            if (ma->target) {
                auto* recvTy = ctx_.exprType(*ma->target);
                const auto& m = ma->member;
                if (isDictTypeName(recvTy) && m == "TryGetValue" &&
                    recvTy->genericArgs.size() >= 2 && recvTy->genericArgs[1]) {
                    // Value type = second generic arg of Dictionary<K,V>
                    slotTy = lowerTypeRef(recvTy->genericArgs[1]);
                }
            }
        }
        llvm::AllocaInst* slot = entryAlloca(slotTy);
        locals_[id] = slot;
        if (loc && loc->decl) locals_[loc->decl] = slot;
        pos++;
    }
}


// §8 / §9 / §10: intercept the compiler-synthesized value-semantics members
// — `ToString`, `GetHashCode`, and (for records) `Equals` — on a record- or
// enum-typed receiver when the type declares no user member of that name.
// Returns the lowered value, or null when this is not such a call.
llvm::Value* Emitter::tryLowerValueSemanticsCall(const ast::CallExpr& call) {
    auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(call.callee.get());
    if (!ma || !ma->target) return nullptr;
    const auto& member = ma->member;
    bool isToString    = member == "ToString"    && call.args.empty();
    bool isGetHashCode = member == "GetHashCode"  && call.args.empty();
    bool isEquals      = member == "Equals"       && call.args.size() == 1;
    if (!isToString && !isGetHashCode && !isEquals) return nullptr;

    // A user-declared method of this name wins (the callee resolves to it).
    if (dynamic_cast<sema::MethodSymbol*>(ctx_.exprSymbol(*call.callee)))
        return nullptr;

    auto* recvTy = ctx_.exprType(*ma->target);
    if (!recvTy || recvTy->kind != sema::TypeRef::Kind::Named ||
        !recvTy->declaration)
        return nullptr;
    const sema::TypeSymbol* ts = recvTy->declaration;
    bool isRecord = ts->kind == ast::TypeDeclKind::Record;
    bool isEnum   = ts->kind == ast::TypeDeclKind::Enum;
    if (!isRecord && !isEnum) return nullptr;

    llvm::Value* recv = lowerExpr(*ma->target);
    if (!recv) return nullptr;

    if (isEnum) {
        // §8.5: enum ToString → the type's qualified name (spec default).
        if (isToString)
            return lowerStringLiteral(
                std::string("\"") + std::string(ts->name) + "\"", false);
        // §8.3: enum GetHashCode → the underlying integer value (i32).
        if (isGetHashCode)
            return builder_.CreateZExtOrTrunc(recv, i32());
        // §8.4/§10: enum Equals → integer comparison.
        if (isEquals && call.args[0].value) {
            llvm::Value* other = lowerExpr(*call.args[0].value);
            if (!other) return nullptr;
            return builder_.CreateICmpEQ(
                recv, builder_.CreateZExtOrTrunc(other, recv->getType()));
        }
        return nullptr;
    }

    // §9.1 / §9.2: a positional / combined-form record always carries the
    // synthesized value-semantics members (the §9.1 package). A *body* record
    // carries them only when it is auto-equality-bearing (§9.2); an ineligible
    // body record gets the type-name default for ToString (§8.5) and no auto
    // equality.
    bool positional = false;
    if (auto* td = dynamic_cast<const ast::TypeDecl*>(ts->decl))
        positional = td->hasPositionalFields;
    bool autoBearing =
        positional || sema::isAutoEqualityBearingBodyRecord(ts);

    if (isToString && !autoBearing)
        // §8.5: an ineligible body record's ToString is the type-name default.
        return lowerStringLiteral(
            std::string("\"") + std::string(ts->name) + "\"", false);
    if ((isGetHashCode || isEquals) && !autoBearing)
        // §9.2: an ineligible body record has no auto equality. Do not
        // synthesize; fall through to ordinary dispatch (which finds the
        // user's own override if one exists).
        return nullptr;

    // Record: obtain the struct value (a `this` pointer or spill loads it).
    llvm::StructType* st = recordStruct(ts);
    if (recv->getType()->isPointerTy())
        recv = builder_.CreateLoad(st, recv);
    if (!recv->getType()->isStructTy()) return nullptr;

    if (isToString)    return emitRecordToString(recv, ts);   // §8.5 / §9.1 / §9.2
    if (isGetHashCode) return emitRecordHashCode(recv, ts);   // §8.3 / §9.1 / §9.2
    if (isEquals && call.args[0].value) {
        // §9.1 / §9.2: structural equality, same as the `=` operator (§8.3).
        llvm::Value* other = lowerExpr(*call.args[0].value);
        if (!other) return nullptr;
        if (other->getType()->isPointerTy())
            other = builder_.CreateLoad(st, other);
        if (!other->getType()->isStructTy()) return nullptr;
        // §9.1/§9.2: drive per-field comparison off the record's declared
        // member types so string fields compare by content (per §8.3).
        return emitStructEquality(recv, other, st, ts);
    }
    return nullptr;
}


// per §24: the EventSymbol an event-access expression refers to, or null.
// A bare identifier (`Clicked` — implicit `this`) or a member access
// (`obj.Clicked` / `this.Clicked`) both carry the EventSymbol on the expr.
const sema::EventSymbol* Emitter::eventSymbolOf(const ast::Expr& e) {
    if (!dynamic_cast<const ast::IdentifierExpr*>(&e) &&
        !dynamic_cast<const ast::MemberAccessExpr*>(&e))
        return nullptr;
    return dynamic_cast<const sema::EventSymbol*>(ctx_.exprSymbol(e));
}

// per §24.1: load the backing EventSource<T> pointer for an event access.
llvm::Value* Emitter::eventSourcePtr(const ast::Expr& e) {
    const sema::EventSymbol* es = eventSymbolOf(e);
    if (!es || !es->parent) return nullptr;
    auto idxIt = fieldIndex_.find(es);
    if (idxIt == fieldIndex_.end()) return nullptr;  // static event (deferred)
    // Receiver: implicit `this` for a bare identifier; the target object for
    // `obj.Clicked` / `this.Clicked`.
    llvm::Value* recv = nullptr;
    if (dynamic_cast<const ast::IdentifierExpr*>(&e)) {
        recv = currentThis_;
    } else if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
        if (ma->target) recv = lowerExpr(*ma->target);
    }
    if (!recv) return nullptr;
    llvm::StructType* st = classStruct(es->parent);
    llvm::Value* slot = builder_.CreateStructGEP(st, recv, idxIt->second);
    return builder_.CreateLoad(ptrTy(), slot, "event.src");
}

// per §24.1 / §24.3: the resolved payload type of an event member (the `: T`
// annotation), or null when the event has no payload (unit `()` form).
const sema::TypeRef* Emitter::eventPayloadType(const sema::EventSymbol* es) {
    if (!es) return nullptr;
    auto* ed = dynamic_cast<const ast::EventDecl*>(es->decl);
    if (!ed || !ed->type) return nullptr;  // no-payload event → unit
    return ctx_.resolvedType(*ed->type);
}

// per §24.5: invoke every active handler closure with `payloadLL` (the lowered
// payload value, or null for the no-payload unit form). Each handler is a
// Bezel closure { fn_ptr, env_ptr }; the call convention is
// (env_ptr, payload?) -> void. Handlers run in subscription order; with zero
// subscribers this is a no-op (no null check at the call site, per §24.5).
//
// NOTE (§24.5, deferred): "if a handler throws, subsequent handlers are not
// invoked and the exception propagates" — handler exception propagation is not
// wired here (handlers are plain closure calls without an unwind edge). With
// the current synchronous closure-call path a throwing handler would still
// abort the loop via the process-level unwind, but the spec's precise
// stop-and-propagate semantics are not yet enforced. Reported as deferred.
void Emitter::emitEventRaise(llvm::Value* srcPtr, llvm::Value* payloadLL,
                             llvm::Type* payloadTy) {
    if (!srcPtr) return;
    auto* countFn = declareC("bz_event_count", i64(), {ptrTy()});
    auto* atFn = declareC("bz_event_handler_at", ptrTy(), {ptrTy(), i64()});
    llvm::Value* n = builder_.CreateCall(countFn, {srcPtr}, "event.n");

    // Build the handler function type: (env_ptr, payload?) -> void.
    std::vector<llvm::Type*> handlerArgTys;
    handlerArgTys.push_back(ptrTy());  // env_ptr
    if (payloadLL) handlerArgTys.push_back(payloadTy);
    auto* handlerFnTy = llvm::FunctionType::get(
        llvm::Type::getVoidTy(llvm_), handlerArgTys, false);

    // for (i = 0; i < n; ++i) { c = handler_at(src, i); fn = c[0];
    //                           env = c[1]; fn(env, payload?); }
    llvm::Function* fn = currentFn_;
    auto* loopBB = llvm::BasicBlock::Create(llvm_, "raise.loop", fn);
    auto* bodyBB = llvm::BasicBlock::Create(llvm_, "raise.body", fn);
    auto* endBB = llvm::BasicBlock::Create(llvm_, "raise.end", fn);
    auto* iSlot = entryAlloca(i64());
    builder_.CreateStore(builder_.getInt64(0), iSlot);
    builder_.CreateBr(loopBB);

    builder_.SetInsertPoint(loopBB);
    llvm::Value* i = builder_.CreateLoad(i64(), iSlot);
    builder_.CreateCondBr(builder_.CreateICmpSLT(i, n), bodyBB, endBB);

    builder_.SetInsertPoint(bodyBB);
    llvm::Value* iv = builder_.CreateLoad(i64(), iSlot);
    llvm::Value* closure = builder_.CreateCall(atFn, {srcPtr, iv}, "event.h");
    llvm::Value* fnPtr = builder_.CreateLoad(ptrTy(), closure, "h.fn");
    llvm::Value* envSlot =
        builder_.CreateGEP(ptrTy(), closure, {builder_.getInt32(1)});
    llvm::Value* envPtr = builder_.CreateLoad(ptrTy(), envSlot, "h.env");
    std::vector<llvm::Value*> callArgs;
    callArgs.push_back(envPtr);
    if (payloadLL) callArgs.push_back(payloadLL);
    builder_.CreateCall(handlerFnTy, fnPtr, callArgs);
    builder_.CreateStore(builder_.CreateAdd(iv, builder_.getInt64(1)), iSlot);
    builder_.CreateBr(loopBB);

    builder_.SetInsertPoint(endBB);
}

std::optional<llvm::Value*> Emitter::tryLowerEventCall(const ast::CallExpr& call) {
    auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(call.callee.get());
    if (!ma || !ma->target) return std::nullopt;
    // The target of `.Subscribe`/`.Raise`/`.Add`/`.Remove` must itself be an
    // event access.
    const sema::EventSymbol* es = eventSymbolOf(*ma->target);
    if (!es) return std::nullopt;
    const auto& method = ma->member;
    const bool isSubscribe = method == "Subscribe" && call.args.size() == 1;
    const bool isAdd       = method == "Add"       && call.args.size() == 1;
    const bool isRemove    = method == "Remove"    && call.args.size() == 1;
    const bool isRaise     = method == "Raise";
    if (!isSubscribe && !isAdd && !isRemove && !isRaise)
        return std::nullopt;

    llvm::Value* src = eventSourcePtr(*ma->target);
    if (!src) return std::nullopt;

    if (isSubscribe || isAdd || isRemove) {
        // per §24.3 / stdlib §6.5: the handler argument is a function value
        // (lambda or method reference) lowered to a closure { fn, env }.
        // Inform lowerLambda of the expected handler type so an untyped lambda
        // param `(e) => …` resolves the payload type.
        const sema::TypeRef* saved = expectedFunctionType_;
        sema::TypeRef handlerTy;  // synthesized (payload) -> ()
        handlerTy.kind = sema::TypeRef::Kind::Function;
        if (const sema::TypeRef* pt = eventPayloadType(es)) {
            sema::TypeRef::FuncParam p;
            p.type = const_cast<sema::TypeRef*>(pt);
            handlerTy.funcParams.push_back(p);
        }
        expectedFunctionType_ = &handlerTy;
        llvm::Value* closure = lowerExpr(*call.args[0].value);
        expectedFunctionType_ = saved;
        if (!closure) return std::optional<llvm::Value*>(nullptr);
        if (isSubscribe) {
            auto* subFn =
                declareC("bz_event_subscribe", ptrTy(), {ptrTy(), ptrTy()});
            return std::optional<llvm::Value*>(
                builder_.CreateCall(subFn, {src, closure}, "subscription"));
        }
        if (isAdd) {
            auto* addFn = declareC("bz_event_add", i64(), {ptrTy(), ptrTy()});
            builder_.CreateCall(addFn, {src, closure});
            return std::optional<llvm::Value*>(nullptr);  // Add returns unit
        }
        // isRemove
        auto* remFn = declareC("bz_event_remove_closure",
                               llvm::Type::getVoidTy(llvm_), {ptrTy(), ptrTy()});
        builder_.CreateCall(remFn, {src, closure});
        return std::optional<llvm::Value*>(nullptr);  // Remove returns unit
    }

    // isRaise — per §24.5: invoke each subscribed handler with the payload.
    llvm::Value* payloadLL = nullptr;
    llvm::Type* payloadTy = nullptr;
    if (!call.args.empty() && call.args[0].value) {
        payloadLL = lowerExpr(*call.args[0].value);
        if (!payloadLL) return std::optional<llvm::Value*>(nullptr);
        payloadTy = payloadLL->getType();
    }
    emitEventRaise(src, payloadLL, payloadTy);
    return std::optional<llvm::Value*>(nullptr);  // Raise returns unit
}


// per §2.11 / §24.4: the built-in `Subscription` resolves (TypeResolution_Names)
// to a Named TypeRef with a null user declaration whose first name part is
// "Subscription". Same shape as the collection/StringBuilder built-in checks.
bool Emitter::isSubscriptionType(const sema::TypeRef* rt) {
    if (!rt || rt->kind != sema::TypeRef::Kind::Named || rt->declaration)
        return false;
    if (!rt->astNode || !rt->astNode->name || rt->astNode->name->parts.empty())
        return false;
    return rt->astNode->name->parts[0] == "Subscription";
}

bool Emitter::isSubscriptionExpr(const ast::Expr& e) {
    // Primary, type-based recognition (per §2.11): any expression whose static
    // type is `Subscription` disposes through the events runtime, regardless of
    // syntactic position — method-returned, parameter, field, array element, or
    // a local declared/inferred as Subscription. This replaces the previous
    // purely-syntactic recognition, which only saw a literal `.Subscribe(...)`.
    if (isSubscriptionType(ctx_.exprType(e))) return true;
    // Fallback for the inferred-local literal form: `var sub := …Subscribe(…)`.
    // The Subscribe call result is not (yet) typed as `Subscription` by sema,
    // so an inferred local bound to it carries no exprType; recognize it
    // structurally as before so the literal declarator keeps disposing.
    auto isSubscribeCall = [&](const ast::Expr& x) -> bool {
        auto* ce = dynamic_cast<const ast::CallExpr*>(&x);
        if (!ce || !ce->callee) return false;
        auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(ce->callee.get());
        if (!ma || ma->member != "Subscribe" || !ma->target ||
            ce->args.size() != 1)
            return false;
        return eventSymbolOf(*ma->target) != nullptr;
    };
    if (isSubscribeCall(e)) return true;
    if (dynamic_cast<const ast::IdentifierExpr*>(&e)) {
        if (auto* loc = dynamic_cast<sema::LocalSymbol*>(ctx_.exprSymbol(e))) {
            if (auto* ld = dynamic_cast<const ast::LocalDecl*>(loc->decl)) {
                if (ld->initializer) return isSubscribeCall(*ld->initializer);
            }
        }
    }
    return false;
}


// per §14.1/§14.2 + §26.5 (issue #391). Structurally unify a generic method's
// declared return TypeRef against the concrete return type sema solved from the
// call's target, binding any still-unbound method type parameter.
void Emitter::unifyMethodReturnTypeArg(
    const sema::TypeRef* declRet, const sema::TypeRef* resultTy,
    const std::vector<ast::GenericParam>& genericParams,
    std::unordered_map<std::string_view, llvm::Type*>& subst,
    std::unordered_map<std::string_view, const sema::TypeRef*>& semaTy,
    bool& added) {
    if (!declRet || !resultTy) return;
    // A bare type-parameter reference lowers to a Named TypeRef with no
    // declaration whose single name part is the parameter name (mirrors the
    // argument-inference loop and lowerTypeRef's generic-param fallback). When
    // that name is one of this method's parameters and it is not yet bound,
    // bind it to the matching concrete component of the result type.
    if (declRet->kind == sema::TypeRef::Kind::Named && !declRet->declaration &&
        declRet->astNode && declRet->astNode->name &&
        declRet->astNode->name->parts.size() == 1) {
        std::string_view nm = declRet->astNode->name->parts[0];
        bool isParam = false;
        for (const auto& gp : genericParams)
            if (gp.name == nm) { isParam = true; break; }
        if (isParam && !subst.count(nm)) {
            subst[nm] = lowerTypeRef(resultTy);
            semaTy[nm] = resultTy;
            added = true;
        }
        return;
    }
    // A constructed generic return type (`Box<T>`) unifies against a
    // same-shaped result (`Box<Widget>`): same declaration and generic-arg
    // count; recurse pairwise so the nested parameter is solved from the
    // result's matching argument (§26.5).
    if (declRet->kind == sema::TypeRef::Kind::Named && declRet->declaration &&
        resultTy->kind == sema::TypeRef::Kind::Named &&
        declRet->declaration == resultTy->declaration &&
        declRet->genericArgs.size() == resultTy->genericArgs.size()) {
        for (std::size_t i = 0; i < declRet->genericArgs.size(); ++i)
            unifyMethodReturnTypeArg(declRet->genericArgs[i],
                                     resultTy->genericArgs[i], genericParams,
                                     subst, semaTy, added);
    }
}


llvm::Value* Emitter::explicitDisposeOwnedReceiverSlot(
        const ast::Expr& receiver, llvm::Type*& slotTy) {
    // #415: classify the `.Dispose()` receiver as a slot/field THIS scope owns —
    // one whose nulling suppresses the only other reclaim of the object — so the
    // explicit site can reclaim the block once and null the slot (no double-free
    // with a following auto-dispose). See the helper's declaration for the cases.
    if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(&receiver)) {
        sema::Symbol* sym = ctx_.exprSymbol(*id);
        if (auto* ps = dynamic_cast<sema::ParameterSymbol*>(sym)) {
            // §28.8: an `owned` parameter carries the disposal obligation. (A
            // `ref`/`out`/borrow parameter does not — disposing it would free a
            // value the caller still owns.) Skip `out owned` (by-ref, callee
            // does not own at the dispose point).
            if (ps->modifier == "owned") return lvalueAddr(receiver, slotTy);
            return nullptr;
        }
        if (auto* loc = dynamic_cast<sema::LocalSymbol*>(sym)) {
            auto* ld = dynamic_cast<const ast::LocalDecl*>(loc->decl);
            if (!ld) return nullptr;
            // A `using var` local is owned by this scope (§28.10). A plain `var`
            // owned local (init `new C(...)` or an `owned T` return) is owned by
            // structure — exactly the §28.3 unwind-disposal eligibility. Either
            // way this scope's slot is the sole other reclaim path; nulling it
            // after reclaiming makes free happen exactly once. A borrow-
            // initialized local (no `owned`) returns null — its real owner
            // reclaims elsewhere, so reclaiming here would double-free.
            if (loc->isUsing) return lvalueAddr(receiver, slotTy);
            if (unwindOwnedLocalClass(ld)) return lvalueAddr(receiver, slotTy);
            return nullptr;
        }
        return nullptr;
    }
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&receiver)) {
        // §28.5: an `owned` reference field — disposed with the containing object
        // by the §28.4-step-2 owned-field auto-dispose, which null-guards its
        // reload. Reclaiming + nulling the field here makes the explicit
        // `field.Dispose()` (§28.5 line 5066) free the field once; the auto-
        // dispose then finds null and skips. A non-owned (borrow) field returns
        // null — the field's real owner frees it, so reclaiming would double-free.
        if (auto* fs = dynamic_cast<sema::FieldSymbol*>(ctx_.exprSymbol(*ma))) {
            if (isOwnedReferenceField(fs)) return lvalueAddr(receiver, slotTy);
        }
        return nullptr;
    }
    return nullptr;
}


llvm::Value* Emitter::lowerCall(const ast::CallExpr& call) {
    if (!call.callee) return nullptr;
    // Pre-create out var slots so lvalueAddr can find them in all paths.
    ensureOutVarSlots(call);
    // per §24.3/§24.5: event method calls — `<event>.Subscribe(h)` (returns an
    // owned Subscription), `.Add`/`.Remove` (the += / -= desugaring targets),
    // and `.Raise(payload)` / `.Raise()`. Checked first so the event member
    // access does not fall through to ordinary (no-such-method) dispatch.
    if (auto v = tryLowerEventCall(call)) return *v;
    // §8/§9/§10: compiler-synthesized value-semantics members (ToString,
    // GetHashCode, Equals) on a record/enum receiver. Checked before the
    // ordinary dispatch so a record with no user ToString gets the §9.1 form
    // rather than falling through to the no-method path.
    if (auto* v = tryLowerValueSemanticsCall(call)) return v;
    // CG-console: Console.Write / Console.WriteLine / Console.WriteLine()
    if (tryLowerConsoleCall(call)) return nullptr;  // unit result
    // Console.ReadLine() → string? / Console.ReadKey() → char
    if (call.args.empty() && call.callee) {
        if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(call.callee.get())) {
            if (ma->target) {
                if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(ma->target.get())) {
                    if (id->name == "Console") {
                        if (ma->member == "ReadLine") return emitConsoleReadLine();
                        if (ma->member == "ReadKey") {
                            // getchar() → char (i32)
                            auto* gcFn = module_->getFunction("getchar");
                            if (!gcFn) {
                                auto* ft = llvm::FunctionType::get(i32(), {}, false);
                                gcFn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "getchar", module_.get());
                            }
                            return builder_.CreateCall(gcFn, {}, "readkey");
                        }
                    }
                }
            }
        }
    }
    // §3.4: primitive type static methods: int.Parse(s), double.Parse(s), etc.
    if (auto* v = tryLowerPrimitiveStaticCall(call)) return v;
    // stdlib §5: deferred LINQ operators (Where/Select/Take/Skip/Distinct/
    // OrderBy[Descending]) and the ToArray materializer. Handled before the
    // collection path so they intercept LINQ calls on both Array<T> receivers
    // and lazy IEnumerable<T> receivers (chaining).
    if (auto* v = tryLowerLazyLinqCall(call)) return v;
    // §3: stdlib collection instance methods: arr.Add, arr.Get, dict.TryGetValue, etc.
    if (auto* v = tryLowerCollectionInstanceCall(call)) return v;
    // stdlib §3.2: Encoding.Utf8.GetBytes(s) / Encoding.Utf8.GetString(bytes).
    if (auto* v = tryLowerEncodingCall(call)) return v;
    // Built-in class methods: Convert.ToInt32, Math.Abs, etc.
    if (auto* v = tryLowerBuiltinClassCall(call)) return v;
    // CG44: explicit conversion `int(x)` (§5.2) — a callee that is a
    // primitive type name (no resolved symbol) converts its argument.
    if (auto* cid =
            dynamic_cast<const ast::IdentifierExpr*>(call.callee.get())) {
        if (!ctx_.exprSymbol(*call.callee)) {
            sema::PrimitiveType pt = sema::primitiveFromName(cid->name);
            if (pt != sema::PrimitiveType::None)
                return lowerConversion(pt, call);
        }
    }
    sema::Symbol* calleeSym = ctx_.exprSymbol(*call.callee);
    // CG44: an enum cast `E(i)` (§5.4) — the callee is an enum type
    // name. An enum is represented by its underlying integer (i32), so
    // the argument, width-adjusted to i32, is the enum value.
    if (auto* ets = dynamic_cast<sema::TypeSymbol*>(calleeSym)) {
        if (ets->kind == ast::TypeDeclKind::Enum)
            return lowerEnumCast(call);
        // §9.5: records are constructed with `new Record(...)` (handled in
        // lowerNew), not call-style `Record(...)`. A bare call on a record
        // type name is not construction and is rejected in sema.
    }
    // CG26: a call to an extern (FFI) function — direct C-ABI call to
    // the external declaration. (Native functions don't set the
    // exception global, so no post-call unwind check is needed.)
    if (auto* xd = dynamic_cast<sema::ExternDeclSymbol*>(calleeSym)) {
        auto it = externFns_.find(xd);
        if (it == externFns_.end()) return nullptr;
        const ast::ExternDecl* ed = xd->externDecl;
        // per §32.5: an extern parameter declared `CString` is a
        // null-terminated UTF-8 `char*`. When a Bezel `string` is passed
        // there, marshal it to a null-terminated UTF-8 buffer (§32.6 "the C
        // side reads a char* until the null"). For a width-1 string the raw
        // data pointer (field 0) is already null-terminated ASCII/UTF-8 and is
        // handed over directly (the fast path). A width>1 string stores its
        // code points in 2- or 4-byte units with interior zero bytes, so the
        // raw pointer would truncate at the first such byte (bug B); for those
        // we re-encode to a fresh UTF-8 buffer via bz_string_to_utf8z.
        auto cstringParamAt = [&](std::size_t i) -> bool {
            if (!ed || i >= ed->params.size()) return false;
            const ast::Type* pt = ed->params[i].type.get();
            return pt && pt->kind == ast::TypeKind::Named && pt->name &&
                   pt->name->parts.size() == 1 &&
                   pt->name->parts.front() == "CString";
        };
        std::vector<llvm::Value*> args;
        std::size_t idx = 0;
        for (const auto& a : call.args) {
            if (!a.value) return nullptr;
            llvm::Value* v = lowerExpr(*a.value);
            if (!v) return nullptr;
            // Marshal a Bezel `string` argument into a CString parameter
            // (§32.5 / §32.6). Branch on the string's runtime byte width:
            // width==1 → raw data pointer (already null-terminated UTF-8);
            // width>1  → re-encode to a fresh null-terminated UTF-8 buffer so
            //            interior zero bytes don't truncate the C-side read.
            if (cstringParamAt(idx)) {
                const sema::TypeRef* at = ctx_.exprType(*a.value);
                if (at && at->kind == sema::TypeRef::Kind::Primitive &&
                    at->primitive == sema::PrimitiveType::String) {
                    llvm::Value* strPtr = v;
                    llvm::Value* w = loadStringWidth(strPtr);
                    llvm::Value* isW1 = builder_.CreateICmpEQ(
                        w, llvm::ConstantInt::get(
                               llvm::Type::getInt8Ty(llvm_), 1), "cstr.w1");
                    auto* w1BB  = llvm::BasicBlock::Create(llvm_, "cstr.ascii", currentFn_);
                    auto* wnBB  = llvm::BasicBlock::Create(llvm_, "cstr.utf8", currentFn_);
                    auto* endBB = llvm::BasicBlock::Create(llvm_, "cstr.end", currentFn_);
                    builder_.CreateCondBr(isW1, w1BB, wnBB);
                    // width==1: raw data pointer.
                    builder_.SetInsertPoint(w1BB);
                    llvm::Value* rawData = loadStringData(strPtr);
                    auto* w1End = builder_.GetInsertBlock();
                    builder_.CreateBr(endBB);
                    // width>1: bz_string_to_utf8z(strPtr) → fresh char*.
                    builder_.SetInsertPoint(wnBB);
                    llvm::Function* enc = declareC(
                        "bz_string_to_utf8z", ptrTy(), {ptrTy()});
                    llvm::Value* encData = builder_.CreateCall(enc, {strPtr});
                    auto* wnEnd = builder_.GetInsertBlock();
                    builder_.CreateBr(endBB);
                    builder_.SetInsertPoint(endBB);
                    auto* phi = builder_.CreatePHI(ptrTy(), 2, "cstr.data");
                    phi->addIncoming(rawData, w1End);
                    phi->addIncoming(encData, wnEnd);
                    v = phi;
                }
            }
            args.push_back(v);
            ++idx;
        }
        if (args.size() != it->second->arg_size()) return nullptr;
        return builder_.CreateCall(it->second, args);
    }
    // CG60: call through a function-typed parameter or local (lambda
    // value). Lambda values are heap-allocated closure structs
    // { ptr fn, ptr env }. The function's actual type includes an env_ptr
    // as first arg: (ptr env, user_params...) -> ret.
    auto emitClosureCall = [&](llvm::Value* closurePtr,
                               const sema::TypeRef* rt) -> llvm::Value* {
        if (!closurePtr || !rt ||
            rt->kind != sema::TypeRef::Kind::Function)
            return nullptr;
        // Load fn_ptr from closure[0].
        llvm::Value* fnPtr = builder_.CreateLoad(ptrTy(), closurePtr);
        // Load env_ptr from closure[1].
        llvm::Value* envSlot = builder_.CreateGEP(
            ptrTy(), closurePtr, {builder_.getInt32(1)});
        llvm::Value* envPtr = builder_.CreateLoad(ptrTy(), envSlot);
        // Build call args first so we can reconcile types.
        std::vector<llvm::Value*> args;
        args.push_back(envPtr);  // env_ptr first
        for (const auto& a : call.args) {
            if (!a.value) return nullptr;
            llvm::Value* v = lowerExpr(*a.value);
            if (!v) return nullptr;
            args.push_back(v);
        }
        // Build function type: (ptr env, user_params...) -> ret.
        // Use the actual argument LLVM types where the declared type resolves
        // to i32 (unresolved generic param T) — e.g. (T)->T with T=string
        // needs ptr->ptr, not i32->i32.
        llvm::Type* retTy = rt->funcReturn
                                ? lowerTypeRef(rt->funcReturn)
                                : llvm::Type::getVoidTy(llvm_);
        // CG-generic: if return type is i32 (unresolved generic param C),
        // try to resolve via the callee's localGenericSubst_.  This handles
        // the case of a generic method returning a function type (B)->C where
        // C is a type parameter — the local variable's stored substitution
        // maps C to the concrete type (e.g. f64 for Curried<…,double>).
        if (retTy == i32() && rt->funcReturn) {
            auto* fr = rt->funcReturn;
            if (fr->kind == sema::TypeRef::Kind::Named && !fr->declaration &&
                fr->astNode && fr->astNode->name &&
                fr->astNode->name->parts.size() == 1) {
                std::string_view paramName = fr->astNode->name->parts[0];
                // Look up in the callee's localGenericSubst_.
                sema::Symbol* calleeSym2 = ctx_.exprSymbol(*call.callee);
                const ast::Node* decl = nullptr;
                if (auto* loc = dynamic_cast<sema::LocalSymbol*>(calleeSym2))
                    decl = loc->decl;
                if (decl) {
                    auto lit = localGenericSubst_.find(decl);
                    if (lit != localGenericSubst_.end()) {
                        auto sit = lit->second.find(paramName);
                        if (sit != lit->second.end() && sit->second != i32())
                            retTy = sit->second;
                    }
                }
                // Also try current genericSubst_.
                if (retTy == i32()) {
                    auto sit = genericSubst_.find(paramName);
                    if (sit != genericSubst_.end() && sit->second != i32())
                        retTy = sit->second;
                }
            }
        }
        std::vector<llvm::Type*> allArgTys;
        allArgTys.push_back(ptrTy());  // env
        // Resolve callee's generic substitution for param/return types.
        // When the closure is a local variable from a generic method call,
        // its localGenericSubst_ has the concrete type mapping.
        const std::unordered_map<std::string_view, llvm::Type*>* calleeSubst = nullptr;
        {
            sema::Symbol* calleeSym3 = ctx_.exprSymbol(*call.callee);
            const ast::Node* decl3 = nullptr;
            if (auto* loc = dynamic_cast<sema::LocalSymbol*>(calleeSym3))
                decl3 = loc->decl;
            if (decl3) {
                auto lit = localGenericSubst_.find(decl3);
                if (lit != localGenericSubst_.end())
                    calleeSubst = &lit->second;
            }
        }
        for (std::size_t pi = 0; pi < rt->funcParams.size(); ++pi) {
            llvm::Type* declared =
                rt->funcParams[pi].type ? lowerTypeRef(rt->funcParams[pi].type) : i32();
            // CG-generic: if declared resolved to i32 (unresolved generic T):
            // 1. Try callee's localGenericSubst_ to resolve the generic param name.
            // 2. If still i32, upgrade to ptr if the actual arg is a pointer.
            if (declared == i32() && rt->funcParams[pi].type) {
                auto* fp = rt->funcParams[pi].type;
                if (fp->kind == sema::TypeRef::Kind::Named && !fp->declaration &&
                    fp->astNode && fp->astNode->name &&
                    fp->astNode->name->parts.size() == 1 && calleeSubst) {
                    auto sit = calleeSubst->find(fp->astNode->name->parts[0]);
                    if (sit != calleeSubst->end() && sit->second != i32())
                        declared = sit->second;
                }
            }
            std::size_t ai = pi + 1;  // +1 for env_ptr
            if (declared == i32() && ai < args.size() &&
                args[ai]->getType()->isPointerTy())
                declared = args[ai]->getType();
            allArgTys.push_back(declared);
        }
        // Coerce args to match declared param types (e.g. f64 → i32 for
        // an untyped lambda param used with a double argument — ideally the
        // caller annotates the lambda, but we convert rather than crash).
        for (std::size_t ai = 1; ai < args.size(); ++ai) {
            llvm::Type* declTy = (ai < allArgTys.size()) ? allArgTys[ai] : nullptr;
            if (!declTy || args[ai]->getType() == declTy) continue;
            if (declTy->isIntegerTy() && args[ai]->getType()->isFloatingPointTy())
                args[ai] = builder_.CreateFPToSI(args[ai], declTy);
            else if (declTy->isFloatingPointTy() && args[ai]->getType()->isIntegerTy())
                args[ai] = builder_.CreateSIToFP(args[ai], declTy);
            else if (declTy->isIntegerTy() && args[ai]->getType()->isIntegerTy()) {
                unsigned db = declTy->getIntegerBitWidth();
                unsigned sb = args[ai]->getType()->getIntegerBitWidth();
                if (db > sb) args[ai] = builder_.CreateSExt(args[ai], declTy);
                else args[ai] = builder_.CreateTrunc(args[ai], declTy);
            }
        }
        // Similarly fix up the return type if it's an unresolved generic.
        // ONLY upgrade if funcReturn is an unresolved generic (Named with no decl)
        // AND its type parameter name matches at least one of the param type
        // parameter names — this ensures '(T)->T' upgrades but '(S)->A' does not
        // (different type params mean S=string doesn't imply A=string).
        if (retTy == i32() && !args.empty() &&
            rt->funcReturn &&
            rt->funcReturn->kind == sema::TypeRef::Kind::Named &&
            !rt->funcReturn->declaration &&
            rt->funcReturn->astNode && rt->funcReturn->astNode->name &&
            rt->funcReturn->astNode->name->parts.size() == 1) {
            std::string_view retParamName = rt->funcReturn->astNode->name->parts[0];
            // Check if any param has the same type parameter name.
            bool retMatchesParam = false;
            for (const auto& fp : rt->funcParams) {
                if (!fp.type) continue;
                if (fp.type->kind == sema::TypeRef::Kind::Named &&
                    !fp.type->declaration && fp.type->astNode &&
                    fp.type->astNode->name &&
                    fp.type->astNode->name->parts.size() == 1 &&
                    fp.type->astNode->name->parts[0] == retParamName) {
                    retMatchesParam = true; break;
                }
            }
            if (retMatchesParam) {
                // If all non-env params are ptr, the return is also ptr.
                bool anyPtr = false;
                for (std::size_t ai = 1; ai < args.size(); ++ai)
                    if (args[ai]->getType()->isPointerTy()) { anyPtr = true; break; }
                if (anyPtr && allArgTys.size() > 1) {
                    bool allPtrs = true;
                    for (std::size_t ti = 1; ti < allArgTys.size(); ++ti)
                        if (!allArgTys[ti]->isPointerTy()) { allPtrs = false; break; }
                    if (allPtrs) retTy = ptrTy();
                }
            }
        }
        auto* fnTy = llvm::FunctionType::get(retTy, allArgTys, false);
        if (args.size() != allArgTys.size()) return nullptr;
        return builder_.CreateCall(fnTy, fnPtr, args);
    };
    if (auto* par = dynamic_cast<sema::ParameterSymbol*>(calleeSym)) {
        sema::TypeRef* rt = par->declType
                                ? ctx_.resolvedType(*par->declType)
                                : nullptr;
        if (rt && rt->kind == sema::TypeRef::Kind::Function) {
            llvm::Value* closurePtr = nullptr;
            auto pit = paramSlots_.find(par);
            if (pit != paramSlots_.end()) {
                // §27.5: a captured function-typed param reads its closure
                // value through the shared cell.
                llvm::Type* ety = nullptr;
                llvm::Value* addr = boxedStorage(pit->second, ety);
                closurePtr = builder_.CreateLoad(ptrTy(), addr);
            }
            return emitClosureCall(closurePtr, rt);
        }
    }
    if (auto* loc = dynamic_cast<sema::LocalSymbol*>(calleeSym)) {
        sema::TypeRef* rt = loc->declType;
        if (rt && rt->kind == sema::TypeRef::Kind::Function) {
            llvm::Value* closurePtr = nullptr;
            if (loc->decl) {
                auto lit = locals_.find(loc->decl);
                if (lit != locals_.end()) {
                    // §27.5: a captured function-typed local reads through
                    // its shared cell.
                    llvm::Type* ety = nullptr;
                    llvm::Value* addr = boxedStorage(lit->second, ety);
                    closurePtr = builder_.CreateLoad(ptrTy(), addr);
                }
            }
            return emitClosureCall(closurePtr, rt);
        }
    }
    // CG60: call through a function-typed field or auto-property —
    // `obj._callback()` where `_callback: () -> int`. The FieldSymbol has
    // a declared type (from the FieldDecl); if it's a Function TypeRef,
    // lower the field access as a closure pointer and dispatch.
    if (auto* fs = dynamic_cast<sema::FieldSymbol*>(calleeSym)) {
        auto* fd = dynamic_cast<const ast::FieldDecl*>(fs->decl);
        if (fd && fd->type) {
            sema::TypeRef* rt = ctx_.resolvedType(*fd->type);
            if (rt && rt->kind == sema::TypeRef::Kind::Function) {
                llvm::Value* closurePtr = lowerExpr(*call.callee);
                return emitClosureCall(closurePtr, rt);
            }
        }
    }
    // CG60: call through an indexed array element of function type —
    // `handlers[i]()` where handlers: [N](T) -> R. The IndexExpr has
    // no resolved symbol (calleeSym = null), so we lower the IndexExpr
    // directly and use its exprType to drive emitClosureCall.
    if (!calleeSym) {
        sema::TypeRef* calleeTy = ctx_.exprType(*call.callee);
        if (calleeTy && calleeTy->kind == sema::TypeRef::Kind::Function) {
            llvm::Value* closurePtr = lowerExpr(*call.callee);
            return emitClosureCall(closurePtr, calleeTy);
        }
    }
    auto* ms = dynamic_cast<sema::MethodSymbol*>(calleeSym);
    if (!ms) {
        // CG13/CG18: explicit `obj.Dispose()` call where Dispose is a
        // compiler-generated method (no MethodSymbol in sema). Emit a
        // dynamic vtable dispatch to the object's Dispose function (slot 1).
        if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(
                call.callee.get())) {
            if (ma->member == "Dispose" && ma->target) {
                // per §2.11 / §24.4: disposing a Subscription removes the
                // handler from its event source. A Subscription is the value
                // produced by `<event>.Subscribe(...)`; a local bound to such a
                // call (`var sub := …Subscribe(…)`) carries no resolved type,
                // so detect it structurally (the local's initializer is an
                // event Subscribe call, or the receiver is the call itself).
                if (isSubscriptionExpr(*ma->target)) {
                    llvm::Value* recv = lowerExpr(*ma->target);
                    if (recv && recv->getType()->isPointerTy()) {
                        auto* fn = declareC("bz_subscription_dispose",
                            llvm::Type::getVoidTy(llvm_), {ptrTy()});
                        builder_.CreateCall(fn, {recv});
                    }
                    return nullptr;
                }
                llvm::Value* recv = lowerExpr(*ma->target);
                if (recv && recv->getType()->isPointerTy()) {
                    // §3: collection types use bz_array_free / bz_dict_free.
                    auto* recvTy = ctx_.exprType(*ma->target);
                    // issues #343 / D8: a Set is dict-backed (records,
                    // primitives, strings — see lowerNew), so it frees with
                    // bz_dict_free, not bz_array_free.
                    bool isSetName = recvTy && recvTy->astNode &&
                        recvTy->astNode->name &&
                        !recvTy->astNode->name->parts.empty() &&
                        (recvTy->astNode->name->parts[0] == "Set" ||
                         recvTy->astNode->name->parts[0] == "OwnedSet");
                    bool dictBackedSet = isSetName &&
                        genericBackingType(recvTy, 0, /*isSet=*/true, nullptr)
                            != nullptr;
                    if (dictBackedSet) {
                        auto* fn = declareC("bz_dict_free",
                            llvm::Type::getVoidTy(llvm_), {ptrTy(), ptrTy()});
                        builder_.CreateCall(fn, {recv,
                            llvm::ConstantPointerNull::get(ptrTy())});
                    } else if (isCollectionTypeName(recvTy)) {
                        // #362 / stdlib §4.5: explicit Dispose() on an owning
                        // collection cascades disposal into its class elements;
                        // a borrowing collection frees only the backing.
                        llvm::Function* elemDisp =
                            owningCollectionElemDisposer(recvTy);
                        llvm::Value* disp = elemDisp
                            ? llvm::cast<llvm::Value>(elemDisp)
                            : llvm::ConstantPointerNull::get(ptrTy());
                        auto* fn = declareC("bz_array_free",
                            llvm::Type::getVoidTy(llvm_), {ptrTy(), ptrTy()});
                        builder_.CreateCall(fn, {recv, disp});
                    } else if (isDictTypeName(recvTy)) {
                        // #396 / stdlib §4.9 + #401 / stdlib §4.8: explicit
                        // Dispose() on a hash-backed dictionary frees the backing
                        // table (and key bytes) and runs the per-value callback,
                        // the same one the #394/#401 scope-end path uses:
                        //   * OwnedDictionary<K, class> → dispose+free each
                        //     surviving class value (#396/#394).
                        //   * borrowing Dictionary<K, record> → free each
                        //     surviving record value BOX (#401; a record is a
                        //     value type, nothing to dispose, §3.2 / §9).
                        // A scalar/string-valued borrowing Dictionary stores no
                        // box → null disposer (only the backing is freed). The
                        // two value callbacks are mutually exclusive, so no box
                        // is freed twice.
                        llvm::Function* valDisp =
                            owningDictValueDisposer(recvTy);
                        if (!valDisp) valDisp = dictValueBoxFreer(recvTy);
                        llvm::Value* disp = valDisp
                            ? llvm::cast<llvm::Value>(valDisp)
                            : llvm::ConstantPointerNull::get(ptrTy());
                        auto* fn = declareC("bz_dict_free",
                            llvm::Type::getVoidTy(llvm_), {ptrTy(), ptrTy()});
                        builder_.CreateCall(fn, {recv, disp});
                        // §28.2 idempotency: a `using var` dict (or an owned
                        // dict field) that is explicitly Dispose()d is ALSO
                        // disposed at scope end (the #394 `it->dict` path) or by
                        // a later explicit call. bz_dict_free is NOT idempotent
                        // — a second call on the same freed table double-frees
                        // it (and would double-dispose its values). Null the
                        // receiver's slot so any subsequent bz_dict_free reloads
                        // null and no-ops (bz_dict_free(null) returns). This
                        // makes "first Dispose disposes; subsequent calls are
                        // no-ops" (§28.2) hold for the dict's combined paths.
                        // lvalueAddr yields the slot for an identifier receiver
                        // (using-var local or owned field, boxed or not); for a
                        // non-lvalue receiver (a temporary) there is no second
                        // path to guard. Restricted to the identifier / member-
                        // access lvalue forms — the ones that name a using-var
                        // local or an owned field — so re-deriving the address
                        // re-evaluates only a side-effect-free base (the local's
                        // slot, or `this`/the owner for a field).
                        bool lvalueRecv =
                            dynamic_cast<const ast::IdentifierExpr*>(
                                ma->target.get()) ||
                            dynamic_cast<const ast::MemberAccessExpr*>(
                                ma->target.get());
                        llvm::Type* slotTy = nullptr;
                        if (lvalueRecv) {
                            if (llvm::Value* slot =
                                    lvalueAddr(*ma->target, slotTy)) {
                                builder_.CreateStore(
                                    llvm::ConstantPointerNull::get(ptrTy()),
                                    slot);
                            }
                        }
                    } else {
                        // #415 / §28.2 / §28.4: an explicit `.Dispose()` on a
                        // class/interface instance. After #411 split dispatch
                        // from reclamation, emitDynamicDispose only DISPATCHES
                        // the idempotent Dispose (destructor + owned fields +
                        // base chain, run AT MOST ONCE per object — §28.12 line
                        // 5624). The block is reclaimed by the OWNING auto-
                        // dispose site (emitDisposeAndReclaim). So when an owned
                        // value's ONLY disposal is an explicit `.Dispose()` (a
                        // plain `var` owned local or `owned` parameter disposed
                        // via `x.Dispose()`, with no auto-dispose follow), the
                        // destructor ran but the heap block LEAKED.
                        //
                        // Fix: when the receiver is a slot/field this scope OWNS
                        // — and thus can NULL to block a second reclaim — RECLAIM
                        // the block here and null that slot, mirroring the auto-
                        // dispose site (LlvmBackend_Setup.cpp). §28.2: "the first
                        // call performs the disposal; subsequent calls are no-
                        // ops" — the run-once flag (§28.12) makes a second
                        // DISPATCH a no-op, but a second RECLAIM (free) would be
                        // a double-free, so only the FIRST disposal frees and the
                        // slot is nulled so a following auto-dispose (using-var
                        // scope-end, owned-field auto-dispose, unwind) reloads
                        // null and skips. The scope-end / field / unwind paths
                        // all null-guard their reload (LlvmBackend_Setup.cpp), so
                        // free happens EXACTLY ONCE across explicit + auto.
                        //
                        // Conservative fallback (per #415): when the receiver is
                        // NOT a slot this scope owns (a temporary like
                        // `new R().Dispose()`, a borrow, or a non-owned field) we
                        // CANNOT null a slot to prevent a second reclaim — a
                        // borrow's real owner reclaims elsewhere via ITS slot —
                        // so reclaiming could double-free. Stay dispatch-only for
                        // those (the destructor still runs idempotently); an
                        // owned TEMPORARY explicitly disposed (no second reclaim
                        // path, but no slot to null either) is the deferred sub-
                        // case noted in #415.
                        llvm::Type* slotTy = nullptr;
                        llvm::Value* slot =
                            explicitDisposeOwnedReceiverSlot(*ma->target,
                                                             slotTy);
                        if (slot) {
                            emitDisposeAndReclaim(recv);
                            builder_.CreateStore(
                                llvm::ConstantPointerNull::get(ptrTy()), slot);
                        } else {
                            emitDynamicDispose(recv);
                        }
                    }
                    return nullptr;
                }
            }
        }
        return nullptr;
    }

    std::vector<llvm::Value*> args;
    // issue #421 / #410: source expression per arg (index-aligned with `args`)
    // so emitMethodCall can recover each argument's signedness when widening.
    std::vector<const ast::Expr*> argExprs;
    bool baseQualified = false;  // `base.M()` → non-virtual (§12.3)
    if (!isStaticMethod(ms)) {
        // Receiver: the member-access target, else the current `this`.
        llvm::Value* recv = nullptr;
        if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(
                call.callee.get())) {
            if (auto* tid = dynamic_cast<const ast::IdentifierExpr*>(
                    ma->target.get())) {
                if (tid->name == "base") baseQualified = true;
            }
            recv = ma->target ? lowerExpr(*ma->target) : nullptr;
        } else {
            recv = currentThis_;
        }
        if (!recv) return nullptr;
        // CG29: a record method receives `this` as a pointer to the record
        // value; if the receiver is a struct value (the usual case), spill
        // it to a stack slot and pass its address.
        if (ms->parent && ms->parent->kind == ast::TypeDeclKind::Record &&
            !recv->getType()->isPointerTy()) {
            llvm::Value* tmp = entryAlloca(recordStruct(ms->parent));
            builder_.CreateStore(recv, tmp);
            recv = tmp;
        }
        args.push_back(recv);
        argExprs.push_back(nullptr);  // receiver has no widening signedness
    }
    // §22.4/§22.7: place arguments into parameter order (named by name,
    // positional by position, omitted parameters defaulted).
    if (auto* md = dynamic_cast<const ast::MethodDecl*>(ms->decl)) {
        if (!buildOrderedArgs(md->params, call.args, args, &argExprs))
            return nullptr;
    } else {
        for (const auto& a : call.args) {
            if (!a.value) return nullptr;
            llvm::Value* v = lowerExpr(*a.value);
            if (!v) return nullptr;
            args.push_back(v);
            argExprs.push_back(a.value.get());
        }
    }

    // CG-generic: before calling the method, if the receiver's exprType
    // has generic args and the parent TypeSymbol has genericParams,
    // set up the substitution for the method body specialization.
    auto savedSubst = genericSubst_;
    auto savedSemaTy = genericSubstSemaTy_;
    if (!isStaticMethod(ms) && ms->parent && !args.empty()) {
        // Find the receiver expression to get its type.
        const ast::Expr* recvExpr = nullptr;
        if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(
                call.callee.get()))
            recvExpr = ma->target.get();
        if (recvExpr) {
            sema::TypeRef* recvTy = ctx_.exprType(*recvExpr);
            if (recvTy && recvTy->kind == sema::TypeRef::Kind::Named &&
                recvTy->declaration == ms->parent &&
                !recvTy->genericArgs.empty()) {
                auto* td = dynamic_cast<const ast::TypeDecl*>(
                    ms->parent->decl);
                if (td && td->genericParams.size() ==
                          recvTy->genericArgs.size()) {
                    // Resolve generic args using the OUTER substitution so
                    // that `Pair<T>` in `Pair<string>` context resolves T→ptr.
                    std::unordered_map<std::string_view, llvm::Type*> newSubst;
                    std::unordered_map<std::string_view, const sema::TypeRef*> newSema;
                    for (std::size_t gi = 0;
                         gi < td->genericParams.size(); ++gi) {
                        llvm::Type* concrTy = lowerTypeRef(recvTy->genericArgs[gi]);
                        std::string_view pname = td->genericParams[gi].name;
                        newSubst[pname] = concrTy;
                        newSema[pname] = recvTy->genericArgs[gi];
                    }
                    // Check if any resolved arg is still i32 (unresolved generic
                    // fallback). If so, try the localGenericSubst_ for the receiver.
                    bool hasUnresolved = false;
                    for (auto& [name, ty] : newSubst)
                        if (ty == i32()) { hasUnresolved = true; break; }
                    if (hasUnresolved) {
                        // Try looking up the local's stored substitution.
                        sema::Symbol* recvSym = ctx_.exprSymbol(*recvExpr);
                        const ast::Node* recvDecl = nullptr;
                        if (auto* loc = dynamic_cast<sema::LocalSymbol*>(recvSym))
                            recvDecl = loc->decl;
                        if (recvDecl) {
                            auto lit = localGenericSubst_.find(recvDecl);
                            if (lit != localGenericSubst_.end())
                                newSubst = lit->second;  // use stored concrete subst
                        }
                    }
                    genericSubst_ = std::move(newSubst);
                    genericSubstSemaTy_ = std::move(newSema);
                }
            }
        }
    }
    // per §14.1/§14.2: a generic *method* (its own `<T>` params, e.g.
    // `static method Clone<T>(proto: T): T where T: class, new()`) is
    // monomorphized by inferring each method type parameter from the actual
    // argument types. Without this, a `new T()` in the body has no
    // substitution and constructs nothing (issue #361 / D12). This handles
    // both static and instance generic methods; the receiver-class path above
    // covers class-level params and runs first, so this only adds the
    // method-level params it left unset.
    if (auto* md = dynamic_cast<const ast::MethodDecl*>(ms->decl)) {
        if (!md->genericParams.empty()) {
            std::unordered_map<std::string_view, llvm::Type*> newSubst =
                genericSubst_;
            std::unordered_map<std::string_view, const sema::TypeRef*> newSema =
                genericSubstSemaTy_;
            for (const auto& gp : md->genericParams) {
                if (newSubst.count(gp.name)) continue;  // already bound
                // Find a value parameter whose declared type is exactly this
                // type parameter, and read the matching call argument's type.
                for (std::size_t pi = 0; pi < md->params.size() &&
                     pi < call.args.size(); ++pi) {
                    const ast::Type* pt = md->params[pi].type.get();
                    if (!pt || pt->kind != ast::TypeKind::Named || !pt->name ||
                        pt->name->parts.size() != 1 ||
                        pt->name->parts[0] != gp.name)
                        continue;
                    if (!call.args[pi].value) continue;
                    sema::TypeRef* at = ctx_.exprType(*call.args[pi].value);
                    if (!at) continue;
                    newSubst[gp.name] = lowerTypeRef(at);
                    newSema[gp.name] = at;
                    break;
                }
            }
            genericSubst_ = std::move(newSubst);
            genericSubstSemaTy_ = std::move(newSema);
            // per §14.1/§14.2 + §26.5 (issue #391): a method type parameter
            // that appears *only* in the return type — `Create<T>(): T` — is
            // bound by no argument, so the loop above leaves it unset and
            // `new T()` in the body would lower `T` to the i32 fallback (a
            // miscompile that constructs nothing). Bezel has no call-site
            // type-argument syntax (§A.7), so sema solves such a parameter from
            // the call's *target* type and re-types the call to the substituted
            // return type (recorded on the call's exprType). Recover the
            // parameter here by unifying the method's *declared* return type
            // against that concrete result type, then prime the substitution
            // before the body is emitted.
            sema::TypeRef* resultTy = ctx_.exprType(call);
            sema::TypeRef* declRet = md->returnType
                                         ? ctx_.resolvedType(*md->returnType)
                                         : nullptr;
            if (resultTy && declRet) {
                std::unordered_map<std::string_view, llvm::Type*> retSubst =
                    genericSubst_;
                std::unordered_map<std::string_view, const sema::TypeRef*>
                    retSema = genericSubstSemaTy_;
                bool added = false;
                // Unify the declared return TypeRef against the concrete result
                // TypeRef, binding any still-unbound method type parameter to
                // the matching concrete component. Handles a bare `T` return as
                // well as a constructed `Box<T>` whose nested parameter is
                // solved from the result's generic argument (§26.5).
                unifyMethodReturnTypeArg(declRet, resultTy, md->genericParams,
                                         retSubst, retSema, added);
                if (added) {
                    genericSubst_ = std::move(retSubst);
                    genericSubstSemaTy_ = std::move(retSema);
                }
            }
        }
    }
    llvm::Value* result = emitMethodCall(ms, args, baseQualified, &argExprs);
    // Record the concrete subst that was active during this call so that
    // lowerLocalDecl can associate it with the result's local variable.
    if (!genericSubst_.empty()) {
        lastCallGenericSubst_ = genericSubst_;
    } else {
        // Even if genericSubst_ is empty (non-generic caller), try to
        // derive the subst from the method's return type so that
        // 'var primes := CreatePrimes(...)' correctly associates
        // the Gen<int> substitution rather than inheriting stale
        // lastCallGenericSubst_ from prior generic method calls.
        if (auto* md = dynamic_cast<const ast::MethodDecl*>(ms->decl)) {
            if (md->returnType) {
                sema::TypeRef* retTy = ctx_.resolvedType(*md->returnType);
                if (retTy && retTy->kind == sema::TypeRef::Kind::Named &&
                    retTy->declaration && !retTy->genericArgs.empty()) {
                    auto* td = dynamic_cast<const ast::TypeDecl*>(
                        retTy->declaration->decl);
                    if (td && td->genericParams.size() ==
                              retTy->genericArgs.size()) {
                        std::unordered_map<std::string_view, llvm::Type*> newSubst;
                        for (std::size_t gi = 0;
                             gi < td->genericParams.size(); ++gi) {
                            llvm::Type* concrTy =
                                lowerTypeRef(retTy->genericArgs[gi]);
                            newSubst[td->genericParams[gi].name] = concrTy;
                        }
                        if (!newSubst.empty())
                            lastCallGenericSubst_ = std::move(newSubst);
                    }
                }
            }
        }
    }
    genericSubst_ = std::move(savedSubst);
    genericSubstSemaTy_ = std::move(savedSemaTy);
    return result;
}


llvm::Value* Emitter::emitMethodCall(sema::MethodSymbol* ms,
                            std::vector<llvm::Value*>& args,
                            bool baseQualified,
                            const std::vector<const ast::Expr*>* argExprs) {
    // issue #421 / #410: is the argument at index `ai` an unsigned source?
    // Defaults to false (signed) when no source expression is known for the
    // slot (e.g. the receiver) or no argExprs vector was supplied.
    auto argUnsigned = [&](std::size_t ai) -> bool {
        if (!argExprs || ai >= argExprs->size() || !(*argExprs)[ai])
            return false;
        return isUnsignedTargetExpr(*(*argExprs)[ai]);
    };
    bool canDispatch = !baseQualified && !isStaticMethod(ms);
    unsigned dispatchSlot = 0;
    if (canDispatch) {
        auto ifIt = ifaceMethodSlot_.find(ms);
        if (ifIt != ifaceMethodSlot_.end()) {
            dispatchSlot = ifIt->second;
        } else {
            auto slotIt = vtableSlot_.find(ms);
            if (slotIt != vtableSlot_.end()) dispatchSlot = slotIt->second;
            else canDispatch = false;
        }
    }
    if (canDispatch) {
        // CG-generic: if there is an active type-parameter substitution
        // and this method has generic params/return, use inline emission
        // rather than vtable dispatch — the vtable function was compiled
        // with i32-placeholder types and calling it with concrete types
        // (e.g. ptr for string) will mis-type the call or crash.
        if (!genericSubst_.empty()) {
            llvm::FunctionType* fnTy = methodFnType(ms);
            if (fnTy) {
                bool needsInline = false;
                // Check return type for generic placeholder (i32 from lowerType fallback).
                auto* md = dynamic_cast<const ast::MethodDecl*>(ms->decl);
                if (md && md->returnType) {
                    sema::TypeRef* retTy = ctx_.resolvedType(*md->returnType);
                    if (retTy && lowerTypeRef(retTy) != fnTy->getReturnType())
                        needsInline = true;
                }
                // Check param types.
                if (!needsInline) {
                    for (std::size_t ai = 0; ai < args.size(); ++ai)
                        if (args[ai]->getType() != fnTy->getParamType(ai))
                            { needsInline = true; break; }
                }
                if (needsInline) {
                    llvm::Value* r = emitInlineGenericMethod(ms, args);
                    if (r != nullptr || diverged_) {
                        emitPendingCheck();
                        return r;
                    }
                }
            }
        }
        llvm::FunctionType* fnTy = methodFnType(ms);
        if (!fnTy || args.size() != fnTy->getNumParams()) return nullptr;
        // §5.1 / §22.6: widen each argument to its declared parameter type
        // when sema admitted an implicit numeric conversion (e.g. an int
        // argument bound to a double parameter). Otherwise the i32 reaches an
        // f64 parameter as raw bits (issue #408 / finding [17]). An unsigned
        // source widens via UIToFP/ZExt (issue #421 / #410).
        for (std::size_t ai = 0; ai < args.size(); ++ai)
            args[ai] = coerceNumericToType(args[ai], fnTy->getParamType(ai),
                                           argUnsigned(ai));
        llvm::Value* recv = args[0];
        llvm::Value* vptr = builder_.CreateLoad(ptrTy(), recv);
        llvm::Value* slotPtr = builder_.CreateGEP(
            ptrTy(), vptr, {builder_.getInt32(dispatchSlot)});
        llvm::Value* fnPtr = builder_.CreateLoad(ptrTy(), slotPtr);
        llvm::Value* r = builder_.CreateCall(fnTy, fnPtr, args);
        emitPendingCheck();  // CG20: a callee may have thrown
        return r;
    }

    // Non-virtual (or base-qualified) → direct call to the function.
    auto it = methodFns_.find(ms);
    if (it == methodFns_.end()) return nullptr;
    llvm::Function* callee = it->second;
    if (args.size() != callee->arg_size()) return nullptr;
    // CG-generic: if the generic substitution is active and the method
    // is on a class that has generic parameters, we must specialize the
    // call — even for methods with non-generic param/return types (like
    // `Size(): int`), because the struct layout changes when generic field
    // types (e.g. [5]T) are substituted with concrete types (e.g. [5]ptr).
    if (!genericSubst_.empty() && !isStaticMethod(ms)) {
        // Check if the method's parent class has generic params (i.e., is a
        // generic class that has been instantiated with the current subst).
        bool parentIsGeneric = false;
        if (ms->parent && ms->parent->decl) {
            auto* td = dynamic_cast<const ast::TypeDecl*>(ms->parent->decl);
            if (td && !td->genericParams.empty()) parentIsGeneric = true;
        }
        if (parentIsGeneric) {
            llvm::Value* r = emitInlineGenericMethod(ms, args);
            if (r != nullptr || diverged_) {
                emitPendingCheck();
                return r;
            }
        }
    }
    // CG-generic: if the generic substitution is active and the method
    // function has type-mismatched args (generic T→i32 vs concrete f64),
    // emit a specialized inline wrapper rather than calling the compiled
    // generic function. This handles generic methods like `Get(): T`.
    if (!genericSubst_.empty()) {
        bool typesMismatch = false;
        auto* callFnTy = callee->getFunctionType();
        for (std::size_t ai = 0; ai < args.size(); ++ai)
            if (args[ai]->getType() != callFnTy->getParamType(ai))
                { typesMismatch = true; break; }
        if (!typesMismatch && ms->decl) {
            // Also check return type — if the method returns a generic
            // param (i32 fallback) but we need a concrete type.
            llvm::Type* declRetTy = callFnTy->getReturnType();
            auto* md = dynamic_cast<const ast::MethodDecl*>(ms->decl);
            if (md && md->returnType) {
                sema::TypeRef* retTy = ctx_.resolvedType(*md->returnType);
                llvm::Type* concrRetTy = retTy
                    ? lowerTypeRef(retTy) : declRetTy;
                if (concrRetTy != declRetTy) typesMismatch = true;
            }
        }
        if (typesMismatch) {
            // Inline the method body with the current generic substitution.
            llvm::Value* r = emitInlineGenericMethod(ms, args);
            if (r != nullptr || diverged_) {
                emitPendingCheck();
                return r;
            }
        }
    }
    // §5.1 / §22.6: widen each argument to its declared parameter type when
    // sema admitted an implicit numeric conversion (e.g. an int argument bound
    // to a double parameter — also reached by the operator-overload path in
    // lowerBinary). Without this the i32 reaches an f64 parameter as raw bits
    // (issue #408 / finding [17]). Applied after the generic-specialization
    // checks above so it does not perturb their arg/param type comparisons.
    {
        auto* callFnTy = callee->getFunctionType();
        for (std::size_t ai = 0;
             ai < args.size() && ai < callFnTy->getNumParams(); ++ai)
            args[ai] = coerceNumericToType(args[ai], callFnTy->getParamType(ai),
                                           argUnsigned(ai));
    }
    llvm::Value* r = builder_.CreateCall(callee, args);
    emitPendingCheck();  // CG20: a callee may have thrown
    return r;
}


llvm::Value* Emitter::emitGenericSpecialization(sema::MethodSymbol* ms,
                                       std::vector<llvm::Value*>& args,
                                       const CallableInfo& ci) {
    // Build the substitution key for caching.
    std::vector<llvm::Type*> substKey;
    for (auto& [name, ty] : genericSubst_) substKey.push_back(ty);

    auto cacheKey = std::make_pair(ms, substKey);
    auto cit = specializedMethods_.find(cacheKey);
    if (cit != specializedMethods_.end()) {
        // Already compiled — just call it.
        llvm::Function* fn = cit->second;
        if (args.size() != fn->arg_size()) return nullptr;
        llvm::Value* r = builder_.CreateCall(fn, args);
        emitPendingCheck();
        return r;
    }

    // Build the function type with concrete arg/ret types.
    std::vector<llvm::Type*> paramTys;
    for (auto* a : args) paramTys.push_back(a->getType());
    // Return type: apply genericSubst_ to the declared return type.
    llvm::Type* retTy = llvm::Type::getVoidTy(llvm_);
    auto* md = dynamic_cast<const ast::MethodDecl*>(ms->decl);
    if (md && md->returnType) {
        sema::TypeRef* rt = ctx_.resolvedType(*md->returnType);
        if (rt) retTy = lowerTypeRef(rt);
    }
    auto* fnTy = llvm::FunctionType::get(retTy, paramTys, false);

    // Create the specialized function.
    std::string fname = std::string(ms->parent ? ms->parent->name : "")
                        + "$" + std::string(ms->name) + "$spec";
    llvm::Function* specFn = llvm::Function::Create(
        fnTy, llvm::Function::InternalLinkage, fname, module_.get());
    specializedMethods_[cacheKey] = specFn;

    // Emit the body into the specialized function.
    llvm::BasicBlock* savedInsert = builder_.GetInsertBlock();
    llvm::Value* savedThis = currentThis_;
    auto savedLocals = locals_;
    auto savedParams = paramSlots_;
    auto savedRef = refParams_;
    llvm::BasicBlock* savedProp = propagateBB_;
    llvm::BasicBlock* savedUnwind = unwindTarget_;
    llvm::Value* savedCaught = currentCaughtExc_;
    std::size_t savedUnwindDepth = unwindScopeDepth_;
    bool savedDiverged = diverged_;
    auto savedLoop = loopStack_;
    auto savedDispose = disposeStack_;
    llvm::Function* savedFn = currentFn_;

    currentFn_ = specFn;
    auto* entryBB = llvm::BasicBlock::Create(llvm_, "entry", specFn);
    builder_.SetInsertPoint(entryBB);
    locals_.clear();
    paramSlots_.clear();
    refParams_.clear();
    propagateBB_ = nullptr;
    unwindTarget_ = nullptr;
    currentCaughtExc_ = nullptr;
    unwindScopeDepth_ = 0;
    diverged_ = false;
    loopStack_.clear(); finallyStack_.clear();
    disposeStack_.clear();

    // Bind `this` and params.
    auto argIt = specFn->arg_begin();
    std::size_t paramStart = isStaticMethod(ms) ? 0 : 1;
    if (!isStaticMethod(ms)) {
        currentThis_ = &*argIt++;
    } else {
        currentThis_ = nullptr;
    }
    auto* md2 = dynamic_cast<const ast::MethodDecl*>(ms->decl);
    for (std::size_t pi = 0; pi < ms->params.size(); ++pi, ++argIt) {
        sema::Symbol* psym = ms->params[pi];
        auto* ps = dynamic_cast<sema::ParameterSymbol*>(psym);
        // CG50: ref/out params are passed as pointers; register in refParams_
        // so reads/writes go through the pointer (same as non-spec methods).
        if (ps && isByRefMod(ps->modifier)) {
            llvm::Type* elemTy = ps->declType ? lowerType(ps->declType) : i32();
            refParams_[psym] = {&*argIt, elemTy};
        } else {
            auto* slot = entryAlloca(argIt->getType());
            builder_.CreateStore(&*argIt, slot);
            paramSlots_[psym] = slot;
        }
    }

    // Emit block body.
    bool terminated = ci.stmts && lowerScopedBlock(*ci.stmts);
    if (!terminated) emitImplicitReturn(specFn);

    // Restore outer state.
    builder_.SetInsertPoint(savedInsert);
    currentFn_ = savedFn;
    currentThis_ = savedThis;
    locals_ = std::move(savedLocals);
    paramSlots_ = std::move(savedParams);
    refParams_ = std::move(savedRef);
    propagateBB_ = savedProp;
    unwindTarget_ = savedUnwind;
    currentCaughtExc_ = savedCaught;
    unwindScopeDepth_ = savedUnwindDepth;
    diverged_ = savedDiverged;
    loopStack_ = std::move(savedLoop);
    disposeStack_ = std::move(savedDispose);

    // Call the specialized function.
    if (args.size() != specFn->arg_size()) return nullptr;
    llvm::Value* r = builder_.CreateCall(specFn, args);
    emitPendingCheck();
    return r;
}


llvm::Value* Emitter::emitInlineGenericMethod(sema::MethodSymbol* ms,
                                      std::vector<llvm::Value*>& args) {
    CallableInfo ci;
    if (!callableInfo(ms, ci)) return nullptr;
    auto* md = dynamic_cast<const ast::MethodDecl*>(ms->decl);
    if (!md) return nullptr;

    // Save state.
    llvm::Value* savedThis = currentThis_;
    auto savedLocals = locals_;
    auto savedParams = paramSlots_;
    llvm::BasicBlock* savedProp = propagateBB_;
    llvm::BasicBlock* savedUnwind = unwindTarget_;
    llvm::Value* savedCaught = currentCaughtExc_;
    std::size_t savedUnwindDepth = unwindScopeDepth_;
    bool savedDiverged = diverged_;

    // Bind this and params.
    currentThis_ = (!args.empty() && !isStaticMethod(ms))
                    ? args[0] : nullptr;
    locals_.clear();
    paramSlots_.clear();
    propagateBB_ = nullptr;
    currentCaughtExc_ = nullptr;
    unwindScopeDepth_ = 0;
    diverged_ = false;
    // Bind user params (skip leading `this`).
    std::size_t paramStart = isStaticMethod(ms) ? 0 : 1;
    for (std::size_t pi = 0; pi < ms->params.size() &&
         pi + paramStart < args.size(); ++pi) {
        auto* slot = entryAlloca(args[pi + paramStart]->getType());
        builder_.CreateStore(args[pi + paramStart], slot);
        paramSlots_[ms->params[pi]] = slot;
    }

    // Emit the body.
    llvm::Value* result = nullptr;
    if (ci.bodyKind == ast::MethodBodyKind::Expression && ci.exprBody) {
        result = lowerExpr(*ci.exprBody);
    } else if (ci.bodyKind == ast::MethodBodyKind::Block) {
        // Block body: compile a specialized function and call it.
        // Inlining block bodies is problematic because `return` generates
        // `ret` in the outer function. Instead, monomorphize: compile a
        // concrete version of the function with the active substitution.
        result = emitGenericSpecialization(ms, args, ci);
    }

    // Restore state.
    currentThis_ = savedThis;
    locals_ = std::move(savedLocals);
    paramSlots_ = std::move(savedParams);
    propagateBB_ = savedProp;
    unwindTarget_ = savedUnwind;
    currentCaughtExc_ = savedCaught;
    unwindScopeDepth_ = savedUnwindDepth;
    diverged_ = savedDiverged;

    return result;
}


llvm::Value* Emitter::lowerNullCondCall(const ast::CallExpr& call,
                               const ast::NullCondMemberExpr& nm) {
    auto* ms = dynamic_cast<sema::MethodSymbol*>(ctx_.exprSymbol(*call.callee));
    if (!ms || isStaticMethod(ms) || !nm.target) return nullptr;
    llvm::FunctionType* fnTy = methodFnType(ms);
    if (!fnTy) return nullptr;
    llvm::Type* retTy = fnTy->getReturnType();
    bool refResult = retTy->isPointerTy();
    bool voidResult = retTy->isVoidTy();
    // §7.5 / FINDING [3] ADDENDUM: a value-typed return (`int`, `bool`, an enum,
    // a record) lifts to the always-nullable {i8 hasValue; T value} tagged
    // struct — null when the receiver is null, the wrapped value otherwise. A
    // method that ALREADY returns a nullable value (e.g. `int?`) returns the
    // tagged struct directly; its own hasValue carries through.
    const sema::TypeRef* callTy = ctx_.exprType(call);
    bool valueResult = !refResult && !voidResult;
    bool nullableReturn = valueResult && callTy && isNullableValueTy(callTy) &&
        retTy->isStructTy();
    // The §7.5 result type: a pointer for reference returns, the tagged struct
    // for value returns (StructType::get is uniqued, matching the consumer slot).
    llvm::Type* resTy = nullptr;
    if (valueResult)
        resTy = nullableReturn
            ? retTy
            : static_cast<llvm::Type*>(llvm::StructType::get(
                  llvm_, {nullablePrimHasValTy(), retTy}));
    llvm::Value* recv = lowerExpr(*nm.target);
    if (!recv || !recv->getType()->isPointerTy()) return nullptr;

    auto* entryBB = builder_.GetInsertBlock();
    auto* callBB =
        llvm::BasicBlock::Create(llvm_, "nullcall.do", currentFn_);
    auto* endBB =
        llvm::BasicBlock::Create(llvm_, "nullcall.end", currentFn_);
    llvm::Value* isNull = builder_.CreateICmpEQ(
        recv, llvm::ConstantPointerNull::get(ptrTy()));
    builder_.CreateCondBr(isNull, endBB, callBB);

    builder_.SetInsertPoint(callBB);
    std::vector<llvm::Value*> args;
    std::vector<const ast::Expr*> argExprs;
    args.push_back(recv);
    argExprs.push_back(nullptr);  // receiver has no widening signedness
    for (const auto& a : call.args) {
        if (!a.value) return nullptr;
        llvm::Value* v = lowerExpr(*a.value);
        if (!v) return nullptr;
        args.push_back(v);
        argExprs.push_back(a.value.get());
    }
    llvm::Value* r = emitMethodCall(ms, args, /*baseQualified=*/false, &argExprs);
    if (!r && (refResult || valueResult)) return nullptr;
    // Lift the call result to the §7.5 result form. A value-typed return wraps
    // into {i8 1, r}; an already-nullable return passes its own hasValue through.
    llvm::Value* callVal = r;
    if (valueResult && !nullableReturn) {
        callVal = builder_.CreateInsertValue(
            llvm::UndefValue::get(resTy),
            llvm::ConstantInt::get(nullablePrimHasValTy(), 1), {0u});
        callVal = builder_.CreateInsertValue(callVal, r, {1u});
    }
    auto* callEnd = builder_.GetInsertBlock();
    builder_.CreateBr(endBB);

    builder_.SetInsertPoint(endBB);
    if (voidResult) return nullptr;  // void: result discarded by caller
    if (valueResult) {
        // Null receiver → the all-zero (hasValue=false) tagged struct.
        auto* phi = builder_.CreatePHI(resTy, 2);
        phi->addIncoming(llvm::ConstantAggregateZero::get(resTy), entryBB);
        phi->addIncoming(callVal, callEnd);
        return phi;
    }
    auto* phi = builder_.CreatePHI(ptrTy(), 2);
    phi->addIncoming(llvm::ConstantPointerNull::get(ptrTy()), entryBB);
    phi->addIncoming(r, callEnd);
    return phi;
}


llvm::FunctionType* Emitter::methodFnType(const sema::MethodSymbol* ms) {
    auto* md = dynamic_cast<const ast::MethodDecl*>(ms->decl);
    if (!md) return nullptr;
    llvm::Type* retTy = lowerType(md->returnType.get());
    std::vector<llvm::Type*> paramTys;
    if (!isStaticMethod(const_cast<sema::MethodSymbol*>(ms)))
        paramTys.push_back(ptrTy());
    for (const auto& p : md->params)
        paramTys.push_back(isByRefMod(p.modifier)
                               ? ptrTy()  // CG50: ref/out by reference
                               : lowerType(p.type.get()));
    return llvm::FunctionType::get(retTy, paramTys, /*isVarArg=*/false);
}


bool Emitter::isStringExprType(const ast::Expr& e) {
    auto* rt = ctx_.exprType(e);
    return rt && rt->kind == sema::TypeRef::Kind::Primitive &&
           rt->primitive == sema::PrimitiveType::String;
}


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
static int64_t lazyElemSize(llvm::Type* ty) {
    if (!ty) return 8;
    if (ty->isIntegerTy(32) || ty->isFloatTy()) return 4;
    if (ty->isIntegerTy(1) || ty->isIntegerTy(8)) return 1;
    if (ty->isIntegerTy(16)) return 2;
    return 8;  // i64 / double / pointer (string, class)
}

bool Emitter::isLazyEnumerableType(const sema::TypeRef* rt) {
    // The lazy LINQ sentinel: a Named TypeRef with no declaration and no
    // astNode, carrying the element type as genericArgs[0]. Produced only by
    // the LINQ-operator typing in ExprTypecheck_Calls.cpp.
    return rt && rt->kind == sema::TypeRef::Kind::Named && !rt->declaration &&
           !rt->astNode && !rt->genericArgs.empty();
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

// issues D3 / D8 / §4.8 / §4.10: decide whether a Dictionary key / Set element
// at generic-arg index `idx` needs the generic-key runtime backing, and if so
// return its resolved TypeRef and the per-key byte count.
//
//   Dictionary (isSet=false): a string key keeps the bz_dict_*_str fast path
//     (returns null). Every other equality-bearing key — primitives, enums,
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
                      (otherTy->astNode->name->parts[0] == "Set" ||
                       otherTy->astNode->name->parts[0] == "OwnedSet") &&
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
    using K = sema::TypeRef::Kind;
    bool isString = ty->kind == K::Primitive &&
                    ty->primitive == sema::PrimitiveType::String && !ty->nullable;
    // Dictionary: string keys keep their dedicated fast path (bz_dict_*_str).
    if (!isSet && isString) return nullptr;
    // Eligible generic-backed types: a record (existing path) or a non-nullable
    // primitive/enum scalar (D3) — plus string for sets (D8). Nullable keys and
    // other shapes are not wired here.
    bool eligible = false;
    if (recordTypeOfRef(ty)) eligible = true;
    else if (isString) eligible = true;  // set element only (dict returned above)
    else if (!ty->nullable && ty->kind == K::Primitive &&
             ty->primitive != sema::PrimitiveType::String &&
             ty->primitive != sema::PrimitiveType::Object &&
             ty->primitive != sema::PrimitiveType::Decimal)
        eligible = true;  // int/long/bool/char/byte/short/uint/.../float/double
    else if (!ty->nullable && ty->kind == K::Named && ty->declaration &&
             ty->declaration->kind == ast::TypeDeclKind::Enum)
        eligible = true;  // enum key/element (lowers to i32; value-compared)
    if (!eligible) return nullptr;
    if (byteSize) {
        llvm::Type* lt = lowerTypeRef(ty);
        *byteSize = lt ? module_->getDataLayout().getTypeAllocSize(lt)
                       : (uint64_t)8;
    }
    return ty;
}

// issues D3 / D8 / §8.3: generic-dict hash trampoline `uint64_t(const void*)`
// over an arbitrary equality-bearing value type. The runtime stores the key's
// natural bytes; the trampoline reinterprets the byte pointer as a pointer to
// the key's LLVM type, loads the value, and runs the same hash the language
// uses for the type — emitValueHashCode (§8.3): a string folds its code points
// (#346 content hash), a record folds its field hashes, a scalar derives an i32
// from its bits. The i32 is zero-extended (the runtime keeps the low 32 bits).
llvm::Function* Emitter::buildValueHashTrampoline(const sema::TypeRef* ty) {
    // A record key reuses the existing record trampoline (tested path).
    if (const sema::TypeSymbol* rec = recordTypeOfRef(ty))
        return buildRecordHashTrampoline(rec);
    llvm::Type* lt = lowerTypeRef(ty);
    auto* fty = llvm::FunctionType::get(i64(), {ptrTy()}, false);
    auto* fn = llvm::Function::Create(fty, llvm::Function::InternalLinkage,
                                      "bz$valkey$hash", module_.get());
    auto savedIP = builder_.saveIP();
    llvm::Function* savedFn = currentFn_;
    currentFn_ = fn;
    auto* entry = llvm::BasicBlock::Create(llvm_, "entry", fn);
    builder_.SetInsertPoint(entry);
    llvm::Value* v = builder_.CreateLoad(lt, fn->getArg(0), "key.val");
    // emitValueHashCode hashes a string by content when fieldTy is the string
    // primitive; a scalar by its bits.
    llvm::Value* h = emitValueHashCode(v, ty);             // i32
    builder_.CreateRet(builder_.CreateZExt(h, i64()));
    builder_.restoreIP(savedIP);
    currentFn_ = savedFn;
    return fn;
}

// issues D3 / D8 / §8.3: generic-dict equality trampoline
// `bool(const void* a, const void* b)` over an arbitrary equality-bearing value
// type. Loads both keys from their byte pointers and compares them by the
// language's equality for the type: string content (emitStringEq, §4.6),
// structural record equality (emitStructEquality, §9.1/§9.2 — consistent with
// the hash), or an integer/float compare for primitives/enums.
llvm::Function* Emitter::buildValueEqTrampoline(const sema::TypeRef* ty) {
    if (const sema::TypeSymbol* rec = recordTypeOfRef(ty))
        return buildRecordEqTrampoline(rec);
    llvm::Type* lt = lowerTypeRef(ty);
    auto* fty = llvm::FunctionType::get(i1(), {ptrTy(), ptrTy()}, false);
    auto* fn = llvm::Function::Create(fty, llvm::Function::InternalLinkage,
                                      "bz$valkey$eq", module_.get());
    auto savedIP = builder_.saveIP();
    llvm::Function* savedFn = currentFn_;
    currentFn_ = fn;
    auto* entry = llvm::BasicBlock::Create(llvm_, "entry", fn);
    builder_.SetInsertPoint(entry);
    llvm::Value* a = builder_.CreateLoad(lt, fn->getArg(0), "eq.a");
    llvm::Value* b = builder_.CreateLoad(lt, fn->getArg(1), "eq.b");
    using K = sema::TypeRef::Kind;
    bool isString = ty->kind == K::Primitive &&
                    ty->primitive == sema::PrimitiveType::String;
    llvm::Value* eq;
    if (isString)
        eq = emitStringEq(a, b);                           // §4.6 content compare
    else if (lt->isFloatingPointTy())
        eq = builder_.CreateFCmpOEQ(a, b);                 // float/double compare
    else
        eq = builder_.CreateICmpEQ(a, b);                  // int/bool/char/enum
    builder_.CreateRet(eq);
    builder_.restoreIP(savedIP);
    currentFn_ = savedFn;
    return fn;
}

llvm::Function* Emitter::buildLinqTrampoline(int kind, llvm::Type* elemTy,
                                             llvm::Type* outTy,
                                             const sema::TypeSymbol* elemRec) {
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
            eq = emitStringEq(a, b);           // string content equality (§4.6)
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
        // Key: closure returns the int key; widen to i64.
        auto* clTy = llvm::FunctionType::get(i32(), {ptrTy(), elemTy}, false);
        llvm::Value* k = builder_.CreateCall(clTy, fnPtr, {envPtr, elem}, "cl.ret");
        builder_.CreateRet(builder_.CreateSExt(k, i64()));
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
                                                   int64_t innerEsz) {
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
    auto* clTy = llvm::FunctionType::get(ptrTy(), {ptrTy(), elemTy}, false);
    llvm::Value* coll = builder_.CreateCall(clTy, fnPtr, {envPtr, elem}, "fs.coll");
    // Wrap the returned Array<U> as a lazy enumerator (the runtime drains it).
    auto* wrapFn = declareC("bz_lazy_from_array", ptrTy(), {ptrTy(), i64()});
    llvm::Value* inner = builder_.CreateCall(wrapFn,
        {coll, builder_.getInt64(innerEsz)}, "fs.inner");
    builder_.CreateRet(inner);
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

llvm::Value* Emitter::lowerToLazyEnumerator(const ast::Expr& recvExpr,
                                            sema::TypeRef*& elemRef) {
    auto* recvTy = ctx_.exprType(recvExpr);
    llvm::Value* recv = lowerExpr(recvExpr);
    if (!recv) return nullptr;
    if (isLazyEnumerableType(recvTy)) {
        // Already a lazy enumerator (chained LINQ): use it directly.
        elemRef = recvTy->genericArgs[0];
        return recv;
    }
    // issue #389 / stdlib §5 / §4.10: a dict-backed Set<T> is IEnumerable<T>
    // (Set: ICollection<T>: IEnumerable<T>) but its backing pointer is a
    // bz_dict_t*, not a bz_array_t* — wrap it via the Set lazy source, which
    // snapshots the element keys. Only the generic dict-backed Sets (record /
    // primitive / enum / string elements) need this; a plain class-element
    // OwnedSet is array-backed and takes the ARRAY path below.
    elemRef = (recvTy && !recvTy->genericArgs.empty()) ? recvTy->genericArgs[0]
                                                       : nullptr;
    if (recvTy && recvTy->astNode && recvTy->astNode->name &&
        !recvTy->astNode->name->parts.empty() &&
        (recvTy->astNode->name->parts[0] == "Set" ||
         recvTy->astNode->name->parts[0] == "OwnedSet")) {
        uint64_t ksz = 0;
        if (genericBackingType(recvTy, 0, /*isSet=*/true, &ksz)) {
            auto* setFn = declareC("bz_lazy_from_set", ptrTy(), {ptrTy(), i64()});
            return builder_.CreateCall(setFn,
                {recv, builder_.getInt64((int64_t)ksz)}, "lazy.set");
        }
    }
    // Concrete collection (Array<T> family): wrap it as a lazy ARRAY source.
    int64_t esz = collectionElemSize(recvTy);
    auto* wrapFn = declareC("bz_lazy_from_array", ptrTy(), {ptrTy(), i64()});
    return builder_.CreateCall(wrapFn, {recv, builder_.getInt64(esz)}, "lazy.src");
}

llvm::Value* Emitter::tryLowerLazyLinqCall(const ast::CallExpr& call) {
    if (!call.callee) return nullptr;
    auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(call.callee.get());
    if (!ma || !ma->target) return nullptr;
    const auto& m = ma->member;

    // Only intercept on a collection or lazy-enumerable receiver.
    auto* recvTy = ctx_.exprType(*ma->target);
    bool collRecv = isCollectionTypeName(recvTy);
    bool lazyRecv = isLazyEnumerableType(recvTy);
    if (!collRecv && !lazyRecv) return nullptr;

    // Element LLVM type for the receiver's element TypeRef.
    auto elemLlvmOf = [&](sema::TypeRef* er) -> llvm::Type* {
        if (er) if (auto* t = lowerTypeRef(er)) return t;
        return i32();
    };

    // ----- ToArray: materialize the (lazy) pipeline into an owned Array<T> ---
    if (m == "ToArray" && call.args.empty()) {
        sema::TypeRef* elemRef = nullptr;
        llvm::Value* lazy = lowerToLazyEnumerator(*ma->target, elemRef);
        if (!lazy) return nullptr;
        int64_t esz = lazyElemSize(elemLlvmOf(elemRef));
        auto* toArr = declareC("bz_lazy_to_array", ptrTy(), {ptrTy(), i64()});
        return builder_.CreateCall(toArr, {lazy, builder_.getInt64(esz)}, "linq.toarr");
    }

    // The deferred operators. Lower the source to a lazy enumerator first.
    bool isOp = (m == "Where" || m == "Select" || m == "Take" || m == "Skip" ||
                 m == "Distinct" || m == "OrderBy" || m == "OrderByDescending" ||
                 m == "Concat" || m == "TakeWhile" || m == "SkipWhile" ||
                 m == "SelectMany" || m == "Zip");
    if (!isOp) return nullptr;

    sema::TypeRef* elemRef = nullptr;
    llvm::Value* src = lowerToLazyEnumerator(*ma->target, elemRef);
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
        return i < call.args.size() && call.args[i].value &&
               dynamic_cast<const ast::LambdaExpr*>(call.args[i].value.get())
                   != nullptr;
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
        int64_t outEsz = lazyElemSize(outTy);
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
        // equality-bearing record element type.
        const sema::TypeSymbol* elemRec = recordTypeOfRef(elemRef);
        llvm::Function* eqTramp = buildLinqTrampoline(3, elemTy, nullptr, elemRec);
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
        int64_t innerEsz = lazyElemSize(innerElemTy);
        llvm::Function* tramp = buildSelectManyTrampoline(elemTy, innerEsz);
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
        int64_t outEsz = lazyElemSize(outTy);
        auto* zipFn = declareC("bz_lazy_zip", ptrTy(),
                               {ptrTy(), ptrTy(), i64(), ptrTy(), ptrTy()});
        llvm::Value* e = builder_.CreateCall(zipFn,
            {src, down, builder_.getInt64(outEsz), closure, tramp}, "linq.zip");
        return ownClosureIf(e, argIsInlineLambda(1));  // #422: combine is args[1]
    }
    return nullptr;
}


}  // namespace bezel::codegen
