#include "codegen/LlvmBackend_impl.hpp"

namespace bezel::codegen {

llvm::Value* Emitter::loadCodePoint(llvm::Value* strPtr, llvm::Value* idx) {
    auto* i8Ty  = llvm::Type::getInt8Ty(llvm_);
    auto* i16Ty = llvm::Type::getInt16Ty(llvm_);
    llvm::Value* data = loadStringData(strPtr);
    llvm::Value* w = builder_.CreateZExt(loadStringWidth(strPtr), i32(), "cp.w");
    llvm::Value* idx64 = idx->getType() == i64() ? idx
                         : builder_.CreateSExt(idx, i64(), "cp.idx64");
    // Width=1: data[idx]
    llvm::Value* ptr1 = builder_.CreateGEP(i8Ty, data, idx64, "cp.p1");
    llvm::Value* val1 = builder_.CreateZExt(
        builder_.CreateLoad(i8Ty, ptr1, "cp.v1"), i32());
    // Width=2: ((i16*)data)[idx] → byte offset = idx*2
    llvm::Value* off2 = builder_.CreateMul(idx64, builder_.getInt64(2), "cp.o2");
    llvm::Value* ptr2 = builder_.CreateGEP(i8Ty, data, off2, "cp.p2");
    llvm::Value* val2 = builder_.CreateZExt(
        builder_.CreateLoad(i16Ty, ptr2, "cp.v2"), i32());
    // Width=4: ((i32*)data)[idx] → byte offset = idx*4
    llvm::Value* off4 = builder_.CreateMul(idx64, builder_.getInt64(4), "cp.o4");
    llvm::Value* ptr4 = builder_.CreateGEP(i8Ty, data, off4, "cp.p4");
    llvm::Value* val4 = builder_.CreateLoad(i32(), ptr4, "cp.v4");
    // Select on width: if w==4 → val4, elif w==2 → val2, else → val1
    llvm::Value* is4 = builder_.CreateICmpEQ(w, builder_.getInt32(4), "cp.is4");
    llvm::Value* is2 = builder_.CreateICmpEQ(w, builder_.getInt32(2), "cp.is2");
    return builder_.CreateSelect(is4, val4,
           builder_.CreateSelect(is2, val2, val1), "cp.result");
}

// §3.4 / §4.6 / issue #474: `s[range]` — a Range index over a string — returns
// the substring spanning the range as a FRESH, runtime-managed `string`
// (rc := 1). Bound semantics follow §16.1: `a..b` is inclusive on both ends,
// `a..<b` excludes the upper bound, and the open-ended forms (`..b`, `a..`,
// `..`) extend to the start/end of the string. Out-of-range endpoints raise
// IndexOutOfRangeException; an empty range produces the empty string. The
// result preserves the source's per-code-point width (§4.6), copying
// `width * length` bytes so multi-byte strings slice correctly.
llvm::Value* Emitter::emitStringRangeSlice(llvm::Value* self,
                                           const ast::RangeExpr& range) {
    llvm::Value* selfLen = loadStringLen(self);                          // i64

    // start64: the lower endpoint (0 when open on the low side).
    llvm::Value* start64 = builder_.getInt64(0);
    if (range.lo) {
        llvm::Value* lo = lowerExpr(*range.lo);
        if (!lo) return nullptr;
        start64 = lo->getType() == i64()
                      ? lo
                      : builder_.CreateSExt(lo, i64(), "slc.start");
    }

    // endExcl: the EXCLUSIVE upper index into the code-point sequence.
    // A present inclusive upper bound `b` covers index b → endExcl = b + 1;
    // a half-open upper bound `<b` excludes b → endExcl = b; an absent upper
    // bound runs to the end of the string → endExcl = selfLen.
    llvm::Value* endExcl = selfLen;
    if (range.hi) {
        llvm::Value* hi = lowerExpr(*range.hi);
        if (!hi) return nullptr;
        llvm::Value* hi64 = hi->getType() == i64()
                                ? hi
                                : builder_.CreateSExt(hi, i64(), "slc.hi");
        endExcl = range.inclusive
                      ? builder_.CreateAdd(hi64, builder_.getInt64(1),
                                           "slc.endexcl")
                      : hi64;
    }
    return emitStringSliceBytes(self, start64, endExcl);
}

// fix R33 / §3.4: slice a string by a Range *value* (a `%bz.range` struct held
// in a variable), e.g. `var r := 0..2; s[r]`. The range-literal form `s[0..2]`
// is intercepted as a RangeExpr (emitStringRangeSlice above); a Range-typed
// operand instead lowers to the struct { i64 start, i64 end, i64 step, i8 flags }
// and previously fell through to the single-char path, which sext'd the whole
// struct → "SExt only operates on integer" IR-verify failure. Extract the
// endpoints and the open/inclusive flags (bit 0 = inclusive, bit 1 = startOpen,
// bit 2 = endOpen — matching the RangeExpr lowering) and reuse the shared slice.
llvm::Value* Emitter::emitStringRangeSliceValue(llvm::Value* self,
                                                llvm::Value* rangeVal) {
    auto* i8Ty = llvm::Type::getInt8Ty(llvm_);
    llvm::Value* selfLen = loadStringLen(self);
    llvm::Value* start = builder_.CreateExtractValue(rangeVal, {0u}, "slc.vstart");
    llvm::Value* end   = builder_.CreateExtractValue(rangeVal, {1u}, "slc.vend");
    llvm::Value* flags = builder_.CreateExtractValue(rangeVal, {3u}, "slc.vflags");

    auto bitSet = [&](uint8_t mask, const char* nm) -> llvm::Value* {
        return builder_.CreateICmpNE(
            builder_.CreateAnd(flags, llvm::ConstantInt::get(i8Ty, mask)),
            llvm::ConstantInt::get(i8Ty, 0), nm);
    };
    llvm::Value* incl      = bitSet(1u, "slc.vincl");
    llvm::Value* startOpen = bitSet(2u, "slc.vstartopen");
    llvm::Value* endOpen   = bitSet(4u, "slc.vendopen");

    // start64 = startOpen ? 0 : start (the struct already stores 0 when open,
    // but select defensively so a hand-built range value still behaves).
    llvm::Value* start64 = builder_.CreateSelect(
        startOpen, builder_.getInt64(0), start, "slc.vstart64");
    // endExcl = endOpen ? selfLen : (incl ? end+1 : end).
    llvm::Value* endPlus = builder_.CreateAdd(end, builder_.getInt64(1),
                                              "slc.vendincl");
    llvm::Value* endClosed = builder_.CreateSelect(incl, endPlus, end,
                                                   "slc.vendexcl");
    llvm::Value* endExcl = builder_.CreateSelect(endOpen, selfLen, endClosed,
                                                 "slc.vend2");
    return emitStringSliceBytes(self, start64, endExcl);
}

// Shared slice mechanics (§3.4): copy the code-point range [start64, endExcl)
// out of `self` into a fresh runtime-managed string. Both the RangeExpr form
// and the Range-value form funnel here once they have computed the endpoints.
llvm::Value* Emitter::emitStringSliceBytes(llvm::Value* self,
                                           llvm::Value* start64,
                                           llvm::Value* endExcl) {
    auto* i8Ty = llvm::Type::getInt8Ty(llvm_);
    llvm::Value* selfLen = loadStringLen(self);                          // i64
    llvm::Value* width   = builder_.CreateZExt(loadStringWidth(self),    // i64
                                               i64(), "slc.w");

    // §3.4 bounds checks (out-of-range endpoints raise IndexOutOfRangeException):
    //   * a negative lower endpoint is invalid;
    //   * a lower endpoint past the string length is invalid (start == len is
    //     the empty tail, which is admitted);
    //   * an upper endpoint past the string length is invalid (endExcl > len);
    //     for an inclusive `..b` this rejects b >= len, for a half-open `..<b`
    //     it rejects b > len while admitting b == len.
    // An empty range (endExcl <= start) is NOT an error — it yields "".
    llvm::Value* startNeg = builder_.CreateICmpSLT(start64, builder_.getInt64(0),
                                                   "slc.startneg");
    llvm::Value* startBig = builder_.CreateICmpSGT(start64, selfLen,
                                                   "slc.startbig");
    llvm::Value* endBig   = builder_.CreateICmpSGT(endExcl, selfLen,
                                                   "slc.endbig");
    llvm::Value* oor = builder_.CreateOr(startNeg,
                       builder_.CreateOr(startBig, endBig), "slc.oor");
    emitGuardThrow(oor, "IndexOutOfRangeException");

    // subLen = max(endExcl - start, 0). After the guard, both endpoints are in
    // [0, len], so a non-positive difference means an empty range.
    llvm::Value* rawLen = builder_.CreateSub(endExcl, start64, "slc.rawlen");
    llvm::Value* isEmpty = builder_.CreateICmpSLE(rawLen, builder_.getInt64(0),
                                                  "slc.empty");
    llvm::Value* subLen = builder_.CreateSelect(isEmpty, builder_.getInt64(0),
                                                rawLen, "slc.len");

    // Byte sizes (width-aware): copy `subLen * width` bytes from the source data
    // at byte offset `start * width`. Allocate `bytes + width` so there is room
    // for a width-sized zero terminator (the existing string builders NUL-pad).
    llvm::Value* byteLen   = builder_.CreateMul(subLen, width, "slc.bytelen");
    llvm::Value* srcOffset = builder_.CreateMul(start64, width, "slc.srcoff");

    auto* mallocFn = declareC("malloc", ptrTy(), {i64()});
    auto* memcpyFn = declareC("memcpy", ptrTy(), {ptrTy(), ptrTy(), i64()});
    auto* memsetFn = declareC("memset", ptrTy(), {ptrTy(), i32(), i64()});

    llvm::Value* dataSize = builder_.CreateAdd(byteLen, width, "slc.datasize");
    llvm::Value* newData  = builder_.CreateCall(mallocFn, {dataSize}, "slc.data");
    llvm::Value* srcPtr   = builder_.CreateGEP(i8Ty, loadStringData(self),
                                               srcOffset, "slc.src");
    builder_.CreateCall(memcpyFn, {newData, srcPtr, byteLen});
    // Zero a width-sized terminator so a width=2/4 result has a clean tail.
    llvm::Value* termPtr = builder_.CreateGEP(i8Ty, newData, byteLen, "slc.term");
    builder_.CreateCall(memsetFn, {termPtr, builder_.getInt32(0), width});

    uint64_t strBytes = module_->getDataLayout().getTypeAllocSize(stringType());
    llvm::Value* newStr =
        builder_.CreateCall(mallocFn, {builder_.getInt64(strBytes)}, "slc.obj");
    builder_.CreateStore(newData,
                         builder_.CreateStructGEP(stringType(), newStr, 0));
    builder_.CreateStore(subLen,
                         builder_.CreateStructGEP(stringType(), newStr, 1));
    // Preserve the source width (§4.6 per-string storage width).
    builder_.CreateStore(builder_.CreateTrunc(width, i8Ty, "slc.w8"),
                         builder_.CreateStructGEP(stringType(), newStr, 2));
    initStringRc(newStr);  // #422: producer's reference, rc := 1
    return newStr;
}


std::vector<uint32_t> Emitter::processStringCodePoints(
        std::string_view text, bool verbatim) {
    size_t start = verbatim ? 2 : 1;
    if (text.size() < (verbatim ? 3u : 2u)) return {};
    text = text.substr(start, text.size() - start - 1);

    // §4.6 / #528: decode a UTF-8 multi-byte sequence starting at text[i]
    // into a single Unicode scalar value; i is advanced past any continuation
    // bytes consumed beyond the lead byte.  Malformed lead bytes fall through
    // as single-byte Latin-1 values so valid source (§1.1: UTF-8) always works.
    auto decodeUtf8 = [](std::string_view t, size_t& i) -> uint32_t {
        auto b0 = (unsigned char)t[i];
        // 1-byte ASCII: 0xxxxxxx
        if (b0 < 0x80) return b0;
        // 2-byte sequence: 110xxxxx 10xxxxxx
        if ((b0 & 0xE0) == 0xC0 && i + 1 < t.size() &&
                ((unsigned char)t[i+1] & 0xC0) == 0x80) {
            uint32_t cp = ((b0 & 0x1F) << 6) | ((unsigned char)t[i+1] & 0x3F);
            ++i; return cp;
        }
        // 3-byte sequence: 1110xxxx 10xxxxxx 10xxxxxx
        if ((b0 & 0xF0) == 0xE0 && i + 2 < t.size() &&
                ((unsigned char)t[i+1] & 0xC0) == 0x80 &&
                ((unsigned char)t[i+2] & 0xC0) == 0x80) {
            uint32_t cp = ((b0 & 0x0F) << 12) |
                          (((unsigned char)t[i+1] & 0x3F) << 6) |
                          ((unsigned char)t[i+2] & 0x3F);
            i += 2; return cp;
        }
        // 4-byte sequence: 11110xxx 10xxxxxx 10xxxxxx 10xxxxxx
        if ((b0 & 0xF8) == 0xF0 && i + 3 < t.size() &&
                ((unsigned char)t[i+1] & 0xC0) == 0x80 &&
                ((unsigned char)t[i+2] & 0xC0) == 0x80 &&
                ((unsigned char)t[i+3] & 0xC0) == 0x80) {
            uint32_t cp = ((b0 & 0x07) << 18) |
                          (((unsigned char)t[i+1] & 0x3F) << 12) |
                          (((unsigned char)t[i+2] & 0x3F) << 6) |
                          ((unsigned char)t[i+3] & 0x3F);
            i += 3; return cp;
        }
        // Malformed lead byte: fall back to single-byte Latin-1 value.
        return b0;
    };

    std::vector<uint32_t> cps;
    cps.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (verbatim) {
            if (text[i] == '"' && i+1 < text.size() && text[i+1] == '"') {
                cps.push_back('"'); ++i;
            } else {
                // Raw chars in verbatim — decode as UTF-8 code points (§4.6 / #528).
                cps.push_back(decodeUtf8(text, i));
            }
            continue;
        }
        if (text[i] != '\\') {
            // Non-backslash: decode as UTF-8 scalar (§4.6 / #528).
            cps.push_back(decodeUtf8(text, i));
            continue;
        }
        if (++i >= text.size()) break;
        switch (text[i]) {
            case 'n': cps.push_back(0x0A); break;
            case 'r': cps.push_back(0x0D); break;
            case 't': cps.push_back(0x09); break;
            case '\\': cps.push_back('\\'); break;
            case '"': cps.push_back('"'); break;
            case '\'': cps.push_back('\''); break;
            case '0': cps.push_back(0x00); break;
            case 'a': cps.push_back(0x07); break;
            case 'b': cps.push_back(0x08); break;
            case 'f': cps.push_back(0x0C); break;
            case 'v': cps.push_back(0x0B); break;
            case 'u': {
                // §18.8: \uXXXX — 4 hex digits, BMP code point
                if (i + 4 < text.size()) {
                    uint32_t cp = 0;
                    for (int k = 0; k < 4; ++k)
                        cp = (cp << 4) | (uint8_t)
                            (text[i+1+k] >= 'a' ? text[i+1+k]-'a'+10 :
                             text[i+1+k] >= 'A' ? text[i+1+k]-'A'+10 :
                             text[i+1+k] - '0');
                    i += 4;
                    cps.push_back(cp);  // raw code point, NOT UTF-8
                }
                break;
            }
            case 'U': {
                // §18.8: \UXXXXXXXX — 8 hex digits, full Unicode
                if (i + 8 < text.size()) {
                    uint32_t cp = 0;
                    for (int k = 0; k < 8; ++k)
                        cp = (cp << 4) | (uint8_t)
                            (text[i+1+k] >= 'a' ? text[i+1+k]-'a'+10 :
                             text[i+1+k] >= 'A' ? text[i+1+k]-'A'+10 :
                             text[i+1+k] - '0');
                    i += 8;
                    cps.push_back(cp);
                }
                break;
            }
            case 'x': {
                // §18.8: \xH..H — 1-4 hex digits, greedy
                uint32_t cp = 0; size_t count = 0;
                while (count < 4 && i+1 < text.size() &&
                       std::isxdigit((unsigned char)text[i+1])) {
                    cp = (cp << 4) | (uint8_t)
                        (text[i+1] >= 'a' ? text[i+1]-'a'+10 :
                         text[i+1] >= 'A' ? text[i+1]-'A'+10 :
                         text[i+1]-'0');
                    ++i; ++count;
                }
                if (count) cps.push_back(cp);
                break;
            }
            default: cps.push_back((unsigned char)text[i]); break;
        }
    }
    return cps;
}


uint8_t Emitter::codePointsWidth(const std::vector<uint32_t>& cps) {
    uint32_t maxCp = 0;
    for (auto cp : cps) if (cp > maxCp) maxCp = cp;
    if (maxCp <= 0xFF) return 1;
    if (maxCp <= 0xFFFF) return 2;
    return 4;
}


llvm::Constant* Emitter::makeCodePointArray(const std::vector<uint32_t>& cps,
                                    uint8_t width) {
    if (width == 1) {
        std::string bytes;
        bytes.reserve(cps.size());
        for (auto cp : cps) bytes.push_back((char)(cp & 0xFF));
        return llvm::ConstantDataArray::getString(llvm_, bytes, /*AddNull=*/true);
    } else if (width == 2) {
        std::vector<uint16_t> data;
        data.reserve(cps.size() + 1);
        for (auto cp : cps) data.push_back((uint16_t)(cp & 0xFFFF));
        data.push_back(0);
        return llvm::ConstantDataArray::get(
            llvm_, llvm::ArrayRef<uint16_t>(data));
    } else {
        std::vector<uint32_t> data(cps);
        data.push_back(0);
        return llvm::ConstantDataArray::get(
            llvm_, llvm::ArrayRef<uint32_t>(data));
    }
}


std::string Emitter::processStringText(std::string_view text, bool verbatim) {
    auto cps = processStringCodePoints(text, verbatim);
    std::string result;
    result.reserve(cps.size());
    for (auto cp : cps) result.push_back((char)(cp & 0xFF));
    return result;
}


llvm::Value* Emitter::lowerStringLiteral(std::string_view text, bool verbatim) {
    auto cps = processStringCodePoints(text, verbatim);
    uint8_t width = codePointsWidth(cps);
    unsigned id = stringLiteralIdx_++;
    // Data array: [N+1 x iW] where W = width*8 bits.
    llvm::Constant* dataArr = makeCodePointArray(cps, width);
    auto* dataGV = new llvm::GlobalVariable(
        *module_, dataArr->getType(), /*isConstant=*/true,
        llvm::GlobalValue::PrivateLinkage, dataArr,
        ".bz.str.data." + std::to_string(id));
    dataGV->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    dataGV->setAlignment(llvm::Align(width));
    // %bz_string = { ptr data, i64 len, i8 width, i64 rc }
    auto* i8Ty = llvm::Type::getInt8Ty(llvm_);
    llvm::Constant* lenC = llvm::ConstantInt::get(i64(), cps.size());
    llvm::Constant* widC = llvm::ConstantInt::get(i8Ty, width);
    // #422: a string literal is a constant global, never heap-allocated — give
    // it the SENTINEL rc so bz_string_retain/release are no-ops and it is never
    // freed (§4.6 / recorded decision: statics carry a sentinel rc).
    llvm::Constant* strVal = llvm::ConstantStruct::get(
        stringType(), {dataGV, lenC, widC, stringRcSentinel()});
    auto* strGV = new llvm::GlobalVariable(
        *module_, stringType(), /*isConstant=*/true,
        llvm::GlobalValue::PrivateLinkage, strVal,
        ".bz.str." + std::to_string(id));
    return strGV;
}


