#include "codegen/LlvmBackend_impl.hpp"
#include "sema/GenericParams.hpp"  // fix #3143: the ONE both-sources generic-param walk (§14.1/§12.9/§2.5)

// §4.4 fix #521: decimal compile-time literal parser (defined in bezel_runtime.cpp).
extern "C" void bz_decimal_parse_cstr(const char* s, void* out,
                                       void* ove_vtable, void* fe_vtable);

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
    //
    // fix #3352 (§8.6 / §23.4 / §28.3 / §28.9, v0.240): capture an inline
    // §8.6 box mint from this argument's lowering. The inline
    // indexer/property expression-body path (`lowerIndexerGet` line 2141 /
    // `lowerVirtualPropertyGet`'s `boxForTarget` at ~line 5505) mints a
    // fresh box for a value-typed body delivered where `Object` is expected
    // and records the mint in `lastBoxMint_`. #2865's `lowerTypeTest` is the
    // only existing reader; the call-arg position (`Console.WriteLine(e[k])`
    // where `e: E` and `E.property [i: int]: Object => i;`) reaches here
    // with `arg` IS the inline mint (`arg == lastBoxMint_`). Save/clear/
    // restore isolates THIS argument's mint from any sibling
    // `boxForTarget`, exactly as the existing #2952 call-arg
    // `boxForTarget`-site pattern in `LlvmBackend_Assign.cpp:11296`. The
    // discharge fires inside the `releaseRender` arm below, gated on
    // `inlineBoxMint && arg == lastBoxMint_` — see that arm's note for the
    // over-release safety.
    llvm::Value* savedBoxMint = lastBoxMint_;
    lastBoxMint_ = nullptr;
    llvm::Value* arg = lowerExpr(*call.args[0].value);
    bool inlineBoxMint = (lastBoxMint_ && arg == lastBoxMint_);
    lastBoxMint_ = savedBoxMint;
    // fix #3104 (§4.7 / §8.5 / stdlib §2.1, v0.255): `lowerExpr` for a unit-
    // typed argument returns a `Value*` with LLVM `void` type, NOT a null
    // pointer — `builder_.CreateCall(callee, args)` at Calls2.cpp:10688
    // produces a CallInst Value for the side effect, and `void`-typed
    // instruction Values are non-null but unusable as a `bz_string*` arg
    // to `bz_string_to_utf8z`. The pre-fix #3079 `if (!arg)` guard was
    // structured around the (incorrect) premise that lowerExpr returns
    // nullptr for any unlowered shape — true for built-in receiver calls
    // and unconstructed `new`, FALSE for unit-typed calls (this shape).
    // That off-by-one is exactly the asymmetry the issue names: a unit-
    // typed `arg` IS non-null with type void, the guard's `if (!arg)` is
    // false, and the post-guard code reaches `emitConsoleWrite` with a
    // void-typed `strPtr`, which the IR verifier rejects with "Call
    // parameter type does not match function signature" — measured on
    // first cut (see also the sibling branch (b) at #3079, where the
    // built-in receiver call hits the same null-not-void gap).
    //
    // §4.7 admits the boxing, §8.5 gives the rendering `()`, and stdlib
    // §2.1 declares `Console.WriteLine(value: Object)` — so the spec
    // gives the argument a well-defined answer. Replace whatever
    // `arg` holds with the §8.5 sentinel BEFORE the guard runs: a
    // bitcast'd `lowerStringLiteral` whose value is a static-rc global,
    // exactly the same shape the nullable-value branch above uses for
    // its `"null"` literal at line 360 (phied into a `ptrTy()` slot
    // before `emitConsoleWrite`). The §2875 "+1 discharge" rule
    // explicitly exempts borrowed statics, so no `releaseRender` flag.
    if (call.args[0].value) {
        const sema::TypeRef* preTy = ctx_.exprType(*call.args[0].value);
        if (preTy && preTy->kind == sema::TypeRef::Kind::Unit) {
            arg = builder_.CreateBitCast(
                // fix #3442: §8.5's unit rendering, from its one spelling.
                lowerStringLiteral(kUnitToStringSource, false), ptrTy(),
                "console.arg.unit");
        }
    }
    if (!arg) {
        // fix #3079 (§8.5 / §21, v0.249): THE THIRD VALUE-DEMANDING DEGRADE —
        // the one #2966 named as a residual ("the three OTHER value-demanding
        // degrades are deliberately NOT covered") and the only one of the three
        // whose degrade is not a wrong value but the DISAPPEARANCE OF THE WHOLE
        // STATEMENT.
        //
        // This `return false` says "not a console call" about a call this
        // function has ALREADY positively identified as `Console[.Error|.Out]
        // .Write(Line)` with exactly one argument. Nothing downstream matches
        // that shape, so lowerCallDispatch runs out of arms and its `if (!ms)`
        // chain returns nullptr — and a nullptr call result in statement
        // position emits nothing at all. Not even `Console.NewLine`: exit 0, no
        // diagnostic at ANY severity, output silently missing.
        //
        // MEASURED on 309356e, three times over, on the ORDINARY forms this
        // file's other notes each measured once:
        //   • `Console.WriteLine(K.ToString())` for `public const K: int := 7;`
        //     — printed nothing (#3079 shape (i); root cause: sema left the
        //     bare `const` receiver untyped, fixed in ExprTypecheck_Exprs.cpp).
        //   • `Console.WriteLine(a.Length.ToString())` for `var a: [16]int;` —
        //     printed nothing (#3079 shape (ii); sema left `a.Length` untyped,
        //     fixed in ExprTypecheck_Calls.cpp).
        //   • `Console.WriteLine(w?.GetHashCode())` on a non-null `Widget?` —
        //     the SAME disappearance recorded verbatim at
        //     LlvmBackend_Calls2.cpp's #2982 note, and again at its #2469 note.
        // Both #3079 shapes are typed by this commit, so neither reaches here
        // any more. This guard is for the NEXT one: a receiver shape nobody has
        // written down yet must not be able to delete a user's output line in
        // silence.
        //
        // The severity split is #2434/#2491/#2935's, reused rather than
        // re-derived — the shared `builtinReceiverOfMemberCall` /
        // `unconstructedBuiltinNewName` predicates, not a fourth copy of them
        // (#2924's four-way-mirror finding):
        //   (a) a UNIT-typed argument is EXEMPT and silent — `()` lowers to
        //       LLVM `void` (#2915), so having no lowered value IS the type's
        //       representation, not a lowering failure.
        //   (b) a member call on a BUILT-IN receiver, or a `new` on one, that
        //       neither sema nor codegen could make sense of ⇒ located Error.
        //       The conjunction #2935 requires holds here for the same reason
        //       it holds at a local declaration and at §21.7's `return expr;`:
        //       a value is DEMANDED. `Console.Write(Line)` renders its one
        //       argument (stdlib §2.1); an argument that yields nothing cannot
        //       be rendered.
        //   (c) everything else ⇒ the `internal:` Warning, NOT an Error — the
        //       §3/#605 permissive surface (`Console.WriteLine(o.Anything())`
        //       for `o: Object`) is accepted by sema BY DESIGN and reaches
        //       here, so an Error would false-reject legal programs exactly as
        //       #2434's lit-corpus check found for the local-decl twin.
        //       `Driver::compileExecutableObject` prints every diagnostic
        //       whatever its severity, so the Warning is fully visible on
        //       stderr without failing the build.
        // All three are guarded on `!hasErrors()` for message quality: lowerExpr
        // has sites that return nullptr AFTER reporting their own specific
        // Error, and stacking a generic note on those is noise.
        //
        // The `return false` itself is UNCHANGED — deliberately. Reporting is
        // what closes the silence; re-routing the dispatch would additionally
        // claim no later arm can ever serve this shape, which this fix has no
        // evidence for and does not need.
        const ast::Expr* argE = call.args[0].value.get();
        const sema::TypeRef* aTy = ctx_.exprType(*argE);
        const bool unitArg = aTy && aTy->kind == sema::TypeRef::Kind::Unit;
        // fix #3104: a unit-typed argument is now handled by the pre-guard
        // detection above (it overwrites `arg` with a bitcast'd §8.5
        // sentinel, so `if (!arg)` is false and we never reach this guard
        // for the unit case). The short-circuits below (`unitArg ? "" : …`)
        // and the local `unitArg` capture are KEPT for symmetry with the
        // original three-way severity split, even though with #3104 the
        // unit-shaped arm never reports — the predicate helpers
        // (`builtinReceiverOfMemberCall`, `unconstructedBuiltinNewName`)
        // are non-trivial to evaluate on a unit-typed receiver, and the
        // "diagnosed at most once" rule the original guard documented holds
        // vacuously when the arm is unreachable.
        std::string argMember, argAppliedMember;
        const std::string argBuiltinRecv =
            unitArg ? std::string()
                    : builtinReceiverOfMemberCall(argE, &argMember,
                                                  &argAppliedMember);
        const std::string argBuiltinNew =
            (unitArg || !argBuiltinRecv.empty())
                ? std::string()
                : unconstructedBuiltinNewName(argE);
        const char* siteClause = isWriteLine ? "supply this `Console.WriteLine` "
                                               "argument"
                                             : "supply this `Console.Write` "
                                               "argument";
        if (!unitArg && !argBuiltinRecv.empty() && !ctx_.diagnostics().hasErrors()) {
            ctx_.diagnostics().report(
                Severity::Error, argE->span.begin,
                "`" + argMember + "` on built-in type `" + argBuiltinRecv +
                "` produced no value, so it cannot " + siteClause + ". `" +
                argBuiltinRecv + "` has no user declaration, so its member "
                "surface is exactly what the standard library documents for "
                "it: either `" + argMember + "` is not on that surface (a "
                "misspelling), or it is documented but yields nothing here "
                "(#2935 \xE2\x80\x94 if it is documented and should yield a "
                "value, it is unimplemented, #2457)" +
                builtinMemberChainNote(argMember, argAppliedMember));
        } else if (!argBuiltinNew.empty() && !ctx_.diagnostics().hasErrors()) {
            ctx_.diagnostics().report(
                Severity::Error, argE->span.begin,
                unconstructedBuiltinNewNote(argBuiltinNew, siteClause));
        } else if (!ctx_.diagnostics().hasErrors()) {
            ctx_.diagnostics().report(
                Severity::Warning, argE->span.begin,
                std::string("internal: the argument of this `Console.") +
                (isWriteLine ? "WriteLine" : "Write") +
                "` failed to lower to a value, so the whole statement emits "
                "nothing \xE2\x80\x94 not even the line terminator (stdlib "
                "\xC2\xA7""2.1 / \xC2\xA7""21.9) \xE2\x80\x94 #3079");
        }
        // fix #3104 (§4.7 / §8.5 / stdlib §2.1, v0.255): with the pre-guard
        // unit detection above, the unit case no longer enters this guard
        // (`arg` was overwritten with the §8.5 sentinel, so `if (!arg)`
        // is false). The unitArg check the previous cut hoisted into this
        // return is therefore gone — every entry here is a non-unit
        // failure that the diagnostic just reported, and `return false`
        // is uniformly correct. The "skip unit" predicate was load-bearing
        // only when the unit case routed through here, which it does not.
        return false;
    }
    auto* argTy = ctx_.exprType(*call.args[0].value);
    llvm::Value* strPtr = arg;
    // issue #421: a NUMERIC/char primitive conversion (emitPrimitiveToString's
    // snprintf path) heap-allocates BOTH the bz_string object and its data
    // buffer solely to feed this write; nothing else references it, so it must
    // be reclaimed after the write or it leaks (~26 bytes/call for
    // `WriteLine(7)`).
    //
    // fix #2875 / §8.5 / §28.9 (v0.240): reclaim it with the RC-AWARE release,
    // never a raw free(), and key that on the PROPERTY "the console converted
    // the argument" rather than on an enumeration of primitive kinds. The
    // rendered string is a `+1` the console owns and discharges exactly once
    // (the same contract #2066 records for the record-field render path) — but
    // "+1" does not imply "a private malloc". Three of `emitPrimitiveToString`'s
    // `P::Object` arms hand back something free() destroys or corrupts:
    //   • a CLASS receiver renders §8.5's "the type's qualified name", an
    //     immortal string LITERAL — free() on a constant global ABORTED the
    //     process ("free(): invalid pointer");
    //   • a STRING receiver gets the RECEIVER back (stdlib §9.6 `String`:
    //     "public pure override method ToString(): string;  // returns this"),
    //     retained since #2846 — free() destroyed the caller's own live string,
    //     silently (a later read SEGV'd in bz_string_to_utf8z);
    //   • a boxed `bool` or a boxed `enum` with the default rendering reaches
    //     the box thunk's sentinel answer ("True"/"False", the type's name) —
    //     a literal again.
    // The old predicate excluded exactly `String` and `Bool` by name, so
    // `Object` — which is none of the kinds it was written for — fell through
    // to the raw free. `bz_string_release` is the one discharge correct for
    // every arm: a no-op on a sentinel or null, a decrement on a shared
    // reference, and precisely the `free(data); free(obj)` pair on a private
    // rc==1 render. `P::String` never reaches the conversion at all (the branch
    // below skips it), so the BORROWED string argument is still left to the
    // #1031 producer arm after the write.
    bool releaseRender = false;
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
        // fix #2880 / §8.5 / §28.9 (v0.240): DISCHARGE this render too. The old
        // note here said releasing a PHI result "requires a second hasValue
        // check"; since #2875 the discharge is `emitStringRelease`, which is
        // rc-aware and sentinel-safe, so no second check is needed — one
        // unconditional release covers both arms:
        //   • the `null` arm is `lowerStringLiteral("null")`, whose rc is the
        //     #422 sentinel — release is a NO-OP, not a free of a constant
        //     global (the hazard the raw free() of the pre-#2875 code posed);
        //   • the hasValue arm is `emitValueToString` on the NON-nullable inner
        //     type, which #2066 records as "a fresh +1 producer" for every
        //     value-typed rendering (`bz_int_to_string`, `bz_double_to_string`,
        //     `bz_char_to_string`, `bz_decimal_to_string`, a nested record's
        //     final concat) and a sentinel literal for the rest (`bool` →
        //     "True"/"False", an enum without a user `ToString` → §8.5's type
        //     name). Release is exactly-once on the first group, a no-op on the
        //     second.
        // The one shape #2066 calls a BORROW — `string`'s identity render
        // (stdlib §9.6 `String.ToString`: "returns this") — cannot reach here:
        // `isNullableValueTy` admits only nullable PRIMITIVE-value / enum /
        // record / choice / value-builtin types, and `string?` is a nullable
        // REFERENCE (a plain `ptr`, so both this branch's guards fail). Nor can
        // a nullable CLASS or INTERFACE, so the callee-side unknown #2879
        // records for an `Object`/class `ToString` is structurally out of reach
        // of this branch. Verified by re-reading every source after the write.
        //
        // Falls through to the SHARED write+discharge tail instead of doing its
        // own — one write site, one discharge site.
        releaseRender = true;
    }
    // `strPtr == arg` — i.e. no conversion has happened yet. The nullable-value
    // branch above is the one path that leaves this block with a converted
    // `strPtr`, and it must NOT be re-converted: `emitPrimitiveToString` on the
    // raw `{i8; T}` struct is exactly the #581/#583 crash/misprint that branch
    // exists to prevent.
    if (argTy && strPtr == arg) {
        if (argTy->kind == sema::TypeRef::Kind::Primitive &&
            argTy->primitive != sema::PrimitiveType::String) {
            strPtr = emitPrimitiveToString(arg, argTy->primitive);
            // fix #2875: the render is a `+1` this call owns — discharge it
            // rc-aware below. `strPtr != arg` is the whole condition: nothing
            // here needs to know WHICH primitive produced it.
            releaseRender = (strPtr != arg);
        } else if (arg->getType()->isStructTy() &&
                   (isSpanLikeTypeName(argTy) || isRangeType(argTy) ||
                    (argTy->kind == sema::TypeRef::Kind::Named &&
                     !argTy->declaration && argTy->astNode &&
                     argTy->astNode->name &&
                     !argTy->astNode->name->parts.empty() &&
                     argTy->astNode->name->parts[0] == "Range"))) {
            // fix #2661 (spec §8.5 / stdlib §2.1, v0.252): a SPAN or `Range<T>`
            // argument. `Console.Write(Line)` "renders its one argument", and
            // since §8.5's defaults are universal that argument now HAS a
            // rendering. Before this the value fell past every arm and the raw
            // `bz.arrayspan` / `bz.range` struct was handed to
            // `bz_string_to_utf8z` as if it were a `%bz.string` — MEASURED as
            // an IR-verifier abort ("Call parameter type does not match
            // function signature"), i.e. a hard miscompile, not a wrong value.
            //
            // WHY IT FELL PAST — and it is NOT one reason for both. A struct
            // matches no primitive kind, so the Primitive arm above never sees
            // either. Below, the Named arm is gated on `argTy->declaration`: a
            // SPAN has none (`isSpanLikeTypeName` requires exactly that), so it
            // never reached the arm at all; a `Range<T>` DOES carry a
            // declaration — the builtin stub, which is what `isRangeType` keys
            // on — so it reached the arm and was mis-rendered there as a
            // name-only default instead. This test therefore has to run BEFORE
            // the Named arm, not merely alongside it.
            //
            // Delegated to `emitValueToString`, the one renderer the explicit
            // call and the §18.5 hole also reach.
            strPtr = emitValueToString(arg, argTy);
            if (!strPtr) return false;
            releaseRender = true;   // a fresh +1 concat accumulator
        } else if (argTy->kind == sema::TypeRef::Kind::Named &&
                   argTy->declaration) {
            // fix #2878 (§8.5 / §28.3 / §28.9, v0.240): route the whole Named
            // arm through `emitValueToString` and discharge the result with
            // `releaseRender = true`. The previous ad-hoc member walk had
            // three defects in one branch (#2878's body, measured on 7c466ee):
            //   (a) INTERFACE receivers — interfaces declare no body, so no
            //       `ToString()` MethodSymbol was findable and the branch fell
            //       to the literal `spec85NameOnlyDefaultSpelling(ts)`
            //       placeholder, printing the STATIC interface name. §8.5 says
            //       `ToString` is invoked "via virtual dispatch" — the dynamic
            //       runtime type, not the static spelling. `emitValueToString`'s
            //       interface arm (LlvmBackend_String.cpp, the `#1077` arm)
            //       routes through `lowerObjectToString`, so `$"{ix}"` and
            //       `ix.ToString()` already print the vtable answer. The
            //       console arm joins them now.
            //   (c) CLASS receivers with a minting `ToString` override —
            //       `emitMethodCall` returned a fresh +1 the console owned,
            //       and the branch never set `releaseRender`. One mint per call
            //       leaked; measured at 711 B / 40 allocations over 20
            //       iterations. `releaseRender = true` is correct for every
            //       `emitValueToString` arm that returns a fresh +1
            //       (class-with-override, record auto-bearing, choice
            //       auto-hashable, interface vtable/string, enum user override)
            //       AND a no-op on the literal-only arms (class-without-
            //       override, enum-without-override, non-auto-bearing record,
            //       "null" sentinel for a null interface) — §28.9 makes
            //       `bz_string_release` on a sentinel a no-op, so one flag
            //       covers every arm without a per-kind case analysis.
            //   (b) RECORD receivers without a user `ToString` — the
            //       synthesized §9.1 listing was a third miss, separately
            //       fixed by #3118 in this same arm. The reroute here folds
            //       the record/choice dispatch into `emitValueToString`'s own
            //       `emitRecordToString` / `emitChoiceToString` arms, so the
            //       inline `recordIsAutoBearing` / `isAutoEqualityBearingChoice`
            //       arms become redundant — one renderer, one discharge flag,
            //       and the inline §3118 block above this `else if` is now
            //       dead (the rest of the function does not reach it).
            //
            // WHY `emitValueToString` AND NOT A HAND-ROLLED WALK. §8.5's
            // surface is THE SAME answer for every call site of one type —
            // direct `.ToString()`, §18.5 hole, `Console.Write(Line)`, `Object`
            // typed, record component, generic-T operand. Four (now five, post-
            // #2878) call sites agreeing on one render is the structural
            // invariant the previous ad-hoc walk kept breaking (#2913, #2969,
            // #3118). `emitValueToString` is the ONE renderer every sibling
            // spelling reaches; the console arm joining it is the fix. The
            // pre-fix `tryLowerConsoleCall` comment "N places, one rule" was
            // already the diagnosis — the cure is one renderer in one place.
            strPtr = emitValueToString(arg, argTy);
            if (!strPtr) return false;
            // fix #2878 / §28.9: every `emitValueToString` arm returns either a
            // fresh +1 producer (the minting class override, the synthesized
            // record/choice field listing, the interface vtable dispatch, the
            // enum user override) or a literal sentinel (the type-name
            // placeholder for class-without-override, enum-without-override,
            // non-auto-bearing record, the "null" sentinel for a null interface
            // receiver). `bz_string_release` is a no-op on a sentinel, so
            // `releaseRender = true` is correct universally — the discharge
            // flag below (`if (releaseRender) emitStringRelease(strPtr)`) is
            // the SAME path the #2875 `P::Object` arm installed and that
            // #3118 used for its record/choice arms; the four sibling fixes
            // (#2875, #2878, #3118, the next-after) all funnel through it.
            releaseRender = true;
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
        } else if (arg->getType()->isIntegerTy(1)) {
            strPtr = emitPrimitiveToString(arg, sema::PrimitiveType::Bool);
        } else if (arg->getType()->isIntegerTy()) {
            // Widen to i64 so the %lld snprintf path receives the full value
            // (matches the interpolation path's sign-extension to i64).
            llvm::Value* iv = arg->getType()->isIntegerTy(64)
                ? arg
                : builder_.CreateSExt(arg, i64());
            strPtr = emitPrimitiveToString(iv, sema::PrimitiveType::Long);
        }
        // fix #2875: same rule as the typed branch — the numeric snprintf path
        // heap-allocates and the bool path yields a sentinel; a release is
        // right for both, so no per-kind bookkeeping is needed here either.
        releaseRender = (strPtr != arg);
    }
    if (!strPtr) return false;
    emitConsoleWrite(strPtr, isWriteLine, stdfd);
    // issue #421 / fix #2875: discharge the write-only render. Done AFTER the
    // write, which only reads the string. `bz_string_release` frees the data
    // buffer and then the header at rc == 0 — the exact pair the raw free()
    // here used to emit — and is a no-op on the literal/sentinel and shared
    // arms that free() corrupted (see the `releaseRender` declaration above).
    if (releaseRender) {
        emitStringRelease(strPtr);
        // fix #2879 (spec §8.5 / §28.3 / §28.9, v0.240): an OBJECT-typed argument
        // that is itself a +1 string producer METHOD CALL (`m.Fresh()` where
        // `Fresh(): Object => "x" + S`) transfers exactly +1 from the callee —
        // the fresh string the callee's body minted. `lowerObjectToString`
        // (#2846) RETAINs the receiver, which `releaseRender` above discharges
        // (the render's +1); but the callee's OWN +1 is now orphaned unless the
        // caller explicitly releases it. Without this extra discharge the
        // string leaks (38 B/call measured on 564b6d1: 760 B / 40 allocations
        // over 20 iterations, both in a Console.WriteLine(h.Fresh()) and in a
        // `$"{h.Fresh()}"` interpolation — the bug is a string twin of #2865's
        // BOX residual).
        //
        // Gating: `isUserMethodObjectStringReturnCall` is positive ONLY for a
        // call to a LOCALLY-COMPILED, NON-OPERATOR method declared to return
        // Object/interface whose body provably always returns a +1 string
        // producer (every ReturnStmt passes isPlusOneStringProducer — runtime
        // concat, interpolation, string `+`, `.ToString()`, etc.; an arrow body
        // is itself a +1 string producer). An IDENTIFIER read (`var o := …;
        // Console.WriteLine(o);`) is NOT a producer call — `o`'s own disposal
        // handles its borrow — so the predicate is correctly negative and we
        // don't over-release. `arg` and `strPtr` are the SAME runtime pointer
        // (lowerObjectToString's PHI returns the retained receiver), so this is
        // a second rc-aware release on the same string: rc goes from 2 to 1
        // (render) to 0 (producer) → free at the second decrement.
        if (call.args[0].value &&
            isUserMethodObjectStringReturnCall(call.args[0].value.get())) {
            emitStringRelease(arg);
        }
        // fix #2876 (spec §8.6 / §28.3 / §28.9, v0.278): the BOX twin of #2879.
        // An Object-typed argument that is a +1 BOX producer method call
        // (`h.M()` where `M(): Object => _n;`) transfers exactly +1 from the
        // callee — the fresh box the callee's body minted. The strPtr above is
        // a fresh +1 STRING the render (`lowerObjectToString`'s vtable thunk)
        // minted and `releaseRender` already discharged; the callee's OWN +1
        // (the BOX) is now orphaned unless we release it here. Without this
        // extra discharge the box leaks (24 B/call measured on `f693622`: 480 B
        // in 20 allocations over 20 iterations, both for `Console.WriteLine
        // (h.M())` and `$"[{h.M()}]"` interpolation — the box twin of #2865's
        // residual, since #2865 only closed the inline / `lastBoxMint_` and
        // bound-local shapes; this is the box twin of #2879's string fix).
        //
        // Gating: `isUserMethodBoxReturnCall` is positive ONLY for a call to a
        // LOCALLY-COMPILED, NON-OPERATOR method declared to return Object/
        // interface whose body provably always returns a boxable value (every
        // ReturnStmt passes isBoxableValueType / isNullableValueTy per the
        // #2865 pre-pass `collectBoxReturnMethods`). An IDENTIFIER read
        // (`var o: Object := box; Console.WriteLine(o);`) is NOT a producer
        // call — `o`'s own scope-end disposal handles its borrow — so the
        // predicate is correctly negative and we don't over-release. The BOX
        // (`arg`) and the RENDERED STRING (`strPtr`) are DISTINCT runtime
        // pointers (unlike #2879's string twin, where `arg` is its own string
        // and `strPtr == arg`); `lowerObjectToString` does NOT retain the box
        // (only the string arm retains, per #2846), so a SINGLE release on
        // `arg` drops rc from 1 to 0 → free. The two predicates
        // (`isUserMethodObjectStringReturnCall` and `isUserMethodBoxReturnCall`)
        // classify disjoint sets (a method's body returns either a boxable
        // value or a +1 string, never both), so this branch does not
        // double-release on the same call.
        if (call.args[0].value &&
            isUserMethodBoxReturnCall(call.args[0].value.get())) {
            emitBoxRelease(arg);
        }
        // fix #3352 (spec §8.6 / §23.4 / §28.3 / §28.9, v0.240): the inline
        // §8.6 box mint in call-arg position — the indexer/property inline-
        // body half of the residual. `Console.WriteLine(e[k])` where
        // `E.property [i: int]: Object => i;` lowers as: lowerExpr(e[k]) →
        // lowerIndexerGet → boxForTarget mints a fresh box AND records it in
        // `lastBoxMint_`; `arg` IS the inline mint; lowerObjectToString
        // dispatches the box's vtable ToString, which mints a fresh STRING
        // (released by the `releaseRender` arm above), and does NOT retain
        // the box — so the box's +1 is orphaned unless we discharge it here.
        // Pre-fix, the box leaks 24 B/eval (480 B / 20 allocations measured
        // on the issue's repro).
        //
        // Gating (parallel to the `isUserMethodBoxReturnCall` arm just
        // above, with a disjoint predicate):
        //   * `inlineBoxMint` — captured by the save/clear/restore around
        //     `lowerExpr` at the top of this function; positive ONLY when
        //     `lastBoxMint_` was set DURING THIS argument's lowering and
        //     `arg == lastBoxMint_` (i.e., the inline mint IS this
        //     argument);
        //   * `arg->getType()->isPointerTy()` — the inline mint is always a
        //     pointer; a non-pointer `arg` means a different lowering path
        //     fired and there is no box to release (defensive guard,
        //     matching the same shape the sibling arm implicitly trusts);
        //   * the two predicates — `isUserMethodBoxReturnCall` (cross-frame
        //     mint at the callee's return edge, `arg` is the callee's box)
        //     and `inlineBoxMint` (inline mint at this argument's read
        //     site, `arg` is the locally minted box) — classify disjoint
        //     sets: a method returning `Object` either mints cross-frame
        //     (no inline mint at this site, since the callee already
        //     produced a box) or its body is value-typed at an inline
        //     indexer/property site (no cross-frame callee-mint), so they
        //     never both fire for the same `arg`. The `arg == lastBoxMint_`
        //     gate is the strictest over-release safety: a plain IDENTIFIER
        //     read of a bound Object local is just a slot load, leaves
        //     `lastBoxMint_` null, and correctly declines (the local's
        //     scope-end release handles its borrow). The pre-pass
        //     `collectBoxReturnMethods` rejects methods whose body isn't a
        //     provably boxable-value producer, so the inline mint path
        //     never sees a borrowed box pointer from a callee.
        if (inlineBoxMint && arg && arg->getType()->isPointerTy()) {
            emitBoxRelease(arg);
        }
    } else if (strPtr == arg && call.args[0].value &&
               isPlusOneStringProducer(call.args[0].value.get())) {
        // #1031 / §4.6 / §28.9: a fresh +1 STRING argument ($"..." interpolation,
        // string `+`, a `s[a..b]` slice, or a numeric `.ToString()`) is otherwise
        // unreferenced after the write — release it (rc-aware) so
        // `Console.Write/WriteLine($"...")` / `(a + b)` does not leak (it bypasses
        // the normal call-arg disposal because Console output is special-lowered).
        // A borrow (string local/field/literal) is NOT a plus-one producer, so it
        // is not released (no premature free of the shared reference).
        // emitStringRelease is null/sentinel-safe; gated on `strPtr == arg` so a
        // converted render (released above) is never double-released here.
        emitStringRelease(strPtr);
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
//   a) !rt->declaration: the name lives in rt->headName().
//   b) rt->declaration is a builtinClassSym (null decl AND empty moduleName, so
//      an imported user class — same null-decl shape, but a populated moduleName
//      — is NOT mistaken for one; see #2997): name = declaration->name.
// Returns an empty string_view if the TypeRef is not in either shape.
std::string_view Emitter::builtinTypeName(const sema::TypeRef* rt) {
    if (!rt || rt->kind != sema::TypeRef::Kind::Named) return {};
    if (!rt->declaration) {
        // #1462 / §27.8: see through a transparent type alias
        // (`type OA := OwningArray<R>`) — the alias-resolved TypeRef keeps the
        // use-site astNode (`OA`) for diagnostics, so a bare astNode read would
        // mis-identify the builtin. aliasResolvedHead carries the underlying head.
        if (!rt->aliasResolvedHead.empty()) return rt->aliasResolvedHead;
        if (rt->astNode && rt->astNode->name &&
            !rt->astNode->name->parts.empty())
            return rt->headName();
        // #662 foundation B: for IMPORTED fields the TypeRef has no astNode;
        // the loader stores the raw type name in importedName when lookupNamed
        // returns null (see ModuleLoader.cpp convert(), §2.5).
        if (!rt->importedName.empty()) return rt->importedName;
        return {};
    }
    // builtinClassSym — non-null declaration, null decl (no user TypeDecl), AND
    // no originating module. fix #2997 (spec §11.3 / §28.3, v0.240): an IMPORTED
    // user class is shaped EXACTLY like a builtinClassSym here — declaration
    // non-null, decl null (there is no ast::TypeDecl for another module's
    // source) — so `!decl` alone is not "built-in". §11.3 reserves no names, so
    // a dependency module is free to export a class called `Stopwatch` and an
    // importer's `using var s := new Stopwatch();` is a legal program with no
    // built-in in it. The fact that tells them apart is one the compiler already
    // computes: the module loader (ModuleLoader.cpp loadMetadataPass1) populates
    // TypeSymbol::moduleName from the dependency's metadata for EVERY imported
    // type, while a synthetic builtinClassSym (SemaContext::builtinClassSym)
    // leaves it empty. That empty/non-empty split — not `!decl` — is what
    // identifies a real built-in, and answering it in this one chokepoint fixes
    // every probe routed through builtinTypeName (the five residual arms of
    // registerUsingDisposal: Stopwatch / FileStream / CancellationTokenSource /
    // Random / Atomic) at once.
    if (!rt->declaration->decl && rt->declaration->moduleName.empty())
        return rt->declaration->name;
    return {};
}


// fix #2809 / §32.5 / stdlib §10.1–§10.3 (v0.240): see the header for why this
// must read BOTH TypeRef shapes rather than `declaration->name`.
bool Emitter::isFfiBufferTypeRef(const sema::TypeRef* rt) {
    return common::isFfiBufferBuiltinName(builtinTypeName(rt));
}


bool Emitter::isCollectionTypeName(const sema::TypeRef* rt) {
    if (!rt || rt->kind != sema::TypeRef::Kind::Named) return false;
    // Reject a USER-CLASS declaration — only builtin shapes are the array-backed
    // collections named in the spec. `rt->declaration` can be null (the
    // declaration-less Named shape TypeResolution produces for an annotation) OR
    // a builtinClassSym (the declaration stamped for a static-factory result
    // like `FrozenArray<T>.From(...)`); both are accepted, and the actual
    // membership test is the name-only lookup on the registry below.
    if (rt->declaration && (rt->declaration->decl ||
                            !rt->declaration->moduleName.empty()))
        return false;
    std::string_view n = builtinTypeName(rt);  // handles both astNode and importedName
    if (n.empty()) return false;
    // per §4.6/§4.7: LinkedList/OwningLinkedList are NOT array-backed — they
    // use bz_llist_t; §4.14 OwningBuffer<T> IS bz_array_t-backed. The name set
    // lives in the #1414 stage-3 registry (one home, snapshot-pinned).
    // fix #3249: §4.19 FrozenArray<T> joins the array-backed set (it shares
    // bz_array_t* storage with Array<T>); its `From(...)` factory result carries
    // builtinClassSym, which this predicate now also accepts.
    return common::isArrayBackedCollectionName(n);
}

// per §4.6 / §4.7: true for LinkedList<T> and OwningLinkedList<T> (bz_llist_t-backed).
bool Emitter::isLinkedListTypeName(const sema::TypeRef* rt) {
    if (!rt || rt->kind != sema::TypeRef::Kind::Named || rt->declaration)
        return false;
    std::string_view n = builtinTypeName(rt);  // handles both astNode and importedName
    if (n.empty()) return false;
    return common::isLinkedListName(n);  // #1414 stage 3 registry
}


bool Emitter::isDictTypeName(const sema::TypeRef* rt) {
    if (!rt || rt->kind != sema::TypeRef::Kind::Named || rt->declaration)
        return false;
    std::string_view n = builtinTypeName(rt);  // handles both astNode and importedName
    if (n.empty()) return false;
    // stdlib §4.17 (#1354): SortedDictionary shares the bz_dict_t* rep. The name
    // set lives in the #1414 stage-3 registry (one home, snapshot-pinned).
    return common::isDictionaryName(n);
}


// issue #401 / stdlib §4.1 / §4.2 / §4.3: the int-indexed list interfaces.
// IReadOnlyList<T> (get-only, §4.1) is the shared read-only view of BOTH the
// borrowing array (Array<T>: IList<T>, §4.4) and the owning array (OwningArray<T>:
// IOwningList<T>, §4.5). All three are array-backed at runtime, so an interface-
// typed receiver carries the same bz_array_t* as the concrete collection and the
// indexer get/set lower through the identical bz_array_* runtime. IOwningList<T>
// is the only owning-set (disposing) one; IReadOnlyList<T> / IList<T> are
// borrowing — IReadOnlyList<T> is read-only so its setter branch never runs.
bool Emitter::isListInterfaceName(const sema::TypeRef* rt, bool* outOwning) {
    if (outOwning) *outOwning = false;
    if (!rt || rt->kind != sema::TypeRef::Kind::Named || rt->declaration)
        return false;
    if (!rt->astNode || !rt->astNode->name || rt->astNode->name->parts.empty())
        return false;
    auto n = rt->headName();
    if (n == "IOwningList") { if (outOwning) *outOwning = true; return true; }
    return n == "IReadOnlyList" || n == "IList";
}


// fix #853 / stdlib §4.1–§4.3: the *non-list* collection interfaces
// (IReadOnlyCollection<T>, ICollection<T>, IOwningCollection<T>).  Unlike the
// list interfaces above — which only Array<T>/OwningArray<T> implement, so an
// interface-typed receiver is unambiguously the array-backed bz_array_t* — these
// collection interfaces are implemented by collections with INCOMPATIBLE runtime
// representations: Array/Stack/Queue (bz_array_t*, count at offset 8),
// LinkedList (bz_llist_t*, count at offset 16), Dictionary/Set (bz_dict_t*).
// The interface-typed receiver erases which concrete backing it holds, the
// runtime structs share no kind-tag header, and there is no itable wiring these
// members to the runtime.  So a compile-time "treat as bz_array_t*" assumption is
// UNSOUND here (it would read garbage / no-op for a LinkedList/Dictionary/Set
// receiver) — calls through these interfaces are diagnosed rather than silently
// miscompiled (#853 root cause: they previously fell through to a stub returning
// 0 / a no-op).  A real fix needs a runtime foundation (kind-tag header or a
// per-backing collection itable); that is deferred (#853 stays open).
bool Emitter::isCollectionInterfaceName(const sema::TypeRef* rt) {
    if (!rt || rt->kind != sema::TypeRef::Kind::Named || rt->declaration)
        return false;
    std::string_view n = builtinTypeName(rt);
    return n == "IReadOnlyCollection" || n == "ICollection" ||
           n == "IOwningCollection";
}


// issue #343 / §4.8: a Dictionary<K,V> key (idx=0) or Set<T> element (idx=0)
// is a "record key" when the generic arg resolves to a record TypeSymbol that
// actually carries synthesized GetHashCode/Equals (positional records, or
// auto-hashable body records — §9.1 / §9.2). For such keys the generic
// per §11.8's strategy latitude (this implementation monomorphizes; proposal 0103 records the choice): see the header comment. Build and
// install `genericSubst_` from the receiver `target`'s concrete instantiation
// of `parent`, returning the previous BOTH maps (`genericSubst_` AND
// `genericSubstSemaTy_`) for the caller to restore. Restoring only
// `genericSubst_` would leave the sema-type map polluted with the inner
// receiver's bindings — a subsequent generic-subst lookup on a DIFFERENT
// generic receiver in the same Main would then resolve its `_v: T` field at
// the wrong offset. Latent bug surfaced by the pre-gate review of the
// #3346/#3348 narrow fix; the METHOD twin (Calls.cpp:~8980) and the property
// inline-body twin (Exprs.cpp:~5360) already save and restore BOTH maps —
// this function now mirrors that discipline.
Emitter::SavedReceiverSubst
Emitter::setupReceiverGenericSubst(const ast::Expr* target,
                                   const sema::TypeSymbol* parent) {
    SavedReceiverSubst saved;
    saved.subst = genericSubst_;
    saved.semaTy = genericSubstSemaTy_;
    if (!target || !parent) return saved;
    // fix #3143 (§14.1 / §2.5, v0.254): the parameter names through the one
    // both-sources accessor. The old `!parent->decl` early-out plus the AST-only
    // read meant an IMPORTED generic receiver installed NO substitution at all,
    // so the callee ran against the erased T→i32 template layout. The cheap
    // ARITY question is asked first and the name list is built only once every
    // guard has passed — `genericParamCount` allocates nothing.
    //
    // fix #3346 / #3348 (§14.1 / §23.4 / §23.3, v0.278): do NOT short-circuit on
    // `nGP == 0` — even if ps->parent is non-generic (an indexer/property
    // declared on the derived class whose body touches an INHERITED generic-base
    // type-parameter field), the SETTER body needs the inherited base's
    // substitution installed for its implicit field lookup (`(_v: T)`) to land
    // at the right offset. The nGP == 0 short-circuit would skip the inherited-
    // ancestor walk below and silently re-emit the body against the T→i32
    // placeholder — exactly the SETTER half of the silent-corruption family
    // #3346 names. The nGP == 0 path here was originally a fast-path for
    // non-generic DECLARER classes with no substitution work; with #3346 the
    // work often IS required, just to a different ancestor.
    const std::size_t nGP = sema::genericParamCount(parent);
    sema::TypeRef* recvTy = ctx_.exprType(*target);
    if (!recvTy || recvTy->kind != sema::TypeRef::Kind::Named ||
        recvTy->declaration != parent || recvTy->genericArgs.empty()) {
        // fix #3346 / #3348 (§14.1 / §23.4 / §23.3, v0.278): three
        // independent cases land here:
        //
        //   (a) `recvTy->declaration != parent`: an INHERITED accessor on
        //       a generic base reached via a derived receiver (`g[i]` /
        //       `g.P` where `g: GD`, the indexer/property is in
        //       `GBase<T>`, and the receiver's static type is NOT the
        //       generic base). The inlined body lowers against `GBase`'s
        //       T→i32 placeholder, and the read returns only the first 4
        //       bytes of a `%record.Pt`-shaped _v field (#3346's measured
        //       silent corruption on `class GD<U>: GBase<U>` + `g[0]`
        //       returning 11 where 23 is correct). Walk the base chain.
        //
        //   (b) `recvTy->declaration == parent` BUT
        //       `recvTy->genericArgs` is empty: a non-generic receiver of
        //       the generic base's accessor, e.g. `GD: GBase<Pt>`
        //       accessing `P` declared on `GBase<T>` itself. The DIRECT
        //       gate bails because `nGP != recvTy->genericArgs.size()`,
        //       leaving the inherited body's implicit field lookup
        //       `(_v: T)` at the T→i32 erased placeholder.
        //
        //   (c) `recvTy->declaration == parent` (non-generic parent
        //       declared on a class that is itself a derived class, e.g.
        //       `class GD` declares `[i: int]: T` whose body touches
        //       the INHERITED `_v: T` from `GBase<T>`). The body's
        //       implicit field lookup needs GBase's T→Pt binding, which
        //       lives on GBase (not on `parent = GD`). Walk ALL generic
        //       ancestors of recvTy->declaration and add their
        //       substitutions.
        //
        // All three walk via `computeAncestorGenericSubst`, the SAME
        // helper the inherited-METHOD-CALL site uses (#585/#902/#3347
        // closed METHOD in single- and cross-module form). The set of
        // ancestor walks this function may now perform is strictly
        // additive: paths that were never reachable before remain
        // untouched; paths that were reachable install a substitution
        // where they previously did not.
        if (recvTy && recvTy->kind == sema::TypeRef::Kind::Named &&
            recvTy->declaration) {
            // Path (a) — direct walk from receiver declaration to the
            // accessor-declaring class.
            std::unordered_map<std::string_view, llvm::Type*> ancSubst;
            std::unordered_map<std::string_view, const sema::TypeRef*>
                ancSema;
            if (computeAncestorGenericSubst(recvTy, parent, ancSubst,
                                            ancSema)) {
                genericSubst_ = std::move(ancSubst);
                genericSubstSemaTy_ = std::move(ancSema);
                return saved;
            }
            // Path (c) — accessor declared on a non-generic class whose
            // body touches INHERITED generic-base type-parameter fields;
            // walk every generic ancestor of recvTy->declaration.
            std::unordered_map<std::string_view, llvm::Type*> addSubst;
            std::unordered_map<std::string_view, const sema::TypeRef*>
                addSema;
            for (const sema::TypeSymbol* anc = baseClassOf(recvTy->declaration);
                 anc; anc = baseClassOf(anc)) {
                if (anc->genericParams.empty()) continue;
                std::unordered_map<std::string_view, llvm::Type*> aSubst;
                std::unordered_map<std::string_view, const sema::TypeRef*>
                    aSema;
                if (!computeAncestorGenericSubst(recvTy, anc, aSubst, aSema))
                    continue;
                for (const auto& [nm, ty] : aSubst) {
                    if (addSubst.count(nm) == 0) addSubst[nm] = ty;
                    auto sit = aSema.find(nm);
                    if (sit != aSema.end() && addSema.count(nm) == 0)
                        addSema[nm] = sit->second;
                }
            }
            if (!addSubst.empty()) {
                genericSubst_ = std::move(addSubst);
                genericSubstSemaTy_ = std::move(addSema);
                return saved;
            }
        }
        return saved;
    }
    if (nGP != recvTy->genericArgs.size())
        return saved;
    const std::vector<std::string_view> gpNames =
        sema::genericParamNames(parent);
    std::unordered_map<std::string_view, llvm::Type*> newSubst;
    std::unordered_map<std::string_view, const sema::TypeRef*> newSema;
    for (std::size_t gi = 0; gi < gpNames.size(); ++gi) {
        const sema::TypeRef* arg = recvTy->genericArgs[gi];
        newSubst[gpNames[gi]] = lowerTypeRef(arg);
        newSema[gpNames[gi]] = resolveTypeParamRef(arg);
    }
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
    genericSubstSemaTy_ = std::move(newSema);
    return saved;
}

// fix #3353 (§14.1 / §22.7 / §23.4, v0.240): emit a per-(ps, recvTy) wrapper
// that reads the inherited-base indexer's body slot at the SUBSTITUTED
// offset, replacing the wrong-layout `bz$<GBase>$idxget$<arity>` stub the
// LIB exports against the T→i32 erased placeholder.
//
// Background: the cross-module dispatch at lowerIndexerGet (line ~1758) calls
// `importedIndexerGetterFns_[ps]`, an ExternalLinkage stub whose body the LIB
// lowered against `classStruct(GBase)` with no substitution (T→i32). The
// importer's runtime layout is `classStruct(GBase<Pt>)` — the `_v: T` slot is
// 8 bytes for `T = %record.Pt`, not 4. The stub GEPs the i32 slot, reads only
// the first 4 bytes, and returns the wrong value (silent corruption on T=record;
// silent right value by accident on T=class because ptr width is 8 and the i32
// lower 4 bytes happen to be zero in the high 4 — measured 14 where 15 is
// correct on the issue's repro).
//
// This helper is the LOCAL fix's cross-module twin (setupReceiverGenericSubst
// at line ~853 inlines the body under the substitution; we cannot inline
// because `ps->decl == nullptr` per #3275 — see the file header for the
// rationale). Instead, we synthesise a stub whose body is "GEP inherited
// slot, load, return", bypassing the LIB's wrong stub. The substitution is
// installed for `classStruct` so the wrapper reads at the substituted offset
// (the only thing the substitution affects here — `classStruct` resolves T
// to Pt, which lays the slot out as %record.Pt at offset 2).
//
// Pre-condition (#3353 file header): the accessor body must be a SINGLE field
// read on an inherited type-parameter slot. This helper ACCEPTS that case
// (emits the wrapper); for non-trivial bodies it returns nullptr and the
// call site falls through to the existing stub, where the substitution's
// absence means wrong values for any body shape — the same defect class the
// issue acknowledges in its "Estimated scope" comment.
//
// Detection: ps is imported (`ps->decl == nullptr` per #3275); the accessor is
// declared on a GENERIC base (`!ps->parent->genericParams.empty()`); the
// receiver's static type carries a SUBSTITUTION for those params
// (`computeAncestorGenericSubst` succeeds); the accessor's result type is the
// inherited type-parameter T (the body's `=> _v` returns T — for any other
// return type the wrapper would read `_v` where the body computes something
// else, and we decline).
//
// Caching: keyed by `(ps, recvTy)`. Different receivers instantiate the
// wrapper under different substitutions and therefore read at different
// offsets. Each emitter invocation uses a fresh InternalLinkage function;
// the cache key encodes the (ps, recvTy) pair so a second call with the
// same pair returns the previously-emitted function.
llvm::Function* Emitter::emitImportedIndexerGetterInheritedWrapper(
        const sema::PropertySymbol* ps,
        const sema::TypeRef* recvTy) {
    if (!ps || !ps->parent || !recvTy) return nullptr;
    // Pre-condition: only inherited accessors on a generic base apply.
    // `recvTy->declaration != ps->parent` means the receiver is a derived
    // class (the local-receiver case has no substitution needed; the LIB
    // stub is already correct).
    if (recvTy->kind != sema::TypeRef::Kind::Named ||
        !recvTy->declaration ||
        recvTy->declaration == ps->parent) return nullptr;
    if (ps->parent->genericParams.empty()) return nullptr;
    // Cache lookup.
    auto cacheKey = std::make_pair(ps, recvTy);
    auto cached = importedIndexerGetterInheritedWrappers_.find(cacheKey);
    if (cached != importedIndexerGetterInheritedWrappers_.end())
        return cached->second;
    // Compute the substitution from recvTy up the chain to ps->parent
    // (the accessor's declaring class). For `g: GD: GBase<Pt>` this yields
    // T→Pt; for `g: GD<U>: GBase<U>` instantiated as `GD<Pt>` it yields
    // T→Pt via the U→Pt receiver binding.
    std::unordered_map<std::string_view, llvm::Type*> subst;
    std::unordered_map<std::string_view, const sema::TypeRef*> substSema;
    if (!computeAncestorGenericSubst(recvTy, ps->parent, subst, substSema)) return nullptr;
    // Pre-condition: the accessor's return type must be the inherited
    // type-parameter T. If it is anything else (e.g. `int => _v.Length`),
    // the wrapper reads `_v` (T-typed) where the body computes a derived
    // expression — wrong for any body shape except the trivial one.
    //
    // The imported return type is a Named TypeRef. When it is a generic
    // parameter reference (e.g. `T` on `GBase<T>`), `importedMemberType`
    // carries it with `declaration == nullptr` and `importedName == "T"`
    // — see ModuleLoader::convert's lookupNamed path (it searches the
    // dependency's EXPORTED user types, and a generic parameter is not
    // one). Match the head name against ps->parent's generic params to
    // confirm the return type is the inherited type-parameter T.
    sema::TypeRef* retRef = ctx_.importedMemberType(ps);
    if (!retRef || retRef->kind != sema::TypeRef::Kind::Named) return nullptr;
    bool retIsGenericParam = false;
    if (!retRef->declaration) {
        // Generic parameter reference — match the head name against the
        // parent's own generic-parameter list.
        std::string_view retName = retRef->headName();
        for (const sema::Symbol* gp : ps->parent->genericParams) {
            if (gp && std::string(gp->name) == std::string(retName)) {
                retIsGenericParam = true;
                break;
            }
        }
    } else {
        // Direct declaration match — used if a future schema bump carries
        // a generic-param TypeSymbol in `declaration`.
        for (const sema::Symbol* gp : ps->parent->genericParams) {
            if (gp && retRef->declaration == gp) {
                retIsGenericParam = true;
                break;
            }
        }
    }
    if (!retIsGenericParam) return nullptr;
    // Determine the offset of the FIRST instance-storage field under the
    // substitution. For our case (`class GBase<T> { _v: T }`) the offset
    // is `disposedFlagIndex + 1` = 2 (after vtable ptr and disposed flag).
    // For an exception-derived class the offset is `5 + 1` = 6 (the extra
    // 4 slots are the built-in exception base: Message, Inner, CauseChain,
    // ParamName). We use `instanceStorageTypes` under the substitution
    // and the struct's `disposedFlagIndex` to derive the exact slot index.
    auto savedSubst = genericSubst_;
    auto savedSemaTy = genericSubstSemaTy_;
    genericSubst_ = subst;
    genericSubstSemaTy_ = substSema;
    llvm::StructType* specialized = classStruct(ps->parent);
    genericSubst_ = std::move(savedSubst);
    genericSubstSemaTy_ = std::move(savedSemaTy);
    if (!specialized) return nullptr;
    // Find the offset of the inherited type-parameter field. The body is
    // `_v: T`; the offset depends on which field T is. For the common
    // case (`class GBase<T> { _v: T }`) the FIRST instance-storage slot
    // is `_v`, at index `disposedFlagIndex(ps->parent) + 1`.
    std::vector<const sema::Symbol*> storage;
    collectInstanceStorage(ps->parent, storage);
    if (storage.empty()) return nullptr;
    // Look up the first storage field in fieldIndex_ (populated by
    // classStruct above under substitution). For a single-field base
    // (`class GBase<T> { _v: T }`) this gives the offset of `_v`.
    auto idxIt = fieldIndex_.find(storage[0]);
    if (idxIt == fieldIndex_.end()) {
        // Re-run classStruct to populate fieldIndex_ under the substitution
        // — mirrors the equivalent retry in implicitFieldAddr (~line 5184).
        genericSubst_ = subst;
        genericSubstSemaTy_ = substSema;
        (void)classStruct(ps->parent);
        genericSubst_ = std::move(savedSubst);
        genericSubstSemaTy_ = std::move(savedSemaTy);
        idxIt = fieldIndex_.find(storage[0]);
        if (idxIt == fieldIndex_.end()) return nullptr;
    }
    unsigned slotIdx = idxIt->second;
    // Build the wrapper function with the imported stub's signature
    // (`T get(ptr this, idx...)`). Use the substituted return type —
    // `lowerTypeRef(resolveTypeParamRef(retRef))` resolves T to Pt under
    // the substitution we just installed (saved and restored below).
    genericSubst_ = subst;
    genericSubstSemaTy_ = substSema;
    llvm::Type* retTy = lowerTypeRef(resolveTypeParamRef(retRef));
    genericSubst_ = std::move(savedSubst);
    genericSubstSemaTy_ = std::move(savedSemaTy);
    if (!retTy) retTy = i32();
    std::vector<llvm::Type*> paramTys;
    paramTys.push_back(ptrTy());   // `this`
    for (sema::Symbol* p : ps->indexerParams) {
        auto* pps = dynamic_cast<sema::ParameterSymbol*>(p);
        sema::TypeRef* pt = pps ? pps->resolvedDeclType : nullptr;
        paramTys.push_back(pt ? lowerTypeRef(pt) : i32());
    }
    auto* fnTy = llvm::FunctionType::get(retTy, paramTys, false);
    // Build a stable, cache-encoded name: the wrapper is InternalLinkage
    // (no cross-module ABI to maintain) and uniquely named per (ps,
    // recvTy) pair so duplicate emission does not occur.
    // fix #3548: name the (ps, recvTy) pair from its §2.8.3 rule 3 identity
    // — `ps`'s owner+name (mangledTypeName(ps->parent) + ps->name) and
    // recvTy's own mangled identity — not the two POINTER values. A pointer
    // is a fresh heap address every process, which made the emitted symbol
    // (and so the object file) differ between two runs of the SAME compiler
    // on the SAME source. Any residual collision (two distinct (ps, recvTy)
    // pairs mangling to the same string) is caught by llvm::Function::Create's
    // Module insertion, which auto-renames via ValueSymbolTable::create
    // ValueName — deterministic on creation order, itself deterministic here
    // (single-threaded, syntax-order codegen walk).
    std::string wrapperName =
        "bz$" + std::string(ps->parent->name) + "$idxget$" +
        std::to_string(ps->indexerParams.size()) + "$inh$" +
        mangledTypeName(ps->parent) + "$" + std::string(ps->name) + "_" +
        mangleTypeRefForSymbol(recvTy);
    auto* fn = llvm::Function::Create(
        fnTy, llvm::Function::InternalLinkage, wrapperName, module_.get());
    importedIndexerGetterInheritedWrappers_[cacheKey] = fn;
    // Emit the body under the substitution (so the GEP element type
    // matches the substituted slot type, not the i32 placeholder).
    auto* savedFn = currentFn_;
    auto* savedInsert = builder_.GetInsertBlock();
    auto savedIP = savedInsert ? builder_.GetInsertPoint()
                               : decltype(savedInsert->begin()){};
    auto* entry = llvm::BasicBlock::Create(llvm_, "entry", fn);
    currentFn_ = fn;
    builder_.SetInsertPoint(entry);
    genericSubst_ = subst;
    genericSubstSemaTy_ = substSema;
    llvm::Value* thisPtr = &*fn->args().begin();
    llvm::Value* slot = builder_.CreateStructGEP(specialized, thisPtr, slotIdx);
    llvm::Value* result = builder_.CreateLoad(retTy, slot, "idxget.inh");
    builder_.CreateRet(result);
    genericSubst_ = std::move(savedSubst);
    genericSubstSemaTy_ = std::move(savedSemaTy);
    currentFn_ = savedFn;
    if (savedInsert) builder_.SetInsertPoint(savedInsert, savedIP);
    return fn;
}

// fix #3353 setter twin. Mirrors the getter wrapper but emits
// `void set(ptr this, idx..., T value)` whose body is `GEP slot; store value`.
// Same pre-conditions: imported, generic base, derived receiver with
// substitution, accessor return/parameter type is T, body is a single
// field write of `value` to `_v: T` (the trivial setter).
llvm::Function* Emitter::emitImportedIndexerSetterInheritedWrapper(
        const sema::PropertySymbol* ps,
        const sema::TypeRef* recvTy) {
    if (!ps || !ps->parent || !recvTy) return nullptr;
    if (recvTy->kind != sema::TypeRef::Kind::Named ||
        !recvTy->declaration ||
        recvTy->declaration == ps->parent) return nullptr;
    if (ps->parent->genericParams.empty()) return nullptr;
    auto cacheKey = std::make_pair(ps, recvTy);
    auto cached = importedIndexerSetterInheritedWrappers_.find(cacheKey);
    if (cached != importedIndexerSetterInheritedWrappers_.end())
        return cached->second;
    std::unordered_map<std::string_view, llvm::Type*> subst;
    std::unordered_map<std::string_view, const sema::TypeRef*> substSema;
    if (!computeAncestorGenericSubst(recvTy, ps->parent, subst, substSema))
        return nullptr;
    // Setter param type — same T-typed check as the getter return type.
    sema::TypeRef* retRef = ctx_.importedMemberType(ps);
    if (!retRef || retRef->kind != sema::TypeRef::Kind::Named) return nullptr;
    bool retIsGenericParam = false;
    if (!retRef->declaration) {
        std::string_view retName = retRef->headName();
        for (const sema::Symbol* gp : ps->parent->genericParams) {
            if (gp && std::string(gp->name) == std::string(retName)) {
                retIsGenericParam = true;
                break;
            }
        }
    } else {
        for (const sema::Symbol* gp : ps->parent->genericParams) {
            if (gp && retRef->declaration == gp) {
                retIsGenericParam = true;
                break;
            }
        }
    }
    if (!retIsGenericParam) return nullptr;
    auto savedSubst = genericSubst_;
    auto savedSemaTy = genericSubstSemaTy_;
    genericSubst_ = subst;
    genericSubstSemaTy_ = substSema;
    llvm::StructType* specialized = classStruct(ps->parent);
    genericSubst_ = std::move(savedSubst);
    genericSubstSemaTy_ = std::move(savedSemaTy);
    if (!specialized) return nullptr;
    std::vector<const sema::Symbol*> storage;
    collectInstanceStorage(ps->parent, storage);
    if (storage.empty()) return nullptr;
    auto idxIt = fieldIndex_.find(storage[0]);
    if (idxIt == fieldIndex_.end()) {
        genericSubst_ = subst;
        genericSubstSemaTy_ = substSema;
        (void)classStruct(ps->parent);
        genericSubst_ = std::move(savedSubst);
        genericSubstSemaTy_ = std::move(savedSemaTy);
        idxIt = fieldIndex_.find(storage[0]);
        if (idxIt == fieldIndex_.end()) return nullptr;
    }
    unsigned slotIdx = idxIt->second;
    genericSubst_ = subst;
    genericSubstSemaTy_ = substSema;
    llvm::Type* retTy = lowerTypeRef(resolveTypeParamRef(retRef));
    genericSubst_ = std::move(savedSubst);
    genericSubstSemaTy_ = std::move(savedSemaTy);
    if (!retTy) retTy = i32();
    std::vector<llvm::Type*> paramTys;
    paramTys.push_back(ptrTy());
    for (sema::Symbol* p : ps->indexerParams) {
        auto* pps = dynamic_cast<sema::ParameterSymbol*>(p);
        sema::TypeRef* pt = pps ? pps->resolvedDeclType : nullptr;
        paramTys.push_back(pt ? lowerTypeRef(pt) : i32());
    }
    paramTys.push_back(retTy);   // `value` parameter
    auto* fnTy = llvm::FunctionType::get(builder_.getVoidTy(), paramTys, false);
    // fix #3548: see emitImportedIndexerGetterInheritedWrapper above — the
    // same §2.8.3 rule 3 identity, not the (ps, recvTy) POINTER pair.
    std::string wrapperName =
        "bz$" + std::string(ps->parent->name) + "$idxset$" +
        std::to_string(ps->indexerParams.size()) + "$inh$" +
        mangledTypeName(ps->parent) + "$" + std::string(ps->name) + "_" +
        mangleTypeRefForSymbol(recvTy);
    auto* fn = llvm::Function::Create(
        fnTy, llvm::Function::InternalLinkage, wrapperName, module_.get());
    importedIndexerSetterInheritedWrappers_[cacheKey] = fn;
    auto* savedFn = currentFn_;
    auto* savedInsert = builder_.GetInsertBlock();
    auto savedIP = savedInsert ? builder_.GetInsertPoint()
                               : decltype(savedInsert->begin()){};
    auto* entry = llvm::BasicBlock::Create(llvm_, "entry", fn);
    currentFn_ = fn;
    builder_.SetInsertPoint(entry);
    genericSubst_ = subst;
    genericSubstSemaTy_ = substSema;
    llvm::Value* thisPtr = &*fn->args().begin();
    llvm::Value* slot = builder_.CreateStructGEP(specialized, thisPtr, slotIdx);
    // The `value` parameter is the LAST argument (after `this` and the
    // index params). Skip the first N+1 args (this + indices), take the
    // last as `value`.
    llvm::Value* value = &*std::next(fn->args().end(), -1);
    builder_.CreateStore(value, slot);
    builder_.CreateRetVoid();
    genericSubst_ = std::move(savedSubst);
    genericSubstSemaTy_ = std::move(savedSemaTy);
    currentFn_ = savedFn;
    if (savedInsert) builder_.SetInsertPoint(savedInsert, savedIP);
    return fn;
}

// fix #3353 named-property getter twin. The setter is below. Both
// follow the same shape as the indexer twins but the indexerParams are
// empty (a NAMED property has no index params, just `T get(ptr this)` and
// `void set(ptr this, T value)`).
llvm::Function* Emitter::emitImportedPropGetterInheritedWrapper(
        const sema::PropertySymbol* ps,
        const sema::TypeRef* recvTy) {
    if (!ps || !ps->parent || !recvTy) return nullptr;
    if (recvTy->kind != sema::TypeRef::Kind::Named ||
        !recvTy->declaration ||
        recvTy->declaration == ps->parent) return nullptr;
    if (ps->parent->genericParams.empty()) return nullptr;
    auto cacheKey = std::make_pair(ps, recvTy);
    auto cached = importedPropGetterInheritedWrappers_.find(cacheKey);
    if (cached != importedPropGetterInheritedWrappers_.end())
        return cached->second;
    std::unordered_map<std::string_view, llvm::Type*> subst;
    std::unordered_map<std::string_view, const sema::TypeRef*> substSema;
    if (!computeAncestorGenericSubst(recvTy, ps->parent, subst, substSema))
        return nullptr;
    sema::TypeRef* retRef = ctx_.importedMemberType(ps);
    if (!retRef || retRef->kind != sema::TypeRef::Kind::Named ||
        !retRef->declaration) return nullptr;
    bool retIsGenericParam = false;
    for (const sema::Symbol* gp : ps->parent->genericParams) {
        if (gp && retRef->declaration == gp) {
            retIsGenericParam = true;
            break;
        }
    }
    if (!retIsGenericParam) return nullptr;
    auto savedSubst = genericSubst_;
    auto savedSemaTy = genericSubstSemaTy_;
    genericSubst_ = subst;
    genericSubstSemaTy_ = substSema;
    llvm::StructType* specialized = classStruct(ps->parent);
    genericSubst_ = std::move(savedSubst);
    genericSubstSemaTy_ = std::move(savedSemaTy);
    if (!specialized) return nullptr;
    std::vector<const sema::Symbol*> storage;
    collectInstanceStorage(ps->parent, storage);
    if (storage.empty()) return nullptr;
    auto idxIt = fieldIndex_.find(storage[0]);
    if (idxIt == fieldIndex_.end()) {
        genericSubst_ = subst;
        genericSubstSemaTy_ = substSema;
        (void)classStruct(ps->parent);
        genericSubst_ = std::move(savedSubst);
        genericSubstSemaTy_ = std::move(savedSemaTy);
        idxIt = fieldIndex_.find(storage[0]);
        if (idxIt == fieldIndex_.end()) return nullptr;
    }
    unsigned slotIdx = idxIt->second;
    genericSubst_ = subst;
    genericSubstSemaTy_ = substSema;
    llvm::Type* retTy = lowerTypeRef(resolveTypeParamRef(retRef));
    genericSubst_ = std::move(savedSubst);
    genericSubstSemaTy_ = std::move(savedSemaTy);
    if (!retTy) retTy = i32();
    std::vector<llvm::Type*> paramTys;
    paramTys.push_back(ptrTy());   // `this`
    auto* fnTy = llvm::FunctionType::get(retTy, paramTys, false);
    // fix #3548: see emitImportedIndexerGetterInheritedWrapper above — the
    // same §2.8.3 rule 3 identity, not the (ps, recvTy) POINTER pair.
    std::string wrapperName =
        "bz$" + std::string(ps->parent->name) + "$" +
        std::string(ps->name) + "$propget$inh$" +
        mangledTypeName(ps->parent) + "$" + std::string(ps->name) + "_" +
        mangleTypeRefForSymbol(recvTy);
    auto* fn = llvm::Function::Create(
        fnTy, llvm::Function::InternalLinkage, wrapperName, module_.get());
    importedPropGetterInheritedWrappers_[cacheKey] = fn;
    auto* savedFn = currentFn_;
    auto* savedInsert = builder_.GetInsertBlock();
    auto savedIP = savedInsert ? builder_.GetInsertPoint()
                               : decltype(savedInsert->begin()){};
    auto* entry = llvm::BasicBlock::Create(llvm_, "entry", fn);
    currentFn_ = fn;
    builder_.SetInsertPoint(entry);
    genericSubst_ = subst;
    genericSubstSemaTy_ = substSema;
    llvm::Value* thisPtr = &*fn->args().begin();
    llvm::Value* slot = builder_.CreateStructGEP(specialized, thisPtr, slotIdx);
    llvm::Value* result = builder_.CreateLoad(retTy, slot, "propget.inh");
    builder_.CreateRet(result);
    genericSubst_ = std::move(savedSubst);
    genericSubstSemaTy_ = std::move(savedSemaTy);
    currentFn_ = savedFn;
    if (savedInsert) builder_.SetInsertPoint(savedInsert, savedIP);
    return fn;
}

llvm::Function* Emitter::emitImportedPropSetterInheritedWrapper(
        const sema::PropertySymbol* ps,
        const sema::TypeRef* recvTy) {
    if (!ps || !ps->parent || !recvTy) return nullptr;
    if (recvTy->kind != sema::TypeRef::Kind::Named ||
        !recvTy->declaration ||
        recvTy->declaration == ps->parent) return nullptr;
    if (ps->parent->genericParams.empty()) return nullptr;
    auto cacheKey = std::make_pair(ps, recvTy);
    auto cached = importedPropSetterInheritedWrappers_.find(cacheKey);
    if (cached != importedPropSetterInheritedWrappers_.end())
        return cached->second;
    std::unordered_map<std::string_view, llvm::Type*> subst;
    std::unordered_map<std::string_view, const sema::TypeRef*> substSema;
    if (!computeAncestorGenericSubst(recvTy, ps->parent, subst, substSema))
        return nullptr;
    sema::TypeRef* retRef = ctx_.importedMemberType(ps);
    if (!retRef || retRef->kind != sema::TypeRef::Kind::Named ||
        !retRef->declaration) return nullptr;
    bool retIsGenericParam = false;
    for (const sema::Symbol* gp : ps->parent->genericParams) {
        if (gp && retRef->declaration == gp) {
            retIsGenericParam = true;
            break;
        }
    }
    if (!retIsGenericParam) return nullptr;
    auto savedSubst = genericSubst_;
    auto savedSemaTy = genericSubstSemaTy_;
    genericSubst_ = subst;
    genericSubstSemaTy_ = substSema;
    llvm::StructType* specialized = classStruct(ps->parent);
    genericSubst_ = std::move(savedSubst);
    genericSubstSemaTy_ = std::move(savedSemaTy);
    if (!specialized) return nullptr;
    std::vector<const sema::Symbol*> storage;
    collectInstanceStorage(ps->parent, storage);
    if (storage.empty()) return nullptr;
    auto idxIt = fieldIndex_.find(storage[0]);
    if (idxIt == fieldIndex_.end()) {
        genericSubst_ = subst;
        genericSubstSemaTy_ = substSema;
        (void)classStruct(ps->parent);
        genericSubst_ = std::move(savedSubst);
        genericSubstSemaTy_ = std::move(savedSemaTy);
        idxIt = fieldIndex_.find(storage[0]);
        if (idxIt == fieldIndex_.end()) return nullptr;
    }
    unsigned slotIdx = idxIt->second;
    genericSubst_ = subst;
    genericSubstSemaTy_ = substSema;
    llvm::Type* retTy = lowerTypeRef(resolveTypeParamRef(retRef));
    genericSubst_ = std::move(savedSubst);
    genericSubstSemaTy_ = std::move(savedSemaTy);
    if (!retTy) retTy = i32();
    std::vector<llvm::Type*> paramTys;
    paramTys.push_back(ptrTy());   // `this`
    paramTys.push_back(retTy);     // `value`
    auto* fnTy = llvm::FunctionType::get(builder_.getVoidTy(), paramTys, false);
    // fix #3548: see emitImportedIndexerGetterInheritedWrapper above — the
    // same §2.8.3 rule 3 identity, not the (ps, recvTy) POINTER pair.
    std::string wrapperName =
        "bz$" + std::string(ps->parent->name) + "$" +
        std::string(ps->name) + "$propset$inh$" +
        mangledTypeName(ps->parent) + "$" + std::string(ps->name) + "_" +
        mangleTypeRefForSymbol(recvTy);
    auto* fn = llvm::Function::Create(
        fnTy, llvm::Function::InternalLinkage, wrapperName, module_.get());
    importedPropSetterInheritedWrappers_[cacheKey] = fn;
    auto* savedFn = currentFn_;
    auto* savedInsert = builder_.GetInsertBlock();
    auto savedIP = savedInsert ? builder_.GetInsertPoint()
                               : decltype(savedInsert->begin()){};
    auto* entry = llvm::BasicBlock::Create(llvm_, "entry", fn);
    currentFn_ = fn;
    builder_.SetInsertPoint(entry);
    genericSubst_ = subst;
    genericSubstSemaTy_ = substSema;
    llvm::Value* thisPtr = &*fn->args().begin();
    llvm::Value* slot = builder_.CreateStructGEP(specialized, thisPtr, slotIdx);
    llvm::Value* value = &*std::next(fn->args().begin(), 1);
    builder_.CreateStore(value, slot);
    builder_.CreateRetVoid();
    genericSubst_ = std::move(savedSubst);
    genericSubstSemaTy_ = std::move(savedSemaTy);
    currentFn_ = savedFn;
    if (savedInsert) builder_.SetInsertPoint(savedInsert, savedIP);
    return fn;
}

// runtime variant is used; string and primitive keys keep their fast paths.
// issue #343 / §9.1 / §9.2: if `ref` resolves to a record type that actually
// carries synthesized GetHashCode/Equals (a positional record, or an
// auto-hashable body record), return its TypeSymbol; otherwise null.
const sema::TypeSymbol* Emitter::recordTypeOfRef(const sema::TypeRef* ref) {
    if (!ref || ref->kind != sema::TypeRef::Kind::Named || !ref->declaration)
        return nullptr;
    const sema::TypeSymbol* ts = ref->declaration;
    // fix #3132 (§9.1 / §9.2 / §8.3 / §2.8.3 rule 4, v0.252; family F7 of the
    // imported-symbol seam #3152). This used to decide "is it positional?" by
    // casting `ts->decl` to an `ast::TypeDecl` and reading that node's
    // `hasPositionalFields`. `ModuleLoader` never assigns `TypeSymbol::decl` for
    // an imported type, so the cast was null for every import and an imported
    // positional record answered "no auto equality" here — routed off the
    // structural record-key/record-element path.
    //
    // MEASURED on a0b655e with a real two-module build (`bezelc -c dep.bzl`
    // then `bezelc --import dep.o app.bzl`) for `record PtR(X: int, Y: int)`:
    // the importer ABORTED the compiler — `ICmpInst::AssertOK` "Invalid operand
    // types for ICmp instruction" — because the fallback path an ineligible
    // record takes compares the loaded struct with an integer compare. The
    // record arm is, as `emitRemoveElementEquality` states above its own call,
    // "mandatory (not an extension)", so this was never a slow path: it was a
    // wrong answer that produces invalid IR. The same program with `PtR`
    // declared locally compiled and ran clean.
    //
    // `recordIsAutoBearing` is the both-sources predicate (`hasPositionalFields`
    // first, AST fallback, then §9.2's body-record conditions) introduced by
    // #2934's follow-up for the two §8.5 rendering sites. One predicate, every
    // caller, so the two sources cannot be read apart again.
    if (recordIsAutoBearing(ts)) return ts;
    return nullptr;  // not a record, or an ineligible body record (§9.2)
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
            if (!(ms && !ms->isOperator && ms->name == "Equals" &&
                  ms->params.size() == 1 && !isStaticMethod(ms)))
                continue;
            // #1115/#1151: only a REFERENCE-parameter `Equals` (compiled
            // param[1] is a pointer, e.g. `Equals(Object?)`) is the §8.4
            // Object.Equals override used for collection element comparison; a
            // value-typed `Equals(other: int)` (an IEquatable<int> impl) is not
            // — returning it would pass a pointer argument to an i32 parameter.
            // Conservatively keep the prior accept for an uncompiled method.
            auto fit = methodFns_.find(ms);
            if (fit == methodFns_.end() ||
                (fit->second->getFunctionType()->getNumParams() >= 2 &&
                 fit->second->getFunctionType()->getParamType(1)->isPointerTy()))
                return ms;
        }
        const sema::TypeSymbol* nxt = nullptr;
        for (auto* b : cur->baseTypes)
            if (b && b->kind == ast::TypeDeclKind::Class) { nxt = b; break; }
        cur = nxt;
    }
    return nullptr;
}


// issue #400 / §25.2: the per-element comparison `Array<T>`/`OwningArray<T>`'s
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


// stdlib §4.10 / §8.3 (#837 / #1474; extracted for #1070): dedup-insert one
// element into an ARRAY-BACKED `Set<T>` / `OwningSet<T>`.
//
// A class-or-interface element has no generic-dict backing (genericBackingType
// declines every class), so `new Set<C>()` / `new OwningSet<C>()` allocate a
// bz_array_t and every membership operation is a linear §8.3-equality scan.
// This scans `setArr` for an element EQUAL to `item` per that contract (a user
// `Equals` override is honoured, else reference identity — emitRemoveElement-
// Equality); on a duplicate it calls `dupDisposer` (non-null only for the
// OWNING family, whose §4.10 contract disposes the rejected element — the
// caller relinquished ownership) and does NOT append; otherwise it appends via
// bz_array_add. `item` is stored by value into the backing, so the array is the
// sole remaining owner of an appended element.
//
// Shared by `OwningSet<T>.Add` / `.UnionWith` (the call path) and by the
// §15.4 collection-literal materializer, so a literal-built set is bit-for-bit
// what `new` + `.Add` builds. Returns false only on an internal
// emitRemoveElementEquality failure (malformed comparison IR).
bool Emitter::emitArrayBackedSetInsert(llvm::Value* setArr, llvm::Value* item,
                                       const sema::TypeRef* elemRef,
                                       llvm::Function* dupDisposer,
                                       int64_t esz) {
    if (!setArr || !item) return false;
    auto* lenFn = declareC("bz_array_len", i64(), {ptrTy()});
    llvm::Value* arrLen = builder_.CreateCall(lenFn, {setArr}, "sa.len");
    auto* condBB = llvm::BasicBlock::Create(llvm_, "sa.c", currentFn_);
    auto* bodyBB = llvm::BasicBlock::Create(llvm_, "sa.b", currentFn_);
    auto* nextBB = llvm::BasicBlock::Create(llvm_, "sa.n", currentFn_);
    auto* dupBB  = llvm::BasicBlock::Create(llvm_, "sa.dup", currentFn_);
    auto* newBB  = llvm::BasicBlock::Create(llvm_, "sa.new", currentFn_);
    auto* endBB  = llvm::BasicBlock::Create(llvm_, "sa.end", currentFn_);
    auto* idxSlot = entryAlloca(i64());
    builder_.CreateStore(builder_.getInt64(0), idxSlot);
    builder_.CreateBr(condBB);
    builder_.SetInsertPoint(condBB);
    llvm::Value* curIdx = builder_.CreateLoad(i64(), idxSlot, "sa.i");
    builder_.CreateCondBr(
        builder_.CreateICmpSLT(curIdx, arrLen), bodyBB, newBB);
    builder_.SetInsertPoint(bodyBB);
    auto* ptrFn = declareC("bz_array_ptr_at", ptrTy(),
                           {ptrTy(), i64(), i64()});
    llvm::Value* ep = builder_.CreateCall(
        ptrFn, {setArr, curIdx, builder_.getInt64(esz)}, "sa.ep");
    llvm::Value* existing = builder_.CreateLoad(ptrTy(), ep, "sa.ex");
    llvm::Value* same = emitRemoveElementEquality(
        existing, item, elemRef, /*elemIsClass=*/true);
    if (!same) return false;
    builder_.CreateCondBr(same, dupBB, nextBB);
    builder_.SetInsertPoint(nextBB);
    builder_.CreateStore(
        builder_.CreateAdd(curIdx, builder_.getInt64(1)), idxSlot);
    builder_.CreateBr(condBB);
    // duplicate: dispose the passed item (§4.10), do not append.
    builder_.SetInsertPoint(dupBB);
    if (dupDisposer) builder_.CreateCall(dupDisposer, {item});
    builder_.CreateBr(endBB);
    // new: append.
    builder_.SetInsertPoint(newBB);
    {
        llvm::AllocaInst* slot = entryAlloca(item->getType());
        builder_.CreateStore(item, slot);
        auto* addFn = declareC("bz_array_add",
            llvm::Type::getVoidTy(llvm_), {ptrTy(), ptrTy(), i64()});
        builder_.CreateCall(addFn, {setArr, slot, builder_.getInt64(esz)});
    }
    builder_.CreateBr(endBB);
    builder_.SetInsertPoint(endBB);
    return true;
}


llvm::Value* Emitter::tryLowerCollectionInstanceCall(const ast::CallExpr& call) {
    if (!call.callee) return nullptr;
    auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(call.callee.get());
    if (!ma || !ma->target) return nullptr;
    const auto& m = ma->member;

    auto* recvTy = ctx_.exprType(*ma->target);
    // fix #3345: handle the static-call form `Array.Sort(a)` /
    // `Array.Reverse(a)` / `Array.Fill(a, v)` — the target is the TYPE
    // NAME (IdentifierExpr "Array"), not a value; recvTy is null. Sema
    // (ExprTypecheck_Calls) accepts the static-call shape by typing it
    // as the first argument's array type and returns Unit for the
    // in-place methods. Codegen's static-call sugar: re-target to the
    // FIRST ARGUMENT (the array receiver value) for the duration of
    // this function. The ast::MemberAccessExpr itself is const, so we
    // shadow `ma->target` with `effectiveRecvExpr` for the one site
    // that lowers the receiver (line ~1379 below, `recv = lowerExpr(*ma->target)`).
    // Every other ma->target use in this function (igrouping recv,
    // grouping reshape, freeUnboundOwnedBorrowingArrayTemp cleanup)
    // is gated on recvTy being a non-null grouping / array type, which
    // the static-call form never produces — they short-circuit cleanly.
    // The `isStaticArrayCall` flag below is consulted by the in-place
    // Sort/Reverse arms (and any other in-place static-call member
    // that needs to know whether args[0] is the implicit receiver).
    bool isStaticArrayCall = false;
    const ast::Expr* effectiveRecvExpr = ma->target.get();
    if (!recvTy) {
        if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(ma->target.get())) {
            if ((id->name == "Array" || id->name == "OwningArray") &&
                !call.args.empty() && call.args[0].value) {
                recvTy = ctx_.exprType(*call.args[0].value);
                if (recvTy) {
                    effectiveRecvExpr = call.args[0].value.get();
                    isStaticArrayCall = true;
                }
            }
        }
    }
    // stdlib §5 (#755, supersedes #1296): an `IGrouping<TKey, T>` receiver.
    // `IGrouping : IEnumerable<T>` (stdlib §5:3076), so every §5 eager terminal
    // (Sum/Min/Max/Average/Any/All/Aggregate/Count(pred)/First/Last/Single/
    // ElementAt/*OrNull/Contains/IndexOf) resolves on it — but its runtime value
    // is a bz_grouping_t*, which none of the receiver shapes below recognized, so
    // the calls fell through to the generic dispatch and read the grouping as an
    // EMPTY sequence (Sum → 0, Any → false, Min/Max → 0). #1296 patched a subset
    // by hand in a separate slot; that duplicate reproduced none of the array
    // path's spec-mandated faults (Single with no arity check, ElementAt with no
    // bounds check — #2265) and never covered the selector/predicate forms.
    //
    // The grouping's members are a fully-materialized `bz_array_t*` the GROUPING
    // owns (bz_grouping_elements). Restate the receiver as that `Array<T>` — type
    // AND value — and the entire array-receiver lowering below applies verbatim,
    // faults included. `lazyTempArr` deliberately stays null for this shape: the
    // array is the grouping's, not a snapshot this call may free.
    const sema::TypeRef* igroupingElemRef = iGroupingElemTypeRef(recvTy);
    if (igroupingElemRef) recvTy = elementArrayTypeRef(igroupingElemRef);
    // fix #853 / stdlib §4.1–§4.3: a BORROWING list-interface receiver
    // (IReadOnlyList<T> / IList<T>) is implemented only by the array-backed
    // Array<T>, so its runtime value is the same bz_array_t* as the concrete
    // collection and its mutating/query methods (Add / Clear / Contains / Remove
    // / IndexOf / Insert) lower through the identical bz_array_* runtime — the
    // same bridge the indexer get/set already uses (#401). Without this they fell
    // through to a stub: Add was a silent no-op (#853). The OWNING list interface
    // IOwningList<T> is excluded here (outOwning) — mutation through it disposes
    // owned elements, which is deferred (diagnosed below). Read-only `.Count` is
    // a property handled in LlvmBackend_Exprs.cpp, not here.
    bool listIfaceOwning = false;
    bool isBorrowingListIface =
        isListInterfaceName(recvTy, &listIfaceOwning) && !listIfaceOwning;
    bool isArr  = isCollectionTypeName(recvTy) || isBorrowingListIface;
    bool isDict = isDictTypeName(recvTy);
    // Check for StringBuilder
    bool isStrBld = (recvTy && recvTy->astNode && recvTy->astNode->name &&
                     !recvTy->astNode->name->parts.empty() &&
                     recvTy->headName() == "StringBuilder");

    // §28.6 / §28.11 (v0.213, axiom A8 retired, #2081): `.DisposeElements()` is
    // gone from the collection surface. Sema (ExprTypecheck_Calls) rejects it on
    // a CONCRETE collection receiver, but an INTERFACE-typed receiver
    // (IList/IReadOnlyList/ICollection/IReadOnlyCollection/IOwningList/
    // IOwningCollection) slips past that concrete-only check and would otherwise
    // mislower here — the borrowing-list-iface bridge to the (now-deleted) array
    // lowering, or the polymorphic-interface diagnostic below. Reject it for
    // every collection-interface receiver so no `.DisposeElements()` compiles.
    bool listIfaceForDispElem = false;
    if (m == "DisposeElements" &&
        (isListInterfaceName(recvTy, &listIfaceForDispElem) ||
         isCollectionInterfaceName(recvTy))) {
        ctx_.diagnostics().report(
            Severity::Error, call.span.begin,
            "`.DisposeElements()` was removed from the collection surface "
            "(\xC2\xA7""28.6 / \xC2\xA7""28.11, v0.213 \xE2\x80\x94 axiom A8, the "
            "transit exception, is retired): a borrowing collection is a transit "
            "point and never disposes its elements. Declare the owning family "
            "(OwningArray/OwningDictionary/\xE2\x80\xA6) and let `Dispose()` "
            "cascade, or take each element out first (`Drain()`, the `Pop*` "
            "family, or `move coll[i]`) into an owned binding.");
        return llvm::ConstantInt::get(i32(), 0);  // unit; compile already failed
    }

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
    // issue #775 / §16.3: a `Range<T>` is IEnumerable<T>; route its aggregate
    // terminals (Count/Sum/Any/…) through the materialize-then-run path, the
    // same as a lazy/LinkedList receiver. lowerToLazyEnumerator wraps the range
    // via bz_lazy_from_range. (Recognized by name — in receiver position the
    // Range TypeSymbol carries the builtin class decl, so isRangeType is false,
    // but the value is the %bz.range struct.) `Contains` keeps its own §17.5
    // range-membership lowering and is intentionally not range-materialized here.
    bool isRangeRecv = recvTy && recvTy->kind == sema::TypeRef::Kind::Named &&
                       recvTy->declaration &&
                       recvTy->declaration->name == "Range" && m != "Contains";
    bool isArrTerminal =
        m == "Count" || m == "Sum" || m == "Aggregate" || m == "Average" ||
        m == "Min" || m == "Max" || m == "First" || m == "Single" ||
        m == "Last" || m == "ElementAt" || m == "Any" || m == "All" ||
        m == "Contains" || m == "IndexOf" ||
        // stdlib §5 *OrNull variants (lazy source → materialize, then check):
        m == "FirstOrNull" || m == "LastOrNull" || m == "SingleOrNull";
    // issue #995 / stdlib §5 / §29.11: a vtable-backed IEnumerable<T> receiver
    // (interface-typed local/param, or a yield-method result) is materialized via
    // lowerToLazyEnumerator (driving the iterator vtable once into an Array<T>),
    // the same materialize-then-run path lazy/LinkedList/Range receivers use. This
    // eliminates the #995 silent miscompile where Count/Sum/Any/First/… returned
    // garbage on such a receiver.
    bool vtIEnum = isVtableBackedIEnumerableType(recvTy);
    // fix #1220 / §5 / §9.6: string is IEnumerable<char> — route its aggregate
    // terminals (Count/Any/Sum/First/…) through the same materialize-then-run
    // path. lowerToLazyEnumerator materializes via bz_str_to_char_array.
    bool isStringRecvTerm = recvTy &&
        recvTy->kind == sema::TypeRef::Kind::Primitive &&
        recvTy->primitive == sema::PrimitiveType::String;
    bool lazyTerminal =
        (lazyRecv || isLinkedList || isRangeRecv || vtIEnum ||
         isStringRecvTerm) && isArrTerminal;

    // fix #853 / stdlib §4.1–§4.3: a built-in collection member invoked through a
    // POLYMORPHIC collection interface (IReadOnlyCollection<T> / ICollection<T> /
    // IOwningCollection<T>) OR through the OWNING list interface (IOwningList<T>).
    // These erase the concrete backing, which spans incompatible runtime
    // representations (bz_array_t* / bz_llist_t* / bz_dict_t*) with no shared kind
    // tag and no collection itable — so no single bz_* call is sound. Previously
    // these fell through to a stub: ICollection<T>.Add was a silent no-op, etc.
    // (#853). Diagnose the recognised built-in collection members rather than
    // miscompile. (LINQ extension operators over IEnumerable<T> are handled by
    // tryLowerLazyLinqCall before this function, so they are unaffected.) The
    // sound subset — the array-backed BORROWING list interfaces — is bridged
    // above (isBorrowingListIface). A real fix for the polymorphic interfaces
    // needs a runtime foundation; deferred (#853 stays open).
    {
        bool ownerOnly = false;
        bool isPolyCollIface =
            isCollectionInterfaceName(recvTy) ||
            (isListInterfaceName(recvTy, &ownerOnly) && ownerOnly);  // IOwningList
        bool isBuiltinCollMember =
            m == "Add" || m == "Clear" || m == "Contains" || m == "Remove" ||
            m == "Insert" || m == "IndexOf";
        if (isPolyCollIface && isBuiltinCollMember) {
            ctx_.diagnostics().report(
                Severity::Error, call.span.begin,
                "`." + std::string(m) +
                "` through a polymorphic collection interface "
                "(IReadOnlyCollection<T> / ICollection<T> / IOwningCollection<T> "
                "/ IOwningList<T>) is not yet supported in codegen "
                "(stdlib \xC2\xA7""4.1\xE2\x80\x93\xC2\xA7""4.3, #853): these "
                "interfaces are implemented by collections with incompatible "
                "runtime representations (array / linked-list / hash-table) and "
                "the concrete backing is erased at the interface, so no single "
                "runtime dispatch is sound. Call the method on the concrete "
                "collection, or pass a borrowing list interface "
                "(IReadOnlyList<T> / IList<T>) which is array-backed.");
            // Return a poison value of the call's RESULT type so the enclosing
            // expression (e.g. an expression-body lambda `=> c.Contains(x)`
            // returning bool) emits well-formed IR — the diagnostic already
            // failed the compile, so this placeholder is never executed, but it
            // must type-match to avoid a spurious IR-verification error masking
            // the real (clean) diagnostic. The result type is fixed by the
            // stdlib §4.1 interface signatures: Contains → bool (i1); IndexOf →
            // int (i32); Add / Clear / Insert / Remove → unit
            // (i32). (`Remove` is bool in the spec but its result is unused at the
            // diagnosed call sites; i32 poison is harmless since the compile
            // already failed — and exprType, when resolved, overrides it.)
            llvm::Type* resTy = (m == "Contains") ? static_cast<llvm::Type*>(i1())
                                                  : static_cast<llvm::Type*>(i32());
            if (sema::TypeRef* rt = ctx_.exprType(call))
                if (llvm::Type* lt = lowerTypeRef(rt))
                    resTy = lt;
            return llvm::PoisonValue::get(resTy);
        }
    }

    if (!isArr && !isDict && !isStrBld && !lazyTerminal) return nullptr;

    // The materialized-array temp to free before the terminal returns its
    // result (null unless we materialized a lazy receiver above).
    llvm::Value* lazyTempArr = nullptr;
    // #1924: set when lazyTempArr is the CONCRETE fresh groups array
    // (Array<IGrouping>) from a bare-GroupBy scalar terminal — freeLazyTemp then
    // also frees every grouping (bz_groupby_free_groups) before the outer array.
    bool lazyTempIsGroupings = false;
    llvm::Value* recv = nullptr;
    // #1924 / stdlib §5 / §28.9: a bare `GroupBy(sel)` consumed by a SCALAR
    // terminal leaked the materialized groups — the outer Array<IGrouping> plus
    // every grouping's element array, key buffer, and group object. The #1914
    // for-in fix (bz_lazy_from_owned_grouping_array) could not be reused here: the
    // snapshot path (bz_lazy_to_array) drains-and-disposes the lazy BEFORE the
    // terminal reads it, so owning at the lazy would UAF a grouping pointer the
    // terminal returns. Fix: when the receiver is a bare GroupBy and the terminal
    // result is NOT itself a grouping pointer (Count/Any/All/Sum/Aggregate-with-
    // seed/ForEach/… — the fold reads each grouping DURING and returns a scalar),
    // materialize the groups CONCRETELY (they are already a bz_array_t* of
    // grouping pointers — no snapshot needed), run the array terminal over them,
    // then free the groupings + outer array AFTER the result is computed (each
    // freed exactly once, no UAF). An element-RETURNING terminal (result type
    // IGrouping: First/Single/Last/ElementAt/*OrNull/no-seed Aggregate) is handled
    // by the #1930 keep-path below (materialize concretely; free every grouping
    // EXCEPT the returned one; the returned grouping's own disposal is tied to the
    // result — scope-end for a proven non-escaping bound local, else it leaks).
    bool ownGroupByGroups = false;     // #1924 scalar terminal: free ALL groups after
    bool keepGroupByGrouping = false;  // #1930 element terminal: keep the returned one
    if (lazyTerminal && isGroupByCall(ma->target.get())) {
        // Gate on the TERMINAL's result type, not the method name: a result that is
        // NOT an IGrouping<…> pointer is a scalar (free ALL after); a result that IS
        // an IGrouping pointer is an element-returning terminal (keep the returned).
        auto isGroupingRef = [](const sema::TypeRef* t) {
            return t && t->kind == sema::TypeRef::Kind::Named && !t->declaration &&
                   t->astNode && t->astNode->name &&
                   !t->astNode->name->parts.empty() &&
                   t->headName() == "IGrouping";
        };
        bool resultIsGrouping = isGroupingRef(ctx_.exprType(call));
        ownGroupByGroups    = !resultIsGrouping;
        // #1930: the keep-path is enabled for the element-returning terminals whose
        // array lowering below identifies the returned grouping (so siblings can be
        // freed while it survives): the no-arg First/Single/Last/*OrNull and
        // ElementAt(i). Other grouping-returning shapes (predicate forms, no-seed
        // Aggregate) keep their existing borrowing-snapshot path — the returned
        // grouping is still reclaimed when it is bound to a proven non-escaping local
        // (groupingFreeEligible_), only its siblings are not (the safe direction).
        bool handledElemTerm =
            (call.args.empty() &&
             (m == "First" || m == "Single" || m == "Last" ||
              m == "FirstOrNull" || m == "LastOrNull" || m == "SingleOrNull")) ||
            (m == "ElementAt" && call.args.size() == 1);
        keepGroupByGrouping = resultIsGrouping && handledElemTerm;
    }
    if (ownGroupByGroups || keepGroupByGrouping) {
        auto* gbCall = dynamic_cast<const ast::CallExpr*>(ma->target.get());
        // Materialize directly to the concrete groups Array<IGrouping> (bypasses
        // the borrowing bz_lazy_from_array + bz_lazy_to_array snapshot the other
        // lazy terminals use). lowerGroupByMaterialize already drains+disposes the
        // GroupBy's own inner source (bz_groupby_to_array) and releases the key
        // closure — so only the fresh groups temporary is left for us to reclaim.
        llvm::Value* groups = lowerGroupByMaterialize(*gbCall);
        if (!groups) return nullptr;
        recv = groups;
        lazyTempArr = groups;
        lazyTempIsGroupings = true;
        isArr = true;  // route through the concrete-array terminal lowerings
    } else if (lazyTerminal) {
        sema::TypeRef* elemRef = nullptr;
        llvm::Value* lazy = lowerToLazyEnumerator(*ma->target, elemRef);
        if (!lazy) return nullptr;
        // #1252 / §29.11: a bound lazy var consumed by a prior for-loop or
        // terminal op holds null in its slot (the affine single-consumption null).
        // Guard the materialize so a terminal op on an already-consumed lazy raises
        // InvalidOperationException — matching the for-loop guard (#1226) — rather
        // than calling bz_lazy_to_array(null) (a crash) or draining freed memory.
        // ONLY for a genuine lazy receiver: LinkedList/Range/string/vtable sources
        // are wrapped fresh by lowerToLazyEnumerator and are never the null slot.
        if (lazyRecv) {
            llvm::Value* termIsNull = builder_.CreateICmpEQ(
                lazy, llvm::ConstantPointerNull::get(ptrTy()), "lz.term.isnull");
            emitGuardThrow(termIsNull, "InvalidOperationException");
        }
        // The materialized element byte size mirrors collectionElemSize: both
        // read the element TypeRef (genericArgs[0]). The lazy sentinel carries
        // it too, so collectionElemSize(recvTy) yields the same value the lazy
        // operators used when they were built (matching bz_lazy_to_array's esz).
        // Exception: for a string receiver, recvTy carries no genericArgs so
        // collectionElemSize returns 8 (wrong). char is always i32 = 4 bytes.
        int64_t esz = isStringRecvTerm ? 4 : collectionElemSize(recvTy);
        auto* toArr = declareC("bz_lazy_to_array", ptrTy(), {ptrTy(), i64()});
        recv = builder_.CreateCall(toArr, {lazy, builder_.getInt64(esz)},
                                   "linq.term.mat");
        lazyTempArr = recv;
        isArr = true;  // route through the concrete-array terminal lowerings
        // #1252 / §29.11: a TERMINAL op (Count/Sum/First/…) on a BOUND lazy var
        // drains and DISPOSES the lazy (bz_lazy_to_array), so the bound slot now
        // holds a freed bz_lazy_t*. Null the slot — exactly as the for-loop does
        // (#1226, LlvmBackend_Stmts) — so a subsequent for-loop or chained terminal
        // sees null and raises InvalidOperationException via the null-guard instead
        // of a use-after-free. ONLY for a genuine LAZY receiver: LinkedList / Range /
        // string / vtable-IEnumerable materializers do NOT free their source, so
        // their bound slots must stay valid for reuse. A fresh inline lazy
        // (chained CallExpr, no owning slot) has no localSlot → no-op.
        if (lazyRecv) {
            if (llvm::AllocaInst* boundSlot = localSlot(*ma->target)) {
                llvm::Type* ety = nullptr;
                llvm::Value* addr = boxedStorage(boundSlot, ety);
                builder_.CreateStore(
                    llvm::ConstantPointerNull::get(ptrTy()), addr);
            }
        }
    } else if (igroupingElemRef) {
        // stdlib §5 (#755): the receiver TYPE was restated as `Array<T>` above;
        // restate the VALUE to match — the grouping's element array. This is a
        // BORROW: the grouping owns the array and frees it in bz_grouping_free,
        // so no lazyTempArr is registered and nothing here frees it.
        llvm::Value* grp = lowerExpr(*ma->target);
        if (!grp) return nullptr;
        auto* elemsFn = declareC("bz_grouping_elements", ptrTy(), {ptrTy()});
        recv = builder_.CreateCall(elemsFn, {grp}, "igrp.elems");
    } else {
        recv = lowerExpr(*effectiveRecvExpr);
    }
    if (!recv) return nullptr;

    // Free the materialized lazy snapshot (no element disposal — it borrows).
    // A no-op when the receiver was already a concrete collection. Emitted at
    // each terminal's return site so the result value is computed (loaded) from
    // the array BEFORE the backing buffer is released.
    //
    // #1930: `keep` is the returned grouping pointer of an element-returning
    // terminal over a bare GroupBy (keepGroupByGrouping). When set, free every
    // grouping EXCEPT `keep` (bz_groupby_free_groups_except) — the returned grouping
    // stays live (the caller now holds it; freeing it would dangle the result). Its
    // own disposal is tied to the result: scope-end bz_grouping_free for a proven
    // non-escaping bound local (groupingFreeEligible_ / the `grouping` Disposal),
    // else it leaks (the safe direction). `keep` may be a runtime-null Value (an
    // *OrNull empty result) — then _except frees ALL (correct, nothing returned).
    auto freeLazyTemp = [&](llvm::Value* keep = nullptr) {
        if (!lazyTempArr) return;
        if (lazyTempIsGroupings) {
            if (keepGroupByGrouping) {
                // #1930 element terminal: free the SIBLING groups, keeping `keep`.
                // If the terminal did not identify `keep` (a C++ nullptr — a shape
                // this keep-path does not specially handle, e.g. a predicate form),
                // free NONE of the groupings — never risk freeing the returned one
                // (UAF); they leak (safe), only the array spine below is reclaimed.
                if (keep) {
                    auto* gfx = declareC("bz_groupby_free_groups_except",
                                         llvm::Type::getVoidTy(llvm_),
                                         {ptrTy(), ptrTy()});
                    builder_.CreateCall(gfx, {lazyTempArr, keep});
                }
            } else {
                // #1924 scalar terminal: a CONCRETE groups array (Array<IGrouping>)
                // this call owns. Free every grouping (element array + key data +
                // group object) BEFORE the outer array — this runs AFTER the terminal
                // computed its non-grouping result (the fold read each grouping
                // during), so each is freed exactly once and never after a use.
                auto* gfree = declareC("bz_groupby_free_groups",
                                       llvm::Type::getVoidTy(llvm_), {ptrTy()});
                builder_.CreateCall(gfree, {lazyTempArr});
            }
        }
        auto* fn = declareC("bz_array_free", llvm::Type::getVoidTy(llvm_),
                            {ptrTy(), ptrTy()});
        builder_.CreateCall(fn,
            {lazyTempArr, llvm::ConstantPointerNull::get(ptrTy())});
    };

    // #1004 / stdlib §5 / §3 / §28: free the INPUT array of a copy-form
    // (`.Sorted()`/`.Reversed()`) when the receiver expression (`ma->target`)
    // is itself a fresh, ownerless materializer temporary (`X.ToArray()` /
    // `X.Sorted()` / `X.Reversed()`) — that intermediate array is consumed only
    // as the copy source and otherwise unreferenced, so it must be reclaimed
    // after the copy or it leaks. Gated strictly on the receiver being a fresh
    // ownerless materializer CALL (isFreshOwnerlessArrayMaterializerExpr), so a
    // named-local / field / param / `move` / `new` receiver (a borrowed or
    // owned-elsewhere array) is NEVER freed here → no UAF, no double-free. The
    // backing is freed via bz_array_free; the per-element disposer matches what
    // ToArray/Sorted/Reversed retained: the string-release thunk for the
    // `Array<string>` co-owned +1 (released symmetrically — else the +1 leaks),
    // null for value/class element borrows. Call AFTER the copy is built and
    // (for the comparer form) BEFORE any closure release, mirroring the copy's
    // own retain. Idempotent guards mean a non-materializer receiver is a no-op.
    auto freeFreshInputTemp = [&]() {
        if (lazyTempArr) return;  // the lazy-terminal path frees via freeLazyTemp
        // stdlib §5 (#755): `recv` is an IGrouping's element array — the GROUPING
        // owns it (bz_grouping_free), and `ma->target` is the grouping, not a
        // fresh array materializer. Never reclaim it here.
        if (igroupingElemRef) return;
        // #1004: a builtin fresh-array materializer receiver (ToArray/Sorted/
        // Reversed). #1006(b): generalized along the `owned` axis — an UNBOUND
        // `owned`-returning USER method call used as the chained receiver (e.g.
        // `P.Fold(s).Sorted()` where `Fold(): owned Array<int>`). Both produce a
        // fresh borrowing `Array<T>` no name owns; freeing the backing here (with
        // the same null/string-release element disposer) reclaims the intermediate
        // at end-of-full-expression. A bound/`move`d/returned/owned-arg receiver is
        // not a chained-receiver CallExpr of this shape → not freed → no UAF.
        freeUnboundOwnedBorrowingArrayTemp(ma->target.get(), recv, recvTy);
    };

    // -----------------------------------------------------------------------
    // Array / Stack / Queue / Set / LinkedList methods
    // -----------------------------------------------------------------------
    if (isArr) {
        // fix #1856 / §5 / §9.6: for a STRING terminal receiver the array-backed
        // path runs over the char array materialized at the `lazyTerminal` step
        // above via bz_lazy_to_array(lazy, /*esz=*/4) — one i32 code point per
        // slot. recvTy is the `string` primitive (no genericArgs), so
        // collectionElemSize(recvTy) reports 8, NOT 4; using it as the element
        // stride made bz_array_ptr_at compute idx*8 over a 4-byte-stride buffer,
        // reading one element (4 bytes) past the end on the last iteration
        // (heap-buffer-overflow in .Any/.All/.Count(pred)/.First/…). Mirror the
        // materialization's element size exactly (matches the `esz` at line ~845).
        int64_t esz = isStringRecvTerm ? 4 : collectionElemSize(recvTy);

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

        // issues #343 / D8 / §4.10: a Set<T>/OwningSet<T> is backed by the
        // generic-key hash table (allocated in the `new` path). Route its core
        // membership operations to the generic dict entry points so set
        // semantics follow the element's hash/equality (§8.3). This covers
        // records (#343), primitive/enum elements, AND string elements (D8) —
        // none of which the array backing could deduplicate. Element value is a
        // non-null sentinel; only membership matters. Other Set operations
        // (LINQ, set-algebra) are not wired in this slice and fall through.
        bool isSetName = recvTy && recvTy->astNode && recvTy->astNode->name &&
                         !recvTy->astNode->name->parts.empty() &&
                         (recvTy->headName() == "Set" ||
                          recvTy->headName() == "OwningSet" ||
                          // stdlib §4.18 (#1426): SortedSet<T> shares the generic-key
                          // hash table backing of Set/OwningSet.
                          recvTy->headName() == "SortedSet");
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
                // fix #1426 / stdlib §4.18: SortedSet<T>.First (least) and
                // SortedSet<T>.Last (greatest) — O(n) scan returning the
                // min/max element by K's CompareTo order.  Throws
                // InvalidOperationException on empty set (§4.18).
                // Only available on SortedSet; not on Set/OwningSet.
                bool isSortedSetRecv =
                    recvTy->headName() == "SortedSet";
                if (isSortedSetRecv &&
                    (m == "First" || m == "Last") && call.args.empty()) {
                    bool isMax = (m == "Last");
                    // Guard: empty set throws InvalidOperationException.
                    auto* cntFn = declareC("bz_dict_count", i64(), {ptrTy()});
                    llvm::Value* cnt = builder_.CreateCall(cntFn, {recv}, "ss.cnt");
                    emitGuardThrow(
                        builder_.CreateICmpEQ(cnt, builder_.getInt64(0)),
                        "InvalidOperationException");
                    // Build comparator for the element type.
                    llvm::Function* cmpFn = buildSortedKeyComparator(elemTy);
                    if (!cmpFn) return nullptr;  // unsupported elem type
                    // Call runtime: returns malloc'd copy of min/max key bytes.
                    const char* rtName = isMax ? "bz_dict_max_key_gen"
                                               : "bz_dict_min_key_gen";
                    auto* minmaxFn = declareC(rtName, ptrTy(), {ptrTy(), ptrTy()});
                    llvm::Value* keyPtr = builder_.CreateCall(
                        minmaxFn, {recv, cmpFn}, "ss.mm");
                    // Load the element value from the returned bytes, then free.
                    llvm::Type* elemLlvm = lowerTypeRef(elemTy);
                    if (!elemLlvm) return nullptr;
                    llvm::Value* elemVal = builder_.CreateLoad(
                        elemLlvm, keyPtr, "ss.elem");
                    auto* freeFn = declareC("free",
                        llvm::Type::getVoidTy(llvm_), {ptrTy()});
                    builder_.CreateCall(freeFn, {keyPtr});
                    return elemVal;
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
            if (i >= call.args.size() || !call.args[i].value) return false;
            const ast::Expr* a = call.args[i].value.get();
            if (dynamic_cast<const ast::LambdaExpr*>(a)) return true;
            // §27.6 (v0.184, #1758): a bare primitive-intrinsic method reference
            // (`arr.Any(char.IsDigit)`) is likewise a fresh +1 closure the eager
            // op fully consumes and must release at loop-end — same inline-
            // producer shape as a lambda literal (#1758 pre-gate review).
            return isPrimIntrinsicMethodRef(a);
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
            // #1006(b): the scalar length is fully read above, so an UNBOUND owned
            // borrowing-array receiver (`P.Fold(s).Length`) can be reclaimed now —
            // a borrow-position member access leaving the temporary otherwise
            // unreferenced. No-op unless the receiver is exactly that shape.
            freeFreshInputTemp();
            return len32;
        }
        // per §5 LINQ: .Sum(fn: (T)->int): int  /  .Sum(): int (for Array<int>)
        // fix #666: also handles Sum over decimal (struct accumulator via bz_decimal_add).
        if (m == "Sum") {
            const bool relCl = argIsInlineLambda(0);
            llvm::Type* elemTy = getElemTy();

            // fix #666 / stdlib §5: decimal Sum — element/selector result is a
            // 16-byte %bz.decimal struct; accumulate via bz_decimal_add into an
            // alloca'd slot (pointer-based ABI, same as all other decimal ops).
            // Selector form: closure returns decimal struct by value (LLVM IR ABI).
            // No-selector form: element is loaded as decimal struct directly.
            llvm::StructType* decTy = decimalStructType();
            bool isDecimalSum = (elemTy == decTy);
            // If the selector returns decimal, also use the decimal path.
            if (!isDecimalSum && !call.args.empty() && call.args[0].value) {
                if (auto* lamTy = ctx_.exprType(*call.args[0].value))
                    if (lamTy->kind == sema::TypeRef::Kind::Function && lamTy->funcReturn)
                        if (auto* rt = lamTy->funcReturn)
                            if (rt->kind == sema::TypeRef::Kind::Primitive &&
                                rt->primitive == sema::PrimitiveType::Decimal)
                                isDecimalSum = true;
            }
            if (isDecimalSum) {
                // fix #686: detect nullable-decimal elements (Array<decimal?>).
                // elemTy for decimal? is {i8, bz.decimal} (not decTy itself).
                // We must load the full {i8,decimal} struct from the element slot
                // and pass it to the closure with the correct ABI, not truncate to
                // decTy (which misreads the 17-byte slot as 16 bytes).
                auto* nullDecTy = nullablePrimStructTy(sema::PrimitiveType::Decimal);
                bool isNullableDecElem = (elemTy == nullDecTy);
                // Actual load type for element: {i8,decimal} for decimal?, decimal for decimal.
                llvm::Type* loadTy = isNullableDecElem ? static_cast<llvm::Type*>(nullDecTy)
                                                       : static_cast<llvm::Type*>(decTy);

                // Lower selector with decimal return TypeRef so unannotated lambdas
                // get the right param hint.
                sema::TypeRef* decRetRef = ctx_.primitiveTypeRef(sema::PrimitiveType::Decimal);
                llvm::Value* closure = call.args.empty() ? nullptr
                    : lowerLinqLambda(call.args[0].value.get(), decRetRef);
                if (!call.args.empty() && !closure) return nullptr;
                auto* lenFn = declareC("bz_array_len", i64(), {ptrTy()});
                llvm::Value* len = builder_.CreateCall(lenFn, {recv});
                auto* idxSlot = entryAlloca(i64());
                // Accumulator: alloca'd decimal slot, zero-initialised.
                auto* accSlot = entryAlloca(decTy);
                builder_.CreateStore(llvm::ConstantAggregateZero::get(decTy), accSlot);
                builder_.CreateStore(builder_.getInt64(0), idxSlot);
                auto* addFn = declareC("bz_decimal_add", llvm::Type::getVoidTy(llvm_),
                                       {ptrTy(), ptrTy(), ptrTy(), ptrTy()});
                // OverflowException vtable (null if not yet set up — decimal sum
                // overflow is extremely unlikely in practice but spec-required).
                auto getOVEVtable = [&]() -> llvm::Value* {
                    auto vit = builtinVtable_.find("OverflowException");
                    if (vit == builtinVtable_.end())
                        return llvm::ConstantPointerNull::get(ptrTy());
                    return vit->second;
                };
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
                // fix #686: load element at the correct width.
                // For decimal?: load full {i8,decimal} struct (NOT just decTy).
                // For decimal:  load decTy directly (as before).
                llvm::Value* el = builder_.CreateLoad(loadTy, ep);
                // Apply selector if present.
                // fix #686: closure ABI uses loadTy (element's actual LLVM type)
                // as the param type; return type is always decTy.
                llvm::Value* val = el;
                if (closure) {
                    llvm::Value* fnPtr = builder_.CreateLoad(ptrTy(), closure, "cl.fn");
                    llvm::Value* envSlot = builder_.CreateGEP(ptrTy(), closure,
                                                              {builder_.getInt32(1)});
                    llvm::Value* envPtr = builder_.CreateLoad(ptrTy(), envSlot, "cl.env");
                    // fix #686: param type is loadTy (decimal? → {i8,decimal};
                    // decimal → decTy). The selector returns decimal (decTy).
                    auto* fnTy = llvm::FunctionType::get(decTy, {ptrTy(), loadTy}, false);
                    val = builder_.CreateCall(fnTy, fnPtr, {envPtr, el}, "cl.dec");
                }
                // Store val into a temp slot for the add call.
                auto* valSlot = entryAlloca(decTy);
                builder_.CreateStore(val, valSlot);
                // out slot for the add result (reuse accSlot after copying).
                auto* outSlot = entryAlloca(decTy);
                builder_.CreateCall(addFn, {accSlot, valSlot, outSlot, getOVEVtable()});
                emitPendingCheck();
                // Copy result back to accSlot.
                llvm::Value* addResult = builder_.CreateLoad(decTy, outSlot);
                builder_.CreateStore(addResult, accSlot);
                builder_.CreateStore(builder_.CreateAdd(idx, builder_.getInt64(1)), idxSlot);
                builder_.CreateBr(condBB);
                builder_.SetInsertPoint(endBB);
                if (relCl && closure) emitClosureRelease(closure);
                freeLazyTemp();
                return builder_.CreateLoad(decTy, accSlot, "sum.r");
            }

            // fix #1757 / stdlib §5 / §22.6: accumulate the sum in the
            // *selected* Sum overload's result type — the selector's return
            // type R (one of int/uint/long/ulong/float/double; decimal took the
            // path above, and the ambiguous co-equal-convert case, e.g. a byte
            // selector, was rejected in sema). The pre-fix accumulator was
            // hardwired to i32, which truncated a long/uint/ulong selector's
            // result to 32 bits and emitted malformed IR for a float/double
            // selector (its i32-typed sum then mismatched the double-typed use
            // site — #1697/#1757). Derive the accumulator LLVM type from R.
            //
            // fix #1836 / stdlib §5.1 (v0.226): the NO-SELECTOR Sum() form
            // accumulates in the element's OWN numeric type (long/uint/ulong/
            // float/double — `Sum(): long` for Array<long>), not int. The pre-
            // fix hardwire of R = int for the no-selector form truncated a
            // long/ulong element's sum to 32 bits (9_000_000_000 mod 2^32 =
            // 410_065_408). Decimal took the isDecimalSum path above.
            sema::PrimitiveType retP = sema::PrimitiveType::Int;
            if (!call.args.empty() && call.args[0].value) {
                if (auto* lamTy = ctx_.exprType(*call.args[0].value))
                    if (lamTy->kind == sema::TypeRef::Kind::Function &&
                        lamTy->funcReturn &&
                        lamTy->funcReturn->kind ==
                            sema::TypeRef::Kind::Primitive &&
                        !lamTy->funcReturn->nullable) {
                        switch (lamTy->funcReturn->primitive) {
                            case sema::PrimitiveType::Int:
                            case sema::PrimitiveType::UInt:
                            case sema::PrimitiveType::Long:
                            case sema::PrimitiveType::ULong:
                            case sema::PrimitiveType::Float:
                            case sema::PrimitiveType::Double:
                                retP = lamTy->funcReturn->primitive;
                                break;
                            default:
                                break;  // keep Int (legacy) for anything else
                        }
                    }
            } else if (recvTy && !recvTy->genericArgs.empty() &&
                       recvTy->genericArgs[0]) {
                // no-selector: per stdlib §5.1 the result is the element's own
                // numeric type. Sema types it likewise (ExprTypecheck_Calls);
                // derive the accumulator width from the element so a long/uint/
                // ulong/float/double element is summed at its own width.
                sema::TypeRef* e = recvTy->genericArgs[0];
                if (e->kind == sema::TypeRef::Kind::Primitive && !e->nullable) {
                    switch (e->primitive) {
                        case sema::PrimitiveType::UInt:
                        case sema::PrimitiveType::Long:
                        case sema::PrimitiveType::ULong:
                        case sema::PrimitiveType::Float:
                        case sema::PrimitiveType::Double:
                            retP = e->primitive;
                            break;
                        default:
                            break;  // Int stays; decimal handled above
                    }
                }
            }
            sema::TypeRef* retTyRef = ctx_.primitiveTypeRef(retP);
            const bool accIsFP = (retP == sema::PrimitiveType::Float ||
                                  retP == sema::PrimitiveType::Double);
            llvm::Type* accLLTy = lowerTypeRef(retTyRef);
            if (!accLLTy) accLLTy = i32();
            llvm::Value* closure = call.args.empty() ? nullptr
                : lowerLinqLambda(call.args[0].value.get(), retTyRef);
            if (!call.args.empty() && !closure) return nullptr;
            auto* lenFn = declareC("bz_array_len", i64(), {ptrTy()});
            llvm::Value* len = builder_.CreateCall(lenFn, {recv});
            auto* idxSlot = entryAlloca(i64());
            auto* accSlot = entryAlloca(accLLTy);
            builder_.CreateStore(builder_.getInt64(0), idxSlot);
            builder_.CreateStore(
                accIsFP ? static_cast<llvm::Value*>(
                              llvm::ConstantFP::get(accLLTy, 0.0))
                        : static_cast<llvm::Value*>(
                              llvm::ConstantInt::get(accLLTy, 0)),
                accSlot);
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
            llvm::Value* val = closure ? callClosure(closure, el, accLLTy) : el;
            // fix #1836: with the no-selector accumulator now derived from the
            // element type (#1836) and the selector accumulator from the
            // selector return (#1757), val already matches accLLTy on every
            // path; this integer-only narrowing is a defensive no-op kept for
            // the int-default fallback (a non-numeric element).
            if (val->getType() != accLLTy && val->getType()->isIntegerTy() &&
                accLLTy->isIntegerTy())
                val = builder_.CreateTrunc(val, accLLTy);
            llvm::Value* acc = builder_.CreateLoad(accLLTy, accSlot);
            builder_.CreateStore(
                accIsFP ? builder_.CreateFAdd(acc, val)
                        : builder_.CreateAdd(acc, val),
                accSlot);
            builder_.CreateStore(builder_.CreateAdd(idx, builder_.getInt64(1)), idxSlot);
            builder_.CreateBr(condBB);
            builder_.SetInsertPoint(endBB);
            if (relCl && closure) emitClosureRelease(closure);  // #422: inline-lambda
            freeLazyTemp();  // FINDING [5]: release materialized lazy snapshot
            return builder_.CreateLoad(accLLTy, accSlot, "sum.r");
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
        // fix #666 / stdlib §5: Average(selector: (T)->decimal): decimal
        //   and IEnumerable<decimal>.Average(): decimal — accumulate via
        //   bz_decimal_add, then divide by count via bz_decimal_div.
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

            // Detect decimal selector or decimal element type.
            llvm::StructType* decTy = decimalStructType();
            // fix #686: also detect nullable-decimal element type ({i8,decimal}).
            auto* nullDecTyAvg = nullablePrimStructTy(sema::PrimitiveType::Decimal);
            bool isNullableDecElemAvg = (elemTy == nullDecTyAvg);
            bool isDecimalAvg = (elemTy == decTy || selRetTy == decTy ||
                                 isNullableDecElemAvg);
            if (isDecimalAvg) {
                // stdlib §5 (L2589) / §5.1 (L2676):
                //   Average(selector: (T)->decimal): decimal
                //   IEnumerable<decimal>.Average(): decimal
                // Accumulate via bz_decimal_add; divide sum by count via bz_decimal_div.
                // fix #686: load type for element: {i8,decimal} for decimal?,
                // decimal for decimal.
                llvm::Type* loadTyAvg = isNullableDecElemAvg
                    ? static_cast<llvm::Type*>(nullDecTyAvg)
                    : static_cast<llvm::Type*>(decTy);
                sema::TypeRef* decRetRef = ctx_.primitiveTypeRef(sema::PrimitiveType::Decimal);
                llvm::Value* closure = call.args.empty() ? nullptr
                    : lowerLinqLambda(call.args[0].value.get(), decRetRef);
                if (!call.args.empty() && !closure) return nullptr;
                auto* lenFn = declareC("bz_array_len", i64(), {ptrTy()});
                llvm::Value* len = builder_.CreateCall(lenFn, {recv});
                auto* idxSlot = entryAlloca(i64());
                auto* accSlot = entryAlloca(decTy);
                builder_.CreateStore(llvm::ConstantAggregateZero::get(decTy), accSlot);
                builder_.CreateStore(builder_.getInt64(0), idxSlot);
                auto* addFn = declareC("bz_decimal_add", llvm::Type::getVoidTy(llvm_),
                                       {ptrTy(), ptrTy(), ptrTy(), ptrTy()});
                auto getOVEVtable = [&]() -> llvm::Value* {
                    auto vit = builtinVtable_.find("OverflowException");
                    if (vit == builtinVtable_.end())
                        return llvm::ConstantPointerNull::get(ptrTy());
                    return vit->second;
                };
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
                // fix #686: load at correct width (nullable-decimal vs decimal).
                llvm::Value* el = builder_.CreateLoad(loadTyAvg, ep);
                llvm::Value* val = el;
                if (closure) {
                    llvm::Value* fnPtr = builder_.CreateLoad(ptrTy(), closure, "cl.fn");
                    llvm::Value* envSlot = builder_.CreateGEP(ptrTy(), closure,
                                                              {builder_.getInt32(1)});
                    llvm::Value* envPtr = builder_.CreateLoad(ptrTy(), envSlot, "cl.env");
                    // fix #686: param type is loadTyAvg.
                    auto* fnTy = llvm::FunctionType::get(decTy, {ptrTy(), loadTyAvg}, false);
                    val = builder_.CreateCall(fnTy, fnPtr, {envPtr, el}, "cl.dec");
                }
                auto* valSlot = entryAlloca(decTy);
                builder_.CreateStore(val, valSlot);
                auto* outSlot = entryAlloca(decTy);
                builder_.CreateCall(addFn, {accSlot, valSlot, outSlot, getOVEVtable()});
                emitPendingCheck();
                builder_.CreateStore(builder_.CreateLoad(decTy, outSlot), accSlot);
                builder_.CreateStore(builder_.CreateAdd(idx, builder_.getInt64(1)), idxSlot);
                builder_.CreateBr(condBB);
                builder_.SetInsertPoint(endBB);
                // Divide sum by count: bz_decimal_from_int64(len) then bz_decimal_div.
                auto* cntSlot = entryAlloca(decTy);
                auto* fromI64 = declareC("bz_decimal_from_int64",
                                         llvm::Type::getVoidTy(llvm_), {i64(), ptrTy()});
                builder_.CreateCall(fromI64, {len, cntSlot});
                auto* divSlot = entryAlloca(decTy);
                auto* divFn = declareC("bz_decimal_div", llvm::Type::getVoidTy(llvm_),
                                       {ptrTy(), ptrTy(), ptrTy(), ptrTy()});
                builder_.CreateCall(divFn, {accSlot, cntSlot, divSlot, getOVEVtable()});
                emitPendingCheck();
                if (relCl && closure) emitClosureRelease(closure);
                freeLazyTemp();
                return builder_.CreateLoad(decTy, divSlot, "avg.r");
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
        // fix #666 / stdlib §5.1: also handles Min/Max over Array<decimal> via bz_decimal_cmp.
        if ((m == "Min" || m == "Max") &&
            (call.args.empty() || call.args.size() == 1)) {
            bool isMax = (m == "Max");
            const bool relCl = argIsInlineLambda(0);
            llvm::Type* elemTy = getElemTy();

            // Detect decimal element type (no selector case: stdlib §5.1 T: IComparable<T>).
            llvm::StructType* decTy = decimalStructType();
            // fix #686: also handle decimal selector with nullable-decimal elements.
            auto* nullDecTyMM = nullablePrimStructTy(sema::PrimitiveType::Decimal);
            bool isNullableDecElemMM = (elemTy == nullDecTyMM);
            // Check if the selector (if any) returns decimal.
            bool decSelectorMM = false;
            if (!call.args.empty() && call.args[0].value)
                if (auto* lamTy = ctx_.exprType(*call.args[0].value))
                    if (lamTy->kind == sema::TypeRef::Kind::Function && lamTy->funcReturn)
                        if (lamTy->funcReturn->kind == sema::TypeRef::Kind::Primitive &&
                            lamTy->funcReturn->primitive == sema::PrimitiveType::Decimal)
                            decSelectorMM = true;
            if (elemTy == decTy && call.args.empty()) {
                // stdlib §5.1: Array<decimal>.Min()/Max(): decimal
                // Throws InvalidOperationException on empty source.
                auto* lenFn = declareC("bz_array_len", i64(), {ptrTy()});
                llvm::Value* len = builder_.CreateCall(lenFn, {recv});
                llvm::Value* isEmpty = builder_.CreateICmpEQ(len, builder_.getInt64(0));
                emitGuardThrow(isEmpty, "InvalidOperationException");
                auto* idxSlot  = entryAlloca(i64());
                auto* accSlot  = entryAlloca(decTy);
                auto* initSlot = entryAlloca(i1());
                builder_.CreateStore(builder_.getInt64(0), idxSlot);
                builder_.CreateStore(llvm::ConstantAggregateZero::get(decTy), accSlot);
                builder_.CreateStore(builder_.getFalse(), initSlot);
                auto* cmpFn = declareC("bz_decimal_cmp", i32(), {ptrTy(), ptrTy()});
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
                // Store element into a temp slot for bz_decimal_cmp.
                auto* valSlot = entryAlloca(decTy);
                llvm::Value* el = builder_.CreateLoad(decTy, ep);
                builder_.CreateStore(el, valSlot);
                // Compare val vs acc: cmp = bz_decimal_cmp(val, acc)
                // isMax: update when cmp > 0 (val > acc)
                // isMin: update when cmp < 0 (val < acc)
                llvm::Value* cmpResult = builder_.CreateCall(cmpFn, {valSlot, accSlot}, "dec.cmp");
                llvm::Value* shouldUpdate = isMax
                    ? builder_.CreateICmpSGT(cmpResult, builder_.getInt32(0))
                    : builder_.CreateICmpSLT(cmpResult, builder_.getInt32(0));
                llvm::Value* inited = builder_.CreateLoad(i1(), initSlot);
                builder_.CreateCondBr(
                    builder_.CreateOr(builder_.CreateNot(inited), shouldUpdate),
                    updBB, nextBB);
                builder_.SetInsertPoint(updBB);
                builder_.CreateStore(el, accSlot);
                builder_.CreateStore(builder_.getTrue(), initSlot);
                builder_.CreateBr(nextBB);
                builder_.SetInsertPoint(nextBB);
                builder_.CreateStore(builder_.CreateAdd(idx, builder_.getInt64(1)), idxSlot);
                builder_.CreateBr(condBB);
                builder_.SetInsertPoint(endBB);
                freeLazyTemp();
                return builder_.CreateLoad(decTy, accSlot, "mm.r");
            }

            // fix #686 / stdlib §5.1: Min/Max with decimal selector
            // (e.g. Array<decimal?>.Min((x) => x ?? sentinel)).
            // loadTy is the element LLVM type: {i8,decimal} for decimal?, decimal for decimal.
            if (decSelectorMM || (isNullableDecElemMM && call.args.size() == 1)) {
                llvm::Type* loadTyMM = isNullableDecElemMM
                    ? static_cast<llvm::Type*>(nullDecTyMM)
                    : static_cast<llvm::Type*>(decTy);
                sema::TypeRef* decRetRef = ctx_.primitiveTypeRef(sema::PrimitiveType::Decimal);
                llvm::Value* closure = lowerLinqLambda(call.args[0].value.get(), decRetRef);
                if (!closure) return nullptr;
                const bool relClMM = argIsInlineLambda(0);
                auto* lenFn = declareC("bz_array_len", i64(), {ptrTy()});
                llvm::Value* len = builder_.CreateCall(lenFn, {recv});
                llvm::Value* isEmpty = builder_.CreateICmpEQ(len, builder_.getInt64(0));
                emitGuardThrow(isEmpty, "InvalidOperationException");
                auto* idxSlot  = entryAlloca(i64());
                auto* accSlot  = entryAlloca(decTy);
                auto* initSlot = entryAlloca(i1());
                builder_.CreateStore(builder_.getInt64(0), idxSlot);
                builder_.CreateStore(llvm::ConstantAggregateZero::get(decTy), accSlot);
                builder_.CreateStore(builder_.getFalse(), initSlot);
                auto* cmpFn = declareC("bz_decimal_cmp", i32(), {ptrTy(), ptrTy()});
                auto* condBB = llvm::BasicBlock::Create(llvm_, "mm.c", currentFn_);
                auto* bodyBB = llvm::BasicBlock::Create(llvm_, "mm.b", currentFn_);
                auto* updBB  = llvm::BasicBlock::Create(llvm_, "mm.u", currentFn_);
                auto* nextBB = llvm::BasicBlock::Create(llvm_, "mm.n", currentFn_);
                auto* endBB  = llvm::BasicBlock::Create(llvm_, "mm.e", currentFn_);
                builder_.CreateBr(condBB);
                builder_.SetInsertPoint(condBB);
                llvm::Value* idx = builder_.CreateLoad(i64(), idxSlot);
                builder_.CreateCondBr(builder_.CreateICmpSLT(idx, len), bodyBB, endBB);
                builder_.SetInsertPoint(bodyBB);
                auto* ptrFn = declareC("bz_array_ptr_at", ptrTy(), {ptrTy(), i64(), i64()});
                llvm::Value* ep = builder_.CreateCall(ptrFn, {recv, idx, builder_.getInt64(esz)});
                // fix #686: load at correct width per element type.
                llvm::Value* el = builder_.CreateLoad(loadTyMM, ep);
                // Call selector: (env, elem) -> decimal.
                llvm::Value* fnPtr = builder_.CreateLoad(ptrTy(), closure, "cl.fn");
                llvm::Value* envSlotP = builder_.CreateGEP(ptrTy(), closure, {builder_.getInt32(1)});
                llvm::Value* envPtr = builder_.CreateLoad(ptrTy(), envSlotP, "cl.env");
                auto* fnTyMM = llvm::FunctionType::get(decTy, {ptrTy(), loadTyMM}, false);
                llvm::Value* selVal = builder_.CreateCall(fnTyMM, fnPtr, {envPtr, el}, "cl.dec");
                auto* valSlot = entryAlloca(decTy);
                builder_.CreateStore(selVal, valSlot);
                llvm::Value* cmpResult = builder_.CreateCall(cmpFn, {valSlot, accSlot}, "dec.cmp");
                llvm::Value* shouldUpdate = isMax
                    ? builder_.CreateICmpSGT(cmpResult, builder_.getInt32(0))
                    : builder_.CreateICmpSLT(cmpResult, builder_.getInt32(0));
                llvm::Value* inited = builder_.CreateLoad(i1(), initSlot);
                builder_.CreateCondBr(
                    builder_.CreateOr(builder_.CreateNot(inited), shouldUpdate),
                    updBB, nextBB);
                builder_.SetInsertPoint(updBB);
                builder_.CreateStore(selVal, accSlot);
                builder_.CreateStore(builder_.getTrue(), initSlot);
                builder_.CreateBr(nextBB);
                builder_.SetInsertPoint(nextBB);
                builder_.CreateStore(builder_.CreateAdd(idx, builder_.getInt64(1)), idxSlot);
                builder_.CreateBr(condBB);
                builder_.SetInsertPoint(endBB);
                if (relClMM) emitClosureRelease(closure);
                freeLazyTemp();
                return builder_.CreateLoad(decTy, accSlot, "mm.r");
            }

            // per stdlib §5 / §8.4 (#1194): no-selector Min()/Max() over an
            // Array whose element is a PTR-typed IComparable<T> — a user class
            // implementing `IComparable<T>` (§14.2), or `string` (§9.6). The
            // generic i32 path below loads the element and truncs it to i32
            // (`trunc ptr … to i32` → malformed IR). Instead, dispatch the
            // element ordering through the element's `CompareTo` (string:
            // ordinal strcmp on the data pointers, mirroring the Sort()
            // comparator), returning the winning element pointer. (Related:
            // Math.Min/Max IComparable, #1148/#1141.)
            if (call.args.empty()) {
                sema::TypeRef* elemTypeRef =
                    (recvTy && !recvTy->genericArgs.empty())
                        ? recvTy->genericArgs[0] : nullptr;
                bool isStrElem = isStringTypeRefNonNull(elemTypeRef);
                sema::MethodSymbol* cmpMs = nullptr;
                if (!isStrElem && elemTypeRef &&
                    elemTypeRef->kind == sema::TypeRef::Kind::Named &&
                    elemTypeRef->declaration &&
                    (elemTypeRef->declaration->kind == ast::TypeDeclKind::Class ||
                     elemTypeRef->declaration->kind == ast::TypeDeclKind::Interface)) {
                    if (sema::MethodSymbol* cm =
                            findMethodByName(elemTypeRef->declaration, "CompareTo"))
                        if (cm->params.size() == 1) cmpMs = cm;
                }
                // fix #2770 (stdlib §5.1, spec §14.2, v0.240): an INTERFACE
                // element never satisfies the probe above — findMethodByName
                // walks CLASS bases only (baseClassOf) and additionally demands
                // `methodFns_.count(ms)`, a COMPILED BODY, while the element's
                // `CompareTo` is declared on `IComparable` and has none. So
                // `cmpMs` stayed null, this whole arm was skipped, and the
                // numeric path below built an `icmp` between a pointer and an
                // i32 — LLVM's `ICmpInst::AssertOK` fired and the compiler
                // ABORTED. stdlib §5.1 declares these forms in
                //     public extension IEnumerable<T> where T: IComparable<T>
                //       public method Min(): T;  public method Max(): T;
                // and spec §14.2 conformance rule 3 admits an interface
                // substitution — "**T is an interface** that has I in its
                // extension chain" — so this is a document-admissible shape
                // that must compile and must order by the user's CompareTo.
                //
                // Reuses #2768's `bz$ifacecmp$<Iface>` thunk UNCHANGED: its
                // `bz_array_sort` ABI takes the ADDRESS of each element slot,
                // and `ep` (from bz_array_ptr_at) and `accSlot` are exactly
                // that, so no second thunk and no generalization is needed.
                // The arm is chosen from the STATIC element type by the exact
                // §5.1 constraint (see selfComparableIfaceCompareTo), never by
                // a runtime word test — so, as in #2768, no other element
                // category can be mis-routed into it. Tried only when `cmpMs`
                // is null, so every shape that works today is bit-identical.
                llvm::Function* ifaceCmpFn = nullptr;
                if (!isStrElem && !cmpMs) {
                    if (sema::MethodSymbol* ifm =
                            selfComparableIfaceCompareTo(elemTypeRef))
                        ifaceCmpFn = ensureIfaceCmpThunk(
                            elemTypeRef->declaration, ifm);
                }
                if (isStrElem || cmpMs || ifaceCmpFn) {
                    // Element is a pointer (string object / class or interface
                    // instance) in all cases.
                    auto* lenFn = declareC("bz_array_len", i64(), {ptrTy()});
                    llvm::Value* len = builder_.CreateCall(lenFn, {recv});
                    // stdlib §5.1: Min()/Max() throw on an empty source.
                    llvm::Value* isEmpty =
                        builder_.CreateICmpEQ(len, builder_.getInt64(0));
                    emitGuardThrow(isEmpty, "InvalidOperationException");
                    auto* ptrFn = declareC("bz_array_ptr_at", ptrTy(),
                                           {ptrTy(), i64(), i64()});
                    // Seed the accumulator with element[0] and scan from index 1
                    // — this avoids comparing against a null accumulator on the
                    // first iteration (a CompareTo/strcmp on null would fault).
                    auto* idxSlot = entryAlloca(i64());
                    auto* accSlot = entryAlloca(ptrTy());
                    llvm::Value* p0 = builder_.CreateCall(
                        ptrFn, {recv, builder_.getInt64(0), builder_.getInt64(esz)});
                    builder_.CreateStore(builder_.CreateLoad(ptrTy(), p0), accSlot);
                    builder_.CreateStore(builder_.getInt64(1), idxSlot);
                    auto* condBB = llvm::BasicBlock::Create(llvm_, "mm.c", currentFn_);
                    auto* bodyBB = llvm::BasicBlock::Create(llvm_, "mm.b", currentFn_);
                    auto* updBB  = llvm::BasicBlock::Create(llvm_, "mm.u", currentFn_);
                    auto* nextBB = llvm::BasicBlock::Create(llvm_, "mm.n", currentFn_);
                    auto* endBB  = llvm::BasicBlock::Create(llvm_, "mm.e", currentFn_);
                    builder_.CreateBr(condBB);
                    builder_.SetInsertPoint(condBB);
                    llvm::Value* idx = builder_.CreateLoad(i64(), idxSlot);
                    builder_.CreateCondBr(builder_.CreateICmpSLT(idx, len),
                                          bodyBB, endBB);
                    builder_.SetInsertPoint(bodyBB);
                    llvm::Value* ep = builder_.CreateCall(
                        ptrFn, {recv, idx, builder_.getInt64(esz)});
                    llvm::Value* el  = builder_.CreateLoad(ptrTy(), ep, "mm.el");
                    llvm::Value* acc = builder_.CreateLoad(ptrTy(), accSlot, "mm.acc");
                    // cmp = (el <=> acc): el.CompareTo(acc), or strcmp(el, acc).
                    llvm::Value* cmp = nullptr;
                    if (isStrElem) {
                        llvm::Value* elData = builder_.CreateLoad(ptrTy(),
                            builder_.CreateStructGEP(stringType(), el, 0));
                        llvm::Value* accData = builder_.CreateLoad(ptrTy(),
                            builder_.CreateStructGEP(stringType(), acc, 0));
                        auto* strcmpFn = module_->getFunction("strcmp");
                        if (!strcmpFn) {
                            auto* ft = llvm::FunctionType::get(
                                i32(), {ptrTy(), ptrTy()}, false);
                            strcmpFn = llvm::Function::Create(
                                ft, llvm::Function::ExternalLinkage, "strcmp",
                                module_.get());
                        }
                        cmp = builder_.CreateCall(strcmpFn, {elData, accData}, "mm.cmp");
                    } else if (ifaceCmpFn) {
                        // fix #2770: §13 itable dispatch of the element's own
                        // `CompareTo` — the identical dispatch a written-out
                        // `el.CompareTo(acc)` uses. `ep` and `accSlot` are slot
                        // ADDRESSES, which is the thunk's ABI.
                        cmp = builder_.CreateCall(ifaceCmpFn, {ep, accSlot},
                                                  "mm.cmp");
                    } else {
                        std::vector<llvm::Value*> cargs{el, acc};
                        cmp = emitMethodCall(cmpMs, cargs, /*baseQualified=*/false);
                        if (!cmp) return nullptr;
                    }
                    // isMax: replace acc when el > acc (cmp > 0).
                    // isMin: replace acc when el < acc (cmp < 0).
                    llvm::Value* zero = llvm::ConstantInt::get(cmp->getType(), 0);
                    llvm::Value* shouldUpdate = isMax
                        ? builder_.CreateICmpSGT(cmp, zero)
                        : builder_.CreateICmpSLT(cmp, zero);
                    builder_.CreateCondBr(shouldUpdate, updBB, nextBB);
                    builder_.SetInsertPoint(updBB);
                    builder_.CreateStore(el, accSlot);
                    builder_.CreateBr(nextBB);
                    builder_.SetInsertPoint(nextBB);
                    builder_.CreateStore(
                        builder_.CreateAdd(idx, builder_.getInt64(1)), idxSlot);
                    builder_.CreateBr(condBB);
                    builder_.SetInsertPoint(endBB);
                    freeLazyTemp();
                    return builder_.CreateLoad(ptrTy(), accSlot, "mm.r");
                }
                // fix #2770: a REFERENCE element (class or interface — both
                // lower to a pointer) for which no comparator could be
                // resolved must not fall through to the numeric path below.
                // That path derives an i32 accumulator and folds the result
                // with `icmp`, so it built `icmp ptr, i32` and LLVM's
                // `ICmpInst::AssertOK` ABORTED the compiler — the ICE this
                // issue reports. Sema's §5.1 gate scans the element's DIRECT
                // AST base list for the NAME `IComparable` without checking
                // its type argument or nullability, so it admits shapes the
                // interface arm above correctly declines, and every one of
                // them landed on that assertion. MEASURED to reach here:
                //   * `Array<IFoo?>` — spec §14.2: "A type-bound constraint
                //     — naming a class or interface — implies non-null T",
                //     so a nullable element does not satisfy §5.1's
                //     `where T: IComparable<T>`.
                //   * `interface IG<T>: IComparable<IG<int>>` with element
                //     `IG<string>` — §12.9, IComparable is INVARIANT, so this
                //     does not satisfy the self-recurrence either.
                // Reporting an Error and returning a type-correct null keeps
                // the IR well-formed and ends the build with a diagnostic
                // instead of an assertion failure. This can only affect
                // programs that abort the compiler today: sema rejects a
                // reference element whose type names no `IComparable` at all,
                // so the residue reaching here is exactly the ICE set.
                // fix #2784 (stdlib §5.1, spec §14.2, v0.240): this guard
                // originally also required `elemTypeRef->declaration`, and a
                // BUILTIN interface element has none — the whitelist at
                // TypeResolution_Names.cpp:1511 attaches a synthesized
                // Interface TypeSymbol to IComparable/IEquatable/
                // IEqualityComparer/IReadOnlySpan/INumber/ISteppable/IParsable
                // and NOT to IDisposable (root cause: #2789). So
                // `Array<IDisposable>.Min()` slipped past BOTH this guard and
                // sema's #1198 gate — which scans the element declaration's
                // base list and likewise found nothing to scan — and compiled
                // with NO diagnostic at all, returning a non-null pointer
                // chosen under no defined ordering. MEASURED: it returned 92
                // (a non-null result) where the user-defined analogue is
                // correctly rejected.
                //
                // The declaration requirement is therefore dropped: what makes
                // an element admissible here is that a comparator was RESOLVED
                // (the arms above), not that it happens to carry a declaration.
                // Anything reaching this point is a reference element with no
                // usable ordering, whatever its symbol representation. Same
                // reasoning as #2786's category-blind guard — enumerating the
                // shapes that may arrive is exactly how this family kept
                // acquiring new members.
                //
                // `headName()` is used for the message because it is defined
                // for a Named TypeRef WITHOUT a declaration (it falls back to
                // the syntactic head, then to the metadata importedName) and
                // it resolves through a type alias (#1462).
                if (elemTypeRef &&
                    elemTypeRef->kind == sema::TypeRef::Kind::Named &&
                    (!elemTypeRef->declaration ||
                     elemTypeRef->declaration->kind == ast::TypeDeclKind::Class ||
                     elemTypeRef->declaration->kind ==
                         ast::TypeDeclKind::Interface)) {
                    std::string_view en = elemTypeRef->declaration
                                              ? elemTypeRef->declaration->name
                                              : elemTypeRef->headName();
                    ctx_.diagnostics().report(
                        Severity::Error, call.span.begin,
                        std::string(m) +
                        "() without a selector requires the element type to "
                        "satisfy `T: IComparable<T>` (stdlib §5.1 / spec "
                        "§14.2 / #2770); `" +
                        (en.empty() ? std::string("<element>") : std::string(en)) +
                        "` does not — its `CompareTo` could not be resolved "
                        "for dispatch. Use " + std::string(m) +
                        "(selector: (T) -> U) instead.");
                    return llvm::ConstantPointerNull::get(ptrTy());
                }
            }

            // fix #1901 / stdlib §5: the selector overload `Min<U>/Max<U>(sel):
            // U` — generalize over the selector's actual return type U. The
            // legacy generic path below hardwired an i32 accumulator: it
            // truncated a long/uint/ulong projection, emitted garbage IR for a
            // float/double projection, and mangled a class/string projection.
            // (decimal U and the no-selector class/string forms are handled
            // above; a *no-selector* numeric Min()/Max() keeps the legacy i32
            // path here — its result is typed int by sema, a separate concern.)
            // U is read from the selector lambda's inferred return TypeRef.
            sema::TypeRef* uRef = nullptr;
            if (!call.args.empty() && call.args[0].value)
                if (auto* lamTy = ctx_.exprType(*call.args[0].value))
                    if (lamTy->kind == sema::TypeRef::Kind::Function &&
                        lamTy->funcReturn)
                        uRef = lamTy->funcReturn;

            // --- Selector projecting to a POINTER U: a class implementing
            // IComparable<U> (§14.2) or string (§9.6). Mirror the no-selector
            // pointer path (above), but project each element through the
            // selector first; compare the projected values via CompareTo /
            // ordinal strcmp and return the winning projection. The result is a
            // borrow (§28.3, tracked in the ownership pass); codegen returns the
            // pointer and takes no disposal obligation. ---
            if (uRef) {
                bool uIsStr = isStringTypeRefNonNull(uRef);
                sema::MethodSymbol* uCmpMs = nullptr;
                if (!uIsStr && uRef->kind == sema::TypeRef::Kind::Named &&
                    uRef->declaration &&
                    (uRef->declaration->kind == ast::TypeDeclKind::Class ||
                     uRef->declaration->kind == ast::TypeDeclKind::Interface)) {
                    if (sema::MethodSymbol* cm =
                            findMethodByName(uRef->declaration, "CompareTo"))
                        if (cm->params.size() == 1) uCmpMs = cm;
                }
                // fix #2770: the SELECTOR sibling of the defect fixed above —
                // the identical findMethodByName probe, so an INTERFACE
                // projection U was equally invisible. stdlib §5 declares
                //     public method Min<U>(selector: (T) -> U): U
                //                                     where U: IComparable<U>
                // so an interface U is admissible on exactly the §14.2 rule-3
                // grounds. MEASURED: this site does not reach the `icmp`
                // assertion — with U an interface the call degrades to the
                // #2491 "return-value expression failed to lower" internal
                // diagnostic instead — but the ROOT CAUSE is the same probe,
                // and a class U at the same site compiles and runs correctly.
                llvm::Function* uIfaceCmpFn = nullptr;
                if (!uIsStr && !uCmpMs) {
                    if (sema::MethodSymbol* ifm =
                            selfComparableIfaceCompareTo(uRef))
                        uIfaceCmpFn =
                            ensureIfaceCmpThunk(uRef->declaration, ifm);
                }
                if (uIsStr || uCmpMs || uIfaceCmpFn) {
                    llvm::Value* closureP =
                        lowerLinqLambda(call.args[0].value.get(), uRef);
                    if (!closureP) return nullptr;
                    auto* lenFn = declareC("bz_array_len", i64(), {ptrTy()});
                    llvm::Value* len = builder_.CreateCall(lenFn, {recv});
                    llvm::Value* isEmpty =
                        builder_.CreateICmpEQ(len, builder_.getInt64(0));
                    emitGuardThrow(isEmpty, "InvalidOperationException");
                    auto* ptrFn = declareC("bz_array_ptr_at", ptrTy(),
                                           {ptrTy(), i64(), i64()});
                    auto* idxSlot = entryAlloca(i64());
                    auto* accSlot = entryAlloca(ptrTy());
                    // fix #2770: scratch slot for the interface arm below (the
                    // thunk takes slot addresses, not values). Unused by the
                    // string / class arms.
                    auto* pvSlot = entryAlloca(ptrTy());
                    // Seed the accumulator with selector(element[0]) and scan
                    // from index 1 (avoids comparing against a null accumulator).
                    llvm::Value* p0 = builder_.CreateCall(
                        ptrFn, {recv, builder_.getInt64(0), builder_.getInt64(esz)});
                    llvm::Value* e0 = builder_.CreateLoad(elemTy, p0);
                    builder_.CreateStore(callClosure(closureP, e0, ptrTy()),
                                         accSlot);
                    builder_.CreateStore(builder_.getInt64(1), idxSlot);
                    auto* condBB = llvm::BasicBlock::Create(llvm_, "mm.c", currentFn_);
                    auto* bodyBB = llvm::BasicBlock::Create(llvm_, "mm.b", currentFn_);
                    auto* updBB  = llvm::BasicBlock::Create(llvm_, "mm.u", currentFn_);
                    auto* nextBB = llvm::BasicBlock::Create(llvm_, "mm.n", currentFn_);
                    auto* endBB  = llvm::BasicBlock::Create(llvm_, "mm.e", currentFn_);
                    builder_.CreateBr(condBB);
                    builder_.SetInsertPoint(condBB);
                    llvm::Value* idx = builder_.CreateLoad(i64(), idxSlot);
                    builder_.CreateCondBr(builder_.CreateICmpSLT(idx, len),
                                          bodyBB, endBB);
                    builder_.SetInsertPoint(bodyBB);
                    llvm::Value* ep = builder_.CreateCall(
                        ptrFn, {recv, idx, builder_.getInt64(esz)});
                    llvm::Value* el = builder_.CreateLoad(elemTy, ep);
                    llvm::Value* pv = callClosure(closureP, el, ptrTy());
                    llvm::Value* acc = builder_.CreateLoad(ptrTy(), accSlot, "mm.acc");
                    llvm::Value* cmp = nullptr;
                    if (uIsStr) {
                        llvm::Value* pvData = builder_.CreateLoad(ptrTy(),
                            builder_.CreateStructGEP(stringType(), pv, 0));
                        llvm::Value* accData = builder_.CreateLoad(ptrTy(),
                            builder_.CreateStructGEP(stringType(), acc, 0));
                        auto* strcmpFn = module_->getFunction("strcmp");
                        if (!strcmpFn) {
                            auto* ft = llvm::FunctionType::get(
                                i32(), {ptrTy(), ptrTy()}, false);
                            strcmpFn = llvm::Function::Create(
                                ft, llvm::Function::ExternalLinkage, "strcmp",
                                module_.get());
                        }
                        cmp = builder_.CreateCall(strcmpFn, {pvData, accData},
                                                  "mm.cmp");
                    } else if (uIfaceCmpFn) {
                        // fix #2770: the thunk's ABI is slot ADDRESSES, and the
                        // projection `pv` is a value, so spill it to an entry
                        // alloca. `accSlot` already is such a slot.
                        builder_.CreateStore(pv, pvSlot);
                        cmp = builder_.CreateCall(uIfaceCmpFn, {pvSlot, accSlot},
                                                  "mm.cmp");
                    } else {
                        std::vector<llvm::Value*> cargs{pv, acc};
                        cmp = emitMethodCall(uCmpMs, cargs, /*baseQualified=*/false);
                        if (!cmp) return nullptr;
                    }
                    llvm::Value* zero = llvm::ConstantInt::get(cmp->getType(), 0);
                    llvm::Value* shouldUpdate = isMax
                        ? builder_.CreateICmpSGT(cmp, zero)
                        : builder_.CreateICmpSLT(cmp, zero);
                    builder_.CreateCondBr(shouldUpdate, updBB, nextBB);
                    builder_.SetInsertPoint(updBB);
                    builder_.CreateStore(pv, accSlot);
                    builder_.CreateBr(nextBB);
                    builder_.SetInsertPoint(nextBB);
                    builder_.CreateStore(
                        builder_.CreateAdd(idx, builder_.getInt64(1)), idxSlot);
                    builder_.CreateBr(condBB);
                    builder_.SetInsertPoint(endBB);
                    if (relCl) emitClosureRelease(closureP);  // #422 inline-lambda
                    freeLazyTemp();
                    return builder_.CreateLoad(ptrTy(), accSlot, "mm.r");
                }
            }

            // --- Numeric U (selector) or the no-selector numeric element.
            // Derive the accumulator LLVM type, float-ness, and signedness from
            // U (selector) or from the element type (no-selector) so a
            // long/uint/ulong value is not truncated and a float/double value
            // uses FP compare (mirrors the #1757 Sum-selector fix and the #1836
            // no-selector Sum fix). Per stdlib §5.1 (v0.226) the no-selector
            // Min()/Max() returns the element's own type T; a non-numeric
            // element keeps the legacy int fallback. ---
            sema::PrimitiveType retP = sema::PrimitiveType::Int;
            const bool selNumeric =
                uRef && uRef->kind == sema::TypeRef::Kind::Primitive &&
                !uRef->nullable;
            if (selNumeric) {
                retP = uRef->primitive;
            } else if (call.args.empty() && recvTy &&
                       !recvTy->genericArgs.empty() &&
                       recvTy->genericArgs[0]) {
                // fix #1836 / stdlib §5.1 (v0.226): no-selector Min()/Max()
                // returns the element's own numeric type T (long/uint/ulong/
                // float/double), not int. (Decimal took the §5.1 decimal path
                // above; class/string took the pointer-comparable path.)
                sema::TypeRef* e = recvTy->genericArgs[0];
                if (e->kind == sema::TypeRef::Kind::Primitive && !e->nullable) {
                    switch (e->primitive) {
                        case sema::PrimitiveType::UInt:
                        case sema::PrimitiveType::Long:
                        case sema::PrimitiveType::ULong:
                        case sema::PrimitiveType::Float:
                        case sema::PrimitiveType::Double:
                            retP = e->primitive;
                            break;
                        default:
                            break;  // Int stays
                    }
                }
            }
            sema::TypeRef* retTyRef = ctx_.primitiveTypeRef(retP);
            const bool accIsFP = (retP == sema::PrimitiveType::Float ||
                                  retP == sema::PrimitiveType::Double);
            const bool accIsUnsigned = isUnsignedPrim(retP);
            llvm::Type* accLLTy = lowerTypeRef(retTyRef);
            if (!accLLTy) accLLTy = i32();
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
            auto* accSlot  = entryAlloca(accLLTy);
            auto* initSlot = entryAlloca(i1());
            builder_.CreateStore(builder_.getInt64(0), idxSlot);
            builder_.CreateStore(
                accIsFP ? static_cast<llvm::Value*>(
                              llvm::ConstantFP::get(accLLTy, 0.0))
                        : static_cast<llvm::Value*>(
                              llvm::ConstantInt::get(accLLTy, 0)),
                accSlot);
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
            llvm::Value* val = closure ? callClosure(closure, el, accLLTy) : el;
            // fix #1836: with the no-selector accumulator now derived from the
            // element type (#1836) and the selector accumulator from U (#1901),
            // val already matches accLLTy on every path; this integer-only
            // narrowing is a defensive no-op kept for the int-default fallback.
            if (val->getType() != accLLTy && val->getType()->isIntegerTy() &&
                accLLTy->isIntegerTy())
                val = builder_.CreateTrunc(val, accLLTy);
            llvm::Value* inited = builder_.CreateLoad(i1(), initSlot);
            llvm::Value* acc = builder_.CreateLoad(accLLTy, accSlot);
            llvm::Value* cmp = accIsFP
                ? (isMax ? builder_.CreateFCmpOGT(val, acc)
                         : builder_.CreateFCmpOLT(val, acc))
                : (isMax
                       ? (accIsUnsigned ? builder_.CreateICmpUGT(val, acc)
                                        : builder_.CreateICmpSGT(val, acc))
                       : (accIsUnsigned ? builder_.CreateICmpULT(val, acc)
                                        : builder_.CreateICmpSLT(val, acc)));
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
            return builder_.CreateLoad(accLLTy, accSlot, "mm.r");
        }
        // fix #2783 (stdlib §5.1, v0.240): .MinOrNull() / .MaxOrNull() —
        // sibling of Min()/Max() that returns T? (null on empty source)
        // instead of throwing InvalidOperationException. stdlib §5.1
        // declares both in `extension IEnumerable<T> where T:
        // IComparable<T>`; Array<T>'s Min/Max (handled above) already
        // compiles, so this handler mirrors its no-selector numeric arm
        // for the most common element types and returns T? as a tagged
        // {hasValue, value} struct for value types or a nullable pointer
        // for class/string/interface elements (mirroring FirstOrNull at
        // ~line 3987). Selector overloads (`MinOrNull<U>(sel: (T)->U)`)
        // and decimal elements are out of scope here — file a follow-up
        // if needed. No-arg form only.
        if ((m == "MinOrNull" || m == "MaxOrNull") && call.args.empty() &&
            isArr) {
            bool isMaxOrNull = (m == "MaxOrNull");
            sema::TypeRef* elemTypeRef =
                (recvTy && !recvTy->genericArgs.empty())
                    ? recvTy->genericArgs[0] : nullptr;
            llvm::Type* elemLLTy = getElemTy();
            auto* lenFn = declareC("bz_array_len", i64(), {ptrTy()});
            llvm::Value* len = builder_.CreateCall(lenFn, {recv});
            // Empty path: return null T? (skip the scan entirely).
            auto* emptyBB  = llvm::BasicBlock::Create(llvm_, "mon.e", currentFn_);
            auto* seedBB   = llvm::BasicBlock::Create(llvm_, "mon.s", currentFn_);
            auto* condBB   = llvm::BasicBlock::Create(llvm_, "mon.c", currentFn_);
            auto* bodyBB   = llvm::BasicBlock::Create(llvm_, "mon.b", currentFn_);
            auto* updBB    = llvm::BasicBlock::Create(llvm_, "mon.u", currentFn_);
            auto* nextBB   = llvm::BasicBlock::Create(llvm_, "mon.n", currentFn_);
            auto* endBB    = llvm::BasicBlock::Create(llvm_, "mon.x", currentFn_);
            llvm::Value* isEmpty =
                builder_.CreateICmpEQ(len, builder_.getInt64(0));
            builder_.CreateCondBr(isEmpty, emptyBB, seedBB);

            // Seed block: load element[0] as the initial accumulator and
            // start scanning from index 1. (Seeding avoids comparing
            // against a null/zero accumulator on the first iteration.)
            builder_.SetInsertPoint(seedBB);
            auto* ptrFn = declareC("bz_array_ptr_at", ptrTy(),
                                   {ptrTy(), i64(), i64()});
            llvm::Value* p0 = builder_.CreateCall(
                ptrFn, {recv, builder_.getInt64(0), builder_.getInt64(esz)});
            llvm::AllocaInst* accSlot = entryAlloca(elemLLTy);
            llvm::AllocaInst* idxSlot = entryAlloca(i64());
            builder_.CreateStore(builder_.CreateLoad(elemLLTy, p0), accSlot);
            builder_.CreateStore(builder_.getInt64(1), idxSlot);
            builder_.CreateBr(condBB);

            // Compare-loop body.
            builder_.SetInsertPoint(condBB);
            llvm::Value* idx = builder_.CreateLoad(i64(), idxSlot);
            // Load the accumulator before branching — needed by the
            // endBB PHI when the loop exits. LLVM DCEs this on the
            // true branch (bodyBB).
            llvm::Value* exitVal =
                builder_.CreateLoad(elemLLTy, accSlot, "mon.exitv");
            builder_.CreateCondBr(builder_.CreateICmpSLT(idx, len),
                                  bodyBB, endBB);
            builder_.SetInsertPoint(bodyBB);
            llvm::Value* ep = builder_.CreateCall(
                ptrFn, {recv, idx, builder_.getInt64(esz)});
            llvm::Value* el  = builder_.CreateLoad(elemLLTy, ep, "mon.el");
            llvm::Value* acc = builder_.CreateLoad(elemLLTy, accSlot, "mon.acc");
            // For class/string/interface (pointer) elements, use
            // ordinal strcmp (string), itable CompareTo (interface,
            // #2770), or direct CompareTo call (class). For numeric
            // primitive elements, use direct icmp/fcmp.
            llvm::Value* shouldUpdate;
            bool isStrElem = isStringTypeRefNonNull(elemTypeRef);
            sema::MethodSymbol* cmpMs = nullptr;
            llvm::Function* ifaceCmpFn = nullptr;
            if (!isStrElem && elemTypeRef &&
                elemTypeRef->kind == sema::TypeRef::Kind::Named &&
                (elemTypeRef->declaration == nullptr ||
                 elemTypeRef->declaration->kind ==
                     ast::TypeDeclKind::Class ||
                 elemTypeRef->declaration->kind ==
                     ast::TypeDeclKind::Interface)) {
                if (elemTypeRef->declaration) {
                    if (sema::MethodSymbol* cm =
                            findMethodByName(elemTypeRef->declaration,
                                             "CompareTo"))
                        if (cm->params.size() == 1) cmpMs = cm;
                }
                if (!cmpMs) {
                    if (sema::MethodSymbol* ifm =
                            selfComparableIfaceCompareTo(elemTypeRef))
                        ifaceCmpFn =
                            ensureIfaceCmpThunk(
                                elemTypeRef->declaration, ifm);
                }
            }
            if (isStrElem || cmpMs || ifaceCmpFn) {
                // Pointer-typed element: string-ordinal strcmp (string),
                // itable CompareTo (interface, #2770), or direct
                // CompareTo call (class).
                llvm::Value* cmp = nullptr;
                if (isStrElem) {
                    llvm::Value* elData = builder_.CreateLoad(ptrTy(),
                        builder_.CreateStructGEP(stringType(), el, 0));
                    llvm::Value* accData = builder_.CreateLoad(ptrTy(),
                        builder_.CreateStructGEP(stringType(), acc, 0));
                    auto* strcmpFn = module_->getFunction("strcmp");
                    if (!strcmpFn) {
                        auto* ft = llvm::FunctionType::get(
                            i32(), {ptrTy(), ptrTy()}, false);
                        strcmpFn = llvm::Function::Create(
                            ft, llvm::Function::ExternalLinkage, "strcmp",
                            module_.get());
                    }
                    cmp = builder_.CreateCall(strcmpFn,
                                              {elData, accData},
                                              "mon.cmp");
                } else if (ifaceCmpFn) {
                    cmp = builder_.CreateCall(ifaceCmpFn, {ep, accSlot},
                                              "mon.cmp");
                } else {
                    std::vector<llvm::Value*> cargs{el, acc};
                    cmp = emitMethodCall(cmpMs, cargs,
                                         /*baseQualified=*/false);
                    if (!cmp) return nullptr;
                }
                llvm::Value* zero = llvm::ConstantInt::get(
                    cmp->getType(), 0);
                shouldUpdate = isMaxOrNull
                    ? builder_.CreateICmpSGT(cmp, zero)
                    : builder_.CreateICmpSLT(cmp, zero);
            } else if (elemLLTy->isIntegerTy()) {
                shouldUpdate = isMaxOrNull
                    ? builder_.CreateICmpSGT(el, acc)
                    : builder_.CreateICmpSLT(el, acc);
            } else if (elemLLTy->isFloatTy() ||
                       elemLLTy->isDoubleTy()) {
                shouldUpdate = isMaxOrNull
                    ? builder_.CreateFCmpOGT(el, acc)
                    : builder_.CreateFCmpOLT(el, acc);
            } else {
                // Unknown element type (record/decimal/etc.) — reject
                // loudly rather than silently miscompare.
                ctx_.diagnostics().report(
                    Severity::Error, call.span.begin,
                    std::string("`") + std::string(m) +
                    "()` on `Array<" +
                    (elemTypeRef ? std::string(elemTypeRef->headName())
                    : std::string("T")) +
                    ">` is not yet implemented for this element type "
                    "(stdlib §5.1, #2783): supported element types for "
                    "the no-selector overload are numeric primitives "
                    "(int/long/uint/ulong/float/double), string, and "
                    "class/interface types implementing `IComparable<T>`.");
                return llvm::Constant::getNullValue(elemLLTy);
            }
            builder_.CreateCondBr(shouldUpdate, updBB, nextBB);
            builder_.SetInsertPoint(updBB);
            builder_.CreateStore(el, accSlot);
            builder_.CreateBr(nextBB);
            builder_.SetInsertPoint(nextBB);
            builder_.CreateStore(
                builder_.CreateAdd(idx, builder_.getInt64(1)), idxSlot);
            builder_.CreateBr(condBB);

            // Empty path: T? = null.
            builder_.SetInsertPoint(emptyBB);
            // Determine whether this is a value-type element (tagged
            // {hasValue, value} nullable struct) or a pointer-typed
            // element (nullable pointer — null = empty).
            sema::TypeRef nullRT;
            if (elemTypeRef) {
                nullRT = *elemTypeRef;
                nullRT.nullable = true;
            }
            bool isValTy = elemTypeRef && isNullableValueTy(&nullRT);
            llvm::Value* nullResult;
            if (isValTy) {
                auto* nst = nullableValueStructTy(&nullRT);
                auto* hv = llvm::ConstantInt::get(
                    nullablePrimHasValTy(), 0);
                auto* vv = llvm::Constant::getNullValue(elemLLTy);
                llvm::Value* res = builder_.CreateInsertValue(
                    llvm::UndefValue::get(nst), hv, {0u});
                nullResult = builder_.CreateInsertValue(
                    res, vv, {1u});
            } else {
                nullResult = llvm::Constant::getNullValue(elemLLTy);
            }
            builder_.CreateBr(endBB);

            // End block: PHI for the accumulator value + hasValue flag.
            builder_.SetInsertPoint(endBB);
            llvm::Value* resultV;
            if (isValTy) {
                auto* nst = nullableValueStructTy(&nullRT);
                auto* valPhi = builder_.CreatePHI(elemLLTy, 2, "mon.val");
                valPhi->addIncoming(exitVal, condBB);
                valPhi->addIncoming(
                    llvm::Constant::getNullValue(elemLLTy), emptyBB);
                auto* hvPhi = builder_.CreatePHI(
                    nullablePrimHasValTy(), 2, "mon.hv");
                hvPhi->addIncoming(
                    llvm::ConstantInt::get(nullablePrimHasValTy(), 1),
                    condBB);
                hvPhi->addIncoming(
                    llvm::ConstantInt::get(nullablePrimHasValTy(), 0),
                    emptyBB);
                freeLazyTemp();
                llvm::Value* res = builder_.CreateInsertValue(
                    llvm::UndefValue::get(nst), hvPhi, {0u});
                resultV = builder_.CreateInsertValue(
                    res, valPhi, {1u});
            } else {
                auto* ptrPhi = builder_.CreatePHI(elemLLTy, 2, "mon.ptr");
                ptrPhi->addIncoming(exitVal, condBB);
                ptrPhi->addIncoming(nullResult, emptyBB);
                freeLazyTemp();
                resultV = ptrPhi;
            }
            return resultV;
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
        // per §5 LINQ (#789): .Last(pred: (T)->bool): T — the LAST element
        // satisfying the predicate; throws InvalidOperationException if none does.
        // Identical scan to First(pred) except the match branch does NOT break: it
        // keeps scanning so the final survivor in resultSlot is the last match.
        if (m == "Last" && call.args.size() == 1 && call.args[0].value) {
            const bool relCl = argIsInlineLambda(0);
            llvm::Value* closure = lowerLinqLambda(call.args[0].value.get(),
                ctx_.primitiveTypeRef(sema::PrimitiveType::Bool));
            if (!closure) return nullptr;
            llvm::Type* elemTy = getElemTy();
            auto* lenFn = declareC("bz_array_len", i64(), {ptrTy()});
            llvm::Value* len = builder_.CreateCall(lenFn, {recv});
            auto* resultSlot = entryAlloca(elemTy);
            auto* foundSlot = entryAlloca(i1());
            builder_.CreateStore(builder_.getFalse(), foundSlot);
            auto* idxSlot = entryAlloca(i64());
            builder_.CreateStore(builder_.getInt64(0), idxSlot);
            auto* condBB  = llvm::BasicBlock::Create(llvm_, "lstp.c", currentFn_);
            auto* bodyBB  = llvm::BasicBlock::Create(llvm_, "lstp.b", currentFn_);
            auto* foundBB = llvm::BasicBlock::Create(llvm_, "lstp.f", currentFn_);
            auto* nextBB  = llvm::BasicBlock::Create(llvm_, "lstp.n", currentFn_);
            auto* endBB   = llvm::BasicBlock::Create(llvm_, "lstp.e", currentFn_);
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
            builder_.CreateBr(nextBB);  // keep scanning — last match wins
            builder_.SetInsertPoint(nextBB);
            builder_.CreateStore(builder_.CreateAdd(idx, builder_.getInt64(1)), idxSlot);
            builder_.CreateBr(condBB);
            builder_.SetInsertPoint(endBB);
            if (relCl) emitClosureRelease(closure);
            llvm::Value* notFound = builder_.CreateNot(builder_.CreateLoad(i1(), foundSlot));
            emitGuardThrow(notFound, "InvalidOperationException");
            llvm::Value* lstv = builder_.CreateLoad(elemTy, resultSlot, "lstp.r");
            freeLazyTemp();
            return lstv;
        }
        // per §5 LINQ (#789): .Single(pred): T — the sole element satisfying the
        // predicate; throws InvalidOperationException if zero or more than one
        // match. Scan-and-count like SingleOrNull(pred) but throws (not null) and
        // returns plain T.
        if (m == "Single" && call.args.size() == 1 && call.args[0].value) {
            const bool relCl = argIsInlineLambda(0);
            llvm::Value* closure = lowerLinqLambda(call.args[0].value.get(),
                ctx_.primitiveTypeRef(sema::PrimitiveType::Bool));
            if (!closure) return nullptr;
            llvm::Type* elemTy = getElemTy();
            auto* lenFn = declareC("bz_array_len", i64(), {ptrTy()});
            llvm::Value* len = builder_.CreateCall(lenFn, {recv});
            auto* resultSlot = entryAlloca(elemTy);
            auto* matchCountSlot = entryAlloca(i32());
            builder_.CreateStore(builder_.getInt32(0), matchCountSlot);
            auto* idxSlot = entryAlloca(i64());
            builder_.CreateStore(builder_.getInt64(0), idxSlot);
            auto* condBB  = llvm::BasicBlock::Create(llvm_, "sgl.c", currentFn_);
            auto* bodyBB  = llvm::BasicBlock::Create(llvm_, "sgl.b", currentFn_);
            auto* matchBB = llvm::BasicBlock::Create(llvm_, "sgl.m", currentFn_);
            auto* nextBB  = llvm::BasicBlock::Create(llvm_, "sgl.n", currentFn_);
            auto* endBB   = llvm::BasicBlock::Create(llvm_, "sgl.e", currentFn_);
            builder_.CreateBr(condBB);
            builder_.SetInsertPoint(condBB);
            llvm::Value* idx = builder_.CreateLoad(i64(), idxSlot);
            builder_.CreateCondBr(builder_.CreateICmpSLT(idx, len), bodyBB, endBB);
            builder_.SetInsertPoint(bodyBB);
            auto* ptrFn = declareC("bz_array_ptr_at", ptrTy(), {ptrTy(), i64(), i64()});
            llvm::Value* ep = builder_.CreateCall(ptrFn, {recv, idx, builder_.getInt64(esz)});
            llvm::Value* el = builder_.CreateLoad(elemTy, ep);
            llvm::Value* ok = callClosure(closure, el, i1());
            builder_.CreateCondBr(ok, matchBB, nextBB);
            builder_.SetInsertPoint(matchBB);
            llvm::Value* curCnt = builder_.CreateLoad(i32(), matchCountSlot);
            llvm::Value* isFirst = builder_.CreateICmpEQ(curCnt, builder_.getInt32(0));
            auto* storeMatchBB = llvm::BasicBlock::Create(llvm_, "sgl.sm", currentFn_);
            auto* afterStoreBB = llvm::BasicBlock::Create(llvm_, "sgl.as", currentFn_);
            builder_.CreateCondBr(isFirst, storeMatchBB, afterStoreBB);
            builder_.SetInsertPoint(storeMatchBB);
            builder_.CreateStore(el, resultSlot);
            builder_.CreateBr(afterStoreBB);
            builder_.SetInsertPoint(afterStoreBB);
            llvm::Value* newCnt = builder_.CreateAdd(curCnt, builder_.getInt32(1));
            llvm::Value* saturated = builder_.CreateSelect(
                builder_.CreateICmpSGT(newCnt, builder_.getInt32(2)),
                builder_.getInt32(2), newCnt);
            builder_.CreateStore(saturated, matchCountSlot);
            builder_.CreateBr(nextBB);
            builder_.SetInsertPoint(nextBB);
            builder_.CreateStore(builder_.CreateAdd(idx, builder_.getInt64(1)), idxSlot);
            builder_.CreateBr(condBB);
            builder_.SetInsertPoint(endBB);
            if (relCl) emitClosureRelease(closure);
            llvm::Value* finalCnt = builder_.CreateLoad(i32(), matchCountSlot);
            llvm::Value* notOne = builder_.CreateICmpNE(finalCnt, builder_.getInt32(1));
            emitGuardThrow(notOne, "InvalidOperationException");
            llvm::Value* sglv = builder_.CreateLoad(elemTy, resultSlot, "sgl.r");
            freeLazyTemp();
            return sglv;
        }
        // per §5 LINQ (#789): .LastOrNull(pred): T? — the LAST element satisfying
        // the predicate, or null if none. Scan-keep-last like Last(pred) but
        // returns T? (mirrors FirstOrNull(pred)).
        if (m == "LastOrNull" && call.args.size() == 1 && call.args[0].value) {
            const bool relCl = argIsInlineLambda(0);
            llvm::Value* closure = lowerLinqLambda(call.args[0].value.get(),
                ctx_.primitiveTypeRef(sema::PrimitiveType::Bool));
            if (!closure) return nullptr;
            llvm::Type* elemTy = getElemTy();
            auto* lenFn = declareC("bz_array_len", i64(), {ptrTy()});
            llvm::Value* len = builder_.CreateCall(lenFn, {recv});
            auto* resultSlot = entryAlloca(elemTy);
            auto* foundSlot = entryAlloca(i1());
            builder_.CreateStore(builder_.getFalse(), foundSlot);
            auto* idxSlot = entryAlloca(i64());
            builder_.CreateStore(builder_.getInt64(0), idxSlot);
            auto* condBB  = llvm::BasicBlock::Create(llvm_, "lon.c", currentFn_);
            auto* bodyBB  = llvm::BasicBlock::Create(llvm_, "lon.b", currentFn_);
            auto* foundBB = llvm::BasicBlock::Create(llvm_, "lon.f", currentFn_);
            auto* nextBB  = llvm::BasicBlock::Create(llvm_, "lon.n", currentFn_);
            auto* endBB   = llvm::BasicBlock::Create(llvm_, "lon.e", currentFn_);
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
            builder_.CreateBr(nextBB);  // keep scanning — last match wins
            builder_.SetInsertPoint(nextBB);
            builder_.CreateStore(builder_.CreateAdd(idx, builder_.getInt64(1)), idxSlot);
            builder_.CreateBr(condBB);
            builder_.SetInsertPoint(endBB);
            if (relCl) emitClosureRelease(closure);
            sema::TypeRef* elemRefNN = (recvTy && !recvTy->genericArgs.empty())
                ? recvTy->genericArgs[0] : nullptr;
            sema::TypeRef nullRT;
            if (elemRefNN) { nullRT = *elemRefNN; nullRT.nullable = true; }
            bool isValTy = elemRefNN && isNullableValueTy(&nullRT);
            llvm::Value* found = builder_.CreateLoad(i1(), foundSlot);
            llvm::Value* notFound = builder_.CreateNot(found);
            llvm::Value* loadedV = builder_.CreateLoad(elemTy, resultSlot, "lon.v");
            llvm::Value* resultV = nullptr;
            if (isValTy) {
                auto* nst = nullableValueStructTy(&nullRT);
                llvm::Value* res = builder_.CreateInsertValue(
                    llvm::UndefValue::get(nst),
                    builder_.CreateSelect(notFound,
                        llvm::ConstantInt::get(nullablePrimHasValTy(), 0),
                        llvm::ConstantInt::get(nullablePrimHasValTy(), 1)),
                    {0u});
                resultV = builder_.CreateInsertValue(res,
                    builder_.CreateSelect(notFound,
                        llvm::Constant::getNullValue(elemTy), loadedV),
                    {1u});
            } else {
                resultV = builder_.CreateSelect(notFound,
                    llvm::Constant::getNullValue(elemTy), loadedV, "lon.ptr");
            }
            freeLazyTemp();
            return resultV;
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
            // #1930: over a bare GroupBy this returns a live grouping — keep it,
            // free its siblings + the array spine (else free the borrow snapshot).
            freeLazyTemp(keepGroupByGrouping ? fstv : nullptr);
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
            freeLazyTemp(keepGroupByGrouping ? lstv : nullptr);  // #1930: keep the returned grouping
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
            freeLazyTemp(keepGroupByGrouping ? eav : nullptr);  // #1930: keep the returned grouping
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
                // #1930: over a bare GroupBy an IGrouping is a pointer — this branch
                // returns the grouping (or null on empty). Keep the returned one and
                // free its siblings + the array spine; a runtime-null ptrPhi (empty)
                // makes _except free all (correct, nothing is returned).
                freeLazyTemp(keepGroupByGrouping ? ptrPhi : nullptr);  // Free AFTER the PHI node.
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
        // stdlib §5 (#661): SingleOrNull(predicate: (T)->bool): T? — return the
        // single element satisfying the predicate, or null if zero or two+ match.
        // Per stdlib §5 "*OrNull variants return null instead [of throwing]" and
        // Single() "throws if not exactly one". Semantics: scan ALL elements;
        // if exactly one satisfies pred → return it; if none or more than one → null.
        if (m == "SingleOrNull" && call.args.size() == 1 && call.args[0].value) {
            const bool relCl = argIsInlineLambda(0);
            llvm::Value* closure = lowerLinqLambda(call.args[0].value.get(),
                ctx_.primitiveTypeRef(sema::PrimitiveType::Bool));
            if (!closure) return nullptr;
            llvm::Type* elemTy = getElemTy();
            auto* lenFn = declareC("bz_array_len", i64(), {ptrTy()});
            llvm::Value* len = builder_.CreateCall(lenFn, {recv});
            // resultSlot: stores the (first) matching element.
            // matchCountSlot (i32): 0=none, 1=exactly one, 2+=too many.
            auto* resultSlot    = entryAlloca(elemTy);
            auto* matchCountSlot = entryAlloca(i32());
            builder_.CreateStore(builder_.getInt32(0), matchCountSlot);
            auto* idxSlot = entryAlloca(i64());
            builder_.CreateStore(builder_.getInt64(0), idxSlot);
            auto* condBB  = llvm::BasicBlock::Create(llvm_, "son.c", currentFn_);
            auto* bodyBB  = llvm::BasicBlock::Create(llvm_, "son.b", currentFn_);
            auto* matchBB = llvm::BasicBlock::Create(llvm_, "son.m", currentFn_);
            auto* nextBB  = llvm::BasicBlock::Create(llvm_, "son.n", currentFn_);
            auto* endBB   = llvm::BasicBlock::Create(llvm_, "son.e", currentFn_);
            builder_.CreateBr(condBB);
            // Loop condition: idx < len.
            builder_.SetInsertPoint(condBB);
            llvm::Value* idx = builder_.CreateLoad(i64(), idxSlot);
            builder_.CreateCondBr(builder_.CreateICmpSLT(idx, len), bodyBB, endBB);
            // Loop body: test predicate.
            builder_.SetInsertPoint(bodyBB);
            auto* ptrFn = declareC("bz_array_ptr_at", ptrTy(),
                                   {ptrTy(), i64(), i64()});
            llvm::Value* ep = builder_.CreateCall(ptrFn,
                {recv, idx, builder_.getInt64(esz)});
            llvm::Value* el = builder_.CreateLoad(elemTy, ep);
            llvm::Value* ok = callClosure(closure, el, i1());
            builder_.CreateCondBr(ok, matchBB, nextBB);
            // Match: store first match element, increment counter (capped at 2).
            builder_.SetInsertPoint(matchBB);
            llvm::Value* curCnt = builder_.CreateLoad(i32(), matchCountSlot);
            // Only overwrite resultSlot if this is the first match (cnt == 0).
            llvm::Value* isFirst = builder_.CreateICmpEQ(curCnt, builder_.getInt32(0));
            llvm::Value* slotAddr = resultSlot;
            (void)slotAddr;
            // Conditional store: write element only when count == 0.
            auto* storeMatchBB = llvm::BasicBlock::Create(llvm_, "son.sm", currentFn_);
            auto* afterStoreBB = llvm::BasicBlock::Create(llvm_, "son.as", currentFn_);
            builder_.CreateCondBr(isFirst, storeMatchBB, afterStoreBB);
            builder_.SetInsertPoint(storeMatchBB);
            builder_.CreateStore(el, resultSlot);
            builder_.CreateBr(afterStoreBB);
            builder_.SetInsertPoint(afterStoreBB);
            // Increment count: saturate at 2.
            llvm::Value* newCnt = builder_.CreateAdd(curCnt, builder_.getInt32(1));
            llvm::Value* saturated = builder_.CreateSelect(
                builder_.CreateICmpSGT(newCnt, builder_.getInt32(2)),
                builder_.getInt32(2), newCnt);
            builder_.CreateStore(saturated, matchCountSlot);
            builder_.CreateBr(nextBB);
            // Advance index.
            builder_.SetInsertPoint(nextBB);
            builder_.CreateStore(
                builder_.CreateAdd(idx, builder_.getInt64(1)), idxSlot);
            builder_.CreateBr(condBB);
            builder_.SetInsertPoint(endBB);
            if (relCl) emitClosureRelease(closure);

            // Build T? result: valid iff matchCount == 1.
            sema::TypeRef* elemRefNN = (recvTy && !recvTy->genericArgs.empty())
                ? recvTy->genericArgs[0] : nullptr;
            sema::TypeRef nullRT;
            if (elemRefNN) { nullRT = *elemRefNN; nullRT.nullable = true; }
            bool isValTy = elemRefNN && isNullableValueTy(&nullRT);

            llvm::Value* finalCnt = builder_.CreateLoad(i32(), matchCountSlot);
            llvm::Value* exactlyOne = builder_.CreateICmpEQ(
                finalCnt, builder_.getInt32(1));
            llvm::Value* notExactlyOne = builder_.CreateNot(exactlyOne);
            llvm::Value* loadedV = builder_.CreateLoad(elemTy, resultSlot, "son.v");
            llvm::Value* resultV = nullptr;
            if (isValTy) {
                auto* nst = nullableValueStructTy(&nullRT);
                llvm::Value* res = builder_.CreateInsertValue(
                    llvm::UndefValue::get(nst),
                    builder_.CreateSelect(notExactlyOne,
                        llvm::ConstantInt::get(nullablePrimHasValTy(), 0),
                        llvm::ConstantInt::get(nullablePrimHasValTy(), 1)),
                    {0u});
                resultV = builder_.CreateInsertValue(
                    res,
                    builder_.CreateSelect(notExactlyOne,
                        llvm::Constant::getNullValue(elemTy), loadedV),
                    {1u});
            } else {
                // Pointer / class / string: null when not exactly one match.
                resultV = builder_.CreateSelect(notExactlyOne,
                    llvm::Constant::getNullValue(elemTy), loadedV,
                    "son.ptr");
            }
            freeLazyTemp();
            return resultV;
        }
        // per §5 LINQ: .Skip / .Take are LAZY — see tryLowerLazyLinqCall.

        // per §3: .Sort() — in-place ascending sort for int arrays.
        // Uses bz_array_sort with a built-in i32 comparator thunk.
        // fix #3345: the static-call form `Array.Sort(a)` re-binds the
        // receiver to args[0] above; here, accept either the instance
        // form (call.args.empty()) or the static form (call.args.size()
        // == 1 with the array in args[0]). Both lower the same way:
        // the receiver `recv` is the array.
        if (m == "Sort" && (call.args.empty() ||
                            (isStaticArrayCall && call.args.size() == 1))) {
            llvm::Type* elemTy2 = getElemTy();
            // #1195: detect class IComparable<T> BEFORE the isStringElem check —
            // class elements are ptrTy()-typed just like strings, so without this
            // guard a class-element pointer was passed to strcmp → wrong order.
            // per §14.2 / stdlib §2.8.
            const sema::TypeRef* elemTR = (recvTy && !recvTy->genericArgs.empty())
                ? recvTy->genericArgs[0] : nullptr;
            sema::MethodSymbol* classCompareMs = nullptr;
            if (elemTR && elemTR->kind == sema::TypeRef::Kind::Named &&
                elemTR->declaration &&
                elemTR->declaration->kind == ast::TypeDeclKind::Class) {
                sema::MethodSymbol* cm =
                    findMethodByName(elemTR->declaration, "CompareTo");
                if (cm && cm->params.size() == 1) classCompareMs = cm;
            }
            // fix #2768 (stdlib §4.4/§2.8, spec §14.2, v0.240): an INTERFACE
            // element `IFoo: IComparable<IFoo>` is neither a Class (so
            // classCompareMs stays null) nor a string, yet it lowers to
            // ptrTy() — so isStringElem below used to select bz$strcmp, which
            // strcmp'd each element's VTABLE POINTER as a char*. Every instance
            // of one class shares one vtable, so every pair compared equal and
            // Sort() was a silent no-op with CompareTo never called. Dispatch
            // through the §13 itable instead. See selfComparableIfaceCompareTo
            // / ensureIfaceCmpThunk for why no other element category can be
            // routed here.
            sema::MethodSymbol* ifaceCompareMs =
                classCompareMs ? nullptr : selfComparableIfaceCompareTo(elemTR);
            // Null when the interface descriptor or itable index is missing;
            // the pre-fix comparator choice then stands unchanged.
            llvm::Function* ifaceCmpFn =
                ifaceCompareMs
                    ? ensureIfaceCmpThunk(elemTR->declaration, ifaceCompareMs)
                    : nullptr;
            // Choose comparator: class IComparable → bz$classcmp$<M.Name>,
            // interface IComparable → bz$ifacecmp$<M.Name>,
            // string → bz$strcmp, int/other → bz$intcmp.  Both reference-type
            // names are MODULE-QUALIFIED (`mangledTypeName`), per §2.8.3
            // rule 3 — see buildObjectCompareToThunk for what the unqualified
            // class name cost (#3512).
            bool isStringElem =
                !classCompareMs && !ifaceCmpFn && (elemTy2 == ptrTy());
            llvm::Function* cmpFn = nullptr;
            if (ifaceCmpFn) {
                cmpFn = ifaceCmpFn;
            } else if (classCompareMs) {
                // Class IComparable<T> comparator thunk: dispatches a.CompareTo(b)
                // via vtable (virtual/override) or direct compiled call otherwise.
                // Each thunk arg is ptr* — a pointer to the class-pointer slot in
                // the array buffer.  per §14.2 / #1195.
                //
                // fix #3488: the body used to be built INLINE here — which is
                // why #3428 and #3488 both concluded there was no
                // `CompareTo`-through-the-vtable comparator at all and deferred
                // the sorted-collection work on that ground. It is now
                // buildObjectCompareToThunk, so `Array<T>.Sort()`,
                // `Array<T>.Sorted()` and `SortedDictionary<K, V>`'s ordered
                // operations share ONE builder and one `bz$classcmp$<M.C>` per
                // module instead of three spellings of the same dispatch.
                // ⚠ #3488's own commit body called this "the only such builder";
                // that was wrong — `Sorted()` below carried a third, verbatim
                // copy until `narrow #3488` deleted it.
                cmpFn = buildObjectCompareToThunk(elemTR);
                // Unreachable: `classCompareMs` came from the same
                // findMethodByName probe the helper's class arm runs, so it
                // builds. Guarded because a null here would be handed to
                // bz_array_sort as the comparator and called through.
                if (!cmpFn) return nullptr;
            } else if (isStringElem) {
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
        // fix #3345: accept the static-call form `Array.Reverse(a)` —
        // the static arm re-binds the receiver to args[0] above, so
        // this arm sees recv as the array regardless of call shape.
        if (m == "Reverse" && (call.args.empty() ||
                               (isStaticArrayCall && call.args.size() == 1))) {
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
            recvTy->headName() == "Array") {
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
            freeFreshInputTemp();  // #1004: reclaim a fresh ownerless input temp
            return copy;
        }

        // per §3 / #472: Array<T>.Sorted() — returns a new sorted Array<T>
        // (no-comparer form; T must be IComparable<T>: int, string, or a class
        // implementing IComparable<T> — #1195).
        // Implementation: shallow-copy, then in-place Sort() on the copy.
        if (m == "Sorted" && call.args.empty() &&
            recvTy && recvTy->astNode && recvTy->astNode->name &&
            !recvTy->astNode->name->parts.empty() &&
            recvTy->headName() == "Array") {
            llvm::Type* elemTy2 = getElemTy();
            auto* copyFn = declareC("bz_array_copy", ptrTy(), {ptrTy(), i64()});
            llvm::Value* copy = builder_.CreateCall(copyFn, {recv, builder_.getInt64(esz)},
                                                    "srtd.copy");
            // #429: a copied Array<string> co-owns a +1 per element (see Reversed).
            emitArrayRetainStringsIfStringArray(recvTy, copy);
            // #1195: detect class IComparable<T> first — class elements are
            // ptrTy()-typed just like strings; without this guard they fall into
            // the strcmp path and are sorted by pointer value.  per §14.2.
            const sema::TypeRef* elemTR2 = (recvTy && !recvTy->genericArgs.empty())
                ? recvTy->genericArgs[0] : nullptr;
            sema::MethodSymbol* classCompareMs2 = nullptr;
            if (elemTR2 && elemTR2->kind == sema::TypeRef::Kind::Named &&
                elemTR2->declaration &&
                elemTR2->declaration->kind == ast::TypeDeclKind::Class) {
                sema::MethodSymbol* cm =
                    findMethodByName(elemTR2->declaration, "CompareTo");
                if (cm && cm->params.size() == 1) classCompareMs2 = cm;
            }
            // fix #2768: interface element — see the Sort() site above. The
            // sema gate on the comparerless Sorted() (ExprTypecheck_Calls.cpp)
            // admits exactly the same self-referential shape, so this arm and
            // that gate accept or reject in lockstep.
            sema::MethodSymbol* ifaceCompareMs2 =
                classCompareMs2 ? nullptr
                                : selfComparableIfaceCompareTo(elemTR2);
            llvm::Function* ifaceCmpFn2 =
                ifaceCompareMs2
                    ? ensureIfaceCmpThunk(elemTR2->declaration, ifaceCompareMs2)
                    : nullptr;
            bool isStringElem2 =
                !classCompareMs2 && !ifaceCmpFn2 && (elemTy2 == ptrTy());
            llvm::Function* cmpFn2 = nullptr;
            if (ifaceCmpFn2) {
                cmpFn2 = ifaceCmpFn2;
            } else if (classCompareMs2) {
                // Class IComparable<T> comparator thunk. per §14.2 / #1195.
                //
                // narrow #3488 (v0.283): this was a VERBATIM INLINE COPY of the
                // Sort() builder — the THIRD spelling of one decision, after
                // #3488 had already lifted the other two into
                // buildObjectCompareToThunk while its own commit body claimed
                // Sort()'s was "the only such builder in the compiler". The two
                // spellings already DIVERGED in their miss arm (the helper
                // returns null; this copy emitted a constant-0 comparator, i.e.
                // a silent no-op sort), and they were kept in step on ordering
                // only by sharing a cache NAME — so #3512's requalification of
                // that name would have left this copy colliding cross-module
                // AND emitting a second, differently-named thunk per type.
                // Deleted rather than aligned, per the standing rule that a
                // second spelling of an existing decision is this codebase's
                // dominant defect generator.
                cmpFn2 = buildObjectCompareToThunk(elemTR2);
                // Unreachable, exactly as at the Sort() site: `classCompareMs2`
                // came from the same `findMethodByName` probe the helper's class
                // arm runs, and that helper demands `methodFns_.count(ms)` — so
                // the deleted copy's constant-0 fallback was dead code, not a
                // behaviour this arm relied on. Guarded because a null here
                // would reach bz_array_sort as the comparator and be called.
                if (!cmpFn2) return nullptr;
            } else if (isStringElem2) {
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
            freeFreshInputTemp();  // #1004: reclaim a fresh ownerless input temp
            return copy;
        }

        // per §3 / #472: Array<T>.Sorted(comparer: (T,T)->int) — returns a new
        // sorted Array<T> using the user-supplied comparer.
        // Implementation: shallow-copy, then in-place Sort(comparer) on the copy.
        if (m == "Sorted" && call.args.size() == 1 && call.args[0].value &&
            recvTy && recvTy->astNode && recvTy->astNode->name &&
            !recvTy->astNode->name->parts.empty() &&
            recvTy->headName() == "Array") {
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
            freeFreshInputTemp();  // #1004: reclaim a fresh ownerless input temp
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
        // owning family flag: the collection name starts with "Owning" (§28.11 table).
        // NOTE: the Set/OwningSet branch above already handled dict-backed sets and
        // returned; everything from here down is array-backed.
        const sema::TypeRef* arrElemRef =
            (recvTy && !recvTy->genericArgs.empty()) ? recvTy->genericArgs[0]
                                                     : nullptr;
        bool arrIsOwning = recvTy && recvTy->astNode && recvTy->astNode->name &&
                           !recvTy->astNode->name->parts.empty() &&
                           (common::isOwningPrefixedName(recvTy->headName()));
        const ElementPolicy arrElemPol =
            elementPolicyFor(arrElemRef, arrIsOwning);
        // Convenience predicates derived from the policy (replacing the old
        // delegate calls; behavior preserved exactly):
        //
        //   arrIsStringElem: RuntimeManaged + non-nullable string primitive
        //     (same guard as emitArrayRetainStringsIfStringArray; the collection-
        //     name guard is already enforced above: the Set/OwningSet branch
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
        // stdlib §4.10 / §8.3 (#837): OwningSet<T>.Add(owned item) for class-typed
        // T DEDUPLICATES — on a duplicate (per T's equality contract, honouring a
        // user `Equals` override) it silently disposes the passed item rather than
        // appending. The generic array Add below just appends, which on a set is a
        // wrong-Count / duplicate-membership bug. Mirrors the TryAdd scan but
        // returns unit. Gated to OwningSet (OwningArray.Add must still append).
        // fix #1474 / stdlib §4.10: shared dedup-insert-or-dispose helper for an
        // array-backed OwningSet<class T> — scans self for an element EQUAL to
        // `item` per the §8.3 equality contract (a user Equals override is
        // honoured, else reference identity); a duplicate is disposed (the
        // caller relinquished ownership at the call site), otherwise `item` is
        // appended. Factored out of the .Add handler below so .UnionWith can
        // reuse the exact same per-element semantics when draining `other`.
        // Returns false only on an internal emitRemoveElementEquality failure
        // (malformed comparison IR) — callers propagate that as `return nullptr`.
        // fix #1070: the body moved to the shared member helper
        // emitArrayBackedSetInsert so the §15.4 collection-literal materializer
        // builds a class-element set with the IDENTICAL dedup/dispose semantics.
        auto emitSetDedupInsert = [&](llvm::Value* item) -> bool {
            return emitArrayBackedSetInsert(recv, item, arrElemRef,
                                            arrElemPol.disposerThunk, esz);
        };
        // isOwnedSetRecv: this call's receiver is exactly an OwningSet<T> (class
        // T). Gates the set-specific .Add/.UnionWith dedup handlers below —
        // OwningArray/OwningStack/OwningQueue must keep their plain-append Add.
        bool isOwnedSetRecv = recvTy && recvTy->astNode &&
            recvTy->astNode->name && !recvTy->astNode->name->parts.empty() &&
            recvTy->headName() == "OwningSet";
        // stdlib §4.10 / §8.3 (#837): OwningSet<T>.Add(owned item) for class-typed
        // T DEDUPLICATES — on a duplicate (per T's equality contract, honouring a
        // user `Equals` override) it silently disposes the passed item rather than
        // appending. The generic array Add below just appends, which on a set is a
        // wrong-Count / duplicate-membership bug.
        if (m == "Add" && call.args.size() == 1 && call.args[0].value &&
            isOwnedSetRecv && arrElemIsClass && arrIsOwning) {
            llvm::Value* item = lowerExpr(*call.args[0].value);
            if (!item) return nullptr;
            if (!emitSetDedupInsert(item)) return nullptr;
            // fix #1970 / §28.4 / §28.6: OwningSet<T>.Add takes `owned item`
            // (the dup branch disposes `item`, the non-dup branch transfers
            // ownership into the backing array). Either way ownership has
            // resolved at this point and the caller's slot is no longer
            // live. Null it for a bare owned local/param argument so the
            // caller's null-guarded unwindOnly disposal — which
            // UnwindIneligibleWalker now leaves eligible for this callee shape
            // (calleeIsOwningCollectionTransfer) — is exactly-once: it
            // disposes a value still live at a throw that PRECEDES the Add,
            // and skips the already-resolved value at the Add-or-dup throw-
            // after edge (no double-free with this lowering's own disposer
            // call on the dup branch). Mirrors the OwningArray.Add/Insert and
            // user-method nulling paths (#1955 / #1918).
            nullTransferredOwnedLocalSlot(call.args[0].value.get());
            return llvm::ConstantInt::get(i32(), 0);  // unit
        }
        // fix #1474 / stdlib §4.10: OwningSet<T>.UnionWith(owned other:
        // OwningSet<T>) for class-typed T (array-backed path — class elements
        // are ineligible for the dict-backed set path since genericBackingType
        // returns null for class types; the `isSetName` block near the top of
        // this method is therefore dead for every valid OwningSet instantiation,
        // since OwningSet<T> requires `where T: class`). `other` is drained
        // element-by-element through the same dedup-insert helper Add uses: a
        // duplicate (per self's current contents, which grow as `other`'s
        // unique elements are appended) is disposed, a fresh element is
        // transferred into self. Once every element of `other` has been
        // resolved (transferred or disposed), `other`'s own backing array is
        // freed with a NULL element disposer — every element it held has
        // already had its ownership resolved by the loop, so a second pass over
        // them here would double-free/-dispose.
        if (m == "UnionWith" && call.args.size() == 1 && call.args[0].value &&
            isOwnedSetRecv && arrElemIsClass && arrIsOwning) {
            llvm::Value* other = lowerExpr(*call.args[0].value);
            if (!other) return nullptr;
            auto* lenFn = declareC("bz_array_len", i64(), {ptrTy()});
            auto* ptrFn = declareC("bz_array_ptr_at", ptrTy(),
                                   {ptrTy(), i64(), i64()});
            llvm::Value* otherLen =
                builder_.CreateCall(lenFn, {other}, "uw.olen");
            auto* oIdxSlot = entryAlloca(i64());
            builder_.CreateStore(builder_.getInt64(0), oIdxSlot);
            auto* ocondBB = llvm::BasicBlock::Create(llvm_, "uw.c", currentFn_);
            auto* obodyBB = llvm::BasicBlock::Create(llvm_, "uw.b", currentFn_);
            auto* oendBB  = llvm::BasicBlock::Create(llvm_, "uw.e", currentFn_);
            builder_.CreateBr(ocondBB);
            builder_.SetInsertPoint(ocondBB);
            llvm::Value* oIdx = builder_.CreateLoad(i64(), oIdxSlot, "uw.i");
            builder_.CreateCondBr(
                builder_.CreateICmpSLT(oIdx, otherLen), obodyBB, oendBB);
            builder_.SetInsertPoint(obodyBB);
            {
                llvm::Value* oep = builder_.CreateCall(
                    ptrFn, {other, oIdx, builder_.getInt64(esz)}, "uw.oep");
                llvm::Value* item = builder_.CreateLoad(ptrTy(), oep, "uw.item");
                if (!emitSetDedupInsert(item)) return nullptr;
                // emitSetDedupInsert leaves the builder positioned at its
                // "sa.end" block; continue the outer loop from there.
                builder_.CreateStore(
                    builder_.CreateAdd(oIdx, builder_.getInt64(1)), oIdxSlot);
                builder_.CreateBr(ocondBB);
            }
            builder_.SetInsertPoint(oendBB);
            // `other`'s elements have all been transferred or disposed above;
            // free its now-empty backing with a NULL disposer (mirrors the
            // established "consumed materializer" idiom, e.g. freeLazyTemp /
            // freeUnboundOwnedBorrowingArrayTemp elsewhere in this file).
            auto* freeFn = declareC("bz_array_free",
                llvm::Type::getVoidTy(llvm_), {ptrTy(), ptrTy()});
            builder_.CreateCall(freeFn,
                {other, llvm::ConstantPointerNull::get(ptrTy())});
            return llvm::ConstantInt::get(i32(), 0);  // unit
        }
        // fix #1478 / stdlib §4.10: OwningSet<T>.ExceptWith(other: IEnumerable<T>)
        // for class-typed T (array-backed path — class elements are ineligible
        // for the dict-backed set path since genericBackingType returns null for
        // class types, so the isSetName block near the top of this method is
        // dead for every valid OwningSet instantiation; see the #1474 UnionWith
        // comment above). Unlike UnionWith, `other` here is a BORROW
        // (`other: IEnumerable<T>`, not `owned`): its elements stay owned by
        // `other`, and per §4.10 only self's matching elements are removed AND
        // DISPOSED ("disposes elements in other"). `other` may be any
        // IEnumerable<T> (Array<T>, another Set, a LINQ pipeline, …), so it is
        // materialized once via the same lowerToLazyEnumerator + bz_lazy_to_array
        // path the ToArray() terminal uses (§5) into a throwaway snapshot array
        // of the SAME borrowed element pointers `other` holds — the snapshot
        // never takes ownership, so it is freed afterward with a NULL disposer.
        // Self is then scanned once: for each element that equals some snapshot
        // entry (§8.3 equality, honouring a user Equals override via
        // emitRemoveElementEquality), it is removed and disposed via
        // bz_array_remove_at_disposing, mirroring the .Remove(item) loop above.
        // The scan index is NOT advanced on a removal — bz_array_remove_at shifts
        // later elements down into the freed slot, so the next element to test is
        // already at the same index.
        if (m == "ExceptWith" && call.args.size() == 1 && call.args[0].value &&
            isOwnedSetRecv && arrElemIsClass && arrIsOwning) {
            sema::TypeRef* otherElemRef = nullptr;
            llvm::Value* lazy =
                lowerToLazyEnumerator(*call.args[0].value, otherElemRef);
            if (!lazy) return nullptr;
            // Element LLVM-type mismatch (an ill-typed `other`) → defer rather
            // than read snapshot bytes at the wrong stride (mirrors
            // tryLowerSetAlgebra's same guard for the dict-backed path).
            if (!otherElemRef ||
                lowerTypeRef(otherElemRef) != lowerTypeRef(arrElemRef))
                return nullptr;
            auto* toArrFn = declareC("bz_lazy_to_array", ptrTy(), {ptrTy(), i64()});
            llvm::Value* snap = builder_.CreateCall(
                toArrFn, {lazy, builder_.getInt64(esz)}, "ew.snap");

            auto* lenFn = declareC("bz_array_len", i64(), {ptrTy()});
            auto* ptrFn = declareC("bz_array_ptr_at", ptrTy(),
                                   {ptrTy(), i64(), i64()});
            llvm::Value* snapLen =
                builder_.CreateCall(lenFn, {snap}, "ew.slen");
            llvm::Function* rmDisp = arrElemPol.disposerThunk;

            auto* idxSlot = entryAlloca(i64());
            builder_.CreateStore(builder_.getInt64(0), idxSlot);
            auto* condBB = llvm::BasicBlock::Create(llvm_, "ew.c", currentFn_);
            auto* bodyBB = llvm::BasicBlock::Create(llvm_, "ew.b", currentFn_);
            auto* rmBB   = llvm::BasicBlock::Create(llvm_, "ew.rm", currentFn_);
            auto* nextBB = llvm::BasicBlock::Create(llvm_, "ew.n", currentFn_);
            auto* endBB  = llvm::BasicBlock::Create(llvm_, "ew.e", currentFn_);
            builder_.CreateBr(condBB);
            builder_.SetInsertPoint(condBB);
            llvm::Value* curLen =
                builder_.CreateCall(lenFn, {recv}, "ew.len");
            llvm::Value* idx = builder_.CreateLoad(i64(), idxSlot, "ew.i");
            builder_.CreateCondBr(
                builder_.CreateICmpSLT(idx, curLen), bodyBB, endBB);
            builder_.SetInsertPoint(bodyBB);
            {
                llvm::Value* ep = builder_.CreateCall(
                    ptrFn, {recv, idx, builder_.getInt64(esz)}, "ew.ep");
                llvm::Value* el = builder_.CreateLoad(ptrTy(), ep, "ew.el");

                // Inner scan: does `el` match any snapshot entry?
                auto* jSlot = entryAlloca(i64());
                builder_.CreateStore(builder_.getInt64(0), jSlot);
                auto* jcondBB  = llvm::BasicBlock::Create(llvm_, "ew.jc", currentFn_);
                auto* jbodyBB  = llvm::BasicBlock::Create(llvm_, "ew.jb", currentFn_);
                auto* jfoundBB = llvm::BasicBlock::Create(llvm_, "ew.jf", currentFn_);
                auto* jnextBB  = llvm::BasicBlock::Create(llvm_, "ew.jn", currentFn_);
                auto* jendBB   = llvm::BasicBlock::Create(llvm_, "ew.je", currentFn_);
                builder_.CreateBr(jcondBB);
                builder_.SetInsertPoint(jcondBB);
                llvm::Value* jIdx = builder_.CreateLoad(i64(), jSlot, "ew.j");
                builder_.CreateCondBr(
                    builder_.CreateICmpSLT(jIdx, snapLen), jbodyBB, jendBB);
                builder_.SetInsertPoint(jbodyBB);
                llvm::Value* jep = builder_.CreateCall(
                    ptrFn, {snap, jIdx, builder_.getInt64(esz)}, "ew.jep");
                llvm::Value* sel = builder_.CreateLoad(ptrTy(), jep, "ew.sel");
                llvm::Value* same = emitRemoveElementEquality(
                    el, sel, arrElemRef, /*elemIsClass=*/true);
                if (!same) return nullptr;
                builder_.CreateCondBr(same, jfoundBB, jnextBB);
                builder_.SetInsertPoint(jnextBB);
                builder_.CreateStore(
                    builder_.CreateAdd(jIdx, builder_.getInt64(1)), jSlot);
                builder_.CreateBr(jcondBB);
                builder_.SetInsertPoint(jfoundBB);
                builder_.CreateBr(jendBB);
                builder_.SetInsertPoint(jendBB);
                auto* matched = builder_.CreatePHI(i1(), 2, "ew.matched");
                matched->addIncoming(builder_.getTrue(), jfoundBB);
                matched->addIncoming(builder_.getFalse(), jcondBB);
                builder_.CreateCondBr(matched, rmBB, nextBB);
            }
            // Found in `other`: remove (and dispose, per §4.10) self's element.
            // Do NOT advance idx — the shift-down leaves the next element here.
            builder_.SetInsertPoint(rmBB);
            if (rmDisp) {
                auto* rmFn = declareC("bz_array_remove_at_disposing",
                    llvm::Type::getVoidTy(llvm_),
                    {ptrTy(), i64(), i64(), ptrTy()});
                builder_.CreateCall(rmFn,
                    {recv, idx, builder_.getInt64(esz), rmDisp});
            } else {
                auto* rmFn = declareC("bz_array_remove_at",
                    llvm::Type::getVoidTy(llvm_), {ptrTy(), i64(), i64()});
                builder_.CreateCall(rmFn, {recv, idx, builder_.getInt64(esz)});
            }
            builder_.CreateBr(condBB);
            // Not found: keep self's element, advance to the next index.
            builder_.SetInsertPoint(nextBB);
            builder_.CreateStore(
                builder_.CreateAdd(idx, builder_.getInt64(1)), idxSlot);
            builder_.CreateBr(condBB);
            builder_.SetInsertPoint(endBB);
            // `snap` never took ownership of the (borrowed) elements it copied
            // from `other` — free its shell only, per the "consumed snapshot"
            // idiom UnionWith's `other` free uses above.
            auto* freeSnapFn = declareC("bz_array_free",
                llvm::Type::getVoidTy(llvm_), {ptrTy(), ptrTy()});
            builder_.CreateCall(freeSnapFn,
                {snap, llvm::ConstantPointerNull::get(ptrTy())});
            return llvm::ConstantInt::get(i32(), 0);  // unit
        }
        // fix #1477 / stdlib §4.10: OwningSet<T>.IntersectWith(other:
        // IEnumerable<T>) for class-typed T (array-backed path — see the
        // #1474 UnionWith comment above for why the dict-backed isSetName
        // block near the top of this method is dead for every valid
        // OwningSet instantiation, since OwningSet<T> requires `where T:
        // class`). Per §4.10 ("disposes elements not in other"), `other` is
        // a BORROW: its elements stay owned by `other`. Self keeps only the
        // elements that ALSO appear in `other`; every self element that does
        // NOT appear in `other` is removed and disposed. This is exactly
        // ExceptWith above with the match outcome inverted: `other` is
        // materialized once via the same lowerToLazyEnumerator +
        // bz_lazy_to_array snapshot (freed afterward with a NULL disposer —
        // it never took ownership), self is scanned once, and each element
        // is compared against the snapshot via emitRemoveElementEquality
        // (§8.3 equality, honouring a user Equals override). A match is
        // KEPT (index advances, mirroring ExceptWith's "not found" arm); a
        // non-match is REMOVED+DISPOSED via bz_array_remove_at_disposing
        // (index does not advance — the shift-down leaves the next element
        // at the same slot, mirroring ExceptWith's "found" arm).
        if (m == "IntersectWith" && call.args.size() == 1 && call.args[0].value &&
            isOwnedSetRecv && arrElemIsClass && arrIsOwning) {
            sema::TypeRef* otherElemRef = nullptr;
            llvm::Value* lazy =
                lowerToLazyEnumerator(*call.args[0].value, otherElemRef);
            if (!lazy) return nullptr;
            // Element LLVM-type mismatch (an ill-typed `other`) → defer rather
            // than read snapshot bytes at the wrong stride (mirrors
            // tryLowerSetAlgebra's same guard for the dict-backed path).
            if (!otherElemRef ||
                lowerTypeRef(otherElemRef) != lowerTypeRef(arrElemRef))
                return nullptr;
            auto* toArrFn = declareC("bz_lazy_to_array", ptrTy(), {ptrTy(), i64()});
            llvm::Value* snap = builder_.CreateCall(
                toArrFn, {lazy, builder_.getInt64(esz)}, "iw.snap");

            auto* lenFn = declareC("bz_array_len", i64(), {ptrTy()});
            auto* ptrFn = declareC("bz_array_ptr_at", ptrTy(),
                                   {ptrTy(), i64(), i64()});
            llvm::Value* snapLen =
                builder_.CreateCall(lenFn, {snap}, "iw.slen");
            llvm::Function* rmDisp = arrElemPol.disposerThunk;

            auto* idxSlot = entryAlloca(i64());
            builder_.CreateStore(builder_.getInt64(0), idxSlot);
            auto* condBB = llvm::BasicBlock::Create(llvm_, "iw.c", currentFn_);
            auto* bodyBB = llvm::BasicBlock::Create(llvm_, "iw.b", currentFn_);
            auto* rmBB   = llvm::BasicBlock::Create(llvm_, "iw.rm", currentFn_);
            auto* keepBB = llvm::BasicBlock::Create(llvm_, "iw.keep", currentFn_);
            auto* endBB  = llvm::BasicBlock::Create(llvm_, "iw.e", currentFn_);
            builder_.CreateBr(condBB);
            builder_.SetInsertPoint(condBB);
            llvm::Value* curLen =
                builder_.CreateCall(lenFn, {recv}, "iw.len");
            llvm::Value* idx = builder_.CreateLoad(i64(), idxSlot, "iw.i");
            builder_.CreateCondBr(
                builder_.CreateICmpSLT(idx, curLen), bodyBB, endBB);
            builder_.SetInsertPoint(bodyBB);
            {
                llvm::Value* ep = builder_.CreateCall(
                    ptrFn, {recv, idx, builder_.getInt64(esz)}, "iw.ep");
                llvm::Value* el = builder_.CreateLoad(ptrTy(), ep, "iw.el");

                // Inner scan: does `el` match any snapshot entry?
                auto* jSlot = entryAlloca(i64());
                builder_.CreateStore(builder_.getInt64(0), jSlot);
                auto* jcondBB  = llvm::BasicBlock::Create(llvm_, "iw.jc", currentFn_);
                auto* jbodyBB  = llvm::BasicBlock::Create(llvm_, "iw.jb", currentFn_);
                auto* jfoundBB = llvm::BasicBlock::Create(llvm_, "iw.jf", currentFn_);
                auto* jnextBB  = llvm::BasicBlock::Create(llvm_, "iw.jn", currentFn_);
                auto* jendBB   = llvm::BasicBlock::Create(llvm_, "iw.je", currentFn_);
                builder_.CreateBr(jcondBB);
                builder_.SetInsertPoint(jcondBB);
                llvm::Value* jIdx = builder_.CreateLoad(i64(), jSlot, "iw.j");
                builder_.CreateCondBr(
                    builder_.CreateICmpSLT(jIdx, snapLen), jbodyBB, jendBB);
                builder_.SetInsertPoint(jbodyBB);
                llvm::Value* jep = builder_.CreateCall(
                    ptrFn, {snap, jIdx, builder_.getInt64(esz)}, "iw.jep");
                llvm::Value* sel = builder_.CreateLoad(ptrTy(), jep, "iw.sel");
                llvm::Value* same = emitRemoveElementEquality(
                    el, sel, arrElemRef, /*elemIsClass=*/true);
                if (!same) return nullptr;
                builder_.CreateCondBr(same, jfoundBB, jnextBB);
                builder_.SetInsertPoint(jnextBB);
                builder_.CreateStore(
                    builder_.CreateAdd(jIdx, builder_.getInt64(1)), jSlot);
                builder_.CreateBr(jcondBB);
                builder_.SetInsertPoint(jfoundBB);
                builder_.CreateBr(jendBB);
                builder_.SetInsertPoint(jendBB);
                auto* matched = builder_.CreatePHI(i1(), 2, "iw.matched");
                matched->addIncoming(builder_.getTrue(), jfoundBB);
                matched->addIncoming(builder_.getFalse(), jcondBB);
                // §4.10: IntersectWith KEEPS a matching element and REMOVES a
                // non-matching one — the inverse of ExceptWith's branch.
                builder_.CreateCondBr(matched, keepBB, rmBB);
            }
            // Not found in `other`: remove (and dispose, per §4.10 "disposes
            // elements not in other") self's element. Do NOT advance idx —
            // the shift-down leaves the next element here.
            builder_.SetInsertPoint(rmBB);
            if (rmDisp) {
                auto* rmFn = declareC("bz_array_remove_at_disposing",
                    llvm::Type::getVoidTy(llvm_),
                    {ptrTy(), i64(), i64(), ptrTy()});
                builder_.CreateCall(rmFn,
                    {recv, idx, builder_.getInt64(esz), rmDisp});
            } else {
                auto* rmFn = declareC("bz_array_remove_at",
                    llvm::Type::getVoidTy(llvm_), {ptrTy(), i64(), i64()});
                builder_.CreateCall(rmFn, {recv, idx, builder_.getInt64(esz)});
            }
            builder_.CreateBr(condBB);
            // Found: keep self's element, advance to the next index.
            builder_.SetInsertPoint(keepBB);
            builder_.CreateStore(
                builder_.CreateAdd(idx, builder_.getInt64(1)), idxSlot);
            builder_.CreateBr(condBB);
            builder_.SetInsertPoint(endBB);
            // `snap` never took ownership of the (borrowed) elements it copied
            // from `other` — free its shell only, per the "consumed snapshot"
            // idiom UnionWith's `other` free / ExceptWith's snapshot free use.
            auto* freeSnapFn2 = declareC("bz_array_free",
                llvm::Type::getVoidTy(llvm_), {ptrTy(), ptrTy()});
            builder_.CreateCall(freeSnapFn2,
                {snap, llvm::ConstantPointerNull::get(ptrTy())});
            return llvm::ConstantInt::get(i32(), 0);  // unit
        }
        if ((m == "Add" || m == "Push" || m == "Enqueue") &&
            call.args.size() == 1 && call.args[0].value) {
            llvm::Value* v = lowerExpr(*call.args[0].value);
            if (!v) return nullptr;
            v = ensureNullableElem(v);
            // #819 / §14.4: an element bound to an INTERFACE/Object element slot
            // must be heap-boxed when it is a value type (record/enum/primitive)
            // so a vtable sits at word 0 of the stored pointer. Otherwise the raw
            // value bytes are stored in the pointer-sized slot and the element
            // disposer (elementDisposerThunk) treats the first word as a vtable
            // pointer → SIGSEGV. boxForTarget is a no-op for a class/already-
            // pointer element or a non-Object element slot.
            if (recvTy && !recvTy->genericArgs.empty() && recvTy->genericArgs[0])
                v = boxForTarget(v, recvTy->genericArgs[0],
                                 call.args[0].value.get());
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
            // fix #1881 / §28.3 Unbound owned temporaries / §28.11 "Storing a
            // borrow": a BORROWING collection's Add/Push/Enqueue takes a
            // BORROW — its element parameter carries no `owned` marker — so
            // the call transfers no ownership. Per §2771's own citation,
            // "Add, Insert, and the indexer-set on a borrowing collection
            // remain governed by §28.3's unbound-temporary admission and
            // §28.11's residual-trust store rule" — this call is admitted
            // (only the COLLECTION-LITERAL form is rejected at the literal
            // site, §2771), and §28.3 enumerates "a non-owned argument" as
            // one of the borrowing uses an unbound owned temporary may serve
            // before being "disposed at the end of the full expression".
            // Before this fix nothing disposed such an argument here: a
            // borrowing collection never takes the #1955 owning-transfer
            // path below (arrIsOwning is false), and no other codegen site
            // reaches an Add/Push/Enqueue argument — `new Res(1)` in
            // `arr.Add(new Res(1))` leaked with its destructor never run
            // (issue #1881). Add/Push/Enqueue return unit, so nothing in
            // this statement can chain off the call's result — the call's
            // own end IS the full-expression boundary for this argument.
            // isFreshOwnedClassProducer (shared with the #1039/#1053
            // receiver-temp fix) is the sound gate: a BORROW argument (a
            // named-binding read, a field, another borrow-returning call) is
            // not a fresh producer and stays undisposed here — no UAF of a
            // value some other holder still owns. Disposing the fresh
            // producer here creates exactly the "borrow-lifetime hazard that
            // checked disposal mode backstops" the spec names at §2771: the
            // copy just stored in `recv` dangles from this point on,
            // backstopped by DisposedAccessException in checked-disposal
            // mode (undefined behavior, by design, unchecked) — the same
            // accepted residual as every other borrowing-collection store
            // (§28.11's "Storing a borrow" store-site rule).
            if (!arrIsOwning && isFreshOwnedClassProducer(call.args[0].value.get()))
                emitDisposeAndReclaim(v);
            // #1955 / §28.3: for an OWNING collection the element parameter is
            // `owned` (the collection takes over the element's disposal), so a
            // bare owned local/param element argument's ownership just
            // transferred into the collection. Null its caller slot so the
            // caller's null-guarded unwindOnly disposal — which
            // UnwindIneligibleWalker now leaves eligible for this callee shape
            // (calleeIsOwningCollectionTransfer) — is exactly-once: it skips the
            // just-moved value (no double-free with the collection's element
            // disposal) yet disposes a value still live at a throw that PRECEDES
            // the transfer. A BORROWING Array<T>.Add keeps the caller as owner —
            // no transfer, no nulling.
            if (arrIsOwning)
                nullTransferredOwnedLocalSlot(call.args[0].value.get());
            return llvm::ConstantInt::get(i32(), 0);  // unit/void result
        }
        // fix #654-tail B / stdlib §4.10: OwningSet<T>.TryAdd(owned item): bool
        // for class-typed T (array-backed path — class elements are ineligible
        // for the dict-backed set path since genericBackingType returns null for
        // class types). Equality for class T follows §8.3: a user `Equals`
        // override is honoured (#837), otherwise reference identity.
        // Semantics: scan the array for an EQUAL element; if not found, append
        // and return true; if found, dispose the passed item (caller transferred
        // ownership; §4.10 "on false, the item is disposed") and return false.
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
            // §8.3 / §4.10 (#837): match by the element's equality contract — a
            // user `Equals` override is dispatched (so a content-equal element is
            // a duplicate), else reference identity (the §8.3 class default).
            llvm::Value* same = emitRemoveElementEquality(
                existing, item, recvTy->genericArgs[0], /*elemIsClass=*/true);
            if (!same) return nullptr;
            builder_.CreateCondBr(same, foundBB, nextBB);
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
        // fix #1476 / stdlib §4.10: OwningSet<T>.Take(query: T): owned T —
        // finds the element EQUAL to `query` (§8.3 equality: a user Equals
        // override is honoured, else reference identity — same contract
        // emitRemoveElementEquality dispatches for TryAdd/Remove above),
        // removes it, and transfers its ownership to the caller. Raises
        // InvalidOperationException when no equal element is present (mirrors
        // OwningDictionary.PopValue's "raises if not present" contract,
        // stdlib §4.9 / LlvmBackend_Calls.cpp's PopValue handler below);
        // TryTake (next) is the no-throw variant. `query` is a PURE BORROW —
        // never consumed or disposed (stdlib §4.10 "borrow-taking query").
        //
        // Before this handler, "Take" on an OwningSet was intercepted upstream
        // by the generic LINQ Take(count: int) operator (now gated off for an
        // OwningSet receiver in LlvmBackend_Calls3.cpp's tryLowerLazyLinqCall),
        // which treated the class-pointer query as an int count and emitted
        // `sext ptr ... to i64` — malformed IR LLVM rejects.
        if (m == "Take" && call.args.size() == 1 && call.args[0].value &&
            isOwnedSetRecv && arrElemIsClass && arrIsOwning) {
            llvm::Value* query = lowerExpr(*call.args[0].value);
            if (!query) return nullptr;
            auto* lenFn = declareC("bz_array_len", i64(), {ptrTy()});
            llvm::Value* len = builder_.CreateCall(lenFn, {recv}, "tk.len");
            auto* ptrFn = declareC("bz_array_ptr_at", ptrTy(),
                                   {ptrTy(), i64(), i64()});
            auto* idxSlot = entryAlloca(i64());
            builder_.CreateStore(builder_.getInt64(0), idxSlot);
            auto* condBB  = llvm::BasicBlock::Create(llvm_, "tk.c", currentFn_);
            auto* bodyBB  = llvm::BasicBlock::Create(llvm_, "tk.b", currentFn_);
            auto* foundBB = llvm::BasicBlock::Create(llvm_, "tk.f", currentFn_);
            auto* nextBB  = llvm::BasicBlock::Create(llvm_, "tk.n", currentFn_);
            auto* endBB   = llvm::BasicBlock::Create(llvm_, "tk.e", currentFn_);
            builder_.CreateBr(condBB);
            builder_.SetInsertPoint(condBB);
            llvm::Value* idx = builder_.CreateLoad(i64(), idxSlot, "tk.i");
            builder_.CreateCondBr(
                builder_.CreateICmpSLT(idx, len), bodyBB, endBB);
            builder_.SetInsertPoint(bodyBB);
            llvm::Value* ep = builder_.CreateCall(
                ptrFn, {recv, idx, builder_.getInt64(esz)}, "tk.ep");
            llvm::Value* existing = builder_.CreateLoad(ptrTy(), ep, "tk.ex");
            llvm::Value* eq = emitRemoveElementEquality(
                existing, query, recvTy->genericArgs[0], /*elemIsClass=*/true);
            if (!eq) return nullptr;
            builder_.CreateCondBr(eq, foundBB, nextBB);
            builder_.SetInsertPoint(nextBB);
            builder_.CreateStore(
                builder_.CreateAdd(idx, builder_.getInt64(1)), idxSlot);
            builder_.CreateBr(condBB);
            builder_.SetInsertPoint(foundBB);
            builder_.CreateBr(endBB);
            builder_.SetInsertPoint(endBB);
            auto* foundPhi = builder_.CreatePHI(i1(), 2, "tk.found");
            foundPhi->addIncoming(builder_.getTrue(), foundBB);
            foundPhi->addIncoming(builder_.getFalse(), condBB);
            auto* idxPhi = builder_.CreatePHI(i64(), 2, "tk.idxphi");
            idxPhi->addIncoming(idx, foundBB);
            idxPhi->addIncoming(builder_.getInt64(0), condBB);
            auto* valPhi = builder_.CreatePHI(ptrTy(), 2, "tk.val");
            valPhi->addIncoming(existing, foundBB);
            valPhi->addIncoming(llvm::ConstantPointerNull::get(ptrTy()), condBB);
            llvm::Value* notFound = builder_.CreateNot(foundPhi, "tk.notfound");
            emitGuardThrow(notFound, "InvalidOperationException");
            // Remove the matched slot WITHOUT disposing — ownership transfers
            // to the caller (unlike Remove(item), which disposes; #1476 /
            // stdlib §4.10). Mirrors Pop/Dequeue's get-then-remove shape
            // below.
            auto* rmFn = declareC("bz_array_remove_at",
                llvm::Type::getVoidTy(llvm_), {ptrTy(), i64(), i64()});
            builder_.CreateCall(rmFn, {recv, idxPhi, builder_.getInt64(esz)});
            return valPhi;
        }
        // fix #1476 / stdlib §4.10: OwningSet<T>.TryTake(query: T, out owned
        // removed: T?): bool — the no-throw analogue of Take above: finds the
        // element equal to `query`, removes it (WITHOUT disposing — ownership
        // transfers through the out param) and returns true; on no match,
        // writes null through the out param (mirrors TryPopValue/TryGetValue's
        // "pre-null on the false path" convention, #891) and returns false.
        // `query` is a pure borrow, never disposed. Discovered broken (silent
        // no-op / always-false, `r` left null) alongside Take while fixing
        // #1476 — same missing-handler root cause (no codegen dispatch existed
        // for either method on a class-typed OwningSet receiver).
        if (m == "TryTake" && call.args.size() == 2 &&
            call.args[0].value && call.args[1].value &&
            isOwnedSetRecv && arrElemIsClass && arrIsOwning) {
            llvm::Value* query = lowerExpr(*call.args[0].value);
            if (!query) return nullptr;
            // fix #2938 / §22.3: see TryGetValue above.
            llvm::Type* outTy = nullptr;
            std::vector<AccessorRefArg> outArgs;
            llvm::Value* outAddr = resolveOutAddrWithBracket(
                *call.args[1].value, outTy, outArgs);
            if (!outAddr) {
                emitAccessorRefArgWriteBacks(outArgs, /*emit=*/false);
                return nullptr;
            }
            // False-path null now (§22.3 / #891 pre-null convention); the
            // found path below overwrites it. T is always class (`where T:
            // class`, stdlib §4.10), so this is always a plain nullable
            // pointer slot (never the tagged {i8,T} nullable-value struct).
            storeTryOutNull(outAddr, outTy);
            auto* lenFn = declareC("bz_array_len", i64(), {ptrTy()});
            llvm::Value* len = builder_.CreateCall(lenFn, {recv}, "tt.len");
            auto* ptrFn = declareC("bz_array_ptr_at", ptrTy(),
                                   {ptrTy(), i64(), i64()});
            auto* idxSlot = entryAlloca(i64());
            builder_.CreateStore(builder_.getInt64(0), idxSlot);
            auto* condBB  = llvm::BasicBlock::Create(llvm_, "tt.c",    currentFn_);
            auto* bodyBB  = llvm::BasicBlock::Create(llvm_, "tt.b",    currentFn_);
            auto* foundBB = llvm::BasicBlock::Create(llvm_, "tt.f",    currentFn_);
            auto* nextBB  = llvm::BasicBlock::Create(llvm_, "tt.next", currentFn_);
            auto* endBB   = llvm::BasicBlock::Create(llvm_, "tt.end",  currentFn_);
            builder_.CreateBr(condBB);
            builder_.SetInsertPoint(condBB);
            llvm::Value* idx = builder_.CreateLoad(i64(), idxSlot, "tt.i");
            builder_.CreateCondBr(
                builder_.CreateICmpSLT(idx, len), bodyBB, endBB);
            builder_.SetInsertPoint(bodyBB);
            llvm::Value* ep = builder_.CreateCall(
                ptrFn, {recv, idx, builder_.getInt64(esz)}, "tt.ep");
            llvm::Value* existing = builder_.CreateLoad(ptrTy(), ep, "tt.ex");
            llvm::Value* eq = emitRemoveElementEquality(
                existing, query, recvTy->genericArgs[0], /*elemIsClass=*/true);
            if (!eq) return nullptr;
            builder_.CreateCondBr(eq, foundBB, nextBB);
            builder_.SetInsertPoint(nextBB);
            builder_.CreateStore(
                builder_.CreateAdd(idx, builder_.getInt64(1)), idxSlot);
            builder_.CreateBr(condBB);
            // found: remove WITHOUT disposing (ownership transfers through the
            // out param), overwrite the pre-stored null with the owned ptr.
            builder_.SetInsertPoint(foundBB);
            {
                auto* rmFn = declareC("bz_array_remove_at",
                    llvm::Type::getVoidTy(llvm_), {ptrTy(), i64(), i64()});
                builder_.CreateCall(rmFn, {recv, idx, builder_.getInt64(esz)});
                storeTryOutValue(outAddr, outTy, existing, /*isValTy=*/false);
            }
            llvm::BasicBlock* foundEnd = builder_.GetInsertBlock();
            builder_.CreateBr(endBB);
            builder_.SetInsertPoint(endBB);
            // fix #2938 / §22.3: flush any bracket on the normal-return edge.
            emitAccessorRefArgWriteBacks(outArgs, /*emit=*/true);
            auto* phi2 = builder_.CreatePHI(i1(), 2, "tt.r");
            phi2->addIncoming(builder_.getTrue(), foundEnd);
            phi2->addIncoming(builder_.getFalse(), condBB);
            return phi2;
        }
        // per §4.5: .Insert(index, item) — insert at index, shifting up. For an
        // OwningArray<T> the item is `owned` (already moved in by the caller);
        // there is no element to dispose (a fresh slot is created, not a
        // replacement), so insertion is the same op for owning and borrowing
        // arrays — the runtime just shifts and stores the element bytes.
        if (m == "Insert" && call.args.size() == 2 &&
            call.args[0].value && call.args[1].value) {
            llvm::Value* idx = lowerExpr(*call.args[0].value);
            llvm::Value* v   = lowerExpr(*call.args[1].value);
            if (!idx || !v) return nullptr;
            idx = builder_.CreateSExt(idx, i64());
            // per §4.5 (#1200): Insert(index, item) raises IndexOutOfRangeException
            // when index < 0 or index > Count (Insert at exactly Count is valid —
            // it appends; mirrors the BoundsCheck convention in stdlib §4.5).
            {
                auto* insLenFn = declareC("bz_array_len", i64(), {ptrTy()});
                llvm::Value* insLen = builder_.CreateCall(insLenFn, {recv}, "ins.len");
                llvm::Value* insNeg = builder_.CreateICmpSLT(
                    idx, builder_.getInt64(0), "ins.neg");
                llvm::Value* insOor = builder_.CreateICmpSGT(idx, insLen, "ins.oor");
                emitGuardThrow(builder_.CreateOr(insNeg, insOor, "ins.oob"),
                               "IndexOutOfRangeException");
            }
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
            // fix #2204 / §28.3 Unbound owned temporaries / §28.11 "Storing a
            // borrow": the SAME fresh-producer argument leak the #1881 Add/
            // Push/Enqueue fix disposes exists here for a BORROWING array's
            // `.Insert(index, item)` — an owned TEMPORARY (`new R(1)`) is a
            // non-owned argument to a borrow-taking call, transfers no
            // ownership (§28.3's "non-owned argument"), and under the pre-fix
            // codegen was never disposed: dtor never ran, LSan reported 16B
            // leaked. Per §28.3 v0.226 the call's end IS the full-expression
            // boundary for this argument (Insert returns unit — nothing can
            // chain off the result). Dispose a fresh-producer argument
            // (isFreshOwnedClassProducer — the same discriminant the #1039/
            // #1053 receiver-temp and #1881 Add/Push/Enqueue fixes use) right
            // after the bz_array_insert_at call, gated on the receiving
            // collection being BORROWING (`!arrIsOwning`; an OWNING collection
            // instead takes ownership via the #1955 nulling path below —
            // mutually exclusive, no double dispatch). This intentionally
            // creates the same "borrow-lifetime hazard that checked disposal
            // mode backstops" the spec names: the copy just stored in `recv`
            // dangles from this point on, backstopped by DisposedAccessException
            // in checked-disposal mode, undefined behavior (by design)
            // unchecked — the same accepted residual as every other
            // borrowing-collection store (§28.11).
            if (!arrIsOwning && isFreshOwnedClassProducer(call.args[1].value.get()))
                emitDisposeAndReclaim(v);
            // #1955 / §28.3: for an OWNING collection Insert takes ownership of
            // the element (arg 1) — null the caller's slot for a bare owned
            // local/param so the caller's null-guarded unwind disposal stays
            // exactly-once (see the Add/Push/Enqueue path above). Borrowing
            // Array<T>.Insert keeps the caller as owner — no nulling.
            if (arrIsOwning)
                nullTransferredOwnedLocalSlot(call.args[1].value.get());
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
                // Queue/OwningQueue — front element (index 0); Stack/OwningStack
                // peek the top element (last inserted, index len-1).
                bool isQueuePeek = recvTy && recvTy->astNode &&
                    recvTy->astNode->name &&
                    !recvTy->astNode->name->parts.empty() &&
                    (recvTy->headName() == "Queue" ||
                     recvTy->headName() == "OwningQueue");
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
            // #919: pick the displaced-element disposer — the string-release
            // thunk for a borrowing Array<string>, OR the element disposer thunk
            // for an OWNING collection of class elements (OwningArray<Cell>).
            // Without the latter, .Set leaked the replaced owned element; the
            // array owns it, so disposing it (exactly as the indexer setter
            // `arr[i] := v` does) is sound — no double-free.
            llvm::Function* setDisp =
                arrIsStringElem ? stringElementReleaseThunk()
                                : (arrIsOwning ? arrElemPol.disposerThunk
                                               : nullptr);
            if (setDisp) {
                // A borrowing string array co-owns a +1 per element: retain the
                // new string unless it is already a +1 producer. A class element
                // is moved into the slot, so no retain.
                if (arrIsStringElem &&
                    !isPlusOneStringProducer(call.args[1].value.get()))
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
                                         builder_.getInt64(esz), setDisp});
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
            // per §4.5 (#1200): RemoveAt(index) raises IndexOutOfRangeException
            // when index < 0 or index >= Count (valid range 0..Count-1), matching
            // the BoundsCheck convention in stdlib §4.5 and the PopAt guard (#1199).
            {
                auto* raLenFn = declareC("bz_array_len", i64(), {ptrTy()});
                llvm::Value* raLen = builder_.CreateCall(raLenFn, {recv}, "ra.len");
                llvm::Value* raNeg = builder_.CreateICmpSLT(
                    idx, builder_.getInt64(0), "ra.neg");
                llvm::Value* raOor = builder_.CreateICmpSGE(idx, raLen, "ra.oor");
                emitGuardThrow(builder_.CreateOr(raNeg, raOor, "ra.oob"),
                               "IndexOutOfRangeException");
            }
            // #602 / #362 / §4.5 / #429 / §4.6 / §28.9 / §28.11 v0.135 fix #619:
            // OwningArray<T>.RemoveAt disposes the removed element (ClassOwned row —
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
        // and remove it; returns whether one was found. For an OwningArray<T> the
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
        // §4.11/§4.12 (#725): Stack.TryPop/TryPeek, Queue.TryDequeue/TryPeek
        // (out item: T): bool. On a non-empty collection, write the top (Stack)
        // or front (Queue) element to the out param and — for TryPop/TryDequeue
        // — remove it, returning true; on empty, return false and leave the out
        // param unchanged. The removal is NON-disposing: the element transfers to
        // the out param (a borrow for a borrowing collection, an owned value for
        // an Owned* collection). TryPeek never removes. Modeled on Pop/Dequeue +
        // the TryGetValue out-write; bz_array_len/get/remove_at already exist.
        if ((m == "TryPop" || m == "TryDequeue" || m == "TryPeek") &&
            call.args.size() == 1 && call.args[0].value) {
            // fix #2938 / §22.3: see TryGetValue above.
            llvm::Type* outTy = nullptr;
            std::vector<AccessorRefArg> outArgs;
            llvm::Value* outAddr = resolveOutAddrWithBracket(
                *call.args[0].value, outTy, outArgs);
            if (!outAddr) {
                emitAccessorRefArgWriteBacks(outArgs, /*emit=*/false);
                return nullptr;
            }
            // v0.171 (#1336): the out param is `T?`. A value instantiation
            // (Stack<int>/Queue<int>) uses the tagged `{i8,T}` slot; a class
            // instantiation a nullable pointer. On the FALSE (empty) path the
            // slot is assigned null; on the TRUE path the extracted value.
            const bool isValTy = tryOutIsNullableValue(recvTy, 0);
            auto* lenFn = declareC("bz_array_len", i64(), {ptrTy()});
            llvm::Value* len = builder_.CreateCall(lenFn, {recv}, "trypop.len");
            // False-path null: store null now (the found path overwrites it).
            storeTryOutNull(outAddr, outTy);
            llvm::Value* nonEmpty = builder_.CreateICmpNE(
                len, builder_.getInt64(0), "trypop.ne");
            auto* foundBB =
                llvm::BasicBlock::Create(llvm_, "trypop.found", currentFn_);
            auto* endBB =
                llvm::BasicBlock::Create(llvm_, "trypop.end", currentFn_);
            llvm::BasicBlock* preBB = builder_.GetInsertBlock();
            builder_.CreateCondBr(nonEmpty, foundBB, endBB);
            builder_.SetInsertPoint(foundBB);
            // top (Stack) vs front (Queue): TryPop → Stack top (len-1);
            // TryDequeue → Queue front (0); TryPeek → by the receiver kind.
            std::string_view cn =
                (recvTy && recvTy->astNode && recvTy->astNode->name &&
                 !recvTy->astNode->name->parts.empty())
                    ? recvTy->headName()
                    : std::string_view{};
            bool isStack = cn.find("Stack") != std::string_view::npos;
            bool front = (m == "TryDequeue") ? true
                       : (m == "TryPop")     ? false
                                             : !isStack;  // TryPeek
            llvm::Value* idx = front
                ? builder_.getInt64(0)
                : builder_.CreateSub(len, builder_.getInt64(1), "trypop.idx");
            auto* getFn = declareC("bz_array_get", llvm::Type::getVoidTy(llvm_),
                                   {ptrTy(), i64(), ptrTy(), i64()});
            if (isValTy) {
                // Read the raw non-nullable element into a temp, then wrap it
                // into the `{i8 1, T}` present form and store into the slot.
                llvm::Type* rawTy = getElemTy();
                llvm::AllocaInst* tmp = entryAlloca(rawTy);
                builder_.CreateCall(getFn,
                                    {recv, idx, tmp, builder_.getInt64(esz)});
                llvm::Value* raw = builder_.CreateLoad(rawTy, tmp, "trypop.raw");
                storeTryOutValue(outAddr, outTy, raw, /*isValTy=*/true);
            } else {
                builder_.CreateCall(getFn,
                                    {recv, idx, outAddr, builder_.getInt64(esz)});
            }
            if (m != "TryPeek") {
                auto* remFn = declareC("bz_array_remove_at",
                                       llvm::Type::getVoidTy(llvm_),
                                       {ptrTy(), i64(), i64()});
                builder_.CreateCall(remFn,
                                    {recv, idx, builder_.getInt64(esz)});
            }
            builder_.CreateBr(endBB);
            builder_.SetInsertPoint(endBB);
            // fix #2938 / §22.3: flush any bracket on the normal-return edge.
            emitAccessorRefArgWriteBacks(outArgs, /*emit=*/true);
            auto* phi = builder_.CreatePHI(i1(), 2, "trypop.r");
            phi->addIncoming(builder_.getTrue(), foundBB);
            phi->addIncoming(builder_.getFalse(), preBB);
            return phi;
        }
        // fix #642 / stdlib §4.5: .PopAt(index): owned T / .PopLast(): owned T /
        // .PopFirst(): owned T  — remove and return the element at the given slot,
        // transferring ownership to the caller.  Unlike Pop/Dequeue (Stack/Queue),
        // these are defined on OwningArray<T> and its alias forms.
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
            llvm::Value* idx = nullptr;
            if (m == "PopAt") {
                llvm::Value* rawIdx = lowerExpr(*call.args[0].value);
                if (!rawIdx) return nullptr;
                idx = builder_.CreateSExt(rawIdx, i64(), "popat.idx");
                // per stdlib §4.5 (#1199): PopAt(index) calls BoundsCheck(index)
                // first, raising IndexOutOfRangeException when index < 0 or
                // index >= Count (this also covers the empty-array case, where
                // Count == 0). Mirrors the OwningArray indexer / ElementAt
                // bounds-check. Without this guard bz_array_pop_at leaves *out
                // unwritten on an out-of-bounds index, so the caller reads an
                // uninitialized owned T and faults (SIGSEGV) instead of throwing.
                llvm::Value* neg = builder_.CreateICmpSLT(
                    idx, builder_.getInt64(0), "popat.neg");
                llvm::Value* oor = builder_.CreateICmpSGE(idx, len, "popat.oor");
                emitGuardThrow(builder_.CreateOr(neg, oor),
                               "IndexOutOfRangeException");
            } else {
                // PopFirst / PopLast raise InvalidOperationException on an empty
                // array, matching Stack.Pop / Queue.Dequeue (stdlib §4.5
                // "raises if empty"). The derived index is then in bounds.
                llvm::Value* isEmpty = builder_.CreateICmpEQ(
                    len, builder_.getInt64(0), "popat.empty");
                emitGuardThrow(isEmpty, "InvalidOperationException");
                if (m == "PopLast")
                    idx = builder_.CreateSub(len, builder_.getInt64(1), "popl.idx");
                else  // PopFirst
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
        // fix #831 / stdlib §4.5: .TryPopFirst(out owned item): bool /
        // .TryPopLast(out owned item): bool — the TRY analogue of PopFirst /
        // PopLast on OwningArray<T>. On a non-empty array write the first/last
        // element to the out param and remove it WITHOUT disposal (ownership
        // transfers to the out param), returning true; on empty return false and
        // leave the out param unchanged. Before this fix it fell through to no
        // handler and the result was a non-i1 value used as a branch condition →
        // malformed IR (bug16). Modeled on TryPop + the PopFirst/PopLast pop.
        if ((m == "TryPopFirst" || m == "TryPopLast") &&
            call.args.size() == 1 && call.args[0].value) {
            // fix #2938 / §22.3: see TryPop above.
            llvm::Type* outTy = nullptr;
            std::vector<AccessorRefArg> outArgs;
            llvm::Value* outAddr = resolveOutAddrWithBracket(
                *call.args[0].value, outTy, outArgs);
            if (!outAddr) {
                emitAccessorRefArgWriteBacks(outArgs, /*emit=*/false);
                return nullptr;
            }
            // v0.171 (#1336): the out param is `owned T?`. Owning collections
            // require class elements, so the slot is a nullable pointer; still,
            // handle the tagged-struct form uniformly. False path → null.
            const bool isValTy = tryOutIsNullableValue(recvTy, 0);
            auto* lenFn = declareC("bz_array_len", i64(), {ptrTy()});
            llvm::Value* len = builder_.CreateCall(lenFn, {recv}, "trypopfl.len");
            storeTryOutNull(outAddr, outTy);  // false-path null (§22.3)
            llvm::Value* nonEmpty = builder_.CreateICmpNE(
                len, builder_.getInt64(0), "trypopfl.ne");
            auto* foundBB =
                llvm::BasicBlock::Create(llvm_, "trypopfl.found", currentFn_);
            auto* endBB =
                llvm::BasicBlock::Create(llvm_, "trypopfl.end", currentFn_);
            llvm::BasicBlock* preBB = builder_.GetInsertBlock();
            builder_.CreateCondBr(nonEmpty, foundBB, endBB);
            builder_.SetInsertPoint(foundBB);
            llvm::Value* idx = (m == "TryPopFirst")
                ? builder_.getInt64(0)
                : builder_.CreateSub(len, builder_.getInt64(1), "trypopfl.idx");
            // bz_array_pop_at copies the element into *outAddr, then removes the
            // slot WITHOUT disposal — same non-disposing transfer PopFirst uses.
            auto* popFn = declareC("bz_array_pop_at",
                llvm::Type::getVoidTy(llvm_), {ptrTy(), i64(), ptrTy(), i64()});
            if (isValTy) {
                llvm::Type* rawTy = getElemTy();
                llvm::AllocaInst* tmp = entryAlloca(rawTy);
                builder_.CreateCall(popFn,
                    {recv, idx, tmp, builder_.getInt64(esz)});
                llvm::Value* raw = builder_.CreateLoad(rawTy, tmp, "trypopfl.raw");
                storeTryOutValue(outAddr, outTy, raw, /*isValTy=*/true);
            } else {
                builder_.CreateCall(popFn,
                    {recv, idx, outAddr, builder_.getInt64(esz)});
            }
            builder_.CreateBr(endBB);
            builder_.SetInsertPoint(endBB);
            // fix #2938 / §22.3: flush any bracket on the normal-return edge.
            emitAccessorRefArgWriteBacks(outArgs, /*emit=*/true);
            auto* phi = builder_.CreatePHI(i1(), 2, "trypopfl.r");
            phi->addIncoming(builder_.getTrue(), foundBB);
            phi->addIncoming(builder_.getFalse(), preBB);
            return phi;
        }
        // .Clear()
        if (m == "Clear" && call.args.empty()) {
            // #602 / #362 / §4.5 / #429 / §4.6 / §28.9 / §28.11 v0.134:
            // OwningArray<T>.Clear disposes every element (ClassOwned row via
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
        // (§28.11 v0.213, #2081: `.DisposeElements()` was retired with axiom A8
        // and is now a sema-time error — no lowering reaches here.)
        // .IsEmpty(): bool
        if (m == "IsEmpty" && call.args.empty()) {
            auto* lenFn = declareC("bz_array_len", i64(), {ptrTy()});
            llvm::Value* len = builder_.CreateCall(lenFn, {recv});
            return builder_.CreateICmpEQ(len, builder_.getInt64(0), "arr.empty");
        }
        // stdlib §4.4 (v0.240) declares on `Array<T>`:
        //     public method AddRange(items: IEnumerable<T>);
        // and stdlib §4.5 declares the sibling owning overload on `OwningArray<T>`:
        //     public method AddRange(owned items: OwningArray<T>);   // drains items into self
        if (m == "AddRange" && call.args.size() == 1 && call.args[0].value) {
            // fix #2721 / stdlib §4.4 (v0.240): `items` is declared
            // `IEnumerable<T>`, NOT `Array<T>` — so the lowered argument is only
            // a `bz_array_t*` when the source happens to be array-backed. For a
            // lazy LINQ pipeline it is a `bz_lazy_t*`, and for an interface-typed
            // source it is an iterator-vtable object; reading either with
            // `bz_array_len`/`bz_array_ptr_at` silently appended the wrong
            // elements (`xs.AddRange(ys.Where(…))` yielded a wrong Count with no
            // diagnostic, no crash and no ASan report — it read a length out of a
            // struct of a different shape).
            //
            // Same remedy as #2693's `new LinkedList<T>(source)`: drain the
            // source once through lowerToLazyEnumerator, the one backing-agnostic
            // normalizer (already used by ToArray/ToSet/ExceptWith), into a
            // snapshot array and run the existing per-element append loop over
            // THAT. It covers array-backed collections, bz_llist, Set/Dictionary,
            // Range, lazy pipelines and vtable `IEnumerable<T>` in one call, and
            // carries the "dispose the source only if it was a temporary" rule.
            //
            // SCOPE. The normalizer is taken only for a BORROWING receiver with a
            // non-array-backed source, which is exactly the set of shapes that is
            // broken today; every shape that already works (any array-backed
            // source, and the whole owning family) keeps the byte-identical raw
            // path below. Two reasons this restriction is deliberate rather than
            // conservatism: (a) §4.5's owning overload takes `owned items:
            // OwningArray<T>` — a concrete array-backed source by declaration, so
            // there is no lazy shape to normalize; and (b) copying borrowed
            // element pointers out of a snapshot into an OWNING array would give
            // two owners for one element (§28.11), so widening the normalizer to
            // the owning receiver would trade a wrong Count for a double dispose.
            //
            // ⚠ `isArrayBackedCollectionName` is NOT usable on its own here.
            // `kArrayBackedCollections` (BuiltinRegistry.cpp) lists the whole
            // SET family — `Set`, `OwningSet`, `SortedSet` — even though
            // lowerNew builds them with `bz_dict_alloc_generic` whenever
            // `genericBackingType` is non-null, i.e. for every record,
            // non-nullable primitive, enum and string element (#2726). Trusting
            // that list pointed `bz_array_len`/`bz_array_ptr_at` at a
            // `bz_dict_t*` — the very defect this fix exists to remove. It hid
            // because `bz_array_t{void* data; int64_t count; …}` and
            // `bz_dict_t{bz_entry_t* buckets; int64_t count; …}` put `count` at
            // the SAME offset, so the Count aliased correctly while the elements
            // did not: `bz_array_ptr_at` indexes the bucket array at the
            // receiver's stride, but `bz_dict_set_gen` stores a malloc'd copy of
            // the key and puts that POINTER in `buckets[i].key`, so the loop
            // read `bz_entry_t` header bytes. Measured: `Set<int>{7,8}` gave
            // Count 2 but `b[0] <> 7`, and `Set<string>` lost the contents.
            //
            // So the fast path is taken only for a name that is provably
            // `bz_array_t*`-backed. The set family is excluded and goes through
            // lowerToLazyEnumerator, which re-decides with the SAME
            // `genericBackingType` discriminator lowerNew constructs with:
            // dict-backed sets take `bz_lazy_from_set`, and a class-element
            // `Set`/`OwningSet` (for which `genericBackingType` is null, and
            // which lowerNew therefore really does build as an array) falls
            // through to the normalizer's own array path. `SortedSet` is
            // excluded for the same honesty but is NOT thereby fixed — the
            // normalizer's set branch tests only `Set`/`OwningSet`, so a
            // `SortedSet` source is still misread there; that is #2726's blast
            // radius (it also governs ToArray/ToSet/ExceptWith/for-in) and is
            // deliberately not widened from here.
            const sema::TypeRef* srcTy = ctx_.exprType(*call.args[0].value);
            std::string_view srcHead =
                (srcTy && srcTy->astNode && srcTy->astNode->name &&
                 !srcTy->astNode->name->parts.empty())
                    ? srcTy->headName() : std::string_view{};
            bool srcIsSetFamily = srcHead == "Set" || srcHead == "OwningSet" ||
                                  srcHead == "SortedSet";
            bool srcIsArrayBacked =
                !srcHead.empty() && !srcIsSetFamily &&
                common::isArrayBackedCollectionName(srcHead);
            llvm::Value* other = nullptr;
            llvm::Value* snapToFree = nullptr;   // non-null ⇒ free the shell below
            if (arrIsOwning || srcIsArrayBacked) {
                other = lowerExpr(*call.args[0].value);
            } else {
                sema::TypeRef* srcElemRef = nullptr;
                llvm::Value* lazy =
                    lowerToLazyEnumerator(*call.args[0].value, srcElemRef);
                if (!lazy) return nullptr;
                // Element-type guard, mirroring OwningSet.ExceptWith's: reading
                // the snapshot at the receiver's stride is only sound when the
                // source's element lowers to the same LLVM type. Sema has no
                // method model for the builtin collections (they have no
                // TypeSymbol), so an ill-typed source reaches codegen unchecked.
                // Only a POSITIVE mismatch is diagnosed — when the source element
                // is unknown the raw path trusted the argument completely, and
                // widening that into a rejection would be a false reject.
                if (srcElemRef && arrElemRef &&
                    lowerTypeRef(srcElemRef) != lowerTypeRef(arrElemRef)) {
                    ctx_.diagnostics().report(
                        Severity::Error, call.span.begin,
                        "stdlib §4.4: 'Array<T>.AddRange(items: IEnumerable<T>)' "
                        "requires an enumerable source whose element type is T");
                    return llvm::ConstantInt::get(i32(), 0);
                }
                auto* toArrFn =
                    declareC("bz_lazy_to_array", ptrTy(), {ptrTy(), i64()});
                other = builder_.CreateCall(
                    toArrFn, {lazy, builder_.getInt64(esz)}, "ar.snap");
                snapToFree = other;
            }
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
            // fix #2721: unchanged on the snapshot path. `bz_lazy_to_array` does
            // NOT retain (the ToArray terminal calls bz_array_retain_strings
            // separately), so a snapshot slot holds exactly the same BORROWED
            // reference the raw source slot did — the same single +1 per appended
            // element, consumed by the receiver's own release-on-removal thunk.
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
            // fix #2721: the snapshot never took ownership of the (borrowed)
            // elements it copied out of the source, and each one it handed to the
            // receiver was retained above — so free its SHELL only, with a NULL
            // disposer. This is the "consumed snapshot" idiom ExceptWith /
            // IntersectWith use for their own bz_lazy_to_array snapshots.
            if (snapToFree) {
                auto* freeSnapFn = declareC("bz_array_free",
                    llvm::Type::getVoidTy(llvm_), {ptrTy(), ptrTy()});
                builder_.CreateCall(freeSnapFn,
                    {snapToFree, llvm::ConstantPointerNull::get(ptrTy())});
            }
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
        // Any record value (hashable or not) is an aggregate and must be
        // boxed — not just the hashable ones recordTypeOfRef accepts.
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
        // OwningDictionary owns its values; borrowing Dictionary borrows/
        // runtime-manages them (string values in the RuntimeManaged row).
        //
        // dictValIsOwning: a dictionary name starting with "Owning" (OwningDictionary).
        // Note: dictValueBoxFreer (record-value boxing path) is not a policy-table
        // concern — record values are value types and land in the Value row of the
        // §28.11 table.  Their heap-boxing is a STORAGE encoding detail (inttoptr
        // cannot hold an aggregate), not an element-slot lifecycle decision.  We
        // keep dictValueBoxFreer calls unchanged (batch 3 / #602 follow-on).
        bool dictValIsOwning = recvTy && recvTy->astNode && recvTy->astNode->name &&
                               !recvTy->astNode->name->parts.empty() &&
                               (common::isOwningPrefixedName(recvTy->headName()));
        const ElementPolicy dictValPol =
            elementPolicyFor(valTy, dictValIsOwning);
        // Convenience predicate: is this a borrowing Dictionary<K, string>?
        //   RuntimeManaged + non-nullable string primitive + NOT owning.
        // Mirrors isStringValueDict (same exclusions: OwningDictionary excluded,
        // nullable string? excluded, function types share RuntimeManaged but
        // use a different release path — same string-vs-function distinction).
        bool dictIsStringValue =
            !dictValIsOwning &&
            dictValPol.cat == ElementPolicy::Category::RuntimeManaged &&
            valTy &&
            valTy->kind == sema::TypeRef::Kind::Primitive &&
            valTy->primitive == sema::PrimitiveType::String &&
            !valTy->nullable;
        // dictValClassDisposer: non-null for OwningDictionary<K, class-or-interface V>.
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
            // fix #685 / §4.4: any struct-typed value (record, nullable-record, and
            // primitive structs such as decimal { i32×4 }) must be heap-boxed —
            // they are too wide for the scalar inttoptr void* slot and ZExt is
            // illegal on aggregate types.  (Records and nullable-value-types are
            // already handled by the valRec/valNullableVal guards above; this arm
            // catches remaining struct types — decimal is the current case.)
            if (v->getType()->isStructTy()) {
                // Box the struct into a malloc'd region (size = LLVM alloc size).
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

        // narrow #3576 / stdlib §5 / §4.8 / §4.17 (v0.286): First()/Last()/
        // ElementAt(i) over a Dictionary/SortedDictionary receiver. Sema
        // types the call as `DictionaryEntry<K,V>` (the shared `entryTy`
        // arm in ExprTypecheck_Calls.cpp's `colName == "Dictionary" ||
        // colName == "SortedDictionary"` block), but codegen had no arm for
        // it: `isArr` is false here (`isDictTypeName` is disjoint from
        // `isCollectionTypeName`) and this `isDict` block previously covered
        // only Add/TryAdd/ContainsKey/Remove/TryGetValue/GetOrNull/GetOrAdd/
        // Clear/Count, so the call fell through to `nullptr` and
        // `lowerLocalDecl`'s #2434 defense-in-depth zero-filled the local
        // SILENTLY — issue #3576 measured `dictFirst=0 dictLast=0
        // dictElemAt1=0` plus the internal "degraded to a zero-initialized
        // value" note (which is not an error, so the program still compiled
        // and ran).
        //
        // NARROWED to a non-string key (any K). A string key hits a
        // disposal dead end unrelated to this arm's own logic — see the
        // `if (!keyTy)` reject a few lines down and #3720 — so this fix
        // covers `Dictionary`/`SortedDictionary`'s scalar/enum/record-keyed
        // shapes fully and rejects the string-keyed shape loudly rather
        // than shipping a leak.
        //
        // Snapshot the key/value tables exactly as the for-in path does
        // (mirrors `lowerDictEntryFor`, LlvmBackend_Stmts.cpp — "the same
        // enumerator the for-in path uses"), applying the §4.17 ascending-
        // key sort for SortedDictionary (fix #3511) so all families agree
        // with `for var e in dict`, then decode ONLY the one index First/
        // Last/ElementAt's own guard selects. This is a 4th "mirrors
        // lowerDictEntryFor" copy (joining `LlvmBackend_Assign.cpp`'s and
        // the Calls3 LINQ materializer's) rather than a call into either of
        // those: both existing copies decode EVERY entry into a fresh
        // owned array for a DIFFERENT consumption shape (a loop variable, a
        // full drained `IEnumerable<DictionaryEntry<K,V>>`), and routing
        // through the LINQ materializer's `bz_lazy_to_array` here would
        // additionally require reconciling `collectionElemSize`/`getElemTy`
        // (which read `recvTy->genericArgs[0]` = K, not the `{K,V}` entry)
        // with the generic array-terminal block above — a materially
        // larger, riskier unification that is out of this lane's scope
        // (LANE_RULES §6b: a family lands whole or not at all). Filed as
        // #3717 for a future dedicated unification of all four copies.
        //
        // §5's ownership note ("results at a class or interface element
        // type are borrows... the caller takes no disposal obligation")
        // governs the Value decode below: it is read straight out of the
        // dict's own storage slot with no retain (a borrow, matching
        // GetOrNull's identical `dv = valPtr` decode a few arms down), so an
        // `OwningDictionary<K,V>` value returned through First/Last/
        // ElementAt is disposed exactly once, at the dictionary's own
        // scope end — never here.
        //
        // SortedDictionary<K,V> reaches this arm too (isDictTypeName is a
        // three-name registry) but stays LOUD-REJECTED end to end, by a
        // DIFFERENT, pre-existing sema mechanism this arm does not touch:
        // ExprTypecheck_Calls.cpp's shared entryTy block types `sd.First()`
        // as `DictionaryEntry<K,V>` exactly like `Dictionary`, but its
        // `dictEntryElemTyped` hand-off deliberately EXCLUDES these four
        // method names so a LATER, SortedDictionary-only catch-all clobbers
        // that type back to the bare `V` — a containment measure put in
        // place specifically because this codegen arm did not exist yet
        // (its own comment cites #3576 by number). Measured on this tree:
        // `sd.First()` still gives "no member `Key` on primitive type
        // `int`" — unchanged by this fix, and correctly so; lifting the
        // exclusion (now that the codegen half exists) is a `src/sema/**`
        // change outside this lane's file ownership.
        //
        // OwningDictionary<K,V> ALSO reaches this arm (same registry) and
        // this arm computes a value for `od.First()` correctly — but sema
        // has NO typing rule for `First`/`Last`/`ElementAt`/`Single` on an
        // OwningDictionary receiver at all (absent from both the entryTy
        // block above and the OwningDictionary-specific method-list block
        // below — verified against ExprTypecheck_Calls.cpp), so
        // `ctx_.exprType()` of the call stays unset regardless of what this
        // arm returns. Measured on this tree: `od.First()` itself lowers
        // (no #2434 note on ITS OWN local any more), but a consuming
        // expression like `fe.Key` still degrades — now via "return-value
        // expression failed to lower to a value ... #2491" instead, since
        // `fe`'s sema type is unresolved either way. Not a regression this
        // fix introduces (the same silent zero-degradation existed before,
        // one level up, at `fe`'s own now-fixed local-decl); still a
        // `src/sema/**` gap this arm cannot close. Both surviving halves
        // (the SortedDictionary exclusion and the missing OwningDictionary
        // typing rule) are filed as #3716.
        if ((call.args.empty() && (m == "First" || m == "Last")) ||
            (m == "ElementAt" && call.args.size() == 1 && call.args[0].value)) {
            // narrow #3576 / #3720: a STRING key has no disposal hook for
            // this consumption shape. `bz_dict_keys_str` hands back raw,
            // non-refcounted bytes; the only way to give the caller a
            // `string`-typed Key is to rebuild a fresh, independently-owned
            // `bz_string*` (as the for-in loop and the LINQ materializer
            // both do) — but BOTH of those thread its release through their
            // OWN consumption shape (a loop's incBB, a wrapped array's own
            // dispose), which only exists because they own that shape. A
            // plain local (`var fe := d.First();`) has no such hook: the
            // general per-local disposal pass does not know
            // `DictionaryEntry<K,V>`'s fields (it is a SYNTHETIC TypeRef,
            // `declaration = nullptr`, built ad hoc in
            // ExprTypecheck_Calls.cpp/ExprTypecheck.cpp — there is no real
            // TypeSymbol with a field list for that pass to walk). Measured
            // (200x under ASan/LSan, `Dictionary<string,int>.First()`
            // bound to a plain local): the VALUE is correct
            // (`total=4400`) but every call leaks the rebuilt Key (6400 B
            // header + 600 B data over 200 iterations = 7000 B in 400
            // allocations) — shipping it would be the loud->quiet trade
            // §6c forbids, just moved from "wrong value" to "leak" instead
            // of "silent zero". Reject loudly instead; #3720 is the
            // sema-side fix (give `DictionaryEntry`/`DrainedEntry` a real
            // field list the general disposal pass can walk), outside
            // `LlvmBackend_Calls*.cpp`'s file ownership. Once #3720 lands,
            // this whole `if (!keyTy)` arm is deleted and the string-key
            // rebuild (identical to `lowerDictEntryFor`'s) slots in beside
            // the generic-key decode below — the value decode, the
            // SortedDictionary sort, and the free-loop are already
            // key-type-agnostic.
            if (!keyTy) {
                ctx_.diagnostics().report(
                    Severity::Error, call.span.begin,
                    "`." + std::string(m) + "` over a string-keyed "
                    "Dictionary/SortedDictionary is not yet supported "
                    "(stdlib \xC2\xA7""5 / \xC2\xA7""4.8, #3720): the "
                    "returned entry's Key would be a freshly-built string "
                    "with no disposal hook at the returned local's scope "
                    "end -- every existing consumer of this shape (the "
                    "for-in loop, the \xC2\xA7""5 LINQ operators, "
                    "ToArray()) threads its own release through ITS OWN "
                    "consumption shape, which a plain local has none of; "
                    "shipping it would leak on every call. Use "
                    "`for var e in dict` (which disposes it) or "
                    "`.Keys`/`.Values` instead.");
                llvm::Type* resTy = i32();
                if (sema::TypeRef* rt = ctx_.exprType(call))
                    if (llvm::Type* lt = lowerTypeRef(rt))
                        resTy = lt;
                return llvm::PoisonValue::get(resTy);
            }

            llvm::Type* dtKeyLlvm = lowerTypeRef(keyTy);
            if (!dtKeyLlvm) dtKeyLlvm = ptrTy();
            llvm::Type* dtValLlvm = valTy ? lowerTypeRef(valTy) : ptrTy();
            if (!dtValLlvm) dtValLlvm = ptrTy();
            llvm::StructType* dtEntryStTy =
                llvm::StructType::get(llvm_, {dtKeyLlvm, dtValLlvm});

            // fix (review c6-b12-review, NEEDS-FIX finding 1): read the
            // count via bz_dict_count -- no allocation -- and run First/
            // Last/ElementAt's own empty/out-of-range guard on THAT count,
            // BEFORE any snapshot call. bz_dict_keys_gen/bz_dict_values_gen
            // malloc their spine arrays unconditionally (even for an
            // n=0 dict), and emitGuardThrow's throw path DIVERGES -- it
            // never reaches the dtFreeFn calls below -- so snapshotting
            // before the guard leaked both spine arrays on every throw.
            // Measured 200x under ASan/LSan pre-fix: `new
            // Dictionary<int,int>().First()` guarded in a try/catch leaked
            // 3200 B in 400 allocations (8 B keys-array + 8 B values-array
            // per throw). Deferring the snapshot to AFTER the guard passes
            // gives this arm the SAME "no allocation before reject"
            // discipline the string-key reject above already has.
            llvm::Value* dtCnt0;
            {
                auto* cfn = declareC("bz_dict_count", i64(), {ptrTy()});
                dtCnt0 = builder_.CreateCall(cfn, {recv});
            }

            // Compute/guard the target index (stdlib §5 line 3594: First/
            // Last throw InvalidOperationException on empty; ElementAt
            // throws IndexOutOfRangeException out of bounds) -- before any
            // snapshot allocation, per the note above.
            llvm::Value* dtIdx;
            if (m == "First") {
                emitGuardThrow(
                    builder_.CreateICmpEQ(dtCnt0, builder_.getInt64(0)),
                    "InvalidOperationException");
                dtIdx = builder_.getInt64(0);
            } else if (m == "Last") {
                emitGuardThrow(
                    builder_.CreateICmpEQ(dtCnt0, builder_.getInt64(0)),
                    "InvalidOperationException");
                dtIdx = builder_.CreateSub(dtCnt0, builder_.getInt64(1));
            } else {
                llvm::Value* rawIdx = lowerExpr(*call.args[0].value);
                if (!rawIdx) return nullptr;
                dtIdx = builder_.CreateSExt(rawIdx, i64());
                llvm::Value* neg =
                    builder_.CreateICmpSLT(dtIdx, builder_.getInt64(0));
                llvm::Value* oor = builder_.CreateICmpSGE(dtIdx, dtCnt0);
                emitGuardThrow(builder_.CreateOr(neg, oor),
                               "IndexOutOfRangeException");
            }

            // Guard passed (dtCnt0 > 0 for First/Last; dtIdx in
            // [0, dtCnt0) for ElementAt) -- safe to allocate the snapshot.
            auto* dtFreeFn = declareC("free", llvm::Type::getVoidTy(llvm_), {ptrTy()});
            auto* dtKeysOutSlot = entryAlloca(ptrTy());
            auto* dtCntSlot     = entryAlloca(i64());
            auto* dtValsOutSlot = entryAlloca(ptrTy());
            auto* kfn = declareC("bz_dict_keys_gen",
                llvm::Type::getVoidTy(llvm_), {ptrTy(), ptrTy(), ptrTy()});
            builder_.CreateCall(kfn, {recv, dtKeysOutSlot, dtCntSlot});
            auto* vfn = declareC("bz_dict_values_gen",
                llvm::Type::getVoidTy(llvm_), {ptrTy(), ptrTy(), ptrTy()});
            builder_.CreateCall(vfn, {recv, dtValsOutSlot, entryAlloca(i64())});
            llvm::Value* dtKeysArr = builder_.CreateLoad(ptrTy(), dtKeysOutSlot);
            llvm::Value* dtValsArr = builder_.CreateLoad(ptrTy(), dtValsOutSlot);
            llvm::Value* dtCnt     = builder_.CreateLoad(i64(), dtCntSlot);

            // fix #3511 / stdlib §4.17: ascending key order for
            // SortedDictionary (mirrors #3511's shape for the for-in path's
            // identical step). A key type `buildSortedKeyComparator` cannot
            // rank (returns null) leaves the snapshot unsorted, matching
            // what the for-in path already does for that same key type
            // (#1426).
            if (builtinTypeName(recvTy) == "SortedDictionary") {
                if (llvm::Function* cmpFn = buildSortedKeyComparator(keyTy)) {
                    auto* sortFn = declareC("bz_sort_kv_gen",
                        llvm::Type::getVoidTy(llvm_),
                        {ptrTy(), ptrTy(), i64(), ptrTy()});
                    builder_.CreateCall(sortFn,
                        {dtKeysArr, dtValsArr, dtCnt, cmpFn});
                }
            }

            // Decode the target key (narrow #3717: shared with
            // lowerDictEntryFor and wrapDictionaryAsIEnumerable — see
            // LlvmBackend_impl.hpp): a plain value LOAD out of the malloc'd
            // raw-bytes copy `bz_dict_keys_gen` wrote at the ONE computed
            // index rather than a loop var — no separate heap ownership
            // survives the load, so freeing the blob below (for every index,
            // including this one) is always safe.
            llvm::Value* dtKeyDecoded =
                decodeDictGenericKeyAt(dtKeyLlvm, dtKeysArr, dtIdx,
                                       "dictterm.kgen").first;

            // Decode the target value. NOTE (#3717 §6b): this arm's
            // `loadAsStruct` predicate is `dtValLlvm->isStructTy()` alone —
            // broader than lowerDictEntryFor's / wrapDictionaryAsIEnumerable's
            // `valRec || (valNullableVal && isStructTy())` (see
            // decodeDictValueAt's declaration for why the two are preserved
            // separately rather than merged: they are not proven equivalent
            // for a struct-lowering V that is neither a record nor a nullable
            // value type). Identical decode to GetOrNull's stored-slot unbox
            // a few arms down otherwise: struct -> load the box, pointer ->
            // the borrow itself, scalar -> ptrtoint + trunc/bitcast.
            llvm::Value* dtValDecoded = decodeDictValueAt(
                dtValLlvm, dtValsArr, dtIdx,
                /*loadAsStruct=*/dtValLlvm->isStructTy(), "dictterm.vrec");

            // Free every per-key blob (bz_dict_keys_gen mallocs one per
            // entry regardless of which index is chosen — mirrors
            // lowerDictEntryFor's incBB free, run here in one pass since
            // there is no per-iteration loop body). The target key was
            // already loaded BY VALUE above, so freeing its blob too is
            // safe — no aliasing survives the decode.
            {
                auto* fiSlot = entryAlloca(i64());
                builder_.CreateStore(builder_.getInt64(0), fiSlot);
                auto* fCond = llvm::BasicBlock::Create(
                    llvm_, "dictterm.fcond", currentFn_);
                auto* fBody = llvm::BasicBlock::Create(
                    llvm_, "dictterm.fbody", currentFn_);
                auto* fEnd  = llvm::BasicBlock::Create(
                    llvm_, "dictterm.fend", currentFn_);
                builder_.CreateBr(fCond);
                builder_.SetInsertPoint(fCond);
                llvm::Value* fi = builder_.CreateLoad(i64(), fiSlot);
                builder_.CreateCondBr(
                    builder_.CreateICmpSLT(fi, dtCnt), fBody, fEnd);
                builder_.SetInsertPoint(fBody);
                llvm::Value* fp = builder_.CreateGEP(ptrTy(), dtKeysArr, fi);
                llvm::Value* fkp = builder_.CreateLoad(ptrTy(), fp);
                builder_.CreateCall(dtFreeFn, {fkp});
                builder_.CreateStore(
                    builder_.CreateAdd(fi, builder_.getInt64(1)), fiSlot);
                builder_.CreateBr(fCond);
                builder_.SetInsertPoint(fEnd);
            }
            builder_.CreateCall(dtFreeFn, {dtKeysArr});
            builder_.CreateCall(dtFreeFn, {dtValsArr});

            // #1013-style reclaim: a fresh unbound Dictionary/SortedDictionary
            // materializer receiver (e.g. `arr.ToDictionary(sel).First()`) —
            // a no-op for the common named-local receiver. Mirrors the
            // `.Count()` arm's identical call, placed last since it is the
            // final use of `recv`.
            freeUnboundOwnedBorrowingArrayTemp(ma->target.get(), recv, recvTy);

            llvm::Value* dtEntryVal = llvm::UndefValue::get(dtEntryStTy);
            dtEntryVal = builder_.CreateInsertValue(dtEntryVal, dtKeyDecoded, {0u});
            dtEntryVal = builder_.CreateInsertValue(dtEntryVal, dtValDecoded, {1u});
            return dtEntryVal;
        }

        // .Count: int
        if (m == "Count" && call.args.empty()) {
            auto* fn = declareC("bz_dict_count", i64(), {ptrTy()});
            llvm::Value* c = builder_.CreateCall(fn, {recv});
            llvm::Value* cnt = builder_.CreateTrunc(c, i32());
            // #1013 / §28.3: reclaim an UNBOUND `a.ToDictionary(...)` temporary at
            // end-of-statement — the count is read from the dict above FIRST, then
            // the dict-backed temp is freed via bz_dict_free (inside the helper). A
            // no-op for a bound/aliased/`move`d receiver (not a ToDictionary
            // CallExpr) → that dict is freed by its own using-var disposal; no
            // double-free, no UAF.
            freeUnboundOwnedBorrowingArrayTemp(ma->target.get(), recv, recvTy);
            return cnt;
        }
        // §4.8 / §4.9: .Clear() — empty the table in place (it stays allocated;
        // Count is 0 afterward). The value-disposal obligation mirrors the
        // scope-end Dispose exactly (bz_dict_free in LlvmBackend_Setup.cpp):
        //   * OwningDictionary<K, class>  → dispose+free every surviving value via
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
            // dictValClassDisposer: non-null for OwningDictionary<K,class> (ClassOwned).
            // dictValueBoxFreer: kept unchanged (value-type record box; not a policy
            //   table decision — batch 3 / #602 follow-on).
            // dictIsStringValue: borrowing Dictionary<K,string> → release each +1.
            // per §28.11 v0.135 fix #619: dictValClassDisposer is now non-null for
            // interface-valued OwningDictionary (class-or-interface element).
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
        // fix #3372 / stdlib §4.8 + §4.9 (v0.282, §1 Try shape (d)):
        //   Dictionary<K, V>.TryAdd(key: K, value: V): bool
        //   OwningDictionary<K, V>.TryAdd(key: K, owned value: V): bool
        // The no-throw sibling of `.Add(k, v)` directly below: probe the key;
        // absent -> insert and return true, present -> leave the table untouched
        // and return false. For the OWNING form the passed value was consumed at
        // the call site, so the false path DISPOSES it — the `OwningSet<T>.TryAdd`
        // contract stdlib §4.9 cites verbatim (§4.10; §28.3 discharge rule, the
        // same reasoning `Add`'s duplicate-key throw path below applies).
        if (m == "TryAdd" && call.args.size() == 2 &&
            call.args[0].value && call.args[1].value) {
            llvm::Value* k = lowerExpr(*call.args[0].value);
            llvm::Value* v = lowerExpr(*call.args[1].value);
            if (!k || !v) return nullptr;
            llvm::Value* isDup;
            if (keyTy) {
                auto* cf = declareC("bz_dict_contains_gen", i1(),
                                    {ptrTy(), ptrTy()});
                isDup = builder_.CreateCall(cf, {recv, genKeyPtr(k)}, "tryadd.dup");
            } else {
                auto [kd0, kl0] = strDataLen(k);
                auto* cf = declareC("bz_dict_contains_str", i1(),
                                    {ptrTy(), ptrTy(), i64()});
                isDup = builder_.CreateCall(cf, {recv, kd0, kl0}, "tryadd.dup");
            }
            auto* dupBB = llvm::BasicBlock::Create(llvm_, "tryadd.dup.disp",
                                                   currentFn_);
            auto* insBB = llvm::BasicBlock::Create(llvm_, "tryadd.ins",
                                                   currentFn_);
            auto* endBB = llvm::BasicBlock::Create(llvm_, "tryadd.end",
                                                   currentFn_);
            builder_.CreateCondBr(isDup, dupBB, insBB);
            // dupBB: the key is present. §4.9 (v0.282) — the owning form's value
            // parameter is `owned`, so ownership arrived here and must be
            // discharged (§28.3) rather than leaked; the borrowing §4.8 form
            // leaves the argument alone.
            builder_.SetInsertPoint(dupBB);
            if (dictValIsOwning && dictValClassDisposer)
                builder_.CreateCall(dictValClassDisposer, {v});
            llvm::BasicBlock* dupEnd = builder_.GetInsertBlock();
            builder_.CreateBr(endBB);
            // insBB: absent — the insert path is `Add`'s, verbatim (#429 / §28.9
            // string-value +1 retain, then the key-shaped store).
            builder_.SetInsertPoint(insBB);
            if (dictIsStringValue &&
                !isPlusOneStringProducer(call.args[1].value.get()))
                emitStringRetain(v);
            llvm::Value* vPtr = encodeValue(v);
            if (keyTy) {
                auto* fn = declareC("bz_dict_set_gen",
                                    llvm::Type::getVoidTy(llvm_),
                                    {ptrTy(), ptrTy(), ptrTy()});
                builder_.CreateCall(fn, {recv, genKeyPtr(k), vPtr});
            } else {
                auto [kd, kl] = strDataLen(k);
                auto* fn = declareC("bz_dict_set_str",
                                    llvm::Type::getVoidTy(llvm_),
                                    {ptrTy(), ptrTy(), i64(), ptrTy()});
                builder_.CreateCall(fn, {recv, kd, kl, vPtr});
            }
            llvm::BasicBlock* insEnd = builder_.GetInsertBlock();
            builder_.CreateBr(endBB);
            builder_.SetInsertPoint(endBB);
            // The PHI is created BEFORE any other instruction in endBB — a PHI
            // must be the first thing in its block — and the incoming blocks are
            // the ACTUAL predecessors (`GetInsertBlock` after each arm), not the
            // blocks the arms started in, so the wiring survives a helper that
            // splits its arm.
            auto* phi = builder_.CreatePHI(i1(), 2, "tryadd.r");
            phi->addIncoming(builder_.getFalse(), dupEnd);
            phi->addIncoming(builder_.getTrue(), insEnd);
            // fix #1970 / §28.4 / §28.6 (the `Add` handler below carries the full
            // reasoning): the owning form consumed the value on BOTH edges — the
            // insert transferred it into the table, the duplicate edge disposed it
            // — so the caller's bare owned local/param slot is nulled once here,
            // after the join, keeping the caller's null-guarded unwindOnly
            // disposal exactly-once.
            if (dictValIsOwning)
                nullTransferredOwnedLocalSlot(call.args[1].value.get());
            return phi;
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
            // §4.8 / #1201 / #1214: Dictionary.Add and OwningDictionary.Add must
            // throw ArgumentException when the key is already present (the indexer
            // setter `d[k] := v` is the add-or-update / overwrite path and must
            // not throw).  .Set is sema-rejected per §4.8 v0.148; the m=="Set"
            // branch is dead for borrowing dicts. The probe fires before the
            // string-value retain so no retain leak occurs on the throw path.
            if (m == "Add" && !dictValIsOwning) {
                // §4.8 / #1201: borrowing Dictionary — plain emitGuardThrow
                // suffices (no owned value to discharge on the throw path).
                if (keyTy) {
                    auto* cf = declareC("bz_dict_contains_gen",
                                        i1(), {ptrTy(), ptrTy()});
                    emitGuardThrow(
                        builder_.CreateCall(cf, {recv, genKeyPtr(k)}, "add.dup"),
                        "ArgumentException");
                } else {
                    auto [kd0, kl0] = strDataLen(k);
                    auto* cf = declareC("bz_dict_contains_str",
                                        i1(), {ptrTy(), ptrTy(), i64()});
                    emitGuardThrow(
                        builder_.CreateCall(cf, {recv, kd0, kl0}, "add.dup"),
                        "ArgumentException");
                }
            }
            // fix #1214 / §4.8 / §28.3: OwningDictionary.Add on a duplicate key
            // must throw ArgumentException AND dispose the transferred owned value
            // (§28.3 discharge rule) before throwing — the value won't be inserted
            // so ownership must be consumed here or it leaks.  Cannot use
            // emitGuardThrow (it does not run value cleanup before throw); build
            // the branch manually: probe → dup → dispose transferred v → throw.
            if (m == "Add" && dictValIsOwning) {
                llvm::Value* isDup;
                if (keyTy) {
                    auto* cf = declareC("bz_dict_contains_gen",
                                        i1(), {ptrTy(), ptrTy()});
                    isDup = builder_.CreateCall(cf, {recv, genKeyPtr(k)},
                                                "add.dup");
                } else {
                    auto [kd0, kl0] = strDataLen(k);
                    auto* cf = declareC("bz_dict_contains_str",
                                        i1(), {ptrTy(), ptrTy(), i64()});
                    isDup = builder_.CreateCall(cf, {recv, kd0, kl0}, "add.dup");
                }
                auto* dupBB  = llvm::BasicBlock::Create(
                    llvm_, "add.dup.disp", currentFn_);
                auto* contBB = llvm::BasicBlock::Create(
                    llvm_, "add.cont", currentFn_);
                builder_.CreateCondBr(isDup, dupBB, contBB);
                builder_.SetInsertPoint(dupBB);
                // §28.3: discharge the transferred owned value — ownership must
                // be consumed here to prevent a leak (the value is not inserted).
                if (dictValClassDisposer)
                    builder_.CreateCall(dictValClassDisposer, {v});
                emitThrowBuiltin("ArgumentException");
                builder_.SetInsertPoint(contBB);
            }
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
                // fix #1970 / §28.4 / §28.6: for OwningDictionary<K,V>.Add the
                // value parameter is `owned` and has just been stored into the
                // dict's table (the duplicate-key branch above already disposed
                // `v` and threw — argument-shape skipped, slot still live; the
                // common path here transferred ownership). Null the caller's
                // slot for a bare owned local/param `owned v` so the caller's
                // null-guarded unwindOnly disposal — which
                // UnwindIneligibleWalker now leaves eligible for this callee
                // shape (calleeIsOwningCollectionTransfer) — is exactly-once:
                // disposes a value still live at a throw that PRECEDES the Add
                // (fixes the #1970 leak); skips the just-transferred value at
                // the post-transfer throw edge (no double-free with the
                // dict's table disposal on Clear/Drain). Mirror the OwningArray
                // / OwningSet Add nulling paths.
                if (dictValIsOwning)
                    nullTransferredOwnedLocalSlot(call.args[1].value.get());
                return llvm::ConstantInt::get(i32(), 0);
            }
            auto [kd, kl] = strDataLen(k);
            auto* fn = declareC("bz_dict_set_str", llvm::Type::getVoidTy(llvm_),
                                {ptrTy(), ptrTy(), i64(), ptrTy()});
            builder_.CreateCall(fn, {recv, kd, kl, vPtr});
            // fix #1970 / §28.4 / §28.6: see the gen-key branch above — for
            // OwningDictionary<K,V> with a string key, the value (arg 1) has
            // just been transferred. Null the caller's bare owned local/param
            // slot; the UnwindIneligibleWalker-eligible unwindOnly disposal
            // stays exactly-once (throw-before disposes, throw-after skips).
            if (dictValIsOwning)
                nullTransferredOwnedLocalSlot(call.args[1].value.get());
            return llvm::ConstantInt::get(i32(), 0);
        }
        // .Remove(k): bool — per §4.9: OwningDictionary.Remove disposes the
        // removed value (exactly once) before unlinking; a non-owning
        // Dictionary.Remove unlinks without disposal (§4.8, borrows).
        // #429 / §28.9: a borrowing Dictionary<K, string> co-owns a +1 per
        // stored value — Remove must release the removed value's +1 (else it
        // leaks). Use the disposing-remove variant with stringElementReleaseThunk
        // as the value-disposer, exactly mirroring the OwningDictionary path.
        if (m == "Remove" && call.args.size() == 1 && call.args[0].value) {
            llvm::Value* k = lowerExpr(*call.args[0].value);
            if (!k) return nullptr;
            // For an OwningDictionary<K, class> get the value disposer thunk
            // (same helper Clear uses, so the dispose logic is exactly-once
            // and consistent). For borrowing Dictionary<K, string>, get the
            // string-release thunk. For borrowing Dict or non-class/non-string
            // values, valDisp is null → fall through to plain non-disposing remove.
            // #602 / §28.11 v0.135 fix #619: derive disposer from dictValPol (hoisted
            // above).  dictValClassDisposer: OwningDictionary<K,class-or-interface>
            // (ClassOwned row per §28.11 v0.135).
            // dictIsStringValue: borrowing Dictionary<K,string> (RuntimeManaged row).
            llvm::Function* valDisp = dictValClassDisposer;
            if (!valDisp && dictIsStringValue)
                valDisp = stringElementReleaseThunk();
            if (keyTy) {
                if (valDisp) {
                    // OwningDictionary (or Dict<K,string>) with generic key.
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
                // OwningDictionary (or Dict<string,string>) with string key.
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
        // Only meaningful on OwningDictionary<K,V>; the borrowing Dictionary does
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
            // fix #685: any struct-typed value (record, nullable, decimal, etc.) is
            // heap-boxed by encodeValue; load the struct bytes back from the box.
            llvm::Value* valPtr = builder_.CreateLoad(ptrTy(), tmpSlot, "pv.vptr");
            if (valLlvm->isStructTy()) {
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
        // .TryPopValue(k, out owned v): bool — stdlib §4.9 OwningDictionary.
        // The TRY analogue of PopValue: get-then-remove WITHOUT disposing the
        // value (ownership transfers to the out param), but return false and
        // leave the out param unchanged when the key is absent (instead of
        // throwing). Combines PopValue's pop runtime call (bz_dict_pop_value_*)
        // with TryGetValue's out-param decode. Before this fix TryPopValue fell
        // through to no handler → bz_dict_try_get was never emitted and the
        // result was a wrong/always-false bool (#831, bug15).
        if (m == "TryPopValue" && call.args.size() == 2 &&
            call.args[0].value && call.args[1].value) {
            llvm::Value* k = lowerExpr(*call.args[0].value);
            if (!k) return nullptr;
            // fix #2938 / §22.3: see TryGetValue above.
            llvm::Type* outTy = nullptr;
            std::vector<AccessorRefArg> outArgs;
            llvm::Value* outAddr = resolveOutAddrWithBracket(
                *call.args[1].value, outTy, outArgs);
            if (!outAddr) {
                emitAccessorRefArgWriteBacks(outArgs, /*emit=*/false);
                return nullptr;
            }
            auto* tmpSlot = entryAlloca(ptrTy());
            // §28.11, #891: pre-null tmpSlot so that on the key-not-found
            // (false) path — where bz_dict_pop_value_* does NOT write *out —
            // the subsequent load yields null rather than uninitialized garbage.
            // Without this, a nullable class out-param receives a garbage
            // pointer and v?.Dispose() dereferences it → SIGSEGV.
            builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()),
                                  tmpSlot);
            llvm::Value* found;
            if (keyTy) {
                auto* fn = declareC("bz_dict_pop_value_gen", i1(),
                                    {ptrTy(), ptrTy(), ptrTy()});
                found = builder_.CreateCall(fn, {recv, genKeyPtr(k), tmpSlot},
                                            "tpv.found");
            } else {
                auto [kd2, kl2] = strDataLen(k);
                auto* fn = declareC("bz_dict_pop_value_str", i1(),
                                    {ptrTy(), ptrTy(), i64(), ptrTy()});
                found = builder_.CreateCall(fn, {recv, kd2, kl2, tmpSlot},
                                            "tpv.found");
            }
            llvm::Value* valPtr = builder_.CreateLoad(ptrTy(), tmpSlot, "tpv.vptr");
            // Decode the stored void* into *outAddr — mirror TryGetValue: a
            // struct (record / nullable / decimal) is heap-boxed, copy it back
            // under a null-guard (null on not-found); a scalar is ptrtoint-
            // decoded; a pointer (class/string) is stored directly.
            if (outTy && outTy->isStructTy()) {
                auto* copyBB = llvm::BasicBlock::Create(llvm_, "tpv.copy", currentFn_);
                auto* endBB  = llvm::BasicBlock::Create(llvm_, "tpv.end", currentFn_);
                llvm::Value* nonNull = builder_.CreateICmpNE(
                    valPtr, llvm::ConstantPointerNull::get(ptrTy()), "tpv.nn");
                builder_.CreateCondBr(nonNull, copyBB, endBB);
                builder_.SetInsertPoint(copyBB);
                llvm::Value* recVal = builder_.CreateLoad(outTy, valPtr, "tpv.rec");
                builder_.CreateStore(recVal, outAddr);
                builder_.CreateBr(endBB);
                builder_.SetInsertPoint(endBB);
            } else if (outTy && !outTy->isPointerTy()) {
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
            // fix #2938 / §22.3: flush any bracket on the normal-return edge.
            emitAccessorRefArgWriteBacks(outArgs, /*emit=*/true);
            return found;
        }
        // .TryGetValue(k, out v): bool
        // (Note: the out-var slot is pre-created by ensureOutVarSlots)
        if (m == "TryGetValue" && call.args.size() == 2 &&
            call.args[0].value &&
            (call.args[1].value || call.args[1].isDiscard)) {
            llvm::Value* k = lowerExpr(*call.args[0].value);
            if (!k) return nullptr;
            // Get the address of the out parameter.
            // fix #2938 / §22.3: a property or indexer slot target is admitted
            // by §22.3 and resolves through the §22.3 copy-in/copy-out bracket;
            // `lvalueAddr` alone declines, so the builtin call bailed and the
            // write-back silently dropped. `outArgs` carries the bracket record
            // for the normal-return write-back flush at the merge block below.
            llvm::Type* outTy = nullptr;
            llvm::Value* outAddr;
            std::vector<AccessorRefArg> outArgs;
            if (call.args[1].isDiscard) {
                // §22.4 (#1301): `dict.TryGetValue(key, out _)` — the discard has
                // no destination lvalue (the spec's canonical form: the receiving
                // slot "is internal to the compiler and is not observable").
                // Allocate a throwaway scratch slot of the value type V so the
                // decode below has somewhere to write; the value is never read.
                outTy = valTy ? lowerTypeRef(valTy) : ptrTy();
                if (!outTy) outTy = ptrTy();
                outAddr = entryAlloca(outTy);
            } else {
                outAddr = resolveOutAddrWithBracket(
                    *call.args[1].value, outTy, outArgs);
                if (!outAddr) {
                    emitAccessorRefArgWriteBacks(outArgs, /*emit=*/false);
                    return nullptr;
                }
            }
            // bz_dict_try_get_* writes the stored void* into *tmpSlot.
            // For pointer values: the void* IS the string/object pointer.
            // For integer/float values: the void* encodes the value via
            // inttoptr (mirroring the Add encoding above). Decode via ptrtoint.
            // issue D4: for a record value the void* is the heap box; copy the
            // record bytes from the box into the out slot.
            auto* tmpSlot = entryAlloca(ptrTy());
            // §28.11, #891: pre-null tmpSlot so that on the key-not-found
            // (false) path — where bz_dict_try_get_* does NOT write *out —
            // the subsequent load yields null rather than uninitialized garbage.
            builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()),
                                  tmpSlot);
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
            // v0.171 (#1336): the out param is `V?`. Assign null on the miss
            // (false) path; on the hit path decode the stored value as V and
            // coerce it into the recipient's (possibly tagged `{i8,V}`) slot.
            // Keying the decode on V's STORAGE — not on the recipient LLVM type
            // — is essential: a value V (int) is inttoptr-encoded, so a
            // now-nullable `{i8,i32}` recipient must NOT be read as a boxed
            // struct. coerceToNullableSlot wraps a bare V into `{i8 1, V}` when
            // the recipient is the tagged form, and passes a pointer / an
            // already-tagged value through unchanged.
            // fix #685: a record / decimal / nullable-value V is heap-boxed by
            // encodeValue; load it back from the box on the hit path.
            llvm::Type* valLlvm = valTy ? lowerTypeRef(valTy) : ptrTy();
            if (!valLlvm) valLlvm = ptrTy();
            storeTryOutNull(outAddr, outTy);  // false-path null (§22.3)
            auto* tgSetBB = llvm::BasicBlock::Create(llvm_, "tg.set", currentFn_);
            auto* tgEndBB = llvm::BasicBlock::Create(llvm_, "tg.end", currentFn_);
            builder_.CreateCondBr(found, tgSetBB, tgEndBB);
            builder_.SetInsertPoint(tgSetBB);
            llvm::Value* dv;
            if (valLlvm->isStructTy()) {
                dv = builder_.CreateLoad(valLlvm, valPtr, "tg.rec");
            } else if (!valLlvm->isPointerTy()) {
                llvm::Value* asI64 = builder_.CreatePtrToInt(valPtr, i64());
                dv = asI64;
                if (valLlvm->isFloatingPointTy()) {
                    if (valLlvm->isDoubleTy())
                        dv = builder_.CreateBitCast(asI64, valLlvm);
                    else
                        dv = builder_.CreateBitCast(
                            builder_.CreateTrunc(asI64, i32()), valLlvm);
                } else if (!valLlvm->isIntegerTy(64)) {
                    dv = builder_.CreateTrunc(asI64, valLlvm);
                }
            } else {
                dv = valPtr;  // pointer value (string / class)
            }
            builder_.CreateStore(coerceToNullableSlot(outTy, dv), outAddr);
            builder_.CreateBr(tgEndBB);
            builder_.SetInsertPoint(tgEndBB);
            // fix #2938 / §22.3: flush any bracket on the normal-return edge.
            // The TryGetValue path never throws, so emit is always true.
            emitAccessorRefArgWriteBacks(outArgs, /*emit=*/true);
            return found;
        }
        // fix #3510 / stdlib §4.8 / §4.9 / §4.17 (v0.285; the §1 *OrNull tier,
        // v0.231): `.GetOrNull(k): V?` — `null` when the key is absent, otherwise
        // the stored value under the family's read semantics (a copy for a
        // value-typed V and `string`, a BORROW of the stored value for a
        // class-typed V; ownership stays with the dictionary, so no disposal
        // obligation attaches — the same posture as `GetOrAdd` below and as the
        // indexer's get).
        //
        // Receiver coverage: this arm sits inside the `isDict` block, whose
        // predicate is the #1414 stage-3 registry's `common::isDictionaryName`
        // (Dictionary / OwningDictionary / SortedDictionary all share the
        // bz_dict_t* representation), so one lowering serves all three families
        // that declare the member. The sorted family is unaffected by ordering
        // here: `GetOrNull` is a POINT lookup, which §4.17 specifies as O(log n)
        // on the same table the ordered iteration snapshots separately.
        //
        // Lowering = TryGetValue's probe + decode, with the out-parameter write
        // replaced by a `V?` value: the tagged `{i8 hasValue, V}` struct for a
        // value-typed V, and a nullable pointer for a `string`/class V. When V is
        // ITSELF nullable the result type coincides with V's own lowering
        // (`?` is idempotent under substitution — spec §7.10; §4.8's own caveat
        // that the result cannot then distinguish "absent" from "a present null"),
        // so the decoded value IS the result and the miss path is its zero.
        if (m == "GetOrNull" && call.args.size() == 1 && call.args[0].value) {
            llvm::Value* k = lowerExpr(*call.args[0].value);
            if (!k) return nullptr;
            llvm::Type* valLlvm = valTy ? lowerTypeRef(valTy) : ptrTy();
            if (!valLlvm) valLlvm = ptrTy();
            // §28.11, #891: pre-null the slot so the miss path — where
            // bz_dict_try_get_* does NOT write — reads null, not stack garbage.
            auto* tmpSlot = entryAlloca(ptrTy());
            builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()),
                                 tmpSlot);
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
            auto* hitBB = llvm::BasicBlock::Create(llvm_, "gon.hit", currentFn_);
            auto* endBB = llvm::BasicBlock::Create(llvm_, "gon.end", currentFn_);
            llvm::BasicBlock* preBB = builder_.GetInsertBlock();
            builder_.CreateCondBr(found, hitBB, endBB);

            builder_.SetInsertPoint(hitBB);
            llvm::Value* valPtr = builder_.CreateLoad(ptrTy(), tmpSlot, "gon.slot");
            llvm::Value* dv;
            if (valLlvm->isStructTy()) {
                // fix #685: a record / decimal / nullable-value V is heap-boxed
                // by encodeValue; load it back out of the box (TryGetValue's
                // decode, verbatim).
                dv = builder_.CreateLoad(valLlvm, valPtr, "gon.rec");
            } else if (!valLlvm->isPointerTy()) {
                llvm::Value* asI64 = builder_.CreatePtrToInt(valPtr, i64());
                dv = asI64;
                if (valLlvm->isFloatingPointTy()) {
                    if (valLlvm->isDoubleTy())
                        dv = builder_.CreateBitCast(asI64, valLlvm);
                    else
                        dv = builder_.CreateBitCast(
                            builder_.CreateTrunc(asI64, i32()), valLlvm);
                } else if (!valLlvm->isIntegerTy(64)) {
                    dv = builder_.CreateTrunc(asI64, valLlvm);
                }
            } else {
                dv = valPtr;  // pointer value (string / class) — a borrow
            }
            llvm::BasicBlock* hitEnd = builder_.GetInsertBlock();
            builder_.CreateBr(endBB);

            builder_.SetInsertPoint(endBB);
            sema::TypeRef nullRT;
            bool haveNullRT = false;
            if (valTy) { nullRT = *valTy; nullRT.nullable = true; haveNullRT = true; }
            llvm::StructType* nst =
                (haveNullRT && isNullableValueTy(&nullRT))
                    ? nullableValueStructTy(&nullRT) : nullptr;
            if (nst && nst != valLlvm) {
                // Value-typed V: build `{i8 hasValue, V}`.
                auto* valPhi = builder_.CreatePHI(valLlvm, 2, "gon.val");
                valPhi->addIncoming(dv, hitEnd);
                valPhi->addIncoming(llvm::Constant::getNullValue(valLlvm), preBB);
                auto* hvPhi =
                    builder_.CreatePHI(nullablePrimHasValTy(), 2, "gon.hv");
                hvPhi->addIncoming(
                    llvm::ConstantInt::get(nullablePrimHasValTy(), 1), hitEnd);
                hvPhi->addIncoming(
                    llvm::ConstantInt::get(nullablePrimHasValTy(), 0), preBB);
                llvm::Value* res = builder_.CreateInsertValue(
                    llvm::UndefValue::get(nst), hvPhi, {0u});
                return builder_.CreateInsertValue(res, valPhi, {1u});
            }
            // A `string`/class V (nullable pointer), or a V that is ITSELF
            // nullable (its own lowering already carries the tag): the decoded
            // value is the result and the miss path is its all-zero form.
            auto* phi = builder_.CreatePHI(valLlvm, 2, "gon.res");
            phi->addIncoming(dv, hitEnd);
            phi->addIncoming(llvm::Constant::getNullValue(valLlvm), preBB);
            return phi;
        }
        // .GetOrAdd(k, factory): V  (stdlib §4.8 borrowing / §4.9 owning)
        //   Dictionary<K,V>:       factory is (K) -> V
        //   OwningDictionary<K,V>:  factory is (K) -> owned V, result is a BORROW
        //
        // Semantics (§4.8 / §4.9): if `k` is present, return the existing value
        // and DO NOT invoke the factory; otherwise invoke factory(k) to produce a
        // V, store it under `k` (the same insert path Add/Set uses — record values
        // heap-boxed, class pointers stored directly), and return that value.
        //
        // Ownership (the cardinal risk, §28.3 / §28.8):
        //   * OwningDictionary MISS: the factory yields an `owned V` (+1 class
        //     pointer); we STORE it (the dict takes ownership, exactly as
        //     `Add(owned)` / the indexer set does — encodeValue stores the pointer
        //     directly, bz_dict_set_* records it) and RETURN A BORROW = the stored
        //     slot read back (NOT a transfer; the dict still owns it and disposes
        //     it once at scope-end via its #394 disposal cascade). producerState
        //     in the ownership checker classifies the GetOrAdd result as Borrowed
        //     (no resolved `owned` return), so the caller gets no disposal
        //     obligation — no double-free.
        //   * OwningDictionary HIT: the factory is NOT invoked (the call is in the
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
            // the TryGetValue / indexer-get decode (record, nullable, or primitive
            // struct e.g. decimal → load the boxed struct; scalar → ptrtoint then
            // truncate/bitcast; pointer → the void* IS the string/object pointer).
            // fix #685: any struct-typed value is heap-boxed; unbox via Load.
            // `slotV` is the raw stored void*.
            auto decodeSlot = [&](llvm::Value* slotV) -> llvm::Value* {
                if (valLlvm->isStructTy())
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

        // fix #1426 / stdlib §4.17: SortedDictionary<K,V>.FirstKey (least key)
        // and .LastKey (greatest key) — O(n) scan over the hash-table rep.
        // Throws InvalidOperationException if the dict is empty (§4.17).
        // Only meaningful on SortedDictionary; regular Dictionary has no such
        // property and sema rejects it there.
        bool isSortedDictRecv = recvTy && recvTy->astNode && recvTy->astNode->name &&
                                !recvTy->astNode->name->parts.empty() &&
                                recvTy->headName() == "SortedDictionary";
        if (isSortedDictRecv && (m == "FirstKey" || m == "LastKey") &&
            call.args.empty()) {
            bool isMax = (m == "LastKey");
            // Guard: empty dict throws InvalidOperationException.
            auto* cntFn = declareC("bz_dict_count", i64(), {ptrTy()});
            llvm::Value* cnt = builder_.CreateCall(cntFn, {recv}, "sd.cnt");
            emitGuardThrow(builder_.CreateICmpEQ(cnt, builder_.getInt64(0)),
                           "InvalidOperationException");

            if (keyTy) {
                // Generic-key path: build comparator, call bz_dict_min/max_key_gen.
                llvm::Function* cmpFn = buildSortedKeyComparator(keyTy);
                if (!cmpFn) return nullptr;  // unsupported key type
                const char* rtName = isMax ? "bz_dict_max_key_gen"
                                           : "bz_dict_min_key_gen";
                auto* mmFn = declareC(rtName, ptrTy(), {ptrTy(), ptrTy()});
                llvm::Value* keyBytesPtr = builder_.CreateCall(
                    mmFn, {recv, cmpFn}, "sd.mm");
                // Load the key value, then free the malloc'd copy.
                llvm::Type* keyLlvmTy = lowerTypeRef(keyTy);
                if (!keyLlvmTy) return nullptr;
                llvm::Value* keyVal = builder_.CreateLoad(
                    keyLlvmTy, keyBytesPtr, "sd.key");
                auto* freeFn2 = declareC("free", llvm::Type::getVoidTy(llvm_),
                                         {ptrTy()});
                builder_.CreateCall(freeFn2, {keyBytesPtr});
                return keyVal;
            } else {
                // String-key path: bz_dict_min/max_key_str returns char* data + len.
                // Build a bz_string heap object from those, same as lowerDictKeysFor.
                const char* rtNameStr = isMax ? "bz_dict_max_key_str"
                                              : "bz_dict_min_key_str";
                auto* dataSlot = entryAlloca(ptrTy());
                auto* lenSlot  = entryAlloca(i64());
                builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), dataSlot);
                builder_.CreateStore(builder_.getInt64(0), lenSlot);
                auto* mmStrFn = declareC(rtNameStr, i1(),
                                         {ptrTy(), ptrTy(), ptrTy()});
                builder_.CreateCall(mmStrFn, {recv, dataSlot, lenSlot});
                // data and len are now filled; build a heap bz_string object.
                llvm::Value* kdata = builder_.CreateLoad(ptrTy(), dataSlot, "sd.kd");
                llvm::Value* klen  = builder_.CreateLoad(i64(), lenSlot, "sd.kl");
                auto* i8Ty = llvm::Type::getInt8Ty(llvm_);
                uint64_t strSz = module_->getDataLayout().getTypeAllocSize(stringType());
                auto* callocFn  = declareC("calloc", ptrTy(), {i64(), i64()});
                llvm::Value* strObj = builder_.CreateCall(callocFn,
                    {builder_.getInt64(1), builder_.getInt64((int64_t)strSz)});
                builder_.CreateStore(kdata, builder_.CreateStructGEP(stringType(), strObj, 0));
                builder_.CreateStore(klen,  builder_.CreateStructGEP(stringType(), strObj, 1));
                builder_.CreateStore(llvm::ConstantInt::get(i8Ty, 1),
                    builder_.CreateStructGEP(stringType(), strObj, 2));
                initStringRc(strObj);
                return strObj;
            }
        }
    }

    // ---------------------------------------------------------------------------
    // StringBuilder — bz_array_t of string pointers (parts list).
    // Append adds a string part; ToString concatenates all parts.
    // ---------------------------------------------------------------------------
    if (isStrBld) {
        constexpr int64_t SB_ESZ = 8;  // pointer-sized elements (string ptrs)

        // Helper: coerce value to string. Returns the string pointer AND, via
        // the out-param, whether it was freshly RENDERED (a +1 producer: a
        // primitive formatted via emitPrimitiveToString, or an Object/class
        // rendered via lowerObjectToString) — the Append path uses that to decide
        // retain-vs-transfer (#1185).
        auto toStr = [&](llvm::Value* v, const ast::Expr* expr,
                         bool* rendered) -> llvm::Value* {
            if (!v) return nullptr;
            auto* rt = expr ? ctx_.exprType(*expr) : nullptr;
            if (rt && rt->kind == sema::TypeRef::Kind::Primitive &&
                rt->primitive != sema::PrimitiveType::String) {
                if (rendered) *rendered = true;
                return emitPrimitiveToString(v, rt->primitive);
            }
            // #1185 / stdlib §3.1: "Append(value: Object) = Append(value.
            // ToString())". An Object/class arg is converted via lowerObjectToString
            // (a fresh +1 string). This both matches the stdlib AND guarantees
            // every part is a real bz_string so the builder's part disposal
            // (bz_string_release) is never handed a raw object pointer (which
            // would misread a vtable as a refcount / data buffer).
            if (rt && rt->kind != sema::TypeRef::Kind::Primitive) {
                if (llvm::Value* r = lowerObjectToString(v)) {
                    if (rendered) *rendered = true;
                    return r;
                }
            }
            if (v->getType()->isPointerTy()) return v;
            // Fallback: format as integer
            if (rendered) *rendered = true;
            llvm::Value* iv = v->getType()->isIntegerTy(64)
                ? v : builder_.CreateSExt(v, i64());
            return emitPrimitiveToString(iv, sema::PrimitiveType::Long);
        };

        if ((m == "Append" || m == "AppendLine") && !call.args.empty()) {
            const ast::Expr* sExpr = call.args[0].value.get();
            llvm::Value* s = nullptr;
            // #1185 / stdlib §3.1 / §4.6: the builder must own an independent +1
            // for each appended part so the part stays live for the builder's
            // lifetime and is released on its disposal. A freshly RENDERED string
            // (a primitive/Object formatted via toStr) or a STRING arg that is
            // itself a +1 PRODUCER (`+`/interpolation/`string.Join`/ToString()/
            // string-returning call) hands a single +1 — TRANSFER (no retain).
            // Any other string arg (a literal / local / field borrow) is RETAINED.
            // Mirrors lowerStringFieldStore.
            bool freshPlusOne = false;
            if (sExpr) {
                bool rendered = false;
                s = toStr(lowerExpr(*sExpr), sExpr, &rendered);
                freshPlusOne = rendered || isPlusOneStringProducer(sExpr);
            }
            if (s) {
                if (!freshPlusOne) emitStringRetain(s);  // borrow → builder's own +1
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
            // #1185: the builder owns a +1 per part (Append retains), so Clear
            // must release each part before emptying — bz_array_clear_disposing
            // with bz_string_release (null-/sentinel-safe) does exactly that.
            auto* relFn = declareC("bz_string_release",
                                   llvm::Type::getVoidTy(llvm_), {ptrTy()});
            auto* fn = declareC("bz_array_clear_disposing",
                                llvm::Type::getVoidTy(llvm_), {ptrTy(), ptrTy()});
            builder_.CreateCall(fn, {recv, relFn});
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
        // §3.1 AppendFormat(format: string, args: Array<Object>): StringBuilder.
        // Substitutes {N} placeholders with args[N].ToString() and appends the
        // result (bz_sb_append_format runtime helper). fix #1179: previously
        // this had no dispatch case and the call fell through as a silent no-op.
        if (m == "AppendFormat" && call.args.size() == 2 &&
            call.args[0].value && call.args[1].value) {
            llvm::Value* fmtV  = lowerExpr(*call.args[0].value);
            llvm::Value* argsV = lowerExpr(*call.args[1].value);
            if (!fmtV || !argsV) return nullptr;
            auto* fn = declareC("bz_sb_append_format", llvm::Type::getVoidTy(llvm_),
                                {ptrTy(), ptrTy(), ptrTy()});
            builder_.CreateCall(fn, {recv, fmtV, argsV});
            return recv;  // allow chaining
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
        // stdlib §3.1 / §8.5 / §18.5: `sb.ToString()` — the builder's OWN
        // declared `ToString(): string`, not the §8.5 type-name default.
        // fix #2923 (v0.240): the body moved to `emitStringBuilderToString` so
        // the §18.5 interpolation hole `$"{sb}"` reaches the SAME lowering
        // instead of a second, drifting spelling of it (interpolation used to
        // reach `lowerObjectToString`, which mis-read the builder's parts array
        // as a `%bz.string`).
        if (m == "ToString" && call.args.empty())
            return emitStringBuilderToString(recv);
    }

    return nullptr;  // not a collection call
}


// fix #2513 / spec §35.2 (v0.236): the stdlib §2.20 `Result<T, E>` carries four
// compiler-provided per-instantiation combinators — `Map`, `MapError`,
// `AndThen`, `UnwrapOr`. They have NO MethodSymbol (the prelude body injection
// #2541's previous-lane attempt tried aborts with an LLVM PHI type assertion in
// lowerCaseExpr on a choice-shaped return). Sema already typed the call with
// the result instantiation's TypeRef (ExprTypecheck_Calls.cpp ~line 7412), so
// the result type is known here; the dedicated path emits the inline body — a
// switch on the choice discriminator tag + branch-specific bodies — and
// returns null when the call is not a Result combinator, so the regular
// method-symbol dispatch runs unchanged.
//
// ADMISSIBILITY (sema has already rejected non-value-like receivers and
// non-value-like results per §35.4 rule 3, so by the time we get here both
// receiver and result instantiations ARE value-like). The body is:
//   * Map(f):    Ok(v)  -> Ok(f(v));     Error(e) -> Error(e)
//   * MapError:  Ok(v)  -> Ok(v);        Error(e) -> Error(f(e))
//   * AndThen:   Ok(v)  -> f(v);         Error(e) -> Error(e)
//   * UnwrapOr:  Ok(v)  -> v;            Error(_) -> fallback
// Every combinator is total — no fault path. Function parameters are
// call-bounded (§28.14), so f is invoked now and never retained. Each call's
// `f` is lowered via the function-typed argument's exprType (a Function
// TypeRef), reusing the same fn/env/fnPtr-load pattern lowerCall's closure-
// call arm uses; the lambda parameter binds the receiver's case payload.
//
// Returns nullptr when the call is not a Result combinator (lets the regular
// dispatch handle it). Always lowers the receiver and arguments when
// returning non-null — the dispatch returns the result value the caller
// expects.
// fix #3287 helper: invoke a PRE-EVALUATED function-typed expression whose
// closure pointer is `closurePtr` (a `ptr`) with the single payload value
// `val` (LLVM type `valLL`), expecting the function's return type to be
// `retSemTy`. Mirrors lowerCall's closure-call arm's fn-ptr + env-ptr +
// retTy derivation. Split out from `invokeClosureOnValue` so the argument
// expression can be evaluated eagerly at the call site (spec §22.5
// evaluation order) before the Ok/Error branch, while the actual invocation
// (which depends on the chosen arm's payload value `v` or `e`) stays in the
// appropriate arm.
llvm::Value* Emitter::invokeEvaluatedClosurePtr(
        llvm::Value* closurePtr,
        llvm::Value* val,
        llvm::Type* valLL,
        const sema::TypeRef* retSemTy) {
    if (!closurePtr || !closurePtr->getType()->isPointerTy()) return nullptr;
    llvm::Value* fnPtr = builder_.CreateLoad(ptrTy(), closurePtr);
    llvm::Value* envSlot = builder_.CreateGEP(
        ptrTy(), closurePtr, {builder_.getInt32(1)});
    llvm::Value* envPtr = builder_.CreateLoad(ptrTy(), envSlot);
    llvm::Type* retLL = lowerTypeRef(retSemTy);
    auto* fnTy = llvm::FunctionType::get(
        retLL, {ptrTy(), valLL}, false);
    return builder_.CreateCall(
        fnTy, fnPtr, {envPtr, val}, "rescomb.fcall");
}

llvm::Value* Emitter::tryLowerResultCombinatorCall(const ast::CallExpr& call) {
    if (!call.callee) return nullptr;
    auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(call.callee.get());
    if (!ma || !ma->target) return nullptr;
    const std::string_view m = ma->member;
    if (m != "Map" && m != "MapError" && m != "AndThen" && m != "UnwrapOr") {
        return nullptr;
    }
    const sema::TypeRef* recvTy = ctx_.exprType(*ma->target);
    if (!recvTy || recvTy->kind != sema::TypeRef::Kind::Named ||
        !recvTy->declaration ||
        recvTy->declaration->kind != ast::TypeDeclKind::Choice ||
        recvTy->declaration->name != "Result" ||
        recvTy->genericArgs.size() != 2) {
        return nullptr;
    }
    if (call.args.size() != 1 || !call.args[0].value) {
        // Sema rejected arity mismatches; defensive only.
        return nullptr;
    }
    // The receiver choice struct type (specialized per-instantiation via the
    // active substitution — `Result<int, string>` lays Error's payload as
    // {ptr} for string, {i32} for int, etc., so the slot types match). The
    // genericSubst_ populated by lowerTypeRef/RecordSubstGuard at the call
    // site also drives the result struct below.
    llvm::StructType* recvSt = choiceStruct(recvTy->declaration);
    // §35.13 / §14.1: a GENERIC choice specializes its payload layout per
    // instantiation, so the recvSt above is the UNSPECIALIZED template
    // (`%choice.Result = { i32, { i32 }, { i32 } }` for T=int, E=int). The
    // receiver value, lowered WITH the receiver's substitution active (a local
    // `r: Result<int, string>` is built at `var r := ...Ok(5)` with the T/E
    // substitutions installed), carries the SPECIALIZED layout
    // (`%choice.Result$spec0 = { i32, { i32 }, { ptr } }` for E=string). Install
    // the receiver's substitution so choiceStruct returns the specialized
    // struct the receiver's value actually has, AND so the element-type
    // derivations below (tLL/eLL) line up with the payload structs. Save and
    // restore mirrors the rest of codegen's per-call substitution convention.
    auto savedRecvSubstPair =
        setupReceiverGenericSubst(ma->target.get(), recvTy->declaration);
    llvm::StructType* recvStSpec = choiceStruct(recvTy->declaration);
    genericSubst_ = std::move(savedRecvSubstPair.subst);
    genericSubstSemaTy_ = std::move(savedRecvSubstPair.semaTy);
    if (recvStSpec) recvSt = recvStSpec;
    // Lower the receiver expression. A local/parameter receiver lowers to a
    // pointer (the spilled struct slot); a fresh construction or match
    // scrutinee lowers to a struct value. Normalize to a struct value.
    llvm::Value* recvPtrOrVal = lowerExpr(*ma->target);
    if (!recvPtrOrVal) return nullptr;
    llvm::Value* recvVal = recvPtrOrVal;
    if (recvPtrOrVal->getType()->isPointerTy()) {
        // Pointer → struct value load (the spilled slot is exactly recvSt).
        recvVal = builder_.CreateLoad(recvSt, recvPtrOrVal, "rescomb.recv");
    } else if (recvPtrOrVal->getType() != recvSt) {
        // Unexpected type (e.g. ptr-to-struct or unrelated). Bail.
        return nullptr;
    }
    // Read the discriminator tag at slot 0 (i32) and split on Ok/Error.
    // Result's cases are declared Ok then Error, so OkTag == 0, ErrorTag == 1.
    llvm::Value* tag = builder_.CreateExtractValue(recvVal, {0}, "rescomb.tag");
    // Index 1 = Ok payload slot, 2 = Error payload slot.
    llvm::Value* okPldStruct = builder_.CreateExtractValue(
        recvVal, {1}, "rescomb.ok.pld");
    llvm::Value* errPldStruct = builder_.CreateExtractValue(
        recvVal, {2}, "rescomb.err.pld");
    // Per-instantiation payload element types — for `Result<int, string>` the
    // Ok payload struct is {i32} and the Error payload struct is {ptr}, so
    // the elements are lowerTypeRef(recvTy->genericArgs[0|1]).
    const sema::TypeRef* tArg = recvTy->genericArgs[0];
    const sema::TypeRef* eArg = recvTy->genericArgs[1];
    llvm::Type* tLL = lowerTypeRef(tArg);
    llvm::Type* eLL = lowerTypeRef(eArg);
    llvm::Value* v = builder_.CreateExtractValue(
        okPldStruct, {0}, "rescomb.v");    // Ok(value: T)
    llvm::Value* e = builder_.CreateExtractValue(
        errPldStruct, {0}, "rescomb.e");  // Error(error: E)
    // Decide the result type. For Map/AndThen the result is Result<U, E>;
    // for MapError it is Result<T, F>; for UnwrapOr it is T. sema already
    // set call.exprType to that instantiation's TypeRef, so use it.
    const sema::TypeRef* resultSemTy = ctx_.exprType(call);
    if (!resultSemTy) return nullptr;
    // MapError's result is a different choice layout than the receiver (the
    // Ok payload T is the same, but the Error payload type changes from E to
    // F). Map / AndThen / UnwrapOr keep the same shape as the receiver (for
    // Map / AndThen, only the Ok payload type changes from T to U — same
    // struct {i32, Ok_payload, Error_payload} with the Ok slot's ELEMENT
    // type swapped; choiceStruct keying on field types produces a distinct
    // specialized struct per §14.1).
    llvm::StructType* resultSt = nullptr;
    // §35.13 / §14.1: the result choice also specializes per instantiation
    // — for `Map(f: int->long)` on `Result<int, string>` the result is
    // `Result<long, string>` which lays Error's payload as {ptr} (string)
    // and Ok's payload as {i64} (long). Install the result TypeRef's
    // substitution so the result choiceStruct AND every buildChoiceStructValue
    // call (which reads genericSubst_ to materialize the payload struct type)
    // return the specialized struct — not the unspecialized template.
    RecordSubstGuard resultSubst(this, m == "UnwrapOr" ? nullptr : resultSemTy);
    if (m != "UnwrapOr") {
        resultSt = choiceStruct(resultSemTy->declaration);
    }
    // Build the dispatch. For Map/MapError/AndThen: tag-switch with Ok and
    // Error branches building a Result<…> choice struct; for UnwrapOr: tag-
    // switch returning either v or the fallback arg's value.
    auto* okBB  = llvm::BasicBlock::Create(llvm_, "rescomb.ok",  currentFn_);
    auto* errBB = llvm::BasicBlock::Create(llvm_, "rescomb.err", currentFn_);
    auto* endBB = llvm::BasicBlock::Create(llvm_, "rescomb.end", currentFn_);
    // fix #3287: spec §22.5 (evaluation order) requires the call's supplied
    // argument expressions to run BEFORE the call body enters, in textual
    // order — and stdlib §2.20 ("UnwrapOr(fallback: T) declares by-value
    // param — no lazy twin; UnwrapOrElse recorded-decline") makes this
    // normative for all four combinators. The pre-fix shape evaluated the
    // argument INSIDE the Ok or Error arm, so a side-effecting or throwing
    // argument on the unused branch never ran. Evaluate it here at the call
    // site, then stash the result in an alloca slot so each arm can load it.
    // For Map/MapError/AndThen the argument is a closure (function-typed);
    // we evaluate it eagerly to a closure pointer here. The actual
    // `CreateCall` (which takes the arm's payload v or e) still belongs in
    // the arm — that part is arm-dependent. For UnwrapOr the argument is a
    // plain T value evaluated eagerly once; the Error arm uses it.
    const sema::TypeRef* argSemTy = ctx_.exprType(*call.args[0].value);
    llvm::Value* argVal = lowerExpr(*call.args[0].value);
    if (!argVal) return nullptr;
    // fix #3296 / §17.12 (v0.266): a `throw` expression used as the
    // combinator's argument diverges (CG40 — `lowerExpr` set `diverged_ = true`
    // and branched the current BB to `unwindBB`). The dispatch below
    // (`CreateStore(argVal, argSlot)` / `CreateICmpEQ` / `CreateCondBr` /
    // arm-body emit) would all land in a now-terminated basic block and the
    // IR verifier would reject the result with "Basic Block in function ...
    // does not have terminator". The call's control flow on the throw edge is
    // already complete (the throw branched to `unwindBB`); there is no join
    // to build. Return a poison placeholder consistent with the CG40 shape —
    // the call's value at this scope is bottom (a `throw` argument never
    // produces one), so a poison value is correct at every join that would
    // have consumed it. Scoped narrowly to the diverged-argument case; the
    // happy path is untouched.
    if (diverged_) return llvm::PoisonValue::get(ptrTy());
    llvm::Type* argSlotTy = nullptr;
    if (m == "UnwrapOr") {
        // UnwrapOr: the lowered value is the fallback itself. May be an SSA
        // primitive (i32 from a literal/local) or a pointer (a class instance
        // / spilled aggregate). The slot stores the value as-is.
        argSlotTy = argVal->getType();
    } else {
        // Map/MapError/AndThen: argVal must be a pointer to the closure
        // struct. Reserve a `ptr`-sized slot holding the closure pointer.
        if (!argVal->getType()->isPointerTy()) return nullptr;
        argSlotTy = ptrTy();
    }
    llvm::Value* argSlot = entryAlloca(argSlotTy);
    builder_.CreateStore(argVal, argSlot);
    llvm::Value* isOk = builder_.CreateICmpEQ(
        tag, llvm::ConstantInt::get(i32(), 0), "rescomb.isok");
    builder_.CreateCondBr(isOk, okBB, errBB);
    // --- Ok branch ---
    builder_.SetInsertPoint(okBB);
    llvm::Value* okResult = nullptr;
    if (m == "Map") {
        // Ok(f(v)) — arg was eagerly evaluated at the call site; load the
        // closure pointer and invoke it with the Ok payload v.
        llvm::Value* closurePtr = builder_.CreateLoad(
            ptrTy(), argSlot, "rescomb.arg.closure");
        llvm::Value* mapped = invokeEvaluatedClosurePtr(
            closurePtr, v, tLL,
            resultSemTy->genericArgs[0]);  // U
        okResult = buildChoiceStructValue(
            resultSemTy->declaration, /*okTag=*/0,
            /*okPayload=*/mapped,
            /*errPayload=*/llvm::UndefValue::get(eLL));
    } else if (m == "MapError") {
        // Ok(v) — pass through unchanged.
        okResult = buildChoiceStructValue(
            resultSemTy->declaration, /*okTag=*/0,
            /*okPayload=*/v,
            /*errPayload=*/llvm::UndefValue::get(
                lowerTypeRef(resultSemTy->genericArgs[1])));
    } else if (m == "AndThen") {
        // f(v) is itself a Result<U, E> (the lambda's return type). Invoke
        // the pre-evaluated closure with v; the closure returns the
        // Result<U, E> struct directly.
        llvm::Value* closurePtr = builder_.CreateLoad(
            ptrTy(), argSlot, "rescomb.arg.closure");
        okResult = invokeEvaluatedClosurePtr(
            closurePtr, v, tLL, resultSemTy);
    } else {  // UnwrapOr
        okResult = v;
    }
    if (m == "UnwrapOr") {
        builder_.CreateBr(endBB);
    } else {
        builder_.CreateBr(endBB);
    }
    auto* okEnd = builder_.GetInsertBlock();
    // --- Error branch ---
    builder_.SetInsertPoint(errBB);
    llvm::Value* errResult = nullptr;
    if (m == "Map") {
        // Error(e) — pass through unchanged.
        errResult = buildChoiceStructValue(
            resultSemTy->declaration, /*errTag=*/1,
            /*okPayload=*/llvm::UndefValue::get(
                lowerTypeRef(resultSemTy->genericArgs[0])),
            /*errPayload=*/e);
    } else if (m == "MapError") {
        // Error(f(e)) — arg was eagerly evaluated at the call site; load
        // the closure pointer and invoke it with the Error payload e.
        llvm::Value* closurePtr = builder_.CreateLoad(
            ptrTy(), argSlot, "rescomb.arg.closure");
        llvm::Value* mapped = invokeEvaluatedClosurePtr(
            closurePtr, e, eLL,
            resultSemTy->genericArgs[1]);  // F
        errResult = buildChoiceStructValue(
            resultSemTy->declaration, /*errTag=*/1,
            /*okPayload=*/llvm::UndefValue::get(tLL),
            /*errPayload=*/mapped);
    } else if (m == "AndThen") {
        // Error(e) — pass through.
        errResult = buildChoiceStructValue(
            resultSemTy->declaration, /*errTag=*/1,
            /*okPayload=*/llvm::UndefValue::get(
                lowerTypeRef(resultSemTy->genericArgs[0])),
            /*errPayload=*/e);
    } else {  // UnwrapOr
        // The fallback value was eagerly evaluated at the call site (spec
        // §22.5 evaluation order); load it here and use it as the Error
        // arm's result. The Ok arm's result is the payload v.
        errResult = builder_.CreateLoad(argSlotTy, argSlot, "rescomb.arg.fb");
    }
    builder_.CreateBr(endBB);
    auto* errEnd = builder_.GetInsertBlock();
    // --- Merge ---
    builder_.SetInsertPoint(endBB);
    if (m == "UnwrapOr") {
        auto* phi = builder_.CreatePHI(tLL, 2, "rescomb.unwraphi");
        phi->addIncoming(okResult, okEnd);
        phi->addIncoming(errResult, errEnd);
        return phi;
    }
    auto* phi = builder_.CreatePHI(resultSt, 2, "rescomb.phi");
    phi->addIncoming(okResult, okEnd);
    phi->addIncoming(errResult, errEnd);
    return phi;
}

// fix #2513 helper: invoke the function-typed expression `arg` with the
// single payload value `val` (LLVM type `valLL`), expecting the function's
// return type to be `retSemTy`. Mirrors lowerCall's closure-call arm's
// fn-ptr + env-ptr + retTy derivation.
llvm::Value* Emitter::invokeClosureOnValue(
        const ast::Expr& arg,
        llvm::Value* val,
        llvm::Type* valLL,
        const sema::TypeRef* retSemTy) {
    const sema::TypeRef* ft = ctx_.exprType(arg);
    if (!ft || ft->kind != sema::TypeRef::Kind::Function) return nullptr;
    llvm::Value* closurePtr = lowerExpr(arg);
    if (!closurePtr || !closurePtr->getType()->isPointerTy()) return nullptr;
    llvm::Value* fnPtr = builder_.CreateLoad(ptrTy(), closurePtr);
    llvm::Value* envSlot = builder_.CreateGEP(
        ptrTy(), closurePtr, {builder_.getInt32(1)});
    llvm::Value* envPtr = builder_.CreateLoad(ptrTy(), envSlot);
    llvm::Type* retLL = lowerTypeRef(retSemTy);
    auto* fnTy = llvm::FunctionType::get(
        retLL, {ptrTy(), valLL}, false);
    llvm::Value* ret = builder_.CreateCall(
        fnTy, fnPtr, {envPtr, val}, "rescomb.fcall");
    return ret;
}

// fix #2513 helper: build a `Result<...>` choice struct value { i32 tag,
// Ok_payload_struct, Error_payload_struct } with the active case's payload
// filled and the other slot's element value left undefined (the inactive
// payload struct's tag slot is never read; choice values carry no per-slot
// moved-from state per §35.4). Used by every combinator's branch bodies.
llvm::Value* Emitter::buildChoiceStructValue(
        const sema::TypeSymbol* choiceTy,
        unsigned activeTag,
        llvm::Value* okPayload,
        llvm::Value* errPayload) {
    llvm::StructType* st = choiceStruct(choiceTy);
    llvm::Value* whole = llvm::UndefValue::get(st);
    whole = builder_.CreateInsertValue(
        whole, llvm::ConstantInt::get(i32(), activeTag), {0});
    // Active case: wrap the element value into its payload struct {T} and
    // insert into the case's struct slot.
    llvm::Type* payloadStructTy = st->getElementType(1 + activeTag);
    llvm::Value* activePldStruct = llvm::UndefValue::get(payloadStructTy);
    if (auto* pldStruct = llvm::dyn_cast<llvm::StructType>(payloadStructTy)) {
        if (pldStruct->getNumElements() >= 1) {
            activePldStruct = builder_.CreateInsertValue(
                activePldStruct, (activeTag == 0 ? okPayload : errPayload),
                {0});
        }
    } else {
        // Payload struct is itself a single-element wrapper for non-record
        // payloads; emit the value directly.
        activePldStruct = (activeTag == 0 ? okPayload : errPayload);
    }
    whole = builder_.CreateInsertValue(
        whole, activePldStruct, {1 + activeTag});
    return whole;
}


// fix #2728 / §32.6 + §28.3 *Unbound owned temporaries* (v0.240). See the
// declaration in LlvmBackend_impl.hpp for the correctness argument; the short
// form is that this must match the set of expressions that MINT a buffer, not
// the set that merely HAS FFI-buffer type.
//
// ⚠ RawBytes.FromForeign(p, length, freeOnDispose) (stdlib §10.3) is a fifth
// `owned`-returning static and is DELIBERATELY NOT in this table: its buffer is
// FOREIGN, so bz_rawbytes_free would free memory Bezel never allocated. Anyone
// widening this predicate must widen it by ENUMERATION, never by a rule of the
// shape "any owned-returning factory" — that phrasing admits FromForeign and
// turns this reclaim into a free of foreign memory. (Noted by the #2728 review;
// FromForeign has no handler in tryLowerBuiltinClassCall today, so the hazard is
// latent rather than live.)
bool Emitter::isFfiBufferFactoryCall(const ast::Expr& e, bool& isRawBytes) {
    isRawBytes = false;
    auto* call = dynamic_cast<const ast::CallExpr*>(&e);
    if (!call || !call->callee) return false;
    auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(call->callee.get());
    if (!ma || !ma->target) return false;
    auto* id = dynamic_cast<const ast::IdentifierExpr*>(ma->target.get());
    if (!id) return false;
    if (call->args.size() != 1 || !call->args[0].value) return false;
    // (1) the syntactic factory table, kept in lockstep with the lowering below.
    bool shape = false;
    if (id->name == "CString") {
        shape = (ma->member == "From" || ma->member == "FromBytes");
    } else if (id->name == "WString") {
        shape = (ma->member == "From");
    } else if (id->name == "RawBytes") {
        shape = (ma->member == "From");
        isRawBytes = shape;
    }
    if (!shape) return false;
    // (2) and sema must actually have resolved the result to the BUILTIN
    // CString/WString/RawBytes head (ExprTypecheck_Calls.cpp stamps a
    // builtinClassSym TypeRef). Without this conjunct a user type that happened
    // to be named `CString` with a static `From` would have its result freed.
    std::string_view n = builtinTypeName(ctx_.exprType(e));
    if (n != id->name) { isRawBytes = false; return false; }
    return true;
}

// fix #2728: the reclaim itself — the same null-guarded shape emitDisposals
// uses for a `using var`-bound FFI buffer (LlvmBackend_Disposal.cpp's
// `ffiBuffer` arm), so the bound and unbound spellings free identically.
void Emitter::emitFfiBufferTempFree(llvm::AllocaInst* slot, bool isRawBytes) {
    llvm::Value* buf = builder_.CreateLoad(ptrTy(), slot, "ffitmp.buf");
    llvm::Value* isNull = builder_.CreateICmpEQ(
        buf, llvm::ConstantPointerNull::get(ptrTy()), "ffitmp.isnull");
    auto* frBB = llvm::BasicBlock::Create(llvm_, "ffitmp.free", currentFn_);
    auto* contBB = llvm::BasicBlock::Create(llvm_, "ffitmp.cont", currentFn_);
    builder_.CreateCondBr(isNull, contBB, frBB);
    builder_.SetInsertPoint(frBB);
    if (isRawBytes) {
        // stdlib §10.3 / fix #985: a RawBytes is a { ptr, len } struct whose
        // buffer is a separate allocation — both are reclaimed by the runtime
        // helper, exactly as the `using var` path does.
        auto* fn = declareC("bz_rawbytes_free", llvm::Type::getVoidTy(llvm_),
                            {ptrTy()});
        builder_.CreateCall(fn, {buf});
    } else {
        builder_.CreateCall(freeFn(), {buf});
    }
    builder_.CreateBr(contBB);
    builder_.SetInsertPoint(contBB);
}

// fix #2733 / §28.3 (v0.240): establish an extern-argument FFI buffer's reclaim.
//
// Two things happen here, and they are the two halves of the one defect:
//
// (1) A `ffiBuffer` Disposal is pushed onto disposeStack_.back() AT PRODUCTION.
//     §28.3: "each temporary's disposal is established at its production, so the
//     pass runs in reverse order of production on the establishment-ordered
//     cleanup stack". This is what covers the EXCEPTION edge — the backend's
//     unwind path (emitPendingCheck → emitUnwindDisposals) is literally a walk
//     of disposeStack_, so a later argument that throws now disposes this
//     buffer instead of leaking it.
//
//     ⚠ `unwindOnly` is deliberately NOT set. emitDisposals tests `unwindOnly`
//     BEFORE `ffiBuffer` and each arm `continue`s, so an entry carrying both
//     would take the unwindOnly arm and reach emitDisposeAndReclaim — a VTABLE
//     DISPATCH through the first bytes of a raw malloc'd buffer (it has no
//     object header). Leaving it a plain `ffiBuffer` entry also makes it fire
//     on the normal scope-exit edge, which is the deliberate conservative
//     BACKSTOP: if no full-expression boundary ever reaches this temp (a
//     lowering path that produces it below depth 0 — e.g. inside a lambda body
//     lowered mid-expression), it is still freed exactly once at its own scope
//     end. Later than §28.3's boundary, never earlier: a bounded over-live, not
//     a use-after-free.
//
// (2) The temp is queued for the next full-expression boundary, where
//     flushFfiExprTemps frees it and NULLS the slot — after which the entry
//     from (1) is a null-guarded no-op on every subsequent edge.
//
// ⚠ LATENT INVARIANT THIS DESIGN RESTS ON, undocumented before the #2733 review:
// a LEAF THUNK must never call lowerExpr. The ~45 leaf thunks and trampolines
// (LINQ, box ToString/Equals/GetHashCode, sort comparators, spawn adapters,
// widening thunks) do not touch disposeStack_, which is safe ONLY because none
// of them lowers a user expression, so this function never runs under one. If
// one ever did, (1) would push a Disposal holding a FOREIGN function's alloca
// onto the enclosing function's live disposeStack_ — malformed IR. Every emitter
// that DOES lower user code under a swapped currentFn_ (the lambda body, the six
// property/indexer accessor emitters, buildSpecializedMethodFn, the three
// Layout.cpp thunks, genericInstSfinitFn, the specialized disposers) saves,
// clears and restores disposeStack_, which is what keeps (1) per-function.
void Emitter::registerFfiExprTemp(llvm::AllocaInst* slot, bool isRawBytes) {
    if (!slot) return;
    if (!disposeStack_.empty()) {
        Disposal d{slot, nullptr};
        d.ffiBuffer = true;
        d.rawBytes = isRawBytes;
        disposeStack_.back().push_back(d);
    }
    // fix #2754 (§28.3 / §30.13, v0.282) — promote the slot into the async
    // state machine's frame when this FFI temp is produced INSIDE an async
    // method body (`smStateStruct_` non-null). Without this promotion, the
    // entry-block alloca is a plain stack slot; on a real cross-thread resume
    // the worker thread reads an uninitialised stack offset, the K8 boundary
    // free's null-guard sees `null` and skips the reclaim, and the
    // bz_string_to_utf8z buffer leaks (the strictly-better-than-wild-free
    // shape #2733 introduced). Mirror the analogous Pattern A task-temp
    // promotion at LlvmBackend_Async.cpp:3702 (registerAwaitedTaskTemp) —
    // `fixupSuspendDominance` heap-spills the slot through
    // `asyncSpillPtrField_` so the post-resume load reads the persisted
    // value the store-before-suspend wrote. // per §28.3, §30.13
    if (smStateStruct_) asyncHandlerSpillSlots_.push_back(slot);
    ffiExprTemps_.push_back({slot, isRawBytes, currentFn_});
}

// fix #2733 / §28.3 (v0.240): the full-expression boundary discharge. Called
// from lowerExpr when nesting depth returns to 0 — i.e. once the whole K8 full
// expression has been lowered and "every use of it within that expression has
// completed", which is precisely where §28.3 puts the disposal. Emitting here
// instead of after the callee's CreateCall is what keeps the buffer live across
// an OUTER call that reads an alias the extern returned.
void Emitter::flushFfiExprTemps() {
    // A terminated block means the full expression diverged (a `throw`); the
    // unwind edge's disposeStack_ walk has already reclaimed these, and emitting
    // after a terminator would be invalid IR. Drop the queue: entry (1) above
    // still guards every remaining edge.
    llvm::BasicBlock* bb = builder_.GetInsertBlock();
    bool live = currentFn_ && bb && !bb->getTerminator();
    std::vector<FfiExprTemp> keep;
    for (const auto& t : ffiExprTemps_) {
        // Never emit a free against another function's alloca (see
        // FfiExprTemp::fn), and — this is the part that matters — do not DROP it
        // either. A nested-function boundary fires while the ENCLOSING
        // expression is still open (`f(CString.From(x), lambda …)`), so the
        // outer function's temps are still owed a reclaim at the outer boundary.
        // Keeping them queued is what makes that boundary still find them.
        if (t.fn != currentFn_) { keep.push_back(t); continue; }
        // Diverged: the block is terminated, the unwind edge's disposeStack_
        // walk already reclaimed this, and emitting after a terminator would be
        // invalid IR. Drop without emitting.
        if (!live) continue;
        emitFfiBufferTempFree(t.slot, t.rawBytes);
        // Discharge, exactly as buildOrderedArgs nulls transferredOwnedArgSlots:
        // the disposeStack_ entry established at production stays registered but
        // now loads null, so scope end / return / unwind are all no-ops. This is
        // what makes the reclaim exactly-once on every path, and it is also what
        // makes a LOOP correct — the slot is an entry-block alloca reused by
        // every iteration, and each iteration's flush re-arms it.
        builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), t.slot);
    }
    ffiExprTemps_.swap(keep);
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
            // Decimal.ToString(format: string) — §9.3 / §2.18 #1412. Routes to
            // bz_decimal_to_string_fmt which handles the full §2.18 decimal
            // vocabulary (F/Fn, E/En, G, P/Pn) and returns nullptr for anything
            // else; the null check here raises FormatException (strict-fault, §2.18).
            if (mname == "ToString" && call.args.size() == 1 &&
                call.args[0].value) {
                llvm::Value* fmtVal = lowerExpr(*call.args[0].value);
                if (!fmtVal) return nullptr;
                auto* fn = declareC("bz_decimal_to_string_fmt", ptrTy(),
                                    {ptrTy(), ptrTy()});
                llvm::Value* res = builder_.CreateCall(fn, {slot, fmtVal},
                                                       "dec.tostrf");
                // nullptr → unrecognized/malformed specifier → FormatException
                // (§2.18 strict-fault, #1412).
                llvm::Value* isNull = builder_.CreateICmpEQ(
                    res,
                    llvm::Constant::getNullValue(ptrTy()),
                    "dec.fmt.null");
                emitGuardThrow(isNull, "FormatException");
                return res;
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
            // Per stdlib §9.3 (v0.195, #2339): digits outside 0..28 must
            // raise ArgumentOutOfRangeException. The codegen guard fires
            // BEFORE the runtime call so the bounds check is statically
            // visible at the IR level (it elides the runtime entirely on
            // the rejection path, matching the §1/§4.4 constructor-guard
            // pattern in LlvmBackend_Exprs2.cpp).
            if (mname == "Round" && call.args.size() == 1 && call.args[0].value) {
                llvm::Value* digits = lowerExpr(*call.args[0].value);
                if (!digits) return nullptr;
                if (!digits->getType()->isIntegerTy(32))
                    digits = builder_.CreateTrunc(digits, i32(), "dec.round.d");
                emitGuardThrow(
                    builder_.CreateOr(
                        builder_.CreateICmpSLT(digits, builder_.getInt32(0)),
                        builder_.CreateICmpSGT(digits, builder_.getInt32(28))),
                    "ArgumentOutOfRangeException");
                llvm::AllocaInst* oSlot = entryAlloca(dst);
                auto* fn = declareC("bz_decimal_round", llvm::Type::getVoidTy(llvm_),
                                    {ptrTy(), i32(), ptrTy()});
                builder_.CreateCall(fn, {slot, digits, oSlot});
                return builder_.CreateLoad(dst, oSlot, "dec.round");
            }
        }
    }

    // stdlib §10.3 / fix #985: RawBytes.CopyTo(destination: Array<byte>,
    // destOffset: int). The receiver is a RawBytes ({ ptr, len } struct);
    // bz_rawbytes_copyto memcpy's `len` bytes into the destination array at
    // destOffset (bounds-clamped). Returns unit. (.Length and .Pointer are
    // property reads handled in LlvmBackend_Exprs.cpp; CopyTo is the only
    // instance method.)
    {
        auto* recvTy = ctx_.exprType(*ma->target);
        // fix #2809 (v0.240): both TypeRef shapes — a `rb: RawBytes` PARAMETER
        // carries no `declaration`, so the declaration-only test silently
        // declined the call and `rb.CopyTo(dst, 0)` became a NO-OP that copied
        // nothing (measured: the round-trip sum came back 0 instead of 26).
        bool isRaw = builtinTypeName(recvTy) == "RawBytes" &&
            isFfiBufferTypeRef(recvTy);
        if (isRaw && ma->member == "CopyTo" && call.args.size() == 2 &&
            call.args[0].value && call.args[1].value) {
            llvm::Value* rb = lowerExpr(*ma->target);
            if (!rb) return nullptr;
            llvm::Value* dest = lowerExpr(*call.args[0].value);
            if (!dest) return nullptr;
            llvm::Value* off = lowerExpr(*call.args[1].value);
            if (!off) return nullptr;
            // §10.3: destOffset is `int` (i32) — widen to the i64 runtime param.
            if (off->getType()->isIntegerTy() &&
                off->getType()->getIntegerBitWidth() < 64)
                off = builder_.CreateSExt(off, i64(), "raw.off64");
            auto* fn = declareC("bz_rawbytes_copyto",
                                llvm::Type::getVoidTy(llvm_),
                                {ptrTy(), ptrTy(), i64()});
            builder_.CreateCall(fn, {rb, dest, off});
            return llvm::ConstantInt::get(i32(), 0);  // unit
        }
    }

    // stdlib §10.1 / issue #1000: CString.ToBytes(): owned Array<byte>. The
    // receiver is a CString (a bare null-terminated char*); bz_cstring_to_bytes
    // returns a FRESH, independently-owned bz_array_t holding a COPY of the
    // strlen bytes (count = byte length excluding the null terminator, §10.1
    // `Length`; elem_size 1). The copy is essential: without it the returned
    // array's data would alias the CString's own buffer, and disposing both the
    // CString (free) and the array (bz_array_free → free(data)) at scope end
    // would free the same allocation twice (free(): invalid pointer / SIGABRT).
    // A fresh malloc gives the result its own buffer, so each owner — the
    // CString and the Array<byte> — frees exactly once (§28.3). The result type
    // is set to Array<byte> in sema (ExprTypecheck_Calls.cpp, rcn == "CString"),
    // so the local picks up the array-backed disposal path (bz_array_free).
    {
        auto* recvTy = ctx_.exprType(*ma->target);
        // fix #2809 (v0.240): both TypeRef shapes — on a `c: CString` PARAMETER
        // receiver the declaration-only test declined and the call fell through
        // to codegen's "initializer failed to lower" internal degrade (#2434).
        bool isCStr = builtinTypeName(recvTy) == "CString" &&
            isFfiBufferTypeRef(recvTy);
        if (isCStr && ma->member == "ToBytes" && call.args.empty()) {
            llvm::Value* cstr = lowerExpr(*ma->target);
            if (!cstr) return nullptr;
            auto* fn = declareC("bz_cstring_to_bytes", ptrTy(), {ptrTy()});
            return builder_.CreateCall(fn, {cstr}, "cstr.tobytes");
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

            // stdlib §2.6 / §2.18 (#1989): ToString(format): string — the format
            // overload. "D" (dashed) / "N" (no hyphens) per the §2.18 Guid
            // vocabulary; any other specifier raises FormatException (§2.18
            // strict-fault, v0.197). bz_guid_to_string_fmt returns nullptr for an
            // unrecognized specifier — we then emitGuardThrow. This handles a
            // compile-time-literal AND a runtime format string uniformly, and mints
            // the same fresh rc=1 heap string the no-arg form does (so the result is
            // a +1 producer — see isPlusOneStringProducer). Mirrors the decimal
            // ToString(format) path above (bz_decimal_to_string_fmt) exactly.
            if (mname == "ToString" && call.args.size() == 1 && call.args[0].value) {
                llvm::Value* fmtVal = lowerExpr(*call.args[0].value);
                if (!fmtVal) return nullptr;
                auto* fn = declareC("bz_guid_to_string_fmt", ptrTy(),
                                    {ptrTy(), ptrTy()});
                llvm::Value* res =
                    builder_.CreateCall(fn, {slot, fmtVal}, "guid.tostr.fmt");
                llvm::Value* isNull = builder_.CreateICmpEQ(
                    res, llvm::Constant::getNullValue(ptrTy()), "guid.fmt.null");
                emitGuardThrow(isNull, "FormatException");
                return res;
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
            // per stdlib §2.6 / #1357: CompareTo(other: Guid): int — 16-byte lexicographic.
            if (mname == "CompareTo" && call.args.size() == 1 && call.args[0].value) {
                llvm::Value* otherVal = lowerExpr(*call.args[0].value);
                if (!otherVal) return nullptr;
                llvm::AllocaInst* otherSlot = entryAlloca(gst);
                if (otherVal->getType()->isPointerTy())
                    otherVal = builder_.CreateLoad(gst, otherVal, "guid.cmp.other.load");
                builder_.CreateStore(otherVal, otherSlot);
                auto* fn = declareC("bz_guid_compare", i32(), {ptrTy(), ptrTy()});
                return builder_.CreateCall(fn, {slot, otherSlot}, "guid.cmp");
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
            igrpRecvTy->headName() == "IGrouping") {
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
                    // fix #2938 / §22.3: see TryGetValue above.
                    llvm::Type* slotTy = nullptr;
                    std::vector<AccessorRefArg> outArgs;
                    llvm::Value* outAddr = resolveOutAddrWithBracket(
                        *call.args[0].value, slotTy, outArgs);
                    if (!outAddr) {
                        emitAccessorRefArgWriteBacks(outArgs, /*emit=*/false);
                        return builder_.getFalse();
                    }
                    auto* fn = declareC("bz_weak_try_upgrade", i32(),
                                        {ptrTy(), ptrTy()});
                    llvm::Value* r = builder_.CreateCall(fn, {handle, outAddr},
                                                         "weak.tryupgrade");
                    // fix #2938 / §22.3: flush any bracket on the normal-return edge.
                    emitAccessorRefArgWriteBacks(outArgs, /*emit=*/true);
                    return builder_.CreateICmpNE(r, builder_.getInt32(0),
                                                 "weak.upgraded");
                }
                // Dispose() is handled in the explicit-Dispose block; fall through.
            }
        }
    }

    // ---------------------------------------------------------------------------
    // §6.1 / §30.8: CancellationTokenSource instance methods and properties.
    // The receiver's exprType name-part is "CancellationTokenSource".
    // #1025: placed BEFORE the `if (!id) return nullptr` guard below — the
    // receiver of an instance call can be ANY expression (e.g. a chained
    // `source.Token` is a MemberAccessExpr, not an IdentifierExpr), and these
    // handlers dispatch on the receiver's exprType, not on an identifier name.
    // (Sibling instance handlers — Decimal/Guid/Shared/Weak above — are likewise
    // before the guard. The static-call handlers below DO need `id` for the
    // type-name receiver, which is why the guard stays where it is.)
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
                    // §6.1 (#1413 / #1378): CancelAfter(delay: TimeSpan) — schedule
                    // this source to cancel after `delay` on the dedicated BzTimer.
                    // TimeSpan lowers to an i64 tick count (100-ns units) → ms =
                    // ticks / 10000 (same conversion as Task.Delay(TimeSpan)).
                    if (mname == "CancelAfter" && call.args.size() == 1 &&
                        call.args[0].value) {
                        llvm::Value* span = lowerExpr(*call.args[0].value);
                        if (!span) return nullptr;
                        if (!span->getType()->isIntegerTy(64))
                            span = builder_.CreateSExtOrTrunc(span, i64());
                        llvm::Value* ms = builder_.CreateSDiv(
                            span, builder_.getInt64(10000), "cancelafter.ms");
                        auto* fn = declareC("bz_cts_cancel_after",
                            llvm::Type::getVoidTy(llvm_), {ptrTy(), i64()});
                        builder_.CreateCall(fn, {cts, ms});
                        return nullptr;  // unit
                    }
                    // §6.1: WithCancelAfter is a STATIC factory — handled in
                    // tryLowerStaticCall, not here.
                }
            }
        }
    }

    // ---------------------------------------------------------------------------
    // stdlib §6.10 / §28.14 (v0.175, #808): TaskScope instance methods.
    // The receiver's exprType name-part is "TaskScope".
    // ---------------------------------------------------------------------------
    {
        if (auto* ma2 = dynamic_cast<const ast::MemberAccessExpr*>(call.callee.get())) {
            if (ma2->target) {
                auto* recvTy = ctx_.exprType(*ma2->target);
                bool isTS = recvTy && recvTy->kind == sema::TypeRef::Kind::Named &&
                    builtinTypeName(recvTy) == "TaskScope";
                if (isTS) {
                    llvm::Value* sc = lowerExpr(*ma2->target);
                    if (!sc) return nullptr;
                    const auto& mname = ma2->member;
                    // §6.10: Token property (call-form mirror of the Exprs path).
                    if (mname == "Token" && call.args.empty()) {
                        auto* fn = declareC("bz_task_scope_token",
                                            ptrTy(), {ptrTy()});
                        return builder_.CreateCall(fn, {sc}, "tscope.token");
                    }
                    // §6.10: Cancel() — cancel all children now.
                    if (mname == "Cancel" && call.args.empty()) {
                        auto* fn = declareC("bz_task_scope_cancel",
                            llvm::Type::getVoidTy(llvm_), {ptrTy()});
                        builder_.CreateCall(fn, {sc});
                        return nullptr;  // unit
                    }
                    // §6.10: CancelAfter(delay: TimeSpan) — scope deadline.
                    // TimeSpan lowers to i64 ticks (100 ns) → ms (the same
                    // conversion as CTS.CancelAfter / Task.Delay).
                    if (mname == "CancelAfter" && call.args.size() == 1 &&
                        call.args[0].value) {
                        llvm::Value* span = lowerExpr(*call.args[0].value);
                        if (!span) return nullptr;
                        if (!span->getType()->isIntegerTy(64))
                            span = builder_.CreateSExtOrTrunc(span, i64());
                        llvm::Value* ms = builder_.CreateSDiv(
                            span, builder_.getInt64(10000), "tscope.ca.ms");
                        auto* fn = declareC("bz_task_scope_cancel_after",
                            llvm::Type::getVoidTy(llvm_), {ptrTy(), i64()});
                        builder_.CreateCall(fn, {sc, ms});
                        return nullptr;  // unit
                    }
                    // §6.10 / §28.14: Spawn(scoped body: () -> Task) — lower
                    // the callback like an event handler (a closure cell with
                    // a transferred +1; §28.14 admission was checked by the
                    // ownership pass), start the child immediately on the
                    // current domain, then check for the Spawn-after-close
                    // InvalidOperationException (the runtime raises it via
                    // bz_exc; emitPendingCheck turns it into a Bezel throw).
                    if (mname == "Spawn" && call.args.size() == 1 &&
                        call.args[0].value) {
                        llvm::Value* closure = lowerEventHandlerClosure(
                            call.args[0].value.get(), /*payload=*/nullptr);
                        if (!closure) return nullptr;
                        // fix #2334 / §28.14 / §28.9: lowerEventHandlerClosure
                        // -> lowerLambda just populated
                        // lastLambdaCapturedCells_ for any cellFreeEligible_
                        // captures IFF the argument is a lambda LITERAL on a
                        // `using var`/`await using var` TaskScope receiver
                        // (collectCellFreeEligible's TaskScope.Spawn special
                        // case, mirroring the IIFE arm's own consume call —
                        // LlvmBackend_Calls2.cpp). A method-reference argument
                        // does not go through lowerLambda and would leave the
                        // vector stale/empty from whatever lambda last ran
                        // elsewhere in this function — guard on the AST shape
                        // here, exactly like the IIFE arm, so this never
                        // consumes an unrelated lambda's cells. Consuming is a
                        // harmless no-op when the pre-scan did not flag
                        // anything (an ordinary capture-free Spawn lambda, or
                        // one whose receiver isn't a using-var local).
                        if (dynamic_cast<const ast::LambdaExpr*>(
                                call.args[0].value.get()))
                            consumeLastLambdaCapturedCells();
                        llvm::Value* ioeVt =
                            llvm::ConstantPointerNull::get(ptrTy());
                        {   auto vit = builtinVtable_.find(
                                "InvalidOperationException");
                            if (vit != builtinVtable_.end()) ioeVt = vit->second;
                        }
                        auto* fn = declareC("bz_task_scope_spawn",
                            llvm::Type::getVoidTy(llvm_),
                            {ptrTy(), ptrTy(), ptrTy()});
                        builder_.CreateCall(fn, {sc, closure, ioeVt});
                        emitPendingCheck();  // Spawn-after-close IOE (§6.10)
                        return nullptr;  // unit
                    }
                    // §6.10: await Join() — the sole re-raise site. Returns
                    // the join task; the standard await machinery consumes it
                    // (first fault re-raises in the awaiter).
                    if (mname == "Join" && call.args.empty()) {
                        auto* fn = declareC("bz_task_scope_join",
                                            ptrTy(), {ptrTy()});
                        return builder_.CreateCall(fn, {sc}, "tscope.join");
                    }
                    // §6.10: explicit Dispose() is rejected by the ownership
                    // pass (await-using only); defensively lower any residual
                    // occurrence to the non-blocking unwind dispose.
                    if (mname == "Dispose" && call.args.empty()) {
                        auto* fn = declareC("bz_task_scope_dispose_unwind",
                            llvm::Type::getVoidTy(llvm_), {ptrTy()});
                        builder_.CreateCall(fn, {sc});
                        return nullptr;  // unit
                    }
                }
            }
        }
    }

    // ---------------------------------------------------------------------------
    // §6.1 / §30.8: CancellationToken instance methods and properties.
    // The receiver's exprType name-part is "CancellationToken".
    // CancellationToken.None is handled in tryLowerStaticCall (member of type name).
    // #1025: also before the `if (!id)` guard — `source.Token.ThrowIfCancellation
    // Requested()` chains a MemberAccessExpr receiver and must reach this handler.
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
                                    {builder_.getInt64(1), builder_.getInt64(kExcObjSize)},
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
                    // §6.1 (#1413): Register(callback: () -> ()): owned Subscription.
                    // Lower the callback exactly like an event handler — a `() -> ()`
                    // closure (payload null) handed over with a transferred +1 — then
                    // install it on the token's cancel state (bz_ctoken_register) and
                    // return the Subscription handle.  Disposal routes through
                    // bz_subscription_dispose's cancel-callback branch.  On an already-
                    // cancelled token the runtime fires the callback synchronously and
                    // returns a no-op Subscription (§6.1); on None the callback never
                    // fires and its +1 is dropped, also yielding a no-op Subscription.
                    if (mname == "Register" && call.args.size() == 1 &&
                        call.args[0].value) {
                        llvm::Value* closure = lowerEventHandlerClosure(
                            call.args[0].value.get(), /*payload=*/nullptr);
                        if (!closure) return nullptr;
                        // fix #2334 / §28.14 / §6.1: lowerEventHandlerClosure
                        // -> lowerLambda just populated
                        // lastLambdaCapturedCells_ for any cellFreeEligible_
                        // captures IFF the argument is a lambda LITERAL on a
                        // `using var` / `await using var` CancellationToken
                        // receiver (collectCellFreeEligible's
                        // CancellationToken.Register special case, mirroring
                        // the TaskScope.Spawn arm's own consume above and the
                        // IIFE arm's consume in LlvmBackend_Calls2.cpp). A
                        // method-reference argument does not go through
                        // lowerLambda and would leave the vector stale/empty
                        // from whatever lambda last ran elsewhere in this
                        // function — guard on the AST shape here, exactly
                        // like the TaskScope.Spawn arm, so this never
                        // consumes an unrelated lambda's cells. Consuming is
                        // a harmless no-op when the pre-scan did not flag
                        // anything (an ordinary capture-free Register
                        // lambda, or one whose receiver isn't a using-var
                        // local).
                        if (dynamic_cast<const ast::LambdaExpr*>(
                                call.args[0].value.get()))
                            consumeLastLambdaCapturedCells();
                        llvm::Value* tokArg =
                            tok ? tok : llvm::ConstantPointerNull::get(ptrTy());
                        auto* fn = declareC("bz_ctoken_register",
                                            ptrTy(), {ptrTy(), ptrTy()});
                        return builder_.CreateCall(fn, {tokArg, closure},
                                                   "ctoken.register");
                    }
                }
            }
        }
    }

    auto* id = dynamic_cast<const ast::IdentifierExpr*>(ma->target.get());
    // #1026: do NOT return early when the receiver is not a bare identifier. The
    // static-call (cls-keyed) handlers below — CString.From, Math.Sqrt,
    // Convert.*, Environment.*, etc. — need an identifier-named class, but the
    // INSTANCE-method handlers further down (Stopwatch, DateTime, ...) dispatch
    // on the receiver's exprType and accept ANY receiver expression. A chained
    // receiver like `DateTime.UnixEpoch.AddDays(2)` is a MemberAccessExpr, not an
    // IdentifierExpr, so the old `if (!id) return nullptr` made every chained
    // instance call silently lower to a no-op (the receiver was not even
    // evaluated — wrong values, e.g. AddDays not applied). Instead, give the
    // static handlers an EMPTY `cls` (which matches no class name) when the
    // receiver is non-identifier so they all fall through to the instance
    // handlers. (No code after this point dereferences `id` except `cls`.)
    const std::string_view cls = id ? id->name : std::string_view{};
    const auto& m = ma->member;

    // stdlib §10.1/§10.2/§10.3 / §32.6 / fix #977: CString / WString / RawBytes
    // static factory methods. Each allocates a fresh malloc'd buffer and returns
    // it as a ptrTy() (the codegen representation of CString/WString/RawBytes).
    // At scope-end the buffer is freed by the `ffiBuffer` disposal path (free()).
    if (cls == "CString") {
        // CString.From(s: string): owned CString — null-terminated UTF-8 buffer
        // from a Bezel string. Reuses bz_string_to_utf8z, which allocates with
        // malloc and always produces a null-terminated UTF-8 byte sequence, exactly
        // the contract CString.From defines (stdlib §10.1).
        if (m == "From" && call.args.size() == 1 && call.args[0].value) {
            llvm::Value* s = lowerExpr(*call.args[0].value);
            if (!s) return nullptr;
            auto* fn = declareC("bz_string_to_utf8z", ptrTy(), {ptrTy()});
            return builder_.CreateCall(fn, {s}, "cstr.from");
        }
        // CString.FromBytes(bytes: Array<byte>): owned CString — wraps an
        // existing null-terminated byte array. The stdlib notes the caller is
        // responsible for ensuring the bytes are already null-terminated (§10.1).
        // We copy the byte-array data into a fresh malloc'd buffer with a
        // trailing null so the returned CString is independently owned.
        if (m == "FromBytes" && call.args.size() == 1 && call.args[0].value) {
            llvm::Value* arr =
                lowerByteArrayFactoryArg(*call.args[0].value);
            if (!arr) return nullptr;
            // Use bz_encoding_utf8_get_bytes' inverse: load the raw data pointer
            // (field 0 of bz_array_t is the elem_count, field 1 is the elements
            // pointer — same layout as struct { i64 count, i64 elem_size, ptr data }).
            // bz_array_t layout: [i64 count][i64 elem_size][ptr data]
            // We call a new runtime helper bz_cstring_from_bytes that copies
            // the array data + null-terminates it. For now lower to the direct
            // data-pointer path: load elem ptr (GEP index 2 of the i64/i64/ptr
            // struct) and duplicate it via strndup-style malloc + memcpy + null.
            // Simpler: declare bz_cstring_from_bytes and call it.
            auto* fn = declareC("bz_cstring_from_bytes", ptrTy(), {ptrTy()});
            return builder_.CreateCall(fn, {arr}, "cstr.frombytes");
        }
    }
    if (cls == "WString") {
        // WString.From(s: string): owned WString — null-terminated UTF-16 buffer
        // from a Bezel string. Uses bz_string_to_utf16z (allocates with malloc).
        if (m == "From" && call.args.size() == 1 && call.args[0].value) {
            llvm::Value* s = lowerExpr(*call.args[0].value);
            if (!s) return nullptr;
            auto* fn = declareC("bz_string_to_utf16z", ptrTy(), {ptrTy()});
            return builder_.CreateCall(fn, {s}, "wstr.from");
        }
    }
    if (cls == "RawBytes") {
        // RawBytes.From(buffer: Array<byte>): owned RawBytes — copy of the byte
        // array's data as an independently-owned heap buffer. Uses bz_rawbytes_from
        // which malloc-copies the array contents (no null-termination assumed).
        if (m == "From" && call.args.size() == 1 && call.args[0].value) {
            llvm::Value* arr =
                lowerByteArrayFactoryArg(*call.args[0].value);
            if (!arr) return nullptr;
            auto* fn = declareC("bz_rawbytes_from", ptrTy(), {ptrTy()});
            return builder_.CreateCall(fn, {arr}, "rawbytes.from");
        }
    }

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
            // Convert.ToInt32(s: string): int — §2.3 / #1238
            // #1238: replaced atoi (silent 0 on bad input) with strtoll +
            // endptr validation so invalid strings throw FormatException and
            // out-of-range strings throw OverflowException, matching §2.3 and
            // the int.Parse strict-fault contract (#1227).
            if (m == "ToInt32") {
                llvm::Value* arg = lowerExpr(*call.args[0].value);
                if (!arg) return nullptr;
                auto* argTy = ctx_.exprType(*call.args[0].value);
                // If it's already an int, return as-is.
                if (argTy && argTy->kind == sema::TypeRef::Kind::Primitive &&
                    argTy->primitive == sema::PrimitiveType::Int)
                    return arg;
                // For string: strtoll + endptr → FormatException on bad input,
                // OverflowException on out-of-int32-range (per §2.3 / #1227).
                llvm::Value* data = loadStringData(arg);
                auto* strtollC32 = declareC("strtoll", i64(), {ptrTy(), ptrTy(), i32()});
                auto* endSlotC32 = entryAlloca(ptrTy());
                llvm::Value* v64C32 = builder_.CreateCall(
                    strtollC32, {data, endSlotC32, builder_.getInt32(10)}, "cvt32.v");
                auto* i8C32 = llvm::Type::getInt8Ty(llvm_);
                llvm::Value* fcC32 = builder_.CreateLoad(i8C32, data, "cvt32.fc");
                llvm::Value* fcWsC32 = builder_.CreateICmpULE(
                    fcC32, llvm::ConstantInt::get(i8C32, ' '), "cvt32.fws");
                llvm::Value* epC32 = builder_.CreateLoad(ptrTy(), endSlotC32, "cvt32.ep");
                llvm::Value* advC32 = builder_.CreateICmpNE(epC32, data, "cvt32.adv");
                llvm::Value* echC32 = builder_.CreateLoad(i8C32, epC32, "cvt32.ech");
                llvm::Value* nulC32 = builder_.CreateICmpEQ(
                    echC32, llvm::ConstantInt::get(i8C32, 0), "cvt32.nul");
                llvm::Value* validC32 = builder_.CreateAnd(
                    builder_.CreateNot(fcWsC32),
                    builder_.CreateAnd(advC32, nulC32), "cvt32.vfmt");
                emitGuardThrow(builder_.CreateNot(validC32, "cvt32.fmtbad"),
                               "FormatException");
                llvm::Value* loC32 = builder_.CreateICmpSLT(
                    v64C32, llvm::ConstantInt::getSigned(i64(), INT32_MIN), "cvt32.lo");
                llvm::Value* hiC32 = builder_.CreateICmpSGT(
                    v64C32, llvm::ConstantInt::getSigned(i64(), INT32_MAX), "cvt32.hi");
                emitGuardThrow(builder_.CreateOr(loC32, hiC32, "cvt32.ovf"),
                               "OverflowException");
                return builder_.CreateTrunc(v64C32, i32());
            }
            // Convert.ToInt64(s: string): long — §2.3 / #1238
            // #1238: replaced atoll (silent 0 on bad input) with strtoll +
            // endptr validation → FormatException on invalid input.
            if (m == "ToInt64") {
                llvm::Value* arg = lowerExpr(*call.args[0].value);
                if (!arg) return nullptr;
                llvm::Value* data = loadStringData(arg);
                auto* strtollC64 = declareC("strtoll", i64(), {ptrTy(), ptrTy(), i32()});
                auto* endSlotC64 = entryAlloca(ptrTy());
                llvm::Value* v64C64 = builder_.CreateCall(
                    strtollC64, {data, endSlotC64, builder_.getInt32(10)}, "cvt64.v");
                auto* i8C64 = llvm::Type::getInt8Ty(llvm_);
                llvm::Value* fcC64 = builder_.CreateLoad(i8C64, data, "cvt64.fc");
                llvm::Value* fcWsC64 = builder_.CreateICmpULE(
                    fcC64, llvm::ConstantInt::get(i8C64, ' '), "cvt64.fws");
                llvm::Value* epC64 = builder_.CreateLoad(ptrTy(), endSlotC64, "cvt64.ep");
                llvm::Value* advC64 = builder_.CreateICmpNE(epC64, data, "cvt64.adv");
                llvm::Value* echC64 = builder_.CreateLoad(i8C64, epC64, "cvt64.ech");
                llvm::Value* nulC64 = builder_.CreateICmpEQ(
                    echC64, llvm::ConstantInt::get(i8C64, 0), "cvt64.nul");
                llvm::Value* validC64 = builder_.CreateAnd(
                    builder_.CreateNot(fcWsC64),
                    builder_.CreateAnd(advC64, nulC64), "cvt64.vfmt");
                emitGuardThrow(builder_.CreateNot(validC64, "cvt64.fmtbad"),
                               "FormatException");
                return v64C64;
            }
            // Convert.ToDouble(s: string): double — §2.3 / #1238
            // #1238: replaced atof (silent 0.0 on bad input) with strtod +
            // endptr validation → FormatException on invalid input.
            if (m == "ToDouble") {
                llvm::Value* arg = lowerExpr(*call.args[0].value);
                if (!arg) return nullptr;
                llvm::Value* data = loadStringData(arg);
                auto* strtodCd = declareC("strtod",
                    llvm::Type::getDoubleTy(llvm_), {ptrTy(), ptrTy()});
                auto* endSlotCd = entryAlloca(ptrTy());
                llvm::Value* dvCd = builder_.CreateCall(
                    strtodCd, {data, endSlotCd}, "cvtd.v");
                auto* i8Cd = llvm::Type::getInt8Ty(llvm_);
                llvm::Value* fcCd = builder_.CreateLoad(i8Cd, data, "cvtd.fc");
                llvm::Value* fcWsCd = builder_.CreateICmpULE(
                    fcCd, llvm::ConstantInt::get(i8Cd, ' '), "cvtd.fws");
                llvm::Value* epCd = builder_.CreateLoad(ptrTy(), endSlotCd, "cvtd.ep");
                llvm::Value* advCd = builder_.CreateICmpNE(epCd, data, "cvtd.adv");
                llvm::Value* echCd = builder_.CreateLoad(i8Cd, epCd, "cvtd.ech");
                llvm::Value* nulCd = builder_.CreateICmpEQ(
                    echCd, llvm::ConstantInt::get(i8Cd, 0), "cvtd.nul");
                llvm::Value* validCd = builder_.CreateAnd(
                    builder_.CreateNot(fcWsCd),
                    builder_.CreateAnd(advCd, nulCd), "cvtd.vfmt");
                emitGuardThrow(builder_.CreateNot(validCd, "cvtd.fmtbad"),
                               "FormatException");
                return dvCd;
            }
            // per stdlib §2.3 / #1356: Convert.ToDecimal(s: string): decimal
            // Delegates to bz_decimal_parse (same as Decimal.Parse).
            if (m == "ToDecimal") {
                llvm::Value* arg = lowerExpr(*call.args[0].value);
                if (!arg) return nullptr;
                llvm::StructType* dst = decimalStructType();
                llvm::AllocaInst* slot = entryAlloca(dst);
                auto getOVE = [&]() -> llvm::Value* {
                    auto vit = builtinVtable_.find("OverflowException");
                    if (vit == builtinVtable_.end())
                        return llvm::ConstantPointerNull::get(ptrTy());
                    return vit->second;
                };
                auto getFE = [&]() -> llvm::Value* {
                    auto vit = builtinVtable_.find("FormatException");
                    if (vit == builtinVtable_.end())
                        return llvm::ConstantPointerNull::get(ptrTy());
                    return vit->second;
                };
                auto* fn = declareC("bz_decimal_parse", llvm::Type::getVoidTy(llvm_),
                                    {ptrTy(), ptrTy(), ptrTy(), ptrTy()});
                builder_.CreateCall(fn, {arg, slot, getOVE(), getFE()});
                emitPendingCheck();
                return builder_.CreateLoad(dst, slot, "cvtdec.val");
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
        // Returns true and sets result when `value` is a fully-valid integer in
        // [INT32_MIN, INT32_MAX]; false and sets result=0 otherwise.  Mirrors
        // int.TryParse strtoll-with-endptr + range-check + whitespace-reject
        // (#1238 / #1227 / §9.1).
        if (m == "TryToInt32" && call.args.size() == 2 &&
            call.args[0].value && call.args[1].value) {
            llvm::Value* strVal = lowerExpr(*call.args[0].value);
            if (!strVal) return nullptr;
            llvm::Value* strData = loadStringData(strVal);
            // Address of the out parameter (slot pre-created by ensureOutVarSlots).
            // fix #2938 / §22.3: see TryGetValue in tryLowerBuiltinClassCall.
            llvm::Type* et = nullptr;
            std::vector<AccessorRefArg> outArgs;
            llvm::Value* outAddr = resolveOutAddrWithBracket(
                *call.args[1].value, et, outArgs);
            if (!outAddr) {
                emitAccessorRefArgWriteBacks(outArgs, /*emit=*/false);
                return nullptr;
            }
            // #1238: use strtoll (not strtol) for unambiguous 64-bit result so
            // we can range-check to [INT32_MIN, INT32_MAX] before truncating.
            auto* strtollTry = declareC("strtoll", i64(), {ptrTy(), ptrTy(), i32()});
            auto* endSlotT = entryAlloca(ptrTy());
            llvm::Value* val64 = builder_.CreateCall(
                strtollTry, {strData, endSlotT, builder_.getInt32(10)}, "tryc.val");
            llvm::Value* endPtr = builder_.CreateLoad(ptrTy(), endSlotT, "tryc.ep");
            // ok1 = first char not whitespace AND (endPtr advanced) AND (*endPtr == '\0')
            auto* i8t = llvm::Type::getInt8Ty(llvm_);
            llvm::Value* fc = builder_.CreateLoad(i8t, strData, "tryc.fc");
            llvm::Value* fcWs = builder_.CreateICmpULE(
                fc, llvm::ConstantInt::get(i8t, ' '), "tryc.fws");
            llvm::Value* advanced = builder_.CreateICmpNE(endPtr, strData, "tryc.adv");
            llvm::Value* endCh = builder_.CreateLoad(i8t, endPtr, "tryc.end");
            llvm::Value* atNul = builder_.CreateICmpEQ(
                endCh, llvm::ConstantInt::get(i8t, 0), "tryc.nul");
            llvm::Value* ok = builder_.CreateAnd(
                builder_.CreateNot(fcWs),
                builder_.CreateAnd(advanced, atNul), "tryc.ok1");
            // #1238 / §2.3: range-check to int32 bounds → ok=false for overflow.
            llvm::Value* inRange = builder_.CreateAnd(
                builder_.CreateICmpSGE(
                    val64, llvm::ConstantInt::getSigned(i64(), INT32_MIN), "tryc.lo"),
                builder_.CreateICmpSLE(
                    val64, llvm::ConstantInt::getSigned(i64(), INT32_MAX), "tryc.hi"),
                "tryc.inr");
            ok = builder_.CreateAnd(ok, inRange, "tryc.ok2");
            // Store truncated value on success, 0 on failure (default value per §2.3).
            llvm::Value* trunc32 = builder_.CreateTrunc(val64, i32());
            llvm::Value* sel32 = builder_.CreateSelect(ok, trunc32, builder_.getInt32(0), "tryc.sel");
            builder_.CreateStore(sel32, outAddr);
            // fix #2938 / §22.3: flush any bracket on the normal-return edge.
            emitAccessorRefArgWriteBacks(outArgs, /*emit=*/true);
            return ok;
        }
        // per stdlib §2.3 / #1356: Convert.TryToInt64(value: string, out result: long): bool.
        if (m == "TryToInt64" && call.args.size() == 2 &&
            call.args[0].value && call.args[1].value) {
            llvm::Value* strVal = lowerExpr(*call.args[0].value);
            if (!strVal) return nullptr;
            llvm::Value* strData = loadStringData(strVal);
            // fix #2938 / §22.3: see Convert.TryToInt32 above.
            llvm::Type* et64 = nullptr;
            std::vector<AccessorRefArg> outArgs;
            llvm::Value* outAddr64 = resolveOutAddrWithBracket(
                *call.args[1].value, et64, outArgs);
            if (!outAddr64) {
                emitAccessorRefArgWriteBacks(outArgs, /*emit=*/false);
                return nullptr;
            }
            auto* strtollT64 = declareC("strtoll", i64(), {ptrTy(), ptrTy(), i32()});
            auto* endSlotT64 = entryAlloca(ptrTy());
            llvm::Value* val64_t = builder_.CreateCall(
                strtollT64, {strData, endSlotT64, builder_.getInt32(10)}, "tryc64.val");
            llvm::Value* endPtr64 = builder_.CreateLoad(ptrTy(), endSlotT64, "tryc64.ep");
            auto* i8t64 = llvm::Type::getInt8Ty(llvm_);
            llvm::Value* fc64 = builder_.CreateLoad(i8t64, strData, "tryc64.fc");
            llvm::Value* fcWs64 = builder_.CreateICmpULE(
                fc64, llvm::ConstantInt::get(i8t64, ' '), "tryc64.fws");
            llvm::Value* adv64 = builder_.CreateICmpNE(endPtr64, strData, "tryc64.adv");
            llvm::Value* endCh64 = builder_.CreateLoad(i8t64, endPtr64, "tryc64.end");
            llvm::Value* atNul64 = builder_.CreateICmpEQ(
                endCh64, llvm::ConstantInt::get(i8t64, 0), "tryc64.nul");
            llvm::Value* ok64 = builder_.CreateAnd(
                builder_.CreateNot(fcWs64),
                builder_.CreateAnd(adv64, atNul64), "tryc64.ok");
            llvm::Value* sel64 = builder_.CreateSelect(
                ok64, val64_t, builder_.getInt64(0), "tryc64.sel");
            builder_.CreateStore(sel64, outAddr64);
            // fix #2938 / §22.3: flush any bracket on the normal-return edge.
            emitAccessorRefArgWriteBacks(outArgs, /*emit=*/true);
            return ok64;
        }
        // per stdlib §2.3 / #1356: Convert.TryToDouble(value: string, out result: double): bool.
        if (m == "TryToDouble" && call.args.size() == 2 &&
            call.args[0].value && call.args[1].value) {
            llvm::Value* strVal = lowerExpr(*call.args[0].value);
            if (!strVal) return nullptr;
            llvm::Value* strData = loadStringData(strVal);
            // fix #2938 / §22.3: see Convert.TryToInt32 above.
            llvm::Type* etd = nullptr;
            std::vector<AccessorRefArg> outArgs;
            llvm::Value* outAddrd = resolveOutAddrWithBracket(
                *call.args[1].value, etd, outArgs);
            if (!outAddrd) {
                emitAccessorRefArgWriteBacks(outArgs, /*emit=*/false);
                return nullptr;
            }
            auto* strtodTr = declareC("strtod",
                llvm::Type::getDoubleTy(llvm_), {ptrTy(), ptrTy()});
            auto* endSlotTd = entryAlloca(ptrTy());
            llvm::Value* dvTd = builder_.CreateCall(
                strtodTr, {strData, endSlotTd}, "tryd.val");
            llvm::Value* endPtrTd = builder_.CreateLoad(ptrTy(), endSlotTd, "tryd.ep");
            auto* i8td = llvm::Type::getInt8Ty(llvm_);
            llvm::Value* fcTd = builder_.CreateLoad(i8td, strData, "tryd.fc");
            llvm::Value* fcWsTd = builder_.CreateICmpULE(
                fcTd, llvm::ConstantInt::get(i8td, ' '), "tryd.fws");
            llvm::Value* advTd = builder_.CreateICmpNE(endPtrTd, strData, "tryd.adv");
            llvm::Value* endChTd = builder_.CreateLoad(i8td, endPtrTd, "tryd.end");
            llvm::Value* atNulTd = builder_.CreateICmpEQ(
                endChTd, llvm::ConstantInt::get(i8td, 0), "tryd.nul");
            llvm::Value* okTd = builder_.CreateAnd(
                builder_.CreateNot(fcWsTd),
                builder_.CreateAnd(advTd, atNulTd), "tryd.ok");
            llvm::Value* selTd = builder_.CreateSelect(
                okTd, dvTd,
                llvm::ConstantFP::get(llvm::Type::getDoubleTy(llvm_), 0.0), "tryd.sel");
            builder_.CreateStore(selTd, outAddrd);
            // fix #2938 / §22.3: flush any bracket on the normal-return edge.
            emitAccessorRefArgWriteBacks(outArgs, /*emit=*/true);
            return okTd;
        }
        // per stdlib §2.3 / #1356: Convert.TryToDecimal(value: string, out result: decimal): bool.
        if (m == "TryToDecimal" && call.args.size() == 2 &&
            call.args[0].value && call.args[1].value) {
            llvm::Value* strVal = lowerExpr(*call.args[0].value);
            if (!strVal) return nullptr;
            llvm::StructType* dst = decimalStructType();
            llvm::AllocaInst* slotDec = entryAlloca(dst);
            auto* fn = declareC("bz_decimal_try_parse", i32(), {ptrTy(), ptrTy()});
            llvm::Value* okDec = builder_.CreateCall(fn, {strVal, slotDec}, "trydec.ok");
            // fix #2938 / §22.3: see Convert.TryToInt32 above. Note the
            // existing branch was `if (outAddrdec) { ... }` (the value
            // assignment was conditional on outAddrdec being non-null,
            // so a null lvalueAddr silently dropped the write). With the
            // bracket, the slot is always present — only the writeback
            // gating changes — so the assignment runs unconditionally.
            llvm::Type* etdec = nullptr;
            std::vector<AccessorRefArg> outArgs;
            llvm::Value* outAddrdec = resolveOutAddrWithBracket(
                *call.args[1].value, etdec, outArgs);
            if (!outAddrdec) {
                emitAccessorRefArgWriteBacks(outArgs, /*emit=*/false);
                return nullptr;
            }
            if (outAddrdec) {
                llvm::Value* resultDec = builder_.CreateLoad(dst, slotDec, "trydec.val");
                builder_.CreateStore(resultDec, outAddrdec);
            }
            // fix #2938 / §22.3: flush any bracket on the normal-return edge.
            emitAccessorRefArgWriteBacks(outArgs, /*emit=*/true);
            return builder_.CreateICmpNE(okDec, builder_.getInt32(0), "trydec.bool");
        }
        // per stdlib §2.3 / #1356: Convert.TryToBoolean(value: string, out result: bool): bool.
        if (m == "TryToBoolean" && call.args.size() == 2 &&
            call.args[0].value && call.args[1].value) {
            llvm::Value* strVal = lowerExpr(*call.args[0].value);
            if (!strVal) return nullptr;
            llvm::Value* strData = loadStringData(strVal);
            auto* strcmpFnTB = module_->getFunction("strcasecmp");
            if (!strcmpFnTB) {
                auto* ft = llvm::FunctionType::get(i32(), {ptrTy(), ptrTy()}, false);
                strcmpFnTB = llvm::Function::Create(ft, llvm::Function::ExternalLinkage,
                                                    "strcasecmp", module_.get());
            }
            llvm::Constant* trueStrTB = llvm::ConstantDataArray::getString(llvm_, "true", true);
            unsigned tidTB = stringLiteralIdx_++;
            auto* trueGVTB = new llvm::GlobalVariable(*module_, trueStrTB->getType(), true,
                llvm::GlobalValue::PrivateLinkage, trueStrTB, ".bz.tb.true." + std::to_string(tidTB));
            llvm::Value* cmpTrueTB = builder_.CreateICmpEQ(
                builder_.CreateCall(strcmpFnTB, {strData, trueGVTB}), builder_.getInt32(0));
            llvm::Constant* oneStrTB = llvm::ConstantDataArray::getString(llvm_, "1", true);
            auto* oneGVTB = new llvm::GlobalVariable(*module_, oneStrTB->getType(), true,
                llvm::GlobalValue::PrivateLinkage, oneStrTB, ".bz.tb.one." + std::to_string(tidTB));
            llvm::Value* cmpOneTB = builder_.CreateICmpEQ(
                builder_.CreateCall(strcmpFnTB, {strData, oneGVTB}), builder_.getInt32(0));
            llvm::Value* boolResult = builder_.CreateOr(cmpTrueTB, cmpOneTB, "tryb.res");
            // fix #2938 / §22.3: see Convert.TryToInt32 above. Same pattern as
            // TryToDecimal: the existing branch was `if (outAddrb)` (the value
            // assignment was conditional on outAddrb being non-null), so a null
            // lvalueAddr silently dropped the write. With the bracket, the slot
            // is always present — only the writeback gating changes — so the
            // assignment runs unconditionally.
            llvm::Type* etb = nullptr;
            std::vector<AccessorRefArg> outArgs;
            llvm::Value* outAddrb = resolveOutAddrWithBracket(
                *call.args[1].value, etb, outArgs);
            if (!outAddrb) {
                emitAccessorRefArgWriteBacks(outArgs, /*emit=*/false);
                // (return nullptr would skip the ok computation below; the
                // pre-existing site returned okTB regardless.)
                return nullptr;
            }
            builder_.CreateStore(boolResult, outAddrb);
            // TryToBoolean always succeeds for "true"/"1"/"false"/"0" input; if neither
            // matches we return false and store false — mirrors Convert.ToBoolean contract.
            llvm::Constant* falseStrTB = llvm::ConstantDataArray::getString(llvm_, "false", true);
            auto* falseGVTB = new llvm::GlobalVariable(*module_, falseStrTB->getType(), true,
                llvm::GlobalValue::PrivateLinkage, falseStrTB, ".bz.tb.false." + std::to_string(tidTB));
            llvm::Value* cmpFalseTB = builder_.CreateICmpEQ(
                builder_.CreateCall(strcmpFnTB, {strData, falseGVTB}), builder_.getInt32(0));
            llvm::Constant* zeroStrTB = llvm::ConstantDataArray::getString(llvm_, "0", true);
            auto* zeroGVTB = new llvm::GlobalVariable(*module_, zeroStrTB->getType(), true,
                llvm::GlobalValue::PrivateLinkage, zeroStrTB, ".bz.tb.zero." + std::to_string(tidTB));
            llvm::Value* cmpZeroTB = builder_.CreateICmpEQ(
                builder_.CreateCall(strcmpFnTB, {strData, zeroGVTB}), builder_.getInt32(0));
            // success = matched any of true/1/false/0
            llvm::Value* okTB = builder_.CreateOr(
                builder_.CreateOr(cmpTrueTB, cmpOneTB),
                builder_.CreateOr(cmpFalseTB, cmpZeroTB), "tryb.ok");
            // fix #2938 / §22.3: flush any bracket on the normal-return edge.
            emitAccessorRefArgWriteBacks(outArgs, /*emit=*/true);
            return okTB;
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

    // §9.6: string.Join(separator, items: IEnumerable<string>): string
    if ((cls == "string" || cls == "String") && m == "Join" &&
        call.args.size() == 2 && call.args[0].value && call.args[1].value) {
        llvm::Value* sep = lowerExpr(*call.args[0].value);
        if (!sep) return nullptr;
        // per stdlib §9.6 (#1182): `items` is an IEnumerable<string>, NOT just a
        // concrete Array<string>. A lazy LINQ query (`.Where(...)`/`.Select(...)`),
        // a vtable-backed IEnumerable, a LinkedList/Set/Range — none of these
        // lower to a bz_array_t*; lowering one and then indexing it below with
        // bz_array_ptr_at dereferences a query/state-machine/node-list pointer as
        // if it were an array → SIGSEGV (#1182). Materialize ANY source uniformly
        // into a fresh bz_array_t* snapshot via the shared lazy-enumerator path
        // (the same one `ToArray()` and `for-in` use): lowerToLazyEnumerator wraps
        // a concrete Array/Stack/Queue, a dict-backed Set, a LinkedList, a Range,
        // or a vtable IEnumerable, and bz_lazy_to_array drains it to a contiguous
        // pointer array of the element strings. The snapshot is freed at the
        // return site below with a NULL element disposer — the join only borrows
        // the element strings (it copies their bytes), it does not own them, so
        // there is no element release, no leak, no double-free. Elements are
        // always strings (IEnumerable<string>): one pointer (8 bytes) each — the
        // same element width the loop below indexes with.
        llvm::Value* matTemp = nullptr;
        llvm::Value* arr = nullptr;
        {
            sema::TypeRef* itElem = nullptr;
            llvm::Value* lazy = lowerToLazyEnumerator(*call.args[1].value, itElem);
            if (lazy) {
                auto* toArr =
                    declareC("bz_lazy_to_array", ptrTy(), {ptrTy(), i64()});
                arr = builder_.CreateCall(
                    toArr, {lazy, builder_.getInt64(8)}, "join.items");
                matTemp = arr;
            } else {
                // Not an IEnumerable source the lazy path recognizes — lower it
                // directly (prior behavior; no worse than before for this case).
                arr = lowerExpr(*call.args[1].value);
            }
        }
        if (!arr) return nullptr;
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
        // per stdlib §9.6 (#1182): reclaim the materialized snapshot AFTER the
        // joined string is fully built. NULL element disposer: the join borrowed
        // the element strings (copied their bytes) — frees only the snapshot's
        // header + buffer, never the strings (no leak, no double-free). A no-op
        // when the items argument was lowered directly (matTemp == null).
        if (matTemp) {
            auto* freeFn = declareC("bz_array_free",
                                    llvm::Type::getVoidTy(llvm_), {ptrTy(), ptrTy()});
            builder_.CreateCall(
                freeFn, {matTemp, llvm::ConstantPointerNull::get(ptrTy())});
        }
        return strObj;
    }

    if (cls == "Math") {
        // #1141 / stdlib §1: Math.Min/Max/Clamp are declared ONLY as the generic
        // `Min<T>(a, b): T where T: IComparable<T>` (no numeric overload). For a
        // non-numeric value-type argument (a record/enum that implements
        // `IComparable<T>`, §14.2) the comparison must dispatch through the
        // type's `CompareTo`, not the numeric sitofp-to-double path (which emits
        // `sitofp %record … to double` → malformed IR). `comparableCompareTo`
        // returns the operand type's single-parameter `CompareTo` method when the
        // operand is such a value type, else null (numeric path handles int/
        // double); `emitCompareToCmp` lowers `a.CompareTo(b)` to its int result,
        // spilling a record receiver to a stack slot (records pass `this` by
        // pointer — CG29). Enums lower as their integer ordinal and never reach
        // here (they take the integer min/max branch).
        auto comparableCompareTo =
            [&](const ast::Expr* e) -> sema::MethodSymbol* {
            const sema::TypeRef* t = e ? ctx_.exprType(*e) : nullptr;
            // #1215 / §14.2: nullable types do not satisfy IComparable<T> —
            // sema already rejects them; guard here defensively so a nullable
            // pointer is never passed as a CompareTo receiver (null deref).
            if (t && !t->nullable &&
                t->kind == sema::TypeRef::Kind::Named && t->declaration &&
                (t->declaration->kind == ast::TypeDeclKind::Record ||
                 t->declaration->kind == ast::TypeDeclKind::Enum ||
                 // #1148: a CLASS that implements IComparable<T> (§14.2 / stdlib §2.2).
                 // Classes are pointer-typed; the CompareTo receiver needs no spill
                 // (emitCompareToCmp only spills for record value receivers). Virtual
                 // dispatch is handled by emitMethodCall: if CompareTo is in vtableSlot_
                 // (override/virtual) it dispatches through the vtable; otherwise the
                 // concrete class's compiled body is called directly.
                 t->declaration->kind == ast::TypeDeclKind::Class)) {
                if (sema::MethodSymbol* cm =
                        findMethodByName(t->declaration, "CompareTo"))
                    if (cm->params.size() == 1) return cm;
            }
            // fix #2782 (stdlib §2.2, spec §14.2, v0.240): an INTERFACE
            // argument was omitted from the kind test above, so this returned
            // null, the `Math.Min`/`Max`/`Clamp` arms fell through to the
            // float path, and the call degraded to a ZERO/NULL result with
            // only the #2491 internal Warning — the build SUCCEEDED and the
            // program silently computed the wrong answer. That is strictly
            // worse than #2770's ICE on the sibling `Array<T>.Min()` shape:
            // a stopped build cannot ship, a wrong value can.
            //
            // stdlib §2.2 declares
            //     public static method Min<T>(a: T, b: T): T
            //                                     where T: IComparable<T>;
            // and spec §14.2 conformance rule 3 admits an interface
            // substitution — "**T is an interface** that has I in its
            // extension chain" — so this is a document-admissible shape that
            // must compute the user's ordering.
            //
            // Selected by the exact §2.2 constraint (selfComparableIfaceCompareTo,
            // shared with #2768/#2770), from the STATIC argument type, never by
            // a runtime word test — so no other category can be mis-routed in,
            // and `string` is structurally excluded (stdlib §9.6 names no
            // `IComparable<Self>`). Tried only after the kind test above
            // returns nothing, so every shape that works today is unchanged.
            if (sema::MethodSymbol* ifm = selfComparableIfaceCompareTo(t))
                return ifm;
            return nullptr;
        };
        // fix #2785 (stdlib §2.2 / §9.6, spec §4.6, v0.240): is this argument a
        // non-null `string`? stdlib §9.6 declares
        //     public class String: IEnumerable<char>, IComparable<string>,
        //                          IEquatable<string>
        // so string satisfies §2.2's `where T: IComparable<T>` and
        // `Math.Min("b", "a")` is document-admissible. It was not admitted by
        // `comparableCompareTo` (string is none of Record/Enum/Class and has no
        // MethodSymbol to return), so the Min/Max/Clamp arms fell through to
        // the FLOAT path and `CreateSIToFP` was handed a POINTER — the compiler
        // ABORTED with "SIToFP source must be integer or integer vector".
        //
        // A NULLABLE `string?` TypeRef is accepted here as well, and that is
        // deliberate rather than an oversight: §7.4 narrowing does NOT rewrite
        // the variable's exprType (ExprTypecheck_impl.hpp:146 — "the declared
        // (nullable) type is preserved so codegen's nullable-value layout logic
        // … stays correct"), so a `string?` PROVEN non-null by `if p <> null`
        // still presents as nullable here. Requiring non-null would abort the
        // compiler on legal narrowed code. Safety comes from the paired sema
        // gate, which rejects an UN-narrowed `string?` argument outright, and
        // from `emitStrCmp`, which fails closed unless both operands lowered to
        // pointers — `string?` shares `string`'s plain-pointer representation
        // (verified by measuring the narrowed shape end-to-end, not assumed).
        auto isStringArg = [&](const ast::Expr* e) -> bool {
            const sema::TypeRef* t = e ? ctx_.exprType(*e) : nullptr;
            return t && t->kind == sema::TypeRef::Kind::Primitive &&
                   t->primitive == sema::PrimitiveType::String;
        };
        // fix #2785: ORDINAL comparison on the two data pointers — the same
        // lowering `Array<string>.Min()` already uses (tryLowerCollectionInstanceCall's
        // `isStrElem` arm), so `Math.Min` and the collection form agree by
        // construction. stdlib §9.6 fixes string ordering as ordinal.
        auto emitStrCmp = [&](llvm::Value* a, llvm::Value* b) -> llvm::Value* {
            if (!a || !b || !a->getType()->isPointerTy() ||
                !b->getType()->isPointerTy())
                return nullptr;
            llvm::Value* aData = builder_.CreateLoad(
                ptrTy(), builder_.CreateStructGEP(stringType(), a, 0));
            llvm::Value* bData = builder_.CreateLoad(
                ptrTy(), builder_.CreateStructGEP(stringType(), b, 0));
            auto* strcmpFn = module_->getFunction("strcmp");
            if (!strcmpFn) {
                auto* ft =
                    llvm::FunctionType::get(i32(), {ptrTy(), ptrTy()}, false);
                strcmpFn = llvm::Function::Create(
                    ft, llvm::Function::ExternalLinkage, "strcmp",
                    module_.get());
            }
            return builder_.CreateCall(strcmpFn, {aData, bData}, "mm.strcmp");
        };
        // fix #2786 (stdlib §2.2, spec §14.2, v0.240): the float fallback below
        // is reached by anything the integer, CompareTo (#1141/#1148/#2782) and
        // string (#2785) arms all declined. It immediately does
        // `CreateSIToFP(v, double)`, which ABORTS the compiler outright on a
        // POINTER operand ("SIToFP source must be integer or integer vector").
        //
        // This guard is deliberately CATEGORY-BLIND — it tests the lowered LLVM
        // type rather than enumerating the source categories that may reach it.
        // Enumerating is what produced this family in the first place: the kind
        // tests listed Record|Enum|Class and were then found to be missing
        // Interface (#2782), string (#2785), and now the builtin interfaces.
        // Any operand that is not numeric cannot satisfy §2.2's
        // `where T: IComparable<T>` in a form this lowering can compare, so the
        // honest answer is a spec-cited Error, not an assertion failure.
        //
        // The known reacher is a BUILTIN interface argument (`IDisposable`).
        // ROOT CAUSE, recorded here because the guard only contains it: the
        // whitelist at TypeResolution_Names.cpp:1511 attaches a real synthesized
        // Interface TypeSymbol to `IComparable`/`IEquatable`/`IEqualityComparer`/
        // `IReadOnlySpan`/`INumber`/`ISteppable`/`IParsable` — and NOT to
        // `IDisposable`, whose TypeRefs therefore carry no `declaration` at all.
        // Every gate in this family keys on `declaration`, which is why the same
        // blind spot shows up as #2784 (the §5.1 gate bypassed for
        // `Array<IDisposable>`) and #2766 (no `resolvedDeclType`, so a thunk
        // signature fell back to an i32 placeholder). Adding IDisposable to that
        // whitelist would close all three at the source, but it changes the
        // symbol representation of the type the whole §28 disposal machinery is
        // built on — out of scope for a bug fix, filed instead.
        auto rejectNonNumericMathOperand = [&](llvm::Value* v) -> bool {
            if (!v) return false;
            if (v->getType()->isIntegerTy() || v->getType()->isFloatingPointTy())
                return false;
            // ⚠ The message must NOT claim the argument type HAS no ordering.
            // It reaches here whenever this lowering could not find one, and
            // those are different claims: `decimal` also lands here (measured —
            // it aborted identically before this guard), yet stdlib §4.4 gives
            // it a perfectly good ordering that Math simply has no arm for.
            // Filed as its own defect; an inaccurate diagnostic would be a poor
            // trade for an assertion failure.
            ctx_.diagnostics().report(
                Severity::Error, call.span.begin,
                "`Math." + std::string(m) +
                "` could not lower a comparison for this argument type "
                "(stdlib §2.2 requires `where T: IComparable<T>`): it is not a "
                "numeric primitive this intrinsic handles, not `string`, and "
                "exposes no `CompareTo` reachable from here. (#2786)");
            return true;
        };
        // fix #2788 (stdlib §2.2 / §4.4, spec §4.4, v0.240): a `decimal`
        // argument. decimal is a numeric primitive with a total order, so §2.2's
        // `where T: IComparable<T>` admits it — but it lowers to a STRUCT, not
        // an LLVM integer or float, so it too fell into the float fallback and
        // aborted on `CreateSIToFP` (measured, identical to #2785's string
        // abort). This is the fourth category this family was found to be
        // missing, after Interface (#2782), string (#2785) and the builtin
        // interfaces (#2786).
        //
        // Reuses the runtime comparison every other decimal ordering already
        // uses — `bz_decimal_cmp` on two struct slots, returning -1/0/+1, the
        // same path a plain `x < 2m` takes (LlvmBackend_Exprs2.cpp:7431's
        // decCmp). So Math.Min and the `<` operator agree by construction.
        auto isDecimalArg = [&](const ast::Expr* e) -> bool {
            const sema::TypeRef* t = e ? ctx_.exprType(*e) : nullptr;
            return t && !t->nullable &&
                   t->kind == sema::TypeRef::Kind::Primitive &&
                   t->primitive == sema::PrimitiveType::Decimal;
        };
        // Fails CLOSED: a null result falls through to #2786's guard, which
        // reports a spec-cited Error, rather than to the aborting SIToFP.
        auto emitDecCmp = [&](llvm::Value* a, llvm::Value* b) -> llvm::Value* {
            llvm::StructType* dst = decimalStructType();
            if (!a || !b || !dst || a->getType() != dst || b->getType() != dst)
                return nullptr;
            llvm::AllocaInst* aSlot = entryAlloca(dst);
            llvm::AllocaInst* bSlot = entryAlloca(dst);
            builder_.CreateStore(a, aSlot);
            builder_.CreateStore(b, bSlot);
            auto* cmpFn = declareC("bz_decimal_cmp", i32(), {ptrTy(), ptrTy()});
            return builder_.CreateCall(cmpFn, {aSlot, bSlot}, "mm.deccmp");
        };
        auto emitCompareToCmp = [&](sema::MethodSymbol* cmpMs, llvm::Value* a,
                                    llvm::Value* b,
                                    const ast::Expr* recvExpr) -> llvm::Value* {
            // fix #2782: an interface receiver's `CompareTo` is declared on
            // `IComparable` and has NO compiled body, so emitMethodCall below
            // cannot reach it. Dispatch through #2768's `bz$ifacecmp$<Iface>`
            // thunk instead — the same §13 itable dispatch a written-out
            // `a.CompareTo(b)` uses. The thunk's ABI is two element-SLOT
            // addresses, and here both operands are values, so each spills to
            // one entry alloca (exactly as #2770's selector site does).
            // Fail-CLOSED: any leg that cannot be established returns null,
            // which the callers propagate as "not lowered" rather than
            // silently selecting an operand.
            if (cmpMs && cmpMs->parent &&
                cmpMs->parent->kind == ast::TypeDeclKind::Interface) {
                const sema::TypeRef* rt =
                    recvExpr ? ctx_.exprType(*recvExpr) : nullptr;
                if (!rt || !rt->declaration) return nullptr;
                if (!a || !b || !a->getType()->isPointerTy() ||
                    !b->getType()->isPointerTy())
                    return nullptr;
                llvm::Function* thunk =
                    ensureIfaceCmpThunk(rt->declaration, cmpMs);
                if (!thunk) return nullptr;
                llvm::Value* aSlot = entryAlloca(ptrTy());
                llvm::Value* bSlot = entryAlloca(ptrTy());
                builder_.CreateStore(a, aSlot);
                builder_.CreateStore(b, bSlot);
                return builder_.CreateCall(thunk, {aSlot, bSlot}, "cmp.iface");
            }
            // fix #1858 / §5 / §11 / §14.2: for a GENERIC-record IComparable
            // instantiation (`Box<string>`), install the per-instantiation
            // generic substitution from the receiver's concrete type arguments
            // BEFORE the emitMethodCall — exactly as a normal `x.CompareTo(y)`
            // method call does in lowerMethodCall. Without it, genericSubst_ is
            // empty at this intrinsic call site, so emitMethodCall falls through
            // to a DIRECT call of the UNSPECIALIZED template `bz$Box$CompareTo`
            // (param `%record.Box` = {i32,i32}) while passing the SPECIALIZED
            // `%record.Box$spec0` receiver/argument — a "Call parameter type
            // does not match function signature" IR-verification failure. With
            // the substitution active, emitMethodCall routes through the
            // parentIsGeneric block → emitInlineGenericMethod, which
            // monomorphizes CompareTo for the concrete instantiation. A no-op
            // for a non-generic record (no type params to bind).
            auto savedSubst = genericSubst_;
            auto savedSubstSema = genericSubstSemaTy_;
            bool substInstalled = false;
            if (recvExpr && cmpMs->parent &&
                !cmpMs->parent->genericParams.empty()) {
                sema::TypeRef* recvTy = ctx_.exprType(*recvExpr);
                if (recvTy && recvTy->kind == sema::TypeRef::Kind::Named &&
                    recvTy->declaration == cmpMs->parent &&
                    !recvTy->genericArgs.empty()) {
                    // §14.1 / #783: take the type-parameter NAMES from the AST
                    // decl when present (local generic), else from the imported
                    // TypeSymbol's genericParams (cross-module).
                    // fix #3143: through the one shared walk.
                    const std::vector<std::string_view> gpNames =
                        sema::genericParamNames(cmpMs->parent);
                    if (gpNames.size() == recvTy->genericArgs.size() &&
                        !gpNames.empty()) {
                        // Resolve the args under the OUTER substitution (still
                        // active here) so a `Box<U>` receiver inside a generic
                        // body binds T→U's concrete type.
                        std::unordered_map<std::string_view, llvm::Type*>
                            newSubst;
                        std::unordered_map<std::string_view,
                                           const sema::TypeRef*> newSema;
                        for (std::size_t gi = 0; gi < gpNames.size(); ++gi) {
                            newSubst[gpNames[gi]] =
                                lowerTypeRef(recvTy->genericArgs[gi]);
                            newSema[gpNames[gi]] = recvTy->genericArgs[gi];
                        }
                        // If any resolved arg is still the i32 fallback (an
                        // unresolved enclosing param), prefer the receiver
                        // local's stored concrete substitution.
                        bool hasUnresolved = false;
                        for (auto& [name, ty] : newSubst)
                            if (ty == i32()) { hasUnresolved = true; break; }
                        if (hasUnresolved) {
                            sema::Symbol* recvSym = ctx_.exprSymbol(*recvExpr);
                            const ast::Node* recvDecl = nullptr;
                            if (auto* loc =
                                    dynamic_cast<sema::LocalSymbol*>(recvSym))
                                recvDecl = loc->decl;
                            if (recvDecl) {
                                auto lit = localGenericSubst_.find(recvDecl);
                                if (lit != localGenericSubst_.end())
                                    newSubst = lit->second;
                            }
                        }
                        genericSubst_ = std::move(newSubst);
                        genericSubstSemaTy_ = std::move(newSema);
                        substInstalled = true;
                    }
                }
            }
            llvm::Value* recv = a;
            if (cmpMs->parent &&
                cmpMs->parent->kind == ast::TypeDeclKind::Record &&
                !recv->getType()->isPointerTy()) {
                // fix #1858 (latent #1855 sibling) / §14.2: size the spill slot
                // to the RECEIVER VALUE's own (monomorphized) type, not the
                // unspecialized `recordStruct(cmpMs->parent)` placeholder —
                // storing an 8-byte specialized value into the 4-byte
                // i32-placeholder slot is a stack-buffer-overflow WRITE (the
                // masked sibling of #1855, LlvmBackend_Calls2.cpp:4915). A no-op
                // for a non-generic record (recv->getType() ==
                // recordStruct(cmpMs->parent)).
                llvm::Value* tmp = entryAlloca(recv->getType());
                builder_.CreateStore(recv, tmp);
                recv = tmp;
            }
            std::vector<llvm::Value*> cargs{recv, b};
            llvm::Value* r =
                emitMethodCall(cmpMs, cargs, /*baseQualified=*/false);
            if (substInstalled) {
                genericSubst_ = std::move(savedSubst);
                genericSubstSemaTy_ = std::move(savedSubstSema);
            }
            return r;
        };
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
                // Abs for int/long — check MinValue first (#1231 / §6.1):
                // |int.MinValue| and |long.MinValue| are not representable as
                // int/long respectively, so raise OverflowException per the
                // checked-arithmetic rule (same as integer overflow in §6.1).
                {
                    bool isL = arg->getType()->isIntegerTy(64);
                    int64_t minV = isL ? INT64_MIN : INT32_MIN;
                    llvm::Value* minConst = llvm::ConstantInt::getSigned(
                        arg->getType(), minV);
                    llvm::Value* isMin = builder_.CreateICmpEQ(
                        arg, minConst, "abs.ismin");
                    emitGuardThrow(isMin, "OverflowException");
                }
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
                } else if (sema::MethodSymbol* cmpMs =
                               comparableCompareTo(call.args[0].value.get())) {
                    // #1141: non-numeric IComparable<T> → a if a.CompareTo(b) <= 0.
                    llvm::Value* cmp =
                        emitCompareToCmp(cmpMs, a, b, call.args[0].value.get());
                    if (!cmp) return nullptr;
                    llvm::Value* le = builder_.CreateICmpSLE(
                        cmp, llvm::ConstantInt::get(cmp->getType(), 0));
                    return builder_.CreateSelect(le, a, b, "min.cmp.result");
                } else if (isStringArg(call.args[0].value.get())) {
                    // fix #2785 / stdlib §9.6: ordinal string ordering.
                    llvm::Value* cmp = emitStrCmp(a, b);
                    if (!cmp) return nullptr;
                    llvm::Value* le = builder_.CreateICmpSLE(
                        cmp, llvm::ConstantInt::get(cmp->getType(), 0));
                    return builder_.CreateSelect(le, a, b, "min.str.result");
                } else if (llvm::Value* dcmp =
                               isDecimalArg(call.args[0].value.get())
                                   ? emitDecCmp(a, b) : nullptr) {
                    // fix #2788 / stdlib §4.4: decimal ordering via bz_decimal_cmp.
                    llvm::Value* le = builder_.CreateICmpSLE(
                        dcmp, llvm::ConstantInt::get(dcmp->getType(), 0));
                    return builder_.CreateSelect(le, a, b, "min.dec.result");
                } else {
                    // fix #2786: fail CLOSED before the SIToFP below.
                    if (rejectNonNumericMathOperand(a) ||
                        rejectNonNumericMathOperand(b))
                        return nullptr;
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
                } else if (sema::MethodSymbol* cmpMs =
                               comparableCompareTo(call.args[0].value.get())) {
                    // #1141: non-numeric IComparable<T> → a if a.CompareTo(b) >= 0.
                    llvm::Value* cmp =
                        emitCompareToCmp(cmpMs, a, b, call.args[0].value.get());
                    if (!cmp) return nullptr;
                    llvm::Value* ge = builder_.CreateICmpSGE(
                        cmp, llvm::ConstantInt::get(cmp->getType(), 0));
                    return builder_.CreateSelect(ge, a, b, "max.cmp.result");
                } else if (isStringArg(call.args[0].value.get())) {
                    // fix #2785 / stdlib §9.6: ordinal string ordering.
                    llvm::Value* cmp = emitStrCmp(a, b);
                    if (!cmp) return nullptr;
                    llvm::Value* ge = builder_.CreateICmpSGE(
                        cmp, llvm::ConstantInt::get(cmp->getType(), 0));
                    return builder_.CreateSelect(ge, a, b, "max.str.result");
                } else if (llvm::Value* dcmp =
                               isDecimalArg(call.args[0].value.get())
                                   ? emitDecCmp(a, b) : nullptr) {
                    // fix #2788 / stdlib §4.4: decimal ordering via bz_decimal_cmp.
                    llvm::Value* ge = builder_.CreateICmpSGE(
                        dcmp, llvm::ConstantInt::get(dcmp->getType(), 0));
                    return builder_.CreateSelect(ge, a, b, "max.dec.result");
                } else {
                    // fix #2786: fail CLOSED before the SIToFP below.
                    if (rejectNonNumericMathOperand(a) ||
                        rejectNonNumericMathOperand(b))
                        return nullptr;
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
        // §2.2 (v0.285): Math.Pi, Math.Tau — the surviving named constants.
        // fix #3559: `E` and the never-documented `PI` spelling were removed at
        // v0.284 (G1 ruling, issue 3411) and are rejected in sema.
        if (call.args.empty()) {
            auto* dbl = llvm::Type::getDoubleTy(llvm_);
            if (m == "Pi") return llvm::ConstantFP::get(dbl, 3.14159265358979323846);
            if (m == "Tau") return llvm::ConstantFP::get(dbl, 6.28318530717958647692);
        }
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
            } else if (sema::MethodSymbol* cmpMs =
                           comparableCompareTo(call.args[0].value.get())) {
                // #1141: non-numeric IComparable<T> → value.CompareTo(lo) < 0 ?
                // lo : (value.CompareTo(hi) > 0 ? hi : value).
                llvm::Value* cmpLo =
                    emitCompareToCmp(cmpMs, val, lo, call.args[0].value.get());
                llvm::Value* cmpHi =
                    emitCompareToCmp(cmpMs, val, hi, call.args[0].value.get());
                if (!cmpLo || !cmpHi) return nullptr;
                llvm::Value* ltLo = builder_.CreateICmpSLT(
                    cmpLo, llvm::ConstantInt::get(cmpLo->getType(), 0));
                llvm::Value* gtHi = builder_.CreateICmpSGT(
                    cmpHi, llvm::ConstantInt::get(cmpHi->getType(), 0));
                llvm::Value* inner = builder_.CreateSelect(gtHi, hi, val);
                return builder_.CreateSelect(ltLo, lo, inner, "clamp.cmp.result");
            } else if (isDecimalArg(call.args[0].value.get())) {
                // fix #2788 / stdlib §4.4: decimal ordering via bz_decimal_cmp,
                // same shape as the CompareTo arm above. Each comparison is
                // emitted EXACTLY ONCE — testing emitDecCmp in the `else if`
                // condition would emit its allocas and call and then emit them
                // again in the body, leaving dead `bz_decimal_cmp` calls in the
                // block.
                llvm::Value* cmpLo = emitDecCmp(val, lo);
                llvm::Value* cmpHi = emitDecCmp(val, hi);
                // Fail CLOSED, and loudly: a null here means the operands did
                // not carry the decimal struct layout after all, which must not
                // become a silent zero/null result (#2491).
                if (!cmpLo || !cmpHi) {
                    rejectNonNumericMathOperand(val);
                    return nullptr;
                }
                llvm::Value* ltLo = builder_.CreateICmpSLT(
                    cmpLo, llvm::ConstantInt::get(cmpLo->getType(), 0));
                llvm::Value* gtHi = builder_.CreateICmpSGT(
                    cmpHi, llvm::ConstantInt::get(cmpHi->getType(), 0));
                llvm::Value* inner = builder_.CreateSelect(gtHi, hi, val);
                return builder_.CreateSelect(ltLo, lo, inner, "clamp.dec.result");
            } else if (isStringArg(call.args[0].value.get())) {
                // fix #2785 / stdlib §9.6: ordinal string ordering, same shape
                // as the CompareTo arm above.
                llvm::Value* cmpLo = emitStrCmp(val, lo);
                llvm::Value* cmpHi = emitStrCmp(val, hi);
                if (!cmpLo || !cmpHi) return nullptr;
                llvm::Value* ltLo = builder_.CreateICmpSLT(
                    cmpLo, llvm::ConstantInt::get(cmpLo->getType(), 0));
                llvm::Value* gtHi = builder_.CreateICmpSGT(
                    cmpHi, llvm::ConstantInt::get(cmpHi->getType(), 0));
                llvm::Value* inner = builder_.CreateSelect(gtHi, hi, val);
                return builder_.CreateSelect(ltLo, lo, inner, "clamp.str.result");
            } else {
                // fix #2786: fail CLOSED before the SIToFP below.
                if (rejectNonNumericMathOperand(val) ||
                    rejectNonNumericMathOperand(lo) ||
                    rejectNonNumericMathOperand(hi))
                    return nullptr;
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
        // fix #3559 (stdlib §2.2, v0.285): the libc-backed lowerings for
        // `Log`/`Log2`/`Log10`, the trig/hyperbolic/`Exp` block, `Atan2` and
        // `Hypot` were deleted here when v0.284 removed those members from the
        // documents. `Fmod` and `IEEERemainder` are the two survivors of the
        // 2-arg group, so it no longer needs a name-to-libc table.
        if (call.args.size() == 2 && call.args[0].value && call.args[1].value &&
            (m == "Fmod" || m == "IEEERemainder")) {
            llvm::Value* a = lowerExpr(*call.args[0].value);
            llvm::Value* b = lowerExpr(*call.args[1].value);
            if (!a || !b) return nullptr;
            auto* dbl = llvm::Type::getDoubleTy(llvm_);
            if (!a->getType()->isDoubleTy()) a = builder_.CreateSIToFP(a, dbl);
            if (!b->getType()->isDoubleTy()) b = builder_.CreateSIToFP(b, dbl);
            const char* fn2 = m == "Fmod" ? "fmod" : "remainder";
            auto* fn = module_->getFunction(fn2);
            if (!fn) {
                auto* ft = llvm::FunctionType::get(dbl, {dbl, dbl}, false);
                fn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, fn2, module_.get());
            }
            return builder_.CreateCall(fn, {a, b}, std::string(m) + ".result");
        }
        // fix #3580 (stdlib §2.2, v0.286): Math.DivRem(a, b, out remainder)
        // — returns the quotient and writes the remainder through the `out`
        // parameter. Two overloads: (int, int, out int): int and
        // (long, long, out long): long. The return type follows the first
        // argument (sema's polymorphic-return refinement sets it to long
        // when the arg is long; int stays int). Semantics follow §6.1, the
        // same rule `/` already lowers under (LlvmBackend_Exprs.cpp:8885):
        //   - b = 0          → DivideByZeroException
        //   - INT_MIN / -1   → OverflowException (signed-result unrepresentable;
        //                      remainder on that one path is mathematically 0
        //                      and is forced via a `select`, the same
        //                      `srem`-safe-on-INT_MIN pattern §6.1 uses)
        //   - otherwise      → `sdiv` (quotient), `srem` (remainder)
        // The `out` parameter is resolved the same way Convert.TryToInt32
        // (just below) resolves its out parameter: a slot pre-created by
        // `ensureOutVarSlots`, written through `resolveOutAddrWithBracket`,
        // with `emitAccessorRefArgWriteBacks` flushing any bracket
        // (`arr[i]` / `obj.Prop` target) on the normal-return edge (#2938).
        // Not `pure` (§22.15): a pure member declares no `out`.
        if (m == "DivRem" && call.args.size() == 3 &&
            call.args[0].value && call.args[1].value &&
            call.args[2].value) {
            llvm::Value* a = lowerExpr(*call.args[0].value);
            llvm::Value* b = lowerExpr(*call.args[1].value);
            if (!a || !b) return nullptr;
            // Reject a non-integer arg with the same "not a numeric
            // primitive this intrinsic handles" wording used for `Fmod`/
            // `IEEERemainder`/`Sqrt`/`Pow` — except `Math.DivRem` is signed-
            // integer-only by its declaration, so float/decimal are also
            // rejected explicitly (no `sitofp`/`fdiv` ladder, unlike the
            // float-returning Math members).
            if (!a->getType()->isIntegerTy() || !b->getType()->isIntegerTy()) {
                ctx_.diagnostics().report(
                    Severity::Error, call.span.begin,
                    "`Math.DivRem` requires `int` or `long` arguments "
                    "(stdlib §2.2)");
                return nullptr;
            }
            // §6.1: zero divisor — emit guard FIRST so the trap fires
            // before any UB-producing op, mirroring the `/` lowering.
            emitGuardThrow(builder_.CreateICmpEQ(b, llvm::ConstantInt::get(b->getType(), 0)),
                           "DivideByZeroException");
            // §6.1: signed INT_MIN / -1. LLVM `sdiv` on that pair is UB
            // (LangRef) and x86 IDIV faults (SIGFPE); `srem` is also UB.
            // The `/` operator raises OverflowException; the `mod` operator
            // substitutes a 1-divisor and selects 0. DivRem's QUOTIENT
            // follows `/`; the REMAINDER is mathematically 0 on that path
            // and is representable, so the same select-to-0 pattern that
            // `mod` uses applies — but ONLY to the remainder, not the
            // quotient. The branch is identical for both shapes (both
            // ucomps must hold), so emit one `isMinNegOne` test and reuse
            // it for both edges.
            llvm::Value* minV = llvm::ConstantInt::getSigned(
                a->getType(),
                a->getType()->isIntegerTy(64) ? INT64_MIN : INT32_MIN);
            llvm::Value* negOne = llvm::ConstantInt::getSigned(a->getType(), -1);
            llvm::Value* isMinNegOne = builder_.CreateAnd(
                builder_.CreateICmpEQ(a, minV),
                builder_.CreateICmpEQ(b, negOne));
            // The quotient's edge matches `/`: raise OverflowException
            // (sdiv is UB on this path, so we MUST trap before reaching it).
            emitGuardThrow(isMinNegOne, "OverflowException");
            llvm::Value* quotient = builder_.CreateSDiv(a, b);
            // The remainder's edge matches `mod`: substitute a 1-divisor
            // for srem (avoids the UB), then select 0 as the result.
            // `1` is representable in both int and long, so CreateSRem
            // with the safe divisor is well-defined on every input.
            llvm::Value* safeR = builder_.CreateSelect(
                isMinNegOne, llvm::ConstantInt::getSigned(b->getType(), 1), b);
            llvm::Value* remRaw = builder_.CreateSRem(a, safeR);
            llvm::Value* remainder = builder_.CreateSelect(
                isMinNegOne, llvm::ConstantInt::get(b->getType(), 0), remRaw);
            // `out remainder` — same pattern as Convert.TryToInt32 (#2938).
            llvm::Type* et = nullptr;
            std::vector<AccessorRefArg> outArgs;
            llvm::Value* outAddr = resolveOutAddrWithBracket(
                *call.args[2].value, et, outArgs);
            if (!outAddr) {
                emitAccessorRefArgWriteBacks(outArgs, /*emit=*/false);
                return nullptr;
            }
            builder_.CreateStore(remainder, outAddr);
            emitAccessorRefArgWriteBacks(outArgs, /*emit=*/true);
            return quotient;
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
        // allocation as `new CancellationTokenSource()`) and immediately schedules
        // it to cancel after `delay` via bz_cts_cancel_after (#1413 / #1378 timer).
        // TimeSpan lowers to an i64 tick count (100-ns units) → ms = ticks / 10000
        // (same conversion as Task.Delay(TimeSpan) and CancelAfter above).
        if (m == "WithCancelAfter" && !call.args.empty()) {
            auto* allocFn = declareC("bz_cts_alloc", ptrTy(), {});
            llvm::Value* src = builder_.CreateCall(allocFn, {}, "cts.wca");
            if (call.args[0].value) {
                llvm::Value* span = lowerExpr(*call.args[0].value);
                if (span) {
                    if (!span->getType()->isIntegerTy(64))
                        span = builder_.CreateSExtOrTrunc(span, i64());
                    llvm::Value* ms = builder_.CreateSDiv(
                        span, builder_.getInt64(10000), "wca.ms");
                    auto* fn = declareC("bz_cts_cancel_after",
                        llvm::Type::getVoidTy(llvm_), {ptrTy(), i64()});
                    builder_.CreateCall(fn, {src, ms});
                }
            }
            return src;
        }
        // §30.8 / stdlib §6.1 (v0.173, #810): CreateLinked(a, b) — a linked child
        // source cancelled when either parent token cancels (or via its own
        // Cancel/CancelAfter, whichever first).  Each argument lowers to a
        // CancellationToken handle (a ptr — the parent source's state cell, or a
        // null ptr for CancellationToken.None).  The runtime links the child onto
        // each non-null parent and fires synchronously for any already-cancelled
        // parent.  The result is an owned CancellationTokenSource (disposed via
        // bz_cts_dispose, which unregisters from all parents first).
        if (m == "CreateLinked" && call.args.size() == 2) {
            llvm::Value* a = call.args[0].value ? lowerExpr(*call.args[0].value)
                                                : nullptr;
            llvm::Value* b = call.args[1].value ? lowerExpr(*call.args[1].value)
                                                : nullptr;
            if (!a) a = llvm::ConstantPointerNull::get(ptrTy());
            if (!b) b = llvm::ConstantPointerNull::get(ptrTy());
            auto* fn = declareC("bz_cts_create_linked", ptrTy(), {ptrTy(), ptrTy()});
            return builder_.CreateCall(fn, {a, b}, "cts.linked");
        }
        // §30.8 / stdlib §6.1 (v0.173, #810): CreateLinkedAll(parents) — the same
        // over an Array<CancellationToken>.  The argument lowers to a bz_array_t*
        // whose ptr-sized elements are CancellationToken handles; the runtime
        // links the child onto every non-null element.
        if (m == "CreateLinkedAll" && call.args.size() == 1 && call.args[0].value) {
            llvm::Value* arr = lowerExpr(*call.args[0].value);
            if (!arr) return nullptr;
            auto* fn = declareC("bz_cts_create_linked_all", ptrTy(), {ptrTy()});
            return builder_.CreateCall(fn, {arr}, "cts.linkedall");
        }
    }

    // §8.1 / issue #646: Stopwatch static factory.
    // StartNew() allocates a new Stopwatch and immediately starts it.
    if (cls == "Stopwatch") {
        if (m == "StartNew" && call.args.empty()) {
            auto* allocFn = declareC("bz_stopwatch_alloc", ptrTy(), {});
            llvm::Value* sw = builder_.CreateCall(allocFn, {}, "sw.new");
            auto* startFn = declareC("bz_stopwatch_start",
                                     llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(startFn, {sw});
            return sw;
        }
    }

    // §8.2 / issue #646: Debug static methods.
    // Assert(condition) / Assert(condition, message): no-op if true; abort if false.
    // WriteLine(message): write to stderr.
    if (cls == "Debug") {
        if (m == "Assert" && call.args.size() >= 1 && call.args[0].value) {
            llvm::Value* cond = lowerExpr(*call.args[0].value);
            if (!cond) return nullptr;
            // Normalize condition to i1.
            if (!cond->getType()->isIntegerTy(1)) {
                cond = builder_.CreateICmpNE(cond, llvm::ConstantInt::get(cond->getType(), 0));
            }
            auto* okBB   = llvm::BasicBlock::Create(llvm_, "dbg.assert.ok",   currentFn_);
            auto* failBB = llvm::BasicBlock::Create(llvm_, "dbg.assert.fail", currentFn_);
            builder_.CreateCondBr(cond, okBB, failBB);
            builder_.SetInsertPoint(failBB);
            // Determine the message argument (string ptr or null).
            llvm::Value* msgPtr = llvm::ConstantPointerNull::get(ptrTy());
            if (call.args.size() >= 2 && call.args[1].value) {
                llvm::Value* mv = lowerExpr(*call.args[1].value);
                if (mv && mv->getType()->isPointerTy()) msgPtr = mv;
            }
            auto* failFn = declareC("bz_debug_assert_fail",
                                    llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(failFn, {msgPtr});
            builder_.CreateUnreachable();
            builder_.SetInsertPoint(okBB);
            return nullptr;
        }
        if (m == "WriteLine" && call.args.size() == 1 && call.args[0].value) {
            llvm::Value* msgPtr = lowerExpr(*call.args[0].value);
            // fix #2879 (spec §8.5 / §28.3 / §28.9, v0.240): the OBJECT-typed
            // call arm — a CallExpr to a method whose body provably always
            // returns a +1 string producer (`m.Fresh()` where
            // `Fresh(): Object => "x" + S`) — transfers exactly +1 from the
            // callee. Debug.WriteLine is special-lowered and bypasses normal
            // call-arg disposal; without an explicit release here the callee's
            // +1 leaks (38 B/call measured on 564b6d1, the string twin of
            // #2865). isPlusOneStringProducer on its own cannot recognize a
            // user method returning Object whose body is a string producer —
            // isUserMethodObjectStringReturnCall does. A borrow (IdentifierExpr
            // read of an Object local holding a string) is NOT a producer call,
            // so the predicate is correctly negative there.
            bool freshStr = msgPtr &&
                            (isPlusOneStringProducer(call.args[0].value.get()) ||
                             isUserMethodObjectStringReturnCall(
                                 call.args[0].value.get()));
            if (!msgPtr) msgPtr = llvm::ConstantPointerNull::get(ptrTy());
            auto* wlFn = declareC("bz_debug_writeline",
                                  llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(wlFn, {msgPtr});
            // #1031 (sibling): Debug.WriteLine is special-lowered and bypasses the
            // normal call-arg disposal, so a fresh +1 string argument ($"..." /
            // a + b / slice / numeric ToString) is otherwise unreferenced after the
            // write — release it (rc-aware) so it does not leak. A borrow (string
            // local/field/literal) is not a plus-one producer → not released.
            if (freshStr) emitStringRelease(msgPtr);
            return nullptr;
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
        // bool, etc.) only — OwningTask<T> for class T is FromOwnedResult (deferred).
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
        // stdlib §6.7 (line 2993) / §30.5 Pattern B:
        // Task.FromOwnedResult<T>(owned value: T): OwningTask<T> where T: class.
        // Build an already-completed OWNED task carrying the owned class-pointer
        // result. The value is a ptr (a class object handle); store it into a
        // stack slot and complete the task with sizeof(void*) bytes — the same
        // already-done task primitive FromResult uses, but the stored result is
        // the heap-object pointer the awaiter takes ownership of (Pattern B).
        // The ownership pass treats `await OwningTask<T>` as the consume point that
        // disposes the awaited task and transfers the owned T to the awaiter, so
        // the value here is NOT released at the call site (it flows into the task
        // and out through the await). Without this arm the call produced no value
        // and the OwningTask<T> local was codegen'd with i32 storage → the await's
        // bz_task_is_done(i32 …) IR mismatch (#939) / runtime crash (#940).
        if (m == "FromOwnedResult" && !call.args.empty() &&
            call.args[0].value) {
            llvm::Value* val = lowerExpr(*call.args[0].value);
            if (!val) return nullptr;
            // The owned value is a class object pointer; store it at pointer
            // width so the awaiter reads back the same handle.
            llvm::AllocaInst* slot = builder_.CreateAlloca(ptrTy(), nullptr,
                                                           "from_owned.slot");
            builder_.CreateStore(val, slot);
            int64_t sz = static_cast<int64_t>(
                module_->getDataLayout().getTypeAllocSize(ptrTy()));
            auto* fromResultFn = declareC("bz_task_from_result",
                                          ptrTy(), {ptrTy(), i64()});
            return builder_.CreateCall(fromResultFn,
                {builder_.CreateBitCast(slot, ptrTy()),
                 builder_.getInt64(sz)},
                "task.from_owned_result");
        }
        // stdlib §6.7 (line 2986) / §30.10 (#738): Task.FromException<T>(exc):
        // Task<T> — an already-FAULTED task carrying the exception object.
        // Awaiting it re-raises the original exception (the await machinery reads
        // bz_task_error_obj). Allocate a task and fault it with the Exception.
        if (m == "FromException" && !call.args.empty() && call.args[0].value) {
            llvm::Value* exc = lowerExpr(*call.args[0].value);
            if (!exc) return nullptr;
            auto* allocFn = declareC("bz_task_alloc", ptrTy(), {});
            llvm::Value* task =
                builder_.CreateCall(allocFn, {}, "task.fromexc");
            auto* faultFn = declareC("bz_task_complete_error_obj",
                                     llvm::Type::getVoidTy(llvm_),
                                     {ptrTy(), ptrTy()});
            builder_.CreateCall(faultFn, {task, exc});
            return task;
        }
        // stdlib §6.7 (line 2987) / §30.10 (#738): Task.FromCancelled<T>():
        // Task<T> — an already-faulted task carrying an
        // OperationCanceledException. Awaiting it re-raises the OCE. Construct
        // the OCE inline ({vtable, msg} — same layout as bz_task_make_oce / a
        // builtin exception) and fault a fresh task with it.
        if (m == "FromCancelled") {
            llvm::Value* oceVtable = llvm::ConstantPointerNull::get(ptrTy());
            auto vit = builtinVtable_.find("OperationCanceledException");
            if (vit != builtinVtable_.end())
                oceVtable = builder_.CreateBitCast(vit->second, ptrTy());
            auto* callocFn = declareC("calloc", ptrTy(), {i64(), i64()});
            llvm::Value* oce = builder_.CreateCall(
                callocFn, {builder_.getInt64(1), builder_.getInt64(kExcObjSize)}, "oce");
            builder_.CreateStore(oceVtable, oce);  // slot 0 = vtable
            auto* allocFn = declareC("bz_task_alloc", ptrTy(), {});
            llvm::Value* task =
                builder_.CreateCall(allocFn, {}, "task.fromcancel");
            auto* faultFn = declareC("bz_task_complete_error_obj",
                                     llvm::Type::getVoidTy(llvm_),
                                     {ptrTy(), ptrTy()});
            builder_.CreateCall(faultFn, {task, oce});
            return task;
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
            // #1616 / §30.10: mirror WhenAny. An INLINE task-array literal
            // argument — `Task.WhenAll([t1, t2])` — is not a general expression
            // value: WhenAll is a compiler intrinsic with no MethodDecl param, so
            // sema never gave the literal a materialization target and a bare
            // lowerExpr returns null for the CollectionLiteralExpr (§15.4).
            // `arr` was then null and the whole WhenAll call silently dropped out
            // of codegen — the `using var w := Task.WhenAll([…])` store never
            // executed, leaving `w` (and the async frame's pending-task slot)
            // holding stack garbage; the subsequent `await w` dereferenced it and
            // SIGSEGV'd (bound elements) or hung (anonymous elements). lowerTaskArrayArg
            // materializes the literal into an owned Array<Task> temporary (freed
            // SYNCHRONOUSLY after the borrowing runtime call, freeTaskArrayArgTemp —
            // both bz_task_when_all and bz_task_when_all_results snapshot the input
            // task pointers before returning and never retain the array), exactly
            // as WhenAny does; a pre-bound Array<Task> argument is unaffected
            // (plain lowerExpr, isLiteralTemp=false).
            auto [arr, isLiteralTemp] = lowerTaskArrayArg(*call.args[0].value);
            if (!arr) return nullptr;
            // Check if the arg is Array<Task<T>> (generic result variant).
            // arrTy is the sema TypeRef for the argument expression.
            auto* arrTy = ctx_.exprType(*call.args[0].value);
            // arrTy should be Named("Array", [Named("Task", [T])]).
            // If Task<T> has a non-empty genericArgs, we use the typed variant.
            bool isTyped = false;
            int64_t elemSz = 8;
            const sema::TypeRef* T_for_typed = nullptr;
            if (arrTy && arrTy->kind == sema::TypeRef::Kind::Named &&
                !arrTy->genericArgs.empty() && arrTy->genericArgs[0]) {
                auto* elemTaskTy = arrTy->genericArgs[0];
                // elemTaskTy should be Task<T>.
                auto elemName = builtinTypeName(elemTaskTy);
                if ((elemName == "Task" || elemName == "OwningTask") &&
                    !elemTaskTy->genericArgs.empty() &&
                    elemTaskTy->genericArgs[0]) {
                    T_for_typed = elemTaskTy->genericArgs[0];
                    llvm::Type* tLlvmTy = lowerTypeRef(T_for_typed);
                    if (tLlvmTy) {
                        elemSz = static_cast<int64_t>(
                            module_->getDataLayout().getTypeAllocSize(tLlvmTy));
                    }
                    isTyped = true;
                }
            }
            // fix #1830 / §6.7 / stdlib §6.7 (line 2875): typed Task.WhenAll<T>
            // over an INLINE collection literal. sema may have set the literal's
            // exprType to the bare Array<Task> shape (the arg has no MethodDecl
            // target type, so the argTy path above is null / falls through to
            // the bare-Task isTyped=false). Fall back to inspecting the
            // literal's element exprTypes directly: when ALL elements are
            // Task<T> for the same T, emit the typed bz_task_when_all_results
            // call so the runtime collects each task's T-typed result into
            // an Array<T> (matching the result type sema produced). Mirrors
            // the sema-side inferLiteralTaskTType in ExprTypecheck_Calls.cpp.
            if (!isTyped) {
                if (auto* cl = dynamic_cast<const ast::CollectionLiteralExpr*>(
                        call.args[0].value.get())) {
                    if (!cl->elements.empty()) {
                        const sema::TypeRef* commonT = nullptr;
                        bool allTaskT = true;
                        for (const auto& elUp : cl->elements) {
                            if (!elUp) { allTaskT = false; break; }
                            const sema::TypeRef* et =
                                ctx_.exprType(*elUp);
                            if (!et || et->kind != sema::TypeRef::Kind::Named ||
                                et->genericArgs.empty() ||
                                !et->genericArgs[0]) {
                                allTaskT = false; break;
                            }
                            auto ename = builtinTypeName(et);
                            if ((ename != "Task" && ename != "OwningTask") ||
                                et->genericArgs.empty() ||
                                !et->genericArgs[0]) {
                                allTaskT = false; break;
                            }
                            const sema::TypeRef* T = et->genericArgs[0];
                            if (!commonT) {
                                commonT = T;
                            } else if (commonT != T) {
                                if (commonT->kind != T->kind ||
                                    commonT->headName() != T->headName()) {
                                    allTaskT = false; break;
                                }
                            }
                        }
                        if (allTaskT && commonT) {
                            llvm::Type* tLlvmTy = lowerTypeRef(commonT);
                            if (tLlvmTy) {
                                elemSz = static_cast<int64_t>(
                                    module_->getDataLayout().getTypeAllocSize(tLlvmTy));
                                isTyped = true;
                            }
                        }
                    }
                }
            }
            llvm::Value* result;
            if (isTyped) {
                // Task.WhenAll<T>: collect results into Array<T>.
                auto* whenAllResFn = declareC("bz_task_when_all_results",
                                              ptrTy(), {ptrTy(), i64()});
                result = builder_.CreateCall(whenAllResFn,
                    {arr, builder_.getInt64(elemSz)},
                    "task.when_all_results");
                // fix #1830 / §6.7 (v0.217) / §30.5: the runtime
                // bz_task_when_all_results completes the aggregate with a
                // HEAP-ALLOCATED `Array<T>*` (a bz_array_t). The result
                // buffer holds a POINTER to the array (sizeof void*).
                // Disposal: the using-var / scope-end path on the LOCAL
                // bound by the await (`using var arr := await wa;`) calls
                // bz_array_free on the array at scope end — that is the
                // implementation's current owner. We deliberately do NOT
                // attach a result_disposer here: an attached disposer
                // double-frees that local, and use-after-frees the unbound
                // temporary form `using var rs := await Task.WhenAll([…]);`
                // (the task dies at end of statement while `rs` still points
                // at its array). Both measured under #2278.
                //
                // #2278 (open): the shapes where NO awaiting binding takes the
                // array — a discarded `await w;`, and a plain `var rs := await
                // w;` — therefore LEAK the aggregate `Array<T>`, and the same
                // hole exists for any user `async` method returning
                // `Task<Array<T>>`. It is not specific to WhenAll; #1830's
                // literal-element fallback merely routed the #1616 inline-
                // literal shape into it. The fix is a coordinated one — attach
                // a container disposer at every Pattern A collection-result
                // mint AND make the awaiting binding a §30.5 borrow-pin
                // (usingVarSourceIsBorrow) so exactly one reclaim survives.
            } else {
                auto* whenAllFn = declareC("bz_task_when_all", ptrTy(), {ptrTy()});
                result = builder_.CreateCall(whenAllFn, {arr}, "task.when_all");
            }
            // #1616: free a literal temp array synchronously, right after the
            // runtime call that borrowed it — mirror WhenAny (see freeTaskArrayArgTemp
            // / lowerTaskArrayArg for why this must not be a deferred scope-end free).
            if (isLiteralTemp) freeTaskArrayArgTemp(arr);
            return result;
        }
        // stdlib §6.7 (line 2884): Task.WhenAny(tasks: Array<Task>): Task<int>
        // (v0.162 / #1332). Returns a new task that completes with the i32 INDEX
        // of the first-done input task (not the task pointer — Task<Task> was
        // unconstructible under §30.5). sema types the result as Task<int>, so the
        // await-result is loaded as i32. The runtime bz_task_when_any completes
        // the aggregate with sizeof(int32_t) bytes holding the winner index.
        //
        // #1501 / §30.10 / §30.15: the natural "select" idiom pairs WhenAny with
        // an INLINE array literal of per-branch Receive tasks —
        // `Task.WhenAny([a.Receive(), b.Receive()])`. A bare lowerExpr on that
        // CollectionLiteralExpr returned null (§15.4: not a general expression
        // value — WhenAny is a compiler intrinsic with no MethodDecl param, so
        // sema never gave the literal a materialization target), so `arr` was
        // null and the whole WhenAny call silently dropped out of codegen —
        // the `using var w := Task.WhenAny([…])` store never executed, leaving
        // `w` (and the async frame's pending-task slot) holding whatever
        // garbage was already on the stack. The subsequent `await w` then
        // called bz_task_is_done on that garbage pointer, corrupting an
        // unrelated stack slot's "mutex" and SIGSEGV/aborting. lowerTaskArrayArg
        // materializes a literal into a proper owned Array<Task> temporary,
        // exactly as the byte-array FFI factories do for the identical class of
        // bug (#999); a pre-bound Array<Task> argument is unaffected (plain
        // lowerExpr). Unlike #999's factories, the temp here is freed
        // SYNCHRONOUSLY right after this call (freeTaskArrayArgTemp), not via
        // scope-end disposal — see lowerTaskArrayArg's comment: a deferred free
        // would need to survive the `await w` that (almost) always follows,
        // which can genuinely suspend the async state machine, and the temp's
        // alloca is invisible to collectSuspendLocals' frame-spill analysis.
        if (m == "WhenAny" && !call.args.empty() && call.args[0].value) {
            auto [arr, isLiteralTemp] = lowerTaskArrayArg(*call.args[0].value);
            if (!arr) return nullptr;
            // §30.10 / #1529: "WhenAny of an empty array raises ArgumentException
            // synchronously at the call site, not as a faulted task (it could
            // never complete, and the misuse is local)." Guard before calling
            // into the runtime, which otherwise has no input to wait on.
            {
                auto* lenFn = declareC("bz_array_len", i64(), {ptrTy()});
                llvm::Value* len =
                    builder_.CreateCall(lenFn, {arr}, "whenany.len");
                llvm::Value* isEmpty =
                    builder_.CreateICmpEQ(len, builder_.getInt64(0));
                emitGuardThrow(isEmpty, "ArgumentException");
            }
            auto* whenAnyFn = declareC("bz_task_when_any", ptrTy(), {ptrTy()});
            llvm::Value* result =
                builder_.CreateCall(whenAnyFn, {arr}, "task.when_any");
            if (isLiteralTemp) freeTaskArrayArgTemp(arr);
            return result;
        }
        // stdlib §6.7 (line 2942): Task.WhenAllOwned<T>(
        //   owned tasks: OwningArray<OwningTask<T>>): OwningTask<OwningArray<T>>
        //   where T: class.
        //
        // Pattern B aggregator for class T. The caller transfers ownership of
        // the OwningArray<OwningTask<T>>; bz_task_when_all_owned collects each
        // OwningTask<T> result (a pointer) into a new OwningArray<T> and frees
        // each input task and the input array. Returns OwningTask<OwningArray<T>>
        // (a bz_task_t* whose result is an OwningArray<T> pointer).
        if (m == "WhenAllOwned" && !call.args.empty() && call.args[0].value) {
            llvm::Value* arr = lowerExpr(*call.args[0].value);
            if (!arr) return nullptr;
            auto* whenAllOwnedFn = declareC("bz_task_when_all_owned",
                                            ptrTy(), {ptrTy()});
            return builder_.CreateCall(whenAllOwnedFn, {arr},
                                       "task.when_all_owned");
        }
        // stdlib §6.7 v0.212 / proposal 0023 D1-D7: Task.WhenEach streaming
        // fan-in. Returns an IAsyncEnumerable<Task> / IAsyncEnumerable<Task<T>>
        // (a vtable-backed runtime object — see the comment at
        // bz_task_when_each_init_borrowing in bezel_runtime_concurrency.cpp).
        // The input array is BORROWED (per §28.3 deferred-activation frame
        // carriage: the returned iterable is a borrow-carrying value whose
        // source is the argument array, and the borrow tracker enforces that
        // the array outlives the `await for` loop). The #1813 input seam —
        // the named-binding discipline that avoids `tasks.Add(freshCall())`
        // abandoning the freshly-minted task at end of statement — is
        // documented in the stdlib text; tests build the borrowed array over
        // named task bindings.
        if (m == "WhenEach" && !call.args.empty() && call.args[0].value) {
            // The arg is either Array<Task> (no-result) or Array<Task<T>>.
            // Both share the same runtime shape: ptr-sized task elements, a
            // borrowed bz_array_t* of the same layout. `is_typed` is a marker
            // for the runtime (1 = Task<T> element, 0 = Task element); both
            // yield already-completed task pointers to the consumer.
            auto [arr, isLiteralTemp] = lowerTaskArrayArg(*call.args[0].value);
            if (!arr) return nullptr;
            auto* arrTy = ctx_.exprType(*call.args[0].value);
            int32_t isTyped = 0;
            if (arrTy && arrTy->kind == sema::TypeRef::Kind::Named &&
                !arrTy->genericArgs.empty() && arrTy->genericArgs[0]) {
                auto* elemTaskTy = arrTy->genericArgs[0];
                auto elemName = builtinTypeName(elemTaskTy);
                if ((elemName == "Task" || elemName == "OwningTask") &&
                    !elemTaskTy->genericArgs.empty() &&
                    elemTaskTy->genericArgs[0]) {
                    isTyped = 1;
                }
            }
            auto* whenEachFn = declareC("bz_task_when_each_init_borrowing",
                                        ptrTy(), {ptrTy(), i32()});
            llvm::Value* result = builder_.CreateCall(
                whenEachFn, {arr, builder_.getInt32(isTyped)},
                "task.when_each");
            // The runtime copies task pointers into its observer registrations;
            // it never retains the array container itself. Match WhenAll/WhenAny:
            // release an intrinsic-only literal materialization synchronously.
            if (isLiteralTemp) freeTaskArrayArgTemp(arr);
            return result;
        }
        // stdlib §6.7 v0.212 / proposal 0023 D7: Task.WhenEachOwned<T>(
        //   owned tasks: OwningArray<OwningTask<T>>
        // ): IOwningAsyncEnumerable<OwningTask<T>> where T: class.
        // Takes ownership of the OwningArray<OwningTask<T>> (Pattern B);
        // unconsumed tasks are freed by the enumerator's Dispose via
        // bz_task_free, whose result_disposer cascades to the produced
        // class value per §30.5. The runtime object has the
        // IOwningAsyncEnumerable<OwningTask<T>> vtable (slot 3 =
        // TakeCurrent, transferring ownership of the yielded
        // OwningTask<T> and nulling the slot).
        if (m == "WhenEachOwned" && !call.args.empty() && call.args[0].value) {
            llvm::Value* arr = lowerExpr(*call.args[0].value);
            if (!arr) return nullptr;
            // #2096 / §30.5 / proposal 0023 D7: derive the per-element result
            // disposer for T from OwningArray<OwningTask<T>>. The runtime is
            // type-erased and a bare `bz_task_from_result` (Task.FromOwnedResult)
            // task carries NO result_disposer of its own — its result disposer
            // is normally attached at the caller's scope-end drop, which never
            // runs once the task is moved into the array. So the enumerator's
            // Dispose (which frees tasks not consumed before enumeration ends)
            // would leak each unconsumed OwningTask<T>'s produced class value.
            // Thread T's destructor thunk to the runtime, which attaches it to
            // every input task so bz_task_free cascades disposal per §30.5.
            llvm::Value* resultDisposer =
                llvm::ConstantPointerNull::get(ptrTy());
            if (auto* argTy = ctx_.exprType(*call.args[0].value)) {
                if (argTy->kind == sema::TypeRef::Kind::Named &&
                    !argTy->genericArgs.empty() && argTy->genericArgs[0]) {
                    auto* taskTy = argTy->genericArgs[0];  // OwningTask<T>
                    if (!taskTy->genericArgs.empty() && taskTy->genericArgs[0])
                        resultDisposer = elementDisposerArg(
                            taskTy->genericArgs[0], /*owningFamily=*/true);
                }
            }
            auto* whenEachOwnedFn = declareC("bz_task_when_each_init_owning",
                                              ptrTy(), {ptrTy(), ptrTy()});
            return builder_.CreateCall(whenEachOwnedFn, {arr, resultDisposer},
                                       "task.when_each_owned");
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
            // §30.10 / §28.3 (#1890): the `owned` parameter transfers ownership
            // of the task to the runtime, which fires-and-forgets it here. When
            // the argument is a NAMED owned task local (`Task.FireAndForget(owned
            // t)` — the §22.3 spelling — or the `move t` form), that local also
            // carries a scope-end `d.task` fire-and-forget (#445: a plain `var t
            // := M();` task local is freed at scope end). Leaving the slot
            // non-null would fire-and-forget the SAME task twice — a double
            // completion-continuation registration / double bz_task_free that
            // corrupts async state and hangs. Null the source slot now (mirroring
            // the `move`-of-using-var nulling in lowerUnary and the async-return
            // task-transfer nulling in lowerReturn) so the null-guarded scope-end
            // disposal reloads null and skips. The fresh-producer form
            // (`Task.FireAndForget(M())`) has no named slot and is unaffected.
            const ast::Expr* av = call.args[0].value.get();
            if (auto* un = dynamic_cast<const ast::UnaryExpr*>(av))
                if (un->op == "move" && un->operand) av = un->operand.get();
            if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(av)) {
                if (dynamic_cast<sema::LocalSymbol*>(ctx_.exprSymbol(*id))) {
                    if (llvm::AllocaInst* slotAI = localSlot(*id)) {
                        llvm::Type* ety = nullptr;
                        llvm::Value* addr = boxedStorage(slotAI, ety);
                        if (ety && ety->isPointerTy())
                            builder_.CreateStore(
                                llvm::ConstantPointerNull::get(ptrTy()), addr);
                    }
                }
            }
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
    // per stdlib §2.7 / #1359: static Random.NextInt64/NextBoolean/NextBytes —
    // legacy static path (falls through to instance handler below for rng. calls).
    // These branch to libc rand() like the existing static Next path.
    if (cls == "Random") {
        if (m == "NextInt64" && call.args.empty()) {
            // Combine two rand() calls → 64-bit value.
            auto* randFn2 = module_->getFunction("rand");
            if (!randFn2) {
                auto* ft = llvm::FunctionType::get(i32(), {}, false);
                randFn2 = llvm::Function::Create(
                    ft, llvm::Function::ExternalLinkage, "rand", module_.get());
            }
            llvm::Value* lo = builder_.CreateZExt(
                builder_.CreateCall(randFn2, {}, "rng.lo"), i64());
            llvm::Value* hi = builder_.CreateZExt(
                builder_.CreateCall(randFn2, {}, "rng.hi"), i64());
            return builder_.CreateOr(lo, builder_.CreateShl(hi, builder_.getInt64(31)));
        }
        if (m == "NextBoolean" && call.args.empty()) {
            auto* randFnB = module_->getFunction("rand");
            if (!randFnB) {
                auto* ft = llvm::FunctionType::get(i32(), {}, false);
                randFnB = llvm::Function::Create(
                    ft, llvm::Function::ExternalLinkage, "rand", module_.get());
            }
            llvm::Value* r = builder_.CreateCall(randFnB, {}, "rng.b");
            return builder_.CreateICmpNE(
                builder_.CreateAnd(r, builder_.getInt32(1)),
                builder_.getInt32(0), "rng.bool");
        }
    }

    // ---------------------------------------------------------------------------
    // stdlib §9.3 / §4.4 fix #521: Decimal — static members and methods.
    // Decimal is a 128-bit value type stored as { i32 lo32, i32 mid32, i32 hi32, i32 flags }.
    // All runtime functions take a pointer to the struct.
    // ---------------------------------------------------------------------------
    if (cls == "Decimal" || cls == "decimal") {  // §5: both spellings (spec L243)
        llvm::StructType* dst = decimalStructType();

        // Helper: allocate a decimal-sized alloca slot.
        auto makeDecSlot = [&]() -> llvm::AllocaInst* {
            return entryAlloca(dst);
        };

        // Helper: build a decimal struct from compile-time parse of a cstr.
        auto decConstant = [&](const char* s) -> llvm::Value* {
            uint32_t dec[4] = {0, 0, 0, 0};
            bz_decimal_parse_cstr(s, dec, nullptr, nullptr);
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

        // Decimal.Parse(s: string): decimal — throws FormatException for malformed
        // input or OverflowException for out-of-range values (per fix #521 / §6.4).
        if (m == "Parse" && call.args.size() == 1 && call.args[0].value) {
            llvm::Value* strVal = lowerExpr(*call.args[0].value);
            if (!strVal) return nullptr;
            llvm::AllocaInst* slot = makeDecSlot();
            // per fix #521: pass OverflowException and FormatException vtables
            // so the runtime stores the correct exception in bz_exc on failure.
            auto getOVEVtable = [&]() -> llvm::Value* {
                auto vit = builtinVtable_.find("OverflowException");
                if (vit == builtinVtable_.end())
                    return llvm::ConstantPointerNull::get(ptrTy());
                return vit->second;
            };
            auto getFEVtable = [&]() -> llvm::Value* {
                auto vit = builtinVtable_.find("FormatException");
                if (vit == builtinVtable_.end())
                    return llvm::ConstantPointerNull::get(ptrTy());
                return vit->second;
            };
            auto* fn = declareC("bz_decimal_parse", llvm::Type::getVoidTy(llvm_),
                                {ptrTy(), ptrTy(), ptrTy(), ptrTy()});
            builder_.CreateCall(fn, {strVal, slot, getOVEVtable(), getFEVtable()});
            emitPendingCheck();  // per fix #521: runtime sets bz_exc on parse failure
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
            // Write the result into the out parameter. // fix #2938 / §22.3
            // The pre-fix branch was `if (outAddr)`, which silently dropped
            // the write when the target was an accessor-mediated location
            // (lvalueAddr returned null). resolveOutAddrWithBracket falls back
            // to the §22.3 bracket, so the assignment always runs and the
            // bracket flushes below.
            llvm::Type* et = nullptr;
            std::vector<AccessorRefArg> outArgs;
            llvm::Value* outAddr = resolveOutAddrWithBracket(
                *call.args[1].value, et, outArgs);
            if (!outAddr) {
                emitAccessorRefArgWriteBacks(outArgs, /*emit=*/false);
                return nullptr;
            }
            llvm::Value* result = builder_.CreateLoad(dst, slot, "dec.tryp.val");
            builder_.CreateStore(result, outAddr);
            emitAccessorRefArgWriteBacks(outArgs, /*emit=*/true);
            return builder_.CreateICmpNE(ok, builder_.getInt32(0), "dec.tryp.bool");
        }
    }

    // ---------------------------------------------------------------------------
    // stdlib §2.12 / issue #654: BitConverter static methods.
    // Pure bit-reinterpretation via LLVM bitcast — no runtime calls needed.
    //   FloatToInt32Bits(float):  bitcast f32 → i32
    //   Int32BitsToFloat(int):    bitcast i32 → f32
    //   DoubleToInt64Bits(double): bitcast f64 → i64
    //   Int64BitsToDouble(long):  bitcast i64 → f64
    // ---------------------------------------------------------------------------
    if (cls == "BitConverter") {
        if (call.args.size() == 1 && call.args[0].value) {
            llvm::Value* arg = lowerExpr(*call.args[0].value);
            if (!arg) return nullptr;
            if (m == "FloatToInt32Bits") {
                // Ensure arg is f32.
                if (!arg->getType()->isFloatTy())
                    arg = builder_.CreateFPTrunc(arg, llvm::Type::getFloatTy(llvm_), "bc.f32");
                return builder_.CreateBitCast(arg, i32(), "bc.f2i");
            }
            if (m == "Int32BitsToFloat") {
                // Ensure arg is i32.
                if (!arg->getType()->isIntegerTy(32))
                    arg = builder_.CreateTrunc(arg, i32(), "bc.i32");
                return builder_.CreateBitCast(arg, llvm::Type::getFloatTy(llvm_), "bc.i2f");
            }
            if (m == "DoubleToInt64Bits") {
                // Ensure arg is f64.
                if (!arg->getType()->isDoubleTy())
                    arg = builder_.CreateFPExt(arg, llvm::Type::getDoubleTy(llvm_), "bc.f64");
                return builder_.CreateBitCast(arg, i64(), "bc.d2l");
            }
            if (m == "Int64BitsToDouble") {
                // Ensure arg is i64.
                if (!arg->getType()->isIntegerTy(64))
                    arg = builder_.CreateSExt(arg, i64(), "bc.i64");
                return builder_.CreateBitCast(arg, llvm::Type::getDoubleTy(llvm_), "bc.l2d");
            }
            if (m == "Reinterpret") {
                // stdlib §2.12 (#753): generic same-width bit reinterpretation.
                // The target type is the call's resolved type (the explicit type
                // argument). Same LLVM type (e.g. int↔uint, both iN) is a no-op;
                // a same-width differently-shaped pair (e.g. float↔int) is a real
                // bitcast. Width mismatches are rejected in sema (typeCall).
                const sema::TypeRef* rt = ctx_.exprType(call);
                if ((!rt || rt->kind != sema::TypeRef::Kind::Primitive) && call.callee)
                    rt = ctx_.exprType(*call.callee);
                if (!rt || rt->kind != sema::TypeRef::Kind::Primitive) return nullptr;
                llvm::Type* dstTy = lowerPrimitive(rt->primitive);
                if (arg->getType() == dstTy) return arg;  // identical repr → no-op
                if (arg->getType()->getPrimitiveSizeInBits() ==
                    dstTy->getPrimitiveSizeInBits())
                    return builder_.CreateBitCast(arg, dstTy, "bc.reinterpret");
                return nullptr;  // width mismatch (sema rejects; defensive)
            }
        }
        return nullptr;
    }

    // ---------------------------------------------------------------------------
    // stdlib §9.4 / issue #654: Boolean static methods.
    // Reached when the receiver is the `Boolean` class name (the keyword alias
    // `bool` is handled by tryLowerPrimitiveStaticCall via primitiveFromName).
    // ---------------------------------------------------------------------------
    if (cls == "Boolean") {
        // Boolean.TryParse(s: string, out result: bool): bool
        // Accepts "true"/"false" case-insensitively; everything else → false.
        if (m == "TryParse" && call.args.size() == 2 &&
            call.args[0].value && call.args[1].value) {
            llvm::Value* strVal = lowerExpr(*call.args[0].value);
            if (!strVal) return nullptr;
            llvm::Value* strData = loadStringData(strVal);
            // fix #2938 / §22.3: see Convert.TryToInt32 above.
            llvm::Type* et = nullptr;
            std::vector<AccessorRefArg> outArgs;
            llvm::Value* outAddr = resolveOutAddrWithBracket(
                *call.args[1].value, et, outArgs);
            if (!outAddr) {
                emitAccessorRefArgWriteBacks(outArgs, /*emit=*/false);
                return nullptr;
            }
            auto* strcasecmpFn = declareC("strcasecmp", i32(), {ptrTy(), ptrTy()});
            llvm::Constant* trueStr = llvm::ConstantDataArray::getString(llvm_, "true", true);
            unsigned tid = stringLiteralIdx_++;
            auto* trueGV = new llvm::GlobalVariable(*module_, trueStr->getType(), true,
                llvm::GlobalValue::PrivateLinkage, trueStr,
                ".bz.btpc.true." + std::to_string(tid));
            llvm::Constant* falseStr = llvm::ConstantDataArray::getString(llvm_, "false", true);
            auto* falseGV = new llvm::GlobalVariable(*module_, falseStr->getType(), true,
                llvm::GlobalValue::PrivateLinkage, falseStr,
                ".bz.btpc.false." + std::to_string(tid));
            llvm::Value* cmpTrue = builder_.CreateICmpEQ(
                builder_.CreateCall(strcasecmpFn, {strData, trueGV}),
                builder_.getInt32(0), "btpc.isTrue");
            llvm::Value* cmpFalse = builder_.CreateICmpEQ(
                builder_.CreateCall(strcasecmpFn, {strData, falseGV}),
                builder_.getInt32(0), "btpc.isFalse");
            llvm::Value* ok = builder_.CreateOr(cmpTrue, cmpFalse, "btpc.ok");
            builder_.CreateStore(cmpTrue, outAddr);
            // fix #2938 / §22.3: flush any bracket on the normal-return edge.
            emitAccessorRefArgWriteBacks(outArgs, /*emit=*/true);
            return ok;
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

        // Guid.Parse(s: string): Guid — accepts the "D" and "N" renderings
        // (§2.18); a malformed string raises a catchable FormatException
        // (stdlib §2.6, v0.197). bz_guid_parse returns 0 on failure; guard on
        // that and throw via the standard builtin-throw path.
        //
        // Before #2004 the failure edge emitted CreateUnreachable — an
        // uncatchable trap where the spec promises a catchable exception, so a
        // `catch FormatException` never fired and execution fell into undefined
        // code (the zeroed/garbage Guid bytes were misread as a pointer →
        // wild read / SIGSEGV). Routing through emitGuardThrow makes the fault
        // a first-class, catchable throw like every other builtin guard.
        if (m == "Parse" && call.args.size() == 1 && call.args[0].value) {
            const ast::Expr* parseArg = call.args[0].value.get();
            llvm::Value* strVal = lowerExpr(*call.args[0].value);
            if (!strVal) return nullptr;
            llvm::AllocaInst* slot = makeGuidSlot();
            auto* fn = declareC("bz_guid_parse", i32(), {ptrTy(), ptrTy()});
            llvm::Value* ok = builder_.CreateCall(fn, {strVal, slot}, "guid.parse.ok");
            // fix #1998 / §28.3 "Unbound owned temporaries" / §4.6 / §28.9
            // (v0.226): Guid.Parse takes its `s: string` argument by BORROW —
            // bz_guid_parse (→ guid_parse_str) only READS s->data to decode the
            // 16 bytes, it does not adopt the reference (no `owned` on the
            // parameter, stdlib §2.6). So a fresh +1 string producer passed as
            // that argument — the issue's repro `Guid.Parse(g.ToString())`,
            // where g.ToString() mints an rc=1 heap bz.string (#1982/#1989) —
            // is the producer-temp-consumed-only-by-a-borrow-parameter shape:
            // §28.3's unbound-temporary rule enumerates "a non-`owned`
            // argument" as a borrowing use an unbound owned temporary may
            // serve before being "disposed at the end of the full expression",
            // and Parse returns a Guid (nothing chains off the argument past
            // the call), so the call's own end IS that boundary. Before this
            // fix nothing disposed the temp here: nothing else owns the +1 and
            // the argument is not bound to a name, so it leaked (the #1998
            // repro: 69 bytes/iteration — the 32-byte header + 37-byte buffer
            // — under ASan/LSan). Release the +1 reference immediately after
            // the consuming call: the string has been fully read by
            // guid_parse_str before bz_guid_parse returns (ok is 1/0), so the
            // reference is dead from this point on, on both the success and
            // the FormatException paths (the release runs before
            // emitGuardThrow's branch). A non-producer argument (a string
            // local/field/literal — a borrow, or an immortal rc=0 literal) is
            // NOT released here: its real owner reclaims it, and releasing a
            // borrow would be a double-free. Same shape as the #1881
            // borrowing-collection-Add and #2204 Insert/indexer-set arg-temp
            // disposal (isFreshOwnedClassProducer + emitDisposeAndReclaim),
            // adapted to the runtime-managed string (emitStringRelease).
            if (isPlusOneStringProducer(parseArg))
                emitStringRelease(strVal);
            llvm::Value* failed =
                builder_.CreateICmpEQ(ok, builder_.getInt32(0), "guid.parse.bad");
            emitGuardThrow(failed, "FormatException");  // §2.6 / §2.18, #2004
            return builder_.CreateLoad(gst, slot, "guid.parsed");
        }

        // Guid.TryParse(s: string, out result: Guid): bool
        if (m == "TryParse" && call.args.size() == 2 &&
            call.args[0].value && call.args[1].value) {
            const ast::Expr* parseArg = call.args[0].value.get();
            llvm::Value* strVal = lowerExpr(*call.args[0].value);
            if (!strVal) return nullptr;
            // fix #2938 / §22.3: see Convert.TryToInt32 above.
            llvm::Type* et = nullptr;
            std::vector<AccessorRefArg> outArgs;
            llvm::Value* outAddr = resolveOutAddrWithBracket(
                *call.args[1].value, et, outArgs);
            if (!outAddr) {
                emitAccessorRefArgWriteBacks(outArgs, /*emit=*/false);
                return nullptr;
            }
            // If the out slot is not a guid-struct slot (different elem type),
            // use our own slot and copy.  Normally outAddr is already the right size.
            auto* fn = declareC("bz_guid_try_parse", i32(), {ptrTy(), ptrTy()});
            llvm::Value* ok = builder_.CreateCall(fn, {strVal, outAddr}, "guid.tryp.ok");
            // fix #1998 / §28.3 "Unbound owned temporaries" / §4.6 / §28.9
            // (v0.226): same producer-temp-consumed-only-by-a-borrow-parameter
            // shape as Guid.Parse above — TryParse's `s: string` is a BORROW
            // (bz_guid_try_parse only reads s->data; no `owned` on the
            // parameter, stdlib §2.6), so a fresh +1 string producer argument
            // (e.g. Guid.TryParse(g.ToString(), out r)) leaked before this
            // release. The bool result is returned by value, so the call's end
            // is the full-expression boundary for the argument (§28.3).
            if (isPlusOneStringProducer(parseArg))
                emitStringRelease(strVal);
            // fix #2938 / §22.3: flush any bracket on the normal-return edge.
            emitAccessorRefArgWriteBacks(outArgs, /*emit=*/true);
            return builder_.CreateICmpNE(ok, builder_.getInt32(0), "guid.tryp.bool");
        }
        // per stdlib §2.6 / #1357: Guid.FromByteArray(bytes: Array<byte>): Guid
        // "borrows; requires Length = 16, ArgumentException otherwise" — guard
        // BEFORE the 16-byte copy (a shorter array would be an out-of-bounds
        // read in bz_guid_from_bytes).
        if (m == "FromByteArray" && call.args.size() == 1 && call.args[0].value) {
            llvm::Value* arr = lowerExpr(*call.args[0].value);
            if (!arr) return nullptr;
            auto* lenFn = declareC("bz_array_len", i64(), {ptrTy()});
            llvm::Value* len = builder_.CreateCall(lenFn, {arr}, "guid.fb.len");
            emitGuardThrow(
                builder_.CreateICmpNE(len, builder_.getInt64(16), "guid.fb.bad"),
                "ArgumentException");
            auto* ptrFn = declareC("bz_array_ptr_at", ptrTy(),
                                   {ptrTy(), i64(), i64()});
            llvm::Value* bytesPtr = builder_.CreateCall(
                ptrFn, {arr, builder_.getInt64(0), builder_.getInt64(1)},
                "guid.fb.ptr");
            llvm::AllocaInst* slot = makeGuidSlot();
            auto* fn = declareC("bz_guid_from_bytes",
                                 llvm::Type::getVoidTy(llvm_), {ptrTy(), ptrTy()});
            builder_.CreateCall(fn, {bytesPtr, slot});
            return builder_.CreateLoad(gst, slot, "guid.frombytes");
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
        // per §2.5 / #648: DateTime.Parse(s: string): DateTime — accepts the
        // type's own rendered forms (round-trip `yyyy-MM-ddTHH:mm:ss`, an
        // optional `.fff` fractional-seconds tail, and the date-only form
        // `yyyy-MM-dd`); any other input raises a catchable FormatException
        // (stdlib §2.5, v0.197). bz_datetime_try_parse returns 1 on success, 0
        // on failure; guard on that and throw via the standard builtin-throw
        // path.
        //
        // Before #2009 the failure edge emitted CreateUnreachable — an
        // uncatchable trap where the spec promises a catchable exception, so a
        // `catch FormatException` never fired and control flowed past the
        // `unreachable` block into undefined code → wild read / SIGSEGV (ASan
        // "unknown-crash"). Routing through emitGuardThrow makes the fault a
        // first-class, catchable throw like every other builtin guard (mirrors
        // #2004 Guid.Parse).
        if (m == "Parse" && call.args.size() == 1 && call.args[0].value) {
            llvm::Value* strVal = lowerExpr(*call.args[0].value);
            if (!strVal) return nullptr;
            // Call bz_datetime_try_parse(str_p, &out_ticks) to validate+get ticks.
            auto* ticksSlot = entryAlloca(i64());
            auto* tryFn = declareC("bz_datetime_try_parse", i32(),
                                   {ptrTy(), ptrTy()});
            llvm::Value* ok = builder_.CreateCall(tryFn, {strVal, ticksSlot},
                                                  "dt.parse.ok");
            llvm::Value* failed =
                builder_.CreateICmpEQ(ok, builder_.getInt32(0), "dt.parse.bad");
            emitGuardThrow(failed, "FormatException");  // §2.5, v0.197, #2009
            return builder_.CreateLoad(i64(), ticksSlot, "dt.parsed");
        }
        // per §2.5 / #648: DateTime.TryParse(s: string, out result: DateTime): bool
        if (m == "TryParse" && call.args.size() == 2 &&
            call.args[0].value && call.args[1].value) {
            llvm::Value* strVal = lowerExpr(*call.args[0].value);
            if (!strVal) return nullptr;
            // fix #2938 / §22.3: see Convert.TryToInt32 above.
            llvm::Type* et = nullptr;
            std::vector<AccessorRefArg> outArgs;
            llvm::Value* outAddr = resolveOutAddrWithBracket(
                *call.args[1].value, et, outArgs);
            if (!outAddr) {
                emitAccessorRefArgWriteBacks(outArgs, /*emit=*/false);
                return nullptr;
            }
            auto* tryFn = declareC("bz_datetime_try_parse", i32(),
                                   {ptrTy(), ptrTy()});
            llvm::Value* ok = builder_.CreateCall(tryFn, {strVal, outAddr},
                                                  "dt.tryp.ok");
            // fix #2938 / §22.3: flush any bracket on the normal-return edge.
            emitAccessorRefArgWriteBacks(outArgs, /*emit=*/true);
            return builder_.CreateICmpNE(ok, builder_.getInt32(0), "dt.tryp.bool");
        }
        // per stdlib §2.5 / #1357: DateTime.FromUnixTimeSeconds(seconds: long): DateTime
        if (m == "FromUnixTimeSeconds" && call.args.size() == 1 && call.args[0].value) {
            llvm::Value* sec = lowerExpr(*call.args[0].value);
            if (!sec) return nullptr;
            if (!sec->getType()->isIntegerTy(64))
                sec = builder_.CreateSExt(sec, i64());
            auto* fn = declareC("bz_datetime_from_unix_seconds", i64(), {i64()});
            return builder_.CreateCall(fn, {sec}, "dt.fromux");
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
        // per stdlib §2.5 / #1357: TimeSpan.FromTicks(ticks: long): TimeSpan (identity).
        if (m == "FromTicks" && call.args.size() == 1 && call.args[0].value) {
            llvm::Value* v = lowerExpr(*call.args[0].value);
            if (!v) return nullptr;
            if (!v->getType()->isIntegerTy(64))
                v = builder_.CreateSExt(v, i64());
            return v;
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
        // §7.3: File.ReadAllBytes(path) → owned Array<byte> (bz_array_t* elem_size=1).
        if (m == "ReadAllBytes" && call.args.size() == 1 && call.args[0].value) {
            llvm::Value* s = lowerExpr(*call.args[0].value);
            if (!s) return nullptr;
            auto* fn = declareC("bz_file_read_all_bytes_arr", ptrTy(), {ptrTy()});
            return builder_.CreateCall(fn, {strToPath(s)}, "bytes.arr");
        }
        // §7.3: File.WriteAllBytes(path, contents: Array<byte>).
        // The `contents` arg may be an Array<byte> (bz_array_t* ptr) or a
        // ReadOnlyArraySpan<byte> struct value (source, start, len).  Both
        // cases are handled: the span case extracts the raw data pointer and
        // byte count and calls bz_file_write_all_bytes directly (avoiding the
        // bz_array_t wrapper entirely).
        if (m == "WriteAllBytes" && call.args.size() == 2 &&
            call.args[0].value && call.args[1].value) {
            llvm::Value* pathStr = lowerExpr(*call.args[0].value);
            llvm::Value* contents = lowerExpr(*call.args[1].value);
            if (!pathStr || !contents) return nullptr;
            llvm::Value* pathC = strToPath(pathStr);
            if (contents->getType()->isStructTy()) {
                // Span arg: extract bz_array_t* source, byte start, length.
                llvm::Value* src   = spanSource(contents);
                llvm::Value* start = builder_.CreateZExt(spanStart(contents), i64());
                llvm::Value* slen  = builder_.CreateZExt(spanLength(contents), i64());
                // data ptr = arr->data + start (elem_size=1 for byte)
                auto* lenFn = declareC("bz_array_len", i64(), {ptrTy()});
                auto* ptrFn = declareC("bz_array_ptr_at", ptrTy(), {ptrTy(), i64(), i64()});
                (void)lenFn;  // used for bounds in production; skip check here
                llvm::Value* dataPtr = builder_.CreateCall(ptrFn,
                    {src, start, builder_.getInt64(1)}, "span.data");
                auto* fn = declareC("bz_file_write_all_bytes", i32(),
                                    {ptrTy(), ptrTy(), i64()});
                return builder_.CreateCall(fn, {pathC, dataPtr, slen});
            }
            // Array<byte> arg: bz_array_t* passed directly.
            auto* fn = declareC("bz_file_write_all_bytes_arr", i32(),
                                {ptrTy(), ptrTy()});
            return builder_.CreateCall(fn, {pathC, contents});
        }
        // §7.3: File.WriteAllLines(path, lines: IEnumerable<string>).
        // In practice callers pass Array<string> (bz_array_t* of bz_string*).
        if (m == "WriteAllLines" && call.args.size() == 2 &&
            call.args[0].value && call.args[1].value) {
            llvm::Value* path = lowerExpr(*call.args[0].value);
            llvm::Value* arr  = lowerExpr(*call.args[1].value);
            if (!path || !arr) return nullptr;
            auto* fn = declareC("bz_file_write_all_lines", i32(),
                                {ptrTy(), ptrTy()});
            return builder_.CreateCall(fn, {strToPath(path), arr});
        }
        // §7.3: File.Copy(source, destination, overwrite: bool).
        if (m == "Copy" && call.args.size() == 3 &&
            call.args[0].value && call.args[1].value && call.args[2].value) {
            llvm::Value* src  = lowerExpr(*call.args[0].value);
            llvm::Value* dst  = lowerExpr(*call.args[1].value);
            llvm::Value* over = lowerExpr(*call.args[2].value);
            if (!src || !dst || !over) return nullptr;
            auto* fn = declareC("bz_file_copy", i32(),
                                {ptrTy(), ptrTy(), i1()});
            return builder_.CreateCall(fn, {strToPath(src), strToPath(dst), over});
        }
        // §7.3: File.Move(source, destination).
        if (m == "Move" && call.args.size() == 2 &&
            call.args[0].value && call.args[1].value) {
            llvm::Value* src = lowerExpr(*call.args[0].value);
            llvm::Value* dst = lowerExpr(*call.args[1].value);
            if (!src || !dst) return nullptr;
            auto* fn = declareC("bz_file_move", i32(), {ptrTy(), ptrTy()});
            return builder_.CreateCall(fn, {strToPath(src), strToPath(dst)});
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
                    recvTy->headName() == "Random") {
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
                    // per stdlib §2.7 / #1359: NextInt64() → long.
                    if (mname == "NextInt64" && call.args.empty()) {
                        auto* fn = declareC("bz_random_next_long", i64(), {ptrTy()});
                        return builder_.CreateCall(fn, {rng}, "rng.i64");
                    }
                    // per stdlib §2.7 / #1359: NextBoolean() → bool.
                    if (mname == "NextBoolean" && call.args.empty()) {
                        auto* fn = declareC("bz_random_next_bool", i32(), {ptrTy()});
                        llvm::Value* r = builder_.CreateCall(fn, {rng}, "rng.b");
                        return builder_.CreateICmpNE(r, builder_.getInt32(0), "rng.bool");
                    }
                    // per stdlib §2.7 / #1359: NextBytes(buffer: Array<byte>) → unit.
                    if (mname == "NextBytes" && call.args.size() == 1 && call.args[0].value) {
                        llvm::Value* arr = lowerExpr(*call.args[0].value);
                        if (!arr) return nullptr;
                        // Get array length and data pointer.
                        auto* lenFn = declareC("bz_array_len", i64(), {ptrTy()});
                        auto* ptrFn = declareC("bz_array_ptr_at", ptrTy(),
                                               {ptrTy(), i64(), i64()});
                        llvm::Value* arrLen = builder_.CreateCall(lenFn, {arr}, "rng.arrlen");
                        llvm::Value* arrPtr = builder_.CreateCall(
                            ptrFn, {arr, builder_.getInt64(0), builder_.getInt64(1)},
                            "rng.arrptr");
                        auto* fn = declareC("bz_random_next_bytes",
                                            llvm::Type::getVoidTy(llvm_),
                                            {ptrTy(), ptrTy(), i64()});
                        builder_.CreateCall(fn, {rng, arrPtr, arrLen});
                        return nullptr;  // unit
                    }
                }
            }
        }
    }

    // ---------------------------------------------------------------------------
    // §8.1 / issue #646: Stopwatch instance methods.
    // Receiver's exprType name-part is "Stopwatch".
    // ---------------------------------------------------------------------------
    {
        if (auto* ma2 = dynamic_cast<const ast::MemberAccessExpr*>(call.callee.get())) {
            if (ma2->target) {
                auto* recvTy = ctx_.exprType(*ma2->target);
                bool isSW = recvTy && recvTy->kind == sema::TypeRef::Kind::Named &&
                    builtinTypeName(recvTy) == "Stopwatch";
                if (isSW) {
                    llvm::Value* sw = lowerExpr(*ma2->target);
                    if (!sw) return nullptr;
                    const auto& mname = ma2->member;
                    if (mname == "Start" && call.args.empty()) {
                        auto* fn = declareC("bz_stopwatch_start",
                            llvm::Type::getVoidTy(llvm_), {ptrTy()});
                        builder_.CreateCall(fn, {sw});
                        return nullptr;
                    }
                    if (mname == "Stop" && call.args.empty()) {
                        auto* fn = declareC("bz_stopwatch_stop",
                            llvm::Type::getVoidTy(llvm_), {ptrTy()});
                        builder_.CreateCall(fn, {sw});
                        return nullptr;
                    }
                    if (mname == "Reset" && call.args.empty()) {
                        auto* fn = declareC("bz_stopwatch_reset",
                            llvm::Type::getVoidTy(llvm_), {ptrTy()});
                        builder_.CreateCall(fn, {sw});
                        return nullptr;
                    }
                    if (mname == "Restart" && call.args.empty()) {
                        auto* fn = declareC("bz_stopwatch_restart",
                            llvm::Type::getVoidTy(llvm_), {ptrTy()});
                        builder_.CreateCall(fn, {sw});
                        return nullptr;
                    }
                }
            }
        }
    }

    // ---------------------------------------------------------------------------
    // stdlib §7.1/§7.2 / issue #645: FileStream instance methods.
    // Receiver's exprType name-part is "FileStream" (a bz_filestream_t*).
    // ---------------------------------------------------------------------------
    {
        if (auto* ma2 = dynamic_cast<const ast::MemberAccessExpr*>(call.callee.get())) {
            if (ma2->target &&
                ctx_.exprType(*ma2->target) &&
                builtinTypeName(ctx_.exprType(*ma2->target)) == "FileStream") {
                llvm::Value* fs = lowerExpr(*ma2->target);
                if (!fs) return nullptr;
                const auto& mname = ma2->member;
                // Lower an int-typed argument to i32.
                auto argI32 = [&](size_t i) -> llvm::Value* {
                    if (i >= call.args.size() || !call.args[i].value) return nullptr;
                    llvm::Value* v = lowerExpr(*call.args[i].value);
                    if (!v || !v->getType()->isIntegerTy()) return nullptr;
                    if (v->getType()->isIntegerTy(32)) return v;
                    return v->getType()->getIntegerBitWidth() < 32
                        ? builder_.CreateSExt(v, i32())
                        : builder_.CreateTrunc(v, i32());
                };
                // per stdlib §7.1: Read(buffer, offset, count): int and
                // Write(buffer, offset, count). The runtime validates the
                // window [offset, offset+count) against the buffer's Count and
                // reports an out-of-range window as -1 WITHOUT performing any
                // I/O; §7.1 makes that IndexOutOfRangeException.
                if ((mname == "Read" || mname == "Write") &&
                    call.args.size() == 3) {
                    llvm::Value* buf = call.args[0].value
                        ? lowerExpr(*call.args[0].value) : nullptr;
                    llvm::Value* off = argI32(1);
                    llvm::Value* cnt = argI32(2);
                    if (!buf || !off || !cnt) return nullptr;
                    if (mname == "Read") {
                        auto* fn = declareC("bz_filestream_read", i64(),
                                            {ptrTy(), ptrTy(), i32(), i32()});
                        llvm::Value* n =
                            builder_.CreateCall(fn, {fs, buf, off, cnt}, "fs.read");
                        emitGuardThrow(
                            builder_.CreateICmpSLT(n, builder_.getInt64(0), "fs.read.oor"),
                            "IndexOutOfRangeException");
                        return builder_.CreateTrunc(n, i32(), "fs.read.n");
                    }
                    auto* fn = declareC("bz_filestream_write", i32(),
                                        {ptrTy(), ptrTy(), i32(), i32()});
                    llvm::Value* st =
                        builder_.CreateCall(fn, {fs, buf, off, cnt}, "fs.write");
                    // Only the -1 (window) status raises. The -2 short/failed
                    // write is swallowed for now: §7.1's "write the entire
                    // window or raise" needs the §7.5 IOException surface the
                    // whole §7 runtime still lacks (#2286, the same deferral
                    // §7.3 already records).
                    emitGuardThrow(
                        builder_.CreateICmpEQ(st, builder_.getInt32(-1), "fs.write.oor"),
                        "IndexOutOfRangeException");
                    return nullptr;
                }
                if (mname == "Flush" && call.args.empty()) {
                    auto* fn = declareC("bz_filestream_flush",
                        llvm::Type::getVoidTy(llvm_), {ptrTy()});
                    builder_.CreateCall(fn, {fs});
                    return nullptr;
                }
                // per stdlib §7.1: Seek(offset: long, origin: SeekOrigin): long.
                if (mname == "Seek" && call.args.size() == 2 &&
                    call.args[0].value && call.args[1].value) {
                    llvm::Value* off = lowerExpr(*call.args[0].value);
                    llvm::Value* org = lowerExpr(*call.args[1].value);
                    if (!off || !org) return nullptr;
                    if (!off->getType()->isIntegerTy(64))
                        off = builder_.CreateSExt(off, i64());
                    if (!org->getType()->isIntegerTy(32))
                        org = builder_.CreateZExt(org, i32());
                    auto* fn = declareC("bz_filestream_seek", i64(),
                                        {ptrTy(), i64(), i32()});
                    return builder_.CreateCall(fn, {fs, off, org}, "fs.seek");
                }
                // per stdlib §7.1: ReadAll(): owned Array<byte>.
                if (mname == "ReadAll" && call.args.empty()) {
                    auto* fn = declareC("bz_filestream_read_all", ptrTy(), {ptrTy()});
                    return builder_.CreateCall(fn, {fs}, "fs.readall");
                }
            }
        }
    }

    // #1025: the CancellationTokenSource / CancellationToken instance-method and
    // property handlers were MOVED to before the `if (!id) return nullptr` guard
    // above — they accept ANY receiver expression (a chained `cts.Token` is a
    // MemberAccessExpr, not an IdentifierExpr), and the guard was making the
    // chained form (`cts.Token.ThrowIfCancellationRequested()`) silently no-op.

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
                    // per stdlib §2.5 / #1357: CompareTo(other: DateTime): int.
                    // Inline: (a > b) - (a < b) on i64.
                    if (mname == "CompareTo" && call.args.size() == 1 && call.args[0].value) {
                        llvm::Value* other = lowerExpr(*call.args[0].value);
                        if (!other) return nullptr;
                        if (!other->getType()->isIntegerTy(64))
                            other = builder_.CreateSExt(other, i64());
                        llvm::Value* gt = builder_.CreateZExt(
                            builder_.CreateICmpSGT(ticks, other), i32(), "dt.cmp.gt");
                        llvm::Value* lt = builder_.CreateZExt(
                            builder_.CreateICmpSLT(ticks, other), i32(), "dt.cmp.lt");
                        return builder_.CreateSub(gt, lt, "dt.cmp");
                    }
                    // per stdlib §2.5 / #1357: ToUnixTimeSeconds(): long.
                    if (mname == "ToUnixTimeSeconds" && call.args.empty()) {
                        auto* fn = declareC("bz_datetime_to_unix_seconds", i64(), {i64()});
                        return builder_.CreateCall(fn, {ticks}, "dt.toux");
                    }
                    // per stdlib §2.5 (#1114): ToString() no-arg — ISO 8601
                    // default format "yyyy-MM-ddTHH:mm:ss" (19 chars).
                    // Reuses the same bz_datetime_to_string_fmt helper that
                    // ToString(format) uses; no new runtime function needed.
                    if (mname == "ToString" && call.args.empty()) {
                        llvm::Value* fmtStr = lowerStringLiteral(
                            "\"yyyy-MM-ddTHH:mm:ss\"", false);
                        auto* fn = declareC("bz_datetime_to_string_fmt",
                                            ptrTy(), {i64(), ptrTy()});
                        return builder_.CreateCall(fn, {ticks, fmtStr},
                                                   "dt.tostr.default");
                    }
                    // per §2.5 / #648 / §2.18 v0.230 / #3292: ToString(format)
                    // — expand format specifiers via bz_datetime_to_string_fmt.
                    // The runtime returns nullptr on a malformed specifier run
                    // (§2.18 strict-fault); check for nullptr and raise
                    // FormatException via emitGuardThrow, matching the decimal
                    // pattern at bz_decimal_to_string_fmt.
                    if (mname == "ToString" && call.args.size() == 1 &&
                        call.args[0].value) {
                        llvm::Value* fmtStr = lowerExpr(*call.args[0].value);
                        if (!fmtStr) return nullptr;
                        auto* fn = declareC("bz_datetime_to_string_fmt", ptrTy(),
                                            {i64(), ptrTy()});
                        llvm::Value* res = builder_.CreateCall(fn, {ticks, fmtStr},
                                                               "dt.tostr.fmt");
                        llvm::Value* isNull = builder_.CreateICmpEQ(
                            res,
                            llvm::Constant::getNullValue(ptrTy()),
                            "dt.fmt.null");
                        emitGuardThrow(isNull, "FormatException");
                        return res;
                    }
                }
            }
        }
    }

    // ---------------------------------------------------------------------------
    // per §2.5 / #1357: TimeSpan instance methods — CompareTo and ToString.
    // ---------------------------------------------------------------------------
    {
        if (auto* ma2 = dynamic_cast<const ast::MemberAccessExpr*>(call.callee.get())) {
            if (ma2->target) {
                auto* recvTy = ctx_.exprType(*ma2->target);
                bool isTS = recvTy && builtinTypeName(recvTy) == "TimeSpan";
                if (isTS) {
                    llvm::Value* ticks = lowerExpr(*ma2->target);
                    if (!ticks) return nullptr;
                    const auto& mname = ma2->member;
                    // CompareTo(other: TimeSpan): int — inline (a>b)-(a<b).
                    if (mname == "CompareTo" && call.args.size() == 1 && call.args[0].value) {
                        llvm::Value* other = lowerExpr(*call.args[0].value);
                        if (!other) return nullptr;
                        if (!other->getType()->isIntegerTy(64))
                            other = builder_.CreateSExt(other, i64());
                        llvm::Value* gt = builder_.CreateZExt(
                            builder_.CreateICmpSGT(ticks, other), i32(), "ts.cmp.gt");
                        llvm::Value* lt = builder_.CreateZExt(
                            builder_.CreateICmpSLT(ticks, other), i32(), "ts.cmp.lt");
                        return builder_.CreateSub(gt, lt, "ts.cmp");
                    }
                    // ToString(): string — emit "d.hh:mm:ss" format.
                    if (mname == "ToString" && call.args.empty()) {
                        // Build "±d.hh:mm:ss" via snprintf in a stack buffer.
                        auto* i32s  = llvm::Type::getInt32Ty(llvm_);
                        llvm::Value* dS  = entryAlloca(i32s);
                        llvm::Value* hS  = entryAlloca(i32s);
                        llvm::Value* miS = entryAlloca(i32s);
                        llvm::Value* sS  = entryAlloca(i32s);
                        llvm::Value* msS = entryAlloca(i32s);
                        auto* compFn = declareC("bz_timespan_components",
                            llvm::Type::getVoidTy(llvm_),
                            {i64(), ptrTy(), ptrTy(), ptrTy(), ptrTy(), ptrTy()});
                        builder_.CreateCall(compFn, {ticks, dS, hS, miS, sS, msS});
                        llvm::Value* days  = builder_.CreateLoad(i32s, dS, "ts.d");
                        llvm::Value* hours = builder_.CreateLoad(i32s, hS, "ts.h");
                        llvm::Value* mins  = builder_.CreateLoad(i32s, miS, "ts.mi");
                        llvm::Value* secs  = builder_.CreateLoad(i32s, sS, "ts.s");
                        // Use snprintf to format; 64-byte buffer is ample.
                        auto* i8t = llvm::Type::getInt8Ty(llvm_);
                        auto* buf64 = entryAlloca(llvm::ArrayType::get(i8t, 64));
                        llvm::Value* bufPtr = builder_.CreateGEP(
                            llvm::ArrayType::get(i8t, 64), buf64,
                            {builder_.getInt64(0), builder_.getInt64(0)});
                        auto* snpFn = module_->getFunction("snprintf");
                        if (!snpFn) {
                            auto* ft = llvm::FunctionType::get(i32(), {ptrTy(), i64(), ptrTy()}, true);
                            snpFn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "snprintf", module_.get());
                        }
                        unsigned sidTS = stringLiteralIdx_++;
                        llvm::Constant* fmtTS = llvm::ConstantDataArray::getString(llvm_, "%d.%02d:%02d:%02d", true);
                        auto* fmtGV = new llvm::GlobalVariable(*module_, fmtTS->getType(), true,
                            llvm::GlobalValue::PrivateLinkage, fmtTS, ".bz.ts.fmt." + std::to_string(sidTS));
                        fmtGV->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
                        llvm::Value* written = builder_.CreateCall(snpFn,
                            {bufPtr, builder_.getInt64(64), fmtGV, days, hours, mins, secs});
                        auto* mallocFn2 = declareC("malloc", ptrTy(), {i64()});
                        auto* memcpyFn2 = declareC("memcpy", ptrTy(), {ptrTy(), ptrTy(), i64()});
                        llvm::Value* len2 = builder_.CreateSExt(written, i64());
                        llvm::Value* dataSz2 = builder_.CreateAdd(len2, builder_.getInt64(1));
                        llvm::Value* dataCopy2 = builder_.CreateCall(mallocFn2, {dataSz2}, "ts.str.data");
                        builder_.CreateCall(memcpyFn2, {dataCopy2, bufPtr, len2});
                        llvm::Value* nulPtr2 = builder_.CreateGEP(i8t, dataCopy2, len2);
                        builder_.CreateStore(llvm::ConstantInt::get(i8t, 0), nulPtr2);
                        uint64_t strBytesTS = module_->getDataLayout().getTypeAllocSize(stringType());
                        llvm::Value* strObj2 = builder_.CreateCall(mallocFn2, {builder_.getInt64(strBytesTS)}, "ts.str.obj");
                        builder_.CreateStore(dataCopy2, builder_.CreateStructGEP(stringType(), strObj2, 0));
                        builder_.CreateStore(len2, builder_.CreateStructGEP(stringType(), strObj2, 1));
                        builder_.CreateStore(llvm::ConstantInt::get(i8t, 1), builder_.CreateStructGEP(stringType(), strObj2, 2));
                        initStringRc(strObj2);
                        return strObj2;
                    }
                }
            }
        }
    }

    // ---------------------------------------------------------------------------
    // per §2.5 / #1357: DateOnly instance methods.
    // DateOnly is stored as i32 day-number.
    // ---------------------------------------------------------------------------
    {
        if (auto* ma2 = dynamic_cast<const ast::MemberAccessExpr*>(call.callee.get())) {
            if (ma2->target) {
                auto* recvTy = ctx_.exprType(*ma2->target);
                bool isDO = recvTy && builtinTypeName(recvTy) == "DateOnly";
                if (isDO) {
                    llvm::Value* dayNum = lowerExpr(*ma2->target);
                    if (!dayNum) return nullptr;
                    if (!dayNum->getType()->isIntegerTy(32))
                        dayNum = builder_.CreateTrunc(dayNum, i32());
                    const auto& mname = ma2->member;
                    // CompareTo(other: DateOnly): int — inline (a>b)-(a<b) on i32.
                    if (mname == "CompareTo" && call.args.size() == 1 && call.args[0].value) {
                        llvm::Value* other = lowerExpr(*call.args[0].value);
                        if (!other) return nullptr;
                        if (!other->getType()->isIntegerTy(32))
                            other = builder_.CreateTrunc(other, i32());
                        llvm::Value* gt = builder_.CreateZExt(
                            builder_.CreateICmpSGT(dayNum, other), i32(), "do.cmp.gt");
                        llvm::Value* lt = builder_.CreateZExt(
                            builder_.CreateICmpSLT(dayNum, other), i32(), "do.cmp.lt");
                        return builder_.CreateSub(gt, lt, "do.cmp");
                    }
                    // ToString(): string — "yyyy-MM-dd" format.
                    if (mname == "ToString" && call.args.empty()) {
                        llvm::Value* dn64 = builder_.CreateSExt(dayNum, i64(), "do.dn64");
                        llvm::Value* tpd = builder_.getInt64(864000000000LL);
                        llvm::Value* dticks = builder_.CreateMul(dn64, tpd, "do.ticks");
                        llvm::Value* fmtStr = lowerStringLiteral("\"yyyy-MM-dd\"", false);
                        auto* fn = declareC("bz_datetime_to_string_fmt", ptrTy(), {i64(), ptrTy()});
                        return builder_.CreateCall(fn, {dticks, fmtStr}, "do.tostr");
                    }
                    // ToDateTime(time: TimeOnly): DateTime — merge day number and ms.
                    if (mname == "ToDateTime" && call.args.size() == 1 && call.args[0].value) {
                        llvm::Value* ms = lowerExpr(*call.args[0].value);
                        if (!ms) return nullptr;
                        if (!ms->getType()->isIntegerTy(32))
                            ms = builder_.CreateTrunc(ms, i32());
                        // ticks = dayNum * TICKS_PER_DAY + ms * TICKS_PER_MS
                        llvm::Value* dn64 = builder_.CreateSExt(dayNum, i64(), "do.td.dn");
                        llvm::Value* ms64 = builder_.CreateSExt(ms, i64(), "do.td.ms");
                        llvm::Value* tpd = builder_.getInt64(864000000000LL);
                        llvm::Value* tpm = builder_.getInt64(10000LL);
                        return builder_.CreateAdd(
                            builder_.CreateMul(dn64, tpd),
                            builder_.CreateMul(ms64, tpm), "do.todt");
                    }
                }
            }
        }
    }

    // ---------------------------------------------------------------------------
    // per §2.5 / #1357: DateOnly static methods.
    // ---------------------------------------------------------------------------
    if (cls == "DateOnly") {
        // DateOnly.Parse(s: string): DateOnly — accepts the type's own rendered
        // form `yyyy-MM-dd` (and the datetime forms bz_datetime_try_parse
        // recognises, whose time-of-day is then truncated by the /TICKS_PER_DAY
        // below); any other input raises a catchable FormatException (stdlib
        // §2.5, v0.197). bz_datetime_try_parse returns 1 on success, 0 on
        // failure; guard on that and throw via the standard builtin-throw path.
        //
        // Before #2015 the failure edge emitted CreateUnreachable — an
        // uncatchable trap where the spec promises a catchable exception, so a
        // `catch FormatException` never fired and control flowed past the
        // `unreachable` block into undefined code → wild read / SIGSEGV (ASan
        // "unknown-crash"). Routing through emitGuardThrow makes the fault a
        // first-class, catchable throw (mirrors #2009 DateTime.Parse / #2004
        // Guid.Parse).
        if (m == "Parse" && call.args.size() == 1 && call.args[0].value) {
            llvm::Value* strVal = lowerExpr(*call.args[0].value);
            if (!strVal) return nullptr;
            // Parse date using bz_datetime_try_parse then divide by TICKS_PER_DAY.
            auto* ticksSlot = entryAlloca(i64());
            auto* tryFn = declareC("bz_datetime_try_parse", i32(), {ptrTy(), ptrTy()});
            llvm::Value* ok = builder_.CreateCall(tryFn, {strVal, ticksSlot}, "do.parse.ok");
            llvm::Value* failed =
                builder_.CreateICmpEQ(ok, builder_.getInt32(0), "do.parse.bad");
            emitGuardThrow(failed, "FormatException");  // §2.5, v0.197, #2015
            llvm::Value* ticks = builder_.CreateLoad(i64(), ticksSlot, "do.parsed");
            llvm::Value* tpd = builder_.getInt64(864000000000LL);
            return builder_.CreateTrunc(builder_.CreateSDiv(ticks, tpd), i32(), "do.dn");
        }
        // DateOnly.TryParse(s: string, out result: DateOnly): bool
        if (m == "TryParse" && call.args.size() == 2 && call.args[0].value && call.args[1].value) {
            llvm::Value* strVal = lowerExpr(*call.args[0].value);
            if (!strVal) return nullptr;
            auto* ticksSlot = entryAlloca(i64());
            auto* tryFn = declareC("bz_datetime_try_parse", i32(), {ptrTy(), ptrTy()});
            llvm::Value* ok = builder_.CreateCall(tryFn, {strVal, ticksSlot}, "do.tryp.ok");
            // fix #2938 / §22.3: see Convert.TryToDecimal above. Pre-fix was
            // `if (outAddrdo)` (write dropped silently on null lvalueAddr); with
            // the bracket the slot is always present and the writeback flushes
            // below.
            llvm::Type* etdo = nullptr;
            std::vector<AccessorRefArg> outArgs;
            llvm::Value* outAddrdo = resolveOutAddrWithBracket(
                *call.args[1].value, etdo, outArgs);
            if (!outAddrdo) {
                emitAccessorRefArgWriteBacks(outArgs, /*emit=*/false);
                return nullptr;
            }
            llvm::Value* ticks = builder_.CreateLoad(i64(), ticksSlot, "do.tryp.ticks");
            llvm::Value* tpd = builder_.getInt64(864000000000LL);
            llvm::Value* dn = builder_.CreateTrunc(
                builder_.CreateSDiv(ticks, tpd), i32(), "do.tryp.dn");
            llvm::Value* okBool = builder_.CreateICmpNE(ok, builder_.getInt32(0));
            llvm::Value* sel = builder_.CreateSelect(okBool, dn, builder_.getInt32(0));
            builder_.CreateStore(sel, outAddrdo);
            // fix #2938 / §22.3: flush any bracket on the normal-return edge.
            emitAccessorRefArgWriteBacks(outArgs, /*emit=*/true);
            return builder_.CreateICmpNE(ok, builder_.getInt32(0), "do.tryp.bool");
        }
    }

    // ---------------------------------------------------------------------------
    // per §2.5 / #1357: TimeOnly instance methods.
    // TimeOnly is stored as i32 ms from midnight.
    // ---------------------------------------------------------------------------
    {
        if (auto* ma2 = dynamic_cast<const ast::MemberAccessExpr*>(call.callee.get())) {
            if (ma2->target) {
                auto* recvTy = ctx_.exprType(*ma2->target);
                bool isTO = recvTy && builtinTypeName(recvTy) == "TimeOnly";
                if (isTO) {
                    llvm::Value* ms = lowerExpr(*ma2->target);
                    if (!ms) return nullptr;
                    if (!ms->getType()->isIntegerTy(32))
                        ms = builder_.CreateTrunc(ms, i32());
                    const auto& mname = ma2->member;
                    // CompareTo(other: TimeOnly): int — inline (a>b)-(a<b) on i32.
                    if (mname == "CompareTo" && call.args.size() == 1 && call.args[0].value) {
                        llvm::Value* other = lowerExpr(*call.args[0].value);
                        if (!other) return nullptr;
                        if (!other->getType()->isIntegerTy(32))
                            other = builder_.CreateTrunc(other, i32());
                        llvm::Value* gt = builder_.CreateZExt(
                            builder_.CreateICmpSGT(ms, other), i32(), "to.cmp.gt");
                        llvm::Value* lt = builder_.CreateZExt(
                            builder_.CreateICmpSLT(ms, other), i32(), "to.cmp.lt");
                        return builder_.CreateSub(gt, lt, "to.cmp");
                    }
                    // ToString(): string — "HH:mm:ss" format.
                    if (mname == "ToString" && call.args.empty()) {
                        auto* fn = declareC("bz_timeonly_to_string", ptrTy(), {i32()});
                        return builder_.CreateCall(fn, {ms}, "to.tostr");
                    }
                }
            }
        }
    }

    // ---------------------------------------------------------------------------
    // per §2.5 / #1357: TimeOnly static methods.
    // ---------------------------------------------------------------------------
    if (cls == "TimeOnly") {
        // TimeOnly.Parse(s: string): TimeOnly — accepts the type's own rendered
        // form `HH:mm:ss` with an optional `.fff` tail; any other input raises a
        // catchable FormatException (stdlib §2.5, v0.197). bz_timeonly_try_parse
        // returns 1 on success, 0 on failure; guard on that and throw via the
        // standard builtin-throw path.
        //
        // Before #2015 the failure edge emitted CreateUnreachable — an
        // uncatchable trap where the spec promises a catchable exception, so a
        // `catch FormatException` never fired and control flowed past the
        // `unreachable` block into undefined code → wild read / SIGSEGV (ASan
        // "unknown-crash"). Routing through emitGuardThrow makes the fault a
        // first-class, catchable throw (mirrors #2009 DateTime.Parse / #2004
        // Guid.Parse).
        if (m == "Parse" && call.args.size() == 1 && call.args[0].value) {
            llvm::Value* strVal = lowerExpr(*call.args[0].value);
            if (!strVal) return nullptr;
            auto* msSlot = entryAlloca(i32());
            auto* tryFn = declareC("bz_timeonly_try_parse", i32(), {ptrTy(), ptrTy()});
            llvm::Value* ok = builder_.CreateCall(tryFn, {strVal, msSlot}, "to.parse.ok");
            llvm::Value* failed =
                builder_.CreateICmpEQ(ok, builder_.getInt32(0), "to.parse.bad");
            emitGuardThrow(failed, "FormatException");  // §2.5, v0.197, #2015
            return builder_.CreateLoad(i32(), msSlot, "to.parsed");
        }
        // TimeOnly.TryParse(s: string, out result: TimeOnly): bool
        if (m == "TryParse" && call.args.size() == 2 && call.args[0].value && call.args[1].value) {
            llvm::Value* strVal = lowerExpr(*call.args[0].value);
            if (!strVal) return nullptr;
            auto* msSlot = entryAlloca(i32());
            auto* tryFn = declareC("bz_timeonly_try_parse", i32(), {ptrTy(), ptrTy()});
            llvm::Value* ok = builder_.CreateCall(tryFn, {strVal, msSlot}, "to.tryp.ok");
            // fix #2938 / §22.3: see Convert.TryToDecimal above.
            llvm::Type* etto = nullptr;
            std::vector<AccessorRefArg> outArgs;
            llvm::Value* outAddrto = resolveOutAddrWithBracket(
                *call.args[1].value, etto, outArgs);
            if (!outAddrto) {
                emitAccessorRefArgWriteBacks(outArgs, /*emit=*/false);
                return nullptr;
            }
            llvm::Value* ms = builder_.CreateLoad(i32(), msSlot, "to.tryp.ms");
            llvm::Value* okBool = builder_.CreateICmpNE(ok, builder_.getInt32(0));
            llvm::Value* sel = builder_.CreateSelect(okBool, ms, builder_.getInt32(0));
            builder_.CreateStore(sel, outAddrto);
            // fix #2938 / §22.3: flush any bracket on the normal-return edge.
            emitAccessorRefArgWriteBacks(outArgs, /*emit=*/true);
            return builder_.CreateICmpNE(ok, builder_.getInt32(0), "to.tryp.bool");
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
                        // fix #2938 / §22.3: a property or indexer slot target
                        // is admitted by §22.3 and resolves through the §22.3
                        // copy-in/copy-out bracket; `lvalueAddr` alone declines,
                        // so the builtin call bailed (#2434) and the write-back
                        // silently dropped. `outArgs` carries the bracket record
                        // for the normal-return write-back flush below.
                        llvm::Type* slotTy = nullptr;
                        std::vector<AccessorRefArg> outArgs;
                        llvm::Value* outAddr = resolveOutAddrWithBracket(
                            *call.args[0].value, slotTy, outArgs);
                        if (!outAddr) {
                            emitAccessorRefArgWriteBacks(outArgs,
                                                         /*emit=*/false);
                            return builder_.getFalse();
                        }
                        auto* fn = declareC("bz_weak_try_upgrade", i32(),
                                            {ptrTy(), ptrTy()});
                        llvm::Value* r = builder_.CreateCall(fn, {handle, outAddr},
                                                             "weak.tryupgrade");
                        // fix #2938 / §22.3: flush any bracket on the
                        // normal-return edge.
                        emitAccessorRefArgWriteBacks(outArgs, /*emit=*/true);
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


}  // namespace bezel::codegen
