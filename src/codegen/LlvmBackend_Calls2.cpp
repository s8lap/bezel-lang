#include "codegen/LlvmBackend_impl.hpp"

namespace bezel::codegen {


// stdlib §3.2 v0.136: Encoding.{Utf8,Utf16,Utf32,Ascii}.GetBytes/GetString/Name.
// The call's callee is `(Encoding.Utf8).GetBytes` — a member access whose TARGET
// is itself the member access `Encoding.Utf8`. We match that two-level shape:
//   ma          = (Encoding.Utf8).GetBytes        (the callee)
//   ma->target  = Encoding.Utf8                    (a MemberAccessExpr)
//   inner       = Encoding.Utf8 — inner->target is the `Encoding` identifier,
//                 inner->member is the singleton name (Utf8/Utf16/Utf32/Ascii).
// All four singletons are implemented (see bz_encoding_* in bezel_runtime.cpp).
// For Utf16/Utf32/Ascii: GetString and GetBytes may return null on encoding
// error; codegen checks for null and emits emitGuardThrow("FormatException").
llvm::Value* Emitter::tryLowerEncodingCall(const ast::CallExpr& call) {
    if (!call.callee) return nullptr;
    auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(call.callee.get());
    if (!ma || !ma->target) return nullptr;
    auto* inner = dynamic_cast<const ast::MemberAccessExpr*>(ma->target.get());
    if (!inner || !inner->target) return nullptr;
    auto* clsId = dynamic_cast<const ast::IdentifierExpr*>(inner->target.get());
    if (!clsId || clsId->name != "Encoding") return nullptr;

    const auto& sg = inner->member;  // "UTF8", "UTF16", "UTF32", or "ASCII"
    const auto& m  = ma->member;     // "GetBytes", "GetString", or "Name"

    // Determine runtime function names and whether GetBytes/GetString can fail.
    // All four encodings follow the strict error model (stdlib §3.2 v0.136):
    // GetString returns null on ill-formed input (incl. UTF8 — fix #1144), and
    // ASCII GetBytes returns null on unrepresentable code points. The runtimes
    // that never fail (UTF8/UTF16/UTF32 GetBytes) just never produce a null, so
    // the null-check guard is dead for them — harmless and uniform.
    bool canFail = true;
    std::string getBytesName = "bz_encoding_" +
        (sg == "UTF8"  ? std::string("utf8")  :
         sg == "UTF16" ? std::string("utf16") :
         sg == "UTF32" ? std::string("utf32") :
                         std::string("ascii")) + "_get_bytes";
    std::string getStringName = "bz_encoding_" +
        (sg == "UTF8"  ? std::string("utf8")  :
         sg == "UTF16" ? std::string("utf16") :
         sg == "UTF32" ? std::string("utf32") :
                         std::string("ascii")) + "_get_string";

    if (sg != "UTF8" && sg != "UTF16" && sg != "UTF32" && sg != "ASCII")
        return nullptr;

    // GetBytes(s): owned Array<byte>.  Returns null on failure for strict
    // encodings; codegen adds emitGuardThrow(null_check, "FormatException").
    if (m == "GetBytes" && call.args.size() == 1 && call.args[0].value) {
        llvm::Value* strPtr = lowerExpr(*call.args[0].value);
        if (!strPtr) return nullptr;
        auto* fn = declareC(getBytesName.c_str(), ptrTy(), {ptrTy()});
        llvm::Value* result = builder_.CreateCall(fn, {strPtr}, "enc.bytes");
        if (canFail) {
            // Null return → FormatException (stdlib §3.2 v0.136 error model).
            llvm::Value* isNull = builder_.CreateICmpEQ(
                result, llvm::Constant::getNullValue(ptrTy()), "enc.fail");
            emitGuardThrow(isNull, "FormatException");
        }
        return result;
    }

    // GetByteCount(s): int — count bytes needed without allocating.
    // v0.165 (#1355): new 1-arg overload on all four encodings.
    if (m == "GetByteCount" && call.args.size() == 1 && call.args[0].value) {
        llvm::Value* strPtr = lowerExpr(*call.args[0].value);
        if (!strPtr) return nullptr;
        std::string cntFnName = "bz_encoding_" +
            (sg == "UTF8"  ? std::string("utf8")  :
             sg == "UTF16" ? std::string("utf16") :
             sg == "UTF32" ? std::string("utf32") :
                             std::string("ascii")) + "_get_byte_count";
        auto* cntFn = declareC(cntFnName.c_str(), i64(), {ptrTy()});
        llvm::Value* cnt = builder_.CreateCall(cntFn, {strPtr}, "enc.cnt");
        // ASCII: -1 means a code point > U+007F → FormatException (§3.2 v0.136).
        if (sg == "ASCII") {
            llvm::Value* isFail =
                builder_.CreateICmpSLT(cnt, builder_.getInt64(0), "enc.asc.fail");
            emitGuardThrow(isFail, "FormatException");
        }
        // Spec says int; the runtime returns i64; truncate to i32.
        return builder_.CreateTrunc(cnt, i32(), "enc.cnt32");
    }

    // GetBytes(s, destination: Span<byte>): int — v0.165 (#1355) 2-arg overload.
    // Encodes `s` into an existing writable byte span; returns bytes written.
    // IndexOutOfRangeException if the span is shorter than the encoded length.
    if (m == "GetBytes" && call.args.size() == 2 &&
        call.args[0].value && call.args[1].value) {
        llvm::Value* strPtr  = lowerExpr(*call.args[0].value);
        llvm::Value* spanVal = lowerExpr(*call.args[1].value);
        if (!strPtr || !spanVal) return nullptr;
        // Count bytes needed.
        std::string cntFnName = "bz_encoding_" +
            (sg == "UTF8"  ? std::string("utf8")  :
             sg == "UTF16" ? std::string("utf16") :
             sg == "UTF32" ? std::string("utf32") :
                             std::string("ascii")) + "_get_byte_count";
        auto* cntFn = declareC(cntFnName.c_str(), i64(), {ptrTy()});
        llvm::Value* needed = builder_.CreateCall(cntFn, {strPtr}, "enc.sp.needed");
        // ASCII: -1 means non-ASCII code point → FormatException.
        if (sg == "ASCII") {
            llvm::Value* isFail =
                builder_.CreateICmpSLT(needed, builder_.getInt64(0), "enc.sp.afail");
            emitGuardThrow(isFail, "FormatException");
        }
        // Unpack span struct { ptr source, i32 start, i32 length }.
        auto* spanSt = arraySpanStructType();
        auto* spanSlot = entryAlloca(spanSt);
        builder_.CreateStore(spanVal, spanSlot);
        llvm::Value* spanSrc = builder_.CreateLoad(ptrTy(),
            builder_.CreateStructGEP(spanSt, spanSlot, 0), "enc.sp.src");
        llvm::Value* spanStart = builder_.CreateLoad(i32(),
            builder_.CreateStructGEP(spanSt, spanSlot, 1), "enc.sp.st");
        llvm::Value* spanLen = builder_.CreateLoad(i32(),
            builder_.CreateStructGEP(spanSt, spanSlot, 2), "enc.sp.len");
        // Bounds: span length (as i64) must be >= needed.
        llvm::Value* spanLen64 = builder_.CreateSExt(spanLen, i64(), "enc.sp.len64");
        emitGuardThrow(
            builder_.CreateICmpSLT(spanLen64, needed), "IndexOutOfRangeException");
        // Get raw byte pointer into the backing array at spanStart.
        auto* ptrAtFn = declareC("bz_array_ptr_at", ptrTy(), {ptrTy(), i64(), i64()});
        llvm::Value* spanStart64 = builder_.CreateSExt(spanStart, i64());
        llvm::Value* rawPtr = builder_.CreateCall(
            ptrAtFn, {spanSrc, spanStart64, builder_.getInt64(1)}, "enc.sp.raw");
        // Write bytes into the destination buffer.
        std::string wrtFnName = "bz_encoding_" +
            (sg == "UTF8"  ? std::string("utf8")  :
             sg == "UTF16" ? std::string("utf16") :
             sg == "UTF32" ? std::string("utf32") :
                             std::string("ascii")) + "_write_bytes";
        auto* wrtFn = declareC(wrtFnName.c_str(),
            llvm::Type::getVoidTy(llvm_), {ptrTy(), ptrTy()});
        builder_.CreateCall(wrtFn, {strPtr, rawPtr});
        // Return bytes written as int (truncated from i64).
        return builder_.CreateTrunc(needed, i32(), "enc.sp.written");
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
            // stdlib §3.2 v0.165 (#1355): the (bytes, index, count) window ranges
            // over the array's Count; an out-of-range window raises
            // IndexOutOfRangeException (like every other bounds rule in the
            // library). Guard `index < 0 || count < 0 || index + count > Count`
            // before the decode so an out-of-range request faults rather than
            // reading past the backing storage.
            auto* lenFn = declareC("bz_array_len", i64(), {ptrTy()});
            llvm::Value* alen =
                builder_.CreateCall(lenFn, {arr}, "enc.gs.len");
            llvm::Value* idxNeg =
                builder_.CreateICmpSLT(index, builder_.getInt64(0));
            llvm::Value* cntNeg =
                builder_.CreateICmpSLT(count, builder_.getInt64(0));
            llvm::Value* endPos =
                builder_.CreateAdd(index, count, "enc.gs.end");
            llvm::Value* over = builder_.CreateICmpSGT(endPos, alen);
            llvm::Value* bad = builder_.CreateOr(
                builder_.CreateOr(idxNeg, cntNeg), over, "enc.gs.oor");
            emitGuardThrow(bad, "IndexOutOfRangeException");
        }
        auto* fn = declareC(getStringName.c_str(), ptrTy(),
                            {ptrTy(), i64(), i64()});
        llvm::Value* result = builder_.CreateCall(fn, {arr, index, count},
                                                  "enc.str");
        if (canFail) {
            // Null return → FormatException (stdlib §3.2 v0.136 error model).
            llvm::Value* isNull = builder_.CreateICmpEQ(
                result, llvm::Constant::getNullValue(ptrTy()), "enc.fail");
            emitGuardThrow(isNull, "FormatException");
        }
        return result;
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
    // §11.8 (v0.176, #807): `T.Parse(...)` / `T.Zero()` / … — a static
    // interface member called through a constrained TYPE PARAMETER, at a
    // specialization where T is a PRIMITIVE (the compiler-provided §2.19
    // conformances). Resolve T through the active substitution and lower
    // exactly like the concrete primitive's static (the existing per-member
    // handlers below). Non-primitive substitutions fall through to the
    // user-static redirect in lowerCall; sema guarantees only conforming
    // primitives reach a §2.19-bearing constraint.
    if (pt == sema::PrimitiveType::None &&
        dynamic_cast<sema::GenericParamSymbol*>(ctx_.exprSymbol(*id))) {
        auto sit = genericSubstSemaTy_.find(id->name);
        if (sit != genericSubstSemaTy_.end() && sit->second &&
            sit->second->kind == sema::TypeRef::Kind::Primitive)
            pt = sit->second->primitive;
    }
    if (pt == sema::PrimitiveType::None) return nullptr;
    const auto& m = ma->member;
    using P = sema::PrimitiveType;

    // §11.8 / stdlib §2.19 (v0.176, #807): the compiler-provided conformance
    // members of the primitive tower. Reached only through a constrained type
    // parameter (sema rejects `int.Zero()` in ordinary code — §3.4's closed
    // static-member sets are unchanged).
    //   INumber<T>.Zero() / One()  → the primitive's 0 / 1 constant
    //   ISteppable<T>.Step(v, by)  → advance in T's space (v + by)
    if ((m == "Zero" || m == "One") && call.args.empty()) {
        const bool one = (m == "One");
        if (pt == P::Decimal) {
            if (one) return emitDecimalOne();
            llvm::AllocaInst* slot = entryAlloca(decimalStructType());
            emitIntToDecimal(builder_.getInt32(0), P::Int, slot);
            return builder_.CreateLoad(decimalStructType(), slot, "dec.zero");
        }
        llvm::Type* t = lowerPrimitive(pt);
        if (t->isFloatingPointTy())
            return llvm::ConstantFP::get(t, one ? 1.0 : 0.0);
        if (t->isIntegerTy())
            return llvm::ConstantInt::get(t, one ? 1 : 0);
        return nullptr;
    }
    if (m == "Step" && call.args.size() == 2 && call.args[0].value &&
        call.args[1].value) {
        llvm::Value* v = lowerExpr(*call.args[0].value);
        llvm::Value* by = lowerExpr(*call.args[1].value);
        if (!v || !by) return nullptr;
        // `by` is declared `long` — normalize to i64 first.
        if (by->getType()->isIntegerTy() && !by->getType()->isIntegerTy(64))
            by = builder_.CreateSExt(by, i64(), "step.by64");
        if (pt == P::Decimal) {
            // decimal: promote `by` and add via the runtime (§6.4 semantics).
            llvm::StructType* dst = decimalStructType();
            llvm::AllocaInst* bySlot = entryAlloca(dst);
            emitIntToDecimal(by, P::Long, bySlot);
            llvm::AllocaInst* aSlot = entryAlloca(dst);
            llvm::AllocaInst* oSlot = entryAlloca(dst);
            builder_.CreateStore(v, aSlot);
            auto vt = [&]() -> llvm::Value* {
                auto it = builtinVtable_.find("OverflowException");
                if (it == builtinVtable_.end())
                    return llvm::ConstantPointerNull::get(ptrTy());
                return it->second;
            };
            auto* fn = declareC("bz_decimal_add", llvm::Type::getVoidTy(llvm_),
                                {ptrTy(), ptrTy(), ptrTy(), ptrTy()});
            builder_.CreateCall(fn, {aSlot, bySlot, oSlot, vt()});
            emitPendingCheck();
            return builder_.CreateLoad(dst, oSlot, "dec.step");
        }
        if (v->getType()->isFloatingPointTy()) {
            llvm::Value* byFp =
                builder_.CreateSIToFP(by, v->getType(), "step.byfp");
            return builder_.CreateFAdd(v, byFp, "step.f");
        }
        if (v->getType()->isIntegerTy()) {
            // Integers and char: advance the value (code point) by `by`,
            // performed at the value's own width (wrapping, like `+`).
            unsigned w = v->getType()->getIntegerBitWidth();
            llvm::Value* byW =
                w < 64 ? builder_.CreateTrunc(by, v->getType(), "step.byw")
                       : by;
            return builder_.CreateAdd(v, byW, "step.i");
        }
        return nullptr;
    }
    // stdlib §9.5 / §2.19: char.Parse / char.TryParse — a one-code-point
    // string yields that code point; anything else is FormatException /
    // false. (Reached via IParsable<char>'s compiler-provided conformance;
    // also completes the lowercase `char.Parse(s)` static surface.)
    if (pt == P::Char && m == "Parse" && call.args.size() == 1 &&
        call.args[0].value) {
        llvm::Value* s = lowerExpr(*call.args[0].value);
        if (!s) return nullptr;
        llvm::Value* len = loadStringLen(s);
        llvm::Value* isOne = builder_.CreateICmpEQ(
            len, builder_.getInt64(1), "chp.one");
        emitGuardThrow(builder_.CreateNot(isOne, "chp.bad"),
                       "FormatException");
        return loadCodePoint(s, builder_.getInt64(0));
    }
    if (pt == P::Char && m == "TryParse" && call.args.size() == 2 &&
        call.args[0].value && call.args[1].value) {
        llvm::Value* s = lowerExpr(*call.args[0].value);
        if (!s) return nullptr;
        llvm::Type* et = nullptr;
        llvm::Value* outAddr = lvalueAddr(*call.args[1].value, et);
        if (!outAddr) return nullptr;
        llvm::Value* len = loadStringLen(s);
        llvm::Value* ok = builder_.CreateICmpEQ(
            len, builder_.getInt64(1), "chtp.ok");
        // out = the sole code point on success, '\0' (the default) on failure.
        llvm::Value* cp0 = loadCodePoint(s, builder_.getInt64(0));
        llvm::Value* sel = builder_.CreateSelect(
            ok, cp0, llvm::ConstantInt::get(cp0->getType(), 0), "chtp.sel");
        // §11.8 / §2.19 (#1470): nullable-companion aware store for `T.TryParse`
        // dispatched through IParsable<char> into a generic `out T?`.
        storeTryParseOut(outAddr, et, sel, ok);
        return ok;
    }
    // stdlib §9.3 / §2.19: decimal.TryParse(s, out result): bool — routes to
    // bz_decimal_try_parse (no exception on failure; out gets 0 per §9.3).
    if (pt == P::Decimal && m == "TryParse" && call.args.size() == 2 &&
        call.args[0].value && call.args[1].value) {
        llvm::Value* s = lowerExpr(*call.args[0].value);
        if (!s) return nullptr;
        llvm::Type* et = nullptr;
        llvm::Value* outAddr = lvalueAddr(*call.args[1].value, et);
        if (!outAddr) return nullptr;
        auto* fn = declareC("bz_decimal_try_parse", i32(), {ptrTy(), ptrTy()});
        // §11.8 / §2.19 (#1470): the runtime writes a BARE decimal through the
        // pointer it is given. When the out slot is a nullable companion `{i8,
        // decimal}` (generic IParsable<decimal> dispatch into a `out T?`),
        // let it write into a scratch bare-decimal slot, then marshal that into
        // the companion with hasValue = ok. A direct `decimal.TryParse` slot is a
        // bare decimal — write straight through it.
        bool decNullable =
            et && et->isStructTy() &&
            llvm::cast<llvm::StructType>(et)->getNumElements() == 2 &&
            llvm::cast<llvm::StructType>(et)->getElementType(0) ==
                nullablePrimHasValTy();
        llvm::Value* target = outAddr;
        llvm::AllocaInst* decScratch = nullptr;
        if (decNullable) {
            decScratch = entryAlloca(decimalStructType());
            target = decScratch;
        }
        llvm::Value* r = builder_.CreateCall(fn, {s, target}, "dectp.r");
        llvm::Value* okDec =
            builder_.CreateICmpNE(r, builder_.getInt32(0), "dectp.ok");
        if (decScratch) {
            llvm::Value* dv =
                builder_.CreateLoad(decimalStructType(), decScratch, "dectp.v");
            storeTryParseOut(outAddr, et, dv, okDec);
        }
        return okDec;
    }

    // §8.1 / §8.4 / §8.6 (#1119; updated v0.182 / #1579): Object.ReferenceEquals
    // (a, b): bool — compares object identity by boxing composition (§8.6).
    // Classes / interfaces / Object? / a `null` literal all lower to a raw
    // pointer already (their "box" is themselves), so the identity test is a
    // pointer `icmp eq` (a null reference compares equal to another null,
    // true). A NON-NULLABLE value-type argument boxes to a FRESH heap object
    // per §8.6, so it never aliases anything else — including another
    // occurrence of an equal-valued argument, since each occurrence boxes
    // independently — so its mere presence forces the result to `false`
    // without needing to actually allocate a box (no observable behavior
    // depends on the box's identity beyond "it is fresh"). A NULLABLE
    // value-type argument (`T?`) that is null is different (the v0.182 fix):
    // it boxes to the null reference (§8.6 Nullable value-type boxing), not
    // to "always false" as the pre-v0.182 wording said — so
    // `ReferenceEquals(x, null)` is `true` for a null `int?` `x`. Each operand
    // therefore reduces to (present: i1, identityPtr: ptr): a real
    // pointer operand is never "present" in this sense (its identity IS the
    // pointer, null or not); a nullable-value operand's `present` is its
    // runtime has-value flag, which gates the whole comparison to `false`
    // when set and contributes a null-pointer identity when clear.
    if (pt == P::Object && m == "ReferenceEquals" && call.args.size() == 2 &&
        call.args[0].value && call.args[1].value) {
        llvm::Value* a = lowerExpr(*call.args[0].value);
        llvm::Value* b = lowerExpr(*call.args[1].value);
        if (!a || !b) return nullptr;
        const sema::TypeRef* tyA = ctx_.exprType(*call.args[0].value);
        const sema::TypeRef* tyB = ctx_.exprType(*call.args[1].value);
        bool nullableValA = !a->getType()->isPointerTy() && isNullableValueTy(tyA);
        bool nullableValB = !b->getType()->isPointerTy() && isNullableValueTy(tyB);
        // A non-pointer operand that is NOT the nullable-value tagged struct
        // is a plain non-nullable value type: always a fresh, never-aliasing
        // box, so the result is unconditionally false.
        if ((!a->getType()->isPointerTy() && !nullableValA) ||
            (!b->getType()->isPointerTy() && !nullableValB))
            return builder_.getFalse();
        llvm::Value* presentA = nullableValA ? nullablePrimHasValue(a)
                                              : builder_.getFalse();
        llvm::Value* presentB = nullableValB ? nullablePrimHasValue(b)
                                              : builder_.getFalse();
        llvm::Value* ptrA = nullableValA
            ? llvm::ConstantPointerNull::get(ptrTy())
            : a;
        llvm::Value* ptrB = nullableValB
            ? llvm::ConstantPointerNull::get(ptrTy())
            : b;
        llvm::Value* neitherPresent = builder_.CreateAnd(
            builder_.CreateNot(presentA), builder_.CreateNot(presentB),
            "refeq.neitherpresent");
        llvm::Value* ptrEq = builder_.CreateICmpEQ(ptrA, ptrB, "refeq.ptr");
        return builder_.CreateAnd(neitherPresent, ptrEq, "refeq");
    }

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
        else if (m == "IsControl") fnName = "iscntrl";   // stdlib §9.5 (sweep #7)
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

    // §9.6 (#747 R43): string.From(buffer: Array<char>, start, count): string —
    // build a string from the char array's code points at slots [start,
    // start+count). The 3-arg overload had NO codegen handler, so the call
    // produced a garbage string pointer that SIGSEGV'd on first use. Lowers to
    // bz_str_from_char_array (the inverse of ToCharArray). Out-of-range start /
    // count raise IndexOutOfRangeException (the §4.6 / stdlib bounds convention),
    // checked here against the array's Count before the runtime call.
    if (pt == P::String && m == "From" && call.args.size() == 3 &&
        call.args[0].value && call.args[1].value && call.args[2].value) {
        llvm::Value* arr   = lowerExpr(*call.args[0].value);
        llvm::Value* start = lowerExpr(*call.args[1].value);
        llvm::Value* count = lowerExpr(*call.args[2].value);
        if (!arr || !start || !count) return nullptr;
        llvm::Value* start64 = builder_.CreateSExt(start, i64(), "from.start");
        llvm::Value* count64 = builder_.CreateSExt(count, i64(), "from.count");
        auto* lenFn = declareC("bz_array_len", i64(), {ptrTy()});
        llvm::Value* n = builder_.CreateCall(lenFn, {arr}, "from.n");
        // start in [0, n]; count >= 0; start + count <= n.
        llvm::Value* end = builder_.CreateAdd(start64, count64, "from.end");
        llvm::Value* oor = builder_.CreateOr(
            builder_.CreateICmpSLT(start64, builder_.getInt64(0), "from.sneg"),
            builder_.CreateOr(
                builder_.CreateICmpSGT(start64, n, "from.sbig"),
                builder_.CreateOr(
                    builder_.CreateICmpSLT(count64, builder_.getInt64(0),
                                           "from.cneg"),
                    builder_.CreateICmpSGT(end, n, "from.ebig"), "from.c"),
                "from.b"),
            "from.oor");
        emitGuardThrow(oor, "IndexOutOfRangeException");
        auto* fn = declareC("bz_str_from_char_array", ptrTy(),
                            {ptrTy(), i64(), i64()});
        return builder_.CreateCall(fn, {arr, start64, count64}, "str.from");
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

    // double.TryParse / float.TryParse(s, out result): bool — stdlib §9.2
    // #1227: on failure, out result receives the default (0.0), not the partial
    // parse value (e.g. 1.5 for "1.5abc"). Per §9.3: "TryParse returns false
    // in every case where Parse would raise"; the out param gets the default.
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
        auto* endSlotD = entryAlloca(ptrTy());
        llvm::Value* valD = builder_.CreateCall(
            strtodFn, {strData, endSlotD}, "tryp.d");
        llvm::Value* endPtrD = builder_.CreateLoad(ptrTy(), endSlotD);
        // ok = (endPtr != strData) AND (*endPtr == '\0') AND first char not whitespace
        // per §9.3: no surrounding whitespace is accepted.
        auto* i8TyD = llvm::Type::getInt8Ty(llvm_);
        llvm::Value* fcD = builder_.CreateLoad(i8TyD, strData, "tryp.fc");
        llvm::Value* fcWsD = builder_.CreateICmpULE(
            fcD, llvm::ConstantInt::get(i8TyD, ' '), "tryp.fws");
        llvm::Value* advD = builder_.CreateICmpNE(endPtrD, strData, "tryp.adv");
        llvm::Value* endChD = builder_.CreateLoad(i8TyD, endPtrD, "tryp.end");
        llvm::Value* atNulD = builder_.CreateICmpEQ(
            endChD, llvm::ConstantInt::get(i8TyD, 0), "tryp.nul");
        llvm::Value* okD = builder_.CreateAnd(
            builder_.CreateNot(fcWsD),
            builder_.CreateAnd(advD, atNulD), "tryp.ok");
        // Store 0.0 on failure, parsed value on success (§9.3: out = default on false).
        auto* dblTy = llvm::Type::getDoubleTy(llvm_);
        llvm::Value* zeroD = llvm::ConstantFP::get(dblTy, 0.0);
        llvm::Value* selD = builder_.CreateSelect(okD, valD, zeroD, "tryp.sel");
        // §11.8 / §2.19 (#1470): nullable-companion aware store (generic
        // IParsable<double>/<float> dispatch into a `out T?`); bare otherwise.
        if (pt == P::Float) {
            auto* fltTy = llvm::Type::getFloatTy(llvm_);
            storeTryParseOut(outAddr, et,
                             builder_.CreateFPTrunc(selD, fltTy), okD);
        } else {
            storeTryParseOut(outAddr, et, selD, okD);
        }
        return okD;
    }

    // Integer TryParse(s, out result): bool — stdlib §9.1. Returns true and
    // sets result if s is a valid integer in the target type's range; false
    // and sets result to 0 (default) otherwise.
    // #1227: on failure, out result receives 0, not the partial parse value.
    // Per §9.3: "TryParse returns false in every case where Parse would raise";
    // the out param gets the default.  No surrounding whitespace is accepted.
    // §2.19 (v0.176, #807): extended from int/long to the whole integer tower
    // (byte…ulong) — IParsable<T>'s compiler-provided conformance needs every
    // tower member; each narrower width range-checks the strtoll result the
    // same way int always did (#1134). ulong parses via strtoull (full width).
    if (m == "TryParse" && call.args.size() == 2 &&
        call.args[0].value && call.args[1].value &&
        (pt == P::Int || pt == P::Long || pt == P::UInt || pt == P::ULong ||
         pt == P::Short || pt == P::UShort || pt == P::Byte ||
         pt == P::SByte)) {
        llvm::Value* strVal = lowerExpr(*call.args[0].value);
        if (!strVal) return nullptr;
        llvm::Value* strData = loadStringData(strVal);
        // Get address of the out parameter.
        llvm::Type* et = nullptr;
        llvm::Value* outAddr = lvalueAddr(*call.args[1].value, et);
        if (!outAddr) return nullptr;
        // strtoll/strtoull(data, &endptr, 10) for validation.
        auto* strtoFnT = (pt == P::ULong)
            ? declareC("strtoull", i64(), {ptrTy(), ptrTy(), i32()})
            : declareC("strtoll", i64(), {ptrTy(), ptrTy(), i32()});
        auto* endSlotI = entryAlloca(ptrTy());
        llvm::Value* val64 = builder_.CreateCall(
            strtoFnT, {strData, endSlotI, builder_.getInt32(10)}, "tryp.val");
        llvm::Value* endPtrI = builder_.CreateLoad(ptrTy(), endSlotI);
        // ok = first-char not whitespace AND (endPtr != strData) AND (*endPtr == '\0')
        auto* i8TyI = llvm::Type::getInt8Ty(llvm_);
        llvm::Value* fcI = builder_.CreateLoad(i8TyI, strData, "tryp.fc");
        llvm::Value* fcWsI = builder_.CreateICmpULE(
            fcI, llvm::ConstantInt::get(i8TyI, ' '), "tryp.fws");
        llvm::Value* advI = builder_.CreateICmpNE(endPtrI, strData, "tryp.adv");
        llvm::Value* endChI = builder_.CreateLoad(i8TyI, endPtrI, "tryp.end");
        llvm::Value* atNulI = builder_.CreateICmpEQ(
            endChI, llvm::ConstantInt::get(i8TyI, 0), "tryp.nul");
        llvm::Value* ok = builder_.CreateAnd(
            builder_.CreateNot(fcWsI),
            builder_.CreateAnd(advI, atNulI), "tryp.ok");
        // Store result: 0 on failure, parsed value on success.
        if (pt == P::Long || pt == P::ULong) {
            // ulong additionally rejects a leading '-' (strtoull silently
            // negates it); long is the full strtoll width — no range check.
            if (pt == P::ULong) {
                llvm::Value* fcNeg = builder_.CreateICmpEQ(
                    fcI, llvm::ConstantInt::get(i8TyI, '-'), "tryp.neg");
                ok = builder_.CreateAnd(ok, builder_.CreateNot(fcNeg),
                                        "tryp.okU");
            }
            llvm::Value* zero64 = builder_.getInt64(0);
            llvm::Value* sel64 = builder_.CreateSelect(ok, val64, zero64, "tryp.sel");
            // §11.8 / §2.19 (#1470): marshal into a nullable companion when the
            // out slot is `T?` (generic IParsable dispatch); bare store otherwise.
            storeTryParseOut(outAddr, et, sel64, ok);
        } else {
            // #1134 / stdlib §9.1: TryParse must return FALSE for an
            // out-of-range value, not silently wrap the low bits. strtoll on a
            // 64-bit platform returns the full magnitude, so range-check it
            // against the target type's bounds.
            int64_t rlo, rhi;
            switch (pt) {
                case P::Int:    rlo = INT32_MIN; rhi = INT32_MAX;  break;
                case P::UInt:   rlo = 0;         rhi = UINT32_MAX; break;
                case P::Short:  rlo = INT16_MIN; rhi = INT16_MAX;  break;
                case P::UShort: rlo = 0;         rhi = UINT16_MAX; break;
                case P::Byte:   rlo = 0;         rhi = UINT8_MAX;  break;
                default:        rlo = INT8_MIN;  rhi = INT8_MAX;   break; // SByte
            }
            llvm::Value* inRange = builder_.CreateAnd(
                builder_.CreateICmpSGE(
                    val64, llvm::ConstantInt::getSigned(i64(), rlo)),
                builder_.CreateICmpSLE(
                    val64, llvm::ConstantInt::getSigned(i64(), rhi)),
                "tryp.inrange");
            ok = builder_.CreateAnd(ok, inRange, "tryp.ok2");
            // On failure, store 0; on success store the truncated value.
            llvm::Type* tt = lowerPrimitive(pt);
            llvm::Value* truncV = builder_.CreateTrunc(val64, tt);
            llvm::Value* zeroV = llvm::ConstantInt::get(tt, 0);
            llvm::Value* selV = builder_.CreateSelect(ok, truncV, zeroV, "tryp.sel");
            // §11.8 / §2.19 (#1470): nullable-companion aware store (see above).
            storeTryParseOut(outAddr, et, selV, ok);
        }
        return ok;
    }

    // §9.4 / issue #654: bool.TryParse(s: string, out result: bool): bool.
    // Accepts "true"/"false" case-insensitively; everything else → false, ok=false.
    // Unlike Convert.ToBoolean, "1" is NOT accepted (only the two canonical words).
    if (pt == P::Bool && m == "TryParse" && call.args.size() == 2 &&
        call.args[0].value && call.args[1].value) {
        llvm::Value* strVal = lowerExpr(*call.args[0].value);
        if (!strVal) return nullptr;
        llvm::Value* strData = loadStringData(strVal);
        llvm::Type* et = nullptr;
        llvm::Value* outAddr = lvalueAddr(*call.args[1].value, et);
        if (!outAddr) return nullptr;
        auto* strcasecmpFn = declareC("strcasecmp", i32(), {ptrTy(), ptrTy()});
        // Build "true" and "false" constant C-strings.
        llvm::Constant* trueStr = llvm::ConstantDataArray::getString(llvm_, "true", true);
        unsigned tid = stringLiteralIdx_++;
        auto* trueGV = new llvm::GlobalVariable(*module_, trueStr->getType(), true,
            llvm::GlobalValue::PrivateLinkage, trueStr, ".bz.btp.true." + std::to_string(tid));
        llvm::Constant* falseStr = llvm::ConstantDataArray::getString(llvm_, "false", true);
        auto* falseGV = new llvm::GlobalVariable(*module_, falseStr->getType(), true,
            llvm::GlobalValue::PrivateLinkage, falseStr, ".bz.btp.false." + std::to_string(tid));
        // cmpTrue = (strcasecmp(strData, "true") == 0)
        llvm::Value* cmpTrue = builder_.CreateICmpEQ(
            builder_.CreateCall(strcasecmpFn, {strData, trueGV}),
            builder_.getInt32(0), "btp.isTrue");
        // cmpFalse = (strcasecmp(strData, "false") == 0)
        llvm::Value* cmpFalse = builder_.CreateICmpEQ(
            builder_.CreateCall(strcasecmpFn, {strData, falseGV}),
            builder_.getInt32(0), "btp.isFalse");
        // ok = cmpTrue || cmpFalse
        llvm::Value* ok = builder_.CreateOr(cmpTrue, cmpFalse, "btp.ok");
        // result = cmpTrue (true iff "true"; false for "false" or garbage)
        // §11.8 / §2.19 (#1470): nullable-companion aware store for `T.TryParse`
        // dispatched through IParsable<bool> into a generic `out T?`.
        storeTryParseOut(outAddr, et, cmpTrue, ok);
        return ok;
    }

    // §9.4: bool.Parse(s: string): bool — "true"/"false" case-insensitive.
    // Same recognition as TryParse, but per the §9.x strict-fault Parse
    // contract (stdlib: "TryParse returns false in every case where Parse
    // would raise"), any input other than the two canonical words raises
    // FormatException rather than yielding a value. Without this handler the
    // call fell through to the generic `return arg` below and handed the
    // string pointer back as an i1, emitting malformed IR (`br ptr` — verifier
    // failure, would crash; an unimplemented-lowering bug, not a reject).
    if (pt == P::Bool && m == "Parse" && call.args.size() == 1 &&
        call.args[0].value) {
        llvm::Value* strVal = lowerExpr(*call.args[0].value);
        if (!strVal) return nullptr;
        llvm::Value* strData = loadStringData(strVal);
        auto* strcasecmpFn = declareC("strcasecmp", i32(), {ptrTy(), ptrTy()});
        llvm::Constant* trueStr = llvm::ConstantDataArray::getString(llvm_, "true", true);
        unsigned bid = stringLiteralIdx_++;
        auto* trueGV = new llvm::GlobalVariable(*module_, trueStr->getType(), true,
            llvm::GlobalValue::PrivateLinkage, trueStr, ".bz.bp.true." + std::to_string(bid));
        llvm::Constant* falseStr = llvm::ConstantDataArray::getString(llvm_, "false", true);
        auto* falseGV = new llvm::GlobalVariable(*module_, falseStr->getType(), true,
            llvm::GlobalValue::PrivateLinkage, falseStr, ".bz.bp.false." + std::to_string(bid));
        llvm::Value* cmpTrue = builder_.CreateICmpEQ(
            builder_.CreateCall(strcasecmpFn, {strData, trueGV}),
            builder_.getInt32(0), "bp.isTrue");
        llvm::Value* cmpFalse = builder_.CreateICmpEQ(
            builder_.CreateCall(strcasecmpFn, {strData, falseGV}),
            builder_.getInt32(0), "bp.isFalse");
        llvm::Value* ok = builder_.CreateOr(cmpTrue, cmpFalse, "bp.ok");
        // Neither "true" nor "false" → FormatException (strict-fault Parse).
        emitGuardThrow(builder_.CreateNot(ok, "bp.bad"), "FormatException");
        return cmpTrue;
    }

    // int.Parse(s, radix): base-N integer parse (§5.2). The two-argument form
    // reaches this primitive handler via the lowercase keyword receiver; without
    // it the call was dropped and the result read an uninitialized slot
    // (#747 R42). strtol with the given base, narrowed to the target width.
    if (m == "Parse" && call.args.size() == 2 && call.args[0].value &&
        call.args[1].value) {
        auto* argTy = ctx_.exprType(*call.args[0].value);
        if (argTy && argTy->kind == sema::TypeRef::Kind::Primitive &&
            argTy->primitive == P::String &&
            (pt == P::Int || pt == P::UInt || pt == P::Short ||
             pt == P::UShort || pt == P::Byte || pt == P::SByte ||
             pt == P::Long || pt == P::ULong)) {
            llvm::Value* sArg = lowerExpr(*call.args[0].value);
            llvm::Value* radix = lowerExpr(*call.args[1].value);
            if (!sArg || !radix) return nullptr;
            llvm::Value* data = loadStringData(sArg);
            // radix is `int` (i32) — strtol's `base` parameter.
            if (!radix->getType()->isIntegerTy(32))
                radix = builder_.CreateTrunc(radix, i32());
            auto* strtolFn = declareC("strtol", i64(), {ptrTy(), ptrTy(), i32()});
            llvm::Value* r = builder_.CreateCall(
                strtolFn,
                {data, llvm::ConstantPointerNull::get(ptrTy()), radix});
            llvm::Type* tt = lowerPrimitive(pt);
            return tt->isIntegerTy(64) ? r : builder_.CreateTrunc(r, tt);
        }
    }

    // int.Parse(s) / long.Parse(s) / double.Parse(s) etc.
    if (m == "Parse" && call.args.size() == 1 && call.args[0].value) {
        llvm::Value* arg = lowerExpr(*call.args[0].value);
        if (!arg) return nullptr;
        // #747 R42: `decimal.Parse(s)` reaches this primitive handler via the
        // lowercase keyword receiver (pt==Decimal). Route it to bz_decimal_parse
        // exactly like `Decimal.Parse` (§6.4 / fix #521); without this it fell
        // through to `return arg`, handing back the string pointer as a decimal
        // struct and crashing codegen with an IntegerType cast assertion.
        if (pt == P::Decimal) {
            llvm::AllocaInst* slot = entryAlloca(decimalStructType());
            auto vt = [&](const char* n) -> llvm::Value* {
                auto it = builtinVtable_.find(n);
                if (it == builtinVtable_.end())
                    return llvm::ConstantPointerNull::get(ptrTy());
                return it->second;
            };
            auto* fn = declareC("bz_decimal_parse",
                                llvm::Type::getVoidTy(llvm_),
                                {ptrTy(), ptrTy(), ptrTy(), ptrTy()});
            builder_.CreateCall(
                fn, {arg, slot, vt("OverflowException"), vt("FormatException")});
            emitPendingCheck();
            return builder_.CreateLoad(decimalStructType(), slot, "dec.parsed");
        }
        // If the argument is a string, use atoi/atoll/atof.
        auto* argTy = ctx_.exprType(*call.args[0].value);
        if (argTy && argTy->kind == sema::TypeRef::Kind::Primitive &&
            argTy->primitive == P::String) {
            llvm::Value* data = loadStringData(arg);
            if (pt == P::Int || pt == P::UInt || pt == P::Short ||
                pt == P::UShort || pt == P::Byte || pt == P::SByte) {
                // #1134 / stdlib §9.1: parse at full 64-bit width via strtoll,
                // then OverflowException if the value is outside the target
                // type's range — `atoi` silently truncated an out-of-range string
                // to the low 32 bits (e.g. int.Parse("99999999999") → garbage).
                // #1227 / §9.1: also reject empty, non-numeric, trailing-garbage,
                // and leading-whitespace input → FormatException.  Per §9.3 (the
                // normative "strict-fault model" that applies throughout §9.1):
                // surrounding whitespace is not accepted; callers that mean to
                // accept padded input trim explicitly.
                auto* strtollFnP =
                    declareC("strtoll", i64(), {ptrTy(), ptrTy(), i32()});
                auto* endSlotP = entryAlloca(ptrTy());
                llvm::Value* v64 = builder_.CreateCall(
                    strtollFnP, {data, endSlotP, builder_.getInt32(10)}, "parse.v");
                // FormatException guard: first char must not be whitespace, the
                // conversion must have advanced the pointer (at least one char
                // consumed), and there must be no trailing garbage.
                auto* i8TyP = llvm::Type::getInt8Ty(llvm_);
                llvm::Value* fcP = builder_.CreateLoad(i8TyP, data, "parse.fc");
                llvm::Value* fcWsP = builder_.CreateICmpULE(
                    fcP, llvm::ConstantInt::get(i8TyP, ' '), "parse.fws");
                llvm::Value* endPtrP = builder_.CreateLoad(ptrTy(), endSlotP, "parse.ep");
                llvm::Value* advP = builder_.CreateICmpNE(endPtrP, data, "parse.adv");
                llvm::Value* endChP = builder_.CreateLoad(i8TyP, endPtrP, "parse.ech");
                llvm::Value* atNulP = builder_.CreateICmpEQ(
                    endChP, llvm::ConstantInt::get(i8TyP, 0), "parse.nul");
                llvm::Value* validFmt = builder_.CreateAnd(
                    builder_.CreateNot(fcWsP),
                    builder_.CreateAnd(advP, atNulP), "parse.vfmt");
                emitGuardThrow(builder_.CreateNot(validFmt, "parse.fmt.bad"),
                               "FormatException");
                // Range check → OverflowException.
                int64_t lo, hi;
                switch (pt) {
                    case P::Int:    lo = INT32_MIN; hi = INT32_MAX;   break;
                    case P::UInt:   lo = 0;         hi = UINT32_MAX;  break;
                    case P::Short:  lo = INT16_MIN; hi = INT16_MAX;   break;
                    case P::UShort: lo = 0;         hi = UINT16_MAX;  break;
                    case P::Byte:   lo = 0;         hi = UINT8_MAX;   break;
                    default:        lo = INT8_MIN;  hi = INT8_MAX;    break; // SByte
                }
                llvm::Value* tooLow = builder_.CreateICmpSLT(
                    v64, llvm::ConstantInt::getSigned(i64(), lo), "parse.lo");
                llvm::Value* tooHigh = builder_.CreateICmpSGT(
                    v64, llvm::ConstantInt::getSigned(i64(), hi), "parse.hi");
                emitGuardThrow(builder_.CreateOr(tooLow, tooHigh, "parse.ovf"),
                               "OverflowException");
                return (pt == P::Int || pt == P::UInt)
                           ? builder_.CreateTrunc(v64, i32())
                           : builder_.CreateTrunc(v64, lowerPrimitive(pt));
            }
            if (pt == P::Long || pt == P::ULong) {
                // #1227 / §9.1: use strtoll (not atoll) so we can validate the
                // input.  Reject empty, non-numeric leading chars, trailing
                // garbage, and leading whitespace → FormatException.
                auto* strtollFnL =
                    declareC("strtoll", i64(), {ptrTy(), ptrTy(), i32()});
                auto* endSlotL = entryAlloca(ptrTy());
                llvm::Value* v64L = builder_.CreateCall(
                    strtollFnL, {data, endSlotL, builder_.getInt32(10)}, "parse.vL");
                auto* i8TyL = llvm::Type::getInt8Ty(llvm_);
                llvm::Value* fcL = builder_.CreateLoad(i8TyL, data, "parse.fcL");
                llvm::Value* fcWsL = builder_.CreateICmpULE(
                    fcL, llvm::ConstantInt::get(i8TyL, ' '), "parse.fwsL");
                llvm::Value* endPtrL = builder_.CreateLoad(ptrTy(), endSlotL, "parse.epL");
                llvm::Value* advL = builder_.CreateICmpNE(endPtrL, data, "parse.advL");
                llvm::Value* endChL = builder_.CreateLoad(i8TyL, endPtrL, "parse.echL");
                llvm::Value* atNulL = builder_.CreateICmpEQ(
                    endChL, llvm::ConstantInt::get(i8TyL, 0), "parse.nulL");
                llvm::Value* validL = builder_.CreateAnd(
                    builder_.CreateNot(fcWsL),
                    builder_.CreateAnd(advL, atNulL), "parse.vfmtL");
                emitGuardThrow(builder_.CreateNot(validL, "parse.fmtbadL"),
                               "FormatException");
                return v64L;
            }
            if (pt == P::Double || pt == P::Float) {
                // #1227 / §9.2: use strtod (not atof) so we can validate the
                // input.  Reject empty, non-numeric leading chars, trailing
                // garbage, and leading whitespace → FormatException (per the
                // strict-fault model in §9.3 which applies to all §9 Parse).
                auto* strtodFnP = declareC("strtod",
                    llvm::Type::getDoubleTy(llvm_), {ptrTy(), ptrTy()});
                auto* endSlotF = entryAlloca(ptrTy());
                llvm::Value* dVal = builder_.CreateCall(
                    strtodFnP, {data, endSlotF}, "parse.dv");
                auto* i8TyF = llvm::Type::getInt8Ty(llvm_);
                llvm::Value* fcF = builder_.CreateLoad(i8TyF, data, "parse.fcF");
                llvm::Value* fcWsF = builder_.CreateICmpULE(
                    fcF, llvm::ConstantInt::get(i8TyF, ' '), "parse.fwsF");
                llvm::Value* endPtrF = builder_.CreateLoad(ptrTy(), endSlotF, "parse.epF");
                llvm::Value* advF = builder_.CreateICmpNE(endPtrF, data, "parse.advF");
                llvm::Value* endChF = builder_.CreateLoad(i8TyF, endPtrF, "parse.echF");
                llvm::Value* atNulF = builder_.CreateICmpEQ(
                    endChF, llvm::ConstantInt::get(i8TyF, 0), "parse.nulF");
                llvm::Value* validF = builder_.CreateAnd(
                    builder_.CreateNot(fcWsF),
                    builder_.CreateAnd(advF, atNulF), "parse.vfmtF");
                emitGuardThrow(builder_.CreateNot(validF, "parse.fmtbadF"),
                               "FormatException");
                if (pt == P::Float)
                    dVal = builder_.CreateFPTrunc(dVal, llvm::Type::getFloatTy(llvm_));
                return dVal;
            }
        }
        // If already the right type, just return it.
        return arg;
    }
    return nullptr;
}


// §28.3 / §28.11 / §22.4 (#1466). See the header for the full contract: mirrors
// MethodOwnership::markOutOwnedArgs / builtinOutOwnedArg
// (src/ownership/Ownership_Checks.cpp) so codegen agrees with the ownership pass
// on which argument position is a genuine `out owned` transfer, rather than
// trusting the plain "out" call-site modifier alone (a plain `out` argument
// targets a plain borrowing `out` parameter far more often than an `out owned`
// one).
bool Emitter::isOutOwnedArgPosition(const ast::CallExpr& call, std::size_t pos) {
    // A USER method whose formal at `pos` is declared `out owned`.
    if (auto* calleeSym = ctx_.exprSymbol(*call.callee)) {
        if (auto* ms = dynamic_cast<sema::MethodSymbol*>(calleeSym)) {
            if (auto* md = dynamic_cast<const ast::MethodDecl*>(ms->decl)) {
                if (pos < md->params.size())
                    return md->params[pos].modifier == "out owned";
            }
        }
    }
    // A builtin owned-OUT collection Try* method — no AST declaration, so
    // recognize it by receiver type name + member name, exactly like
    // MethodOwnership::builtinOutOwnedArg.
    auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(call.callee.get());
    if (!ma || !ma->target) return false;
    const sema::TypeRef* recvTy = ctx_.exprType(*ma->target);
    if (!recvTy) return false;
    std::string_view cn = builtinTypeName(recvTy);
    std::string_view m = ma->member;
    if (cn == "OwningStack" && m == "TryPop") return pos == 0;
    if (cn == "OwningQueue" && m == "TryDequeue") return pos == 0;
    // stdlib §4.5 / §4.7 (v0.171 #1336): OwningArray<T> and OwningLinkedList<T>
    // both declare TryPopFirst/TryPopLast(out owned item: T?) at arg 0 — the
    // #1466 repro is OwningArray specifically.
    if ((cn == "OwningLinkedList" || cn == "OwningArray") &&
        (m == "TryPopFirst" || m == "TryPopLast")) return pos == 0;
    // OwningSet<T>.TryTake(query, out owned removed: T?) — out-owned at arg 1.
    if (cn == "OwningSet" && m == "TryTake") return pos == 1;
    // OwningDictionary<K,V>.TryPopValue(key, out owned value: V) — arg 1.
    if (cn == "OwningDictionary" && m == "TryPopValue") return pos == 1;
    if (cn == "Weak" && m == "TryUpgrade") return pos == 0;
    // Channel<T>.TryReceive(out owned item: T?) — arg 0, only when T is a
    // class/interface (value-typed/string T drops the ownership modifiers,
    // stdlib §6.9).
    if (cn == "Channel" && m == "TryReceive" && pos == 0) {
        if (!recvTy->genericArgs.empty() && recvTy->genericArgs[0]) {
            const sema::TypeRef* elemT = recvTy->genericArgs[0];
            return elemT->kind == sema::TypeRef::Kind::Named &&
                   elemT->declaration &&
                   (elemT->declaration->kind == ast::TypeDeclKind::Class ||
                    elemT->declaration->kind == ast::TypeDeclKind::Interface);
        }
        return false;
    }
    return false;
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
                    // §9.4 / issue #654: `Boolean` is not handled by
                    // primitiveFromName (only `bool` is). Map it explicitly so
                    // Boolean.TryParse's out-slot gets the correct i1 type.
                    if (callPrimTy == sema::PrimitiveType::None &&
                        id->name == "Boolean")
                        callPrimTy = sema::PrimitiveType::Bool;
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
                // §28.13 vs §28.11 / #889: the scope-end disposal kind depends on
                // what the out value IS. Weak<T>.TryUpgrade(out using var h) binds
                // a Shared<T> control-block HANDLE → bz_shared_release. Every other
                // owned-OUT `out using var` (an owning-collection / channel Try*:
                // TryPop/TryDequeue/TryPopFirst/TryPopLast/TryReceive) binds a moved-
                // out OWNED CLASS ELEMENT → dispatch its destructor + reclaim the
                // block via the object's own vtable (dynamicDispose). Using the
                // sharedHandle path for a class element calls bz_shared_release on a
                // raw object pointer, corrupting the heap and leaking the element.
                // Discriminate by the RECEIVER builtin type name.
                bool isUpgrade = false;
                if (auto* maC = dynamic_cast<const ast::MemberAccessExpr*>(
                        call.callee.get())) {
                    if (maC->target) {
                        if (const sema::TypeRef* rt =
                                ctx_.exprType(*maC->target)) {
                            isUpgrade = (builtinTypeName(rt) == "Weak");
                        }
                    }
                }
                if (!alreadyMapped) {
                    llvm::AllocaInst* outSlot = entryAlloca(ptrTy());
                    // Zero-initialize: null → the Try* false path (no upgrade / no
                    // element). emitDisposals' null-guard then skips the release.
                    builder_.CreateStore(
                        llvm::ConstantPointerNull::get(ptrTy()), outSlot);
                    locals_[id2] = outSlot;
                    if (loc2 && loc2->decl) locals_[loc2->decl] = outSlot;
                    if (!disposeStack_.empty()) {
                        Disposal disp{outSlot, nullptr};
                        if (isUpgrade)
                            disp.sharedHandle = true;  // §28.13: Shared<T> handle
                        else
                            disp.dynamicDispose = true;  // §28.11: owned class elem
                        disposeStack_.back().push_back(disp);
                    }
                }
            }
            pos++;
            continue;
        }
        // §28.11 / #889: `s.TryPop(out owned item)` (and the other owned-OUT
        // Try* — TryDequeue/TryPopFirst/TryPopLast/TryPopValue/TryReceive) writes
        // the moved-out OWNED element into `item`'s pre-declared slot; the local
        // then SOLELY owns the element (the ownership pass marks it Owned per
        // §28.11, and there is no scope-end auto-dispose for an out-owned
        // recipient). Record the recipient LocalDecl so a later explicit
        // `item.Dispose()` / `item?.Dispose()` RECLAIMS the heap block (and nulls
        // the slot) exactly once via explicitDisposeOwnedReceiverSlot, rather than
        // only dispatching the destructor (which leaks the block). The slot itself
        // is the pre-declared local's — already created by its LocalDecl; we only
        // record ownership here.
        // §1466 fix: since v0.160 (§A.4) the parser never re-emits the literal
        // "out owned" modifier for a call-site argument — bare `out owned x` was
        // replaced by the inline `out owned var x` form — so the literal-string
        // arm alone is unreachable for a pre-declared recipient. The form that
        // actually reaches a PRE-DECLARED recipient at an `out owned` parameter is
        // the bare `out item` (no `var`), carrying the plain "out" modifier —
        // indistinguishable at the call site from an ordinary borrowing `out`
        // argument. Recognize it via isOutOwnedArgPosition (the codegen mirror of
        // MethodOwnership::builtinOutOwnedArg) so an ordinary `out` argument is
        // not misregistered as an owned recipient. The literal "out owned" is
        // kept for defensiveness.
        if ((a.modifier == "out owned" ||
             (a.modifier == "out" && isOutOwnedArgPosition(call, pos))) &&
            a.value) {
            if (auto* idoo = dynamic_cast<const ast::IdentifierExpr*>(
                    a.value.get())) {
                sema::Symbol* symoo = ctx_.exprSymbol(*idoo);
                if (auto* locoo = dynamic_cast<sema::LocalSymbol*>(symoo)) {
                    if (locoo->decl) {
                        outOwnedRecipients_.insert(
                            static_cast<const void*>(locoo->decl));
                        // §28.3 (#900 / #1466): the recipient SOLELY owns the
                        // moved-out element. Register a null-guarded scope-end
                        // disposal on its slot so the element is reclaimed exactly
                        // once — the same two-way choice the inline `out owned var`
                        // form makes (below):
                        //   • autoDropAtScopeEnd (the ownership pass proved the
                        //     recipient is never discharged on any path) →
                        //     dynamicDispose, firing on BOTH the normal-exit and
                        //     unwind edges. This is the §28.3 affine AUTO-DROP for
                        //     the recipient — a no-op when the slot is null (a
                        //     failed TryPop left it null on the false path), and it
                        //     RECLAIMS the heap block (not just the destructor) when
                        //     non-null, closing the #1466 leak. Never over-rejects:
                        //     a program that only pops-then-drops is accepted and
                        //     leak-free.
                        //   • otherwise (the recipient is discharged on some path —
                        //     `item?.Dispose()`, `move`, transfer) → unwindOnly, the
                        //     exception-edge-only backstop; the normal path is
                        //     covered by that explicit discharge (which nulls the
                        //     slot, so no double-free).
                        // Both are null-guarded (an explicit `item?.Dispose()` /
                        // transfer before a throw nulls the slot →
                        // explicitDisposeOwnedReceiverSlot). Gated on
                        // unwindIneligible_ so a recipient later moved/transferred
                        // out (whose plain-var slot a move may not null) registers
                        // nothing — its owner disposes it — mirroring
                        // registerUnwindOwnedLocal; and on the analyzed-body guard.
                        auto slotIt = locals_.find(locoo->decl);
                        if (slotIt != locals_.end() && !disposeStack_.empty() &&
                            currentFn_ == unwindIneligibleFn_ &&
                            !emittingDisposerBody_ &&
                            !unwindIneligible_.count(
                                static_cast<const void*>(locoo->decl))) {
                            Disposal d{slotIt->second, nullptr};
                            if (locoo->autoDropAtScopeEnd)
                                d.dynamicDispose = true;
                            else
                                d.unwindOnly = true;
                            disposeStack_.back().push_back(d);
                        }
                    }
                }
            }
            pos++;
            continue;
        }
        // §22.4 / v0.160 (#1305/#1344): `out owned var x` is the INLINE form —
        // x is declared fresh here (no pre-existing LocalDecl). Create a ptr
        // alloca, null-initialize it, record it in outOwnedRecipients_ so that
        // a later explicit `x?.Dispose()` reclaims the slot, and register a
        // null-guarded unwindOnly disposal for the exception path (§28.3 #900).
        // per §22.4, §28.11, §28.3
        if (a.modifier == "out owned var" && a.value) {
            if (auto* idoo = dynamic_cast<const ast::IdentifierExpr*>(
                    a.value.get())) {
                sema::Symbol* symoo = ctx_.exprSymbol(*idoo);
                auto* locoo = dynamic_cast<sema::LocalSymbol*>(symoo);
                bool alreadyMapped = (locoo && locoo->decl &&
                                      locals_.count(locoo->decl)) ||
                                     locals_.count(idoo);
                if (!alreadyMapped) {
                    llvm::AllocaInst* outSlot = entryAlloca(ptrTy());
                    builder_.CreateStore(
                        llvm::ConstantPointerNull::get(ptrTy()), outSlot);
                    locals_[idoo] = outSlot;
                    if (locoo && locoo->decl) locals_[locoo->decl] = outSlot;
                    // Record as out-owned recipient so explicit dispose reclaims.
                    const void* declKey =
                        locoo && locoo->decl
                            ? static_cast<const void*>(locoo->decl)
                            : static_cast<const void*>(idoo);
                    outOwnedRecipients_.insert(declKey);
                    // Normal-exit or exception-path disposal.
                    // §28.3 / #1387 (Item 2b): if the ownership pass set
                    // autoDropAtScopeEnd on the symbol (the inline local is never
                    // discharged on any path), register a dynamicDispose entry that
                    // fires on BOTH the normal and unwind paths — exactly-once-safe
                    // because the slot is null-guarded and nulled on disposal.
                    // Otherwise register the prior unwindOnly entry (exception edge
                    // only; an explicit Dispose() / _ := item / move will handle
                    // the normal path). // per §28.3, §22.4, §28.11
                    if (!disposeStack_.empty() &&
                        currentFn_ == unwindIneligibleFn_ &&
                        !emittingDisposerBody_ &&
                        !unwindIneligible_.count(declKey)) {
                        Disposal d{outSlot, nullptr};
                        if (locoo && locoo->autoDropAtScopeEnd)
                            d.dynamicDispose = true;
                        else
                            d.unwindOnly = true;
                        disposeStack_.back().push_back(d);
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
        if (ms && ms->parent &&
            ms->parent->kind == ast::TypeDeclKind::Interface &&
            isStaticMethod(ms) && loc && loc->declType) {
            // §11.8 (v0.176, #807): `T.TryParse(s, out var v)` — a static
            // interface member through a constrained type parameter. Sema
            // typed the LOCAL from the call site (interface `T?` substituted
            // to the call-site parameter, nullable stripped); the interface's
            // own declared `T?` node would lower to the wrong slot shape here
            // (a `{i8,T}` wrapper where the callee writes the bare value).
            // Resolve the local's type through the active substitution.
            slotTy = lowerTypeRef(resolveTypeParamRef(loc->declType));
        } else if (ms && loc && loc->declType &&
                   isNullableValueTy(loc->declType)) {
            // §14.2 / §7.10 (#1469): a GENERIC method's `out T?` value parameter.
            // Sema (typeCall, ExprTypecheck_Calls.cpp) resolved the out-var local
            // to the concrete nullable value companion (`long?`/`int?`/…) via the
            // call's type argument. The formal parameter type `T?` would lower
            // HERE with the i32 placeholder (genericSubst_ is not active during
            // slot creation), sizing the slot as {i8,i32} — correct only when the
            // instantiation is `int`; for `long?`/`bool?`/`double?`/an enum the
            // specialized callee writes a wider/narrower {i8,T} struct into the
            // undersized slot, dropping the value (garbage on read-back). Size the
            // slot from the sema-resolved concrete local type instead.
            slotTy = lowerTypeRef(loc->declType);
        } else if (ms) {
            // §22.4 / #1303: prefer the sema-resolved concrete local type
            // (loc->declType). For a GENERIC method's `out r: T`, sema
            // (typeCall, ExprTypecheck_Calls.cpp) resolved the local to the
            // substituted concrete type (int/string/long/…) via the call's
            // type argument; the formal parameter type `T` lowers HERE with
            // the i32 placeholder (genericSubst_ is not active during slot
            // creation) — correct only for the int instantiation, and WRONG
            // for a reference type (string → ptr), a wider value type
            // (long → i64), or a record/enum, sizing the slot undersized so
            // the specialized callee's write corrupts it (a malformed-IR GEP
            // on the read-back). For a non-generic method loc->declType is
            // the parameter's own concrete type, so this is a no-op there.
            // Fall back to the formal parameter only when sema left the
            // local untyped (loc == null / declType unset).
            if (loc && loc->declType) {
                slotTy = lowerTypeRef(loc->declType);
            } else {
                auto* md = dynamic_cast<const ast::MethodDecl*>(ms->decl);
                if (md && pos < md->params.size() && md->params[pos].type) {
                    sema::TypeRef* pt =
                        ctx_.resolvedType(*md->params[pos].type);
                    if (pt) slotTy = lowerTypeRef(pt);
                }
            }
        } else if (callPrimTy != sema::PrimitiveType::None) {
            using P = sema::PrimitiveType;
            // For TryParse, the out result is the same primitive type.
            if (callPrimTy == P::Double) slotTy = llvm::Type::getDoubleTy(llvm_);
            else if (callPrimTy == P::Float) slotTy = llvm::Type::getFloatTy(llvm_);
            else if (callPrimTy == P::Long || callPrimTy == P::ULong) slotTy = i64();
            // §9.4 / issue #654: bool.TryParse out-slot is i1 (Bool lowers to i1).
            else if (callPrimTy == P::Bool) slotTy = i1();
            else slotTy = i32();
        } else if (loc && loc->declType) {
            // v0.171 (#1336): a BUILTIN collection Try's inline `out var x` has
            // no MethodDecl; sema typed the local `T?`/`V?` (the borrowing
            // TryPop/TryDequeue/TryPeek/TryGetValue family). Lower that nullable
            // type so the slot is the tagged `{i8,T}` struct for a value
            // instantiation (a class instantiation stays a nullable pointer);
            // the false-path null-store and value writeback then land correctly.
            slotTy = lowerTypeRef(loc->declType);
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


void Emitter::ensureCtorOutVarSlots(const ast::NewExpr& ne,
                                    const sema::TypeRef* rt) {
    if (!rt || rt->kind != sema::TypeRef::Kind::Named || !rt->declaration)
        return;
    // Resolve the matching constructor decl (single, or arity match) for the
    // out-parameter types — mirrors checkNewExprArgs / the sema out-var path.
    const ast::ConstructorDecl* ctorDecl = nullptr;
    std::vector<const ast::ConstructorDecl*> cds;
    for (sema::Symbol* m : rt->declaration->members)
        if (auto* cs = dynamic_cast<sema::ConstructorSymbol*>(m))
            if (!cs->isStatic)
                if (auto* cd =
                        dynamic_cast<const ast::ConstructorDecl*>(cs->decl))
                    cds.push_back(cd);
    if (cds.size() == 1)
        ctorDecl = cds[0];
    else
        for (auto* cd : cds)
            if (cd->params.size() == ne.args.size()) { ctorDecl = cd; break; }

    std::size_t pos = 0;
    for (const auto& a : ne.args) {
        if (a.modifier != "out var" || !a.value) { pos++; continue; }
        auto* id = dynamic_cast<const ast::IdentifierExpr*>(a.value.get());
        if (!id) { pos++; continue; }
        sema::Symbol* sym = ctx_.exprSymbol(*id);
        auto* loc = dynamic_cast<sema::LocalSymbol*>(sym);
        if ((loc && loc->decl && locals_.count(loc->decl)) ||
            locals_.count(id)) { pos++; continue; }
        llvm::Type* slotTy = i32();
        if (ctorDecl && pos < ctorDecl->params.size() &&
            ctorDecl->params[pos].type) {
            sema::TypeRef* pt = ctx_.resolvedType(*ctorDecl->params[pos].type);
            if (pt) slotTy = lowerTypeRef(pt);
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
    if (!recvTy) return nullptr;
    // #1091 (§8.1/§14.2): GetHashCode/Equals on a bare unconstrained generic
    // type parameter `T` (Named, no declaration). The value-semantics gate below
    // only matches a CONCRETE primitive/record/enum, so an unconstrained-T
    // receiver fell through and the call returned a 0 stub (ToString on the same
    // T already works via the genericSubst path in the ms==null fallback).
    // Resolve T → its concrete bound type through the active monomorphization and
    // route to the matching handler: primitive/record/enum →
    // lowerValueSemMethodOn; class/interface/string/Object → lowerObject*. (Only
    // the broken members — GetHashCode/Equals — are intercepted here; ToString
    // keeps its existing working path.)
    if ((isGetHashCode || isEquals) &&
        recvTy->kind == sema::TypeRef::Kind::Named && !recvTy->declaration &&
        !genericSubstSemaTy_.empty() && recvTy->astNode &&
        recvTy->astNode->name && recvTy->astNode->name->parts.size() == 1) {
        auto sit = genericSubstSemaTy_.find(recvTy->headName());
        if (sit != genericSubstSemaTy_.end() && sit->second) {
            const sema::TypeRef* concrete = sit->second;
            using K = sema::TypeRef::Kind;
            using P = sema::PrimitiveType;
            bool concreteValSem =
                (concrete->kind == K::Primitive &&
                 concrete->primitive != P::String &&
                 concrete->primitive != P::Object) ||
                (concrete->kind == K::Named && concrete->declaration &&
                 (concrete->declaration->kind == ast::TypeDeclKind::Record ||
                  concrete->declaration->kind == ast::TypeDeclKind::Enum));
            bool concreteRefSem =
                (concrete->kind == K::Named && concrete->declaration &&
                 (concrete->declaration->kind == ast::TypeDeclKind::Class ||
                  concrete->declaration->kind == ast::TypeDeclKind::Interface)) ||
                (concrete->kind == K::Primitive &&
                 (concrete->primitive == P::String ||
                  concrete->primitive == P::Object));
            if (concreteValSem) {
                if (llvm::Value* recv = lowerExpr(*ma->target))
                    return lowerValueSemMethodOn(concrete, recv, call, member);
            } else if (concreteRefSem) {
                if (llvm::Value* recv = lowerExpr(*ma->target)) {
                    if (isGetHashCode) {
                        if (llvm::Value* r = lowerObjectGetHashCode(recv))
                            return r;
                    } else if (call.args[0].value) {
                        if (llvm::Value* other = lowerExpr(*call.args[0].value))
                            if (llvm::Value* r = lowerObjectEquals(recv, other))
                                return r;
                    }
                }
            }
        }
    }
    // Lower the receiver ONLY for the value-semantics receiver kinds the core
    // handles (a primitive Equals/GetHashCode, or any record/enum member); a
    // primitive ToString (left to the dedicated string path) or a non-value
    // receiver returns nullptr WITHOUT lowering, so the outer dispatch lowers the
    // receiver exactly once even when it has side effects (#1075). The dispatch
    // core itself is factored into lowerValueSemMethodOn so the null-conditional
    // path can reuse it with the unwrapped nullable inner value.
    bool primValueSem = recvTy->kind == sema::TypeRef::Kind::Primitive &&
        recvTy->primitive != sema::PrimitiveType::String &&
        recvTy->primitive != sema::PrimitiveType::Object &&
        (isEquals || isGetHashCode);
    bool namedValueSem = recvTy->kind == sema::TypeRef::Kind::Named &&
        recvTy->declaration &&
        (recvTy->declaration->kind == ast::TypeDeclKind::Record ||
         recvTy->declaration->kind == ast::TypeDeclKind::Enum);
    if (!primValueSem && !namedValueSem) return nullptr;

    llvm::Value* recv = lowerExpr(*ma->target);
    if (!recv) return nullptr;
    return lowerValueSemMethodOn(recvTy, recv, call, member);
}

llvm::Value* Emitter::lowerValueSemMethodOn(const sema::TypeRef* recvTy,
                                            llvm::Value* recv,
                                            const ast::CallExpr& call,
                                            std::string_view member) {
    bool isToString    = member == "ToString"    && call.args.empty();
    bool isGetHashCode = member == "GetHashCode"  && call.args.empty();
    bool isEquals      = member == "Equals"       && call.args.size() == 1;
    if (!recvTy || !recv) return nullptr;
    // fix #747 R43 / stdlib §9.1–§9.5: a PRIMITIVE value type (int/long/bool/
    // char/double/…) implements IEquatable<T> with an auto-generated
    // `Equals(other): bool` and a `GetHashCode(): int`. Codegen had no handler,
    // so `a.Equals(b)` fell through and returned the receiver's raw value — the
    // result was always-false when branched and produced a `br i32` (non-i1)
    // IR-verification failure when stored to a `var`. Emit the value comparison /
    // hash directly. ToString on a primitive is handled by the dedicated
    // primitive-ToString path, so it is left to fall through.
    if (recvTy->kind == sema::TypeRef::Kind::Primitive &&
        recvTy->primitive != sema::PrimitiveType::String &&
        recvTy->primitive != sema::PrimitiveType::Object &&
        (isEquals || isGetHashCode)) {
        if (isGetHashCode) {
            // §8.3-style: the value itself as an i32 hash (bit-pattern for FP).
            if (recv->getType()->isDoubleTy())
                recv = builder_.CreateBitCast(recv, i64());
            else if (recv->getType()->isFloatingPointTy())
                recv = builder_.CreateBitCast(recv, i32());
            if (recv->getType()->isIntegerTy())
                return builder_.CreateZExtOrTrunc(recv, i32(), "prim.hash");
            return nullptr;
        }
        // isEquals: value equality, returning i1 (Bool).
        if (!call.args[0].value) return nullptr;
        llvm::Value* other = lowerExpr(*call.args[0].value);
        if (!other) return nullptr;
        if (recv->getType()->isFloatingPointTy()) {
            if (other->getType() != recv->getType())
                other = recv->getType()->isDoubleTy()
                    ? builder_.CreateFPExt(other, recv->getType())
                    : builder_.CreateFPTrunc(other, recv->getType());
            return builder_.CreateFCmpOEQ(recv, other, "prim.eq");
        }
        if (recv->getType()->isIntegerTy()) {
            if (other->getType() != recv->getType())
                other = builder_.CreateSExtOrTrunc(other, recv->getType());
            return builder_.CreateICmpEQ(recv, other, "prim.eq");
        }
        return nullptr;
    }
    if (recvTy->kind != sema::TypeRef::Kind::Named ||
        !recvTy->declaration)
        return nullptr;
    const sema::TypeSymbol* ts = recvTy->declaration;
    bool isRecord = ts->kind == ast::TypeDeclKind::Record;
    bool isEnum   = ts->kind == ast::TypeDeclKind::Enum;
    if (!isRecord && !isEnum) return nullptr;

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
        positional || sema::isAutoEqualityBearingBodyRecord(ts, ctx_);

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
// §24.5 (#747 R42): "if a handler throws, subsequent handlers are not invoked
// and the exception propagates." Bezel propagates exceptions through the global
// exception slot (emitExcSlot), not LLVM unwind edges, so a plain closure call
// does not stop the loop on its own — the slot must be tested after each
// handler. After invoking a handler, if the slot is non-null the loop breaks to
// `endBB` (no further handlers) and the still-pending exception propagates to
// Raise's caller via the normal post-call pending check.
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

    auto* invokeBB = llvm::BasicBlock::Create(llvm_, "raise.invoke", fn);
    auto* contBB = llvm::BasicBlock::Create(llvm_, "raise.cont", fn);
    builder_.SetInsertPoint(bodyBB);
    llvm::Value* iv = builder_.CreateLoad(i64(), iSlot);
    llvm::Value* closure = builder_.CreateCall(atFn, {srcPtr, iv}, "event.h");
    // §24.5 (#908): a handler deactivated MID-RAISE — e.g. another handler in this
    // same Raise disposed this subscription — leaves a null slot;
    // bz_event_handler_at returns null for it. Loading the function pointer from
    // null would SIGSEGV, so skip a null handler and continue the iteration.
    llvm::Value* closNull = builder_.CreateICmpEQ(
        closure, llvm::ConstantPointerNull::get(ptrTy()), "h.null");
    builder_.CreateCondBr(closNull, contBB, invokeBB);

    builder_.SetInsertPoint(invokeBB);
    llvm::Value* fnPtr = builder_.CreateLoad(ptrTy(), closure, "h.fn");
    llvm::Value* envSlot =
        builder_.CreateGEP(ptrTy(), closure, {builder_.getInt32(1)});
    llvm::Value* envPtr = builder_.CreateLoad(ptrTy(), envSlot, "h.env");
    std::vector<llvm::Value*> callArgs;
    callArgs.push_back(envPtr);
    if (payloadLL) callArgs.push_back(payloadLL);
    builder_.CreateCall(handlerFnTy, fnPtr, callArgs);
    // §24.5: stop on a throwing handler — break the loop if the handler set the
    // global exception slot; the exception stays pending and propagates.
    llvm::Value* pend =
        builder_.CreateLoad(ptrTy(), emitExcSlot(), "raise.exc");
    llvm::Value* threw = builder_.CreateICmpNE(
        pend, llvm::ConstantPointerNull::get(ptrTy()), "raise.threw");
    builder_.CreateCondBr(threw, endBB, contBB);

    builder_.SetInsertPoint(contBB);
    builder_.CreateStore(builder_.CreateAdd(iv, builder_.getInt64(1)), iSlot);
    builder_.CreateBr(loopBB);

    builder_.SetInsertPoint(endBB);
}

// per §24.5 / §30.13 / #1377: async-event RaiseAsync via continuation chaining.
//
// Prior approach (blocked domain thread via bz_task_await_sync): if any handler
// genuinely suspends (e.g. awaits Task.Delay), the domain drain thread parks in
// bz_task_await_sync, but the handler's resume step is enqueued to that same
// domain and can never execute → deadlock (§30.13: run-to-completion).
//
// New approach: delegate the entire chain to bz_event_raise_async_chain (runtime
// helper, bezel_runtime.cpp).  The helper invokes handler[0] non-blockingly,
// registers bz_raise_chain_step via bz_task_then, and returns the outer Task
// immediately.  Each step: check fault (first-error-wins §24.5), free completed
// task, invoke next handler, repeat.  No domain thread is ever blocked.
//
// Payload ABI: the payload type is arbitrary (ptr for class/string, struct for
// record, i64/i1 for primitives).  A per-call-site SHIM function is generated
// with signature (ptr closure, ptr payload_buf) → ptr (Task*) that loads the
// payload from payload_buf with the exact LLVM type and invokes the handler's
// fn(env, payload?).  The runtime memcpys the payload into a heap buffer in the
// chain context (valid across continuation steps), then calls the shim for each
// subsequent handler.  This avoids ABI tricks and works for struct, ptr, and
// integer payloads alike.
//
// Payload lifetime: the caller's async frame (the method that awaits RaiseAsync)
// lives until the outer task completes, so the payload object/value that was in
// the frame remains valid for the full chain.  The heap-copied payload_buf is an
// extra safety copy for the case where the payload is a stack-sized value type
// whose LLVM alloca would be gone after the current continuation step returns.
llvm::Value* Emitter::emitAsyncEventRaise(llvm::Value* srcPtr,
                                          llvm::Value* payloadLL,
                                          llvm::Type* payloadTy) {
    if (!srcPtr) return nullptr;

    // 1. Generate the handler-invoke SHIM (per-call-site, InternalLinkage).
    //    Shim ABI: (ptr closure, ptr payload_buf) → ptr  (Task*)
    //    Knows the exact LLVM payload type; isolates the runtime from LLVM types.
    auto* shimFnTy = llvm::FunctionType::get(
        ptrTy(), {ptrTy(), ptrTy()}, false);

    static std::atomic<unsigned> shimSeq{0};
    std::string shimName =
        "__bz_raiseasync_shim_" + std::to_string(shimSeq.fetch_add(1));
    auto* shimFn = llvm::Function::Create(
        shimFnTy, llvm::Function::InternalLinkage, shimName, module_.get());
    shimFn->addFnAttr(llvm::Attribute::AlwaysInline);

    {
        auto* savedFn = currentFn_;
        auto* savedBB = builder_.GetInsertBlock();
        auto  savedIP = savedBB ? builder_.GetInsertPoint()
                                : decltype(savedBB->begin()){};
        currentFn_ = shimFn;
        auto* shimBB = llvm::BasicBlock::Create(llvm_, "entry", shimFn);
        builder_.SetInsertPoint(shimBB);

        auto argIt = shimFn->arg_begin();
        llvm::Value* closureArg    = &*argIt++;
        llvm::Value* payloadBufArg = &*argIt;

        // fn  = load ptr, ptr %closure
        // env = load ptr, ptr (closure + sizeof(ptr))
        llvm::Value* fn  = builder_.CreateLoad(ptrTy(), closureArg, "h.fn");
        llvm::Value* envGEP = builder_.CreateGEP(
            ptrTy(), closureArg, {builder_.getInt32(1)}, "h.env.gep");
        llvm::Value* env = builder_.CreateLoad(ptrTy(), envGEP, "h.env");

        std::vector<llvm::Value*> callArgs = {env};
        std::vector<llvm::Type*>  handlerArgTys = {ptrTy()};
        if (payloadLL && payloadTy) {
            // Load the payload with its exact type from the caller's buffer.
            llvm::Value* pl =
                builder_.CreateLoad(payloadTy, payloadBufArg, "h.pl");
            callArgs.push_back(pl);
            handlerArgTys.push_back(payloadTy);
        }
        auto* handlerFnTy =
            llvm::FunctionType::get(ptrTy(), handlerArgTys, false);
        llvm::Value* task =
            builder_.CreateCall(handlerFnTy, fn, callArgs, "h.task");
        builder_.CreateRet(task);

        currentFn_ = savedFn;
        if (savedBB) builder_.SetInsertPoint(savedBB, savedIP);
    }

    // 2. Store the payload to a stack buffer (valid during this call).
    //    bz_event_raise_async_chain memcpys the bytes into the heap ctx before
    //    returning, so subsequent chain steps reference the heap copy.
    llvm::Value* payloadBuf  = llvm::ConstantPointerNull::get(ptrTy());
    int64_t      payloadSize = 0;
    if (payloadLL && payloadTy) {
        auto* slot = entryAlloca(payloadTy);
        builder_.CreateStore(payloadLL, slot);
        payloadBuf  = slot;
        payloadSize = (int64_t)module_->getDataLayout().getTypeAllocSize(payloadTy);
    }

    // 3. Call the runtime chain helper.
    //    bz_event_raise_async_chain(src, payload_buf, payload_size, shim) → Task*
    //    per §24.5 / §30.13 / #1377 (bezel_runtime.cpp).
    auto* chainFn = declareC("bz_event_raise_async_chain",
                              ptrTy(),
                              {ptrTy(), ptrTy(), i64(), ptrTy()});
    return builder_.CreateCall(
        chainFn,
        {srcPtr, payloadBuf, builder_.getInt64(payloadSize), shimFn},
        "raiseasync.task");
}

bool Emitter::isIEventTypeName(const sema::TypeRef* rt) {
    if (!rt || rt->kind != sema::TypeRef::Kind::Named || rt->declaration)
        return false;
    if (!rt->astNode || !rt->astNode->name || rt->astNode->name->parts.empty())
        return false;
    const auto& n = rt->headName();
    return n == "IEvent" || n == "IAsyncEvent";
}

// §6.5 / §24.3 (#1610): the producer-side event-source classes. Like
// isIEventTypeName, a NULL-declaration Named TypeRef whose astNode first name
// part is "EventSource" / "AsyncEventSource". A value of this static type held
// in a local/parameter/field is the same bz_event_source_t* a direct event
// access yields, so member calls dispatch against it identically.
bool Emitter::isEventSourceTypeName(const sema::TypeRef* rt) {
    if (!rt || rt->kind != sema::TypeRef::Kind::Named || rt->declaration)
        return false;
    if (!rt->astNode || !rt->astNode->name || rt->astNode->name->parts.empty())
        return false;
    const auto& n = rt->headName();
    return n == "EventSource" || n == "AsyncEventSource";
}

llvm::Value* Emitter::lowerEventHandlerClosure(const ast::Expr* handlerExpr,
                                               const sema::TypeRef* payload) {
    if (!handlerExpr) return nullptr;
    const sema::TypeRef* saved = expectedFunctionType_;
    sema::TypeRef handlerTy;  // synthesized (payload) -> ()
    handlerTy.kind = sema::TypeRef::Kind::Function;
    if (payload) {
        sema::TypeRef::FuncParam p;
        p.type = const_cast<sema::TypeRef*>(payload);
        handlerTy.funcParams.push_back(p);
    }
    // §24.3: a handler returns unit. Set funcReturn so lowerLambda resolves a
    // block-bodied handler's return type to void — without this it defaulted to
    // i32, defining the handler as i32(...) while the Raise loop calls it through
    // a void(...) function pointer (an indirect-call type mismatch).
    auto* unitRet = ctx_.makeTypeRef();
    unitRet->kind = sema::TypeRef::Kind::Unit;
    handlerTy.funcReturn = unitRet;
    expectedFunctionType_ = &handlerTy;
    llvm::Value* closure = lowerExpr(*handlerExpr);
    expectedFunctionType_ = saved;
    return closure;
}

std::optional<llvm::Value*> Emitter::tryLowerEventCall(const ast::CallExpr& call) {
    auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(call.callee.get());
    if (!ma || !ma->target) return std::nullopt;

    // §24.3 / §24.5: dispatch a recognized event method (Subscribe/Add/Remove/
    // Raise/RaiseAsync) against a bz_event_source_t* (`src`) whose payload type
    // is `payload`. Shared by the direct-event-access path (src from
    // eventSourcePtr, payload from eventPayloadType) and the
    // EventSource<T>/AsyncEventSource<T>-typed local/param/field path (#1610:
    // src from lowerExpr — the value is the same bz_event_source_t* via the
    // event-as-value read, payload from genericArgs[0]). Returns nullopt when
    // `ma->member` is not one of the event methods (so non-event members fall
    // through to ordinary dispatch).
    auto dispatchEventMethods = [&](llvm::Value* src,
                                    const sema::TypeRef* payload)
        -> std::optional<llvm::Value*> {
        const auto& method = ma->member;
        const bool isSubscribe  = method == "Subscribe"  && call.args.size() == 1;
        const bool isAdd        = method == "Add"        && call.args.size() == 1;
        const bool isRemove     = method == "Remove"     && call.args.size() == 1;
        const bool isRaise      = method == "Raise";
        const bool isRaiseAsync = method == "RaiseAsync";
        if (!isSubscribe && !isAdd && !isRemove && !isRaise && !isRaiseAsync)
            return std::nullopt;  // not an event method — ordinary dispatch
        if (isSubscribe || isAdd || isRemove) {
            // per §24.3 / stdlib §6.5: the handler argument is a function value
            // (lambda or method reference) lowered to a closure { fn, env } with
            // the synthesized `(payload) -> ()` expected type (shared helper).
            llvm::Value* closure =
                lowerEventHandlerClosure(call.args[0].value.get(), payload);
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

        // isRaise / isRaiseAsync — per §24.5: invoke each subscribed handler
        // with the payload.
        // #737: gate payload lowering on the event's payload TYPE, not on the
        // call args being non-empty. For a no-payload event the explicit
        // `Raise(())` form passes the unit literal `()` as an arg — non-empty,
        // but lowerExpr has no case for a UnitExpr and returns null, so the old
        // `!call.args.empty` check then bailed BEFORE emitEventRaise and
        // silently dropped the entire dispatch (the Subscribe side already
        // builds a `void(env)` handler for unit events). §24.5: `Raise()` and
        // `Raise(())` are equivalent and must dispatch identically.
        const bool hasPayload =
            payload && payload->kind != sema::TypeRef::Kind::Unit;
        llvm::Value* payloadLL = nullptr;
        llvm::Type* payloadTy = nullptr;
        if (hasPayload && !call.args.empty() && call.args[0].value) {
            payloadLL = lowerExpr(*call.args[0].value);
            if (!payloadLL) return std::optional<llvm::Value*>(nullptr);
            payloadTy = payloadLL->getType();
        }
        if (isRaiseAsync) {
            // §24.5 / §30: RaiseAsync returns a Task* (awaited by the caller).
            // emitAsyncEventRaise calls each handler (which returns Task*),
            // awaits each synchronously via bz_task_await_sync in order, and
            // returns an already-done Task carrying either success or the first
            // fault.
            llvm::Value* task = emitAsyncEventRaise(src, payloadLL, payloadTy);
            return std::optional<llvm::Value*>(task);
        }
        emitEventRaise(src, payloadLL, payloadTy);
        return std::optional<llvm::Value*>(nullptr);  // Raise returns unit
    };

    // The target of `.Subscribe`/`.Raise`/`.Add`/`.Remove` may itself be a
    // direct event access (EventSymbol on the target expr).
    const sema::EventSymbol* es = eventSymbolOf(*ma->target);
    if (es) {
        // Gate BEFORE lowering the source: a non-event member on a direct-event
        // access (semantically bogus, e.g. `Clicked.GetInvocationList()`) must
        // fall through to ordinary dispatch WITHOUT first lowering the event
        // source — eventSourcePtr evaluates the target (lowerExpr) and emits
        // GEPs/loads (and an itable lookup for interface events), which would
        // otherwise be dead IR plus a double evaluation once ordinary dispatch
        // re-lowers the target. Cheap name compares; dispatchEventMethods
        // re-checks (idempotently) after the source is loaded.
        const auto& method = ma->member;
        const bool isEventMethod =
            (method == "Subscribe" && call.args.size() == 1) ||
            (method == "Add" && call.args.size() == 1) ||
            (method == "Remove" && call.args.size() == 1) ||
            method == "Raise" || method == "RaiseAsync";
        if (!isEventMethod) return std::nullopt;
        llvm::Value* src = eventSourcePtr(*ma->target);
        if (!src) return std::nullopt;
        return dispatchEventMethods(src, eventPayloadType(es));
    }
    // es == null: the target is not a direct event access. Its static type
    // determines the dispatch.
    const sema::TypeRef* recvTy = ctx_.exprType(*ma->target);
    // §24.3 (#747): `.Subscribe` on a STORED IEvent<T>/IAsyncEvent<T> value
    // (`var ev := s.Happened; ev.Subscribe(h)`). IEvent<T> has no itable, but
    // its runtime value IS the bz_event_source_t* (via the event-as-value
    // read), so dispatch directly to bz_event_subscribe. The payload is the
    // receiver type's genericArgs[0] (the upcast type, variance-safe per §12.9).
    // Add/Remove/Raise are NOT on the IEvent<T> interface (§24.3) and stay
    // rejected (no IEvent member-dispatch block admits them).
    if (ma->member == "Subscribe" && call.args.size() == 1 &&
        call.args[0].value && isIEventTypeName(recvTy)) {
        llvm::Value* src = lowerExpr(*ma->target);
        if (!src) return std::optional<llvm::Value*>(nullptr);
        const sema::TypeRef* payload =
            (!recvTy->genericArgs.empty()) ? recvTy->genericArgs[0] : nullptr;
        llvm::Value* closure =
            lowerEventHandlerClosure(call.args[0].value.get(), payload);
        if (!closure) return std::optional<llvm::Value*>(nullptr);
        auto* subFn =
            declareC("bz_event_subscribe", ptrTy(), {ptrTy(), ptrTy()});
        return std::optional<llvm::Value*>(
            builder_.CreateCall(subFn, {src, closure}, "subscription"));
    }
    // §6.5 / §24.3 (#1610): a value held in a local/param/field of static type
    // EventSource<T>/AsyncEventSource<T>. Its runtime value is the same
    // bz_event_source_t* a direct event access yields (via the event-as-value
    // read), so Raise/Add/Remove/Subscribe/RaiseAsync dispatch identically.
    // (Sema's EventSource/AsyncEventSource rcn block already rejected any
    // non-method name before codegen, so only valid methods reach here.)
    if (isEventSourceTypeName(recvTy)) {
        llvm::Value* src = lowerExpr(*ma->target);
        if (!src) return std::optional<llvm::Value*>(nullptr);
        const sema::TypeRef* payload =
            (!recvTy->genericArgs.empty()) ? recvTy->genericArgs[0] : nullptr;
        return dispatchEventMethods(src, payload);
    }
    return std::nullopt;
}


// per §2.11 / §24.4: the built-in `Subscription` resolves (TypeResolution_Names)
// to a Named TypeRef with a null user declaration whose first name part is
// "Subscription". Same shape as the collection/StringBuilder built-in checks.
bool Emitter::isSubscriptionType(const sema::TypeRef* rt) {
    if (!rt || rt->kind != sema::TypeRef::Kind::Named || rt->declaration)
        return false;
    if (!rt->astNode || !rt->astNode->name || rt->astNode->name->parts.empty())
        return false;
    return rt->headName() == "Subscription";
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
        // A direct event access OR a stored IEvent<T>/IAsyncEvent<T> value
        // (#747): both produce an owned Subscription that must be disposed.
        return eventSymbolOf(*ma->target) != nullptr ||
               isIEventTypeName(ctx_.exprType(*ma->target));
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
        std::string_view nm = declRet->headName();
        bool isParam = false;
        for (const auto& gp : genericParams)
            if (gp.name == nm) { isParam = true; break; }
        if (isParam && !subst.count(nm)) {
            // §14.1 / #850: peel the nullable when the declared parameter type
            // is `T?` (nullable) and the concrete argument type is also nullable
            // — bind T to the non-nullable concrete type so the monomorphized
            // body lowers `T` as i32 (not {i8,i32}) and `T?` as the proper
            // nullable struct. Mirrors the sema fix in unifyArgSubst.
            const sema::TypeRef* concreteTy = resultTy;
            if (declRet->nullable && resultTy->nullable) {
                if (resultTy->kind == sema::TypeRef::Kind::Primitive) {
                    concreteTy = ctx_.primitiveTypeRef(resultTy->primitive,
                                                       /*nullable=*/false);
                } else {
                    // Named value type (record/enum): allocate a ctx-owned
                    // non-nullable copy for a stable semaTy pointer.
                    sema::TypeRef* nn = ctx_.makeTypeRef();
                    *nn = *resultTy;
                    nn->nullable = false;
                    concreteTy = nn;
                }
            }
            // §14.1 / #885 #886: do NOT bind a type param when the concrete
            // candidate is itself an unresolved type param (Named, no
            // declaration) OR the Object primitive (which is how sema types
            // null literals that weren't resolved to a concrete nullable value
            // type). Either form carries no real concrete-type information for
            // the generic param and blocks the correct binding from a subsequent
            // concrete argument or explicit type arg. Skip here; the outer
            // lowerCall loop already skips NullLiteralExpr args, but this guard
            // is a second line of defence for other null-typed sources.
            // per §14.1/§14.2, fixes #885/#886.
            //
            // §14.1 / #1162: EXCEPTION — a builtin COLLECTION/runtime type
            // (Array<int>, Dictionary<…>, Task, …) is also a decl-less Named,
            // but it is fully concrete: lowerTypeRef gives its real
            // collection/value type (ptr for heap collections). Inferring
            // U → Array<int> from `Echo(a)` must bind it (lowering the call's
            // return/param fnTy to ptr, identical to the explicit
            // `Echo<Array<int>>(a)` path); skipping it left U unbound, so the
            // call lowered U → the i32 fallback while the monomorphized body
            // returned ptr → 'Call parameter type does not match'. Recognise a
            // builtin name (astNode single name part, or importedName) and bind
            // it; only a genuine unresolved type-parameter name is skipped.
            if (concreteTy->kind == sema::TypeRef::Kind::Named &&
                !concreteTy->declaration) {
                std::string_view cn;
                if (concreteTy->astNode && concreteTy->astNode->name &&
                    concreteTy->astNode->name->parts.size() == 1)
                    cn = concreteTy->headName();
                else if (!concreteTy->astNode && !concreteTy->importedName.empty())
                    cn = concreteTy->importedName;
                if (cn.empty() || !isBuiltinTypeRefName(cn))
                    return;
            }
            if (concreteTy->kind == sema::TypeRef::Kind::Primitive &&
                concreteTy->primitive == sema::PrimitiveType::Object)
                return;
            subst[nm] = lowerTypeRef(concreteTy);
            semaTy[nm] = concreteTy;
            added = true;
        }
        // §12 / §14.2 / #1246: when `nm` is a BUILTIN GENERIC TYPE name (e.g.
        // "OwningArray", "Array", "LinkedList") and NOT a type-parameter name,
        // the user-type branch below (both declaration=non-null) never fires for
        // builtin collections (declaration=null). Without this block the function
        // returns here and the generic args are discarded unseen, leaving T
        // unbound. That causes the method body to execute as the i32-erased
        // compiled template rather than the per-instantiation specialisation, so
        // any virtual/interface call on a collection element (`x.V()` in
        // `for var x in arr`) dispatches to the constraint-base stub (returning
        // 0) instead of the most-derived override — a soundness bug (§12/§14).
        //
        // Fix: if the declared type is a builtin (no declaration) with generic
        // args and the actual type has the SAME builtin name, recurse pairwise
        // so that `OwningArray<T>` vs `OwningArray<B>` produces T=B. The binding
        // then triggers the force-specialisation check at the call site (see
        // ~line 12343) which inlines the method body under the concrete subst.
        if (!isParam && !declRet->genericArgs.empty() &&
            resultTy->kind == sema::TypeRef::Kind::Named &&
            !resultTy->declaration &&
            declRet->genericArgs.size() == resultTy->genericArgs.size()) {
            std::string_view rn;
            if (resultTy->astNode && resultTy->astNode->name &&
                !resultTy->astNode->name->parts.empty())
                rn = resultTy->headName();
            else if (!resultTy->importedName.empty())
                rn = resultTy->importedName;
            if (!rn.empty() && rn == nm) {
                for (std::size_t i = 0; i < declRet->genericArgs.size(); ++i)
                    unifyMethodReturnTypeArg(declRet->genericArgs[i],
                                             resultTy->genericArgs[i],
                                             genericParams, subst, semaTy, added);
            }
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
    // §28.3 / §28.11 (#1466): `item!.Dispose()` — the null-forgiving `!` narrows
    // nullability but does not change WHICH object the receiver denotes, so
    // `item!` reclaims through the exact same slot as bare `item` / `item?`.
    // Unwrap it here (a nullable owned local, and in particular a nullable
    // out-owned recipient, is disposed via `!.`/`?.`) rather than requiring every
    // caller to. Before this the PostfixBangExpr shape matched no case below and
    // fell to the final `return nullptr`, so `item!.Dispose()` only DISPATCHED the
    // destructor and never reclaimed the heap block.
    if (auto* bang = dynamic_cast<const ast::PostfixBangExpr*>(&receiver)) {
        if (bang->operand)
            return explicitDisposeOwnedReceiverSlot(*bang->operand, slotTy);
        return nullptr;
    }
    if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(&receiver)) {
        sema::Symbol* sym = ctx_.exprSymbol(*id);
        // #991 / §28.5 / §28.7: a bare-identifier reference to an `owned`
        // reference FIELD (`_v?.Dispose()` / `_v.Dispose()` inside a method,
        // the implicit-`this` form of `this._v?.Dispose()`).  The explicit
        // `this._v` MemberAccessExpr form is handled below; the bare-identifier
        // form resolves to a FieldSymbol but is neither a ParameterSymbol nor a
        // LocalSymbol, so it fell through to dispatch-only — the destructor ran
        // but the field's 16-byte block LEAKED (the §28.7 programmer-disposes-
        // first pattern `_v?.Dispose(); _v := b;` then lost the old pointer).
        // Reclaim + null the field here (mirroring the MemberAccessExpr branch):
        // the §28.4-step-2 owned-field auto-dispose null-guards its reload, so a
        // following auto-dispose finds null and skips — free happens exactly
        // once.  A non-owned (borrow) field returns null (its real owner frees).
        if (auto* ffs = dynamic_cast<sema::FieldSymbol*>(sym)) {
            if (isOwnedReferenceField(ffs))
                return lvalueAddr(receiver, slotTy);
            return nullptr;
        }
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
            if (!ld) {
                // §22.4 / v0.160 (#1305): `out owned var x` declares x inline;
                // sema sets lsym->decl = the IdentifierExpr (not a LocalDecl).
                // If the IdentifierExpr is in outOwnedRecipients_ we pre-created
                // the slot in ensureOutVarSlots and must reclaim it here.
                if (auto* ie = dynamic_cast<const ast::IdentifierExpr*>(loc->decl)) {
                    if (outOwnedRecipients_.count(static_cast<const void*>(ie)))
                        return lvalueAddr(receiver, slotTy);
                }
                // #963 / §28.11 / §29.2: a for-loop variable of an
                // `IOwningEnumerable<T>` loop (`for var x in Gen()`) is a
                // LocalSymbol whose decl is the ForStmt (sema sets `lsym->decl =
                // fs`), not a LocalDecl. Each yielded element is OWNED by the
                // loop variable — the iterator transfers ownership on yield and
                // does not re-dispose an already-yielded element — so this
                // scope's slot (locals_[&fs]) is the sole reclaim path. Without
                // this, an explicit `x.Dispose()` ran the destructor but never
                // reclaimed the element block (a normal-path leak). Reclaiming +
                // nulling here frees it exactly once. A BORROW loop (over an
                // Array<T> / IEnumerable<T> the collection still owns) is NOT an
                // IOwningEnumerable, so its loop variable is correctly excluded.
                if (auto* fs = dynamic_cast<const ast::ForStmt*>(loc->decl)) {
                    const sema::TypeRef* itTy =
                        fs->iterable ? ctx_.exprType(*fs->iterable) : nullptr;
                    std::string_view inm;
                    if (itTy && itTy->kind == sema::TypeRef::Kind::Named) {
                        if (itTy->astNode && itTy->astNode->name &&
                            !itTy->astNode->name->parts.empty())
                            inm = itTy->headName();
                        else if (!itTy->importedName.empty())
                            inm = itTy->importedName;
                    }
                    if (inm == "IOwningEnumerable")
                        return lvalueAddr(receiver, slotTy);
                }
                return nullptr;
            }
            // A `using var` local is owned by this scope (§28.10). A plain `var`
            // owned local (init `new C(...)` or an `owned T` return) is owned by
            // structure — exactly the §28.3 unwind-disposal eligibility. Either
            // way this scope's slot is the sole other reclaim path; nulling it
            // after reclaiming makes free happen exactly once. A borrow-
            // initialized local (no `owned`) returns null — its real owner
            // reclaims elsewhere, so reclaiming here would double-free.
            if (loc->isUsing) return lvalueAddr(receiver, slotTy);
            if (unwindOwnedLocalClass(ld)) return lvalueAddr(receiver, slotTy);
            // §28.11 / #889: a local that RECEIVED a moved-out owned element via
            // `out owned` (e.g. `s.TryPop(out owned item)`) is the sole owner of
            // that element — recorded in outOwnedRecipients_ at the call site. Its
            // slot is the only reclaim path (no scope-end auto-dispose covers an
            // out-owned recipient), so reclaim + null here makes free happen once.
            if (outOwnedRecipients_.count(static_cast<const void*>(ld)))
                return lvalueAddr(receiver, slotTy);
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
    // per §4.6/§4.7: LinkedList<T>/OwningLinkedList<T> node-based API.
    if (auto* v = tryLowerLinkedListInstanceCall(call)) return v;
    // §4.15 (#549): ArraySpan<T>/ReadOnlyArraySpan<T> instance methods and
    // Array<T>.AsSpan()/AsReadOnlySpan() construction.
    if (auto* v = tryLowerArraySpanCall(call)) return v;
    // §15.1.2 / #1342: inline-array `[N]T` built-in intrinsics ToArray /
    // Contains / (§8.5 v0.182) default ToString.
    if (auto* v = tryLowerInlineArrayCall(call)) return v;
    // stdlib §4.4 / spec §16.3 (v0.182, #1579): `Array<T>.Slice(r: Range<int>):
    // owned Array<T>` — shares codegen with `arr[r]` (the "compiler-
    // recognized equivalent") via lowerArrayRangeSlice.
    if (call.args.size() == 1 && call.args[0].value && call.callee) {
        if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(call.callee.get())) {
            if (ma->member == "Slice" && ma->target) {
                if (const sema::TypeRef* rt = ctx_.exprType(*ma->target)) {
                    if (builtinTypeName(rt) == "Array") {
                        if (llvm::Value* recv = lowerExpr(*ma->target)) {
                            if (auto* sliced = lowerArrayRangeSlice(
                                    recv, rt, *call.args[0].value))
                                return sliced;
                        }
                    }
                }
            }
        }
    }
    // §8.5 (v0.182, #1579): the unit type's default `ToString` is the
    // constant `"()"` — unit has exactly one value, so nothing about the
    // receiver needs to be read. Handled here (rather than through the
    // general class/interface/Object vtable-slot-3 ToString dispatch)
    // because unit has no vtable to dispatch through.
    if (call.args.empty() && call.callee) {
        if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(call.callee.get())) {
            if (ma->member == "ToString" && ma->target) {
                if (const sema::TypeRef* rt = ctx_.exprType(*ma->target)) {
                    if (rt->kind == sema::TypeRef::Kind::Unit)
                        return lowerStringLiteral("\"()\"", /*verbatim=*/false);
                }
            }
        }
    }
    // §3: stdlib collection instance methods: arr.Add, arr.Get, dict.TryGetValue, etc.
    if (auto* v = tryLowerCollectionInstanceCall(call)) return v;
    // stdlib §3.2: Encoding.Utf8.GetBytes(s) / Encoding.Utf8.GetString(bytes).
    if (auto* v = tryLowerEncodingCall(call)) return v;
    // §30.14/§30.15 (#600 phase c): Channel<T>/ChannelSender<T> methods + Domain.Spawn.
    if (auto* v = tryLowerChannelAndDomainCall(call)) return v;
    // §6.2 / issue #647: Mutex.Acquire / TryAcquire / AcquireAsync / TryAcquireAsync.
    if (auto* v = tryLowerMutexLockCall(call)) return v;
    // §6.3 / issue #647: Semaphore.Acquire / TryAcquire / AcquireAsync / TryAcquireAsync.
    if (auto* v = tryLowerSemaphoreTokenCall(call)) return v;
    // §6.6 / issue #974: TaskCompletionSource / OwningTaskCompletionSource methods.
    if (auto* v = tryLowerTcsCall(call)) return v;
    // §6.4 / issue #647: Atomic<T> / AtomicInteger<T> instance methods.
    if (auto* v = tryLowerAtomicCall(call)) return v;
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
            // §32.7 / §5.2 / stdlib §10 (#1512): the explicit conversion
            // `IntPtr(longValue)` / `UIntPtr(ulongValue)`. IntPtr/UIntPtr are
            // pointer-sized integers (i64 host), so the conversion is an integer
            // width-adjustment of the argument to i64 (identity when the source
            // is already `long`/`ulong`). Sign of any widening follows the
            // source's signedness (sema restricts the argument to an integer).
            if ((cid->name == "IntPtr" || cid->name == "UIntPtr") &&
                call.args.size() == 1 && call.args[0].value) {
                llvm::Value* v = lowerExpr(*call.args[0].value);
                if (!v) return nullptr;
                if (v->getType()->isIntegerTy()) {
                    unsigned bw = v->getType()->getIntegerBitWidth();
                    if (bw < 64) {
                        bool srcUns = false;
                        if (auto* at = ctx_.exprType(*call.args[0].value))
                            if (at->kind == sema::TypeRef::Kind::Primitive)
                                srcUns = isUnsignedPrim(at->primitive);
                        v = srcUns ? builder_.CreateZExt(v, i64(), "intptr.zext")
                                   : builder_.CreateSExt(v, i64(), "intptr.sext");
                    } else if (bw > 64) {
                        v = builder_.CreateTrunc(v, i64(), "intptr.trunc");
                    }
                }
                return v;
            }
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
        // fix #985: a `RawBytes` extern parameter (§32.5) receives the raw byte
        // buffer (void*), not the { ptr, len } struct that is RawBytes' runtime
        // value. When a RawBytes argument is passed here, extract rb->ptr via
        // bz_rawbytes_ptr so the C side sees the buffer pointer (as it did under
        // the prior bare-buffer rep).
        auto rawbytesParamAt = [&](std::size_t i) -> bool {
            if (!ed || i >= ed->params.size()) return false;
            const ast::Type* pt = ed->params[i].type.get();
            return pt && pt->kind == ast::TypeKind::Named && pt->name &&
                   pt->name->parts.size() == 1 &&
                   pt->name->parts.front() == "RawBytes";
        };
        std::vector<llvm::Value*> args;
        std::size_t idx = 0;
        for (const auto& a : call.args) {
            if (!a.value) return nullptr;
            llvm::Value* v = lowerExpr(*a.value);
            if (!v) return nullptr;
            // fix #985: marshal a RawBytes argument to its underlying buffer
            // pointer (rb->ptr) for a RawBytes-typed extern parameter (§32.5).
            if (rawbytesParamAt(idx)) {
                const sema::TypeRef* at = ctx_.exprType(*a.value);
                bool isRaw = at && at->kind == sema::TypeRef::Kind::Named &&
                    at->declaration && at->declaration->name == "RawBytes";
                if (isRaw) {
                    auto* fn = declareC("bz_rawbytes_ptr", ptrTy(), {ptrTy()});
                    v = builder_.CreateCall(fn, {v}, "raw.arg.ptr");
                }
            }
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
        for (std::size_t pi = 0; pi < call.args.size(); ++pi) {
            const auto& a = call.args[pi];
            if (!a.value) return nullptr;
            // fix #747 R43 / §27.1: a `ref`/`out` argument to a function value
            // passes the caller's storage ADDRESS, not its value — the lambda's
            // matching parameter is by-pointer (see lowerLambda). Use the
            // call-site `ref`/`out` modifier (the formal funcParams modifier may
            // be absent on an inferred function type). Without this the closure
            // received the value by copy and the callee's mutation was lost.
            bool byRef = isByRefMod(a.modifier) ||
                         (pi < rt->funcParams.size() &&
                          isByRefMod(rt->funcParams[pi].modifier));
            if (byRef) {
                llvm::Type* elemTy = nullptr;
                llvm::Value* addr = lvalueAddr(*a.value, elemTy);
                if (!addr) return nullptr;
                args.push_back(addr);
                continue;
            }
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
                std::string_view paramName = fr->headName();
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
            // fix #747 R43 / §27.1: a `ref`/`out` parameter of the function type
            // is passed by pointer (its argument is an address), matching the
            // lambda's by-pointer parameter.
            if (isByRefMod(rt->funcParams[pi].modifier) ||
                (pi < call.args.size() && isByRefMod(call.args[pi].modifier))) {
                allArgTys.push_back(ptrTy());
                continue;
            }
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
                    auto sit = calleeSubst->find(fp->headName());
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
            std::string_view retParamName = rt->funcReturn->headName();
            // Check if any param has the same type parameter name.
            bool retMatchesParam = false;
            for (const auto& fp : rt->funcParams) {
                if (!fp.type) continue;
                if (fp.type->kind == sema::TypeRef::Kind::Named &&
                    !fp.type->declaration && fp.type->astNode &&
                    fp.type->astNode->name &&
                    fp.type->astNode->name->parts.size() == 1 &&
                    fp.type->headName() == retParamName) {
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
        // #719: a lambda body can `throw`; like a normal method call, propagate a
        // pending exception after the call instead of silently continuing with the
        // (default) result. Without this, `var fn := () => throw E(); var v := fn();`
        // swallowed the throw and ran on. emitPendingCheck branches to the unwind
        // path when the exception slot is set; non-throwing calls just fall through.
        llvm::Value* r = builder_.CreateCall(fnTy, fnPtr, args);
        emitPendingCheck();
        return retTy->isVoidTy() ? nullptr : r;
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
        // The field's declared type is on the body FieldDecl, or — for a
        // POSITIONAL record field (§9.1) — on posParam (fs->decl is then the
        // owning TypeDecl, so the FieldDecl cast fails). #747 R43: without the
        // positional branch, `box.fn()` on a record whose `fn` field is a
        // positional function-typed parameter skipped the closure-call path and
        // fell through to a no-method/garbage result; the same call worked only
        // after extracting `b.fn` to a local first.
        sema::TypeRef* rt = nullptr;
        if (auto* fd = dynamic_cast<const ast::FieldDecl*>(fs->decl)) {
            if (fd->type) rt = ctx_.resolvedType(*fd->type);
        } else if (fs->posParam && fs->posParam->type) {
            rt = ctx_.resolvedType(*fs->posParam->type);
        }
        if (rt && rt->kind == sema::TypeRef::Kind::Function) {
            llvm::Value* closurePtr = lowerExpr(*call.callee);
            return emitClosureCall(closurePtr, rt);
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
            // fix #678 (narrowed) / §27.2: mirror the void-builtin check in
            // lowerLambda — when the body is a Console.Write/WriteLine call
            // (null exprType, no Unit TypeRef from sema), lowerLambda declares
            // the lambda void(…); the call-site fnTy must match.
            else if (isLambdaExprBodyVoidBuiltin(*iifeLam))
                retTy = llvm::Type::getVoidTy(llvm_);
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
            // #1106 (§12.3 defect 5): `base.ToString()` must dispatch
            // NON-virtually to the BASE class's ToString — never the virtual
            // vtable slot below, which points at the most-derived override and
            // would recurse infinitely (base.ToString() inside the override →
            // the same override → stack-overflow SIGSEGV). Resolve the enclosing
            // class's base chain: call the first ancestor ToString override
            // directly on the receiver; when no ancestor overrides ToString
            // (base is Object) emit the §8.5 type-name default — Object.ToString
            // returns the runtime type's name, i.e. the enclosing class's name.
            if (auto* btid = dynamic_cast<const ast::IdentifierExpr*>(
                    cg_ma->target.get())) {
                // The enclosing (derived) class is the owner of the method being
                // emitted — recover it from currentFn_ via methodFns_ (the
                // §8.5 type-name default is the runtime type's name, i.e. this
                // class). When the class has no explicit base, `base` types as
                // Object (no declaration), so the receiver's exprType is not a
                // reliable source; the method-owner reverse-map is.
                const sema::TypeSymbol* cg_cls = nullptr;
                if (btid->name == "base") {
                    for (auto& kv : methodFns_)
                        if (kv.second == currentFn_ && kv.first) {
                            cg_cls = kv.first->parent;
                            break;
                        }
                    if (!cg_cls) {
                        const sema::TypeRef* btRecvTy =
                            ctx_.exprType(*cg_ma->target);
                        if (btRecvTy &&
                            btRecvTy->kind == sema::TypeRef::Kind::Named)
                            cg_cls = btRecvTy->declaration;
                    }
                }
                if (btid->name == "base" && cg_cls) {
                    const sema::TypeSymbol* startBase = nullptr;
                    for (const sema::TypeSymbol* b : cg_cls->baseTypes)
                        if (b && b->kind == ast::TypeDeclKind::Class) {
                            startBase = b; break;
                        }
                    sema::MethodSymbol* baseToStr = nullptr;
                    for (const sema::TypeSymbol* cur = startBase;
                         cur && !baseToStr;) {
                        for (sema::Symbol* sym : cur->members) {
                            auto* m = dynamic_cast<sema::MethodSymbol*>(sym);
                            if (m && !isStaticMethod(m) &&
                                m->name == "ToString" && m->params.empty()) {
                                baseToStr = m; break;
                            }
                        }
                        const sema::TypeSymbol* nxt = nullptr;
                        for (const sema::TypeSymbol* b : cur->baseTypes)
                            if (b && b->kind == ast::TypeDeclKind::Class) {
                                nxt = b; break;
                            }
                        cur = nxt;
                    }
                    if (baseToStr) {
                        auto fit = methodFns_.find(baseToStr);
                        llvm::Value* brecv = lowerExpr(*cg_ma->target);
                        if (fit != methodFns_.end() && brecv)
                            return builder_.CreateCall(
                                fit->second->getFunctionType(),
                                fit->second, {brecv});
                    }
                    // No base override → §8.5 type-name default (the enclosing
                    // class's name; Object.ToString returns the runtime type).
                    return lowerStringLiteral(
                        std::string("\"") + std::string(cg_cls->name) + "\"",
                        false);
                }
            }
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
    // fix #747 R43 / §27.2 / §14.1: a user method call `x.M(...)` where `x` is
    // an unannotated lambda parameter whose type sema left unresolved (so
    // `calleeSym` is null) — but codegen injected a concrete class/record type
    // via `lambdaParamTypeHint_` (the same hint that already lets `x.field` and
    // `x.ToString()` resolve; see fieldAddr at LlvmBackend_Layout.cpp and the
    // ToString paths above). Resolve the MethodSymbol by name from the hinted
    // type's members so the ordinary instance-dispatch path below (ms != null)
    // handles the call. Without this, the unresolved call returned null and the
    // enclosing expression-body lambda emitted `ret void` into a non-void
    // function → IR verification failure.
    if (!calleeSym) {
        if (auto* hma = dynamic_cast<const ast::MemberAccessExpr*>(
                call.callee.get())) {
            if (auto* hid = dynamic_cast<const ast::IdentifierExpr*>(
                    hma->target.get())) {
                sema::Symbol* hSym = ctx_.exprSymbol(*hid);
                auto hit = lambdaParamTypeHint_.find(hSym);
                if (hit != lambdaParamTypeHint_.end() && hit->second &&
                    hit->second->kind == sema::TypeRef::Kind::Named &&
                    hit->second->declaration) {
                    for (sema::Symbol* m : hit->second->declaration->members) {
                        if (m && m->name == hma->member &&
                            dynamic_cast<sema::MethodSymbol*>(m)) {
                            calleeSym = m;
                            break;
                        }
                    }
                }
            }
        }
    }
    // #903 / §14.2 / §9.x: `a.CompareTo(b)` / `a.Equals(b)` where `a` is a bare
    // type-parameter constrained `where T: IComparable<T>` / `IEquatable<T>` (the
    // BUILTIN interfaces, which have a null TypeDecl, so sema's constraint-member
    // resolution skipped them and left `calleeSym` null). When monomorphized with
    // T = a concrete RECORD / CLASS that implements the contract member, resolve
    // the receiver's type-parameter through genericSubstSemaTy_ to its concrete
    // declaration and bind `calleeSym` to that type's own CompareTo / Equals, so
    // the ordinary instance dispatch below devirtualizes to it (a record VALUE
    // receiver is spilled by the same record-receiver shaping the user-interface
    // constraint case uses). Without this the call fell through unresolved and
    // emitted the default 0/false (the silent wrong value of #903). The primitive
    // case (T = int) and the user-interface constraint case already work; this
    // covers only the builtin-interface × user-type combination.
    if (!calleeSym && !genericSubstSemaTy_.empty()) {
        if (auto* cma = dynamic_cast<const ast::MemberAccessExpr*>(
                call.callee.get())) {
            bool isContractMember =
                (cma->member == "CompareTo" || cma->member == "Equals") &&
                call.args.size() == 1;
            if (isContractMember && cma->target) {
                const sema::TypeRef* recvTy = ctx_.exprType(*cma->target);
                if (recvTy && recvTy->kind == sema::TypeRef::Kind::Named &&
                    !recvTy->declaration && recvTy->astNode &&
                    recvTy->astNode->name &&
                    recvTy->astNode->name->parts.size() == 1) {
                    auto sit = genericSubstSemaTy_.find(
                        recvTy->headName());
                    if (sit != genericSubstSemaTy_.end() && sit->second &&
                        sit->second->declaration &&
                        (sit->second->declaration->kind ==
                             ast::TypeDeclKind::Record ||
                         sit->second->declaration->kind ==
                             ast::TypeDeclKind::Class)) {
                        if (sema::MethodSymbol* impl = findMethodByName(
                                sit->second->declaration, cma->member))
                            // The single-parameter CompareTo(T)/Equals(T) contract
                            // shape (not an unrelated overload).
                            if (impl->params.size() == 1)
                                calleeSym = impl;
                    }
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
                        recvTy->headName();
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
        // §9.6 (#946/#947/#948): `o.Equals(x)` / `o.GetHashCode()` where o is
        // Object / interface (the universal Object members, no MethodSymbol —
        // the receiver's static type has no override). Dispatch through the
        // uniform vtable fixed slots (kEqualsSlot / kGetHashCodeSlot); a string
        // operand (no vtable) uses the content intrinsic; a class/box dispatches
        // to the override (or the §8.3 reference-identity / identity-hash
        // default — a class WITH an override resolves a MethodSymbol and never
        // reaches here). Equals returns i1 so `not (o.Equals(x))` is valid IR.
        //
        // Gated to Object- and interface-typed receivers ONLY: both are always
        // heap pointers, so lowerObjectEquals/lowerObjectGetHashCode never bail
        // out (their non-pointer guard never fires) and the receiver is lowered
        // exactly once. A record/enum/primitive receiver is handled earlier by
        // tryLowerValueSemanticsCall; a concrete class receiver with an override
        // resolves a real MethodSymbol (ms != null) and dispatches there.
        if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(
                call.callee.get())) {
            bool isEqualsCall = ma->member == "Equals" && ma->target &&
                                call.args.size() == 1 && call.args[0].value;
            bool isHashCall = ma->member == "GetHashCode" && ma->target &&
                              call.args.empty();
            if (isEqualsCall || isHashCall) {
                // #1111 (§12.3): `base.Equals(o)` / `base.GetHashCode()` inside
                // an overriding class must dispatch NON-virtually to the BASE
                // class's implementation. Without this interception the code falls
                // through to lowerObjectEquals/lowerObjectGetHashCode below, which
                // reads vtable[kEqualsSlot/kGetHashCodeSlot] — the slot points at
                // the most-derived override (the same method we're already in) →
                // infinite recursion or wrong value (§12.3 requires base.M to call
                // the base class's impl, never the virtual slot). Mirrors the
                // base.ToString() fix above (line ~9506).
                if (auto* btid = dynamic_cast<const ast::IdentifierExpr*>(
                        ma->target.get())) {
                    if (btid->name == "base") {
                        // Recover the enclosing (derived) class from currentFn_
                        // via methodFns_ (reliable even when the class has no
                        // explicit base, where `base` types as Object).
                        const sema::TypeSymbol* beh_cls = nullptr;
                        for (auto& kv : methodFns_)
                            if (kv.second == currentFn_ && kv.first) {
                                beh_cls = kv.first->parent; break;
                            }
                        if (!beh_cls) {
                            const sema::TypeRef* bty =
                                ctx_.exprType(*ma->target);
                            if (bty &&
                                bty->kind == sema::TypeRef::Kind::Named)
                                beh_cls = bty->declaration;
                        }
                        if (beh_cls) {
                            // Walk the BASE chain (skip the enclosing class
                            // itself) for the first ancestor override.
                            const sema::TypeSymbol* beh_start = nullptr;
                            for (const sema::TypeSymbol* b : beh_cls->baseTypes)
                                if (b && b->kind == ast::TypeDeclKind::Class) {
                                    beh_start = b; break;
                                }
                            if (isEqualsCall) {
                                sema::MethodSymbol* beh_eq = nullptr;
                                for (const sema::TypeSymbol* cur = beh_start;
                                     cur && !beh_eq;) {
                                    for (sema::Symbol* sym : cur->members) {
                                        auto* m =
                                            dynamic_cast<sema::MethodSymbol*>(
                                                sym);
                                        if (!(m && !isStaticMethod(m) &&
                                              m->name == "Equals" &&
                                              m->params.size() == 1))
                                            continue;
                                        // #1115/#1151: only a pointer-param
                                        // Equals is the §8.4 Object.Equals
                                        // shape; skip a value-typed overload.
                                        auto fit2 = methodFns_.find(m);
                                        if (fit2 != methodFns_.end() &&
                                            fit2->second->getFunctionType()
                                                    ->getNumParams() >= 2 &&
                                            !fit2->second->getFunctionType()
                                                    ->getParamType(1)
                                                    ->isPointerTy())
                                            continue;
                                        beh_eq = m; break;
                                    }
                                    const sema::TypeSymbol* nxt = nullptr;
                                    for (const sema::TypeSymbol* b :
                                         cur->baseTypes)
                                        if (b &&
                                            b->kind == ast::TypeDeclKind::Class)
                                        { nxt = b; break; }
                                    cur = nxt;
                                }
                                llvm::Value* beh_recv =
                                    lowerExpr(*ma->target);
                                llvm::Value* beh_other =
                                    lowerExpr(*call.args[0].value);
                                if (beh_eq) {
                                    auto fit = methodFns_.find(beh_eq);
                                    if (fit != methodFns_.end() &&
                                        beh_recv && beh_other)
                                        return builder_.CreateCall(
                                            fit->second->getFunctionType(),
                                            fit->second,
                                            {beh_recv, beh_other});
                                }
                                // No ancestor override → §8.3 reference-identity.
                                if (beh_recv && beh_other)
                                    return builder_.CreateICmpEQ(
                                        beh_recv, beh_other, "base.eq.ref");
                            } else {
                                // isHashCall
                                sema::MethodSymbol* beh_hash = nullptr;
                                for (const sema::TypeSymbol* cur = beh_start;
                                     cur && !beh_hash;) {
                                    for (sema::Symbol* sym : cur->members) {
                                        auto* m =
                                            dynamic_cast<sema::MethodSymbol*>(
                                                sym);
                                        if (m && !isStaticMethod(m) &&
                                            m->name == "GetHashCode" &&
                                            m->params.empty()) {
                                            beh_hash = m; break;
                                        }
                                    }
                                    const sema::TypeSymbol* nxt = nullptr;
                                    for (const sema::TypeSymbol* b :
                                         cur->baseTypes)
                                        if (b &&
                                            b->kind == ast::TypeDeclKind::Class)
                                        { nxt = b; break; }
                                    cur = nxt;
                                }
                                llvm::Value* beh_recv =
                                    lowerExpr(*ma->target);
                                if (beh_hash) {
                                    auto fit = methodFns_.find(beh_hash);
                                    if (fit != methodFns_.end() && beh_recv)
                                        return builder_.CreateCall(
                                            fit->second->getFunctionType(),
                                            fit->second, {beh_recv});
                                }
                                // No ancestor override → §8.3 identity hash
                                // (pointer bits truncated to i32).
                                if (beh_recv)
                                    return builder_.CreateTrunc(
                                        builder_.CreatePtrToInt(
                                            beh_recv, i64(), "base.hash.ptr"),
                                        i32(), "base.hash.r");
                            }
                        }
                    }
                }
                const sema::TypeRef* recvTy = ctx_.exprType(*ma->target);
                using K = sema::TypeRef::Kind;
                using P = sema::PrimitiveType;
                bool isObjectRecv = recvTy && recvTy->kind == K::Primitive &&
                                    recvTy->primitive == P::Object;
                bool isIfaceRecv =
                    recvTy && recvTy->kind == K::Named && recvTy->declaration &&
                    recvTy->declaration->kind == ast::TypeDeclKind::Interface;
                // #1106: a concrete CLASS receiver with NO Equals/GetHashCode
                // override (the default §8.3 reference-identity / identity-hash)
                // has no MethodSymbol, so the call reaches this ms==null fallback
                // — route it through the same uniform vtable dispatch as an
                // Object/interface receiver (a class WITH an override resolves a
                // real MethodSymbol earlier and never reaches here). Without this
                // the default Equals/GetHashCode on a class mis-lowered: a bare
                // field load that returned false and crashed an `if` branch with
                // `br i32` (defects 1 + 3). Mirrors the ToString gate above.
                bool isNamedClassRecv =
                    recvTy && recvTy->kind == K::Named && recvTy->declaration &&
                    recvTy->declaration->kind == ast::TypeDeclKind::Class;
                if (isObjectRecv || isIfaceRecv || isNamedClassRecv) {
                    llvm::Value* recv = lowerExpr(*ma->target);
                    if (isEqualsCall) {
                        llvm::Value* other = lowerExpr(*call.args[0].value);
                        if (recv && other)
                            if (llvm::Value* r = lowerObjectEquals(recv, other))
                                return r;
                    } else {
                        if (recv)
                            if (llvm::Value* r = lowerObjectGetHashCode(recv))
                                return r;
                    }
                }
            }
        }
        // §30.3 (#941): `t.Wait()` — the synchronous escape hatch on a Task /
        // Task<T> / OwningTask<T> receiver (a builtin class with no MethodSymbol,
        // so this call would otherwise fall through and be silently dropped,
        // leaving statements after `await Task.Delay(N)` un-run). Wait() blocks
        // the calling thread until the task completes (its continuation, run on
        // a pool worker, sets the post-await state); bz_task_await_sync is the
        // same blocking primitive the async-Main wrapper uses (§30.13). The
        // result buffer pointer is discarded — Wait() returns unit. A faulted
        // task is NOT re-raised here (spec §30.3 attributes re-raise to reading
        // `Result`, not `Wait()`); the fault surfaces when the result is read.
        //
        // §30.3 (v0.162 / #1362): Wait() inside async code is a sema error (the
        // sema check fires before codegen). For non-async helpers called from a
        // continuation, the runtime bz_task_await_sync domain guard catches it.
        if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(
                call.callee.get())) {
            if (ma->member == "Wait" && ma->target && call.args.empty()) {
                const sema::TypeRef* recvTy = ctx_.exprType(*ma->target);
                if (isTaskTypeName(recvTy)) {
                    llvm::Value* task = lowerExpr(*ma->target);
                    if (task && task->getType()->isPointerTy()) {
                        // §30.3 (v0.162 / #1362): catchable guard AT the call
                        // site — blocking on an INCOMPLETE task inside a domain
                        // continuation raises InvalidOperationException (the
                        // runtime abort in bz_task_await_sync stays as a last-
                        // resort backstop for paths that bypass this lowering).
                        auto* isDoneFn = declareC("bz_task_is_done",
                                                  builder_.getInt1Ty(), {ptrTy()});
                        auto* curDomFn = declareC("bz_current_domain_ptr",
                                                  ptrTy(), {});
                        llvm::Value* done =
                            builder_.CreateCall(isDoneFn, {task}, "wait.done");
                        llvm::Value* dom =
                            builder_.CreateCall(curDomFn, {}, "wait.dom");
                        llvm::Value* inDomain = builder_.CreateICmpNE(
                            dom, llvm::Constant::getNullValue(ptrTy()),
                            "wait.indomain");
                        llvm::Value* wouldDeadlock = builder_.CreateAnd(
                            builder_.CreateNot(done), inDomain, "wait.deadlock");
                        emitGuardThrow(wouldDeadlock, "InvalidOperationException");
                        auto* awaitFn = declareC("bz_task_await_sync", ptrTy(),
                                                 {ptrTy()});
                        builder_.CreateCall(awaitFn, {task}, "task.wait");
                        return nullptr;  // unit result
                    }
                }
            }
        }
        // issue #1002 / §29.3 / §29.11 / stdlib §4.4/§4.12: explicit
        // `arr.GetEnumerator()` on a builtin Array<T>/OwningArray<T> receiver.
        // The stdlib types this `owned IEnumerator<T>`, and sema now types the
        // result as the builtin IEnumerator<T> sentinel (so #494 MoveNext/Current
        // typing fires). Lower it to a concrete iterator-vtable enumerator by
        // reusing the #943 array-wrapper machinery: wrap the array as the
        // IEnumerable vtable object ([0]=td,[1]=Dispose,[2]=MoveNext,[3]=Current,
        // [4]=GetEnumerator) then call slot 4 to hand back a FRESH, INDEPENDENT
        // cursor enumerator (positioned before the first element). The returned
        // enumerator's slot-1 Dispose frees only the small wrapper struct — it
        // BORROWS the array (`arr`), never freeing it — so a separate `using var
        // arr` is disposed exactly once, on its own, with no double-free (§28.2).
        // A `using var e := arr.GetEnumerator()` is disposed via the iterator
        // slot-1 path registered in registerUsingDisposal (§29.7/§29.11).
        if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(
                call.callee.get())) {
            if (ma->member == "GetEnumerator" && ma->target &&
                    call.args.empty()) {
                const sema::TypeRef* recvTy = ctx_.exprType(*ma->target);
                std::string_view rbn = builtinTypeName(recvTy);
                // #1102C / §29.11: GetEnumerator() on a vtable-backed
                // IEnumerable<T> (a yield-method result / interface-typed value)
                // — the source value the yield method returns IS the enumerator
                // (its vtable is [0]=td,[1]=Dispose,[2]=MoveNext,[3]=Current,
                // [4]=GetEnumerator; the consumer drives slots 2/3 directly).
                // Return the receiver unchanged — the SAME identity the §30.7
                // GetAsyncEnumerator path returns for the async iterator — so a
                // `using var it := C.Gen().GetEnumerator()` disposes exactly that
                // one struct (slot 1) with no separate leaked source temp and no
                // double-free. (The Array/OwningArray case below instead wraps +
                // clones because the array is a distinct long-lived object.)
                if (isVtableBackedIEnumerableType(recvTy)) {
                    return lowerExpr(*ma->target);
                }
                if ((rbn == "Array" || rbn == "OwningArray") && recvTy &&
                        !recvTy->genericArgs.empty() && recvTy->genericArgs[0]) {
                    llvm::Value* arr = lowerExpr(*ma->target);
                    if (arr && arr->getType()->isPointerTy()) {
                        // Wrap as the §29.11 iterator-vtable IEnumerable object.
                        llvm::Value* iter = wrapArrayAsIEnumerable(arr, recvTy);
                        if (iter && iter->getType()->isPointerTy()) {
                            // slot 4: fresh independent cursor enumerator (#943).
                            llvm::Value* vt =
                                builder_.CreateLoad(ptrTy(), iter);
                            llvm::Value* geSlot = builder_.CreateGEP(
                                ptrTy(), vt, {builder_.getInt32(4)});
                            llvm::Value* geFn =
                                builder_.CreateLoad(ptrTy(), geSlot);
                            auto* geTy = llvm::FunctionType::get(
                                ptrTy(), {ptrTy()}, false);
                            llvm::Value* en = builder_.CreateCall(
                                geTy, geFn, {iter}, "arr.getenum");
                            // The wrapper IEnumerable object `iter` itself is a
                            // throwaway: slot-4 produced an independent clone, so
                            // free the wrapper struct (its Dispose is a plain
                            // free() of the borrow-holding struct; the array is
                            // untouched). Keeps the heap clean with no leak.
                            llvm::Value* iterVt =
                                builder_.CreateLoad(ptrTy(), iter);
                            llvm::Value* dSlot = builder_.CreateGEP(
                                ptrTy(), iterVt, {builder_.getInt32(1)});
                            llvm::Value* dFn =
                                builder_.CreateLoad(ptrTy(), dSlot);
                            auto* dTy = llvm::FunctionType::get(
                                llvm::Type::getVoidTy(llvm_), {ptrTy()}, false);
                            builder_.CreateCall(dTy, dFn, {iter});
                            return en;
                        }
                    }
                }
            }
        }
        // issue #494 / §29.2 / §29.3: explicit `e.MoveNext()` call where `e` is
        // typed as the builtin IEnumerator<T> or IOwningEnumerator<T> (no sema
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
                    (recvTy->headName() == "IEnumerator" ||
                     recvTy->headName() == "IOwningEnumerator");
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
        // #1349 / §29.1 v0.161: explicit `e.TakeCurrent()` on builtin
        // IOwningEnumerator<T> / IOwningAsyncEnumerator<T> (no sema TypeDecl).
        // Dispatches vtable slot 3 (same slot as IEnumerator Current).
        // emitPendingCheck() follows because TakeCurrent may raise
        // InvalidOperationException (double-take / before-first / after-exhaust).
        if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(
                call.callee.get())) {
            if (ma->member == "TakeCurrent" && ma->target &&
                    call.args.empty()) {
                const sema::TypeRef* recvTy = ctx_.exprType(*ma->target);
                bool isOwningEnumerator =
                    recvTy &&
                    recvTy->kind == sema::TypeRef::Kind::Named &&
                    !recvTy->declaration &&
                    recvTy->astNode && recvTy->astNode->name &&
                    !recvTy->astNode->name->parts.empty() &&
                    (recvTy->headName() == "IOwningEnumerator" ||
                     recvTy->headName() == "IOwningAsyncEnumerator");
                if (isOwningEnumerator) {
                    llvm::Value* recv = lowerExpr(*ma->target);
                    if (recv && recv->getType()->isPointerTy()) {
                        llvm::Type* elemTy =
                            (!recvTy->genericArgs.empty() &&
                             recvTy->genericArgs[0])
                            ? lowerTypeRef(recvTy->genericArgs[0])
                            : i32();
                        llvm::Value* vtable =
                            builder_.CreateLoad(ptrTy(), recv);
                        auto* tcTy =
                            llvm::FunctionType::get(elemTy, {ptrTy()}, false);
                        llvm::Value* tcSlotPtr = builder_.CreateGEP(
                            ptrTy(), vtable, {builder_.getInt32(3)});
                        llvm::Value* tcFnPtr =
                            builder_.CreateLoad(ptrTy(), tcSlotPtr);
                        llvm::Value* r = builder_.CreateCall(
                            tcTy, tcFnPtr, {recv}, "tc");
                        emitPendingCheck();  // TakeCurrent may raise IOE
                        return r;
                    }
                }
            }
        }
        // #1349 / §29.1 v0.161: explicit `src.GetOwningEnumerator()` on builtin
        // IOwningEnumerable<T>. Dispatches vtable slot 4 (same as GetEnumerator in
        // lowerIEnumerableBuiltinFor) to produce a fresh, independent
        // IOwningEnumerator<T> clone. The result is typed IOwningEnumerator<T> (by
        // sema), so using-var disposal wires up via isSyncIterEnum in Setup.cpp.
        if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(
                call.callee.get())) {
            if (ma->member == "GetOwningEnumerator" && ma->target &&
                    call.args.empty()) {
                const sema::TypeRef* recvTy = ctx_.exprType(*ma->target);
                bool isOwnedEnumerable =
                    recvTy &&
                    recvTy->kind == sema::TypeRef::Kind::Named &&
                    !recvTy->declaration &&
                    recvTy->astNode && recvTy->astNode->name &&
                    !recvTy->astNode->name->parts.empty() &&
                    recvTy->headName() == "IOwningEnumerable";
                if (isOwnedEnumerable) {
                    llvm::Value* src = lowerExpr(*ma->target);
                    if (src && src->getType()->isPointerTy()) {
                        llvm::Value* vt = builder_.CreateLoad(ptrTy(), src);
                        auto* geTy =
                            llvm::FunctionType::get(ptrTy(), {ptrTy()}, false);
                        llvm::Value* geSlotPtr = builder_.CreateGEP(
                            ptrTy(), vt, {builder_.getInt32(4)});
                        llvm::Value* geFnPtr =
                            builder_.CreateLoad(ptrTy(), geSlotPtr);
                        return builder_.CreateCall(
                            geTy, geFnPtr, {src}, "ge.owned");
                    }
                }
            }
        }
        // issue #759 / §29.3 / §30.7: explicit `e.MoveNextAsync()` where `e` is
        // the builtin IAsyncEnumerator<T> / IOwningAsyncEnumerator<T> (no sema
        // TypeDecl, hence no MethodSymbol). Same async-iterator vtable layout
        // [0]=td,[1]=Dispose,[2]=MoveNextAsync(→Task<bool> ptr),[3]=Current —
        // the layout emitAsyncIteratorMethod emits and `await for` dispatches.
        // Dispatch slot 2, returning the Task<bool> pointer; `await` (lowerAwait)
        // then consumes it as i1. Without this the call falls to a generic path
        // returning an i32, and `not <i32>` produces the malformed `br i32`.
        if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(
                call.callee.get())) {
            if (ma->member == "MoveNextAsync" && ma->target &&
                    call.args.empty()) {
                const sema::TypeRef* recvTy = ctx_.exprType(*ma->target);
                bool isAsyncEnumerator =
                    recvTy &&
                    recvTy->kind == sema::TypeRef::Kind::Named &&
                    !recvTy->declaration &&
                    recvTy->astNode && recvTy->astNode->name &&
                    !recvTy->astNode->name->parts.empty() &&
                    (recvTy->headName() == "IAsyncEnumerator" ||
                     recvTy->headName() == "IOwningAsyncEnumerator");
                if (isAsyncEnumerator) {
                    llvm::Value* recv = lowerExpr(*ma->target);
                    if (recv && recv->getType()->isPointerTy()) {
                        llvm::Value* vtable = builder_.CreateLoad(ptrTy(), recv);
                        auto* mnaTy =
                            llvm::FunctionType::get(ptrTy(), {ptrTy()}, false);
                        llvm::Value* slotPtr = builder_.CreateGEP(
                            ptrTy(), vtable, {builder_.getInt32(2)});
                        llvm::Value* fnPtr =
                            builder_.CreateLoad(ptrTy(), slotPtr);
                        llvm::Value* r = builder_.CreateCall(
                            mnaTy, fnPtr, {recv}, "mna");
                        emitPendingCheck();  // CG20: callee may have thrown
                        return r;
                    }
                }
            }
        }
        // issue #757 / §30.7: a direct `src.GetAsyncEnumerator(token)` on a builtin
        // IAsyncEnumerable<T>. For a compiler-generated async iterator the source
        // value (the frame `Gen()` returns) IS the enumerator — the same identity
        // the `await for` desugaring relies on (LlvmBackend_Stmts.cpp "the value IS
        // the enumerator"). Return the receiver unchanged; the token argument is
        // evaluated for side-effects/ownership then discarded (§30.8 cancellation
        // deferred, consistent with the `await for` path).
        if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(
                call.callee.get())) {
            if (ma->member == "GetAsyncEnumerator" && ma->target) {
                const sema::TypeRef* recvTy = ctx_.exprType(*ma->target);
                bool isAsyncEnumerable =
                    recvTy &&
                    recvTy->kind == sema::TypeRef::Kind::Named &&
                    !recvTy->declaration &&
                    recvTy->astNode && recvTy->astNode->name &&
                    !recvTy->astNode->name->parts.empty() &&
                    (recvTy->headName() == "IAsyncEnumerable" ||
                     recvTy->headName() == "IOwningAsyncEnumerable");
                if (isAsyncEnumerable) {
                    if (!call.args.empty() && call.args[0].value)
                        (void)lowerExpr(*call.args[0].value);
                    return lowerExpr(*ma->target);
                }
            }
        }
        // CG13/CG18: explicit `obj.Dispose()` call where Dispose is a
        // compiler-generated method (no MethodSymbol in sema). Emit a
        // dynamic vtable dispatch to the object's Dispose function (slot 1).
        if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(
                call.callee.get())) {
            if (ma->member == "Dispose" && ma->target) {
                // issue #1002 / §29.3 / §29.7: explicit `e.Dispose()` where `e`
                // is the builtin IEnumerator<T> / IOwningEnumerator<T> sentinel (a
                // GetEnumerator result, or a yield-method enumerator). It has no
                // sema TypeDecl, so the generic Dispose dispatch below would not
                // recognise it and the enumerator struct/frame would leak. Its
                // vtable is the iterator format [0]=td,[1]=Dispose,[2]=MoveNext,
                // [3]=Current — dispatch slot 1 (which for the array-wrapper clone
                // is a plain free() of the borrow-holding struct, leaving the
                // borrowed array intact, and for a yield enumerator runs the
                // state machine's pending finally then frees the frame). Null the
                // receiver slot afterward so a subsequent using/scope-exit
                // disposal of the same slot is a no-op (§28.2 exactly-once).
                {
                    auto* recvTy = ctx_.exprType(*ma->target);
                    std::string_view ebn = builtinTypeName(recvTy);
                    if (ebn == "IEnumerator" || ebn == "IOwningEnumerator") {
                        llvm::Value* obj = lowerExpr(*ma->target);
                        if (obj && obj->getType()->isPointerTy()) {
                            llvm::Value* vt = builder_.CreateLoad(ptrTy(), obj);
                            llvm::Value* dSlot = builder_.CreateGEP(
                                ptrTy(), vt, {builder_.getInt32(1)});
                            llvm::Value* dFn =
                                builder_.CreateLoad(ptrTy(), dSlot);
                            auto* dTy = llvm::FunctionType::get(
                                llvm::Type::getVoidTy(llvm_), {ptrTy()}, false);
                            builder_.CreateCall(dTy, dFn, {obj});
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
                // §6.2 / issue #647: Mutex.Dispose() — marks disposed (idempotent),
                // drops the Mutex object's core ref (destroys the underlying
                // pthread_mutex when no Lock is outstanding).
                {
                    auto* recvTy = ctx_.exprType(*ma->target);
                    if (recvTy && builtinTypeName(recvTy) == "Mutex") {
                        llvm::Value* mx = lowerExpr(*ma->target);
                        if (mx && mx->getType()->isPointerTy()) {
                            auto* fn = declareC("bz_mutex_dispose",
                                llvm::Type::getVoidTy(llvm_), {ptrTy()});
                            builder_.CreateCall(fn, {mx});
                            // Null the slot so any following using-var scope-exit
                            // reload sees null and skips the dispose (§28.2).
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
                // §6.2 / issue #647: Lock.Dispose() — release the held lock
                // (idempotent per §28.2: bz_lock_release handles the released flag).
                {
                    auto* recvTy = ctx_.exprType(*ma->target);
                    if (recvTy && builtinTypeName(recvTy) == "Lock") {
                        llvm::Value* lk = lowerExpr(*ma->target);
                        if (lk && lk->getType()->isPointerTy()) {
                            auto* fn = declareC("bz_lock_release",
                                llvm::Type::getVoidTy(llvm_), {ptrTy()});
                            builder_.CreateCall(fn, {lk});
                            // Null the slot so the using-var scope-exit guard
                            // finds null and skips (§28.2 exactly-once rule).
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
                // §6.3 / issue #647: Semaphore.Dispose() — marks disposed (idempotent),
                // drops the Semaphore object's core ref.
                {
                    auto* recvTy = ctx_.exprType(*ma->target);
                    if (recvTy && builtinTypeName(recvTy) == "Semaphore") {
                        llvm::Value* sem = lowerExpr(*ma->target);
                        if (sem && sem->getType()->isPointerTy()) {
                            auto* fn = declareC("bz_semaphore_dispose",
                                llvm::Type::getVoidTy(llvm_), {ptrTy()});
                            builder_.CreateCall(fn, {sem});
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
                // §6.3 / issue #647: SemaphoreToken.Dispose() — release the held permit
                // (idempotent per §28.2: bz_semaphore_token_release handles the flag).
                {
                    auto* recvTy = ctx_.exprType(*ma->target);
                    if (recvTy && builtinTypeName(recvTy) == "SemaphoreToken") {
                        llvm::Value* tok = lowerExpr(*ma->target);
                        if (tok && tok->getType()->isPointerTy()) {
                            auto* fn = declareC("bz_semaphore_token_release",
                                llvm::Type::getVoidTy(llvm_), {ptrTy()});
                            builder_.CreateCall(fn, {tok});
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
                // §6.4 / issue #647: Atomic<T>.Dispose() and AtomicInteger<T>.Dispose()
                // — idempotent dispose via bz_atomic_dispose (§28.2 exactly-once rule).
                {
                    auto* recvTy = ctx_.exprType(*ma->target);
                    std::string_view rbn = builtinTypeName(recvTy);
                    if (rbn == "Atomic" || rbn == "AtomicInteger") {
                        llvm::Value* atom = lowerExpr(*ma->target);
                        if (atom && atom->getType()->isPointerTy()) {
                            auto* fn = declareC("bz_atomic_dispose",
                                llvm::Type::getVoidTy(llvm_), {ptrTy()});
                            builder_.CreateCall(fn, {atom});
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
                // issue #545: Buffer<T>.Dispose() — free via bz_buffer_free.
                // bz_buffer_free is null-safe (no-op on null); value elements carry
                // no ownership obligation so no per-element callback is needed.
                {
                    auto* recvTy = ctx_.exprType(*ma->target);
                    if (recvTy && recvTy->kind == sema::TypeRef::Kind::Named &&
                        builtinTypeName(recvTy) == "Buffer") {
                        llvm::Value* buf = lowerExpr(*ma->target);
                        if (buf && buf->getType()->isPointerTy()) {
                            auto* fn = declareC("bz_buffer_free",
                                llvm::Type::getVoidTy(llvm_), {ptrTy()});
                            builder_.CreateCall(fn, {buf});
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
                // §8.1 / issue #646: Stopwatch.Dispose() — free the bz_stopwatch_t
                // heap object via bz_stopwatch_free.  Called by `using var` scope
                // exit (§28.2 exactly-once rule).
                {
                    auto* recvTy = ctx_.exprType(*ma->target);
                    if (recvTy && recvTy->kind == sema::TypeRef::Kind::Named &&
                        builtinTypeName(recvTy) == "Stopwatch") {
                        llvm::Value* recv = lowerExpr(*ma->target);
                        if (recv && recv->getType()->isPointerTy()) {
                            auto* fn = declareC("bz_stopwatch_free",
                                llvm::Type::getVoidTy(llvm_), {ptrTy()});
                            builder_.CreateCall(fn, {recv});
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
                        (recvTy->headName() == "Set" ||
                         recvTy->headName() == "OwningSet");
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
                        // OwningLinkedList<T> cascades disposal into class elements.
                        // #602 / §28.11 v0.135: route directly through elementPolicyFor.
                        {
                            const sema::TypeRef* llElem =
                                (!recvTy->genericArgs.empty())
                                    ? recvTy->genericArgs[0] : nullptr;
                            bool llIsOwning =
                                recvTy->astNode && recvTy->astNode->name &&
                                !recvTy->astNode->name->parts.empty() &&
                                common::isOwningPrefixedName(recvTy->headName());
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
                                common::isOwningPrefixedName(recvTy->headName());
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
                        //   * OwningDictionary<K, class/interface> → dispose+free
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
                                recvTy->headName() == "OwningDictionary";
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
                            // §28.2: a *repeated* explicit `x.Dispose()` on the
                            // same owned local must be a no-op. The first call
                            // freed the block and nulled the slot (below), so on
                            // the second call `recv` (reloaded from that slot) is
                            // null — dispatching a vtable Dispose through it would
                            // be a use-after-free / null-deref SIGSEGV. Guard the
                            // whole dispatch+reclaim on a non-null receiver so the
                            // second `x.Dispose()` cleanly skips. (Checked mode
                            // does not null the slot — the object is quarantined —
                            // so `recv` stays non-null there and the run-once flag
                            // makes the re-dispatch a no-op.)
                            auto* nn = llvm::BasicBlock::Create(
                                llvm_, "disp.nn", currentFn_);
                            auto* cont = llvm::BasicBlock::Create(
                                llvm_, "disp.cont", currentFn_);
                            builder_.CreateCondBr(
                                builder_.CreateICmpNE(
                                    recv,
                                    llvm::ConstantPointerNull::get(ptrTy())),
                                nn, cont);
                            builder_.SetInsertPoint(nn);
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
                            builder_.CreateBr(cont);
                            builder_.SetInsertPoint(cont);
                        } else if (isFreshOwnedClassProducer(ma->target.get())) {
                            // #1053-case-1 / #415: no owning slot, but the
                            // receiver is a FRESH owned producer (`new X()` or an
                            // `owned`-returning call) — owned by nothing else, so
                            // dispatch-only (below) would leak its block. Reclaim
                            // it exactly once: this IS the sole explicit Dispose
                            // (one destructor run) plus the block free. A BORROW
                            // receiver is NOT a fresh producer (its real owner
                            // reclaims via ITS slot), so it stays dispatch-only.
                            emitDisposeAndReclaim(recv);
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

    // §11.8 (v0.176, #807): `T.Member(...)` — a STATIC interface member called
    // through a constrained type parameter. Sema resolved the callee to the
    // INTERFACE's static member symbol; at this specialization T is concrete
    // (genericSubstSemaTy_), so redirect `ms` to the CONCRETE type's own
    // static member — a direct call, no vtable/itable (statics have no runtime
    // dispatch). A PRIMITIVE substitution was already intercepted by
    // tryLowerPrimitiveStaticCall (the §2.19 compiler-provided conformances);
    // reaching here with one means an unbridged member — emit the placeholder
    // default so the (dead) generic template body still IR-verifies.
    if (ms->parent && ms->parent->kind == ast::TypeDeclKind::Interface &&
        !ms->isOperator && isStaticMethod(ms)) {
        if (auto* cma = dynamic_cast<const ast::MemberAccessExpr*>(
                call.callee.get())) {
            auto* tid = cma->target
                ? dynamic_cast<const ast::IdentifierExpr*>(cma->target.get())
                : nullptr;
            if (tid && dynamic_cast<sema::GenericParamSymbol*>(
                           ctx_.exprSymbol(*tid))) {
                const sema::TypeRef* concrete = nullptr;
                auto sit = genericSubstSemaTy_.find(tid->name);
                if (sit != genericSubstSemaTy_.end()) concrete = sit->second;
                sema::MethodSymbol* impl = nullptr;
                if (concrete && concrete->kind == sema::TypeRef::Kind::Named &&
                    concrete->declaration) {
                    // The §11.8 conformance check guarantees a signature-exact
                    // static; match by name + arity (statics don't overload on
                    // the interface surface in v1).
                    for (sema::Symbol* m : concrete->declaration->members) {
                        auto* cand = dynamic_cast<sema::MethodSymbol*>(m);
                        if (!cand || cand->isOperator) continue;
                        if (cand->name != ms->name) continue;
                        if (!isStaticMethod(cand)) continue;
                        if (cand->params.size() != ms->params.size()) continue;
                        impl = cand;
                        break;
                    }
                }
                if (impl) {
                    ms = impl;  // direct static call on the concrete type
                } else {
                    // Unspecialized template body (T unbound) or an unbridged
                    // substitution: produce a type-correct placeholder value —
                    // the template is never a call target (#817 posture).
                    sema::TypeRef* resTy = ctx_.exprType(call);
                    if (resTy && resTy->kind != sema::TypeRef::Kind::Unit) {
                        llvm::Type* t = lowerTypeRef(resolveTypeParamRef(resTy));
                        if (t && !t->isVoidTy())
                            return llvm::Constant::getNullValue(t);
                    }
                    return nullptr;
                }
            }
        }
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
        // §14.2 (value-type-conforms-to-interface) + §9.4/§10.5 (record/enum =
        // value type): when a generic method `M<T>(x: T) where T: IFoo` is
        // monomorphized with T = a RECORD or an ENUM, the body's `x.Foo()` was
        // resolved by sema to the INTERFACE method `IFoo.Foo` (member look-up
        // went through the constraint — §14.2's "T's surface includes IFoo's
        // members"). The receiver `x` is a value (a record struct, or an enum's
        // underlying integer — not a pointer), so the itable-dispatch path in
        // emitMethodCall would either need a pointer receiver or hit the #484
        // non-pointer guard (returning nullptr → a malformed direct call to the
        // i32-typed generic template, which for CompareTo/Equals silently yields
        // the receiver-against-itself value, #1495). Since T is concretely known
        // at this monomorphization, devirtualize: redirect `ms` to the concrete
        // value type's own implementing method (records are sealed — §9.6 — and
        // an enum's declared interface method — §10.5 — is likewise exactly the
        // one declared). The receiver-shaping block below then spills a record
        // value (matching `point.Foo()`) or passes an enum receiver as its
        // underlying value (matching the direct `Priority.Low.CompareTo(...)`
        // path — enum methods take `this` by value, not by pointer).
        if (ms->parent && ms->parent->kind == ast::TypeDeclKind::Interface &&
            !recv->getType()->isPointerTy()) {
            if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(
                    call.callee.get())) {
                if (const sema::TypeRef* recvTy =
                        ma->target ? ctx_.exprType(*ma->target) : nullptr) {
                    // Resolve a bare type-parameter receiver (`T`, no
                    // declaration) through the active substitution to its
                    // concrete sema type, then take its record/enum declaration.
                    const sema::TypeRef* concrete = recvTy;
                    if (recvTy->kind == sema::TypeRef::Kind::Named &&
                        !recvTy->declaration && recvTy->astNode &&
                        recvTy->astNode->name &&
                        recvTy->astNode->name->parts.size() == 1) {
                        auto sit = genericSubstSemaTy_.find(
                            recvTy->headName());
                        if (sit != genericSubstSemaTy_.end() && sit->second)
                            concrete = sit->second;
                    }
                    if (concrete && concrete->declaration &&
                        (concrete->declaration->kind ==
                             ast::TypeDeclKind::Record ||
                         concrete->declaration->kind ==
                             ast::TypeDeclKind::Enum)) {
                        if (sema::MethodSymbol* impl = findMethodByName(
                                concrete->declaration, ms->name))
                            ms = impl;  // devirtualize to the value type's method
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
        // §26.5 / #1237: when the extension targets an INTERFACE and the actual
        // receiver is a value type (record/enum), box it into a proper interface
        // reference ({ vtable*, i64 rc, record-bytes }) so the extension body's
        // `this.M()` / `this[i]` dispatches through the correct itable.
        if (isExtensionMethod(ms)) {
            bool byPtr = false;
            llvm::Type* thisTy = extensionThisType(ms, &byPtr);
            if (byPtr && thisTy && !recv->getType()->isPointerTy()) {
                // Determine whether the declared extension target is an interface.
                const sema::TypeRef* extTgt = ms->extensionDecl->targetType
                    ? ctx_.resolvedType(*ms->extensionDecl->targetType)
                    : nullptr;
                bool targetIsIface = extTgt &&
                    extTgt->kind == sema::TypeRef::Kind::Named &&
                    extTgt->declaration &&
                    extTgt->declaration->kind == ast::TypeDeclKind::Interface;
                if (targetIsIface) {
                    // Box the record/value receiver into an interface reference
                    // so the extension body's itable dispatch (`this.M()` /
                    // `this[i]`) reaches the correct vtable / itable map.
                    // Simply spilling to a stack slot leaves a raw struct with
                    // no vtable header → SIGSEGV on itable lookup (§26.5/#1237).
                    const sema::TypeRef* recvSemaTy = nullptr;
                    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(
                            call.callee.get())) {
                        if (ma->target)
                            recvSemaTy = ctx_.exprType(*ma->target);
                    }
                    if (recvSemaTy && isBoxableValueType(recvSemaTy))
                        recv = boxValue(recv, recvSemaTy);
                    // If the sema type could not be recovered (unusual) or the
                    // value is not a boxable type, leave recv as-is; the callee
                    // will guard via the non-pointer check if needed.
                } else {
                    // Record (or other value) receiver: spill to a stack slot
                    // and pass its address so `this.field` in the body uses the
                    // record field-access machinery (§26.1 / #433 behaviour).
                    llvm::Value* tmp = entryAlloca(recv->getType());
                    builder_.CreateStore(recv, tmp);
                    recv = tmp;
                }
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
            // §22.3 / #1221: for an imported method (ms->decl is not an
            // ast::MethodDecl) the formal param modifier lives on the
            // ParameterSymbol in ms->params.  Mirror buildOrderedArgs: pass the
            // variable's ADDRESS for ref/out params instead of its loaded value.
            std::size_t pi = 0;
            for (const auto& a : call.args) {
                if (!a.value) {
                    if (didPrePass) {
                        genericSubst_ = std::move(preSavedSubst);
                        genericSubstSemaTy_ = std::move(preSavedSema);
                    }
                    return nullptr;
                }
                // Determine the formal parameter's ref/out modifier from the
                // imported ParameterSymbol; also honour the call-site modifier
                // (a.modifier carries "ref"/"out" written at the call site).
                std::string_view formalMod;
                if (pi < ms->params.size())
                    if (auto* ps =
                            dynamic_cast<sema::ParameterSymbol*>(ms->params[pi]))
                        formalMod = ps->modifier;
                bool byRef = isByRefMod(a.modifier) || isByRefMod(formalMod);
                llvm::Value* v;
                if (byRef) {
                    llvm::Type* et = nullptr;
                    v = lvalueAddr(*a.value, et);
                } else {
                    v = lowerExpr(*a.value);
                }
                if (!v) {
                    if (didPrePass) {
                        genericSubst_ = std::move(preSavedSubst);
                        genericSubstSemaTy_ = std::move(preSavedSema);
                    }
                    return nullptr;
                }
                args.push_back(v);
                argExprs.push_back(a.value.get());
                ++pi;
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
                // §14.1 / #783: an IMPORTED generic class has no ast::TypeDecl
                // (ms->parent->decl is null) but DOES carry sema-level
                // genericParams (reconstructed by the loader). Take the type
                // parameter NAMES from the AST decl when present, else from the
                // imported TypeSymbol's genericParams, so the per-instantiation
                // substitution installs for a cross-module receiver too.
                std::vector<std::string_view> gpNames;
                if (td) {
                    for (const auto& gp : td->genericParams)
                        gpNames.push_back(gp.name);
                } else {
                    for (sema::Symbol* gp : ms->parent->genericParams)
                        if (gp) gpNames.push_back(gp->name);
                }
                if (gpNames.size() == recvTy->genericArgs.size() &&
                    !gpNames.empty()) {
                    // Resolve generic args using the OUTER substitution so
                    // that `Pair<T>` in `Pair<string>` context resolves T→ptr.
                    std::unordered_map<std::string_view, llvm::Type*> newSubst;
                    std::unordered_map<std::string_view, const sema::TypeRef*> newSema;
                    for (std::size_t gi = 0;
                         gi < gpNames.size(); ++gi) {
                        llvm::Type* concrTy = lowerTypeRef(recvTy->genericArgs[gi]);
                        std::string_view pname = gpNames[gi];
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
                       !ms->parent->genericParams.empty()) {
                // §12 / §14 (issue #585): an INHERITED method declared on a
                // generic BASE (`ms->parent`) called on a derived instance
                // (`recvTy` == Wrapper<string>). The branch above requires
                // recvTy->declaration == ms->parent, so it does not fire; without
                // a substitution the inherited method runs as the T→i32 template
                // and reads a non-int base field at the wrong width (#585). Walk
                // the base chain to bind the base's generic params to the
                // concrete arguments the derived class supplied.
                //
                // #902: this also covers a CONCRETE (non-generic) class derived
                // from a generic base — `class CtrHolder : Holder<Counter>`. Such
                // a receiver has NO generic args of its own, so the previous
                // `!recvTy->genericArgs.empty()` guard excluded it and the
                // inherited `holder.GetConcrete()` dispatched to the type-erased
                // template (which after #817 nulls its irreconcilable `T`-typed
                // return → wrong null). computeAncestorGenericSubst walks the base
                // chain and returns false when there is nothing to bind, so it is
                // safe to reach here for a concrete-derived receiver too.
                std::unordered_map<std::string_view, llvm::Type*> ancSubst;
                std::unordered_map<std::string_view, const sema::TypeRef*>
                    ancSema;
                if (computeAncestorGenericSubst(recvTy, ms->parent, ancSubst,
                                                ancSema)) {
                    genericSubst_ = std::move(ancSubst);
                    genericSubstSemaTy_ = std::move(ancSema);
                }
            } else if (!recvTy && !ms->parent->genericParams.empty()) {
                // §12 / §14 (#747 R42): an INHERITED method declared on a generic
                // BASE, called via a `this`/`base` KEYWORD receiver. `this`/`base`
                // carry no exprType (recvTy null), so neither branch above fired
                // and the inherited body ran as the T→i32 template (IR_FAIL on a
                // ptr/struct return, silent wrong value). Recover the enclosing
                // class (indexReceiverDecl's reverse-lookup of currentFn_) and bind
                // the base's generic params from the derived→base<…> chain, exactly
                // as the #585 local-receiver branch above.
                if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(recvExpr)) {
                    if (id->name == "this" || id->name == "base") {
                        const sema::TypeSymbol* enclosingCls =
                            indexReceiverDecl(recvExpr);
                        if (enclosingCls && enclosingCls != ms->parent) {
                            sema::TypeRef synth;
                            synth.kind = sema::TypeRef::Kind::Named;
                            synth.declaration =
                                const_cast<sema::TypeSymbol*>(enclosingCls);
                            std::unordered_map<std::string_view, llvm::Type*>
                                ancSubst;
                            std::unordered_map<std::string_view,
                                               const sema::TypeRef*> ancSema;
                            if (computeAncestorGenericSubst(&synth, ms->parent,
                                                            ancSubst, ancSema)) {
                                genericSubst_ = std::move(ancSubst);
                                genericSubstSemaTy_ = std::move(ancSema);
                            }
                        }
                    }
                }
            }
        } else if (!ms->parent->genericParams.empty()) {
            // §12 / §14 / #1101-C: an INHERITED generic-base method called via a
            // BARE identifier (implicit `this.Make()`), not a member access — so
            // `recvExpr` is null and none of the branches above fired. The
            // inherited body then ran as the T→i32 template, an IR_FAIL on a
            // ptr/struct `T?` return for a non-int instantiation (the call
            // returned `{i8,i32}` into a ptr-returning caller). Recover the
            // enclosing class from the current function (mirroring
            // indexReceiverDecl's reverse-lookup of currentFn_) and bind the
            // base's generic params from the derived→base<…> chain, exactly as
            // the `this`/`base` keyword branch above.
            const sema::TypeSymbol* enclosingCls = nullptr;
            if (currentFn_) {
                for (const auto& [m2, fn] : methodFns_)
                    if (fn == currentFn_ && m2->parent) {
                        enclosingCls = m2->parent;
                        break;
                    }
                if (!enclosingCls && currentSpecMethodSymbol_)
                    enclosingCls = currentSpecMethodSymbol_->parent;
            }
            if (enclosingCls && enclosingCls != ms->parent) {
                sema::TypeRef synth;
                synth.kind = sema::TypeRef::Kind::Named;
                synth.declaration = const_cast<sema::TypeSymbol*>(enclosingCls);
                std::unordered_map<std::string_view, llvm::Type*> ancSubst;
                std::unordered_map<std::string_view, const sema::TypeRef*>
                    ancSema;
                if (computeAncestorGenericSubst(&synth, ms->parent, ancSubst,
                                                ancSema)) {
                    genericSubst_ = std::move(ancSubst);
                    genericSubstSemaTy_ = std::move(ancSema);
                }
            }
        }
    }
    // §26.5 (#714): a GENERIC EXTENSION METHOD (ms->parent == null,
    // ms->extensionDecl != null, extension has free type variables).  Sema
    // recorded the free-var binding (e.g. T → int for `Array<int>` receiver
    // on `extension IEnumerable<T>`) in the SemaContext keyed by the callee
    // MemberAccessExpr.  Read it here to set up genericSubst_/
    // genericSubstSemaTy_ so that emitInlineGenericMethod specialises the body
    // with the correct concrete types (replacing the i32 placeholder).
    if (!isStaticMethod(ms) && !ms->parent && ms->extensionDecl) {
        if (auto* callMa = dynamic_cast<const ast::MemberAccessExpr*>(
                call.callee.get())) {
            if (const auto* bnd = ctx_.exprExtBinding(*callMa)) {
                if (!bnd->empty()) {
                    std::unordered_map<std::string_view, llvm::Type*> newSubst;
                    std::unordered_map<std::string_view,
                                       const sema::TypeRef*> newSema;
                    for (const auto& [nm, ty] : *bnd) {
                        if (!ty) continue;
                        newSubst[nm] = lowerTypeRef(ty);
                        newSema[nm] = ty;
                    }
                    if (!newSubst.empty()) {
                        genericSubst_ = std::move(newSubst);
                        genericSubstSemaTy_ = std::move(newSema);
                    }
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
                        // §22.12 / #684: for LOCAL generics, read the param
                        // names from the ast::TypeDecl node (ms->parent->decl).
                        // For IMPORTED generics, ms->parent->decl is null but
                        // ms->parent->genericParams is populated by ModuleLoader
                        // from the metadata (§14/#662c). Fall back to the
                        // TypeSymbol's genericParams so imported generics get
                        // the correct per-instantiation genericSubst_ and
                        // staticFieldGlobal routes to the per-instantiation
                        // global, not the external template-level declaration.
                        auto* td = dynamic_cast<const ast::TypeDecl*>(
                            ms->parent->decl);
                        const std::size_t nGP =
                            td ? td->genericParams.size()
                               : ms->parent->genericParams.size();
                        if (nGP == recvTy->genericArgs.size()) {
                            std::unordered_map<std::string_view, llvm::Type*>
                                newSubst;
                            std::unordered_map<std::string_view,
                                               const sema::TypeRef*>
                                newSema;
                            for (std::size_t gi = 0; gi < nGP; ++gi) {
                                llvm::Type* concrTy =
                                    lowerTypeRef(recvTy->genericArgs[gi]);
                                // Local generic: name comes from ast::TypeDecl;
                                // imported: name comes from sema::Symbol.
                                std::string_view pname =
                                    td ? td->genericParams[gi].name
                                       : ms->parent->genericParams[gi]->name;
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
    // per §14.1/§14.2 (issue #1278): a generic *method* (its own `<T>`
    // params, e.g. `static method Clone<T>(proto: T): T where T: class,
    // new()`) is monomorphized by inferring each method type parameter from
    // the actual argument types, explicit call-site type args, or the call's
    // target-inferred result type. Factored into applyMethodGenericSubst so
    // lowerNullCondCall (`?.`) shares the exact same trigger.
    applyMethodGenericSubst(call, ms);
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

    // #429 (ARGUMENT-PASS category) / §4.6 / §28.9: a +1 string producer
    // (concat / interpolation / range-slice / user-method-return) that appears
    // DIRECTLY as a call argument (not bound to a local first) holds an
    // independent rc=1 reference.  The callee receives the string as a BORROW
    // (Bezel parameters are borrows by default); if the callee stores the param
    // into a string field/auto-property the field-store path inside the callee
    // already retains it independently (rc→2), so our release here (rc→1) is
    // safe — the field still owns its +1.  If the callee only reads the string
    // and discards it, our release (rc→0) reclaims the temporary.
    //
    // Release is emitted ONLY when the call was actually lowered (!diverged_):
    // on an exception path the temporary leaks, which is the pre-existing safe
    // direction (a leak is harmless; a double-free / UAF is not).
    //
    // Identification: argExprs (populated by buildOrderedArgs) is parallel to
    // args.  For instance methods args[0] = receiver (argExprs[0] = nullptr);
    // for static methods the args start at index 0.  We scan from argOffset
    // to args.size() and release any argument whose expression is a +1 producer.
    if (!diverged_ && !args.empty()) {
        std::size_t argOffset = isStaticMethod(ms) ? 0 : 1;
        for (std::size_t ai = argOffset; ai < args.size() && ai < argExprs.size(); ++ai) {
            if (argExprs[ai] && isPlusOneStringProducer(argExprs[ai]))
                emitStringRelease(args[ai]);
        }
    }

    // #1039 / #1053 / §28.3: a fresh owned-producer (`new <class>()` or an
    // `owned`-returning call) used only as the receiver of an instance-method
    // call (args[0] = receiver) leaves the temporary owned by nothing — dispose
    // it after the call. The helper is a no-op unless the receiver is such a
    // fresh producer of a heap class and the result is a primitive/enum/unit
    // value (Bezel's borrow checker forbids `this` escaping such a call, so
    // post-call disposal is sound — §15.3). An explicit `.Dispose()` never
    // reaches here — it is handled by the dedicated `obj.Dispose()` path above,
    // which reclaims a fresh unbound producer itself (#1053-case-1).
    if (!diverged_ && !isStaticMethod(ms) && !args.empty() &&
        ms->name != "Dispose") {
        const ast::Expr* recvExpr = nullptr;
        if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(
                call.callee.get()))
            recvExpr = ma->target.get();
        disposeFreshNewReceiverTemp(recvExpr, args[0], ctx_.exprType(call));
    }

    return result;
}

// per §14.1/§14.2 + §26.5/§7.5 (issue #1278): see the header comment in
// LlvmBackend_impl.hpp. Extracted verbatim from lowerCall's inline block so
// lowerNullCondCall (`?.`) drives the identical method-generic substitution
// trigger as lowerCall (`.`) — previously only `.` installed this, so an
// explicit-type-arg `?.` generic call reached emitMethodCall with an empty
// genericSubst_ and called the unspecialized i32-placeholder body.
void Emitter::applyMethodGenericSubst(const ast::CallExpr& call,
                                      sema::MethodSymbol* ms) {
    auto* md = dynamic_cast<const ast::MethodDecl*>(ms->decl);
    if (!md || md->genericParams.empty()) return;
    std::unordered_map<std::string_view, llvm::Type*> newSubst =
        genericSubst_;
    std::unordered_map<std::string_view, const sema::TypeRef*> newSema =
        genericSubstSemaTy_;
    // §14.1 (#785): bind each method type parameter by STRUCTURALLY
    // unifying every value parameter's declared type against the call
    // argument's type — so a generic-typed parameter `p: Pair<A, B>`
    // binds A and B from a `Pair<int, string>` argument, not only a
    // top-level bare `p: T`. Reuses the same recursive unification the
    // return-type inference uses (unifyMethodReturnTypeArg); the helper
    // skips any param already bound (genericSubst_ from the receiver, or
    // an earlier parameter).
    for (std::size_t pi = 0; pi < md->params.size() &&
         pi < call.args.size(); ++pi) {
        if (!md->params[pi].type || !call.args[pi].value) continue;
        // §14.1 / #885 #886: a `null` literal carries no concrete type
        // information — it is typed by sema as Object (the root reference
        // type) or as the declared param type T?, neither of which reveals
        // T's concrete binding. Binding T → Object here blocks the correct
        // binding from a subsequent concrete argument or explicit type arg,
        // causing nullableTypeParamConcrete to fail → null not wrapped →
        // PHI/ICmp type-mismatch crash in the specialized body. Skip null
        // literals during type-param inference. per §14.1/§14.2, #885/#886.
        if (dynamic_cast<const ast::NullLiteralExpr*>(
                call.args[pi].value.get()))
            continue;
        const sema::TypeRef* declPt =
            ctx_.resolvedType(*md->params[pi].type);
        sema::TypeRef* at = ctx_.exprType(*call.args[pi].value);
        if (!declPt || !at) continue;
        bool added = false;
        unifyMethodReturnTypeArg(declPt, at, md->genericParams,
                                 newSubst, newSema, added);
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
    // The callee may be a MemberAccessExpr (`obj.Method<T>(...)`), a
    // NullCondMemberExpr (`obj?.Method<T>(...)` — §7.5/#1278), or a bare
    // IdentifierExpr (`Id<T>(...)` — in-scope call without dot).
    {
    const std::vector<std::unique_ptr<ast::Type>>* callTAs = nullptr;
    if (auto* callMa = dynamic_cast<const ast::MemberAccessExpr*>(
            call.callee.get()))
        callTAs = &callMa->typeArgs;
    else if (auto* callNcm = dynamic_cast<const ast::NullCondMemberExpr*>(
                 call.callee.get()))
        callTAs = &callNcm->typeArgs;
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
                    auto it = curArgs.find(arg->headName());
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
        // #1592 / §12.1: if this class-virtual slot is widened (a VALUE-typed
        // parameter, e.g. `int` → `int?`) by ANY contributor anywhere in the
        // whole hierarchy, every call to the slot — regardless of which
        // ancestor's static type resolved `ms` — must use the family's
        // uniform (wide) ABI, matching what declareVtables installed at
        // every contributing class's vtable slot (a direct wide-ABI fn, or
        // an unboxing thunk for a still-narrow contributor —
        // LlvmBackend_Setup.cpp). A call resolved through a narrow
        // (`T`-declaring) static type always has a definite (non-null) value
        // in hand, so it is boxed here; a call already resolved through a
        // wide (`T?`-declaring) static type finds its argument already
        // boxed by the ordinary per-parameter coercion the caller performed
        // against `ms`'s own (already wide) declared type. Reference-typed
        // widening (#1560) is unaffected: isNullableValueTy is false for
        // those, so `widened` stays all-false and this block is a no-op.
        // Interface dispatch and any active generic substitution don't use
        // this mechanism (out of scope — gated out below).
        if (!isIfaceMethod && genericSubst_.empty()) {
            const sema::MethodSymbol* rootMs = introducingVirtualMethod(ms);
            const std::vector<bool>& widened = widenedParamPositions(rootMs);
            bool anyWiden = false;
            for (bool w : widened) if (w) { anyWiden = true; break; }
            if (anyWiden) {
                if (llvm::FunctionType* uniformTy =
                        uniformVtableFnType(rootMs, widened)) {
                    for (std::size_t pi = 0; pi < widened.size(); ++pi) {
                        if (!widened[pi]) continue;
                        std::size_t ai = pi + 1;  // skip the receiver
                        if (ai >= args.size() || !fnTy ||
                            ai >= fnTy->getNumParams())
                            continue;
                        if (fnTy->getParamType(ai) == uniformTy->getParamType(ai))
                            continue;  // ms's own declared param is already wide.
                        sema::TypeRef* pt = paramTypeOf(rootMs->params[pi]);
                        if (!pt) continue;
                        sema::TypeRef nn = *pt;
                        nn.nullable = true;
                        args[ai] = wrapNullableValue(&nn, args[ai]);
                    }
                    fnTy = uniformTy;
                }
            }
        }
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
        // #1183 / §11.2 / §12.1: builtin generic interfaces
        // (IComparable<T>/IEquatable<T>/IEqualityComparer<T>) compile
        // their template method with T erased to i32; when the substituted
        // T's ABI differs (a record struct, ptr for string, i64 for long),
        // fnTy's parameter types don't match the actual args. Build a
        // concrete fnTy from the real arg types so the call instruction is
        // well-typed. The itable slot holds the concrete function directly
        // (ensureNullableWideningThunk skips thunk-generation for
        // non-widening mismatches), so the call dispatches correctly.
        // Only applied to interface dispatch (isIfaceMethod) where the
        // generic-T erasure mismatch can arise.
        if (isIfaceMethod) {
            bool needsConcrete = false;
            for (std::size_t ai = 0; ai < args.size(); ++ai)
                if (args[ai]->getType() != fnTy->getParamType(ai))
                    { needsConcrete = true; break; }
            if (needsConcrete) {
                std::vector<llvm::Type*> concParamTys;
                concParamTys.reserve(args.size());
                for (llvm::Value* a : args)
                    concParamTys.push_back(a->getType());
                fnTy = llvm::FunctionType::get(
                    fnTy->getReturnType(), concParamTys, /*isVarArg=*/false);
            }
        }
        llvm::Value* r = builder_.CreateCall(fnTy, fnPtr, args);
        emitPendingCheck();  // CG20: a callee may have thrown
        return r;
    }

    // §14.1 / §2.5 / #783: an IMPORTED generic-class method has no compiled
    // function in methodFns_ (its body lives in the defining module, compiled
    // against the i32 placeholder). When a per-instantiation substitution is
    // active and the loader has attached the re-parsed body (ms->decl is now a
    // live ast::MethodDecl — schema v5 methodBodyText), monomorphize the body
    // importer-side under the concrete genericSubst_, exactly as the local
    // generic-method path does. This is the cross-module complement that lifts
    // the #783 sema gate for the shapes whose body could be reconstructed; a
    // method whose body was not exported (abstract, no span, parse/sema failure)
    // has no ast::MethodDecl, falls through, and stays gated.
    if (!genericSubst_.empty() && ctx_.isImported(ms) && ms->parent &&
        !ms->parent->genericParams.empty() &&
        dynamic_cast<const ast::MethodDecl*>(ms->decl)) {
        llvm::Value* r = emitInlineGenericMethod(ms, args);
        emitPendingCheck();
        // HARD route: never fall through to methodFns_ for an imported generic
        // method. The only entry there is the EXTERNAL declaration of the
        // defining module's i32-PLACEHOLDER template (`bz$<Class>$<M>`); calling
        // it with concrete `ptr`/struct args for a non-int instantiation would
        // pass/return at the wrong width — a silent miscompile. The inline
        // monomorphization is the only correct lowering; return its result
        // (null → the caller emits the same safe stub as any unlowered call,
        // never a wrong-typed call to the template). The §14.1/#783 sema gate
        // already rejects shapes whose body could not be reconstructed (decl
        // null), so reaching here means the body IS available.
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
            // #1253: `md` is null when ms->decl is non-null but NOT an
            // ast::MethodDecl — e.g. a user-defined record/class `operator`
            // declaration. Such a decl carries no method-level generic params,
            // so there is nothing to force-specialize; guard the deref (the
            // unguarded `md->genericParams` here SIGSEGV'd the compiler when a
            // generic method body invoked a record operator, §14/§25.3).
            if (!typesMismatch && md && !md->genericParams.empty()) {
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
    // Build the function type with concrete arg/ret types. Each param type
    // defaults to its argument's LLVM type, BUT when the declared parameter is a
    // nullable VALUE type `T?` (a bare value-type generic param under the active
    // substitution) and the argument arrived as the bare `T` (or a null ptr),
    // the specialized body reads the param as the tagged `{i8, T}` struct — so
    // the argument must be wrapped HERE and the param typed as that struct.
    // Without this, a bare-`int` argument bound to a `T?` parameter leaves the
    // param as `i32`, and `v!` / `v ?? …` in the body extract a field from a
    // non-struct → LLVM GEP assertion crash (#846) / IR-verify failure (#845).
    // `args` is [this?, params...]; map back to ms->params with the `this`
    // offset. (#568/#604 resolve the same `T?` shape at the `!` / return sites.)
    std::vector<llvm::Type*> paramTys;
    std::size_t thisOff = isStaticMethod(ms) ? 0u : 1u;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (i >= thisOff && (i - thisOff) < ms->params.size()) {
            if (auto* ps = dynamic_cast<sema::ParameterSymbol*>(
                    ms->params[i - thisOff])) {
                const sema::TypeRef* formal =
                    ps->declType ? ctx_.resolvedType(*ps->declType)
                                 : ps->resolvedDeclType;
                sema::TypeRef concBuf;
                const sema::TypeRef* nvTy = nullptr;
                if (formal && isNullableValueTy(formal)) {
                    nvTy = formal;
                } else if (formal && formal->nullable &&
                           formal->kind == sema::TypeRef::Kind::Named &&
                           !formal->declaration &&
                           nullableTypeParamConcrete(formal, concBuf) &&
                           isNullableValueTy(&concBuf)) {
                    nvTy = &concBuf;
                }
                if (nvTy) {
                    auto* expectedSt = nullableValueStructTy(nvTy);
                    if (args[i]->getType() != expectedSt) {
                        if (args[i]->getType()->isPointerTy() &&
                            llvm::isa<llvm::ConstantPointerNull>(args[i])) {
                            args[i] = nullableValueNull(nvTy);
                        } else if (args[i]->getType() ==
                                   expectedSt->getElementType(1)) {
                            args[i] = wrapNullableValue(nvTy, args[i]);
                        }
                    }
                }
            }
        }
        paramTys.push_back(args[i]->getType());
    }
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
        if (rt) {
            retTy = lowerTypeRef(rt);
            // §14.1 (#767): when the declared return type is a BARE, NON-NULLABLE
            // type parameter `T` bound to a record, lowerTypeRef yields the
            // cached UNSPECIALIZED struct from genericSubst_, but the body lowers
            // `new T()` to the per-instantiation SPECIALIZED struct (recordStruct
            // under the active substitution) — a return-type mismatch (IR_FAIL).
            // Re-derive the return type exactly as the body does. Restricted to a
            // bare non-nullable param: a nullable `T?` return goes through the
            // tagged `{i8; T}` struct (lowerTypeRef is already correct, #568), and
            // a direct record return (rt has a declaration) is already specialized
            // by lowerTypeRef.
            if (rt->kind == sema::TypeRef::Kind::Named && !rt->declaration &&
                !rt->nullable) {
                const sema::TypeRef* resolved = resolveTypeParamRef(rt);
                if (resolved && resolved->kind == sema::TypeRef::Kind::Named &&
                    resolved->declaration &&
                    resolved->declaration->kind == ast::TypeDeclKind::Record)
                    // #1076: lower the CONCRETE resolved record instantiation
                    // under ITS OWN type args (lowerTypeRef sets up the per-arg
                    // substitution), NOT a bare `recordStruct(decl)` under the
                    // METHOD's active substitution. When the method's type params
                    // and the record's type params share names (`Select<A,B>: A`
                    // with `A = Pair<int,int>` where `Pair<A,B>` reuses A/B),
                    // recordStruct(Pair) under {A→Pair<int,int>, B→int} wrongly
                    // re-specialized Pair's fields → `Pair<Pair<int,int>,int>`
                    // ($spec1), mismatching the body's `ret $spec0` (IR_FAIL).
                    // lowerTypeRef(resolved) builds the canonical Pair<int,int>
                    // ($spec0) that the body returns. A non-generic record
                    // resolves identically to the old recordStruct(decl) path.
                    retTy = lowerTypeRef(resolved);
            }
        }
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
    // fix #1483 / §28.3: registerUnwindOwnedParam (and the sibling unwind-local
    // registration sites) gate on `currentFn_ == unwindIneligibleFn_`, consulting
    // unwindIneligible_ for the symbols the walk proved are moved/disposed/
    // reassigned (so registration should SKIP them). Both are per-function state
    // set by the outer caller's own body lowering; save/restore them around this
    // nested specialization build exactly like paramSlots_/disposeStack_ above,
    // so a specialization built mid-emission of an outer body (or another
    // specialization) doesn't clobber the outer's ineligible-set once emission
    // returns to it.
    auto savedUnwindIneligible = unwindIneligible_;
    llvm::Function* savedUnwindIneligibleFn = unwindIneligibleFn_;
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
    // #817: a specialization is the LIVE call target with the substitution
    // active, so its layouts match its signature — the template-only return
    // backstop must be OFF here (it may be ON because this specialization is
    // being built lazily while a generic TEMPLATE body is mid-emission).
    bool savedGenTemplate = emittingGenericTemplateBody_;
    emittingGenericTemplateBody_ = false;
    // #1060 / #1061: a lazily-built specialization of an async / iterator method
    // emits its OWN state machine via the emit*Method helpers below, which
    // overwrite the shared state-machine context (smStateStruct_, smSelf_, the
    // spill maps, the async result type, …). When this specialization is built
    // mid-emission of ANOTHER state-machine method's body — e.g. `await
    // Echo<int>()` lowered inside an async `Run` — that clobbers the caller's
    // live frame: emitAwaitSuspend would then GEP a null smStateStruct_/smSelf_
    // and crash. Save and restore the full SM context (exactly as the
    // locals/params/fn context above is saved) so building a specialization is
    // transparent to the caller's state machine. Containers are moved out, which
    // both saves them O(1) AND hands the helper a clean (empty) slate.
    SuspendKind savedSmKind = smKind_;
    llvm::StructType* savedSmStateStruct = smStateStruct_;
    llvm::Value* savedSmSelf = smSelf_;
    llvm::Value* savedSmStateGEP = smStateGEP_;
    llvm::BasicBlock* savedSmDoneBB = smDoneBB_;
    unsigned savedSmNextState = smNextSuspendState_;
    unsigned savedIterCurValField = iterCurrentValField_;
    llvm::Type* savedIterElemTy = iterElemTy_;
    unsigned savedAsyncOuterField = asyncOuterTaskField_;
    unsigned savedAsyncPendingField = asyncPendingTaskField_;
    unsigned savedAsyncSpillField = asyncSpillPtrField_;
    llvm::Function* savedAsyncResumeFn = asyncResumeFn_;
    llvm::Type* savedAsyncResultTy = asyncResultTy_;
    const sema::TypeRef* savedAsyncResultSemaTy = asyncResultSemaType_;
    // §30.5 / #1503: save/restore alongside its asyncResultTy_/
    // asyncResultSemaType_ siblings — emitAsyncMethod (called below for a
    // specialized async method) both sets and resets this field itself, but
    // this wrapper may run mid-emission of an OUTER async state machine, so
    // the outer's value must survive the nested specialization untouched.
    llvm::Function* savedAsyncPatternAResultDisposer =
        asyncPatternAResultDisposer_;
    // §30.1 (#1521): same nested-emission hazard as asyncPatternAResultDisposer_
    // above — emitAsyncMethod (called below for a specialized plain async method)
    // both publishes and clears this field itself around its OWN emission, but a
    // specialization built mid-emission of an OUTER async method's Resume body
    // must not lose the outer's frame pointer across the nested call, nor must
    // the nested emission see the OUTER frame while smStateStruct_/smSelf_ are
    // reset to the specialization's own (different) frame — reset to null below
    // alongside the other SM scalars.
    const StateMachineFrame* savedAsyncOwnedParamFrame = asyncOwnedParamFrame_;
    auto savedSmStateBBs = std::move(smStateBBs_);
    auto savedSmLocalFields = std::move(smLocalFields_);
    auto savedSmForInSrcFields = std::move(smForInSrcFields_);
    auto savedSmAllocaToField = std::move(smAllocaToField_);
    auto savedSmIterEnumFrames = std::move(smIterEnumFramesAtState_);
    // fix #1567: preserve the outer iterator's unified establishment-ordered
    // cleanup snapshot across a nested SM emission (parallel to the enum-frame
    // save above).
    auto savedSmIterCleanup = std::move(smIterCleanupAtState_);
    auto savedAsyncHandlerSpill = std::move(asyncHandlerSpillSlots_);
    // #1062: RESET the SM scalars to their fresh top-level values before the
    // helper runs — saving alone is not enough. emitResumeDispatch
    // (LlvmBackend_Iterators.cpp) runs its `emitDone` callback (→ emitAsyncComplete)
    // BEFORE it publishes smStateStruct_/smSelf_, so emitDone reads whatever they
    // were on entry. At top level they are null (cleared after the previous SM
    // method), so the fall-off emitAsyncComplete is a guarded no-op. For a spec
    // built mid-emission of an OUTER state machine they would otherwise still hold
    // the outer frame (e.g. %async.Run / the outer resume's self arg), so the
    // spec's own mn.done would GEP the outer frame and free the outer's arg — a
    // cross-function reference ("Referring to an argument in another function").
    // Clearing them makes the lazy spec emission start exactly like a top-level
    // method; the helper republishes the spec's own frame. (The containers above
    // are already emptied by the std::move.)
    smKind_ = SuspendKind::Yield;
    smStateStruct_ = nullptr;
    smSelf_ = nullptr;
    smStateGEP_ = nullptr;
    smDoneBB_ = nullptr;
    smNextSuspendState_ = 0;
    iterCurrentValField_ = 0;
    iterElemTy_ = nullptr;
    asyncOuterTaskField_ = 0;
    asyncPendingTaskField_ = 0;
    asyncSpillPtrField_ = 0;
    asyncResumeFn_ = nullptr;
    asyncResultTy_ = nullptr;
    asyncResultSemaType_ = nullptr;
    asyncPatternAResultDisposer_ = nullptr;
    asyncOwnedParamFrame_ = nullptr;  // §30.1 (#1521); emitAsyncMethod republishes it

    currentFn_ = specFn;
    // #1060 / #1061: a STATE-MACHINE method (async / iterator) must have its
    // state-machine TRANSFORMATION applied to the specialized function — the
    // plain body emission below would emit a raw `ret <value>` / yield body,
    // which IR-fails (async: `ret i32` in a `Task`-returning function) or
    // crashes at consumption (iterator: a yield body with no state machine).
    // Route to the same emit*Method helpers the non-specialized method emission
    // uses (LlvmBackend_Setup.cpp ~L3717); they create their OWN entry block and
    // bind params, and read the active genericSubst_ to specialize the frame, so
    // the specialized state machine matches the specialized signature.
    bool emittedStateMachine = false;
    if (ci.bodyKind == ast::MethodBodyKind::Block && ci.stmts) {
        if (isAsyncMethod(ms) &&
            (containsYield(*ci.stmts) || returnsAsyncIteratorType(ms))) {
            emitAsyncIteratorMethod(ms, specFn, ci);
            emittedStateMachine = true;
        } else if (!isAsyncMethod(ms) && containsYield(*ci.stmts)) {
            emitIteratorMethod(ms, specFn, ci);
            emittedStateMachine = true;
        } else if (isAsyncMethod(ms)) {
            emitAsyncMethod(ms, specFn, ci);
            emittedStateMachine = true;
        }
    } else if (ci.bodyKind == ast::MethodBodyKind::Expression && ci.exprBody &&
               isAsyncMethod(ms)) {
        // §30.1: an expression-bodied async method (`=> expr`) — synthesize a
        // single `return <exprBody>` block and route it through the state-machine
        // emission, exactly as the non-specialized path does. The synthesized
        // ReturnStmt BORROWS the AST-owned exprBody; release it afterward so its
        // destructor does not free the AST node.
        std::vector<std::unique_ptr<ast::Stmt>> synthBody;
        auto ret = std::make_unique<ast::ReturnStmt>();
        ret->span = ci.exprBody->span;
        ret->value.reset(const_cast<ast::Expr*>(ci.exprBody));
        synthBody.push_back(std::move(ret));
        CallableInfo aci = ci;
        aci.bodyKind = ast::MethodBodyKind::Block;
        aci.stmts = &synthBody;
        aci.exprBody = nullptr;
        if (returnsAsyncIteratorType(ms))
            emitAsyncIteratorMethod(ms, specFn, aci);
        else
            emitAsyncMethod(ms, specFn, aci);
        if (auto* rs = dynamic_cast<ast::ReturnStmt*>(synthBody[0].get()))
            (void)rs->value.release();
        emittedStateMachine = true;
    }

    if (!emittedStateMachine) {
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

    // fix #1483 / §28.3: populate unwindIneligible_ for THIS specialized
    // function, mirroring the non-specialized method-body prolog
    // (LlvmBackend_Setup.cpp ~line 2911-2965: `unwindIneligible_.clear();
    // unwindIneligibleFn_ = fn; collectUnwindIneligible(*ci.stmts);`).
    // registerUnwindOwnedParam refuses to register anything unless
    // `currentFn_ == unwindIneligibleFn_`; specFn is a freshly-created
    // llvm::Function, so without this the gate silently rejected owned-param
    // unwind registration for every generic specialization (the walk itself is
    // symbol-identity based — ms->params/locals are the same AST/sema symbols
    // across every instantiation — so it needs no genericSubst_ awareness).
    unwindIneligibleFn_ = specFn;
    if (ci.bodyKind == ast::MethodBodyKind::Block && ci.stmts)
        collectUnwindIneligible(*ci.stmts);
    else
        unwindIneligible_.clear();

    // Bind `this` and params. fix #1483 / §28.3: route through bindParameters
    // (the same helper the non-specialized method path uses, LlvmBackend_Setup.cpp
    // ~line 3000) instead of a manual loop. bindParameters is the ONLY code path
    // that populates pendingOwnedParams_ — the manual loop this replaces bound
    // paramSlots_/refParams_ directly and never touched pendingOwnedParams_, so
    // lowerScopedBlock's root-scope registration (line ~11725) saw an empty list
    // and never called registerUnwindOwnedParam for a specialized method body. An
    // `owned t: T` parameter of a generic method (T erased/substituted) was
    // therefore never auto-dropped on normal exit or disposed on exception
    // unwind. currentThis_ mirrors the manual loop's static-method reset;
    // bindParameters sets it from arg 0 when hasThis is true.
    currentThis_ = nullptr;
    bindParameters(specFn, /*hasThis=*/!isStaticMethod(ms), &ms->params);

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
            // §29.2 / §4.3 (#1310): wrap an array/user-class result returned as
            // IEnumerable<T> into the vtable-backed enumerator (raw array → the
            // caller's for-in / iterEnumDispose SIGSEGVs). Mirrors the other
            // return-coercion sites.
            if (rv && ci.returnType)
                rv = wrapAsIEnumerableForTarget(
                    rv, ctx_.resolvedType(*ci.returnType), ci.exprBody);
            if (!emitReturn(specFn, rv)) emitImplicitReturn(specFn);
        }
    } else {
        bool terminated = ci.stmts && lowerScopedBlock(*ci.stmts);
        if (!terminated) emitImplicitReturn(specFn);
    }
    }  // !emittedStateMachine

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
    unwindIneligible_ = std::move(savedUnwindIneligible);  // fix #1483
    unwindIneligibleFn_ = savedUnwindIneligibleFn;          // fix #1483
    currentReturnTypeNode_ = savedRetNode;
    currentSpecMethodSymbol_ = savedSpecMs;  // fix #628
    emittingGenericTemplateBody_ = savedGenTemplate;  // #817
    // Restore the caller's state-machine context (#1060 / #1061).
    smKind_ = savedSmKind;
    smStateStruct_ = savedSmStateStruct;
    smSelf_ = savedSmSelf;
    smStateGEP_ = savedSmStateGEP;
    smDoneBB_ = savedSmDoneBB;
    smNextSuspendState_ = savedSmNextState;
    iterCurrentValField_ = savedIterCurValField;
    iterElemTy_ = savedIterElemTy;
    asyncOuterTaskField_ = savedAsyncOuterField;
    asyncPendingTaskField_ = savedAsyncPendingField;
    asyncSpillPtrField_ = savedAsyncSpillField;
    asyncResumeFn_ = savedAsyncResumeFn;
    asyncResultTy_ = savedAsyncResultTy;
    asyncResultSemaType_ = savedAsyncResultSemaTy;
    asyncPatternAResultDisposer_ = savedAsyncPatternAResultDisposer;
    asyncOwnedParamFrame_ = savedAsyncOwnedParamFrame;  // §30.1 (#1521)
    smStateBBs_ = std::move(savedSmStateBBs);
    smLocalFields_ = std::move(savedSmLocalFields);
    smForInSrcFields_ = std::move(savedSmForInSrcFields);
    smAllocaToField_ = std::move(savedSmAllocaToField);
    smIterEnumFramesAtState_ = std::move(savedSmIterEnumFrames);
    smIterCleanupAtState_ = std::move(savedSmIterCleanup);  // fix #1567
    asyncHandlerSpillSlots_ = std::move(savedAsyncHandlerSpill);

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
    // #1059: do NOT bail when `ms->decl` is an OperatorDecl rather than a
    // MethodDecl — an operator overload on a GENERIC record/class must
    // specialize per-instantiation just like a regular method. callableInfo
    // already accepts both decl kinds and fills `ci` from the right one; the
    // body emission below uses `ci`, not the decl, so an operator inlines
    // correctly. (Previously `auto* md = dynamic_cast<MethodDecl*>(ms->decl);
    // if (!md) return nullptr;` excluded operators, so they fell through to the
    // unspecialized compiled function and the specialized call args mismatched
    // its parameter types — IR_FAIL.)

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
    // §28.11 / §28.2 / #889: `local?.Dispose()` — the canonical null-safe
    // discharge of a conservatively-Owned local (e.g. an `out owned` item that is
    // null on a Try* false path). `Dispose()` is compiler-synthesized and has NO
    // MethodSymbol (it is rejected as a user member by §28.2), so the generic
    // dispatch below (which needs `ms`) cannot lower it — the call would silently
    // emit nothing and the element would leak. Handle it like the regular
    // `obj.Dispose()` path: guard on a non-null receiver, then dynamic-dispatch
    // the destructor; when the receiver slot is one this scope OWNS (an out-owned
    // recipient, a `using var`, …) RECLAIM the block and null the slot so a
    // repeated `?.Dispose()` is a clean no-op (mirrors lowerCall's Dispose arm).
    if (!ms && nm.member == "Dispose" && call.args.empty() && nm.target) {
        const sema::TypeRef* recvTy0 = ctx_.exprType(*nm.target);
        bool recvIsClassOrIface =
            recvTy0 && recvTy0->kind == sema::TypeRef::Kind::Named &&
            recvTy0->declaration &&
            (recvTy0->declaration->kind == ast::TypeDeclKind::Class ||
             recvTy0->declaration->kind == ast::TypeDeclKind::Interface);
        // #991 / §28.5 / §14.2: the receiver may be a bare generic type
        // PARAMETER (`_v?.Dispose()` where `_v: T?` / `T`, T constrained
        // `where T: class`).  A type-parameter ref has a null `declaration`
        // and a single-part Named name, so the class/interface test above
        // misses it and `?.Dispose()` silently emitted nothing — the field's
        // referent leaked (its destructor never ran).  A generic class's
        // method body is lowered ONCE, type-erased (no active substitution),
        // so resolve the parameter to its concrete type when a substitution
        // is active, else accept the bare parameter: the field's runtime value
        // is a class pointer regardless, the `?.` null-guard is the dispose's
        // own non-null guard, and the `recvD` pointer-type check below rejects
        // any value-typed nullable receiver (which lowers to a struct, never a
        // pointer) — so this never mis-dispatches Dispose on a non-reference.
        bool recvIsTypeParam =
            !recvIsClassOrIface && recvTy0 &&
            recvTy0->kind == sema::TypeRef::Kind::Named &&
            recvTy0->declaration == nullptr && recvTy0->astNode &&
            recvTy0->astNode->name &&
            recvTy0->astNode->name->parts.size() == 1;
        if (recvIsTypeParam) {
            // When a substitution is active, only treat it as disposable if the
            // concrete bound type is a class/interface (a value-typed binding
            // must not dispatch a vtable Dispose); without a substitution the
            // pointer-type guard below is the safety net.
            const sema::TypeRef* concr = resolveTypeParamRef(recvTy0);
            recvIsClassOrIface =
                (concr == recvTy0)   // unresolved: accept here (pointer-guarded)
                    ? true
                    : (concr && concr->kind == sema::TypeRef::Kind::Named &&
                       concr->declaration &&
                       (concr->declaration->kind == ast::TypeDeclKind::Class ||
                        concr->declaration->kind ==
                            ast::TypeDeclKind::Interface));
        }
        if (recvIsClassOrIface) {
            llvm::Value* recvD = lowerExpr(*nm.target);
            if (!recvD || !recvD->getType()->isPointerTy()) return nullptr;
            // The `?.` null-guard is the dispose's own non-null guard: a null
            // receiver simply skips (no-op), matching the §7.5 short-circuit.
            auto* doBB = llvm::BasicBlock::Create(llvm_, "ncdisp.do", currentFn_);
            auto* endBB =
                llvm::BasicBlock::Create(llvm_, "ncdisp.end", currentFn_);
            builder_.CreateCondBr(
                builder_.CreateICmpNE(
                    recvD, llvm::ConstantPointerNull::get(ptrTy())),
                doBB, endBB);
            builder_.SetInsertPoint(doBB);
            llvm::Type* slotTy = nullptr;
            llvm::Value* slot =
                explicitDisposeOwnedReceiverSlot(*nm.target, slotTy);
            if (slot) {
                emitDisposeAndReclaim(recvD);
                if (!checkedDisposal_)
                    builder_.CreateStore(
                        llvm::ConstantPointerNull::get(ptrTy()), slot);
            } else {
                emitDynamicDispose(recvD);
            }
            builder_.CreateBr(endBB);
            builder_.SetInsertPoint(endBB);
        }
        return nullptr;  // Dispose() is void — result discarded.
    }
    // §7.5 / #1075: an inherited Object value-semantics method (ToString /
    // GetHashCode / Equals) called via `?.` on a NULLABLE value-type/enum/record
    // receiver has NO MethodSymbol, so the ms-driven path below bails and the T?
    // result is zero-initialised (= null) — and a `?.ToString()` result with a
    // non-pointer IR type crashed a downstream string compare. Lower it as
    // `hasValue ? wrap(method(inner)) : null`.
    if (!ms && nm.target) {
        bool isToString    = nm.member == "ToString"    && call.args.empty();
        bool isGetHashCode = nm.member == "GetHashCode"  && call.args.empty();
        bool isEquals      = nm.member == "Equals"       && call.args.size() == 1;
        if (isToString || isGetHashCode || isEquals) {
            const sema::TypeRef* recvTy0 = ctx_.exprType(*nm.target);
            if (recvTy0 && isNullableValueTy(recvTy0)) {
                llvm::Value* recv = lowerExpr(*nm.target);
                if (recv && recv->getType()->isStructTy())
                    return lowerNullCondValueSemCall(call, nm, recvTy0, recv,
                                                     isToString, isGetHashCode,
                                                     isEquals);
            }
        }
    }
    if (!ms || isStaticMethod(ms) || !nm.target) return nullptr;
    llvm::FunctionType* fnTy = methodFnType(ms);
    if (!fnTy) return nullptr;
    llvm::Type* retTy = fnTy->getReturnType();
    // §14.1/§7.5 (issue #1278): for a GENERIC method reached via `?.`
    // (`recv?.Method<T>(...)`), the unspecialized methodFnType return is the
    // i32 type-parameter placeholder — so the ref-vs-value result-shape
    // decision (and the {i8,T} wrap width) below would be computed from `i32`
    // even though the specialized call actually returns, e.g., a `ptr` for a
    // reference T.  Re-derive `retTy` by lowering the return type under the
    // call's method-generic substitution (methodFnType uses lowerType, which
    // honours genericSubst_).  Localized save/restore — the real call below
    // re-installs the same substitution around emitMethodCall.
    if (auto* md0 = dynamic_cast<const ast::MethodDecl*>(ms->decl)) {
        if (!md0->genericParams.empty()) {
            auto savedShapeSubst = genericSubst_;
            auto savedShapeSema = genericSubstSemaTy_;
            applyMethodGenericSubst(call, ms);
            if (auto* specFnTy = methodFnType(ms))
                retTy = specFnTy->getReturnType();
            genericSubst_ = std::move(savedShapeSubst);
            genericSubstSemaTy_ = std::move(savedShapeSema);
        }
    }
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
    // per §14.1/§14.2 + §7.5 (issue #1278): install the method-level generic
    // substitution BEFORE calling emitMethodCall, exactly as lowerCall does
    // for the `.` form — otherwise a generic method reached only via `?.`
    // (with or without an explicit `<T>`) hits emitMethodCall with an empty
    // genericSubst_ and calls the unspecialized i32-placeholder body,
    // producing malformed IR (`Call parameter type does not match function
    // signature`) for a reference-typed T. Save/restore mirrors lowerCall's
    // discipline so this call's substitution never leaks to sibling code.
    auto savedNcSubst = genericSubst_;
    auto savedNcSemaTy = genericSubstSemaTy_;
    applyMethodGenericSubst(call, ms);
    llvm::Value* r = emitMethodCall(ms, args, /*baseQualified=*/false, &argExprs);
    genericSubst_ = std::move(savedNcSubst);
    genericSubstSemaTy_ = std::move(savedNcSemaTy);
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


// §7.5 / #1075: lower `valueRecv?.ToString()/GetHashCode()/Equals(x)` on a
// nullable value-type/enum/record receiver. `recv` is the receiver's nullable
// {i8 hasValue; T} struct; the result is the §7.5-lifted nullable form of the
// method's return: `string?` (a plain ptr, null when the receiver is null) for
// ToString, `int?` for GetHashCode, `bool?` for Equals.
llvm::Value* Emitter::lowerNullCondValueSemCall(
        const ast::CallExpr& call, const ast::NullCondMemberExpr& nm,
        const sema::TypeRef* recvTy, llvm::Value* recv, bool isToString,
        bool isGetHashCode, bool isEquals) {
    (void)isGetHashCode; (void)isEquals;
    // The non-nullable inner type drives emitValueToString / the dispatch core.
    sema::TypeRef innerTy = *recvTy;
    innerTy.nullable = false;

    auto* entryBB = builder_.GetInsertBlock();
    auto* callBB = llvm::BasicBlock::Create(llvm_, "ncvs.do", currentFn_);
    auto* endBB  = llvm::BasicBlock::Create(llvm_, "ncvs.end", currentFn_);
    llvm::Value* hasVal = nullablePrimHasValue(recv);  // i1
    builder_.CreateCondBr(hasVal, callBB, endBB);

    builder_.SetInsertPoint(callBB);
    llvm::Value* innerVal = nullablePrimValue(recv);   // the T in {i8,T}
    // ToString routes through emitValueToString (handles primitive/enum/record),
    // which the dispatch core deliberately leaves to the dedicated string path;
    // GetHashCode/Equals go through the factored value-semantics core.
    llvm::Value* res = isToString
        ? emitValueToString(innerVal, &innerTy)
        : lowerValueSemMethodOn(&innerTy, innerVal, call, nm.member);
    if (!res) return nullptr;
    bool refResult = res->getType()->isPointerTy();   // ToString → string ptr
    llvm::Type* wrapTy = refResult
        ? static_cast<llvm::Type*>(ptrTy())
        : static_cast<llvm::Type*>(llvm::StructType::get(
              llvm_, {nullablePrimHasValTy(), res->getType()}));
    llvm::Value* callVal = res;
    if (!refResult) {
        callVal = builder_.CreateInsertValue(
            llvm::UndefValue::get(wrapTy),
            llvm::ConstantInt::get(nullablePrimHasValTy(), 1), {0u});
        callVal = builder_.CreateInsertValue(callVal, res, {1u});
    }
    auto* callEnd = builder_.GetInsertBlock();
    builder_.CreateBr(endBB);

    builder_.SetInsertPoint(endBB);
    auto* phi = builder_.CreatePHI(wrapTy, 2);
    phi->addIncoming(refResult ? static_cast<llvm::Value*>(
                                     llvm::ConstantPointerNull::get(ptrTy()))
                               : static_cast<llvm::Value*>(
                                     llvm::ConstantAggregateZero::get(wrapTy)),
                     entryBB);
    phi->addIncoming(callVal, callEnd);
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
        // #735 / §10.3 (#740): an ENUM instance method's `this` is the underlying
        // integer by value (default i32), matching the exporter (CG30) — not a ptr.
        if (!isStaticMethod(const_cast<sema::MethodSymbol*>(ms))) {
            bool isEnumMethod =
                ms->parent && ms->parent->kind == ast::TypeDeclKind::Enum;
            paramTys.push_back(
                isEnumMethod ? enumLlvmType(ms->parent) : ptrTy());
        }
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



}  // namespace bezel::codegen