llvm::Value* Emitter::emitStringEq(llvm::Value* l, llvm::Value* r) {
    // per §4.6 / §9.1 / §7.2 / §8.4: null-safe string equality.
    // `string?` lowers to a plain pointer (null = real null pointer).  We must
    // never dereference either operand without first confirming it is non-null.
    //
    // Control-flow structure (4-incoming PHI at endBB):
    //   1. samePtr   (l == r)        → true   [covers null == null and same obj]
    //   2. nullCheckBB (l==null | r==null, but l!=r) → false [null vs. non-null]
    //   3. lenBB sameLen=false       → false  [both non-null, different lengths]
    //   4. cmpBB memcmp result       → memcmp==0 [both non-null, same length]
    auto* entryBB    = builder_.GetInsertBlock();
    auto* nullCheckBB = llvm::BasicBlock::Create(llvm_, "str.eq.nullchk", currentFn_);
    auto* lenBB      = llvm::BasicBlock::Create(llvm_, "str.eq.len",     currentFn_);
    auto* cmpBB      = llvm::BasicBlock::Create(llvm_, "str.eq.cmp",     currentFn_);
    auto* endBB      = llvm::BasicBlock::Create(llvm_, "str.eq.end",     currentFn_);

    // Fast path 1: same pointer → equal (also handles null == null).
    llvm::Value* samePtr = builder_.CreateICmpEQ(l, r, "str.sameptr");
    builder_.CreateCondBr(samePtr, endBB, nullCheckBB);

    // Null guard: we know l != r here.  If either is null the other must be
    // non-null (since l != r), so they cannot be equal.
    builder_.SetInsertPoint(nullCheckBB);
    auto* null_ = llvm::ConstantPointerNull::get(ptrTy());
    llvm::Value* lNull = builder_.CreateICmpEQ(l, null_, "str.lnull");
    llvm::Value* rNull = builder_.CreateICmpEQ(r, null_, "str.rnull");
    llvm::Value* eitherNull = builder_.CreateOr(lNull, rNull, "str.eithernull");
    auto* nullCheckEnd = builder_.GetInsertBlock();
    builder_.CreateCondBr(eitherNull, endBB, lenBB);

    // Code-point-count check: must be equal for content equality.
    builder_.SetInsertPoint(lenBB);
    llvm::Value* lenL = loadStringLen(l);
    llvm::Value* lenR = loadStringLen(r);
    llvm::Value* sameLen = builder_.CreateICmpEQ(lenL, lenR, "str.samelen");
    auto* lenEnd = builder_.GetInsertBlock();
    builder_.CreateCondBr(sameLen, cmpBB, endBB);

    // Compare byte data: len * width bytes for each string.
    // For equal-width strings: memcmp(dataL, dataR, len*width) == 0.
    // For different-width strings (e.g. width=1 vs width=2): we compare
    // lenL * max(wL,wR) bytes — but this can give false positives for
    // mixed widths. Since all current strings are width=1, we always use
    // memcmp(data, data, byteSize) which is correct when wL == wR.
    builder_.SetInsertPoint(cmpBB);
    llvm::Value* dataL = loadStringData(l);
    llvm::Value* dataR = loadStringData(r);
    auto* memcmpFn = declareC("memcmp", i32(), {ptrTy(), ptrTy(), i64()});
    // Use the byte size of l (= len * widthL); correct when both widths equal.
    llvm::Value* byteSzL = loadStringByteSize(l);
    llvm::Value* cmpRes = builder_.CreateCall(memcmpFn, {dataL, dataR, byteSzL});
    llvm::Value* cmpEq = builder_.CreateICmpEQ(
        cmpRes, llvm::ConstantInt::get(i32(), 0), "str.cmpeq");
    auto* cmpEnd = builder_.GetInsertBlock();
    builder_.CreateBr(endBB);

    // Merge: 4 incoming edges.
    builder_.SetInsertPoint(endBB);
    auto* phi = builder_.CreatePHI(i1(), 4, "str.eq");
    phi->addIncoming(builder_.getTrue(),  entryBB);    // samePtr (incl. null==null)
    phi->addIncoming(builder_.getFalse(), nullCheckEnd); // one side null
    phi->addIncoming(builder_.getFalse(), lenEnd);      // different lengths
    phi->addIncoming(cmpEq,              cmpEnd);       // memcmp result
    return phi;
}


llvm::Value* Emitter::emitStringConcat(llvm::Value* l, llvm::Value* r) {
    auto* i8Ty = llvm::Type::getInt8Ty(llvm_);
    auto* mallocFn = declareC("malloc", ptrTy(), {i64()});
    auto* memcpyFn = declareC("memcpy", ptrTy(), {ptrTy(), ptrTy(), i64()});
    llvm::Value* lenL = loadStringLen(l);
    llvm::Value* lenR = loadStringLen(r);
    llvm::Value* total = builder_.CreateAdd(lenL, lenR, "str.total");
    llvm::Value* wL32 = builder_.CreateZExt(loadStringWidth(l), i32(), "wL");
    llvm::Value* wR32 = builder_.CreateZExt(loadStringWidth(r), i32(), "wR");
    llvm::Value* maxW32 = builder_.CreateSelect(
        builder_.CreateICmpUGT(wL32, wR32), wL32, wR32, "maxW");
    llvm::Value* maxW64 = builder_.CreateZExt(maxW32, i64(), "maxW64");
    llvm::Value* maxW8  = builder_.CreateTrunc(maxW32, i8Ty, "maxW8");
    // Allocate (total + 1) * maxWidth bytes for data.
    llvm::Value* dataSz = builder_.CreateMul(
        builder_.CreateAdd(total, builder_.getInt64(1)), maxW64, "str.dsz");
    llvm::Value* newData = builder_.CreateCall(mallocFn, {dataSz}, "str.data");
    llvm::Value* bszL = loadStringByteSize(l);
    llvm::Value* bszR = loadStringByteSize(r);
    llvm::Value* sameW = builder_.CreateICmpEQ(wL32, wR32, "sameW");
    auto* fastBB = llvm::BasicBlock::Create(llvm_, "cat.fast", currentFn_);
    auto* slowBB = llvm::BasicBlock::Create(llvm_, "cat.slow", currentFn_);
    auto* doneBB = llvm::BasicBlock::Create(llvm_, "cat.done", currentFn_);
    builder_.CreateCondBr(sameW, fastBB, slowBB);

    // Fast path: same width — plain memcpy for both halves.
    builder_.SetInsertPoint(fastBB);
    builder_.CreateCall(memcpyFn, {newData, loadStringData(l), bszL});
    builder_.CreateCall(memcpyFn,
        {builder_.CreateGEP(i8Ty, newData, bszL), loadStringData(r), bszR});
    builder_.CreateBr(doneBB);

    // Slow path: different widths — element-by-element up-conversion.
    // Loop over L's code points first, then R's, each stored at maxW64 bytes.
    builder_.SetInsertPoint(slowBB);

    // Loop over L: for i in [0, lenL)
    {
        auto* idxS = entryAlloca(i64());
        builder_.CreateStore(builder_.getInt64(0), idxS);
        auto* condBB = llvm::BasicBlock::Create(llvm_, "cat.Lcond", currentFn_);
        auto* bodyBB = llvm::BasicBlock::Create(llvm_, "cat.Lbody", currentFn_);
        auto* exitBB = llvm::BasicBlock::Create(llvm_, "cat.Lexit", currentFn_);
        builder_.CreateBr(condBB);

        builder_.SetInsertPoint(condBB);
        llvm::Value* si = builder_.CreateLoad(i64(), idxS, "cat.Li");
        builder_.CreateCondBr(builder_.CreateICmpSLT(si, lenL), bodyBB, exitBB);

        builder_.SetInsertPoint(bodyBB);
        llvm::Value* cp  = loadCodePoint(l, si);
        llvm::Value* off = builder_.CreateMul(si, maxW64, "cat.Loff");
        llvm::Value* dp  = builder_.CreateGEP(i8Ty, newData, off, "cat.Ldp");
        // emitStoreCodePoint terminates bodyBB and leaves builder at seBlock.
        emitStoreCodePoint(cp, dp, maxW64);
        // Builder is now at the "scp.e" merge block; finish the loop iteration.
        builder_.CreateStore(
            builder_.CreateAdd(si, builder_.getInt64(1)), idxS);
        builder_.CreateBr(condBB);

        builder_.SetInsertPoint(exitBB);
    }

    // Loop over R: for i in [0, lenR), byte offset = (lenL + i) * maxW64
    {
        auto* idxS = entryAlloca(i64());
        builder_.CreateStore(builder_.getInt64(0), idxS);
        llvm::Value* rBase = builder_.CreateMul(lenL, maxW64, "cat.rbase");
        auto* condBB = llvm::BasicBlock::Create(llvm_, "cat.Rcond", currentFn_);
        auto* bodyBB = llvm::BasicBlock::Create(llvm_, "cat.Rbody", currentFn_);
        auto* exitBB = llvm::BasicBlock::Create(llvm_, "cat.Rexit", currentFn_);
        builder_.CreateBr(condBB);

        builder_.SetInsertPoint(condBB);
        llvm::Value* si = builder_.CreateLoad(i64(), idxS, "cat.Ri");
        builder_.CreateCondBr(builder_.CreateICmpSLT(si, lenR), bodyBB, exitBB);

        builder_.SetInsertPoint(bodyBB);
        llvm::Value* cp  = loadCodePoint(r, si);
        llvm::Value* off = builder_.CreateAdd(
            rBase, builder_.CreateMul(si, maxW64), "cat.Roff");
        llvm::Value* dp  = builder_.CreateGEP(i8Ty, newData, off, "cat.Rdp");
        // emitStoreCodePoint terminates bodyBB and leaves builder at seBlock.
        emitStoreCodePoint(cp, dp, maxW64);
        // Builder is now at the "scp.e" merge block; finish the loop iteration.
        builder_.CreateStore(
            builder_.CreateAdd(si, builder_.getInt64(1)), idxS);
        builder_.CreateBr(condBB);

        builder_.SetInsertPoint(exitBB);
    }

    builder_.CreateBr(doneBB);

    // Merge point: write null terminator and build the bz_string struct.
    builder_.SetInsertPoint(doneBB);
    llvm::Value* nulOff = builder_.CreateMul(total, maxW64, "cat.nuloff");
    llvm::Value* nulPtr = builder_.CreateGEP(i8Ty, newData, nulOff, "cat.nulptr");
    auto* memsetFn = module_->getFunction("memset");
    if (!memsetFn) {
        auto* ft = llvm::FunctionType::get(ptrTy(), {ptrTy(), i32(), i64()}, false);
        memsetFn = llvm::Function::Create(
            ft, llvm::Function::ExternalLinkage, "memset", module_.get());
    }
    builder_.CreateCall(memsetFn, {nulPtr, builder_.getInt32(0), maxW64});
    // Build the bz_string object.
    uint64_t strBytes = module_->getDataLayout().getTypeAllocSize(stringType());
    llvm::Value* newStr = builder_.CreateCall(
        mallocFn, {builder_.getInt64(strBytes)}, "str.obj");
    builder_.CreateStore(newData, builder_.CreateStructGEP(stringType(), newStr, 0));
    builder_.CreateStore(total,   builder_.CreateStructGEP(stringType(), newStr, 1));
    builder_.CreateStore(maxW8,   builder_.CreateStructGEP(stringType(), newStr, 2));
    initStringRc(newStr);  // #422: producer's reference, rc := 1
    return newStr;
}


void Emitter::emitStoreCodePoint(llvm::Value* cp, llvm::Value* ptr, llvm::Value* dstW64) {
    auto* i8Ty  = llvm::Type::getInt8Ty(llvm_);
    auto* i16Ty = llvm::Type::getInt16Ty(llvm_);

    auto* s4Block   = llvm::BasicBlock::Create(llvm_, "scp.4",    currentFn_);
    auto* chk2Block = llvm::BasicBlock::Create(llvm_, "scp.chk2", currentFn_);
    auto* s2Block   = llvm::BasicBlock::Create(llvm_, "scp.2",    currentFn_);
    auto* s1Block   = llvm::BasicBlock::Create(llvm_, "scp.1",    currentFn_);
    auto* seBlock   = llvm::BasicBlock::Create(llvm_, "scp.e",    currentFn_);

    // Terminate the current block with the is4 branch.
    llvm::Value* is4 = builder_.CreateICmpEQ(dstW64, builder_.getInt64(4), "scp.is4");
    builder_.CreateCondBr(is4, s4Block, chk2Block);

    // s4Block: store full i32.
    builder_.SetInsertPoint(s4Block);
    builder_.CreateStore(cp, ptr);
    builder_.CreateBr(seBlock);

    // chk2Block: test for width 2.
    builder_.SetInsertPoint(chk2Block);
    llvm::Value* is2 = builder_.CreateICmpEQ(dstW64, builder_.getInt64(2), "scp.is2");
    builder_.CreateCondBr(is2, s2Block, s1Block);

    // s2Block: store low 16 bits.
    builder_.SetInsertPoint(s2Block);
    builder_.CreateStore(builder_.CreateTrunc(cp, i16Ty, "scp.t16"), ptr);
    builder_.CreateBr(seBlock);

    // s1Block: store low 8 bits.
    builder_.SetInsertPoint(s1Block);
    builder_.CreateStore(builder_.CreateTrunc(cp, i8Ty, "scp.t8"), ptr);
    builder_.CreateBr(seBlock);

    // Leave builder at the merge block so the caller continues there.
    builder_.SetInsertPoint(seBlock);
}


llvm::Value* Emitter::emitWithFormatSpec(llvm::Value* val,
                                const std::string& spec,
                                const sema::TypeRef* rt) {
    if (spec.empty() || !val) return nullptr;
    // §18.5 alignment specifier `,WIDTH` or `,WIDTH:FORMAT`:
    // Positive width = right-align, negative = left-align.
    // First apply FORMAT (if any) to get the value string, then pad.
    if (spec[0] == ',') {
        // Split ",WIDTH" and optional ":FORMAT".
        size_t colonPos = spec.find(':', 1);
        std::string widthPart = (colonPos == std::string::npos)
            ? spec.substr(1) : spec.substr(1, colonPos - 1);
        std::string innerFmt = (colonPos == std::string::npos)
            ? "" : spec.substr(colonPos + 1);
        int alignWidth = std::atoi(widthPart.c_str());
        bool leftAlign = alignWidth < 0;
        int w = leftAlign ? -alignWidth : alignWidth;
        if (w == 0) return nullptr;
        std::string snpFmt = leftAlign
            ? ("%-" + std::to_string(w) + "s")
            : ("%" + std::to_string(w) + "s");
        // If an inner format spec exists, apply it first.
        llvm::Value* strVal = nullptr;
        if (!innerFmt.empty()) {
            llvm::Value* formatted = emitWithFormatSpec(val, innerFmt, rt);
            if (formatted) strVal = formatted;
        }
        using P = sema::PrimitiveType;
        if (!strVal) {
            // Default string conversion.
            bool isStr = rt && rt->kind == sema::TypeRef::Kind::Primitive &&
                         rt->primitive == P::String;
            if (isStr) {
                strVal = val;
            } else if (rt && rt->kind == sema::TypeRef::Kind::Primitive) {
                strVal = emitPrimitiveToString(val, rt->primitive);
            } else if (val->getType()->isDoubleTy() || val->getType()->isFloatTy()) {
                strVal = emitPrimitiveToString(val, P::Double);
            } else if (val->getType()->isIntegerTy()) {
                llvm::Value* iv = val->getType()->isIntegerTy(64) ? val
                    : builder_.CreateSExt(val, i64());
                strVal = emitPrimitiveToString(iv, P::Long);
            }
        }
        if (!strVal || !strVal->getType()->isPointerTy()) return nullptr;
        llvm::Value* datPtr = builder_.CreateLoad(
            ptrTy(), builder_.CreateStructGEP(stringType(), strVal, 0));
        return emitSnprintfToString(snpFmt, datPtr);
    }
    char specCh = spec[0];
    char kind = std::toupper(static_cast<unsigned char>(specCh));
    int width = (spec.size() > 1) ? std::atoi(spec.c_str() + 1) : -1;
    std::string snprintfFmt;
    bool isFloat = val->getType()->isDoubleTy() ||
                   val->getType()->isFloatTy() ||
                   (rt && rt->kind == sema::TypeRef::Kind::Primitive &&
                    (rt->primitive == sema::PrimitiveType::Double ||
                     rt->primitive == sema::PrimitiveType::Float));
    bool isInt = val->getType()->isIntegerTy() ||
                 (rt && rt->kind == sema::TypeRef::Kind::Primitive &&
                  !isFloat);
    bool numeric = isFloat || isInt;
    // F/E/G/P (§18.5) format a numeric value as a real number, so they apply to
    // both floating and integer operands (an int operand is widened to double
    // first — `{42:F2}` → "42.00", issue #739/WR11). X/D apply to integers
    // only. The case of the specifier letter is preserved for E/G/X so an
    // uppercase specifier yields uppercase output (issue #739/WR10, mirroring
    // the existing X/x handling).
    bool useFloatEmit = false;
    double scale = 1.0;  // P (percent) multiplies the value by 100
    if (kind == 'F' && numeric) {
        snprintfFmt = "%." + (width >= 0 ? std::to_string(width) : "6") + "f";
        useFloatEmit = true;
    } else if (kind == 'E' && numeric) {
        char ech = (specCh == 'e') ? 'e' : 'E';
        snprintfFmt = "%." + (width >= 0 ? std::to_string(width) : "6") + ech;
        useFloatEmit = true;
    } else if (kind == 'G' && numeric) {
        char gch = (specCh == 'g') ? 'g' : 'G';
        snprintfFmt = "%." + (width >= 0 ? std::to_string(width) : "6") + gch;
        useFloatEmit = true;
    } else if (kind == 'P' && numeric) {
        // §18.5 normative example: `{ratio:P1}` → "85.0%". Scale by 100, format
        // fixed-point to the requested digit count (default 2), append a
        // literal '%' (snprintf renders "%%" as one '%'). Issue #739/WR9.
        snprintfFmt = "%." + (width >= 0 ? std::to_string(width) : "2") + "f%%";
        useFloatEmit = true;
        scale = 100.0;
    } else if (kind == 'X' && isInt) {
        // §18.5 X[n]: zero-padded hex to n digits minimum.
        char xch = (specCh == 'x') ? 'x' : 'X';
        snprintfFmt = width >= 0
            ? ("%0" + std::to_string(width) + xch)
            : (std::string("%") + xch);
    } else if (kind == 'D' && isInt) {
        // §18.5 D[n]: zero-padded decimal to n digits minimum.
        snprintfFmt = width >= 0
            ? ("%0" + std::to_string(width) + "d")
            : "%d";
    } else {
        return nullptr;  // unknown format spec
    }
    // Emit snprintf call.
    if (useFloatEmit) {
        llvm::Type* dblTy = llvm::Type::getDoubleTy(llvm_);
        llvm::Value* dv;
        if (val->getType()->isFloatTy()) {
            dv = builder_.CreateFPExt(val, dblTy);
        } else if (val->getType()->isDoubleTy()) {
            dv = val;
        } else if (val->getType()->isIntegerTy()) {
            bool uns = rt && rt->kind == sema::TypeRef::Kind::Primitive &&
                       isUnsignedPrim(rt->primitive);
            dv = uns ? builder_.CreateUIToFP(val, dblTy)
                     : builder_.CreateSIToFP(val, dblTy);
        } else {
            return nullptr;
        }
        if (scale != 1.0)
            dv = builder_.CreateFMul(dv, llvm::ConstantFP::get(dblTy, scale));
        return emitSnprintfToString(snprintfFmt, dv);
    } else {
        llvm::Value* iv = val->getType()->isIntegerTy(64) ? val
            : builder_.CreateSExt(val, i64());
        return emitSnprintfToString(snprintfFmt, iv);
    }
}


