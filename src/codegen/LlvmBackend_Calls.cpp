#include "codegen/LlvmBackend_impl.hpp"

// §4.4 fix #521: decimal compile-time literal parser (defined in bezel_runtime.cpp).
extern "C" void bz_decimal_parse_cstr(const char* s, void* out);

namespace bezel::codegen {

llvm::Value* Emitter::emitConsoleWrite(llvm::Value* strPtr, bool newline,
                               int fd) {
    // fix #592 / per §4.6: Bezel strings use a per-string storage width of 1,
    // 2, or 4 bytes per code point (Python PEP 393 model). The raw data
    // pointer is NOT UTF-8 for non-ASCII content: a width-1 string stores
    // code points in uint8_t (Latin-1 range 0x80..0xFF are NOT valid UTF-8
    // single bytes), and width-2/4 strings store code points in uint16_t /
    // uint32_t with interior zero bytes. Calling write() with the raw pointer
    // and the code-point count as the byte length produced garbled output
    // (e.g. "café" → "caf\xE9", "世界" → 2 raw uint16 bytes).
    //
    // Fix: re-encode to a NUL-terminated UTF-8 buffer via bz_string_to_utf8z
    // (runtime helper already used by the FFI CString marshalling path in
    // this file), determine the byte length with strlen, write the UTF-8
    // bytes, then free the buffer.  bz_string_to_utf8z always malloc-allocates
    // a fresh buffer (even for empty strings), so the free is unconditional.

    // Declare POSIX write(2): ssize_t write(int, const void*, size_t)
    auto* writeFn = module_->getFunction("write");
    if (!writeFn) {
        auto* ft = llvm::FunctionType::get(i64(), {i32(), ptrTy(), i64()},
                                           false);
        writeFn = llvm::Function::Create(
            ft, llvm::Function::ExternalLinkage, "write", module_.get());
    }
    // Re-encode to UTF-8: char* utf8buf = bz_string_to_utf8z(strPtr);
    llvm::Function* toUtf8 = declareC("bz_string_to_utf8z", ptrTy(), {ptrTy()});
    llvm::Value* utf8buf = builder_.CreateCall(toUtf8, {strPtr}, "utf8buf");
    // size_t byteLen = strlen(utf8buf);
    llvm::Function* strlenFn = declareC("strlen", i64(), {ptrTy()});
    llvm::Value* byteLen = builder_.CreateCall(strlenFn, {utf8buf}, "utf8.len");
    builder_.CreateCall(writeFn, {builder_.getInt32(fd), utf8buf, byteLen});
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
    // free(utf8buf) — bz_string_to_utf8z always malloc-allocates.
    builder_.CreateCall(freeFn(), {utf8buf});
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
    // fix #581 / fix #583 / §7 / §18.5: nullable VALUE types (int?, bool?,
    // double?, enum?, nullable record) lower to a tagged {i8 hasValue; T}
    // struct.  The existing Primitive dispatch below calls
    // emitPrimitiveToString(arg, primitive) with the RAW struct value, which
    // (a) passes a struct where an integer is expected → IR-crash for bool?
    //     ("Trunc only operates on integer: trunc {i8,i1} to i1") because
    //     emitPrimitiveToString's bool branch does CreateTrunc(val, i1()), and
    // (b) passes the {i8,i32} struct to snprintf's %d for int?, printing the
    //     first byte of the struct (the hasValue tag, value 1) instead of 42.
    // Intercept BEFORE the Primitive check: branch on the hasValue tag and
    // format the inner value via emitValueToString (which already handles all
    // primitive/enum/record cases), or emit the literal "null".  This mirrors
    // the interpolation path in LlvmBackend_Exprs.cpp (~line 127) and the
    // emitValueToString path in LlvmBackend_String.cpp (~line 1904).
    if (argTy && isNullableValueTy(argTy) && arg->getType()->isStructTy()) {
        llvm::Value* hasVal = nullablePrimHasValue(arg);           // i1
        llvm::Value* innerVal = nullablePrimValue(arg);             // T
        auto* valBB  = llvm::BasicBlock::Create(llvm_, "wln.nv.val",  currentFn_);
        auto* nullBB = llvm::BasicBlock::Create(llvm_, "wln.nv.null", currentFn_);
        auto* endBB  = llvm::BasicBlock::Create(llvm_, "wln.nv.end",  currentFn_);
        builder_.CreateCondBr(hasVal, valBB, nullBB);
        // hasValue: format the inner value using its non-nullable TypeRef.
        builder_.SetInsertPoint(valBB);
        sema::TypeRef innerTy = *argTy;
        innerTy.nullable = false;
        llvm::Value* valStr = emitValueToString(innerVal, &innerTy);
        auto* valEnd = builder_.GetInsertBlock();
        builder_.CreateBr(endBB);
        // null: the literal "null" (matching §18.5 interpolation null rendering).
        builder_.SetInsertPoint(nullBB);
        llvm::Value* nullStr = lowerStringLiteral("\"null\"", false);
        auto* nullEnd = builder_.GetInsertBlock();
        builder_.CreateBr(endBB);
        builder_.SetInsertPoint(endBB);
        auto* phi = builder_.CreatePHI(ptrTy(), 2, "wln.nv.str");
        phi->addIncoming(valStr, valEnd);
        phi->addIncoming(nullStr, nullEnd);
        strPtr = phi;
        // Note: freeTransient stays false here.  The valStr may be a
        // transient heap string (for numeric inner types) or a constant
        // global (for bool/string).  Freeing a PHI result conditionally after
        // the write requires a second hasValue check; the minor leak matches
        // what the interpolation path accepts.  Tracking this is a follow-up
        // to issue #421 for nullable arguments.
        emitConsoleWrite(strPtr, isWriteLine, stdfd);
        return true;
    }
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
    // §9: a record element type lowers to an LLVM StructType whose allocation
    // size may exceed 8 bytes (e.g. {ptr, i32} for a ref+value record = 16
    // bytes with padding).  Returning the hardcoded 8 here was the root cause
    // of bug D: bz_array_add/bz_array_get used an 8-byte stride for a 16-byte
    // element, so only the first 8 bytes were stored/loaded and the second
    // field (Age) was read from the wrong offset.  Use the DataLayout to get
    // the exact allocation size for any non-scalar element type.
    if (et->isStructTy())
        return (int64_t)module_->getDataLayout().getTypeAllocSize(et);
    return 8;  // ptr, i64, double
}


// §6.1 / §30.8: extract the builtin type name from a Named TypeRef with no
// user TypeDecl.  Two shapes are possible:
//   a) !rt->declaration: the name lives in rt->astNode->name->parts[0].
//   b) rt->declaration is a builtinClassSym (null decl): name = declaration->name.
// Returns an empty string_view if the TypeRef is not in either shape.
std::string_view Emitter::builtinTypeName(const sema::TypeRef* rt) {
    if (!rt || rt->kind != sema::TypeRef::Kind::Named) return {};
    if (!rt->declaration) {
        if (rt->astNode && rt->astNode->name &&
            !rt->astNode->name->parts.empty())
            return rt->astNode->name->parts[0];
        return {};
    }
    // builtinClassSym — non-null declaration, null decl (no user TypeDecl).
    if (!rt->declaration->decl) return rt->declaration->name;
    return {};
}


bool Emitter::isCollectionTypeName(const sema::TypeRef* rt) {
    if (!rt || rt->kind != sema::TypeRef::Kind::Named || rt->declaration)
        return false;
    if (!rt->astNode || !rt->astNode->name || rt->astNode->name->parts.empty())
        return false;
    auto n = rt->astNode->name->parts[0];
    // per §4.6/§4.7: LinkedList/OwnedLinkedList are NOT array-backed — they
    // use bz_llist_t. Exclude them here so they route through their own
    // tryLowerLinkedListInstanceCall handler rather than the bz_array_* paths.
    return n == "Array" || n == "OwnedArray" ||
           n == "Stack" || n == "OwnedStack" ||
           n == "Queue" || n == "OwnedQueue" ||
           n == "Set" || n == "OwnedSet";
}

// per §4.6 / §4.7: true for LinkedList<T> and OwnedLinkedList<T> (bz_llist_t-backed).
bool Emitter::isLinkedListTypeName(const sema::TypeRef* rt) {
    if (!rt || rt->kind != sema::TypeRef::Kind::Named || rt->declaration)
        return false;
    if (!rt->astNode || !rt->astNode->name || rt->astNode->name->parts.empty())
        return false;
    auto n = rt->astNode->name->parts[0];
    return n == "LinkedList" || n == "OwnedLinkedList";
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
// per §14.1 (generics are monomorphized): see the header comment. Build and
// install `genericSubst_` from the receiver `target`'s concrete instantiation
// of `parent`, returning the previous map for the caller to restore.
std::unordered_map<std::string_view, llvm::Type*>
Emitter::setupReceiverGenericSubst(const ast::Expr* target,
                                   const sema::TypeSymbol* parent) {
    auto savedSubst = genericSubst_;
    if (!target || !parent || !parent->decl) return savedSubst;
    auto* td = dynamic_cast<const ast::TypeDecl*>(parent->decl);
    if (!td || td->genericParams.empty()) return savedSubst;
    sema::TypeRef* recvTy = ctx_.exprType(*target);
    if (!recvTy || recvTy->kind != sema::TypeRef::Kind::Named ||
        recvTy->declaration != parent || recvTy->genericArgs.empty())
        return savedSubst;
    if (td->genericParams.size() != recvTy->genericArgs.size())
        return savedSubst;
    std::unordered_map<std::string_view, llvm::Type*> newSubst;
    for (std::size_t gi = 0; gi < td->genericParams.size(); ++gi)
        newSubst[td->genericParams[gi].name] =
            lowerTypeRef(recvTy->genericArgs[gi]);
    // A still-unresolved positional arg (i32 fallback) — recover the local's
    // stored concrete substitution, the same fallback fieldAddr / the property
    // getter path use for a generic receiver bound through a `var` local.
    bool hasUnresolved = false;
    for (auto& [n, ty] : newSubst)
        if (ty == i32()) { hasUnresolved = true; break; }
    if (hasUnresolved) {
        sema::Symbol* recvSym = ctx_.exprSymbol(*target);
        const ast::Node* decl = nullptr;
        if (auto* loc = dynamic_cast<sema::LocalSymbol*>(recvSym))
            decl = loc->decl;
        if (decl) {
            auto lit = localGenericSubst_.find(decl);
            if (lit != localGenericSubst_.end()) newSubst = lit->second;
        }
    }
    genericSubst_ = std::move(newSubst);
    return savedSubst;
}

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
        // issue #462 / §14.1: a GENERIC record element (`Array<Box<string>>`)
        // must use the specialized struct (load size + field offsets) and resolve
        // each `T`-typed field to its concrete type for per-field equality. A
        // no-op for a non-generic record element.
        RecordSubstGuard elemSubst(this, elemTy);
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
    // per §4.6/§4.7 + §5 fix #450: a concrete LinkedList<T> is IEnumerable<T>
    // but NOT isCollectionTypeName (bz_array_t*-backed). LINQ aggregate/terminal
    // operators must enumerate it via bz_lazy_from_llist + bz_lazy_to_array, the
    // same materialize-and-run path that lazy receivers use.
    bool isLinkedList = isLinkedListTypeName(recvTy);
    bool isArrTerminal =
        m == "Count" || m == "Sum" || m == "Aggregate" || m == "Average" ||
        m == "Min" || m == "Max" || m == "First" || m == "Single" ||
        m == "Last" || m == "ElementAt" || m == "Any" || m == "All" ||
        m == "ForEach" || m == "Contains" || m == "IndexOf" ||
        // stdlib §5 *OrNull variants (lazy source → materialize, then check):
        m == "FirstOrNull" || m == "LastOrNull" || m == "SingleOrNull";
    bool lazyTerminal = (lazyRecv || isLinkedList) && isArrTerminal;

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

        // §28.2 v0.135 / issue #627: checked-disposal mode guard for array-backed
        // collection methods. bz_array_is_disposed reads the disposed byte on the
        // bz_array_t header (set by bz_array_mark_disposed at Dispose time in
        // checked builds). Skip for lazy-materialized temporaries (those are
        // freshly-allocated by bz_lazy_to_array — they cannot be disposed).
        if (checkedDisposal_ && !lazyTempArr && isCollectionTypeName(recvTy)) {
            llvm::Type* i8Ty = llvm::Type::getInt8Ty(llvm_);
            auto* isFn = declareC("bz_array_is_disposed", i8Ty, {ptrTy()});
            llvm::Value* flag = builder_.CreateCall(isFn, {recv}, "cd.arrdis");
            llvm::Value* disposed = builder_.CreateICmpNE(
                flag, llvm::ConstantInt::get(i8Ty, 0), "cd.disposed");
            emitGuardThrow(disposed, "DisposedAccessException");
        }

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
                // #602 / #429 / §28.9 / §4.10 / §28.11 v0.134: consult the
                // element-slot policy to decide whether this Set<T> co-owns a +1
                // per element (Set is the dict-backed BORROWING collection; its
                // element type must be in the RuntimeManaged row AND be specifically
                // a non-nullable string primitive).
                // #602 batch 3: isStringElementSet was deleted; all Set<T> sites
                // now use elementPolicyFor directly (batch 4: stale reference removed).
                const ElementPolicy setElemPol =
                    elementPolicyFor(elemTy, /*owningFamily=*/false);
                // isStrSet: RuntimeManaged AND specifically a `string` primitive
                // (not a function type, which shares the RuntimeManaged row but uses
                // a different release path — per §28.11 v0.134).
                bool isStrSet =
                    setElemPol.cat == ElementPolicy::Category::RuntimeManaged &&
                    elemTy->kind == sema::TypeRef::Kind::Primitive &&
                    elemTy->primitive == sema::PrimitiveType::String &&
                    !elemTy->nullable;
                // .Add(item) — insert (idempotent by content; §4.10 dedup).
                if (m == "Add" && call.args.size() == 1 && call.args[0].value) {
                    llvm::Value* item = lowerExpr(*call.args[0].value);
                    if (!item) return nullptr;
                    // #429 / §28.9: retain the string element before inserting —
                    // the set takes an INDEPENDENT +1. A +1 producer is transferred
                    // (its single reference becomes the set's — no retain); a borrow
                    // / literal / identifier is retained. retain is null/sentinel-safe.
                    if (isStrSet && !isPlusOneStringProducer(call.args[0].value.get()))
                        emitStringRetain(item);
                    auto* fn = declareC("bz_dict_set_gen",
                        llvm::Type::getVoidTy(llvm_), {ptrTy(), ptrTy(), ptrTy()});
                    builder_.CreateCall(fn, {recv, elemPtr(item), sentinel});
                    return llvm::ConstantInt::get(i32(), 0);
                }
                // fix #522 / §4.10: .TryAdd(item): bool — inserts iff not
                // already present; returns true when newly added. Equivalent to
                // check-then-set; the result type is bool (i1) so it is safe
                // as a branch condition (unlike Add which returns unit/i32 0).
                if (m == "TryAdd" && call.args.size() == 1 && call.args[0].value) {
                    llvm::Value* item = lowerExpr(*call.args[0].value);
                    if (!item) return nullptr;
                    llvm::Value* ep = elemPtr(item);
                    auto* containsFn = declareC("bz_dict_contains_gen", i1(),
                                                {ptrTy(), ptrTy()});
                    llvm::Value* already = builder_.CreateCall(
                        containsFn, {recv, ep}, "set.has");
                    // If NOT already present, insert with sentinel value.
                    auto* insertBB = llvm::BasicBlock::Create(
                        llvm_, "ta.ins", currentFn_);
                    auto* afterBB  = llvm::BasicBlock::Create(
                        llvm_, "ta.after", currentFn_);
                    llvm::Value* notPresent = builder_.CreateNot(already, "ta.new");
                    builder_.CreateCondBr(notPresent, insertBB, afterBB);
                    builder_.SetInsertPoint(insertBB);
                    // #429 / §28.9: retain on insert (same rule as Add — the set
                    // co-owns the +1 only if TryAdd actually inserts, so emit the
                    // retain inside the insertBB arm). A +1 producer is transferred.
                    if (isStrSet && !isPlusOneStringProducer(call.args[0].value.get()))
                        emitStringRetain(item);
                    auto* setFn = declareC("bz_dict_set_gen",
                        llvm::Type::getVoidTy(llvm_), {ptrTy(), ptrTy(), ptrTy()});
                    builder_.CreateCall(setFn, {recv, ep, sentinel});
                    builder_.CreateBr(afterBB);
                    builder_.SetInsertPoint(afterBB);
                    // Return true iff newly inserted (i.e. was not present).
                    return notPresent;
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
                // #429 / §28.9 / §4.10: for a Set<string>, use the releasing
                // variant (bz_dict_remove_set_string) which releases the string
                // element's +1 before freeing its key copy — else the removed
                // string leaks. The query `item` is a pure borrow; the release
                // target is the KEY copy stored in the table, not the query itself.
                if (m == "Remove" && call.args.size() == 1 && call.args[0].value) {
                    llvm::Value* item = lowerExpr(*call.args[0].value);
                    if (!item) return nullptr;
                    if (isStrSet) {
                        auto* fn = declareC("bz_dict_remove_set_string", i1(),
                                            {ptrTy(), ptrTy()});
                        return builder_.CreateCall(fn, {recv, elemPtr(item)}, "set.rm");
                    }
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
                // table stays allocated for reuse; Count is 0 afterward.
                // #429 / §28.9: for a Set<string>, use bz_dict_clear_set_strings
                // which releases each string element's +1 before freeing its key
                // copy — else the cleared elements would leak. For any other
                // element type the value slots are sentinels: pass null disposer.
                if (m == "Clear" && call.args.empty()) {
                    if (isStrSet) {
                        auto* fn = declareC("bz_dict_clear_set_strings",
                            llvm::Type::getVoidTy(llvm_), {ptrTy()});
                        builder_.CreateCall(fn, {recv});
                    } else {
                        auto* fn = declareC("bz_dict_clear",
                            llvm::Type::getVoidTy(llvm_), {ptrTy(), ptrTy()});
                        builder_.CreateCall(fn,
                            {recv, llvm::ConstantPointerNull::get(ptrTy())});
                    }
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
            // per stdlib §5.1 (lines 2528–2529): Min()/Max() throw
            // InvalidOperationException if the source is empty.
            llvm::Value* isEmpty = builder_.CreateICmpEQ(len, builder_.getInt64(0));
            emitGuardThrow(isEmpty, "InvalidOperationException");
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
        // stdlib §5 line 2446 + 2515: throws InvalidOperationException if no
        // element satisfies the predicate.
        if (m == "First" && call.args.size() == 1 && call.args[0].value) {
            const bool relCl = argIsInlineLambda(0);
            llvm::Value* closure = lowerLinqLambda(call.args[0].value.get(),
                ctx_.primitiveTypeRef(sema::PrimitiveType::Bool));
            if (!closure) return nullptr;
            llvm::Type* elemTy = getElemTy();
            auto* lenFn = declareC("bz_array_len", i64(), {ptrTy()});
            llvm::Value* len = builder_.CreateCall(lenFn, {recv});
            auto* resultSlot = entryAlloca(elemTy);
            // foundSlot: i1 tracking whether the predicate matched any element.
            auto* foundSlot = entryAlloca(i1());
            builder_.CreateStore(builder_.getFalse(), foundSlot);
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
            builder_.CreateStore(builder_.getTrue(), foundSlot);
            builder_.CreateBr(endBB);
            builder_.SetInsertPoint(nextBB);
            builder_.CreateStore(builder_.CreateAdd(idx, builder_.getInt64(1)), idxSlot);
            builder_.CreateBr(condBB);
            builder_.SetInsertPoint(endBB);
            if (relCl) emitClosureRelease(closure);  // #422: inline-lambda transient
            // per stdlib §5 line 2515: throw InvalidOperationException if no match.
            llvm::Value* notFound = builder_.CreateNot(
                builder_.CreateLoad(i1(), foundSlot));
            emitGuardThrow(notFound, "InvalidOperationException");
            llvm::Value* fstv = builder_.CreateLoad(elemTy, resultSlot, "fst.r");
            freeLazyTemp();  // FINDING [5]: release materialized lazy snapshot
            return fstv;
        }
        // per §5 LINQ element access (no-predicate forms): First()/Single()
        // return the first element; Last() returns the last; ElementAt(i)
        // returns the element at index i (stdlib §5 — each `: T`).
        //
        // stdlib §5 line 2445: First() throws InvalidOperationException if empty.
        // stdlib §5 line 2449: Last()  throws InvalidOperationException if empty.
        // stdlib §5 line 2451: Single() throws InvalidOperationException if not
        //   exactly one element (empty OR ≥2).
        // stdlib §5 line 2515: "throw InvalidOperationException … on missing-element;
        //   IndexOutOfRangeException for ElementAt."
        if ((m == "First" || m == "Single") && call.args.empty()) {
            llvm::Type* retTy = getElemTy();
            auto* lenFn = declareC("bz_array_len", i64(), {ptrTy()});
            llvm::Value* len = builder_.CreateCall(lenFn, {recv});
            if (m == "First") {
                // Throw if empty (len == 0).
                llvm::Value* isEmpty = builder_.CreateICmpEQ(
                    len, builder_.getInt64(0));
                emitGuardThrow(isEmpty, "InvalidOperationException");
            } else {
                // Single(): throw if len != 1 (covers both empty and multi).
                llvm::Value* notOne = builder_.CreateICmpNE(
                    len, builder_.getInt64(1));
                emitGuardThrow(notOne, "InvalidOperationException");
            }
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
            // per stdlib §5 line 2449 + 2515: throw InvalidOperationException if empty.
            llvm::Value* isEmpty = builder_.CreateICmpEQ(len, builder_.getInt64(0));
            emitGuardThrow(isEmpty, "InvalidOperationException");
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
            // per stdlib §5 line 2515: ElementAt throws IndexOutOfRangeException
            // when the index is out of bounds (negative or >= length).
            auto* lenFn = declareC("bz_array_len", i64(), {ptrTy()});
            llvm::Value* len = builder_.CreateCall(lenFn, {recv});
            llvm::Value* neg = builder_.CreateICmpSLT(idx, builder_.getInt64(0));
            llvm::Value* oor = builder_.CreateICmpSGE(idx, len);
            emitGuardThrow(builder_.CreateOr(neg, oor), "IndexOutOfRangeException");
            llvm::AllocaInst* out = entryAlloca(retTy);
            auto* getFn = declareC("bz_array_get", llvm::Type::getVoidTy(llvm_),
                                   {ptrTy(), i64(), ptrTy(), i64()});
            builder_.CreateCall(getFn, {recv, idx, out, builder_.getInt64(esz)});
            llvm::Value* eav = builder_.CreateLoad(retTy, out, "elemat.r");
            freeLazyTemp();  // FINDING [5]: release materialized lazy snapshot
            return eav;
        }
        // stdlib §5: FirstOrNull() / FirstOrNull(predicate): return the first
        // element as T? (null if empty). For value-type T, the result is the
        // tagged {i8 hasValue; T value} struct; for class/string T it is a
        // nullable pointer (null on empty).
        // stdlib §5: FirstOrNull() — first element as T?, null if empty.
        // stdlib §5: LastOrNull()  — last element as T?, null if empty.
        // stdlib §5: SingleOrNull() — single element as T?, null if not exactly one.
        // These are materializing terminals (lazy sources were pre-materialized
        // above into `recv`). The result is T? — for int/record/enum: the tagged
        // {i8 hasValue; T value} struct; for string/class: a nullable pointer.
        if ((m == "FirstOrNull" || m == "LastOrNull" || m == "SingleOrNull") &&
            call.args.empty()) {
            llvm::Type* retTy = getElemTy();
            auto* lenFn = declareC("bz_array_len", i64(), {ptrTy()});
            llvm::Value* len = builder_.CreateCall(lenFn, {recv});
            llvm::Value* condV;
            if (m == "SingleOrNull")
                condV = builder_.CreateICmpEQ(len, builder_.getInt64(1));
            else
                condV = builder_.CreateICmpSGT(len, builder_.getInt64(0));
            auto* loadBB = llvm::BasicBlock::Create(llvm_, "on.load", currentFn_);
            auto* contBB = llvm::BasicBlock::Create(llvm_, "on.cont", currentFn_);
            llvm::BasicBlock* preBB = builder_.GetInsertBlock();
            builder_.CreateCondBr(condV, loadBB, contBB);

            builder_.SetInsertPoint(loadBB);
            llvm::AllocaInst* slot = entryAlloca(retTy);
            auto* getFn = declareC("bz_array_get", llvm::Type::getVoidTy(llvm_),
                                   {ptrTy(), i64(), ptrTy(), i64()});
            llvm::Value* idx64;
            if (m == "LastOrNull") {
                idx64 = builder_.CreateSub(len, builder_.getInt64(1));
            } else {
                idx64 = builder_.getInt64(0);
            }
            builder_.CreateCall(getFn, {recv, idx64, slot, builder_.getInt64(esz)});
            llvm::Value* loaded = builder_.CreateLoad(retTy, slot, "on.v");
            builder_.CreateBr(contBB);

            builder_.SetInsertPoint(contBB);

            // Build T? result — check element ref for nullable value type.
            // In the isArr section, elemRef is not local — use recvTy->genericArgs[0].
            sema::TypeRef* elemRefNN = (recvTy && !recvTy->genericArgs.empty())
                ? recvTy->genericArgs[0] : nullptr;
            // Make a nullable copy of the element type for the return struct.
            sema::TypeRef nullRT;
            if (elemRefNN) { nullRT = *elemRefNN; nullRT.nullable = true; }
            bool isValTy = elemRefNN && isNullableValueTy(&nullRT);

            // IMPORTANT: PHI nodes must be the FIRST instructions in contBB.
            // Create all PHI nodes before calling freeLazyTemp() (which emits
            // a bz_array_free call in contBB and would violate the "PHI at top"
            // LLVM invariant if called first).
            llvm::Value* resultV = nullptr;
            if (isValTy) {
                auto* nst = nullableValueStructTy(&nullRT);
                auto* valPhi = builder_.CreatePHI(retTy, 2, "on.val");
                valPhi->addIncoming(loaded, loadBB);
                valPhi->addIncoming(llvm::Constant::getNullValue(retTy), preBB);
                auto* hvPhi = builder_.CreatePHI(nullablePrimHasValTy(), 2, "on.hv");
                hvPhi->addIncoming(
                    llvm::ConstantInt::get(nullablePrimHasValTy(), 1), loadBB);
                hvPhi->addIncoming(
                    llvm::ConstantInt::get(nullablePrimHasValTy(), 0), preBB);
                // Free AFTER the PHI nodes.
                freeLazyTemp();
                llvm::Value* res = builder_.CreateInsertValue(
                    llvm::UndefValue::get(nst), hvPhi, {0u});
                resultV = builder_.CreateInsertValue(res, valPhi, {1u});
            } else {
                // Pointer / class / string: null pointer = null.
                auto* ptrPhi = builder_.CreatePHI(retTy, 2, "on.ptr");
                ptrPhi->addIncoming(loaded, loadBB);
                ptrPhi->addIncoming(llvm::Constant::getNullValue(retTy), preBB);
                freeLazyTemp();  // Free AFTER the PHI node.
                resultV = ptrPhi;
            }
            return resultV;
        }
        // stdlib §5 (#650): FirstOrNull(predicate: (T)->bool): T? — find the
        // first element satisfying the predicate, or null if none does.
        // Analogous to First(pred) but returns T? instead of throwing.
        // For value-type T: returns {i8 hasValue; T value}; for class/string T:
        // returns a nullable pointer (null on no match).
        if (m == "FirstOrNull" && call.args.size() == 1 && call.args[0].value) {
            const bool relCl = argIsInlineLambda(0);
            llvm::Value* closure = lowerLinqLambda(call.args[0].value.get(),
                ctx_.primitiveTypeRef(sema::PrimitiveType::Bool));
            if (!closure) return nullptr;
            llvm::Type* elemTy = getElemTy();
            auto* lenFn = declareC("bz_array_len", i64(), {ptrTy()});
            llvm::Value* len = builder_.CreateCall(lenFn, {recv});
            auto* resultSlot = entryAlloca(elemTy);
            auto* foundSlot   = entryAlloca(i1());
            builder_.CreateStore(builder_.getFalse(), foundSlot);
            auto* idxSlot = entryAlloca(i64());
            builder_.CreateStore(builder_.getInt64(0), idxSlot);
            auto* condBB  = llvm::BasicBlock::Create(llvm_, "fon.c", currentFn_);
            auto* bodyBB  = llvm::BasicBlock::Create(llvm_, "fon.b", currentFn_);
            auto* foundBB = llvm::BasicBlock::Create(llvm_, "fon.f", currentFn_);
            auto* nextBB  = llvm::BasicBlock::Create(llvm_, "fon.n", currentFn_);
            auto* endBB   = llvm::BasicBlock::Create(llvm_, "fon.e", currentFn_);
            builder_.CreateBr(condBB);
            builder_.SetInsertPoint(condBB);
            llvm::Value* idx = builder_.CreateLoad(i64(), idxSlot);
            builder_.CreateCondBr(builder_.CreateICmpSLT(idx, len), bodyBB, endBB);
            builder_.SetInsertPoint(bodyBB);
            auto* ptrFn = declareC("bz_array_ptr_at", ptrTy(),
                                   {ptrTy(), i64(), i64()});
            llvm::Value* ep = builder_.CreateCall(ptrFn,
                {recv, idx, builder_.getInt64(esz)});
            llvm::Value* el = builder_.CreateLoad(elemTy, ep);
            llvm::Value* ok = callClosure(closure, el, i1());
            builder_.CreateCondBr(ok, foundBB, nextBB);
            builder_.SetInsertPoint(foundBB);
            builder_.CreateStore(el, resultSlot);
            builder_.CreateStore(builder_.getTrue(), foundSlot);
            builder_.CreateBr(endBB);
            builder_.SetInsertPoint(nextBB);
            builder_.CreateStore(
                builder_.CreateAdd(idx, builder_.getInt64(1)), idxSlot);
            builder_.CreateBr(condBB);
            builder_.SetInsertPoint(endBB);
            if (relCl) emitClosureRelease(closure);

            // Build T? result — same logic as the no-predicate *OrNull path.
            sema::TypeRef* elemRefNN = (recvTy && !recvTy->genericArgs.empty())
                ? recvTy->genericArgs[0] : nullptr;
            sema::TypeRef nullRT;
            if (elemRefNN) { nullRT = *elemRefNN; nullRT.nullable = true; }
            bool isValTy = elemRefNN && isNullableValueTy(&nullRT);

            llvm::Value* found = builder_.CreateLoad(i1(), foundSlot);
            llvm::Value* notFound = builder_.CreateNot(found);
            llvm::Value* loadedV  = builder_.CreateLoad(elemTy, resultSlot, "fon.v");
            llvm::Value* resultV  = nullptr;
            if (isValTy) {
                auto* nst = nullableValueStructTy(&nullRT);
                llvm::Value* res = builder_.CreateInsertValue(
                    llvm::UndefValue::get(nst),
                    builder_.CreateSelect(notFound,
                        llvm::ConstantInt::get(nullablePrimHasValTy(), 0),
                        llvm::ConstantInt::get(nullablePrimHasValTy(), 1)),
                    {0u});
                resultV = builder_.CreateInsertValue(
                    res,
                    builder_.CreateSelect(notFound,
                        llvm::Constant::getNullValue(elemTy), loadedV),
                    {1u});
            } else {
                // Pointer / class / string: null pointer when not found.
                resultV = builder_.CreateSelect(notFound,
                    llvm::Constant::getNullValue(elemTy), loadedV,
                    "fon.ptr");
            }
            freeLazyTemp();
            return resultV;
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

        // per §3 / #472: .Sort(comparer: (T,T)->int) — in-place sort with
        // user-supplied comparison closure.  `bz_array_sort_closure` takes a
        // closure-aware trampoline `int(void* cl, const void* a, const void* b)`
        // so qsort can reach the user lambda without a global pointer.
        if (m == "Sort" && call.args.size() == 1 && call.args[0].value) {
            llvm::Type* elemTy2 = getElemTy();
            // fix #533 / §4.4: the comparer is (T,T)->int — TWO params of
            // element type.  lowerLinqLambda only adds ONE param to the synthetic
            // funcRef used to infer unannotated lambda param types; for the second
            // param `b` in `(a,b)=>a.X-b.X` with a record T, no type hint was
            // injected, fieldAddr on `b.X` returned null, and the lambda body
            // emitted `ret void` into an i32-returning function → IR failure.
            // Fix: build a two-param funcRef so both lambda params get the
            // correct element TypeRef as their type hint.
            sema::TypeRef* intRef = ctx_.primitiveTypeRef(sema::PrimitiveType::Int);
            sema::TypeRef* elemTypeRef = (recvTy && !recvTy->genericArgs.empty())
                ? recvTy->genericArgs[0] : nullptr;
            auto* cmpFuncRef = ctx_.makeTypeRef();
            cmpFuncRef->kind = sema::TypeRef::Kind::Function;
            cmpFuncRef->funcReturn = intRef;
            if (elemTypeRef) {
                sema::TypeRef::FuncParam fp1;
                fp1.type = elemTypeRef;
                cmpFuncRef->funcParams.push_back(std::move(fp1));
                sema::TypeRef::FuncParam fp2;
                fp2.type = elemTypeRef;
                cmpFuncRef->funcParams.push_back(std::move(fp2));
            }
            const sema::TypeRef* savedExpected = expectedFunctionType_;
            expectedFunctionType_ = cmpFuncRef;
            llvm::Value* closure = lowerExpr(*call.args[0].value.get());
            expectedFunctionType_ = savedExpected;
            if (!closure) return nullptr;

            // Build a fresh trampoline: int(void* closure, const void* a, const void* b)
            // Loads T from a and b, calls the closure as fn(env, T_a, T_b) → int.
            auto savedIP = builder_.saveIP();
            llvm::Function* savedFn = currentFn_;
            auto* tTy = llvm::FunctionType::get(
                i32(), {ptrTy(), ptrTy(), ptrTy()}, false);
            auto* tramp = llvm::Function::Create(
                tTy, llvm::Function::InternalLinkage, "bz$sortcmp", module_.get());
            currentFn_ = tramp;
            auto* trampBB = llvm::BasicBlock::Create(llvm_, "entry", tramp);
            builder_.SetInsertPoint(trampBB);
            {
                llvm::Value* cl = tramp->getArg(0);
                llvm::Value* ap = tramp->getArg(1);
                llvm::Value* bp = tramp->getArg(2);
                llvm::Value* av = builder_.CreateLoad(elemTy2, ap, "cmp.a");
                llvm::Value* bv = builder_.CreateLoad(elemTy2, bp, "cmp.b");
                llvm::Value* fnPtr = builder_.CreateLoad(ptrTy(), cl, "cl.fn");
                llvm::Value* envSlot = builder_.CreateGEP(ptrTy(), cl,
                                           {builder_.getInt32(1)});
                llvm::Value* envPtr = builder_.CreateLoad(ptrTy(), envSlot, "cl.env");
                auto* clTy = llvm::FunctionType::get(i32(), {ptrTy(), elemTy2, elemTy2}, false);
                llvm::Value* r = builder_.CreateCall(clTy, fnPtr, {envPtr, av, bv}, "cmp.r");
                builder_.CreateRet(r);
            }
            builder_.restoreIP(savedIP);
            currentFn_ = savedFn;

            auto* sortClFn = declareC("bz_array_sort_closure",
                llvm::Type::getVoidTy(llvm_),
                {ptrTy(), i64(), ptrTy(), ptrTy()});
            builder_.CreateCall(sortClFn, {recv, builder_.getInt64(esz), closure, tramp});
            // Release inline-lambda closure after the sort completes (#422).
            if (argIsInlineLambda(0)) emitClosureRelease(closure);
            return llvm::ConstantInt::get(i32(), 0);
        }

        // per §3 / #472: Array<T>.Reversed() — returns a new Array<T> with
        // elements in reverse order; the receiver `arr` is unchanged.
        // Implementation: shallow-copy via bz_array_copy, then in-place reverse.
        if (m == "Reversed" && call.args.empty() &&
            recvTy && recvTy->astNode && recvTy->astNode->name &&
            !recvTy->astNode->name->parts.empty() &&
            recvTy->astNode->name->parts[0] == "Array") {
            llvm::Type* elemTy = getElemTy();
            auto* copyFn = declareC("bz_array_copy", ptrTy(), {ptrTy(), i64()});
            llvm::Value* copy = builder_.CreateCall(copyFn, {recv, builder_.getInt64(esz)},
                                                    "rev.copy");
            // #429 / §4.6 / §28.9: a copied Array<string> independently co-owns a
            // +1 per element (it shares the source's element pointers). Retain each
            // so the copy can be released symmetrically at its own free — else
            // releasing un-retained elements would prematurely free strings the
            // source still owns (a double-free). retain is null/sentinel-safe.
            emitArrayRetainStringsIfStringArray(recvTy, copy);
            // In-place reverse on the copy (same logic as Reverse()).
            auto* lenFn = declareC("bz_array_len", i64(), {ptrTy()});
            llvm::Value* len = builder_.CreateCall(lenFn, {copy});
            auto* idxSlot = entryAlloca(i64());
            builder_.CreateStore(builder_.getInt64(0), idxSlot);
            auto* condBB = llvm::BasicBlock::Create(llvm_, "revd.c", currentFn_);
            auto* bodyBB = llvm::BasicBlock::Create(llvm_, "revd.b", currentFn_);
            auto* endBB  = llvm::BasicBlock::Create(llvm_, "revd.e", currentFn_);
            builder_.CreateBr(condBB);
            builder_.SetInsertPoint(condBB);
            llvm::Value* i = builder_.CreateLoad(i64(), idxSlot);
            llvm::Value* half = builder_.CreateSDiv(len, builder_.getInt64(2));
            builder_.CreateCondBr(builder_.CreateICmpSLT(i, half), bodyBB, endBB);
            builder_.SetInsertPoint(bodyBB);
            auto* ptrFn = declareC("bz_array_ptr_at", ptrTy(), {ptrTy(), i64(), i64()});
            llvm::Value* j = builder_.CreateSub(builder_.CreateSub(len, i),
                                                builder_.getInt64(1));
            llvm::Value* pa = builder_.CreateCall(ptrFn, {copy, i, builder_.getInt64(esz)});
            llvm::Value* pb = builder_.CreateCall(ptrFn, {copy, j, builder_.getInt64(esz)});
            llvm::Value* ta = builder_.CreateLoad(elemTy, pa);
            llvm::Value* tb = builder_.CreateLoad(elemTy, pb);
            builder_.CreateStore(tb, pa);
            builder_.CreateStore(ta, pb);
            builder_.CreateStore(builder_.CreateAdd(i, builder_.getInt64(1)), idxSlot);
            builder_.CreateBr(condBB);
            builder_.SetInsertPoint(endBB);
            return copy;
        }

        // per §3 / #472: Array<T>.Sorted() — returns a new sorted Array<T>
        // (no-comparer form; T must be IComparable<T>: int or string here).
        // Implementation: shallow-copy, then in-place Sort() on the copy.
        if (m == "Sorted" && call.args.empty() &&
            recvTy && recvTy->astNode && recvTy->astNode->name &&
            !recvTy->astNode->name->parts.empty() &&
            recvTy->astNode->name->parts[0] == "Array") {
            llvm::Type* elemTy2 = getElemTy();
            auto* copyFn = declareC("bz_array_copy", ptrTy(), {ptrTy(), i64()});
            llvm::Value* copy = builder_.CreateCall(copyFn, {recv, builder_.getInt64(esz)},
                                                    "srtd.copy");
            // #429: a copied Array<string> co-owns a +1 per element (see Reversed).
            emitArrayRetainStringsIfStringArray(recvTy, copy);
            // Re-use the same built-in comparator logic as Sort() no-arg.
            bool isStringElem2 = (elemTy2 == ptrTy());
            llvm::Function* cmpFn2 = nullptr;
            if (isStringElem2) {
                auto* cmpName = "bz$strcmp";
                cmpFn2 = module_->getFunction(cmpName);
                if (!cmpFn2) {
                    auto* cmpTy = llvm::FunctionType::get(i32(), {ptrTy(), ptrTy()}, false);
                    cmpFn2 = llvm::Function::Create(cmpTy, llvm::Function::InternalLinkage,
                                                    cmpName, module_.get());
                    auto* bb2 = llvm::BasicBlock::Create(llvm_, "entry", cmpFn2);
                    llvm::IRBuilder<> cmpB2(bb2);
                    llvm::Value* aSlot = &*cmpFn2->arg_begin();
                    llvm::Value* bSlot = &*std::next(cmpFn2->arg_begin());
                    llvm::Value* aStr = cmpB2.CreateLoad(ptrTy(), aSlot);
                    llvm::Value* bStr = cmpB2.CreateLoad(ptrTy(), bSlot);
                    llvm::Value* aData = cmpB2.CreateLoad(ptrTy(),
                        cmpB2.CreateStructGEP(stringType(), aStr, 0));
                    llvm::Value* bData = cmpB2.CreateLoad(ptrTy(),
                        cmpB2.CreateStructGEP(stringType(), bStr, 0));
                    auto* strcmpFn2 = module_->getFunction("strcmp");
                    if (!strcmpFn2) {
                        auto* ft = llvm::FunctionType::get(i32(), {ptrTy(), ptrTy()}, false);
                        strcmpFn2 = llvm::Function::Create(ft, llvm::Function::ExternalLinkage,
                                                           "strcmp", module_.get());
                    }
                    cmpB2.CreateRet(cmpB2.CreateCall(strcmpFn2, {aData, bData}));
                }
            } else {
                auto* cmpName = "bz$intcmp";
                cmpFn2 = module_->getFunction(cmpName);
                if (!cmpFn2) {
                    auto* cmpTy = llvm::FunctionType::get(i32(), {ptrTy(), ptrTy()}, false);
                    cmpFn2 = llvm::Function::Create(cmpTy, llvm::Function::InternalLinkage,
                                                    cmpName, module_.get());
                    auto* bb = llvm::BasicBlock::Create(llvm_, "entry", cmpFn2);
                    llvm::IRBuilder<> cmpB(bb);
                    llvm::Value* a = &*cmpFn2->arg_begin();
                    llvm::Value* b = &*std::next(cmpFn2->arg_begin());
                    llvm::Value* av = cmpB.CreateLoad(i32(), a);
                    llvm::Value* bv = cmpB.CreateLoad(i32(), b);
                    cmpB.CreateRet(cmpB.CreateSub(av, bv));
                }
            }
            auto* sortFn2 = declareC("bz_array_sort", llvm::Type::getVoidTy(llvm_),
                                     {ptrTy(), i64(), ptrTy()});
            builder_.CreateCall(sortFn2, {copy, builder_.getInt64(esz), cmpFn2});
            return copy;
        }

        // per §3 / #472: Array<T>.Sorted(comparer: (T,T)->int) — returns a new
        // sorted Array<T> using the user-supplied comparer.
        // Implementation: shallow-copy, then in-place Sort(comparer) on the copy.
        if (m == "Sorted" && call.args.size() == 1 && call.args[0].value &&
            recvTy && recvTy->astNode && recvTy->astNode->name &&
            !recvTy->astNode->name->parts.empty() &&
            recvTy->astNode->name->parts[0] == "Array") {
            llvm::Type* elemTy2 = getElemTy();
            auto* copyFn = declareC("bz_array_copy", ptrTy(), {ptrTy(), i64()});
            llvm::Value* copy = builder_.CreateCall(copyFn, {recv, builder_.getInt64(esz)},
                                                    "srtdc.copy");
            // #429: a copied Array<string> co-owns a +1 per element (see Reversed).
            emitArrayRetainStringsIfStringArray(recvTy, copy);
            // fix #533 / §4.4: same two-param funcRef fix as Sort(comparer) above.
            // The comparer is (T,T)->int; build a two-param type hint so that
            // unannotated lambda params (e.g. `(p,q)=>p.X-q.X` on Array<Point>)
            // both receive the element TypeRef and field access resolves correctly.
            sema::TypeRef* intRef2 = ctx_.primitiveTypeRef(sema::PrimitiveType::Int);
            sema::TypeRef* elemTypeRef2 = (recvTy && !recvTy->genericArgs.empty())
                ? recvTy->genericArgs[0] : nullptr;
            auto* cmpFuncRef2 = ctx_.makeTypeRef();
            cmpFuncRef2->kind = sema::TypeRef::Kind::Function;
            cmpFuncRef2->funcReturn = intRef2;
            if (elemTypeRef2) {
                sema::TypeRef::FuncParam fp1;
                fp1.type = elemTypeRef2;
                cmpFuncRef2->funcParams.push_back(std::move(fp1));
                sema::TypeRef::FuncParam fp2;
                fp2.type = elemTypeRef2;
                cmpFuncRef2->funcParams.push_back(std::move(fp2));
            }
            const sema::TypeRef* savedExpected2 = expectedFunctionType_;
            expectedFunctionType_ = cmpFuncRef2;
            llvm::Value* closure = lowerExpr(*call.args[0].value.get());
            expectedFunctionType_ = savedExpected2;
            if (!closure) return nullptr;

            // Build trampoline: int(void* closure, const void* a, const void* b)
            auto savedIP2 = builder_.saveIP();
            llvm::Function* savedFn2 = currentFn_;
            auto* tTy2 = llvm::FunctionType::get(
                i32(), {ptrTy(), ptrTy(), ptrTy()}, false);
            auto* tramp2 = llvm::Function::Create(
                tTy2, llvm::Function::InternalLinkage, "bz$sortcmpc", module_.get());
            currentFn_ = tramp2;
            auto* trampBB2 = llvm::BasicBlock::Create(llvm_, "entry", tramp2);
            builder_.SetInsertPoint(trampBB2);
            {
                llvm::Value* cl = tramp2->getArg(0);
                llvm::Value* ap = tramp2->getArg(1);
                llvm::Value* bp = tramp2->getArg(2);
                llvm::Value* av = builder_.CreateLoad(elemTy2, ap, "cmpc.a");
                llvm::Value* bv = builder_.CreateLoad(elemTy2, bp, "cmpc.b");
                llvm::Value* fnPtr = builder_.CreateLoad(ptrTy(), cl, "cl.fn");
                llvm::Value* envSlot = builder_.CreateGEP(ptrTy(), cl,
                                           {builder_.getInt32(1)});
                llvm::Value* envPtr = builder_.CreateLoad(ptrTy(), envSlot, "cl.env");
                auto* clTy = llvm::FunctionType::get(i32(), {ptrTy(), elemTy2, elemTy2}, false);
                llvm::Value* r = builder_.CreateCall(clTy, fnPtr, {envPtr, av, bv}, "cmpc.r");
                builder_.CreateRet(r);
            }
            builder_.restoreIP(savedIP2);
            currentFn_ = savedFn2;

            auto* sortClFn2 = declareC("bz_array_sort_closure",
                llvm::Type::getVoidTy(llvm_),
                {ptrTy(), i64(), ptrTy(), ptrTy()});
            builder_.CreateCall(sortClFn2, {copy, builder_.getInt64(esz), closure, tramp2});
            // Release inline-lambda closure after the sort completes (#422).
            if (argIsInlineLambda(0)) emitClosureRelease(closure);
            return copy;
        }

        // #602 / §28.11 v0.134: hoist one element-slot policy lookup for all
        // array-backed collection operations that need to know whether elements
        // need retain/release bookkeeping (RuntimeManaged row), disposal
        // (ClassOwned row), or class-reference equality (ClassBorrow row).
        //
        // arrElemRef: the sema TypeRef for the element type (genericArgs[0]);
        // null for unparameterized collections (handled safely by elementPolicyFor).
        //
        // owning family flag: the collection name starts with "Owned" (§28.11 table).
        // NOTE: the Set/OwnedSet branch above already handled dict-backed sets and
        // returned; everything from here down is array-backed.
        const sema::TypeRef* arrElemRef =
            (recvTy && !recvTy->genericArgs.empty()) ? recvTy->genericArgs[0]
                                                     : nullptr;
        bool arrIsOwning = recvTy && recvTy->astNode && recvTy->astNode->name &&
                           !recvTy->astNode->name->parts.empty() &&
                           (recvTy->astNode->name->parts[0].rfind("Owned", 0) == 0);
        const ElementPolicy arrElemPol =
            elementPolicyFor(arrElemRef, arrIsOwning);
        // Convenience predicates derived from the policy (replacing the old
        // delegate calls; behavior preserved exactly):
        //
        //   arrIsStringElem: RuntimeManaged + non-nullable string primitive
        //     (same guard as emitArrayRetainStringsIfStringArray; the collection-
        //     name guard is already enforced above: the Set/OwnedSet branch
        //     returned, so we are in an array-backed collection; Owned* is
        //     excluded at instantiation, §15.2).
        //     #602 batch 4: isStringElementArray was deleted; the RuntimeManaged
        //     check here directly mirrors the policy.
        //
        // per §28.11 v0.135 / fix #619: arrElemPol is derived from
        // elementPolicyFor which now correctly covers both Class and Interface
        // element types.  No DIVERGENCE — the factory handles the full
        // class/interface row of the §28.11 table.
        bool arrIsStringElem =
            arrElemPol.cat == ElementPolicy::Category::RuntimeManaged &&
            arrElemRef &&
            arrElemRef->kind == sema::TypeRef::Kind::Primitive &&
            arrElemRef->primitive == sema::PrimitiveType::String &&
            !arrElemRef->nullable;
        // arrElemIsClass: whether the element is a class-or-interface type —
        // drives both the ClassOwned disposer (for owning arrays) and ClassBorrow
        // reference identity (for emitRemoveElementEquality).  Per §28.11 v0.135
        // fix #619: Interface is now included (elementPolicyFor returns ClassOwned/
        // ClassBorrow for interface elements, collectionElemIsClass widened).
        bool arrElemIsClass =
            arrElemPol.cat == ElementPolicy::Category::ClassOwned ||
            arrElemPol.cat == ElementPolicy::Category::ClassBorrow;

        // per §3/§25.2: .IndexOf(item): int — index of first match, -1 if not
        // found. Match semantics (issue #424 / §25.2): dispatched per element
        // type by emitRemoveElementEquality — record structural equality (§8.3),
        // class user Equals/reference (§8.3/§8.4), string content (§4.6), value
        // type `==`. The query `target` is a PURE BORROW — not consumed or
        // disposed by IndexOf (§4.4/§4.5).
        if (m == "IndexOf" && call.args.size() == 1 && call.args[0].value) {
            llvm::Value* target = lowerExpr(*call.args[0].value);
            if (!target) return nullptr;
            llvm::Type* elemTy = getElemTy();
            // #602 / §28.11 v0.134: derive elemIsClass from arrElemPol (hoisted above)
            // instead of calling collectionElemIsClass(recvTy, 0) per-handler.
            const sema::TypeRef* elemRef = arrElemRef;  // from policy hoist
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
            llvm::Value* eq =
                emitRemoveElementEquality(el, target, elemRef, arrElemIsClass);
            if (!eq) return nullptr;
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

        // fix #455 / §4.5: when the element type is a nullable VALUE type
        // (int?, bool?, enum?, record?) the lowered argument may be the inner
        // primitive/enum/record value rather than the full tagged struct
        // {i8 hasValue; T value} that the runtime expects.  If v's LLVM type
        // is the inner type (not yet the tagged struct), wrap it here so the
        // alloca matches the esz that collectionElemSize computed.
        // Two cases arise:
        //   (a) v is the inner scalar (i32/i1/…) — wrap as {hasValue=1, v}.
        //   (b) v is a null pointer (from an untyped `null` literal whose sema
        //       type was not resolved to int?) — produce {hasValue=0, 0}.
        // If the sema type WAS resolved to int? correctly, lowerExpr already
        // returns the tagged struct and v->getType() == expected → no-op.
        // fix #569 / §14.1: inside a monomorphized generic body the element
        // genericArg is a BARE nullable type-parameter (`T?`), for which
        // isNullableValueTy is false (T resolves to no known value type). Resolve
        // it through the active monomorphization (genericSubstSemaTy_, the #567/
        // #568 mechanism) to the concrete nullable value type so Add wraps the
        // bare T value into the SAME tagged {i8, T} struct the (now 8-byte) esz
        // and the caller-side `arr[i]` decode expect. Without this the bare value
        // was stored at the nullable element's stride → garbage tag/value on read.
        auto ensureNullableElem = [&](llvm::Value* v) -> llvm::Value* {
            const sema::TypeRef* elemTyRef =
                (recvTy && !recvTy->genericArgs.empty())
                ? recvTy->genericArgs[0] : nullptr;
            if (!elemTyRef) return v;
            sema::TypeRef nvBuf;
            const sema::TypeRef* nvTy = nullptr;
            if (isNullableValueTy(elemTyRef)) {
                nvTy = elemTyRef;
            } else if (elemTyRef->nullable &&
                       elemTyRef->kind == sema::TypeRef::Kind::Named &&
                       !elemTyRef->declaration && !genericSubstSemaTy_.empty()) {
                if (nullableTypeParamConcrete(elemTyRef, nvBuf) &&
                    isNullableValueTy(&nvBuf))
                    nvTy = &nvBuf;
            }
            if (!nvTy) return v;
            llvm::StructType* expected = nullableValueStructTy(nvTy);
            if (!expected || v->getType() == expected) return v;
            // (b) null pointer → null tagged struct.
            if (v->getType()->isPointerTy())
                return nullableValueNull(nvTy);
            // (a) inner scalar/struct value → {hasValue=1, v}.
            return wrapNullableValue(nvTy, v);
        };

        // .Add(v) / .Push(v) / .Enqueue(v)
        if ((m == "Add" || m == "Push" || m == "Enqueue") &&
            call.args.size() == 1 && call.args[0].value) {
            llvm::Value* v = lowerExpr(*call.args[0].value);
            if (!v) return nullptr;
            v = ensureNullableElem(v);
            // #602 / #429 / §4.6 / §28.9 / §28.11 v0.134: a borrowing Array<string>
            // co-owns a +1 per element. Take an INDEPENDENT +1 at insert, released
            // per element on free (via stringElementReleaseThunk — RuntimeManaged row).
            // Mirror the field-store rule: a fresh +1 producer is TRANSFERRED (its
            // single reference becomes the array's — no retain); anything else is
            // RETAINED. retain is null-safe and a no-op on a sentinel/literal.
            // arrIsStringElem is derived from arrElemPol (hoisted above).
            if (arrIsStringElem &&
                !isPlusOneStringProducer(call.args[0].value.get()))
                emitStringRetain(v);
            // Store elem to stack alloca, pass pointer to runtime
            llvm::AllocaInst* slot = entryAlloca(v->getType());
            builder_.CreateStore(v, slot);
            auto* fn = declareC("bz_array_add", llvm::Type::getVoidTy(llvm_),
                                {ptrTy(), ptrTy(), i64()});
            builder_.CreateCall(fn, {recv, slot, builder_.getInt64(esz)});
            return llvm::ConstantInt::get(i32(), 0);  // unit/void result
        }
        // fix #654-tail B / stdlib §4.10: OwnedSet<T>.TryAdd(owned item): bool
        // for class-typed T (array-backed path — class elements are ineligible
        // for the dict-backed set path since genericBackingType returns null for
        // class types).  Equality for class T is reference identity (§8.3).
        // Semantics: scan the array for an element with the same pointer; if not
        // found, append and return true; if found, dispose the passed item
        // (caller transferred ownership; §4.10 "on false, the item is disposed")
        // and return false.
        if (m == "TryAdd" && call.args.size() == 1 && call.args[0].value &&
            arrElemIsClass && arrIsOwning) {
            llvm::Value* item = lowerExpr(*call.args[0].value);
            if (!item) return nullptr;
            auto* lenFn = declareC("bz_array_len", i64(), {ptrTy()});
            llvm::Value* arrLen = builder_.CreateCall(lenFn, {recv}, "ta.len");
            // Create all BBs upfront to avoid CFG wiring errors.
            auto* condBB  = llvm::BasicBlock::Create(llvm_, "ta.c",    currentFn_);
            auto* bodyBB  = llvm::BasicBlock::Create(llvm_, "ta.b",    currentFn_);
            auto* nextBB  = llvm::BasicBlock::Create(llvm_, "ta.next", currentFn_);
            auto* foundBB = llvm::BasicBlock::Create(llvm_, "ta.dup",  currentFn_);
            auto* newBB   = llvm::BasicBlock::Create(llvm_, "ta.new",  currentFn_);
            auto* endBB   = llvm::BasicBlock::Create(llvm_, "ta.end",  currentFn_);
            // idxSlot: loop counter.
            auto* idxSlot = entryAlloca(i64());
            builder_.CreateStore(builder_.getInt64(0), idxSlot);
            builder_.CreateBr(condBB);
            // condBB: exit when idx >= len (not found).
            builder_.SetInsertPoint(condBB);
            llvm::Value* curIdx = builder_.CreateLoad(i64(), idxSlot, "ta.i");
            builder_.CreateCondBr(
                builder_.CreateICmpSLT(curIdx, arrLen), bodyBB, newBB);
            // bodyBB: compare element pointer at curIdx with item.
            builder_.SetInsertPoint(bodyBB);
            auto* ptrFn = declareC("bz_array_ptr_at", ptrTy(),
                                   {ptrTy(), i64(), i64()});
            llvm::Value* ep = builder_.CreateCall(
                ptrFn, {recv, curIdx, builder_.getInt64(esz)}, "ta.ep");
            llvm::Value* existing = builder_.CreateLoad(ptrTy(), ep, "ta.ex");
            builder_.CreateCondBr(
                builder_.CreateICmpEQ(existing, item, "ta.same"), foundBB, nextBB);
            // nextBB: advance index, loop back.
            builder_.SetInsertPoint(nextBB);
            builder_.CreateStore(
                builder_.CreateAdd(curIdx, builder_.getInt64(1)), idxSlot);
            builder_.CreateBr(condBB);
            // foundBB: duplicate — dispose the passed item and return false.
            builder_.SetInsertPoint(foundBB);
            if (arrElemPol.disposerThunk)
                builder_.CreateCall(arrElemPol.disposerThunk, {item});
            builder_.CreateBr(endBB);
            // newBB: not present — append and return true.
            builder_.SetInsertPoint(newBB);
            {
                llvm::AllocaInst* slot = entryAlloca(item->getType());
                builder_.CreateStore(item, slot);
                auto* addFn = declareC("bz_array_add",
                    llvm::Type::getVoidTy(llvm_), {ptrTy(), ptrTy(), i64()});
                builder_.CreateCall(addFn, {recv, slot, builder_.getInt64(esz)});
            }
            builder_.CreateBr(endBB);
            // endBB: PHI for bool result.
            builder_.SetInsertPoint(endBB);
            auto* phi = builder_.CreatePHI(i1(), 2, "ta.r");
            phi->addIncoming(builder_.getFalse(), foundBB);
            phi->addIncoming(builder_.getTrue(), newBB);
            return phi;
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
            v = ensureNullableElem(v);
            // #602 / #429 / §4.6 / §28.9 / §28.11 v0.134: Insert co-owns a +1
            // per string element, exactly like Add — transfer a +1 producer,
            // retain anything else. arrIsStringElem derived from arrElemPol.
            if (arrIsStringElem &&
                !isPlusOneStringProducer(call.args[1].value.get()))
                emitStringRetain(v);
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
                // fix #523 / stdlib §4.11: Peek() raises InvalidOperationException
                // when the stack/queue is empty (same spec rule as Pop/Dequeue).
                auto* lenFn = declareC("bz_array_len", i64(), {ptrTy()});
                llvm::Value* len = builder_.CreateCall(lenFn, {recv});
                llvm::Value* isEmpty = builder_.CreateICmpEQ(
                    len, builder_.getInt64(0), "peek.empty");
                emitGuardThrow(isEmpty, "InvalidOperationException");
                // fix #654-tail A / stdlib §4.11: Peek() FIFO semantics for
                // Queue/OwnedQueue — front element (index 0); Stack/OwnedStack
                // peek the top element (last inserted, index len-1).
                bool isQueuePeek = recvTy && recvTy->astNode &&
                    recvTy->astNode->name &&
                    !recvTy->astNode->name->parts.empty() &&
                    (recvTy->astNode->name->parts[0] == "Queue" ||
                     recvTy->astNode->name->parts[0] == "OwnedQueue");
                if (isQueuePeek)
                    idx = builder_.getInt64(0);
                else
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
            v = ensureNullableElem(v);  // fix #455: lift inner type to tagged struct
            // #602 / #429 / §4.6 / §28.9 / §28.11 v0.134: .Set(i, v) OVERWRITES
            // slot i on a borrowing Array<string> — release the displaced old
            // element and take a +1 on the new one. Use the disposing setter so
            // the runtime releases the old value AFTER reading it (retain-new-
            // before-release-old keeps a self-set `a[i] := a[i]` safe). The
            // disposer is the string-release thunk.
            // arrIsStringElem and arrElemPol.retainRelease derived from arrElemPol.
            if (llvm::Function* strDisp = arrIsStringElem
                                          ? stringElementReleaseThunk()
                                          : nullptr) {
                if (!isPlusOneStringProducer(call.args[1].value.get()))
                    emitStringRetain(v);
                llvm::Value* oor2 = builder_.CreateICmpUGE(
                    idx, builder_.CreateCall(
                        declareC("bz_array_len", i64(), {ptrTy()}), {recv}),
                    "set.oor");
                emitGuardThrow(oor2, "IndexOutOfRangeException");
                llvm::AllocaInst* slot = entryAlloca(v->getType());
                builder_.CreateStore(v, slot);
                auto* fn = declareC("bz_array_set_disposing",
                    llvm::Type::getVoidTy(llvm_),
                    {ptrTy(), i64(), ptrTy(), i64(), ptrTy()});
                builder_.CreateCall(fn, {recv, idx, slot,
                                         builder_.getInt64(esz), strDisp});
                return llvm::ConstantInt::get(i32(), 0);
            }
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
            // #602 / #362 / §4.5 / #429 / §4.6 / §28.9 / §28.11 v0.135 fix #619:
            // OwnedArray<T>.RemoveAt disposes the removed element (ClassOwned row —
            // class or interface); a borrowing Array<string> releases the removed
            // element's co-owned +1 (RuntimeManaged row). The two disposers are
            // mutually exclusive; arrElemPol.disposerThunk is non-null for ClassOwned.
            llvm::Function* elemDisp = arrElemPol.disposerThunk;
            if (!elemDisp && arrIsStringElem) elemDisp = stringElementReleaseThunk();
            if (elemDisp) {
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
            // #602 / §28.11 v0.134: derive elemIsClass and elemRef from arrElemPol
            // (hoisted above) instead of per-handler collectionElemIsClass calls.
            const sema::TypeRef* elemRef = arrElemRef;  // from policy hoist
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
                emitRemoveElementEquality(el, target, elemRef, arrElemIsClass);
            if (!eq) return nullptr;
            builder_.CreateCondBr(eq, foundBB, nextBB);
            builder_.SetInsertPoint(foundBB);
            // Remove (and, for an owning array, dispose) the matched slot.
            // #602 / #429 / §28.11 v0.135 fix #619: arrElemPol.disposerThunk is
            // non-null for ClassOwned (class or interface); arrIsStringElem drives
            // the string-release path.  Both derived from arrElemPol (hoisted above).
            llvm::Function* rmDisp = arrElemPol.disposerThunk;
            if (!rmDisp && arrIsStringElem) rmDisp = stringElementReleaseThunk();
            if (rmDisp) {
                auto* fn = declareC("bz_array_remove_at_disposing",
                    llvm::Type::getVoidTy(llvm_),
                    {ptrTy(), i64(), i64(), ptrTy()});
                builder_.CreateCall(fn, {recv, idx, builder_.getInt64(esz),
                                         rmDisp});
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
            // fix #523 / stdlib §4.11/§4.12: Pop()/Dequeue() raise
            // InvalidOperationException on an empty stack/queue.
            // Get last (Pop) or first (Dequeue) element, then remove it.
            auto* lenFn = declareC("bz_array_len", i64(), {ptrTy()});
            llvm::Value* len = builder_.CreateCall(lenFn, {recv});
            llvm::Value* isEmpty = builder_.CreateICmpEQ(
                len, builder_.getInt64(0), "pop.empty");
            emitGuardThrow(isEmpty, "InvalidOperationException");
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
        // fix #642 / stdlib §4.5: .PopAt(index): owned T / .PopLast(): owned T /
        // .PopFirst(): owned T  — remove and return the element at the given slot,
        // transferring ownership to the caller.  Unlike Pop/Dequeue (Stack/Queue),
        // these are defined on OwnedArray<T> and its alias forms.
        // Ownership rule (§4.5 / §28.11 v0.135): the slot is removed WITHOUT
        // disposal — bz_array_pop_at copies the element bytes BEFORE shifting so
        // the pointer is captured before the shift, then the slot is removed.
        // The caller receives the owned T and is responsible for disposing it.
        if ((m == "PopAt" && call.args.size() == 1 && call.args[0].value) ||
            ((m == "PopLast" || m == "PopFirst") && call.args.empty())) {
            // Determine return type from generic arg.
            llvm::Type* retTy = i32();
            if (recvTy && !recvTy->genericArgs.empty() && recvTy->genericArgs[0])
                retTy = lowerTypeRef(recvTy->genericArgs[0]);
            // Determine index.
            auto* lenFn = declareC("bz_array_len", i64(), {ptrTy()});
            llvm::Value* len = builder_.CreateCall(lenFn, {recv}, "popatl.len");
            llvm::Value* isEmpty = builder_.CreateICmpEQ(
                len, builder_.getInt64(0), "popat.empty");
            emitGuardThrow(isEmpty, "InvalidOperationException");
            llvm::Value* idx = nullptr;
            if (m == "PopAt") {
                llvm::Value* rawIdx = lowerExpr(*call.args[0].value);
                if (!rawIdx) return nullptr;
                idx = builder_.CreateSExt(rawIdx, i64(), "popat.idx");
            } else if (m == "PopLast") {
                idx = builder_.CreateSub(len, builder_.getInt64(1), "popl.idx");
            } else {  // PopFirst
                idx = builder_.getInt64(0);
            }
            llvm::AllocaInst* out = entryAlloca(retTy);
            // bz_array_pop_at: copy element at idx into *out, then remove the
            // slot WITHOUT disposal — ownership transfers to the caller.
            auto* popFn = declareC("bz_array_pop_at",
                llvm::Type::getVoidTy(llvm_), {ptrTy(), i64(), ptrTy(), i64()});
            builder_.CreateCall(popFn,
                {recv, idx, out, builder_.getInt64(esz)});
            return builder_.CreateLoad(retTy, out, "popat.val");
        }
        // .Clear()
        if (m == "Clear" && call.args.empty()) {
            // #602 / #362 / §4.5 / #429 / §4.6 / §28.9 / §28.11 v0.134:
            // OwnedArray<T>.Clear disposes every element (ClassOwned row via
            // arrElemPol.disposerThunk). A borrowing Array<string> releases each
            // element's co-owned +1 (RuntimeManaged row via arrIsStringElem).
            // The two paths are mutually exclusive; all derived from arrElemPol.
            // per §28.11 v0.135 fix #619: Interface elements now included in
            // ClassOwned, so arrElemPol.disposerThunk is non-null for both class
            // and interface element types in the owning family.
            llvm::Function* elemDisp = arrElemPol.disposerThunk;
            if (!elemDisp && arrIsStringElem) elemDisp = stringElementReleaseThunk();
            if (elemDisp) {
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
        // #602 / §28.11 v0.135 fix #619: use arrElemIsClass (derived from
        // arrElemPol — now covers Class|Interface per §28.11 / §14.4).
        if (m == "DisposeElements" && call.args.empty()) {
            if (arrElemIsClass) {
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
            // #602 / #429 / §4.6 / §28.9 / §28.11 v0.134: AddRange into a
            // borrowing Array<string> takes an INDEPENDENT +1 per appended element
            // (the source `other` keeps its own +1). The element is always RETAINED
            // (a borrowed copy from `other`). arrIsStringElem derived from arrElemPol.
            if (arrIsStringElem) {
                llvm::Value* elemVal = builder_.CreateLoad(ptrTy(), ep, "ar.elem");
                emitStringRetain(elemVal);
            }
            auto* addFn = declareC("bz_array_add", llvm::Type::getVoidTy(llvm_),
                                   {ptrTy(), ptrTy(), i64()});
            builder_.CreateCall(addFn, {recv, ep, builder_.getInt64(esz)});
            builder_.CreateStore(builder_.CreateAdd(idx, builder_.getInt64(1)), idxSlot);
            builder_.CreateBr(condBB);
            builder_.SetInsertPoint(endBB);
            return llvm::ConstantInt::get(i32(), 0);
        }
        // per §3/§25.2: .Contains(item: T): bool — first element that Equals
        // the query. Match semantics (issue #424 / §25.2): dispatched per
        // element type by emitRemoveElementEquality — record structural
        // equality (§8.3), class user Equals/reference (§8.3/§8.4), string
        // content (§4.6), value type `==`. The query `target` is a PURE BORROW
        // — not consumed or disposed by Contains (§4.4/§4.5).
        if (m == "Contains" && call.args.size() == 1 && call.args[0].value) {
            llvm::Value* target = lowerExpr(*call.args[0].value);
            if (!target) return nullptr;
            llvm::Type* elemTy = getElemTy();
            // #602 / §28.11 v0.134: derive elemIsClass and elemRef from arrElemPol.
            const sema::TypeRef* elemRef = arrElemRef;  // from policy hoist
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
            llvm::Value* eq =
                emitRemoveElementEquality(el, target, elemRef, arrElemIsClass);
            if (!eq) return nullptr;
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
        // §28.2 v0.135 / issue #627: checked-disposal mode guard for dict-backed
        // collection methods. bz_dict_is_disposed reads the disposed byte on the
        // bz_dict_t header (set by bz_dict_mark_disposed at Dispose time in
        // checked builds).
        if (checkedDisposal_) {
            llvm::Type* i8Ty = llvm::Type::getInt8Ty(llvm_);
            auto* isFn = declareC("bz_dict_is_disposed", i8Ty, {ptrTy()});
            llvm::Value* flag = builder_.CreateCall(isFn, {recv}, "cd.ddis");
            llvm::Value* disposed = builder_.CreateICmpNE(
                flag, llvm::ConstantInt::get(i8Ty, 0), "cd.disposed");
            emitGuardThrow(disposed, "DisposedAccessException");
        }

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
        // issue #539 / §4.8 / §7: nullable value types (int?, bool?, enum?,
        // nullable record) lower to a tagged {i8 hasValue; T} struct.  Like a
        // non-nullable record they are too wide for the scalar inttoptr slot
        // encoding and must be heap-boxed the same way (malloc the struct,
        // store the tag + payload bytes, put the box pointer in the slot).
        // valRec above covers non-nullable records; valNullableVal covers the
        // nullable-value-type case (which slipped to ZExt/Trunc and crashed).
        bool valNullableVal = valTy && isNullableValueTy(valTy);

        // #602 / §28.11 v0.134: hoist one element-slot policy lookup for the
        // dictionary VALUE slot.  Per §28.11 the "value" slot of a Dictionary
        // follows the same borrowing/owning column as the collection family.
        // OwnedDictionary owns its values; borrowing Dictionary borrows/
        // runtime-manages them (string values in the RuntimeManaged row).
        //
        // dictValIsOwning: a dictionary name starting with "Owned" (OwnedDictionary).
        // Note: dictValueBoxFreer (record-value boxing path) is not a policy-table
        // concern — record values are value types and land in the Value row of the
        // §28.11 table.  Their heap-boxing is a STORAGE encoding detail (inttoptr
        // cannot hold an aggregate), not an element-slot lifecycle decision.  We
        // keep dictValueBoxFreer calls unchanged (batch 3 / #602 follow-on).
        bool dictValIsOwning = recvTy && recvTy->astNode && recvTy->astNode->name &&
                               !recvTy->astNode->name->parts.empty() &&
                               (recvTy->astNode->name->parts[0].rfind("Owned", 0) == 0);
        const ElementPolicy dictValPol =
            elementPolicyFor(valTy, dictValIsOwning);
        // Convenience predicate: is this a borrowing Dictionary<K, string>?
        //   RuntimeManaged + non-nullable string primitive + NOT owning.
        // Mirrors isStringValueDict (same exclusions: OwnedDictionary excluded,
        // nullable string? excluded, function types share RuntimeManaged but
        // use a different release path — same string-vs-function distinction).
        bool dictIsStringValue =
            !dictValIsOwning &&
            dictValPol.cat == ElementPolicy::Category::RuntimeManaged &&
            valTy &&
            valTy->kind == sema::TypeRef::Kind::Primitive &&
            valTy->primitive == sema::PrimitiveType::String &&
            !valTy->nullable;
        // dictValClassDisposer: non-null for OwnedDictionary<K, class-or-interface V>.
        // Derived from dictValPol (elementPolicyFor) which now handles both Class
        // and Interface correctly per §28.11 v0.135 fix #619.
        llvm::Function* dictValClassDisposer = dictValPol.disposerThunk;

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
        // issue #539: nullable value types (int?, bool?, etc.) lower to a tagged
        // {i8,T} struct and are also heap-boxed via the same path — they are
        // rejected by ZExt/Trunc (illegal on struct operands).
        // issue #544: #539 only handled the case where the argument ALREADY lowers
        // as a struct (e.g. `var v: int? := 42; d.Set("x", v)` — v is emitted as
        // {i8,i32}).  When the argument is a scalar literal (`d.Add("a", 42)`, or
        // a null-pointer literal), lowerExpr emits i32/null-ptr, not the tagged
        // struct, so the isStructTy() guard was false and the value fell through to
        // ZExt/inttoptr.  Decode then does CreateLoad({i8,i32}, inttoptr'd-ptr) →
        // SIGSEGV.  Fix: when valNullableVal is true but the value is not yet a
        // struct, promote it to the {i8,T} tagged struct (wrapNullableValue for a
        // non-null scalar, nullableValueNull for a null pointer) before boxing.
        auto encodeValue = [&](llvm::Value* v) -> llvm::Value* {
            // issue #544: promote a bare scalar/null to the {i8,T} nullable struct
            // before the struct-boxing branch below, so the box always holds the
            // full tagged struct regardless of whether the arg was already typed
            // as T? or arrived as a plain T scalar / null literal.
            if (valNullableVal && !v->getType()->isStructTy()) {
                if (v->getType()->isPointerTy() &&
                    llvm::isa<llvm::ConstantPointerNull>(v)) {
                    v = nullableValueNull(valTy);   // null literal → {i8=0, T=0}
                } else if (!v->getType()->isPointerTy()) {
                    v = wrapNullableValue(valTy, v); // scalar T → {i8=1, T=v}
                }
                // pointer (non-null class pointer stored as T?) falls through to
                // the pointer case below — class/string pointers are not boxed.
            }
            // fix #570 / §4.8 / §7 / §9: nullable RECORD value (Item?) — when
            // the caller passes the inner record struct directly (e.g. `new Item(42)`
            // implicitly coerced to Item?), the value is an Item struct (not yet
            // the tagged {i8,Item} struct).  The #544 promotion above skips it
            // (isStructTy() is true so `!isStructTy()` is false).  Wrap it into
            // the {i8=1, Item} tagged struct before boxing so decode always sees
            // the full tagged struct.  Detect by comparing against the outer tagged
            // struct type: if they differ the inner record must be wrapped.
            if (valNullableVal && v->getType()->isStructTy() &&
                v->getType() != nullableValueStructTy(valTy))
                v = wrapNullableValue(valTy, v);  // inner record → {i8=1, record}
            if ((valRec || valNullableVal) && v->getType()->isStructTy()) {
                // Box the struct (record or nullable-value-type tagged struct)
                // into a malloc'd region.  The tag byte for nullable types is
                // preserved in the stored bytes, so null vs non-null survives.
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
        //   * #429 / §28.9: borrowing Dictionary<K, string> → release each
        //     surviving string value's +1 (stringElementReleaseThunk). The dict
        //     co-owns a +1 per stored value; Clear reclaims them all, exactly
        //     mirroring the scope-end free. A value displaced earlier by the
        //     indexer setter (#429 overwrite path) was already released there —
        //     not in the table here, no double-release.
        //   * everything else (scalar, borrowing class value) → no disposal.
        // bz_dict_clear handles BOTH the string-key and generic-key tables (it
        // frees e.key — a strndup'd copy in string mode, a malloc'd byte copy in
        // generic mode — for every live entry).
        if (m == "Clear" && call.args.empty()) {
            // #602 / §28.11 v0.134: derive disposer from dictValPol (hoisted above).
            // dictValClassDisposer: non-null for OwnedDictionary<K,class> (ClassOwned).
            // dictValueBoxFreer: kept unchanged (value-type record box; not a policy
            //   table decision — batch 3 / #602 follow-on).
            // dictIsStringValue: borrowing Dictionary<K,string> → release each +1.
            // per §28.11 v0.135 fix #619: dictValClassDisposer is now non-null for
            // interface-valued OwnedDictionary (class-or-interface element).
            llvm::Function* disp = dictValClassDisposer;
            if (!disp) disp = dictValueBoxFreer(recvTy);
            if (!disp && dictIsStringValue)
                disp = stringElementReleaseThunk();
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
        // #429 / §28.9: for a borrowing Dictionary<K, string> co-owns a +1 per
        // stored value. Take an INDEPENDENT +1 at insert/overwrite by retaining
        // the string value before storing it. Mirror the field-store rule: a
        // fresh +1 producer is TRANSFERRED (its single reference becomes the
        // dict's — no retain); anything else (borrow/literal/identifier) is
        // RETAINED. retain is null-safe and a no-op on a sentinel/literal.
        // NOTE: .Add and .Set on a BORROWING Dictionary do NOT release a prior
        // value (they are documented to throw DuplicateKey on .Add; .Set simply
        // overwrites without disposal — the indexer setter `d[k] := v` is the
        // overwriting path and IS handled in lowerCollectionIndexerSet).
        if ((m == "Add" || m == "Set") && call.args.size() == 2 &&
            call.args[0].value && call.args[1].value) {
            llvm::Value* k = lowerExpr(*call.args[0].value);
            llvm::Value* v = lowerExpr(*call.args[1].value);
            if (!k || !v) return nullptr;
            // #602 / #429 / §28.9 / §28.11 v0.134: retain the string value BEFORE
            // encoding (v is still the raw bz_string* at this point; encodeValue
            // returns it as-is for pointers). dictIsStringValue derived from dictValPol.
            if (dictIsStringValue &&
                !isPlusOneStringProducer(call.args[1].value.get()))
                emitStringRetain(v);
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
        // .Remove(k): bool — per §4.9: OwnedDictionary.Remove disposes the
        // removed value (exactly once) before unlinking; a non-owning
        // Dictionary.Remove unlinks without disposal (§4.8, borrows).
        // #429 / §28.9: a borrowing Dictionary<K, string> co-owns a +1 per
        // stored value — Remove must release the removed value's +1 (else it
        // leaks). Use the disposing-remove variant with stringElementReleaseThunk
        // as the value-disposer, exactly mirroring the OwnedDictionary path.
        if (m == "Remove" && call.args.size() == 1 && call.args[0].value) {
            llvm::Value* k = lowerExpr(*call.args[0].value);
            if (!k) return nullptr;
            // For an OwnedDictionary<K, class> get the value disposer thunk
            // (same helper Clear uses, so the dispose logic is exactly-once
            // and consistent). For borrowing Dictionary<K, string>, get the
            // string-release thunk. For borrowing Dict or non-class/non-string
            // values, valDisp is null → fall through to plain non-disposing remove.
            // #602 / §28.11 v0.135 fix #619: derive disposer from dictValPol (hoisted
            // above).  dictValClassDisposer: OwnedDictionary<K,class-or-interface>
            // (ClassOwned row per §28.11 v0.135).
            // dictIsStringValue: borrowing Dictionary<K,string> (RuntimeManaged row).
            llvm::Function* valDisp = dictValClassDisposer;
            if (!valDisp && dictIsStringValue)
                valDisp = stringElementReleaseThunk();
            if (keyTy) {
                if (valDisp) {
                    // OwnedDictionary (or Dict<K,string>) with generic key.
                    auto* fn = declareC("bz_dict_remove_gen_disposing", i1(),
                                        {ptrTy(), ptrTy(), ptrTy()});
                    return builder_.CreateCall(
                        fn, {recv, genKeyPtr(k), llvm::cast<llvm::Value>(valDisp)},
                        "dict.remove");
                }
                auto* fn = declareC("bz_dict_remove_gen", i1(), {ptrTy(), ptrTy()});
                return builder_.CreateCall(fn, {recv, genKeyPtr(k)}, "dict.remove");
            }
            auto [kd, kl] = strDataLen(k);
            if (valDisp) {
                // OwnedDictionary (or Dict<string,string>) with string key.
                auto* fn = declareC("bz_dict_remove_str_disposing", i1(),
                                    {ptrTy(), ptrTy(), i64(), ptrTy()});
                return builder_.CreateCall(
                    fn, {recv, kd, kl, llvm::cast<llvm::Value>(valDisp)},
                    "dict.remove");
            }
            auto* fn = declareC("bz_dict_remove_str", i1(), {ptrTy(), ptrTy(), i64()});
            return builder_.CreateCall(fn, {recv, kd, kl}, "dict.remove");
        }
        // fix #643 / stdlib §4.9: .PopValue(k): owned V — retrieve the owned
        // value for key k, remove the entry WITHOUT disposing the value
        // (ownership transfers to the caller), raise if absent.
        // Unlike Remove(k) which disposes, PopValue transfers the owned V out.
        // Only meaningful on OwnedDictionary<K,V>; the borrowing Dictionary does
        // not expose a PopValue method.
        if (m == "PopValue" && call.args.size() == 1 && call.args[0].value) {
            llvm::Value* k = lowerExpr(*call.args[0].value);
            if (!k) return nullptr;
            // Determine value LLVM type.
            llvm::Type* valLlvm = valTy ? lowerTypeRef(valTy) : ptrTy();
            if (!valLlvm) valLlvm = ptrTy();
            auto* tmpSlot = entryAlloca(ptrTy());
            llvm::Value* found;
            if (keyTy) {
                // #643: bz_dict_pop_value_gen — get-then-remove without disposal.
                auto* fn = declareC("bz_dict_pop_value_gen", i1(),
                                    {ptrTy(), ptrTy(), ptrTy()});
                found = builder_.CreateCall(fn, {recv, genKeyPtr(k), tmpSlot},
                                            "pv.found");
            } else {
                auto [kd2, kl2] = strDataLen(k);
                auto* fn = declareC("bz_dict_pop_value_str", i1(),
                                    {ptrTy(), ptrTy(), i64(), ptrTy()});
                found = builder_.CreateCall(fn, {recv, kd2, kl2, tmpSlot},
                                            "pv.found");
            }
            // Raise InvalidOperationException when key is not found.
            llvm::Value* notFound = builder_.CreateNot(found, "pv.notfound");
            emitGuardThrow(notFound, "InvalidOperationException");
            // Decode the stored void* into the value LLVM type (same as TryGetValue).
            llvm::Value* valPtr = builder_.CreateLoad(ptrTy(), tmpSlot, "pv.vptr");
            if ((valRec || valNullableVal) && valLlvm->isStructTy()) {
                return builder_.CreateLoad(valLlvm, valPtr, "pv.rec");
            } else if (!valLlvm->isPointerTy()) {
                llvm::Value* asI64 = builder_.CreatePtrToInt(valPtr, i64());
                if (valLlvm->isDoubleTy())
                    return builder_.CreateBitCast(asI64, valLlvm);
                if (valLlvm->isFloatTy()) {
                    llvm::Value* trunc = builder_.CreateTrunc(asI64, i32());
                    return builder_.CreateBitCast(trunc, valLlvm);
                }
                if (valLlvm->isIntegerTy(64)) return asI64;
                return builder_.CreateTrunc(asI64, valLlvm);
            }
            return valPtr;  // pointer value (class/string)
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
            if ((valRec || valNullableVal) && outTy && outTy->isStructTy()) {
                // Load the boxed struct (record or nullable-value-type tagged
                // struct) and store it into the out slot.  Guard the box pointer
                // (it is null on a not-found lookup) so we never dereference
                // null: only copy on the found path.
                // issue #539: nullable value types produce a non-null box even
                // for the null? value (hasValue=0 is in the bytes, not the ptr).
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
            // the TryGetValue / indexer-get decode (record or nullable-value-type
            // tagged struct → load the boxed struct; scalar → ptrtoint then
            // truncate/bitcast; pointer → the void* IS the string/object pointer).
            // issue #539: nullable value types (int?, bool?, etc.) lower to a
            // {i8,T} struct and must be unboxed the same way records are.
            // `slotV` is the raw stored void*.
            auto decodeSlot = [&](llvm::Value* slotV) -> llvm::Value* {
                if ((valRec || valNullableVal) && valLlvm->isStructTy())
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

    // ---------------------------------------------------------------------------
    // stdlib §9.3 / §4.4 fix #521: Decimal instance methods.
    // ---------------------------------------------------------------------------
    {
        using P = sema::PrimitiveType;
        auto* recvTy = ctx_.exprType(*ma->target);
        bool isDec = recvTy &&
            recvTy->kind == sema::TypeRef::Kind::Primitive &&
            recvTy->primitive == P::Decimal;
        if (isDec) {
            llvm::StructType* dst = decimalStructType();
            const auto& mname = ma->member;

            llvm::Value* recvVal = lowerExpr(*ma->target);
            if (!recvVal) return nullptr;
            // Store receiver into an alloca for pass-by-ptr calls.
            llvm::AllocaInst* slot = entryAlloca(dst);
            builder_.CreateStore(recvVal, slot);

            // Decimal.ToString(): string — §9.3.
            if (mname == "ToString" && call.args.empty()) {
                auto* fn = declareC("bz_decimal_to_string", ptrTy(), {ptrTy()});
                return builder_.CreateCall(fn, {slot}, "dec.tostr");
            }
            // Decimal.ToString(format: string) — not implemented; fall through to plain ToString.
            if (mname == "ToString" && call.args.size() == 1) {
                auto* fn = declareC("bz_decimal_to_string", ptrTy(), {ptrTy()});
                return builder_.CreateCall(fn, {slot}, "dec.tostr");
            }
            // Decimal.CompareTo(other: decimal): int — §9.3.
            if (mname == "CompareTo" && call.args.size() == 1 && call.args[0].value) {
                llvm::Value* other = lowerExpr(*call.args[0].value);
                if (!other) return nullptr;
                llvm::AllocaInst* oSlot = entryAlloca(dst);
                builder_.CreateStore(other, oSlot);
                auto* fn = declareC("bz_decimal_cmp", i32(), {ptrTy(), ptrTy()});
                return builder_.CreateCall(fn, {slot, oSlot}, "dec.cmp");
            }
            // Decimal.Round(digits: int): decimal — §9.3.
            if (mname == "Round" && call.args.size() == 1 && call.args[0].value) {
                llvm::Value* digits = lowerExpr(*call.args[0].value);
                if (!digits) return nullptr;
                llvm::AllocaInst* oSlot = entryAlloca(dst);
                auto* fn = declareC("bz_decimal_round", llvm::Type::getVoidTy(llvm_),
                                    {ptrTy(), i32(), ptrTy()});
                builder_.CreateCall(fn, {slot, digits, oSlot});
                return builder_.CreateLoad(dst, oSlot, "dec.round");
            }
        }
    }

    // stdlib §2.6: Guid instance methods — handled before the IdentifierExpr
    // guard below because the receiver may be a chained expression (e.g.
    // Guid.Empty.ToString()) whose target is not a bare identifier.
    // ---------------------------------------------------------------------------
    {
        auto* recvTy = ctx_.exprType(*ma->target);
        bool isGuid = recvTy &&
            recvTy->kind == sema::TypeRef::Kind::Named &&
            builtinTypeName(recvTy) == "Guid";
        if (isGuid) {
            llvm::StructType* gst = guidStructType();
            const auto& mname = ma->member;

            llvm::Value* recvVal = lowerExpr(*ma->target);
            if (!recvVal) return nullptr;

            // Ensure we have a ptr to the Guid struct for the runtime call.
            llvm::AllocaInst* slot = entryAlloca(gst);
            if (recvVal->getType()->isPointerTy())
                recvVal = builder_.CreateLoad(gst, recvVal, "guid.load");
            builder_.CreateStore(recvVal, slot);

            // stdlib §2.6: ToString(): string — canonical 36-char dashed form.
            if (mname == "ToString" && call.args.empty()) {
                auto* fn = declareC("bz_guid_to_string", ptrTy(), {ptrTy()});
                return builder_.CreateCall(fn, {slot}, "guid.tostr");
            }

            // stdlib §2.6: Equals(other: Guid): bool — bytewise compare.
            if (mname == "Equals" && call.args.size() == 1 && call.args[0].value) {
                llvm::Value* otherVal = lowerExpr(*call.args[0].value);
                if (!otherVal) return nullptr;
                llvm::AllocaInst* otherSlot = entryAlloca(gst);
                if (otherVal->getType()->isPointerTy())
                    otherVal = builder_.CreateLoad(gst, otherVal, "guid.other.load");
                builder_.CreateStore(otherVal, otherSlot);
                auto* fn = declareC("bz_guid_equals", i32(), {ptrTy(), ptrTy()});
                llvm::Value* r = builder_.CreateCall(fn, {slot, otherSlot}, "guid.eq.r");
                return builder_.CreateICmpNE(r, builder_.getInt32(0), "guid.eq");
            }
        }
    }

    // ---------------------------------------------------------------------------
    // stdlib §5 (#651): IGrouping<TKey, T> instance methods — Count() and Key.
    // IGrouping objects are bz_grouping_t* pointers produced by GroupBy.
    // count method and Key property are both accessed here via builtinTypeName.
    // ---------------------------------------------------------------------------
    {
        auto* igrpRecvTy = ctx_.exprType(*ma->target);
        if (igrpRecvTy && igrpRecvTy->kind == sema::TypeRef::Kind::Named &&
            !igrpRecvTy->declaration &&
            igrpRecvTy->astNode && igrpRecvTy->astNode->name &&
            !igrpRecvTy->astNode->name->parts.empty() &&
            igrpRecvTy->astNode->name->parts[0] == "IGrouping") {
            llvm::Value* grp = lowerExpr(*ma->target);
            if (!grp) return nullptr;
            const auto& mname = ma->member;
            // Count(): int — number of elements in this group.
            if (mname == "Count" && call.args.empty()) {
                auto* fn = declareC("bz_grouping_count", i64(), {ptrTy()});
                llvm::Value* cnt64 = builder_.CreateCall(fn, {grp}, "grp.cnt");
                return builder_.CreateTrunc(cnt64, i32(), "grp.cnt.i32");
            }
        }
    }
    // ---------------------------------------------------------------------------
    // §28.13 / stdlib §2.16 (#603 / #653): Shared<T> and Weak<T> instance
    // methods.  Placed BEFORE the IdentifierExpr guard below because the
    // receiver may be any expression (e.g. nullable!.Clone() where the target
    // is PostfixBangExpr, not a bare identifier).  Uses the same receiver-type
    // check pattern as the Guid block above.
    // ---------------------------------------------------------------------------
    {
        auto* recvTy653 = ctx_.exprType(*ma->target);
        std::string_view btn653 = builtinTypeName(recvTy653);
        if (btn653 == "Shared" || btn653 == "Weak") {
            llvm::Value* handle = lowerExpr(*ma->target);
            if (!handle) return nullptr;
            const auto& mname = ma->member;
            if (btn653 == "Shared") {
                // §28.2 v0.135 / issue #627: checked-disposal guard.
                if (checkedDisposal_ &&
                    (mname == "Clone" || mname == "Get" || mname == "Downgrade")) {
                    llvm::Type* i8Ty = llvm::Type::getInt8Ty(llvm_);
                    auto* isFn = declareC("bz_shared_is_disposed", i8Ty, {ptrTy()});
                    llvm::Value* flag =
                        builder_.CreateCall(isFn, {handle}, "cd.shrdis");
                    llvm::Value* disposed = builder_.CreateICmpNE(
                        flag, llvm::ConstantInt::get(i8Ty, 0), "cd.disposed");
                    emitGuardThrow(disposed, "DisposedAccessException");
                }
                if (mname == "Clone" && call.args.empty()) {
                    auto* fn = declareC("bz_shared_clone", ptrTy(), {ptrTy()});
                    return builder_.CreateCall(fn, {handle}, "shared.clone");
                }
                if (mname == "Get" && call.args.empty()) {
                    auto* fn = declareC("bz_shared_get", ptrTy(), {ptrTy()});
                    return builder_.CreateCall(fn, {handle}, "shared.get");
                }
                if (mname == "Downgrade" && call.args.empty()) {
                    auto* fn = declareC("bz_shared_downgrade", ptrTy(), {ptrTy()});
                    return builder_.CreateCall(fn, {handle}, "shared.downgrade");
                }
                // Dispose() is handled in the explicit-Dispose block; fall through.
            }
            if (btn653 == "Weak") {
                if (mname == "TryUpgrade" && call.args.size() == 1 &&
                    call.args[0].value) {
                    llvm::Type* slotTy = nullptr;
                    llvm::Value* outAddr = lvalueAddr(*call.args[0].value, slotTy);
                    if (!outAddr) return builder_.getFalse();
                    auto* fn = declareC("bz_weak_try_upgrade", i32(),
                                        {ptrTy(), ptrTy()});
                    llvm::Value* r = builder_.CreateCall(fn, {handle, outAddr},
                                                         "weak.tryupgrade");
                    return builder_.CreateICmpNE(r, builder_.getInt32(0),
                                                 "weak.upgraded");
                }
                // Dispose() is handled in the explicit-Dispose block; fall through.
            }
        }
    }

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
            // The null case branches straight to endBB (its PHI edge supplies
            // the null pointer); no separate null block is needed. A
            // previously-created but never-branched-to `env.null` block was left
            // without a terminator, producing malformed IR.
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
                // stdlib §2.2: Sign(x: int|long|double): int
                // Always returns int (-1, 0, or 1), regardless of argument type.
                // For floating-point args use FCmp; for integer args use ICmp.
                // The result is always i32 (Bezel int).
                auto* i32Ty = llvm::Type::getInt32Ty(llvm_);
                auto* zero32 = llvm::ConstantInt::get(i32Ty, 0);
                auto* one32  = llvm::ConstantInt::getSigned(i32Ty, 1);
                auto* neg132 = llvm::ConstantInt::getSigned(i32Ty, -1);
                if (arg->getType()->isFloatingPointTy()) {
                    // FP path: use ordered FCmp so NaN → 0 (neither negative nor positive).
                    auto* zeroFP = llvm::Constant::getNullValue(arg->getType());
                    llvm::Value* isNeg = builder_.CreateFCmpOLT(arg, zeroFP, "sgn.neg");
                    llvm::Value* isPos = builder_.CreateFCmpOGT(arg, zeroFP, "sgn.pos");
                    llvm::Value* s1 = builder_.CreateSelect(isPos, one32, zero32, "sgn.1");
                    return builder_.CreateSelect(isNeg, neg132, s1, "sgn.result");
                }
                // Integer path (int or long): compute in arg's native width then trunc/sext to i32.
                llvm::Value* zero = llvm::ConstantInt::get(arg->getType(), 0);
                llvm::Value* isNeg = builder_.CreateICmpSLT(arg, zero, "sgn.neg");
                llvm::Value* isPos = builder_.CreateICmpSGT(arg, zero, "sgn.pos");
                llvm::Value* one = llvm::ConstantInt::get(arg->getType(), 1);
                llvm::Value* negOne = llvm::ConstantInt::getSigned(arg->getType(), -1);
                llvm::Value* s1 = builder_.CreateSelect(isPos, one, zero, "sgn.1");
                llvm::Value* sRes = builder_.CreateSelect(isNeg, negOne, s1, "sgn.pre");
                // Normalise to i32: no-op for i32 arg, sext/trunc for i64 (long).
                if (sRes->getType() != i32Ty)
                    sRes = builder_.CreateSExtOrTrunc(sRes, i32Ty, "sgn.result");
                return sRes;
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

    // §6.1 / §30.8: CancellationToken static properties.
    // CancellationToken.None is the "never-cancels" sentinel (null ptr).
    if (cls == "CancellationToken") {
        if (m == "None" && call.args.empty()) {
            // CancellationToken.None = null pointer (bz_cancel_state_t* == null).
            return llvm::ConstantPointerNull::get(ptrTy());
        }
    }
    // §6.1: CancellationTokenSource static methods.
    if (cls == "CancellationTokenSource") {
        // WithCancelAfter(delay: TimeSpan): owned CancellationTokenSource.
        // Allocates a new CancellationTokenSource via bz_cts_alloc (the same
        // allocation as `new CancellationTokenSource()`).  The timer that
        // triggers cancellation after `delay` is DEFERRED pending scheduler
        // infra (bz_task_delay already schedules work on the thread pool;
        // CancelAfter needs an analogous bz_cts_cancel_after helper).  Until
        // that helper exists, the source is allocated normally and no
        // cancellation is scheduled — programs compile and run cleanly with
        // well-formed IR; the timer simply does not fire.  (#463: the bug was
        // that the source was never even allocated because sema gave the call
        // expression no type, which caused IR verification failure downstream.)
        if (m == "WithCancelAfter" && !call.args.empty()) {
            // Evaluate the delay argument for any side-effects (TimeSpan.FromMilliseconds
            // etc.) even though we do not yet pass it to a timer.
            if (call.args[0].value) lowerExpr(*call.args[0].value);
            auto* allocFn = declareC("bz_cts_alloc", ptrTy(), {});
            return builder_.CreateCall(allocFn, {}, "cts.wca");
        }
    }

    // §30.10 / stdlib §6.7: Task static helpers. `Task.Delay(...)` produces a
    // genuinely-incomplete Task that a thread-pool worker completes after the
    // delay elapses (bz_task_delay). This is the primitive that exercises the
    // cross-thread suspend/resume path (§30.13): `await Task.Delay(...)` registers
    // its continuation via bz_task_then and resumes on the pool when the timer
    // fires. The Delay(int) overload's arg is milliseconds; Delay(TimeSpan)'s arg
    // is an i64 tick count (100-ns units) converted to ms.
    //
    // stdlib §6.7 (line 2868) / spec §30.8: Delay(ms, token) and Delay(span, token)
    // overloads now honor the CancellationToken: if the token is already cancelled
    // at call time OR becomes cancelled during the sleep, the returned task is
    // faulted with OperationCanceledException (bz_task_delay_cancelable, #451).
    // Mid-delay polling checks the token after the sleep elapses; sub-millisecond
    // interrupt is a deferred refinement (future increment).
    if (cls == "Task") {
        if (m == "Delay" && !call.args.empty() && call.args[0].value) {
            llvm::Value* arg = lowerExpr(*call.args[0].value);
            if (!arg) return nullptr;
            // Determine ms. An `int` (i32) arg is milliseconds directly; a
            // TimeSpan arg is an i64 tick count (100-ns units) → ms = ticks/10000.
            llvm::Value* ms = nullptr;
            auto* argTy = ctx_.exprType(*call.args[0].value);
            bool isTimeSpan = argTy && argTy->kind == sema::TypeRef::Kind::Named &&
                              builtinTypeName(argTy) == "TimeSpan";
            if (isTimeSpan) {
                if (!arg->getType()->isIntegerTy(64))
                    arg = builder_.CreateSExtOrTrunc(arg, i64());
                ms = builder_.CreateSDiv(arg, builder_.getInt64(10000),
                                         "delay.ms");
            } else {
                ms = arg->getType()->isIntegerTy(64)
                         ? arg
                         : builder_.CreateSExtOrTrunc(arg, i64());
            }
            // Token-aware overloads: Delay(ms, token) and Delay(span, token).
            // If a second argument is provided and it is a CancellationToken
            // (not None — the None sentinel is a null pointer and disables
            // cancellation checks), dispatch to bz_task_delay_cancelable.
            if (call.args.size() >= 2 && call.args[1].value) {
                llvm::Value* tokArg = lowerExpr(*call.args[1].value);
                if (tokArg) {
                    // Retrieve the OperationCanceledException vtable for the
                    // runtime to construct the OCE object if cancelled.
                    llvm::Value* oceVtable =
                        llvm::ConstantPointerNull::get(ptrTy());  // fallback
                    auto vit = builtinVtable_.find("OperationCanceledException");
                    if (vit != builtinVtable_.end())
                        oceVtable = builder_.CreateBitCast(vit->second, ptrTy());
                    auto* delayFn = declareC("bz_task_delay_cancelable",
                                             ptrTy(), {i64(), ptrTy(), ptrTy()});
                    return builder_.CreateCall(delayFn,
                        {ms, tokArg, oceVtable}, "task.delay.cancel");
                }
            }
            auto* delayFn = declareC("bz_task_delay", ptrTy(), {i64()});
            return builder_.CreateCall(delayFn, {ms}, "task.delay");
        }
        // stdlib §6.7 (line 2855): Task.FromResult<T>(value: T): Task<T>.
        // Allocate a task and immediately complete it with the argument value.
        // The value is stored through a stack alloca so we have a pointer to pass
        // to bz_task_from_result. ptr-result types (class objects, task pointers)
        // are stored as i64 sizeof(void*); integer types are stored at their
        // natural width. Per the doc, this handles non-class T (value types, int,
        // bool, etc.) only — OwnedTask<T> for class T is FromOwnedResult (deferred).
        if (m == "FromResult" && !call.args.empty() && call.args[0].value) {
            llvm::Value* val = lowerExpr(*call.args[0].value);
            if (!val) return nullptr;
            llvm::Type* valTy = val->getType();
            // Store the value to a stack slot so we can pass a pointer.
            llvm::AllocaInst* slot = builder_.CreateAlloca(valTy, nullptr,
                                                            "from_result.slot");
            builder_.CreateStore(val, slot);
            int64_t sz = static_cast<int64_t>(
                module_->getDataLayout().getTypeAllocSize(valTy));
            auto* fromResultFn = declareC("bz_task_from_result",
                                          ptrTy(), {ptrTy(), i64()});
            return builder_.CreateCall(fromResultFn,
                {builder_.CreateBitCast(slot, ptrTy()),
                 builder_.getInt64(sz)},
                "task.from_result");
        }
        // stdlib §6.7 (line 2872): Task.WhenAll(tasks: Array<Task>): Task.
        // stdlib §6.7 (line 2875): Task.WhenAll<T>(tasks: Array<Task<T>>): Task<Array<T>>.
        //
        // Dispatch: if the arg is Array<Task<T>> (the element type is Task<T>,
        // i.e. Task with a generic arg), call bz_task_when_all_results which
        // collects each task's T-typed result into an Array<T> and completes
        // the aggregate with the array pointer. Otherwise (Array<Task>, no
        // generic arg on Task), call the plain bz_task_when_all.
        if (m == "WhenAll" && !call.args.empty() && call.args[0].value) {
            llvm::Value* arr = lowerExpr(*call.args[0].value);
            if (!arr) return nullptr;
            // Check if the arg is Array<Task<T>> (generic result variant).
            // arrTy is the sema TypeRef for the argument expression.
            auto* arrTy = ctx_.exprType(*call.args[0].value);
            // arrTy should be Named("Array", [Named("Task", [T])]).
            // If Task<T> has a non-empty genericArgs, we use the typed variant.
            bool isTyped = false;
            int64_t elemSz = 8;
            if (arrTy && arrTy->kind == sema::TypeRef::Kind::Named &&
                !arrTy->genericArgs.empty() && arrTy->genericArgs[0]) {
                auto* elemTaskTy = arrTy->genericArgs[0];
                // elemTaskTy should be Task<T>.
                auto elemName = builtinTypeName(elemTaskTy);
                if ((elemName == "Task" || elemName == "OwnedTask") &&
                    !elemTaskTy->genericArgs.empty() &&
                    elemTaskTy->genericArgs[0]) {
                    // T is elemTaskTy->genericArgs[0].
                    auto* T = elemTaskTy->genericArgs[0];
                    llvm::Type* tLlvmTy = lowerTypeRef(T);
                    if (tLlvmTy) {
                        elemSz = static_cast<int64_t>(
                            module_->getDataLayout().getTypeAllocSize(tLlvmTy));
                    }
                    isTyped = true;
                }
            }
            if (isTyped) {
                // Task.WhenAll<T>: collect results into Array<T>.
                auto* whenAllResFn = declareC("bz_task_when_all_results",
                                              ptrTy(), {ptrTy(), i64()});
                return builder_.CreateCall(whenAllResFn,
                    {arr, builder_.getInt64(elemSz)},
                    "task.when_all_results");
            } else {
                auto* whenAllFn = declareC("bz_task_when_all", ptrTy(), {ptrTy()});
                return builder_.CreateCall(whenAllFn, {arr}, "task.when_all");
            }
        }
        // stdlib §6.7 (line 2884): Task.WhenAny(tasks: Array<Task>): Task<Task>.
        // Returns a new task that completes (with a ptr-sized result — the first-
        // done task pointer) as soon as any one input task is done.
        if (m == "WhenAny" && !call.args.empty() && call.args[0].value) {
            llvm::Value* arr = lowerExpr(*call.args[0].value);
            if (!arr) return nullptr;
            auto* whenAnyFn = declareC("bz_task_when_any", ptrTy(), {ptrTy()});
            return builder_.CreateCall(whenAnyFn, {arr}, "task.when_any");
        }
        // stdlib §6.7 (line 2942): Task.WhenAllOwned<T>(
        //   owned tasks: OwnedArray<OwnedTask<T>>): OwnedTask<OwnedArray<T>>
        //   where T: class.
        //
        // Pattern B aggregator for class T. The caller transfers ownership of
        // the OwnedArray<OwnedTask<T>>; bz_task_when_all_owned collects each
        // OwnedTask<T> result (a pointer) into a new OwnedArray<T> and frees
        // each input task and the input array. Returns OwnedTask<OwnedArray<T>>
        // (a bz_task_t* whose result is an OwnedArray<T> pointer).
        if (m == "WhenAllOwned" && !call.args.empty() && call.args[0].value) {
            llvm::Value* arr = lowerExpr(*call.args[0].value);
            if (!arr) return nullptr;
            auto* whenAllOwnedFn = declareC("bz_task_when_all_owned",
                                            ptrTy(), {ptrTy()});
            return builder_.CreateCall(whenAllOwnedFn, {arr},
                                       "task.when_all_owned");
        }
        // stdlib §6.7 (line 2885): Task.FireAndForget(owned task: Task): void.
        // Registers a continuation that frees the task on completion; ownership
        // transfers. Returns unit (nullptr from codegen → discarded).
        if (m == "FireAndForget" && !call.args.empty() && call.args[0].value) {
            llvm::Value* task = lowerExpr(*call.args[0].value);
            if (!task) return nullptr;
            auto* fafFn = declareC("bz_task_fire_and_forget",
                                    llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(fafFn, {task});
            return nullptr;  // unit
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
    // stdlib §9.3 / §4.4 fix #521: Decimal — static members and methods.
    // Decimal is a 128-bit value type stored as { i32 lo32, i32 mid32, i32 hi32, i32 flags }.
    // All runtime functions take a pointer to the struct.
    // ---------------------------------------------------------------------------
    if (cls == "Decimal") {
        llvm::StructType* dst = decimalStructType();

        // Helper: allocate a decimal-sized alloca slot.
        auto makeDecSlot = [&]() -> llvm::AllocaInst* {
            return entryAlloca(dst);
        };

        // Helper: build a decimal struct from compile-time parse of a cstr.
        auto decConstant = [&](const char* s) -> llvm::Value* {
            uint32_t dec[4] = {0, 0, 0, 0};
            bz_decimal_parse_cstr(s, dec);
            return llvm::ConstantStruct::get(dst, {
                llvm::ConstantInt::get(i32(), dec[0]),
                llvm::ConstantInt::get(i32(), dec[1]),
                llvm::ConstantInt::get(i32(), dec[2]),
                llvm::ConstantInt::get(i32(), dec[3])
            });
        };

        // Decimal.MinValue — .NET = -79228162514264337593543950335 (max 96-bit, negative).
        if (m == "MinValue" && call.args.empty())
            return decConstant("-79228162514264337593543950335");
        // Decimal.MaxValue — .NET = 79228162514264337593543950335.
        if (m == "MaxValue" && call.args.empty())
            return decConstant("79228162514264337593543950335");
        // Decimal.Zero — 0.
        if (m == "Zero" && call.args.empty())
            return llvm::ConstantAggregateZero::get(dst);
        // Decimal.One — 1.
        if (m == "One" && call.args.empty())
            return decConstant("1");

        // Decimal.Parse(s: string): decimal — throws FormatException on failure.
        if (m == "Parse" && call.args.size() == 1 && call.args[0].value) {
            llvm::Value* strVal = lowerExpr(*call.args[0].value);
            if (!strVal) return nullptr;
            llvm::AllocaInst* slot = makeDecSlot();
            auto* fn = declareC("bz_decimal_parse", llvm::Type::getVoidTy(llvm_),
                                {ptrTy(), ptrTy()});
            builder_.CreateCall(fn, {strVal, slot});
            return builder_.CreateLoad(dst, slot, "dec.parsed");
        }

        // Decimal.TryParse(s: string, out result: decimal): bool
        if (m == "TryParse" && call.args.size() == 2 &&
            call.args[0].value && call.args[1].value) {
            llvm::Value* strVal = lowerExpr(*call.args[0].value);
            if (!strVal) return nullptr;
            llvm::AllocaInst* slot = makeDecSlot();
            auto* fn = declareC("bz_decimal_try_parse", i32(),
                                {ptrTy(), ptrTy()});
            llvm::Value* ok = builder_.CreateCall(fn, {strVal, slot}, "dec.tryp.ok");
            // Write the result into the out parameter.
            llvm::Type* et = nullptr;
            llvm::Value* outAddr = lvalueAddr(*call.args[1].value, et);
            if (outAddr) {
                llvm::Value* result = builder_.CreateLoad(dst, slot, "dec.tryp.val");
                builder_.CreateStore(result, outAddr);
            }
            return builder_.CreateICmpNE(ok, builder_.getInt32(0), "dec.tryp.bool");
        }
    }

    // ---------------------------------------------------------------------------
    // stdlib §2.6: Guid — static members and methods via bz_guid_* runtime.
    // Guid is a 128-bit value type stored as { i64 lo, i64 hi }.  All runtime
    // functions take a bz_guid_t* (ptr) so the value is passed by reference.
    // ---------------------------------------------------------------------------
    if (cls == "Guid") {
        llvm::StructType* gst = guidStructType();

        // Helper: allocate a Guid-sized alloca slot and return its address (ptr).
        auto makeGuidSlot = [&]() -> llvm::AllocaInst* {
            return entryAlloca(gst);
        };

        // Guid.Empty — the all-zero Guid (static readonly property §2.6).
        // Returns a zero-initialised { i64 0, i64 0 } struct value.
        if (m == "Empty" && call.args.empty()) {
            llvm::AllocaInst* slot = makeGuidSlot();
            builder_.CreateStore(llvm::ConstantAggregateZero::get(gst), slot);
            return builder_.CreateLoad(gst, slot, "guid.empty");
        }

        // Guid.NewGuid() — fill a Guid from OS entropy (RFC 4122 v4).
        if (m == "NewGuid" && call.args.empty()) {
            llvm::AllocaInst* slot = makeGuidSlot();
            auto* fn = declareC("bz_guid_new", llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(fn, {slot});
            return builder_.CreateLoad(gst, slot, "guid.new");
        }

        // Guid.Parse(s: string): Guid — parse canonical form; trap on failure.
        if (m == "Parse" && call.args.size() == 1 && call.args[0].value) {
            llvm::Value* strVal = lowerExpr(*call.args[0].value);
            if (!strVal) return nullptr;
            llvm::AllocaInst* slot = makeGuidSlot();
            auto* fn = declareC("bz_guid_parse", i32(), {ptrTy(), ptrTy()});
            llvm::Value* ok = builder_.CreateCall(fn, {strVal, slot}, "guid.parse.ok");
            // Trap (unreachable) on parse failure — the spec makes Parse throw;
            // until exception support for this path is in, trap is the safe choice.
            llvm::Value* isOk = builder_.CreateICmpNE(ok, builder_.getInt32(0), "guid.parse.chk");
            auto* okBB   = llvm::BasicBlock::Create(llvm_, "guid.parse.ok", currentFn_);
            auto* badBB  = llvm::BasicBlock::Create(llvm_, "guid.parse.bad", currentFn_);
            builder_.CreateCondBr(isOk, okBB, badBB);
            builder_.SetInsertPoint(badBB);
            builder_.CreateUnreachable();
            builder_.SetInsertPoint(okBB);
            return builder_.CreateLoad(gst, slot, "guid.parsed");
        }

        // Guid.TryParse(s: string, out result: Guid): bool
        if (m == "TryParse" && call.args.size() == 2 &&
            call.args[0].value && call.args[1].value) {
            llvm::Value* strVal = lowerExpr(*call.args[0].value);
            if (!strVal) return nullptr;
            llvm::Type* et = nullptr;
            llvm::Value* outAddr = lvalueAddr(*call.args[1].value, et);
            if (!outAddr) return nullptr;
            // If the out slot is not a guid-struct slot (different elem type),
            // use our own slot and copy.  Normally outAddr is already the right size.
            auto* fn = declareC("bz_guid_try_parse", i32(), {ptrTy(), ptrTy()});
            llvm::Value* ok = builder_.CreateCall(fn, {strVal, outAddr}, "guid.tryp.ok");
            return builder_.CreateICmpNE(ok, builder_.getInt32(0), "guid.tryp.bool");
        }
    }

    // ---------------------------------------------------------------------------
    // §2.5 DateTime — static members and methods via bz_datetime_* runtime.
    // DateTime is a scalar i64 tick count (100-ns since 0001-01-01 UTC).
    // ---------------------------------------------------------------------------
    if (cls == "DateTime") {
        // Static properties: Now / UtcNow / Today.
        if (m == "Now" && call.args.empty()) {
            auto* fn = declareC("bz_datetime_now_ticks", i64(), {});
            return builder_.CreateCall(fn, {}, "dt.now");
        }
        if (m == "UtcNow" && call.args.empty()) {
            auto* fn = declareC("bz_datetime_utcnow_ticks", i64(), {});
            return builder_.CreateCall(fn, {}, "dt.utcnow");
        }
        // Today = date part of Now (zero the time components).
        if (m == "Today" && call.args.empty()) {
            auto* fn = declareC("bz_datetime_now_ticks", i64(), {});
            llvm::Value* t = builder_.CreateCall(fn, {}, "dt.today.ticks");
            // Truncate to day boundary: t / TICKS_PER_DAY * TICKS_PER_DAY.
            llvm::Value* tpd = builder_.getInt64(864000000000LL);
            llvm::Value* d = builder_.CreateSDiv(t, tpd);
            return builder_.CreateMul(d, tpd, "dt.today");
        }
        // MinValue / MaxValue.
        if (m == "MinValue" && call.args.empty())
            return builder_.getInt64(0LL);
        if (m == "MaxValue" && call.args.empty())
            return builder_.getInt64(3155378975999999999LL);
        // DateTime.FromTicks(ticks: long): DateTime (identity)
        if (m == "FromTicks" && call.args.size() == 1 && call.args[0].value) {
            return lowerExpr(*call.args[0].value);
        }
        // per §2.5 / #648: DateTime.Parse(s: string): DateTime
        // Calls bz_datetime_parse(str_p) → i64 ticks.  The codegen emits a
        // guard that traps (unreachable) on parse failure — same pattern as
        // Guid.Parse.  bz_datetime_try_parse returns 1 on success, 0 on failure.
        if (m == "Parse" && call.args.size() == 1 && call.args[0].value) {
            llvm::Value* strVal = lowerExpr(*call.args[0].value);
            if (!strVal) return nullptr;
            // Call bz_datetime_try_parse(str_p, &out_ticks) to validate+get ticks.
            auto* ticksSlot = entryAlloca(i64());
            auto* tryFn = declareC("bz_datetime_try_parse", i32(),
                                   {ptrTy(), ptrTy()});
            llvm::Value* ok = builder_.CreateCall(tryFn, {strVal, ticksSlot},
                                                  "dt.parse.ok");
            llvm::Value* isOk = builder_.CreateICmpNE(ok, builder_.getInt32(0));
            auto* okBB  = llvm::BasicBlock::Create(llvm_, "dt.parse.ok", currentFn_);
            auto* badBB = llvm::BasicBlock::Create(llvm_, "dt.parse.bad", currentFn_);
            builder_.CreateCondBr(isOk, okBB, badBB);
            builder_.SetInsertPoint(badBB);
            builder_.CreateUnreachable();
            builder_.SetInsertPoint(okBB);
            return builder_.CreateLoad(i64(), ticksSlot, "dt.parsed");
        }
        // per §2.5 / #648: DateTime.TryParse(s: string, out result: DateTime): bool
        if (m == "TryParse" && call.args.size() == 2 &&
            call.args[0].value && call.args[1].value) {
            llvm::Value* strVal = lowerExpr(*call.args[0].value);
            if (!strVal) return nullptr;
            llvm::Type* et = nullptr;
            llvm::Value* outAddr = lvalueAddr(*call.args[1].value, et);
            if (!outAddr) return nullptr;
            auto* tryFn = declareC("bz_datetime_try_parse", i32(),
                                   {ptrTy(), ptrTy()});
            llvm::Value* ok = builder_.CreateCall(tryFn, {strVal, outAddr},
                                                  "dt.tryp.ok");
            return builder_.CreateICmpNE(ok, builder_.getInt32(0), "dt.tryp.bool");
        }
    }
    // ---------------------------------------------------------------------------
    // §2.5 TimeSpan — static factories and Zero constant.
    // TimeSpan is a scalar i64 tick count (signed, 100-ns units).
    // ---------------------------------------------------------------------------
    if (cls == "TimeSpan") {
        // TimeSpan.Zero — the zero-duration constant.
        if (m == "Zero" && call.args.empty())
            return builder_.getInt64(0LL);
        // Static factory methods: From* convert a double quantity to ticks.
        // Tick ratios: per §2.5 / bz_runtime constants.
        //   TICKS_PER_MS      = 10_000
        //   TICKS_PER_SECOND  = 10_000_000
        //   TICKS_PER_MINUTE  = 600_000_000
        //   TICKS_PER_HOUR    = 36_000_000_000
        //   TICKS_PER_DAY     = 864_000_000_000
        auto fromDouble = [&](double ratio) -> llvm::Value* {
            if (call.args.empty() || !call.args[0].value) return nullptr;
            llvm::Value* v = lowerExpr(*call.args[0].value);
            if (!v) return nullptr;
            // Promote int arguments to double.
            if (v->getType()->isIntegerTy())
                v = builder_.CreateSIToFP(v, llvm::Type::getDoubleTy(llvm_));
            else if (!v->getType()->isDoubleTy() && v->getType()->isFloatTy())
                v = builder_.CreateFPExt(v, llvm::Type::getDoubleTy(llvm_));
            llvm::Value* r = llvm::ConstantFP::get(llvm::Type::getDoubleTy(llvm_), ratio);
            llvm::Value* prod = builder_.CreateFMul(v, r, "ts.product");
            return builder_.CreateFPToSI(prod, i64(), "ts.ticks");
        };
        if (m == "FromDays")         return fromDouble(864000000000.0);
        if (m == "FromHours")        return fromDouble(36000000000.0);
        if (m == "FromMinutes")      return fromDouble(600000000.0);
        if (m == "FromSeconds")      return fromDouble(10000000.0);
        if (m == "FromMilliseconds") return fromDouble(10000.0);
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

    // ---------------------------------------------------------------------------
    // §6.1 / §30.8: CancellationTokenSource instance methods and properties.
    // The receiver's exprType name-part is "CancellationTokenSource".
    // ---------------------------------------------------------------------------
    {
        if (auto* ma2 = dynamic_cast<const ast::MemberAccessExpr*>(call.callee.get())) {
            if (ma2->target) {
                auto* recvTy = ctx_.exprType(*ma2->target);
                bool isCTS = recvTy && recvTy->kind == sema::TypeRef::Kind::Named &&
                    builtinTypeName(recvTy) == "CancellationTokenSource";
                if (isCTS) {
                    llvm::Value* cts = lowerExpr(*ma2->target);
                    if (!cts) return nullptr;
                    const auto& mname = ma2->member;
                    // §6.1: Token property — returns the CancellationToken handle.
                    if (mname == "Token" && call.args.empty()) {
                        auto* fn = declareC("bz_cts_token", ptrTy(), {ptrTy()});
                        return builder_.CreateCall(fn, {cts}, "cts.token");
                    }
                    // §6.1: IsCancellationRequested property (convenience mirror).
                    if (mname == "IsCancellationRequested" && call.args.empty()) {
                        auto* fn = declareC("bz_cts_is_cancelled", i32(), {ptrTy()});
                        llvm::Value* flag = builder_.CreateCall(fn, {cts}, "cts.iscancelled");
                        return builder_.CreateICmpNE(flag, builder_.getInt32(0), "cts.icr");
                    }
                    // §6.1: Cancel() — sets the cancellation flag.
                    if (mname == "Cancel" && call.args.empty()) {
                        auto* fn = declareC("bz_cts_cancel",
                            llvm::Type::getVoidTy(llvm_), {ptrTy()});
                        builder_.CreateCall(fn, {cts});
                        return nullptr;  // unit
                    }
                    // §6.1: CancelAfter / WithCancelAfter — deferred (needs timer).
                }
            }
        }
    }

    // ---------------------------------------------------------------------------
    // §6.1 / §30.8: CancellationToken instance methods and properties.
    // The receiver's exprType name-part is "CancellationToken".
    // CancellationToken.None is handled in tryLowerStaticCall (member of type name).
    // ---------------------------------------------------------------------------
    {
        if (auto* ma2 = dynamic_cast<const ast::MemberAccessExpr*>(call.callee.get())) {
            if (ma2->target) {
                auto* recvTy = ctx_.exprType(*ma2->target);
                bool isCT = recvTy && recvTy->kind == sema::TypeRef::Kind::Named &&
                    builtinTypeName(recvTy) == "CancellationToken";
                if (isCT) {
                    llvm::Value* tok = lowerExpr(*ma2->target);
                    const auto& mname = ma2->member;
                    // §6.1: IsCancellationRequested property.
                    if (mname == "IsCancellationRequested" && call.args.empty()) {
                        if (!tok) return builder_.getFalse();
                        auto* fn = declareC("bz_ctoken_is_cancelled", i32(), {ptrTy()});
                        llvm::Value* flag = builder_.CreateCall(fn, {tok}, "ct.iscancelled");
                        return builder_.CreateICmpNE(flag, builder_.getInt32(0), "ct.icr");
                    }
                    // §6.1: ThrowIfCancellationRequested() — if cancelled, construct
                    // and throw an OperationCanceledException.  We call the runtime
                    // predicate (bz_ctoken_throw_if_cancelled → i32) and then emit a
                    // conditional block that, on true, executes the exception throw.
                    // This mirrors the index-out-of-range guard (tryLowerIndexExpr).
                    if (mname == "ThrowIfCancellationRequested" && call.args.empty()) {
                        if (!tok) return nullptr;  // None — never cancels, no-op
                        auto* fn = declareC("bz_ctoken_throw_if_cancelled",
                                            i32(), {ptrTy()});
                        llvm::Value* flag = builder_.CreateCall(fn, {tok}, "ct.tfcr");
                        llvm::Value* isCancelled =
                            builder_.CreateICmpNE(flag, builder_.getInt32(0));
                        auto* throwBB  = llvm::BasicBlock::Create(llvm_, "ct.throw",
                                                                   currentFn_);
                        auto* resumeBB = llvm::BasicBlock::Create(llvm_, "ct.resume",
                                                                   currentFn_);
                        builder_.CreateCondBr(isCancelled, throwBB, resumeBB);

                        // throwBB: construct OperationCanceledException and throw.
                        // emitThrowValue terminates the block (branches to unwindBB),
                        // so no explicit terminator is needed for throwBB afterward.
                        builder_.SetInsertPoint(throwBB);
                        {
                            auto vit = builtinVtable_.find("OperationCanceledException");
                            if (vit != builtinVtable_.end()) {
                                llvm::Value* obj = builder_.CreateCall(
                                    callocFn(),
                                    {builder_.getInt64(1), builder_.getInt64(16)},
                                    "oce.obj");
                                builder_.CreateStore(vit->second, obj);
                                emitThrowValue(obj);
                                // emitThrowValue ends with CreateBr(unwindBB)
                                // — throwBB is now terminated.
                            } else {
                                // Fallback: branch to resumeBB (no vtable available).
                                builder_.CreateBr(resumeBB);
                            }
                        }

                        builder_.SetInsertPoint(resumeBB);
                        return nullptr;  // unit
                    }
                    // §6.1: Register(cb) — deferred.  Needs closure-list infra
                    // (a per-source list of registered lambdas fired on Cancel()).
                    // Report: not implemented.
                }
            }
        }
    }

    // ---------------------------------------------------------------------------
    // §2.5: DateTime instance methods — receiver is a DateTime (i64 ticks).
    // Handles Add*, Subtract, ToUtc, ToLocal (where receiver type is DateTime).
    // ---------------------------------------------------------------------------
    {
        if (auto* ma2 = dynamic_cast<const ast::MemberAccessExpr*>(call.callee.get())) {
            if (ma2->target) {
                auto* recvTy = ctx_.exprType(*ma2->target);
                bool isDT = recvTy && builtinTypeName(recvTy) == "DateTime";
                if (isDT) {
                    llvm::Value* ticks = lowerExpr(*ma2->target);
                    if (!ticks) return nullptr;
                    const auto& mname = ma2->member;

                    // Helper: get first arg as i64.
                    auto arg0i64 = [&]() -> llvm::Value* {
                        if (call.args.empty() || !call.args[0].value) return nullptr;
                        llvm::Value* v = lowerExpr(*call.args[0].value);
                        if (!v) return nullptr;
                        if (!v->getType()->isIntegerTy(64)) v = builder_.CreateSExt(v, i64());
                        return v;
                    };
                    // Helper: get first arg as double.
                    auto arg0dbl = [&]() -> llvm::Value* {
                        if (call.args.empty() || !call.args[0].value) return nullptr;
                        llvm::Value* v = lowerExpr(*call.args[0].value);
                        if (!v) return nullptr;
                        auto* dbl = llvm::Type::getDoubleTy(llvm_);
                        if (v->getType()->isIntegerTy())
                            v = builder_.CreateSIToFP(v, dbl);
                        else if (v->getType()->isFloatTy())
                            v = builder_.CreateFPExt(v, dbl);
                        return v;
                    };
                    // Helper: add a double quantity (in ticks) to ticks.
                    auto addDoubleTicks = [&](double ratio) -> llvm::Value* {
                        llvm::Value* qty = arg0dbl();
                        if (!qty) return nullptr;
                        auto* dbl = llvm::Type::getDoubleTy(llvm_);
                        llvm::Value* r = llvm::ConstantFP::get(dbl, ratio);
                        llvm::Value* delta = builder_.CreateFPToSI(
                            builder_.CreateFMul(qty, r), i64(), "dt.delta");
                        return builder_.CreateAdd(ticks, delta, "dt.add");
                    };

                    // §2.5: DateTime.Add(span: TimeSpan): DateTime
                    if (mname == "Add" && call.args.size() == 1 && call.args[0].value) {
                        llvm::Value* span = arg0i64();
                        if (!span) return nullptr;
                        return builder_.CreateAdd(ticks, span, "dt.addspan");
                    }
                    // §2.5: AddDays/Hours/Minutes/Seconds/Milliseconds(double): DateTime
                    if (mname == "AddDays")         return addDoubleTicks(864000000000.0);
                    if (mname == "AddHours")        return addDoubleTicks(36000000000.0);
                    if (mname == "AddMinutes")      return addDoubleTicks(600000000.0);
                    if (mname == "AddSeconds")      return addDoubleTicks(10000000.0);
                    if (mname == "AddMilliseconds") return addDoubleTicks(10000.0);

                    // §2.5: AddYears(years: int): DateTime — calendar-aware.
                    if (mname == "AddYears" && !call.args.empty() && call.args[0].value) {
                        llvm::Value* y = lowerExpr(*call.args[0].value);
                        if (!y) return nullptr;
                        if (!y->getType()->isIntegerTy(32)) y = builder_.CreateTrunc(y, i32());
                        auto* fn = declareC("bz_datetime_add_years", i64(), {i64(), i32()});
                        return builder_.CreateCall(fn, {ticks, y}, "dt.addyears");
                    }
                    // §2.5: AddMonths(months: int): DateTime — calendar-aware.
                    if (mname == "AddMonths" && !call.args.empty() && call.args[0].value) {
                        llvm::Value* mo = lowerExpr(*call.args[0].value);
                        if (!mo) return nullptr;
                        if (!mo->getType()->isIntegerTy(32)) mo = builder_.CreateTrunc(mo, i32());
                        auto* fn = declareC("bz_datetime_add_months", i64(), {i64(), i32()});
                        return builder_.CreateCall(fn, {ticks, mo}, "dt.addmonths");
                    }
                    // §2.5: Subtract(other: DateTime): TimeSpan
                    if (mname == "Subtract" && call.args.size() == 1 && call.args[0].value) {
                        auto* argTy = ctx_.exprType(*call.args[0].value);
                        bool otherIsSpan = argTy && builtinTypeName(argTy) == "TimeSpan";
                        llvm::Value* arg = arg0i64();
                        if (!arg) return nullptr;
                        // Subtract(span: TimeSpan): DateTime
                        if (otherIsSpan)
                            return builder_.CreateSub(ticks, arg, "dt.subspan");
                        // Subtract(other: DateTime): TimeSpan
                        return builder_.CreateSub(ticks, arg, "dt.subdt");
                    }
                    // §2.5: ToUtc() / ToLocal() — identity for now.
                    // The spec defines Kind (Utc/Local/Unspecified) but the runtime
                    // stores UTC ticks only; Kind is not persisted in the scalar.
                    // Deferred: full timezone conversion needs platform TZ APIs.
                    if (mname == "ToUtc" || mname == "ToLocal")
                        return ticks;
                    // per §2.5 / #648: ToString(format: string): string —
                    // expand format specifiers via bz_datetime_to_string_fmt.
                    if (mname == "ToString" && call.args.size() == 1 &&
                        call.args[0].value) {
                        llvm::Value* fmtStr = lowerExpr(*call.args[0].value);
                        if (!fmtStr) return nullptr;
                        auto* fn = declareC("bz_datetime_to_string_fmt", ptrTy(),
                                            {i64(), ptrTy()});
                        return builder_.CreateCall(fn, {ticks, fmtStr},
                                                   "dt.tostr.fmt");
                    }
                }
            }
        }
    }

    // ---------------------------------------------------------------------------
    // §2.5: TimeSpan arithmetic operators (instance methods encoded as calls).
    // The TimeSpan+TimeSpan, TimeSpan-TimeSpan, TimeSpan*double forms are
    // lowered here when the receiver is a TimeSpan value.
    // Note: binary operators (+,-,*) between TimeSpan values are handled by
    // lowerBinary's user-operator path; however the codegen may also encounter
    // them as method calls if sema resolved them that way.  For safety, handle
    // both paths for TimeSpan arithmetic: instance-call form here, and the
    // binary expression form is pure i64 add/sub/mul (handled in lowerBinary
    // via the fallthrough — since these are NOT user-defined operators in Bezel
    // source, they are implemented via the builtin binary path below).
    // ---------------------------------------------------------------------------

    // ---------------------------------------------------------------------------
    // §28.13 / stdlib §2.16 (#603): Shared<T> and Weak<T> instance methods.
    // Receiver is identified by builtinTypeName "Shared" or "Weak".
    // Shared<T> methods: Clone() → bz_shared_clone; Get() → bz_shared_get;
    //   Downgrade() → bz_shared_downgrade; Dispose() → bz_shared_release.
    // Weak<T> methods: TryUpgrade(out using var h) → bz_weak_try_upgrade;
    //   Dispose() → bz_weak_release.
    // NOTE: explicit Dispose() on these types is intercepted in lowerCall's
    // Dispose() block (around the CTS Dispose arm) — see below.
    // ---------------------------------------------------------------------------
    {
        if (auto* ma2 = dynamic_cast<const ast::MemberAccessExpr*>(call.callee.get())) {
            if (ma2->target) {
                auto* recvTy = ctx_.exprType(*ma2->target);
                std::string_view btn = builtinTypeName(recvTy);
                // ── Shared<T> instance methods ──────────────────────────────
                if (btn == "Shared") {
                    llvm::Value* handle = lowerExpr(*ma2->target);
                    if (!handle) return nullptr;
                    const auto& mname = ma2->member;
                    // §28.2 v0.135 / issue #627: checked-disposal guard for
                    // Shared<T> instance methods (Clone/Get/Downgrade).
                    // bz_shared_is_disposed returns non-zero when the handle's
                    // strong count is 0 (i.e. it was already Dispose()d).
                    // Zero default-mode cost — the test is absent in unchecked builds.
                    if (checkedDisposal_ &&
                        (mname == "Clone" || mname == "Get" ||
                         mname == "Downgrade")) {
                        llvm::Type* i8Ty = llvm::Type::getInt8Ty(llvm_);
                        auto* isFn = declareC("bz_shared_is_disposed", i8Ty,
                                              {ptrTy()});
                        llvm::Value* flag =
                            builder_.CreateCall(isFn, {handle}, "cd.shrdis");
                        llvm::Value* disposed = builder_.CreateICmpNE(
                            flag, llvm::ConstantInt::get(i8Ty, 0), "cd.disposed");
                        emitGuardThrow(disposed, "DisposedAccessException");
                    }
                    // Clone(): Shared<T> — increments strong count, returns new handle.
                    if (mname == "Clone" && call.args.empty()) {
                        auto* fn = declareC("bz_shared_clone", ptrTy(), {ptrTy()});
                        return builder_.CreateCall(fn, {handle}, "shared.clone");
                    }
                    // Get(): T — returns raw pointer to the owned value (borrow).
                    if (mname == "Get" && call.args.empty()) {
                        auto* fn = declareC("bz_shared_get", ptrTy(), {ptrTy()});
                        return builder_.CreateCall(fn, {handle}, "shared.get");
                    }
                    // Downgrade(): Weak<T> — returns a weak handle to the same ctrl block.
                    if (mname == "Downgrade" && call.args.empty()) {
                        auto* fn = declareC("bz_shared_downgrade", ptrTy(), {ptrTy()});
                        return builder_.CreateCall(fn, {handle}, "shared.downgrade");
                    }
                    // Dispose() is handled in the explicit-Dispose block; fall through.
                }
                // ── Weak<T> instance methods ────────────────────────────────
                if (btn == "Weak") {
                    llvm::Value* handle = lowerExpr(*ma2->target);
                    if (!handle) return nullptr;
                    const auto& mname = ma2->member;
                    // TryUpgrade(out using var s: Shared<T>): bool
                    // bz_weak_try_upgrade(weak_handle, &out_slot) → int (1=success).
                    // The out-slot was pre-created in ensureOutVarSlots.
                    if (mname == "TryUpgrade" && call.args.size() == 1 &&
                        call.args[0].value) {
                        llvm::Type* slotTy = nullptr;
                        llvm::Value* outAddr = lvalueAddr(*call.args[0].value,
                                                          slotTy);
                        if (!outAddr) return builder_.getFalse();
                        auto* fn = declareC("bz_weak_try_upgrade", i32(),
                                            {ptrTy(), ptrTy()});
                        llvm::Value* r = builder_.CreateCall(fn, {handle, outAddr},
                                                             "weak.tryupgrade");
                        return builder_.CreateICmpNE(r, builder_.getInt32(0),
                                                     "weak.upgraded");
                    }
                    // Dispose() is handled in the explicit-Dispose block; fall through.
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
        // The null case branches straight to endBB (its PHI edge supplies
        // `true`); no separate null block is needed. A previously-created but
        // never-branched-to `ine.null` block was left without a terminator,
        // producing malformed IR.
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

    // stdlib §9.2: double / float static FP predicates — all return bool (i1).
    // These lower to inline fcmp sequences; no runtime helper needed.
    // IsNaN            → fcmp uno %x, %x
    // IsInfinity       → (fcmp oeq %x, +inf) | (fcmp oeq %x, -inf)
    // IsPositiveInfinity → fcmp oeq %x, +inf
    // IsNegativeInfinity → fcmp oeq %x, -inf
    // IsFinite         → !(IsNaN | IsInfinity)
    if (call.args.size() == 1 && call.args[0].value &&
        (pt == P::Double || pt == P::Float) &&
        (m == "IsNaN" || m == "IsInfinity" || m == "IsPositiveInfinity" ||
         m == "IsNegativeInfinity" || m == "IsFinite")) {
        llvm::Value* arg = lowerExpr(*call.args[0].value);
        if (!arg) return nullptr;
        // Widen float to double only when the argument arrives as a narrower
        // float type; if the value is already the correct FP width, use it
        // directly.  IsNaN/IsFinite work on any FP width; the infinity checks
        // need a same-width constant, so we use arg->getType() consistently.
        auto* fty = arg->getType();
        // If the arg is an integer (e.g. widened from an int literal 0) or
        // some unexpected type, promote to double before comparing.
        if (!fty->isFloatingPointTy()) {
            fty = llvm::Type::getDoubleTy(llvm_);
            arg = builder_.CreateSIToFP(arg, fty);
        }
        if (m == "IsNaN") {
            // stdlib §9.2: IsNaN — unordered self-compare yields i1.
            return builder_.CreateFCmpUNO(arg, arg, "isnan");
        }
        if (m == "IsInfinity") {
            // stdlib §9.2: true for both +∞ and -∞.
            auto* posInf = llvm::ConstantFP::getInfinity(fty, false);
            auto* negInf = llvm::ConstantFP::getInfinity(fty, true);
            return builder_.CreateOr(
                builder_.CreateFCmpOEQ(arg, posInf),
                builder_.CreateFCmpOEQ(arg, negInf), "isinf");
        }
        if (m == "IsPositiveInfinity") {
            // stdlib §9.2: true only for +∞.
            auto* posInf = llvm::ConstantFP::getInfinity(fty, false);
            return builder_.CreateFCmpOEQ(arg, posInf, "isposinf");
        }
        if (m == "IsNegativeInfinity") {
            // stdlib §9.2: true only for -∞.
            auto* negInf = llvm::ConstantFP::getInfinity(fty, true);
            return builder_.CreateFCmpOEQ(arg, negInf, "isneginf");
        }
        // IsFinite: not NaN and not infinity (per §9.2).
        llvm::Value* isNaN = builder_.CreateFCmpUNO(arg, arg);
        auto* posInf = llvm::ConstantFP::getInfinity(fty, false);
        auto* negInf = llvm::ConstantFP::getInfinity(fty, true);
        llvm::Value* isInf = builder_.CreateOr(
            builder_.CreateFCmpOEQ(arg, posInf),
            builder_.CreateFCmpOEQ(arg, negInf));
        return builder_.CreateNot(builder_.CreateOr(isNaN, isInf), "isfinite");
    }

    // stdlib §9.2 (#519): double / float IeeeEquals / BitwiseEquals —
    // two-arg static equality variants; both return bool (i1).
    //
    //   IeeeEquals(a, b)    → fcmp oeq a, b
    //     (ordered equal: NaN op anything → false, including NaN-vs-NaN)
    //   BitwiseEquals(a, b) → bitcast a/b to i64/i32, then icmp eq
    //     (distinguishes +0.0 from -0.0 and distinct NaN payloads)
    if (call.args.size() == 2 && call.args[0].value && call.args[1].value &&
        (pt == P::Double || pt == P::Float) &&
        (m == "IeeeEquals" || m == "BitwiseEquals")) {
        llvm::Value* a = lowerExpr(*call.args[0].value);
        llvm::Value* b = lowerExpr(*call.args[1].value);
        if (!a || !b) return nullptr;
        // Ensure both are the correct FP type (promote ints from literal 0).
        auto* fty = (pt == P::Float)
            ? llvm::Type::getFloatTy(llvm_)
            : llvm::Type::getDoubleTy(llvm_);
        if (!a->getType()->isFloatingPointTy())
            a = builder_.CreateSIToFP(a, fty, "ieee.a.fp");
        if (!b->getType()->isFloatingPointTy())
            b = builder_.CreateSIToFP(b, fty, "ieee.b.fp");
        if (m == "IeeeEquals") {
            // stdlib §9.2: fcmp oeq — ordered equal → false for any NaN.
            return builder_.CreateFCmpOEQ(a, b, "ieee.eq");
        }
        // BitwiseEquals: bitcast both to matching-width integer, icmp eq.
        llvm::Type* intTy = (pt == P::Float) ? i32() : i64();
        llvm::Value* ai = builder_.CreateBitCast(a, intTy, "bw.ai");
        llvm::Value* bi = builder_.CreateBitCast(b, intTy, "bw.bi");
        return builder_.CreateICmpEQ(ai, bi, "bw.eq");
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
        // §28.13 / stdlib §2.16 (#603): Weak<T>.TryUpgrade(out using var h).
        // The modifier is "out using var" — pre-create a null-initialized ptr
        // slot so lvalueAddr finds it. Register a sharedHandle disposal so the
        // handle is released at scope end: if TryUpgrade succeeded the slot
        // holds a valid Shared<T> handle; if it failed the slot is null and
        // emitDisposals' null-guard skips the release (no-op).
        if (a.modifier == "out using var" && a.value) {
            auto* id2 = dynamic_cast<const ast::IdentifierExpr*>(a.value.get());
            if (id2) {
                sema::Symbol* sym2 = ctx_.exprSymbol(*id2);
                auto* loc2 = dynamic_cast<sema::LocalSymbol*>(sym2);
                bool alreadyMapped = (loc2 && loc2->decl &&
                                      locals_.count(loc2->decl)) ||
                                     locals_.count(id2);
                if (!alreadyMapped) {
                    llvm::AllocaInst* outSlot = entryAlloca(ptrTy());
                    // Zero-initialize: null → upgrade did not happen (no-op on release).
                    builder_.CreateStore(
                        llvm::ConstantPointerNull::get(ptrTy()), outSlot);
                    locals_[id2] = outSlot;
                    if (loc2 && loc2->decl) locals_[loc2->decl] = outSlot;
                    // Register a sharedHandle disposal so bz_shared_release is
                    // called at scope end (null-guarded by emitDisposals).
                    if (!disposeStack_.empty()) {
                        Disposal disp{outSlot, nullptr};
                        disp.sharedHandle = true;
                        disposeStack_.back().push_back(disp);
                    }
                }
            }
            pos++;
            continue;
        }
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

    // issue #462 / §14.1: for a GENERIC record receiver (`Box<string>.ToString()`
    // / `.GetHashCode()` / `.Equals(...)`) install the receiver's concrete type
    // args so recordStruct returns the SPECIALIZED struct (the right size to load
    // a `ptr`/8-byte field — the i32 placeholder would truncate it) AND the
    // synthesized members resolve each `T`-typed field to its concrete type (a
    // string field compares/formats/hashes by CONTENT). A no-op for a non-generic
    // record. The receiver value was lowered above under the outer subst with the
    // specialized shape already (construction is specialized since #460).
    RecordSubstGuard valSubst(this, recvTy);

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

    // §24.1 / §22.13 (#477 B6): a STATIC event has no per-instance slot — its
    // backing EventSource<T> is a program-wide global, allocated in Main's
    // prologue. Load it directly (no receiver).
    if (auto sit = staticEventGlobals_.find(es);
        sit != staticEventGlobals_.end())
        return builder_.CreateLoad(ptrTy(), sit->second, "static.event.src");

    // §24.3 / §24.7 (#477 B7): the event was resolved on an INTERFACE that
    // declares it (`i.Ready` where `i: ISource`). The interface has no backing
    // field — the EventSource<T> lives in the implementing OBJECT's struct, at
    // a class-specific offset unknown statically through the interface. Dispatch
    // through the object's itable (exactly as an interface METHOD call, §13):
    // each implementing class contributes an accessor `ptr get(this)` returning
    // its backing source; the itab slot for this event holds that accessor.
    auto idxIt = fieldIndex_.find(es);
    if (idxIt == fieldIndex_.end() &&
        es->parent->kind == ast::TypeDeclKind::Interface) {
        auto kIt = ifaceEventIndex_.find(es);
        auto dIt = descArray_.find(es->parent);
        if (kIt == ifaceEventIndex_.end() || dIt == descArray_.end())
            return nullptr;
        llvm::Value* recv = nullptr;
        if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
            if (ma->target) recv = lowerExpr(*ma->target);
        }
        if (!recv) return nullptr;
        // vptr[1] = itmap$C; bz_itable_lookup(itmap, td$I) → itab$C$I;
        // accessor = itab$C$I[k]; src = accessor(recv).
        auto* lookupFn = declareC("bz_itable_lookup", ptrTy(),
                                  {ptrTy(), ptrTy()});
        llvm::Value* vptr = builder_.CreateLoad(ptrTy(), recv);
        llvm::Value* itmapPtr = builder_.CreateLoad(
            ptrTy(), builder_.CreateGEP(ptrTy(), vptr,
                                        {builder_.getInt32(1)}));
        llvm::Value* itab = builder_.CreateCall(
            llvm::FunctionType::get(ptrTy(), {ptrTy(), ptrTy()}, false),
            lookupFn, {itmapPtr, dIt->second}, "itab");
        llvm::Value* accFn = builder_.CreateLoad(
            ptrTy(),
            builder_.CreateGEP(ptrTy(), itab,
                               {builder_.getInt32(kIt->second)}),
            "evt.acc");
        auto* accTy = llvm::FunctionType::get(ptrTy(), {ptrTy()}, false);
        return builder_.CreateCall(accTy, accFn, {recv}, "event.src");
    }
    if (idxIt == fieldIndex_.end()) return nullptr;
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
        // §24.3: a handler returns unit. Set funcReturn so lowerLambda resolves
        // a block-bodied handler's return type to void — without this it
        // defaulted to i32, defining the handler as i32(...) while the Raise
        // loop calls it through a void(...) function pointer (an indirect-call
        // type mismatch / malformed IR: `ret void` in an i32 function).
        auto* unitRet = ctx_.makeTypeRef();
        unitRet->kind = sema::TypeRef::Kind::Unit;
        handlerTy.funcReturn = unitRet;
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
    // §9.8 / §2.14–2.15 (issue #550): BinarySerializer/Deserializer construction,
    // ToBytes, AsReadOnlySpan, and synthesized Serialize/Deserialize on record/enum
    // receivers.  tryLowerSerializationCall returns a Value* for value-returning
    // calls (Deserialize, ToBytes, AsReadOnlySpan) and nullptr for void calls
    // (Serialize, Dispose) and for unrecognised calls. We use isSerializationCall()
    // to distinguish "handled void" from "not handled".
    if (call.callee) {
        if (auto* sma = dynamic_cast<const ast::MemberAccessExpr*>(
                call.callee.get())) {
            if (isSerializationCall(call, *sma)) {
                llvm::Value* sv = tryLowerSerializationCall(call);
                return sv;  // nullptr for void or unrecognised (both are fine here)
            }
        }
    }
    // §8.2 (#487): `v.GetType()` — universal Object member returning the `Type`
    // runtime class (stdlib §2.13). Sema types the CallExpr as Named Type, so
    // the expression is well-typed. Codegen defers the implementation (the
    // runtime type-descriptor table is not yet emitted); intercepted here to
    // emit a clean error instead of silently returning garbage or crashing.
    if (call.args.empty() && call.callee) {
        if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(
                call.callee.get())) {
            if (ma->member == "GetType") {
                ctx_.diagnostics().report(
                    Severity::Error, call.span.begin,
                    "`GetType()` is not yet implemented in codegen: the runtime "
                    "`Type` class (stdlib \xC2\xA7""2.13) requires a type-descriptor "
                    "table that is not yet emitted (\xC2\xA7""8.2, #487). This call "
                    "will be supported in a future milestone.");
                return llvm::PoisonValue::get(ptrTy());
            }
        }
    }
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
    // per §4.6/§4.7: LinkedList<T>/OwnedLinkedList<T> node-based API.
    if (auto* v = tryLowerLinkedListInstanceCall(call)) return v;
    // §4.15 (#549): ArraySpan<T>/ReadOnlyArraySpan<T> instance methods and
    // Array<T>.AsSpan()/AsReadOnlySpan() construction.
    if (auto* v = tryLowerArraySpanCall(call)) return v;
    // §3: stdlib collection instance methods: arr.Add, arr.Get, dict.TryGetValue, etc.
    if (auto* v = tryLowerCollectionInstanceCall(call)) return v;
    // stdlib §3.2: Encoding.Utf8.GetBytes(s) / Encoding.Utf8.GetString(bytes).
    if (auto* v = tryLowerEncodingCall(call)) return v;
    // §30.14/§30.15 (#600 phase c): Channel<T>/ChannelSender<T> methods + Domain.Spawn.
    if (auto* v = tryLowerChannelAndDomainCall(call)) return v;
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
    // §10 / #482 fix B: also accept a NestedTypeSymbol whose ->type is an
    // enum — a nested enum `Outer.Inner(i)` reaches here as a NestedTypeSymbol;
    // NestedTypeSymbol does NOT inherit TypeSymbol, so the dynamic_cast below
    // would miss it without the extra branch.
    if (auto* nts = dynamic_cast<sema::NestedTypeSymbol*>(calleeSym)) {
        if (nts->type && nts->type->kind == ast::TypeDeclKind::Enum)
            return lowerEnumCast(call);
    }
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
            // per §32 / issue #496: when the user's extern declaration was
            // remapped to an existing internal function (e.g. malloc(i32)
            // collides with the internal malloc(i64)), coerce each argument
            // to the canonical parameter type so the call is well-typed.
            // Only integer width adjustments and int/ptr conversions are
            // needed: the ABI-relevant case is int (i32) vs. size_t (i64).
            if (idx < it->second->getFunctionType()->getNumParams()) {
                llvm::Type* want = it->second->getFunctionType()
                                       ->getParamType(idx);
                llvm::Type* have = v->getType();
                if (have != want) {
                    bool haveInt = have->isIntegerTy();
                    bool wantInt = want->isIntegerTy();
                    bool havePtr = have->isPointerTy();
                    bool wantPtr = want->isPointerTy();
                    if (haveInt && wantInt) {
                        unsigned hw = have->getIntegerBitWidth();
                        unsigned ww = want->getIntegerBitWidth();
                        v = (hw < ww)
                                ? builder_.CreateZExt(v, want, "ffi.zext")
                                : builder_.CreateTrunc(v, want, "ffi.trunc");
                    } else if (haveInt && wantPtr) {
                        v = builder_.CreateIntToPtr(v, want, "ffi.i2p");
                    } else if (havePtr && wantInt) {
                        v = builder_.CreatePtrToInt(v, want, "ffi.p2i");
                    }
                    // ptr→ptr is identity in opaque-pointer LLVM (no bitcast).
                }
            }
            args.push_back(v);
            ++idx;
        }
        if (args.size() != it->second->arg_size()) return nullptr;
        // per §32 / issue #496: coerce the return value if the canonical
        // function returns a different type than the user's extern declared.
        // E.g. malloc returns ptr but user declared IntPtr (i64): ptrtoint.
        llvm::Value* result = builder_.CreateCall(it->second, args);
        llvm::Type* canonRet = it->second->getReturnType();
        llvm::Type* userRet = ed && ed->returnType
                                  ? lowerType(ed->returnType.get())
                                  : llvm::Type::getVoidTy(llvm_);
        if (canonRet != userRet && !canonRet->isVoidTy() &&
            !userRet->isVoidTy()) {
            bool canonInt = canonRet->isIntegerTy();
            bool userInt  = userRet->isIntegerTy();
            bool canonPtr = canonRet->isPointerTy();
            bool userPtr  = userRet->isPointerTy();
            if (canonPtr && userInt) {
                result = builder_.CreatePtrToInt(result, userRet, "ffi.rp2i");
            } else if (canonInt && userPtr) {
                result = builder_.CreateIntToPtr(result, userRet, "ffi.ri2p");
            } else if (canonInt && userInt) {
                unsigned cw = canonRet->getIntegerBitWidth();
                unsigned uw = userRet->getIntegerBitWidth();
                result = (cw < uw)
                             ? builder_.CreateZExt(result, userRet, "ffi.rzext")
                             : builder_.CreateTrunc(result, userRet, "ffi.rtrunc");
            }
        }
        return result;
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
            // per §7.5 / §22.1: if the declared parameter is a nullable value
            // type (T?) and the actual argument is the bare T (a plain scalar
            // or record value), promote it to {i8=1, T} — the same promotion
            // that regular (non-closure) call sites perform via the CG-nullable
            // block in lowerCallArgs (LlvmBackend_Assign.cpp ~L1673).
            // pi = ai - 1 maps from the args slot (which has +1 for env_ptr)
            // back to the funcParams index.
            {
                std::size_t pi = ai - 1;
                if (pi < rt->funcParams.size() && rt->funcParams[pi].type) {
                    // funcParams[pi].type is already a resolved sema::TypeRef*.
                    const sema::TypeRef* formalTy = rt->funcParams[pi].type;
                    if (isNullableValueTy(formalTy)) {
                        auto* expectedSt = nullableValueStructTy(formalTy);
                        if (args[ai]->getType() != expectedSt) {
                            if (args[ai]->getType()->isPointerTy() &&
                                llvm::isa<llvm::ConstantPointerNull>(
                                    args[ai])) {
                                args[ai] = nullableValueNull(formalTy);
                            } else if (args[ai]->getType() ==
                                       expectedSt->getElementType(1)) {
                                args[ai] = wrapNullableValue(formalTy,
                                                             args[ai]);
                            }
                        }
                        continue;  // type is now correct; skip numeric coercions
                    }
                }
            }
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
    // fix #507 / §27.3: IIFE — the callee is a LambdaExpr invoked directly at
    // its construction site: `((x) => x * 2)(5)`.  The sema TypeRef for the
    // lambda has null funcReturn when the lambda's params are unannotated (the
    // param `x` has no sema type, so the binary body `x * 2` also has no sema
    // type, so funcReturn stays null).  The generic `!calleeSym` path below
    // would call emitClosureCall with a null funcReturn → void return type →
    // miscompile/SIGILL when the result is used.
    //
    // Fix: detect LambdaExpr callee early, lower the lambda's arguments
    // directly to LLVM values, compute the actual function type from the
    // lowered argument types + the best-effort return type (consulting the
    // lambda's own sema exprBody type, with i32 fallback matching lowerLambda),
    // then emit the closure call using that concrete function type.  This
    // mirrors the closure-call path for variable-stored lambdas but derives the
    // function type from the call-site argument values rather than the TypeRef,
    // which is always sound for the argument types.
    if (auto* iifeLam =
            dynamic_cast<const ast::LambdaExpr*>(call.callee.get())) {
        // Lower the lambda to a closure struct { fn_ptr, env_ptr, rc }.
        llvm::Value* closureObj = lowerLambda(*iifeLam);
        if (!closureObj) return nullptr;
        // Load fn_ptr from closure[0] and env_ptr from closure[1].
        llvm::Value* fnPtr = builder_.CreateLoad(ptrTy(), closureObj,
                                                 "iife.fn");
        llvm::Value* envSlot = builder_.CreateGEP(
            ptrTy(), closureObj, {builder_.getInt32(1)});
        llvm::Value* envPtr = builder_.CreateLoad(ptrTy(), envSlot,
                                                  "iife.env");
        // Lower call arguments.
        std::vector<llvm::Value*> args;
        args.push_back(envPtr);  // env_ptr is always the first arg
        for (const auto& a : call.args) {
            if (!a.value) return nullptr;
            llvm::Value* v = lowerExpr(*a.value);
            if (!v) return nullptr;
            args.push_back(v);
        }
        // Build the function type from the actual argument LLVM types.
        // Return type: consult the sema exprType of the expression body
        // (same logic as lowerLambda), falling back to i32 when null.
        // This matches what lowerLambda emits for the actual LLVM function.
        llvm::Type* retTy = i32();
        if (iifeLam->exprBody) {
            if (auto* rt = ctx_.exprType(*iifeLam->exprBody))
                retTy = lowerTypeRef(rt);
        } else if (auto* lamTy = ctx_.exprType(*iifeLam)) {
            // Block-body lambda: pull return type from the lambda's own
            // sema exprType (set by ExprTypecheck_Exprs.cpp).
            if (lamTy->kind == sema::TypeRef::Kind::Function &&
                lamTy->funcReturn)
                retTy = lowerTypeRef(lamTy->funcReturn);
        }
        std::vector<llvm::Type*> argTys;
        for (auto* v : args) argTys.push_back(v->getType());
        auto* fnTy = llvm::FunctionType::get(retTy, argTys, false);
        llvm::Value* result = builder_.CreateCall(fnTy, fnPtr, args,
                                                  "iife.ret");
        // #422 / §28.9: release the transient closure after the call.
        emitClosureRelease(closureObj);
        return retTy->isVoidTy() ? nullptr : result;
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
    // #608 / §8.5: `e.ToString()` on ANY user-class or built-in exception
    // receiver dispatches through vtable slot 3 (classToStringThunk).
    //
    // classToStringThunk handles both the "no override" and "has override"
    // cases:
    //   • no user override → returns the class's qualified type name (§8.5 default)
    //   • user override  → calls the override function body directly (the thunk
    //     holds a direct non-virtual reference to it, so the thunk is not
    //     re-entered, and the virtual override slot 4 is bypassed).
    //
    // We intercept HERE (before the ms-based emitMethodCall path below) for ALL
    // user-class and builtin-exception receivers, regardless of whether sema
    // resolved a MethodSymbol for the override.  Routing through the virtual
    // override slot 4 instead requires methodFnType(ms) to succeed; for user
    // exception classes that inherit from a builtin base, the builtin base is
    // absent from TypeSymbol::baseTypes (no TypeSymbol for it), which can cause
    // vtable-layout anomalies that make methodFnType return nullptr, causing
    // emitMethodCall to silently return nullptr and the call-site to emit 0.
    // Slot 3 avoids that hazard entirely.  per §8.5 / §12 (v0.134).
    //
    // Guard for generic contexts: a Named TypeRef with declaration==nullptr can
    // also be a type parameter (T, U, …) inside a generic body.  Those are
    // handled by the genericSubstSemaTy_ block in the !ms path below; skip the
    // early interception for them by requiring either (a) a non-null declaration
    // (concrete user class) or (b) a known builtin exception name while NOT
    // inside a generic substitution (no type-param ambiguity).
    if (auto* cg_ma = dynamic_cast<const ast::MemberAccessExpr*>(
            call.callee.get())) {
        if (cg_ma->member == "ToString" && cg_ma->target &&
                call.args.empty()) {
            const sema::TypeRef* cg_recvTy =
                ctx_.exprType(*cg_ma->target);
            if (cg_recvTy) {
                using CG_K = sema::TypeRef::Kind;
                bool cg_isClass =
                    cg_recvTy->kind == CG_K::Named &&
                    cg_recvTy->declaration &&
                    cg_recvTy->declaration->kind ==
                        ast::TypeDeclKind::Class;
                // Built-in exception types: Named with no declaration.
                // Only intercept when not in a generic context (to avoid
                // confusing a type-parameter T with a builtin exception name).
                bool cg_isBuiltinEx =
                    genericSubstSemaTy_.empty() &&
                    cg_recvTy->kind == CG_K::Named &&
                    !cg_recvTy->declaration;
                // Route through lowerObjectToString (vtable slot 3,
                // classToStringThunk) for ALL ToString calls on user-class and
                // builtin-exception receivers, regardless of whether sema
                // resolved a MethodSymbol (override) or not.
                //
                // classToStringThunk (slot 3) already handles both cases:
                //   • no user override → returns the class's qualified type name
                //   • user override → calls the override body directly (the thunk
                //     holds a direct reference to the override function, so it is
                //     not dispatched via the virtual slot 4 and cannot loop).
                //
                // Routing through the virtual override slot (slot 4, assigned by
                // buildVtable when `override method ToString()` exists) is
                // semantically equivalent BUT requires methodFnType(ms) to
                // succeed; for user exception classes that inherit from a builtin
                // base, methodFnType can return nullptr due to a vtable-layout
                // anomaly (no TypeSymbol base), causing emitMethodCall to return
                // nullptr and the call-site to silently emit 0.  Slot 3 avoids
                // that hazard entirely.  per §8.5 / §12 (v0.134).
                if (cg_isClass || cg_isBuiltinEx) {
                    llvm::Value* cg_recv = lowerExpr(*cg_ma->target);
                    if (llvm::Value* cg_r = lowerObjectToString(cg_recv))
                        return cg_r;
                }
            }
        }
    }
    auto* ms = dynamic_cast<sema::MethodSymbol*>(calleeSym);
    if (!ms) {
        // §8.5 / §8.6 (#417): `o.ToString()` where o: Object / interface (the
        // universal Object.ToString, no MethodSymbol — the receiver's static
        // type has no `declaration`). Dispatch through the uniform vtable
        // ToString slot; a string operand (no vtable) returns itself. Gated on
        // an Object- or interface-typed receiver so a record/enum/primitive
        // receiver (handled by tryLowerValueSemanticsCall above) and a typed
        // class receiver (resolved to a real MethodSymbol) never reach here.
        if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(
                call.callee.get())) {
            if (ma->member == "ToString" && ma->target && call.args.empty()) {
                const sema::TypeRef* recvTy = ctx_.exprType(*ma->target);
                using K = sema::TypeRef::Kind;
                using P = sema::PrimitiveType;
                // per §14.1 / §8.5 (issue #515): inside a generic method body
                // that has been monomorphized to a value type (e.g. U=int),
                // the receiver `v` has sema type `U` (Named, no declaration).
                // sema sets no MethodSymbol for `v.ToString()` (the notnull
                // constraint provides no type-surface member lookup). Without
                // this guard the call falls through and returns nullptr →
                // emitReturn emits `ret ptr null` for the string return →
                // SIGSEGV at runtime on first dereference. Fix: resolve the
                // receiver's type-param name through genericSubstSemaTy_; if
                // the concrete bound type is a primitive, lower the receiver
                // as that primitive and call emitPrimitiveToString directly —
                // the same path `(42).ToString()` takes in lowerExpr.
                if (!genericSubstSemaTy_.empty() &&
                    recvTy && recvTy->kind == K::Named &&
                    !recvTy->declaration &&
                    recvTy->astNode && recvTy->astNode->name &&
                    recvTy->astNode->name->parts.size() == 1) {
                    std::string_view paramName =
                        recvTy->astNode->name->parts[0];
                    auto sit = genericSubstSemaTy_.find(paramName);
                    if (sit != genericSubstSemaTy_.end() && sit->second) {
                        const sema::TypeRef* concreteTy = sit->second;
                        if (concreteTy->kind == K::Primitive &&
                            concreteTy->primitive != P::Object &&
                            concreteTy->primitive != P::None) {
                            // Value type (int/bool/double/…): use the
                            // primitive ToString path.
                            llvm::Value* recv = lowerExpr(*ma->target);
                            if (recv)
                                return emitPrimitiveToString(
                                    recv, concreteTy->primitive);
                        } else if (concreteTy->kind == K::Named &&
                                   concreteTy->declaration &&
                                   (concreteTy->declaration->kind ==
                                        ast::TypeDeclKind::Class ||
                                    concreteTy->declaration->kind ==
                                        ast::TypeDeclKind::Interface)) {
                            // Reference type (class / interface): dispatch
                            // through the uniform vtable ToString slot —
                            // same path as `o: Object` / `o: IFoo`.
                            llvm::Value* recv = lowerExpr(*ma->target);
                            if (recv) {
                                if (llvm::Value* r = lowerObjectToString(recv))
                                    return r;
                            }
                        }
                    }
                }
                bool isObjectRecv =
                    recvTy && recvTy->kind == K::Primitive &&
                    recvTy->primitive == P::Object;
                bool isIfaceRecv =
                    recvTy && recvTy->kind == K::Named && recvTy->declaration &&
                    recvTy->declaration->kind == ast::TypeDeclKind::Interface;
                // #608 / §8.5 / §12: user-defined class types (including user
                // exception classes that inherit from a built-in exception base)
                // also dispatch ToString through the uniform vtable slot 3.
                // When no MethodSymbol exists (ToString not in TypeSymbol::baseTypes
                // for the built-in exception base), the call reaches here with
                // ms==nullptr. Extend the dispatch to include named Class receivers
                // so they use the same lowerObjectToString path as Object/interface.
                // per §8.5 (v0.134).
                bool isNamedClassRecv =
                    recvTy && recvTy->kind == K::Named && recvTy->declaration &&
                    recvTy->declaration->kind == ast::TypeDeclKind::Class;
                // #608 / §8.5 / §31: built-in exception types (Named with no
                // declaration) in a GENERIC context (genericSubstSemaTy_ active)
                // can reach here when the type-param substitution lookup above
                // did not handle them (e.g. a bare `Exception`-typed variable in
                // a generic body).  Dispatch through lowerObjectToString.
                // per §8.5 (v0.134).
                bool isBuiltinExRecv =
                    recvTy && recvTy->kind == K::Named && !recvTy->declaration;
                if (isObjectRecv || isIfaceRecv || isNamedClassRecv ||
                        isBuiltinExRecv) {
                    llvm::Value* recv = lowerExpr(*ma->target);
                    if (llvm::Value* r = lowerObjectToString(recv)) return r;
                }
            }
        }
        // issue #494 / §29.2 / §29.3: explicit `e.MoveNext()` call where `e` is
        // typed as the builtin IEnumerator<T> or OwnedEnumerator<T> (no sema
        // TypeDecl, hence no MethodSymbol for MoveNext). The iterator state
        // struct's vtable layout is [0]=td, [1]=Dispose, [2]=MoveNext(→i1),
        // [3]=Current(→T) — the same layout emitted by emitIteratorMethod.
        // Dispatch directly through vtable slot 2, returning i1 (bool), so that
        // an inferred-var slot `var moved := e.MoveNext()` is typed i1 and a
        // subsequent `if moved then` produces valid IR (not `br i32`).
        if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(
                call.callee.get())) {
            if (ma->member == "MoveNext" && ma->target && call.args.empty()) {
                const sema::TypeRef* recvTy = ctx_.exprType(*ma->target);
                bool isBuiltinEnumerator =
                    recvTy &&
                    recvTy->kind == sema::TypeRef::Kind::Named &&
                    !recvTy->declaration &&
                    recvTy->astNode && recvTy->astNode->name &&
                    !recvTy->astNode->name->parts.empty() &&
                    (recvTy->astNode->name->parts[0] == "IEnumerator" ||
                     recvTy->astNode->name->parts[0] == "OwnedEnumerator");
                if (isBuiltinEnumerator) {
                    llvm::Value* recv = lowerExpr(*ma->target);
                    if (recv && recv->getType()->isPointerTy()) {
                        llvm::Value* vtable = builder_.CreateLoad(ptrTy(), recv);
                        auto* moveNextTy =
                            llvm::FunctionType::get(i1(), {ptrTy()}, false);
                        llvm::Value* mnSlotPtr = builder_.CreateGEP(
                            ptrTy(), vtable, {builder_.getInt32(2)});
                        llvm::Value* mnFnPtr =
                            builder_.CreateLoad(ptrTy(), mnSlotPtr);
                        llvm::Value* r = builder_.CreateCall(
                            moveNextTy, mnFnPtr, {recv}, "mn");
                        emitPendingCheck();  // CG20: callee may have thrown
                        return r;
                    }
                }
            }
        }
        // CG13/CG18: explicit `obj.Dispose()` call where Dispose is a
        // compiler-generated method (no MethodSymbol in sema). Emit a
        // dynamic vtable dispatch to the object's Dispose function (slot 1).
        if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(
                call.callee.get())) {
            if (ma->member == "Dispose" && ma->target) {
                // per §6.8 / #652: Domain instance Dispose() — release the
                // bz_domain_t ref acquired by bz_domain_alloc. Calls
                // bz_domain_dispose(dom) which calls domain_release().
                {
                    auto* recvTy = ctx_.exprType(*ma->target);
                    if (recvTy && builtinTypeName(recvTy) == "Domain") {
                        llvm::Value* dom = lowerExpr(*ma->target);
                        if (dom && dom->getType()->isPointerTy()) {
                            auto* fn = declareC("bz_domain_dispose",
                                llvm::Type::getVoidTy(llvm_), {ptrTy()});
                            builder_.CreateCall(fn, {dom});
                            if (!checkedDisposal_) {
                                llvm::Type* slotTy = nullptr;
                                if (llvm::Value* slot =
                                        lvalueAddr(*ma->target, slotTy))
                                    builder_.CreateStore(
                                        llvm::ConstantPointerNull::get(ptrTy()),
                                        slot);
                            }
                        }
                        return nullptr;
                    }
                }
                // §30.15 (#600 phase c): Channel<T>.Dispose() — close the channel,
                // fault pending receivers with ChannelClosedException, dispose
                // undelivered items in reverse order, and free the struct.
                // After the call, null the receiver slot (if it is a local/field
                // this scope owns) so the `using var` scope-exit guard finds null
                // and skips, preventing a double-free (§28.2 exactly-once rule).
                {
                    auto* recvTy = ctx_.exprType(*ma->target);
                    if (recvTy && builtinTypeName(recvTy) == "Channel") {
                        llvm::Value* ch = lowerExpr(*ma->target);
                        if (ch && ch->getType()->isPointerTy()) {
                            llvm::Value* cceVt =
                                llvm::ConstantPointerNull::get(ptrTy());
                            {   auto vit = builtinVtable_.find(
                                    "ChannelClosedException");
                                if (vit != builtinVtable_.end())
                                    cceVt = vit->second;
                            }
                            auto* fn = declareC("bz_channel_dispose",
                                llvm::Type::getVoidTy(llvm_),
                                {ptrTy(), ptrTy()});
                            builder_.CreateCall(fn, {ch, cceVt});
                            // Null the slot so any subsequent using-var scope-exit
                            // reload sees null and skips (§28.2).
                            if (!checkedDisposal_) {
                                llvm::Type* slotTy = nullptr;
                                if (llvm::Value* slot =
                                        lvalueAddr(*ma->target, slotTy))
                                    builder_.CreateStore(
                                        llvm::ConstantPointerNull::get(ptrTy()),
                                        slot);
                            }
                        }
                        return nullptr;
                    }
                }
                // §30.15 (#600 phase c): ChannelSender<T>.Dispose() — release
                // this sender handle (may close the channel if last sender).
                // After the call, null the receiver slot to prevent double-free
                // at using-var scope exit (§28.2 exactly-once rule).
                {
                    auto* recvTy = ctx_.exprType(*ma->target);
                    if (recvTy && builtinTypeName(recvTy) == "ChannelSender") {
                        llvm::Value* tx = lowerExpr(*ma->target);
                        if (tx && tx->getType()->isPointerTy()) {
                            llvm::Value* cceVt =
                                llvm::ConstantPointerNull::get(ptrTy());
                            {   auto vit = builtinVtable_.find(
                                    "ChannelClosedException");
                                if (vit != builtinVtable_.end())
                                    cceVt = vit->second;
                            }
                            auto* fn = declareC("bz_channel_sender_release",
                                llvm::Type::getVoidTy(llvm_),
                                {ptrTy(), ptrTy()});
                            builder_.CreateCall(fn, {tx, cceVt});
                            // Null the slot so any subsequent using-var scope-exit
                            // reload sees null and skips (§28.2).
                            if (!checkedDisposal_) {
                                llvm::Type* slotTy = nullptr;
                                if (llvm::Value* slot =
                                        lvalueAddr(*ma->target, slotTy))
                                    builder_.CreateStore(
                                        llvm::ConstantPointerNull::get(ptrTy()),
                                        slot);
                            }
                        }
                        return nullptr;
                    }
                }
                // §6.1 / §30.8: CancellationTokenSource.Dispose() — runs the
                // source's destructor (cancels surviving token observers, frees
                // the source object) via bz_cts_dispose.  Recognized by the
                // receiver's builtin type name "CancellationTokenSource".
                {
                    auto* recvTy = ctx_.exprType(*ma->target);
                    bool isCTS = recvTy &&
                        recvTy->kind == sema::TypeRef::Kind::Named &&
                        builtinTypeName(recvTy) == "CancellationTokenSource";
                    if (isCTS) {
                        llvm::Value* recv = lowerExpr(*ma->target);
                        if (recv && recv->getType()->isPointerTy()) {
                            auto* fn = declareC("bz_cts_dispose",
                                llvm::Type::getVoidTy(llvm_), {ptrTy()});
                            builder_.CreateCall(fn, {recv});
                        }
                        return nullptr;
                    }
                }
                // §28.13 / stdlib §2.16 (#603): Shared<T>.Dispose() — explicit
                // release of one strong handle.
                // §28.2 v0.135 / issue #627: in checked-disposal mode, call
                // bz_shared_mark_disposed (quarantine: dispose value, set
                // handle_disposed=1, keep block alive for guard reads) instead of
                // bz_shared_release (which frees the block and prevents guards).
                {
                    auto* recvTy2 = ctx_.exprType(*ma->target);
                    if (recvTy2 && builtinTypeName(recvTy2) == "Shared") {
                        llvm::Value* recv2 = lowerExpr(*ma->target);
                        if (recv2 && recv2->getType()->isPointerTy()) {
                            const char* relName = checkedDisposal_
                                ? "bz_shared_mark_disposed"
                                : "bz_shared_release";
                            auto* fn = declareC(relName,
                                llvm::Type::getVoidTy(llvm_), {ptrTy()});
                            builder_.CreateCall(fn, {recv2});
                        }
                        return nullptr;
                    }
                }
                // §28.13 / stdlib §2.16 (#603): Weak<T>.Dispose() — explicit
                // release of one weak handle via bz_weak_release.
                {
                    auto* recvTy2 = ctx_.exprType(*ma->target);
                    if (recvTy2 && builtinTypeName(recvTy2) == "Weak") {
                        llvm::Value* recv2 = lowerExpr(*ma->target);
                        if (recv2 && recv2->getType()->isPointerTy()) {
                            auto* fn = declareC("bz_weak_release",
                                llvm::Type::getVoidTy(llvm_), {ptrTy()});
                            builder_.CreateCall(fn, {recv2});
                        }
                        return nullptr;
                    }
                }
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
                    } else if (isLinkedListTypeName(recvTy)) {
                        // per §4.6/§4.7: LinkedList<T>.Dispose() → bz_llist_free.
                        // OwnedLinkedList<T> cascades disposal into class elements.
                        // #602 / §28.11 v0.135: route directly through elementPolicyFor.
                        {
                            const sema::TypeRef* llElem =
                                (!recvTy->genericArgs.empty())
                                    ? recvTy->genericArgs[0] : nullptr;
                            bool llIsOwning =
                                recvTy->astNode && recvTy->astNode->name &&
                                !recvTy->astNode->name->parts.empty() &&
                                recvTy->astNode->name->parts[0].rfind("Owned",0) == 0;
                            llvm::Function* elemDisp =
                                elementPolicyFor(llElem, llIsOwning).disposerThunk;
                            llvm::Value* disp = elemDisp
                                ? llvm::cast<llvm::Value>(elemDisp)
                                : llvm::ConstantPointerNull::get(ptrTy());
                            auto* fn = declareC("bz_llist_free",
                                llvm::Type::getVoidTy(llvm_), {ptrTy(), ptrTy()});
                            builder_.CreateCall(fn, {recv, disp});
                        }
                    } else if (isCollectionTypeName(recvTy)) {
                        // #362 / stdlib §4.5: explicit Dispose() on an owning
                        // collection cascades disposal into its class elements;
                        // a borrowing collection frees only the backing.
                        // #602 / §28.11 v0.135: disposer derived from
                        // elementPolicyFor (covers Interface elements, #619).
                        // §28.2 v0.135 / #621: checked-disposal mode quarantines
                        // via bz_array_mark_disposed (flag stays readable for the
                        // indexer guard); unchecked mode frees as before.
                        {
                            const sema::TypeRef* arrElem =
                                (!recvTy->genericArgs.empty())
                                    ? recvTy->genericArgs[0] : nullptr;
                            bool arrIsOwning =
                                recvTy->astNode && recvTy->astNode->name &&
                                !recvTy->astNode->name->parts.empty() &&
                                recvTy->astNode->name->parts[0].rfind("Owned",0) == 0;
                            llvm::Function* elemDisp =
                                elementPolicyFor(arrElem, arrIsOwning).disposerThunk;
                            llvm::Value* disp = elemDisp
                                ? llvm::cast<llvm::Value>(elemDisp)
                                : llvm::ConstantPointerNull::get(ptrTy());
                            const char* freeName = checkedDisposal_
                                ? "bz_array_mark_disposed"
                                : "bz_array_free";
                            auto* fn = declareC(freeName,
                                llvm::Type::getVoidTy(llvm_), {ptrTy(), ptrTy()});
                            builder_.CreateCall(fn, {recv, disp});
                        }
                    } else if (isDictTypeName(recvTy)) {
                        // #396 / stdlib §4.9 + #401 / stdlib §4.8: explicit
                        // Dispose() on a hash-backed dictionary frees the backing
                        // table (and key bytes) and runs the per-value callback,
                        // the same one the #394/#401 scope-end path uses:
                        //   * OwnedDictionary<K, class/interface> → dispose+free
                        //     each surviving value (#396/#394).
                        //   * borrowing Dictionary<K, record> → free each
                        //     surviving record value BOX (#401; a record is a
                        //     value type, nothing to dispose, §3.2 / §9).
                        // A scalar/string-valued borrowing Dictionary stores no
                        // box → null disposer (only the backing is freed). The
                        // two value callbacks are mutually exclusive, so no box
                        // is freed twice.
                        // #602 / §28.11 v0.135: derive owning-class disposer
                        // directly from elementPolicyFor (covers Interface too).
                        // §28.2 v0.135 / issue #627: checked-disposal mode
                        // quarantines via bz_dict_mark_disposed (header stays
                        // alive for method-entry guard reads; bz_dict_free is
                        // idempotent w.r.t. disposed flag). In checked mode, the
                        // slot is NOT nulled — the pointer must remain valid so
                        // subsequent method calls can load and test it. The
                        // scope-end bz_dict_free is a no-op when disposed=1.
                        {
                            const sema::TypeRef* dictVal =
                                (recvTy->genericArgs.size() > 1)
                                    ? recvTy->genericArgs[1] : nullptr;
                            bool dictIsOwning =
                                recvTy->astNode && recvTy->astNode->name &&
                                !recvTy->astNode->name->parts.empty() &&
                                recvTy->astNode->name->parts[0] == "OwnedDictionary";
                            llvm::Function* valDisp =
                                elementPolicyFor(dictVal, dictIsOwning).disposerThunk;
                            if (!valDisp) valDisp = dictValueBoxFreer(recvTy);
                            llvm::Value* disp = valDisp
                                ? llvm::cast<llvm::Value>(valDisp)
                                : llvm::ConstantPointerNull::get(ptrTy());
                            const char* freeName = checkedDisposal_
                                ? "bz_dict_mark_disposed"
                                : "bz_dict_free";
                            auto* fn = declareC(freeName,
                                llvm::Type::getVoidTy(llvm_), {ptrTy(), ptrTy()});
                            builder_.CreateCall(fn, {recv, disp});
                            // §28.2 idempotency: in UNCHECKED mode, null the slot so
                            // any subsequent bz_dict_free reloads null and no-ops
                            // (bz_dict_free(null) returns immediately). In CHECKED
                            // mode, the slot is intentionally NOT nulled — the pointer
                            // must stay valid for guard checks; bz_dict_free is safe
                            // because it returns early when disposed==1 (#627).
                            if (!checkedDisposal_) {
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
                            // §28.2 v0.134 / issue #601: in checked-disposal
                            // mode the object is quarantined (not freed), so
                            // the slot must remain valid — the disposed-flag
                            // guard at the next member-entry reads through it.
                            // In unchecked mode, null the slot as usual to
                            // prevent a second reclaim.
                            if (!checkedDisposal_)
                                builder_.CreateStore(
                                    llvm::ConstantPointerNull::get(ptrTy()),
                                    slot);
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
        // §14.2 (record-conforms-to-interface) + §9.4 (record = value type):
        // when a generic method `M<T>(x: T) where T: IFoo` is monomorphized with
        // T = a RECORD, the body's `x.Foo()` was resolved by sema to the
        // INTERFACE method `IFoo.Foo` (member look-up went through the constraint
        // — §14.2's "T's surface includes IFoo's members"). The receiver `x` is a
        // record VALUE (a struct, not a pointer), so the itable-dispatch path in
        // emitMethodCall would either need a pointer receiver or hit the #484
        // non-pointer guard (returning nullptr → a malformed direct call to the
        // i32-typed generic template). Since T is concretely known at this
        // monomorphization, devirtualize: redirect `ms` to the record's own
        // implementing method (records are sealed — §9.6 — so the implementation
        // is exactly the one declared on the record). The receiver-shaping block
        // below then spills the record value and the call becomes a direct call
        // to `Point.Foo`, matching the non-generic `point.Foo()` path (control
        // (f) of record_generic_constraint_test).
        if (ms->parent && ms->parent->kind == ast::TypeDeclKind::Interface &&
            !recv->getType()->isPointerTy()) {
            if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(
                    call.callee.get())) {
                if (const sema::TypeRef* recvTy =
                        ma->target ? ctx_.exprType(*ma->target) : nullptr) {
                    // Resolve a bare type-parameter receiver (`T`, no
                    // declaration) through the active substitution to its
                    // concrete sema type, then take its record declaration.
                    const sema::TypeRef* concrete = recvTy;
                    if (recvTy->kind == sema::TypeRef::Kind::Named &&
                        !recvTy->declaration && recvTy->astNode &&
                        recvTy->astNode->name &&
                        recvTy->astNode->name->parts.size() == 1) {
                        auto sit = genericSubstSemaTy_.find(
                            recvTy->astNode->name->parts[0]);
                        if (sit != genericSubstSemaTy_.end() && sit->second)
                            concrete = sit->second;
                    }
                    if (concrete && concrete->declaration &&
                        concrete->declaration->kind ==
                            ast::TypeDeclKind::Record) {
                        if (sema::MethodSymbol* impl = findMethodByName(
                                concrete->declaration, ms->name))
                            ms = impl;  // devirtualize to the record's method
                    }
                }
            }
        }
        // §26.1 (#433): shape the receiver to the extension method's declared
        // `this` type. For a class/interface receiver `this` is the object
        // pointer (recv already is one); for a primitive/enum receiver it is
        // the value (recv already is one). For a RECORD receiver `this` is a
        // pointer to a spilled copy of the value (so the body's `this.field`
        // reuses record field access) — spill if recv came through as a value.
        if (isExtensionMethod(ms)) {
            bool byPtr = false;
            llvm::Type* thisTy = extensionThisType(ms, &byPtr);
            if (byPtr && thisTy && !recv->getType()->isPointerTy()) {
                // Record receiver passed as a value — spill to a stack slot
                // and hand over its address (the declared `this` is a ptr).
                llvm::Value* tmp = entryAlloca(recv->getType());
                builder_.CreateStore(recv, tmp);
                recv = tmp;
            }
        } else
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
    // per §14.1/§14.2 (issue #505): a generic *method* with a closure-typed
    // parameter (e.g. `GetV<T>(obj:T, getter:(T)->int):int`) may have lambda
    // arguments that need the concrete LLVM type for T to be compiled
    // correctly. `buildOrderedArgs` runs `lowerLambda` for each lambda arg,
    // which consults `expectedFunctionType_` (set from the formal param's
    // function type) to resolve param types. When the formal function type
    // contains an unresolved generic param T, `lowerTypeRef(T)` returns i32
    // and the lambda param keeps the placeholder type. As a result `x.v` on
    // an i32 receiver returns null → the closure emits `ret void` in an i32
    // function → IR verification failure.
    //
    // Fix: before calling `buildOrderedArgs`, infer the method-level generic
    // substitution from non-lambda arguments (using their sema exprType, not
    // lowering them) and install it in genericSubst_ so `lowerTypeRef(T)` in
    // `lowerLambda` returns the concrete LLVM type. IMPORTANT: restore
    // genericSubst_ to its ORIGINAL value after buildOrderedArgs completes,
    // before the main substitution setup at lines 5906-5974 runs. This prevents
    // the pre-inferred substitution from leaking into subsequent compilations
    // (emitFunctionBody for the unspecialized generic would then use T=ptr for
    // its closure call type, producing a i32(ptr,ptr) fnTy with an i32 arg →
    // IR verification failure).
    {
        bool didPrePass = false;
        std::unordered_map<std::string_view, llvm::Type*> preSavedSubst;
        std::unordered_map<std::string_view, const sema::TypeRef*> preSavedSema;
        if (auto* md = dynamic_cast<const ast::MethodDecl*>(ms->decl)) {
            if (!md->genericParams.empty()) {
                // Check whether any argument is a lambda (function-type formal).
                bool hasLambdaArg = false;
                for (const auto& a : call.args) {
                    if (a.value &&
                        dynamic_cast<const ast::LambdaExpr*>(a.value.get())) {
                        hasLambdaArg = true; break;
                    }
                }
                if (hasLambdaArg) {
                    // Pre-infer method generic params from non-lambda args.
                    std::unordered_map<std::string_view, llvm::Type*> preSubst =
                        genericSubst_;
                    std::unordered_map<std::string_view,
                                       const sema::TypeRef*> preSema =
                        genericSubstSemaTy_;
                    for (const auto& gp : md->genericParams) {
                        if (preSubst.count(gp.name)) continue;
                        for (std::size_t pi = 0;
                             pi < md->params.size() && pi < call.args.size();
                             ++pi) {
                            const ast::Type* pt = md->params[pi].type.get();
                            if (!pt || pt->kind != ast::TypeKind::Named ||
                                !pt->name ||
                                pt->name->parts.size() != 1 ||
                                pt->name->parts[0] != gp.name)
                                continue;
                            if (!call.args[pi].value) continue;
                            // Skip lambda args — only infer from concrete vals.
                            if (dynamic_cast<const ast::LambdaExpr*>(
                                    call.args[pi].value.get()))
                                continue;
                            sema::TypeRef* at =
                                ctx_.exprType(*call.args[pi].value);
                            if (!at) continue;
                            llvm::Type* lt = lowerTypeRef(at);
                            if (lt != i32()) {
                                preSubst[gp.name] = lt;
                                preSema[gp.name] = at;
                            }
                            break;
                        }
                    }
                    // per §14.1 (issue #516/#561): a generic method with U
                    // appearing ONLY in the lambda's return type (e.g.
                    // `Apply<U>(x:int, fn:(int)->U):U`) may leave U unbound
                    // after the non-lambda arg loop above — no concrete arg has
                    // declared type exactly `U`. If the call site supplies
                    // explicit type arguments (`Apply<string>(...)`), those
                    // explicit args settle the substitution. Seed from the
                    // callee's typeArgs (MemberAccessExpr or bare IdentifierExpr)
                    // before buildOrderedArgs runs so lowerLambda resolves
                    // lowerTypeRef(U) correctly when lowering the lambda return.
                    {
                    const std::vector<std::unique_ptr<ast::Type>>* callTAs =
                        nullptr;
                    if (auto* callMa =
                            dynamic_cast<const ast::MemberAccessExpr*>(
                                call.callee.get()))
                        callTAs = &callMa->typeArgs;
                    else if (auto* callId =
                                 dynamic_cast<const ast::IdentifierExpr*>(
                                     call.callee.get()))
                        callTAs = &callId->typeArgs;
                    if (callTAs) {
                        for (std::size_t gi = 0;
                             gi < md->genericParams.size() &&
                             gi < callTAs->size(); ++gi) {
                            std::string_view pname = md->genericParams[gi].name;
                            if (preSubst.count(pname)) continue;
                            if (!(*callTAs)[gi]) continue;
                            sema::TypeRef* concreteArg =
                                ctx_.resolvedType(*(*callTAs)[gi]);
                            if (!concreteArg) continue;
                            llvm::Type* lt = lowerTypeRef(concreteArg);
                            if (lt != i32()) {
                                preSubst[pname] = lt;
                                preSema[pname] = concreteArg;
                            }
                        }
                    }
                    }
                    if (preSubst != genericSubst_) {
                        preSavedSubst = genericSubst_;
                        preSavedSema  = genericSubstSemaTy_;
                        genericSubst_ = std::move(preSubst);
                        genericSubstSemaTy_ = std::move(preSema);
                        didPrePass = true;
                    }
                }
            }
        }
        // §22.4/§22.7: place arguments into parameter order.
        if (auto* md = dynamic_cast<const ast::MethodDecl*>(ms->decl)) {
            if (!buildOrderedArgs(md->params, call.args, args, &argExprs)) {
                // Restore before early-return.
                if (didPrePass) {
                    genericSubst_ = std::move(preSavedSubst);
                    genericSubstSemaTy_ = std::move(preSavedSema);
                }
                return nullptr;
            }
        } else {
            for (const auto& a : call.args) {
                if (!a.value) {
                    if (didPrePass) {
                        genericSubst_ = std::move(preSavedSubst);
                        genericSubstSemaTy_ = std::move(preSavedSema);
                    }
                    return nullptr;
                }
                llvm::Value* v = lowerExpr(*a.value);
                if (!v) {
                    if (didPrePass) {
                        genericSubst_ = std::move(preSavedSubst);
                        genericSubstSemaTy_ = std::move(preSavedSema);
                    }
                    return nullptr;
                }
                args.push_back(v);
                argExprs.push_back(a.value.get());
            }
        }
        // Restore genericSubst_ to original state after the lambda compilation
        // that benefited from the pre-inferred substitution. The definitive
        // substitution is set up below (lines ~5930+).
        if (didPrePass) {
            genericSubst_ = std::move(preSavedSubst);
            genericSubstSemaTy_ = std::move(preSavedSema);
        }
    }  // end pre-pass + buildOrderedArgs block

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
            } else if (recvTy &&
                       recvTy->kind == sema::TypeRef::Kind::Named &&
                       recvTy->declaration && recvTy->declaration != ms->parent &&
                       !recvTy->genericArgs.empty() &&
                       !ms->parent->genericParams.empty()) {
                // §12 / §14 (issue #585): an INHERITED method declared on a
                // generic BASE (`ms->parent`) called on a derived instance
                // (`recvTy` == Wrapper<string>). The branch above requires
                // recvTy->declaration == ms->parent, so it does not fire; without
                // a substitution the inherited method runs as the T→i32 template
                // and reads a non-int base field at the wrong width (#585). Walk
                // the base chain to bind the base's generic params to the
                // concrete arguments the derived class supplied.
                std::unordered_map<std::string_view, llvm::Type*> ancSubst;
                std::unordered_map<std::string_view, const sema::TypeRef*>
                    ancSema;
                if (computeAncestorGenericSubst(recvTy, ms->parent, ancSubst,
                                                ancSema)) {
                    genericSubst_ = std::move(ancSubst);
                    genericSubstSemaTy_ = std::move(ancSema);
                }
            }
        }
    }
    // per §14.1 / §22.12 (issue #520): a STATIC method on a generic class
    // accessed via explicit receiver type args (`Registry<int>.Make(7)`) has no
    // instance receiver to derive the substitution from — the `!isStaticMethod`
    // guard above skips it.  Instead, recover the concrete type args from the
    // callee's MemberAccessExpr target, which is an IdentifierExpr with a
    // non-empty `typeArgs` vector and an exprType of kind Named whose
    // genericArgs were populated by sema's IdentifierExpr visitor.
    if (isStaticMethod(ms) && ms->parent) {
        if (auto* callMa = dynamic_cast<const ast::MemberAccessExpr*>(
                call.callee.get())) {
            if (auto* recvId = dynamic_cast<const ast::IdentifierExpr*>(
                    callMa->target.get())) {
                if (!recvId->typeArgs.empty()) {
                    // The sema IdentifierExpr visitor built an instantiated
                    // TypeRef on the receiver; read its genericArgs directly.
                    sema::TypeRef* recvTy = ctx_.exprType(*recvId);
                    if (recvTy && recvTy->kind == sema::TypeRef::Kind::Named &&
                        recvTy->declaration == ms->parent &&
                        !recvTy->genericArgs.empty()) {
                        auto* td = dynamic_cast<const ast::TypeDecl*>(
                            ms->parent->decl);
                        if (td && td->genericParams.size() ==
                                  recvTy->genericArgs.size()) {
                            std::unordered_map<std::string_view, llvm::Type*>
                                newSubst;
                            std::unordered_map<std::string_view,
                                               const sema::TypeRef*>
                                newSema;
                            for (std::size_t gi = 0;
                                 gi < td->genericParams.size(); ++gi) {
                                llvm::Type* concrTy =
                                    lowerTypeRef(recvTy->genericArgs[gi]);
                                std::string_view pname =
                                    td->genericParams[gi].name;
                                newSubst[pname] = concrTy;
                                newSema[pname] = recvTy->genericArgs[gi];
                            }
                            genericSubst_ = std::move(newSubst);
                            genericSubstSemaTy_ = std::move(newSema);
                        }
                    }
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
            // per §14.1 (issues #515/#516/#561): a generic method whose type
            // param appears ONLY in lambda param/return positions (not in any
            // non-lambda param's declared type) leaves the param unbound by
            // the arg-inference loop above. When the call site supplies
            // explicit type arguments (`Transform<int>`), use them to bind
            // any still-unbound method generic params before the body is
            // emitted. This complements the pre-pass (which seeds the binding
            // for lowerLambda) and ensures the MAIN substitution (which drives
            // emitInlineGenericMethod / emitGenericSpecialization) also has
            // the correct binding, so that body-level dispatch (v.ToString()
            // on a U-typed receiver) resolves via genericSubstSemaTy_.
            // The callee may be a MemberAccessExpr (`obj.Method<T>(...)`) or
            // a bare IdentifierExpr (`Id<T>(...)` — in-scope call without dot).
            {
            const std::vector<std::unique_ptr<ast::Type>>* callTAs = nullptr;
            if (auto* callMa = dynamic_cast<const ast::MemberAccessExpr*>(
                    call.callee.get()))
                callTAs = &callMa->typeArgs;
            else if (auto* callId = dynamic_cast<const ast::IdentifierExpr*>(
                         call.callee.get()))
                callTAs = &callId->typeArgs;
            if (callTAs) {
                for (std::size_t gi = 0;
                     gi < md->genericParams.size() &&
                     gi < callTAs->size(); ++gi) {
                    std::string_view pname = md->genericParams[gi].name;
                    if (newSubst.count(pname)) continue;
                    if (!(*callTAs)[gi]) continue;
                    sema::TypeRef* concreteArg =
                        ctx_.resolvedType(*(*callTAs)[gi]);
                    if (!concreteArg) continue;
                    newSubst[pname] = lowerTypeRef(concreteArg);
                    newSema[pname] = concreteArg;
                }
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
    // Record the concrete subst that drives the RESULT local's layout so that
    // lowerLocalDecl can associate it with the result's local variable.
    //
    // issue #597 (§14.1 / §22.4): the layout of the call result is governed by
    // the call's RESULT type, which is NOT necessarily the substitution active
    // during the call.  For `Pair<int,string>.Swap(): Pair<B,A>` the active
    // genericSubst_ binds the RECEIVER's params (A→int, B→string), but the
    // result is `Pair<string,int>` (params swapped); recording the receiver's
    // subst as the result local's subst laid `s` out as the receiver type
    // `Pair<int,string>` and the later `s._b` read used the wrong field type
    // (ptr vs i32) → malformed `ret ptr` in an i32 function.  Derive the subst
    // from the call's sema exprType (the SUBSTITUTED result type) — its
    // genericArgs are the concrete types in declaration order — binding the
    // result class's own params positionally against them.  This runs while
    // genericSubst_/genericSubstSemaTy_ are still active, so a result type that
    // still references an enclosing param (`Box<U>` inside a `Foo<U>` body)
    // lowers its args through that binding correctly.  Only when the result
    // type is not a concrete generic Named do we fall back to the active subst.
    bool recorded = false;
    if (sema::TypeRef* resultTy = ctx_.exprType(call)) {
        if (resultTy->kind == sema::TypeRef::Kind::Named &&
            resultTy->declaration && !resultTy->genericArgs.empty()) {
            auto* td = dynamic_cast<const ast::TypeDecl*>(
                resultTy->declaration->decl);
            if (td && td->genericParams.size() ==
                      resultTy->genericArgs.size()) {
                std::unordered_map<std::string_view, llvm::Type*> newSubst;
                for (std::size_t gi = 0;
                     gi < td->genericParams.size(); ++gi) {
                    llvm::Type* concrTy =
                        lowerTypeRef(resultTy->genericArgs[gi]);
                    newSubst[td->genericParams[gi].name] = concrTy;
                }
                if (!newSubst.empty()) {
                    lastCallGenericSubst_ = std::move(newSubst);
                    recorded = true;
                }
            }
        }
    }
    if (!recorded && !genericSubst_.empty())
        lastCallGenericSubst_ = genericSubst_;
    genericSubst_ = std::move(savedSubst);
    genericSubstSemaTy_ = std::move(savedSemaTy);
    return result;
}


// §12 / §14 (issue #585): see the header comment in LlvmBackend_impl.hpp. Walk
// the base-class chain from `recvTy`'s declaration up to `ancestor`, resolving
// each level's base type-arguments through the level below, then fill the
// ancestor's generic-param substitution maps.
bool Emitter::computeAncestorGenericSubst(
        const sema::TypeRef* recvTy, const sema::TypeSymbol* ancestor,
        std::unordered_map<std::string_view, llvm::Type*>& outSubst,
        std::unordered_map<std::string_view, const sema::TypeRef*>& outSema) {
    if (!recvTy || recvTy->kind != sema::TypeRef::Kind::Named ||
        !recvTy->declaration || !ancestor || ancestor->genericParams.empty())
        return false;
    const sema::TypeSymbol* cur = recvTy->declaration;
    if (cur == ancestor) return false;  // declared directly on receiver class

    // curArgs: the concrete TypeRefs bound to `cur`'s generic params at this
    // level. Seeded from the receiver's own generic args, then re-derived for
    // each base level by resolving the base ref's args through this map.
    std::unordered_map<std::string_view, const sema::TypeRef*> curArgs;
    auto* curTd = dynamic_cast<const ast::TypeDecl*>(cur->decl);
    if (!curTd || curTd->genericParams.size() != recvTy->genericArgs.size())
        return false;
    for (std::size_t gi = 0; gi < curTd->genericParams.size(); ++gi)
        curArgs[curTd->genericParams[gi].name] = recvTy->genericArgs[gi];

    // Walk up the class base chain until we reach `ancestor`.
    while (cur && cur != ancestor) {
        const sema::TypeSymbol* base = baseClassOf(cur);
        if (!base) return false;  // ancestor not on the chain
        // Find the base TypeRef (carries the base's generic args expressed in
        // terms of `cur`'s generic params, e.g. Container<T_cur>).
        const sema::TypeRef* baseRef = nullptr;
        for (std::size_t bi = 0; bi < cur->baseTypes.size(); ++bi) {
            if (cur->baseTypes[bi] == base) {
                baseRef = (bi < cur->baseTypeRefs.size())
                              ? cur->baseTypeRefs[bi] : nullptr;
                break;
            }
        }
        // Resolve each of the base's generic args through curArgs (a bare
        // generic-param ref `T` → its concrete binding; an already-concrete
        // arg passes through). If the base has params but no usable baseRef,
        // we cannot resolve — bail.
        std::unordered_map<std::string_view, const sema::TypeRef*> nextArgs;
        auto* baseTd = dynamic_cast<const ast::TypeDecl*>(base->decl);
        if (!base->genericParams.empty()) {
            if (!baseRef || !baseTd ||
                baseTd->genericParams.size() != baseRef->genericArgs.size())
                return false;
            for (std::size_t gi = 0; gi < baseTd->genericParams.size(); ++gi) {
                const sema::TypeRef* arg = baseRef->genericArgs[gi];
                if (arg && arg->kind == sema::TypeRef::Kind::Named &&
                    !arg->declaration && arg->astNode && arg->astNode->name &&
                    arg->astNode->name->parts.size() == 1) {
                    auto it = curArgs.find(arg->astNode->name->parts[0]);
                    if (it != curArgs.end()) arg = it->second;
                }
                nextArgs[baseTd->genericParams[gi].name] = arg;
            }
        }
        cur = base;
        curArgs = std::move(nextArgs);
    }
    if (cur != ancestor) return false;
    // curArgs now holds ancestor's param-name → concrete TypeRef.
    for (const sema::Symbol* gp : ancestor->genericParams) {
        auto it = curArgs.find(gp->name);
        if (it == curArgs.end() || !it->second) return false;
        outSubst[gp->name] = lowerTypeRef(it->second);
        outSema[gp->name] = it->second;
    }
    return !outSubst.empty();
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
    // Is this an interface method dispatch? Check if the declaring type is
    // an interface (CG35-itable, §13).
    bool isIfaceMethod = ms->parent &&
                         ms->parent->kind == ast::TypeDeclKind::Interface;
    bool canDispatch = !baseQualified && !isStaticMethod(ms);
    unsigned vtSlot = 0;  // used only for class-virtual dispatch
    if (canDispatch) {
        if (isIfaceMethod) {
            // Interface dispatch: ifaceMethodIndex_ must have an entry.
            auto ifIt = ifaceMethodIndex_.find(ms);
            if (ifIt == ifaceMethodIndex_.end()) canDispatch = false;
        } else {
            auto slotIt = vtableSlot_.find(ms);
            if (slotIt != vtableSlot_.end()) vtSlot = slotIt->second;
            else canDispatch = false;
        }
    }
    if (canDispatch) {
        // fix #580 / §14.1 / §14.2 / §12 / §13: a VIRTUAL generic method (its own
        // `<T>` params bound in the active substitution) cannot dispatch through
        // the fixed-signature vtable/itable slot — the slot holds the i32-erased
        // template, so a non-int type argument is passed at the wrong
        // width/representation (a silent miscompile). Dispatch by the receiver's
        // dynamic descriptor to a per-(class, type-arg) monomorphized body
        // instead. Covers both class-virtual and interface-method parents.
        if (!genericSubst_.empty()) {
            auto* md = dynamic_cast<const ast::MethodDecl*>(ms->decl);
            bool genericMethodBound = false;
            if (md && !md->genericParams.empty())
                for (const auto& gp : md->genericParams)
                    if (genericSubst_.count(gp.name)) { genericMethodBound = true; break; }
            if (genericMethodBound) {
                llvm::Value* r = emitVirtualGenericDispatch(ms, args);
                if (r != nullptr || diverged_) {
                    return r;  // emitVirtualGenericDispatch runs emitPendingCheck per call
                }
            }
        }
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
        // issue #484 / §14.2: guard against emitting an itable/vtable load from
        // a non-pointer receiver. This happens when the compiled (unspecialized)
        // body of a generic method (e.g. `GenGet<T>(x: T)`) is emitted with T's
        // LLVM fallback type `i32`. In that body, `x: T` lowered to `i32`, so
        // `recv` is `i32`. A `load ptr from i32` instruction is invalid IR that
        // the backend rejects with "Cannot emit physreg copy instruction".
        // Returning nullptr here causes `emitImplicitReturn` to emit `ret i32 0`
        // for the unspecialized stub — the same safe no-op as before the sema fix.
        // The specialized versions (called via emitInlineGenericMethod /
        // emitGenericSpecialization with concrete `ptr` args) have a real `ptr`
        // receiver and reach the dispatch correctly.
        if (!recv->getType()->isPointerTy()) return nullptr;
        llvm::Value* vptr = builder_.CreateLoad(ptrTy(), recv);
        llvm::Value* fnPtr = nullptr;
        if (isIfaceMethod) {
            // CG35-itable (§13): per-interface itable dispatch.
            // vptr[1] = itmap$C; call bz_itable_lookup(itmap, td$I) → itab$C$I;
            // fnPtr = itab$C$I[k] where k = positional index of ms in I.
            auto* lookupFn = declareC("bz_itable_lookup", ptrTy(),
                                      {ptrTy(), ptrTy()});
            llvm::Value* itmapPtr = builder_.CreateLoad(
                ptrTy(), builder_.CreateGEP(ptrTy(), vptr,
                                            {builder_.getInt32(1)}));
            // td$I is the descriptor global for the interface.
            const sema::TypeSymbol* iface = ms->parent;
            auto descIt = descArray_.find(iface);
            if (descIt == descArray_.end()) { return nullptr; }
            llvm::Value* ifaceTd = descIt->second;
            llvm::Value* itab = builder_.CreateCall(
                llvm::FunctionType::get(ptrTy(), {ptrTy(), ptrTy()}, false),
                lookupFn, {itmapPtr, ifaceTd}, "itab");
            unsigned k = ifaceMethodIndex_.at(ms);
            fnPtr = builder_.CreateLoad(
                ptrTy(),
                builder_.CreateGEP(ptrTy(), itab, {builder_.getInt32(k)}),
                "ifn");
        } else {
            // Class virtual dispatch: vtSlot is the vtable index (virtuals
            // base at slot 4 after td/itmap/Dispose/ToString — §8.6/§8.5 #417).
            llvm::Value* slotPtr = builder_.CreateGEP(
                ptrTy(), vptr, {builder_.getInt32(vtSlot)});
            fnPtr = builder_.CreateLoad(ptrTy(), slotPtr);
        }
        llvm::Value* r = builder_.CreateCall(fnTy, fnPtr, args);
        emitPendingCheck();  // CG20: a callee may have thrown
        return r;
    }

    // Non-virtual (or base-qualified) → direct call to the function.
    auto it = methodFns_.find(ms);
    if (it == methodFns_.end()) return nullptr;
    llvm::Function* callee = it->second;
    if (args.size() != callee->arg_size()) return nullptr;
    // §14.2 / issue #484 (non-virtual complement): guard against a direct call
    // to a method whose first parameter is `ptr` (the class/record `this`) when
    // the receiver arg is NOT a pointer. This happens when the UNSPECIALIZED
    // generic template body is emitted with type param T erased to i32: the
    // receiver `args[0]` is `i32`, but the callee's first param is `ptr`. The
    // virtual-dispatch branch already has this guard (line ~5925); without it
    // here the compiler emits a malformed IR instruction
    //   call i32 @bz$Helper$Add(i32 recv, ...)
    // that the IR verifier rejects. Returning nullptr lets emitImplicitReturn
    // emit `ret i32 0` for the stub — the same safe no-op as the virtual path.
    // The specialized invocation (emitInlineGenericMethod / emitGenericSpec
    // with a concrete `ptr` receiver) reaches this code correctly and is
    // unaffected, because its receiver arg IS a pointer.
    if (!isStaticMethod(ms) && !args.empty()) {
        llvm::FunctionType* calleeTy = callee->getFunctionType();
        if (!args[0]->getType()->isPointerTy() &&
            calleeTy->getNumParams() > 0 &&
            calleeTy->getParamType(0)->isPointerTy())
            return nullptr;
    }
    // CG-generic: if the generic substitution is active and the method
    // is on a class that has generic parameters, we must specialize the
    // call — even for methods with non-generic param/return types (like
    // `Size(): int`), because the struct layout changes when generic field
    // types (e.g. [5]T) are substituted with concrete types (e.g. [5]ptr).
    // §22.12 / #536: this now also covers STATIC methods on generic classes —
    // a static method body may access static fields, which must resolve to the
    // per-instantiation global (e.g. `bz$Counter$int$count`).  Monomorphizing
    // the body ensures genericSubst_ is active inside it so staticFieldGlobal
    // can redirect to the instantiation-specific global.
    if (!genericSubst_.empty()) {
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
            // per §14.1 (issue #515): a generic METHOD (with method-level
            // `<U>` params — distinct from class-level generic params handled
            // by the parentIsGeneric block above) has a compiled template body
            // that used the i32 placeholder for every type param. Method calls
            // on a type-param receiver in the template body (e.g.
            // `v.ToString()` where v:U) return null — the template body is a
            // correct TYPE-SAFE stub but semantically wrong: it returns null
            // when the body needs a real method dispatch. Even when the
            // template's function signature matches the concrete call's arg
            // types (e.g. both U=int and the template both use i32), the body
            // logic is wrong. Force specialization so the monomorphized body
            // is emitted with the active genericSubst_ and can correctly
            // dispatch any Object methods on the concrete type.
            if (!typesMismatch && !md->genericParams.empty()) {
                // At least one of the method's type params is bound in the
                // current substitution — force specialization.
                for (const auto& gp : md->genericParams) {
                    if (genericSubst_.count(gp.name)) {
                        typesMismatch = true; break;
                    }
                }
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
    // Build the function type with concrete arg/ret types.
    std::vector<llvm::Type*> paramTys;
    for (auto* a : args) paramTys.push_back(a->getType());
    llvm::Function* specFn = buildSpecializedMethodFn(ms, ci, paramTys);
    if (!specFn) return nullptr;
    // Call the specialized function.
    if (args.size() != specFn->arg_size()) return nullptr;
    llvm::Value* r = builder_.CreateCall(specFn, args);
    emitPendingCheck();
    return r;
}


llvm::Function* Emitter::buildSpecializedMethodFn(
        sema::MethodSymbol* ms, const CallableInfo& ci,
        const std::vector<llvm::Type*>& paramTys) {
    // Build the substitution key for caching. per §14.2 (monomorphization): the
    // key must distinguish instantiations whose specialized bodies differ. The
    // erased LLVM type vector alone is not enough — every class/interface type
    // erases to `ptr`, so `Maker<A>.Run` and `Maker<B>.Run` would collide on
    // `[ptr]` and the second would reuse the first's body (wrong when the body
    // disposes a concrete class via `using var a := new T()`). Pair each LLVM
    // type with a stable per-type identity token from the parallel
    // genericSubstSemaTy_ (TypeSymbol* for a Named type, tagged primitive id
    // otherwise), iterated in lockstep so the two vectors stay order-aligned.
    std::vector<llvm::Type*> substKey;
    std::vector<const void*> idKey;
    for (auto& [name, ty] : genericSubst_) {
        substKey.push_back(ty);
        auto sit = genericSubstSemaTy_.find(name);
        idKey.push_back(
            sit != genericSubstSemaTy_.end()
                ? typeRefIdentityToken(sit->second)
                : nullptr);
    }

    auto cacheKey = std::make_tuple(ms, substKey, idKey);
    auto cit = specializedMethods_.find(cacheKey);
    if (cit != specializedMethods_.end()) {
        // Already compiled — share one specialization across identical
        // instantiations (same ms + same LLVM types + same type identities).
        return cit->second;
    }

    // Return type: apply genericSubst_ to the declared return type.
    llvm::Type* retTy = llvm::Type::getVoidTy(llvm_);
    if (ci.returnType) {
        sema::TypeRef* rt = ctx_.resolvedType(*ci.returnType);
        if (rt) retTy = lowerTypeRef(rt);
    }
    auto* fnTy = llvm::FunctionType::get(retTy, paramTys, false);

    // Create the specialized function. Append a per-type-arg discriminator so
    // distinct instantiations get distinct, readable IR names (e.g.
    // Maker$Run$spec$A vs Maker$Run$spec$B). InternalLinkage names need not be
    // unique — LLVM uniquifies them — but a meaningful suffix aids debugging and
    // makes the type-aware key visible in the emitted IR. Genuinely-identical
    // instantiations never reach here (they hit the cache above).
    std::string fname = std::string(ms->parent ? ms->parent->name : "")
                        + "$" + std::string(ms->name) + "$spec";
    for (auto& [name, sty] : genericSubstSemaTy_) {
        if (sty && sty->kind == sema::TypeRef::Kind::Named && sty->declaration)
            fname += "$" + std::string(sty->declaration->name);
    }
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
    // fix #568 / §8.6: the specialized body's ReturnStmt lowering consults
    // currentReturnTypeNode_ (return-value boxing, nullable-value coercion). It
    // must name THIS method's declared return type while the body is emitted —
    // not the outer caller's. Save and restore around the body.
    const ast::Type* savedRetNode = currentReturnTypeNode_;
    currentReturnTypeNode_ = ci.returnType;
    // fix #628 / §9.5 / §14.2: specFn is NOT registered in methodFns_, so
    // lowerWith's reverse-lookup (fn == currentFn_ in methodFns_) cannot find
    // the parent record TypeSymbol.  Store `ms` in currentSpecMethodSymbol_ for
    // the duration of the body so lowerWith can use it as a fallback.
    sema::MethodSymbol* savedSpecMs = currentSpecMethodSymbol_;
    currentSpecMethodSymbol_ = ms;

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
    if (!isStaticMethod(ms)) {
        currentThis_ = &*argIt++;
    } else {
        currentThis_ = nullptr;
    }
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

    // Emit the body. fix #580: handle both EXPRESSION and block bodies, so a
    // virtual generic method whose override is `=> $"..."` (an expression body)
    // produces a callable standalone function for dispatch-by-descriptor.
    if (ci.bodyKind == ast::MethodBodyKind::Expression && ci.exprBody) {
        llvm::Value* rv = lowerExpr(*ci.exprBody);
        // Mirror emitMethodBody's `=> expr` handling: a diverging expr (e.g.
        // `=> throw E`) already terminated the block; otherwise box a value-typed
        // result returned through an Object/interface return type (§8.6).
        if (diverged_) {
            /* block already terminated */
        } else {
            if (rv && ci.returnType)
                rv = boxForTarget(rv, ctx_.resolvedType(*ci.returnType),
                                  ci.exprBody);
            if (!emitReturn(specFn, rv)) emitImplicitReturn(specFn);
        }
    } else {
        bool terminated = ci.stmts && lowerScopedBlock(*ci.stmts);
        if (!terminated) emitImplicitReturn(specFn);
    }

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
    currentReturnTypeNode_ = savedRetNode;
    currentSpecMethodSymbol_ = savedSpecMs;  // fix #628

    return specFn;
}


// fix #580 / §14.1 / §14.2 / §12 / §13: dispatch a VIRTUAL generic method by the
// receiver's dynamic type, calling a per-(class, type-arg) monomorphized body.
//
// A virtual generic method cannot be dispatched through the ordinary vtable (or
// interface itable) slot: every override/implementation is compiled ONCE as a
// template with each type parameter erased to the i32 placeholder, so the slot's
// function signature is fixed to that placeholder. Calling it with a concrete
// non-int type argument (string=ptr, double=f64) passes the argument at the wrong
// width/representation through the slot — a silent miscompile (issue #580). And
// the template body itself formats `x:T` as an int (T→i32), so even a
// width-correct call would be wrong.
//
// The dynamic type is statically known to be one of a finite set:
//   * class parent     — the static class (= ms->parent) and its subclasses;
//   * interface parent — every class that implements the interface.
// We load the receiver's runtime descriptor (vtable[0] = td$C), compare it
// against each candidate class's descriptor global, and call the monomorphized
// specialization of the override/implementation that candidate runs.
llvm::Value* Emitter::emitVirtualGenericDispatch(sema::MethodSymbol* ms,
                                        std::vector<llvm::Value*>& args) {
    if (args.empty() || !args[0]->getType()->isPointerTy()) return nullptr;
    const sema::TypeSymbol* staticType = ms->parent;
    if (!staticType) return nullptr;
    auto* staticMd = dynamic_cast<const ast::MethodDecl*>(ms->decl);
    if (!staticMd) return nullptr;
    bool ifaceParent = staticType->kind == ast::TypeDeclKind::Interface;

    // The most-derived method named `ms->name` reachable from class `c`
    // (the override or interface implementation `c`'s instances run).
    auto mostDerived = [&](const sema::TypeSymbol* c) -> sema::MethodSymbol* {
        return findMethodByName(c, ms->name);
    };
    auto isSubclassOf = [&](const sema::TypeSymbol* c,
                            const sema::TypeSymbol* anc) -> bool {
        for (const sema::TypeSymbol* t = c; t; t = baseClassOf(t))
            if (t == anc) return true;
        return false;
    };
    auto implementsIface = [&](const sema::TypeSymbol* c,
                               const sema::TypeSymbol* iface) -> bool {
        std::vector<const sema::TypeSymbol*> supers;
        collectTypeAndBases(c, supers);
        for (auto* s : supers) if (s == iface) return true;
        return false;
    };

    // Candidate classes whose instances could reach this call: subclasses (class
    // parent) or implementers (interface parent), each paired with the
    // implementation it runs.
    struct Branch { const sema::TypeSymbol* cls; sema::MethodSymbol* impl; };
    std::vector<Branch> branches;
    sema::MethodSymbol* defaultImpl = nullptr;

    if (!ifaceParent) {
        // CLASS parent: the static class's own override is the natural default
        // (always reachable). A subclass that does NOT override differently
        // inherits this default — no branch needed for it (dedup by impl).
        defaultImpl = mostDerived(staticType);
        if (!defaultImpl) defaultImpl = ms;
        for (const auto& sym : ctx_.symbols()) {
            auto* c = dynamic_cast<const sema::TypeSymbol*>(sym.get());
            if (!c || c->kind != ast::TypeDeclKind::Class) continue;
            if (c == staticType) continue;
            if (!isSubclassOf(c, staticType)) continue;
            if (!descArray_.count(c)) continue;
            sema::MethodSymbol* impl = mostDerived(c);
            if (!impl || impl == defaultImpl) continue;  // inherits the default
            branches.push_back({c, impl});
        }
    } else {
        // INTERFACE parent: there is no instantiable static type, so every
        // implementer class must be matched by its OWN descriptor (two classes
        // may share an inherited implementation but still need distinct branches,
        // since the default is only correct for the one class it belongs to). The
        // last implementer becomes the default branch.
        std::vector<Branch> all;
        for (const auto& sym : ctx_.symbols()) {
            auto* c = dynamic_cast<const sema::TypeSymbol*>(sym.get());
            if (!c || c->kind != ast::TypeDeclKind::Class) continue;
            if (!implementsIface(c, staticType)) continue;
            if (!descArray_.count(c)) continue;
            sema::MethodSymbol* impl = mostDerived(c);
            if (!impl) continue;
            all.push_back({c, impl});
        }
        if (all.empty()) return nullptr;  // no implementer — bail safely
        defaultImpl = all.back().impl;
        all.pop_back();
        branches = std::move(all);
    }
    if (!defaultImpl) return nullptr;  // no implementation found — bail safely

    // Re-key the active substitution by the override's own generic-param names.
    // Base::Show<T> and Derived::Show<U> may name the parameter differently; the
    // substitution we built keys on the static method's names, so positionally
    // remap to the override's names before building its specialization.
    auto specForImpl = [&](sema::MethodSymbol* impl) -> llvm::Function* {
        CallableInfo ci;
        if (!callableInfo(impl, ci)) return nullptr;
        auto* implMd = dynamic_cast<const ast::MethodDecl*>(impl->decl);
        // Build paramTys from the actual args (already concrete).
        std::vector<llvm::Type*> paramTys;
        for (auto* a : args) paramTys.push_back(a->getType());
        auto savedSubst = genericSubst_;
        auto savedSema = genericSubstSemaTy_;
        if (implMd && implMd->genericParams.size() ==
                          staticMd->genericParams.size() &&
            !implMd->genericParams.empty()) {
            std::unordered_map<std::string_view, llvm::Type*> rk;
            std::unordered_map<std::string_view, const sema::TypeRef*> rks;
            for (std::size_t gi = 0; gi < staticMd->genericParams.size(); ++gi) {
                std::string_view from = staticMd->genericParams[gi].name;
                std::string_view to = implMd->genericParams[gi].name;
                auto it = genericSubst_.find(from);
                if (it != genericSubst_.end()) rk[to] = it->second;
                auto is = genericSubstSemaTy_.find(from);
                if (is != genericSubstSemaTy_.end()) rks[to] = is->second;
            }
            genericSubst_ = std::move(rk);
            genericSubstSemaTy_ = std::move(rks);
        }
        llvm::Function* fn = buildSpecializedMethodFn(impl, ci, paramTys);
        genericSubst_ = std::move(savedSubst);
        genericSubstSemaTy_ = std::move(savedSema);
        return fn;
    };

    // Build all specializations up front (each into its own standalone function);
    // none is emitted inline into the current block.
    llvm::Function* defFn = specForImpl(defaultImpl);
    if (!defFn) return nullptr;
    std::vector<std::pair<const sema::TypeSymbol*, llvm::Function*>> brFns;
    for (auto& b : branches) {
        llvm::Function* f = specForImpl(b.impl);
        if (!f) return nullptr;
        brFns.push_back({b.cls, f});
    }

    // Common case: no subclass overrides differently — a direct call to the
    // single specialization is correct (the dynamic type always most-derives it).
    if (brFns.empty()) {
        if (args.size() != defFn->arg_size()) return nullptr;
        llvm::Value* r = builder_.CreateCall(defFn, args);
        emitPendingCheck();
        return r;
    }

    // Emit the dispatch: load td = vptr[0], compare against each branch class's
    // descriptor, call its specialization; otherwise the default. The call result
    // (if non-void) is collected into a phi over the per-branch end blocks.
    llvm::FunctionType* defTy = defFn->getFunctionType();
    llvm::Type* retTy = defTy->getReturnType();
    bool hasResult = !retTy->isVoidTy();

    llvm::Value* recv = args[0];
    llvm::Value* vptr = builder_.CreateLoad(ptrTy(), recv, "vt");
    llvm::Value* td = builder_.CreateLoad(
        ptrTy(),
        builder_.CreateGEP(ptrTy(), vptr, {builder_.getInt32(0)}), "td");

    llvm::BasicBlock* endBB =
        llvm::BasicBlock::Create(llvm_, "vgen.end", currentFn_);
    std::vector<std::pair<llvm::Value*, llvm::BasicBlock*>> incoming;

    for (std::size_t i = 0; i < brFns.size(); ++i) {
        llvm::Value* desc = descArray_[brFns[i].first];
        llvm::Value* eq = builder_.CreateICmpEQ(td, desc, "vgen.is");
        llvm::BasicBlock* thenBB =
            llvm::BasicBlock::Create(llvm_, "vgen.case", currentFn_);
        llvm::BasicBlock* nextBB =
            llvm::BasicBlock::Create(llvm_, "vgen.next", currentFn_);
        builder_.CreateCondBr(eq, thenBB, nextBB);
        builder_.SetInsertPoint(thenBB);
        if (args.size() != brFns[i].second->arg_size()) return nullptr;
        llvm::Value* cr = builder_.CreateCall(brFns[i].second, args);
        emitPendingCheck();
        incoming.push_back({hasResult ? cr : nullptr,
                            builder_.GetInsertBlock()});
        builder_.CreateBr(endBB);
        builder_.SetInsertPoint(nextBB);
    }
    // Default branch (current block = the last nextBB).
    if (args.size() != defFn->arg_size()) return nullptr;
    llvm::Value* dr = builder_.CreateCall(defFn, args);
    emitPendingCheck();
    incoming.push_back({hasResult ? dr : nullptr, builder_.GetInsertBlock()});
    builder_.CreateBr(endBB);

    builder_.SetInsertPoint(endBB);
    if (!hasResult) {
        // Void method: there is no value to phi, but the caller distinguishes
        // "handled" from "not handled" by a non-null return. Return the (void)
        // default call instruction as a non-null sentinel — its value is never
        // used (void), and returning nullptr would make the caller fall through
        // to the regular vtable path and emit a second, broken call.
        return dr;
    }
    llvm::PHINode* phi = builder_.CreatePHI(retTy, incoming.size(), "vgen.res");
    for (auto& [v, bb] : incoming) phi->addIncoming(v, bb);
    return phi;
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
    // fix #628 (phase a) / §9.5 / §14.2: for EXPRESSION-body generic record
    // instance methods (`this with … end`), lowerWith's §575 reverse-lookup
    // uses `currentFn_` to find the parent record TypeSymbol.  But
    // emitInlineGenericMethod does NOT change currentFn_ (the inlining happens
    // inside the CALLER's function), so the lookup finds the caller's function
    // instead of Box.WithValue's registered template — and fails.  Store `ms`
    // in currentSpecMethodSymbol_ as a fallback, exactly as buildSpecializedMethodFn
    // does for block bodies.
    sema::MethodSymbol* savedSpecMs = currentSpecMethodSymbol_;
    currentSpecMethodSymbol_ = ms;

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
    currentSpecMethodSymbol_ = savedSpecMs;  // fix #628 (phase a)

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
    if (!recv) return nullptr;

    // §7.5 / #458: handle a NULLABLE VALUE type receiver (`int?`, `bool?`,
    // `enum?`, `record?`). These lower to a tagged {i8 hasValue; T value}
    // struct, NOT a pointer. Nullness is `hasValue == 0`.
    const sema::TypeRef* recvTy = ctx_.exprType(*nm.target);
    bool nullableValueRecv = recvTy && isNullableValueTy(recvTy) &&
                             recv->getType()->isStructTy();
    // For a plain reference receiver, fail-fast on non-pointer (old guard).
    if (!nullableValueRecv && !recv->getType()->isPointerTy()) return nullptr;

    auto* entryBB = builder_.GetInsertBlock();
    auto* callBB =
        llvm::BasicBlock::Create(llvm_, "nullcall.do", currentFn_);
    auto* endBB =
        llvm::BasicBlock::Create(llvm_, "nullcall.end", currentFn_);
    if (nullableValueRecv) {
        // §7.5 / #458: null-conditional on a nullable VALUE receiver: extract
        // hasValue and branch; the value slot is only used when hasValue is true.
        llvm::Value* hasVal = nullablePrimHasValue(recv);  // i1
        builder_.CreateCondBr(hasVal, callBB, endBB);
    } else {
        llvm::Value* isNull = builder_.CreateICmpEQ(
            recv, llvm::ConstantPointerNull::get(ptrTy()));
        builder_.CreateCondBr(isNull, endBB, callBB);
    }

    builder_.SetInsertPoint(callBB);
    std::vector<llvm::Value*> args;
    std::vector<const ast::Expr*> argExprs;
    if (nullableValueRecv) {
        // §7.5 / #458: pass the INNER value (not the tagged struct) as `this`.
        // For an extension method on a record (byPointer=true) the inner value
        // must be spilled to a stack slot first; for a primitive/enum it is
        // passed by value. Mirror the shaping that lowerCall applies
        // (LlvmBackend_Calls.cpp §26.1 block) so the ABI matches the callee.
        llvm::Value* innerVal = nullablePrimValue(recv);  // the T in {i8,T}
        if (isExtensionMethod(ms)) {
            bool byPtr = false;
            llvm::Type* thisTy = extensionThisType(ms, &byPtr);
            if (byPtr && thisTy && !innerVal->getType()->isPointerTy()) {
                llvm::Value* tmp = entryAlloca(innerVal->getType());
                builder_.CreateStore(innerVal, tmp);
                innerVal = tmp;
            }
        }
        args.push_back(innerVal);
    } else {
        args.push_back(recv);
    }
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
    if (!md) {
        // §2.5 / §12 (issue #328 item 2): an IMPORTED method has no AST decl;
        // reconstruct its function type from the loader-recorded signature, the
        // same shape declareImportedFunctions uses, so a VIRTUAL call through an
        // imported object's vtable types its dispatch correctly.
        if (!ctx_.isImported(ms)) return nullptr;
        sema::TypeRef* retRef = ctx_.importedReturnType(ms);
        llvm::Type* retTy = retRef ? lowerTypeRef(retRef)
                                   : llvm::Type::getVoidTy(llvm_);
        std::vector<llvm::Type*> paramTys;
        if (!isStaticMethod(const_cast<sema::MethodSymbol*>(ms)))
            paramTys.push_back(ptrTy());
        for (sema::Symbol* p : ms->params) {
            auto* ps = dynamic_cast<sema::ParameterSymbol*>(p);
            sema::TypeRef* pt = ps ? ps->resolvedDeclType : nullptr;
            paramTys.push_back(
                isByRefMod(ps ? ps->modifier : std::string_view())
                    ? ptrTy()
                    : (pt ? lowerTypeRef(pt) : i32()));
        }
        return llvm::FunctionType::get(retTy, paramTys, /*isVarArg=*/false);
    }
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
    if (const sema::TypeSymbol* rec = recordTypeOfRef(ty)) {
        // issue #462 / §14.1: a GENERIC record key (`Dictionary<Box<string>, V>`)
        // needs the specialized struct layout + concrete per-field hashing inside
        // the trampoline. A no-op for a non-generic record key.
        RecordSubstGuard keySubst(this, ty);
        return buildRecordHashTrampoline(rec);
    }
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
    if (const sema::TypeSymbol* rec = recordTypeOfRef(ty)) {
        // issue #462 / §14.1: a GENERIC record key needs the specialized struct
        // layout + concrete per-field equality inside the trampoline. A no-op for
        // a non-generic record key.
        RecordSubstGuard keySubst(this, ty);
        return buildRecordEqTrampoline(rec);
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

// stdlib §5 Set-like (Union/Intersect/Except): hash trampoline for the
// membership dict. Matches the runtime's `uint64_t(*)(const void*)` function
// pointer slot in bz_dict_t. Reuses buildValueHashTrampoline for non-null
// elemRef (record / scalar / string — all equality-bearing types). Falls back
// to a simple identity hash on the raw element bytes for pointer-sized elements
// (class pointers) where content equality is reference equality.
llvm::Function* Emitter::buildLinqHashTrampoline(llvm::Type* elemTy,
                                                  const sema::TypeRef* elemRef,
                                                  const sema::TypeSymbol* elemRec) {
    // For an equality-bearing value type (record, primitive, enum) reuse the
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
    llvm::Value* h = elemTy->isIntegerTy(64) ? v
                   : elemTy->isIntegerTy(32)
                       ? builder_.CreateZExt(v, i64())
                       : elemTy->isPointerTy()
                           ? builder_.CreatePtrToInt(v, i64())
                           : builder_.CreateZExtOrBitCast(v, i64());
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
    // per §4.6/§4.7 + §5 fix #450: LinkedList<T>/OwnedLinkedList<T> is
    // IEnumerable<T> but backed by bz_llist_t*, not bz_array_t*.  Wrap via
    // bz_lazy_from_llist so LINQ operators can enumerate it correctly.
    if (isLinkedListTypeName(recvTy)) {
        int64_t esz = collectionElemSize(recvTy);
        auto* fn = declareC("bz_lazy_from_llist", ptrTy(), {ptrTy(), i64()});
        return builder_.CreateCall(fn, {recv, builder_.getInt64(esz)}, "lazy.llist");
    }
    // fix #449: only Array<T>-family (isCollectionTypeName) collections are
    // bz_array_t*-backed.  Do NOT fall through for an unrecognised receiver type
    // (e.g. the i32(0) produced by in-place Array.Reverse()) — that would pass
    // garbage as a pointer to bz_lazy_from_array, causing SIGABRT.
    if (!isCollectionTypeName(recvTy)) return nullptr;
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

    // Only intercept on a collection, lazy-enumerable, or LinkedList receiver.
    auto* recvTy = ctx_.exprType(*ma->target);
    bool collRecv = isCollectionTypeName(recvTy);
    bool lazyRecv = isLazyEnumerableType(recvTy);
    // fix #454 / §5 / §4.6: LinkedList<T> implements IEnumerable<T> but is
    // bz_llist_t*-backed, not bz_array_t*-backed.  lowerToLazyEnumerator already
    // routes it via bz_lazy_from_llist, but the guard here used to exclude it
    // (neither isCollectionTypeName nor isLazyEnumerableType is true for a
    // LinkedList), so Where/Select/ToArray/etc. silently fell through and returned
    // 0 or crashed.  Include it so the lazy operators wrap it correctly.
    bool llRecv = isLinkedListTypeName(recvTy);
    if (!collRecv && !lazyRecv && !llRecv) return nullptr;

    // Element LLVM type for the receiver's element TypeRef.
    auto elemLlvmOf = [&](sema::TypeRef* er) -> llvm::Type* {
        if (er) if (auto* t = lowerTypeRef(er)) return t;
        return i32();
    };

    // ----- ToArray: materialize the (lazy) pipeline into an owned Array<T> ---
    if (m == "ToArray" && call.args.empty()) {
        // stdlib §5 (#651): GroupBy(...).ToArray() — intercept before the
        // generic lazy-materialization path.  The GroupBy source is itself a
        // call expression; detect by checking if it's a CallExpr whose callee
        // is a MemberAccessExpr with member "GroupBy".
        if (auto* srcCall = dynamic_cast<const ast::CallExpr*>(ma->target.get())) {
            if (srcCall->callee) {
                if (auto* srcMa = dynamic_cast<const ast::MemberAccessExpr*>(
                        srcCall->callee.get())) {
                    if (srcMa->member == "GroupBy" && srcMa->target &&
                        srcCall->args.size() == 1 && srcCall->args[0].value) {
                        // srcMa->target is the source collection/lazy.
                        auto* srcRecvTy = ctx_.exprType(*srcMa->target);
                        sema::TypeRef* valElemRef =
                            (srcRecvTy && !srcRecvTy->genericArgs.empty())
                            ? srcRecvTy->genericArgs[0] : nullptr;
                        llvm::Type* valElemTy = valElemRef
                            ? lowerTypeRef(valElemRef) : i32();
                        int64_t val_esz = lazyElemSize(valElemTy,
                            module_->getDataLayout());
                        // Determine key element type from the key lambda's return.
                        sema::TypeRef* keyElemRef = valElemRef; // default: identity
                        auto* callTy = ctx_.exprType(*srcCall);
                        // callTy = IEnumerable<IGrouping<TKey,T>>;
                        // IGrouping genericArgs[0]=TKey.
                        if (callTy && !callTy->genericArgs.empty() &&
                            callTy->genericArgs[0] &&
                            callTy->genericArgs[0]->astNode &&
                            callTy->genericArgs[0]->astNode->name &&
                            !callTy->genericArgs[0]->astNode->name->parts.empty() &&
                            callTy->genericArgs[0]->astNode->name->parts[0] == "IGrouping" &&
                            callTy->genericArgs[0]->genericArgs.size() >= 1) {
                            keyElemRef = callTy->genericArgs[0]->genericArgs[0];
                        }
                        llvm::Type* keyElemTy = keyElemRef
                            ? lowerTypeRef(keyElemRef) : i32();
                        int64_t key_esz = lazyElemSize(keyElemTy,
                            module_->getDataLayout());
                        // Lower the source to a lazy enumerator.
                        sema::TypeRef* dummyElem = nullptr;
                        llvm::Value* srcLazy = lowerToLazyEnumerator(
                            *srcMa->target, dummyElem);
                        if (!srcLazy) return nullptr;
                        // Lower the key lambda to a closure.
                        // Use selector trampoline (kind 1): void(cl, elem, out)
                        // so the key is written as bytes into the output slot.
                        const bool relCl =
                            srcCall->args[0].value &&
                            dynamic_cast<const ast::LambdaExpr*>(
                                srcCall->args[0].value.get()) != nullptr;
                        // lowerLinqLambda is defined AFTER the ToArray block;
                        // inline its logic: set expectedFunctionType_ to a
                        // function (valElemRef)->keyElemRef and lowerExpr.
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
                            closure = lowerExpr(*srcCall->args[0].value);
                            expectedFunctionType_ = saved;
                        }
                        if (!closure) return nullptr;
                        llvm::Function* keyTramp = buildLinqTrampoline(
                            1, valElemTy, keyElemTy);
                        auto* gbFn = declareC("bz_groupby_to_array", ptrTy(),
                            {ptrTy(), i64(), i64(), ptrTy(), ptrTy()});
                        llvm::Value* groups = builder_.CreateCall(gbFn,
                            {srcLazy,
                             builder_.getInt64(val_esz),
                             builder_.getInt64(key_esz),
                             closure,
                             keyTramp},
                            "linq.groupby");
                        if (relCl && closure) emitClosureRelease(closure);
                        return groups;
                    }
                }
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
        {
            const ElementPolicy toArrPol =
                elementPolicyFor(elemRef, /*owningFamily=*/false);
            if (toArrPol.cat == ElementPolicy::Category::RuntimeManaged &&
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
        llvm::Function* eqTramp   = buildLinqTrampoline(3, elemTy2, nullptr, elemRec);
        auto* toSet = declareC("bz_lazy_to_set", ptrTy(),
                               {ptrTy(), i64(), ptrTy(), ptrTy()});
        return builder_.CreateCall(toSet,
            {lazy, builder_.getInt64(esz), hashTramp, eqTramp}, "linq.toset");
    }

    // ----- ToDictionary: materialize into an owned Dictionary<TKey, T> -------
    // stdlib §5: ToDictionary<TKey>(key: (T)->TKey): owned Dictionary<TKey, T>.
    // The one-arg overload uses the element itself as the value; the two-arg form
    // (key, value: (T)->TValue) is not modeled in this JIT milestone (the second
    // arg would require a value-selector lambda).
    if (m == "ToDictionary" && call.args.size() >= 1 && call.args[0].value) {
        sema::TypeRef* elemRef = nullptr;
        llvm::Value* lazy = lowerToLazyEnumerator(*ma->target, elemRef);
        if (!lazy) return nullptr;
        llvm::Type* elemTy2 = elemLlvmOf(elemRef);
        int64_t valEsz = lazyElemSize(elemTy2, module_->getDataLayout());

        // Key type: the lambda (T)->TKey; TKey is its return type.
        sema::TypeRef* keyRefSema = nullptr;
        llvm::Type* keyTy = i32();
        if (auto* lamTy = ctx_.exprType(*call.args[0].value))
            if (lamTy->kind == sema::TypeRef::Kind::Function && lamTy->funcReturn) {
                keyRefSema = lamTy->funcReturn;
                if (auto* t = lowerTypeRef(keyRefSema)) keyTy = t;
            }
        int64_t keyEsz = lazyElemSize(keyTy, module_->getDataLayout());

        // Key closure + trampoline (kind 2 = key: i64(closure, elem_ptr)).
        // Note: lowerLinqLambda is not in scope here (defined later in the isOp
        // section). For ToDictionary the user always annotates the lambda key
        // type explicitly, so lowerExpr suffices.
        llvm::Value* keyClosure = lowerExpr(*call.args[0].value);
        if (!keyClosure) return nullptr;
        llvm::Function* keyTramp = buildLinqTrampoline(2, elemTy2, nullptr);

        // Key hash + equality trampolines for the dict backing.
        const sema::TypeSymbol* keyRec = recordTypeOfRef(keyRefSema);
        llvm::Function* hashTramp = buildLinqHashTrampoline(keyTy, keyRefSema, keyRec);
        llvm::Function* eqTramp   = buildLinqTrampoline(3, keyTy, nullptr, keyRec);

        auto* toDict = declareC("bz_lazy_to_dict", ptrTy(),
            {ptrTy(), i64(), i64(), ptrTy(), ptrTy(), ptrTy(), ptrTy()});
        llvm::Value* result = builder_.CreateCall(toDict,
            {lazy, builder_.getInt64(valEsz), builder_.getInt64(keyEsz),
             keyTramp, keyClosure, hashTramp, eqTramp}, "linq.todict");
        // Release the inline-lambda key closure if we own it (eager materialization
        // — the closure is fully consumed during bz_lazy_to_dict, same as eager
        // LINQ terminals like Count/Sum).
        bool inlineLambda = !call.args.empty() && call.args[0].value &&
            dynamic_cast<const ast::LambdaExpr*>(call.args[0].value.get()) != nullptr;
        if (inlineLambda) emitClosureRelease(keyClosure);
        return result;
    }

    // The deferred operators. Lower the source to a lazy enumerator first.
    // NOTE: Reverse is intercepted ONLY on a lazy receiver — on a concrete
    // collection (Array) it falls through to the in-place Array.Reverse()
    // handler in tryLowerCollectionInstanceCall (§3).
    bool isOp = (m == "Where" || m == "Select" || m == "Take" || m == "Skip" ||
                 m == "Distinct" || m == "OrderBy" || m == "OrderByDescending" ||
                 m == "Concat" || m == "TakeWhile" || m == "SkipWhile" ||
                 m == "SelectMany" || m == "Zip" ||
                 // stdlib §5 Set-like lazy ops (this milestone):
                 m == "Union" || m == "Intersect" || m == "Except" ||
                 // stdlib §5 Ordering secondary key:
                 m == "ThenBy" || m == "ThenByDescending" ||
                 // stdlib §5 lazy Reverse — only on a lazy receiver (not Array):
                 (m == "Reverse" && lazyRecv) ||
                 // stdlib §5 (#651): OfType<U>() — lazy identity/type-filter.
                 m == "OfType");
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
        int64_t innerEsz = lazyElemSize(innerElemTy, module_->getDataLayout());
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
        llvm::Function* eqTramp   = buildLinqTrampoline(3, elemTy, nullptr, elemRec);
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
    // stdlib §5 (#651): OfType<U>() — type-filter lazy operator.
    // For value-type sources (monomorphic): U must equal T (statically enforced
    // by sema since Bezel has no runtime type tags for value types), so OfType
    // is an identity — wrap source as a Where-all (predicate always true).
    // The element size is unchanged (U == T).
    if (m == "OfType" && call.args.empty()) {
        // Build a Where with an always-true predicate trampoline.
        // kind-0 trampoline: i32(ptr closure, ptr elem) returns 1 always.
        llvm::FunctionType* fty = llvm::FunctionType::get(i32(),
            {ptrTy(), ptrTy()}, false);
        auto* alwaysTrueFn = llvm::Function::Create(fty,
            llvm::Function::InternalLinkage, "bz$oftype$tramp", module_.get());
        auto savedIP = builder_.saveIP();
        llvm::Function* savedFn = currentFn_;
        currentFn_ = alwaysTrueFn;
        auto* entry = llvm::BasicBlock::Create(llvm_, "entry", alwaysTrueFn);
        builder_.SetInsertPoint(entry);
        builder_.CreateRet(builder_.getInt32(1));
        builder_.restoreIP(savedIP);
        currentFn_ = savedFn;
        // bz_lazy_where with nullptr closure and alwaysTrueFn → pass-through.
        auto* whereFn = declareC("bz_lazy_where", ptrTy(), {ptrTy(), ptrTy(), ptrTy()});
        return builder_.CreateCall(whereFn,
            {src, llvm::ConstantPointerNull::get(llvm::PointerType::get(llvm_, 0)),
             alwaysTrueFn},
            "linq.oftype");
    }
    return nullptr;
}


// ---------------------------------------------------------------------------
// per §4.6/§4.7: LinkedList<T> / OwnedLinkedList<T> node-based API codegen.
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
    // LinkedList<T> / OwnedLinkedList<T> method lowerings.
    // -----------------------------------------------------------------------
    if (!isLinkedListTypeName(recvTy)) return nullptr;

    // Element size for this instantiation.
    int64_t esz = collectionElemSize(recvTy);
    bool isOwning = recvTy && recvTy->astNode && recvTy->astNode->name &&
                    !recvTy->astNode->name->parts.empty() &&
                    recvTy->astNode->name->parts[0] == "OwnedLinkedList";

    llvm::Value* recv = lowerExpr(*ma->target);
    if (!recv) return nullptr;

    // Element LLVM type (for loading values from nodes).
    llvm::Type* elemLLTy = i32();
    if (recvTy && !recvTy->genericArgs.empty() && recvTy->genericArgs[0])
        elemLLTy = lowerTypeRef(recvTy->genericArgs[0]);

    // #602 / §28.11 v0.134: hoist one element-slot policy lookup for LinkedList
    // operations that need per-element equality (Find/Remove/Contains) or disposal
    // (OwnedLinkedList RemoveFirst/RemoveLast/Clear/Remove).
    // LinkedList is always in the borrowing family (no Owned* string elements);
    // OwnedLinkedList is in the owning family (isOwning flag above).
    const sema::TypeRef* llElemRef =
        (recvTy && !recvTy->genericArgs.empty()) ? recvTy->genericArgs[0] : nullptr;
    const ElementPolicy llElemPol = elementPolicyFor(llElemRef, isOwning);
    // llElemIsClass: class-or-interface element — per §28.11 v0.135 fix #619:
    // elementPolicyFor now returns ClassOwned/ClassBorrow for interface elements.
    bool llElemIsClass =
        llElemPol.cat == ElementPolicy::Category::ClassOwned ||
        llElemPol.cat == ElementPolicy::Category::ClassBorrow;

    // --- AddFirst(item): LinkedListNode<T> ---
    if (m == "AddFirst" && call.args.size() == 1 && call.args[0].value) {
        llvm::Value* v = lowerExpr(*call.args[0].value);
        if (!v) return nullptr;
        llvm::AllocaInst* slot = entryAlloca(v->getType());
        builder_.CreateStore(v, slot);
        auto* fn = declareC("bz_llist_add_first", ptrTy(),
                            {ptrTy(), ptrTy(), i64()});
        return builder_.CreateCall(fn, {recv, slot, builder_.getInt64(esz)},
                                   "ll.addFirst");
    }
    // --- AddLast(item): LinkedListNode<T> ---
    if (m == "AddLast" && call.args.size() == 1 && call.args[0].value) {
        llvm::Value* v = lowerExpr(*call.args[0].value);
        if (!v) return nullptr;
        llvm::AllocaInst* slot = entryAlloca(v->getType());
        builder_.CreateStore(v, slot);
        auto* fn = declareC("bz_llist_add_last", ptrTy(),
                            {ptrTy(), ptrTy(), i64()});
        return builder_.CreateCall(fn, {recv, slot, builder_.getInt64(esz)},
                                   "ll.addLast");
    }
    // --- AddBefore(node, item): LinkedListNode<T> ---
    if (m == "AddBefore" && call.args.size() == 2 &&
        call.args[0].value && call.args[1].value) {
        llvm::Value* node = lowerExpr(*call.args[0].value);
        llvm::Value* v    = lowerExpr(*call.args[1].value);
        if (!node || !v) return nullptr;
        llvm::AllocaInst* slot = entryAlloca(v->getType());
        builder_.CreateStore(v, slot);
        auto* fn = declareC("bz_llist_add_before", ptrTy(),
                            {ptrTy(), ptrTy(), ptrTy(), i64()});
        return builder_.CreateCall(fn,
            {recv, node, slot, builder_.getInt64(esz)}, "ll.addBefore");
    }
    // --- AddAfter(node, item): LinkedListNode<T> ---
    if (m == "AddAfter" && call.args.size() == 2 &&
        call.args[0].value && call.args[1].value) {
        llvm::Value* node = lowerExpr(*call.args[0].value);
        llvm::Value* v    = lowerExpr(*call.args[1].value);
        if (!node || !v) return nullptr;
        llvm::AllocaInst* slot = entryAlloca(v->getType());
        builder_.CreateStore(v, slot);
        auto* fn = declareC("bz_llist_add_after", ptrTy(),
                            {ptrTy(), ptrTy(), ptrTy(), i64()});
        return builder_.CreateCall(fn,
            {recv, node, slot, builder_.getInt64(esz)}, "ll.addAfter");
    }
    // --- PopFirst(): owned T / PopLast(): owned T (OwnedLinkedList only) ---
    // fix #642 / stdlib §4.7: transfer ownership of the first/last element to the
    // caller; remove WITHOUT disposal.  Only valid on OwnedLinkedList<T> (the
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
    // --- RemoveFirst() ---
    if (m == "RemoveFirst" && call.args.empty()) {
        // #602 / §28.11 v0.135 fix #619: use llElemPol.disposerThunk (hoisted above).
        // Interface elements now produce a non-null disposerThunk via ClassOwned.
        llvm::Function* elemDisp = isOwning ? llElemPol.disposerThunk : nullptr;
        llvm::Value* disp = elemDisp
            ? llvm::cast<llvm::Value>(elemDisp)
            : llvm::ConstantPointerNull::get(ptrTy());
        auto* fn = declareC("bz_llist_remove_first", llvm::Type::getVoidTy(llvm_),
                            {ptrTy(), ptrTy()});
        builder_.CreateCall(fn, {recv, disp});
        return llvm::ConstantInt::get(i32(), 0);
    }
    // --- RemoveLast() ---
    if (m == "RemoveLast" && call.args.empty()) {
        // #602 / §28.11 v0.134: use llElemPol.disposerThunk (hoisted above).
        llvm::Function* elemDisp = isOwning ? llElemPol.disposerThunk : nullptr;
        llvm::Value* disp = elemDisp
            ? llvm::cast<llvm::Value>(elemDisp)
            : llvm::ConstantPointerNull::get(ptrTy());
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
        llvm::Function* elemDisp = isOwning ? llElemPol.disposerThunk : nullptr;
        llvm::Value* disp = elemDisp
            ? llvm::cast<llvm::Value>(elemDisp)
            : llvm::ConstantPointerNull::get(ptrTy());
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
    // --- Clear() --- disposes all elements (OwnedLinkedList) and frees all
    // nodes, then re-allocates an empty list. Simplest: remove all from front.
    // We implement by calling bz_llist_free then re-alloc would break existing
    // pointer. Instead: walk and free each node via remove_first repeatedly.
    // Cleanest: add bz_llist_clear to runtime. For now, implement as a loop.
    if (m == "Clear" && call.args.empty()) {
        // #602 / §28.11 v0.135 fix #619: use llElemPol.disposerThunk (hoisted above).
        // Interface elements now produce a non-null disposerThunk via ClassOwned.
        llvm::Function* elemDisp = isOwning ? llElemPol.disposerThunk : nullptr;
        llvm::Value* disp = elemDisp
            ? llvm::cast<llvm::Value>(elemDisp)
            : llvm::ConstantPointerNull::get(ptrTy());
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
    // --- Remove(item): bool — per §4.6/§4.3 IOwnedCollection ---
    // fix #565 / §4.6/§8.3/§9.1: the previous bz_llist_find path used memcmp on
    // raw element bytes, comparing bz_string* pointer values (not content) for
    // string elements and raw pointer bytes for records with string fields.
    // Mirror the #562 fix for LinkedList<T>.Contains: walk node-by-node applying
    // emitRemoveElementEquality for per-element comparison, then call
    // bz_llist_remove_node on the matching node (unlinks + optionally disposes).
    // Ownership semantics are preserved: OwnedLinkedList passes its disposer;
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
        // OwnedLinkedList disposes the element; LinkedList borrows, no disposal.
        // #602 / §28.11 v0.135 fix #619: use llElemPol.disposerThunk from policy
        // hoist.  Interface elements now produce a non-null disposerThunk.
        llvm::Function* elemDisp =
            isOwning ? llElemPol.disposerThunk : nullptr;
        llvm::Value* disp = elemDisp
            ? llvm::cast<llvm::Value>(elemDisp)
            : llvm::ConstantPointerNull::get(ptrTy());
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
        // `in` operator LinkedList path in LlvmBackend_Exprs.cpp §2764).
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
    std::string_view rcn = recvTy->astNode->name->parts[0];

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
    // -----------------------------------------------------------------------
    if (rcn == "Array" && (m == "AsSpan" || m == "AsReadOnlySpan")) {
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
    // ArraySpan<T> / ReadOnlyArraySpan<T> instance method calls.
    // -----------------------------------------------------------------------
    if (rcn != "ArraySpan" && rcn != "ReadOnlyArraySpan") return nullptr;
    bool isWritable = (rcn == "ArraySpan");

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

    // -- Length / IsEmpty -------------------------------------------------------
    if (m == "Length" && call.args.empty()) {
        return loadLen();  // i32
    }
    if (m == "IsEmpty" && call.args.empty()) {
        return builder_.CreateICmpEQ(loadLen(), builder_.getInt32(0), "sp.empty");
    }

    // -- Slice(start: int, length: int) ----------------------------------------
    if (m == "Slice" && call.args.size() == 2 &&
        call.args[0].value && call.args[1].value) {
        llvm::Value* src  = loadSource();
        llvm::Value* base = loadStart();
        llvm::Value* slen = loadLen();
        llvm::Value* start = lowerExpr(*call.args[0].value);
        llvm::Value* slen2 = lowerExpr(*call.args[1].value);
        if (!start || !slen2) return nullptr;
        if (!start->getType()->isIntegerTy(32))
            start = builder_.CreateTrunc(start, i32());
        if (!slen2->getType()->isIntegerTy(32))
            slen2 = builder_.CreateTrunc(slen2, i32());
        // bounds check against this span's length
        llvm::Value* startNeg = builder_.CreateICmpSLT(start, builder_.getInt32(0));
        llvm::Value* startOOB = builder_.CreateICmpSGT(start, slen);
        emitGuardThrow(builder_.CreateOr(startNeg, startOOB),
                       "IndexOutOfRangeException");
        llvm::Value* avail = builder_.CreateSub(slen, start);
        llvm::Value* slen2Neg = builder_.CreateICmpSLT(slen2, builder_.getInt32(0));
        llvm::Value* slen2OOB = builder_.CreateICmpSGT(slen2, avail);
        emitGuardThrow(builder_.CreateOr(slen2Neg, slen2OOB),
                       "IndexOutOfRangeException");
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
    if (m == "SequenceEqual" && call.args.size() == 1 && call.args[0].value) {
        llvm::Value* otherVal = lowerExpr(*call.args[0].value);
        if (!otherVal) return nullptr;
        auto* otherSlot = entryAlloca(spanSt);
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

    return nullptr;
}

// ---------------------------------------------------------------------------
// §30.14/§30.15 (#600 phase c): Channel<T> and Domain.Spawn codegen.
//
// Handles:
//   Channel<T>:
//     NewSender()               → bz_channel_new_sender(ch)        → ptr (ChannelSender<T>)
//     Send(owned item)          → bz_channel_send(ch, item)        → void
//     Receive()                 → bz_channel_receive(ch, cce_vtable) → Task/OwnedTask<T>
//     TryReceive(out item)      → bz_channel_try_receive(ch, &slot)  → bool
//     Count                     → bz_channel_count(ch)             → int
//     IsClosed                  → bz_channel_is_closed(ch)         → bool
//     Dispose()                 → bz_channel_dispose(ch, cce_vtable)→ void
//   ChannelSender<T>:
//     Send(owned item)          → bz_channel_sender_send(tx, item) → void
//     Dispose()                 → bz_channel_sender_release(tx, cce_vtable) → void
//   Domain.Spawn(entry, arg?)   → bz_domain_spawn(fn, arg, dfe_vtable) → OwnedTask<R>
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
                                    {ptrTy(), ptrTy(), ptrTy()});
                builder_.CreateCall(fn, {ch, itemArg, getCCEVtable()});
                emitPendingCheck();  // per fix #634: bz_channel_send sets bz_exc on closed channel
                return nullptr;  // void — handled; fallthrough is benign (no symbol match)
            }

            // Receive(): returns a Task<T> or OwnedTask<T>.
            if (mname == "Receive" && call.args.empty()) {
                auto* fn = declareC("bz_channel_receive", ptrTy(),
                                    {ptrTy(), ptrTy()});
                return builder_.CreateCall(fn, {ch, getCCEVtable()}, "ch.recv");
            }

            // TryReceive(out owned item: T?) → bool.
            // The out-var slot was pre-created by ensureOutVarSlots.
            if (mname == "TryReceive" && call.args.size() == 1 &&
                call.args[0].value) {
                // The out argument's address.
                llvm::Type* outEty = nullptr;
                llvm::Value* outSlot = lvalueAddr(*call.args[0].value, outEty);
                if (!outSlot) {
                    // Fallback: create a temporary slot.
                    outSlot = entryAlloca(ptrTy());
                }
                // Allocate a ptr-sized slot for the raw item pointer.
                llvm::AllocaInst* rawSlot = entryAlloca(ptrTy());
                builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()),
                                     rawSlot);
                auto* fn = declareC("bz_channel_try_receive", i32(),
                                    {ptrTy(), ptrTy()});
                llvm::Value* ok = builder_.CreateCall(fn, {ch, rawSlot},
                                                      "ch.tryrecv");
                // If ok: copy rawSlot into outSlot (the caller's out-var).
                auto* okBB   = llvm::BasicBlock::Create(llvm_, "tryrecv.ok",
                                                         currentFn_);
                auto* contBB = llvm::BasicBlock::Create(llvm_, "tryrecv.cont",
                                                         currentFn_);
                llvm::Value* isOk = builder_.CreateICmpNE(ok, builder_.getInt32(0));
                builder_.CreateCondBr(isOk, okBB, contBB);
                builder_.SetInsertPoint(okBB);
                llvm::Value* rawItem = builder_.CreateLoad(ptrTy(), rawSlot);
                // Decode item: for value types, ptrtoint + trunc; for ptr types
                // store as-is. The element type decides the decoding.
                builder_.CreateStore(rawItem, outSlot);
                builder_.CreateBr(contBB);
                builder_.SetInsertPoint(contBB);
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
                                    {ptrTy(), ptrTy(), ptrTy()});
                builder_.CreateCall(fn, {tx, itemArg, getCCEVtable()});
                emitPendingCheck();  // per fix #634: signals ChannelClosedException via bz_exc
                return nullptr;
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

            // Resolve the entry method symbol.
            sema::MethodSymbol* entryMs =
                dynamic_cast<sema::MethodSymbol*>(ctx_.exprSymbol(*entryExpr));
            if (!entryMs) return nullptr;

            // Find the actual LLVM function for the method.
            auto mfIt = methodFns_.find(entryMs);
            if (mfIt == methodFns_.end()) return nullptr;
            llvm::Function* origFn = mfIt->second;

            // Build the adapter: `void* adapter(void* arg)`.
            // The adapter's body:
            //   - If entry has 1 param: call origFn(arg)
            //   - If entry has 0 params: call origFn()
            //   - Return value: origFn's result cast to void* (or null for void)
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

                std::vector<llvm::Value*> callArgs;
                std::size_t nArgs = origFn->arg_size();
                if (nArgs == 1) {
                    // Entry takes one argument: interpret arg as the param type.
                    llvm::Type* paramTy = origFn->getArg(0)->getType();
                    llvm::Value* typedArg;
                    if (paramTy->isPointerTy()) {
                        typedArg = argParam;
                    } else if (paramTy->isIntegerTy(64)) {
                        typedArg = builder_.CreatePtrToInt(argParam, i64());
                    } else if (paramTy->isIntegerTy(32)) {
                        llvm::Value* i = builder_.CreatePtrToInt(argParam, i64());
                        typedArg = builder_.CreateTrunc(i, i32());
                    } else {
                        typedArg = builder_.CreatePtrToInt(argParam, i64());
                    }
                    callArgs.push_back(typedArg);
                }
                // else: nArgs == 0 — no-arg entry, no call args.

                llvm::Value* ret = builder_.CreateCall(origFn, callArgs);
                if (origFn->getReturnType()->isVoidTy()) {
                    builder_.CreateRet(llvm::ConstantPointerNull::get(ptrTy()));
                } else if (origFn->getReturnType()->isPointerTy()) {
                    builder_.CreateRet(ret);
                } else {
                    // Value result: inttoptr.
                    llvm::Value* wide = ret;
                    if (!wide->getType()->isIntegerTy(64))
                        wide = builder_.CreateZExt(wide, i64());
                    builder_.CreateRet(
                        builder_.CreateIntToPtr(wide, ptrTy()));
                }

                // Restore emitter state.
                currentFn_   = savedFn;
                currentThis_ = savedThis;
                if (savedBB) builder_.SetInsertPoint(savedBB);
            }

            // Lower the optional argument to bz_domain_spawn.
            llvm::Value* argVal = llvm::ConstantPointerNull::get(ptrTy());
            if (call.args.size() >= 2 && call.args[1].value) {
                llvm::Value* argExprV = lowerExpr(*call.args[1].value);
                if (argExprV) {
                    if (argExprV->getType()->isPointerTy()) {
                        argVal = argExprV;
                    } else if (argExprV->getType()->isIntegerTy(64)) {
                        argVal = builder_.CreateIntToPtr(argExprV, ptrTy(),
                                                         "spawn.arg.cast");
                    } else if (argExprV->getType()->isIntegerTy()) {
                        argVal = builder_.CreateIntToPtr(
                            builder_.CreateZExt(argExprV, i64()), ptrTy(),
                            "spawn.arg.cast");
                    } else {
                        argVal = builder_.CreateIntToPtr(
                            builder_.CreateBitCast(argExprV, i64()), ptrTy(),
                            "spawn.arg.cast");
                    }
                }
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


}  // namespace bezel::codegen