llvm::Value* Emitter::emitSnprintfToString(const std::string& fmt,
                                  llvm::Value* val) {
    auto* i8Ty = llvm::Type::getInt8Ty(llvm_);
    auto* mallocFn = declareC("malloc", ptrTy(), {i64()});
    auto* snpFn = module_->getFunction("snprintf");
    if (!snpFn) {
        auto* ft = llvm::FunctionType::get(i32(), {ptrTy(), i64(), ptrTy()}, true);
        snpFn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage,
                                       "snprintf", module_.get());
    }
    const unsigned BUF = 64;
    llvm::Value* buf = entryAlloca(llvm::ArrayType::get(i8Ty, BUF));
    llvm::Value* bufPtr = builder_.CreateGEP(
        llvm::ArrayType::get(i8Ty, BUF), buf,
        {builder_.getInt64(0), builder_.getInt64(0)});
    // Build format string global.
    auto* fmtData = llvm::ConstantDataArray::getString(llvm_, fmt, true);
    unsigned fid = stringLiteralIdx_++;
    auto* fmtGV = new llvm::GlobalVariable(
        *module_, fmtData->getType(), true,
        llvm::GlobalValue::PrivateLinkage, fmtData,
        ".bz.fmtspec." + std::to_string(fid));
    llvm::Value* wr = builder_.CreateCall(
        snpFn, {bufPtr, builder_.getInt64(BUF), fmtGV, val});
    llvm::Value* l64 = builder_.CreateSExt(wr, i64());
    // Build heap-backed bz_string.
    llvm::Value* dSz = builder_.CreateAdd(l64, builder_.getInt64(1));
    auto* memcpyFn = declareC("memcpy", ptrTy(), {ptrTy(), ptrTy(), i64()});
    llvm::Value* dc = builder_.CreateCall(mallocFn, {dSz});
    builder_.CreateCall(memcpyFn, {dc, bufPtr, l64});
    llvm::Value* np = builder_.CreateGEP(i8Ty, dc, l64);
    builder_.CreateStore(llvm::ConstantInt::get(i8Ty, 0), np);
    uint64_t strBytes = module_->getDataLayout().getTypeAllocSize(stringType());
    llvm::Value* so = builder_.CreateCall(mallocFn, {builder_.getInt64(strBytes)});
    builder_.CreateStore(dc, builder_.CreateStructGEP(stringType(), so, 0));
    builder_.CreateStore(l64, builder_.CreateStructGEP(stringType(), so, 1));
    builder_.CreateStore(llvm::ConstantInt::get(i8Ty, 1),
                         builder_.CreateStructGEP(stringType(), so, 2));
                         initStringRc(so);  // #422: producer's reference, rc := 1
    return so;
}


llvm::Value* Emitter::emitPrimitiveToString(llvm::Value* val,
                                   sema::PrimitiveType prim) {
    using P = sema::PrimitiveType;
    auto* i8Ty = llvm::Type::getInt8Ty(llvm_);
    auto* mallocFn = declareC("malloc", ptrTy(), {i64()});

    // §8.6 / #453: Object and interface are pointer-typed — they carry a vtable
    // (class/box) or are a bare bz_string*.  Dispatch through lowerObjectToString
    // (the same path used by `o.ToString()` on an Object receiver) instead of
    // falling to the default `%d` snprintf, which would print the 64-bit pointer
    // value as a truncated i32. This covers both interpolation and Console.WriteLine.
    if (prim == P::Object) {
        if (llvm::Value* r = lowerObjectToString(val)) return r;
        // Fallback (should not be reached for a well-typed Object): type name.
        return lowerStringLiteral("\"Object\"", false);
    }

    // §4.4 / fix #521: decimal.ToString() → call bz_decimal_to_string.
    if (prim == P::Decimal) {
        llvm::StructType* dst = decimalStructType();
        llvm::AllocaInst* slot = entryAlloca(dst);
        builder_.CreateStore(val, slot);
        auto* fn = declareC("bz_decimal_to_string", ptrTy(), {ptrTy()});
        return builder_.CreateCall(fn, {slot}, "dec.tostr");
    }

    // Special case: bool → "True" or "False"
    // per stdlib bool.ToString() → "True" or "False" (capital first letter).
    if (prim == P::Bool) {
        // lowerStringLiteral materializes constant globals.
        auto* trueStr  = lowerStringLiteral("\"True\"", false);
        auto* falseStr = lowerStringLiteral("\"False\"", false);
        return builder_.CreateSelect(
            builder_.CreateTrunc(val, i1()), trueStr, falseStr,
            "bool.str");
    }

    // Special case: string.ToString() → identity (return itself).
    if (prim == P::String) return val;

    // Numeric types: snprintf into a stack buffer, then heap-copy.
    // Buffer of 64 bytes is ample for any numeric type.
    const unsigned BUF = 64;
    llvm::Value* buf = entryAlloca(
        llvm::ArrayType::get(i8Ty, BUF));

    // Declare snprintf: int snprintf(char*, size_t, const char*, ...)
    auto* snprintfFn = module_->getFunction("snprintf");
    if (!snprintfFn) {
        auto* ft = llvm::FunctionType::get(
            i32(), {ptrTy(), i64(), ptrTy()}, /*variadic=*/true);
        snprintfFn = llvm::Function::Create(
            ft, llvm::Function::ExternalLinkage, "snprintf", module_.get());
    }

    // Pick format string and possibly widen/convert val.
    const char* fmt = "%d";
    llvm::Value* printVal = val;
    switch (prim) {
        case P::Long: case P::ULong:
            fmt = prim == P::ULong ? "%llu" : "%lld";
            break;
        case P::UInt:
            fmt = "%u"; break;
        case P::Byte: case P::SByte:
        case P::Short: case P::UShort:
            // Widen to i32: signed types (SByte, Short) use SExt;
            // unsigned types (Byte, UShort) use ZExt.
            printVal = (prim == P::SByte || prim == P::Short)
                ? builder_.CreateSExt(val, i32())
                : builder_.CreateZExt(val, i32());
            fmt = "%d"; break;
        case P::Float:
            printVal = builder_.CreateFPExt(val, llvm::Type::getDoubleTy(llvm_));
            fmt = "%.7g"; break;  // 7 significant digits for float (f32)
        case P::Double:
            fmt = "%.15g"; break;  // 15 significant digits for double (f64)
        case P::Char: {
            // Char is i32 code point → build a 1-char string directly.
            // Use snprintf with %c (ASCII, fast path) for code points 0-127.
            // For code points > 127, use %lc (wide char).
            // For simplicity, always use %c and truncate to 1 byte.
            // TODO: proper UTF-8 encoding for non-ASCII chars.
            fmt = "%c"; break;
        }
        default:
            fmt = "%d"; break;
    }

    // Create a global constant for the format string.
    llvm::Constant* fmtData = llvm::ConstantDataArray::getString(
        llvm_, fmt, /*AddNull=*/true);
    unsigned fmtId = stringLiteralIdx_++;
    auto* fmtGV = new llvm::GlobalVariable(
        *module_, fmtData->getType(), true,
        llvm::GlobalValue::PrivateLinkage, fmtData,
        ".bz.fmt." + std::to_string(fmtId));
    fmtGV->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);

    llvm::Value* bufPtr = builder_.CreateBitCast(
        builder_.CreateGEP(
            llvm::ArrayType::get(i8Ty, BUF), buf,
            {builder_.getInt64(0), builder_.getInt64(0)}),
        ptrTy());

    // snprintf(bufPtr, BUF, fmt, printVal)
    llvm::Value* written = builder_.CreateCall(
        snprintfFn,
        {bufPtr, builder_.getInt64(BUF), fmtGV, printVal},
        "snprintf.ret");

    // Allocate heap storage for the data: written+1 bytes.
    llvm::Value* dataSize = builder_.CreateAdd(
        builder_.CreateSExt(written, i64()), builder_.getInt64(1),
        "str.ts.datasize");
    auto* memcpyFn = declareC("memcpy", ptrTy(), {ptrTy(), ptrTy(), i64()});
    llvm::Value* dataCopy = builder_.CreateCall(mallocFn, {dataSize}, "str.ts.data");
    builder_.CreateCall(memcpyFn, {dataCopy, bufPtr,
                                   builder_.CreateSExt(written, i64())});
    // Null-terminate.
    llvm::Value* endPtr = builder_.CreateGEP(
        i8Ty, dataCopy, builder_.CreateSExt(written, i64()));
    builder_.CreateStore(llvm::ConstantInt::get(i8Ty, 0), endPtr);

    // Build the bz_string object.
    uint64_t strBytes = module_->getDataLayout().getTypeAllocSize(stringType());
    llvm::Value* newStr = builder_.CreateCall(
        mallocFn, {builder_.getInt64(strBytes)}, "str.ts.obj");
    builder_.CreateStore(dataCopy,
        builder_.CreateStructGEP(stringType(), newStr, 0));
    builder_.CreateStore(builder_.CreateSExt(written, i64()),
        builder_.CreateStructGEP(stringType(), newStr, 1));
    builder_.CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(llvm_), 1),
        builder_.CreateStructGEP(stringType(), newStr, 2));
        initStringRc(newStr);  // #422: producer's reference, rc := 1
    return newStr;
}


llvm::Value* Emitter::emitStringOrdCmp(llvm::Value* l, llvm::Value* r) {
    // Compare min(lenL,lenR) code points with memcmp (scaled by width);
    // if equal, compare code-point counts as tiebreak.
    llvm::Value* lenL = loadStringLen(l);
    llvm::Value* lenR = loadStringLen(r);
    llvm::Value* minLen = builder_.CreateSelect(
        builder_.CreateICmpSLT(lenL, lenR), lenL, lenR, "str.minlen");
    llvm::Value* dataL = loadStringData(l);
    llvm::Value* dataR = loadStringData(r);
    auto* memcmpFn = declareC("memcmp", i32(), {ptrTy(), ptrTy(), i64()});
    // Scale minLen by the element width so memcmp sees the right byte count.
    llvm::Value* wL64 = builder_.CreateZExt(loadStringWidth(l), i64(), "ordw");
    llvm::Value* minByteSz = builder_.CreateMul(minLen, wL64, "str.minbytes");
    llvm::Value* cmpRes = builder_.CreateCall(memcmpFn, {dataL, dataR, minByteSz},
                                              "str.cmp");
    // If memcmp returned 0, fall back to comparing code-point counts.
    auto* cmpZero = builder_.CreateICmpEQ(
        cmpRes, llvm::ConstantInt::get(i32(), 0), "str.cmpeq");
    llvm::Value* lenDiff = builder_.CreateSub(
        builder_.CreateTrunc(lenL, i32()),
        builder_.CreateTrunc(lenR, i32()), "str.lendiff");
    return builder_.CreateSelect(cmpZero, lenDiff, cmpRes, "str.ord");
}


llvm::Value* Emitter::tryLowerStringMethod(const ast::CallExpr& call,
                                   const ast::MemberAccessExpr& ma,
                                   llvm::Value* self) {
    const auto& method = ma.member;
    if (!ma.target) return nullptr;
    if (!self) self = lowerExpr(*ma.target);
    if (!self) return nullptr;

    // Helpers for extracting the first string argument.
    auto getStrArg = [&](size_t idx) -> llvm::Value* {
        if (idx >= call.args.size() || !call.args[idx].value) return nullptr;
        return lowerExpr(*call.args[idx].value);
    };
    auto getIntArg = [&](size_t idx) -> llvm::Value* {
        if (idx >= call.args.size() || !call.args[idx].value) return nullptr;
        return lowerExpr(*call.args[idx].value);
    };

    // §9.6 / §3.3 (#883): string.AsSpan() and its overloads → a ReadOnlyStringSpan
    // { Source = self (string ptr), Start, Length } over a contiguous code-point
    // range. Reuses the {ptr,i32,i32} span struct (buildArraySpan); the span's
    // indexer / Length read code points from the string source (loadCodePoint),
    // not array elements. Length/Start are CODE-POINT units (loadStringLen).
    // Bounds-checks mirror Array<T>.AsSpan (IndexOutOfRangeException).
    if (method == "AsSpan") {
        llvm::Value* len64 = loadStringLen(self);                  // i64 code pts
        llvm::Value* count = builder_.CreateTrunc(len64, i32(), "ss.count");
        if (call.args.empty())
            return buildArraySpan(self, builder_.getInt32(0), count);
        // (r: Range<int>) — Start = r.Start, Length = element count.
        if (call.args.size() == 1 && call.args[0].value &&
            isRangeType(ctx_.exprType(*call.args[0].value))) {
            llvm::Value* rv = lowerExpr(*call.args[0].value);
            if (!rv || !rv->getType()->isStructTy()) return nullptr;
            llvm::Value* lo = builder_.CreateTrunc(
                builder_.CreateExtractValue(rv, {0u}), i32(), "ss.r.lo");
            llvm::Value* hi = builder_.CreateTrunc(
                builder_.CreateExtractValue(rv, {1u}), i32(), "ss.r.hi");
            llvm::Value* incl = builder_.CreateTrunc(
                builder_.CreateExtractValue(rv, {3u}), i1(), "ss.r.incl");
            llvm::Value* span0 = builder_.CreateSub(hi, lo, "ss.r.span");
            llvm::Value* len = builder_.CreateSelect(
                incl, builder_.CreateAdd(span0, builder_.getInt32(1)), span0,
                "ss.r.len");
            llvm::Value* startNeg =
                builder_.CreateICmpSLT(lo, builder_.getInt32(0));
            llvm::Value* lenNeg = builder_.CreateICmpSLT(len, builder_.getInt32(0));
            llvm::Value* endOOB =
                builder_.CreateICmpSGT(builder_.CreateAdd(lo, len), count);
            emitGuardThrow(
                builder_.CreateOr(builder_.CreateOr(startNeg, lenNeg), endOOB),
                "IndexOutOfRangeException");
            return buildArraySpan(self, lo, len);
        }
        // (start) — [start, Length)
        if (call.args.size() == 1 && call.args[0].value) {
            llvm::Value* start = getIntArg(0);
            if (!start) return nullptr;
            if (!start->getType()->isIntegerTy(32))
                start = builder_.CreateTrunc(start, i32());
            emitGuardThrow(
                builder_.CreateOr(
                    builder_.CreateICmpSLT(start, builder_.getInt32(0)),
                    builder_.CreateICmpSGT(start, count)),
                "IndexOutOfRangeException");
            return buildArraySpan(self, start,
                                  builder_.CreateSub(count, start, "ss.len"));
        }
        // (start, length)
        if (call.args.size() == 2 && call.args[0].value && call.args[1].value) {
            llvm::Value* start = getIntArg(0);
            llvm::Value* len   = getIntArg(1);
            if (!start || !len) return nullptr;
            if (!start->getType()->isIntegerTy(32))
                start = builder_.CreateTrunc(start, i32());
            if (!len->getType()->isIntegerTy(32))
                len = builder_.CreateTrunc(len, i32());
            emitGuardThrow(
                builder_.CreateOr(
                    builder_.CreateICmpSLT(start, builder_.getInt32(0)),
                    builder_.CreateICmpSGT(start, count)),
                "IndexOutOfRangeException");
            llvm::Value* avail = builder_.CreateSub(count, start);
            emitGuardThrow(
                builder_.CreateOr(
                    builder_.CreateICmpSLT(len, builder_.getInt32(0)),
                    builder_.CreateICmpSGT(len, avail)),
                "IndexOutOfRangeException");
            return buildArraySpan(self, start, len);
        }
        return nullptr;
    }

    // §9.6: String parse methods — convenience wrappers over Convert.ToXxx.
    if (call.args.empty()) {
        llvm::Value* data = loadStringData(self);
        if (method == "ToInt32") {
            auto* atoiFn = module_->getFunction("atoi");
            if (!atoiFn) {
                auto* ft = llvm::FunctionType::get(i32(), {ptrTy()}, false);
                atoiFn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "atoi", module_.get());
            }
            return builder_.CreateCall(atoiFn, {data}, "atoi.ret");
        }
        if (method == "ToInt64") {
            auto* atollFn = module_->getFunction("atoll");
            if (!atollFn) {
                auto* ft = llvm::FunctionType::get(i64(), {ptrTy()}, false);
                atollFn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "atoll", module_.get());
            }
            return builder_.CreateCall(atollFn, {data}, "atoll.ret");
        }
        if (method == "ToDouble" || method == "ToFloat") {
            auto* atofFn = module_->getFunction("atof");
            if (!atofFn) {
                auto* ft = llvm::FunctionType::get(llvm::Type::getDoubleTy(llvm_), {ptrTy()}, false);
                atofFn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "atof", module_.get());
            }
            return builder_.CreateCall(atofFn, {data}, "atof.ret");
        }
        if (method == "ToBoolean") {
            // "true" → true, "1" → true, anything else → false
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
                builder_.CreateCall(strcmpFn, {data, trueGV}),
                llvm::ConstantInt::get(i32(), 0));
            llvm::Constant* oneStr = llvm::ConstantDataArray::getString(llvm_, "1", true);
            auto* oneGV = new llvm::GlobalVariable(*module_, oneStr->getType(), true,
                llvm::GlobalValue::PrivateLinkage, oneStr, ".bz.bool.one." + std::to_string(tid));
            llvm::Value* cmpOne = builder_.CreateICmpEQ(
                builder_.CreateCall(strcmpFn, {data, oneGV}),
                llvm::ConstantInt::get(i32(), 0));
            return builder_.CreateOr(cmpTrue, cmpOne, "tobool");
        }
        // stdlib §9.6, #882: GetHashCode(): int — content-based hash consistent
        // with string Equals/`=` (two content-equal strings hash equal).
        // Reuses emitValueHashCode with a string TypeRef, which folds code points
        // via h = h*31 + cp (same hash the synthesized record GetHashCode uses
        // when hashing a string-typed field, ensuring cross-context consistency).
        if (method == "GetHashCode") {
            sema::TypeRef strTy;
            strTy.kind = sema::TypeRef::Kind::Primitive;
            strTy.primitive = sema::PrimitiveType::String;
            return emitValueHashCode(self, &strTy);
        }
    }

    // §9.6: Equals(other: string): bool — content equality (same as `=`).
    // Equals(other, ignoreCase: bool): bool — optional case-insensitive.
    if (method == "Equals" && !call.args.empty() && call.args[0].value) {
        llvm::Value* other = getStrArg(0);
        if (!other) return nullptr;
        if (call.args.size() == 1) {
            return emitStringEq(self, other);
        }
        // 2-arg form: Equals(other, ignoreCase).
        // For ignoreCase=true: compare ToLower versions.
        llvm::Value* ignoreCase = lowerExpr(*call.args[1].value);
        if (!ignoreCase) return emitStringEq(self, other);
        // Fast path: ignoreCase=false → plain equality.
        auto* icBB = llvm::BasicBlock::Create(llvm_, "eq.ic", currentFn_);
        auto* planBB = llvm::BasicBlock::Create(llvm_, "eq.pl", currentFn_);
        auto* endBB  = llvm::BasicBlock::Create(llvm_, "eq.end", currentFn_);
        builder_.CreateCondBr(ignoreCase, icBB, planBB);
        builder_.SetInsertPoint(planBB);
        llvm::Value* plainEq = emitStringEq(self, other);
        auto* planEnd = builder_.GetInsertBlock();
        builder_.CreateBr(endBB);
        builder_.SetInsertPoint(icBB);
        // ToLower both, then compare.
        auto emitToLower = [&](llvm::Value* s) -> llvm::Value* {
            auto* i8Ty = llvm::Type::getInt8Ty(llvm_);
            auto* tolowerFn = module_->getFunction("tolower");
            if (!tolowerFn) {
                auto* ft = llvm::FunctionType::get(i32(), {i32()}, false);
                tolowerFn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "tolower", module_.get());
            }
            llvm::Value* slen = loadStringLen(s);
            auto* mallocFn = declareC("malloc", ptrTy(), {i64()});
            llvm::Value* dsz = builder_.CreateAdd(slen, builder_.getInt64(1));
            llvm::Value* nd = builder_.CreateCall(mallocFn, {dsz});
            auto* idxS = entryAlloca(i64());
            builder_.CreateStore(builder_.getInt64(0), idxS);
            auto* cBB2 = llvm::BasicBlock::Create(llvm_, "eq.lc", currentFn_);
            auto* bBB2 = llvm::BasicBlock::Create(llvm_, "eq.lb", currentFn_);
            auto* eBB2 = llvm::BasicBlock::Create(llvm_, "eq.le", currentFn_);
            builder_.CreateBr(cBB2);
            builder_.SetInsertPoint(cBB2);
            llvm::Value* si = builder_.CreateLoad(i64(), idxS);
            builder_.CreateCondBr(builder_.CreateICmpSLT(si, slen), bBB2, eBB2);
            builder_.SetInsertPoint(bBB2);
            llvm::Value* siB = builder_.CreateLoad(i64(), idxS);
            llvm::Value* cp = loadCodePoint(s, siB);
            llvm::Value* lc = builder_.CreateCall(tolowerFn, {cp});
            llvm::Value* off = builder_.CreateMul(siB, builder_.getInt64(1));  // width=1
            llvm::Value* dp = builder_.CreateGEP(i8Ty, nd, off);
            builder_.CreateStore(builder_.CreateTrunc(lc, i8Ty), dp);
            builder_.CreateStore(builder_.CreateAdd(siB, builder_.getInt64(1)), idxS);
            builder_.CreateBr(cBB2);
            builder_.SetInsertPoint(eBB2);
            llvm::Value* np = builder_.CreateGEP(i8Ty, nd, slen);
            builder_.CreateStore(llvm::ConstantInt::get(i8Ty, 0), np);
            uint64_t sb = module_->getDataLayout().getTypeAllocSize(stringType());
            llvm::Value* so = builder_.CreateCall(mallocFn, {builder_.getInt64(sb)});
            builder_.CreateStore(nd, builder_.CreateStructGEP(stringType(), so, 0));
            builder_.CreateStore(slen, builder_.CreateStructGEP(stringType(), so, 1));
            builder_.CreateStore(llvm::ConstantInt::get(i8Ty, 1),
                builder_.CreateStructGEP(stringType(), so, 2));
                initStringRc(so);  // #422: producer's reference, rc := 1
            return so;
        };
        llvm::Value* selfLc  = emitToLower(self);
        llvm::Value* otherLc = emitToLower(other);
        llvm::Value* icEq = emitStringEq(selfLc, otherLc);
        auto* icEnd = builder_.GetInsertBlock();
        builder_.CreateBr(endBB);
        builder_.SetInsertPoint(endBB);
        auto* phi = builder_.CreatePHI(i1(), 2, "eq.result");
        phi->addIncoming(plainEq, planEnd);
        phi->addIncoming(icEq, icEnd);
        return phi;
    }

    // §9.6: CompareTo(other: string): int — ordinal lexicographic comparison.
    // Returns negative / 0 / positive (same as emitStringOrdCmp).
    if (method == "CompareTo" && call.args.size() == 1 && call.args[0].value) {
        llvm::Value* other = getStrArg(0);
        if (!other) return nullptr;
        return builder_.CreateTrunc(emitStringOrdCmp(self, other), i32(), "cmp.result");
    }

    // §9.6 / #535: StartsWith(prefix: string): bool — code-point comparison.
    // Delegates to bz_str_startswith which handles width 1/2/4 uniformly.
    if (method == "StartsWith") {
        llvm::Value* prefix = getStrArg(0);
        if (!prefix) return nullptr;
        auto* fn = declareC("bz_str_startswith", i32(), {ptrTy(), ptrTy()});
        llvm::Value* res = builder_.CreateCall(fn, {self, prefix}, "sw.result");
        return builder_.CreateTrunc(res, i1(), "sw.bool");
    }

    // §9.6 / #535: EndsWith(suffix: string): bool — code-point comparison.
    // Delegates to bz_str_endswith which handles width 1/2/4 uniformly.
    if (method == "EndsWith") {
        llvm::Value* suffix = getStrArg(0);
        if (!suffix) return nullptr;
        auto* fn = declareC("bz_str_endswith", i32(), {ptrTy(), ptrTy()});
        llvm::Value* res = builder_.CreateCall(fn, {self, suffix}, "ew.result");
        return builder_.CreateTrunc(res, i1(), "ew.bool");
    }

    // §9.6: Contains(sub: string): bool — uses libc memmem (POSIX).
    if (method == "Contains") {
        // §9.6: Contains(char) overload — scan code-point by code-point.
        if (call.args.size() == 1 && call.args[0].value) {
            auto* argTy = ctx_.exprType(*call.args[0].value);
            if (argTy && argTy->kind == sema::TypeRef::Kind::Primitive &&
                argTy->primitive == sema::PrimitiveType::Char) {
                llvm::Value* needle = lowerExpr(*call.args[0].value);
                if (!needle) return nullptr;
                llvm::Value* len = loadStringLen(self);
                auto* idxS = entryAlloca(i64());
                builder_.CreateStore(builder_.getInt64(0), idxS);
                auto* loopBB = llvm::BasicBlock::Create(llvm_, "cic.loop", currentFn_);
                auto* bodyBB = llvm::BasicBlock::Create(llvm_, "cic.body", currentFn_);
                auto* foundBB = llvm::BasicBlock::Create(llvm_, "cic.found", currentFn_);
                auto* endBB  = llvm::BasicBlock::Create(llvm_, "cic.end",  currentFn_);
                builder_.CreateBr(loopBB);
                builder_.SetInsertPoint(loopBB);
                llvm::Value* idx = builder_.CreateLoad(i64(), idxS);
                builder_.CreateCondBr(
                    builder_.CreateICmpSLT(idx, len), bodyBB, endBB);
                builder_.SetInsertPoint(bodyBB);
                llvm::Value* cp = loadCodePoint(self, idx);
                auto* incBB = llvm::BasicBlock::Create(llvm_, "cic.inc", currentFn_);
                builder_.CreateCondBr(
                    builder_.CreateICmpEQ(cp, needle), foundBB, incBB);
                builder_.SetInsertPoint(incBB);
                builder_.CreateStore(
                    builder_.CreateAdd(idx, builder_.getInt64(1)), idxS);
                builder_.CreateBr(loopBB);
                builder_.SetInsertPoint(foundBB);
                builder_.CreateBr(endBB);
                builder_.SetInsertPoint(endBB);
                auto* phi = builder_.CreatePHI(i1(), 3);
                phi->addIncoming(builder_.getFalse(), loopBB);
                phi->addIncoming(builder_.getTrue(), foundBB);
                return phi;
            }
        }
        // §9.6 / #535: Contains(sub: string) — code-point comparison via runtime.
        llvm::Value* sub = getStrArg(0);
        if (!sub) return nullptr;
        auto* fn = declareC("bz_str_contains", i32(), {ptrTy(), ptrTy()});
        llvm::Value* res = builder_.CreateCall(fn, {self, sub}, "contains.r");
        return builder_.CreateTrunc(res, i1(), "contains");
    }

    // §9.6: IndexOf(sub: string): int — first occurrence, -1 if not found.
    if (method == "IndexOf") {
        // Check if arg is a char (not a string) — linear scan for code point.
        if (call.args.size() == 1 && call.args[0].value) {
            auto* argTy = ctx_.exprType(*call.args[0].value);
            if (argTy && argTy->kind == sema::TypeRef::Kind::Primitive &&
                argTy->primitive == sema::PrimitiveType::Char) {
                llvm::Value* needle = lowerExpr(*call.args[0].value);
                if (!needle) return nullptr;
                llvm::Value* len = loadStringLen(self);
                auto* idxS = entryAlloca(i64());
                builder_.CreateStore(builder_.getInt64(0), idxS);
                auto* cBB = llvm::BasicBlock::Create(llvm_, "ioc.c", currentFn_);
                auto* bBB = llvm::BasicBlock::Create(llvm_, "ioc.b", currentFn_);
                auto* sBB = llvm::BasicBlock::Create(llvm_, "ioc.s", currentFn_);
                auto* eBB = llvm::BasicBlock::Create(llvm_, "ioc.e", currentFn_);
                builder_.CreateBr(cBB);
                builder_.SetInsertPoint(cBB);
                llvm::Value* idx = builder_.CreateLoad(i64(), idxS);
                builder_.CreateCondBr(builder_.CreateICmpSLT(idx, len), bBB, sBB);
                builder_.SetInsertPoint(bBB);
                llvm::Value* idxB = builder_.CreateLoad(i64(), idxS);
                llvm::Value* cp = loadCodePoint(self, idxB);
                llvm::Value* found = builder_.CreateICmpEQ(cp, needle);
                auto* incBB = llvm::BasicBlock::Create(llvm_, "ioc.inc", currentFn_);
                builder_.CreateCondBr(found, eBB, incBB);
                builder_.SetInsertPoint(incBB);
                builder_.CreateStore(builder_.CreateAdd(idxB, builder_.getInt64(1)), idxS);
                builder_.CreateBr(cBB);
                builder_.SetInsertPoint(sBB);  // not found
                builder_.CreateBr(eBB);
                builder_.SetInsertPoint(eBB);
                llvm::Value* fi = builder_.CreateLoad(i64(), idxS);
                // If we exited because idx >= len (sBB), result = -1
                llvm::Value* exhausted = builder_.CreateICmpSGE(fi, len, "ioc.exhausted");
                llvm::Value* result = builder_.CreateSelect(exhausted,
                    llvm::ConstantInt::getSigned(i32(), -1),
                    builder_.CreateTrunc(fi, i32()), "ioc.result");
                return result;
            }
        }
        // §9.6 / #649: IndexOf(sub: string, start: int) — 2-arg overload.
        // `start` is a code-point index (§4.6 adaptive-width strings); the
        // runtime bz_str_indexof_from honours the width-aware code-point model.
        if (call.args.size() == 2 && call.args[0].value && call.args[1].value) {
            llvm::Value* sub = lowerExpr(*call.args[0].value);
            if (!sub) return nullptr;
            llvm::Value* startVal = lowerExpr(*call.args[1].value);
            if (!startVal) return nullptr;
            // Normalize start to i64 for the runtime.
            if (!startVal->getType()->isIntegerTy(64))
                startVal = builder_.CreateSExt(startVal, i64(), "iof2.s64");
            auto* fn = declareC("bz_str_indexof_from", i32(),
                                {ptrTy(), ptrTy(), i64()});
            return builder_.CreateCall(fn, {self, sub, startVal}, "iof2.result");
        }
        // §9.6 / #535: IndexOf(sub: string) — code-point index via runtime.
        llvm::Value* sub = getStrArg(0);
        if (!sub) return nullptr;
        auto* fn = declareC("bz_str_indexof", i32(), {ptrTy(), ptrTy()});
        return builder_.CreateCall(fn, {self, sub}, "iof.result");
    }

    // §9.6: LastIndexOf(sub: string): int — last occurrence, -1 if not found.
    // §9.6: LastIndexOf(char) overload — scan backward, return last match index.
    if (method == "LastIndexOf" && call.args.size() == 1 && call.args[0].value) {
        auto* argTy = ctx_.exprType(*call.args[0].value);
        if (argTy && argTy->kind == sema::TypeRef::Kind::Primitive &&
            argTy->primitive == sema::PrimitiveType::Char) {
            llvm::Value* needle = lowerExpr(*call.args[0].value);
            if (!needle) return nullptr;
            llvm::Value* len = loadStringLen(self);
            // Scan from len-1 down to 0.
            auto* lastS = entryAlloca(i32());
            builder_.CreateStore(builder_.getInt32(-1), lastS);
            auto* cBB = llvm::BasicBlock::Create(llvm_, "lioc.c", currentFn_);
            auto* bBB = llvm::BasicBlock::Create(llvm_, "lioc.b", currentFn_);
            auto* eBB = llvm::BasicBlock::Create(llvm_, "lioc.e", currentFn_);
            auto* idxS = entryAlloca(i64());
            llvm::Value* startIdx = builder_.CreateSub(len, builder_.getInt64(1));
            builder_.CreateStore(startIdx, idxS);
            builder_.CreateBr(cBB);
            builder_.SetInsertPoint(cBB);
            llvm::Value* idx = builder_.CreateLoad(i64(), idxS);
            builder_.CreateCondBr(builder_.CreateICmpSGE(idx, builder_.getInt64(0)), bBB, eBB);
            builder_.SetInsertPoint(bBB);
            llvm::Value* idxB = builder_.CreateLoad(i64(), idxS);
            llvm::Value* cp = loadCodePoint(self, idxB);
            llvm::Value* found = builder_.CreateICmpEQ(cp, needle);
            auto* storeB = llvm::BasicBlock::Create(llvm_, "lioc.st", currentFn_);
            auto* decBB = llvm::BasicBlock::Create(llvm_, "lioc.dec", currentFn_);
            builder_.CreateCondBr(found, storeB, decBB);
            builder_.SetInsertPoint(storeB);
            builder_.CreateStore(builder_.CreateTrunc(idxB, i32()), lastS);
            builder_.CreateBr(eBB);  // stop at first match from the end
            builder_.SetInsertPoint(decBB);
            builder_.CreateStore(builder_.CreateSub(idxB, builder_.getInt64(1)), idxS);
            builder_.CreateBr(cBB);
            builder_.SetInsertPoint(eBB);
            return builder_.CreateLoad(i32(), lastS, "lioc.result");
        }
    }
    // §9.6 / #535: LastIndexOf(sub: string) — code-point index via runtime.
    if (method == "LastIndexOf") {
        llvm::Value* sub = getStrArg(0);
        if (!sub) return nullptr;
        auto* fn = declareC("bz_str_lastindexof", i32(), {ptrTy(), ptrTy()});
        return builder_.CreateCall(fn, {self, sub}, "liof.result");
    }

    // §9.6: Substring(start) → new string from start to end.
    // §9.6 / #535: Substring(start[, length]): owned string — code-point slice.
    // Delegates to bz_str_substring which uses the source string's width field
    // to copy the correct bytes (width * count) and preserves the source width.
    if (method == "Substring") {
        llvm::Value* startV = getIntArg(0);
        if (!startV) return nullptr;
        llvm::Value* start64 = builder_.CreateSExt(startV, i64(), "sub.start");
        bool hasLen = call.args.size() >= 2 && call.args[1].value;
        // If length arg absent, pass -1 to signal "rest of string" to runtime.
        llvm::Value* count64 = hasLen
            ? builder_.CreateSExt(lowerExpr(*call.args[1].value), i64(), "sub.count")
            : builder_.getInt64(-1);
        // fix #747 R43 / §9.6 / language spec §4.6: an out-of-range start or
        // length raises IndexOutOfRangeException — the same convention every
        // string/span/array bounds operation follows (the runtime previously
        // silently CLAMPED, the only clamping bounds op in the stdlib). `start`
        // is valid in [0, len] (start == len is the empty tail); for the
        // two-arg form `length` must be >= 0 and `start + length <= len`. The
        // single-arg form keeps the runtime's -1 "rest of string" sentinel.
        // Bounds are in CODE POINTS (loadStringLen), matching #535's code-point
        // Substring and the slicing path in emitStringSliceBytes.
        llvm::Value* len = loadStringLen(self);  // i64 code-point count
        llvm::Value* oor = builder_.CreateOr(
            builder_.CreateICmpSLT(start64, builder_.getInt64(0), "sub.sneg"),
            builder_.CreateICmpSGT(start64, len, "sub.sbig"), "sub.soor");
        if (hasLen) {
            llvm::Value* lenNeg =
                builder_.CreateICmpSLT(count64, builder_.getInt64(0), "sub.lneg");
            llvm::Value* end = builder_.CreateAdd(start64, count64, "sub.end");
            llvm::Value* endBig =
                builder_.CreateICmpSGT(end, len, "sub.ebig");
            oor = builder_.CreateOr(
                oor, builder_.CreateOr(lenNeg, endBig, "sub.loor"), "sub.oor");
        }
        emitGuardThrow(oor, "IndexOutOfRangeException");
        auto* fn = declareC("bz_str_substring", ptrTy(), {ptrTy(), i64(), i64()});
        return builder_.CreateCall(fn, {self, start64, count64}, "sub.result");
    }

    // §9.6: Replace(oldChar, newChar): string — replaces all char occurrences.
    // Detected by arg type being char (Char primitive).
    if (method == "Replace" && call.args.size() == 2 &&
        call.args[0].value && call.args[1].value) {
        auto* at0 = ctx_.exprType(*call.args[0].value);
        if (at0 && at0->kind == sema::TypeRef::Kind::Primitive &&
            at0->primitive == sema::PrimitiveType::Char) {
            llvm::Value* oldCh = lowerExpr(*call.args[0].value);
            llvm::Value* newCh = lowerExpr(*call.args[1].value);
            if (!oldCh || !newCh) return nullptr;
            auto* i8Ty  = llvm::Type::getInt8Ty(llvm_);
            auto* mallocFn = declareC("malloc", ptrTy(), {i64()});
            auto* memcpyFn = declareC("memcpy", ptrTy(), {ptrTy(), ptrTy(), i64()});
            // Only handle ASCII (width=1) strings for now.
            llvm::Value* len  = loadStringLen(self);   // i64
            llvm::Value* data = loadStringData(self);  // ptr
            // Alloc new buffer: same length.
            llvm::Value* bufLen = builder_.CreateAdd(len, builder_.getInt64(1));
            llvm::Value* newBuf = builder_.CreateCall(mallocFn, {bufLen});
            builder_.CreateCall(memcpyFn, {newBuf, data, len});
            // Store NUL terminator.
            builder_.CreateStore(
                llvm::ConstantInt::get(i8Ty, 0),
                builder_.CreateGEP(i8Ty, newBuf, len));
            // Loop: for i in 0..len, if newBuf[i]==oldCh, newBuf[i]=newCh.
            auto* condBB = llvm::BasicBlock::Create(llvm_, "rcc.cond", currentFn_);
            auto* bodyBB = llvm::BasicBlock::Create(llvm_, "rcc.body", currentFn_);
            auto* contBB = llvm::BasicBlock::Create(llvm_, "rcc.cont", currentFn_);
            auto* endBB  = llvm::BasicBlock::Create(llvm_, "rcc.end",  currentFn_);
            auto* iSlot  = entryAlloca(i64());
            builder_.CreateStore(builder_.getInt64(0), iSlot);
            builder_.CreateBr(condBB);
            builder_.SetInsertPoint(condBB);
            llvm::Value* i = builder_.CreateLoad(i64(), iSlot);
            builder_.CreateCondBr(builder_.CreateICmpSLT(i, len), bodyBB, endBB);
            builder_.SetInsertPoint(bodyBB);
            llvm::Value* i2  = builder_.CreateLoad(i64(), iSlot);
            llvm::Value* ptr = builder_.CreateGEP(i8Ty, newBuf, i2);
            llvm::Value* ch  = builder_.CreateLoad(i8Ty, ptr);
            llvm::Value* ch32 = builder_.CreateZExt(ch, i32());
            llvm::Value* isOld = builder_.CreateICmpEQ(ch32, oldCh);
            builder_.CreateCondBr(isOld, contBB, contBB);  // body always falls to cont
            builder_.SetInsertPoint(contBB);
            // Re-load i since we're in a new block.
            llvm::Value* i3  = builder_.CreateLoad(i64(), iSlot);
            llvm::Value* ptr3 = builder_.CreateGEP(i8Ty, newBuf, i3);
            llvm::Value* ch3  = builder_.CreateLoad(i8Ty, ptr3);
            llvm::Value* ch33 = builder_.CreateZExt(ch3, i32());
            llvm::Value* isOld3 = builder_.CreateICmpEQ(ch33, oldCh);
            // newCh to i8 (truncate — ASCII only)
            llvm::Value* newChI8 = builder_.CreateTrunc(newCh, i8Ty);
            llvm::Value* ch8 = builder_.CreateLoad(i8Ty, ptr3);
            llvm::Value* selected = builder_.CreateSelect(isOld3, newChI8, ch8);
            builder_.CreateStore(selected, ptr3);
            builder_.CreateStore(
                builder_.CreateAdd(i3, builder_.getInt64(1)), iSlot);
            builder_.CreateBr(condBB);
            builder_.SetInsertPoint(endBB);
            // Wrap in bz_string struct.
            uint64_t strBytes = module_->getDataLayout().getTypeAllocSize(stringType());
            llvm::Value* newStr2 = builder_.CreateCall(
                mallocFn, {builder_.getInt64(strBytes)}, "rcc.obj");
            builder_.CreateStore(newBuf, builder_.CreateStructGEP(stringType(), newStr2, 0));
            builder_.CreateStore(len, builder_.CreateStructGEP(stringType(), newStr2, 1));
            builder_.CreateStore(
                llvm::ConstantInt::get(i8Ty, 1),
                builder_.CreateStructGEP(stringType(), newStr2, 2));
                initStringRc(newStr2);  // #422: producer's reference, rc := 1
            return newStr2;
        }
    }

    // §9.6 / #535: Replace(oldStr, newStr): string — code-point-aware via runtime.
    // bz_str_replace handles width 1/2/4 correctly and chooses the narrowest
    // output width for the result (§4.6 adaptive storage).
    if (method == "Replace" && call.args.size() == 2 &&
        call.args[0].value && call.args[1].value) {
        llvm::Value* oldStr = lowerExpr(*call.args[0].value);
        llvm::Value* newStr = lowerExpr(*call.args[1].value);
        if (!oldStr || !newStr) return nullptr;
        auto* fn = declareC("bz_str_replace", ptrTy(), {ptrTy(), ptrTy(), ptrTy()});
        return builder_.CreateCall(fn, {self, oldStr, newStr}, "repl.result");
    }

    // §9.6: ToUpper/ToLower — ASCII-only case conversion via toupper/tolower.
    if (method == "ToUpper" || method == "ToLower") {
        if (!call.args.empty()) return nullptr;  // no extra args
        auto* i8Ty = llvm::Type::getInt8Ty(llvm_);
        llvm::Value* len = loadStringLen(self);
        llvm::Value* data = loadStringData(self);
        // Declare toupper / tolower: int fn(int)
        const char* fnName = method == "ToUpper" ? "toupper" : "tolower";
        auto* caseFn = module_->getFunction(fnName);
        if (!caseFn) {
            auto* ft = llvm::FunctionType::get(i32(), {i32()}, false);
            caseFn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, fnName, module_.get());
        }
        // Allocate new buffer.
        auto* mallocFn = declareC("malloc", ptrTy(), {i64()});
        llvm::Value* bufSz = builder_.CreateAdd(len, builder_.getInt64(1));
        llvm::Value* newData = builder_.CreateCall(mallocFn, {bufSz}, "tc.data");
        // Loop: for i in 0..len, newData[i] = fn(data[i])
        auto* idxSlot = entryAlloca(i64());
        builder_.CreateStore(builder_.getInt64(0), idxSlot);
        auto* condBB = llvm::BasicBlock::Create(llvm_, "tc.cond", currentFn_);
        auto* bodyBB = llvm::BasicBlock::Create(llvm_, "tc.body", currentFn_);
        auto* endBB  = llvm::BasicBlock::Create(llvm_, "tc.end",  currentFn_);
        builder_.CreateBr(condBB);
        builder_.SetInsertPoint(condBB);
        llvm::Value* idx = builder_.CreateLoad(i64(), idxSlot);
        builder_.CreateCondBr(builder_.CreateICmpSLT(idx, len), bodyBB, endBB);
        builder_.SetInsertPoint(bodyBB);
        llvm::Value* idxB = builder_.CreateLoad(i64(), idxSlot);
        llvm::Value* bytePtr = builder_.CreateGEP(i8Ty, data, idxB);
        llvm::Value* byte = builder_.CreateLoad(i8Ty, bytePtr);
        llvm::Value* asI32 = builder_.CreateZExt(byte, i32());
        llvm::Value* converted = builder_.CreateCall(caseFn, {asI32});
        llvm::Value* asByte = builder_.CreateTrunc(converted, i8Ty);
        llvm::Value* dstPtr = builder_.CreateGEP(i8Ty, newData, idxB);
        builder_.CreateStore(asByte, dstPtr);
        llvm::Value* next = builder_.CreateAdd(idxB, builder_.getInt64(1));
        builder_.CreateStore(next, idxSlot);
        builder_.CreateBr(condBB);
        builder_.SetInsertPoint(endBB);
        // Null-terminate.
        llvm::Value* nullPtr = builder_.CreateGEP(i8Ty, newData, len);
        builder_.CreateStore(llvm::ConstantInt::get(i8Ty, 0), nullPtr);
        // Build result string.
        uint64_t strBytes = module_->getDataLayout().getTypeAllocSize(stringType());
        llvm::Value* newStr = builder_.CreateCall(mallocFn, {builder_.getInt64(strBytes)}, "tc.obj");
        builder_.CreateStore(newData, builder_.CreateStructGEP(stringType(), newStr, 0));
        builder_.CreateStore(len, builder_.CreateStructGEP(stringType(), newStr, 1));
        builder_.CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(llvm_), 1),
            builder_.CreateStructGEP(stringType(), newStr, 2));
            initStringRc(newStr);  // #422: producer's reference, rc := 1
        return newStr;
    }

    // §9.6: Trim/TrimStart/TrimEnd — remove leading/trailing ASCII whitespace.
    if (method == "Trim" || method == "TrimStart" || method == "TrimEnd") {
        // stdlib §9.6 (#741): Trim(chars: IEnumerable<char>) strips leading and
        // trailing code points in the given set (passed as Array<char>). No case
        // existed → the call emitted nothing, so `t` stayed at its null alloca
        // and `.Length` dereferenced null. (TrimStart/TrimEnd char-set overloads
        // are analogous but not yet wired; only Trim(set) is handled here.)
        if (method == "Trim" && call.args.size() == 1 && call.args[0].value) {
            llvm::Value* chars = lowerExpr(*call.args[0].value);
            if (!chars) return nullptr;
            auto* fn = declareC("bz_str_trim_chars", ptrTy(), {ptrTy(), ptrTy()});
            return builder_.CreateCall(fn, {self, chars}, "trim.chars.result");
        }
        if (!call.args.empty()) return nullptr;
        // Implement via stripping bytes that match isspace.
        auto* i8Ty = llvm::Type::getInt8Ty(llvm_);
        llvm::Value* len = loadStringLen(self);
        llvm::Value* data = loadStringData(self);
        auto* mallocFn = declareC("malloc", ptrTy(), {i64()});
        // Declare isspace: int isspace(int)
        auto* isspaceFn = module_->getFunction("isspace");
        if (!isspaceFn) {
            auto* ft = llvm::FunctionType::get(i32(), {i32()}, false);
            isspaceFn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "isspace", module_.get());
        }
        // Find start and end indices.
        auto startSlot = entryAlloca(i64());
        auto endSlot   = entryAlloca(i64());
        builder_.CreateStore(builder_.getInt64(0), startSlot);
        builder_.CreateStore(len, endSlot);
        // Simple approach: use memmove-based trimming with loop.
        // For brevity, reuse Substring logic after computing new bounds.
        llvm::Value* startV = builder_.getInt64(0);
        llvm::Value* endV   = len;
        // Skip leading whitespace (for Trim and TrimStart).
        if (method == "Trim" || method == "TrimStart") {
            auto* stBB  = llvm::BasicBlock::Create(llvm_, "trim.s.cond", currentFn_);
            auto* stBdy = llvm::BasicBlock::Create(llvm_, "trim.s.body", currentFn_);
            auto* stEnd = llvm::BasicBlock::Create(llvm_, "trim.s.end",  currentFn_);
            builder_.CreateStore(startV, startSlot);
            builder_.CreateBr(stBB);
            builder_.SetInsertPoint(stBB);
            llvm::Value* si = builder_.CreateLoad(i64(), startSlot);
            llvm::Value* sLt = builder_.CreateICmpSLT(si, endV);
            builder_.CreateCondBr(sLt, stBdy, stEnd);
            builder_.SetInsertPoint(stBdy);
            llvm::Value* siB = builder_.CreateLoad(i64(), startSlot);
            llvm::Value* bp  = builder_.CreateGEP(i8Ty, data, siB);
            llvm::Value* by  = builder_.CreateLoad(i8Ty, bp);
            llvm::Value* byI = builder_.CreateZExt(by, i32());
            llvm::Value* isSp = builder_.CreateCall(isspaceFn, {byI});
            llvm::Value* isSpBool = builder_.CreateICmpNE(isSp, builder_.getInt32(0));
            auto* nextSBB = llvm::BasicBlock::Create(llvm_, "trim.s.next", currentFn_);
            builder_.CreateCondBr(isSpBool, nextSBB, stEnd);
            builder_.SetInsertPoint(nextSBB);
            llvm::Value* siN = builder_.CreateLoad(i64(), startSlot);
            builder_.CreateStore(builder_.CreateAdd(siN, builder_.getInt64(1)), startSlot);
            builder_.CreateBr(stBB);
            builder_.SetInsertPoint(stEnd);
            startV = builder_.CreateLoad(i64(), startSlot);
        }
        // Skip trailing whitespace (for Trim and TrimEnd).
        if (method == "Trim" || method == "TrimEnd") {
            builder_.CreateStore(len, endSlot);
            auto* etBB  = llvm::BasicBlock::Create(llvm_, "trim.e.cond", currentFn_);
            auto* etBdy = llvm::BasicBlock::Create(llvm_, "trim.e.body", currentFn_);
            auto* etEnd = llvm::BasicBlock::Create(llvm_, "trim.e.end",  currentFn_);
            builder_.CreateBr(etBB);
            builder_.SetInsertPoint(etBB);
            llvm::Value* ei = builder_.CreateLoad(i64(), endSlot);
            llvm::Value* eGt = builder_.CreateICmpSGT(ei, startV);
            builder_.CreateCondBr(eGt, etBdy, etEnd);
            builder_.SetInsertPoint(etBdy);
            llvm::Value* eiB = builder_.CreateLoad(i64(), endSlot);
            llvm::Value* prev = builder_.CreateSub(eiB, builder_.getInt64(1));
            llvm::Value* bp  = builder_.CreateGEP(i8Ty, data, prev);
            llvm::Value* by  = builder_.CreateLoad(i8Ty, bp);
            llvm::Value* byI = builder_.CreateZExt(by, i32());
            llvm::Value* isSp = builder_.CreateCall(isspaceFn, {byI});
            llvm::Value* isSpBool = builder_.CreateICmpNE(isSp, builder_.getInt32(0));
            auto* nextEBB = llvm::BasicBlock::Create(llvm_, "trim.e.next", currentFn_);
            builder_.CreateCondBr(isSpBool, nextEBB, etEnd);
            builder_.SetInsertPoint(nextEBB);
            llvm::Value* eiN = builder_.CreateLoad(i64(), endSlot);
            builder_.CreateStore(builder_.CreateSub(eiN, builder_.getInt64(1)), endSlot);
            builder_.CreateBr(etBB);
            builder_.SetInsertPoint(etEnd);
            endV = builder_.CreateLoad(i64(), endSlot);
        }
        // Build trimmed string.
        llvm::Value* trimLen = builder_.CreateSub(endV, startV);
        llvm::Value* dataSz = builder_.CreateAdd(trimLen, builder_.getInt64(1));
        llvm::Value* newData = builder_.CreateCall(mallocFn, {dataSz}, "trim.data");
        auto* memcpyFn = declareC("memcpy", ptrTy(), {ptrTy(), ptrTy(), i64()});
        llvm::Value* srcPtr = builder_.CreateGEP(i8Ty, data, startV);
        builder_.CreateCall(memcpyFn, {newData, srcPtr, trimLen});
        llvm::Value* nulPtr = builder_.CreateGEP(i8Ty, newData, trimLen);
        builder_.CreateStore(llvm::ConstantInt::get(i8Ty, 0), nulPtr);
        uint64_t strBytes = module_->getDataLayout().getTypeAllocSize(stringType());
        llvm::Value* newStr = builder_.CreateCall(mallocFn, {builder_.getInt64(strBytes)}, "trim.obj");
        builder_.CreateStore(newData, builder_.CreateStructGEP(stringType(), newStr, 0));
        builder_.CreateStore(trimLen, builder_.CreateStructGEP(stringType(), newStr, 1));
        builder_.CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(llvm_), 1),
            builder_.CreateStructGEP(stringType(), newStr, 2));
            initStringRc(newStr);  // #422: producer's reference, rc := 1
        return newStr;
    }

    // §9.6: PadLeft(totalWidth[, padChar])/PadRight(totalWidth[, padChar]): string
    // Pads with the given character (default: space) to reach totalWidth.
    if ((method == "PadLeft" || method == "PadRight") &&
        !call.args.empty() && call.args[0].value) {
        llvm::Value* widthArg = lowerExpr(*call.args[0].value);
        if (!widthArg) return nullptr;
        auto* i8Ty = llvm::Type::getInt8Ty(llvm_);
        llvm::Value* selfLen = loadStringLen(self);
        llvm::Value* selfData = loadStringData(self);
        llvm::Value* width64 = builder_.CreateSExt(widthArg, i64(), "pad.width");
        // If selfLen >= totalWidth, return self unchanged.
        llvm::Value* needPad = builder_.CreateICmpSGT(width64, selfLen, "pad.need");
        auto* padBB = llvm::BasicBlock::Create(llvm_, "pad.do",  currentFn_);
        auto* endBB = llvm::BasicBlock::Create(llvm_, "pad.end", currentFn_);
        auto* entBB = builder_.GetInsertBlock();
        builder_.CreateCondBr(needPad, padBB, endBB);
        builder_.SetInsertPoint(padBB);
        llvm::Value* padLen = builder_.CreateSub(width64, selfLen, "pad.len");
        auto* mallocFn = declareC("malloc", ptrTy(), {i64()});
        auto* memcpyFn = declareC("memcpy", ptrTy(), {ptrTy(), ptrTy(), i64()});
        auto* memsetFn = module_->getFunction("memset");
        if (!memsetFn) {
            auto* ft = llvm::FunctionType::get(ptrTy(), {ptrTy(), i32(), i64()}, false);
            memsetFn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "memset", module_.get());
        }
        llvm::Value* dataSz = builder_.CreateAdd(width64, builder_.getInt64(1));
        llvm::Value* newData = builder_.CreateCall(mallocFn, {dataSz}, "pad.data");
        // Get the pad character (default: space, or 2nd arg if provided).
        llvm::Value* padCharI32 = builder_.getInt32(' ');
        if (call.args.size() >= 2 && call.args[1].value) {
            llvm::Value* pc = lowerExpr(*call.args[1].value);
            if (pc && pc->getType()->isIntegerTy(32)) padCharI32 = pc;
            else if (pc) padCharI32 = builder_.CreateSExt(pc, i32());
        }
        // Fill with pad character.
        builder_.CreateCall(memsetFn, {newData, padCharI32, width64});
        if (method == "PadLeft") {
            // Self goes after the spaces.
            llvm::Value* dst = builder_.CreateGEP(i8Ty, newData, padLen);
            builder_.CreateCall(memcpyFn, {dst, selfData, selfLen});
        } else {
            // Self goes at the start.
            builder_.CreateCall(memcpyFn, {newData, selfData, selfLen});
        }
        // Null-terminate.
        llvm::Value* nulPtr = builder_.CreateGEP(i8Ty, newData, width64);
        builder_.CreateStore(llvm::ConstantInt::get(i8Ty, 0), nulPtr);
        uint64_t strBytes = module_->getDataLayout().getTypeAllocSize(stringType());
        llvm::Value* newStr = builder_.CreateCall(mallocFn, {builder_.getInt64(strBytes)}, "pad.obj");
        builder_.CreateStore(newData, builder_.CreateStructGEP(stringType(), newStr, 0));
        builder_.CreateStore(width64, builder_.CreateStructGEP(stringType(), newStr, 1));
        builder_.CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(llvm_), 1),
            builder_.CreateStructGEP(stringType(), newStr, 2));
            initStringRc(newStr);  // #422: producer's reference, rc := 1
        auto* padEnd = builder_.GetInsertBlock();
        builder_.CreateBr(endBB);
        builder_.SetInsertPoint(endBB);
        auto* phi = builder_.CreatePHI(ptrTy(), 2, "pad.result");
        phi->addIncoming(self, entBB);    // no padding needed
        phi->addIncoming(newStr, padEnd); // padded result
        return phi;
    }

    // §9.6: Repeat(n: int): string — repeat self n times.
    if (method == "Repeat" && call.args.size() == 1 && call.args[0].value) {
        llvm::Value* nArg = lowerExpr(*call.args[0].value);
        if (!nArg) return nullptr;
        auto* i8Ty = llvm::Type::getInt8Ty(llvm_);
        llvm::Value* selfLen = loadStringLen(self);
        llvm::Value* selfData = loadStringData(self);
        llvm::Value* n64 = builder_.CreateSExt(nArg, i64(), "rep.n");
        llvm::Value* totalLen = builder_.CreateMul(selfLen, n64, "rep.total");
        auto* mallocFn = declareC("malloc", ptrTy(), {i64()});
        auto* memcpyFn = declareC("memcpy", ptrTy(), {ptrTy(), ptrTy(), i64()});
        llvm::Value* dataSz = builder_.CreateAdd(totalLen, builder_.getInt64(1));
        llvm::Value* newData = builder_.CreateCall(mallocFn, {dataSz}, "rep.data");
        // Loop: copy self n times.
        auto* idxSlot = entryAlloca(i64());
        builder_.CreateStore(builder_.getInt64(0), idxSlot);
        auto* condBB = llvm::BasicBlock::Create(llvm_, "rep.cond", currentFn_);
        auto* bodyBB = llvm::BasicBlock::Create(llvm_, "rep.body", currentFn_);
        auto* endBB  = llvm::BasicBlock::Create(llvm_, "rep.end",  currentFn_);
        builder_.CreateBr(condBB);
        builder_.SetInsertPoint(condBB);
        llvm::Value* idx = builder_.CreateLoad(i64(), idxSlot);
        builder_.CreateCondBr(builder_.CreateICmpSLT(idx, n64), bodyBB, endBB);
        builder_.SetInsertPoint(bodyBB);
        llvm::Value* idxB = builder_.CreateLoad(i64(), idxSlot);
        llvm::Value* off = builder_.CreateMul(idxB, selfLen, "rep.off");
        llvm::Value* dst = builder_.CreateGEP(i8Ty, newData, off, "rep.dst");
        builder_.CreateCall(memcpyFn, {dst, selfData, selfLen});
        builder_.CreateStore(builder_.CreateAdd(idxB, builder_.getInt64(1)), idxSlot);
        builder_.CreateBr(condBB);
        builder_.SetInsertPoint(endBB);
        llvm::Value* nulPtr = builder_.CreateGEP(i8Ty, newData, totalLen);
        builder_.CreateStore(llvm::ConstantInt::get(i8Ty, 0), nulPtr);
        uint64_t strBytes = module_->getDataLayout().getTypeAllocSize(stringType());
        llvm::Value* strObj = builder_.CreateCall(mallocFn, {builder_.getInt64(strBytes)}, "rep.obj");
        builder_.CreateStore(newData, builder_.CreateStructGEP(stringType(), strObj, 0));
        builder_.CreateStore(totalLen, builder_.CreateStructGEP(stringType(), strObj, 1));
        builder_.CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(llvm_), 1),
            builder_.CreateStructGEP(stringType(), strObj, 2));
            initStringRc(strObj);  // #422: producer's reference, rc := 1
        return strObj;
    }

    // §9.6-ext: GetField(delim: char, n: int): string
    // Returns the n-th 0-indexed field when splitting self by delim.
    // Returns empty string if n is out of range.
    if (method == "GetField" && call.args.size() == 2 &&
        call.args[0].value && call.args[1].value) {
        llvm::Value* delimArg = lowerExpr(*call.args[0].value);
        llvm::Value* nArg = lowerExpr(*call.args[1].value);
        if (!delimArg || !nArg) return nullptr;
        auto* i8Ty = llvm::Type::getInt8Ty(llvm_);
        llvm::Value* selfLen = loadStringLen(self);
        llvm::Value* selfData = loadStringData(self);
        // Convert delim (char = i32) to i8.
        llvm::Value* delimByte = builder_.CreateTrunc(delimArg, i8Ty, "gf.delim");
        // Walk the string counting fields.
        auto* idxSlot = entryAlloca(i64());  // current scan position
        auto* fldSlot = entryAlloca(i32());  // current field count
        auto* staSlot = entryAlloca(i64());  // start of current field
        builder_.CreateStore(builder_.getInt64(0), idxSlot);
        builder_.CreateStore(builder_.getInt32(0), fldSlot);
        builder_.CreateStore(builder_.getInt64(0), staSlot);
        // Loop: find the n-th field.
        auto* condBB = llvm::BasicBlock::Create(llvm_, "gf.cond", currentFn_);
        auto* bodyBB = llvm::BasicBlock::Create(llvm_, "gf.body", currentFn_);
        auto* endBB  = llvm::BasicBlock::Create(llvm_, "gf.end",  currentFn_);
        builder_.CreateBr(condBB);
        builder_.SetInsertPoint(condBB);
        llvm::Value* idx = builder_.CreateLoad(i64(), idxSlot);
        llvm::Value* fld = builder_.CreateLoad(i32(), fldSlot);
        llvm::Value* inRange = builder_.CreateICmpSLE(idx, selfLen);
        llvm::Value* notDone = builder_.CreateICmpSLT(fld, nArg);
        builder_.CreateCondBr(builder_.CreateAnd(inRange, notDone), bodyBB, endBB);
        builder_.SetInsertPoint(bodyBB);
        llvm::Value* idxB = builder_.CreateLoad(i64(), idxSlot);
        // Check if we've reached the end OR found the delimiter.
        llvm::Value* atEnd = builder_.CreateICmpSGE(idxB, selfLen);
        auto* delimBB  = llvm::BasicBlock::Create(llvm_, "gf.delim", currentFn_);
        auto* scanBB   = llvm::BasicBlock::Create(llvm_, "gf.scan",  currentFn_);
        builder_.CreateCondBr(atEnd, delimBB, scanBB);
        builder_.SetInsertPoint(scanBB);
        llvm::Value* bp  = builder_.CreateGEP(i8Ty, selfData, idxB);
        llvm::Value* by  = builder_.CreateLoad(i8Ty, bp);
        llvm::Value* isDlm = builder_.CreateICmpEQ(by, delimByte);
        builder_.CreateCondBr(isDlm, delimBB, delimBB);  // always → delimBB
        // Merged: either we're at end or found delimiter (or just scanning).
        // Actually, let's restructure: scan forward to next delimiter.
        // Simpler: use a while-char-loop, exit when delimiter or end.
        builder_.SetInsertPoint(delimBB);
        // If at delimiter: advance field count, update start, advance pos.
        llvm::Value* idxC = builder_.CreateLoad(i64(), idxSlot);
        llvm::Value* byC = builder_.CreateSelect(
            builder_.CreateICmpSGE(idxC, selfLen),
            llvm::ConstantInt::get(i8Ty, 0),
            builder_.CreateLoad(i8Ty, builder_.CreateGEP(i8Ty, selfData, idxC)));
        llvm::Value* atDlm = builder_.CreateICmpEQ(byC, delimByte);
        llvm::Value* atEnd2 = builder_.CreateICmpSGE(idxC, selfLen);
        llvm::Value* shouldNext = builder_.CreateOr(atDlm, atEnd2);
        auto* nextBB = llvm::BasicBlock::Create(llvm_, "gf.next", currentFn_);
        auto* stepBB = llvm::BasicBlock::Create(llvm_, "gf.step", currentFn_);
        builder_.CreateCondBr(shouldNext, nextBB, stepBB);
        builder_.SetInsertPoint(stepBB);
        // Just advance position.
        builder_.CreateStore(builder_.CreateAdd(idxC, builder_.getInt64(1)), idxSlot);
        builder_.CreateBr(condBB);
        builder_.SetInsertPoint(nextBB);
        // Advance to next field.
        llvm::Value* fldC = builder_.CreateLoad(i32(), fldSlot);
        builder_.CreateStore(builder_.CreateAdd(fldC, builder_.getInt32(1)), fldSlot);
        // New start = pos+1 (after delimiter). If at end, start stays.
        llvm::Value* newStart = builder_.CreateSelect(
            atDlm, builder_.CreateAdd(idxC, builder_.getInt64(1)), idxC);
        builder_.CreateStore(newStart, staSlot);
        builder_.CreateStore(newStart, idxSlot);
        builder_.CreateBr(condBB);
        // After loop: extract from staSlot to either next delimiter or end.
        builder_.SetInsertPoint(endBB);
        llvm::Value* sta = builder_.CreateLoad(i64(), staSlot);
        // Find end of this field: scan from sta forward to delimiter or end.
        auto* es = entryAlloca(i64());
        builder_.CreateStore(sta, es);
        auto* ecBB = llvm::BasicBlock::Create(llvm_, "gf.ec", currentFn_);
        auto* ebBB = llvm::BasicBlock::Create(llvm_, "gf.eb", currentFn_);
        auto* eBB  = llvm::BasicBlock::Create(llvm_, "gf.e",  currentFn_);
        builder_.CreateBr(ecBB);
        builder_.SetInsertPoint(ecBB);
        llvm::Value* ei = builder_.CreateLoad(i64(), es);
        llvm::Value* eiLt = builder_.CreateICmpSLT(ei, selfLen);
        builder_.CreateCondBr(eiLt, ebBB, eBB);
        builder_.SetInsertPoint(ebBB);
        llvm::Value* eip = builder_.CreateGEP(i8Ty, selfData, ei);
        llvm::Value* eib = builder_.CreateLoad(i8Ty, eip);
        llvm::Value* eiDlm = builder_.CreateICmpEQ(eib, delimByte);
        auto* eincBB = llvm::BasicBlock::Create(llvm_, "gf.einc", currentFn_);
        builder_.CreateCondBr(eiDlm, eBB, eincBB);
        builder_.SetInsertPoint(eincBB);
        llvm::Value* eiL = builder_.CreateLoad(i64(), es);
        builder_.CreateStore(builder_.CreateAdd(eiL, builder_.getInt64(1)), es);
        builder_.CreateBr(ecBB);
        builder_.SetInsertPoint(eBB);
        llvm::Value* fieldEnd = builder_.CreateLoad(i64(), es);
        llvm::Value* fieldLen = builder_.CreateSub(fieldEnd, sta);
        // Create string for the field.
        auto* mallocFn = declareC("malloc", ptrTy(), {i64()});
        auto* memcpyFn = declareC("memcpy", ptrTy(), {ptrTy(), ptrTy(), i64()});
        llvm::Value* dataSz = builder_.CreateAdd(fieldLen, builder_.getInt64(1));
        llvm::Value* fData = builder_.CreateCall(mallocFn, {dataSz}, "gf.data");
        llvm::Value* srcPtr = builder_.CreateGEP(i8Ty, selfData, sta, "gf.src");
        builder_.CreateCall(memcpyFn, {fData, srcPtr, fieldLen});
        llvm::Value* nul = builder_.CreateGEP(i8Ty, fData, fieldLen);
        builder_.CreateStore(llvm::ConstantInt::get(i8Ty, 0), nul);
        uint64_t strBytes = module_->getDataLayout().getTypeAllocSize(stringType());
        llvm::Value* strObj = builder_.CreateCall(mallocFn, {builder_.getInt64(strBytes)}, "gf.obj");
        builder_.CreateStore(fData, builder_.CreateStructGEP(stringType(), strObj, 0));
        builder_.CreateStore(fieldLen, builder_.CreateStructGEP(stringType(), strObj, 1));
        builder_.CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(llvm_), 1),
            builder_.CreateStructGEP(stringType(), strObj, 2));
            initStringRc(strObj);  // #422: producer's reference, rc := 1
        return strObj;
    }

    // §9.6-ext: CountOf(sub: string): int — count non-overlapping occurrences.
    if (method == "CountOf" && call.args.size() == 1 && call.args[0].value) {
        llvm::Value* sub = getStrArg(0);
        if (!sub) return nullptr;
        auto* i8Ty = llvm::Type::getInt8Ty(llvm_);
        llvm::Value* selfLen = loadStringLen(self);
        llvm::Value* selfData = loadStringData(self);
        llvm::Value* subLen = loadStringLen(sub);
        llvm::Value* subData = loadStringData(sub);
        auto* memmemFn = module_->getFunction("memmem");
        if (!memmemFn) {
            auto* ft = llvm::FunctionType::get(ptrTy(), {ptrTy(), i64(), ptrTy(), i64()}, false);
            memmemFn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "memmem", module_.get());
        }
        auto* cntSlot = entryAlloca(i32());
        auto* posSlot = entryAlloca(ptrTy());
        builder_.CreateStore(builder_.getInt32(0), cntSlot);
        builder_.CreateStore(selfData, posSlot);
        llvm::Value* endPtr = builder_.CreateGEP(i8Ty, selfData, selfLen);
        auto* condBB = llvm::BasicBlock::Create(llvm_, "cnt.cond", currentFn_);
        auto* bodyBB = llvm::BasicBlock::Create(llvm_, "cnt.body", currentFn_);
        auto* endBB  = llvm::BasicBlock::Create(llvm_, "cnt.end",  currentFn_);
        builder_.CreateBr(condBB);
        builder_.SetInsertPoint(condBB);
        llvm::Value* pos = builder_.CreateLoad(ptrTy(), posSlot);
        llvm::Value* rem = builder_.CreatePtrDiff(i8Ty, endPtr, pos);
        llvm::Value* canSearch = builder_.CreateICmpSGE(rem, subLen);
        builder_.CreateCondBr(canSearch, bodyBB, endBB);
        builder_.SetInsertPoint(bodyBB);
        llvm::Value* posB = builder_.CreateLoad(ptrTy(), posSlot);
        llvm::Value* remB = builder_.CreatePtrDiff(i8Ty, endPtr, posB);
        llvm::Value* found = builder_.CreateCall(memmemFn, {posB, remB, subData, subLen});
        auto* nullPtr = llvm::ConstantPointerNull::get(ptrTy());
        llvm::Value* notFound = builder_.CreateICmpEQ(found, nullPtr);
        auto* incBB = llvm::BasicBlock::Create(llvm_, "cnt.inc", currentFn_);
        builder_.CreateCondBr(notFound, endBB, incBB);
        builder_.SetInsertPoint(incBB);
        // Increment count, advance past found occurrence.
        llvm::Value* cnt = builder_.CreateLoad(i32(), cntSlot);
        builder_.CreateStore(builder_.CreateAdd(cnt, builder_.getInt32(1)), cntSlot);
        llvm::Value* afterFound = builder_.CreateGEP(i8Ty, found, subLen);
        builder_.CreateStore(afterFound, posSlot);
        builder_.CreateBr(condBB);
        builder_.SetInsertPoint(endBB);
        return builder_.CreateLoad(i32(), cntSlot, "countof.result");
    }

    // §9.6: Insert(index, value): string — insert value at code-point position index.
    if (method == "Insert" && call.args.size() == 2 &&
        call.args[0].value && call.args[1].value) {
        llvm::Value* idxV = getIntArg(0);
        llvm::Value* ins  = getStrArg(1);
        if (!idxV || !ins) return nullptr;
        auto* i8Ty = llvm::Type::getInt8Ty(llvm_);
        llvm::Value* selfLen = loadStringLen(self);
        llvm::Value* insLen  = loadStringLen(ins);
        llvm::Value* idx64 = builder_.CreateSExt(idxV, i64(), "ins.idx");
        // Clamp index to [0, selfLen]
        llvm::Value* tooHigh = builder_.CreateICmpSGT(idx64, selfLen);
        idx64 = builder_.CreateSelect(tooHigh, selfLen, idx64);
        llvm::Value* tooLow = builder_.CreateICmpSLT(idx64, builder_.getInt64(0));
        idx64 = builder_.CreateSelect(tooLow, builder_.getInt64(0), idx64);
        llvm::Value* resultLen = builder_.CreateAdd(selfLen, insLen, "ins.reslen");
        auto* mallocFn = declareC("malloc", ptrTy(), {i64()});
        auto* memcpyFn = declareC("memcpy", ptrTy(), {ptrTy(), ptrTy(), i64()});
        // Width handling: use self's width (assume same width for ins for simplicity).
        llvm::Value* wid = builder_.CreateZExt(loadStringWidth(self), i64());
        llvm::Value* dataSz = builder_.CreateAdd(
            builder_.CreateMul(resultLen, wid), builder_.getInt64(1));
        llvm::Value* newData = builder_.CreateCall(mallocFn, {dataSz}, "ins.data");
        llvm::Value* selfData = loadStringData(self);
        llvm::Value* insData  = loadStringData(ins);
        // Copy prefix: self[0..idx)
        llvm::Value* prefBytes = builder_.CreateMul(idx64, wid);
        builder_.CreateCall(memcpyFn, {newData, selfData, prefBytes});
        // Copy insertion: ins[0..insLen)
        llvm::Value* insBytes  = builder_.CreateMul(insLen, wid);
        llvm::Value* midDst = builder_.CreateGEP(i8Ty, newData, prefBytes);
        builder_.CreateCall(memcpyFn, {midDst, insData, insBytes});
        // Copy suffix: self[idx..selfLen)
        llvm::Value* suffOff = builder_.CreateMul(idx64, wid);
        llvm::Value* suffSrc = builder_.CreateGEP(i8Ty, selfData, suffOff);
        llvm::Value* suffDst = builder_.CreateGEP(i8Ty, newData,
            builder_.CreateAdd(prefBytes, insBytes));
        llvm::Value* suffBytes = builder_.CreateMul(
            builder_.CreateSub(selfLen, idx64), wid);
        builder_.CreateCall(memcpyFn, {suffDst, suffSrc, suffBytes});
        // Null-terminate.
        llvm::Value* resultBytes = builder_.CreateMul(resultLen, wid);
        llvm::Value* nulPtr = builder_.CreateGEP(i8Ty, newData, resultBytes);
        builder_.CreateStore(llvm::ConstantInt::get(i8Ty, 0), nulPtr);
        llvm::Value* maxW8 = builder_.CreateTrunc(loadStringWidth(self), i8Ty);
        uint64_t strBytes = module_->getDataLayout().getTypeAllocSize(stringType());
        llvm::Value* newStr = builder_.CreateCall(mallocFn, {builder_.getInt64(strBytes)}, "ins.obj");
        builder_.CreateStore(newData, builder_.CreateStructGEP(stringType(), newStr, 0));
        builder_.CreateStore(resultLen, builder_.CreateStructGEP(stringType(), newStr, 1));
        builder_.CreateStore(maxW8, builder_.CreateStructGEP(stringType(), newStr, 2));
        initStringRc(newStr);  // #422: producer's reference, rc := 1
        return newStr;
    }

    // §9.6: Remove(start): string — remove from start to end.
    // §9.6: Remove(start, count): string — remove count code points.
    if (method == "Remove" && !call.args.empty() && call.args[0].value) {
        llvm::Value* startV = getIntArg(0);
        if (!startV) return nullptr;
        auto* i8Ty = llvm::Type::getInt8Ty(llvm_);
        llvm::Value* selfLen  = loadStringLen(self);
        llvm::Value* selfData = loadStringData(self);
        llvm::Value* start64 = builder_.CreateSExt(startV, i64(), "rm.start");
        // end = start + count (if given), else = selfLen
        llvm::Value* end64;
        if (call.args.size() >= 2 && call.args[1].value) {
            llvm::Value* cntV = lowerExpr(*call.args[1].value);
            if (!cntV) return nullptr;
            llvm::Value* cnt64 = builder_.CreateSExt(cntV, i64(), "rm.cnt");
            end64 = builder_.CreateAdd(start64, cnt64, "rm.end");
            // Clamp to selfLen
            llvm::Value* pastEnd = builder_.CreateICmpSGT(end64, selfLen);
            end64 = builder_.CreateSelect(pastEnd, selfLen, end64);
        } else {
            end64 = selfLen;  // remove from start to end of string
        }
        // Result length = start + (selfLen - end)
        llvm::Value* tailLen = builder_.CreateSub(selfLen, end64, "rm.tail");
        llvm::Value* resultLen = builder_.CreateAdd(start64, tailLen, "rm.reslen");
        auto* mallocFn = declareC("malloc", ptrTy(), {i64()});
        auto* memcpyFn = declareC("memcpy", ptrTy(), {ptrTy(), ptrTy(), i64()});
        // Byte sizes (width=1 for now — simple byte operations).
        llvm::Value* dataSz = builder_.CreateAdd(resultLen, builder_.getInt64(1));
        llvm::Value* newData = builder_.CreateCall(mallocFn, {dataSz}, "rm.data");
        // Copy prefix (0..start).
        llvm::Value* wid = builder_.CreateZExt(loadStringWidth(self), i64());
        llvm::Value* prefBytes = builder_.CreateMul(start64, wid);
        builder_.CreateCall(memcpyFn, {newData, selfData, prefBytes});
        // Copy suffix (end..selfLen).
        llvm::Value* tailSrcOff = builder_.CreateMul(end64, wid);
        llvm::Value* tailSrc = builder_.CreateGEP(i8Ty, selfData, tailSrcOff);
        llvm::Value* tailDst = builder_.CreateGEP(i8Ty, newData, prefBytes);
        llvm::Value* tailBytes = builder_.CreateMul(tailLen, wid);
        builder_.CreateCall(memcpyFn, {tailDst, tailSrc, tailBytes});
        // Null-terminate.
        llvm::Value* resultBytes = builder_.CreateMul(resultLen, wid);
        llvm::Value* nulPtr = builder_.CreateGEP(i8Ty, newData, resultBytes);
        llvm::Value* maxW8 = builder_.CreateTrunc(loadStringWidth(self), llvm::Type::getInt8Ty(llvm_));
        builder_.CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(llvm_), 0), nulPtr);
        uint64_t strBytes = module_->getDataLayout().getTypeAllocSize(stringType());
        llvm::Value* newStr = builder_.CreateCall(mallocFn, {builder_.getInt64(strBytes)}, "rm.obj");
        builder_.CreateStore(newData, builder_.CreateStructGEP(stringType(), newStr, 0));
        builder_.CreateStore(resultLen, builder_.CreateStructGEP(stringType(), newStr, 1));
        builder_.CreateStore(maxW8, builder_.CreateStructGEP(stringType(), newStr, 2));
        initStringRc(newStr);  // #422: producer's reference, rc := 1
        return newStr;
    }

    // §9.6 / #535: Split — two overloads lowered here, both now width-aware.
    //   Split(separator: char)   — code-point comparison via bz_str_split_cp
    //   Split(separator: string) — code-point comparison via bz_str_split_str
    // The Split(separators: Array<char>) overload is NOT yet lowered (same
    // reason as before: Array<char> element storage interacts with pre-existing
    // Array<char> read-path issues — deferred rather than risk a miscompile).
    if (method == "Split" && call.args.size() == 1 && call.args[0].value) {
        auto* argTy = ctx_.exprType(*call.args[0].value);

        // Split(separator: char) — pass self + the separator code point.
        if (argTy && argTy->kind == sema::TypeRef::Kind::Primitive &&
            argTy->primitive == sema::PrimitiveType::Char) {
            llvm::Value* sepCh = lowerExpr(*call.args[0].value);
            if (!sepCh) return nullptr;
            if (!sepCh->getType()->isIntegerTy(32))
                sepCh = builder_.CreateSExt(sepCh, i32());
            auto* fn = declareC("bz_str_split_cp", ptrTy(), {ptrTy(), i32()});
            return builder_.CreateCall(fn, {self, sepCh}, "split.char.result");
        }

        // stdlib §9.6 (#741): Split(separators: Array<char>) — split on ANY of
        // the separator chars. The arg is a char collection (lowers to a
        // bz_array_t*). No case existed → the call emitted nothing and the
        // uninitialized result array crashed on Count/dispose.
        if (argTy && argTy->kind == sema::TypeRef::Kind::Named) {
            llvm::Value* seps = lowerExpr(*call.args[0].value);
            if (!seps) return nullptr;
            auto* fn = declareC("bz_str_split_chars", ptrTy(), {ptrTy(), ptrTy()});
            return builder_.CreateCall(fn, {self, seps}, "split.chars.result");
        }

        // Split(separator: string) — pass self + the delimiter bz_string*.
        // Guard type to avoid mis-GEP on a non-string argument.
        if (!argTy || argTy->kind != sema::TypeRef::Kind::Primitive ||
            argTy->primitive != sema::PrimitiveType::String)
            return nullptr;
        llvm::Value* delim = lowerExpr(*call.args[0].value);
        if (!delim) return nullptr;
        auto* fn = declareC("bz_str_split_str", ptrTy(), {ptrTy(), ptrTy()});
        return builder_.CreateCall(fn, {self, delim}, "split.result");
    }

    // §9.6 / issue #654: ToCharArray(): owned Array<char>.
    // Returns a fresh bz_array_t* (elem_size=4; each slot is an i32 code point).
    // The result is owned by the caller and must be disposed via bz_array_dispose;
    // callers should bind it with `using var` so the automatic disposal path fires.
    if (method == "ToCharArray" && call.args.empty()) {
        auto* fn = declareC("bz_str_to_char_array", ptrTy(), {ptrTy()});
        return builder_.CreateCall(fn, {self}, "tochararray.result");
    }

    return nullptr;  // method not recognised
}


llvm::Value* Emitter::emitCheckedArith(llvm::Intrinsic::ID id, llvm::Value* l,
                              llvm::Value* r) {
    auto* fn = llvm::Intrinsic::getOrInsertDeclaration(
        module_.get(), id, {l->getType()});
    llvm::Value* pair = builder_.CreateCall(fn, {l, r});
    llvm::Value* res = builder_.CreateExtractValue(pair, 0);
    llvm::Value* ovf = builder_.CreateExtractValue(pair, 1);
    emitGuardThrow(ovf, "OverflowException");
    return res;
}


// ---------------------------------------------------------------------------
// §8 / §9 / §10: compiler-synthesized value-semantics members
// (ToString, GetHashCode) for records and enums.
// ---------------------------------------------------------------------------

const sema::TypeRef* Emitter::recordFieldType(const sema::FieldSymbol* fs) {
    // §9.1: a positional field carries its type on the originating Param;
    // a body field on its FieldDecl.
    if (fs->posParam && fs->posParam->type)
        return ctx_.resolvedType(*fs->posParam->type);
    if (auto* fd = dynamic_cast<const ast::FieldDecl*>(fs->decl))
        if (fd->type)
            return ctx_.resolvedType(*fd->type);
    return nullptr;
}

const sema::TypeRef* Emitter::recordMemberType(const sema::Symbol* m) {
    // §9.1: FieldSymbol storage (positional Param or body FieldDecl).
    if (auto* fs = dynamic_cast<const sema::FieldSymbol*>(m))
        return recordFieldType(fs);
    // §9.2: body auto-property storage — type on the PropertyDecl.
    if (auto* pd = dynamic_cast<const ast::PropertyDecl*>(m->decl))
        if (pd->type)
            return ctx_.resolvedType(*pd->type);
    return nullptr;
}

// Format a single field/element value of static type `fieldTy` as a string,
// recursing for nested records (§9.1) and using each type's own ToString.
llvm::Value* Emitter::emitValueToString(llvm::Value* val,
                                        const sema::TypeRef* fieldTy) {
    using K = sema::TypeRef::Kind;
    // per §8.3/§9.1: a NULLABLE REFERENCE field (string?, Object?, nullable
    // class/interface) is a plain `ptr` with a real null. A null field renders
    // as the literal "null" — the same rendering null interpolation uses
    // (LlvmBackend_Exprs.cpp) — instead of dereferencing it (bug A). A non-null
    // field formats by its normal rule below.
    if (isNullableRefTy(fieldTy) && val->getType()->isPointerTy()) {
        llvm::Value* isNull = builder_.CreateICmpEQ(
            val, llvm::ConstantPointerNull::get(ptrTy()), "ts.null");
        auto* liveBB = llvm::BasicBlock::Create(llvm_, "ts.live", currentFn_);
        auto* nullBB = llvm::BasicBlock::Create(llvm_, "ts.nullstr", currentFn_);
        auto* endBB  = llvm::BasicBlock::Create(llvm_, "ts.end", currentFn_);
        builder_.CreateCondBr(isNull, nullBB, liveBB);
        // Non-null: format by the field's non-nullable form.
        builder_.SetInsertPoint(liveBB);
        sema::TypeRef liveTy = *fieldTy;
        liveTy.nullable = false;
        llvm::Value* liveStr = emitValueToString(val, &liveTy);
        auto* liveEnd = builder_.GetInsertBlock();
        builder_.CreateBr(endBB);
        // Null: the literal "null".
        builder_.SetInsertPoint(nullBB);
        llvm::Value* nullStr = lowerStringLiteral("\"null\"", false);
        auto* nullEnd = builder_.GetInsertBlock();
        builder_.CreateBr(endBB);
        builder_.SetInsertPoint(endBB);
        auto* phi = builder_.CreatePHI(ptrTy(), 2, "ts.nullable");
        phi->addIncoming(liveStr, liveEnd);
        phi->addIncoming(nullStr, nullEnd);
        return phi;
    }
    // per §8.5/§9.1 / issue #364: a NULLABLE VALUE field (int?, bool?, double?,
    // … the value primitives isNullablePrimTy recognizes, AND a nullable enum —
    // isNullableValueTy) is the tagged struct {i8 hasValue; T value}. hasValue →
    // format the inner T by its normal ToString (recursing with the
    // non-nullable inner type: a primitive uses emitPrimitiveToString, an enum
    // its type name per §8.5); null → the literal "null" (matching null
    // interpolation and the nullable-ref rendering above). Selecting off the tag
    // avoids rendering the slot of an absent value (a zero fill).
    if (isNullableValueTy(fieldTy) && val->getType()->isStructTy()) {
        llvm::Value* has = nullablePrimHasValue(val);          // i1
        auto* liveBB = llvm::BasicBlock::Create(llvm_, "ts.nvlive", currentFn_);
        auto* nullBB = llvm::BasicBlock::Create(llvm_, "ts.nvnull", currentFn_);
        auto* endBB  = llvm::BasicBlock::Create(llvm_, "ts.nvend", currentFn_);
        builder_.CreateCondBr(has, liveBB, nullBB);
        // hasValue: format the inner value by its non-nullable rule.
        builder_.SetInsertPoint(liveBB);
        sema::TypeRef innerTy = *fieldTy;
        innerTy.nullable = false;
        llvm::Value* liveStr =
            emitValueToString(nullablePrimValue(val), &innerTy);
        auto* liveEnd = builder_.GetInsertBlock();
        builder_.CreateBr(endBB);
        // null: the literal "null".
        builder_.SetInsertPoint(nullBB);
        llvm::Value* nullStr = lowerStringLiteral("\"null\"", false);
        auto* nullEnd = builder_.GetInsertBlock();
        builder_.CreateBr(endBB);
        builder_.SetInsertPoint(endBB);
        auto* phi = builder_.CreatePHI(ptrTy(), 2, "ts.nullableval");
        phi->addIncoming(liveStr, liveEnd);
        phi->addIncoming(nullStr, nullEnd);
        return phi;
    }
    if (fieldTy && fieldTy->kind == K::Primitive) {
        // §9.1: strings are formatted by their own ToString — which for
        // string is identity (§9.6); the value is the bz_string itself.
        return emitPrimitiveToString(val, fieldTy->primitive);
    }
    if (fieldTy && fieldTy->kind == K::Named && fieldTy->declaration) {
        const sema::TypeSymbol* ts = fieldTy->declaration;
        if (ts->kind == ast::TypeDeclKind::Record && val->getType()->isStructTy())
            return emitRecordToString(val, ts);          // §9.1 recurse
        if (ts->kind == ast::TypeDeclKind::Enum)
            // §8.5: enum default ToString is the type's qualified name.
            return lowerStringLiteral(
                std::string("\"") + std::string(ts->name) + "\"", false);
        if (ts->kind == ast::TypeDeclKind::Class && val->getType()->isPointerTy()) {
            // §8.5: a class field formats via its ToString override if any,
            // else the type name. Walk the class chain for a ToString method.
            sema::MethodSymbol* toStr = nullptr;
            for (auto* cur = ts; cur && !toStr;) {
                for (auto* sym : cur->members) {
                    auto* ms = dynamic_cast<sema::MethodSymbol*>(sym);
                    if (ms && ms->name == "ToString" && ms->params.empty()) {
                        toStr = ms; break;
                    }
                }
                sema::TypeSymbol* nxt = nullptr;
                for (auto* b : cur->baseTypes)
                    if (b && b->kind == ast::TypeDeclKind::Class) { nxt = b; break; }
                cur = nxt;
            }
            if (toStr) {
                std::vector<llvm::Value*> a{val};
                if (llvm::Value* r = emitMethodCall(toStr, a, false)) return r;
            }
            return lowerStringLiteral(
                std::string("\"") + std::string(ts->name) + "\"", false);
        }
        if (ts->kind == ast::TypeDeclKind::Interface &&
            val->getType()->isPointerTy()) {
            // §8.5 (#1077): an interface-typed value is a vtable-bearing object;
            // format by the DYNAMIC ToString (the runtime override if any, else
            // the runtime type name) via the universal object dispatch — the
            // same path an Object-typed value uses. Without this branch an
            // interface value fell to the fallback below and was mis-rendered as
            // a raw bz_string (`$"{ishape}"` → "").
            if (llvm::Value* r = lowerObjectToString(val)) return r;
            return lowerStringLiteral("\"\"", false);
        }
    }
    // Fallback: format by the LLVM shape (string ptr → identity; integer → int).
    if (val->getType()->isPointerTy()) return val;            // assume bz_string
    if (val->getType()->isIntegerTy(64))
        return emitPrimitiveToString(val, sema::PrimitiveType::Long);
    if (val->getType()->isIntegerTy(1))
        return emitPrimitiveToString(val, sema::PrimitiveType::Bool);
    if (val->getType()->isIntegerTy())
        return emitPrimitiveToString(val, sema::PrimitiveType::Int);
    if (val->getType()->isDoubleTy())
        return emitPrimitiveToString(val, sema::PrimitiveType::Double);
    if (val->getType()->isFloatTy())
        return emitPrimitiveToString(val, sema::PrimitiveType::Float);
    return lowerStringLiteral("\"\"", false);
}

// §8.5 / §8.6 (#417): lower `o.ToString()` where o: Object / interface (no
// resolved MethodSymbol — the receiver's static type has no `declaration`). The
// dynamic value is either a string (a bare bz_string*, NO vtable) or a
// class/box/exception (vtable at word 0, ToString at the fixed slot 3). Branch
// on bz_obj_is_string: a string returns itself (§9.6 string.ToString = this); a
// class/box dispatches through vtable[kToStringSlot]. A null receiver renders as
// the literal "null" (the same rendering null interpolation uses).
llvm::Value* Emitter::lowerObjectToString(llvm::Value* recv) {
    if (!recv || !recv->getType()->isPointerTy()) return nullptr;

    auto* nullBB   = llvm::BasicBlock::Create(llvm_, "ts.obj.null", currentFn_);
    auto* liveBB   = llvm::BasicBlock::Create(llvm_, "ts.obj.live", currentFn_);
    auto* strBB    = llvm::BasicBlock::Create(llvm_, "ts.obj.str", currentFn_);
    auto* vtBB     = llvm::BasicBlock::Create(llvm_, "ts.obj.vt", currentFn_);
    auto* endBB    = llvm::BasicBlock::Create(llvm_, "ts.obj.end", currentFn_);

    llvm::Value* isNull = builder_.CreateICmpEQ(
        recv, llvm::ConstantPointerNull::get(ptrTy()), "ts.obj.isnull");
    builder_.CreateCondBr(isNull, nullBB, liveBB);

    // null → "null".
    builder_.SetInsertPoint(nullBB);
    llvm::Value* nullStr = lowerStringLiteral("\"null\"", false);
    builder_.CreateBr(endBB);

    // non-null: string or vtable-bearing?
    builder_.SetInsertPoint(liveBB);
    auto* isStrFn = declareC("bz_obj_is_string", i32(), {ptrTy(), ptrTy()});
    llvm::Value* isStr = builder_.CreateCall(
        isStrFn, {recv, vtableTablePtr()}, "ts.obj.isstr");
    llvm::Value* isStrBit = builder_.CreateICmpNE(isStr, builder_.getInt32(0));
    builder_.CreateCondBr(isStrBit, strBB, vtBB);

    // string: ToString is identity (§9.6).
    builder_.SetInsertPoint(strBB);
    llvm::Value* strResult = recv;
    builder_.CreateBr(endBB);

    // class/box: dispatch vtable[kToStringSlot](recv).
    builder_.SetInsertPoint(vtBB);
    llvm::Value* vptr = builder_.CreateLoad(ptrTy(), recv);
    llvm::Value* slotPtr = builder_.CreateGEP(
        ptrTy(), vptr, {builder_.getInt32(kToStringSlot)}, "ts.obj.slot");
    llvm::Value* fnPtr = builder_.CreateLoad(ptrTy(), slotPtr);
    auto* fnTy = llvm::FunctionType::get(ptrTy(), {ptrTy()}, /*isVarArg=*/false);
    llvm::Value* vtResult = builder_.CreateCall(fnTy, fnPtr, {recv}, "ts.obj.r");
    builder_.CreateBr(endBB);

    builder_.SetInsertPoint(endBB);
    auto* phi = builder_.CreatePHI(ptrTy(), 3, "ts.obj.res");
    phi->addIncoming(nullStr, nullBB);
    phi->addIncoming(strResult, strBB);
    phi->addIncoming(vtResult, vtBB);
    return phi;
}


// §9.6 (#946/#947): lower `o.Equals(x)` where o: Object / interface (no resolved
// MethodSymbol). Mirrors lowerObjectToString. The dynamic value is null, a bare
// bz_string* (no vtable), or a class/box (vtable at word 0). A null receiver uses
// reference identity (null == other, true iff other is also null); a string
// receiver uses content equality when `other` is also a string (else not equal);
// a class/box dispatches through vtable[kEqualsSlot] to the override (or §8.3
// reference-identity default). Returns i1 — so an inferred `var r := o.Equals(x)`
// is bool-typed and `not r` produces valid IR (#947).
llvm::Value* Emitter::lowerObjectEquals(llvm::Value* recv, llvm::Value* other) {
    if (!recv || !recv->getType()->isPointerTy() ||
        !other || !other->getType()->isPointerTy())
        return nullptr;

    auto* nullBB  = llvm::BasicBlock::Create(llvm_, "eq.obj.null", currentFn_);
    auto* liveBB  = llvm::BasicBlock::Create(llvm_, "eq.obj.live", currentFn_);
    auto* strBB   = llvm::BasicBlock::Create(llvm_, "eq.obj.str", currentFn_);
    auto* strEqBB = llvm::BasicBlock::Create(llvm_, "eq.obj.streq", currentFn_);
    auto* strNeBB = llvm::BasicBlock::Create(llvm_, "eq.obj.strne", currentFn_);
    auto* vtBB    = llvm::BasicBlock::Create(llvm_, "eq.obj.vt", currentFn_);
    auto* endBB   = llvm::BasicBlock::Create(llvm_, "eq.obj.end", currentFn_);

    llvm::Value* isNull = builder_.CreateICmpEQ(
        recv, llvm::ConstantPointerNull::get(ptrTy()), "eq.obj.isnull");
    builder_.CreateCondBr(isNull, nullBB, liveBB);

    // null receiver → reference identity (null == other).
    builder_.SetInsertPoint(nullBB);
    llvm::Value* nullEq = builder_.CreateICmpEQ(recv, other, "eq.obj.nulleq");
    builder_.CreateBr(endBB);

    // non-null: string or vtable-bearing?
    builder_.SetInsertPoint(liveBB);
    auto* isStrFn = declareC("bz_obj_is_string", i32(), {ptrTy(), ptrTy()});
    llvm::Value* isStr = builder_.CreateCall(
        isStrFn, {recv, vtableTablePtr()}, "eq.obj.isstr");
    builder_.CreateCondBr(
        builder_.CreateICmpNE(isStr, builder_.getInt32(0)), strBB, vtBB);

    // string receiver: content equality only when `other` is also a string
    // (§9.6 string.Equals(Object?) — a string equals another object iff that
    // object is a string of equal content). A null/non-string `other` → false.
    builder_.SetInsertPoint(strBB);
    llvm::Value* otherIsStr = builder_.CreateCall(
        isStrFn, {other, vtableTablePtr()}, "eq.obj.oisstr");
    builder_.CreateCondBr(
        builder_.CreateICmpNE(otherIsStr, builder_.getInt32(0)),
        strEqBB, strNeBB);
    builder_.SetInsertPoint(strEqBB);
    llvm::Value* strEq = emitStringEq(recv, other);
    llvm::BasicBlock* strEqEnd = builder_.GetInsertBlock();
    builder_.CreateBr(endBB);
    builder_.SetInsertPoint(strNeBB);
    builder_.CreateBr(endBB);

    // class/box receiver: dispatch vtable[kEqualsSlot](recv, other) → i1.
    builder_.SetInsertPoint(vtBB);
    llvm::Value* vptr = builder_.CreateLoad(ptrTy(), recv);
    llvm::Value* slotPtr = builder_.CreateGEP(
        ptrTy(), vptr, {builder_.getInt32(kEqualsSlot)}, "eq.obj.slot");
    llvm::Value* fnPtr = builder_.CreateLoad(ptrTy(), slotPtr);
    auto* fnTy = llvm::FunctionType::get(i1(), {ptrTy(), ptrTy()},
                                         /*isVarArg=*/false);
    llvm::Value* vtEq = builder_.CreateCall(fnTy, fnPtr, {recv, other},
                                            "eq.obj.r");
    llvm::BasicBlock* vtEnd = builder_.GetInsertBlock();
    builder_.CreateBr(endBB);

    builder_.SetInsertPoint(endBB);
    auto* phi = builder_.CreatePHI(i1(), 4, "eq.obj.res");
    phi->addIncoming(nullEq, nullBB);
    phi->addIncoming(strEq, strEqEnd);
    phi->addIncoming(builder_.getFalse(), strNeBB);
    phi->addIncoming(vtEq, vtEnd);
    return phi;
}


// §9.6 (#948): lower `o.GetHashCode()` where o: Object / interface. Mirrors
// lowerObjectEquals: null → 0, a string → its content hash (consistent with
// string equality), a class/box → vtable[kGetHashCodeSlot]. Returns i32 (int).
llvm::Value* Emitter::lowerObjectGetHashCode(llvm::Value* recv) {
    if (!recv || !recv->getType()->isPointerTy()) return nullptr;

    auto* nullBB = llvm::BasicBlock::Create(llvm_, "h.obj.null", currentFn_);
    auto* liveBB = llvm::BasicBlock::Create(llvm_, "h.obj.live", currentFn_);
    auto* strBB  = llvm::BasicBlock::Create(llvm_, "h.obj.str", currentFn_);
    auto* vtBB   = llvm::BasicBlock::Create(llvm_, "h.obj.vt", currentFn_);
    auto* endBB  = llvm::BasicBlock::Create(llvm_, "h.obj.end", currentFn_);

    llvm::Value* isNull = builder_.CreateICmpEQ(
        recv, llvm::ConstantPointerNull::get(ptrTy()), "h.obj.isnull");
    builder_.CreateCondBr(isNull, nullBB, liveBB);

    // null → 0.
    builder_.SetInsertPoint(nullBB);
    builder_.CreateBr(endBB);

    builder_.SetInsertPoint(liveBB);
    auto* isStrFn = declareC("bz_obj_is_string", i32(), {ptrTy(), ptrTy()});
    llvm::Value* isStr = builder_.CreateCall(
        isStrFn, {recv, vtableTablePtr()}, "h.obj.isstr");
    builder_.CreateCondBr(
        builder_.CreateICmpNE(isStr, builder_.getInt32(0)), strBB, vtBB);

    // string: content hash (same as string.GetHashCode / a string field hash).
    builder_.SetInsertPoint(strBB);
    sema::TypeRef strTy;
    strTy.kind = sema::TypeRef::Kind::Primitive;
    strTy.primitive = sema::PrimitiveType::String;
    llvm::Value* strHash = emitValueHashCode(recv, &strTy);
    if (strHash->getType()->isIntegerTy() && !strHash->getType()->isIntegerTy(32))
        strHash = builder_.CreateZExtOrTrunc(strHash, i32());
    llvm::BasicBlock* strEnd = builder_.GetInsertBlock();
    builder_.CreateBr(endBB);

    // class/box: dispatch vtable[kGetHashCodeSlot](recv) → i32.
    builder_.SetInsertPoint(vtBB);
    llvm::Value* vptr = builder_.CreateLoad(ptrTy(), recv);
    llvm::Value* slotPtr = builder_.CreateGEP(
        ptrTy(), vptr, {builder_.getInt32(kGetHashCodeSlot)}, "h.obj.slot");
    llvm::Value* fnPtr = builder_.CreateLoad(ptrTy(), slotPtr);
    auto* fnTy = llvm::FunctionType::get(i32(), {ptrTy()}, /*isVarArg=*/false);
    llvm::Value* vtHash = builder_.CreateCall(fnTy, fnPtr, {recv}, "h.obj.r");
    llvm::BasicBlock* vtEnd = builder_.GetInsertBlock();
    builder_.CreateBr(endBB);

    builder_.SetInsertPoint(endBB);
    auto* phi = builder_.CreatePHI(i32(), 3, "h.obj.res");
    phi->addIncoming(builder_.getInt32(0), nullBB);
    phi->addIncoming(strHash, strEnd);
    phi->addIncoming(vtHash, vtEnd);
    return phi;
}


// §8.5 / §9.1 / §9.2: build `"TypeName(Prop1: value1, Prop2: value2)"` from the
// record's storage members in declaration order — positional/body fields for a
// positional record (§9.1), the auto-properties for an eligible body record
// (§9.2) — the same order emitStructEquality and recordStruct iterate. A
// zero-storage record yields `"TypeName()"` (§9.2 — the natural specialization
// of the property-listing format).
llvm::Value* Emitter::emitRecordToString(llvm::Value* recVal,
                                         const sema::TypeSymbol* rec) {
    llvm::Value* acc = lowerStringLiteral(
        std::string("\"") + std::string(rec->name) + "(\"", false);
    std::vector<const sema::Symbol*> storage;
    collectRecordStorage(rec, storage);
    unsigned idx = 0;       // struct element index
    bool first = true;
    for (const sema::Symbol* m : storage) {
        if (!first)
            acc = emitStringConcat(acc, lowerStringLiteral("\", \"", false));
        first = false;
        // `PropName: ` prefix — the member's declared name verbatim (§9.1:
        // "the compiler does not transform them").
        acc = emitStringConcat(acc, lowerStringLiteral(
            std::string("\"") + std::string(m->name) + ": \"", false));
        llvm::Value* fieldVal = builder_.CreateExtractValue(recVal, {idx});
        // issue #462 / §14.1: a generic record's `T`-typed field formats by its
        // CONCRETE type under the active subst (a string renders its value, a
        // record recurses) — not the type-erased shape fallback.
        acc = emitStringConcat(
            acc, emitValueToString(fieldVal, recordMemberTypeResolved(m)));
        ++idx;
    }
    acc = emitStringConcat(acc, lowerStringLiteral("\")\"", false));
    return acc;
}

// §8.3 / §9.1: hash a single field value of static type `fieldTy` → i32.
// Strings hash by content (consistent with content equality); nested records
// recurse; enums hash by their integer value; primitives by value.
llvm::Value* Emitter::emitValueHashCode(llvm::Value* val,
                                        const sema::TypeRef* fieldTy) {
    using K = sema::TypeRef::Kind;
    // Nested record (§9.1 transitivity): combine its own field hashes.
    if (fieldTy && fieldTy->kind == K::Named && fieldTy->declaration &&
        fieldTy->declaration->kind == ast::TypeDeclKind::Record &&
        val->getType()->isStructTy())
        return emitRecordHashCode(val, fieldTy->declaration);
    // per §8.3/§9.1: a NULLABLE REFERENCE field (string?, Object?, nullable
    // class/interface) is a plain `ptr` with a real null. A null field hashes
    // to a fixed sentinel (0) rather than dereferencing it (bug A); a non-null
    // field hashes by its normal rule below.
    if (isNullableRefTy(fieldTy) && val->getType()->isPointerTy()) {
        llvm::Value* isNull = builder_.CreateICmpEQ(
            val, llvm::ConstantPointerNull::get(ptrTy()), "h.null");
        auto* hashBB = llvm::BasicBlock::Create(llvm_, "h.live", currentFn_);
        auto* endBB  = llvm::BasicBlock::Create(llvm_, "h.end", currentFn_);
        auto* entryBB = builder_.GetInsertBlock();
        builder_.CreateCondBr(isNull, endBB, hashBB);
        builder_.SetInsertPoint(hashBB);
        // Recurse with the non-nullable form: a string hashes by content, a
        // reference by identity (its pointer bits truncated to i32).
        bool isStr = fieldTy->kind == K::Primitive &&
                     fieldTy->primitive == sema::PrimitiveType::String;
        llvm::Value* live;
        if (isStr) {
            sema::TypeRef strTy;
            strTy.kind = K::Primitive;
            strTy.primitive = sema::PrimitiveType::String;
            live = emitValueHashCode(val, &strTy);
        } else {
            live = builder_.CreatePtrToInt(val, i64(), "h.refbits");
            live = builder_.CreateTrunc(live, i32(), "h.ref32");
        }
        auto* hashEnd = builder_.GetInsertBlock();
        builder_.CreateBr(endBB);
        builder_.SetInsertPoint(endBB);
        auto* phi = builder_.CreatePHI(i32(), 2, "h.nullable");
        phi->addIncoming(builder_.getInt32(0), entryBB);   // null → sentinel 0
        phi->addIncoming(live, hashEnd);
        return phi;
    }
    // per §8.3/§9.1 / issue #364: a NULLABLE VALUE field (int?, bool?, double?,
    // … the value primitives isNullablePrimTy recognizes, AND a nullable enum —
    // isNullableValueTy) is the tagged struct {i8 hasValue; T value}. hasValue →
    // hash the inner value; null → the fixed sentinel 0 — the SAME sentinel the
    // null-equal pair collapses to, so the hash-equality invariant holds (two
    // both-null fields are equal and hash identically). Mirrors the nullable-ref
    // handling above.
    if (isNullableValueTy(fieldTy) && val->getType()->isStructTy()) {
        llvm::Value* has = nullablePrimHasValue(val);          // i1
        auto* hashBB = llvm::BasicBlock::Create(llvm_, "h.nvlive", currentFn_);
        auto* endBB  = llvm::BasicBlock::Create(llvm_, "h.nvend", currentFn_);
        auto* entryBB = builder_.GetInsertBlock();
        builder_.CreateCondBr(has, hashBB, endBB);
        builder_.SetInsertPoint(hashBB);
        // Hash the inner value by its non-nullable rule (the integer/float folds
        // below — an enum's i32 value hits the integer fold — via a
        // nullable-off recursion).
        sema::TypeRef innerTy = *fieldTy;
        innerTy.nullable = false;
        llvm::Value* live = emitValueHashCode(nullablePrimValue(val), &innerTy);
        auto* hashEnd = builder_.GetInsertBlock();
        builder_.CreateBr(endBB);
        builder_.SetInsertPoint(endBB);
        auto* phi = builder_.CreatePHI(i32(), 2, "h.nullableval");
        phi->addIncoming(builder_.getInt32(0), entryBB);   // null → sentinel 0
        phi->addIncoming(live, hashEnd);
        return phi;
    }
    bool isString = fieldTy && fieldTy->kind == K::Primitive &&
                    fieldTy->primitive == sema::PrimitiveType::String;
    if (isString || (!fieldTy && val->getType()->isPointerTy())) {
        // §4.6: hash a bz_string by folding its code points (FNV-style:
        // h = h*31 + cp). Equal content → equal hash, consistent with the
        // content-equality the language gives strings.
        auto* hashSlot = entryAlloca(i32());
        builder_.CreateStore(builder_.getInt32(0), hashSlot);
        auto* idxSlot = entryAlloca(i64());
        builder_.CreateStore(builder_.getInt64(0), idxSlot);
        llvm::Value* len = loadStringLen(val);
        auto* condBB = llvm::BasicBlock::Create(llvm_, "shash.c", currentFn_);
        auto* bodyBB = llvm::BasicBlock::Create(llvm_, "shash.b", currentFn_);
        auto* endBB  = llvm::BasicBlock::Create(llvm_, "shash.e", currentFn_);
        builder_.CreateBr(condBB);
        builder_.SetInsertPoint(condBB);
        llvm::Value* i = builder_.CreateLoad(i64(), idxSlot);
        builder_.CreateCondBr(builder_.CreateICmpSLT(i, len), bodyBB, endBB);
        builder_.SetInsertPoint(bodyBB);
        llvm::Value* i2 = builder_.CreateLoad(i64(), idxSlot);
        llvm::Value* cp = loadCodePoint(val, i2);            // i32 code point
        llvm::Value* h = builder_.CreateLoad(i32(), hashSlot);
        h = builder_.CreateAdd(builder_.CreateMul(h, builder_.getInt32(31)), cp);
        builder_.CreateStore(h, hashSlot);
        builder_.CreateStore(builder_.CreateAdd(i2, builder_.getInt64(1)), idxSlot);
        builder_.CreateBr(condBB);
        builder_.SetInsertPoint(endBB);
        return builder_.CreateLoad(i32(), hashSlot);
    }
    // fix #685 / §4.4 / §8.3: decimal value struct { i32×4 } — bz_decimal_hash
    // (normalized, consistent with bz_decimal_cmp so 1.0m == 1.00m → same hash).
    if (fieldTy && fieldTy->kind == sema::TypeRef::Kind::Primitive &&
        fieldTy->primitive == sema::PrimitiveType::Decimal &&
        !fieldTy->nullable) {
        llvm::AllocaInst* slot = entryAlloca(decimalStructType());
        builder_.CreateStore(val, slot);
        auto* hashFn = declareC("bz_decimal_hash", i32(), {ptrTy()});
        return builder_.CreateCall(hashFn, {slot}, "dec.hash");
    }
    // Numeric / bool / char / enum: derive an i32 from the bits.
    if (val->getType()->isIntegerTy()) {
        if (val->getType()->isIntegerTy(64)) {
            // long → fold high/low halves into i32 (§8.3 value-based hash).
            llvm::Value* lo = builder_.CreateTrunc(val, i32());
            llvm::Value* hi = builder_.CreateTrunc(
                builder_.CreateLShr(val, builder_.getInt64(32)), i32());
            return builder_.CreateXor(lo, hi);
        }
        return builder_.CreateZExtOrTrunc(val, i32());
    }
    if (val->getType()->isDoubleTy())
        return builder_.CreateTrunc(
            builder_.CreateBitCast(val, i64()), i32());
    if (val->getType()->isFloatTy())
        return builder_.CreateBitCast(val, i32());
    return builder_.getInt32(0);
}

// §8.3 / §9.1 / §9.2: combine per-storage-member hashes in declaration order
// with h = h*31 + memberHash, over the positional/body fields (§9.1) or the
// auto-properties of an eligible body record (§9.2). Order-dependent (so
// Pair(1,2) and Pair(2,1) differ) and consistent with the structural equality
// of emitStructEquality: equal records (same storage values) compute the same
// fold. A zero-storage record returns a fixed constant (§9.2).
llvm::Value* Emitter::emitRecordHashCode(llvm::Value* recVal,
                                         const sema::TypeSymbol* rec) {
    auto* hashSlot = entryAlloca(i32());
    builder_.CreateStore(builder_.getInt32(17), hashSlot);   // non-zero seed
    std::vector<const sema::Symbol*> storage;
    collectRecordStorage(rec, storage);
    unsigned idx = 0;
    for (const sema::Symbol* m : storage) {
        llvm::Value* fieldVal = builder_.CreateExtractValue(recVal, {idx});
        // issue #462 / §14.1: a generic record's `T`-typed field hashes by its
        // CONCRETE type under the active subst (a string hashes by CONTENT, so
        // the hash-equality invariant holds for content-equal values) — not by
        // pointer bits via the type-erased shape fallback.
        llvm::Value* fh = emitValueHashCode(fieldVal, recordMemberTypeResolved(m));
        llvm::Value* h = builder_.CreateLoad(i32(), hashSlot);
        h = builder_.CreateAdd(builder_.CreateMul(h, builder_.getInt32(31)), fh);
        builder_.CreateStore(h, hashSlot);
        ++idx;
    }
    // §9.2: zero-storage record keeps the fixed seed constant.
    return builder_.CreateLoad(i32(), hashSlot);
}


}  // namespace bezel::codegen
